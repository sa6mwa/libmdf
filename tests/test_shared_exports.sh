#!/usr/bin/env sh
set -eu

nm_tool=$1
library=$2
expected='mdf_begin_document
mdf_create
mdf_detect_osc8_support
mdf_dump_html_jetbrains_mono_font
mdf_dump_html_jetbrains_mono_font_force
mdf_dump_html_jetbrains_mono_font_to_paths
mdf_dump_html_jetbrains_mono_font_to_paths_force
mdf_feed
mdf_finish
mdf_finish_document
mdf_flush
mdf_html_font_resolve_dump_paths
mdf_html_jetbrains_mono_font
mdf_options_init
mdf_pager_file
mdf_pager_source
mdf_paths_alias
mdf_render
mdf_reset
mdf_set_geometry
mdf_set_html_title
mdf_set_sink
mdf_set_width
mdf_status_string
mdf_terminal_width
mdf_theme_count
mdf_theme_exists
mdf_theme_name
mdf_write_token'

actual=$($nm_tool -D --defined-only "$library" | awk '{print $3}' |
  sed -e 's/@@.*$//' -e '/^LIBMDF_[0-9][0-9_]*$/d' -e '/^$/d' | sort)
if [ "$actual" != "$expected" ]; then
  printf '%s\n' 'libmdf shared export allowlist mismatch' >&2
  printf '%s\n' 'expected:' >&2
  printf '%s\n' "$expected" >&2
  printf '%s\n' 'actual:' >&2
  printf '%s\n' "$actual" >&2
  exit 1
fi
