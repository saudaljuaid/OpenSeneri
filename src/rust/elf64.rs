// SPDX-License-Identifier: GPL-3.0-only
//! Parser for the one bounded ELF64 executable admitted by the Ring 3 proof.
//!
//! System V ELF Object File Format 4.3 defines the byte fields.  This module
//! decodes them individually from a checked slice.  It never overlays an ELF
//! structure, retains caller storage, allocates, or performs machine-state
//! work.  Phipia-specific collision policy remains in C.

/// The accepted file length of the v0.7.0 Ring 3 proof executable.
pub const FILE_BYTES: usize = 128;
/// The accepted file length of the bounded multiprocess executable.
pub const MULTIPROCESS_FILE_BYTES: usize = 256;
/// The longest file any admitted profile has, which bounds every buffer here.
pub const MAX_FILE_BYTES: usize = MULTIPROCESS_FILE_BYTES;
/// ELF64 header size from the System V ABI.
pub const HEADER_BYTES: u16 = 64;
/// ELF64 program-header size from the System V ABI.
pub const PROGRAM_HEADER_BYTES: u16 = 56;
/// Page alignment required by the bounded executable.
pub const PAGE_BYTES: u64 = 4096;
/// Offset of the first executable instruction in the exact file.
pub const CODE_OFFSET: u64 = 120;
/// The deterministic instruction stream carried by the load segment.
pub const CODE: [u8; 8] = [0xB8, 0x37, 0x50, 0x41, 0x53, 0xCD, 0x81, 0xF4];
/// The deterministic instruction stream carried by the multiprocess profile.
///
/// It is one bounded loop. Each pass publishes the round it has reached and
/// the identity the kernel handed it on its own private stack, then leaves
/// through the same software interrupt the Ring 3 proof uses, so the kernel
/// can save its registers and give the processor to another process. After the
/// last round it leaves with a different value in RAX, which is how the kernel
/// tells a yield from an exit. The branch at the top exists so one process can
/// be told to fault into its unmapped guard page instead: that is the control
/// that shows a failing process is terminated without disturbing its
/// neighbours. The tail is HLT, which is privileged, so a processor that ever
/// ran past the end of the program would fault rather than continue.
pub const MULTIPROCESS_CODE: [u8; 136] = [
    0x31, 0xC9, 0x48, 0x89, 0xE5, 0x48, 0xFF, 0xC1,
    0x48, 0x89, 0x4D, 0xF8, 0x48, 0x89, 0x7D, 0xF0,
    0x48, 0x39, 0xD1, 0x74, 0x1B, 0xB8, 0x4D, 0x50,
    0x41, 0x53, 0x48, 0x89, 0xCB, 0xCD, 0x81, 0x48,
    0x39, 0xF1, 0x72, 0xE1, 0xB8, 0x58, 0x50, 0x41,
    0x53, 0x48, 0x89, 0xCB, 0xCD, 0x81, 0x0F, 0x0B,
    0x48, 0xB8, 0x00, 0x00, 0x20, 0x00, 0x00, 0x40,
    0x00, 0x00, 0x48, 0xC7, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0F, 0x0B, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
    0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4, 0xF4,
];
/// Offset of the yield return address inside the multiprocess instructions.
pub const MULTIPROCESS_YIELD_RETURN_OFFSET: u64 = 0x1F;
/// Offset of the exit return address inside the multiprocess instructions.
pub const MULTIPROCESS_EXIT_RETURN_OFFSET: u64 = 0x2E;
/// Offset of the deliberate guard-page store inside the multiprocess program.
pub const MULTIPROCESS_FAULT_OFFSET: u64 = 0x3A;
/// The value the multiprocess program leaves in EAX when it yields.
pub const MULTIPROCESS_YIELD_RESULT: u32 = 0x5341_504D;
/// The value the multiprocess program leaves in EAX when it exits.
pub const MULTIPROCESS_EXIT_RESULT: u32 = 0x5341_5058;

/// One admitted executable body: how long the file is, where its instructions
/// begin, and exactly which instructions they are. Everything else about the
/// accepted subset - the identification, the single read-execute load segment,
/// the absent section table, the page-aligned canonical user placement - is
/// the same for every profile and is checked once, below.
pub struct Profile {
    /// The one accepted file length for this profile.
    pub file_bytes: usize,
    /// Offset of the first instruction, which is also the entry offset.
    pub code_offset: u64,
    /// The exact instruction bytes the file must carry.
    pub code: &'static [u8],
}

/// The v0.7.0 Ring 3 proof executable.
pub const PROOF: Profile = Profile {
    file_bytes: FILE_BYTES,
    code_offset: CODE_OFFSET,
    code: &CODE,
};

/// The bounded multiprocess executable.
pub const MULTIPROCESS: Profile = Profile {
    file_bytes: MULTIPROCESS_FILE_BYTES,
    code_offset: CODE_OFFSET,
    code: &MULTIPROCESS_CODE,
};

/// Parser controls represented by the accepted path and mutation families.
pub const ROBUSTNESS_CONTROLS: u32 = 34;

const EI_CLASS: usize = 4;
const EI_DATA: usize = 5;
const EI_VERSION: usize = 6;
const EI_OSABI: usize = 7;
const EI_ABIVERSION: usize = 8;
const EI_PAD: usize = 9;
const IDENT_BYTES: usize = 16;
const ELF_TYPE: usize = 16;
const ELF_MACHINE: usize = 18;
const ELF_VERSION: usize = 20;
const ELF_ENTRY: usize = 24;
const ELF_PROGRAM_OFFSET: usize = 32;
const ELF_SECTION_OFFSET: usize = 40;
const ELF_FLAGS: usize = 48;
const ELF_HEADER_SIZE: usize = 52;
const ELF_PROGRAM_SIZE: usize = 54;
const ELF_PROGRAM_COUNT: usize = 56;
const ELF_SECTION_SIZE: usize = 58;
const ELF_SECTION_COUNT: usize = 60;
const ELF_SECTION_NAMES: usize = 62;

const PROGRAM_TYPE: usize = 0;
const PROGRAM_FLAGS: usize = 4;
const PROGRAM_OFFSET: usize = 8;
const PROGRAM_VIRTUAL: usize = 16;
const PROGRAM_PHYSICAL: usize = 24;
const PROGRAM_FILE_SIZE: usize = 32;
const PROGRAM_MEMORY_SIZE: usize = 40;
const PROGRAM_ALIGNMENT: usize = 48;

/// A named parser conclusion.  The C mirror has the same discriminants.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    /// The entire exact image is valid.
    Ok = 0,
    /// A null pointer was presented at the C boundary.
    NullArgument = 1,
    /// A requested scalar or required file range was truncated.
    Truncated = 2,
    /// The file contains bytes beyond or short of the exact subset.
    FileLength = 3,
    /// One of the four ELF magic bytes is wrong.
    Magic = 4,
    /// The identification does not select ELFCLASS64.
    Class = 5,
    /// The identification does not select little-endian data.
    Data = 6,
    /// The identification version is not current.
    IdentVersion = 7,
    /// OSABI or ABI version is outside the System V subset.
    Abi = 8,
    /// An identification padding byte is nonzero.
    IdentPadding = 9,
    /// The object is not ET_EXEC.
    Type = 10,
    /// The machine is not EM_X86_64.
    Machine = 11,
    /// The ELF header version is not current.
    HeaderVersion = 12,
    /// Processor-specific ELF flags are nonzero.
    HeaderFlags = 13,
    /// The ELF header size is not exactly 64 bytes.
    HeaderSize = 14,
    /// The program table does not start immediately after the header.
    ProgramOffset = 15,
    /// A program-header entry is not exactly 56 bytes.
    ProgramSize = 16,
    /// The program table does not contain exactly one entry.
    ProgramCount = 17,
    /// Section-table state is present even though sections are forbidden.
    SectionTable = 18,
    /// Checked table offset, size, end, or integer conversion failed.
    ProgramTable = 19,
    /// The sole program header is not PT_LOAD.
    SegmentType = 20,
    /// The segment flags are not exactly PF_R plus PF_X.
    SegmentFlags = 21,
    /// The file offset/range is not the exact in-file extent.
    FileRange = 22,
    /// File and memory sizes are empty, unequal, or outside the subset.
    LoadSize = 23,
    /// Segment alignment or offset/address congruence is invalid.
    Alignment = 24,
    /// The virtual load range is unaligned, noncanonical, or not user-half.
    VirtualAddress = 25,
    /// Checked virtual-address or page-rounding arithmetic overflowed.
    AddressOverflow = 26,
    /// The entry is not the first proof instruction inside executable bytes.
    Entry = 27,
    /// The unused physical-address field is nonzero.
    PhysicalAddress = 28,
    /// The deterministic eight instruction bytes disagree.
    Code = 29,
}

/// Pointer-free validated executable facts returned across the C ABI.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ValidatedImage {
    /// One only after every check succeeds.
    pub valid: u32,
    /// Exactly one validated load segment.
    pub segment_count: u32,
    /// Validated ELF object type.
    pub elf_type: u16,
    /// Validated ELF machine number.
    pub machine: u16,
    /// Validated program permission flags.
    pub program_flags: u32,
    /// Validated executable entry address.
    pub entry: u64,
    /// Start of the file-backed bytes.
    pub file_offset: u64,
    /// Start of the virtual load extent.
    pub virtual_address: u64,
    /// Exact file-backed byte count.
    pub file_size: u64,
    /// Exact in-memory byte count.
    pub memory_size: u64,
    /// Validated power-of-two alignment.
    pub alignment: u64,
    /// First page included by the load mapping.
    pub mapping_start: u64,
    /// Exclusive page-rounded end of the load mapping.
    pub mapping_end: u64,
    /// Proof instruction bytes copied by value.
    pub code: [u8; 8],
}

impl ValidatedImage {
    /// A stable all-zero value written before every parse attempt.
    pub const fn invalid() -> Self {
        Self {
            valid: 0,
            segment_count: 0,
            elf_type: 0,
            machine: 0,
            program_flags: 0,
            entry: 0,
            file_offset: 0,
            virtual_address: 0,
            file_size: 0,
            memory_size: 0,
            alignment: 0,
            mapping_start: 0,
            mapping_end: 0,
            code: [0; 8],
        }
    }
}

fn byte(input: &[u8], offset: usize) -> Result<u8, Status> {
    input.get(offset).copied().ok_or(Status::Truncated)
}

fn u16_le(input: &[u8], offset: usize) -> Result<u16, Status> {
    let offset_1 = offset.checked_add(1).ok_or(Status::Truncated)?;
    Ok(u16::from(byte(input, offset)?) | (u16::from(byte(input, offset_1)?) << 8))
}

fn u32_le(input: &[u8], offset: usize) -> Result<u32, Status> {
    let mut value = 0u32;
    for shift in 0..4usize {
        let position = offset.checked_add(shift).ok_or(Status::Truncated)?;
        value |= u32::from(byte(input, position)?) << (shift * 8);
    }
    Ok(value)
}

fn u64_le(input: &[u8], offset: usize) -> Result<u64, Status> {
    let mut value = 0u64;
    for shift in 0..8usize {
        let position = offset.checked_add(shift).ok_or(Status::Truncated)?;
        value |= u64::from(byte(input, position)?) << (shift * 8);
    }
    Ok(value)
}

fn canonical_user(address: u64) -> bool {
    address <= 0x0000_7FFF_FFFF_FFFF
}

/// Decode and validate the exact ELF64 subset from one CPU-owned byte slice.
pub fn parse(input: &[u8]) -> Result<ValidatedImage, Status> {
    parse_with(input, &PROOF)
}

/// Decode and validate the bounded multiprocess executable.
pub fn parse_multiprocess(input: &[u8]) -> Result<ValidatedImage, Status> {
    parse_with(input, &MULTIPROCESS)
}

/// Decode and validate one CPU-owned byte slice against one admitted profile.
pub fn parse_with(input: &[u8], profile: &Profile) -> Result<ValidatedImage, Status> {
    if input.len() < profile.file_bytes {
        return Err(Status::Truncated);
    }
    if input.len() != profile.file_bytes {
        return Err(Status::FileLength);
    }
    if byte(input, 0)? != 0x7F
        || byte(input, 1)? != b'E'
        || byte(input, 2)? != b'L'
        || byte(input, 3)? != b'F'
    {
        return Err(Status::Magic);
    }
    if input.get(EI_CLASS).copied().ok_or(Status::Truncated)? != 2 {
        return Err(Status::Class);
    }
    if input.get(EI_DATA).copied().ok_or(Status::Truncated)? != 1 {
        return Err(Status::Data);
    }
    if input.get(EI_VERSION).copied().ok_or(Status::Truncated)? != 1 {
        return Err(Status::IdentVersion);
    }
    if input.get(EI_OSABI).copied().ok_or(Status::Truncated)? != 0
        || input.get(EI_ABIVERSION).copied().ok_or(Status::Truncated)? != 0
    {
        return Err(Status::Abi);
    }
    for offset in EI_PAD..IDENT_BYTES {
        if byte(input, offset)? != 0 {
            return Err(Status::IdentPadding);
        }
    }

    let elf_type = u16_le(input, ELF_TYPE)?;
    let machine = u16_le(input, ELF_MACHINE)?;
    let header_version = u32_le(input, ELF_VERSION)?;
    let entry = u64_le(input, ELF_ENTRY)?;
    let program_offset = u64_le(input, ELF_PROGRAM_OFFSET)?;
    let section_offset = u64_le(input, ELF_SECTION_OFFSET)?;
    let header_flags = u32_le(input, ELF_FLAGS)?;
    let header_size = u16_le(input, ELF_HEADER_SIZE)?;
    let program_size = u16_le(input, ELF_PROGRAM_SIZE)?;
    let program_count = u16_le(input, ELF_PROGRAM_COUNT)?;
    let section_size = u16_le(input, ELF_SECTION_SIZE)?;
    let section_count = u16_le(input, ELF_SECTION_COUNT)?;
    let section_names = u16_le(input, ELF_SECTION_NAMES)?;

    if elf_type != 2 {
        return Err(Status::Type);
    }
    if machine != 62 {
        return Err(Status::Machine);
    }
    if header_version != 1 {
        return Err(Status::HeaderVersion);
    }
    if header_flags != 0 {
        return Err(Status::HeaderFlags);
    }
    if header_size != HEADER_BYTES {
        return Err(Status::HeaderSize);
    }
    if program_offset != u64::from(HEADER_BYTES) {
        return Err(Status::ProgramOffset);
    }
    if program_size != PROGRAM_HEADER_BYTES {
        return Err(Status::ProgramSize);
    }
    if program_count != 1 {
        return Err(Status::ProgramCount);
    }
    if section_offset != 0 || section_size != 0 || section_count != 0 || section_names != 0 {
        return Err(Status::SectionTable);
    }

    let table_offset = usize::try_from(program_offset).map_err(|_| Status::ProgramTable)?;
    let table_bytes = usize::from(program_size)
        .checked_mul(usize::from(program_count))
        .ok_or(Status::ProgramTable)?;
    let table_end = table_offset.checked_add(table_bytes).ok_or(Status::ProgramTable)?;
    if table_end != usize::try_from(profile.code_offset).map_err(|_| Status::ProgramTable)?
        || table_end > input.len()
    {
        return Err(Status::ProgramTable);
    }

    let segment_type = u32_le(input, table_offset + PROGRAM_TYPE)?;
    let program_flags = u32_le(input, table_offset + PROGRAM_FLAGS)?;
    let file_offset = u64_le(input, table_offset + PROGRAM_OFFSET)?;
    let virtual_address = u64_le(input, table_offset + PROGRAM_VIRTUAL)?;
    let physical_address = u64_le(input, table_offset + PROGRAM_PHYSICAL)?;
    let file_size = u64_le(input, table_offset + PROGRAM_FILE_SIZE)?;
    let memory_size = u64_le(input, table_offset + PROGRAM_MEMORY_SIZE)?;
    let alignment = u64_le(input, table_offset + PROGRAM_ALIGNMENT)?;

    if segment_type != 1 {
        return Err(Status::SegmentType);
    }
    if program_flags != 5 {
        return Err(Status::SegmentFlags);
    }
    if physical_address != 0 {
        return Err(Status::PhysicalAddress);
    }
    if file_offset != 0 {
        return Err(Status::FileRange);
    }
    if file_size != profile.file_bytes as u64 || memory_size != profile.file_bytes as u64 {
        return Err(Status::LoadSize);
    }
    let file_end = file_offset.checked_add(file_size).ok_or(Status::FileRange)?;
    if file_end != input.len() as u64 {
        return Err(Status::FileRange);
    }
    if alignment != PAGE_BYTES || !alignment.is_power_of_two() {
        return Err(Status::Alignment);
    }
    if virtual_address & (alignment - 1) != file_offset & (alignment - 1) {
        return Err(Status::Alignment);
    }
    if virtual_address & (PAGE_BYTES - 1) != 0 || !canonical_user(virtual_address) {
        return Err(Status::VirtualAddress);
    }
    let virtual_end = virtual_address
        .checked_add(memory_size)
        .ok_or(Status::AddressOverflow)?;
    if virtual_end == 0 || !canonical_user(virtual_end - 1) {
        return Err(Status::VirtualAddress);
    }
    let mapping_end = virtual_end
        .checked_add(PAGE_BYTES - 1)
        .ok_or(Status::AddressOverflow)?
        & !(PAGE_BYTES - 1);
    if mapping_end <= virtual_address || !canonical_user(mapping_end - 1) {
        return Err(Status::VirtualAddress);
    }
    let expected_entry = virtual_address
        .checked_add(profile.code_offset)
        .ok_or(Status::AddressOverflow)?;
    let executable_end = virtual_address.checked_add(file_size).ok_or(Status::AddressOverflow)?;
    if entry != expected_entry || entry < virtual_address || entry >= executable_end {
        return Err(Status::Entry);
    }
    let code_offset = usize::try_from(profile.code_offset).map_err(|_| Status::FileRange)?;
    for (index, expected) in profile.code.iter().copied().enumerate() {
        let position = code_offset.checked_add(index).ok_or(Status::FileRange)?;
        if byte(input, position)? != expected {
            return Err(Status::Code);
        }
    }

    Ok(ValidatedImage {
        valid: 1,
        segment_count: 1,
        elf_type,
        machine,
        program_flags,
        entry,
        file_offset,
        virtual_address,
        file_size,
        memory_size,
        alignment,
        mapping_start: virtual_address,
        mapping_end,
        code: leading_code(profile),
    })
}

/// The first eight instruction bytes, which is what the C receipt carries.
fn leading_code(profile: &Profile) -> [u8; 8] {
    let mut leading = [0u8; 8];

    for index in 0..8usize {
        leading[index] = match profile.code.get(index).copied() {
            Some(value) => value,
            None => 0,
        };
    }
    leading
}

fn put_byte(image: &mut [u8], offset: usize, value: u8) -> bool {
    match image.get_mut(offset) {
        Some(destination) => {
            *destination = value;
            true
        }
        None => false,
    }
}

fn put_u16(image: &mut [u8], offset: usize, value: u16) -> bool {
    put_integer(image, offset, u64::from(value), 2)
}

fn put_u32(image: &mut [u8], offset: usize, value: u32) -> bool {
    put_integer(image, offset, u64::from(value), 4)
}

fn put_u64(image: &mut [u8], offset: usize, value: u64) -> bool {
    put_integer(image, offset, value, 8)
}

fn put_integer(
    image: &mut [u8],
    offset: usize,
    value: u64,
    width: usize,
) -> bool {
    for index in 0..width {
        let Some(position) = offset.checked_add(index) else {
            return false;
        };
        if !put_byte(image, position, (value >> (index * 8)) as u8) {
            return false;
        }
    }
    true
}

fn write_fixture(image: &mut [u8], profile: &Profile) -> bool {
    let ident = [
        0x7F, b'E', b'L', b'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    ];
    for (offset, value) in ident.iter().copied().enumerate() {
        if !put_byte(image, offset, value) {
            return false;
        }
    }
    if !put_u16(image, ELF_TYPE, 2)
        || !put_u16(image, ELF_MACHINE, 62)
        || !put_u32(image, ELF_VERSION, 1)
        || !put_u64(image, ELF_ENTRY, 0x0000_4000_0000_0078)
        || !put_u64(image, ELF_PROGRAM_OFFSET, 64)
        || !put_u16(image, ELF_HEADER_SIZE, 64)
        || !put_u16(image, ELF_PROGRAM_SIZE, 56)
        || !put_u16(image, ELF_PROGRAM_COUNT, 1)
        || !put_u32(image, 64 + PROGRAM_TYPE, 1)
        || !put_u32(image, 64 + PROGRAM_FLAGS, 5)
        || !put_u64(image, 64 + PROGRAM_VIRTUAL, 0x0000_4000_0000_0000)
        || !put_u64(image, 64 + PROGRAM_FILE_SIZE, profile.file_bytes as u64)
        || !put_u64(image, 64 + PROGRAM_MEMORY_SIZE, profile.file_bytes as u64)
        || !put_u64(image, 64 + PROGRAM_ALIGNMENT, 4096)
    {
        return false;
    }
    let Ok(code_offset) = usize::try_from(profile.code_offset) else {
        return false;
    };
    for (index, value) in profile.code.iter().copied().enumerate() {
        let Some(offset) = code_offset.checked_add(index) else {
            return false;
        };
        if !put_byte(image, offset, value) {
            return false;
        }
    }
    true
}

fn fixture(profile: &Profile) -> [u8; MAX_FILE_BYTES] {
    let mut image = [0u8; MAX_FILE_BYTES];
    let Some(body) = image.get_mut(..profile.file_bytes) else {
        return [0; MAX_FILE_BYTES];
    };
    if !write_fixture(body, profile) {
        return [0; MAX_FILE_BYTES];
    }
    image
}

fn rejects(image: &[u8], profile: &Profile, expected: Status) -> bool {
    matches!(parse_with(image, profile), Err(found) if found == expected)
}

/// Exercise and count the accepted image and mutation families 1 through 34.
pub fn self_test() -> u32 {
    self_test_with(&PROOF)
}

/// The same thirty-four controls applied to the multiprocess profile.
pub fn self_test_multiprocess() -> u32 {
    self_test_with(&MULTIPROCESS)
}

/// Exercise and count one profile's accepted image and mutation families.
pub fn self_test_with(profile: &Profile) -> u32 {
    let mut passed = 0u32;
    let stored = fixture(profile);
    let Some(base) = stored.get(..profile.file_bytes) else {
        return 0;
    };
    let accepted = match parse_with(base, profile) {
        Ok(value) => value,
        Err(_) => return 0,
    };
    if accepted.valid != 1
        || accepted.segment_count != 1
        || accepted.mapping_end - accepted.mapping_start != PAGE_BYTES
        || accepted.code != leading_code(profile)
    {
        return 0;
    }
    passed += 1;
    for length in 0..profile.file_bytes {
        let Some(truncated) = base.get(..length) else {
            return 0;
        };
        if !rejects(truncated, profile, Status::Truncated) {
            return 0;
        }
    }
    passed += 1;
    let mut long = [0u8; MAX_FILE_BYTES + 1];
    let Some(long_body) = long.get_mut(..profile.file_bytes) else {
        return 0;
    };
    if !write_fixture(long_body, profile) {
        return 0;
    }
    let Some(long_image) = long.get(..profile.file_bytes + 1) else {
        return 0;
    };
    if !rejects(long_image, profile, Status::FileLength) {
        return 0;
    }
    passed += 1;

    let Ok(code_offset) = usize::try_from(profile.code_offset) else {
        return 0;
    };
    let mutations: &[(usize, u8, Status)] = &[
        (0, 0, Status::Magic),
        (EI_CLASS, 1, Status::Class),
        (EI_DATA, 2, Status::Data),
        (EI_VERSION, 0, Status::IdentVersion),
        (EI_OSABI, 3, Status::Abi),
        (EI_ABIVERSION, 1, Status::Abi),
        (EI_PAD, 1, Status::IdentPadding),
        (ELF_TYPE, 3, Status::Type),
        (ELF_MACHINE, 3, Status::Machine),
        (ELF_VERSION, 2, Status::HeaderVersion),
        (ELF_FLAGS, 1, Status::HeaderFlags),
        (ELF_HEADER_SIZE, 63, Status::HeaderSize),
        (ELF_PROGRAM_OFFSET, 63, Status::ProgramOffset),
        (ELF_PROGRAM_SIZE, 55, Status::ProgramSize),
        (ELF_PROGRAM_COUNT, 2, Status::ProgramCount),
        (ELF_SECTION_OFFSET, 1, Status::SectionTable),
        (ELF_SECTION_SIZE, 1, Status::SectionTable),
        (ELF_SECTION_COUNT, 1, Status::SectionTable),
        (ELF_SECTION_NAMES, 1, Status::SectionTable),
        (64 + PROGRAM_TYPE, 2, Status::SegmentType),
        (64 + PROGRAM_FLAGS, 7, Status::SegmentFlags),
        (64 + PROGRAM_OFFSET, 1, Status::FileRange),
        (64 + PROGRAM_FILE_SIZE, 0xFF, Status::LoadSize),
        (64 + PROGRAM_MEMORY_SIZE, 0xFF, Status::LoadSize),
        (64 + PROGRAM_ALIGNMENT, 1, Status::Alignment),
        (64 + PROGRAM_VIRTUAL, 1, Status::Alignment),
        (64 + PROGRAM_PHYSICAL, 1, Status::PhysicalAddress),
        (ELF_ENTRY, 0, Status::Entry),
        (code_offset, 0x90, Status::Code),
    ];
    let mut storage = fixture(profile);
    for &(offset, value, expected) in mutations {
        let Some(changed) = storage.get_mut(..profile.file_bytes) else {
            return 0;
        };
        let original = match changed.get(offset).copied() {
            Some(found) => found,
            None => return 0,
        };
        let Some(destination) = changed.get_mut(offset) else {
            return 0;
        };
        *destination = value;
        if !rejects(changed, profile, expected) {
            return 0;
        }
        if !put_byte(changed, offset, original) {
            return 0;
        }
        passed += 1;
    }

    let Some(changed) = storage.get_mut(..profile.file_bytes) else {
        return 0;
    };
    if !put_u64(changed, 64 + PROGRAM_VIRTUAL, 0xFFFF_8000_0000_0000)
        || !put_u64(changed, ELF_ENTRY, 0xFFFF_8000_0000_0000 + profile.code_offset)
    {
        return 0;
    }
    if !rejects(changed, profile, Status::VirtualAddress) {
        return 0;
    }
    passed += 1;
    if !put_u64(changed, 64 + PROGRAM_VIRTUAL, u64::MAX & !0xFFF)
        || !put_u64(changed, ELF_ENTRY, (u64::MAX & !0xFFF) + profile.code_offset)
    {
        return 0;
    }
    if !rejects(changed, profile, Status::VirtualAddress) {
        return 0;
    }
    passed += 1;
    passed
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepted_image_is_pointer_free_and_page_rounded() {
        let stored = fixture(&PROOF);
        let parsed = parse(&stored[..PROOF.file_bytes]).expect("exact ELF must parse");
        assert_eq!(parsed.virtual_address, 0x0000_4000_0000_0000);
        assert_eq!(parsed.entry, 0x0000_4000_0000_0078);
        assert_eq!(parsed.mapping_end, 0x0000_4000_0000_1000);
    }

    #[test]
    fn every_frozen_parser_control_passes() {
        assert_eq!(self_test(), ROBUSTNESS_CONTROLS);
    }

    #[test]
    fn multiprocess_image_is_one_page_of_read_execute_bytes() {
        let stored = fixture(&MULTIPROCESS);
        let parsed = parse_multiprocess(&stored[..MULTIPROCESS.file_bytes])
            .expect("multiprocess ELF must parse");
        assert_eq!(parsed.file_size, MULTIPROCESS_FILE_BYTES as u64);
        assert_eq!(parsed.program_flags, 5);
        assert_eq!(parsed.entry, 0x0000_4000_0000_0078);
        assert_eq!(parsed.mapping_end, 0x0000_4000_0000_1000);
    }

    #[test]
    fn every_multiprocess_parser_control_passes() {
        assert_eq!(self_test_multiprocess(), ROBUSTNESS_CONTROLS);
    }

    #[test]
    fn the_two_profiles_refuse_each_other() {
        let proof = fixture(&PROOF);
        let multiprocess = fixture(&MULTIPROCESS);
        assert!(parse_multiprocess(&proof[..PROOF.file_bytes]).is_err());
        assert!(parse(&multiprocess[..MULTIPROCESS.file_bytes]).is_err());
    }

    #[test]
    fn the_multiprocess_return_offsets_name_real_instructions() {
        let yield_return = MULTIPROCESS_YIELD_RETURN_OFFSET as usize;
        let exit_return = MULTIPROCESS_EXIT_RETURN_OFFSET as usize;
        let fault = MULTIPROCESS_FAULT_OFFSET as usize;

        assert_eq!(MULTIPROCESS_CODE[yield_return - 2], 0xCD);
        assert_eq!(MULTIPROCESS_CODE[yield_return - 1], 0x81);
        assert_eq!(MULTIPROCESS_CODE[exit_return - 2], 0xCD);
        assert_eq!(MULTIPROCESS_CODE[exit_return - 1], 0x81);
        assert_eq!(MULTIPROCESS_CODE[fault], 0x48);
        assert_eq!(MULTIPROCESS_CODE[fault + 1], 0xC7);
    }

    #[test]
    fn the_multiprocess_markers_are_the_immediates_the_program_loads() {
        let yield_immediate = u32::from_le_bytes([
            MULTIPROCESS_CODE[0x16],
            MULTIPROCESS_CODE[0x17],
            MULTIPROCESS_CODE[0x18],
            MULTIPROCESS_CODE[0x19],
        ]);
        let exit_immediate = u32::from_le_bytes([
            MULTIPROCESS_CODE[0x25],
            MULTIPROCESS_CODE[0x26],
            MULTIPROCESS_CODE[0x27],
            MULTIPROCESS_CODE[0x28],
        ]);

        assert_eq!(MULTIPROCESS_CODE[0x15], 0xB8);
        assert_eq!(MULTIPROCESS_CODE[0x24], 0xB8);
        assert_eq!(yield_immediate, MULTIPROCESS_YIELD_RESULT);
        assert_eq!(exit_immediate, MULTIPROCESS_EXIT_RESULT);
        assert_ne!(MULTIPROCESS_YIELD_RESULT, MULTIPROCESS_EXIT_RESULT);
    }

    #[test]
    fn the_multiprocess_tail_is_a_privileged_instruction() {
        let body = MULTIPROCESS_FAULT_OFFSET as usize + 9;

        for byte in MULTIPROCESS_CODE.iter().skip(body).copied() {
            assert_eq!(byte, 0xF4);
        }
    }
}
