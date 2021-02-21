#include "video_buffer.h"

#include <assert.h>
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>

#include "util/log.h"

bool
video_buffer_init(struct video_buffer *vb, bool wait_consumer,
                  const struct video_buffer_callbacks *cbs,
                  void *cbs_userdata) {
    vb->pending_frame = av_frame_alloc();
    if (!vb->pending_frame) {
        goto error_0;
    }

    vb->consumer_frame = av_frame_alloc();
    if (!vb->consumer_frame) {
        goto error_1;
    }

    bool ok = sc_mutex_init(&vb->mutex);
    if (!ok) {
        goto error_2;
    }

    vb->wait_consumer = wait_consumer;
    if (wait_consumer) {
        ok = sc_cond_init(&vb->pending_frame_consumed_cond);
        if (!ok) {
            sc_mutex_destroy(&vb->mutex);
            goto error_2;
        }
        // interrupted is not used if expired frames are not rendered
        // since offering a frame will never block
        vb->interrupted = false;
    }

    // there is initially no rendering frame, so consider it has already been
    // consumed
    vb->pending_frame_consumed = true;

    vb->skipped = 0;

    assert(cbs);
    assert(cbs->on_frame_available);
    vb->cbs = cbs;
    vb->cbs_userdata = cbs_userdata;

    return true;

error_2:
    av_frame_free(&vb->consumer_frame);
error_1:
    av_frame_free(&vb->pending_frame);
error_0:
    return false;
}

void
video_buffer_destroy(struct video_buffer *vb) {
    if (vb->wait_consumer) {
        sc_cond_destroy(&vb->pending_frame_consumed_cond);
    }
    sc_mutex_destroy(&vb->mutex);
    av_frame_free(&vb->consumer_frame);
    av_frame_free(&vb->pending_frame);
}

static inline void
swap_frames(AVFrame **lhs, AVFrame **rhs) {
    AVFrame *tmp = *lhs;
    *lhs = *rhs;
    *rhs = tmp;
}

void
video_buffer_producer_offer_frame(struct video_buffer *vb, AVFrame **pframe) {
    sc_mutex_lock(&vb->mutex);
    if (vb->wait_consumer) {
        // wait for the current (expired) frame to be consumed
        while (!vb->pending_frame_consumed && !vb->interrupted) {
            sc_cond_wait(&vb->pending_frame_consumed_cond, &vb->mutex);
        }
    }

    av_frame_unref(vb->pending_frame);
    swap_frames(pframe, &vb->pending_frame);

    bool skipped = !vb->pending_frame_consumed;
    if (skipped) {
        ++vb->skipped;
    }

    vb->pending_frame_consumed = false;

    sc_mutex_unlock(&vb->mutex);

    if (!skipped) {
        // If skipped, then the previous call will consume this frame, the
        // callback must not be called
        vb->cbs->on_frame_available(vb, vb->cbs_userdata);
    }
}

const AVFrame *
video_buffer_consumer_take_frame(struct video_buffer *vb, unsigned *skipped) {
    sc_mutex_lock(&vb->mutex);
    assert(!vb->pending_frame_consumed);
    vb->pending_frame_consumed = true;

    swap_frames(&vb->consumer_frame, &vb->pending_frame);
    av_frame_unref(vb->pending_frame);

    if (vb->wait_consumer) {
        // unblock video_buffer_offer_decoded_frame()
        sc_cond_signal(&vb->pending_frame_consumed_cond);
    }

    if (skipped) {
        *skipped = vb->skipped;
    }
    vb->skipped = 0; // reset

    sc_mutex_unlock(&vb->mutex);

    // consumer_frame is only written from this thread, no need to lock
    return vb->consumer_frame;
}

void
video_buffer_interrupt(struct video_buffer *vb) {
    if (vb->wait_consumer) {
        sc_mutex_lock(&vb->mutex);
        vb->interrupted = true;
        sc_mutex_unlock(&vb->mutex);
        // wake up blocking wait
        sc_cond_signal(&vb->pending_frame_consumed_cond);
    }
}
