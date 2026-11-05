/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <SDL2/SDL.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include "mydvi.h"
#include "providers.h"
#include "renderer.h"
#include "engine.h"
#include "driver.h"
#include "synctex.h"
#include "vstack.h"
#include "prot_parser.h"
#include "editor.h"
#include "base64.h"

struct persistent_state *pstate;

static void schedule_event(enum custom_events ev)
{
  pstate->schedule_event(ev);
}

static bool should_reload_binary(void)
{
  return pstate->should_reload_binary();
}

#ifdef __APPLE__
# define st_time(a) st_##a##timespec
#else
# define st_time(a) st_##a##tim
#endif

static bool is_more_recent(uint64_t *time, char *candidate)
{
  struct stat st;
  if (stat(candidate, &st) == 0 && st.st_time(c).tv_sec > *time)
  {
    *time = st.st_time(c).tv_sec;
    return 1;
  }
  return 0;
}

static void set_more_recent(uint64_t *time, char **result, char *candidate)
{
  if (is_more_recent(time, candidate))
    *result = candidate;
}

static void find_engine(char engine_path[4096], const char *exec_path)
{
  strcpy(engine_path, exec_path);
  char *basename = NULL;
  for (int i = 0; i < 4096 && engine_path[i]; ++i)
    if (engine_path[i] == '/')
      basename = engine_path + i + 1;
  uint64_t time = 0;
  if (basename)
  {
    strcpy(basename, "texpresso-xetex");
    if (!is_more_recent(&time, engine_path))
      strcpy(engine_path, "texpresso-xetex");
  }
}

/* UI state */

enum ui_mouse_status {
  UI_MOUSE_NONE,
  UI_MOUSE_SELECT,
  UI_MOUSE_MOVE,
};

typedef struct {
  txp_engine *eng;
  txp_renderer *doc_renderer;
  SDL_Renderer *sdl_renderer;
  SDL_Window *window;

  int page;
  int need_synctex;
  int zoom;

  // Mouse input state
  int last_mouse_x, last_mouse_y;
  uint32_t last_click_ticks;
  enum ui_mouse_status mouse_status;
  bool advancing;

  // Export of the displayed page as a PNG (render-to), for an editor that
  // shows it itself: the file, the box to fit it in, and the mapping of the
  // last image written (for click-at).
  char *export_path;
  int export_width, export_height;
  bool export_valid;
  float export_scale;
  fz_rect export_bounds;
  fz_point export_origin;       // the page's top left corner in the image
  fz_display_list *export_dl;   // the page of the last image, for select
  // The selection highlighted in the image (select), on page sel_page
  fz_quad sel_quads[512];
  int sel_count, sel_page;
  // Thumbnails of every page (thumbnails): TeX goes on to the end meanwhile
  bool want_all;
} ui_state;

/* UI rendering */

static float zoom_factor(int count)
{
  return expf((float)count / 5000.0f);
}

static void render(fz_context *ctx, ui_state *ui)
{
  SDL_SetRenderDrawColor(ui->sdl_renderer, 0, 0, 0, 255);
  SDL_RenderClear(ui->sdl_renderer);
  txp_renderer_render(ctx, ui->doc_renderer);
  SDL_RenderPresent(ui->sdl_renderer);
}

struct repaint_on_resize_env
{
  fz_context *ctx;
  ui_state *ui;
};

static int repaint_on_resize(void *data, SDL_Event *event)
{
  struct repaint_on_resize_env *env = data;
  if (event->type == SDL_WINDOWEVENT &&
      event->window.event == SDL_WINDOWEVENT_RESIZED &&
      SDL_GetWindowFromID(event->window.windowID) == env->ui->window)
  {
    render(env->ctx, env->ui);
  }
  return 0;
}

/* Document processing */

static bool need_advance(fz_context *ctx, ui_state *ui)
{
  if (send(get_status, ui->eng) != DOC_RUNNING)
    return false;

  if (send(is_finishing, ui->eng))
    return true;

  int need = ui->want_all || send(page_count, ui->eng) <= ui->page;

  if (!need)
  {
    fz_buffer *buf;
    synctex_t *stx = send(synctex, ui->eng, &buf);
    need =
      (ui->need_synctex && synctex_page_count(stx) <= ui->page) ||
      synctex_has_target(stx);
  }

  return need;
}

static bool advance_engine(fz_context *ctx, ui_state *ui)
{
  bool need = need_advance(ctx, ui);
  if (!need && ui->advancing)
    editor_flush();
  ui->advancing = need;
  if (!need)
    return false;

  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  int steps = 10;
  while (need)
  {
    if (!send(step, ui->eng, ctx, false))
      break;

    steps -= 1;
    need = need_advance(ctx, ui);

    if (steps == 0)
    {
      steps = 10;

      struct timespec curr;
      clock_gettime(CLOCK_MONOTONIC, &curr);

      int delta =
        (curr.tv_sec - start.tv_sec) * 1000 * 1000 * 1000 +
        (curr.tv_nsec - start.tv_nsec);

      if (delta > 5000000)
        break;
    }
  }
  return need;
}

static fz_point get_scale_factor(SDL_Window *window)
{
  int ww, wh, pw, ph;
  SDL_GetWindowSize(window, &ww, &wh);

#if SDL_VERSION_ATLEAST(2, 0, 26)
  SDL_GetWindowSizeInPixels(window, &pw, &ph);
#else
  SDL_GetRendererOutputSize(SDL_GetRenderer(window), &pw, &ph);
#endif

  return fz_make_point(ww != 0 ? (float)pw / ww : 1,
                       wh != 0 ? (float)ph / wh : 1);
}

/* UI events */

static void ui_mouse_down(struct persistent_state *ps, ui_state *ui, int x, int y, bool ctrl)
{
  if (ctrl)
    ui->mouse_status = UI_MOUSE_MOVE;
  else
  {
    ui->mouse_status = UI_MOUSE_SELECT;
    fz_point scale = get_scale_factor(ui->window);
    fz_point p = fz_make_point(scale.x * x, scale.y * y);

    uint32_t ticks = SDL_GetTicks();

    bool double_click = ticks - ui->last_click_ticks < 500 &&
                        abs(ui->last_mouse_x - x) < 30 && abs(ui->last_mouse_y - y) < 30;

    bool diff;

    if (double_click)
    {
      diff = txp_renderer_select_word(ps->ctx, ui->doc_renderer, p);
    }
    else
    {
      diff = txp_renderer_start_selection(ps->ctx, ui->doc_renderer, p);
      diff = txp_renderer_select_char(ps->ctx, ui->doc_renderer, p) || diff;
      ui->last_click_ticks = ticks;

      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      if (stx && buf)
      {
        fz_point pt = txp_renderer_screen_to_document(ps->ctx, ui->doc_renderer, p);
        float f = 1 / send(scale_factor, ui->eng);
        // pt.x -= 72;
        // pt.y -= 72;
        fprintf(stderr, "click: (%f,%f) mapped:(%f,%f)\n",
                pt.x, pt.y, f * pt.x, f * pt.y);
        synctex_scan(ps->ctx, stx, buf, ps->doc_path, ui->page, f * pt.x, f * pt.y);
      }
    }

    if (diff)
      schedule_event(RENDER_EVENT);
  }

  ui->last_mouse_x = x;
  ui->last_mouse_y = y;
}

static void ui_mouse_up(ui_state *ui)
{
  ui->mouse_status = UI_MOUSE_NONE;
}

static void ui_mouse_move(fz_context *ctx, ui_state *ui, int x, int y)
{
  fz_point scale = get_scale_factor(ui->window);
  switch (ui->mouse_status)
  {
    case UI_MOUSE_NONE:
      break;

    case UI_MOUSE_SELECT:
    {
      fz_point p = fz_make_point(scale.x * x, scale.y * y);
      // fprintf(stderr, "drag sel\n");
      if (txp_renderer_drag_selection(ctx, ui->doc_renderer, p))
        schedule_event(RENDER_EVENT);
      break;
    }

    case UI_MOUSE_MOVE:
    {
      txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
      int dx = x - ui->last_mouse_x;
      int dy = y - ui->last_mouse_y;
      if (dx != 0 || dy != 0)
      {
        config->pan.x += scale.x * dx;
        config->pan.y += scale.y * dy;
        ui->last_mouse_x = x;
        ui->last_mouse_y = y;
        schedule_event(RENDER_EVENT);
      }
      break;
    }
  }
}

static void ui_mouse_wheel(fz_context *ctx, ui_state *ui, float dx, float dy, int mousex, int mousey, bool ctrl, int timestamp)
{
  fz_point scale = get_scale_factor(ui->window);

  if (ui->mouse_status != UI_MOUSE_NONE)
    return;

  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);

  if (ctrl)
  {
    SDL_FRect rect;
    if (dy != 0 && txp_renderer_page_position(ctx, ui->doc_renderer, &rect, NULL, NULL))
    {
      ui->zoom = fz_maxi(ui->zoom + dy * 100, 0);
      int ww, wh;
      SDL_GetWindowSize(ui->window, &ww, &wh);
      float mx = (mousex - ww / 2.0f) * scale.x;
      float my = (mousey - wh / 2.0f) * scale.y;
      float of = config->zoom, nf = zoom_factor(ui->zoom);
      config->pan.x = mx + nf * ((config->pan.x - mx) / of);
      config->pan.y = my + nf * ((config->pan.y - my) / of);
      config->zoom = nf;
      schedule_event(RENDER_EVENT);
    }
  }
  else
  {
    (void)timestamp;
    float x = scale.x * dx * 5;
    float y = scale.y * dy * 5;
    config->pan.x -= x;
    config->pan.y += y;
    // fprintf(stderr, "wheel pan: (%.02f, %.02f) raw:(%.02f, %.02f)\n", x, y, dx, dy);
    schedule_event(RENDER_EVENT);
  }
}

/* Stdin polling */

static int SDLCALL poll_stdin_thread_main(void *data)
{
  int *pipes = data;
  char c;
  int n;

  while (1)
  {
    n = read(pipes[0], &c, 1);
    if (n == -1)
    {
      if (errno == EINTR)
        continue;
      return 1;
    }

    if (n == 0)
      return 1;

    if (c == 'q')
      return 0;

    if (c != 'c')
      abort();

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLRDNORM;
    fds[0].revents = 0;
    fds[1].fd = pipes[0];
    fds[1].events = POLLRDNORM;
    fds[1].revents = 0;

    while (1)
    {
      n = poll(fds, 2, -1);
      if (n == -1)
      {
        if (errno == EINTR)
          continue;
        return 1;
      }
      if ((fds[0].revents & POLLRDNORM) != 0)
        schedule_event(STDIN_EVENT);
      break;
    }
  }
}

static bool poll_stdin(void)
{
  struct pollfd fd;
  fd.fd = STDIN_FILENO;
  fd.events = POLLRDNORM;
  fd.revents = 0;
  return (poll(&fd, 1, 0) == 1) && ((fd.revents & POLLRDNORM) != 0);
}

static void wakeup_poll_thread(int poll_stdin_pipe[2], char c)
{
  while (1)
  {
    int n = write(poll_stdin_pipe[1], &c, 1);
    if (n == 1)
      break;
    if (n == -1 && errno == EINTR)
      continue;
    perror("write(poll_stdin_pipe, _, _)");
    break;
  }
}

/* Command interpreter */

enum pan_to { PAN_TO_TOP, PAN_TO_BOTTOM };
static void pan_to(fz_context *ctx, ui_state *ui, enum pan_to to)
{
  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
  txp_renderer_bounds bounds;
  if (txp_renderer_page_bounds(ctx, ui->doc_renderer, &bounds))
    config->pan.y = (to == PAN_TO_TOP) ? bounds.pan_interval.y : -bounds.pan_interval.y;
  // a helper function for other UI actions, so no event scheduled
}

static void previous_page(fz_context *ctx, ui_state *ui, bool pan)
{
  synctex_set_target(send(synctex, ui->eng, NULL), 0, NULL, 0);
  if (ui->page > 0)
  {
    ui->page -= 1;

    int page_count = send(page_count, ui->eng);
    if (page_count > 0 && ui->page >= page_count &&
        send(get_status, ui->eng) == DOC_TERMINATED)
      ui->page = page_count - 1;

    // FIXME: technically, this is slightly incorrect.
    // The new page has not been loaded yet, so we compute the coordinate with
    // respect to the page currently displayed. Most of the time, pages have the
    // same dimension, so this is fine.
    if (pan)
      pan_to(ctx, ui, PAN_TO_BOTTOM);

    schedule_event(RELOAD_EVENT);
  }
}

static void next_page(fz_context *ctx, ui_state *ui, bool pan)
{
  synctex_set_target(send(synctex, ui->eng, NULL), 0, NULL, 0);
  ui->page += 1;
  // FIXME: Same remark as in previous_page.
  if (pan)
    pan_to(ctx, ui, PAN_TO_TOP);
  schedule_event(RELOAD_EVENT);
}

static void ui_pan_x(fz_context *ctx, ui_state *ui, float factor)
{
  fz_point scale = get_scale_factor(ui->window);

  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);

  txp_renderer_bounds bounds;
  if (!txp_renderer_page_bounds(ctx, ui->doc_renderer, &bounds))
    return;

  float delta = bounds.window_size.x * scale.x * factor;

  config->pan.x += delta;
  schedule_event(RENDER_EVENT);
}

static void ui_pan(fz_context *ctx, ui_state *ui, float factor)
{
  fz_point scale = get_scale_factor(ui->window);

  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);

  txp_renderer_bounds bounds;
  if (!txp_renderer_page_bounds(ctx, ui->doc_renderer, &bounds))
    return;

  float delta = bounds.window_size.y * scale.y * factor;
  float range = bounds.pan_interval.y < 0 ? 0 : bounds.pan_interval.y;

  //fprintf(stderr, "ui_pan: factor:%.02f delta:%.02f current:%.02f range:%.02f\n",
  //        factor, delta, config->pan.y, range);

  if (config->pan.y == -range && factor < 0)
  {
    next_page(ctx, ui, 1);
    return;
  }

  if (config->pan.y == range && factor > 0)
  {
    previous_page(ctx, ui, 1);
    return;
  }

  config->pan.y += delta;
  schedule_event(RENDER_EVENT);
}

static const char *relative_path(const char *path, const char *dir, int *go_up)
{
  const char *rel_path = path, *dir_path = dir;

  // Skip common parts
  while (*rel_path && *rel_path == *dir_path)
  {
    if (*rel_path == '/')
    {
      while (*rel_path == '/') rel_path += 1;
      while (*dir_path == '/') dir_path += 1;
    }
    else
    {
      rel_path += 1;
      dir_path += 1;
    }
  }

  // Go back to last directory separator
  if (*rel_path && *dir_path)
  {
    rel_path -= 1;
    dir_path -= 1;
    while (path < rel_path && *rel_path != '/')
    {
      rel_path -= 1;
      dir_path -= 1;
    }
    if (*rel_path == '/')
    {
      if (*dir_path != '/') abort();
      rel_path += 1;
      dir_path += 1;
    }
  }

  // Count number of '../'
  *go_up = 0;
  if (*dir_path)
  {
    *go_up = 1;
    while (*dir_path)
    {
      if (*dir_path == '/')
      {
        *go_up += 1;
        while (*dir_path == '/')
          dir_path += 1;
      }
      else
        dir_path += 1;
    }
  }

  while (*rel_path == '/')
    rel_path += 1;

  return rel_path;
}

static int find_diff(const fz_buffer *buf, const void *data, int size)
{
  const unsigned char *ptr = data;
  int i, len = fz_mini(buf->len, size);
  for (i = 0; i < len && buf->data[i] == ptr[i]; ++i);
  fprintf(stderr, "i:%d len:%d size:%d\n", i, (int)buf->len, size);
  return i;
}

#include "utf_mapping.h"

static void realize_change(struct persistent_state *ps,
                           ui_state *ui,
                           struct editor_change *op)
{
  int go_up = 0;
  const char *path = relative_path(op->path, ps->doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] change %s: file has a different root, skipping\n", path);
    return;
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] change %s: file not found, skipping\n", path);
    return;
  }

  fz_buffer *b = e->edit_data;
  if (!b)
  {
    fprintf(stderr, "[command] change %s: file not opened, skipping\n", path);
    return;
  }

  int offset = op->span.offset, remove = op->span.remove, length = op->length;

  if (op->base == BASE_LINE)
  {
    // Compute byte offsets from line offsets
    int line = offset, count = remove;

    offset = remove = 0;

    uint8_t *p = b->data;
    size_t len = b->len;

    while (line > 0 && offset < len)
    {
      if (p[offset] == '\n')
        line -= 1;
      offset++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change line %s: invalid line number, skipping\n", path);
      return;
    }

    remove = offset;
    while (count > 0 && remove < len)
    {
      if (p[remove] == '\n')
        count -= 1;
      remove++;
    }

    if (count > 1)
    {
      fprintf(stderr, "[command] change line %s: invalid line count, skipping\n", path);
      return;
    }

    remove -= offset;
  }
  else if (op->base == BASE_RANGE)
  {
    // Compute byte offsets from line offsets
    int line = op->range.start_line;
    offset = remove = 0;

    uint8_t *p = b->data;
    size_t len = b->len;

    while (line > 0 && offset < len)
    {
      if (p[offset] == '\n')
        line -= 1;
      offset++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change range %s: invalid start line, skipping\n", path);
      return;
    }

    int start_char_offset = utf16_to_utf8_offset(p + offset, p + len, op->range.start_char);
    if (start_char_offset == -1)
    {
      fprintf(stderr, "[command] change range %s: invalid start char, skipping\n", path);
      return;
    }

    remove = offset;
    offset += start_char_offset;

    line = op->range.end_line - op->range.start_line;
    if (line < 0)
    {
      fprintf(stderr, "[command] change range %s: invalid end line, skipping\n", path);
      return;
    }

    while (line > 0 && remove < len)
    {
      if (p[remove] == '\n')
        line -= 1;
      remove++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change range %s: invalid end line, skipping\n", path);
      return;
    }

    int end_char_offset = utf16_to_utf8_offset(p + remove, p + len, op->range.end_char);
    if (end_char_offset == -1)
    {
      fprintf(stderr, "[command] change range %s: invalid end char, skipping\n", path);
      return;
    }

    remove += end_char_offset;
    remove -= offset;
  }

  if (remove < 0 || offset < 0 || offset + remove > b->len)
  {
    fprintf(stderr, "[command] change %s: invalid range, skipping\n", path);
    return;
  }

  if (b->len - remove + length > b->cap)
    fz_resize_buffer(ps->ctx, b, b->len - remove + length + 128);

  memmove(b->data + offset + length, b->data + offset + remove,
          b->len - offset - remove);

  b->len = b->len - remove + length;

  memmove(b->data + offset, op->data, length);

  fprintf(stderr, "[command] change %s: changed offset %d\n", path, offset);
  send(notify_file_changes, ui->eng, ps->ctx, e, offset);
}

#define BUFFERED_OPS 64
#define BUFFERED_CHARS 4096
#define T_IDLE_MS 500
#define MAX_RERUNS 5

struct {
  char buffer[BUFFERED_CHARS];
  int cursor;
  struct editor_change op[BUFFERED_OPS];
  int count;
} delayed_changes = {0,};

static void flush_changes(struct persistent_state *ps,
                          ui_state *ui)
{
  int count = delayed_changes.count;
  if (count)
  {
    delayed_changes.count = 0;
    delayed_changes.cursor = 0;
    for (int i = 0; i < count; ++i)
    {
      struct editor_change *op = &delayed_changes.op[i];
      realize_change(ps, ui, op);
    }
  }
}

static void interpret_change(struct persistent_state *ps,
                             ui_state *ui,
                             struct editor_change *op)
{
  int plen = strlen(op->path);
  int page_count = send(page_count, ui->eng);
  int cursor = delayed_changes.cursor;

  if ((page_count == ui->page - 2 || page_count == ui->page - 1) &&
      send(get_status, ui->eng) == DOC_RUNNING &&
      delayed_changes.count < BUFFERED_OPS &&
      cursor + plen + 1 + op->length <= BUFFERED_CHARS)
  {
    char *op_path = delayed_changes.buffer + cursor;
    memcpy(op_path, op->path, plen + 1);
    cursor += plen + 1;
    char *op_data = delayed_changes.buffer + cursor;
    memcpy(op_data, op->data, op->length);
    cursor += op->length;
    delayed_changes.cursor = cursor;

    delayed_changes.op[delayed_changes.count] = *op;
    delayed_changes.op[delayed_changes.count].path = op_path;
    delayed_changes.op[delayed_changes.count].data = op_data;
    delayed_changes.count += 1;
  }
  else
  {
    flush_changes(ps, ui);
    realize_change(ps, ui, op);
  }
}

static void interpret_open(struct persistent_state *ps,
                           ui_state *ui,
                           const char *path,
                           const void *data,
                           int size)
{
  if (path[0] == '/')
  {
    int go_up = 0;
    path = relative_path(path, ps->doc_path, &go_up);
    if (go_up > 0)
    {
      fprintf(stderr, "[command] open %s: file has a different root, skipping\n", path);
      return;
    }
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] open %s: file not found, skipping\n", path);
    return;
  }

  flush_changes(ps, ui);

  int changed = -1;
  bool had_edit_data = (e->edit_data != NULL);

  if (e->edit_data)
  {
    fprintf(stderr, "[command] open %s: known file, updating\n", path);
    changed = find_diff(e->edit_data, data, size);
    if (e->edit_data->cap < size)
      fz_resize_buffer(ps->ctx, e->edit_data, size + 128);
    e->edit_data->len = size;
    memcpy(e->edit_data->data, data, size);
  }
  else
  {
    fprintf(stderr, "[command] open %s: new file\n", path);
    e->edit_data = fz_new_buffer_from_copied_data(ps->ctx, data, size);
    if (e->fs_data)
      changed = find_diff(e->fs_data, data, size);
    else if (e->seen >= 0)
      changed = 0;
  }

  if (changed >= 0)
  {
    if (e->promised && !had_edit_data)
      fprintf(stderr, "[command] open %s: resolving deferred query\n", path);
    else
    {
      fprintf(stderr, "[command] open %s: changed offset is %d\n", path, changed);
      send(notify_file_changes, ui->eng, ps->ctx, e, changed);
    }
  }
}

static void interpret_close(struct persistent_state *ps,
                            ui_state *ui,
                            const char *path)
{
  int go_up = 0;
  path = relative_path(path, ps->doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] close %s: file has a different root, skipping\n", path);
    return;
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] close %s: file not found, skipping\n", path);
    return;
  }

  if (!e->edit_data)
  {
    fprintf(stderr, "[command] close %s: file not opened, skipping\n", path);
    return;
  }

  flush_changes(ps, ui);

  int changed = 0;

  if (e->fs_data)
    changed = find_diff(e->fs_data, e->edit_data->data, e->edit_data->len);

  fz_drop_buffer(ps->ctx, e->edit_data);
  e->edit_data = NULL;

  fprintf(stderr, "[command] close %s: closing, changed offset %d\n", path,
          changed);

  send(notify_file_changes, ui->eng, ps->ctx, e, changed);
}

static uint32_t convert_color(fz_context *ctx, vstack *stack, float frgb[3])
{
  uint8_t rgb[3];

  for (int i = 0; i < 3; ++i)
    rgb[i] = fz_clampi(frgb[i] * 255.0 + 0.5, 0, 255) & 0xFF;

  return (rgb[0] << 16) | (rgb[1] << 8) | (rgb[2]);
}

// The colours the window paints with (the editor's theme, maybe inverted):
// ink goes to fg, paper to bg, as in the renderer.
static void export_recolor(fz_context *ctx, ui_state *ui, fz_pixmap *pix)
{
  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
  uint32_t bg = 0xFFFFFF, fg = 0x000000;
  if (config->themed_color)
  {
    bg = config->background_color & 0xFFFFFF;
    fg = config->foreground_color & 0xFFFFFF;
  }
  if (config->invert_color)
  {
    uint32_t tmp = fg;
    fg = bg;
    bg = tmp;
  }
  if (bg == 0xFFFFFF && fg == 0x000000)
    return;

  uint8_t ink[3] = {(fg >> 16) & 0xFF, (fg >> 8) & 0xFF, fg & 0xFF};
  uint8_t paper[3] = {(bg >> 16) & 0xFF, (bg >> 8) & 0xFF, bg & 0xFF};
  uint8_t *samples = fz_pixmap_samples(ctx, pix);
  int stride = fz_pixmap_stride(ctx, pix);
  int w = fz_pixmap_width(ctx, pix), h = fz_pixmap_height(ctx, pix);
  for (int y = 0; y < h; ++y)
  {
    uint8_t *p = samples + stride * y;
    for (int x = 0; x < w; ++x, p += 3)
      for (int c = 0; c < 3; ++c)
        p[c] = ink[c] + (p[c] * (paper[c] - ink[c])) / 255;
  }
}

// Tint the selected text (sel_quads, page coordinates) towards the ink
// colour, a third of the way, so it reads as highlighted on any theme.
static void export_highlight(fz_context *ctx, ui_state *ui, fz_pixmap *pix, fz_matrix ctm)
{
  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
  uint32_t fg = config->themed_color ? config->foreground_color & 0xFFFFFF : 0;
  uint8_t ink[3] = {(fg >> 16) & 0xFF, (fg >> 8) & 0xFF, fg & 0xFF};
  uint8_t *samples = fz_pixmap_samples(ctx, pix);
  int stride = fz_pixmap_stride(ctx, pix);
  int w = fz_pixmap_width(ctx, pix), h = fz_pixmap_height(ctx, pix);
  // Mark first, tint once: the rectangles of adjacent lines overlap
  uint8_t *mask = fz_calloc(ctx, (size_t)w * h, 1);
  for (int i = 0; i < ui->sel_count; ++i)
  {
    fz_irect r = fz_irect_from_rect(
        fz_transform_rect(fz_rect_from_quad(ui->sel_quads[i]), ctm));
    int x0 = fz_maxi(r.x0, 0), y0 = fz_maxi(r.y0, 0);
    int x1 = fz_mini(r.x1, w), y1 = fz_mini(r.y1, h);
    for (int y = y0; y < y1; ++y)
      if (x1 > x0)
        memset(mask + (size_t)y * w + x0, 1, x1 - x0);
  }
  for (int y = 0; y < h; ++y)
  {
    uint8_t *p = samples + stride * y, *m = mask + (size_t)y * w;
    for (int x = 0; x < w; ++x, p += 3)
      if (m[x])
        for (int c = 0; c < 3; ++c)
          p[c] = p[c] + (ink[c] - p[c]) / 3;
  }
  fz_free(ctx, mask);
}

static int sixel_bucket(const uint8_t *p)
{
  return ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3);
}

struct sixel_bucket_count { int count, bucket; };

static int sixel_bucket_cmp(const void *a, const void *b)
{
  int ca = ((const struct sixel_bucket_count *)a)->count;
  int cb = ((const struct sixel_bucket_count *)b)->count;
  return (cb > ca) - (cb < ca);
}

static void sixel_run(FILE *f, int ch, int n)
{
  if (n > 3)
    fprintf(f, "!%d%c", n, ch);
  else
    while (n-- > 0)
      fputc(ch, f);
}

// Write PIX (RGB, no alpha) as a Sixel picture, for an editor in a
// terminal.  Colours are read at 5 bits a channel; the 256 most frequent
// make the palette (a page is mostly paper, ink and their blends), each
// drawn in the mean of its pixels, and every pixel takes the nearest.
static void save_pixmap_as_sixel(fz_context *ctx, fz_pixmap *pix, const char *path)
{
  enum { BUCKETS = 1 << 15, COLORS = 256 };
  int w = fz_pixmap_width(ctx, pix), h = fz_pixmap_height(ctx, pix);
  int stride = fz_pixmap_stride(ctx, pix);
  const uint8_t *samples = fz_pixmap_samples(ctx, pix);
  int *count = NULL, *map = NULL;
  struct sixel_bucket_count *order = NULL;
  int64_t *sum = NULL;
  uint8_t *index = NULL, *bits = NULL;
  FILE *f = NULL;
  fz_var(count); fz_var(order); fz_var(map); fz_var(sum);
  fz_var(index); fz_var(bits); fz_var(f);
  fz_try(ctx)
  {
    count = fz_calloc(ctx, BUCKETS, sizeof(int));
    sum = fz_calloc(ctx, BUCKETS * 3, sizeof(int64_t));
    for (int y = 0; y < h; ++y)
    {
      const uint8_t *p = samples + (size_t)stride * y;
      for (int x = 0; x < w; ++x, p += 3)
      {
        int b = sixel_bucket(p);
        count[b]++;
        for (int c = 0; c < 3; ++c)
          sum[b * 3 + c] += p[c];
      }
    }

    order = fz_calloc(ctx, BUCKETS, sizeof(*order));
    int used = 0;
    for (int b = 0; b < BUCKETS; ++b)
      if (count[b])
        order[used++] = (struct sixel_bucket_count){count[b], b};
    qsort(order, used, sizeof(*order), sixel_bucket_cmp);
    int ncolors = fz_mini(used, COLORS);
    uint8_t palette[COLORS][3];
    map = fz_calloc(ctx, BUCKETS, sizeof(int));
    for (int i = 0; i < ncolors; ++i)
    {
      int b = order[i].bucket;
      for (int c = 0; c < 3; ++c)
        palette[i][c] = (sum[b * 3 + c] + count[b] / 2) / count[b];
      map[b] = i;
    }
    for (int i = ncolors; i < used; ++i)
    {
      int b = order[i].bucket, best = 0, best_d = INT32_MAX;
      int rgb[3];
      for (int c = 0; c < 3; ++c)
        rgb[c] = (sum[b * 3 + c] + count[b] / 2) / count[b];
      for (int j = 0; j < ncolors; ++j)
      {
        int d = 0;
        for (int c = 0; c < 3; ++c)
          d += (rgb[c] - palette[j][c]) * (rgb[c] - palette[j][c]);
        if (d < best_d)
          best_d = d, best = j;
      }
      map[b] = best;
    }

    index = fz_malloc(ctx, (size_t)w * h);
    for (int y = 0; y < h; ++y)
    {
      const uint8_t *p = samples + (size_t)stride * y;
      for (int x = 0; x < w; ++x, p += 3)
        index[(size_t)y * w + x] = map[sixel_bucket(p)];
    }

    f = fopen(path, "wb");
    if (!f)
      fz_throw(ctx, FZ_ERROR_GENERIC, "cannot open %s: %s", path, strerror(errno));
    // P2 = 1: pixels not drawn keep what is under them (every pixel is drawn)
    fprintf(f, "\033P0;1;0q\"1;1;%d;%d", w, h);
    for (int i = 0; i < ncolors; ++i)
      fprintf(f, "#%d;2;%d;%d;%d", i, (palette[i][0] * 100 + 127) / 255,
              (palette[i][1] * 100 + 127) / 255, (palette[i][2] * 100 + 127) / 255);

    // Six rows at a time: for each colour of the band, the column's bits
    // that are in it, written from its first column to its last.
    bits = fz_calloc(ctx, (size_t)COLORS * w, 1);
    int lo[COLORS], hi[COLORS];
    for (int y0 = 0; y0 < h; y0 += 6)
    {
      for (int i = 0; i < ncolors; ++i)
        lo[i] = w, hi[i] = -1;
      for (int dy = 0; dy < 6 && y0 + dy < h; ++dy)
      {
        const uint8_t *row = index + (size_t)(y0 + dy) * w;
        for (int x = 0; x < w; ++x)
        {
          int i = row[x];
          bits[(size_t)i * w + x] |= 1 << dy;
          if (x < lo[i]) lo[i] = x;
          if (x > hi[i]) hi[i] = x;
        }
      }
      bool first = true;
      for (int i = 0; i < ncolors; ++i)
      {
        if (hi[i] < 0)
          continue;
        if (!first)
          fputc('$', f);
        first = false;
        fprintf(f, "#%d", i);
        sixel_run(f, '?', lo[i]);
        uint8_t *b = bits + (size_t)i * w;
        for (int x = lo[i]; x <= hi[i];)
        {
          int x1 = x + 1;
          while (x1 <= hi[i] && b[x1] == b[x])
            ++x1;
          sixel_run(f, 63 + b[x], x1 - x);
          x = x1;
        }
        memset(b + lo[i], 0, hi[i] - lo[i] + 1);
      }
      if (y0 + 6 < h)
        fputc('-', f);
    }
    fputs("\033\\", f);
    if (ferror(f))
      fz_throw(ctx, FZ_ERROR_GENERIC, "cannot write %s", path);
  }
  fz_always(ctx)
  {
    if (f && fclose(f) != 0)
      fprintf(stderr, "[export] cannot close %s: %s\n", path, strerror(errno));
    fz_free(ctx, count);
    fz_free(ctx, sum);
    fz_free(ctx, order);
    fz_free(ctx, map);
    fz_free(ctx, index);
    fz_free(ctx, bits);
  }
  fz_catch(ctx)
    fz_rethrow(ctx);
}

// Write the displayed page as a PNG (a Sixel picture when the path ends in
// ".six") fitted to the box asked for by render-to, through a temporary
// file renamed in place (a reader never sees half an image), then tell the
// editor.  A Sixel picture is the whole box, the page centred on paper: a
// terminal keeps what a picture does not cover, so each one covers what the
// last one did.
static void export_page(struct persistent_state *ps, ui_state *ui, fz_display_list *dl)
{
  if (!ui->export_path || ui->export_width <= 0 || ui->export_height <= 0)
    return;

  fz_context *ctx = ps->ctx;
  fz_pixmap *pix = NULL;
  fz_device *dev = NULL;
  char *tmp = NULL;
  fz_var(pix);
  fz_var(dev);
  fz_var(tmp);
  fz_try(ctx)
  {
    size_t len = strlen(ui->export_path);
    bool sixel = len > 4 && strcmp(ui->export_path + len - 4, ".six") == 0;
    fz_rect b = fz_bound_display_list(ctx, dl);
    float bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    if (bw <= 0 || bh <= 0)
      fz_throw(ctx, FZ_ERROR_GENERIC, "empty page");
    float scale = fz_min(ui->export_width / bw, ui->export_height / bh);
    int pw = fz_maxi(1, (int)(bw * scale + 0.5f));
    int ph = fz_maxi(1, (int)(bh * scale + 0.5f));
    int iw = sixel ? fz_maxi(pw, ui->export_width) : pw;
    int ih = sixel ? fz_maxi(ph, ui->export_height) : ph;
    int ox = (iw - pw) / 2, oy = (ih - ph) / 2;

    // Cleared to white, the paper: export_recolor gives it the theme's
    // background, inside the page and around it alike.
    pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), iw, ih, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pix, 255);
    fz_matrix ctm = fz_concat(fz_pre_translate(fz_scale(scale, scale), -b.x0, -b.y0),
                              fz_translate(ox, oy));
    dev = fz_new_draw_device(ctx, ctm, pix);
    fz_run_display_list(ctx, dl, dev, fz_identity, fz_infinite_rect, NULL);
    fz_close_device(ctx, dev);
    export_recolor(ctx, ui, pix);
    // A selection belongs to the contents it was made on: another page, or
    // the same page typeset again after an edit, drops it.
    if (ui->sel_count > 0 && (ui->sel_page != ui->page || dl != ui->export_dl))
      ui->sel_count = 0;
    if (ui->sel_count > 0)
      export_highlight(ctx, ui, pix, ctm);

    size_t n = len + 5;
    tmp = fz_malloc(ctx, n);
    snprintf(tmp, n, "%s.tmp", ui->export_path);
    if (sixel)
      save_pixmap_as_sixel(ctx, pix, tmp);
    else
      fz_save_pixmap_as_png(ctx, pix, tmp);
    if (rename(tmp, ui->export_path) != 0)
      fz_throw(ctx, FZ_ERROR_GENERIC, "cannot rename %s: %s", tmp, strerror(errno));

    ui->export_valid = true;
    ui->export_scale = scale;
    ui->export_bounds = b;
    ui->export_origin = fz_make_point(ox, oy);
    if (ui->export_dl != dl)
    {
      fz_drop_display_list(ctx, ui->export_dl);
      ui->export_dl = fz_keep_display_list(ctx, dl);
    }
    const char *src = NULL;
    int src_len = 0, src_line = 0;
    fz_buffer *sbuf = NULL;
    synctex_t *stx = send(synctex, ui->eng, &sbuf);
    if (!synctex_page_source(ctx, stx, sbuf, ui->page, &src, &src_len, &src_line))
      src = NULL;
    editor_page_image(ui->export_path, ui->page, send(page_count, ui->eng), iw, ih,
                      ps->doc_path, src, src_len, src_line);
    fflush(stdout);
  }
  fz_always(ctx)
  {
    fz_drop_device(ctx, dev);
    fz_drop_pixmap(ctx, pix);
    fz_free(ctx, tmp);
  }
  fz_catch(ctx)
  {
    fprintf(stderr, "[export] %s\n", fz_caught_message(ctx));
  }
}

// Write each page typeset as dir/page-N.png, fitted to width x height and
// in the theme's colours, and tell the editor of each and of the count.
static void export_thumbnails(struct persistent_state *ps, ui_state *ui,
                              const char *dir, int width, int height)
{
  fz_context *ctx = ps->ctx;
  int count = send(page_count, ui->eng);
  fz_buffer *sbuf = NULL;
  synctex_t *stx = send(synctex, ui->eng, &sbuf);
  for (int page = 0; page < count; ++page)
  {
    fz_display_list *dl = NULL;
    fz_pixmap *pix = NULL;
    fz_device *dev = NULL;
    char path[1024], tmp[1040];
    snprintf(path, sizeof(path), "%s/page-%d.png", dir, page);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fz_var(dl);
    fz_var(pix);
    fz_var(dev);
    fz_try(ctx)
    {
      dl = send(render_page, ui->eng, ctx, page);
      fz_rect b = fz_bound_display_list(ctx, dl);
      float bw = b.x1 - b.x0, bh = b.y1 - b.y0;
      if (bw <= 0 || bh <= 0)
        fz_throw(ctx, FZ_ERROR_GENERIC, "empty page");
      float scale = fz_min(width / bw, height / bh);
      int pw = fz_maxi(1, (int)(bw * scale + 0.5f));
      int ph = fz_maxi(1, (int)(bh * scale + 0.5f));
      pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), pw, ph, NULL, 0);
      fz_clear_pixmap_with_value(ctx, pix, 255);
      fz_matrix ctm = fz_pre_translate(fz_scale(scale, scale), -b.x0, -b.y0);
      dev = fz_new_draw_device(ctx, ctm, pix);
      fz_run_display_list(ctx, dl, dev, fz_identity, fz_infinite_rect, NULL);
      fz_close_device(ctx, dev);
      export_recolor(ctx, ui, pix);
      fz_save_pixmap_as_png(ctx, pix, tmp);
      if (rename(tmp, path) != 0)
        fz_throw(ctx, FZ_ERROR_GENERIC, "cannot rename %s: %s", tmp, strerror(errno));
      const char *src = NULL;
      int src_len = 0, src_line = 0;
      if (!synctex_page_source(ctx, stx, sbuf, page, &src, &src_len, &src_line))
        src = NULL;
      editor_thumbnail(path, page, pw, ph, ps->doc_path, src, src_len, src_line);
    }
    fz_always(ctx)
    {
      fz_drop_device(ctx, dev);
      fz_drop_pixmap(ctx, pix);
      fz_drop_display_list(ctx, dl);
    }
    fz_catch(ctx)
    {
      fprintf(stderr, "[thumbnails] page %d: %s\n", page, fz_caught_message(ctx));
    }
  }
  editor_thumbnails_done(count, send(get_status, ui->eng) != DOC_RUNNING);
  fflush(stdout);
}

static void display_page(struct persistent_state *ps, ui_state *ui)
{
  fz_display_list *dl = send(render_page, ui->eng, ps->ctx, ui->page);
  txp_renderer_set_contents(ps->ctx, ui->doc_renderer, dl);
  export_page(ps, ui, dl);
  fz_drop_display_list(ps->ctx, dl);
  schedule_event(RENDER_EVENT);
}

#if !SDL_VERSION_ATLEAST(2, 0, 16)
static void
SDL_SetWindowAlwaysOnTop(SDL_Window *window, SDL_bool state)
{
  (void)window;
  (void)state;
  fprintf(stderr, "[info] stay-on-top feature is not available with "
                  "SDL older than 2.16.0\n");
}
#endif


static void interpret_register(struct persistent_state *ps,
                               ui_state *ui,
                               const char *path)
{
  if (path[0] == '/')
  {
    int go_up = 0;
    path = relative_path(path, ps->doc_path, &go_up);
    if (go_up > 0)
    {
      fprintf(stderr, "[command] register %s: file has a different root, skipping\n", path);
      return;
    }
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] register %s: file not found, skipping\n", path);
    return;
  }

  e->promised = true;
  fprintf(stderr, "[command] register %s: marked as promised\n", path);
}

static void interpret_command(struct persistent_state *ps,
                              ui_state *ui,
                              vstack *stack,
                              val command)
{
  struct editor_command cmd;
  if (!editor_parse(ps->ctx, stack, command, &cmd))
    return;

  switch (cmd.tag)
  {
    case EDIT_OPEN:
      if (cmd.open.base64)
      {
        unsigned char *buf = malloc(cmd.open.length);
        if (!buf) break;
        memcpy(buf, cmd.open.data, cmd.open.length);
        int decoded_len = base64_decode(buf, cmd.open.length);
        if (decoded_len < 0)
        {
          fprintf(stderr, "[command] open-base64: invalid base64 data\n");
          free(buf);
          break;
        }
        interpret_open(ps, ui, cmd.open.path, (const char *)buf, decoded_len);
        free(buf);
      }
      else
        interpret_open(ps, ui, cmd.open.path, cmd.open.data, cmd.open.length);
      break;

    case EDIT_CLOSE:
      interpret_close(ps, ui, cmd.close.path);
      break;

    case EDIT_CHANGE:
      interpret_change(ps, ui, &cmd.change);
      break;

    case EDIT_THEME:
    {
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      config->background_color = convert_color(ps->ctx, stack, cmd.theme.bg);
      config->foreground_color = convert_color(ps->ctx, stack, cmd.theme.fg);
      config->themed_color = 1;
      schedule_event(RENDER_EVENT);
      fprintf(stderr, "[command] theme %x %x\n",
              config->background_color, config->foreground_color);
      // The editor shows the page written by render-to: write it again in
      // the new colours (the window above repaints by itself).
      if (ui->export_path && ui->export_dl)
        export_page(ps, ui, ui->export_dl);
    }
    break;

    case EDIT_PREVIOUS_PAGE:
      previous_page(ps->ctx, ui, 0);
      break;

    case EDIT_NEXT_PAGE:
      next_page(ps->ctx, ui, 0);
      break;

    case EDIT_MOVE_WINDOW:
    {
      float x = cmd.move_window.x, y = cmd.move_window.y,
            w = cmd.move_window.w, h = cmd.move_window.h;
      int x0 = x, y0 = y;
      SDL_SetWindowPosition(ui->window, x, y);
      SDL_GetWindowPosition(ui->window, &x0, &y0);
      SDL_SetWindowSize(ui->window, w + x - x0, h + y - y0);
      fprintf(stderr, "[command] move-window %f %f %f %f (pos: %d %d)\n",
              x, y, w, h, x0, y0);
    }
    break;

    case EDIT_MAP_WINDOW:
    {
      float x = cmd.move_window.x, y = cmd.move_window.y,
            w = cmd.move_window.w, h = cmd.move_window.h;
      int x0 = x, y0 = y;
      SDL_SetWindowBordered(ui->window, SDL_FALSE);
      SDL_SetWindowAlwaysOnTop(ui->window, SDL_TRUE);
      SDL_SetWindowPosition(ui->window, x, y);
      SDL_GetWindowPosition(ui->window, &x0, &y0);
      SDL_SetWindowSize(ui->window, w + x - x0, h + y - y0);
      fprintf(stderr, "[command] map-window %f %f %f %f (pos: %d %d)\n",
              x, y, w, h, x0, y0);
    }
    break;

    case EDIT_UNMAP_WINDOW:
    {
      if (!(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_INPUT_FOCUS))
        SDL_SetWindowBordered(ui->window, SDL_TRUE);
      SDL_SetWindowAlwaysOnTop(ui->window, SDL_FALSE);
      fprintf(stderr, "[command] unmap-window\n");
    }
    break;

    case EDIT_RESCAN:
      schedule_event(SCAN_EVENT);
      break;

    case EDIT_STAY_ON_TOP:
      SDL_SetWindowAlwaysOnTop(ui->window, cmd.stay_on_top.status);
      fprintf(stderr, "[command] stay-on-top %d\n", cmd.stay_on_top.status);
      break;

    case EDIT_SYNCTEX_FORWARD:
    {
      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      int go_up = 0;
      const char *path = relative_path(cmd.synctex_forward.path, ps->doc_path, &go_up);
      if (go_up > 0)
      {
        fprintf(stderr,
                "[command] synctex-forward %s: file has a different root, skipping\n",
                path);
      }
      else
      {
        synctex_set_target(stx, ui->page, path, cmd.synctex_forward.line);
        schedule_event(STDIN_EVENT);
      }
    }
    break;

    case EDIT_CROP:
    {
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      config->crop = !config->crop;
      schedule_event(RENDER_EVENT);
    }
    break;
    case EDIT_INVERT:
    {
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      config->invert_color = !config->invert_color;
      schedule_event(RENDER_EVENT);
    }
    break;

    case EDIT_REGISTER:
      interpret_register(ps, ui, cmd.reg.path);
      break;

    case EDIT_PAUSE:
      ps->paused = true;
      fprintf(stderr, "[command] pause: engine stepping suspended\n");
      break;

    case EDIT_RESUME:
      ps->paused = false;
      fprintf(stderr, "[command] resume: engine stepping enabled\n");
      // Spawn the worker if -stream started paused (no-op otherwise)
      send(step, ui->eng, ps->ctx, true);
      schedule_event(SCAN_EVENT);
      break;

    case EDIT_RERUN:
      ps->rerun_enabled = cmd.rerun.status;
      fprintf(stderr, "[command] rerun %s\n",
              ps->rerun_enabled ? "enabled" : "disabled");
      break;

    case EDIT_RERUN_ONCE:
      ps->rerun_once_pending = true;
      fprintf(stderr, "[command] rerun-once: pending immediate pass\n");
      schedule_event(SCAN_EVENT);
      break;

    case EDIT_RENDER_TO:
    {
      free(ui->export_path);
      ui->export_path = NULL;
      ui->export_valid = false;
      ui->export_width = cmd.render_to.width;
      ui->export_height = cmd.render_to.height;
      if (cmd.render_to.path[0] && ui->export_width > 0 && ui->export_height > 0)
      {
        ui->export_path = strdup(cmd.render_to.path);
        // Write the page on display at once, at the new size
        if (ui->page < send(page_count, ui->eng))
          display_page(ps, ui);
      }
      fprintf(stderr, "[command] render-to %s %dx%d\n",
              ui->export_path ? ui->export_path : "(off)",
              ui->export_width, ui->export_height);
    }
    break;

    case EDIT_THUMBNAILS:
    {
      ui->want_all = cmd.thumbnails.dir[0] != 0;
      if (ui->want_all && cmd.thumbnails.width > 0 && cmd.thumbnails.height > 0)
        export_thumbnails(ps, ui, cmd.thumbnails.dir,
                          cmd.thumbnails.width, cmd.thumbnails.height);
      fprintf(stderr, "[command] thumbnails %s %dx%d\n",
              ui->want_all ? cmd.thumbnails.dir : "(off)",
              cmd.thumbnails.width, cmd.thumbnails.height);
    }
    break;

    case EDIT_SELECT:
    {
      if (!ui->export_valid || !ui->export_dl)
        break;
      fz_context *ctx = ps->ctx;
      fz_rect b = ui->export_bounds;
      float s = ui->export_scale;
      fz_point o = ui->export_origin;
      fz_point a = fz_make_point(b.x0 + (cmd.select.x0 - o.x) / s,
                                 b.y0 + (cmd.select.y0 - o.y) / s);
      fz_point z = fz_make_point(b.x0 + (cmd.select.x1 - o.x) / s,
                                 b.y0 + (cmd.select.y1 - o.y) / s);
      fz_stext_page *page = NULL;
      char *text = NULL;
      fz_var(page);
      fz_var(text);
      fz_try(ctx)
      {
        page = fz_new_stext_page_from_display_list(ctx, ui->export_dl, NULL);
        ui->sel_count = fz_highlight_selection(ctx, page, a, z, ui->sel_quads,
                                               nelem(ui->sel_quads));
        ui->sel_page = ui->page;
        text = fz_copy_selection(ctx, page, a, z, 0);
        editor_selected_text(text);
        fprintf(stderr, "[command] select: %d quads, %d bytes\n",
                ui->sel_count, (int)strlen(text));
        export_page(ps, ui, ui->export_dl);
      }
      fz_always(ctx)
      {
        fz_free(ctx, text);
        fz_drop_stext_page(ctx, page);
      }
      fz_catch(ctx)
      {
        fprintf(stderr, "[command] select: %s\n", fz_caught_message(ctx));
      }
    }
    break;

    case EDIT_CLICK_AT:
    {
      if (!ui->export_valid)
        break;
      if (ui->sel_count > 0)
      {
        ui->sel_count = 0;
        if (ui->export_dl)
          export_page(ps, ui, ui->export_dl);
      }
      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      if (stx && buf)
      {
        fz_point pt = fz_make_point(
            ui->export_bounds.x0 + (cmd.click_at.x - ui->export_origin.x) / ui->export_scale,
            ui->export_bounds.y0 + (cmd.click_at.y - ui->export_origin.y) / ui->export_scale);
        float f = 1 / send(scale_factor, ui->eng);
        fprintf(stderr, "[command] click-at (%f,%f) mapped:(%f,%f)\n",
                cmd.click_at.x, cmd.click_at.y, f * pt.x, f * pt.y);
        synctex_scan(ps->ctx, stx, buf, ps->doc_path, ui->page, f * pt.x, f * pt.y);
      }
    }
    break;
  }
}

static void sync_fullscreen_state(struct fullscreen_state *fs,
                                  txp_renderer_config *config,
                                  SDL_Window *win)
{
  bool cur_fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
  if (cur_fs == fs->prev_fs) return;

  if (cur_fs) {
    if (!fs->has_backup) {
      fs->windowed_backup.crop = config->crop;
      fs->windowed_backup.fit  = config->fit;
      fs->windowed_backup.zoom = config->zoom;
      fs->has_backup = true;
    }
    config->crop = false;
    config->fit  = FIT_PAGE;
    config->zoom = 1.0;
  } else if (fs->has_backup) {
    config->crop = fs->windowed_backup.crop;
    config->fit  = fs->windowed_backup.fit;
    config->zoom = fs->windowed_backup.zoom;
    fs->has_backup = false;
  }
  fs->prev_fs = cur_fs;
}

/* Entry point */

bool texpresso_main(struct persistent_state *ps)
{
  editor_set_protocol(ps->protocol);
  editor_set_line_output(ps->line_output);
  pstate = ps;

  struct fullscreen_state fs = {
    .has_backup = false,
    .prev_fs = (SDL_GetWindowFlags(ps->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0,
  };

  ui_state raw_ui, *ui = &raw_ui;

  ui->window = ps->window;
  ui->export_path = NULL;
  ui->export_width = ui->export_height = 0;
  ui->export_valid = false;
  ui->export_dl = NULL;
  ui->sel_count = 0;
  ui->sel_page = -1;

  bool using_texlive = 0;

  if (ps->use_texlive)
  {
    if (!texlive_available())
    {
      fprintf(stderr,
              "[fatal] cannot find kpsewhich command for texlive integration "
              "(please make sure it is installed and visible in PATH)\n");
      return 0;
    }
    using_texlive = 1;
  }
  else if (ps->use_tectonic)
  {
    if (!tectonic_available())
    {
      fprintf(stderr,
              "[fatal] cannot find tectonic "
              "(please make sure it is installed and visible in PATH)\n");
      return 0;
    }
  }
  else if (!(using_texlive = texlive_available()) && !tectonic_available())
  {
    fprintf(stderr,
            "[fatal] cannot find tectonic nor kpsewhich (texlive)"
            "(please make sure at least one of them is installed and visible in PATH)\n");
    return 0;
  }

  const char *doc_ext = NULL;

  for (const char *ptr = ps->doc_name; *ptr; ptr++)
    if (*ptr == '.')
      doc_ext = ptr + 1;

  char engine_path[4096];
  find_engine(engine_path, ps->exe_path);
  fprintf(stderr, "[info] engine path: %s\n", engine_path);

  if (doc_ext && strcmp(doc_ext, "pdf") == 0)
    ui->eng = txp_create_pdf_engine(ps->ctx, ps->doc_name);
  else
  {
    dvi_reshooks hooks;
    if (using_texlive)
      hooks = dvi_texlive_hooks(ps->ctx, ps->doc_path);
    else
      hooks = dvi_tectonic_hooks(ps->ctx, ps->doc_path);

    if (doc_ext && (strcmp(doc_ext, "dvi") == 0 || strcmp(doc_ext, "xdv") == 0))
      ui->eng = txp_create_dvi_engine(ps->ctx, ps->doc_name, hooks);
    else
      ui->eng = txp_create_tex_engine(ps->ctx, engine_path, using_texlive,
                                      ps->stream_mode, ps->inclusion_path,
                                      ps->doc_name, hooks);
  }

  ui->sdl_renderer = ps->renderer;
  ui->doc_renderer = txp_renderer_new(ps->ctx, ui->sdl_renderer);

  if (ps->initial.initialized)
  {
    ui->page = ps->initial.page;
    ui->zoom = ps->initial.zoom;
    ui->need_synctex = ps->initial.need_synctex;
    *txp_renderer_get_config(ps->ctx, ui->doc_renderer) = ps->initial.config;
    txp_renderer_set_contents(ps->ctx, ui->doc_renderer,
                              ps->initial.display_list);
    editor_reset_sync();
  }
  else
  {
    ui->page = 0;
    ui->zoom = 0;
    ui->need_synctex = 1;
  }

  ui->mouse_status = UI_MOUSE_NONE;
  ui->last_mouse_x = -1000;
  ui->last_mouse_y = -1000;
  ui->last_click_ticks = SDL_GetTicks() - 200000000;

  bool quit = 0, reload = 0;
  if (!ps->paused)
    send(step, ui->eng, ps->ctx, true);
  render(ps->ctx, ui);
  schedule_event(RELOAD_EVENT);

  struct repaint_on_resize_env repaint_on_resize_env = {.ctx = ps->ctx, .ui = ui};
  SDL_AddEventWatch(repaint_on_resize, &repaint_on_resize_env);

  vstack *cmd_stack = vstack_new(ps->ctx);
  prot_parser cmd_parser;
  prot_initialize(&cmd_parser, (ps->protocol == EDITOR_JSON));

  // Start watching stdin
  int poll_stdin_pipe[2];
  if (pipe(poll_stdin_pipe) == -1)
  {
    perror("pipe");
    abort();
  }

  SDL_Thread *poll_stdin_thread =
    SDL_CreateThread(poll_stdin_thread_main, "poll_stdin_thread", poll_stdin_pipe);
  bool stdin_eof = 0;
  int rerun_count = 0;

  while (!quit)
  {
    SDL_Event e;
    bool has_event = SDL_PollEvent(&e);

    // Process stdin
    send(begin_changes, ui->eng, ps->ctx);
    char buffer[4096];
    int n = -1;
    while (!stdin_eof && poll_stdin() && (n = read(STDIN_FILENO, buffer, 4096)) != 0)
    {
      if (n == -1)
      {
        if (errno == EINTR)
          continue;
        perror("poll stdin");
        break;
      }

      fprintf(stderr, "stdin: %.*s\n", n, buffer);

      const char *ptr = buffer, *lim = buffer + n;
      fz_try(ps->ctx)
      {
        while ((ptr = prot_parse(ps->ctx, &cmd_parser, cmd_stack, ptr, lim)))
        {
          val cmds = vstack_get_values(ps->ctx, cmd_stack);
          int n_cmds = val_array_length(ps->ctx, cmd_stack, cmds);
          for (int i = 0; i < n_cmds; i++)
          {
            val cmd = val_array_get(ps->ctx, cmd_stack, cmds, i);
            interpret_command(ps, ui, cmd_stack, cmd);
          }
        }
      }
      fz_catch(ps->ctx)
      {
        fprintf(stderr, "error while reading stdin commands: %s\n",
                fz_caught_message(ps->ctx));
        vstack_reset(ps->ctx, cmd_stack);
        prot_reinitialize(&cmd_parser);
      }
    }
    if (n == 0) stdin_eof = 1;

    if (send(end_changes, ui->eng, ps->ctx))
    {
      if (!ps->paused)
        send(step, ui->eng, ps->ctx, true);
      schedule_event(RELOAD_EVENT);
      rerun_count = 0;
    }

    // Process document
    {
      int before_page_count = send(page_count, ui->eng);
      bool advance = !ps->paused && advance_engine(ps->ctx, ui);
      send(finish_convergence, ui->eng, ps->ctx);
      int after_page_count = send(page_count, ui->eng);
      fflush(stdout);

      if (ui->page >= before_page_count && ui->page < after_page_count)
        schedule_event(RELOAD_EVENT);

      // Fire an on-demand rerun as soon as the engine is ready, regardless of
      // idle state. Runs in the main body of the loop (not inside the idle
      // wait) so it triggers even during active compilation cycles.
      bool aux_ready = !send(is_finishing, ui->eng)
                       && send(aux_dirty, ui->eng);
      if (ps->rerun_once_pending && aux_ready)
      {
        ps->rerun_once_pending = false;
        fprintf(stderr, "[rerun] on-demand: finishing pass\n");
        send(start_finishing, ui->eng);
        schedule_event(RELOAD_EVENT);
        continue;
      }

      if (!has_event)
      {
        if (advance)
          continue;
        if (!stdin_eof)
          wakeup_poll_thread(poll_stdin_pipe, 'c');

        bool rerun_eligible = ps->rerun_enabled
                              && rerun_count < MAX_RERUNS
                              && aux_ready;
        if (rerun_eligible)
          has_event = SDL_WaitEventTimeout(&e, T_IDLE_MS);
        else
          has_event = SDL_WaitEvent(&e);
        if (!has_event)
        {
          if (rerun_eligible)
          {
            rerun_count++;
            fprintf(stderr, "[rerun] idle %dms: finishing pass %d/%d\n",
                    T_IDLE_MS, rerun_count, MAX_RERUNS);
            send(start_finishing, ui->eng);
            schedule_event(RELOAD_EVENT);
            continue;
          }
          fprintf(stderr, "SDL_WaitEvent error: %s\n", SDL_GetError());
          break;
        }
      }

      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      int page = -1, x = -1, y = -1;
      if (synctex_find_target(ps->ctx, stx, buf, &page, &x, &y))
      {
        fprintf(stderr, "[synctex forward] sync: hit page %d, coordinates (%d, %d)\n",
                page, x, y);

        if (page != ui->page &&
            page >= 0 && page < send(page_count, ui->eng))
        {
          ui->page = page;
          display_page(ps, ui);
        }

        // FIXME: Scroll to point
        float f = send(scale_factor, ui->eng);
        fz_point p = fz_make_point(f * x, f * y);
        fz_point pt = txp_renderer_document_to_screen(ps->ctx, ui->doc_renderer, p);
        fprintf(stderr, "[synctex forward] position on screen: (%.02f, %.02f)\n",
                pt.x, pt.y);
        int w, h;
        txp_renderer_screen_size(ps->ctx, ui->doc_renderer, &w, &h);
        float margin_lo = h / 4.0;
        float margin_hi = h / 3.0;

        txp_renderer_config *config =
            txp_renderer_get_config(ps->ctx, ui->doc_renderer);

        float delta = 0.0;
        if (pt.y < margin_lo)
          delta = - pt.y + margin_hi;
        else if (pt.y >= h - margin_lo)
          delta = h - pt.y - margin_hi;
        fprintf(stderr, "[synctex forward] pan.y = %.02f + %.02f = %.02f\n",
                config->pan.y, delta, config->pan.y + delta);
        config->pan.y += delta;
        if (delta != 0.0)
          schedule_event(RENDER_EVENT);
      }
    }

    txp_renderer_set_scale_factor(ps->ctx, ui->doc_renderer,
                                  get_scale_factor(ui->window));
    txp_renderer_config *config =
        txp_renderer_get_config(ps->ctx, ui->doc_renderer);

    // Process event
    switch (e.type)
    {
      case SDL_QUIT:
        quit = 1;
        break;

      case SDL_KEYDOWN:
        switch (e.key.keysym.sym)
        {
          case SDLK_LEFT:
          case SDLK_PAGEUP:
            previous_page(ps->ctx, ui, 0);
            break;

          case SDLK_UP:
            ui_pan(ps->ctx, ui, 2.0/3.0);
            break;

          case SDLK_DOWN:
            ui_pan(ps->ctx, ui, -2.0/3.0);
            break;

          case SDLK_RIGHT:
          case SDLK_PAGEDOWN:
            next_page(ps->ctx, ui, 0);
            break;

          case SDLK_PLUS:
          case SDLK_KP_PLUS:
          case SDLK_EQUALS: // Handles standard '=' key (often sharing '+' on keyboards)
            config->zoom *= 1.1;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_MINUS:
          case SDLK_KP_MINUS:
            config->zoom /= 1.1;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_h:
            if (SDL_GetModState() & KMOD_SHIFT)
              ui_pan_x(ps->ctx, ui, 1.0/5.0);  // Large scroll left
            else
              ui_pan_x(ps->ctx, ui, 1.0/25.0);
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_l:
            if (SDL_GetModState() & KMOD_SHIFT)
              ui_pan_x(ps->ctx, ui, -1.0/5.0);  // Large scroll right
            else
              ui_pan_x(ps->ctx, ui, -1.0/25.0);
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_j:
            if (SDL_GetModState() & KMOD_SHIFT)
              ui_pan(ps->ctx, ui, -1.0/5.0); // Medium down-pan
            else
              ui_pan(ps->ctx, ui, -1.0/25.0); // Fine line down-pan
            break;

          case SDLK_k:
            if (SDL_GetModState() & KMOD_SHIFT)
              ui_pan(ps->ctx, ui, 1.0/5.0);  // Medium up-pan
            else
              ui_pan(ps->ctx, ui, 1.0/25.0);  // Fine line up-pan
            break;

          case SDLK_SPACE:
            ui_pan(ps->ctx, ui, -2.0/3.0); // Page down (matching down arrow)
            break;

          case SDLK_b:
            if (SDL_GetModState() & KMOD_SHIFT)
            {
              // Shift + B: Toggle Window Border
              SDL_SetWindowBordered(
                  ui->window,
                  !!(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_BORDERLESS));
            }
            else
            {
              ui_pan(ps->ctx, ui, 2.0/3.0); // Lowercase b: Page up (matching up arrow)
            }
            break;

          case SDLK_w:
            config->fit = FIT_WIDTH;
            config->zoom = 1;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_p:
            config->fit = FIT_PAGE;
            config->zoom = 1;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_t:
            SDL_SetWindowAlwaysOnTop(
                ui->window,
                !(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_ALWAYS_ON_TOP));
            break;

          case SDLK_c:
            config->crop = !config->crop;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_i:
            if ((SDL_GetModState() & KMOD_SHIFT))
              config->themed_color = !config->themed_color;
            else
              config->invert_color = !config->invert_color;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_ESCAPE:
            SDL_SetWindowFullscreen(ui->window, 0);
            break;

          // Toggle Fullscreen
          case SDLK_f:
          case SDLK_F5:
          case SDLK_F11:
            SDL_SetWindowFullscreen(ui->window,
              (SDL_GetWindowFlags(ui->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ?
                0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            schedule_event(RENDER_EVENT);
            break;

          // case SDLK_r:
          //   reload = 1;
          case SDLK_q:
            quit = 1;
            break;
        }
        break;

      case SDL_MOUSEWHEEL:
        {
           int mx = 0, my = 0;
           float px = 0, py = 0;
#if SDL_VERSION_ATLEAST(2, 0, 260)
           mx = e.wheel.mouseX;
           my = e.wheel.mouseY;
#else
           SDL_GetMouseState(&mx, &my);
#endif
#if SDL_VERSION_ATLEAST(2, 0, 18)
          px = e.wheel.preciseX;
          py = e.wheel.preciseY;
#else
          px = e.wheel.x;
          py = e.wheel.y;
#endif
          bool ctrl = !!(SDL_GetModState() & KMOD_CTRL);
          ui_mouse_wheel(ps->ctx, ui, px, py, mx, my, ctrl, e.wheel.timestamp);
        }
        break;

      case SDL_MOUSEBUTTONDOWN:
        ui_mouse_down(ps, ui, e.button.x, e.button.y,
                      SDL_GetModState() & KMOD_CTRL);
        break;

      case SDL_MOUSEBUTTONUP:
        ui_mouse_up(ui);
        break;

      case SDL_MOUSEMOTION:
        ui_mouse_move(ps->ctx, ui, e.motion.x, e.motion.y);
        break;

      case SDL_WINDOWEVENT:
        switch (e.window.event)
        {
          case SDL_WINDOWEVENT_SIZE_CHANGED:
          case SDL_WINDOWEVENT_RESIZED:
          case SDL_WINDOWEVENT_EXPOSED:
            sync_fullscreen_state(&fs, config, ui->window);
            schedule_event(RENDER_EVENT);
            break;
        }
        break;
    }

    if (e.type == ps->custom_event)
    {
      int page_count;
      *(char *)e.user.data1 = 0;
      switch (e.user.code)
      {
        case SCAN_EVENT:
          if (should_reload_binary())
          {
            quit = reload = 1;
            continue;
          }
          send(begin_changes, ui->eng, ps->ctx);
          flush_changes(ps, ui);
          send(detect_changes, ui->eng, ps->ctx);
          if (send(end_changes, ui->eng, ps->ctx))
          {
            if (!ps->paused)
              send(step, ui->eng, ps->ctx, true);
            schedule_event(RELOAD_EVENT);
          }
          break;

        case RENDER_EVENT:
          render(ps->ctx, ui);
          send(begin_changes, ui->eng, ps->ctx);
          flush_changes(ps, ui);
          if (send(end_changes, ui->eng, ps->ctx))
          {
            if (!ps->paused)
              send(step, ui->eng, ps->ctx, true);
            schedule_event(RELOAD_EVENT);
          }
          break;

        case RELOAD_EVENT:
          page_count = send(page_count, ui->eng);
          if (ui->page >= page_count &&
              send(get_status, ui->eng) == DOC_TERMINATED)
          {
            if (page_count > 0)
              ui->page = page_count - 1;
          }
          if (ui->page < page_count)
            display_page(ps, ui);
          break;

        case STDIN_EVENT:
          break;
      }
    }
    if (ps->initialize_only &&
        (send(page_count, ui->eng) > 0 ||
         (send(get_status, ui->eng) == DOC_TERMINATED && stdin_eof)))
    {
      fprintf(stderr, "[info] Initialize mode: terminating engine process\n");
      quit = 1;
    }
  }

  {
    int status = 0;
    wakeup_poll_thread(poll_stdin_pipe, 'q');
    SDL_WaitThread(poll_stdin_thread, &status);
    close(poll_stdin_pipe[0]);
    close(poll_stdin_pipe[1]);
  }

  SDL_DelEventWatch(repaint_on_resize, &repaint_on_resize_env);

  if (ps->initial.initialized && ps->initial.display_list)
    fz_drop_display_list(ps->ctx, ps->initial.display_list);
  ps->initial.initialized = 1;
  ps->initial.page = ui->page;
  ps->initial.need_synctex = ui->need_synctex;
  ps->initial.zoom = ui->zoom;
  ps->initial.config = *txp_renderer_get_config(ps->ctx, ui->doc_renderer);
  ps->initial.display_list = txp_renderer_get_contents(ps->ctx, ui->doc_renderer);
  if (ps->initial.display_list)
    fz_keep_display_list(ps->ctx, ps->initial.display_list);

  txp_renderer_free(ps->ctx, ui->doc_renderer);
  send(destroy, ui->eng, ps->ctx);

  return reload;
}
