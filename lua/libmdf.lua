local core = require("libmdf.core")
local mdf = {}

---@alias libmdf.Reader fun(cap: integer): string|nil
---@alias libmdf.Writer fun(chunk: string): boolean|nil
---@alias libmdf.Trace fun(format: string, chunk: string): boolean|nil

---@class libmdf.FontOptions
---@field family? string CSS family name.
---@field regular_format? "woff2"|"ttf" Format of `regular_data`.
---@field regular_data? string Regular face bytes.
---@field italic_format? "woff2"|"ttf" Format of `italic_data`.
---@field italic_data? string Italic face bytes.

---@class libmdf.Options
---@field format? "ansi"|"html"|"deck"|"html_deck" Output format.
---@field html? boolean Select HTML output.
---@field deck? boolean Select HTML deck output.
---@field boring? boolean Disable ANSI decoration.
---@field osc8? boolean Enable ANSI OSC8 hyperlinks when supported.
---@field width? integer Total ANSI width before margins.
---@field margin_left? integer ANSI columns left of content.
---@field margin_right? integer ANSI columns right of content.
---@field theme? string Built-in ANSI theme name.
---@field html_content_width_ch? number HTML content width in `ch` units.
---@field html_title? string Explicit HTML or deck title.
---@field deck_transition? "fade"|"cross"|"hard" Deck slide transition.
---@field slide_numbers? boolean Show deck slide numbers after the first slide.
---@field deck_center_front_text? boolean Center first-slide paragraph text.
---@field table_buffer_mode? "full"|"row" Table decision-buffer policy.
---@field table_wire_mode? "line"|"ascii"|"space" Table border style.
---@field write_trace? libmdf.Trace Observe completed ANSI sink writes.
---@field html_font? libmdf.FontOptions Custom embedded HTML/deck faces.
---@field disable_embedded_font? boolean Do not embed built-in HTML/deck faces.
---@field font_uri? string Shared external font URI base.
---@field font_regular_uri? string External regular-face URI.
---@field font_italic_uri? string External italic-face URI.
---@field dump_font? boolean Write selected HTML/deck font files.
---@field dump_font_force? boolean Replace existing selected font files.
---@field font_path? string Directory for selected font dumps.
---@field font_regular_path? string Regular font-dump path.
---@field font_italic_path? string Italic font-dump path.

---@class libmdf.Token
---@field type integer|string A `mdf.token` value or its lower-case name.
---@field text? string
---@field level? integer

---@class libmdf.Handle
---@field render fun(self: libmdf.Handle, markdown: string): string Materializes one complete render without using the bound sink.
---@field render_stream fun(self: libmdf.Handle, read: libmdf.Reader): boolean Streams one source to the bound sink.
---@field set_sink fun(self: libmdf.Handle, write: libmdf.Writer): boolean Replaces the persistent sink and discards an unfinished document.
---@field set_width fun(self: libmdf.Handle, width: integer): boolean Reflows un-emitted decisions only when width changes.
---@field reset fun(self: libmdf.Handle): boolean Closes ANSI state and discards current parsing state.
---@field set_html_title fun(self: libmdf.Handle, title: string|nil): boolean Sets an HTML/deck title before output begins.
---@field write_token fun(self: libmdf.Handle, token: libmdf.Token): boolean Streams one manual token to the bound sink.
---@field finish fun(self: libmdf.Handle): boolean Finishes manual-token rendering.
---@field feed fun(self: libmdf.Handle, chunk: string): boolean Feeds one nonempty incremental fragment.
---@field flush fun(self: libmdf.Handle): boolean Validates a non-EOF boundary without emitting.
---@field finish_document fun(self: libmdf.Handle): boolean Applies EOF exactly once.
---@field begin_document fun(self: libmdf.Handle): boolean Starts the next document after a successful finish.
---@field error fun(self: libmdf.Handle): string Returns the latest diagnostic.
---@field close fun(self: libmdf.Handle) Releases the native renderer outside active callbacks.
---@field destroy fun(self: libmdf.Handle) Alias for `close`.

---@class libmdf.DocumentStream
---@field write fun(self: libmdf.DocumentStream, chunk: string): boolean Feeds one nonempty incremental fragment.
---@field flush fun(self: libmdf.DocumentStream): boolean Validates a non-EOF boundary without emitting.
---@field finish_document fun(self: libmdf.DocumentStream): boolean Applies EOF exactly once.
---@field begin_document fun(self: libmdf.DocumentStream): boolean Starts the next document after a successful finish.
---@field set_sink fun(self: libmdf.DocumentStream, write: libmdf.Writer): boolean Replaces the writer and discards an unfinished document.
---@field set_width fun(self: libmdf.DocumentStream, width: integer): boolean Reflows un-emitted decisions only when width changes.
---@field reset fun(self: libmdf.DocumentStream): boolean Closes ANSI state and discards current parsing state.
---@field set_html_title fun(self: libmdf.DocumentStream, title: string|nil): boolean Sets an HTML/deck title before output begins.
---@field error fun(self: libmdf.DocumentStream): string Returns the latest diagnostic.
---@field close fun(self: libmdf.DocumentStream) Releases the native renderer outside active callbacks.
---@field destroy fun(self: libmdf.DocumentStream) Alias for `close`.

---@type table<string, integer>
mdf.token = core.token
---@type table<string, integer>
mdf.status = core.status
---@type string
mdf.version = core.version
---@type integer
mdf.version_major = core.version_major
---@type integer
mdf.version_minor = core.version_minor
---@type integer
mdf.version_patch = core.version_patch

---Create a renderer handle. Bind one persistent output callback with
---`handle:set_sink(write)` before streaming receiver methods. A reader may
---change width; sink/trace callbacks cannot mutate lifecycle state.
---@param opts? libmdf.Options
---@return libmdf.Handle
function mdf.new(opts)
  return core.new(opts or {})
end

---Alias for `mdf.new`.
---@param opts? libmdf.Options
---@return libmdf.Handle
function mdf.create(opts)
  return core.create(opts or {})
end

---Render a complete string and return a materialized string.
---@param markdown string
---@param opts? libmdf.Options
---@return string
function mdf.render(markdown, opts)
  return core.render(markdown or "", opts or {})
end

---Stream through explicit per-call reader and writer callbacks without binding
---the writer to a persistent handle. The reader returns a string up to `cap`
---or `nil` at EOF; a writer/trace callback returns `false` to fail.
---@param read libmdf.Reader
---@param write libmdf.Writer
---@param opts? libmdf.Options
---@return boolean
function mdf.render_stream(read, write, opts)
  return core.render_stream(read, write, opts or {})
end

---Create an incremental document stream with one persistent writer callback.
---`set_width` recomputes only un-emitted decisions; `reset` and `set_sink`
---discard state, so callers replay source for a complete reflow. Callbacks run
---on the Lua state making each method call, so collected coroutines are safe.
---@param opts libmdf.Options
---@param write libmdf.Writer
---@return libmdf.DocumentStream
function mdf.document_stream(opts, write)
  return core.document_stream(opts or {}, write)
end

---Interactively page a named regular file on the controlling terminal.
---@param path string
---@param opts? libmdf.Options|{format?: "markdown"|"text"|"text/markdown"}
---@return boolean
function mdf.pager(path, opts)
  return core.pager(path, opts or {})
end

---Resolve enabled HTML font-dump options without writing either font file.
---@param opts libmdf.Options
---@return string regular_path
---@return string italic_path
function mdf.html_font_dump_paths(opts)
  return core.html_font_dump_paths(opts or {})
end

---Return whether two nonempty paths identify the same destination.
---@param first_path string
---@param second_path string
---@return boolean
function mdf.paths_alias(first_path, second_path)
  return core.paths_alias(first_path, second_path)
end

---Return whether a path aliases the process's current stdout destination.
---@param path string
---@return boolean
function mdf.path_aliases_stdout(path)
  return core.path_aliases_stdout(path)
end

---Return all built-in theme names.
---@return string[]
function mdf.theme_names()
  return core.theme_names()
end

---Return whether a built-in theme matches `name` case-insensitively.
---@param name string|nil
---@return boolean
function mdf.theme_exists(name)
  return core.theme_exists(name)
end

---Return the stable description for a libmdf status value.
---@param status integer
---@return string
function mdf.status_string(status)
  return core.status_string(status)
end

---Return whether the current terminal is likely to support OSC8 hyperlinks.
---@return boolean
function mdf.detect_osc8_support()
  return core.detect_osc8_support()
end

---Return width for `fd`, then `COLUMNS`, then a positive fallback.
---@param fd? integer
---@param fallback? integer
---@return integer
function mdf.terminal_width(fd, fallback)
  return core.terminal_width(fd, fallback)
end

return mdf
