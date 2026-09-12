// SPDX-License-Identifier: GPL-3.0-only
#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;
use core::sync::atomic::{AtomicU32, Ordering};
use phipia::{File, OPEN_CREATE, OPEN_TRUNCATE, OPEN_WRITE, Startup, Thread,
    VOLUME_DATA, console_write, monotonic_ns, random, sleep_until, thread_exit};

static THREAD_VALUE: AtomicU32 = AtomicU32::new(0);

extern "C" fn worker(argument: usize) -> ! {
    THREAD_VALUE.store(argument as u32, Ordering::Release);
    thread_exit(0)
}

fn application(startup: Startup) -> i32 {
    if startup.argc() < 1 || startup.argument(0).is_none() {
        return 10;
    }
    let mut values = Vec::with_capacity(64);
    for value in 0u8..64 { values.push(value ^ 0x5a); }
    if values.len() != 64 || values[63] != (63 ^ 0x5a) { return 11; }

    let mut entropy = [0u8; 16];
    if random(&mut entropy).is_err() { return 12; }
    let thread = match Thread::create(worker, 73, 0, 64 * 1024) {
        Ok(value) => value,
        Err(_) => return 13,
    };
    if thread.join() != Ok(0) || THREAD_VALUE.load(Ordering::Acquire) != 73 {
        return 14;
    }

    let file = match File::open(VOLUME_DATA, b"RUST.TXT",
            OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE) {
        Ok(value) => value,
        Err(_) => return 15,
    };
    if file.write(b"native Rust no_std ABI v1\n") != Ok(26) { return 16; }
    drop(file);
    let now = match monotonic_ns() { Ok(value) => value, Err(_) => return 17 };
    if sleep_until(now + 100_000).is_err() { return 18; }
    if console_write(b"PHIPIA RUST PASS alloc file time entropy thread\n").is_err() {
        return 19;
    }
    0
}

phipia::phipia_main!(application);
