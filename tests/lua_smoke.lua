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
local parity_opts = { boring = true, osc8 = mdf.detect_osc8_support() }

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

local incremental_chunks = {}
local incremental = mdf.document_stream({ format = "ansi", boring = true }, function(chunk)
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
  local stream = mdf.document_stream({ format = "ansi", boring = true, width = 80 }, function(chunk)
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
  local first = {}
  local second = {}
  local stream = mdf.document_stream({ format = "ansi", boring = true, osc8 = true }, function(chunk)
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
  local stream = mdf.document_stream({ format = "ansi", boring = true }, sink)
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
  local stream = mdf.document_stream({ format = "ansi", boring = true }, function()
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
  local stream = mdf.document_stream({ format = "ansi", boring = true }, function()
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
  local markdown = "# Split Lua\n\n- item\n\n`code` [link](https://example.com)\n"
  local chunks = {}
  local split = mdf.document_stream({ format = "ansi", boring = true }, function(chunk)
    chunks[#chunks + 1] = chunk
  end)
  for i = 1, #markdown do
    assert(split:write(markdown:sub(i, i)), "lua document stream accepts byte-sized fragments")
  end
  assert(split:finish_document(), "lua document stream finishes byte-sized fragments")
  assert_equal(table.concat(chunks), mdf.render(markdown, { boring = true }),
               "lua document stream byte split parity")
  split = nil
  collectgarbage("collect")
end

local callback_failure = mdf.document_stream({ format = "ansi", boring = true }, function()
  return false
end)
local callback_ok = pcall(function() callback_failure:write("callback failure") end)
assert(not callback_ok, "lua document stream surfaces callback failure")
assert_equal(callback_failure:error(), "sink write failed",
             "lua document stream exposes its core error after callback failure")
callback_failure:close()

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
  local reset_handle = mdf.new({ format = "ansi", boring = true })
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
      boring = true,
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
      boring = true,
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
  local nested_handle = mdf.new({ format = "ansi", boring = true })
  local nested_once = false
  local nested_sink
  nested_sink = function(chunk)
    nested_chunks[#nested_chunks + 1] = chunk
    if not nested_once then
      local nested_co

      nested_once = true
      nested_co = coroutine.create(function()
        local reset_ok = pcall(function() nested_handle:reset() end)

        assert(not reset_ok)
        assert(nested_handle:set_sink(nested_sink))
        assert(nested_handle:set_width(80))
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
  local source_handle = mdf.new({ format = "ansi", boring = true })
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
  close_stream = mdf.document_stream({ format = "ansi", boring = true }, function(chunk)
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
local token_handle = mdf.new({ boring = true })
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
  local receiver = mdf.new({ format = "ansi", boring = true })
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

local cmdf_lua_traced, cmdf_lua_trace = run_capture_with_trace(shell_quote(cmdf_lua) .. " -b", sample)
assert_equal(cmdf_lua_traced, cmdf_ansi, "cmdf.lua traced ansi parity")
local trace_lines = 0
for _ in cmdf_lua_trace:gmatch("[^\n]+") do trace_lines = trace_lines + 1 end
assert(trace_lines > 3, "cmdf.lua traced render emits decision chunks")

local cmdf_html = run_capture(shell_quote(cmdf) .. " --html", sample)
local cmdf_lua_html = run_capture(shell_quote(cmdf_lua) .. " --html", sample)
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
  shell_quote(cmdf_lua) .. " --html --html-dump-font -o " .. shell_quote(cmdf_lua_bare_output_path), sample)
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
  "cd " .. shell_quote(cmdf_lua_relative_dump_dir) .. " && " .. shell_quote(cmdf_lua) ..
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
  "cd " .. shell_quote(cmdf_lua_stdout_dump_dir) .. " && " .. shell_quote(cmdf_lua) ..
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
  shell_quote(cmdf_lua) .. " --html --html-dump-font-regular-path " ..
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
  shell_quote(cmdf_lua) .. " --html --html-dump-font-regular-path " ..
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
  shell_quote(cmdf_lua) .. " --html --html-dump-font-regular-path " ..
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
  shell_quote(cmdf_lua) .. " --html --html-dump-font-force --html-dump-font-regular-path /dev/stdout" ..
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
  shell_quote(cmdf_lua) .. " --html --html-font-uri '?v=1' --html-dump-font-force " ..
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
  shell_quote(cmdf_lua) .. " --html --html-dump-font-force --html-dump-font-regular-path " ..
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
