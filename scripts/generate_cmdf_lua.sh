#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-"$ROOT/build/cmdf.lua"}
mkdir -p "$(dirname "$OUT")"

cat > "$OUT" <<EOF
#!/usr/bin/env lua
local mdf = require("libmdf")

local function b64encode(s)
  local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local out = {}
  for i = 1, #s, 3 do
    local a = s:byte(i) or 0
    local b = s:byte(i + 1) or 0
    local c = s:byte(i + 2) or 0
    local n = a * 65536 + b * 256 + c
    out[#out + 1] = alphabet:sub(math.floor(n / 262144) % 64 + 1, math.floor(n / 262144) % 64 + 1)
    out[#out + 1] = alphabet:sub(math.floor(n / 4096) % 64 + 1, math.floor(n / 4096) % 64 + 1)
    if i + 1 <= #s then
      out[#out + 1] = alphabet:sub(math.floor(n / 64) % 64 + 1, math.floor(n / 64) % 64 + 1)
    else
      out[#out + 1] = "="
    end
    if i + 2 <= #s then
      out[#out + 1] = alphabet:sub(n % 64 + 1, n % 64 + 1)
    else
      out[#out + 1] = "="
    end
  end
  return table.concat(out)
end

local function usage()
  io.stderr:write("Usage: cmdf.lua [flags] [input]\\n")
  io.stderr:write("  -H, --html                 Render HTML\\n")
  io.stderr:write("      --deck                 Render HTML slide deck\\n")
  io.stderr:write("  -b, --boring               Boring ANSI output\\n")
  io.stderr:write("  -o, --output PATH          Output file\\n")
  io.stderr:write("  -t, --theme NAME           Theme name\\n")
  io.stderr:write("  -T, --title TITLE          HTML document title\\n")
  io.stderr:write("  -w, --width WIDTH          ANSI output width\\n")
  io.stderr:write("      --margin-left N        ANSI left margin in spaces\\n")
  io.stderr:write("      --margin-right N       ANSI right margin in spaces\\n")
  io.stderr:write("  -8, --osc8 MODE            OSC8 mode: auto|on|off\\n")
  io.stderr:write("      --list-themes          List available themes\\n")
  io.stderr:write("      --html-content-width N HTML content max width in ch\\n")
  io.stderr:write("      --html-disable-embedded-font Use external JetBrains Mono web fonts\\n")
  io.stderr:write("      --html-font-uri URI    External JetBrains Mono font URI base\\n")
  io.stderr:write("      --html-font-regular-uri URI Override external regular font URI\\n")
  io.stderr:write("      --html-font-italic-uri URI External italic JetBrains Mono font URI\\n")
  io.stderr:write("      --html-dump-font       Dump built-in fonts to local URI or configured paths\\n")
  io.stderr:write("      --html-dump-font-force Replace existing font files while dumping\\n")
  io.stderr:write("      --html-dump-font-path DIR Dump paired fonts in DIR\\n")
  io.stderr:write("      --html-dump-font-regular-path PATH Override regular font destination\\n")
  io.stderr:write("      --html-dump-font-italic-path PATH Override italic font destination\\n")
  io.stderr:write("  -x, --transition MODE      Deck transition: fade|cross|hard\\n")
  io.stderr:write("      --slide-numbers        Show deck slide numbers after the first slide\\n")
  io.stderr:write("      --deck-center-front-text Center-align first-slide paragraph text\\n")
  io.stderr:write("      --table-buffer MODE    Table buffering mode: full|row\\n")
  io.stderr:write("      --table-wire MODE      Table wire mode: line|ascii|space\\n")
  io.stderr:write("      --simulate             Simulate input streaming with default chunk\\n")
  io.stderr:write("      --simulate-chunk N     Simulate input streaming with max N bytes per read\\n")
  io.stderr:write("      --simulate-delay D     Delay duration between simulated reads; implies --simulate\\n")
  io.stderr:write("      --trace-writes PATH    Write ANSI renderer-emission NDJSON trace to PATH or - for stderr\\n")
  io.stderr:write("  -h, --help                 Show help\\n")
  io.stderr:write("  -V, --version              Show version\\n")
end

local opts = { osc8 = mdf.detect_osc8_support() }
local input_path, output_path, trace_path, trace_file
local format_explicit = false
local width_flag = nil
local html_content_width_seen = false
local simulate_enabled = false
local simulate_chunk = nil
local simulate_delay_seconds = 0
local i = 1

local function parse_duration_seconds(s)
  if not s or s == "" then return nil end
  local pos = 1
  local total = 0
  while pos <= #s do
    local num, next_pos = s:match("^([+-]?%d+%.?%d*[eE][+-]?%d+)()", pos)
    if not num then num, next_pos = s:match("^([+-]?%.%d+[eE][+-]?%d+)()", pos) end
    if not num then num, next_pos = s:match("^([+-]?%d+%.?%d*)()", pos) end
    if not num then num, next_pos = s:match("^([+-]?%.%d+)()", pos) end
    if not num then return nil end
    local value = tonumber(num)
    if not value or value < 0 then return nil end
    local unit
    if s:sub(next_pos, next_pos + 1) == "ns" then
      unit = "ns"
      next_pos = next_pos + 2
    elseif s:sub(next_pos, next_pos + 1) == "us" then
      unit = "us"
      next_pos = next_pos + 2
    elseif s:sub(next_pos, next_pos + 2) == "µs" then
      unit = "µs"
      next_pos = next_pos + 3
    elseif s:sub(next_pos, next_pos + 1) == "ms" then
      unit = "ms"
      next_pos = next_pos + 2
    else
      unit = s:sub(next_pos, next_pos)
      if unit == "s" or unit == "m" or unit == "h" then
        next_pos = next_pos + 1
      else
        return nil
      end
    end
    local scale = ({ ns = 0.000000001, us = 0.000001, ["µs"] = 0.000001, ms = 0.001, s = 1, m = 60, h = 3600 })[unit]
    total = total + value * scale
    pos = next_pos
  end
  if total <= 0 then return nil end
  return total
end

local function sleep_seconds(seconds)
  if not seconds or seconds <= 0 then return end
  os.execute(string.format("sleep %.9f", seconds))
end

while i <= #arg do
  local a = arg[i]
  if a == "-h" or a == "--help" then
    usage()
    os.exit(0)
  elseif a == "-V" or a == "--version" then
    print(mdf.version)
    os.exit(0)
  elseif a == "-H" or a == "--html" then
    opts.html = true
    format_explicit = true
  elseif a == "--deck" then
    -- Match native cmdf: deck is the more specific HTML mode once requested.
    opts.deck = true
    opts.html = nil
    format_explicit = true
  elseif a == "-b" or a == "--boring" then
    opts.boring = true
  elseif a == "-w" or a == "--width" then
    i = i + 1
    opts.width = tonumber(arg[i])
    if not opts.width or opts.width <= 0 or opts.width > 10000 or opts.width ~= math.floor(opts.width) then error("cmdf.lua: invalid width", 0) end
    width_flag = opts.width
  elseif a == "--margin-left" then
    i = i + 1
    opts.margin_left = tonumber(arg[i])
    if not opts.margin_left or opts.margin_left < 0 then error("cmdf.lua: invalid left margin", 0) end
  elseif a == "--margin-right" then
    i = i + 1
    opts.margin_right = tonumber(arg[i])
    if not opts.margin_right or opts.margin_right < 0 then error("cmdf.lua: invalid right margin", 0) end
  elseif a == "-t" or a == "--theme" then
    i = i + 1
    opts.theme = arg[i]
    if not opts.theme then error("cmdf.lua: missing theme", 0) end
  elseif a == "-T" or a == "--title" then
    i = i + 1
    opts.html_title = arg[i]
    if opts.html_title == nil then error("cmdf.lua: missing title", 0) end
  elseif a == "-o" or a == "--output" then
    i = i + 1
    output_path = arg[i]
    if not output_path then error("cmdf.lua: missing output path", 0) end
  elseif a == "--html-content-width" then
    i = i + 1
    opts.html_content_width_ch = tonumber(arg[i])
    if not opts.html_content_width_ch or opts.html_content_width_ch <= 0 then error("cmdf.lua: invalid html content width", 0) end
    html_content_width_seen = true
  elseif a == "--html-disable-embedded-font" then
    opts.disable_embedded_font = true
  elseif a == "--html-font-uri" then
    i = i + 1
    opts.font_uri = arg[i]
    if not opts.font_uri then error("cmdf.lua: missing HTML font URI", 0) end
  elseif a == "--html-font-regular-uri" then
    i = i + 1
    opts.font_regular_uri = arg[i]
    if not opts.font_regular_uri then error("cmdf.lua: missing HTML regular font URI", 0) end
  elseif a == "--html-font-italic-uri" then
    i = i + 1
    opts.font_italic_uri = arg[i]
    if not opts.font_italic_uri then error("cmdf.lua: missing HTML italic font URI", 0) end
  elseif a == "--html-dump-font" then
    opts.dump_font = true
  elseif a == "--html-dump-font-force" then
    opts.dump_font = true
    opts.dump_font_force = true
  elseif a == "--html-dump-font-path" then
    i = i + 1
    opts.font_path = arg[i]
    if not opts.font_path then error("cmdf.lua: missing HTML dump font path", 0) end
    opts.dump_font = true
  elseif a == "--html-dump-font-regular-path" then
    i = i + 1
    opts.font_regular_path = arg[i]
    if not opts.font_regular_path then error("cmdf.lua: missing HTML regular dump font path", 0) end
    opts.dump_font = true
  elseif a == "--html-dump-font-italic-path" then
    i = i + 1
    opts.font_italic_path = arg[i]
    if not opts.font_italic_path then error("cmdf.lua: missing HTML italic dump font path", 0) end
    opts.dump_font = true
  elseif a == "-x" or a == "--transition" then
    i = i + 1
    opts.deck_transition = arg[i]
    if opts.deck_transition ~= "fade" and opts.deck_transition ~= "cross" and opts.deck_transition ~= "hard" then
      error("cmdf.lua: invalid deck transition", 0)
    end
    opts.deck_option_seen = true
  elseif a == "--slide-numbers" then
    opts.slide_numbers = true
    opts.deck_option_seen = true
  elseif a == "--deck-center-front-text" then
    opts.deck_center_front_text = true
    opts.deck_option_seen = true
  elseif a == "--table-buffer" then
    i = i + 1
    opts.table_buffer_mode = arg[i]
    if not opts.table_buffer_mode then error("cmdf.lua: missing table buffer mode", 0) end
  elseif a == "--table-wire" then
    i = i + 1
    opts.table_wire_mode = arg[i]
    if not opts.table_wire_mode then error("cmdf.lua: missing table wire mode", 0) end
  elseif a == "--simulate" then
    simulate_enabled = true
    simulate_chunk = 3
    simulate_delay_seconds = 0.02
  elseif a == "-S" or a == "--simulate-chunk" then
    i = i + 1
    simulate_chunk = tonumber(arg[i])
    if not simulate_chunk or simulate_chunk <= 0 or simulate_chunk > 10000 or simulate_chunk ~= math.floor(simulate_chunk) then
      error("cmdf.lua: invalid simulate chunk", 0)
    end
  elseif a == "--simulate-delay" then
    i = i + 1
    simulate_delay_seconds = parse_duration_seconds(arg[i])
    if not simulate_delay_seconds then error("cmdf.lua: invalid simulate delay", 0) end
    simulate_enabled = true
  elseif a == "--trace-writes" then
    i = i + 1
    trace_path = arg[i]
    if not trace_path then error("cmdf.lua: missing trace path", 0) end
  elseif a == "-8" or a == "--osc8" then
    i = i + 1
    local mode = arg[i]
    if mode == "on" then opts.osc8 = true
    elseif mode == "off" then opts.osc8 = false
    elseif mode == "auto" then opts.osc8 = mdf.detect_osc8_support()
    else error("cmdf.lua: invalid OSC8 mode", 0) end
  elseif a == "--list-themes" then
    for _, name in ipairs(mdf.theme_names()) do print(name) end
    os.exit(0)
  elseif a:sub(1, 1) == "-" then
    error("cmdf.lua: unknown flag: " .. a, 0)
  elseif input_path == nil then
    input_path = a
  else
    error("cmdf.lua: expected at most one input", 0)
  end
  i = i + 1
end

local function has_html_extension(path)
  if not path then return false end
  return path:lower():match("%.html$") ~= nil or path:lower():match("%.htm$") ~= nil
end

if not format_explicit and has_html_extension(output_path) then
  opts.html = true
  io.stderr:write("cmdf.lua: warning: inferring --html from output path " .. output_path .. "\\n")
end

if opts.deck_option_seen and not opts.deck then
  error("cmdf.lua: deck options require --deck", 0)
end
opts.deck_option_seen = nil

local html_like = opts.html or opts.deck

if (opts.disable_embedded_font or opts.font_uri or opts.font_regular_uri or opts.font_italic_uri or opts.dump_font or opts.dump_font_force or
    opts.font_path or opts.font_regular_path or opts.font_italic_path) and not html_like then
  error("cmdf.lua: HTML font options require HTML or deck output", 0)
end

if (simulate_enabled or simulate_delay_seconds > 0) and not simulate_chunk then
  simulate_chunk = 3
end

if html_like and width_flag and not html_content_width_seen then
  opts.html_content_width_ch = width_flag
end

if html_like and opts.table_wire_mode == "ascii" then
  error("cmdf.lua: --table-wire ascii is not supported with HTML output; use line or space", 0)
end
if trace_path and html_like then
  error("cmdf.lua: --trace-writes is only supported for ANSI output", 0)
end

if trace_path then
  if trace_path == "-" then
    trace_file = io.stderr
  else
    trace_file = assert(io.open(trace_path, "wb"))
  end
  local seq = 0
  opts.write_trace = function(format, chunk)
    seq = seq + 1
    trace_file:write(string.format('{"seq":%d,"format":"%s","op":"emit","bytes":%d,"data_b64":"%s"}\\n',
      seq, format, #chunk, b64encode(chunk)))
    trace_file:flush()
  end
end

local input_file
if input_path then
  input_file = assert(io.open(input_path, "rb"))
else
  input_file = io.stdin
end

local output_file
if output_path then
  output_file = assert(io.open(output_path, "wb"))
else
  output_file = io.stdout
end

local simulated_reads = 0

local function maybe_sleep_between_simulated_reads()
  if simulated_reads > 0 then
    sleep_seconds(simulate_delay_seconds)
  end
end

local function read_chunk(cap)
  local n = cap
  if n == nil or n <= 0 or n > 4096 then n = 4096 end
  if simulate_chunk and n > simulate_chunk then n = simulate_chunk end
  local chunk = input_file:read(n)
  if chunk == "" then return nil end
  if chunk ~= nil then
    maybe_sleep_between_simulated_reads()
    simulated_reads = simulated_reads + 1
  end
  return chunk
end

local function write_chunk(chunk)
  output_file:write(chunk)
  output_file:flush()
end

mdf.render_stream(read_chunk, write_chunk, opts)

if input_file ~= io.stdin then
  input_file:close()
end
if output_file ~= io.stdout then
  output_file:close()
end
if trace_file and trace_file ~= io.stderr then
  trace_file:close()
end
EOF
chmod +x "$OUT"
