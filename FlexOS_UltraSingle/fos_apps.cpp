// #############################################################
// ##  Flex OS UltraSingle  -  fos_apps.cpp
// ##  Tabla de aplicaciones (PROGMEM)
// #############################################################
#include "fos_apps.h"

const FApp APPS[APP_N] PROGMEM = {
  { appClockEnter,    appClockTick,    AF_NONE       },  // APP_CLOCK
  { appCalendarEnter, appCalendarTick, AF_NONE       },  // APP_CALENDAR
  { appCalcEnter,     appCalcTick,     AF_NONE       },  // APP_CALC
  { appNotesEnter,    appNotesTick,    AF_NONE       },  // APP_NOTES
  { appPaintEnter,    appPaintTick,    AF_NONE       },  // APP_PAINT
  { appGamesEnter,    appGamesTick,    AF_NONE       },  // APP_GAMES
  { appTimerEnter,    appTimerTick,    AF_NONE       },  // APP_TIMER
  { appLightEnter,    appLightTick,    AF_FULLSCREEN },  // APP_LIGHT
  { appWellEnter,     appWellTick,     AF_NONE       },  // APP_WELL
  { appMemoryEnter,   appMemoryTick,   AF_NONE       },  // APP_MEMORY
  { appSysinfoEnter,  appSysinfoTick,  AF_NONE       },  // APP_SYSINFO
  { appSettingsEnter, appSettingsTick, AF_NONE       }   // APP_SETTINGS
};

static uint8_t curApp = 0xFF;

uint8_t appCurrent(){ return curApp; }

uint8_t appFlags(uint8_t id){
  if(id >= APP_N) return AF_NONE;
  return pgm_read_byte(&APPS[id].flags);
}

void appOpen(uint8_t id){
  if(id >= APP_N) return;
  curApp = id;
  FAppFn fn = (FAppFn)pgm_read_word(&APPS[id].enter);
  if(fn) fn();
}

void appTick(){
  if(curApp >= APP_N) return;
  FAppFn fn = (FAppFn)pgm_read_word(&APPS[curApp].tick);
  if(fn) fn();
}
