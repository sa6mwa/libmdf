#!/usr/bin/env bash
# Private Bootlin runtime for AFL++ and the compiler backends loading its plugins.
# Sets cpkt_afl_runtime_{loader,library_path,link_options,cc,cxx} for the builder.
cpkt_afl_owned_file() {
  local root=$1 path
  path=$(readlink -f -- "$2") || return 1
  [[ -f "$path" && "$path" == "$root/"* ]]
}

cpkt_afl_resolve_runtime() {
  local cc=$1 cxx=$2 sysroot=$3 collection=$4
  local library backend compiler name directory resolved boundary
  collection=$(CDPATH= cd -- "$collection" && pwd -P) || return "$?"
  sysroot=$(CDPATH= cd -- "$sysroot" && pwd -P) || return "$?"
  [[ "$sysroot" == "$collection/"* ]] || die 'AFL++ requires the selected Bootlin sysroot'
  for compiler in "$cc" "$cxx"; do
    [[ -x "$compiler" ]] && cpkt_afl_owned_file "$collection" "$compiler" || die 'AFL++ requires the selected Bootlin compiler'
  done
  cpkt_afl_runtime_loader="$sysroot/lib/ld-linux-x86-64.so.2"
  [[ -x "$cpkt_afl_runtime_loader" ]] && cpkt_afl_owned_file "$sysroot" "$cpkt_afl_runtime_loader" ||
    die "Bootlin runtime loader is missing or outside the selected sysroot: $cpkt_afl_runtime_loader"
  library=$("$cxx" -print-file-name=libstdc++.so.6) || return "$?"
  library=$(readlink -f -- "$library") || return "$?"
  [[ -f "$library" && "$library" == "$collection/"* ]] || die 'Bootlin C++ runtime is missing or outside the selected collection'
  for directory in "$sysroot/lib" "$sysroot/usr/lib" "$(dirname -- "$library")" "$collection/lib"; do
    boundary=$collection
    [[ "$directory" != "$sysroot/"* ]] || boundary=$sysroot
    resolved=$(CDPATH= cd -- "$directory" && pwd -P) || die "Missing Bootlin runtime directory: $directory"
    [[ "$resolved" == "$boundary" || "$resolved" == "$boundary/"* ]] ||
      die "Bootlin runtime directory is outside the selected collection/sysroot: $directory"
  done
  cpkt_afl_runtime_library_path="$sysroot/lib:$sysroot/usr/lib:$(dirname -- "$library"):$collection/lib"
  cpkt_afl_runtime_link_options=(
    "-Wl,--dynamic-linker,$cpkt_afl_runtime_loader"
    -Wl,--disable-new-dtags
    "-Wl,-rpath,$cpkt_afl_runtime_library_path")
  for name in cc1 cc1plus; do
    compiler=$cc
    [[ "$name" != cc1plus ]] || compiler=$cxx
    backend=$("$compiler" -print-prog-name="$name") || return "$?"
    [[ -x "$backend" ]] && cpkt_afl_owned_file "$collection" "$backend" || die "Bootlin compiler backend is missing or outside the selected collection: $backend"
    printf -v "cpkt_afl_runtime_$name" '%s' "$backend"
  done
}

cpkt_afl_prepare_runtime() {
  local stage=$1 cc=$2 cxx=$3 sysroot=$4 collection=$5
  local backend compiler name backend_dir
  cpkt_afl_resolve_runtime "$cc" "$cxx" "$sysroot" "$collection" || return "$?"
  backend_dir="$stage/libexec/bootlin-runtime"
  mkdir -p "$stage/bin" "$backend_dir"
  for name in cc1 cc1plus; do
    backend=$cpkt_afl_runtime_cc1
    [[ "$name" != cc1plus ]] || backend=$cpkt_afl_runtime_cc1plus
    printf '#!/usr/bin/env bash\nset -euo pipefail\nexec %q --library-path %q %q "$@"\n' \
      "$cpkt_afl_runtime_loader" "$cpkt_afl_runtime_library_path" "$backend" > "$backend_dir/$name"
    chmod +x "$backend_dir/$name"
  done
  for name in gcc g++; do
    compiler=$cc
    [[ "$name" != g++ ]] || compiler=$cxx
    {
      printf '#!/usr/bin/env bash\nset -euo pipefail\n'
      # Resolve relative to the wrapper so the verified staging tree can move.
      printf 'backend_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../libexec/bootlin-runtime" && pwd -P)\n'
      printf 'exec %q --library-path %q %q -B "$backend_dir/" "$@"\n' \
        "$cpkt_afl_runtime_loader" "$cpkt_afl_runtime_library_path" "$compiler"
    } > "$stage/bin/bootlin-$name"
    chmod +x "$stage/bin/bootlin-$name"
  done
  cpkt_afl_runtime_cc="$stage/bin/bootlin-gcc"
  cpkt_afl_runtime_cxx="$stage/bin/bootlin-g++"
}
