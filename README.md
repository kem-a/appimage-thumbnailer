# AppImage Thumbnailer

A simple and efficient thumbnailer for AppImage files that extracts and displays icons as thumbnails in file managers.

## Features

- Extracts `.DirIcon` from AppImage files
- Supports PNG and SVG icon formats
- Automatically resizes thumbnails to requested size
- Integrates with system file managers
- Does not support zstd compressed AppImage files (use [appimageinfo](https://github.com/kem-a/appimageinfo) to check)

## Requirements

- `7z` (p7zip)
- `ImageMagick`
- `base64`

## Installation

1. Clone this repository:
```bash
git clone https://github.com/yourusername/appimage-thumbnailer
cd appimage-thumbnailer
```
2. Install the thumbnailer:
```bash
sudo sh install.sh
```

## Uninstallation
To remove the thumbnailer:
```bash
sudo sh uninstall.sh
```

## How It Works
The thumbnailer extracts the .DirIcon from AppImage files using 7zip, converts it to the appropriate size using ImageMagick, and saves it as a thumbnail that your file manager can display.

## File Manager Integration
The thumbnailer is automatically integrated with file managers that support the freedesktop.org thumbnail specification through the installed .thumbnailer file in thumbnailers.

## License
This project is licensed under the MIT License. See the LICENSE file for details.

## Contributing
Contributions are welcome! Please feel free to submit a Pull Request.