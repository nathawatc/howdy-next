#ifndef HOWDY_SRC_PAM_ENTER_DEVICE_HH
#define HOWDY_SRC_PAM_ENTER_DEVICE_HH

#include <memory>

class EnterDevice {
public:
	virtual ~EnterDevice() = default;

	virtual void send_enter_press() = 0;
};

auto create_enter_device() -> std::unique_ptr<EnterDevice>;

#endif  // HOWDY_SRC_PAM_ENTER_DEVICE_HH
