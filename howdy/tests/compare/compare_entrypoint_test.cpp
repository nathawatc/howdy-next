#include "compare/compare_descriptor_test_support.hpp"
#include "compare/entrypoint.hpp"
#include "model_assets/opencv_model_manifest.hpp"
#include "protocol/compare_descriptors.hpp"
#include "protocol/compare_exit.hpp"
#include "protocol/compare_verdict.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

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

	// Verifies that compare rejects stored models with an incompatible backend or model name,
	// reports kNoFaceModel on both exit status and verdict socket, and closes all protocol fds.
	auto ExpectIncompatibleDescriptorModels(int config_fd, const fs::path &root, uid_t owner)
	    -> bool {
		using howdy::native::CompareExit;
		using howdy::test::Expect;
		using howdy::test::ScopedEnvironmentVariable;
		using howdy::test::compare::DescriptorOpen;
		using howdy::test::compare::OpenAboveProtocol;
		using howdy::test::compare::ScopedDescriptorAt;
		namespace descriptors = howdy::native::compare_descriptors;
		namespace verdict     = howdy::native::compare_verdict;

		struct IncompatibleModelCase {
			const char *label;
			const char *filename;
			std::string json;
		};

		const std::string sface_model(howdy::native::kSfaceModelDescriptor.filename);
		const auto        cases = std::to_array<IncompatibleModelCase>({
		    {
		        .label    = "incompatible backend",
		        .filename = "incompatible-backend-model.dat",
		        .json     = R"([{"id":1,"label":"bad","backend":"other_backend","model":")" +
				            sface_model + R"(","data":[[0.1,0.2]]}])",
		    },
		    {
		        .label    = "incompatible model",
		        .filename = "incompatible-model-name.dat",
		        .json =
		            R"([{"id":1,"label":"bad","backend":"opencv_dnn_sface","model":"other_model.onnx","data":[[0.1,0.2]]}])",
		    },
		});

		std::array<std::string, 2> arguments{"howdy-compare", "coverage-user"};
		std::array<char *, 2>      argv{arguments.at(0).data(), arguments.at(1).data()};
		bool                       ok = true;
		for (const auto &test_case : cases) {
			const auto bad_path = root / test_case.filename;
			ok &= Expect(howdy::test::WriteFile(bad_path, test_case.json) &&
			                 chmod(bad_path.c_str(), 0600) == 0,
			             std::string("write ") + test_case.label + " descriptor-mode model");
			const auto                bad_model = OpenAboveProtocol(bad_path, O_RDONLY);
			const auto                pair      = howdy::test::compare::MakeVerdictPair();
			const ScopedDescriptorAt  verdict_at(verdict::kFd, pair.compare_end.Get());
			const ScopedDescriptorAt  config_at(descriptors::kConfigFd, config_fd);
			const ScopedDescriptorAt  model_at(descriptors::kModelFd, bad_model.Get());
			ScopedEnvironmentVariable verdict_env(verdict::kEnvironmentName);
			ScopedEnvironmentVariable config_env(descriptors::kConfigEnvironmentName);
			ScopedEnvironmentVariable model_env(descriptors::kModelEnvironmentName);
			verdict_env.Set(verdict::kEnvironmentValue);
			config_env.Set(descriptors::kConfigEnvironmentValue);
			model_env.Set(descriptors::kModelEnvironmentValue);

			const std::ostringstream errors;
			int                      status = -1;
			{
				const howdy::test::StreamRedirect redirect(std::cerr, errors.rdbuf());
				status = howdy::native::RunCompareEntrypoint(static_cast<int>(argv.size()),
				                                             argv.data(), owner);
			}
			unsigned char verdict_byte = 0;
			const bool    has_verdict  = read(pair.pam_end.Get(), &verdict_byte, 1) == 1;
			const bool    fds_closed =
			    !DescriptorOpen(descriptors::kConfigFd) && !DescriptorOpen(descriptors::kModelFd);
			ok &= Expect(status == static_cast<int>(CompareExit::kNoFaceModel) && has_verdict &&
			                 verdict_byte == static_cast<int>(CompareExit::kNoFaceModel) &&
			                 !errors.str().empty() && fds_closed,
			             std::string("descriptor mode rejects ") + test_case.label +
			                 " with kNoFaceModel and closes descriptors");
		}
		return ok;
	}

	// PAM hands compare the config at fd 4 and the model at fd 5 next to the verdict socket at
	// fd 3, all announced in the environment. Compare loads from them, closes them, reports its
	// exit code on the verdict socket, and fails closed on any mix it does not expect.
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
		// Decoys for the verdict descriptor: an ordinary file and a stream socket.
		auto decoy_template = std::to_array("/tmp/howdy-verdict-decoy-XXXXXX");
		const howdy::native::ScopedFd decoy(
		    howdy::test::compare::RaiseAboveProtocol(mkstemp(decoy_template.data())).Release());
		(void)unlink(decoy_template.data());
		std::array<int, 2> stream{-1, -1};
		ok &=
		    Expect(decoy.Valid() && socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
			                                   0, stream.data()) == 0,
			       "create verdict decoys");
		const auto stream_peer = howdy::test::compare::RaiseAboveProtocol(stream.at(0));
		const auto stream_end  = howdy::test::compare::RaiseAboveProtocol(stream.at(1));

		struct Case {
			int         config_fd        = -1;
			int         model_fd         = -1;
			bool        announce_config  = true;
			bool        announce_model   = true;
			bool        announce_verdict = true;
			const char *config_env       = descriptors::kConfigEnvironmentValue;
			bool        with_config_flag = false;
			uid_t       owner            = 0;
			// Placed at the verdict descriptor instead of compare's end of the verdict socket.
			int verdict_decoy = -1;
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
			const ScopedDescriptorAt  verdict_at(verdict::kFd, test_case.verdict_decoy >= 0
			                                                       ? test_case.verdict_decoy
																   : pair.compare_end.Get());
			const ScopedDescriptorAt  config_at(descriptors::kConfigFd, test_case.config_fd);
			const ScopedDescriptorAt  model_at(descriptors::kModelFd, test_case.model_fd);
			ScopedEnvironmentVariable verdict_env(verdict::kEnvironmentName);
			ScopedEnvironmentVariable config_env(descriptors::kConfigEnvironmentName);
			ScopedEnvironmentVariable model_env(descriptors::kModelEnvironmentName);
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
				        ? howdy::native::RunCompareEntrypoint(static_cast<int>(config_argv.size()),
				                                              config_argv.data(), test_case.owner)
				        : howdy::native::RunCompareEntrypoint(static_cast<int>(plain_argv.size()),
				                                              plain_argv.data(), test_case.owner);
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

		const auto invalid_shape_path = root / "invalid-shape-model.dat";
		ok &= Expect(howdy::test::WriteFile(invalid_shape_path, "{}") &&
		                 chmod(invalid_shape_path.c_str(), 0600) == 0,
		             "write invalid-shape descriptor-mode model");
		const auto invalid_shape_model = OpenAboveProtocol(invalid_shape_path, O_RDONLY);
		const auto invalid_shape_run   = run({
		    .config_fd = config.Get(),
		    .model_fd  = invalid_shape_model.Get(),
		    .owner     = owner,
		});
		ok &= Expect(aborted(invalid_shape_run, "") && !invalid_shape_run.diagnostics.empty(),
		             "descriptor mode aborts with diagnostic on invalid-shape model JSON object");
		ok &= ExpectIncompatibleDescriptorModels(config.Get(), root, owner);

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
		ok &= Expect(aborted(config_only, "announced as"),
		             "descriptor mode needs both descriptors announced");
		ok &= Expect(!config_only.config_open && config_only.model_open,
		             "compare closes what it was announced and leaves the rest alone");

		const auto wrong_number = run({
		    .config_fd  = config.Get(),
		    .model_fd   = model.Get(),
		    .config_env = "7",
		    .owner      = owner,
		});
		ok &= Expect(aborted(wrong_number, "announced as"),
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
		                     "Unknown argument: --config"),
		             "compare no longer takes a config path");

		const auto nothing = run({
		    .announce_config  = false,
		    .announce_model   = false,
		    .announce_verdict = false,
		    .owner            = owner,
		});
		ok &= Expect(aborted(nothing, "needs its config and model descriptors"),
		             "compare without descriptors fails closed");

		// Announced, but something else sits at the verdict descriptor, as when a wrapper closed
		// it and the number was reused: compare writes nothing there and fails closed.
		const auto  file_decoy = run({
		    .config_fd     = config.Get(),
		    .model_fd      = model.Get(),
		    .owner         = owner,
		    .verdict_decoy = decoy.Get(),
		});
		struct stat decoy_status{};
		ok &= Expect(aborted(file_decoy, "verdict socket") &&
		                 fstat(decoy.Get(), &decoy_status) == 0 && decoy_status.st_size == 0,
		             "compare writes no verdict to a descriptor that is not a socket");
		const auto    stream_decoy = run({
		    .config_fd     = config.Get(),
		    .model_fd      = model.Get(),
		    .owner         = owner,
		    .verdict_decoy = stream_end.Get(),
		});
		unsigned char byte         = 0;
		ok &= Expect(aborted(stream_decoy, "verdict socket") &&
		                 read(stream_peer.Get(), &byte, 1) < 0 && errno == EAGAIN,
		             "compare writes no verdict to a socket of the wrong type");
		return ok;
	}

	struct HelpProtocolCase {
		const char *label;
		const char *verdict_env;
		const char *config_env;
		const char *model_env;
		bool        attach_verdict;
		bool        attach_config;
		bool        attach_model;
		bool        expect_verdict_abort;
	};

	struct HelpProtocolFixture {
		int   config_fd;
		int   model_fd;
		uid_t owner;
	};

	void ApplyOptionalEnvironment(howdy::test::ScopedEnvironmentVariable &variable,
	                              const char                             *value) {
		if (value != nullptr) {
			variable.Set(value);
		}
	}

	auto RunHelpProtocolCase(const HelpProtocolCase &test_case, const char *help_flag,
	                         const HelpProtocolFixture &fixture) -> bool {
		using howdy::native::CompareExit;
		using howdy::test::Expect;
		using howdy::test::ScopedEnvironmentVariable;
		using howdy::test::compare::DescriptorOpen;
		using howdy::test::compare::ScopedDescriptorAt;
		namespace descriptors = howdy::native::compare_descriptors;
		namespace verdict     = howdy::native::compare_verdict;

		std::array<std::string, 2> help_arguments{"howdy-compare", help_flag};
		std::array<char *, 2>      help_argv{
		    help_arguments.at(0).data(),
		    help_arguments.at(1).data(),
		};
		const auto               pair = howdy::test::compare::MakeVerdictPair();
		const ScopedDescriptorAt verdict_at(verdict::kFd,
		                                    test_case.attach_verdict ? pair.compare_end.Get() : -1);
		const ScopedDescriptorAt config_at(descriptors::kConfigFd,
		                                   test_case.attach_config ? fixture.config_fd : -1);
		const ScopedDescriptorAt model_at(descriptors::kModelFd,
		                                  test_case.attach_model ? fixture.model_fd : -1);
		ScopedEnvironmentVariable verdict_env(verdict::kEnvironmentName);
		ScopedEnvironmentVariable config_env(descriptors::kConfigEnvironmentName);
		ScopedEnvironmentVariable model_env(descriptors::kModelEnvironmentName);
		ApplyOptionalEnvironment(verdict_env, test_case.verdict_env);
		ApplyOptionalEnvironment(config_env, test_case.config_env);
		ApplyOptionalEnvironment(model_env, test_case.model_env);

		const std::ostringstream out_stream;
		const std::ostringstream err_stream;
		int                      status = -1;
		{
			const howdy::test::StreamRedirect out_redirect(std::cout, out_stream.rdbuf());
			const howdy::test::StreamRedirect err_redirect(std::cerr, err_stream.rdbuf());
			status = howdy::native::RunCompareEntrypoint(static_cast<int>(help_argv.size()),
			                                             help_argv.data(), fixture.owner);
		}
		unsigned char byte          = 0;
		const ssize_t verdict_bytes = read(pair.pam_end.Get(), &byte, 1);
		const bool    verdict_ok =
		    test_case.expect_verdict_abort
		        ? (verdict_bytes == 1 && byte == static_cast<unsigned char>(CompareExit::kAbort))
		        : (verdict_bytes < 0 && errno == EAGAIN);
		const bool descriptors_closed =
		    !DescriptorOpen(descriptors::kConfigFd) && !DescriptorOpen(descriptors::kModelFd);
		return Expect(status == static_cast<int>(CompareExit::kAbort) && verdict_ok &&
		                  descriptors_closed && out_stream.str().empty() &&
		                  err_stream.str().contains(std::string("Unknown argument: ") + help_flag),
		              test_case.label);
	}

	auto ExpectHelpProtocolHandling(const fs::path &root) -> bool {
		using howdy::native::CompareExit;
		using howdy::test::Expect;
		using howdy::test::ScopedEnvironmentVariable;
		using howdy::test::compare::OpenAboveProtocol;
		using howdy::test::compare::ScopedDescriptorAt;
		namespace descriptors = howdy::native::compare_descriptors;
		namespace verdict     = howdy::native::compare_verdict;

		bool       ok         = true;
		const auto model_path = root / "help-protocol-model.dat";
		ok &=
		    Expect(howdy::test::WriteFile(model_path, "[]") && chmod(model_path.c_str(), 0600) == 0,
			       "write help-protocol fixture model");
		const auto                config = OpenAboveProtocol(root / "config.ini", O_RDONLY);
		const auto                model  = OpenAboveProtocol(model_path, O_RDONLY);
		const HelpProtocolFixture fixture{
		    .config_fd = config.Get(),
		    .model_fd  = model.Get(),
		    .owner     = geteuid(),
		};
		ok &= Expect(config.Valid() && model.Valid(), "open help-protocol config and model");

		const auto protocol_cases = std::to_array<HelpProtocolCase>({
		    {
		        .label                = "full descriptor mode rejects help with abort verdict",
		        .verdict_env          = verdict::kEnvironmentValue,
		        .config_env           = descriptors::kConfigEnvironmentValue,
		        .model_env            = descriptors::kModelEnvironmentValue,
		        .attach_verdict       = true,
		        .attach_config        = true,
		        .attach_model         = true,
		        .expect_verdict_abort = true,
		    },
		    {
		        .label                = "verdict-only env rejects help with abort verdict",
		        .verdict_env          = verdict::kEnvironmentValue,
		        .config_env           = nullptr,
		        .model_env            = nullptr,
		        .attach_verdict       = true,
		        .attach_config        = false,
		        .attach_model         = false,
		        .expect_verdict_abort = true,
		    },
		    {
		        .label                = "config-only env rejects help and closes announced config",
		        .verdict_env          = nullptr,
		        .config_env           = descriptors::kConfigEnvironmentValue,
		        .model_env            = nullptr,
		        .attach_verdict       = false,
		        .attach_config        = true,
		        .attach_model         = false,
		        .expect_verdict_abort = false,
		    },
		    {
		        .label                = "model-only env rejects help and closes announced model",
		        .verdict_env          = nullptr,
		        .config_env           = nullptr,
		        .model_env            = descriptors::kModelEnvironmentValue,
		        .attach_verdict       = false,
		        .attach_config        = false,
		        .attach_model         = true,
		        .expect_verdict_abort = false,
		    },
		    {
		        .label                = "malformed verdict env rejects help without verdict write",
		        .verdict_env          = "99",
		        .config_env           = nullptr,
		        .model_env            = nullptr,
		        .attach_verdict       = true,
		        .attach_config        = false,
		        .attach_model         = false,
		        .expect_verdict_abort = false,
		    },
		    {
		        .label                = "empty config env rejects help",
		        .verdict_env          = nullptr,
		        .config_env           = "",
		        .model_env            = nullptr,
		        .attach_verdict       = false,
		        .attach_config        = false,
		        .attach_model         = false,
		        .expect_verdict_abort = false,
		    },
		    {
		        .label                = "wrong model env rejects help",
		        .verdict_env          = nullptr,
		        .config_env           = nullptr,
		        .model_env            = "7",
		        .attach_verdict       = false,
		        .attach_config        = false,
		        .attach_model         = false,
		        .expect_verdict_abort = false,
		    },
		});

		for (const char *help_flag : {"--help", "-h"}) {
			for (const auto &test_case : protocol_cases) {
				ok &= RunHelpProtocolCase(test_case, help_flag, fixture);
			}

			// Standalone invocation with no compare protocol environment variables keeps --help/-h.
			std::array<std::string, 2> help_arguments{"howdy-compare", help_flag};
			std::array<char *, 2>      help_argv{
			    help_arguments.at(0).data(),
			    help_arguments.at(1).data(),
			};
			const auto                pair = howdy::test::compare::MakeVerdictPair();
			const ScopedDescriptorAt  verdict_at(verdict::kFd, pair.compare_end.Get());
			ScopedEnvironmentVariable verdict_env(verdict::kEnvironmentName);
			ScopedEnvironmentVariable config_env(descriptors::kConfigEnvironmentName);
			ScopedEnvironmentVariable model_env(descriptors::kModelEnvironmentName);
			const std::ostringstream  out_stream;
			const std::ostringstream  err_stream;
			int                       status = -1;
			{
				const howdy::test::StreamRedirect out_redirect(std::cout, out_stream.rdbuf());
				const howdy::test::StreamRedirect err_redirect(std::cerr, err_stream.rdbuf());
				status = howdy::native::RunCompareEntrypoint(static_cast<int>(help_argv.size()),
				                                             help_argv.data(), fixture.owner);
			}
			unsigned char byte = 0;
			ok &= Expect(status == static_cast<int>(CompareExit::kSuccess) &&
			                 read(pair.pam_end.Get(), &byte, 1) < 0 && errno == EAGAIN &&
			                 out_stream.str().contains("Usage: howdy-compare <user>\n") &&
			                 err_stream.str().empty(),
			             "standalone compare help prints usage, exits 0, and writes no verdict");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	using howdy::test::Expect;

	const fs::path root(HOWDY_COMPARE_NO_MODEL_ROOT);
	if (!Expect(PrepareFixture(root), "prepare secure compare no-model fixture")) {
		return 1;
	}

	bool ok = true;
	ok &= ExpectDescriptorMode(root);
	ok &= ExpectHelpProtocolHandling(root);

	std::error_code cleanup_error;
	fs::remove_all(root, cleanup_error);
	ok &= Expect(!cleanup_error, "clean compare no-model fixture");
	return ok ? 0 : 1;
}
