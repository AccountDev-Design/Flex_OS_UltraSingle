// #############################################################
// ##  Flex OS UltraSingle  -  app_system.cpp
// ##  Bienestar, Memoria e Informacion del sistema
// #############################################################
//
//  Tres aplicaciones pequenas de solo lectura en un unico modulo:
//  comparten las mismas primitivas (fila de dato y barra de uso) y
//  separarlas en tres ficheros solo duplicaria codigo.
//
//  Todos los numeros son REALES, medidos en el propio equipo: SRAM
//  libre por el hueco entre el heap y la pila, Flash por el simbolo
//  del enlazador, EEPROM por el mapa fijo de fos_store.
// #############################################################
#include "fos_apps.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_str.h"
#include "fos_store.h"
#include "fos_time.h"
#include "fos_touch.h"
#include <string.h>

// Fin de la imagen en Flash (codigo + datos inicializados): lo publica el
// enlazador de avr-gcc, asi que el dato es exacto para esta compilacion.
extern char __data_load_end;

#define SY_Y0 56

// ---------------- Primitivas compartidas ----------------
static void statRow(int16_t y, PGM_P label, const char* value){
  uiCard(12, y, SCR_W - 24, ROW_H - 6);
  gfxTextP(26, y + (ROW_H - 6) / 2 - 7, label, 2, TXT_HI);
  gfxTextR(SCR_W - 26, y + (ROW_H - 6) / 2 - 4, value, 1, TXT_LO);
}

static void statRowP(int16_t y, PGM_P label, PGM_P value){
  uiCard(12, y, SCR_W - 24, ROW_H - 6);
  gfxTextP(26, y + (ROW_H - 6) / 2 - 7, label, 2, TXT_HI);
  gfxTextP(SCR_W - 26 - gfxTextWP(value, 1), y + (ROW_H - 6) / 2 - 4, value, 1, TXT_LO);
}

static void usageBar(int16_t y, PGM_P label, uint32_t used, uint32_t total, uint16_t col){
  uiCard(12, y, SCR_W - 24, 56);
  gfxTextP(26, y + 10, label, 2, TXT_HI);
  // "usado/total" en la unidad que evita truncar: bytes si el total cabe en
  // 16 bits, kilobytes si no (la Flash del Mega son 248 KB utiles).
  char b[24];
  uint32_t u = used, t = total;
  char unit = 'B';
  if(t > 9999){ u >>= 10; t >>= 10; unit = 'K'; }
  char* p = fmtU16(b, (uint16_t)u);
  *p++ = '/';
  p = fmtU16(p, (uint16_t)t);
  *p++ = unit;
  *p = 0;
  gfxTextR(SCR_W - 26, y + 12, b, 1, TXT_LO);
  int16_t bw = SCR_W - 24 - 28;
  uint8_t pc = total ? (uint8_t)((uint32_t)used * 100 / total) : 0;
  if(pc > 100) pc = 100;
  tft.fillRoundRect(26, y + 34, bw, 8, 4, gDark ? RGB(50,56,72) : RGB(214,220,232));
  if(pc) tft.fillRoundRect(26, y + 34, (int16_t)((int32_t)bw * pc / 100), 8, 4, col);
}

// #############################################################
//  Bienestar
// #############################################################
static const char BW_SESS[] PROGMEM = "Sesi\xA2n";
static const char BW_MIN[]  PROGMEM = "min";

void appWellEnter(){
  uiAppScreen(appNameP(APP_WELL));
  char b[24];
  uint32_t up = uptimeSec();
  char* p = fmtU16(b, (uint16_t)(up / 3600));
  *p++ = 'h'; *p++ = ' ';
  p = fmtU16(p, (uint16_t)((up / 60) % 60));
  *p++ = 'm'; *p = 0;
  statRow(SY_Y0, BW_SESS, b);

  p = fmtU16(b, (uint16_t)(gMinutes > 65535 ? 65535 : gMinutes));
  *p++ = ' ';
  strcpy_P(p, BW_MIN);
  statRow(SY_Y0 + ROW_H + 6, strP(S_TOTAL), b);

  fmtU16(b, (uint16_t)(gTouches > 65535 ? 65535 : gTouches));
  statRow(SY_Y0 + 2 * (ROW_H + 6), strP(S_TOUCHES), b);

  // Uso del dia frente a una referencia de 4 horas, en la misma barra que
  // usa Memoria: el objetivo es que se lea de un vistazo, no ser exacto.
  usageBar(SY_Y0 + 3 * (ROW_H + 6) + 8, strP(S_UPTIME), up / 60, 240, C_OK);
}

void appWellTick(){}

// #############################################################
//  Memoria
// #############################################################
void appMemoryEnter(){
  uiAppScreen(appNameP(APP_MEMORY));
  uint16_t fr = freeRam();
  usageBar(SY_Y0,           strP(S_RAM),    (uint32_t)(8192 - fr), 8192UL,   C_ACCENT);
  usageBar(SY_Y0 + 66,      strP(S_FLASH),  (uint32_t)&__data_load_end, 253952UL, C_WARN);
  usageBar(SY_Y0 + 132,     strP(S_EEPROM), (uint32_t)EE_END, 4096UL,   C_OK);

  char b[16];
  fmtU16(b, fr);
  statRow(SY_Y0 + 204, strP(S_FREE), b);
}

void appMemoryTick(){}

// #############################################################
//  Informacion del sistema
// #############################################################
static const char SI_OS[]    PROGMEM = FOS_NAME;
static const char SI_BRD[]   PROGMEM = FOS_BOARD;
static const char SI_PAN[]   PROGMEM = FOS_PANEL;
static const char SI_VER[]   PROGMEM = FOS_VERSION;
static const char SI_MCU[]   PROGMEM = "MCU";
static const char SI_MCUV[]  PROGMEM = "ATmega2560 16 MHz";
static const char SI_RES[]   PROGMEM = "Resoluci\xA2n";
static const char SI_RESV[]  PROGMEM = "320x480";
static const char SI_BUS[]   PROGMEM = "Bus";
static const char SI_BUSV[]  PROGMEM = "8 bits paralelo";
static const char SI_TCH[]   PROGMEM = "T\xA0" "ctil";
static const char SI_TCHV[]  PROGMEM = "Resistivo 4 hilos";

void appSysinfoEnter(){
  uiAppScreen(appNameP(APP_SYSINFO));
  gfxTextCP(SCR_W / 2, 52, SI_OS, 2, TXT_HI);
  gfxTextCP(SCR_W / 2, 74, SI_VER, 1, C_ACCENT);
  int16_t y = 100;
  statRowP(y, strP(S_BOARD), SI_BRD);            y += ROW_H;
  statRowP(y, SI_MCU,        SI_MCUV);           y += ROW_H;
  statRowP(y, strP(S_PANEL), SI_PAN);            y += ROW_H;
  statRowP(y, SI_RES,        SI_RESV);           y += ROW_H;
  statRowP(y, SI_BUS,        SI_BUSV);           y += ROW_H;
  statRowP(y, SI_TCH,        SI_TCHV);           y += ROW_H;
  char b[16];
  fmtU16(b, freeRam());
  statRow(y, strP(S_RAM), b);
}

void appSysinfoTick(){}
