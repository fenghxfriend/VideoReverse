/*
 * DPX (.dpx) image decoder
 * Copyright (c) 2009 Jimmy Christensen
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

#include "libavutil/intreadwrite.h"
#include "libavutil/imgutils.h"
#include "bytestream.h"
#include "avcodec.h"

typedef struct DPXContext {
    AVFrame picture;
} DPXContext;


static unsigned int read32(const uint8_t **ptr, int is_big)
{
    unsigned int temp;
    if (is_big) {
        temp = AV_RB32(*ptr);
    } else {
        temp = AV_RL32(*ptr);
    }
    *ptr += 4;
    return temp;
}

static int decode_frame(AVCodecContext *avctx,
                        void *data,
                        int *data_size,
                        AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    const uint8_t *buf_end = avpkt->data + avpkt->size;
    int buf_size       = avpkt->size;
    DPXContext *const s = avctx->priv_data;
    AVFrame *picture  = data;
    AVFrame *const p = &s->picture;
    uint8_t *ptr[AV_NUM_DATA_POINTERS];

    unsigned int offset;
    int magic_num, endian;
    int x, y, i;
    int w, h, stride, bits_per_color, descriptor, elements, target_packet_size, source_packet_size;
    int planar;

    unsigned int rgbBuffer;

    if (avpkt->size <= 1634) {
        av_log(avctx, AV_LOG_ERROR, "Packet too small for DPX header\n");
        return AVERROR_INVALIDDATA;
    }

    magic_num = AV_RB32(buf);
    buf += 4;

    /* Check if the files "magic number" is "SDPX" which means it uses
     * big-endian or XPDS which is for little-endian files */
    if (magic_num == AV_RL32("SDPX")) {
        endian = 0;
    } else if (magic_num == AV_RB32("SDPX")) {
        endian = 1;
    } else {
        av_log(avctx, AV_LOG_ERROR, "DPX marker not found\n");
        return -1;
    }

    offset = read32(&buf, endian);
    if (avpkt->size <= offset) {
        av_log(avctx, AV_LOG_ERROR, "Invalid data start offset\n");
        return AVERROR_INVALIDDATA;
    }
    // Need to end in 0x304 offset from start of file
    buf = avpkt->data + 0x304;
    w = read32(&buf, endian);
    h = read32(&buf, endian);

    // Need to end in 0x320 to read the descriptor
    buf += 20;
    descriptor = buf[0];

    // Need to end in 0x323 to read the bits per color
    buf += 3;
    avctx->bits_per_raw_sample =
    bits_per_color = buf[0];

    buf += 825;
    avctx->sample_aspect_ratio.num = read32(&buf, endian);
    avctx->sample_aspect_ratio.den = read32(&buf, endian);
    if (avctx->sample_aspect_ratio.num > 0 && avctx->sample_aspect_ratio.den > 0)
        av_reduce(&avctx->sample_aspect_ratio.num, &avctx->sample_aspect_ratio.den,
                   avctx->sample_aspect_ratio.num,  avctx->sample_aspect_ratio.den,
                  0x10000);
    else
        avctx->sample_aspect_ratio = (AVRational){ 0, 1 };

    switch (descriptor) {
        case 51: // RGBA
            elements = 4;
            break;
        case 50: // RGB
            elements = 3;
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "Unsupported descriptor %d\n", descriptor);
            return -1;
    }

    switch (bits_per_color) {
        case 8:
            if (elements == 4) {
                avctx->pix_fmt = PIX_FMT_RGBA;
            } else {
                avctx->pix_fmt = PIX_FMT_RGB24;
            }
            source_packet_size = elements;
            target_packet_size = elements;
            planar = 0;
            break;
        case 10:
            avctx->pix_fmt = PIX_FMT_GBRP10;
            target_packet_size = 6;
            source_packet_size = 4;
            planar = 1;
            break;
        case 12:
            if (endian) {
                avctx->pix_fmt = elements == 4 ? PIX_FMT_GBRP12BE : PIX_FMT_GBRP12BE;
            } else {
                avctx->pix_fmt = elements == 4 ? PIX_FMT_GBRP12LE : PIX_FMT_GBRP12LE;
            }
            target_packet_size = 6;
            source_packet_size = 6;
            planar = 1;
            break;
        case 16:
            if (endian) {
                avctx->pix_fmt = elements == 4 ? PIX_FMT_RGBA64BE : PIX_FMT_RGB48BE;
            } else {
                avctx->pix_fmt = elements == 4 ? PIX_FMT_RGBA64LE : PIX_FMT_RGB48LE;
            }
            target_packet_size =
            source_packet_size = elements * 2;
            planar = 0;
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "Unsupported color depth : %d\n", bits_per_color);
            return -1;
    }

    if (s->picture.data[0])
        avctx->release_buffer(avctx, &s->picture);
    if (av_image_check_size(w, h, 0, avctx))
        return -1;
    if (w != avctx->width || h != avctx->height)
        avcodec_set_dimensions(avctx, w, h);
    if (avctx->get_buffer(avctx, p) < 0) {
        av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
        return -1;
    }

    // Move pointer to offset from start of file
    buf =  avpkt->data + offset;

    for (i=0; i<AV_NUM_DATA_POINTERS; i++)
        ptr[i] = p->data[i];
    stride = p->linesize[0];

    if (source_packet_size*avctx->width*avctx->height > buf_end - buf) {
        av_log(avctx, AV_LOG_ERROR, "Overread buffer. Invalid header?\n");
        return -1;
    }
    switch (bits_per_color) {
        case 10:
            for (x = 0; x < avctx->height; x++) {
                uint16_t *dst[3] = {(uint16_t*)ptr[0],
                                    (uint16_t*)ptr[1],
                                    (uint16_t*)ptr[2]};
               for (y = 0; y < avctx->width; y++) {
                   rgbBuffer = read32(&buf, endian);
                   *dst[0]++ = (rgbBuffer >> 12) & 0x3FF;
                   *dst[1]++ = (rgbBuffer >> 2)  & 0x3FF;
                   *dst[2]++ = (rgbBuffer >> 22) & 0x3FF;
               }
               for (i=0; i<3; i++)
                   ptr[i] += p->linesize[i];
            }
            break;
        case 8:
        case 12:
        case 16:
            if (planar) {
                int source_bpc = target_packet_size / elements;
                int target_bpc = target_packet_size / elements;
                for (x = 0; x < avctx->height; x++) {
                    uint8_t *dst[AV_NUM_DATA_POINTERS];
                    for (i=0; i<elements; i++)
                        dst[i] = ptr[i];
                    for (y = 0; y < avctx->width; y++) {
                        for (i=0; i<3; i++) {
                            memcpy(dst[i], buf, FFMIN(source_bpc, target_bpc));
                            dst[i] += target_bpc;
                            buf += source_bpc;
                        }
                    }
                    for (i=0; i<elements; i++)
                        ptr[i] += p->linesize[i];
                }
            } else {
                if (source_packet_size == target_packet_size) {
                    for (x = 0; x < avctx->height; x++) {
                        memcpy(ptr[0], buf, target_packet_size*avctx->width);
                        ptr[0] += stride;
                        buf += source_packet_size*avctx->width;
                    }
                } else {
                    for (x = 0; x < avctx->height; x++) {
                        uint8_t *dst = ptr[0];
                        for (y = 0; y < avctx->width; y++) {
                            memcpy(dst, buf, target_packet_size);
                            dst += target_packet_size;
                            buf += source_packet_size;
                        }
                        ptr[0] += stride;
                    }
                }
            }
            break;
    }

    *picture   = s->picture;
    *data_size = sizeof(AVPicture);

    return buf_size;
}

static av_cold int decode_init(AVCodecContext *avctx)
{
    DPXContext *s = avctx->priv_data;
    avcodec_get_frame_defaults(&s->picture);
    avctx->coded_frame = &s->picture;
    return 0;
}

static av_cold int decode_end(AVCodecContext *avctx)
{
    DPXContext *s = avctx->priv_data;
    if (s->picture.data[0])
        avctx->release_buffer(avctx, &s->picture);

    return 0;
}

AVCodec ff_dpx_decoder = {
    .name           = "dpx",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_DPX,
    .priv_data_size = sizeof(DPXContext),
    .init           = decode_init,
    .close          = decode_end,
    .decode         = decode_frame,
    .capabilities   = CODEC_CAP_DR1,
    .long_name      = NULL_IF_CONFIG_SMALL("DPX image"),
};
