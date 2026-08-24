local core = require("libmdf.core")
local mdf = {}

mdf.token = core.token
mdf.status = core.status
mdf.version = core.version
mdf.version_major = core.version_major
mdf.version_minor = core.version_minor
mdf.version_patch = core.version_patch

function mdf.new(opts)
  return core.new(opts or {})
end

function mdf.create(opts)
  return core.create(opts or {})
end

function mdf.render(markdown, opts)
  return core.render(markdown or "", opts or {})
end

function mdf.render_stream(read, write, opts)
  return core.render_stream(read, write, opts or {})
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
