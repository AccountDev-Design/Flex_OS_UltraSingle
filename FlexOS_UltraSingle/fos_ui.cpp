// #############################################################
// ##  Flex OS UltraSingle  -  fos_ui.cpp
// #############################################################
#include "fos_ui.h"
#include "fos_icons.h"
#include "fos_str.h"
#include "fos_time.h"
#include "fos_store.h"
#include <string.h>

// -------------------------------------------------------------
//  Barra superior
// -------------------------------------------------------------
//  Mismo reparto que Flex OS Ultra: hora grande y fecha corta a la
//  izquierda, indicadores a la derecha.
static void barBg(bool overWall){
  if(overWall) gfxWallRect(0, 0, SCR_W, BAR_TOP_H);
  else         tft.fillRect(0, 0, SCR_W, BAR_TOP_H, PAGE_BG);
}

void uiStatusClock(bool overWall){
  uint16_t fg  = overWall ? C_ON_WALL    : TXT_HI;
  uint16_t fg2 = overWall ? C_ON_WALL_LO : TXT_LO;
  if(overWall) gfxWallRect(0, 0, 150, BAR_TOP_H);
  else         tft.fillRect(0, 0, 150, BAR_TOP_H, PAGE_BG);
  char b[24];
  timeStr(b);
  gfxText(10, 4, b, 2, fg);
  dateShortStr(b);
  gfxText(10, 21, b, 1, fg2);
}

void uiStatusBar(bool overWall){
  barBg(overWall);
  uint16_t fg = overWall ? C_ON_WALL : TXT_HI;
  uiStatusClock(overWall);
  drawSignal(SCR_W - 62, 22, 12, fg);
  drawBattery(SCR_W - 40, 10, 26, 13, 82, fg);
}

// -------------------------------------------------------------
//  Barra de navegacion
// -------------------------------------------------------------
void uiNavBar(bool overWall){
  int16_t y = SCR_H - BAR_NAV_H;
  if(overWall) gfxWallRect(0, y, SCR_W, BAR_NAV_H);
  else         tft.fillRect(0, y, SCR_W, BAR_NAV_H, PAGE_BG);
  uint16_t col = overWall ? NAV_TINT : TXT_LO;
  if(gNavGest) drawHomeIndicator(SCR_H, col);
  else         drawNavBar(y + 10, col);
}

int8_t uiNavHit(int16_t x, int16_t y){
  if(y < SCR_H - BAR_NAV_H) return -1;
  if(gNavGest) return 1;                 // en modo gestos, toda la barra vuelve a Inicio
  if(x < SCR_W / 3)          return 0;
  if(x < SCR_W * 2 / 3)      return 1;
  return 2;
}

// -------------------------------------------------------------
//  Cabecera de aplicacion
// -------------------------------------------------------------
void uiAppHeader(PGM_P title){
  tft.fillRect(0, 0, SCR_W, APP_HDR_H, PAGE_BG);
  // Flecha de vuelta: punta a la IZQUIERDA + asta horizontal.
  const int16_t ay = APP_HDR_H / 2;
  gfxThickLine(24, ay - 9, 15, ay, 2, TXT_HI);
  gfxThickLine(15, ay, 24, ay + 9, 2, TXT_HI);
  tft.fillRect(15, ay - 1, 16, 2, TXT_HI);
  gfxTextCP(SCR_W / 2, APP_HDR_H / 2 - 7, title, 2, TXT_HI);
  tft.drawFastHLine(0, APP_HDR_H - 1, SCR_W, BORDER);
}

void uiAppScreen(PGM_P title){
  tft.fillRect(0, 0, SCR_W, SCR_H, PAGE_BG);
  uiAppHeader(title);
  uiNavBar(false);
}

bool uiBackHit(int16_t x, int16_t y){ return y < APP_HDR_H && x < 60; }

// -------------------------------------------------------------
//  Controles
// -------------------------------------------------------------
void uiCard(int16_t x, int16_t y, int16_t w, int16_t h){
  if(gGlass) tintRect(x, y, w, h, 10, CARD_ALT, 120, PAGE_BG);
  else       tft.fillRoundRect(x, y, w, h, 10, CARD_BG);
}

void uiToggle(int16_t x, int16_t y, bool on){
  const int16_t w = 42, h = 22;
  tft.fillRoundRect(x, y, w, h, h / 2, on ? C_ACCENT : (gDark ? RGB(70,76,92) : RGB(200,206,218)));
  tft.fillCircle(on ? x + w - h / 2 : x + h / 2, y + h / 2, h / 2 - 3, C_WHITE);
}

static void rowBase(int16_t y, PGM_P label){
  uiCard(12, y, SCR_W - 24, ROW_H - 6);
  gfxTextP(26, y + (ROW_H - 6) / 2 - 7, label, 2, TXT_HI);
}

void uiRow(int16_t y, PGM_P label, const char* value, bool chevron){
  rowBase(y, label);
  int16_t rx = SCR_W - 24 - (chevron ? 20 : 6);
  if(value) gfxTextR(rx, y + (ROW_H - 6) / 2 - 4, value, 1, TXT_LO);
  if(chevron) gfxChevron(SCR_W - 32, y + (ROW_H - 6) / 2, 5, CHEVRON);
}

void uiRowP(int16_t y, PGM_P label, PGM_P value, bool chevron){
  rowBase(y, label);
  int16_t rx = SCR_W - 24 - (chevron ? 20 : 6);
  if(value) gfxTextP(rx - gfxTextWP(value, 1), y + (ROW_H - 6) / 2 - 4, value, 1, TXT_LO);
  if(chevron) gfxChevron(SCR_W - 32, y + (ROW_H - 6) / 2, 5, CHEVRON);
}

void uiRowToggle(int16_t y, PGM_P label, bool on){
  rowBase(y, label);
  uiToggle(SCR_W - 24 - 42 - 8, y + (ROW_H - 6) / 2 - 11, on);
}

void uiButtonP(int16_t x, int16_t y, int16_t w, int16_t h, PGM_P label, bool primary){
  tft.fillRoundRect(x, y, w, h, h / 3, primary ? C_ACCENT : CARD_ALT);
  gfxTextCP(x + w / 2, y + h / 2 - 7, label, 2, primary ? C_WHITE : TXT_HI);
}

// -------------------------------------------------------------
//  Aviso breve
// -------------------------------------------------------------
static PGM_P   toastMsg = 0;
static uint32_t toastT0 = 0;
#define TOAST_MS 1400UL
#define TOAST_H  32
#define TOAST_Y  (SCR_H - BAR_NAV_H - TOAST_H - 10)

void uiToast(PGM_P msg){
  toastMsg = msg;
  toastT0  = millis();
  int16_t w = gfxTextWP(msg, 1) + 32;
  if(w > SCR_W - 40) w = SCR_W - 40;
  tft.fillRoundRect((SCR_W - w) / 2, TOAST_Y, w, TOAST_H, TOAST_H / 2, C_ACCENT);
  gfxTextCP(SCR_W / 2, TOAST_Y + TOAST_H / 2 - 4, msg, 1, C_WHITE);
}

void uiToastTick(){
  if(!toastMsg) return;
  if(millis() - toastT0 < TOAST_MS) return;
  tft.fillRect(0, TOAST_Y, SCR_W, TOAST_H, PAGE_BG);
  toastMsg = 0;
}

// -------------------------------------------------------------
//  Teclado numerico (PIN)
// -------------------------------------------------------------
//  Rejilla 3x4: 1..9, borrar, 0. Se usa en la pantalla de bloqueo y al
//  cambiar el PIN, con el mismo aspecto de circulos translucidos de
//  Flex OS Ultra.
static const uint8_t PAD_MAP[12] PROGMEM = { 1,2,3, 4,5,6, 7,8,9, 200,0,201 };

void uiPinPad(uint16_t bg, bool overWall){
  for(uint8_t i = 0; i < 12; i++){
    uint8_t k = pgm_read_byte(&PAD_MAP[i]);
    if(k == 201) continue;                        // hueco
    int16_t x = PAD_X + (i % 3) * PAD_HSTEP;
    int16_t y = PAD_Y + (i / 3) * PAD_VSTEP;
    if(k == 200){                                  // borrar: sin circulo, solo el glifo
      uint16_t fg = overWall ? C_ON_WALL : TXT_HI;
      tft.fillTriangle(x + 10, y + PAD_BTN/2, x + 24, y + PAD_BTN/2 - 12,
                       x + 24, y + PAD_BTN/2 + 12, fg);
      tft.fillRect(x + 24, y + PAD_BTN/2 - 12, 22, 24, fg);
      gfxThickLine(x + 30, y + PAD_BTN/2 - 6, x + 42, y + PAD_BTN/2 + 6, 2, overWall ? C_ACCENT_DK : PAGE_BG);
      gfxThickLine(x + 42, y + PAD_BTN/2 - 6, x + 30, y + PAD_BTN/2 + 6, 2, overWall ? C_ACCENT_DK : PAGE_BG);
      continue;
    }
    if(overWall) glassRect(x, y, PAD_BTN, PAD_BTN, PAD_BTN / 2, GLASS_CARD, 130);
    else         tft.fillCircle(x + PAD_BTN/2, y + PAD_BTN/2, PAD_BTN/2, CARD_ALT);
    char d[2] = { (char)('0' + k), 0 };
    gfxTextC(x + PAD_BTN/2, y + PAD_BTN/2 - 10, d, 3, overWall ? C_ON_WALL : TXT_HI);
  }
  (void)bg;
}

int8_t uiPinPadHit(int16_t x, int16_t y){
  for(uint8_t i = 0; i < 12; i++){
    uint8_t k = pgm_read_byte(&PAD_MAP[i]);
    if(k == 201) continue;
    int16_t bx = PAD_X + (i % 3) * PAD_HSTEP;
    int16_t by = PAD_Y + (i / 3) * PAD_VSTEP;
    if(hitRect(x, y, bx, by, PAD_BTN, PAD_BTN)) return (k == 200) ? -2 : (int8_t)k;
  }
  return -1;
}

// -------------------------------------------------------------
//  Teclado de texto
// -------------------------------------------------------------
//  Cuatro filas, aspecto de teclas redondeadas como el teclado de
//  Flex OS Ultra pero sin sus fases B-G (multitoque, portapapeles de
//  12 ranuras, autocompletado): en 8 KB de SRAM eso no cabe y ademas
//  no aporta nada a las dos apps que escriben texto.
static const char KB_R0[] PROGMEM = "qwertyuiop";
static const char KB_R1[] PROGMEM = "asdfghjkl";
static const char KB_R2[] PROGMEM = "zxcvbnm";
static const char KB_N0[] PROGMEM = "1234567890";
static const char KB_N1[] PROGMEM = "-/:;()$&@\"";
static const char KB_N2[] PROGMEM = ".,?!'+*=%";

static bool kbShift = false;
static bool kbNum   = false;

#define KEY_H   32
#define KEY_GAP 3
#define KEY_W   ((SCR_W - 11 * KEY_GAP) / 10)

void uiKeyboardShiftToggle(){ kbShift = !kbShift; }

static PGM_P kbRow(uint8_t r){
  if(kbNum) return r == 0 ? KB_N0 : (r == 1 ? KB_N1 : KB_N2);
  return r == 0 ? KB_R0 : (r == 1 ? KB_R1 : KB_R2);
}

static void kbKey(int16_t x, int16_t y, int16_t w, char c, PGM_P lbl, bool accent){
  tft.fillRoundRect(x, y, w, KEY_H, 6, accent ? C_ACCENT : CARD_ALT);
  if(lbl) gfxTextCP(x + w / 2, y + KEY_H / 2 - 4, lbl, 1, accent ? C_WHITE : TXT_HI);
  else {
    char s[2] = { c, 0 };
    gfxTextC(x + w / 2, y + KEY_H / 2 - 7, s, 2, TXT_HI);
  }
}

static const char L_SH[]  PROGMEM = "^";
static const char L_DEL[] PROGMEM = "<-";
static const char L_NUM[] PROGMEM = "123";
static const char L_ABC[] PROGMEM = "ABC";
static const char L_OK[]  PROGMEM = "OK";

void uiKeyboard(){
  tft.fillRect(0, KB_Y, SCR_W, KB_H, gDark ? RGB(26,29,38) : RGB(226,230,238));
  for(uint8_t r = 0; r < 3; r++){
    PGM_P row = kbRow(r);
    uint8_t n = (uint8_t)strlen_P(row);
    int16_t total = n * KEY_W + (n - 1) * KEY_GAP;
    int16_t x0 = (SCR_W - total) / 2;
    int16_t y  = KB_Y + 4 + r * (KEY_H + KEY_GAP);
    for(uint8_t i = 0; i < n; i++){
      char c = (char)pgm_read_byte(row + i);
      if(kbShift && !kbNum && c >= 'a' && c <= 'z') c = (char)(c - 32);
      kbKey(x0 + i * (KEY_W + KEY_GAP), y, KEY_W, c, 0, false);
    }
  }
  // Cuarta fila: mayusculas, numeros, espacio, borrar, aceptar.
  int16_t y = KB_Y + 4 + 3 * (KEY_H + KEY_GAP);
  int16_t w2 = KEY_W * 2 + KEY_GAP;
  kbKey(KEY_GAP,                       y, w2, 0, kbShift ? L_SH : L_SH, kbShift);
  kbKey(KEY_GAP * 2 + w2,              y, w2, 0, kbNum ? L_ABC : L_NUM, false);
  kbKey(KEY_GAP * 3 + w2 * 2,          y, SCR_W - (KEY_GAP * 5 + w2 * 4), 0, 0, false);
  kbKey(SCR_W - KEY_GAP * 2 - w2 * 2,  y, w2, 0, L_DEL, false);
  kbKey(SCR_W - KEY_GAP - w2,          y, w2, 0, L_OK, true);
}

char uiKeyboardHit(int16_t x, int16_t y){
  if(y < KB_Y) return 0;
  for(uint8_t r = 0; r < 3; r++){
    PGM_P row = kbRow(r);
    uint8_t n = (uint8_t)strlen_P(row);
    int16_t total = n * KEY_W + (n - 1) * KEY_GAP;
    int16_t x0 = (SCR_W - total) / 2;
    int16_t ky = KB_Y + 4 + r * (KEY_H + KEY_GAP);
    if(y < ky || y >= ky + KEY_H) continue;
    for(uint8_t i = 0; i < n; i++){
      int16_t kx = x0 + i * (KEY_W + KEY_GAP);
      if(x >= kx && x < kx + KEY_W){
        char c = (char)pgm_read_byte(row + i);
        if(kbShift && !kbNum && c >= 'a' && c <= 'z') c = (char)(c - 32);
        return c;
      }
    }
    return 0;
  }
  int16_t ky = KB_Y + 4 + 3 * (KEY_H + KEY_GAP);
  if(y < ky || y >= ky + KEY_H) return 0;
  int16_t w2 = KEY_W * 2 + KEY_GAP;
  if(x < KEY_GAP + w2)                    { kbShift = !kbShift; uiKeyboard(); return 0; }
  if(x < KEY_GAP * 2 + w2 * 2)            { kbNum = !kbNum; kbShift = false; uiKeyboard(); return 0; }
  if(x >= SCR_W - KEY_GAP - w2)           return 13;
  if(x >= SCR_W - KEY_GAP * 2 - w2 * 2)   return 8;
  return ' ';
}
