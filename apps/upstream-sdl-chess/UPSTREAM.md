# SDL Chess Board source

`upstream.c` is the byte-exact `test/testdrawchessboard.c` from SDL's official
`release-2.32.10` tag at commit
`5d249570393f7a37e037abf22cd6012a4cc56a71`.

- Source: <https://github.com/libsdl-org/SDL/blob/release-2.32.10/test/testdrawchessboard.c>
- Git blob: `db9da41c9dea7eb3e296d0f95abf8d9db3b3afcd`
- SHA-256: `1a5e7ee6511a7d6942a4a7c51e15f8629ee1e5b8eeff101409c4dc65f056a63a`
- License: zlib, retained verbatim in the source header and in
  `vendor/sdl2/LICENSE.txt`

`main.c` includes the unchanged source and supplies a bounded platform proof:
it runs the upstream event/render loop for eight frames, persists an exact
receipt through SDL's preference-filesystem API, synchronizes Data, and exits
so the QEMU scenario can verify resource reclamation and the retained disk.
