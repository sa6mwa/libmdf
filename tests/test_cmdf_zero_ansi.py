#!/usr/bin/env python3
"""Prove output policy against pipes, files, and a real terminal destination."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import struct
import subprocess
import sys
import tempfile
import termios

cmdf = sys.argv[1]
lua_cli = "--lua" in sys.argv[2:]
env = dict(os.environ, COLUMNS="200", TERM_PROGRAM="WezTerm")
source = b"# The Outcome-Based Agile Framework\n\n" + b"alpha beta gamma delta " * 7 + b"\n\n[Link](https://example.org)\n"


def run(args=(), terminal=False, data=source):
    if not terminal:
        result = subprocess.run([cmdf, *args], input=data, capture_output=True, env=env, timeout=10)
        assert result.returncode == 0, result.stderr
        return result.stdout
    master, slave = pty.openpty()
    try:
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        result = subprocess.run([cmdf, *args], input=data, stdout=slave, stderr=subprocess.PIPE, env=env, timeout=10)
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


plain = run()
assert b"\x1b" not in plain and max(map(len, plain.splitlines())) <= 80
assert plain == run(["--ascii", "--osc8", "on"])
if not lua_cli:
    assert plain == run(["--incremental", "--simulate-chunk", "1", "--osc8", "on"])
else:
    assert plain == run(["--simulate-chunk", "1", "--osc8", "on"])
assert b"\x1b" not in run(["--ascii", "--boring", "--osc8", "on"])
assert b"\x1b" in run(["--ansi", "on", "--osc8", "on"])
assert b"\x1b]8;;" in run(["--ansi", "on", "--boring", "--osc8", "on"])
assert b"\x1b" in run(terminal=True)
assert run(["--output", "/dev/stdout"], terminal=True) == run(terminal=True)
terminal_plain = run(["--ascii", "--osc8", "on"], terminal=True)
assert b"\x1b" not in terminal_plain
assert 80 < max(map(len, terminal_plain.splitlines())) <= 100
assert max(map(len, run(["--width", "30"]).splitlines())) <= 30

root = Path(__file__).resolve().parents[1]
(root / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="zero-ansi-", dir=root / "build") as scratch:
    output = Path(scratch) / "output.txt"
    run(["--output", str(output)], terminal=True)
    assert output.read_bytes() == plain, "named file follows its own destination rather than stdout"
    run(["--output", str(output), "--width", "30"])
    assert max(map(len, output.read_bytes().splitlines())) <= 30
    run(["--output", str(output), "--ansi", "on"])
    assert b"\x1b" in output.read_bytes()
    original = b"existing contents must survive invalid options\n"
    for args in (["--width", "10", "--margin-left", "10"], ["--theme", "missing-theme"]):
        output.write_bytes(original)
        invalid = subprocess.run([cmdf, "--output", str(output), *args],
                                 input=source, capture_output=True, env=env, timeout=10)
        assert invalid.returncode != 0
        assert output.read_bytes() == original, "renderer validation must precede output truncation"

for mode in ("--html", "--deck"):
    args = [mode, "--html-disable-embedded-font"]
    assert run(args) == run([*args, "--ascii"]), "ANSI policy must leave HTML/deck unchanged"

invalid = subprocess.run([cmdf, "--ansi", "invalid"], input=source, capture_output=True, timeout=10)
assert invalid.returncode != 0 and b"auto|on|off" in invalid.stderr
if not lua_cli:
    assert invalid.returncode == 2, "native CLI rejects invalid policy as a usage error"
print("cmdf zero-ANSI pipe, file, terminal, width, boring, OSC8, incremental, HTML/deck checks passed")
