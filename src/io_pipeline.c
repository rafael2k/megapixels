#include "io_pipeline.h"

#include "camera.h"
#include "device.h"
#include "flash.h"
#include "pipeline.h"
#include "process_pipeline.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

MPDevice *device;
static MPMode mode;

static bool just_switched_mode = false;
static int blank_frame_count = 0;

static int burst_length;
static int captures_remaining = 0;

static int preview_width;
static int preview_height;

static int device_rotation;

struct control_state {
        bool gain_is_manual;
        int gain;

        bool exposure_is_manual;
        int exposure;
};

static struct control_state desired_controls = {};
static struct control_state current_controls = {};

static bool flash_enabled = false;

static bool want_focus = false;

static MPPipeline *pipeline;
static GSource *capture_source;

static void
setup_camera(MPDevice *device)
{
        // Make sure the camera starts out as disabled
        mp_device_setup_link(device, device->sensor_pad, device->csi_pad, false);

        device->video_fd = open(device->video_path, O_RDWR);
        if (device->video_fd == -1) {
                g_printerr("Could not open %s: %s\n",
                           device->video_path,
                           strerror(errno));
                exit(EXIT_FAILURE);
        }

        // Start with the capture format, this works around a bug with
        // the ov5640 driver where it won't allow setting the preview
        // format initially.
        MPMode mode = device->capture_mode;

        MPCamera *camera = mp_camera_new(device->video_fd, device->sensor_fd);

        // TODO: Set up media entity pipeline from config if needed
        mp_camera_set_mode(camera, &mode);

        // Trigger continuous auto focus if the sensor supports it
        if (mp_camera_query_control(camera, V4L2_CID_FOCUS_AUTO, NULL)) {
                device->has_auto_focus_continuous = true;
                mp_camera_control_set_bool_bg(camera, V4L2_CID_FOCUS_AUTO, true);
        }
        if (mp_camera_query_control(camera, V4L2_CID_AUTO_FOCUS_START, NULL)) {
                device->has_auto_focus_start = true;
        }

        MPControl control;
        if (mp_camera_query_control(camera, V4L2_CID_GAIN, &control)) {
                device->gain_ctrl = V4L2_CID_GAIN;
                device->gain_max = control.max;
        } else if (mp_camera_query_control(
                           camera, V4L2_CID_ANALOGUE_GAIN, &control)) {
                device->gain_ctrl = V4L2_CID_ANALOGUE_GAIN;
                device->gain_max = control.max;
        }

        // Setup flash
        if (device->cfg_flash_path[0]) {
                device->flash = mp_led_flash_from_path(device->cfg_flash_path);
        } else if (device->flash_display) {
                device->flash = mp_create_display_flash();
        } else {
                device->flash = NULL;
        }
}

static void
setup(MPPipeline *pipeline, const void *data)
{
        MPDeviceList *devices = (MPDeviceList *)data;
        while (devices) {
                setup_camera(devices->device);
                devices = devices->next;
        }
}

static void
clean_cameras()
{
}

void
mp_io_pipeline_start(MPDeviceList **devices)
{
        mp_process_pipeline_start();

        pipeline = mp_pipeline_new();

        mp_pipeline_invoke(pipeline, setup, devices, 0);
}

void
mp_io_pipeline_stop()
{
        if (capture_source) {
                g_source_destroy(capture_source);
        }

        clean_cameras();

        mp_pipeline_free(pipeline);

        mp_process_pipeline_stop();
}

static void
update_process_pipeline()
{
        // Grab the latest control values
        if (!current_controls.gain_is_manual) {
                current_controls.gain = mp_camera_control_get_int32(
                        device->camera, device->gain_ctrl);
        }
        if (!current_controls.exposure_is_manual) {
                current_controls.exposure = mp_camera_control_get_int32(
                        device->camera, V4L2_CID_EXPOSURE);
        }

        struct mp_process_pipeline_state pipeline_state = {
                .device = device,
                .mode = mode,
                .burst_length = burst_length,
                .preview_width = preview_width,
                .preview_height = preview_height,
                .device_rotation = device_rotation,
                .gain_is_manual = current_controls.gain_is_manual,
                .gain = current_controls.gain,
                .gain_max = device->gain_max,
                .exposure_is_manual = current_controls.exposure_is_manual,
                .exposure = current_controls.exposure,
                .has_auto_focus_continuous = device->has_auto_focus_continuous,
                .has_auto_focus_start = device->has_auto_focus_start,
                .flash_enabled = flash_enabled,
        };
        mp_process_pipeline_update_state(&pipeline_state);
}

static void
focus(MPPipeline *pipeline, const void *data)
{
        want_focus = true;
}

void
mp_io_pipeline_focus()
{
        mp_pipeline_invoke(pipeline, focus, NULL, 0);
}

static void
capture(MPPipeline *pipeline, const void *data)
{
        uint32_t gain;
        float gain_norm;

        // Disable the autogain/exposure while taking the burst
        mp_camera_control_set_int32(device->camera, V4L2_CID_AUTOGAIN, 0);
        mp_camera_control_set_int32(
                device->camera, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);

        // Get current gain to calculate a burst length;
        // with low gain there's 2, with the max automatic gain of the ov5640
        // the value seems to be 248 which creates a 5 frame burst
        // for manual gain you can go up to 11 frames
        gain = mp_camera_control_get_int32(device->camera, V4L2_CID_GAIN);
        gain_norm = (float)gain / (float)device->gain_max;
        burst_length = (int)fmax(sqrt(gain_norm) * 10, 1) + 1;
        captures_remaining = burst_length;

        // Change camera mode for capturing
        mp_process_pipeline_sync();
        mp_camera_stop_capture(device->camera);

        mode = device->capture_mode;
        // TODO: set up media graph
        mp_camera_set_mode(device->camera, &mode);
        just_switched_mode = true;

        mp_camera_start_capture(device->camera);

        // Enable flash
        if (device->flash && flash_enabled) {
                mp_flash_enable(device->flash);
        }

        update_process_pipeline();

        mp_process_pipeline_capture();
}

void
mp_io_pipeline_capture()
{
        mp_pipeline_invoke(pipeline, capture, NULL, 0);
}

static void
release_buffer(MPPipeline *pipeline, const uint32_t *buffer_index)
{
        mp_camera_release_buffer(device->camera, *buffer_index);
}

void
mp_io_pipeline_release_buffer(uint32_t buffer_index)
{
        mp_pipeline_invoke(pipeline,
                           (MPPipelineCallback)release_buffer,
                           &buffer_index,
                           sizeof(uint32_t));
}

static pid_t focus_continuous_task = 0;
static pid_t start_focus_task = 0;
static void
start_focus()
{
        // only run 1 manual focus at once
        if (!mp_camera_check_task_complete(device->camera, start_focus_task) ||
            !mp_camera_check_task_complete(device->camera, focus_continuous_task))
                return;

        if (device->has_auto_focus_continuous) {
                focus_continuous_task = mp_camera_control_set_bool_bg(
                        device->camera, V4L2_CID_FOCUS_AUTO, 1);
        } else if (device->has_auto_focus_start) {
                start_focus_task = mp_camera_control_set_bool_bg(
                        device->camera, V4L2_CID_AUTO_FOCUS_START, 1);
        }
}

static void
update_controls()
{
        // Don't update controls while capturing
        if (captures_remaining > 0) {
                return;
        }

        if (want_focus) {
                start_focus();
                want_focus = false;
        }

        if (current_controls.gain_is_manual != desired_controls.gain_is_manual) {
                mp_camera_control_set_bool_bg(device->camera,
                                              V4L2_CID_AUTOGAIN,
                                              !desired_controls.gain_is_manual);
        }

        if (desired_controls.gain_is_manual &&
            current_controls.gain != desired_controls.gain) {
                mp_camera_control_set_int32_bg(
                        device->camera, device->gain_ctrl, desired_controls.gain);
        }

        if (current_controls.exposure_is_manual !=
            desired_controls.exposure_is_manual) {
                mp_camera_control_set_int32_bg(device->camera,
                                               V4L2_CID_EXPOSURE_AUTO,
                                               desired_controls.exposure_is_manual ?
                                                       V4L2_EXPOSURE_MANUAL :
                                                       V4L2_EXPOSURE_AUTO);
        }

        if (desired_controls.exposure_is_manual &&
            current_controls.exposure != desired_controls.exposure) {
                mp_camera_control_set_int32_bg(device->camera,
                                               V4L2_CID_EXPOSURE,
                                               desired_controls.exposure);
        }

        current_controls = desired_controls;
}

static void
on_frame(MPBuffer buffer, void *_data)
{
        // Only update controls right after a frame was captured
        update_controls();

        // When the mode is switched while capturing we get a couple blank frames,
        // presumably from buffers made ready during the switch. Ignore these.
        if (just_switched_mode) {
                if (blank_frame_count < 20) {
                        // Only check a 10x10 area
                        size_t test_size =
                                MIN(10, mode.width) * MIN(10, mode.height);

                        bool image_is_blank = true;
                        for (size_t i = 0; i < test_size; ++i) {
                                if (buffer.data[i] != 0) {
                                        image_is_blank = false;
                                }
                        }

                        if (image_is_blank) {
                                ++blank_frame_count;
                                return;
                        }
                } else {
                        printf("Blank image limit reached, resulting capture may be blank\n");
                }

                just_switched_mode = false;
                blank_frame_count = 0;
        }

        // Send the image off for processing
        mp_process_pipeline_process_image(buffer);

        if (captures_remaining > 0) {
                --captures_remaining;

                if (captures_remaining == 0) {
                        // Restore the auto exposure and gain if needed
                        if (!current_controls.exposure_is_manual) {
                                mp_camera_control_set_int32_bg(
                                        device->camera,
                                        V4L2_CID_EXPOSURE_AUTO,
                                        V4L2_EXPOSURE_AUTO);
                        }

                        if (!current_controls.gain_is_manual) {
                                mp_camera_control_set_bool_bg(
                                        device->camera, V4L2_CID_AUTOGAIN, true);
                        }

                        // Go back to preview mode
                        mp_process_pipeline_sync();
                        mp_camera_stop_capture(device->camera);

                        mode = device->preview_mode;

                        // TODO: Update media graph
                        mp_camera_set_mode(device->camera, &mode);
                        just_switched_mode = true;

                        mp_camera_start_capture(device->camera);

                        // Disable flash
                        if (device->flash && flash_enabled) {
                                mp_flash_disable(device->flash);
                        }

                        update_process_pipeline();
                }
        }
}

static void
update_state(MPPipeline *pipeline, const struct mp_io_pipeline_state *state)
{
        // Make sure the state isn't updated more than it needs to be by checking
        // whether this state change actually changes anything.
        bool has_changed = false;

        if (device != state->device) {
                has_changed = true;

                if (device) {
                        mp_process_pipeline_sync();
                        mp_camera_stop_capture(device->camera);
                        mp_device_setup_link(
                                device, device->sensor_pad, device->csi_pad, false);

                        // TODO: teardown media graph
                }

                if (capture_source) {
                        g_source_destroy(capture_source);
                        capture_source = NULL;
                }

                device = state->device;

                if (device) {
                        mp_device_setup_link(
                                device, device->sensor_pad, device->csi_pad, true);

                        // TODO: Build media graph

                        mode = device->preview_mode;
                        mp_camera_set_mode(device->camera, &mode);

                        mp_camera_start_capture(device->camera);
                        capture_source = mp_pipeline_add_capture_source(
                                pipeline, device->camera, on_frame, NULL);

                        current_controls.gain_is_manual =
                                mp_camera_control_get_bool(device->camera,
                                                           V4L2_CID_AUTOGAIN) == 0;
                        current_controls.gain = mp_camera_control_get_int32(
                                device->camera, device->gain_ctrl);

                        current_controls.exposure_is_manual =
                                mp_camera_control_get_int32(
                                        device->camera, V4L2_CID_EXPOSURE_AUTO) ==
                                V4L2_EXPOSURE_MANUAL;
                        current_controls.exposure = mp_camera_control_get_int32(
                                device->camera, V4L2_CID_EXPOSURE);
                }
        }

        has_changed = has_changed || burst_length != state->burst_length ||
                      preview_width != state->preview_width ||
                      preview_height != state->preview_height ||
                      device_rotation != state->device_rotation;

        burst_length = state->burst_length;
        preview_width = state->preview_width;
        preview_height = state->preview_height;
        device_rotation = state->device_rotation;

        if (device) {
                struct control_state previous_desired = desired_controls;

                desired_controls.gain_is_manual = state->gain_is_manual;
                desired_controls.gain = state->gain;
                desired_controls.exposure_is_manual = state->exposure_is_manual;
                desired_controls.exposure = state->exposure;

                has_changed = has_changed ||
                              memcmp(&previous_desired,
                                     &desired_controls,
                                     sizeof(struct control_state)) != 0 ||
                              flash_enabled != state->flash_enabled;

                flash_enabled = state->flash_enabled;
        }

        assert(has_changed);

        update_process_pipeline();
}

void
mp_io_pipeline_update_state(const struct mp_io_pipeline_state *state)
{
        mp_pipeline_invoke(pipeline,
                           (MPPipelineCallback)update_state,
                           state,
                           sizeof(struct mp_io_pipeline_state));
}
