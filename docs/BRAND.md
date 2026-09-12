# Phipia identity

Phipia is the product, desktop, shell, and release identity. Every surface a
user can see uses `Phipia`: boot and panic text, the `phip>` shell prompt,
Linux `uname`, Settings, application chrome, package presentation, filesystem
labels, screenshots, videos, releases, and downloadable artifacts.

The repository, public C include namespace, SDK, packages, tooling, boot
diagnostics, and user-facing product all use the Phipia identity. No legacy
identity is retained as a compatibility alias or fallback.

## Canonical identity assets

[`assets/phipia/logo.png`](../assets/phipia/logo.png) is the canonical Phipia
logo. Its SHA-256 is:

    6A07ABE324C2D80AA0F1DD3A318C103C3B6A81FEF1F72D5F4808D589626B1E88

[`assets/phipia/wallpaper.png`](../assets/phipia/wallpaper.png) is the default
1024x768 desktop wallpaper. Application icons are the Phipia icon set embedded
by the deterministic asset generators. Old product marks must not be bundled,
drawn, or used as fallbacks.

The kernel does not parse PNG at runtime. The normal asset pipeline converts
the canonical logo and wallpaper into bounded embedded streams; the boot proof
decodes and verifies the displayed pixels.

## Product copy

The current development version is `Phipia 2.2.0`. Public copy uses short,
direct descriptions. Proof terms such as `PASS`, `READY`, and `ONLINE` belong
in diagnostics rather than desktop chrome.

## Verification

Linux CI is authoritative for the kernel build and QEMU evidence. Verification
must reject a shipped transcript or user-facing asset that contains an earlier
product, shell, or release identity. QEMU captures are generated from the exact
commit and are never manually edited.
