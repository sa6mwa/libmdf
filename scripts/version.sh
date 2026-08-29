#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RESERVED_TEST_TAG=v99.99.99

if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1 &&
   [ "$(CDPATH= cd -- "$(git -C "$ROOT" rev-parse --show-toplevel)" && pwd)" = "$ROOT" ]; then
  if git -C "$ROOT" show-ref --verify --quiet "refs/tags/$RESERVED_TEST_TAG"; then
    printf '%s\n' "$RESERVED_TEST_TAG is reserved for lifecycle testing and cannot be a release tag" >&2
    exit 1
  fi
  tag=$(
    git -C "$ROOT" tag --points-at HEAD --list 'v[0-9]*.[0-9]*.[0-9]*' |
      while IFS= read -r candidate; do
        case "$candidate" in
          v[0-9]*.[0-9]*.[0-9]*)
            version=${candidate#v}
            if printf '%s\n' "$version" | grep -Eq '^[0-9]+[.][0-9]+[.][0-9]+$' &&
               [ "$(git -C "$ROOT" cat-file -t "refs/tags/$candidate" 2>/dev/null || true)" = "commit" ]; then
              printf '%s\n' "$version"
            fi
            ;;
        esac
      done |
      awk -F. '
        NF == 3 {
          major = $1 + 0
          minor = $2 + 0
          patch = $3 + 0
          if (!seen ||
              major > best_major ||
              (major == best_major && minor > best_minor) ||
              (major == best_major && minor == best_minor && patch > best_patch)) {
            seen = 1
            best = $0
            best_major = major
            best_minor = minor
            best_patch = patch
          }
        }
        END {
          if (seen) {
            print best
          }
        }'
  )
  if [ -n "$tag" ]; then
    printf '%s\n' "$tag"
    exit 0
  fi
  printf '%s\n' '0.0.0'
  exit 0
fi

if [ -f "$ROOT/VERSION" ]; then
  version=$(sed -n '1p' "$ROOT/VERSION" | tr -d '[:space:]')
  case "$version" in
    [0-9]*.[0-9]*.[0-9]*)
      if printf '%s\n' "$version" | grep -Eq '^[0-9]+[.][0-9]+[.][0-9]+$'; then
        printf '%s\n' "$version"
        exit 0
      fi
      ;;
  esac
fi

printf '%s\n' '0.0.0'
