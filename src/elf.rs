//! ELF header parsing: where the AppImage runtime ends and what follows it.

use std::fs::File;
use std::os::unix::fs::FileExt;

/// uruntime metadata sidecar slot: the DwarFS payload starts this far past the ELF end.
pub const SIDECAR_SLOT: u64 = 128 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Format {
    Squashfs,
    Dwarfs,
}

/// The payload starts where the section header table ends:
/// `e_shoff + e_shnum * e_shentsize`, for ELF32 and ELF64 in either byte order.
pub fn payload_offset(file: &File) -> Option<u64> {
    let mut h = [0u8; 64];
    file.read_exact_at(&mut h[..16], 0).ok()?;
    if h[..4] != *b"\x7fELF" {
        debug!("payload_offset: not an ELF file");
        return None;
    }
    // (header length, e_shoff position, e_shoff width, e_shentsize position)
    let (len, shoff_at, shoff_len, shentsize_at) = match h[4] {
        1 => (52, 32, 4, 46),
        2 => (64, 40, 8, 58),
        class => {
            debug!("payload_offset: unknown ELF class {class}");
            return None;
        }
    };
    file.read_exact_at(&mut h[16..len], 16).ok()?;

    let big_endian = h[5] == 2;
    let field = |at: usize, width: usize| -> u64 {
        let bytes = &h[at..at + width];
        let fold = |v: u64, b: &u8| v << 8 | u64::from(*b);
        if big_endian {
            bytes.iter().fold(0, fold)
        } else {
            bytes.iter().rev().fold(0, fold)
        }
    };
    let shoff = field(shoff_at, shoff_len);
    let shentsize = field(shentsize_at, 2);
    let shnum = field(shentsize_at + 2, 2);
    let offset = shoff.checked_add(shnum * shentsize)?;
    debug!("payload_offset: shoff={shoff}, shnum={shnum}, shentsize={shentsize} -> {offset}");
    (offset > 0).then_some(offset)
}

/// Identifies the filesystem image at `offset` by its magic bytes.
pub fn format_at(file: &File, offset: u64) -> Option<Format> {
    let mut magic = [0u8; 6];
    let n = file.read_at(&mut magic, offset).unwrap_or(0);
    let format = if n >= 4 && magic[..4] == *b"hsqs" {
        Some(Format::Squashfs)
    } else if n == 6 && magic == *b"DWARFS" {
        Some(Format::Dwarfs)
    } else {
        None
    };
    debug!("format_at: {format:?} at offset {offset}");
    format
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    /// ELF header with only the fields `payload_offset` reads filled in.
    fn header(class: u8, big_endian: bool, shoff: u64, shentsize: u16, shnum: u16) -> Vec<u8> {
        let (len, shoff_at, shoff_len, shentsize_at) = if class == 2 {
            (64, 40, 8, 58)
        } else {
            (52, 32, 4, 46)
        };
        let mut h = vec![0u8; len];
        h[..4].copy_from_slice(b"\x7fELF");
        h[4] = class;
        h[5] = if big_endian { 2 } else { 1 };
        let mut put = |at: usize, width: usize, value: u64| {
            let bytes = if big_endian {
                value.to_be_bytes()[8 - width..].to_vec()
            } else {
                value.to_le_bytes()[..width].to_vec()
            };
            h[at..at + width].copy_from_slice(&bytes);
        };
        put(shoff_at, shoff_len, shoff);
        put(shentsize_at, 2, shentsize.into());
        put(shentsize_at + 2, 2, shnum.into());
        h
    }

    fn temp_file(bytes: &[u8]) -> File {
        let mut file = tempfile::tempfile().unwrap();
        file.write_all(bytes).unwrap();
        file
    }

    #[test]
    fn offset_for_every_class_and_byte_order() {
        for class in [1, 2] {
            for big_endian in [false, true] {
                let file = temp_file(&header(class, big_endian, 1000, 64, 3));
                assert_eq!(
                    payload_offset(&file),
                    Some(1192),
                    "class {class}, BE {big_endian}"
                );
            }
        }
    }

    #[test]
    fn offset_rejects_bad_headers() {
        let mut bad_class = header(2, false, 1000, 64, 3);
        bad_class[4] = 3;
        let cases = [
            b"definitely not an ELF file".to_vec(),
            bad_class,
            header(2, false, 1000, 64, 3)[..40].to_vec(),
            header(2, false, u64::MAX, 64, 3),
            header(2, false, 0, 0, 0),
        ];
        for bytes in cases {
            assert_eq!(payload_offset(&temp_file(&bytes)), None);
        }
    }

    #[test]
    fn magic_detection() {
        let file = temp_file(b"0123hsqs....DWARFS..DWARF");
        assert_eq!(format_at(&file, 4), Some(Format::Squashfs));
        assert_eq!(format_at(&file, 12), Some(Format::Dwarfs));
        assert_eq!(format_at(&file, 0), None);
        assert_eq!(format_at(&file, 20), None);
        assert_eq!(format_at(&file, 1000), None);
    }
}
