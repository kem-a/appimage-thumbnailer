# AppImage Thumbnailer

An in-process thumbnailer that extracts AppImage icons and writes ready-to-use PNG thumbnails for desktop environments implementing the freedesktop.org spec.

## Features
- Resolves `.DirIcon` pointers (with bounded symlink depth) before falling back to the first root-level `.svg` and then `.png` inside the AppImage.
- Streams archive data directly from `7z` into librsvg/GdkPixbuf without temporary files.
- Preserves aspect ratio, never upscales unnecessarily, and honors requested sizes inside the safe `1–4096` range (default `256`).
- Ships `appimage-thumbnailer.thumbnailer` so file managers can dispatch the helper automatically once installed.

## Prerequisites
- Tooling: `meson` (>=0.59) and `ninja` for builds.
- Runtime: `7z` plus GLib/GIO (>=2.56), GdkPixbuf (>=2.42), librsvg (>=2.54), Cairo, and libm (optional but detected).
- Platform: a freedesktop.org-compliant thumbnail cache (GNOME, KDE, etc.).

## Build & Install
```bash
git clone https://github.com/kem-a/appimage-thumbnailer.git
cd appimage-thumbnailer
meson setup build
ninja -C build
sudo ninja -C build install
```
Installation drops the `appimage-thumbnailer` binary and `appimage-thumbnailer.thumbnailer` descriptor under your Meson `prefix` (default `/usr/local`).

Uninstall with `sudo ninja -C build uninstall` using the same build directory.

## Testing
Run the helper manually to test before wiring into a desktop:
```bash
./build/appimage-thumbnailer Sample.AppImage /tmp/icon.png 256
```
- Argument order is `<AppImage> <output.png> [size]`.
- The command exits `0` only after writing a valid PNG to `%o`; failures emit diagnostics on stderr so file managers can fall back to generic icons.

## File Manager Integration
Installing via Meson registers the `.thumbnailer` file automatically. If icons do not refresh immediately, clear cached entries and reopen your file manager:
```bash
rm -rf ~/.cache/thumbnails/*
```

## License
This project is licensed under the MIT License. See `LICENSE` for details.
