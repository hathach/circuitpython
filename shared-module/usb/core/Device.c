/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "shared-bindings/usb/core/Device.h"

#include "tusb_config.h"

#include "lib/tinyusb/src/host/usbh.h"
#include "py/runtime.h"
#include "shared/runtime/interrupt_char.h"
#include "shared-bindings/usb/core/__init__.h"
#include "shared-module/usb/utf16le.h"
#include "supervisor/shared/tick.h"

bool common_hal_usb_core_device_construct(usb_core_device_obj_t *self, uint8_t device_number) {
    if (device_number == 0 || device_number > CFG_TUH_DEVICE_MAX + CFG_TUH_HUB) {
        return false;
    }
    if (!tuh_ready(device_number)) {
        return false;
    }
    self->device_number = device_number;
    return true;
}

uint16_t common_hal_usb_core_device_get_idVendor(usb_core_device_obj_t *self) {
    uint16_t vid;
    uint16_t pid;
    tuh_vid_pid_get(self->device_number, &vid, &pid);
    return vid;
}

uint16_t common_hal_usb_core_device_get_idProduct(usb_core_device_obj_t *self) {
    uint16_t vid;
    uint16_t pid;
    tuh_vid_pid_get(self->device_number, &vid, &pid);
    return pid;
}

STATIC xfer_result_t _get_string_result;
STATIC void _transfer_done_cb(tuh_xfer_t *xfer) {
    // Store the result so we stop waiting for the transfer.
    _get_string_result = xfer->result;
}

STATIC bool _wait_for_callback(void) {
    while (!mp_hal_is_interrupted() &&
           _get_string_result == 0xff) {
        // The background tasks include TinyUSB which will call the function
        // we provided above. In other words, the callback isn't in an interrupt.
        RUN_BACKGROUND_TASKS;
    }
    return _get_string_result == XFER_RESULT_SUCCESS;
}

STATIC mp_obj_t _get_string(const uint16_t *temp_buf) {
    size_t utf16_len = ((temp_buf[0] & 0xff) - 2) / sizeof(uint16_t);
    if (utf16_len == 0) {
        return mp_const_none;
    }
    return utf16le_to_string(temp_buf + 1, utf16_len);
}

mp_obj_t common_hal_usb_core_device_get_serial_number(usb_core_device_obj_t *self) {
    _get_string_result = 0xff;
    uint16_t temp_buf[127];
    if (!tuh_descriptor_get_serial_string(self->device_number, 0, temp_buf, MP_ARRAY_SIZE(temp_buf), _transfer_done_cb, 0) ||
        !_wait_for_callback()) {
        return mp_const_none;
    }
    return _get_string(temp_buf);
}

mp_obj_t common_hal_usb_core_device_get_product(usb_core_device_obj_t *self) {
    _get_string_result = 0xff;
    uint16_t temp_buf[127];
    if (!tuh_descriptor_get_product_string(self->device_number, 0, temp_buf, MP_ARRAY_SIZE(temp_buf), _transfer_done_cb, 0) ||
        !_wait_for_callback()) {
        return mp_const_none;
    }
    return _get_string(temp_buf);
}

mp_obj_t common_hal_usb_core_device_get_manufacturer(usb_core_device_obj_t *self) {
    _get_string_result = 0xff;
    uint16_t temp_buf[127];
    if (!tuh_descriptor_get_manufacturer_string(self->device_number, 0, temp_buf, MP_ARRAY_SIZE(temp_buf), _transfer_done_cb, 0) ||
        !_wait_for_callback()) {
        return mp_const_none;
    }
    return _get_string(temp_buf);
}

void common_hal_usb_core_device_set_configuration(usb_core_device_obj_t *self, mp_int_t configuration) {
    if (configuration == 0x100) {
        tusb_desc_configuration_t desc;
        if (!tuh_descriptor_get_configuration(self->device_number, 0, &desc, sizeof(desc), _transfer_done_cb, 0) ||
            !_wait_for_callback()) {
            return;
        }
        configuration = desc.bConfigurationValue;
    }
    tuh_configuration_set(self->device_number, configuration, _transfer_done_cb, 0);
    _wait_for_callback();
}

xfer_result_t xfer_result;
STATIC void _xfer_complete_cb(tuh_xfer_t *xfer) {
    xfer_result = xfer->result;
}

STATIC size_t _xfer(tuh_xfer_t *xfer, mp_int_t timeout) {
    xfer_result = XFER_RESULT_STALLED;
    xfer->complete_cb = _xfer_complete_cb;
    if (!tuh_edpt_xfer(xfer)) {
        mp_raise_usb_core_USBError(NULL);
    }
    uint32_t start_time = supervisor_ticks_ms32();
    while (supervisor_ticks_ms32() - start_time < (uint32_t)timeout &&
           !mp_hal_is_interrupted() &&
           xfer_result == XFER_RESULT_STALLED) {
        // The background tasks include TinyUSB which will call the function
        // we provided above. In other words, the callback isn't in an interrupt.
        RUN_BACKGROUND_TASKS;
    }
    if (xfer_result == XFER_RESULT_STALLED) {
        mp_raise_usb_core_USBTimeoutError();
    }
    if (xfer_result == XFER_RESULT_SUCCESS) {
        return xfer->actual_len;
    }

    return 0;
}

STATIC bool _open_endpoint(usb_core_device_obj_t *self, mp_int_t endpoint) {
    bool endpoint_open = false;
    for (size_t i = 0; i < sizeof(self->open_endpoints); i++) {
        if (self->open_endpoints[i] == endpoint) {
            endpoint_open = true;
        }
    }
    if (endpoint_open) {
        return true;
    }

    // Fetch the full configuration descriptor and search for the endpoint's descriptor.
    uint8_t desc_buf[128];
    if (!tuh_descriptor_get_configuration(self->device_number, self->configuration_index, &desc_buf, sizeof(desc_buf), _transfer_done_cb, 0) ||
        !_wait_for_callback()) {
        return false;
    }
    tusb_desc_configuration_t *desc_cfg = (tusb_desc_configuration_t *)desc_buf;

    uint8_t const *desc_end = ((uint8_t const *)desc_cfg) + tu_le16toh(desc_cfg->wTotalLength);
    uint8_t const *p_desc = tu_desc_next(desc_cfg);

    // parse each interfaces
    while (p_desc < desc_end) {
        console_uart_printf("usb desc %p %d\r\n", p_desc, tu_desc_type(p_desc));
        if (TUSB_DESC_ENDPOINT == tu_desc_type(p_desc)) {
            tusb_desc_endpoint_t const *desc_ep = (tusb_desc_endpoint_t const *)p_desc;
            if (desc_ep->bEndpointAddress == endpoint) {
                break;
            }
        }

        p_desc = tu_desc_next(p_desc);
    }
    if (p_desc >= desc_end) {
        return false;
    }
    tusb_desc_endpoint_t const *desc_ep = (tusb_desc_endpoint_t const *)p_desc;

    return tuh_edpt_open(self->device_number, desc_ep);
}

mp_int_t common_hal_usb_core_device_write(usb_core_device_obj_t *self, mp_int_t endpoint, const uint8_t *buffer, mp_int_t len, mp_int_t timeout) {
    if (!_open_endpoint(self, endpoint)) {
        mp_raise_usb_core_USBError(NULL);
    }
    tuh_xfer_t xfer;
    xfer.daddr = self->device_number;
    xfer.ep_addr = endpoint;
    xfer.buffer = (uint8_t *)buffer;
    xfer.buflen = len;
    return _xfer(&xfer, timeout);
}

mp_int_t common_hal_usb_core_device_read(usb_core_device_obj_t *self, mp_int_t endpoint, uint8_t *buffer, mp_int_t len, mp_int_t timeout) {
    if (!_open_endpoint(self, endpoint)) {
        mp_raise_usb_core_USBError(NULL);
    }
    tuh_xfer_t xfer;
    xfer.daddr = self->device_number;
    xfer.ep_addr = endpoint;
    xfer.buffer = buffer;
    xfer.buflen = len;
    return _xfer(&xfer, timeout);
}

xfer_result_t control_result;
STATIC void _control_complete_cb(tuh_xfer_t *xfer) {
    control_result = xfer->result;
}

mp_int_t common_hal_usb_core_device_ctrl_transfer(usb_core_device_obj_t *self,
    mp_int_t bmRequestType, mp_int_t bRequest,
    mp_int_t wValue, mp_int_t wIndex,
    uint8_t *buffer, mp_int_t len, mp_int_t timeout) {
    // Timeout is in ms.

    tusb_control_request_t request = {
        .bmRequestType = bmRequestType,
        .bRequest = bRequest,
        .wValue = wValue,
        .wIndex = wIndex,
        .wLength = len
    };
    tuh_xfer_t xfer = {
        .daddr = self->device_number,
        .ep_addr = 0,
        .setup = &request,
        .buffer = buffer,
        .complete_cb = _control_complete_cb,
    };

    control_result = XFER_RESULT_STALLED;

    if (!tuh_control_xfer(&xfer)) {
        mp_raise_usb_core_USBError(NULL);
    }
    uint32_t start_time = supervisor_ticks_ms32();
    while (supervisor_ticks_ms32() - start_time < (uint32_t)timeout &&
           !mp_hal_is_interrupted() &&
           control_result == XFER_RESULT_STALLED) {
        // The background tasks include TinyUSB which will call the function
        // we provided above. In other words, the callback isn't in an interrupt.
        RUN_BACKGROUND_TASKS;
    }
    if (control_result == XFER_RESULT_STALLED) {
        mp_raise_usb_core_USBTimeoutError();
    }
    if (control_result == XFER_RESULT_SUCCESS) {
        return len;
    }

    return 0;
}

bool common_hal_usb_core_device_is_kernel_driver_active(usb_core_device_obj_t *self, mp_int_t interface) {
    // TODO: Implement this when CP natively uses a keyboard.
    return false;
}

void common_hal_usb_core_device_detach_kernel_driver(usb_core_device_obj_t *self, mp_int_t interface) {
    // TODO: Implement this when CP natively uses a keyboard.
}

void common_hal_usb_core_device_attach_kernel_driver(usb_core_device_obj_t *self, mp_int_t interface) {
    // TODO: Implement this when CP natively uses a keyboard.
}
