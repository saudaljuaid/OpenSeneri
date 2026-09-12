// SPDX-License-Identifier: GPL-3.0-only
#![no_std]

extern crate alloc;

use core::alloc::{GlobalAlloc, Layout};
use core::arch::{asm, global_asm};
use core::panic::PanicInfo;
use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};

pub const ABI_VERSION: u32 = 1;
pub type Handle = u64;
pub type Result<T> = core::result::Result<T, Error>;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(transparent)]
pub struct Error(pub i32);

impl Error {
    pub const TIMED_OUT: Self = Self(110);
    pub const CANCELED: Self = Self(125);
}

const SYS_ABI_VERSION: u64 = 0x0000;
const SYS_EXIT: u64 = 0x0001;
const SYS_CONSOLE_WRITE: u64 = 0x0002;
const SYS_HANDLE_CLOSE: u64 = 0x0004;
const SYS_MEMORY_MAP: u64 = 0x0100;
const SYS_MEMORY_UNMAP: u64 = 0x0101;
const SYS_FILE_OPEN: u64 = 0x0200;
const SYS_FILE_READ: u64 = 0x0201;
const SYS_FILE_WRITE: u64 = 0x0202;
const SYS_FILE_SEEK: u64 = 0x0203;
const SYS_TIME_MONOTONIC: u64 = 0x0300;
const SYS_SLEEP_UNTIL: u64 = 0x0301;
const SYS_WAIT: u64 = 0x0302;
const SYS_RANDOM: u64 = 0x0303;
const SYS_CANCEL: u64 = 0x0306;
const SYS_WINDOW_CREATE: u64 = 0x0400;
const SYS_SURFACE_PRESENT: u64 = 0x0401;
const SYS_EVENT_READ: u64 = 0x0402;
const SYS_DNS_RESOLVE: u64 = 0x0500;
const SYS_STREAM_OPEN: u64 = 0x0501;
const SYS_STREAM_CONNECT: u64 = 0x0502;
const SYS_STREAM_READ: u64 = 0x0503;
const SYS_STREAM_WRITE: u64 = 0x0504;
const SYS_THREAD_CREATE: u64 = 0x0600;
const SYS_THREAD_EXIT: u64 = 0x0601;
const SYS_THREAD_JOIN: u64 = 0x0602;
const SYS_TLS_SET: u64 = 0x0603;
const SYS_TLS_GET: u64 = 0x0604;
const SYS_FUTEX_WAIT: u64 = 0x0605;
const SYS_FUTEX_WAKE: u64 = 0x0606;

pub const VOLUME_SYSTEM: u16 = 1;
pub const VOLUME_DATA: u16 = 2;
pub const OPEN_READ: u32 = 1 << 0;
pub const OPEN_WRITE: u32 = 1 << 1;
pub const OPEN_CREATE: u32 = 1 << 2;
pub const OPEN_TRUNCATE: u32 = 1 << 3;
pub const SEEK_START: u32 = 0;
pub const SEEK_CURRENT: u32 = 1;
pub const SEEK_END: u32 = 2;
pub const WAIT_READABLE: u32 = 1 << 0;
pub const PIXEL_XRGB8888: u32 = 1;
pub const EVENT_KEY: u32 = 1;
pub const EVENT_POINTER_MOVE: u32 = 2;
pub const EVENT_POINTER_BUTTON: u32 = 3;
pub const EVENT_FOCUS: u32 = 4;
pub const EVENT_CLOSE: u32 = 5;

global_asm!(r#"
    .section .text.start,"ax",@progbits
    .global _start
    .type _start,@function
_start:
    xorq %rbp, %rbp
    andq $-16, %rsp
    call phipia_rust_start
    ud2
    .size _start, .-_start
"#, options(att_syntax));

#[inline]
unsafe fn syscall0(number: u64) -> i64 {
    let result: i64;
    // SAFETY: the Phipia ABI declares RCX and R11 clobbered by SYSCALL.
    unsafe {
        asm!("syscall", inlateout("rax") number as i64 => result,
            lateout("rcx") _, lateout("r11") _, options(nostack));
    }
    result
}

#[inline]
unsafe fn syscall1(number: u64, a0: u64) -> i64 {
    let result: i64;
    // SAFETY: arguments and clobbers follow the native ABI v1 register contract.
    unsafe {
        asm!("syscall", inlateout("rax") number as i64 => result,
            in("rdi") a0, lateout("rcx") _, lateout("r11") _,
            options(nostack));
    }
    result
}

#[inline]
unsafe fn syscall2(number: u64, a0: u64, a1: u64) -> i64 {
    let result: i64;
    // SAFETY: arguments and clobbers follow the native ABI v1 register contract.
    unsafe {
        asm!("syscall", inlateout("rax") number as i64 => result,
            in("rdi") a0, in("rsi") a1, lateout("rcx") _, lateout("r11") _,
            options(nostack));
    }
    result
}

#[inline]
unsafe fn syscall3(number: u64, a0: u64, a1: u64, a2: u64) -> i64 {
    let result: i64;
    // SAFETY: arguments and clobbers follow the native ABI v1 register contract.
    unsafe {
        asm!("syscall", inlateout("rax") number as i64 => result,
            in("rdi") a0, in("rsi") a1, in("rdx") a2,
            lateout("rcx") _, lateout("r11") _, options(nostack));
    }
    result
}

fn result(value: i64) -> Result<u64> {
    if value < 0 { Err(Error((-value) as i32)) } else { Ok(value as u64) }
}

pub fn abi_version() -> Result<u32> {
    // SAFETY: the version query has no pointer arguments.
    result(unsafe { syscall0(SYS_ABI_VERSION) }).map(|value| value as u32)
}

pub fn exit(status: i32) -> ! {
    // SAFETY: process exit consumes no user pointer.
    let _ = unsafe { syscall1(SYS_EXIT, status as i64 as u64) };
    loop { core::hint::spin_loop(); }
}

pub fn console_write(bytes: &[u8]) -> Result<usize> {
    // SAFETY: the slice is readable for its complete length during the call.
    result(unsafe { syscall2(SYS_CONSOLE_WRITE, bytes.as_ptr() as u64,
        bytes.len() as u64) }).map(|value| value as usize)
}

#[repr(C, packed)]
struct MemoryMapRequest {
    size: u32,
    version: u32,
    length: u64,
    address_hint: u64,
    flags: u32,
    reserved: u32,
}

#[repr(C, packed)]
struct MemoryMapResponse {
    size: u32,
    version: u32,
    address: u64,
    length: u64,
}

#[repr(C)]
struct AllocationHeader {
    base: u64,
    length: u64,
}

struct PageAllocator {
    lock: AtomicU32,
}

unsafe impl Sync for PageAllocator {}

impl PageAllocator {
    fn lock(&self) {
        while self.lock.compare_exchange(0, 1, Ordering::Acquire,
                Ordering::Relaxed).is_err() {
            let wait = FutexRequest::new(&self.lock, 1, 0, 0);
            // SAFETY: wait points to an ABI-sized request for a live atomic word.
            let _ = unsafe { syscall1(SYS_FUTEX_WAIT, &wait as *const _ as u64) };
        }
    }

    fn unlock(&self) {
        self.lock.store(0, Ordering::Release);
        let wake = FutexRequest::new(&self.lock, 0, 0, 1);
        // SAFETY: wake points to an ABI-sized request for a live atomic word.
        let _ = unsafe { syscall1(SYS_FUTEX_WAKE, &wake as *const _ as u64) };
    }
}

#[global_allocator]
static ALLOCATOR: PageAllocator = PageAllocator { lock: AtomicU32::new(0) };

unsafe impl GlobalAlloc for PageAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let header = core::mem::size_of::<AllocationHeader>();
        let requested = match layout.size().checked_add(layout.align())
            .and_then(|value| value.checked_add(header)) {
            Some(value) => value,
            None => return ptr::null_mut(),
        };
        let request = MemoryMapRequest { size: 32, version: ABI_VERSION,
            length: requested as u64, address_hint: 0, flags: 3, reserved: 0 };
        let mut response = MemoryMapResponse { size: 24, version: ABI_VERSION,
            address: 0, length: 0 };
        self.lock();
        // SAFETY: both packed records are valid for the duration of the syscall.
        let mapped = unsafe { syscall2(SYS_MEMORY_MAP,
            &request as *const _ as u64, &mut response as *mut _ as u64) };
        self.unlock();
        if mapped < 0 { return ptr::null_mut(); }
        let start = response.address as usize + header;
        let aligned = (start + layout.align() - 1) & !(layout.align() - 1);
        let allocation_header = (aligned - header) as *mut AllocationHeader;
        // SAFETY: the header lies within the newly mapped writable range.
        unsafe { allocation_header.write(AllocationHeader {
            base: response.address, length: response.length,
        }); }
        aligned as *mut u8
    }

    unsafe fn dealloc(&self, pointer: *mut u8, _layout: Layout) {
        if pointer.is_null() { return; }
        let header_pointer = unsafe { pointer.sub(core::mem::size_of::<AllocationHeader>()) }
            as *const AllocationHeader;
        // SAFETY: alloc stored this header immediately before the returned address.
        let header = unsafe { header_pointer.read() };
        self.lock();
        // SAFETY: base and length identify the allocation's complete native mapping.
        let _ = unsafe { syscall2(SYS_MEMORY_UNMAP, header.base, header.length) };
        self.unlock();
    }
}

#[repr(C, packed)]
struct Path {
    address: u64,
    length: u32,
    volume: u16,
    reserved: u16,
}

#[repr(C, packed)]
struct FileOpenRequest {
    size: u32,
    version: u32,
    path: Path,
    flags: u32,
    reserved: u32,
}

#[repr(C, packed)]
struct IoRequest {
    size: u32,
    version: u32,
    handle: Handle,
    buffer: u64,
    offset: u64,
    length: u32,
    flags: u32,
}

#[repr(C, packed)]
struct SeekRequest {
    size: u32,
    version: u32,
    handle: Handle,
    offset: i64,
    origin: u32,
    reserved: u32,
}

pub struct File(Handle);

impl File {
    pub fn open(volume: u16, path: &[u8], flags: u32) -> Result<Self> {
        if path.len() > u32::MAX as usize { return Err(Error(22)); }
        let request = FileOpenRequest { size: 32, version: ABI_VERSION,
            path: Path { address: path.as_ptr() as u64, length: path.len() as u32,
                volume, reserved: 0 }, flags, reserved: 0 };
        // SAFETY: request and its path bytes remain readable during the call.
        result(unsafe { syscall1(SYS_FILE_OPEN, &request as *const _ as u64) })
            .map(Self)
    }

    pub fn read(&self, buffer: &mut [u8]) -> Result<usize> {
        self.io(SYS_FILE_READ, buffer.as_mut_ptr(), buffer.len())
    }

    pub fn write(&self, buffer: &[u8]) -> Result<usize> {
        self.io(SYS_FILE_WRITE, buffer.as_ptr() as *mut u8, buffer.len())
    }

    fn io(&self, number: u64, buffer: *mut u8, length: usize) -> Result<usize> {
        if length > u32::MAX as usize { return Err(Error(22)); }
        let request = IoRequest { size: 40, version: ABI_VERSION, handle: self.0,
            buffer: buffer as u64, offset: u64::MAX, length: length as u32,
            flags: 0 };
        // SAFETY: the caller-provided slice is valid for the operation direction.
        result(unsafe { syscall1(number, &request as *const _ as u64) })
            .map(|value| value as usize)
    }

    pub fn seek(&self, offset: i64, origin: u32) -> Result<u64> {
        let request = SeekRequest { size: 32, version: ABI_VERSION,
            handle: self.0, offset, origin, reserved: 0 };
        // SAFETY: request is a complete immutable ABI record.
        result(unsafe { syscall1(SYS_FILE_SEEK, &request as *const _ as u64) })
    }

    pub fn handle(&self) -> Handle { self.0 }
}

impl Drop for File {
    fn drop(&mut self) {
        // SAFETY: a File owns one live handle and drops it at most once.
        let _ = unsafe { syscall1(SYS_HANDLE_CLOSE, self.0) };
    }
}

pub fn monotonic_ns() -> Result<u64> {
    // SAFETY: the monotonic query has no pointer arguments.
    result(unsafe { syscall0(SYS_TIME_MONOTONIC) })
}

pub fn sleep_until(deadline_ns: u64) -> Result<()> {
    // SAFETY: the deadline is passed by value.
    result(unsafe { syscall1(SYS_SLEEP_UNTIL, deadline_ns) }).map(|_| ())
}

pub fn random(buffer: &mut [u8]) -> Result<()> {
    // SAFETY: the output slice is writable for its complete length.
    result(unsafe { syscall2(SYS_RANDOM, buffer.as_mut_ptr() as u64,
        buffer.len() as u64) }).map(|_| ())
}

#[derive(Clone, Copy)]
#[repr(C, packed)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

#[repr(C, packed)]
struct WindowCreateRequest {
    size: u32,
    version: u32,
    title: u64,
    title_length: u32,
    width: u32,
    height: u32,
    pixel_format: u32,
    flags: u32,
    reserved: u32,
}

#[derive(Clone, Copy)]
#[repr(C, packed)]
struct WindowCreateResponse {
    size: u32,
    version: u32,
    window: Handle,
    events: Handle,
    surface_address: u64,
    width: u32,
    height: u32,
    stride_bytes: u32,
    pixel_format: u32,
}

#[repr(C, packed)]
struct PresentRequest {
    size: u32,
    version: u32,
    window: Handle,
    rectangles: u64,
    rectangle_count: u32,
    flags: u32,
}

#[derive(Clone, Copy)]
#[repr(C, packed)]
pub struct Event {
    pub size: u32,
    pub version: u32,
    pub kind: u32,
    pub flags: u32,
    pub monotonic_ns: u64,
    pub x: i32,
    pub y: i32,
    pub delta_x: i32,
    pub delta_y: i32,
    pub code: u32,
    pub value: u32,
    pub modifiers: u32,
    pub reserved: u32,
}

pub struct Window {
    window: Handle,
    events: Handle,
    surface: *mut u32,
    width: u32,
    height: u32,
    stride_bytes: u32,
}

impl Window {
    pub fn create(title: &[u8], width: u32, height: u32) -> Result<Self> {
        if title.len() > u32::MAX as usize { return Err(Error(22)); }
        let request = WindowCreateRequest { size: 40, version: ABI_VERSION,
            title: title.as_ptr() as u64, title_length: title.len() as u32,
            width, height, pixel_format: PIXEL_XRGB8888, flags: 0, reserved: 0 };
        let mut response = WindowCreateResponse { size: 48,
            version: ABI_VERSION, window: 0, events: 0, surface_address: 0,
            width: 0, height: 0, stride_bytes: 0, pixel_format: 0 };
        // SAFETY: request and response are valid complete ABI records.
        result(unsafe { syscall2(SYS_WINDOW_CREATE, &request as *const _ as u64,
            &mut response as *mut _ as u64) })?;
        Ok(Self { window: response.window, events: response.events,
            surface: response.surface_address as *mut u32,
            width: response.width, height: response.height,
            stride_bytes: response.stride_bytes })
    }

    pub fn dimensions(&self) -> (u32, u32, u32) {
        (self.width, self.height, self.stride_bytes)
    }

    pub fn surface(&mut self) -> &mut [u32] {
        let pixels = self.stride_bytes as usize / 4 * self.height as usize;
        // SAFETY: the kernel created this process-local writable surface mapping.
        unsafe { core::slice::from_raw_parts_mut(self.surface, pixels) }
    }

    pub fn present(&self, rectangles: &[Rect]) -> Result<()> {
        if rectangles.len() > 8 { return Err(Error(22)); }
        let request = PresentRequest { size: 32, version: ABI_VERSION,
            window: self.window, rectangles: rectangles.as_ptr() as u64,
            rectangle_count: rectangles.len() as u32, flags: 0 };
        // SAFETY: the damage array remains readable for the complete call.
        result(unsafe { syscall1(SYS_SURFACE_PRESENT,
            &request as *const _ as u64) }).map(|_| ())
    }

    pub fn wait_event(&self, deadline_ns: u64) -> Result<Event> {
        let mut item = WaitItem { handle: self.events,
            interests: WAIT_READABLE, ready: 0 };
        let request = WaitRequest { size: 32, version: ABI_VERSION,
            items: &mut item as *mut _ as u64, deadline_ns, count: 1, flags: 0 };
        // SAFETY: request and item remain valid and writable while waiting.
        result(unsafe { syscall1(SYS_WAIT, &request as *const _ as u64) })?;
        let mut event = Event::empty();
        // SAFETY: event is a writable ABI-sized output object.
        result(unsafe { syscall2(SYS_EVENT_READ, self.events,
            &mut event as *mut _ as u64) })?;
        Ok(event)
    }
}

impl Drop for Window {
    fn drop(&mut self) {
        // SAFETY: each owned handle is closed exactly once.
        let _ = unsafe { syscall1(SYS_HANDLE_CLOSE, self.events) };
        // SAFETY: each owned handle is closed exactly once.
        let _ = unsafe { syscall1(SYS_HANDLE_CLOSE, self.window) };
    }
}

impl Event {
    const fn empty() -> Self {
        Self { size: 56, version: ABI_VERSION, kind: 0, flags: 0,
            monotonic_ns: 0, x: 0, y: 0, delta_x: 0, delta_y: 0,
            code: 0, value: 0, modifiers: 0, reserved: 0 }
    }
}

#[repr(C, packed)]
struct WaitItem {
    handle: Handle,
    interests: u32,
    ready: u32,
}

#[repr(C, packed)]
struct WaitRequest {
    size: u32,
    version: u32,
    items: u64,
    deadline_ns: u64,
    count: u32,
    flags: u32,
}

#[derive(Clone, Copy)]
#[repr(C, packed)]
pub struct Ipv4Endpoint {
    pub address: u32,
    pub port: u16,
    pub reserved: u16,
}

#[repr(C, packed)]
struct NetworkIo {
    size: u32,
    version: u32,
    handle: Handle,
    buffer: u64,
    deadline_ns: u64,
    endpoint: Ipv4Endpoint,
    length: u32,
    flags: u32,
}

pub fn dns_resolve(hostname: &[u8], deadline_ns: u64) -> Result<u32> {
    // SAFETY: hostname remains readable for its complete length.
    result(unsafe { syscall3(SYS_DNS_RESOLVE, hostname.as_ptr() as u64,
        hostname.len() as u64, deadline_ns) }).map(|value| value as u32)
}

pub struct TcpStream(Handle);

impl TcpStream {
    pub fn open() -> Result<Self> {
        // SAFETY: stream creation has no pointer arguments.
        result(unsafe { syscall0(SYS_STREAM_OPEN) }).map(Self)
    }

    pub fn connect(&self, endpoint: &Ipv4Endpoint, deadline_ns: u64) -> Result<()> {
        // SAFETY: endpoint remains readable during the call.
        result(unsafe { syscall3(SYS_STREAM_CONNECT, self.0,
            endpoint as *const _ as u64, deadline_ns) }).map(|_| ())
    }

    pub fn read(&self, buffer: &mut [u8], deadline_ns: u64) -> Result<usize> {
        self.io(SYS_STREAM_READ, buffer.as_mut_ptr(), buffer.len(), deadline_ns)
    }

    pub fn write(&self, buffer: &[u8], deadline_ns: u64) -> Result<usize> {
        self.io(SYS_STREAM_WRITE, buffer.as_ptr() as *mut u8,
            buffer.len(), deadline_ns)
    }

    fn io(&self, number: u64, buffer: *mut u8, length: usize,
        deadline_ns: u64) -> Result<usize> {
        if length > u32::MAX as usize { return Err(Error(22)); }
        let request = NetworkIo { size: 48, version: ABI_VERSION, handle: self.0,
            buffer: buffer as u64, deadline_ns,
            endpoint: Ipv4Endpoint { address: 0, port: 0, reserved: 0 },
            length: length as u32, flags: 0 };
        // SAFETY: the supplied slice is valid for the operation direction.
        result(unsafe { syscall1(number, &request as *const _ as u64) })
            .map(|value| value as usize)
    }

    pub fn cancel(&self) -> Result<()> {
        // SAFETY: the stream handle is passed by value.
        result(unsafe { syscall1(SYS_CANCEL, self.0) }).map(|_| ())
    }
}

impl Drop for TcpStream {
    fn drop(&mut self) {
        // SAFETY: the owned handle is closed exactly once.
        let _ = unsafe { syscall1(SYS_HANDLE_CLOSE, self.0) };
    }
}

#[repr(C, packed)]
struct ThreadCreateRequest {
    size: u32,
    version: u32,
    entry: u64,
    argument: u64,
    tls_base: u64,
    stack_bytes: u32,
    flags: u32,
}

#[repr(C, packed)]
struct FutexRequest {
    size: u32,
    version: u32,
    address: u64,
    deadline_ns: u64,
    expected: u32,
    count: u32,
}

impl FutexRequest {
    fn new(atomic: &AtomicU32, expected: u32, deadline_ns: u64,
        count: u32) -> Self {
        Self { size: 32, version: ABI_VERSION,
            address: atomic as *const _ as u64, deadline_ns, expected, count }
    }
}

pub fn futex_wait(word: &AtomicU32, expected: u32,
    deadline_ns: u64) -> Result<()> {
    let request = FutexRequest::new(word, expected, deadline_ns, 0);
    // SAFETY: word and the complete request remain live while the thread waits.
    result(unsafe { syscall1(SYS_FUTEX_WAIT,
        &request as *const _ as u64) }).map(|_| ())
}

pub fn futex_wake(word: &AtomicU32, count: u32) -> Result<usize> {
    if count == 0 { return Err(Error(22)); }
    let request = FutexRequest::new(word, 0, 0, count);
    // SAFETY: word and the immutable request are valid for the complete call.
    result(unsafe { syscall1(SYS_FUTEX_WAKE,
        &request as *const _ as u64) }).map(|value| value as usize)
}

pub struct Thread(Handle);

impl Thread {
    pub fn create(entry: extern "C" fn(usize) -> !, argument: usize,
        tls_base: usize, stack_bytes: u32) -> Result<Self> {
        let request = ThreadCreateRequest { size: 40, version: ABI_VERSION,
            entry: entry as usize as u64, argument: argument as u64,
            tls_base: tls_base as u64, stack_bytes, flags: 0 };
        // SAFETY: request is a complete immutable ABI record.
        result(unsafe { syscall1(SYS_THREAD_CREATE,
            &request as *const _ as u64) }).map(Self)
    }

    pub fn join(self) -> Result<i32> {
        let handle = self.0;
        // SAFETY: the thread handle is owned and joinable.
        let joined = result(unsafe { syscall1(SYS_THREAD_JOIN, handle) })?;
        core::mem::forget(self);
        // SAFETY: a successful join leaves the terminated thread handle owned.
        result(unsafe { syscall1(SYS_HANDLE_CLOSE, handle) })?;
        Ok(joined as i32)
    }
}

impl Drop for Thread {
    fn drop(&mut self) {
        // SAFETY: the owned handle is closed exactly once.
        let _ = unsafe { syscall1(SYS_HANDLE_CLOSE, self.0) };
    }
}

pub fn thread_exit(status: i32) -> ! {
    // SAFETY: thread exit consumes no pointer.
    let _ = unsafe { syscall1(SYS_THREAD_EXIT, status as i64 as u64) };
    loop { core::hint::spin_loop(); }
}

pub fn tls_set(base: usize) -> Result<()> {
    // SAFETY: TLS base is validated by the kernel before installing FS base.
    result(unsafe { syscall1(SYS_TLS_SET, base as u64) }).map(|_| ())
}

pub fn tls_get() -> Result<usize> {
    // SAFETY: TLS query has no pointer arguments.
    result(unsafe { syscall0(SYS_TLS_GET) }).map(|value| value as usize)
}

pub struct Startup {
    argc: usize,
    argv: *const *const u8,
    environment: *const *const u8,
}

impl Startup {
    pub const fn argc(&self) -> usize { self.argc }

    pub fn argument(&self, index: usize) -> Option<&'static [u8]> {
        if index >= self.argc { return None; }
        // SAFETY: the loader provides argc live NUL-terminated argument pointers.
        let pointer = unsafe { *self.argv.add(index) };
        c_string(pointer)
    }

    pub const fn environment(&self) -> *const *const u8 { self.environment }
}

fn c_string(pointer: *const u8) -> Option<&'static [u8]> {
    if pointer.is_null() { return None; }
    let mut length = 0usize;
    // SAFETY: startup strings are immutable and NUL-terminated in the loader stack.
    unsafe {
        while *pointer.add(length) != 0 { length += 1; }
        Some(core::slice::from_raw_parts(pointer, length))
    }
}

#[doc(hidden)]
pub fn start(main: fn(Startup) -> i32, argc: usize, argv: *const *const u8,
    environment: *const *const u8) -> ! {
    if abi_version() != Ok(ABI_VERSION) { exit(126); }
    exit(main(Startup { argc, argv, environment }))
}

#[macro_export]
macro_rules! phipia_main {
    ($main:path) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn phipia_rust_start(argc: usize, argv: *const *const u8,
            environment: *const *const u8) -> ! {
            $crate::start($main, argc, argv, environment)
        }
    };
}

#[panic_handler]
fn panic(_information: &PanicInfo<'_>) -> ! {
    let _ = console_write(b"Phipia Rust application panic\n");
    exit(127)
}
