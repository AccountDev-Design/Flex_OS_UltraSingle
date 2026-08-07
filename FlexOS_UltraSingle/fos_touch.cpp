// #############################################################
// ##  Flex OS UltraSingle  -  fos_touch.cpp
// #############################################################
//
//  El tactil resistivo del MAR3501 COMPARTE pines con el bus de
//  datos del LCD (XM=A2 y YP=A3 son ademas lineas de control del
//  ILI9486). TouchScreen los reconfigura como entradas analogicas
//  para medir, asi que hay que devolverlos a OUTPUT en cuanto se
//  termina la lectura o el siguiente dibujo sale corrupto. Ese es
//  el motivo de los pinMode() al final de touchPoll().
//
//  La calibracion es la OFICIAL del proyecto (fos_config.h) y no
//  se toca.
// #############################################################
#include "fos_touch.h"
#include <TouchScreen.h>

static TouchScreen ts = TouchScreen(TS_XP, TS_YP, TS_XM, TS_YM, TS_RXPLATE);

FTouch tp;

static bool     rawDown   = false;   // estado crudo filtrado por antirrebote
static uint32_t edgeMs    = 0;       // instante del ultimo cambio de estado
static bool     longFired = false;
static uint32_t lastAct   = 0;

void touchInit(){
  memset(&tp, 0, sizeof(tp));
  lastAct = millis();
}

uint32_t touchLastActivity(){ return lastAct; }

bool hitRect(int16_t px, int16_t py, int16_t x, int16_t y, int16_t w, int16_t h){
  return px >= x && px < x + w && py >= y && py < y + h;
}

void touchConsume(){
  tp.pressed = tp.released = tp.tap = tp.longPress = false;
  tp.swipeUp = tp.swipeDown = tp.swipeLeft = tp.swipeRight = false;
  longFired = true;                 // no dispares una pulsacion larga heredada
}

void touchPoll(){
  tp.pressed = tp.released = tp.tap = tp.longPress = false;
  tp.swipeUp = tp.swipeDown = tp.swipeLeft = tp.swipeRight = false;

  TSPoint p = ts.getPoint();
  // Devolver el bus del LCD a su estado normal (ver cabecera del fichero).
  pinMode(TS_XM, OUTPUT);
  pinMode(TS_YP, OUTPUT);

  bool now = (p.z > TS_PRESS_MIN && p.z < TS_PRESS_MAX);
  uint32_t ms = millis();

  int16_t mx = 0, my = 0;
  if(now){
    // Calibracion oficial, retrato 320x480.
    long cx = map(p.x, TS_LEFT, TS_RT,  0, SCR_W);
    long cy = map(p.y, TS_TOP,  TS_BOT, 0, SCR_H);
    if(cx < 0) cx = 0;
    if(cx > SCR_W - 1) cx = SCR_W - 1;
    if(cy < 0) cy = 0;
    if(cy > SCR_H - 1) cy = SCR_H - 1;
    mx = (int16_t)cx; my = (int16_t)cy;
  }

  // Antirrebote: un cambio de estado solo se acepta si se mantiene.
  if(now != rawDown){
    if(ms - edgeMs >= GEST_DEBOUNCE_MS){ rawDown = now; edgeMs = ms; }
    else if(!now) return;            // rebote a la baja: ignora este frame
  } else {
    edgeMs = ms;
  }

  if(rawDown){
    lastAct = ms;
    if(!tp.down){                    // flanco de bajada
      tp.down = true;
      tp.pressed = true;
      tp.startX = mx; tp.startY = my;
      tp.downMs = ms;
      tp.moved = false;
      longFired = false;
    }
    tp.x = mx; tp.y = my;
    tp.dx = mx - tp.startX;
    tp.dy = my - tp.startY;
    int16_t ax = tp.dx < 0 ? -tp.dx : tp.dx;
    int16_t ay = tp.dy < 0 ? -tp.dy : tp.dy;
    if(ax > GEST_TAP_SLOP || ay > GEST_TAP_SLOP) tp.moved = true;
    if(!longFired && !tp.moved && (ms - tp.downMs) >= GEST_LONG_MS){
      tp.longPress = true;
      longFired = true;
    }
  } else if(tp.down){                // flanco de subida
    tp.down = false;
    tp.released = true;
    lastAct = ms;
    int16_t ax = tp.dx < 0 ? -tp.dx : tp.dx;
    int16_t ay = tp.dy < 0 ? -tp.dy : tp.dy;
    if(!tp.moved && (ms - tp.downMs) <= GEST_TAP_MAX_MS && !longFired){
      tp.tap = true;
    } else if(ax >= GEST_SWIPE_MIN || ay >= GEST_SWIPE_MIN){
      if(ax > ay){ if(tp.dx > 0) tp.swipeRight = true; else tp.swipeLeft = true; }
      else       { if(tp.dy > 0) tp.swipeDown  = true; else tp.swipeUp   = true; }
    }
  }
}
