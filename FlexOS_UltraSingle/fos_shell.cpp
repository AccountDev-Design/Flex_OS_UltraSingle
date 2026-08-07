// #############################################################
// ##  Flex OS UltraSingle  -  fos_shell.cpp
// ##  Arranque, OOBE y bucle principal
// #############################################################
#include "fos_shell.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_fx.h"
#include "fos_icons.h"
#include "fos_str.h"
#include "fos_time.h"
#include "fos_store.h"
#include "fos_touch.h"
#include "fos_apps.h"
#include "fos_home.h"
#include "fos_lock.h"

uint8_t gState     = ST_SPLASH;
bool    gMinuteTick = false;

static uint32_t splashT0 = 0;
static uint32_t lastFrame = 0;

// -------------------------------------------------------------
//  Arranque
// -------------------------------------------------------------
static const char SP_NAME[] PROGMEM = "Flex OS";
static const char SP_EDIT[] PROGMEM = FOS_SHORT;
static const char SP_BRD[]  PROGMEM = FOS_BOARD;

static void splashDraw(){
  tft.fillScreen(C_BLACK);
  // Aparicion progresiva del anillo de marca (sin framebuffer: se dibuja
  // circulo a circulo directamente sobre la GRAM).
  for(uint8_t r = 4; r <= 34; r += 3){
    tft.drawCircle(SCR_W / 2, 180, r, mix565(C_BLACK, C_ACCENT, (uint8_t)(60 + r * 5)));
  }
  tft.fillCircle(SCR_W / 2, 180, 12, C_ACCENT);
  gfxTextCP(SCR_W / 2, 250, SP_NAME, 3, C_WHITE);
  gfxTextCP(SCR_W / 2, 282, SP_EDIT, 2, C_ACCENT);
  gfxTextCP(SCR_W / 2, SCR_H - 40, SP_BRD, 1, RGB(110,116,132));
}

// -------------------------------------------------------------
//  OOBE: eleccion de idioma
// -------------------------------------------------------------
static const char OO_ES[] PROGMEM = "Espa\xA4ol";
static const char OO_EN[] PROGMEM = "English";

#define OO_Y0 200
#define OO_H  64

static void oobeDraw(){
  gfxWallpaper();
  gfxTextCP(SCR_W / 2, 120, strP(S_LANGUAGE), 3, C_ON_WALL);
  for(uint8_t i = 0; i < 2; i++){
    int16_t y = OO_Y0 + i * (OO_H + 16);
    bool sel = (gLang == i);
    if(gGlass) glassRect(30, y, SCR_W - 60, OO_H, 18, sel ? C_ACCENT : GLASS_CARD, sel ? 200 : 120);
    else       tft.fillRoundRect(30, y, SCR_W - 60, OO_H, 18, sel ? C_ACCENT : RGB(44,50,68));
    gfxTextCP(SCR_W / 2, y + OO_H / 2 - 7, i == 0 ? OO_ES : OO_EN, 2, C_WHITE);
  }
  uiButtonP(60, 380, SCR_W - 120, 46, strP(S_CONTINUE), true);
}

static void oobeTick(){
  if(!tp.tap) return;
  for(uint8_t i = 0; i < 2; i++){
    int16_t y = OO_Y0 + i * (OO_H + 16);
    if(hitRect(tp.x, tp.y, 30, y, SCR_W - 60, OO_H)){
      gLang = i;
      storeSaveLang();
      oobeDraw();
      return;
    }
  }
  if(hitRect(tp.x, tp.y, 60, 380, SCR_W - 120, 46)){
    storeSaveLang();
    gState = ST_HOME;
    touchConsume();
    homeEnter();
  }
}

// -------------------------------------------------------------
//  Transiciones de estado
// -------------------------------------------------------------
void shellGoHome(){
  gState = ST_HOME;
  touchConsume();
  homeInvalidate();
  homeEnter();
}

void shellGoLock(){
  gState = ST_LOCK;
  touchConsume();
  fxWipe(false);
  lockEnter();
}

void shellRequestAuth(uint8_t reason){
  gState = ST_AUTH;
  touchConsume();
  authEnter(reason);
}

void shellOpenApp(uint8_t id, int16_t ix, int16_t iy, uint8_t is){
  fxOpenApp(ix, iy, is);
  gState = ST_APP;
  touchConsume();
  appOpen(id);
}

// -------------------------------------------------------------
//  setup / loop
// -------------------------------------------------------------
void shellSetup(){
  gfxInit();
  tft.cp437(true);                 // acentos correctos en la fuente 5x7
  storeInit();
  timeInit();
  storeLoadTime();
  touchInit();
  splashDraw();
  splashT0 = millis();
  lastFrame = millis();
}

void shellLoop(){
  touchPoll();
  if(tp.pressed) gTouches++;

  gMinuteTick = timeTick();
  if(gMinuteTick){
    gMinutes++;
    // La EEPROM tiene ~100.000 ciclos por celda: la hora y el contador de uso
    // se vuelcan cada 15 minutos, no cada minuto.
    if((gMinutes % 15) == 0){ storeSaveTime(); storeSaveUsage(); }
  }

  switch(gState){
    case ST_SPLASH:
      if(millis() - splashT0 >= SPLASH_MS){
        if(gFirstRun){ gState = ST_OOBE_LANG; oobeDraw(); }
        else         { gState = ST_LOCK; lockEnter(); }
      }
      break;

    case ST_OOBE_LANG:
      oobeTick();
      break;

    case ST_LOCK:
      lockTick();
      break;

    case ST_AUTH:
      authTick();
      break;

    case ST_HOME:
      homeTick();
      break;

    case ST_APP: {
      uint8_t f = appFlags(appCurrent());
      if(!(f & AF_FULLSCREEN) && tp.tap){
        int8_t n = uiNavHit(tp.x, tp.y);
        if(n == 0 || n == 1){
          fxCloseApp();
          shellGoHome();
          return;
        }
        if(n == 2) return;             // sin conmutador de tareas
      }
      appTick();
      if(!(f & AF_FULLSCREEN)) uiToastTick();
    } break;
  }

  // Bloqueo automatico por inactividad.
  if((gState == ST_HOME || gState == ST_APP) && autoLockMs()){
    if(millis() - touchLastActivity() >= autoLockMs()) shellGoLock();
  }

  // Ritmo del bucle: sin esto el tactil resistivo se sobremuestrea y el
  // sistema gasta el bus en repintados que nadie ve.
  uint32_t now = millis();
  if(now - lastFrame < TICK_MS) delay(TICK_MS - (now - lastFrame));
  lastFrame = millis();
}
