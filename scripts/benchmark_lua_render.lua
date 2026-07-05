local mdf = require("libmdf")

local opts = {}
local input
local i = 1

while i <= #arg do
  local a = arg[i]
  if a == "--ansi" then
    opts.format = "ansi"
  elseif a == "--html" then
    opts.format = "html"
  elseif a == "--deck" then
    opts.format = "deck"
  elseif a == "-w" or a == "--width" then
    i = i + 1
    opts.width = tonumber(arg[i])
    opts.html_content_width_ch = opts.width
  elseif a == "--boring" then
    opts.boring = true
  elseif a == "--margin-left" then
    i = i + 1
    opts.margin_left = tonumber(arg[i])
  elseif a == "--margin-right" then
    i = i + 1
    opts.margin_right = tonumber(arg[i])
  elseif a == "--slide-numbers" then
    opts.slide_numbers = true
  elseif a == "-x" or a == "--transition" then
    i = i + 1
    opts.deck_transition = arg[i]
  elseif a:sub(1, 1) == "-" then
    error("unknown option: " .. a)
  elseif not input then
    input = a
  else
    error("expected one input")
  end
  i = i + 1
end

assert(input, "missing input")

local f = assert(io.open(input, "rb"))
local eof = false

mdf.render_stream(function(cap)
  if eof then return nil end
  local chunk = f:read(math.min(cap, 4096))
  if not chunk or #chunk == 0 then
    eof = true
    f:close()
    return nil
  end
  return chunk
end, function(chunk)
  io.write(chunk)
end, opts)
