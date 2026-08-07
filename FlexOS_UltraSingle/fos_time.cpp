// #############################################################
// ##  Flex OS UltraSingle  -  fos_time.cpp
// #############################################################
#include "fos_time.h"
#include "fos_str.h"
#include "fos_gfx.h"
#include <string.h>

FosTime gTime;
bool    gH24 = false;

static uint32_t lastMs   = 0;    // ultimo instante consumido de millis()
static uint32_t bootMs   = 0;

bool isLeap(uint16_t y){ return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

static const uint8_t MDAYS[12] PROGMEM = { 31,28,31,30,31,30,31,31,30,31,30,31 };

uint8_t daysInMonth(uint16_t y, uint8_t mon){
  mon %= 12;
  uint8_t d = pgm_read_byte(&MDAYS[mon]);
  if(mon == 1 && isLeap(y)) d = 29;
  return d;
}

// Congruencia de Sakamoto: dia de la semana sin tablas de calendario.
uint8_t dowOf(uint16_t y, uint8_t mon, uint8_t day){
  static const uint8_t T[12] PROGMEM = { 0,3,2,5,0,3,5,1,4,6,2,4 };
  uint16_t yy = y;
  if(mon < 2) yy--;
  return (uint8_t)((yy + yy/4 - yy/100 + yy/400 + pgm_read_byte(&T[mon % 12]) + day) % 7);
}

void timeSet(uint8_t h, uint8_t m, uint8_t day, uint8_t mon, uint16_t year){
  gTime.h = h % 24; gTime.m = m % 60; gTime.s = 0;
  gTime.mon = mon % 12;
  if(day < 1) day = 1;
  uint8_t dm = daysInMonth(year, gTime.mon);
  gTime.day = day > dm ? dm : day;
  gTime.year = year;
  gTime.dow = dowOf(year, gTime.mon, gTime.day);
  lastMs = millis();
}

void timeInit(){
  // Siembra por defecto, igual que Flex OS Ultra: sabado 4 de julio, 13:23.
  timeSet(13, 23, 4, 6, 2026);
  bootMs = millis();
  lastMs = bootMs;
}

bool timeTick(){
  uint32_t now = millis();
  uint32_t diff = now - lastMs;             // seguro ante el desbordamiento
  if(diff < 1000UL) return false;
  uint16_t secs = (uint16_t)(diff / 1000UL);
  lastMs += (uint32_t)secs * 1000UL;
  bool minuteChanged = false;
  while(secs--){
    if(++gTime.s < 60) continue;
    gTime.s = 0;
    minuteChanged = true;
    if(++gTime.m < 60) continue;
    gTime.m = 0;
    if(++gTime.h < 24) continue;
    gTime.h = 0;
    gTime.dow = (uint8_t)((gTime.dow + 1) % 7);
    if(++gTime.day <= daysInMonth(gTime.year, gTime.mon)) continue;
    gTime.day = 1;
    if(++gTime.mon < 12) continue;
    gTime.mon = 0;
    gTime.year++;
  }
  return minuteChanged;
}

uint32_t uptimeSec(){ return (millis() - bootMs) / 1000UL; }

void timeStr(char* dst){ fmtClock(dst, gTime.h, gTime.m, gH24); }

static char* appendP(char* dst, PGM_P src){
  char c;
  while((c = (char)pgm_read_byte(src++))) *dst++ = c;
  *dst = 0;
  return dst;
}

void dateShortStr(char* dst){
  dst = appendP(dst, dayShortP(gTime.dow));
  *dst++ = ','; *dst++ = ' ';
  dst = fmtU16(dst, gTime.day);
  *dst++ = ' ';
  appendP(dst, monthShortP(gTime.mon));
}

void dateLongStr(char* dst){
  dst = appendP(dst, dayFullP(gTime.dow));
  *dst++ = ','; *dst++ = ' ';
  dst = fmtU16(dst, gTime.day);
  *dst++ = ' ';
  // "4 de julio" en espanol, "4 July" en ingles.
  if(gLang == LANG_ES){ *dst++ = 'd'; *dst++ = 'e'; *dst++ = ' '; }
  appendP(dst, monthFullP(gTime.mon));
}
