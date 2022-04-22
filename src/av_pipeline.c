#include "av_pipeline.h"

#include "pipeline.h"
#include "io_pipeline.h"
#include <gst/gst.h>
#include <gst/video/gstvideoutils.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>

static MPPipeline *pipeline;

static GstElement *record_pipeline, *appsrc, *videoconvert, *x264enc, *matroskamux, *filesink;

static uint32_t pending_frames;
static uint32_t stride;
static uint32_t width, height;

static void
setup(MPPipeline *pipeline, const void *data)
{
	gst_init(NULL, NULL);

	appsrc = gst_element_factory_make("appsrc", "video_src");
	videoconvert = gst_element_factory_make("videoconvert", "video_converter");
	x264enc = gst_element_factory_make("x264enc", "video_encoding");
	matroskamux = gst_element_factory_make("matroskamux", "matroskamux");
	filesink = gst_element_factory_make("filesink", "file_sink");

	record_pipeline = gst_pipeline_new("video-recording");

	assert(appsrc && videoconvert && x264enc && matroskamux && filesink
		&& record_pipeline);

	g_object_set(x264enc,
		"speed-preset", 1 /*ultrafast*/,
		NULL);

	gst_bin_add_many(GST_BIN(record_pipeline), appsrc, videoconvert, x264enc, matroskamux, filesink, NULL);
	assert(gst_element_link_many(videoconvert, x264enc, matroskamux, filesink, NULL));
}

void
mp_av_pipeline_start()
{
	pipeline = mp_pipeline_new();

	mp_pipeline_invoke(pipeline, setup, NULL, 0);
}

void
mp_av_pipeline_stop()
{
	mp_pipeline_free(pipeline);
}

struct record_args {
	char *file;
	MPPixelFormat pixel_format;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	struct v4l2_fract interval;
};

static void
record(MPPipeline *pipeline, const struct record_args *args)
{
	assert(args->pixel_format == MP_PIXEL_FMT_RGB565);

	width = args->width;
	height = args->height;
	stride = args->stride;
	size_t size = stride * height;

	g_object_set(filesink, "location", args->file, NULL);

	GstVideoInfo video_info;
	gst_video_info_set_format(&video_info, GST_VIDEO_FORMAT_RGB16, width, height);
	video_info.fps_n = args->interval.numerator;
	video_info.fps_d = args->interval.denominator;
	GstCaps *video_caps = gst_video_info_to_caps(&video_info);
	g_object_set(appsrc,
		"caps", video_caps,
		"block", true,
		"max-bytes", size,
		"format", GST_FORMAT_TIME,
		NULL);
	gst_caps_unref(video_caps);

	assert(gst_element_link_many(appsrc, videoconvert, NULL));

	gst_element_set_state (record_pipeline, GST_STATE_PLAYING);
}

void
mp_av_pipeline_record(const char *file, MPPixelFormat pixel_format, uint32_t width, uint32_t height, uint32_t stride, struct v4l2_fract interval)
{
	struct record_args args = {
		.file = strdup(file),
		.pixel_format = pixel_format,
		.width = width,
		.height = height,
		.stride = stride,
		.interval = interval,
	};
	mp_pipeline_invoke(pipeline, (MPPipelineCallback)record, &args, sizeof(struct record_args));
}

struct frame
{
	uint8_t * image;
	uint64_t pts;
};

static void
add_frame(MPPipeline *pipeline, const struct frame * frame)
{
	// Wrap the data for gst
	GstBuffer * buf = gst_buffer_new_wrapped_full(
		GST_MEMORY_FLAG_READONLY,
		frame->image,
		stride * height,
		0,
		stride * height,
		frame->image,
		(GDestroyNotify) free);

	buf->pts = frame->pts;

	GstFlowReturn ret;
	g_signal_emit_by_name(appsrc, "push-buffer", buf, &ret);
	assert(ret == GST_FLOW_OK);

	gst_buffer_unref(buf);
}

void
mp_av_pipeline_add_frame(uint8_t *image)
{
	// Set a timestamp. It would be best to get this from the camera but
	// this works for now.
	struct timespec tms;
	clock_gettime(CLOCK_REALTIME, &tms);

	struct frame f = {
		.image = image,
		.pts = tms.tv_sec * 1000000000 + tms.tv_nsec,
	};
	mp_pipeline_invoke(pipeline, (MPPipelineCallback)add_frame, &f, sizeof(struct frame));
}

static void
finish(MPPipeline *pipeline, void *data)
{
	GstFlowReturn ret;
	g_signal_emit_by_name(appsrc, "end-of-stream", &ret);
	assert(ret == GST_FLOW_OK);
}

void
mp_av_pipeline_finish()
{
	mp_pipeline_invoke(pipeline, (MPPipelineCallback)finish, NULL, 0);
}

