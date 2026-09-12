// SPDX-License-Identifier: GPL-3.0-only
//! Checked parsing primitives for Phipia's bounded FAT32 subset.

const BOOT_BYTES: usize = 512;
const ENTRY_BYTES: usize = 32;
const FAT32_MIN_CLUSTERS: u64 = 65_525;
const FAT32_MAX_CLUSTER: u32 = 0x0fff_ffef;
const FAT32_BAD: u32 = 0x0fff_fff7;
const FAT32_EOC_MIN: u32 = 0x0fff_fff8;
const FAT32_MASK: u32 = 0x0fff_ffff;
const MAX_COMPONENT_BYTES: usize = 12;

#[repr(i32)]
#[derive(Clone, Copy, Eq, PartialEq)]
#[cfg_attr(test, derive(Debug))]
#[cfg_attr(test, allow(dead_code))]
pub(crate) enum Status {
    Ok = 0,
    NullArgument,
    Truncated,
    BootSignature,
    BytesPerSector,
    SectorsPerCluster,
    ReservedCount,
    FatCount,
    LegacyGeometry,
    TotalSectors,
    FatSize,
    SpanOverflow,
    SpanRange,
    FatClass,
    FatCapacity,
    RootCluster,
    FsInfoSector,
    BackupSector,
    FsInfoSignature,
    FsInfoHint,
    FatMismatch,
    FatReserved,
    ClusterRange,
    ClusterFree,
    ClusterReserved,
    ClusterBad,
    DirectoryEntry,
    LongNameUnsupported,
    LongNameMalformed,
    LongNameEncoding,
    PathEmpty,
    PathAbsolute,
    PathTooLong,
    PathMalformed,
    ComponentTooLong,
    NameMalformed,
    AboveRoot,
    #[allow(dead_code)]
    Count,
}

#[repr(C)]
#[derive(Clone, Copy, Eq, PartialEq)]
#[cfg_attr(test, derive(Debug))]
pub(crate) struct Geometry {
    pub(crate) bytes_per_sector: u32,
    pub(crate) sectors_per_cluster: u32,
    pub(crate) reserved_sectors: u32,
    pub(crate) fat_copies: u32,
    pub(crate) total_sectors: u64,
    pub(crate) fat_sectors: u64,
    pub(crate) first_fat_sector: u64,
    pub(crate) first_data_sector: u64,
    pub(crate) cluster_count: u64,
    pub(crate) maximum_cluster: u32,
    pub(crate) root_cluster: u32,
    pub(crate) fsinfo_sector: u32,
    pub(crate) backup_boot_sector: u32,
    pub(crate) volume_id: u32,
    pub(crate) volume_label: [u8; 11],
    pub(crate) valid: u32,
}

impl Geometry {
    #[cfg_attr(test, allow(dead_code))]
    pub(crate) const fn invalid() -> Self {
        Self {
            bytes_per_sector: 0,
            sectors_per_cluster: 0,
            reserved_sectors: 0,
            fat_copies: 0,
            total_sectors: 0,
            fat_sectors: 0,
            first_fat_sector: 0,
            first_data_sector: 0,
            cluster_count: 0,
            maximum_cluster: 0,
            root_cluster: 0,
            fsinfo_sector: 0,
            backup_boot_sector: 0,
            volume_id: 0,
            volume_label: [0; 11],
            valid: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Eq, PartialEq)]
#[cfg_attr(test, derive(Debug))]
pub(crate) struct FsInfo {
    pub(crate) free_hint: u32,
    pub(crate) next_hint: u32,
    pub(crate) free_hint_valid: u32,
    pub(crate) next_hint_valid: u32,
}

impl FsInfo {
    #[cfg_attr(test, allow(dead_code))]
    pub(crate) const fn invalid() -> Self {
        Self {
            free_hint: u32::MAX,
            next_hint: u32::MAX,
            free_hint_valid: 0,
            next_hint_valid: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Eq, PartialEq)]
#[cfg_attr(test, derive(Debug))]
pub(crate) struct DirectoryEntry {
    pub(crate) short_name: [u8; 11],
    pub(crate) attributes: u8,
    pub(crate) kind: u8,
    pub(crate) reserved: u16,
    pub(crate) first_cluster: u32,
    pub(crate) size: u32,
    pub(crate) valid: u32,
}

impl DirectoryEntry {
    pub(crate) const fn invalid() -> Self {
        Self {
            short_name: [0; 11],
            attributes: 0,
            kind: 0,
            reserved: 0,
            first_cluster: 0,
            size: 0,
            valid: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Eq, PartialEq)]
#[cfg_attr(test, derive(Debug))]
pub(crate) struct Name {
    pub(crate) canonical: [u8; 11],
    pub(crate) kind: u8,
    pub(crate) reserved: [u8; 4],
}

impl Name {
    pub(crate) const fn invalid() -> Self {
        Self {
            canonical: [0; 11],
            kind: 0,
            reserved: [0; 4],
        }
    }
}

fn field(bytes: &[u8], offset: usize, length: usize) -> Result<&[u8], Status> {
    let end = offset.checked_add(length).ok_or(Status::SpanOverflow)?;
    bytes.get(offset..end).ok_or(Status::Truncated)
}

fn byte(bytes: &[u8], offset: usize) -> Result<u8, Status> {
    bytes.get(offset).copied().ok_or(Status::Truncated)
}

fn le16(bytes: &[u8], offset: usize) -> Result<u16, Status> {
    let value = field(bytes, offset, 2)?;
    Ok(u16::from_le_bytes([byte(value, 0)?, byte(value, 1)?]))
}

fn le32(bytes: &[u8], offset: usize) -> Result<u32, Status> {
    let value = field(bytes, offset, 4)?;
    Ok(u32::from_le_bytes([
        byte(value, 0)?,
        byte(value, 1)?,
        byte(value, 2)?,
        byte(value, 3)?,
    ]))
}

fn add(left: u64, right: u64) -> Result<u64, Status> {
    left.checked_add(right).ok_or(Status::SpanOverflow)
}

fn multiply(left: u64, right: u64) -> Result<u64, Status> {
    left.checked_mul(right).ok_or(Status::SpanOverflow)
}

#[inline(never)]
fn bytes_differ(left: u8, right: u8) -> bool {
    left != right
}

fn bytewise_equal(left: &[u8], right: &[u8]) -> bool {
    if left.len() != right.len() {
        return false;
    }
    for (left_byte, right_byte) in left.iter().zip(right.iter()) {
        // The non-inlined predicate keeps this bounded walk explicit;
        // otherwise LLVM lowers slice equality to a GOT-backed memcmp.
        if bytes_differ(*left_byte, *right_byte) {
            return false;
        }
    }
    true
}

fn power_of_two(value: u32) -> bool {
    value != 0 && value & (value - 1) == 0
}

pub(crate) fn parse_bpb(
    bytes: &[u8],
    namespace_blocks: u64,
    namespace_block_bytes: u32,
) -> Result<Geometry, Status> {
    if bytes.len() < BOOT_BYTES {
        return Err(Status::Truncated);
    }
    if byte(bytes, 510)? != 0x55 || byte(bytes, 511)? != 0xaa {
        return Err(Status::BootSignature);
    }
    let bytes_per_sector = u32::from(le16(bytes, 11)?);
    if !matches!(bytes_per_sector, 512 | 1024 | 2048 | 4096)
        || bytes_per_sector != namespace_block_bytes
    {
        return Err(Status::BytesPerSector);
    }
    let sectors_per_cluster = u32::from(byte(bytes, 13)?);
    if !power_of_two(sectors_per_cluster) || sectors_per_cluster > 128 {
        return Err(Status::SectorsPerCluster);
    }
    let reserved_sectors = u32::from(le16(bytes, 14)?);
    if reserved_sectors < 2 {
        return Err(Status::ReservedCount);
    }
    let fat_copies = u32::from(byte(bytes, 16)?);
    if fat_copies != 2 {
        return Err(Status::FatCount);
    }
    if le16(bytes, 17)? != 0 || le16(bytes, 19)? != 0 || le16(bytes, 22)? != 0 {
        return Err(Status::LegacyGeometry);
    }
    let total_sectors = u64::from(le32(bytes, 32)?);
    if total_sectors == 0 || total_sectors != namespace_blocks {
        return Err(Status::TotalSectors);
    }
    let fat_sectors = u64::from(le32(bytes, 36)?);
    if fat_sectors == 0 {
        return Err(Status::FatSize);
    }
    let fat_span = multiply(u64::from(fat_copies), fat_sectors)?;
    let first_data_sector = add(u64::from(reserved_sectors), fat_span)?;
    if first_data_sector >= total_sectors {
        return Err(Status::SpanRange);
    }
    let data_sectors = total_sectors - first_data_sector;
    let cluster_count = data_sectors / u64::from(sectors_per_cluster);
    if cluster_count < FAT32_MIN_CLUSTERS
        || cluster_count > u64::from(FAT32_MAX_CLUSTER - 1)
    {
        return Err(Status::FatClass);
    }
    let maximum_cluster = u32::try_from(add(cluster_count, 1)?)
        .map_err(|_| Status::SpanOverflow)?;
    let fat_bytes = multiply(fat_sectors, u64::from(bytes_per_sector))?;
    if fat_bytes / 4 <= u64::from(maximum_cluster) {
        return Err(Status::FatCapacity);
    }
    let root_cluster = le32(bytes, 44)?;
    if root_cluster < 2 || root_cluster > maximum_cluster {
        return Err(Status::RootCluster);
    }
    let fsinfo_sector = u32::from(le16(bytes, 48)?);
    if fsinfo_sector == 0 || fsinfo_sector >= reserved_sectors {
        return Err(Status::FsInfoSector);
    }
    let backup_boot_sector = u32::from(le16(bytes, 50)?);
    if backup_boot_sector == 0
        || backup_boot_sector >= reserved_sectors
        || backup_boot_sector == fsinfo_sector
    {
        return Err(Status::BackupSector);
    }
    let mut volume_label = [0_u8; 11];
    let label = field(bytes, 71, volume_label.len())?;
    for (destination, source) in volume_label.iter_mut().zip(label.iter().copied()) {
        *destination = source;
    }
    if volume_label.iter().any(|byte| !(0x20..=0x7e).contains(byte)) {
        return Err(Status::NameMalformed);
    }
    let image_bytes = multiply(total_sectors, u64::from(bytes_per_sector))?;
    let namespace_bytes = multiply(namespace_blocks, u64::from(namespace_block_bytes))?;
    if image_bytes != namespace_bytes {
        return Err(Status::SpanRange);
    }
    Ok(Geometry {
        bytes_per_sector,
        sectors_per_cluster,
        reserved_sectors,
        fat_copies,
        total_sectors,
        fat_sectors,
        first_fat_sector: u64::from(reserved_sectors),
        first_data_sector,
        cluster_count,
        maximum_cluster,
        root_cluster,
        fsinfo_sector,
        backup_boot_sector,
        volume_id: le32(bytes, 67)?,
        volume_label,
        valid: 1,
    })
}

pub(crate) fn parse_fsinfo(bytes: &[u8], geometry: &Geometry) -> Result<FsInfo, Status> {
    if geometry.valid == 0 || bytes.len() < usize::try_from(geometry.bytes_per_sector).unwrap_or(0) {
        return Err(Status::Truncated);
    }
    if le32(bytes, 0)? != 0x4161_5252
        || le32(bytes, 484)? != 0x6141_7272
        || le32(bytes, 508)? != 0xaa55_0000
    {
        return Err(Status::FsInfoSignature);
    }
    let free_hint = le32(bytes, 488)?;
    let next_hint = le32(bytes, 492)?;
    let free_valid = free_hint == u32::MAX || u64::from(free_hint) <= geometry.cluster_count;
    let next_valid = next_hint == u32::MAX
        || (next_hint >= 2 && next_hint <= geometry.maximum_cluster);
    if !free_valid || !next_valid {
        return Err(Status::FsInfoHint);
    }
    Ok(FsInfo {
        free_hint,
        next_hint,
        free_hint_valid: u32::from(free_hint != u32::MAX),
        next_hint_valid: u32::from(next_hint != u32::MAX),
    })
}

pub(crate) fn validate_fat_pair(
    first: &[u8],
    second: &[u8],
    geometry: &Geometry,
) -> Result<(), Status> {
    if geometry.valid == 0
        || first.len() != usize::try_from(geometry.bytes_per_sector).unwrap_or(0)
        || first.len() != second.len()
    {
        return Err(Status::Truncated);
    }
    if !bytewise_equal(first, second) {
        return Err(Status::FatMismatch);
    }
    let media = le32(first, 0)? & FAT32_MASK;
    let reserved = le32(first, 4)? & FAT32_MASK;
    if media != 0x0fff_fff8 || reserved < FAT32_EOC_MIN {
        return Err(Status::FatReserved);
    }
    Ok(())
}

pub(crate) fn classify_cluster(value: u32, geometry: &Geometry) -> Result<u32, Status> {
    if geometry.valid == 0 {
        return Err(Status::ClusterRange);
    }
    let cluster = value & FAT32_MASK;
    if cluster == 0 {
        return Err(Status::ClusterFree);
    }
    if cluster == FAT32_BAD {
        return Err(Status::ClusterBad);
    }
    if (0x0fff_fff0..=0x0fff_fff6).contains(&cluster) {
        return Err(Status::ClusterReserved);
    }
    if cluster >= FAT32_EOC_MIN {
        return Ok(cluster);
    }
    if cluster < 2 || cluster > geometry.maximum_cluster {
        return Err(Status::ClusterRange);
    }
    Ok(cluster)
}

fn canonical_character(byte: u8) -> bool {
    byte.is_ascii_uppercase()
        || byte.is_ascii_digit()
        || matches!(byte, b'$' | b'%' | b'\'' | b'-' | b'_' | b'@' | b'~' | b'`' | b'!' | b'(' | b')' | b'{' | b'}' | b'^' | b'#' | b'&')
}

fn uppercase(byte: u8) -> u8 {
    if byte.is_ascii_lowercase() {
        byte - b'a' + b'A'
    } else {
        byte
    }
}

pub(crate) fn parse_component(bytes: &[u8]) -> Result<Name, Status> {
    if bytes.is_empty() {
        return Err(Status::PathEmpty);
    }
    if bytes.len() > MAX_COMPONENT_BYTES {
        return Err(Status::ComponentTooLong);
    }
    if bytes == b"." {
        let mut result = Name::invalid();
        result.kind = 1;
        return Ok(result);
    }
    if bytes == b".." {
        let mut result = Name::invalid();
        result.kind = 2;
        return Ok(result);
    }
    let mut result = Name::invalid();
    result.canonical.fill(b' ');
    let mut dot = None;
    for (index, byte) in bytes.iter().copied().enumerate() {
        if byte == b'.' {
            if dot.is_some() || index == 0 || index + 1 == bytes.len() {
                return Err(Status::NameMalformed);
            }
            dot = Some(index);
        } else if byte < 0x20 || byte > 0x7e || !canonical_character(uppercase(byte)) {
            return Err(Status::NameMalformed);
        }
    }
    let base_end = dot.unwrap_or(bytes.len());
    let extension_start = dot.map_or(bytes.len(), |index| index + 1);
    if base_end == 0 || base_end > 8 || bytes.len() - extension_start > 3 {
        return Err(Status::NameMalformed);
    }
    for index in 0..base_end {
        let source = byte(bytes, index)?;
        let destination = result.canonical.get_mut(index).ok_or(Status::SpanOverflow)?;
        *destination = uppercase(source);
    }
    for index in 0..bytes.len() - extension_start {
        let source_index = extension_start
            .checked_add(index)
            .ok_or(Status::SpanOverflow)?;
        let destination_index = 8_usize.checked_add(index).ok_or(Status::SpanOverflow)?;
        let source = byte(bytes, source_index)?;
        let destination = result
            .canonical
            .get_mut(destination_index)
            .ok_or(Status::SpanOverflow)?;
        *destination = uppercase(source);
    }
    result.kind = 3;
    Ok(result)
}

pub(crate) fn validate_path(bytes: &[u8]) -> Result<u32, Status> {
    if bytes.is_empty() {
        return Err(Status::PathEmpty);
    }
    let first = bytes.first().copied().ok_or(Status::PathEmpty)?;
    if first == b'/' || first == b'\\' {
        return Err(Status::PathAbsolute);
    }
    if bytes.len() > 127 {
        return Err(Status::PathTooLong);
    }
    if bytes.last() == Some(&b'/') || bytes.iter().any(|byte| *byte == 0 || *byte == b'\\') {
        return Err(Status::PathMalformed);
    }
    let mut depth = 0_u32;
    let mut components = 0_u32;
    for component in bytes.split(|byte| *byte == b'/') {
        if component.is_empty() {
            return Err(Status::PathMalformed);
        }
        let parsed = parse_component(component)?;
        if parsed.kind == 2 {
            if depth == 0 {
                return Err(Status::AboveRoot);
            }
            depth -= 1;
        } else if parsed.kind == 3 {
            depth = depth.checked_add(1).ok_or(Status::PathTooLong)?;
        }
        components = components.checked_add(1).ok_or(Status::PathTooLong)?;
    }
    Ok(components)
}

/// Parse one entry in Phipia's bounded directory subset.
///
/// The root contains no dot entries. Subdirectory `.` and `..` entries must
/// name explicit clusters at or above 2; the optional FAT encoding of a root
/// parent as cluster zero is outside this deterministic subset.
pub(crate) fn parse_directory_entry(bytes: &[u8]) -> Result<DirectoryEntry, Status> {
    if bytes.len() < ENTRY_BYTES {
        return Err(Status::Truncated);
    }
    let first = byte(bytes, 0)?;
    if first == 0 {
        let mut result = DirectoryEntry::invalid();
        result.kind = 1;
        return Ok(result);
    }
    if first == 0xe5 {
        let mut result = DirectoryEntry::invalid();
        result.kind = 2;
        return Ok(result);
    }
    let attributes = byte(bytes, 11)?;
    if attributes == 0x0f {
        let ordinal = first & 0x1f;
        if ordinal == 0 || ordinal > 20 || byte(bytes, 12)? != 0 || le16(bytes, 26)? != 0 {
            return Err(Status::LongNameMalformed);
        }
        for offset in [1_usize, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30] {
            let codepoint = le16(bytes, offset)?;
            if codepoint != 0 && codepoint != 0xffff && !(0x20..=0x7e).contains(&codepoint) {
                return Err(Status::LongNameEncoding);
            }
        }
        return Err(Status::LongNameUnsupported);
    }
    if attributes & 0xc0 != 0 || first == 0x20 {
        return Err(Status::DirectoryEntry);
    }
    let mut short_name = [0_u8; 11];
    let encoded_name = field(bytes, 0, short_name.len())?;
    for (destination, source) in short_name.iter_mut().zip(encoded_name.iter().copied()) {
        *destination = source;
    }
    if short_name != *b".          " && short_name != *b"..         " {
        let mut base_space = false;
        let mut extension_space = false;
        for (index, byte) in short_name.iter().copied().enumerate() {
            if byte == b' ' {
                if index < 8 {
                    base_space = true;
                } else {
                    extension_space = true;
                }
            } else if !canonical_character(byte) {
                return Err(Status::NameMalformed);
            } else if (index < 8 && base_space) || (index >= 8 && extension_space) {
                return Err(Status::NameMalformed);
            }
        }
    }
    let first_cluster = u32::from(le16(bytes, 20)?) << 16 | u32::from(le16(bytes, 26)?);
    let size = le32(bytes, 28)?;
    if attributes & 0x10 != 0 && (first_cluster < 2 || size != 0) {
        return Err(Status::DirectoryEntry);
    }
    if attributes & 0x18 == 0 && ((size == 0 && first_cluster != 0) || (size != 0 && first_cluster < 2)) {
        return Err(Status::DirectoryEntry);
    }
    Ok(DirectoryEntry {
        short_name,
        attributes,
        kind: 3,
        reserved: 0,
        first_cluster,
        size,
        valid: 1,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const TOTAL: u32 = 131_072;
    const FAT_SECTORS: u32 = 1009;

    fn put16(bytes: &mut [u8], offset: usize, value: u16) {
        bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
    }

    fn put32(bytes: &mut [u8], offset: usize, value: u32) {
        bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }

    fn boot() -> [u8; 512] {
        let mut bytes = [0_u8; 512];
        bytes[0..3].copy_from_slice(b"\xeb\x58\x90");
        bytes[3..11].copy_from_slice(b"PHIPIA22");
        put16(&mut bytes, 11, 512);
        bytes[13] = 1;
        put16(&mut bytes, 14, 32);
        bytes[16] = 2;
        put32(&mut bytes, 32, TOTAL);
        put32(&mut bytes, 36, FAT_SECTORS);
        put32(&mut bytes, 44, 2);
        put16(&mut bytes, 48, 1);
        put16(&mut bytes, 50, 6);
        put32(&mut bytes, 67, 0x2000_0002);
        bytes[71..82].copy_from_slice(b"PHIPIADATA ");
        bytes[510..512].copy_from_slice(b"\x55\xaa");
        bytes
    }

    #[test]
    fn parses_checked_geometry_and_rejects_each_boundary() {
        let original = boot();
        let geometry = parse_bpb(&original, u64::from(TOTAL), 512).unwrap();
        assert_eq!(geometry.cluster_count, 129_022);
        assert_eq!(geometry.maximum_cluster, 129_023);

        let mut changed = original;
        changed[510] = 0;
        assert_eq!(parse_bpb(&changed, u64::from(TOTAL), 512), Err(Status::BootSignature));
        let mut changed = original;
        put16(&mut changed, 11, 768);
        assert_eq!(parse_bpb(&changed, u64::from(TOTAL), 512), Err(Status::BytesPerSector));
        let mut changed = original;
        changed[13] = 3;
        assert_eq!(parse_bpb(&changed, u64::from(TOTAL), 512), Err(Status::SectorsPerCluster));
        let mut changed = original;
        put32(&mut changed, 44, 1);
        assert_eq!(parse_bpb(&changed, u64::from(TOTAL), 512), Err(Status::RootCluster));
        assert_eq!(parse_bpb(&original, u64::from(TOTAL - 1), 512), Err(Status::TotalSectors));
    }

    #[test]
    fn fsinfo_is_a_checked_hint() {
        let geometry = parse_bpb(&boot(), u64::from(TOTAL), 512).unwrap();
        let mut info = [0_u8; 512];
        put32(&mut info, 0, 0x4161_5252);
        put32(&mut info, 484, 0x6141_7272);
        put32(&mut info, 488, 129_021);
        put32(&mut info, 492, 3);
        put32(&mut info, 508, 0xaa55_0000);
        let parsed = parse_fsinfo(&info, &geometry).unwrap();
        assert_eq!(parsed.free_hint, 129_021);
        put32(&mut info, 488, u32::MAX);
        assert_eq!(parse_fsinfo(&info, &geometry).unwrap().free_hint_valid, 0);
        put32(&mut info, 488, 129_024);
        assert_eq!(parse_fsinfo(&info, &geometry), Err(Status::FsInfoHint));
    }

    #[test]
    fn fat_copies_and_cluster_classes_are_bounded() {
        let geometry = parse_bpb(&boot(), u64::from(TOTAL), 512).unwrap();
        let mut first = [0_u8; 512];
        put32(&mut first, 0, 0x0fff_fff8);
        put32(&mut first, 4, FAT32_EOC_MIN);
        let mut second = first;
        assert_eq!(validate_fat_pair(&first, &second, &geometry), Ok(()));
        second[8] = 1;
        assert_eq!(validate_fat_pair(&first, &second, &geometry), Err(Status::FatMismatch));
        assert_eq!(classify_cluster(0, &geometry), Err(Status::ClusterFree));
        assert_eq!(classify_cluster(FAT32_BAD, &geometry), Err(Status::ClusterBad));
        assert_eq!(classify_cluster(0x0fff_fff2, &geometry), Err(Status::ClusterReserved));
        assert_eq!(classify_cluster(geometry.maximum_cluster + 1, &geometry), Err(Status::ClusterRange));
        assert_eq!(classify_cluster(FAT32_EOC_MIN, &geometry), Ok(FAT32_EOC_MIN));
    }

    #[test]
    fn paths_are_relative_bounded_and_canonical() {
        assert_eq!(parse_component(b"notes.txt").unwrap().canonical, *b"NOTES   TXT");
        assert_eq!(validate_path(b"projects/notes.txt"), Ok(2));
        assert_eq!(validate_path(b"../escape"), Err(Status::AboveRoot));
        assert_eq!(validate_path(b"/absolute"), Err(Status::PathAbsolute));
        assert_eq!(validate_path(b"a//b"), Err(Status::PathMalformed));
        assert_eq!(validate_path(b"bad name"), Err(Status::NameMalformed));
        assert_eq!(validate_path(&[b'a'; 128]), Err(Status::PathTooLong));
    }

    #[test]
    fn directory_entries_reject_long_names_and_inconsistent_sizes() {
        let mut entry = [0_u8; 32];
        entry[..11].copy_from_slice(b"NOTES   TXT");
        entry[11] = 0x20;
        put16(&mut entry, 26, 3);
        put32(&mut entry, 28, 9);
        assert_eq!(parse_directory_entry(&entry).unwrap().first_cluster, 3);
        entry[11] = 0x0f;
        entry[0] = 0x40;
        assert_eq!(parse_directory_entry(&entry), Err(Status::LongNameMalformed));
        entry[0] = 0x41;
        entry[12] = 0;
        put16(&mut entry, 26, 0);
        put16(&mut entry, 1, 0xd800);
        assert_eq!(parse_directory_entry(&entry), Err(Status::LongNameEncoding));
    }
}
