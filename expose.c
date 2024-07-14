#include "expose.h"
#include "xdg-shell-client-protocol.h"
#include <cairo/cairo.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#define ASSERT(condition, message)                                             \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "Assertion failed: (%s), function %s, line %d.\n",       \
              #condition, __FUNCTION__, __LINE__);                             \
      fprintf(stderr, "Error message: %s.\n", message);                        \
    }                                                                          \
  } while (0)

static void randname(char *buf) {
  struct timespec ts;
  ASSERT(clock_gettime(CLOCK_REALTIME, &ts) == 0, "failed to get time");
  long r = ts.tv_nsec;
  for (int i = 0; i < 6; ++i) {
    buf[i] = 'A' + (r & 15) + (r & 16) * 2;
    r >>= 5;
  }
}

static int create_shm_file(void) {
  int retries = 100;
  do {
    char name[] = "/wl_shm-XXXXXX";
    randname(name + sizeof(name) - 7);
    --retries;
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      shm_unlink(name);
      return fd;
    }
  } while (retries > 0 && errno == EEXIST);
  return -1;
}

static int allocate_shm_file(size_t size) {
  int fd = create_shm_file();
  ASSERT(fd >= 0, "failed to create shared memory");
  if (fd < 0)
    return -1;
  int ret;
  do {
    ret = ftruncate(fd, size);
  } while (ret < 0 && errno == EINTR);
  if (ret < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

struct dupli {
  int var1;
  int var2;
};

typedef struct dupli tuple;

struct wl_window {
  int node;
  int width, height;
  int ctrw, ctrh;
  int xcr, ycr;
  float scale_factor;
  char *title;
};

struct client_state {
  struct wl_display *wl_display;
  struct wl_registry *wl_registry;
  struct wl_shm *wl_shm;
  struct wl_compositor *wl_compositor;
  struct xdg_wm_base *xdg_wm_base;

  struct wl_surface *wl_surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *xdg_toplevel;

  struct wl_seat *wl_seat;
  struct xkb_context *xkb_context;
  struct xkb_state *xkb_state;
  int32_t xkb_delay;

  int display_width, display_height;
  int window_sum;
  struct wl_window *window;

  int focused_window;
  bool focus_changing;
  clock_t last_focus_change_time;
  bool frame_render;

  cairo_surface_t *surface;
  cairo_t *cr;

  bool closed;
};

static void find_nearest_window(struct client_state *state, char dir) {
  clock_t current_time = clock();
  double time_elapsed_ms =
      ((double)(current_time - state->last_focus_change_time)) /
      CLOCKS_PER_SEC * 1000;

  if (state->focus_changing)
    return;
  else if (time_elapsed_ms < state->xkb_delay)
    return;

  state->focus_changing = true;

  int xori, yori;
  int dist, nwin = -1,
            near = state->display_width * state->display_width +
                   state->display_height * state->display_height;

  switch (dir) {
  case 'u':
    xori = state->window[state->focused_window].xcr +
           state->window[state->focused_window].width *
               state->window[state->focused_window].scale_factor * 0.5;
    for (int i = 0; i < state->window_sum; i++) {
      if (state->window[i].xcr < xori &&
          state->window[i].xcr +
                  state->window[i].width * state->window[i].scale_factor >
              xori &&
          state->window[i].ycr < state->window[state->focused_window].ycr) {
        int xsep =
            (state->window[i].xcr +
             state->window[i].width * state->window[i].scale_factor * 0.5) -
            (state->window[state->focused_window].xcr +
             state->window[state->focused_window].width *
                 state->window[state->focused_window].scale_factor * 0.5);
        int ysep =
            (state->window[i].ycr -
             state->window[i].height * state->window[i].scale_factor * 0.5) -
            (state->window[state->focused_window].ycr +
             state->window[state->focused_window].height *
                 state->window[state->focused_window].scale_factor * 0.5);
        dist = xsep * xsep + ysep * ysep;
        if (dist < near) {
          near = dist;
          nwin = i;
        }
      }
    }
    break;
  case 'd':
    xori = state->window[state->focused_window].xcr +
           state->window[state->focused_window].width *
               state->window[state->focused_window].scale_factor * 0.5;
    for (int i = 0; i < state->window_sum; i++) {
      if (state->window[i].xcr < xori &&
          state->window[i].xcr +
                  state->window[i].width * state->window[i].scale_factor >
              xori &&
          state->window[i].ycr > state->window[state->focused_window].ycr) {
        int xsep =
            (state->window[i].xcr +
             state->window[i].width * state->window[i].scale_factor * 0.5) -
            (state->window[state->focused_window].xcr +
             state->window[state->focused_window].width *
                 state->window[state->focused_window].scale_factor * 0.5);
        int ysep =
            (state->window[i].ycr -
             state->window[i].height * state->window[i].scale_factor * 0.5) -
            (state->window[state->focused_window].ycr +
             state->window[state->focused_window].height *
                 state->window[state->focused_window].scale_factor * 0.5);
        dist = xsep * xsep + ysep * ysep;
        if (dist < near) {
          near = dist;
          nwin = i;
        }
      }
    }
    break;
  case 'l':
    yori = state->window[state->focused_window].ycr +
           state->window[state->focused_window].height *
               state->window[state->focused_window].scale_factor * 0.5;
    for (int i = 0; i < state->window_sum; i++) {
      if (state->window[i].ycr < yori &&
          state->window[i].ycr +
                  state->window[i].height * state->window[i].scale_factor >
              yori &&
          state->window[i].xcr < state->window[state->focused_window].xcr) {
        int xsep =
            (state->window[i].xcr +
             state->window[i].width * state->window[i].scale_factor * 0.5) -
            (state->window[state->focused_window].xcr +
             state->window[state->focused_window].width *
                 state->window[state->focused_window].scale_factor * 0.5);
        int ysep =
            (state->window[i].ycr -
             state->window[i].height * state->window[i].scale_factor * 0.5) -
            (state->window[state->focused_window].ycr +
             state->window[state->focused_window].height *
                 state->window[state->focused_window].scale_factor * 0.5);
        dist = xsep * xsep + ysep * ysep;
        if (dist < near) {
          near = dist;
          nwin = i;
        }
      }
    }
    break;
  case 'r':
    yori = state->window[state->focused_window].ycr +
           state->window[state->focused_window].height *
               state->window[state->focused_window].scale_factor * 0.5;
    for (int i = 0; i < state->window_sum; i++) {
      if (state->window[i].ycr < yori &&
          state->window[i].ycr +
                  state->window[i].height * state->window[i].scale_factor >
              yori &&
          state->window[i].xcr > state->window[state->focused_window].xcr) {
        int xsep =
            (state->window[i].xcr +
             state->window[i].width * state->window[i].scale_factor * 0.5) -
            (state->window[state->focused_window].xcr +
             state->window[state->focused_window].width *
                 state->window[state->focused_window].scale_factor * 0.5);
        int ysep =
            (state->window[i].ycr -
             state->window[i].height * state->window[i].scale_factor * 0.5) -
            (state->window[state->focused_window].ycr +
             state->window[state->focused_window].height *
                 state->window[state->focused_window].scale_factor * 0.5);
        dist = xsep * xsep + ysep * ysep;
        if (dist < near) {
          near = dist;
          nwin = i;
        }
      }
    }
    break;
  }

  if (nwin > -1 && nwin != state->focused_window)
    state->focused_window = nwin;

  state->focus_changing = false;
  state->last_focus_change_time = clock();
}

static void focus_window(int node_id) {
  char focus_command[256];
  snprintf(focus_command, sizeof(focus_command),
           "sleep %f; swaymsg [con_id=%d] focus", DELAY_SEC, node_id);
  pid_t swayipc_pid = fork();
  ASSERT(swayipc_pid != 0, "failed to create new process");
  execl("/bin/sh", "sh", "-c", focus_command, (char *)NULL);
}

static void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                               uint32_t format, int32_t fd, uint32_t size) {
  struct client_state *state = data;

  char *map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ASSERT(map_shm != MAP_FAILED, "failed to mmap keymap");

  struct xkb_keymap *keymap = xkb_keymap_new_from_string(
      state->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  ASSERT(keymap != NULL, "failed to create keymap");
  munmap(map_shm, size);
  close(fd);

  state->xkb_state = xkb_state_new(keymap);
  xkb_keymap_unref(keymap);
  ASSERT(state->xkb_state != NULL, "failed to create xkb_state");
}

static void wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
                            uint32_t serial, uint32_t time, uint32_t key,
                            uint32_t state_) {
  struct client_state *state = data;

  xkb_keycode_t keycode = key + 8;
  xkb_keysym_t keysym = xkb_state_key_get_one_sym(state->xkb_state, keycode);

  if (state_ == WL_KEYBOARD_KEY_STATE_PRESSED) {
    xkb_state_update_key(state->xkb_state, keycode, XKB_KEY_DOWN);
  } else {
    xkb_state_update_key(state->xkb_state, keycode, XKB_KEY_UP);
  }

  switch (keysym) {
  case XKB_KEY_Escape:
    state->closed = true;
    return;
  case XKB_KEY_Left:
    if (!state->frame_render) {
      state->frame_render = true;
      return;
    }
    state->frame_render = true;
    find_nearest_window(state, 'l');
    return;
  case XKB_KEY_Right:
    if (!state->frame_render) {
      state->frame_render = true;
      return;
    }
    state->frame_render = true;
    find_nearest_window(state, 'r');
    return;
  case XKB_KEY_Up:
    if (!state->frame_render) {
      state->frame_render = true;
      return;
    }
    state->frame_render = true;
    find_nearest_window(state, 'u');
    return;
  case XKB_KEY_Down:
    if (!state->frame_render) {
      state->frame_render = true;
      return;
    }
    state->frame_render = true;
    find_nearest_window(state, 'd');
    return;
  case XKB_KEY_space:
    focus_window(state->window[state->focused_window].node);
    state->closed = true;
    return;
  }
}

static void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                                  uint32_t serial, uint32_t mods_depressed,
                                  uint32_t mods_latched, uint32_t mods_locked,
                                  uint32_t group) {
  struct client_state *state = data;
  xkb_state_update_mask(state->xkb_state, mods_depressed, mods_latched,
                        mods_locked, 0, 0, group);
}

static void wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                                    int32_t rate, int32_t delay) {
  struct client_state *state = data;
  state->xkb_delay = delay;
}

static void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
                              uint32_t serial, struct wl_surface *surface,
                              struct wl_array *keys) {}

static const struct wl_keyboard_listener wl_keyboard_listener = {
    .keymap = wl_keyboard_keymap,
    .enter = wl_keyboard_enter,
    .key = wl_keyboard_key,
    .modifiers = wl_keyboard_modifiers,
    .repeat_info = wl_keyboard_repeat_info,
};

static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
  wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

int compare_windows(const void *win1, const void *win2) {
  struct wl_window *window1 = (struct wl_window *)win1;
  struct wl_window *window2 = (struct wl_window *)win2;
  if (window1->height != window2->height)
    return window2->height - window1->height;
  if (window1->width != window2->width)
    return window2->width - window1->width;
  return window2->node - window1->node;
}

int nfdh(int strip_width, struct wl_window *windows, int window_count) {
  int current_level_height = 0, current_y = 0, current_x = 0;

  for (int i = 0; i < window_count; i++) {
    windows[i].xcr = current_x;
    windows[i].ycr = current_y;
    if (current_x + windows[i].ctrw > strip_width) {
      current_x = 0;
      current_y += current_level_height;
      current_level_height = 0;
      windows[i].xcr = current_x;
      windows[i].ycr = current_y;
    }
    current_x += windows[i].ctrw;
    if (windows[i].ctrh > current_level_height)
      current_level_height = windows[i].ctrh;
  }

  return current_y + current_level_height;
}

void adjust_sizes_of_windows(struct client_state *state) {
  for (int i = 0; i < state->window_sum; i++) {
    state->window[i].ctrw = state->window[i].width > state->display_width
                                ? state->display_width
                                : state->window[i].width;
    state->window[i].ctrh = state->window[i].height > state->display_height
                                ? state->display_height
                                : state->window[i].height;
    state->window[i].ctrw += 2 * state->window[i].ctrw * MARGIN_RATIO;
    state->window[i].ctrh += 2 * state->window[i].ctrh * MARGIN_RATIO;
    state->window[i].scale_factor = 1;
  }
}

tuple find_good_packing(struct client_state *state) {
  qsort(state->window, state->window_sum, sizeof(struct wl_window),
        compare_windows);

  const float target_ratio =
      (float)state->display_height / (float)state->display_width;

  int strip_width_min = 0, strip_width_max = 0;
  for (int i = 0; i < state->window_sum; i++) {
    strip_width_min = fmax(strip_width_min, state->window[i].width);
    strip_width_max += state->window[i].width;
  }

  tuple res;

  int plmt_high = nfdh(strip_width_min, state->window, state->window_sum);
  float ratio_high = (float)plmt_high / (float)strip_width_min;

  if (ratio_high <= target_ratio) {
    res.var1 = strip_width_min;
    res.var2 = plmt_high;
    return res;
  }

  int plmt_low = nfdh(strip_width_max, state->window, state->window_sum);
  float ratio_low = (float)plmt_low / (float)strip_width_max;

  if (ratio_low >= target_ratio) {
    res.var1 = strip_width_max;
    res.var2 = plmt_low;
    return res;
  }

  while ((float)strip_width_max / (float)strip_width_min > 1 + COVGT_TOL) {
    int strip_width = sqrt(strip_width_min * strip_width_max);
    int plmt_bin = nfdh(strip_width, state->window, state->window_sum);
    float ratio_bin = (float)plmt_bin / (float)strip_width;
    if (ratio_bin > target_ratio) {
      ratio_high = ratio_bin;
      plmt_high = plmt_bin;
      strip_width_min = strip_width;
    } else {
      ratio_low = ratio_bin;
      plmt_low = plmt_bin;
      strip_width_max = strip_width;
    }
  }

  if (ratio_high - target_ratio < target_ratio - ratio_low) {
    res.var1 = strip_width_min;
    res.var2 = plmt_high;
  } else {
    res.var1 = strip_width_max;
    res.var2 = plmt_low;
  }

  res.var2 = nfdh(res.var1, state->window, state->window_sum);

  return res;
}

void refine_packing(tuple pack, struct client_state *state) {
  float width_ratio = (float)pack.var1 / (float)state->display_width;
  float height_ratio = (float)pack.var2 / (float)state->display_height;
  float scale_factor = width_ratio > height_ratio
                           ? state->display_width * PACK_RATIO / pack.var1
                           : state->display_height * PACK_RATIO / pack.var2;

  for (int i = 0, level = 0, boundary = 0, track = 0, height = 0;
       i < state->window_sum; i++) {
    if (state->window[i].ycr == level) {
      if (state->window[i].xcr + state->window[i].ctrw > boundary)
        boundary = state->window[i].xcr + state->window[i].ctrw;
      if (state->window[i].ctrh > height)
        height = state->window[i].ctrh;
    } else {
      for (int j = track; j < i; j++) {
        state->window[j].xcr += (pack.var1 - boundary) * 0.5;
        state->window[j].ycr += (height - state->window[j].ctrh) * 0.5;
      }
      level = state->window[i].ycr;
      boundary = state->window[i].xcr + state->window[i].ctrw;
      height = state->window[i].ctrh;
      track = i;
    }
    if (i == state->window_sum - 1) {
      for (int j = track; j <= i; j++) {
        state->window[j].xcr += (pack.var1 - boundary) * 0.5;
        state->window[j].ycr += (height - state->window[j].ctrh) * 0.5;
      }
    }
  }

  for (int i = 0; i < state->window_sum; i++) {
    state->window[i].scale_factor *= scale_factor;
    state->window[i].xcr =
        (state->display_width - pack.var1 * state->window[i].scale_factor) *
            0.5 +
        (state->window[i].xcr +
         (state->window[i].ctrw - state->window[i].width) * 0.5) *
            state->window[i].scale_factor;
    state->window[i].ycr =
        state->display_height -
        (state->display_height - pack.var2 * state->window[i].scale_factor) *
            0.5 -
        (state->window[i].ycr + state->window[i].ctrh -
         (state->window[i].ctrh - state->window[i].height) * 0.5) *
            state->window[i].scale_factor;
  }
}

static void expose_layout_alloc(struct client_state *state) {
  adjust_sizes_of_windows(state);
  refine_packing(find_good_packing(state), state);
}

static void wl_window_plot(struct client_state *state, int n) {
  char imagepath[256];
  snprintf(imagepath, sizeof(imagepath), "%s%d.png", getenv("EXPOSWAYDIR"),
           state->window[n].node);

  cairo_surface_t *image = cairo_image_surface_create_from_png(imagepath);
  ASSERT(image != NULL, "failed to create cairo image surface");
  cairo_save(state->cr);

  cairo_translate(state->cr, state->window[n].xcr, state->window[n].ycr);
  cairo_scale(state->cr, state->window[n].scale_factor,
              state->window[n].scale_factor);

  cairo_set_source_surface(state->cr, image, 0, 0);
  cairo_paint(state->cr);

  if (state->frame_render && state->focused_window == n) {
    cairo_set_source_rgb(state->cr, FRAME_CLR);
    cairo_set_line_width(state->cr, FRAME_WDH / state->window[n].scale_factor);
    cairo_rectangle(state->cr, -FRAME_WDH / state->window[n].scale_factor,
                    -FRAME_WDH / state->window[n].scale_factor,
                    state->window[state->focused_window].width +
                        FRAME_WDH * 2 / state->window[n].scale_factor,
                    state->window[state->focused_window].height +
                        FRAME_WDH * 2 / state->window[n].scale_factor);
    cairo_stroke(state->cr);
  }

  cairo_restore(state->cr);
  cairo_surface_destroy(image);
}

static void wl_title_render(struct client_state *state, int n) {
  PangoFontDescription *font_description;
  font_description = pango_font_description_new();
  pango_font_description_set_family(font_description, "monospace");
  pango_font_description_set_weight(font_description, PANGO_WEIGHT_NORMAL);
  pango_font_description_set_absolute_size(font_description,
                                           TITLE_SIZE * PANGO_SCALE);

  PangoLayout *layout;
  layout = pango_cairo_create_layout(state->cr);
  pango_layout_set_font_description(layout, font_description);
  pango_layout_set_text(layout, state->window[n].title, -1);

  PangoRectangle extends;
  pango_layout_get_pixel_extents(layout, NULL, &extends);

  cairo_set_source_rgb(state->cr, TITLE_CLR);
  cairo_move_to(state->cr,
                state->window[n].xcr +
                    (state->window[n].width * state->window[n].scale_factor -
                     extends.width) /
                        2,
                state->window[n].ycr +
                    state->window[n].height * state->window[n].scale_factor +
                    extends.height / 4);
  pango_cairo_show_layout(state->cr, layout);

  g_object_unref(layout);
  pango_font_description_free(font_description);
}

static struct wl_buffer *draw_cairo(struct client_state *state) {
  const int width = state->display_width, height = state->display_height;
  int stride = width * 4;
  int size = stride * height;

  int fd = allocate_shm_file(size);
  if (fd == -1)
    return NULL;

  unsigned char *data =
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    return NULL;
  }

  struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, size);
  ASSERT(pool != NULL, "failed to create wayland shared memory pool");

  struct wl_buffer *buffer = wl_shm_pool_create_buffer(
      pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
  ASSERT(buffer != NULL, "failed to create wayland buffer");
  wl_shm_pool_destroy(pool);
  close(fd);

  state->surface = cairo_image_surface_create_for_data(
      data, CAIRO_FORMAT_ARGB32, width, height, stride);
  ASSERT(state->surface != NULL,
         "failed to create cairo image surface for wayland");
  state->cr = cairo_create(state->surface);

  for (int n = 0; n < state->window_sum; n++) {
    wl_window_plot(state, n);
    wl_title_render(state, n);
  }

  cairo_destroy(state->cr);
  cairo_surface_destroy(state->surface);

  munmap(data, size);

  wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
  return buffer;
}

static void xdg_toplevel_configure(void *data,
                                   struct xdg_toplevel *xdg_toplevel,
                                   int32_t width, int32_t height,
                                   struct wl_array *states) {
  struct client_state *state = data;
  xdg_toplevel_set_min_size(state->xdg_toplevel, state->display_width,
                            state->display_height);
  xdg_toplevel_set_fullscreen(xdg_toplevel, NULL);
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
  struct client_state *state = data;
  state->closed = true;

  wl_display_disconnect(state->wl_display);

  exit(EXIT_SUCCESS);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial) {
  struct client_state *state = data;

  xdg_surface_ack_configure(xdg_surface, serial);

  struct wl_buffer *buffer = draw_cairo(state);
  ASSERT(buffer != NULL, "failed to create wayland buffer for cairo");
  wl_surface_attach(state->wl_surface, buffer, 0, 0);
  wl_surface_damage(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);

  wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base,
                             uint32_t serial) {
  xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static const struct wl_callback_listener wl_surface_frame_listener;

static void wl_surface_frame_done(void *data, struct wl_callback *cb,
                                  uint32_t time) {
  struct client_state *state = data;

  wl_callback_destroy(cb);

  struct wl_buffer *buffer = draw_cairo(state);
  ASSERT(buffer != NULL, "failed to create wayland buffer");
  wl_surface_attach(state->wl_surface, buffer, 0, 0);
  wl_surface_damage(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);

  cb = wl_surface_frame(state->wl_surface);
  wl_callback_add_listener(cb, &wl_surface_frame_listener, state);

  wl_surface_commit(state->wl_surface);
}

static const struct wl_callback_listener wl_surface_frame_listener = {
    .done = wl_surface_frame_done,
};

static void registry_global(void *data, struct wl_registry *wl_registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  struct client_state *state = data;

  if (strcmp(interface, wl_shm_interface.name) == 0) {
    state->wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
    ASSERT(state->wl_shm != NULL, "failed to bind wayland shared memory");
  } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
    state->wl_compositor =
        wl_registry_bind(wl_registry, name, &wl_compositor_interface, 4);
    ASSERT(state->wl_compositor != NULL, "failed to bind wayland compositor");
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    state->xdg_wm_base =
        wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, 1);
    ASSERT(state->xdg_wm_base != NULL,
           "failed to bind xdg windows manager base");
    xdg_wm_base_add_listener(state->xdg_wm_base, &xdg_wm_base_listener, state);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    state->wl_seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 8);
    ASSERT(state->wl_seat != NULL, "failed to bind wayland seat");
    struct wl_keyboard *keyboard = wl_seat_get_keyboard(state->wl_seat);
    ASSERT(keyboard != NULL, "failed to get keyboard");
    wl_keyboard_add_listener(keyboard, &wl_keyboard_listener, state);
  }
}

static void registry_global_remove(void *data, struct wl_registry *wl_registry,
                                   uint32_t name) {}

static const struct wl_registry_listener wl_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {
  ASSERT(getenv("EXPOSWAYMON") != NULL,
         "unset environment variable EXPOSWAYMON");
  ASSERT(getenv("EXPOSWAYDIR") != NULL,
         "unset environment variable EXPOSWAYDIR");
  struct client_state state = {0};

  FILE *monitor = fopen(getenv("EXPOSWAYMON"), "r");
  ASSERT(monitor != NULL, "failed to open monitor specification file");
  ASSERT(fscanf(monitor, "%d %d", &state.display_width,
                &state.display_height) == 2,
         "incorrect monitor specification file format");
  fclose(monitor);

  DIR *dir;
  struct dirent *entry;
  dir = opendir(getenv("EXPOSWAYDIR"));
  ASSERT(dir != NULL, "failed to open sway expose state directory");

  char line[1024], title[1024], filepath[256];
  state.window = NULL;
  int numwin = 0;
  while ((entry = readdir(dir)) != NULL) {
    char *endptr;
    long node = strtol(entry->d_name, &endptr, 10);
    if (*endptr == '\0') {
      ++numwin;
      sprintf(filepath, "%s%s", getenv("EXPOSWAYDIR"), entry->d_name);
      state.window = realloc(state.window, numwin * sizeof(*state.window));
      ASSERT(state.window != NULL, "failed to reallocate memory for windows");
      struct wl_window *instance = &state.window[numwin - 1];
      instance->node = (int)node;
      FILE *inst = fopen(filepath, "r");
      ASSERT(inst != NULL, "failed to open instance file for window");
      fgets(line, sizeof(line), inst);
      ASSERT(sscanf(line, "%*d,%*d %dx%d %[^\n]", &instance->width,
                    &instance->height, title) == 3,
             "incorrect instance file format");
      instance->title = malloc(strlen(title) + 1);
      ASSERT(instance->title != NULL,
             "failed to allocate memory for window title");
      strcpy(instance->title, title);
      fclose(inst);
    }
  }
  closedir(dir);
  state.window_sum = numwin;

  state.frame_render = false;
  state.focused_window = 0;

  expose_layout_alloc(&state);

  state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  ASSERT(state.xkb_context != NULL, "failed to create xkb context");

  state.wl_display = wl_display_connect(NULL);
  ASSERT(state.wl_display != NULL, "failed to connect to the wayland display");

  state.wl_registry = wl_display_get_registry(state.wl_display);
  wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state);
  wl_display_roundtrip(state.wl_display);

  state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
  ASSERT(state.wl_surface != NULL, "failed to create wayland surface");

  state.xdg_surface =
      xdg_wm_base_get_xdg_surface(state.xdg_wm_base, state.wl_surface);
  ASSERT(state.xdg_surface != NULL, "failed to get xdg surface");
  xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);

  state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
  ASSERT(state.xdg_toplevel != NULL, "failed to get xdg toplevel");
  xdg_toplevel_add_listener(state.xdg_toplevel, &xdg_toplevel_listener, &state);
  xdg_toplevel_set_title(state.xdg_toplevel, "Expose Sway");

  wl_surface_commit(state.wl_surface);

  struct wl_callback *cb = wl_surface_frame(state.wl_surface);
  ASSERT(cb != NULL, "failed to create wayland surface frame callback");
  wl_callback_add_listener(cb, &wl_surface_frame_listener, &state);

  while (!state.closed) {
    if (wl_display_dispatch(state.wl_display) == -1) {
      ASSERT(false, "dispatch display failed");
      break;
    }
  }

  while (numwin-- > 0)
    free(state.window[numwin].title);
  free(state.window);

  return 0;
}
