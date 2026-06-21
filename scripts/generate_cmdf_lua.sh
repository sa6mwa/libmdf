#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-"$ROOT/build/cmdf.lua"}
mkdir -p "$(dirname "$OUT")"

font_b64() {
  perl -0777 -ne 'if (/\{(.*)\};/s) { $b = $1; while ($b =~ /\b(0x[0-9a-fA-F]+|[0-9]+)\b/g) { print chr(oct($1)) } }' "$1" | base64 -w0
}

REGULAR=$(font_b64 "$ROOT/src/html_embedded/jetbrains_regular.h")
ITALIC=$(font_b64 "$ROOT/src/html_embedded/jetbrains_italic.h")

cat > "$OUT" <<EOF
#!/usr/bin/env lua
local mdf = require("libmdf")

local regular_b64 = "$REGULAR"
local italic_b64 = "$ITALIC"

local function b64decode(s)
  local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local map = {}
  for i = 1, #alphabet do map[alphabet:sub(i, i)] = i - 1 end
  local out = {}
  local bits, bit_count = 0, 0
  for i = 1, #s do
    local c = s:sub(i, i)
    if c == "=" then break end
    local v = map[c]
    if v then
      bits = bits * 64 + v
      bit_count = bit_count + 6
      while bit_count >= 8 do
        bit_count = bit_count - 8
        local byte = math.floor(bits / (2 ^ bit_count))
        out[#out + 1] = string.char(byte % 256)
        bits = bits % (2 ^ bit_count)
      end
    end
  end
  return table.concat(out)
end

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
  io.stderr:write("  -b, --boring               Boring ANSI output\\n")
  io.stderr:write("  -o, --output PATH          Output file\\n")
  io.stderr:write("  -t, --theme NAME           Theme name\\n")
  io.stderr:write("  -w, --width WIDTH          ANSI output width\\n")
  io.stderr:write("      --margin-left N        ANSI left margin in spaces\\n")
  io.stderr:write("      --margin-right N       ANSI right margin in spaces\\n")
  io.stderr:write("  -8, --osc8 MODE            OSC8 mode: auto|on|off\\n")
  io.stderr:write("      --list-themes          List available themes\\n")
  io.stderr:write("      --html-content-width N HTML content max width in ch\\n")
  io.stderr:write("      --table-buffer MODE    Table buffering mode: full|row\\n")
  io.stderr:write("      --table-wire MODE      Table wire mode: line|ascii|space\\n")
  io.stderr:write("      --trace-writes PATH    Write ANSI renderer-emission NDJSON trace to PATH or - for stderr\\n")
  io.stderr:write("  -h, --help                 Show help\\n")
end

local opts = { osc8 = mdf.detect_osc8_support() }
local input_path, output_path, trace_path, trace_file
local i = 1
while i <= #arg do
  local a = arg[i]
  if a == "-h" or a == "--help" then
    usage()
    os.exit(0)
  elseif a == "-H" or a == "--html" then
    opts.html = true
  elseif a == "-b" or a == "--boring" then
    opts.boring = true
  elseif a == "-w" or a == "--width" then
    i = i + 1
    opts.width = tonumber(arg[i])
    if not opts.width or opts.width <= 0 then error("cmdf.lua: invalid width", 0) end
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
  elseif a == "-o" or a == "--output" then
    i = i + 1
    output_path = arg[i]
    if not output_path then error("cmdf.lua: missing output path", 0) end
  elseif a == "--html-content-width" then
    i = i + 1
    opts.html_content_width_ch = tonumber(arg[i])
    if not opts.html_content_width_ch or opts.html_content_width_ch <= 0 then error("cmdf.lua: invalid html content width", 0) end
  elseif a == "--table-buffer" then
    i = i + 1
    opts.table_buffer_mode = arg[i]
    if not opts.table_buffer_mode then error("cmdf.lua: missing table buffer mode", 0) end
  elseif a == "--table-wire" then
    i = i + 1
    opts.table_wire_mode = arg[i]
    if not opts.table_wire_mode then error("cmdf.lua: missing table wire mode", 0) end
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

if opts.html and opts.table_wire_mode == "ascii" then
  error("cmdf.lua: --table-wire ascii is not supported with --html; use line or space", 0)
end
if trace_path and opts.html then
  error("cmdf.lua: --trace-writes is only supported for ANSI output", 0)
end

if opts.html then
  opts.html_font = {
    family = "JetBrains Mono",
    regular_format = "woff2",
    regular_data = b64decode(regular_b64),
    italic_format = "woff2",
    italic_data = b64decode(italic_b64),
  }
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

local function read_chunk(cap)
  local n = cap
  if n == nil or n <= 0 or n > 4096 then n = 4096 end
  local chunk = input_file:read(n)
  if chunk == "" then return nil end
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
