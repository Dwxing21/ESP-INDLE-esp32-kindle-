# EPUB Import — On-Device, No PC Needed

## What it does
Adds a **"+ Import EPUB..."** entry at the top of the main menu. Selecting
it lets you pick any `.epub` file sitting in a folder on the SD card, and
the ESP32-S3 itself unzips it, extracts the chapter text, re-flows it to
fit the screen, and saves it into your library — all on the device, no
computer involved.

## How to use it
1. Copy `.epub` files onto the SD card into a folder named `/epub_import`
   (the firmware creates this folder automatically on first boot if it
   doesn't exist yet — you just need to drop files into it).
2. On the device, open the main menu and select **+ Import EPUB...**
3. Pick a file from the list and press Select.
4. Wait while it converts — you'll see a progress screen counting through
   chapters (this can take anywhere from several seconds to a couple of
   minutes depending on the book's length; text-only books are much
   faster than image-heavy ones since there's no JPEG decoding involved).
5. When it's done, you're dropped back at the main menu with the new book
   already in your library, ready to read with the exact same
   Next/Prev/Select controls as manga chapters.

## What it can and can't do
- **Can:** extract and paginate the actual reading text of standard
  "reflowable" EPUB books — novels, most non-fiction, etc.
- **Can't:** render embedded images, custom fonts, complex CSS layouts,
  or "fixed layout" EPUBs (the kind used for comics/picture books — use
  the manga pipeline for those instead, via `prepare_manga.py`).
- The EPUB/HTML parsing uses lightweight pattern-matching rather than a
  full XML parser, to keep it practical on a microcontroller. It handles
  the great majority of real-world EPUB files correctly, but an unusual
  or malformed one could occasionally produce a slightly rough result
  (e.g. a stray heading picked up as a chapter title, or a missing
  chapter if its manifest entry is unusual). If a specific book converts
  oddly, let me know the details and I can tune the parser.

## Where things end up on the SD card
```
/epub_import/mybook.epub          <- source file you dropped in
/manga/My Book Title/
  001 - Chapter One/
    001.txt
    002.txt
    ...
  002 - Chapter Two/
    001.txt
    ...
```
This is the same folder structure as manga chapters, just with `.txt`
page files instead of `.jpg` — the reader already treats both the same
way once you're browsing the menu.

## Only import books you're legally entitled to read this way
Your own purchases, your own writing, and public domain works are all
fine. This feature converts files already on your SD card — it doesn't
fetch or download anything from anywhere.
