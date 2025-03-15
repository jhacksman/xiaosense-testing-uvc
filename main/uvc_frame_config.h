/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "stdint.h"
#include "usb_device_uvc.h"

/**
 * @brief UVC Streaming Frame Interval Type Descriptor
 */
typedef struct {
    uint8_t bLength;         /*!< Size of this descriptor, in bytes: 6+2*n */
    uint8_t bDescriptorType; /*!< CS_INTERFACE descriptor type: 0x24 */
    uint8_t bDescriptorSubType; /*!< VS_FRAME_UNCOMPRESSED */
    uint8_t bFrameIntervalType; /*!< 0 for continuous, n for discrete intervals */
    uint32_t *dwFrameInterval; /*!< Available frame intervals */
} __attribute__((packed)) uvc_frame_interval_t;

/**
 * @brief UVC Streaming Frame Type Descriptor
 */
typedef struct {
    uint16_t width;      /*!< Width of frame */
    uint16_t height;     /*!< Height of frame */
    uint8_t rate;        /*!< Frame rate in fps */
    uint32_t interval;   /*!< Frame interval in 100ns units */
} uvc_frame_info_t;

/* Frame configuration for default MJPEG format */
static const uvc_frame_info_t UVC_FRAMES_INFO[][4] = {
    {
        /* Format: UVC_FORMAT_MJPEG */
        {640, 480, 30, 333333}, /* VGA 30fps - default for XIAO ESP32-S3 Sense */
        {320, 240, 30, 333333}, /* QVGA 30fps */
        {480, 320, 30, 333333}, /* HVGA 30fps */
        {1280, 720, 15, 666666}, /* HD 15fps */
    }
};

#define UVC_CONFIG_FORMAT_MJPEG_INDEX 0
