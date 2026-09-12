<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Reproducible BusyBox proof inputs

BusyBox and musl are separate userspace works. Neither is copied into or linked
with Phipia's GPL-3.0-only kernel. Their source, configurations, licenses,
traces, and build records remain distinct release materials.

## Pinned source inputs

| Input | SHA-256 |
| --- | --- |
| BusyBox 1.38.0 source archive | `34F9EA6FF8636F2C9241153B9114EEFA9E65674A45318AE1EF95BB5F31C53BB2` |
| musl 1.2.6 source archive | `D585FD3B613C66151FC3249E8ED44F77020CB5E6C1E635A616D3F9F82460512A` |
| `userspace/busybox/busybox.config` | `3FBC0403C6A4865FC4397240961C367EE9B36D6D350CC6CEB2D22CBBBEA28480` |
| `userspace/busybox/busybox-uname.config` | `6D972C7A1F3DF0034D5996CC24B58B7364EFBB7851F926C5D8D2FD18C41EBB2B` |
| `userspace/busybox/busybox-cat.config` | `ACC38083863385286FF2BB2D8D594E6DF629CCAE2D84BEB8B838AFED8D7CE669` |
| BusyBox license from the archive | `BBFC9843646D483C334664F651C208B9839626891D8F17604DB2146962F43548` |
| musl copyright record from the archive | `B870108EC5E7790E9F9919064F1B9421D62D5F9B0E6C230C6ADF7EA2DA62E97B` |

The source archives are committed under `userspace/busybox/source/`. CI uses
Ubuntu 24.04, GCC 13.3, binutils 2.42, and a musl 1.2.6 `musl-gcc` wrapper.

## Reproduction

```sh
bash tools/build-busybox-proof.sh build/busybox-contract build/busybox-work
bash tools/build-busybox-uname-proof.sh \
    build/busybox-uname-contract build/busybox-uname-work
bash tools/build-busybox-cat-proof.sh \
    build/busybox-cat-contract build/busybox-cat-work
```

Each script performs two clean source/toolchain builds and requires
byte-identical results. It rejects changed inputs, configurations, output
hashes, unexpected ELF shape, runtime relocations, dynamic dependencies, W+X,
and exercised MMX/SSE/AVX instructions.

The images are static non-PIE `ET_EXEC` files at fixed high user
addresses. The build selects musl's `crt1.o`, disables linker relaxation, uses
the large code model, and omits unused constructor bookends. The uname build
adds a build-only scalar target attribute to musl `vfprintf`; the published
source archives remain byte-identical.

## Frozen results

| Profile | Executable | Size | Executable SHA-256 | FAT16 fixture SHA-256 |
| --- | --- | ---: | --- | --- |
| v0.8.0 | `echo PHIPIA` | 33,584 | `B308F2CAD5B5CD0EEB92A622DEC8D71C1A08F628A22CDC5BCDE2B98B53220746` | `79EE482967A1979C34DCFC87B68813C5DA79B27292362DDA890839B6263FF821` |
| v0.9.0 | `uname -s` | 38,368 | `389AD6B13804EB7307BA589C8E8A7C702F91302005A7C5FC6E9E99124FCEAF43` | `CDB8E920F06AC93F63E73854FC5A6A63CDBCC7DCEDBBFB62325C7EC4B408AD36` |
| v1.1.0 | `cat` | 38,632 | `8191596A22778B575942895071A2E50CCEEE0F82F4D88B6D986584CE0914FC3E` | composed v1.1.0 volume |

All three executables have five program headers, four load segments, no interpreter,
dynamic section, runtime relocation, PIE, shared object, or RWX segment. Their
exact syscall traces and allowlists are committed beside the configurations.

Version 1.1.0 places all three exact executables in one 16 MiB read-only FAT16
volume. `tools/make-phipia-proof-userland.py` rebuilds every byte, independently
verifies each file and root entry, runs negative mutations, and pins the volume
SHA-256 to
`C2A2B2FEC703C654E1260EF07A91FF1DD7808F8D83734C0D7AFD3967525B34B9`.
The same builder preserves the v1.0.0 echo/uname-only image byte-for-byte as the
v1.1.0 missing-cat negative fixture with SHA-256
`F7DB823EE1CB7FF2A05E7020DB0F4502656B9950EFBBE79E23ED0EA755FC8478`.

Version 2.0.0 places those same executable bytes in the deterministic immutable
FAT32 system image. `tools/fat32_image.py` verifies each filename, size, digest,
and complete allocation chain independently. The system image SHA-256 is
`A88A44BE394AEFB6D5B7729A6378F4D180E214D60EB6035B3425C6C724936F04`.
The historical FAT16 images and checksums above remain release contracts.

## Release requirement

A release containing any of these executables must also provide the exact
BusyBox and musl source archives, configurations, BusyBox license, musl
copyright record, build scripts, checksum manifest, volume builder, and
profile-specific syscall evidence. The v1.1.0 bundle additionally carries the
deterministic FAT16 image, foreground lifecycle and input/output contracts,
production positive and missing-cat QEMU transcripts, robustness results,
screenshot, video, and verification summary generated from the release commit.
A binary is not published unless those records match.

The v2.0.0 bundle additionally carries both deterministic FAT32 images,
geometry and identity records, consistency reports, clean-reboot persistence
evidence, and the same executable sources, configurations, licenses, and
instruction audits.
