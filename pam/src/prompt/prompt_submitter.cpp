#include "prompt/prompt_submitter.hpp"

#include "prompt/internal_fd.hpp"
#include "support/eintr.hpp"
#include "support/error_message.hpp"

#include <array>
#include <cerrno>
#include <format>
#include <memory>
#include <stdexcept>

#include <fcntl.h>

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>

namespace {
	struct InputEventSpec {
		unsigned int type;
		unsigned int code;
		int          value;
	};

	class UinputPromptSubmitter final : public howdy::pam::PromptSubmitter {
	public:
		UinputPromptSubmitter()
		    : raw_uinput_device_(nullptr, &libevdev_uinput_destroy) {
			const std::unique_ptr<struct libevdev, decltype(&libevdev_free)> raw_device(
			    libevdev_new(), &libevdev_free);
			if (raw_device == nullptr) {
				throw std::runtime_error("Failed to create libevdev object");
			}

			libevdev_set_name(raw_device.get(), "Howdy virtual keyboard");

			if (libevdev_enable_event_type(raw_device.get(), EV_KEY) != 0) {
				throw std::runtime_error("Failed to enable EV_KEY on virtual keyboard");
			}
			if (libevdev_enable_event_code(raw_device.get(), EV_KEY, KEY_ENTER, nullptr) != 0) {
				throw std::runtime_error("Failed to enable KEY_ENTER on virtual keyboard");
			}
			uinput_fd_ = howdy::pam::detail::NormalizeInternalFd(
			    howdy::pam::detail::ScopedFd(howdy::native::RetryOnEintr([] -> int {
				    return open(howdy::pam::kUinputDevicePath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
			    })));
			if (!uinput_fd_.Valid()) {
				const int error_number = errno;
				throw std::runtime_error(
				    std::format("Failed to open uinput device: {} ({})",
					            howdy::native::SystemErrorMessage(error_number), error_number));
			}

			libevdev_uinput *raw_uinput = nullptr;
			const int        create_result =
			    libevdev_uinput_create_from_device(raw_device.get(), uinput_fd_.Get(), &raw_uinput);
			if (create_result != 0) {
				const int error_number = create_result < 0 ? -create_result : create_result;
				throw std::runtime_error(
				    std::format("Failed to create uinput device: {} ({})",
					            howdy::native::SystemErrorMessage(error_number), error_number));
			}
			raw_uinput_device_.reset(raw_uinput);
		}

		void SubmitPrompt() override {
			constexpr std::array<InputEventSpec, 4> events{
			    {
			        {.type = EV_KEY, .code = KEY_ENTER, .value = 1},
			        {.type = EV_SYN, .code = SYN_REPORT, .value = 0},
			        {.type = EV_KEY, .code = KEY_ENTER, .value = 0},
			        {.type = EV_SYN, .code = SYN_REPORT, .value = 0},
			    },
			};
			for (const auto &event : events) {
				int write_result = -EINTR;
				while (write_result == -EINTR) {
					write_result = libevdev_uinput_write_event(raw_uinput_device_.get(), event.type,
					                                           event.code, event.value);
				}
				if (write_result != 0) {
					const int error_number = write_result < 0 ? -write_result : write_result;
					throw std::runtime_error(
					    std::format("Failed to send Enter keypress: {} ({})",
						            howdy::native::SystemErrorMessage(error_number), error_number));
				}
			}
		}

	private:
		// Order matters: raw_uinput_device_ must be destroyed before uinput_fd_ closes the device
		// descriptor it references.
		howdy::pam::detail::ScopedFd uinput_fd_;
		std::unique_ptr<struct libevdev_uinput, decltype(&libevdev_uinput_destroy)>
		    raw_uinput_device_;
	};
}  // namespace

namespace howdy::pam {
	auto CreateUinputPromptSubmitter() -> std::unique_ptr<PromptSubmitter> {
		return std::make_unique<UinputPromptSubmitter>();
	}
}  // namespace howdy::pam
