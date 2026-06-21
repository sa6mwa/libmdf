#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-"$ROOT/dist/libmdf-$(sh "$ROOT/scripts/version.sh")-1.rockspec"}
VERSION=${2:-$(sh "$ROOT/scripts/version.sh")}
SOURCE_URL=${3:-"file://libmdf-lua-$VERSION.tar.gz"}
SOURCE_DIR=${4:-"libmdf-lua-$VERSION"}

mkdir -p "$(dirname "$OUT")"
awk \
  -v version="$VERSION" \
  -v source_url="$SOURCE_URL" \
  -v source_dir="$SOURCE_DIR" '
    /^version = / {
      print "version = \"" version "-1\""
      next
    }
    /^source = / {
      print "source = { url = \"" source_url "\", dir = \"" source_dir "\" }"
      next
    }
    {
      gsub(/@VERSION@/, version)
      print
    }
  ' "$ROOT/libmdf-scm-1.rockspec.in" > "$OUT"
