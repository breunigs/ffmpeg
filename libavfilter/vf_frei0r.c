/*
 * Copyright (c) 2010 Stefano Sabatini
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

/**
 * @file
 * frei0r wrapper
 */

#include <frei0r.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "compat/w32dlfcn.h"
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/getenv_utf8.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "framesync.h"
#include "video.h"

typedef f0r_instance_t (*f0r_construct_f)(unsigned int width, unsigned int height);
typedef void (*f0r_destruct_f)(f0r_instance_t instance);
typedef void (*f0r_deinit_f)(void);
typedef int (*f0r_init_f)(void);
typedef void (*f0r_get_plugin_info_f)(f0r_plugin_info_t *info);
typedef void (*f0r_get_param_info_f)(f0r_param_info_t *info, int param_index);
typedef void (*f0r_update_f)(f0r_instance_t instance, double time, const uint32_t *inframe, uint32_t *outframe);
typedef void (*f0r_update2_f)(f0r_instance_t instance, double time, const uint32_t *inframe1, const uint32_t *inframe2, const uint32_t *inframe3, uint32_t *outframe);
typedef void (*f0r_set_param_value_f)(f0r_instance_t instance, f0r_param_t param, int param_index);
typedef void (*f0r_get_param_value_f)(f0r_instance_t instance, f0r_param_t param, int param_index);

typedef struct Frei0rContext {
    const AVClass *class;
    f0r_update_f update;
    f0r_update2_f update2;
    void *dl_handle;            /* dynamic library handle   */
    f0r_instance_t instance;
    f0r_plugin_info_t plugin_info;

    f0r_get_param_info_f  get_param_info;
    f0r_get_param_value_f get_param_value;
    f0r_set_param_value_f set_param_value;
    f0r_construct_f       construct;
    f0r_destruct_f        destruct;
    f0r_deinit_f          deinit;

    FFFrameSync fs;
    AVFrame **frames;
    int nb_inputs;
    char *dl_name;
    char *params;
    AVRational framerate;

    /* only used by the source */
    int w, h;
    AVRational time_base;
    uint64_t pts;
} Frei0rContext;

static void *load_sym(AVFilterContext *ctx, const char *sym_name)
{
    Frei0rContext *s = ctx->priv;
    void *sym = dlsym(s->dl_handle, sym_name);
    if (!sym)
        av_log(ctx, AV_LOG_ERROR, "Could not find symbol '%s' in loaded module.\n", sym_name);
    return sym;
}

static int set_param(AVFilterContext *ctx, f0r_param_info_t info, int index, char *param)
{
    Frei0rContext *s = ctx->priv;
    union {
        double d;
        f0r_param_color_t col;
        f0r_param_position_t pos;
        f0r_param_string str;
    } val;
    char *tail;
    uint8_t rgba[4];

    switch (info.type) {
    case F0R_PARAM_BOOL:
        if      (!strcmp(param, "y")) val.d = 1.0;
        else if (!strcmp(param, "n")) val.d = 0.0;
        else goto fail;
        break;

    case F0R_PARAM_DOUBLE:
        val.d = av_strtod(param, &tail);
        if (*tail || val.d == HUGE_VAL)
            goto fail;
        break;

    case F0R_PARAM_COLOR:
        if (sscanf(param, "%f/%f/%f", &val.col.r, &val.col.g, &val.col.b) != 3) {
            if (av_parse_color(rgba, param, -1, ctx) < 0)
                goto fail;
            val.col.r = rgba[0] / 255.0;
            val.col.g = rgba[1] / 255.0;
            val.col.b = rgba[2] / 255.0;
        }
        break;

    case F0R_PARAM_POSITION:
        if (sscanf(param, "%lf/%lf", &val.pos.x, &val.pos.y) != 2)
            goto fail;
        break;

    case F0R_PARAM_STRING:
        val.str = param;
        break;
    }

    s->set_param_value(s->instance, &val, index);
    return 0;

fail:
    av_log(ctx, AV_LOG_ERROR, "Invalid value '%s' for parameter '%s'.\n",
           param, info.name);
    return AVERROR(EINVAL);
}

static int set_params(AVFilterContext *ctx, const char *params)
{
    Frei0rContext *s = ctx->priv;
    int i;

    if (!params)
        return 0;

    for (i = 0; i < s->plugin_info.num_params; i++) {
        f0r_param_info_t info;
        char *param;
        int ret;

        s->get_param_info(&info, i);

        if (*params) {
            if (!(param = av_get_token(&params, "|")))
                return AVERROR(ENOMEM);
            if (*params)
                params++;               /* skip ':' */
            ret = set_param(ctx, info, i, param);
            av_free(param);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static int load_path(AVFilterContext *ctx, void **handle_ptr, const char *prefix, const char *name)
{
    char *path = av_asprintf("%s%s%s", prefix, name, SLIBSUF);
    if (!path)
        return AVERROR(ENOMEM);
    av_log(ctx, AV_LOG_DEBUG, "Looking for frei0r effect in '%s'.\n", path);
    *handle_ptr = dlopen(path, RTLD_NOW|RTLD_LOCAL);
    av_free(path);
    return 0;
}

static av_cold int frei0r_init(AVFilterContext *ctx, const char *dl_name,
                               const int min_inputs, const int max_inputs)
{
    Frei0rContext *s = ctx->priv;
    f0r_init_f            f0r_init;
    f0r_get_plugin_info_f f0r_get_plugin_info;
    f0r_plugin_info_t *pi;
    char *path;
    int ret = 0;
    int i;
    static const char* const frei0r_pathlist[] = {
        "/usr/local/lib/frei0r-1/",
        "/usr/lib/frei0r-1/",
        "/usr/local/lib64/frei0r-1/",
        "/usr/lib64/frei0r-1/"
    };

    if (!dl_name) {
        av_log(ctx, AV_LOG_ERROR, "No filter name provided.\n");
        return AVERROR(EINVAL);
    }

    /* see: http://frei0r.dyne.org/codedoc/html/group__pluglocations.html */
    if (path = getenv_dup("FREI0R_PATH")) {
#ifdef _WIN32
        const char *separator = ";";
#else
        const char *separator = ":";
#endif
        char *p, *ptr = NULL;
        for (p = path; p = av_strtok(p, separator, &ptr); p = NULL) {
            /* add additional trailing slash in case it is missing */
            char *p1 = av_asprintf("%s/", p);
            if (!p1) {
                ret = AVERROR(ENOMEM);
                goto check_path_end;
            }
            ret = load_path(ctx, &s->dl_handle, p1, dl_name);
            av_free(p1);
            if (ret < 0)
                goto check_path_end;
            if (s->dl_handle)
                break;
        }

    check_path_end:
        av_free(path);
        if (ret < 0)
            return ret;
    }
    if (!s->dl_handle && (path = getenv_utf8("HOME"))) {
        char *prefix = av_asprintf("%s/.frei0r-1/lib/", path);
        if (!prefix) {
            ret = AVERROR(ENOMEM);
            goto home_path_end;
        }
        ret = load_path(ctx, &s->dl_handle, prefix, dl_name);
        av_free(prefix);

    home_path_end:
        freeenv_utf8(path);
        if (ret < 0)
            return ret;
    }
    for (i = 0; !s->dl_handle && i < FF_ARRAY_ELEMS(frei0r_pathlist); i++) {
        ret = load_path(ctx, &s->dl_handle, frei0r_pathlist[i], dl_name);
        if (ret < 0)
            return ret;
    }
    if (!s->dl_handle) {
        av_log(ctx, AV_LOG_ERROR, "Could not find module '%s'.\n", dl_name);
        return AVERROR(EINVAL);
    }

    if (!(f0r_init                = load_sym(ctx, "f0r_init"           )) ||
        !(f0r_get_plugin_info     = load_sym(ctx, "f0r_get_plugin_info")) ||
        !(s->get_param_info  = load_sym(ctx, "f0r_get_param_info" )) ||
        !(s->get_param_value = load_sym(ctx, "f0r_get_param_value")) ||
        !(s->set_param_value = load_sym(ctx, "f0r_set_param_value")) ||
        !(s->construct       = load_sym(ctx, "f0r_construct"      )) ||
        !(s->destruct        = load_sym(ctx, "f0r_destruct"       )) ||
        !(s->deinit          = load_sym(ctx, "f0r_deinit"         )))
        return AVERROR(EINVAL);

    if (f0r_init() < 0) {
        av_log(ctx, AV_LOG_ERROR, "Could not init the frei0r module.\n");
        return AVERROR(EINVAL);
    }

    f0r_get_plugin_info(&s->plugin_info);
    pi = &s->plugin_info;

    s->nb_inputs = pi->plugin_type == F0R_PLUGIN_TYPE_SOURCE ? 0 :
                   pi->plugin_type == F0R_PLUGIN_TYPE_FILTER ? 1 :
                   pi->plugin_type == F0R_PLUGIN_TYPE_MIXER2 ? 2 :
                   pi->plugin_type == F0R_PLUGIN_TYPE_MIXER3 ? 3 : -1;

    if (s->nb_inputs < 2) {
        s->update = load_sym(ctx, "f0r_update");
        if (!s->update)
            return AVERROR(EINVAL);
    } else {
        s->update2 = load_sym(ctx, "f0r_update2");
        if (!s->update2)
            return AVERROR(EINVAL);
    }

    if (s->nb_inputs < min_inputs || s->nb_inputs > max_inputs) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid type '%s' for this plugin\n",
               pi->plugin_type == F0R_PLUGIN_TYPE_FILTER ? "filter" :
               pi->plugin_type == F0R_PLUGIN_TYPE_SOURCE ? "source" :
               pi->plugin_type == F0R_PLUGIN_TYPE_MIXER2 ? "mixer2" :
               pi->plugin_type == F0R_PLUGIN_TYPE_MIXER3 ? "mixer3" : "unknown");
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_VERBOSE,
           "name:%s author:'%s' explanation:'%s' color_model:%s "
           "frei0r_version:%d version:%d.%d num_params:%d\n",
           pi->name, pi->author, pi->explanation,
           pi->color_model == F0R_COLOR_MODEL_BGRA8888 ? "bgra8888" :
           pi->color_model == F0R_COLOR_MODEL_RGBA8888 ? "rgba8888" :
           pi->color_model == F0R_COLOR_MODEL_PACKED32 ? "packed32" : "unknown",
           pi->frei0r_version, pi->major_version, pi->minor_version, pi->num_params);

    return 0;
}

static av_cold int filter_init(AVFilterContext *ctx)
{
    Frei0rContext *s = ctx->priv;
    int i, ret;

    ret = frei0r_init(ctx, s->dl_name, 1, 3);
    if (ret < 0)
        return ret;

    s->frames = av_calloc(s->nb_inputs, sizeof(*s->frames));
    if (!s->frames)
        return AVERROR(ENOMEM);

    for (i = 0; i < s->nb_inputs; i++) {
        AVFilterPad pad = {
            .type = AVMEDIA_TYPE_VIDEO,
            .name = av_asprintf("input%d", i),
        };

        if (!pad.name)
            return AVERROR(ENOMEM);

        ret = ff_append_inpad_free_name(ctx, &pad);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    Frei0rContext *s = ctx->priv;

    if (s->destruct && s->instance)
        s->destruct(s->instance);
    if (s->deinit)
        s->deinit();
    if (s->dl_handle)
        dlclose(s->dl_handle);

    ff_framesync_uninit(&s->fs);
    av_freep(&s->frames);
}

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    const Frei0rContext *s = ctx->priv;
    AVFilterFormats *formats = NULL;
    int ret;

    if        (s->plugin_info.color_model == F0R_COLOR_MODEL_BGRA8888) {
        if ((ret = ff_add_format(&formats, AV_PIX_FMT_BGRA)) < 0)
            return ret;
    } else if (s->plugin_info.color_model == F0R_COLOR_MODEL_RGBA8888) {
        if ((ret = ff_add_format(&formats, AV_PIX_FMT_RGBA)) < 0)
            return ret;
    } else {                                   /* F0R_COLOR_MODEL_PACKED32 */
        static const enum AVPixelFormat pix_fmts[] = {
            AV_PIX_FMT_BGRA, AV_PIX_FMT_ARGB, AV_PIX_FMT_ABGR, AV_PIX_FMT_NONE
        };
        formats = ff_make_format_list(pix_fmts);
    }

    if (!formats)
        return AVERROR(ENOMEM);

    return ff_set_common_formats2(ctx, cfg_in, cfg_out, formats);
}

static int filter_activate(AVFilterContext *ctx)
{
    Frei0rContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

static int filter_frame(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    AVFilterLink *outlink = ctx->outputs[0];
    FilterLink *ol = ff_filter_link(outlink);
    Frei0rContext *s = fs->opaque;
    AVFrame **in = s->frames;
    int copies = 0;
    AVFrame *out;
    int i, ret;
    double time;

    for (i=0; i < s->nb_inputs; i++) {
        ret = ff_framesync_get_frame(&s->fs, i, &in[i], 0);
        if (ret < 0)
            return ret;
    }

    if (ctx->is_disabled) {
        out = av_frame_clone(in[0]);
        if (!out)
            return AVERROR(ENOMEM);
        return ff_filter_frame(outlink, out);
    }

    /* align parameter is the line alignment, not the buffer alignment.
     * frei0r expects line size to be width*4 so we want an align of 1
     * to ensure lines aren't padded out. */
    out = ff_default_get_video_buffer2(outlink, outlink->w, outlink->h, 1);
    if (!out)
        return AVERROR(ENOMEM);

    ret = av_frame_copy_props(out, in[0]);
    if (ret < 0) {
        av_frame_free(&out);
        return ret;
    }
    out->pts = av_rescale_q(s->fs.pts, s->fs.time_base, outlink->time_base);
    if (s->nb_inputs >= 2 && ol->frame_rate.den == 0)
        out->duration = 0;

    time = s->fs.pts * av_q2d(s->fs.time_base);

    for (i=0; i < s->nb_inputs; i++) {
        if (in[i]->linesize[0] != out->linesize[0]) {
            AVFrame *aligned = ff_default_get_video_buffer2(outlink, outlink->w, outlink->h, 1);
            if(!aligned)
                goto end;
            ret = av_frame_copy(aligned, in[i]);
            copies |= 1 << i;
            in[i] = aligned;
            if(ret < 0)
                goto end;
        }
    }

    if (s->nb_inputs == 1) {
        s->update(s->instance, time,
                    (const uint32_t *)in[0]->data[0],
                    (uint32_t *)out->data[0]);
    } else {
        s->update2(s->instance, time,
                    (const uint32_t *)in[0]->data[0],
                    s->nb_inputs >= 2 ? (const uint32_t *)in[1]->data[0] : NULL,
                    s->nb_inputs >= 3 ? (const uint32_t *)in[2]->data[0] : NULL,
                    (uint32_t *)out->data[0]);
    }

    ret = ff_filter_frame(outlink, out);

end:
    for (i=0; i < s->nb_inputs; i++) {
        if ((copies >> i) & 0x1)
            av_frame_free(&in[i]);
    }

    return ret;
}

static int filter_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    Frei0rContext *s = ctx->priv;
    FilterLink *il = ff_filter_link(ctx->inputs[0]);
    FilterLink *ol = ff_filter_link(outlink);
    int height = ctx->inputs[0]->h;
    int width = ctx->inputs[0]->w;
    int i, ret;

    for (i = 1; i < s->nb_inputs; i++) {
        if (ctx->inputs[i]->w != width || ctx->inputs[i]->h != height) {
            av_log(ctx, AV_LOG_ERROR,
                   "Input %d dimensions %dx%d do not match input %d dimensions %dx%d.\n",
                   i, ctx->inputs[i]->w, ctx->inputs[i]->h, 0, width, height);
            return AVERROR(EINVAL);
        }
    }

    if (s->destruct && s->instance)
        s->destruct(s->instance);
    s->instance = s->construct(width, height);
    if (!s->instance) {
        av_log(ctx, AV_LOG_ERROR, "Impossible to load frei0r instance.\n");
        return AVERROR(EINVAL);
    }

    ret = set_params(ctx, s->params);
    if (ret < 0)
        return ret;

    outlink->w = width;
    outlink->h = height;
    outlink->sample_aspect_ratio = ctx->inputs[0]->sample_aspect_ratio;
    ol->frame_rate = il->frame_rate;

    for (i = 1; i < s->nb_inputs; i++) {
        il = ff_filter_link(ctx->inputs[i]);
        if (ol->frame_rate.num != il->frame_rate.num ||
            ol->frame_rate.den != il->frame_rate.den) {
            av_log(ctx, AV_LOG_VERBOSE,
                    "Video inputs have different frame rates, output will be VFR\n");
            ol->frame_rate = av_make_q(1, 0);
            break;
        }
    }

    ret = ff_framesync_init(&s->fs, ctx, s->nb_inputs);
    if (ret < 0)
        return ret;

    s->fs.opaque = s;
    s->fs.on_event = filter_frame;

    for (i = 0; i < s->nb_inputs; i++) {
        AVFilterLink *inlink = ctx->inputs[i];

        s->fs.in[i].time_base = inlink->time_base;
        s->fs.in[i].sync      = 1;
        s->fs.in[i].before    = EXT_STOP;
        s->fs.in[i].after     = EXT_INFINITY;
    }

    ret = ff_framesync_configure(&s->fs);
    outlink->time_base = s->fs.time_base;

    return ret;
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    Frei0rContext *s = ctx->priv;
    int ret;

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    return set_params(ctx, s->params);
}

#define OFFSET(x) offsetof(Frei0rContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM
#define TFLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption frei0r_options[] = {
    { "filter_name",   NULL, OFFSET(dl_name), AV_OPT_TYPE_STRING, .flags = FLAGS },
    { "filter_params", NULL, OFFSET(params),  AV_OPT_TYPE_STRING, .flags = TFLAGS },
    { NULL }
};

FRAMESYNC_DEFINE_CLASS(frei0r, Frei0rContext, fs);

static const AVFilterPad avfilter_vf_frei0r_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = filter_config_props,
    },
};

const AVFilter ff_vf_frei0r = {
    .name          = "frei0r",
    .description   = NULL_IF_CONFIG_SMALL("Apply a frei0r effect."),
    .preinit       = frei0r_framesync_preinit,
    .init          = filter_init,
    .uninit        = uninit,
    .activate      = filter_activate,
    .priv_size     = sizeof(Frei0rContext),
    .priv_class    = &frei0r_class,
    FILTER_OUTPUTS(avfilter_vf_frei0r_outputs),
    FILTER_QUERY_FUNC2(query_formats),
    .process_command = process_command,
    .flags         = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
};

static av_cold int source_init(AVFilterContext *ctx)
{
    Frei0rContext *s = ctx->priv;

    s->time_base.num = s->framerate.den;
    s->time_base.den = s->framerate.num;

    return frei0r_init(ctx, s->dl_name, 0, 0);
}

static int source_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    FilterLink        *l = ff_filter_link(outlink);
    Frei0rContext *s = ctx->priv;

    if (av_image_check_size(s->w, s->h, 0, ctx) < 0)
        return AVERROR(EINVAL);
    outlink->w = s->w;
    outlink->h = s->h;
    outlink->time_base = s->time_base;
    l->frame_rate = av_inv_q(s->time_base);
    outlink->sample_aspect_ratio = (AVRational){1,1};

    if (s->destruct && s->instance)
        s->destruct(s->instance);
    if (!(s->instance = s->construct(outlink->w, outlink->h))) {
        av_log(ctx, AV_LOG_ERROR, "Impossible to load frei0r instance.\n");
        return AVERROR(EINVAL);
    }
    if (!s->params) {
        av_log(ctx, AV_LOG_ERROR, "frei0r filter parameters not set.\n");
        return AVERROR(EINVAL);
    }

    return set_params(ctx, s->params);
}

static int source_request_frame(AVFilterLink *outlink)
{
    Frei0rContext *s = outlink->src->priv;
    AVFrame *frame = ff_default_get_video_buffer2(outlink, outlink->w, outlink->h, 1);

    if (!frame)
        return AVERROR(ENOMEM);

    frame->sample_aspect_ratio = (AVRational) {1, 1};
    frame->pts = s->pts++;
    frame->duration = 1;

    s->update(s->instance, av_rescale_q(frame->pts, s->time_base, (AVRational){1,1000}),
                   NULL, (uint32_t *)frame->data[0]);

    return ff_filter_frame(outlink, frame);
}

static const AVOption frei0r_src_options[] = {
    { "size",          "Dimensions of the generated video.", OFFSET(w),         AV_OPT_TYPE_IMAGE_SIZE, { .str = "320x240" }, .flags = FLAGS },
    { "framerate",     NULL,                                 OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, { .str = "25" }, 0, INT_MAX, .flags = FLAGS },
    { "filter_name",   NULL,                                 OFFSET(dl_name),   AV_OPT_TYPE_STRING,                  .flags = FLAGS },
    { "filter_params", NULL,                                 OFFSET(params),    AV_OPT_TYPE_STRING,                  .flags = FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(frei0r_src);

static const AVFilterPad avfilter_vsrc_frei0r_src_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = source_request_frame,
        .config_props  = source_config_props
    },
};

const AVFilter ff_vsrc_frei0r_src = {
    .name          = "frei0r_src",
    .description   = NULL_IF_CONFIG_SMALL("Generate a frei0r source."),
    .priv_size     = sizeof(Frei0rContext),
    .priv_class    = &frei0r_src_class,
    .init          = source_init,
    .uninit        = uninit,
    .inputs        = NULL,
    FILTER_OUTPUTS(avfilter_vsrc_frei0r_src_outputs),
    FILTER_QUERY_FUNC2(query_formats),
};
