use crate::block_index::FsBlockIndex;
use crate::checksum::Checksum;
use crate::block_group::TruncatedChecksum;
use crate::error::CorruptKind;
use crate::features::{CompatibleFeatures, IncompatibleFeatures, ReadOnlyCompatibleFeatures};
use crate::{Ext4, Ext4Error};

use crate::util::usize_from_u32;
use alloc::vec;
use alloc::collections::{BTreeMap, BTreeSet};
use alloc::vec::Vec;
use core::ops::RangeBounds;

/// Allocation view for one read-only validation pass. Discard before mutations;
/// each encountered group is read once rather than once per directory entry.
pub struct InodeAllocationSnapshot<'a> {
    filesystem: &'a Ext4,
    groups: BTreeMap<u32, Vec<u8>>,
}

/// Checksummed block allocation view for one read-only validation pass.
/// It never initializes lazy bitmaps or survives filesystem mutation.
pub struct BlockAllocationSnapshot<'a> {
    filesystem: &'a Ext4,
    groups: BTreeMap<u32, Vec<u8>>,
    extent_ranges: BTreeMap<u64, u64>,
    validated_inodes: BTreeSet<crate::inode::InodeIndex>,
    xattr_references: BTreeMap<u64, (u32, u32, crate::inode::InodeIndex)>,
    invalid: bool,
    fixed_reserved: bool,
}

impl<'a> BlockAllocationSnapshot<'a> {
    pub(crate) fn new(filesystem: &'a Ext4) -> Self {
        Self { filesystem, groups: BTreeMap::new(), extent_ranges: BTreeMap::new(),
            validated_inodes: BTreeSet::new(), xattr_references: BTreeMap::new(),
            invalid: false, fixed_reserved: false }
    }

    /// Reserve primary/backup superblocks and descriptor tables, block/inode
    /// bitmaps and inode tables in the admitted non-flex, non-resize profile.
    /// This validates geometry only; it does not initialize lazy bitmaps.
    pub fn reserve_fixed_metadata(&mut self) -> Result<(), Ext4Error> {
        if self.invalid { return Err(Ext4Error::Readonly); }
        if self.fixed_reserved { return Ok(()); }
        let result = (|| {
            let fs = self.filesystem;
            let sb = fs.superblock();
            if sb.incompatible_features().intersects(IncompatibleFeatures::META_BLOCK_GROUPS | IncompatibleFeatures::FLEXIBLE_BLOCK_GROUPS)
                || sb.compatible_features().contains(CompatibleFeatures::RESIZE_INODE)
                || sb.compatible_features().bits() & 0x200 != 0 { return Err(Ext4Error::Readonly); }
            let descriptor_blocks = (u64::from(sb.num_block_groups()) * u64::from(sb.block_group_descriptor_size()))
                .div_ceil(sb.block_size().to_u64());
            let table_blocks = (u64::from(sb.inodes_per_block_group().get()) * u64::from(sb.inode_size()))
                .div_ceil(sb.block_size().to_u64());
            let power = |mut value: u32, base: u32| {
                while value > 1 && value % base == 0 { value /= base; }
                value == 1
            };
            let mut reserve = |start: u64, count: u64, group: u32| -> Result<(), Ext4Error> {
                if start.checked_add(count).is_none_or(|end| end > sb.blocks_count()) {
                    return Err(CorruptKind::BlockGroupDescriptor(group).into());
                }
                let count = u32::try_from(count).map_err(|_| Ext4Error::FileTooLarge)?;
                self.claim_extent_range(start, count, crate::inode::InodeIndex::new(2).unwrap())
            };
            for (group, descriptor) in fs.0.block_group_descriptors.iter().enumerate() {
                let group = u32::try_from(group).map_err(|_| Ext4Error::FileTooLarge)?;
                let group_start = u64::from(sb.first_data_block()) + u64::from(group) * u64::from(sb.blocks_per_group().get());
                let has_super = !sb.read_only_compatible_features().contains(ReadOnlyCompatibleFeatures::SPARSE_SUPERBLOCKS)
                    || group == 0 || group == 1 || power(group, 3) || power(group, 5) || power(group, 7);
                if has_super { reserve(group_start, 1 + descriptor_blocks, group)?; }
                reserve(descriptor.block_bitmap_block(), 1, group)?;
                reserve(descriptor.inode_bitmap_block(), 1, group)?;
                reserve(descriptor.inode_table_first_block(), table_blocks, group)?;
            }
            Ok(())
        })();
        if result.is_err() { self.invalid = true; } else { self.fixed_reserved = true; }
        result
    }

    /// Data and tree blocks have exactly one claim. Shared xattr blocks enter
    /// this map once, with their reference counts recorded separately.
    pub(crate) fn claim_extent_range(&mut self, start: u64, count: u32,
        inode: crate::inode::InodeIndex) -> Result<(), Ext4Error> {
        let end = start.checked_add(u64::from(count)).ok_or(CorruptKind::ExtentBlock(inode))?;
        if self.extent_ranges.len() >= 65_536 { return Err(Ext4Error::FileTooLarge); }
        if self.invalid || count == 0
            || self.extent_ranges.range(..end).next_back().is_some_and(|(_, previous_end)| *previous_end > start)
        {
            return Err(CorruptKind::ExtentBlock(inode).into());
        }
        self.extent_ranges.insert(start, end);
        Ok(())
    }

    /// Reserve the internal journal's data mapping in this ownership pass.
    /// Extent-based journals also validate and claim their tree nodes.
    #[maybe_async::maybe_async]
    pub async fn validate_internal_journal(&mut self) -> Result<(), Ext4Error> {
        let result = self.validate_internal_journal_inner().await;
        if result.is_err() { self.invalid = true; }
        result
    }

    #[maybe_async::maybe_async]
    async fn validate_internal_journal_inner(&mut self) -> Result<(), Ext4Error> {
        if self.invalid { return Err(Ext4Error::Readonly); }
        let Some(index) = self.filesystem.superblock().journal_inode() else { return Ok(()) };
        if self.validated_inodes.contains(&index) { return Ok(()); }
        if !self.filesystem.inode_is_allocated(index).await? {
            return Err(CorruptKind::ExtentBlock(index).into());
        }
        let inode = crate::inode::Inode::read(self.filesystem, index).await?;
        self.validate_inode_extents(&inode).await
    }

    /// Walk nonzero legacy pointers instead of expanding sparse logical holes.
    /// Depth is at most three; claiming before each read catches cycles and
    /// data/indirect aliases. The shared range budget bounds hostile trees.
    #[maybe_async::maybe_async]
    async fn validate_legacy_mapping(&mut self, inode: &crate::inode::Inode) -> Result<(), Ext4Error> {
        if inode.file_type().is_symlink() && inode.size_in_bytes() < 60 {
            return Ok(()); // i_block contains the fast symlink's literal bytes
        }
        if !inode.file_type().is_regular_file() && !inode.file_type().is_dir() && !inode.file_type().is_symlink() {
            return Err(Ext4Error::Readonly);
        }
        let mut pending = Vec::new();
        for index in 0..15usize {
            let block = u64::from(crate::util::read_u32le(&inode.inline_data(), index * 4));
            if block != 0 { pending.push((block, index.saturating_sub(11) as u8)); }
        }
        while let Some((block, depth)) = pending.pop() {
            self.claim_extent_range(block, 1, inode.index)?;
            if !self.range_is_allocated(block, 1).await? {
                return Err(CorruptKind::BlockMap(block as u32).into());
            }
            if depth != 0 {
                let bytes = self.filesystem.read_block(block).await?;
                for entry in bytes.chunks_exact(4) {
                    let child = u64::from(crate::util::read_u32le(entry, 0));
                    if child != 0 { pending.push((child, depth - 1)); }
                }
            }
        }
        Ok(())
    }

    #[maybe_async::maybe_async]
    async fn validate_xattr_reference(&mut self, inode: &crate::inode::Inode) -> Result<(), Ext4Error> {
        let Some((block, expected)) = inode.external_xattr_reference(self.filesystem).await? else { return Ok(()) };
        if let Some((previous_expected, seen, _)) = self.xattr_references.get_mut(&block) {
            if *previous_expected != expected || *seen >= expected {
                return Err(CorruptKind::Xattr(inode.index).into());
            }
            *seen += 1;
        } else {
            self.claim_extent_range(block, 1, inode.index)?;
            if !self.range_is_allocated(block, 1).await? {
                return Err(CorruptKind::Xattr(inode.index).into());
            }
            self.xattr_references.insert(block, (expected, 1, inode.index));
        }
        Ok(())
    }

    /// Finish a complete reachable/orphan ownership pass. A stored xattr
    /// count must equal its distinct inode references before any release.
    #[maybe_async::maybe_async]
    pub async fn finish(self, inodes: &mut InodeAllocationSnapshot<'_>) -> Result<(), Ext4Error> {
        if self.invalid { return Err(Ext4Error::Readonly); }
        inodes.validate_known_allocations(&self.validated_inodes).await?;
        for (_, (expected, seen, inode)) in self.xattr_references {
            if expected != seen { return Err(CorruptKind::Xattr(inode).into()); }
        }
        Ok(())
    }

    /// Require every block in a nonempty physical range to be allocated.
    /// Allocation alone does not establish which inode owns the range.
    #[maybe_async::maybe_async]
    pub async fn range_is_allocated(&mut self, start: u64, count: u32) -> Result<bool, Ext4Error> {
        let fs = self.filesystem;
        let sb = fs.superblock();
        let Some(end) = start.checked_add(u64::from(count)) else { return Ok(false) };
        if count == 0 || start < u64::from(sb.first_data_block()) || end > sb.blocks_count() {
            return Ok(false);
        }
        let mut block = start;
        while block < end {
            let (group, offset) = fs.block_block_group_location(block)?;
            if !self.groups.contains_key(&group) {
                let descriptor = fs.0.block_group_descriptors.get(group as usize)
                    .ok_or(CorruptKind::BlockGroupDescriptor(group))?;
                let bitmap = BitmapHandle::new(descriptor.block_bitmap_block(), false);
                self.groups.insert(group, bitmap.read_validated(fs, group).await?);
            }
            let length = (end - block).min(u64::from(sb.blocks_per_group().get() - offset));
            let bytes = self.groups.get(&group).ok_or(CorruptKind::BlockGroupDescriptor(group))?;
            for bit in u64::from(offset)..u64::from(offset) + length {
                let byte = bytes.get((bit / 8) as usize).ok_or(CorruptKind::BlockGroupDescriptor(group))?;
                if byte & (1 << (bit % 8)) == 0 { return Ok(false); }
            }
            block += length;
        }
        Ok(true)
    }

    /// Validate allocation and exclusive mapping-node/data ownership among the
    /// inodes in this pass. Discard this snapshot after any error or mutation.
    /// Shared external xattrs have checked allocation and reference counts;
    /// call finish after visiting all inodes. Reserve fixed metadata before
    /// validating the namespace or journal.
    #[maybe_async::maybe_async]
    pub async fn validate_inode_extents(&mut self, inode: &crate::inode::Inode) -> Result<(), Ext4Error> {
        if self.invalid { return Err(CorruptKind::ExtentBlock(inode.index).into()); }
        if self.validated_inodes.contains(&inode.index) { return Ok(()); }
        if inode.flags().contains(crate::inode::InodeFlags::INLINE_DATA) {
            self.invalid = true;
            return Err(Ext4Error::Readonly);
        }
        if inode.flags().contains(crate::inode::InodeFlags::EXTENTS) {
            let result = match crate::iters::extents::Extents::new(self.filesystem.clone(), inode) {
                Ok(extents) => extents.validate_allocation(self).await,
                Err(error) => Err(error),
            };
            if let Err(error) = result {
                self.invalid = true;
                return Err(error);
            }
        } else if let Err(error) = self.validate_legacy_mapping(inode).await {
            self.invalid = true;
            return Err(error);
        }
        if let Err(error) = self.validate_xattr_reference(inode).await {
            self.invalid = true;
            return Err(error);
        }
        self.validated_inodes.insert(inode.index);
        Ok(())
    }
}

impl<'a> InodeAllocationSnapshot<'a> {
    pub(crate) fn new(filesystem: &'a Ext4) -> Self {
        Self { filesystem, groups: BTreeMap::new() }
    }

    /// Check a bounded inode number against its validated allocation bitmap.
    #[maybe_async::maybe_async]
    pub async fn is_allocated(&mut self, index: crate::inode::InodeIndex) -> Result<bool, Ext4Error> {
        let fs = self.filesystem;
        if index.get() > fs.0.superblock.inodes_count() { return Ok(false); }
        let (group, offset) = crate::inode::get_inode_block_group_location(&fs.0.superblock, index)?;
        if !self.groups.contains_key(&group) {
            let descriptor = fs.0.block_group_descriptors.get(group as usize)
                .ok_or(CorruptKind::BlockGroupDescriptor(group))?;
            let bitmap = BitmapHandle::new(descriptor.inode_bitmap_block(), true);
            let bytes = bitmap.read_validated(fs, group).await?;
            self.groups.insert(group, bytes);
        }
        let byte = self.groups.get(&group).and_then(|bytes| bytes.get((offset / 8) as usize))
            .ok_or(CorruptKind::BlockGroupDescriptor(group))?;
        Ok(byte & (1 << (offset % 8)) != 0)
    }

    /// Every allocated non-reserved inode must have participated in the
    /// namespace/orphan ownership pass. Otherwise an unreachable inode could
    /// hide a conflicting block claim or an unaccounted xattr reference.
    #[maybe_async::maybe_async]
    async fn validate_known_allocations(&mut self, known: &BTreeSet<crate::inode::InodeIndex>) -> Result<(), Ext4Error> {
        let fs = self.filesystem;
        let sb = fs.superblock();
        let per_group = sb.inodes_per_block_group().get();
        for group in 0..sb.num_block_groups() {
            let descriptor = fs.0.block_group_descriptors.get(group as usize)
                .ok_or(CorruptKind::BlockGroupDescriptor(group))?;
            if descriptor.flags() & 1 != 0 {
                // A lazy inode group is logically empty; do not initialize it
                // or interpret stale bitmap bytes as real allocations.
                if descriptor.free_inodes_count() != per_group || descriptor.used_dirs_count() != 0
                    || descriptor.unused_inodes_count() != per_group {
                    return Err(CorruptKind::BlockGroupDescriptor(group).into());
                }
                continue;
            }
            let cached = self.groups.contains_key(&group);
            if !cached {
                let bitmap = BitmapHandle::new(descriptor.inode_bitmap_block(), true);
                self.groups.insert(group, bitmap.read_validated(fs, group).await?);
            }
            let bytes = self.groups.get(&group).ok_or(CorruptKind::BlockGroupDescriptor(group))?;
            for offset in 0..per_group {
                let number = u64::from(group) * u64::from(per_group) + u64::from(offset) + 1;
                if number < u64::from(sb.first_allocatable_inode()) || number > u64::from(sb.inodes_count()) { continue; }
                let byte = bytes.get((offset / 8) as usize).ok_or(CorruptKind::BlockGroupDescriptor(group))?;
                if byte & (1 << (offset % 8)) != 0 {
                    let index = crate::inode::InodeIndex::new(number as u32).unwrap();
                    if !known.contains(&index) { return Err(CorruptKind::OrphanInode(index.get()).into()); }
                }
            }
            // Keep only bitmaps already needed by the namespace walk. The
            // complete census otherwise needs one extra group buffer at a time.
            if !cached { self.groups.remove(&group); }
        }
        Ok(())
    }
}

fn calc_index(byte_index: u32, bit_index: u32) -> u32 {
    byte_index
        .checked_mul(8)
        .unwrap()
        .checked_add(bit_index)
        .unwrap()
}

pub(crate) struct BitmapHandle {
    block: FsBlockIndex,
    is_inode_bitmap: bool,
}

#[expect(unused)]
impl BitmapHandle {
    /// Materialize the admitted non-flex, non-resize group's lazy bitmap in
    /// the configured writer. Phipia's writer stages this with the allocation.
    #[maybe_async::maybe_async]
    pub(crate) async fn initialize(&self, ext4: &Ext4, group: u32) -> Result<(), Ext4Error> {
        let descriptor = ext4.get_block_group_descriptor(group);
        let flag = if self.is_inode_bitmap { 1 } else { 2 };
        if descriptor.flags() & flag == 0 { return Ok(()); }
        let sb = &ext4.0.superblock;
        if ext4.0.writer.is_none() || group == 0 || sb.block_size().to_u32() != 4096
            || sb.incompatible_features().intersects(IncompatibleFeatures::META_BLOCK_GROUPS | IncompatibleFeatures::FLEXIBLE_BLOCK_GROUPS)
            || sb.compatible_features().contains(CompatibleFeatures::RESIZE_INODE)
            || sb.compatible_features().bits() & 0x200 != 0 {
            return Err(Ext4Error::Readonly);
        }
        let bad = || Ext4Error::from(CorruptKind::BlockGroupDescriptor(group));
        let mut bytes = vec![0xff; sb.block_size().to_usize()];
        let bits = if self.is_inode_bitmap {
            sb.inodes_per_block_group().get()
        } else {
            let start = u64::from(group) * u64::from(sb.blocks_per_group().get()) + u64::from(sb.first_data_block());
            u32::try_from(sb.blocks_count().checked_sub(start).ok_or_else(bad)?
                .min(u64::from(sb.blocks_per_group().get()))).map_err(|_| bad())?
        };
        if bits == 0 || u64::from(bits) > bytes.len() as u64 * 8 { return Err(bad()); }
        for bit in 0..bits { bytes[(bit / 8) as usize] &= !(1 << (bit % 8)); }
        if self.is_inode_bitmap {
            if descriptor.free_inodes_count() != bits || descriptor.used_dirs_count() != 0
                || descriptor.unused_inodes_count() != bits { return Err(bad()); }
        } else {
            let start = u64::from(group) * u64::from(sb.blocks_per_group().get()) + u64::from(sb.first_data_block());
            let mut mark = |block: u64| -> Result<(), Ext4Error> {
                let bit = block.checked_sub(start).filter(|bit| *bit < u64::from(bits)).ok_or_else(bad)?;
                bytes[(bit / 8) as usize] |= 1 << (bit % 8);
                Ok(())
            };
            let power = |mut value: u32, base: u32| {
                while value > 1 && value % base == 0 { value /= base; }
                value == 1
            };
            let has_super = !sb.read_only_compatible_features().contains(ReadOnlyCompatibleFeatures::SPARSE_SUPERBLOCKS)
                || group == 1 || power(group, 3) || power(group, 5) || power(group, 7);
            if has_super {
                let descriptors = (u64::from(sb.num_block_groups()) * u64::from(sb.block_group_descriptor_size()))
                    .div_ceil(sb.block_size().to_u64());
                for block in 0..=descriptors { mark(start + block)?; }
            }
            mark(descriptor.block_bitmap_block())?;
            mark(descriptor.inode_bitmap_block())?;
            let table_blocks = (u64::from(sb.inodes_per_block_group().get()) * u64::from(sb.inode_size()))
                .div_ceil(sb.block_size().to_u64());
            for index in 0..table_blocks { mark(descriptor.inode_table_first_block().checked_add(index).ok_or_else(bad)?)?; }
            let free = (0..bits).filter(|bit| bytes[(*bit / 8) as usize] & (1 << (*bit % 8)) == 0).count();
            if free as u64 != u64::from(descriptor.free_blocks_count()) { return Err(bad()); }
        }
        ext4.write_to_block(self.block, 0, &bytes).await?;
        let checksum = self.calc_checksum(ext4, group).await?;
        if self.is_inode_bitmap { descriptor.set_inode_bitmap_checksum(checksum); }
        else { descriptor.set_block_bitmap_checksum(checksum); }
        descriptor.set_flags(descriptor.flags() & !flag);
        descriptor.write(ext4).await
    }

    #[maybe_async::maybe_async]
    pub(crate) async fn validate(&self, ext4: &Ext4, group: u32) -> Result<(), Ext4Error> {
        self.read_validated(ext4, group).await.map(|_| ())
    }

    #[maybe_async::maybe_async]
    async fn read_validated(&self, ext4: &Ext4, group: u32) -> Result<Vec<u8>, Ext4Error> {
        let descriptor = ext4.get_block_group_descriptor(group);
        // Lazy bitmap initialization is a separate mutation: never interpret
        // uninitialized bytes as allocation state or repair their checksum.
        let uninitialized = if self.is_inode_bitmap { 1 } else { 2 };
        if descriptor.flags() & uninitialized != 0 { return Err(Ext4Error::Readonly); }
        let mut bytes = vec![0; ext4.0.superblock.block_size().to_usize()];
        ext4.read_from_block(self.block, 0, &mut bytes).await?;
        if !ext4.0.superblock.read_only_compatible_features()
            .contains(ReadOnlyCompatibleFeatures::METADATA_CHECKSUMS) { return Ok(bytes); }
        let actual = self.checksum_bytes(ext4, group, &bytes)?;
        let expected = if self.is_inode_bitmap { descriptor.inode_bitmap_checksum() }
            else { descriptor.block_bitmap_checksum() };
        let matches = match expected {
            TruncatedChecksum::Full(value) => actual == value,
            TruncatedChecksum::Truncated(value) => actual & 0xffff == u32::from(value),
        };
        if !matches { return Err(CorruptKind::BlockGroupDescriptorChecksum(group).into()); }
        Ok(bytes)
    }

    pub(crate) fn new(block: FsBlockIndex, is_inode_bitmap: bool) -> Self {
        Self {
            block,
            is_inode_bitmap,
        }
    }

    /// Query the bitmap for the value of bit `n`.
    #[maybe_async::maybe_async]
    pub(crate) async fn query(
        &self,
        n: u32,
        ext4: &Ext4,
    ) -> Result<bool, Ext4Error> {
        let mut dst = [0; 1];
        let byte_index = n / 8;
        let bit_index = n % 8;
        ext4.read_from_block(self.block, byte_index, &mut dst)
            .await?;
        // Get the value of the bit at `bit_index` in `dst[0]`.
        Ok((dst[0] & (1 << bit_index)) != 0)
    }

    /// Set the value of bit `n` in the bitmap to `value`.
    #[maybe_async::maybe_async]
    pub(crate) async fn set(
        &self,
        n: u32,
        value: bool,
        ext4: &Ext4,
    ) -> Result<(), Ext4Error> {
        let mut dst = [0; 1];
        let byte_index = n / 8;
        let bit_index = n % 8;
        ext4.read_from_block(self.block, byte_index, &mut dst)
            .await?;
        if value {
            dst[0] |= 1 << bit_index;
        } else {
            dst[0] &= !(1 << bit_index);
        }
        ext4.write_to_block(self.block, byte_index, &dst).await?;
        Ok(())
    }

    /// Find the first bit in the bitmap with value `value`, and return its index.
    /// Returns `Ok(None)` if no such bit is found.
    #[maybe_async::maybe_async]
    pub(crate) async fn find_first(
        &self,
        value: bool,
        range: impl RangeBounds<u32>,
        ext4: &Ext4,
    ) -> Result<Option<u32>, Ext4Error> {
        let mut bytes = vec![0; ext4.0.superblock.block_size().to_usize()];
        ext4.read_from_block(self.block, 0, &mut bytes).await?;
        for (byte_index, byte) in bytes.into_iter().enumerate() {
            let byte_index = u32::try_from(byte_index).unwrap();
            if value {
                // Look for a bit with value 1.
                if byte != 0 {
                    for bit_index in 0..8 {
                        if (byte & (1 << bit_index)) != 0 {
                            let index = calc_index(byte_index, bit_index);
                            if !range.contains(&(index)) {
                                continue;
                            }
                            return Ok(Some(index));
                        }
                    }
                }
            } else {
                // Look for a bit with value 0.
                if byte != 0xFF {
                    for bit_index in 0..8 {
                        if (byte & (1 << bit_index)) == 0 {
                            let index = calc_index(byte_index, bit_index);
                            if !range.contains(&(index)) {
                                continue;
                            }
                            return Ok(Some(index));
                        }
                    }
                }
            }
        }
        Ok(None)
    }

    /// Find the first `n` bits in the bitmap with value `value`, and return the initial index.
    /// Returns `Ok(None)` if no such sequence of bits is found.
    #[maybe_async::maybe_async]
    pub(crate) async fn find_first_n(
        &self,
        n: u32,
        value: bool,
        range: impl RangeBounds<u32>,
        ext4: &Ext4,
    ) -> Result<Option<u32>, Ext4Error> {
        let mut bytes = vec![0; ext4.0.superblock.block_size().to_usize()];
        ext4.read_from_block(self.block, 0, &mut bytes).await?;
        let mut count: u32 = 0;
        for (byte_index, byte) in bytes.into_iter().enumerate() {
            let byte_index = u32::try_from(byte_index).unwrap();
            for bit_index in 0..8 {
                if ((byte & (1 << bit_index)) != 0) == value {
                    let index = calc_index(byte_index, bit_index);

                    if !range.contains(&(index)) {
                        count = 0;
                        continue;
                    }
                    count = count.checked_add(1).unwrap();
                    if count == n {
                        return Ok(Some(
                            index
                                .checked_add(1)
                                .unwrap()
                                .checked_sub(n)
                                .unwrap(),
                        ));
                    }
                } else {
                    count = 0;
                }
            }
        }
        Ok(None)
    }

    #[maybe_async::maybe_async]
    pub(crate) async fn calc_checksum(
        &self,
        ext4: &Ext4,
        block_group_index: u32,
    ) -> Result<u32, Ext4Error> {
        let mut dst = vec![0; ext4.0.superblock.block_size().to_usize()];
        ext4.read_from_block(self.block, 0, &mut dst).await?;
        self.checksum_bytes(ext4, block_group_index, &dst)
    }

    fn checksum_bytes(&self, ext4: &Ext4, block_group_index: u32, dst: &[u8]) -> Result<u32, Ext4Error> {
        let mut checksum =
            Checksum::with_seed(ext4.0.superblock.checksum_seed());

        let bytes_to_hash = if self.is_inode_bitmap {
            let inodes_per_group =
                ext4.0.superblock.inodes_per_block_group().get();
            (usize_from_u32(inodes_per_group).checked_add(7).unwrap()) / 8
        } else {
            // Linux ext4_block_bitmap_csum_set() hashes exactly
            // EXT4_CLUSTERS_PER_GROUP / 8 bytes. Phipia rejects bigalloc,
            // so one cluster is one block and the remaining bitmap-block
            // padding must not contribute to this checksum.
            usize_from_u32(ext4.0.superblock.blocks_per_group().get()) / 8
        };

        checksum.update(dst.get(..bytes_to_hash).ok_or(CorruptKind::BlockGroupDescriptor(block_group_index))?);
        Ok(checksum.finalize())
    }
}

#[cfg(test)]
mod tests {
    #[cfg(feature = "std")]
    #[maybe_async::test(
        feature = "sync",
        async(not(feature = "sync"), tokio::test)
    )]
    async fn test_bitmap_handle() {
        let fs = crate::test_util::load_test_disk1().await;

        let bitmap = fs.get_block_bitmap_handle(0);
        let first = bitmap.find_first(false, .., &fs).await.unwrap();
        // Ensure false
        let query = bitmap.query(first.unwrap(), &fs).await.unwrap();
        assert!(!query);
        let first = bitmap.find_first(true, .., &fs).await.unwrap();
        // Ensure true
        let query = bitmap.query(first.unwrap(), &fs).await;
        assert!(query.unwrap());
    }
}
