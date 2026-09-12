<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Offline TLS fixture

These keys and certificates exist only for Phipia's deterministic local TLS
peer. They are public test material and must never be installed as production
trust roots. The fixed realtime used by the host client is 2026-08-31 12:00 UTC.

`ca.pem` is the trusted offline root. `anchor.txt` is the same root's DER
subject name and RSA public key encoded for the BearSSL test client. The valid,
expired, future, and untrusted leaf records all use the hostname
`repo.phipia.test`; the untrusted leaf is signed by a deliberately absent root.
The Python peer consumes the committed PEM records without network access or
certificate generation.

SHA-256:

```text
23cbda48ee8b659919b25760011481d7cf7db444af21adc9d47095a24bdc4503  anchor.txt
d13a4f5b79fbebfe1bd69401cea778fa8e1827c4d1ec16cab1a5d06ec9d5dac5  ca.pem
f207af1bc263e1acee9d40f12f58856a5e107b56e6da45fbc10ab72b23839936  expired-key.pem
1896a926080114f6fd04960cc31c7234a79be0e7b27844ff336b005f1bb26d21  expired.pem
83401a2d3db347b41b35014e384dd601b42ef81587daf063d3ca357b112ffa51  future-key.pem
ff3696429950a6aefd347e49c4f1f0e725ccdfac5f779c45a2732b233de8a8c4  future.pem
4db57fbd96f5810ed07c17c9cb032ece5b4a3aec59f97736abb5c2b469afea8e  untrusted-key.pem
aab065072e062305cbc0775e64df403e345304c6df62ec97bcc8bcc9fd738a82  untrusted.pem
55c0533e694dc6e038cb8cc533e132e74d356a40972a684ca69d094bb59764fe  valid-key.pem
966961e7cbd7922bc6ab207dfc60bd221de24fb17d6119cb42d1fb52935ca20f  valid.pem
```
