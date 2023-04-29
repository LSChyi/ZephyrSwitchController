/*
 * Copyright (c) 2016-2018 Intel Corporation.
 * Copyright (c) 2018-2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <init.h>
#include <drivers/gpio.h>
#include <drivers/uart.h>

#include <usb/usb_device.h>
#include <usb/class/usb_hid.h>

#define LOG_LEVEL LOG_LEVEL_INF
LOG_MODULE_REGISTER(main);

/* change this to any other UART peripheral if desired */
#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)

#define MSG_SIZE 7

/* queue to store up to 10 messages (aligned to 4-byte boundary) */
K_MSGQ_DEFINE(uart_msgq, MSG_SIZE, 100, 4);

static const struct device *uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

/* receive buffer used in UART ISR callback */
static char rx_buf[MSG_SIZE];
static int rx_buf_pos = 0;

static bool configured;
static const struct device *hdev;
static K_SEM_DEFINE(hid_sem, 1, 1);

struct joystick_report {
	uint16_t button;
	uint8_t hat;
	uint8_t lx;
	uint8_t ly;
	uint8_t rx;
	uint8_t ry;
    uint8_t vendor_specific;
};

static const uint8_t hid_report_desc[] = {
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_GAMEPAD),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
        HID_LOGICAL_MIN8(0x00),
        HID_LOGICAL_MAX8(0x01),
        0x35, 0x00, // Physical Minimum (0)
        0x45, 0x01, // Physical Maximum (1)
        HID_REPORT_SIZE(1),
        HID_REPORT_COUNT(16),
        HID_USAGE_PAGE(HID_USAGE_GEN_BUTTON),
        HID_USAGE_MIN8(1),
        HID_USAGE_MAX8(0x10),
        HID_INPUT(0x02), // Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
        HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
        HID_LOGICAL_MAX8(0x07),
        0x46, 0x3B, 0x01,  //   Physical Maximum (315)
        HID_REPORT_SIZE(4),
        HID_REPORT_COUNT(1),
        0x65, 0x14,        //   Unit (System: English Rotation, Length: Centimeter)
        0x09, 0x39,        //   Usage (Hat switch)
        0x81, 0x42,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State)
        0x65, 0x00,        //   Unit (None)
        HID_REPORT_COUNT(1),
        0x81, 0x01,        //   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
        HID_LOGICAL_MAX16(0xFF, 0x00),  //   Logical Maximum (255)
        0x46, 0xFF, 0x00,  //   Physical Maximum (255)
        0x09, 0x30,        //   Usage (X)
        0x09, 0x31,        //   Usage (Y)
        0x09, 0x32,        //   Usage (Z)
        0x09, 0x35,        //   Usage (Rz)
        HID_REPORT_SIZE(8),
        HID_REPORT_COUNT(4),
        0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
        0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
        0x09, 0x20,        //   Usage (0x20)
        HID_REPORT_COUNT(1),
        0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
        0x0A, 0x21, 0x26,  //   Usage (0x2621)
        HID_REPORT_COUNT(8),
        0x91, 0x02,        //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
	HID_END_COLLECTION,
};

static void int_in_ready_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
}

/*
 * On Idle callback is available here as an example even if actual use is
 * very limited. In contrast to report_event_handler(),
 * report value is not incremented here.
 */
static void on_idle_cb(const struct device *dev, uint16_t report_id)
{
	LOG_DBG("On idle callback");
}

static void protocol_cb(const struct device *dev, uint8_t protocol)
{
	LOG_INF("New protocol: %s", protocol == HID_PROTOCOL_BOOT ?
		"boot" : "report");
}

static const struct hid_ops ops = {
	.int_in_ready = int_in_ready_cb,
	.on_idle = on_idle_cb,
	.protocol_change = protocol_cb,
};

static void status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
	switch (status) {
	case USB_DC_RESET:
		configured = false;
		break;
	case USB_DC_CONFIGURED:
		if (!configured) {
			int_in_ready_cb(hdev);
			configured = true;
		}
		break;
	case USB_DC_SOF:
		break;
	default:
		LOG_DBG("status %u unhandled", status);
		break;
	}
}

void serial_cb(const struct device *dev, void *user_data)
{
	if (!uart_irq_update(uart_dev)) {
		return;
	}

	while (uart_irq_rx_ready(uart_dev)) {
		uart_fifo_read(uart_dev, &rx_buf[rx_buf_pos], 1);
		rx_buf_pos = (rx_buf_pos + 1) % MSG_SIZE;

		if (rx_buf_pos == 0) {
			k_msgq_put(&uart_msgq, &rx_buf, K_NO_WAIT);
		}
	}
}

void print_uart(char *buf)
{
	int msg_len = strlen(buf);

	for (int i = 0; i < msg_len; i++) {
		uart_poll_out(uart_dev, buf[i]);
	}
}

void main(void)
{
	int ret;

	LOG_INF("Starting application");

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not found!");
		return;
	}

	/* configure interrupt and callback to receive data */
	uart_irq_callback_user_data_set(uart_dev, serial_cb, NULL);
	uart_irq_rx_enable(uart_dev);

	ret = usb_enable(status_cb);
	if (ret != 0) {
		LOG_ERR("Failed to enable USB");
		return;
	}

	static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
	if (!device_is_ready(led.port)) {
		LOG_ERR("led gpio port is not ready\n");
		return;
	}
	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("Failed to configure led as output\n");
		return;
	}

	int wrote;
	// Add one more byte and set it as string end when debugging.
	char tx_buf[MSG_SIZE + 1];
	tx_buf[MSG_SIZE] = '\0';
	while (k_msgq_get(&uart_msgq, &tx_buf, K_FOREVER) == 0) {
		gpio_pin_toggle_dt(&led);
		k_sem_take(&hid_sem, K_MSEC(30));
		ret = hid_int_ep_write(hdev, (uint8_t *)tx_buf, sizeof(struct joystick_report),
				       &wrote);
		if (ret != 0) {
			LOG_ERR("Failed to write hid event");
			k_sem_give(&hid_sem);
		}
	}
}

static int composite_pre_init(const struct device *dev)
{
	hdev = device_get_binding("HID_0");
	if (hdev == NULL) {
		LOG_ERR("Cannot get USB HID Device");
		return -ENODEV;
	}

	LOG_INF("HID Device: dev %p", hdev);

	usb_hid_register_device(hdev, hid_report_desc, sizeof(hid_report_desc),
				&ops);

	return usb_hid_init(hdev);
}

SYS_INIT(composite_pre_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
