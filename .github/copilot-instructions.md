# Copilot Instructions

## Architecture Snapshot
- Only one entrypoint ships: the Meson-built C binary `src/appimage-thumbnailer.c`, invoked as `appimage-thumbnailer <AppImage> <output.png> [size]` and expected to exit `0` only after writing `%o`.
- `appimage-thumbnailer.thumbnailer` is the sole integration point; keep its MIME list in sync with any runtime changes because desktop shells rely on it to dispatch the helper.
- The code wraps `7z` via `command_capture`/`extract_entry`, keeps everything in-memory, and never writes temp files—maintain that streaming contract when adding formats or tooling.

## Icon Extraction Flow
- Every path is canonicalized (`realpath` + `g_canonicalize_filename`) before use; mirror that when accepting new inputs.
- `parse_size_argument` clamps requested sizes to `1–4096` with a default of `256`. Respect this clamp when introducing new call-sites.
- Resolution order lives in `main`: try `.DirIcon` first (following pointer-like text indirections via `process_entry_following_symlinks` up to `MAX_SYMLINK_DEPTH`), then fall back to the first root `.svg`, then `.png` discovered through `list_archive_paths`/`find_root_with_extension`.
- SVG payloads travel through `process_svg_payload` (librsvg + Cairo). Raster payloads hit `GdkPixbufLoader` and `scale_pixbuf`, which preserve aspect ratio and only upscale when necessary. Always reuse `process_icon_payload` so both pipelines stay in sync.

## Build & Manual Test Workflow
- Configure and build with Meson/Ninja: `meson setup build` (once) then `ninja -C build`. Install to your prefix with `sudo ninja -C build install` to drop the binary plus `.thumbnailer` under `$datadir/thumbnailers`.
- Quick verification: `./build/appimage-thumbnailer sample.AppImage /tmp/icon.png 256` and ensure the command exits `0` only when `/tmp/icon.png` exists and is a valid PNG.
- Desktop caches rarely refresh instantly; when validating integration, clear GNOME caches with `rm -rf ~/.cache/thumbnails/*` before reopening Nautilus.

## Dependencies & External Tools
- Runtime requires `7z` plus the libraries declared in `meson.build`: GLib/GIO (2.56+), GdkPixbuf (2.42+), librsvg (2.54+), Cairo, and libm (optional). If you add more, update both the top-level `meson.build` and `README.md`.
- The helper does not ship installer scripts in this branch; assume Meson-based deployment when documenting or automating setup steps.

## Implementation Conventions
- Error handling is user-facing: log actionable messages with `g_printerr` and propagate failures all the way to `main` so desktop environments can fall back cleanly.
- Keep archive operations streaming—`command_capture` already drains stdout to memory; avoid commands that would require seeking inside the AppImage.
- Respect the pointer guardrails in `is_pointer_candidate`/`MAX_SYMLINK_DEPTH` to prevent infinite loops when chasing `.DirIcon` indirections.
- When adding heuristics for locating icons, short-circuit as soon as a thumbnail succeeds and consider updating both the `.DirIcon` fast path and the root extension scan to keep behavior predictable.
