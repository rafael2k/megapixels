#pragma once

#include "camera_config.h"
#include "process_pipeline.h"

void mp_av_pipeline_start();
void mp_av_pipeline_stop();

void mp_av_pipeline_record(const char *file, MPPixelFormat pixel_format, uint32_t width, uint32_t height, uint32_t stride, struct v4l2_fract interval);
void mp_av_pipeline_add_frame(uint8_t *image);
void mp_av_pipeline_finish();
