#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

awk '
  /^int mdf_emit_all\(mdf_impl \*impl, mdf_sink \*sink, const char \*src, size_t len\)$/ {
    in_func = 1
    depth = 0
    body = ""
  }
  in_func {
    line = $0
    body = body line "\n"
    opens = gsub(/\{/, "{", line)
    closes = gsub(/\}/, "}", line)
    depth += opens - closes
    if (depth == 0 && body ~ /\{/) {
      in_func = 0
    }
  }
  END {
    if (body == "") {
      print "mdf_emit_all not found" > "/dev/stderr"
      exit 1
    }
    if (body ~ /src[ \t]*\[[^]]+\]/) {
      print "mdf_emit_all must not inspect source emission bytes" > "/dev/stderr"
      exit 1
    }
    if (body ~ /emit_buf[ \t]*\[[^]]+\]/) {
      print "mdf_emit_all must not inspect buffered emission bytes" > "/dev/stderr"
      exit 1
    }
    if (body ~ /\\033|0x1[bB]/) {
      print "mdf_emit_all must not interpret ANSI escapes" > "/dev/stderr"
      exit 1
    }
  }
' "$ROOT/src/mdf.c"
