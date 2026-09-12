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
archive="$root/ports/sqlite/source/sqlite-amalgamation-3460000.zip"
sdk="$root/build/sdk"
cc=${PHIPIA_SDK_CC:-clang}
ld=${PHIPIA_SDK_LD:-ld.lld}
python=${PYTHON:-python3}

case "$output" in /*) ;; *) output="$root/$output" ;; esac
case "$work" in /*) ;; *) work="$root/$work" ;; esac
case "$output:$work" in
    "$root"/*:"$root"/*) ;;
    *) echo "SQLite output and work directories must be inside the repository" >&2; exit 2 ;;
esac

(cd "$root/ports/sqlite/source" && sha256sum -c SHA256SUMS)
rm -rf -- "$output" "$work"
mkdir -p -- "$output" "$work/source" "$work/obj"
"$python" -m zipfile -e "$archive" "$work/source"
source_dir="$work/source/sqlite-amalgamation-3460000"

cflags=(
    --target=x86_64-unknown-none-elf
    -I"$sdk/include" -I"$root/include" -I"$source_dir"
    -std=c11 -Os -ffreestanding -fno-pie -fno-stack-protector
    -mcmodel=large -mno-red-zone -fno-builtin
    -ffunction-sections -fdata-sections -ftls-model=local-exec
    -Wall -Wextra -Werror -Wpedantic
)
defines=(
    -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL
    -DSQLITE_TEMP_STORE=3 -DSQLITE_DQS=0
    -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED
    -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_UTF16
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_MMAP_SIZE=0
    -DSQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=16384
)

"$cc" "${cflags[@]}" "${defines[@]}" \
    -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
    -Wno-type-limits -c "$source_dir/sqlite3.c" -o "$work/obj/sqlite3.o"
"$cc" "${cflags[@]}" -c "$root/ports/sqlite/phipia_vfs.c" \
    -o "$work/obj/phipia_vfs.o"
"$cc" "${cflags[@]}" -c "$root/ports/sqlite/main.c" \
    -o "$work/obj/main.o"

"$ld" -nostdlib -static --gc-sections --build-id=none \
    -z max-page-size=0x1000 -z noexecstack --fatal-warnings \
    --orphan-handling=error -T "$sdk/linker.ld" \
    -Map="$output/SQLITE.map" -o "$output/SQLITE.APP" \
    "$sdk/lib/crt0.o" "$work/obj/sqlite3.o" \
    "$work/obj/phipia_vfs.o" "$work/obj/main.o" "$sdk/lib/libphipia.a"

echo "SQLite 3.46.0 Phipia executable: $output/SQLITE.APP"
