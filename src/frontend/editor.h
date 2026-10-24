#ifndef EDITOR_H_
#define EDITOR_H_

#include "driver.h"
#include "vstack.h"

void editor_set_protocol(enum editor_protocol protocol);
void editor_set_line_output(bool line);

// Receiving commands

enum EDITOR_CHANGE_UNIT
{
  CHANGE_BYTES,
  CHANGE_LINES,
  CHANGE_RANGE
};

enum EDITOR_COMMAND
{
  EDIT_OPEN,
  EDIT_CLOSE,
  EDIT_CHANGE,
  EDIT_THEME,
  EDIT_PREVIOUS_PAGE,
  EDIT_NEXT_PAGE,
  EDIT_MOVE_WINDOW,
  EDIT_RESCAN,
  EDIT_STAY_ON_TOP,
  EDIT_SYNCTEX_FORWARD,
  EDIT_MAP_WINDOW,
  EDIT_UNMAP_WINDOW,
  EDIT_CROP,
  EDIT_INVERT,
  EDIT_REGISTER,
  EDIT_PAUSE,
  EDIT_RESUME,
  EDIT_RERUN,
  EDIT_RERUN_ONCE,
  EDIT_RENDER_TO,
  EDIT_CLICK_AT,
  EDIT_SELECT,
};

struct editor_change
{
  const char *path;
  const char *data;
  enum
  {
    BASE_BYTE,
    BASE_LINE,
    BASE_RANGE,
  } base;
  int length;
  union
  {
    struct
    {
      int offset, remove;
    } span;
    struct
    {
      int start_line, start_char;
      int end_line, end_char;
    } range;
  };
};

struct editor_command
{
  enum EDITOR_COMMAND tag;
  union {
    struct {
      const char *path;
      const char *data;
      int length;
      bool base64;
    } open;

    struct {
      const char *path;
    } close;

    struct editor_change change;

    struct {
      float bg[3], fg[3];
    } theme;

    struct {
    } previous_page;

    struct {
    } next_page;

    struct {
      float x, y, w, h;
    } move_window;

    struct {
    } rescan;

    struct {
      bool status;
    } stay_on_top;

    struct {
      const char *path;
      int line;
    } synctex_forward;

    struct {
      float x, y, w, h;
    } map_window;

    struct {
    } unmap_window;

    struct {
    } crop;

    struct {
    } invert;

    struct {
      const char *path;
    } reg;

    struct {
    } pause;

    struct {
    } resume;

    struct {
      bool status;
    } rerun;

    struct {
    } rerun_once;

    // Write the displayed page as a PNG at `path`, fitted to width x height
    // pixels, after every change; an empty path or a zero size stops it.
    // A path ending in ".six" gets a Sixel picture instead, for a terminal.
    struct {
      const char *path;
      int width, height;
    } render_to;

    // A click at (x, y), in pixels of the last image written by render-to:
    // answered with a synctex message, as a click in the window is.
    struct {
      float x, y;
    } click_at;

    // Select the text from (x0, y0) to (x1, y1), in pixels of the last image
    // written by render-to, in reading order: answered with selected-text,
    // and the image is written again with the selection highlighted.
    struct {
      float x0, y0, x1, y1;
    } select;
  };
};

bool editor_parse(fz_context *ctx,
                  vstack *stack,
                  val command,
                  struct editor_command *out);

// Sending message

enum EDITOR_INFO_BUFFER
{
  BUF_OUT, // TeX process stdout
  BUF_LOG, // TeX output log file
};

void editor_append(enum EDITOR_INFO_BUFFER name, fz_buffer *buf, int pos);
void editor_truncate(enum EDITOR_INFO_BUFFER name, fz_buffer *buf);
void editor_flush(void);
void editor_synctex(const char *dirname, const char *basename, int basename_len, int line, int column);
void editor_reset_sync(void);
void editor_notify_file_opened(int index, const char *path, int len);
void editor_page_image(const char *path, int page, int page_count, int width, int height,
                       const char *dirname, const char *source, int source_len, int line);
void editor_selected_text(const char *text);

enum EDITOR_LOOKUP_STATUS
{
  LOOKUP_SUCCESSFUL,
  LOOKUP_FAILED,
  LOOKUP_PROMISED,
};

void editor_notify_lookup(const char *path,
                          int len,
                          bool read,
                          enum EDITOR_LOOKUP_STATUS status);

#endif  // EDITOR_H_
