#!/usr/bin/env bash
set -euo pipefail
cmdf=$1
root=$2
scratch=$(mktemp -d "$3/c89-cli.XXXXXX")
printf '# C89 test\n\nText with [DOI](https://doi.org/10.1016/0048-7333(86)90027-2).\n\n## Next\n\n| Name | Value |\n| --- | --- |\n| Table | Cell |\n' > "$scratch/input.md"
mkdir "$scratch/page" "$scratch/assets #1"

"$cmdf" --html --c89 "$scratch/input.md" > "$scratch/embedded.html"
sed -n 's/.*background-image:url("data:image\/svg+xml;base64,\([^"]*\)").*/\1/p' "$scratch/embedded.html" | base64 --decode > "$scratch/mark.svg"
sed -n 's/.*class="banner-brand".*<img src="data:image\/svg+xml;base64,\([^"]*\)".*/\1/p' "$scratch/embedded.html" | base64 --decode > "$scratch/logo.svg"
cmp "$root/assets/c89/c89-mark.svg" "$scratch/mark.svg"
cmp "$root/assets/c89/c89-systems.svg" "$scratch/logo.svg"
grep -Eq 'data:font/woff2;base64,' "$scratch/embedded.html"
grep -Eq '</script>' "$scratch/embedded.html"

"$cmdf" -o "$scratch/page/test.html" --c89 --html-dump-font "$scratch/input.md"
cmp "$root/assets/c89/c89-mark.svg" "$scratch/page/c89-mark.svg"
cmp "$root/assets/c89/c89-systems.svg" "$scratch/page/c89-systems.svg"
test -s "$scratch/page/JetBrainsMono-Regular.woff2"
test -s "$scratch/page/JetBrainsMono-Italic.woff2"
grep -Fq 'background-image:url("./c89-mark.svg")' "$scratch/page/test.html"
grep -Fq 'src="./c89-systems.svg"' "$scratch/page/test.html"
! grep -Eq 'data:(font/|image/svg)' "$scratch/page/test.html"

printf 'preserved' > "$scratch/page/c89-mark.svg"
"$cmdf" --html --c89 --html-dump-font -o "$scratch/page/test.html" "$scratch/input.md"
test "$(cat "$scratch/page/c89-mark.svg")" = preserved
"$cmdf" --html --c89 --html-dump-font-force -o "$scratch/page/test.html" "$scratch/input.md"
cmp "$root/assets/c89/c89-mark.svg" "$scratch/page/c89-mark.svg"

"$cmdf" --html --c89 --html-dump-font-path "$scratch/assets #1" -o "$scratch/page/test.html" "$scratch/input.md"
cmp "$root/assets/c89/c89-systems.svg" "$scratch/assets #1/c89-systems.svg"
grep -Fq '../assets%20%231/c89-mark.svg' "$scratch/page/test.html"
grep -Fq '../assets%20%231/c89-systems.svg' "$scratch/page/test.html"

# Every user-owned destination is protected before renderer creation/dumping.
for collision in input output stdout; do
  printf 'preserved' > "$scratch/page/c89-mark.svg"
  args=(--html --c89 --html-dump-font-force --html-dump-font-path "$scratch/page")
  case $collision in
    input) args+=("$scratch/page/c89-mark.svg");;
    output) args+=(-o "$scratch/page/c89-mark.svg" "$scratch/input.md");;
    stdout) args+=("$scratch/input.md");;
  esac
  if [[ $collision == stdout ]]; then
    if "$cmdf" "${args[@]}" >> "$scratch/page/c89-mark.svg" 2> "$scratch/error"; then exit 1; fi
  elif "$cmdf" "${args[@]}" > "$scratch/rejected.html" 2> "$scratch/error"; then exit 1; fi
  test "$(cat "$scratch/page/c89-mark.svg")" = preserved
  grep -Eq 'aliases input, output, or stdout' "$scratch/error"
done
for format in --ascii --deck; do
  if "$cmdf" "$format" --c89 "$scratch/input.md" > "$scratch/rejected.html" 2> "$scratch/error"; then exit 1; fi
  grep -Eq -- '--c89 requires HTML output' "$scratch/error"
done

# A font/SVG alias must fail before any dump changes the existing bytes.
printf 'preserved' > "$scratch/page/c89-mark.svg"
if "$cmdf" --html --c89 --html-dump-font-force --html-dump-font-path "$scratch/page" \
  --html-dump-font-regular-path "$scratch/page/c89-mark.svg" "$scratch/input.md" \
  > "$scratch/rejected.html" 2> "$scratch/error"; then exit 1; fi
test "$(cat "$scratch/page/c89-mark.svg")" = preserved

# SVG write failure must leave an existing HTML output intact.
mkdir "$scratch/page/c89-systems.svg.blocked"
mv "$scratch/page/c89-systems.svg" "$scratch/page/saved-logo.svg"
ln -s c89-systems.svg.blocked "$scratch/page/c89-systems.svg"
printf 'preserved output' > "$scratch/page/test.html"
if "$cmdf" --html --c89 --html-dump-font-force -o "$scratch/page/test.html" "$scratch/input.md" \
  > "$scratch/rejected.html" 2> "$scratch/error"; then exit 1; fi
test "$(cat "$scratch/page/test.html")" = 'preserved output'

"$cmdf" --html --c89 --html-disable-embedded-font "$scratch/input.md" > "$scratch/upstream.html"
grep -Fq 'https://c89.systems/assets/logos/c89-systems.svg' "$scratch/upstream.html"
! grep -Eq 'data:(font/|image/svg)' "$scratch/upstream.html"
"$cmdf" --html "$scratch/input.md" > "$scratch/plain.html"
! grep -Eq 'C89 Systems|c89-mark|lights-out' "$scratch/plain.html"
