#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 OUTPUT_DIRECTORY WORK_DIRECTORY" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$1
work=$2
archive="$root/ports/lua/source/lua-5.4.7.tar.gz"
sdk="$root/build/sdk"
cc=${PHIPIA_SDK_CC:-clang}
ld=${PHIPIA_SDK_LD:-ld.lld}

case "$output" in /*) ;; *) output="$root/$output" ;; esac
case "$work" in /*) ;; *) work="$root/$work" ;; esac

case "$output:$work" in
    "$root"/*:"$root"/*) ;;
    *) echo "Lua output and work directories must be inside the repository" >&2; exit 2 ;;
esac

(cd "$root/ports/lua/source" && sha256sum -c SHA256SUMS)
rm -rf -- "$output" "$work"
mkdir -p -- "$output" "$work/source" "$work/obj"
tar -xzf "$archive" -C "$work/source"

cflags=(
    --target=x86_64-unknown-none-elf
    -I"$sdk/include"
    -I"$root/include"
    -I"$work/source/lua-5.4.7/src"
    -std=c11 -Os -ffreestanding -fno-pie -fno-stack-protector
    -mcmodel=large -mno-red-zone -fno-builtin
    -ffunction-sections -fdata-sections -ftls-model=local-exec
    -DLUA_USE_C89 -DLUA_USE_JUMPTABLE=0
    -Wall -Wextra -Werror -Wpedantic
)

objects=()
for source in "$work/source/lua-5.4.7/src"/l*.c; do
    if [ "$(basename -- "$source")" = "luac.c" ]; then
        continue
    fi
    object="$work/obj/$(basename -- "${source%.c}").o"
    "$cc" "${cflags[@]}" -c "$source" -o "$object"
    objects+=("$object")
done
performance="$work/obj/phipia-performance.o"
"$cc" "${cflags[@]}" -c "$root/ports/lua/performance.c" -o "$performance"
objects+=("$performance")

"$ld" -nostdlib -static --gc-sections --build-id=none \
    -z max-page-size=0x1000 -z noexecstack --fatal-warnings \
    --orphan-handling=error --wrap=luaL_openlibs -T "$sdk/linker.ld" \
    -Map="$output/LUA.map" -o "$output/LUA.APP" \
    "$sdk/lib/crt0.o" "${objects[@]}" "$sdk/lib/libphipia.a"

echo "Lua 5.4.7 Phipia executable: $output/LUA.APP"
