local core = require("libmdf.core")
local mdf = {}

mdf.token = core.token
mdf.status = core.status
mdf.version = core.version
mdf.version_major = core.version_major
mdf.version_minor = core.version_minor
mdf.version_patch = core.version_patch

---Create a renderer handle. Bind one persistent output callback with
---`handle:set_sink(write)` before calling its streaming receiver methods.
---A `render_stream` reader may change width, but cannot reset or replace the
---sink until that synchronous render returns.
---Use `feed`, `flush`, `finish_document`, and `begin_document` for incremental
---documents; use `write_token` and `finish` for the manual-token surface.
function mdf.new(opts)
  return core.new(opts or {})
end

---Alias for `mdf.new`.
function mdf.create(opts)
  return core.create(opts or {})
end

---Render a complete string and return a materialized string.
function mdf.render(markdown, opts)
  return core.render(markdown or "", opts or {})
end

---Stream through explicit per-call reader and writer callbacks without binding
---the writer to a persistent handle.
function mdf.render_stream(read, write, opts)
  return core.render_stream(read, write, opts or {})
end

---Create an incremental document stream with one persistent writer callback.
---Use `set_width`, `reset`, `set_sink`, or `set_html_title` on the returned
---stream as needed; `error` exposes the latest core diagnostic and `close` or
---`destroy` releases the renderer, except it is rejected while one of its
---callbacks is active. Callbacks run on the Lua state invoking each method, so
---a stream returned from a collected coroutine remains usable.
---A failed terminal reset reports an error after discarding current state.
---Rebinding the identical callback leaves the current document intact.
---Replacing a callback discards state and still binds the new callback if old
---terminal cleanup reports a write failure; callers replay their own source.
function mdf.document_stream(opts, write)
  return core.document_stream(opts or {}, write)
end

function mdf.pager(path, opts)
  return core.pager(path, opts or {})
end

function mdf.html_font_dump_paths(opts)
  return core.html_font_dump_paths(opts or {})
end

function mdf.paths_alias(first_path, second_path)
  return core.paths_alias(first_path, second_path)
end

function mdf.path_aliases_stdout(path)
  return core.path_aliases_stdout(path)
end

function mdf.theme_names()
  return core.theme_names()
end

function mdf.theme_exists(name)
  return core.theme_exists(name)
end

function mdf.status_string(status)
  return core.status_string(status)
end

function mdf.detect_osc8_support()
  return core.detect_osc8_support()
end

function mdf.terminal_width(fd, fallback)
  return core.terminal_width(fd, fallback)
end

return mdf
