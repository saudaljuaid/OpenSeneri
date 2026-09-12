<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Browser port assessment

Phipia does not ship a browser engine. NetSurf's framebuffer frontend is the
selected port candidate because it has its own layout engine, a small portable
surface, and no dependency on a host GUI toolkit. No NetSurf source is vendored
or included in the compatibility claims.

Any import must pin and audit one upstream revision and its complete dependency
closure.

## Port boundary

1. **Freestanding build.** Build the NetSurf core, framebuffer frontend,
   LibCSS, Hubbub/LibDOM, and admitted image libraries as a static native
   application.
2. **Frontend surface.** Use a bounded Phipia window, damage rectangles, and
   native keyboard and pointer events through the public ABI.
3. **URL fetcher.** Implement NetSurf callbacks over DNS and native streams,
   including deadlines, cancellation, reset, and partial I/O.
4. **Resources and cache.** Keep application resources on read-only System and
   a bounded synchronized cache under the application Data root.
5. **TLS.** Apply the browser release criteria in `TLS_EVALUATION.md`; the
   bounded SDK trust-store profile is not a browser origin policy.
6. **Isolation.** Run fetched content in a private process generation with
   explicit quotas and teardown.

## Required services

The port requires a browser-scale allocation policy, bounded asynchronous work,
Unicode text and scalable fonts, certificate-store maintenance, content
encoding, cookies, cache policy, origin-scoped storage, watchdogs, quotas, and
update handling. Hosted libc sockets, signals, dynamic loading, and general file
descriptors are not part of the Phipia native ABI.

## Acceptance

Each stage needs host parser tests, a QEMU scenario, matching resource censuses,
malformed-input controls, and framebuffer evidence. The acceptance sequence is:

1. an offline static page and image;
2. input, scrolling, navigation, and bounded history;
3. deterministic HTTP loading through the offline peer;
4. cancellation, timeout, reset, and out-of-memory recovery;
5. synchronized user-approved downloads to Data;
6. independently reviewed TLS and HTTPS behavior.

Release notes describe only the stages supported by completed evidence.
