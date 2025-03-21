// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

// Some of this code was adapted from 'if_py_both.h' from the original
// vim source
#include <lauxlib.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "nvim/api/buffer.h"
#include "nvim/api/private/defs.h"
#include "nvim/api/private/helpers.h"
#include "nvim/buffer.h"
#include "nvim/buffer_updates.h"
#include "nvim/change.h"
#include "nvim/charset.h"
#include "nvim/cursor.h"
#include "nvim/decoration.h"
#include "nvim/ex_cmds.h"
#include "nvim/ex_docmd.h"
#include "nvim/extmark.h"
#include "nvim/fileio.h"
#include "nvim/getchar.h"
#include "nvim/lua/executor.h"
#include "nvim/map.h"
#include "nvim/map_defs.h"
#include "nvim/mark.h"
#include "nvim/memline.h"
#include "nvim/memory.h"
#include "nvim/misc1.h"
#include "nvim/move.h"
#include "nvim/ops.h"
#include "nvim/undo.h"
#include "nvim/vim.h"
#include "nvim/window.h"

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "api/buffer.c.generated.h"
#endif


/// \defgroup api-buffer
///
/// \brief For more information on buffers, see |buffers|
///
/// Unloaded Buffers:~
///
/// Buffers may be unloaded by the |:bunload| command or the buffer's
/// |'bufhidden'| option. When a buffer is unloaded its file contents are freed
/// from memory and vim cannot operate on the buffer lines until it is reloaded
/// (usually by opening the buffer again in a new window). API methods such as
/// |nvim_buf_get_lines()| and |nvim_buf_line_count()| will be affected.
///
/// You can use |nvim_buf_is_loaded()| or |nvim_buf_line_count()| to check
/// whether a buffer is loaded.


/// Gets the buffer line count
///
/// @param buffer   Buffer handle, or 0 for current buffer
/// @param[out] err Error details, if any
/// @return Line count, or 0 for unloaded buffer. |api-buffer|
Integer nvim_buf_line_count(Buffer buffer, Error *err)
  FUNC_API_SINCE(1)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return 0;
  }

  // return sentinel value if the buffer isn't loaded
  if (buf->b_ml.ml_mfp == NULL) {
    return 0;
  }

  return buf->b_ml.ml_line_count;
}

/// Activates buffer-update events on a channel, or as Lua callbacks.
///
/// Example (Lua): capture buffer updates in a global `events` variable
/// (use "print(vim.inspect(events))" to see its contents):
/// <pre>
///   events = {}
///   vim.api.nvim_buf_attach(0, false, {
///     on_lines=function(...) table.insert(events, {...}) end})
/// </pre>
///
/// @see |nvim_buf_detach()|
/// @see |api-buffer-updates-lua|
///
/// @param channel_id
/// @param buffer Buffer handle, or 0 for current buffer
/// @param send_buffer True if the initial notification should contain the
///        whole buffer: first notification will be `nvim_buf_lines_event`.
///        Else the first notification will be `nvim_buf_changedtick_event`.
///        Not for Lua callbacks.
/// @param  opts  Optional parameters.
///             - on_lines: Lua callback invoked on change.
///               Return `true` to detach. Args:
///               - the string "lines"
///               - buffer handle
///               - b:changedtick
///               - first line that changed (zero-indexed)
///               - last line that was changed
///               - last line in the updated range
///               - byte count of previous contents
///               - deleted_codepoints (if `utf_sizes` is true)
///               - deleted_codeunits (if `utf_sizes` is true)
///             - on_bytes: lua callback invoked on change.
///               This callback receives more granular information about the
///               change compared to on_lines.
///               Return `true` to detach.
///               Args:
///               - the string "bytes"
///               - buffer handle
///               - b:changedtick
///               - start row of the changed text (zero-indexed)
///               - start column of the changed text
///               - byte offset of the changed text (from the start of
///                   the buffer)
///               - old end row of the changed text
///               - old end column of the changed text
///               - old end byte length of the changed text
///               - new end row of the changed text
///               - new end column of the changed text
///               - new end byte length of the changed text
///             - on_changedtick: Lua callback invoked on changedtick
///               increment without text change. Args:
///               - the string "changedtick"
///               - buffer handle
///               - b:changedtick
///             - on_detach: Lua callback invoked on detach. Args:
///               - the string "detach"
///               - buffer handle
///             - on_reload: Lua callback invoked on reload. The entire buffer
///                          content should be considered changed. Args:
///               - the string "detach"
///               - buffer handle
///             - utf_sizes: include UTF-32 and UTF-16 size of the replaced
///               region, as args to `on_lines`.
///             - preview: also attach to command preview (i.e. 'inccommand')
///               events.
/// @param[out] err Error details, if any
/// @return False if attach failed (invalid parameter, or buffer isn't loaded);
///         otherwise True. TODO: LUA_API_NO_EVAL
Boolean nvim_buf_attach(uint64_t channel_id, Buffer buffer, Boolean send_buffer,
                        DictionaryOf(LuaRef) opts, Error *err)
  FUNC_API_SINCE(4)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return false;
  }

  bool is_lua = (channel_id == LUA_INTERNAL_CALL);
  BufUpdateCallbacks cb = BUF_UPDATE_CALLBACKS_INIT;
  struct {
    const char *name;
    LuaRef *dest;
  } cbs[] = {
    { "on_lines", &cb.on_lines },
    { "on_bytes", &cb.on_bytes },
    { "on_changedtick", &cb.on_changedtick },
    { "on_detach", &cb.on_detach },
    { "on_reload", &cb.on_reload },
    { NULL, NULL },
  };

  for (size_t i = 0; i < opts.size; i++) {
    String k = opts.items[i].key;
    Object *v = &opts.items[i].value;
    bool key_used = false;
    if (is_lua) {
      for (size_t j = 0; cbs[j].name; j++) {
        if (strequal(cbs[j].name, k.data)) {
          if (v->type != kObjectTypeLuaRef) {
            api_set_error(err, kErrorTypeValidation,
                          "%s is not a function", cbs[j].name);
            goto error;
          }
          *(cbs[j].dest) = v->data.luaref;
          v->data.luaref = LUA_NOREF;
          key_used = true;
          break;
        }
      }

      if (key_used) {
        continue;
      } else if (strequal("utf_sizes", k.data)) {
        if (v->type != kObjectTypeBoolean) {
          api_set_error(err, kErrorTypeValidation, "utf_sizes must be boolean");
          goto error;
        }
        cb.utf_sizes = v->data.boolean;
        key_used = true;
      } else if (strequal("preview", k.data)) {
        if (v->type != kObjectTypeBoolean) {
          api_set_error(err, kErrorTypeValidation, "preview must be boolean");
          goto error;
        }
        cb.preview = v->data.boolean;
        key_used = true;
      }
    }

    if (!key_used) {
      api_set_error(err, kErrorTypeValidation, "unexpected key: %s", k.data);
      goto error;
    }
  }

  return buf_updates_register(buf, channel_id, cb, send_buffer);

error:
  buffer_update_callbacks_free(cb);
  return false;
}

/// Deactivates buffer-update events on the channel.
///
/// @see |nvim_buf_attach()|
/// @see |api-lua-detach| for detaching Lua callbacks
///
/// @param channel_id
/// @param buffer Buffer handle, or 0 for current buffer
/// @param[out] err Error details, if any
/// @return False if detach failed (because the buffer isn't loaded);
///         otherwise True.
Boolean nvim_buf_detach(uint64_t channel_id, Buffer buffer, Error *err)
  FUNC_API_SINCE(4) FUNC_API_REMOTE_ONLY
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return false;
  }

  buf_updates_unregister(buf, channel_id);
  return true;
}

void nvim__buf_redraw_range(Buffer buffer, Integer first, Integer last, Error *err)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);
  if (!buf) {
    return;
  }

  redraw_buf_range_later(buf, (linenr_T)first+1, (linenr_T)last);
}

/// Gets a line-range from the buffer.
///
/// Indexing is zero-based, end-exclusive. Negative indices are interpreted
/// as length+1+index: -1 refers to the index past the end. So to get the
/// last element use start=-2 and end=-1.
///
/// Out-of-bounds indices are clamped to the nearest valid value, unless
/// `strict_indexing` is set.
///
/// @param channel_id
/// @param buffer           Buffer handle, or 0 for current buffer
/// @param start            First line index
/// @param end              Last line index (exclusive)
/// @param strict_indexing  Whether out-of-bounds should be an error.
/// @param[out] err         Error details, if any
/// @return Array of lines, or empty array for unloaded buffer.
ArrayOf(String) nvim_buf_get_lines(uint64_t channel_id,
                                   Buffer buffer,
                                   Integer start,
                                   Integer end,
                                   Boolean strict_indexing,
                                   Error *err)
  FUNC_API_SINCE(1)
{
  Array rv = ARRAY_DICT_INIT;
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return rv;
  }

  // return sentinel value if the buffer isn't loaded
  if (buf->b_ml.ml_mfp == NULL) {
    return rv;
  }

  bool oob = false;
  start = normalize_index(buf, start, &oob);
  end = normalize_index(buf, end, &oob);

  if (strict_indexing && oob) {
    api_set_error(err, kErrorTypeValidation, "Index out of bounds");
    return rv;
  }

  if (start >= end) {
    // Return 0-length array
    return rv;
  }

  rv.size = (size_t)(end - start);
  rv.items = xcalloc(rv.size, sizeof(Object));

  if (!buf_collect_lines(buf, rv.size, start,
                         (channel_id != VIML_INTERNAL_CALL), &rv, err)) {
    goto end;
  }

end:
  if (ERROR_SET(err)) {
    for (size_t i = 0; i < rv.size; i++) {
      xfree(rv.items[i].data.string.data);
    }

    xfree(rv.items);
    rv.items = NULL;
  }

  return rv;
}

static bool check_string_array(Array arr, bool disallow_nl, Error *err)
{
  for (size_t i = 0; i < arr.size; i++) {
    if (arr.items[i].type != kObjectTypeString) {
      api_set_error(err,
                    kErrorTypeValidation,
                    "All items in the replacement array must be strings");
      return false;
    }
    // Disallow newlines in the middle of the line.
    if (disallow_nl) {
      const String l = arr.items[i].data.string;
      if (memchr(l.data, NL, l.size)) {
        api_set_error(err, kErrorTypeValidation,
                      "String cannot contain newlines");
        return false;
      }
    }
  }
  return true;
}

/// Sets (replaces) a line-range in the buffer.
///
/// Indexing is zero-based, end-exclusive. Negative indices are interpreted
/// as length+1+index: -1 refers to the index past the end. So to change
/// or delete the last element use start=-2 and end=-1.
///
/// To insert lines at a given index, set `start` and `end` to the same index.
/// To delete a range of lines, set `replacement` to an empty array.
///
/// Out-of-bounds indices are clamped to the nearest valid value, unless
/// `strict_indexing` is set.
///
/// @param channel_id
/// @param buffer           Buffer handle, or 0 for current buffer
/// @param start            First line index
/// @param end              Last line index (exclusive)
/// @param strict_indexing  Whether out-of-bounds should be an error.
/// @param replacement      Array of lines to use as replacement
/// @param[out] err         Error details, if any
void nvim_buf_set_lines(uint64_t channel_id, Buffer buffer, Integer start, Integer end,
                        Boolean strict_indexing, ArrayOf(String) replacement, Error *err)
  FUNC_API_SINCE(1)
  FUNC_API_CHECK_TEXTLOCK
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return;
  }

  bool oob = false;
  start = normalize_index(buf, start, &oob);
  end = normalize_index(buf, end, &oob);

  if (strict_indexing && oob) {
    api_set_error(err, kErrorTypeValidation, "Index out of bounds");
    return;
  }


  if (start > end) {
    api_set_error(err,
                  kErrorTypeValidation,
                  "Argument \"start\" is higher than \"end\"");
    return;
  }

  bool disallow_nl = (channel_id != VIML_INTERNAL_CALL);
  if (!check_string_array(replacement, disallow_nl, err)) {
    return;
  }

  size_t new_len = replacement.size;
  size_t old_len = (size_t)(end - start);
  ptrdiff_t extra = 0;  // lines added to text, can be negative
  char **lines = (new_len != 0) ? xcalloc(new_len, sizeof(char *)) : NULL;

  for (size_t i = 0; i < new_len; i++) {
    const String l = replacement.items[i].data.string;

    // Fill lines[i] with l's contents. Convert NULs to newlines as required by
    // NL-used-for-NUL.
    lines[i] = xmemdupz(l.data, l.size);
    memchrsub(lines[i], NUL, NL, l.size);
  }

  try_start();
  aco_save_T aco;
  aucmd_prepbuf(&aco, buf);

  if (!MODIFIABLE(buf)) {
    api_set_error(err, kErrorTypeException, "Buffer is not 'modifiable'");
    goto end;
  }

  if (u_save((linenr_T)(start - 1), (linenr_T)end) == FAIL) {
    api_set_error(err, kErrorTypeException, "Failed to save undo information");
    goto end;
  }

  bcount_t deleted_bytes = get_region_bytecount(curbuf, start, end, 0, 0);

  // If the size of the range is reducing (ie, new_len < old_len) we
  // need to delete some old_len. We do this at the start, by
  // repeatedly deleting line "start".
  size_t to_delete = (new_len < old_len) ? (size_t)(old_len - new_len) : 0;
  for (size_t i = 0; i < to_delete; i++) {
    if (ml_delete((linenr_T)start, false) == FAIL) {
      api_set_error(err, kErrorTypeException, "Failed to delete line");
      goto end;
    }
  }

  if (to_delete > 0) {
    extra -= (ptrdiff_t)to_delete;
  }

  // For as long as possible, replace the existing old_len with the
  // new old_len. This is a more efficient operation, as it requires
  // less memory allocation and freeing.
  size_t to_replace = old_len < new_len ? old_len : new_len;
  bcount_t inserted_bytes = 0;
  for (size_t i = 0; i < to_replace; i++) {
    int64_t lnum = start + (int64_t)i;

    if (lnum >= MAXLNUM) {
      api_set_error(err, kErrorTypeValidation, "Index value is too high");
      goto end;
    }

    if (ml_replace((linenr_T)lnum, (char_u *)lines[i], false) == FAIL) {
      api_set_error(err, kErrorTypeException, "Failed to replace line");
      goto end;
    }

    inserted_bytes += (bcount_t)strlen(lines[i]) + 1;
    // Mark lines that haven't been passed to the buffer as they need
    // to be freed later
    lines[i] = NULL;
  }

  // Now we may need to insert the remaining new old_len
  for (size_t i = to_replace; i < new_len; i++) {
    int64_t lnum = start + (int64_t)i - 1;

    if (lnum >= MAXLNUM) {
      api_set_error(err, kErrorTypeValidation, "Index value is too high");
      goto end;
    }

    if (ml_append((linenr_T)lnum, (char_u *)lines[i], 0, false) == FAIL) {
      api_set_error(err, kErrorTypeException, "Failed to insert line");
      goto end;
    }

    inserted_bytes += (bcount_t)strlen(lines[i]) + 1;

    // Same as with replacing, but we also need to free lines
    xfree(lines[i]);
    lines[i] = NULL;
    extra++;
  }

  // Adjust marks. Invalidate any which lie in the
  // changed range, and move any in the remainder of the buffer.
  // Only adjust marks if we managed to switch to a window that holds
  // the buffer, otherwise line numbers will be invalid.
  mark_adjust((linenr_T)start,
              (linenr_T)(end - 1),
              MAXLNUM,
              (long)extra,
              kExtmarkNOOP);

  extmark_splice(curbuf, (int)start-1, 0, (int)(end-start), 0,
                 deleted_bytes, (int)new_len, 0, inserted_bytes,
                 kExtmarkUndo);

  changed_lines((linenr_T)start, 0, (linenr_T)end, (long)extra, true);
  fix_cursor((linenr_T)start, (linenr_T)end, (linenr_T)extra);

end:
  for (size_t i = 0; i < new_len; i++) {
    xfree(lines[i]);
  }

  xfree(lines);
  aucmd_restbuf(&aco);
  try_end(err);
}

/// Sets (replaces) a range in the buffer
///
/// This is recommended over nvim_buf_set_lines when only modifying parts of a
/// line, as extmarks will be preserved on non-modified parts of the touched
/// lines.
///
/// Indexing is zero-based and end-exclusive.
///
/// To insert text at a given index, set `start` and `end` ranges to the same
/// index. To delete a range, set `replacement` to an array containing
/// an empty string, or simply an empty array.
///
/// Prefer nvim_buf_set_lines when adding or deleting entire lines only.
///
/// @param channel_id
/// @param buffer           Buffer handle, or 0 for current buffer
/// @param start_row        First line index
/// @param start_column     Last column
/// @param end_row          Last line index
/// @param end_column       Last column
/// @param replacement      Array of lines to use as replacement
/// @param[out] err         Error details, if any
void nvim_buf_set_text(uint64_t channel_id, Buffer buffer, Integer start_row, Integer start_col,
                       Integer end_row, Integer end_col, ArrayOf(String) replacement, Error *err)
  FUNC_API_SINCE(7)
{
  FIXED_TEMP_ARRAY(scratch, 1);
  if (replacement.size == 0) {
    scratch.items[0] = STRING_OBJ(STATIC_CSTR_AS_STRING(""));
    replacement = scratch;
  }

  buf_T *buf = find_buffer_by_handle(buffer, err);
  if (!buf) {
    return;
  }

  bool oob = false;

  // check range is ordered and everything!
  // start_row, end_row within buffer len (except add text past the end?)
  start_row = normalize_index(buf, start_row, &oob);
  if (oob || start_row == buf->b_ml.ml_line_count + 1) {
    api_set_error(err, kErrorTypeValidation, "start_row out of bounds");
    return;
  }

  end_row = normalize_index(buf, end_row, &oob);
  if (oob || end_row == buf->b_ml.ml_line_count + 1) {
    api_set_error(err, kErrorTypeValidation, "end_row out of bounds");
    return;
  }

  char *str_at_start = (char *)ml_get_buf(buf, start_row, false);
  if (start_col < 0 || (size_t)start_col > strlen(str_at_start)) {
    api_set_error(err, kErrorTypeValidation, "start_col out of bounds");
    return;
  }

  char *str_at_end = (char *)ml_get_buf(buf, end_row, false);
  size_t len_at_end = strlen(str_at_end);
  if (end_col < 0 || (size_t)end_col > len_at_end) {
    api_set_error(err, kErrorTypeValidation, "end_col out of bounds");
    return;
  }

  if (start_row > end_row || (end_row == start_row && start_col > end_col)) {
    api_set_error(err, kErrorTypeValidation, "start is higher than end");
    return;
  }

  bool disallow_nl = (channel_id != VIML_INTERNAL_CALL);
  if (!check_string_array(replacement, disallow_nl, err)) {
    return;
  }

  size_t new_len = replacement.size;

  bcount_t new_byte = 0;
  bcount_t old_byte = 0;

  // calculate byte size of old region before it gets modified/deleted
  if (start_row == end_row) {
    old_byte = (bcount_t)end_col - start_col;
  } else {
    const char *bufline;
    old_byte += (bcount_t)strlen(str_at_start) - start_col;
    for (int64_t i = 1; i < end_row - start_row; i++) {
      int64_t lnum = start_row + i;

      bufline = (char *)ml_get_buf(buf, lnum, false);
      old_byte += (bcount_t)(strlen(bufline))+1;
    }
    old_byte += (bcount_t)end_col+1;
  }

  String first_item = replacement.items[0].data.string;
  String last_item = replacement.items[replacement.size-1].data.string;

  size_t firstlen = (size_t)start_col+first_item.size;
  size_t last_part_len = strlen(str_at_end) - (size_t)end_col;
  if (replacement.size == 1) {
    firstlen += last_part_len;
  }
  char *first = xmallocz(firstlen);
  char *last = NULL;
  memcpy(first, str_at_start, (size_t)start_col);
  memcpy(first+start_col, first_item.data, first_item.size);
  memchrsub(first+start_col, NUL, NL, first_item.size);
  if (replacement.size == 1) {
    memcpy(first+start_col+first_item.size, str_at_end+end_col, last_part_len);
  } else {
    last = xmallocz(last_item.size+last_part_len);
    memcpy(last, last_item.data, last_item.size);
    memchrsub(last, NUL, NL, last_item.size);
    memcpy(last+last_item.size, str_at_end+end_col, last_part_len);
  }

  char **lines = xcalloc(new_len, sizeof(char *));
  lines[0] = first;
  new_byte += (bcount_t)(first_item.size);
  for (size_t i = 1; i < new_len-1; i++) {
    const String l = replacement.items[i].data.string;

    // Fill lines[i] with l's contents. Convert NULs to newlines as required by
    // NL-used-for-NUL.
    lines[i] = xmemdupz(l.data, l.size);
    memchrsub(lines[i], NUL, NL, l.size);
    new_byte += (bcount_t)(l.size)+1;
  }
  if (replacement.size > 1) {
    lines[replacement.size-1] = last;
    new_byte += (bcount_t)(last_item.size)+1;
  }

  try_start();
  aco_save_T aco;
  aucmd_prepbuf(&aco, buf);

  if (!MODIFIABLE(buf)) {
    api_set_error(err, kErrorTypeException, "Buffer is not 'modifiable'");
    goto end;
  }

  // Small note about undo states: unlike set_lines, we want to save the
  // undo state of one past the end_row, since end_row is inclusive.
  if (u_save((linenr_T)start_row - 1, (linenr_T)end_row + 1) == FAIL) {
    api_set_error(err, kErrorTypeException, "Failed to save undo information");
    goto end;
  }

  ptrdiff_t extra = 0;  // lines added to text, can be negative
  size_t old_len = (size_t)(end_row-start_row+1);

  // If the size of the range is reducing (ie, new_len < old_len) we
  // need to delete some old_len. We do this at the start, by
  // repeatedly deleting line "start".
  size_t to_delete = (new_len < old_len) ? (size_t)(old_len - new_len) : 0;
  for (size_t i = 0; i < to_delete; i++) {
    if (ml_delete((linenr_T)start_row, false) == FAIL) {
      api_set_error(err, kErrorTypeException, "Failed to delete line");
      goto end;
    }
  }

  if (to_delete > 0) {
    extra -= (ptrdiff_t)to_delete;
  }

  // For as long as possible, replace the existing old_len with the
  // new old_len. This is a more efficient operation, as it requires
  // less memory allocation and freeing.
  size_t to_replace = old_len < new_len ? old_len : new_len;
  for (size_t i = 0; i < to_replace; i++) {
    int64_t lnum = start_row + (int64_t)i;

    if (lnum >= MAXLNUM) {
      api_set_error(err, kErrorTypeValidation, "Index value is too high");
      goto end;
    }

    if (ml_replace((linenr_T)lnum, (char_u *)lines[i], false) == FAIL) {
      api_set_error(err, kErrorTypeException, "Failed to replace line");
      goto end;
    }
    // Mark lines that haven't been passed to the buffer as they need
    // to be freed later
    lines[i] = NULL;
  }

  // Now we may need to insert the remaining new old_len
  for (size_t i = to_replace; i < new_len; i++) {
    int64_t lnum = start_row + (int64_t)i - 1;

    if (lnum >= MAXLNUM) {
      api_set_error(err, kErrorTypeValidation, "Index value is too high");
      goto end;
    }

    if (ml_append((linenr_T)lnum, (char_u *)lines[i], 0, false) == FAIL) {
      api_set_error(err, kErrorTypeException, "Failed to insert line");
      goto end;
    }

    // Same as with replacing, but we also need to free lines
    xfree(lines[i]);
    lines[i] = NULL;
    extra++;
  }

  // Adjust marks. Invalidate any which lie in the
  // changed range, and move any in the remainder of the buffer.
  mark_adjust((linenr_T)start_row,
              (linenr_T)end_row,
              MAXLNUM,
              (long)extra,
              kExtmarkNOOP);

  colnr_T col_extent = (colnr_T)(end_col
                                 - ((end_row == start_row) ? start_col : 0));
  extmark_splice(buf, (int)start_row-1, (colnr_T)start_col,
                 (int)(end_row-start_row), col_extent, old_byte,
                 (int)new_len-1, (colnr_T)last_item.size, new_byte,
                 kExtmarkUndo);


  changed_lines((linenr_T)start_row, 0, (linenr_T)end_row + 1,
                (long)extra, true);

  // adjust cursor like an extmark ( i e it was inside last_part_len)
  if (curwin->w_cursor.lnum == end_row && curwin->w_cursor.col > end_col) {
    curwin->w_cursor.col -= col_extent - (colnr_T)last_item.size;
  }
  fix_cursor((linenr_T)start_row, (linenr_T)end_row, (linenr_T)extra);

end:
  for (size_t i = 0; i < new_len; i++) {
    xfree(lines[i]);
  }
  xfree(lines);
  aucmd_restbuf(&aco);
  try_end(err);
}

/// Returns the byte offset of a line (0-indexed). |api-indexing|
///
/// Line 1 (index=0) has offset 0. UTF-8 bytes are counted. EOL is one byte.
/// 'fileformat' and 'fileencoding' are ignored. The line index just after the
/// last line gives the total byte-count of the buffer. A final EOL byte is
/// counted if it would be written, see 'eol'.
///
/// Unlike |line2byte()|, throws error for out-of-bounds indexing.
/// Returns -1 for unloaded buffer.
///
/// @param buffer     Buffer handle, or 0 for current buffer
/// @param index      Line index
/// @param[out] err   Error details, if any
/// @return Integer byte offset, or -1 for unloaded buffer.
Integer nvim_buf_get_offset(Buffer buffer, Integer index, Error *err)
  FUNC_API_SINCE(5)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);
  if (!buf) {
    return 0;
  }

  // return sentinel value if the buffer isn't loaded
  if (buf->b_ml.ml_mfp == NULL) {
    return -1;
  }

  if (index < 0 || index > buf->b_ml.ml_line_count) {
    api_set_error(err, kErrorTypeValidation, "Index out of bounds");
    return 0;
  }

  return ml_find_line_or_offset(buf, (int)index+1, NULL, true);
}

/// Gets a buffer-scoped (b:) variable.
///
/// @param buffer     Buffer handle, or 0 for current buffer
/// @param name       Variable name
/// @param[out] err   Error details, if any
/// @return Variable value
Object nvim_buf_get_var(Buffer buffer, String name, Error *err)
  FUNC_API_SINCE(1)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return (Object)OBJECT_INIT;
  }

  return dict_get_value(buf->b_vars, name, err);
}

/// Gets a changed tick of a buffer
///
/// @param[in]  buffer  Buffer handle, or 0 for current buffer
/// @param[out] err     Error details, if any
///
/// @return `b:changedtick` value.
Integer nvim_buf_get_changedtick(Buffer buffer, Error *err)
  FUNC_API_SINCE(2)
{
  const buf_T *const buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return -1;
  }

  return buf_get_changedtick(buf);
}

/// Gets a list of buffer-local |mapping| definitions.
///
/// @param  mode       Mode short-name ("n", "i", "v", ...)
/// @param  buffer     Buffer handle, or 0 for current buffer
/// @param[out]  err   Error details, if any
/// @returns Array of maparg()-like dictionaries describing mappings.
///          The "buffer" key holds the associated buffer handle.
ArrayOf(Dictionary) nvim_buf_get_keymap(Buffer buffer, String mode, Error *err)
  FUNC_API_SINCE(3)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return (Array)ARRAY_DICT_INIT;
  }

  return keymap_array(mode, buf);
}

/// Sets a buffer-local |mapping| for the given mode.
///
/// @see |nvim_set_keymap()|
///
/// @param  buffer  Buffer handle, or 0 for current buffer
void nvim_buf_set_keymap(Buffer buffer, String mode, String lhs, String rhs, Dict(keymap) *opts,
                         Error *err)
  FUNC_API_SINCE(6)
{
  modify_keymap(buffer, false, mode, lhs, rhs, opts, err);
}

/// Unmaps a buffer-local |mapping| for the given mode.
///
/// @see |nvim_del_keymap()|
///
/// @param  buffer  Buffer handle, or 0 for current buffer
void nvim_buf_del_keymap(Buffer buffer, String mode, String lhs, Error *err)
  FUNC_API_SINCE(6)
{
  String rhs = { .data = "", .size = 0 };
  modify_keymap(buffer, true, mode, lhs, rhs, NULL, err);
}

/// Gets a map of buffer-local |user-commands|.
///
/// @param  buffer  Buffer handle, or 0 for current buffer
/// @param  opts  Optional parameters. Currently not used.
/// @param[out]  err   Error details, if any.
///
/// @returns Map of maps describing commands.
Dictionary nvim_buf_get_commands(Buffer buffer, Dict(get_commands) *opts, Error *err)
  FUNC_API_SINCE(4)
{
  bool global = (buffer == -1);
  bool builtin = api_object_to_bool(opts->builtin, "builtin", false, err);
  if (ERROR_SET(err)) {
    return (Dictionary)ARRAY_DICT_INIT;
  }

  if (global) {
    if (builtin) {
      api_set_error(err, kErrorTypeValidation, "builtin=true not implemented");
      return (Dictionary)ARRAY_DICT_INIT;
    }
    return commands_array(NULL);
  }

  buf_T *buf = find_buffer_by_handle(buffer, err);
  if (builtin || !buf) {
    return (Dictionary)ARRAY_DICT_INIT;
  }
  return commands_array(buf);
}

/// Sets a buffer-scoped (b:) variable
///
/// @param buffer     Buffer handle, or 0 for current buffer
/// @param name       Variable name
/// @param value      Variable value
/// @param[out] err   Error details, if any
void nvim_buf_set_var(Buffer buffer, String name, Object value, Error *err)
  FUNC_API_SINCE(1)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return;
  }

  dict_set_var(buf->b_vars, name, value, false, false, err);
}

/// Removes a buffer-scoped (b:) variable
///
/// @param buffer     Buffer handle, or 0 for current buffer
/// @param name       Variable name
/// @param[out] err   Error details, if any
void nvim_buf_del_var(Buffer buffer, String name, Error *err)
  FUNC_API_SINCE(1)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return;
  }

  dict_set_var(buf->b_vars, name, NIL, true, false, err);
}


/// Gets a buffer option value
///
/// @param buffer     Buffer handle, or 0 for current buffer
/// @param name       Option name
/// @param[out] err   Error details, if any
/// @return Option value
Object nvim_buf_get_option(Buffer buffer, String name, Error *err)
  FUNC_API_SINCE(1)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return (Object)OBJECT_INIT;
  }

  return get_option_from(buf, SREQ_BUF, name, err);
}

/// Sets a buffer option value. Passing 'nil' as value deletes the option (only
/// works if there's a global fallback)
///
/// @param channel_id
/// @param buffer     Buffer handle, or 0 for current buffer
/// @param name       Option name
/// @param value      Option value
/// @param[out] err   Error details, if any
void nvim_buf_set_option(uint64_t channel_id, Buffer buffer, String name, Object value, Error *err)
  FUNC_API_SINCE(1)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return;
  }

  set_option_to(channel_id, buf, SREQ_BUF, name, value, err);
}

/// Gets the full file name for the buffer
///
/// @param buffer     Buffer handle, or 0 for current buffer
/// @param[out] err   Error details, if any
/// @return Buffer name
String nvim_buf_get_name(Buffer buffer, Error *err)
  FUNC_API_SINCE(1)
{
  String rv = STRING_INIT;
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf || buf->b_ffname == NULL) {
    return rv;
  }

  return cstr_to_string((char *)buf->b_ffname);
}

/// Sets the full file name for a buffer
///
/// @param buffer     Buffer handle, or 0 for current buffer
/// @param name       Buffer name
/// @param[out] err   Error details, if any
void nvim_buf_set_name(Buffer buffer, String name, Error *err)
  FUNC_API_SINCE(1)
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return;
  }

  try_start();

  // Using aucmd_*: autocommands will be executed by rename_buffer
  aco_save_T aco;
  aucmd_prepbuf(&aco, buf);
  int ren_ret = rename_buffer((char_u *)name.data);
  aucmd_restbuf(&aco);

  if (try_end(err)) {
    return;
  }

  if (ren_ret == FAIL) {
    api_set_error(err, kErrorTypeException, "Failed to rename buffer");
  }
}

/// Checks if a buffer is valid and loaded. See |api-buffer| for more info
/// about unloaded buffers.
///
/// @param buffer Buffer handle, or 0 for current buffer
/// @return true if the buffer is valid and loaded, false otherwise.
Boolean nvim_buf_is_loaded(Buffer buffer)
  FUNC_API_SINCE(5)
{
  Error stub = ERROR_INIT;
  buf_T *buf = find_buffer_by_handle(buffer, &stub);
  api_clear_error(&stub);
  return buf && buf->b_ml.ml_mfp != NULL;
}

/// Deletes the buffer. See |:bwipeout|
///
/// @param buffer Buffer handle, or 0 for current buffer
/// @param opts  Optional parameters. Keys:
///          - force:  Force deletion and ignore unsaved changes.
///          - unload: Unloaded only, do not delete. See |:bunload|
void nvim_buf_delete(Buffer buffer, Dictionary opts, Error *err)
  FUNC_API_SINCE(7)
  FUNC_API_CHECK_TEXTLOCK
{
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (ERROR_SET(err)) {
    return;
  }

  bool force = false;
  bool unload = false;
  for (size_t i = 0; i < opts.size; i++) {
    String k = opts.items[i].key;
    Object v = opts.items[i].value;
    if (strequal("force", k.data)) {
      force = api_object_to_bool(v, "force", false, err);
    } else if (strequal("unload", k.data)) {
      unload = api_object_to_bool(v, "unload", false, err);
    } else {
      api_set_error(err, kErrorTypeValidation, "unexpected key: %s", k.data);
      return;
    }
  }

  if (ERROR_SET(err)) {
    return;
  }

  int result = do_buffer(unload ? DOBUF_UNLOAD : DOBUF_WIPE,
                         DOBUF_FIRST,
                         FORWARD,
                         buf->handle,
                         force);

  if (result == FAIL) {
    api_set_error(err, kErrorTypeException, "Failed to unload buffer.");
    return;
  }
}

/// Checks if a buffer is valid.
///
/// @note Even if a buffer is valid it may have been unloaded. See |api-buffer|
/// for more info about unloaded buffers.
///
/// @param buffer Buffer handle, or 0 for current buffer
/// @return true if the buffer is valid, false otherwise.
Boolean nvim_buf_is_valid(Buffer buffer)
  FUNC_API_SINCE(1)
{
  Error stub = ERROR_INIT;
  Boolean ret = find_buffer_by_handle(buffer, &stub) != NULL;
  api_clear_error(&stub);
  return ret;
}

/// Deletes a named mark in the buffer. See |mark-motions|.
///
/// @note only deletes marks set in the buffer, if the mark is not set
/// in the buffer it will return false.
/// @param buffer     Buffer to set the mark on
/// @param name       Mark name
/// @return true if the mark was deleted, else false.
/// @see |nvim_buf_set_mark()|
/// @see |nvim_del_mark()|
Boolean nvim_buf_del_mark(Buffer buffer, String name, Error *err)
  FUNC_API_SINCE(8)
{
  bool res = false;
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return res;
  }

  if (name.size != 1) {
    api_set_error(err, kErrorTypeValidation,
                  "Mark name must be a single character");
    return res;
  }

  pos_T *pos = getmark_buf(buf, *name.data, false);

  // pos point to NULL when there's no mark with name
  if (pos == NULL) {
    api_set_error(err, kErrorTypeValidation, "Invalid mark name: '%c'",
                  *name.data);
    return res;
  }

  // pos->lnum is 0 when the mark is not valid in the buffer, or is not set.
  if (pos->lnum != 0) {
    // since the mark belongs to the buffer delete it.
    res = set_mark(buf, name, 0, 0, err);
  }

  return res;
}

/// Sets a named mark in the given buffer, all marks are allowed
/// file/uppercase, visual, last change, etc. See |mark-motions|.
///
/// Marks are (1,0)-indexed. |api-indexing|
///
/// @note Passing 0 as line deletes the mark
///
/// @param buffer     Buffer to set the mark on
/// @param name       Mark name
/// @param line       Line number
/// @param col        Column/row number
/// @return true if the mark was set, else false.
/// @see |nvim_buf_del_mark()|
/// @see |nvim_buf_get_mark()|
Boolean nvim_buf_set_mark(Buffer buffer, String name, Integer line, Integer col, Error *err)
  FUNC_API_SINCE(8)
{
  bool res = false;
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return res;
  }

  if (name.size != 1) {
    api_set_error(err, kErrorTypeValidation,
                  "Mark name must be a single character");
    return res;
  }

  res = set_mark(buf, name, line, col, err);

  return res;
}

/// Returns a tuple (row,col) representing the position of the named mark. See
/// |mark-motions|.
///
/// Marks are (1,0)-indexed. |api-indexing|
///
/// @param buffer     Buffer handle, or 0 for current buffer
/// @param name       Mark name
/// @param[out] err   Error details, if any
/// @return (row, col) tuple, (0, 0) if the mark is not set, or is an
/// uppercase/file mark set in another buffer.
/// @see |nvim_buf_set_mark()|
/// @see |nvim_buf_del_mark()|
ArrayOf(Integer, 2) nvim_buf_get_mark(Buffer buffer, String name, Error *err)
  FUNC_API_SINCE(1)
{
  Array rv = ARRAY_DICT_INIT;
  buf_T *buf = find_buffer_by_handle(buffer, err);

  if (!buf) {
    return rv;
  }

  if (name.size != 1) {
    api_set_error(err, kErrorTypeValidation,
                  "Mark name must be a single character");
    return rv;
  }

  pos_T *posp;
  char mark = *name.data;

  try_start();
  bufref_T save_buf;
  switch_buffer(&save_buf, buf);
  posp = getmark(mark, false);
  restore_buffer(&save_buf);

  if (try_end(err)) {
    return rv;
  }

  if (posp == NULL) {
    api_set_error(err, kErrorTypeValidation, "Invalid mark name");
    return rv;
  }

  ADD(rv, INTEGER_OBJ(posp->lnum));
  ADD(rv, INTEGER_OBJ(posp->col));

  return rv;
}


/// call a function with buffer as temporary current buffer
///
/// This temporarily switches current buffer to "buffer".
/// If the current window already shows "buffer", the window is not switched
/// If a window inside the current tabpage (including a float) already shows the
/// buffer One of these windows will be set as current window temporarily.
/// Otherwise a temporary scratch window (calleed the "autocmd window" for
/// historical reasons) will be used.
///
/// This is useful e.g. to call vimL functions that only work with the current
/// buffer/window currently, like |termopen()|.
///
/// @param buffer     Buffer handle, or 0 for current buffer
/// @param fun        Function to call inside the buffer (currently lua callable
///                   only)
/// @param[out] err   Error details, if any
/// @return           Return value of function. NB: will deepcopy lua values
///                   currently, use upvalues to send lua references in and out.
Object nvim_buf_call(Buffer buffer, LuaRef fun, Error *err)
  FUNC_API_SINCE(7)
  FUNC_API_LUA_ONLY
{
  buf_T *buf = find_buffer_by_handle(buffer, err);
  if (!buf) {
    return NIL;
  }
  try_start();
  aco_save_T aco;
  aucmd_prepbuf(&aco, buf);

  Array args = ARRAY_DICT_INIT;
  Object res = nlua_call_ref(fun, NULL, args, true, err);

  aucmd_restbuf(&aco);
  try_end(err);
  return res;
}

Dictionary nvim__buf_stats(Buffer buffer, Error *err)
{
  Dictionary rv = ARRAY_DICT_INIT;

  buf_T *buf = find_buffer_by_handle(buffer, err);
  if (!buf) {
    return rv;
  }

  // Number of times the cached line was flushed.
  // This should generally not increase while editing the same
  // line in the same mode.
  PUT(rv, "flush_count", INTEGER_OBJ(buf->flush_count));
  // lnum of current line
  PUT(rv, "current_lnum", INTEGER_OBJ(buf->b_ml.ml_line_lnum));
  // whether the line has unflushed changes.
  PUT(rv, "line_dirty", BOOLEAN_OBJ(buf->b_ml.ml_flags & ML_LINE_DIRTY));
  // NB: this should be zero at any time API functions are called,
  // this exists to debug issues
  PUT(rv, "dirty_bytes", INTEGER_OBJ((Integer)buf->deleted_bytes));
  PUT(rv, "dirty_bytes2", INTEGER_OBJ((Integer)buf->deleted_bytes2));
  PUT(rv, "virt_blocks", INTEGER_OBJ((Integer)buf->b_virt_line_blocks));

  u_header_T *uhp = NULL;
  if (buf->b_u_curhead != NULL) {
    uhp = buf->b_u_curhead;
  } else if (buf->b_u_newhead) {
    uhp = buf->b_u_newhead;
  }
  if (uhp) {
    PUT(rv, "uhp_extmark_size", INTEGER_OBJ((Integer)kv_size(uhp->uh_extmark)));
  }

  return rv;
}

// Check if deleting lines made the cursor position invalid.
// Changed lines from `lo` to `hi`; added `extra` lines (negative if deleted).
static void fix_cursor(linenr_T lo, linenr_T hi, linenr_T extra)
{
  if (curwin->w_cursor.lnum >= lo) {
    // Adjust cursor position if it's in/after the changed lines.
    if (curwin->w_cursor.lnum >= hi) {
      curwin->w_cursor.lnum += extra;
      check_cursor_col();
    } else if (extra < 0) {
      check_cursor();
    } else {
      check_cursor_col();
    }
    changed_cline_bef_curs();
  }
  invalidate_botline();
}

// Normalizes 0-based indexes to buffer line numbers
static int64_t normalize_index(buf_T *buf, int64_t index, bool *oob)
{
  int64_t line_count = buf->b_ml.ml_line_count;
  // Fix if < 0
  index = index < 0 ? line_count + index +1 : index;

  // Check for oob
  if (index > line_count) {
    *oob = true;
    index = line_count;
  } else if (index < 0) {
    *oob = true;
    index = 0;
  }
  // Convert the index to a vim line number
  index++;
  return index;
}
