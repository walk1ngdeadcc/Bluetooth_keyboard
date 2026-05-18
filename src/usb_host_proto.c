#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usbd_msg.h>

#include "rgb_led.h"
#include "usb_host_proto.h"

#define USB_HOST_PROTO_NODE DT_NODELABEL(cdc_acm_uart0)

#define USB_HOST_PROTO_MAGIC_0 0x55U
#define USB_HOST_PROTO_MAGIC_1 0xAAU
#define USB_HOST_PROTO_FRAME_HEADER_SIZE 3U
#define USB_HOST_PROTO_MAX_PAYLOAD_LEN 64U
#define USB_HOST_PROTO_RX_RING_SIZE 256U
#define USB_HOST_PROTO_MAX_FRAME_SIZE \
	(USB_HOST_PROTO_FRAME_HEADER_SIZE + USB_HOST_PROTO_MAX_PAYLOAD_LEN)

#define USB_HOST_PROTO_VERSION 1U
#define USB_HOST_PROTO_VENDOR_ID 0x1915U
#define USB_HOST_PROTO_PRODUCT_ID 0x52F0U
#define USB_HOST_PROTO_FIRMWARE_MAJOR 0U
#define USB_HOST_PROTO_FIRMWARE_MINOR 0U
#define USB_HOST_PROTO_CAP_THEME_RGB 0x04U

#define PROTO_FIELD_HELLO_REQ 1U
#define PROTO_FIELD_HELLO_RSP 2U
#define PROTO_FIELD_THEME_RGB 7U
#define PROTO_FIELD_RESPONSE 8U
#define PROTO_FIELD_MSG_ID 10U
#define PROTO_FIELD_REPLY_TO 11U

#define PROTO_HELLO_PROTOCOL_VERSION_FIELD 1U

#define PROTO_THEME_RED_FIELD 1U
#define PROTO_THEME_GREEN_FIELD 2U
#define PROTO_THEME_BLUE_FIELD 3U

#define PROTO_RESPONSE_ERROR_CODE_FIELD 1U

#define PB_WIRE_VARINT 0U
#define PB_WIRE_FIXED64 1U
#define PB_WIRE_LEN 2U
#define PB_WIRE_FIXED32 5U

BUILD_ASSERT(DT_NODE_HAS_STATUS(USB_HOST_PROTO_NODE, okay),
	     "cdc_acm_uart0 must be enabled for host protocol");

enum proto_session_state {
	PROTO_SESSION_DOWN = 0,
	PROTO_SESSION_WAIT_HELLO,
	PROTO_SESSION_ACTIVE,
};

enum proto_response_code {
	PROTO_RESPONSE_CODE_OK = 0,
	PROTO_RESPONSE_CODE_UNKNOWN_TYPE = 1,
	PROTO_RESPONSE_CODE_INVALID_LENGTH = 2,
	PROTO_RESPONSE_CODE_INVALID_PARAM = 3,
	PROTO_RESPONSE_CODE_NOT_READY = 4,
};

enum proto_parser_state {
	PROTO_PARSER_SYNC_0 = 0,
	PROTO_PARSER_SYNC_1,
	PROTO_PARSER_LEN,
	PROTO_PARSER_PAYLOAD,
};

enum proto_message_type {
	PROTO_MSG_NONE = 0,
	PROTO_MSG_HELLO_REQ,
	PROTO_MSG_THEME_RGB,
};

struct proto_hello_req {
	uint32_t protocol_version;
	bool has_protocol_version;
};

struct proto_theme_rgb {
	uint32_t red;
	uint32_t green;
	uint32_t blue;
};

struct proto_message {
	enum proto_message_type type;
	uint32_t msg_id;
	bool has_msg_id;
	struct proto_hello_req hello_req;
	struct proto_theme_rgb theme_rgb;
};

static const struct device *const usb_host_proto_uart = DEVICE_DT_GET(USB_HOST_PROTO_NODE);

static uint8_t usb_host_proto_rx_ring_buffer[USB_HOST_PROTO_RX_RING_SIZE];
static struct ring_buf usb_host_proto_rx_ring;
static struct k_work usb_host_proto_rx_work;

static enum proto_session_state usb_host_proto_session = PROTO_SESSION_DOWN;
static enum proto_parser_state usb_host_proto_parser_state = PROTO_PARSER_SYNC_0;
static uint8_t usb_host_proto_payload_len;
static uint8_t usb_host_proto_payload_pos;
static uint8_t usb_host_proto_payload[USB_HOST_PROTO_MAX_PAYLOAD_LEN];
static bool usb_host_proto_initialized;
static bool usb_host_proto_usb_enabled;
static bool usb_host_proto_dtr_asserted;
static bool usb_host_proto_rx_throttled;

static void usb_host_proto_reset_parser(void)
{
	usb_host_proto_parser_state = PROTO_PARSER_SYNC_0;
	usb_host_proto_payload_len = 0U;
	usb_host_proto_payload_pos = 0U;
}

static void usb_host_proto_set_session(enum proto_session_state state)
{
	usb_host_proto_session = state;
	ring_buf_reset(&usb_host_proto_rx_ring);
	usb_host_proto_reset_parser();
}

static int pb_read_varint(const uint8_t *buf, size_t len, size_t *offset, uint64_t *value)
{
	uint64_t result = 0U;
	uint8_t shift = 0U;

	while (*offset < len) {
		uint8_t byte = buf[*offset];

		(*offset)++;
		result |= ((uint64_t)(byte & 0x7FU)) << shift;

		if ((byte & 0x80U) == 0U) {
			*value = result;
			return 0;
		}

		shift += 7U;
		if (shift >= 64U) {
			return -EINVAL;
		}
	}

	return -EMSGSIZE;
}

static int pb_skip_field(const uint8_t *buf, size_t len, size_t *offset, uint8_t wire_type)
{
	uint64_t tmp;

	switch (wire_type) {
	case PB_WIRE_VARINT:
		return pb_read_varint(buf, len, offset, &tmp);

	case PB_WIRE_FIXED64:
		if ((len - *offset) < 8U) {
			return -EMSGSIZE;
		}
		*offset += 8U;
		return 0;

	case PB_WIRE_LEN:
		if (pb_read_varint(buf, len, offset, &tmp) != 0) {
			return -EMSGSIZE;
		}
		if (tmp > (len - *offset)) {
			return -EMSGSIZE;
		}
		*offset += (size_t)tmp;
		return 0;

	case PB_WIRE_FIXED32:
		if ((len - *offset) < 4U) {
			return -EMSGSIZE;
		}
		*offset += 4U;
		return 0;

	default:
		return -ENOTSUP;
	}
}

static int proto_parse_hello_req(const uint8_t *buf, size_t len, struct proto_hello_req *hello_req)
{
	size_t offset = 0U;

	memset(hello_req, 0, sizeof(*hello_req));

	while (offset < len) {
		uint64_t tag;
		uint32_t field_number;
		uint8_t wire_type;

		if (pb_read_varint(buf, len, &offset, &tag) != 0) {
			return -EMSGSIZE;
		}

		field_number = (uint32_t)(tag >> 3);
		wire_type = (uint8_t)(tag & 0x07U);

		if ((field_number == PROTO_HELLO_PROTOCOL_VERSION_FIELD) &&
		    (wire_type == PB_WIRE_VARINT)) {
			uint64_t value;

			if (pb_read_varint(buf, len, &offset, &value) != 0) {
				return -EMSGSIZE;
			}

			hello_req->protocol_version = (uint32_t)value;
			hello_req->has_protocol_version = true;
			continue;
		}

		if (pb_skip_field(buf, len, &offset, wire_type) != 0) {
			return -EMSGSIZE;
		}
	}

	return 0;
}

static int proto_parse_theme_rgb(const uint8_t *buf, size_t len, struct proto_theme_rgb *theme_rgb)
{
	size_t offset = 0U;

	memset(theme_rgb, 0, sizeof(*theme_rgb));

	while (offset < len) {
		uint64_t tag;
		uint32_t field_number;
		uint8_t wire_type;
		uint64_t value;

		if (pb_read_varint(buf, len, &offset, &tag) != 0) {
			return -EMSGSIZE;
		}

		field_number = (uint32_t)(tag >> 3);
		wire_type = (uint8_t)(tag & 0x07U);

		if (wire_type != PB_WIRE_VARINT) {
			if (pb_skip_field(buf, len, &offset, wire_type) != 0) {
				return -EMSGSIZE;
			}
			continue;
		}

		if (pb_read_varint(buf, len, &offset, &value) != 0) {
			return -EMSGSIZE;
		}

		switch (field_number) {
		case PROTO_THEME_RED_FIELD:
			theme_rgb->red = (uint32_t)value;
			break;
		case PROTO_THEME_GREEN_FIELD:
			theme_rgb->green = (uint32_t)value;
			break;
		case PROTO_THEME_BLUE_FIELD:
			theme_rgb->blue = (uint32_t)value;
			break;
		default:
			break;
		}
	}

	return 0;
}

static int proto_parse_device_message(const uint8_t *buf, size_t len, struct proto_message *msg)
{
	size_t offset = 0U;

	memset(msg, 0, sizeof(*msg));

	while (offset < len) {
		uint64_t tag;
		uint32_t field_number;
		uint8_t wire_type;

		if (pb_read_varint(buf, len, &offset, &tag) != 0) {
			return -EMSGSIZE;
		}

		field_number = (uint32_t)(tag >> 3);
		wire_type = (uint8_t)(tag & 0x07U);

		if ((field_number == PROTO_FIELD_MSG_ID) && (wire_type == PB_WIRE_VARINT)) {
			uint64_t value;

			if (pb_read_varint(buf, len, &offset, &value) != 0) {
				return -EMSGSIZE;
			}

			msg->msg_id = (uint32_t)value;
			msg->has_msg_id = true;
			continue;
		}

		if ((field_number == PROTO_FIELD_HELLO_REQ) && (wire_type == PB_WIRE_LEN)) {
			uint64_t body_len;

			if (pb_read_varint(buf, len, &offset, &body_len) != 0) {
				return -EMSGSIZE;
			}
			if (body_len > (len - offset)) {
				return -EMSGSIZE;
			}
			if ((msg->type != PROTO_MSG_NONE) && (msg->type != PROTO_MSG_HELLO_REQ)) {
				return -EINVAL;
			}

			msg->type = PROTO_MSG_HELLO_REQ;
			if (proto_parse_hello_req(&buf[offset], (size_t)body_len, &msg->hello_req) != 0) {
				return -EINVAL;
			}
			offset += (size_t)body_len;
			continue;
		}

		if ((field_number == PROTO_FIELD_THEME_RGB) && (wire_type == PB_WIRE_LEN)) {
			uint64_t body_len;

			if (pb_read_varint(buf, len, &offset, &body_len) != 0) {
				return -EMSGSIZE;
			}
			if (body_len > (len - offset)) {
				return -EMSGSIZE;
			}
			if ((msg->type != PROTO_MSG_NONE) && (msg->type != PROTO_MSG_THEME_RGB)) {
				return -EINVAL;
			}

			msg->type = PROTO_MSG_THEME_RGB;
			if (proto_parse_theme_rgb(&buf[offset], (size_t)body_len, &msg->theme_rgb) != 0) {
				return -EINVAL;
			}
			offset += (size_t)body_len;
			continue;
		}

		if (pb_skip_field(buf, len, &offset, wire_type) != 0) {
			return -EMSGSIZE;
		}
	}

	return 0;
}

static int pb_write_varint(uint8_t *buf, size_t size, size_t *offset, uint64_t value)
{
	do {
		if (*offset >= size) {
			return -ENOSPC;
		}

		buf[*offset] = value & 0x7FU;
		value >>= 7;
		if (value != 0U) {
			buf[*offset] |= 0x80U;
		}

		(*offset)++;
	} while (value != 0U);

	return 0;
}

static int pb_write_tag(uint8_t *buf, size_t size, size_t *offset,
			uint32_t field_number, uint8_t wire_type)
{
	uint64_t tag = ((uint64_t)field_number << 3) | wire_type;

	return pb_write_varint(buf, size, offset, tag);
}

static int pb_write_field_varint(uint8_t *buf, size_t size, size_t *offset,
				 uint32_t field_number, uint64_t value)
{
	if (pb_write_tag(buf, size, offset, field_number, PB_WIRE_VARINT) != 0) {
		return -ENOSPC;
	}

	return pb_write_varint(buf, size, offset, value);
}

static int pb_write_field_len(uint8_t *buf, size_t size, size_t *offset,
			      uint32_t field_number, const uint8_t *data, size_t data_len)
{
	if (pb_write_tag(buf, size, offset, field_number, PB_WIRE_LEN) != 0) {
		return -ENOSPC;
	}
	if (pb_write_varint(buf, size, offset, data_len) != 0) {
		return -ENOSPC;
	}
	if ((size - *offset) < data_len) {
		return -ENOSPC;
	}

	memcpy(&buf[*offset], data, data_len);
	*offset += data_len;
	return 0;
}

static int usb_host_proto_send_payload(const uint8_t *payload, size_t payload_len)
{
	uint8_t frame[USB_HOST_PROTO_MAX_FRAME_SIZE];

	if (!usb_host_proto_usb_enabled || !usb_host_proto_dtr_asserted) {
		return -EACCES;
	}
	if (payload_len > USB_HOST_PROTO_MAX_PAYLOAD_LEN) {
		return -EMSGSIZE;
	}

	frame[0] = USB_HOST_PROTO_MAGIC_0;
	frame[1] = USB_HOST_PROTO_MAGIC_1;
	frame[2] = (uint8_t)payload_len;
	memcpy(&frame[USB_HOST_PROTO_FRAME_HEADER_SIZE], payload, payload_len);

	for (size_t i = 0; i < (USB_HOST_PROTO_FRAME_HEADER_SIZE + payload_len); i++) {
		uart_poll_out(usb_host_proto_uart, frame[i]);
	}

	return 0;
}

static void usb_host_proto_send_response(uint32_t reply_to,
					 enum proto_response_code error_code)
{
	uint8_t body[8];
	uint8_t payload[USB_HOST_PROTO_MAX_PAYLOAD_LEN];
	size_t body_len = 0U;
	size_t payload_len = 0U;

	if (pb_write_field_varint(body, sizeof(body), &body_len,
				  PROTO_RESPONSE_ERROR_CODE_FIELD,
				  error_code) != 0) {
		return;
	}

	if (reply_to != 0U) {
		if (pb_write_field_varint(payload, sizeof(payload), &payload_len,
					  PROTO_FIELD_REPLY_TO,
					  reply_to) != 0) {
			return;
		}
	}

	if (pb_write_field_len(payload, sizeof(payload), &payload_len,
			       PROTO_FIELD_RESPONSE, body, body_len) != 0) {
		return;
	}

	if (usb_host_proto_send_payload(payload, payload_len) != 0) {
		printk("usb host proto response send failed\n");
	}
}

static void usb_host_proto_send_hello_rsp(uint32_t reply_to)
{
	uint8_t body[32];
	uint8_t payload[USB_HOST_PROTO_MAX_PAYLOAD_LEN];
	size_t body_len = 0U;
	size_t payload_len = 0U;

	if (pb_write_field_varint(body, sizeof(body), &body_len,
				  PROTO_HELLO_PROTOCOL_VERSION_FIELD,
				  USB_HOST_PROTO_VERSION) != 0 ||
	    pb_write_field_varint(body, sizeof(body), &body_len, 2U,
				  USB_HOST_PROTO_VENDOR_ID) != 0 ||
	    pb_write_field_varint(body, sizeof(body), &body_len, 3U,
				  USB_HOST_PROTO_PRODUCT_ID) != 0 ||
	    pb_write_field_varint(body, sizeof(body), &body_len, 4U,
				  USB_HOST_PROTO_FIRMWARE_MAJOR) != 0 ||
	    pb_write_field_varint(body, sizeof(body), &body_len, 5U,
				  USB_HOST_PROTO_FIRMWARE_MINOR) != 0 ||
	    pb_write_field_varint(body, sizeof(body), &body_len, 6U,
				  USB_HOST_PROTO_CAP_THEME_RGB) != 0) {
		return;
	}

	if (reply_to != 0U) {
		if (pb_write_field_varint(payload, sizeof(payload), &payload_len,
					  PROTO_FIELD_REPLY_TO,
					  reply_to) != 0) {
			return;
		}
	}

	if (pb_write_field_len(payload, sizeof(payload), &payload_len,
			       PROTO_FIELD_HELLO_RSP, body, body_len) != 0) {
		return;
	}

	if (usb_host_proto_send_payload(payload, payload_len) != 0) {
		printk("usb host proto hello response send failed\n");
	}
}

static void usb_host_proto_process_message(const struct proto_message *msg)
{
	int ret;
	uint32_t reply_to = msg->has_msg_id ? msg->msg_id : 0U;

	if (msg->type == PROTO_MSG_HELLO_REQ) {
		if (!msg->hello_req.has_protocol_version ||
		    (msg->hello_req.protocol_version != USB_HOST_PROTO_VERSION)) {
			usb_host_proto_send_response(reply_to,
						     PROTO_RESPONSE_CODE_INVALID_PARAM);
			return;
		}

		usb_host_proto_session = PROTO_SESSION_ACTIVE;
		usb_host_proto_send_hello_rsp(reply_to);
		printk("usb host proto active\n");
		return;
	}

	if (usb_host_proto_session != PROTO_SESSION_ACTIVE) {
		usb_host_proto_send_response(reply_to, PROTO_RESPONSE_CODE_NOT_READY);
		return;
	}

	if (msg->type == PROTO_MSG_THEME_RGB) {
		if ((msg->theme_rgb.red > 0xFFU) ||
		    (msg->theme_rgb.green > 0xFFU) ||
		    (msg->theme_rgb.blue > 0xFFU)) {
			usb_host_proto_send_response(reply_to,
						     PROTO_RESPONSE_CODE_INVALID_PARAM);
			return;
		}

		ret = rgb_led_set_all((uint8_t)msg->theme_rgb.red,
				      (uint8_t)msg->theme_rgb.green,
				      (uint8_t)msg->theme_rgb.blue);
		if (ret != 0) {
			usb_host_proto_send_response(reply_to,
						     PROTO_RESPONSE_CODE_NOT_READY);
			return;
		}

		usb_host_proto_send_response(reply_to, PROTO_RESPONSE_CODE_OK);
		printk("usb host proto theme rgb: r=%u g=%u b=%u\n",
		       msg->theme_rgb.red, msg->theme_rgb.green, msg->theme_rgb.blue);
		return;
	}

	usb_host_proto_send_response(reply_to, PROTO_RESPONSE_CODE_UNKNOWN_TYPE);
}

static void usb_host_proto_handle_frame(const uint8_t *payload, size_t payload_len)
{
	struct proto_message msg;
	int ret;

	ret = proto_parse_device_message(payload, payload_len, &msg);
	if (ret == -EMSGSIZE) {
		usb_host_proto_send_response(0U, PROTO_RESPONSE_CODE_INVALID_LENGTH);
		return;
	}
	if (ret != 0) {
		usb_host_proto_send_response(0U, PROTO_RESPONSE_CODE_INVALID_PARAM);
		return;
	}

	usb_host_proto_process_message(&msg);
}

static void usb_host_proto_parser_feed(uint8_t byte)
{
	switch (usb_host_proto_parser_state) {
	case PROTO_PARSER_SYNC_0:
		if (byte == USB_HOST_PROTO_MAGIC_0) {
			usb_host_proto_parser_state = PROTO_PARSER_SYNC_1;
		}
		break;

	case PROTO_PARSER_SYNC_1:
		if (byte == USB_HOST_PROTO_MAGIC_1) {
			usb_host_proto_parser_state = PROTO_PARSER_LEN;
		} else if (byte != USB_HOST_PROTO_MAGIC_0) {
			usb_host_proto_parser_state = PROTO_PARSER_SYNC_0;
		}
		break;

	case PROTO_PARSER_LEN:
		if (byte > USB_HOST_PROTO_MAX_PAYLOAD_LEN) {
			usb_host_proto_reset_parser();
			break;
		}

		usb_host_proto_payload_len = byte;
		usb_host_proto_payload_pos = 0U;
		if (usb_host_proto_payload_len == 0U) {
			usb_host_proto_handle_frame(usb_host_proto_payload, 0U);
			usb_host_proto_reset_parser();
		} else {
			usb_host_proto_parser_state = PROTO_PARSER_PAYLOAD;
		}
		break;

	case PROTO_PARSER_PAYLOAD:
		usb_host_proto_payload[usb_host_proto_payload_pos++] = byte;
		if (usb_host_proto_payload_pos >= usb_host_proto_payload_len) {
			usb_host_proto_handle_frame(usb_host_proto_payload,
						    usb_host_proto_payload_len);
			usb_host_proto_reset_parser();
		}
		break;

	default:
		usb_host_proto_reset_parser();
		break;
	}
}

static void usb_host_proto_rx_work_handler(struct k_work *work)
{
	uint8_t buf[32];
	uint32_t len;

	ARG_UNUSED(work);

	while ((len = ring_buf_get(&usb_host_proto_rx_ring, buf, sizeof(buf))) > 0U) {
		for (uint32_t i = 0U; i < len; i++) {
			usb_host_proto_parser_feed(buf[i]);
		}
	}

	if (usb_host_proto_rx_throttled &&
	    (ring_buf_space_get(&usb_host_proto_rx_ring) > 0U)) {
		uart_irq_rx_enable(usb_host_proto_uart);
		usb_host_proto_rx_throttled = false;
	}
}

static void usb_host_proto_uart_irq_handler(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!usb_host_proto_rx_throttled && uart_irq_rx_ready(dev)) {
			uint8_t buf[32];
			size_t free_len;
			int recv_len;
			uint32_t stored_len;

			free_len = MIN((size_t)ring_buf_space_get(&usb_host_proto_rx_ring),
				       sizeof(buf));
			if (free_len == 0U) {
				uart_irq_rx_disable(dev);
				usb_host_proto_rx_throttled = true;
				continue;
			}

			recv_len = uart_fifo_read(dev, buf, free_len);
			if (recv_len <= 0) {
				continue;
			}

			stored_len = ring_buf_put(&usb_host_proto_rx_ring, buf, recv_len);
			if (stored_len < (uint32_t)recv_len) {
				printk("usb host proto drop %u bytes\n",
				       (unsigned int)(recv_len - stored_len));
			}

			if (stored_len > 0U) {
				(void)k_work_submit(&usb_host_proto_rx_work);
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uart_irq_tx_disable(dev);
		}
	}
}

int usb_host_proto_init(void)
{
	int ret;

	if (usb_host_proto_initialized) {
		return 0;
	}

	if (!device_is_ready(usb_host_proto_uart)) {
		return -ENODEV;
	}

	ring_buf_init(&usb_host_proto_rx_ring,
		      sizeof(usb_host_proto_rx_ring_buffer),
		      usb_host_proto_rx_ring_buffer);
	k_work_init(&usb_host_proto_rx_work, usb_host_proto_rx_work_handler);
	usb_host_proto_set_session(PROTO_SESSION_DOWN);

	ret = uart_irq_callback_set(usb_host_proto_uart,
				    usb_host_proto_uart_irq_handler);
	if (ret != 0) {
		return ret;
	}

	uart_irq_rx_disable(usb_host_proto_uart);
	uart_irq_tx_disable(usb_host_proto_uart);

	usb_host_proto_initialized = true;
	printk("usb host proto ready: cdc_acm_uart0\n");
	return 0;
}

void usb_host_proto_handle_usbd_msg(const struct usbd_msg *msg)
{
	uint32_t dtr = 0U;

	if (!usb_host_proto_initialized || (msg == NULL)) {
		return;
	}

	switch (msg->type) {
	case USBD_MSG_VBUS_READY:
		rgb_led_request_restore();
		break;

	case USBD_MSG_CDC_ACM_CONTROL_LINE_STATE:
		if (msg->dev != usb_host_proto_uart) {
			return;
		}

		if (uart_line_ctrl_get(usb_host_proto_uart, UART_LINE_CTRL_DTR, &dtr) != 0) {
			return;
		}

		if ((dtr != 0U) == usb_host_proto_dtr_asserted) {
			return;
		}

		usb_host_proto_dtr_asserted = (dtr != 0U);
		if (usb_host_proto_dtr_asserted && usb_host_proto_usb_enabled) {
			usb_host_proto_set_session(PROTO_SESSION_WAIT_HELLO);
			uart_irq_rx_enable(usb_host_proto_uart);
			printk("usb host proto link ready, wait hello\n");
		} else {
			uart_irq_rx_disable(usb_host_proto_uart);
			usb_host_proto_set_session(PROTO_SESSION_DOWN);
			printk("usb host proto link down\n");
		}
		break;

	case USBD_MSG_VBUS_REMOVED:
		rgb_led_request_restore();
		__fallthrough;
	case USBD_MSG_RESET:
		usb_host_proto_dtr_asserted = false;
		uart_irq_rx_disable(usb_host_proto_uart);
		usb_host_proto_set_session(PROTO_SESSION_DOWN);
		break;

	default:
		break;
	}
}

void usb_host_proto_set_usb_enabled(bool enabled)
{
	usb_host_proto_usb_enabled = enabled;

	if (!enabled) {
		usb_host_proto_dtr_asserted = false;
		uart_irq_rx_disable(usb_host_proto_uart);
		usb_host_proto_set_session(PROTO_SESSION_DOWN);
		return;
	}

	if (usb_host_proto_dtr_asserted) {
		usb_host_proto_set_session(PROTO_SESSION_WAIT_HELLO);
		uart_irq_rx_enable(usb_host_proto_uart);
	}
}
