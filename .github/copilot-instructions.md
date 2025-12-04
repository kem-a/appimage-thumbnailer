# Copilot Instructions

## Project Snapshot
- Two entrypoints exist: the shipping Bash script `appimage-thumbnailer` plus the Meson-built C binary in `src/main.c`. Both accept `appimage-thumbnailer <AppImage> <output.png> [size]` and must only exit `0` after writing `%o`.
- `.thumbnailer` wires the helper into the freedesktop thumbnailer spec; whenever MIME types change, update both this file and any installer copy logic.
- `install.sh`/`uninstall.sh` are the supported deployment paths. They assume sudo, install runtime deps per package manager, copy the script to `/usr/local/bin`, and flush `~/.cache/thumbnails` for the invoking user so new icons appear immediately.

## Icon Extraction Flow
- Inputs are canonicalized via `realpath`/`g_canonicalize_filename`; callers should feed real files, not symlinks. Size defaults to 256px when absent or invalid and clamps to 1–4096 (see `parse_size_argument`).
- Extraction is streaming: `7z e -so` pipes bytes directly into converters. SVG payloads are rendered through `rsvg-convert` (Bash) or librsvg+cairo (`process_svg_payload`); raster payloads go through ImageMagick or GdkPixbuf. Never introduce temp files.
- Primary lookup resolves `.DirIcon`, following text indirections up to `MAX_DIRICON_CHAIN` (8) and bailing with a logged error if no actual image appears. Keep new heuristics in this fast path when possible.
- Fallback lookup lists archive contents, picks the first `.desktop`, parses `Icon=` and searches `<icon>{,.svg,.png,.xpm}` alongside common icon roots (`.local/share/icons`, `usr/share/icons`, `usr/share/pixmaps`). Preserve this order so we try explicit matches before exhaustive scans.
- Both implementations emit actionable `stderr` and exit `1` when no icon is found; desktop environments rely on that failure to fall back to generic thumbnails.

## Build & Runtime Dependencies
- Script runtime: `7z`, `file`, `base64`, ImageMagick (`magick`/`convert`), and `rsvg-convert`. Any new dependency must be checked in `install.sh` for apt/dnf/pacman/zypper plus documented in `README.md`.
- Native build: Meson + Ninja compile `src/main.c` against GLib (gio), GdkPixbuf, librsvg, Cairo, and optionally libm. Run `meson setup build && ninja -C build && sudo ninja -C build install` to install the binary; this path installs `appimage-thumbnailer.thumbnailer` under `$datadir/thumbnailers` as well.

## Daily Workflows
- Fast manual test: `./appimage-thumbnailer sample.AppImage /tmp/icon.png 128` then view the PNG; expect exit code `0` only if `/tmp/icon.png` exists.
- Integration smoke: `sudo ./install.sh`, open a file manager showing a directory of AppImages, and verify thumbnails refresh (clear caches with `rm -rf ~/.cache/thumbnails/*` if needed).
- Removal: `sudo ./uninstall.sh` cleans `/usr/local/bin/appimage-thumbnailer` and the thumbnailer descriptor; rerun `install.sh` when validating idempotency.

## Conventions & Gotchas
- Stick to Bash features already in use (arrays, `[[ ]]`, process substitution) so the script remains portable; mirror the same behavior in `src/main.c` for parity.
- Keep every icon-processing path funneled through `process_icon`/`process_icon_payload` so format detection, base64 decoding, scaling, and error messaging stay consistent.
- Avoid assumptions about archive compression; zstd AppImages are explicitly unsupported (documented in `README.md`).
- Any new heuristics should short-circuit on success and log why a branch failed, matching current `stderr` tone.
- When editing installer logic, remember it prompts before installing packages and runs under sudo—never silently install.
