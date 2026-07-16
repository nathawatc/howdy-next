#include "prompt/enter_device.hpp"

#ifdef HOWDY_PAM_TESTING
#	include "support/prompt_coordinator_testing.hpp"
#endif

#include <stdexcept>

#ifdef HOWDY_PAM_TESTING
namespace {
	struct EnterDeviceTestState {
		bool                                    fail_construction = false;
		bool                                    fail_send         = false;
		howdy::pam::testing::EnterPressCallback callback          = nullptr;
		void                                   *context           = nullptr;
		int                                     constructions     = 0;
		int                                     presses           = 0;
	};

	EnterDeviceTestState g_enter_device_test_state;
}  // namespace
#endif

EnterDevice::EnterDevice()
    : raw_device(nullptr, &libevdev_free)
    , raw_uinput_device(nullptr, &libevdev_uinput_destroy) {
#ifdef HOWDY_PAM_TESTING
	++g_enter_device_test_state.constructions;
	if (g_enter_device_test_state.fail_construction) {
		throw std::runtime_error("Failed to create uinput device");
	}
	return;
#endif
	auto *raw_libevdev = libevdev_new();
	if (raw_libevdev == nullptr) {
		throw std::runtime_error("Failed to create libevdev object");
	}
	raw_device.reset(raw_libevdev);

	libevdev_set_name(raw_device.get(), "Howdy virtual keyboard");

	libevdev_enable_event_type(raw_device.get(), EV_KEY);
	libevdev_enable_event_code(raw_device.get(), EV_KEY, KEY_ENTER, nullptr);

	libevdev_uinput *raw_uinput    = nullptr;
	const int        create_result = libevdev_uinput_create_from_device(
	    raw_device.get(), LIBEVDEV_UINPUT_OPEN_MANAGED, &raw_uinput);
	if (create_result != 0) {
		throw std::runtime_error("Failed to create uinput device");
	}
	raw_uinput_device.reset(raw_uinput);
}

void EnterDevice::send_enter_press() {
#ifdef HOWDY_PAM_TESTING
	++g_enter_device_test_state.presses;
	if (g_enter_device_test_state.fail_send) {
		throw std::runtime_error("Failed to send Enter keypress");
	}
	if (g_enter_device_test_state.callback != nullptr) {
		g_enter_device_test_state.callback(g_enter_device_test_state.context);
	}
	return;
#endif
	if (libevdev_uinput_write_event(raw_uinput_device.get(), EV_KEY, KEY_ENTER, 1) != 0 ||
	    libevdev_uinput_write_event(raw_uinput_device.get(), EV_SYN, SYN_REPORT, 0) != 0 ||
	    libevdev_uinput_write_event(raw_uinput_device.get(), EV_KEY, KEY_ENTER, 0) != 0 ||
	    libevdev_uinput_write_event(raw_uinput_device.get(), EV_SYN, SYN_REPORT, 0) != 0) {
		throw std::runtime_error("Failed to send Enter keypress");
	}
}

#ifdef HOWDY_PAM_TESTING
namespace howdy::pam::testing {
	void configure_enter_device(bool fail_construction, bool fail_send, EnterPressCallback callback,
	                            void *context) {
		g_enter_device_test_state = {
		    .fail_construction = fail_construction,
		    .fail_send         = fail_send,
		    .callback          = callback,
		    .context           = context,
		};
	}

	void reset_enter_device() {
		g_enter_device_test_state = {};
	}

	auto enter_device_construction_count() -> int {
		return g_enter_device_test_state.constructions;
	}

	auto enter_press_count() -> int {
		return g_enter_device_test_state.presses;
	}
}  // namespace howdy::pam::testing
#endif
