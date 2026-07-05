#!/usr/bin/env sh
set -eu

emit_case() {
  name=$1
  input=$2
  shift 2
  printf '%s %s' "$name" "$input"
  for arg do
    printf ' %s' "$arg"
  done
  printf '\n'
}

emit_ansi_grid() {
  fixture=$1
  input=$2
  widths='20 40 80 120'
  profiles='0:0 2:0 0:1 4:6 20:20'

  for style in styled boring; do
    for width in $widths; do
      for profile in $profiles; do
        left=${profile%:*}
        right=${profile#*:}
        if [ $((width - left - right)) -lt 15 ]; then
          continue
        fi
        case "$style" in
          styled)
            emit_case "$fixture.ansi.$style.w$width.margin-l$left-r$right" "$input" \
              --ansi -w "$width" --margin-left "$left" --margin-right "$right"
            ;;
          boring)
            emit_case "$fixture.ansi.$style.w$width.margin-l$left-r$right" "$input" \
              --ansi --boring -w "$width" --margin-left "$left" --margin-right "$right"
            ;;
        esac
      done
    done
  done

}

emit_html_deck() {
  fixture=$1
  input=$2

  emit_case "$fixture.html.default" "$input" --html
  emit_case "$fixture.html.w42" "$input" --html -w 42
  emit_case "$fixture.deck.default" "$input" --deck
  emit_case "$fixture.deck.w42" "$input" --deck -w 42
  emit_case "$fixture.deck.slide-numbers.fade" "$input" --deck --slide-numbers -x fade
  emit_case "$fixture.deck.boring" "$input" --deck --boring
}

emit_fixture() {
  fixture=$1
  input=$2

  emit_ansi_grid "$fixture" "$input"
  emit_html_deck "$fixture" "$input"
}

printf '%s\n' '# name input args...'
emit_fixture comprehensive testdata/deck-corpus/comprehensive.md
emit_fixture mixed-layout tests/golden-fixtures/mixed-layout.md
emit_fixture container-charts tests/golden-fixtures/container-charts.md
