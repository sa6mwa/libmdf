#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK="$ROOT/build/test-version-source"
REPO="$WORK/repo"
UNTAGGED="$WORK/untagged"
EXTRACT="$WORK/extract"

fail() {
  printf '%s\n' "FAIL: $1" >&2
  exit 1
}

rm -rf "$WORK"
mkdir -p "$REPO/scripts" "$UNTAGGED/scripts" "$EXTRACT"

cp "$ROOT/scripts/version.sh" "$REPO/scripts/version.sh"
cp "$ROOT/scripts/package_source.sh" "$REPO/scripts/package_source.sh"
git -C "$REPO" init -q
git -C "$REPO" config user.email test@example.invalid
git -C "$REPO" config user.name "libmdf test"
git -C "$REPO" add scripts/version.sh scripts/package_source.sh
git -C "$REPO" commit -q -m initial
git -C "$REPO" tag v2.3.4

test "$(sh "$REPO/scripts/version.sh")" = "2.3.4" || fail "exact v tag must resolve release version"

git -C "$REPO" tag -a v9.9.9 -m annotated
test "$(sh "$REPO/scripts/version.sh")" = "2.3.4" ||
  fail "annotated v tag must not satisfy lightweight release version contract"
sh "$REPO/scripts/package_source.sh" >/dev/null
test -f "$REPO/dist/libmdf-2.3.4.tar.gz" || fail "source archive was not produced from tag version"
test "$(tar -xOzf "$REPO/dist/libmdf-2.3.4.tar.gz" libmdf-2.3.4/VERSION)" = "2.3.4" ||
  fail "source archive VERSION was not inserted from tag version"

tar -C "$EXTRACT" -xzf "$REPO/dist/libmdf-2.3.4.tar.gz"
test "$(sh "$EXTRACT/libmdf-2.3.4/scripts/version.sh")" = "2.3.4" ||
  fail "source archive checkout must resolve version from VERSION"

NOSORT="$WORK/no-sort-bin"
mkdir -p "$NOSORT"
cat >"$NOSORT/sort" <<'EOF'
#!/usr/bin/env sh
printf '%s\n' 'sort must not be required by version.sh' >&2
exit 99
EOF
chmod +x "$NOSORT/sort"
git -C "$REPO" tag v2.9.9
git -C "$REPO" tag v2.10.0
test "$(PATH="$NOSORT:$PATH" sh "$REPO/scripts/version.sh")" = "2.10.0" ||
  fail "multiple tags on HEAD must resolve highest semver without GNU sort"
git -C "$REPO" tag v99.99.99
if sh "$REPO/scripts/version.sh" >/dev/null 2>&1; then
  fail "reserved lifecycle test tag must reject release version resolution"
fi

cp "$ROOT/scripts/version.sh" "$UNTAGGED/scripts/version.sh"
git -C "$UNTAGGED" init -q
git -C "$UNTAGGED" config user.email test@example.invalid
git -C "$UNTAGGED" config user.name "libmdf test"
git -C "$UNTAGGED" add scripts/version.sh
git -C "$UNTAGGED" commit -q -m initial
test "$(sh "$UNTAGGED/scripts/version.sh")" = "0.0.0" ||
  fail "untagged git checkout without VERSION must resolve development version"

printf '%s\n' '7.8.9' > "$UNTAGGED/VERSION"
test "$(sh "$UNTAGGED/scripts/version.sh")" = "0.0.0" ||
  fail "untagged git checkout must ignore untracked VERSION"
