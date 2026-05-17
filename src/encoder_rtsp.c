#include "encoder_rtsp.h"
#include "plugin.h"

#include <inttypes.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

struct EncoderRtspCtx {
    char *url;
    int width;
    int height;
    int fps;
    int64_t next_pts;

    const AVCodec *codec;
    AVCodecContext *codec_ctx;
    AVFrame *frame;
    AVPacket *packet;
    struct SwsContext *sws;

    AVFormatContext *fmt_ctx;
    AVStream *stream;
    int output_open;
};

static int open_output(EncoderRtspCtx *ctx);

static void close_output(EncoderRtspCtx *ctx) {
    if (!ctx || !ctx->fmt_ctx)
        return;

    if (ctx->output_open)
        av_write_trailer(ctx->fmt_ctx);

    if (!(ctx->fmt_ctx->oformat->flags & AVFMT_NOFILE) && ctx->fmt_ctx->pb)
        avio_closep(&ctx->fmt_ctx->pb);

    avformat_free_context(ctx->fmt_ctx);
    ctx->fmt_ctx = NULL;
    ctx->stream = NULL;
    ctx->output_open = 0;
}

static int init_encoder(EncoderRtspCtx *ctx, const char *name) {
    int ret;
    AVDictionary *opts = NULL;

    ctx->codec = avcodec_find_encoder_by_name(name);
    if (!ctx->codec)
        return AVERROR_ENCODER_NOT_FOUND;

    ctx->codec_ctx = avcodec_alloc_context3(ctx->codec);
    if (!ctx->codec_ctx)
        return AVERROR(ENOMEM);

    ctx->codec_ctx->width = ctx->width;
    ctx->codec_ctx->height = ctx->height;
    ctx->codec_ctx->time_base = (AVRational){1, ctx->fps};
    ctx->codec_ctx->framerate = (AVRational){ctx->fps, 1};
    ctx->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->codec_ctx->bit_rate = 4000000;
    ctx->codec_ctx->rc_max_rate = 4000000;
    ctx->codec_ctx->rc_buffer_size = 4000000;
    ctx->codec_ctx->gop_size = ctx->fps;
    ctx->codec_ctx->max_b_frames = 0;
    ctx->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (strcmp(name, "h264_nvenc") == 0) {
        av_dict_set(&opts, "preset", "p1", 0);
        av_dict_set(&opts, "tune", "ll", 0);
        av_dict_set(&opts, "rc", "cbr", 0);
        av_dict_set(&opts, "zerolatency", "1", 0);
    } else {
        av_dict_set(&opts, "preset", "ultrafast", 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
    }

    ret = avcodec_open2(ctx->codec_ctx, ctx->codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        avcodec_free_context(&ctx->codec_ctx);
        return ret;
    }

    ilog("using H.264 encoder: %s", name);
    return 0;
}

EncoderRtspCtx *encoder_rtsp_init(const char *url, int width, int height, int fps) {
    EncoderRtspCtx *ctx = (EncoderRtspCtx *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->url = strdup(url);
    ctx->width = width;
    ctx->height = height;
    ctx->fps = fps > 0 ? fps : 25;

    if (init_encoder(ctx, "h264_nvenc") < 0) {
        elog("h264_nvenc unavailable, falling back to libx264");
        if (init_encoder(ctx, "libx264") < 0) {
            elog("no usable H.264 encoder found");
            encoder_rtsp_close(ctx);
            return NULL;
        }
    }

    ctx->frame = av_frame_alloc();
    ctx->packet = av_packet_alloc();
    if (!ctx->frame || !ctx->packet) {
        encoder_rtsp_close(ctx);
        return NULL;
    }

    ctx->frame->format = ctx->codec_ctx->pix_fmt;
    ctx->frame->width = ctx->width;
    ctx->frame->height = ctx->height;
    if (av_frame_get_buffer(ctx->frame, 32) < 0) {
        encoder_rtsp_close(ctx);
        return NULL;
    }

    ctx->sws = sws_getContext(ctx->width, ctx->height, AV_PIX_FMT_BGR24,
        ctx->width, ctx->height, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!ctx->sws) {
        encoder_rtsp_close(ctx);
        return NULL;
    }

    if (open_output(ctx) < 0)
        elog("RTSP output not ready yet; will retry on frames");

    return ctx;
}

static int open_output(EncoderRtspCtx *ctx) {
    int ret;
    AVDictionary *opts = NULL;

    close_output(ctx);

    ret = avformat_alloc_output_context2(&ctx->fmt_ctx, NULL, "rtsp", ctx->url);
    if (ret < 0 || !ctx->fmt_ctx)
        return ret < 0 ? ret : AVERROR_UNKNOWN;

    ctx->stream = avformat_new_stream(ctx->fmt_ctx, NULL);
    if (!ctx->stream)
        return AVERROR(ENOMEM);

    ctx->stream->time_base = ctx->codec_ctx->time_base;
    ret = avcodec_parameters_from_context(ctx->stream->codecpar, ctx->codec_ctx);
    if (ret < 0)
        return ret;

    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    if (!(ctx->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&ctx->fmt_ctx->pb, ctx->url, AVIO_FLAG_WRITE, NULL, &opts);
        if (ret < 0) {
            av_dict_free(&opts);
            close_output(ctx);
            return ret;
        }
    }

    ret = avformat_write_header(ctx->fmt_ctx, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        close_output(ctx);
        return ret;
    }

    ctx->output_open = 1;
    ilog("publishing RTSP to %s", ctx->url);
    return 0;
}

static int write_packet(EncoderRtspCtx *ctx, AVPacket *packet) {
    int ret;

    if (!ctx->output_open && open_output(ctx) < 0)
        return -1;

    av_packet_rescale_ts(packet, ctx->codec_ctx->time_base, ctx->stream->time_base);
    packet->stream_index = ctx->stream->index;

    ret = av_interleaved_write_frame(ctx->fmt_ctx, packet);
    if (ret < 0) {
        elog("RTSP write failed; reconnecting");
        close_output(ctx);
        if (open_output(ctx) == 0) {
            ret = av_interleaved_write_frame(ctx->fmt_ctx, packet);
        }
    }

    return ret;
}

int encoder_rtsp_encode_frame(EncoderRtspCtx *ctx, const uint8_t *bgr_data, int64_t pts) {
    int ret;

    if (!ctx || !bgr_data)
        return -1;

    const uint8_t *src_slices[1] = { bgr_data };
    int src_stride[1] = { ctx->width * 3 };

    ret = av_frame_make_writable(ctx->frame);
    if (ret < 0)
        return ret;

    sws_scale(ctx->sws, src_slices, src_stride, 0, ctx->height,
        ctx->frame->data, ctx->frame->linesize);

    int64_t frame_pts = pts >= 0 ? av_rescale_q(pts, (AVRational){1, 1000000}, ctx->codec_ctx->time_base) : ctx->next_pts;
    if (frame_pts < ctx->next_pts)
        frame_pts = ctx->next_pts;
    ctx->frame->pts = frame_pts;
    ctx->next_pts = frame_pts + 1;

    ret = avcodec_send_frame(ctx->codec_ctx, ctx->frame);
    if (ret < 0)
        return ret;

    while ((ret = avcodec_receive_packet(ctx->codec_ctx, ctx->packet)) == 0) {
        int write_ret = write_packet(ctx, ctx->packet);
        av_packet_unref(ctx->packet);
        if (write_ret < 0)
            return write_ret;
    }

    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

void encoder_rtsp_close(EncoderRtspCtx *ctx) {
    if (!ctx)
        return;

    if (ctx->codec_ctx) {
        avcodec_send_frame(ctx->codec_ctx, NULL);
        while (ctx->packet && avcodec_receive_packet(ctx->codec_ctx, ctx->packet) == 0) {
            if (ctx->output_open)
                write_packet(ctx, ctx->packet);
            av_packet_unref(ctx->packet);
        }
    }

    close_output(ctx);
    if (ctx->sws)
        sws_freeContext(ctx->sws);
    if (ctx->frame)
        av_frame_free(&ctx->frame);
    if (ctx->packet)
        av_packet_free(&ctx->packet);
    if (ctx->codec_ctx)
        avcodec_free_context(&ctx->codec_ctx);
    free(ctx->url);
    free(ctx);
}
