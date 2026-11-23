//! Runs the binary on AppImages assembled from an ELF stub and the images in tests/data.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use image::RgbaImage;
use tempfile::TempDir;

const BIN: &str = env!("CARGO_BIN_EXE_appimage-thumbnailer");
const SIDECAR_SLOT: usize = 128 * 1024;

/// ELF64 header without sections, so the payload starts right after it.
fn elf_stub() -> Vec<u8> {
    let mut h = vec![0u8; 64];
    h[..6].copy_from_slice(b"\x7fELF\x02\x01");
    h[40..48].copy_from_slice(&64u64.to_le_bytes());
    h
}

fn fixture(name: &str) -> Vec<u8> {
    fs::read(
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/data")
            .join(name),
    )
    .unwrap()
}

struct Run {
    output: Output,
    thumbnail: Option<RgbaImage>,
}

fn run(appimage: &[u8], extra_args: &[&str]) -> Run {
    let dir = TempDir::new().unwrap();
    let input = dir.path().join("test.AppImage");
    let out: PathBuf = dir.path().join("out.png");
    fs::write(&input, appimage).unwrap();
    let output = Command::new(BIN)
        .arg(&input)
        .arg(&out)
        .args(extra_args)
        .output()
        .unwrap();
    let thumbnail = out
        .exists()
        .then(|| image::open(&out).unwrap().into_rgba8());
    Run { output, thumbnail }
}

/// The fixtures' .DirIcon is an opaque red disc.
fn assert_red_icon(run: &Run, size: u32) {
    assert!(
        run.output.status.success(),
        "{}",
        String::from_utf8_lossy(&run.output.stderr)
    );
    let thumbnail = run.thumbnail.as_ref().unwrap();
    assert_eq!(thumbnail.dimensions(), (size, size));
    let center = thumbnail.get_pixel(size / 2, size / 2);
    assert!(
        center[0] > 200 && center[1] < 50 && center[3] == 255,
        "{center:?}"
    );
}

#[test]
fn every_fixture_format() {
    for name in [
        "gzip.sqfs",
        "xz.sqfs",
        "zstd.sqfs",
        "lz4.sqfs",
        "zstd.dwarfs",
        "lzma.dwarfs",
        "lz4.dwarfs",
    ] {
        let appimage = [elf_stub(), fixture(name)].concat();
        assert_red_icon(&run(&appimage, &["128"]), 128);
    }
}

#[test]
fn sidecar_retries_the_dwarfs_payload() {
    let mut appimage = [elf_stub(), fixture("sidecar.sqfs")].concat();
    // Without the DwarFS payload the sidecar's .DirIcon leads nowhere.
    let alone = run(&appimage, &[]);
    assert!(!alone.output.status.success() && alone.thumbnail.is_none());

    appimage.resize(64 + SIDECAR_SLOT, 0);
    appimage.extend(fixture("zstd.dwarfs"));
    assert_red_icon(&run(&appimage, &[]), 256);
}

#[test]
fn size_argument() {
    let appimage = [elf_stub(), fixture("zstd.sqfs")].concat();
    assert_red_icon(&run(&appimage, &[]), 256);
    assert_red_icon(&run(&appimage, &["64px"]), 64);
    for bad in ["abc", "0", "5000"] {
        assert_red_icon(&run(&appimage, &[bad]), 256);
    }
}

#[test]
fn rejects_non_appimages() {
    let garbage = [elf_stub(), b"neither squashfs nor dwarfs".to_vec()].concat();
    for input in [b"plain text".to_vec(), garbage] {
        let r = run(&input, &[]);
        assert!(!r.output.status.success() && r.thumbnail.is_none());
        assert!(!r.output.stderr.is_empty());
    }
}

#[test]
fn command_line() {
    let help = Command::new(BIN).arg("--help").output().unwrap();
    assert!(help.status.success());
    assert!(String::from_utf8_lossy(&help.stdout).starts_with("Usage: "));

    let version = Command::new(BIN).arg("-V").output().unwrap();
    assert!(version.status.success());
    let expected = format!("appimage-thumbnailer v{}\n", env!("CARGO_PKG_VERSION"));
    assert!(String::from_utf8_lossy(&version.stdout).starts_with(&expected));

    for args in [&[][..], &["only-one"], &["a", "b", "c", "d"]] {
        let r = Command::new(BIN).args(args).output().unwrap();
        assert!(!r.status.success(), "{args:?}");
    }
}
