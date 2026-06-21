local core = require("libmdf.core")
local mdf = {}

mdf.token = core.token

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

function mdf.theme_names()
  return core.theme_names()
end

function mdf.theme_exists(name)
  return core.theme_exists(name)
end

function mdf.detect_osc8_support()
  return core.detect_osc8_support()
end

function mdf.terminal_width(fd, fallback)
  return core.terminal_width(fd, fallback)
end

return mdf
