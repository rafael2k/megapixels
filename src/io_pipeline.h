#pragma once

#include "camera_config.h"

struct mp_io_pipeline_state {
        const struct mp_camera_config *camera;

        MPAppMode app_mode;

        int burst_length;

        int preview_width;
        int preview_height;

        int device_rotation;

        bool gain_is_manual;
        int gain;

        bool exposure_is_manual;
        int exposure;

        bool save_dng;
        bool flash_enabled;
};

void mp_io_pipeline_start();
void mp_io_pipeline_stop();

void mp_io_pipeline_focus();
void mp_io_pipeline_capture();

void mp_io_pipeline_release_buffer(uint32_t buffer_index);

void mp_io_pipeline_update_state(const struct mp_io_pipeline_state *state);
