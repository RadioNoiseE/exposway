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

#define FRAME_CLR 16, 102, 130 /* frame color */
#define FRAME_WDH 1.6          /* frame width */
#define FRAME_SEP 2            /* frame seperation */
#define TITLE_CLR 1, 1, 1      /* title font color */
#define TITLE_SZE 12           /* title font size */
#define DELAY_SEC 0.36         /* grim shot delay */
#define MARGN_RTO 0.07f        /* window-margin factor */
#define COVGT_TOL 0.2f         /* binary search tolerance */
#define EPACK_RTO 0.9f         /* ratio of packing and display */
#define ASSERT(condition, message)                                             \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "Assertion failed: (%s), function %s, line %d.\n",       \
              #condition, __FUNCTION__, __LINE__);                             \
      fprintf(stderr, "Error message: %s.\n", message);                        \
    }                                                                          \
  } while (0)

typedef struct {
  int var1;
  int var2;
} tuple;

struct wl_window {
  int node;
  int width, height;
  int phantom_width, phantom_height;
  int xcr, ycr;
  float scale_factor;
  char *title;
};

struct client_state {
  struct wl_display *wl_display;
  struct wl_registry *wl_registry;
  struct wl_shm *wl_shm;
  struct wl_compositor *wl_compositor;
  struct wl_surface *wl_surface;
  struct wl_seat *wl_seat;
  struct wl_window *wl_window;

  struct xdg_wm_base *xdg_wm_base;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *xdg_toplevel;

  struct xkb_context *xkb_context;
  struct xkb_state *xkb_state;
  int32_t xkb_delay;

  cairo_surface_t *surface;
  cairo_t *cr;

  int display_width, display_height;
  int window_count;
  int window_focused;
  bool frame_draw;
  bool focus_changed;
  bool exit;
  clock_t focus_changed_time;
};

static void randname(char *buf) {
  struct timespec ts;
  ASSERT(clock_gettime(CLOCK_REALTIME, &ts) == 0, "clock_gettime failed");
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
  ASSERT(fd >= 0, "create_shm_file failed");
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

static void nearest_window(struct client_state *state, char dir) {
  clock_t current_time = clock();
  double time_elapsed_ms =
      ((double)(current_time - state->focus_changed_time)) / CLOCKS_PER_SEC *
      1000;

  if (state->focus_changed)
    return;
  else if (time_elapsed_ms < state->xkb_delay)
    return;

  state->focus_changed = true;

  int xori, yori;
  int dist, nwin = -1,
            near = state->display_width * state->display_width +
                   state->display_height * state->display_height;

  switch (dir) {
  case 'u':
    xori = state->wl_window[state->window_focused].xcr +
           state->wl_window[state->window_focused].width *
               state->wl_window[state->window_focused].scale_factor * 0.5;
    for (int i = 0; i < state->window_count; i++) {
      if (state->wl_window[i].xcr < xori &&
          state->wl_window[i].xcr +
                  state->wl_window[i].width * state->wl_window[i].scale_factor >
              xori &&
          state->wl_window[i].ycr <
              state->wl_window[state->window_focused].ycr) {
        int xsep =
            (state->wl_window[i].xcr + state->wl_window[i].width *
                                           state->wl_window[i].scale_factor *
                                           0.5) -
            (state->wl_window[state->window_focused].xcr +
             state->wl_window[state->window_focused].width *
                 state->wl_window[state->window_focused].scale_factor * 0.5);
        int ysep =
            (state->wl_window[i].ycr - state->wl_window[i].height *
                                           state->wl_window[i].scale_factor *
                                           0.5) -
            (state->wl_window[state->window_focused].ycr +
             state->wl_window[state->window_focused].height *
                 state->wl_window[state->window_focused].scale_factor * 0.5);
        dist = xsep * xsep + ysep * ysep;
        if (dist < near) {
          near = dist;
          nwin = i;
        }
      }
    }
    break;
  case 'd':
    xori = state->wl_window[state->window_focused].xcr +
           state->wl_window[state->window_focused].width *
               state->wl_window[state->window_focused].scale_factor * 0.5;
    for (int i = 0; i < state->window_count; i++) {
      if (state->wl_window[i].xcr < xori &&
          state->wl_window[i].xcr +
                  state->wl_window[i].width * state->wl_window[i].scale_factor >
              xori &&
          state->wl_window[i].ycr >
              state->wl_window[state->window_focused].ycr) {
        int xsep =
            (state->wl_window[i].xcr + state->wl_window[i].width *
                                           state->wl_window[i].scale_factor *
                                           0.5) -
            (state->wl_window[state->window_focused].xcr +
             state->wl_window[state->window_focused].width *
                 state->wl_window[state->window_focused].scale_factor * 0.5);
        int ysep =
            (state->wl_window[i].ycr - state->wl_window[i].height *
                                           state->wl_window[i].scale_factor *
                                           0.5) -
            (state->wl_window[state->window_focused].ycr +
             state->wl_window[state->window_focused].height *
                 state->wl_window[state->window_focused].scale_factor * 0.5);
        dist = xsep * xsep + ysep * ysep;
        if (dist < near) {
          near = dist;
          nwin = i;
        }
      }
    }
    break;
  case 'l':
    yori = state->wl_window[state->window_focused].ycr +
           state->wl_window[state->window_focused].height *
               state->wl_window[state->window_focused].scale_factor * 0.5;
    for (int i = 0; i < state->window_count; i++) {
      if (state->wl_window[i].ycr < yori &&
          state->wl_window[i].ycr + state->wl_window[i].height *
                                        state->wl_window[i].scale_factor >
              yori &&
          state->wl_window[i].xcr <
              state->wl_window[state->window_focused].xcr) {
        int xsep =
            (state->wl_window[i].xcr + state->wl_window[i].width *
                                           state->wl_window[i].scale_factor *
                                           0.5) -
            (state->wl_window[state->window_focused].xcr +
             state->wl_window[state->window_focused].width *
                 state->wl_window[state->window_focused].scale_factor * 0.5);
        int ysep =
            (state->wl_window[i].ycr - state->wl_window[i].height *
                                           state->wl_window[i].scale_factor *
                                           0.5) -
            (state->wl_window[state->window_focused].ycr +
             state->wl_window[state->window_focused].height *
                 state->wl_window[state->window_focused].scale_factor * 0.5);
        dist = xsep * xsep + ysep * ysep;
        if (dist < near) {
          near = dist;
          nwin = i;
        }
      }
    }
    break;
  case 'r':
    yori = state->wl_window[state->window_focused].ycr +
           state->wl_window[state->window_focused].height *
               state->wl_window[state->window_focused].scale_factor * 0.5;
    for (int i = 0; i < state->window_count; i++) {
      if (state->wl_window[i].ycr < yori &&
          state->wl_window[i].ycr + state->wl_window[i].height *
                                        state->wl_window[i].scale_factor >
              yori &&
          state->wl_window[i].xcr >
              state->wl_window[state->window_focused].xcr) {
        int xsep =
            (state->wl_window[i].xcr + state->wl_window[i].width *
                                           state->wl_window[i].scale_factor *
                                           0.5) -
            (state->wl_window[state->window_focused].xcr +
             state->wl_window[state->window_focused].width *
                 state->wl_window[state->window_focused].scale_factor * 0.5);
        int ysep =
            (state->wl_window[i].ycr - state->wl_window[i].height *
                                           state->wl_window[i].scale_factor *
                                           0.5) -
            (state->wl_window[state->window_focused].ycr +
             state->wl_window[state->window_focused].height *
                 state->wl_window[state->window_focused].scale_factor * 0.5);
        dist = xsep * xsep + ysep * ysep;
        if (dist < near) {
          near = dist;
          nwin = i;
        }
      }
    }
    break;
  }

  if (nwin > -1 && nwin != state->window_focused)
    state->window_focused = nwin;

  state->focus_changed = false;
  state->focus_changed_time = clock();
}

static void window_focus(int node_id) {
  char focus_command[256];
  snprintf(focus_command, sizeof(focus_command),
           "sleep %f; swaymsg [con_id=%d] focus", DELAY_SEC, node_id);
  pid_t exp_pid = fork();
  ASSERT(exp_pid != 0, "pid fork failed");
  execl("/bin/sh", "sh", "-c", focus_command, (char *)NULL);
}

static void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                               uint32_t format, int32_t fd, uint32_t size) {
  struct client_state *state = data;

  char *map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ASSERT(map_shm != MAP_FAILED, "map_shm mmap failed");

  struct xkb_keymap *keymap = xkb_keymap_new_from_string(
      state->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  ASSERT(keymap != NULL, "xkb_keymap new failed");
  munmap(map_shm, size);
  close(fd);

  state->xkb_state = xkb_state_new(keymap);
  xkb_keymap_unref(keymap);
  ASSERT(state->xkb_state != NULL, "xkb_state new failed");
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
    state->exit = true;
    return;
  case XKB_KEY_Left:
    if (!state->frame_draw) {
      state->frame_draw = true;
      return;
    }
    state->frame_draw = true;
    nearest_window(state, 'l');
    return;
  case XKB_KEY_Right:
    if (!state->frame_draw) {
      state->frame_draw = true;
      return;
    }
    state->frame_draw = true;
    nearest_window(state, 'r');
    return;
  case XKB_KEY_Up:
    if (!state->frame_draw) {
      state->frame_draw = true;
      return;
    }
    state->frame_draw = true;
    nearest_window(state, 'u');
    return;
  case XKB_KEY_Down:
    if (!state->frame_draw) {
      state->frame_draw = true;
      return;
    }
    state->frame_draw = true;
    nearest_window(state, 'd');
    return;
  case XKB_KEY_space:
    window_focus(state->wl_window[state->window_focused].node);
    state->exit = true;
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

int _equate(const void *window1, const void *window2) {
  struct wl_window *win1 = (struct wl_window *)window1;
  struct wl_window *win2 = (struct wl_window *)window2;

  if (win1->height != win2->height)
    return win2->height - win1->height;
  if (win1->width != win2->width)
    return win2->width - win1->width;
  return win2->node - win1->node;
}

int _nfdh(int strip_width, struct wl_window *windows, int window_count) {
  int current_level_height = 0, current_ycr = 0, current_xcr = 0;

  for (int i = 0; i < window_count; i++) {
    windows[i].xcr = current_xcr;
    windows[i].ycr = current_ycr;
    if (current_xcr + windows[i].phantom_width > strip_width) {
      current_xcr = 0;
      current_ycr += current_level_height;
      current_level_height = 0;
      windows[i].xcr = current_xcr;
      windows[i].ycr = current_ycr;
    }
    current_xcr += windows[i].phantom_width;
    if (windows[i].phantom_height > current_level_height)
      current_level_height = windows[i].phantom_height;
  }

  return current_ycr + current_level_height;
}

void _phantom(struct client_state *state) {
  for (int i = 0; i < state->window_count; i++) {
    state->wl_window[i].phantom_width =
        state->wl_window[i].width > state->display_width
            ? state->display_width
            : state->wl_window[i].width;
    state->wl_window[i].phantom_height =
        state->wl_window[i].height > state->display_height
            ? state->display_height
            : state->wl_window[i].height;
    state->wl_window[i].phantom_width +=
        2 * state->wl_window[i].phantom_width * MARGN_RTO;
    state->wl_window[i].phantom_height +=
        2 * state->wl_window[i].phantom_height * MARGN_RTO;
    state->wl_window[i].scale_factor = 1;
  }
}

tuple _pack(struct client_state *state) {
  qsort(state->wl_window, state->window_count, sizeof(struct wl_window),
        _equate);

  const float target_ratio =
      (float)state->display_height / (float)state->display_width;

  int strip_width_min = 0, strip_width_max = 0;
  for (int i = 0; i < state->window_count; i++) {
    strip_width_min = fmax(strip_width_min, state->wl_window[i].width);
    strip_width_max += state->wl_window[i].width;
  }

  tuple res;

  int plmt_high = _nfdh(strip_width_min, state->wl_window, state->window_count);
  float ratio_high = (float)plmt_high / (float)strip_width_min;
  if (ratio_high <= target_ratio) {
    res.var1 = strip_width_min;
    res.var2 = plmt_high;
    return res;
  }

  int plmt_low = _nfdh(strip_width_max, state->wl_window, state->window_count);
  float ratio_low = (float)plmt_low / (float)strip_width_max;
  if (ratio_low >= target_ratio) {
    res.var1 = strip_width_max;
    res.var2 = plmt_low;
    return res;
  }

  while ((float)strip_width_max / (float)strip_width_min > 1 + COVGT_TOL) {
    int strip_width = sqrt(strip_width_min * strip_width_max);
    int plmt_bin = _nfdh(strip_width, state->wl_window, state->window_count);
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

  res.var2 = _nfdh(res.var1, state->wl_window, state->window_count);

  return res;
}

void _refine(tuple pack, struct client_state *state) {
  float width_ratio = (float)pack.var1 / (float)state->display_width;
  float height_ratio = (float)pack.var2 / (float)state->display_height;
  float scale_factor = width_ratio > height_ratio
                           ? state->display_width * EPACK_RTO / pack.var1
                           : state->display_height * EPACK_RTO / pack.var2;

  for (int i = 0, level = 0, boundary = 0, track = 0, height = 0;
       i < state->window_count; i++) {
    if (state->wl_window[i].ycr == level) {
      if (state->wl_window[i].xcr + state->wl_window[i].phantom_width >
          boundary)
        boundary = state->wl_window[i].xcr + state->wl_window[i].phantom_width;
      if (state->wl_window[i].phantom_height > height)
        height = state->wl_window[i].phantom_height;
    } else {
      for (int j = track; j < i; j++) {
        state->wl_window[j].xcr += (pack.var1 - boundary) * 0.5;
        state->wl_window[j].ycr +=
            (height - state->wl_window[j].phantom_height) * 0.5;
      }
      level = state->wl_window[i].ycr;
      boundary = state->wl_window[i].xcr + state->wl_window[i].phantom_width;
      height = state->wl_window[i].phantom_height;
      track = i;
    }
    if (i == state->window_count - 1) {
      for (int j = track; j <= i; j++) {
        state->wl_window[j].xcr += (pack.var1 - boundary) * 0.5;
        state->wl_window[j].ycr +=
            (height - state->wl_window[j].phantom_height) * 0.5;
      }
    }
  }

  for (int i = 0; i < state->window_count; i++) {
    state->wl_window[i].scale_factor *= scale_factor;
    state->wl_window[i].xcr =
        (state->display_width - pack.var1 * state->wl_window[i].scale_factor) *
            0.5 +
        (state->wl_window[i].xcr +
         (state->wl_window[i].phantom_width - state->wl_window[i].width) *
             0.5) *
            state->wl_window[i].scale_factor;
    state->wl_window[i].ycr =
        state->display_height -
        (state->display_height - pack.var2 * state->wl_window[i].scale_factor) *
            0.5 -
        (state->wl_window[i].ycr + state->wl_window[i].phantom_height -
         (state->wl_window[i].phantom_height - state->wl_window[i].height) *
             0.5) *
            state->wl_window[i].scale_factor;
  }
}

static void expose_layout_alloc(struct client_state *state) {
  _phantom(state);
  _refine(_pack(state), state);
}

static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
  wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

static void _plot(struct client_state *state, int n) {
  char imagepath[256];
  snprintf(imagepath, sizeof(imagepath), "%s%d.png", getenv("EXPOSWAYDIR"),
           state->wl_window[n].node);

  cairo_surface_t *image = cairo_image_surface_create_from_png(imagepath);
  ASSERT(image != NULL, "failed to create cairo image surface");
  cairo_save(state->cr);

  cairo_translate(state->cr, state->wl_window[n].xcr, state->wl_window[n].ycr);
  cairo_scale(state->cr, state->wl_window[n].scale_factor,
              state->wl_window[n].scale_factor);

  cairo_set_source_surface(state->cr, image, 0, 0);
  cairo_paint(state->cr);

  if (state->frame_draw && state->window_focused == n) {
    cairo_set_source_rgb(state->cr, FRAME_CLR);
    cairo_set_line_width(state->cr,
                         FRAME_WDH / state->wl_window[n].scale_factor);
    cairo_rectangle(state->cr, -FRAME_WDH / state->wl_window[n].scale_factor,
                    -FRAME_WDH / state->wl_window[n].scale_factor,
                    state->wl_window[state->window_focused].width +
                        FRAME_WDH * 2 / state->wl_window[n].scale_factor,
                    state->wl_window[state->window_focused].height +
                        FRAME_WDH * 2 / state->wl_window[n].scale_factor);
    cairo_stroke(state->cr);
  }

  cairo_restore(state->cr);
  cairo_surface_destroy(image);
}

static void _title(struct client_state *state, int n) {
  PangoFontDescription *font_description;
  font_description = pango_font_description_new();
  pango_font_description_set_family(font_description, "monospace");
  pango_font_description_set_weight(font_description, PANGO_WEIGHT_NORMAL);
  pango_font_description_set_absolute_size(font_description,
                                           TITLE_SZE * PANGO_SCALE);

  PangoLayout *layout;
  layout = pango_cairo_create_layout(state->cr);
  pango_layout_set_font_description(layout, font_description);
  pango_layout_set_text(layout, state->wl_window[n].title, -1);

  PangoRectangle extends;
  pango_layout_get_pixel_extents(layout, NULL, &extends);

  cairo_set_source_rgb(state->cr, TITLE_CLR);
  cairo_move_to(
      state->cr,
      state->wl_window[n].xcr +
          (state->wl_window[n].width * state->wl_window[n].scale_factor -
           extends.width) /
              2,
      state->wl_window[n].ycr +
          state->wl_window[n].height * state->wl_window[n].scale_factor +
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
  ASSERT(pool != NULL, "wl_shm_pool create failed");

  struct wl_buffer *buffer = wl_shm_pool_create_buffer(
      pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
  ASSERT(buffer != NULL, "wl_buffer create failed");
  wl_shm_pool_destroy(pool);
  close(fd);

  state->surface = cairo_image_surface_create_for_data(
      data, CAIRO_FORMAT_ARGB32, width, height, stride);
  ASSERT(state->surface != NULL, "cairo_image_surface create failed");
  state->cr = cairo_create(state->surface);

  for (int n = 0; n < state->window_count; n++) {
    _plot(state, n);
    _title(state, n);
  }

  cairo_destroy(state->cr);
  cairo_surface_destroy(state->surface);

  munmap(data, size);

  wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
  return buffer;
}

static const struct wl_callback_listener wl_surface_frame_listener;

static void wl_surface_frame_done(void *data, struct wl_callback *cb,
                                  uint32_t time) {
  struct client_state *state = data;

  wl_callback_destroy(cb);

  struct wl_buffer *buffer = draw_cairo(state);
  ASSERT(buffer != NULL, "draw_cairo failed");
  wl_surface_attach(state->wl_surface, buffer, 0, 0);
  wl_surface_damage(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);

  cb = wl_surface_frame(state->wl_surface);
  wl_callback_add_listener(cb, &wl_surface_frame_listener, state);

  wl_surface_commit(state->wl_surface);
}

static const struct wl_callback_listener wl_surface_frame_listener = {
    .done = wl_surface_frame_done,
};

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
  state->exit = true;

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
  ASSERT(buffer != NULL, "draw_cairo failed");
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

static void registry_global(void *data, struct wl_registry *wl_registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  struct client_state *state = data;

  if (strcmp(interface, wl_shm_interface.name) == 0) {
    state->wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
    ASSERT(state->wl_shm != NULL, "wl_shm bind failed");
  } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
    state->wl_compositor =
        wl_registry_bind(wl_registry, name, &wl_compositor_interface, 4);
    ASSERT(state->wl_compositor != NULL, "wl_compositor bind failed");
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    state->xdg_wm_base =
        wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, 1);
    ASSERT(state->xdg_wm_base != NULL, "xdg_wm_base bind failed");
    xdg_wm_base_add_listener(state->xdg_wm_base, &xdg_wm_base_listener, state);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    state->wl_seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 8);
    ASSERT(state->wl_seat != NULL, "wl_seat bind failed");
    struct wl_keyboard *keyboard = wl_seat_get_keyboard(state->wl_seat);
    ASSERT(keyboard != NULL, "wl_keyboard bind failed");
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
  ASSERT(getenv("EXPOSWAYMON") != NULL, "curcial environment variable unset");
  ASSERT(getenv("EXPOSWAYDIR") != NULL, "crucial environment variable unset");
  struct client_state state = {0};

  FILE *monitor = fopen(getenv("EXPOSWAYMON"), "r");
  ASSERT(monitor != NULL, "monitor specification file open failed");
  ASSERT(fscanf(monitor, "%d %d", &state.display_width,
                &state.display_height) == 2,
         "monitor specification file format incorrect");
  fclose(monitor);

  DIR *dir;
  struct dirent *entry;
  dir = opendir(getenv("EXPOSWAYDIR"));
  ASSERT(dir != NULL, "snapshot directory open failed");

  char line[1024], title[1024], filepath[256];
  state.wl_window = NULL;
  int numwin = 0;
  while ((entry = readdir(dir)) != NULL) {
    char *endptr;
    long node = strtol(entry->d_name, &endptr, 10);
    if (*endptr == '\0') {
      ++numwin;
      sprintf(filepath, "%s%s", getenv("EXPOSWAYDIR"), entry->d_name);
      state.wl_window =
          realloc(state.wl_window, numwin * sizeof(*state.wl_window));
      ASSERT(state.wl_window != NULL, "reallocate memory failed");
      struct wl_window *instance = &state.wl_window[numwin - 1];
      instance->node = (int)node;
      FILE *inst = fopen(filepath, "r");
      ASSERT(inst != NULL, "instance file open failed");
      fgets(line, sizeof(line), inst);
      ASSERT(sscanf(line, "%*d,%*d %dx%d %[^\n]", &instance->width,
                    &instance->height, title) == 3,
             "instance file format incorrect");
      instance->title = malloc(strlen(title) + 1);
      ASSERT(instance->title != NULL,
             "allocate memory for window title failed");
      strcpy(instance->title, title);
      fclose(inst);
    }
  }
  closedir(dir);
  state.window_count = numwin;

  state.frame_draw = false;
  state.window_focused = 0;

  expose_layout_alloc(&state);

  state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  ASSERT(state.xkb_context != NULL, "xkb_context new failed");

  state.wl_display = wl_display_connect(NULL);
  ASSERT(state.wl_display != NULL, "wl_display connect failed");

  state.wl_registry = wl_display_get_registry(state.wl_display);
  wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state);
  wl_display_roundtrip(state.wl_display);

  state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
  ASSERT(state.wl_surface != NULL, "wl_surface create failed");

  state.xdg_surface =
      xdg_wm_base_get_xdg_surface(state.xdg_wm_base, state.wl_surface);
  ASSERT(state.xdg_surface != NULL, "xdg_surface assign failed");
  xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);

  state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
  ASSERT(state.xdg_toplevel != NULL, "xdg_toplevel assign failed");
  xdg_toplevel_add_listener(state.xdg_toplevel, &xdg_toplevel_listener, &state);
  xdg_toplevel_set_title(state.xdg_toplevel, "Sway Expose");

  wl_surface_commit(state.wl_surface);

  struct wl_callback *cb = wl_surface_frame(state.wl_surface);
  ASSERT(cb != NULL, "wl_callback hook failed");
  wl_callback_add_listener(cb, &wl_surface_frame_listener, &state);

  while (!state.exit) {
    if (wl_display_dispatch(state.wl_display) == -1) {
      ASSERT(false, "wl_display dispatch failed");
      break;
    }
  }

  while (numwin-- > 0)
    free(state.wl_window[numwin].title);
  free(state.wl_window);

  return 0;
}
