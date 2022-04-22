#include "process_pipeline.h"

#include "config.h"
#include "gles2_debayer.h"
#include "io_pipeline.h"
#include "main.h"
#include "pipeline.h"
#include "zbar_pipeline.h"
#include "av_pipeline.h"
#include "panfrost_swizzle.h"
#include <assert.h>
#include <gtk/gtk.h>
#include <math.h>
#include <tiffio.h>

#include "gl_util.h"
#include <libdrm/drm_fourcc.h>
#include <sys/mman.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define TIFFTAG_FORWARDMATRIX1 50964

static const float colormatrix_srgb[] = { 3.2409, -1.5373, -0.4986, -0.9692, 1.8759,
                                          0.0415, 0.0556,  -0.2039, 1.0569 };

static MPPipeline *pipeline;

static char burst_dir[23];
static char processing_script[512];

static volatile bool is_capturing = false;
static volatile int frames_processed = 0;
static volatile int frames_received = 0;

static const struct mp_camera_config *camera;
static int camera_rotation;

static MPCameraMode camera_mode;

static MPAppMode app_mode;

static int burst_length;
static int captures_remaining = 0;

static int preview_width;
static int preview_height;

static int device_rotation;

static int output_buffer_width = -1;
static int output_buffer_height = -1;

// static bool gain_is_manual;
static int gain;
static int gain_max;

static bool exposure_is_manual;
static int exposure;

static bool save_dng;

static char capture_fname[255];

static void
register_custom_tiff_tags(TIFF *tif)
{
        static const TIFFFieldInfo custom_fields[] = {
                { TIFFTAG_FORWARDMATRIX1,
                  -1,
                  -1,
                  TIFF_SRATIONAL,
                  FIELD_CUSTOM,
                  1,
                  1,
                  "ForwardMatrix1" },
        };

        // Add missing dng fields
        TIFFMergeFieldInfo(tif,
                           custom_fields,
                           sizeof(custom_fields) / sizeof(custom_fields[0]));
}

static bool
find_processor(char *script)
{
        char filename[] = "postprocess.sh";

        // Check postprocess.sh in the current working directory
        sprintf(script, "./data/%s", filename);
        if (access(script, F_OK) != -1) {
                sprintf(script, "./data/%s", filename);
                printf("Found postprocessor script at %s\n", script);
                return true;
        }

        // Check for a script in XDG_CONFIG_HOME
        sprintf(script, "%s/megapixels/%s", g_get_user_config_dir(), filename);
        if (access(script, F_OK) != -1) {
                printf("Found postprocessor script at %s\n", script);
                return true;
        }

        // Check user overridden /etc/megapixels/postprocessor.sh
        sprintf(script, "%s/megapixels/%s", SYSCONFDIR, filename);
        if (access(script, F_OK) != -1) {
                printf("Found postprocessor script at %s\n", script);
                return true;
        }

        // Check packaged /usr/share/megapixels/postprocessor.sh
        sprintf(script, "%s/megapixels/%s", DATADIR, filename);
        if (access(script, F_OK) != -1) {
                printf("Found postprocessor script at %s\n", script);
                return true;
        }

        return false;
}

static void
setup(MPPipeline *pipeline, const void *data)
{
        TIFFSetTagExtender(register_custom_tiff_tags);

        if (!find_processor(processing_script)) {
                g_printerr("Could not find any post-process script\n");
                exit(1);
        }
}

void
mp_process_pipeline_start()
{
        pipeline = mp_pipeline_new();

        mp_pipeline_invoke(pipeline, setup, NULL, 0);

        mp_zbar_pipeline_start();
        mp_av_pipeline_start();
}

void
mp_process_pipeline_stop()
{
        mp_pipeline_free(pipeline);

        mp_zbar_pipeline_stop();
        mp_av_pipeline_stop();
}

void
mp_process_pipeline_sync()
{
        mp_pipeline_sync(pipeline);
}

#define NUM_BUFFERS 4

struct _MPProcessPipelineBuffer {
        GLuint texture_id;
        int fd;
        int32_t offset;
        int32_t stride;

        uint32_t *data;
        size_t size;
        EGLImage egl_image;

        _Atomic(int) refcount;
};
static MPProcessPipelineBuffer output_buffers[NUM_BUFFERS];

void
mp_process_pipeline_buffer_ref(MPProcessPipelineBuffer *buf)
{
        ++buf->refcount;
}

void
mp_process_pipeline_buffer_unref(MPProcessPipelineBuffer *buf)
{
        --buf->refcount;
}

uint32_t
mp_process_pipeline_buffer_get_texture_id(MPProcessPipelineBuffer *buf)
{
        return buf->texture_id;
}

static void
repack_image_sequencial(const uint8_t *src_buf,
                        uint8_t *dst_buf,
                        size_t width,
                        size_t height)
{
        uint16_t pixels[4];

        /*
         * Repack 40 bits stored in sensor format into sequencial format
         *
         * src_buf: 11111111 22222222 33333333 44444444 11223344 ...
         * dst_buf: 11111111 11222222 22223333 33333344 44444444 ...
         */
        assert(width % 4 == 0);
        for (size_t i = 0; i < (width + width / 4) * height; i += 5) {
                /* Extract pixels from packed sensor format */
                pixels[0] = (src_buf[i] << 2) | (src_buf[i + 4] >> 6);
                pixels[1] = (src_buf[i + 1] << 2) | (src_buf[i + 4] >> 4 & 0x03);
                pixels[2] = (src_buf[i + 2] << 2) | (src_buf[i + 4] >> 2 & 0x03);
                pixels[3] = (src_buf[i + 3] << 2) | (src_buf[i + 4] & 0x03);

                /* Pack pixels into sequencial format */
                dst_buf[i] = (pixels[0] >> 2 & 0xff);
                dst_buf[i + 1] = (pixels[0] << 6 & 0xff) | (pixels[1] >> 4 & 0x3f);
                dst_buf[i + 2] = (pixels[1] << 4 & 0xff) | (pixels[2] >> 6 & 0x0f);
                dst_buf[i + 3] = (pixels[2] << 2 & 0xff) | (pixels[3] >> 8 & 0x03);
                dst_buf[i + 4] = (pixels[3] & 0xff);
        }
}

const uint32_t *mp_process_pipeline_buffer_get_pixels(MPProcessPipelineBuffer *buf)
{
        return (uint32_t *)buf->data;
}

static GLES2Debayer *gles2_debayer = NULL;

static GdkGLContext *context;

// #define RENDERDOC

#ifdef RENDERDOC
#include <renderdoc/app.h>
extern RENDERDOC_API_1_1_2 *rdoc_api;
#endif

static EGLBoolean (*eglExportDMABUFImageQueryMESA)(EGLDisplay dpy,
                                                   EGLImage image,
                                                   int *fourcc,
                                                   int *num_planes,
                                                   EGLuint64KHR *modifiers);

static EGLBoolean (*eglExportDMABUFImageMESA)(EGLDisplay dpy,
                                              EGLImage image,
                                              int *fds,
                                              EGLint *strides,
                                              EGLint *offsets);

static void
init_gl(MPPipeline *pipeline, GdkSurface **surface)
{
        GError *error = NULL;
        context = gdk_surface_create_gl_context(*surface, &error);
        if (context == NULL) {
                printf("Failed to initialize OpenGL context: %s\n", error->message);
                g_clear_error(&error);
                return;
        }

        gdk_gl_context_set_use_es(context, true);
        gdk_gl_context_set_required_version(context, 2, 0);
        gdk_gl_context_set_forward_compatible(context, false);
#ifdef DEBUG
        gdk_gl_context_set_debug_enabled(context, true);
#else
        gdk_gl_context_set_debug_enabled(context, false);
#endif

        gdk_gl_context_realize(context, &error);
        if (error != NULL) {
                printf("Failed to create OpenGL context: %s\n", error->message);
                g_clear_object(&context);
                g_clear_error(&error);
                return;
        }

        gdk_gl_context_make_current(context);
        check_gl();

        // Get EGL extensions for DMA
        EGLContext egl_context = eglGetCurrentContext();
        assert(egl_context != EGL_NO_CONTEXT);

        eglExportDMABUFImageQueryMESA = (void*) eglGetProcAddress("eglExportDMABUFImageQueryMESA");
        eglExportDMABUFImageMESA = (void*) eglGetProcAddress("eglExportDMABUFImageMESA");

        // Make a VAO for OpenGL
        if (!gdk_gl_context_get_use_es(context)) {
                GLuint vao;
                glGenVertexArrays(1, &vao);
                glBindVertexArray(vao);
                check_gl();
        }

        gles2_debayer = gles2_debayer_new(MP_PIXEL_FMT_BGGR8);
        check_gl();

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        check_gl();

        gles2_debayer_use(gles2_debayer);

        for (size_t i = 0; i < NUM_BUFFERS; ++i) {
                MPProcessPipelineBuffer *buf = &output_buffers[i];
                buf->fd = -1;
                buf->data = MAP_FAILED;
                buf->egl_image = EGL_NO_IMAGE;
                buf->size = 0;

                glGenTextures(1, &buf->texture_id);
                glBindTexture(GL_TEXTURE_2D, buf->texture_id);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }

        glBindTexture(GL_TEXTURE_2D, 0);

        gboolean is_es = gdk_gl_context_get_use_es(context);
        int major, minor;
        gdk_gl_context_get_version(context, &major, &minor);

        printf("Initialized %s %d.%d\n", is_es ? "OpenGL ES" : "OpenGL", major, minor);

        printf("  extensions: %s\n", eglQueryString(eglGetCurrentDisplay(),  EGL_EXTENSIONS));
}

void
mp_process_pipeline_init_gl(GdkSurface *surface)
{
        mp_pipeline_invoke(pipeline,
                           (MPPipelineCallback)init_gl,
                           &surface,
                           sizeof(GdkSurface *));
}

static GdkTexture *
process_image_for_preview(const uint8_t *image)
{
#ifdef PROFILE_DEBAYER
        clock_t t1 = clock();
#endif

        // Pick an available buffer
        MPProcessPipelineBuffer *output_buffer = NULL;
        for (size_t i = 0; i < NUM_BUFFERS; ++i) {
                if (output_buffers[i].refcount == 0) {
                        output_buffer = &output_buffers[i];
                }
        }

        if (output_buffer == NULL) {
                return NULL;
        }
        assert(output_buffer != NULL);

#ifdef RENDERDOC
        if (rdoc_api) {
                rdoc_api->StartFrameCapture(NULL, NULL);
        }
#endif

        GLuint input_texture = 0;

        if (camera_mode.pixel_format == MP_PIXEL_FMT_RGB565) {
                glBindTexture(GL_TEXTURE_2D, output_buffer->texture_id);
                glTexImage2D(GL_TEXTURE_2D,
                             0,
                             GL_RGB,
                             camera_mode.width,
                             camera_mode.height,
                             0,
                             GL_RGB,
                             GL_UNSIGNED_SHORT_5_6_5,
                             image);
                check_gl();
        } else {
                // Copy image to a GL texture. TODO: This can be avoided

                glGenTextures(1, &input_texture);
                glBindTexture(GL_TEXTURE_2D, input_texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D,
                             0,
                             GL_LUMINANCE,
                             mp_pixel_format_width_to_bytes(camera_mode.pixel_format,
                                                            camera_mode.width),
                             camera_mode.height,
                             0,
                             GL_LUMINANCE,
                             GL_UNSIGNED_BYTE,
                             image);
                check_gl();

                gles2_debayer_process(
                        gles2_debayer, output_buffer->texture_id, input_texture);
                check_gl();
        }

        glFinish();

        if (input_texture) {
                glDeleteTextures(1, &input_texture);
        }

#ifdef PROFILE_DEBAYER
        clock_t t2 = clock();
        printf("process_image_for_preview %fms\n",
               (float)(t2 - t1) / CLOCKS_PER_SEC * 1000);
#endif

#ifdef DUMP_PREVIEW
        // Optionally dump each preview frame to disk for debugging purposes
        {
                static int dump_count = 0;
                char buf[128];
                snprintf(buf, 128, "preview%d.dump", dump_count);
                ++dump_count;

                uint32_t *data = malloc(output_buffer->size);
                panfrost_load_tiled_image(
                        data,
                        output_buffer->data,
                        output_buffer_width,
                        output_buffer_height,
                        output_buffer->stride,
                        output_buffer->stride);

                FILE *f = fopen(buf, "w");
                fwrite(data, output_buffer->size, 1, f);
                fclose(f);
                printf("Dumped %s\n", buf);

                free(data);
        }
#endif

#ifdef RENDERDOC
        if (rdoc_api) {
                rdoc_api->EndFrameCapture(NULL, NULL);
        }
#endif

        mp_process_pipeline_buffer_ref(output_buffer);
        mp_main_set_preview(output_buffer);

        // Create a thumbnail from the preview for the last capture
        GdkTexture *thumb = NULL;
        if (app_mode == MP_APP_MODE_PICTURE && captures_remaining == 1) {
                printf("Making thumbnail\n");

                size_t size = output_buffer_height * output_buffer->stride;

                uint32_t *data = g_malloc_n(size, 1);
                panfrost_load_tiled_image(
                        data,
                        output_buffer->data,
                        output_buffer_width,
                        output_buffer_height,
                        output_buffer->stride,
                        output_buffer->stride);

                // Flip vertically
                uint32_t swidth = output_buffer->stride / sizeof(uint32_t);
                for (size_t y = 0; y < output_buffer_height / 2; ++y) {
                        for (size_t x = 0; x < swidth; ++x) {
                                uint32_t tmp = data[(output_buffer_height - y - 1) * swidth + x];
                                data[(output_buffer_height - y - 1) * swidth + x] = data[y * swidth + x];
                                data[y * swidth + x] = tmp;
                        }
                }

                thumb = gdk_memory_texture_new(
                        output_buffer_width,
                        output_buffer_height,
                        GDK_MEMORY_R8G8B8A8,
                        g_bytes_new_take(data, size),
                        output_buffer->stride);
        }

        return thumb;
}

static void
process_image_for_capture(const uint8_t *image, int count)
{
        time_t rawtime;
        time(&rawtime);
        struct tm tim = *(localtime(&rawtime));

        char datetime[20] = { 0 };
        strftime(datetime, 20, "%Y:%m:%d %H:%M:%S", &tim);

        char fname[255];
        sprintf(fname, "%s/%d.dng", burst_dir, count);

        TIFF *tif = TIFFOpen(fname, "w");
        if (!tif) {
                printf("Could not open tiff\n");
        }

        // Define TIFF thumbnail
        TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 1);
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, camera_mode.width >> 4);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, camera_mode.height >> 4);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(tif, TIFFTAG_MAKE, mp_get_device_make());
        TIFFSetField(tif, TIFFTAG_MODEL, mp_get_device_model());
        uint16_t orientation;
        if (camera_rotation == 0) {
                orientation = camera->mirrored ? ORIENTATION_TOPRIGHT :
                                                 ORIENTATION_TOPLEFT;
        } else if (camera_rotation == 90) {
                orientation = camera->mirrored ? ORIENTATION_RIGHTBOT :
                                                 ORIENTATION_LEFTBOT;
        } else if (camera_rotation == 180) {
                orientation = camera->mirrored ? ORIENTATION_BOTLEFT :
                                                 ORIENTATION_BOTRIGHT;
        } else {
                orientation = camera->mirrored ? ORIENTATION_LEFTTOP :
                                                 ORIENTATION_RIGHTTOP;
        }
        TIFFSetField(tif, TIFFTAG_ORIENTATION, orientation);
        TIFFSetField(tif, TIFFTAG_DATETIME, datetime);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_SOFTWARE, "Megapixels");
        long sub_offset = 0;
        TIFFSetField(tif, TIFFTAG_SUBIFD, 1, &sub_offset);
        TIFFSetField(tif, TIFFTAG_DNGVERSION, "\001\001\0\0");
        TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, "\001\0\0\0");
        char uniquecameramodel[255];
        sprintf(uniquecameramodel,
                "%s %s",
                mp_get_device_make(),
                mp_get_device_model());
        TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, uniquecameramodel);
        if (camera->colormatrix[0]) {
                TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, camera->colormatrix);
        } else {
                TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, colormatrix_srgb);
        }
        if (camera->forwardmatrix[0]) {
                TIFFSetField(tif, TIFFTAG_FORWARDMATRIX1, 9, camera->forwardmatrix);
        }
        static const float neutral[] = { 1.0, 1.0, 1.0 };
        TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
        TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, 21);
        // Write black thumbnail, only windows uses this
        {
                unsigned char *buf =
                        (unsigned char *)calloc(1, (camera_mode.width >> 4) * 3);
                for (int row = 0; row < (camera_mode.height >> 4); row++) {
                        TIFFWriteScanline(tif, buf, row, 0);
                }
                free(buf);
        }
        TIFFWriteDirectory(tif);

        // Define main photo
        TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, camera_mode.width);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, camera_mode.height);
        TIFFSetField(tif,
                     TIFFTAG_BITSPERSAMPLE,
                     mp_pixel_format_bits_per_pixel(camera_mode.pixel_format));
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        static const short cfapatterndim[] = { 2, 2 };
        TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, cfapatterndim);
#if (TIFFLIB_VERSION < 20201219) && !LIBTIFF_CFA_PATTERN
        TIFFSetField(tif,
                     TIFFTAG_CFAPATTERN,
                     mp_pixel_format_cfa_pattern(camera_mode.pixel_format));
#else
        TIFFSetField(tif,
                     TIFFTAG_CFAPATTERN,
                     4,
                     mp_pixel_format_cfa_pattern(camera_mode.pixel_format));
#endif
        printf("TIFF version %d\n", TIFFLIB_VERSION);
        int whitelevel = camera->whitelevel;
        if (!whitelevel) {
                whitelevel = (1 << mp_pixel_format_pixel_depth(
                                      camera_mode.pixel_format)) -
                             1;
        }
        TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &whitelevel);
        if (camera->blacklevel) {
                const float blacklevel = camera->blacklevel;
                TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 1, &blacklevel);
        }
        TIFFCheckpointDirectory(tif);
        printf("Writing frame to %s\n", fname);

        uint8_t *output_image = (uint8_t *)image;

        // Repack 10-bit image from sensor format into a sequencial format
        if (mp_pixel_format_bits_per_pixel(camera_mode.pixel_format) == 10) {
                output_image =
                        malloc(mp_pixel_format_width_to_bytes(
                                       camera_mode.pixel_format, camera_mode.width) *
                               camera_mode.height);

                repack_image_sequencial(
                        image, output_image, camera_mode.width, camera_mode.height);
        }

        for (int row = 0; row < camera_mode.height; row++) {
                TIFFWriteScanline(tif,
                                  (void *)output_image +
                                          (row * mp_pixel_format_width_to_bytes(
                                                         camera_mode.pixel_format,
                                                         camera_mode.width)),
                                  row,
                                  0);
        }
        TIFFWriteDirectory(tif);

        if (output_image != image)
                free(output_image);

        // Add an EXIF block to the tiff
        TIFFCreateEXIFDirectory(tif);
        // 1 = manual, 2 = full auto, 3 = aperture priority, 4 = shutter priority
        if (!exposure_is_manual) {
                TIFFSetField(tif, EXIFTAG_EXPOSUREPROGRAM, 2);
        } else {
                TIFFSetField(tif, EXIFTAG_EXPOSUREPROGRAM, 1);
        }

        TIFFSetField(tif,
                     EXIFTAG_EXPOSURETIME,
                     (camera_mode.frame_interval.numerator /
                      (float)camera_mode.frame_interval.denominator) /
                             ((float)camera_mode.height / (float)exposure));
        if (camera->iso_min && camera->iso_max) {
                uint16_t isospeed = remap(
                        gain - 1, 0, gain_max, camera->iso_min, camera->iso_max);
                TIFFSetField(tif, EXIFTAG_ISOSPEEDRATINGS, 1, &isospeed);
        }
        TIFFSetField(tif, EXIFTAG_FLASH, 0);

        TIFFSetField(tif, EXIFTAG_DATETIMEORIGINAL, datetime);
        TIFFSetField(tif, EXIFTAG_DATETIMEDIGITIZED, datetime);
        if (camera->fnumber) {
                TIFFSetField(tif, EXIFTAG_FNUMBER, camera->fnumber);
        }
        if (camera->focallength) {
                TIFFSetField(tif, EXIFTAG_FOCALLENGTH, camera->focallength);
        }
        if (camera->focallength && camera->cropfactor) {
                TIFFSetField(tif,
                             EXIFTAG_FOCALLENGTHIN35MMFILM,
                             (short)(camera->focallength * camera->cropfactor));
        }
        uint64_t exif_offset = 0;
        TIFFWriteCustomDirectory(tif, &exif_offset);
        TIFFFreeDirectory(tif);

        // Update exif pointer
        TIFFSetDirectory(tif, 0);
        TIFFSetField(tif, TIFFTAG_EXIFIFD, exif_offset);
        TIFFRewriteDirectory(tif);

        TIFFClose(tif);
}

static void
post_process_finished(GSubprocess *proc, GAsyncResult *res, GdkTexture *thumb)
{
        char *stdout;
        g_subprocess_communicate_utf8_finish(proc, res, &stdout, NULL, NULL);

        // The last line contains the file name
        int end = strlen(stdout);
        // Skip the newline at the end
        stdout[--end] = '\0';

        char *path = path = stdout + end - 1;
        do {
                if (*path == '\n') {
                        path++;
                        break;
                }
                --path;
        } while (path > stdout);

        mp_main_capture_completed(thumb, path);
}

static void
process_capture_burst(GdkTexture *thumb)
{
        time_t rawtime;
        time(&rawtime);
        struct tm tim = *(localtime(&rawtime));

        char timestamp[30];
        strftime(timestamp, 30, "%Y%m%d%H%M%S", &tim);

        if (g_get_user_special_dir(G_USER_DIRECTORY_PICTURES) != NULL) {
                sprintf(capture_fname,
                        "%s/IMG%s",
                        g_get_user_special_dir(G_USER_DIRECTORY_PICTURES),
                        timestamp);
        } else if (getenv("XDG_PICTURES_DIR") != NULL) {
                sprintf(capture_fname,
                        "%s/IMG%s",
                        getenv("XDG_PICTURES_DIR"),
                        timestamp);
        } else {
                sprintf(capture_fname,
                        "%s/Pictures/IMG%s",
                        getenv("HOME"),
                        timestamp);
        }

        char save_dng_s[2] = "0";
        if (save_dng) {
                save_dng_s[0] = '1';
        }

        // Start post-processing the captured burst
        g_print("Post process %s to %s.ext (save-dng %s)\n",
                burst_dir,
                capture_fname,
                save_dng_s);
        g_autoptr(GError) error = NULL;
        GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                                             &error,
                                             processing_script,
                                             burst_dir,
                                             capture_fname,
                                             save_dng_s,
                                             NULL);

        if (!proc) {
                g_printerr("Failed to spawn postprocess process: %s\n",
                           error->message);
                return;
        }

        g_subprocess_communicate_utf8_async(
                proc, NULL, NULL, (GAsyncReadyCallback)post_process_finished, thumb);
}

static void
process_image(MPPipeline *pipeline, const MPBuffer *buffer)
{
#ifdef PROFILE_PROCESS
        clock_t t1 = clock();
#endif

        size_t size = mp_pixel_format_width_to_bytes(camera_mode.pixel_format,
                                                     camera_mode.width) *
                      camera_mode.height;
        uint8_t *image = malloc(size);
        memcpy(image, buffer->data, size);

        mp_io_pipeline_release_buffer(buffer->index);

        MPZBarImage *zbar_image = NULL;
        if (app_mode == MP_APP_MODE_SCAN) {
                zbar_image = mp_zbar_image_new(image,
                                               camera_mode.pixel_format,
                                               camera_mode.width,
                                               camera_mode.height,
                                               camera_rotation,
                                               camera->mirrored);
                mp_zbar_pipeline_process_image(mp_zbar_image_ref(zbar_image));
        }

#ifdef PROFILE_PROCESS
        clock_t t2 = clock();
#endif

        GdkTexture *thumb = process_image_for_preview(image);

        if (app_mode == MP_APP_MODE_PICTURE && captures_remaining > 0) {
                int count = burst_length - captures_remaining;
                --captures_remaining;

                process_image_for_capture(image, count);

                if (captures_remaining == 0) {
                        assert(thumb);
                        process_capture_burst(thumb);
                } else {
                        assert(!thumb);
                }
        } else {
                assert(!thumb);
        }

        if (app_mode == MP_APP_MODE_SCAN) {
                mp_zbar_image_unref(zbar_image);
        } else if (app_mode == MP_APP_MODE_VIDEO && captures_remaining) {
                mp_av_pipeline_add_frame(image);
        } else {
                free(image);
        }

        ++frames_processed;
        if (captures_remaining == 0) {
                is_capturing = false;
        }

#ifdef PROFILE_PROCESS
        clock_t t3 = clock();
        printf("process_image %fms, step 1:%fms, step 2:%fms\n",
               (float)(t3 - t1) / CLOCKS_PER_SEC * 1000,
               (float)(t2 - t1) / CLOCKS_PER_SEC * 1000,
               (float)(t3 - t2) / CLOCKS_PER_SEC * 1000);
#endif
}

void
mp_process_pipeline_process_image(MPBuffer buffer)
{
        // If we haven't processed the previous frame yet, drop this one
        if (frames_received != frames_processed && !is_capturing) {
                mp_io_pipeline_release_buffer(buffer.index);
                return;
        }

        ++frames_received;

        mp_pipeline_invoke(pipeline,
                           (MPPipelineCallback)process_image,
                           &buffer,
                           sizeof(MPBuffer));
}

static void
start_capture()
{
        char template[] = "/tmp/megapixels.XXXXXX";
        char *tempdir;
        tempdir = mkdtemp(template);

        if (tempdir == NULL) {
                g_printerr("Could not make capture directory %s\n", template);
                exit(EXIT_FAILURE);
        }

        strcpy(burst_dir, tempdir);

        captures_remaining = burst_length;
}

static void
start_recording()
{
        time_t rawtime;
        time(&rawtime);
        struct tm tim = *(localtime(&rawtime));

        char fname[37];
        strftime(fname, 37, "VID%Y%m%d%H%M%S.mkv", &tim);

        if (g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS) != NULL) {
                sprintf(capture_fname,
                        "%s/%s",
                        g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS),
                        fname);
        } else if (getenv("XDG_VIDEOS_DIR") != NULL) {
                sprintf(capture_fname,
                        "%s/%s",
                        getenv("XDG_VIDEOS_DIR"),
                        fname);
        } else {
                sprintf(capture_fname,
                        "%s/Videos/%s",
                        getenv("HOME"),
                        fname);
        }

        uint32_t stride = mp_pixel_format_width_to_bytes(camera_mode.pixel_format, camera_mode.width);

        mp_av_pipeline_record(capture_fname, camera_mode.pixel_format, camera_mode.width, camera_mode.height, stride, camera_mode.frame_interval);

        // Use captures remaining to keep track of whether video is being
        // recorded
        captures_remaining = 1;
}

static void
stop_recording()
{
        mp_av_pipeline_finish();
        captures_remaining = 0;
}

static void
capture()
{
        if (app_mode == MP_APP_MODE_PICTURE) {
                start_capture();
        } else if (app_mode == MP_APP_MODE_VIDEO) {
                if (captures_remaining) {
                        stop_recording();
                } else {
                        start_recording();
                }
        } else {
                assert(false);
        }
}

void
mp_process_pipeline_capture()
{
        is_capturing = true;

        mp_pipeline_invoke(pipeline, capture, NULL, 0);
}

static void
on_output_changed(bool format_changed)
{
        output_buffer_width = camera_mode.width / 2;
        output_buffer_height = camera_mode.height / 2;

        if (camera->rotate != 0 || camera->rotate != 180) {
                int tmp = output_buffer_width;
                output_buffer_width = output_buffer_height;
                output_buffer_height = tmp;
        }

        EGLContext context = eglGetCurrentContext();
        assert(context != EGL_NO_CONTEXT);

        EGLDisplay display = eglGetCurrentDisplay();
        assert(display != EGL_NO_DISPLAY);

        for (size_t i = 0; i < NUM_BUFFERS; ++i) {
                MPProcessPipelineBuffer *buf = &output_buffers[i];

                if (buf->data != MAP_FAILED) {
                        munmap(buf->data, buf->size);
                }

                if (buf->fd != -1) {
                        close(buf->fd);
                }

                if (buf->egl_image != EGL_NO_IMAGE) {
                        eglDestroyImage(display, buf->egl_image);
                }

                glBindTexture(GL_TEXTURE_2D, buf->texture_id);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, output_buffer_width, output_buffer_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

                buf->egl_image = eglCreateImage(display, context, EGL_GL_TEXTURE_2D, (EGLClientBuffer) (size_t) buf->texture_id, NULL);
                assert(buf->egl_image != EGL_NO_IMAGE);

                int fourcc;
                int num_planes;
                EGLuint64KHR modifiers;
                EGLBoolean succeeded = eglExportDMABUFImageQueryMESA(display, buf->egl_image, &fourcc, &num_planes, &modifiers);
                assert(succeeded);
                assert(fourcc == DRM_FORMAT_ABGR8888);
                assert(num_planes == 1);

                succeeded = eglExportDMABUFImageMESA(display, buf->egl_image, &buf->fd, &buf->stride, &buf->offset);
                assert(succeeded);
                assert(buf->offset == 0);
                assert((buf->stride / 4) % 16 == 0);

                buf->size = output_buffer_height * buf->stride + buf->offset;
                buf->data = mmap(0, buf->size, PROT_READ, MAP_SHARED, buf->fd, 0);
                assert(buf->data != MAP_FAILED);
        }

        glBindTexture(GL_TEXTURE_2D, 0);

        gles2_debayer_configure(
                gles2_debayer,
                output_buffer_width, output_buffer_height,
                camera_mode.width, camera_mode.height,
                camera->rotate, camera->mirrored,
                camera->previewmatrix[0] == 0 ? NULL : camera->previewmatrix,
                camera->blacklevel);
}

static int
mod(int a, int b)
{
        int r = a % b;
        return r < 0 ? r + b : r;
}

static void
update_state(MPPipeline *pipeline, const struct mp_process_pipeline_state *state)
{
        const bool output_changed =
                !mp_camera_mode_is_equivalent(&camera_mode, &state->mode) ||
                preview_width != state->preview_width ||
                preview_height != state->preview_height ||
                device_rotation != state->device_rotation;

        const bool format_changed =
                camera_mode.pixel_format != state->mode.pixel_format;

        camera = state->camera;
        camera_mode = state->mode;

        preview_width = state->preview_width;
        preview_height = state->preview_height;

        device_rotation = state->device_rotation;

        burst_length = state->burst_length;
        save_dng = state->save_dng;

        // gain_is_manual = state->gain_is_manual;
        gain = state->gain;
        gain_max = state->gain_max;

        exposure_is_manual = state->exposure_is_manual;
        exposure = state->exposure;

        if (output_changed) {
                camera_rotation = mod(camera->rotate - device_rotation, 360);

                on_output_changed(format_changed);
        }

        if (app_mode != state->app_mode) {
                assert(captures_remaining == 0);

                app_mode = state->app_mode;
        }

        struct mp_main_state main_state = {
                .camera = camera,
                .mode = camera_mode,
                .image_width = output_buffer_width,
                .image_height = output_buffer_height,
                .gain_is_manual = state->gain_is_manual,
                .gain = gain,
                .gain_max = gain_max,
                .exposure_is_manual = exposure_is_manual,
                .exposure = exposure,
                .has_auto_focus_continuous = state->has_auto_focus_continuous,
                .has_auto_focus_start = state->has_auto_focus_start,
        };
        mp_main_update_state(&main_state);
}

void
mp_process_pipeline_update_state(const struct mp_process_pipeline_state *new_state)
{
        mp_pipeline_invoke(pipeline,
                           (MPPipelineCallback)update_state,
                           new_state,
                           sizeof(struct mp_process_pipeline_state));
}

// GTK4 seems to require this
void
pango_fc_font_get_languages()
{
}
