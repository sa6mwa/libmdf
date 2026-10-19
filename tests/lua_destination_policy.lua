local mdf = require("libmdf")
local workflow, policy, destination, width = arg[1], arg[2], arg[3], tonumber(arg[4])
local opts = { ansi_mode = policy, width = width, osc8 = true }
if destination == "known" then opts.output_fd = mdf.file_descriptor(io.stdout) end
local text = "µ界 " .. string.rep("alpha beta gamma delta ", 7) .. "\n"
local dirty = "\27[31m" .. text .. "\27[0m"
local writes, traces = {}, {}
local function write(chunk)
  writes[#writes + 1] = chunk
  assert(io.stdout:write(chunk))
end
opts.write_trace = function(_, chunk) traces[#traces + 1] = chunk end
local pos = 1
local function read(cap)
  if pos > #dirty then return nil end
  local chunk = dirty:sub(pos, pos + math.min(cap, 1) - 1)
  pos = pos + #chunk
  return chunk
end
if workflow == "render" then
  io.write(mdf.render(dirty, opts))
elseif workflow == "stream" then
  mdf.render_stream(read, write, opts)
elseif workflow == "document" then
  local s = mdf.document_stream(opts, write)
  for i = 1, #dirty do s:write(dirty:sub(i, i)); s:flush() end
  s:finish_document()
  s:close()
else
  local h = mdf.new(opts)
  if workflow == "handle_render" then
    io.write(h:render(dirty))
  else
    h:set_sink(write)
    if workflow == "handle_stream" then h:render_stream(read)
    elseif workflow == "feed" then
      for i = 1, #dirty do h:feed(dirty:sub(i, i)); h:flush() end
      h:finish_document()
    elseif workflow == "tokens" then
      h:write_token({type = "text", text = dirty})
      h:finish()
    else error("unknown workflow") end
  end
  h:close()
end
if #writes > 0 then
  assert(#writes == #traces, "sink/trace count mismatch")
  for i = 1, #writes do assert(writes[i] == traces[i], "sink/trace byte mismatch") end
end
