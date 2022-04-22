#pragma once

#include <stdint.h>

void panfrost_store_tiled_image(uint32_t *dst, const uint32_t *src,
				uint32_t w, uint32_t h,
				uint32_t dst_stride,
				uint32_t src_stride);

void panfrost_load_tiled_image(uint32_t *dst, const uint32_t *src,
			       uint32_t w, uint32_t h,
			       uint32_t dst_stride,
			       uint32_t src_stride);
