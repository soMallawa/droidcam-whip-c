#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EncoderRtspCtx EncoderRtspCtx;

EncoderRtspCtx *encoder_rtsp_init(const char *url, int width, int height, int fps);
int encoder_rtsp_encode_frame(EncoderRtspCtx *ctx, const uint8_t *bgr_data, int64_t pts);
void encoder_rtsp_close(EncoderRtspCtx *ctx);

#ifdef __cplusplus
}
#endif
