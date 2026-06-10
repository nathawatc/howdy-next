#include "enter_device.hpp"

#include <stdexcept>

EnterDevice::EnterDevice()
    : raw_device(nullptr, &libevdev_free)
    , raw_uinput_device(nullptr, &libevdev_uinput_destroy) {
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
	if (libevdev_uinput_write_event(raw_uinput_device.get(), EV_KEY, KEY_ENTER, 1) != 0 ||
	    libevdev_uinput_write_event(raw_uinput_device.get(), EV_SYN, SYN_REPORT, 0) != 0 ||
	    libevdev_uinput_write_event(raw_uinput_device.get(), EV_KEY, KEY_ENTER, 0) != 0 ||
	    libevdev_uinput_write_event(raw_uinput_device.get(), EV_SYN, SYN_REPORT, 0) != 0) {
		throw std::runtime_error("Failed to send Enter keypress");
	}
}
