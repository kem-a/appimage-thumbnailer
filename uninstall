#!/bin/bash

# Exit on error
set -e

# Check if running with sudo/root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root or with sudo"
    exit 1
fi

# Remove the thumbnailer script
echo "Removing appimage-thumbnailer from /usr/local/bin..."
rm -f /usr/local/bin/appimage-thumbnailer

# Remove the thumbnailer configuration
echo "Removing thumbnailer configuration from /usr/share/thumbnailers..."
rm -f /usr/share/thumbnailers/appimage-thumbnailer.thumbnailer

echo "Uninstallation completed successfully!"