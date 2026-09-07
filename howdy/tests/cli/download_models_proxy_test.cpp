#include "cli/download_models_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace howdy::test::download_models {

	using howdy::test::expect;
	using howdy::test::ScopedFd;

	namespace {

		constexpr int         kPollTimeoutMilliseconds     = 50;
		constexpr std::size_t kMaxProxyRequestBytes        = 8192;
		constexpr long        kConnectTimeoutMilliseconds  = 1000L;
		constexpr long        kTransferTimeoutMilliseconds = 2000L;

		enum class ProbeKind : std::uint8_t {
			kProxy,
			kOrigin,
		};

		class LoopbackProbe {
		public:
			explicit LoopbackProbe(const ProbeKind kind)
			    : kind_(kind) {
				Initialize();
			}

			LoopbackProbe(const LoopbackProbe &)                     = delete;
			auto operator=(const LoopbackProbe &) -> LoopbackProbe & = delete;

			~LoopbackProbe() {
				stop_requested_.store(true);
				if (worker_.joinable()) {
					worker_.join();
				}
			}

			[[nodiscard]] auto Ok() const -> bool {
				return listener_fd_.get() >= 0 && port_ != 0 && worker_.joinable();
			}

			[[nodiscard]] auto Port() const -> std::uint16_t {
				return port_;
			}

			auto WaitForConnection(const std::chrono::milliseconds timeout) const -> bool {
				std::unique_lock lock(state_mutex_);
				(void)state_changed_.wait_for(lock, timeout, [this] -> bool {
					return connection_observed_ || worker_finished_;
				});
				return connection_observed_;
			}

			auto WaitForRequest(const std::chrono::milliseconds timeout) const -> bool {
				std::unique_lock lock(state_mutex_);
				(void)state_changed_.wait_for(lock, timeout, [this] -> bool {
					return request_observed_ || worker_finished_;
				});
				return request_observed_;
			}

			[[nodiscard]] auto Request() const -> std::string {
				std::scoped_lock lock(state_mutex_);
				return request_;
			}

		private:
			void Initialize() {
				listener_fd_.reset(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
				if (listener_fd_.get() < 0) {
					return;
				}

				int reuse_address = 1;
				if (setsockopt(listener_fd_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse_address,
				               sizeof(reuse_address)) != 0) {
					listener_fd_.reset();
					return;
				}

				sockaddr_in address{};
				address.sin_family      = AF_INET;
				address.sin_port        = 0;
				address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				if (bind(listener_fd_.get(), reinterpret_cast<const sockaddr *>(&address),
				         sizeof(address)) != 0 ||
				    listen(listener_fd_.get(), 1) != 0) {
					listener_fd_.reset();
					return;
				}

				auto address_length = static_cast<socklen_t>(sizeof(address));
				if (getsockname(listener_fd_.get(), reinterpret_cast<sockaddr *>(&address),
				                &address_length) != 0) {
					listener_fd_.reset();
					return;
				}
				port_ = ntohs(address.sin_port);

				worker_ = std::thread([this] -> void {
					Run();
				});
			}

			void Run() noexcept {
				while (!stop_requested_.load()) {
					pollfd descriptor{
					    .fd      = listener_fd_.get(),
					    .events  = POLLIN,
					    .revents = 0,
					};
					const int poll_result = poll(&descriptor, 1, kPollTimeoutMilliseconds);
					if (stop_requested_.load()) {
						break;
					}
					if (poll_result < 0) {
						if (errno == EINTR) {
							continue;
						}
						break;
					}
					if (poll_result == 0) {
						continue;
					}
					if ((descriptor.revents & POLLIN) == 0) {
						break;
					}

					ScopedFd client_fd(accept(listener_fd_.get(), nullptr, nullptr));
					if (client_fd.get() < 0) {
						if (errno == EINTR) {
							continue;
						}
						break;
					}
					MarkConnectionObserved();
					if (kind_ == ProbeKind::kProxy) {
						ReadProxyRequest(client_fd.get());
					}
					break;
				}
				MarkWorkerFinished();
			}

			void ReadProxyRequest(const int client_fd) noexcept {
				std::string request;
				const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (!stop_requested_.load() && std::chrono::steady_clock::now() < deadline) {
					pollfd descriptor{
					    .fd      = client_fd,
					    .events  = POLLIN,
					    .revents = 0,
					};
					const int poll_result = poll(&descriptor, 1, kPollTimeoutMilliseconds);
					if (poll_result < 0) {
						if (errno == EINTR) {
							continue;
						}
						return;
					}
					if (poll_result == 0) {
						continue;
					}
					if ((descriptor.revents & POLLIN) == 0) {
						return;
					}

					std::array<char, 2048> buffer{};
					const ssize_t          bytes_read =
					    recv(client_fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
					if (bytes_read == 0) {
						return;
					}
					if (bytes_read < 0) {
						if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
							continue;
						}
						return;
					}

					const auto bytes = static_cast<std::size_t>(bytes_read);
					if (bytes > kMaxProxyRequestBytes ||
					    request.size() > kMaxProxyRequestBytes - bytes) {
						return;
					}
					request.append(buffer.data(), bytes);
					if (!request.contains("\r\n\r\n")) {
						continue;
					}

					MarkRequestObserved(std::move(request));
					constexpr std::string_view response = "HTTP/1.1 200 Connection Established\r\n"
					                                      "\r\n";
					(void)send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
					return;
				}
			}

			void MarkConnectionObserved() {
				std::scoped_lock lock(state_mutex_);
				connection_observed_ = true;
				state_changed_.notify_all();
			}

			void MarkRequestObserved(std::string request) {
				std::scoped_lock lock(state_mutex_);
				request_          = std::move(request);
				request_observed_ = true;
				state_changed_.notify_all();
			}

			void MarkWorkerFinished() {
				std::scoped_lock lock(state_mutex_);
				worker_finished_ = true;
				state_changed_.notify_all();
			}

			ProbeKind                       kind_;
			ScopedFd                        listener_fd_;
			std::uint16_t                   port_           = 0;
			std::atomic_bool                stop_requested_ = false;
			std::thread                     worker_;
			mutable std::mutex              state_mutex_;
			mutable std::condition_variable state_changed_;
			bool                            connection_observed_ = false;
			bool                            request_observed_    = false;
			bool                            worker_finished_     = false;
			std::string                     request_;
		};

		class ProxyEnvironmentGuard {
		public:
			ProxyEnvironmentGuard()
			    : previous_{} {
				for (std::size_t index = 0; index < kNames.size(); ++index) {
					const char *value = std::getenv(kNames[index]);
					if (value != nullptr) {
						previous_[index] = value;
					}
				}
				clear_ok_ = Clear();
			}

			ProxyEnvironmentGuard(const ProxyEnvironmentGuard &)                     = delete;
			auto operator=(const ProxyEnvironmentGuard &) -> ProxyEnvironmentGuard & = delete;

			~ProxyEnvironmentGuard() {
				for (std::size_t index = 0; index < kNames.size(); ++index) {
					const auto &previous = previous_[index];
					if (previous.has_value()) {
						(void)setenv(kNames[index], previous->c_str(), 1);
					} else {
						(void)unsetenv(kNames[index]);
					}
				}
			}

			[[nodiscard]] auto InitiallyClear() const -> bool {
				return clear_ok_;
			}

			static auto Clear() -> bool {
				bool ok = true;
				for (const char *name : kNames) {
					ok &= unsetenv(name) == 0;
				}
				return ok;
			}

			static auto Set(const char *name, const std::string &value) -> bool {
				return setenv(name, value.c_str(), 1) == 0;
			}

		private:
			static constexpr std::array kNames = {
			    "http_proxy", "HTTP_PROXY",  "https_proxy", "HTTPS_PROXY", "ftp_proxy",
			    "FTP_PROXY",  "sftp_proxy",  "SFTP_PROXY",  "all_proxy",   "ALL_PROXY",
			    "no_proxy",   "NO_PROXY",    "ws_proxy",    "WS_PROXY",    "wss_proxy",
			    "WSS_PROXY",  "socks_proxy", "SOCKS_PROXY",
			};

			std::array<std::optional<std::string>, kNames.size()> previous_;
			bool                                                  clear_ok_ = false;
		};

		auto TestSetoptLong(void * /*context*/, CURL *curl, CURLoption option, const long value)
		    -> CURLcode {
			return curl_easy_setopt(curl, option, value);
		}

		auto TestSetoptOffT(void * /*context*/, CURL *curl, CURLoption option,
		                    const curl_off_t value) -> CURLcode {
			return curl_easy_setopt(curl, option, value);
		}

		auto TestSetoptString(void * /*context*/, CURL *curl, CURLoption option, const char *value)
		    -> CURLcode {
			return curl_easy_setopt(curl, option, value);
		}

		constexpr howdy::native::download_models_internal::CurlSetoptOperations
		    kTestSetoptOperations{
		        .set_long   = TestSetoptLong,
		        .set_off_t  = TestSetoptOffT,
		        .set_string = TestSetoptString,
		    };

		auto
		RealCurlDownloadFile(const std::string                                           &url,
		                     howdy::native::download_models_internal::StagedDownloadFile &staged)
		    -> bool {
			CURL *curl = curl_easy_init();
			if (curl == nullptr) {
				return false;
			}

			howdy::native::download_models_internal::DownloadWriteContext write_context{
			    .staged = &staged,
			};
			bool configured = curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK &&
			                  howdy::native::download_models_internal::ConfigureTransferPolicy(
			                      curl, kTestSetoptOperations);
			configured = configured &&
			             curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
			                              kConnectTimeoutMilliseconds) == CURLE_OK &&
			             curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTransferTimeoutMilliseconds) ==
			                 CURLE_OK;
			configured =
			    configured &&
			    curl_easy_setopt(
			        curl, CURLOPT_WRITEFUNCTION,
			        howdy::native::download_models_internal::DownloadModelsWriteCallback) ==
			        CURLE_OK &&
			    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_context) == CURLE_OK;

			const CURLcode result = configured ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
			curl_easy_cleanup(curl);
			return result == CURLE_OK;
		}

		auto LocalModel(std::string_view url) -> howdy::native::OpenCvModelDescriptor {
			return {
			    .type     = kTestModel.type,
			    .filename = kTestModel.filename,
			    .url      = url,
			    .sha256   = kTestModel.sha256,
			    .size     = kTestModel.size,
			};
		}

		auto RunProxyEnvironmentCase(const std::filesystem::path &temp_root,
		                             std::string_view case_name, const char *proxy_variable,
		                             const bool bypass_proxy) -> bool {
			if (!ProxyEnvironmentGuard::Clear()) {
				return expect(false, "clear proxy environment between cases");
			}
			LoopbackProbe proxy(ProbeKind::kProxy);
			LoopbackProbe origin(ProbeKind::kOrigin);
			if (!proxy.Ok() || !origin.Ok()) {
				return expect(false, "create loopback proxy and origin listeners");
			}

			const auto origin_url =
			    "https://127.0.0.1:" + std::to_string(origin.Port()) + "/test-model.onnx";
			const auto proxy_url      = "http://127.0.0.1:" + std::to_string(proxy.Port());
			bool       environment_ok = true;
			if (bypass_proxy) {
				environment_ok &= ProxyEnvironmentGuard::Set("HTTPS_PROXY", proxy_url);
				environment_ok &= ProxyEnvironmentGuard::Set("NO_PROXY", "127.0.0.1");
			} else {
				environment_ok &= ProxyEnvironmentGuard::Set(proxy_variable, proxy_url);
			}
			if (!environment_ok) {
				return expect(false, "configure proxy environment");
			}

			const auto       model      = LocalModel(origin_url);
			const std::array models     = {model};
			const auto       models_dir = temp_root / std::string(case_name) / "models";
			const auto       output     = temp_root / (std::string(case_name) + "-output.txt");
			int              exit_code  = 0;
			bool ok = expect(RunTestDownload({.models_dir = models_dir, .output = output},
			                                 &exit_code, models, RealCurlDownloadFile),
			                 "run real libcurl download");
			ok &= expect(exit_code == EXIT_FAILURE, "incomplete local TLS transfer fails download");

			if (bypass_proxy) {
				ok &= expect(origin.WaitForConnection(std::chrono::seconds(1)),
				             "NO_PROXY connects directly to local origin");
				ok &= expect(!proxy.WaitForConnection(std::chrono::milliseconds(100)),
				             "NO_PROXY does not connect to proxy");
			} else {
				ok &= expect(proxy.WaitForRequest(std::chrono::seconds(1)),
				             "configured proxy receives request");
				const auto expected_connect =
				    "CONNECT 127.0.0.1:" + std::to_string(origin.Port()) + " ";
				ok &= expect(proxy.Request().starts_with(expected_connect),
				             "HTTPS download uses HTTP CONNECT through proxy");
				ok &= expect(!origin.WaitForConnection(std::chrono::milliseconds(100)),
				             "proxied download does not connect directly to origin");
			}
			ok &= expect(count_files_with_prefix(models_dir, ".howdy-download-") == 0,
			             "failed proxy transfer removes staged file");
			return ok;
		}

	}  // namespace

	auto RunDownloadModelsProxyTests() -> bool {
		namespace fs = std::filesystem;

		const auto      temp_root = fs::current_path() / "howdy-download-models-test";
		std::error_code ec;
		fs::remove_all(temp_root, ec);
		if (ec || !fs::create_directories(temp_root, ec) || ec) {
			return expect(false, "create proxy test temp root");
		}

		ProxyEnvironmentGuard environment;
		bool ok = expect(environment.InitiallyClear(), "clear inherited proxy environment");
		ok &= RunProxyEnvironmentCase(temp_root, "https-proxy", "HTTPS_PROXY", false);
		ok &= RunProxyEnvironmentCase(temp_root, "all-proxy", "ALL_PROXY", false);
		ok &= RunProxyEnvironmentCase(temp_root, "no-proxy", nullptr, true);

		fs::remove_all(temp_root, ec);
		ok &= expect(!ec, "remove proxy test temp root");
		return ok;
	}

}  // namespace howdy::test::download_models
