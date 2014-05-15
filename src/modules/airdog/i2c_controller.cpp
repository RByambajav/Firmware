#include "i2c_controller.h"

#include <nuttx/config.h>
#include <nuttx/clock.h> 

#include <drivers/drv_hrt.h>

#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>

#include <systemlib/perf_counter.h>
#include <systemlib/err.h>
#include <systemlib/systemlib.h>

#include <uORB/topics/i2c_button_status.h>

#define I2C_BUTTON_COUNT 9

I2C_CONTROLLER::I2C_CONTROLLER(int bus, int addr) :
	I2C("buttons", "/dev/buttons", bus, addr, 100000),
    _running(false),
    _should_run(false),
    _listening_interval(USEC2TICK(100000))
{
	memset(&_work, 0, sizeof(_work));
}

I2C_CONTROLLER::~I2C_CONTROLLER()
{
    delete _buttons;
}

int
I2C_CONTROLLER::init()
{
	int ret;
	ret = I2C::init();

	if (ret != OK) {
		return ret;
	}

    init_buttons();

    ret = init_led();
    if (ret != OK) {
		return ret;
	}

    _led_update_in_progress = false;

	return OK;
}

int
I2C_CONTROLLER::probe()
{
    uint8_t response[1] = {0};
    uint8_t requests[1] = {0x00};

    int ret = transfer(&requests[0], sizeof(requests), nullptr, 0);
    ret = transfer(nullptr, 0, response, sizeof(response)); 
    warnx("Result code is: %d, response: %x", ret, response[0]);

	return ret;
}

void
I2C_CONTROLLER::init_buttons()
{
    _buttons = new i2c_button_s[I2C_BUTTON_COUNT];
    int pin_id = 0;
    for (int i = 0; i < I2C_BUTTON_COUNT; i++) {
        if (pin_id == 8) {
            pin_id = 0;
        }
        _buttons[i] = { i, pin_id, false, false, 0};
        pin_id++;
    }

    _cmd_pub = -1;
}

int
I2C_CONTROLLER::init_led()
{
    uint8_t requests[2] = {0x07, 0x3F};
    int ret = transfer(requests, sizeof(requests), nullptr, 0);
    warnx("init_led() code is: %d", ret);
    return ret;
}

void
I2C_CONTROLLER::listener_trampoline(void *arg)
{
	I2C_CONTROLLER *rgbl = reinterpret_cast<I2C_CONTROLLER *>(arg);

	rgbl->listen();
}

void
I2C_CONTROLLER::listen()
{
    if (!_should_run) {
		_running = false;
		return;
	}

    if (!_led_update_in_progress) {
        int mask_r1 = get_buttons_state_from_r1();

        for (int i = 0; i < I2C_BUTTON_COUNT - 1; i++) {
            check_button(&_buttons[i], mask_r1);
        }
        int mask_r2 = get_buttons_state_from_r2();
        for (int i = I2C_BUTTON_COUNT - 1; i < I2C_BUTTON_COUNT; i++) {
            check_button(&_buttons[i], mask_r2);
        }
    }

    work_queue(LPWORK, &_work, (worker_t)&I2C_CONTROLLER::listener_trampoline, this, _listening_interval);
}

void
I2C_CONTROLLER::start_listening()
{
    _should_run = true;
    _running = true;
    work_queue(LPWORK, &_work, (worker_t)&I2C_CONTROLLER::listener_trampoline, this, _listening_interval);
}

int
I2C_CONTROLLER::get_buttons_state_from_r1()
{
    uint8_t response[1] = {0};
    uint8_t requests[1] = {0x00};

    int ret = transfer(&requests[0], sizeof(requests), nullptr, 0);
    ret = transfer(nullptr, 0, response, sizeof(response)); 

    if (ret == OK) {
        return response[0];
    }
    return -1;
}

int
I2C_CONTROLLER::get_buttons_state_from_r2()
{
    uint8_t response[1] = {0};
    uint8_t requests[2] = {0x00, 0x00};

    int ret = transfer(requests, sizeof(requests), nullptr, 0);
    ret = transfer(nullptr, 0, response, sizeof(response)); 

    if (ret == OK) {
        return response[0];
    }
    return -1;
}

void
I2C_CONTROLLER::check_button(struct i2c_button_s *button, int gpio_values)
{
	if (!(gpio_values & (1 << button->register_pin))) {
		uint64_t now = hrt_absolute_time();
		float elapsed = (now - button->time_pressed) / 10000;

		if (button->button_pressed == false){
			button->button_pressed = true;
			button->time_pressed = now;
		} else if (button->button_pressed & !button->long_press & elapsed > 150) {
			warnx("long press button %d", button->pin + 1);
			button_pressed(button, true);
			button->long_press = true;
		}
	} else {
		if (button->button_pressed == true){
			if (!button->long_press)
			{
				warnx("short press button %d", button->pin + 1);
				button_pressed(button, false);
			}
			button->button_pressed = false;
			button->long_press = false;
		}
	}
};

void
I2C_CONTROLLER::button_pressed(struct i2c_button_s *button, bool long_press)
{
    if (_cmd_pub < 0) {
        _cmd_pub = orb_advertise(ORB_ID(i2c_button_status), button);
    } else {
        warnx("press published");
        orb_publish(ORB_ID(i2c_button_status), _cmd_pub, button);
    }
}

void
I2C_CONTROLLER::set_indicators_state(uint8_t state)
{
    uint8_t response[1] = {0};

    uint8_t requests[2] = {0x03, state};
    int ret = transfer(requests, sizeof(requests), nullptr, 0);
}