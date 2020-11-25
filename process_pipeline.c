#include "process_pipeline.h"

#include "pipeline.h"
#include "main.h"
#include "config.h"
#include "quickpreview.h"
#include <tiffio.h>
#include <assert.h>
#include <math.h>
#include <wordexp.h>
#include <gtk/gtk.h>

#define TIFFTAG_FORWARDMATRIX1 50964

static const float colormatrix_srgb[] = {
    3.2409, -1.5373, -0.4986,
    -0.9692, 1.8759, 0.0415,
    0.0556, -0.2039, 1.0569
};

static MPPipeline *pipeline;

static char burst_dir[23];
static char processing_script[512];

static volatile bool is_capturing = false;
static volatile int frames_processed = 0;
static volatile int frames_received = 0;

static const struct mp_camera_config *camera;

static MPCameraMode mode;

static int burst_length;
static int captures_remaining = 0;

static int preview_width;
static int preview_height;

// static bool gain_is_manual;
static int gain;
static int gain_max;

static bool exposure_is_manual;
static int exposure;

static char capture_fname[255];

// static void
// process_image(const int *p, int size)
// {
//     time_t rawtime;
//     char datetime[20] = {0};
//     struct tm tim;
//     uint8_t *pixels;
//     char fname[255];
//     char fname_target[255];
//     char command[1024];
//     char timestamp[30];
//     char uniquecameramodel[255];
//     GdkPixbuf *pixbuf;
//     GdkPixbuf *pixbufrot;
//     GdkPixbuf *thumb;
//     GError *error = NULL;
//     double scale;
//     cairo_t *cr;
//     TIFF *tif;
//     int skip = 2;
//     long sub_offset = 0;
//     uint64 exif_offset = 0;
//     static const short cfapatterndim[] = {2, 2};
//     static const float neutral[] = {1.0, 1.0, 1.0};
//     static uint16_t isospeed[] = {0};

//     // Only process preview frames when not capturing
//     if (capture == 0) {

//     } else {


//         if (capture == 0) {


//             // Restore the auto exposure and gain if needed
//             // if (auto_exposure) {
//             //     v4l2_ctrl_set(current.fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_AUTO);
//             // }
//             // if (auto_gain) {
//             //     v4l2_ctrl_set(current.fd, V4L2_CID_AUTOGAIN, 1);
//             // }
//         }
//     }
// }

static void
register_custom_tiff_tags(TIFF *tif)
{
    static const TIFFFieldInfo custom_fields[] = {
        {TIFFTAG_FORWARDMATRIX1, -1, -1, TIFF_SRATIONAL, FIELD_CUSTOM, 1, 1, "ForwardMatrix1"},
    };

    // Add missing dng fields
    TIFFMergeFieldInfo(tif, custom_fields, sizeof(custom_fields) / sizeof(custom_fields[0]));
}

static bool
find_processor(char *script)
{
    char *xdg_config_home;
    char filename[] = "postprocess.sh";
    wordexp_t exp_result;

    // Resolve XDG stuff
    if ((xdg_config_home = getenv("XDG_CONFIG_HOME")) == NULL) {
        xdg_config_home = "~/.config";
    }
    wordexp(xdg_config_home, &exp_result, 0);
    xdg_config_home = strdup(exp_result.we_wordv[0]);
    wordfree(&exp_result);

    // Check postprocess.h in the current working directory
    sprintf(script, "%s", filename);
    if(access(script, F_OK) != -1) {
        sprintf(script, "./%s", filename);
        printf("Found postprocessor script at %s\n", script);
        return true;
    }

    // Check for a script in XDG_CONFIG_HOME
    sprintf(script, "%s/megapixels/%s", xdg_config_home, filename);
    if(access(script, F_OK) != -1) {
        printf("Found postprocessor script at %s\n", script);
        return true;
    }

    // Check user overridden /etc/megapixels/postprocessor.sh
    sprintf(script, "%s/megapixels/%s", SYSCONFDIR, filename);
    if(access(script, F_OK) != -1) {
        printf("Found postprocessor script at %s\n", script);
        return true;
    }

    // Check packaged /usr/share/megapixels/postprocessor.sh
    sprintf(script, "%s/megapixels/%s", DATADIR, filename);
    if(access(script, F_OK) != -1) {
        printf("Found postprocessor script at %s\n", script);
        return true;
    }

    return false;
}

static void setup(MPPipeline *pipeline, const void *data)
{
    TIFFSetTagExtender(register_custom_tiff_tags);

    if (!find_processor(processing_script)) {
        g_printerr("Could not find any post-process script\n");
        exit(1);
    }
}

void mp_process_pipeline_start()
{
    pipeline = mp_pipeline_new();

    mp_pipeline_invoke(pipeline, setup, NULL, 0);
}

void mp_process_pipeline_stop()
{
    mp_pipeline_free(pipeline);
}

static void
process_image_for_preview(const MPImage *image)
{
    uint32_t surface_width, surface_height, skip;
    quick_preview_size(
        &surface_width,
        &surface_height,
        &skip,
        preview_width,
        preview_height,
        image->width,
        image->height,
        image->pixel_format,
        camera->rotate);

    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_RGB24,
        surface_width,
        surface_height);

    uint8_t *pixels = cairo_image_surface_get_data(surface);

    quick_preview(
        (uint32_t *)pixels,
        surface_width,
        surface_height,
        image->data,
        image->width,
        image->height,
        image->pixel_format,
        camera->rotate,
        camera->mirrored,
        camera->colormatrix[0] == 0 ? NULL : camera->colormatrix,
        camera->blacklevel,
        skip);

    mp_main_set_preview(surface);
}

static void
process_image_for_capture(const MPImage *image, int count)
{
    time_t rawtime;
    time(&rawtime);
    struct tm tim = *(localtime(&rawtime));

    char datetime[20] = {0};
    strftime(datetime, 20, "%Y:%m:%d %H:%M:%S", &tim);

    char fname[255];
    sprintf(fname, "%s/%d.dng", burst_dir, count);

    TIFF *tif = TIFFOpen(fname, "w");
    if(!tif) {
        printf("Could not open tiff\n");
    }

    // Define TIFF thumbnail
    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 1);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, image->width >> 4);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, image->height >> 4);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_MAKE, mp_get_device_make());
    TIFFSetField(tif, TIFFTAG_MODEL, mp_get_device_model());
    uint16_t orientation;
    if (camera->rotate == 0) {
        orientation = camera->mirrored ? ORIENTATION_TOPRIGHT : ORIENTATION_TOPLEFT;
    } else if (camera->rotate == 90) {
        orientation = camera->mirrored ? ORIENTATION_RIGHTBOT : ORIENTATION_LEFTBOT;
    } else if (camera->rotate == 180) {
        orientation = camera->mirrored ? ORIENTATION_BOTLEFT : ORIENTATION_BOTRIGHT;
    } else {
        orientation = camera->mirrored ? ORIENTATION_LEFTTOP : ORIENTATION_RIGHTTOP;
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
    sprintf(uniquecameramodel, "%s %s", mp_get_device_make(), mp_get_device_model());
    TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, uniquecameramodel);
    if(camera->colormatrix[0]) {
        TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, camera->colormatrix);
    } else {
        TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, colormatrix_srgb);
    }
    if(camera->forwardmatrix[0]) {
        TIFFSetField(tif, TIFFTAG_FORWARDMATRIX1, 9, camera->forwardmatrix);
    }
    static const float neutral[] = {1.0, 1.0, 1.0};
    TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
    TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, 21);
    // Write black thumbnail, only windows uses this
    {
        unsigned char *buf = (unsigned char *)calloc(1, (int)image->width >> 4);
        for (int row = 0; row < image->height>>4; row++) {
            TIFFWriteScanline(tif, buf, row, 0);
        }
        free(buf);
    }
    TIFFWriteDirectory(tif);

    // Define main photo
    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, image->width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, image->height);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    static const short cfapatterndim[] = {2, 2};
    TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, cfapatterndim);
    TIFFSetField(tif, TIFFTAG_CFAPATTERN, "\002\001\001\000"); // BGGR
    if(camera->whitelevel) {
        TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &camera->whitelevel);
    }
    if(camera->blacklevel) {
        TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 1, &camera->blacklevel);
    }
    TIFFCheckpointDirectory(tif);
    printf("Writing frame to %s\n", fname);

    unsigned char *pLine = (unsigned char*)malloc(image->width);
    for(int row = 0; row < image->height; row++){
        TIFFWriteScanline(tif, image->data + (row * image->width), row, 0);
    }
    free(pLine);
    TIFFWriteDirectory(tif);

    // Add an EXIF block to the tiff
    TIFFCreateEXIFDirectory(tif);
    // 1 = manual, 2 = full auto, 3 = aperture priority, 4 = shutter priority
    if (!exposure_is_manual) {
        TIFFSetField(tif, EXIFTAG_EXPOSUREPROGRAM, 2);
    } else {
        TIFFSetField(tif, EXIFTAG_EXPOSUREPROGRAM, 1);
    }

    TIFFSetField(tif, EXIFTAG_EXPOSURETIME, (mode.frame_interval.numerator / (float)mode.frame_interval.denominator) / ((float)image->height / (float)exposure));
    uint16_t isospeed[1];
    isospeed[0] = (uint16_t)remap(gain - 1, 0, gain_max, camera->iso_min, camera->iso_max);
    TIFFSetField(tif, EXIFTAG_ISOSPEEDRATINGS, 1, isospeed);
    TIFFSetField(tif, EXIFTAG_FLASH, 0);

    TIFFSetField(tif, EXIFTAG_DATETIMEORIGINAL, datetime);
    TIFFSetField(tif, EXIFTAG_DATETIMEDIGITIZED, datetime);
    if(camera->fnumber) {
        TIFFSetField(tif, EXIFTAG_FNUMBER, camera->fnumber);
    }
    if(camera->focallength) {
        TIFFSetField(tif, EXIFTAG_FOCALLENGTH, camera->focallength);
    }
    if(camera->focallength && camera->cropfactor) {
        TIFFSetField(tif, EXIFTAG_FOCALLENGTHIN35MMFILM, (short)(camera->focallength * camera->cropfactor));
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
process_capture_burst()
{
    time_t rawtime;
    time(&rawtime);
    struct tm tim = *(localtime(&rawtime));

    char timestamp[30];
    strftime(timestamp, 30, "%Y%m%d%H%M%S", &tim);

    sprintf(capture_fname, "%s/Pictures/IMG%s", getenv("HOME"), timestamp);

    // Start post-processing the captured burst
    g_print("Post process %s to %s.ext\n", burst_dir, capture_fname);
    char command[1024];
    sprintf(command, "%s %s %s &", processing_script, burst_dir, capture_fname);
    system(command);
}

static void
process_image(MPPipeline *pipeline, const MPImage *image)
{
    assert(image->width == mode.width && image->height == mode.height);

    process_image_for_preview(image);

    if (captures_remaining > 0) {
        int count = burst_length - captures_remaining;
        --captures_remaining;

        process_image_for_capture(image, count);

        if (captures_remaining == 0) {
            process_capture_burst();

            mp_main_capture_completed(capture_fname);
        }
    }

    free(image->data);

    ++frames_processed;
    if (captures_remaining == 0) {
        is_capturing = false;
    }
}

void mp_process_pipeline_process_image(MPImage image)
{
    // If we haven't processed the previous frame yet, drop this one
    if (frames_received != frames_processed && !is_capturing) {
        printf("Dropped frame at capture %d %d\n", frames_received, frames_processed);
        return;
    }

    ++frames_received;

    mp_pipeline_invoke(pipeline, (MPPipelineCallback)process_image, &image, sizeof(MPImage));
}

static void capture()
{
    char template[] = "/tmp/megapixels.XXXXXX";
    char *tempdir;
    tempdir = mkdtemp(template);

    if (tempdir == NULL) {
        g_printerr("Could not make capture directory %s\n", template);
        exit (EXIT_FAILURE);
    }

    strcpy(burst_dir, tempdir);

    captures_remaining = burst_length;
}

void mp_process_pipeline_capture()
{
    is_capturing = true;

    mp_pipeline_invoke(pipeline, capture, NULL, 0);
}

static void
update_state(MPPipeline *pipeline, const struct mp_process_pipeline_state *state)
{
    camera = state->camera;
    mode = state->mode;

    burst_length = state->burst_length;

    preview_width = state->preview_width;
    preview_height = state->preview_height;

    // gain_is_manual = state->gain_is_manual;
    gain = state->gain;
    gain_max = state->gain_max;

    exposure_is_manual = state->exposure_is_manual;
    exposure = state->exposure;

    struct mp_main_state main_state = {
        .camera = camera,
        .mode = mode,
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

void mp_process_pipeline_update_state(const struct mp_process_pipeline_state *new_state)
{
    mp_pipeline_invoke(pipeline, (MPPipelineCallback)update_state, new_state, sizeof(struct mp_process_pipeline_state));
}
