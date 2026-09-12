/*
 * Keep the exercised uname -s formatting path compatible with Phipia's
 * integer-only Linux userspace ABI.  This build-only overlay leaves musl's
 * source archive byte-for-byte unchanged and applies only to vfprintf; the
 * unexercised floating-point formatting helpers retain their normal target.
 */
#define vfprintf \
    __attribute__((target("no-sse,no-sse2,no-mmx"))) vfprintf
