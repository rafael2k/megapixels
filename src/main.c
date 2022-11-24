#include "main.h"

#include "camera_config.h"
#include "device.h"
#include "flash.h"
#include "gl_util.h"
#include "io_pipeline.h"
#include "process_pipeline.h"
#include <asm/errno.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#define LIBFEEDBACK_USE_UNSTABLE_API
#include <libfeedback.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>
#endif
#ifdef GDK_WINDOWING_X11
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <gdk/x11/gdkx.h>
#endif
#include <limits.h>
#include <linux/kdev_t.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <locale.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <wordexp.h>
#include <zbar.h>

// #define RENDERDOC

#ifdef RENDERDOC
#include <dlfcn.h>
#include <renderdoc/app.h>
RENDERDOC_API_1_1_2 *rdoc_api = NULL;
#endif

#define APP_ID "org.postmarketos.Megapixels"

enum user_control { USER_CONTROL_ISO, USER_CONTROL_SHUTTER };

static bool camera_is_initialized = false;
static MPDeviceList *devices = NULL;
static MPDeviceList *device = NULL;
static MPMode mode;

static int preview_width = -1;
static int preview_height = -1;

static int device_rotation = 0;

static bool gain_is_manual = false;
static int gain;
static int gain_max;

static bool exposure_is_manual = false;
static int exposure;

static bool has_auto_focus_continuous;
static bool has_auto_focus_start;

static bool flash_enabled = false;

static MPProcessPipelineBuffer *current_preview_buffer = NULL;
static int preview_buffer_width = -1;
static int preview_buffer_height = -1;

static char last_path[260] = "";

static MPZBarScanResult *zbar_result = NULL;

static int burst_length = 0;

// Widgets
GtkWidget *preview;
GtkWidget *main_stack;
GtkWidget *open_last_stack;
GtkWidget *thumb_last;
GtkWidget *process_spinner;
GtkWidget *scanned_codes;
GtkWidget *preview_top_box;
GtkWidget *preview_bottom_box;
GtkWidget *flash_button;
LfbEvent *capture_event;

GSettings *settings;
GSettings *fb_settings;

int
remap(int value, int input_min, int input_max, int output_min, int output_max)
{
        const long long factor = 1000000000;
        long long output_spread = output_max - output_min;
        long long input_spread = input_max - input_min;

        long long zero_value = value - input_min;
        zero_value *= factor;
        long long percentage = zero_value / input_spread;

        long long zero_output = percentage * output_spread / factor;

        long long result = output_min + zero_output;
        return (int)result;
}

static void
update_io_pipeline()
{
        struct mp_io_pipeline_state io_state = {
                .device = device->device,
                .burst_length = burst_length,
                .preview_width = preview_width,
                .preview_height = preview_height,
                .device_rotation = device_rotation,
                .gain_is_manual = gain_is_manual,
                .gain = gain,
                .exposure_is_manual = exposure_is_manual,
                .exposure = exposure,
                .flash_enabled = flash_enabled,
        };
        mp_io_pipeline_update_state(&io_state);

        // Make the right settings available for the camera
        gtk_widget_set_visible(flash_button, device->device->has_flash);
}

static bool
update_state(const struct mp_main_state *state)
{
        if (!camera_is_initialized) {
                camera_is_initialized = true;
        }

        if (device->device == state->device) {
                mode = state->mode;

                if (!gain_is_manual) {
                        gain = state->gain;
                }
                gain_max = state->gain_max;

                if (!exposure_is_manual) {
                        exposure = state->exposure;
                }

                has_auto_focus_continuous = state->has_auto_focus_continuous;
                has_auto_focus_start = state->has_auto_focus_start;
        }

        preview_buffer_width = state->image_width;
        preview_buffer_height = state->image_height;

        return false;
}

void
mp_main_update_state(const struct mp_main_state *state)
{
        struct mp_main_state *state_copy = malloc(sizeof(struct mp_main_state));
        *state_copy = *state;

        g_main_context_invoke_full(g_main_context_default(),
                                   G_PRIORITY_DEFAULT_IDLE,
                                   (GSourceFunc)update_state,
                                   state_copy,
                                   free);
}

static bool
set_zbar_result(MPZBarScanResult *result)
{
        if (zbar_result) {
                for (uint8_t i = 0; i < zbar_result->size; ++i) {
                        free(zbar_result->codes[i].data);
                }

                free(zbar_result);
        }

        zbar_result = result;
        gtk_widget_queue_draw(preview);

        return false;
}

void
mp_main_set_zbar_result(MPZBarScanResult *result)
{
        g_main_context_invoke_full(g_main_context_default(),
                                   G_PRIORITY_DEFAULT_IDLE,
                                   (GSourceFunc)set_zbar_result,
                                   result,
                                   NULL);
}

static bool
set_preview(MPProcessPipelineBuffer *buffer)
{
        if (current_preview_buffer) {
                mp_process_pipeline_buffer_unref(current_preview_buffer);
        }
        current_preview_buffer = buffer;
        gtk_widget_queue_draw(preview);
        return false;
}

void
mp_main_set_preview(MPProcessPipelineBuffer *buffer)
{
        g_main_context_invoke_full(g_main_context_default(),
                                   G_PRIORITY_DEFAULT_IDLE,
                                   (GSourceFunc)set_preview,
                                   buffer,
                                   NULL);
}

struct capture_completed_args {
        GdkTexture *thumb;
        char *fname;
};

static bool
capture_completed(struct capture_completed_args *args)
{
        strncpy(last_path, args->fname, 259);

        gtk_image_set_from_paintable(GTK_IMAGE(thumb_last),
                                     GDK_PAINTABLE(args->thumb));

        gtk_spinner_stop(GTK_SPINNER(process_spinner));
        gtk_stack_set_visible_child(GTK_STACK(open_last_stack), thumb_last);

        g_object_unref(args->thumb);
        g_free(args->fname);

        return false;
}

void
mp_main_capture_completed(GdkTexture *thumb, const char *fname)
{
        struct capture_completed_args *args =
                malloc(sizeof(struct capture_completed_args));
        args->thumb = thumb;
        args->fname = g_strdup(fname);
        g_main_context_invoke_full(g_main_context_default(),
                                   G_PRIORITY_DEFAULT_IDLE,
                                   (GSourceFunc)capture_completed,
                                   args,
                                   free);
}

static GLuint blit_program;
static GLuint blit_uniform_transform;
static GLuint blit_uniform_texture;
static GLuint solid_program;
static GLuint solid_uniform_color;
static GLuint quad;

static void
preview_realize(GtkGLArea *area)
{
        gtk_gl_area_make_current(area);

        if (gtk_gl_area_get_error(area) != NULL) {
                return;
        }

        // Make a VAO for OpenGL
        if (!gtk_gl_area_get_use_es(area)) {
                GLuint vao;
                glGenVertexArrays(1, &vao);
                glBindVertexArray(vao);
                check_gl();
        }

        GLuint blit_shaders[] = {
                gl_util_load_shader("/org/postmarketos/Megapixels/blit.vert",
                                    GL_VERTEX_SHADER,
                                    NULL,
                                    0),
                gl_util_load_shader("/org/postmarketos/Megapixels/blit.frag",
                                    GL_FRAGMENT_SHADER,
                                    NULL,
                                    0),
        };

        blit_program = gl_util_link_program(blit_shaders, 2);
        glBindAttribLocation(blit_program, GL_UTIL_VERTEX_ATTRIBUTE, "vert");
        glBindAttribLocation(blit_program, GL_UTIL_TEX_COORD_ATTRIBUTE, "tex_coord");
        check_gl();

        blit_uniform_transform = glGetUniformLocation(blit_program, "transform");
        blit_uniform_texture = glGetUniformLocation(blit_program, "texture");

        GLuint solid_shaders[] = {
                gl_util_load_shader("/org/postmarketos/Megapixels/solid.vert",
                                    GL_VERTEX_SHADER,
                                    NULL,
                                    0),
                gl_util_load_shader("/org/postmarketos/Megapixels/solid.frag",
                                    GL_FRAGMENT_SHADER,
                                    NULL,
                                    0),
        };

        solid_program = gl_util_link_program(solid_shaders, 2);
        glBindAttribLocation(solid_program, GL_UTIL_VERTEX_ATTRIBUTE, "vert");
        check_gl();

        solid_uniform_color = glGetUniformLocation(solid_program, "color");

        quad = gl_util_new_quad();
}

static void
position_preview(float *offset_x, float *offset_y, float *size_x, float *size_y)
{
        int buffer_width, buffer_height;
        if (device_rotation == 0 || device_rotation == 180) {
                buffer_width = preview_buffer_width;
                buffer_height = preview_buffer_height;
        } else {
                buffer_width = preview_buffer_height;
                buffer_height = preview_buffer_width;
        }

        int scale_factor = gtk_widget_get_scale_factor(preview);
        int top_height =
                gtk_widget_get_allocated_height(preview_top_box) * scale_factor;
        int bottom_height =
                gtk_widget_get_allocated_height(preview_bottom_box) * scale_factor;
        int inner_height = preview_height - top_height - bottom_height;

        double scale = MIN(preview_width / (float)buffer_width,
                           preview_height / (float)buffer_height);

        *size_x = scale * buffer_width;
        *size_y = scale * buffer_height;

        *offset_x = (preview_width - *size_x) / 2.0;

        if (*size_y > inner_height) {
                *offset_y = (preview_height - *size_y) / 2.0;
        } else {
                *offset_y = top_height + (inner_height - *size_y) / 2.0;
        }
}

static gboolean
preview_draw(GtkGLArea *area, GdkGLContext *ctx, gpointer data)
{
        if (gtk_gl_area_get_error(area) != NULL) {
                return FALSE;
        }

        if (!camera_is_initialized) {
                return FALSE;
        }

#ifdef RENDERDOC
        if (rdoc_api) {
                rdoc_api->StartFrameCapture(NULL, NULL);
        }
#endif

        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        float offset_x, offset_y, size_x, size_y;
        position_preview(&offset_x, &offset_y, &size_x, &size_y);
        glViewport(offset_x, preview_height - size_y - offset_y, size_x, size_y);

        if (current_preview_buffer) {
                glUseProgram(blit_program);

                GLfloat rotation_list[4] = { 0, -1, 0, 1 };
                int rotation_index = device_rotation / 90;

                GLfloat sin_rot = rotation_list[rotation_index];
                GLfloat cos_rot = rotation_list[(4 + rotation_index - 1) % 4];
                GLfloat matrix[9] = {
                        // clang-format off
                        cos_rot,  sin_rot, 0,
                        -sin_rot, cos_rot, 0,
                        0,              0, 1,
                        // clang-format on
                };
                glUniformMatrix3fv(blit_uniform_transform, 1, GL_FALSE, matrix);
                check_gl();

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D,
                              mp_process_pipeline_buffer_get_texture_id(
                                      current_preview_buffer));
                glUniform1i(blit_uniform_texture, 0);
                check_gl();

                gl_util_bind_quad(quad);
                gl_util_draw_quad(quad);
        }

        if (zbar_result) {
                GLuint buffer;
                if (!gtk_gl_area_get_use_es(area)) {
                        glGenBuffers(1, &buffer);
                        glBindBuffer(GL_ARRAY_BUFFER, buffer);
                        check_gl();
                }

                glUseProgram(solid_program);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                glUniform4f(solid_uniform_color, 1, 0, 0, 0.5);

                for (uint8_t i = 0; i < zbar_result->size; ++i) {
                        MPZBarCode *code = &zbar_result->codes[i];

                        GLfloat vertices[] = {
                                code->bounds_x[0], code->bounds_y[0],
                                code->bounds_x[1], code->bounds_y[1],
                                code->bounds_x[3], code->bounds_y[3],
                                code->bounds_x[2], code->bounds_y[2],
                        };

                        for (int i = 0; i < 4; ++i) {
                                vertices[i * 2] =
                                        2 * vertices[i * 2] / preview_buffer_width -
                                        1.0;
                                vertices[i * 2 + 1] =
                                        1.0 - 2 * vertices[i * 2 + 1] /
                                                      preview_buffer_height;
                        }

                        if (gtk_gl_area_get_use_es(area)) {
                                glVertexAttribPointer(GL_UTIL_VERTEX_ATTRIBUTE,
                                                      2,
                                                      GL_FLOAT,
                                                      0,
                                                      0,
                                                      vertices);
                                check_gl();
                                glEnableVertexAttribArray(GL_UTIL_VERTEX_ATTRIBUTE);
                                check_gl();
                        } else {
                                glBufferData(GL_ARRAY_BUFFER,
                                             sizeof(vertices),
                                             vertices,
                                             GL_STREAM_DRAW);
                                check_gl();

                                glVertexAttribPointer(GL_UTIL_VERTEX_ATTRIBUTE,
                                                      2,
                                                      GL_FLOAT,
                                                      GL_FALSE,
                                                      0,
                                                      0);
                                glEnableVertexAttribArray(GL_UTIL_VERTEX_ATTRIBUTE);
                                check_gl();
                        }

                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        check_gl();
                }

                glDisable(GL_BLEND);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        glFlush();

#ifdef RENDERDOC
        if (rdoc_api) {
                rdoc_api->EndFrameCapture(NULL, NULL);
        }
#endif

        return FALSE;
}

static gboolean
preview_resize(GtkWidget *widget, int width, int height, gpointer data)
{
        if (preview_width != width || preview_height != height) {
                preview_width = width;
                preview_height = height;
                update_io_pipeline();
        }

        return TRUE;
}

void
run_open_last_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
        char uri[275];
        g_autoptr(GError) error = NULL;

        if (strlen(last_path) == 0) {
                return;
        }
        sprintf(uri, "file://%s", last_path);
        if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
                g_printerr("Could not launch image viewer for '%s': %s\n",
                           uri,
                           error->message);
        }
}

void
run_open_photos_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
        char uri[270];
        g_autoptr(GError) error = NULL;
        sprintf(uri, "file://%s", g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));
        if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
                g_printerr("Could not launch image viewer: %s\n", error->message);
        }
}

void
run_capture_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
        gtk_spinner_start(GTK_SPINNER(process_spinner));
        gtk_stack_set_visible_child(GTK_STACK(open_last_stack), process_spinner);
        if (capture_event)
                lfb_event_trigger_feedback_async(capture_event, NULL, NULL, NULL);

        mp_io_pipeline_capture();
}

void
run_about_action(GSimpleAction *action, GVariant *param, GApplication *app)
{
        GtkWindow *parent = gtk_application_get_active_window(GTK_APPLICATION(app));
        gtk_show_about_dialog(parent,
                              "program-name",
                              "Megapixels",
                              "title",
                              "Megapixels",
                              "logo-icon-name",
                              "org.postmarketos.Megapixels",
                              "comments",
                              "The postmarketOS camera application",
                              "website",
                              "https://gitlab.com/postmarketOS/megapixels",
                              "version",
                              VERSION,
                              "license-type",
                              GTK_LICENSE_GPL_3_0_ONLY,
                              NULL);
}

void
run_quit_action(GSimpleAction *action, GVariant *param, GApplication *app)
{
        g_application_quit(app);
}

static bool
check_point_inside_bounds(int x, int y, int *bounds_x, int *bounds_y)
{
        bool right = false, left = false, top = false, bottom = false;

        for (int i = 0; i < 4; ++i) {
                if (x <= bounds_x[i])
                        left = true;
                if (x >= bounds_x[i])
                        right = true;
                if (y <= bounds_y[i])
                        top = true;
                if (y >= bounds_y[i])
                        bottom = true;
        }

        return right && left && top && bottom;
}

static void
on_zbar_dialog_response(GtkDialog *dialog, int response, char *data)
{
        g_autoptr(GError) error = NULL;
        switch (response) {
        case GTK_RESPONSE_YES:
                if (!g_app_info_launch_default_for_uri(data, NULL, &error)) {
                        g_printerr("Could not launch application: %s\n",
                                   error->message);
                }
        case GTK_RESPONSE_ACCEPT: {
                GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(dialog));
                gdk_clipboard_set_text(gdk_display_get_clipboard(display), data);
        }
        case GTK_RESPONSE_CANCEL:
                break;
        default:
                g_printerr("Wrong dialog response: %d\n", response);
        }

        g_free(data);
        gtk_window_destroy(GTK_WINDOW(dialog));
}

static void
on_zbar_code_tapped(GtkWidget *widget, const MPZBarCode *code)
{
        GtkWidget *dialog;
        GtkDialogFlags flags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;
        bool data_is_url =
                g_uri_is_valid(code->data, G_URI_FLAGS_PARSE_RELAXED, NULL);

        char *data = strdup(code->data);

        if (data_is_url) {
                dialog = gtk_message_dialog_new(
                        GTK_WINDOW(gtk_widget_get_root(widget)),
                        flags,
                        GTK_MESSAGE_QUESTION,
                        GTK_BUTTONS_NONE,
                        "Found a URL '%s' encoded in a %s.",
                        code->data,
                        code->type);
                gtk_dialog_add_buttons(
                        GTK_DIALOG(dialog), "_Open URL", GTK_RESPONSE_YES, NULL);
        } else {
                dialog = gtk_message_dialog_new(
                        GTK_WINDOW(gtk_widget_get_root(widget)),
                        flags,
                        GTK_MESSAGE_QUESTION,
                        GTK_BUTTONS_NONE,
                        "Found data encoded in a %s.",
                        code->type);
                gtk_message_dialog_format_secondary_markup(
                        GTK_MESSAGE_DIALOG(dialog), "<small>%s</small>", code->data);
        }
        gtk_dialog_add_buttons(GTK_DIALOG(dialog),
                               "_Copy",
                               GTK_RESPONSE_ACCEPT,
                               "_Cancel",
                               GTK_RESPONSE_CANCEL,
                               NULL);

        g_signal_connect(
                dialog, "response", G_CALLBACK(on_zbar_dialog_response), data);

        gtk_widget_show(GTK_WIDGET(dialog));
}

static void
preview_pressed(GtkGestureClick *gesture, int n_press, double x, double y)
{
        GtkWidget *widget =
                gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
        int scale_factor = gtk_widget_get_scale_factor(widget);

        // Tapped zbar result
        if (zbar_result) {
                // Transform the event coordinates to the image
                float offset_x, offset_y, size_x, size_y;
                position_preview(&offset_x, &offset_y, &size_x, &size_y);

                int zbar_x = (x - offset_x) * scale_factor / size_x *
                             preview_buffer_width;
                int zbar_y = (y - offset_y) * scale_factor / size_y *
                             preview_buffer_height;

                for (uint8_t i = 0; i < zbar_result->size; ++i) {
                        MPZBarCode *code = &zbar_result->codes[i];

                        if (check_point_inside_bounds(zbar_x,
                                                      zbar_y,
                                                      code->bounds_x,
                                                      code->bounds_y)) {
                                on_zbar_code_tapped(widget, code);
                                return;
                        }
                }
        }

        // Tapped preview image itself, try focussing
        if (has_auto_focus_start) {
                mp_io_pipeline_focus();
        }
}

static void
run_camera_switch_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
        if (device->next != NULL) {
                device = device->next;
        } else {
                device = devices;
        }
        update_io_pipeline();
}

static void
run_open_settings_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
        gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "settings");
}

static void
run_close_settings_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
        gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "main");
}

static void
on_controls_scale_changed(GtkAdjustment *adjustment, void (*set_fn)(double))
{
        set_fn(gtk_adjustment_get_value(adjustment));
}

static void
update_value(GtkAdjustment *adjustment, GtkLabel *label)
{
        char buf[12];
        snprintf(buf, 12, "%.0f", gtk_adjustment_get_value(adjustment));
        gtk_label_set_label(label, buf);
}

static void
on_auto_controls_toggled(GtkToggleButton *button, void (*set_auto_fn)(bool))
{
        set_auto_fn(gtk_toggle_button_get_active(button));
}

static void
update_scale(GtkToggleButton *button, GtkScale *scale)
{
        gtk_widget_set_sensitive(GTK_WIDGET(scale),
                                 !gtk_toggle_button_get_active(button));
}

static void
open_controls(GtkWidget *parent,
              const char *title_name,
              double min_value,
              double max_value,
              double current,
              bool auto_enabled,
              void (*set_fn)(double),
              void (*set_auto_fn)(bool))
{
        GtkBuilder *builder = gtk_builder_new_from_resource(
                "/org/postmarketos/Megapixels/controls-popover.ui");
        GtkPopover *popover =
                GTK_POPOVER(gtk_builder_get_object(builder, "controls"));
        GtkScale *scale = GTK_SCALE(gtk_builder_get_object(builder, "scale"));
        GtkLabel *title = GTK_LABEL(gtk_builder_get_object(builder, "title"));
        GtkLabel *value_label =
                GTK_LABEL(gtk_builder_get_object(builder, "value-label"));
        GtkToggleButton *auto_button =
                GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "auto-button"));

        gtk_label_set_label(title, title_name);

        GtkAdjustment *adjustment = gtk_range_get_adjustment(GTK_RANGE(scale));
        gtk_adjustment_set_lower(adjustment, min_value);
        gtk_adjustment_set_upper(adjustment, max_value);
        gtk_adjustment_set_value(adjustment, current);
        update_value(adjustment, value_label);

        gtk_toggle_button_set_active(auto_button, auto_enabled);
        update_scale(auto_button, scale);

        g_signal_connect(adjustment,
                         "value-changed",
                         G_CALLBACK(on_controls_scale_changed),
                         set_fn);
        g_signal_connect(
                adjustment, "value-changed", G_CALLBACK(update_value), value_label);
        g_signal_connect(auto_button,
                         "toggled",
                         G_CALLBACK(on_auto_controls_toggled),
                         set_auto_fn);
        g_signal_connect(auto_button, "toggled", G_CALLBACK(update_scale), scale);

        gtk_widget_set_parent(GTK_WIDGET(popover), parent);
        gtk_popover_popup(popover);
        // g_object_unref(popover);
}

static void
set_gain(double value)
{
        if (gain != (int)value) {
                gain = value;
                update_io_pipeline();
        }
}

static void
set_gain_auto(bool is_auto)
{
        if (gain_is_manual != !is_auto) {
                gain_is_manual = !is_auto;
                update_io_pipeline();
        }
}

static void
open_iso_controls(GtkWidget *button, gpointer user_data)
{
        open_controls(button,
                      "ISO",
                      0,
                      gain_max,
                      gain,
                      !gain_is_manual,
                      set_gain,
                      set_gain_auto);
}

static void
set_shutter(double value)
{
        int new_exposure = (int)(value / 360.0 * device->device->capture_mode.height);
        if (new_exposure != exposure) {
                exposure = new_exposure;
                update_io_pipeline();
        }
}

static void
set_shutter_auto(bool is_auto)
{
        if (exposure_is_manual != !is_auto) {
                exposure_is_manual = !is_auto;
                update_io_pipeline();
        }
}

static void
open_shutter_controls(GtkWidget *button, gpointer user_data)
{
        open_controls(button,
                      "Shutter",
                      1.0,
                      360.0,
                      exposure,
                      !exposure_is_manual,
                      set_shutter,
                      set_shutter_auto);
}

static void
flash_button_clicked(GtkWidget *button, gpointer user_data)
{
        flash_enabled = !flash_enabled;
        update_io_pipeline();

        const char *icon_name =
                flash_enabled ? "flash-enabled-symbolic" : "flash-disabled-symbolic";
        gtk_button_set_icon_name(GTK_BUTTON(button), icon_name);
}

static void
on_realize(GtkWidget *window, gpointer *data)
{
        GtkNative *native = gtk_widget_get_native(window);
        mp_process_pipeline_init_gl(gtk_native_get_surface(native));

        // Get the first camera by default
        device = devices;
        update_io_pipeline();
}

static GSimpleAction *
create_simple_action(GtkApplication *app, const char *name, GCallback callback)
{
        GSimpleAction *action = g_simple_action_new(name, NULL);
        g_signal_connect(action, "activate", callback, app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action));
        return action;
}

static void
update_ui_rotation()
{
        if (device_rotation == 0 || device_rotation == 180) {
                // Portrait
                gtk_widget_set_halign(preview_top_box, GTK_ALIGN_FILL);
                gtk_orientable_set_orientation(GTK_ORIENTABLE(preview_top_box),
                                               GTK_ORIENTATION_VERTICAL);

                gtk_widget_set_halign(preview_bottom_box, GTK_ALIGN_FILL);
                gtk_orientable_set_orientation(GTK_ORIENTABLE(preview_bottom_box),
                                               GTK_ORIENTATION_HORIZONTAL);

                if (device_rotation == 0) {
                        gtk_widget_set_valign(preview_top_box, GTK_ALIGN_START);
                        gtk_widget_set_valign(preview_bottom_box, GTK_ALIGN_END);
                } else {
                        gtk_widget_set_valign(preview_top_box, GTK_ALIGN_END);
                        gtk_widget_set_valign(preview_bottom_box, GTK_ALIGN_START);
                }
        } else {
                // Landscape
                gtk_widget_set_valign(preview_top_box, GTK_ALIGN_FILL);
                gtk_orientable_set_orientation(GTK_ORIENTABLE(preview_top_box),
                                               GTK_ORIENTATION_HORIZONTAL);

                gtk_widget_set_valign(preview_bottom_box, GTK_ALIGN_FILL);
                gtk_orientable_set_orientation(GTK_ORIENTABLE(preview_bottom_box),
                                               GTK_ORIENTATION_VERTICAL);

                if (device_rotation == 90) {
                        gtk_widget_set_halign(preview_top_box, GTK_ALIGN_END);
                        gtk_widget_set_halign(preview_bottom_box, GTK_ALIGN_START);
                } else {
                        gtk_widget_set_halign(preview_top_box, GTK_ALIGN_START);
                        gtk_widget_set_halign(preview_bottom_box, GTK_ALIGN_END);
                }
        }
}

char *
munge_app_id(const char *app_id)
{
        char *id = g_strdup(app_id);
        int i;

        if (g_str_has_suffix(id, ".desktop")) {
                char *c = g_strrstr(id, ".desktop");
                if (c)
                        *c = '\0';
        }

        g_strcanon(id,
                   "0123456789"
                   "abcdefghijklmnopqrstuvwxyz"
                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                   "-",
                   '-');
        for (i = 0; id[i] != '\0'; i++)
                id[i] = g_ascii_tolower(id[i]);

        return id;
}

/* Verbatim from feedbackd */
#define FEEDBACKD_SCHEMA_ID "org.sigxcpu.feedbackd"
#define FEEDBACKD_KEY_PROFILE "profile"
#define FEEDBACKD_APP_SCHEMA FEEDBACKD_SCHEMA_ID ".application"
#define FEEDBACKD_APP_PREFIX "/org/sigxcpu/feedbackd/application/"

static gboolean
fb_profile_to_state(GValue *value, GVariant *variant, gpointer user_data)
{
        const gchar *name;
        gboolean state = FALSE;

        name = g_variant_get_string(variant, NULL);

        if (g_strcmp0(name, "full") == 0)
                state = TRUE;

        g_value_set_boolean(value, state);

        return TRUE;
}

static GVariant *
state_to_fb_profile(const GValue *value,
                    const GVariantType *expected_type,
                    gpointer user_data)
{
        gboolean state = g_value_get_boolean(value);

        return g_variant_new_string(state ? "full" : "silent");
}

static void
setup_fb_switch(GtkBuilder *builder)
{
        g_autofree char *path = NULL;
        g_autofree char *munged_id = NULL;
        g_autoptr(GSettingsSchema) schema = NULL;
        GSettingsSchemaSource *schema_source =
                g_settings_schema_source_get_default();
        GtkWidget *shutter_sound_switch =
                GTK_WIDGET(gtk_builder_get_object(builder, "shutter-sound-switch"));
        GtkWidget *feedback_box =
                GTK_WIDGET(gtk_builder_get_object(builder, "feedback-box"));

        schema = g_settings_schema_source_lookup(
                schema_source, FEEDBACKD_APP_SCHEMA, TRUE);
        if (schema == NULL) {
                gtk_widget_set_sensitive(feedback_box, FALSE);
                return;
        }

        munged_id = munge_app_id(APP_ID);
        path = g_strconcat(FEEDBACKD_APP_PREFIX, munged_id, "/", NULL);
        fb_settings = g_settings_new_with_path(FEEDBACKD_APP_SCHEMA, path);
        g_settings_bind_with_mapping(fb_settings,
                                     FEEDBACKD_KEY_PROFILE,
                                     shutter_sound_switch,
                                     "active",
                                     G_SETTINGS_BIND_DEFAULT,
                                     fb_profile_to_state,
                                     state_to_fb_profile,
                                     NULL,
                                     NULL);
}

#ifdef GDK_WINDOWING_WAYLAND
static void
wl_handle_geometry(void *data,
                   struct wl_output *wl_output,
                   int32_t x,
                   int32_t y,
                   int32_t physical_width,
                   int32_t physical_height,
                   int32_t subpixel,
                   const char *make,
                   const char *model,
                   int32_t transform)
{
        assert(transform < 4);
        int new_rotation = transform * 90;

        if (new_rotation != device_rotation) {
                device_rotation = new_rotation;
                update_io_pipeline();
                update_ui_rotation();
        }
}

static void
wl_handle_mode(void *data,
               struct wl_output *wl_output,
               uint32_t flags,
               int32_t width,
               int32_t height,
               int32_t refresh)
{
        // Do nothing
}

static const struct wl_output_listener output_listener = {
        .geometry = wl_handle_geometry,
        .mode = wl_handle_mode
};

static void
wl_handle_global(void *data,
                 struct wl_registry *wl_registry,
                 uint32_t name,
                 const char *interface,
                 uint32_t version)
{
        if (strcmp(interface, wl_output_interface.name) == 0) {
                struct wl_output *output =
                        wl_registry_bind(wl_registry, name, &wl_output_interface, 1);
                wl_output_add_listener(output, &output_listener, NULL);
        }
}

static void
wl_handle_global_remove(void *data, struct wl_registry *wl_registry, uint32_t name)
{
        // Do nothing
}

static const struct wl_registry_listener registry_listener = {
        .global = wl_handle_global,
        .global_remove = wl_handle_global_remove
};
#endif // GDK_WINDOWING_WAYLAND

#ifdef GDK_WINDOWING_X11
static gboolean
xevent_handler(GdkDisplay *display, XEvent *xevent, gpointer data)
{
        Display *xdisplay = gdk_x11_display_get_xdisplay(display);
        int event_base, error_base;
        XRRQueryExtension(xdisplay, &event_base, &error_base);
        if (xevent->type - event_base == RRScreenChangeNotify) {
                Rotation xrotation =
                        ((XRRScreenChangeNotifyEvent *)xevent)->rotation;
                int new_rotation = 0;
                switch (xrotation) {
                case RR_Rotate_0:
                        new_rotation = 0;
                        break;
                case RR_Rotate_90:
                        new_rotation = 90;
                        break;
                case RR_Rotate_180:
                        new_rotation = 180;
                        break;
                case RR_Rotate_270:
                        new_rotation = 270;
                        break;
                }
                if (new_rotation != device_rotation) {
                        device_rotation = new_rotation;
                        update_io_pipeline();
                        update_ui_rotation();
                }
        }

        // The return value of this function should always be FALSE; if it's
        // TRUE, we prevent GTK/GDK from handling the event.
        return FALSE;
}
#endif // GDK_WINDOWING_X11

static void
activate(GtkApplication *app, gpointer data)
{
        g_object_set(gtk_settings_get_default(),
                     "gtk-application-prefer-dark-theme",
                     TRUE,
                     NULL);
        GdkDisplay *display = gdk_display_get_default();
        GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(display);
        gtk_icon_theme_add_resource_path(icon_theme, "/org/postmarketos/Megapixels");

        GtkCssProvider *provider = gtk_css_provider_new();
        gtk_css_provider_load_from_resource(
                provider, "/org/postmarketos/Megapixels/camera.css");
        gtk_style_context_add_provider_for_display(
                display,
                GTK_STYLE_PROVIDER(provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

        GtkBuilder *builder = gtk_builder_new_from_resource(
                "/org/postmarketos/Megapixels/camera.ui");

        GtkWidget *window = GTK_WIDGET(gtk_builder_get_object(builder, "window"));
        GtkWidget *iso_button =
                GTK_WIDGET(gtk_builder_get_object(builder, "iso-controls-button"));
        GtkWidget *shutter_button = GTK_WIDGET(
                gtk_builder_get_object(builder, "shutter-controls-button"));
        flash_button =
                GTK_WIDGET(gtk_builder_get_object(builder, "flash-controls-button"));
        GtkWidget *setting_dng_button =
                GTK_WIDGET(gtk_builder_get_object(builder, "setting-raw"));
        GtkWidget *setting_postprocessor_combo =
                GTK_WIDGET(gtk_builder_get_object(builder, "setting-processor"));
        GtkListStore *setting_postprocessor_list = GTK_LIST_STORE(
                gtk_builder_get_object(builder, "list-postprocessors"));
        preview = GTK_WIDGET(gtk_builder_get_object(builder, "preview"));
        main_stack = GTK_WIDGET(gtk_builder_get_object(builder, "main_stack"));
        open_last_stack =
                GTK_WIDGET(gtk_builder_get_object(builder, "open_last_stack"));
        thumb_last = GTK_WIDGET(gtk_builder_get_object(builder, "thumb_last"));
        process_spinner =
                GTK_WIDGET(gtk_builder_get_object(builder, "process_spinner"));
        scanned_codes = GTK_WIDGET(gtk_builder_get_object(builder, "scanned-codes"));
        preview_top_box = GTK_WIDGET(gtk_builder_get_object(builder, "top-box"));
        preview_bottom_box =
                GTK_WIDGET(gtk_builder_get_object(builder, "bottom-box"));

        g_signal_connect(window, "realize", G_CALLBACK(on_realize), NULL);

        g_signal_connect(preview, "realize", G_CALLBACK(preview_realize), NULL);
        g_signal_connect(preview, "render", G_CALLBACK(preview_draw), NULL);
        g_signal_connect(preview, "resize", G_CALLBACK(preview_resize), NULL);
        GtkGesture *click = gtk_gesture_click_new();
        g_signal_connect(click, "pressed", G_CALLBACK(preview_pressed), NULL);
        gtk_widget_add_controller(preview, GTK_EVENT_CONTROLLER(click));

        g_signal_connect(iso_button, "clicked", G_CALLBACK(open_iso_controls), NULL);
        g_signal_connect(
                shutter_button, "clicked", G_CALLBACK(open_shutter_controls), NULL);
        g_signal_connect(
                flash_button, "clicked", G_CALLBACK(flash_button_clicked), NULL);

        setup_fb_switch(builder);

        // Setup actions
        create_simple_action(app, "capture", G_CALLBACK(run_capture_action));
        create_simple_action(
                app, "switch-camera", G_CALLBACK(run_camera_switch_action));
        create_simple_action(
                app, "open-settings", G_CALLBACK(run_open_settings_action));
        create_simple_action(
                app, "close-settings", G_CALLBACK(run_close_settings_action));
        create_simple_action(app, "open-last", G_CALLBACK(run_open_last_action));
        create_simple_action(app, "open-photos", G_CALLBACK(run_open_photos_action));
        create_simple_action(app, "about", G_CALLBACK(run_about_action));
        create_simple_action(app, "quit", G_CALLBACK(run_quit_action));

        // Setup shortcuts
        const char *capture_accels[] = { "space", NULL };
        gtk_application_set_accels_for_action(app, "app.capture", capture_accels);

        const char *quit_accels[] = { "<Ctrl>q", "<Ctrl>w", NULL };
        gtk_application_set_accels_for_action(app, "app.quit", quit_accels);

        // Setup settings
        settings = g_settings_new("org.postmarketos.Megapixels");
        char *setting_postproc = g_settings_get_string(settings, "postprocessor");

        // Initialize the postprocessing gsetting to the old processor if
        // it was not set yet
        if (setting_postproc == NULL || setting_postproc[0] == '\0') {
                printf("Initializing postprocessor gsetting\n");
                setting_postproc = malloc(512);
                if (!mp_process_find_processor(setting_postproc)) {
                        printf("No processor found\n");
                        exit(1);
                }
                g_settings_set_string(settings, "postprocessor", setting_postproc);
                printf("Initialized postprocessor to %s\n", setting_postproc);
        }

        // Find all postprocessors for the settings list
        mp_process_find_all_processors(setting_postprocessor_list);

        // Bind settings widgets to the actual settings
        g_settings_bind(settings,
                        "save-raw",
                        setting_dng_button,
                        "active",
                        G_SETTINGS_BIND_DEFAULT);
        g_settings_bind(settings,
                        "postprocessor",
                        setting_postprocessor_combo,
                        "active-id",
                        G_SETTINGS_BIND_DEFAULT);

#ifdef GDK_WINDOWING_WAYLAND
        // Listen for Wayland rotation
        if (GDK_IS_WAYLAND_DISPLAY(display)) {
                struct wl_display *wl_display =
                        gdk_wayland_display_get_wl_display(display);
                struct wl_registry *wl_registry =
                        wl_display_get_registry(wl_display);
                // The registry listener will bind to our wl_output and add our
                // listeners
                wl_registry_add_listener(wl_registry, &registry_listener, NULL);
                // GTK will take care of dispatching wayland events for us.
                // Wayland sends us a geometry event as soon as we bind to the
                // wl_output, so we don't need to manually check the initial
                // rotation here.
        }
#endif
#ifdef GDK_WINDOWING_X11
        // Listen for X rotation
        if (GDK_IS_X11_DISPLAY(display)) {
                g_signal_connect(
                        display, "xevent", G_CALLBACK(xevent_handler), NULL);
                // Set initial rotation
                Display *xdisplay = gdk_x11_display_get_xdisplay(display);
                int screen =
                        XScreenNumberOfScreen(gdk_x11_display_get_xscreen(display));
                Rotation xrotation;
                XRRRotations(xdisplay, screen, &xrotation);
                int new_rotation = 0;
                switch (xrotation) {
                case RR_Rotate_0:
                        new_rotation = 0;
                        break;
                case RR_Rotate_90:
                        new_rotation = 90;
                        break;
                case RR_Rotate_180:
                        new_rotation = 180;
                        break;
                case RR_Rotate_270:
                        new_rotation = 270;
                        break;
                }
                if (new_rotation != device_rotation) {
                        device_rotation = new_rotation;
                        update_ui_rotation();
                }
        }
#endif

        // Initialize display flash
        GDBusConnection *conn =
                g_application_get_dbus_connection(G_APPLICATION(app));
        mp_flash_gtk_init(conn);

        mp_io_pipeline_start(&devices);

        gtk_application_add_window(app, GTK_WINDOW(window));
        gtk_widget_show(window);
}

static void
startup(GApplication *app, gpointer data)
{
        g_autoptr(GError) err = NULL;

        if (lfb_init(APP_ID, &err))
                capture_event = lfb_event_new("camera-shutter");
        else
                g_warning("Failed to init libfeedback: %s", err->message);
}

static void
shutdown(GApplication *app, gpointer data)
{
        // Only do cleanup in development, let the OS clean up otherwise
#ifdef DEBUG
        mp_io_pipeline_stop();
        mp_flash_gtk_clean();

        g_clear_object(&fb_settings);
        g_clear_object(&capture_event);
        lfb_uninit();
#endif
}

int
main(int argc, char *argv[])
{
#ifdef RENDERDOC
        {
                void *mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
                if (mod) {
                        pRENDERDOC_GetAPI RENDERDOC_GetAPI =
                                (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
                        int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_1_2,
                                                   (void **)&rdoc_api);
                        assert(ret == 1);
                } else {
                        printf("Renderdoc not found\n");
                }
        }
#endif

        setenv("LC_NUMERIC", "C", 1);

        // Load config
        devices = mp_load_config();

        GtkApplication *app = gtk_application_new(APP_ID, 0);

        g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
        g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
        g_signal_connect(app, "shutdown", G_CALLBACK(shutdown), NULL);

        g_application_run(G_APPLICATION(app), argc, argv);

        return 0;
}
