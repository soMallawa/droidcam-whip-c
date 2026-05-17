// Copyright (C) 2023 DEV47APPS, github.com/dev47apps
#include <windows.h>
#include "plugin.h"

typedef LONG (WINAPI *rtl_get_version_t)(PRTL_OSVERSIONINFOW);

void get_os_name_version(char *out, size_t out_size) {
    strncpy(out, "windows", out_size);
    if (out_size > 0)
        out[out_size - 1] = 0;

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll)
        return;

    rtl_get_version_t rtl_get_version =
        (rtl_get_version_t)GetProcAddress(ntdll, "RtlGetVersion");
    if (!rtl_get_version)
        return;

    RTL_OSVERSIONINFOW version = {0};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version(&version) == 0) {
        snprintf(out, out_size, "win%lu.%lu.%lu",
            (unsigned long)version.dwMajorVersion,
            (unsigned long)version.dwMinorVersion,
            (unsigned long)version.dwBuildNumber);
    }
}
