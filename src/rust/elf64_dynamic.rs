// SPDX-License-Identifier: GPL-3.0-only
//! Allocation-free admission and relocation groundwork for bounded x86-64 ET_DYN objects.
//!
//! This module intentionally performs no filesystem access, page mapping, or constructor
//! execution.  Callers must authenticate every byte slice before parsing, choose deterministic
//! load biases, copy segments into private writable preparation memory, apply relocations, and
//! only then install the final permission intents returned here.

/// Maximum ELF file size admitted by this foundation.
pub const MAX_FILE_BYTES: usize = 64 * 1024 * 1024;
/// Maximum program-header count.
pub const MAX_PROGRAM_HEADERS: usize = 32;
/// Maximum PT_LOAD count.
pub const MAX_LOAD_SEGMENTS: usize = 16;
/// Maximum direct DT_NEEDED count.
pub const MAX_NEEDED: usize = 16;
/// Maximum dynamic-table entries.
pub const MAX_DYNAMIC_ENTRIES: usize = 256;
/// Maximum dynamic symbol count derived from an admitted hash table.
pub const MAX_SYMBOLS: usize = 4096;
/// Maximum total RELA records, including PLT relocations.
pub const MAX_RELOCATIONS: usize = 256;
/// Maximum objects in one deterministic dependency scope.
pub const MAX_DEPENDENCY_OBJECTS: usize = 16;
/// Exact authenticated dependency-catalog byte count.
pub const CATALOG_BYTES: usize = 2048;
/// Maximum constructor or destructor addresses emitted for one process.
pub const MAX_LIFECYCLE_FUNCTIONS: usize = 256;
/// Maximum mapped span of one object.
pub const MAX_IMAGE_SPAN: u64 = 256 * 1024 * 1024;

const PAGE: u64 = 4096;
const MAX_ALIGNMENT: u64 = 2 * 1024 * 1024;
const MAX_LINK_ADDRESS: u64 = 0x0000_0001_0000_0000;
const MAX_RUNTIME_ADDRESS: u64 = 0x0000_8000_0000_0000;
const MAX_STRING_BYTES: u64 = 1024 * 1024;
const MAX_ARRAY_ENTRIES: u64 = 256;
const MAX_TLS_BYTES: u64 = 1024 * 1024;
const CATALOG_MAGIC: [u8; 8] = *b"PHIPDYN1";
const CATALOG_HEADER_BYTES: usize = 64;
const CATALOG_ENTRY_BYTES: usize = 96;

const PT_NULL: u32 = 0;
const PT_LOAD: u32 = 1;
const PT_DYNAMIC: u32 = 2;
const PT_INTERP: u32 = 3;
const PT_NOTE: u32 = 4;
const PT_PHDR: u32 = 6;
const PT_TLS: u32 = 7;
const PT_GNU_EH_FRAME: u32 = 0x6474_e550;
const PT_GNU_STACK: u32 = 0x6474_e551;
const PT_GNU_RELRO: u32 = 0x6474_e552;

const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;

const DT_NULL: i64 = 0;
const DT_NEEDED: i64 = 1;
const DT_PLTRELSZ: i64 = 2;
const DT_PLTGOT: i64 = 3;
const DT_HASH: i64 = 4;
const DT_STRTAB: i64 = 5;
const DT_SYMTAB: i64 = 6;
const DT_RELA: i64 = 7;
const DT_RELASZ: i64 = 8;
const DT_RELAENT: i64 = 9;
const DT_STRSZ: i64 = 10;
const DT_SYMENT: i64 = 11;
const DT_INIT: i64 = 12;
const DT_FINI: i64 = 13;
const DT_SONAME: i64 = 14;
const DT_DEBUG: i64 = 21;
const DT_JMPREL: i64 = 23;
const DT_BIND_NOW: i64 = 24;
const DT_INIT_ARRAY: i64 = 25;
const DT_FINI_ARRAY: i64 = 26;
const DT_INIT_ARRAYSZ: i64 = 27;
const DT_FINI_ARRAYSZ: i64 = 28;
const DT_FLAGS: i64 = 30;
const DT_GNU_HASH: i64 = 0x6fff_fef5;
const DT_RELACOUNT: i64 = 0x6fff_fff9;
const DT_FLAGS_1: i64 = 0x6fff_fffb;
const DT_PLTREL: i64 = 20;
const DT_RELR: i64 = 36;
const DT_RELRSZ: i64 = 35;
const DT_RELRENT: i64 = 37;

const DF_BIND_NOW: u64 = 0x8;
const DF_STATIC_TLS: u64 = 0x10;
const DF_1_NOW: u64 = 0x1;
const DF_1_PIE: u64 = 0x0800_0000;

const R_X86_64_NONE: u32 = 0;
const R_X86_64_64: u32 = 1;
const R_X86_64_PC32: u32 = 2;
const R_X86_64_GLOB_DAT: u32 = 6;
const R_X86_64_JUMP_SLOT: u32 = 7;
const R_X86_64_RELATIVE: u32 = 8;
const R_X86_64_TPOFF64: u32 = 18;

const SHN_UNDEF: u16 = 0;
const SHN_ABS: u16 = 0xfff1;
const STB_LOCAL: u8 = 0;
const STB_GLOBAL: u8 = 1;
const STB_WEAK: u8 = 2;
const STT_NOTYPE: u8 = 0;
const STT_OBJECT: u8 = 1;
const STT_FUNC: u8 = 2;
const STT_TLS: u8 = 6;
const STV_DEFAULT: u8 = 0;
const STV_PROTECTED: u8 = 3;

/// Named, bounded admission or relocation refusal.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    /// The requested operation completed.
    Ok = 0,
    /// A null pointer crossed the C boundary.
    NullArgument,
    Length,
    Magic,
    Identity,
    Type,
    Machine,
    Header,
    ProgramTable,
    ProgramType,
    ProgramFlags,
    FileRange,
    LoadSize,
    Alignment,
    Address,
    Overlap,
    Entry,
    Stack,
    DynamicSegment,
    DynamicEntry,
    DynamicDuplicate,
    DynamicMissing,
    DynamicUnsupported,
    StringTable,
    String,
    SymbolTable,
    Symbol,
    HashTable,
    RelocationTable,
    RelocationType,
    RelocationTarget,
    RelocationOverflow,
    MemorySize,
    UndefinedSymbol,
    DependencyMissing,
    DependencyAmbiguous,
    DependencyCycle,
    DependencyBound,
    /// A catalog or object digest does not match its authenticated record.
    Authentication,
    /// A dependency catalog is malformed or noncanonical.
    Catalog,
    /// Constructor or destructor metadata is not executable and bounded.
    Lifecycle,
    /// One past the final stable status value.
    Count,
}

/// One bounded ELF string copied into pointer-free admission output.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Name {
    bytes: [u8; 64],
    length: u8,
}

impl Name {
    const fn empty() -> Self {
        Self {
            bytes: [0; 64],
            length: 0,
        }
    }

    /// Exact non-NUL bytes.
    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes[..usize::from(self.length)]
    }

    /// Whether this is the absent optional name.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.length == 0
    }
}

/// One admitted load segment and its final W^X intent.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Segment {
    pub file_offset: u64,
    pub virtual_address: u64,
    pub file_size: u64,
    pub memory_size: u64,
    pub mapping_start: u64,
    pub mapping_end: u64,
    pub flags: u32,
    /// Explicit zero padding in the C boundary.
    pub reserved: u32,
}

impl Segment {
    const fn empty() -> Self {
        Self {
            file_offset: 0,
            virtual_address: 0,
            file_size: 0,
            memory_size: 0,
            mapping_start: 0,
            mapping_end: 0,
            flags: 0,
            reserved: 0,
        }
    }
}

/// Optional initial-exec TLS template in one dynamic object.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Tls {
    pub file_offset: u64,
    pub virtual_address: u64,
    pub file_size: u64,
    pub memory_size: u64,
    pub alignment: u64,
}

impl Tls {
    const fn empty() -> Self {
        Self {
            file_offset: 0,
            virtual_address: 0,
            file_size: 0,
            memory_size: 0,
            alignment: 0,
        }
    }
}

/// One catalog-bound SONAME and exact object digest.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CatalogEntry {
    pub name: Name,
    pub sha256: [u8; 32],
}

impl CatalogEntry {
    const fn empty() -> Self {
        Self {
            name: Name::empty(),
            sha256: [0; 32],
        }
    }
}

/// Pointer-free authenticated System dependency catalog.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Catalog {
    pub entries: [CatalogEntry; MAX_DEPENDENCY_OBJECTS],
    pub entry_count: u8,
    pub reserved: [u8; 7],
}

impl Catalog {
    /// Stable all-zero result for a refused boundary call.
    pub const fn empty() -> Self {
        Self {
            entries: [CatalogEntry::empty(); MAX_DEPENDENCY_OBJECTS],
            entry_count: 0,
            reserved: [0; 7],
        }
    }
}

/// One C-owned admitted object and its private preparation allocation.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PreparedObject {
    pub image: *const Image,
    pub input: *const u8,
    pub input_length: usize,
    pub memory: *mut u8,
    pub memory_length: usize,
    pub load_bias: u64,
    pub tls_offset: i64,
}

/// Fully validated Ring 3 lifecycle call sequence.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Lifecycle {
    pub constructors: [u64; MAX_LIFECYCLE_FUNCTIONS],
    pub destructors: [u64; MAX_LIFECYCLE_FUNCTIONS],
    pub constructor_count: u16,
    pub destructor_count: u16,
    pub reserved: u32,
}

impl Lifecycle {
    /// Stable all-zero result for a refused boundary call.
    pub const fn empty() -> Self {
        Self {
            constructors: [0; MAX_LIFECYCLE_FUNCTIONS],
            destructors: [0; MAX_LIFECYCLE_FUNCTIONS],
            constructor_count: 0,
            destructor_count: 0,
            reserved: 0,
        }
    }
}

/// Final mapping intent after all relocations have been applied in private preparation memory.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PermissionIntent {
    Read,
    ReadExecute,
    ReadWrite,
}

/// Selected ELF hash-table family.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HashStyle {
    SysV,
    Gnu,
}

/// One validated dynamic symbol.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Symbol {
    pub name: Name,
    pub value: u64,
    pub size: u64,
    pub binding: u8,
    pub kind: u8,
    pub visibility: u8,
    pub section: u16,
}

/// Pointer-free admission result for one ET_DYN object.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Image {
    pub entry: u64,
    pub mapping_start: u64,
    pub mapping_end: u64,
    pub segments: [Segment; MAX_LOAD_SEGMENTS],
    pub segment_count: u8,
    pub soname: Name,
    pub needed: [Name; MAX_NEEDED],
    pub needed_count: u8,
    pub string_address: u64,
    pub string_size: u64,
    pub symbol_address: u64,
    pub symbol_count: u32,
    pub hash_style: HashStyle,
    pub sysv_hash_address: u64,
    pub gnu_hash_address: u64,
    pub rela_address: u64,
    pub rela_count: u32,
    pub plt_rela_address: u64,
    pub plt_rela_count: u32,
    pub relative_count: u32,
    pub relro_start: u64,
    pub relro_end: u64,
    pub init: u64,
    pub fini: u64,
    pub init_array: u64,
    pub init_array_count: u32,
    pub fini_array: u64,
    pub fini_array_count: u32,
    pub tls: Tls,
    pub bind_now: bool,
}

impl Image {
    /// Exact mapped span required by [`load_image`].
    #[must_use]
    pub fn memory_bytes(&self) -> usize {
        usize::try_from(self.mapping_end - self.mapping_start).unwrap_or(usize::MAX)
    }

    /// Final permission intent at one admitted link-time address.
    #[must_use]
    pub fn permission_intent(&self, address: u64) -> Option<PermissionIntent> {
        for segment in &self.segments[..usize::from(self.segment_count)] {
            let end = segment.virtual_address.checked_add(segment.memory_size)?;
            if address < segment.virtual_address || address >= end {
                continue;
            }
            if address >= self.relro_start && address < self.relro_end {
                return Some(PermissionIntent::Read);
            }
            return Some(if segment.flags == (PF_R | PF_X) {
                PermissionIntent::ReadExecute
            } else if segment.flags == (PF_R | PF_W) {
                PermissionIntent::ReadWrite
            } else {
                PermissionIntent::Read
            });
        }
        None
    }
}

/// One authenticated object in deterministic global symbol-search order.
#[derive(Clone, Copy)]
pub struct Object<'a> {
    pub image: &'a Image,
    pub file: &'a [u8],
    pub load_bias: u64,
    /// Signed offset from the variant-II thread pointer to this TLS block.
    pub tls_offset: i64,
}

#[derive(Clone, Copy)]
struct Program {
    kind: u32,
    flags: u32,
    offset: u64,
    address: u64,
    physical: u64,
    file_size: u64,
    memory_size: u64,
    alignment: u64,
}

#[derive(Clone, Copy)]
struct RawSymbol {
    name: u32,
    info: u8,
    other: u8,
    section: u16,
    value: u64,
    size: u64,
}

#[derive(Clone, Copy)]
struct Rela {
    offset: u64,
    symbol: u32,
    kind: u32,
    addend: i64,
}

fn bytes(input: &[u8], offset: usize, size: usize, error: Status) -> Result<&[u8], Status> {
    let end = offset.checked_add(size).ok_or(error)?;
    input.get(offset..end).ok_or(error)
}

fn u16_at(input: &[u8], offset: usize, error: Status) -> Result<u16, Status> {
    Ok(u16::from_le_bytes(
        bytes(input, offset, 2, error)?
            .try_into()
            .map_err(|_| error)?,
    ))
}

fn u32_at(input: &[u8], offset: usize, error: Status) -> Result<u32, Status> {
    Ok(u32::from_le_bytes(
        bytes(input, offset, 4, error)?
            .try_into()
            .map_err(|_| error)?,
    ))
}

fn u64_at(input: &[u8], offset: usize, error: Status) -> Result<u64, Status> {
    Ok(u64::from_le_bytes(
        bytes(input, offset, 8, error)?
            .try_into()
            .map_err(|_| error)?,
    ))
}

fn i64_at(input: &[u8], offset: usize, error: Status) -> Result<i64, Status> {
    Ok(i64::from_le_bytes(
        bytes(input, offset, 8, error)?
            .try_into()
            .map_err(|_| error)?,
    ))
}

fn program(input: &[u8], offset: usize) -> Result<Program, Status> {
    Ok(Program {
        kind: u32_at(input, offset, Status::ProgramTable)?,
        flags: u32_at(input, offset + 4, Status::ProgramTable)?,
        offset: u64_at(input, offset + 8, Status::ProgramTable)?,
        address: u64_at(input, offset + 16, Status::ProgramTable)?,
        physical: u64_at(input, offset + 24, Status::ProgramTable)?,
        file_size: u64_at(input, offset + 32, Status::ProgramTable)?,
        memory_size: u64_at(input, offset + 40, Status::ProgramTable)?,
        alignment: u64_at(input, offset + 48, Status::ProgramTable)?,
    })
}

fn singleton(slot: &mut Option<u64>, value: u64) -> Result<(), Status> {
    if slot.replace(value).is_some() {
        return Err(Status::DynamicDuplicate);
    }
    Ok(())
}

fn loaded_range(image: &Image, address: u64, size: u64, writable: bool) -> bool {
    let Some(end) = address.checked_add(size) else {
        return false;
    };
    image.segments[..usize::from(image.segment_count)]
        .iter()
        .any(|segment| {
            let Some(segment_end) = segment.virtual_address.checked_add(segment.memory_size) else {
                return false;
            };
            address >= segment.virtual_address
                && end <= segment_end
                && (!writable || segment.flags == (PF_R | PF_W))
        })
}

fn executable_address(image: &Image, address: u64) -> bool {
    image.segments[..usize::from(image.segment_count)]
        .iter()
        .any(|segment| {
            segment.flags == (PF_R | PF_X)
                && address >= segment.virtual_address
                && segment
                    .virtual_address
                    .checked_add(segment.memory_size)
                    .is_some_and(|end| address < end)
        })
}

fn virtual_file_offset(image: &Image, address: u64, size: u64) -> Result<usize, Status> {
    let end = address.checked_add(size).ok_or(Status::FileRange)?;
    for segment in &image.segments[..usize::from(image.segment_count)] {
        let file_end = segment
            .virtual_address
            .checked_add(segment.file_size)
            .ok_or(Status::FileRange)?;
        if address >= segment.virtual_address && end <= file_end {
            let offset = segment
                .file_offset
                .checked_add(address - segment.virtual_address)
                .ok_or(Status::FileRange)?;
            return usize::try_from(offset).map_err(|_| Status::FileRange);
        }
    }
    Err(Status::FileRange)
}

fn string<'a>(
    image: &Image,
    input: &'a [u8],
    offset: u64,
    allow_empty: bool,
) -> Result<&'a [u8], Status> {
    if offset >= image.string_size {
        return Err(Status::String);
    }
    let address = image
        .string_address
        .checked_add(offset)
        .ok_or(Status::String)?;
    let remaining = image.string_size - offset;
    let start = virtual_file_offset(image, address, remaining)?;
    let field = bytes(
        input,
        start,
        usize::try_from(remaining).map_err(|_| Status::String)?,
        Status::String,
    )?;
    let end = field
        .iter()
        .position(|byte| *byte == 0)
        .ok_or(Status::String)?;
    if (!allow_empty && end == 0) || end >= 64 {
        return Err(Status::String);
    }
    Ok(&field[..end])
}

fn copied_name(value: &[u8]) -> Result<Name, Status> {
    if value.is_empty() || value.len() >= 64 || value.iter().any(|byte| *byte == 0) {
        return Err(Status::String);
    }
    let mut result = Name::empty();
    result.bytes[..value.len()].copy_from_slice(value);
    result.length = u8::try_from(value.len()).map_err(|_| Status::String)?;
    Ok(result)
}

fn library_name(value: &[u8]) -> Result<Name, Status> {
    if value == b"."
        || value == b".."
        || value.iter().any(|byte| {
            !byte.is_ascii_alphanumeric() && !matches!(*byte, b'.' | b'_' | b'+' | b'-')
        })
    {
        return Err(Status::String);
    }
    copied_name(value)
}

/// Parse one exact, digest-authenticated dependency catalog.
pub fn parse_catalog(input: &[u8]) -> Result<Catalog, Status> {
    if input.len() != CATALOG_BYTES || input.get(..8) != Some(CATALOG_MAGIC.as_slice()) {
        return Err(Status::Catalog);
    }
    if u16_at(input, 8, Status::Catalog)? != 1
        || usize::from(u16_at(input, 10, Status::Catalog)?) != CATALOG_HEADER_BYTES
        || usize::try_from(u32_at(input, 12, Status::Catalog)?).map_err(|_| Status::Catalog)?
            != CATALOG_BYTES
        || usize::from(u16_at(input, 18, Status::Catalog)?) != CATALOG_ENTRY_BYTES
        || input[20..CATALOG_HEADER_BYTES]
            .iter()
            .any(|byte| *byte != 0)
    {
        return Err(Status::Catalog);
    }
    let count = usize::from(u16_at(input, 16, Status::Catalog)?);
    if count == 0 || count > MAX_DEPENDENCY_OBJECTS {
        return Err(Status::Catalog);
    }
    let used_end = CATALOG_HEADER_BYTES
        .checked_add(count.checked_mul(CATALOG_ENTRY_BYTES).ok_or(Status::Catalog)?)
        .ok_or(Status::Catalog)?;
    if input[used_end..].iter().any(|byte| *byte != 0) {
        return Err(Status::Catalog);
    }
    let mut result = Catalog::empty();
    for index in 0..count {
        let offset = CATALOG_HEADER_BYTES + index * CATALOG_ENTRY_BYTES;
        let field = bytes(input, offset, 64, Status::Catalog)?;
        let end = field.iter().position(|byte| *byte == 0).ok_or(Status::Catalog)?;
        if end == 0 || field[end..].iter().any(|byte| *byte != 0) {
            return Err(Status::Catalog);
        }
        let name = library_name(&field[..end]).map_err(|_| Status::Catalog)?;
        if index != 0 && result.entries[index - 1].name.as_bytes() >= name.as_bytes() {
            return Err(Status::Catalog);
        }
        let digest: [u8; 32] = bytes(input, offset + 64, 32, Status::Catalog)?
            .try_into()
            .map_err(|_| Status::Catalog)?;
        if digest.iter().all(|byte| *byte == 0) {
            return Err(Status::Catalog);
        }
        result.entries[index] = CatalogEntry {
            name,
            sha256: digest,
        };
    }
    result.entry_count = u8::try_from(count).map_err(|_| Status::Catalog)?;
    Ok(result)
}

fn raw_symbol(image: &Image, input: &[u8], index: u32) -> Result<RawSymbol, Status> {
    if index >= image.symbol_count {
        return Err(Status::Symbol);
    }
    let byte_offset = u64::from(index)
        .checked_mul(24)
        .ok_or(Status::SymbolTable)?;
    let address = image
        .symbol_address
        .checked_add(byte_offset)
        .ok_or(Status::SymbolTable)?;
    let offset = virtual_file_offset(image, address, 24)?;
    Ok(RawSymbol {
        name: u32_at(input, offset, Status::SymbolTable)?,
        info: *bytes(input, offset + 4, 1, Status::SymbolTable)?
            .first()
            .ok_or(Status::SymbolTable)?,
        other: *bytes(input, offset + 5, 1, Status::SymbolTable)?
            .first()
            .ok_or(Status::SymbolTable)?,
        section: u16_at(input, offset + 6, Status::SymbolTable)?,
        value: u64_at(input, offset + 8, Status::SymbolTable)?,
        size: u64_at(input, offset + 16, Status::SymbolTable)?,
    })
}

fn checked_symbol(image: &Image, input: &[u8], index: u32) -> Result<Symbol, Status> {
    let raw = raw_symbol(image, input, index)?;
    let binding = raw.info >> 4;
    let kind = raw.info & 0xf;
    let visibility = raw.other & 0x3;
    if !matches!(binding, STB_LOCAL | STB_GLOBAL | STB_WEAK)
        || !matches!(kind, STT_NOTYPE | STT_OBJECT | STT_FUNC | STT_TLS)
        || raw.other & !0x3 != 0
    {
        return Err(Status::Symbol);
    }
    let name = if index == 0 && raw.name == 0 {
        Name::empty()
    } else {
        copied_name(string(image, input, u64::from(raw.name), false)?)?
    };
    if raw.section != SHN_UNDEF && raw.section != SHN_ABS {
        if kind == STT_TLS {
            if image.tls.memory_size == 0
                || raw
                    .value
                    .checked_add(raw.size.max(1))
                    .is_none_or(|end| end > image.tls.memory_size)
            {
                return Err(Status::Symbol);
            }
        } else if !loaded_range(image, raw.value, raw.size.max(1), false) {
            return Err(Status::Symbol);
        }
    } else if kind == STT_TLS && raw.section == SHN_ABS {
        return Err(Status::Symbol);
    }
    if index != 0
        && raw.section == SHN_UNDEF
        && (raw.value != 0 || binding == STB_LOCAL || visibility != STV_DEFAULT)
    {
        return Err(Status::Symbol);
    }
    Ok(Symbol {
        name,
        value: raw.value,
        size: raw.size,
        binding,
        kind,
        visibility,
        section: raw.section,
    })
}

fn sysv_hash(name: &[u8]) -> u32 {
    let mut hash = 0u32;
    for byte in name {
        hash = hash.wrapping_shl(4).wrapping_add(u32::from(*byte));
        let high = hash & 0xf000_0000;
        if high != 0 {
            hash ^= high >> 24;
        }
        hash &= !high;
    }
    hash
}

fn gnu_hash(name: &[u8]) -> u32 {
    let mut hash = 5381u32;
    for byte in name {
        hash = hash.wrapping_mul(33).wrapping_add(u32::from(*byte));
    }
    hash
}

fn sysv_symbol_count(image: &Image, input: &[u8], address: u64) -> Result<u32, Status> {
    let header = virtual_file_offset(image, address, 8)?;
    let buckets = u32_at(input, header, Status::HashTable)?;
    let chains = u32_at(input, header + 4, Status::HashTable)?;
    if buckets == 0
        || usize::try_from(buckets).map_err(|_| Status::HashTable)? > MAX_SYMBOLS
        || chains == 0
        || usize::try_from(chains).map_err(|_| Status::HashTable)? > MAX_SYMBOLS
    {
        return Err(Status::HashTable);
    }
    let words = u64::from(buckets)
        .checked_add(u64::from(chains))
        .and_then(|value| value.checked_add(2))
        .ok_or(Status::HashTable)?;
    let table_bytes = words.checked_mul(4).ok_or(Status::HashTable)?;
    let table = virtual_file_offset(image, address, table_bytes)?;
    let bucket_start = table + 8;
    let chain_start = bucket_start
        .checked_add(usize::try_from(buckets).map_err(|_| Status::HashTable)? * 4)
        .ok_or(Status::HashTable)?;
    for index in 0..buckets {
        if u32_at(
            input,
            bucket_start + usize::try_from(index).map_err(|_| Status::HashTable)? * 4,
            Status::HashTable,
        )? >= chains
        {
            return Err(Status::HashTable);
        }
    }
    for index in 0..chains {
        if u32_at(
            input,
            chain_start + usize::try_from(index).map_err(|_| Status::HashTable)? * 4,
            Status::HashTable,
        )? >= chains
        {
            return Err(Status::HashTable);
        }
    }
    for bucket in 0..buckets {
        let mut symbol = u32_at(
            input,
            bucket_start + usize::try_from(bucket).map_err(|_| Status::HashTable)? * 4,
            Status::HashTable,
        )?;
        for _ in 0..chains {
            if symbol == 0 {
                break;
            }
            let chain = chain_start
                .checked_add(usize::try_from(symbol).map_err(|_| Status::HashTable)? * 4)
                .ok_or(Status::HashTable)?;
            symbol = u32_at(input, chain, Status::HashTable)?;
        }
        if symbol != 0 {
            return Err(Status::HashTable);
        }
    }
    Ok(chains)
}

fn gnu_layout(image: &Image, input: &[u8], address: u64) -> Result<(u32, u32, u32, u64), Status> {
    let header = virtual_file_offset(image, address, 16)?;
    let buckets = u32_at(input, header, Status::HashTable)?;
    let symbol_offset = u32_at(input, header + 4, Status::HashTable)?;
    let bloom_size = u32_at(input, header + 8, Status::HashTable)?;
    let bloom_shift = u32_at(input, header + 12, Status::HashTable)?;
    if buckets == 0
        || bloom_size == 0
        || !bloom_size.is_power_of_two()
        || bloom_shift >= 64
        || usize::try_from(buckets).map_err(|_| Status::HashTable)? > MAX_SYMBOLS
        || symbol_offset == 0
        || usize::try_from(symbol_offset).map_err(|_| Status::HashTable)? >= MAX_SYMBOLS
        || usize::try_from(bloom_size).map_err(|_| Status::HashTable)? > MAX_SYMBOLS
    {
        return Err(Status::HashTable);
    }
    let bucket_address = address
        .checked_add(16)
        .and_then(|value| value.checked_add(u64::from(bloom_size) * 8))
        .ok_or(Status::HashTable)?;
    let _ = virtual_file_offset(image, bucket_address, u64::from(buckets) * 4)?;
    Ok((buckets, symbol_offset, bloom_shift, bucket_address))
}

fn gnu_symbol_count(image: &Image, input: &[u8], address: u64) -> Result<u32, Status> {
    let (buckets, symbol_offset, _, bucket_address) = gnu_layout(image, input, address)?;
    let chain_address = bucket_address
        .checked_add(u64::from(buckets) * 4)
        .ok_or(Status::HashTable)?;
    let mut maximum = symbol_offset;
    for bucket_index in 0..buckets {
        let offset = virtual_file_offset(image, bucket_address + u64::from(bucket_index) * 4, 4)?;
        let first = u32_at(input, offset, Status::HashTable)?;
        if first == 0 {
            continue;
        }
        if first < symbol_offset
            || usize::try_from(first).map_err(|_| Status::HashTable)? >= MAX_SYMBOLS
        {
            return Err(Status::HashTable);
        }
        let mut symbol = first;
        loop {
            if usize::try_from(symbol).map_err(|_| Status::HashTable)? >= MAX_SYMBOLS {
                return Err(Status::HashTable);
            }
            let chain = chain_address
                .checked_add(u64::from(symbol - symbol_offset) * 4)
                .ok_or(Status::HashTable)?;
            let chain_offset = virtual_file_offset(image, chain, 4)?;
            let value = u32_at(input, chain_offset, Status::HashTable)?;
            maximum = maximum.max(symbol.checked_add(1).ok_or(Status::HashTable)?);
            if value & 1 != 0 {
                break;
            }
            symbol = symbol.checked_add(1).ok_or(Status::HashTable)?;
        }
    }
    if maximum == 0 {
        return Err(Status::HashTable);
    }
    Ok(maximum)
}

fn rela(image: &Image, input: &[u8], address: u64, index: u32) -> Result<Rela, Status> {
    let entry_address = address
        .checked_add(u64::from(index) * 24)
        .ok_or(Status::RelocationTable)?;
    let offset = virtual_file_offset(image, entry_address, 24)?;
    let info = u64_at(input, offset + 8, Status::RelocationTable)?;
    Ok(Rela {
        offset: u64_at(input, offset, Status::RelocationTable)?,
        symbol: u32::try_from(info >> 32).map_err(|_| Status::RelocationTable)?,
        kind: info as u32,
        addend: i64_at(input, offset + 16, Status::RelocationTable)?,
    })
}

fn validate_relocations(image: &Image, input: &[u8]) -> Result<(), Status> {
    let total = image
        .rela_count
        .checked_add(image.plt_rela_count)
        .ok_or(Status::RelocationTable)?;
    if usize::try_from(total).map_err(|_| Status::RelocationTable)? > MAX_RELOCATIONS {
        return Err(Status::RelocationTable);
    }
    let mut targets = [0u64; MAX_RELOCATIONS];
    let mut widths = [0u8; MAX_RELOCATIONS];
    let mut used = 0usize;
    for table in 0..2u8 {
        let (address, count) = if table == 0 {
            (image.rela_address, image.rela_count)
        } else {
            (image.plt_rela_address, image.plt_rela_count)
        };
        for index in 0..count {
            let item = rela(image, input, address, index)?;
            let width = if item.kind == R_X86_64_PC32 { 4 } else { 8 };
            if table == 0 && index < image.relative_count && item.kind != R_X86_64_RELATIVE {
                return Err(Status::RelocationType);
            }
            if item.kind == R_X86_64_NONE {
                if item.offset != 0 || item.symbol != 0 || item.addend != 0 {
                    return Err(Status::RelocationType);
                }
                continue;
            }
            if !matches!(
                item.kind,
                R_X86_64_64
                    | R_X86_64_PC32
                    | R_X86_64_GLOB_DAT
                    | R_X86_64_JUMP_SLOT
                    | R_X86_64_RELATIVE
                    | R_X86_64_TPOFF64
            ) {
                return Err(Status::RelocationType);
            }
            if table == 1 && item.kind != R_X86_64_JUMP_SLOT {
                return Err(Status::RelocationType);
            }
            if item.kind == R_X86_64_RELATIVE && item.symbol != 0
                || item.kind != R_X86_64_RELATIVE
                    && item.kind != R_X86_64_TPOFF64
                    && item.symbol == 0
                || item.kind == R_X86_64_TPOFF64
                    && item.symbol == 0
                    && (image.tls.memory_size == 0
                        || item.addend < 0
                        || item.addend as u64 >= image.tls.memory_size)
                || item.symbol >= image.symbol_count
                || !loaded_range(image, item.offset, width, true)
            {
                return Err(Status::RelocationTarget);
            }
            let target_end = item
                .offset
                .checked_add(width)
                .ok_or(Status::RelocationTarget)?;
            for prior in 0..used {
                let prior_end = targets[prior]
                    .checked_add(u64::from(widths[prior]))
                    .ok_or(Status::RelocationTarget)?;
                if item.offset < prior_end && targets[prior] < target_end {
                    return Err(Status::RelocationTarget);
                }
            }
            targets[used] = item.offset;
            widths[used] = width as u8;
            used += 1;
            if item.symbol != 0 {
                let symbol = checked_symbol(image, input, item.symbol)?;
                if symbol.name.is_empty()
                    || (item.kind == R_X86_64_TPOFF64) != (symbol.kind == STT_TLS)
                {
                    return Err(Status::Symbol);
                }
            }
        }
    }
    Ok(())
}

fn validate_sections(input: &[u8]) -> Result<(), Status> {
    let offset = u64_at(input, 40, Status::Header)?;
    let entry_size = u16_at(input, 58, Status::Header)?;
    let count = u16_at(input, 60, Status::Header)?;
    let names = u16_at(input, 62, Status::Header)?;
    if count == 0 {
        if offset != 0 || names != 0 || !matches!(entry_size, 0 | 64) {
            return Err(Status::Header);
        }
        return Ok(());
    }
    if entry_size != 64 || count > 256 || names >= count {
        return Err(Status::Header);
    }
    let table_bytes = u64::from(count).checked_mul(64).ok_or(Status::Header)?;
    let end = offset.checked_add(table_bytes).ok_or(Status::Header)?;
    if end > input.len() as u64 {
        return Err(Status::Header);
    }
    for index in 0..u64::from(count) {
        let position = usize::try_from(offset + index * 64).map_err(|_| Status::Header)?;
        let kind = u32_at(input, position + 4, Status::Header)?;
        let section_offset = u64_at(input, position + 24, Status::Header)?;
        let size = u64_at(input, position + 32, Status::Header)?;
        if kind != 8 && section_offset.checked_add(size).ok_or(Status::Header)? > input.len() as u64
        {
            return Err(Status::Header);
        }
    }
    Ok(())
}

/// Strictly parse one x86-64 ET_DYN object without allocating.
pub fn parse(input: &[u8]) -> Result<Image, Status> {
    if input.is_empty() || input.len() > MAX_FILE_BYTES {
        return Err(Status::Length);
    }
    if input.get(..4) != Some(b"\x7fELF") {
        return Err(Status::Magic);
    }
    if input.get(4..9) != Some([2u8, 1, 1, 0, 0].as_slice())
        || input
            .get(9..16)
            .ok_or(Status::Identity)?
            .iter()
            .any(|byte| *byte != 0)
    {
        return Err(Status::Identity);
    }
    if u16_at(input, 16, Status::Header)? != 3 {
        return Err(Status::Type);
    }
    if u16_at(input, 18, Status::Header)? != 62 {
        return Err(Status::Machine);
    }
    if u32_at(input, 20, Status::Header)? != 1
        || u32_at(input, 48, Status::Header)? != 0
        || u16_at(input, 52, Status::Header)? != 64
        || u16_at(input, 54, Status::Header)? != 56
    {
        return Err(Status::Header);
    }
    validate_sections(input)?;
    let entry = u64_at(input, 24, Status::Header)?;
    let table_offset = u64_at(input, 32, Status::ProgramTable)?;
    let program_count = usize::from(u16_at(input, 56, Status::ProgramTable)?);
    if program_count == 0 || program_count > MAX_PROGRAM_HEADERS {
        return Err(Status::ProgramTable);
    }
    let table_bytes = u64::try_from(program_count)
        .map_err(|_| Status::ProgramTable)?
        .checked_mul(56)
        .ok_or(Status::ProgramTable)?;
    if table_offset < 64
        || table_offset
            .checked_add(table_bytes)
            .ok_or(Status::ProgramTable)?
            > input.len() as u64
    {
        return Err(Status::ProgramTable);
    }

    let mut segments = [Segment::empty(); MAX_LOAD_SEGMENTS];
    let mut segment_count = 0usize;
    let mut stack_count = 0usize;
    let mut dynamic_program: Option<Program> = None;
    let mut relro_start = 0u64;
    let mut relro_end = 0u64;
    let mut relro_exact_start = 0u64;
    let mut relro_exact_end = 0u64;
    let mut relro_offset = 0u64;
    let mut relro_file_size = 0u64;
    let mut tls = Tls::empty();
    let mut mapping_start = u64::MAX;
    let mut mapping_end = 0u64;
    for index in 0..program_count {
        let position = usize::try_from(table_offset)
            .map_err(|_| Status::ProgramTable)?
            .checked_add(index * 56)
            .ok_or(Status::ProgramTable)?;
        let item = program(input, position)?;
        match item.kind {
            PT_NULL => {
                if item.flags != 0
                    || item.offset != 0
                    || item.address != 0
                    || item.physical != 0
                    || item.file_size != 0
                    || item.memory_size != 0
                    || item.alignment != 0
                {
                    return Err(Status::ProgramType);
                }
            }
            PT_LOAD => {
                if segment_count == MAX_LOAD_SEGMENTS {
                    return Err(Status::ProgramTable);
                }
                if item.flags != PF_R && item.flags != (PF_R | PF_X) && item.flags != (PF_R | PF_W)
                    || item.flags & (PF_W | PF_X) == (PF_W | PF_X)
                {
                    return Err(Status::ProgramFlags);
                }
                if item.memory_size == 0 || item.file_size > item.memory_size {
                    return Err(Status::LoadSize);
                }
                if item
                    .offset
                    .checked_add(item.file_size)
                    .ok_or(Status::FileRange)?
                    > input.len() as u64
                {
                    return Err(Status::FileRange);
                }
                if item.alignment < PAGE
                    || item.alignment > MAX_ALIGNMENT
                    || !item.alignment.is_power_of_two()
                    || item.offset & (item.alignment - 1) != item.address & (item.alignment - 1)
                {
                    return Err(Status::Alignment);
                }
                if item.physical != 0 && item.physical != item.address {
                    return Err(Status::Address);
                }
                let virtual_end = item
                    .address
                    .checked_add(item.memory_size)
                    .ok_or(Status::Address)?;
                let page_start = item.address & !(PAGE - 1);
                let page_end =
                    virtual_end.checked_add(PAGE - 1).ok_or(Status::Address)? & !(PAGE - 1);
                if virtual_end <= item.address || page_end > MAX_LINK_ADDRESS {
                    return Err(Status::Address);
                }
                if segment_count != 0 && item.address < segments[segment_count - 1].virtual_address
                {
                    return Err(Status::ProgramTable);
                }
                for prior in &segments[..segment_count] {
                    let prior_end = prior
                        .virtual_address
                        .checked_add(prior.memory_size)
                        .ok_or(Status::Overlap)?;
                    if item.address < prior_end && prior.virtual_address < virtual_end
                        || page_start < prior.mapping_end && prior.mapping_start < page_end
                    {
                        return Err(Status::Overlap);
                    }
                }
                segments[segment_count] = Segment {
                    file_offset: item.offset,
                    virtual_address: item.address,
                    file_size: item.file_size,
                    memory_size: item.memory_size,
                    mapping_start: page_start,
                    mapping_end: page_end,
                    flags: item.flags,
                    reserved: 0,
                };
                segment_count += 1;
                mapping_start = mapping_start.min(page_start);
                mapping_end = mapping_end.max(page_end);
            }
            PT_DYNAMIC => {
                if dynamic_program.replace(item).is_some() {
                    return Err(Status::DynamicSegment);
                }
            }
            PT_GNU_STACK => {
                if stack_count != 0
                    || item.flags != (PF_R | PF_W)
                    || item.offset != 0
                    || item.address != 0
                    || item.physical != 0
                    || item.file_size != 0
                    || item.memory_size != 0
                    || item.alignment > 16
                {
                    return Err(Status::Stack);
                }
                stack_count += 1;
            }
            PT_GNU_RELRO => {
                if relro_end != 0
                    || item.flags != PF_R
                    || item.file_size > item.memory_size
                    || item.memory_size == 0
                    || item.physical != 0 && item.physical != item.address
                    || item.alignment > PAGE
                    || item
                        .offset
                        .checked_add(item.file_size)
                        .ok_or(Status::FileRange)?
                        > input.len() as u64
                {
                    return Err(Status::ProgramType);
                }
                relro_offset = item.offset;
                relro_file_size = item.file_size;
                relro_start = item.address & !(PAGE - 1);
                relro_exact_start = item.address;
                relro_exact_end = item
                    .address
                    .checked_add(item.memory_size)
                    .ok_or(Status::Address)?;
                relro_end = relro_exact_end
                    .checked_add(PAGE - 1)
                    .ok_or(Status::Address)?
                    & !(PAGE - 1);
            }
            PT_TLS => {
                if tls.memory_size != 0
                    || item.flags != PF_R
                    || item.file_size > item.memory_size
                    || item.memory_size == 0
                    || item.memory_size > MAX_TLS_BYTES
                    || item.alignment == 0
                    || item.alignment > PAGE
                    || !item.alignment.is_power_of_two()
                    || item.physical != 0 && item.physical != item.address
                    || item
                        .offset
                        .checked_add(item.file_size)
                        .ok_or(Status::FileRange)?
                        > input.len() as u64
                {
                    return Err(Status::ProgramType);
                }
                tls = Tls {
                    file_offset: item.offset,
                    virtual_address: item.address,
                    file_size: item.file_size,
                    memory_size: item.memory_size,
                    alignment: item.alignment,
                };
            }
            PT_NOTE | PT_PHDR | PT_GNU_EH_FRAME => {
                if item.flags != PF_R
                    || item
                        .offset
                        .checked_add(item.file_size)
                        .ok_or(Status::FileRange)?
                        > input.len() as u64
                    || item.file_size > item.memory_size
                {
                    return Err(Status::ProgramType);
                }
            }
            PT_INTERP => return Err(Status::ProgramType),
            _ => return Err(Status::ProgramType),
        }
    }
    if segment_count == 0
        || stack_count != 1
        || mapping_start >= mapping_end
        || mapping_end - mapping_start > MAX_IMAGE_SPAN
    {
        return Err(Status::Stack);
    }
    let mut image = Image {
        entry,
        mapping_start,
        mapping_end,
        segments,
        segment_count: segment_count as u8,
        soname: Name::empty(),
        needed: [Name::empty(); MAX_NEEDED],
        needed_count: 0,
        string_address: 0,
        string_size: 0,
        symbol_address: 0,
        symbol_count: 0,
        hash_style: HashStyle::SysV,
        sysv_hash_address: 0,
        gnu_hash_address: 0,
        rela_address: 0,
        rela_count: 0,
        plt_rela_address: 0,
        plt_rela_count: 0,
        relative_count: 0,
        relro_start,
        relro_end,
        init: 0,
        fini: 0,
        init_array: 0,
        init_array_count: 0,
        fini_array: 0,
        fini_array_count: 0,
        tls,
        bind_now: false,
    };
    if entry != 0 && !executable_address(&image, entry) {
        return Err(Status::Entry);
    }
    if relro_end != 0
        && (!loaded_range(
            &image,
            relro_exact_start,
            relro_exact_end - relro_exact_start,
            true,
        ) || relro_file_size != 0
            && virtual_file_offset(&image, relro_exact_start, relro_file_size)?
                != usize::try_from(relro_offset).map_err(|_| Status::FileRange)?)
    {
        return Err(Status::ProgramType);
    }
    if image.tls.memory_size != 0 {
        let tls_end = image
            .tls
            .virtual_address
            .checked_add(image.tls.memory_size)
            .ok_or(Status::ProgramType)?;
        if !image.segments[..usize::from(image.segment_count)]
            .iter()
            .any(|segment| {
                segment.flags == (PF_R | PF_W)
                    && image.tls.virtual_address >= segment.virtual_address
                    && segment
                        .virtual_address
                        .checked_add(segment.memory_size)
                        .is_some_and(|end| tls_end <= end)
            })
        {
            return Err(Status::ProgramType);
        }
    }
    let dynamic = dynamic_program.ok_or(Status::DynamicSegment)?;
    if dynamic.flags != (PF_R | PF_W)
        || dynamic.physical != 0 && dynamic.physical != dynamic.address
        || dynamic.alignment != 8
        || dynamic.file_size == 0
        || dynamic.file_size != dynamic.memory_size
        || dynamic.file_size % 16 != 0
        || usize::try_from(dynamic.file_size / 16).map_err(|_| Status::DynamicSegment)?
            > MAX_DYNAMIC_ENTRIES
        || dynamic
            .offset
            .checked_add(dynamic.file_size)
            .ok_or(Status::DynamicSegment)?
            > input.len() as u64
        || !loaded_range(&image, dynamic.address, dynamic.memory_size, true)
        || virtual_file_offset(&image, dynamic.address, dynamic.file_size)?
            != usize::try_from(dynamic.offset).map_err(|_| Status::DynamicSegment)?
    {
        return Err(Status::DynamicSegment);
    }

    let mut needed_offsets = [0u64; MAX_NEEDED];
    let mut needed_count = 0usize;
    let mut soname = None;
    let mut strtab = None;
    let mut strsz = None;
    let mut symtab = None;
    let mut syment = None;
    let mut sysv_hash = None;
    let mut gnu_hash_value = None;
    let mut rela_address = None;
    let mut rela_size = None;
    let mut rela_entry = None;
    let mut relative_count = None;
    let mut jump_rela = None;
    let mut plt_size = None;
    let mut plt_kind = None;
    let mut pltgot = None;
    let mut init = None;
    let mut fini = None;
    let mut init_array = None;
    let mut init_array_size = None;
    let mut fini_array = None;
    let mut fini_array_size = None;
    let mut flags = None;
    let mut flags_1 = None;
    let mut bind_now_tag = false;
    let mut debug_tag = false;
    let dynamic_count =
        usize::try_from(dynamic.file_size / 16).map_err(|_| Status::DynamicSegment)?;
    let dynamic_offset = usize::try_from(dynamic.offset).map_err(|_| Status::DynamicSegment)?;
    let mut terminated = false;
    for index in 0..dynamic_count {
        let offset = dynamic_offset
            .checked_add(index * 16)
            .ok_or(Status::DynamicEntry)?;
        let tag = i64_at(input, offset, Status::DynamicEntry)?;
        let value = u64_at(input, offset + 8, Status::DynamicEntry)?;
        if terminated {
            if tag != 0 || value != 0 {
                return Err(Status::DynamicEntry);
            }
            continue;
        }
        if tag == DT_NULL {
            if value != 0 {
                return Err(Status::DynamicEntry);
            }
            terminated = true;
            continue;
        }
        match tag {
            DT_NEEDED => {
                if needed_count == MAX_NEEDED {
                    return Err(Status::DynamicEntry);
                }
                needed_offsets[needed_count] = value;
                needed_count += 1;
            }
            DT_SONAME => singleton(&mut soname, value)?,
            DT_STRTAB => singleton(&mut strtab, value)?,
            DT_STRSZ => singleton(&mut strsz, value)?,
            DT_SYMTAB => singleton(&mut symtab, value)?,
            DT_SYMENT => singleton(&mut syment, value)?,
            DT_HASH => singleton(&mut sysv_hash, value)?,
            DT_GNU_HASH => singleton(&mut gnu_hash_value, value)?,
            DT_RELA => singleton(&mut rela_address, value)?,
            DT_RELASZ => singleton(&mut rela_size, value)?,
            DT_RELAENT => singleton(&mut rela_entry, value)?,
            DT_RELACOUNT => singleton(&mut relative_count, value)?,
            DT_JMPREL => singleton(&mut jump_rela, value)?,
            DT_PLTRELSZ => singleton(&mut plt_size, value)?,
            DT_PLTREL => singleton(&mut plt_kind, value)?,
            DT_PLTGOT => singleton(&mut pltgot, value)?,
            DT_INIT => singleton(&mut init, value)?,
            DT_FINI => singleton(&mut fini, value)?,
            DT_INIT_ARRAY => singleton(&mut init_array, value)?,
            DT_INIT_ARRAYSZ => singleton(&mut init_array_size, value)?,
            DT_FINI_ARRAY => singleton(&mut fini_array, value)?,
            DT_FINI_ARRAYSZ => singleton(&mut fini_array_size, value)?,
            DT_BIND_NOW => {
                if bind_now_tag {
                    return Err(Status::DynamicDuplicate);
                }
                if value != 0 {
                    return Err(Status::DynamicEntry);
                }
                bind_now_tag = true;
                image.bind_now = true;
            }
            DT_FLAGS => singleton(&mut flags, value)?,
            DT_FLAGS_1 => singleton(&mut flags_1, value)?,
            DT_DEBUG => {
                if debug_tag {
                    return Err(Status::DynamicDuplicate);
                }
                if value != 0 {
                    return Err(Status::DynamicUnsupported);
                }
                debug_tag = true;
            }
            DT_RELR | DT_RELRSZ | DT_RELRENT => return Err(Status::DynamicUnsupported),
            _ => return Err(Status::DynamicUnsupported),
        }
    }
    if !terminated {
        return Err(Status::DynamicEntry);
    }
    image.string_address = strtab.ok_or(Status::DynamicMissing)?;
    image.string_size = strsz.ok_or(Status::DynamicMissing)?;
    image.symbol_address = symtab.ok_or(Status::DynamicMissing)?;
    if image.string_size == 0
        || image.string_size > MAX_STRING_BYTES
        || syment != Some(24)
        || virtual_file_offset(&image, image.string_address, image.string_size).is_err()
    {
        return Err(Status::StringTable);
    }
    if string(&image, input, 0, true)? != b"" {
        return Err(Status::StringTable);
    }
    image.sysv_hash_address = sysv_hash.unwrap_or(0);
    image.gnu_hash_address = gnu_hash_value.unwrap_or(0);
    if image.sysv_hash_address == 0 && image.gnu_hash_address == 0 {
        return Err(Status::HashTable);
    }
    let sysv_count = if image.sysv_hash_address == 0 {
        None
    } else {
        Some(sysv_symbol_count(&image, input, image.sysv_hash_address)?)
    };
    let gnu_count = if image.gnu_hash_address == 0 {
        None
    } else {
        Some(gnu_symbol_count(&image, input, image.gnu_hash_address)?)
    };
    if sysv_count.is_some() && gnu_count.is_some() && sysv_count != gnu_count {
        return Err(Status::HashTable);
    }
    image.symbol_count = gnu_count.or(sysv_count).ok_or(Status::HashTable)?;
    image.hash_style = if gnu_count.is_some() {
        HashStyle::Gnu
    } else {
        HashStyle::SysV
    };
    let symbol_bytes = u64::from(image.symbol_count)
        .checked_mul(24)
        .ok_or(Status::SymbolTable)?;
    let symbol_zero = virtual_file_offset(&image, image.symbol_address, symbol_bytes)?;
    if bytes(input, symbol_zero, 24, Status::SymbolTable)?
        .iter()
        .any(|byte| *byte != 0)
    {
        return Err(Status::SymbolTable);
    }
    for index in 1..image.symbol_count {
        let symbol = checked_symbol(&image, input, index)?;
        if symbol.section == SHN_UNDEF
            || !matches!(symbol.binding, STB_GLOBAL | STB_WEAK)
            || !matches!(symbol.visibility, STV_DEFAULT | STV_PROTECTED)
        {
            continue;
        }
        for prior in 1..index {
            let other = checked_symbol(&image, input, prior)?;
            if other.section != SHN_UNDEF
                && matches!(other.binding, STB_GLOBAL | STB_WEAK)
                && matches!(other.visibility, STV_DEFAULT | STV_PROTECTED)
                && other.name == symbol.name
            {
                return Err(Status::Symbol);
            }
        }
        if lookup(&image, input, symbol.name.as_bytes())? != Some(symbol) {
            return Err(Status::HashTable);
        }
    }
    if let Some(offset) = soname {
        image.soname = library_name(string(&image, input, offset, false)?)?;
    }
    for (index, name_offset) in needed_offsets[..needed_count].iter().enumerate() {
        let name = library_name(string(&image, input, *name_offset, false)?)?;
        if image.needed[..index].contains(&name) {
            return Err(Status::DynamicDuplicate);
        }
        image.needed[index] = name;
    }
    image.needed_count = needed_count as u8;

    let main_size = rela_size.unwrap_or(0);
    if main_size % 24 != 0
        || main_size != 0 && rela_entry != Some(24)
        || main_size == 0 && rela_address.is_some()
        || main_size != 0 && rela_address.is_none()
    {
        return Err(Status::RelocationTable);
    }
    image.rela_address = rela_address.unwrap_or(0);
    image.rela_count = u32::try_from(main_size / 24).map_err(|_| Status::RelocationTable)?;
    image.relative_count =
        u32::try_from(relative_count.unwrap_or(0)).map_err(|_| Status::RelocationTable)?;
    if image.relative_count > image.rela_count {
        return Err(Status::RelocationTable);
    }
    if main_size != 0 {
        let _ = virtual_file_offset(&image, image.rela_address, main_size)?;
    }
    let jump_size = plt_size.unwrap_or(0);
    if jump_size % 24 != 0
        || jump_size != 0 && plt_kind != Some(DT_RELA as u64)
        || jump_size == 0 && (jump_rela.is_some() || plt_kind.is_some())
        || jump_size != 0 && jump_rela.is_none()
    {
        return Err(Status::RelocationTable);
    }
    image.plt_rela_address = jump_rela.unwrap_or(0);
    image.plt_rela_count = u32::try_from(jump_size / 24).map_err(|_| Status::RelocationTable)?;
    if jump_size != 0 {
        let _ = virtual_file_offset(&image, image.plt_rela_address, jump_size)?;
    }
    if main_size != 0 && jump_size != 0 {
        let main_end = image
            .rela_address
            .checked_add(main_size)
            .ok_or(Status::RelocationTable)?;
        let jump_end = image
            .plt_rela_address
            .checked_add(jump_size)
            .ok_or(Status::RelocationTable)?;
        if image.rela_address < jump_end && image.plt_rela_address < main_end {
            return Err(Status::RelocationTable);
        }
    }
    if image
        .rela_count
        .checked_add(image.plt_rela_count)
        .and_then(|count| usize::try_from(count).ok())
        .is_none_or(|count| count > MAX_RELOCATIONS)
    {
        return Err(Status::RelocationTable);
    }
    if let Some(address) = pltgot {
        if !loaded_range(&image, address, 8, true) {
            return Err(Status::DynamicEntry);
        }
    }
    if flags.unwrap_or(0) & !(DF_BIND_NOW | DF_STATIC_TLS) != 0
        || flags.unwrap_or(0) & DF_STATIC_TLS != 0 && image.tls.memory_size == 0
        || flags_1.unwrap_or(0) & !(DF_1_NOW | DF_1_PIE) != 0
    {
        return Err(Status::DynamicUnsupported);
    }
    image.bind_now |= flags.unwrap_or(0) & DF_BIND_NOW != 0 || flags_1.unwrap_or(0) & DF_1_NOW != 0;
    image.init = init.unwrap_or(0);
    image.fini = fini.unwrap_or(0);
    if image.init != 0 && !executable_address(&image, image.init)
        || image.fini != 0 && !executable_address(&image, image.fini)
    {
        return Err(Status::DynamicEntry);
    }
    image.init_array = init_array.unwrap_or(0);
    image.fini_array = fini_array.unwrap_or(0);
    let init_size = init_array_size.unwrap_or(0);
    let fini_size = fini_array_size.unwrap_or(0);
    if init_size % 8 != 0
        || fini_size % 8 != 0
        || init_size / 8 > MAX_ARRAY_ENTRIES
        || fini_size / 8 > MAX_ARRAY_ENTRIES
        || init_size != 0
            && (image.init_array == 0 || !loaded_range(&image, image.init_array, init_size, true))
        || fini_size != 0
            && (image.fini_array == 0 || !loaded_range(&image, image.fini_array, fini_size, true))
        || init_size == 0 && image.init_array != 0
        || fini_size == 0 && image.fini_array != 0
    {
        return Err(Status::DynamicEntry);
    }
    image.init_array_count = u32::try_from(init_size / 8).map_err(|_| Status::DynamicEntry)?;
    image.fini_array_count = u32::try_from(fini_size / 8).map_err(|_| Status::DynamicEntry)?;
    validate_relocations(&image, input)?;
    Ok(image)
}

fn sysv_lookup(image: &Image, input: &[u8], name: &[u8]) -> Result<Option<Symbol>, Status> {
    let address = image.sysv_hash_address;
    let header = virtual_file_offset(image, address, 8)?;
    let bucket_count = u32_at(input, header, Status::HashTable)?;
    let chain_count = u32_at(input, header + 4, Status::HashTable)?;
    if bucket_count == 0 {
        return Err(Status::HashTable);
    }
    let bucket_address = address.checked_add(8).ok_or(Status::HashTable)?;
    let chain_address = bucket_address
        .checked_add(u64::from(bucket_count) * 4)
        .ok_or(Status::HashTable)?;
    let bucket = sysv_hash(name) % bucket_count;
    let bucket_offset = virtual_file_offset(image, bucket_address + u64::from(bucket) * 4, 4)?;
    let mut symbol_index = u32_at(input, bucket_offset, Status::HashTable)?;
    for _ in 0..chain_count {
        if symbol_index == 0 {
            return Ok(None);
        }
        if symbol_index >= chain_count {
            return Err(Status::HashTable);
        }
        let symbol = checked_symbol(image, input, symbol_index)?;
        if symbol.section != SHN_UNDEF
            && matches!(symbol.binding, STB_GLOBAL | STB_WEAK)
            && matches!(symbol.visibility, STV_DEFAULT | STV_PROTECTED)
            && symbol.name.as_bytes() == name
        {
            return Ok(Some(symbol));
        }
        let chain_offset =
            virtual_file_offset(image, chain_address + u64::from(symbol_index) * 4, 4)?;
        symbol_index = u32_at(input, chain_offset, Status::HashTable)?;
    }
    Err(Status::HashTable)
}

fn gnu_lookup(image: &Image, input: &[u8], name: &[u8]) -> Result<Option<Symbol>, Status> {
    let address = image.gnu_hash_address;
    let header = virtual_file_offset(image, address, 16)?;
    let bloom_size = u32_at(input, header + 8, Status::HashTable)?;
    let (bucket_count, symbol_offset, bloom_shift, bucket_address) =
        gnu_layout(image, input, address)?;
    let hash = gnu_hash(name);
    let bloom_address = address + 16 + u64::from((hash / 64) & (bloom_size - 1)) * 8;
    let bloom_offset = virtual_file_offset(image, bloom_address, 8)?;
    let bloom = u64_at(input, bloom_offset, Status::HashTable)?;
    let mask = (1u64 << (hash % 64)) | (1u64 << ((hash >> bloom_shift) % 64));
    if bloom & mask != mask {
        return Ok(None);
    }
    let bucket_offset = virtual_file_offset(
        image,
        bucket_address + u64::from(hash % bucket_count) * 4,
        4,
    )?;
    let first = u32_at(input, bucket_offset, Status::HashTable)?;
    if first == 0 {
        return Ok(None);
    }
    if first < symbol_offset || first >= image.symbol_count {
        return Err(Status::HashTable);
    }
    let chain_address = bucket_address + u64::from(bucket_count) * 4;
    let mut symbol_index = first;
    loop {
        if symbol_index >= image.symbol_count {
            return Err(Status::HashTable);
        }
        let chain_offset = virtual_file_offset(
            image,
            chain_address + u64::from(symbol_index - symbol_offset) * 4,
            4,
        )?;
        let chain_hash = u32_at(input, chain_offset, Status::HashTable)?;
        if (chain_hash | 1) == (hash | 1) {
            let symbol = checked_symbol(image, input, symbol_index)?;
            if symbol.section != SHN_UNDEF
                && matches!(symbol.binding, STB_GLOBAL | STB_WEAK)
                && matches!(symbol.visibility, STV_DEFAULT | STV_PROTECTED)
                && symbol.name.as_bytes() == name
            {
                return Ok(Some(symbol));
            }
        }
        if chain_hash & 1 != 0 {
            return Ok(None);
        }
        symbol_index = symbol_index.checked_add(1).ok_or(Status::HashTable)?;
    }
}

/// Look up one exported symbol using the admitted SysV or GNU hash table.
pub fn lookup(image: &Image, input: &[u8], name: &[u8]) -> Result<Option<Symbol>, Status> {
    if name.is_empty() || name.len() >= 64 || input.len() > MAX_FILE_BYTES {
        return Err(Status::String);
    }
    if image.gnu_hash_address != 0 {
        gnu_lookup(image, input, name)
    } else {
        sysv_lookup(image, input, name)
    }
}

/// Copy admitted PT_LOAD bytes into an exactly sized, zero-filled preparation image.
pub fn load_image(image: &Image, input: &[u8], memory: &mut [u8]) -> Result<(), Status> {
    if memory.len() != image.memory_bytes() {
        return Err(Status::MemorySize);
    }
    memory.fill(0);
    for segment in &image.segments[..usize::from(image.segment_count)] {
        let source = bytes(
            input,
            usize::try_from(segment.file_offset).map_err(|_| Status::FileRange)?,
            usize::try_from(segment.file_size).map_err(|_| Status::FileRange)?,
            Status::FileRange,
        )?;
        let destination = usize::try_from(segment.virtual_address - image.mapping_start)
            .map_err(|_| Status::MemorySize)?;
        let end = destination
            .checked_add(source.len())
            .ok_or(Status::MemorySize)?;
        memory
            .get_mut(destination..end)
            .ok_or(Status::MemorySize)?
            .copy_from_slice(source);
    }
    Ok(())
}

fn runtime_address(bias: u64, value: u64) -> Result<u64, Status> {
    if bias & (PAGE - 1) != 0 {
        return Err(Status::Address);
    }
    let address = bias.checked_add(value).ok_or(Status::RelocationOverflow)?;
    if address >= MAX_RUNTIME_ADDRESS {
        return Err(Status::Address);
    }
    Ok(address)
}

fn absolute_address(value: u64) -> Result<u64, Status> {
    if value < MAX_RUNTIME_ADDRESS {
        Ok(value)
    } else {
        Err(Status::Address)
    }
}

fn add_signed(base: u64, addend: i64) -> Result<u64, Status> {
    if addend >= 0 {
        base.checked_add(addend as u64)
            .ok_or(Status::RelocationOverflow)
    } else {
        base.checked_sub(addend.unsigned_abs())
            .ok_or(Status::RelocationOverflow)
    }
}

fn object_symbol_address(object: &Object<'_>, symbol: Symbol) -> Result<u64, Status> {
    if symbol.section == SHN_ABS {
        absolute_address(symbol.value)
    } else {
        runtime_address(object.load_bias, symbol.value)
    }
}

fn symbol_address(
    image: &Image,
    input: &[u8],
    bias: u64,
    symbol_index: u32,
    scope: &[Object<'_>],
) -> Result<u64, Status> {
    let symbol = checked_symbol(image, input, symbol_index)?;
    if symbol.binding == STB_LOCAL {
        if symbol.section == SHN_UNDEF {
            return Err(Status::UndefinedSymbol);
        }
        return if symbol.section == SHN_ABS {
            absolute_address(symbol.value)
        } else {
            runtime_address(bias, symbol.value)
        };
    }
    if symbol.section != SHN_UNDEF && symbol.visibility != STV_DEFAULT {
        return if symbol.section == SHN_ABS {
            absolute_address(symbol.value)
        } else {
            runtime_address(bias, symbol.value)
        };
    }
    for object in scope {
        if let Some(found) = lookup(object.image, object.file, symbol.name.as_bytes())? {
            return object_symbol_address(object, found);
        }
    }
    if symbol.section != SHN_UNDEF {
        return if symbol.section == SHN_ABS {
            absolute_address(symbol.value)
        } else {
            runtime_address(bias, symbol.value)
        };
    }
    if symbol.binding == STB_WEAK {
        Ok(0)
    } else {
        Err(Status::UndefinedSymbol)
    }
}

fn tls_symbol<'a>(
    image: &Image,
    input: &[u8],
    symbol_index: u32,
    scope: &'a [Object<'a>],
) -> Result<(&'a Object<'a>, Symbol), Status> {
    let symbol = checked_symbol(image, input, symbol_index)?;
    if symbol.kind != STT_TLS {
        return Err(Status::Symbol);
    }
    if symbol.binding == STB_LOCAL
        || symbol.section != SHN_UNDEF && symbol.visibility != STV_DEFAULT
    {
        let object = scope
            .iter()
            .find(|object| core::ptr::eq(object.image, image))
            .ok_or(Status::UndefinedSymbol)?;
        return if symbol.section == SHN_UNDEF {
            Err(Status::UndefinedSymbol)
        } else {
            Ok((object, symbol))
        };
    }
    for object in scope {
        if let Some(found) = lookup(object.image, object.file, symbol.name.as_bytes())? {
            if found.kind != STT_TLS {
                return Err(Status::Symbol);
            }
            return Ok((object, found));
        }
    }
    if symbol.section != SHN_UNDEF {
        let object = scope
            .iter()
            .find(|object| core::ptr::eq(object.image, image))
            .ok_or(Status::UndefinedSymbol)?;
        return Ok((object, symbol));
    }
    Err(Status::UndefinedSymbol)
}

fn tls_relocation_value(
    image: &Image,
    input: &[u8],
    symbol_index: u32,
    addend: i64,
    scope: &[Object<'_>],
) -> Result<i64, Status> {
    if symbol_index == 0 {
        let object = scope
            .iter()
            .find(|object| core::ptr::eq(object.image, image))
            .ok_or(Status::UndefinedSymbol)?;
        if image.tls.memory_size == 0
            || addend < 0
            || addend as u64 >= image.tls.memory_size
        {
            return Err(Status::Symbol);
        }
        return i64::try_from(i128::from(object.tls_offset) + i128::from(addend))
            .map_err(|_| Status::RelocationOverflow);
    }
    let (object, symbol) = tls_symbol(image, input, symbol_index, scope)?;
    let value = i128::from(object.tls_offset)
        .checked_add(i128::from(symbol.value))
        .and_then(|value| value.checked_add(i128::from(addend)))
        .ok_or(Status::RelocationOverflow)?;
    i64::try_from(value).map_err(|_| Status::RelocationOverflow)
}

fn write_relocation(
    image: &Image,
    input: &[u8],
    memory: &mut [u8],
    bias: u64,
    scope: &[Object<'_>],
    item: Rela,
) -> Result<(), Status> {
    if item.kind == R_X86_64_NONE {
        return Ok(());
    }
    let width = if item.kind == R_X86_64_PC32 {
        4usize
    } else {
        8usize
    };
    let destination = usize::try_from(
        item.offset
            .checked_sub(image.mapping_start)
            .ok_or(Status::RelocationTarget)?,
    )
    .map_err(|_| Status::RelocationTarget)?;
    let place = runtime_address(bias, item.offset)?;
    let target = memory
        .get_mut(
            destination
                ..destination
                    .checked_add(width)
                    .ok_or(Status::RelocationTarget)?,
        )
        .ok_or(Status::RelocationTarget)?;
    if item.kind == R_X86_64_RELATIVE {
        let value = absolute_address(add_signed(bias, item.addend)?)?;
        target.copy_from_slice(&value.to_le_bytes());
        return Ok(());
    }
    if item.kind == R_X86_64_TPOFF64 {
        let value = tls_relocation_value(image, input, item.symbol, item.addend, scope)?;
        target.copy_from_slice(&value.to_le_bytes());
        return Ok(());
    }
    let symbol = symbol_address(image, input, bias, item.symbol, scope)?;
    let value = add_signed(symbol, item.addend)?;
    if item.kind == R_X86_64_PC32 {
        let relative = i128::from(value) - i128::from(place);
        let encoded = i32::try_from(relative).map_err(|_| Status::RelocationOverflow)?;
        target.copy_from_slice(&encoded.to_le_bytes());
    } else {
        absolute_address(value)?;
        target.copy_from_slice(&value.to_le_bytes());
    }
    Ok(())
}

/// Eagerly apply the admitted RELA and PLT RELA tables in their file order.
///
/// `memory` must still be private preparation memory. `scope` is searched in
/// exact caller order; callers implementing ELF preemption should include the
/// root object first, followed by breadth-first `DT_NEEDED` load order. The
/// dependencies-first order returned by [`dependency_order`] is for lifecycle
/// calls and must not replace the global lookup order.
pub fn apply_relocations(
    image: &Image,
    input: &[u8],
    memory: &mut [u8],
    load_bias: u64,
    scope: &[Object<'_>],
) -> Result<(), Status> {
    if memory.len() != image.memory_bytes() || scope.len() > MAX_DEPENDENCY_OBJECTS {
        return Err(Status::MemorySize);
    }
    validate_relocations(image, input)?;
    let _ = runtime_address(load_bias, image.mapping_start)?;
    let _ = runtime_address(load_bias, image.mapping_end - 1)?;
    for table in 0..2u8 {
        let (address, count) = if table == 0 {
            (image.rela_address, image.rela_count)
        } else {
            (image.plt_rela_address, image.plt_rela_count)
        };
        for index in 0..count {
            write_relocation(
                image,
                input,
                memory,
                load_bias,
                scope,
                rela(image, input, address, index)?,
            )?;
        }
    }
    Ok(())
}

fn runtime_executable(scope: &[Object<'_>], address: u64) -> bool {
    scope.iter().any(|object| {
        object.image.segments[..usize::from(object.image.segment_count)]
            .iter()
            .any(|segment| {
                segment.flags == (PF_R | PF_X)
                    && object
                        .load_bias
                        .checked_add(segment.virtual_address)
                        .is_some_and(|start| {
                            segment
                                .memory_size
                                .checked_add(start)
                                .is_some_and(|end| address >= start && address < end)
                        })
            })
    })
}

fn push_lifecycle(
    scope: &[Object<'_>],
    output: &mut [u64; MAX_LIFECYCLE_FUNCTIONS],
    count: &mut usize,
    address: u64,
) -> Result<(), Status> {
    if address == 0
        || address == u64::MAX
        || *count == MAX_LIFECYCLE_FUNCTIONS
        || !runtime_executable(scope, address)
    {
        return Err(Status::Lifecycle);
    }
    output[*count] = address;
    *count += 1;
    Ok(())
}

fn relocated_array_value(
    object: &Object<'_>,
    memory: &[u8],
    address: u64,
    index: u32,
) -> Result<u64, Status> {
    if memory.len() != object.image.memory_bytes() {
        return Err(Status::MemorySize);
    }
    let entry = address
        .checked_add(u64::from(index) * 8)
        .ok_or(Status::Lifecycle)?;
    let offset = usize::try_from(
        entry
            .checked_sub(object.image.mapping_start)
            .ok_or(Status::Lifecycle)?,
    )
    .map_err(|_| Status::Lifecycle)?;
    u64_at(memory, offset, Status::Lifecycle)
}

/// Append one object's initializers in System V object-local order.
pub fn append_initializers(
    object: &Object<'_>,
    memory: &[u8],
    scope: &[Object<'_>],
    output: &mut [u64; MAX_LIFECYCLE_FUNCTIONS],
    count: &mut usize,
) -> Result<(), Status> {
    if object.image.init != 0 {
        push_lifecycle(
            scope,
            output,
            count,
            runtime_address(object.load_bias, object.image.init)?,
        )?;
    }
    for index in 0..object.image.init_array_count {
        push_lifecycle(
            scope,
            output,
            count,
            relocated_array_value(object, memory, object.image.init_array, index)?,
        )?;
    }
    Ok(())
}

/// Append one object's finalizers in reverse-array then DT_FINI order.
pub fn append_finalizers(
    object: &Object<'_>,
    memory: &[u8],
    scope: &[Object<'_>],
    output: &mut [u64; MAX_LIFECYCLE_FUNCTIONS],
    count: &mut usize,
) -> Result<(), Status> {
    for remaining in (0..object.image.fini_array_count).rev() {
        push_lifecycle(
            scope,
            output,
            count,
            relocated_array_value(object, memory, object.image.fini_array, remaining)?,
        )?;
    }
    if object.image.fini != 0 {
        push_lifecycle(
            scope,
            output,
            count,
            runtime_address(object.load_bias, object.image.fini)?,
        )?;
    }
    Ok(())
}

fn named_object(images: &[Image], name: &Name) -> Result<usize, Status> {
    let mut found = None;
    for (index, image) in images.iter().enumerate() {
        if image.soname == *name {
            if found.is_some() {
                return Err(Status::DependencyAmbiguous);
            }
            found = Some(index);
        }
    }
    found.ok_or(Status::DependencyMissing)
}

fn visit_dependency(
    index: usize,
    images: &[Image],
    state: &mut [u8; MAX_DEPENDENCY_OBJECTS],
    output: &mut [usize; MAX_DEPENDENCY_OBJECTS],
    count: &mut usize,
) -> Result<(), Status> {
    if state[index] == 1 {
        return Err(Status::DependencyCycle);
    }
    if state[index] == 2 {
        return Ok(());
    }
    state[index] = 1;
    let image = &images[index];
    for needed in &image.needed[..usize::from(image.needed_count)] {
        let dependency = named_object(images, needed)?;
        visit_dependency(dependency, images, state, output, count)?;
    }
    state[index] = 2;
    if *count == MAX_DEPENDENCY_OBJECTS {
        return Err(Status::DependencyBound);
    }
    output[*count] = index;
    *count += 1;
    Ok(())
}

/// Resolve exact SONAME dependencies in DT_NEEDED order and return dependencies first.
pub fn dependency_order(
    root: &Image,
    images: &[Image],
    output: &mut [usize; MAX_DEPENDENCY_OBJECTS],
) -> Result<usize, Status> {
    if images.len() > MAX_DEPENDENCY_OBJECTS {
        return Err(Status::DependencyBound);
    }
    for image in images {
        if image.soname.is_empty() {
            return Err(Status::DependencyMissing);
        }
    }
    for left in 0..images.len() {
        for right in left + 1..images.len() {
            if images[left].soname == images[right].soname {
                return Err(Status::DependencyAmbiguous);
            }
        }
    }
    let mut state = [0u8; MAX_DEPENDENCY_OBJECTS];
    let mut count = 0usize;
    for needed in &root.needed[..usize::from(root.needed_count)] {
        let index = named_object(images, needed)?;
        visit_dependency(index, images, &mut state, output, &mut count)?;
    }
    Ok(count)
}

/// Run host-independent ABI and checked-address controls compiled into Phipia.
#[must_use]
pub fn self_test() -> u32 {
    if core::mem::size_of::<Name>() != 65
        || core::mem::size_of::<Segment>() != 56
        || core::mem::size_of::<Tls>() != 40
        || core::mem::size_of::<Catalog>() != 1560
        || Status::Count as i32 != 41
        || add_signed(7, -8) != Err(Status::RelocationOverflow)
        || runtime_address(1, PAGE) != Err(Status::Address)
        || runtime_address(MAX_RUNTIME_ADDRESS - PAGE, PAGE) != Err(Status::Address)
    {
        return 0;
    }
    9
}
