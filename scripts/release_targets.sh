#!/usr/bin/env bash
# The declared shipment matrix. Validate selectors before starting producers.
read -r -a release_targets <<< "${LIBMDF_TARGETS:-x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin}"
[[ ${#release_targets[@]} -gt 0 ]] || { printf 'No release targets selected\n' >&2; exit 2; }
for target in "${release_targets[@]}"; do
  case "$target" in
    x86_64-linux-gnu|x86_64-linux-musl|aarch64-linux-gnu|aarch64-linux-musl|armhf-linux-gnu|armhf-linux-musl|arm64-apple-darwin) ;;
    *) printf 'Unknown release target: %s\n' "$target" >&2; exit 2;;
  esac
done
[[ $(printf '%s\n' "${release_targets[@]}" | sort -u | wc -l) -eq ${#release_targets[@]} ]] || {
  printf 'Release targets must be unique\n' >&2; exit 2;
}
