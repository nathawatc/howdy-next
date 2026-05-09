#include "enter_device.hh"

#include <cstring>
#include <memory>
#include <stdexcept>

EnterDevice::EnterDevice()
    : raw_device(libevdev_new(), &libevdev_free),
      raw_uinput_device(nullptr, &libevdev_uinput_destroy) {
  auto *dev_ptr = raw_device.get();

  libevdev_set_name(dev_ptr, "enter device");
  libevdev_enable_event_type(dev_ptr, EV_KEY);
  libevdev_enable_event_code(dev_ptr, EV_KEY, KEY_ENTER, nullptr);

  int init_err = 0;
  struct libevdev_uinput *uinput_dev = nullptr;

  init_err = libevdev_uinput_create_from_device(dev_ptr, LIBEVDEV_UINPUT_OPEN_MANAGED, &uinput_dev);
  if (init_err != 0) {
    throw std::runtime_error(std::string("Failed to create device: ") + strerror(-init_err));
  }

  raw_uinput_device.reset(uinput_dev);
};

void EnterDevice::send_enter_press() {
  auto *uinput_dev = raw_uinput_device.get();

  int write_err = 0;
  write_err = libevdev_uinput_write_event(uinput_dev, EV_KEY, KEY_ENTER, 1);
  if (write_err != 0) {
    throw std::runtime_error(std::string("Failed to write event: ") + strerror(-write_err));
  }

  write_err = libevdev_uinput_write_event(uinput_dev, EV_KEY, KEY_ENTER, 0);
  if (write_err != 0) {
    throw std::runtime_error(std::string("Failed to write event: ") + strerror(-write_err));
  }

  write_err = libevdev_uinput_write_event(uinput_dev, EV_SYN, SYN_REPORT, 0);
  if (write_err != 0) {
    throw std::runtime_error(std::string("Failed to write event: ") + strerror(-write_err));
  }
}
