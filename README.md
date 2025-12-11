<!-- Core project info -->
[![Download](https://img.shields.io/badge/Download-latest-blue)](https://github.com/kem-a/appimage-thumbnailer/releases/latest)
[![Release](https://img.shields.io/github/v/release/kem-a/appimage-thumbnailer?semver)](https://github.com/kem-a/appimage-thumbnailer/releases/latest)
[![License](https://img.shields.io/github/license/kem-a/appimage-thumbnailer)](https://github.com/kem-a/appimage-thumbnailer/blob/main/LICENSE)
![C](https://img.shields.io/badge/C-17-blue)
[![Stars](https://img.shields.io/github/stars/kem-a/appimage-thumbnailer?style=social)](https://github.com/kem-a/appimage-thumbnailer/stargazers)


# AppImage Thumbnailer

An in-process thumbnailer that extracts AppImage icons and writes ready-to-use PNG thumbnails for desktop environments implementing the freedesktop.org spec.

<img width="815" height="480" alt="Screenshot From 2025-12-05 00-38-50" src="https://github.com/user-attachments/assets/d5a99e85-71d3-47dd-9174-fc0c56e43d13" />


## Features
- Supports both **SquashFS** (traditional) and **DwarFS** AppImage formats.
- Resolves `.DirIcon` pointers (with bounded symlink depth) before falling back to the first root-level `.svg` and then `.png` inside the AppImage.
- Streams archive data directly from `7z` into librsvg/GdkPixbuf without temporary files.
- Preserves aspect ratio while scaling toward the requested size (upscales when the source is smaller) within the safe `1–4096` range (default `256`).
- Ships `appimage-thumbnailer.thumbnailer` so file managers can dispatch the helper automatically once installed.

## Prerequisites
- Tooling: `meson` (>=1.1) and `ninja` for builds.
- Runtime (at least one required):
	- `7z`/`p7zip-full` for SquashFS AppImages (traditional format).
	- `dwarfs` tools (`dwarfsextract`, `dwarfsck`) for DwarFS AppImages — bundled automatically during build.
- Linked system libraries (usually present on major distros): GLib/GIO (>=2.56), GdkPixbuf (>=2.42), librsvg (>=2.54), Cairo, and libm (optional but detected).
- Platform: a freedesktop.org-compliant thumbnail cache (GNOME, KDE, etc.).

<details> <summary> Installing Dependencies <b>(click to open)</b> </summary>

**Fedora / RHEL / CentOS:**
```bash
sudo dnf install meson ninja-build p7zip glib2-devel gdk-pixbuf2-devel librsvg2-devel cairo-devel
```

**Ubuntu / Debian:**
```bash
sudo apt install meson ninja-build p7zip-full libglib2.0-dev libgdk-pixbuf-2.0-dev librsvg2-dev libcairo2-dev
```

**Arch Linux:**
```bash
sudo pacman -S meson ninja p7zip glib2 gdk-pixbuf2 librsvg cairo
```
</details> 

### DwarFS Support
[DwarFS](https://github.com/mhx/dwarfs) is a high-compression read-only filesystem that offers significantly better compression ratios than SquashFS. Some modern AppImages (e.g., those created with [uruntime](https://github.com/VHSgunzo/uruntime) or [PELF](https://github.com/xplshn/pelf)) use DwarFS instead of SquashFS.

**DwarFS tools are bundled automatically** during the build process — static binaries (`dwarfsextract`, `dwarfsck`) are downloaded from the [official releases](https://github.com/mhx/dwarfs/releases) and installed alongside the thumbnailer. No additional installation is required.

To disable bundling (e.g., for distro packaging where you want to use system-provided dwarfs):
```bash
meson setup build -Dbundle_dwarfs=false
```

## Build & Install
```bash
git clone https://github.com/kem-a/appimage-thumbnailer.git
cd appimage-thumbnailer
meson setup build
ninja -C build
sudo ninja -C build install
```
Installation drops the `appimage-thumbnailer` binary and `appimage-thumbnailer.thumbnailer` descriptor under your Meson `prefix` (default `/usr/local`).

Uninstall with `ninja -C build uninstall` (add `sudo` if you installed system-wide) using the same build directory.

## Testing
Run the helper manually to test before wiring into a desktop:
```bash
./build/appimage-thumbnailer Sample.AppImage /tmp/icon.png 256
```
- Argument order is `<AppImage> <output.png> [size]`.
- The command exits `0` only after writing a valid PNG to `%o`; failures emit diagnostics on stderr so file managers can fall back to generic icons.

## Troubleshooting

1. If icons do not refresh immediately, clear cached entries and reopen your file manager:
```bash
rm -rf ~/.cache/thumbnails/*
```
2. Run thumbnailer manually to test if icon is extracted
```bash
appimage-thumbnailer sample.AppImage icon.png 256
```

## License
This project is licensed under the MIT License. See `LICENSE` for details.
