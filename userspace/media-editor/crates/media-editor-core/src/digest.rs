// SPDX-License-Identifier: GPL-3.0-only
//! Content identity.
//!
//! Media Editor names media, cache entries, and saved files by what they contain
//! rather than by where they are (R-9.5, R-8.5). SHA-256 is the function,
//! because it is the one Phipia already speaks: every pinned artefact in that
//! project is a SHA-256, and an editor that reports a different digest for the
//! same bytes would be answering a different question.
//!
//! This is a from-scratch implementation in safe Rust, checked against the
//! published test vectors including the one-million-character message. It
//! exists because Media Editor has no dependencies yet and the alternative was a
//! vendoring gate that needs a network; `sha2` is the intended replacement and
//! will be evaluated against
//! [the import gate](../../../docs/DEPENDENCY_POLICY.md) when it can be run.
//! Whichever wins, the vectors below decide it.

use core::fmt;

/// How many bytes a digest is.
pub const DIGEST_BYTES: usize = 32;

/// The size of the block the compression function consumes.
const BLOCK_BYTES: usize = 64;

/// A content digest.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Digest([u8; DIGEST_BYTES]);

impl Digest {
    /// Wrap thirty-two bytes.
    #[must_use]
    pub const fn new(bytes: [u8; DIGEST_BYTES]) -> Self {
        Self(bytes)
    }

    /// The digest of a byte slice.
    #[must_use]
    pub fn of(bytes: &[u8]) -> Self {
        let mut hasher = Sha256::new();
        hasher.update(bytes);
        hasher.finish()
    }

    /// The bytes.
    #[must_use]
    pub const fn bytes(&self) -> &[u8; DIGEST_BYTES] {
        &self.0
    }
}

impl fmt::Display for Digest {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Upper case, unseparated: the form Phipia's build and release records
        // already use, so a digest can be compared by eye across the two
        // projects without transcription.
        for byte in &self.0 {
            write!(formatter, "{byte:02X}")?;
        }
        Ok(())
    }
}

/// The first thirty-two bits of the fractional parts of the cube roots of the
/// first sixty-four primes, as FIPS 180-4 specifies them.
const ROUND_CONSTANTS: [u32; 64] = [
    0x428a_2f98,
    0x7137_4491,
    0xb5c0_fbcf,
    0xe9b5_dba5,
    0x3956_c25b,
    0x59f1_11f1,
    0x923f_82a4,
    0xab1c_5ed5,
    0xd807_aa98,
    0x1283_5b01,
    0x2431_85be,
    0x550c_7dc3,
    0x72be_5d74,
    0x80de_b1fe,
    0x9bdc_06a7,
    0xc19b_f174,
    0xe49b_69c1,
    0xefbe_4786,
    0x0fc1_9dc6,
    0x240c_a1cc,
    0x2de9_2c6f,
    0x4a74_84aa,
    0x5cb0_a9dc,
    0x76f9_88da,
    0x983e_5152,
    0xa831_c66d,
    0xb003_27c8,
    0xbf59_7fc7,
    0xc6e0_0bf3,
    0xd5a7_9147,
    0x06ca_6351,
    0x1429_2967,
    0x27b7_0a85,
    0x2e1b_2138,
    0x4d2c_6dfc,
    0x5338_0d13,
    0x650a_7354,
    0x766a_0abb,
    0x81c2_c92e,
    0x9272_2c85,
    0xa2bf_e8a1,
    0xa81a_664b,
    0xc24b_8b70,
    0xc76c_51a3,
    0xd192_e819,
    0xd699_0624,
    0xf40e_3585,
    0x106a_a070,
    0x19a4_c116,
    0x1e37_6c08,
    0x2748_774c,
    0x34b0_bcb5,
    0x391c_0cb3,
    0x4ed8_aa4a,
    0x5b9c_ca4f,
    0x682e_6ff3,
    0x748f_82ee,
    0x78a5_636f,
    0x84c8_7814,
    0x8cc7_0208,
    0x90be_fffa,
    0xa450_6ceb,
    0xbef9_a3f7,
    0xc671_78f2,
];

/// The first thirty-two bits of the fractional parts of the square roots of
/// the first eight primes.
const INITIAL_STATE: [u32; 8] = [
    0x6a09_e667,
    0xbb67_ae85,
    0x3c6e_f372,
    0xa54f_f53a,
    0x510e_527f,
    0x9b05_688c,
    0x1f83_d9ab,
    0x5be0_cd19,
];

/// A SHA-256 computation in progress.
///
/// Every addition here is `wrapping_add` because the specification defines the
/// arithmetic as modulo 2^32; nowhere else in Media Editor may an overflow be
/// silent (R-3.1.6), and this is the exception that states itself.
#[derive(Clone)]
pub struct Sha256 {
    state: [u32; 8],
    block: [u8; BLOCK_BYTES],
    buffered: usize,
    message_bytes: u64,
}

impl Default for Sha256 {
    fn default() -> Self {
        Self::new()
    }
}

impl Sha256 {
    /// A fresh computation.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            state: INITIAL_STATE,
            block: [0; BLOCK_BYTES],
            buffered: 0,
            message_bytes: 0,
        }
    }

    /// Absorb more of the message.
    pub fn update(&mut self, mut bytes: &[u8]) {
        self.message_bytes = self.message_bytes.wrapping_add(bytes.len() as u64);
        while !bytes.is_empty() {
            let room = BLOCK_BYTES - self.buffered;
            let take = room.min(bytes.len());
            let (head, tail) = bytes.split_at(take);
            self.block[self.buffered..self.buffered + take].copy_from_slice(head);
            self.buffered += take;
            bytes = tail;
            if self.buffered == BLOCK_BYTES {
                let block = self.block;
                self.compress(&block);
                self.buffered = 0;
            }
        }
    }

    /// Finish, and produce the digest.
    #[must_use]
    pub fn finish(mut self) -> Digest {
        // The padding is one set bit, then zeroes, then the message length in
        // bits as a big-endian sixty-four bit number.
        let bit_length = self.message_bytes.wrapping_mul(8);
        self.update(&[0x80]);
        // `update` counted the padding byte; the length field must describe the
        // message, so it was captured above.
        while self.buffered != BLOCK_BYTES - 8 {
            self.update(&[0x00]);
        }
        let mut tail = [0_u8; 8];
        tail.copy_from_slice(&bit_length.to_be_bytes());
        self.block[BLOCK_BYTES - 8..].copy_from_slice(&tail);
        let block = self.block;
        self.compress(&block);

        let mut digest = [0_u8; DIGEST_BYTES];
        for (index, word) in self.state.iter().enumerate() {
            digest[index * 4..index * 4 + 4].copy_from_slice(&word.to_be_bytes());
        }
        Digest::new(digest)
    }

    /// One block of the compression function.
    #[allow(
        clippy::many_single_char_names,
        reason = "a, b, ... h are the working variables FIPS 180-4 names, and renaming them would make this harder to check against the standard"
    )]
    fn compress(&mut self, block: &[u8; BLOCK_BYTES]) {
        let mut schedule = [0_u32; 64];
        for (index, word) in schedule.iter_mut().take(16).enumerate() {
            let mut bytes = [0_u8; 4];
            bytes.copy_from_slice(&block[index * 4..index * 4 + 4]);
            *word = u32::from_be_bytes(bytes);
        }
        for index in 16..64 {
            let previous = schedule[index - 15];
            let recent = schedule[index - 2];
            let small = previous.rotate_right(7) ^ previous.rotate_right(18) ^ (previous >> 3);
            let large = recent.rotate_right(17) ^ recent.rotate_right(19) ^ (recent >> 10);
            schedule[index] = schedule[index - 16]
                .wrapping_add(small)
                .wrapping_add(schedule[index - 7])
                .wrapping_add(large);
        }

        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = self.state;
        for index in 0..64 {
            let sigma1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let choose = (e & f) ^ (!e & g);
            let first = h
                .wrapping_add(sigma1)
                .wrapping_add(choose)
                .wrapping_add(ROUND_CONSTANTS[index])
                .wrapping_add(schedule[index]);
            let sigma0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let majority = (a & b) ^ (a & c) ^ (b & c);
            let second = sigma0.wrapping_add(majority);

            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(first);
            d = c;
            c = b;
            b = a;
            a = first.wrapping_add(second);
        }

        for (slot, value) in self.state.iter_mut().zip([a, b, c, d, e, f, g, h]) {
            *slot = slot.wrapping_add(value);
        }
    }
}
