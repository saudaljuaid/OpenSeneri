# zlib upstream provenance

- Release: zlib 1.3.2 (2026-02-17)
- Official release page: https://zlib.net/
- Official archive: https://zlib.net/zlib-1.3.2.tar.gz
- Archive SHA-256: `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`
- Official repository: https://github.com/madler/zlib.git
- Tag: `v1.3.2`
- Annotated tag object: `216c70c020aa53f0c40920d155f808b6b59c9acb`
- Dereferenced commit: `da607da739fa6047df13e66a2af6b8bec7c2a498`
- License: zlib License (`LICENSE`)

The archive was downloaded from the official release URL and accepted only
after its SHA-256 matched the digest published on the official release page.
`SOURCE-MANIFEST.sha256` records the retained files after extraction. The
retained upstream files are byte-for-byte copies; only their placement into
`include/` and `src/` differs from the archive root.

Retained paths are the public headers, the upstream `--solo` core source set,
the private/generated headers needed to compile that set, the license, and the
upstream README. Excluded paths are gzip/stdio adapters (`gz*.c`, `gzguts.h`),
convenience buffer wrappers (`compress.c`, `uncompr.c`), examples, tests,
contrib/minizip code, platform projects, documentation, and upstream build
systems. These exclusions are a distribution-size decision, not source edits.
