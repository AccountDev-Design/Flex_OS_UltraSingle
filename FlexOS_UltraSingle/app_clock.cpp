// #############################################################
// ##  Flex OS UltraSingle  -  app_clock.cpp   (Reloj)
// #############################################################
//
//  Esfera analogica vectorial + hora digital, con el mismo trazo
//  grueso redondeado del reloj de Flex OS Ultra. Las agujas usan
//  una tabla de senos de 60 entradas en PROGMEM (60 B) en vez de
//  sinf/cosf: en AVR eso ahorra la biblioteca matematica entera.
//
//  Solo se repinta la esfera cuando cambia el SEGUNDO, y solo la
//  zona del dial -- el marco, los indices y la fecha se dibujan
//  una vez al entrar.
// #############################################################
#include "fos_apps.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_str.h"
#include "fos_time.h"
#include "fos_touch.h"

#define CK_CX  (SCR_W / 2)
#define CK_CY  180
#define CK_R   96

// 127 * sin(2*pi*i/60)
static const int8_t SIN60[60] PROGMEM = {
     0,   13,   26,   39,   52,   63,   75,   85,   94,  103,
   110,  116,  121,  124,  126,  127,  126,  124,  121,  116,
   110,  103,   94,   85,   75,   63,   52,   39,   26,   13,
     0,  -13,  -26,  -39,  -52,  -63,  -75,  -85,  -94, -103,
  -110, -116, -121, -124, -126, -127, -126, -124, -121, -116,
  -110, -103,  -94,  -85,  -75,  -63,  -52,  -39,  -26,  -13
};

static inline int8_t sin60(uint8_t i){ return (int8_t)pgm_read_byte(&SIN60[i % 60]); }
static inline int8_t cos60(uint8_t i){ return sin60((uint8_t)(i + 15)); }

// Punto sobre la esfera a distancia 'len' del centro, en la marca 'tick'
// (0 = las 12, avanzando en sentido horario).
static void dialPoint(uint8_t tick, int16_t len, int16_t &x, int16_t &y){
  x = CK_CX + (int16_t)(((int32_t)sin60(tick) * len) / 127);
  y = CK_CY - (int16_t)(((int32_t)cos60(tick) * len) / 127);
}

static uint8_t lastSec = 0xFF;

static void drawFace(){
  tft.fillCircle(CK_CX, CK_CY, CK_R + 6, CARD_BG);
  gfxRing(CK_CX, CK_CY, CK_R + 6, 2, BORDER);
  for(uint8_t i = 0; i < 60; i += 5){
    int16_t x0, y0, x1, y1;
    dialPoint(i, CK_R - (i % 15 == 0 ? 16 : 9), x0, y0);
    dialPoint(i, CK_R - 2, x1, y1);
    gfxThickLine(x0, y0, x1, y1, (i % 15 == 0) ? 3 : 1, i % 15 == 0 ? TXT_HI : TXT_MUTE);
  }
}

static void drawHands(){
  // Borra solo el interior del dial (los indices viven fuera de este radio).
  tft.fillCircle(CK_CX, CK_CY, CK_R - 18, CARD_BG);
  uint8_t hTick = (uint8_t)(((gTime.h % 12) * 5 + gTime.m / 12) % 60);
  int16_t x, y;
  dialPoint(hTick, CK_R - 46, x, y);
  gfxThickLine(CK_CX, CK_CY, x, y, 6, TXT_HI);
  dialPoint(gTime.m, CK_R - 28, x, y);
  gfxThickLine(CK_CX, CK_CY, x, y, 4, TXT_HI);
  dialPoint(gTime.s, CK_R - 24, x, y);
  gfxThickLine(CK_CX, CK_CY, x, y, 2, RGB(245,140,30));
  tft.fillCircle(CK_CX, CK_CY, 5, RGB(245,140,30));
}

static void drawDigital(){
  tft.fillRect(0, 300, SCR_W, 60, PAGE_BG);
  char b[40];
  timeStr(b);
  gfxTextC(SCR_W / 2, 304, b, 4, TXT_HI);
  dateLongStr(b);
  gfxTextC(SCR_W / 2, 342, b, 1, TXT_LO);
}

void appClockEnter(){
  uiAppScreen(appNameP(APP_CLOCK));
  drawFace();
  drawHands();
  drawDigital();
  lastSec = gTime.s;
}

void appClockTick(){
  if(gTime.s == lastSec) return;
  lastSec = gTime.s;
  drawHands();
  if(gTime.s == 0) drawDigital();
}
