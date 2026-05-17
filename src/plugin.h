// Copyright (C) 2021 DEV47APPS, github.com/dev47apps
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PLUGIN_VERSION_STR "250"

#define LOG_INFO 2
#define LOG_WARNING 1
#define LOG_ERROR 0

#define xlog(log_level, format, ...) \
    do { \
        const char *level = (log_level) == LOG_ERROR ? "error" : ((log_level) == LOG_WARNING ? "warn" : "info"); \
        fprintf(stderr, "[droidcam-whip] %s: " format "\n", level, ##__VA_ARGS__); \
    } while (0)

#ifdef DEBUG
#define dlog(format, ...) xlog(LOG_INFO, format, ##__VA_ARGS__)
#else
#define dlog(format, ...) do {} while (0)
#endif
#define ilog(format, ...) xlog(LOG_INFO, format, ##__VA_ARGS__)
#define elog(format, ...) xlog(LOG_WARNING, format, ##__VA_ARGS__)

#define HEADER_SIZE 12
#define NO_PTS UINT64_C(~0)
#define ARRAY_LEN(a) (sizeof(a) / sizeof(a[0]))
#define MAX_AV_PLANES 8
#define LIBOBS_API_MAJOR_VER 28

static inline void *bmalloc(size_t size) { return malloc(size); }
static inline void *brealloc(void *ptr, size_t size) { return realloc(ptr, size); }
static inline void bfree(void *ptr) { free(ptr); }

static inline void os_sleep_ms(uint32_t ms) { usleep((useconds_t)ms * 1000); }

static inline uint64_t os_gettime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static inline char *obs_module_file(const char *path) {
    size_t len = strlen(path) + 1;
    char *out = (char *)bmalloc(len);
    if (out) memcpy(out, path, len);
    return out;
}

enum video_format {
    VIDEO_FORMAT_NONE,
    VIDEO_FORMAT_I420,
    VIDEO_FORMAT_NV12,
    VIDEO_FORMAT_YUY2,
    VIDEO_FORMAT_UYVY,
    VIDEO_FORMAT_I422,
    VIDEO_FORMAT_I010,
    VIDEO_FORMAT_P010,
    VIDEO_FORMAT_I210,
};

enum video_range_type {
    VIDEO_RANGE_DEFAULT,
    VIDEO_RANGE_PARTIAL,
    VIDEO_RANGE_FULL,
};

enum video_colorspace {
    VIDEO_CS_DEFAULT,
    VIDEO_CS_601,
    VIDEO_CS_709,
    VIDEO_CS_SRGB,
    VIDEO_CS_2100_PQ,
    VIDEO_CS_2100_HLG,
};

enum video_trc {
    VIDEO_TRC_DEFAULT,
    VIDEO_TRC_SRGB,
    VIDEO_TRC_PQ,
    VIDEO_TRC_HLG,
};

enum audio_format {
    AUDIO_FORMAT_UNKNOWN,
    AUDIO_FORMAT_U8BIT,
    AUDIO_FORMAT_16BIT,
    AUDIO_FORMAT_32BIT,
    AUDIO_FORMAT_FLOAT,
    AUDIO_FORMAT_U8BIT_PLANAR,
    AUDIO_FORMAT_16BIT_PLANAR,
    AUDIO_FORMAT_32BIT_PLANAR,
    AUDIO_FORMAT_FLOAT_PLANAR,
};

enum speaker_layout {
    SPEAKERS_UNKNOWN,
    SPEAKERS_MONO,
    SPEAKERS_STEREO,
    SPEAKERS_2POINT1,
    SPEAKERS_4POINT0,
    SPEAKERS_4POINT1,
    SPEAKERS_5POINT1,
    SPEAKERS_7POINT1,
};

struct obs_source_frame2 {
    uint8_t *data[MAX_AV_PLANES];
    uint32_t linesize[MAX_AV_PLANES];
    uint32_t width;
    uint32_t height;
    uint64_t timestamp;
    enum video_format format;
    enum video_range_type range;
    enum video_trc trc;
    bool flip;
    float color_matrix[16];
    float color_range_min[3];
    float color_range_max[3];
};

struct obs_source_audio {
    uint8_t *data[MAX_AV_PLANES];
    uint32_t frames;
    uint32_t samples_per_sec;
    uint64_t timestamp;
    enum audio_format format;
    enum speaker_layout speakers;
};

static inline const char *get_video_format_name(enum video_format format) {
    switch (format) {
        case VIDEO_FORMAT_I420: return "I420";
        case VIDEO_FORMAT_NV12: return "NV12";
        case VIDEO_FORMAT_YUY2: return "YUY2";
        case VIDEO_FORMAT_UYVY: return "UYVY";
        case VIDEO_FORMAT_I422: return "I422";
        case VIDEO_FORMAT_I010: return "I010";
        case VIDEO_FORMAT_P010: return "P010";
        case VIDEO_FORMAT_I210: return "I210";
        default: return "NONE";
    }
}

static inline const char *get_video_colorspace_name(enum video_colorspace cs) {
    switch (cs) {
        case VIDEO_CS_601: return "601";
        case VIDEO_CS_709: return "709";
        case VIDEO_CS_SRGB: return "sRGB";
        case VIDEO_CS_2100_PQ: return "2100 PQ";
        case VIDEO_CS_2100_HLG: return "2100 HLG";
        default: return "default";
    }
}

static inline void video_format_get_parameters(enum video_colorspace cs,
        enum video_range_type range, float *matrix, float *range_min, float *range_max) {
    (void)cs;
    memset(matrix, 0, sizeof(float) * 16);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    range_min[0] = range_min[1] = range_min[2] = 0.0f;
    range_max[0] = range_max[1] = range_max[2] = range == VIDEO_RANGE_FULL ? 1.0f : 0.92f;
}

static inline void video_format_get_parameters_for_format(enum video_colorspace cs,
        enum video_range_type range, enum video_format format, float *matrix,
        float *range_min, float *range_max) {
    (void)format;
    video_format_get_parameters(cs, range, matrix, range_min, range_max);
}

#ifdef __cplusplus
extern "C" {
#endif
void get_os_name_version(char *, size_t);
#ifdef __cplusplus
}
#endif
