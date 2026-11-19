/*
  CYD_VT100.ino  -  VT100 / ANSI display terminal for a 65C02 SBC
  Board : ESP32-2432S028 "Cheap Yellow Display" (ILI9341 320x240)
  Core  : Arduino ESP32 (board "ESP32 Dev Module")
  Lib   : LovyanGFX (Library Manager) - display set up below, no User_Setup.h

  SBC link (CN1 4-pin connector on the CYD):
      CN1 GND     -> SBC GND
      CN1 GPIO27  <- SBC ACIA TX  (via 1k/2k divider: 5V -> 3.3V)
      CN1 GPIO22  -> SBC ACIA RX  (3.3V is fine for a TTL input)
  Power the CYD from SBC +5V into P1 "VIN" (P1 GND to SBC GND).

  Keyboard (choose with KEYBOARD below):
    2 = USB keyboard through a CH9350L module (default)
          CH9350 TX -> CYD P3 GPIO35   (divider if the module's TX is 5V)
          CH9350 GND -> GND, CH9350 5V -> 5V
          Board: normal "esp32" package -> "ESP32 Dev Module".
    1 = Bluetooth keyboard via Bluepad32
          Board: "esp32_bluepad32" package -> "ESP32 Dev Module".
          Hold BOOT while pressing RESET to forget a paired keyboard.
    0 = none (the USB Serial Monitor still works as a keyboard)

  Anything typed into the USB serial monitor is also sent to the SBC.
*/

// ------------------------------------------------------------ test switches
#define KEYBOARD        2        // 0 = none, 1 = Bluetooth (Bluepad32), 2 = CH9350 USB module
#define DEBUG_STAGES    0        // print start-up progress to the USB Serial Monitor
#define KBD_DEBUG       0        // print raw CH9350 frames to the Serial Monitor

#if KEYBOARD == 1
#include <Bluepad32.h>
#endif
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// CYD display: ILI9341 on the ESP32's HSPI (SPI2) pins
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI       _bus;
public:
  LGFX() {
    { auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = false;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc   = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { auto cfg = _panel.config();
      cfg.pin_cs   = 15;
      cfg.pin_rst  = -1;
      cfg.pin_busy = -1;
      cfg.panel_width  = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable   = true;
      cfg.invert     = false;
      cfg.rgb_order  = false;      // set true if red and blue are swapped
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

// ------------------------------------------------------------ settings
#define SBC_BAUD        19200    // match your ACIA at $8000
#define SBC_RX_PIN      27
#define SBC_TX_PIN      22
#define BACKLIGHT_PIN   21
#define ROTATION        1        // 1/3 = landscape 53x30, 0/2 = portrait 40x40
#define INVERT_COLOURS  0        // set 1 if colours look negative (some CYD variants)
#define STRIP_BIT7      1        // mask incoming bytes to 7 bits
#define LF_DOES_CR      0        // 1 if the SBC sends LF only
#define USB_TO_SBC      1        // forward USB serial input to the SBC
#define LOCAL_ECHO      0        // 1 = also show typed keys on the CYD (for testing without the SBC)
#define DEFAULT_FG      2        // 2 = green, 3 = amber-ish (yellow), 7 = white
#define DEFAULT_BG      0

// keyboard settings
#define UK_LAYOUT       1        // 1 = UK keyboard (" on 2, @ on ', # key), 0 = US
#define CAPS_AT_START   1        // start with Caps Lock on (OS-65D wants capitals)
#define KBD_BACKSPACE   0x08     // or 0x7F if your software expects DEL
#define KBD_DELETE      "\x7f"
#define REPEAT_DELAY    500      // ms before auto-repeat starts
#define REPEAT_RATE     60       // ms between repeats
#define SHOW_KBD_STATUS 1        // show "keyboard connected" on screen
#define FORGET_PIN      0        // CYD BOOT button (Bluetooth: forget pairing)
#define CH9350_RX_PIN   35       // P3 connector, input-only pin
#define CH9350_TX_PIN   4        // not wired - the CYD's red LED pin, just a parking place
#define CH9350_BAUD     115200   // CH9350 default

#define CW 6                     // character cell (built-in 5x7 GLCD font)
#define CH 8
#define MAXC (320 / CW)          // 53
#define MAXR (320 / CH)          // 40

LGFX tft;
HardwareSerial SBC(2);

// ------------------------------------------------------------ screen model
struct Cell { char ch; uint8_t attr; };   // attr: b0-2 fg, b3-5 bg, b6 bright, b7 underline
static Cell scr[MAXR][MAXC];
static bool rowDirty[MAXR];
static int  COLS, ROWS;

static int  cx, cy;                // cursor
static int  scrollTop, scrollBot;  // scroll region (inclusive)
static bool wrapPending, autoWrap = true, cursorOn = true;
static int  savX, savY; static uint8_t savFg, savBg, savBright, savUl, savRev;

static uint8_t fg = DEFAULT_FG, bg = DEFAULT_BG, bright, underline, reverse;

static const uint16_t palette[16] = {
  0x0000, 0xA800, 0x0540, 0xAD40, 0x0015, 0xA815, 0x0555, 0xAD55,   // normal
  0x52AA, 0xFAAA, 0x57EA, 0xFFEA, 0x52BF, 0xFABF, 0x57FF, 0xFFFF    // bright
};

static uint8_t curAttr() {
  uint8_t f = fg, b = bg;
  if (reverse) { uint8_t t = f; f = b; b = t; }
  return (f & 7) | ((b & 7) << 3) | (bright ? 0x40 : 0) | (underline ? 0x80 : 0);
}

static void clearCells(int r, int c0, int c1) {          // inclusive range on one row
  uint8_t a = ((bg & 7) << 3) | (fg & 7);
  for (int c = c0; c <= c1; c++) { scr[r][c].ch = ' '; scr[r][c].attr = a; }
  rowDirty[r] = true;
}

static void scrollUp(int top, int bot, int n = 1) {
  while (n--) {
    memmove(&scr[top][0], &scr[top + 1][0], sizeof(scr[0]) * (bot - top));
    clearCells(bot, 0, COLS - 1);
  }
  for (int r = top; r <= bot; r++) rowDirty[r] = true;
}

static void scrollDown(int top, int bot, int n = 1) {
  while (n--) {
    memmove(&scr[top + 1][0], &scr[top][0], sizeof(scr[0]) * (bot - top));
    clearCells(top, 0, COLS - 1);
  }
  for (int r = top; r <= bot; r++) rowDirty[r] = true;
}

static void lineFeed() {
  if (cy == scrollBot) scrollUp(scrollTop, scrollBot);
  else if (cy < ROWS - 1) cy++;
}

static void reverseIndex() {
  if (cy == scrollTop) scrollDown(scrollTop, scrollBot);
  else if (cy > 0) cy--;
}

static void resetTerminal() {
  fg = DEFAULT_FG; bg = DEFAULT_BG; bright = underline = reverse = 0;
  scrollTop = 0; scrollBot = ROWS - 1;
  cx = cy = 0; wrapPending = false; autoWrap = true; cursorOn = true;
  for (int r = 0; r < ROWS; r++) clearCells(r, 0, COLS - 1);
}

static void putPrintable(char c) {
  if (wrapPending) { cx = 0; lineFeed(); wrapPending = false; }
  scr[cy][cx].ch = c; scr[cy][cx].attr = curAttr(); rowDirty[cy] = true;
  if (cx < COLS - 1) cx++;
  else if (autoWrap) wrapPending = true;
}

static void sendStr(const char *s) { SBC.print(s); }

// ------------------------------------------------------------ escape parser
enum { S_NORMAL, S_ESC, S_CSI, S_SKIP1 } state = S_NORMAL;
static int  par[8], npar; static bool priv;

static int P(int i, int def) { return (i < npar && par[i] > 0) ? par[i] : def; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void doSGR() {
  if (npar == 0) npar = 1, par[0] = 0;
  for (int i = 0; i < npar; i++) {
    int p = par[i];
    if (p == 0)       { fg = DEFAULT_FG; bg = DEFAULT_BG; bright = underline = reverse = 0; }
    else if (p == 1)  bright = 1;
    else if (p == 4)  underline = 1;
    else if (p == 7)  reverse = 1;
    else if (p == 22) bright = 0;
    else if (p == 24) underline = 0;
    else if (p == 27) reverse = 0;
    else if (p >= 30 && p <= 37) fg = p - 30;
    else if (p == 39) fg = DEFAULT_FG;
    else if (p >= 40 && p <= 47) bg = p - 40;
    else if (p == 49) bg = DEFAULT_BG;
  }
}

static void doCSI(char f) {
  char buf[24];
  int n;
  switch (f) {
    case 'A': cy = clampi(cy - P(0, 1), 0, ROWS - 1); break;
    case 'B': cy = clampi(cy + P(0, 1), 0, ROWS - 1); break;
    case 'C': cx = clampi(cx + P(0, 1), 0, COLS - 1); break;
    case 'D': cx = clampi(cx - P(0, 1), 0, COLS - 1); break;
    case 'G': cx = clampi(P(0, 1) - 1, 0, COLS - 1); break;
    case 'd': cy = clampi(P(0, 1) - 1, 0, ROWS - 1); break;
    case 'H': case 'f':
      cy = clampi(P(0, 1) - 1, 0, ROWS - 1);
      cx = clampi(P(1, 1) - 1, 0, COLS - 1); break;
    case 'J':
      n = P(0, 0) ;
      if (npar == 0) n = 0;
      if (n == 0) { clearCells(cy, cx, COLS - 1); for (int r = cy + 1; r < ROWS; r++) clearCells(r, 0, COLS - 1); }
      else if (n == 1) { clearCells(cy, 0, cx); for (int r = 0; r < cy; r++) clearCells(r, 0, COLS - 1); }
      else { for (int r = 0; r < ROWS; r++) clearCells(r, 0, COLS - 1); }
      break;
    case 'K':
      n = (npar == 0) ? 0 : par[0];
      if (n == 0) clearCells(cy, cx, COLS - 1);
      else if (n == 1) clearCells(cy, 0, cx);
      else clearCells(cy, 0, COLS - 1);
      break;
    case 'L': if (cy >= scrollTop && cy <= scrollBot) scrollDown(cy, scrollBot, P(0, 1)); break;
    case 'M': if (cy >= scrollTop && cy <= scrollBot) scrollUp(cy, scrollBot, P(0, 1)); break;
    case 'P': n = min(P(0, 1), COLS - cx);                        // delete chars
      memmove(&scr[cy][cx], &scr[cy][cx + n], sizeof(Cell) * (COLS - cx - n));
      clearCells(cy, COLS - n, COLS - 1); break;
    case '@': n = min(P(0, 1), COLS - cx);                        // insert chars
      memmove(&scr[cy][cx + n], &scr[cy][cx], sizeof(Cell) * (COLS - cx - n));
      clearCells(cy, cx, cx + n - 1); break;
    case 'X': n = min(P(0, 1), COLS - cx); clearCells(cy, cx, cx + n - 1); break;
    case 'm': doSGR(); break;
    case 'r':
      scrollTop = clampi(P(0, 1) - 1, 0, ROWS - 1);
      scrollBot = clampi(P(1, ROWS) - 1, 0, ROWS - 1);
      if (scrollTop >= scrollBot) { scrollTop = 0; scrollBot = ROWS - 1; }
      cx = cy = 0; break;
    case 's': savX = cx; savY = cy; break;
    case 'u': cx = savX; cy = savY; break;
    case 'n':
      if (P(0, 0) == 6) { snprintf(buf, sizeof buf, "\x1b[%d;%dR", cy + 1, cx + 1); sendStr(buf); }
      else if (P(0, 0) == 5) sendStr("\x1b[0n");
      break;
    case 'c': sendStr("\x1b[?1;0c"); break;                       // "I am a VT100"
    case 'h': case 'l': {
      bool on = (f == 'h');
      for (int i = 0; i < npar; i++) {
        if (priv && par[i] == 25) cursorOn = on;
        if (priv && par[i] == 7)  autoWrap = on;
      }
    } break;
  }
  wrapPending = false;
}

static void feed(uint8_t b) {
#if STRIP_BIT7
  b &= 0x7F;
#endif
  switch (state) {
    case S_NORMAL:
      if (b >= 0x20 && b < 0x7F) { putPrintable((char)b); return; }
      switch (b) {
        case 0x1B: state = S_ESC; break;
        case '\r': cx = 0; wrapPending = false; break;
        case '\n': case 0x0B: case 0x0C:
          lineFeed(); wrapPending = false;
#if LF_DOES_CR
          cx = 0;
#endif
          break;
        case 0x08: if (cx > 0) cx--; wrapPending = false; break;
        case 0x09: cx = min((cx / 8 + 1) * 8, COLS - 1); break;
        case 0x07: break;                                         // BEL - could flash/beep
      }
      return;

    case S_ESC:
      state = S_NORMAL;
      switch (b) {
        case '[': state = S_CSI; npar = 0; priv = false; memset(par, 0, sizeof par); break;
        case 'D': lineFeed(); break;
        case 'E': cx = 0; lineFeed(); break;
        case 'M': reverseIndex(); break;
        case '7': savX = cx; savY = cy; savFg = fg; savBg = bg; savBright = bright; savUl = underline; savRev = reverse; break;
        case '8': cx = savX; cy = savY; fg = savFg; bg = savBg; bright = savBright; underline = savUl; reverse = savRev; break;
        case 'c': resetTerminal(); break;
        case 'Z': sendStr("\x1b[?1;0c"); break;
        case '(': case ')': case '#': state = S_SKIP1; break;     // charset / DECALN - ignore
      }
      wrapPending = false;
      return;

    case S_SKIP1: state = S_NORMAL; return;

    case S_CSI:
      if (b == '?') { priv = true; return; }
      if (b >= '0' && b <= '9') {
        if (npar == 0) npar = 1;
        par[npar - 1] = par[npar - 1] * 10 + (b - '0');
        return;
      }
      if (b == ';') { if (npar == 0) npar = 1; if (npar < 8) npar++; return; }
      if (b >= 0x40 && b <= 0x7E) { doCSI((char)b); state = S_NORMAL; return; }
      if (b == 0x1B) { state = S_ESC; return; }
      if (b < 0x20) { feed(b); state = S_CSI; return; }           // controls inside CSI
      state = S_NORMAL; return;
  }
}

// ------------------------------------------------------------ rendering
static bool blinkOn = true;
static int  lastCx = -1, lastCy = -1;

static void drawRow(int r) {
  int y = r * CH;
  for (int c = 0; c < COLS; c++) {
    Cell &cell = scr[r][c];
    uint8_t a = cell.attr;
    uint16_t f = palette[(a & 7) | ((a & 0x40) ? 8 : 0)];
    uint16_t b = palette[(a >> 3) & 7];
    if (cursorOn && blinkOn && r == cy && c == cx) { uint16_t t = f; f = b; b = t; }
    int x = c * CW;
    if (f == b) tft.fillRect(x, y, CW, CH, b);
    else { tft.setTextColor(f, b); tft.drawChar((uint16_t)(uint8_t)cell.ch, x, y); }
    if (a & 0x80) tft.drawFastHLine(x, y + CH - 1, CW, f);
  }
}

static void refresh() {
  if (cx != lastCx || cy != lastCy) {
    if (lastCy >= 0) rowDirty[lastCy] = true;
    rowDirty[cy] = true;
    lastCx = cx; lastCy = cy;
    blinkOn = true;
  }
  tft.startWrite();
  for (int r = 0; r < ROWS; r++)
    if (rowDirty[r]) { drawRow(r); rowDirty[r] = false; }
  tft.endWrite();
}

// ------------------------------------------------------------ keyboard (common)
// keyDown[] holds the HID key codes currently held, from whichever keyboard source.
static uint8_t curDown[32], prevDown[32];
static bool capsLock = CAPS_AT_START;
static int repKey = 0; static bool repShift, repCtrl; static uint32_t repAt;
static bool kbdConnected = false;

static inline bool isDown(const uint8_t *map, int k) { return map[k >> 3] & (1 << (k & 7)); }
static inline void setDown(uint8_t *map, int k)      { map[k >> 3] |= (1 << (k & 7)); }

static void localMessage(const char *s) {
#if SHOW_KBD_STATUS
  for (; *s; s++) feed(*s);
#endif
}

// HID usage codes 0x2D..0x38:   -  =  [  ]  \  #  ;  '  `  ,  .  /
static const char symPlain[] = "-=[]\\#;'`,./";
#if UK_LAYOUT
static const char symShift[] = "_+{}|~:@\0<>?";    // shift+` is a not-sign: ignored
static const char digShift[] = "!\"#$%^&*()";       // shift+3 is a pound sign: sends #
#else
static const char symShift[] = "_+{}|~:\"~<>?";
static const char digShift[] = "!@#$%^&*()";
#endif

static void sendKey(int k, bool shift, bool ctrl) {
  char c = 0;
  const char *seq = nullptr;

  if (k >= 0x04 && k <= 0x1D) {                         // A-Z
    c = 'a' + (k - 0x04);
    if (shift != capsLock) c -= 32;
  } else if (k >= 0x1E && k <= 0x27) {                  // 1-9, 0
    c = shift ? digShift[k - 0x1E] : "1234567890"[k - 0x1E];
  } else if (k >= 0x2D && k <= 0x38) {
    c = shift ? symShift[k - 0x2D] : symPlain[k - 0x2D];
  } else if (k == 0x64) {                                // extra \ | key on UK boards
    c = shift ? '|' : '\\';
  } else if (k >= 0x59 && k <= 0x62) {                   // keypad 1-9, 0
    c = "1234567890"[k - 0x59];
  } else {
    switch (k) {
      case 0x28: case 0x58: c = '\r'; break;             // Enter, keypad Enter
      case 0x29: c = 0x1B; break;                        // Esc
      case 0x2A: c = KBD_BACKSPACE; break;
      case 0x2B: c = '\t'; break;
      case 0x2C: c = ' '; break;
      case 0x39: capsLock = !capsLock; return;           // Caps Lock
      case 0x3A: seq = "\x1bOP"; break;                  // F1-F4
      case 0x3B: seq = "\x1bOQ"; break;
      case 0x3C: seq = "\x1bOR"; break;
      case 0x3D: seq = "\x1bOS"; break;
      case 0x49: seq = "\x1b[2~"; break;                 // Insert
      case 0x4A: seq = "\x1b[H"; break;                  // Home
      case 0x4B: seq = "\x1b[5~"; break;                 // Page Up
      case 0x4C: seq = KBD_DELETE; break;                // Delete
      case 0x4D: seq = "\x1b[F"; break;                  // End
      case 0x4E: seq = "\x1b[6~"; break;                 // Page Down
      case 0x4F: seq = "\x1b[C"; break;                  // Right
      case 0x50: seq = "\x1b[D"; break;                  // Left
      case 0x51: seq = "\x1b[B"; break;                  // Down
      case 0x52: seq = "\x1b[A"; break;                  // Up
      case 0x54: c = '/'; break;                         // keypad operators
      case 0x55: c = '*'; break;
      case 0x56: c = '-'; break;
      case 0x57: c = '+'; break;
      case 0x63: c = '.'; break;
    }
  }

  if (seq) {
    SBC.print(seq);
#if LOCAL_ECHO
    for (const char *p = seq; *p; p++) feed(*p);
#endif
    return;
  }
  if (!c) return;
  if (ctrl) {                                            // Ctrl-A..Z, Ctrl-[ \ ] etc.
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= '@' && c <= '_') c &= 0x1F;
    else if (c == ' ') c = 0;
  }
  SBC.write((uint8_t)c);
#if LOCAL_ECHO
  if (c == '\r') { feed('\r'); feed('\n'); }
  else if (c == KBD_BACKSPACE) { feed(8); feed(' '); feed(8); }
  else feed((uint8_t)c);
#endif
}

// Compare curDown with last time: send newly pressed keys, handle auto-repeat.
static void processKeys() {
  bool shift = isDown(curDown, 0xE1) || isDown(curDown, 0xE5);
  bool ctrl  = isDown(curDown, 0xE0) || isDown(curDown, 0xE4);

  for (int k = 0x04; k <= 0x64; k++) {
    if (isDown(curDown, k) && !isDown(prevDown, k)) {
      sendKey(k, shift, ctrl);
      repKey = k; repShift = shift; repCtrl = ctrl;
      repAt = millis() + REPEAT_DELAY;
    }
  }
  memcpy(prevDown, curDown, sizeof prevDown);

  if (repKey) {
    if (!isDown(curDown, repKey) || repKey == 0x39) repKey = 0;
    else if ((int32_t)(millis() - repAt) >= 0) {
      sendKey(repKey, repShift, repCtrl);
      repAt = millis() + REPEAT_RATE;
    }
  }
}

// Load curDown from a standard 8-byte HID keyboard report: mods, 0, key1..key6
static void loadBootReport(const uint8_t *r) {
  memset(curDown, 0, sizeof curDown);
  for (int i = 0; i < 8; i++) if (r[0] & (1 << i)) setDown(curDown, 0xE0 + i);
  for (int i = 2; i < 8; i++) if (r[i] >= 0x04 && r[i] <= 0x65) setDown(curDown, r[i]);
}

#if KEYBOARD == 1
// ------------------------------------------------------------ Bluetooth (Bluepad32)
static ControllerPtr kbd = nullptr;

void onConnectedController(ControllerPtr ctl) {
  Serial.printf("Bluetooth device connected: %s  keyboard=%d\n",
                ctl->getModelName().c_str(), ctl->isKeyboard());
  if (!ctl->isKeyboard()) localMessage("\r\n[BT device connected - not a keyboard]\r\n");
  if (kbd == nullptr && ctl->isKeyboard()) {
    kbd = ctl;
    localMessage("\r\n[Keyboard connected]\r\n");
  }
}

void onDisconnectedController(ControllerPtr ctl) {
  if (ctl == kbd) {
    kbd = nullptr;
    memset(curDown, 0, sizeof curDown);
    localMessage("\r\n[Keyboard disconnected]\r\n");
  }
}

static void pollKeyboard() {
  BP32.update();
  if (!kbd || !kbd->isConnected()) { memset(curDown, 0, sizeof curDown); processKeys(); return; }
  memset(curDown, 0, sizeof curDown);
  for (int k = 0x04; k <= 0x64; k++)
    if (kbd->isKeyPressed(static_cast<KeyboardKey>(k))) setDown(curDown, k);
  for (int k = 0xE0; k <= 0xE7; k++)
    if (kbd->isKeyPressed(static_cast<KeyboardKey>(k))) setDown(curDown, k);
  processKeys();
}

static void keyboardSetup() {
  BP32.setup(&onConnectedController, &onDisconnectedController);
  pinMode(FORGET_PIN, INPUT_PULLUP);
  if (digitalRead(FORGET_PIN) == LOW) {
    BP32.forgetBluetoothKeys();
    localMessage("Paired keyboard forgotten\r\n");
  }
  BP32.enableNewBluetoothConnections(true);
  localMessage("Waiting for Bluetooth keyboard...\r\n");
}

#elif KEYBOARD == 2
// ------------------------------------------------------------ CH9350L USB keyboard module
// Frames from the CH9350 (lower-computer mode):
//   57 AB 88 LEN ID data... SN CS   keyboard/mouse data, state 0 (standalone)
//   57 AB 83 LEN ID data... SN CS   same, state 1
//   57 AB 01 m 0 k1 k2 k3 k4 k5 k6  keyboard, states 2/3/4 (fixed 8-byte report)
//   57 AB 82 xx                     status request, sent about once a second - ignored
HardwareSerial KBD(1);

static uint8_t fr[80];
static int  frPos = 0, frNeed = 0;
static uint8_t frOp = 0;
static uint32_t lastFrame = 0;

static void dumpFrame(const char *tag, const uint8_t *d, int n) {
#if KBD_DEBUG
  Serial.printf("%s:", tag);
  for (int i = 0; i < n; i++) Serial.printf(" %02X", d[i]);
  Serial.println();
#endif
}

static void keyboardData(const uint8_t *d, int n) {
  // A plain keyboard report is 8 bytes; some keyboards put a report-ID byte first.
  if (n >= 8) loadBootReport(d + (n - 8));
  if (!kbdConnected) { kbdConnected = true; localMessage("\r\n[USB keyboard ready]\r\n"); }
}

static void frameDone() {
  lastFrame = millis();
  if (frOp == 0x01) {                                   // states 2/3/4
    dumpFrame("kbd", fr, 8);
    keyboardData(fr, 8);
  } else if (frOp == 0x88 || frOp == 0x83) {            // fr = ID data... SN CS
    int len = frNeed;
    if (len < 3) return;
    uint8_t id = fr[0];
    uint8_t sum = 0;
    for (int i = 1; i < len - 1; i++) sum += fr[i];
    dumpFrame(sum == fr[len - 1] ? "kbd" : "kbd (checksum?)", fr, len);
    if (((id >> 4) & 3) == 1)                           // bits 5-4 = 01: keyboard
      keyboardData(fr + 1, len - 3);
  }
}

static void ch9350Byte(uint8_t b) {
  switch (frPos) {
    case 0: if (b == 0x57) frPos = 1; return;
    case 1: frPos = (b == 0xAB) ? 2 : (b == 0x57 ? 1 : 0); return;
    case 2:
      frOp = b;
      if (b == 0x01)                   { frNeed = 8; frPos = 4; }
      else if (b == 0x88 || b == 0x83) { frPos = 3; }
      else                             { frPos = 0; }    // status / mouse / other: skip
      return;
    case 3:
      if (b == 0 || b > sizeof fr) { frPos = 0; return; }
      frNeed = b; frPos = 4; return;
    default:
      fr[frPos - 4] = b;
      if (++frPos - 4 >= frNeed) { frameDone(); frPos = 0; }
      return;
  }
}

static void pollKeyboard() {
  while (KBD.available()) ch9350Byte(KBD.read());
  processKeys();
}

static void keyboardSetup() {
  KBD.begin(CH9350_BAUD, SERIAL_8N1, CH9350_RX_PIN, CH9350_TX_PIN);
  localMessage("Waiting for USB keyboard (CH9350)...\r\n");
}

#else
static void pollKeyboard() {}
static void keyboardSetup() {}
#endif

#if DEBUG_STAGES
#define STAGE(x) do { Serial.println(x); Serial.flush(); } while (0)
#else
#define STAGE(x)
#endif

// ------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(200);
  STAGE("1: serial started");
  SBC.setRxBufferSize(8192);                 // absorb bursts while redrawing
  SBC.begin(SBC_BAUD, SERIAL_8N1, SBC_RX_PIN, SBC_TX_PIN);

  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);

  STAGE("2: SBC UART started");
  tft.init();
  STAGE("3: display initialised");
  tft.setRotation(ROTATION);
  tft.invertDisplay(INVERT_COLOURS);
  tft.setFont(&fonts::Font0);           // classic 6x8 character font
  tft.setTextSize(1);
  tft.fillScreen((uint16_t)0);

  COLS = min((int)tft.width()  / CW, MAXC);
  ROWS = min((int)tft.height() / CH, MAXR);
  resetTerminal();

  char banner[48];
  snprintf(banner, sizeof banner, "CYD VT100  %dx%d  %d baud\r\n", COLS, ROWS, SBC_BAUD);
  for (char *p = banner; *p; p++) feed(*p);

  refresh();
  STAGE("4: banner drawn");

  keyboardSetup();
  STAGE("5: keyboard set up");
  refresh();
  STAGE("6: setup finished, entering loop");
}

void loop() {
  static uint32_t lastBlink = 0;

  uint32_t t0 = millis();                    // take incoming data for up to 20 ms,
  while (SBC.available() && millis() - t0 < 20)   // then redraw what changed
    feed(SBC.read());

  pollKeyboard();

#if USB_TO_SBC
  while (Serial.available()) SBC.write(Serial.read());
#endif

  if (millis() - lastBlink > 450) {
    lastBlink = millis();
    blinkOn = !blinkOn;
    rowDirty[cy] = true;
  }
  refresh();

  delay(1);   // let the Bluetooth task run - without this the watchdog resets the ESP32
}
