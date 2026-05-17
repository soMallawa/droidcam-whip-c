#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "net.h"
#include "plugin.h"
#include "plugin_properties.h"
#include "source.h"

char os_name_version[64];
const char* bindIP = NULL;

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s phone-ip [rtsp-url] [--port 4747] [--width 1280] [--height 720] [--fps 25] [--format avc|hevc|jpg] [--no-hw]\n",
        argv0);
}

static enum VideoFormat parse_format(const char *value) {
    if (strcmp(value, "hevc") == 0 || strcmp(value, "h265") == 0)
        return FORMAT_HEVC;
    if (strcmp(value, "jpg") == 0 || strcmp(value, "mjpg") == 0 || strcmp(value, "mjpeg") == 0)
        return FORMAT_MJPG;
    return FORMAT_AVC;
}

int main(int argc, char **argv) {
    struct droidcam_source_config config;
    const char *phone_ip = NULL;
    const char *rtsp_url = "rtsp://127.0.0.1:8554/cam_in";

    memset(&config, 0, sizeof(config));
    config.port = DEFAULT_PORT;
    config.width = 1280;
    config.height = 720;
    config.fps = 25;
    config.video_format = FORMAT_AVC;
    config.use_hw = 1;
    config.use_hdr = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            config.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            config.width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            config.height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            config.fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            config.video_format = parse_format(argv[++i]);
        } else if (strcmp(argv[i], "--no-hw") == 0) {
            config.use_hw = 0;
        } else if (strcmp(argv[i], "--hdr") == 0) {
            config.use_hdr = 1;
        } else if (!phone_ip) {
            phone_ip = argv[i];
        } else {
            rtsp_url = argv[i];
        }
    }

    if (!phone_ip) {
        usage(argv[0]);
        return 2;
    }

    if (config.port <= 0 || config.port > 65535 || config.width <= 0 || config.height <= 0 || config.fps <= 0) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    memset(os_name_version, 0, sizeof(os_name_version));
    get_os_name_version(os_name_version, sizeof(os_name_version));

    if (!net_init()) {
        elog("network init failed");
        return 1;
    }

    config.device_ip = phone_ip;
    config.device_id = phone_ip;
    config.rtsp_url = rtsp_url;

    void *source = droidcam_source_start(&config);
    if (!source) {
        net_cleanup();
        return 1;
    }

    while (!stop_requested)
        sleep(1);

    droidcam_source_stop(source);
    net_cleanup();
    return 0;
}
