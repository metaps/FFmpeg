/*
 * Android MediaCodec encoder
 *
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

#include <string.h>
#include <sys/types.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/fifo.h"
#include "libavutil/log.h"
#include "libavutil/pixfmt.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"
#include "libavutil/pixdesc.h"

#include "avcodec.h"
#include "internal.h"

#include "mediacodec_sw_buffer.h"
#include "mediacodec_wrapper.h"
#include "mediacodecenc.h"

#define INPUT_DEQUEUE_TIMEOUT_US 8000
#define OUTPUT_DEQUEUE_TIMEOUT_US 8000

enum {
    COLOR_FormatYUV420Planar                              = 0x13,
    COLOR_FormatYUV420SemiPlanar                          = 0x15,
};

static const struct {

    enum AVPixelFormat pix_fmt;
    int color_format;

} color_formats[] = {

    { AV_PIX_FMT_YUV420P, COLOR_FormatYUV420Planar},
    { AV_PIX_FMT_NV12, COLOR_FormatYUV420SemiPlanar},
    { 0 }
};

static int mcdec_map_pixel_format(AVCodecContext *avctx,
        MediaCodecEncContext *s,
        enum AVPixelFormat pix_fmt)
{
    int i;
    int ret = 0;

    for (i = 0; i < FF_ARRAY_ELEMS(color_formats); i++) {
        if (color_formats[i].pix_fmt == pix_fmt) {
            return color_formats[i].color_format;
        }
    }

    av_log(avctx, AV_LOG_ERROR, "Output pix format 0x%x (value=%d) is not supported\n",
        pix_fmt, pix_fmt);

    return ret;
}

static int mediacodec_wrap_frame(AVCodecContext *avctx,
                                  MediaCodecEncContext *s,
                                  uint8_t *data,
                                  size_t size,
                                  ssize_t index,
                                  AVFrame *frame)
{
    int ret = 0;
    int status = 0;

    /*
    av_log(avctx, AV_LOG_DEBUG,
            "Frame: width=%d height=%d pix_fmt=%s\n"
            "source frame linesizes=%d,%d,%d.\n" ,
            avctx->width, avctx->height, av_get_pix_fmt_name(avctx->pix_fmt),
            frame->linesize[0], frame->linesize[1], frame->linesize[2]);
            */

    switch (s->color_format) {
    case COLOR_FormatYUV420Planar:
        ff_mediacodec_sw_frame_copy_yuv420_planar(avctx, s, data, &size, frame);
        break;
    case COLOR_FormatYUV420SemiPlanar:
        ff_mediacodec_sw_frame_copy_yuv420_semi_planar(avctx, s, data, &size, frame);
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported color format 0x%x (value=%d)\n",
            s->color_format, s->color_format);
        ret = AVERROR(EINVAL);
        goto done;
    }

    ret = 0;
done:
    status = ff_AMediaCodec_releaseOutputBuffer(s->codec, index, 0);
    if (status < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
        ret = AVERROR_EXTERNAL;
    }

    return ret;
}

static int mediacodec_enc_parse_format(AVCodecContext *avctx, MediaCodecEncContext *s)
{
    return 0;
}

int ff_mediacodec_enc_init(AVCodecContext *avctx, MediaCodecEncContext *s,
                           const char *mime, FFAMediaFormat *format)
{
    int ret = 0;
    int status = 0;

    s->first_buffer = 0;

    s->codec = ff_AMediaCodec_createEncoderByType(mime);
    if (!s->codec) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create media encoder for type %s\n", mime);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    status = ff_AMediaCodec_configure(s->codec, format, NULL, NULL, 1);
    if (status < 0) {
        char *desc = ff_AMediaFormat_toString(format);
        av_log(avctx, AV_LOG_ERROR,
            "Failed to configure codec (status = %d) with format %s\n",
            status, desc);
        av_freep(&desc);

        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    status = ff_AMediaCodec_start(s->codec);
    if (status < 0) {
        char *desc = ff_AMediaFormat_toString(format);
        av_log(avctx, AV_LOG_ERROR,
            "Failed to start codec (status = %d) with format %s\n",
            status, desc);
        av_freep(&desc);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    av_log(avctx, AV_LOG_DEBUG, "MediaCodec encoder %p started successfully\n", s->codec);

    return 0;

fail:
    av_log(avctx, AV_LOG_ERROR, "MediaCodec encoder %p failed to start\n", s->codec);
    ff_mediacodec_enc_close(avctx, s);
    return ret;
}

int ff_mediacodec_enc_encode(AVCodecContext *avctx, MediaCodecEncContext *s,
                             AVPacket *pkt, AVFrame *frame,
                             int *got_packet, int frame_size)
{
    int ret = 0;
    int offset = 0;
    int need_flushing = 0;

    int status = 0;

    FFAMediaCodec *codec = s->codec;
    FFAMediaCodecBufferInfo info = { 0 };

    ssize_t index;
    size_t size;

    uint8_t *data;

    int64_t input_dequeue_timeout_us = INPUT_DEQUEUE_TIMEOUT_US;
    int64_t output_dequeue_timeout_us = OUTPUT_DEQUEUE_TIMEOUT_US;

    while (offset < frame_size || frame_size == 0) {

        index = ff_AMediaCodec_dequeueInputBuffer(codec, input_dequeue_timeout_us);

        if (ff_AMediaCodec_infoTryAgainLater(codec, index)) {
            av_log(avctx, AV_LOG_ERROR, "[e]Try again to dequeue input buffer.\n");
            break;
        }

        if (index < 0) {
            av_log(avctx, AV_LOG_ERROR,
                    "[e]Failed to dequeue input buffer (status=%zd)\n", index);
            return AVERROR_EXTERNAL;
        }

        // get DirectBufferAddress of InputBuffer
        data = ff_AMediaCodec_getInputBuffer(codec, index, &size);
        if (!data) {
            av_log(avctx, AV_LOG_ERROR, "[e]Failed to get input buffer\n");
            return AVERROR_EXTERNAL;
        }
        /*
        av_log(avctx, AV_LOG_DEBUG,
                "[e][log][MC] format=%s size=%d index=%d pts=%"PRId64" data=%"PRIu8" ..\n" ,
                av_get_pix_fmt_name(avctx->pix_fmt),
                size, index, frame->pts, data);
                */

        // copy frame -> data(Java DirectByteBuffer)
        /*
        if ((ret = mediacodec_wrap_frame(avctx, s, data, &size, index, frame)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to wrap frame\n");
            return ret;
        }
        */
        if (frame_size != 0) {
            size = FFMIN(frame_size - offset, size);

            //av_fifo_generic_read(s->fifo, data, size, NULL);

            // nv12
            memcpy(data, frame->data[0], s->width * s->height);
            memcpy(data + s->width * s->height, frame->data[1], s->width * s->height / 2);

            offset += size;

            int64_t pts_us = frame->pts * av_q2d(avctx->time_base) * 1000 * 1000;

            av_log(avctx, AV_LOG_DEBUG, "[e][log][I] pts=%"PRIi64" pts_us=%"PRIi64" "
                    "size=%d frame_size=%d ..\n",
                    frame->pts, pts_us, size, frame_size);

            status = ff_AMediaCodec_queueInputBuffer(codec, index, 0, size, pts_us, 0);

            if (status < 0) {
                av_log(avctx, AV_LOG_ERROR,
                        "[e]Failed to queue input buffer (status = %d)\n", status);
                return AVERROR_EXTERNAL;
            }

            s->queued_buffer_nb++;
            if (s->queued_buffer_nb > s->queued_buffer_max) {
                s->queued_buffer_max = s->queued_buffer_nb;
            }
        } else {
            uint32_t flags = ff_AMediaCodec_getBufferFlagEndOfStream(codec);

            av_log(avctx, AV_LOG_DEBUG, "Sending End Of Stream signal\n");

            status = ff_AMediaCodec_queueInputBuffer(codec, index, 0, 0, 0, flags);

            if (status < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to queue input empty buffer (status = %d)\n", status);
                return AVERROR_EXTERNAL;
            }

            break;
        }

    }

    index = ff_AMediaCodec_dequeueOutputBuffer(codec, &info, output_dequeue_timeout_us);

    av_log(avctx, AV_LOG_DEBUG, "[e][log][O]Got encoded output buffer(%d)"
            " offset=%" PRIi32 " size=%" PRIi32 " ts=%" PRIi64" flags=%" PRIu32 " ..\n",
            index, info.offset, info.size,
            info.presentationTimeUs, info.flags);

    /*
    if (info.flags & ff_AMediaCodec_getBufferFlagCodecConfig(codec)) {
        av_log(avctx, AV_LOG_ERROR, "[e]codec config.\n");
        status = ff_AMediaCodec_releaseOutputBuffer(codec, index, 0);
        if (status < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
            return AVERROR_EXTERNAL;
        }

        return offset;
    }
    */

    if (index >= 0) {
        int ret;

        data = ff_AMediaCodec_getOutputBuffer(codec, index, &size);
        if (!data) {
            av_log(avctx, AV_LOG_ERROR, "[e]Failed to get output buffer\n");
            return AVERROR_EXTERNAL;
        }


        // copy data(DirectByteBuffer) -> AVPacket

        if ((ret = ff_alloc_packet2(avctx, pkt, info.size, info.size))) {
            av_log(avctx, AV_LOG_ERROR, "[e]Error to get output packet size(%d).\n", info.size);
            return ret;
        }

        if (info.size > 0) {
            if (info.flags & ff_AMediaCodec_getBufferFlagCodecConfig(codec)) {
                av_log(avctx, AV_LOG_ERROR, "[e]codec config.\n");

                s->extradata = av_mallocz(info.size + AV_INPUT_BUFFER_PADDING_SIZE);
                if (!s->extradata) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to alloc extradata.\n");
                    return AVERROR_EXTERNAL;
                } else {
                    av_log(avctx, AV_LOG_DEBUG, "Success to alloc extradata.\n");
                }
                s->extradata_size = info.size;
                memcpy(s->extradata, data, s->extradata_size);

                av_log(avctx, AV_LOG_DEBUG, "Success to memcpy extradata.\n");

                status = ff_AMediaCodec_releaseOutputBuffer(codec, index, 0);
                if (status < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
                    return AVERROR_EXTERNAL;
                }

                if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
                    avctx->extradata = av_mallocz(s->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                    if (!avctx->extradata) {
                        return AVERROR(ENOMEM);
                    }
                    avctx->extradata_size = s->extradata_size;
                    memcpy(avctx->extradata, data, s->extradata_size);

                    av_log(avctx, AV_LOG_DEBUG, "Success to memcpy global header.\n");
                }
                return offset;
            }

            int size = info.size;
            if (!s->first_buffer++ && !(avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)) {
                av_log(avctx, AV_LOG_DEBUG, "[e]Got first buffer.\n");
                memcpy(pkt->data, s->extradata, s->extradata_size);
                memcpy(pkt->data + s->extradata_size, data, info.size);

                size += s->extradata_size;

                // desc
                av_log(avctx, AV_LOG_DEBUG, "[e][log][G] pts=%"PRId64" size=%d data  ",
                        frame->pts, s->extradata_size);

                int j;
                for (j = 0; j < s->extradata_size; j++) {
                    av_log(NULL, AV_LOG_DEBUG, "%d  ", s->extradata[j]);
                }

                av_log(avctx, AV_LOG_DEBUG, "[end]  \n");
                // desc

            } else {
                memcpy(pkt->data, data, info.size);
            }

            int64_t pkt_pts = round((double)info.presentationTimeUs / av_q2d(avctx->time_base) / 1000.0 / 1000.0);

            if (info.flags & ff_AMediaCodec_getBufferFlagKeyFrame(codec)) {
                av_log(avctx, AV_LOG_ERROR, "[e] Key frame.\n");
                pkt->flags |= AV_PKT_FLAG_KEY;
            }

            pkt->size = size;
            pkt->pts = pkt_pts;
            pkt->dts = pkt_pts;
            *got_packet = 1;

            s->queued_buffer_nb--;
            s->dequeued_buffer_nb++;

        } else {
            pkt->size = 0;
            pkt->pts = AV_NOPTS_VALUE;
        }

        status = ff_AMediaCodec_releaseOutputBuffer(codec, index, 0);
        if (status < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to release output buffer\n");
            return AVERROR_EXTERNAL;
        }

    } else if (ff_AMediaCodec_infoOutputFormatChanged(codec, index)) {
        char *format = NULL;

        if (s->format) {
            status = ff_AMediaFormat_delete(s->format);
            if (status < 0) {
                av_log(avctx, AV_LOG_ERROR, "[e]Failed to delete MediaFormat %p\n", s->format);
            }
        }

        s->format = ff_AMediaCodec_getOutputFormat(codec);
        if (!s->format) {
            av_log(avctx, AV_LOG_ERROR, "[e]Failed to get output format\n");
            return AVERROR_EXTERNAL;
        }

        format = ff_AMediaFormat_toString(s->format);
        if (!format) {
            return AVERROR_EXTERNAL;
        }
        av_log(avctx, AV_LOG_INFO, "[e]Output MediaFormat changed to %s\n", format);
        av_freep(&format);

        if ((ret = mediacodec_enc_parse_format(avctx, s)) < 0) {
            return ret;
        }

    } else if (ff_AMediaCodec_infoOutputBuffersChanged(codec, index)) {
        av_log(avctx, AV_LOG_DEBUG, "[e]Changed Output buffer(%d) ..\n", index);
        ff_AMediaCodec_cleanOutputBuffers(codec);
    } else if (ff_AMediaCodec_infoTryAgainLater(codec, index)) {
        if (s->flushing) {
            av_log(avctx, AV_LOG_ERROR,
                    "[e]Failed to dequeue output buffer within %" PRIi64 "ms "
                    "while flushing remaining frames, output will probably lack last %d frames\n",
                    output_dequeue_timeout_us / 1000, s->queued_buffer_nb);
        } else {
            av_log(avctx, AV_LOG_DEBUG, "[e]No output buffer available, try again later\n");
        }
    } else {
        av_log(avctx, AV_LOG_ERROR, "[e]Failed to dequeue output buffer (status=%zd)\n", index);
        return AVERROR_EXTERNAL;
    }


    return offset;
}

int ff_mediacodec_enc_close(AVCodecContext *avctx, MediaCodecEncContext *s)
{
    if (s->codec) {
        ff_AMediaCodec_delete(s->codec);
        s->codec = NULL;
    }

    return 0;
}
