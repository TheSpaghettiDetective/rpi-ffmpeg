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

/**
 * @file
 * deinterlace video filter - V4L2 M2M
 */

#include <drm_fourcc.h>

#include <linux/videodev2.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#define FF_INTERNAL_FIELDS 1
#include "framequeue.h"
#include "filters.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define LOGBUF(buf) if (buf) av_log(NULL, AV_LOG_DEBUG, "%s: type:%d i:%d fd:%d pts:%lld flags:%x field:%d\n", \
                    __func__, buf->q->format.type, buf - &buf->q->buffers[0], \
                    buf->drm_frame.objects[0].fd, v4l2_get_pts(buf), buf->buffer.flags, buf->buffer.field); \
                    else av_log(NULL, AV_LOG_DEBUG, "%s: null buf\n", __func__);

typedef struct V4L2Queue V4L2Queue;
typedef struct DeintV4L2M2MContextShared DeintV4L2M2MContextShared;

typedef struct V4L2PlaneInfo {
    int bytesperline;
    size_t length;
} V4L2PlaneInfo;

typedef struct V4L2Buffer {
    int enqueued;
    int reenqueue;
    int fd;
    struct v4l2_buffer buffer;
    AVFrame frame;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    int num_planes;
    V4L2PlaneInfo plane_info[VIDEO_MAX_PLANES];
    AVDRMFrameDescriptor drm_frame;
    V4L2Queue *q;
} V4L2Buffer;

typedef struct V4L2Queue {
    struct v4l2_format format;
    int num_buffers;
    V4L2Buffer *buffers;
    DeintV4L2M2MContextShared *ctx;
} V4L2Queue;

typedef struct DeintV4L2M2MContextShared {
    int fd;
    int done;
    int width;
    int height;
    int orig_width;
    int orig_height;
    AVRational sample_aspect_ratio;
    atomic_uint refcount;

    AVBufferRef *hw_frames_ctx;

    unsigned int field_order;
    int64_t last_pts;
    int64_t frame_interval;

    V4L2Queue output;
    V4L2Queue capture;
} DeintV4L2M2MContextShared;

typedef struct DeintV4L2M2MContext {
    const AVClass *class;

    DeintV4L2M2MContextShared *shared;
} DeintV4L2M2MContext;

#define USEC_PER_SEC 1000000

static inline void v4l2_set_pts(V4L2Buffer *out, int64_t pts)
{
    if (pts == AV_NOPTS_VALUE)
    {
        out->buffer.timestamp.tv_usec = 0;
        out->buffer.timestamp.tv_sec = 1000000;
    }
    else
    {
        out->buffer.timestamp.tv_usec = pts % USEC_PER_SEC;
        out->buffer.timestamp.tv_sec = pts / USEC_PER_SEC;
    }
av_log(NULL, AV_LOG_DEBUG, "%s: %ld.%ld\n", __func__, out->buffer.timestamp.tv_sec, out->buffer.timestamp.tv_usec);
}

static inline int64_t v4l2_get_pts(V4L2Buffer *avbuf)
{
av_log(NULL, AV_LOG_DEBUG, "%s: %ld.%ld\n", __func__, avbuf->buffer.timestamp.tv_sec, avbuf->buffer.timestamp.tv_usec);
    if (avbuf->buffer.timestamp.tv_sec == 1000000 && avbuf->buffer.timestamp.tv_usec == 0)
    {
        return AV_NOPTS_VALUE;
    }
    else
    {
        return (int64_t)avbuf->buffer.timestamp.tv_sec * USEC_PER_SEC +
                        avbuf->buffer.timestamp.tv_usec;
    }
}


static int deint_v4l2m2m_prepare_context(DeintV4L2M2MContextShared *ctx)
{
    struct v4l2_capability cap;
    int ret;

    memset(&cap, 0, sizeof(cap));
    ret = ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap);
    if (ret < 0)
        return ret;

    if (!(cap.capabilities & V4L2_CAP_STREAMING))
        return AVERROR(EINVAL);

    if (cap.capabilities & V4L2_CAP_VIDEO_M2M) {
        ctx->capture.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ctx->output.format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

        return 0;
    }

    if (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) {
        ctx->capture.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ctx->output.format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

        return 0;
    }

    return AVERROR(EINVAL);
}

static int deint_v4l2m2m_try_format(V4L2Queue *queue)
{
    struct v4l2_format *fmt        = &queue->format;
    DeintV4L2M2MContextShared *ctx = queue->ctx;
    int ret, field;

    ret = ioctl(ctx->fd, VIDIOC_G_FMT, fmt);
    if (ret)
        av_log(NULL, AV_LOG_ERROR, "VIDIOC_G_FMT failed: %d\n", ret);

    if (V4L2_TYPE_IS_OUTPUT(fmt->type))
        field = V4L2_FIELD_INTERLACED_TB;
    else
        field = V4L2_FIELD_NONE;

    if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
        fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
        fmt->fmt.pix_mp.field = field;
        fmt->fmt.pix_mp.width = ctx->width;
        fmt->fmt.pix_mp.height = ctx->height;
    } else {
        fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
        fmt->fmt.pix.field = field;
        fmt->fmt.pix.width = ctx->width;
        fmt->fmt.pix.height = ctx->height;
    }

    av_log(NULL, AV_LOG_DEBUG, "%s: Trying format for type %d, wxh: %dx%d, fmt: %08x, size %u bpl %u pre\n", __func__,
		 fmt->type, fmt->fmt.pix_mp.width, fmt->fmt.pix_mp.height,
		 fmt->fmt.pix_mp.pixelformat,
		 fmt->fmt.pix_mp.plane_fmt[0].sizeimage, fmt->fmt.pix_mp.plane_fmt[0].bytesperline);

    ret = ioctl(ctx->fd, VIDIOC_TRY_FMT, fmt);
    if (ret)
        return AVERROR(EINVAL);

    av_log(NULL, AV_LOG_DEBUG, "%s: Trying format for type %d, wxh: %dx%d, fmt: %08x, size %u bpl %u post\n", __func__,
		 fmt->type, fmt->fmt.pix_mp.width, fmt->fmt.pix_mp.height,
		 fmt->fmt.pix_mp.pixelformat,
		 fmt->fmt.pix_mp.plane_fmt[0].sizeimage, fmt->fmt.pix_mp.plane_fmt[0].bytesperline);

    if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
        if (fmt->fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420 ||
            fmt->fmt.pix_mp.field != field) {
            av_log(NULL, AV_LOG_DEBUG, "format not supported for type %d\n", fmt->type);

            return AVERROR(EINVAL);
        }
    } else {
        if (fmt->fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420 ||
            fmt->fmt.pix.field != field) {
            av_log(NULL, AV_LOG_DEBUG, "format not supported for type %d\n", fmt->type);

            return AVERROR(EINVAL);
        }
    }

    return 0;
}

static int deint_v4l2m2m_set_format(V4L2Queue *queue, uint32_t field, int width, int height, int pitch, int ysize)
{
    struct v4l2_format *fmt        = &queue->format;
    DeintV4L2M2MContextShared *ctx = queue->ctx;
    int ret;

    struct v4l2_selection sel = {
        .type = fmt->type,
        .target = V4L2_TYPE_IS_OUTPUT(fmt->type) ? V4L2_SEL_TGT_CROP_BOUNDS : V4L2_SEL_TGT_COMPOSE_BOUNDS,
    };

    if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
        fmt->fmt.pix_mp.field = field;
        fmt->fmt.pix_mp.width = width;
        fmt->fmt.pix_mp.height = ysize / pitch;
        fmt->fmt.pix_mp.plane_fmt[0].bytesperline = pitch;
        fmt->fmt.pix_mp.plane_fmt[0].sizeimage = ysize + (ysize >> 1);
    } else {
        fmt->fmt.pix.field = field;
        fmt->fmt.pix.width = width;
        fmt->fmt.pix.height = height;
        fmt->fmt.pix.sizeimage = 0;
        fmt->fmt.pix.bytesperline = 0;
    }

    ret = ioctl(ctx->fd, VIDIOC_S_FMT, fmt);
    if (ret)
        av_log(NULL, AV_LOG_ERROR, "VIDIOC_S_FMT failed: %d\n", ret);

    ret = ioctl(ctx->fd, VIDIOC_G_SELECTION, &sel);
    if (ret)
        av_log(NULL, AV_LOG_ERROR, "VIDIOC_G_SELECTION failed: %d\n", ret);

    sel.r.width = width;
    sel.r.height = height;
    sel.r.left = 0;
    sel.r.top = 0;
    sel.target = V4L2_TYPE_IS_OUTPUT(fmt->type) ? V4L2_SEL_TGT_CROP : V4L2_SEL_TGT_COMPOSE,
    sel.flags = V4L2_SEL_FLAG_LE;

    ret = ioctl(ctx->fd, VIDIOC_S_SELECTION, &sel);
    if (ret)
        av_log(NULL, AV_LOG_ERROR, "VIDIOC_S_SELECTION failed: %d\n", ret);

    return ret;
}

static int deint_v4l2m2m_probe_device(DeintV4L2M2MContextShared *ctx, char *node)
{
    int ret;

    ctx->fd = open(node, O_RDWR | O_NONBLOCK, 0);
    if (ctx->fd < 0)
        return AVERROR(errno);

    ret = deint_v4l2m2m_prepare_context(ctx);
    if (ret)
        goto fail;

    ret = deint_v4l2m2m_try_format(&ctx->capture);
    if (ret)
        goto fail;

    ret = deint_v4l2m2m_try_format(&ctx->output);
    if (ret)
        goto fail;

    return 0;

fail:
    close(ctx->fd);
    ctx->fd = -1;

    return ret;
}

static int deint_v4l2m2m_find_device(DeintV4L2M2MContextShared *ctx)
{
    int ret = AVERROR(EINVAL);
    struct dirent *entry;
    char node[PATH_MAX];
    DIR *dirp;

    dirp = opendir("/dev");
    if (!dirp)
        return AVERROR(errno);

    for (entry = readdir(dirp); entry; entry = readdir(dirp)) {

        if (strncmp(entry->d_name, "video", 5))
            continue;

        snprintf(node, sizeof(node), "/dev/%s", entry->d_name);
        av_log(NULL, AV_LOG_DEBUG, "probing device %s\n", node);
        ret = deint_v4l2m2m_probe_device(ctx, node);
        if (!ret)
            break;
    }

    closedir(dirp);

    if (ret) {
        av_log(NULL, AV_LOG_ERROR, "Could not find a valid device\n");
        ctx->fd = -1;

        return ret;
    }

    av_log(NULL, AV_LOG_INFO, "Using device %s\n", node);

    return 0;
}

static int deint_v4l2m2m_enqueue_buffer(V4L2Buffer *buf)
{
    int ret;

    LOGBUF(buf);

    ret = ioctl(buf->q->ctx->fd, VIDIOC_QBUF, &buf->buffer);
    if (ret < 0)
        return AVERROR(errno);

    buf->enqueued = 1;

    return 0;
}

static int v4l2_buffer_export_drm(V4L2Buffer* avbuf)
{
    struct v4l2_exportbuffer expbuf;
    int i, ret;

    for (i = 0; i < avbuf->num_planes; i++) {
        memset(&expbuf, 0, sizeof(expbuf));

        expbuf.index = avbuf->buffer.index;
        expbuf.type = avbuf->buffer.type;
        expbuf.plane = i;

        ret = ioctl(avbuf->q->ctx->fd, VIDIOC_EXPBUF, &expbuf);
        if (ret < 0)
            return AVERROR(errno);

        avbuf->fd = expbuf.fd;

        if (V4L2_TYPE_IS_MULTIPLANAR(avbuf->buffer.type)) {
            /* drm frame */
            avbuf->drm_frame.objects[i].size = avbuf->buffer.m.planes[i].length;
            avbuf->drm_frame.objects[i].fd = expbuf.fd;
            avbuf->drm_frame.objects[i].format_modifier = DRM_FORMAT_MOD_LINEAR;
        } else {
            /* drm frame */
            avbuf->drm_frame.objects[0].size = avbuf->buffer.length;
            avbuf->drm_frame.objects[0].fd = expbuf.fd;
            avbuf->drm_frame.objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
        }
    }

    return 0;
}

static int deint_v4l2m2m_allocate_buffers(V4L2Queue *queue)
{
    struct v4l2_format *fmt = &queue->format;
    DeintV4L2M2MContextShared *ctx = queue->ctx;
    struct v4l2_requestbuffers req;
    int ret, i, j, multiplanar;
    uint32_t memory;

    memory = V4L2_TYPE_IS_OUTPUT(fmt->type) ?
        V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;

    multiplanar = V4L2_TYPE_IS_MULTIPLANAR(fmt->type);

    memset(&req, 0, sizeof(req));
    req.count = queue->num_buffers;
    req.memory = memory;
    req.type = fmt->type;

    ret = ioctl(ctx->fd, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));

        return AVERROR(errno);
    }

    queue->num_buffers = req.count;
    queue->buffers = av_mallocz(queue->num_buffers * sizeof(V4L2Buffer));
    if (!queue->buffers) {
        av_log(NULL, AV_LOG_ERROR, "malloc enomem\n");

        return AVERROR(ENOMEM);
    }

    for (i = 0; i < queue->num_buffers; i++) {
        V4L2Buffer *buf = &queue->buffers[i];

        buf->enqueued = 0;
        buf->fd = -1;
        buf->q = queue;

        buf->buffer.type = fmt->type;
        buf->buffer.memory = memory;
        buf->buffer.index = i;

        if (multiplanar) {
            buf->buffer.length = VIDEO_MAX_PLANES;
            buf->buffer.m.planes = buf->planes;
        }

        ret = ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf->buffer);
        if (ret < 0) {
            ret = AVERROR(errno);

            goto fail;
        }

        if (multiplanar)
            buf->num_planes = buf->buffer.length;
        else
            buf->num_planes = 1;

        for (j = 0; j < buf->num_planes; j++) {
            V4L2PlaneInfo *info = &buf->plane_info[j];

            if (multiplanar) {
                info->bytesperline = fmt->fmt.pix_mp.plane_fmt[j].bytesperline;
                info->length = buf->buffer.m.planes[j].length;
            } else {
                info->bytesperline = fmt->fmt.pix.bytesperline;
                info->length = buf->buffer.length;
            }
        }

        if (!V4L2_TYPE_IS_OUTPUT(fmt->type)) {
            ret = deint_v4l2m2m_enqueue_buffer(buf);
            if (ret)
                goto fail;

            ret = v4l2_buffer_export_drm(buf);
            if (ret)
                goto fail;
        }
        LOGBUF(buf);
    }

    return 0;

fail:
    for (i = 0; i < queue->num_buffers; i++)
        if (queue->buffers[i].fd >= 0)
            close(queue->buffers[i].fd);
    av_free(queue->buffers);
    queue->buffers = NULL;

    return ret;
}

static int deint_v4l2m2m_streamon(V4L2Queue *queue)
{
    int type = queue->format.type;
    int ret;

    ret = ioctl(queue->ctx->fd, VIDIOC_STREAMON, &type);
    av_log(NULL, AV_LOG_DEBUG, "%s: type:%d ret:%d errno:%d\n", __func__, type, ret, AVERROR(errno));
    if (ret < 0)
        return AVERROR(errno);

    return 0;
}

static int deint_v4l2m2m_streamoff(V4L2Queue *queue)
{
    int type = queue->format.type;
    int ret;

    ret = ioctl(queue->ctx->fd, VIDIOC_STREAMOFF, &type);
    av_log(NULL, AV_LOG_DEBUG, "%s: type:%d ret:%d errno:%d\n", __func__, type, ret, AVERROR(errno));
    if (ret < 0)
        return AVERROR(errno);

    return 0;
}

static V4L2Buffer* deint_v4l2m2m_dequeue_buffer(V4L2Queue *queue, int timeout)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    DeintV4L2M2MContextShared *ctx = queue->ctx;
    struct v4l2_buffer buf = { 0 };
    V4L2Buffer* avbuf = NULL;
    struct pollfd pfd;
    short events;
    int ret;

    if (V4L2_TYPE_IS_OUTPUT(queue->format.type))
        events =  POLLOUT | POLLWRNORM;
    else
        events = POLLIN | POLLRDNORM;

    pfd.events = events;
    pfd.fd = ctx->fd;

    for (;;) {
        ret = poll(&pfd, 1, timeout);
        if (ret > 0)
            break;
        if (errno == EINTR)
            continue;
        return NULL;
    }

    if (pfd.revents & POLLERR)
        return NULL;

    if (pfd.revents & events) {
        memset(&buf, 0, sizeof(buf));
        buf.memory = V4L2_MEMORY_MMAP;
        buf.type = queue->format.type;
        if (V4L2_TYPE_IS_MULTIPLANAR(queue->format.type)) {
            memset(planes, 0, sizeof(planes));
            buf.length = VIDEO_MAX_PLANES;
            buf.m.planes = planes;
        }

        ret = ioctl(ctx->fd, VIDIOC_DQBUF, &buf);
        if (ret) {
            if (errno != EAGAIN)
                av_log(NULL, AV_LOG_DEBUG, "VIDIOC_DQBUF, errno (%s)\n",
                       av_err2str(AVERROR(errno)));
            return NULL;
        }

        avbuf = &queue->buffers[buf.index];
        avbuf->enqueued = 0;
        avbuf->buffer = buf;
        if (V4L2_TYPE_IS_MULTIPLANAR(queue->format.type)) {
            memcpy(avbuf->planes, planes, sizeof(planes));
            avbuf->buffer.m.planes = avbuf->planes;
        }
        LOGBUF(avbuf);
        return avbuf;
    }

    return NULL;
}

static V4L2Buffer *deint_v4l2m2m_find_free_buf(V4L2Queue *queue)
{
    int i;
    V4L2Buffer *buf = NULL;

    for (i = 0; i < queue->num_buffers; i++)
        if (!queue->buffers[i].enqueued) {
            buf = &queue->buffers[i];
            break;
        }
    LOGBUF(buf);
    return buf;
}

static void deint_v4l2m2m_unref_queued(V4L2Queue *queue)
{
    int i;
    V4L2Buffer *buf = NULL;

    if (!queue || !queue->buffers)
        return;
    for (i = 0; i < queue->num_buffers; i++) {
        buf = &queue->buffers[i];
        if (queue->buffers[i].enqueued)
            av_frame_unref(&buf->frame);
    }
}

static void recycle_q(V4L2Queue * const queue)
{
    V4L2Buffer* avbuf;
    while (avbuf = deint_v4l2m2m_dequeue_buffer(queue, 0), avbuf) {
        av_frame_unref(&avbuf->frame);
    }
}

static int count_enqueued(V4L2Queue *queue)
{
    int i;
    int n = 0;

    for (i = 0; i < queue->num_buffers; i++)
        if (queue->buffers[i].enqueued)
            ++n;
    return n;
}

static int deint_v4l2m2m_enqueue_frame(V4L2Queue *queue, AVFrame* frame)
{
    AVDRMFrameDescriptor *drm_desc = (AVDRMFrameDescriptor *)frame->data[0];
    V4L2Buffer *buf;
    int i;

    if (V4L2_TYPE_IS_OUTPUT(queue->format.type))
        recycle_q(queue);

    buf = deint_v4l2m2m_find_free_buf(queue);
    if (!buf) {
        av_log(NULL, AV_LOG_ERROR, "%s: error %d finding free buf\n", __func__, 0);
        return AVERROR(EAGAIN);
    }
    if (V4L2_TYPE_IS_MULTIPLANAR(buf->buffer.type))
        for (i = 0; i < drm_desc->nb_objects; i++)
            buf->buffer.m.planes[i].m.fd = drm_desc->objects[i].fd;
    else
        buf->buffer.m.fd = drm_desc->objects[0].fd;

    if (frame->interlaced_frame)
    {
        if (frame->top_field_first)
            buf->buffer.field = V4L2_FIELD_INTERLACED_TB;
        else
            buf->buffer.field = V4L2_FIELD_INTERLACED_BT;
    }

    v4l2_set_pts(buf, frame->pts);

    buf->drm_frame.objects[0].fd = drm_desc->objects[0].fd;

    av_frame_move_ref(&buf->frame, frame);

    return deint_v4l2m2m_enqueue_buffer(buf);
}

static void deint_v4l2m2m_destroy_context(DeintV4L2M2MContextShared *ctx)
{
    if (atomic_fetch_sub(&ctx->refcount, 1) == 1) {
        V4L2Queue *capture = &ctx->capture;
        V4L2Queue *output  = &ctx->output;
        int i;

        av_log(NULL, AV_LOG_DEBUG, "%s - destroying context\n", __func__);

        if (ctx->fd >= 0) {
            deint_v4l2m2m_streamoff(capture);
            deint_v4l2m2m_streamoff(output);
        }

        if (capture->buffers)
            for (i = 0; i < capture->num_buffers; i++) {
                capture->buffers[i].q = NULL;
                if (capture->buffers[i].fd >= 0)
                    close(capture->buffers[i].fd);
            }

        deint_v4l2m2m_unref_queued(output);

        av_buffer_unref(&ctx->hw_frames_ctx);

        if (capture->buffers)
            av_free(capture->buffers);

        if (output->buffers)
            av_free(output->buffers);

        if (ctx->fd >= 0) {
            close(ctx->fd);
            ctx->fd = -1;
        }

        av_free(ctx);
    }
}

static void v4l2_free_buffer(void *opaque, uint8_t *unused)
{
    V4L2Buffer *buf                = opaque;
    DeintV4L2M2MContextShared *ctx = buf->q->ctx;

    LOGBUF(buf);

    if (!ctx->done)
        deint_v4l2m2m_enqueue_buffer(buf);

    deint_v4l2m2m_destroy_context(ctx);
}

static uint8_t * v4l2_get_drm_frame(V4L2Buffer *avbuf, int height)
{
    int av_pix_fmt = AV_PIX_FMT_YUV420P;
    AVDRMFrameDescriptor *drm_desc = &avbuf->drm_frame;
    AVDRMLayerDescriptor *layer;

    /* fill the DRM frame descriptor */
    drm_desc->nb_objects = avbuf->num_planes;
    drm_desc->nb_layers = 1;

    layer = &drm_desc->layers[0];
    layer->nb_planes = avbuf->num_planes;

    for (int i = 0; i < avbuf->num_planes; i++) {
        layer->planes[i].object_index = i;
        layer->planes[i].offset = 0;
        layer->planes[i].pitch = avbuf->plane_info[i].bytesperline;
    }

    switch (av_pix_fmt) {
    case AV_PIX_FMT_YUYV422:

        layer->format = DRM_FORMAT_YUYV;
        layer->nb_planes = 1;

        break;

    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21:

        layer->format = av_pix_fmt == AV_PIX_FMT_NV12 ?
            DRM_FORMAT_NV12 : DRM_FORMAT_NV21;

        if (avbuf->num_planes > 1)
            break;

        layer->nb_planes = 2;

        layer->planes[1].object_index = 0;
        layer->planes[1].offset = avbuf->plane_info[0].bytesperline *
            height;
        layer->planes[1].pitch = avbuf->plane_info[0].bytesperline;
        break;

    case AV_PIX_FMT_YUV420P:

        layer->format = DRM_FORMAT_YUV420;

        if (avbuf->num_planes > 1)
            break;

        layer->nb_planes = 3;

        layer->planes[1].object_index = 0;
        layer->planes[1].offset = avbuf->plane_info[0].bytesperline *
            height;
        layer->planes[1].pitch = avbuf->plane_info[0].bytesperline >> 1;

        layer->planes[2].object_index = 0;
        layer->planes[2].offset = layer->planes[1].offset +
            ((avbuf->plane_info[0].bytesperline *
              height) >> 2);
        layer->planes[2].pitch = avbuf->plane_info[0].bytesperline >> 1;
        break;

    default:
        drm_desc->nb_layers = 0;
        break;
    }

    return (uint8_t *) drm_desc;
}

static int deint_v4l2m2m_dequeue_frame(V4L2Queue *queue, AVFrame* frame, int timeout)
{
    DeintV4L2M2MContextShared *ctx = queue->ctx;
    V4L2Buffer* avbuf;

    av_log(NULL, AV_LOG_TRACE, "<<< %s\n", __func__);

    avbuf = deint_v4l2m2m_dequeue_buffer(queue, timeout);
    if (!avbuf) {
        av_log(NULL, AV_LOG_DEBUG, "%s: No buffer to dequeue (timeout=%d)\n", __func__, timeout);
        return AVERROR(EAGAIN);
    }

    frame->buf[0] = av_buffer_create((uint8_t *) &avbuf->drm_frame,
                            sizeof(avbuf->drm_frame), v4l2_free_buffer,
                            avbuf, AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0]) {
        av_log(NULL, AV_LOG_ERROR, "%s: error %d creating buffer\n", __func__, 0);
        return AVERROR(ENOMEM);
    }

    atomic_fetch_add(&ctx->refcount, 1);

    frame->data[0] = (uint8_t *)v4l2_get_drm_frame(avbuf, ctx->orig_height);
    frame->format = AV_PIX_FMT_DRM_PRIME;
    if (ctx->hw_frames_ctx)
        frame->hw_frames_ctx = av_buffer_ref(ctx->hw_frames_ctx);
    frame->height = ctx->height;
    frame->width = ctx->width;
    frame->sample_aspect_ratio = ctx->sample_aspect_ratio;

    frame->pts = v4l2_get_pts(avbuf);

    if (frame->pts == AV_NOPTS_VALUE || frame->pts == ctx->last_pts)
        frame->pts = ctx->last_pts + ctx->frame_interval;

    frame->best_effort_timestamp = frame->pts;

    ctx->last_pts = frame->pts;

    v4l2_set_pts(avbuf, frame->pts);

    if (avbuf->buffer.flags & V4L2_BUF_FLAG_ERROR) {
        av_log(NULL, AV_LOG_ERROR, "driver decode error\n");
        frame->decode_error_flags |= FF_DECODE_ERROR_INVALID_BITSTREAM;
    }

    LOGBUF(avbuf);
    return 0;
}


static int deint_v4l2m2m_request_frame(AVFilterLink *link)
{
    AVFilterContext *avctx         = link->src;
    DeintV4L2M2MContext *priv      = avctx->priv;
    DeintV4L2M2MContextShared *ctx = priv->shared;
    AVFilterLink *outlink          = avctx->outputs[0];
    AVFrame *output_frame;
    int n;
    int err;

    av_log(priv, AV_LOG_TRACE, "<<< %s\n", __func__);
    av_log(priv, AV_LOG_DEBUG, "--- %s: [src] in status in %d/ot %d; out status in %d/out %d\n", __func__,
           avctx->inputs[0]->status_in, avctx->inputs[0]->status_out, avctx->outputs[0]->status_in, avctx->outputs[0]->status_out);

    if (ff_outlink_get_status(avctx->inputs[0])) {
        av_log(priv, AV_LOG_TRACE, ">>> %s: EOF\n", __func__);
        ff_outlink_set_status(outlink, AVERROR_EOF, avctx->inputs[0]->status_in_pts);
        return 0;
    }

    recycle_q(&ctx->output);
    n = count_enqueued(&ctx->output);

    av_log(priv, AV_LOG_TRACE, "%s: n=%d\n", __func__, n);

    output_frame = av_frame_alloc();
    if (!output_frame) {
        av_log(priv, AV_LOG_ERROR, "%s: error %d allocating frame\n", __func__, err);
        return AVERROR(ENOMEM);
    }

    err = deint_v4l2m2m_dequeue_frame(&ctx->capture, output_frame, n < 5 ? 0 : 10000);
    if (err) {
        if (err != AVERROR(EAGAIN))
            av_log(priv, AV_LOG_ERROR, "%s: deint_v4l2m2m_dequeue_frame error %d\n", __func__, err);
        av_frame_free(&output_frame);
        if (err == AVERROR(EAGAIN)) {
            if (n < 5)
                ff_request_frame(avctx->inputs[0]);
            av_log(priv, AV_LOG_TRACE, ">>> %s: %s\n", __func__, av_err2str(err));
        }
        return err;
    }

    output_frame->interlaced_frame = 0;

    err = ff_filter_frame(outlink, output_frame);
    if (err) {
        av_log(priv, AV_LOG_ERROR, "%s: ff_filter_frame error %d\n", __func__, err);
        av_frame_free(&output_frame);
        return err;
    }

    av_log(priv, AV_LOG_TRACE, ">>> %s: OK\n", __func__);
    return 0;
}

static int deint_v4l2m2m_config_props(AVFilterLink *outlink)
{
    AVFilterLink *inlink           = outlink->src->inputs[0];
    AVFilterContext *avctx         = outlink->src;
    DeintV4L2M2MContext *priv      = avctx->priv;
    DeintV4L2M2MContextShared *ctx = priv->shared;
    int ret;

    ctx->height = avctx->inputs[0]->h;
    ctx->width = avctx->inputs[0]->w;

    av_log(NULL, AV_LOG_DEBUG, "%s: %dx%d\n", __func__, ctx->width, ctx->height);

    outlink->frame_rate = av_mul_q(inlink->frame_rate,
                                   (AVRational){ 2, 1 });
    outlink->time_base  = av_mul_q(inlink->time_base,
                                   (AVRational){ 1, 2 });

    ret = deint_v4l2m2m_find_device(ctx);
    if (ret)
        return ret;

    if (inlink->hw_frames_ctx) {
        ctx->hw_frames_ctx = av_buffer_ref(inlink->hw_frames_ctx);
        if (!ctx->hw_frames_ctx)
            return AVERROR(ENOMEM);
    }
    return 0;
}

static int deint_v4l2m2m_query_formats(AVFilterContext *avctx)
{
    static const enum AVPixelFormat pixel_formats[] = {
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE,
    };

    return ff_set_common_formats(avctx, ff_make_format_list(pixel_formats));
}

static int deint_v4l2m2m_filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *avctx         = link->dst;
    DeintV4L2M2MContext *priv      = avctx->priv;
    DeintV4L2M2MContextShared *ctx = priv->shared;
    V4L2Queue *capture             = &ctx->capture;
    V4L2Queue *output              = &ctx->output;
    int ret;

    av_log(priv, AV_LOG_DEBUG, "<<< %s: input pts: %"PRId64" (%"PRId64") field :%d interlaced: %d aspect:%d/%d\n",
          __func__, in->pts, AV_NOPTS_VALUE, in->top_field_first, in->interlaced_frame, in->sample_aspect_ratio.num, in->sample_aspect_ratio.den);
    av_log(priv, AV_LOG_DEBUG, "--- %s: in status in %d/ot %d; out status in %d/out %d\n", __func__,
           avctx->inputs[0]->status_in, avctx->inputs[0]->status_out, avctx->outputs[0]->status_in, avctx->outputs[0]->status_out);

    ctx->sample_aspect_ratio = in->sample_aspect_ratio;

    if (ctx->field_order == V4L2_FIELD_ANY) {
        AVDRMFrameDescriptor *drm_desc = (AVDRMFrameDescriptor *)in->data[0];
        ctx->orig_width = drm_desc->layers[0].planes[0].pitch;
        ctx->orig_height = drm_desc->layers[0].planes[1].offset / ctx->orig_width;

        av_log(priv, AV_LOG_DEBUG, "%s: %dx%d (%d,%d)\n", __func__, ctx->width, ctx->height,
           drm_desc->layers[0].planes[0].pitch, drm_desc->layers[0].planes[1].offset);

        if (in->top_field_first)
            ctx->field_order = V4L2_FIELD_INTERLACED_TB;
        else
            ctx->field_order = V4L2_FIELD_INTERLACED_BT;

        ret = deint_v4l2m2m_set_format(output, ctx->field_order, ctx->width, ctx->height, ctx->orig_width, drm_desc->layers[0].planes[1].offset);
        if (ret)
            return ret;

        ret = deint_v4l2m2m_set_format(capture, V4L2_FIELD_NONE, ctx->width, ctx->height, ctx->orig_width, drm_desc->layers[0].planes[1].offset);
        if (ret)
            return ret;

        ret = deint_v4l2m2m_allocate_buffers(capture);
        if (ret)
            return ret;

        ret = deint_v4l2m2m_streamon(capture);
        if (ret)
            return ret;

        ret = deint_v4l2m2m_allocate_buffers(output);
        if (ret)
            return ret;

        ret = deint_v4l2m2m_streamon(output);
        if (ret)
            return ret;
    }

    ret = deint_v4l2m2m_enqueue_frame(output, in);

    av_log(priv, AV_LOG_TRACE, ">>> %s: %s\n", __func__, av_err2str(ret));
    return ret;
}

static av_cold int deint_v4l2m2m_init(AVFilterContext *avctx)
{
    DeintV4L2M2MContext *priv = avctx->priv;
    DeintV4L2M2MContextShared *ctx;

    ctx = av_mallocz(sizeof(DeintV4L2M2MContextShared));
    if (!ctx) {
        av_log(priv, AV_LOG_ERROR, "%s: error %d allocating context\n", __func__, 0);
        return AVERROR(ENOMEM);
    }
    priv->shared = ctx;
    ctx->fd = -1;
    ctx->output.ctx = ctx;
    ctx->output.num_buffers = 10;
    ctx->capture.ctx = ctx;
    ctx->capture.num_buffers = 8;
    ctx->done = 0;
    ctx->field_order = V4L2_FIELD_ANY;
    ctx->last_pts = 0;
    ctx->frame_interval = 1000000 / 60;
    atomic_init(&ctx->refcount, 1);

    return 0;
}

static void deint_v4l2m2m_uninit(AVFilterContext *avctx)
{
    DeintV4L2M2MContext *priv = avctx->priv;
    DeintV4L2M2MContextShared *ctx = priv->shared;

    ctx->done = 1;
    deint_v4l2m2m_destroy_context(ctx);
}

static const AVOption deinterlace_v4l2m2m_options[] = {
    { NULL },
};

AVFILTER_DEFINE_CLASS(deinterlace_v4l2m2m);

static const AVFilterPad deint_v4l2m2m_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = deint_v4l2m2m_filter_frame,
    },
    { NULL }
};

static const AVFilterPad deint_v4l2m2m_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = deint_v4l2m2m_config_props,
        .request_frame = deint_v4l2m2m_request_frame,
    },
    { NULL }
};

AVFilter ff_vf_deinterlace_v4l2m2m = {
    .name           = "deinterlace_v4l2m2m",
    .description    = NULL_IF_CONFIG_SMALL("V4L2 M2M deinterlacer"),
    .priv_size      = sizeof(DeintV4L2M2MContext),
    .init           = &deint_v4l2m2m_init,
    .uninit         = &deint_v4l2m2m_uninit,
    .query_formats  = &deint_v4l2m2m_query_formats,
    .inputs         = deint_v4l2m2m_inputs,
    .outputs        = deint_v4l2m2m_outputs,
    .priv_class     = &deinterlace_v4l2m2m_class,
};
