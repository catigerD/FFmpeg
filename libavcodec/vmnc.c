/*
 * VMware Screen Codec (VMnc) decoder
 * Copyright (c) 2006 Konstantin Shishkov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/**
 * @file vmnc.c
 * VMware Screen Codec (VMnc) decoder
 * As Alex Beregszaszi discovered, this is effectively RFB data dump
 */

#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "avcodec.h"

enum EncTypes {
    MAGIC_WMVd = 0x574D5664,
    MAGIC_WMVe,
    MAGIC_WMVf,
    MAGIC_WMVg,
    MAGIC_WMVh,
    MAGIC_WMVi,
    MAGIC_WMVj
};

enum HexTile_Flags {
    HT_RAW =  1, // tile is raw
    HT_BKG =  2, // background color is present
    HT_FG  =  4, // foreground color is present
    HT_SUB =  8, // subrects are present
    HT_CLR = 16  // each subrect has own color
};

/*
 * Decoder context
 */
typedef struct VmncContext {
    AVCodecContext *avctx;
    AVFrame pic;

    int bpp;
    int bpp2;
    int bigendian;
    uint8_t pal[768];
    int width, height;

    /* cursor data */
    int cur_w, cur_h;
    int cur_x, cur_y;
    int cur_hx, cur_hy;
    uint8_t* curbits, *curmask;
    uint8_t* screendta;
} VmncContext;

/* read pixel value from stream */
static always_inline int vmnc_get_pixel(uint8_t* buf, int bpp, int be) {
    switch(bpp * 2 + be) {
    case 2:
    case 3: return *buf;
    case 4: return LE_16(buf);
    case 5: return BE_16(buf);
    case 8: return LE_32(buf);
    case 9: return BE_32(buf);
    default: return 0;
    }
}

static void load_cursor(VmncContext *c, uint8_t *src)
{
    int i, j, p;
    const int bpp = c->bpp2;
    uint8_t  *dst8  = c->curbits;
    uint16_t *dst16 = (uint16_t*)c->curbits;
    uint32_t *dst32 = (uint32_t*)c->curbits;

    for(j = 0; j < c->cur_h; j++) {
        for(i = 0; i < c->cur_w; i++) {
            p = vmnc_get_pixel(src, bpp, c->bigendian);
            src += bpp;
            if(bpp == 1) *dst8++ = p;
            if(bpp == 2) *dst16++ = p;
            if(bpp == 4) *dst32++ = p;
        }
    }
    dst8 = c->curmask;
    dst16 = (uint16_t*)c->curmask;
    dst32 = (uint32_t*)c->curmask;
    for(j = 0; j < c->cur_h; j++) {
        for(i = 0; i < c->cur_w; i++) {
            p = vmnc_get_pixel(src, bpp, c->bigendian);
            src += bpp;
            if(bpp == 1) *dst8++ = p;
            if(bpp == 2) *dst16++ = p;
            if(bpp == 4) *dst32++ = p;
        }
    }
}

static void put_cursor(uint8_t *dst, int stride, VmncContext *c, int dx, int dy)
{
    int i, j, t;
    int w, h, x, y;
    w = c->cur_w;
    if(c->width < c->cur_x + c->cur_w) w = c->width - c->cur_x;
    h = c->cur_h;
    if(c->height < c->cur_y + c->cur_h) h = c->height - c->cur_y;
    x = c->cur_x;
    y = c->cur_y;
    if(x < 0) {
        w += x;
        x = 0;
    }
    if(y < 0) {
        h += y;
        y = 0;
    }

    if((w < 1) || (h < 1)) return;
    dst += x * c->bpp2 + y * stride;

    if(c->bpp2 == 1) {
        uint8_t* cd = c->curbits, *msk = c->curmask;
        for(j = 0; j < h; j++) {
            for(i = 0; i < w; i++)
                dst[i] = (dst[i] & cd[i]) ^ msk[i];
            msk += c->cur_w;
            cd += c->cur_w;
            dst += stride;
        }
    } else if(c->bpp2 == 2) {
        uint16_t* cd = (uint16_t*)c->curbits, *msk = (uint16_t*)c->curmask;
        uint16_t* dst2;
        for(j = 0; j < h; j++) {
            dst2 = (uint16_t*)dst;
            for(i = 0; i < w; i++)
                dst2[i] = (dst2[i] & cd[i]) ^ msk[i];
            msk += c->cur_w;
            cd += c->cur_w;
            dst += stride;
        }
    } else if(c->bpp2 == 4) {
        uint32_t* cd = (uint32_t*)c->curbits, *msk = (uint32_t*)c->curmask;
        uint32_t* dst2;
        for(j = 0; j < h; j++) {
            dst2 = (uint32_t*)dst;
            for(i = 0; i < w; i++)
                dst2[i] = (dst2[i] & cd[i]) ^ msk[i];
            msk += c->cur_w;
            cd += c->cur_w;
            dst += stride;
        }
    }
}

/* fill rectangle with given colour */
static always_inline void paint_rect(uint8_t *dst, int dx, int dy, int w, int h, int color, int bpp, int stride)
{
    int i, j;
    dst += dx * bpp + dy * stride;
    if(bpp == 1){
        for(j = 0; j < h; j++) {
            memset(dst, color, w);
            dst += stride;
        }
    }else if(bpp == 2){
        uint16_t* dst2;
        for(j = 0; j < h; j++) {
            dst2 = (uint16_t*)dst;
            for(i = 0; i < w; i++) {
                *dst2++ = color;
            }
            dst += stride;
        }
    }else if(bpp == 4){
        uint32_t* dst2;
        for(j = 0; j < h; j++) {
            dst2 = (uint32_t*)dst;
            for(i = 0; i < w; i++) {
                dst2[i] = color;
            }
            dst += stride;
        }
    }
}

static always_inline void paint_raw(uint8_t *dst, int w, int h, uint8_t* src, int bpp, int be, int stride)
{
    int i, j, p;
    for(j = 0; j < h; j++) {
        for(i = 0; i < w; i++) {
            p = vmnc_get_pixel(src, bpp, be);
            src += bpp;
            switch(bpp){
            case 1:
                dst[i] = p;
                break;
            case 2:
                ((uint16_t*)dst)[i] = p;
                break;
            case 4:
                ((uint32_t*)dst)[i] = p;
                break;
            }
        }
        dst += stride;
    }
}

static int decode_hextile(VmncContext *c, uint8_t* dst, uint8_t* src, int w, int h, int stride)
{
    int i, j, k;
    int bg = 0, fg = 0, rects, color, flags, xy, wh;
    const int bpp = c->bpp2;
    uint8_t *dst2;
    int bw = 16, bh = 16;
    uint8_t *ssrc=src;

    for(j = 0; j < h; j += 16) {
        dst2 = dst;
        bw = 16;
        if(j + 16 > h) bh = h - j;
        for(i = 0; i < w; i += 16, dst2 += 16 * bpp) {
            if(i + 16 > w) bw = w - i;
            flags = *src++;
            if(flags & HT_RAW) {
                paint_raw(dst2, bw, bh, src, bpp, c->bigendian, stride);
                src += bw * bh * bpp;
            } else {
                if(flags & HT_BKG) {
                    bg = vmnc_get_pixel(src, bpp, c->bigendian); src += bpp;
                }
                if(flags & HT_FG) {
                    fg = vmnc_get_pixel(src, bpp, c->bigendian); src += bpp;
                }
                rects = 0;
                if(flags & HT_SUB)
                    rects = *src++;
                color = (flags & HT_CLR);

                paint_rect(dst2, 0, 0, bw, bh, bg, bpp, stride);

                for(k = 0; k < rects; k++) {
                    if(color) {
                        fg = vmnc_get_pixel(src, bpp, c->bigendian); src += bpp;
                    }
                    xy = *src++;
                    wh = *src++;
                    paint_rect(dst2, xy >> 4, xy & 0xF, (wh>>4)+1, (wh & 0xF)+1, fg, bpp, stride);
                }
            }
        }
        dst += stride * 16;
    }
    return src - ssrc;
}

static int decode_frame(AVCodecContext *avctx, void *data, int *data_size, uint8_t *buf, int buf_size)
{
    VmncContext * const c = (VmncContext *)avctx->priv_data;
    uint8_t *outptr;
    uint8_t *src = buf;
    int dx, dy, w, h, depth, enc, chunks, res;

    c->pic.reference = 1;
    c->pic.buffer_hints = FF_BUFFER_HINTS_VALID | FF_BUFFER_HINTS_PRESERVE | FF_BUFFER_HINTS_REUSABLE;
    if(avctx->reget_buffer(avctx, &c->pic) < 0){
        av_log(avctx, AV_LOG_ERROR, "reget_buffer() failed\n");
        return -1;
    }

    c->pic.key_frame = 0;
    c->pic.pict_type = FF_P_TYPE;

    //restore screen after cursor
    if(c->screendta) {
        int i;
        w = c->cur_w;
        if(c->width < c->cur_x + w) w = c->width - c->cur_x;
        h = c->cur_h;
        if(c->height < c->cur_y + h) h = c->height - c->cur_y;
        dx = c->cur_x;
        if(dx < 0) {
            w += dx;
            dx = 0;
        }
        dy = c->cur_y;
        if(dy < 0) {
            h += dy;
            dy = 0;
        }
        if((w > 0) && (h > 0)) {
            outptr = c->pic.data[0] + dx * c->bpp2 + dy * c->pic.linesize[0];
            for(i = 0; i < h; i++) {
                memcpy(outptr, c->screendta + i * c->cur_w * c->bpp2, w * c->bpp2);
                outptr += c->pic.linesize[0];
            }
        }
    }
    src += 2;
    chunks = BE_16(src); src += 2;
    while(chunks--) {
        dx = BE_16(src); src += 2;
        dy = BE_16(src); src += 2;
        w  = BE_16(src); src += 2;
        h  = BE_16(src); src += 2;
        enc = BE_32(src); src += 4;
        outptr = c->pic.data[0] + dx * c->bpp2 + dy * c->pic.linesize[0];
        switch(enc) {
        case MAGIC_WMVd: // cursor
            src += 2;
            c->cur_w = w;
            c->cur_h = h;
            c->cur_hx = dx;
            c->cur_hy = dy;
            if((c->cur_hx > c->cur_w) || (c->cur_hy > c->cur_h)) {
                av_log(avctx, AV_LOG_ERROR, "Cursor hot spot is not in image: %ix%i of %ix%i cursor size\n", c->cur_hx, c->cur_hy, c->cur_w, c->cur_h);
                c->cur_hx = c->cur_hy = 0;
            }
            c->curbits = av_realloc(c->curbits, c->cur_w * c->cur_h * c->bpp2);
            c->curmask = av_realloc(c->curmask, c->cur_w * c->cur_h * c->bpp2);
            c->screendta = av_realloc(c->screendta, c->cur_w * c->cur_h * c->bpp2);
            load_cursor(c, src);
            src += w * h * c->bpp2 * 2;
            break;
        case MAGIC_WMVe: // unknown
            src += 2;
            break;
        case MAGIC_WMVf: // update cursor position
            c->cur_x = dx - c->cur_hx;
            c->cur_y = dy - c->cur_hy;
            break;
        case MAGIC_WMVi: // ServerInitialization struct
            c->pic.key_frame = 1;
            c->pic.pict_type = FF_I_TYPE;
            depth = *src++;
            if(depth != c->bpp) {
                av_log(avctx, AV_LOG_INFO, "Depth mismatch. Container %i bpp, Frame data: %i bpp\n", c->bpp, depth);
            }
            src++;
            c->bigendian = *src++;
            if(c->bigendian & (~1)) {
                av_log(avctx, AV_LOG_INFO, "Invalid header: bigendian flag = %i\n", c->bigendian);
                return -1;
            }
            //skip the rest of pixel format data
            src += 13;
            break;
        case 0x00000000: // raw rectangle data
            if((dx + w > c->width) || (dy + h > c->height)) {
                av_log(avctx, AV_LOG_ERROR, "Incorrect frame size: %ix%i+%ix%i of %ix%i\n", w, h, dx, dy, c->width, c->height);
                return -1;
            }
            paint_raw(outptr, w, h, src, c->bpp2, c->bigendian, c->pic.linesize[0]);
            src += w * h * c->bpp2;
            break;
        case 0x00000005: // HexTile encoded rectangle
            if((dx + w > c->width) || (dy + h > c->height)) {
                av_log(avctx, AV_LOG_ERROR, "Incorrect frame size: %ix%i+%ix%i of %ix%i\n", w, h, dx, dy, c->width, c->height);
                return -1;
            }
            res = decode_hextile(c, outptr, src, w, h, c->pic.linesize[0]);
            if(res < 0)
                return -1;
            src += res;
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "Unsupported block type 0x%08X\n", enc);
            chunks = 0; // leave chunks decoding loop
        }
    }
    if(c->screendta){
        int i;
        //save screen data before painting cursor
        w = c->cur_w;
        if(c->width < c->cur_x + w) w = c->width - c->cur_x;
        h = c->cur_h;
        if(c->height < c->cur_y + h) h = c->height - c->cur_y;
        dx = c->cur_x;
        if(dx < 0) {
            w += dx;
            dx = 0;
        }
        dy = c->cur_y;
        if(dy < 0) {
            h += dy;
            dy = 0;
        }
        if((w > 0) && (h > 0)) {
            outptr = c->pic.data[0] + dx * c->bpp2 + dy * c->pic.linesize[0];
            for(i = 0; i < h; i++) {
                memcpy(c->screendta + i * c->cur_w * c->bpp2, outptr, w * c->bpp2);
                outptr += c->pic.linesize[0];
            }
            outptr = c->pic.data[0];
            put_cursor(outptr, c->pic.linesize[0], c, c->cur_x, c->cur_y);
        }
    }
    *data_size = sizeof(AVFrame);
    *(AVFrame*)data = c->pic;

    /* always report that the buffer was completely consumed */
    return buf_size;
}



/*
 *
 * Init VMnc decoder
 *
 */
static int decode_init(AVCodecContext *avctx)
{
    VmncContext * const c = (VmncContext *)avctx->priv_data;

    c->avctx = avctx;
    avctx->has_b_frames = 0;

    c->pic.data[0] = NULL;
    c->width = avctx->width;
    c->height = avctx->height;

    if (avcodec_check_dimensions(avctx, avctx->height, avctx->width) < 0) {
        return 1;
    }
    c->bpp = avctx->bits_per_sample;
    c->bpp2 = c->bpp/8;

    switch(c->bpp){
    case 8:
        avctx->pix_fmt = PIX_FMT_PAL8;
        break;
    case 16:
        avctx->pix_fmt = PIX_FMT_RGB555;
        break;
    case 32:
        avctx->pix_fmt = PIX_FMT_RGB32;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported bitdepth %i\n", c->bpp);
    }

    return 0;
}



/*
 *
 * Uninit VMnc decoder
 *
 */
static int decode_end(AVCodecContext *avctx)
{
    VmncContext * const c = (VmncContext *)avctx->priv_data;

    if (c->pic.data[0])
        avctx->release_buffer(avctx, &c->pic);

    av_free(c->curbits);
    av_free(c->curmask);
    av_free(c->screendta);
    return 0;
}

AVCodec vmnc_decoder = {
    "VMware video",
    CODEC_TYPE_VIDEO,
    CODEC_ID_VMNC,
    sizeof(VmncContext),
    decode_init,
    NULL,
    decode_end,
    decode_frame
};

