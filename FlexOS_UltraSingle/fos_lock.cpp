// #############################################################
// ##  Flex OS UltraSingle  -  fos_lock.cpp
// #############################################################
//
//  La pantalla de bloqueo de Flex OS Ultra: reloj vectorial grande
//  sobre panel de vidrio, fecha larga, tarjetas de widget y la
//  pildora inferior con "Desliza para desbloquear".
//
//  Se conserva el contador de fallos con espera progresiva del
//  original (LOCK_FAILS_SOFT/HARD), que es proteccion de verdad y
//  no cuesta memoria; se elimina la sacudida animada, que en el P4
//  recomponia el framebuffer entero por fotograma.
// #############################################################
#include "fos_lock.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_icons.h"
#include "fos_str.h"
#include "fos_time.h"
#include "fos_store.h"
#include "fos_shell.h"
#include "fos_touch.h"
#include "fos_apps.h"
#include <string.h>

// ---- Geometria ----
#define LK_PANEL_X  18
#define LK_PANEL_Y  110
#define LK_PANEL_W  284
#define LK_PANEL_H  150
#define LK_CARD_X   18
#define LK_CARD_W   284
#define LK_CARD_H   48
#define LK_CARD1_Y  276
#define LK_CARD2_Y  332

// Espera progresiva tras fallos consecutivos (igual que en Flex OS Ultra).
#define LOCK_FAILS_SOFT   4
#define LOCK_FAILS_HARD   6
#define LOCK_WAIT_SOFT_MS 30000UL
#define LOCK_WAIT_HARD_MS 300000UL

static uint8_t  failCount = 0;
static uint32_t waitUntil = 0;

// Opciones de bloqueo automatico (indice guardado en EEPROM).
static const uint32_t AUTOLOCK_MS[AUTOLOCK_OPTIONS] PROGMEM = {
  30000UL, 60000UL, 300000UL, 600000UL, 0UL
};
uint32_t autoLockMs(){
  uint8_t i = gAutoLockIdx < AUTOLOCK_OPTIONS ? gAutoLockIdx : 1;
  return pgm_read_dword(&AUTOLOCK_MS[i]);
}

// -------------------------------------------------------------
//  Pantalla de bloqueo
// -------------------------------------------------------------
static const char LK_UP[] PROGMEM = "Encendido";

static void lockCard(int16_t y, PGM_P title, const char* val, uint16_t accent){
  if(gGlass) glassRect(LK_CARD_X, y, LK_CARD_W, LK_CARD_H, 14, GLASS_CARD, 130);
  else       tft.fillRoundRect(LK_CARD_X, y, LK_CARD_W, LK_CARD_H, 14, RGB(40,46,64));
  tft.fillCircle(LK_CARD_X + 26, y + LK_CARD_H / 2, 9, accent);
  gfxTextP(LK_CARD_X + 46, y + 10, title, 1, C_ON_WALL);
  if(val) gfxText(LK_CARD_X + 46, y + 26, val, 1, C_ON_WALL_LO);
}

static void lockClock(){
  if(gGlass) glassRect(LK_PANEL_X, LK_PANEL_Y, LK_PANEL_W, LK_PANEL_H, 24, GLASS_CLOCK, 150);
  else       gfxWallRect(LK_PANEL_X, LK_PANEL_Y, LK_PANEL_W, LK_PANEL_H);
  char b[40];
  timeStr(b);
  gfxBigClock(b, SCR_W / 2, LK_PANEL_Y + 62, 40, 9, C_ON_WALL);
  dateLongStr(b);
  gfxTextC(SCR_W / 2, LK_PANEL_Y + 118, b, 1, C_ON_WALL_LO);
}

void lockEnter(){
  gfxWallpaper();
  drawSignal(SCR_W - 62, 26, 12, C_ON_WALL);
  drawBattery(SCR_W - 40, 14, 26, 13, 82, C_ON_WALL);
  lockClock();

  char b[24];
  uint32_t up = uptimeSec();
  char* p = fmtU16(b, (uint16_t)(up / 3600));
  *p++ = 'h'; *p++ = ' ';
  p = fmtU16(p, (uint16_t)((up / 60) % 60));
  *p++ = 'm'; *p = 0;
  lockCard(LK_CARD1_Y, LK_UP, b, C_OK);

  if(noteEmpty(0)){
    lockCard(LK_CARD2_Y, appNameP(APP_NOTES), 0, C_WARN);
    gfxTextP(LK_CARD_X + 46, LK_CARD2_Y + 26, strP(S_EMPTY), 1, C_ON_WALL_LO);
  } else {
    char n[NOTE_LEN + 1];
    noteLoad(0, n);
    if(strlen(n) > 30) n[30] = 0;
    lockCard(LK_CARD2_Y, appNameP(APP_NOTES), n, C_WARN);
  }

  tft.fillRoundRect(SCR_W / 2 - 46, SCR_H - 92, 92, 7, 3, C_WHITE);
  gfxTextCP(SCR_W / 2, SCR_H - 72, strP(S_SWIPEUNLOCK), 1, C_ON_WALL);
}

void lockTick(){
  if(gMinuteTick) lockClock();
  if(tp.swipeUp || (tp.tap && tp.y > SCR_H - 120)){
    if(gPinSet) shellRequestAuth(AUTH_UNLOCK);
    else        shellGoHome();
  }
}

// -------------------------------------------------------------
//  Verificacion por PIN
// -------------------------------------------------------------
static uint8_t authReason = AUTH_UNLOCK;
static uint8_t entered    = 0;
static uint8_t buf[LOCK_PIN_LEN];
static uint8_t stage      = 0;   // AUTH_SETPIN: 0 = teclear, 1 = repetir
static uint8_t first[LOCK_PIN_LEN];

#define DOT_ROW_Y 155

static void drawDots(bool error){
  gfxWallRect(SCR_W / 2 - 70, DOT_ROW_Y - 12, 140, 24);
  for(uint8_t i = 0; i < LOCK_PIN_LEN; i++){
    int16_t x = SCR_W / 2 - 45 + i * 30;
    if(i < entered) tft.fillCircle(x, DOT_ROW_Y, 8, error ? C_DANGER : C_WHITE);
    else            gfxRing(x, DOT_ROW_Y, 8, 2, error ? C_DANGER : C_WHITE);
  }
}

static void drawTitle(){
  gfxWallRect(0, 96, SCR_W, 26);
  PGM_P t;
  if(authReason == AUTH_SETPIN) t = strP(S_SETPIN);
  else                          t = strP(S_ENTERPIN);
  gfxTextCP(SCR_W / 2, 100, t, 2, C_ON_WALL);
}

void authEnter(uint8_t reason){
  authReason = reason;
  entered = 0;
  stage = 0;
  gfxWallpaper();
  drawTitle();
  drawDots(false);
  uiPinPad(0, true);
  if(!gPinSet && reason == AUTH_UNLOCK) shellGoHome();
}

static void authFail(){
  failCount++;
  drawDots(true);
  gfxWallRect(0, DOT_ROW_Y + 20, SCR_W, 20);
  gfxTextCP(SCR_W / 2, DOT_ROW_Y + 22, strP(S_WRONGPIN), 1, C_DANGER);
  if(failCount >= LOCK_FAILS_HARD)      waitUntil = millis() + LOCK_WAIT_HARD_MS;
  else if(failCount >= LOCK_FAILS_SOFT) waitUntil = millis() + LOCK_WAIT_SOFT_MS;
  entered = 0;
}

static void authAccept(){
  if(authReason == AUTH_SETPIN){
    if(stage == 0){
      memcpy(first, buf, LOCK_PIN_LEN);
      stage = 1;
      entered = 0;
      drawDots(false);
      gfxWallRect(0, DOT_ROW_Y + 20, SCR_W, 20);
      gfxTextCP(SCR_W / 2, DOT_ROW_Y + 22, strP(S_OK), 1, C_ON_WALL_LO);
      return;
    }
    if(memcmp(first, buf, LOCK_PIN_LEN) == 0){
      memcpy(gPin, buf, LOCK_PIN_LEN);
      gPinSet = true;
      storeSavePin();
      shellGoHome();
    } else {
      stage = 0;
      entered = 0;
      drawDots(true);
    }
    return;
  }
  if(memcmp(gPin, buf, LOCK_PIN_LEN) == 0){
    failCount = 0;
    waitUntil = 0;
    shellGoHome();
  } else {
    authFail();
  }
}

void authTick(){
  if(waitUntil && (int32_t)(millis() - waitUntil) < 0){
    if(tp.tap){
      gfxWallRect(0, DOT_ROW_Y + 20, SCR_W, 20);
      char b[16];
      char* p = fmtU16(b, (uint16_t)((waitUntil - millis()) / 1000UL));
      *p++ = 's'; *p = 0;
      gfxTextC(SCR_W / 2, DOT_ROW_Y + 22, b, 1, C_DANGER);
    }
    return;
  }
  if(!tp.tap) return;
  if(uiNavHit(tp.x, tp.y) == 0 || uiNavHit(tp.x, tp.y) == 1){
    if(authReason == AUTH_SETPIN){ shellGoHome(); return; }
  }
  int8_t k = uiPinPadHit(tp.x, tp.y);
  if(k == -1) return;
  if(k == -2){
    if(entered) entered--;
    drawDots(false);
    return;
  }
  if(entered < LOCK_PIN_LEN){
    buf[entered++] = (uint8_t)k;
    drawDots(false);
    if(entered == LOCK_PIN_LEN) authAccept();
  }
}
