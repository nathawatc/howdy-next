#include "prompt/prompt_submitter.hpp"

#include "prompt/internal_fd.hpp"

#include <fcntl.h>
#include <memory>
#include <stdexcept>

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

namespace {
	class UinputPromptSubmitter final : public howdy::pam::PromptSubmitter {
	public:
		UinputPromptSubmitter()
		    : raw_device_(nullptr, &libevdev_free)
		    , raw_uinput_device_(nullptr, &libevdev_uinput_destroy) {
			auto *raw_libevdev = libevdev_new();
			if (raw_libevdev == nullptr) {
				throw std::runtime_error("Failed to create libevdev object");
			}
			raw_device_.reset(raw_libevdev);

			libevdev_set_name(raw_device_.get(), "Howdy virtual keyboard");

			libevdev_enable_event_type(raw_device_.get(), EV_KEY);
			libevdev_enable_event_code(raw_device_.get(), EV_KEY, KEY_ENTER, nullptr);
			uinput_fd_ = howdy::pam::detail::normalize_internal_fd(
			    howdy::pam::detail::ScopedFd(open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC)));
			if (!uinput_fd_.valid()) {
				throw std::runtime_error("Failed to open uinput device");
			}

			libevdev_uinput *raw_uinput    = nullptr;
			const int        create_result = libevdev_uinput_create_from_device(
			    raw_device_.get(), uinput_fd_.get(), &raw_uinput);
			if (create_result != 0) {
				throw std::runtime_error("Failed to create uinput device");
			}
			raw_uinput_device_.reset(raw_uinput);
		}

		void submit_prompt() override {
			if (libevdev_uinput_write_event(raw_uinput_device_.get(), EV_KEY, KEY_ENTER, 1) != 0 ||
			    libevdev_uinput_write_event(raw_uinput_device_.get(), EV_SYN, SYN_REPORT, 0) != 0 ||
			    libevdev_uinput_write_event(raw_uinput_device_.get(), EV_KEY, KEY_ENTER, 0) != 0 ||
			    libevdev_uinput_write_event(raw_uinput_device_.get(), EV_SYN, SYN_REPORT, 0) != 0) {
				throw std::runtime_error("Failed to send Enter keypress");
			}
		}

	private:
		std::unique_ptr<struct libevdev, decltype(&libevdev_free)> raw_device_;
		howdy::pam::detail::ScopedFd                               uinput_fd_;
		std::unique_ptr<struct libevdev_uinput, decltype(&libevdev_uinput_destroy)>
		    raw_uinput_device_;
	};
}  // namespace

namespace howdy::pam {
	auto create_uinput_prompt_submitter() -> std::unique_ptr<PromptSubmitter> {
		return std::make_unique<UinputPromptSubmitter>();
	}
}  // namespace howdy::pam
