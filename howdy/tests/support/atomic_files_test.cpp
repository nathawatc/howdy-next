#include "cli/download_models/internal.hpp"
#include "test_support.hpp"

#include <array>
#include <csignal>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/stat.h>

namespace howdy::test::download_models {

	using howdy::test::CountFilesWithPrefix;
	using howdy::test::Expect;
	using howdy::test::ReadFile;
	using howdy::test::WriteFile;

	auto RunAtomicFilesTests() -> bool;

	namespace {

		auto CommitIsDurable(howdy::native::AtomicFileCommitResult result) -> bool {
			return howdy::native::AtomicFileCommitIsDurable(result);
		}

		auto FailParentSync(const std::filesystem::path & /*path*/) -> bool {
			return false;
		}

		template <typename Value, typename Callback>
		void WithPresent(std::optional<Value> &value, Callback callback) {
			if (value.has_value()) {
				callback();
			}
		}

	}  // namespace

	auto RunAtomicFilesTests() -> bool {
		namespace fs              = std::filesystem;
		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-download-models-test";
		std::error_code ec;

		howdy::native::StagedFile empty_path_install{
		    .fd   = howdy::native::ScopedFd(open("/dev/null", O_WRONLY)),
		    .path = {},
		};
		ok &= Expect(empty_path_install.fd.Get() >= 0, "open fd for empty-path staged install");
		ok &= Expect(!CommitIsDurable(howdy::native::InstallStagedFile(empty_path_install,
		                                                               temp_root / "unused")),
		             "staged install rejects empty path");
		ok &= Expect(empty_path_install.fd.Get() < 0, "empty-path staged install closes fd");

		const auto blocked_parent = temp_root / "blocked-parent";
		ok &= Expect(WriteFile(blocked_parent, "not a directory"),
		             "create file blocking staged parent directory");
		ok &= Expect(!howdy::native::PrepareStagedFile(blocked_parent / "models" / "model.onnx",
		                                               ".howdy-download-")
		                  .has_value(),
		             "prepare staged file returns nullopt when parent creation fails");
		ok &= Expect(!howdy::native::SyncParentDirectory(blocked_parent / "model.onnx"),
		             "parent-directory sync reports open failure");

		const auto require_existing_parent      = temp_root / "require-existing-parent";
		const auto require_existing_destination = require_existing_parent / "model.onnx";
		fs::create_directory(require_existing_parent, ec);
		ok &= Expect(!ec, "create existing parent for require-existing policy");
		auto require_existing_staged = howdy::native::PrepareStagedFile(
		    require_existing_destination, ".howdy-require-existing-",
		    howdy::native::kDefaultAtomicFileMode,
		    howdy::native::StagedFileMetadataPolicy::kPreserveExisting,
		    howdy::native::StagedFileParentPolicy::kRequireExisting);
		ok &= Expect(require_existing_staged.has_value(),
		             "require-existing policy accepts existing real parent");
		WithPresent(require_existing_staged, [&] -> void {
			ok &= Expect(fs::exists(require_existing_staged->path, ec) && !ec,
			             "require-existing policy creates staged file in real parent");
			howdy::native::CleanupStagedFile(*require_existing_staged);
		});
		fs::remove_all(require_existing_parent, ec);
		ec.clear();

		const auto missing_require_existing_parent = temp_root / "missing-require-existing-parent";
		const auto missing_require_existing_destination =
		    missing_require_existing_parent / "model.onnx";
		fs::remove_all(missing_require_existing_parent, ec);
		ec.clear();
		auto missing_require_existing_staged = howdy::native::PrepareStagedFile(
		    missing_require_existing_destination, ".howdy-require-existing-",
		    howdy::native::kDefaultAtomicFileMode,
		    howdy::native::StagedFileMetadataPolicy::kPreserveExisting,
		    howdy::native::StagedFileParentPolicy::kRequireExisting);
		ok &= Expect(!missing_require_existing_staged.has_value(),
		             "require-existing policy rejects missing parent");
		ok &= Expect(!fs::exists(missing_require_existing_parent, ec) && !ec,
		             "require-existing policy does not create missing parent");

		const auto symlink_target      = temp_root / "require-existing-symlink-target";
		const auto symlink_parent      = temp_root / "require-existing-symlink-parent";
		const auto symlink_destination = symlink_parent / "model.onnx";
		fs::remove_all(symlink_target, ec);
		fs::remove(symlink_parent, ec);
		ec.clear();
		fs::create_directory(symlink_target, ec);
		ok &= Expect(!ec, "create require-existing symlink target");
		const bool symlink_created = symlink(symlink_target.c_str(), symlink_parent.c_str()) == 0;
		ok &= Expect(symlink_created, "create require-existing symlink parent");
		if (symlink_created) {
			auto symlink_staged = howdy::native::PrepareStagedFile(
			    symlink_destination, ".howdy-require-existing-",
			    howdy::native::kDefaultAtomicFileMode,
			    howdy::native::StagedFileMetadataPolicy::kPreserveExisting,
			    howdy::native::StagedFileParentPolicy::kRequireExisting);
			ok &= Expect(!symlink_staged.has_value(),
			             "require-existing policy rejects symlink parent");
			ok &= Expect(CountFilesWithPrefix(symlink_target, ".howdy-require-existing-") == 0,
			             "require-existing policy creates no staged file through symlink");
		}
		fs::remove(symlink_parent, ec);
		fs::remove_all(symlink_target, ec);
		ec.clear();

		const auto overflow_destination = temp_root / "overflow-write.onnx";
		auto       overflow_staged =
		    howdy::native::PrepareStagedFile(overflow_destination, ".howdy-download-");
		ok &=
		    Expect(overflow_staged.has_value(), "prepare staged file for overflow write callback");
		WithPresent(overflow_staged, [&] -> void {
			howdy::native::download_models_internal::DownloadWriteContext write_context{
			    .staged = &*overflow_staged,
			};
			std::array<char, 2> payload        = {'x', 'y'};
			const auto          overflow_size  = (std::numeric_limits<std::size_t>::max() / 2) + 2;
			const auto          overflow_count = static_cast<std::size_t>(2);
			const auto          result =
			    howdy::native::download_models_internal::DownloadModelsWriteCallback(
			        payload.data(), overflow_size, overflow_count, &write_context);
			ok &= Expect(result == 0, "overflowing write callback returns 0");
			ok &= Expect(ReadFile(overflow_staged->path).empty(),
			             "overflowing write callback leaves staged file empty");
			howdy::native::CleanupStagedFile(*overflow_staged);
		});

		const auto limited_destination = temp_root / "limited-write.onnx";
		auto       limited_staged =
		    howdy::native::PrepareStagedFile(limited_destination, ".howdy-download-");
		ok &= Expect(limited_staged.has_value(), "prepare staged file for cumulative write limit");
		WithPresent(limited_staged, [&] -> void {
			howdy::native::download_models_internal::DownloadWriteContext write_context{
			    .staged    = &*limited_staged,
			    .max_bytes = 3,
			};
			std::array<char, 2> first_payload  = {'x', 'y'};
			std::array<char, 2> second_payload = {'z', '!'};
			const auto          first_result =
			    howdy::native::download_models_internal::DownloadModelsWriteCallback(
			        first_payload.data(), 1, first_payload.size(), &write_context);
			const auto second_result =
			    howdy::native::download_models_internal::DownloadModelsWriteCallback(
			        second_payload.data(), 1, second_payload.size(), &write_context);
			ok &= Expect(first_result == first_payload.size(),
			             "write callback accepts data within cumulative limit");
			ok &= Expect(second_result == 0,
			             "write callback rejects data exceeding cumulative limit");
			ok &= Expect(write_context.bytes_written == first_payload.size(),
			             "write callback tracks only committed bytes");
			ok &= Expect(ReadFile(limited_staged->path) == "xy",
			             "write callback leaves rejected chunk unwritten");
			howdy::native::CleanupStagedFile(*limited_staged);
		});

		const auto successful_install_destination = temp_root / "successful-install.onnx";
		auto       successful_install =
		    howdy::native::PrepareStagedFile(successful_install_destination, ".howdy-download-");
		ok &= Expect(successful_install.has_value(), "prepare staged file for successful install");
		WithPresent(successful_install, [&] -> void {
			const auto staged_path = successful_install->path;
			ok &= Expect(howdy::native::WriteAllToFd(successful_install->fd.Get(), "model"),
			             "download-like write leaves staged fd open before install");
			ok &= Expect(successful_install->fd.Get() >= 0,
			             "download-like staged fd remains open until install");
			ok &= Expect(CommitIsDurable(howdy::native::InstallStagedFile(
			                 *successful_install, successful_install_destination)),
			             "install staged file fsyncs and closes download-like open fd");
			ok &= Expect(successful_install->fd.Get() < 0, "successful install closes staged fd");
			ok &= Expect(successful_install->path.empty(), "successful install clears staged path");
			ok &= Expect(fs::exists(successful_install_destination, ec) && !ec,
			             "successful install creates destination");
			ok &= Expect(ReadFile(successful_install_destination) == "model",
			             "successful install preserves staged content");
			ok &= Expect(!fs::exists(staged_path, ec) && !ec,
			             "successful install removes staged temp path");
		});

		const auto uncertain_install_destination = temp_root / "uncertain-install.onnx";
		auto       uncertain_install =
		    howdy::native::PrepareStagedFile(uncertain_install_destination, ".howdy-download-");
		ok &= Expect(uncertain_install.has_value(), "prepare staged file for uncertain install");
		WithPresent(uncertain_install, [&] -> void {
			ok &= Expect(howdy::native::WriteAllToFd(uncertain_install->fd.Get(), "model"),
			             "write uncertain staged install content");
			const auto install_result = howdy::native::InstallStagedFile(
			    *uncertain_install, uncertain_install_destination, FailParentSync);
			ok &= Expect(install_result ==
			                 howdy::native::AtomicFileCommitResult::kCommittedSyncFailed,
			             "failed parent sync reports committed install with uncertain durability");
			ok &= Expect(ReadFile(uncertain_install_destination) == "model",
			             "failed parent sync leaves committed destination visible");
			ok &= Expect(uncertain_install->path.empty(),
			             "failed parent sync clears consumed staged path");
		});

		const auto failed_install_destination = temp_root / "failed-install.onnx";
		auto       failed_install =
		    howdy::native::PrepareStagedFile(failed_install_destination, ".howdy-download-");
		ok &= Expect(failed_install.has_value(), "prepare staged file for failed install");
		WithPresent(failed_install, [&] -> void {
			const auto staged_path = failed_install->path;
			ok &= Expect(!CommitIsDurable(howdy::native::InstallStagedFile(
			                 *failed_install, temp_root / "missing" / "model")),
			             "staged install fails for missing destination parent");
			ok &= Expect(!fs::exists(staged_path, ec) && !ec,
			             "failed staged install removes staged file");
		});

		const auto atomic_new_path = temp_root / "atomic" / "new-file";
		ok &= Expect(CommitIsDurable(howdy::native::WriteAtomicFile(atomic_new_path, "new content",
		                                                            S_IRUSR | S_IWUSR)),
		             "atomic writer creates new file");
		ok &= Expect(ReadFile(atomic_new_path) == "new content",
		             "atomic writer preserves new content");
		struct stat atomic_new_stat{};
		ok &= Expect(stat(atomic_new_path.c_str(), &atomic_new_stat) == 0,
		             "stat atomic writer new file");
		ok &= Expect(S_ISREG(atomic_new_stat.st_mode), "atomic writer new target is regular file");
		ok &= Expect((atomic_new_stat.st_mode & 07777) == 0600,
		             "atomic writer applies requested new-file mode");
		ok &= Expect(CountFilesWithPrefix(atomic_new_path.parent_path(), ".howdy-atomic-") == 0,
		             "atomic writer removes staged file after new-file install");

		const auto atomic_existing_path = temp_root / "atomic-existing";
		ok &= Expect(WriteFile(atomic_existing_path, "initial"),
		             "create existing atomic writer target");
		ok &= Expect(chmod(atomic_existing_path.c_str(), 0640) == 0,
		             "set existing atomic writer target mode");
		ok &= Expect(CommitIsDurable(
		                 howdy::native::WriteAtomicFile(atomic_existing_path, "replacement", 0600)),
		             "atomic writer replaces existing file");
		ok &= Expect(ReadFile(atomic_existing_path) == "replacement",
		             "atomic writer preserves replacement content");
		struct stat atomic_existing_stat{};
		ok &= Expect(stat(atomic_existing_path.c_str(), &atomic_existing_stat) == 0,
		             "stat replaced atomic writer file");
		ok &= Expect((atomic_existing_stat.st_mode & 07777) == 0640,
		             "atomic writer preserves existing-file mode");
		ok &=
		    Expect(CountFilesWithPrefix(atomic_existing_path.parent_path(), ".howdy-atomic-") == 0,
		           "atomic writer removes staged file after replacement");

		const auto no_replace_path = temp_root / "atomic-no-replace";
		ok &=
		    Expect(WriteFile(no_replace_path, "initial"), "create no-replace atomic writer target");
		auto no_replace_staged =
		    howdy::native::PrepareStagedFile(no_replace_path, ".howdy-no-replace-");
		ok &= Expect(no_replace_staged.has_value(), "prepare no-replace staged file");
		WithPresent(no_replace_staged, [&] -> void {
			const auto staged_path = no_replace_staged->path;
			ok &= Expect(howdy::native::WriteAllToFd(no_replace_staged->fd.Get(), "replacement"),
			             "write no-replace staged file");
			const auto result = howdy::native::InstallStagedFile(
			    *no_replace_staged, no_replace_path, howdy::native::SyncParentDirectory,
			    howdy::native::AtomicFileInstallPolicy::kNoReplaceExisting);
			ok &= Expect(result == howdy::native::AtomicFileCommitResult::kDestinationExists,
			             "no-replace install reports existing destination");
			ok &= Expect(!howdy::native::AtomicFileMayHaveCommitted(result),
			             "no-replace collision is not possibly committed");
			ok &= Expect(ReadFile(no_replace_path) == "initial",
			             "no-replace install preserves existing content");
			ok &= Expect(!fs::exists(staged_path, ec) && !ec,
			             "no-replace collision removes staged file");
		});

		const auto default_mode_path = temp_root / "default-mode-existing";
		ok &= Expect(WriteFile(default_mode_path, "old"), "create default-mode policy target");
		ok &= Expect(chmod(default_mode_path.c_str(), 0600) == 0,
		             "set default-mode policy target mode");
		auto default_mode_staged = howdy::native::PrepareStagedFile(
		    default_mode_path, ".howdy-default-mode-", S_IRUSR | S_IWUSR | S_IRGRP,
		    howdy::native::StagedFileMetadataPolicy::kUseDefaultMode);
		ok &= Expect(default_mode_staged.has_value(), "prepare default-mode policy staged file");
		WithPresent(default_mode_staged, [&] -> void {
			ok &= Expect(howdy::native::WriteAllToFd(default_mode_staged->fd.Get(), "new"),
			             "write default-mode policy staged file");
			ok &= Expect(CommitIsDurable(howdy::native::InstallStagedFile(*default_mode_staged,
			                                                              default_mode_path)),
			             "install default-mode policy staged file");
			ok &= Expect(ReadFile(default_mode_path) == "new",
			             "default-mode policy installs replacement content");
			struct stat default_mode_stat{};
			ok &= Expect(stat(default_mode_path.c_str(), &default_mode_stat) == 0,
			             "stat default-mode policy target");
			ok &= Expect((default_mode_stat.st_mode & 07777) == 0640,
			             "default-mode policy applies requested mode to existing file");
			ok &= Expect(
			    CountFilesWithPrefix(default_mode_path.parent_path(), ".howdy-default-mode-") == 0,
			    "default-mode policy removes staged file after replacement");
		});

		const auto atomic_directory_target = temp_root / "atomic-directory-target";
		fs::create_directory(atomic_directory_target, ec);
		ok &= Expect(!ec, "create directory atomic writer target");
		ok &= Expect(!CommitIsDurable(howdy::native::WriteAtomicFile(atomic_directory_target,
		                                                             "must not replace")),
		             "atomic writer rejects directory target");
		ok &= Expect(fs::is_directory(atomic_directory_target, ec) && !ec,
		             "atomic writer leaves directory target unchanged");
		ok &= Expect(
		    CountFilesWithPrefix(atomic_directory_target.parent_path(), ".howdy-atomic-") == 0,
		    "atomic writer creates no staged file for directory target");

		const auto atomic_blocked_parent = temp_root / "atomic-blocked-parent";
		ok &= Expect(WriteFile(atomic_blocked_parent, "blocking content"),
		             "create file blocking atomic writer parent");
		const auto atomic_blocked_path = atomic_blocked_parent / "child" / "file";
		ok &= Expect(
		    !CommitIsDurable(howdy::native::WriteAtomicFile(atomic_blocked_path, "must not write")),
		    "atomic writer rejects blocked parent path");
		ok &= Expect(!fs::exists(atomic_blocked_parent / "child", ec) && !ec,
		             "atomic writer creates no child under blocked parent");
		ok &= Expect(ReadFile(atomic_blocked_parent) == "blocking content",
		             "atomic writer preserves blocking file content");

		const auto atomic_write_failure_path = temp_root / "atomic-write-failure";
		ok &= Expect(WriteFile(atomic_write_failure_path, "original content"),
		             "create atomic writer write-failure target");
		struct rlimit original_file_size_limit{};
		const bool have_file_size_limit = getrlimit(RLIMIT_FSIZE, &original_file_size_limit) == 0;
		ok &= Expect(have_file_size_limit, "read file-size resource limit");
		const auto previous_sigxfsz_handler = std::signal(SIGXFSZ, SIG_IGN);
		const bool have_sigxfsz_handler     = previous_sigxfsz_handler != SIG_ERR;
		ok &= Expect(have_sigxfsz_handler, "ignore file-size signal for failed atomic write");
		bool atomic_write_failed = false;
		if (have_file_size_limit && have_sigxfsz_handler) {
			auto zero_file_size_limit     = original_file_size_limit;
			zero_file_size_limit.rlim_cur = 0;
			const bool limited_file_size  = setrlimit(RLIMIT_FSIZE, &zero_file_size_limit) == 0;
			ok &= Expect(limited_file_size, "limit file size for failed atomic write");
			if (limited_file_size) {
				atomic_write_failed = !CommitIsDurable(
				    howdy::native::WriteAtomicFile(atomic_write_failure_path, "replacement"));
				ok &= Expect(setrlimit(RLIMIT_FSIZE, &original_file_size_limit) == 0,
				             "restore file-size resource limit");
			}
		}
		if (have_sigxfsz_handler) {
			ok &= Expect(std::signal(SIGXFSZ, previous_sigxfsz_handler) != SIG_ERR,
			             "restore file-size signal handler");
		}
		ok &= Expect(atomic_write_failed, "atomic writer reports staged content write failure");
		ok &= Expect(ReadFile(atomic_write_failure_path) == "original content",
		             "failed atomic write preserves existing target");
		ok &= Expect(
		    CountFilesWithPrefix(atomic_write_failure_path.parent_path(), ".howdy-atomic-") == 0,
		    "failed atomic write removes staged file");
		return ok;
	}

}  // namespace howdy::test::download_models
