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

local function run_command_expect_fail(cmd)
  local p = assert(io.popen(cmd .. " 2>&1", "r"))
  local out = p:read("*a")
  local ok = p:close()
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
local parity_opts = { ansi_mode = "on", boring = true, osc8 = mdf.detect_osc8_support() }

do
  local f = assert(io.open(cmdf_lua, "rb"))
  local generated = f:read("*a")
  f:close()
  assert(generated:find("renderer:set_sink(write_chunk)", 1, true),
         "cmdf.lua binds one renderer sink before streaming")
  assert(generated:find("renderer:render_stream(read_chunk)", 1, true),
         "cmdf.lua uses the bound-sink receiver streaming API")
  assert(not generated:find("renderer:render_stream(read_chunk, write_chunk)", 1, true),
         "cmdf.lua does not pass a sink to every receiver render call")
  assert(not generated:match('read%("%*a"%)'), "cmdf.lua must not materialize full input")
  assert(not generated:match("pending_chunks"), "cmdf.lua must not replay prescanned input")
  assert(not generated:match("detect_html_title"), "cmdf.lua must not own HTML title detection")
  assert(not generated:match('read%(1%)'), "cmdf.lua must not perform byte-level title prescan reads")
  assert(not generated:match("mdf%.render%("), "cmdf.lua must not materialize full output")
  assert(generated:find("mdf.pager(input_path, opts)", 1, true),
         "cmdf.lua delegates --pager to the Lua library")
  assert(not generated:match("os%.clock%("), "cmdf.lua simulate delay must not busy-wait")
  assert(generated:find("sleep %.9f", 1, true), "cmdf.lua simulate delay uses wall-clock sleep")
  assert(generated:find("local simulated_reads = 0", 1, true),
         "cmdf.lua tracks simulated reads")
  assert(generated:find("if simulated_reads > 0 then", 1, true),
         "cmdf.lua sleeps only between simulated reads")
  assert(generated:find("input_path and (mdf.paths_alias(dump_regular_path, input_path)", 1, true),
         "cmdf.lua rechecks input aliases after creating dumped fonts")
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
assert(type(mdf.pager) == "function", "lua facade exposes pager")
assert(mdf.paths_alias("same-path", "same-path"), "lua facade exposes path alias checks")

do
  -- Every non-private native module function must be reachable through the facade.
  for name, value in pairs(require("libmdf.core")) do
    if type(value) == "function" and name:sub(1, 1) ~= "_" then
      assert(type(mdf[name]) == "function", "Lua facade missing native function: " .. name)
    end
  end
  assert_equal(mdf.path_relative_to("/docs/output", "/docs/fonts/face name.woff2"),
               "../fonts/face%20name.woff2", "relative paths escape URI bytes")
  assert_equal(mdf.path_relative_to("/docs/./output", "/docs/output/../output"),
               ".", "relative paths normalize dot components")
  assert_equal(mdf.path_relative_to("build/docs", "build/fonts/face.woff2"),
               "../fonts/face.woff2", "relative inputs resolve against the same working directory")
  assert_equal(mdf.file_descriptor(io.stdout), 1, "stdout descriptor exposed without closing")
  local f = assert(io.open(cmdf_lua, "rb"))
  assert(mdf.file_descriptor(f) >= 0, "open Lua file has a usable destination descriptor")
  assert(f:read(1), "descriptor helper leaves file open and readable")
  f:close()
  assert(not pcall(mdf.file_descriptor, f), "descriptor helper rejects closed files")
  assert(not pcall(mdf.file_descriptor, {}), "descriptor helper rejects non-files")
end

do
  local path = os.tmpname() .. ".txt"
  local f = assert(io.open(path, "wb"))
  f:write("# pager override\n")
  f:close()
  local pager_ok, pager_err = pcall(mdf.pager, path, { format = "text/markdown" })
  assert(not pager_ok and tostring(pager_err):match("mdf_pager: invalid argument"),
         "lua pager assumes UTF-8 for the bare HTTP markdown media type before rejecting a non-terminal session")
  pager_ok, pager_err = pcall(mdf.pager, path, { format = "Text/Markdown; charset=utf-8" })
  assert(not pager_ok and tostring(pager_err):match("mdf_pager: invalid argument"),
         "lua pager accepts parameterized HTTP markdown media types before rejecting a non-terminal session")
  pager_ok, pager_err = pcall(mdf.pager, path, { format = "t" })
  assert(not pager_ok and tostring(pager_err):match("pager format must be"),
         "lua pager rejects short invalid format strings before pager startup")
  os.remove(path)
end

local out = mdf.render("# Lua\n\nbody\n", { ansi_mode = "on", boring = true })
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
local cmdf_ansi = run_capture(shell_quote(cmdf) .. " --ansi on -b", sample)
assert_equal(lua_ansi, cmdf_ansi, "lua facade ansi parity")

local cmdf_lua_html_stdin = run_stdin_capture(shell_quote(cmdf_lua) .. " --ansi on --html", "# Pipe Lua\n\nbody\n")
assert(cmdf_lua_html_stdin:match("<title>Pipe Lua</title>"), "cmdf.lua stdin html auto-title uses first ATX heading")

local cmdf_lua_html_tab_blank = run_stdin_capture(shell_quote(cmdf_lua) .. " --ansi on --html", "\t\n# Tab Pipe Lua\n\nbody\n")
assert(cmdf_lua_html_tab_blank:match("<title>mdf</title>"),
       "cmdf.lua stdin html auto-title stops when leading tab rules out a heading")

local stream_ansi = collect_stream(sample, parity_opts, 1)
assert_equal(stream_ansi, cmdf_ansi, "lua streaming ansi parity")

local margin_sample = "alpha\n\nbeta gamma\n"
local margin_expected = "  alpha\n\n  beta\n  gamma\n"
local lua_margin = mdf.render(margin_sample, { ansi_mode = "on", boring = true, width = 10, margin_left = 2, margin_right = 1 })
assert_equal(lua_margin, margin_expected, "lua ansi margins")

local trace = {}
local traced = mdf.render(sample, {
  ansi_mode = "on", boring = true,
  osc8 = mdf.detect_osc8_support(),
  write_trace = function(format, chunk)
    trace[#trace + 1] = format .. ":" .. chunk
  end,
})
assert_equal(traced, cmdf_ansi, "lua traced render parity")
assert(#trace > 3, "lua write trace captures decision emissions")

local incremental_chunks = {}
local incremental = mdf.document_stream({ format = "ansi", ansi_mode = "on", boring = true }, function(chunk)
  incremental_chunks[#incremental_chunks + 1] = chunk
end)
assert(incremental:write("Hello incremental"), "lua document stream accepts a fragment")
assert(table.concat(incremental_chunks):match("Hello"),
       "lua document stream emits decidable text before document finish")
assert(incremental:flush(), "lua document stream flush is a soft boundary")
assert(incremental:finish_document(), "lua document stream finishes once")
local incremental_ok = pcall(function() incremental:write("late") end)
assert(not incremental_ok, "lua document stream rejects post-finish writes")
assert(incremental:begin_document(), "lua document stream starts a next document")
assert(incremental:write("Second document"), "lua document stream accepts next-document input")
assert(incremental:finish_document(), "lua document stream finishes next document")
incremental:destroy()

do
  local chunks = {}
  local stream = mdf.document_stream({ format = "ansi", ansi_mode = "on", boring = true, width = 80 }, function(chunk)
    chunks[#chunks + 1] = chunk
  end)
  assert(stream:write("alpha "), "lua document stream accepts input before runtime width change")
  assert(stream:set_width(5), "lua document stream changes width during an active document")
  assert(stream:write("beta\n"), "lua document stream continues after runtime width change")
  assert(stream:finish_document(), "lua document stream finishes after runtime width change")
  assert(table.concat(chunks):match("alpha\nbeta"),
         "lua document stream applies runtime width to later layout decisions")
  stream:close()
end

do
  local writes, traces = {}, {}
  local stream = mdf.document_stream({
    format = "ansi", ansi_mode = "on", boring = true, width = 80,
    write_trace = function(_, chunk) traces[#traces + 1] = chunk end,
  }, function(chunk) writes[#writes + 1] = chunk end)
  assert(stream:write("`abcdefghij`"), "lua geometry stream retains undecided inline code")
  assert(#writes == 0 and #traces == 0, "lua geometry has no early emission")
  assert(stream:set_geometry(12, 2, 2), "lua stream changes width and margins mid-document")
  assert(#writes == 0 and #traces == 0, "lua geometry setter does not emit")
  assert(stream:write(" tail\n"), "lua geometry stream continues without replay")
  assert(stream:finish_document(), "lua geometry stream finishes its original document")
  assert_equal(table.concat(writes),
               mdf.render("`abcdefghij` tail\n", {
                 format = "ansi", ansi_mode = "on", boring = true, width = 12, margin_left = 2, margin_right = 2,
               }), "lua pending code uses new geometry")
  assert(#writes == #traces, "lua geometry sink and trace counts match")
  for i = 1, #writes do
    assert_equal(writes[i], traces[i], "lua geometry sink and trace bytes match")
  end
  local invalid = pcall(function() stream:set_geometry(4, 2, 1) end)
  assert(not invalid, "lua geometry rejects insufficient content width")
  assert(not pcall(function() stream:set_geometry(12, -1, 0) end),
         "lua geometry rejects a negative left margin")
  if math.maxinteger > 2147483647 then
    assert(not pcall(function() stream:set_geometry(math.maxinteger, 0, 0) end),
           "lua geometry rejects integers outside the C range")
    assert(not pcall(function() mdf.new({ width = math.maxinteger }) end),
           "lua constructor rejects out-of-range geometry options")
  end
  stream:close()
end

do
  local writes, traces = {}, {}
  local stream = mdf.document_stream({
    format = "ansi", ansi_mode = "on", boring = true, width = 20,
    write_trace = function(_, chunk) traces[#traces + 1] = chunk end,
  }, function(chunk) writes[#writes + 1] = chunk end)
  assert(stream:write("alpha\n\nbeta "),
         "lua zero-margin stream emits text after earlier newlines")
  assert_equal(table.concat(writes), "alpha\n\nbeta",
               "lua zero-margin current line is already committed")
  local committed = #writes
  assert(stream:set_geometry(20, 2, 0),
         "lua stream changes from zero to positive left margin")
  assert(#writes == committed, "lua midline geometry change does not emit")
  assert(stream:write("tail\n\nnext\n"),
         "lua stream continues after midline margin change")
  assert(stream:finish_document(), "lua stream finishes after midline margin change")
  assert_equal(table.concat(writes), "alpha\n\nbeta tail\n\n  next\n",
               "lua new margin begins on a future line, not mid-line")
  assert(#writes == #traces, "lua midline geometry trace count matches sink writes")
  for i = 1, #writes do
    assert_equal(writes[i], traces[i], "lua midline geometry trace bytes match sink writes")
  end
  stream:close()
end

do
  local cases = {
    { "", "alpha\n", "  alpha\n", 0, 2 },
    { "alpha\n\n", "beta\n", "alpha\n\n  beta\n", 0, 2 },
    { "alpha ", "beta\n\nnext\n", "alpha beta\n\n  next\n", 0, 2 },
    { "alpha\n\nbeta ", "tail\n\nnext\n", "alpha\n\nbeta tail\n\n  next\n", 0, 2 },
    { "alpha\n\nbeta `code`", " tail\n\nnext\n",
      "alpha\n\nbeta code tail\n\n  next\n", 0, 2 },
    { "alpha\n\nbeta <https://example.com>", " tail\n\nnext\n",
      "alpha\n\nbeta https://example.com tail\n\n  next\n", 0, 2 },
    { "alpha\n\nbeta [site](https://example.com)", " tail\n\nnext\n",
      "alpha\n\nbeta site (https://example.com) tail\n\n  next\n", 0, 2 },
    { "alpha\n\nbeta ", "tail\n\nnext\n",
      "  alpha\n\n  beta tail\n\nnext\n", 2, 0 },
  }
  for case_index, case in ipairs(cases) do
    for chunk_mode = 1, 2 do
      local writes, traces = {}, {}
      local stream = mdf.document_stream({
        format = "ansi", ansi_mode = "on", boring = true, width = 80, margin_left = case[4],
        write_trace = function(_, chunk) traces[#traces + 1] = chunk end,
      }, function(chunk) writes[#writes + 1] = chunk end)
      local function feed(src)
        if chunk_mode == 1 then
          if #src > 0 then assert(stream:write(src)) end
        else
          for i = 1, #src do assert(stream:write(src:sub(i, i))) end
        end
      end
      feed(case[1])
      local committed = #writes
      assert(stream:set_geometry(80, case[5], 0),
             "lua margin matrix updates geometry")
      assert(#writes == committed and #traces == committed,
             "lua margin matrix setter never emits or traces")
      feed(case[2])
      assert(stream:finish_document(), "lua margin matrix finishes")
      assert_equal(table.concat(writes), case[3],
                   "lua margin matrix output case " .. case_index .. "/" .. chunk_mode)
      assert(#writes == #traces, "lua margin matrix trace count")
      for i = 1, #writes do
        assert_equal(writes[i], traces[i], "lua margin matrix trace bytes")
      end
      stream:close()
    end
  end
end

do
  local cases = {
    { "alpha\n\nbeta ", "tail\n\nnext\n", "alpha\n\nbeta tail\n\n   next\n" },
    { "alpha\n\nbeta `code`", " tail\n\nnext\n",
      "alpha\n\nbeta code tail\n\n   next\n" },
    { "alpha\n\nbeta <https://example.com>", " tail\n\nnext\n",
      "alpha\n\nbeta https://example.com tail\n\n   next\n" },
    { "alpha\n\nbeta [site](https://example.com)", " tail\n\nnext\n",
      "alpha\n\nbeta site (https://example.com) tail\n\n   next\n" },
  }
  for case_index, case in ipairs(cases) do
    local writes, traces = {}, {}
    local stream = mdf.document_stream({
      format = "ansi", ansi_mode = "on", boring = true, width = 80,
      write_trace = function(_, chunk) traces[#traces + 1] = chunk end,
    }, function(chunk) writes[#writes + 1] = chunk end)
    assert(stream:write(case[1]), "lua repeated geometry feeds first fragment")
    local committed = #writes
    assert(not pcall(function() stream:set_geometry(4, 2, 1) end),
           "lua invalid midline geometry is rejected")
    assert(#writes == committed and #traces == committed,
           "lua invalid midline geometry does not emit")
    assert(stream:set_geometry(25, 2, 1), "lua repeated geometry sets margins")
    assert(stream:set_width(30), "lua repeated geometry preserves margins on width change")
    assert(stream:set_geometry(40, 0, 0), "lua repeated geometry removes margins")
    assert(stream:set_geometry(40, 3, 2), "lua repeated geometry installs latest margins")
    assert(stream:flush(), "lua repeated geometry accepts a soft boundary")
    assert(#writes == committed and #traces == committed,
           "lua repeated geometry does not emit or trace")
    assert(stream:write(case[2]), "lua repeated geometry feeds last fragment")
    assert(stream:finish_document(), "lua repeated geometry finishes")
    assert_equal(table.concat(writes), case[3],
                 "lua repeated geometry output case " .. case_index)
    assert(stream:begin_document(), "lua repeated geometry starts a fresh document")
    assert(stream:write("fresh\n"), "lua repeated geometry writes fresh document")
    assert(stream:finish_document(), "lua repeated geometry finishes fresh document")
    assert(table.concat(writes):find("\n   fresh\n", 1, true),
           "lua fresh document clears previous line's margin suppression")
    assert(#writes == #traces, "lua repeated geometry trace count")
    for i = 1, #writes do
      assert_equal(writes[i], traces[i], "lua repeated geometry trace bytes")
    end
    stream:close()
  end
end

do
  local writes, traces = {}, {}
  local stream = mdf.document_stream({
    format = "ansi", ansi_mode = "on", boring = true, width = 12,
    table_buffer_mode = "row", table_wire_mode = "space",
    write_trace = function(_, chunk) traces[#traces + 1] = chunk end,
  }, function(chunk) writes[#writes + 1] = chunk end)
  assert(stream:write("| AlphaLongWord | 1234567890 |\n| BravoLongWord | 67890 |\n"),
         "lua row table emits its first rows before geometry change")
  local committed = #writes
  assert(committed > 0, "lua row table actually streamed before resize")
  assert(stream:set_geometry(40, 2, 2), "lua row table widens without a restart")
  assert(#writes == committed, "lua row geometry does not replay emitted rows")
  assert(stream:write("| LaterLongWord | 42 |\n\n"),
         "lua row table emits the next row at the new geometry")
  assert(stream:finish_document(), "lua row table closes at the new geometry")
  assert(table.concat(writes, "", committed + 1):find("LaterLongWord", 1, true),
         "lua row table recovers natural column width")
  assert(#writes == #traces, "lua row geometry sink and trace counts match")
  for i = 1, #writes do
    assert_equal(writes[i], traces[i], "lua row geometry sink and trace bytes match")
  end
  stream:close()
end

do
  local writes, traces = {}, {}
  local stream = mdf.document_stream({
    format = "ansi", ansi_mode = "on", boring = true, width = 20, table_buffer_mode = "row",
    write_trace = function(_, chunk) traces[#traces + 1] = chunk end,
  }, function(chunk) writes[#writes + 1] = chunk end)
  assert(stream:write("| abcdefghij | one two six ten |\n| --- | --- |\n" ..
                      "| abcdefghij | one two six ten |\n"),
         "lua row table emits before a one-column widening")
  local committed = #writes
  assert(committed > 0, "lua row table has committed first rows")
  assert(stream:set_geometry(21, 0, 0), "lua row table widens by one column")
  assert(#writes == committed, "lua row-table resize makes no sink write")
  assert(stream:write("| abcdefghij | one two six ten |\n\n"),
         "lua row table emits the next row after widening")
  assert(stream:finish_document(), "lua row table completes after widening")
  local future = table.concat(writes, "", committed + 1)
  assert(future:find("│ abcdefghij │ one ", 1, true),
         "lua row-table widening keeps the preferred whole-word width")
  assert(not future:find("│ abcdefg │", 1, true),
         "lua row-table widening does not split a word unnecessarily")
  assert(#writes == #traces, "lua row-table word-width trace count matches sink writes")
  for i = 1, #writes do
    assert_equal(writes[i], traces[i], "lua row-table word-width trace bytes match sink writes")
  end
  stream:close()
end

do
  local writes = {}
  local handle = mdf.new({ format = "ansi", ansi_mode = "on", boring = true, width = 12 })
  assert(handle:set_sink(function(chunk) writes[#writes + 1] = chunk end),
         "lua geometry handle binds its sink")
  local reads = 0
  assert(handle:render_stream(function()
    reads = reads + 1
    if reads == 1 then return "alpha\n\n" end
    if reads == 2 then
      assert(handle:set_geometry(12, 2, 2),
             "lua geometry changes from a synchronous source callback")
      return "beta gamma\n"
    end
    return nil
  end), "lua geometry handle keeps rendering through the same sink")
  assert(table.concat(writes):match("  beta\n  gamma"),
         "lua geometry handle applies the new margins and wrap width")
  handle:close()
end

do
  local writes, traces = {}, {}
  local stream = mdf.document_stream({
    format = "ansi", ansi_mode = "on", boring = true, width = 12,
    write_trace = function(_, chunk) traces[#traces + 1] = chunk end,
  }, function(chunk) writes[#writes + 1] = chunk end)
  assert(stream:write("alpha\n\n"), "lua geometry stream emits its first paragraph")
  local committed = #writes
  assert(stream:set_geometry(12, 2, 2), "lua stream updates geometry after output")
  assert(#writes == committed, "lua geometry does not replay committed output")
  assert(stream:write("beta gamma\n"), "lua stream continues the same document")
  assert(stream:finish_document(), "lua stream finishes after the geometry change")
  assert(table.concat(writes):match("^alpha\n\n  beta\n  gamma"),
         "lua active document applies new width and margin to future output")
  assert(#writes == #traces, "lua active geometry sink and trace counts match")
  for i = 1, #writes do
    assert_equal(writes[i], traces[i], "lua active geometry sink and trace bytes match")
  end
  stream:close()
end

do
  local first = {}
  local second = {}
  local stream = mdf.document_stream({ format = "ansi", ansi_mode = "on", boring = true, osc8 = true }, function(chunk)
    first[#first + 1] = chunk
  end)
  assert(stream:write("discarded "), "lua document stream starts before sink replacement")
  assert(stream:set_sink(function(chunk)
    second[#second + 1] = chunk
  end), "lua document stream replaces its persistent sink")
  assert(table.concat(first):find("\27%[0m"),
         "lua document stream closes ANSI state on the replaced sink")
  assert(stream:write("fresh\n"), "lua document stream accepts caller replay after sink replacement")
  assert(stream:finish_document(), "lua document stream finishes replay on replacement sink")
  assert(table.concat(second):match("fresh") and not table.concat(second):match("discarded"),
         "lua document stream does not buffer discarded input for replay")
  stream:close()
end

do
  local chunks = {}
  local sink = function(chunk)
    chunks[#chunks + 1] = chunk
  end
  local stream = mdf.document_stream({ format = "ansi", ansi_mode = "on", boring = true }, sink)
  assert(stream:write("retained"), "lua document stream starts before an identical sink bind")
  assert(stream:set_sink(sink), "lua document stream accepts an identical sink bind")
  assert(stream:write(" input\n"), "lua document stream continues after an identical sink bind")
  assert(stream:finish_document(), "lua document stream finishes after an identical sink bind")
  assert(table.concat(chunks):match("retained input"),
         "lua document stream does not reset state when its sink is unchanged")
  stream:close()
end

do
  local replacement = {}
  local stream = mdf.document_stream({ format = "ansi", ansi_mode = "on", boring = true }, function()
    return false
  end)
  assert(stream:write("discarded"),
         "lua document stream retains undecided input until a replacement reset")
  assert(stream:set_sink(function(chunk)
    replacement[#replacement + 1] = chunk
  end), "lua document stream adopts a replacement sink after old cleanup fails")
  assert(stream:write("fresh\n"),
         "lua document stream accepts caller replay after failed old-sink cleanup")
  assert(stream:finish_document(),
         "lua document stream finishes after failed old-sink cleanup")
  assert(table.concat(replacement):match("fresh") and not table.concat(replacement):match("discarded"),
         "lua document stream replacement discards state despite old cleanup failure")
  stream:close()
end

do
  local replacement = {}
  local stream = mdf.document_stream({ format = "ansi", ansi_mode = "on", boring = true }, function()
    return false
  end)
  assert(stream:write("discarded"), "lua document stream starts before a failed reset")
  local reset_ok = pcall(function() stream:reset() end)
  assert(not reset_ok and stream:error():match("sink write failed while resetting renderer"),
         "lua document stream reports a failed terminal reset write")
  assert(stream:set_sink(function(chunk)
    replacement[#replacement + 1] = chunk
  end), "lua document stream replaces its sink after a failed reset")
  assert(stream:write("fresh after reset\n"),
         "lua document stream accepts replay after a failed reset")
  assert(stream:finish_document(), "lua document stream finishes replay after a failed reset")
  assert(table.concat(replacement):match("fresh after reset") and
         not table.concat(replacement):match("discarded"),
         "lua document stream failed reset discards state without buffering replay")
  stream:close()
end

do
  local weak = setmetatable({}, { __mode = "v" })
  do
    local opts = { format = "html", font_uri = {} }
    local captured = {}
    local sink = function()
      return captured
    end
    weak.opts = opts
    weak.sink = sink
    weak.captured = captured
    local ok = pcall(mdf.document_stream, opts, sink)
    assert(not ok, "lua document stream rejects invalid font options")
  end
  collectgarbage("collect")
  collectgarbage("collect")
  assert(weak.opts == nil and weak.sink == nil and weak.captured == nil,
         "lua document stream constructor failure releases registry references")
end

do
  local handle_ok, handle_error = pcall(mdf.new, { width = 1 })
  local stream_ok, stream_error = pcall(mdf.document_stream, { width = 1 }, function() end)

  assert(not handle_ok and type(handle_error) == "string",
         "lua handle constructor reports invalid width without crashing")
  assert(not stream_ok and type(stream_error) == "string",
         "lua document stream constructor reports invalid width without crashing")
end

do
  local weak = setmetatable({}, { __mode = "v" })

  local function create_cyclic_handle()
    local handle

    handle = mdf.new({ format = "ansi", ansi_mode = "on", boring = true })
    assert(handle:set_sink(function()
      return handle:set_width(80)
    end), "lua handle accepts a sink that captures its receiver")
    weak.handle = handle
  end

  local function create_cyclic_stream()
    local stream

    stream = mdf.document_stream({ format = "ansi", ansi_mode = "on", boring = true }, function()
      return stream:set_width(80)
    end)
    weak.stream = stream
  end

  create_cyclic_handle()
  create_cyclic_stream()
  collectgarbage("collect")
  collectgarbage("collect")
  assert(weak.handle == nil and weak.stream == nil,
         "persistent Lua callbacks that capture their receiver remain collectible")
end

do
  local markdown = "# Split Lua\n\n- item\n\n`code` [link](https://example.com)\n"
  local chunks = {}
  local split = mdf.document_stream({ format = "ansi", ansi_mode = "on", boring = true }, function(chunk)
    chunks[#chunks + 1] = chunk
  end)
  for i = 1, #markdown do
    assert(split:write(markdown:sub(i, i)), "lua document stream accepts byte-sized fragments")
  end
  assert(split:finish_document(), "lua document stream finishes byte-sized fragments")
  assert_equal(table.concat(chunks), mdf.render(markdown, { ansi_mode = "on", boring = true }),
               "lua document stream byte split parity")
  split = nil
  collectgarbage("collect")
end

do
  local input = "(<https://example.com>.\n"
  local outputs = {}

  for run = 1, 2 do
    local writes = {}
    local traces = {}
    local stream = mdf.document_stream({
      format = "ansi", width = 21, margin_left = 1, margin_right = 1,
      write_trace = function(_, chunk) traces[#traces + 1] = chunk end,
    }, function(chunk) writes[#writes + 1] = chunk end)
    for i = 1, #input do
      local char = input:sub(i, i)
      assert(stream:write(char), "lua pending autolink accepts byte-sized input")
      if run == 2 and char == ">" then
        assert(stream:set_width(21), "lua unchanged width accepts pending autolink")
      end
    end
    assert(stream:finish_document(), "lua unchanged-width autolink finishes")
    assert(#writes > 0 and #writes == #traces,
           "lua unchanged-width autolink keeps sink and trace counts equal")
    for i = 1, #writes do
      assert_equal(writes[i], traces[i],
                   "lua unchanged-width autolink keeps each sink and trace emission equal")
    end
    outputs[run] = table.concat(writes)
    stream:close()
  end
  assert_equal(outputs[2], outputs[1],
               "lua unchanged width preserves pending autolink layout")
end

do
  local input = "> 1. <https://example.com> next\n"
  local expected = "> 1.\n>    https://example.com\n>    next\n"
  local writes, traces = {}, {}
  local stream = mdf.document_stream({
    format = "ansi", width = 20, ansi_mode = "on", boring = true, osc8 = false,
    write_trace = function(_, chunk) traces[#traces + 1] = chunk end,
  }, function(chunk) writes[#writes + 1] = chunk end)
  assert(stream:write("> 1. <https://example.com>"),
         "lua quoted-list autolink accepts pending prefix")
  local writes_before = #writes
  local traces_before = #traces
  assert(stream:set_width(21), "lua quoted-list autolink widens")
  assert(#writes == writes_before and #traces == traces_before,
         "lua quoted-list autolink width change does not emit")
  assert(stream:write(" next\n"), "lua quoted-list autolink accepts suffix")
  assert(stream:finish_document(), "lua quoted-list autolink finishes")
  assert(#writes == #traces, "lua quoted-list autolink sink and trace counts match")
  for i = 1, #writes do
    assert_equal(writes[i], traces[i],
                 "lua quoted-list autolink sink and trace bytes match")
  end
  assert_equal(table.concat(writes), expected,
               "lua quoted-list autolink does not add an empty prefix line")
  stream:close()
  assert_equal(mdf.render(input, { format = "ansi", width = 21, ansi_mode = "on", boring = true, osc8 = false }),
               expected, "lua quoted-list baseline agrees with resized stream")
end

local callback_failure = mdf.document_stream({ format = "ansi", ansi_mode = "on", boring = true }, function()
  return false
end)
local callback_ok = pcall(function() callback_failure:write("callback failure") end)
assert(not callback_ok, "lua document stream surfaces callback failure")
assert_equal(callback_failure:error(), "sink write failed",
             "lua document stream exposes its core error after callback failure")
callback_failure:close()

local ok, err = pcall(function()
  mdf.render(sample, {
    ansi_mode = "on", boring = true,
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

local handle_chunks = {}
local handle_sink = function(chunk)
  handle_chunks[#handle_chunks + 1] = chunk
end
assert(handle:set_sink(handle_sink), "lua handle binds one persistent sink")

ok, err = pcall(function()
  handle:render_stream(function()
    return false
  end)
end)
assert(not ok, "lua handle render_stream rejects non-string reader chunks")
assert(tostring(err):match("mdf_render_stream: io error"), tostring(err))

do
  local phase = 0
  assert(handle:reset(), "lua handle reset discards failed streaming state")
  assert(handle:set_width(80), "lua handle restores wide runtime width")
  assert(handle:render_stream(function()
    phase = phase + 1
    if phase == 1 then return "alpha " end
    if phase == 2 then
      local reset_ok = pcall(function() handle:reset() end)
      local sink_ok = pcall(function()
        handle:set_sink(function() end)
      end)
      local same_sink_ok = pcall(function()
        handle:set_sink(handle_sink)
      end)
      assert(not reset_ok, "lua handle reset rejects interruption from its reader")
      assert(not sink_ok, "lua handle set_sink rejects interruption from its reader")
      assert(same_sink_ok, "lua handle allows an identical sink bind from its reader")
      assert(handle:set_width(5), "lua handle changes width from its source callback")
      return "beta\n"
    end
    return nil
  end), "lua handle renders through its bound sink")
  assert(table.concat(handle_chunks):match("alpha\nbeta"),
         "lua handle applies source-time width changes to later decisions")
end

do
  local writes = {}
  local traces = {}
  local width_change_ok
  local trace_width_change_ok
  local geometry_change_ok
  local trace_geometry_change_ok
  local handle

  handle = mdf.new({
    format = "ansi",
    ansi_mode = "on", boring = true,
    width = 80,
    write_trace = function(_, chunk)
      traces[#traces + 1] = chunk
      if trace_width_change_ok == nil then
        trace_width_change_ok = pcall(function() handle:set_width(3) end)
        trace_geometry_change_ok = pcall(function() handle:set_geometry(12, 2, 2) end)
      end
    end,
  })
  assert(handle:set_sink(function(chunk)
    writes[#writes + 1] = chunk
    if width_change_ok == nil then
      width_change_ok = pcall(function() handle:set_width(3) end)
      geometry_change_ok = pcall(function() handle:set_geometry(12, 2, 2) end)
    end
  end), "lua handle binds an output-callback width-change regression sink")
  assert(handle:feed("`abcdefghij` text\n"),
         "lua handle renders while its sink attempts a width change")
  assert(handle:finish_document(),
         "lua handle finishes after rejecting an output-callback width change")
  assert(not width_change_ok,
         "lua handle rejects a width change from an output callback")
  assert(not trace_width_change_ok,
         "lua handle rejects a width change from a trace callback")
  assert(not geometry_change_ok and not trace_geometry_change_ok,
         "lua handle rejects geometry changes from sink and trace callbacks")
  assert_equal(table.concat(writes),
               mdf.render("`abcdefghij` text\n", { format = "ansi", ansi_mode = "on", boring = true, width = 80 }),
               "lua output-callback width rejection preserves the active layout")
  assert(#writes == #traces,
         "lua output-callback width rejection keeps sink and trace event counts aligned")
  for index, chunk in ipairs(writes) do
    assert_equal(traces[index], chunk,
                 "lua output-callback width rejection keeps sink and trace bytes aligned")
  end
  handle:close()
end

do
  local weak = setmetatable({}, { __mode = "v" })
  local handle_trace = function() end
  local stream_trace = function() end
  local ok

  weak[1] = handle_trace
  ok = pcall(function()
    mdf.new({
      format = "html",
      write_trace = handle_trace,
      html_font = { regular_format = "invalid" },
    })
  end)
  assert(not ok, "lua handle constructor rejects invalid HTML font options after trace options")
  handle_trace = nil

  weak[2] = stream_trace
  ok = pcall(function()
    mdf.document_stream({
      format = "html",
      write_trace = stream_trace,
      html_font = { regular_format = "invalid" },
    }, function() end)
  end)
  assert(not ok, "lua stream constructor rejects invalid HTML font options after trace options")
  stream_trace = nil
  collectgarbage("collect")
  collectgarbage("collect")
  assert(weak[1] == nil and weak[2] == nil,
         "failed Lua constructors release untransferred trace callback references")
end

do
  local writes = {}
  local traces = {}
  local handle = mdf.new({
    format = "ansi",
    ansi_mode = "on", boring = true,
    width = 80,
    write_trace = function(_, chunk)
      traces[#traces + 1] = chunk
    end,
  })

  assert(handle:set_sink(function(chunk)
    writes[#writes + 1] = chunk
  end), "lua handle binds a separator-reflow regression sink")
  assert(handle:feed("a "), "lua handle accepts the prefix before pending code")
  assert(handle:feed("`a.b.c.d.e.f`"), "lua handle retains complete code before its boundary")
  assert_equal(table.concat(writes), "a",
               "lua pending code has not decided its separator before resize")
  assert(handle:set_width(5), "lua handle accepts a narrow width before pending code emission")
  assert(handle:feed(" text\n"), "lua handle continues after pending-code resize")
  assert(handle:finish_document(), "lua handle finishes separator-reflow regression input")
  assert_equal(table.concat(writes), "a\na.b.…\ntext\n",
               "lua separator reflow recomputes placement at the new width")
  assert_equal(#writes, #traces,
               "lua separator reflow keeps sink and trace event counts aligned")
  for index, chunk in ipairs(writes) do
    assert_equal(traces[index], chunk,
                 "lua separator reflow keeps sink and trace event bytes aligned")
  end
  handle:close()
end

do
  local writes = {}
  local width_change_ok
  local handle = mdf.new({
    format = "html",
    width = 80,
    html_title = "HTML callback guard",
  })

  assert(handle:set_sink(function(chunk)
    writes[#writes + 1] = chunk
    if width_change_ok == nil then
      width_change_ok = pcall(function() handle:set_width(3) end)
    end
  end), "lua HTML handle binds an output-callback width-change sink")
  assert(handle:feed("HTML callback width guard\n"),
         "lua HTML handle renders while its sink attempts a width change")
  assert(handle:finish_document(),
         "lua HTML handle finishes after rejecting an output-callback width change")
  assert(not width_change_ok,
         "lua HTML handle rejects a width change from an output callback")
  assert_equal(table.concat(writes),
               mdf.render("HTML callback width guard\n", {
                 format = "html",
                 width = 80,
                 html_title = "HTML callback guard",
               }),
               "lua HTML output-callback width rejection preserves output")
  handle:close()
end

do
  local replacement = {}
  assert(handle:set_sink(function()
    return false
  end), "lua handle binds a sink that will reject terminal cleanup")
  assert(handle:set_sink(function(chunk)
    replacement[#replacement + 1] = chunk
  end), "lua handle adopts a replacement sink after old cleanup fails")
  assert_equal(handle:error(), "", "lua handle replacement clears the old sink error")
  local emitted = false
  assert(handle:render_stream(function()
    if emitted then return nil end
    emitted = true
    return "fresh\n"
  end), "lua handle renders through a replacement sink after failed cleanup")
  assert(table.concat(replacement):match("fresh"),
         "lua handle sends subsequent output only to its replacement sink")
end

do
  local replacement = {}
  local reset_handle = mdf.new({ format = "ansi", ansi_mode = "on", boring = true })
  assert(reset_handle:set_sink(function()
    return false
  end), "lua handle binds a sink before a failed reset")
  assert(reset_handle:feed("discarded"), "lua handle starts before a failed reset")
  local reset_ok = pcall(function() reset_handle:reset() end)
  assert(not reset_ok and reset_handle:error():match("sink write failed while resetting renderer"),
         "lua handle reports a failed terminal reset write")
  assert(reset_handle:set_sink(function(chunk)
    replacement[#replacement + 1] = chunk
  end), "lua handle replaces its sink after a failed reset")
  assert(reset_handle:feed("fresh after reset\n"), "lua handle accepts replay after a failed reset")
  assert(reset_handle:finish_document(), "lua handle finishes replay after a failed reset")
  reset_handle:close()
  assert(table.concat(replacement):match("fresh after reset") and
         not table.concat(replacement):match("discarded"),
         "lua handle failed reset discards state without buffering replay")
end

do
  local stream_old_chunks = {}
  local stream_new_chunks = {}
  local stream_trace = {}
  local stream_co = coroutine.create(function()
    return mdf.document_stream({
      format = "ansi",
      ansi_mode = "on", boring = true,
      write_trace = function(_, chunk)
        stream_trace[#stream_trace + 1] = chunk
      end,
    }, function(chunk)
      stream_old_chunks[#stream_old_chunks + 1] = chunk
    end)
  end)
  local stream_ok, coroutine_stream = coroutine.resume(stream_co)
  assert(stream_ok, "lua coroutine creates a document stream")
  stream_co = nil
  collectgarbage("collect")
  collectgarbage("collect")
  assert(coroutine_stream:set_sink(function(chunk)
    stream_new_chunks[#stream_new_chunks + 1] = chunk
  end), "lua document stream replaces a coroutine-created sink after collection")
  assert(coroutine_stream:write("stream after coroutine collection\n"),
         "lua document stream refreshes its collected coroutine callback context")
  assert(coroutine_stream:finish_document(), "lua document stream finishes after coroutine collection")
  coroutine_stream:close()
  assert(table.concat(stream_old_chunks):match("\27%[0m") and
         table.concat(stream_new_chunks):match("stream after coroutine collection") and
         #stream_trace > 0,
         "lua document stream refreshes sink and trace callbacks for each caller state")

  local handle_old_chunks = {}
  local handle_new_chunks = {}
  local handle_trace = {}
  local handle_co = coroutine.create(function()
    local coroutine_handle = mdf.new({
      format = "ansi",
      ansi_mode = "on", boring = true,
      write_trace = function(_, chunk)
        handle_trace[#handle_trace + 1] = chunk
      end,
    })
    assert(coroutine_handle:set_sink(function(chunk)
      handle_old_chunks[#handle_old_chunks + 1] = chunk
    end))
    return coroutine_handle
  end)
  local handle_ok, coroutine_handle = coroutine.resume(handle_co)
  assert(handle_ok, "lua coroutine creates a bound handle")
  handle_co = nil
  collectgarbage("collect")
  collectgarbage("collect")
  assert(coroutine_handle:reset(), "lua handle resets a coroutine-created sink after collection")
  assert(coroutine_handle:set_sink(function(chunk)
    handle_new_chunks[#handle_new_chunks + 1] = chunk
  end), "lua handle replaces its sink after coroutine collection")
  assert(coroutine_handle:feed("handle after coroutine collection\n"),
         "lua handle refreshes its collected coroutine callback context")
  assert(coroutine_handle:finish_document(), "lua handle finishes after coroutine collection")
  coroutine_handle:close()
  assert(table.concat(handle_old_chunks):match("\27%[0m") and
         table.concat(handle_new_chunks):match("handle after coroutine collection") and
         #handle_trace > 0,
         "lua handle refreshes sink and trace callbacks for each caller state")

  local nested_chunks = {}
  local nested_handle = mdf.new({ format = "ansi", ansi_mode = "on", boring = true })
  local nested_once = false
  local nested_sink
  nested_sink = function(chunk)
    nested_chunks[#nested_chunks + 1] = chunk
    if not nested_once then
      local nested_co

      nested_once = true
      nested_co = coroutine.create(function()
        local reset_ok = pcall(function() nested_handle:reset() end)
        local width_ok = pcall(function() nested_handle:set_width(80) end)

        assert(not reset_ok)
        assert(nested_handle:set_sink(nested_sink))
        assert(not width_ok)
      end)
      assert(coroutine.resume(nested_co),
             "nested coroutine can inspect and configure an active lua handle")
    end
  end
  assert(nested_handle:set_sink(nested_sink), "lua handle binds a nested-coroutine regression sink")
  assert(nested_handle:feed("nested coroutine callback\n"),
         "nested coroutine leaves the active sink callback context intact")
  assert(nested_handle:finish_document(), "nested coroutine handle finishes its document")
  nested_handle:close()
  assert(table.concat(nested_chunks):match("nested coroutine callback"),
         "nested coroutine callback preserves subsequent Lua sink writes")

  local source_chunks = {}
  local source_handle = mdf.new({ format = "ansi", ansi_mode = "on", boring = true })
  local source_done = false
  assert(source_handle:set_sink(function(chunk)
    source_chunks[#source_chunks + 1] = chunk
  end), "lua handle binds a nested-source regression sink")
  assert(source_handle:render_stream(function()
    if source_done then return nil end
    source_done = true
    local source_co = coroutine.create(function()
      local render_ok = pcall(function()
        source_handle:render_stream(function()
          return nil
        end)
      end)
      local reset_ok = pcall(function() source_handle:reset() end)
      local close_ok = pcall(function() source_handle:close() end)

      assert(not render_ok)
      assert(not reset_ok)
      assert(not close_ok)
    end)
    assert(coroutine.resume(source_co),
           "nested coroutine cannot reset an active lua source render")
    source_co = nil
    collectgarbage("collect")
    return "source callback after nested coroutine\n"
  end), "nested source coroutine leaves later sink callback context valid")
  source_handle:close()
  assert(table.concat(source_chunks):match("source callback after nested coroutine"),
         "nested source coroutine preserves Lua sink writes after collection")

  local close_stream_chunks = {}
  local close_stream
  close_stream = mdf.document_stream({ format = "ansi", ansi_mode = "on", boring = true }, function(chunk)
    close_stream_chunks[#close_stream_chunks + 1] = chunk
    local close_ok = pcall(function() close_stream:close() end)
    assert(not close_ok, "lua document stream rejects close from its sink callback")
  end)
  assert(close_stream:write("close callback remains live\n"),
         "lua document stream remains valid after rejected callback close")
  assert(close_stream:finish_document(),
         "lua document stream finishes after rejected callback close")
  close_stream:close()
  assert(table.concat(close_stream_chunks):match("close callback remains live"),
         "lua document stream preserves its sink after rejected callback close")

  local reset_chunks = {}
  local reset_trace = {}
  local reset_handle
  local reset_reentered = false
  reset_handle = mdf.new({
    format = "ansi",
    ansi_mode = "on", boring = true,
    write_trace = function(_, chunk)
      reset_trace[#reset_trace + 1] = chunk
    end,
  })
  assert(reset_handle:set_sink(function(chunk)
    reset_chunks[#reset_chunks + 1] = chunk
    if not reset_reentered then
      local feed_ok

      reset_reentered = true
      feed_ok = pcall(function() reset_handle:feed("nested ") end)
      assert(not feed_ok, "lua reset sink callback cannot reenter feed")
    end
  end), "lua handle binds a reset reentry regression sink")
  assert(reset_handle:reset(), "lua handle resets through its guarded sink")
  assert(#reset_chunks == #reset_trace,
         "lua reset sink writes and traces have matching event counts")
  for index, chunk in ipairs(reset_chunks) do
    assert_equal(reset_trace[index], chunk,
                 "lua reset sink writes and traces preserve exact bytes")
  end
  reset_handle:close()

  local handle_a_chunks = {}
  local handle_b_chunks = {}
  local handle_c_chunks = {}
  local replacement_handle
  local handle_nested_replace_ok
  replacement_handle = mdf.new({ format = "ansi", ansi_mode = "on", boring = true })
  assert(replacement_handle:set_sink(function(chunk)
    handle_a_chunks[#handle_a_chunks + 1] = chunk
    if handle_nested_replace_ok == nil then
      handle_nested_replace_ok = pcall(function()
        replacement_handle:set_sink(function(next_chunk)
          handle_c_chunks[#handle_c_chunks + 1] = next_chunk
        end)
      end)
    end
  end), "lua handle binds the old replacement sink")
  assert(replacement_handle:set_sink(function(chunk)
    handle_b_chunks[#handle_b_chunks + 1] = chunk
  end), "lua handle installs the requested replacement sink")
  assert(not handle_nested_replace_ok,
         "lua handle rejects nested sink replacement during terminal cleanup")
  assert(replacement_handle:feed("handle replacement target\n"),
         "lua handle accepts output after guarded sink replacement")
  assert(replacement_handle:finish_document(),
         "lua handle finishes after guarded sink replacement")
  replacement_handle:close()
  assert(table.concat(handle_b_chunks):match("handle replacement target") and #handle_c_chunks == 0,
         "lua handle retains the outer replacement rather than a nested sink")

  local stream_a_chunks = {}
  local stream_b_chunks = {}
  local stream_c_chunks = {}
  local replacement_stream
  local stream_nested_replace_ok
  replacement_stream = mdf.document_stream({ format = "ansi", ansi_mode = "on", boring = true }, function(chunk)
    stream_a_chunks[#stream_a_chunks + 1] = chunk
    if stream_nested_replace_ok == nil then
      stream_nested_replace_ok = pcall(function()
        replacement_stream:set_sink(function(next_chunk)
          stream_c_chunks[#stream_c_chunks + 1] = next_chunk
        end)
      end)
    end
  end)
  assert(replacement_stream:set_sink(function(chunk)
    stream_b_chunks[#stream_b_chunks + 1] = chunk
  end), "lua document stream installs the requested replacement sink")
  assert(not stream_nested_replace_ok,
         "lua document stream rejects nested sink replacement during terminal cleanup")
  assert(replacement_stream:write("stream replacement target\n"),
         "lua document stream accepts output after guarded sink replacement")
  assert(replacement_stream:finish_document(),
         "lua document stream finishes after guarded sink replacement")
  replacement_stream:close()
  assert(table.concat(stream_b_chunks):match("stream replacement target") and #stream_c_chunks == 0,
         "lua document stream retains the outer replacement rather than a nested sink")
end

handle:close()

local html_handle = mdf.new({ format = "html" })
html_handle:set_html_title("Handle HTML")
local handle_html = html_handle:render("# Ignored\n\nbody\n")
html_handle:close()
assert(handle_html:match("<title>Handle HTML</title>"), "lua html handle set_html_title applies title")

do
  local chunks = {}
  local stream = mdf.document_stream({ format = "html" }, function(chunk)
    chunks[#chunks + 1] = chunk
  end)
  assert(stream:set_html_title("Stream HTML Setter"),
         "lua document stream sets an HTML title before its first write")
  assert(stream:write("# Ignored Stream Heading\n\nbody\n"),
         "lua document stream accepts HTML after setting its title")
  local late_title_ok = pcall(function() stream:set_html_title("Late Stream Title") end)
  assert(not late_title_ok and stream:error():match("HTML title must be set before rendering starts"),
         "lua document stream rejects an HTML title change after its first write")
  assert(stream:finish_document(), "lua document stream finishes HTML after setting its title")
  stream:close()
  assert(table.concat(chunks):match("<title>Stream HTML Setter</title>"),
         "lua document stream applies its explicit HTML title")
  assert(not table.concat(chunks):match("Late Stream Title"),
         "lua document stream preserves the title emitted in its shell")
end

local external_font_html = mdf.render("# External Font\n", {
  html = true,
  font_uri = "fonts",
  font_regular_uri = "fonts/JetBrainsMono-Regular.woff2",
  font_italic_uri = "fonts/JetBrainsMono-Italic.woff2",
})
assert(external_font_html:match('src:url%("fonts/JetBrainsMono%-Regular%.woff2"%) format%(\'woff2\'%)'),
  "lua paired regular font URI disables embedded font")
assert(external_font_html:match('src:url%("fonts/JetBrainsMono%-Italic%.woff2"%) format%(\'woff2\'%)'),
  "lua paired italic font URI disables embedded font")
assert(not external_font_html:match("data:font/woff2;base64,"),
  "lua external font configuration omits embedded font data")

local default_external_font_html = mdf.render("# Default External Font\n", {
  html = true,
  disable_embedded_font = true,
})
assert(default_external_font_html:find("JetBrainsMono%5Bwght%5D.woff2", 1, true),
  "lua disable_embedded_font uses the byte-identical regular variable web font URI")
assert(default_external_font_html:find("JetBrainsMono-Italic%5Bwght%5D.woff2", 1, true),
  "lua disable_embedded_font uses the byte-identical italic variable web font URI")
assert(not default_external_font_html:match("data:font/woff2;base64,"),
  "lua disable_embedded_font omits embedded font data")

local lua_regular_font_path = os.tmpname()
local lua_italic_font_path = os.tmpname()
os.remove(lua_regular_font_path)
os.remove(lua_italic_font_path)
local lua_regular_font_uri = "file://" .. lua_regular_font_path
local lua_italic_font_uri = "file://" .. lua_italic_font_path
local lua_dumped_font_html = mdf.render("# Lua Local Font\n", {
  html = true,
  font_regular_uri = lua_regular_font_uri,
  font_italic_uri = lua_italic_font_uri,
  dump_font = true,
})
assert(lua_dumped_font_html:find(lua_regular_font_uri, 1, true),
  "lua local regular file URI remains the HTML reference")
assert(lua_dumped_font_html:find(lua_italic_font_uri, 1, true),
  "lua local italic file URI remains the HTML reference")
assert(not lua_dumped_font_html:match("data:font/woff2;base64,"),
  "lua local file URI dumping omits embedded font data")
local lua_regular_font_file = assert(io.open(lua_regular_font_path, "rb"))
assert(lua_regular_font_file:read(1) == "w", "lua local file URI dump writes regular WOFF2 data")
lua_regular_font_file:close()
os.remove(lua_regular_font_path)
os.remove(lua_italic_font_path)

local lua_passive_dump_path_html = mdf.render("# Lua Passive Dump Paths\n", {
  html = true,
  font_regular_path = lua_regular_font_path,
  font_italic_path = lua_italic_font_path,
})
assert(lua_passive_dump_path_html:match("data:font/woff2;base64,"),
  "lua dump paths do not disable embedded fonts without dump_font")
assert(io.open(lua_regular_font_path, "rb") == nil and io.open(lua_italic_font_path, "rb") == nil,
  "lua dump paths do not write files without dump_font")

local token_out = {}
local token_handle = mdf.new({ ansi_mode = "on", boring = true })
token_handle:set_sink(function(chunk) token_out[#token_out + 1] = chunk end)
token_handle:write_token({ type = mdf.token.TEXT, text = "Hello" })
token_handle:write_token({ type = "space", text = " " })
token_handle:write_token({ type = "text", text = "Lua" })
token_handle:write_token({ type = "document_end" })
token_handle:finish()
token_handle:close()
assert(table.concat(token_out):match("Hello Lua"), "lua manual token API renders text")

do
  local chunks = {}
  local receiver = mdf.new({ format = "ansi", ansi_mode = "on", boring = true })
  local sink = function(chunk) chunks[#chunks + 1] = chunk end
  assert(receiver:set_sink(sink),
         "lua handle binds its incremental receiver sink")
  assert(receiver:feed("first"), "lua handle feeds an incremental document")
  assert(receiver:set_sink(sink), "lua handle accepts an identical sink bind")
  assert(receiver:flush(), "lua handle flushes an incremental document")
  assert(receiver:finish_document(), "lua handle finishes an incremental document")
  assert(receiver:begin_document(), "lua handle begins a distinct next incremental document")
  assert(receiver:feed("second\n"), "lua handle feeds its next incremental document")
  assert(receiver:finish_document(), "lua handle finishes its next incremental document")
  receiver:close()
  assert(table.concat(chunks):match("first") and table.concat(chunks):match("second"),
         "lua handle preserves its sink across incremental document boundaries")
end

local cmdf_lua_ansi = run_capture(shell_quote(cmdf_lua) .. " --ansi on -b", sample)
assert_equal(cmdf_lua_ansi, cmdf_ansi, "cmdf.lua ansi parity")

local cmdf_lua_margin = run_capture(shell_quote(cmdf_lua) .. " --ansi on -b -w 10 --margin-left 2 --margin-right 1", margin_sample)
assert_equal(cmdf_lua_margin, margin_expected, "cmdf.lua ansi margins")

local cmdf_version = run_command_capture(shell_quote(cmdf) .. " --ansi on --version")
local cmdf_lua_version = run_command_capture(shell_quote(cmdf_lua) .. " --ansi on --version")
assert_equal(cmdf_lua_version, cmdf_version, "cmdf.lua version parity")

local invalid_width = run_expect_fail(shell_quote(cmdf_lua) .. " --ansi on -w 0", sample)
assert(invalid_width:match("invalid width"), invalid_width)

local invalid_fractional_width = run_expect_fail(shell_quote(cmdf_lua) .. " --ansi on -w 10.5", sample)
assert(invalid_fractional_width:match("invalid width"), invalid_fractional_width)

local invalid_large_width = run_expect_fail(shell_quote(cmdf_lua) .. " --ansi on --html -w 10001", sample)
assert(invalid_large_width:match("invalid width"), invalid_large_width)

local invalid_html_width = run_expect_fail(shell_quote(cmdf_lua) .. " --ansi on --html --html-content-width 0", sample)
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

local lua_deck_external_font = mdf.render(deck_sample, {
  format = "deck",
  font_uri = "deck-fonts",
})
assert(lua_deck_external_font:match("deck%-fonts/JetBrainsMono%-Regular%.woff2"),
       "lua deck external font URI renders regular reference")
assert(lua_deck_external_font:match("deck%-fonts/JetBrainsMono%-Italic%.woff2"),
       "lua deck external font URI renders italic reference")
assert(not lua_deck_external_font:match("data:font/woff2;base64,"),
       "lua deck external font URI omits embedded faces")

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

local cmdf_lua_traced, cmdf_lua_trace = run_capture_with_trace(shell_quote(cmdf_lua) .. " --ansi on -b", sample)
assert_equal(cmdf_lua_traced, cmdf_ansi, "cmdf.lua traced ansi parity")
local trace_lines = 0
for _ in cmdf_lua_trace:gmatch("[^\n]+") do trace_lines = trace_lines + 1 end
assert(trace_lines > 3, "cmdf.lua traced render emits decision chunks")

local cmdf_html = run_capture(shell_quote(cmdf) .. " --ansi on --html", sample)
local cmdf_lua_html = run_capture(shell_quote(cmdf_lua) .. " --ansi on --html", sample)
assert(cmdf_lua_html:match("JetBrains Mono"), "cmdf.lua html embeds JetBrains Mono")
assert_equal(cmdf_lua_html, cmdf_html, "cmdf.lua html parity")
assert(mdf.path_aliases_stdout("/dev/stdout"), "lua facade identifies stdout path aliases")

local cmdf_lua_regular_font_path = os.tmpname()
local cmdf_lua_italic_font_path = os.tmpname()
os.remove(cmdf_lua_regular_font_path)
os.remove(cmdf_lua_italic_font_path)

local cmdf_lua_bare_dump_dir = os.tmpname()
os.remove(cmdf_lua_bare_dump_dir)
assert(os.execute("mkdir -p " .. shell_quote(cmdf_lua_bare_dump_dir)))
local cmdf_lua_bare_output_path = cmdf_lua_bare_dump_dir .. "/index.html"
local cmdf_lua_bare_dumped_font_html = run_capture(
  shell_quote(cmdf_lua) .. " --ansi on --html --html-dump-font -o " .. shell_quote(cmdf_lua_bare_output_path), sample)
assert_equal(cmdf_lua_bare_dumped_font_html, "", "cmdf.lua bare dump writes explicit output")
local cmdf_lua_bare_output = assert(io.open(cmdf_lua_bare_output_path, "rb"))
local cmdf_lua_bare_html = cmdf_lua_bare_output:read("*a")
cmdf_lua_bare_output:close()
assert(cmdf_lua_bare_html:find('src:url("./JetBrainsMono-Regular.woff2")', 1, true),
       "cmdf.lua bare dump references fonts beside explicit output")
local cmdf_lua_bare_regular_font = assert(io.open(cmdf_lua_bare_dump_dir .. "/JetBrainsMono-Regular.woff2", "rb"))
assert(cmdf_lua_bare_regular_font:read(1) == "w", "cmdf.lua bare dump writes regular font beside explicit output")
cmdf_lua_bare_regular_font:close()
os.remove(cmdf_lua_bare_dump_dir .. "/JetBrainsMono-Regular.woff2")
os.remove(cmdf_lua_bare_dump_dir .. "/JetBrainsMono-Italic.woff2")
os.remove(cmdf_lua_bare_output_path)
assert(os.execute("rmdir " .. shell_quote(cmdf_lua_bare_dump_dir)))
local cmdf_lua_relative_dump_dir = os.tmpname()
os.remove(cmdf_lua_relative_dump_dir)
assert(os.execute("mkdir -p " .. shell_quote(cmdf_lua_relative_dump_dir .. "/out") ..
                  " " .. shell_quote(cmdf_lua_relative_dump_dir .. "/assets#v1?%")))
local cmdf_lua_relative_input = os.tmpname()
local cmdf_lua_relative_input_file = assert(io.open(cmdf_lua_relative_input, "wb"))
cmdf_lua_relative_input_file:write(sample)
cmdf_lua_relative_input_file:close()
local cmdf_lua_relative_dump_output = run_command_capture(
  "cd " .. shell_quote(cmdf_lua_relative_dump_dir) .. " && " .. shell_quote(cmdf_lua) .. " --ansi on" ..
  " --html --html-dump-font-path 'assets#v1?%' -o out/index.html " .. shell_quote(cmdf_lua_relative_input))
assert_equal(cmdf_lua_relative_dump_output, "", "cmdf.lua relative font dump writes explicit output")
local cmdf_lua_relative_html_file = assert(io.open(cmdf_lua_relative_dump_dir .. "/out/index.html", "rb"))
local cmdf_lua_relative_html = cmdf_lua_relative_html_file:read("*a")
cmdf_lua_relative_html_file:close()
assert(cmdf_lua_relative_html:find('src:url("../assets%23v1%3F%25/JetBrainsMono-Regular.woff2")', 1, true),
       "cmdf.lua references relative regular dump paths from the HTML output directory")
assert(cmdf_lua_relative_html:find('src:url("../assets%23v1%3F%25/JetBrainsMono-Italic.woff2")', 1, true),
       "cmdf.lua references relative italic dump paths from the HTML output directory")
local cmdf_lua_relative_regular_font = assert(io.open(
  cmdf_lua_relative_dump_dir .. "/assets#v1?%/JetBrainsMono-Regular.woff2", "rb"))
assert(cmdf_lua_relative_regular_font:read(1) == "w",
       "cmdf.lua writes relative dump paths from its working directory")
cmdf_lua_relative_regular_font:close()
os.remove(cmdf_lua_relative_dump_dir .. "/assets#v1?%/JetBrainsMono-Regular.woff2")
os.remove(cmdf_lua_relative_dump_dir .. "/assets#v1?%/JetBrainsMono-Italic.woff2")
os.remove(cmdf_lua_relative_dump_dir .. "/out/index.html")
os.remove(cmdf_lua_relative_input)
assert(os.execute("rmdir " .. shell_quote(cmdf_lua_relative_dump_dir .. "/assets#v1?%") ..
                  " " .. shell_quote(cmdf_lua_relative_dump_dir .. "/out") ..
                  " " .. shell_quote(cmdf_lua_relative_dump_dir)))
local cmdf_lua_stdout_dump_dir = os.tmpname()
os.remove(cmdf_lua_stdout_dump_dir)
assert(os.execute("mkdir -p " .. shell_quote(cmdf_lua_stdout_dump_dir .. "/assets#stdout?%")))
local cmdf_lua_stdout_input = os.tmpname()
local cmdf_lua_stdout_input_file = assert(io.open(cmdf_lua_stdout_input, "wb"))
cmdf_lua_stdout_input_file:write(sample)
cmdf_lua_stdout_input_file:close()
local cmdf_lua_stdout_dump_html = run_command_capture(
  "cd " .. shell_quote(cmdf_lua_stdout_dump_dir) .. " && " .. shell_quote(cmdf_lua) .. " --ansi on" ..
  " --html --html-dump-font-path 'assets#stdout?%' " .. shell_quote(cmdf_lua_stdout_input))
assert(cmdf_lua_stdout_dump_html:find('src:url("assets%23stdout%3F%25/JetBrainsMono-Regular.woff2")', 1, true),
       "cmdf.lua URI-encodes local regular dump paths when HTML is written to stdout")
assert(cmdf_lua_stdout_dump_html:find('src:url("assets%23stdout%3F%25/JetBrainsMono-Italic.woff2")', 1, true),
       "cmdf.lua URI-encodes local italic dump paths when HTML is written to stdout")
local cmdf_lua_stdout_regular_font = assert(io.open(
  cmdf_lua_stdout_dump_dir .. "/assets#stdout?%/JetBrainsMono-Regular.woff2", "rb"))
assert(cmdf_lua_stdout_regular_font:read(1) == "w",
       "cmdf.lua writes the stdout regular font dump at its local filesystem path")
cmdf_lua_stdout_regular_font:close()
os.remove(cmdf_lua_stdout_dump_dir .. "/assets#stdout?%/JetBrainsMono-Regular.woff2")
os.remove(cmdf_lua_stdout_dump_dir .. "/assets#stdout?%/JetBrainsMono-Italic.woff2")
os.remove(cmdf_lua_stdout_input)
assert(os.execute("rmdir " .. shell_quote(cmdf_lua_stdout_dump_dir .. "/assets#stdout?%") ..
                  " " .. shell_quote(cmdf_lua_stdout_dump_dir)))
local cmdf_lua_dumped_font_html = run_capture(
  shell_quote(cmdf_lua) .. " --ansi on --html --html-dump-font-regular-path " ..
  shell_quote(cmdf_lua_regular_font_path) .. " --html-dump-font-italic-path " ..
  shell_quote(cmdf_lua_italic_font_path), sample)
assert(cmdf_lua_dumped_font_html:find(cmdf_lua_regular_font_path, 1, true),
  "cmdf.lua dump path references dumped regular font")
assert(not cmdf_lua_dumped_font_html:match("data:font/woff2;base64,"),
  "cmdf.lua dump paths disable embedded fonts")
local cmdf_lua_regular_font = assert(io.open(cmdf_lua_regular_font_path, "rb"))
assert(cmdf_lua_regular_font:read(1) == "w", "cmdf.lua dump paths write the regular WOFF2 font")
cmdf_lua_regular_font:close()
os.remove(cmdf_lua_regular_font_path)
os.remove(cmdf_lua_italic_font_path)

local cmdf_lua_alias_output_path = os.tmpname()
local cmdf_lua_alias_italic_path = os.tmpname()
os.remove(cmdf_lua_alias_output_path)
os.remove(cmdf_lua_alias_italic_path)
local cmdf_lua_alias_output = run_expect_fail(
  shell_quote(cmdf_lua) .. " --ansi on --html --html-dump-font-regular-path " ..
  shell_quote(cmdf_lua_alias_output_path) .. " --html-dump-font-italic-path " ..
  shell_quote(cmdf_lua_alias_italic_path) .. " -o " .. shell_quote(cmdf_lua_alias_output_path), sample)
assert(cmdf_lua_alias_output:match("font dump destination aliases input, output, or stdout"),
  "cmdf.lua rejects a font dump destination that aliases output")
assert(io.open(cmdf_lua_alias_output_path, "rb") == nil,
  "cmdf.lua alias rejection leaves a new output path untouched")
os.remove(cmdf_lua_alias_italic_path)

local cmdf_lua_alias_input_path = os.tmpname()
local cmdf_lua_alias_input_italic_path = os.tmpname()
local cmdf_lua_alias_input_file = assert(io.open(cmdf_lua_alias_input_path, "wb"))
cmdf_lua_alias_input_file:write("# Preserve this input\n")
cmdf_lua_alias_input_file:close()
os.remove(cmdf_lua_alias_input_italic_path)
local cmdf_lua_alias_input = run_command_expect_fail(
  shell_quote(cmdf_lua) .. " --ansi on --html --html-dump-font-regular-path " ..
  shell_quote(cmdf_lua_alias_input_path) .. " --html-dump-font-italic-path " ..
  shell_quote(cmdf_lua_alias_input_italic_path) .. " " .. shell_quote(cmdf_lua_alias_input_path))
assert(cmdf_lua_alias_input:match("font dump destination aliases input, output, or stdout"),
  "cmdf.lua rejects a font dump destination that aliases input")
local cmdf_lua_alias_input_check = assert(io.open(cmdf_lua_alias_input_path, "rb"))
assert_equal(cmdf_lua_alias_input_check:read("*a"), "# Preserve this input\n",
  "cmdf.lua alias rejection preserves the input file")
cmdf_lua_alias_input_check:close()
os.remove(cmdf_lua_alias_input_path)
os.remove(cmdf_lua_alias_input_italic_path)

local cmdf_lua_stdout_alias_italic_path = os.tmpname()
os.remove(cmdf_lua_stdout_alias_italic_path)
local cmdf_lua_stdout_alias = run_expect_fail(
  shell_quote(cmdf_lua) .. " --ansi on --html --html-dump-font-force --html-dump-font-regular-path /dev/stdout" ..
  " --html-dump-font-italic-path " .. shell_quote(cmdf_lua_stdout_alias_italic_path), sample)
assert(cmdf_lua_stdout_alias:match("font dump destination aliases input, output, or stdout"),
  "cmdf.lua rejects a font dump destination that aliases stdout")
assert(io.open(cmdf_lua_stdout_alias_italic_path, "rb") == nil,
  "cmdf.lua stdout alias rejection does not create the paired font")
os.remove(cmdf_lua_stdout_alias_italic_path)

local cmdf_lua_suffix_regular_path = os.tmpname()
local cmdf_lua_suffix_italic_path = os.tmpname()
os.remove(cmdf_lua_suffix_regular_path)
os.remove(cmdf_lua_suffix_italic_path)
local cmdf_lua_suffix_only = run_expect_fail(
  shell_quote(cmdf_lua) .. " --ansi on --html --html-font-uri '?v=1' --html-dump-font-force " ..
  "--html-dump-font-regular-path " .. shell_quote(cmdf_lua_suffix_regular_path) ..
  " --html-dump-font-italic-path " .. shell_quote(cmdf_lua_suffix_italic_path), sample)
assert(cmdf_lua_suffix_only:match("html font dump paths: invalid argument"),
  "cmdf.lua rejects query-only HTML font URI bases")
assert(io.open(cmdf_lua_suffix_regular_path, "rb") == nil and
       io.open(cmdf_lua_suffix_italic_path, "rb") == nil,
       "cmdf.lua query-only font URI rejection writes no forced dump files")

local cmdf_lua_missing_input = os.tmpname()
local cmdf_lua_missing_regular_path = os.tmpname()
local cmdf_lua_missing_italic_path = os.tmpname()
local cmdf_lua_missing_regular = assert(io.open(cmdf_lua_missing_regular_path, "wb"))
cmdf_lua_missing_regular:write("preserve regular")
cmdf_lua_missing_regular:close()
local cmdf_lua_missing_italic = assert(io.open(cmdf_lua_missing_italic_path, "wb"))
cmdf_lua_missing_italic:write("preserve italic")
cmdf_lua_missing_italic:close()
os.remove(cmdf_lua_missing_input)
local cmdf_lua_missing_input_error = run_command_expect_fail(
  shell_quote(cmdf_lua) .. " --ansi on --html --html-dump-font-force --html-dump-font-regular-path " ..
  shell_quote(cmdf_lua_missing_regular_path) .. " --html-dump-font-italic-path " ..
  shell_quote(cmdf_lua_missing_italic_path) .. " " .. shell_quote(cmdf_lua_missing_input))
assert(cmdf_lua_missing_input_error:match("No such file") or cmdf_lua_missing_input_error:match("cannot open"),
  "cmdf.lua reports an unreadable explicit input")
local cmdf_lua_missing_regular_check = assert(io.open(cmdf_lua_missing_regular_path, "rb"))
assert_equal(cmdf_lua_missing_regular_check:read("*a"), "preserve regular",
  "cmdf.lua does not force-replace regular fonts when input cannot open")
cmdf_lua_missing_regular_check:close()
local cmdf_lua_missing_italic_check = assert(io.open(cmdf_lua_missing_italic_path, "rb"))
assert_equal(cmdf_lua_missing_italic_check:read("*a"), "preserve italic",
  "cmdf.lua does not force-replace italic fonts when input cannot open")
cmdf_lua_missing_italic_check:close()
os.remove(cmdf_lua_missing_regular_path)
os.remove(cmdf_lua_missing_italic_path)

local cmdf_html_width = run_capture(shell_quote(cmdf) .. " --ansi on --html -w 42", sample)
local cmdf_lua_html_width = run_capture(shell_quote(cmdf_lua) .. " --ansi on --html -w 42", sample)
assert_equal(cmdf_lua_html_width, cmdf_html_width, "cmdf.lua html width parity")

local cmdf_html_after_deck = run_capture(shell_quote(cmdf) .. " --ansi on --deck --html -T 'Lua Mixed'", deck_sample)
local cmdf_lua_html_after_deck = run_capture(shell_quote(cmdf_lua) .. " --ansi on --deck --html -T 'Lua Mixed'", deck_sample)
assert_equal(cmdf_lua_html_after_deck, cmdf_html_after_deck, "cmdf.lua deck then html format flag parity")

local cmdf_lua_simulated = run_capture(shell_quote(cmdf_lua) .. " --ansi on -b --simulate-chunk 1", sample)
assert_equal(cmdf_lua_simulated, cmdf_ansi, "cmdf.lua simulate chunk ansi parity")

local cmdf_lua_simulated_short = run_capture(shell_quote(cmdf_lua) .. " --ansi on -b -S 1", sample)
assert_equal(cmdf_lua_simulated_short, cmdf_ansi, "cmdf.lua -S simulate chunk ansi parity")

local cmdf_lua_simulated_delay = run_capture(shell_quote(cmdf_lua) .. " --ansi on -b --simulate-delay 1ms", sample)
assert_equal(cmdf_lua_simulated_delay, cmdf_ansi, "cmdf.lua simulate delay ansi parity")

for _, delay in ipairs({ ".001s", "1e-3s", "+1e-3s" }) do
  local cmdf_delay = run_capture(shell_quote(cmdf) .. " --ansi on -b --simulate-delay " .. delay, sample)
  local cmdf_lua_delay = run_capture(shell_quote(cmdf_lua) .. " --ansi on -b --simulate-delay " .. delay, sample)
  assert_equal(cmdf_lua_delay, cmdf_delay, "cmdf.lua simulate delay duration syntax parity: " .. delay)
end

local cmdf_deck = run_capture(shell_quote(cmdf) .. " --ansi on --deck --slide-numbers -x cross -T 'Lua Deck'", deck_sample)
local cmdf_lua_deck = run_capture(shell_quote(cmdf_lua) .. " --ansi on --deck --slide-numbers -x cross -T 'Lua Deck'", deck_sample)
assert(cmdf_lua_deck:match("JetBrains Mono"), "cmdf.lua deck embeds JetBrains Mono")
assert_equal(cmdf_lua_deck, cmdf_deck, "cmdf.lua deck parity")

local cmdf_deck_after_html = run_capture(shell_quote(cmdf) .. " --ansi on --html --deck -T 'Lua Mixed'", deck_sample)
local cmdf_lua_deck_after_html = run_capture(shell_quote(cmdf_lua) .. " --ansi on --html --deck -T 'Lua Mixed'", deck_sample)
assert_equal(cmdf_lua_deck_after_html, cmdf_deck_after_html, "cmdf.lua html then deck format flag parity")

local cmdf_deck_width = run_capture(shell_quote(cmdf) .. " --ansi on --deck -w 42 -T 'Lua Deck'", deck_sample)
local cmdf_lua_deck_width = run_capture(shell_quote(cmdf_lua) .. " --ansi on --deck -w 42 -T 'Lua Deck'", deck_sample)
assert_equal(cmdf_lua_deck_width, cmdf_deck_width, "cmdf.lua deck width parity")

local invalid_deck_transition = run_expect_fail(shell_quote(cmdf_lua) .. " --ansi on --deck -x spin", deck_sample)
assert(invalid_deck_transition:match("invalid deck transition"), invalid_deck_transition)

local invalid_simulate_chunk = run_expect_fail(shell_quote(cmdf_lua) .. " --ansi on --simulate-chunk 0", sample)
assert(invalid_simulate_chunk:match("invalid simulate chunk"), invalid_simulate_chunk)

local invalid_simulate_delay = run_expect_fail(shell_quote(cmdf_lua) .. " --ansi on --simulate-delay 0.5", sample)
assert(invalid_simulate_delay:match("invalid simulate delay"), invalid_simulate_delay)

local deck_option_without_deck = run_expect_fail(shell_quote(cmdf_lua) .. " --ansi on --slide-numbers", deck_sample)
assert(deck_option_without_deck:find("deck options require --deck", 1, true), deck_option_without_deck)

-- Unknown sinks default to plain UTF-8; terminal policy remains independent of boring.
do
  local text = "# The Outcome-Based Agile Framework\n\nµ界 with [link](https://example.org)\n"
  local plain = mdf.render(text)
  assert(not plain:find("\27", 1, true), "unknown Lua sink defaults to no escapes")
  assert(plain:find("µ界", 1, true), "plain Lua output preserves UTF-8")
  assert_equal(plain, mdf.render(text, { ansi_mode = "off", osc8 = true }), "OFF overrides OSC8")
  local file = assert(io.open(cmdf_lua, "rb"))
  assert_equal(plain, mdf.render(text, { output_fd = mdf.file_descriptor(file) }),
               "regular-file descriptor keeps AUTO plain")
  file:close()
  assert(mdf.render(text, { ansi_mode = "on" }):find("\27", 1, true), "ON enables styling for string output")
  local writes, traces = {}, {}
  local stream = mdf.document_stream({ ansi_mode = "off", osc8 = true,
    write_trace = function(_, chunk) traces[#traces + 1] = chunk end,
  }, function(chunk) writes[#writes + 1] = chunk end)
  for i = 1, #text do stream:write(text:sub(i, i)); stream:flush() end
  stream:finish_document()
  assert_equal(table.concat(writes), plain, "plain one-byte Lua stream matches string output")
  assert(#writes == #traces, "plain Lua sink and trace counts match")
  for i = 1, #writes do assert_equal(writes[i], traces[i], "plain Lua trace describes exact sink write") end
  local count = #writes
  stream:reset()
  stream:close()
  assert(#writes == count, "plain Lua reset and close emit no terminal cleanup")
  assert(not pcall(function() mdf.new({ ansi_mode = "invalid" }) end), "invalid ANSI policy rejected")
  assert(not pcall(function() mdf.new({ output_fd = "not-an-fd" }) end), "invalid output fd rejected")
  local dirty = "\27[31m# The Outcome-Based Agile Framework\27[0m\n\n" ..
                "µ界\27]8;;https://hidden.example\7 with\27]8;;\7 [link](https://example.org)\n"
  for _, opts in ipairs({ {}, { ansi_mode = "off", osc8 = true }, { boring = true, ansi_mode = "off" } }) do
    local expected = mdf.render(text, opts)
    assert_equal(mdf.render(dirty, opts), expected, "Lua string input escapes stripped before parsing")
    assert_equal(collect_stream(dirty, opts, 1), expected, "Lua callback filter survives one-byte boundaries")
    local h = mdf.new(opts)
    assert_equal(h:render(dirty), expected, "Lua handle string uses the same plain policy")
    local chunks = {}
    h:set_sink(function(chunk) chunks[#chunks + 1] = chunk end)
    local pos = 1
    h:render_stream(function(cap)
      if pos > #dirty then return nil end
      local chunk = dirty:sub(pos, pos + math.min(cap, 1) - 1)
      pos = pos + #chunk
      return chunk
    end)
    assert_equal(table.concat(chunks), expected, "Lua handle streaming uses the same plain policy")
    h:close()
  end
  for _, format in ipairs({ "html", "deck" }) do
    local opts = { format = format, disable_embedded_font = true }
    local expected = mdf.render(text, opts)
    opts.ansi_mode = "off"
    assert_equal(mdf.render(text, opts), expected, "Lua ANSI policy leaves " .. format .. " unchanged")
  end
  assert_equal(run_capture(shell_quote(cmdf), text), plain, "native CLI defaults to plain output")
  assert_equal(run_capture(shell_quote(cmdf_lua), text), plain, "Lua CLI defaults to plain output")
  assert_equal(run_capture(shell_quote(cmdf_lua) .. " --ascii --osc8 on", text), plain, "Lua CLI ASCII overrides OSC8")
end
