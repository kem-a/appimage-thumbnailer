#!/bin/bash

# Exit on error
set -e

# Check if running with sudo/root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root or with sudo"
    exit 1
fi

# Function to check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to detect package manager and install packages
install_dependencies() {
    local missing_deps=("$@")
    
    echo "Missing dependencies: ${missing_deps[*]}"
    echo "Would you like to install them? (y/N)"
    read -r response
    
    if [[ ! "$response" =~ ^[Yy]$ ]]; then
        echo "Installation cancelled. Dependencies are required for the thumbnailer to work."
        exit 1
    fi
    
    # Detect package manager and install dependencies
    if command_exists apt-get; then
        echo "Installing dependencies using apt-get..."
        apt-get update
        for dep in "${missing_deps[@]}"; do
            case "$dep" in
                "7z") apt-get install -y p7zip-full ;;
                "convert") apt-get install -y imagemagick ;;
                "rsvg-convert") apt-get install -y librsvg2-bin ;;
                "base64") apt-get install -y coreutils ;;
            esac
        done
    elif command_exists dnf; then
        echo "Installing dependencies using dnf..."
        for dep in "${missing_deps[@]}"; do
            case "$dep" in
                "7z") dnf install -y p7zip p7zip-plugins ;;
                "convert") dnf install -y ImageMagick ;;
                "rsvg-convert") dnf install -y librsvg2-tools ;;
                "base64") dnf install -y coreutils ;;
            esac
        done
    elif command_exists pacman; then
        echo "Installing dependencies using pacman..."
        for dep in "${missing_deps[@]}"; do
            case "$dep" in
                "7z") pacman -S --noconfirm p7zip ;;
                "convert") pacman -S --noconfirm imagemagick ;;
                "rsvg-convert") pacman -S --noconfirm librsvg ;;
                "base64") pacman -S --noconfirm coreutils ;;
            esac
        done
    elif command_exists zypper; then
        echo "Installing dependencies using zypper..."
        for dep in "${missing_deps[@]}"; do
            case "$dep" in
                "7z") zypper install -y p7zip ;;
                "convert") zypper install -y ImageMagick ;;
                "rsvg-convert") zypper install -y librsvg-tools ;;
                "base64") zypper install -y coreutils ;;
            esac
        done
    else
        echo "Error: Could not detect package manager. Please install the following packages manually:"
        echo "- p7zip (for 7z command)"
        echo "- ImageMagick (for convert command)"
        echo "- librsvg tools (for rsvg-convert command)"
        echo "- coreutils (for base64 command)"
        exit 1
    fi
}

# Check for dependencies
echo "Checking dependencies..."
missing_deps=()

if ! command_exists 7z; then
    missing_deps+=("7z")
fi

if ! command_exists convert; then
    missing_deps+=("convert")
fi

if ! command_exists rsvg-convert; then
    missing_deps+=("rsvg-convert")
fi

if ! command_exists base64; then
    missing_deps+=("base64")
fi

# Install missing dependencies if any
if [ ${#missing_deps[@]} -gt 0 ]; then
    install_dependencies "${missing_deps[@]}"
    echo "Dependencies installed successfully!"
else
    echo "All dependencies are already installed."
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

# Clean thumbnail cache to ensure fresh thumbnails are generated
echo "Cleaning thumbnail cache..."
if [ -n "$SUDO_USER" ]; then
    # If running with sudo, clean cache for the actual user
    sudo -u "$SUDO_USER" rm -rf /home/"$SUDO_USER"/.cache/thumbnails/*
else
    # If running as root directly, clean root's cache
    rm -rf ~/.cache/thumbnails/*
fi

echo "Installation completed successfully!"
echo "Log out needed for changes to take effect."