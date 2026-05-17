#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "gui_win32.h"
#include "plugin.h"
#include "plugin_properties.h"

#define GUI_CLASS_NAME "DroidCamWhipWindow"
#define ID_CONNECT 1001
#define ID_DISCONNECT 1002
#define ID_SETTINGS 1003
#define ID_STARTSTOP 1004
#define ID_STATUSBAR 1005

#define IDC_IP 2001
#define IDC_PORT 2002
#define IDC_RTSP 2003
#define IDC_WIDTH 2004
#define IDC_HEIGHT 2005
#define IDC_FORMAT 2006
#define IDC_FPS 2007

struct WinGui {
    HINSTANCE hInstance;
    HWND hwnd;
    HWND statusbar;
    CRITICAL_SECTION lock;
    uint8_t *frame;
    size_t frame_size;
    int frame_width;
    int frame_height;
    char status[256];
    char phone_ip[64];
    char rtsp_url[256];
    int port;
    int width;
    int height;
    int fps;
    enum VideoFormat format;
    int running;
    int frame_count;
    int shown_fps;
    DWORD fps_tick;
    char ini_path[MAX_PATH];
    gui_start_callback start_cb;
    gui_stop_callback stop_cb;
    void *callback_userdata;
};

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static const char *format_to_string(enum VideoFormat format) {
    switch (format) {
        case FORMAT_HEVC: return "hevc";
        case FORMAT_MJPG: return "jpg";
        case FORMAT_AVC:
        default: return "avc";
    }
}

static enum VideoFormat string_to_format(const char *value) {
    if (strcmp(value, "hevc") == 0 || strcmp(value, "h265") == 0)
        return FORMAT_HEVC;
    if (strcmp(value, "jpg") == 0 || strcmp(value, "mjpg") == 0 || strcmp(value, "mjpeg") == 0)
        return FORMAT_MJPG;
    return FORMAT_AVC;
}

static void get_ini_path(WinGui *gui) {
    DWORD len = GetModuleFileNameA(NULL, gui->ini_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        strncpy(gui->ini_path, "settings.ini", sizeof(gui->ini_path));
        return;
    }

    char *slash = strrchr(gui->ini_path, '\\');
    if (!slash)
        slash = strrchr(gui->ini_path, '/');
    if (slash)
        slash[1] = 0;
    strncat(gui->ini_path, "settings.ini", sizeof(gui->ini_path) - strlen(gui->ini_path) - 1);
}

static void load_settings(WinGui *gui) {
    char format[32];

    get_ini_path(gui);
    GetPrivateProfileStringA("connection", "phone_ip", "192.168.1.100",
        gui->phone_ip, sizeof(gui->phone_ip), gui->ini_path);
    gui->port = GetPrivateProfileIntA("connection", "port", DEFAULT_PORT, gui->ini_path);
    gui->width = GetPrivateProfileIntA("video", "width", 1280, gui->ini_path);
    gui->height = GetPrivateProfileIntA("video", "height", 720, gui->ini_path);
    gui->fps = GetPrivateProfileIntA("video", "fps", 25, gui->ini_path);
    GetPrivateProfileStringA("video", "format", "avc", format, sizeof(format), gui->ini_path);
    gui->format = string_to_format(format);
    GetPrivateProfileStringA("output", "rtsp_url", "rtsp://127.0.0.1:8554/cam_in",
        gui->rtsp_url, sizeof(gui->rtsp_url), gui->ini_path);
}

static void write_int_setting(const char *section, const char *key, int value, const char *path) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(section, key, buf, path);
}

static void save_settings(WinGui *gui) {
    WritePrivateProfileStringA("connection", "phone_ip", gui->phone_ip, gui->ini_path);
    write_int_setting("connection", "port", gui->port, gui->ini_path);
    write_int_setting("video", "width", gui->width, gui->ini_path);
    write_int_setting("video", "height", gui->height, gui->ini_path);
    write_int_setting("video", "fps", gui->fps, gui->ini_path);
    WritePrivateProfileStringA("video", "format", format_to_string(gui->format), gui->ini_path);
    WritePrivateProfileStringA("output", "rtsp_url", gui->rtsp_url, gui->ini_path);
}

static void update_statusbar(WinGui *gui) {
    char text[512];
    snprintf(text, sizeof(text), "%s | %s | %dx%d",
        gui->running ? "connected" : "disconnected",
        gui->phone_ip, gui->width, gui->height);
    SendMessageA(gui->statusbar, SB_SETTEXTA, 0, (LPARAM)text);
}

static void update_startstop_text(WinGui *gui) {
    HMENU menu = GetMenu(gui->hwnd);
    if (menu) {
        ModifyMenuA(menu, ID_STARTSTOP, MF_BYCOMMAND | MF_STRING,
            ID_STARTSTOP, gui->running ? "Stop" : "Start");
        DrawMenuBar(gui->hwnd);
    }
}

static void request_start(WinGui *gui) {
    struct droidcam_source_config config;
    if (gui->running || !gui->start_cb)
        return;
    gui_get_config(gui, &config);
    if (gui->start_cb(gui->callback_userdata, &config) == 0)
        gui_set_running(gui, 1);
}

static void request_stop(WinGui *gui) {
    if (!gui->running)
        return;
    if (gui->stop_cb)
        gui->stop_cb(gui->callback_userdata);
    gui_set_running(gui, 0);
    gui_set_status(gui, "DroidCam WHIP - Waiting for connection...");
}

static void add_label(HWND parent, const char *text, int x, int y, int w, int h) {
    CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h,
        parent, NULL, NULL, NULL);
}

static void show_settings_dialog(WinGui *gui) {
    HWND dlg = CreateWindowExA(WS_EX_DLGMODALFRAME, "STATIC", "Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 430, 300,
        gui->hwnd, NULL, gui->hInstance, NULL);
    if (!dlg)
        return;

    EnableWindow(gui->hwnd, FALSE);

    add_label(dlg, "Phone IP", 16, 20, 90, 22);
    HWND ip = CreateWindowA("EDIT", gui->phone_ip, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        120, 18, 270, 24, dlg, (HMENU)IDC_IP, gui->hInstance, NULL);
    add_label(dlg, "Port", 16, 54, 90, 22);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", gui->port);
    HWND port = CreateWindowA("EDIT", buf, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        120, 52, 100, 24, dlg, (HMENU)IDC_PORT, gui->hInstance, NULL);
    add_label(dlg, "RTSP URL", 16, 88, 90, 22);
    HWND rtsp = CreateWindowA("EDIT", gui->rtsp_url, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        120, 86, 270, 24, dlg, (HMENU)IDC_RTSP, gui->hInstance, NULL);
    add_label(dlg, "Width", 16, 122, 90, 22);
    snprintf(buf, sizeof(buf), "%d", gui->width);
    HWND width = CreateWindowA("EDIT", buf, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        120, 120, 100, 24, dlg, (HMENU)IDC_WIDTH, gui->hInstance, NULL);
    add_label(dlg, "Height", 230, 122, 55, 22);
    snprintf(buf, sizeof(buf), "%d", gui->height);
    HWND height = CreateWindowA("EDIT", buf, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        290, 120, 100, 24, dlg, (HMENU)IDC_HEIGHT, gui->hInstance, NULL);
    add_label(dlg, "FPS", 16, 156, 90, 22);
    snprintf(buf, sizeof(buf), "%d", gui->fps);
    HWND fps = CreateWindowA("EDIT", buf, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
        120, 154, 100, 24, dlg, (HMENU)IDC_FPS, gui->hInstance, NULL);
    add_label(dlg, "Format", 230, 156, 55, 22);
    HWND format = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        290, 154, 100, 120, dlg, (HMENU)IDC_FORMAT, gui->hInstance, NULL);
    SendMessageA(format, CB_ADDSTRING, 0, (LPARAM)"avc");
    SendMessageA(format, CB_ADDSTRING, 0, (LPARAM)"hevc");
    SendMessageA(format, CB_ADDSTRING, 0, (LPARAM)"jpg");
    SendMessageA(format, CB_SETCURSEL, gui->format == FORMAT_HEVC ? 1 : (gui->format == FORMAT_MJPG ? 2 : 0), 0);

    HWND ok = CreateWindowA("BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        220, 215, 80, 28, dlg, (HMENU)IDOK, gui->hInstance, NULL);
    HWND cancel = CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE,
        310, 215, 80, 28, dlg, (HMENU)IDCANCEL, gui->hInstance, NULL);
    (void)ip; (void)port; (void)rtsp; (void)width; (void)height; (void)fps; (void)ok; (void)cancel;

    ShowWindow(dlg, SW_SHOW);

    MSG msg;
    int done = 0;
    while (!done && GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (msg.hwnd == ok || (msg.message == WM_COMMAND && LOWORD(msg.wParam) == IDOK)) {
            GetWindowTextA(ip, gui->phone_ip, sizeof(gui->phone_ip));
            GetWindowTextA(rtsp, gui->rtsp_url, sizeof(gui->rtsp_url));
            GetWindowTextA(port, buf, sizeof(buf)); gui->port = atoi(buf);
            GetWindowTextA(width, buf, sizeof(buf)); gui->width = atoi(buf);
            GetWindowTextA(height, buf, sizeof(buf)); gui->height = atoi(buf);
            GetWindowTextA(fps, buf, sizeof(buf)); gui->fps = atoi(buf);
            int sel = (int)SendMessageA(format, CB_GETCURSEL, 0, 0);
            gui->format = sel == 1 ? FORMAT_HEVC : (sel == 2 ? FORMAT_MJPG : FORMAT_AVC);
            if (gui->port <= 0) gui->port = DEFAULT_PORT;
            if (gui->width <= 0) gui->width = 1280;
            if (gui->height <= 0) gui->height = 720;
            if (gui->fps <= 0) gui->fps = 25;
            save_settings(gui);
            update_statusbar(gui);
            done = 1;
        } else if (msg.hwnd == cancel || (msg.message == WM_COMMAND && LOWORD(msg.wParam) == IDCANCEL)) {
            done = 1;
        } else if (msg.message == WM_CLOSE && msg.hwnd == dlg) {
            done = 1;
        } else if (!IsDialogMessageA(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    DestroyWindow(dlg);
    EnableWindow(gui->hwnd, TRUE);
    SetForegroundWindow(gui->hwnd);
}

static void paint_window(WinGui *gui, HDC hdc) {
    RECT rc;
    GetClientRect(gui->hwnd, &rc);
    RECT status_rc;
    GetWindowRect(gui->statusbar, &status_rc);
    rc.bottom -= status_rc.bottom - status_rc.top;

    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(hdc, &rc, black);

    EnterCriticalSection(&gui->lock);
    if (gui->frame && gui->frame_width > 0 && gui->frame_height > 0) {
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = gui->frame_width;
        bmi.bmiHeader.biHeight = -gui->frame_height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;
        bmi.bmiHeader.biCompression = BI_RGB;

        int area_w = rc.right - rc.left;
        int area_h = rc.bottom - rc.top;
        int draw_w = area_w;
        int draw_h = (int)((double)draw_w * gui->frame_height / gui->frame_width);
        if (draw_h > area_h) {
            draw_h = area_h;
            draw_w = (int)((double)draw_h * gui->frame_width / gui->frame_height);
        }
        int x = (area_w - draw_w) / 2;
        int y = (area_h - draw_h) / 2;
        StretchDIBits(hdc, x, y, draw_w, draw_h, 0, 0, gui->frame_width,
            gui->frame_height, gui->frame, &bmi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(220, 220, 220));
        DrawTextA(hdc, gui->status, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    char status[256];
    int shown_fps = gui->shown_fps;
    LeaveCriticalSection(&gui->lock);

    snprintf(status, sizeof(status), "%d FPS", shown_fps);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextA(hdc, status, -1, &rc, DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WinGui *gui = (WinGui *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTA *cs = (CREATESTRUCTA *)lParam;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return TRUE;
        }
        case WM_SIZE:
            if (gui && gui->statusbar)
                SendMessageA(gui->statusbar, WM_SIZE, 0, 0);
            return 0;
        case WM_COMMAND:
            if (!gui)
                break;
            switch (LOWORD(wParam)) {
                case ID_CONNECT:
                case ID_STARTSTOP:
                    if (gui->running)
                        request_stop(gui);
                    else
                        request_start(gui);
                    return 0;
                case ID_DISCONNECT:
                    request_stop(gui);
                    return 0;
                case ID_SETTINGS:
                    show_settings_dialog(gui);
                    return 0;
            }
            break;
        case WM_CLOSE:
            if (gui)
                request_stop(gui);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (gui)
                paint_window(gui, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

WinGui *gui_create(HINSTANCE hInstance, int nCmdShow) {
    WinGui *gui = (WinGui *)calloc(1, sizeof(*gui));
    if (!gui)
        return NULL;

    gui->hInstance = hInstance;
    InitializeCriticalSection(&gui->lock);
    strncpy(gui->status, "DroidCam WHIP - Waiting for connection...", sizeof(gui->status));
    load_settings(gui);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = GUI_CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    HMENU menu = CreateMenu();
    AppendMenuA(menu, MF_STRING, ID_CONNECT, "Connect");
    AppendMenuA(menu, MF_STRING, ID_DISCONNECT, "Disconnect");
    AppendMenuA(menu, MF_STRING, ID_SETTINGS, "Settings");
    AppendMenuA(menu, MF_STRING, ID_STARTSTOP, "Start");

    gui->hwnd = CreateWindowExA(0, GUI_CLASS_NAME, "DroidCam WHIP",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 600,
        NULL, menu, hInstance, gui);
    if (!gui->hwnd) {
        gui_destroy(gui);
        return NULL;
    }

    gui->statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
        gui->hwnd, (HMENU)ID_STATUSBAR, hInstance, NULL);
    update_statusbar(gui);

    ShowWindow(gui->hwnd, nCmdShow);
    UpdateWindow(gui->hwnd);
    return gui;
}

void gui_set_frame(WinGui *gui, const uint8_t *bgr_data, int width, int height) {
    if (!gui || !bgr_data || width <= 0 || height <= 0)
        return;

    size_t needed = (size_t)width * (size_t)height * 3u;
    EnterCriticalSection(&gui->lock);
    if (gui->frame_size < needed) {
        uint8_t *next = (uint8_t *)realloc(gui->frame, needed);
        if (!next) {
            LeaveCriticalSection(&gui->lock);
            return;
        }
        gui->frame = next;
        gui->frame_size = needed;
    }
    memcpy(gui->frame, bgr_data, needed);
    gui->frame_width = width;
    gui->frame_height = height;
    gui->frame_count++;
    DWORD now = GetTickCount();
    if (gui->fps_tick == 0)
        gui->fps_tick = now;
    if (now - gui->fps_tick >= 1000) {
        gui->shown_fps = gui->frame_count;
        gui->frame_count = 0;
        gui->fps_tick = now;
    }
    LeaveCriticalSection(&gui->lock);
    InvalidateRect(gui->hwnd, NULL, FALSE);
}

void gui_set_status(WinGui *gui, const char *text) {
    if (!gui || !text)
        return;
    EnterCriticalSection(&gui->lock);
    strncpy(gui->status, text, sizeof(gui->status) - 1);
    gui->status[sizeof(gui->status) - 1] = 0;
    LeaveCriticalSection(&gui->lock);
    InvalidateRect(gui->hwnd, NULL, FALSE);
}

int gui_run(WinGui *gui) {
    (void)gui;
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}

void gui_destroy(WinGui *gui) {
    if (!gui)
        return;
    if (gui->hwnd)
        DestroyWindow(gui->hwnd);
    free(gui->frame);
    DeleteCriticalSection(&gui->lock);
    free(gui);
}

void gui_set_pipeline_callbacks(WinGui *gui, gui_start_callback start_cb,
    gui_stop_callback stop_cb, void *userdata) {
    if (!gui)
        return;
    gui->start_cb = start_cb;
    gui->stop_cb = stop_cb;
    gui->callback_userdata = userdata;
}

void gui_get_config(WinGui *gui, struct droidcam_source_config *config) {
    memset(config, 0, sizeof(*config));
    config->device_ip = gui->phone_ip;
    config->device_id = gui->phone_ip;
    config->port = gui->port;
    config->width = gui->width;
    config->height = gui->height;
    config->fps = gui->fps;
    config->video_format = gui->format;
    config->use_hw = 1;
    config->use_hdr = 0;
    config->rtsp_url = gui->rtsp_url;
}

void gui_set_running(WinGui *gui, int running) {
    if (!gui)
        return;
    gui->running = running;
    update_startstop_text(gui);
    update_statusbar(gui);
}

int gui_is_running(WinGui *gui) {
    return gui ? gui->running : 0;
}

#endif
