#!/bin/sh
# post-install.sh - safely clear thumbnail cache and set MIME defaults as the non-root user
# This script is intended to be executed during `ninja install`, or by a package manager's
# post-install script. It will try to detect the appropriate non-root user to operate on
# and use `su`/`runuser` to run user-level commands.

set -eu

log() {
  printf 'post-install: %s\n' "$*" >&2
}

# Determine the target non-root user. Prefer the invoking user if not root.
if [ "$(id -u)" -ne 0 ]; then
  TARGET_USER="$(id -un)"
else
  TARGET_USER="${SUDO_USER:-}"
  if [ -z "$TARGET_USER" ]; then
    # Fall back to logname which may give the login name; it can fail on non-login shells.
    TARGET_USER="$(logname 2>/dev/null || true)"
  fi
fi

# If we still have no non-root user, there's nothing to do for user-specific caches.
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
  log "no non-root user detected; skipping user cache and MIME updates"
  exit 0
fi

# Resolve home directory for the target user
USER_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
if [ -z "$USER_HOME" ]; then
  log "could not determine home directory for user '$TARGET_USER'; aborting"
  exit 0
fi

# Try to obtain XDG_CACHE_HOME from a running process environment for that user.
USER_XDG_CACHE=""
for pid in $(pgrep -u "$TARGET_USER" || true); do
  if [ -r "/proc/$pid/environ" ]; then
    val="$(tr '\0' '\n' < /proc/$pid/environ | awk -F= '$1=="XDG_CACHE_HOME" {print $2; exit}')"
    if [ -n "$val" ]; then
      USER_XDG_CACHE="$val"
      break
    fi
  fi
done

if [ -z "$USER_XDG_CACHE" ]; then
  # default fallback
  USER_XDG_CACHE="$USER_HOME/.cache"
fi

THUMBNAIL_DIR="$USER_XDG_CACHE/thumbnails"
if [ -d "$THUMBNAIL_DIR" ]; then
  log "clearing thumbnail cache in $THUMBNAIL_DIR for user '$TARGET_USER'"
  # Remove as the target user to avoid permission issues and avoid touching root files.
  if command -v runuser >/dev/null 2>&1; then
    runuser -l "$TARGET_USER" -c "rm -rf \"$THUMBNAIL_DIR\"/*"
  else
    su -s /bin/sh "$TARGET_USER" -c "rm -rf \"$THUMBNAIL_DIR\"/*" || true
  fi
else
  log "no thumbnail cache directory found at $THUMBNAIL_DIR; nothing to clear"
fi

# If xdg-mime is available, try to set a MIME default for application/vnd.appimage
if command -v xdg-mime >/dev/null 2>&1; then
  DESKTOP_FILENAME="appimage-thumbnailer.desktop"
  SYSTEM_DESKTOP="/usr/share/applications/$DESKTOP_FILENAME"
  USER_DESKTOP="$USER_HOME/.local/share/applications/$DESKTOP_FILENAME"

  if [ -f "$SYSTEM_DESKTOP" ] || [ -f "$USER_DESKTOP" ]; then
    log "setting MIME default for application/vnd.appimage to $DESKTOP_FILENAME for user '$TARGET_USER'"
    if command -v runuser >/dev/null 2>&1; then
      runuser -l "$TARGET_USER" -c "xdg-mime default $DESKTOP_FILENAME application/vnd.appimage || true"
    else
      su -s /bin/sh "$TARGET_USER" -c "xdg-mime default $DESKTOP_FILENAME application/vnd.appimage || true" || true
    fi
  else
    log "no $DESKTOP_FILENAME desktop file installed system-wide or in the user's local applications; skipping xdg-mime"
  fi
else
  log "xdg-mime is not available; skipping MIME default setup"
fi

exit 0
