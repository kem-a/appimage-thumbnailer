<!-- Core project info -->
[![Download](https://img.shields.io/badge/Download-latest-blue)](https://github.com/kem-a/appimage-thumbnailer/releases/latest)
[![Release](https://img.shields.io/github/v/release/kem-a/appimage-thumbnailer?semver)](https://github.com/kem-a/appimage-thumbnailer/releases/latest)
[![License](https://img.shields.io/github/license/kem-a/appimage-thumbnailer)](https://github.com/kem-a/appimage-thumbnailer/blob/main/LICENSE)
![C](https://img.shields.io/badge/C-11-blue)
[![Stars](https://img.shields.io/github/stars/kem-a/appimage-thumbnailer?style=social)](https://github.com/kem-a/appimage-thumbnailer/stargazers)

# AppImage Thumbnailer

An in-process thumbnailer that extracts AppImage icons and writes ready-to-use PNG thumbnails for desktop environments implementing the freedesktop.org spec.

<img width="815" height="480" alt="Screenshot From 2025-12-05 00-38-50" src="https://github.com/user-attachments/assets/d5a99e85-71d3-47dd-9174-fc0c56e43d13" />

## Features

- Supports both **SquashFS** (traditional) and **DwarFS** AppImage formats.
- Resolves `.DirIcon` pointers (with bounded symlink depth).

## Prerequisites

- Tooling: `meson` (>=1.1) and `ninja` for builds.
- Runtime requirements:
  - `unsquashfs` from [`squashfs-tools`](https://github.com/plougher/squashfs-tools) for SquashFS AppImages (traditional format). Available in all major distro repos. Optionally bundled at build time with `-Dbundle_squashfs=true`.
  - `dwarfsextract` from [`dwarfs`](https://github.com/mhx/dwarfs) for DwarFS AppImages — bundled automatically during build.
- Linked system libraries (usually present on major distros): GLib/GIO (>=2.56), GdkPixbuf (>=2.42), librsvg (>=2.54), Cairo, and libm (optional but detected).
- Platform: a freedesktop.org-compliant thumbnail cache (GNOME, KDE, etc.).

### DwarFS Support

[DwarFS](https://github.com/mhx/dwarfs) is a high-compression read-only filesystem that offers significantly better compression ratios than SquashFS. Some modern AppImages (e.g., those created with [uruntime](https://github.com/VHSgunzo/uruntime) or [PELF](https://github.com/xplshn/pelf)) use DwarFS instead of SquashFS.

**DwarFS tools are bundled automatically** during the build process — the static `dwarfsextract` binary is downloaded from the [official releases](https://github.com/mhx/dwarfs/releases) and installed alongside the thumbnailer. No additional installation is required.

To disable bundling (e.g., for distro packaging where you want to use system-provided dwarfs) use `meson setup build -Dbundle_dwarfs=false`

## Build & Install

<details> <summary> Installing Dependencies <b>(click to open)</b> </summary>

**Fedora / RHEL / CentOS:**

```bash
sudo dnf install meson ninja-build squashfs-tools glib2-devel gdk-pixbuf2-devel librsvg2-devel cairo-devel
```

Additional packages required when bundling unsquashfs (`-Dbundle_squashfs=true`) or dwarfsextract (`-Dbundle_dwarfs=true`, enabled by default):

```bash
sudo dnf install curl zlib-devel libzstd-devel xz-devel
```

**Ubuntu / Debian:**

```bash
sudo apt install meson ninja-build squashfs-tools libglib2.0-dev libgdk-pixbuf-2.0-dev librsvg2-dev libcairo2-dev
```

Additional packages required when bundling unsquashfs or dwarfsextract:

```bash
sudo apt install curl zlib1g-dev libzstd-dev liblzma-dev
```

**Arch Linux:**

```bash
sudo pacman -S meson ninja squashfs-tools glib2 gdk-pixbuf2 librsvg cairo
```

Additional packages required when bundling unsquashfs or dwarfsextract:

```bash
sudo pacman -S curl zlib zstd xz
```

</details>

```bash
git clone https://github.com/kem-a/appimage-thumbnailer.git
cd appimage-thumbnailer
meson setup build
ninja -C build
sudo ninja -C build install
```

Installation drops the `appimage-thumbnailer` binary and `appimage-thumbnailer.thumbnailer` descriptor under your Meson `prefix` (default `/usr/local`).

Uninstall with `sudo ninja -C build uninstall` using the same build directory.

## Remove thumbnail background

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
```

## License

This project is licensed under the MIT License. See `LICENSE` for details.
