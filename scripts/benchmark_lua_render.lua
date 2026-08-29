local mdf = require("libmdf")

local opts = {}
local input
local repeat_count = 1
local i = 1

while i <= #arg do
  local a = arg[i]
  if a == "--ansi" then
    opts.format = "ansi"
  elseif a == "--html" then
    opts.format = "html"
  elseif a == "--deck" then
    opts.format = "deck"
  elseif a == "--repeat" then
    i = i + 1
    repeat_count = tonumber(arg[i])
    assert(repeat_count and repeat_count > 0 and repeat_count == math.floor(repeat_count), "--repeat must be a positive integer")
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

for run = 1, repeat_count do
  local f = assert(io.open(input, "rb"))
  local eof = false
  local bytes = 0

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
    if repeat_count == 1 then
      io.write(chunk)
    else
      bytes = bytes + #chunk
    end
  end, opts)

  if repeat_count > 1 then
    assert(bytes > 0, "renderer produced no output")
  end
end
