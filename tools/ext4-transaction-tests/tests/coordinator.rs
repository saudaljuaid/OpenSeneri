// SPDX-License-Identifier: GPL-3.0-only
//! Exercise the production coordinator with the existing e2fsprogs fixture.

#[allow(dead_code)]
#[path = "../../../src/rust/ext4.rs"]
mod ext4;

use ext4::Status;
use std::cell::RefCell;
use std::path::PathBuf;

#[derive(Clone, Debug, Eq, PartialEq)]
enum Event {
    Write(u64, Vec<u8>),
    Flush(u32),
}

#[derive(Default)]
struct Device {
    bytes: Vec<u8>,
    events: Vec<Event>,
    fail_event: Option<usize>,
    accept_failed_write: bool,
    fail_superblock_read: bool,
    failed_reads: usize,
    watched_read_block: Option<u64>,
    watched_reads: usize,
    fail_watched_read: Option<usize>,
}

thread_local! {
    static DEVICE: RefCell<Device> = RefCell::new(Device::default());
    static MUTATION_TIME: std::cell::Cell<u64> = const { std::cell::Cell::new(1_780_000_000) };
}

// These are the production adapter's three native I/O callbacks. No upstream
// Ext4 writer receives the device; only the coordinator's executor calls write.
mod abi {
    use super::{DEVICE, Event};
    pub fn ext4_current_time(context: usize) -> u64 {
        assert_eq!(context, 1);
        super::MUTATION_TIME.with(std::cell::Cell::get)
    }

    pub fn ext4_block_read(context: usize, start: u64, output: &mut [u8]) -> bool {
        assert_eq!(context, 1);
        DEVICE.with_borrow_mut(|device| {
            if device.watched_read_block.is_some_and(|block| start / 4096 == block) {
                device.watched_reads += 1;
                if device.fail_watched_read == Some(device.watched_reads) { return false; }
            }
            if device.fail_superblock_read && start == 1024 && output.len() == 1024 {
                device.failed_reads += 1;
                return false;
            }
            let Ok(start) = usize::try_from(start) else {
                return false;
            };
            let Some(end) = start.checked_add(output.len()) else {
                return false;
            };
            let Some(bytes) = device.bytes.get(start..end) else {
                return false;
            };
            output.copy_from_slice(bytes);
            true
        })
    }

    pub fn ext4_block_write(context: usize, start: u64, input: &[u8]) -> bool {
        assert_eq!(context, 1);
        DEVICE.with_borrow_mut(|device| {
            let failed = device.fail_event == Some(device.events.len());
            device.events.push(Event::Write(start, input.to_vec()));
            if !failed || device.accept_failed_write {
                let Ok(start) = usize::try_from(start) else {
                    return false;
                };
                let Some(end) = start.checked_add(input.len()) else {
                    return false;
                };
                let Some(bytes) = device.bytes.get_mut(start..end) else {
                    return false;
                };
                bytes.copy_from_slice(input);
            }
            !failed
        })
    }

    pub fn ext4_block_flush(context: usize, boundary: u32) -> bool {
        assert_eq!(context, 1);
        DEVICE.with_borrow_mut(|device| {
            let failed = device.fail_event == Some(device.events.len());
            device.events.push(Event::Flush(boundary));
            !failed
        })
    }
}

fn fixture() -> Option<PathBuf> {
    let Some(path) = std::env::var_os("PHIPIA_EXT4_RUST_FIXTURE") else {
        eprintln!("coordinator fixture unavailable; no Linux interoperability gate claimed");
        return None;
    };
    let path = PathBuf::from(path);
    assert!(path.is_file(), "configured coordinator fixture must exist");
    Some(path)
}

fn mount_fixture(path: &std::path::Path) -> Box<ext4::Mounted> {
    let bytes = std::fs::read(path).unwrap();
    mount_bytes(bytes)
}

#[test]
fn admitted_fixture_has_complete_storage_and_namespace_census() {
    let Some(path) = fixture() else { return };
    let bytes = std::fs::read(&path).unwrap();
    DEVICE.with_borrow_mut(|device| *device = Device { bytes: bytes.clone(), ..Device::default() });
    let profile = ext4::validate_profile(1, bytes.len() as u64);
    assert!(profile.is_ok(), "profile {profile:?}; fields={:?}",
        [0x18, 0x1c, 0x20, 0x24, 0x28, 0x48, 0x4c, 0x174].map(|offset|
            (offset, u32::from_le_bytes(bytes[1024 + offset..1028 + offset].try_into().unwrap()))));
    let raw = ext4plus::Ext4::load(Box::new(bytes)).unwrap();
    let mut blocks = raw.block_allocation_snapshot();
    blocks.reserve_fixed_metadata().expect("fixed storage census");
    blocks.validate_internal_journal().expect("journal storage census");
    let mut allocations = raw.inode_allocation_snapshot();
    let mut pending = vec![String::from("/")];
    let mut known = std::collections::BTreeMap::new();
    let mut references = std::collections::BTreeMap::<u32, u32>::new();
    while let Some(path) = pending.pop() {
        let node = raw.path_to_inode(ext4plus::path::Path::try_from(path.as_str()).unwrap(),
            ext4plus::FollowSymlinks::ExcludeFinalComponent).unwrap();
        if known.contains_key(&node.index.get()) { continue; }
        blocks.validate_inode_extents(&node).unwrap_or_else(|error|
            panic!("storage census {path}, inode {}: {error:?}", node.index));
        assert!(allocations.is_allocated(node.index).unwrap());
        known.insert(node.index.get(), (path.clone(), node.links_count()));
        if node.file_type().is_dir() {
            for entry in raw.read_dir(path.as_str()).unwrap() {
                let entry = entry.unwrap();
                *references.entry(entry.inode.get()).or_default() += 1;
                if entry.file_name() != "." && entry.file_name() != ".." {
                    pending.push(String::from_utf8(entry.path().as_ref().to_vec()).unwrap());
                }
            }
        }
    }
    for (inode, (path, links)) in &known {
        assert_eq!(references.get(inode).copied().unwrap_or(0), u32::from(*links),
            "namespace census {path}, inode {inode}");
    }
    blocks.finish(&mut allocations).expect("complete bitmap/reserved-inode census");
    let mounted = mount_fixture(&path);
    ext4::unmount(&mounted).unwrap();
}

fn mount_bytes(bytes: Vec<u8>) -> Box<ext4::Mounted> {
    let length = bytes.len() as u64;
    DEVICE.with_borrow_mut(|device| {
        *device = Device {
            bytes,
            ..Device::default()
        }
    });
    ext4::mount(1, length).unwrap().0
}

fn assert_public_reads_refused(mounted: &ext4::Mounted) {
    assert_eq!(ext4::free_bytes(mounted), Err(Status::Io));
    assert_eq!(ext4::stat(mounted, b"system/README.TXT"), Err(Status::Io));
    let mut bytes = [0xa5; 16];
    assert_eq!(
        ext4::pread(mounted, b"system/README.TXT", 0, &mut bytes),
        Err(Status::Io)
    );
    assert_eq!(bytes, [0xa5; 16]);
    assert!(matches!(
        ext4::directory_entry(mounted, b"system", 0),
        Err(Status::Io)
    ));
    assert!(ext4::unmount(mounted).is_err());
}

fn write_sparse_fixture(path: &std::path::Path, bytes: &[u8]) -> std::io::Result<()> {
    use std::io::{Seek, SeekFrom, Write};
    let mut file = std::fs::File::create(path)?;
    for (index, block) in bytes.chunks(4096).enumerate() {
        if block.iter().any(|byte| *byte != 0) {
            file.seek(SeekFrom::Start(index as u64 * 4096))?;
            file.write_all(block)?;
        }
    }
    file.set_len(bytes.len() as u64)
}

#[test]
fn sparse_fixture_writer_preserves_holes_partial_tail_and_overwrite_length() {
    let Some(path) = fixture() else { return };
    let output = path.with_extension("coordinator-sparse-writer.img");
    let mut bytes = vec![0; 9 * 4096 + 3];
    bytes[4093..4101].copy_from_slice(b"boundary");
    bytes[9 * 4096..].copy_from_slice(b"end");
    write_sparse_fixture(&output, &bytes).unwrap();
    assert_eq!(std::fs::read(&output).unwrap(), bytes);
    bytes.truncate(13);
    write_sparse_fixture(&output, &bytes).unwrap();
    assert_eq!(std::fs::read(&output).unwrap(), bytes);
}

fn fsck(path: &std::path::Path, suffix: &str) {
    let output = path.with_extension(format!("{suffix}.img"));
    // Keep every exact image, but represent zero-filled ranges as host holes.
    // Dense copies of every crash cut exhausted the Linux runner's disk.
    DEVICE.with_borrow(|device| write_sparse_fixture(&output, &device.bytes).unwrap());
    let result = std::process::Command::new("e2fsck")
        .args(["-f", "-n"])
        .arg(&output)
        .output()
        .unwrap();
    let log = format!(
        "{}\n{}",
        String::from_utf8_lossy(&result.stdout),
        String::from_utf8_lossy(&result.stderr)
    );
    std::fs::write(output.with_extension("e2fsck.txt"), &log).unwrap();
    println!("ext4 coordinator fsck {suffix}:\n{log}");
    let hash = std::process::Command::new("sha256sum").arg(&output).output().unwrap();
    assert!(hash.status.success(), "could not hash coordinator disk");
    std::fs::write(output.with_extension("sha256.txt"), &hash.stdout).unwrap();
    println!("ext4 coordinator disk {}", String::from_utf8_lossy(&hash.stdout));
    assert!(result.status.success(), "e2fsck rejected {suffix}: {log}");
}

fn read_exact(mounted: &ext4::Mounted, path: &[u8], output: &mut [u8]) {
    let mut read = 0;
    while read < output.len() {
        let count = ext4::pread(mounted, path, read as u64, &mut output[read..]).unwrap();
        assert!(count > 0, "unexpected EOF at {read}");
        read += count;
    }
}

#[test]
fn rollback_reload_failure_drops_allocator_view_and_sync_retries() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    // Arm the journal, then force inline-xattr ENOSPC and refuse the rollback
    // reload. Allocation rollback is independently covered by the full fixture.
    ext4::transaction_probe(&mut mounted, b"system/README.TXT", 0, b"R").unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    let original = ext4::stat(&mounted, b"system/README.TXT").unwrap();
    let before = DEVICE.with_borrow(|device| device.bytes.clone());
    DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.fail_superblock_read = true;
    });
    assert_eq!(
        ext4::set_xattr(&mut mounted, b"system/README.TXT", b"user.too-large", Some(&[0x52; 4096])),
        Err(Status::Io)
    );
    DEVICE.with_borrow(|device| {
        assert!(device.failed_reads > 0, "rollback reload was not injected");
        assert!(
            device.events.is_empty(),
            "upstream mutation wrote to the device"
        );
        assert_eq!(device.bytes, before);
    });
    assert_public_reads_refused(&mounted);
    assert_eq!(ext4::sync(&mut mounted), Err(Status::Io));
    assert_public_reads_refused(&mounted);
    DEVICE.with_borrow_mut(|device| device.fail_superblock_read = false);
    ext4::sync(&mut mounted).unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    assert_eq!(ext4::stat(&mounted, b"system/README.TXT"), Ok(original));
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-rollback");
}

#[test]
fn commit_reload_failure_hides_view_and_retry_does_not_rewrite_storage() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, b"system/reload-test", 0o600).unwrap();
    DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.fail_superblock_read = true;
    });
    assert_eq!(
        ext4::transaction_probe(&mut mounted, b"system/reload-test", 0, b"saved"),
        Err(Status::Io)
    );
    DEVICE.with_borrow(|device| {
        assert_eq!(device.events.last(), Some(&Event::Flush(5)));
        assert!(device.failed_reads > 0);
    });
    assert_public_reads_refused(&mounted);
    DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.fail_superblock_read = false;
    });
    assert_eq!(
        ext4::transaction_probe(&mut mounted, b"system/reload-test", 0, b"saved"),
        Ok(5)
    );
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
    let mut bytes = [0; 5];
    assert_eq!(
        ext4::pread(&mounted, b"system/reload-test", 0, &mut bytes),
        Ok(5)
    );
    assert_eq!(&bytes, b"saved");
    DEVICE.with_borrow_mut(|device| device.fail_superblock_read = true);
    assert_eq!(ext4::sync(&mut mounted), Err(Status::Io));
    DEVICE.with_borrow(|device| assert_eq!(device.events.last(), Some(&Event::Flush(0))));
    assert_public_reads_refused(&mounted);
    DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.fail_superblock_read = false;
    });
    ext4::sync(&mut mounted).unwrap();
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-reload");
}

#[test]
fn pending_commit_is_hidden_and_every_storage_refusal_retries_exact_bytes() {
    let Some(path) = fixture() else { return };
    for case in 0..28 {
        let mut mounted = mount_fixture(&path);
        if case != 0 && case < 5 {
            if case == 4 {
                ext4::create_directory_probe(&mut mounted, b"system/retry-test").unwrap();
            } else {
                ext4::create_file_probe(&mut mounted, b"system/retry-test", 0o600).unwrap();
            }
            if case == 2 {
                ext4::transaction_probe(&mut mounted, b"system/retry-test", 0, &vec![0x5a; 8192])
                    .unwrap();
            }
            ext4::sync(&mut mounted).unwrap();
        }
        if case == 7 || case == 22 {
            ext4::create_file_probe(&mut mounted, b"system/retry-test", 0o600).unwrap();
            ext4::create_file_probe(&mut mounted, b"data/user/retry-test", 0o600).unwrap();
            ext4::transaction_probe(&mut mounted, b"data/user/retry-test", 0, &vec![0x33; 8192]).unwrap();
            ext4::sync(&mut mounted).unwrap();
        }
        if case == 8 {
            ext4::create_file_probe(&mut mounted, b"system/retry-test", 0o600).unwrap();
            ext4::transaction_probe(&mut mounted, b"system/retry-test", 0, &vec![0x71; 4093]).unwrap();
            ext4::sync(&mut mounted).unwrap();
        }
        if case >= 9 && case != 22 {
            if case == 19 || case == 23 || case == 24 {
                ext4::create_directory_probe(&mut mounted, b"system/retry-test").unwrap();
                if case == 24 {
                    ext4::create_directory_probe(&mut mounted, b"data/user/retry-test").unwrap();
                }
            } else if case == 17 {
                ext4::symlink_probe(&mut mounted, b"system/retry-test", b"missing").unwrap();
            } else {
                ext4::create_file_probe(&mut mounted, b"system/retry-test", 0o600).unwrap();
            }
            if case == 11 {
                ext4::set_xattr(&mut mounted, b"system/retry-test", b"user.note", Some(b"old")).unwrap();
            }
            if case == 26 || case == 27 {
                ext4::set_xattr(&mut mounted, b"system/retry-test", b"user.note", Some(&[0x41; 701])).unwrap();
            }
            if case == 14 || case == 21 {
                ext4::transaction_probe(&mut mounted, b"system/retry-test", 0, &vec![0x37; 8192]).unwrap();
            }
            if case == 18 {
                ext4::link_file_probe(&mut mounted, b"system/retry-test", b"system/retained-alias").unwrap();
            }
            ext4::sync(&mut mounted).unwrap();
        }
        let initial = DEVICE.with_borrow_mut(|device| {
            device.events.clear();
            device.bytes.clone()
        });
        drop(mounted);
        let initial = if case == 8 || case == 12 {
            let image = path.with_extension(format!("coordinator-append-retry-{case}.img"));
            std::fs::write(&image, initial).unwrap();
            debugfs(&image, "set_inode_field /system/retry-test flags 0x80020");
            std::fs::read(&image).unwrap()
        } else { initial };
        let mut mounted = mount_bytes(initial.clone());
        let target = if case == 5 { b"README.TXT".to_vec() } else { vec![b'x'; 97] };
        let inode = ext4::stat(&mounted, b"system/retry-test").map(|metadata| metadata.inode).unwrap_or(0);
        let replaced_inode = ext4::stat(&mounted, b"data/user/retry-test").map(|metadata| metadata.inode).unwrap_or(0);
        let mutate = |mounted: &mut ext4::Mounted| match case {
            0 => ext4::create_file_probe(mounted, b"system/retry-test", 0o600),
            1 => ext4::transaction_probe(mounted, b"system/retry-test", 4095, b"cross-block")
                .map(|_| ()),
            2 => ext4::truncate_probe(mounted, b"system/retry-test", 101),
            3 | 4 => ext4::rename_probe(mounted, b"system/retry-test", b"data/user/retry-test"),
            7 => ext4::rename_replace_probe(mounted, b"system/retry-test", b"data/user/retry-test"),
            8 => ext4::append_probe(mounted, b"system/retry-test", b"append-retry", 16384)
                .map(|result| assert_eq!(result, (4093, 12))),
            9 => ext4::chmod(mounted, b"system/retry-test", 0o640),
            10 => ext4::set_xattr(mounted, b"system/retry-test", b"user.note", Some(b"journaled")),
            11 => ext4::set_xattr(mounted, b"system/retry-test", b"user.note", None),
            12 => ext4::append_inode(mounted, inode, b"stable", 16384).map(|result| assert_eq!(result, (0, 6))),
            13 => ext4::write_inode(mounted, inode, 4095, b"stable").map(|count| assert_eq!(count, 6)),
            14 => ext4::truncate_inode(mounted, inode, 101),
            15 => ext4::set_times(mounted, b"system/retry-test", 2_200_000_000, 123, 2_300_000_000, 456),
            16 | 17 => ext4::link_file_probe(mounted, b"system/retry-test", b"data/user/retry-test"),
            18 => ext4::unlink_file_guarded(mounted, b"system/retry-test", &[inode]),
            19 | 20 => ext4::remove_entry_guarded(mounted, b"system/retry-test", &[]),
            21 => ext4::unlink_file_guarded(mounted, b"system/retry-test", &[inode, inode]),
            22 => ext4::rename_replace_guarded(mounted, b"system/retry-test", b"data/user/retry-test",
                &[inode, replaced_inode, replaced_inode]),
            23 => ext4::remove_directory_guarded(mounted, b"system/retry-test", &[inode, inode]),
            24 => ext4::rename_replace_guarded(mounted, b"system/retry-test", b"data/user/retry-test",
                &[inode, replaced_inode, replaced_inode]),
            25 | 26 => ext4::set_xattr(mounted, b"system/retry-test", b"user.note", Some(&[0x52; 701])),
            27 => ext4::set_xattr(mounted, b"system/retry-test", b"user.note", Some(b"inline")),
            _ => ext4::symlink_probe(mounted, b"system/retry-test", &target),
        };
        mutate(&mut mounted).unwrap();
        let expected = DEVICE.with_borrow(|device| device.events.clone());
        if case == 1 {
            assert!(
                expected.contains(&Event::Flush(1)),
                "ordered data not exercised"
            );
        }
        ext4::sync(&mut mounted).unwrap();
        let expected_disk = DEVICE.with_borrow(|device| device.bytes.clone());
        drop(mounted);
        // Both a write rejected before acceptance and a write accepted with a lost
        // completion must retain the same plan. Flush errors never imply durability.
        for accept in [false, true] {
            for failed_at in 0..expected.len() {
                let mut mounted = mount_bytes(initial.clone());
                DEVICE.with_borrow_mut(|device| {
                    device.fail_event = Some(failed_at);
                    device.accept_failed_write = accept;
                });
                assert_eq!(
                    mutate(&mut mounted),
                    Err(Status::Io),
                    "event {failed_at}, accepted {accept}"
                );
                let failed_prefix = DEVICE.with_borrow(|device| device.events.clone());
                assert_eq!(failed_prefix, expected[..=failed_at]);
                if failed_at >= 2 {
                    assert_public_reads_refused(&mounted);
                    if case == 8 {
                        assert_eq!(ext4::transaction_probe(&mut mounted, b"system/retry-test", 4093, b"append-retry"),
                            Err(Status::Invalid), "ordinary write stole pending append");
                    }
                    assert_eq!(
                        ext4::create_file_probe(&mut mounted, b"system/other", 0o600),
                        Err(Status::Invalid)
                    );
                }
                DEVICE.with_borrow_mut(|device| {
                    device.events.clear();
                    device.fail_event = None;
                });
                mutate(&mut mounted).unwrap();
                let retry = DEVICE.with_borrow(|device| device.events.clone());
                let phase_start = if failed_at < 2 {
                    0
                } else if failed_at >= expected.len() - 2 {
                    expected.len() - 2
                } else {
                    2
                };
                assert_eq!(retry, expected[phase_start..], "retry event {failed_at}");
                let name: &[u8] = if case == 18 { b"system/retained-alias" }
                    else if case == 3 || case == 4 || case == 7 || case == 22 || case == 24 { b"data/user/retry-test" }
                    else { b"system/retry-test" };
                if case == 19 || case == 20 || case == 21 || case == 23 {
                    assert_eq!(ext4::lstat(&mounted, name), Err(Status::NotFound));
                } else if case == 5 || case == 6 || case == 17 {
                    let target = if case == 17 { b"missing".as_slice() } else { target.as_slice() };
                    let mut bytes = vec![0; target.len()];
                    assert_eq!(ext4::readlink(&mounted, name, &mut bytes), Ok(target.len()));
                    assert_eq!(bytes, target);
                    assert_eq!(ext4::lstat(&mounted, name).unwrap().links, if case == 17 { 2 } else { 1 });
                } else {
                    assert_eq!(ext4::stat(&mounted, name).unwrap().links,
                        if case == 4 || case == 16 || case == 24 { 2 } else { 1 }, "case {case}, event {failed_at}");
                }
                ext4::sync(&mut mounted).unwrap();
                ext4::unmount(&mounted).unwrap();
                DEVICE.with_borrow(|device| {
                    assert_eq!(
                        device.bytes, expected_disk,
                        "case {case}, event {failed_at}, accepted {accept}"
                    )
                });
            }
        }
        // Every refusal converged byte-for-byte to this same image, including
        // counters, journal sequence and bitmap/metadata checksums.
        fsck(&path, &format!("coordinator-retry-case-{case}"));
    }
}

#[test]
fn append_uses_live_eof_through_aliases_and_refuses_overflow_without_writes() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/append-file";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::symlink_probe(&mut mounted, b"system/append-alias", b"append-file").unwrap();
    let first = vec![0x63; 4093];
    assert_eq!(ext4::append_probe(&mut mounted, name, &first, 16384), Ok((0, 4093)));
    assert_eq!(ext4::append_probe(&mut mounted, b"system/append-alias", b"second", 16384), Ok((4093, 6)));
    assert_eq!(ext4::append_probe(&mut mounted, name, b"third", 16384), Ok((4099, 5)));
    DEVICE.with_borrow_mut(|device| device.events.clear());
    assert_eq!(ext4::append_probe(&mut mounted, name, b"overflow", 4104), Err(Status::Range));
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
    ext4::sync(&mut mounted).unwrap();
    let bytes = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let mut mounted = mount_bytes(bytes);
    let mut actual = vec![0; 4104];
    read_exact(&mounted, name, &mut actual);
    let mut expected = first;
    expected.extend_from_slice(b"secondthird");
    assert_eq!(actual, expected);
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-append");
}

#[test]
fn maximum_vfs_file_growth_keeps_holes_zero_and_reclaims_the_last_extent() {
    let Some(path) = fixture() else { return };
    let maximum = 64 * 1024 * 1024u64;
    // Keep this interoperability boundary tied to the ordinary C backend.
    assert!(include_str!("../../../include/phipia/ext4_fs.h")
        .contains("PHIPIA_EXT4_MAX_MUTABLE_FILE_BYTES UINT64_C(67108864)"));
    let name = b"system/maximum-vfs-file";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::truncate_probe(&mut mounted, name, maximum - 2).unwrap();
    assert_eq!(ext4::free_bytes(&mounted).unwrap(), free);
    assert_eq!(ext4::transaction_probe(&mut mounted, name, maximum - 2, b"x"), Ok(1));
    assert_eq!(ext4::append_probe(&mut mounted, name, b"y", maximum), Ok((maximum - 1, 1)));
    assert_eq!(ext4::stat(&mounted, name).unwrap().size, maximum);
    assert_eq!(ext4::free_bytes(&mounted).unwrap(), free - 4096);
    DEVICE.with_borrow_mut(|device| device.events.clear());
    assert_eq!(ext4::append_probe(&mut mounted, name, b"z", maximum), Err(Status::Range));
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-maximum-vfs-live");
    drop(mounted);
    let persisted = DEVICE.with_borrow(|device| device.bytes.clone());
    let mut mounted = mount_bytes(persisted);
    for offset in [0, 16 * 1024 * 1024, maximum - 8192] {
        let mut hole = [0xa5; 4096];
        assert_eq!(ext4::pread(&mounted, name, offset, &mut hole), Ok(hole.len()));
        assert!(hole.iter().all(|byte| *byte == 0));
    }
    let mut tail = [0xa5; 4096];
    assert_eq!(ext4::pread(&mounted, name, maximum - 4096, &mut tail), Ok(tail.len()));
    assert!(tail[..4094].iter().all(|byte| *byte == 0));
    assert_eq!(&tail[4094..], b"xy");
    ext4::truncate_probe(&mut mounted, name, maximum - 1).unwrap();
    ext4::truncate_probe(&mut mounted, name, maximum).unwrap();
    assert_eq!(ext4::pread(&mounted, name, maximum - 2, &mut tail[..2]), Ok(2));
    assert_eq!(&tail[..2], b"x\0");
    ext4::truncate_probe(&mut mounted, name, 0).unwrap();
    assert_eq!(ext4::free_bytes(&mounted).unwrap(), free);
    ext4::unlink_file_probe(&mut mounted, name).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-maximum-vfs-reclaimed");
}

#[test]
fn mutation_timestamps_are_journaled_and_linux_epoch_encoding_matches() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/time-test";
    let create_time = 1_780_000_100;
    MUTATION_TIME.with(|time| time.set(create_time));
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    let snapshot = || {
        let bytes = DEVICE.with_borrow(|device| device.bytes.clone());
        let raw = ext4plus::Ext4::load(Box::new(bytes)).unwrap();
        raw.metadata(b"/system/time-test").unwrap()
    };
    assert_eq!(snapshot().crtime.unwrap().as_secs(), create_time);
    assert_eq!(snapshot().atime.as_secs(), create_time);
    MUTATION_TIME.with(|time| time.set(create_time + 1));
    ext4::transaction_probe(&mut mounted, name, 0, b"timestamp").unwrap();
    assert_eq!(snapshot().mtime.as_secs(), create_time + 1);
    MUTATION_TIME.with(|time| time.set(create_time + 2));
    ext4::chmod(&mut mounted, name, 0o640).unwrap();
    assert_eq!(snapshot().ctime.as_secs(), create_time + 2);
    assert_eq!(snapshot().mtime.as_secs(), create_time + 1);
    assert_eq!(snapshot().atime.as_secs(), create_time);
    for seconds in [0x7fff_ffff, 0x8000_0000, 0xffff_ffff, 0x1_8000_0000, 0x3_7fff_ffff] {
        MUTATION_TIME.with(|time| time.set(seconds));
        ext4::transaction_probe(&mut mounted, name, 0, b"time").unwrap();
        assert_eq!(snapshot().mtime.as_secs(), seconds);
        ext4::sync(&mut mounted).unwrap();
        let image = path.with_extension("coordinator-time-check.img");
        DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
        let report = std::process::Command::new("debugfs").args(["-R", "stat /system/time-test"])
            .arg(&image).output().unwrap();
        assert!(report.status.success());
        let report = String::from_utf8(report.stdout).unwrap();
        let epoch = (seconds + 0x8000_0000) >> 32;
        assert!(report.contains(&format!("mtime: 0x{:08x}:{epoch:08x}", seconds as u32)), "{report}");
    }
    MUTATION_TIME.with(|time| time.set(u64::MAX));
    DEVICE.with_borrow_mut(|device| device.events.clear());
    assert_eq!(ext4::chmod(&mut mounted, name, 0o600), Err(Status::Io));
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
    MUTATION_TIME.with(|time| time.set(create_time));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-timestamps");
}

#[test]
fn directory_times_follow_namespace_changes_and_explicit_times_preserve_nanoseconds() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let dir = b"system/time-dir";
    let now = 1_780_000_000;
    MUTATION_TIME.with(|time| time.set(now));
    ext4::create_directory_probe(&mut mounted, dir).unwrap();
    let metadata = |path: &[u8]| {
        let bytes = DEVICE.with_borrow(|device| device.bytes.clone());
        ext4plus::Ext4::load(Box::new(bytes)).unwrap().metadata(path).unwrap()
    };
    MUTATION_TIME.with(|time| time.set(now + 1));
    ext4::create_file_probe(&mut mounted, b"system/time-dir/child", 0o600).unwrap();
    assert_eq!(metadata(b"/system/time-dir").mtime.as_secs(), now + 1);
    MUTATION_TIME.with(|time| time.set(now + 2));
    ext4::chmod(&mut mounted, dir, 0o2751).unwrap();
    assert_eq!(metadata(b"/system/time-dir").mtime.as_secs(), now + 1);
    assert_eq!(metadata(b"/system/time-dir").ctime.as_secs(), now + 2);
    ext4::chmod(&mut mounted, dir, 0o751).unwrap();
    assert_eq!(metadata(b"/system/time-dir").mode.bits() & 0o7777, 0o751);
    ext4::set_times(&mut mounted, dir, 0x8000_0000, 123_456_789, 0xffff_ffff, 999_999_999).unwrap();
    let value = metadata(b"/system/time-dir");
    assert_eq!(value.atime, std::time::Duration::new(0x8000_0000, 123_456_789));
    assert_eq!(value.mtime, std::time::Duration::new(0xffff_ffff, 999_999_999));
    assert_eq!(value.ctime.as_secs(), now + 2);
    DEVICE.with_borrow_mut(|device| device.events.clear());
    assert_eq!(ext4::set_times(&mut mounted, dir, 1, 1_000_000_000, 2, 0), Err(Status::Range));
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
    MUTATION_TIME.with(|time| time.set(now + 3));
    ext4::rename_probe(&mut mounted, dir, b"data/user/time-dir").unwrap();
    let renamed = metadata(b"/data/user/time-dir");
    assert_eq!(renamed.ctime.as_secs(), now + 3);
    assert_eq!(renamed.mtime, value.mtime);
    assert_eq!(metadata(b"/system").mtime.as_secs(), now + 3);
    assert_eq!(metadata(b"/data/user").mtime.as_secs(), now + 3);
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-explicit-times");
}

#[test]
fn mode_and_inline_xattrs_survive_remount_and_rollback_enospc() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/metadata-test";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::chmod(&mut mounted, name, 0o751).unwrap();
    assert_eq!(ext4::stat(&mounted, name).unwrap().mode & 0o777, 0o751);
    ext4::set_xattr(&mut mounted, name, b"user.note", Some(b"first")).unwrap();
    let mut output = [0xa5; 12];
    assert_eq!(ext4::get_xattr(&mounted, name, b"user.note", &mut []), Ok(5));
    assert_eq!(ext4::get_xattr(&mounted, name, b"user.note", &mut output[..4]), Err(Status::Range));
    assert_eq!(output, [0xa5; 12]);
    assert_eq!(ext4::get_xattr(&mounted, name, b"user.note", &mut output), Ok(5));
    assert_eq!(&output[..5], b"first");
    let free = ext4::free_bytes(&mounted).unwrap();
    assert_eq!(ext4::set_xattr(&mut mounted, name, b"user.note", Some(&[0x52; 4096])), Err(Status::Full));
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    assert_eq!(ext4::get_xattr(&mounted, name, b"user.note", &mut output), Ok(5));
    assert_eq!(&output[..5], b"first");
    assert_eq!(ext4::set_xattr(&mut mounted, name, b"security.capability", Some(b"x")), Err(Status::Invalid));
    ext4::set_xattr(&mut mounted, name, b"user.note", Some(b"changed")).unwrap();
    ext4::set_xattr(&mut mounted, name, b"user.empty", Some(b"")).unwrap();
    ext4::sync(&mut mounted).unwrap();
    let bytes = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let mut mounted = mount_bytes(bytes);
    assert_eq!(ext4::stat(&mounted, name).unwrap().mode & 0o777, 0o751);
    assert_eq!(ext4::get_xattr(&mounted, name, b"user.note", &mut output), Ok(7));
    assert_eq!(&output[..7], b"changed");
    assert_eq!(ext4::get_xattr(&mounted, name, b"user.empty", &mut output), Ok(0));
    ext4::set_xattr(&mut mounted, name, b"user.note", None).unwrap();
    assert_eq!(ext4::get_xattr(&mounted, name, b"user.note", &mut output), Err(Status::NotFound));
    assert_eq!(ext4::set_xattr(&mut mounted, name, b"user.note", None), Err(Status::NotFound));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-mode-xattrs");
}

#[test]
fn linux_acl_entries_survive_user_xattr_mutation_and_guard_chmod() {
    let Some(path) = fixture() else { return };
    let name = b"system/acl-preserve";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o640).unwrap();
    ext4::transaction_probe(&mut mounted, name, 0, b"access ACL").unwrap();
    let number = ext4::stat(&mounted, name).unwrap().inode as u32;
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    let image = path.with_extension("coordinator-acl-input.img");
    write_sparse_fixture(&image, &DEVICE.with_borrow(|device| device.bytes.clone())).unwrap();
    let mut posix = Vec::from(2u32.to_le_bytes());
    let mut disk_acl = Vec::from(1u32.to_le_bytes());
    for (tag, permissions, id) in [(1u16, 6u16, u32::MAX), (2, 4, 1000),
        (4, 4, u32::MAX), (16, 4, u32::MAX), (32, 0, u32::MAX)] {
        posix.extend_from_slice(&tag.to_le_bytes());
        posix.extend_from_slice(&permissions.to_le_bytes());
        posix.extend_from_slice(&id.to_le_bytes());
        disk_acl.extend_from_slice(&tag.to_le_bytes());
        disk_acl.extend_from_slice(&permissions.to_le_bytes());
        if tag == 2 { disk_acl.extend_from_slice(&id.to_le_bytes()); }
    }
    let acl_file = path.with_extension("coordinator-posix-acl.bin");
    std::fs::write(&acl_file, &posix).unwrap();
    debugfs(&image, &format!("ea_set -f {} /system/acl-preserve system.posix_acl_access", acl_file.display()));
    debugfs(&image, "ea_set /system/acl-preserve user.note first");
    let baseline = std::fs::read(&image).unwrap();
    let check_acl = |bytes: Vec<u8>| {
        let raw = ext4plus::Ext4::load(Box::new(bytes)).unwrap();
        let inode = ext4plus::inode::Inode::read(&raw, std::num::NonZeroU32::new(number).unwrap()).unwrap();
        assert_eq!(inode.get_xattr(&raw, b"system.posix_acl_access").unwrap().as_ref(), Some(&disk_acl));
        assert!(inode.list_xattrs(&raw).unwrap().contains(&b"system.posix_acl_access".to_vec()));
    };
    check_acl(baseline.clone());
    let mut mounted = mount_bytes(baseline.clone());
    fsck(&path, "coordinator-acl-import");
    ext4::set_xattr(&mut mounted, name, b"user.note", Some(b"second")).unwrap();
    let before = DEVICE.with_borrow_mut(|device| { device.events.clear(); device.bytes.clone() });
    assert_eq!(ext4::chmod(&mut mounted, name, 0o777), Err(Status::ReadOnly));
    DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, before); });
    assert_eq!(ext4::stat(&mounted, name).unwrap().mode & 0o777, 0o640);
    ext4::transaction_probe(&mut mounted, name, 0, b"preserved").unwrap();
    ext4::set_xattr(&mut mounted, name, b"user.note", None).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    let output = DEVICE.with_borrow(|device| device.bytes.clone());
    check_acl(output.clone());
    fsck(&path, "coordinator-acl-preserved");
    write_sparse_fixture(&image, &output).unwrap();
    let exported = path.with_extension("coordinator-posix-acl-export.bin");
    debugfs(&image, &format!("ea_get -f {} /system/acl-preserve system.posix_acl_access", exported.display()));
    assert_eq!(std::fs::read(exported).unwrap(), posix);
    drop(mounted);

    let ipg = u32::from_le_bytes(baseline[1064..1068].try_into().unwrap());
    let descriptor = 4096 + ((number - 1) / ipg) as usize * 64;
    let table = u32::from_le_bytes(baseline[descriptor + 8..descriptor + 12].try_into().unwrap()) as usize * 4096;
    let inode_start = table + ((number - 1) % ipg) as usize * 256;
    let extra = u16::from_le_bytes(baseline[inode_start + 128..inode_start + 130].try_into().unwrap()) as usize;
    let body = inode_start + 128 + extra;
    assert_eq!(&baseline[body..body + 4], &0xea02_0000u32.to_le_bytes());
    // Locate the non-ACL entry; inode-body entries need not be sorted.
    let mut entry = body + 4;
    while baseline[entry + 1] != 1 {
        assert_ne!(&baseline[entry..entry + 4], &[0; 4]);
        entry += (16 + baseline[entry] as usize + 3) & !3;
    }
    assert_eq!(&baseline[entry + 16..entry + 20], b"note");
    let generation = u32::from_le_bytes(baseline[inode_start + 0x64..inode_start + 0x68].try_into().unwrap());
    for case in 0..5 {
        for dirty in [false, true] {
            let mut hostile = baseline.clone();
            match case {
                0 => hostile[entry] = 0, // only ACL indices admit an empty name
                1 => hostile[entry + 16] = 0,
                2 => hostile[entry + 2..entry + 4].fill(0), // value over names
                3 => hostile[entry + 2..entry + 4].copy_from_slice(&((256 - 128 - extra - 5) as u16).to_le_bytes()),
                _ => hostile[entry + 8..entry + 12].copy_from_slice(&u32::MAX.to_le_bytes()),
            }
            write_sparse_fixture(&image, &hostile).unwrap();
            debugfs(&image, &format!("set_inode_field <{number}> generation {generation}"));
            if dirty { debugfs(&image, "feature needs_recovery"); }
            let hostile = std::fs::read(&image).unwrap();
            let raw = ext4plus::Ext4::load(Box::new(hostile.clone())).unwrap();
            let inode = ext4plus::inode::Inode::read(&raw, std::num::NonZeroU32::new(number).unwrap()).unwrap();
            assert!(inode.list_xattrs(&raw).is_err(), "case={case}");
            DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
            assert!(ext4::mount(1, hostile.len() as u64).is_err());
            DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
        }
    }
}

#[test]
fn partial_truncate_zeroes_retained_tail_and_keeps_holes_sparse() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/truncate-test";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    for size in [1, 4095, 4096, 4097] {
        assert_eq!(
            ext4::transaction_probe(&mut mounted, name, 0, &vec![0x5a; 8192]),
            Ok(8192)
        );
        ext4::truncate_probe(&mut mounted, name, size).unwrap();
        let free_after_shrink = ext4::free_bytes(&mounted).unwrap();
        ext4::truncate_probe(&mut mounted, name, 8192).unwrap();
        assert_eq!(ext4::free_bytes(&mounted), Ok(free_after_shrink));
        let mut result = vec![0xa5; 8192];
        read_exact(&mounted, name, &mut result);
        assert!(result[..size as usize].iter().all(|byte| *byte == 0x5a));
        assert!(result[size as usize..].iter().all(|byte| *byte == 0));
    }
    // Shortening a sparse file in a hole must neither allocate nor initialize it.
    ext4::truncate_probe(&mut mounted, name, 0).unwrap();
    let free_empty = ext4::free_bytes(&mounted).unwrap();
    ext4::truncate_probe(&mut mounted, name, 16384).unwrap();
    ext4::truncate_probe(&mut mounted, name, 7001).unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free_empty));
    ext4::transaction_probe(&mut mounted, name, 8000, b"end").unwrap();
    let mut result = vec![0xa5; 8003];
    read_exact(&mounted, name, &mut result);
    assert!(result[..8000].iter().all(|byte| *byte == 0));
    assert_eq!(&result[8000..], b"end");
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-truncate");
}

#[test]
fn partial_truncate_replays_size_and_zeroed_tail_together_at_every_barrier() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/truncate-cut";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, name, 0, &vec![0x5a; 8192]).unwrap();
    ext4::sync(&mut mounted).unwrap();
    let initial = DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.bytes.clone()
    });
    ext4::truncate_probe(&mut mounted, name, 101).unwrap();
    let events = DEVICE.with_borrow(|device| device.events.clone());
    assert!(
        events.contains(&Event::Flush(3)),
        "truncate never committed"
    );
    drop(mounted);
    let mut prefix = initial;
    let mut committed = false;
    for (index, event) in events.iter().enumerate() {
        match event {
            Event::Write(start, bytes) => {
                let start = *start as usize;
                prefix[start..start + bytes.len()].copy_from_slice(bytes);
            }
            Event::Flush(boundary) => {
                committed |= *boundary == 3;
                // A durable prefix ending at this acknowledged flush is the
                // backing of a new mount; no original stage or ring survives.
                DEVICE.with_borrow_mut(|device| {
                    *device = Device {
                        bytes: prefix.clone(),
                        ..Device::default()
                    }
                });
                let mut recovered = ext4::mount(1, prefix.len() as u64).unwrap().0;
                let expected_size = if committed { 101 } else { 8192 };
                assert_eq!(ext4::stat(&recovered, name).unwrap().size, expected_size);
                ext4::truncate_probe(&mut recovered, name, 8192).unwrap();
                let mut result = vec![0xa5; 8192];
                read_exact(&recovered, name, &mut result);
                assert!(result[..101].iter().all(|byte| *byte == 0x5a));
                let expected_tail = if committed { 0 } else { 0x5a };
                assert!(result[101..].iter().all(|byte| *byte == expected_tail));
                ext4::sync(&mut recovered).unwrap();
                ext4::unmount(&recovered).unwrap();
                fsck(&path, &format!("coordinator-truncate-cut-{index}"));
            }
        }
    }
}

fn debugfs(image: &std::path::Path, command: &str) {
    let result = std::process::Command::new("debugfs")
        .args(["-w", "-R", command]).arg(image).output().unwrap();
    assert!(result.status.success(), "debugfs: {}", String::from_utf8_lossy(&result.stderr));
}

fn refresh_fixture_descriptor_checksums(image: &std::path::Path, groups: &[usize]) {
    let mut bytes = std::fs::read(image).unwrap();
    assert_eq!(u16::from_le_bytes(bytes[1278..1280].try_into().unwrap()), 64);
    for &group in groups {
        let start = 4096 + group * 64;
        let mut descriptor: [u8; 64] = bytes[start..start + 64].try_into().unwrap();
        descriptor[30..32].fill(0);
        let mut crc = u32::from_le_bytes(bytes[1648..1652].try_into().unwrap());
        for byte in (group as u32).to_le_bytes().iter().chain(&descriptor) {
            crc ^= u32::from(*byte);
            for _ in 0..8 { crc = (crc >> 1) ^ (0x82f6_3b78u32 & 0u32.wrapping_sub(crc & 1)); }
        }
        bytes[start + 30..start + 32].copy_from_slice(&(crc as u16).to_le_bytes());
    }
    write_sparse_fixture(image, &bytes).unwrap();
}

#[test]
fn e2fsprogs_independently_replays_phipia_truncate_at_every_barrier() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/linux-replay";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, name, 0, &vec![0x69; 12288]).unwrap();
    ext4::sync(&mut mounted).unwrap();
    let mut prefix = DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.bytes.clone()
    });
    ext4::truncate_probe(&mut mounted, name, 101).unwrap();
    let events = DEVICE.with_borrow(|device| device.events.clone());
    drop(mounted);
    let mut committed = false;
    for (index, event) in events.iter().enumerate() {
        match event {
            Event::Write(start, bytes) => {
                let start = *start as usize;
                prefix[start..start + bytes.len()].copy_from_slice(bytes);
            }
            Event::Flush(boundary) => {
                committed |= *boundary == 3;
                let image = path.with_extension(format!("coordinator-linux-replay-{index}.img"));
                std::fs::write(&image, &prefix).unwrap();
                // Replay only on a disposable cut image. No filesystem repair:
                // the independent read-only full fsck below must pass afterward.
                let result = std::process::Command::new("e2fsck")
                    .args(["-E", "journal_only", "-p"]).arg(&image).output().unwrap();
                let log = format!("{}\n{}", String::from_utf8_lossy(&result.stdout),
                    String::from_utf8_lossy(&result.stderr));
                println!("e2fsprogs journal-only replay boundary {index}:\n{log}");
                std::fs::write(image.with_extension("replay.txt"), &log).unwrap();
                assert!(matches!(result.status.code(), Some(0 | 1)), "{log}");
                let mut recovered = mount_fixture(&image);
                assert_eq!(ext4::stat(&recovered, name).unwrap().size, if committed { 101 } else { 12288 });
                let mut bytes = vec![0; if committed { 101 } else { 12288 }];
                read_exact(&recovered, name, &mut bytes);
                assert!(bytes.iter().all(|byte| *byte == 0x69));
                ext4::sync(&mut recovered).unwrap();
                ext4::unmount(&recovered).unwrap();
                fsck(&path, &format!("coordinator-linux-replayed-{index}"));
            }
        }
    }
}

#[test]
fn large_truncate_and_unlink_revoke_multiple_records_and_recover() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/large-free";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    let initial_free = ext4::free_bytes(&mounted).unwrap();
    let chunk = vec![0x45; 64 * 4096];
    for index in 0..9 {
        assert_eq!(ext4::transaction_probe(&mut mounted, name, index * chunk.len() as u64, &chunk), Ok(chunk.len()));
    }
    ext4::sync(&mut mounted).unwrap();
    let initial = DEVICE.with_borrow_mut(|device| { device.events.clear(); device.bytes.clone() });
    ext4::truncate_probe(&mut mounted, name, 101).unwrap();
    let events = DEVICE.with_borrow(|device| device.events.clone());
    let records = events.iter().filter(|event| matches!(event, Event::Write(_, bytes)
        if bytes.len() == 4096 && bytes[..8] == [0xc0, 0x3b, 0x39, 0x98, 0, 0, 0, 5])).count();
    assert_eq!(records, 2);
    assert_eq!(ext4::free_bytes(&mounted), Ok(initial_free - 4096));
    drop(mounted);
    let mut cut = initial;
    for event in events {
        match event {
            Event::Write(start, bytes) => {
                let start = start as usize;
                cut[start..start + bytes.len()].copy_from_slice(&bytes);
            }
            Event::Flush(3) => break,
            Event::Flush(_) => {}
        }
    }
    let mut recovered = mount_bytes(cut.clone());
    assert_eq!(ext4::stat(&recovered, name).unwrap().size, 101);
    ext4::sync(&mut recovered).unwrap();
    fsck(&path, "coordinator-large-truncate-recovery");
    drop(recovered);
    let image = path.with_extension("coordinator-large-linux-replay.img");
    std::fs::write(&image, cut).unwrap();
    let result = std::process::Command::new("e2fsck").args(["-E", "journal_only", "-p"])
        .arg(&image).output().unwrap();
    let log = format!("{}\n{}", String::from_utf8_lossy(&result.stdout), String::from_utf8_lossy(&result.stderr));
    println!("large truncate independent replay:\n{log}");
    assert!(matches!(result.status.code(), Some(0 | 1)), "{log}");
    let mut mounted = mount_fixture(&image);
    assert_eq!(ext4::stat(&mounted, name).unwrap().size, 101);
    for index in 0..9 {
        ext4::transaction_probe(&mut mounted, name, index * chunk.len() as u64, &chunk).unwrap();
    }
    ext4::unlink_file_probe(&mut mounted, name).unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(initial_free));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-large-unlink");
}

#[test]
fn inode_io_survives_file_and_parent_rename_and_original_name_reuse() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    ext4::create_directory_probe(&mut mounted, b"system/open-parent").unwrap();
    ext4::create_file_probe(&mut mounted, b"system/open-parent/file", 0o600).unwrap();
    let inode = ext4::stat(&mounted, b"system/open-parent/file").unwrap().inode;
    ext4::write_inode(&mut mounted, inode, 0, b"original").unwrap();
    ext4::rename_probe(&mut mounted, b"system/open-parent/file", b"system/open-parent/moved").unwrap();
    ext4::create_file_probe(&mut mounted, b"system/open-parent/file", 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, b"system/open-parent/file", 0, b"new").unwrap();
    ext4::rename_probe(&mut mounted, b"system/open-parent", b"data/user/open-parent").unwrap();
    assert_eq!(ext4::append_inode(&mut mounted, inode, b"-append", 16384), Ok((8, 7)));
    let mut content = [0; 15];
    assert_eq!(ext4::pread_inode(&mounted, inode, 0, &mut content), Ok(15));
    assert_eq!(&content, b"original-append");
    assert_eq!(ext4::stat_inode(&mounted, inode).unwrap().size, 15);
    let mut replacement = [0; 3];
    read_exact(&mounted, b"data/user/open-parent/file", &mut replacement);
    assert_eq!(&replacement, b"new");
    ext4::truncate_inode(&mut mounted, inode, 3).unwrap();
    assert_eq!(ext4::stat(&mounted, b"data/user/open-parent/moved").unwrap().size, 3);
    assert_eq!(ext4::pread_inode(&mounted, inode, 0, &mut content), Ok(3));
    assert_eq!(&content[..3], b"ori");
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-inode-io");
}

#[test]
fn directory_snapshot_keeps_original_names_across_namespace_mutations() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    ext4::create_directory_probe(&mut mounted, b"system/snapshot").unwrap();
    let long = format!("system/snapshot/{}", "n".repeat(255));
    for name in [b"system/snapshot/alpha".as_slice(), long.as_bytes(), b"system/snapshot/omega"] {
        ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    }
    let snapshot = ext4::directory_snapshot(&mounted, b"system/snapshot").unwrap();
    let names = |snapshot: &ext4::DirectorySnapshot| {
        (0..).map_while(|index| snapshot.entry(index))
            .map(|entry| entry.name[..entry.name_length as usize].to_vec()).collect::<Vec<_>>()
    };
    let original = names(&snapshot);
    assert_eq!(original.len(), 3);
    assert!(original.contains(&vec![b'n'; 255]));
    ext4::unlink_file_probe(&mut mounted, b"system/snapshot/alpha").unwrap();
    ext4::create_file_probe(&mut mounted, b"system/snapshot/after", 0o600).unwrap();
    ext4::rename_probe(&mut mounted, b"system/snapshot/omega", b"system/snapshot/moved").unwrap();
    assert_eq!(names(&snapshot), original);
    let fresh = ext4::directory_snapshot(&mounted, b"system/snapshot").unwrap();
    assert!(!names(&fresh).contains(&b"alpha".to_vec()));
    assert!(names(&fresh).contains(&b"after".to_vec()));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-directory-snapshot");
    drop(mounted);
    assert_eq!(names(&snapshot), original);
}

#[test]
fn external_xattr_updates_pack_hashes_and_release_storage_without_losing_inline_values() {
    let Some(path) = fixture() else { return };
    let name = b"system/large-attributes";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::set_xattr(&mut mounted, name, b"user.small", Some(b"inline")).unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    // Exhaust the staging budget during allocation: the bitmap reservation
    // must disappear before a larger-budget retry can allocate the same block.
    ext4::set_stage_block_limit(&mut mounted, 1).unwrap();
    let before = DEVICE.with_borrow_mut(|device| { device.events.clear(); device.bytes.clone() });
    assert!(ext4::set_xattr(&mut mounted, name, b"user.a", Some(&[0x61; 301])).is_err());
    DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, before); });
    assert_eq!(ext4::free_bytes(&mounted).unwrap(), free);
    ext4::set_stage_block_limit(&mut mounted, 64).unwrap();
    let mut attributes = vec![("user.a", vec![0x61; 301]), ("user.aa", vec![0x62; 512]),
        ("user.z", vec![0x63; 701]), ("user.é", vec![0x64; 105])];
    for (key, value) in &attributes {
        ext4::set_xattr(&mut mounted, name, key.as_bytes(), Some(value)).unwrap();
        assert_eq!(ext4::free_bytes(&mounted).unwrap(), free - 4096);
    }
    attributes[0].1 = vec![0x7a; 401];
    ext4::set_xattr(&mut mounted, name, b"user.a", Some(&attributes[0].1)).unwrap();
    let before = DEVICE.with_borrow_mut(|device| { device.events.clear(); device.bytes.clone() });
    assert_eq!(ext4::set_xattr(&mut mounted, name, b"user.a", Some(&[0x66; 4096])), Err(Status::Full));
    DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, before); });
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-external-xattrs-written");
    drop(mounted);
    let persisted = DEVICE.with_borrow(|device| device.bytes.clone());
    let image = path.with_extension("coordinator-external-xattrs.img");
    write_sparse_fixture(&image, &persisted).unwrap();
    let mut mounted = mount_bytes(persisted);
    for (index, (key, value)) in attributes.iter().enumerate() {
        let mut output = vec![0; value.len()];
        assert_eq!(ext4::get_xattr(&mounted, name, key.as_bytes(), &mut output), Ok(value.len()));
        assert_eq!(&output, value);
        let exported = path.with_extension(format!("coordinator-external-xattr-{index}.bin"));
        debugfs(&image, &format!("ea_get -f {} /system/large-attributes {key}", exported.display()));
        assert_eq!(&std::fs::read(exported).unwrap(), value); // e2fsprogs validates entry hashes
    }
    let mut small = [0; 6];
    assert_eq!(ext4::get_xattr(&mounted, name, b"user.small", &mut small), Ok(6));
    assert_eq!(&small, b"inline");
    for (key, _) in &attributes {
        ext4::set_xattr(&mut mounted, name, key.as_bytes(), None).unwrap();
    }
    assert_eq!(ext4::free_bytes(&mounted).unwrap(), free);
    assert_eq!(ext4::get_xattr(&mounted, name, b"user.small", &mut small), Ok(6));
    assert_eq!(&small, b"inline");
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-external-xattrs-released");
}

#[test]
fn xattr_packing_uses_available_inode_space_before_reporting_enospc() {
    let Some(path) = fixture() else { return };
    let name = b"system/packed-xattrs";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    // Inline capacity88 bytes: a costs60; b+c cost44+44. The external
    // block can hold a+large (4052 bytes) but not b+c+large (4080 bytes).
    let values = [("user.a", vec![0x41; 40]), ("user.b", vec![0x42; 24]),
        ("user.c", vec![0x43; 24]), ("user.large", vec![0x44; 3968])];
    for (key, value) in &values {
        ext4::set_xattr(&mut mounted, name, key.as_bytes(), Some(value)).unwrap();
    }
    assert_eq!(ext4::free_bytes(&mounted).unwrap(), free - 4096);
    let before = DEVICE.with_borrow_mut(|device| { device.events.clear(); device.bytes.clone() });
    assert_eq!(ext4::set_xattr(&mut mounted, name, b"user.large", Some(&[0x55; 3980])), Err(Status::Full));
    DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, before); });
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-xattrs-packed");
    drop(mounted);
    let image = path.with_extension("coordinator-xattrs-packed.img");
    let bytes = DEVICE.with_borrow(|device| device.bytes.clone());
    write_sparse_fixture(&image, &bytes).unwrap();
    let mut mounted = mount_bytes(bytes);
    for (index, (key, value)) in values.iter().enumerate() {
        let mut read = vec![0; value.len()];
        assert_eq!(ext4::get_xattr(&mounted, name, key.as_bytes(), &mut read), Ok(value.len()));
        assert_eq!(&read, value);
        let exported = path.with_extension(format!("coordinator-packed-xattr-{index}.bin"));
        debugfs(&image, &format!("ea_get -f {} /system/packed-xattrs {key}", exported.display()));
        assert_eq!(&std::fs::read(exported).unwrap(), value);
    }
    ext4::unlink_file_probe(&mut mounted, name).unwrap();
    assert_eq!(ext4::free_bytes(&mounted).unwrap(), free);
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-xattrs-packed-release");
}

#[test]
fn external_xattr_checksums_shared_release_and_final_free() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, b"system/ea-first", 0o600).unwrap();
    ext4::create_file_probe(&mut mounted, b"system/ea-second", 0o600).unwrap();
    ext4::create_file_probe(&mut mounted, b"system/ea-data", 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, b"system/ea-data", 0, b"separate allocation").unwrap();
    let data_inode = ext4::stat(&mounted, b"system/ea-data").unwrap().inode;
    ext4::sync(&mut mounted).unwrap();
    drop(mounted);
    let image = path.with_extension("coordinator-xattr-input.img");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    debugfs(&image, &format!("ea_set /system/ea-first user.large {}", "q".repeat(300)));
    let stat = std::process::Command::new("debugfs").args(["-R", "stat /system/ea-first"])
        .arg(&image).output().unwrap();
    assert!(stat.status.success());
    let stat = String::from_utf8(stat.stdout).unwrap();
    let block: u64 = stat.lines().find_map(|line| line.strip_prefix("File ACL: "))
        .expect("debugfs xattr block").split_whitespace().next().unwrap().parse().unwrap();
    assert_ne!(block, 0, "fixture must have external attributes");
    // Construct two independent inode references to Linux-created attributes.
    debugfs(&image, &format!("set_inode_field /system/ea-second file_acl {block}"));
    debugfs(&image, "set_inode_field /system/ea-second blocks 8");
    let mut initial = std::fs::read(&image).unwrap();
    let start = block as usize * 4096;
    let set_references = |bytes: &mut [u8], references: u32| {
        bytes[start + 4..start + 8].copy_from_slice(&references.to_le_bytes());
        bytes[start + 16..start + 20].fill(0);
        let mut crc = u32::from_le_bytes(bytes[1648..1652].try_into().unwrap());
        for byte in block.to_le_bytes().iter().chain(&bytes[start..start + 4096]) {
            crc ^= u32::from(*byte);
            for _ in 0..8 { crc = (crc >> 1) ^ (0x82f6_3b78u32 & 0u32.wrapping_sub(crc & 1)); }
        }
        bytes[start + 16..start + 20].copy_from_slice(&crc.to_le_bytes());
    };
    set_references(&mut initial, 2);
    for count in [1, 3] {
        let mut hostile = initial.clone();
        set_references(&mut hostile, count);
        DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
        assert!(ext4::mount(1, hostile.len() as u64).is_err());
        DEVICE.with_borrow(|device| {
            assert!(device.events.is_empty());
            assert_eq!(device.bytes, hostile);
        });
    }
    for orphan in [false, true] {
        std::fs::write(&image, &initial).unwrap();
        debugfs(&image, &format!("set_inode_field <{data_inode}> block[5] {block}"));
        if orphan {
            debugfs(&image, &format!("set_inode_field <{data_inode}> links_count 0"));
            debugfs(&image, "unlink /system/ea-data");
            debugfs(&image, &format!("set_super_value last_orphan {data_inode}"));
            debugfs(&image, "feature needs_recovery");
        }
        let hostile = std::fs::read(&image).unwrap();
        DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
        assert!(ext4::mount(1, hostile.len() as u64).is_err());
        DEVICE.with_borrow(|device| {
            assert!(device.events.is_empty());
            assert_eq!(device.bytes, hostile);
        });
    }
    std::fs::write(&image, &initial).unwrap();
    let raw = ext4plus::Ext4::load(Box::new(initial.clone())).unwrap();
    let first_inode = raw.path_to_inode(ext4plus::path::Path::try_from("/system/ea-first").unwrap(),
        ext4plus::FollowSymlinks::All).unwrap().index.get();
    debugfs(&image, &format!("set_inode_field <{first_inode}> links_count 0"));
    debugfs(&image, "unlink /system/ea-first");
    debugfs(&image, &format!("set_super_value last_orphan {first_inode}"));
    debugfs(&image, "feature needs_recovery");
    let mut recovered = mount_bytes(std::fs::read(&image).unwrap());
    let mut attribute = [0; 300];
    assert_eq!(ext4::get_xattr(&recovered, b"system/ea-second", b"user.large", &mut attribute), Ok(300));
    assert_eq!(attribute, [b'q'; 300]);
    ext4::sync(&mut recovered).unwrap();
    ext4::unmount(&recovered).unwrap();
    fsck(&path, "coordinator-xattr-shared-orphan");
    drop(recovered);
    let mut copied = mount_bytes(initial.clone());
    let free_before_copy = ext4::free_bytes(&copied).unwrap();
    ext4::set_xattr(&mut copied, b"system/ea-first", b"user.large", Some(&[b'r'; 301])).unwrap();
    let copy_operations = DEVICE.with_borrow(|device| device.events.clone());
    assert_eq!(ext4::free_bytes(&copied).unwrap(), free_before_copy - 4096);
    assert_eq!(ext4::get_xattr(&copied, b"system/ea-second", b"user.large", &mut attribute), Ok(300));
    assert_eq!(attribute, [b'q'; 300]);
    let mut private = [0; 301];
    assert_eq!(ext4::get_xattr(&copied, b"system/ea-first", b"user.large", &mut private), Ok(301));
    assert_eq!(private, [b'r'; 301]);
    ext4::sync(&mut copied).unwrap();
    ext4::unmount(&copied).unwrap();
    fsck(&path, "coordinator-xattr-shared-copy");
    let copied_disk = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(copied);
    let mut cut = initial.clone();
    for (index, operation) in copy_operations.iter().enumerate() {
        for accept in [false, true] {
            let mut retry = mount_bytes(initial.clone());
            DEVICE.with_borrow_mut(|device| {
                device.fail_event = Some(index);
                device.accept_failed_write = accept;
            });
            assert_eq!(ext4::set_xattr(&mut retry, b"system/ea-first", b"user.large", Some(&[b'r'; 301])), Err(Status::Io));
            DEVICE.with_borrow_mut(|device| device.fail_event = None);
            ext4::set_xattr(&mut retry, b"system/ea-first", b"user.large", Some(&[b'r'; 301])).unwrap();
            ext4::sync(&mut retry).unwrap();
            ext4::unmount(&retry).unwrap();
            DEVICE.with_borrow(|device| assert_eq!(device.bytes, copied_disk, "shared copy event {index}, accept={accept}"));
        }
        if let Event::Write(start, bytes) = operation {
            cut[*start as usize..*start as usize + bytes.len()].copy_from_slice(bytes);
        }
        let mut recovered = mount_bytes(cut.clone());
        let size = ext4::get_xattr(&recovered, b"system/ea-first", b"user.large", &mut []).unwrap();
        assert!(size == 300 || size == 301);
        let mut value = vec![0; size];
        assert_eq!(ext4::get_xattr(&recovered, b"system/ea-first", b"user.large", &mut value), Ok(size));
        assert!(value.iter().all(|byte| *byte == if size == 300 { b'q' } else { b'r' }));
        assert_eq!(ext4::free_bytes(&recovered).unwrap(), free_before_copy - if size == 301 { 4096 } else { 0 });
        assert_eq!(ext4::get_xattr(&recovered, b"system/ea-second", b"user.large", &mut attribute), Ok(300));
        assert_eq!(attribute, [b'q'; 300]);
        ext4::sync(&mut recovered).unwrap();
        ext4::unmount(&recovered).unwrap();
        fsck(&path, &format!("coordinator-xattr-shared-copy-cut-{index}"));
    }
    let mut mounted = mount_bytes(initial.clone());
    fsck(&path, "coordinator-xattr-shared-input");
    ext4::link_file_probe(&mut mounted, b"system/ea-first", b"system/ea-hardlink").unwrap();
    ext4::unlink_file_probe(&mut mounted, b"system/ea-hardlink").unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::unlink_file_probe(&mut mounted, b"system/ea-first").unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    ext4::sync(&mut mounted).unwrap();
    fsck(&path, "coordinator-xattr-shared-release");
    let shared = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let mut mounted = mount_bytes(shared);
    ext4::unlink_file_probe(&mut mounted, b"system/ea-second").unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free + 4096));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-xattr-final-free");
    drop(mounted);
    initial[start + 4095] ^= 1;
    let length = initial.len() as u64;
    DEVICE.with_borrow_mut(|device| *device = Device { bytes: initial, ..Device::default() });
    assert!(ext4::mount(1, length).is_err(), "corrupt external xattr admitted");
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
}

#[test]
fn lazy_group_bitmaps_initialize_in_the_allocation_transaction_and_roll_back() {
    let Some(path) = fixture() else { return };
    let bytes = std::fs::read(&path).unwrap();
    let free = u16::from_le_bytes(bytes[4096 + 0x0e..4096 + 0x10].try_into().unwrap());
    assert!(free > 1);
    let image = path.with_extension("coordinator-lazy-input.img");
    std::fs::write(&image, bytes).unwrap();
    let empty = path.with_extension("coordinator-lazy-empty.bin");
    std::fs::write(&empty, []).unwrap();
    let commands = path.with_extension("coordinator-lazy-fill.commands");
    let mut script = String::from("mkdir /lazy-fill\n");
    for index in 0..free - 1 {
        script.push_str(&format!("write \"{}\" /lazy-fill/entry-{index}\n", empty.display()));
    }
    std::fs::write(&commands, script).unwrap();
    let result = std::process::Command::new("debugfs").args(["-w", "-f"])
        .arg(&commands).arg(&image).output().unwrap();
    assert!(result.status.success(), "{}", String::from_utf8_lossy(&result.stderr));
    // e2fsprogs may materialize empty bitmaps while writing another group.
    // Recreate Linux's lazy state for this entirely unused group, including
    // arbitrary backing bytes which must never be interpreted as allocation.
    let mut bytes = std::fs::read(&image).unwrap();
    assert_eq!(u16::from_le_bytes(bytes[4174..4176].try_into().unwrap()), 1024);
    let flags = u16::from_le_bytes(bytes[4178..4180].try_into().unwrap()) | 3;
    bytes[4178..4180].copy_from_slice(&flags.to_le_bytes());
    for offset in [0, 4] {
        let block = u32::from_le_bytes(bytes[4160 + offset..4164 + offset].try_into().unwrap()) as usize;
        bytes[block * 4096..(block + 1) * 4096].fill(0xa5);
    }
    bytes[4190..4192].fill(0);
    let mut crc = u32::from_le_bytes(bytes[1024 + 0x270..1024 + 0x274].try_into().unwrap());
    for byte in 1u32.to_le_bytes().iter().chain(&bytes[4160..4224]) {
        crc ^= u32::from(*byte);
        for _ in 0..8 { crc = (crc >> 1) ^ (0x82f6_3b78u32 & 0u32.wrapping_sub(crc & 1)); }
    }
    bytes[4190..4192].copy_from_slice(&crc.to_le_bytes()[..2]);
    std::fs::write(&image, bytes).unwrap();
    let mut mounted = mount_fixture(&image);
    let before = DEVICE.with_borrow(|device| device.bytes.clone());
    assert_eq!(u16::from_le_bytes(before[4110..4112].try_into().unwrap()), 0);
    assert_eq!(u16::from_le_bytes(before[4178..4180].try_into().unwrap()) & 3, 3);
    fsck(&path, "coordinator-lazy-initial");
    assert_eq!(ext4::create_file_probe(&mut mounted, b"system/README.TXT", 0o600), Err(Status::Exists));
    ext4::sync(&mut mounted).unwrap();
    DEVICE.with_borrow(|device| assert!(device.bytes == before, "failed create retained lazy allocation"));
    let free_blocks = ext4::free_bytes(&mounted).unwrap();
    ext4::create_file_probe(&mut mounted, b"system/lazy-allocated", 0o600).unwrap();
    let inode = ext4::stat(&mounted, b"system/lazy-allocated").unwrap().inode;
    assert_eq!(inode, 1025);
    DEVICE.with_borrow(|device| assert_eq!(u16::from_le_bytes(device.bytes[4178..4180].try_into().unwrap()) & 3, 2));
    ext4::transaction_probe(&mut mounted, b"system/lazy-allocated", 0, b"initialized").unwrap();
    DEVICE.with_borrow(|device| assert_eq!(u16::from_le_bytes(device.bytes[4178..4180].try_into().unwrap()) & 3, 0));
    let mut content = [0; 11];
    read_exact(&mounted, b"system/lazy-allocated", &mut content);
    assert_eq!(&content, b"initialized");
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-lazy-allocated");
    drop(mounted);
    let disk = DEVICE.with_borrow(|device| device.bytes.clone());
    let mut mounted = mount_bytes(disk);
    ext4::unlink_file_probe(&mut mounted, b"system/lazy-allocated").unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free_blocks));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-lazy-freed");
}

#[test]
fn real_inode_exhaustion_rolls_back_namespace_and_reuses_a_freed_inode() {
    let Some(path) = fixture() else { return };
    let original = std::fs::read(&path).unwrap();
    let free = u32::from_le_bytes(original[1040..1044].try_into().unwrap());
    assert!(free > 2 && free < 8192);
    let image = path.with_extension("coordinator-inode-full-input.img");
    std::fs::write(&image, original).unwrap();
    let empty = path.with_extension("coordinator-inode-empty.bin");
    std::fs::write(&empty, []).unwrap();
    let commands = path.with_extension("coordinator-inode-fill.commands");
    let mut script = String::from("mkdir /inode-full\n");
    for index in 0..free - 1 {
        script.push_str(&format!("write \"{}\" /inode-full/entry-{index}\n", empty.display()));
    }
    std::fs::write(&commands, script).unwrap();
    let result = std::process::Command::new("debugfs").args(["-w", "-f"])
        .arg(&commands).arg(&image).output().unwrap();
    assert!(result.status.success(), "{}", String::from_utf8_lossy(&result.stderr));
    let mut mounted = mount_fixture(&image);
    fsck(&path, "coordinator-inode-full-input");
    let before = DEVICE.with_borrow(|device| device.bytes.clone());
    assert_eq!(u32::from_le_bytes(before[1040..1044].try_into().unwrap()), 0);
    for case in 0..3 {
        let result = match case {
            0 => ext4::create_file_probe(&mut mounted, b"system/no-inode", 0o600),
            1 => ext4::create_directory_probe(&mut mounted, b"system/no-inode"),
            _ => ext4::symlink_probe(&mut mounted, b"system/no-inode", b"missing"),
        };
        assert_eq!(result, Err(Status::Full));
        assert_eq!(ext4::lstat(&mounted, b"system/no-inode"), Err(Status::NotFound));
        ext4::sync(&mut mounted).unwrap();
        DEVICE.with_borrow(|device| assert!(device.bytes == before));
    }
    let recycled = ext4::stat(&mounted, b"inode-full/entry-0").unwrap().inode;
    ext4::unlink_file_probe(&mut mounted, b"inode-full/entry-0").unwrap();
    ext4::create_file_probe(&mut mounted, b"system/reused-inode", 0o640).unwrap();
    assert_eq!(ext4::stat(&mounted, b"system/reused-inode").unwrap().inode, recycled);
    ext4::transaction_probe(&mut mounted, b"system/reused-inode", 0, b"reused").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-inode-reused");
}

#[test]
fn allocation_bitmap_corruption_is_not_rechecksummed_into_a_transaction() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    DEVICE.with_borrow_mut(|device| {
        let block = u32::from_le_bytes(device.bytes[4100..4104].try_into().unwrap());
        device.watched_read_block = Some(u64::from(block));
    });
    ext4::create_file_probe(&mut mounted, b"system/bitmap-write", 0o600).unwrap();
    DEVICE.with_borrow_mut(|device| {
        assert!(device.watched_reads < 16, "inode bitmap scan issued {} storage reads", device.watched_reads);
        device.watched_read_block = None;
    });
    ext4::sync(&mut mounted).unwrap();
    let initial = DEVICE.with_borrow(|device| device.bytes.clone());
    for inode_bitmap in [false, true] {
        let offset = if inode_bitmap { 4 } else { 0 };
        let low = u32::from_le_bytes(initial[4096 + offset..4100 + offset].try_into().unwrap());
        let high = u32::from_le_bytes(initial[4096 + 0x20 + offset..4100 + 0x20 + offset].try_into().unwrap());
        let start = ((u64::from(high) << 32) | u64::from(low)) as usize * 4096;
        DEVICE.with_borrow_mut(|device| device.bytes[start] ^= 1);
        let result = if inode_bitmap {
            ext4::create_file_probe(&mut mounted, b"system/bitmap-new", 0o600)
        } else {
            ext4::transaction_probe(&mut mounted, b"system/bitmap-write", 0, b"new").map(|_| ())
        };
        assert_eq!(result, Err(Status::Invalid), "inode bitmap: {inode_bitmap}");
        // Reload checks reachable inode and data allocations; keep the view
        // absent until the bitmap is restored and sync retries reload.
        assert_public_reads_refused(&mounted);
        DEVICE.with_borrow_mut(|device| device.bytes[start] ^= 1);
        ext4::sync(&mut mounted).unwrap();
        assert_eq!(ext4::stat(&mounted, b"system/bitmap-new"), Err(Status::NotFound));
        assert_eq!(ext4::stat(&mounted, b"system/bitmap-write").unwrap().size, 0);
        DEVICE.with_borrow(|device| assert!(device.bytes == initial));
    }
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-bitmap-refusal");
    drop(mounted);

    // A valid bitmap CRC does not make a contradictory free-inode count safe.
    // This full bitmap used to keep alloc_inode retrying the same group forever.
    let crc32c = |seed: u32, bytes: &[u8]| {
        let mut crc = seed;
        for byte in bytes {
            crc ^= u32::from(*byte);
            for _ in 0..8 { crc = (crc >> 1) ^ (0x82f6_3b78u32 & 0u32.wrapping_sub(crc & 1)); }
        }
        crc
    };
    let mut full = initial;
    let start = u32::from_le_bytes(full[4100..4104].try_into().unwrap()) as usize * 4096;
    let ipg = u32::from_le_bytes(full[1024 + 0x28..1024 + 0x2c].try_into().unwrap()) as usize;
    full[start..start + ipg / 8].fill(0xff);
    let seed = u32::from_le_bytes(full[1024 + 0x270..1024 + 0x274].try_into().unwrap());
    let checksum = crc32c(seed, &full[start..start + ipg / 8]).to_le_bytes();
    full[4096 + 0x1a..4096 + 0x1c].copy_from_slice(&checksum[..2]);
    full[4096 + 0x3a..4096 + 0x3c].copy_from_slice(&checksum[2..]);
    full[4096 + 0x1e..4096 + 0x20].fill(0);
    let group_seed = crc32c(seed, &0u32.to_le_bytes());
    let checksum = crc32c(group_seed, &full[4096..4160]).to_le_bytes();
    full[4096 + 0x1e..4096 + 0x20].copy_from_slice(&checksum[..2]);
    DEVICE.with_borrow_mut(|device| *device = Device { bytes: full.clone(), ..Device::default() });
    assert!(ext4::mount(1, full.len() as u64).is_err());
    DEVICE.with_borrow(|device| {
        assert!(device.events.is_empty());
        assert_eq!(device.bytes, full);
    });
}

#[test]
fn inode_geometry_reserved_allocations_and_free_counter_overflow_are_refused() {
    let Some(path) = fixture() else { return };
    let original = std::fs::read(&path).unwrap();
    let inodes = u32::from_le_bytes(original[1024..1028].try_into().unwrap());
    let image = path.with_extension("inode-geometry.img");
    for (field, offset, value) in [
        ("inodes_count", 0, inodes - 1), ("inodes_count", 0, inodes + 1),
        ("first_ino", 0x54, 10), ("inodes_per_group", 0x28, 32769),
        ("blocks_per_group", 0x20, 32769),
    ] {
        std::fs::write(&image, &original).unwrap();
        debugfs(&image, &format!("set_super_value {field} {value}"));
        let hostile = std::fs::read(&image).unwrap();
        assert_eq!(u32::from_le_bytes(hostile[1024 + offset..1028 + offset].try_into().unwrap()), value);
        DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
        assert!(ext4::mount(1, hostile.len() as u64).is_err(), "admitted {field}={value}");
        DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert!(device.bytes == hostile); });
    }
    let mut mounted = mount_fixture(&path);
    let file = b"system/free-counter-file";
    let directory = b"system/free-counter-dir";
    ext4::create_file_probe(&mut mounted, file, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, file, 0, b"preserved").unwrap();
    ext4::create_directory_probe(&mut mounted, directory).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    debugfs(&image, &format!("set_super_value free_inodes_count {inodes}"));
    let hostile = std::fs::read(&image).unwrap();
    // The complete inode census now refuses contradictory counters before
    // publishing a mount, rather than waiting for the first inode release.
    DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
    assert!(ext4::mount(1, hostile.len() as u64).is_err());
    DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
    std::fs::write(&image, &original).unwrap();
    debugfs(&image, "freei <1>");
    let hostile = std::fs::read(&image).unwrap();
    let bitmap = u32::from_le_bytes(hostile[4100..4104].try_into().unwrap()) as usize * 4096;
    assert_eq!(hostile[bitmap] & 1, 0);
    DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
    assert!(ext4::mount(1, hostile.len() as u64).is_err());
    DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
}

#[test]
fn writable_profile_refuses_foreign_inode_layout_and_inconsistent_cluster_geometry() {
    let Some(path) = fixture() else { return };
    let pristine = std::fs::read(&path).unwrap();
    let bpg = u32::from_le_bytes(pristine[1056..1060].try_into().unwrap());
    let ipg = u32::from_le_bytes(pristine[1064..1068].try_into().unwrap());
    for (offset, width, value) in [
        (0x1c, 4, 3u32), (0x24, 4, bpg / 2), (0x48, 4, 1),
        (0x4c, 4, 0), (0x175, 1, 0), (0x20, 4, bpg + 1), (0x28, 4, ipg + 1),
        (0x15c, 2, 132), (0x15c, 2, 3), (0x15e, 2, 132), (0x15e, 2, 3),
    ] {
        let mut hostile = pristine.clone();
        hostile[1024 + offset..1024 + offset + width].copy_from_slice(&value.to_le_bytes()[..width]);
        let mut crc = u32::MAX;
        for byte in &hostile[1024..2044] {
            crc ^= u32::from(*byte);
            for _ in 0..8 { crc = (crc >> 1) ^ (0x82f6_3b78u32 & 0u32.wrapping_sub(crc & 1)); }
        }
        hostile[2044..2048].copy_from_slice(&crc.to_le_bytes());
        DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
        assert!(ext4::mount(1, hostile.len() as u64).is_err(), "accepted superblock field {offset:x}={value}");
        DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
    }
}

#[test]
fn writable_inode_flags_refuse_unimplemented_data_and_namespace_semantics() {
    let Some(path) = fixture() else { return };
    let name = b"system/flag-profile";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    let number = ext4::stat(&mounted, name).unwrap().inode;
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    let pristine = DEVICE.with_borrow(|device| device.bytes.clone());
    let image = path.with_extension("coordinator-inode-flag-profile.img");
    for flag in [1u32, 2, 4, 0x100, 0x800, 0x1000, 0x2000, 0x4000,
        0x100000, 0x200000, 0x10000000, 0x20000000, 0x40000000, 0x80000000] {
        for orphan in [false, true] {
            write_sparse_fixture(&image, &pristine).unwrap();
            debugfs(&image, &format!("set_inode_field <{number}> flags {}", 0x80000 | flag));
            if orphan {
                debugfs(&image, &format!("set_inode_field <{number}> links_count 0"));
                debugfs(&image, "unlink /system/flag-profile");
                debugfs(&image, &format!("set_super_value last_orphan {number}"));
                debugfs(&image, "feature needs_recovery");
            }
            let hostile = std::fs::read(&image).unwrap();
            let raw = ext4plus::Ext4::load(Box::new(hostile.clone())).unwrap();
            let inode = ext4plus::inode::Inode::read(&raw, std::num::NonZeroU32::new(number as u32).unwrap()).unwrap();
            assert_eq!(inode.flags().bits(), 0x80000 | flag);
            DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
            assert!(ext4::mount(1, hostile.len() as u64).is_err(), "accepted inode flag {flag:x}, orphan={orphan}");
            DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
        }
    }
    write_sparse_fixture(&image, &pristine).unwrap();
    debugfs(&image, "set_inode_field /system/flag-profile flags 0x800c8");
    let mut mounted = mount_fixture(&image);
    ext4::transaction_probe(&mut mounted, name, 4093, b"sync-nodump-noatime").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-inode-benign-flags-valid");
}

#[test]
fn inode_bitmap_census_checks_group_totals_directory_counts_and_unused_tail() {
    let Some(path) = fixture() else { return };
    let pristine = std::fs::read(&path).unwrap();
    let image = path.with_extension("coordinator-inode-census.img");
    let per_group = u32::from_le_bytes(pristine[1064..1068].try_into().unwrap());
    let free = u16::from_le_bytes(pristine[4110..4112].try_into().unwrap()) as u32;
    let dirs = u16::from_le_bytes(pristine[4112..4114].try_into().unwrap()) as u32;
    let total = u32::from_le_bytes(pristine[1040..1044].try_into().unwrap());
    let lazy = (0..pristine.len() / (8192 * 4096)).find(|group| {
        let descriptor = 4096 + group * 64;
        u16::from_le_bytes(pristine[descriptor + 18..descriptor + 20].try_into().unwrap()) & 1 != 0
    }).expect("fixture has a lazy inode group");
    for case in 0..9 {
        for dirty in [false, true] {
            write_sparse_fixture(&image, &pristine).unwrap();
            match case {
                0 => debugfs(&image, &format!("set_bg 0 free_inodes_count {}", free - 1)),
                1 => debugfs(&image, &format!("set_bg 0 free_inodes_count {}", free + 1)),
                2 => debugfs(&image, &format!("set_bg 0 used_dirs_count {}", dirs - 1)),
                3 => debugfs(&image, &format!("set_bg 0 used_dirs_count {}", dirs + 1)),
                4 => debugfs(&image, &format!("set_bg 0 itable_unused {per_group}")),
                5 => debugfs(&image, &format!("set_super_value free_inodes_count {}", total - 1)),
                6 => debugfs(&image, &format!("set_bg {lazy} free_inodes_count {}", per_group - 1)),
                7 => debugfs(&image, &format!("set_bg {lazy} itable_unused {}", per_group - 1)),
                _ => {
                    let bitmap = u32::from_le_bytes(pristine[4100..4104].try_into().unwrap()) as usize * 4096;
                    assert!(per_group < 32768);
                    let mut hostile = pristine.clone();
                    // Padding is outside the bitmap checksum's logical span.
                    hostile[bitmap + 4095] &= 0x7f;
                    write_sparse_fixture(&image, &hostile).unwrap();
                }
            }
            if dirty { debugfs(&image, "feature needs_recovery"); }
            refresh_fixture_descriptor_checksums(&image, &[0, lazy]);
            let hostile = std::fs::read(&image).unwrap();
            assert_ne!(hostile, pristine, "debugfs must create a real counter fault");
            // Refusal must come from the census, not stale descriptor CRCs.
            ext4plus::Ext4::load(Box::new(hostile.clone())).unwrap();
            DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
            assert!(ext4::mount(1, hostile.len() as u64).is_err(), "accepted census case {case}, dirty={dirty}");
            DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
        }
    }
    let mut mounted = mount_bytes(pristine);
    ext4::create_directory_probe(&mut mounted, b"system/census-dir").unwrap();
    ext4::create_file_probe(&mut mounted, b"system/census-dir/file", 0o600).unwrap();
    ext4::link_file_probe(&mut mounted, b"system/census-dir/file", b"system/census-dir/link").unwrap();
    ext4::unlink_file_probe(&mut mounted, b"system/census-dir/file").unwrap();
    ext4::unlink_file_probe(&mut mounted, b"system/census-dir/link").unwrap();
    ext4::remove_directory_probe(&mut mounted, b"system/census-dir").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-inode-census-valid");
}

#[test]
fn allocated_inodes_missing_from_namespace_and_orphan_chain_refuse_admission() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, b"system/unreachable", 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, b"system/unreachable", 0, b"still allocated").unwrap();
    let inode = ext4::stat(&mounted, b"system/unreachable").unwrap().inode;
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-reachable-inode-before");
    drop(mounted);
    let pristine = DEVICE.with_borrow(|device| device.bytes.clone());
    let image = path.with_extension("coordinator-unreachable-inode.img");
    for linked in [false, true] {
        for dirty in [false, true] {
            std::fs::write(&image, &pristine).unwrap();
            debugfs(&image, "unlink /system/unreachable");
            debugfs(&image, &format!("set_inode_field <{inode}> links_count {}", u8::from(linked)));
            if dirty { debugfs(&image, "feature needs_recovery"); }
            let hostile = std::fs::read(&image).unwrap();
            DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
            assert!(ext4::mount(1, hostile.len() as u64).is_err());
            DEVICE.with_borrow(|device| {
                assert!(device.events.is_empty());
                assert_eq!(device.bytes, hostile);
            });
        }
    }
    // A real open-unlinked inode remains accounted for by its orphan record.
    let mut mounted = mount_bytes(pristine);
    ext4::unlink_file_guarded(&mut mounted, b"system/unreachable", &[inode]).unwrap();
    ext4::sync_with_open_inodes(&mut mounted, &[inode]).unwrap();
    assert_eq!(ext4::stat_inode(&mounted, inode).unwrap().links, 0);
    let dirty = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let recovered = mount_bytes(dirty);
    assert_eq!(ext4::stat_inode(&recovered, inode), Err(Status::NotFound));
    ext4::unmount(&recovered).unwrap();
    fsck(&path, "coordinator-reachable-orphan-recovery");
}

#[test]
fn block_bitmap_census_refuses_unowned_allocations_fixed_frees_and_false_counters() {
    let Some(path) = fixture() else { return };
    let pristine = std::fs::read(&path).unwrap();
    let image = path.with_extension("coordinator-block-census.img");
    let per_group = u32::from_le_bytes(pristine[1056..1060].try_into().unwrap()) as usize;
    let bitmap = u32::from_le_bytes(pristine[4096..4100].try_into().unwrap()) as usize * 4096;
    let free = u16::from_le_bytes(pristine[4108..4110].try_into().unwrap()) as u32;
    let total = u32::from_le_bytes(pristine[1036..1040].try_into().unwrap());
    let first_free = (0..per_group).find(|bit| pristine[bitmap + bit / 8] & (1 << (bit % 8)) == 0).unwrap();
    let lazy = (0..pristine.len().div_ceil(per_group * 4096)).find(|group| {
        let descriptor = 4096 + group * 64;
        u16::from_le_bytes(pristine[descriptor + 18..descriptor + 20].try_into().unwrap()) & 2 != 0
    }).expect("fixture has a lazy block group");
    let lazy_free = u16::from_le_bytes(pristine[4096 + lazy * 64 + 12..4096 + lazy * 64 + 14].try_into().unwrap());
    for case in 0..8 {
        for dirty in [false, true] {
            write_sparse_fixture(&image, &pristine).unwrap();
            match case {
                0 => debugfs(&image, &format!("set_bg 0 free_blocks_count {}", free - 1)),
                1 => debugfs(&image, &format!("set_bg 0 free_blocks_count {}", free + 1)),
                2 => debugfs(&image, &format!("set_super_value free_blocks_count {}", total - 1)),
                3 => debugfs(&image, &format!("set_bg {lazy} free_blocks_count {}", lazy_free - 1)),
                4 => debugfs(&image, "freeb 1"), // descriptor table still owns it
                5 => {
                    debugfs(&image, &format!("setb {first_free}"));
                    // Make both counters agree with the bitmap: only the
                    // complete ownership comparison can detect this leak.
                    debugfs(&image, &format!("set_bg 0 free_blocks_count {}", free - 1));
                    debugfs(&image, &format!("set_super_value free_blocks_count {}", total - 1));
                }
                6 => {
                    assert!(per_group < 32768);
                    let mut hostile = pristine.clone();
                    hostile[bitmap + 4095] &= 0x7f;
                    write_sparse_fixture(&image, &hostile).unwrap();
                }
                _ => {
                    let other = u32::from_le_bytes(pristine[4096 + lazy * 64..4100 + lazy * 64].try_into().unwrap());
                    debugfs(&image, &format!("set_bg 0 block_bitmap {other}"));
                }
            }
            if dirty { debugfs(&image, "feature needs_recovery"); }
            refresh_fixture_descriptor_checksums(&image, &[0, lazy]);
            let hostile = std::fs::read(&image).unwrap();
            ext4plus::Ext4::load(Box::new(hostile.clone())).unwrap();
            DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
            assert!(ext4::mount(1, hostile.len() as u64).is_err(), "accepted block census case {case}, dirty={dirty}");
            DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
        }
    }
    let mounted = mount_bytes(pristine.clone());
    ext4::unmount(&mounted).unwrap();
    DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, pristine); });
    fsck(&path, "coordinator-block-census-valid-lazy");
}

#[test]
fn reserved_inodes_cannot_hide_storage_outside_the_ownership_census() {
    let Some(path) = fixture() else { return };
    let pristine = std::fs::read(&path).unwrap();
    let image = path.with_extension("coordinator-reserved-storage.img");
    let table = u32::from_le_bytes(pristine[4104..4108].try_into().unwrap()) as usize * 4096;
    let root_block = u32::from_le_bytes(pristine[table + 256 + 0x3c..table + 256 + 0x40].try_into().unwrap());
    assert_ne!(root_block, 0);
    for (index, field, value) in [
        (1, "block[0]", u64::from(root_block)),
        (3, "block[TIND]", u64::from(root_block)),
        (4, "file_acl", u64::from(root_block)),
        (5, "size", 4096), (6, "blocks", 8),
        (7, "mode", 0o100600), (9, "links_count", 1), (10, "flags", 0x80000),
    ] {
        for dirty in [false, true] {
            write_sparse_fixture(&image, &pristine).unwrap();
            debugfs(&image, &format!("set_inode_field <{index}> {field} {value}"));
            if dirty { debugfs(&image, "feature needs_recovery"); }
            let hostile = std::fs::read(&image).unwrap();
            assert_ne!(&hostile[table + (index - 1) * 256..table + index * 256],
                &pristine[table + (index - 1) * 256..table + index * 256]);
            ext4plus::Ext4::load(Box::new(hostile.clone())).unwrap();
            DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
            assert!(ext4::mount(1, hostile.len() as u64).is_err(), "accepted reserved inode {index} {field}, dirty={dirty}");
            DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
        }
    }
    // Inactive reserved bodies can retain checksummed timestamps. They must
    // remain byte-identical across admission and ordinary file publication.
    write_sparse_fixture(&image, &pristine).unwrap();
    debugfs(&image, "set_inode_field <3> atime 1780000000");
    let valid = std::fs::read(&image).unwrap();
    assert_ne!(&valid[table + 512..table + 768], &pristine[table + 512..table + 768]);
    let mut mounted = mount_bytes(valid.clone());
    ext4::create_file_probe(&mut mounted, b"system/reserved-control", 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, b"system/reserved-control", 0, b"control").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    DEVICE.with_borrow(|device| assert_eq!(&device.bytes[table..table + 256], &valid[table..table + 256]));
    DEVICE.with_borrow(|device| assert_eq!(&device.bytes[table + 512..table + 768], &valid[table + 512..table + 768]));
    fsck(&path, "coordinator-reserved-empty-valid");
    let mut hostile = valid;
    hostile[table + 512 + 0x7c] ^= 1;
    DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
    assert!(ext4::mount(1, hostile.len() as u64).is_err());
    DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
}

#[test]
fn checksummed_extents_require_allocated_data_even_beyond_eof_or_on_orphan_chain() {
    let Some(path) = fixture() else { return };
    let name = b"system/allocation-map";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, name, 0, &vec![0x64; 8192]).unwrap();
    for block in 0..5 { ext4::transaction_probe(&mut mounted, name, (3 + block * 2) * 4096, b"sparse").unwrap(); }
    let number = ext4::stat(&mounted, name).unwrap().inode;
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-data-allocation-before");
    drop(mounted);
    let pristine = DEVICE.with_borrow(|device| device.bytes.clone());
    let filesystem = ext4plus::Ext4::load(Box::new(pristine.clone())).unwrap();
    let file = filesystem.open("/system/allocation-map").unwrap();
    let first = file.filesystem_block_at_offset(0).unwrap().unwrap();
    let second = file.filesystem_block_at_offset(4096).unwrap().unwrap();
    let ipg = u32::from_le_bytes(pristine[1064..1068].try_into().unwrap());
    let descriptor = 4096 + ((number as u32 - 1) / ipg) as usize * 64;
    let table = u32::from_le_bytes(pristine[descriptor + 8..descriptor + 12].try_into().unwrap());
    let inode_start = table as usize * 4096 + ((number as u32 - 1) % ipg) as usize * 256;
    assert_eq!(u16::from_le_bytes(pristine[inode_start + 0x2e..inode_start + 0x30].try_into().unwrap()), 1);
    let leaf = u64::from(u32::from_le_bytes(pristine[inode_start + 0x38..inode_start + 0x3c].try_into().unwrap()));
    let mut bitmap = filesystem.block_allocation_snapshot();
    assert!(bitmap.range_is_allocated(first, 1).unwrap());
    assert!(bitmap.range_is_allocated(second, 1).unwrap());
    assert!(!bitmap.range_is_allocated(first, 0).unwrap());
    assert!(!bitmap.range_is_allocated(u64::MAX, 2).unwrap());
    assert!(!bitmap.range_is_allocated((pristine.len() / 4096) as u64, 1).unwrap());
    let image = path.with_extension("coordinator-unallocated-extent.img");
    for target in [second, leaf] {
        for zero_size in [false, true] {
            for orphan in [false, true] {
                std::fs::write(&image, &pristine).unwrap();
                // debugfs recomputes the bitmap and descriptor checksums. The
                // inode still points at a data/extent-node block marked available.
                debugfs(&image, &format!("freeb {target}"));
                if zero_size { debugfs(&image, &format!("set_inode_field <{number}> size 0")); }
                if orphan {
                    debugfs(&image, &format!("set_inode_field <{number}> links_count 0"));
                    debugfs(&image, "unlink /system/allocation-map");
                    debugfs(&image, &format!("set_super_value last_orphan {number}"));
                    debugfs(&image, "feature needs_recovery");
                }
                let hostile = std::fs::read(&image).unwrap();
                let filesystem = ext4plus::Ext4::load(Box::new(hostile.clone())).unwrap();
                let mut bitmap = filesystem.block_allocation_snapshot();
                assert!(bitmap.range_is_allocated(first, 1).unwrap());
                assert!(!bitmap.range_is_allocated(target, 1).unwrap());
                let inode = ext4plus::inode::Inode::read(&filesystem, std::num::NonZeroU32::new(number as u32).unwrap()).unwrap();
                assert!(bitmap.validate_inode_extents(&inode).is_err());
                let size = hostile.len() as u64;
                DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), watched_read_block: Some(leaf), ..Device::default() });
                assert!(ext4::mount(1, size).is_err());
                DEVICE.with_borrow(|device| {
                    assert!(device.events.is_empty());
                    assert_eq!(device.bytes, hostile);
                    if target == leaf && !orphan { assert_eq!(device.watched_reads, 0); }
                });
            }
        }
    }
    let mut mounted = mount_bytes(pristine);
    ext4::transaction_probe(&mut mounted, name, 4093, b"allocated").unwrap();
    ext4::truncate_probe(&mut mounted, name, 17).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-data-allocation-valid");
}

#[test]
fn duplicate_extent_release_refuses_and_rolls_back_bitmap_and_namespace() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/duplicate-extents";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, name, 0, b"first").unwrap();
    ext4::transaction_probe(&mut mounted, name, 8192, b"second").unwrap();
    let inode = ext4::stat(&mounted, name).unwrap().inode as u32;
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-duplicate-before");
    drop(mounted);
    let pristine = DEVICE.with_borrow(|device| device.bytes.clone());
    let mut corrupt = pristine.clone();
    let ipg = u32::from_le_bytes(corrupt[1064..1068].try_into().unwrap());
    let group = (inode - 1) / ipg;
    let descriptor = 4096 + group as usize * 64;
    let table = u32::from_le_bytes(corrupt[descriptor + 8..descriptor + 12].try_into().unwrap());
    let start = table as usize * 4096 + ((inode - 1) % ipg) as usize * 256;
    assert_eq!(u16::from_le_bytes(corrupt[start + 0x2a..start + 0x2c].try_into().unwrap()), 2);
    assert_eq!(u16::from_le_bytes(corrupt[start + 0x2e..start + 0x30].try_into().unwrap()), 0);
    // Two different logical extents now reference the same physical block.
    // Keep the inode CRC valid to exercise release validation, not CRC refusal.
    corrupt.copy_within(start + 0x3a..start + 0x40, start + 0x46);
    corrupt[start + 0x7c..start + 0x7e].fill(0);
    corrupt[start + 0x82..start + 0x84].fill(0);
    let mut crc = u32::from_le_bytes(corrupt[1648..1652].try_into().unwrap());
    for byte in inode.to_le_bytes().iter()
        .chain(&corrupt[start + 0x64..start + 0x68])
        .chain(&corrupt[start..start + 256]) {
        crc ^= u32::from(*byte);
        for _ in 0..8 { crc = (crc >> 1) ^ (0x82f6_3b78u32 & 0u32.wrapping_sub(crc & 1)); }
    }
    corrupt[start + 0x7c..start + 0x7e].copy_from_slice(&crc.to_le_bytes()[..2]);
    corrupt[start + 0x82..start + 0x84].copy_from_slice(&crc.to_le_bytes()[2..]);
    DEVICE.with_borrow_mut(|device| *device = Device { bytes: corrupt.clone(), ..Device::default() });
    assert!(ext4::mount(1, corrupt.len() as u64).is_err());
    DEVICE.with_borrow(|device| assert!(device.events.is_empty()));
    // Admission now refuses duplicate ownership. Retain the upstream release
    // regression too: even without admission its partial free must roll back.
    for unlink in [false, true] {
        let stage = std::rc::Rc::new(ext4plus::JournalMutationStage::new(
            Box::new(corrupt.clone()), corrupt.len() as u64).unwrap());
        let raw = ext4plus::Ext4::load_with_writer(Box::new(stage.clone()), Some(Box::new(stage.clone()))).unwrap();
        if unlink {
            let parent = raw.path_to_inode(ext4plus::path::Path::try_from("/system").unwrap(),
                ext4plus::FollowSymlinks::All).unwrap();
            let mut directory = ext4plus::dir::Dir::open_inode(&raw, parent).unwrap();
            let name = ext4plus::DirEntryName::try_from("duplicate-extents").unwrap();
            let inode = directory.get_entry(name).unwrap();
            assert!(directory.unlink(name, inode).is_err());
        } else { assert!(raw.open("/system/duplicate-extents").unwrap().truncate(0).is_err()); }
        drop(raw);
        stage.rollback();
        assert!(stage.is_empty());
        let restored = ext4plus::Ext4::load(Box::new(stage.clone())).unwrap();
        assert_eq!(restored.metadata("/system/duplicate-extents").unwrap().size_in_bytes, 8198);
        DEVICE.with_borrow(|device| assert!(device.bytes == corrupt, "partial free escaped rollback"));
    }
    // Valid allocation/free still restores a Linux-clean image.
    let mut mounted = mount_bytes(pristine);
    ext4::unlink_file_probe(&mut mounted, name).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-duplicate-valid-free");
}

#[test]
fn legacy_indirect_mapping_ownership_is_checked_before_writes_or_recovery() {
    let Some(path) = fixture() else { return };
    let image = path.with_extension("coordinator-legacy-map.img");
    let input = path.with_extension("coordinator-legacy-map.bin");
    let mut payload = vec![0; 1038 * 4096 + 11];
    payload[..6].copy_from_slice(b"direct");
    payload[12 * 4096..12 * 4096 + 6].copy_from_slice(b"single");
    payload[1038 * 4096..].copy_from_slice(b"double-tail");
    std::fs::write(&input, &payload).unwrap();
    std::fs::copy(&path, &image).unwrap();
    // Create only this inode with Linux's legacy block mapping; restore the
    // filesystem feature before any Phipia admission or e2fsck validation.
    debugfs(&image, "feature ^extents");
    debugfs(&image, &format!("write \"{}\" /system/legacy-map", input.display()));
    debugfs(&image, "feature extents");
    let pristine = std::fs::read(&image).unwrap();
    let mut mounted = mount_bytes(pristine.clone());
    fsck(&path, "coordinator-legacy-map-before");
    let inode = ext4::stat(&mounted, b"system/legacy-map").unwrap().inode;
    let mut read = vec![0xa5; payload.len()];
    read_exact(&mounted, b"system/legacy-map", &mut read);
    assert_eq!(read, payload);
    let raw = ext4plus::Ext4::load(Box::new(pristine.clone())).unwrap();
    let node = ext4plus::inode::Inode::read(&raw, std::num::NonZeroU32::new(inode as u32).unwrap()).unwrap();
    assert!(!node.flags().contains(ext4plus::inode::InodeFlags::EXTENTS));
    let ipg = u32::from_le_bytes(pristine[1064..1068].try_into().unwrap());
    let descriptor = 4096 + ((inode as u32 - 1) / ipg) as usize * 64;
    let table = u32::from_le_bytes(pristine[descriptor + 8..descriptor + 12].try_into().unwrap());
    let inode_start = table as usize * 4096 + ((inode as u32 - 1) % ipg) as usize * 256;
    let single = u32::from_le_bytes(pristine[inode_start + 0x58..inode_start + 0x5c].try_into().unwrap());
    let double = u32::from_le_bytes(pristine[inode_start + 0x5c..inode_start + 0x60].try_into().unwrap());
    assert_ne!(single, 0);
    assert_ne!(double, 0);
    // Ordinary mutation still uses the existing block-map writer/coordinator.
    let previous_time = MUTATION_TIME.with(|time| time.replace(1_780_000_500));
    assert_eq!(ext4::transaction_probe(&mut mounted, b"system/legacy-map", 0, b"DIRECT"), Ok(6));
    let snapshot = ext4plus::Ext4::load(Box::new(DEVICE.with_borrow(|device| device.bytes.clone()))).unwrap();
    assert_eq!(snapshot.metadata("/system/legacy-map").unwrap().mtime.as_secs(), 1_780_000_500);
    MUTATION_TIME.with(|time| time.set(previous_time));
    assert_eq!(ext4::transaction_probe(&mut mounted, b"system/legacy-map", 4093, b"legacy-write"), Ok(12));
    ext4::truncate_probe(&mut mounted, b"system/legacy-map", 17).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-legacy-map-truncated");
    drop(mounted);
    for case in 0..4 {
        for orphan in [false, true] {
            std::fs::write(&image, &pristine).unwrap();
            match case {
                0 => debugfs(&image, &format!("freeb {single}")),
                1 => debugfs(&image, &format!("set_inode_field <{inode}> block[0] {single}")),
                2 => {
                    debugfs(&image, &format!("set_inode_field <{inode}> block[DIND] {single}"));
                    let changed = std::fs::read(&image).unwrap();
                    assert_eq!(u32::from_le_bytes(changed[inode_start + 0x5c..inode_start + 0x60].try_into().unwrap()), single,
                        "debugfs must replace the double-indirect root");
                }
                _ => {
                    // Legacy indirect records have no CRC. An allocated
                    // self-reference must be caught before recursive reads.
                    let mut bytes = pristine.clone();
                    bytes[double as usize * 4096..double as usize * 4096 + 4].copy_from_slice(&double.to_le_bytes());
                    std::fs::write(&image, bytes).unwrap();
                }
            }
            debugfs(&image, &format!("set_inode_field <{inode}> size 0"));
            if orphan {
                debugfs(&image, &format!("set_inode_field <{inode}> links_count 0"));
                debugfs(&image, "unlink /system/legacy-map");
                debugfs(&image, &format!("set_super_value last_orphan {inode}"));
                debugfs(&image, "feature needs_recovery");
            }
            let hostile = std::fs::read(&image).unwrap();
            DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
            assert!(ext4::mount(1, hostile.len() as u64).is_err(), "legacy case {case}, orphan={orphan}");
            DEVICE.with_borrow(|device| {
                assert!(device.events.is_empty());
                assert_eq!(device.bytes, hostile);
            });
        }
    }
}

#[test]
fn extent_data_and_nodes_cannot_be_shared_between_live_or_orphan_inodes() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, b"system/extent-owner", 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, b"system/extent-owner", 0, &vec![0x38; 12288]).unwrap();
    for block in 0..5 { ext4::transaction_probe(&mut mounted, b"system/extent-owner", (4 + block * 2) * 4096, b"fragment").unwrap(); }
    ext4::link_file_probe(&mut mounted, b"system/extent-owner", b"system/extent-owner-link").unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/directory-owner").unwrap();
    ext4::create_file_probe(&mut mounted, b"system/extent-thief", 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, b"system/extent-thief", 0, b"mine").unwrap();
    let owner_number = ext4::stat(&mounted, b"system/extent-owner").unwrap().inode as u32;
    let thief_number = ext4::stat(&mounted, b"system/extent-thief").unwrap().inode;
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-exclusive-extents-before");
    drop(mounted);
    let pristine = DEVICE.with_borrow(|device| device.bytes.clone());
    let raw = ext4plus::Ext4::load(Box::new(pristine.clone())).unwrap();
    let owner = raw.open("/system/extent-owner").unwrap();
    let data_block = owner.filesystem_block_at_offset(4096).unwrap().unwrap();
    let directory_inode = raw.path_to_inode(ext4plus::path::Path::try_from("/system/directory-owner").unwrap(),
        ext4plus::FollowSymlinks::All).unwrap();
    let directory = ext4plus::file::File::open_inode(&raw, directory_inode).unwrap();
    let directory_block = directory.filesystem_block_at_offset(0).unwrap().unwrap();
    let ipg = u32::from_le_bytes(pristine[1064..1068].try_into().unwrap());
    let descriptor = 4096 + ((owner_number - 1) / ipg) as usize * 64;
    let table = u32::from_le_bytes(pristine[descriptor + 8..descriptor + 12].try_into().unwrap());
    let inode_start = table as usize * 4096 + ((owner_number - 1) % ipg) as usize * 256;
    assert_eq!(u16::from_le_bytes(pristine[inode_start + 0x2e..inode_start + 0x30].try_into().unwrap()), 1);
    let leaf = u64::from(u32::from_le_bytes(pristine[inode_start + 0x38..inode_start + 0x3c].try_into().unwrap()));
    let image = path.with_extension("coordinator-shared-extent.img");
    let journal_block = ext4plus::load_journal_inode_map(&raw).unwrap().physical_blocks()[1];
    for target in [data_block, directory_block, leaf, journal_block] {
        for zero_size in [false, true] {
            for orphan in [false, true] {
                std::fs::write(&image, &pristine).unwrap();
                debugfs(&image, &format!("set_inode_field <{thief_number}> block[5] {target}"));
                if zero_size { debugfs(&image, &format!("set_inode_field <{thief_number}> size 0")); }
                if orphan {
                    debugfs(&image, &format!("set_inode_field <{thief_number}> links_count 0"));
                    debugfs(&image, "unlink /system/extent-thief");
                    debugfs(&image, &format!("set_super_value last_orphan {thief_number}"));
                    debugfs(&image, "feature needs_recovery");
                }
                let hostile = std::fs::read(&image).unwrap();
                let raw = ext4plus::Ext4::load(Box::new(hostile.clone())).unwrap();
                let owner_path = if target == directory_block { "/system/directory-owner" } else { "/system/extent-owner" };
                let owner = raw.path_to_inode(ext4plus::path::Path::try_from(owner_path).unwrap(), ext4plus::FollowSymlinks::All).unwrap();
                let thief = ext4plus::inode::Inode::read(&raw, std::num::NonZeroU32::new(thief_number as u32).unwrap()).unwrap();
                for reverse in [false, true] {
                    let mut claims = raw.block_allocation_snapshot();
                    if target == journal_block {
                        if reverse {
                            claims.validate_inode_extents(&thief).unwrap();
                            assert!(claims.validate_internal_journal().is_err());
                        } else {
                            claims.validate_internal_journal().unwrap();
                            claims.validate_internal_journal().unwrap();
                            assert!(claims.validate_inode_extents(&thief).is_err());
                        }
                        assert!(claims.validate_inode_extents(&thief).is_err());
                        continue;
                    }
                    let (first, second) = if reverse { (&thief, &owner) } else { (&owner, &thief) };
                    claims.validate_inode_extents(first).unwrap();
                    claims.validate_inode_extents(first).unwrap(); // hard-link identity
                    assert!(claims.validate_inode_extents(second).is_err());
                    assert!(claims.validate_inode_extents(first).is_err()); // poisoned partial pass
                }
                DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
                assert!(ext4::mount(1, hostile.len() as u64).is_err());
                DEVICE.with_borrow(|device| {
                    assert!(device.events.is_empty());
                    assert_eq!(device.bytes, hostile);
                });
            }
        }
    }
    let mut mounted = mount_bytes(pristine);
    ext4::transaction_probe(&mut mounted, b"system/extent-owner-link", 4093, b"valid-owner").unwrap();
    ext4::unlink_file_probe(&mut mounted, b"system/extent-thief").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-exclusive-extents-valid");
}

#[test]
fn legacy_zero_link_orphan_recovery_retries_every_storage_failure() {
    for case in 0..4 { orphan_recovery_failure_case(case); }
}

#[test]
fn checksummed_malformed_extent_trees_are_refused_before_traversal_or_mutation() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/extent-cycle";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    for block in 0..6 { ext4::transaction_probe(&mut mounted, name, block * 8192, b"extent").unwrap(); }
    let number = ext4::stat(&mounted, name).unwrap().inode as u32;
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-extent-cycle-before");
    drop(mounted);
    let mut bytes = DEVICE.with_borrow(|device| device.bytes.clone());
    let ipg = u32::from_le_bytes(bytes[1064..1068].try_into().unwrap());
    let descriptor = 4096 + ((number - 1) / ipg) as usize * 64;
    let table = u64::from(u32::from_le_bytes(bytes[descriptor + 8..descriptor + 12].try_into().unwrap()))
        | (u64::from(u32::from_le_bytes(bytes[descriptor + 40..descriptor + 44].try_into().unwrap())) << 32);
    let inode_start = table as usize * 4096 + ((number - 1) % ipg) as usize * 256;
    assert_eq!(u16::from_le_bytes(bytes[inode_start + 0x2e..inode_start + 0x30].try_into().unwrap()), 1);
    let leaf = u64::from(u32::from_le_bytes(bytes[inode_start + 0x38..inode_start + 0x3c].try_into().unwrap()))
        | (u64::from(u16::from_le_bytes(bytes[inode_start + 0x3c..inode_start + 0x3e].try_into().unwrap())) << 32);
    let start = leaf as usize * 4096;
    let original = bytes.clone();
    for case in 0..10 {
        bytes.clone_from(&original);
        match case {
            0 => {
                // A checksummed node referring to itself cannot decrease depth.
                bytes[start + 2..start + 4].copy_from_slice(&1u16.to_le_bytes());
                bytes[start + 6..start + 8].copy_from_slice(&1u16.to_le_bytes());
                bytes[start + 12..start + 16].fill(0);
                bytes[start + 16..start + 20].copy_from_slice(&(leaf as u32).to_le_bytes());
                bytes[start + 20..start + 22].copy_from_slice(&((leaf >> 32) as u16).to_le_bytes());
                bytes[start + 22..start + 24].fill(0);
            }
            1 => bytes[start + 16..start + 18].fill(0), // zero-length extent
            2 => bytes[start + 24..start + 28].fill(0), // overlapping logical ranges
            3 => bytes[start + 12..start + 16].copy_from_slice(&u32::MAX.to_le_bytes()),
            4 => {
                bytes[start + 2..start + 4].fill(0);
                bytes[start + 6..start + 8].copy_from_slice(&1u16.to_le_bytes());
            }
            5 => bytes[start + 18..start + 24].fill(0), // initialized block zero
            6 => bytes[start + 12..start + 16].copy_from_slice(&8u32.to_le_bytes()),
            // Locally valid, non-overlapping leaf whose first key disagrees
            // with its parent's logical zero. The checksum alone admits it.
            7 => bytes[start + 12..start + 16].copy_from_slice(&1u32.to_le_bytes()),
            8 => bytes[start + 40..start + 42].fill(0), // malformed middle extent
            _ => {
                // A middle initialized extent outside the physical image.
                bytes[start + 42..start + 44].fill(0);
                bytes[start + 44..start + 48].copy_from_slice(&((original.len() / 4096) as u32).to_le_bytes());
            }
        }
        let maximum = u16::from_le_bytes(bytes[start + 4..start + 6].try_into().unwrap()) as usize;
        let checksum_offset = start + 12 * (maximum + 1);
        let mut checksum = u32::from_le_bytes(bytes[1648..1652].try_into().unwrap());
        for byte in number.to_le_bytes().iter()
            .chain(&bytes[inode_start + 0x64..inode_start + 0x68])
            .chain(&bytes[start..checksum_offset]) {
            checksum ^= u32::from(*byte);
            for _ in 0..8 { checksum = (checksum >> 1) ^ (0x82f6_3b78u32 & 0u32.wrapping_sub(checksum & 1)); }
        }
        bytes[checksum_offset..checksum_offset + 4].copy_from_slice(&checksum.to_le_bytes());
        let stage = std::rc::Rc::new(ext4plus::JournalMutationStage::new(Box::new(bytes.clone()), bytes.len() as u64).unwrap());
        let filesystem = ext4plus::Ext4::load_with_writer(Box::new(stage.clone()), Some(Box::new(stage.clone()))).unwrap();
        let inode = ext4plus::inode::Inode::read(&filesystem, std::num::NonZeroU32::new(number).unwrap()).unwrap();
        assert!(inode.validate_extent_tree(&filesystem).is_err());
        // Exercise the separate read-only iterator as well as mutable lookup and
        // complete-tree collection. No malformed directory entry is reached.
        let mut iterator = ext4plus::ReadDir::new(filesystem.clone(), &inode, ext4plus::path::PathBuf::empty()).unwrap();
        assert!(iterator.next().unwrap().is_err());
        assert!(iterator.next().is_none());
        let mut file = ext4plus::file::File::open_inode(&filesystem, inode).unwrap();
        // A direct lookup need only visit the selected valid extent; the
        // whole-tree admission and mutation collection must find case9 too.
        if case != 9 {
            assert!(file.read_bytes_at(&mut [0; 1], 0).is_err());
            assert!(file.write_bytes_at(b"bad", 0).is_err());
        }
        assert!(file.truncate(0).is_err());
        assert!(stage.is_empty());
        let size = bytes.len() as u64;
        DEVICE.with_borrow_mut(|device| *device = Device { bytes: bytes.clone(), watched_read_block: Some(leaf), ..Device::default() });
        assert!(ext4::mount(1, size).is_err());
        DEVICE.with_borrow(|device| {
            assert!(device.events.is_empty());
            assert!(device.watched_reads <= 2);
            assert_eq!(device.bytes, bytes);
        });
        if case >= 8 {
            // Even a zero-size inode may own unwritten allocation beyond EOF.
            // Validating only the first/last readable byte would miss this.
            let image = path.with_extension(format!("coordinator-empty-corrupt-extents-{case}.img"));
            std::fs::write(&image, &bytes).unwrap();
            debugfs(&image, &format!("set_inode_field <{number}> size 0"));
            let empty = std::fs::read(&image).unwrap();
            DEVICE.with_borrow_mut(|device| *device = Device { bytes: empty.clone(), ..Device::default() });
            assert!(ext4::mount(1, size).is_err());
            DEVICE.with_borrow(|device| {
                assert!(device.events.is_empty());
                assert_eq!(device.bytes, empty);
            });
            debugfs(&image, &format!("set_inode_field <{number}> links_count 0"));
            debugfs(&image, "unlink /system/extent-cycle");
            debugfs(&image, &format!("set_super_value last_orphan {number}"));
            debugfs(&image, "feature needs_recovery");
            let orphan = std::fs::read(&image).unwrap();
            DEVICE.with_borrow_mut(|device| *device = Device { bytes: orphan.clone(), ..Device::default() });
            assert!(ext4::mount(1, size).is_err());
            DEVICE.with_borrow(|device| {
                assert!(device.events.is_empty(), "malformed orphan was checkpointed");
                assert_eq!(device.bytes, orphan);
            });
        }
    }
}

#[test]
fn inode_checksums_follow_declared_extra_size_and_preserve_undeclared_bytes() {
    let Some(path) = fixture() else { return };
    let pristine = std::fs::read(&path).unwrap();
    let table = u32::from_le_bytes(pristine[4104..4108].try_into().unwrap()) as usize * 4096;
    let root = table + 256;
    for extra in [1u16, 2, 3, 129, 132, u16::MAX] {
        let image = path.with_extension(format!("coordinator-extra-size-invalid-{extra}.img"));
        write_sparse_fixture(&image, &pristine).unwrap();
        debugfs(&image, &format!("set_inode_field <2> extra_isize {extra}"));
        let hostile = std::fs::read(&image).unwrap();
        assert_eq!(u16::from_le_bytes(hostile[root + 0x80..root + 0x82].try_into().unwrap()), extra);
        let raw = ext4plus::Ext4::load(Box::new(hostile.clone())).unwrap();
        let error = ext4plus::inode::Inode::read(&raw, std::num::NonZeroU32::new(2).unwrap()).unwrap_err();
        assert!(format!("{error:?}").starts_with("Corrupt(InodeTruncated"), "{error:?}");
        DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
        assert!(ext4::mount(1, hostile.len() as u64).is_err());
        DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
    }
    for extra in [0u16, 4, 16, 32] {
        let image = path.with_extension(format!("coordinator-checksum-width-{extra}.img"));
        write_sparse_fixture(&image, &pristine).unwrap();
        // debugfs recalculates the inode checksum with the Linux has_hi rule.
        // For extra=0, the old bytes at0x82 are covered as ordinary data.
        debugfs(&image, &format!("set_inode_field <2> extra_isize {extra}"));
        let input = std::fs::read(&image).unwrap();
        assert_eq!(u16::from_le_bytes(input[root + 0x80..root + 0x82].try_into().unwrap()), extra);
        let raw = ext4plus::Ext4::load(Box::new(input.clone())).unwrap();
        let inode = ext4plus::inode::Inode::read(&raw, std::num::NonZeroU32::new(2).unwrap()).unwrap();
        assert!(inode.list_xattrs(&raw).unwrap().is_empty());
        let mut mounted = mount_bytes(input.clone());
        fsck(&path, &format!("coordinator-checksum-width-before-{extra}"));
        ext4::create_file_probe(&mut mounted, b"checksum-width-file", 0o600).unwrap();
        ext4::transaction_probe(&mut mounted, b"checksum-width-file", 4093, b"checksum width").unwrap();
        if extra == 0 {
            let before = DEVICE.with_borrow(|device| device.bytes.clone());
            assert_eq!(ext4::set_xattr(&mut mounted, b".", b"user.note", Some(&[0x55; 4096])), Err(Status::Full));
            DEVICE.with_borrow(|device| assert_eq!(device.bytes, before));
        }
        ext4::set_times(&mut mounted, b".", 1_780_000_001, 0, 1_780_000_002, 0).unwrap();
        if extra < 16 {
            let before = DEVICE.with_borrow(|device| device.bytes.clone());
            for (seconds, nanos) in [(0x8000_0000, 0), (1_780_000_003, 1)] {
                assert_eq!(ext4::set_times(&mut mounted, b".", 1_780_000_001, 0, seconds, nanos), Err(Status::Range));
                DEVICE.with_borrow(|device| assert_eq!(device.bytes, before));
            }
        } else {
            // atime/mtime extras fit at16 bytes even though crtime does not.
            ext4::set_times(&mut mounted, b".", 0x8000_0000, 123, 0xffff_ffff, 456).unwrap();
        }
        ext4::sync(&mut mounted).unwrap();
        ext4::unmount(&mounted).unwrap();
        let output = DEVICE.with_borrow(|device| device.bytes.clone());
        let raw = ext4plus::Ext4::load(Box::new(output.clone())).unwrap();
        let node = ext4plus::inode::Inode::read(&raw, std::num::NonZeroU32::new(2).unwrap()).unwrap();
        assert_eq!(node.atime(), std::time::Duration::new(if extra < 16 { 1_780_000_001 } else { 0x8000_0000 }, if extra < 16 { 0 } else { 123 }));
        assert_eq!(node.mtime(), std::time::Duration::new(if extra < 16 { 1_780_000_002 } else { 0xffff_ffff }, if extra < 16 { 0 } else { 456 }));
        if extra == 0 {
            assert_eq!(&output[root + 0x82..root + 256], &input[root + 0x82..root + 256]);
        }
        fsck(&path, &format!("coordinator-checksum-width-after-{extra}"));
        drop(mounted);
        let mut hostile = output;
        hostile[root + 0x82] ^= 1;
        DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
        assert!(ext4::mount(1, hostile.len() as u64).is_err());
        DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
    }
}

#[test]
fn automatic_timestamps_respect_each_inode_field_and_roll_back_reservations() {
    let Some(path) = fixture() else { return };
    let now = 1_780_000_000;
    let later = 0x8000_0000;
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, b"timestamp-field-file", 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, b"timestamp-field-file", 0, b"original bytes").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    let pristine = DEVICE.with_borrow(|device| device.bytes.clone());
    for extra in [0u16, 4, 8, 12, 16, 20, 24, 32] {
        let image = path.with_extension(format!("coordinator-automatic-time-{extra}.img"));
        write_sparse_fixture(&image, &pristine).unwrap();
        for name in ["<2>", "/timestamp-field-file"] {
            debugfs(&image, &format!("set_inode_field {name} extra_isize {extra}"));
        }
        let mut mounted = mount_bytes(std::fs::read(&image).unwrap());
        // Arm the recovery marker before testing rollback so its legitimate
        // first-write transition cannot hide unintended mutation writes.
        ext4::chmod(&mut mounted, b"system/README.TXT", 0o600).unwrap();
        for operation in 0..4 {
            let before = DEVICE.with_borrow_mut(|device| {
                device.events.clear();
                device.bytes.clone()
            });
            let free = ext4::free_bytes(&mounted).unwrap();
            let apply = |mounted: &mut ext4::Mounted| match operation {
                0 => ext4::chmod(mounted, b".", 0o700),
                1 => ext4::create_file_probe(mounted, b"timestamp-new-child", 0o600),
                2 => ext4::transaction_probe(mounted, b"timestamp-field-file", 8191, b"epoch boundary").map(|_| ()),
                _ => ext4::truncate_probe(mounted, b"timestamp-field-file", 1),
            };
            let previous = MUTATION_TIME.with(|time| time.replace(later));
            let result = apply(&mut mounted);
            MUTATION_TIME.with(|time| time.set(previous));
            // ctime fits at8 extra bytes; mtime needs12. atime/crtime are
            // untouched by these operations, so their absence is irrelevant.
            if extra < if operation == 0 { 8 } else { 12 } {
                assert_eq!(result, Err(Status::Range), "extra={extra}, operation={operation}");
                assert_eq!(ext4::free_bytes(&mounted).unwrap(), free);
                DEVICE.with_borrow(|device| {
                    assert!(device.events.is_empty());
                    assert_eq!(device.bytes, before);
                });
                let previous = MUTATION_TIME.with(|time| time.replace(now));
                let retry = apply(&mut mounted);
                MUTATION_TIME.with(|time| time.set(previous));
                retry.unwrap();
            } else {
                result.unwrap();
                let raw = ext4plus::Ext4::load(Box::new(DEVICE.with_borrow(|device| device.bytes.clone()))).unwrap();
                let name = if operation < 2 { "/" } else { "/timestamp-field-file" };
                let inode = raw.path_to_inode(ext4plus::path::Path::try_from(name).unwrap(),
                    ext4plus::FollowSymlinks::All).unwrap();
                assert_eq!(inode.ctime(), std::time::Duration::from_secs(later));
                if operation != 0 {
                    assert_eq!(inode.mtime(), std::time::Duration::from_secs(later));
                }
            }
        }
        ext4::sync(&mut mounted).unwrap();
        ext4::unmount(&mounted).unwrap();
        fsck(&path, &format!("coordinator-automatic-time-{extra}"));
    }
}

#[test]
fn inode_block_count_uses_48_bits_without_overwriting_xattr_address() {
    use ext4plus::Ext4Read;
    let Some(path) = fixture() else { return };
    let image = path.with_extension("coordinator-inode-block-fields.img");
    let pristine = std::fs::read(&path).unwrap();
    write_sparse_fixture(&image, &pristine).unwrap();
    // These deliberately oversized fields exercise the Linux disk layout,
    // without admitting the synthetic inode as a writable filesystem.
    debugfs(&image, "set_inode_field <2> blocks 0x123456780008");
    debugfs(&image, "set_inode_field <2> file_acl 0xa55a11223344");
    let bytes = std::fs::read(&image).unwrap();
    let table = u32::from_le_bytes(bytes[4104..4108].try_into().unwrap()) as usize;
    let offset = table * 4096 + 256;
    let stage = std::rc::Rc::new(ext4plus::JournalMutationStage::new(Box::new(bytes.clone()), bytes.len() as u64).unwrap());
    let filesystem = ext4plus::Ext4::load_with_writer(Box::new(stage.clone()), Some(Box::new(stage.clone()))).unwrap();
    let index = std::num::NonZeroU32::new(2).unwrap();
    let original = ext4plus::inode::Inode::read(&filesystem, index).unwrap();
    assert_eq!(original.blocks(), 0x1234_5678_0008);
    assert_eq!(original.fs_blocks(&filesystem).unwrap(), 0x1234_5678_0008 / 8);
    for huge in [false, true] {
        let divisor = if huge { 1 } else { 8 };
        let maximum = ((1u64 << 48) - 1) / divisor;
        for count in [0, 1, (1u64 << 32) / divisor, maximum] {
            let mut inode = original.clone();
            let mut flags = inode.flags();
            flags.set(ext4plus::inode::InodeFlags::HUGE_FILE, huge);
            inode.set_flags(flags);
            assert_eq!(inode.set_fs_blocks(count, &filesystem).unwrap(), count * divisor);
            assert_eq!(inode.fs_blocks(&filesystem).unwrap(), count);
            // Neither encoding overflow nor sector multiplication overflow may
            // partially change an inode that the caller could later persist.
            for overflow in [maximum + 1, u64::MAX] {
                assert!(inode.set_fs_blocks(overflow, &filesystem).is_err());
                assert_eq!(inode.blocks(), count * divisor);
                assert!(stage.is_empty());
            }
            inode.write(&filesystem).unwrap();
            let loaded = ext4plus::inode::Inode::read(&filesystem, index).unwrap();
            assert_eq!(loaded.fs_blocks(&filesystem).unwrap(), count);
            let mut raw = [0; 256];
            stage.read(offset as u64, &mut raw).unwrap();
            assert_eq!(&raw[0x68..0x6c], &0x1122_3344u32.to_le_bytes());
            assert_eq!(&raw[0x76..0x78], &0xa55au16.to_le_bytes());
            assert_eq!(u32::from_le_bytes(raw[0x1c..0x20].try_into().unwrap()) as u64
                | ((u16::from_le_bytes(raw[0x74..0x76].try_into().unwrap()) as u64) << 32), count * divisor);
            stage.rollback();
            stage.read(offset as u64, &mut raw).unwrap();
            assert_eq!(&raw, &bytes[offset..offset + 256]);
        }
    }
    debugfs(&image, "set_inode_field <2> blocks 9");
    let filesystem = ext4plus::Ext4::load(Box::new(std::fs::read(&image).unwrap())).unwrap();
    let inode = ext4plus::inode::Inode::read(&filesystem, index).unwrap();
    assert!(inode.fs_blocks(&filesystem).is_err());
}

#[test]
fn overwrite_merging_an_imported_extent_tree_accounts_for_freed_nodes() {
    let Some(path) = fixture() else { return };
    let name = b"system/merge-count";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    for index in 0..6 { ext4::transaction_probe(&mut mounted, name, index * 8192, &[index as u8 + 1]).unwrap(); }
    let number = ext4::stat(&mounted, name).unwrap().inode as u32;
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    let mut bytes = DEVICE.with_borrow(|device| device.bytes.clone());
    let ipg = u32::from_le_bytes(bytes[1064..1068].try_into().unwrap());
    let descriptor = 4096 + ((number - 1) / ipg) as usize * 64;
    let table = u32::from_le_bytes(bytes[descriptor + 8..descriptor + 12].try_into().unwrap()) as usize;
    let inode_start = table * 4096 + ((number - 1) % ipg) as usize * 256;
    assert_eq!(u16::from_le_bytes(bytes[inode_start + 0x2e..inode_start + 0x30].try_into().unwrap()), 1);
    let leaf = u32::from_le_bytes(bytes[inode_start + 0x38..inode_start + 0x3c].try_into().unwrap()) as usize * 4096;
    assert_eq!(u16::from_le_bytes(bytes[leaf + 2..leaf + 4].try_into().unwrap()), 6);
    let mut runs = 0;
    let mut previous = None;
    let mut old = vec![0; 6 * 4096];
    for index in 0..6 {
        let entry = leaf + 12 + index * 12;
        bytes[entry..entry + 4].copy_from_slice(&(index as u32).to_le_bytes());
        let physical = u32::from_le_bytes(bytes[entry + 8..entry + 12].try_into().unwrap());
        assert_eq!(u16::from_le_bytes(bytes[entry + 4..entry + 6].try_into().unwrap()), 1);
        if previous != physical.checked_sub(1) { runs += 1; }
        previous = Some(physical);
        old[index * 4096] = index as u8 + 1;
    }
    assert!(runs <= 4, "fixture must collapse to the inline root after merging");
    let maximum = u16::from_le_bytes(bytes[leaf + 4..leaf + 6].try_into().unwrap()) as usize;
    let checksum_offset = leaf + 12 * (maximum + 1);
    let mut crc = u32::from_le_bytes(bytes[1648..1652].try_into().unwrap());
    for byte in number.to_le_bytes().iter().chain(&bytes[inode_start + 0x64..inode_start + 0x68])
        .chain(&bytes[leaf..checksum_offset]) {
        crc ^= u32::from(*byte);
        for _ in 0..8 { crc = (crc >> 1) ^ (0x82f6_3b78u32 & 0u32.wrapping_sub(crc & 1)); }
    }
    bytes[checksum_offset..checksum_offset + 4].copy_from_slice(&crc.to_le_bytes());
    let image = path.with_extension("coordinator-merge-count-input.img");
    write_sparse_fixture(&image, &bytes).unwrap();
    debugfs(&image, "set_inode_field /system/merge-count size 24576");
    let baseline = std::fs::read(&image).unwrap();
    let mut mounted = mount_bytes(baseline.clone());
    let free = ext4::free_bytes(&mounted).unwrap();
    let mut data = vec![0; old.len()];
    read_exact(&mounted, name, &mut data);
    assert_eq!(data, old);
    fsck(&path, "coordinator-merge-count-before");
    let mut new = old.clone();
    new[17..22].copy_from_slice(b"merge");
    ext4::transaction_probe(&mut mounted, name, 17, b"merge").unwrap();
    let operations = DEVICE.with_borrow(|device| device.events.clone());
    assert_eq!(ext4::free_bytes(&mounted).unwrap(), free + 4096);
    assert!(operations.iter().any(|event| matches!(event, Event::Write(_, bytes)
        if bytes.len() == 4096 && bytes[..8] == [0xc0, 0x3b, 0x39, 0x98, 0, 0, 0, 5])));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-merge-count-after");
    drop(mounted);
    let failure = operations.iter().position(|event| matches!(event, Event::Flush(3))).unwrap();
    let mut mounted = mount_bytes(baseline.clone());
    DEVICE.with_borrow_mut(|device| device.fail_event = Some(failure));
    assert_eq!(ext4::transaction_probe(&mut mounted, name, 17, b"merge"), Err(Status::Io));
    DEVICE.with_borrow_mut(|device| device.fail_event = None);
    ext4::transaction_probe(&mut mounted, name, 17, b"merge").unwrap();
    assert_eq!(ext4::free_bytes(&mounted).unwrap(), free + 4096);
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-merge-count-retry");
    drop(mounted);
    let mut cut = baseline;
    let mut committed = false;
    for (index, operation) in operations.iter().enumerate() {
        match operation {
            Event::Write(start, bytes) => cut[*start as usize..*start as usize + bytes.len()].copy_from_slice(bytes),
            Event::Flush(boundary) => {
                committed |= *boundary == 3;
                for linux in [false, true] {
                    let input = if linux {
                        let image = path.with_extension(format!("coordinator-merge-count-linux-{index}.img"));
                        write_sparse_fixture(&image, &cut).unwrap();
                        let replay = std::process::Command::new("e2fsck").args(["-E", "journal_only", "-p"])
                            .arg(&image).output().unwrap();
                        let log = format!("{}\n{}", String::from_utf8_lossy(&replay.stdout), String::from_utf8_lossy(&replay.stderr));
                        std::fs::write(image.with_extension("replay.txt"), &log).unwrap();
                        assert!(matches!(replay.status.code(), Some(0 | 1)), "{log}");
                        std::fs::read(&image).unwrap()
                    } else { cut.clone() };
                    let recovered = mount_bytes(input);
                    assert_eq!(ext4::free_bytes(&recovered).unwrap(), if committed { free + 4096 } else { free });
                    read_exact(&recovered, name, &mut data);
                    if committed { assert_eq!(data, new); } else { assert!(data == old || data == new); }
                    ext4::unmount(&recovered).unwrap();
                    fsck(&path, &format!("coordinator-merge-count-cut-{index}-{linux}"));
                }
            }
        }
    }
}

#[test]
fn inode_counts_must_match_all_mapping_and_xattr_blocks_before_admission() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/count-file";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    for block in 0..6 { ext4::transaction_probe(&mut mounted, name, block * 8192, b"allocated").unwrap(); }
    ext4::create_file_probe(&mut mounted, b"system/count-empty", 0o600).unwrap();
    ext4::symlink_probe(&mut mounted, b"system/count-short", b"missing").unwrap();
    ext4::symlink_probe(&mut mounted, b"system/count-long", &[b'x'; 80]).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    let image = path.with_extension("coordinator-inode-count-input.img");
    DEVICE.with_borrow(|device| write_sparse_fixture(&image, &device.bytes).unwrap());
    debugfs(&image, &format!("ea_set /system/count-file user.large {}", "q".repeat(300)));
    let pristine = std::fs::read(&image).unwrap();
    let raw = ext4plus::Ext4::load(Box::new(pristine.clone())).unwrap();
    let mut mounted = mount_bytes(pristine.clone());
    fsck(&path, "coordinator-inode-count-valid");
    let number = ext4::stat(&mounted, name).unwrap().inode as u32;
    let inode = ext4plus::inode::Inode::read(&raw, std::num::NonZeroU32::new(number).unwrap()).unwrap();
    assert_eq!(inode.fs_blocks(&raw).unwrap(), 8, "six data blocks, one extent node, one xattr");
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    for entry in ["/system/count-file", "/system/count-empty", "/system/count-short", "/system/count-long", "<2>", "<8>"] {
        // Read debugfs's independent on-disk sector count, including legacy
        // journal mapping blocks and both inline and external symlinks.
        let stat = std::process::Command::new("debugfs").args(["-R", &format!("stat {entry}")])
            .arg(&image).output().unwrap();
        assert!(stat.status.success());
        let output = String::from_utf8(stat.stdout).unwrap();
        let sectors: u64 = output.split("Blockcount:").nth(1).unwrap()
            .split_whitespace().next().unwrap().parse().unwrap();
        for count in [sectors + 1, sectors + 8, sectors.saturating_sub(8)] {
            if count == sectors { continue; }
            for orphan in [false, true] {
                if orphan && entry != "/system/count-file" { continue; }
                write_sparse_fixture(&image, &pristine).unwrap();
                debugfs(&image, &format!("set_inode_field {entry} blocks {count}"));
                if orphan {
                    debugfs(&image, &format!("set_inode_field <{number}> size 0"));
                    debugfs(&image, &format!("set_inode_field <{number}> links_count 0"));
                    debugfs(&image, "unlink /system/count-file");
                    debugfs(&image, &format!("set_super_value last_orphan {number}"));
                    debugfs(&image, "feature needs_recovery");
                }
                let hostile = std::fs::read(&image).unwrap();
                DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
                assert!(ext4::mount(1, hostile.len() as u64).is_err(), "accepted {entry}, blocks={count}, orphan={orphan}");
                DEVICE.with_borrow(|device| {
                    assert!(device.events.is_empty());
                    assert_eq!(device.bytes, hostile);
                });
            }
        }
        write_sparse_fixture(&image, &pristine).unwrap();
    }
    // Truncating a tree with an external xattr must leave its charge intact;
    // the subsequent unlink releases that final block with its inode.
    mounted = mount_bytes(pristine);
    ext4::truncate_probe(&mut mounted, name, 0).unwrap();
    ext4::sync(&mut mounted).unwrap();
    fsck(&path, "coordinator-inode-count-truncated");
    ext4::unlink_file_probe(&mut mounted, name).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-inode-count-freed");
}

#[test]
fn namespace_counts_parents_types_and_unique_names_are_checked_before_mutation() {
    let Some(path) = fixture() else { return };
    let directory = b"system/namespace-count";
    let first = b"system/namespace-count/first";
    let alias = b"system/namespace-count/alias";
    let mut mounted = mount_fixture(&path);
    ext4::create_directory_probe(&mut mounted, directory).unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/namespace-count/child").unwrap();
    ext4::create_file_probe(&mut mounted, first, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, first, 0, b"two names").unwrap();
    ext4::link_file_probe(&mut mounted, first, alias).unwrap();
    ext4::symlink_probe(&mut mounted, b"system/namespace-count/sym-a", b"missing").unwrap();
    ext4::link_file_probe(&mut mounted, b"system/namespace-count/sym-a", b"system/namespace-count/sym-b").unwrap();
    let number = ext4::stat(&mounted, directory).unwrap().inode as u32;
    let root_links = ext4::stat(&mounted, b".").unwrap().links;
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-namespace-count-before");
    drop(mounted);
    let pristine = DEVICE.with_borrow(|device| device.bytes.clone());
    let raw = ext4plus::Ext4::load(Box::new(pristine.clone())).unwrap();
    let node = ext4plus::inode::Inode::read(&raw, std::num::NonZeroU32::new(number).unwrap()).unwrap();
    let file = ext4plus::file::File::open_inode(&raw, node).unwrap();
    let block = file.filesystem_block_at_offset(0).unwrap().unwrap() as usize * 4096;
    let ipg = u32::from_le_bytes(pristine[1064..1068].try_into().unwrap());
    let descriptor = 4096 + ((number - 1) / ipg) as usize * 64;
    let table = u32::from_le_bytes(pristine[descriptor + 8..descriptor + 12].try_into().unwrap()) as usize;
    let inode_start = table * 4096 + ((number - 1) % ipg) as usize * 256;
    let entry_offset = |name: &[u8]| {
        let mut offset = block;
        while offset < block + 4084 {
            let length = pristine[offset + 6] as usize;
            if &pristine[offset + 8..offset + 8 + length] == name { return offset; }
            offset += u16::from_le_bytes(pristine[offset + 4..offset + 6].try_into().unwrap()) as usize;
        }
        panic!("fixture directory entry missing");
    };
    let first_offset = entry_offset(b"first");
    let alias_offset = entry_offset(b"alias");
    let image = path.with_extension("coordinator-namespace-count-input.img");
    for case in 0..20 {
        for dirty in [false, true] {
            write_sparse_fixture(&image, &pristine).unwrap();
            match case {
                0 => debugfs(&image, "set_inode_field /system/namespace-count/first links_count 1"),
                1 => debugfs(&image, "set_inode_field /system/namespace-count/first links_count 3"),
                2 => debugfs(&image, "set_inode_field /system/namespace-count/sym-a links_count 1"),
                3 => debugfs(&image, "set_inode_field /system/namespace-count links_count 2"),
                4 => debugfs(&image, &format!("set_inode_field <2> links_count {}", root_links + 1)),
                5 => debugfs(&image, "link /system/namespace-count/child /data/user/directory-alias"),
                6 => debugfs(&image, "link /system/namespace-count /system/namespace-count/cycle"),
                _ => {
                    let mut hostile = pristine.clone();
                    match case {
                        7 => hostile[block..block + 4].copy_from_slice(&2u32.to_le_bytes()),
                        8 => hostile[block + 12..block + 16].copy_from_slice(&2u32.to_le_bytes()),
                        9 => hostile[block..block + 4].fill(0),
                        10 => hostile[alias_offset + 8..alias_offset + 13].copy_from_slice(b"first"),
                        11 => hostile[first_offset + 7] = 2, // inode remains regular
                        12 => hostile[first_offset + 4..first_offset + 6].copy_from_slice(&9u16.to_le_bytes()),
                        13 => hostile[first_offset + 4..first_offset + 6].copy_from_slice(&65532u16.to_le_bytes()),
                        14 => hostile[first_offset + 4..first_offset + 6].copy_from_slice(&8u16.to_le_bytes()),
                        _ => {
                            if case != 19 { hostile[first_offset..first_offset + 4].fill(0); }
                            let length = match case {
                                15 => 65532,
                                16 => (block + 4092 - first_offset) as u16,
                                18 | 19 => (block + 4096 - first_offset) as u16,
                                _ => 9,
                            };
                            hostile[first_offset + 4..first_offset + 6].copy_from_slice(&length.to_le_bytes());
                        }
                    }
                    let mut crc = u32::from_le_bytes(hostile[1648..1652].try_into().unwrap());
                    for byte in number.to_le_bytes().iter().chain(&hostile[inode_start + 0x64..inode_start + 0x68])
                        .chain(&hostile[block..block + 4084]) {
                        crc ^= u32::from(*byte);
                        for _ in 0..8 { crc = (crc >> 1) ^ (0x82f6_3b78u32 & 0u32.wrapping_sub(crc & 1)); }
                    }
                    hostile[block + 4092..block + 4096].copy_from_slice(&crc.to_le_bytes());
                    write_sparse_fixture(&image, &hostile).unwrap();
                }
            }
            if dirty { debugfs(&image, "feature needs_recovery"); }
            let hostile = std::fs::read(&image).unwrap();
            let raw = ext4plus::Ext4::load(Box::new(hostile.clone())).unwrap();
            // Both directory and inode CRCs are valid; the new graph/count
            // checks must detect these semantically invalid namespaces.
            if case < 12 {
                for entry in raw.read_dir("/system/namespace-count").unwrap() { entry.unwrap().metadata().unwrap(); }
            } else {
                let error = raw.read_dir("/system/namespace-count").unwrap()
                    .find_map(|entry| entry.err()).expect("malformed record must fail iteration");
                assert!(format!("{error:?}").starts_with("Corrupt(DirEntry"),
                    "record bounds, not a checksum failure: {error:?}");
            }
            DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
            assert!(ext4::mount(1, hostile.len() as u64).is_err(), "accepted namespace case {case}, dirty={dirty}");
            DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
        }
    }
    let mut mounted = mount_bytes(pristine);
    ext4::unlink_file_probe(&mut mounted, first).unwrap();
    let mut contents = [0; 9];
    read_exact(&mounted, alias, &mut contents);
    assert_eq!(&contents, b"two names");
    assert_eq!(ext4::stat(&mounted, alias).unwrap().links, 1);
    ext4::rename_probe(&mut mounted, b"system/namespace-count/child", b"data/user/moved-child").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-namespace-count-valid");
}

#[test]
fn file_extents_cannot_overwrite_or_free_fixed_metadata() {
    let Some(path) = fixture() else { return };
    let name = b"system/metadata-alias";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, name, 0, b"original").unwrap();
    let inode = ext4::stat(&mounted, name).unwrap().inode;
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-metadata-alias-before");
    drop(mounted);
    let pristine = DEVICE.with_borrow(|device| device.bytes.clone());
    let block_bitmap = u32::from_le_bytes(pristine[4096..4100].try_into().unwrap());
    let inode_bitmap = u32::from_le_bytes(pristine[4100..4104].try_into().unwrap());
    let inode_table = u32::from_le_bytes(pristine[4104..4108].try_into().unwrap());
    let image = path.with_extension("coordinator-metadata-alias-input.img");
    let mut reference = mount_bytes(pristine.clone());
    ext4::transaction_probe(&mut reference, name, 0, b"original").unwrap();
    ext4::sync(&mut reference).unwrap();
    ext4::unmount(&reference).unwrap();
    let expected_clean = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(reference);
    for target in [1, block_bitmap, inode_bitmap, inode_table, 8192, 8193] {
        std::fs::write(&image, &pristine).unwrap();
        // i_block[5] is ee_start_lo in this one-extent inline root. debugfs
        // recomputes the inode checksum, so checksum refusal cannot mask an
        // incorrect ordered-data classification of fixed filesystem metadata.
        debugfs(&image, &format!("set_inode_field <{inode}> block[5] {target}"));
        let hostile = std::fs::read(&image).unwrap();
        DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
        assert!(ext4::mount(1, hostile.len() as u64).is_err());
        DEVICE.with_borrow(|device| {
            assert!(device.events.is_empty());
            assert_eq!(device.bytes, hostile);
        });
        // Also retain mutation-boundary coverage: inject a checksummed inode
        // fault after admission, without changing the allocator or namespace.
        let ipg = u32::from_le_bytes(pristine[1064..1068].try_into().unwrap());
        let descriptor = 4096 + ((inode as u32 - 1) / ipg) as usize * 64;
        let table = u32::from_le_bytes(pristine[descriptor + 8..descriptor + 12].try_into().unwrap());
        let inode_start = table as usize * 4096 + ((inode as u32 - 1) % ipg) as usize * 256;
        for operation in 0..4 {
            let mut mounted = mount_bytes(pristine.clone());
            // Arm the marker on a valid view first, so its mandatory reload
            // cannot hide the later ordered-data/revoke/tail classification.
            ext4::transaction_probe(&mut mounted, name, 0, b"original").unwrap();
            let baseline = DEVICE.with_borrow(|device| device.bytes.clone());
            let metadata = ext4::stat(&mounted, name).unwrap();
            DEVICE.with_borrow_mut(|device| {
                device.bytes[inode_start..inode_start + 256].copy_from_slice(&hostile[inode_start..inode_start + 256]);
                device.events.clear();
            });
            let result = match operation {
                0 => ext4::transaction_probe(&mut mounted, name, 0, b"new-data").map(|_| ()),
                1 => ext4::truncate_probe(&mut mounted, name, 0),
                2 => ext4::truncate_probe(&mut mounted, name, 1),
                _ => ext4::unlink_file_probe(&mut mounted, name),
            };
            assert_eq!(result, Err(Status::Invalid), "fixed block {target}, operation={operation}");
            assert_public_reads_refused(&mounted);
            DEVICE.with_borrow_mut(|device| {
                assert!(device.events.is_empty(), "classification emitted storage writes");
                device.bytes[inode_start..inode_start + 256].copy_from_slice(&baseline[inode_start..inode_start + 256]);
                assert_eq!(device.bytes, baseline);
            });
            ext4::sync(&mut mounted).unwrap();
            assert_eq!(ext4::stat(&mounted, name), Ok(metadata));
            ext4::unmount(&mounted).unwrap();
            DEVICE.with_borrow(|device| assert_eq!(device.bytes, expected_clean));
        }
        // The input deliberately contains an invalid ownership alias; do not
        // present it as a clean-fsck result. Refusal preserves it byte-for-byte.
    }
    let mut mounted = mount_bytes(pristine);
    assert_eq!(ext4::transaction_probe(&mut mounted, name, 0, b"new-data"), Ok(8));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-metadata-alias-valid-write");
}

fn orphan_recovery_failure_case(case: u8) {
    let directory = case != 0;
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    if directory {
        ext4::create_directory_probe(&mut mounted, b"system/orphan").unwrap();
    } else {
        ext4::create_file_probe(&mut mounted, b"system/orphan", 0o600).unwrap();
        ext4::transaction_probe(&mut mounted, b"system/orphan", 0, b"unclosed").unwrap();
    }
    let inode = ext4::stat(&mounted, b"system/orphan").unwrap().inode;
    ext4::sync(&mut mounted).unwrap();
    if directory {
        ext4::remove_directory_guarded(&mut mounted, b"system/orphan", &[inode]).unwrap();
    } else { ext4::unmount(&mounted).unwrap(); }
    drop(mounted);
    let image = path.with_extension(format!("coordinator-orphan-input-case-{case}.img"));
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    if !directory {
        debugfs(&image, "set_inode_field /system/orphan links_count 0");
        debugfs(&image, "unlink /system/orphan");
        debugfs(&image, &format!("set_super_value last_orphan {inode}"));
        debugfs(&image, "feature needs_recovery");
    }
    if case >= 2 {
        // Match Linux's zero i_size while the unlinked directory's storage
        // remains allocated, and its later data-free/inode-retained state.
        if case == 3 { debugfs(&image, &format!("punch <{inode}> 0 4294967295")); }
        debugfs(&image, &format!("set_inode_field <{inode}> size 0"));
    }
    let bytes = std::fs::read(&image).unwrap();
    let size = bytes.len() as u64;
    let mounted = mount_bytes(bytes.clone());
    assert_eq!(ext4::stat(&mounted, b"system/orphan"), Err(Status::NotFound));
    ext4::unmount(&mounted).unwrap();
    let events = DEVICE.with_borrow(|device| device.events.len());
    drop(mounted);
    fsck(&path, &format!("coordinator-orphan-cleanup-case-{case}"));
    for fail in 0..events {
        DEVICE.with_borrow_mut(|device| *device = Device {
            bytes: bytes.clone(), fail_event: Some(fail), ..Device::default()
        });
        assert!(matches!(ext4::mount(1, size), Err(Status::Io)), "orphan cleanup failure {fail}");
        DEVICE.with_borrow_mut(|device| { device.fail_event = Some(0); device.events.clear(); });
        // A repeated crash/refusal during replay must retain the cleanup work.
        let result = ext4::mount(1, size);
        if let Ok((mounted, _)) = result { ext4::unmount(&mounted).unwrap(); }
        else { assert!(matches!(result, Err(Status::Io))); }
        DEVICE.with_borrow_mut(|device| { device.fail_event = None; device.events.clear(); });
        let (mounted, _) = ext4::mount(1, size).unwrap();
        ext4::unmount(&mounted).unwrap();
        drop(mounted);
        fsck(&path, &format!("coordinator-orphan-case-{case}-failure-{fail}"));
    }
    // Invalid chain nodes must be rejected before replay checkpoints or clears
    // anything, even when debugfs has recomputed their inode checksums.
    for (case, command) in [
        ("linked", format!("set_inode_field <{inode}> links_count 1")),
        ("cycle", format!("set_inode_field <{inode}> dtime {inode}")),
        ("reachable", "link <INODE> /system/bad-orphan".replace("<INODE>", &format!("<{inode}>"))),
    ] {
        std::fs::write(&image, &bytes).unwrap();
        debugfs(&image, &command);
        let hostile = std::fs::read(&image).unwrap();
        DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
        assert!(ext4::mount(1, size).is_err(), "admitted {case} orphan");
        DEVICE.with_borrow(|device| {
            assert!(device.events.is_empty(), "wrote before refusing {case} orphan");
            assert!(device.bytes == hostile);
        });
    }
}

#[test]
fn unlink_open_preserves_inode_io_until_sync_observes_final_close() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/unlink-handle";
    let alias = b"data/user/retained-link";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    let inode = ext4::stat(&mounted, name).unwrap().inode;
    ext4::write_inode(&mut mounted, inode, 0, b"original").unwrap();
    ext4::link_file_probe(&mut mounted, name, alias).unwrap();
    ext4::unlink_file_guarded(&mut mounted, name, &[inode, inode]).unwrap();
    assert_eq!(ext4::stat(&mounted, name), Err(Status::NotFound));
    assert_eq!(ext4::stat_inode(&mounted, inode).unwrap().links, 1);
    ext4::append_inode(&mut mounted, inode, b"-open", 16384).unwrap();
    let mut bytes = [0; 13];
    read_exact(&mounted, alias, &mut bytes);
    assert_eq!(&bytes, b"original-open");
    ext4::symlink_probe(&mut mounted, b"system/open-symlink", b"../data/user/retained-link").unwrap();
    ext4::unlink_file_guarded(&mut mounted, b"system/open-symlink", &[inode]).unwrap();
    ext4::unlink_file_guarded(&mut mounted, alias, &[inode, inode]).unwrap();
    assert_eq!(ext4::stat(&mounted, alias), Err(Status::NotFound));
    assert_eq!(ext4::stat_inode(&mounted, inode).unwrap().links, 0);
    ext4::sync_with_open_inodes(&mut mounted, &[inode, inode]).unwrap();
    assert!(ext4::unmount(&mounted).is_err());
    ext4::append_inode(&mut mounted, inode, b"-orphan", 16384).unwrap();
    ext4::sync_with_open_inodes(&mut mounted, &[inode]).unwrap();
    assert_eq!(ext4::stat_inode(&mounted, inode).unwrap().size, 20);
    let mut held = [0; 20];
    assert_eq!(ext4::pread_inode(&mounted, inode, 0, &mut held).unwrap(), 20);
    assert_eq!(&held, b"original-open-orphan");
    ext4::sync(&mut mounted).unwrap();
    assert!(ext4::stat_inode(&mounted, inode).is_err());
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-unlink-open-alias");
}

#[test]
fn linked_truncate_orphan_recovery_preserves_names_inode_xattrs_and_prefix() {
    let Some(path) = fixture() else { return };
    for large in [false, true] {
        let name = b"system/linked-truncate";
        let alias = b"data/user/truncate-alias";
        let mut mounted = mount_fixture(&path);
        ext4::create_file_probe(&mut mounted, name, 0o640).unwrap();
        ext4::set_xattr(&mut mounted, name, b"user.retained", Some(b"attribute")).unwrap();
        ext4::link_file_probe(&mut mounted, name, alias).unwrap();
        let free_before_data = ext4::free_bytes(&mounted).unwrap();
        ext4::transaction_probe(&mut mounted, name, 0, &[0x53; 8192]).unwrap();
        let inode = ext4::stat(&mounted, name).unwrap().inode;
        ext4::sync(&mut mounted).unwrap();
        drop(mounted);
        let image = path.with_extension(format!("coordinator-linked-truncate-input-{large}.img"));
        DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
        if large { debugfs(&image, "fallocate /system/linked-truncate 0 8193"); }
        // Linux commits the desired EOF while the remaining allocated suffix
        // and nonzero link count identify a truncate orphan, not an unlink.
        debugfs(&image, "set_inode_field /system/linked-truncate size 17");
        debugfs(&image, &format!("set_super_value last_orphan {inode}"));
        debugfs(&image, "feature needs_recovery");
        let dirty = std::fs::read(&image).unwrap();
        let check = |mounted: &ext4::Mounted| {
            let metadata = ext4::stat(mounted, name).unwrap();
            assert_eq!((metadata.inode, metadata.size, metadata.links, metadata.mode & 0o777), (inode, 17, 2, 0o640));
            assert_eq!(ext4::stat(mounted, alias), Ok(metadata));
            assert_eq!(ext4::free_bytes(mounted), Ok(free_before_data - 4096));
            let mut content = [0; 17];
            read_exact(mounted, alias, &mut content);
            assert_eq!(content, [0x53; 17]);
            let mut value = [0; 9];
            assert_eq!(ext4::get_xattr(mounted, name, b"user.retained", &mut value), Ok(9));
            assert_eq!(&value, b"attribute");
            ext4::unmount(mounted).unwrap();
        };
        let reference = mount_bytes(dirty.clone());
        check(&reference);
        let events = DEVICE.with_borrow(|device| device.events.clone());
        if large { assert!(events.iter().filter(|event| **event == Event::Flush(3)).count() >= 3); }
        drop(reference);
        fsck(&path, &format!("coordinator-linked-truncate-complete-{large}"));
        for accept in [false, true] {
            for fail in 0..events.len() {
                DEVICE.with_borrow_mut(|device| *device = Device {
                    bytes: dirty.clone(), fail_event: Some(fail), accept_failed_write: accept,
                    ..Device::default()
                });
                assert!(matches!(ext4::mount(1, dirty.len() as u64), Err(Status::Io)), "linked recovery {large}/{fail}");
                DEVICE.with_borrow_mut(|device| { device.fail_event = Some(0); device.events.clear(); });
                match ext4::mount(1, dirty.len() as u64) {
                    Ok((mounted, _)) => check(&mounted),
                    Err(error) => assert_eq!(error, Status::Io),
                }
                DEVICE.with_borrow_mut(|device| { device.fail_event = None; device.events.clear(); });
                let (mut mounted, _) = ext4::mount(1, dirty.len() as u64).unwrap();
                check(&mounted);
                // Regrowth must expose zeroes throughout the removed tail,
                // including the retained partial block after repeated crashes.
                ext4::truncate_inode(&mut mounted, inode, 8192).unwrap();
                let mut content = [0xa5; 8192];
                read_exact(&mounted, name, &mut content);
                assert_eq!(&content[..17], &[0x53; 17]);
                assert!(content[17..].iter().all(|byte| *byte == 0));
                ext4::sync(&mut mounted).unwrap();
                ext4::unmount(&mounted).unwrap();
                fsck(&path, &format!("coordinator-linked-truncate-{large}-{accept}-{fail}"));
            }
        }
        let mut prefix = dirty;
        for (cut, event) in events.iter().enumerate() {
            match event {
                Event::Write(start, bytes) => prefix[*start as usize..*start as usize + bytes.len()].copy_from_slice(bytes),
                Event::Flush(_) => {
                    let recovered = mount_bytes(prefix.clone());
                    check(&recovered);
                    fsck(&path, &format!("coordinator-linked-truncate-{large}-cut-{cut}"));
                }
            }
        }
    }
}

#[test]
fn split_linked_truncate_retries_exact_request_and_preserves_old_or_new_prefix() {
    let Some(path) = fixture() else { return };
    for (large, size) in [(false, 17u64), (true, 17), (true, 0)] {
        let name = b"system/split-truncate";
        let alias = b"data/user/split-truncate-alias";
        let mut mounted = mount_fixture(&path);
        ext4::create_file_probe(&mut mounted, name, 0o640).unwrap();
        ext4::link_file_probe(&mut mounted, name, alias).unwrap();
        let free_before_data = ext4::free_bytes(&mounted).unwrap();
        ext4::transaction_probe(&mut mounted, name, 0, &[0x53; 8192]).unwrap();
        let inode = ext4::stat(&mounted, name).unwrap().inode;
        ext4::sync(&mut mounted).unwrap();
        drop(mounted);
        let image = path.with_extension(format!("coordinator-split-truncate-input-{large}-{size}.img"));
        DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
        if large {
            debugfs(&image, "fallocate /system/split-truncate 0 8193");
            debugfs(&image, &format!("set_inode_field /system/split-truncate size {}", 8194u64 * 4096));
        }
        let initial = std::fs::read(&image).unwrap();
        let prepare = || {
            let mut mounted = mount_bytes(initial.clone());
            if !large { ext4::set_stage_block_limit(&mut mounted, 4).unwrap(); }
            mounted
        };
        let perform = |mounted: &mut ext4::Mounted| {
            if large { ext4::truncate_inode(mounted, inode, size) }
            else { ext4::truncate_probe(mounted, name, size) }
        };
        let mut reference = prepare();
        let original_metadata = ext4::stat(&reference, name).unwrap();
        let original_free = ext4::free_bytes(&reference).unwrap();
        fsck(&path, &format!("coordinator-split-truncate-before-{large}-{size}"));
        perform(&mut reference).unwrap();
        let expected_metadata = ext4::stat(&reference, name).unwrap();
        let trace = DEVICE.with_borrow(|device| device.events.clone());
        assert!(trace.iter().filter(|event| **event == Event::Flush(3)).count() >= 3);
        let check = |mounted: &ext4::Mounted| {
            assert_eq!(ext4::stat(mounted, name), Ok(expected_metadata));
            assert_eq!(ext4::stat(mounted, alias), Ok(expected_metadata));
            assert_eq!((expected_metadata.inode, expected_metadata.size, expected_metadata.links), (inode, size, 2));
            assert_eq!(ext4::free_bytes(mounted), Ok(free_before_data - size.div_ceil(4096) * 4096));
            let mut content = vec![0; size as usize];
            read_exact(mounted, name, &mut content);
            assert!(content.iter().all(|byte| *byte == 0x53));
        };
        check(&reference);
        ext4::sync(&mut reference).unwrap();
        let final_bytes = DEVICE.with_borrow(|device| device.bytes.clone());
        drop(reference);
        for accept in [false, true] {
            for fail in 0..trace.len() {
                let mut mounted = prepare();
                DEVICE.with_borrow_mut(|device| {
                    device.fail_event = Some(fail); device.accept_failed_write = accept;
                });
                assert_eq!(perform(&mut mounted), Err(Status::Io), "truncate {large}/{size}/{fail}");
                if fail >= 2 {
                    assert_public_reads_refused(&mounted);
                    assert!(ext4::truncate_inode(&mut mounted, inode, size + 1).is_err());
                    assert!(ext4::unlink_file_probe(&mut mounted, name).is_err());
                }
                DEVICE.with_borrow_mut(|device| {
                    assert_eq!(device.events, trace[..=fail]);
                    device.events.clear(); device.fail_event = None;
                });
                let through_sync = accept && fail >= 2;
                if through_sync { ext4::sync(&mut mounted).unwrap(); }
                else { perform(&mut mounted).unwrap(); }
                let start = trace[..fail].iter().rposition(|event|
                    matches!(event, Event::Flush(0 | 4 | 5))).map_or(0, |index| index + 1);
                DEVICE.with_borrow(|device| {
                    if through_sync { assert!(device.events.starts_with(&trace[start..])); }
                    else { assert_eq!(device.events, trace[start..]); }
                });
                check(&mounted);
                ext4::sync(&mut mounted).unwrap();
                ext4::unmount(&mounted).unwrap();
                DEVICE.with_borrow(|device| assert!(device.bytes == final_bytes));
            }
        }
        fsck(&path, &format!("coordinator-split-truncate-retry-{large}-{size}"));
        let checkpoints: Vec<_> = trace.iter().enumerate().filter_map(|(index, event)|
            (*event == Event::Flush(4)).then_some(index)).collect();
        for fail in [checkpoints[0], *checkpoints.last().unwrap()] {
            let mut mounted = prepare();
            DEVICE.with_borrow_mut(|device| device.fail_event = Some(fail));
            assert_eq!(perform(&mut mounted), Err(Status::Io));
            DEVICE.with_borrow_mut(|device| {
                device.fail_event = None; device.fail_superblock_read = true;
            });
            // The checkpoint succeeds on retry but reload refuses. Preserve
            // both the journal Reload phase and the original truncate request.
            assert_eq!(perform(&mut mounted), Err(Status::Io));
            assert_public_reads_refused(&mounted);
            DEVICE.with_borrow_mut(|device| device.fail_superblock_read = false);
            perform(&mut mounted).unwrap();
            check(&mounted);
            ext4::sync(&mut mounted).unwrap();
            ext4::unmount(&mounted).unwrap();
            DEVICE.with_borrow(|device| assert!(device.bytes == final_bytes));
        }
        let mut prefix = initial;
        for (cut, event) in trace.iter().enumerate() {
            match event {
                Event::Write(start, bytes) => prefix[*start as usize..*start as usize + bytes.len()].copy_from_slice(bytes),
                Event::Flush(_) => {
                    let recovered = mount_bytes(prefix.clone());
                    if ext4::stat(&recovered, name) == Ok(original_metadata) {
                        assert_eq!(ext4::stat(&recovered, alias), Ok(original_metadata));
                        assert_eq!(ext4::free_bytes(&recovered), Ok(original_free));
                        let mut content = [0; 8192];
                        read_exact(&recovered, name, &mut content);
                        assert_eq!(content, [0x53; 8192]);
                    } else { check(&recovered); }
                    ext4::unmount(&recovered).unwrap();
                    fsck(&path, &format!("coordinator-split-truncate-{large}-{size}-cut-{cut}"));
                }
            }
        }
        let mut mounted = mount_bytes(final_bytes);
        ext4::truncate_inode(&mut mounted, inode, 8192).unwrap();
        let mut content = [0xa5; 8192];
        read_exact(&mounted, alias, &mut content);
        assert!(content[..size as usize].iter().all(|byte| *byte == 0x53));
        assert!(content[size as usize..].iter().all(|byte| *byte == 0));
        ext4::sync(&mut mounted).unwrap();
        ext4::unmount(&mounted).unwrap();
        fsck(&path, &format!("coordinator-split-truncate-regrow-{large}-{size}"));
    }
}

#[test]
fn split_truncate_of_open_unlinked_inode_retains_handle_until_final_close() {
    let Some(path) = fixture() else { return };
    for large in [false, true] {
        let name = b"system/open-split-truncate";
        let mut mounted = mount_fixture(&path);
        let free_before_file = ext4::free_bytes(&mounted).unwrap();
        ext4::create_file_probe(&mut mounted, name, 0o640).unwrap();
        ext4::transaction_probe(&mut mounted, name, 0, &[0x53; 8192]).unwrap();
        let inode = ext4::stat(&mounted, name).unwrap().inode;
        ext4::sync(&mut mounted).unwrap();
        drop(mounted);
        let image = path.with_extension(format!("coordinator-open-truncate-input-{large}.img"));
        DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
        if large {
            debugfs(&image, "fallocate /system/open-split-truncate 0 8193");
            debugfs(&image, &format!("set_inode_field /system/open-split-truncate size {}", 8194u64 * 4096));
        }
        let initial = std::fs::read(&image).unwrap();
        let prepare = || {
            let mut mounted = mount_bytes(initial.clone());
            ext4::unlink_file_guarded(&mut mounted, name, &[inode]).unwrap();
            if !large { ext4::set_stage_block_limit(&mut mounted, 4).unwrap(); }
            DEVICE.with_borrow_mut(|device| device.events.clear());
            mounted
        };
        let check = |mounted: &ext4::Mounted| {
            assert_eq!(ext4::stat(mounted, name), Err(Status::NotFound));
            let metadata = ext4::stat_inode(mounted, inode).unwrap();
            assert_eq!((metadata.inode, metadata.links, metadata.size), (inode, 0, 17));
            assert_eq!(ext4::free_bytes(mounted), Ok(free_before_file - 4096));
            let mut content = [0; 17];
            assert_eq!(ext4::pread_inode(mounted, inode, 0, &mut content), Ok(17));
            assert_eq!(content, [0x53; 17]);
            assert!(ext4::unmount(mounted).is_err());
        };
        let mut reference = prepare();
        let orphan_bytes = DEVICE.with_borrow(|device| device.bytes.clone());
        ext4::truncate_inode(&mut reference, inode, 17).unwrap();
        check(&reference);
        let trace = DEVICE.with_borrow(|device| device.events.clone());
        assert!(trace.iter().filter(|event| **event == Event::Flush(3)).count() >= 2);
        ext4::sync_with_open_inodes(&mut reference, &[inode, inode]).unwrap();
        check(&reference);
        let retained_bytes = DEVICE.with_borrow(|device| device.bytes.clone());
        ext4::sync(&mut reference).unwrap();
        assert_eq!(ext4::stat_inode(&reference, inode), Err(Status::NotFound));
        assert_eq!(ext4::free_bytes(&reference), Ok(free_before_file));
        ext4::unmount(&reference).unwrap();
        drop(reference);
        for accept in [false, true] {
            for fail in 0..trace.len() {
                let mut mounted = prepare();
                DEVICE.with_borrow_mut(|device| {
                    device.fail_event = Some(fail); device.accept_failed_write = accept;
                });
                assert_eq!(ext4::truncate_inode(&mut mounted, inode, 17), Err(Status::Io));
                assert_public_reads_refused(&mounted);
                assert_eq!(ext4::truncate_inode(&mut mounted, inode, 18), Err(Status::Invalid));
                DEVICE.with_borrow_mut(|device| {
                    assert_eq!(device.events, trace[..=fail]);
                    device.events.clear(); device.fail_event = None;
                });
                if accept { ext4::sync_with_open_inodes(&mut mounted, &[inode]).unwrap(); }
                else { ext4::truncate_inode(&mut mounted, inode, 17).unwrap(); }
                let start = trace[..fail].iter().rposition(|event|
                    matches!(event, Event::Flush(4 | 5))).map_or(0, |index| index + 1);
                DEVICE.with_borrow(|device| {
                    assert_eq!(device.events, trace[start..]);
                    assert!(device.bytes == retained_bytes);
                });
                check(&mounted);
                ext4::sync(&mut mounted).unwrap();
                assert_eq!(ext4::stat_inode(&mounted, inode), Err(Status::NotFound));
                assert_eq!(ext4::free_bytes(&mounted), Ok(free_before_file));
                ext4::unmount(&mounted).unwrap();
            }
        }
        fsck(&path, &format!("coordinator-open-truncate-retry-{large}"));
        let mut prefix = orphan_bytes;
        for (cut, event) in trace.iter().enumerate() {
            match event {
                Event::Write(start, bytes) => prefix[*start as usize..*start as usize + bytes.len()].copy_from_slice(bytes),
                Event::Flush(_) => {
                    // Open handles do not survive reboot. Recovery releases
                    // the zero-link inode regardless of the truncate prefix.
                    let recovered = mount_bytes(prefix.clone());
                    assert_eq!(ext4::stat_inode(&recovered, inode), Err(Status::NotFound));
                    assert_eq!(ext4::free_bytes(&recovered), Ok(free_before_file));
                    ext4::unmount(&recovered).unwrap();
                    fsck(&path, &format!("coordinator-open-truncate-{large}-cut-{cut}"));
                }
            }
        }
        let mut mounted = prepare();
        ext4::truncate_inode(&mut mounted, inode, 17).unwrap();
        ext4::truncate_inode(&mut mounted, inode, 8192).unwrap();
        let mut content = [0xa5; 8192];
        let mut read = 0;
        while read < content.len() {
            let count = ext4::pread_inode(&mounted, inode, read as u64, &mut content[read..]).unwrap();
            assert!(count > 0, "unexpected orphan EOF at {read}");
            read += count;
        }
        assert_eq!(ext4::pread_inode(&mounted, inode, 8192, &mut [0; 1]), Ok(0));
        assert_eq!(&content[..17], &[0x53; 17]);
        assert!(content[17..].iter().all(|byte| *byte == 0));
        ext4::sync_with_open_inodes(&mut mounted, &[inode]).unwrap();
        assert_eq!(ext4::stat_inode(&mounted, inode).unwrap().size, 8192);
        ext4::sync(&mut mounted).unwrap();
        ext4::unmount(&mounted).unwrap();
        fsck(&path, &format!("coordinator-open-truncate-regrow-{large}"));
    }
}

#[test]
fn freed_checksummed_inode_bodies_do_not_authorize_inode_io() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/freed-inode";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    let inode = ext4::stat(&mounted, name).unwrap().inode;
    ext4::unlink_file_probe(&mut mounted, name).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    let image = path.with_extension("freed-inode.img");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    // Linux can leave mode and a valid checksum in a freed inode. debugfs
    // reconstructs that state without setting its allocation bitmap bit.
    debugfs(&image, &format!("set_inode_field <{inode}> mode 0100600"));
    debugfs(&image, &format!("set_inode_field <{inode}> links_count 0"));
    debugfs(&image, &format!("set_inode_field <{inode}> dtime 1780000000"));
    let mut mounted = mount_fixture(&image);
    fsck(&path, "coordinator-freed-inode-before");
    let before = DEVICE.with_borrow_mut(|device| { device.events.clear(); device.bytes.clone() });
    for number in [inode, u64::from(u32::MAX)] {
        assert_eq!(ext4::stat_inode(&mounted, number), Err(Status::NotFound));
        let mut data = [0xa5; 16];
        assert_eq!(ext4::pread_inode(&mounted, number, 0, &mut data), Err(Status::NotFound));
        assert_eq!(data, [0xa5; 16]);
        assert_eq!(ext4::write_inode(&mut mounted, number, 0, b"stale"), Err(Status::NotFound));
        assert_eq!(ext4::append_inode(&mut mounted, number, b"stale", 16384), Err(Status::NotFound));
        assert_eq!(ext4::truncate_inode(&mut mounted, number, 8192), Err(Status::NotFound));
    }
    DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert!(device.bytes == before); });
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-freed-inode-after");
    drop(mounted);
    // Even plausible link counts and valid directory/inode checksums must
    // not make a dangling name acceptable at mount.
    debugfs(&image, &format!("set_inode_field <{inode}> links_count 1"));
    debugfs(&image, &format!("set_inode_field <{inode}> dtime 0"));
    debugfs(&image, &format!("link <{inode}> /system/stale-name"));
    let hostile = std::fs::read(&image).unwrap();
    let size = hostile.len() as u64;
    DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
    assert!(ext4::mount(1, size).is_err());
    DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert!(device.bytes == hostile); });
    // Dirty mounts must validate the projected namespace before checkpointing
    // or clearing the recovery marker, even with an empty orphan chain.
    debugfs(&image, "set_super_value feature_incompat 0x20c6");
    let hostile = std::fs::read(&image).unwrap();
    DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
    assert!(ext4::mount(1, size).is_err());
    DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert!(device.bytes == hostile); });
}

#[test]
#[cfg(unix)]
fn linux_kernel_mounts_phipia_results_and_recovers_open_replace_cuts() {
    if std::env::var("PHIPIA_EXT4_KERNEL_INTEROP").as_deref() != Ok("1") {
        eprintln!("Linux loop-mount interoperability not requested; no kernel interoperability gate claimed");
        return;
    }
    let Some(path) = fixture() else { panic!("kernel interoperability requires the configured fixture") };
    fn linux(args: &[&str]) -> std::process::Output {
        let output = std::process::Command::new("sudo").arg("-n").args(args).output().unwrap();
        assert!(output.status.success(), "sudo {args:?}: {}", String::from_utf8_lossy(&output.stderr));
        output
    }
    fn kernel_directory_exists(path: &std::path::Path) -> bool {
        // /data/user is deliberately root-owned 0750. Inspect as root and
        // distinguish ENOENT from permission or other errors: Path::is_dir
        // silently turns all of them into false in the unprivileged runner.
        let output = linux(&["python3", "-c", "import os, stat, sys\ntry:\n mode = os.stat(sys.argv[1]).st_mode\nexcept FileNotFoundError:\n print('missing')\nelse:\n assert stat.S_ISDIR(mode), 'expected a directory'\n print('directory')", path.to_str().unwrap()]);
        match output.stdout.as_slice() {
            b"directory\n" => true,
            b"missing\n" => false,
            unexpected => panic!("unexpected kernel directory result: {unexpected:?}"),
        }
    }
    struct LoopMount { directory: PathBuf, active: bool }
    impl LoopMount {
        fn mount(image: &std::path::Path, directory: &std::path::Path) -> Self {
            let mounted = Self { directory: directory.to_path_buf(), active: true };
            linux(&["mount", "-t", "ext4", "-o", "loop,nodev,nosuid,noexec", image.to_str().unwrap(), directory.to_str().unwrap()]);
            mounted
        }
        fn unmount(mut self) {
            linux(&["umount", self.directory.to_str().unwrap()]);
            self.active = false;
        }
    }
    impl Drop for LoopMount {
        fn drop(&mut self) {
            if self.active {
                let _ = std::process::Command::new("sudo").args(["-n", "umount"])
                    .arg(&self.directory).status();
            }
        }
    }
    println!("ext4 kernel interoperability Linux {}", std::fs::read_to_string("/proc/sys/kernel/osrelease").unwrap().trim());
    let directory = path.with_extension("kernel-mount");
    std::fs::create_dir_all(&directory).unwrap();
    let image = path.with_extension("kernel-interop.img");
    let mut mounted = mount_fixture(&path);
    let source = b"system/kernel-source";
    let target = b"data/user/kernel-target";
    ext4::create_file_probe(&mut mounted, source, 0o644).unwrap();
    ext4::create_file_probe(&mut mounted, target, 0o644).unwrap();
    ext4::transaction_probe(&mut mounted, source, 0, b"new-from-phipia").unwrap();
    ext4::transaction_probe(&mut mounted, target, 0, b"old-from-phipia").unwrap();
    ext4::set_xattr(&mut mounted, source, b"user.state", Some(&[b'n'; 601])).unwrap();
    ext4::set_xattr(&mut mounted, target, b"user.state", Some(&[b'o'; 601])).unwrap();
    let source_inode = ext4::stat(&mounted, source).unwrap().inode;
    let target_inode = ext4::stat(&mounted, target).unwrap().inode;
    ext4::sync(&mut mounted).unwrap();
    let mut prefix = DEVICE.with_borrow_mut(|device| { device.events.clear(); device.bytes.clone() });
    ext4::rename_replace_guarded(&mut mounted, source, target, &[source_inode, target_inode]).unwrap();
    let events = DEVICE.with_borrow(|device| device.events.clone());
    drop(mounted); // Simulated process/OS loss: the kernel must clean the orphan.
    let mut committed = false;
    for (index, event) in events.iter().enumerate() {
        match event {
            Event::Write(start, data) => prefix[*start as usize..*start as usize + data.len()].copy_from_slice(data),
            Event::Flush(boundary) => {
                committed |= *boundary == 3;
                std::fs::write(&image, &prefix).unwrap();
                let kernel = LoopMount::mount(&image, &directory);
                let output = linux(&["cat", directory.join("data/user/kernel-target").to_str().unwrap()]);
                assert_eq!(&output.stdout, if committed { b"new-from-phipia" } else { b"old-from-phipia" });
                linux(&["python3", "-c", "import os,sys; assert os.getxattr(sys.argv[1], 'user.state') == sys.argv[2].encode()*601",
                    directory.join("data/user/kernel-target").to_str().unwrap(), if committed { "n" } else { "o" }]);
                assert_eq!(directory.join("system/kernel-source").exists(), !committed);
                kernel.unmount();
                let recovered = mount_fixture(&image);
                assert_eq!(ext4::stat(&recovered, target).unwrap().inode, if committed { source_inode } else { target_inode });
                if committed { assert!(ext4::stat_inode(&recovered, target_inode).is_err()); }
                ext4::unmount(&recovered).unwrap();
                fsck(&path, &format!("coordinator-kernel-replace-cut-{index}"));
            }
        }
    }
    // Kernel-created data must remain mutable after Phipia remounts it.
    let payload = path.with_extension("kernel-payload");
    std::fs::write(&payload, b"written-by-linux").unwrap();
    let kernel = LoopMount::mount(&image, &directory);
    let kernel_file = directory.join("system/from-linux");
    linux(&["cp", payload.to_str().unwrap(), kernel_file.to_str().unwrap()]);
    linux(&["ln", kernel_file.to_str().unwrap(), directory.join("system/linux-alias").to_str().unwrap()]);
    linux(&["python3", "-c", "import os,sys; os.setxattr(sys.argv[1], 'user.kernel', b'k'*701); os.setxattr(sys.argv[1], 'user.small', b'inline')",
        kernel_file.to_str().unwrap()]);
    kernel.unmount();
    let mut mounted = mount_fixture(&image);
    let mut data = [0; 16];
    read_exact(&mounted, b"system/from-linux", &mut data);
    assert_eq!(&data, b"written-by-linux");
    let mut attribute = [0; 701];
    assert_eq!(ext4::get_xattr(&mounted, b"system/linux-alias", b"user.kernel", &mut attribute), Ok(701));
    assert_eq!(attribute, [b'k'; 701]);
    ext4::set_xattr(&mut mounted, b"system/linux-alias", b"user.kernel", Some(&[b'p'; 3011])).unwrap();
    ext4::set_xattr(&mut mounted, b"system/from-linux", b"user.z", Some(&[b'z'; 701])).unwrap();
    ext4::append_probe(&mut mounted, b"system/linux-alias", b"+phipia", 16384).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-kernel-write-roundtrip");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    let kernel = LoopMount::mount(&image, &directory);
    let output = linux(&["cat", kernel_file.to_str().unwrap()]);
    assert_eq!(&output.stdout, b"written-by-linux+phipia");
    linux(&["python3", "-c", "import os,sys; p=sys.argv[1]; assert os.getxattr(p, 'user.kernel') == b'p'*3011; assert os.getxattr(p, 'user.z') == b'z'*701; assert os.getxattr(p, 'user.small') == b'inline'; os.setxattr(p, 'user.kernel', b'l'*903)",
        kernel_file.to_str().unwrap()]);
    kernel.unmount();
    let mut mounted = mount_fixture(&image);
    let mut updated = [0; 903];
    assert_eq!(ext4::get_xattr(&mounted, b"system/from-linux", b"user.kernel", &mut updated), Ok(903));
    assert_eq!(updated, [b'l'; 903]);
    ext4::set_xattr(&mut mounted, b"system/from-linux", b"user.kernel", None).unwrap();
    ext4::set_xattr(&mut mounted, b"system/from-linux", b"user.z", None).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-kernel-xattr-roundtrip");

    for replace in [false, true] {
        let mut mounted = mount_fixture(&path);
        let source = b"system/kernel-directory";
        let target = b"data/user/kernel-directory";
        ext4::create_directory_probe(&mut mounted, target).unwrap();
        let target_inode = ext4::stat(&mounted, target).unwrap().inode;
        let source_inode = if replace {
            ext4::create_directory_probe(&mut mounted, source).unwrap();
            ext4::stat(&mounted, source).unwrap().inode
        } else { 0 };
        ext4::sync(&mut mounted).unwrap();
        let mut prefix = DEVICE.with_borrow_mut(|device| { device.events.clear(); device.bytes.clone() });
        if replace {
            ext4::rename_replace_guarded(&mut mounted, source, target, &[source_inode, target_inode]).unwrap();
        } else {
            ext4::remove_directory_guarded(&mut mounted, target, &[target_inode]).unwrap();
        }
        let events = DEVICE.with_borrow(|device| device.events.clone());
        drop(mounted);
        let mut committed = false;
        for (index, event) in events.iter().enumerate() {
            match event {
                Event::Write(start, data) => prefix[*start as usize..*start as usize + data.len()].copy_from_slice(data),
                Event::Flush(boundary) => {
                    committed |= *boundary == 3;
                    std::fs::write(&image, &prefix).unwrap();
                    let kernel = LoopMount::mount(&image, &directory);
                    assert_eq!(kernel_directory_exists(&directory.join("data/user/kernel-directory")), !committed || replace,
                        "target directory, replace={replace}, cut={index}");
                    assert_eq!(kernel_directory_exists(&directory.join("system/kernel-directory")), replace && !committed,
                        "source directory, replace={replace}, cut={index}");
                    kernel.unmount();
                    let recovered = mount_fixture(&image);
                    if committed {
                        assert_eq!(ext4::stat_inode(&recovered, target_inode), Err(Status::NotFound));
                        if replace { assert_eq!(ext4::stat(&recovered, target).unwrap().inode, source_inode); }
                    } else {
                        assert_eq!(ext4::stat(&recovered, target).unwrap().inode, target_inode);
                    }
                    ext4::unmount(&recovered).unwrap();
                    fsck(&path, &format!("coordinator-kernel-directory-replace-{replace}-cut-{index}"));
                }
            }
        }
    }
}

#[test]
fn replacing_open_files_preserves_both_inode_handles_and_delays_target_reuse() {
    let Some(path) = fixture() else { return };
    for same_parent in [true, false] {
        let mut mounted = mount_fixture(&path);
        let source = b"system/replace-source";
        let target = if same_parent { b"system/replace-target".as_slice() } else { b"data/user/replace-target" };
        ext4::create_file_probe(&mut mounted, source, 0o600).unwrap();
        ext4::create_file_probe(&mut mounted, target, 0o600).unwrap();
        let source_inode = ext4::stat(&mounted, source).unwrap().inode;
        let target_inode = ext4::stat(&mounted, target).unwrap().inode;
        ext4::write_inode(&mut mounted, source_inode, 0, b"source").unwrap();
        ext4::write_inode(&mut mounted, target_inode, 0, b"target").unwrap();
        let held = [source_inode, target_inode, target_inode];
        ext4::rename_replace_guarded(&mut mounted, source, target, &held).unwrap();
        assert_eq!(ext4::stat(&mounted, source), Err(Status::NotFound));
        assert_eq!(ext4::stat(&mounted, target).unwrap().inode, source_inode);
        assert_eq!(ext4::stat_inode(&mounted, target_inode).unwrap().links, 0);
        ext4::sync_with_open_inodes(&mut mounted, &held).unwrap();
        ext4::create_file_probe(&mut mounted, source, 0o600).unwrap();
        assert_ne!(ext4::stat(&mounted, source).unwrap().inode, target_inode);
        ext4::append_inode(&mut mounted, target_inode, b"-held", 16384).unwrap();
        ext4::write_inode(&mut mounted, source_inode, 0, b"SOURCE").unwrap();
        let mut old = [0; 11];
        assert_eq!(ext4::pread_inode(&mounted, target_inode, 0, &mut old).unwrap(), 11);
        assert_eq!(&old, b"target-held");
        let mut current = [0; 6];
        read_exact(&mounted, target, &mut current);
        assert_eq!(&current, b"SOURCE");
        ext4::sync_with_open_inodes(&mut mounted, &[source_inode, target_inode]).unwrap();
        assert_eq!(ext4::stat_inode(&mounted, target_inode).unwrap().size, 11);
        ext4::sync_with_open_inodes(&mut mounted, &[source_inode]).unwrap();
        assert!(ext4::stat_inode(&mounted, target_inode).is_err());
        ext4::create_file_probe(&mut mounted, b"system/reused-target", 0o600).unwrap();
        assert_eq!(ext4::stat(&mounted, b"system/reused-target").unwrap().inode, target_inode);
        ext4::sync(&mut mounted).unwrap();
        ext4::unmount(&mounted).unwrap();
        fsck(&path, if same_parent { "coordinator-open-replace-same" } else { "coordinator-open-replace-cross" });
    }
    let mut mounted = mount_fixture(&path);
    ext4::create_directory_probe(&mut mounted, b"system/replace-dir").unwrap();
    ext4::create_directory_probe(&mut mounted, b"data/user/replace-dir").unwrap();
    let target = ext4::stat(&mounted, b"data/user/replace-dir").unwrap().inode;
    ext4::sync(&mut mounted).unwrap();
    let snapshot = ext4::directory_snapshot(&mounted, b"data/user/replace-dir").unwrap();
    ext4::rename_replace_guarded(&mut mounted, b"system/replace-dir", b"data/user/replace-dir", &[target]).unwrap();
    ext4::sync_with_open_inodes(&mut mounted, &[target]).unwrap();
    assert_eq!(ext4::stat_inode(&mounted, target).unwrap().links, 0);
    assert_eq!(snapshot.metadata.inode, target);
    assert!(snapshot.entry(0).is_none());
    ext4::create_directory_probe(&mut mounted, b"system/replace-dir").unwrap();
    assert_ne!(ext4::stat(&mounted, b"system/replace-dir").unwrap().inode, target);
    drop(snapshot);
    ext4::sync(&mut mounted).unwrap();
    assert_eq!(ext4::stat_inode(&mounted, target), Err(Status::NotFound));
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-replace-open-directory");
}

#[test]
fn removed_open_directory_keeps_snapshot_and_inode_until_last_close() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/snapshot-dir";
    ext4::create_directory_probe(&mut mounted, name).unwrap();
    ext4::create_file_probe(&mut mounted, b"system/snapshot-dir/old", 0o600).unwrap();
    let snapshot = ext4::directory_snapshot(&mounted, name).unwrap();
    let inode = snapshot.metadata.inode;
    ext4::unlink_file_probe(&mut mounted, b"system/snapshot-dir/old").unwrap();
    ext4::remove_directory_guarded(&mut mounted, name, &[inode, inode]).unwrap();
    ext4::sync_with_open_inodes(&mut mounted, &[inode]).unwrap();
    assert_eq!(ext4::stat(&mounted, name), Err(Status::NotFound));
    assert_eq!(ext4::stat_inode(&mounted, inode).unwrap().links, 0);
    let entry = snapshot.entry(0).unwrap();
    assert_eq!(&entry.name[..entry.name_length as usize], b"old");
    assert!(snapshot.entry(1).is_none());
    ext4::create_directory_probe(&mut mounted, name).unwrap();
    assert_ne!(ext4::stat(&mounted, name).unwrap().inode, inode);
    let crash = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(snapshot);
    ext4::sync(&mut mounted).unwrap();
    assert_eq!(ext4::stat_inode(&mounted, inode), Err(Status::NotFound));
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-rmdir-open-close");
    drop(mounted);
    let recovered = mount_bytes(crash);
    assert_ne!(ext4::stat(&recovered, name).unwrap().inode, inode);
    assert_eq!(ext4::stat_inode(&recovered, inode), Err(Status::NotFound));
    ext4::unmount(&recovered).unwrap();
    fsck(&path, "coordinator-rmdir-open-recovery");
}

#[test]
fn short_directory_entries_grow_without_consuming_checksum_tails() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/short-names").unwrap();
    for index in 0..340 {
        let name = format!("system/short-names/{index:03}");
        ext4::create_file_probe(&mut mounted, name.as_bytes(), 0o600).unwrap();
    }
    assert_eq!(ext4::stat(&mounted, b"system/short-names").unwrap().size, 8192);
    ext4::sync(&mut mounted).unwrap();
    fsck(&path, "coordinator-short-name-growth");
    for index in (0..340).rev() {
        let name = format!("system/short-names/{index:03}");
        ext4::unlink_file_probe(&mut mounted, name.as_bytes()).unwrap();
    }
    assert_eq!(ext4::stat(&mounted, b"system/short-names").unwrap().size, 4096);
    ext4::remove_directory_probe(&mut mounted, b"system/short-names").unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-short-name-shrink");
}

#[test]
fn linear_directory_shrink_reclaims_previously_emptied_suffix_with_retries() {
    let Some(path) = fixture() else { return };
    let directory = b"system/suffix-dir";
    let names: Vec<String> = (0..33).map(|index| format!("system/suffix-dir/{index:03}{}", "x".repeat(252))).collect();
    let mut mounted = mount_fixture(&path);
    ext4::create_directory_probe(&mut mounted, directory).unwrap();
    for name in &names { ext4::create_file_probe(&mut mounted, name.as_bytes(), 0o600).unwrap(); }
    ext4::transaction_probe(&mut mounted, names[0].as_bytes(), 0, b"retained").unwrap();
    assert_eq!(ext4::stat(&mounted, directory).unwrap().size, 3 * 4096);
    let snapshot = ext4::directory_snapshot(&mounted, directory).unwrap();
    // Empty the middle block before the final block becomes empty. Deleting
    // just the last block used to leave an unnecessary empty suffix behind.
    for name in &names[1..32] { ext4::unlink_file_probe(&mut mounted, name.as_bytes()).unwrap(); }
    assert_eq!(ext4::stat(&mounted, directory).unwrap().size, 3 * 4096);
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    let baseline = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let mut mounted = mount_bytes(baseline.clone());
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::unlink_file_probe(&mut mounted, names[32].as_bytes()).unwrap();
    let operations = DEVICE.with_borrow(|device| device.events.clone());
    assert_eq!(ext4::stat(&mounted, directory).unwrap().size, 4096);
    assert_eq!(ext4::free_bytes(&mounted).unwrap(), free + 8192);
    // The already-open snapshot keeps all its names despite reclaimed blocks.
    for index in 0..33 { assert!(snapshot.entry(index).is_some()); }
    assert!(snapshot.entry(33).is_none());
    drop(snapshot);
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    let final_bytes = DEVICE.with_borrow(|device| device.bytes.clone());
    fsck(&path, "coordinator-directory-suffix-final");
    drop(mounted);
    for failure in 0..operations.len() {
        for accepted in [false, true] {
            let mut mounted = mount_bytes(baseline.clone());
            DEVICE.with_borrow_mut(|device| {
                device.fail_event = Some(failure);
                device.accept_failed_write = accepted;
            });
            assert_eq!(ext4::unlink_file_probe(&mut mounted, names[32].as_bytes()), Err(Status::Io));
            let crash = DEVICE.with_borrow(|device| {
                assert_eq!(&device.events, &operations[..=failure]);
                device.bytes.clone()
            });
            DEVICE.with_borrow_mut(|device| device.fail_event = None);
            ext4::unlink_file_probe(&mut mounted, names[32].as_bytes()).unwrap();
            assert_eq!(ext4::stat(&mounted, directory).unwrap().size, 4096);
            ext4::sync(&mut mounted).unwrap();
            ext4::unmount(&mounted).unwrap();
            DEVICE.with_borrow(|device| assert_eq!(device.bytes, final_bytes));
            drop(mounted);
            let recovered = mount_bytes(crash);
            let removed = ext4::stat(&recovered, names[32].as_bytes()) == Err(Status::NotFound);
            assert_eq!(ext4::stat(&recovered, directory).unwrap().size, if removed { 4096 } else { 3 * 4096 });
            let mut content = [0; 8];
            read_exact(&recovered, names[0].as_bytes(), &mut content);
            assert_eq!(&content, b"retained");
            ext4::unmount(&recovered).unwrap();
            fsck(&path, &format!("coordinator-directory-suffix-cut-{failure}-{accepted}"));
        }
    }
}

#[test]
fn live_directories_require_whole_blocks_and_initialized_extents() {
    let Some(path) = fixture() else { return };
    let pristine = std::fs::read(&path).unwrap();
    let raw = ext4plus::Ext4::load(Box::new(pristine.clone())).unwrap();
    let image = path.with_extension("coordinator-directory-map-fault.img");
    for name in ["/", "/indexed"] {
        let node = raw.path_to_inode(ext4plus::path::Path::try_from(name).unwrap(),
            ext4plus::FollowSymlinks::All).unwrap();
        let number = node.index.get();
        let ipg = u32::from_le_bytes(pristine[1064..1068].try_into().unwrap());
        let descriptor = 4096 + ((number - 1) / ipg) as usize * 64;
        let table = u32::from_le_bytes(pristine[descriptor + 8..descriptor + 12].try_into().unwrap()) as usize * 4096;
        let start = table + ((number - 1) % ipg) as usize * 256;
        assert_eq!(u16::from_le_bytes(pristine[start + 0x2e..start + 0x30].try_into().unwrap()), 0);
        let length_and_high = u32::from_le_bytes(pristine[start + 0x38..start + 0x3c].try_into().unwrap());
        assert!(length_and_high & 0xffff > 0 && length_and_high & 0xffff < 0x8000);
        for (field, value) in [("block[4]", u64::from(length_and_high | 0x8000)),
            ("size", 0), ("size", 1), ("size", node.size_in_bytes() - 1)] {
            for dirty in [false, true] {
                write_sparse_fixture(&image, &pristine).unwrap();
                debugfs(&image, &format!("set_inode_field <{number}> {field} {value}"));
                if dirty { debugfs(&image, "feature needs_recovery"); }
                let hostile = std::fs::read(&image).unwrap();
                assert_ne!(&hostile[start..start + 256], &pristine[start..start + 256]);
                let view = ext4plus::Ext4::load(Box::new(hostile.clone())).unwrap();
                ext4plus::inode::Inode::read(&view, node.index).unwrap(); // valid inode CRC
                DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
                assert!(ext4::mount(1, hostile.len() as u64).is_err(), "{name} {field}={value}, dirty={dirty}");
                DEVICE.with_borrow(|device| { assert!(device.events.is_empty()); assert_eq!(device.bytes, hostile); });
            }
        }
    }
}

#[test]
fn indexed_lookup_io_failure_discards_new_inode_data_and_link_reservations() {
    let Some(path) = fixture() else { return };
    for operation in 0..4 {
        let mut mounted = mount_fixture(&path);
        // Establish the durable recovery marker before injecting the lookup
        // fault, so any following storage write would belong to the mutation.
        ext4::create_file_probe(&mut mounted, b"system/read-fault-anchor", 0o600).unwrap();
        let source = ext4::stat(&mounted, b"system/README.TXT").unwrap();
        let parent = ext4::stat(&mounted, b"indexed").unwrap();
        let free = ext4::free_bytes(&mounted).unwrap();
        let before = DEVICE.with_borrow(|device| device.bytes.clone());
        let raw = ext4plus::Ext4::load(Box::new(before.clone())).unwrap();
        let node = ext4plus::inode::Inode::read(&raw,
            std::num::NonZeroU32::new(parent.inode as u32).unwrap()).unwrap();
        let file = ext4plus::file::File::open_inode(&raw, node).unwrap();
        let root = file.filesystem_block_at_offset(0).unwrap().unwrap();
        DEVICE.with_borrow_mut(|device| {
            device.events.clear();
            device.watched_read_block = Some(root);
            device.watched_reads = 0;
            device.fail_watched_read = Some(1);
        });
        let perform = |mounted: &mut ext4::Mounted| match operation {
            0 => ext4::create_file_probe(mounted, b"indexed/io-target", 0o600),
            1 => ext4::create_directory_probe(mounted, b"indexed/io-target"),
            2 => ext4::symlink_probe(mounted, b"indexed/io-target", &[b'x'; 96]),
            _ => ext4::link_file_probe(mounted, b"system/README.TXT", b"indexed/io-target"),
        };
        assert_eq!(perform(&mut mounted), Err(Status::Io), "operation {operation}");
        DEVICE.with_borrow_mut(|device| {
            assert!(device.watched_reads >= 1);
            assert!(device.events.is_empty(), "lookup failure must not publish any staged allocation");
            assert_eq!(device.bytes, before);
            device.watched_read_block = None;
            device.fail_watched_read = None;
        });
        assert_eq!(ext4::free_bytes(&mounted).unwrap(), free);
        assert_eq!(ext4::stat(&mounted, b"indexed").unwrap(), parent);
        assert_eq!(ext4::stat(&mounted, b"system/README.TXT").unwrap(), source);
        assert_eq!(ext4::stat(&mounted, b"indexed/io-target"), Err(Status::NotFound));
        perform(&mut mounted).unwrap();
        ext4::sync(&mut mounted).unwrap();
        ext4::unmount(&mounted).unwrap();
        fsck(&path, &format!("coordinator-indexed-lookup-io-{operation}"));
    }
}

#[test]
fn indexed_directory_compaction_preserves_names_links_and_replays_every_boundary() {
    let Some(path) = fixture() else { return };
    for sentinel in [false, true] {
        let image = path.with_extension(format!("htree-shrink-input-{sentinel}.img"));
        write_sparse_fixture(&image, &std::fs::read(&path).unwrap()).unwrap();
        if sentinel {
            debugfs(&image, "set_inode_field /indexed links_count 1");
            // Promote the Linux-created htree through a real allocated index
            // block. This exercises collapsing an indirect level as well as
            // removing leaves; debugfs updates allocation/inode checksums.
            let input = std::fs::read(&image).unwrap();
            let raw = ext4plus::Ext4::load(Box::new(input.clone())).unwrap();
            let node = raw.path_to_inode(ext4plus::path::Path::try_from("/indexed").unwrap(), ext4plus::FollowSymlinks::All).unwrap();
            let number = node.index.get();
            let blocks = node.size_in_bytes() / 4096;
            let file = ext4plus::file::File::open_inode(&raw, node).unwrap();
            let root = file.filesystem_block_at_offset(0).unwrap().unwrap() as usize * 4096;
            debugfs(&image, &format!("fallocate /indexed {blocks} {blocks}"));
            // INIT_BEYOND_EOF permits allocation beyond EOF; it does not
            // force initialized extents. Resolve and initialize that exact
            // allocated block before populating it with directory metadata.
            let report = std::process::Command::new("debugfs")
                .args(["-R", &format!("bmap /indexed {blocks}")]).arg(&image).output().unwrap();
            assert!(report.status.success());
            let report = String::from_utf8(report.stdout).unwrap();
            let physical: u64 = report.split_whitespace().next().expect("allocated htree node")
                .parse().expect("debugfs physical block");
            assert_ne!(physical, 0, "{report}");
            debugfs(&image, &format!("bmap /indexed {blocks} {physical}"));
            debugfs(&image, &format!("set_inode_field /indexed size {}", (blocks + 1) * 4096));
            let mut bytes = std::fs::read(&image).unwrap();
            let raw = ext4plus::Ext4::load(Box::new(bytes.clone())).unwrap();
            let node = raw.path_to_inode(ext4plus::path::Path::try_from("/indexed").unwrap(), ext4plus::FollowSymlinks::All).unwrap();
            let file = ext4plus::file::File::open_inode(&raw, node).unwrap();
            assert_eq!(file.filesystem_block_at_offset(blocks * 4096).unwrap(), Some(physical));
            let internal = physical as usize * 4096;
            let count = u16::from_le_bytes(bytes[root + 0x22..root + 0x24].try_into().unwrap()) as usize;
            assert_eq!(u16::from_le_bytes(bytes[root + 0x20..root + 0x22].try_into().unwrap()), 507);
            assert!(count > 1 && count <= 507);
            assert_eq!(bytes[root + 0x1e], 0);
            let indices = bytes[root + 0x20..root + 0x20 + count * 8].to_vec();
            bytes[internal..internal + 4096].fill(0);
            bytes[internal + 4..internal + 6].copy_from_slice(&4096u16.to_le_bytes());
            bytes[internal + 8..internal + 8 + indices.len()].copy_from_slice(&indices);
            bytes[internal + 8..internal + 10].copy_from_slice(&510u16.to_le_bytes());
            bytes[root + 0x1e] = 1;
            bytes[root + 0x22..root + 0x24].copy_from_slice(&1u16.to_le_bytes());
            bytes[root + 0x24..root + 0x28].copy_from_slice(&(blocks as u32).to_le_bytes());
            bytes[root + 0x28..root + 4096].fill(0);
            let ipg = u32::from_le_bytes(bytes[1064..1068].try_into().unwrap());
            let descriptor = 4096 + ((number - 1) / ipg) as usize * 64;
            let table = u32::from_le_bytes(bytes[descriptor + 8..descriptor + 12].try_into().unwrap()) as usize;
            let inode_start = table * 4096 + ((number - 1) % ipg) as usize * 256;
            for (block, used) in [(root, 0x28), (internal, 8 + count * 8)] {
                let mut crc = u32::from_le_bytes(bytes[1648..1652].try_into().unwrap());
                for byte in number.to_le_bytes().iter().chain(&bytes[inode_start + 0x64..inode_start + 0x68])
                    .chain(&bytes[block..block + used]).chain(&[0u8; 8]) {
                    crc ^= u32::from(*byte);
                    for _ in 0..8 { crc = (crc >> 1) ^ (0x82f6_3b78u32 & 0u32.wrapping_sub(crc & 1)); }
                }
                bytes[block + 4092..block + 4096].copy_from_slice(&crc.to_le_bytes());
            }
            write_sparse_fixture(&image, &bytes).unwrap();
        }
        let names: Vec<String> = (0..256).map(|index| format!("indexed/entry-{index:04}-phipia-fixture")).collect();
        let record_bytes = (8 + names[0].split('/').next_back().unwrap().len() + 3) & !3;
        let capacity = (4096 - 12) / record_bytes;
        assert!(capacity + 1 < names.len());
        let mut mounted = mount_fixture(&image);
        let old_size = ext4::stat(&mounted, b"indexed").unwrap().size;
        assert!(old_size > 8192 && old_size <= 64 * 4096);
        fsck(&path, &format!("coordinator-htree-shrink-import-{sentinel}"));
        // Leave exactly one too many names for a single leaf. Empty the target
        // file first so the compaction's free-space delta is directory storage.
        for name in &names[capacity + 1..] { ext4::unlink_file_probe(&mut mounted, name.as_bytes()).unwrap(); }
        ext4::truncate_probe(&mut mounted, names[capacity].as_bytes(), 0).unwrap();
        assert_eq!(ext4::stat(&mounted, b"indexed").unwrap().size, old_size);
        let snapshot = ext4::directory_snapshot(&mounted, b"indexed").unwrap();
        let content_size = ext4::stat(&mounted, names[0].as_bytes()).unwrap().size as usize;
        let mut content = vec![0; content_size];
        read_exact(&mounted, names[0].as_bytes(), &mut content);
        ext4::sync(&mut mounted).unwrap();
        ext4::unmount(&mounted).unwrap();
        fsck(&path, &format!("coordinator-htree-shrink-before-{sentinel}"));
        let baseline = DEVICE.with_borrow(|device| device.bytes.clone());
        drop(mounted);
        // Appending the indirect htree node can also grow the extent mapping
        // beyond the inode root. Compaction frees those mapping blocks too;
        // i_size counts directory blocks only. Admission and fsck independently
        // validate i_blocks against the complete physical allocation census.
        let allocated = |bytes: Vec<u8>| {
            let raw = ext4plus::Ext4::load(Box::new(bytes)).unwrap();
            let inode = raw.path_to_inode(ext4plus::path::Path::try_from("/indexed").unwrap(),
                ext4plus::FollowSymlinks::All).unwrap();
            inode.fs_blocks(&raw).unwrap()
        };
        let old_allocation = allocated(baseline.clone());
        assert!(old_allocation >= old_size / 4096);
        let check = |mounted: &ext4::Mounted, removed: bool| {
            assert_eq!(ext4::stat(mounted, b"indexed").unwrap().size, if removed { 8192 } else { old_size });
            assert_eq!(ext4::stat(mounted, b"indexed").unwrap().links, if sentinel { 1 } else { 2 });
            for name in &names[..capacity] { assert!(ext4::stat(mounted, name.as_bytes()).is_ok(), "{name}"); }
            assert_eq!(ext4::stat(mounted, names[capacity].as_bytes()) == Err(Status::NotFound), removed);
            let mut actual = vec![0; content_size];
            read_exact(mounted, names[0].as_bytes(), &mut actual);
            assert_eq!(actual, content);
        };
        let mut mounted = mount_bytes(baseline.clone());
        let free = ext4::free_bytes(&mounted).unwrap();
        ext4::unlink_file_probe(&mut mounted, names[capacity].as_bytes()).unwrap();
        let operations = DEVICE.with_borrow(|device| device.events.clone());
        check(&mounted, true);
        assert_eq!(allocated(DEVICE.with_borrow(|device| device.bytes.clone())), 2);
        assert_eq!(ext4::free_bytes(&mounted).unwrap(), free + (old_allocation - 2) * 4096);
        assert!(snapshot.entry(capacity as u64).is_some());
        assert!(snapshot.entry(capacity as u64 + 1).is_none());
        drop(snapshot);
        ext4::sync(&mut mounted).unwrap();
        ext4::unmount(&mounted).unwrap();
        let final_bytes = DEVICE.with_borrow(|device| device.bytes.clone());
        fsck(&path, &format!("coordinator-htree-shrink-final-{sentinel}"));
        drop(mounted);
        for failure in 0..operations.len() {
            for accepted in [false, true] {
                let mut mounted = mount_bytes(baseline.clone());
                DEVICE.with_borrow_mut(|device| {
                    device.fail_event = Some(failure);
                    device.accept_failed_write = accepted;
                });
                assert_eq!(ext4::unlink_file_probe(&mut mounted, names[capacity].as_bytes()), Err(Status::Io));
                let crash = DEVICE.with_borrow(|device| {
                    assert_eq!(device.events, operations[..=failure]);
                    device.bytes.clone()
                });
                DEVICE.with_borrow_mut(|device| device.fail_event = None);
                ext4::unlink_file_probe(&mut mounted, names[capacity].as_bytes()).unwrap();
                check(&mounted, true);
                ext4::sync(&mut mounted).unwrap();
                ext4::unmount(&mounted).unwrap();
                DEVICE.with_borrow(|device| assert_eq!(device.bytes, final_bytes));
                drop(mounted);
                let recovered = mount_bytes(crash);
                let removed = ext4::stat(&recovered, names[capacity].as_bytes()) == Err(Status::NotFound);
                check(&recovered, removed);
                ext4::unmount(&recovered).unwrap();
                fsck(&path, &format!("coordinator-htree-shrink-cut-{sentinel}-{failure}-{accepted}"));
            }
        }
        let mut cut = baseline;
        let mut committed = false;
        for (index, operation) in operations.iter().enumerate() {
            match operation {
                Event::Write(start, bytes) => cut[*start as usize..*start as usize + bytes.len()].copy_from_slice(bytes),
                Event::Flush(boundary) => {
                    committed |= *boundary == 3;
                    let image = path.with_extension(format!("htree-shrink-linux-{sentinel}-{index}.img"));
                    write_sparse_fixture(&image, &cut).unwrap();
                    let replay = std::process::Command::new("e2fsck").args(["-E", "journal_only", "-p"])
                        .arg(&image).output().unwrap();
                    let log = format!("{}\n{}", String::from_utf8_lossy(&replay.stdout), String::from_utf8_lossy(&replay.stderr));
                    std::fs::write(image.with_extension("replay.txt"), &log).unwrap();
                    assert!(matches!(replay.status.code(), Some(0 | 1)), "{log}");
                    let recovered = mount_fixture(&image);
                    check(&recovered, committed);
                    ext4::unmount(&recovered).unwrap();
                    fsck(&path, &format!("coordinator-htree-shrink-linux-{sentinel}-{index}"));
                }
            }
        }
        // The compact tree remains writable and can split again. Child mkdir,
        // cross-parent rename and rmdir retain the sentinel where applicable.
        let mut mounted = mount_bytes(final_bytes);
        // One short name consumes the remaining 16 bytes; the next must split
        // the leaf instead of trying to use its 12-byte checksum tail.
        assert_eq!((4096 - 12) % record_bytes, 16);
        ext4::create_file_probe(&mut mounted, b"indexed/a", 0o600).unwrap();
        assert_eq!(ext4::stat(&mounted, b"indexed").unwrap().size, 8192);
        ext4::create_file_probe(&mut mounted, b"indexed/b", 0o600).unwrap();
        assert!(ext4::stat(&mounted, b"indexed").unwrap().size > 8192);
        ext4::unlink_file_probe(&mut mounted, b"indexed/a").unwrap();
        ext4::unlink_file_probe(&mut mounted, b"indexed/b").unwrap();
        check(&mounted, true);
        ext4::create_directory_probe(&mut mounted, b"indexed/child").unwrap();
        ext4::rename_probe(&mut mounted, b"indexed/child", b"system/child").unwrap();
        ext4::rename_probe(&mut mounted, b"system/child", b"indexed/child").unwrap();
        ext4::remove_directory_probe(&mut mounted, b"indexed/child").unwrap();
        for name in &names[capacity..capacity + 8] { ext4::create_file_probe(&mut mounted, name.as_bytes(), 0o600).unwrap(); }
        assert!(ext4::stat(&mounted, b"indexed").unwrap().size > 8192);
        ext4::sync(&mut mounted).unwrap();
        ext4::unmount(&mounted).unwrap();
        fsck(&path, &format!("coordinator-htree-shrink-regrow-{sentinel}"));
    }
}

#[test]
fn indexed_directory_untracked_link_counts_survive_namespace_mutations() {
    let Some(path) = fixture() else { return };
    let image = path.with_extension("dir-nlink.img");
    std::fs::copy(&path, &image).unwrap();
    debugfs(&image, "set_inode_field /indexed links_count 1");
    let mut mounted = mount_fixture(&image);
    fsck(&path, "coordinator-dir-nlink-before");
    let parent = ext4::stat(&mounted, b"system").unwrap();
    ext4::create_directory_probe(&mut mounted, b"indexed/child").unwrap();
    assert_eq!(ext4::stat(&mounted, b"indexed").unwrap().links, 1);
    ext4::rename_probe(&mut mounted, b"indexed/child", b"system/child").unwrap();
    assert_eq!(ext4::stat(&mounted, b"indexed").unwrap().links, 1);
    assert_eq!(ext4::stat(&mounted, b"system").unwrap().links, parent.links + 1);
    ext4::rename_probe(&mut mounted, b"system/child", b"indexed/child").unwrap();
    assert_eq!(ext4::stat(&mounted, b"system").unwrap().links, parent.links);
    assert_eq!(ext4::stat(&mounted, b"indexed").unwrap().links, 1);
    ext4::remove_directory_probe(&mut mounted, b"indexed/child").unwrap();
    assert_eq!(ext4::stat(&mounted, b"indexed").unwrap().links, 1);
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-dir-nlink-after");
}

#[test]
fn indexed_directory_moves_update_dotdot_checksum_and_refuse_hostile_counts() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let inode = ext4::stat(&mounted, b"indexed").unwrap().inode;
    let root_links = ext4::stat(&mounted, b".").unwrap().links;
    let parent = ext4::stat(&mounted, b"data/user").unwrap();
    ext4::rename_probe(&mut mounted, b"indexed", b"data/user/moved-index").unwrap();
    assert_eq!(ext4::stat(&mounted, b"data/user/moved-index").unwrap().inode, inode);
    assert_eq!(ext4::stat(&mounted, b"data/user/moved-index/..").unwrap().inode, parent.inode);
    assert_eq!(ext4::stat(&mounted, b".").unwrap().links, root_links - 1);
    assert_eq!(ext4::stat(&mounted, b"data/user").unwrap().links, parent.links + 1);
    ext4::create_file_probe(&mut mounted, b"data/user/moved-index/new", 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, b"data/user/moved-index/new", 0, b"indexed").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    let disk = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let mut mounted = mount_bytes(disk);
    let mut content = [0; 7];
    read_exact(&mounted, b"data/user/moved-index/new", &mut content);
    assert_eq!(&content, b"indexed");
    ext4::rename_probe(&mut mounted, b"data/user/moved-index", b"indexed").unwrap();
    ext4::unlink_file_probe(&mut mounted, b"indexed/new").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-indexed-directory-move");
    let original = std::fs::read(&path).unwrap();
    let result = std::process::Command::new("debugfs").args(["-R", "bmap /indexed 0"]).arg(&path).output().unwrap();
    assert!(result.status.success());
    let block: usize = String::from_utf8(result.stdout).unwrap().trim().parse().unwrap();
    for (offset, value) in [(0x22, u16::MAX), (0x22, 0), (0x20, u16::MAX)] {
        let mut hostile = original.clone();
        hostile[block * 4096 + offset..block * 4096 + offset + 2].copy_from_slice(&value.to_le_bytes());
        let bytes = hostile.len() as u64;
        DEVICE.with_borrow_mut(|device| *device = Device { bytes: hostile.clone(), ..Device::default() });
        assert!(ext4::mount(1, bytes).is_err());
        DEVICE.with_borrow(|device| {
            assert!(device.events.is_empty());
            assert!(device.bytes == hostile);
        });
    }
}

#[test]
fn final_orphan_release_retries_identical_bytes_without_a_live_handle() {
    for (directory, large, zero_size) in [
        (false, false, false), (true, false, false),
        (false, true, false), (false, true, true),
    ] {
        orphan_release_failure_case(directory, large, zero_size, ReclaimOperation::Finalize);
    }
}

#[test]
fn oversized_quiescent_unlink_retries_namespace_and_reclamation_as_one_request() {
    orphan_release_failure_case(false, false, false, ReclaimOperation::Unlink);
    for zero_size in [false, true] {
        orphan_release_failure_case(false, true, zero_size, ReclaimOperation::Unlink);
    }
}

#[test]
fn oversized_replacement_retries_atomic_namespace_and_old_target_reclamation() {
    for operation in [ReclaimOperation::ReplaceSameParent, ReclaimOperation::ReplaceCrossParent] {
        orphan_release_failure_case(false, false, false, operation);
        orphan_release_failure_case(false, true, false, operation);
    }
}

#[test]
fn oversized_unlink_refuses_out_of_profile_extents_before_namespace_publication() {
    let Some(path) = fixture() else { return };
    let name = b"system/beyond-reclaim";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::sync(&mut mounted).unwrap();
    drop(mounted);
    let image = path.with_extension("coordinator-beyond-reclaim-input.img");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    // Size zero does not bound fallocate(KEEP_SIZE) extents. This allocation
    // exceeds both one transaction's revokes and the split logical profile.
    debugfs(&image, "fallocate /system/beyond-reclaim 16384 24576");
    debugfs(&image, "set_inode_field /system/beyond-reclaim size 0");
    let initial = std::fs::read(&image).unwrap();
    let mut mounted = mount_bytes(initial.clone());
    let metadata = ext4::stat(&mounted, name).unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    fsck(&path, "coordinator-beyond-reclaim-before");
    assert_eq!(ext4::unlink_file_probe(&mut mounted, name), Err(Status::Range));
    assert_eq!(ext4::stat(&mounted, name), Ok(metadata));
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    DEVICE.with_borrow(|device| {
        assert_eq!(device.events.len(), 2);
        assert!(matches!(device.events[0], Event::Write(1024, _)));
        assert_eq!(device.events[1], Event::Flush(0));
    });
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    DEVICE.with_borrow(|device| assert!(device.bytes == initial));
    fsck(&path, "coordinator-beyond-reclaim-refused");
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ReclaimOperation { Finalize, Unlink, ReplaceSameParent, ReplaceCrossParent }

fn orphan_release_failure_case(directory: bool, large: bool, zero_size: bool, operation: ReclaimOperation) {
    let Some(path) = fixture() else { return };
    let mut baseline = mount_fixture(&path);
    let quiescent = operation != ReclaimOperation::Finalize;
    let source: Option<&[u8]> = match operation {
        ReclaimOperation::ReplaceSameParent => Some(b"system/reclaim-source"),
        ReclaimOperation::ReplaceCrossParent => Some(b"data/user/reclaim-source"),
        _ => None,
    };
    let source_metadata = source.map(|source| {
        ext4::create_file_probe(&mut baseline, source, 0o640).unwrap();
        ext4::transaction_probe(&mut baseline, source, 0, b"replacement").unwrap();
        ext4::stat(&baseline, source).unwrap()
    });
    let free_before_file = ext4::free_bytes(&baseline).unwrap();
    let name = b"system/close-retry";
    if directory {
        ext4::create_directory_probe(&mut baseline, name).unwrap();
    } else {
        ext4::create_file_probe(&mut baseline, name, 0o600).unwrap();
        if !zero_size {
            ext4::transaction_probe(&mut baseline, name, 0, &vec![0x53; 8192]).unwrap();
        }
    }
    let inode = ext4::stat(&baseline, name).unwrap().inode;
    ext4::sync(&mut baseline).unwrap();
    let mut initial = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(baseline);
    if large {
        let image = path.with_extension("coordinator-large-orphan-input.img");
        std::fs::write(&image, &initial).unwrap();
        // One more allocated block than a transaction can revoke. Linux
        // creates unwritten extents without constructing a large payload.
        debugfs(&image, "fallocate /system/close-retry 0 8192");
        let size = if zero_size { 0 } else { 8193u64 * 4096 };
        debugfs(&image, &format!("set_inode_field /system/close-retry size {size}"));
        initial = std::fs::read(&image).unwrap();
        let checked = mount_bytes(initial.clone());
        assert_eq!(ext4::stat(&checked, name).unwrap().size, size);
        assert!(free_before_file - ext4::free_bytes(&checked).unwrap() >= 8193 * 4096);
        ext4::unmount(&checked).unwrap();
        fsck(&path, &format!("coordinator-large-orphan-before-zero-{zero_size}"));
    }
    let initial_view = mount_bytes(initial.clone());
    let initial_metadata = ext4::stat(&initial_view, name).unwrap();
    let initial_free = ext4::free_bytes(&initial_view).unwrap();
    drop(initial_view);
    let prepare = || {
        let mut mounted = mount_bytes(initial.clone());
        if quiescent && !large { ext4::set_stage_block_limit(&mut mounted, 5).unwrap(); }
        if !quiescent {
            if directory { ext4::remove_directory_guarded(&mut mounted, name, &[inode]).unwrap(); }
            else { ext4::unlink_file_guarded(&mut mounted, name, &[inode]).unwrap(); }
        }
        DEVICE.with_borrow_mut(|device| device.events.clear());
        mounted
    };
    let perform = |mounted: &mut ext4::Mounted| {
        if let Some(source) = source { ext4::rename_replace_probe(mounted, source, name) }
        else if quiescent { ext4::unlink_file_probe(mounted, name) }
        else { ext4::finalize_orphan(mounted, inode) }
    };
    let assert_reclaimed = |mounted: &ext4::Mounted| {
        assert_eq!(ext4::stat_inode(mounted, inode), Err(Status::NotFound));
        assert_eq!(ext4::free_bytes(mounted), Ok(free_before_file));
        if let Some(source) = source {
            assert_eq!(ext4::stat(mounted, source), Err(Status::NotFound));
            assert_eq!(ext4::stat(mounted, name), Ok(source_metadata.unwrap()));
            let mut content = [0; 11];
            read_exact(mounted, name, &mut content);
            assert_eq!(&content, b"replacement");
        } else { assert_eq!(ext4::stat(mounted, name), Err(Status::NotFound)); }
    };
    let mut reference = prepare();
    let orphan_bytes = DEVICE.with_borrow(|device| device.bytes.clone());
    perform(&mut reference).unwrap();
    let expected = DEVICE.with_borrow(|device| device.events.clone());
    if large || quiescent { assert!(expected.iter().filter(|event| **event == Event::Flush(3)).count() >= 2); }
    assert_reclaimed(&reference);
    ext4::sync(&mut reference).unwrap();
    let final_bytes = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(reference);
    for accept in [false, true] {
        for fail in 0..expected.len() {
            let mut mounted = prepare();
            DEVICE.with_borrow_mut(|device| {
                device.fail_event = Some(fail);
                device.accept_failed_write = accept;
            });
            assert_eq!(perform(&mut mounted), Err(Status::Io));
            if !quiescent || fail >= 2 {
                assert_public_reads_refused(&mounted);
                if quiescent {
                    let events = DEVICE.with_borrow(|device| device.events.len());
                    assert!(ext4::unlink_file_probe(&mut mounted, b"system/README.TXT").is_err());
                    assert!(ext4::create_file_probe(&mut mounted, name, 0o600).is_err());
                    assert!(ext4::truncate_inode(&mut mounted, inode, 0).is_err());
                    if let Some(source) = source {
                        assert!(ext4::rename_probe(&mut mounted, source, name).is_err());
                        assert!(ext4::rename_replace_probe(&mut mounted, name, source).is_err());
                    }
                    DEVICE.with_borrow(|device| assert_eq!(device.events.len(), events));
                }
            }
            DEVICE.with_borrow_mut(|device| {
                assert_eq!(device.events, expected[..=fail]);
                device.events.clear();
                device.fail_event = None;
            });
            let finish_with_sync = quiescent && accept && fail >= 2;
            if finish_with_sync { ext4::sync(&mut mounted).unwrap(); }
            else { perform(&mut mounted).unwrap(); }
            let start = expected[..fail].iter().rposition(|event|
                matches!(event, Event::Flush(0 | 4 | 5))).map_or(0, |index| index + 1);
            DEVICE.with_borrow(|device| {
                if finish_with_sync { assert!(device.events.starts_with(&expected[start..])); }
                else { assert_eq!(device.events, expected[start..]); }
            });
            ext4::sync(&mut mounted).unwrap();
            assert_reclaimed(&mounted);
            ext4::unmount(&mounted).unwrap();
            DEVICE.with_borrow(|device| assert!(device.bytes == final_bytes));
        }
    }
    fsck(&path, &format!("coordinator-orphan-close-retry-directory-{directory}-large-{large}-zero-{zero_size}-{operation:?}"));
    if large || quiescent {
        let mut prefix = orphan_bytes;
        for (index, event) in expected.iter().enumerate() {
            match event {
                Event::Write(start, bytes) => prefix[*start as usize..*start as usize + bytes.len()].copy_from_slice(bytes),
                Event::Flush(_) => {
                    // Reboot every durable prefix, including the middle of
                    // multi-transaction reclamation. No live handle survives.
                    let recovered = mount_bytes(prefix.clone());
                    if quiescent && ext4::stat(&recovered, name) == Ok(initial_metadata) {
                        assert_eq!(ext4::stat(&recovered, name), Ok(initial_metadata));
                        assert_eq!(ext4::free_bytes(&recovered), Ok(initial_free));
                        if let Some(source) = source {
                            assert_eq!(ext4::stat(&recovered, source), Ok(source_metadata.unwrap()));
                            let mut content = [0; 11];
                            read_exact(&recovered, source, &mut content);
                            assert_eq!(&content, b"replacement");
                        }
                        if !zero_size {
                            let mut data = [0; 8192];
                            read_exact(&recovered, name, &mut data);
                            assert_eq!(data, [0x53; 8192]);
                        }
                    } else {
                        assert_reclaimed(&recovered);
                    }
                    ext4::unmount(&recovered).unwrap();
                    fsck(&path, &format!("coordinator-large-orphan-zero-{zero_size}-{operation:?}-cut-{index}"));
                }
            }
        }
    }
}

#[test]
fn staged_orphan_chain_retains_open_data_and_releases_out_of_order() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    for name in [b"system/orphan-a".as_slice(), b"system/orphan-b", b"system/orphan-c"] {
        ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
        ext4::transaction_probe(&mut mounted, name, 0, b"held").unwrap();
    }
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    let backing = DEVICE.with_borrow(|device| device.bytes.clone());
    for rollback in [true, false] {
        let stage = std::rc::Rc::new(ext4plus::JournalMutationStage::new(
            Box::new(backing.clone()), backing.len() as u64).unwrap());
        let raw = ext4plus::Ext4::load_with_writer(Box::new(stage.clone()), Some(Box::new(stage.clone()))).unwrap();
        let parent = raw.path_to_inode(ext4plus::path::Path::try_from("/system").unwrap(),
            ext4plus::FollowSymlinks::All).unwrap();
        let mut directory = ext4plus::dir::Dir::open_inode(&raw, parent).unwrap();
        let mut indices = Vec::new();
        for name in ["orphan-a", "orphan-b", "orphan-c"] {
            let entry = ext4plus::DirEntryName::try_from(name).unwrap();
            let inode = directory.get_entry(entry).unwrap();
            indices.push(inode.index);
            let retained = directory.unlink_open(entry, inode).unwrap().unwrap();
            assert_eq!(retained.links_count(), 0);
        }
        assert_eq!(raw.orphan_inodes().unwrap(), indices.iter().rev().copied().collect::<Vec<_>>());
        assert_eq!(raw.superblock().last_orphan(), indices[2].get());
        DEVICE.with_borrow(|device| assert!(device.bytes == backing, "upstream orphan mutation reached home storage"));
        if rollback {
            drop(directory);
            drop(raw);
            stage.rollback();
            let restored = ext4plus::Ext4::load(Box::new(stage.clone())).unwrap();
            assert!(restored.orphan_inodes().unwrap().is_empty());
            assert!(restored.open(b"/system/orphan-a").is_ok());
            continue;
        }
        // A valid inode CRC must not make a cyclic chain admissible.
        let mut head = ext4plus::inode::Inode::read(&raw, indices[2]).unwrap();
        let next = head.dtime_val();
        head.set_dtime_val(indices[2].get());
        head.write(&raw).unwrap();
        assert!(raw.orphan_inodes().is_err());
        head.set_dtime_val(next);
        head.write(&raw).unwrap();
        let mut held = ext4plus::file::File::open_inode(&raw,
            ext4plus::inode::Inode::read(&raw, indices[0]).unwrap()).unwrap();
        held.write_bytes_at(b"+open", 4).unwrap();
        let mut data = [0; 9];
        assert_eq!(held.read_bytes_at(&mut data, 0).unwrap(), 9);
        assert_eq!(&data, b"held+open");
        drop(held);
        raw.release_orphan(indices[1]).unwrap();
        assert_eq!(raw.orphan_inodes().unwrap(), vec![indices[2], indices[0]]);
        raw.release_orphan(indices[0]).unwrap();
        assert_eq!(raw.orphan_inodes().unwrap(), vec![indices[2]]);
        raw.release_orphan(indices[2]).unwrap();
        assert_eq!(raw.superblock().last_orphan(), 0);
        assert!(raw.orphan_inodes().unwrap().is_empty());
        assert!(stage.revoked_block_count() >= 3);
        // This is an upstream staged-state fixture, not a durability proof.
        // Coordinator admission remains guarded until orphan replay is wired.
        let mut projected = backing.clone();
        for image in stage.staged_images() {
            let start = image.block_index() as usize * 4096;
            projected[start..start + 4096].copy_from_slice(image.bytes());
        }
        DEVICE.with_borrow_mut(|device| device.bytes = projected);
        fsck(&path, "coordinator-staged-orphan-release");
    }
}

#[test]
fn dot_components_walk_symlink_targets_and_preserve_lookup_errors() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let root_mode = ext4::stat(&mounted, b".").unwrap().mode & 0o7777;
    ext4::chmod(&mut mounted, b".", 0o750).unwrap();
    assert_eq!(ext4::stat(&mounted, b".").unwrap().mode & 0o7777, 0o750);
    ext4::chmod(&mut mounted, b".", root_mode).unwrap();
    let unicode = b"system/caf\xc3\xa9";
    ext4::create_file_probe(&mut mounted, unicode, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, unicode, 0, b"utf8").unwrap();
    let mut utf8 = [0; 4];
    read_exact(&mounted, unicode, &mut utf8);
    assert_eq!(&utf8, b"utf8");
    ext4::create_directory_probe(&mut mounted, b"system/walk-target").unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/walk-target/deep").unwrap();
    ext4::symlink_probe(&mut mounted, b"system/walk-link", b"walk-target/deep").unwrap();
    let alias = b"system/walk-link/./../new";
    let actual = b"system/walk-target/new";
    ext4::create_file_probe(&mut mounted, alias, 0o600).unwrap();
    assert_eq!(ext4::stat(&mounted, alias), ext4::stat(&mounted, actual));
    assert_eq!(ext4::stat(&mounted, b"system/new"), Err(Status::NotFound));
    ext4::transaction_probe(&mut mounted, alias, 0, b"walked").unwrap();
    let mut bytes = [0; 6];
    read_exact(&mounted, actual, &mut bytes);
    assert_eq!(&bytes, b"walked");
    assert_eq!(ext4::create_file_probe(&mut mounted, b"system/missing/../bad", 0o600), Err(Status::NotFound));
    assert_eq!(ext4::create_file_probe(&mut mounted, b"system/walk-target/new/../bad", 0o600), Err(Status::NotDirectory));
    assert_eq!(ext4::stat(&mounted, b"system/walk-target/new/."), Err(Status::NotDirectory));
    ext4::link_file_probe(&mut mounted, alias, b"system/walk-copy").unwrap();
    ext4::rename_probe(&mut mounted, alias, b"system/walk-link/../renamed").unwrap();
    assert_eq!(ext4::stat(&mounted, actual), Err(Status::NotFound));
    ext4::unlink_file_probe(&mut mounted, b"system/walk-link/../renamed").unwrap();
    assert_eq!(ext4::stat(&mounted, b"system/walk-copy").unwrap().links, 1);
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-dot-components");
}

#[test]
fn append_only_inodes_admit_appends_and_new_names_but_refuse_destructive_changes() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/append-only";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, name, 0, b"original").unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/append-dir").unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/append-empty").unwrap();
    ext4::create_file_probe(&mut mounted, b"system/append-dir/child", 0o600).unwrap();
    ext4::create_file_probe(&mut mounted, b"system/ordinary", 0o600).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    let image = path.with_extension("coordinator-append-only-input.img");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    for entry in ["append-only", "append-dir", "append-empty"] {
        debugfs(&image, &format!("set_inode_field /system/{entry} flags 0x80020"));
    }
    let mut mounted = mount_fixture(&image);
    let inode = ext4::stat(&mounted, name).unwrap().inode;
    let baseline = DEVICE.with_borrow(|device| device.bytes.clone());
    for case in 0..16 {
        let result = match case {
            0 => ext4::transaction_probe(&mut mounted, name, 0, b"overwrite").map(|_| ()),
            1 => ext4::write_inode(&mut mounted, inode, 8, b"non-append-mode").map(|_| ()),
            2 => ext4::truncate_probe(&mut mounted, name, 0),
            3 => ext4::truncate_inode(&mut mounted, inode, 8), // same-size still refused
            4 => ext4::truncate_inode(&mut mounted, inode, 8192),
            5 => ext4::unlink_file_probe(&mut mounted, name),
            6 => ext4::link_file_probe(&mut mounted, name, b"system/alias"),
            7 => ext4::rename_probe(&mut mounted, name, b"system/renamed"),
            8 => ext4::rename_replace_probe(&mut mounted, b"system/ordinary", name),
            9 => ext4::chmod(&mut mounted, name, 0o777),
            10 => ext4::set_times(&mut mounted, name, 1780000001, 0, 1780000001, 0),
            11 => ext4::set_xattr(&mut mounted, name, b"user.note", Some(b"forbidden")),
            12 => ext4::unlink_file_probe(&mut mounted, b"system/append-dir/child"),
            13 => ext4::remove_directory_probe(&mut mounted, b"system/append-empty"),
            14 => ext4::rename_probe(&mut mounted, b"system/append-dir/child", b"system/moved"),
            _ => ext4::rename_replace_probe(&mut mounted, b"system/ordinary", b"system/append-dir/child"),
        };
        assert_eq!(result, Err(Status::ReadOnly), "append-only case {case}");
        ext4::sync(&mut mounted).unwrap();
        DEVICE.with_borrow(|device| assert!(device.bytes == baseline, "append-only case {case} escaped rollback"));
    }
    assert_eq!(ext4::append_probe(&mut mounted, name, b"+", u64::MAX), Ok((8, 1)));
    let payload = vec![0x5a; 40 * 4096];
    assert_eq!(ext4::append_inode(&mut mounted, inode, &payload, u64::MAX), Ok((9, payload.len())));
    let mut content = vec![0; 9 + payload.len()];
    read_exact(&mounted, name, &mut content);
    assert_eq!(&content[..9], b"original+");
    assert_eq!(&content[9..], &payload);
    ext4::create_file_probe(&mut mounted, b"system/append-dir/new", 0o600).unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/append-dir/new-dir").unwrap();
    ext4::symlink_probe(&mut mounted, b"system/append-dir/new-link", b"missing").unwrap();
    ext4::link_file_probe(&mut mounted, b"system/ordinary", b"system/append-dir/alias").unwrap();
    ext4::rename_probe(&mut mounted, b"system/ordinary", b"system/append-dir/moved-in").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-append-only");
    drop(mounted);
    let mounted = mount_bytes(DEVICE.with_borrow(|device| device.bytes.clone()));
    let mut content = vec![0; 9 + payload.len()];
    read_exact(&mounted, name, &mut content);
    assert_eq!(&content[..9], b"original+");
    assert_eq!(&content[9..], &payload);
}

#[test]
fn immutable_namespace_refusals_discard_allocations_and_link_counts() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    for name in [b"system/protected".as_slice(), b"system/ordinary"] {
        ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    }
    ext4::transaction_probe(&mut mounted, b"system/protected", 0, b"protected").unwrap();
    for name in [b"system/frozen".as_slice(), b"system/frozen-empty"] {
        ext4::create_directory_probe(&mut mounted, name).unwrap();
    }
    ext4::create_file_probe(&mut mounted, b"system/frozen/child", 0o600).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    let image = path.with_extension("coordinator-immutable-input.img");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    for name in ["protected", "frozen", "frozen-empty"] {
        debugfs(&image, &format!("set_inode_field /system/{name} flags 0x80010"));
    }
    let mut mounted = mount_fixture(&image);
    ext4::sync(&mut mounted).unwrap();
    let baseline = DEVICE.with_borrow(|device| device.bytes.clone());
    let free = ext4::free_bytes(&mounted).unwrap();
    for case in 0..13 {
        let result = match case {
            0 => ext4::link_file_probe(&mut mounted, b"system/protected", b"system/alias"),
            1 => ext4::unlink_file_probe(&mut mounted, b"system/protected"),
            2 => ext4::create_file_probe(&mut mounted, b"system/frozen/new", 0o600),
            3 => ext4::create_directory_probe(&mut mounted, b"system/frozen/new-dir"),
            4 => ext4::symlink_probe(&mut mounted, b"system/frozen/new-link", b"missing"),
            5 => ext4::link_file_probe(&mut mounted, b"system/ordinary", b"system/frozen/new"),
            6 => ext4::unlink_file_probe(&mut mounted, b"system/frozen/child"),
            7 => ext4::remove_directory_probe(&mut mounted, b"system/frozen-empty"),
            8 => ext4::rename_probe(&mut mounted, b"system/protected", b"system/moved"),
            9 => ext4::rename_probe(&mut mounted, b"system/frozen/child", b"system/moved"),
            10 => ext4::rename_probe(&mut mounted, b"system/ordinary", b"system/frozen/new"),
            11 => ext4::rename_replace_probe(&mut mounted, b"system/ordinary", b"system/protected"),
            _ => ext4::chmod(&mut mounted, b"system/protected", 0o777),
        };
        assert_eq!(result, Err(Status::ReadOnly), "immutable case {case}");
        assert_eq!(ext4::free_bytes(&mounted), Ok(free));
        ext4::sync(&mut mounted).unwrap();
        // Also proves rollback of inode reservations, parent times, and source
        // link counts: only the temporary recovery marker may have been written.
        DEVICE.with_borrow(|device| assert!(device.bytes == baseline, "immutable case {case} changed disk"));
    }
    ext4::create_file_probe(&mut mounted, b"system/after-refusal", 0o600).unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-immutable-namespace");
}

#[test]
fn real_enospc_rolls_back_allocations_and_immutable_truncate_is_readonly() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/full-test";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::create_file_probe(&mut mounted, b"system/immutable-test", 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, b"system/immutable-test", 0, b"protected").unwrap();
    ext4::sync(&mut mounted).unwrap();
    let free_blocks = ext4::free_bytes(&mounted).unwrap() / 4096;
    ext4::unmount(&mounted).unwrap();
    drop(mounted);
    let image = path.with_extension("coordinator-full-input.img");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    let filler = path.with_extension("coordinator-filler.bin");
    // Leave a few blocks so the failing request allocates before ENOSPC. The
    // reserve also covers debugfs's own extent metadata, checked after mounting.
    std::fs::write(&filler, vec![0x5a; (free_blocks as usize - 48) * 4096]).unwrap();
    debugfs(&image, &format!("write \"{}\" /system/filler", filler.display()));
    debugfs(&image, "set_inode_field /system/immutable-test flags 0x80010");
    let mut mounted = mount_fixture(&image);
    let available = ext4::free_bytes(&mounted).unwrap();
    assert!(available > 32 * 4096 && available < 64 * 4096);
    // One chunk fits, the second must roll back and return a durable short write.
    let written = ext4::transaction_probe(&mut mounted, name, 0, &vec![0x52; 64 * 4096]).unwrap();
    assert_eq!(written, 32 * 4096);
    assert_eq!(ext4::stat(&mounted, name).unwrap().size, written as u64);
    let mut tail = [0xa5; 1];
    assert_eq!(ext4::pread(&mounted, name, written as u64, &mut tail), Ok(0));
    let free = ext4::free_bytes(&mounted).unwrap();
    assert!(free > 0 && free < 32 * 4096, "fixture did not reach low space: {free}");
    let original = ext4::stat(&mounted, name).unwrap();
    assert_eq!(ext4::transaction_probe(&mut mounted, name, written as u64, &vec![0x52; 32 * 4096]),
        Err(Status::Full));
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    assert_eq!(ext4::stat(&mounted, name), Ok(original));
    DEVICE.with_borrow_mut(|device| device.fail_superblock_read = true);
    assert_eq!(ext4::transaction_probe(&mut mounted, name, written as u64, &vec![0x52; 32 * 4096]),
        Err(Status::Io));
    assert_public_reads_refused(&mounted);
    DEVICE.with_borrow_mut(|device| device.fail_superblock_read = false);
    ext4::sync(&mut mounted).unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    assert_eq!(ext4::stat(&mounted, name), Ok(original));
    assert_eq!(ext4::truncate_probe(&mut mounted, b"system/immutable-test", 2),
        Err(Status::ReadOnly));
    let mut content = [0; 9];
    assert_eq!(ext4::pread(&mounted, b"system/immutable-test", 0, &mut content), Ok(9));
    assert_eq!(&content, b"protected");
    // A fresh, fitting allocation must still work after rolling back ENOSPC.
    assert_eq!(ext4::transaction_probe(&mut mounted, name, written as u64, b"fits"), Ok(4));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-full");
}

#[test]
fn sync_finishes_failed_marker_activation_without_starting_the_mutation() {
    let Some(path) = fixture() else { return };
    for fail_at in [0, 1] {
        for accept in [false, true] {
            let mut mounted = mount_fixture(&path);
            let initial = DEVICE.with_borrow(|device| device.bytes.clone());
            DEVICE.with_borrow_mut(|device| {
                device.fail_event = Some(fail_at);
                device.accept_failed_write = accept;
            });
            assert_eq!(ext4::create_file_probe(&mut mounted, b"system/never-created", 0o600),
                Err(Status::Io));
            let first_attempt = DEVICE.with_borrow_mut(|device| {
                let events = device.events.clone();
                device.events.clear();
                events
            });
            // A second refusal through sync must keep the activation retryable.
            assert_eq!(ext4::sync(&mut mounted), Err(Status::Io));
            DEVICE.with_borrow_mut(|device| {
                assert_eq!(device.events, first_attempt);
                device.events.clear();
                device.fail_event = None;
            });
            ext4::sync(&mut mounted).unwrap();
            DEVICE.with_borrow(|device| {
                assert_eq!(&device.events[..first_attempt.len()], &first_attempt);
                assert_eq!(device.events.len(), 4, "only activation and clearing are needed");
                assert_eq!(device.bytes, initial, "sync published the failed create");
            });
            assert_eq!(ext4::stat(&mounted, b"system/never-created"), Err(Status::NotFound));
            ext4::unmount(&mounted).unwrap();
        }
    }
    fsck(&path, "coordinator-marker-sync");
}

#[test]
fn cross_directory_file_rename_preserves_identity_and_refuses_replacement() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    ext4::create_directory_probe(&mut mounted, b"system/move-from").unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/move-to").unwrap();
    let source = b"system/move-from/source";
    let target = b"system/move-to/target";
    ext4::create_file_probe(&mut mounted, source, 0o640).unwrap();
    ext4::transaction_probe(&mut mounted, source, 4095, b"across").unwrap();
    ext4::link_file_probe(&mut mounted, source, b"system/move-from/alias").unwrap();
    let identity = ext4::stat(&mounted, source).unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::rename_probe(&mut mounted, source, target).unwrap();
    assert_eq!(ext4::stat(&mounted, source), Err(Status::NotFound));
    assert_eq!(ext4::stat(&mounted, target), Ok(identity));
    assert_eq!(ext4::stat(&mounted, b"system/move-from/alias"), Ok(identity));
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    let mut content = vec![0xa5; 4101];
    read_exact(&mounted, target, &mut content);
    assert!(content[..4095].iter().all(|byte| *byte == 0));
    assert_eq!(&content[4095..], b"across");
    ext4::create_file_probe(&mut mounted, source, 0o600).unwrap();
    let other = ext4::stat(&mounted, source).unwrap();
    assert_eq!(ext4::rename_probe(&mut mounted, source, target), Err(Status::Exists));
    assert_eq!(ext4::stat(&mounted, source), Ok(other));
    assert_eq!(ext4::stat(&mounted, target), Ok(identity));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-move");
    drop(mounted);
    let image = path.with_extension("coordinator-move.img");
    debugfs(&image, "symlink /system/move-alias /system/move-from");
    let mut mounted = mount_fixture(&image);
    ext4::rename_probe(&mut mounted, source, b"system/move-alias/renamed").unwrap();
    assert_eq!(ext4::stat(&mounted, source), Err(Status::NotFound));
    assert_eq!(ext4::stat(&mounted, b"system/move-from/renamed"), Ok(other));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-move-alias");
}

#[test]
fn replacement_rename_preserves_hardlinks_and_directory_parent_counts() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    for parent in ["system", "data/user"] {
        let source = b"system/replace-source";
        let target = format!("{parent}/replace-target");
        ext4::create_file_probe(&mut mounted, source, 0o640).unwrap();
        ext4::transaction_probe(&mut mounted, source, 0, b"new value").unwrap();
        ext4::link_file_probe(&mut mounted, source, b"system/source-alias").unwrap();
        let source_inode = ext4::lstat(&mounted, source).unwrap();
        ext4::rename_replace_probe(&mut mounted, source, b"system/source-alias").unwrap();
        assert_eq!(ext4::lstat(&mounted, source), Ok(source_inode));
        assert_eq!(ext4::lstat(&mounted, b"system/source-alias"), Ok(source_inode));
        ext4::unlink_file_probe(&mut mounted, b"system/source-alias").unwrap();
        ext4::create_file_probe(&mut mounted, target.as_bytes(), 0o600).unwrap();
        ext4::transaction_probe(&mut mounted, target.as_bytes(), 0, b"old value").unwrap();
        ext4::link_file_probe(&mut mounted, target.as_bytes(), b"system/old-alias").unwrap();
        let old_inode = ext4::lstat(&mounted, target.as_bytes()).unwrap().inode;
        ext4::rename_replace_probe(&mut mounted, source, target.as_bytes()).unwrap();
        assert_eq!(ext4::lstat(&mounted, source), Err(Status::NotFound));
        assert_eq!(ext4::lstat(&mounted, target.as_bytes()).unwrap().inode, source_inode.inode);
        assert_eq!(ext4::lstat(&mounted, b"system/old-alias").unwrap().inode, old_inode);
        let mut data = [0; 9];
        read_exact(&mounted, target.as_bytes(), &mut data);
        assert_eq!(&data, b"new value");
        read_exact(&mounted, b"system/old-alias", &mut data);
        assert_eq!(&data, b"old value");
        ext4::unlink_file_probe(&mut mounted, target.as_bytes()).unwrap();
        ext4::unlink_file_probe(&mut mounted, b"system/old-alias").unwrap();
    }
    ext4::create_directory_probe(&mut mounted, b"system/source-dir").unwrap();
    ext4::create_directory_probe(&mut mounted, b"data/user/target-dir").unwrap();
    ext4::create_file_probe(&mut mounted, b"data/user/target-dir/child", 0o600).unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    assert_eq!(ext4::rename_replace_probe(&mut mounted, b"system/source-dir",
        b"data/user/target-dir"), Err(Status::NotEmpty));
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    assert_eq!(ext4::rename_replace_probe(&mut mounted, b"system/source-dir",
        b"data/user/target-dir/child"), Err(Status::NotDirectory));
    assert_eq!(ext4::rename_replace_probe(&mut mounted, b"data/user/target-dir/child",
        b"system/source-dir"), Err(Status::IsDirectory));
    ext4::unlink_file_probe(&mut mounted, b"data/user/target-dir/child").unwrap();
    let system_links = ext4::stat(&mounted, b"system").unwrap().links;
    let user_links = ext4::stat(&mounted, b"data/user").unwrap().links;
    ext4::rename_replace_probe(&mut mounted, b"system/source-dir", b"data/user/target-dir").unwrap();
    assert_eq!(ext4::stat(&mounted, b"system").unwrap().links, system_links - 1);
    assert_eq!(ext4::stat(&mounted, b"data/user").unwrap().links, user_links);
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-rename-replace");
}

#[test]
fn symlinks_roundtrip_inline_boundary_dangling_and_looping_targets() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let free = ext4::free_bytes(&mounted).unwrap();
    for length in [1, 59, 60, 61, 127, 4095] {
        let name = format!("system/link-{length}");
        // Avoid oversized individual components while preserving literal bytes.
        let mut target = vec![b'x'; length];
        for index in (1..length).step_by(2) { target[index] = b'/'; }
        ext4::symlink_probe(&mut mounted, name.as_bytes(), &target).unwrap();
        let metadata = ext4::lstat(&mounted, name.as_bytes()).unwrap();
        assert_eq!(metadata.file_type, 3);
        assert_eq!(metadata.mode & 0o777, 0o777);
        assert_eq!(metadata.links, 1);
        let mut bytes = vec![0xa5; length + 1];
        assert_eq!(ext4::readlink(&mounted, name.as_bytes(), &mut bytes), Ok(length));
        assert_eq!(&bytes[..length], &target);
        assert_eq!(bytes[length], 0xa5);
        assert_eq!(ext4::readlink(&mounted, name.as_bytes(), &mut bytes[..1]), Ok(1));
        assert_eq!(ext4::symlink_probe(&mut mounted, name.as_bytes(), b"other"), Err(Status::Exists));
        let alias = format!("data/user/link-alias-{length}");
        ext4::link_file_probe(&mut mounted, name.as_bytes(), alias.as_bytes()).unwrap();
        assert_eq!(ext4::lstat(&mounted, alias.as_bytes()).unwrap().inode, metadata.inode);
        assert_eq!(ext4::lstat(&mounted, name.as_bytes()).unwrap().links, 2);
        assert_eq!(ext4::readlink(&mounted, alias.as_bytes(), &mut bytes), Ok(length));
        assert_eq!(&bytes[..length], &target);
    }
    ext4::symlink_probe(&mut mounted, b"system/link-loop", b"link-loop").unwrap();
    ext4::symlink_probe(&mut mounted, b"system/link-real", b"README.TXT").unwrap();
    assert_eq!(ext4::stat(&mounted, b"system/link-real"), ext4::stat(&mounted, b"system/README.TXT"));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-symlinks-live");
    let disk = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let mut mounted = mount_bytes(disk);
    let mut target = [0; 16];
    assert_eq!(ext4::readlink(&mounted, b"system/link-loop", &mut target), Ok(9));
    for length in [1, 59, 60, 61, 127, 4095] {
        let name = format!("system/link-{length}");
        ext4::rename_probe(&mut mounted, name.as_bytes(), b"data/user/moved-link").unwrap();
        ext4::unlink_file_probe(&mut mounted, b"data/user/moved-link").unwrap();
        let alias = format!("data/user/link-alias-{length}");
        assert_eq!(ext4::lstat(&mounted, alias.as_bytes()).unwrap().links, 1);
        let mut literal = vec![0; length];
        assert_eq!(ext4::readlink(&mounted, alias.as_bytes(), &mut literal), Ok(length));
        ext4::unlink_file_probe(&mut mounted, alias.as_bytes()).unwrap();
    }
    ext4::unlink_file_probe(&mut mounted, b"system/link-loop").unwrap();
    ext4::unlink_file_probe(&mut mounted, b"system/link-real").unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-symlinks-removed");
}

#[test]
fn sparse_fragmented_extent_tree_grows_overwrites_and_shrinks() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/fragmented";
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    // Separate logical blocks cannot merge even if physical allocation happens
    // contiguously. Sixty extents force allocation beyond the inode root.
    let mut expected = vec![0; 59 * 8192 + 4096];
    for index in 0..60usize {
        let start = index * 8192;
        let bytes = vec![(index + 1) as u8; 4096];
        assert_eq!(ext4::transaction_probe(&mut mounted, name, start as u64, &bytes), Ok(bytes.len()));
        expected[start..start + bytes.len()].copy_from_slice(&bytes);
    }
    let mut actual = vec![0xa5; expected.len()];
    read_exact(&mounted, name, &mut actual);
    assert_eq!(actual, expected);
    // Coalesce an unaligned overwrite across existing extents and holes.
    let replacement = vec![0xbc; 32 * 4096 + 11];
    ext4::set_stage_block_limit(&mut mounted, 12).unwrap();
    DEVICE.with_borrow_mut(|device| device.events.clear());
    assert_eq!(ext4::transaction_probe(&mut mounted, name, 4093, &replacement), Ok(replacement.len()));
    DEVICE.with_borrow(|device| assert!(device.events.iter().filter(|event| **event == Event::Flush(3)).count() > 2));
    ext4::set_stage_block_limit(&mut mounted, 64).unwrap();
    expected[4093..4093 + replacement.len()].copy_from_slice(&replacement);
    read_exact(&mounted, name, &mut actual);
    assert_eq!(actual, expected);
    ext4::sync(&mut mounted).unwrap();
    fsck(&path, "coordinator-fragmented-before-truncate");
    let shortened = 40 * 8192 + 103;
    // Keep the upstream error visible instead of losing its precise cause in
    // the stable C Invalid status. This isolated stage never writes the device.
    let backing = DEVICE.with_borrow(|device| device.bytes.clone());
    let stage = std::rc::Rc::new(ext4plus::JournalMutationStage::new(
        Box::new(backing.clone()), backing.len() as u64).unwrap());
    let raw = ext4plus::Ext4::load_with_writer(Box::new(stage.clone()),
        Some(Box::new(stage.clone()))).unwrap();
    let mut raw_file = raw.open(b"/system/fragmented").unwrap();
    raw_file.truncate(shortened as u64).expect("upstream fragmented truncate");
    println!("fragmented truncate stage: {} images, {} revokes",
        stage.staged_block_count(), stage.revoked_block_count());
    drop(raw_file);
    drop(raw);
    drop(stage);
    ext4::truncate_probe(&mut mounted, name, shortened as u64).unwrap();
    ext4::truncate_probe(&mut mounted, name, expected.len() as u64).unwrap();
    expected[shortened..].fill(0);
    read_exact(&mounted, name, &mut actual);
    assert_eq!(actual, expected);
    // At most 58 live data blocks and one extent leaf remain after shrink.
    ext4::sync(&mut mounted).unwrap();
    fsck(&path, "coordinator-fragmented-live");
    ext4::unlink_file_probe(&mut mounted, name).unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-fragmented-extents");
}

#[test]
fn directory_growth_shrink_and_multiblock_rmdir_preserve_counters() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/growing").unwrap();
    let mut names = Vec::new();
    for index in 0..55 {
        let name = format!("system/growing/{index:03}-{}", "x".repeat(80));
        ext4::create_directory_probe(&mut mounted, name.as_bytes()).unwrap();
        names.push(name);
    }
    assert!(ext4::stat(&mounted, b"system/growing").unwrap().size > 4096);
    // Reverse removal empties the tail block, requiring both the last child
    // block and the parent block to be revoked in the same rmdir transaction.
    for name in names.iter().rev() {
        ext4::remove_directory_probe(&mut mounted, name.as_bytes()).unwrap();
    }
    assert_eq!(ext4::stat(&mounted, b"system/growing").unwrap().size, 4096);
    ext4::remove_directory_probe(&mut mounted, b"system/growing").unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    ext4::create_directory_probe(&mut mounted, b"system/empty-grown").unwrap();
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    let image = path.with_extension("coordinator-grown-dir.img");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    drop(mounted);
    for _ in 0..3 { debugfs(&image, "expand_dir /system/empty-grown"); }
    let mut mounted = mount_fixture(&image);
    assert_eq!(ext4::stat(&mounted, b"system/empty-grown").unwrap().size, 4 * 4096);
    ext4::remove_directory_probe(&mut mounted, b"system/empty-grown").unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-directory-shrink");
}

#[test]
fn directory_move_updates_parents_and_rejects_descendant_cycles() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let source = b"system/tree";
    let destination = b"data/user/tree";
    ext4::create_directory_probe(&mut mounted, source).unwrap();
    ext4::create_directory_probe(&mut mounted, b"system/tree/child").unwrap();
    ext4::create_file_probe(&mut mounted, b"system/tree/child/file", 0o640).unwrap();
    ext4::transaction_probe(&mut mounted, b"system/tree/child/file", 0, b"preserved").unwrap();
    let identity = ext4::stat(&mounted, source).unwrap();
    let old_parent = ext4::stat(&mounted, b"system").unwrap();
    let new_parent = ext4::stat(&mounted, b"data/user").unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    ext4::sync(&mut mounted).unwrap();
    let initial = DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.bytes.clone()
    });
    assert_eq!(ext4::rename_probe(&mut mounted, source,
        b"system/tree/child/cycle"), Err(Status::Invalid));
    assert_eq!(ext4::stat(&mounted, source), Ok(identity));
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    // Remount so the successful trace includes marker activation from clean.
    drop(mounted);
    let mut mounted = mount_bytes(initial.clone());
    ext4::rename_probe(&mut mounted, source, destination).unwrap();
    let trace = DEVICE.with_borrow(|device| device.events.clone());
    assert_eq!(ext4::stat(&mounted, source), Err(Status::NotFound));
    assert_eq!(ext4::stat(&mounted, destination), Ok(identity));
    assert_eq!(ext4::stat(&mounted, b"system").unwrap().links, old_parent.links - 1);
    assert_eq!(ext4::stat(&mounted, b"data/user").unwrap().links, new_parent.links + 1);
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    drop(mounted);
    // Recover each acknowledged durable prefix. Before commit only the old
    // tree exists; after commit the new tree includes coherent parent links.
    let mut prefix = initial;
    let mut committed = false;
    for (index, event) in trace.iter().enumerate() {
        match event {
            Event::Write(start, bytes) => {
                let start = *start as usize;
                prefix[start..start + bytes.len()].copy_from_slice(bytes);
                continue;
            }
            Event::Flush(3) => committed = true,
            Event::Flush(_) => {}
        }
        let mounted = mount_bytes(prefix.clone());
        let (present, absent, content): (&[u8], &[u8], &[u8]) = if committed {
            (destination, source, b"data/user/tree/child/file")
        } else {
            (source, destination, b"system/tree/child/file")
        };
        assert_eq!(ext4::stat(&mounted, present), Ok(identity));
        assert_eq!(ext4::stat(&mounted, absent), Err(Status::NotFound));
        let mut bytes = [0; 9];
        read_exact(&mounted, content, &mut bytes);
        assert_eq!(&bytes, b"preserved");
        ext4::unmount(&mounted).unwrap();
        fsck(&path, &format!("coordinator-directory-move-cut-{index}"));
    }
    let image = path.with_extension("coordinator-directory-alias.img");
    DEVICE.with_borrow(|device| std::fs::write(&image, &device.bytes).unwrap());
    debugfs(&image, "symlink /system/tree-alias /data/user/tree/child");
    let mut mounted = mount_fixture(&image);
    assert_eq!(ext4::rename_probe(&mut mounted, destination,
        b"system/tree-alias/cycle"), Err(Status::Invalid));
    assert_eq!(ext4::rename_probe(&mut mounted, destination,
        b"data/user/tree/child/existing"), Err(Status::Invalid));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-directory-cycle");
}

#[test]
fn repeated_mount_recovery_refusals_converge_to_the_same_clean_disk() {
    let Some(path) = fixture() else { return };
    let name = b"system/recovery-refusal";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::transaction_probe(&mut mounted, name, 0, &vec![0x5a; 8192]).unwrap();
    ext4::sync(&mut mounted).unwrap();
    let mut crashed = DEVICE.with_borrow_mut(|device| {
        device.events.clear();
        device.bytes.clone()
    });
    ext4::truncate_probe(&mut mounted, name, 101).unwrap();
    let committed = DEVICE.with_borrow(|device| device.events.clone());
    let mut saw_commit = false;
    for event in committed {
        match event {
            Event::Write(start, bytes) => {
                let start = start as usize;
                crashed[start..start + bytes.len()].copy_from_slice(&bytes);
            }
            Event::Flush(3) => { saw_commit = true; break; }
            Event::Flush(_) => {}
        }
    }
    assert!(saw_commit);
    drop(mounted);
    let recovered = mount_bytes(crashed.clone());
    assert_eq!(ext4::stat(&recovered, name).unwrap().size, 101);
    ext4::unmount(&recovered).unwrap();
    let (expected_events, expected_disk) = DEVICE.with_borrow(|device|
        (device.events.clone(), device.bytes.clone()));
    assert!(expected_events.contains(&Event::Flush(4)));
    assert!(expected_events.contains(&Event::Flush(5)));
    assert_eq!(expected_events.last(), Some(&Event::Flush(0)));
    drop(recovered);
    let mut repeated_failures = 0;
    for accept in [false, true] {
        for fail_at in 0..expected_events.len() {
            DEVICE.with_borrow_mut(|device| *device = Device {
                bytes: crashed.clone(), fail_event: Some(fail_at),
                accept_failed_write: accept, ..Device::default()
            });
            assert!(matches!(ext4::mount(1, crashed.len() as u64), Err(Status::Io)));
            DEVICE.with_borrow_mut(|device| {
                assert_eq!(device.events, expected_events[..=fail_at]);
                device.events.clear();
                device.fail_event = Some(0);
            });
            // Crash/refuse again using the first attempt's partly checkpointed
            // bytes, without retaining any failed mount object or replay plan.
            let second = ext4::mount(1, crashed.len() as u64);
            let recovered = match second {
                Ok((mounted, _)) => mounted, // the first clear may have reached disk
                Err(Status::Io) => {
                    repeated_failures += 1;
                    DEVICE.with_borrow_mut(|device| device.fail_event = None);
                    ext4::mount(1, crashed.len() as u64).unwrap().0
                }
                Err(error) => panic!("recovery became unrecoverable: {error:?}"),
            };
            assert_eq!(ext4::stat(&recovered, name).unwrap().size, 101);
            let mut prefix = [0; 101];
            read_exact(&recovered, name, &mut prefix);
            assert_eq!(prefix, [0x5a; 101]);
            ext4::unmount(&recovered).unwrap();
            DEVICE.with_borrow(|device| assert_eq!(device.bytes, expected_disk,
                "recovery event {fail_at}, accepted {accept}"));
        }
    }
    assert!(repeated_failures > 0);
    fsck(&path, "coordinator-recovery-refusals");
}

#[test]
fn unaligned_write_spans_transactions_and_retries_only_the_unfinished_suffix() {
    for block_limit in [64, 12] {
        split_write_retry_case(block_limit);
    }
}

fn split_write_retry_case(block_limit: usize) {
    let Some(path) = fixture() else { return };
    let name = b"system/split-write";
    let mut mounted = mount_fixture(&path);
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::sync(&mut mounted).unwrap();
    let initial = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let source: Vec<u8> = (0..64 * 4096).map(|index| (index % 251) as u8).collect();
    let mut mounted = mount_bytes(initial.clone());
    ext4::set_stage_block_limit(&mut mounted, block_limit).unwrap();
    assert_eq!(ext4::transaction_probe(&mut mounted, name, 7, &source), Ok(source.len()));
    let expected = DEVICE.with_borrow(|device| device.events.clone());
    let commits = expected.iter().filter(|event| **event == Event::Flush(3)).count();
    if block_limit == 64 { assert_eq!(commits, 3); }
    else { assert!(commits >= 9, "capacity pressure did not split the write"); }
    let mut result = vec![0xa5; source.len() + 7];
    read_exact(&mounted, name, &mut result);
    assert_eq!(&result[..7], &[0; 7]);
    assert_eq!(&result[7..], &source);
    ext4::sync(&mut mounted).unwrap();
    let expected_disk = DEVICE.with_borrow(|device| device.bytes.clone());
    drop(mounted);
    let second_start = expected.iter().position(|event| *event == Event::Flush(5)).unwrap() + 1;
    let ordered_flush = second_start + expected[second_start..].iter()
        .position(|event| *event == Event::Flush(1)).unwrap();
    let commit_flush = second_start + expected[second_start..].iter()
        .position(|event| *event == Event::Flush(3)).unwrap();
    for fail_at in [second_start, ordered_flush, commit_flush] {
        for accept in [false, true] {
            for through_sync in [false, true] {
                let mut mounted = mount_bytes(initial.clone());
                ext4::set_stage_block_limit(&mut mounted, block_limit).unwrap();
                DEVICE.with_borrow_mut(|device| {
                    device.fail_event = Some(fail_at);
                    device.accept_failed_write = accept;
                });
                assert_eq!(ext4::transaction_probe(&mut mounted, name, 7, &source), Err(Status::Io));
                assert_public_reads_refused(&mounted);
                assert_eq!(ext4::transaction_probe(&mut mounted, name, 8, &source), Err(Status::Invalid));
                DEVICE.with_borrow_mut(|device| {
                    assert_eq!(device.events, expected[..=fail_at]);
                    device.events.clear();
                    device.fail_event = None;
                });
                if through_sync {
                    ext4::sync(&mut mounted).unwrap();
                } else {
                    assert_eq!(ext4::transaction_probe(&mut mounted, name, 7, &source), Ok(source.len()));
                    ext4::sync(&mut mounted).unwrap();
                }
                DEVICE.with_borrow(|device| {
                    assert!(device.events.starts_with(&expected[second_start..]),
                        "earlier checkpointed chunks were repeated");
                    assert_eq!(device.events.len(), expected.len() - second_start + 2);
                    assert_eq!(device.bytes, expected_disk);
                });
                ext4::unmount(&mounted).unwrap();
            }
        }
    }
    fsck(&path, &format!("coordinator-split-write-budget-{block_limit}"));
}

#[test]
fn minimum_write_capacity_refusal_rolls_back_all_allocations() {
    let Some(path) = fixture() else { return };
    let mut mounted = mount_fixture(&path);
    let name = b"system/stage-full";
    ext4::create_file_probe(&mut mounted, name, 0o600).unwrap();
    ext4::sync(&mut mounted).unwrap();
    let free = ext4::free_bytes(&mounted).unwrap();
    let initial = DEVICE.with_borrow_mut(|device| { device.events.clear(); device.bytes.clone() });
    ext4::set_stage_block_limit(&mut mounted, 1).unwrap();
    assert_eq!(ext4::transaction_probe(&mut mounted, name, 7, &[0x5a; 8192]), Err(Status::Io));
    assert_eq!(ext4::stat(&mounted, name).unwrap().size, 0);
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    // The durable recovery marker is the only permitted write before a
    // transaction fits. The partially staged bitmap/counters never escape.
    DEVICE.with_borrow(|device| {
        assert_eq!(device.events.len(), 2);
        assert!(matches!(&device.events[0], Event::Write(1024, bytes) if bytes.len() == 1024));
        assert_eq!(device.events[1], Event::Flush(0));
        assert_eq!(&device.bytes[..1024], &initial[..1024]);
        assert_eq!(&device.bytes[2048..], &initial[2048..]);
    });
    ext4::sync(&mut mounted).unwrap();
    DEVICE.with_borrow(|device| assert_eq!(device.bytes, initial));
    ext4::set_stage_block_limit(&mut mounted, 64).unwrap();
    assert_eq!(ext4::transaction_probe(&mut mounted, name, 7, &[0x5a; 8192]), Ok(8192));
    ext4::unlink_file_probe(&mut mounted, name).unwrap();
    assert_eq!(ext4::free_bytes(&mounted), Ok(free));
    ext4::sync(&mut mounted).unwrap();
    ext4::unmount(&mounted).unwrap();
    fsck(&path, "coordinator-stage-capacity-rollback");
}
