/*
  ============================================================
  ESPINDLE — ESP32-S3 Manga/Comic/EPUB Reader
  (ESP + Kindle)
  ============================================================
  Hardware: ESP32-S3 DevKit + 2.8" ILI9341 SPI TFT w/ SD slot

  SD card layout expected:
    /manga/<Title>/<Chapter Name>/001.jpg   (image pages -- comics/manga)
    /manga/<Title>/<Chapter Name>/001.txt   (text pages -- imported EPUBs)
    ...
  (Use prepare_manga.py on a PC to generate the image version, or use
  the on-device "+ Import EPUB..." menu option to generate the text
  version directly from a .epub file dropped in /epub_import -- no PC
  needed for that path.)

  EPUB IMPORT NOTES:
  - Only import EPUB files you're legally entitled to read this way
    (your own books, purchases, public domain works) -- same principle
    as the manga content.
  - The importer only handles reflowable text content: chapter text is
    extracted and re-flowed to fit the screen. Embedded images, custom
    fonts, and complex CSS layouts in the EPUB are not rendered.
  - OPF/HTML parsing here uses lightweight heuristics (string search),
    not a full XML parser, to keep this practical on a microcontroller.
    It works on the great majority of real-world EPUB files but isn't
    guaranteed to handle every possible malformed or unusual one.

  Libraries required (install via Library Manager):
    - Adafruit GFX Library
    - Adafruit ILI9341
    - TJpg_Decoder (by Bodmer)
    - unzipLIB (by bitbank2)      <-- new, needed for EPUB import
    - SD (bundled with ESP32 core)

  Wiring: see WIRING_GUIDE.md
  ============================================================
*/

#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <TJpg_Decoder.h>
#include <unzipLIB.h>
#include <vector>
#include <algorithm>

// ---------------- PIN CONFIG ----------------
// Shared SPI bus (TFT + SD)
#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_MISO  13

#define TFT_CS    10
#define TFT_DC    9
#define TFT_RST   8
#define TFT_LED   14   // backlight, PWM controlled

#define SD_CS     15

// Buttons (active LOW, wired to GND with internal pullups)
#define BTN_NEXT   4
#define BTN_PREV   5
#define BTN_SELECT 6

// ---------------- SCREEN ----------------
#define SCREEN_W 240
#define SCREEN_H 320

SPIClass spiBus = SPIClass(HSPI);
// Bound explicitly to spiBus (not the default global SPI object) so this
// works correctly regardless of what a given ESP32-S3 board's default
// hardware SPI pins happen to be.
Adafruit_ILI9341 tft = Adafruit_ILI9341(&spiBus, TFT_DC, TFT_CS, TFT_RST);

// ---------------- BACKLIGHT / SLEEP ----------------
#define IDLE_TIMEOUT_MS   (5UL * 60UL * 1000UL)  // 5 minutes
unsigned long lastActivity = 0;
bool screenDimmed = false;

// ---------------- LIBRARY / FOLDER LAYOUT ----------------
#define MANGA_ROOT       "/manga"
#define EPUB_IMPORT_DIR  "/epub_import"
static const char *IMPORT_EPUB_LABEL = "+ Import EPUB...";

// ---------------- TEXT PAGE LAYOUT (for imported EPUB chapters) ----------------
#define TEXT_SIZE        1   // Adafruit default font: 6px/char wide, 8px/line tall at size 1
#define TEXT_MARGIN      6
#define TEXT_CHAR_W      (6 * TEXT_SIZE)
#define TEXT_LINE_H      (8 * TEXT_SIZE)
#define TEXT_CHARS_PER_LINE  ((SCREEN_W - 2*TEXT_MARGIN) / TEXT_CHAR_W)
#define TEXT_LINES_PER_PAGE  ((SCREEN_H - 2*TEXT_MARGIN) / TEXT_LINE_H - 1) // -1 line of headroom

// ---------------- APP STATE ----------------
enum AppState { STATE_SERIES_MENU, STATE_CHAPTER_MENU, STATE_READING, STATE_EPUB_PICKER };
AppState appState = STATE_SERIES_MENU;

std::vector<String> seriesList;
std::vector<String> chapterList;
std::vector<String> pageList;
std::vector<String> epubFileList;

int seriesIndex = 0;
int chapterIndex = 0;
int pageIndex = 0;
int epubIndex = 0;
int menuScrollTop = 0;
const int MENU_VISIBLE_ROWS = 12;

String currentSeriesPath = "";
String currentChapterPath = "";

// ---------------- BUTTON DEBOUNCE ----------------
struct Button {
  uint8_t pin;
  bool rawState = HIGH;      // last raw (possibly bouncy) pin reading
  bool stableState = HIGH;   // debounce-confirmed state
  unsigned long lastChange = 0;
  unsigned long pressStart = 0;
  bool longPressFired = false;
};
Button btnNext   = {BTN_NEXT};
Button btnPrev   = {BTN_PREV};
Button btnSelect = {BTN_SELECT};

const unsigned long DEBOUNCE_MS   = 30;
const unsigned long LONGPRESS_MS  = 600;

// ---------------- EPUB IMPORT (unzipLIB) ----------------
UNZIP epubZip; // statically allocated per library docs (~41KB)
static File epubZipFile; // backing file for the SD callbacks below

// Callback functions unzipLIB needs to read a zip file from the SD card
// (pattern taken directly from the library's own SD card example --
// note the library passes a ZIPFILE* wrapper to close/read/seek, not the
// raw pointer returned by open(); the real File* lives in ->fHandle).
void *epubZipOpen(const char *filename, int32_t *size) {
  epubZipFile = SD.open(filename);
  if (!epubZipFile) { *size = 0; return NULL; }
  *size = epubZipFile.size();
  return (void *)&epubZipFile;
}
void epubZipClose(void *p) {
  ZIPFILE *pzf = (ZIPFILE *)p;
  File *f = (File *)pzf->fHandle;
  if (f) f->close();
}
int32_t epubZipRead(void *p, uint8_t *buffer, int32_t length) {
  ZIPFILE *pzf = (ZIPFILE *)p;
  File *f = (File *)pzf->fHandle;
  return f->read(buffer, length);
}
int32_t epubZipSeek(void *p, int32_t position, int iType) {
  ZIPFILE *pzf = (ZIPFILE *)p;
  File *f = (File *)pzf->fHandle;
  if (iType == SEEK_SET) return f->seek(position) ? position : -1;
  else if (iType == SEEK_END) return f->seek(pzf->iSize + position) ? (pzf->iSize + position) : -1;
  else { long l = f->position(); return f->seek(l + position) ? (l + position) : -1; }
}

// ============================================================
// JPEG DECODER CALLBACK — draws each decoded MCU block to TFT
// ============================================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return 0;
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return 1;
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  // Backlight PWM (ESP32 Arduino core 3.x pin-based LEDC API)
  ledcAttach(TFT_LED, 5000, 8); // pin, freq Hz, resolution bits
  ledcWrite(TFT_LED, 255);      // full brightness

  // Shared SPI bus for TFT + SD
  spiBus.begin(PIN_SCK, PIN_MISO, PIN_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(0); // portrait, best for manga pages
  tft.fillScreen(ILI9341_BLACK);

  drawStatus("Starting...");

  if (!SD.begin(SD_CS, spiBus)) {
    drawFatalError("SD card not found!\nCheck wiring/card.");
    while (true) delay(1000);
  }

  if (!SD.exists(EPUB_IMPORT_DIR)) SD.mkdir(EPUB_IMPORT_DIR);

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tft_output);

  // Display splash screen if it exists on the SD card
  if (SD.exists("/splash.jpg")) {
    TJpgDec.drawSdJpg(0, 0, "/splash.jpg");
    delay(2000);  // Display splash screen for 2 seconds
  }

  loadSeriesList();
  drawMenu();

  lastActivity = millis();
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  bool nextPressed   = readButton(btnNext);
  bool prevPressed   = readButton(btnPrev);
  bool selectPressed = readButtonWithLongPress(btnSelect);

  if (nextPressed || prevPressed || selectPressed) {
    lastActivity = millis();
    if (screenDimmed) {
      wakeScreen();
      return; // swallow the wake press
    }
  }

  handleIdleSleep();

  if (screenDimmed) { delay(20); return; }

  if (nextPressed)   handleNext();
  if (prevPressed)   handlePrev();
  if (selectPressed) handleSelect();

  delay(10);
}

// ============================================================
// BUTTON HELPERS
// ============================================================
bool readButton(Button &b) {
  bool reading = digitalRead(b.pin);
  bool pressed = false;

  if (reading != b.rawState) b.lastChange = millis();
  b.rawState = reading;

  if ((millis() - b.lastChange) > DEBOUNCE_MS) {
    if (reading != b.stableState) {
      b.stableState = reading;
      if (b.stableState == LOW) pressed = true; // confirmed press edge
    }
  }
  return pressed;
}

// SELECT button: short press = confirm, long press = back
// Returns true only on SHORT press release (confirm action)
bool readButtonWithLongPress(Button &b) {
  bool reading = digitalRead(b.pin);
  bool shortPress = false;

  if (reading != b.rawState) b.lastChange = millis();
  b.rawState = reading;

  if ((millis() - b.lastChange) > DEBOUNCE_MS) {
    if (reading != b.stableState) {
      // confirmed transition
      b.stableState = reading;
      if (b.stableState == LOW) {
        b.pressStart = millis();
        b.longPressFired = false;
      } else if (!b.longPressFired) {
        shortPress = true; // confirmed release, wasn't a long press
      }
    }

    // check for long-press while still held down
    if (b.stableState == LOW && !b.longPressFired) {
      if (millis() - b.pressStart > LONGPRESS_MS) {
        b.longPressFired = true;
        goBack();
      }
    }
  }
  return shortPress;
}

// ============================================================
// NAVIGATION LOGIC
// ============================================================
void handleNext() {
  if (appState == STATE_READING) {
    if (pageIndex < (int)pageList.size() - 1) {
      pageIndex++;
      showPage();
    }
  } else if (appState == STATE_EPUB_PICKER) {
    if (epubIndex < (int)epubFileList.size() - 1) { epubIndex++; drawEpubPickerMenu(); }
  } else {
    int count = (appState == STATE_SERIES_MENU) ? seriesList.size() : chapterList.size();
    int &idx = (appState == STATE_SERIES_MENU) ? seriesIndex : chapterIndex;
    if (idx < count - 1) { idx++; drawMenu(); }
  }
}

void handlePrev() {
  if (appState == STATE_READING) {
    if (pageIndex > 0) {
      pageIndex--;
      showPage();
    }
  } else if (appState == STATE_EPUB_PICKER) {
    if (epubIndex > 0) { epubIndex--; drawEpubPickerMenu(); }
  } else {
    int &idx = (appState == STATE_SERIES_MENU) ? seriesIndex : chapterIndex;
    if (idx > 0) { idx--; drawMenu(); }
  }
}

void handleSelect() {
  if (appState == STATE_SERIES_MENU) {
    if (seriesList.empty()) return;
    if (seriesList[seriesIndex] == IMPORT_EPUB_LABEL) {
      enterEpubPicker();
      return;
    }
    currentSeriesPath = String(MANGA_ROOT) + "/" + seriesList[seriesIndex];
    loadChapterList(currentSeriesPath);
    chapterIndex = 0;
    appState = STATE_CHAPTER_MENU;
    drawMenu();
  } else if (appState == STATE_CHAPTER_MENU) {
    if (chapterList.empty()) return;
    currentChapterPath = currentSeriesPath + "/" + chapterList[chapterIndex];
    loadPageList(currentChapterPath);
    if (pageList.empty()) return; // chapter had no valid page files -- stay on chapter menu
    pageIndex = 0;
    appState = STATE_READING;
    showPage();
  } else if (appState == STATE_READING) {
    // toggle a quick page counter overlay
    drawPageOverlay();
  } else if (appState == STATE_EPUB_PICKER) {
    if (epubFileList.empty()) return;
    String path = String(EPUB_IMPORT_DIR) + "/" + epubFileList[epubIndex];
    runEpubImport(path);
  }
}

void goBack() {
  if (appState == STATE_READING) {
    appState = STATE_CHAPTER_MENU;
    drawMenu();
  } else if (appState == STATE_CHAPTER_MENU) {
    appState = STATE_SERIES_MENU;
    drawMenu();
  } else if (appState == STATE_EPUB_PICKER) {
    appState = STATE_SERIES_MENU;
    drawMenu();
  }
  // already at series menu: long-press does nothing further
}

// ============================================================
// SD DIRECTORY LOADING
// ============================================================
void loadSeriesList() {
  seriesList.clear();
  File dir = SD.open(MANGA_ROOT);
  if (!dir) { drawFatalError("No /manga folder\non SD card."); while (true) delay(1000); }
  while (File entry = dir.openNextFile()) {
    if (entry.isDirectory()) seriesList.push_back(String(entry.name()));
    entry.close();
  }
  dir.close();
  sortStrings(seriesList);
  seriesList.insert(seriesList.begin(), String(IMPORT_EPUB_LABEL));
}

void loadChapterList(const String &seriesPath) {
  chapterList.clear();
  File dir = SD.open(seriesPath);
  if (!dir) return;
  while (File entry = dir.openNextFile()) {
    if (entry.isDirectory()) chapterList.push_back(String(entry.name()));
    entry.close();
  }
  dir.close();
  sortStrings(chapterList);
}

void loadPageList(const String &chapterPath) {
  pageList.clear();
  File dir = SD.open(chapterPath);
  if (!dir) return;
  while (File entry = dir.openNextFile()) {
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      String lower = name; lower.toLowerCase();
      if (lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".txt")) {
        pageList.push_back(name);
      }
    }
    entry.close();
  }
  dir.close();
  sortStrings(pageList); // relies on zero-padded filenames, e.g. 001.jpg / 001.txt
}

void sortStrings(std::vector<String> &v) {
  std::sort(v.begin(), v.end(), [](const String &a, const String &b) {
    return a < b;
  });
}

// ============================================================
// DRAWING
// ============================================================
void drawStatus(const char *msg) {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 140);
  tft.println(msg);
}

void drawFatalError(const char *msg) {
  tft.fillScreen(ILI9341_RED);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 120);
  tft.println(msg);
}

void drawMenu() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(2);

  std::vector<String> *list;
  String title;
  int selected;

  if (appState == STATE_SERIES_MENU) {
    list = &seriesList; title = "Library"; selected = seriesIndex;
  } else {
    list = &chapterList; title = seriesList[seriesIndex]; selected = chapterIndex;
  }

  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(6, 4);
  tft.println(title);
  tft.drawFastHLine(0, 26, SCREEN_W, ILI9341_YELLOW);

  if (list->empty()) {
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(6, 40);
    tft.println("(empty)");
    return;
  }

  // keep selection in view
  if (selected < menuScrollTop) menuScrollTop = selected;
  if (selected >= menuScrollTop + MENU_VISIBLE_ROWS) menuScrollTop = selected - MENU_VISIBLE_ROWS + 1;

  int y = 32;
  for (int i = menuScrollTop; i < (int)list->size() && i < menuScrollTop + MENU_VISIBLE_ROWS; i++) {
    bool isSel = (i == selected);
    tft.fillRect(0, y, SCREEN_W, 20, isSel ? ILI9341_BLUE : ILI9341_BLACK);
    tft.setTextColor(isSel ? ILI9341_WHITE : ILI9341_LIGHTGREY);
    tft.setCursor(6, y + 2);
    tft.println((*list)[i]);
    y += 20;
  }
}

void showPage() {
  tft.fillScreen(ILI9341_BLACK);
  String name = pageList[pageIndex];
  String lower = name; lower.toLowerCase();
  String path = currentChapterPath + "/" + name;
  if (lower.endsWith(".txt")) {
    renderTextPage(path);
  } else {
    TJpgDec.drawSdJpg(0, 0, path.c_str());
  }
}

void drawPageOverlay() {
  char buf[24];
  snprintf(buf, sizeof(buf), "%d / %d", pageIndex + 1, (int)pageList.size());
  tft.fillRect(0, SCREEN_H - 24, SCREEN_W, 24, ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(6, SCREEN_H - 20);
  tft.println(buf);
}

// Renders a pre-wrapped text page file -- each line in the file is already
// sized to fit the screen width (done at import time), so this just prints
// each line in order.
void renderTextPage(const String &path) {
  File f = SD.open(path);
  if (!f) {
    tft.setTextColor(ILI9341_RED);
    tft.setTextSize(2);
    tft.setCursor(10, 140);
    tft.println("Error reading page");
    return;
  }
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(TEXT_SIZE);
  int y = TEXT_MARGIN;
  while (f.available() && y < SCREEN_H - TEXT_MARGIN) {
    String line = f.readStringUntil('\n');
    tft.setCursor(TEXT_MARGIN, y);
    tft.println(line);
    y += TEXT_LINE_H;
  }
  f.close();
}

// ============================================================
// SLEEP / BACKLIGHT
// ============================================================
void handleIdleSleep() {
  if (!screenDimmed && (millis() - lastActivity > IDLE_TIMEOUT_MS)) {
    ledcWrite(TFT_LED, 0);
    screenDimmed = true;
  }
}

void wakeScreen() {
  ledcWrite(TFT_LED, 255);
  screenDimmed = false;
  lastActivity = millis();
}

// ============================================================
// EPUB IMPORT — everything below this line is the new feature
// ============================================================

void enterEpubPicker() {
  scanEpubFolder();
  epubIndex = 0;
  appState = STATE_EPUB_PICKER;
  drawEpubPickerMenu();
}

void scanEpubFolder() {
  epubFileList.clear();
  File dir = SD.open(EPUB_IMPORT_DIR);
  if (!dir) return;
  while (File entry = dir.openNextFile()) {
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      String lower = name; lower.toLowerCase();
      if (lower.endsWith(".epub")) epubFileList.push_back(name);
    }
    entry.close();
  }
  dir.close();
  sortStrings(epubFileList);
}

void drawEpubPickerMenu() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(6, 4);
  tft.println("Import EPUB");
  tft.drawFastHLine(0, 26, SCREEN_W, ILI9341_YELLOW);

  if (epubFileList.empty()) {
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(1);
    tft.setCursor(6, 40);
    tft.println("No .epub files found.");
    tft.setCursor(6, 54);
    tft.println("Copy .epub files to:");
    tft.setCursor(6, 66);
    tft.println(EPUB_IMPORT_DIR);
    tft.setCursor(6, 84);
    tft.println("on the SD card, then");
    tft.setCursor(6, 96);
    tft.println("re-open this menu.");
    return;
  }

  if (epubIndex < menuScrollTop) menuScrollTop = epubIndex;
  if (epubIndex >= menuScrollTop + MENU_VISIBLE_ROWS) menuScrollTop = epubIndex - MENU_VISIBLE_ROWS + 1;

  int y = 32;
  for (int i = menuScrollTop; i < (int)epubFileList.size() && i < menuScrollTop + MENU_VISIBLE_ROWS; i++) {
    bool isSel = (i == epubIndex);
    tft.fillRect(0, y, SCREEN_W, 20, isSel ? ILI9341_BLUE : ILI9341_BLACK);
    tft.setTextColor(isSel ? ILI9341_WHITE : ILI9341_LIGHTGREY);
    tft.setCursor(6, y + 2);
    tft.println(epubFileList[i]);
    y += 20;
  }
}

void drawImportProgress(const String &line1, const String &line2) {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(6, 6);
  tft.println("Importing...");
  tft.drawFastHLine(0, 28, SCREEN_W, ILI9341_YELLOW);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1);
  tft.setCursor(6, 40);
  tft.println(line1);
  tft.setCursor(6, 54);
  tft.println(line2);
}

// Strips characters that are unsafe/problematic in FAT32 filenames.
String sanitizeFilename(const String &in) {
  String out;
  out.reserve(in.length());
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
        c == '"'  || c == '<' || c == '>' || c == '|' || (uint8_t)c < 32) {
      continue; // drop it
    }
    out += c;
  }
  out.trim();
  if (out.length() == 0) out = "Untitled";
  if (out.length() > 48) out = out.substring(0, 48);
  return out;
}

// Reads the whole current zip entry into a String, in small chunks
// (works for files larger than available stack/heap chunk size).
String readCurrentZipEntry(UNZIP &zip, uint32_t expectedSize) {
  String result;
  result.reserve(expectedSize + 16);
  uint8_t buf[512];
  int32_t n;
  zip.openCurrentFile();
  while ((n = zip.readCurrentFile(buf, sizeof(buf))) > 0) {
    for (int32_t i = 0; i < n; i++) result += (char)buf[i];
  }
  zip.closeCurrentFile();
  return result;
}

// Finds the path of the .opf file inside the epub by scanning all entries
// (simpler and in practice just as reliable as parsing META-INF/container.xml
// for the vast majority of real EPUB files, which have exactly one .opf).
String findOpfPath(UNZIP &zip) {
  char szName[256];
  unz_file_info fi;
  zip.gotoFirstFile();
  int rc = UNZ_OK;
  while (rc == UNZ_OK) {
    rc = zip.getFileInfo(&fi, szName, sizeof(szName), NULL, 0, NULL, 0);
    if (rc == UNZ_OK) {
      String name = String(szName);
      String lower = name; lower.toLowerCase();
      if (lower.endsWith(".opf")) return name;
    }
    rc = zip.gotoNextFile();
  }
  return "";
}

String dirnameOf(const String &path) {
  int idx = path.lastIndexOf('/');
  if (idx < 0) return "";
  return path.substring(0, idx + 1);
}

// Very lightweight OPF parser: pulls out the book title, the manifest
// (id -> href map), and the spine (reading order, as a list of hrefs).
// This is string-search based, not a real XML parser -- it works on
// well-formed EPUB OPF files, which covers the great majority in the wild.
struct OpfResult {
  String bookTitle;
  std::vector<String> spineHrefs; // already resolved relative to the epub root
};

String extractBetween(const String &s, const String &startTag, const String &endTag, int fromIdx = 0) {
  int start = s.indexOf(startTag, fromIdx);
  if (start < 0) return "";
  start += startTag.length();
  int end = s.indexOf(endTag, start);
  if (end < 0) return "";
  return s.substring(start, end);
}

String extractAttr(const String &tag, const String &attrName) {
  String pattern = attrName + "=\"";
  int start = tag.indexOf(pattern);
  if (start < 0) return "";
  start += pattern.length();
  int end = tag.indexOf("\"", start);
  if (end < 0) return "";
  return tag.substring(start, end);
}

OpfResult parseOpf(const String &opf, const String &opfDir) {
  OpfResult result;

  // Book title: <dc:title>...</dc:title> (or plain <title> as fallback)
  String title = extractBetween(opf, "<dc:title>", "</dc:title>");
  if (title == "") title = extractBetween(opf, "<dc:title ", "</dc:title>");
  if (title == "") title = extractBetween(opf, "<title>", "</title>");
  // strip any leftover tag remnants
  int gt = title.indexOf('>');
  if (title.startsWith("<") && gt >= 0) title = title.substring(gt + 1);
  result.bookTitle = sanitizeFilename(title == "" ? "Imported Book" : title);

  // Manifest: map id -> href
  std::vector<String> manifestIds, manifestHrefs;
  int searchPos = 0;
  while (true) {
    int itemStart = opf.indexOf("<item ", searchPos);
    if (itemStart < 0) break;
    int itemEnd = opf.indexOf("/>", itemStart);
    int itemEnd2 = opf.indexOf(">", itemStart);
    int tagEnd = (itemEnd >= 0 && (itemEnd2 < 0 || itemEnd < itemEnd2)) ? itemEnd : itemEnd2;
    if (tagEnd < 0) break;
    String tag = opf.substring(itemStart, tagEnd);
    String id = extractAttr(tag, "id");
    String href = extractAttr(tag, "href");
    String mediaType = extractAttr(tag, "media-type");
    if (id != "" && href != "" &&
        (mediaType.indexOf("html") >= 0 || mediaType.indexOf("xml") >= 0)) {
      manifestIds.push_back(id);
      manifestHrefs.push_back(href);
    }
    searchPos = tagEnd + 1;
  }

  // Spine: ordered list of <itemref idref="..."/>
  searchPos = 0;
  while (true) {
    int refStart = opf.indexOf("<itemref ", searchPos);
    if (refStart < 0) break;
    int refEnd = opf.indexOf(">", refStart);
    if (refEnd < 0) break;
    String tag = opf.substring(refStart, refEnd);
    String idref = extractAttr(tag, "idref");
    for (size_t i = 0; i < manifestIds.size(); i++) {
      if (manifestIds[i] == idref) {
        result.spineHrefs.push_back(opfDir + manifestHrefs[i]);
        break;
      }
    }
    searchPos = refEnd + 1;
  }

  return result;
}

// Decodes the handful of HTML entities that actually show up in book text.
String decodeEntities(const String &in) {
  String out = in;
  out.replace("&amp;", "&");
  out.replace("&lt;", "<");
  out.replace("&gt;", ">");
  out.replace("&quot;", "\"");
  out.replace("&apos;", "'");
  out.replace("&#39;", "'");
  out.replace("&nbsp;", " ");
  out.replace("&mdash;", "--");
  out.replace("&ndash;", "-");
  out.replace("&hellip;", "...");
  out.replace("&ldquo;", "\"");
  out.replace("&rdquo;", "\"");
  out.replace("&lsquo;", "'");
  out.replace("&rsquo;", "'");
  return out;
}

// Strips HTML/XHTML tags from a chapter file's content, turning block-level
// elements (paragraphs, headings, line breaks) into blank-line separators
// so the text reads naturally once re-wrapped for the screen.
String stripHtmlToText(const String &html) {
  String out;
  out.reserve(html.length());
  bool inTag = false;
  bool inSkip = false; // inside <script> or <style>
  String tagBuf;

  for (unsigned int i = 0; i < html.length(); i++) {
    char c = html[i];
    if (c == '<') {
      inTag = true;
      tagBuf = "";
      continue;
    }
    if (inTag) {
      if (c == '>') {
        inTag = false;
        String lowerTag = tagBuf; lowerTag.toLowerCase();
        if (lowerTag.startsWith("script") || lowerTag.startsWith("style")) inSkip = true;
        else if (lowerTag.startsWith("/script") || lowerTag.startsWith("/style")) inSkip = false;
        else if (lowerTag.startsWith("p") || lowerTag.startsWith("/p") ||
                 lowerTag.startsWith("br") || lowerTag.startsWith("div") ||
                 lowerTag.startsWith("/div") ||
                 lowerTag.startsWith("h1") || lowerTag.startsWith("h2") ||
                 lowerTag.startsWith("h3") || lowerTag.startsWith("/h1") ||
                 lowerTag.startsWith("/h2") || lowerTag.startsWith("/h3")) {
          out += "\n\n";
        }
      } else {
        tagBuf += c;
      }
      continue;
    }
    if (!inSkip) out += c;
  }

  out = decodeEntities(out);

  // collapse runs of whitespace (but keep paragraph breaks)
  String collapsed;
  collapsed.reserve(out.length());
  bool lastWasNewline = false;
  bool lastWasSpace = false;
  for (unsigned int i = 0; i < out.length(); i++) {
    char c = out[i];
    if (c == '\n') {
      if (!lastWasNewline) collapsed += '\n';
      lastWasNewline = true;
      lastWasSpace = false;
    } else if (c == ' ' || c == '\t' || c == '\r') {
      if (!lastWasSpace && !lastWasNewline) collapsed += ' ';
      lastWasSpace = true;
    } else {
      collapsed += c;
      lastWasNewline = false;
      lastWasSpace = false;
    }
  }
  return collapsed;
}

// Extracts a chapter title from a heading tag if present, else returns "".
String extractChapterTitle(const String &html) {
  String t = extractBetween(html, "<h1", "</h1>");
  if (t == "") t = extractBetween(html, "<h2", "</h2>");
  if (t == "") return "";
  int gt = t.indexOf('>');
  if (gt >= 0) t = t.substring(gt + 1);
  t = stripHtmlToText(t);
  t.trim();
  if (t.length() > 40) t = t.substring(0, 40);
  return t;
}

// Greedy word-wraps plainText to TEXT_CHARS_PER_LINE, groups the result into
// pages of TEXT_LINES_PER_PAGE lines each, and writes them as numbered .txt
// files directly into destDir on the SD card.
void paginateAndWrite(const String &plainText, const String &destDir) {
  std::vector<String> lines;
  String paragraph;
  paragraph.reserve(256);

  auto flushParagraph = [&]() {
    if (paragraph.length() == 0) { lines.push_back(""); return; }
    int start = 0;
    while (start < (int)paragraph.length()) {
      int end = start + TEXT_CHARS_PER_LINE;
      if (end >= (int)paragraph.length()) {
        lines.push_back(paragraph.substring(start));
        break;
      }
      // back up to the last space so we don't split a word
      int breakAt = -1;
      for (int i = end; i > start; i--) {
        if (paragraph[i] == ' ') { breakAt = i; break; }
      }
      if (breakAt < 0) breakAt = end; // no space found -- hard break
      lines.push_back(paragraph.substring(start, breakAt));
      start = breakAt + 1;
    }
    paragraph = "";
  };

  for (unsigned int i = 0; i < plainText.length(); i++) {
    char c = plainText[i];
    if (c == '\n') {
      flushParagraph();
    } else {
      paragraph += c;
    }
  }
  flushParagraph();

  // group lines into pages
  int pageNum = 1;
  for (size_t i = 0; i < lines.size(); i += TEXT_LINES_PER_PAGE) {
    String pageContent;
    for (size_t j = i; j < lines.size() && j < i + TEXT_LINES_PER_PAGE; j++) {
      pageContent += lines[j];
      pageContent += "\n";
    }
    char fname[16];
    snprintf(fname, sizeof(fname), "/%03d.txt", pageNum++);
    File out = SD.open(destDir + String(fname), FILE_WRITE);
    if (out) {
      out.print(pageContent);
      out.close();
    }
  }
}

// Main entry point: converts one EPUB file into the /manga/<Title>/... layout.
void runEpubImport(const String &epubPath) {
  drawImportProgress("Opening archive...", epubPath);

  int rc = epubZip.openZIP(epubPath.c_str(), epubZipOpen, epubZipClose, epubZipRead, epubZipSeek);
  if (rc != UNZ_OK) {
    drawImportProgress("ERROR:", "Could not open EPUB file.");
    delay(2500);
    appState = STATE_EPUB_PICKER;
    drawEpubPickerMenu();
    return;
  }

  String opfPath = findOpfPath(epubZip);
  if (opfPath == "") {
    epubZip.closeZIP();
    drawImportProgress("ERROR:", "No .opf found -- not a valid EPUB?");
    delay(3000);
    appState = STATE_EPUB_PICKER;
    drawEpubPickerMenu();
    return;
  }

  if (epubZip.locateFile(opfPath.c_str()) != UNZ_OK) {
    epubZip.closeZIP();
    drawImportProgress("ERROR:", "Could not locate .opf entry.");
    delay(3000);
    appState = STATE_EPUB_PICKER;
    drawEpubPickerMenu();
    return;
  }
  unz_file_info opfInfo;
  char nameBuf[256];
  epubZip.getFileInfo(&opfInfo, nameBuf, sizeof(nameBuf), NULL, 0, NULL, 0);
  String opfContent = readCurrentZipEntry(epubZip, opfInfo.uncompressed_size);

  String opfDir = dirnameOf(opfPath);
  OpfResult opf = parseOpf(opfContent, opfDir);

  if (opf.spineHrefs.empty()) {
    epubZip.closeZIP();
    drawImportProgress("ERROR:", "Could not find any chapters.");
    delay(3000);
    appState = STATE_EPUB_PICKER;
    drawEpubPickerMenu();
    return;
  }

  String bookDir = String(MANGA_ROOT) + "/" + opf.bookTitle;
  SD.mkdir(bookDir);

  int chapterNum = 1;
  for (auto &href : opf.spineHrefs) {
    char progLine[48];
    snprintf(progLine, sizeof(progLine), "Chapter %d / %d", chapterNum, (int)opf.spineHrefs.size());
    drawImportProgress(String(progLine), opf.bookTitle);

    if (epubZip.locateFile(href.c_str()) != UNZ_OK) { chapterNum++; continue; }
    unz_file_info chInfo;
    epubZip.getFileInfo(&chInfo, nameBuf, sizeof(nameBuf), NULL, 0, NULL, 0);
    String rawHtml = readCurrentZipEntry(epubZip, chInfo.uncompressed_size);

    String chapterTitle = extractChapterTitle(rawHtml);
    String plainText = stripHtmlToText(rawHtml);
    plainText.trim();
    if (plainText.length() == 0) { chapterNum++; continue; } // skip cover/nav/empty pages

    char chapterFolderName[64];
    if (chapterTitle != "") {
      snprintf(chapterFolderName, sizeof(chapterFolderName), "%03d - %s", chapterNum, sanitizeFilename(chapterTitle).c_str());
    } else {
      snprintf(chapterFolderName, sizeof(chapterFolderName), "Chapter %03d", chapterNum);
    }
    String chapterDir = bookDir + "/" + String(chapterFolderName);
    SD.mkdir(chapterDir);

    paginateAndWrite(plainText, chapterDir);
    chapterNum++;
  }

  epubZip.closeZIP();

  drawImportProgress("Done!", "Added: " + opf.bookTitle);
  delay(2000);

  loadSeriesList(); // refresh so the new book shows up
  appState = STATE_SERIES_MENU;
  // put selection on the newly added book if we can find it
  for (size_t i = 0; i < seriesList.size(); i++) {
    if (seriesList[i] == opf.bookTitle) { seriesIndex = i; break; }
  }
  drawMenu();
}
