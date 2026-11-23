//! Reads one entry from the SquashFS or DwarFS image inside an AppImage.

use std::fs::File;
use std::io::{BufReader, Read};
use std::path::Path;

use backhand::{FilesystemReader, InnerNode};
use dwarfs::{Archive, ArchiveIndex, AsChunks, InodeKind};
use positioned_io::Slice;

use crate::elf::Format;

/// Entries read while resolving one path, so at most 4 symlinks are followed.
const MAX_READS: usize = 5;

// Only one Image exists per run, so the size gap between variants does not matter.
#[allow(clippy::large_enum_variant)]
pub enum Image {
    Squashfs(FilesystemReader<'static>),
    Dwarfs(ArchiveIndex, Archive<Slice<File>>),
}

enum Entry {
    Data(Vec<u8>),
    Link(String),
}

impl Image {
    pub fn open(file: File, format: Format, offset: u64) -> Result<Self, String> {
        match format {
            Format::Squashfs => {
                FilesystemReader::from_reader_with_offset(BufReader::new(file), offset)
                    .map(Image::Squashfs)
                    .map_err(|e| format!("cannot read SquashFS image: {e}"))
            }
            Format::Dwarfs => {
                let slice = Slice::new_to_end(file, offset).map_err(|e| e.to_string())?;
                let (index, archive) =
                    Archive::new(slice).map_err(|e| format!("cannot read DwarFS image: {e}"))?;
                Ok(Image::Dwarfs(index, archive))
            }
        }
    }

    /// Reads `path`, following symlinks inside the image.
    pub fn read_following_links(&mut self, path: &str) -> Result<Vec<u8>, String> {
        let mut current = resolve("", path);
        for _ in 0..MAX_READS {
            match self.read(&current)? {
                Entry::Data(data) => {
                    debug!("read_following_links: '{current}' is {} bytes", data.len());
                    return Ok(data);
                }
                Entry::Link(target) => {
                    let next = resolve(&current, &target);
                    debug!("read_following_links: '{current}' -> '{target}' ({next})");
                    current = next;
                }
            }
        }
        Err(format!("too many symlinks resolving '{path}'"))
    }

    fn read(&mut self, path: &str) -> Result<Entry, String> {
        let entry = match self {
            Image::Squashfs(fs) => {
                let full = Path::new("/").join(path);
                match fs.files().find(|n| n.fullpath == full).map(|n| &n.inner) {
                    Some(InnerNode::File(file)) => {
                        let mut data = Vec::new();
                        fs.file(file)
                            .reader_checked()
                            .map_err(|e| e.to_string())?
                            .read_to_end(&mut data)
                            .map_err(|e| e.to_string())?;
                        Some(Entry::Data(data))
                    }
                    Some(InnerNode::Symlink(link)) => {
                        Some(Entry::Link(link.link.to_string_lossy().into_owned()))
                    }
                    _ => None,
                }
            }
            Image::Dwarfs(index, archive) => {
                match index
                    .get_path(path.split('/'))
                    .map(|inode| inode.classify())
                {
                    Some(InodeKind::File(file)) => Some(Entry::Data(
                        file.read_to_vec(archive).map_err(|e| e.to_string())?,
                    )),
                    Some(InodeKind::Symlink(link)) => Some(Entry::Link(link.target().to_owned())),
                    _ => None,
                }
            }
        };
        entry.ok_or_else(|| format!("'{path}' is missing or not a file"))
    }
}

/// Joins a symlink target to the directory of `link` and normalizes it.
/// Absolute targets start at the image root, and ".." stops at the root.
fn resolve(link: &str, target: &str) -> String {
    let base = match link.rsplit_once('/') {
        Some((dir, _)) if !target.starts_with('/') => dir,
        _ => "",
    };
    let mut parts = Vec::new();
    for part in base.split('/').chain(target.split('/')) {
        match part {
            "" | "." => {}
            ".." => {
                parts.pop();
            }
            _ => parts.push(part),
        }
    }
    parts.join("/")
}

#[cfg(test)]
mod tests {
    use super::*;

    const FIXTURES: [&str; 7] = [
        "gzip.sqfs",
        "xz.sqfs",
        "zstd.sqfs",
        "lz4.sqfs",
        "zstd.dwarfs",
        "lzma.dwarfs",
        "lz4.dwarfs",
    ];

    fn open_fixture(name: &str) -> Image {
        let path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/data")
            .join(name);
        let format = if name.ends_with(".sqfs") {
            Format::Squashfs
        } else {
            Format::Dwarfs
        };
        Image::open(File::open(path).unwrap(), format, 0).unwrap_or_else(|e| panic!("{name}: {e}"))
    }

    #[test]
    fn resolve_targets() {
        assert_eq!(resolve("", ".DirIcon"), ".DirIcon");
        assert_eq!(resolve(".DirIcon", "app.png"), "app.png");
        assert_eq!(resolve("a/b/c.png", "../d.svg"), "a/d.svg");
        assert_eq!(resolve("a/b/c.png", "/x/y.png"), "x/y.png");
        assert_eq!(resolve("a/b/c.png", "./x//y.png"), "a/b/x/y.png");
        assert_eq!(resolve("a/c.png", "../../../x.png"), "x.png");
    }

    #[test]
    fn reads_every_fixture_format() {
        let mut first_svg: Option<Vec<u8>> = None;
        for name in FIXTURES {
            let mut image = open_fixture(name);
            let png = image.read_following_links(".DirIcon").unwrap();
            assert!(png.starts_with(b"\x89PNG"), "{name}");

            let links = [
                "usr/share/icons/hicolor/32x32/apps/rel.svg",
                "abs.svg",
                "dot.svg",
                "d1",
            ];
            for link in links {
                let svg = image
                    .read_following_links(link)
                    .unwrap_or_else(|e| panic!("{name} {link}: {e}"));
                assert!(
                    svg.starts_with(b"<svg") && svg.len() > 16384,
                    "{name} {link}"
                );
                assert_eq!(&svg, first_svg.get_or_insert(svg.clone()), "{name} {link}");
            }

            for bad in ["d0", "loop", "missing", "nope", "usr", ""] {
                assert!(image.read_following_links(bad).is_err(), "{name} {bad:?}");
            }
        }
    }
}
