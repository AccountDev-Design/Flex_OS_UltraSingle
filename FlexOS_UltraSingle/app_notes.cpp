// #############################################################
// ##  Flex OS UltraSingle  -  app_notes.cpp   (Notas)
// #############################################################
//
//  Tres notas de 96 caracteres guardadas en la EEPROM. Flex OS
//  Ultra escribia ficheros en LittleFS; aqui el "sistema de
//  archivos" son tres ranuras de tamano fijo, que es lo que cabe
//  sin asignacion dinamica y sin fragmentar 4 KB de EEPROM.
// #############################################################
#include "fos_apps.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_str.h"
#include "fos_store.h"
#include "fos_touch.h"
#include <string.h>

#define NT_ROW_Y  56
#define ED_Y0     50
#define ED_COLS   24
#define ED_LINEH  20

static uint8_t mode = 0;        // 0 = lista, 1 = editor
static uint8_t slot = 0;
static char    buf[NOTE_LEN + 1];
static uint8_t len = 0;

// ---------------- Lista ----------------
static void drawList(){
  uiAppScreen(appNameP(APP_NOTES));
  char prev[NOTE_LEN + 1];
  for(uint8_t i = 0; i < NOTE_SLOTS; i++){
    int16_t y = NT_ROW_Y + i * (ROW_H + 8);
    uiCard(12, y, SCR_W - 24, ROW_H);
    char t[4] = { (char)('1' + i), 0 };
    tft.fillCircle(34, y + ROW_H / 2, 13, C_ACCENT);
    gfxTextC(34, y + ROW_H / 2 - 7, t, 2, C_WHITE);
    if(noteEmpty(i)){
      gfxTextP(60, y + ROW_H / 2 - 4, strP(S_EMPTY), 1, TXT_MUTE);
    } else {
      noteLoad(i, prev);
      if(strlen(prev) > 30) prev[30] = 0;
      gfxText(60, y + ROW_H / 2 - 4, prev, 1, TXT_HI);
    }
    gfxChevron(SCR_W - 32, y + ROW_H / 2, 5, CHEVRON);
  }
  gfxTextCP(SCR_W / 2, NT_ROW_Y + NOTE_SLOTS * (ROW_H + 8) + 20, strP(S_NOTEHINT), 1, TXT_MUTE);
}

// ---------------- Editor ----------------
static void drawText(){
  tft.fillRect(0, ED_Y0, SCR_W, KB_Y - ED_Y0 - 6, PAGE_BG);
  char line[ED_COLS + 1];
  uint8_t row = 0;
  for(uint8_t o = 0; o < len || o == 0; o += ED_COLS){
    uint8_t n = (uint8_t)(len - o);
    if(n > ED_COLS) n = ED_COLS;
    memcpy(line, buf + o, n);
    line[n] = 0;
    gfxText(12, ED_Y0 + 6 + row * ED_LINEH, line, 2, TXT_HI);
    row++;
    if(o + ED_COLS >= len) break;
  }
  // Cursor al final del texto.
  int16_t cx = 12 + (len % ED_COLS) * TXT_ADV(2);
  int16_t cy = ED_Y0 + 6 + (len / ED_COLS) * ED_LINEH;
  tft.fillRect(cx, cy, 2, TXT_H(2) + 2, C_ACCENT);
}

static void drawEditor(){
  tft.fillRect(0, 0, SCR_W, SCR_H, PAGE_BG);
  uiAppHeader(appNameP(APP_NOTES));
  // Contador de caracteres restantes en la cabecera.
  char c[6];
  fmtU16(c, (uint16_t)(NOTE_LEN - len));
  gfxTextR(SCR_W - 14, APP_HDR_H / 2 - 4, c, 1, TXT_MUTE);
  drawText();
  uiKeyboard();
  uiNavBar(false);
}

static void saveAndBack(){
  noteSave(slot, buf);
  mode = 0;
  drawList();
}

void appNotesEnter(){
  mode = 0;
  drawList();
}

void appNotesTick(){
  if(mode == 0){
    if(!tp.tap) return;
    for(uint8_t i = 0; i < NOTE_SLOTS; i++){
      int16_t y = NT_ROW_Y + i * (ROW_H + 8);
      if(!hitRect(tp.x, tp.y, 12, y, SCR_W - 24, ROW_H)) continue;
      slot = i;
      noteLoad(i, buf);
      len = (uint8_t)strlen(buf);
      mode = 1;
      drawEditor();
      return;
    }
    return;
  }

  // --- Editor ---
  if(tp.tap && uiBackHit(tp.x, tp.y)){ saveAndBack(); return; }
  if(!tp.tap) return;
  char c = uiKeyboardHit(tp.x, tp.y);
  if(c == 0) return;
  if(c == 13){ saveAndBack(); return; }
  if(c == 8){
    if(len) buf[--len] = 0;
  } else if(len < NOTE_LEN){
    buf[len++] = c;
    buf[len] = 0;
  }
  drawText();
  char cc[6];
  tft.fillRect(SCR_W - 40, APP_HDR_H / 2 - 6, 26, 10, PAGE_BG);
  fmtU16(cc, (uint16_t)(NOTE_LEN - len));
  gfxTextR(SCR_W - 14, APP_HDR_H / 2 - 4, cc, 1, TXT_MUTE);
}
