/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"
#include "config_components.h"

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/hwcontext.h"
#if CONFIG_D3D11VA
#include "libavutil/hwcontext_d3d11va.h"
#endif
#if CONFIG_DXVA2
#define COBJMACROS
#include "libavutil/hwcontext_dxva2.h"
#endif
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#include "amfenc.h"
#include "encode.h"
#include "internal.h"
#include "libavutil/mastering_display_metadata.h"

static int amf_save_hdr_metadata(AVCodecContext *avctx, const AVFrame *frame, AMFHDRMetadata *hdrmeta)
{
    AVFrameSideData            *sd_display;
    AVFrameSideData            *sd_light;
    AVMasteringDisplayMetadata *display_meta;
    AVContentLightMetadata     *light_meta;

    sd_display = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (sd_display) {
        display_meta = (AVMasteringDisplayMetadata *)sd_display->data;
        if (display_meta->has_luminance) {
            const unsigned int luma_den = 10000;
            hdrmeta->maxMasteringLuminance =
                (amf_uint32)(luma_den * av_q2d(display_meta->max_luminance));
            hdrmeta->minMasteringLuminance =
                FFMIN((amf_uint32)(luma_den * av_q2d(display_meta->min_luminance)), hdrmeta->maxMasteringLuminance);
        }
        if (display_meta->has_primaries) {
            const unsigned int chroma_den = 50000;
            hdrmeta->redPrimary[0] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[0][0])), chroma_den);
            hdrmeta->redPrimary[1] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[0][1])), chroma_den);
            hdrmeta->greenPrimary[0] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[1][0])), chroma_den);
            hdrmeta->greenPrimary[1] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[1][1])), chroma_den);
            hdrmeta->bluePrimary[0] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[2][0])), chroma_den);
            hdrmeta->bluePrimary[1] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->display_primaries[2][1])), chroma_den);
            hdrmeta->whitePoint[0] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->white_point[0])), chroma_den);
            hdrmeta->whitePoint[1] =
                FFMIN((amf_uint16)(chroma_den * av_q2d(display_meta->white_point[1])), chroma_den);
        }

        sd_light = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
        if (sd_light) {
            light_meta = (AVContentLightMetadata *)sd_light->data;
            if (light_meta) {
                hdrmeta->maxContentLightLevel = (amf_uint16)light_meta->MaxCLL;
                hdrmeta->maxFrameAverageLightLevel = (amf_uint16)light_meta->MaxFALL;
            }
        }
        return 0;
    }
    return 1;
}

#if CONFIG_D3D11VA
#include <d3d11.h>
#endif

#ifdef _WIN32
#include "compat/w32dlfcn.h"
#else
#include <dlfcn.h>
#endif

#define FFMPEG_AMF_WRITER_ID L"ffmpeg_amf"

#define PTS_PROP L"PtsProp"

const enum AVPixelFormat ff_amf_pix_fmts[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV420P,
#if CONFIG_D3D11VA
    AV_PIX_FMT_D3D11,
#endif
#if CONFIG_DXVA2
    AV_PIX_FMT_DXVA2_VLD,
#endif
    AV_PIX_FMT_P010,
    AV_PIX_FMT_NONE
};

typedef struct FormatMap {
    enum AVPixelFormat       av_format;
    enum AMF_SURFACE_FORMAT  amf_format;
} FormatMap;

static const FormatMap format_map[] =
{
    { AV_PIX_FMT_NONE,       AMF_SURFACE_UNKNOWN },
    { AV_PIX_FMT_NV12,       AMF_SURFACE_NV12 },
    { AV_PIX_FMT_P010,       AMF_SURFACE_P010 },
    { AV_PIX_FMT_BGR0,       AMF_SURFACE_BGRA },
    { AV_PIX_FMT_RGB0,       AMF_SURFACE_RGBA },
    { AV_PIX_FMT_GRAY8,      AMF_SURFACE_GRAY8 },
    { AV_PIX_FMT_YUV420P,    AMF_SURFACE_YUV420P },
    { AV_PIX_FMT_YUYV422,    AMF_SURFACE_YUY2 },
};

static enum AMF_SURFACE_FORMAT amf_av_to_amf_format(enum AVPixelFormat fmt)
{
    int i;
    for (i = 0; i < amf_countof(format_map); i++) {
        if (format_map[i].av_format == fmt) {
            return format_map[i].amf_format;
        }
    }
    return AMF_SURFACE_UNKNOWN;
}

static void AMF_CDECL_CALL AMFTraceWriter_Write(AMFTraceWriter *pThis,
    const wchar_t *scope, const wchar_t *message)
{
    AmfTraceWriter *tracer = (AmfTraceWriter*)pThis;
    av_log(tracer->avctx, AV_LOG_DEBUG, "%ls: %ls", scope, message); // \n is provided from AMF
}

static void AMF_CDECL_CALL AMFTraceWriter_Flush(AMFTraceWriter *pThis)
{
}

static AMFTraceWriterVtbl tracer_vtbl =
{
    .Write = AMFTraceWriter_Write,
    .Flush = AMFTraceWriter_Flush,
};

static int amf_load_library(AVCodecContext *avctx)
{
    AmfContext        *ctx = avctx->priv_data;
    AMFInit_Fn         init_fun;
    AMFQueryVersion_Fn version_fun;
    AMF_RESULT         res;

    ctx->delayed_frame = av_frame_alloc();
    if (!ctx->delayed_frame) {
        return AVERROR(ENOMEM);
    }
    // hardcoded to current HW queue size - will auto-realloc if too small
    ctx->timestamp_list = av_fifo_alloc2(avctx->max_b_frames + 16, sizeof(int64_t),
                                         AV_FIFO_FLAG_AUTO_GROW);
    if (!ctx->timestamp_list) {
        return AVERROR(ENOMEM);
    }
    ctx->dts_delay = 0;


    ctx->library = dlopen(AMF_DLL_NAMEA, RTLD_NOW | RTLD_LOCAL);
    AMF_RETURN_IF_FALSE(ctx, ctx->library != NULL,
        AVERROR_UNKNOWN, "DLL %s failed to open\n", AMF_DLL_NAMEA);

    init_fun = (AMFInit_Fn)dlsym(ctx->library, AMF_INIT_FUNCTION_NAME);
    AMF_RETURN_IF_FALSE(ctx, init_fun != NULL, AVERROR_UNKNOWN, "DLL %s failed to find function %s\n", AMF_DLL_NAMEA, AMF_INIT_FUNCTION_NAME);

    version_fun = (AMFQueryVersion_Fn)dlsym(ctx->library, AMF_QUERY_VERSION_FUNCTION_NAME);
    AMF_RETURN_IF_FALSE(ctx, version_fun != NULL, AVERROR_UNKNOWN, "DLL %s failed to find function %s\n", AMF_DLL_NAMEA, AMF_QUERY_VERSION_FUNCTION_NAME);

    res = version_fun(&ctx->version);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "%s failed with error %d\n", AMF_QUERY_VERSION_FUNCTION_NAME, res);
    res = init_fun(AMF_FULL_VERSION, &ctx->factory);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "%s failed with error %d\n", AMF_INIT_FUNCTION_NAME, res);
    res = ctx->factory->pVtbl->GetTrace(ctx->factory, &ctx->trace);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetTrace() failed with error %d\n", res);
    res = ctx->factory->pVtbl->GetDebug(ctx->factory, &ctx->debug);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetDebug() failed with error %d\n", res);
    return 0;
}

#if CONFIG_D3D11VA
static int amf_init_from_d3d11_device(AVCodecContext *avctx, AVD3D11VADeviceContext *hwctx)
{
    AmfContext *ctx = avctx->priv_data;
    AMF_RESULT res;

    res = ctx->context->pVtbl->InitDX11(ctx->context, hwctx->device, AMF_DX11_1);
    if (res != AMF_OK) {
        if (res == AMF_NOT_SUPPORTED)
            av_log(avctx, AV_LOG_ERROR, "AMF via D3D11 is not supported on the given device.\n");
        else
            av_log(avctx, AV_LOG_ERROR, "AMF failed to initialise on the given D3D11 device: %d.\n", res);
        return AVERROR(ENODEV);
    }

    return 0;
}
#endif

#if CONFIG_DXVA2
static int amf_init_from_dxva2_device(AVCodecContext *avctx, AVDXVA2DeviceContext *hwctx)
{
    AmfContext *ctx = avctx->priv_data;
    HANDLE device_handle;
    IDirect3DDevice9 *device;
    HRESULT hr;
    AMF_RESULT res;
    int ret;

    hr = IDirect3DDeviceManager9_OpenDeviceHandle(hwctx->devmgr, &device_handle);
    if (FAILED(hr)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to open device handle for Direct3D9 device: %lx.\n", (unsigned long)hr);
        return AVERROR_EXTERNAL;
    }

    hr = IDirect3DDeviceManager9_LockDevice(hwctx->devmgr, device_handle, &device, FALSE);
    if (SUCCEEDED(hr)) {
        IDirect3DDeviceManager9_UnlockDevice(hwctx->devmgr, device_handle, FALSE);
        ret = 0;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Failed to lock device handle for Direct3D9 device: %lx.\n", (unsigned long)hr);
        ret = AVERROR_EXTERNAL;
    }

    IDirect3DDeviceManager9_CloseDeviceHandle(hwctx->devmgr, device_handle);

    if (ret < 0)
        return ret;

    res = ctx->context->pVtbl->InitDX9(ctx->context, device);

    IDirect3DDevice9_Release(device);

    if (res != AMF_OK) {
        if (res == AMF_NOT_SUPPORTED)
            av_log(avctx, AV_LOG_ERROR, "AMF via D3D9 is not supported on the given device.\n");
        else
            av_log(avctx, AV_LOG_ERROR, "AMF failed to initialise on given D3D9 device: %d.\n", res);
        return AVERROR(ENODEV);
    }

    return 0;
}
#endif

static int amf_init_context(AVCodecContext *avctx)
{
    AmfContext *ctx = avctx->priv_data;
    AMFContext1 *context1 = NULL;
    AMF_RESULT  res;
    av_unused int ret;

    ctx->hwsurfaces_in_queue = 0;

    // configure AMF logger
    // the return of these functions indicates old state and do not affect behaviour
    ctx->trace->pVtbl->EnableWriter(ctx->trace, AMF_TRACE_WRITER_DEBUG_OUTPUT, ctx->log_to_dbg != 0 );
    if (ctx->log_to_dbg)
        ctx->trace->pVtbl->SetWriterLevel(ctx->trace, AMF_TRACE_WRITER_DEBUG_OUTPUT, AMF_TRACE_TRACE);
    ctx->trace->pVtbl->EnableWriter(ctx->trace, AMF_TRACE_WRITER_CONSOLE, 0);
    ctx->trace->pVtbl->SetGlobalLevel(ctx->trace, AMF_TRACE_TRACE);

    // connect AMF logger to av_log
    ctx->tracer.vtbl = &tracer_vtbl;
    ctx->tracer.avctx = avctx;
    ctx->trace->pVtbl->RegisterWriter(ctx->trace, FFMPEG_AMF_WRITER_ID,(AMFTraceWriter*)&ctx->tracer, 1);
    ctx->trace->pVtbl->SetWriterLevel(ctx->trace, FFMPEG_AMF_WRITER_ID, AMF_TRACE_TRACE);

    res = ctx->factory->pVtbl->CreateContext(ctx->factory, &ctx->context);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "CreateContext() failed with error %d\n", res);

    // If a device was passed to the encoder, try to initialise from that.
    if (avctx->hw_frames_ctx) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;

        if (amf_av_to_amf_format(frames_ctx->sw_format) == AMF_SURFACE_UNKNOWN) {
            av_log(avctx, AV_LOG_ERROR, "Format of input frames context (%s) is not supported by AMF.\n",
                   av_get_pix_fmt_name(frames_ctx->sw_format));
            return AVERROR(EINVAL);
        }

        switch (frames_ctx->device_ctx->type) {
#if CONFIG_D3D11VA
        case AV_HWDEVICE_TYPE_D3D11VA:
            ret = amf_init_from_d3d11_device(avctx, frames_ctx->device_ctx->hwctx);
            if (ret < 0)
                return ret;
            break;
#endif
#if CONFIG_DXVA2
        case AV_HWDEVICE_TYPE_DXVA2:
            ret = amf_init_from_dxva2_device(avctx, frames_ctx->device_ctx->hwctx);
            if (ret < 0)
                return ret;
            break;
#endif
        default:
            av_log(avctx, AV_LOG_ERROR, "AMF initialisation from a %s frames context is not supported.\n",
                   av_hwdevice_get_type_name(frames_ctx->device_ctx->type));
            return AVERROR(ENOSYS);
        }

        ctx->hw_frames_ctx = av_buffer_ref(avctx->hw_frames_ctx);
        if (!ctx->hw_frames_ctx)
            return AVERROR(ENOMEM);

        if (frames_ctx->initial_pool_size > 0)
            ctx->hwsurfaces_in_queue_max = FFMIN(ctx->hwsurfaces_in_queue_max, frames_ctx->initial_pool_size - 1);

    } else if (avctx->hw_device_ctx) {
        AVHWDeviceContext *device_ctx = (AVHWDeviceContext*)avctx->hw_device_ctx->data;

        switch (device_ctx->type) {
#if CONFIG_D3D11VA
        case AV_HWDEVICE_TYPE_D3D11VA:
            ret = amf_init_from_d3d11_device(avctx, device_ctx->hwctx);
            if (ret < 0)
                return ret;
            break;
#endif
#if CONFIG_DXVA2
        case AV_HWDEVICE_TYPE_DXVA2:
            ret = amf_init_from_dxva2_device(avctx, device_ctx->hwctx);
            if (ret < 0)
                return ret;
            break;
#endif
        default:
            av_log(avctx, AV_LOG_ERROR, "AMF initialisation from a %s device is not supported.\n",
                   av_hwdevice_get_type_name(device_ctx->type));
            return AVERROR(ENOSYS);
        }

        ctx->hw_device_ctx = av_buffer_ref(avctx->hw_device_ctx);
        if (!ctx->hw_device_ctx)
            return AVERROR(ENOMEM);

    } else {
        res = ctx->context->pVtbl->InitDX11(ctx->context, NULL, AMF_DX11_1);
        if (res == AMF_OK) {
            av_log(avctx, AV_LOG_VERBOSE, "AMF initialisation succeeded via D3D11.\n");
        } else {
            res = ctx->context->pVtbl->InitDX9(ctx->context, NULL);
            if (res == AMF_OK) {
                av_log(avctx, AV_LOG_VERBOSE, "AMF initialisation succeeded via D3D9.\n");
            } else {
                AMFGuid guid = IID_AMFContext1();
                res = ctx->context->pVtbl->QueryInterface(ctx->context, &guid, (void**)&context1);
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "CreateContext1() failed with error %d\n", res);

                res = context1->pVtbl->InitVulkan(context1, NULL);
                context1->pVtbl->Release(context1);
                if (res != AMF_OK) {
                    if (res == AMF_NOT_SUPPORTED)
                        av_log(avctx, AV_LOG_ERROR, "AMF via Vulkan is not supported on the given device.\n");
                    else
                        av_log(avctx, AV_LOG_ERROR, "AMF failed to initialise on the given Vulkan device: %d.\n", res);
                    return AVERROR(ENOSYS);
                }
                av_log(avctx, AV_LOG_VERBOSE, "AMF initialisation succeeded via Vulkan.\n");
            }
        }
    }
    return 0;
}

static int amf_init_encoder(AVCodecContext *avctx)
{
    AmfContext        *ctx = avctx->priv_data;
    const wchar_t     *codec_id = NULL;
    AMF_RESULT         res;
    enum AVPixelFormat pix_fmt;

    switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            codec_id = AMFVideoEncoderVCE_AVC;
            break;
        case AV_CODEC_ID_HEVC:
            codec_id = AMFVideoEncoder_HEVC;
            break;
        case AV_CODEC_ID_AV1 :
            codec_id = AMFVideoEncoder_AV1;
            break;
        default:
            break;
    }
    AMF_RETURN_IF_FALSE(ctx, codec_id != NULL, AVERROR(EINVAL), "Codec %d is not supported\n", avctx->codec->id);

    if (ctx->hw_frames_ctx)
        pix_fmt = ((AVHWFramesContext*)ctx->hw_frames_ctx->data)->sw_format;
    else
        pix_fmt = avctx->pix_fmt;

    if (pix_fmt == AV_PIX_FMT_P010) {
        AMF_RETURN_IF_FALSE(ctx, ctx->version >= AMF_MAKE_FULL_VERSION(1, 4, 32, 0), AVERROR_UNKNOWN, "10-bit encoder is not supported by AMD GPU drivers versions lower than 23.30.\n");
    }

    ctx->format = amf_av_to_amf_format(pix_fmt);
    AMF_RETURN_IF_FALSE(ctx, ctx->format != AMF_SURFACE_UNKNOWN, AVERROR(EINVAL),
                        "Format %s is not supported\n", av_get_pix_fmt_name(pix_fmt));

    res = ctx->factory->pVtbl->CreateComponent(ctx->factory, ctx->context, codec_id, &ctx->encoder);
    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_ENCODER_NOT_FOUND, "CreateComponent(%ls) failed with error %d\n", codec_id, res);

    ctx->submitted_frame = 0;

    return 0;
}

int av_cold ff_amf_encode_close(AVCodecContext *avctx)
{
    AmfContext *ctx = avctx->priv_data;

    if (ctx->delayed_surface) {
        ctx->delayed_surface->pVtbl->Release(ctx->delayed_surface);
        ctx->delayed_surface = NULL;
    }

    if (ctx->encoder) {
        ctx->encoder->pVtbl->Terminate(ctx->encoder);
        ctx->encoder->pVtbl->Release(ctx->encoder);
        ctx->encoder = NULL;
    }

    if (ctx->context) {
        ctx->context->pVtbl->Terminate(ctx->context);
        ctx->context->pVtbl->Release(ctx->context);
        ctx->context = NULL;
    }
    av_buffer_unref(&ctx->hw_device_ctx);
    av_buffer_unref(&ctx->hw_frames_ctx);

    if (ctx->trace) {
        ctx->trace->pVtbl->UnregisterWriter(ctx->trace, FFMPEG_AMF_WRITER_ID);
    }
    if (ctx->library) {
        dlclose(ctx->library);
        ctx->library = NULL;
    }
    ctx->trace = NULL;
    ctx->debug = NULL;
    ctx->factory = NULL;
    ctx->version = 0;
    ctx->delayed_drain = 0;
    av_frame_free(&ctx->delayed_frame);
    av_fifo_freep2(&ctx->timestamp_list);

    return 0;
}

static int amf_copy_surface(AVCodecContext *avctx, const AVFrame *frame,
    AMFSurface* surface)
{
    AMFPlane *plane;
    uint8_t  *dst_data[4];
    int       dst_linesize[4];
    int       planes;
    int       i;

    planes = surface->pVtbl->GetPlanesCount(surface);
    av_assert0(planes < FF_ARRAY_ELEMS(dst_data));

    for (i = 0; i < planes; i++) {
        plane = surface->pVtbl->GetPlaneAt(surface, i);
        dst_data[i] = plane->pVtbl->GetNative(plane);
        dst_linesize[i] = plane->pVtbl->GetHPitch(plane);
    }
    av_image_copy2(dst_data, dst_linesize,
                   frame->data, frame->linesize, frame->format,
                   avctx->width, avctx->height);

    return 0;
}

static int amf_copy_buffer(AVCodecContext *avctx, AVPacket *pkt, AMFBuffer *buffer)
{
    AmfContext      *ctx = avctx->priv_data;
    int              ret;
    AMFVariantStruct var = {0};
    int64_t          timestamp = AV_NOPTS_VALUE;
    int64_t          size = buffer->pVtbl->GetSize(buffer);

    if ((ret = ff_get_encode_buffer(avctx, pkt, size, 0)) < 0) {
        return ret;
    }
    memcpy(pkt->data, buffer->pVtbl->GetNative(buffer), size);

    switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &var);
            if(var.int64Value == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR) {
                pkt->flags = AV_PKT_FLAG_KEY;
            }
            break;
        case AV_CODEC_ID_HEVC:
            buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &var);
            if (var.int64Value == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR) {
                pkt->flags = AV_PKT_FLAG_KEY;
            }
            break;
        case AV_CODEC_ID_AV1:
            buffer->pVtbl->GetProperty(buffer, AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &var);
            if (var.int64Value == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY) {
                pkt->flags = AV_PKT_FLAG_KEY;
            }
        default:
            break;
    }

    buffer->pVtbl->GetProperty(buffer, PTS_PROP, &var);

    pkt->pts = var.int64Value; // original pts


    AMF_RETURN_IF_FALSE(ctx, av_fifo_read(ctx->timestamp_list, &timestamp, 1) >= 0,
                        AVERROR_UNKNOWN, "timestamp_list is empty\n");

    // calc dts shift if max_b_frames > 0
    if ((ctx->max_b_frames > 0 || ((ctx->pa_adaptive_mini_gop == 1) ? true : false)) && ctx->dts_delay == 0) {
        int64_t timestamp_last = AV_NOPTS_VALUE;
        size_t can_read = av_fifo_can_read(ctx->timestamp_list);
        AMF_RETURN_IF_FALSE(ctx, can_read > 0, AVERROR_UNKNOWN,
            "timestamp_list is empty while max_b_frames = %d\n", avctx->max_b_frames);
        av_fifo_peek(ctx->timestamp_list, &timestamp_last, 1, can_read - 1);
        if (timestamp < 0 || timestamp_last < AV_NOPTS_VALUE) {
            return AVERROR(ERANGE);
        }
        ctx->dts_delay = timestamp_last - timestamp;
    }
    pkt->dts = timestamp - ctx->dts_delay;
    return 0;
}

// amfenc API implementation
int ff_amf_encode_init(AVCodecContext *avctx)
{
    int ret;

    if ((ret = amf_load_library(avctx)) == 0) {
        if ((ret = amf_init_context(avctx)) == 0) {
            if ((ret = amf_init_encoder(avctx)) == 0) {
                return 0;
            }
        }
    }
    ff_amf_encode_close(avctx);
    return ret;
}

static AMF_RESULT amf_set_property_buffer(AMFSurface *object, const wchar_t *name, AMFBuffer *val)
{
    AMF_RESULT res;
    AMFVariantStruct var;
    res = AMFVariantInit(&var);
    if (res == AMF_OK) {
        AMFGuid guid_AMFInterface = IID_AMFInterface();
        AMFInterface *amf_interface;
        res = val->pVtbl->QueryInterface(val, &guid_AMFInterface, (void**)&amf_interface);

        if (res == AMF_OK) {
            res = AMFVariantAssignInterface(&var, amf_interface);
            amf_interface->pVtbl->Release(amf_interface);
        }
        if (res == AMF_OK) {
            res = object->pVtbl->SetProperty(object, name, var);
        }
        AMFVariantClear(&var);
    }
    return res;
}

static AMF_RESULT amf_get_property_buffer(AMFData *object, const wchar_t *name, AMFBuffer **val)
{
    AMF_RESULT res;
    AMFVariantStruct var;
    res = AMFVariantInit(&var);
    if (res == AMF_OK) {
        res = object->pVtbl->GetProperty(object, name, &var);
        if (res == AMF_OK) {
            if (var.type == AMF_VARIANT_INTERFACE) {
                AMFGuid guid_AMFBuffer = IID_AMFBuffer();
                AMFInterface *amf_interface = AMFVariantInterface(&var);
                res = amf_interface->pVtbl->QueryInterface(amf_interface, &guid_AMFBuffer, (void**)val);
            } else {
                res = AMF_INVALID_DATA_TYPE;
            }
        }
        AMFVariantClear(&var);
    }
    return res;
}

static AMFBuffer *amf_create_buffer_with_frame_ref(const AVFrame *frame, AMFContext *context)
{
    AVFrame *frame_ref;
    AMFBuffer *frame_ref_storage_buffer = NULL;
    AMF_RESULT res;

    res = context->pVtbl->AllocBuffer(context, AMF_MEMORY_HOST, sizeof(frame_ref), &frame_ref_storage_buffer);
    if (res == AMF_OK) {
        frame_ref = av_frame_clone(frame);
        if (frame_ref) {
            memcpy(frame_ref_storage_buffer->pVtbl->GetNative(frame_ref_storage_buffer), &frame_ref, sizeof(frame_ref));
        } else {
            frame_ref_storage_buffer->pVtbl->Release(frame_ref_storage_buffer);
            frame_ref_storage_buffer = NULL;
        }
    }
    return frame_ref_storage_buffer;
}

static void amf_release_buffer_with_frame_ref(AMFBuffer *frame_ref_storage_buffer)
{
    AVFrame *frame_ref;
    memcpy(&frame_ref, frame_ref_storage_buffer->pVtbl->GetNative(frame_ref_storage_buffer), sizeof(frame_ref));
    av_frame_free(&frame_ref);
    frame_ref_storage_buffer->pVtbl->Release(frame_ref_storage_buffer);
}

int ff_amf_receive_packet(AVCodecContext *avctx, AVPacket *avpkt)
{
    AmfContext *ctx = avctx->priv_data;
    AMFSurface *surface;
    AMF_RESULT  res;
    int         ret;
    AMF_RESULT  res_query;
    AMFData    *data = NULL;
    AVFrame    *frame = ctx->delayed_frame;
    int         block_and_wait;
    int         query_output_data_flag = 0;
    AMF_RESULT  res_resubmit;

    if (!ctx->encoder)
        return AVERROR(EINVAL);

    if (!frame->buf[0]) {
        ret = ff_encode_get_frame(avctx, frame);
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
    }

    if (!frame->buf[0]) { // submit drain
        if (!ctx->eof) { // submit drain one time only
            if (ctx->delayed_surface != NULL) {
                ctx->delayed_drain = 1; // input queue is full: resubmit Drain() in ff_amf_receive_packet
            } else if(!ctx->delayed_drain) {
                res = ctx->encoder->pVtbl->Drain(ctx->encoder);
                if (res == AMF_INPUT_FULL) {
                    ctx->delayed_drain = 1; // input queue is full: resubmit Drain() in ff_amf_receive_packet
                } else {
                    if (res == AMF_OK) {
                        ctx->eof = 1; // drain started
                    }
                    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "Drain() failed with error %d\n", res);
                }
            }
        }
    } else if (!ctx->delayed_surface) { // submit frame
        int hw_surface = 0;

        // prepare surface from frame
        switch (frame->format) {
#if CONFIG_D3D11VA
        case AV_PIX_FMT_D3D11:
            {
                static const GUID AMFTextureArrayIndexGUID = { 0x28115527, 0xe7c3, 0x4b66, { 0x99, 0xd3, 0x4f, 0x2a, 0xe6, 0xb4, 0x7f, 0xaf } };
                ID3D11Texture2D *texture = (ID3D11Texture2D*)frame->data[0]; // actual texture
                int index = (intptr_t)frame->data[1]; // index is a slice in texture array is - set to tell AMF which slice to use

                av_assert0(frame->hw_frames_ctx       && ctx->hw_frames_ctx &&
                           frame->hw_frames_ctx->data == ctx->hw_frames_ctx->data);

                texture->lpVtbl->SetPrivateData(texture, &AMFTextureArrayIndexGUID, sizeof(index), &index);

                res = ctx->context->pVtbl->CreateSurfaceFromDX11Native(ctx->context, texture, &surface, NULL); // wrap to AMF surface
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX11Native() failed  with error %d\n", res);

                hw_surface = 1;
            }
            break;
#endif
#if CONFIG_DXVA2
        case AV_PIX_FMT_DXVA2_VLD:
            {
                IDirect3DSurface9 *texture = (IDirect3DSurface9 *)frame->data[3]; // actual texture

                res = ctx->context->pVtbl->CreateSurfaceFromDX9Native(ctx->context, texture, &surface, NULL); // wrap to AMF surface
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "CreateSurfaceFromDX9Native() failed  with error %d\n", res);

                hw_surface = 1;
            }
            break;
#endif
        default:
            {
                res = ctx->context->pVtbl->AllocSurface(ctx->context, AMF_MEMORY_HOST, ctx->format, avctx->width, avctx->height, &surface);
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR(ENOMEM), "AllocSurface() failed  with error %d\n", res);
                amf_copy_surface(avctx, frame, surface);
            }
            break;
        }

        if (hw_surface) {
            AMFBuffer *frame_ref_storage_buffer;

            // input HW surfaces can be vertically aligned by 16; tell AMF the real size
            surface->pVtbl->SetCrop(surface, 0, 0, frame->width, frame->height);

            frame_ref_storage_buffer = amf_create_buffer_with_frame_ref(frame, ctx->context);
            AMF_RETURN_IF_FALSE(ctx, frame_ref_storage_buffer != NULL, AVERROR(ENOMEM), "create_buffer_with_frame_ref() returned NULL\n");

            res = amf_set_property_buffer(surface, L"av_frame_ref", frame_ref_storage_buffer);
            AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "SetProperty failed for \"av_frame_ref\" with error %d\n", res);
            ctx->hwsurfaces_in_queue++;
            frame_ref_storage_buffer->pVtbl->Release(frame_ref_storage_buffer);
        }

        // HDR10 metadata
        if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
            AMFBuffer * hdrmeta_buffer = NULL;
            res = ctx->context->pVtbl->AllocBuffer(ctx->context, AMF_MEMORY_HOST, sizeof(AMFHDRMetadata), &hdrmeta_buffer);
            if (res == AMF_OK) {
                AMFHDRMetadata * hdrmeta = (AMFHDRMetadata*)hdrmeta_buffer->pVtbl->GetNative(hdrmeta_buffer);
                if (amf_save_hdr_metadata(avctx, frame, hdrmeta) == 0) {
                    switch (avctx->codec->id) {
                    case AV_CODEC_ID_H264:
                        AMF_ASSIGN_PROPERTY_INTERFACE(res, ctx->encoder, AMF_VIDEO_ENCODER_INPUT_HDR_METADATA, hdrmeta_buffer); break;
                    case AV_CODEC_ID_HEVC:
                        AMF_ASSIGN_PROPERTY_INTERFACE(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_INPUT_HDR_METADATA, hdrmeta_buffer); break;
                    case AV_CODEC_ID_AV1:
                        AMF_ASSIGN_PROPERTY_INTERFACE(res, ctx->encoder, AMF_VIDEO_ENCODER_AV1_INPUT_HDR_METADATA, hdrmeta_buffer); break;
                    }
                    res = amf_set_property_buffer(surface, L"av_frame_hdrmeta", hdrmeta_buffer);
                    AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "SetProperty failed for \"av_frame_hdrmeta\" with error %d\n", res);
                }
                hdrmeta_buffer->pVtbl->Release(hdrmeta_buffer);
            }
        }

        surface->pVtbl->SetPts(surface, frame->pts);
        AMF_ASSIGN_PROPERTY_INT64(res, surface, PTS_PROP, frame->pts);

        switch (avctx->codec->id) {
        case AV_CODEC_ID_H264:
            AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_INSERT_AUD, !!ctx->aud);
            switch (frame->pict_type) {
            case AV_PICTURE_TYPE_I:
                if (ctx->forced_idr) {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_INSERT_SPS, 1);
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_INSERT_PPS, 1);
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
                } else {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_I);
                }
                break;
            case AV_PICTURE_TYPE_P:
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_P);
                break;
            case AV_PICTURE_TYPE_B:
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_B);
                break;
            }
            break;
        case AV_CODEC_ID_HEVC:
            AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_INSERT_AUD, !!ctx->aud);
            switch (frame->pict_type) {
            case AV_PICTURE_TYPE_I:
                if (ctx->forced_idr) {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, 1);
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
                } else {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_I);
                }
                break;
            case AV_PICTURE_TYPE_P:
                AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_P);
                break;
            }
            break;
        case AV_CODEC_ID_AV1:
            if (frame->pict_type == AV_PICTURE_TYPE_I) {
                if (ctx->forced_idr) {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, 1);
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY);
                } else {
                    AMF_ASSIGN_PROPERTY_INT64(res, surface, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_INTRA_ONLY);
                }
            }
            break;
        default:
            break;
        }

        // submit surface
        res = ctx->encoder->pVtbl->SubmitInput(ctx->encoder, (AMFData*)surface);
        if (res == AMF_INPUT_FULL) { // handle full queue
            //store surface for later submission
            ctx->delayed_surface = surface;
        } else {
            int64_t pts = frame->pts;
            surface->pVtbl->Release(surface);
            AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "SubmitInput() failed with error %d\n", res);

            av_frame_unref(frame);
            ret = av_fifo_write(ctx->timestamp_list, &pts, 1);

            if (ctx->submitted_frame == 0)
            {
                ctx->use_b_frame = (ctx->max_b_frames > 0 || ((ctx->pa_adaptive_mini_gop == 1) ? true : false));
            }
            ctx->submitted_frame++;

            if (ret < 0)
                return ret;
        }
    }


    do {
        block_and_wait = 0;
        // poll data
        if (!avpkt->data && !avpkt->buf && (ctx->use_b_frame ? (ctx->submitted_frame >= 2) : true) ) {
            res_query = ctx->encoder->pVtbl->QueryOutput(ctx->encoder, &data);
            if (data) {
                // copy data to packet
                AMFBuffer *buffer;
                AMFGuid guid = IID_AMFBuffer();
                query_output_data_flag = 1;
                data->pVtbl->QueryInterface(data, &guid, (void**)&buffer); // query for buffer interface
                ret = amf_copy_buffer(avctx, avpkt, buffer);

                ctx->submitted_frame++;
                buffer->pVtbl->Release(buffer);

                if (data->pVtbl->HasProperty(data, L"av_frame_ref")) {
                    AMFBuffer* frame_ref_storage_buffer;
                    res = amf_get_property_buffer(data, L"av_frame_ref", &frame_ref_storage_buffer);
                    AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "GetProperty failed for \"av_frame_ref\" with error %d\n", res);
                    amf_release_buffer_with_frame_ref(frame_ref_storage_buffer);
                    ctx->hwsurfaces_in_queue--;
                }

                data->pVtbl->Release(data);

                AMF_RETURN_IF_FALSE(ctx, ret >= 0, ret, "amf_copy_buffer() failed with error %d\n", ret);
            }
        }
        res_resubmit = AMF_OK;
        if (ctx->delayed_surface != NULL) { // try to resubmit frame
            if (ctx->delayed_surface->pVtbl->HasProperty(ctx->delayed_surface, L"av_frame_hdrmeta")) {
                AMFBuffer * hdrmeta_buffer = NULL;
                res = amf_get_property_buffer((AMFData *)ctx->delayed_surface, L"av_frame_hdrmeta", &hdrmeta_buffer);
                AMF_RETURN_IF_FALSE(avctx, res == AMF_OK, AVERROR_UNKNOWN, "GetProperty failed for \"av_frame_hdrmeta\" with error %d\n", res);
                switch (avctx->codec->id) {
                case AV_CODEC_ID_H264:
                    AMF_ASSIGN_PROPERTY_INTERFACE(res, ctx->encoder, AMF_VIDEO_ENCODER_INPUT_HDR_METADATA, hdrmeta_buffer); break;
                case AV_CODEC_ID_HEVC:
                    AMF_ASSIGN_PROPERTY_INTERFACE(res, ctx->encoder, AMF_VIDEO_ENCODER_HEVC_INPUT_HDR_METADATA, hdrmeta_buffer); break;
                case AV_CODEC_ID_AV1:
                    AMF_ASSIGN_PROPERTY_INTERFACE(res, ctx->encoder, AMF_VIDEO_ENCODER_AV1_INPUT_HDR_METADATA, hdrmeta_buffer); break;
                }
                hdrmeta_buffer->pVtbl->Release(hdrmeta_buffer);
            }
            res_resubmit = ctx->encoder->pVtbl->SubmitInput(ctx->encoder, (AMFData*)ctx->delayed_surface);
            if (res_resubmit != AMF_INPUT_FULL) {
                int64_t pts = ctx->delayed_surface->pVtbl->GetPts(ctx->delayed_surface);
                ctx->delayed_surface->pVtbl->Release(ctx->delayed_surface);
                ctx->delayed_surface = NULL;
                av_frame_unref(ctx->delayed_frame);
                AMF_RETURN_IF_FALSE(ctx, res_resubmit == AMF_OK, AVERROR_UNKNOWN, "Repeated SubmitInput() failed with error %d\n", res_resubmit);

                ctx->submitted_frame++;
                ret = av_fifo_write(ctx->timestamp_list, &pts, 1);
                if (ret < 0)
                    return ret;
            }
        } else if (ctx->delayed_drain) { // try to resubmit drain
            res = ctx->encoder->pVtbl->Drain(ctx->encoder);
            if (res != AMF_INPUT_FULL) {
                ctx->delayed_drain = 0;
                ctx->eof = 1; // drain started
                AMF_RETURN_IF_FALSE(ctx, res == AMF_OK, AVERROR_UNKNOWN, "Repeated Drain() failed with error %d\n", res);
            } else {
                av_log(avctx, AV_LOG_WARNING, "Data acquired but delayed drain submission got AMF_INPUT_FULL- should not happen\n");
            }
        }

        if (query_output_data_flag == 0) {
            if (res_resubmit == AMF_INPUT_FULL || ctx->delayed_drain || (ctx->eof && res_query != AMF_EOF) || (ctx->hwsurfaces_in_queue >= ctx->hwsurfaces_in_queue_max)) {
                block_and_wait = 1;

                // Only sleep if the driver doesn't support waiting in QueryOutput()
                // or if we already have output data so we will skip calling it.
                if (!ctx->query_timeout_supported || avpkt->data || avpkt->buf) {
                    av_usleep(1000);
                }
            }
        }
    } while (block_and_wait);

    if (res_query == AMF_EOF) {
        ret = AVERROR_EOF;
    } else if (data == NULL) {
        ret = AVERROR(EAGAIN);
    } else {
        ret = 0;
    }
    return ret;
}

int ff_amf_get_color_profile(AVCodecContext *avctx)
{
    amf_int64 color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_UNKNOWN;
    if (avctx->color_range == AVCOL_RANGE_JPEG) {
        /// Color Space for Full (JPEG) Range
        switch (avctx->colorspace) {
        case AVCOL_SPC_SMPTE170M:
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_601;
            break;
        case AVCOL_SPC_BT709:
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020;
            break;
        }
    } else {
        /// Color Space for Limited (MPEG) range
        switch (avctx->colorspace) {
        case AVCOL_SPC_SMPTE170M:
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_601;
            break;
        case AVCOL_SPC_BT709:
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_709;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            color_profile = AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020;
            break;
        }
    }
    return color_profile;
}

const AVCodecHWConfigInternal *const ff_amfenc_hw_configs[] = {
#if CONFIG_D3D11VA
    HW_CONFIG_ENCODER_FRAMES(D3D11, D3D11VA),
    HW_CONFIG_ENCODER_DEVICE(NONE,  D3D11VA),
#endif
#if CONFIG_DXVA2
    HW_CONFIG_ENCODER_FRAMES(DXVA2_VLD, DXVA2),
    HW_CONFIG_ENCODER_DEVICE(NONE,      DXVA2),
#endif
    NULL,
};
