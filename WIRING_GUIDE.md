# ESPINDLE — Build Guide
### (ESP32-S3 + Kindle-style e-reader)

## Parts list
- ESP32-S3 DevKit (any variant with enough free GPIO — N16R8 etc. all fine)
- 2.8" ILI9341 SPI TFT module with onboard microSD slot (the common "2.8 SPI TFT + SD" boards)
- 3x momentary push buttons
- microSD card (FAT32, 32GB or less recommended for best compatibility)
- Breadboard + jumper wires (or perfboard once you're happy with it)
- Optional: LiPo battery + TP4056 charge/protection board for portable use

---

## 1. Wiring

Your 2.8" ILI9341 board shares one SPI bus between the screen and the SD card — you only wire SCK/MOSI/MISO once, then give the screen and the SD card their own chip-select (CS) lines.

| Screen/SD pin | ESP32-S3 GPIO | Notes |
|---|---|---|
| VCC | 3.3V | **Not 5V** — most of these boards are 3.3V only |
| GND | GND | |
| CS (TFT_CS) | GPIO 10 | Screen chip-select |
| RESET | GPIO 8 | |
| DC / RS | GPIO 9 | |
| SDI / MOSI | GPIO 11 | Shared with SD |
| SCK | GPIO 12 | Shared with SD |
| SDO / MISO | GPIO 13 | Shared with SD |
| LED (backlight) | GPIO 14 | PWM-driven for dimming/sleep |
| SD_CS | GPIO 15 | SD card chip-select |
| SD_MOSI/SCK/MISO | (same as above) | Same physical wires as the screen |

| Button | ESP32-S3 GPIO | Wiring |
|---|---|---|
| Next page | GPIO 4 | Button between pin and GND (internal pull-up used) |
| Previous page | GPIO 5 | Same |
| Select / Back | GPIO 6 | Short press = select, long press = back |

> **4-leg tactile buttons — check the orientation.** Most small tactile buttons have 4 legs arranged in 2 pairs: two legs on one side are permanently connected to each other, and the two legs on the other side are permanently connected to each other. Pressing the button bridges *those two pairs* together — it does **not** connect all 4 legs. If you accidentally wire both your GPIO and GND wires to legs on the *same* side, the pin will read as permanently pressed (always LOW) whether you press it or not, and the button will appear completely dead. Use a multimeter's continuity mode to confirm: two legs should read "connected" with the button untouched, and only pressing it should connect the other two.

> These GPIOs are just a sensible default free of conflicts with USB/flash pins on most S3 boards. If your specific board already uses GPIO 8/9/10 for something else (check its pinout diagram), just change the `#define` values at the top of `espindle.ino` to match — nothing else needs to change.

Power the whole thing from the ESP32-S3's 5V/USB input; the board's onboard regulator supplies 3.3V to itself, and you tap 3.3V from the ESP32-S3 for the screen/SD.

---

## 2. Arduino IDE setup

1. Install **Arduino IDE 2.x**.
2. Add the ESP32 board package: *File → Preferences → Additional Board URLs* →
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. *Tools → Board → Boards Manager* → search "esp32" → install the Espressif package.
4. Select your board: *Tools → Board → ESP32 Arduino → ESP32S3 Dev Module*.
5. Install libraries via *Sketch → Include Library → Manage Libraries*:
   - **Adafruit GFX Library**
   - **Adafruit ILI9341**
   - **TJpg_Decoder** (by Bodmer)
   - **unzipLIB** (by bitbank2) — needed for the on-device EPUB import feature
   - `SD` is bundled with the ESP32 core — no install needed.
6. Open `espindle.ino`, select the correct COM port, and upload.

---

## 3. Preparing the SD card

1. Format the microSD card as **FAT32**.
2. Create this folder structure on it:
   ```
   /manga/One Piece/Chapter 1001/001.jpg
   /manga/One Piece/Chapter 1001/002.jpg
   /manga/Berserk/Chapter 001/001.jpg
   ```
3. Filenames inside a chapter must be **zero-padded and sequential** (`001.jpg`, `002.jpg`, …) so they sort correctly — the firmware sorts pages alphabetically.
4. Insert the card into the reader's SD slot before powering on.

The section below shows how to generate exactly this structure automatically.

---

## 4. Getting manga/comics onto the SD card as JPEGs

A quick note first: only load content you're legally entitled to read this way — things like your own scanned physical volumes, comics you've bought as digital files (CBZ/CBR/PDF), or officially released free/public-domain comics. Pirating scans isn't something I'll help source.

### Where your source files usually come from
- **Digital purchases** (ComiXology/Kindle, Book Walker, publisher storefronts) — usually download as CBZ, CBR, EPUB, or PDF.
- **Your own scans** of physical volumes — typically end up as a folder of JPG/PNG or a PDF from your scanning software.
- **Public domain / Creative-Commons comics** — usually distributed directly as image sets or PDFs.

### Step A — Get everything into loose image files
- **CBZ** files are literally ZIP archives — rename `.cbz` to `.zip` (or just open with any archive tool) and extract.
- **CBR** files are RAR archives — extract with 7-Zip, WinRAR, or `unar`, then treat like CBZ.
- **PDF** source — extract each page as an image:
  - **Free tool (all platforms):** [XnConvert](https://www.xnview.com/en/xnconvert/) or `pdftoppm` (part of Poppler) — e.g. `pdftoppm -jpeg book.pdf page`
  - **Mac:** Preview → File → Export as JPEG, or use `sips`/`pdftoppm` via Homebrew.
  - **Windows:** Poppler for Windows (`pdftoppm.exe`) or any free "PDF to JPG" desktop tool.

### Step B — Resize, rename, and organize automatically
Doing this by hand for hundreds of pages is painful, so use the included **`prepare_manga.py`** script. It takes a folder of images (or CBZ files) and outputs a properly-sized, correctly-named `/manga/...` folder tree ready to drop onto the SD card.

```bash
pip install pillow

# From a folder of loose images:
python prepare_manga.py --input "path/to/OnePiece_Ch1001" --series "One Piece" --chapter "Chapter 1001" --output "SD_CARD_ROOT/manga"

# Directly from a CBZ file:
python prepare_manga.py --input "path/to/Berserk_001.cbz" --series "Berserk" --chapter "Chapter 001" --output "SD_CARD_ROOT/manga"
```

Point `--output` at the `manga` folder on your actual SD card (once it's mounted on your computer) and the script writes the finished folder structure directly onto it. Run it once per chapter, or see the `--batch` option in the script for converting a whole folder of chapter subfolders at once.

The script:
- Resizes/crops each page to fit the 240×320 screen (preserving aspect ratio, so no stretching)
- Converts everything to JPEG at a size-friendly quality (85) so pages load fast off the SD card
- Renames pages sequentially as `001.jpg`, `002.jpg`, … so they always sort correctly
- Unpacks CBZ files automatically if you point `--input` at a `.cbz` instead of a folder

### Step C — Load the card
Copy (or let the script write directly to) the finished `manga/<Series>/<Chapter>/*.jpg` folders onto the SD card, eject safely, and insert into the reader.

---

## 5. Using the reader
- **Next / Previous** buttons move through the series list, chapter list, or pages depending on where you are.
- **Select** (short press) confirms a menu choice, or shows a page-number overlay while reading.
- **Select** (long press, ~0.6s) goes back a menu level.
- After 5 minutes idle, the backlight turns off to save power; any button press wakes it (the first press just wakes the screen and is otherwise ignored).
