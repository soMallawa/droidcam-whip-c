#pragma once

#ifdef _WIN32

#include <stdint.h>
#include <windows.h>

#include "source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WinGui WinGui;

typedef int (*gui_start_callback)(void *userdata, const struct droidcam_source_config *config);
typedef void (*gui_stop_callback)(void *userdata);

WinGui *gui_create(HINSTANCE hInstance, int nCmdShow);
void gui_set_frame(WinGui *gui, const uint8_t *bgr_data, int width, int height);
void gui_set_status(WinGui *gui, const char *text);
int gui_run(WinGui *gui);
void gui_destroy(WinGui *gui);

void gui_set_pipeline_callbacks(WinGui *gui, gui_start_callback start_cb,
    gui_stop_callback stop_cb, void *userdata);
void gui_get_config(WinGui *gui, struct droidcam_source_config *config);
void gui_set_running(WinGui *gui, int running);
int gui_is_running(WinGui *gui);

#ifdef __cplusplus
}
#endif

#endif
