// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string.h>
#include <threads.h>
#include <unistd.h>

#include <ddk/debug.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform-bus.h>
#include <ddk/protocol/platform-device.h>

#include <hid/descriptor.h>

#include <fbl/algorithm.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/unique_ptr.h>

#include <zircon/syscalls.h>
#include <zircon/syscalls/port.h>

#include "hid-buttons.h"

// clang-format off
// zx_port_packet::type.
#define PORT_TYPE_SHUTDOWN                 0x01
#define PORT_TYPE_INTERRUPT_VOLUME_UP      0x02
#define PORT_TYPE_INTERRUPT_VOLUME_DOWN    0x03
#define PORT_TYPE_INTERRUPT_VOLUME_UP_DOWN 0x04
// clang-format on

namespace buttons {

int HidButtonsDevice::Thread() {
    while (1) {
        zx_port_packet_t packet;
        zx_status_t status = zx_port_wait(port_handle_, ZX_TIME_INFINITE, &packet);
        zxlogf(TRACE, "%s msg received on port key %lu\n", __FUNCTION__, packet.key);
        if (status != ZX_OK) {
            zxlogf(ERROR, "%s port wait failed: %d\n", __FUNCTION__, status);
            return thrd_error;
        }

        switch (packet.key) {
        case PORT_TYPE_SHUTDOWN:
            zxlogf(INFO, "%s shutting down\n", __FUNCTION__);
            return thrd_success;
        case PORT_TYPE_INTERRUPT_VOLUME_UP:
            __FALLTHROUGH;
        case PORT_TYPE_INTERRUPT_VOLUME_DOWN:
            {
                buttons_input_rpt_t input_rpt;
                input_rpt.rpt_id = BUTTONS_RPT_ID_INPUT;
                switch (packet.key) {
                case PORT_TYPE_INTERRUPT_VOLUME_UP:
                    keys_[kGpioVolumeUp].irq.ack();
                    input_rpt.volume = 1;
                    break;
                case PORT_TYPE_INTERRUPT_VOLUME_DOWN:
                    keys_[kGpioVolumeDown].irq.ack();
                    input_rpt.volume = 3; // -1 for 2 bits.
                    break;
                }
                input_rpt.padding = 0;
                fbl::AutoLock lock(&proxy_lock_);
                proxy_.IoQueue(&input_rpt, sizeof(buttons_input_rpt_t));
                // If report could not be filled, we do not ioqueue.
            }
            break;
        case PORT_TYPE_INTERRUPT_VOLUME_UP_DOWN:
            keys_[kGpioVolumeUpDown].irq.ack();
            zxlogf(INFO, "FDR (up and down buttons) pressed\n");
            break;
        }
    }
    return thrd_success;
}

zx_status_t HidButtonsDevice::HidbusStart(const hidbus_ifc_t* ifc) {
    fbl::AutoLock lock(&proxy_lock_);
    if (proxy_.is_valid()) {
        return ZX_ERR_ALREADY_BOUND;
    } else {
        proxy_ = ddk::HidbusIfcProxy(ifc);
    }
    return ZX_OK;
}

zx_status_t HidButtonsDevice::HidbusQuery(uint32_t options, hid_info_t* info) {
    if (!info) {
        return ZX_ERR_INVALID_ARGS;
    }
    info->dev_num = 0;
    info->device_class = HID_DEVICE_CLASS_OTHER;
    info->boot_device = false;

    return ZX_OK;
}

void HidButtonsDevice::HidbusStop() {
    fbl::AutoLock lock(&proxy_lock_);
    proxy_.clear();
}

zx_status_t HidButtonsDevice::HidbusGetDescriptor(uint8_t desc_type, void** data, size_t* len) {
    const uint8_t* desc_ptr;
    uint8_t* buf;
    if (!len || !data) {
        return ZX_ERR_INVALID_ARGS;
    }
    *len = get_buttons_report_desc(&desc_ptr);
    fbl::AllocChecker ac;
    buf = new (&ac) uint8_t[*len];
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    memcpy(buf, desc_ptr, *len);
    *data = buf;
    return ZX_OK;
}

zx_status_t HidButtonsDevice::HidbusGetReport(uint8_t rpt_type, uint8_t rpt_id, void* data,
                                              size_t len, size_t* out_len) {
    if (!data || !out_len) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (rpt_id != BUTTONS_RPT_ID_INPUT) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    *out_len = sizeof(buttons_input_rpt_t);
    if (*out_len > len) {
        return ZX_ERR_BUFFER_TOO_SMALL;
    }

    buttons_input_rpt_t input_rpt;
    input_rpt.rpt_id = BUTTONS_RPT_ID_INPUT;
    input_rpt.volume = 0;
    input_rpt.padding = 0;
    uint8_t val;
    gpio_read(&keys_[kGpioVolumeUp].gpio, &val);
    if (!val) { // Up button is pressed down.
        input_rpt.volume = 1;
    }
    gpio_read(&keys_[kGpioVolumeDown].gpio, &val);
    if (!val) {               // Down button is pressed down.
        input_rpt.volume = 3; // -1 for 2 bits.
    }
    auto out = static_cast<buttons_input_rpt_t*>(data);
    *out = input_rpt;

    return ZX_OK;
}

zx_status_t HidButtonsDevice::HidbusSetReport(uint8_t rpt_type, uint8_t rpt_id, const void* data,
                                              size_t len) {
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t HidButtonsDevice::HidbusGetIdle(uint8_t rpt_id, uint8_t* duration) {
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t HidButtonsDevice::HidbusSetIdle(uint8_t rpt_id, uint8_t duration) {
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t HidButtonsDevice::HidbusGetProtocol(uint8_t* protocol) {
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t HidButtonsDevice::HidbusSetProtocol(uint8_t protocol) {
    return ZX_OK;
}

zx_status_t HidButtonsDevice::ConfigureGpio(uint32_t idx, uint64_t int_port) {
    zx_status_t status;
    platform_device_protocol_t pdev;
    status = device_get_protocol(parent_, ZX_PROTOCOL_PLATFORM_DEV, &pdev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s device_get_protocol failed %d\n", __FUNCTION__, status);
        return status;
    }
    status = pdev_get_protocol(&pdev, ZX_PROTOCOL_GPIO, idx, &keys_[idx].gpio);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s pdev_get_protocol failed %d\n", __FUNCTION__, status);
        return ZX_ERR_NOT_SUPPORTED;
    }
    status = gpio_config_in(&keys_[idx].gpio, GPIO_NO_PULL);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s gpio_config_in failed %d\n", __FUNCTION__, status);
        return ZX_ERR_NOT_SUPPORTED;
    }
    status = gpio_get_interrupt(&keys_[idx].gpio, ZX_INTERRUPT_MODE_EDGE_LOW,
                                keys_[idx].irq.reset_and_get_address());
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s gpio_get_interrupt failed %d\n", __FUNCTION__, status);
        return status;
    }
    status = keys_[idx].irq.bind(port_handle_, int_port, 0);
    if (status != ZX_OK) {
        zxlogf(ERROR, "HidButtonsDevice::Bind: zx_interrupt_bind failed: %d\n", status);
        return status;
    }
    return ZX_OK;
}

zx_status_t HidButtonsDevice::Bind() {
    zx_status_t status;

    status = zx_port_create(ZX_PORT_BIND_TO_INTERRUPT, &port_handle_);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s port_create failed: %d\n", __FUNCTION__, status);
        return status;
    }

    platform_device_protocol_t pdev;
    status = device_get_protocol(parent_, ZX_PROTOCOL_PLATFORM_DEV, &pdev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s device_get_protocol failed %d\n", __FUNCTION__, status);
        return status;
    }

    pdev_device_info_t pdev_info;
    status = pdev_get_device_info(&pdev, &pdev_info);
    if (pdev_info.gpio_count != kNumberOfRequiredGpios) {
        zxlogf(ERROR, "%s Incorrect number of GPIOs configured: %u (%u needed)\n", __FUNCTION__,
               pdev_info.gpio_count, kNumberOfRequiredGpios);
        return ZX_ERR_NOT_SUPPORTED;
    }
    keys_ = fbl::make_unique<GpioKeys[]>(pdev_info.gpio_count);
    // TODO(andresoportus): use fbl::make_unique_checked once array can be used.

    status = ConfigureGpio(kGpioVolumeUp, PORT_TYPE_INTERRUPT_VOLUME_UP);
    if (status != ZX_OK) {
        return status;
    }
    status = ConfigureGpio(kGpioVolumeDown, PORT_TYPE_INTERRUPT_VOLUME_DOWN);
    if (status != ZX_OK) {
        return status;
    }
    status = ConfigureGpio(kGpioVolumeUpDown, PORT_TYPE_INTERRUPT_VOLUME_UP_DOWN);
    if (status != ZX_OK) {
        return status;
    }

    int rc = thrd_create_with_name(&thread_,
                                   [](void* arg) -> int {
                                       return reinterpret_cast<HidButtonsDevice*>(arg)->Thread();
                                   },
                                   this,
                                   "hid-buttons-thread");
    if (rc != thrd_success) {
        return ZX_ERR_INTERNAL;
    }

    status = DdkAdd("hid-buttons");
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s DdkAdd failed: %d\n", __FUNCTION__, status);
        ShutDown();
        return status;
    }

    return ZX_OK;
}

void HidButtonsDevice::ShutDown() {
    zx_port_packet packet = {PORT_TYPE_SHUTDOWN, ZX_PKT_TYPE_USER, ZX_OK, {}};
    zx_status_t status = zx_port_queue(port_handle_, &packet);
    ZX_ASSERT(status == ZX_OK);
    thrd_join(thread_, NULL);
    keys_[kGpioVolumeUp].irq.destroy();
    keys_[kGpioVolumeDown].irq.destroy();
    keys_[kGpioVolumeUpDown].irq.destroy();
    {
        fbl::AutoLock lock(&proxy_lock_);
        proxy_.clear();
    }
}

void HidButtonsDevice::DdkUnbind() {
    ShutDown();
    DdkRemove();
}

void HidButtonsDevice::DdkRelease() {
    delete this;
}

} // namespace buttons

extern "C" zx_status_t hid_buttons_bind(void* ctx, zx_device_t* parent) {
    fbl::AllocChecker ac;
    auto dev = fbl::make_unique_checked<buttons::HidButtonsDevice>(&ac, parent);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    auto status = dev->Bind();
    if (status == ZX_OK) {
        // devmgr is now in charge of the memory for dev.
        __UNUSED auto ptr = dev.release();
    }
    return status;
}
