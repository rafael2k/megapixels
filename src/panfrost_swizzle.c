/*
 * Copyright (c) 2011-2013 Luc Verhaegen <libv@skynet.be>
 * Copyright (c) 2018 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Copyright (c) 2018 Vasily Khoruzhick <anarsoul@gmail.com>
 * Copyright (c) 2019 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "panfrost_swizzle.h"
#include <stdbool.h>

/* This file implements software encode/decode of the tiling format used for
 * textures and framebuffers primarily on Utgard GPUs. Names for this format
 * include "Utgard-style tiling", "(Mali) swizzled textures", and
 * "U-interleaved" (the former two names being used in the community
 * Lima/Panfrost drivers; the latter name used internally at Arm).
 * Conceptually, like any tiling scheme, the pixel reordering attempts to 2D
 * spatial locality, to improve cache locality in both horizontal and vertical
 * directions.
 *
 * This format is tiled: first, the image dimensions must be aligned to 16
 * pixels in each axis. Once aligned, the image is divided into 16x16 tiles.
 * This size harmonizes with other properties of the GPU; on Midgard,
 * framebuffer tiles are logically 16x16 (this is the tile size used in
 * Transaction Elimination and the minimum tile size used in Hierarchical
 * Tiling). Conversely, for a standard 4 bytes-per-pixel format (like
 * RGBA8888), 16 pixels * 4 bytes/pixel = 64 bytes, equal to the cache line
 * size.
 *
 * Within each 16x16 block, the bits are reordered according to this pattern:
 *
 * | y3 | (x3 ^ y3) | y2 | (y2 ^ x2) | y1 | (y1 ^ x1) | y0 | (y0 ^ x0) |
 *
 * Basically, interleaving the X and Y bits, with XORs thrown in for every
 * adjacent bit pair.
 *
 * This is cheap to implement both encode/decode in both hardware and software.
 * In hardware, lines are simply rerouted to reorder and some XOR gates are
 * thrown in. Software has to be a bit more clever.
 *
 * In software, the trick is to divide the pattern into two lines:
 *
 *    | y3 | y3 | y2 | y2 | y1 | y1 | y0 | y0 |
 *  ^ |  0 | x3 |  0 | x2 |  0 | x1 |  0 | x0 |
 *
 * That is, duplicate the bits of the Y and space out the bits of the X. The
 * top line is a function only of Y, so it can be calculated once per row and
 * stored in a register. The bottom line is simply X with the bits spaced out.
 * Spacing out the X is easy enough with a LUT, or by subtracting+ANDing the
 * mask pattern (abusing carry bits).
 *
 * This format is also supported on Midgard GPUs, where it *can* be used for
 * textures and framebuffers. That said, in practice it is usually as a
 * fallback layout; Midgard introduces Arm FrameBuffer Compression, which is
 * significantly more efficient than Utgard-style tiling and preferred for both
 * textures and framebuffers, where possible. For unsupported texture types,
 * for instance sRGB textures and framebuffers, this tiling scheme is used at a
 * performance penalty, as AFBC is not compatible.
 */

/* Given the lower 4-bits of the Y coordinate, we would like to
 * duplicate every bit over. So instead of 0b1010, we would like
 * 0b11001100. The idea is that for the bits in the solely Y place, we
 * get a Y place, and the bits in the XOR place *also* get a Y. */

const uint32_t bit_duplication[16] = {
   0b00000000,
   0b00000011,
   0b00001100,
   0b00001111,
   0b00110000,
   0b00110011,
   0b00111100,
   0b00111111,
   0b11000000,
   0b11000011,
   0b11001100,
   0b11001111,
   0b11110000,
   0b11110011,
   0b11111100,
   0b11111111,
};

/* Space the bits out of a 4-bit nibble */

const unsigned space_4[16] = {
   0b0000000,
   0b0000001,
   0b0000100,
   0b0000101,
   0b0010000,
   0b0010001,
   0b0010100,
   0b0010101,
   0b1000000,
   0b1000001,
   0b1000100,
   0b1000101,
   0b1010000,
   0b1010001,
   0b1010100,
   0b1010101
};

/* The scheme uses 16x16 tiles */

#define TILE_WIDTH 16
#define TILE_HEIGHT 16
#define PIXELS_PER_TILE (TILE_WIDTH * TILE_HEIGHT)

/* Optimized routine to tile an aligned (w & 0xF == 0) texture. Explanation:
 *
 * dest_start precomputes the offset to the beginning of the first horizontal
 * tile we're writing to, knowing that x is 16-aligned. Tiles themselves are
 * stored linearly, so we get the X tile number by shifting and then multiply
 * by the bytes per tile .
 *
 * We iterate across the pixels we're trying to store in source-order. For each
 * row in the destination image, we figure out which row of 16x16 block we're
 * in, by slicing off the lower 4-bits (block_y).
 *
 * dest then precomputes the location of the top-left corner of the block the
 * row starts in. In pixel coordinates (where the origin is the top-left),
 * (block_y, 0) is the top-left corner of the leftmost tile in this row.  While
 * pixels are reordered within a block, the blocks themselves are stored
 * linearly, so multiplying block_y by the pixel stride of the destination
 * image equals the byte offset of that top-left corner of the block this row
 * is in.
 *
 * On the other hand, the source is linear so we compute the locations of the
 * start and end of the row in the source by a simple linear addressing.
 *
 * For indexing within the tile, we need to XOR with the [y3 y3 y2 y2 y1 y1 y0
 * y0] value. Since this is constant across a row, we look it up per-row and
 * store in expanded_y.
 *
 * Finally, we iterate each row in source order. In the outer loop, we iterate
 * each 16 pixel tile. Within each tile, we iterate the 16 pixels (this should
 * be unrolled), calculating the index within the tile and writing.
 */

static inline void
panfrost_access_tiled_image_uint32_t
                              (void *dst, void *src,
                               uint32_t w, uint32_t h,
                               uint32_t dst_stride,
                               uint32_t src_stride,
                               bool is_store)
{
   for (int y = 0, src_y = 0; src_y < h; ++y, ++src_y) {
      uint16_t block_y = y & ~0x0f;
      uint8_t *dest = ((uint8_t *) dst + (block_y * dst_stride));
      uint32_t *source = src + (src_y * src_stride);
      uint32_t *source_end = source + w;
      unsigned expanded_y = bit_duplication[y & 0xF] << 2;
      for (; source < source_end; dest += (PIXELS_PER_TILE << 2)) {
         for (uint8_t i = 0; i < 16; ++i) {
            unsigned index = expanded_y ^ (space_4[i] << 2);
            if (is_store)
                *((uint32_t *) (dest + index)) = *(source++);
            else
                *(source++) = *((uint32_t *) (dest + index));
         }
      }
   }
}

void
panfrost_store_tiled_image(uint32_t *dst, const uint32_t *src,
                           uint32_t w, uint32_t h,
                           uint32_t dst_stride,
                           uint32_t src_stride)
{
    panfrost_access_tiled_image_uint32_t(dst, (uint32_t *) src, w, h, dst_stride, src_stride, true);
}

void
panfrost_load_tiled_image(uint32_t *dst, const uint32_t *src,
                           uint32_t w, uint32_t h,
                           uint32_t dst_stride,
                           uint32_t src_stride)
{
   panfrost_access_tiled_image_uint32_t((uint32_t *) src, dst, w, h, src_stride, dst_stride, false);
}
