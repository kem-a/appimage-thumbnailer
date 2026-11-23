//! freedesktop.org thumbnailer for AppImages. Reads `.DirIcon` from the
//! SquashFS or DwarFS payload in-process and writes a square PNG thumbnail.

use std::env;
use std::ffi::OsString;
use std::fs::File;
use std::path::Path;
use std::process::ExitCode;
use std::sync::OnceLock;

/// Step tracing, enabled like GLib's g_debug with `G_MESSAGES_DEBUG=all`.
macro_rules! debug {
    ($($arg:tt)*) => {
        if $crate::debug_enabled() {
            eprintln!("DEBUG: {}", format_args!($($arg)*));
        }
    };
}

mod elf;
mod extract;
mod render;

use elf::Format;

const DEFAULT_SIZE: u32 = 256;

fn debug_enabled() -> bool {
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| {
        env::var("G_MESSAGES_DEBUG").is_ok_and(|v| v.split([' ', ',']).any(|d| d == "all"))
    })
}

fn main() -> ExitCode {
    let args: Vec<OsString> = env::args_os().collect();
    let prog = args
        .first()
        .map_or("appimage-thumbnailer".into(), |a| a.to_string_lossy());

    if args.len() == 2 {
        match args[1].to_str() {
            Some("-h" | "--help") => {
                print_usage(&prog);
                return ExitCode::SUCCESS;
            }
            Some("-V" | "--version") => {
                print_version();
                return ExitCode::SUCCESS;
            }
            _ => {}
        }
    }

    if !(3..=4).contains(&args.len()) {
        eprintln!("Usage: {prog} <AppImage> <output.png> [size]");
        return ExitCode::FAILURE;
    }

    let size = parse_size(args.get(3));
    match run(Path::new(&args[1]), Path::new(&args[2]), size) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::FAILURE
        }
    }
}

fn run(input: &Path, output: &Path, size: u32) -> Result<(), String> {
    let file =
        File::open(input).map_err(|e| format!("Failed to open '{}': {e}", input.display()))?;
    let offset = elf::payload_offset(&file).ok_or("Not an AppImage: no ELF payload found")?;
    debug!(
        "run: input='{}', size={size}, payload offset={offset}",
        input.display()
    );

    // A uruntime metadata sidecar (SquashFS) can sit in front of the DwarFS
    // payload, and its .DirIcon may be missing or point into that payload.
    let sidecar = offset.saturating_add(elf::SIDECAR_SLOT);
    let images = [
        elf::format_at(&file, offset).map(|f| (f, offset)),
        (elf::format_at(&file, sidecar) == Some(Format::Dwarfs))
            .then_some((Format::Dwarfs, sidecar)),
    ];

    let mut error = String::from("payload is neither SquashFS nor DwarFS");
    for (format, at) in images.into_iter().flatten() {
        debug!("run: trying .DirIcon from {format:?} at offset {at}");
        let icon = file
            .try_clone()
            .map_err(|e| e.to_string())
            .and_then(|f| extract::Image::open(f, format, at))
            .and_then(|mut image| image.read_following_links(".DirIcon"));
        match icon {
            Ok(data) => return render::write_thumbnail(&data, size, output),
            Err(e) => {
                debug!("run: {e}");
                error = e;
            }
        }
    }
    Err(format!("Failed to extract .DirIcon from AppImage: {error}"))
}

/// Parses like strtol: leading digits count. Anything else, or a value
/// outside 1-4096, gives the default size.
fn parse_size(arg: Option<&OsString>) -> u32 {
    let Some(text) = arg.and_then(|a| a.to_str()) else {
        return DEFAULT_SIZE;
    };
    let text = text.trim_start();
    let text = text.strip_prefix('+').unwrap_or(text);
    let end = text
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(text.len());
    match text[..end].parse::<u32>() {
        Ok(n) if (1..=4096).contains(&n) => n,
        _ => DEFAULT_SIZE,
    }
}

fn print_usage(prog: &str) {
    print!(
        "\
Usage: {prog} [OPTIONS] <APPIMAGE> <OUTPUT> [SIZE]

Extract the embedded icon from an AppImage and write it as a PNG thumbnail.
SquashFS and DwarFS AppImages are read in-process, no external tools needed.

Arguments:
  <APPIMAGE>        Path to the AppImage file
  <OUTPUT>          Path to the output PNG thumbnail
  [SIZE]            Thumbnail size in pixels (default: 256, range: 1-4096)

Options:
  -h, --help        Print this help message and exit
  -V, --version     Print version information and exit

Examples:
  {prog} app.AppImage thumbnail.png
  {prog} app.AppImage thumbnail.png 128

Conforms to the freedesktop.org thumbnail specification:
  <https://specifications.freedesktop.org/thumbnail-spec/latest>

License:
  MIT License
  Copyright (c) Arnis Kemlers
  <https://github.com/kem-a/appimage-thumbnailer>

Third-party components:
  SquashFS reading by backhand <https://github.com/wcampbell0x2a/backhand>
  DwarFS reading by dwarfs-rs <https://github.com/oxalica/dwarfs-rs>
  SVG rendering by resvg <https://github.com/linebender/resvg>
  All three are distributed under the MIT and Apache-2.0 licenses.
"
    );
}

fn print_version() {
    println!("appimage-thumbnailer v{}", env!("CARGO_PKG_VERSION"));
    println!("Copyright (c) Arnis Kemlers");
    println!("License: MIT");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn size_argument() {
        let size = |s: &str| parse_size(Some(&OsString::from(s)));
        assert_eq!(parse_size(None), 256);
        assert_eq!(size("128"), 128);
        assert_eq!(size(" +64px"), 64);
        assert_eq!(size("4096"), 4096);
        for bad in ["", "abc", "0", "-5", "4097", "99999999999999999999"] {
            assert_eq!(size(bad), 256, "{bad:?}");
        }
    }
}
