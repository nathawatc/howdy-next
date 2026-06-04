#ifndef HOWDY_SRC_PAM_ENTER_DEVICE_HH
#define HOWDY_SRC_PAM_ENTER_DEVICE_HH

#include <memory>

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

class EnterDevice {
    std::unique_ptr<struct libevdev, decltype(&libevdev_free)>                  raw_device;
    std::unique_ptr<struct libevdev_uinput, decltype(&libevdev_uinput_destroy)> raw_uinput_device;

public:
    EnterDevice();
    void send_enter_press();
    ~EnterDevice()                         = default;
    EnterDevice(EnterDevice &&)            = default;
    EnterDevice &operator=(EnterDevice &&) = default;
};

#endif  // HOWDY_SRC_PAM_ENTER_DEVICE_HH
