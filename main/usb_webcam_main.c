/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "camera_pin.h"
#include "esp_camera.h"
#include "usb_device_uvc.h"
#include "uvc_frame_config.h"

static const char *TAG = "usb_webcam";

#define CAMERA_XCLK_FREQ           CONFIG_CAMERA_XCLK_FREQ
#define CAMERA_FB_COUNT            2

#if CONFIG_IDF_TARGET_ESP32S3
#define UVC_MAX_FRAMESIZE_SIZE     (75*1024)
#else
#define UVC_MAX_FRAMESIZE_SIZE     (60*1024)
#endif

// Format name mapping for logging
static const char *uvc_format_names[] = {
    "UNKNOWN",
    "MJPEG",
    "YUY2",
    "NV12",
    "GRAY8"
};

typedef struct {
    camera_fb_t *cam_fb_p;
    uvc_fb_t uvc_fb;
} fb_t;

static fb_t s_fb;

// Current negotiated UVC parameters
static struct {
    uvc_format_t format;
    int width;
    int height;
    int frame_rate;
    uint32_t frame_interval; // in 100-ns units
} s_uvc_params = {
    .format = UVC_FORMAT_MJPEG,
    .width = 640,
    .height = 480,
    .frame_rate = 30,
    .frame_interval = 333333
};

static esp_err_t camera_init(uint32_t xclk_freq_hz, pixformat_t pixel_format, framesize_t frame_size, int jpeg_quality, uint8_t fb_count)
{
    static bool inited = false;
    static uint32_t cur_xclk_freq_hz = 0;
    static pixformat_t cur_pixel_format = 0;
    static framesize_t cur_frame_size = 0;
    static int cur_jpeg_quality = 0;
    static uint8_t cur_fb_count = 0;

    if ((inited && cur_xclk_freq_hz == xclk_freq_hz && cur_pixel_format == pixel_format
            && cur_frame_size == frame_size && cur_fb_count == fb_count && cur_jpeg_quality == jpeg_quality)) {
        ESP_LOGD(TAG, "camera already inited");
        return ESP_OK;
    } else if (inited) {
        esp_camera_return_all();
        esp_camera_deinit();
        inited = false;
        ESP_LOGI(TAG, "camera RESTART");
    }

    camera_config_t camera_config = {
        .pin_pwdn = CAMERA_PIN_PWDN,
        .pin_reset = CAMERA_PIN_RESET,
        .pin_xclk = CAMERA_PIN_XCLK,
        .pin_sscb_sda = CAMERA_PIN_SIOD,
        .pin_sscb_scl = CAMERA_PIN_SIOC,

        .pin_d7 = CAMERA_PIN_D7,
        .pin_d6 = CAMERA_PIN_D6,
        .pin_d5 = CAMERA_PIN_D5,
        .pin_d4 = CAMERA_PIN_D4,
        .pin_d3 = CAMERA_PIN_D3,
        .pin_d2 = CAMERA_PIN_D2,
        .pin_d1 = CAMERA_PIN_D1,
        .pin_d0 = CAMERA_PIN_D0,
        .pin_vsync = CAMERA_PIN_VSYNC,
        .pin_href = CAMERA_PIN_HREF,
        .pin_pclk = CAMERA_PIN_PCLK,

        .xclk_freq_hz = xclk_freq_hz,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = pixel_format,
        .frame_size = frame_size,

        .jpeg_quality = jpeg_quality,
        .fb_count = fb_count,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .fb_location = CAMERA_FB_IN_PSRAM
    };

    // initialize the camera sensor
    esp_err_t ret = esp_camera_init(&camera_config);
    if (ret != ESP_OK) {
        return ret;
    }

    // Get the sensor object, and then use some of its functions to adjust the parameters when taking a photo.
    // Note: Do not call functions that set resolution, set picture format and PLL clock,
    // If you need to reset the appeal parameters, please reinitialize the sensor.
    sensor_t *s = esp_camera_sensor_get();
    s->set_vflip(s, 1); // flip it back
    
    // initial sensors are flipped vertically and colors are a bit saturated
    if (s->id.PID == OV3660_PID) {
        s->set_brightness(s, 1); // up the blightness just a bit
        s->set_saturation(s, -2); // lower the saturation
    }

    if (s->id.PID == OV3660_PID || s->id.PID == OV2640_PID) {
        s->set_vflip(s, 1); // flip it back
    } else if (s->id.PID == GC0308_PID) {
        s->set_hmirror(s, 0);
    } else if (s->id.PID == GC032A_PID) {
        s->set_vflip(s, 1);
    }

    // Get the basic information of the sensor.
    camera_sensor_info_t *s_info = esp_camera_sensor_get_info(&(s->id));
    ESP_LOGI(TAG, "Camera sensor: %s (PID: 0x%x)", s_info->name, s->id.PID);

    if (ESP_OK == ret && PIXFORMAT_JPEG == pixel_format && s_info->support_jpeg == true) {
        cur_xclk_freq_hz = xclk_freq_hz;
        cur_pixel_format = pixel_format;
        cur_frame_size = frame_size;
        cur_jpeg_quality = jpeg_quality;
        cur_fb_count = fb_count;
        inited = true;
    } else {
        ESP_LOGE(TAG, "JPEG format is not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ret;
}

static void camera_stop_cb(void *cb_ctx)
{
    (void)cb_ctx;
    ESP_LOGI(TAG, "Camera Stop");
}

static esp_err_t camera_start_cb(uvc_format_t format, int width, int height, int rate, void *cb_ctx)
{
    (void)cb_ctx;
    ESP_LOGI(TAG, "========== UVC Negotiation Parameters ==========");
    ESP_LOGI(TAG, "Format: %s (%d)", format < sizeof(uvc_format_names)/sizeof(uvc_format_names[0]) ? 
                                      uvc_format_names[format] : "UNKNOWN", format);
    ESP_LOGI(TAG, "Resolution: %dx%d", width, height);
    ESP_LOGI(TAG, "Frame Rate: %d fps", rate);
    ESP_LOGI(TAG, "Frame Interval: %d (100ns units)", 10000000 / rate);
    ESP_LOGI(TAG, "================================================");
    
    // Store the negotiated parameters
    s_uvc_params.format = format;
    s_uvc_params.width = width;
    s_uvc_params.height = height;
    s_uvc_params.frame_rate = rate;
    s_uvc_params.frame_interval = 10000000 / rate;
    
    framesize_t frame_size = FRAMESIZE_QVGA;
    int jpeg_quality = 14;

    if (format != UVC_FORMAT_MJPEG) {
        ESP_LOGE(TAG, "Only support MJPEG format");
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Map resolution to camera frame size
    if (width == 320 && height == 240) {
        frame_size = FRAMESIZE_QVGA;
        jpeg_quality = 10;
    } else if (width == 480 && height == 320) {
        frame_size = FRAMESIZE_HVGA;
        jpeg_quality = 10;
    } else if (width == 640 && height == 480) {
        frame_size = FRAMESIZE_VGA;
        jpeg_quality = 12;
    } else if (width == 800 && height == 600) {
        frame_size = FRAMESIZE_SVGA;
        jpeg_quality = 14;
    } else if (width == 1280 && height == 720) {
        frame_size = FRAMESIZE_HD;
        jpeg_quality = 16;
    } else if (width == 1920 && height == 1080) {
        frame_size = FRAMESIZE_FHD;
        jpeg_quality = 16;
    } else {
        ESP_LOGE(TAG, "Unsupported frame size %dx%d", width, height);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Initializing camera with %s format, %dx%d resolution, quality %d", 
             format == UVC_FORMAT_MJPEG ? "MJPEG" : "OTHER", width, height, jpeg_quality);
             
    esp_err_t ret = camera_init(CAMERA_XCLK_FREQ, PIXFORMAT_JPEG, frame_size, jpeg_quality, CAMERA_FB_COUNT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static uvc_fb_t* camera_fb_get_cb(void *cb_ctx)
{
    (void)cb_ctx;
    s_fb.cam_fb_p = esp_camera_fb_get();
    if (!s_fb.cam_fb_p) {
        return NULL;
    }
    s_fb.uvc_fb.buf = s_fb.cam_fb_p->buf;
    s_fb.uvc_fb.len = s_fb.cam_fb_p->len;
    s_fb.uvc_fb.width = s_fb.cam_fb_p->width;
    s_fb.uvc_fb.height = s_fb.cam_fb_p->height;
    s_fb.uvc_fb.format = s_fb.cam_fb_p->format;
    s_fb.uvc_fb.timestamp = s_fb.cam_fb_p->timestamp;

    if (s_fb.uvc_fb.len > UVC_MAX_FRAMESIZE_SIZE) {
        ESP_LOGE(TAG, "Frame size %d is larger than max frame size %d", s_fb.uvc_fb.len, UVC_MAX_FRAMESIZE_SIZE);
        esp_camera_fb_return(s_fb.cam_fb_p);
        return NULL;
    }
    return &s_fb.uvc_fb;
}

static void camera_fb_return_cb(uvc_fb_t *fb, void *cb_ctx)
{
    (void)cb_ctx;
    assert(fb == &s_fb.uvc_fb);
    esp_camera_fb_return(s_fb.cam_fb_p);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Selected Camera Board %s", CAMERA_MODULE_NAME);
    uint8_t *uvc_buffer = (uint8_t *)malloc(UVC_MAX_FRAMESIZE_SIZE);
    if (uvc_buffer == NULL) {
        ESP_LOGE(TAG, "malloc frame buffer fail");
        return;
    }

    uvc_device_config_t config = {
        .uvc_buffer = uvc_buffer,
        .uvc_buffer_size = UVC_MAX_FRAMESIZE_SIZE,
        .start_cb = camera_start_cb,
        .fb_get_cb = camera_fb_get_cb,
        .fb_return_cb = camera_fb_return_cb,
        .stop_cb = camera_stop_cb,
    };

    ESP_LOGI(TAG, "====== UVC Configuration Information ======");
    ESP_LOGI(TAG, "Format List");
    ESP_LOGI(TAG, "\tFormat(1) = %s", "MJPEG");
    
    ESP_LOGI(TAG, "Frame List");
    ESP_LOGI(TAG, "\tFrame(1) = %d * %d @%dfps (interval: %d)",
             UVC_FRAMES_INFO[0][0].width, UVC_FRAMES_INFO[0][0].height,
             UVC_FRAMES_INFO[0][0].rate, UVC_FRAMES_INFO[0][0].interval);
    ESP_LOGI(TAG, "\tFrame(2) = %d * %d @%dfps (interval: %d)",
             UVC_FRAMES_INFO[0][1].width, UVC_FRAMES_INFO[0][1].height,
             UVC_FRAMES_INFO[0][1].rate, UVC_FRAMES_INFO[0][1].interval);
    ESP_LOGI(TAG, "\tFrame(3) = %d * %d @%dfps (interval: %d)",
             UVC_FRAMES_INFO[0][2].width, UVC_FRAMES_INFO[0][2].height,
             UVC_FRAMES_INFO[0][2].rate, UVC_FRAMES_INFO[0][2].interval);
    ESP_LOGI(TAG, "\tFrame(4) = %d * %d @%dfps (interval: %d)",
             UVC_FRAMES_INFO[0][3].width, UVC_FRAMES_INFO[0][3].height,
             UVC_FRAMES_INFO[0][3].rate, UVC_FRAMES_INFO[0][3].interval);
    ESP_LOGI(TAG, "===========================================");

    ESP_ERROR_CHECK(uvc_device_config(0, &config));
    ESP_ERROR_CHECK(uvc_device_init());

    ESP_LOGI(TAG, "UVC device initialized. Waiting for USB host connection...");

    // Main loop - just wait for callbacks
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
