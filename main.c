/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2020 ProgAndy */

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-export-dmabuf-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"

struct wayland_output {
  struct wl_list link;
  uint32_t id;
  struct wl_output *output;
  char *make;
  char *model;
  int width;
  int height;
  int framerate;
};

struct frame_object {
  uint32_t index;
  int32_t fd;
  uint32_t size;
  uint32_t offset;
  uint32_t stride;
  uint32_t plane_index;
};

struct frame {
  struct zwlr_export_dmabuf_frame_v1 *frame;
  struct wl_buffer *buffer;
  uint32_t width;
  uint32_t height;
  uint32_t offset_x;
  uint32_t offset_y;
  uint32_t buffer_flags;
  uint32_t flags;
  uint32_t format;
  uint32_t mod_high;
  uint32_t mod_low;
  uint32_t num_objects;
  struct frame_object objects[];
};

struct window {
  struct wl_surface *surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *xdg_toplevel;
  struct wp_viewport *viewport;
  bool init;
  int width, height;
};

struct mirror_context {
  struct wl_display *display;
  struct wl_registry *registry;
  struct zwlr_export_dmabuf_manager_v1 *export_manager;
  struct wl_compositor *compositor;
  struct xdg_wm_base *xdg_wm_base;
  struct zwp_linux_dmabuf_v1 *dmabuf;
  struct wp_viewporter *viewporter;

  struct wl_list output_list;

  struct window *window;
  /* Target */
  struct wl_output *source_output;
  int w;
  int h;
  bool with_cursor;

  /* Main frame callback */
  struct zwlr_export_dmabuf_frame_v1 *frame_callback;

  /* If something happens during capture */
  int err;
  bool quit;

  /* dmabuf frames */
  struct frame *incomplete_frame;
  struct frame *next_frame;


};
struct mirror_context *q_ctx = NULL;

void window_destroy(struct window *window);
int window_create(struct mirror_context *ctx);

static void frame_free(struct frame *f) {

  if (f) {
    if (f->buffer)
      wl_buffer_destroy(f->buffer);
    for (uint32_t i = 0; i < f->num_objects; ++i) {
      close(f->objects[i].fd);
    }
    if (f->frame)
      zwlr_export_dmabuf_frame_v1_destroy(f->frame);
    free(f);
  }

}

static void output_handle_geometry(void *data, struct wl_output *wl_output,
    int32_t x, int32_t y, int32_t phys_width, int32_t phys_height,
    int32_t subpixel, const char *make, const char *model,
    int32_t transform) {
  struct wayland_output *output = data;
  output->make = strdup(make);
  output->model = strdup(model);
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
    uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    struct wayland_output *output = data;
    output->width = width;
    output->height = height;
    output->framerate = refresh;
  }
}

static void output_handle_done(void* data, struct wl_output *wl_output) {
  /* Nothing to do */
}

static void output_handle_scale(void* data, struct wl_output *wl_output,
    int32_t factor) {
  /* Nothing to do */
}

static const struct wl_output_listener output_listener = {
  .geometry = output_handle_geometry,
  .mode = output_handle_mode,
  .done = output_handle_done,
  .scale = output_handle_scale,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
  xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
  xdg_wm_base_ping,
};


static void registry_handle_add(void *data, struct wl_registry *reg,
    uint32_t id, const char *interface, uint32_t ver) {
  struct mirror_context *ctx = data;

  if (!strcmp(interface, wl_output_interface.name)) {
    struct wayland_output *output = calloc(1,sizeof(*output));

    output->id = id;
    output->output = wl_registry_bind(reg, id, &wl_output_interface, 1);

    wl_output_add_listener(output->output, &output_listener, output);
    wl_list_insert(&ctx->output_list, &output->link);
  }

  if (!strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name)) {
    ctx->export_manager = wl_registry_bind(reg, id,
        &zwlr_export_dmabuf_manager_v1_interface, 1);
  }

  if (!strcmp(interface, wl_compositor_interface.name)) {
    ctx->compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 1);
  }

  if (!strcmp(interface, wp_viewporter_interface.name)) {
    ctx->viewporter = wl_registry_bind(reg, id, &wp_viewporter_interface, 1);
  }

  if (!strcmp(interface, xdg_wm_base_interface.name)) {
    ctx->xdg_wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(ctx->xdg_wm_base, &xdg_wm_base_listener, ctx);
  }

  if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name)) {
    if (ver < 3) {
      return;
    }
    ctx->dmabuf = wl_registry_bind(reg, id, &zwp_linux_dmabuf_v1_interface, 3);
    /* assume we get a compatible format from dmabuf export */
  }
}

static void remove_output(struct wayland_output *out) {
  wl_list_remove(&out->link);
  free(out->make);
  free(out->model);
  free(out);
}

static struct wayland_output *find_output(struct mirror_context *ctx,
    struct wl_output *out, uint32_t id) {
  struct wayland_output *output, *tmp;
  wl_list_for_each_safe(output, tmp, &ctx->output_list, link) {
    if ((output->output == out) || (output->id == id)) {
      return output;
    }
  }
  return NULL;
}

static void registry_handle_remove(void *data, struct wl_registry *reg,
    uint32_t id) {
  remove_output(find_output((struct mirror_context *)data, NULL, id));
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_handle_add,
  .global_remove = registry_handle_remove,
};

static void frame_start(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
    uint32_t width, uint32_t height, uint32_t offset_x, uint32_t offset_y,
    uint32_t buffer_flags, uint32_t flags, uint32_t format,
    uint32_t mod_high, uint32_t mod_low, uint32_t num_objects) {
  struct mirror_context *ctx = data;
  int err = 0;

  /* Allocate DRM specific struct */
  struct frame *f = calloc(1,sizeof(*f)+num_objects*sizeof(struct frame_object));
  if (!f) {
    err = ENOMEM;
    goto fail;
  }
  // suppress y-invert flag.
  flags &= ~ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT;
  f->frame = frame;
  f->width = width;
  f->height = height;
  f->offset_x = offset_x;
  f->offset_y = offset_y;
  f->buffer_flags = buffer_flags;
  f->flags = flags;
  f->format = format;
  f->mod_high = mod_high;
  f->mod_low = mod_low;
  f->num_objects = num_objects;

  f->width = width;
  f->height = height;

  ctx->incomplete_frame = f;

  return;

fail:
  ctx->err = err;
  if (f) frame_free(f);
}

static void frame_object(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
    uint32_t index, int32_t fd, uint32_t size, uint32_t offset,
    uint32_t stride, uint32_t plane_index) {
  struct mirror_context *ctx = data;
  struct frame *f = ctx->incomplete_frame;
  f->objects[index].index = index;
  f->objects[index].fd = fd;
  f->objects[index].size = size;
  f->objects[index].offset = offset;
  f->objects[index].stride = stride;
  f->objects[index].plane_index = plane_index;

}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
  struct frame *f = data;

  frame_free(f);
}

static const struct wl_buffer_listener buffer_listener = {
  buffer_release
};


static void display_frame(struct mirror_context *ctx);
static void request_frame(struct mirror_context *ctx);

static void frame_ready(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
  struct mirror_context *ctx = data;
  struct frame *f = ctx->incomplete_frame;
  ctx->incomplete_frame = NULL;
  struct zwp_linux_buffer_params_v1 *params;
  params = zwp_linux_dmabuf_v1_create_params(ctx->dmabuf);

  for (uint32_t i = 0; i < f->num_objects; ++i) {
    zwp_linux_buffer_params_v1_add(params,
                 f->objects[i].fd,
                 f->objects[i].plane_index,
                 f->objects[i].offset,
                 f->objects[i].stride,
                 f->mod_high,
                 f->mod_low);
  }


  // TODO: handle failed creation
  // TODO: handle rotation
  // TODO: necessary to handle Y_INVERT differences?
  f->buffer =
      zwp_linux_buffer_params_v1_create_immed(params,
                f->width,
                f->height,
                f->format,
                f->flags);
  zwp_linux_buffer_params_v1_destroy(params);
  wl_buffer_add_listener(f->buffer, &buffer_listener, f);

  if (ctx->next_frame) {
    frame_free(ctx->next_frame);
  }
  ctx->next_frame = f;


 //* Frames will not be requested in the render loop
  if (!ctx->quit && !ctx->err) {
    if (ctx->window && ctx->window->init) {
      display_frame(ctx);
    }
    request_frame(ctx);
  }
  return;
}

static void frame_cancel(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
    uint32_t reason) {
  struct mirror_context *ctx = data;
  frame_free(ctx->incomplete_frame);
  if (reason == ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT) {
    /* Permanent error. Exit! */
    ctx->err = true;
  } else {
    request_frame(ctx);
  }
}

static const struct zwlr_export_dmabuf_frame_v1_listener frame_listener = {
  .frame = frame_start,
  .object = frame_object,
  .ready = frame_ready,
  .cancel = frame_cancel,
};

static void request_frame(struct mirror_context *ctx) {
  ctx->frame_callback = zwlr_export_dmabuf_manager_v1_capture_output(
      ctx->export_manager, ctx->with_cursor, ctx->source_output);

  zwlr_export_dmabuf_frame_v1_add_listener(ctx->frame_callback,
      &frame_listener, ctx);
}




static void xdg_surface_handle_configure(void *data,
    struct xdg_surface *surface, uint32_t serial) {
  struct mirror_context *ctx = data;

  xdg_surface_ack_configure(surface, serial);
  /* mark window ready */
  ctx->window->init = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
  xdg_surface_handle_configure,
};

static void xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *toplevel,
    int32_t width, int32_t height, struct wl_array *states) {

  struct mirror_context *ctx = data;
  if (width > 0) {
    ctx->window->width = width;
  }
  if (height > 0) {
    ctx->window->height = height;
  }

  if (ctx->window->viewport && ctx->window->width > 0 && ctx->window->height > 0) {
    // TODO: aspect ratio?
    wp_viewport_set_destination(ctx->window->viewport, ctx->window->width, ctx->window->height);
  }
}

static void
xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
  struct mirror_context *ctx = data;
  ctx->quit = true;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
  xdg_toplevel_handle_configure,
  xdg_toplevel_handle_close,
};


static void on_quit_signal(int signo) {
  if (q_ctx) {
    q_ctx->quit = true;
  }
}


static int main_loop(struct mirror_context *ctx) {

  q_ctx = ctx;

  if (signal(SIGINT, on_quit_signal) == SIG_ERR) {
    /*av_log(ctx, AV_LOG_ERROR, "Unable to install signal handler!\n");*/
    return EINVAL;
  }

  request_frame(ctx);

  while (wl_display_dispatch(ctx->display) != -1 && !ctx->err && !ctx->quit);

  return ctx->err;
}

static int init(struct mirror_context *ctx) {
  wl_list_init(&ctx->output_list);

  ctx->display = wl_display_connect(NULL);
  if (!ctx->display) {
    puts("Failed to connect to display");
    return EINVAL;
  }


  ctx->registry = wl_display_get_registry(ctx->display);
  wl_registry_add_listener(ctx->registry, &registry_listener, ctx);

  wl_display_roundtrip(ctx->display);
  wl_display_dispatch(ctx->display);
  assert(ctx->compositor);

  if (!ctx->export_manager) {
    printf("Compositor doesn't support %s!\n",
        zwlr_export_dmabuf_manager_v1_interface.name);
    return -1;
  }
  if (!ctx->dmabuf) {
    printf("Compositor doesn't support %s!\n",
        zwp_linux_dmabuf_v1_interface.name);
    return -1;
  }
  if (!ctx->xdg_wm_base) {
    printf("Compositor doesn't support %s!\n",
        xdg_wm_base_interface.name);
    return -1;
  }

  return 0;
}


void window_destroy(struct window *window) {
  if (window->xdg_toplevel) {
    xdg_toplevel_destroy(window->xdg_toplevel);
  }
  if (window->xdg_surface) {
    xdg_surface_destroy(window->xdg_surface);
  }
  if (window->surface) {
    wl_surface_destroy(window->surface);
  }
  free(window);
}

int window_create(struct mirror_context *ctx) {
  struct window *window;
  int err = 0;

  assert(!ctx->window);

  window = calloc(1,sizeof(*window));
  if (!window) {
    return 1;
  }

  window->width = ctx->w;
  window->height = ctx->h;

  window->surface = wl_compositor_create_surface(ctx->compositor);

  window->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->xdg_wm_base, window->surface);
  xdg_surface_add_listener(window->xdg_surface, &xdg_surface_listener, ctx);

  window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
  xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener, ctx);

  if (ctx->viewporter) {
    window->viewport = wp_viewporter_get_viewport(ctx->viewporter, window->surface);
  }

  xdg_toplevel_set_title(window->xdg_toplevel, "wlr dmabuf output mirror");

  wl_surface_commit(window->surface);

  ctx->window = window;

  return err;
}


static void display_frame(struct mirror_context *ctx) {

  struct frame *next = ctx->next_frame;
  ctx->next_frame = NULL;
  if (!next) return;


  wl_surface_attach(ctx->window->surface, next->buffer, 0, 0);
  wl_surface_damage(ctx->window->surface, 0, 0, ctx->window->width, ctx->window->height);

  wl_surface_commit(ctx->window->surface);
}


static void uninit(struct mirror_context *ctx);

int main(int argc, char *argv[]) {
  int err;
  struct mirror_context ctx = { 0 };
  struct wayland_output *o, *tmp_o;


  err = init(&ctx);
  if (err) {
    puts("Could not initialize wayland");
    goto end;
  }

  if (argc != 2 || !strcmp(argv[1], "-h")) {
    printf("wdomirror SOURCE_ID\n"
        "Mirror wlroots output with dmabuf protocols.\n\n");
    wl_list_for_each_reverse_safe(o, tmp_o, &ctx.output_list, link) {
      printf("Mirrorable output: %s Model: %s: ID: %i\n",
        o->make, o->model, o->id);
    }
    err = 1;
    goto end;
  }

  const int o_id = strtol(argv[1], NULL, 10);
  o = find_output(&ctx, NULL, o_id);
  if (!o) {
    printf("Unable to find output with ID %i!\n", o_id);
    err = 1;
    goto end;
  }
  printf("Mirroring output: %s Model: %s: ID: %i\n",
        o->make, o->model, o->id);

  ctx.source_output = o->output;
  ctx.w = o->width;
  ctx.h = o->height;
  ctx.with_cursor = true;


  err = window_create(&ctx);
  if (err) {
    goto end;
  }

  err = main_loop(&ctx);
  if (err) {
    goto end;
  }

end:
  uninit(&ctx);
  return err;
}

static void uninit(struct mirror_context *ctx) {
  struct wayland_output *output, *tmp_o;
  wl_list_for_each_safe(output, tmp_o, &ctx->output_list, link) {
    remove_output(output);
  }
  if (ctx->window)
    window_destroy(ctx->window);

  if (ctx->export_manager) {
    zwlr_export_dmabuf_manager_v1_destroy(ctx->export_manager);
  }
  if (ctx->dmabuf) {
    zwp_linux_dmabuf_v1_destroy(ctx->dmabuf);
  }
  if (ctx->xdg_wm_base) {
    xdg_wm_base_destroy(ctx->xdg_wm_base);
  }
  if (ctx->compositor) {
    wl_compositor_destroy(ctx->compositor);
  }
  if (ctx->registry) {
    wl_registry_destroy(ctx->registry);
  }
  if (ctx->display) {
    wl_display_flush(ctx->display);
    wl_display_disconnect(ctx->display);
  }

  if (ctx->next_frame) {
    frame_free(ctx->next_frame);
  }

}

/* vim: set ts=2 sw=2 et: */
