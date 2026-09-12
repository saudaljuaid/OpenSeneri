<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Upstream source and license record

Required ports build offline from the exact archives below. Checksums are
verified before extraction and the corresponding license text is retained next
to the port.

| Component | Official source | Pinned input | SHA-256 | License record |
| --- | --- | --- | --- | --- |
| Lua | `https://www.lua.org/ftp/lua-5.4.7.tar.gz` | Lua 5.4.7 release archive | `9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30` | `ports/lua/LICENSE` (MIT) |
| SQLite | `https://www.sqlite.org/2024/sqlite-amalgamation-3460000.zip` | SQLite 3.46.0 amalgamation | `712a7d09d2a22652fb06a49af516e051979a3984adb067da86760e60ed51a7f5` | `ports/sqlite/LICENSE` (public-domain dedication) |

`ports/*/source/SHA256SUMS` is consumed by the build, not merely documentary.
Phipia-specific VFS, test application, manifests, and build scripts are
GPL-3.0-only. Upstream notices remain unmodified.
