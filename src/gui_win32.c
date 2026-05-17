#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui_win32.h"
#include "plugin.h"
#include "plugin_properties.h"

#define GUI_CLASS_NAME "DroidCamWhipWindow"
#define ID_CONNECT        1001
#define ID_SETTINGS       1003
#define ID_STATUSBAR      1005
#define ID_REFRESH_TIMER  1
#define PANEL_W           220

#define IDC_IP     2001
#define IDC_PORT   2002
#define IDC_RTSP   2003
#define IDC_WIDTH  2004
#define IDC_HEIGHT 2005
#define IDC_FORMAT 2006
#define IDC_FPS_C  2007

struct WinGui {
    HINSTANCE hInstance;
    HWND hwnd;
    HWND statusbar;
    /* side-panel controls */
    HWND lbl_header;
    HWND lbl_ip_key;
    HWND lbl_ip_val;
    HWND lbl_rtsp_key;
    HWND lbl_rtsp_val;
    HWND lbl_status;
    HWND lbl_fps;
    HWND btn_connect;
    HWND btn_settings;
    HFONT fnt_status;
    /* video frame buffer (BGR24) — written by worker thread */
    CRITICAL_SECTION lock;
    uint8_t *frame;
    size_t   frame_size;
    int      frame_width;
    int      frame_height;
    /* paint buffer (BGR24) — used only by main thread, no lock needed */
    uint8_t *paint_buf;
    size_t   paint_buf_size;
    int      paint_width;
    int      paint_height;
    /* state */
    char status[256];
    char phone_ip[64];
    char rtsp_url[256];
    int  port;
    int  width;
    int  height;
    int  fps;
    enum VideoFormat format;
    int  running;
    int  frame_count;
    int  shown_fps;
    DWORD fps_tick;
    char ini_path[MAX_PATH];
    gui_start_callback  start_cb;
    gui_stop_callback   stop_cb;
    void               *callback_userdata;
};

/* ------------------------------------------------------------------ */
/* Settings INI                                                         */
/* ------------------------------------------------------------------ */

static const char *format_to_string(enum VideoFormat f) {
    switch (f) {
        case FORMAT_HEVC: return "hevc";
        case FORMAT_MJPG: return "jpg";
        default:          return "avc";
    }
}
static enum VideoFormat string_to_format(const char *v) {
    if (!strcmp(v,"hevc")||!strcmp(v,"h265"))             return FORMAT_HEVC;
    if (!strcmp(v,"jpg")||!strcmp(v,"mjpg")||!strcmp(v,"mjpeg")) return FORMAT_MJPG;
    return FORMAT_AVC;
}
static void get_ini_path(WinGui *g) {
    DWORD n = GetModuleFileNameA(NULL, g->ini_path, MAX_PATH);
    if (!n || n>=MAX_PATH) { strncpy(g->ini_path,"settings.ini",MAX_PATH); return; }
    char *s = strrchr(g->ini_path,'\\');
    if (!s) s = strrchr(g->ini_path,'/');
    if (s) s[1] = 0;
    strncat(g->ini_path,"settings.ini",sizeof(g->ini_path)-strlen(g->ini_path)-1);
}
static void load_settings(WinGui *g) {
    char fmt[32];
    get_ini_path(g);
    GetPrivateProfileStringA("connection","phone_ip","192.168.1.100",g->phone_ip,sizeof(g->phone_ip),g->ini_path);
    g->port   = GetPrivateProfileIntA("connection","port",DEFAULT_PORT,g->ini_path);
    g->width  = GetPrivateProfileIntA("video","width",1280,g->ini_path);
    g->height = GetPrivateProfileIntA("video","height",720,g->ini_path);
    g->fps    = GetPrivateProfileIntA("video","fps",25,g->ini_path);
    GetPrivateProfileStringA("video","format","avc",fmt,sizeof(fmt),g->ini_path);
    g->format = string_to_format(fmt);
    GetPrivateProfileStringA("output","rtsp_url","rtsp://127.0.0.1:8554/cam_in",g->rtsp_url,sizeof(g->rtsp_url),g->ini_path);
}
static void save_settings(WinGui *g) {
    char buf[32];
    WritePrivateProfileStringA("connection","phone_ip",g->phone_ip,g->ini_path);
    snprintf(buf,sizeof(buf),"%d",g->port);  WritePrivateProfileStringA("connection","port",buf,g->ini_path);
    snprintf(buf,sizeof(buf),"%d",g->width); WritePrivateProfileStringA("video","width",buf,g->ini_path);
    snprintf(buf,sizeof(buf),"%d",g->height);WritePrivateProfileStringA("video","height",buf,g->ini_path);
    snprintf(buf,sizeof(buf),"%d",g->fps);   WritePrivateProfileStringA("video","fps",buf,g->ini_path);
    WritePrivateProfileStringA("video","format",format_to_string(g->format),g->ini_path);
    WritePrivateProfileStringA("output","rtsp_url",g->rtsp_url,g->ini_path);
}

/* ------------------------------------------------------------------ */
/* Panel refresh                                                        */
/* ------------------------------------------------------------------ */

static void update_panel(WinGui *g) {
    /* IP value */
    char buf[300];
    snprintf(buf,sizeof(buf),"%s  (port %d)", g->phone_ip, g->port);
    SetWindowTextA(g->lbl_ip_val, buf);

    /* RTSP value — truncate if too long */
    if (strlen(g->rtsp_url) > 26) {
        snprintf(buf,sizeof(buf),"...%s", g->rtsp_url + strlen(g->rtsp_url)-23);
    } else {
        strncpy(buf, g->rtsp_url, sizeof(buf)-1);
    }
    SetWindowTextA(g->lbl_rtsp_val, buf);

    /* Status label: color handled via WM_CTLCOLORSTATIC */
    SetWindowTextA(g->lbl_status, g->running ? "Connected" : "Disconnected");
    InvalidateRect(g->lbl_status, NULL, TRUE);

    /* Connect button text */
    SetWindowTextA(g->btn_connect, g->running ? "Disconnect" : "Connect");

    /* FPS label (blank when idle) */
    if (!g->running) SetWindowTextA(g->lbl_fps, "");

    /* Bottom status bar */
    snprintf(buf,sizeof(buf),"  %s   |   Phone: %s   |   %dx%d @ %d fps   |   %s",
        g->running ? "Connected" : "Disconnected",
        g->phone_ip, g->width, g->height, g->fps,
        format_to_string(g->format));
    SendMessageA(g->statusbar, SB_SETTEXTA, 0, (LPARAM)buf);
}

/* ------------------------------------------------------------------ */
/* Panel layout (called on create and on resize)                        */
/* ------------------------------------------------------------------ */

static void layout_panel(WinGui *g) {
    if (!g->lbl_ip_val) return;
    RECT rc; GetClientRect(g->hwnd, &rc);
    RECT sbrc; GetWindowRect(g->statusbar, &sbrc);
    int sbh = sbrc.bottom - sbrc.top;
    int ch  = rc.bottom - sbh;
    int px  = rc.right - PANEL_W + 1;
    int pad = 14;
    int w   = PANEL_W - pad*2 - 1;
    int y   = 16;

    /* Header */
    SetWindowPos(g->lbl_header, NULL, px+pad, y, w, 22, SWP_NOZORDER|SWP_NOACTIVATE); y += 30;

    /* Phone IP group */
    SetWindowPos(g->lbl_ip_key, NULL, px+pad, y, w, 16, SWP_NOZORDER|SWP_NOACTIVATE); y += 18;
    SetWindowPos(g->lbl_ip_val, NULL, px+pad, y, w, 18, SWP_NOZORDER|SWP_NOACTIVATE); y += 28;

    /* RTSP group */
    SetWindowPos(g->lbl_rtsp_key, NULL, px+pad, y, w, 16, SWP_NOZORDER|SWP_NOACTIVATE); y += 18;
    SetWindowPos(g->lbl_rtsp_val, NULL, px+pad, y, w, 18, SWP_NOZORDER|SWP_NOACTIVATE); y += 36;

    /* Status */
    SetWindowPos(g->lbl_status, NULL, px+pad, y, w, 36, SWP_NOZORDER|SWP_NOACTIVATE); y += 44;
    SetWindowPos(g->lbl_fps,    NULL, px+pad, y, w, 18, SWP_NOZORDER|SWP_NOACTIVATE);

    /* Buttons anchored near bottom */
    int btn_y = ch - 82;
    if (btn_y < y + 16) btn_y = y + 16;
    SetWindowPos(g->btn_connect,  NULL, px+pad, btn_y,    w, 38, SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(g->btn_settings, NULL, px+pad, btn_y+46, w, 28, SWP_NOZORDER|SWP_NOACTIVATE);
}

/* ------------------------------------------------------------------ */
/* Preview painting                                                     */
/* ------------------------------------------------------------------ */

static void paint_preview(WinGui *g, HDC hdc, RECT r) {
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(hdc, &r, black);

    /* Copy frame under the lock, then paint without it so we don't block the worker thread. */
    EnterCriticalSection(&g->lock);
    int has_frame = g->frame && g->frame_width > 0 && g->frame_height > 0;
    if (has_frame) {
        size_t needed = (size_t)g->frame_width * g->frame_height * 3;
        if (g->paint_buf_size < needed) {
            uint8_t *n = (uint8_t*)realloc(g->paint_buf, needed);
            if (n) { g->paint_buf = n; g->paint_buf_size = needed; }
            else   { has_frame = 0; }
        }
        if (has_frame) {
            memcpy(g->paint_buf, g->frame, needed);
            g->paint_width  = g->frame_width;
            g->paint_height = g->frame_height;
        }
    }
    char hint[256];
    strncpy(hint, g->status, sizeof(hint)-1); hint[sizeof(hint)-1]=0;
    LeaveCriticalSection(&g->lock);

    if (has_frame) {
        BITMAPINFO bmi; memset(&bmi,0,sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = g->paint_width;
        bmi.bmiHeader.biHeight      = -g->paint_height;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 24;
        bmi.bmiHeader.biCompression = BI_RGB;
        int aw = r.right-r.left, ah = r.bottom-r.top;
        int dw = aw, dh = (int)((double)dw*g->paint_height/g->paint_width);
        if (dh>ah) { dh=ah; dw=(int)((double)dh*g->paint_width/g->paint_height); }
        StretchDIBits(hdc, (aw-dw)/2,(ah-dh)/2, dw,dh,
            0,0, g->paint_width,g->paint_height,
            g->paint_buf, &bmi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        SetBkMode(hdc, TRANSPARENT);
        RECT tr = {r.left, r.top, r.right, r.top+(r.bottom-r.top)/2};
        SetTextColor(hdc, RGB(60,60,60));
        DrawTextA(hdc, "No Camera Signal", -1, &tr, DT_CENTER|DT_BOTTOM|DT_SINGLELINE);
        RECT hr = {r.left+20, r.top+(r.bottom-r.top)/2+8, r.right-20, r.bottom};
        SetTextColor(hdc, RGB(80,80,80));
        DrawTextA(hdc, hint, -1, &hr, DT_CENTER|DT_TOP|DT_WORDBREAK);
    }
}

/* ------------------------------------------------------------------ */
/* Settings dialog                                                      */
/* ------------------------------------------------------------------ */

static void add_label(HWND parent, const char *text, int x, int y, int w, int h) {
    CreateWindowA("STATIC", text, WS_CHILD|WS_VISIBLE, x,y,w,h, parent, NULL, NULL, NULL);
}

/* WM_COMMAND from buttons goes via SendMessage (synchronous) to the "STATIC"
   parent which silently drops it.  Subclass the dialog window to relay it into
   the posted-message queue via WM_APP so GetMessageA can see it. */
static WNDPROC g_settings_orig_proc;
static LRESULT CALLBACK settings_relay(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COMMAND) { PostMessageA(hwnd, WM_APP, wParam, lParam); return 0; }
    if (msg == WM_CLOSE)   { PostMessageA(hwnd, WM_APP, IDCANCEL, 0);   return 0; }
    return CallWindowProcA(g_settings_orig_proc, hwnd, msg, wParam, lParam);
}

static void show_settings_dialog(WinGui *g) {
    HWND dlg = CreateWindowExA(WS_EX_DLGMODALFRAME, "STATIC", "Settings",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT,CW_USEDEFAULT, 440,310,
        g->hwnd, NULL, g->hInstance, NULL);
    if (!dlg) return;
    EnableWindow(g->hwnd, FALSE);

    add_label(dlg,"Phone IP",       16, 20, 90,22);
    HWND ip = CreateWindowA("EDIT",g->phone_ip,WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
        120,18,280,24,dlg,(HMENU)IDC_IP,g->hInstance,NULL);

    add_label(dlg,"Port",           16, 54, 90,22);
    char buf[32]; snprintf(buf,sizeof(buf),"%d",g->port);
    HWND port = CreateWindowA("EDIT",buf,WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER,
        120,52,100,24,dlg,(HMENU)IDC_PORT,g->hInstance,NULL);

    add_label(dlg,"RTSP URL",       16, 88, 90,22);
    HWND rtsp = CreateWindowA("EDIT",g->rtsp_url,WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
        120,86,280,24,dlg,(HMENU)IDC_RTSP,g->hInstance,NULL);

    add_label(dlg,"Width",          16,122, 90,22);
    snprintf(buf,sizeof(buf),"%d",g->width);
    HWND width = CreateWindowA("EDIT",buf,WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER,
        120,120,100,24,dlg,(HMENU)IDC_WIDTH,g->hInstance,NULL);

    add_label(dlg,"Height",        230,122, 55,22);
    snprintf(buf,sizeof(buf),"%d",g->height);
    HWND height = CreateWindowA("EDIT",buf,WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER,
        290,120,100,24,dlg,(HMENU)IDC_HEIGHT,g->hInstance,NULL);

    add_label(dlg,"FPS",            16,156, 90,22);
    snprintf(buf,sizeof(buf),"%d",g->fps);
    HWND fps = CreateWindowA("EDIT",buf,WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER,
        120,154,100,24,dlg,(HMENU)IDC_FPS_C,g->hInstance,NULL);

    add_label(dlg,"Format",        230,156, 55,22);
    HWND fmt = CreateWindowA("COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,
        290,154,110,120,dlg,(HMENU)IDC_FORMAT,g->hInstance,NULL);
    SendMessageA(fmt,CB_ADDSTRING,0,(LPARAM)"avc (H.264)");
    SendMessageA(fmt,CB_ADDSTRING,0,(LPARAM)"hevc (H.265)");
    SendMessageA(fmt,CB_ADDSTRING,0,(LPARAM)"jpg (MJPEG)");
    SendMessageA(fmt,CB_SETCURSEL, g->format==FORMAT_HEVC?1:(g->format==FORMAT_MJPG?2:0), 0);

    CreateWindowA("BUTTON","Save",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
        230,228,80,28,dlg,(HMENU)IDOK,g->hInstance,NULL);
    CreateWindowA("BUTTON","Cancel",WS_CHILD|WS_VISIBLE,
        320,228,80,28,dlg,(HMENU)IDCANCEL,g->hInstance,NULL);

    g_settings_orig_proc = (WNDPROC)SetWindowLongPtrA(dlg,GWLP_WNDPROC,(LONG_PTR)settings_relay);
    ShowWindow(dlg, SW_SHOW);

    MSG msg; int done=0, result=0;
    while (!done && GetMessageA(&msg,NULL,0,0) > 0) {
        if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); done=1; }
        else if (msg.message==WM_APP && msg.hwnd==dlg) {
            int id = LOWORD(msg.wParam);
            if (id==IDOK)     { result=1; done=1; }
            else if (id==IDCANCEL) { done=1; }
        } else if (!IsDialogMessageA(dlg,&msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    if (result) {
        GetWindowTextA(ip,    g->phone_ip,  sizeof(g->phone_ip));
        GetWindowTextA(rtsp,  g->rtsp_url,  sizeof(g->rtsp_url));
        GetWindowTextA(port,  buf,sizeof(buf)); g->port   = atoi(buf);
        GetWindowTextA(width, buf,sizeof(buf)); g->width  = atoi(buf);
        GetWindowTextA(height,buf,sizeof(buf)); g->height = atoi(buf);
        GetWindowTextA(fps,   buf,sizeof(buf)); g->fps    = atoi(buf);
        int sel = (int)SendMessageA(fmt,CB_GETCURSEL,0,0);
        g->format = sel==1?FORMAT_HEVC:(sel==2?FORMAT_MJPG:FORMAT_AVC);
        if (g->port  <=0) g->port  =DEFAULT_PORT;
        if (g->width <=0) g->width =1280;
        if (g->height<=0) g->height=720;
        if (g->fps   <=0) g->fps   =25;
        save_settings(g);
        update_panel(g);
    }
    DestroyWindow(dlg);
    EnableWindow(g->hwnd, TRUE);
    SetForegroundWindow(g->hwnd);
}

/* ------------------------------------------------------------------ */
/* Connect / disconnect                                                 */
/* ------------------------------------------------------------------ */

static void request_start(WinGui *g) {
    if (g->running || !g->start_cb) return;
    struct droidcam_source_config cfg;
    gui_get_config(g, &cfg);
    if (g->start_cb(g->callback_userdata, &cfg) == 0)
        gui_set_running(g, 1);
}
static void request_stop(WinGui *g) {
    if (!g->running) return;
    if (g->stop_cb) g->stop_cb(g->callback_userdata);
    gui_set_running(g, 0);
    gui_set_status(g, "Enter the phone IP in Settings, then click Connect.");
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                     */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WinGui *g = (WinGui*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA*)lParam;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    case WM_SIZE:
        if (g) {
            if (g->statusbar) SendMessageA(g->statusbar, WM_SIZE, 0, 0);
            layout_panel(g);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1; /* all drawing done in WM_PAINT */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g) {
            RECT rc; GetClientRect(hwnd, &rc);
            RECT sbrc; GetWindowRect(g->statusbar, &sbrc);
            rc.bottom -= sbrc.bottom - sbrc.top;

            /* Panel background */
            RECT panel = {rc.right-PANEL_W, 0, rc.right, rc.bottom};
            FillRect(hdc, &panel, GetSysColorBrush(COLOR_BTNFACE));

            /* Separator line */
            HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
            HPEN old = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, rc.right-PANEL_W, 0, NULL);
            LineTo(hdc,   rc.right-PANEL_W, rc.bottom);
            SelectObject(hdc, old); DeleteObject(pen);

            /* Preview */
            RECT prev = {0, 0, rc.right-PANEL_W, rc.bottom};
            paint_preview(g, hdc, prev);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    /* Color the status label green/gray based on connection state */
    case WM_CTLCOLORSTATIC: {
        if (!g) break;
        HDC  hdc  = (HDC) wParam;
        HWND ctrl = (HWND)lParam;
        SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
        if (ctrl == g->lbl_status) {
            SetTextColor(hdc, g->running ? RGB(0,180,60) : RGB(130,130,130));
        } else if (ctrl == g->lbl_header) {
            SetTextColor(hdc, GetSysColor(COLOR_CAPTIONTEXT));
            SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
        } else if (ctrl == g->lbl_ip_key || ctrl == g->lbl_rtsp_key) {
            SetTextColor(hdc, RGB(100,100,100));
        } else {
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        }
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_TIMER:
        if (wParam == ID_REFRESH_TIMER && g) {
            /* Update FPS label on the main thread — no cross-thread blocking */
            if (g->running) {
                int fps;
                EnterCriticalSection(&g->lock);
                fps = g->shown_fps;
                LeaveCriticalSection(&g->lock);
                char buf[32]; snprintf(buf, sizeof(buf), "%d FPS", fps);
                SetWindowTextA(g->lbl_fps, buf);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_COMMAND:
        if (!g) break;
        switch (LOWORD(wParam)) {
        case ID_CONNECT:
            if (g->running) request_stop(g);
            else            request_start(g);
            return 0;
        case ID_SETTINGS:
            show_settings_dialog(g);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (g) request_stop(g);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, ID_REFRESH_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

WinGui *gui_create(HINSTANCE hInstance, int nCmdShow) {
    WinGui *g = (WinGui*)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->hInstance = hInstance;
    InitializeCriticalSection(&g->lock);
    strncpy(g->status,
        "Enter phone IP in Settings, then click Connect.",
        sizeof(g->status)-1);
    load_settings(g);

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSA wc; memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = GUI_CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    g->hwnd = CreateWindowExA(0, GUI_CLASS_NAME, "DroidCam WHIP",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT, 1020,640,
        NULL,NULL, hInstance, g);
    if (!g->hwnd) { gui_destroy(g); return NULL; }

    g->statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "",
        WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP, 0,0,0,0,
        g->hwnd, (HMENU)ID_STATUSBAR, hInstance, NULL);

    /* Fonts */
    HFONT def_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    LOGFONTA lf; GetObjectA(def_font, sizeof(lf), &lf);
    lf.lfHeight = -15; lf.lfWeight = FW_BOLD;
    g->fnt_status = CreateFontIndirectA(&lf);

    /* Panel labels */
    DWORD ss = WS_CHILD|WS_VISIBLE|SS_LEFT|SS_NOPREFIX;
    g->lbl_header   = CreateWindowA("STATIC","DroidCam WHIP",ss|SS_CENTERIMAGE, 0,0,10,10, g->hwnd,NULL,hInstance,NULL);
    g->lbl_ip_key   = CreateWindowA("STATIC","PHONE IP",     ss, 0,0,10,10, g->hwnd,NULL,hInstance,NULL);
    g->lbl_ip_val   = CreateWindowA("STATIC","",             ss, 0,0,10,10, g->hwnd,NULL,hInstance,NULL);
    g->lbl_rtsp_key = CreateWindowA("STATIC","RTSP OUTPUT",  ss, 0,0,10,10, g->hwnd,NULL,hInstance,NULL);
    g->lbl_rtsp_val = CreateWindowA("STATIC","",             ss, 0,0,10,10, g->hwnd,NULL,hInstance,NULL);
    g->lbl_status   = CreateWindowA("STATIC","",             ss, 0,0,10,10, g->hwnd,NULL,hInstance,NULL);
    g->lbl_fps      = CreateWindowA("STATIC","",             ss, 0,0,10,10, g->hwnd,NULL,hInstance,NULL);
    g->btn_connect  = CreateWindowA("BUTTON","Connect",  WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,10,10,g->hwnd,(HMENU)ID_CONNECT, hInstance,NULL);
    g->btn_settings = CreateWindowA("BUTTON","Settings", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,10,10,g->hwnd,(HMENU)ID_SETTINGS,hInstance,NULL);

    /* Assign fonts */
    if (g->fnt_status) SendMessageA(g->lbl_status, WM_SETFONT,(WPARAM)g->fnt_status,0);
    SendMessageA(g->lbl_header,   WM_SETFONT,(WPARAM)def_font,0);
    SendMessageA(g->lbl_ip_key,   WM_SETFONT,(WPARAM)def_font,0);
    SendMessageA(g->lbl_ip_val,   WM_SETFONT,(WPARAM)def_font,0);
    SendMessageA(g->lbl_rtsp_key, WM_SETFONT,(WPARAM)def_font,0);
    SendMessageA(g->lbl_rtsp_val, WM_SETFONT,(WPARAM)def_font,0);
    SendMessageA(g->lbl_fps,      WM_SETFONT,(WPARAM)def_font,0);
    SendMessageA(g->btn_connect,  WM_SETFONT,(WPARAM)def_font,0);
    SendMessageA(g->btn_settings, WM_SETFONT,(WPARAM)def_font,0);

    update_panel(g);
    ShowWindow(g->hwnd, nCmdShow);
    UpdateWindow(g->hwnd);
    layout_panel(g);
    SetTimer(g->hwnd, ID_REFRESH_TIMER, 33, NULL); /* 30fps repaint driven from main thread */
    return g;
}

int gui_run(WinGui *g) {
    (void)g;
    MSG msg;
    while (GetMessageA(&msg,NULL,0,0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}

void gui_destroy(WinGui *g) {
    if (!g) return;
    if (g->hwnd) DestroyWindow(g->hwnd);
    if (g->fnt_status) DeleteObject(g->fnt_status);
    free(g->frame);
    free(g->paint_buf);
    DeleteCriticalSection(&g->lock);
    free(g);
}

void gui_set_frame(WinGui *g, const uint8_t *bgr_data, int width, int height) {
    if (!g || !bgr_data || width<=0 || height<=0) return;
    size_t needed = (size_t)width*(size_t)height*3u;
    EnterCriticalSection(&g->lock);
    if (g->frame_size < needed) {
        uint8_t *n = (uint8_t*)realloc(g->frame, needed);
        if (!n) { LeaveCriticalSection(&g->lock); return; }
        g->frame = n; g->frame_size = needed;
    }
    memcpy(g->frame, bgr_data, needed);
    g->frame_width  = width;
    g->frame_height = height;
    g->frame_count++;
    DWORD now = GetTickCount();
    if (!g->fps_tick) g->fps_tick = now;
    if (now - g->fps_tick >= 1000) {
        g->shown_fps  = g->frame_count;
        g->frame_count = 0;
        g->fps_tick   = now;
    }
    LeaveCriticalSection(&g->lock);
    /* Repaint and FPS label are driven by WM_TIMER on the main thread.
       The worker never calls any Windows API — no cross-thread blocking possible. */
}

void gui_set_status(WinGui *g, const char *text) {
    if (!g||!text) return;
    EnterCriticalSection(&g->lock);
    strncpy(g->status, text, sizeof(g->status)-1);
    g->status[sizeof(g->status)-1] = 0;
    LeaveCriticalSection(&g->lock);
}

void gui_set_running(WinGui *g, int running) {
    if (!g) return;
    g->running = running;
    update_panel(g);
    InvalidateRect(g->hwnd, NULL, FALSE);
}

int gui_is_running(WinGui *g) { return g ? g->running : 0; }

void gui_set_pipeline_callbacks(WinGui *g,
    gui_start_callback start_cb, gui_stop_callback stop_cb, void *userdata) {
    if (!g) return;
    g->start_cb = start_cb;
    g->stop_cb  = stop_cb;
    g->callback_userdata = userdata;
}

void gui_get_config(WinGui *g, struct droidcam_source_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->device_ip    = g->phone_ip;
    cfg->device_id    = g->phone_ip;
    cfg->port         = g->port;
    cfg->width        = g->width;
    cfg->height       = g->height;
    cfg->fps          = g->fps;
    cfg->video_format = g->format;
    cfg->use_hw       = 1;
    cfg->use_hdr      = 0;
    cfg->rtsp_url     = g->rtsp_url;
}

#endif
