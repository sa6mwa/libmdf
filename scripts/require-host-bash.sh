#!/usr/bin/env bash
# Host prerequisite for this lifecycle helper collection, not the C89 SDK ABI.
# Keep this check parseable by macOS /bin/bash 3.2 and use shell builtins only.
cpkt_host_bash_supported() {
  [[ ${1:-} =~ ^[0-9]+$ && ${2:-} =~ ^[0-9]+$ ]] || return 1
  (( 10#$1 > 4 || (10#$1 == 4 && 10#$2 >= 4) ))
}

if ! cpkt_host_bash_supported "${BASH_VERSINFO[0]}" "${BASH_VERSINFO[1]}"; then
  printf 'cpkt: host Bash >= 4.4 is required by the lifecycle helpers; running %s (%s). On macOS run brew install bash and export PATH="$(brew --prefix bash)/bin:$PATH"; on Linux install/upgrade the host bash package. Then rerun with PATH-selected bash (do not invoke /bin/bash on macOS).\n' "$BASH_VERSION" "$BASH" >&2
  exit 2
fi

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  printf 'cpkt host Bash: %s (%s)\n' "$BASH_VERSION" "$BASH"
fi
