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
#include <libcamera/control_ids.h>
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

        int auto_focus_ctrl;

        int auto_gain_ctrl;
        int manual_gain_ctrl;
        int gain_max;

        int auto_exposure_ctrl;
        int manual_exposure_ctrl;
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

static struct control_state current_controls = {};

static bool flash_enabled = false;

static bool want_focus = false;

static MPPipeline *pipeline;

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

        const libcamera::ControlInfoMap & controls = camera->controls();

        // Fetch control info
        if (controls.find(libcamera::controls::AF_ENABLED) != controls.end()) {
                info->auto_focus_ctrl = libcamera::controls::AF_ENABLED;
        } else if (controls.find(libcamera::controls::AF_START) != controls.end()) {
                info->auto_focus_ctrl = libcamera::controls::AF_START;
        }

        if (controls.find(libcamera::controls::AUTO_GAIN) != controls.end()) {
                info->auto_gain_ctrl = libcamera::controls::AUTO_GAIN;
        }

        if (controls.find(libcamera::controls::DIGITAL_GAIN) != controls.end()) {
                info->manual_gain_ctrl = libcamera::controls::DIGITAL_GAIN;
                info->gain_max = controls.at(libcamera::controls::DIGITAL_GAIN).max().get<float>();
        } else if (controls.find(libcamera::controls::ANALOGUE_GAIN) != controls.end()) {
                info->manual_gain_ctrl = libcamera::controls::ANALOGUE_GAIN;
                info->gain_max = controls.at(libcamera::controls::ANALOGUE_GAIN).max().get<float>();
        }

        if (controls.find(libcamera::controls::AE_ENABLE) != controls.end()) {
                info->auto_exposure_ctrl = libcamera::controls::AE_ENABLE;
        }

        if (controls.find(libcamera::controls::EXPOSURE_VALUE) != controls.end()) {
                info->manual_exposure_ctrl = libcamera::controls::EXPOSURE_VALUE;
        }
}

static void
setup(MPPipeline *pipeline, const void *data)
{
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
        //                 info->camera->properties().get(info->manual_gain_ctrl).get<int>();
        // }
        // if (!current_controls.exposure_is_manual) {
        //         current_controls.exposure =
        //                 info->camera->properties().get(info->manual_exposure_ctrl).get<int>();
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

static libcamera::Stream *active_stream;

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

        const struct mp_camera_config *config = mp_get_camera_config(info->index);

        active_stream = NULL;

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
}

static void queue_request(libcamera::Request *request)
{
        struct camera_info *info = &cameras[current_camera->index];

        bool desired_wants_focus = want_focus;
        struct control_state desired_controls = current_controls;

        if (captures_remaining > 0) {
                desired_wants_focus = false;
                desired_controls.gain_is_manual = true;
                desired_controls.exposure_is_manual = true;
        }

        // Set controls
        libcamera::ControlList *controls = &request->controls();

        if (info->auto_focus_ctrl) {
                controls->set(info->auto_focus_ctrl, libcamera::ControlValue(desired_wants_focus));
        }

        if (info->auto_gain_ctrl) {
                controls->set(info->auto_gain_ctrl, libcamera::ControlValue(!desired_controls.gain_is_manual));
                controls->set(info->manual_gain_ctrl, libcamera::ControlValue((float) desired_controls.gain));
        }

        if (info->auto_exposure_ctrl) {
                controls->set(info->auto_exposure_ctrl, libcamera::ControlValue(!desired_controls.exposure_is_manual));
                controls->set(info->manual_exposure_ctrl, libcamera::ControlValue((float) desired_controls.exposure));
        }

        int ret = info->camera->queueRequest(request);
        if (ret) {
                printf("Failed to queue request: %s\n", strerror(-ret));
        }

        if (desired_wants_focus) {
                want_focus = false;
        }
}

static void camera_start(const camera_info *info, libcamera::CameraConfiguration * cfg)
{
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
                std::unique_ptr<libcamera::Request> request = info->camera->createRequest(buffer_maps.size());
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

        active_stream = stream;

        for (const auto & buffer_map : buffer_maps) {
                queue_request(buffer_map.request.get());
        }
}

static void
capture(MPPipeline *pipeline, const void *data)
{
        struct camera_info *info = &cameras[current_camera->index];

        captures_remaining = burst_length;

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

static void
release_buffer(MPPipeline *pipeline, const MPBuffer *buffer)
{
        if (buffer->index >= buffer_maps.size()
                || buffer_maps[buffer->index].data != buffer->data) {
                return;
        }

        buffer_maps[buffer->index].request->reuse(libcamera::Request::ReuseBuffers);
        queue_request(buffer_maps[buffer->index].request.get());
}

void
mp_io_pipeline_release_buffer(const MPBuffer *buffer)
{
        mp_pipeline_invoke(
                pipeline,
                (MPPipelineCallback)release_buffer,
                buffer,
                sizeof(MPBuffer));
}

struct FrameData {
        uint32_t buffer_index;
        libcamera::Stream *stream;
};

static void
on_frame_impl(MPPipeline *pipeline, FrameData *frame_data)
{
        if (frame_data->stream != active_stream || !active_stream) {
                return;
        }

        // Find buffer
        MPBuffer buffer = {
                .index = frame_data->buffer_index,
                .data = buffer_maps[frame_data->buffer_index].data,
                .fd = 0,
        };

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

                                mp_io_pipeline_release_buffer(&buffer);
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
                        struct camera_info *info = &cameras[current_camera->index];

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
}

static void
on_frame(libcamera::Request *request)
{
        if (request->status() == libcamera::Request::RequestCancelled
                || !active_stream) {
                return;
        }

        FrameData data = {
                .buffer_index = (uint32_t) request->cookie(),
                .stream = active_stream,
        };

        mp_pipeline_invoke(
                pipeline,
                (MPPipelineCallback)on_frame_impl,
                &data,
                sizeof(FrameData));
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

                current_camera = state->camera;

                if (current_camera) {
                        struct camera_info *info = &cameras[current_camera->index];

                        mode = current_camera->preview_mode;
                        camera_start(info, info->preview_cfg.get());
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
                struct control_state previous_desired = current_controls;

                current_controls.gain_is_manual = state->gain_is_manual;
                current_controls.gain = state->gain;
                current_controls.exposure_is_manual = state->exposure_is_manual;
                current_controls.exposure = state->exposure;

                has_changed = has_changed ||
                              memcmp(&previous_desired,
                                     &current_controls,
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
