#!/usr/bin/python3
# -*- coding: utf-8 -*-

import sys
import os
import subprocess
import base64
from pathlib import Path
import io
from PIL import Image
import cairosvg

def run_command(cmd, shell=False):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True, shell=shell)
        return result.stdout.strip()
    except subprocess.CalledProcessError as e:
        print(f"Error: {e.stderr}", file=sys.stderr)
        sys.exit(1)

def main():
    if len(sys.argv) != 4:
        print("Usage: appimage-thumbnailer <input> <output> <size>", file=sys.stderr)
        sys.exit(1)

    in_file = str(Path(sys.argv[1]).resolve())
    out_file = str(Path(sys.argv[2]).resolve())
    size = int(sys.argv[3])

    # Extract .DirIcon from AppImage
    icon_find = run_command(['7z', 'e', '-so', in_file, '.DirIcon'])
    
    if not icon_find:
        print("Failed to extract .DirIcon from AppImage.", file=sys.stderr)
        sys.exit(1)

    while True:
        # Check file type
        file_type = run_command(f"7z e -so '{in_file}' '{icon_find}' | file -", shell=True)
        
        if any(ext.lower() in file_type.lower() for ext in ['png', 'svg']):
            # Extract icon data
            process = subprocess.run(['7z', 'e', '-so', in_file, icon_find], 
                                  capture_output=True, check=True)
            icon_data = process.stdout
            
            if icon_data:
                try:
                    # Convert SVG or load PNG
                    if 'svg' in file_type.lower():
                        png_data = cairosvg.svg2png(bytestring=icon_data)
                        img = Image.open(io.BytesIO(png_data))
                    else:
                        img = Image.open(io.BytesIO(icon_data))

                    # Convert to RGBA
                    if img.mode != 'RGBA':
                        img = img.convert('RGBA')

                    # Calculate new dimensions maintaining aspect ratio
                    ratio = min(size/float(img.size[0]), size/float(img.size[1]))
                    new_size = tuple(int(dim * ratio) for dim in img.size)
                    
                    # Use LANCZOS resampling for high quality
                    resized_img = img.resize(new_size, Image.Resampling.LANCZOS)
                    
                    # Create new image with exact size and paste resized image centered
                    final_img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
                    paste_x = (size - new_size[0]) // 2
                    paste_y = (size - new_size[1]) // 2
                    final_img.paste(resized_img, (paste_x, paste_y))
                    
                    # Save with optimization
                    final_img.save(out_file, "PNG", optimize=True)
                    break
                    
                except Exception as e:
                    print(f"Failed to process image: {str(e)}", file=sys.stderr)
                    sys.exit(1)
        else:
            # Try to read potential target
            potential_target = run_command(['7z', 'e', '-so', in_file, icon_find]).strip()
            
            if potential_target:
                icon_find = potential_target
            else:
                print(f"Failed to read icon file: {icon_find}", file=sys.stderr)
                sys.exit(1)

if __name__ == "__main__":
    main()