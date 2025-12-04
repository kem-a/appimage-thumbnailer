# Copilot Instructions

## Repo Snapshot
- `appimage-thumbnailer` is a Bash executable invoked by the freedesktop thumbnailer interface (`appimage-thumbnailer %i %o %s`). It must exit `0` only after writing the thumbnail image to `%o`.
- `.thumbnailer` declares MIME types (`application/vnd.appimage`, etc.) and must stay in sync with `install.sh` so the desktop caches load the right helper.
- `install.sh`/`uninstall.sh` are the only supported deployment paths; they expect root, install dependencies via distro-specific package managers, and purge `~/.cache/thumbnails` for the invoking user.

## How Thumbnail Extraction Works
- Inputs are normalized immediately with `realpath` so always pass real files, not symlinks.
- Size is optional; missing or non-numeric values default to `256` pixels. Respect this default when adding code paths.
- `process_icon` streams files out of the AppImage via `7z e -so` and inspects them with `file -`. SVGs route through `rsvg-convert`, PNGs through `magick ... -resize`. Keep new conversions streaming to avoid temp files.
- Primary lookup: try `.DirIcon`, following indirections (files that contain the name of another icon). The loop stops only when an actual image is exported.
- Fallback lookup: locate the first `.desktop` entry inside the AppImage, read its `Icon=` key, and try `<icon>.{svg,png,xpm}` plus common icon roots (`.local/share/icons`, `usr/share/icons`, `usr/share/pixmaps`). Preserve this order so the fastest paths run first.
- The script emits actionable `stderr` messages and exits `1` if no icon is found; callers rely on this to fall back to generic icons.

## Dependencies & Tooling
- Runtime requirements: `7z`, `file`, `base64`, `magick` (ImageMagick), and `rsvg-convert`. `install.sh` maps these to distro packages (apt/dnf/pacman/zypper); add new dependencies to every branch of the installer.
- No Meson/CMake build is needed—the repo ships plain scripts. Ignore the generated `build/` folder unless you intentionally add a compiled helper.

## Daily Workflows
- Local test without installing: `./appimage-thumbnailer sample.AppImage /tmp/icon.png 128` then open the PNG; ensure the command exits `0`.
- Integration smoke test after code changes: run `sudo ./install.sh`, open a file manager pointed at a folder of AppImages, and confirm thumbnails refresh (or clear caches with `rm -rf ~/.cache/thumbnails/*`).
- Uninstall via `sudo ./uninstall.sh` before packaging or when validating installer idempotency.

## Conventions & Gotchas
- Stay POSIX-friendly but the script already depends on Bash-only features (arrays, `[[ ]]`); keep new logic Bash-compatible.
- Avoid temporary files; the current approach keeps everything in pipes for speed and for AppImages mounted on slow media.
- Do not attempt to support zstd-compressed AppImages inside this script (see README); document that limitation in commit messages when relevant.
- Any new heuristics should follow the current "try fast path, short-circuit on success" model, and must call `process_icon` so conversions stay centralized.
- When touching installer logic, remember it always runs under sudo and should keep prompting the user before package installs.
