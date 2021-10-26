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

typedef struct pts_stats_s
{
    void * logctx;
    const char * name;  // For debug
    unsigned int last_count;
    unsigned int last_interval;
    int64_t last_pts;
} pts_stats_t;

#define PTS_TRACK_SIZE 32
typedef struct pts_track_el_s
{
    uint32_t n;
    unsigned int interval;
    AVFrame * props;
} pts_track_el_t;

typedef struct pts_track_s
{
    uint32_t n;
    uint32_t last_n;
    int got_2;
    void * logctx;
    pts_stats_t stats;
    pts_track_el_t a[PTS_TRACK_SIZE];
} pts_track_t;

typedef struct DeintV4L2M2MContextShared {
    void * logctx;  // For logging - will be NULL when done

    int fd;
    int done;
    int width;
    int height;
    int orig_width;
    int orig_height;
    atomic_uint refcount;

    AVBufferRef *hw_frames_ctx;

    unsigned int field_order;

    pts_track_t track;

    V4L2Queue output;
    V4L2Queue capture;
} DeintV4L2M2MContextShared;

typedef struct DeintV4L2M2MContext {
    const AVClass *class;

    DeintV4L2M2MContextShared *shared;
} DeintV4L2M2MContext;

static unsigned int pts_stats_interval(const pts_stats_t * const stats)
{
    return stats->last_interval;
}

// Pick 64 for max last count - that is >1sec at 60fps
#define STATS_LAST_COUNT_MAX 64
#define STATS_INTERVAL_MAX (1 << 30)
static void pts_stats_add(pts_stats_t * const stats, int64_t pts)
{
    if (pts == AV_NOPTS_VALUE || pts == stats->last_pts) {
        if (stats->last_count < STATS_LAST_COUNT_MAX)
            ++stats->last_count;
        return;
    }

    if (stats->last_pts != AV_NOPTS_VALUE) {
        const int64_t interval = pts - stats->last_pts;

        if (interval < 0 || interval >= STATS_INTERVAL_MAX ||
            stats->last_count >= STATS_LAST_COUNT_MAX) {
            if (stats->last_interval != 0)
                av_log(stats->logctx, AV_LOG_DEBUG, "%s: %s: Bad interval: %" PRId64 "/%d\n",
                       __func__, stats->name, interval, stats->last_count);
            stats->last_interval = 0;
        }
        else {
            const int64_t frame_time = interval / (int64_t)stats->last_count;

            if (frame_time != stats->last_interval)
                av_log(stats->logctx, AV_LOG_DEBUG, "%s: %s: New interval: %u->%" PRId64 "/%d=%" PRId64 "\n",
                       __func__, stats->name, stats->last_interval, interval, stats->last_count, frame_time);
            stats->last_interval = frame_time;
        }
    }

    stats->last_pts = pts;
    stats->last_count = 1;
}

static void pts_stats_init(pts_stats_t * const stats, void * logctx, const char * name)
{
    *stats = (pts_stats_t){
        .logctx = logctx,
        .name = name,
        .last_count = 1,
        .last_interval = 0,
        .last_pts = AV_NOPTS_VALUE
    };
}

static inline uint32_t pts_track_next_n(pts_track_t * const trk)
{
    if (++trk->n == 0)
        trk->n = 1;
    return trk->n;
}

static int pts_track_get_frame(pts_track_t * const trk, const struct timeval tv, AVFrame * const dst)
{
    uint32_t n = (uint32_t)(tv.tv_usec / 2 + tv.tv_sec * 500000);
    pts_track_el_t * t;

    // As a first guess assume that n==0 means last frame
    if (n == 0) {
        n = trk->last_n;
        if (n == 0)
            goto fail;
    }

    t = trk->a + (n & (PTS_TRACK_SIZE - 1));

    if (t->n != n) {
        av_log(trk->logctx, AV_LOG_ERROR, "%s: track failure: got %u, expected %u\n", __func__, n, trk->n);
        goto fail;
    }

    // 1st frame is simple - just believe it
    if (n != trk->last_n) {
        trk->last_n = n;
        trk->got_2 = 0;
        return av_frame_copy_props(dst, t->props);
    }

    // Only believe in a single interpolated frame
    if (trk->got_2)
        goto fail;
    trk->got_2 = 1;

    av_frame_copy_props(dst, t->props);


    // If we can't guess - don't
    if (t->interval == 0) {
        dst->best_effort_timestamp = AV_NOPTS_VALUE;
        dst->pts = AV_NOPTS_VALUE;
        dst->pkt_dts = AV_NOPTS_VALUE;
    }
    else {
        if (dst->best_effort_timestamp != AV_NOPTS_VALUE)
            dst->best_effort_timestamp += t->interval / 2;
        if (dst->pts != AV_NOPTS_VALUE)
            dst->pts += t->interval / 2;
        if (dst->pkt_dts != AV_NOPTS_VALUE)
            dst->pkt_dts += t->interval / 2;
    }

    return 0;

fail:
    trk->last_n = 0;
    trk->got_2 = 0;
    dst->pts = AV_NOPTS_VALUE;
    dst->pkt_dts = AV_NOPTS_VALUE;
    return 0;
}

static struct timeval pts_track_add_frame(pts_track_t * const trk, const AVFrame * const src)
{
    const uint32_t n = pts_track_next_n(trk);
    pts_track_el_t * const t = trk->a + (n & (PTS_TRACK_SIZE - 1));

    pts_stats_add(&trk->stats, src->pts);

    t->n = n;
    t->interval = pts_stats_interval(&trk->stats); // guess that next interval is the same as the last
    av_frame_unref(t->props);
    av_frame_copy_props(t->props, src);

    // We now know what the previous interval was, rather than having to guess,
    // so set it.  There is a better than decent chance that this is before
    // we use it.
    if (t->interval != 0) {
        pts_track_el_t * const prev_t = trk->a + ((n - 1) & (PTS_TRACK_SIZE - 1));
        prev_t->interval = t->interval;
    }

    // In case deinterlace interpolates frames use every other usec
    return (struct timeval){.tv_sec = n / 500000, .tv_usec = (n % 500000) * 2};
}

static void pts_track_uninit(pts_track_t * const trk)
{
    unsigned int i;
    for (i = 0; i != PTS_TRACK_SIZE; ++i) {
        trk->a[i].n = 0;
        av_frame_free(&trk->a[i].props);
    }
}

static int pts_track_init(pts_track_t * const trk, void *logctx)
{
    unsigned int i;
    trk->n = 1;
    pts_stats_init(&trk->stats, logctx, "track");
    for (i = 0; i != PTS_TRACK_SIZE; ++i) {
        trk->a[i].n = 0;
        if ((trk->a[i].props = av_frame_alloc()) == NULL) {
            pts_track_uninit(trk);
            return AVERROR(ENOMEM);
        }
    }
    return 0;
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
        av_log(ctx->logctx, AV_LOG_ERROR, "VIDIOC_G_FMT failed: %d\n", ret);

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

    av_log(ctx->logctx, AV_LOG_DEBUG, "%s: Trying format for type %d, wxh: %dx%d, fmt: %08x, size %u bpl %u pre\n", __func__,
		 fmt->type, fmt->fmt.pix_mp.width, fmt->fmt.pix_mp.height,
		 fmt->fmt.pix_mp.pixelformat,
		 fmt->fmt.pix_mp.plane_fmt[0].sizeimage, fmt->fmt.pix_mp.plane_fmt[0].bytesperline);

    ret = ioctl(ctx->fd, VIDIOC_TRY_FMT, fmt);
    if (ret)
        return AVERROR(EINVAL);

    av_log(ctx->logctx, AV_LOG_DEBUG, "%s: Trying format for type %d, wxh: %dx%d, fmt: %08x, size %u bpl %u post\n", __func__,
		 fmt->type, fmt->fmt.pix_mp.width, fmt->fmt.pix_mp.height,
		 fmt->fmt.pix_mp.pixelformat,
		 fmt->fmt.pix_mp.plane_fmt[0].sizeimage, fmt->fmt.pix_mp.plane_fmt[0].bytesperline);

    if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
        if (fmt->fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420 ||
            fmt->fmt.pix_mp.field != field) {
            av_log(ctx->logctx, AV_LOG_DEBUG, "format not supported for type %d\n", fmt->type);

            return AVERROR(EINVAL);
        }
    } else {
        if (fmt->fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420 ||
            fmt->fmt.pix.field != field) {
            av_log(ctx->logctx, AV_LOG_DEBUG, "format not supported for type %d\n", fmt->type);

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
        av_log(ctx->logctx, AV_LOG_ERROR, "VIDIOC_S_FMT failed: %d\n", ret);

    ret = ioctl(ctx->fd, VIDIOC_G_SELECTION, &sel);
    if (ret)
        av_log(ctx->logctx, AV_LOG_ERROR, "VIDIOC_G_SELECTION failed: %d\n", ret);

    sel.r.width = width;
    sel.r.height = height;
    sel.r.left = 0;
    sel.r.top = 0;
    sel.target = V4L2_TYPE_IS_OUTPUT(fmt->type) ? V4L2_SEL_TGT_CROP : V4L2_SEL_TGT_COMPOSE,
    sel.flags = V4L2_SEL_FLAG_LE;

    ret = ioctl(ctx->fd, VIDIOC_S_SELECTION, &sel);
    if (ret)
        av_log(ctx->logctx, AV_LOG_ERROR, "VIDIOC_S_SELECTION failed: %d\n", ret);

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
        av_log(ctx->logctx, AV_LOG_DEBUG, "probing device %s\n", node);
        ret = deint_v4l2m2m_probe_device(ctx, node);
        if (!ret)
            break;
    }

    closedir(dirp);

    if (ret) {
        av_log(ctx->logctx, AV_LOG_ERROR, "Could not find a valid device\n");
        ctx->fd = -1;

        return ret;
    }

    av_log(ctx->logctx, AV_LOG_INFO, "Using device %s\n", node);

    return 0;
}

static int deint_v4l2m2m_enqueue_buffer(V4L2Buffer *buf)
{
    int ret;

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
        av_log(ctx->logctx, AV_LOG_ERROR, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));

        return AVERROR(errno);
    }

    queue->num_buffers = req.count;
    queue->buffers = av_mallocz(queue->num_buffers * sizeof(V4L2Buffer));
    if (!queue->buffers) {
        av_log(ctx->logctx, AV_LOG_ERROR, "malloc enomem\n");

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
    DeintV4L2M2MContextShared * const ctx = queue->ctx;
    int type = queue->format.type;
    int ret;

    ret = ioctl(ctx->fd, VIDIOC_STREAMON, &type);
    av_log(ctx->logctx, AV_LOG_DEBUG, "%s: type:%d ret:%d errno:%d\n", __func__, type, ret, AVERROR(errno));
    if (ret < 0)
        return AVERROR(errno);

    return 0;
}

static int deint_v4l2m2m_streamoff(V4L2Queue *queue)
{
    DeintV4L2M2MContextShared * const ctx = queue->ctx;
    int type = queue->format.type;
    int ret;

    ret = ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    av_log(ctx->logctx, AV_LOG_DEBUG, "%s: type:%d ret:%d errno:%d\n", __func__, type, ret, AVERROR(errno));
    if (ret < 0)
        return AVERROR(errno);

    return 0;
}

// timeout in ms
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
                av_log(ctx->logctx, AV_LOG_DEBUG, "VIDIOC_DQBUF, errno (%s)\n",
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

    if (queue->buffers == NULL)
        return 0;

    for (i = 0; i < queue->num_buffers; i++)
        if (queue->buffers[i].enqueued)
            ++n;
    return n;
}

static int deint_v4l2m2m_enqueue_frame(V4L2Queue * const queue, AVFrame * const frame)
{
    DeintV4L2M2MContextShared *const ctx = queue->ctx;
    AVDRMFrameDescriptor *drm_desc = (AVDRMFrameDescriptor *)frame->data[0];
    V4L2Buffer *buf;
    int i;

    if (V4L2_TYPE_IS_OUTPUT(queue->format.type))
        recycle_q(queue);

    buf = deint_v4l2m2m_find_free_buf(queue);
    if (!buf) {
        av_log(ctx->logctx, AV_LOG_ERROR, "%s: error %d finding free buf\n", __func__, 0);
        return AVERROR(EAGAIN);
    }
    if (V4L2_TYPE_IS_MULTIPLANAR(buf->buffer.type))
        for (i = 0; i < drm_desc->nb_objects; i++)
            buf->buffer.m.planes[i].m.fd = drm_desc->objects[i].fd;
    else
        buf->buffer.m.fd = drm_desc->objects[0].fd;

    buf->buffer.field = !frame->interlaced_frame ? V4L2_FIELD_NONE :
        frame->top_field_first ? V4L2_FIELD_INTERLACED_TB :
            V4L2_FIELD_INTERLACED_BT;

    if (ctx->field_order != buf->buffer.field) {
        av_log(ctx->logctx, AV_LOG_DEBUG, "%s: Field changed: %d->%d\n", __func__, ctx->field_order, buf->buffer.field);
        ctx->field_order = buf->buffer.field;
    }

    buf->buffer.timestamp = pts_track_add_frame(&ctx->track, frame);

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

// timeout in ms
static int deint_v4l2m2m_dequeue_frame(V4L2Queue *queue, AVFrame* frame, int timeout)
{
    DeintV4L2M2MContextShared *ctx = queue->ctx;
    V4L2Buffer* avbuf;

    av_log(ctx->logctx, AV_LOG_TRACE, "<<< %s\n", __func__);

    avbuf = deint_v4l2m2m_dequeue_buffer(queue, timeout);
    if (!avbuf) {
        av_log(ctx->logctx, AV_LOG_DEBUG, "%s: No buffer to dequeue (timeout=%d)\n", __func__, timeout);
        return AVERROR(EAGAIN);
    }

    // Fill in PTS and anciliary info from src frame
    // we will want to overwrite some fields as only the pts/dts
    // fields are updated with new timing in this fn
    pts_track_get_frame(&ctx->track, avbuf->buffer.timestamp, frame);

    frame->buf[0] = av_buffer_create((uint8_t *) &avbuf->drm_frame,
                            sizeof(avbuf->drm_frame), v4l2_free_buffer,
                            avbuf, AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0]) {
        av_log(ctx->logctx, AV_LOG_ERROR, "%s: error %d creating buffer\n", __func__, 0);
        return AVERROR(ENOMEM);
    }

    atomic_fetch_add(&ctx->refcount, 1);

    frame->data[0] = (uint8_t *)v4l2_get_drm_frame(avbuf, ctx->orig_height);
    frame->format = AV_PIX_FMT_DRM_PRIME;
    if (ctx->hw_frames_ctx)
        frame->hw_frames_ctx = av_buffer_ref(ctx->hw_frames_ctx);
    frame->height = ctx->height;
    frame->width = ctx->width;

    // Not interlaced now
    frame->interlaced_frame = 0;
    frame->top_field_first = 0;
    // Pkt duration halved
    frame->pkt_duration /= 2;

    if (avbuf->buffer.flags & V4L2_BUF_FLAG_ERROR) {
        av_log(ctx->logctx, AV_LOG_ERROR, "driver decode error\n");
        frame->decode_error_flags |= FF_DECODE_ERROR_INVALID_BITSTREAM;
    }

    av_log(ctx->logctx, AV_LOG_TRACE, ">>> %s: PTS=%"PRId64"\n", __func__, frame->pts);
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

    av_log(priv, AV_LOG_DEBUG, "%s: %dx%d\n", __func__, ctx->width, ctx->height);

    outlink->time_base           = inlink->time_base;
    outlink->w                   = inlink->w;
    outlink->h                   = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outlink->format              = inlink->format;
    outlink->frame_rate = (AVRational) {1, 0};  // Deny knowledge of frame rate

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

static int deint_v4l2m2m_activate(AVFilterContext *avctx)
{
    DeintV4L2M2MContext * const priv = avctx->priv;
    DeintV4L2M2MContextShared *const s = priv->shared;
    AVFilterLink * const outlink = avctx->outputs[0];
    AVFilterLink * const inlink = avctx->inputs[0];
    int n = 0;
    int cn = 99;
    int instatus = 0;
    int64_t inpts = 0;
    int did_something = 0;

    av_log(priv, AV_LOG_TRACE, "<<< %s\n", __func__);

    FF_FILTER_FORWARD_STATUS_BACK_ALL(outlink, avctx);

    ff_inlink_acknowledge_status(inlink, &instatus, &inpts);

    if (!ff_outlink_frame_wanted(outlink)) {
        av_log(priv, AV_LOG_TRACE, "%s: Not wanted out\n", __func__);
    }
    else if (s->field_order != V4L2_FIELD_ANY)  // Can't DQ if no setup!
    {
        AVFrame * frame = av_frame_alloc();
        int rv;

again:
        recycle_q(&s->output);
        n = count_enqueued(&s->output);

        if (frame == NULL) {
            av_log(priv, AV_LOG_ERROR, "%s: error allocating frame\n", __func__);
            return AVERROR(ENOMEM);
        }

        rv = deint_v4l2m2m_dequeue_frame(&s->capture, frame, n > 4 ? 300 : 0);
        if (rv != 0) {
            av_frame_free(&frame);
            if (rv != AVERROR(EAGAIN)) {
                av_log(priv, AV_LOG_ERROR, ">>> %s: DQ fail: %s\n", __func__, av_err2str(rv));
                return rv;
            }
        }
        else {
            frame->interlaced_frame = 0;
            // frame is always consumed by filter_frame - even on error despite
            // a somewhat confusing comment in the header
            rv = ff_filter_frame(outlink, frame);

            if (instatus != 0) {
                av_log(priv, AV_LOG_TRACE, "%s: eof loop\n", __func__);
                goto again;
            }

            av_log(priv, AV_LOG_TRACE, "%s: Filtered: %s\n", __func__, av_err2str(rv));
            did_something = 1;
        }

        cn = count_enqueued(&s->capture);
    }

    if (instatus != 0) {
        ff_outlink_set_status(outlink, instatus, inpts);
        av_log(priv, AV_LOG_TRACE, ">>> %s: Status done: %s\n", __func__, av_err2str(instatus));
        return 0;
    }

    {
        AVFrame * frame;
        int rv;

        recycle_q(&s->output);
        n = count_enqueued(&s->output);

        while (n < 6) {
            if ((rv = ff_inlink_consume_frame(inlink, &frame)) < 0) {
                av_log(priv, AV_LOG_ERROR, "%s: consume in failed: %s\n", __func__, av_err2str(rv));
                return rv;
            }

            if (frame == NULL) {
                av_log(priv, AV_LOG_TRACE, "%s: No frame\n", __func__);
                break;
            }

            deint_v4l2m2m_filter_frame(inlink, frame);
            av_log(priv, AV_LOG_TRACE, "%s: Q frame\n", __func__);
            ++n;
        }
    }

    if (n < 6) {
        ff_inlink_request_frame(inlink);
        did_something = 1;
        av_log(priv, AV_LOG_TRACE, "%s: req frame\n", __func__);
    }

    if (n > 4 && ff_outlink_frame_wanted(outlink)) {
        ff_filter_set_ready(avctx, 1);
        did_something = 1;
        av_log(priv, AV_LOG_TRACE, "%s: ready\n", __func__);
    }

    av_log(priv, AV_LOG_TRACE, ">>> %s: OK (n=%d, cn=%d)\n", __func__, n, cn);
    return did_something ? 0 : FFERROR_NOT_READY;
}

static av_cold int deint_v4l2m2m_init(AVFilterContext *avctx)
{
    DeintV4L2M2MContext * const priv = avctx->priv;
    DeintV4L2M2MContextShared * const ctx = av_mallocz(sizeof(DeintV4L2M2MContextShared));

    if (!ctx) {
        av_log(priv, AV_LOG_ERROR, "%s: error %d allocating context\n", __func__, 0);
        return AVERROR(ENOMEM);
    }
    priv->shared = ctx;
    ctx->logctx = priv;
    ctx->fd = -1;
    ctx->output.ctx = ctx;
    ctx->output.num_buffers = 8;
    ctx->capture.ctx = ctx;
    ctx->capture.num_buffers = 12;
    ctx->done = 0;
    ctx->field_order = V4L2_FIELD_ANY;

    pts_track_init(&ctx->track, priv);

    atomic_init(&ctx->refcount, 1);

    return 0;
}

static void deint_v4l2m2m_uninit(AVFilterContext *avctx)
{
    DeintV4L2M2MContext *priv = avctx->priv;
    DeintV4L2M2MContextShared *ctx = priv->shared;

    ctx->done = 1;
    ctx->logctx = NULL;  // Log to NULL works, log to missing crashes
    pts_track_uninit(&ctx->track);
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
    },
    { NULL }
};

static const AVFilterPad deint_v4l2m2m_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = deint_v4l2m2m_config_props,
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
    .activate       = deint_v4l2m2m_activate,
};
