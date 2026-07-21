#!/usr/bin/env python3
"""
prepare_manga.py

Converts a folder of images OR a .cbz file into the
/manga/<Series>/<Chapter>/001.jpg ... folder structure
expected by the ESP32-S3 manga reader firmware.

Usage:
    pip install pillow

    # Single chapter from a folder of images:
    python prepare_manga.py --input "OnePiece_Ch1001" \
        --series "One Piece" --chapter "Chapter 1001" \
        --output "SD_CARD_ROOT/manga"

    # Single chapter from a CBZ file:
    python prepare_manga.py --input "Berserk_001.cbz" \
        --series "Berserk" --chapter "Chapter 001" \
        --output "SD_CARD_ROOT/manga"

    # Batch mode: --input points to a folder that contains one
    # subfolder or .cbz file PER CHAPTER, named however you like.
    # Chapter names are taken from those subfolder/file names.
    python prepare_manga.py --batch --input "OnePiece_AllChapters" \
        --series "One Piece" --output "SD_CARD_ROOT/manga"
"""

import argparse
import os
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

from PIL import Image

# Pillow 10+ removed the old top-level resampling constants in favor of
# Image.Resampling.*; this works on both old and new Pillow versions.
try:
    RESAMPLE = Image.Resampling.LANCZOS
except AttributeError:
    RESAMPLE = Image.LANCZOS

SCREEN_W = 240
SCREEN_H = 320
JPEG_QUALITY = 85
VALID_EXT = {".jpg", ".jpeg", ".png", ".bmp", ".webp", ".tif", ".tiff"}


def is_cbz(path: Path) -> bool:
    return path.suffix.lower() in (".cbz", ".zip")


def extract_cbz(cbz_path: Path, dest_dir: Path):
    with zipfile.ZipFile(cbz_path, "r") as z:
        z.extractall(dest_dir)


def find_images(folder: Path):
    files = [p for p in folder.rglob("*") if p.suffix.lower() in VALID_EXT]
    files.sort(key=lambda p: str(p).lower())
    return files


def convert_page(src_path: Path, dest_path: Path):
    with Image.open(src_path) as img:
        img = img.convert("RGB")
        img.thumbnail((SCREEN_W, SCREEN_H), RESAMPLE)
        # pad onto a fixed 240x320 black canvas so all pages fill the screen area
        canvas = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
        offset = ((SCREEN_W - img.width) // 2, (SCREEN_H - img.height) // 2)
        canvas.paste(img, offset)
        canvas.save(dest_path, "JPEG", quality=JPEG_QUALITY, optimize=True)


def process_chapter(input_path: Path, series: str, chapter: str, output_root: Path):
    tmp_dir = None
    source_dir = input_path

    if input_path.is_file() and is_cbz(input_path):
        tmp_dir = Path(tempfile.mkdtemp(prefix="manga_"))
        print(f"  Extracting {input_path.name} ...")
        extract_cbz(input_path, tmp_dir)
        source_dir = tmp_dir
    elif not input_path.is_dir():
        print(f"  Skipping '{input_path}': not a folder or .cbz file")
        return

    images = find_images(source_dir)
    if not images:
        print(f"  No images found in {input_path}")
        if tmp_dir:
            shutil.rmtree(tmp_dir, ignore_errors=True)
        return

    dest_dir = output_root / series / chapter
    dest_dir.mkdir(parents=True, exist_ok=True)

    print(f"  {series} / {chapter}: {len(images)} pages")
    for i, img_path in enumerate(images, start=1):
        dest_path = dest_dir / f"{i:03d}.jpg"
        try:
            convert_page(img_path, dest_path)
        except Exception as e:
            print(f"    Warning: failed on {img_path.name}: {e}")

    if tmp_dir:
        shutil.rmtree(tmp_dir, ignore_errors=True)

    print(f"  Done -> {dest_dir}")


def main():
    ap = argparse.ArgumentParser(description="Prepare manga/comic pages for the ESP32-S3 reader.")
    ap.add_argument("--input", required=True, help="Source folder or .cbz file (or a folder of chapters in --batch mode)")
    ap.add_argument("--series", required=True, help="Series name (used as top-level folder)")
    ap.add_argument("--chapter", help="Chapter name (required unless --batch)")
    ap.add_argument("--output", required=True, help="Path to the 'manga' folder (e.g. the SD card's manga/ root)")
    ap.add_argument("--batch", action="store_true", help="Treat --input as a folder containing one subfolder/.cbz per chapter")
    args = ap.parse_args()

    input_path = Path(args.input)
    output_root = Path(args.output)

    if not input_path.exists():
        print(f"Error: input path '{input_path}' does not exist.")
        sys.exit(1)

    output_root.mkdir(parents=True, exist_ok=True)

    if args.batch:
        if not input_path.is_dir():
            print("Error: --batch requires --input to be a folder of chapter subfolders/.cbz files.")
            sys.exit(1)
        entries = sorted(input_path.iterdir(), key=lambda p: p.name.lower())
        if not entries:
            print("No chapter subfolders/files found.")
            sys.exit(1)
        print(f"Batch processing {len(entries)} chapters for series '{args.series}'...")
        for entry in entries:
            chapter_name = entry.stem if is_cbz(entry) else entry.name
            print(f"Chapter: {chapter_name}")
            process_chapter(entry, args.series, chapter_name, output_root)
    else:
        if not args.chapter:
            print("Error: --chapter is required unless using --batch.")
            sys.exit(1)
        process_chapter(input_path, args.series, args.chapter, output_root)

    print("\nAll done. Copy/verify the 'manga' folder is on your SD card, then insert it into the reader.")


if __name__ == "__main__":
    main()
