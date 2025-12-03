/*
 * decode.c — Video decoder with VA-API hardware acceleration
 *
 * Supports Intel, AMD (via Mesa), and NVIDIA (via nvidia-vaapi-driver).
 * On Intel/AMD, frames can be exported as DMA-BUF for zero-copy rendering.
 * On NVIDIA, DMA-BUF export works but import usually fails due to tiled
 * modifiers, so we fall back to CPU readback.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glob.h>
#include <ctype.h>

#include "wlvideo.h"
#include "config.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <drm_fourcc.h>

#ifdef HAVE_VAAPI
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <fcntl.h>
#endif

#ifdef HAVE_CUDA
#include <libavutil/hwcontext_cuda.h>
#endif

/* Convert DRM fourcc to printable string. Thread-safe via thread-local storage. */
const char *fourcc_to_str(uint32_t f) {
    static __thread char buf[5];
    buf[0] = f & 0xFF;
    buf[1] = (f >> 8) & 0xFF;
    buf[2] = (f >> 16) & 0xFF;
    buf[3] = (f >> 24) & 0xFF;
    buf[4] = 0;
    for (int i = 0; i < 4; i++)
        if (buf[i] < 32 || buf[i] > 126) buf[i] = '?';
    return buf;
}

struct Decoder {
    AVFormatContext *fmt_ctx;
    AVCodecContext *codec_ctx;
    AVBufferRef *hw_ctx;

    int stream_idx;
    AVRational time_base;
    double frame_duration;

    AVFrame *frame;
    AVFrame *sw_frame;
    AVPacket *packet;

    enum AVHWDeviceType hw_type;
    bool hw_active;
    bool eof;

    int current_ring_slot;

    ColorSpace colorspace;
    ColorRange color_range;
    GpuVendor gpu_vendor;

    bool dmabuf_export_tested;
    bool dmabuf_export_works;

    uint64_t surface_generation;
    enum AVCodecID codec_id;
    int bit_depth;
};

/* GPU vendor detection from sysfs */
static GpuVendor vendor_from_sysfs(const char *render_node) {
    if (!render_node) return GPU_VENDOR_UNKNOWN;

    const char *name = strrchr(render_node, '/');
    name = name ? name + 1 : render_node;

    char path[256];
    snprintf(path, sizeof(path), "/sys/class/drm/%s/device/vendor", name);

    FILE *f = fopen(path, "r");
    if (!f) return GPU_VENDOR_UNKNOWN;

    unsigned int vid = 0;
    char buf[32];
    if (fgets(buf, sizeof(buf), f))
        sscanf(buf, "0x%x", &vid);
    fclose(f);

    switch (vid) {
    case 0x8086: return GPU_VENDOR_INTEL;
    case 0x1002: return GPU_VENDOR_AMD;
    case 0x10de: return GPU_VENDOR_NVIDIA;
    default: return GPU_VENDOR_UNKNOWN;
    }
}

#ifdef HAVE_VAAPI
/* GPU vendor detection from VA-API driver string */
static GpuVendor vendor_from_vaapi(VADisplay dpy) {
    const char *str = vaQueryVendorString(dpy);
    if (!str) return GPU_VENDOR_UNKNOWN;

    /* Case-insensitive search */
    char lower[256];
    size_t len = strlen(str);
    if (len >= sizeof(lower)) len = sizeof(lower) - 1;
    for (size_t i = 0; i < len; i++)
        lower[i] = tolower((unsigned char)str[i]);
    lower[len] = '\0';

    if (strstr(lower, "intel")) return GPU_VENDOR_INTEL;
    if (strstr(lower, "amd") || strstr(lower, "radeon")) return GPU_VENDOR_AMD;
    if (strstr(lower, "nvidia") || strstr(lower, "nvdec")) return GPU_VENDOR_NVIDIA;
    return GPU_VENDOR_UNKNOWN;
}
#endif

static const char *vendor_name(GpuVendor v) {
    switch (v) {
    case GPU_VENDOR_INTEL: return "Intel";
    case GPU_VENDOR_AMD: return "AMD";
    case GPU_VENDOR_NVIDIA: return "NVIDIA";
    default: return "Unknown";
    }
}

/* nvidia-vaapi-driver doesn't support all codecs */
static bool nvidia_supports_codec(enum AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_VP8:
    case AV_CODEC_ID_VP9:
    case AV_CODEC_ID_AV1:
    case AV_CODEC_ID_MPEG2VIDEO:
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
        return true;
    default:
        return false;
    }
}

/* Detect bit depth from stream parameters */
static int detect_bit_depth(AVCodecParameters *par) {
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(par->format);
    if (desc && desc->comp[0].depth > 0)
        return desc->comp[0].depth;

    if (par->bits_per_raw_sample > 0)
        return par->bits_per_raw_sample;

    /* Profile-based heuristics for common codecs */
    if (par->codec_id == AV_CODEC_ID_HEVC &&
        (par->profile == FF_PROFILE_HEVC_MAIN_10 || par->profile == FF_PROFILE_HEVC_REXT))
        return 10;

    if (par->codec_id == AV_CODEC_ID_VP9 && par->profile >= 2)
        return 10;

    return 8;
}

static ColorSpace detect_colorspace(const AVFrame *f, const AVCodecContext *c) {
    enum AVColorSpace cs = f->colorspace;
    if (cs == AVCOL_SPC_UNSPECIFIED && c)
        cs = c->colorspace;

    switch (cs) {
    case AVCOL_SPC_BT709: return CS_BT709;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return CS_BT2020;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG: return CS_BT601;
    default:
        /* HD content is usually BT.709, SD is BT.601 */
        return (f->width >= 1280) ? CS_BT709 : CS_BT601;
    }
}

static ColorRange detect_range(const AVFrame *f, const AVCodecContext *c) {
    enum AVColorRange cr = f->color_range;
    if (cr == AVCOL_RANGE_UNSPECIFIED && c)
        cr = c->color_range;
    return (cr == AVCOL_RANGE_JPEG) ? CR_FULL : CR_LIMITED;
}

/* Callback for FFmpeg to select hardware pixel format */
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *fmts) {
    enum AVHWDeviceType want = AV_HWDEVICE_TYPE_NONE;
    if (ctx->hw_device_ctx) {
        AVHWDeviceContext *dev = (AVHWDeviceContext *)ctx->hw_device_ctx->data;
        want = dev->type;
    }

    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
#ifdef HAVE_VAAPI
        if (*p == AV_PIX_FMT_VAAPI && want == AV_HWDEVICE_TYPE_VAAPI)
            return AV_PIX_FMT_VAAPI;
#endif
#ifdef HAVE_CUDA
        if (*p == AV_PIX_FMT_CUDA && want == AV_HWDEVICE_TYPE_CUDA)
            return AV_PIX_FMT_CUDA;
#endif
    }

    /* Fallback: prefer NV12 for efficient upload */
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
        if (*p == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
    }

    return fmts[0];
}

#ifdef HAVE_VAAPI
/* Initialize VA-API on the best available device.
 * Prefers Intel/AMD for zero-copy capability, falls back to NVIDIA. */
static int init_vaapi(Decoder *dec, const char *user_device) {
    static const char *devices[] = {
        "/dev/dri/renderD128", "/dev/dri/renderD129",
        "/dev/dri/renderD130", "/dev/dri/renderD131", NULL
    };

    const char *driver_env = getenv("LIBVA_DRIVER_NAME");
    bool want_nvidia = driver_env && strcmp(driver_env, "nvidia") == 0;

    /* User explicitly requested a device */
    if (user_device && user_device[0] && access(user_device, R_OK) == 0) {
        AVBufferRef *ctx = NULL;
        if (av_hwdevice_ctx_create(&ctx, AV_HWDEVICE_TYPE_VAAPI, user_device, NULL, 0) == 0) {
            AVHWDeviceContext *hw = (AVHWDeviceContext *)ctx->data;
            AVVAAPIDeviceContext *va = hw->hwctx;
            dec->hw_ctx = ctx;
            dec->gpu_vendor = vendor_from_vaapi(va->display);
            LOG_INFO("VA-API device %s: %s", user_device, vendor_name(dec->gpu_vendor));
            return 0;
        }
        LOG_WARN("Failed to init VA-API on %s, trying auto-detect", user_device);
    }

    /* Scan available devices, prefer Intel/AMD for zero-copy */
    AVBufferRef *nvidia_ctx = NULL;
    GpuVendor nvidia_vendor = GPU_VENDOR_UNKNOWN;

    for (const char **dev = devices; *dev; dev++) {
        if (access(*dev, R_OK) != 0) continue;

        AVBufferRef *ctx = NULL;
        if (av_hwdevice_ctx_create(&ctx, AV_HWDEVICE_TYPE_VAAPI, *dev, NULL, 0) != 0)
            continue;

        AVHWDeviceContext *hw = (AVHWDeviceContext *)ctx->data;
        AVVAAPIDeviceContext *va = hw->hwctx;
        GpuVendor vendor = vendor_from_vaapi(va->display);

        LOG_DEBUG("Found VA-API device %s: %s", *dev, vendor_name(vendor));

        if (vendor == GPU_VENDOR_NVIDIA) {
            if (want_nvidia) {
                dec->hw_ctx = ctx;
                dec->gpu_vendor = vendor;
                LOG_INFO("VA-API: using NVIDIA (requested via LIBVA_DRIVER_NAME)");
                av_buffer_unref(&nvidia_ctx);
                return 0;
            }
            /* Save as fallback */
            if (!nvidia_ctx) {
                nvidia_ctx = ctx;
                nvidia_vendor = vendor;
            } else {
                av_buffer_unref(&ctx);
            }
            continue;
        }

        /* Intel or AMD — use it for zero-copy */
        dec->hw_ctx = ctx;
        dec->gpu_vendor = vendor;
        LOG_INFO("VA-API device %s: %s (zero-copy capable)", *dev, vendor_name(vendor));
        av_buffer_unref(&nvidia_ctx);
        return 0;
    }

    /* No Intel/AMD found, use NVIDIA if available */
    if (nvidia_ctx) {
        dec->hw_ctx = nvidia_ctx;
        dec->gpu_vendor = nvidia_vendor;
        LOG_INFO("VA-API: using NVIDIA (no Intel/AMD found)");
        return 0;
    }

    return -1;
}
#endif

#ifdef HAVE_CUDA
static int init_cuda(Decoder *dec) {
    if (av_hwdevice_ctx_create(&dec->hw_ctx, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0) == 0) {
        LOG_INFO("CUDA/NVDEC initialized");
        dec->gpu_vendor = GPU_VENDOR_NVIDIA;
        return 0;
    }
    return -1;
}
#endif

int decoder_init(Decoder **out, const char *path, bool hw_accel, const char *gpu_device) {
    Decoder *dec = calloc(1, sizeof(Decoder));
    if (!dec) return -1;

    int ret;

    ret = avformat_open_input(&dec->fmt_ctx, path, NULL, NULL);
    if (ret < 0) {
        LOG_ERROR("Cannot open %s: %s", path, av_err2str(ret));
        goto fail;
    }

    ret = avformat_find_stream_info(dec->fmt_ctx, NULL);
    if (ret < 0) {
        LOG_ERROR("Cannot find stream info: %s", av_err2str(ret));
        goto fail;
    }

    /* Find video stream */
    dec->stream_idx = -1;
    for (unsigned i = 0; i < dec->fmt_ctx->nb_streams; i++) {
        if (dec->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            dec->stream_idx = i;
            break;
        }
    }
    if (dec->stream_idx < 0) {
        LOG_ERROR("No video stream found");
        goto fail;
    }

    AVStream *st = dec->fmt_ctx->streams[dec->stream_idx];
    dec->time_base = st->time_base;
    dec->codec_id = st->codecpar->codec_id;

    /* Frame duration from stream metadata */
    if (st->avg_frame_rate.num > 0)
        dec->frame_duration = av_q2d(av_inv_q(st->avg_frame_rate));
    else if (st->r_frame_rate.num > 0)
        dec->frame_duration = av_q2d(av_inv_q(st->r_frame_rate));
    else
        dec->frame_duration = 1.0 / 30.0;

    /* Clamp to sane values */
    if (dec->frame_duration < 1.0/240.0) dec->frame_duration = 1.0/240.0;
    if (dec->frame_duration > 1.0) dec->frame_duration = 1.0;

    dec->bit_depth = detect_bit_depth(st->codecpar);
    if (dec->bit_depth > 8)
        LOG_INFO("Video is %d-bit", dec->bit_depth);

    /* Find a decoder that supports hardware acceleration */
    const AVCodec *codec = NULL;

    if (hw_accel) {
        void *iter = NULL;
        const AVCodec *c;
        while ((c = av_codec_iterate(&iter))) {
            if (c->id != st->codecpar->codec_id || !av_codec_is_decoder(c))
                continue;

            for (int i = 0;; i++) {
                const AVCodecHWConfig *cfg = avcodec_get_hw_config(c, i);
                if (!cfg) break;
                if (cfg->device_type == AV_HWDEVICE_TYPE_VAAPI &&
                    (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                    codec = c;
                    LOG_DEBUG("Found HW decoder: %s", c->name);
                    break;
                }
            }
            if (codec) break;
        }
    }

    if (!codec)
        codec = avcodec_find_decoder(st->codecpar->codec_id);

    if (!codec) {
        LOG_ERROR("No decoder for %s", avcodec_get_name(st->codecpar->codec_id));
        goto fail;
    }

    dec->codec_ctx = avcodec_alloc_context3(codec);
    if (!dec->codec_ctx) goto fail;

    ret = avcodec_parameters_to_context(dec->codec_ctx, st->codecpar);
    if (ret < 0) goto fail;

    /* Try to set up hardware acceleration */
#ifdef HAVE_VAAPI
    if (hw_accel && !dec->hw_active) {
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;

            if (cfg->device_type == AV_HWDEVICE_TYPE_VAAPI &&
                (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {

                if (init_vaapi(dec, gpu_device) == 0) {
                    /* Check nvidia-vaapi-driver limitations */
                    if (dec->gpu_vendor == GPU_VENDOR_NVIDIA) {
                        if (!nvidia_supports_codec(dec->codec_id)) {
                            LOG_WARN("nvidia-vaapi-driver doesn't support %s",
                                     avcodec_get_name(dec->codec_id));
                            av_buffer_unref(&dec->hw_ctx);
                            break;
                        }
                        if (dec->bit_depth > 8) {
                            LOG_WARN("nvidia-vaapi-driver doesn't support %d-bit",
                                     dec->bit_depth);
                            av_buffer_unref(&dec->hw_ctx);
                            break;
                        }
                    }

                    dec->codec_ctx->hw_device_ctx = av_buffer_ref(dec->hw_ctx);
                    dec->codec_ctx->get_format = get_hw_format;
                    dec->hw_type = AV_HWDEVICE_TYPE_VAAPI;
                    dec->hw_active = true;
                    LOG_INFO("Using VA-API for %s", avcodec_get_name(dec->codec_id));
                }
                break;
            }
        }
    }
#endif

#ifdef HAVE_CUDA
    if (hw_accel && !dec->hw_active) {
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;

            if (cfg->device_type == AV_HWDEVICE_TYPE_CUDA &&
                (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {

                if (init_cuda(dec) == 0) {
                    dec->codec_ctx->hw_device_ctx = av_buffer_ref(dec->hw_ctx);
                    dec->codec_ctx->get_format = get_hw_format;
                    dec->hw_type = AV_HWDEVICE_TYPE_CUDA;
                    dec->hw_active = true;
                    dec->dmabuf_export_tested = true;
                    dec->dmabuf_export_works = false;
                    LOG_INFO("Using CUDA/NVDEC");
                }
                break;
            }
        }
    }
#endif

    /* Software decode with threading */
    if (!dec->hw_active) {
        dec->codec_ctx->thread_count = 0;
        dec->codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (hw_accel)
            LOG_WARN("Hardware decode unavailable, using software");
    }

    ret = avcodec_open2(dec->codec_ctx, codec, NULL);
    if (ret < 0) {
        LOG_ERROR("Cannot open codec: %s", av_err2str(ret));

        /* Retry without hardware if it failed */
        if (dec->hw_active) {
            LOG_INFO("Retrying with software decode");
            av_buffer_unref(&dec->codec_ctx->hw_device_ctx);
            av_buffer_unref(&dec->hw_ctx);
            dec->codec_ctx->get_format = NULL;
            dec->hw_active = false;
            dec->hw_type = AV_HWDEVICE_TYPE_NONE;
            dec->codec_ctx->thread_count = 0;
            dec->codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            ret = avcodec_open2(dec->codec_ctx, codec, NULL);
        }
        if (ret < 0) goto fail;
    }

    dec->frame = av_frame_alloc();
    dec->packet = av_packet_alloc();
    if (!dec->frame || !dec->packet) goto fail;

    *out = dec;
    return 0;

fail:
    decoder_destroy(dec);
    return -1;
}

void decoder_destroy(Decoder *dec) {
    if (!dec) return;
    av_frame_free(&dec->frame);
    av_frame_free(&dec->sw_frame);
    av_packet_free(&dec->packet);
    avcodec_free_context(&dec->codec_ctx);
    av_buffer_unref(&dec->hw_ctx);
    avformat_close_input(&dec->fmt_ctx);
    free(dec);
}

#ifdef HAVE_VAAPI
/* Export VA-API surface as DMA-BUF for zero-copy rendering */
static bool export_vaapi_dmabuf(Decoder *dec, AVFrame *f, Frame *frame) {
    if (f->format != AV_PIX_FMT_VAAPI) return false;

    AVHWDeviceContext *dev = (AVHWDeviceContext *)dec->hw_ctx->data;
    AVVAAPIDeviceContext *va = dev->hwctx;
    VASurfaceID surface = (VASurfaceID)(uintptr_t)f->data[3];

    VADRMPRIMESurfaceDescriptor desc;
    VAStatus st = vaExportSurfaceHandle(
        va->display, surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &desc
    );

    if (st != VA_STATUS_SUCCESS) {
        static bool logged = false;
        if (!logged) {
            LOG_WARN("VA-API DMA-BUF export failed: %d", st);
            logged = true;
        }
        return false;
    }

    /* Log first successful export */
    static bool first = true;
    if (first) {
        LOG_DEBUG("VA-API export: %s %dx%d, %d planes, modifier 0x%llx",
                  fourcc_to_str(desc.fourcc), desc.width, desc.height,
                  desc.num_layers, (unsigned long long)desc.objects[0].drm_format_modifier);
        first = false;
    }

    frame->type = FRAME_HW;
    frame->hw.surface_id = (uintptr_t)surface;
    frame->hw.generation = dec->surface_generation++;

    DmaBuf *dmabuf = &frame->hw.dmabuf;
    memset(dmabuf, 0, sizeof(*dmabuf));
    for (int i = 0; i < 4; i++) {
        dmabuf->fd[i] = -1;
        dmabuf->modifier[i] = DRM_FORMAT_MOD_INVALID;
    }

    dmabuf->fourcc = desc.fourcc;
    dmabuf->width = desc.width;
    dmabuf->height = desc.height;
    dmabuf->num_planes = 0;

    /* Track which object FDs we've taken ownership of */
    bool taken[4] = {false};

    /* Copy plane info from VA-API descriptor */
    for (uint32_t l = 0; l < desc.num_layers && dmabuf->num_planes < 4; l++) {
        for (uint32_t p = 0; p < desc.layers[l].num_planes && dmabuf->num_planes < 4; p++) {
            int idx = dmabuf->num_planes++;
            uint32_t obj = desc.layers[l].object_index[p];

            if (obj < desc.num_objects) {
                if (!taken[obj]) {
                    dmabuf->fd[idx] = desc.objects[obj].fd;
                    taken[obj] = true;
                } else {
                    dmabuf->fd[idx] = dup(desc.objects[obj].fd);
                }
                dmabuf->modifier[idx] = desc.objects[obj].drm_format_modifier;
            }

            dmabuf->offset[idx] = desc.layers[l].offset[p];
            dmabuf->stride[idx] = desc.layers[l].pitch[p];
        }
    }

    /* Close any object FDs we didn't use */
    for (uint32_t i = 0; i < desc.num_objects; i++)
        if (!taken[i]) close(desc.objects[i].fd);

    return dmabuf->num_planes > 0;
}
#endif

/* Copy frame data to preallocated ring buffer for software rendering */
static bool extract_sw_frame(Decoder *dec, Frame *frame, SoftwareRing *ring) {
    AVFrame *src = dec->frame;

    /* Transfer from GPU to CPU if needed */
#ifdef HAVE_VAAPI
    if (src->format == AV_PIX_FMT_VAAPI) {
        if (!dec->sw_frame) dec->sw_frame = av_frame_alloc();
        av_frame_unref(dec->sw_frame);
        dec->sw_frame->format = AV_PIX_FMT_NV12;
        if (av_hwframe_transfer_data(dec->sw_frame, src, 0) < 0)
            return false;
        src = dec->sw_frame;
    }
#endif
#ifdef HAVE_CUDA
    if (src->format == AV_PIX_FMT_CUDA) {
        if (!dec->sw_frame) dec->sw_frame = av_frame_alloc();
        av_frame_unref(dec->sw_frame);
        dec->sw_frame->format = AV_PIX_FMT_NV12;
        if (av_hwframe_transfer_data(dec->sw_frame, src, 0) < 0)
            return false;
        src = dec->sw_frame;
    }
#endif

    if (src->format != AV_PIX_FMT_NV12 && src->format != AV_PIX_FMT_YUV420P) {
        LOG_ERROR("Unsupported format: %s", av_get_pix_fmt_name(src->format));
        return false;
    }

    int slot = dec->current_ring_slot;
    dec->current_ring_slot = (slot + 1) % SW_RING_SIZE;

    uint8_t *y_dst = sw_ring_get_y(ring, slot);
    uint8_t *uv_dst = sw_ring_get_uv(ring, slot);
    int w = src->width;
    int h = src->height;

    /* Copy Y plane */
    if (src->linesize[0] == ring->y_stride) {
        memcpy(y_dst, src->data[0], (size_t)ring->y_stride * h);
    } else {
        int copy_w = src->linesize[0] < ring->y_stride ? src->linesize[0] : w;
        for (int row = 0; row < h; row++)
            memcpy(y_dst + row * ring->y_stride, src->data[0] + row * src->linesize[0], copy_w);
    }

    /* Copy UV plane(s) */
    int uv_h = h / 2;
    if (src->format == AV_PIX_FMT_NV12) {
        if (src->linesize[1] == ring->uv_stride) {
            memcpy(uv_dst, src->data[1], (size_t)ring->uv_stride * uv_h);
        } else {
            int copy_w = src->linesize[1] < ring->uv_stride ? src->linesize[1] : w;
            for (int row = 0; row < uv_h; row++)
                memcpy(uv_dst + row * ring->uv_stride, src->data[1] + row * src->linesize[1], copy_w);
        }
    } else {
        /* YUV420P: interleave U and V into NV12 */
        int uv_w = w / 2;
        for (int row = 0; row < uv_h; row++) {
            const uint8_t *u = src->data[1] + row * src->linesize[1];
            const uint8_t *v = src->data[2] + row * src->linesize[2];
            uint8_t *dst = uv_dst + row * ring->uv_stride;
            for (int x = 0; x < uv_w; x++) {
                dst[x * 2] = u[x];
                dst[x * 2 + 1] = v[x];
            }
        }
    }

    frame->sw.ring_slot = slot;
    frame->sw.pixel_format = AV_PIX_FMT_NV12;
    frame->sw.available = true;

    if (frame->type != FRAME_HW)
        frame->type = FRAME_SW;

    return true;
}

bool decoder_get_frame(Decoder *dec, Frame *frame, SoftwareRing *ring, bool need_sw) {
    int ret;

    while (1) {
        ret = avcodec_receive_frame(dec->codec_ctx, dec->frame);

        if (ret == 0) {
            AVFrame *f = dec->frame;

            frame->pts = (f->pts != AV_NOPTS_VALUE) ? f->pts * av_q2d(dec->time_base) : 0.0;
            frame->width = f->width;
            frame->height = f->height;
            frame->colorspace = detect_colorspace(f, dec->codec_ctx);
            frame->color_range = detect_range(f, dec->codec_ctx);

            frame->type = FRAME_SW;
            frame->sw.available = false;

            bool hw_ok = false;

#ifdef HAVE_VAAPI
            if (f->format == AV_PIX_FMT_VAAPI &&
                (!dec->dmabuf_export_tested || dec->dmabuf_export_works)) {
                hw_ok = export_vaapi_dmabuf(dec, f, frame);
            }
#endif

            if (!hw_ok) need_sw = true;

            if (ring && need_sw) {
                if (!extract_sw_frame(dec, frame, ring)) {
                    if (!hw_ok) return false;
                }
            }

            return hw_ok || frame->sw.available;
        }

        if (ret == AVERROR_EOF) {
            dec->eof = true;
            return false;
        }

        if (ret != AVERROR(EAGAIN)) {
            LOG_ERROR("Decode error: %s", av_err2str(ret));
            return false;
        }

        /* Need more input data */
        ret = av_read_frame(dec->fmt_ctx, dec->packet);

        if (ret == AVERROR_EOF) {
            avcodec_send_packet(dec->codec_ctx, NULL);
            continue;
        }

        if (ret < 0) {
            LOG_ERROR("Read error: %s", av_err2str(ret));
            return false;
        }

        if (dec->packet->stream_index != dec->stream_idx) {
            av_packet_unref(dec->packet);
            continue;
        }

        ret = avcodec_send_packet(dec->codec_ctx, dec->packet);
        av_packet_unref(dec->packet);

        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            LOG_ERROR("Send packet error: %s", av_err2str(ret));
            return false;
        }
    }
}

int decoder_seek_start(Decoder *dec) {
    int ret = av_seek_frame(dec->fmt_ctx, dec->stream_idx, 0, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        ret = avio_seek(dec->fmt_ctx->pb, 0, SEEK_SET);
        if (ret < 0) return -1;
    }
    avcodec_flush_buffers(dec->codec_ctx);
    dec->eof = false;
    dec->surface_generation += 100;
    return 0;
}

void decoder_get_info(Decoder *dec, int *w, int *h, double *fps, bool *hw) {
    if (w) *w = dec->codec_ctx->width;
    if (h) *h = dec->codec_ctx->height;
    if (fps) *fps = 1.0 / dec->frame_duration;
    if (hw) *hw = dec->hw_active;
}

GpuVendor decoder_get_gpu_vendor(Decoder *dec) {
    return dec ? dec->gpu_vendor : GPU_VENDOR_UNKNOWN;
}

bool decoder_dmabuf_export_supported(Decoder *dec) {
    if (!dec) return false;
    if (!dec->dmabuf_export_tested) return true;
    return dec->dmabuf_export_works;
}

void decoder_close_dmabuf(DmaBuf *dmabuf) {
    for (int i = 0; i < 4; i++) {
        if (dmabuf->fd[i] >= 0) {
            close(dmabuf->fd[i]);
            dmabuf->fd[i] = -1;
        }
    }
}

void decoder_set_dmabuf_export_result(Decoder *dec, bool works) {
    if (!dec) return;
    dec->dmabuf_export_tested = true;
    dec->dmabuf_export_works = works;
}

int sw_ring_init(SoftwareRing *ring, int width, int height) {
    ring->width = width;
    ring->height = height;
    ring->y_stride = (width + 63) & ~63;
    ring->uv_stride = ring->y_stride;

    size_t y_size = (size_t)ring->y_stride * height;
    size_t uv_size = (size_t)ring->uv_stride * (height / 2);
    ring->slot_size = y_size + uv_size;

    ring->data = aligned_alloc(64, ring->slot_size * SW_RING_SIZE);
    if (!ring->data) {
        LOG_ERROR("Failed to allocate ring buffer (%zu KiB)", ring->slot_size * SW_RING_SIZE / 1024);
        return -1;
    }

    LOG_INFO("Ring buffer: %d×%d, %zu KiB/slot", width, height, ring->slot_size / 1024);
    return 0;
}

void sw_ring_destroy(SoftwareRing *ring) {
    free(ring->data);
    ring->data = NULL;
}

uint8_t *sw_ring_get_y(SoftwareRing *ring, int slot) {
    return ring->data + slot * ring->slot_size;
}

uint8_t *sw_ring_get_uv(SoftwareRing *ring, int slot) {
    return ring->data + slot * ring->slot_size + (size_t)ring->y_stride * ring->height;
}