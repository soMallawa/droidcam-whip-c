#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <pthread.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "net.h"
#include "plugin.h"
#include "plugin_properties.h"
#include "source.h"
#ifdef _WIN32
#include "gui_win32.h"
#endif

char os_name_version[64];
const char* bindIP = NULL;

static volatile sig_atomic_t stop_requested = 0;

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        stop_requested = 1;
        return TRUE;
    }
    return FALSE;
}
#else
static void handle_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}
#endif

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--gui] phone-ip [rtsp-url] [--port 4747] [--width 1280] [--height 720] [--fps 25] [--format avc|hevc|jpg] [--no-hw]\n",
        argv0);
}

static enum VideoFormat parse_format(const char *value) {
    if (strcmp(value, "hevc") == 0 || strcmp(value, "h265") == 0)
        return FORMAT_HEVC;
    if (strcmp(value, "jpg") == 0 || strcmp(value, "mjpg") == 0 || strcmp(value, "mjpeg") == 0)
        return FORMAT_MJPG;
    return FORMAT_AVC;
}

#ifdef _WIN32
struct gui_pipeline {
    WinGui *gui;
    pthread_t thread;
    int thread_active;
    volatile sig_atomic_t stop_requested;
    void *source;
    struct droidcam_source_config config;
    char phone_ip[64];
    char rtsp_url[256];
};

static void sleep_seconds(int seconds) {
    Sleep((DWORD)seconds * 1000);
}

static void preview_frame(void *userdata, const uint8_t *bgr_data, int width, int height) {
    gui_set_frame((WinGui *)userdata, bgr_data, width, height);
}

static void *gui_pipeline_thread(void *data) {
    struct gui_pipeline *pipeline = (struct gui_pipeline *)data;
    gui_set_status(pipeline->gui, "DroidCam WHIP - Connecting...");

    pipeline->source = droidcam_source_start(&pipeline->config);
    if (!pipeline->source) {
        gui_set_status(pipeline->gui, "DroidCam WHIP - Connection failed");
        return NULL;
    }

    gui_set_status(pipeline->gui, "DroidCam WHIP - Connected");
    while (!pipeline->stop_requested)
        Sleep(100);

    droidcam_source_stop(pipeline->source);
    pipeline->source = NULL;
    gui_set_status(pipeline->gui, "DroidCam WHIP - Waiting for connection...");
    return NULL;
}

static int gui_start_pipeline(void *userdata, const struct droidcam_source_config *config) {
    struct gui_pipeline *pipeline = (struct gui_pipeline *)userdata;
    if (pipeline->thread_active)
        return 0;

    memset(&pipeline->config, 0, sizeof(pipeline->config));
    strncpy(pipeline->phone_ip, config->device_ip ? config->device_ip : "", sizeof(pipeline->phone_ip) - 1);
    strncpy(pipeline->rtsp_url, config->rtsp_url ? config->rtsp_url : "rtsp://127.0.0.1:8554/cam_in",
        sizeof(pipeline->rtsp_url) - 1);

    pipeline->config = *config;
    pipeline->config.device_ip = pipeline->phone_ip;
    pipeline->config.device_id = pipeline->phone_ip;
    pipeline->config.rtsp_url = pipeline->rtsp_url;
    pipeline->config.preview_callback = preview_frame;
    pipeline->config.preview_userdata = pipeline->gui;
    pipeline->stop_requested = 0;

    if (pthread_create(&pipeline->thread, NULL, gui_pipeline_thread, pipeline) != 0) {
        pipeline->thread_active = 0;
        gui_set_status(pipeline->gui, "DroidCam WHIP - Failed to start pipeline");
        return -1;
    }

    pipeline->thread_active = 1;
    return 0;
}

static void gui_stop_pipeline(void *userdata) {
    struct gui_pipeline *pipeline = (struct gui_pipeline *)userdata;
    if (!pipeline->thread_active)
        return;

    pipeline->stop_requested = 1;
    pthread_join(pipeline->thread, NULL);
    pipeline->thread_active = 0;
}
#else
static void sleep_seconds(int seconds) {
    sleep((unsigned int)seconds);
}
#endif

int main(int argc, char **argv) {
    struct droidcam_source_config config;
    const char *phone_ip = NULL;
    const char *rtsp_url = "rtsp://127.0.0.1:8554/cam_in";
    int gui_mode = 0;

    memset(&config, 0, sizeof(config));
    config.port = DEFAULT_PORT;
    config.width = 1280;
    config.height = 720;
    config.fps = 25;
    config.video_format = FORMAT_AVC;
    config.use_hw = 1;
    config.use_hdr = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gui") == 0) {
            gui_mode = 1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
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

    if (!phone_ip && !gui_mode) {
        usage(argv[0]);
        return 2;
    }

    if (config.port <= 0 || config.port > 65535 || config.width <= 0 || config.height <= 0 || config.fps <= 0) {
        usage(argv[0]);
        return 2;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
#endif

    memset(os_name_version, 0, sizeof(os_name_version));
    get_os_name_version(os_name_version, sizeof(os_name_version));

    if (!net_init()) {
        elog("network init failed");
        return 1;
    }

#ifdef _WIN32
    if (gui_mode) {
        WinGui *gui = gui_create(GetModuleHandleA(NULL), SW_SHOWDEFAULT);
        if (!gui) {
            net_cleanup();
            return 1;
        }

        struct gui_pipeline pipeline;
        memset(&pipeline, 0, sizeof(pipeline));
        pipeline.gui = gui;
        gui_set_pipeline_callbacks(gui, gui_start_pipeline, gui_stop_pipeline, &pipeline);

        if (phone_ip) {
            config.device_ip = phone_ip;
            config.device_id = phone_ip;
            config.rtsp_url = rtsp_url;
        } else {
            gui_get_config(gui, &config);
        }

        int rc = gui_run(gui);
        gui_stop_pipeline(&pipeline);
        gui_destroy(gui);
        net_cleanup();
        return rc;
    }
#else
    if (gui_mode) {
        fprintf(stderr, "--gui is only available on Windows; continuing in console mode.\n");
    }
#endif

    if (!phone_ip) {
        usage(argv[0]);
        net_cleanup();
        return 2;
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
        sleep_seconds(1);

    droidcam_source_stop(source);
    net_cleanup();
    return 0;
}
