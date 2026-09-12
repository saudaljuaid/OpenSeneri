// SPDX-License-Identifier: GPL-3.0-only
//! A vault read through the seam, an entry at a time.
//!
//! The reason this exists rather than [`media_editor_io::vault::decode`]: one of
//! Phipia's files holds sixteen mebibytes and a Phipia program is mapped
//! seventy-six kilobytes. Building the whole vault in memory is off by three
//! orders of magnitude on the machine this program is for, and a store read an
//! entry at a time is not.
//!
//! Phipia reaches the same conclusion one layer down: its own bitmap reader
//! issues random row reads through the normal filesystem and `NVMe` paths
//! rather than holding a picture.

use media_editor_abi::seam::{SeamStatus, Slot, Storage};
use media_editor_core::Digest;
use media_editor_io::vault::{
    Catalogue, ENTRY_BYTES, HEADER_BYTES, MAX_ITEMS, Vault, encode, fetch, store,
};
use media_editor_io::{IoStatus, MemoryStorage};

/// A vault of four pieces of material of different lengths.
fn stocked() -> Vault {
    let mut vault = Vault::new();
    for index in 0..4_usize {
        let material = std::vec![u8::try_from(index).expect("a byte"); 100 + index * 37];
        vault
            .insert(&std::format!("a photograph called {index}.bmp"), &material)
            .expect("room");
    }
    vault
}

fn stored(vault: &Vault) -> MemoryStorage {
    let mut storage = MemoryStorage::new(1 << 20);
    store(vault, &mut storage).expect("a store");
    storage
}

#[test]
fn a_vault_is_written_and_read_back_whole() {
    let vault = stocked();
    let mut storage = MemoryStorage::new(1 << 20);
    let digest = store(&vault, &mut storage).expect("a store");
    assert_eq!(digest, Digest::of(&encode(&vault).expect("an encoding")));
    assert_eq!(fetch(&storage).expect("a fetch"), vault);
    assert_eq!(
        storage.stored().expect("the slot"),
        encode(&vault).expect("an encoding").as_slice()
    );
    // And the project slot was never touched, which is what makes the two
    // saves independent.
    assert!(storage.committed().is_none());
}

#[test]
fn a_catalogue_never_reads_the_whole_slot() {
    // The property the whole module exists for. Every answer below comes from
    // ranged reads; the count of whole-slot reads must stay at nought, because
    // one of them on the target would be an allocation of up to sixteen
    // mebibytes on a machine with seventy-six kilobytes.
    let vault = stocked();
    let storage = stored(&vault);
    let before = storage.whole_reads();

    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    assert_eq!(catalogue.len(), 4);
    assert_eq!(
        catalogue.payload_bytes(),
        u64::try_from(vault.payload_bytes()).expect("a length")
    );
    for index in 0..catalogue.len() {
        let entry = catalogue.entry(&storage, index).expect("an entry");
        assert_eq!(entry.name(), vault.items()[index].name());
        assert_eq!(entry.digest(), vault.items()[index].digest());
        assert_eq!(
            entry.length(),
            u64::try_from(vault.items()[index].bytes().len()).expect("a length")
        );
    }
    assert_eq!(
        storage.whole_reads(),
        before,
        "the catalogue read the whole slot at least once"
    );
    assert!(storage.ranged_reads() > 0, "and it read nothing at all");
}

#[test]
fn material_comes_back_a_window_at_a_time() {
    // A caller with a buffer smaller than the material gets what fits, and
    // asking again from where it stopped gets the rest. Which is how a program
    // with less memory than a photograph reads a photograph.
    let vault = stocked();
    let storage = stored(&vault);
    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    let entry = catalogue.entry(&storage, 2).expect("an entry");
    let wanted = vault.items()[2].bytes();

    let mut window = [0_u8; 16];
    let mut gathered = std::vec::Vec::new();
    let mut at = 0_u64;
    loop {
        let read = catalogue
            .material(&storage, &entry, at, &mut window)
            .expect("material");
        if read == 0 {
            break;
        }
        gathered.extend_from_slice(&window[..read]);
        at += u64::try_from(read).expect("a length");
    }
    assert_eq!(gathered, wanted, "the windows did not reassemble");
    assert_eq!(
        gathered.len(),
        174,
        "a hundred and thirty-seven plus thirty-seven"
    );

    // And a buffer larger than the material gets exactly the material, not the
    // item after it.
    let mut room = std::vec![0_u8; wanted.len() + 64];
    let read = catalogue
        .material(&storage, &entry, 0, &mut room)
        .expect("material");
    assert_eq!(read, wanted.len());
    assert_eq!(&room[..read], wanted);
}

#[test]
fn a_catalogue_finds_material_by_what_it_is() {
    let vault = stocked();
    let storage = stored(&vault);
    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    for item in vault.items() {
        let found = catalogue
            .find(&storage, item.digest())
            .expect("a search")
            .expect("the entry");
        assert_eq!(found.name(), item.name());
    }
    assert_eq!(
        catalogue
            .find(&storage, Digest::of(b"never pasted in"))
            .expect("a search"),
        None
    );
}

#[test]
fn an_entry_past_the_count_is_refused() {
    let storage = stored(&stocked());
    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    assert_eq!(
        catalogue.entry(&storage, 4),
        Err(IoStatus::VaultItemAbsent),
        "a vault of four has no fifth entry"
    );
}

#[test]
fn an_empty_vault_opens_and_holds_nothing() {
    let storage = stored(&Vault::new());
    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    assert!(catalogue.is_empty());
    assert_eq!(catalogue.payload_bytes(), 0);
    assert_eq!(catalogue.slot(), Slot::Vault);
    assert_eq!(catalogue.entry(&storage, 0), Err(IoStatus::VaultItemAbsent));
    catalogue.verify(&storage, 64).expect("a verification");
}

#[test]
fn an_empty_slot_is_not_a_vault() {
    let storage = MemoryStorage::new(1 << 20);
    assert_eq!(
        Catalogue::open(&storage, Slot::Vault),
        Err(IoStatus::Seam(SeamStatus::Empty))
    );
}

#[test]
fn a_slot_too_short_to_hold_a_header_is_refused() {
    let mut storage = MemoryStorage::new(1 << 20);
    storage
        .write(Slot::Scratch, b"SSV1 and no more")
        .expect("a write");
    storage.commit(Slot::Vault).expect("a commit");
    assert_eq!(
        Catalogue::open(&storage, Slot::Vault),
        Err(IoStatus::NotAVault)
    );
}

#[test]
fn a_header_disagreeing_with_the_slot_is_refused() {
    // The one cross-check a header can make on its own: the count and the
    // payload length say how long the file is, and the slot says how long it
    // is. A catalogue cannot check the seal without reading everything, so
    // this is the check that has to be right.
    let vault = stocked();
    let file = encode(&vault).expect("an encoding");

    let mut short = MemoryStorage::new(1 << 20);
    short
        .write(Slot::Scratch, &file[..file.len() - 1])
        .expect("a write");
    short.commit(Slot::Vault).expect("a commit");
    assert_eq!(
        Catalogue::open(&short, Slot::Vault),
        Err(IoStatus::TruncatedPayload)
    );

    let mut long = MemoryStorage::new(1 << 20);
    let mut padded = file.clone();
    padded.push(0);
    long.write(Slot::Scratch, &padded).expect("a write");
    long.commit(Slot::Vault).expect("a commit");
    assert_eq!(
        Catalogue::open(&long, Slot::Vault),
        Err(IoStatus::TrailingBytes)
    );
}

#[test]
fn a_header_claiming_more_items_than_the_bound_is_refused() {
    let mut file = encode(&stocked()).expect("an encoding");
    file[8..12].copy_from_slice(&u32::try_from(MAX_ITEMS + 1).expect("a count").to_le_bytes());
    let mut storage = MemoryStorage::new(1 << 20);
    storage.write(Slot::Scratch, &file).expect("a write");
    storage.commit(Slot::Vault).expect("a commit");
    assert_eq!(
        Catalogue::open(&storage, Slot::Vault),
        Err(IoStatus::TooMany),
        "refused on the count before the length arithmetic runs on it"
    );
}

#[test]
fn verify_walks_everything_open_could_not() {
    let vault = stocked();
    let storage = stored(&vault);
    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    // Several window sizes, including one byte and one larger than the file,
    // because a chunked walk is exactly where an off-by-one lives.
    for chunk in [1_usize, 7, 64, 4096] {
        catalogue
            .verify(&storage, chunk)
            .unwrap_or_else(|refusal| panic!("a window of {chunk} refused: {refusal:?}"));
    }
    assert_eq!(catalogue.verify(&storage, 0), Err(IoStatus::TooMany));
}

#[test]
fn verify_finds_what_open_deliberately_did_not() {
    // `open` does not check the seal, and this is the test that says so out
    // loud: a file with a byte of material changed opens perfectly well and
    // fails verification, which is the trade the catalogue exists to make.
    let vault = stocked();
    let file = encode(&vault).expect("an encoding");
    let mut damaged = file.clone();
    let at = HEADER_BYTES + 4 * ENTRY_BYTES + 3;
    damaged[at] ^= 0xFF;
    let mut storage = MemoryStorage::new(1 << 20);
    storage.write(Slot::Scratch, &damaged).expect("a write");
    storage.commit(Slot::Vault).expect("a commit");

    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("it opens");
    assert_eq!(catalogue.len(), 4, "and reads perfectly well");
    assert_eq!(
        catalogue.verify(&storage, 64),
        Err(IoStatus::DigestMismatch),
        "the seal is what catches it, and only verify looks"
    );
}

#[test]
fn verify_names_a_gap_and_a_lie_separately() {
    let vault = stocked();
    let file = encode(&vault).expect("an encoding");

    // A span moved on by one: the material is all there and hashes correctly,
    // and the file now claims a byte between two items that nothing accounts
    // for.
    let mut gapped = file.clone();
    let at = HEADER_BYTES + ENTRY_BYTES + 32;
    let first = u64::from_le_bytes(gapped[at..at + 8].try_into().expect("eight bytes"));
    gapped[at..at + 8].copy_from_slice(&(first + 1).to_le_bytes());
    let mut one = MemoryStorage::new(1 << 20);
    one.write(Slot::Scratch, &resealed(gapped))
        .expect("a write");
    one.commit(Slot::Vault).expect("a commit");
    assert_eq!(
        Catalogue::open(&one, Slot::Vault)
            .expect("a catalogue")
            .verify(&one, 64),
        Err(IoStatus::VaultSpanNotContiguous)
    );

    // And material that is not what its entry says, with the seal recomputed
    // so the seal is not what catches it.
    let mut lying = file.clone();
    lying[HEADER_BYTES + 4 * ENTRY_BYTES] ^= 0xFF;
    let mut two = MemoryStorage::new(1 << 20);
    two.write(Slot::Scratch, &resealed(lying)).expect("a write");
    two.commit(Slot::Vault).expect("a commit");
    assert_eq!(
        Catalogue::open(&two, Slot::Vault)
            .expect("a catalogue")
            .verify(&two, 64),
        Err(IoStatus::VaultItemDigestMismatch)
    );
}

/// The file with its seal recomputed, so a mutation reaches the check below it.
fn resealed(mut file: std::vec::Vec<u8>) -> std::vec::Vec<u8> {
    let mut hasher = media_editor_core::Sha256::new();
    hasher.update(&file[..24]);
    hasher.update(&file[HEADER_BYTES..]);
    let sealed = hasher.finish();
    file[24..HEADER_BYTES].copy_from_slice(sealed.bytes());
    file
}

#[test]
fn a_project_and_a_vault_are_committed_separately() {
    // Two committed slots, one scratch, and neither save can damage the other
    // — which is what makes "the vault is being written" survivable.
    let mut storage = MemoryStorage::new(1 << 20);
    store(&stocked(), &mut storage).expect("a vault");
    let vault_bytes = storage.stored().expect("the vault").to_vec();

    let project = media_editor_model::Project::new();
    media_editor_io::save(&project, &mut storage).expect("a project");
    assert_eq!(
        storage.stored().expect("the vault"),
        vault_bytes.as_slice(),
        "saving the project disturbed the vault"
    );
    assert!(storage.committed().is_some());
    assert_eq!(media_editor_io::load(&storage).expect("a load"), project);
}

#[test]
fn committing_into_the_scratch_slot_is_refused() {
    // It is where a save is assembled. An operation that made it its own
    // destination would be an operation with nothing to say about what
    // happened.
    let mut storage = MemoryStorage::new(1 << 20);
    storage.write(Slot::Scratch, b"anything").expect("a write");
    assert_eq!(storage.commit(Slot::Scratch), Err(SeamStatus::Refused));
}

#[test]
fn a_ranged_read_is_short_at_the_end_rather_than_refused() {
    // What Phipia's `phipfs_read` does: "reads at EOF succeed with a short or
    // zero byte count". A seam that refused there would make every reader
    // carry an arm for a condition that is not an error.
    let vault = stocked();
    let storage = stored(&vault);
    let held = storage.len(Slot::Vault).expect("a length");
    let mut into = [0_u8; 32];
    assert_eq!(
        storage
            .read_at(Slot::Vault, held - 4, &mut into)
            .expect("a read"),
        4
    );
    assert_eq!(
        storage
            .read_at(Slot::Vault, held, &mut into)
            .expect("a read"),
        0
    );
    assert_eq!(
        storage
            .read_at(Slot::Vault, held + 1_000, &mut into)
            .expect("a read"),
        0
    );
}

#[test]
fn a_vault_survives_a_save_that_fails_at_every_step() {
    // R-9.4 for the vault, which had it for the project and not for this. Each
    // fault in turn, and after every one the last good vault is still there —
    // which is the whole reason the protocol is four steps rather than one.
    use media_editor_io::Fault;

    let first = stocked();
    let mut storage = MemoryStorage::new(1 << 20);
    store(&first, &mut storage).expect("a first vault");
    let good = storage.stored().expect("the vault").to_vec();

    let mut second = stocked();
    second
        .insert("a later photograph.bmp", b"different material")
        .expect("room");

    for (fault, expected) in [
        (Fault::OnWrite, IoStatus::Seam(SeamStatus::Refused)),
        (Fault::Corrupting, IoStatus::WriteNotVerified),
        (Fault::OnReadBack, IoStatus::Seam(SeamStatus::Refused)),
        (Fault::OnCommit, IoStatus::Seam(SeamStatus::Refused)),
    ] {
        storage.set_fault(fault);
        assert_eq!(
            store(&second, &mut storage),
            Err(expected),
            "a save failing at {fault:?} did not refuse"
        );
        assert_eq!(
            storage.stored().expect("the vault"),
            good.as_slice(),
            "a save failing at {fault:?} damaged the last good vault"
        );
        // And it still opens, which is the thing somebody actually cares
        // about after an interrupted save.
        let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
        assert_eq!(catalogue.len(), 4);
        catalogue.verify(&storage, 64).expect("a verification");
    }

    // And with the fault cleared it commits, so the failures above were the
    // faults rather than something permanently wrong.
    storage.set_fault(Fault::None);
    store(&second, &mut storage).expect("a second vault");
    assert_eq!(
        Catalogue::open(&storage, Slot::Vault)
            .expect("a catalogue")
            .len(),
        5
    );
}
