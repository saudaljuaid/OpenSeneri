//! Legacy ext4 orphan-chain mutations. The caller must journal the complete
//! operation and roll back its staged filesystem view on any error.

use crate::error::CorruptKind;
use crate::file_blocks::FileBlocks;
use crate::inode::{Inode, InodeFlags, InodeIndex};
#[cfg(not(feature = "sync"))]
use crate::iters::AsyncIterator;
use crate::iters::read_dir::ReadDir;
use crate::iters::extents::Extents;
use crate::path::PathBuf;
use crate::util::read_u16le;
use crate::{Ext4, Ext4Error};
use alloc::vec::Vec;

const MAX_ORPHANS: usize = 128;
const MAX_TRUNCATE_ORPHAN_BLOCKS: u32 = 16_384;

impl Ext4 {
    #[maybe_async::maybe_async]
    async fn validate_orphan_kind(&self, inode: &Inode) -> Result<(), Ext4Error> {
        if inode.flags().intersects(InodeFlags::IMMUTABLE | InodeFlags::APPEND_ONLY) {
            return Err(Ext4Error::Readonly);
        }
        if inode.file_type().is_regular_file() {
            if inode.links_count() != 0 {
                self.validate_extent_reclaim(inode.index, MAX_TRUNCATE_ORPHAN_BLOCKS).await?;
            }
            return Ok(());
        }
        if inode.links_count() != 0 || !inode.file_type().is_dir() { return Err(Ext4Error::Readonly); }
        let block_size = self.superblock().block_size().to_u64();
        let mut directory_inode = inode.clone();
        if inode.size_in_bytes() == 0 {
            // Linux ext4_rmdir clears i_size before orphan cleanup frees the
            // extents. Reconstruct only a bounded contiguous logical prefix
            // for validating the remaining directory blocks; never persist it.
            if !inode.flags().contains(InodeFlags::EXTENTS)
                || read_u16le(&inode.inline_data(), 6) != 0 {
                // Multi-level zero-size directory trees remain outside this
                // recovery profile; do not traverse an unbounded empty tree.
                return Err(Ext4Error::Readonly);
            }
            let _blocks = FileBlocks::from_inode(inode, self.clone())?;
            let mut extents = Extents::new(self.clone(), inode)?;
            let mut end = 0u64;
            while let Some(extent) = extents.next().await {
                let extent = extent?;
                if !extent.is_initialized || u64::from(extent.block_within_file) != end
                    || extent.num_blocks == 0 {
                    return Err(CorruptKind::OrphanInode(inode.index.get()).into());
                }
                end = end.checked_add(u64::from(extent.num_blocks))
                    .filter(|end| *end <= 8192)
                    .ok_or(CorruptKind::OrphanInode(inode.index.get()))?;
            }
            if end == 0 {
                // The final data-free transaction can precede inode release.
                // Admit only an empty inline extent root with no residual
                // allocation accounting or external xattr block.
                if inode.blocks() != 0 || inode.file_acl() != 0 {
                    return Err(CorruptKind::OrphanInode(inode.index.get()).into());
                }
                return Ok(());
            }
            directory_inode.set_size_in_bytes(end * block_size);
        }
        if directory_inode.size_in_bytes() % block_size != 0
            || directory_inode.size_in_bytes() / block_size > 8192 {
            return Err(CorruptKind::OrphanInode(inode.index.get()).into());
        }
        let mut entries = ReadDir::new(self.clone(), &directory_inode, PathBuf::empty())?;
        let (mut dot, mut dotdot) = (false, false);
        while let Some(entry) = entries.next().await {
            let entry = entry?;
            if entry.file_name() == b"." && !dot && entry.inode == inode.index { dot = true; }
            else if entry.file_name() == b".." && !dotdot
                && entry.inode != inode.index
                && (entry.inode.get() == 2 || entry.inode.get() >= 11)
                && entry.inode.get() <= self.superblock().inodes_count() { dotdot = true; }
            else { return Err(CorruptKind::OrphanInode(inode.index.get()).into()); }
        }
        if !dot || !dotdot { return Err(CorruptKind::OrphanInode(inode.index.get()).into()); }
        Ok(())
    }

    #[maybe_async::maybe_async]
    async fn validate_orphan_allocation(&self, index: InodeIndex) -> Result<(), Ext4Error> {
        if index.get() < 11 || index.get() > self.0.superblock.inodes_count() {
            return Err(CorruptKind::OrphanInode(index.get()).into());
        }
        if !self.inode_is_allocated(index).await? {
            return Err(CorruptKind::OrphanInode(index.get()).into());
        }
        Ok(())
    }

    #[maybe_async::maybe_async]
    async fn orphan_chain(&self) -> Result<Vec<Inode>, Ext4Error> {
        let mut chain: Vec<Inode> = Vec::new();
        let mut next = self.0.superblock.last_orphan();
        while next != 0 {
            if next < 11 || next > self.0.superblock.inodes_count()
                || chain.len() == MAX_ORPHANS
                || chain.iter().any(|inode| inode.index.get() == next) {
                return Err(CorruptKind::OrphanInode(next).into());
            }
            let index = InodeIndex::new(next).ok_or(CorruptKind::OrphanInode(next))?;
            self.validate_orphan_allocation(index).await?;
            let inode = Inode::read(self, index).await?;
            self.validate_orphan_kind(&inode).await?;
            next = inode.dtime_val();
            chain.push(inode);
        }
        Ok(chain)
    }

    /// Validate zero-link regular files/empty directories and bounded linked
    /// extent inodes whose interrupted truncate must preserve their namespace.
    #[maybe_async::maybe_async]
    pub async fn orphan_inodes(&self) -> Result<Vec<InodeIndex>, Ext4Error> {
        Ok(self.orphan_chain().await?.into_iter().map(|inode| inode.index).collect())
    }

    #[maybe_async::maybe_async]
    pub(crate) async fn defer_unlinked_inode(&self, inode: &mut Inode) -> Result<(), Ext4Error> {
        if self.0.writer.is_none() { return Err(Ext4Error::Readonly); }
        self.validate_orphan_allocation(inode.index).await?;
        let chain = self.orphan_chain().await?;
        if chain.len() == MAX_ORPHANS || chain.iter().any(|item| item.index == inode.index) {
            return Err(CorruptKind::OrphanInode(inode.index.get()).into());
        }
        self.validate_orphan_kind(inode).await?;
        inode.set_dtime_val(self.0.superblock.last_orphan());
        inode.write(self).await?;
        self.0.superblock.set_last_orphan(inode.index.get());
        self.0.superblock.write(self).await
    }

    /// Check the logical allocation bound before publishing a split unlink.
    /// This does not mutate the inode or claim complete block ownership proof.
    #[maybe_async::maybe_async]
    pub async fn validate_extent_reclaim(&self, index: InodeIndex,
        max_logical_blocks: u32) -> Result<(), Ext4Error> {
        self.validate_orphan_allocation(index).await?;
        let inode = Inode::read(self, index).await?;
        if !inode.file_type().is_regular_file() { return Err(Ext4Error::Readonly); }
        if inode.size_in_bytes().div_ceil(self.superblock().block_size().to_u64())
            > u64::from(max_logical_blocks) { return Err(Ext4Error::FileTooLarge); }
        let FileBlocks::ExtentTree(tree) = FileBlocks::from_inode(&inode, self.clone())?
            else { return Err(Ext4Error::Readonly); };
        tree.reclaim_extent_end(max_logical_blocks).await?;
        let size = inode.size_in_bytes();
        if size % self.superblock().block_size().to_u64() != 0 {
            let file = crate::file::File::open_inode(self, inode)?;
            if let Some(block) = file.filesystem_block_at_offset(size).await? {
                if self.is_fixed_metadata_block(block)? {
                    return Err(CorruptKind::OrphanInode(index.get()).into());
                }
            }
        }
        Ok(())
    }

    /// Reclaim a bounded extent suffix without releasing the orphan inode.
    /// The caller must journal or roll back the entire staged operation.
    #[maybe_async::maybe_async]
    pub async fn trim_orphan_suffix(&self, index: InodeIndex, max_blocks: u32,
        max_logical_blocks: u32) -> Result<(), Ext4Error> {
        if self.0.writer.is_none() { return Err(Ext4Error::Readonly); }
        let mut inode = self.orphan_chain().await?.into_iter()
            .find(|inode| inode.index == index).ok_or(Ext4Error::NotFound)?;
        if !inode.file_type().is_regular_file() || inode.links_count() != 0 { return Err(Ext4Error::Readonly); }
        let FileBlocks::ExtentTree(mut tree) = FileBlocks::from_inode(&inode, self.clone())?
            else { return Err(Ext4Error::Readonly); };
        tree.trim_orphan_suffix(&mut inode, max_blocks, max_logical_blocks, 0).await
    }

    /// Continue a linked orphan's truncate without freeing its inode or xattrs.
    /// False means a bounded suffix was staged; true means cleanup is complete.
    #[maybe_async::maybe_async]
    pub async fn complete_truncate_orphan(&self, index: InodeIndex,
        max_blocks: u32) -> Result<bool, Ext4Error> {
        if self.0.writer.is_none() { return Err(Ext4Error::Readonly); }
        let mut chain = self.orphan_chain().await?;
        let position = chain.iter().position(|inode| inode.index == index).ok_or(Ext4Error::NotFound)?;
        let inode = &mut chain[position];
        if inode.links_count() == 0 || !inode.file_type().is_regular_file() { return Err(Ext4Error::Readonly); }
        let FileBlocks::ExtentTree(mut tree) = FileBlocks::from_inode(inode, self.clone())?
            else { return Err(Ext4Error::Readonly); };
        let block_size = self.superblock().block_size().to_u64();
        let keep = u32::try_from(inode.size_in_bytes().div_ceil(block_size))
            .map_err(|_| Ext4Error::FileTooLarge)?;
        if tree.reclaim_extent_end(MAX_TRUNCATE_ORPHAN_BLOCKS).await? > keep {
            tree.trim_orphan_suffix(inode, max_blocks, MAX_TRUNCATE_ORPHAN_BLOCKS, keep).await?;
            return Ok(false);
        }
        // Finish the partial block even if Linux had already published EOF.
        // This image and orphan removal share a journal transaction.
        let size = inode.size_in_bytes();
        let within = size % block_size;
        if within != 0 {
            let file = crate::file::File::open_inode(self, inode.clone())?;
            if let Some(block) = file.filesystem_block_at_offset(size).await? {
                if self.is_fixed_metadata_block(block)? {
                    return Err(CorruptKind::OrphanInode(index.get()).into());
                }
                self.write_to_block(block, within as u32,
                    &alloc::vec![0; (block_size - within) as usize]).await?;
            }
        }
        let mut inode = self.detach_orphan(&mut chain, position).await?;
        inode.set_dtime_val(0);
        inode.write_preserving_times(self).await?;
        Ok(true)
    }

    #[maybe_async::maybe_async]
    async fn detach_orphan(&self, chain: &mut Vec<Inode>, position: usize) -> Result<Inode, Ext4Error> {
        let inode = chain.remove(position);
        if position == 0 {
            self.0.superblock.set_last_orphan(inode.dtime_val());
            self.0.superblock.write(self).await?;
        } else {
            let previous = &mut chain[position - 1];
            previous.set_dtime_val(inode.dtime_val());
            previous.write_preserving_times(self).await?;
        }
        Ok(inode)
    }

    /// Remove an orphan from any list position and free its inode/data through
    /// the configured writer, including every data/metadata block revocation.
    #[maybe_async::maybe_async]
    pub async fn release_orphan(&self, index: InodeIndex) -> Result<(), Ext4Error> {
        if self.0.writer.is_none() { return Err(Ext4Error::Readonly); }
        let mut chain = self.orphan_chain().await?;
        let position = chain.iter().position(|inode| inode.index == index).ok_or(Ext4Error::NotFound)?;
        if chain[position].links_count() != 0 { return Err(Ext4Error::Readonly); }
        let inode = self.detach_orphan(&mut chain, position).await?;
        self.delete_file(inode).await
    }
}
