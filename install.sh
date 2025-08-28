#!/bin/bash

# Exit on error
set -e

# Check if running with sudo/root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root or with sudo"
    exit 1
fi

# Create directories if they don't exist
mkdir -p /usr/local/bin
mkdir -p /usr/share/thumbnailers

# Copy the thumbnailer script
echo "Installing appimage-thumbnailer to /usr/local/bin..."
install -m 755 appimage-thumbnailer /usr/local/bin/appimage-thumbnailer

# Copy the thumbnailer configuration
echo "Installing thumbnailer configuration to /usr/share/thumbnailers..."
install -m 644 appimage-thumbnailer.thumbnailer /usr/share/thumbnailers/appimage-thumbnailer.thumbnailer

echo "Installation completed successfully!"
echo "Now log out for thumbnailer to start" 
