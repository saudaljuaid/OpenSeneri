//! Legacy ext4 orphan-chain mutations. The caller must journal the complete
//! operation and roll back its staged filesystem view on any error.

use crate::error::CorruptKind;
use crate::inode::{Inode, InodeFlags, InodeIndex};
use crate::{Ext4, Ext4Error};
use alloc::vec::Vec;

const MAX_ORPHANS: usize = 128;

impl Ext4 {
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
            // Linked Linux truncation orphans need a separate cleanup path.
            if inode.links_count() != 0 || !inode.file_type().is_regular_file()
                || inode.flags().intersects(InodeFlags::IMMUTABLE | InodeFlags::APPEND_ONLY) {
                return Err(Ext4Error::Readonly);
            }
            next = inode.dtime_val();
            chain.push(inode);
        }
        Ok(chain)
    }

    /// Validate and list the admitted zero-link regular-file orphan chain.
    #[maybe_async::maybe_async]
    pub async fn orphan_inodes(&self) -> Result<Vec<InodeIndex>, Ext4Error> {
        Ok(self.orphan_chain().await?.into_iter().map(|inode| inode.index).collect())
    }

    #[maybe_async::maybe_async]
    pub(crate) async fn defer_unlinked_inode(&self, inode: &mut Inode) -> Result<(), Ext4Error> {
        if self.0.writer.is_none() { return Err(Ext4Error::Readonly); }
        self.validate_orphan_allocation(inode.index).await?;
        let chain = self.orphan_chain().await?;
        if chain.len() == MAX_ORPHANS || chain.iter().any(|item| item.index == inode.index)
            || inode.links_count() != 0 || !inode.file_type().is_regular_file() {
            return Err(CorruptKind::OrphanInode(inode.index.get()).into());
        }
        inode.set_dtime_val(self.0.superblock.last_orphan());
        inode.write(self).await?;
        self.0.superblock.set_last_orphan(inode.index.get());
        self.0.superblock.write(self).await
    }

    /// Remove an orphan from any list position and free its inode/data through
    /// the configured writer, including every data/metadata block revocation.
    #[maybe_async::maybe_async]
    pub async fn release_orphan(&self, index: InodeIndex) -> Result<(), Ext4Error> {
        if self.0.writer.is_none() { return Err(Ext4Error::Readonly); }
        let mut chain = self.orphan_chain().await?;
        let position = chain.iter().position(|inode| inode.index == index).ok_or(Ext4Error::NotFound)?;
        let inode = chain.remove(position);
        if position == 0 {
            self.0.superblock.set_last_orphan(inode.dtime_val());
            self.0.superblock.write(self).await?;
        } else {
            let previous = &mut chain[position - 1];
            previous.set_dtime_val(inode.dtime_val());
            previous.write_preserving_times(self).await?;
        }
        self.delete_file(inode).await
    }
}
