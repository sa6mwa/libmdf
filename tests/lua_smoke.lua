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
  assert(not generated:match("mdf%.render%("), "cmdf.lua must not materialize full output")
end

local out = mdf.render("# Lua\n\nbody\n", { boring = true })
assert(out:match("Lua"), out)
assert(out:match("body"), out)

local lua_ansi = mdf.render(sample, parity_opts)
local cmdf_ansi = run_capture(shell_quote(cmdf) .. " -b", sample)
assert_equal(lua_ansi, cmdf_ansi, "lua facade ansi parity")

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

local invalid_width = run_expect_fail(shell_quote(cmdf_lua) .. " -w 0", sample)
assert(invalid_width:match("invalid width"), invalid_width)

local invalid_html_width = run_expect_fail(shell_quote(cmdf_lua) .. " --html --html-content-width 0", sample)
assert(invalid_html_width:match("invalid html content width"), invalid_html_width)

local cmdf_lua_traced, cmdf_lua_trace = run_capture_with_trace(shell_quote(cmdf_lua) .. " -b", sample)
assert_equal(cmdf_lua_traced, cmdf_ansi, "cmdf.lua traced ansi parity")
local trace_lines = 0
for _ in cmdf_lua_trace:gmatch("[^\n]+") do trace_lines = trace_lines + 1 end
assert(trace_lines > 3, "cmdf.lua traced render emits decision chunks")

local cmdf_html = run_capture(shell_quote(cmdf) .. " --html", sample)
local cmdf_lua_html = run_capture(shell_quote(cmdf_lua) .. " --html", sample)
assert(cmdf_lua_html:match("JetBrains Mono"), "cmdf.lua html embeds JetBrains Mono")
assert_equal(cmdf_lua_html, cmdf_html, "cmdf.lua html parity")
