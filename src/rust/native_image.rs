// SPDX-License-Identifier: GPL-3.0-only
//! Safe validators for the Phipia application manifest and static ELF64 image.

use crate::sha256;

/// Fixed manifest byte count.
pub const MANIFEST_BYTES: usize = 1024;
/// Maximum accepted ELF program headers.
pub const MAX_PROGRAM_HEADERS: usize = 32;
/// Maximum accepted load segments.
pub const MAX_LOAD_SEGMENTS: usize = 16;
/// Maximum executable file length, matching FAT32's file bound.
pub const MAX_FILE_BYTES: usize = 16 * 1024 * 1024;
const MIN_ADDRESS: u64 = 0x0000_4000_0000_0000;
const MAX_ADDRESS: u64 = 0x0000_4001_0000_0000;
const PAGE: u64 = 4096;
const MANIFEST_MAGIC: &[u8; 8] = b"PHIPIAA1";
const CAPABILITIES_V1: u64 = (1u64 << 12) - 1;
const MIN_MEMORY: u64 = 64 * 1024;
const MAX_MEMORY: u64 = 256 * 1024 * 1024;
const MAX_HANDLES: u16 = 128;
const MAX_THREADS: u16 = 8;
const MAX_TLS_BYTES: u64 = 1024 * 1024;
const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;
const PT_LOAD: u32 = 1;
const PT_TLS: u32 = 7;
const PT_GNU_STACK: u32 = 0x6474_E551;

/// Named validation conclusion mirrored by the C header.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    /// Manifest and ELF form one admitted application.
    Ok = 0,
    /// A null pointer crossed the C boundary.
    NullArgument = 1,
    /// The manifest is not exactly 1,024 bytes.
    ManifestLength = 2,
    /// Manifest magic is invalid.
    ManifestMagic = 3,
    /// Package or ABI version is unsupported.
    ManifestVersion = 4,
    /// The manifest's explicit size is invalid.
    ManifestSize = 5,
    /// A reserved byte or flag is nonzero.
    ManifestReserved = 6,
    /// A bounded text field is malformed.
    ManifestText = 7,
    /// A resource limit is invalid.
    ManifestLimit = 8,
    /// A capability bit is unsupported.
    ManifestCapability = 9,
    /// Entry arguments are malformed.
    ManifestArgument = 10,
    /// ELF length is empty or above the file bound.
    ElfLength = 11,
    /// ELF magic is invalid.
    ElfMagic = 12,
    /// ELF class, byte order, ABI, or identification padding is invalid.
    ElfIdentity = 13,
    /// The object is not static ET_EXEC.
    ElfType = 14,
    /// The machine is not x86-64.
    ElfMachine = 15,
    /// A fixed ELF header field is invalid.
    ElfHeader = 16,
    /// Program-table arithmetic or cardinality is invalid.
    ElfProgramTable = 17,
    /// A program header type is unsupported.
    ElfProgramType = 18,
    /// Segment flags violate W^X or the admitted combinations.
    ElfProgramFlags = 19,
    /// File extent arithmetic is invalid.
    ElfFileRange = 20,
    /// Segment file and memory sizes are invalid.
    ElfLoadSize = 21,
    /// Segment alignment or congruence is invalid.
    ElfAlignment = 22,
    /// A virtual extent is noncanonical or outside the application range.
    ElfAddress = 23,
    /// Load byte or page extents overlap.
    ElfOverlap = 24,
    /// The entry is not within an executable segment.
    ElfEntry = 25,
    /// The non-executable stack declaration is absent or malformed.
    ElfStack = 26,
    /// The optional TLS template is malformed or outside a writable load.
    ElfTls = 27,
    /// Section-table arithmetic or shape is invalid.
    ElfSectionTable = 28,
    /// A relocation, dynamic symbol table, or dynamic section is present.
    ElfRelocation = 29,
    /// The manifest digest does not name the supplied ELF.
    DigestMismatch = 30,
}

/// Pointer-free validated manifest.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Manifest {
    /// One only after complete validation.
    pub valid: u32,
    /// Required native ABI.
    pub abi_version: u32,
    /// Requested v1 capabilities.
    pub capabilities: u64,
    /// Initial and maximum committed-memory limit.
    pub memory_limit: u64,
    /// Process-local handle-table limit.
    pub max_handles: u16,
    /// Process thread limit.
    pub max_threads: u16,
    /// Number of populated argument records.
    pub argument_count: u16,
    /// Explicit zero padding.
    pub reserved: u16,
    /// Display name.
    pub name: [u8; 32],
    /// Stable package identifier.
    pub identifier: [u8; 16],
    /// System-volume executable path.
    pub executable: [u8; 16],
    /// Expected executable digest.
    pub executable_sha256: [u8; 32],
    /// Optional immutable resource directory.
    pub resource_directory: [u8; 16],
    /// Required Data namespace.
    pub data_namespace: [u8; 16],
    /// Optional icon path.
    pub icon: [u8; 16],
    /// Fixed entry-argument records.
    pub arguments: [[u8; 32]; 8],
    /// Optional System-volume dynamic-library catalog path.
    pub dynamic_catalog: [u8; 16],
    /// SHA-256 of the complete dynamic-library catalog.
    pub dynamic_catalog_sha256: [u8; 32],
}

impl Manifest {
    /// Stable all-zero output for a refused call.
    pub const fn invalid() -> Self {
        Self {
            valid: 0, abi_version: 0, capabilities: 0, memory_limit: 0,
            max_handles: 0, max_threads: 0, argument_count: 0, reserved: 0,
            name: [0; 32], identifier: [0; 16], executable: [0; 16],
            executable_sha256: [0; 32], resource_directory: [0; 16],
            data_namespace: [0; 16], icon: [0; 16], arguments: [[0; 32]; 8],
            dynamic_catalog: [0; 16], dynamic_catalog_sha256: [0; 32],
        }
    }
}

/// One admitted PT_LOAD record.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Segment {
    /// File offset.
    pub file_offset: u64,
    /// First virtual byte.
    pub virtual_address: u64,
    /// Initialized byte count.
    pub file_size: u64,
    /// Complete memory byte count.
    pub memory_size: u64,
    /// First mapped page.
    pub mapping_start: u64,
    /// Exclusive mapped page end.
    pub mapping_end: u64,
    /// ELF PF flags.
    pub flags: u32,
    /// Explicit zero padding.
    pub reserved: u32,
}

impl Segment {
    const fn invalid() -> Self {
        Self { file_offset: 0, virtual_address: 0, file_size: 0, memory_size: 0,
            mapping_start: 0, mapping_end: 0, flags: 0, reserved: 0 }
    }
}

/// Optional initial-exec TLS template facts.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Tls {
    /// Template file offset.
    pub file_offset: u64,
    /// Link-time virtual address.
    pub virtual_address: u64,
    /// Initialized byte count.
    pub file_size: u64,
    /// Complete template byte count.
    pub memory_size: u64,
    /// Required alignment.
    pub alignment: u64,
}

impl Tls {
    const fn absent() -> Self {
        Self { file_offset: 0, virtual_address: 0, file_size: 0, memory_size: 0,
            alignment: 0 }
    }
}

/// Pointer-free validated ELF result.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ValidatedImage {
    /// One only after complete validation.
    pub valid: u32,
    /// Decoded program-header count.
    pub program_header_count: u32,
    /// Admitted load-segment count.
    pub segment_count: u32,
    /// Explicit zero padding.
    pub reserved: u32,
    /// Checked entry point.
    pub entry: u64,
    /// First mapped image page.
    pub mapping_start: u64,
    /// Exclusive final mapped image page.
    pub mapping_end: u64,
    /// Optional TLS template.
    pub tls: Tls,
    /// Load segments in program-table order.
    pub segments: [Segment; MAX_LOAD_SEGMENTS],
}

impl ValidatedImage {
    /// Stable all-zero output for a refused call.
    pub const fn invalid() -> Self {
        Self { valid: 0, program_header_count: 0, segment_count: 0, reserved: 0,
            entry: 0, mapping_start: 0, mapping_end: 0, tls: Tls::absent(),
            segments: [Segment::invalid(); MAX_LOAD_SEGMENTS] }
    }
}

fn byte(input: &[u8], offset: usize) -> Result<u8, Status> {
    input.get(offset).copied().ok_or(Status::ElfHeader)
}

fn u16_at(input: &[u8], offset: usize, error: Status) -> Result<u16, Status> {
    let end = offset.checked_add(2).ok_or(error)?;
    let bytes: [u8; 2] = input.get(offset..end).ok_or(error)?
        .try_into().map_err(|_| error)?;
    Ok(u16::from_le_bytes(bytes))
}

fn u32_at(input: &[u8], offset: usize, error: Status) -> Result<u32, Status> {
    let end = offset.checked_add(4).ok_or(error)?;
    let bytes: [u8; 4] = input.get(offset..end).ok_or(error)?
        .try_into().map_err(|_| error)?;
    Ok(u32::from_le_bytes(bytes))
}

fn u64_at(input: &[u8], offset: usize, error: Status) -> Result<u64, Status> {
    let end = offset.checked_add(8).ok_or(error)?;
    let bytes: [u8; 8] = input.get(offset..end).ok_or(error)?
        .try_into().map_err(|_| error)?;
    Ok(u64::from_le_bytes(bytes))
}

fn copy_field<const N: usize>(input: &[u8], offset: usize) -> Result<[u8; N], Status> {
    let end = offset.checked_add(N).ok_or(Status::ManifestSize)?;
    input.get(offset..end).ok_or(Status::ManifestSize)?
        .try_into().map_err(|_| Status::ManifestSize)
}

fn text_valid(text: &[u8], required: bool, identifier: bool) -> bool {
    let mut end = text.len();
    for (index, byte) in text.iter().enumerate() {
        if *byte == 0 {
            end = index;
            break;
        }
    }
    if end == text.len() { return false; }
    if required && end == 0 { return false; }
    for (index, byte) in text.iter().enumerate() {
        if index >= end {
            if *byte != 0 { return false; }
            continue;
        }
        if identifier && *byte == b'/' { return false; }
        if !byte.is_ascii_alphanumeric() && *byte != b'_' && *byte != b'-'
            && (identifier || (*byte != b'.' && *byte != b' ' && *byte != b'/')) {
            return false;
        }
    }
    true
}

fn argument_valid(argument: &[u8], required: bool) -> bool {
    let mut end = argument.len();
    for (index, byte) in argument.iter().enumerate() {
        if *byte == 0 {
            end = index;
            break;
        }
    }
    if end == argument.len() || (required && end == 0) { return false; }
    for (index, byte) in argument.iter().enumerate() {
        if index >= end {
            if *byte != 0 { return false; }
        } else if !(0x20..=0x7e).contains(byte) {
            return false;
        }
    }
    true
}

/// Validate one exact binary application manifest.
pub fn parse_manifest(input: &[u8]) -> Result<Manifest, Status> {
    if input.len() != MANIFEST_BYTES { return Err(Status::ManifestLength); }
    if input.get(..8) != Some(MANIFEST_MAGIC.as_slice()) {
        return Err(Status::ManifestMagic);
    }
    let format = u16_at(input, 8, Status::ManifestSize)?;
    let header_size = u16_at(input, 10, Status::ManifestSize)?;
    let abi_version = u32_at(input, 12, Status::ManifestSize)?;
    let flags = u32_at(input, 16, Status::ManifestSize)?;
    let argument_count = u16_at(input, 20, Status::ManifestSize)?;
    let max_handles = u16_at(input, 22, Status::ManifestSize)?;
    let max_threads = u16_at(input, 24, Status::ManifestSize)?;
    let reserved = u16_at(input, 26, Status::ManifestSize)?;
    let capabilities = u64_at(input, 28, Status::ManifestSize)?;
    let memory_limit = u64_at(input, 36, Status::ManifestSize)?;
    if format != 1 || abi_version != 1 { return Err(Status::ManifestVersion); }
    if header_size as usize != MANIFEST_BYTES { return Err(Status::ManifestSize); }
    if flags != 0 || reserved != 0 || input[44..64].iter().any(|byte| *byte != 0)
        || input[512..].iter().any(|byte| *byte != 0) {
        return Err(Status::ManifestReserved);
    }
    if argument_count as usize > 8 { return Err(Status::ManifestArgument); }
    if max_handles == 0 || max_handles > MAX_HANDLES || max_threads == 0
        || max_threads > MAX_THREADS || memory_limit < MIN_MEMORY
        || memory_limit > MAX_MEMORY || memory_limit % PAGE != 0 {
        return Err(Status::ManifestLimit);
    }
    if capabilities & !CAPABILITIES_V1 != 0 { return Err(Status::ManifestCapability); }
    let name = copy_field::<32>(input, 64)?;
    let identifier = copy_field::<16>(input, 96)?;
    let executable = copy_field::<16>(input, 112)?;
    let executable_sha256 = copy_field::<32>(input, 128)?;
    let resource_directory = copy_field::<16>(input, 160)?;
    let data_namespace = copy_field::<16>(input, 176)?;
    let icon = copy_field::<16>(input, 192)?;
    if !text_valid(&name, true, false) || !text_valid(&identifier, true, true)
        || !text_valid(&executable, true, false)
        || !text_valid(&resource_directory, false, false)
        || !text_valid(&data_namespace, true, true) || !text_valid(&icon, false, false) {
        return Err(Status::ManifestText);
    }
    let mut arguments = [[0u8; 32]; 8];
    for (index, argument) in arguments.iter_mut().enumerate() {
        *argument = copy_field::<32>(input, 208 + index * 32)?;
        let populated = index < argument_count as usize;
        if !argument_valid(argument, populated)
            || (!populated && argument.iter().any(|byte| *byte != 0)) {
            return Err(Status::ManifestArgument);
        }
    }
    let dynamic_catalog = copy_field::<16>(input, 464)?;
    let dynamic_catalog_sha256 = copy_field::<32>(input, 480)?;
    let catalog_present = dynamic_catalog.iter().any(|byte| *byte != 0);
    let catalog_digest_present = dynamic_catalog_sha256.iter().any(|byte| *byte != 0);
    if !text_valid(&dynamic_catalog, false, false)
        || catalog_present != catalog_digest_present
    {
        return Err(Status::ManifestText);
    }
    Ok(Manifest { valid: 1, abi_version, capabilities, memory_limit, max_handles,
        max_threads, argument_count, reserved: 0, name, identifier, executable,
        executable_sha256, resource_directory, data_namespace, icon, arguments,
        dynamic_catalog, dynamic_catalog_sha256 })
}

#[derive(Clone, Copy)]
struct Program {
    kind: u32, flags: u32, offset: u64, address: u64, physical: u64,
    file_size: u64, memory_size: u64, alignment: u64,
}

fn program(input: &[u8], offset: usize) -> Result<Program, Status> {
    Ok(Program {
        kind: u32_at(input, offset, Status::ElfProgramTable)?,
        flags: u32_at(input, offset + 4, Status::ElfProgramTable)?,
        offset: u64_at(input, offset + 8, Status::ElfProgramTable)?,
        address: u64_at(input, offset + 16, Status::ElfProgramTable)?,
        physical: u64_at(input, offset + 24, Status::ElfProgramTable)?,
        file_size: u64_at(input, offset + 32, Status::ElfProgramTable)?,
        memory_size: u64_at(input, offset + 40, Status::ElfProgramTable)?,
        alignment: u64_at(input, offset + 48, Status::ElfProgramTable)?,
    })
}

fn validate_sections(input: &[u8]) -> Result<(), Status> {
    let offset = u64_at(input, 40, Status::ElfSectionTable)?;
    let entry_size = u16_at(input, 58, Status::ElfSectionTable)? as u64;
    let count = u16_at(input, 60, Status::ElfSectionTable)? as u64;
    let names = u16_at(input, 62, Status::ElfSectionTable)? as u64;
    if count == 0 {
        return if offset == 0 && names == 0 { Ok(()) } else { Err(Status::ElfSectionTable) };
    }
    if entry_size != 64 || names >= count { return Err(Status::ElfSectionTable); }
    let table_bytes = entry_size.checked_mul(count).ok_or(Status::ElfSectionTable)?;
    let end = offset.checked_add(table_bytes).ok_or(Status::ElfSectionTable)?;
    if end > input.len() as u64 { return Err(Status::ElfSectionTable); }
    for index in 0..count {
        let position = offset.checked_add(index.checked_mul(entry_size)
            .ok_or(Status::ElfSectionTable)?).ok_or(Status::ElfSectionTable)? as usize;
        let kind = u32_at(input, position + 4, Status::ElfSectionTable)?;
        let flags = u64_at(input, position + 8, Status::ElfSectionTable)?;
        let section_offset = u64_at(input, position + 24, Status::ElfSectionTable)?;
        let size = u64_at(input, position + 32, Status::ElfSectionTable)?;
        if matches!(kind, 4 | 6 | 9 | 11) && size != 0 { return Err(Status::ElfRelocation); }
        if flags & 0x5 == 0x5 { return Err(Status::ElfProgramFlags); }
        if kind != 8 {
            let section_end = section_offset.checked_add(size).ok_or(Status::ElfSectionTable)?;
            if section_end > input.len() as u64 { return Err(Status::ElfSectionTable); }
        }
    }
    Ok(())
}

/// Validate one general static ET_EXEC application.
pub fn parse_elf(input: &[u8]) -> Result<ValidatedImage, Status> {
    if input.is_empty() || input.len() > MAX_FILE_BYTES { return Err(Status::ElfLength); }
    if input.get(..4) != Some(b"\x7fELF") { return Err(Status::ElfMagic); }
    if byte(input, 4)? != 2 || byte(input, 5)? != 1 || byte(input, 6)? != 1
        || byte(input, 7)? != 0 || byte(input, 8)? != 0
        || input.get(9..16).ok_or(Status::ElfIdentity)?.iter().any(|byte| *byte != 0) {
        return Err(Status::ElfIdentity);
    }
    if u16_at(input, 16, Status::ElfHeader)? != 2 { return Err(Status::ElfType); }
    if u16_at(input, 18, Status::ElfHeader)? != 62 { return Err(Status::ElfMachine); }
    if u32_at(input, 20, Status::ElfHeader)? != 1
        || u32_at(input, 48, Status::ElfHeader)? != 0
        || u16_at(input, 52, Status::ElfHeader)? != 64
        || u16_at(input, 54, Status::ElfHeader)? != 56 {
        return Err(Status::ElfHeader);
    }
    let entry = u64_at(input, 24, Status::ElfHeader)?;
    let table_offset = u64_at(input, 32, Status::ElfProgramTable)?;
    let count = u16_at(input, 56, Status::ElfProgramTable)? as usize;
    if count == 0 || count > MAX_PROGRAM_HEADERS { return Err(Status::ElfProgramTable); }
    let table_bytes = (count as u64).checked_mul(56).ok_or(Status::ElfProgramTable)?;
    if table_offset < 64 || table_offset.checked_add(table_bytes)
        .ok_or(Status::ElfProgramTable)? > input.len() as u64 {
        return Err(Status::ElfProgramTable);
    }
    validate_sections(input)?;
    let mut segments = [Segment::invalid(); MAX_LOAD_SEGMENTS];
    let mut loads = 0usize;
    let mut stack_count = 0usize;
    let mut tls = Tls::absent();
    let mut executable_entry = false;
    let mut mapping_start = MAX_ADDRESS;
    let mut mapping_end = MIN_ADDRESS;
    for index in 0..count {
        let offset = table_offset as usize + index * 56;
        let item = program(input, offset)?;
        if item.kind == PT_GNU_STACK {
            if stack_count != 0 || item.flags != PF_R | PF_W || item.offset != 0
                || item.address != 0 || item.physical != 0 || item.file_size != 0
                || item.memory_size != 0 || item.alignment > 16 {
                return Err(Status::ElfStack);
            }
            stack_count += 1;
            continue;
        }
        if item.kind == PT_TLS {
            if tls.memory_size != 0 || item.flags != PF_R || item.file_size > item.memory_size
                || item.memory_size == 0 || item.memory_size > MAX_TLS_BYTES
                || item.alignment == 0
                || !item.alignment.is_power_of_two() || item.alignment > PAGE {
                return Err(Status::ElfTls);
            }
            let end = item.offset.checked_add(item.file_size).ok_or(Status::ElfFileRange)?;
            if end > input.len() as u64 { return Err(Status::ElfFileRange); }
            tls = Tls { file_offset: item.offset, virtual_address: item.address,
                file_size: item.file_size, memory_size: item.memory_size,
                alignment: item.alignment };
            continue;
        }
        if item.kind != PT_LOAD { return Err(Status::ElfProgramType); }
        if loads == MAX_LOAD_SEGMENTS { return Err(Status::ElfProgramTable); }
        if item.flags != PF_R && item.flags != PF_R | PF_X
            && item.flags != PF_R | PF_W
            || item.flags & (PF_W | PF_X) == (PF_W | PF_X) {
            return Err(Status::ElfProgramFlags);
        }
        if item.file_size > item.memory_size || item.memory_size == 0 {
            return Err(Status::ElfLoadSize);
        }
        let file_end = item.offset.checked_add(item.file_size).ok_or(Status::ElfFileRange)?;
        if file_end > input.len() as u64 { return Err(Status::ElfFileRange); }
        if item.alignment != PAGE || item.offset & (PAGE - 1) != item.address & (PAGE - 1) {
            return Err(Status::ElfAlignment);
        }
        if item.physical != item.address { return Err(Status::ElfAddress); }
        let virtual_end = item.address.checked_add(item.memory_size).ok_or(Status::ElfAddress)?;
        let page_start = item.address & !(PAGE - 1);
        let page_end = virtual_end.checked_add(PAGE - 1).ok_or(Status::ElfAddress)? & !(PAGE - 1);
        if item.address < MIN_ADDRESS || virtual_end <= item.address || page_end > MAX_ADDRESS {
            return Err(Status::ElfAddress);
        }
        let admitted = segments.get(..loads).ok_or(Status::ElfProgramTable)?;
        for prior in admitted {
            let prior_end = prior.virtual_address.checked_add(prior.memory_size)
                .ok_or(Status::ElfOverlap)?;
            if item.address < prior_end && prior.virtual_address < virtual_end
                || page_start < prior.mapping_end && prior.mapping_start < page_end {
                return Err(Status::ElfOverlap);
            }
        }
        if item.flags & PF_X != 0 && entry >= item.address && entry < virtual_end {
            executable_entry = true;
        }
        mapping_start = mapping_start.min(page_start);
        mapping_end = mapping_end.max(page_end);
        segments[loads] = Segment { file_offset: item.offset,
            virtual_address: item.address, file_size: item.file_size,
            memory_size: item.memory_size, mapping_start: page_start,
            mapping_end: page_end, flags: item.flags, reserved: 0 };
        loads += 1;
    }
    if loads == 0 || stack_count != 1 { return Err(Status::ElfStack); }
    if !executable_entry { return Err(Status::ElfEntry); }
    if tls.memory_size != 0 {
        let tls_end = tls.virtual_address.checked_add(tls.memory_size).ok_or(Status::ElfTls)?;
        let admitted = segments.get(..loads).ok_or(Status::ElfProgramTable)?;
        let covered = admitted.iter().any(|segment| {
            segment.flags == PF_R | PF_W && tls.virtual_address >= segment.virtual_address
                && segment.virtual_address.checked_add(segment.memory_size)
                    .is_some_and(|end| tls_end <= end)
        });
        if !covered { return Err(Status::ElfTls); }
    }
    Ok(ValidatedImage { valid: 1, program_header_count: count as u32,
        segment_count: loads as u32, reserved: 0, entry, mapping_start, mapping_end,
        tls, segments })
}

/// Validate manifest, ELF structure, and digest as one admission operation.
pub fn validate(manifest_bytes: &[u8], elf: &[u8]) -> Result<(Manifest, ValidatedImage), Status> {
    let manifest = parse_manifest(manifest_bytes)?;
    let image = parse_elf(elf)?;
    if sha256::digest(elf) != manifest.executable_sha256 {
        return Err(Status::DigestMismatch);
    }
    Ok((manifest, image))
}

/// Validate a manifest and authenticate one bounded executable without
/// selecting its static or dynamic ELF parser.
pub fn authenticate(manifest_bytes: &[u8], elf: &[u8]) -> Result<Manifest, Status> {
    let manifest = parse_manifest(manifest_bytes)?;
    if elf.is_empty() || elf.len() > MAX_FILE_BYTES {
        return Err(Status::ElfLength);
    }
    if sha256::digest(elf) != manifest.executable_sha256 {
        return Err(Status::DigestMismatch);
    }
    Ok(manifest)
}

/// Allocation-free invariant count used by the boot self-test.
#[must_use]
pub fn self_test() -> u32 {
    if sha256::digest(b"abc") != [
        0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
        0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
        0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
        0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD,
    ] || core::mem::size_of::<Manifest>() != 480
        || core::mem::size_of::<Segment>() != 56
        || core::mem::size_of::<ValidatedImage>() != 976 {
        return 0;
    }
    12
}

#[cfg(test)]
mod tests {
    use super::{MANIFEST_BYTES, Status, parse_manifest};

    fn manifest() -> [u8; MANIFEST_BYTES] {
        let mut bytes = [0u8; MANIFEST_BYTES];
        bytes[..8].copy_from_slice(b"PHIPIAA1");
        bytes[8..10].copy_from_slice(&1u16.to_le_bytes());
        bytes[10..12].copy_from_slice(&(MANIFEST_BYTES as u16).to_le_bytes());
        bytes[12..16].copy_from_slice(&1u32.to_le_bytes());
        bytes[20..22].copy_from_slice(&1u16.to_le_bytes());
        bytes[22..24].copy_from_slice(&32u16.to_le_bytes());
        bytes[24..26].copy_from_slice(&2u16.to_le_bytes());
        bytes[28..36].copy_from_slice(&1u64.to_le_bytes());
        bytes[36..44].copy_from_slice(&(1024u64 * 1024).to_le_bytes());
        bytes[64..69].copy_from_slice(b"Test\0");
        bytes[96..101].copy_from_slice(b"TEST\0");
        bytes[112..121].copy_from_slice(b"TEST.APP\0");
        bytes[176..181].copy_from_slice(b"TEST\0");
        bytes[208..213].copy_from_slice(b"test\0");
        bytes
    }

    #[test]
    fn manifest_accepts_exact_shape() {
        assert_eq!(parse_manifest(&manifest()).unwrap().argument_count, 1);
    }

    #[test]
    fn manifest_accepts_printable_url_argument() {
        let mut bytes = manifest();
        let url = b"http://phipia.test/welcome.txt\0";
        bytes[208..208 + url.len()].copy_from_slice(url);
        assert!(parse_manifest(&bytes).is_ok());
    }

    #[test]
    fn manifest_rejects_control_character_argument() {
        let mut bytes = manifest();
        bytes[209] = b'\n';
        assert!(matches!(parse_manifest(&bytes), Err(Status::ManifestArgument)));
    }

    #[test]
    fn manifest_rejects_reserved_and_unknown_capability() {
        let mut bytes = manifest();
        bytes[512] = 1;
        assert!(matches!(parse_manifest(&bytes), Err(Status::ManifestReserved)));
        bytes = manifest();
        bytes[28..36].copy_from_slice(&(1u64 << 63).to_le_bytes());
        assert!(matches!(parse_manifest(&bytes), Err(Status::ManifestCapability)));
    }

    #[test]
    fn manifest_accepts_audio_capability() {
        let mut bytes = manifest();
        bytes[28..36].copy_from_slice(&(1u64 << 10).to_le_bytes());
        assert_eq!(parse_manifest(&bytes).unwrap().capabilities, 1u64 << 10);
    }

    #[test]
    fn manifest_accepts_package_capability() {
        let mut bytes = manifest();
        bytes[28..36].copy_from_slice(&(1u64 << 11).to_le_bytes());
        assert_eq!(parse_manifest(&bytes).unwrap().capabilities, 1u64 << 11);
    }

    #[test]
    fn manifest_requires_a_catalog_path_and_digest_together() {
        let mut bytes = manifest();
        bytes[464..476].copy_from_slice(b"DYNAMIC.CAT\0");
        bytes[480..512].fill(0x5a);
        let parsed = parse_manifest(&bytes).expect("paired dynamic catalog");
        assert_eq!(&parsed.dynamic_catalog[..12], b"DYNAMIC.CAT\0");
        assert_eq!(parsed.dynamic_catalog_sha256, [0x5a; 32]);

        bytes[480..512].fill(0);
        assert!(matches!(parse_manifest(&bytes), Err(Status::ManifestText)));
    }
}
