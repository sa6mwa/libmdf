local mdf = require("libmdf")

local function shell_quote(s)
  return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

local function run_capture(cmd, input)
  local path = os.tmpname()
  local f = assert(io.open(path, "wb"))
  f:write(input)
  f:close()
  local p = assert(io.popen(cmd .. " " .. shell_quote(path), "r"))
  local out = p:read("*a")
  local ok = p:close()
  os.remove(path)
  assert(ok, cmd)
  return out
end

local function run_stdin_capture(cmd, input)
  local p = assert(io.popen("printf %s " .. shell_quote(input) .. " | " .. cmd, "r"))
  local out = p:read("*a")
  local ok = p:close()
  assert(ok, cmd)
  return out
end

local function run_command_capture(cmd)
  local p = assert(io.popen(cmd, "r"))
  local out = p:read("*a")
  local ok = p:close()
  assert(ok, cmd)
  return out
end

local function run_capture_with_trace(cmd, input)
  local input_path = os.tmpname()
  local trace_path = os.tmpname()
  local f = assert(io.open(input_path, "wb"))
  f:write(input)
  f:close()
  os.remove(trace_path)
  local p = assert(io.popen(cmd .. " --trace-writes " .. shell_quote(trace_path) .. " " .. shell_quote(input_path), "r"))
  local out = p:read("*a")
  local ok = p:close()
  local tf = assert(io.open(trace_path, "rb"))
  local trace = tf:read("*a")
  tf:close()
  os.remove(input_path)
  os.remove(trace_path)
  assert(ok, cmd)
  return out, trace
end

local function run_expect_fail(cmd, input)
  local path = os.tmpname()
  local f = assert(io.open(path, "wb"))
  f:write(input)
  f:close()
  local p = assert(io.popen(cmd .. " " .. shell_quote(path) .. " 2>&1", "r"))
  local out = p:read("*a")
  local ok = p:close()
  os.remove(path)
  assert(not ok, cmd)
  return out
end

local function assert_equal(a, b, label)
  if a ~= b then
    error(label .. " mismatch\nwant:\n" .. b .. "\ngot:\n" .. a, 2)
  end
end

local function collect_stream(markdown, opts, chunk_size)
  local pos = 1
  local out = {}
  mdf.render_stream(function(cap)
    local n = math.min(chunk_size or cap, cap, #markdown - pos + 1)
    if n <= 0 then return nil end
    local chunk = markdown:sub(pos, pos + n - 1)
    pos = pos + n
    return chunk
  end, function(chunk)
    out[#out + 1] = chunk
  end, opts)
  return table.concat(out)
end

local cmdf = assert(os.getenv("LIBMDF_CMDF"), "LIBMDF_CMDF")
local cmdf_lua = assert(os.getenv("LIBMDF_CMDF_LUA"), "LIBMDF_CMDF_LUA")
local sample = "# Lua\n\nbody with **strong** and [link](https://example.com)\n"
local parity_opts = { boring = true, osc8 = mdf.detect_osc8_support() }

do
  local f = assert(io.open(cmdf_lua, "rb"))
  local generated = f:read("*a")
  f:close()
  assert(generated:match("mdf%.render_stream%("), "cmdf.lua uses streaming render API")
  assert(not generated:match('read%("%*a"%)'), "cmdf.lua must not materialize full input")
  assert(not generated:match("pending_chunks"), "cmdf.lua must not replay prescanned input")
  assert(not generated:match("detect_html_title"), "cmdf.lua must not own HTML title detection")
  assert(not generated:match('read%(1%)'), "cmdf.lua must not perform byte-level title prescan reads")
  assert(not generated:match("mdf%.render%("), "cmdf.lua must not materialize full output")
  assert(not generated:match("os%.clock%("), "cmdf.lua simulate delay must not busy-wait")
  assert(generated:find("sleep %.9f", 1, true), "cmdf.lua simulate delay uses wall-clock sleep")
  assert(generated:find("local simulated_reads = 0", 1, true),
         "cmdf.lua tracks simulated reads")
  assert(generated:find("if simulated_reads > 0 then", 1, true),
         "cmdf.lua sleeps only between simulated reads")
end

assert(type(mdf.version) == "string" and mdf.version:match("^%d+%.%d+%.%d+$"), "lua facade exposes version")
assert(type(mdf.version_major) == "number", "lua facade exposes version_major")
assert(type(mdf.version_minor) == "number", "lua facade exposes version_minor")
assert(type(mdf.version_patch) == "number", "lua facade exposes version_patch")
assert(mdf.status.OK == 0, "lua facade exposes MDF_OK")
assert(mdf.status_string(mdf.status.INVALID) == "invalid argument", "lua facade exposes status_string")
assert(type(mdf.token.CHART_BLOCK) == "number", "lua facade exposes chart block token")
assert(#mdf.theme_names() > 0, "lua facade exposes theme_names")
assert(mdf.theme_exists("default"), "lua facade exposes theme_exists")
assert(type(mdf.terminal_width(-1, 77)) == "number", "lua facade exposes terminal_width")

local out = mdf.render("# Lua\n\nbody\n", { boring = true })
assert(out:match("Lua"), out)
assert(out:match("body"), out)

local lua_html = mdf.render("# Lua HTML\n\nbody\n", {
  format = "html",
  html_title = "Lua HTML",
})
assert(lua_html:match("<title>Lua HTML</title>"), "lua html render applies title")
assert(lua_html:match('<main class="mdf%-document">'), "lua html render emits document shell")

local lua_html_auto_title = mdf.render("# Lua Auto HTML\n\nbody\n", { format = "html" })
assert(lua_html_auto_title:match("<title>Lua Auto HTML</title>"),
       "lua html render auto-title is owned by libmdf")

local stream_html = collect_stream("# Stream HTML\n\nbody\n", {
  html = true,
  html_title = "Stream HTML",
}, 1)
assert(stream_html:match("<title>Stream HTML</title>"), "lua streaming html applies title")
assert(stream_html:match('<main class="mdf%-document">'), "lua streaming html emits document shell")

local stream_html_auto_title = collect_stream("# Stream Auto HTML\n\nbody\n", { html = true }, 1)
assert(stream_html_auto_title:match("<title>Stream Auto HTML</title>"),
       "lua streaming html auto-title is owned by libmdf")

local lua_ansi = mdf.render(sample, parity_opts)
local cmdf_ansi = run_capture(shell_quote(cmdf) .. " -b", sample)
assert_equal(lua_ansi, cmdf_ansi, "lua facade ansi parity")

local cmdf_lua_html_stdin = run_stdin_capture(shell_quote(cmdf_lua) .. " --html", "# Pipe Lua\n\nbody\n")
assert(cmdf_lua_html_stdin:match("<title>Pipe Lua</title>"), "cmdf.lua stdin html auto-title uses first ATX heading")

local cmdf_lua_html_tab_blank = run_stdin_capture(shell_quote(cmdf_lua) .. " --html", "\t\n# Tab Pipe Lua\n\nbody\n")
assert(cmdf_lua_html_tab_blank:match("<title>mdf</title>"),
       "cmdf.lua stdin html auto-title stops when leading tab rules out a heading")

local stream_ansi = collect_stream(sample, parity_opts, 1)
assert_equal(stream_ansi, cmdf_ansi, "lua streaming ansi parity")

local margin_sample = "alpha\n\nbeta gamma\n"
local margin_expected = "  alpha\n\n  beta\n  gamma\n"
local lua_margin = mdf.render(margin_sample, { boring = true, width = 10, margin_left = 2, margin_right = 1 })
assert_equal(lua_margin, margin_expected, "lua ansi margins")

local trace = {}
local traced = mdf.render(sample, {
  boring = true,
  osc8 = mdf.detect_osc8_support(),
  write_trace = function(format, chunk)
    trace[#trace + 1] = format .. ":" .. chunk
  end,
})
assert_equal(traced, cmdf_ansi, "lua traced render parity")
assert(#trace > 3, "lua write trace captures decision emissions")

local ok, err = pcall(function()
  mdf.render(sample, {
    boring = true,
    write_trace = function()
      return false
    end,
  })
end)
assert(not ok, "lua render surfaces write_trace callback failure")
assert(tostring(err):match("mdf_render: io error: sink write failed"), tostring(err))

ok, err = pcall(function()
  mdf.render_stream(function()
    return false
  end, function() end, parity_opts)
end)
assert(not ok, "lua render_stream rejects non-string reader chunks")
assert(tostring(err):match("mdf_render_stream: io error"), tostring(err))

local handle = mdf.new(parity_opts)
assert_equal(handle:render(sample), cmdf_ansi, "lua handle render parity")

ok, err = pcall(function()
  handle:render_stream(function()
    return false
  end, function() end)
end)
assert(not ok, "lua handle render_stream rejects non-string reader chunks")
assert(tostring(err):match("mdf_render_stream: io error"), tostring(err))

handle:close()

local html_handle = mdf.new({ format = "html" })
html_handle:set_html_title("Handle HTML")
local handle_html = html_handle:render("# Ignored\n\nbody\n")
html_handle:close()
assert(handle_html:match("<title>Handle HTML</title>"), "lua html handle set_html_title applies title")

local token_out = {}
local token_handle = mdf.new({ boring = true })
token_handle:write_token({ type = mdf.token.TEXT, text = "Hello" }, function(chunk) token_out[#token_out + 1] = chunk end)
token_handle:write_token({ type = "space", text = " " }, function(chunk) token_out[#token_out + 1] = chunk end)
token_handle:write_token({ type = "text", text = "Lua" }, function(chunk) token_out[#token_out + 1] = chunk end)
token_handle:write_token({ type = "document_end" }, function(chunk) token_out[#token_out + 1] = chunk end)
token_handle:finish(function(chunk) token_out[#token_out + 1] = chunk end)
token_handle:close()
assert(table.concat(token_out):match("Hello Lua"), "lua manual token API renders text")

local cmdf_lua_ansi = run_capture(shell_quote(cmdf_lua) .. " -b", sample)
assert_equal(cmdf_lua_ansi, cmdf_ansi, "cmdf.lua ansi parity")

local cmdf_lua_margin = run_capture(shell_quote(cmdf_lua) .. " -b -w 10 --margin-left 2 --margin-right 1", margin_sample)
assert_equal(cmdf_lua_margin, margin_expected, "cmdf.lua ansi margins")

local cmdf_version = run_command_capture(shell_quote(cmdf) .. " --version")
local cmdf_lua_version = run_command_capture(shell_quote(cmdf_lua) .. " --version")
assert_equal(cmdf_lua_version, cmdf_version, "cmdf.lua version parity")

local invalid_width = run_expect_fail(shell_quote(cmdf_lua) .. " -w 0", sample)
assert(invalid_width:match("invalid width"), invalid_width)

local invalid_fractional_width = run_expect_fail(shell_quote(cmdf_lua) .. " -w 10.5", sample)
assert(invalid_fractional_width:match("invalid width"), invalid_fractional_width)

local invalid_large_width = run_expect_fail(shell_quote(cmdf_lua) .. " --html -w 10001", sample)
assert(invalid_large_width:match("invalid width"), invalid_large_width)

local invalid_html_width = run_expect_fail(shell_quote(cmdf_lua) .. " --html --html-content-width 0", sample)
assert(invalid_html_width:match("invalid html content width"), invalid_html_width)

local deck_sample = "# Front\n\n---\n\n# Second\n\nbody\n"
local lua_deck = mdf.render(deck_sample, {
  format = "deck",
  html_title = "Lua Deck",
  deck_transition = "cross",
  slide_numbers = true,
  deck_center_front_text = true,
})
assert(lua_deck:match("<title>Lua Deck</title>"), "lua deck render applies title")
assert(lua_deck:match('class="mdf%-deck" data%-transition="cross"'), "lua deck render applies transition")
assert(lua_deck:match("mdf%-slide%-number"), "lua deck render emits slide number placeholders")
assert(lua_deck:match("mdf%-center%-front%-text"), "lua deck render emits first-slide centering class")

local lua_deck_auto_title = mdf.render(deck_sample, { format = "deck" })
assert(lua_deck_auto_title:match("<title>Front</title>"),
       "lua deck render auto-title is owned by libmdf")

local stream_deck = collect_stream(deck_sample, {
  deck = true,
  html_title = "Stream Deck",
  deck_transition = "hard",
}, 1)
assert(stream_deck:match("<title>Stream Deck</title>"), "lua streaming deck applies title")
assert(stream_deck:match('class="mdf%-deck" data%-transition="hard"'), "lua streaming deck applies transition")

local stream_deck_auto_title = collect_stream(deck_sample, { deck = true }, 1)
assert(stream_deck_auto_title:match("<title>Front</title>"),
       "lua streaming deck auto-title is owned by libmdf")

local deck_handle = mdf.new({
  format = "html_deck",
  slide_numbers = true,
})
deck_handle:set_html_title("Handle Deck")
local handle_deck = deck_handle:render(deck_sample)
deck_handle:close()
assert(handle_deck:match("<title>Handle Deck</title>"), "lua deck handle applies title")
assert(handle_deck:match("mdf%-slide%-number"), "lua deck handle emits slide number placeholders")

ok, err = pcall(function()
  mdf.render(deck_sample, { format = "deck", deck_transition = "spin" })
end)
assert(not ok, "lua deck rejects invalid transition")
assert(tostring(err):match("unknown deck transition: spin"), tostring(err))

local cmdf_lua_traced, cmdf_lua_trace = run_capture_with_trace(shell_quote(cmdf_lua) .. " -b", sample)
assert_equal(cmdf_lua_traced, cmdf_ansi, "cmdf.lua traced ansi parity")
local trace_lines = 0
for _ in cmdf_lua_trace:gmatch("[^\n]+") do trace_lines = trace_lines + 1 end
assert(trace_lines > 3, "cmdf.lua traced render emits decision chunks")

local cmdf_html = run_capture(shell_quote(cmdf) .. " --html", sample)
local cmdf_lua_html = run_capture(shell_quote(cmdf_lua) .. " --html", sample)
assert(cmdf_lua_html:match("JetBrains Mono"), "cmdf.lua html embeds JetBrains Mono")
assert_equal(cmdf_lua_html, cmdf_html, "cmdf.lua html parity")

local cmdf_html_width = run_capture(shell_quote(cmdf) .. " --html -w 42", sample)
local cmdf_lua_html_width = run_capture(shell_quote(cmdf_lua) .. " --html -w 42", sample)
assert_equal(cmdf_lua_html_width, cmdf_html_width, "cmdf.lua html width parity")

local cmdf_html_after_deck = run_capture(shell_quote(cmdf) .. " --deck --html -T 'Lua Mixed'", deck_sample)
local cmdf_lua_html_after_deck = run_capture(shell_quote(cmdf_lua) .. " --deck --html -T 'Lua Mixed'", deck_sample)
assert_equal(cmdf_lua_html_after_deck, cmdf_html_after_deck, "cmdf.lua deck then html format flag parity")

local cmdf_lua_simulated = run_capture(shell_quote(cmdf_lua) .. " -b --simulate-chunk 1", sample)
assert_equal(cmdf_lua_simulated, cmdf_ansi, "cmdf.lua simulate chunk ansi parity")

local cmdf_lua_simulated_short = run_capture(shell_quote(cmdf_lua) .. " -b -S 1", sample)
assert_equal(cmdf_lua_simulated_short, cmdf_ansi, "cmdf.lua -S simulate chunk ansi parity")

local cmdf_lua_simulated_delay = run_capture(shell_quote(cmdf_lua) .. " -b --simulate-delay 1ms", sample)
assert_equal(cmdf_lua_simulated_delay, cmdf_ansi, "cmdf.lua simulate delay ansi parity")

for _, delay in ipairs({ ".001s", "1e-3s", "+1e-3s" }) do
  local cmdf_delay = run_capture(shell_quote(cmdf) .. " -b --simulate-delay " .. delay, sample)
  local cmdf_lua_delay = run_capture(shell_quote(cmdf_lua) .. " -b --simulate-delay " .. delay, sample)
  assert_equal(cmdf_lua_delay, cmdf_delay, "cmdf.lua simulate delay duration syntax parity: " .. delay)
end

local cmdf_deck = run_capture(shell_quote(cmdf) .. " --deck --slide-numbers -x cross -T 'Lua Deck'", deck_sample)
local cmdf_lua_deck = run_capture(shell_quote(cmdf_lua) .. " --deck --slide-numbers -x cross -T 'Lua Deck'", deck_sample)
assert(cmdf_lua_deck:match("JetBrains Mono"), "cmdf.lua deck embeds JetBrains Mono")
assert_equal(cmdf_lua_deck, cmdf_deck, "cmdf.lua deck parity")

local cmdf_deck_after_html = run_capture(shell_quote(cmdf) .. " --html --deck -T 'Lua Mixed'", deck_sample)
local cmdf_lua_deck_after_html = run_capture(shell_quote(cmdf_lua) .. " --html --deck -T 'Lua Mixed'", deck_sample)
assert_equal(cmdf_lua_deck_after_html, cmdf_deck_after_html, "cmdf.lua html then deck format flag parity")

local cmdf_deck_width = run_capture(shell_quote(cmdf) .. " --deck -w 42 -T 'Lua Deck'", deck_sample)
local cmdf_lua_deck_width = run_capture(shell_quote(cmdf_lua) .. " --deck -w 42 -T 'Lua Deck'", deck_sample)
assert_equal(cmdf_lua_deck_width, cmdf_deck_width, "cmdf.lua deck width parity")

local invalid_deck_transition = run_expect_fail(shell_quote(cmdf_lua) .. " --deck -x spin", deck_sample)
assert(invalid_deck_transition:match("invalid deck transition"), invalid_deck_transition)

local invalid_simulate_chunk = run_expect_fail(shell_quote(cmdf_lua) .. " --simulate-chunk 0", sample)
assert(invalid_simulate_chunk:match("invalid simulate chunk"), invalid_simulate_chunk)

local invalid_simulate_delay = run_expect_fail(shell_quote(cmdf_lua) .. " --simulate-delay 0.5", sample)
assert(invalid_simulate_delay:match("invalid simulate delay"), invalid_simulate_delay)

local deck_option_without_deck = run_expect_fail(shell_quote(cmdf_lua) .. " --slide-numbers", deck_sample)
assert(deck_option_without_deck:find("deck options require --deck", 1, true), deck_option_without_deck)
