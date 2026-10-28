local mdf = require('libmdf')

local function fails_with(expected, fn)
  local ok, err = pcall(fn)
  assert(not ok and err == expected, tostring(err))
end

local function reader(text)
  local once = false
  return function()
    if once then return nil end
    once = true
    return text
  end
end

-- Preserve the original error object, after native cleanup, for every adapter.
for _ = 1, 3 do
  local marker = {}
  fails_with(marker, function()
    mdf.render_stream(function() error(marker) end, function() end)
  end)
  fails_with(marker, function()
    mdf.render_stream(reader('word\n'), function() error(marker) end)
  end)
  fails_with(marker, function()
    mdf.render('word\n', {write_trace=function() error(marker) end})
  end)
  for _, document in ipairs({false, true}) do
    local h = document and mdf.document_stream({}, function() error(marker) end) or mdf.new({})
    if not document then h:set_sink(function() error(marker) end) end
    fails_with(marker, function()
      if document then h:write('word\n\n') else h:feed('word\n\n') end
    end)
    h:close()
    h:close()
  end
end

-- Validation and OOM must not leave registry roots retaining callback graphs.
local weak = setmetatable({}, {__mode='v'})
for i = 1, 20 do
  local marker = {}
  weak[i] = marker
  local trace = function() return marker ~= nil end
  assert(not pcall(mdf.render, 'word', {write_trace=trace, font_uri={}}))
  assert(not pcall(mdf.render_stream, reader('word'), function() end,
    {write_trace=trace, font_uri={}}))
  assert(not pcall(mdf.new, {write_trace=trace, font_uri={}}))
end
collectgarbage('collect')
collectgarbage('collect')
assert(next(weak) == nil, 'failed construction retained callbacks')

-- Native sink replacement commits even when cleanup on the old writer fails.
for _, document in ipairs({false, true}) do
  local marker, armed, output = {}, false, {}
  local function old_writer() if armed then error(marker) end end
  local h = document and mdf.document_stream({ansi_mode='on'}, old_writer)
    or mdf.new({ansi_mode='on'})
  if not document then h:set_sink(old_writer) end
  armed = true
  fails_with(marker, function()
    h:set_sink(function(chunk) output[#output+1] = chunk end)
  end)
  if document then h:write('healthy\n') else h:feed('healthy\n') end
  h:finish_document()
  assert(table.concat(output):find('healthy',1,true))
  h:close()
end

-- A handle pins the selected font bytes even if the caller mutates its table.
do
  local options = {format='html', html_font={family='OwnedFont', regular_format='woff2',
    regular_data=string.rep('fontbytes', 1000)}}
  local expected = mdf.render('hello', options)
  local handle = mdf.new(options)
  options.html_font.regular_data = nil
  options.html_font.family = 'Changed'
  options.html_font = nil
  collectgarbage('collect')
  local garbage = string.rep('overwrite', 5000)
  assert(handle:render('hello') == expected, 'font storage outlived its Lua owner')
  assert(#garbage > 0)
  handle:close()
end

-- Compare actual writer calls and trace events from the same streaming render.
for _, mode in ipairs({'full', 'row'}) do
  local writes, traces = {}, {}
  mdf.render_stream(reader('# The Outcome-Based Agile Framework\n\n| a | b |\n|---|---|\n| x | y |\n'),
    function(chunk) writes[#writes+1] = chunk end,
    {table_buffer_mode=mode, write_trace=function(format, chunk)
      assert(format == 'ansi')
      traces[#traces+1] = chunk
    end})
  assert(#writes == #traces)
  for i, chunk in ipairs(writes) do
    assert(chunk == traces[i])
    assert(not chunk:find('The Outcome-Based Agile Framework', 1, true))
  end
  local words = {}
  for _, chunk in ipairs(writes) do words[chunk] = true end
  for _, word in ipairs({'The', 'Outcome-Based', 'Agile', 'Framework'}) do
    assert(words[word], 'missing decision: '..word)
  end
end

-- Legal reader geometry changes survive; lifecycle changes cannot free active state.
do
  local handle = mdf.new({})
  handle:set_sink(function() end)
  handle:render_stream(function()
    assert(handle:set_geometry(40, 1, 1))
    assert(not pcall(handle.close, handle))
    return nil
  end)
  handle:close()
end

-- C strings reject truncation/coercion; length-aware font data remains binary.
assert(not pcall(mdf.render, 'hello\0hidden'))
assert(not pcall(mdf.new, {format='html\0ansi'}))
assert(not pcall(mdf.new, {theme=12}))
assert(not pcall(mdf.new, {format='html', html_font={regular_format='woff2',regular_data=12}}))
local binary = mdf.render('hello', {format='html',html_font={family='BinaryFont',
  regular_format='woff2',regular_data='a\0b'}})
assert(binary:find('YQBi', 1, true))

-- Getter-produced token bytes stay rooted while another getter runs GC.
do
  local h = mdf.new({width=1200})
  local out = {}
  h:set_sink(function(chunk) out[#out+1] = chunk end)
  h:write_token(setmetatable({}, {__index=function(_, key)
    if key == 'type' then return 'text' end
    if key == 'text' then return string.rep('x',1000) end
    if key == 'level' then collectgarbage('collect'); return 0 end
  end}))
  h:finish()
  assert(table.concat(out):find(string.rep('x',1000), 1, true))
  h:close()
end
do
  local h = mdf.new({})
  h:set_sink(function() end)
  local ok, err = pcall(h.write_token, h, setmetatable({}, {__index=function(_, key)
    h:close()
    if key == 'type' then return 'text' end
    if key == 'text' then return 'closed' end
  end}))
  assert(not ok and err:find('handle is closed', 1, true))
end

-- Copying the materialized result can collect a finalizer that closes its owner.
do
  local h = mdf.new({format='html'})
  local during_render, attempted = false, false
  local finalizer_during_render, close_ok, close_error
  collectgarbage('collect')
  local old_mode = collectgarbage('incremental')
  local pause = collectgarbage('param', 'pause', 0)
  local stepmul = collectgarbage('param', 'stepmul', 100000)
  local stepsize = collectgarbage('param', 'stepsize', 0)
  collectgarbage('stop')
  setmetatable({}, {__gc=function()
    attempted = true
    finalizer_during_render = during_render
    close_ok, close_error = pcall(h.close, h)
  end})
  during_render = true
  collectgarbage('restart')
  local output = h:render('hello')
  during_render = false
  assert(attempted, 'result copy did not exercise the finalizer')
  -- Assert outside __gc: Lua converts finalizer exceptions into warnings.
  assert(finalizer_during_render, 'finalizer must run inside the result copy')
  assert(close_ok == false and close_error:find('active callback', 1, true))
  assert(output:find('hello', 1, true))
  assert(h:render('still open'):find('still open', 1, true))
  h:close()
  collectgarbage('param', 'pause', pause)
  collectgarbage('param', 'stepmul', stepmul)
  collectgarbage('param', 'stepsize', stepsize)
  collectgarbage(old_mode)
end
print('Lua ownership and callback contracts passed')
