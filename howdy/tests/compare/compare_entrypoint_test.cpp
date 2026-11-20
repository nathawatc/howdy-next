#include "compare/compare_descriptor_test_support.hpp"
#include "compare/entrypoint.hpp"
#include "compare/entrypoint/internal.hpp"
#include "config/runtime_config.hpp"
#include "model_assets/model_file.hpp"
#include "protocol/compare_descriptors.hpp"
#include "protocol/compare_exit.hpp"
#include "protocol/compare_verdict.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"
#include "vision/face_model_test_access.hpp"

#include <array>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <opencv2/core/cvstd_wrapper.hpp>
#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/objdetect/face.hpp>

#include <sys/socket.h>
#include <sys/stat.h>

#ifndef HOWDY_PACKAGED_CONFIG_PATH
#	error "HOWDY_PACKAGED_CONFIG_PATH must be defined by CMake"
#endif
#ifndef HOWDY_COMPARE_NO_MODEL_ROOT
#	error "HOWDY_COMPARE_NO_MODEL_ROOT must be defined by CMake"
#endif

namespace {

	namespace fs = std::filesystem;

	struct WarmCounts {
		int alignments = 0;
		int features   = 0;
	};

	class FakeDetector final : public cv::FaceDetectorYN {
	public:
		void setInputSize([[maybe_unused]] const cv::Size &input_size) override {}

		auto getInputSize() -> cv::Size override {
			return {320, 320};
		}

		void setScoreThreshold([[maybe_unused]] float score_threshold) override {}

		auto getScoreThreshold() -> float override {
			return 0.9F;
		}

		void setNMSThreshold([[maybe_unused]] float nms_threshold) override {}

		auto getNMSThreshold() -> float override {
			return 0.3F;
		}

		void setTopK([[maybe_unused]] int top_k) override {}

		auto getTopK() -> int override {
			return 5000;
		}

		auto detect([[maybe_unused]] cv::InputArray image, cv::OutputArray faces) -> int override {
			faces.release();
			return 0;
		}
	};

	class FakeRecognizer final : public cv::FaceRecognizerSF {
	public:
		explicit FakeRecognizer(WarmCounts *counts)
		    : counts_(counts) {}

		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		void alignCrop(cv::InputArray image, [[maybe_unused]] cv::InputArray face,
		               cv::OutputArray aligned) const override {
			++counts_->alignments;
			image.getMat().copyTo(aligned);
		}

		void feature([[maybe_unused]] cv::InputArray aligned, cv::OutputArray feature) override {
			++counts_->features;
			cv::Mat(1, 128, CV_32FC1, cv::Scalar(1.0F)).copyTo(feature);
		}

		[[nodiscard]] auto match([[maybe_unused]] cv::InputArray first,
		                         [[maybe_unused]] cv::InputArray second,
		                         [[maybe_unused]] int dis_type) const -> double override {
			return 0.0;
		}

	private:
		WarmCounts *counts_;
	};

	auto TestProductionWarmBridge() -> bool {
		WarmCounts counts;
		int        yunet_calls = 0;
		auto       backend     = howdy::native::FaceModelTestAccess::Backend{
		    .check_readiness = [](const fs::path &) -> howdy::native::OpenCvModelReadiness {
			    return {.status = howdy::native::OpenCvModelStatus::kOk};
		    },
		    .create_detector = [](const std::string &, const cv::Size &, float, float,
			                      int) -> cv::Ptr<cv::FaceDetectorYN> {
			    return cv::makePtr<FakeDetector>();
		    },
		    .create_recognizer = [&counts](const std::string &) -> cv::Ptr<cv::FaceRecognizerSF> {
			    return cv::makePtr<FakeRecognizer>(&counts);
		    },
		    .set_input_size = [](const cv::Size &) -> void {},
		    .detect         = [&yunet_calls](const cv::Mat &, cv::Mat &faces) -> void {
			    ++yunet_calls;
			    faces.release();
		    },
		};
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		if (!howdy::test::Expect(model.Ok() && counts.alignments == 0 && counts.features == 0,
		                         "production model begins without recognizer warm-up")) {
			return false;
		}
		return howdy::test::Expect(
		    howdy::native::compare_entrypoint_internal::WarmModel(model) &&
		        counts.alignments == 1 && counts.features == 1 && yunet_calls == 0,
		    "production warm bridge invokes SFace alignment and feature without YuNet");
	}

	auto PrepareFixture(const fs::path &root) -> bool {
		std::error_code error;
		fs::remove_all(root, error);
		error.clear();
		if (!fs::create_directories(root / "models", error) || error) {
			return false;
		}
		fs::copy_file(HOWDY_PACKAGED_CONFIG_PATH, root / "config.ini",
		              fs::copy_options::overwrite_existing, error);
		if (error) {
			return false;
		}
		fs::permissions(root,
		                fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
		                    fs::perms::others_read | fs::perms::others_exec,
		                fs::perm_options::replace, error);
		if (error) {
			return false;
		}
		fs::permissions(root / "models",
		                fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
		                    fs::perms::others_read | fs::perms::others_exec,
		                fs::perm_options::replace, error);
		if (error) {
			return false;
		}
		fs::permissions(root / "config.ini",
		                fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
		                    fs::perms::others_read,
		                fs::perm_options::replace, error);
		return !error;
	}

	// With the verdict socket at its descriptor and named in the environment, compare writes its
	// exit code there as one byte; without the variable, or when the descriptor is not a socket,
	// it writes nothing.
	template <typename RunCompare>
	auto ExpectVerdictReported(const RunCompare &run_compare, int expected) -> bool {
		using howdy::native::compare_verdict::kEnvironmentName;
		using howdy::native::compare_verdict::kEnvironmentValue;
		using howdy::native::compare_verdict::kFd;
		using howdy::test::Expect;
		std::array<int, 2> sockets{-1, -1};
		if (!Expect(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
		                       sockets.data()) == 0,
		            "create verdict socket")) {
			return false;
		}
		// Both ends move above the verdict descriptor, so placing compare's end there below
		// cannot close one of them.
		const howdy::native::ScopedFd pam_end(fcntl(sockets.at(0), F_DUPFD_CLOEXEC, kFd + 1));
		const howdy::native::ScopedFd compare_end(fcntl(sockets.at(1), F_DUPFD_CLOEXEC, kFd + 1));
		(void)close(sockets.at(0));
		(void)close(sockets.at(1));
		if (!Expect(pam_end.Valid() && compare_end.Valid(),
		            "move verdict socket above its descriptor")) {
			return false;
		}
		// Keep whatever the test process has at the verdict descriptor.
		const howdy::native::ScopedFd saved(fcntl(kFd, F_DUPFD_CLOEXEC, kFd + 1));
		bool                          ok =
		    Expect(dup2(compare_end.Get(), kFd) == kFd, "place verdict socket at its descriptor");

		const int     unannounced = run_compare();
		unsigned char verdict     = 0;
		ok &= Expect(unannounced == expected && read(pam_end.Get(), &verdict, 1) < 0 &&
		                 errno == EAGAIN,
		             "compare writes no verdict without the environment variable");

		ok &=
		    Expect(setenv(kEnvironmentName, kEnvironmentValue, 1) == 0, "announce verdict socket");
		const int announced = run_compare();
		ok &= Expect(announced == expected && read(pam_end.Get(), &verdict, 1) == 1 &&
		                 verdict == static_cast<unsigned char>(expected),
		             "compare reports its exit code on the verdict socket");

		// Announced, but the descriptor is an ordinary file, as when a wrapper closed it and
		// something else took the number: compare must leave it untouched.
		auto decoy_template = std::to_array("/tmp/howdy-verdict-decoy-XXXXXX");
		const howdy::native::ScopedFd decoy(mkstemp(decoy_template.data()));
		(void)unlink(decoy_template.data());
		ok &= Expect(decoy.Valid() && dup2(decoy.Get(), kFd) == kFd,
		             "place an ordinary file at the verdict descriptor");
		const int   decoyed = run_compare();
		struct stat decoy_status{};
		ok &= Expect(decoyed == expected && fstat(decoy.Get(), &decoy_status) == 0 &&
		                 decoy_status.st_size == 0,
		             "compare writes no verdict to a descriptor that is not a socket");

		// Announced, but an unrelated stream socket sits at the descriptor: also left alone.
		std::array<int, 2> stream{-1, -1};
		const bool         streamed =
		    socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, stream.data()) == 0;
		const howdy::native::ScopedFd stream_peer(stream.at(0));
		const howdy::native::ScopedFd stream_end(stream.at(1));
		ok &= Expect(streamed && dup2(stream_end.Get(), kFd) == kFd,
		             "place a stream socket at the verdict descriptor");
		const int streamed_result = run_compare();
		ok &= Expect(streamed_result == expected && read(stream_peer.Get(), &verdict, 1) < 0 &&
		                 errno == EAGAIN,
		             "compare writes no verdict to a socket of the wrong type");
		ok &= Expect(unsetenv(kEnvironmentName) == 0, "withdraw verdict socket");

		if (saved.Valid()) {
			ok &= Expect(dup2(saved.Get(), kFd) == kFd, "restore the verdict descriptor");
		} else {
			(void)close(kFd);
		}
		return ok;
	}

	// Descriptor mode: PAM hands compare the config at fd 4 and the model at fd 5 next to the
	// verdict socket at fd 3, all announced in the environment. Compare loads from them, closes
	// them, and fails closed on any mix it does not expect.
	auto ExpectDescriptorMode(const fs::path &root) -> bool {
		using howdy::native::CompareExit;
		using howdy::test::Expect;
		using howdy::test::ScopedEnvironmentVariable;
		using howdy::test::compare::DescriptorOpen;
		using howdy::test::compare::OpenAboveProtocol;
		using howdy::test::compare::ScopedDescriptorAt;
		namespace descriptors = howdy::native::compare_descriptors;
		namespace verdict     = howdy::native::compare_verdict;

		bool       ok         = true;
		const auto model_path = root / "descriptor-model.dat";
		ok &=
		    Expect(howdy::test::WriteFile(model_path, "[]") && chmod(model_path.c_str(), 0600) == 0,
			       "write descriptor-mode model");
		const auto config = OpenAboveProtocol(root / "config.ini", O_RDONLY);
		const auto model  = OpenAboveProtocol(model_path, O_RDONLY);
		ok &= Expect(config.Valid() && model.Valid(), "open descriptor-mode config and model");

		std::array<std::string, 2> plain_arguments{
		    "howdy-compare",
		    "coverage-user",
		};
		std::array<char *, 2> plain_argv{
		    plain_arguments.at(0).data(),
		    plain_arguments.at(1).data(),
		};
		std::array<std::string, 4> config_arguments{
		    "howdy-compare",
		    "--config",
		    (root / "config.ini").string(),
		    "coverage-user",
		};
		std::array<char *, 4> config_argv{
		    config_arguments.at(0).data(),
		    config_arguments.at(1).data(),
		    config_arguments.at(2).data(),
		    config_arguments.at(3).data(),
		};

		struct Case {
			int         config_fd        = -1;
			int         model_fd         = -1;
			bool        announce_config  = true;
			bool        announce_model   = true;
			bool        announce_verdict = true;
			const char *config_env       = descriptors::kConfigEnvironmentValue;
			bool        with_config_flag = false;
			uid_t       owner            = 0;
		};

		struct Outcome {
			int         status      = -1;
			int         verdict     = -1;
			bool        config_open = false;
			bool        model_open  = false;
			std::string diagnostics;
		};

		// One compare run with the case's descriptors in place; reports what compare returned and
		// wrote, and whether fds 4 and 5 were still open when it returned.
		const auto run = [&](const Case &test_case) -> Outcome {
			const auto                pair = howdy::test::compare::MakeVerdictPair();
			const ScopedDescriptorAt  verdict_at(verdict::kFd, pair.compare_end.Get());
			const ScopedDescriptorAt  config_at(descriptors::kConfigFd, test_case.config_fd);
			const ScopedDescriptorAt  model_at(descriptors::kModelFd, test_case.model_fd);
			ScopedEnvironmentVariable verdict_env(verdict::kEnvironmentName);
			ScopedEnvironmentVariable config_env(descriptors::kConfigEnvironmentName);
			ScopedEnvironmentVariable model_env(descriptors::kModelEnvironmentName);
			// The path environment names a missing config, which descriptor mode never reads.
			const ScopedEnvironmentVariable broken_config("HOWDY_CONFIG",
			                                              (root / "missing.ini").string());
			if (test_case.announce_verdict) {
				verdict_env.Set(verdict::kEnvironmentValue);
			}
			if (test_case.announce_config) {
				config_env.Set(test_case.config_env);
			}
			if (test_case.announce_model) {
				model_env.Set(descriptors::kModelEnvironmentValue);
			}

			Outcome                  outcome;
			const std::ostringstream errors;
			{
				const howdy::test::StreamRedirect redirect(std::cerr, errors.rdbuf());
				outcome.status =
				    test_case.with_config_flag
				        ? howdy::native::RunCompareEntrypoint(
				              static_cast<int>(config_argv.size()), config_argv.data(),
				              test_case.owner,
				              howdy::native::file_security_internal::ValidationRoot{.path = root})
				        : howdy::native::RunCompareEntrypoint(
				              static_cast<int>(plain_argv.size()), plain_argv.data(),
				              test_case.owner,
				              howdy::native::file_security_internal::ValidationRoot{.path = root});
			}
			outcome.diagnostics = errors.str();
			outcome.config_open = DescriptorOpen(descriptors::kConfigFd);
			outcome.model_open  = DescriptorOpen(descriptors::kModelFd);
			unsigned char byte  = 0;
			if (read(pair.pam_end.Get(), &byte, 1) == 1) {
				outcome.verdict = byte;
			}
			return outcome;
		};
		const auto aborted = [](const Outcome &outcome, std::string_view diagnostic) -> bool {
			return outcome.status == static_cast<int>(CompareExit::kAbort) &&
			       outcome.diagnostics.contains(diagnostic);
		};
		const uid_t owner = geteuid();

		const auto loaded = run({
		    .config_fd = config.Get(),
		    .model_fd  = model.Get(),
		    .owner     = owner,
		});
		ok &= Expect(loaded.status == static_cast<int>(CompareExit::kNoFaceModel) &&
		                 loaded.verdict == static_cast<int>(CompareExit::kNoFaceModel),
		             "descriptor mode loads config and model and reports the verdict");
		ok &= Expect(!loaded.config_open && !loaded.model_open,
		             "descriptor mode closes the config and model descriptors");

		ok &= Expect(aborted(run({
		                         .config_fd = config.Get(),
		                         .model_fd  = model.Get(),
		                         .owner     = static_cast<uid_t>(owner + 1),
		                     }),
		                     "Config file must be owned by UID"),
		             "descriptor mode rejects a config descriptor with another owner");

		const auto writable_model = OpenAboveProtocol(model_path, O_RDWR);
		ok &= Expect(aborted(run({
		                         .config_fd = config.Get(),
		                         .model_fd  = writable_model.Get(),
		                         .owner     = owner,
		                     }),
		                     "must be open read-only"),
		             "descriptor mode rejects a writable model descriptor");

		const auto config_only = run({
		    .config_fd      = config.Get(),
		    .model_fd       = model.Get(),
		    .announce_model = false,
		    .owner          = owner,
		});
		ok &= Expect(aborted(config_only, "announced together"),
		             "descriptor mode needs both descriptors announced");
		ok &= Expect(!config_only.config_open && config_only.model_open,
		             "compare closes what it was announced and leaves the rest alone");

		const auto wrong_number = run({
		    .config_fd  = config.Get(),
		    .model_fd   = model.Get(),
		    .config_env = "7",
		    .owner      = owner,
		});
		ok &= Expect(aborted(wrong_number, "announced together"),
		             "descriptor mode rejects a descriptor announced at another number");
		ok &= Expect(wrong_number.config_open && !wrong_number.model_open,
		             "compare leaves a descriptor announced at another number alone");

		ok &= Expect(aborted(run({
		                         .config_fd = -1,
		                         .model_fd  = model.Get(),
		                         .owner     = owner,
		                     }),
		                     "is not open"),
		             "descriptor mode rejects an announced descriptor that is not open");

		const auto no_verdict = run({
		    .config_fd        = config.Get(),
		    .model_fd         = model.Get(),
		    .announce_verdict = false,
		    .owner            = owner,
		});
		ok &= Expect(aborted(no_verdict, "verdict socket") && !no_verdict.config_open &&
		                 !no_verdict.model_open,
		             "descriptor mode requires the verdict socket");

		ok &= Expect(aborted(run({
		                         .config_fd        = config.Get(),
		                         .model_fd         = model.Get(),
		                         .with_config_flag = true,
		                         .owner            = owner,
		                     }),
		                     "cannot be combined"),
		             "descriptor mode rejects --config");
		return ok;
	}

}  // namespace

auto main() -> int {
	using howdy::test::Expect;

	const fs::path root(HOWDY_COMPARE_NO_MODEL_ROOT);
	if (!Expect(PrepareFixture(root), "prepare secure compare no-model fixture")) {
		return 1;
	}

	const auto config_path = (root / "config.ini").string();
	const auto models_path = (root / "models").string();
	bool       ok          = true;
	ok &= TestProductionWarmBridge();
	ok &= Expect(setenv("HOWDY_CONFIG", config_path.c_str(), 1) == 0, "set compare config path");
	ok &= Expect(setenv("HOWDY_USER_MODELS_DIR", models_path.c_str(), 1) == 0,
	             "set empty user-model directory");

	std::array<std::string, 2> arguments{"howdy-compare", "coverage-user"};
	std::array<char *, 2>      argv{arguments.at(0).data(), arguments.at(1).data()};
	const auto                 run_compare = [&] -> int {
		return howdy::native::RunCompareEntrypoint(
		    static_cast<int>(argv.size()), argv.data(), geteuid(),
		    howdy::native::file_security_internal::ValidationRoot{.path = root});
	};
	const int result = run_compare();
	ok &= Expect(result == static_cast<int>(howdy::native::CompareExit::kNoFaceModel),
	             "empty secure user-model directory returns kNoFaceModel");
	ok &= ExpectVerdictReported(run_compare, result);
	ok &= ExpectDescriptorMode(root);

	std::error_code cleanup_error;
	fs::remove_all(root, cleanup_error);
	ok &= Expect(!cleanup_error, "clean compare no-model fixture");
	return ok ? 0 : 1;
}
