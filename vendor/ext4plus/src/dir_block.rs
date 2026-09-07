// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use crate::Ext4;
use crate::block_index::FsBlockIndex;
use crate::checksum::Checksum;
use crate::error::{CorruptKind, Ext4Error};
use crate::inode::InodeIndex;
use crate::util::{read_u16le, read_u32le, write_u32le};

#[derive(Debug, Eq, PartialEq)]
enum DirBlockType {
    /// Root node of an htree.
    Root,

    /// Non-root internal node of an htree.
    Internal,

    /// Leaf node of an htree, also the format used for all blocks in a
    /// directory without an htree.
    Leaf,
}

/// Struct for reading and validating a directory block.
#[derive(Clone)]
pub(crate) struct DirBlock<'a> {
    pub(crate) fs: &'a Ext4,

    /// Absolute index of the block within the filesystem.
    pub(crate) block_index: FsBlockIndex,

    /// Whether this is the first block of the file.
    pub(crate) is_first: bool,

    /// Directory inode index.
    pub(crate) dir_inode: InodeIndex,

    /// Whether the directory has an htree.
    pub(crate) has_htree: bool,

    /// Checksum base copied from the dir inode.
    pub(crate) checksum_base: Checksum,
}

impl DirBlock<'_> {
    /// Read the directory block's contents into `block`.
    ///
    /// If checksums are enabled for the filesystem, the directory
    /// block's checksum will be verified.
    #[maybe_async::maybe_async]
    pub(crate) async fn read(&self, block: &mut [u8]) -> Result<(), Ext4Error> {
        let block_size = self.fs.0.superblock.block_size();
        assert_eq!(block.len(), block_size);

        self.fs.read_from_block(self.block_index, 0, block).await?;

        if !self.fs.has_metadata_checksums() {
            return if self.get_block_type(block) == DirBlockType::Leaf {
                self.validate_leaf_records(block)
            } else { Ok(()) };
        }

        let block_type = self.get_block_type(block);
        let is_leaf = block_type == DirBlockType::Leaf;

        let (actual_checksum, offset) = if block_type == DirBlockType::Leaf {
            (self.calc_leaf_checksum(block)?, block.len() - 4)
        } else {
            self.calc_internal_checksum(block, block_type)?
        };
        let expected_checksum = read_u32le(block, offset);

        if actual_checksum.finalize() == expected_checksum {
            if is_leaf { self.validate_leaf_records(block) } else { Ok(()) }
        } else {
            Err(CorruptKind::DirBlockChecksum(self.dir_inode).into())
        }
    }

    /// Update the directory block checksum in-place.
    ///
    /// This is needed after mutating directory entries in an existing block
    /// (e.g. add/remove in a non-htree directory).
    pub(crate) fn update_checksum(
        &self,
        block: &mut [u8],
    ) -> Result<(), Ext4Error> {
        let block_size = self.fs.0.superblock.block_size();
        assert_eq!(block.len(), block_size);

        if self.get_block_type(block) == DirBlockType::Leaf {
            self.validate_leaf_records(block)?;
        }

        if !self.fs.has_metadata_checksums() {
            return Ok(());
        }

        let block_type = self.get_block_type(block);

        let (checksum, offset) = if block_type == DirBlockType::Leaf {
            (self.calc_leaf_checksum(block)?, block.len() - 4)
        } else {
            self.calc_internal_checksum(block, block_type)?
        };

        write_u32le(block, offset, checksum.finalize());

        Ok(())
    }

    /// Records must end exactly before the checksum tail. A valid CRC alone
    /// does not prevent a final live/unused record from consuming that tail.
    fn validate_leaf_records(&self, block: &[u8]) -> Result<(), Ext4Error> {
        let end = block.len() - if self.fs.has_metadata_checksums() { 12 } else { 0 };
        let mut offset = 0;
        while offset < end {
            if end - offset < 8 { return Err(CorruptKind::DirEntry(self.dir_inode).into()); }
            let length = usize::from(read_u16le(block, offset + 4));
            if length < 8 || length % 4 != 0 || length > end - offset {
                return Err(CorruptKind::DirEntry(self.dir_inode).into());
            }
            if read_u32le(block, offset) != 0 {
                let name_length = usize::from(block[offset + 6]);
                if name_length == 0 || 8 + name_length > length {
                    return Err(CorruptKind::DirEntry(self.dir_inode).into());
                }
            }
            offset += length;
        }
        Ok(())
    }

    /// Calculate the checksum of a leaf block.
    fn calc_leaf_checksum(&self, block: &[u8]) -> Result<Checksum, Ext4Error> {
        let tail_entry_size = 12;

        // OK to unwrap: minimum block size is 1024.
        let tail_entry_offset =
            block.len().checked_sub(tail_entry_size).unwrap();
        if read_u32le(block, tail_entry_offset) != 0
            || read_u16le(block, tail_entry_offset + 4) != 12
            || block[tail_entry_offset + 6] != 0 || block[tail_entry_offset + 7] != 0xde {
            return Err(CorruptKind::DirBlockChecksum(self.dir_inode).into());
        }

        let mut checksum = self.checksum_base.clone();
        checksum.update(&block[..tail_entry_offset]);

        Ok(checksum)
    }

    /// Calculate the checksum of a non-leaf block.
    fn calc_internal_checksum(
        &self,
        block: &[u8],
        block_type: DirBlockType,
    ) -> Result<(Checksum, usize), Ext4Error> {
        let limit_offset: usize = if block_type == DirBlockType::Root {
            if read_u16le(block, 4) != 12 || usize::from(read_u16le(block, 16)) != block.len() - 12
                || read_u32le(block, 24) != 0 || block[29] != 8 {
                return Err(CorruptKind::DirBlockChecksum(self.dir_inode).into());
            }
            0x20
        } else {
            0x8
        };

        // OK to unwrap: `limit_offset` is at most 0x20.
        let count_offset = limit_offset.checked_add(2).unwrap();

        let count = usize::from(read_u16le(block, count_offset));
        let limit = usize::from(read_u16le(block, limit_offset));
        let tail_entry_offset = limit_offset + limit * 8;
        if count == 0 || count > limit || tail_entry_offset > block.len() - 8 {
            return Err(CorruptKind::DirBlockChecksum(self.dir_inode).into());
        }

        // Count is bounded by the validated limit and tail, so the checksum
        // slice cannot escape the directory block on hostile input.
        let num_bytes = limit_offset + count * 8;

        let mut checksum = self.checksum_base.clone();
        checksum.update(&block[..num_bytes]);
        checksum.update_u32_le(read_u32le(block, tail_entry_offset));
        checksum.update_u32_le(0);

        Ok((checksum, tail_entry_offset + 4))
    }

    fn get_block_type(&self, block: &[u8]) -> DirBlockType {
        // Non-htree directories use the same format as leaf nodes in an htree.
        if !self.has_htree {
            return DirBlockType::Leaf;
        }

        // The first block of an htree is the root node.
        if self.is_first {
            return DirBlockType::Root;
        }

        // Other internal nodes are identified by the first record
        // having a length equal to the whole block.
        let first_rec_len = read_u16le(block, 4);
        if first_rec_len == self.fs.0.superblock.block_size() {
            DirBlockType::Internal
        } else {
            DirBlockType::Leaf
        }
    }
}
