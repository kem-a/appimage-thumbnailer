# AppImage Thumbnailer

<!-- Core project info -->
[![Download](https://img.shields.io/badge/Download-latest-blue)](https://github.com/kem-a/appimage-thumbnailer/releases/latest)
[![Release](https://img.shields.io/github/v/release/kem-a/appimage-thumbnailer?semver)](https://github.com/kem-a/appimage-thumbnailer/releases/latest)
[![License](https://img.shields.io/github/license/kem-a/appimage-thumbnailer)](https://github.com/kem-a/appimage-thumbnailer/blob/main/LICENSE)
![Rust](https://img.shields.io/badge/Rust-2024-orange)
[![Stars](https://img.shields.io/github/stars/kem-a/appimage-thumbnailer?style=social)](https://github.com/kem-a/appimage-thumbnailer/stargazers)

An in-process thumbnailer that extracts AppImage icons and writes ready-to-use PNG thumbnails for desktop environments implementing the freedesktop.org spec.

<img width="815" height="480" alt="Screenshot From 2025-12-05 00-38-50" src="https://github.com/user-attachments/assets/d5a99e85-71d3-47dd-9174-fc0c56e43d13" />

## Features

- Supports both **SquashFS** (traditional) and **DwarFS** AppImage formats.
- Resolves `.DirIcon` pointers (with bounded symlink depth).
- Reads SquashFS (gzip, xz, zstd, lz4) and DwarFS (zstd, lzma, lz4) in-process: no `unsquashfs` or `dwarfsextract` needed.
- Renders a subtle drop shadow beneath each icon (enabled by default, see [Build options](#build-options)).

## Prerequisites

- Build: Rust 1.90 or newer with `cargo`, a C compiler (the bundled zstd, xz and lz4 decoders are compiled from source) and `make`.
- Runtime: no extra packages. The binary links only glibc and libgcc_s.
- Platform: a freedesktop.org-compliant thumbnail cache (GNOME, KDE, etc.).

## Build & Install

<details> <summary> Installing Dependencies <b>(click to open)</b> </summary>

If your distro's `cargo` is older than 1.90 (`cargo --version`), install Rust with [rustup](https://rustup.rs) instead.

**Fedora / RHEL / CentOS:**

```bash
sudo dnf install cargo gcc make
```

**Ubuntu / Debian:**

```bash
sudo apt install cargo build-essential
```

**Arch Linux:**

```bash
sudo pacman -S rust gcc make
```

</details>

```bash
git clone https://github.com/kem-a/appimage-thumbnailer.git
cd appimage-thumbnailer
make
sudo make install
```

Installation drops the `appimage-thumbnailer` binary and `appimage-thumbnailer.thumbnailer` descriptor under `PREFIX` (default `/usr/local`), e.g. `sudo make install PREFIX=/usr`.

Uninstall with `sudo make uninstall`, using the same `PREFIX`.

Upgrading from a 4.x build made with Meson: the bundled `unsquashfs` and `dwarfsextract` under `$prefix/$libdir/appimage-thumbnailer` are no longer used. Run `sudo ninja -C build uninstall` in the old build directory first, or delete that folder.

Run the tests with `make check`.

### Build options

| Option | Default | Description |
| --- | --- | --- |
| `shadow` Cargo feature | on | Render a subtle drop shadow beneath the thumbnail icon. |

Build without the shadow with `make CARGO_FLAGS=--no-default-features`.

## (Optional) Remove thumbnail background

Remove checkered alpha channel drawing around thumbnails and icons in Nautilus. Creates more cleaner look.

Edit `~/.config/gtk-4.0/gtk.css` file and add CSS code snippet to it:

```css
/*Clear Nautilus thumbnail background*/
.thumbnail,
.icon .thumbnail,
.grid-view .thumbnail {
  background: none;
  box-shadow: none;
}
```

## More help

Type `appimage-thumbnailer --help` for more info

## Troubleshooting

1. If icons do not refresh immediately, clear cached entries and reopen your file manager:

```bash
rm -rf ~/.cache/thumbnails/*
```

1. Run thumbnailer manually to test if icon is extracted

```bash
appimage-thumbnailer sample.AppImage icon.png 256
G_MESSAGES_DEBUG=all appimage-thumbnailer sample.AppImage icon.png 256   # trace every step
```

## Known issues

- DwarFS appimage thumbmnails doesn't show icons in Dolphin

Must uncheck "AppImage" under Settings -> Configure Dolphin... -> General -> Previews. Only "AppImage application bundle" needs to be checked

## License

This project is licensed under the MIT License. See `LICENSE` for details.
