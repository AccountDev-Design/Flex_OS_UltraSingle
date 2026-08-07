// #############################################################
// ##  Flex OS UltraSingle  -  fos_store.cpp
// #############################################################
#include "fos_store.h"
#include "fos_str.h"
#include "fos_time.h"
#include "fos_theme.h"
#include <EEPROM.h>
#include <string.h>

#define EE_MAGIC0 'F'
#define EE_MAGIC1 'S'
#define EE_VERSION 1

bool     gGlass      = true;
bool     gIconGlass  = false;
bool     gNavGest    = false;
uint8_t  gAutoLockIdx = 1;
bool     gPinSet     = false;
uint8_t  gPin[LOCK_PIN_LEN] = { 0, 0, 0, 0 };
uint8_t  gHomeOrder[HOME_SLOTS] = { 0,1,2,3,4,5,6,7,8,9,10,11 };
uint16_t gBest    = 0;
uint32_t gTouches = 0;
uint32_t gMinutes = 0;
bool     gFirstRun = false;

static void put32(int addr, uint32_t v){
  for(uint8_t i = 0; i < 4; i++) EEPROM.update(addr + i, (uint8_t)(v >> (8 * i)));
}
static uint32_t get32(int addr){
  uint32_t v = 0;
  for(uint8_t i = 0; i < 4; i++) v |= (uint32_t)EEPROM.read(addr + i) << (8 * i);
  return v;
}

void storeSaveFlags(){
  uint8_t f = 0;
  if(gDark)      f |= 0x01;
  if(gGlass)     f |= 0x02;
  if(gIconGlass) f |= 0x04;
  if(gH24)       f |= 0x08;
  if(gNavGest)   f |= 0x10;
  EEPROM.update(EE_FLAGS, f);
}
void storeSaveLang()     { EEPROM.update(EE_LANG, gLang); }
void storeSaveAutoLock() { EEPROM.update(EE_AUTOLOCK, gAutoLockIdx); }
void storeSaveBest()     { EEPROM.update(EE_BEST, (uint8_t)gBest); EEPROM.update(EE_BEST + 1, (uint8_t)(gBest >> 8)); }
void storeSaveUsage()    { put32(EE_TOUCHES, gTouches); put32(EE_MINUTES, gMinutes); }

void storeSavePin(){
  EEPROM.update(EE_PINSET, gPinSet ? 1 : 0);
  for(uint8_t i = 0; i < LOCK_PIN_LEN; i++) EEPROM.update(EE_PIN + i, gPin[i]);
}
void storeSaveHomeOrder(){
  for(uint8_t i = 0; i < HOME_SLOTS; i++) EEPROM.update(EE_HOMEORD + i, gHomeOrder[i]);
}
void storeSaveTime(){
  EEPROM.update(EE_TIME + 0, gTime.h);
  EEPROM.update(EE_TIME + 1, gTime.m);
  EEPROM.update(EE_TIME + 2, gTime.day);
  EEPROM.update(EE_TIME + 3, gTime.mon);
  EEPROM.update(EE_TIME + 4, (uint8_t)(gTime.year - 2000));
}
void storeLoadTime(){
  uint8_t h = EEPROM.read(EE_TIME + 0), m = EEPROM.read(EE_TIME + 1);
  uint8_t d = EEPROM.read(EE_TIME + 2), mo = EEPROM.read(EE_TIME + 3);
  uint8_t y = EEPROM.read(EE_TIME + 4);
  if(h < 24 && m < 60 && d >= 1 && d <= 31 && mo < 12 && y < 100)
    timeSet(h, m, d, mo, (uint16_t)2000 + y);
}

void storeFactoryReset(){
  EEPROM.update(EE_MAGIC + 0, EE_MAGIC0);
  EEPROM.update(EE_MAGIC + 1, EE_MAGIC1);
  EEPROM.update(EE_VER, EE_VERSION);
  gLang = LANG_ES;
  gDark = true; gGlass = true; gIconGlass = false; gH24 = false; gNavGest = false;
  gAutoLockIdx = 1;
  gPinSet = false;
  for(uint8_t i = 0; i < LOCK_PIN_LEN; i++) gPin[i] = 0;
  for(uint8_t i = 0; i < HOME_SLOTS; i++)   gHomeOrder[i] = i;
  gBest = 0; gTouches = 0; gMinutes = 0;
  storeSaveLang(); storeSaveFlags(); storeSaveAutoLock(); storeSavePin();
  storeSaveHomeOrder(); storeSaveBest(); storeSaveUsage(); storeSaveTime();
  for(uint8_t s = 0; s < NOTE_SLOTS; s++) EEPROM.update(EE_NOTES + s * (NOTE_LEN + 1), 0);
}

void storeInit(){
  if(EEPROM.read(EE_MAGIC) != EE_MAGIC0 || EEPROM.read(EE_MAGIC + 1) != EE_MAGIC1 ||
     EEPROM.read(EE_VER)   != EE_VERSION){
    storeFactoryReset();
    gFirstRun = true;
    return;
  }
  uint8_t l = EEPROM.read(EE_LANG);
  gLang = (l < LANG_N) ? l : (uint8_t)LANG_ES;
  uint8_t f = EEPROM.read(EE_FLAGS);
  gDark      = f & 0x01;
  gGlass     = f & 0x02;
  gIconGlass = f & 0x04;
  gH24       = f & 0x08;
  gNavGest   = f & 0x10;
  gAutoLockIdx = EEPROM.read(EE_AUTOLOCK);
  if(gAutoLockIdx >= AUTOLOCK_OPTIONS) gAutoLockIdx = 1;
  gPinSet = EEPROM.read(EE_PINSET) == 1;
  for(uint8_t i = 0; i < LOCK_PIN_LEN; i++){
    uint8_t d = EEPROM.read(EE_PIN + i);
    gPin[i] = (d < 10) ? d : 0;
  }
  bool ordOk = true;
  uint16_t seen = 0;
  for(uint8_t i = 0; i < HOME_SLOTS; i++){
    uint8_t v = EEPROM.read(EE_HOMEORD + i);
    if(v >= APP_N || (seen & (1u << v))){ ordOk = false; break; }
    seen |= (1u << v);
    gHomeOrder[i] = v;
  }
  if(!ordOk) for(uint8_t i = 0; i < HOME_SLOTS; i++) gHomeOrder[i] = i;
  gBest    = (uint16_t)EEPROM.read(EE_BEST) | ((uint16_t)EEPROM.read(EE_BEST + 1) << 8);
  gTouches = get32(EE_TOUCHES);
  gMinutes = get32(EE_MINUTES);
}

// ---------------- Notas ----------------
static int noteAddr(uint8_t slot){ return EE_NOTES + (int)slot * (NOTE_LEN + 1); }

void noteLoad(uint8_t slot, char* dst){
  if(slot >= NOTE_SLOTS){ dst[0] = 0; return; }
  int a = noteAddr(slot);
  uint8_t i = 0;
  for(; i < NOTE_LEN; i++){
    char c = (char)EEPROM.read(a + i);
    if(c == 0 || (uint8_t)c == 0xFF) break;
    dst[i] = c;
  }
  dst[i] = 0;
}

void noteSave(uint8_t slot, const char* src){
  if(slot >= NOTE_SLOTS) return;
  int a = noteAddr(slot);
  uint8_t i = 0;
  for(; i < NOTE_LEN && src[i]; i++) EEPROM.update(a + i, (uint8_t)src[i]);
  EEPROM.update(a + i, 0);
}

bool noteEmpty(uint8_t slot){
  if(slot >= NOTE_SLOTS) return true;
  uint8_t c = EEPROM.read(noteAddr(slot));
  return c == 0 || c == 0xFF;
}

// ---------------- SRAM libre ----------------
uint16_t freeRam(){
  extern int  __heap_start;
  extern int* __brkval;
  int v;
  return (uint16_t)((int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval));
}
