#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-client.h>
#include "background-image.h"
#include "cairo_util.h"
#include "log.h"
#include "pool-buffer.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "lbm.h"

/*
 * If `color` is a hexadecimal string of the form 'rrggbb' or '#rrggbb',
 * `*result` will be set to the uint32_t version of the color. Otherwise,
 * return false and leave `*result` unmodified.
 */
static bool parse_color(const char *color, uint32_t *result) {
	if (color[0] == '#') {
		++color;
	}

	int len = strlen(color);
	if (len != 6) {
		return false;
	}
	for (int i = 0; i < len; ++i) {
		if (!isxdigit(color[i])) {
			return false;
		}
	}

	uint32_t val = (uint32_t)strtoul(color, NULL, 16);
	*result = (val << 8) | 0xFF;
	return true;
}

struct swaybg_state {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wp_viewporter *viewporter;
	struct wp_single_pixel_buffer_manager_v1 *single_pixel_buffer_manager;
	struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
	struct wl_list configs;  // struct swaybg_output_config::link
	struct wl_list outputs;  // struct swaybg_output::link
	struct wl_list images;   // struct swaybg_image::link
	bool run_display;
};


struct swaybg_output_config {
	char *output;
	const char *image_path;
	struct swaybg_image *image;
	enum background_mode mode;
	uint32_t color;
	struct wl_list link;
};

struct swaybg_output {
	uint32_t wl_name;
	struct wl_output *wl_output;
	char *name;
	char *identifier;

	struct swaybg_state *state;
	struct swaybg_output_config *config;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	uint32_t width, height;
	// TODO: consolidate these.
	// Changing scale to mean 1/120ths will require dividing by 120 everywhere scale is used
	int32_t scale;
	uint32_t scale_hundredtwentieths;

	uint32_t configure_serial;
	bool dirty, needs_ack;
	int32_t committed_width, committed_height, committed_scale;
	uint32_t last_frame_time;
	uint64_t frame_count;

	struct pool_buffer buffer;
	void *native_buffer;
	int lbm_origin_x;
	int lbm_origin_y;
	unsigned int lbm_scale;
	struct wp_fractional_scale_v1 *fractional_scale;
	struct wp_viewport *viewport;
	struct wl_list link;
};

bool is_valid_color(const char *color) {
	int len = strlen(color);
	if (len != 7 || color[0] != '#') {
		swaybg_log(LOG_ERROR, "%s is not a valid color for swaybg. "
				"Color should be specified as #rrggbb (no alpha).", color);
		return false;
	}

	int i;
	for (i = 1; i < len; ++i) {
		if (!isxdigit(color[i])) {
			return false;
		}
	}

	return true;
}

void release_buffer(void *data, struct wl_buffer *buffer) {
	struct swaybg_output *output = data;
	//swaybg_log(LOG_DEBUG, "%s %p",__FUNCTION__, buffer);
	if( output->buffer.buffer == buffer ) { // If frame callback is not reallocating buffer, this is always the case
		// Let the output reuse this buffer if it can
		output->buffer.available = true;
		//swaybg_log(LOG_DEBUG, "%s Reusing %p",__FUNCTION__, buffer);
	} else {
		//swaybg_log(LOG_DEBUG, "%s Destroying %p",__FUNCTION__, buffer);
		destroy_buffer(&output->buffer);
	}
}

const struct wl_buffer_listener buffer_listener = {
	.release = release_buffer
};

static const struct wl_callback_listener wl_surface_frame_listener;


void set_lbm_geometry_for_output( struct swaybg_output *output, int dst_width, int dst_height) {
	output->lbm_scale = 1;
	output->lbm_origin_y = 0;
	output->lbm_origin_x = 0;
	const struct lbm_image *image = output->config->image->anim;
	if( !image ) {
		return;
	}
	// Scale the image up until it matches the configured display mode
	while(1) {
		int image_width = image->width * output->lbm_scale;
		int image_height = image->height * output->lbm_scale;
		output->lbm_origin_x = (dst_width - image_width) / 2;
		output->lbm_origin_y = (dst_height - image_height) / 2;

		swaybg_log(LOG_DEBUG, "%s trying %d,%d at %dx\n", __FUNCTION__, output->lbm_origin_x, output->lbm_origin_y, output->lbm_scale);

		// Allow a small margin in case it *almost* fits at a certain scale
		// TODO: allow providing this margin on the command line
		const int margin = 100;
		if ( output->config->mode == BACKGROUND_MODE_CENTER ) {
			break;
		} else if ( output->config->mode == BACKGROUND_MODE_FIT ) {
			if ( output->lbm_origin_x <= margin || output->lbm_origin_y <= margin ) {
				break;
			}
		} else if( output->config->mode == BACKGROUND_MODE_FILL ) {
			if ( output->lbm_origin_x <= margin && output->lbm_origin_y <= margin ) {
				break;
			}
		}
		output->lbm_scale++;
	}
}

static void render_frame(struct swaybg_output *output, cairo_surface_t *surface) {

	int buffer_width = output->width, buffer_height = output->height;

	if( output->scale_hundredtwentieths ) {
		buffer_width *= output->scale_hundredtwentieths;
		buffer_width /= 120;
		buffer_height *= output->scale_hundredtwentieths;
		buffer_height /= 120;
	} else {
		buffer_width *= output->scale;
		buffer_height *= output->scale;
	}

	swaybg_log(LOG_DEBUG, "output %s using buffer of size %dx%d", output->name, buffer_width, buffer_height);

	// If the last committed buffer has the same size as this one would, do
	// not render a new buffer, because it will be identical to the old one
	// TODO use surface local coordinates here
	if (output->committed_width == buffer_width &&
			output->committed_height == buffer_height) {
		if (output->committed_scale != output->scale) {
			wl_surface_set_buffer_scale(output->surface, output->scale);
			wl_surface_commit(output->surface);

			output->committed_scale = output->scale;
		}
		return;
	}
	swaybg_log(LOG_DEBUG, "%s committed size{%ix%i/%i}, buffer size{%ix%i/%i}", output->name,
			output->committed_width, output->committed_height, output->committed_scale,
			buffer_width, buffer_height, output->scale
			);

	if (output->config->mode == BACKGROUND_MODE_SOLID_COLOR &&
			output->state->viewporter &&
			output->state->single_pixel_buffer_manager) {
		uint8_t r8 = (output->config->color >> 24) & 0xFF;
		uint8_t g8 = (output->config->color >> 16) & 0xFF;
		uint8_t b8 = (output->config->color >> 8) & 0xFF;
		uint8_t a8 = (output->config->color >> 0) & 0xFF;
		uint32_t f = 0xFFFFFFFF / 0xFF; // division result is an integer
		uint32_t r32 = r8 * f;
		uint32_t g32 = g8 * f;
		uint32_t b32 = b8 * f;
		uint32_t a32 = a8 * f;
		struct wl_buffer *buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
			output->state->single_pixel_buffer_manager, r32, g32, b32, a32);
		wl_surface_attach(output->surface, buffer, 0, 0);
		wl_surface_damage_buffer(output->surface, 0, 0, INT32_MAX, INT32_MAX);

		wl_surface_commit(output->surface);

		wl_buffer_destroy(buffer);
		return;
	}

	if (output->committed_width != buffer_width ||
			output->committed_height != buffer_height) {
		if(output->buffer.buffer) {
			destroy_buffer(&output->buffer);
		}
		if( !create_buffer(&output->buffer, output->state->shm,
				buffer_width, buffer_height, WL_SHM_FORMAT_ARGB8888, output) )
			return;
	}

	cairo_t *cairo = output->buffer.cairo;
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);
	if (output->config->mode == BACKGROUND_MODE_SOLID_COLOR) {
		cairo_set_source_u32(cairo, output->config->color);
		cairo_paint(cairo);
	} else {
		if (output->config->color) {
			cairo_set_source_u32(cairo, output->config->color);
			cairo_paint(cairo);
		}

		if (surface) {
			render_background_image(cairo, surface,
				output->config->mode, buffer_width, buffer_height);
		}
	}


	if(output->config->image->anim) {
		if(output->native_buffer) {
			free(output->native_buffer);
		}
		output->native_buffer = calloc(1, buffer_width*buffer_height*4);

		set_lbm_geometry_for_output(output, buffer_width, buffer_height);

		render_lbm_image(output->native_buffer, output->config->image->anim, buffer_width, buffer_height, output->lbm_origin_x, output->lbm_origin_y, output->lbm_scale);
		struct wl_callback *cb = wl_surface_frame(output->surface);
		wl_callback_add_listener(cb, &wl_surface_frame_listener, output);
		swaybg_log(LOG_DEBUG, "Added listener for %d", output->wl_name);
		//const unsigned int surface_width = output->width * output->scale;
		//const unsigned int surface_height = output->height * output->scale;
		memcpy( output->buffer.data, output->native_buffer, buffer_width * buffer_height * 4);
	}

	wl_surface_set_buffer_scale(output->surface, output->scale);
	wl_surface_attach(output->surface, output->buffer.buffer, 0, 0);
	wl_surface_damage_buffer(output->surface, 0, 0, INT32_MAX, INT32_MAX);
	wp_viewport_set_destination(output->viewport, output->width, output->height);

	wl_surface_commit(output->surface);

	output->committed_width = buffer_width;
	output->committed_height = buffer_height;
	output->committed_scale = output->scale;
}

// TODO: Update the driver only when the connected outputs change. Dont need to do this every frame.
struct swaybg_output *get_driver_for_image( const struct swaybg_state *state, const struct lbm_image *image ) {
	struct swaybg_output *output;
	wl_list_for_each(output, &state->outputs, link) {
		if( output == image->userdata ) {
			return output;
		}
	}
	return NULL;
}

static void render_animated_frame(struct swaybg_output* output, struct lbm_image *image)
{
	bool do_render = false;
	bool is_driver = (output == get_driver_for_image(output->state, image));

	// Only one output drives the animation
	if( is_driver ) {
		do_render = cycle_palette(image);
	}

	do_render = image->frame_count > output->frame_count;

	if(do_render) {
		bool buffer_size_changed = false; // TODO: check if size has changed
		if( buffer_size_changed || !output->buffer.available ) {
			swaybg_log(LOG_DEBUG, "%s No buffer available. Skipping frame", __FUNCTION__);
			// Can't use the current buffer. Either it's the wrong size or the compositor hasn't released it
			// If the buffer isn't free yet, just hope it will be released eventually. Seems to be the case.
			// If the compositor ever fails to release a buffer, the animation will stop. Should handle this somehow.
			// Could also allocate a new buffer, but doing that a lot might cause us to fall further behind
			//create_buffer(&output->buffer, output->state->shm, surface_width, surface_height, WL_SHM_FORMAT_ARGB8888, output);
			return;
		}

		struct bounding_box damage;
		unsigned int buffer_width = output->width, buffer_height = output->height;
		if( output->scale_hundredtwentieths ) {
			buffer_width *= output->scale_hundredtwentieths;
			buffer_width /= 120;
			buffer_height *= output->scale_hundredtwentieths;
			buffer_height /= 120;
		} else {
			buffer_width *= output->scale;
			buffer_height *= output->scale;
		}
		render_delta(output->buffer.data, image, buffer_width, buffer_height, output->lbm_origin_x, output->lbm_origin_y, output->lbm_scale, &damage, is_driver);
		wl_surface_set_buffer_scale(output->surface, output->scale);
		wl_surface_attach(output->surface, output->buffer.buffer, 0, 0);

		wl_surface_damage_buffer(output->surface,
				damage.min_x,
				damage.min_y,
				damage.max_x - damage.min_x,
				damage.max_y - damage.min_y);
		output->buffer.available = false;
		output->frame_count = image->frame_count;
	}

	wp_viewport_set_destination( output->viewport, output->width, output->height);
	struct wl_callback *cb = wl_surface_frame(output->surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, output);

	wl_surface_commit(output->surface);
}

static void wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time) {
	wl_callback_destroy(cb);

	struct swaybg_output *output = data;
	//swaybg_log(LOG_DEBUG, "%s %s %d", __FUNCTION__, output->name, time);
	output->dirty = false;
	render_animated_frame(output, output->config->image->anim);
	output->last_frame_time = time;
}

static const struct wl_callback_listener wl_surface_frame_listener = {
	.done = wl_surface_frame_done
};

static void destroy_swaybg_image(struct swaybg_image *image) {
	if (!image) {
		return;
	}
	wl_list_remove(&image->link);
	if (image->anim) {
		free_lbm_image(image->anim);
	}
	free(image);
}

static void destroy_swaybg_output_config(struct swaybg_output_config *config) {
	if (!config) {
		return;
	}
	wl_list_remove(&config->link);
	free(config->output);
	free(config);
}

static void destroy_swaybg_output(struct swaybg_output *output) {
	if (!output) {
		return;
	}
	wl_list_remove(&output->link);
	if (output->layer_surface != NULL) {
		zwlr_layer_surface_v1_destroy(output->layer_surface);
	}
	if (output->surface != NULL) {
		wl_surface_destroy(output->surface);
	}
	if (output->native_buffer != NULL) {
		free(output->native_buffer);
	}
	destroy_buffer(&output->buffer);
	wl_output_destroy(output->wl_output);
	free(output->name);
	free(output->identifier);
	free(output);
}

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct swaybg_output *output = data;
	output->width = width;
	output->height = height;
	output->configure_serial = serial;
	output->needs_ack = true;
	if( output->committed_width != (int32_t)width || output->committed_height != (int32_t)height ) {
		swaybg_log(LOG_DEBUG, "Dirtying output %s because of configure{%p,%d,%d,%d}. Output surface needs ack",
				output->name, surface, width, height, serial);
		output->dirty = true;
	}
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	struct swaybg_output *output = data;
	swaybg_log(LOG_DEBUG, "Destroying output %s (%s)",
			output->name, output->identifier);
	destroy_swaybg_output(output);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void output_geometry(void *data, struct wl_output *output, int32_t x,
		int32_t y, int32_t width_mm, int32_t height_mm, int32_t subpixel,
		const char *make, const char *model, int32_t transform) {
	// Who cares
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
	// Who cares
}

void preferred_scale( void *data, struct wp_fractional_scale_v1 *wp_fractional_scale_v1, uint32_t scale ) {
	struct swaybg_output *output = (struct swaybg_output*)data;
	output->scale_hundredtwentieths = scale;
	output->scale = 1;
	swaybg_log(LOG_INFO, "Output %s prefers scale %d", output->name, scale);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
	.preferred_scale = preferred_scale
};

static void create_layer_surface(struct swaybg_output *output) {
	output->surface = wl_compositor_create_surface(output->state->compositor);
	assert(output->surface);

	// Empty input region
	struct wl_region *input_region =
		wl_compositor_create_region(output->state->compositor);
	assert(input_region);
	wl_surface_set_input_region(output->surface, input_region);
	wl_region_destroy(input_region);

	output->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
			output->state->fractional_scale_manager, output->surface);
	wp_fractional_scale_v1_add_listener(output->fractional_scale,
			&fractional_scale_listener, output);

	output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			output->state->layer_shell, output->surface, output->wl_output,
			ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");
	assert(output->layer_surface);

	zwlr_layer_surface_v1_set_size(output->layer_surface, 0, 0);
	zwlr_layer_surface_v1_set_anchor(output->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
	zwlr_layer_surface_v1_add_listener(output->layer_surface,
			&layer_surface_listener, output);
	wl_surface_commit(output->surface);
}

static void output_done(void *data, struct wl_output *wl_output) {
	struct swaybg_output *output = data;
	if (!output->config) {
		swaybg_log(LOG_DEBUG, "Could not find config for output %s (%s)",
				output->name, output->identifier);
		destroy_swaybg_output(output);
	} else if (!output->layer_surface) {
		swaybg_log(LOG_DEBUG, "Found config %s for output %s (%s)",
				output->config->output, output->name, output->identifier);
		create_layer_surface(output);
	}
	if (!output->viewport) {
		output->viewport = wp_viewporter_get_viewport(
				output->state->viewporter, output->surface);
	}
}

static void output_scale(void *data, struct wl_output *wl_output,
		int32_t scale) {
	struct swaybg_output *output = data;
	output->scale = scale;
	if (output->state->run_display && output->width > 0 && output->height > 0) {
		output->dirty = true;
	}
}

static void find_config(struct swaybg_output *output, const char *name) {
	struct swaybg_output_config *config = NULL;
	wl_list_for_each(config, &output->state->configs, link) {
		if (strcmp(config->output, name) == 0) {
			output->config = config;
			return;
		} else if (!output->config && strcmp(config->output, "*") == 0) {
			output->config = config;
		}
	}
}

static void output_name(void *data, struct wl_output *wl_output,
		const char *name) {
	struct swaybg_output *output = data;
	output->name = strdup(name);

	// If description was sent first, the config may already be populated. If
	// there is an identifier config set, keep it.
	if (!output->config || strcmp(output->config->output, "*") == 0) {
		find_config(output, name);
	}
}

static void output_description(void *data, struct wl_output *wl_output,
		const char *description) {
	struct swaybg_output *output = data;

	// wlroots currently sets the description to `make model serial (name)`
	// If this changes in the future, this will need to be modified.
	char *paren = strrchr(description, '(');
	if (paren) {
		size_t length = paren - description;
		output->identifier = malloc(length);
		if (!output->identifier) {
			swaybg_log(LOG_ERROR, "Failed to allocate output identifier");
			return;
		}
		strncpy(output->identifier, description, length);
		output->identifier[length - 1] = '\0';

		find_config(output, output->identifier);
	}
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct swaybg_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor =
			wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct swaybg_output *output = calloc(1, sizeof(struct swaybg_output));
		output->state = state;
		output->wl_name = name;
		output->wl_output =
			wl_registry_bind(registry, name, &wl_output_interface, 4);
		wl_output_add_listener(output->wl_output, &output_listener, output);
		wl_list_insert(&state->outputs, &output->link);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell =
			wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		state->viewporter = wl_registry_bind(registry, name,
			&wp_viewporter_interface, 1);
	} else if (strcmp(interface,
			wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
		state->single_pixel_buffer_manager = wl_registry_bind(registry, name,
			&wp_single_pixel_buffer_manager_v1_interface, 1);
	} else if (strcmp(interface,
			wp_fractional_scale_manager_v1_interface.name) == 0) {
		state->fractional_scale_manager = wl_registry_bind(registry, name,
				&wp_fractional_scale_manager_v1_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	struct swaybg_state *state = data;
	struct swaybg_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &state->outputs, link) {
		if (output->wl_name == name) {
			swaybg_log(LOG_DEBUG, "Destroying output %s (%s)",
					output->name, output->identifier);
			destroy_swaybg_output(output);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static bool store_swaybg_output_config(struct swaybg_state *state,
		struct swaybg_output_config *config) {
	struct swaybg_output_config *oc = NULL;
	wl_list_for_each(oc, &state->configs, link) {
		if (strcmp(config->output, oc->output) == 0) {
			// Merge on top
			if (config->image_path) {
				oc->image_path = config->image_path;
			}
			if (config->color) {
				oc->color = config->color;
			}
			if (config->mode != BACKGROUND_MODE_INVALID) {
				oc->mode = config->mode;
			}
			return false;
		}
	}
	// New config, just add it
	wl_list_insert(&state->configs, &config->link);
	return true;
}

static void parse_command_line(int argc, char **argv,
		struct swaybg_state *state) {
	static struct option long_options[] = {
		{"color", required_argument, NULL, 'c'},
		{"help", no_argument, NULL, 'h'},
		{"image", required_argument, NULL, 'i'},
		{"mode", required_argument, NULL, 'm'},
		{"output", required_argument, NULL, 'o'},
		{"version", no_argument, NULL, 'v'},
		{0, 0, 0, 0}
	};

	const char *usage =
		"Usage: swaybg <options...>\n"
		"\n"
		"  -c, --color            Set the background color.\n"
		"  -h, --help             Show help message and quit.\n"
		"  -i, --image            Set the image to display.\n"
		"  -m, --mode             Set the mode to use for the image.\n"
		"  -o, --output           Set the output to operate on or * for all.\n"
		"  -v, --version          Show the version number and quit.\n"
		"\n"
		"Background Modes:\n"
		"  stretch, fit, fill, center, tile, or solid_color\n";

	struct swaybg_output_config *config = calloc(1, sizeof(struct swaybg_output_config));
	config->output = strdup("*");
	config->mode = BACKGROUND_MODE_INVALID;
	wl_list_init(&config->link); // init for safe removal

	int c;
	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "c:hi:m:o:v", long_options, &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'c':  // color
			if (!parse_color(optarg, &config->color)) {
				swaybg_log(LOG_ERROR, "%s is not a valid color for swaybg. "
					"Color should be specified as rrggbb or #rrggbb (no alpha).", optarg);
				continue;
			}
			break;
		case 'i':  // image
			config->image_path = optarg;
			break;
		case 'm':  // mode
			config->mode = parse_background_mode(optarg);
			if (config->mode == BACKGROUND_MODE_INVALID) {
				swaybg_log(LOG_ERROR, "Invalid mode: %s", optarg);
			}
			break;
		case 'o':  // output
			if (config && !store_swaybg_output_config(state, config)) {
				// Empty config or merged on top of an existing one
				destroy_swaybg_output_config(config);
			}
			config = calloc(1, sizeof(struct swaybg_output_config));
			config->output = strdup(optarg);
			config->mode = BACKGROUND_MODE_INVALID;
			wl_list_init(&config->link);  // init for safe removal
			break;
		case 'v':  // version
			fprintf(stdout, "swaybg version " SWAYBG_VERSION "\n");
			exit(EXIT_SUCCESS);
			break;
		default:
			fprintf(c == 'h' ? stdout : stderr, "%s", usage);
			exit(c == 'h' ? EXIT_SUCCESS : EXIT_FAILURE);
		}
	}
	if (config && !store_swaybg_output_config(state, config)) {
		// Empty config or merged on top of an existing one
		destroy_swaybg_output_config(config);
	}

	// Check for invalid options
	if (optind < argc) {
		config = NULL;
		struct swaybg_output_config *tmp = NULL;
		wl_list_for_each_safe(config, tmp, &state->configs, link) {
			destroy_swaybg_output_config(config);
		}
		// continue into empty list
	}
	if (wl_list_empty(&state->configs)) {
		fprintf(stderr, "%s", usage);
		exit(EXIT_FAILURE);
	}

	// Set default mode and remove empties
	config = NULL;
	struct swaybg_output_config *tmp = NULL;
	wl_list_for_each_safe(config, tmp, &state->configs, link) {
		if (!config->image_path && !config->color) {
			destroy_swaybg_output_config(config);
		} else if (config->mode == BACKGROUND_MODE_INVALID) {
			config->mode = config->image_path
				? BACKGROUND_MODE_STRETCH
				: BACKGROUND_MODE_SOLID_COLOR;
		}
	}
}

int main(int argc, char **argv) {
	swaybg_log_init(LOG_DEBUG);

	struct swaybg_state state = {0};
	wl_list_init(&state.configs);
	wl_list_init(&state.outputs);
	wl_list_init(&state.images);

	parse_command_line(argc, argv, &state);

	// Identify distinct image paths which will need to be loaded
	struct swaybg_image *image;
	struct swaybg_output_config *config;
	wl_list_for_each(config, &state.configs, link) {
		if (!config->image_path) {
			continue;
		}
		wl_list_for_each(image, &state.images, link) {
			if (strcmp(image->path, config->image_path) == 0) {
				config->image = image;
				break;
			}
		}
		if (config->image) {
			continue;
		}
		image = calloc(1, sizeof(struct swaybg_image));
		image->path = config->image_path;
		wl_list_insert(&state.images, &image->link);
		config->image = image;
	}

	state.display = wl_display_connect(NULL);
	if (!state.display) {
		swaybg_log(LOG_ERROR, "Unable to connect to the compositor. "
				"If your compositor is running, check or set the "
				"WAYLAND_DISPLAY environment variable.");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	if (wl_display_roundtrip(state.display) < 0) {
		swaybg_log(LOG_ERROR, "wl_display_roundtrip failed");
		return 1;
	}
	if (state.compositor == NULL || state.shm == NULL ||
			state.layer_shell == NULL) {
		swaybg_log(LOG_ERROR, "Missing a required Wayland interface");
		return 1;
	}

	state.run_display = true;
	while (wl_display_dispatch(state.display) != -1 && state.run_display) {
#ifdef PROFILE
		static int times = 1000;
		if(times-- == 0) state.run_display = false;
#endif
		// Send acks, and determine which images need to be loaded
		struct swaybg_output *output;
		wl_list_for_each(output, &state.outputs, link) {
			if (output->needs_ack) {
				output->needs_ack = false;
				zwlr_layer_surface_v1_ack_configure(
						output->layer_surface,
						output->configure_serial);
				swaybg_log(LOG_DEBUG, "Acking %s", output->name);
			}

			int buffer_width = output->width * output->scale,
				buffer_height = output->height * output->scale;
			bool buffer_change =
				output->committed_height != buffer_height ||
				output->committed_width != buffer_width;
			if (output->dirty && output->config->image && buffer_change) {
				output->config->image->load_required = true;
			}
		}

		// Load images, render associated frames, and unload
		wl_list_for_each(image, &state.images, link) {
			if (!image->load_required) {
				continue;
			}

			cairo_surface_t *surface = NULL;
			image->anim = read_lbm_image(image->path);
			if (!image->anim) {
				surface = load_background_image(image->path);
				if (!surface) {
					swaybg_log(LOG_ERROR, "Failed to load image: %s", image->path);
					continue;
				}
			}

			wl_list_for_each(output, &state.outputs, link) {
				struct swaybg_image *image = output->config->image;
				if ( image->anim && (output->config->mode != BACKGROUND_MODE_FIT ) &&
						(output->config->mode != BACKGROUND_MODE_FILL) &&
						(output->config->mode != BACKGROUND_MODE_CENTER)) {
					// TODO: tiling should be supported too
					swaybg_log(LOG_ERROR, "Only modes \"fit\", \"fill\" and \"center\" are supported for LBM images");
					free_lbm_image(image->anim);
					image->anim = NULL;
				} else if (output->dirty && output->config->image == image) {

					if ( image->anim ) {
						image->anim->userdata = output;
					}
					output->dirty = false;
					render_frame(output, surface);
				}
			}
			image->load_required = false;
			cairo_surface_destroy(surface);
		}

		// Redraw outputs without associated image
		wl_list_for_each(output, &state.outputs, link) {
			if (output->dirty) {
				output->dirty = false;
				render_frame(output, NULL);
			}
		}
	}

	struct swaybg_output *output, *tmp_output;
	wl_list_for_each_safe(output, tmp_output, &state.outputs, link) {
		destroy_swaybg_output(output);
	}

	struct swaybg_output_config *tmp_config = NULL;
	wl_list_for_each_safe(config, tmp_config, &state.configs, link) {
		destroy_swaybg_output_config(config);
	}

	struct swaybg_image *tmp_image;
	wl_list_for_each_safe(image, tmp_image, &state.images, link) {
		destroy_swaybg_image(image);
	}

	return 0;
}
