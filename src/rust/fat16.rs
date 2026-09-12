// SPDX-License-Identifier: GPL-3.0-only
//! The deliberately bounded, read-only FAT16 metadata parser.
//!
//! The layout follows Microsoft "FAT: General Overview of On-Disk Format",
//! version 1.03 (December 6, 2000), as incorporated by UEFI 2.11 section
//! 13.3. Sector fields are little-endian and are decoded from checked slices;
//! no packed structure ever points into a controller-owned block.

/// The supported namespace and FAT sector size.
pub const BLOCK_BYTES: usize = 4096;
/// The one accepted total-sector count.
pub const TOTAL_SECTORS: u64 = 4096;
/// The one accepted root-directory entry count.
pub const ROOT_ENTRIES: u32 = 128;
/// The one accepted file length.
pub const FILE_BYTES: u32 = 128;
/// The one accepted first data cluster.
pub const FILE_CLUSTER: u16 = 2;
/// The number of parser controls represented by controls 1 through 22.
pub const ROBUSTNESS_CONTROLS: u32 = 22;
/// Canonical on-disk 8.3 bytes for `PHIPIA.BIN`.
pub const PHIPIA_NAME: [u8; 11] = *b"PHIPIA  BIN";
/// SHA-256 of the deterministic 128-byte fixture payload.
pub const PAYLOAD_SHA256: [u8; 32] = [
    0xD3, 0x99, 0xF0, 0x65, 0xC9, 0xF2, 0x1E, 0x2F,
    0xD5, 0x1E, 0x2A, 0xEA, 0xDB, 0x77, 0x68, 0xEA,
    0xB7, 0xE6, 0xE4, 0x5E, 0x51, 0x50, 0xF3, 0x12,
    0x27, 0xC9, 0x71, 0x19, 0x34, 0xA4, 0xD1, 0xD3,
];

const FAT12_MAX_CLUSTERS: u64 = 4084;
const FAT16_MAX_CLUSTERS: u64 = 65_524;
const FAT16_EOC_MIN: u16 = 0xFFF8;
#[cfg(test)]
const FAT16_BAD: u16 = 0xFFF7;
#[cfg(test)]
const FAT16_RESERVED_MIN: u16 = 0xFFF0;
const MEDIA_FIXED: u8 = 0xF8;
const DIRECTORY_ENTRY_BYTES: u64 = 32;
const ATTR_ARCHIVE: u8 = 0x20;
const ATTR_LONG_NAME: u8 = 0x0F;

const BPB_BYTES_PER_SECTOR: usize = 11;
const BPB_SECTORS_PER_CLUSTER: usize = 13;
const BPB_RESERVED_SECTORS: usize = 14;
const BPB_FAT_COUNT: usize = 16;
const BPB_ROOT_ENTRIES: usize = 17;
const BPB_TOTAL_SECTORS_16: usize = 19;
const BPB_MEDIA: usize = 21;
const BPB_FAT_SECTORS_16: usize = 22;
const BPB_HIDDEN_SECTORS: usize = 28;
const BPB_TOTAL_SECTORS_32: usize = 32;
const EBPB_RESERVED: usize = 37;
const EBPB_SIGNATURE: usize = 38;
const BOOT_SIGNATURE: usize = 510;

/// A named parser conclusion. The C mirror has the same discriminants.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Status {
    /// Every field and derived value matches the bounded contract.
    Ok = 0,
    /// A null pointer was presented at the C boundary.
    NullArgument = 1,
    /// A block or field ended before the required byte range.
    Truncated = 2,
    /// The EBPB or byte-510 boot signature is invalid.
    BootSignature = 3,
    /// The FAT sector size is unsupported or differs from the namespace.
    BytesPerSector = 4,
    /// The cluster size is outside the one-sector subset.
    SectorsPerCluster = 5,
    /// The reserved-sector count is outside the subset.
    ReservedCount = 6,
    /// The number of FATs is outside the subset.
    FatCount = 7,
    /// Total-sector variants conflict or differ from the subset.
    TotalSectors = 8,
    /// The FAT-size field is zero, unsupported, or cannot cover the clusters.
    FatSize = 9,
    /// The fixed-root geometry is zero, misaligned, or unsupported.
    RootGeometry = 10,
    /// Checked geometry arithmetic overflowed.
    SpanOverflow = 11,
    /// A derived span overlaps or exceeds the volume or namespace.
    SpanRange = 12,
    /// The derived cluster count classifies as FAT12 or FAT32.
    FatClass = 13,
    /// The BPB and FAT media descriptors disagree.
    Media = 14,
    /// FAT16 reserved entries zero and one are malformed.
    FatReserved = 15,
    /// The fixed root directory has no bounded end marker.
    RootEndMissing = 16,
    /// The canonical target is absent.
    TargetAbsent = 17,
    /// More than one canonical target is active.
    TargetDuplicate = 18,
    /// A long-file-name entry is unsupported.
    LongName = 19,
    /// A deleted directory entry is unsupported by the exact subset.
    Deleted = 20,
    /// A volume label, directory, unrelated file, or attribute is unsupported.
    UnsupportedEntry = 21,
    /// An 8.3 query or active short name is noncanonical.
    NameMalformed = 22,
    /// The file cluster is below two or outside the data-cluster range.
    ClusterRange = 23,
    /// The FAT entry is free, bad, reserved, or outside the data range.
    FatEntry = 24,
    /// The one-cluster file points at another data cluster.
    MultiCluster = 25,
    /// The file length is zero, oversized, or exceeds its destination.
    FileSize = 26,
    /// Cluster-to-sector translation overflowed or escaped the namespace.
    ClusterTranslation = 27,
    /// Allocated directory state appears after the declared end marker.
    TrailingState = 28,
    /// File data is not exactly 128 bytes.
    PayloadLength = 29,
    /// One or more deterministic file bytes disagree.
    PayloadContent = 30,
    /// The computed file digest differs from the documented SHA-256.
    PayloadDigest = 31,
}

/// A candidate FAT16 superfloppy before derived geometry is trusted.
#[derive(Clone, Copy)]
pub struct CandidateVolume {
    bytes_per_sector: u16,
    sectors_per_cluster: u8,
    reserved_sectors: u16,
    fat_count: u8,
    root_entries: u16,
    total_sectors_16: u16,
    total_sectors_32: u32,
    media: u8,
    fat_sectors: u16,
    hidden_sectors: u32,
}

/// Fully checked FAT16 geometry returned across the C ABI by value.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Geometry {
    /// Sectors declared by the selected BPB total-sector field.
    pub total_sectors: u64,
    /// Fixed root-directory length in sectors, rounded up safely.
    pub root_dir_sectors: u64,
    /// First FAT sector relative to volume LBA zero.
    pub first_fat_sector: u64,
    /// Length of the one FAT in sectors.
    pub fat_sectors: u64,
    /// First fixed root-directory sector.
    pub first_root_sector: u64,
    /// First data sector and first sector of cluster two.
    pub first_data_sector: u64,
    /// Data-region sector count.
    pub data_sectors: u64,
    /// Count used normatively to classify FAT12/16/32.
    pub cluster_count: u64,
    /// Identified NVMe namespace length in logical blocks.
    pub namespace_blocks: u64,
    /// Validated namespace and FAT bytes per sector.
    pub bytes_per_sector: u32,
    /// Validated sectors in each cluster.
    pub sectors_per_cluster: u32,
    /// Validated count of fixed root entries.
    pub root_entries: u32,
    /// Validated BPB media descriptor, widened for stable ABI layout.
    pub media: u32,
}

impl Geometry {
    /// An invalid value used to zero C outputs before every parse.
    pub const fn invalid() -> Self {
        Self {
            total_sectors: 0,
            root_dir_sectors: 0,
            first_fat_sector: 0,
            fat_sectors: 0,
            first_root_sector: 0,
            first_data_sector: 0,
            data_sectors: 0,
            cluster_count: 0,
            namespace_blocks: 0,
            bytes_per_sector: 0,
            sectors_per_cluster: 0,
            root_entries: 0,
            media: 0,
        }
    }
}

/// A canonical raw 8.3 query with no pointer or retained caller storage.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct RootQuery {
    /// Eight space-padded base bytes followed by three extension bytes.
    pub canonical_name: [u8; 11],
}

impl RootQuery {
    /// An invalid zero query used at the ABI boundary.
    pub const fn invalid() -> Self {
        Self { canonical_name: [0; 11] }
    }
}

/// One validated regular root-directory entry.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct RootEntry {
    /// Canonical raw 8.3 bytes copied by value.
    pub canonical_name: [u8; 11],
    /// The exact supported regular-file attribute byte.
    pub attribute: u8,
    /// Validated first cluster.
    pub first_cluster: u16,
    /// Validated nonzero byte length.
    pub file_size: u32,
}

/// Pointer-free facts captured from the first FAT sector before root parsing.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct FatState {
    /// FAT entry zero, including the BPB media descriptor in its low byte.
    pub media_entry: u16,
    /// FAT entry one, reserved by the format.
    pub reserved_entry: u16,
    /// The exact cluster whose entry was inspected.
    pub file_cluster: u16,
    /// The FAT16 value for cluster two.
    pub file_entry: u16,
}

impl FatState {
    /// An invalid value used to zero C outputs.
    pub const fn invalid() -> Self {
        Self {
            media_entry: 0,
            reserved_entry: 0,
            file_cluster: 0,
            file_entry: 0,
        }
    }
}

impl RootEntry {
    /// An invalid value used to zero C outputs.
    pub const fn invalid() -> Self {
        Self {
            canonical_name: [0; 11],
            attribute: 0,
            first_cluster: 0,
            file_size: 0,
        }
    }
}

/// A validated one-cluster file extent.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Extent {
    /// The validated data cluster.
    pub cluster: u16,
    /// Its validated FAT16 end-of-chain value.
    pub fat_value: u16,
    /// Metadata-derived namespace LBA.
    pub lba: u64,
    /// File bytes contained by this one cluster.
    pub file_size: u32,
    /// Cluster capacity in bytes.
    pub cluster_bytes: u32,
}

impl Extent {
    /// An invalid value used to zero C outputs.
    pub const fn invalid() -> Self {
        Self {
            cluster: 0,
            fat_value: 0,
            lba: 0,
            file_size: 0,
            cluster_bytes: 0,
        }
    }
}

/// Checked facts about the deterministic file contents.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Payload {
    /// SHA-256 computed from the DMA-originated 128 bytes.
    pub sha256: [u8; 32],
    /// Validated byte count.
    pub byte_count: u32,
    /// One only after every deterministic byte and digest matched.
    pub deterministic: u32,
}

impl Payload {
    /// An invalid value used to zero C outputs.
    pub const fn invalid() -> Self {
        Self { sha256: [0; 32], byte_count: 0, deterministic: 0 }
    }
}

fn field(bytes: &[u8], offset: usize, length: usize) -> Result<&[u8], Status> {
    let end = offset.checked_add(length).ok_or(Status::SpanOverflow)?;
    bytes.get(offset..end).ok_or(Status::Truncated)
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16, Status> {
    let value = field(bytes, offset, 2)?;
    Ok(u16::from_le_bytes([value[0], value[1]]))
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32, Status> {
    let value = field(bytes, offset, 4)?;
    Ok(u32::from_le_bytes([value[0], value[1], value[2], value[3]]))
}

fn exact_block(bytes: &[u8]) -> Result<(), Status> {
    if bytes.len() != BLOCK_BYTES {
        return Err(Status::Truncated);
    }
    Ok(())
}

fn canonical_component(component: &[u8], required: bool) -> bool {
    let mut saw_character = false;
    let mut saw_space = false;
    for byte in component {
        if *byte == b' ' {
            saw_space = true;
        } else if byte.is_ascii_uppercase() || byte.is_ascii_digit() {
            if saw_space {
                return false;
            }
            saw_character = true;
        } else {
            return false;
        }
    }
    saw_character || !required
}

fn canonical_name(name: &[u8]) -> bool {
    name.len() == 11
        && canonical_component(&name[..8], true)
        && canonical_component(&name[8..], true)
}

fn validate_geometry(geometry: &Geometry) -> Result<(), Status> {
    let fat_end = geometry.first_fat_sector
        .checked_add(geometry.fat_sectors)
        .ok_or(Status::SpanOverflow)?;
    let root_end = geometry.first_root_sector
        .checked_add(geometry.root_dir_sectors)
        .ok_or(Status::SpanOverflow)?;
    let data_end = geometry.first_data_sector
        .checked_add(geometry.data_sectors)
        .ok_or(Status::SpanOverflow)?;
    if geometry.bytes_per_sector != BLOCK_BYTES as u32
        || geometry.sectors_per_cluster != 1
        || geometry.root_entries != ROOT_ENTRIES
        || geometry.media != u32::from(MEDIA_FIXED)
        || geometry.total_sectors != TOTAL_SECTORS
        || geometry.root_dir_sectors != 1
        || geometry.first_fat_sector != 1
        || geometry.fat_sectors != 2
        || fat_end != geometry.first_root_sector
        || root_end != geometry.first_data_sector
        || data_end != geometry.total_sectors
        || geometry.total_sectors > geometry.namespace_blocks
        || geometry.cluster_count != geometry.data_sectors
    {
        return Err(Status::SpanRange);
    }
    Ok(())
}

/// Decode and validate the exact BPB/EBPB and derive every subsequent LBA.
pub fn parse_bpb(
    block: &[u8],
    namespace_blocks: u64,
    namespace_block_bytes: u32,
) -> Result<Geometry, Status> {
    exact_block(block)?;
    if field(block, BOOT_SIGNATURE, 2)? != [0x55, 0xAA]
        || block[EBPB_SIGNATURE] != 0x29
        || block[EBPB_RESERVED] != 0
    {
        return Err(Status::BootSignature);
    }
    let candidate = CandidateVolume {
        bytes_per_sector: read_u16(block, BPB_BYTES_PER_SECTOR)?,
        sectors_per_cluster: block[BPB_SECTORS_PER_CLUSTER],
        reserved_sectors: read_u16(block, BPB_RESERVED_SECTORS)?,
        fat_count: block[BPB_FAT_COUNT],
        root_entries: read_u16(block, BPB_ROOT_ENTRIES)?,
        total_sectors_16: read_u16(block, BPB_TOTAL_SECTORS_16)?,
        total_sectors_32: read_u32(block, BPB_TOTAL_SECTORS_32)?,
        media: block[BPB_MEDIA],
        fat_sectors: read_u16(block, BPB_FAT_SECTORS_16)?,
        hidden_sectors: read_u32(block, BPB_HIDDEN_SECTORS)?,
    };
    if candidate.bytes_per_sector != BLOCK_BYTES as u16
        || namespace_block_bytes != BLOCK_BYTES as u32
        || u32::from(candidate.bytes_per_sector) != namespace_block_bytes
    {
        return Err(Status::BytesPerSector);
    }
    if candidate.sectors_per_cluster != 1 {
        return Err(Status::SectorsPerCluster);
    }
    if candidate.reserved_sectors != 1 {
        return Err(Status::ReservedCount);
    }
    if candidate.fat_count != 1 {
        return Err(Status::FatCount);
    }
    if candidate.root_entries == 0 {
        return Err(Status::RootGeometry);
    }
    if candidate.fat_sectors == 0 {
        return Err(Status::FatSize);
    }
    if candidate.media != MEDIA_FIXED {
        return Err(Status::Media);
    }
    if candidate.hidden_sectors != 0 {
        return Err(Status::SpanRange);
    }
    let total_sectors = match (
        candidate.total_sectors_16,
        candidate.total_sectors_32,
    ) {
        (0, 0) | (1.., 1..) => return Err(Status::TotalSectors),
        (value, 0) => u64::from(value),
        (0, value) => u64::from(value),
    };
    let root_bytes = u64::from(candidate.root_entries)
        .checked_mul(DIRECTORY_ENTRY_BYTES)
        .ok_or(Status::SpanOverflow)?;
    let sector_bytes = u64::from(candidate.bytes_per_sector);
    let rounded_root = root_bytes
        .checked_add(sector_bytes.checked_sub(1).ok_or(Status::RootGeometry)?)
        .ok_or(Status::SpanOverflow)?;
    let root_dir_sectors = rounded_root
        .checked_div(sector_bytes)
        .ok_or(Status::RootGeometry)?;
    if root_dir_sectors == 0 || root_bytes % sector_bytes != 0 {
        return Err(Status::RootGeometry);
    }
    let fat_span = u64::from(candidate.fat_count)
        .checked_mul(u64::from(candidate.fat_sectors))
        .ok_or(Status::SpanOverflow)?;
    let first_fat_sector = u64::from(candidate.reserved_sectors);
    let first_root_sector = first_fat_sector
        .checked_add(fat_span)
        .ok_or(Status::SpanOverflow)?;
    let first_data_sector = first_root_sector
        .checked_add(root_dir_sectors)
        .ok_or(Status::SpanOverflow)?;
    let data_sectors = total_sectors
        .checked_sub(first_data_sector)
        .ok_or(Status::SpanRange)?;
    let cluster_count = data_sectors
        .checked_div(u64::from(candidate.sectors_per_cluster))
        .ok_or(Status::SectorsPerCluster)?;
    if cluster_count <= FAT12_MAX_CLUSTERS
        || cluster_count > FAT16_MAX_CLUSTERS
    {
        return Err(Status::FatClass);
    }
    if candidate.total_sectors_16 != TOTAL_SECTORS as u16
        || candidate.total_sectors_32 != 0
    {
        return Err(Status::TotalSectors);
    }
    if candidate.root_entries != ROOT_ENTRIES as u16 {
        return Err(Status::RootGeometry);
    }
    if candidate.fat_sectors != 2 {
        return Err(Status::FatSize);
    }
    let fat_bytes = u64::from(candidate.fat_sectors)
        .checked_mul(sector_bytes)
        .ok_or(Status::SpanOverflow)?;
    let fat_entries = fat_bytes.checked_div(2).ok_or(Status::FatSize)?;
    let required_entries = cluster_count.checked_add(2).ok_or(Status::SpanOverflow)?;
    if fat_entries < required_entries {
        return Err(Status::FatSize);
    }
    let geometry = Geometry {
        total_sectors,
        root_dir_sectors,
        first_fat_sector,
        fat_sectors: u64::from(candidate.fat_sectors),
        first_root_sector,
        first_data_sector,
        data_sectors,
        cluster_count,
        namespace_blocks,
        bytes_per_sector: u32::from(candidate.bytes_per_sector),
        sectors_per_cluster: u32::from(candidate.sectors_per_cluster),
        root_entries: u32::from(candidate.root_entries),
        media: u32::from(candidate.media),
    };
    validate_geometry(&geometry)?;
    Ok(geometry)
}

/// Validate and copy the one canonical root query.
pub fn make_query(name: &[u8]) -> Result<RootQuery, Status> {
    if !canonical_name(name) || name != PHIPIA_NAME {
        return Err(Status::NameMalformed);
    }
    let canonical = [
        name[0], name[1], name[2], name[3], name[4], name[5],
        name[6], name[7], name[8], name[9], name[10],
    ];
    Ok(RootQuery { canonical_name: canonical })
}

/// Scan the one fixed root sector and return exactly one regular file.
pub fn find_root(
    block: &[u8],
    geometry: &Geometry,
    query: &RootQuery,
    destination_bytes: u32,
) -> Result<RootEntry, Status> {
    exact_block(block)?;
    validate_geometry(geometry)?;
    if !canonical_name(&query.canonical_name)
        || query.canonical_name != PHIPIA_NAME
    {
        return Err(Status::NameMalformed);
    }
    let entries = usize::try_from(geometry.root_entries)
        .map_err(|_| Status::RootGeometry)?;
    let mut end_index: Option<usize> = None;
    for index in 0..entries {
        let offset = index.checked_mul(32).ok_or(Status::SpanOverflow)?;
        if field(block, offset, 1)?[0] == 0 {
            end_index = Some(index);
            break;
        }
    }
    let end = end_index.ok_or(Status::RootEndMissing)?;
    let mut found: Option<RootEntry> = None;
    for index in 0..end {
        let offset = index.checked_mul(32).ok_or(Status::SpanOverflow)?;
        let entry = field(block, offset, 32)?;
        match entry[0] {
            0xE5 => return Err(Status::Deleted),
            _ => {}
        }
        let attribute = entry[11];
        if attribute & ATTR_LONG_NAME == ATTR_LONG_NAME {
            return Err(Status::LongName);
        }
        if attribute != ATTR_ARCHIVE {
            return Err(Status::UnsupportedEntry);
        }
        let name = &entry[..11];
        if !canonical_name(name) {
            return Err(Status::NameMalformed);
        }
        if name != query.canonical_name {
            return Err(Status::UnsupportedEntry);
        }
        if found.is_some() {
            return Err(Status::TargetDuplicate);
        }
        let high_cluster = read_u16(entry, 20)?;
        let first_cluster = read_u16(entry, 26)?;
        let file_size = read_u32(entry, 28)?;
        let maximum_cluster = geometry.cluster_count
            .checked_add(1)
            .ok_or(Status::SpanOverflow)?;
        if high_cluster != 0 || first_cluster < 2
            || u64::from(first_cluster) > maximum_cluster
            || first_cluster != FILE_CLUSTER
        {
            return Err(Status::ClusterRange);
        }
        let cluster_bytes = geometry.bytes_per_sector
            .checked_mul(geometry.sectors_per_cluster)
            .ok_or(Status::SpanOverflow)?;
        if file_size == 0 || file_size != FILE_BYTES
            || file_size > cluster_bytes || file_size > destination_bytes
        {
            return Err(Status::FileSize);
        }
        let canonical = [
            name[0], name[1], name[2], name[3], name[4], name[5],
            name[6], name[7], name[8], name[9], name[10],
        ];
        found = Some(RootEntry {
            canonical_name: canonical,
            attribute,
            first_cluster,
            file_size,
        });
    }
    for index in end..entries {
        let offset = index.checked_mul(32).ok_or(Status::SpanOverflow)?;
        let skip = usize::from(index == end);
        let trailing_offset = offset.checked_add(skip).ok_or(Status::SpanOverflow)?;
        let trailing_length = 32usize.checked_sub(skip).ok_or(Status::SpanOverflow)?;
        if field(block, trailing_offset, trailing_length)?
            .iter().any(|byte| *byte != 0)
        {
            return Err(Status::TrailingState);
        }
    }
    found.ok_or(Status::TargetAbsent)
}

/// Validate the FAT reserved entries and capture cluster two's one EOC.
pub fn parse_fat(
    block: &[u8],
    geometry: &Geometry,
) -> Result<FatState, Status> {
    exact_block(block)?;
    validate_geometry(geometry)?;
    let media_entry = read_u16(block, 0)?;
    let reserved_entry = read_u16(block, 2)?;
    if media_entry != 0xFFF8 || reserved_entry != 0xFFFF {
        return if block[0] != geometry.media as u8 {
            Err(Status::Media)
        } else {
            Err(Status::FatReserved)
        };
    }
    let maximum_cluster = geometry.cluster_count
        .checked_add(1)
        .ok_or(Status::SpanOverflow)?;
    let fat_offset = usize::from(FILE_CLUSTER)
        .checked_mul(2)
        .ok_or(Status::SpanOverflow)?;
    let fat_value = read_u16(block, fat_offset)?;
    if fat_value >= FAT16_EOC_MIN {
        // Exact one-cluster termination.
    } else if fat_value >= 2 && u64::from(fat_value) <= maximum_cluster {
        return Err(Status::MultiCluster);
    } else {
        return Err(Status::FatEntry);
    }
    Ok(FatState {
        media_entry,
        reserved_entry,
        file_cluster: FILE_CLUSTER,
        file_entry: fat_value,
    })
}

fn translate_cluster(geometry: &Geometry, cluster: u16) -> Result<u64, Status> {
    let cluster_index = u64::from(cluster)
        .checked_sub(2)
        .ok_or(Status::ClusterTranslation)?;
    let sector_delta = cluster_index
        .checked_mul(u64::from(geometry.sectors_per_cluster))
        .ok_or(Status::ClusterTranslation)?;
    let lba = geometry.first_data_sector
        .checked_add(sector_delta)
        .ok_or(Status::ClusterTranslation)?;
    let extent_end = lba
        .checked_add(u64::from(geometry.sectors_per_cluster))
        .ok_or(Status::ClusterTranslation)?;
    let data_end = geometry.first_data_sector
        .checked_add(geometry.data_sectors)
        .ok_or(Status::ClusterTranslation)?;
    if lba < geometry.first_data_sector || extent_end > data_end
        || extent_end > geometry.total_sectors
        || extent_end > geometry.namespace_blocks
        || lba != 4
    {
        return Err(Status::ClusterTranslation);
    }
    Ok(lba)
}

/// Join validated root and FAT values into one checked one-cluster extent.
pub fn validate_extent(
    geometry: &Geometry,
    entry: &RootEntry,
    fat: &FatState,
) -> Result<Extent, Status> {
    validate_geometry(geometry)?;
    if entry.canonical_name != PHIPIA_NAME || entry.attribute != ATTR_ARCHIVE {
        return Err(Status::UnsupportedEntry);
    }
    let maximum_cluster = geometry.cluster_count
        .checked_add(1)
        .ok_or(Status::SpanOverflow)?;
    if entry.first_cluster < 2
        || u64::from(entry.first_cluster) > maximum_cluster
        || entry.first_cluster != FILE_CLUSTER
        || fat.file_cluster != entry.first_cluster
    {
        return Err(Status::ClusterRange);
    }
    if fat.media_entry != 0xFFF8 || fat.reserved_entry != 0xFFFF {
        return Err(Status::FatReserved);
    }
    if fat.file_entry < FAT16_EOC_MIN {
        return Err(Status::FatEntry);
    }
    let lba = translate_cluster(geometry, entry.first_cluster)?;
    let cluster_bytes = geometry.bytes_per_sector
        .checked_mul(geometry.sectors_per_cluster)
        .ok_or(Status::SpanOverflow)?;
    if entry.file_size == 0 || entry.file_size != FILE_BYTES
        || entry.file_size > cluster_bytes
    {
        return Err(Status::FileSize);
    }
    Ok(Extent {
        cluster: entry.first_cluster,
        fat_value: fat.file_entry,
        lba,
        file_size: entry.file_size,
        cluster_bytes,
    })
}

const SHA256_INITIAL: [u32; 8] = [
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
    0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
];
const SHA256_K: [u32; 64] = [
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
];

#[inline(always)]
fn sha256_input_word(data: &[u8], block_index: usize, word_index: usize) -> u32 {
    if block_index < 2 {
        let offset = block_index * 64 + word_index * 4;
        u32::from_be_bytes([
            data[offset], data[offset + 1], data[offset + 2], data[offset + 3],
        ])
    } else if word_index == 0 {
        0x8000_0000
    } else if word_index == 15 {
        FILE_BYTES * 8
    } else {
        0
    }
}

fn sha256(data: &[u8]) -> Result<[u8; 32], Status> {
    if data.len() != FILE_BYTES as usize {
        return Err(Status::PayloadLength);
    }
    let mut state = SHA256_INITIAL;
    for block_index in 0..3 {
        /* A 16-word rolling schedule avoids any hosted bulk-memory operation. */
        let mut words = [
            sha256_input_word(data, block_index, 0),
            sha256_input_word(data, block_index, 1),
            sha256_input_word(data, block_index, 2),
            sha256_input_word(data, block_index, 3),
            sha256_input_word(data, block_index, 4),
            sha256_input_word(data, block_index, 5),
            sha256_input_word(data, block_index, 6),
            sha256_input_word(data, block_index, 7),
            sha256_input_word(data, block_index, 8),
            sha256_input_word(data, block_index, 9),
            sha256_input_word(data, block_index, 10),
            sha256_input_word(data, block_index, 11),
            sha256_input_word(data, block_index, 12),
            sha256_input_word(data, block_index, 13),
            sha256_input_word(data, block_index, 14),
            sha256_input_word(data, block_index, 15),
        ];
        let mut a = state[0];
        let mut b = state[1];
        let mut c = state[2];
        let mut d = state[3];
        let mut e = state[4];
        let mut f = state[5];
        let mut g = state[6];
        let mut h = state[7];
        for index in 0..64 {
            let schedule_index = index & 15;
            let scheduled = if index < 16 {
                words[schedule_index]
            } else {
                let word_minus_15 = words[(index + 1) & 15];
                let word_minus_2 = words[(index + 14) & 15];
                let s0 = word_minus_15.rotate_right(7)
                    ^ word_minus_15.rotate_right(18)
                    ^ (word_minus_15 >> 3);
                let s1 = word_minus_2.rotate_right(17)
                    ^ word_minus_2.rotate_right(19)
                    ^ (word_minus_2 >> 10);
                let next = words[schedule_index]
                    .wrapping_add(s0)
                    .wrapping_add(words[(index + 9) & 15])
                    .wrapping_add(s1);
                words[schedule_index] = next;
                next
            };
            let upper = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let choose = (e & f) ^ ((!e) & g);
            let first = h.wrapping_add(upper).wrapping_add(choose)
                .wrapping_add(SHA256_K[index]).wrapping_add(scheduled);
            let lower = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let majority = (a & b) ^ (a & c) ^ (b & c);
            let second = lower.wrapping_add(majority);
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(first);
            d = c;
            c = b;
            b = a;
            a = first.wrapping_add(second);
        }
        state[0] = state[0].wrapping_add(a);
        state[1] = state[1].wrapping_add(b);
        state[2] = state[2].wrapping_add(c);
        state[3] = state[3].wrapping_add(d);
        state[4] = state[4].wrapping_add(e);
        state[5] = state[5].wrapping_add(f);
        state[6] = state[6].wrapping_add(g);
        state[7] = state[7].wrapping_add(h);
    }
    let mut digest = [0u8; 32];
    for (index, word) in state.iter().copied().enumerate() {
        let bytes = word.to_be_bytes();
        let offset = index * 4;
        digest[offset] = bytes[0];
        digest[offset + 1] = bytes[1];
        digest[offset + 2] = bytes[2];
        digest[offset + 3] = bytes[3];
    }
    Ok(digest)
}

/// Validate all 128 deterministic bytes and their documented SHA-256.
pub fn validate_payload(data: &[u8]) -> Result<Payload, Status> {
    if data.len() != FILE_BYTES as usize {
        return Err(Status::PayloadLength);
    }
    for (index, byte) in data.iter().enumerate() {
        let expected = ((index * 73 + 19) & 0xFF) as u8;
        if *byte != expected {
            return Err(Status::PayloadContent);
        }
    }
    let digest = sha256(data)?;
    let difference =
        (digest[0] ^ PAYLOAD_SHA256[0]) |
        (digest[1] ^ PAYLOAD_SHA256[1]) |
        (digest[2] ^ PAYLOAD_SHA256[2]) |
        (digest[3] ^ PAYLOAD_SHA256[3]) |
        (digest[4] ^ PAYLOAD_SHA256[4]) |
        (digest[5] ^ PAYLOAD_SHA256[5]) |
        (digest[6] ^ PAYLOAD_SHA256[6]) |
        (digest[7] ^ PAYLOAD_SHA256[7]) |
        (digest[8] ^ PAYLOAD_SHA256[8]) |
        (digest[9] ^ PAYLOAD_SHA256[9]) |
        (digest[10] ^ PAYLOAD_SHA256[10]) |
        (digest[11] ^ PAYLOAD_SHA256[11]) |
        (digest[12] ^ PAYLOAD_SHA256[12]) |
        (digest[13] ^ PAYLOAD_SHA256[13]) |
        (digest[14] ^ PAYLOAD_SHA256[14]) |
        (digest[15] ^ PAYLOAD_SHA256[15]) |
        (digest[16] ^ PAYLOAD_SHA256[16]) |
        (digest[17] ^ PAYLOAD_SHA256[17]) |
        (digest[18] ^ PAYLOAD_SHA256[18]) |
        (digest[19] ^ PAYLOAD_SHA256[19]) |
        (digest[20] ^ PAYLOAD_SHA256[20]) |
        (digest[21] ^ PAYLOAD_SHA256[21]) |
        (digest[22] ^ PAYLOAD_SHA256[22]) |
        (digest[23] ^ PAYLOAD_SHA256[23]) |
        (digest[24] ^ PAYLOAD_SHA256[24]) |
        (digest[25] ^ PAYLOAD_SHA256[25]) |
        (digest[26] ^ PAYLOAD_SHA256[26]) |
        (digest[27] ^ PAYLOAD_SHA256[27]) |
        (digest[28] ^ PAYLOAD_SHA256[28]) |
        (digest[29] ^ PAYLOAD_SHA256[29]) |
        (digest[30] ^ PAYLOAD_SHA256[30]) |
        (digest[31] ^ PAYLOAD_SHA256[31]);
    if difference != 0 {
        return Err(Status::PayloadDigest);
    }
    Ok(Payload { sha256: digest, byte_count: FILE_BYTES, deterministic: 1 })
}

#[cfg(test)]
fn put_u16(block: &mut [u8], offset: usize, value: u16) {
    block[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

#[cfg(test)]
fn put_u32(block: &mut [u8], offset: usize, value: u32) {
    block[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

#[cfg(test)]
fn make_bpb(block: &mut [u8; BLOCK_BYTES]) {
    block.fill(0);
    block[0..3].copy_from_slice(&[0xEB, 0x3C, 0x90]);
    block[3..11].copy_from_slice(b"PHIPIA  ");
    put_u16(block, BPB_BYTES_PER_SECTOR, BLOCK_BYTES as u16);
    block[BPB_SECTORS_PER_CLUSTER] = 1;
    put_u16(block, BPB_RESERVED_SECTORS, 1);
    block[BPB_FAT_COUNT] = 1;
    put_u16(block, BPB_ROOT_ENTRIES, ROOT_ENTRIES as u16);
    put_u16(block, BPB_TOTAL_SECTORS_16, TOTAL_SECTORS as u16);
    block[BPB_MEDIA] = MEDIA_FIXED;
    put_u16(block, BPB_FAT_SECTORS_16, 2);
    block[36] = 0x80;
    block[EBPB_SIGNATURE] = 0x29;
    block[510..512].copy_from_slice(&[0x55, 0xAA]);
}

#[cfg(test)]
fn make_root(block: &mut [u8; BLOCK_BYTES]) {
    block.fill(0);
    block[..11].copy_from_slice(&PHIPIA_NAME);
    block[11] = ATTR_ARCHIVE;
    put_u16(block, 26, FILE_CLUSTER);
    put_u32(block, 28, FILE_BYTES);
}

#[cfg(test)]
fn make_fat(block: &mut [u8; BLOCK_BYTES]) {
    block.fill(0);
    put_u16(block, 0, 0xFFF8);
    put_u16(block, 2, 0xFFFF);
    put_u16(block, usize::from(FILE_CLUSTER) * 2, 0xFFFF);
}

#[cfg(test)]
fn status_is<T>(result: Result<T, Status>, status: Status) -> bool {
    matches!(result, Err(found) if found == status)
}

/// Exercise all accepted forms and controls 1 through 22 with synthetic bytes.
#[cfg(test)]
pub fn self_test() -> u32 {
    let mut block = [0u8; BLOCK_BYTES];
    make_bpb(&mut block);
    let geometry = match parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32) {
        Ok(value) => value,
        Err(_) => return 0,
    };
    if geometry.cluster_count != 4092 || geometry.first_data_sector != 4 {
        return 0;
    }

    // 1: required signatures.
    block[510] = 0;
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::BootSignature) {
        return 0;
    }
    make_bpb(&mut block);
    block[EBPB_SIGNATURE] = 0;
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::BootSignature) {
        return 0;
    }
    make_bpb(&mut block);
    block[EBPB_RESERVED] = 1;
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::BootSignature) {
        return 0;
    }
    make_bpb(&mut block);

    // 2: truncated metadata and namespace sector disagreement.
    if !status_is(parse_bpb(&block[..511], TOTAL_SECTORS, BLOCK_BYTES as u32), Status::Truncated)
        || !status_is(parse_bpb(&block, TOTAL_SECTORS, 512), Status::BytesPerSector)
    {
        return 0;
    }
    put_u16(&mut block, BPB_BYTES_PER_SECTOR, 512);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::BytesPerSector) {
        return 0;
    }
    make_bpb(&mut block);

    // 3-7: each fixed BPB geometry family.
    block[BPB_SECTORS_PER_CLUSTER] = 2;
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::SectorsPerCluster) { return 0; }
    make_bpb(&mut block);
    block[BPB_SECTORS_PER_CLUSTER] = 0;
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::SectorsPerCluster) { return 0; }
    make_bpb(&mut block);
    put_u16(&mut block, BPB_RESERVED_SECTORS, 0);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::ReservedCount) { return 0; }
    make_bpb(&mut block);
    put_u16(&mut block, BPB_RESERVED_SECTORS, 2);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::ReservedCount) { return 0; }
    make_bpb(&mut block);
    block[BPB_FAT_COUNT] = 0;
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::FatCount) { return 0; }
    make_bpb(&mut block);
    block[BPB_FAT_COUNT] = 2;
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::FatCount) { return 0; }
    make_bpb(&mut block);
    put_u16(&mut block, BPB_TOTAL_SECTORS_16, 0);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::TotalSectors) { return 0; }
    make_bpb(&mut block);
    put_u32(&mut block, BPB_TOTAL_SECTORS_32, TOTAL_SECTORS as u32);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::TotalSectors) { return 0; }
    make_bpb(&mut block);
    put_u16(&mut block, BPB_TOTAL_SECTORS_16, TOTAL_SECTORS as u16 - 1);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::TotalSectors) { return 0; }
    make_bpb(&mut block);
    put_u16(&mut block, BPB_FAT_SECTORS_16, 0);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::FatSize) { return 0; }
    make_bpb(&mut block);
    put_u16(&mut block, BPB_FAT_SECTORS_16, u16::MAX);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::SpanRange) { return 0; }
    make_bpb(&mut block);
    put_u16(&mut block, BPB_ROOT_ENTRIES, 0);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::RootGeometry) { return 0; }
    make_bpb(&mut block);
    put_u16(&mut block, BPB_ROOT_ENTRIES, 127);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::RootGeometry) { return 0; }
    make_bpb(&mut block);
    put_u16(&mut block, BPB_ROOT_ENTRIES, u16::MAX);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::RootGeometry) { return 0; }
    make_bpb(&mut block);

    // 8: volume span beyond the identified namespace.
    if !status_is(parse_bpb(&block, TOTAL_SECTORS - 1, BLOCK_BYTES as u32), Status::SpanRange) { return 0; }
    put_u32(&mut block, BPB_HIDDEN_SECTORS, 1);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::SpanRange) { return 0; }
    make_bpb(&mut block);

    // 9: normative FAT12 and FAT32 cluster-count thresholds.
    put_u16(&mut block, BPB_TOTAL_SECTORS_16, 4088);
    if !status_is(parse_bpb(&block, TOTAL_SECTORS, BLOCK_BYTES as u32), Status::FatClass) { return 0; }
    make_bpb(&mut block);
    put_u16(&mut block, BPB_TOTAL_SECTORS_16, 0);
    put_u32(&mut block, BPB_TOTAL_SECTORS_32, 70_000);
    if !status_is(parse_bpb(&block, 70_000, BLOCK_BYTES as u32), Status::FatClass) { return 0; }
    make_bpb(&mut block);

    // Establish canonical query, root entry, and extent before corruptions.
    let query = match make_query(&PHIPIA_NAME) { Ok(value) => value, Err(_) => return 0 };
    make_root(&mut block);
    let entry = match find_root(&block, &geometry, &query, FILE_BYTES) {
        Ok(value) => value,
        Err(_) => return 0,
    };
    make_fat(&mut block);
    let fat = match parse_fat(&block, &geometry) {
        Ok(value) => value,
        Err(_) => return 0,
    };
    put_u16(&mut block, 4, FAT16_EOC_MIN);
    if parse_fat(&block, &geometry).is_err() { return 0; }
    make_fat(&mut block);
    let extent = match validate_extent(&geometry, &entry, &fat) {
        Ok(value) => value,
        Err(_) => return 0,
    };
    if extent.lba != 4 || extent.fat_value < FAT16_EOC_MIN { return 0; }

    // 10: media and reserved FAT entries.
    block[0] = 0xF0;
    if !status_is(parse_fat(&block, &geometry), Status::Media) { return 0; }
    make_fat(&mut block);
    put_u16(&mut block, 2, 0);
    if !status_is(parse_fat(&block, &geometry), Status::FatReserved) { return 0; }

    // 11-16: bounded root scanning and exact 8.3 policy.
    make_root(&mut block);
    for index in 0..ROOT_ENTRIES as usize { block[index * 32] = b'A'; block[index * 32 + 11] = ATTR_ARCHIVE; }
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::RootEndMissing) { return 0; }
    block.fill(0);
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::TargetAbsent) { return 0; }
    make_root(&mut block);
    block[32..43].copy_from_slice(&PHIPIA_NAME);
    block[43] = ATTR_ARCHIVE;
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::TargetDuplicate) { return 0; }
    make_root(&mut block);
    block[11] = ATTR_LONG_NAME;
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::LongName) { return 0; }
    make_root(&mut block);
    block[0] = 0xE5;
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::Deleted) { return 0; }
    make_root(&mut block);
    block[11] = 0x10;
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::UnsupportedEntry) { return 0; }
    make_root(&mut block);
    block[11] = 0x08;
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::UnsupportedEntry) { return 0; }
    make_root(&mut block);
    block[11] = 0x40;
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::UnsupportedEntry) { return 0; }
    let malformed = *b"phipia  BIN";
    if !status_is(make_query(&malformed), Status::NameMalformed) { return 0; }
    make_root(&mut block);
    block[0] = b's';
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::NameMalformed) { return 0; }

    // 17: invalid first clusters.
    make_root(&mut block);
    put_u16(&mut block, 26, 1);
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::ClusterRange) { return 0; }
    make_root(&mut block);
    put_u16(&mut block, 26, 5000);
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::ClusterRange) { return 0; }

    // 18-19: free/bad/reserved/out-of-range and cyclic/multi-cluster FAT values.
    for value in [0, 1, FAT16_BAD, FAT16_RESERVED_MIN, 0xFFF6, 5000] {
        make_fat(&mut block);
        put_u16(&mut block, 4, value);
        if !status_is(parse_fat(&block, &geometry), Status::FatEntry) { return 0; }
    }
    make_fat(&mut block);
    put_u16(&mut block, 4, FILE_CLUSTER);
    if !status_is(parse_fat(&block, &geometry), Status::MultiCluster) { return 0; }

    // 20: zero, oversized, and destination-exceeding file lengths.
    make_root(&mut block);
    put_u32(&mut block, 28, 0);
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::FileSize) { return 0; }
    make_root(&mut block);
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES - 1), Status::FileSize) { return 0; }
    make_root(&mut block);
    put_u32(&mut block, 28, BLOCK_BYTES as u32 + 1);
    if !status_is(find_root(&block, &geometry, &query, u32::MAX), Status::FileSize) { return 0; }

    // 21: the translation helper refuses overflow before any LBA is returned.
    let mut corrupt_geometry = geometry;
    corrupt_geometry.first_data_sector = u64::MAX;
    make_fat(&mut block);
    if !status_is(
        translate_cluster(&corrupt_geometry, entry.first_cluster),
        Status::ClusterTranslation,
    ) || (!status_is(validate_extent(&corrupt_geometry, &entry, &fat), Status::SpanOverflow)
        && !status_is(validate_extent(&corrupt_geometry, &entry, &fat), Status::SpanRange)
    ) {
        return 0;
    }

    // 22: declared directory end cannot conceal trailing allocated state.
    make_root(&mut block);
    block[33] = b'X';
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::TrailingState) { return 0; }
    make_root(&mut block);
    block[64] = b'X';
    if !status_is(find_root(&block, &geometry, &query, FILE_BYTES), Status::TrailingState) { return 0; }

    let mut data = [0u8; FILE_BYTES as usize];
    for (index, byte) in data.iter_mut().enumerate() {
        *byte = ((index * 73 + 19) & 0xFF) as u8;
    }
    if validate_payload(&data).map(|value| value.sha256) != Ok(PAYLOAD_SHA256)
        || !status_is(validate_payload(&data[..FILE_BYTES as usize - 1]), Status::PayloadLength)
    {
        return 0;
    }
    data[64] ^= 1;
    if !status_is(validate_payload(&data), Status::PayloadContent) {
        return 0;
    }
    ROBUSTNESS_CONTROLS
}

#[cfg(test)]
mod tests {
    use super::{self_test, ROBUSTNESS_CONTROLS};

    #[test]
    fn all_bounded_controls_pass() {
        assert_eq!(self_test(), ROBUSTNESS_CONTROLS);
    }
}
