#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 OUTPUT-DIRECTORY" >&2
    exit 2
fi

root=$(cd "$(dirname "$0")/.." && pwd)
output=$1
cc=${PHIPIA_SDK_CC:-clang}
ld=${PHIPIA_SDK_LD:-ld.lld}
python=${PYTHON:-python3}
readelf=${READELF:-llvm-readelf}

for tool in "$cc" "$ld" "$python" "$readelf"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "native dynamic proof requires $tool" >&2
        exit 1
    fi
done

mkdir -p "$output"
common=(--target=x86_64-unknown-none-elf -I"$root/sdk/include" \
    -I"$root/include" -I"$root/apps/native-dynamic" -std=c11 -O2 -g \
    -ffreestanding -fno-stack-protector -mno-red-zone -fno-builtin \
    -fPIC -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -Wpedantic -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes)

"$cc" "${common[@]}" -ftls-model=initial-exec -c \
    "$root/apps/native-dynamic/library.c" -o "$output/library.o"
"$cc" "${common[@]}" -fPIE -ftls-model=local-exec \
    -c "$root/apps/native-dynamic/root.c" \
    -o "$output/root.o"
"$cc" --target=x86_64-unknown-none-elf -ffreestanding -fPIE \
    -mno-red-zone -c "$root/apps/native-dynamic/start.S" \
    -o "$output/start.o"

link_common=(-nostdlib --gc-sections --build-id=none -z max-page-size=0x1000 \
    -z noexecstack -z now -z relro --fatal-warnings --hash-style=gnu)
"$ld" "${link_common[@]}" -shared -soname DYNLIB.SO \
    -o "$output/DYNLIB.SO" "$output/library.o"
"$ld" "${link_common[@]}" -pie --no-dynamic-linker --export-dynamic \
    -e _start -o "$output/DYNROOT.APP" "$output/start.o" \
    "$output/root.o" --no-as-needed "$output/DYNLIB.SO"

"$readelf" -h -l -d -r "$output/DYNROOT.APP" >"$output/DYNROOT.readelf"
"$readelf" -h -l -d -r "$output/DYNLIB.SO" >"$output/DYNLIB.readelf"
grep -Eq 'Type:[[:space:]]+DYN' "$output/DYNROOT.readelf"
grep -Eq 'NEEDED.*\[DYNLIB.SO\]' "$output/DYNROOT.readelf"
grep -Eq 'SONAME.*\[DYNLIB.SO\]' "$output/DYNLIB.readelf"
if grep -Eq 'INTERP|TEXTREL' "$output/DYNROOT.readelf" \
        "$output/DYNLIB.readelf"; then
    echo "dynamic proof unexpectedly gained PT_INTERP or text relocations" >&2
    exit 1
fi

"$python" "$root/tools/make-native-dynamic-proof.py" \
    --spec "$root/apps/native-dynamic/manifest.json" \
    --root "$output/DYNROOT.APP" --library "$output/DYNLIB.SO" \
    --output "$output"
