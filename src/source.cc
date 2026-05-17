/*
Copyright (C) 2022 DEV47APPS, github.com/dev47apps

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.
*/
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "plugin.h"
#include "source.h"
#include "plugin_properties.h"
#include "ffmpeg_decode.h"
#include "mjpeg_decode.h"
#include "net.h"
#include "buffer_util.h"
#include "device_discovery.h"
#include "encoder_rtsp.h"

#define FPS 25
#define MILLI_SEC 1000

extern char os_name_version[64];
extern const char* bindIP;

#define SOURCE_EXISTS() (!plugin->stop_requested)

struct droidcam_obs_source {
    AdbMgr adbMgr;
    USBMux iosMgr;
    MDNS mdnsMgr;
    Decoder* video_decoder;
    pthread_t video_thread;
    pthread_t video_decode_thread;
    volatile bool stop_requested;
    volatile bool reset_requested;
    volatile bool video_running;
    bool activated;
    bool is_showing;
    bool use_hw;
    bool use_hdr;
    int video_width;
    int video_height;
    int fps;
    int usb_port;
    enum VideoFormat video_format;
    struct active_device_info device_info;
    struct obs_source_frame2 video_frame;
    EncoderRtspCtx *encoder;
    char *rtsp_url;
    uint8_t *bgr_frame;
    size_t bgr_frame_size;
    struct SwsContext *to_bgr;
    enum video_format to_bgr_format;
    int to_bgr_width;
    int to_bgr_height;

    droidcam_obs_source()
        : video_decoder(NULL),
          video_thread(0),
          video_decode_thread(0),
          stop_requested(false),
          reset_requested(false),
          video_running(false),
          activated(false),
          is_showing(false),
          use_hw(false),
          use_hdr(false),
          video_width(0),
          video_height(0),
          fps(FPS),
          usb_port(0),
          video_format(FORMAT_AVC),
          encoder(NULL),
          rtsp_url(NULL),
          bgr_frame(NULL),
          bgr_frame_size(0),
          to_bgr(NULL),
          to_bgr_format(VIDEO_FORMAT_NONE),
          to_bgr_width(0),
          to_bgr_height(0) {
        memset(&device_info, 0, sizeof(device_info));
        memset(&video_frame, 0, sizeof(video_frame));
    }
};

static socket_t connect_device(struct droidcam_obs_source *plugin) {
    Device* dev;
    AdbMgr* adbMgr = &plugin->adbMgr;
    USBMux* iosMgr = &plugin->iosMgr;
    MDNS  *mdnsMgr = &plugin->mdnsMgr;

    struct active_device_info *device_info = &plugin->device_info;

    dlog("connect device: id=%s type=%d", device_info->id, (int) device_info->type);

    if (device_info->type == WIFI) {
        return net_connect(device_info->ip, bindIP, device_info->port);
    }

    if (device_info->type == MDNS) {
        dev = mdnsMgr->GetDevice(device_info->id);
        if (dev) {
            return net_connect(dev->address, bindIP, device_info->port);
        }

        mdnsMgr->Reload();
        goto out;
    }

    if (device_info->type == ADB) {
        dev = adbMgr->GetDevice(device_info->id);
        if (dev) {
            if (adbMgr->DeviceOffline(dev)) {
                elog("device is offline");
                goto out;
            }

            int port_start = device_info->port + (adbMgr->Iter() * 10);
            int port_last = port_start + 8;

            if (plugin->usb_port < port_start) {
                plugin->usb_port = port_start;
            } else if (plugin->usb_port > port_last) {
                plugin->usb_port = port_start;
                adbMgr->ClearForwards(port_start, port_last);
            }

            ilog("ADB: mapping %d -> %d [%s]", plugin->usb_port, device_info->port, device_info->id);
            if (!adbMgr->AddForward(dev, plugin->usb_port, device_info->port)) {
                plugin->usb_port++;
                goto out;
            }

            socket_t rc = net_connect(localhost_ip, plugin->usb_port);
            if (rc != INVALID_SOCKET) return rc;

            adbMgr->ClearForwards(port_start, port_last);
            goto out;
        }

        adbMgr->Reload();
        goto out;
    }

    if (device_info->type == IOS) {
        dev = iosMgr->GetDevice(device_info->id);
        if (dev) {
            return iosMgr->Connect(dev, device_info->port, &plugin->usb_port);
        }

        iosMgr->Reload();
        goto out;
    }

out:
    return INVALID_SOCKET;
}

#define MAXCONFIG 1024
#define MAXPACKET 1024 * 1024 * 16
static DataPacket*
read_frame(Decoder *decoder, socket_t sock, int *has_config)
{
    uint8_t header[HEADER_SIZE];
    uint8_t config[MAXCONFIG];
    size_t r;
    size_t len, config_len = 0;
    uint64_t pts;

AGAIN:
    r = net_recv_all(sock, header, HEADER_SIZE);
    if (r != HEADER_SIZE) {
        elog("read header timeout/error: got %ld expected %ld", r, (long)HEADER_SIZE);
        return NULL;
    }

    pts = buffer_read64be(header);
    len = buffer_read32be(&header[8]);

    if (len == NO_PTS) {
        elog("stop/error from app side");
        return NULL;
    }

    if (len == 0 || len > MAXPACKET) {
        elog("read_frame: packet too large or empty at len=%ld", len);
        return NULL;
    }

    if (pts == NO_PTS) {
        if (config_len != 0) {
             elog("double config");
             return NULL;
        }

        if (len > MAXCONFIG) {
            elog("read_frame: config packet too large at %ld", len);
            return NULL;
        }

        r = net_recv_all(sock, config, len);
        if (r != len) {
            elog("read_frame: config timeout/error: got %ld expected %ld", r, len);
            return NULL;
        }

        ilog("read_frame: got config: %ld", len);
        config_len = len;
        *has_config = 1;
        goto AGAIN;
    }

    DataPacket* data_packet = decoder->pull_empty_packet(config_len + len);
    uint8_t *p = data_packet->data;
    if (config_len) {
        memcpy(p, config, config_len);
        p += config_len;
    }

    r = net_recv_all(sock, p, len);
    if (r != len) {
        elog("read_frame: timeout/error: read %ld bytes wanted %ld", r, len);
        decoder->push_empty_packet(data_packet);
        return NULL;
    }

    data_packet->pts = pts;
    data_packet->used = config_len + len;
    return data_packet;
}

static enum AVPixelFormat frame_pix_fmt(enum video_format format) {
    switch (format) {
        case VIDEO_FORMAT_I420: return AV_PIX_FMT_YUV420P;
        case VIDEO_FORMAT_NV12: return AV_PIX_FMT_NV12;
        case VIDEO_FORMAT_YUY2: return AV_PIX_FMT_YUYV422;
        case VIDEO_FORMAT_UYVY: return AV_PIX_FMT_UYVY422;
        case VIDEO_FORMAT_I422: return AV_PIX_FMT_YUV422P;
        case VIDEO_FORMAT_I010: return AV_PIX_FMT_YUV420P10LE;
        case VIDEO_FORMAT_P010: return AV_PIX_FMT_P010LE;
        case VIDEO_FORMAT_I210: return AV_PIX_FMT_YUV422P10LE;
        default: return AV_PIX_FMT_NONE;
    }
}

static bool frame_to_bgr(droidcam_obs_source *plugin, const struct obs_source_frame2 *frame) {
    enum AVPixelFormat src_fmt = frame_pix_fmt(frame->format);
    if (src_fmt == AV_PIX_FMT_NONE)
        return false;

    size_t needed = (size_t)frame->width * frame->height * 3;
    if (plugin->bgr_frame_size < needed) {
        plugin->bgr_frame = (uint8_t *)brealloc(plugin->bgr_frame, needed);
        plugin->bgr_frame_size = needed;
    }

    if (!plugin->bgr_frame)
        return false;

    if (!plugin->to_bgr || plugin->to_bgr_format != frame->format
            || plugin->to_bgr_width != (int)frame->width
            || plugin->to_bgr_height != (int)frame->height) {
        if (plugin->to_bgr)
            sws_freeContext(plugin->to_bgr);

        plugin->to_bgr = sws_getContext(frame->width, frame->height, src_fmt,
            frame->width, frame->height, AV_PIX_FMT_BGR24,
            SWS_FAST_BILINEAR, NULL, NULL, NULL);
        plugin->to_bgr_format = frame->format;
        plugin->to_bgr_width = frame->width;
        plugin->to_bgr_height = frame->height;
    }

    if (!plugin->to_bgr)
        return false;

    uint8_t *dst_data[4] = { plugin->bgr_frame, NULL, NULL, NULL };
    int dst_linesize[4] = { (int)frame->width * 3, 0, 0, 0 };
    const uint8_t *src_data[4] = {
        frame->data[0], frame->data[1], frame->data[2], frame->data[3]
    };
    int src_linesize[4] = {
        (int)frame->linesize[0], (int)frame->linesize[1],
        (int)frame->linesize[2], (int)frame->linesize[3]
    };

    return sws_scale(plugin->to_bgr, src_data, src_linesize, 0, frame->height,
        dst_data, dst_linesize) > 0;
}

static bool push_video_frame(droidcam_obs_source *plugin, const struct obs_source_frame2 *frame, uint64_t pts) {
    if (!plugin->encoder || plugin->video_width != (int)frame->width || plugin->video_height != (int)frame->height) {
        if (plugin->encoder)
            encoder_rtsp_close(plugin->encoder);

        plugin->video_width = frame->width;
        plugin->video_height = frame->height;
        plugin->encoder = encoder_rtsp_init(plugin->rtsp_url, plugin->video_width, plugin->video_height, plugin->fps);
        if (!plugin->encoder)
            return false;
    }

    if (!frame_to_bgr(plugin, frame))
        return false;

    return encoder_rtsp_encode_frame(plugin->encoder, plugin->bgr_frame, (int64_t)pts) >= 0;
}

static void *video_decode_thread(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);

    Decoder *decoder = NULL;
    DataPacket* data_packet = NULL;
    bool got_output;

    ilog("video_decode_thread start");

    while (SOURCE_EXISTS()) {
        if ((decoder = plugin->video_decoder) == NULL || (data_packet = decoder->pull_ready_packet()) == NULL) {
            os_sleep_ms(5);
            continue;
        }

        if (decoder->failed)
            goto LOOP;

        if (!decoder->decode_video(&plugin->video_frame, data_packet, &got_output)) {
            elog("error decoding video");
            decoder->failed = true;
            goto LOOP;
        }

        if (got_output) {
            plugin->video_frame.timestamp = data_packet->pts * 1000;
            if (!push_video_frame(plugin, &plugin->video_frame, data_packet->pts)) {
                elog("error pushing encoded RTSP frame");
            }
        }

LOOP:
        decoder->push_empty_packet(data_packet);
    }

    ilog("video_decode_thread end");
    return NULL;
}

static bool
recv_video_frame(droidcam_obs_source *plugin, socket_t sock) {
    int has_config = 0;
    DataPacket* data_packet;
    Decoder *decoder = plugin->video_decoder;

    if (!decoder) {
        if (plugin->video_format == FORMAT_MJPG) {
            decoder = new MJpegDecoder();
        } else {
            decoder = new FFMpegDecoder();
        }
        plugin->video_decoder = decoder;
    }

    data_packet = read_frame(decoder, sock, &has_config);
    if (!data_packet)
        return false;

    if (decoder->failed) {
FAILED:
        dlog("discarding frame; decoder failed");
        decoder->push_empty_packet(data_packet);
        return true;
    }

    if (!decoder->ready) {
        bool init = false;
        bool use_hw = plugin->use_hw;
        dlog("init video decoder");

        if (plugin->video_format == FORMAT_AVC) {
            init = (((FFMpegDecoder*)decoder)->init(NULL, AV_CODEC_ID_H264, use_hw) >= 0);
        } else if (plugin->video_format == FORMAT_HEVC) {
            init = (((FFMpegDecoder*)decoder)->init(NULL, AV_CODEC_ID_H265, use_hw) >= 0);
        } else if (plugin->video_format == FORMAT_MJPG) {
            init = ((MJpegDecoder*)decoder)->init();
        } else {
            init = false;
        }

        plugin->video_frame.format = VIDEO_FORMAT_NONE;
        plugin->video_frame.range  = VIDEO_RANGE_DEFAULT;
        if (!init) {
            elog("could not initialize decoder");
            decoder->failed = true;
            goto FAILED;
        }
        ilog("decoder initialized");
    }

    decoder->push_ready_packet(data_packet);
    return true;
}

static void release_decoder(droidcam_obs_source *plugin) {
    if (!plugin->video_decoder)
        return;

    while (plugin->video_decoder->recieveQueue.items.size() < plugin->video_decoder->alloc_count
            && SOURCE_EXISTS()) {
        dlog("waiting for decode thread: %lu/%lu",
            plugin->video_decoder->recieveQueue.items.size(),
            plugin->video_decoder->alloc_count);
        os_sleep_ms(MILLI_SEC / FPS);
    }

    dlog("release video_decoder");
    delete plugin->video_decoder;
    plugin->video_decoder = NULL;
}

static void *video_thread(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    const char *client_version_str = PLUGIN_VERSION_STR;
    const char *obs_version_str = "standalone";
    socket_t sock = INVALID_SOCKET;
    char video_req[256];
    int video_req_len = 0;

    ilog("video_thread start");

    if (plugin->activated) {
        switch (plugin->device_info.type) {
            case MDNS:
                plugin->mdnsMgr.Reload();
                plugin->mdnsMgr.ResetIter();
                break;
            case ADB:
                plugin->adbMgr.Reload();
                plugin->adbMgr.ResetIter();
                break;
            case IOS:
                plugin->iosMgr.Reload();
                plugin->iosMgr.ResetIter();
                break;
            case WIFI:
            case NONE:
                break;
        }
    }

    while (SOURCE_EXISTS()) {
        if (plugin->activated && plugin->is_showing) {
            if (plugin->video_running) {
                if (!plugin->reset_requested && recv_video_frame(plugin, sock))
                    continue;

                plugin->reset_requested = false;
                plugin->video_running = false;
                dlog("closing failed video socket %d", sock);
                net_close(sock);
                sock = INVALID_SOCKET;
                goto SLOW_LOOP;
            }

            if ((sock = connect_device(plugin)) == INVALID_SOCKET)
                goto SLOW_LOOP;

            video_req_len = snprintf(video_req, sizeof(video_req), VIDEO_REQ,
                VideoFormatNames[plugin->video_format][1],
                plugin->video_width, plugin->video_height,
                plugin->usb_port,
                os_name_version,
                obs_version_str, client_version_str,
                (plugin->use_hdr && plugin->video_format == FORMAT_HEVC),
                5912);

            dlog("%s", video_req);
            if (net_send_all(sock, video_req, video_req_len) <= 0) {
                elog("send(/video) failed");
                net_close(sock);
                sock = INVALID_SOCKET;

SLOW_LOOP:
                os_sleep_ms(MILLI_SEC * 2);
                goto LOOP;
            }

            set_recv_buf_len(sock, 65536 * 4);
            plugin->video_running = true;
            dlog("starting video via socket %d", sock);
            continue;
        }

        video_req_len = 0;

LOOP:
        if (plugin->video_running) {
            plugin->video_running = false;
        }

        if (sock != INVALID_SOCKET) {
            dlog("closing active video socket %d", sock);
            net_close(sock);
            sock = INVALID_SOCKET;
        }

        release_decoder(plugin);
        os_sleep_ms(MILLI_SEC / FPS);
    }

    ilog("video_thread end");
    plugin->video_running = false;
    if (sock != INVALID_SOCKET) net_close(sock);
    return NULL;
}

void *droidcam_source_start(const struct droidcam_source_config *config) {
    droidcam_obs_source *plugin = new droidcam_obs_source();

    plugin->activated = true;
    plugin->is_showing = true;
    plugin->use_hw = config->use_hw;
    plugin->use_hdr = config->use_hdr;
    plugin->video_width = config->width;
    plugin->video_height = config->height;
    plugin->fps = config->fps > 0 ? config->fps : FPS;
    plugin->video_format = config->video_format;
    plugin->device_info.type = WIFI;
    plugin->device_info.id = config->device_id ? config->device_id : config->device_ip;
    plugin->device_info.ip = config->device_ip;
    plugin->device_info.port = config->port;
    plugin->rtsp_url = strdup(config->rtsp_url);

    ilog("Source: video_format=%s video_resolution=%dx%d rtsp=%s",
        VideoFormatNames[plugin->video_format][1],
        plugin->video_width, plugin->video_height, plugin->rtsp_url);

    if (pthread_create(&plugin->video_thread, NULL, video_thread, plugin) != 0) {
        droidcam_source_stop(plugin);
        return NULL;
    }

    if (pthread_create(&plugin->video_decode_thread, NULL, video_decode_thread, plugin) != 0) {
        droidcam_source_stop(plugin);
        return NULL;
    }

    return plugin;
}

void droidcam_source_stop(void *data) {
    droidcam_obs_source *plugin = (droidcam_obs_source*)(data);
    if (!plugin)
        return;

    ilog("stopping");
    plugin->stop_requested = true;

    if (plugin->video_thread)
        pthread_join(plugin->video_thread, NULL);
    if (plugin->video_decode_thread)
        pthread_join(plugin->video_decode_thread, NULL);

    release_decoder(plugin);

    if (plugin->encoder)
        encoder_rtsp_close(plugin->encoder);
    if (plugin->to_bgr)
        sws_freeContext(plugin->to_bgr);
    if (plugin->bgr_frame)
        bfree(plugin->bgr_frame);
    free(plugin->rtsp_url);
    delete plugin;
}
