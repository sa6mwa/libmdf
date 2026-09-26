#!/usr/bin/env python3
"""Exercise destination-aware defaults through every public Lua render workflow."""
import errno
import fcntl
import os
import pty
import struct
import subprocess
import sys
import termios

lua = sys.argv[1]
program = r'''
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
'''
env = dict(os.environ, COLUMNS="200", TERM_PROGRAM="WezTerm")


def render(workflow, policy="auto", destination="known", width=0, terminal=False):
    args = [lua, "-e", program, "--", "/dev/null", workflow, policy, destination, str(width)]
    if not terminal:
        result = subprocess.run(args, input=b"", capture_output=True, env=env, timeout=10)
        assert result.returncode == 0, result.stderr
        return result.stdout
    master, slave = pty.openpty()
    try:
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        result = subprocess.run(args, input=b"", stdout=slave, stderr=subprocess.PIPE, env=env, timeout=10)
        assert result.returncode == 0, result.stderr
        os.close(slave)
        slave = -1
        chunks = []
        while True:
            try:
                chunk = os.read(master, 4096)
            except OSError as exc:
                if exc.errno == errno.EIO:
                    break
                raise
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks).replace(b"\r\n", b"\n")
    finally:
        os.close(master)
        if slave >= 0:
            os.close(slave)


for workflow in ("render", "stream", "document", "handle_render", "handle_stream", "feed", "tokens"):
    plain = render(workflow)
    assert b"\x1b" not in plain and "µ界".encode() in plain, workflow
    # UTF-8 byte length differs from visible columns; the longest line is ASCII here.
    assert 60 < max(map(len, plain.splitlines())) <= 80, workflow
    assert plain == render(workflow, destination="unknown", terminal=True), workflow
    assert plain == render(workflow, "off"), workflow
    assert b"\x1b" in render(workflow, "on"), workflow
    assert b"\x1b" in render(workflow, terminal=True), workflow
    terminal_plain = render(workflow, "off", terminal=True)
    assert b"\x1b" not in terminal_plain, workflow
    assert 80 < max(map(len, terminal_plain.splitlines())) <= 100, workflow
    narrow = render(workflow, "off", width=30, terminal=True)
    assert max(map(len, narrow.splitlines())) <= 30, workflow
print("Lua zero-ANSI policy, PTY detection, width, UTF-8, and exact sink/trace checks passed for seven workflows")
