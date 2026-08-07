// #############################################################
// ##  Flex OS UltraSingle  -  app_timer.cpp   (Cronometro)
// #############################################################
//
//  Cronometro con el reloj vectorial grande del sistema. Se repinta
//  a 10 Hz y SOLO la banda de los digitos, no la pantalla entera:
//  a 60 Hz el bus de 8 bits no daria abasto y el contador se
//  quedaria atras.
// #############################################################
#include "fos_apps.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_str.h"
#include "fos_touch.h"

#define TM_Y      130
#define TM_H      110
#define TM_BTN_Y  300
#define TM_BTN_H  52

static bool     running = false;
static uint32_t t0      = 0;      // instante de arranque
static uint32_t acc     = 0;      // milisegundos acumulados en pausas
static uint32_t lastDraw = 0;

static uint32_t elapsed(){ return acc + (running ? (millis() - t0) : 0); }

static void drawDigits(){
  uint32_t ms = elapsed();
  uint8_t  mm = (uint8_t)((ms / 60000UL) % 100);
  uint8_t  ss = (uint8_t)((ms / 1000UL) % 60);
  uint8_t  cs = (uint8_t)((ms / 10UL) % 100);
  char b[10];
  char* p = fmtPad2(b, mm);
  *p++ = ':';
  p = fmtPad2(p, ss);
  tft.fillRect(0, TM_Y, SCR_W, TM_H, PAGE_BG);
  gfxBigClock(b, SCR_W / 2, TM_Y + 46, 36, 8, TXT_HI);
  fmtPad2(b, cs);
  gfxTextC(SCR_W / 2, TM_Y + 84, b, 3, C_ACCENT);
}

static void drawButtons(){
  tft.fillRect(0, TM_BTN_Y, SCR_W, TM_BTN_H, PAGE_BG);
  uiButtonP(24, TM_BTN_Y, 130, TM_BTN_H, strP(running ? S_STOP : S_START), true);
  uiButtonP(166, TM_BTN_Y, 130, TM_BTN_H, strP(S_RESET), false);
}

void appTimerEnter(){
  uiAppScreen(appNameP(APP_TIMER));
  running = false;
  acc = 0;
  drawDigits();
  drawButtons();
  lastDraw = millis();
}

void appTimerTick(){
  if(tp.tap){
    if(hitRect(tp.x, tp.y, 24, TM_BTN_Y, 130, TM_BTN_H)){
      if(running){ acc += millis() - t0; running = false; }
      else       { t0 = millis(); running = true; }
      drawButtons();
      drawDigits();
      return;
    }
    if(hitRect(tp.x, tp.y, 166, TM_BTN_Y, 130, TM_BTN_H)){
      running = false;
      acc = 0;
      drawButtons();
      drawDigits();
      return;
    }
  }
  if(!running) return;
  if(millis() - lastDraw < 100) return;
  lastDraw = millis();
  drawDigits();
}
