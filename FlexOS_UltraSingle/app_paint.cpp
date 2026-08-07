// #############################################################
// ##  Flex OS UltraSingle  -  app_paint.cpp   (Paint)
// #############################################################
//
//  Lienzo directo: en el P4 Paint guardaba los trazos en PSRAM y
//  los volcaba a LittleFS. Aqui el lienzo ES la memoria del panel
//  (320x480 en la GRAM del ILI9486), asi que se dibuja sin gastar
//  un byte de SRAM. A cambio no hay "guardar": el dibujo vive
//  mientras la app este abierta, que es lo unico honesto con
//  8 KB de RAM y 4 KB de EEPROM.
// #############################################################
#include "fos_apps.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_str.h"
#include "fos_touch.h"

#define PT_BAR_Y   44
#define PT_BAR_H   36
#define PT_CAN_Y   84
#define PT_CAN_H   (APP_Y1 - PT_CAN_Y)
#define PT_SW      26
#define PT_SWSTEP  28

static const uint16_t PALETTE[8] PROGMEM = {
  RGB(30,30,40),   RGB(230,60,60),  RGB(240,150,40), RGB(240,210,50),
  RGB(80,190,90),  RGB(60,190,200), RGB(70,130,235), RGB(245,245,247)
};

static uint8_t  colIdx  = 1;
static uint8_t  brush   = 3;
static int16_t  lastX   = -1, lastY = -1;

static uint16_t curCol(){ return pgm_read_word(&PALETTE[colIdx]); }

static const char PT_CLR[] PROGMEM = "C";

static void drawBar(){
  tft.fillRect(0, PT_BAR_Y, SCR_W, PT_BAR_H, PAGE_BG);
  for(uint8_t i = 0; i < 8; i++){
    int16_t x = 6 + i * PT_SWSTEP;
    tft.fillRoundRect(x, PT_BAR_Y + 4, PT_SW, PT_SW, 6, pgm_read_word(&PALETTE[i]));
    if(i == colIdx) tft.drawRoundRect(x - 2, PT_BAR_Y + 2, PT_SW + 4, PT_SW + 4, 8, C_ACCENT);
  }
  // Dos grosores de trazo y el boton de limpiar.
  for(uint8_t b = 0; b < 2; b++){
    int16_t x = 238 + b * 28;
    tft.fillRoundRect(x, PT_BAR_Y + 4, 24, PT_SW, 6, CARD_ALT);
    tft.fillCircle(x + 12, PT_BAR_Y + 4 + PT_SW / 2, b == 0 ? 3 : 7, TXT_HI);
    if((b == 0 && brush <= 3) || (b == 1 && brush > 3))
      tft.drawRoundRect(x - 2, PT_BAR_Y + 2, 28, PT_SW + 4, 8, C_ACCENT);
  }
  tft.fillRoundRect(294, PT_BAR_Y + 4, 22, PT_SW, 6, C_DANGER);
  gfxTextCP(305, PT_BAR_Y + 4 + PT_SW / 2 - 7, PT_CLR, 2, C_WHITE);
}

static void clearCanvas(){
  tft.fillRect(0, PT_CAN_Y, SCR_W, PT_CAN_H, gDark ? RGB(24,26,34) : C_WHITE);
}

void appPaintEnter(){
  tft.fillRect(0, 0, SCR_W, SCR_H, PAGE_BG);
  uiAppHeader(appNameP(APP_PAINT));
  drawBar();
  clearCanvas();
  uiNavBar(false);
  lastX = lastY = -1;
}

void appPaintTick(){
  // --- Barra de herramientas ---
  if(tp.tap && tp.y >= PT_BAR_Y && tp.y < PT_BAR_Y + PT_BAR_H){
    for(uint8_t i = 0; i < 8; i++){
      int16_t x = 6 + i * PT_SWSTEP;
      if(hitRect(tp.x, tp.y, x, PT_BAR_Y, PT_SW, PT_BAR_H)){ colIdx = i; drawBar(); return; }
    }
    if(hitRect(tp.x, tp.y, 238, PT_BAR_Y, 24, PT_BAR_H)){ brush = 3; drawBar(); return; }
    if(hitRect(tp.x, tp.y, 266, PT_BAR_Y, 24, PT_BAR_H)){ brush = 7; drawBar(); return; }
    if(hitRect(tp.x, tp.y, 294, PT_BAR_Y, 22, PT_BAR_H)){ clearCanvas(); return; }
    return;
  }

  // --- Lienzo ---
  if(!tp.down || tp.y < PT_CAN_Y || tp.y >= APP_Y1){ lastX = lastY = -1; return; }
  uint16_t c = curCol();
  if(lastX >= 0){
    // Traza el segmento entre la muestra anterior y la actual: el tactil
    // resistivo se lee a ~60 Hz y sin esto los trazos rapidos saldrian
    // como una fila de puntos sueltos.
    gfxThickLine(lastX, lastY, tp.x, tp.y, brush * 2, c);
  }
  tft.fillCircle(tp.x, tp.y, brush, c);
  lastX = tp.x;
  lastY = tp.y;
}
