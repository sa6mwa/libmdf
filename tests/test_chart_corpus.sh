#!/bin/sh
set -eu

CMDF=${1:?usage: test_chart_corpus.sh /path/to/cmdf [corpus-dir]}
CORPUS=${2:-testdata/chart-corpus}
TMP=${TMPDIR:-/tmp}/libmdf-chart-corpus-$$
CORPUS_ROOT=$(dirname "$CORPUS")
CORPUS_NAME=$(basename "$CORPUS")

cleanup() {
  rm -rf "$TMP"
}
trap cleanup EXIT INT HUP TERM

mkdir -p "$TMP"

count=0
for src in "$CORPUS"/*.md; do
  name=$(basename "$src" .md)
  if [ "$name" = "README" ]; then
    continue
  fi
  ansi="$TMP/$name.ansi"
  narrow="$TMP/$name.narrow"
  html="$TMP/$name.html"
  html_boring="$TMP/$name.boring.html"
  ansi80="$TMP/$name.ansi80"
  ansi80_margin="$TMP/$name.ansi80-margin"
  ansi40_margin="$TMP/$name.ansi40-margin"
  expected_ansi80="$CORPUS_ROOT/${CORPUS_NAME}__$name.w80boring.golden"
  expected_ansi80_margin="$CORPUS_ROOT/${CORPUS_NAME}__$name.w80m20boring.golden"
  expected_ansi40_margin="$CORPUS_ROOT/${CORPUS_NAME}__$name.w40m8boring.golden"
  variant_base="$TMP/$name.variant"

  "$CMDF" --boring --width 96 "$src" > "$ansi"
  "$CMDF" --boring --width 32 "$src" > "$narrow"
  "$CMDF" --boring --width 80 "$src" > "$ansi80"
  "$CMDF" --html "$src" > "$html"
  "$CMDF" --html --boring "$src" > "$html_boring"

  test -s "$ansi"
  test -s "$narrow"
  test -s "$ansi80"
  test -s "$html"
  test -s "$html_boring"
  if [ ! -f "$expected_ansi80" ]; then
    echo "missing boring width-80 ANSI golden for $src" >&2
    exit 1
  fi
  diff -u "$expected_ansi80" "$ansi80"
  if [ -f "$expected_ansi80_margin" ]; then
    "$CMDF" --boring --width 80 --margin-left 20 --margin-right 20 "$src" > "$ansi80_margin"
    test -s "$ansi80_margin"
    diff -u "$expected_ansi80_margin" "$ansi80_margin"
  fi
  if [ -f "$expected_ansi40_margin" ]; then
    "$CMDF" --boring --width 40 --margin-left 8 --margin-right 8 "$src" > "$ansi40_margin"
    test -s "$ansi40_margin"
    diff -u "$expected_ansi40_margin" "$ansi40_margin"
  fi
  grep -q '<!doctype html>' "$html"
  grep -q '<!doctype html>' "$html_boring"
  grep -q 'mdf-chart-block' "$html"
  grep -q 'mdf-chart-block' "$html_boring"
  if grep -q 'background-color:rgb(205,0,205)\|background-color:rgb(59,156,255)' "$html_boring"; then
    echo "boring HTML chart output preserved themed chart colors for $src" >&2
    exit 1
  fi
  for width in 40 60 80 96; do
    for margins in none wide; do
      normal="$variant_base.$width.$margins.normal"
      simulated="$variant_base.$width.$margins.simulated"
      if [ "$margins" = wide ]; then
        if [ "$width" -gt 40 ]; then
          left=20
          right=20
        else
          left=8
          right=8
        fi
        "$CMDF" -t everforest --width "$width" --margin-left "$left" --margin-right "$right" "$src" > "$normal"
        "$CMDF" -t everforest --width "$width" --margin-left "$left" --margin-right "$right" --simulate-chunk 3 "$src" > "$simulated"
      else
        "$CMDF" -t everforest --width "$width" "$src" > "$normal"
        "$CMDF" -t everforest --width "$width" --simulate-chunk 3 "$src" > "$simulated"
      fi
      test -s "$normal"
      test -s "$simulated"
      cmp "$normal" "$simulated"
      if grep -q 'key,value\|> >' "$normal" "$simulated"; then
        echo "chart corpus variant leaked chart data or accumulated quote prefixes for $src width=$width margins=$margins" >&2
        exit 1
      fi
    done
  done
  case "$name" in
	    08-container-contexts)
	      quoted_list_margin="$TMP/$name.quoted-list-margin"
	      "$CMDF" --boring --width 60 --margin-left 20 --margin-right 20 --simulate-chunk 3 "$src" > "$quoted_list_margin"
	      if grep -q 'mdf-\(bar\|vertical-bar\|tile\)-chart' "$ansi" "$narrow" "$html" "$html_boring"; then
	        echo "container chart fence leaked as prose for $src" >&2
	        exit 1
	      fi
      if perl -0ne '
          $bad = 0;
          while (/Blockquote (?:horizontal|vertical|tile) chart\.<\/span>(.*?)(?=<span style="color:rgb\(0,0,0\);">(?:Unordered-list|Blockquoted-list)|<\/main>)/sg) {
            $bad = 1 if $1 =~ /<span[^>]*>&gt;<\/span>\n<div class="mdf-chart-block"/;
          }
          END { exit $bad ? 1 : 0 }
        ' "$html_boring"; then
        :
      else
        echo "html blockquote chart output emitted standalone quote marker before chart for $src" >&2
        exit 1
      fi
      if [ "$(perl -0ne '$count = () = /">&gt;<\/span>\n<div class="mdf-chart-block"/g; END { print $count }' "$html_boring")" != "3" ]; then
        echo "html blockquoted-list chart output did not preserve exactly one source blank quote line before each chart for $src" >&2
        exit 1
      fi
      if grep -q '> >\|chart\.-\|chart\.1\.\|chart\.>' "$ansi" "$narrow"; then
        echo "container chart output has accumulated quote prefixes or fused block text for $src" >&2
        exit 1
      fi
	      grep -q '^> .*Quote Build' "$ansi"
	      grep -q '^> .*Quoted List Build' "$ansi"
	      grep -q '^ *List Build' "$ansi"
	      grep -q '^ *Ordered Build' "$ansi"
	      grep -q '^                    >$' "$quoted_list_margin"
	      ;;
    09-indented-backtick-labels)
      grep -q '```' "$ansi"
      grep -q '```' "$narrow"
      grep -q '```' "$html"
      grep -q 'After Backticks' "$ansi"
      grep -q 'After Backticks' "$narrow"
      grep -q 'After Backticks' "$html"
      grep -q 'After Backticks' "$html_boring"
      ;;
    10-block-boundaries)
      if grep -q 'Paragraph before .*Boundary\|^ After' "$ansi" "$narrow"; then
        echo "paragraph-adjacent chart output fused surrounding text for $src" >&2
        exit 1
      fi
      grep -q '^After horizontal\.' "$ansi"
      grep -q '^After vertical\.' "$ansi"
      grep -q '^After tile\.' "$ansi"
      ;;
    11-container-adjacent-charts)
      checked_files="$ansi $narrow $ansi80"
      if [ -s "$ansi80_margin" ]; then
        checked_files="$checked_files $ansi80_margin"
      fi
      if [ -s "$ansi40_margin" ]; then
        checked_files="$checked_files $ansi40_margin"
      fi
      for checked in $checked_files; do
        awk '
          function strip_margin(line) {
            sub(/^ */, "", line);
            return line;
          }
          function is_quote_blank(line) {
            return strip_margin(line) == ">";
          }
          function is_blank(line) {
            return line ~ /^ *$/;
          }
          function is_chart_line(line) {
            return line ~ /(│|┤|└|█|■)/;
          }
          {
            lines[NR] = $0;
          }
          END {
            seen = 0;
            for (i = 1; i <= NR; i++) {
              line = strip_margin(lines[i]);
              if (line ~ /^> before /) {
                seen++;
                blank = line ~ /^> before blank /;
                if (blank) {
                  if (!is_quote_blank(lines[i + 1])) exit 11;
                  if (is_quote_blank(lines[i + 2])) exit 12;
                  if (!is_chart_line(lines[i + 2])) exit 13;
                } else {
                  if (is_quote_blank(lines[i + 1])) exit 14;
                  if (!is_chart_line(lines[i + 1])) exit 15;
                }
              } else if (line ~ /^- before /) {
                seen++;
                blank = line ~ /^- before blank /;
                if (blank) {
                  if (!is_blank(lines[i + 1])) exit 21;
                  if (is_blank(lines[i + 2])) exit 22;
                  if (!is_chart_line(lines[i + 2])) exit 23;
                } else {
                  if (is_blank(lines[i + 1])) exit 24;
                  if (!is_chart_line(lines[i + 1])) exit 25;
                }
              }
            }
            if (seen != 12) {
              exit 30;
            }
          }
        ' "$checked" || {
          echo "container-adjacent chart output violates exact boundary invariants in $checked" >&2
          exit 1
        }
      done
      ;;
  esac
  count=$((count + 1))
done

if [ "$count" -lt 10 ]; then
  echo "expected at least 10 chart corpus files, found $count" >&2
  exit 1
fi
