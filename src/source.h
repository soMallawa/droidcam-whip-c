// Copyright (C) 2023 DEV47APPS, github.com/dev47apps
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum DeviceType {
    NONE,
    WIFI,
    ADB,
    IOS,
    MDNS_WIFI,
};

struct active_device_info {
    enum DeviceType type;
    int port;
    const char *id;
    const char *ip;
};

enum VideoFormat {
    FORMAT_AVC,
    FORMAT_MJPG,
    FORMAT_HEVC,
};

struct droidcam_source_config {
    const char *device_ip;
    const char *device_id;
    int port;
    int width;
    int height;
    int fps;
    enum VideoFormat video_format;
    int use_hw;
    int use_hdr;
    const char *rtsp_url;
    void (*preview_callback)(void *userdata, const uint8_t *bgr_data, int width, int height);
    void *preview_userdata;
};

void *droidcam_source_start(const struct droidcam_source_config *config);
void droidcam_source_stop(void *data);

#ifdef __cplusplus
}
#endif
