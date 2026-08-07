// #############################################################
// ##  Flex OS UltraSingle  -  app_settings.cpp   (Ajustes)
// #############################################################
//
//  La app Ajustes de Flex OS Ultra tenia doce categorias, muchas de
//  ellas dedicadas a hardware que aqui no existe (Wi-Fi, Bluetooth,
//  OTA, Modo PC, camara). UltraSingle conserva la estructura -- un
//  indice de categorias y paginas con filas de tarjeta -- con las
//  cinco que si tienen sentido en un Mega.
// #############################################################
#include "fos_apps.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_str.h"
#include "fos_store.h"
#include "fos_time.h"
#include "fos_touch.h"
#include "fos_shell.h"
#include "fos_home.h"
#include <string.h>

enum { PG_INDEX = 0, PG_DISPLAY, PG_LOCK, PG_LANG, PG_TIME, PG_ABOUT };

#define ST_Y0   56
#define ST_STEP (ROW_H + 6)

static uint8_t page = PG_INDEX;

static const char AL_0[] PROGMEM = "30 s";
static const char AL_1[] PROGMEM = "1 min";
static const char AL_2[] PROGMEM = "5 min";
static const char AL_3[] PROGMEM = "10 min";
static const char* const AL_TBL[AUTOLOCK_OPTIONS - 1] PROGMEM = { AL_0, AL_1, AL_2, AL_3 };

static PGM_P autoLockName(){
  if(gAutoLockIdx >= AUTOLOCK_OPTIONS - 1) return strP(S_NEVER);
  return (PGM_P)pgm_read_word(&AL_TBL[gAutoLockIdx]);
}

static const char SET_GEST[]  PROGMEM = "Barra de gestos";
static const char SET_RESET[] PROGMEM = "Restablecer";
static const char SET_PINON[] PROGMEM = "****";

// -------------------------------------------------------------
//  Fila con controles - / +
// -------------------------------------------------------------
static void adjRow(int16_t y, PGM_P label, const char* value){
  uiCard(12, y, SCR_W - 24, ROW_H - 6);
  gfxTextP(26, y + (ROW_H - 6) / 2 - 7, label, 2, TXT_HI);
  int16_t cy = y + (ROW_H - 6) / 2;
  tft.fillCircle(SCR_W - 88, cy, 14, CARD_ALT);
  tft.fillRect(SCR_W - 95, cy - 1, 14, 3, TXT_HI);
  tft.fillCircle(SCR_W - 30, cy, 14, CARD_ALT);
  tft.fillRect(SCR_W - 37, cy - 1, 14, 3, TXT_HI);
  tft.fillRect(SCR_W - 32, cy - 7, 3, 14, TXT_HI);
  gfxTextC(SCR_W - 59, cy - 4, value, 1, TXT_LO);
}

static int8_t adjHit(int16_t y){
  int16_t cy = y + (ROW_H - 6) / 2;
  if(tp.y < cy - 16 || tp.y > cy + 16) return 0;
  if(tp.x > SCR_W - 106 && tp.x < SCR_W - 72) return -1;
  if(tp.x > SCR_W - 48  && tp.x < SCR_W - 12) return 1;
  return 0;
}

// -------------------------------------------------------------
//  Paginas
// -------------------------------------------------------------
static void drawIndex(){
  uiAppScreen(strP(S_SETTINGS));
  static const uint8_t IDS[5] = { S_DISPLAY, S_LOCKSCREEN, S_LANGUAGE, S_DATETIME, S_ABOUT };
  for(uint8_t i = 0; i < 5; i++)
    uiRow(ST_Y0 + i * ST_STEP, strP(IDS[i]), 0, true);
}

static void drawDisplay(){
  uiAppScreen(strP(S_DISPLAY));
  uiRowToggle(ST_Y0,               strP(S_DARKMODE), gDark);
  uiRowToggle(ST_Y0 + ST_STEP,     strP(S_GLASS),    gGlass);
  uiRowP(ST_Y0 + 2 * ST_STEP,      strP(S_ICONSTYLE), strP(gIconGlass ? S_GLASSY : S_FLAT), true);
  uiRowToggle(ST_Y0 + 3 * ST_STEP, SET_GEST,         gNavGest);
}

static void drawLock(){
  uiAppScreen(strP(S_LOCKSCREEN));
  uiRowP(ST_Y0,           strP(S_PIN),      gPinSet ? SET_PINON : strP(S_NOPIN), true);
  uiRowP(ST_Y0 + ST_STEP, strP(S_AUTOLOCK), autoLockName(), true);
}

static void drawLang(){
  uiAppScreen(strP(S_LANGUAGE));
  static const char L_ES[] PROGMEM = "Espa\xA4ol";
  static const char L_EN[] PROGMEM = "English";
  for(uint8_t i = 0; i < 2; i++){
    int16_t y = ST_Y0 + i * ST_STEP;
    uiCard(12, y, SCR_W - 24, ROW_H - 6);
    gfxTextP(26, y + (ROW_H - 6) / 2 - 7, i == 0 ? L_ES : L_EN, 2, TXT_HI);
    if(gLang == i) tft.fillCircle(SCR_W - 34, y + (ROW_H - 6) / 2, 8, C_ACCENT);
    else           gfxRing(SCR_W - 34, y + (ROW_H - 6) / 2, 8, 2, CHEVRON);
  }
}

static void drawTime(){
  uiAppScreen(strP(S_DATETIME));
  char b[8];
  uiRowToggle(ST_Y0, strP(S_H24), gH24);
  fmtPad2(b, gTime.h);   adjRow(ST_Y0 + ST_STEP,     strP(S_HOUR),   b);
  fmtPad2(b, gTime.m);   adjRow(ST_Y0 + 2 * ST_STEP, strP(S_MINUTE), b);
  fmtU16(b, gTime.day);  adjRow(ST_Y0 + 3 * ST_STEP, strP(S_DAY),    b);
  fmtU16(b, gTime.mon + 1); adjRow(ST_Y0 + 4 * ST_STEP, strP(S_MONTH), b);
  fmtU16(b, gTime.year); adjRow(ST_Y0 + 5 * ST_STEP, strP(S_YEAR),   b);
}

static void drawAbout(){
  uiAppScreen(strP(S_ABOUT));
  static const char A_OS[]  PROGMEM = FOS_NAME;
  static const char A_VER[] PROGMEM = FOS_VERSION;
  static const char A_BRD[] PROGMEM = FOS_BOARD;
  gfxTextCP(SCR_W / 2, 70, A_OS, 2, TXT_HI);
  gfxTextCP(SCR_W / 2, 96, A_VER, 1, C_ACCENT);
  gfxTextCP(SCR_W / 2, 120, A_BRD, 1, TXT_LO);
  uiButtonP(40, 200, SCR_W - 80, 50, SET_RESET, false);
}

static void redraw(){
  switch(page){
    case PG_DISPLAY: drawDisplay(); break;
    case PG_LOCK:    drawLock();    break;
    case PG_LANG:    drawLang();    break;
    case PG_TIME:    drawTime();    break;
    case PG_ABOUT:   drawAbout();   break;
    default:         drawIndex();   break;
  }
}

void appSettingsEnter(){
  page = PG_INDEX;
  redraw();
}

// -------------------------------------------------------------
//  Interaccion
// -------------------------------------------------------------
static void tickIndex(){
  static const uint8_t PGS[5] = { PG_DISPLAY, PG_LOCK, PG_LANG, PG_TIME, PG_ABOUT };
  for(uint8_t i = 0; i < 5; i++){
    if(hitRect(tp.x, tp.y, 12, ST_Y0 + i * ST_STEP, SCR_W - 24, ROW_H - 6)){
      page = PGS[i];
      redraw();
      return;
    }
  }
}

static void tickDisplay(){
  if(hitRect(tp.x, tp.y, 12, ST_Y0, SCR_W - 24, ROW_H - 6)){
    gDark = !gDark; storeSaveFlags(); homeInvalidate(); redraw(); return;
  }
  if(hitRect(tp.x, tp.y, 12, ST_Y0 + ST_STEP, SCR_W - 24, ROW_H - 6)){
    gGlass = !gGlass; storeSaveFlags(); homeInvalidate(); redraw(); return;
  }
  if(hitRect(tp.x, tp.y, 12, ST_Y0 + 2 * ST_STEP, SCR_W - 24, ROW_H - 6)){
    gIconGlass = !gIconGlass; storeSaveFlags(); homeInvalidate(); redraw(); return;
  }
  if(hitRect(tp.x, tp.y, 12, ST_Y0 + 3 * ST_STEP, SCR_W - 24, ROW_H - 6)){
    gNavGest = !gNavGest; storeSaveFlags(); homeInvalidate(); redraw(); return;
  }
}

static void tickLock(){
  if(hitRect(tp.x, tp.y, 12, ST_Y0, SCR_W - 24, ROW_H - 6)){
    if(gPinSet){ gPinSet = false; storeSavePin(); redraw(); }
    else       { shellRequestAuth(AUTH_SETPIN); }
    return;
  }
  if(hitRect(tp.x, tp.y, 12, ST_Y0 + ST_STEP, SCR_W - 24, ROW_H - 6)){
    gAutoLockIdx = (uint8_t)((gAutoLockIdx + 1) % AUTOLOCK_OPTIONS);
    storeSaveAutoLock();
    redraw();
  }
}

static void tickLang(){
  for(uint8_t i = 0; i < 2; i++){
    if(hitRect(tp.x, tp.y, 12, ST_Y0 + i * ST_STEP, SCR_W - 24, ROW_H - 6)){
      gLang = i;
      storeSaveLang();
      homeInvalidate();
      redraw();
      return;
    }
  }
}

static void tickTime(){
  if(hitRect(tp.x, tp.y, 12, ST_Y0, SCR_W - 24, ROW_H - 6)){
    gH24 = !gH24; storeSaveFlags(); homeInvalidate(); redraw(); return;
  }
  int8_t d;
  uint8_t h = gTime.h, m = gTime.m, day = gTime.day, mon = gTime.mon;
  uint16_t yr = gTime.year;
  if((d = adjHit(ST_Y0 + ST_STEP)))     h   = (uint8_t)((h + 24 + d) % 24);
  else if((d = adjHit(ST_Y0 + 2 * ST_STEP))) m = (uint8_t)((m + 60 + d) % 60);
  else if((d = adjHit(ST_Y0 + 3 * ST_STEP))){
    uint8_t dm = daysInMonth(yr, mon);
    day = (uint8_t)(day + d);
    if(day < 1) day = dm;
    if(day > dm) day = 1;
  }
  else if((d = adjHit(ST_Y0 + 4 * ST_STEP))) mon = (uint8_t)((mon + 12 + d) % 12);
  else if((d = adjHit(ST_Y0 + 5 * ST_STEP))) yr  = (uint16_t)(yr + d);
  else return;
  timeSet(h, m, day, mon, yr);
  storeSaveTime();
  homeInvalidate();
  redraw();
}

static void tickAbout(){
  if(hitRect(tp.x, tp.y, 40, 200, SCR_W - 80, 50)){
    storeFactoryReset();
    homeInvalidate();
    shellGoHome();
  }
}

void appSettingsTick(){
  if(!tp.tap) return;
  if(uiBackHit(tp.x, tp.y)){
    if(page == PG_INDEX) return;         // la barra de navegacion cierra la app
    page = PG_INDEX;
    redraw();
    return;
  }
  switch(page){
    case PG_DISPLAY: tickDisplay(); break;
    case PG_LOCK:    tickLock();    break;
    case PG_LANG:    tickLang();    break;
    case PG_TIME:    tickTime();    break;
    case PG_ABOUT:   tickAbout();   break;
    default:         tickIndex();   break;
  }
}
