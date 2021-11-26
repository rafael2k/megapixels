#include "io_pipeline.h"

#include "flash.h"
#include "pipeline.h"
#include "process_pipeline.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <libcamera/camera_manager.h>
#include <libcamera/camera.h>
#include <libcamera/framebuffer_allocator.h>

struct media_link_info {
        unsigned int source_entity_id;
        unsigned int target_entity_id;
        char source_fname[260];
        char target_fname[260];
};

struct camera_info {
        size_t index;

        unsigned int pad_id;

        char dev_fname[260];
        int fd;

        std::shared_ptr<libcamera::Camera> camera;
        std::unique_ptr<libcamera::CameraConfiguration> preview_cfg;
        std::unique_ptr<libcamera::CameraConfiguration> capture_cfg;

        MPFlash *flash;

        int gain_ctrl;
        int gain_max;

        bool has_auto_focus_continuous;
        bool has_auto_focus_start;

        // unsigned int entity_id;
        // enum v4l2_buf_type type;

        // char media_dev_fname[260];
        // char video_dev_fname[260];
        // int media_fd;

        // struct mp_media_link media_links[MP_MAX_LINKS];
        // int num_media_links;

        // int gain_ctrl;
};

static libcamera::CameraManager camera_manager = {};

static struct camera_info cameras[MP_MAX_CAMERAS];

static const struct mp_camera_config *current_camera = NULL;
static MPCameraMode mode;

static bool just_switched_mode = false;
static int blank_frame_count = 0;

static int burst_length;
static int captures_remaining = 0;

static int preview_width;
static int preview_height;

static int device_rotation;

static bool save_dng;

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
// static GSource *capture_source;

static GMutex buffer_mutex;

static void
on_frame(libcamera::Request *request);

static void acquire_camera(libcamera::Camera *camera)
{
        static libcamera::Camera *acquired_camera = NULL;
        int ret;

        if (acquired_camera != camera) {
                if (acquired_camera) {
                        ret = acquired_camera->release();
                        if (ret) {
                                printf("Failed to release camera: %s\n", strerror(-ret));
                                exit(EXIT_FAILURE);
                        }
                }

                acquired_camera = camera;

                ret = camera->acquire();
                if (ret) {
                        printf("Failed to acquire camera: %s\n", strerror(-ret));
                        exit(EXIT_FAILURE);
                }
        }
}

static void
setup_camera(const struct mp_camera_config *config)
{
        std::shared_ptr<libcamera::Camera> camera = camera_manager.get(config->id);
        if (!camera) {
                printf("Could not find camera by id: '%s'\n", config->id);
                exit(EXIT_FAILURE);
        }

        struct camera_info *info = &cameras[config->index];
        info->index = config->index;
        info->camera = camera;
        acquire_camera(camera.get());

        // Setup flash
        if (config->flash_path[0]) {
                info->flash = mp_led_flash_from_path(config->flash_path);
        } else if (config->flash_display) {
                info->flash = mp_create_display_flash();
        } else {
                info->flash = NULL;
        }

        info->preview_cfg = camera->generateConfiguration({ libcamera::Viewfinder });
        libcamera::StreamConfiguration *stream = &info->preview_cfg->at(0);
        stream->pixelFormat = libcamera::PixelFormat(mp_pixel_format_to_v4l_pixel_format(config->preview_mode.pixel_format));
        stream->size.width = config->preview_mode.width;
        stream->size.height = config->preview_mode.height;

        libcamera::CameraConfiguration::Status validation = info->preview_cfg->validate();
        if (validation == libcamera::CameraConfiguration::Invalid) {
                printf("Failed to validate preview configuration\n");
                exit(EXIT_FAILURE);
        }
        if (validation == libcamera::CameraConfiguration::Adjusted) {
                printf("Error: Preview configuration adjusted to %s\n", stream->toString().c_str());
                // exit(EXIT_FAILURE);
        }

        info->capture_cfg = camera->generateConfiguration({ libcamera::Raw });
        stream = &info->capture_cfg->at(0);
        stream->pixelFormat = libcamera::PixelFormat(mp_pixel_format_to_v4l_pixel_format(config->capture_mode.pixel_format));
        stream->size.width = config->capture_mode.width;
        stream->size.height = config->capture_mode.height;

        validation = info->capture_cfg->validate();
        if (validation == libcamera::CameraConfiguration::Invalid) {
                printf("Failed to validate capture configuration\n");
                exit(EXIT_FAILURE);
        }
        if (validation == libcamera::CameraConfiguration::Adjusted) {
                printf("Error: Capture configuration adjusted to %s\n", stream->toString().c_str());
                // exit(EXIT_FAILURE);
        }

        camera->requestCompleted.connect(on_frame);



        // info->camera = mp_camera_new(dev_info->video_fd, info->fd);

        // // Start with the capture format, this works around a bug with
        // // the ov5640 driver where it won't allow setting the preview
        // // format initially.
        // MPCameraMode mode = config->capture_mode;
        // mp_camera_set_mode(info->camera, &mode);

        // // Trigger continuous auto focus if the sensor supports it
        // if (mp_camera_query_control(
        //             info->camera, V4L2_CID_FOCUS_AUTO, NULL)) {
        //         info->has_auto_focus_continuous = true;
        //         mp_camera_control_set_bool_bg(
        //                 info->camera, V4L2_CID_FOCUS_AUTO, true);
        // }
        // if (mp_camera_query_control(
        //             info->camera, V4L2_CID_AUTO_FOCUS_START, NULL)) {
        //         info->has_auto_focus_start = true;
        // }

        // MPControl control;
        // if (mp_camera_query_control(info->camera, V4L2_CID_GAIN, &control)) {
        //         info->gain_ctrl = V4L2_CID_GAIN;
        //         info->gain_max = control.max;
        // } else if (mp_camera_query_control(
        //                    info->camera, V4L2_CID_ANALOGUE_GAIN, &control)) {
        //         info->gain_ctrl = V4L2_CID_ANALOGUE_GAIN;
        //         info->gain_max = control.max;
        // }
}

static void
setup(MPPipeline *pipeline, const void *data)
{
        g_mutex_init(&buffer_mutex);

        int ret = camera_manager.start();
        if (ret) {
                printf("Failed to start camera manager: %s\n", strerror(-ret));
                exit(EXIT_FAILURE);
        }

        printf("%ld Available cameras:\n", camera_manager.cameras().size());
        for (const auto & camera : camera_manager.cameras()) {
                printf("- %s\n", camera->id().c_str());
        }

        for (size_t i = 0; i < MP_MAX_CAMERAS; ++i) {
                const struct mp_camera_config *config = mp_get_camera_config(i);
                if (!config) {
                        break;
                }

                setup_camera(config);
        }
}

static void
clean_cameras()
{
        for (size_t i = 0; i < MP_MAX_CAMERAS; ++i) {
                cameras[i].camera = NULL;
        }

        camera_manager.stop();
}

void
mp_io_pipeline_start()
{
        mp_process_pipeline_start();

        pipeline = mp_pipeline_new();

        mp_pipeline_invoke(pipeline, setup, NULL, 0);
}

void
mp_io_pipeline_stop()
{
        // if (capture_source) {
        //         g_source_destroy(capture_source);
        // }

        clean_cameras();

        mp_pipeline_free(pipeline);

        mp_process_pipeline_stop();
}

static void
update_process_pipeline()
{
        struct camera_info *info = &cameras[current_camera->index];

        // Grab the latest control values
        // if (!current_controls.gain_is_manual) {
        //         current_controls.gain =
        //                 mp_camera_control_get_int32(info->camera, info->gain_ctrl);
        // }
        // if (!current_controls.exposure_is_manual) {
        //         current_controls.exposure =
        //                 mp_camera_control_get_int32(info->camera, V4L2_CID_EXPOSURE);
        // }

        struct mp_process_pipeline_state pipeline_state = {
                .camera = current_camera,
                .mode = mode,
                .burst_length = burst_length,
                .preview_width = preview_width,
                .preview_height = preview_height,
                .device_rotation = device_rotation,
                .gain_is_manual = current_controls.gain_is_manual,
                .gain = current_controls.gain,
                .gain_max = info->gain_max,
                .exposure_is_manual = current_controls.exposure_is_manual,
                .exposure = current_controls.exposure,
                .has_auto_focus_continuous = info->has_auto_focus_continuous,
                .has_auto_focus_start = info->has_auto_focus_start,
                .save_dng = save_dng,
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

static std::unique_ptr<libcamera::FrameBufferAllocator> frame_buffer_allocator;

struct BufferMap {
        libcamera::FrameBuffer *buffer;
        std::unique_ptr<libcamera::Request> request;

        uint8_t *data;
        size_t length;
};
static std::vector<BufferMap> buffer_maps;

static void camera_stop(const camera_info *info)
{
        mp_process_pipeline_sync();

        g_mutex_lock(&buffer_mutex);

        const struct mp_camera_config *config = mp_get_camera_config(info->index);

        int ret = info->camera->stop();
        if (ret) {
                printf("Failed to stop camera %s: %s\n", config->id, strerror(-ret));
        }

        frame_buffer_allocator.reset();

        for (const auto & buffer_map : buffer_maps)
        {
                if (munmap(buffer_map.data, buffer_map.length) == -1) {
                        printf("munmap error %d, %s\n", errno, strerror(errno));
                }
        }
        buffer_maps.clear();

        g_mutex_unlock(&buffer_mutex);
}

static uint32_t request_id = 0;

static void camera_start(const camera_info *info, libcamera::CameraConfiguration * cfg)
{
        g_mutex_lock(&buffer_mutex);

        const struct mp_camera_config *config = mp_get_camera_config(info->index);

        acquire_camera(info->camera.get());

        int ret = info->camera->configure(cfg);
        if (ret) {
                printf("Failed to configure camera %s: %s\n", config->id, strerror(-ret));
        }

        frame_buffer_allocator = std::make_unique<libcamera::FrameBufferAllocator>(info->camera);
        libcamera::Stream *stream = cfg->at(0).stream();

        ret = frame_buffer_allocator->allocate(stream);
        if (ret < 0) {
                printf("Failed to allocate buffers for camera %s: %s\n", config->id, strerror(-ret));
        }

        assert(buffer_maps.empty());
        for (const auto & buffer : frame_buffer_allocator->buffers(stream)) {
                std::unique_ptr<libcamera::Request> request = info->camera->createRequest(++request_id);
                if (!request) {
                        printf("Failed to create request camera %s\n", config->id);
                }

                ret = request->addBuffer(stream, buffer.get());
                if (ret) {
                        printf("Can't set buffer for request\n");
                }

                const libcamera::FrameBuffer::Plane &plane = buffer->planes()[0];

                uint8_t *data = (uint8_t *) mmap(NULL,
                                       plane.length,
                                       PROT_READ,
                                       MAP_SHARED,
                                       plane.fd.fd(),
                                       plane.offset);

                if (data == MAP_FAILED) {
                        printf("mmap error %d, %s\n", errno, strerror(errno));
                        break;
                }

                buffer_maps.push_back(BufferMap {
                        .buffer = buffer.get(),
                        .request = std::move(request),
                        .data = data,
                        .length = plane.length,
                });
        }

        ret = info->camera->start();
        if (ret) {
                printf("Failed to stop camera %s: %s\n", config->id, strerror(-ret));
        }

        for (const auto & buffer_map : buffer_maps) {
                ret = info->camera->queueRequest(buffer_map.request.get());
                if (ret) {
                        printf("Failed to queue request for camera %s: %s\n", config->id, strerror(-ret));
                }
        }

        g_mutex_unlock(&buffer_mutex);
}

static void
capture(MPPipeline *pipeline, const void *data)
{
        struct camera_info *info = &cameras[current_camera->index];

        captures_remaining = burst_length;

        // Disable the autogain/exposure while taking the burst
        // mp_camera_control_set_int32(info->camera, V4L2_CID_AUTOGAIN, 0);
        // mp_camera_control_set_int32(
        //         info->camera, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);

        // Change camera mode for capturing
        camera_stop(info);

        camera_start(info, info->capture_cfg.get());
        mode = current_camera->capture_mode;
        just_switched_mode = true;

        // Enable flash
        if (info->flash && flash_enabled) {
                mp_flash_enable(info->flash);
        }

        update_process_pipeline();

        mp_process_pipeline_capture();
}

void
mp_io_pipeline_capture()
{
        mp_pipeline_invoke(pipeline, capture, NULL, 0);
}

void
mp_io_pipeline_release_buffer(const MPBuffer *buffer)
{
        g_mutex_lock(&buffer_mutex);

        if (buffer->index >= buffer_maps.size()
                || buffer_maps[buffer->index].data != buffer->data) {

                g_mutex_unlock(&buffer_mutex);
                return;
        }

        struct camera_info *info = &cameras[current_camera->index];

        buffer_maps[buffer->index].request->reuse(libcamera::Request::ReuseBuffers);
        info->camera->queueRequest(buffer_maps[buffer->index].request.get());

        g_mutex_unlock(&buffer_mutex);
}

// static pid_t focus_continuous_task = 0;
// static pid_t start_focus_task = 0;
// static void
// start_focus(struct camera_info *info)
// {
//         // only run 1 manual focus at once
//         if (!mp_camera_check_task_complete(info->camera, start_focus_task) ||
//             !mp_camera_check_task_complete(info->camera, focus_continuous_task))
//                 return;

//         if (info->has_auto_focus_continuous) {
//                 focus_continuous_task = mp_camera_control_set_bool_bg(
//                         info->camera, V4L2_CID_FOCUS_AUTO, 1);
//         } else if (info->has_auto_focus_start) {
//                 start_focus_task = mp_camera_control_set_bool_bg(
//                         info->camera, V4L2_CID_AUTO_FOCUS_START, 1);
//         }
// }

static void
update_controls()
{
        // Don't update controls while capturing
        // if (captures_remaining > 0) {
        //         return;
        // }

        // struct camera_info *info = &cameras[current_camera->index];

        // if (want_focus) {
        //         start_focus(info);
        //         want_focus = false;
        // }

        // if (current_controls.gain_is_manual != desired_controls.gain_is_manual) {
        //         mp_camera_control_set_bool_bg(info->camera,
        //                                       V4L2_CID_AUTOGAIN,
        //                                       !desired_controls.gain_is_manual);
        // }

        // if (desired_controls.gain_is_manual &&
        //     current_controls.gain != desired_controls.gain) {
        //         mp_camera_control_set_int32_bg(
        //                 info->camera, info->gain_ctrl, desired_controls.gain);
        // }

        // if (current_controls.exposure_is_manual !=
        //     desired_controls.exposure_is_manual) {
        //         mp_camera_control_set_int32_bg(info->camera,
        //                                        V4L2_CID_EXPOSURE_AUTO,
        //                                        desired_controls.exposure_is_manual ?
        //                                                V4L2_EXPOSURE_MANUAL :
        //                                                V4L2_EXPOSURE_AUTO);
        // }

        // if (desired_controls.exposure_is_manual &&
        //     current_controls.exposure != desired_controls.exposure) {
        //         mp_camera_control_set_int32_bg(
        //                 info->camera, V4L2_CID_EXPOSURE, desired_controls.exposure);
        // }

        current_controls = desired_controls;
}

static void
on_frame(libcamera::Request *request)
{
        if (request->status() == libcamera::Request::RequestCancelled) {
                return;
        }

        g_mutex_lock(&buffer_mutex);

        // Find buffer
        MPBuffer buffer = { .fd = -1 };
        for (size_t i = 0; i < buffer_maps.size(); ++i) {
                if (buffer_maps[i].request.get() == request) {
                        buffer.index = i;
                        buffer.data = buffer_maps[i].data;
                        buffer.fd = 0;
                        break;
                }
        }

        if (buffer.fd != 0) {
                g_mutex_unlock(&buffer_mutex);
                return;
        }

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

                                g_mutex_unlock(&buffer_mutex);
                                return;
                        }
                } else {
                        printf("Blank image limit reached, resulting capture may be blank\n");
                }

                just_switched_mode = false;
                blank_frame_count = 0;
        }

        // Send the image off for processing
        if (!mp_process_pipeline_process_image(buffer)) {
                g_mutex_unlock(&buffer_mutex);
                mp_io_pipeline_release_buffer(&buffer);
                return;
        }

        if (captures_remaining > 0) {
                --captures_remaining;

                if (captures_remaining == 0) {
                        struct camera_info *info = &cameras[current_camera->index];

                        // Restore the auto exposure and gain if needed
                        // if (!current_controls.exposure_is_manual) {
                        //         mp_camera_control_set_int32_bg(
                        //                 info->camera,
                        //                 V4L2_CID_EXPOSURE_AUTO,
                        //                 V4L2_EXPOSURE_AUTO);
                        // }

                        // if (!current_controls.gain_is_manual) {
                        //         mp_camera_control_set_bool_bg(
                        //                 info->camera, V4L2_CID_AUTOGAIN, true);
                        // }

                        // Go back to preview mode
                        camera_stop(info);

                        camera_start(info, info->preview_cfg.get());
                        mode = current_camera->preview_mode;
                        just_switched_mode = true;

                        // Disable flash
                        if (info->flash) {
                                mp_flash_disable(info->flash);
                        }

                        update_process_pipeline();
                }
        }

        g_mutex_unlock(&buffer_mutex);
}

static void
update_state(MPPipeline *pipeline, const struct mp_io_pipeline_state *state)
{
        // Make sure the state isn't updated more than it needs to be by checking
        // whether this state change actually changes anything.
        bool has_changed = false;

        if (current_camera != state->camera) {
                has_changed = true;

                if (current_camera) {
                        struct camera_info *info = &cameras[current_camera->index];

                        camera_stop(info);
                }

                // if (capture_source) {
                //         g_source_destroy(capture_source);
                //         capture_source = NULL;
                // }

                current_camera = state->camera;

                if (current_camera) {
                        struct camera_info *info = &cameras[current_camera->index];

                        mode = current_camera->preview_mode;
                        camera_start(info, info->preview_cfg.get());

                        // capture_source = mp_pipeline_add_capture_source(
                        //         pipeline, info->camera, on_frame, NULL);

                        // current_controls.gain_is_manual =
                        //         mp_camera_control_get_bool(info->camera,
                        //                                    V4L2_CID_AUTOGAIN) == 0;
                        // current_controls.gain = mp_camera_control_get_int32(
                        //         info->camera, info->gain_ctrl);

                        // current_controls.exposure_is_manual =
                        //         mp_camera_control_get_int32(
                        //                 info->camera, V4L2_CID_EXPOSURE_AUTO) ==
                        //         V4L2_EXPOSURE_MANUAL;
                        // current_controls.exposure = mp_camera_control_get_int32(
                        //         info->camera, V4L2_CID_EXPOSURE);
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
        save_dng = state->save_dng;

        if (current_camera) {
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
