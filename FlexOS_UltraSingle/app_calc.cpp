// #############################################################
// ##  Flex OS UltraSingle  -  app_calc.cpp   (Calculadora)
// #############################################################
//
//  Misma disposicion que la calculadora de Flex OS Ultra: visor
//  alineado a la derecha y rejilla 4x5 con la columna de operadores
//  en el naranja del sistema.
//
//  Solo se repinta la TECLA pulsada y el visor: en el bus de 8 bits
//  del Mega, redibujar las veinte teclas por pulsacion se notaria.
// #############################################################
#include "fos_apps.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_str.h"
#include "fos_touch.h"
#include <string.h>
#include <stdlib.h>

#define CC_X0   12
#define CC_Y0   140
#define CC_W    69
#define CC_H    54
#define CC_CS   75
#define CC_RS   60
#define DISP_Y  56
#define DISP_H  70

// '~' = cambio de signo, '<' = borrar el ultimo digito.
static const char CKEYS[20] PROGMEM = {
  'C','~','%','/',
  '7','8','9','*',
  '4','5','6','-',
  '1','2','3','+',
  '0','.','<','='
};

static char  entry[14];
static float acc     = 0;
static char  pending = 0;
static bool  fresh   = true;    // el visor muestra un resultado, no una entrada

static void keyRect(uint8_t i, int16_t &x, int16_t &y){
  x = CC_X0 + (i % 4) * CC_CS;
  y = CC_Y0 + (i / 4) * CC_RS;
}

static void drawKey(uint8_t i, bool down){
  int16_t x, y;
  keyRect(i, x, y);
  char c = (char)pgm_read_byte(&CKEYS[i]);
  bool oper = (i % 4 == 3) || c == 'C' || c == '~' || c == '%';
  uint16_t bg = oper ? ((i % 4 == 3) ? RGB(245,150,30) : RGB(150,150,155)) : CARD_ALT;
  if(down) bg = mix565(bg, C_WHITE, 90);
  tft.fillRoundRect(x, y, CC_W, CC_H, 14, bg);
  char s[3];
  if(c == '~')      { s[0] = '+'; s[1] = '/'; s[2] = 0; }
  else if(c == '<') { s[0] = '<'; s[1] = '-'; s[2] = 0; }
  else              { s[0] = c;   s[1] = 0; }
  gfxTextC(x + CC_W / 2, y + CC_H / 2 - 10, s, s[1] ? 2 : 3, oper ? C_WHITE : TXT_HI);
}

static void drawDisplay(){
  tft.fillRect(0, DISP_Y, SCR_W, DISP_H, PAGE_BG);
  uint8_t sz = strlen(entry) > 10 ? 2 : 3;
  gfxTextR(SCR_W - 16, DISP_Y + DISP_H - 12 - TXT_H(sz), entry, sz, TXT_HI);
  if(pending) tft.fillCircle(20, DISP_Y + 20, 4, RGB(245,150,30));
}

// Formatea 'v' sin arrastrar el printf de coma flotante (que en AVR pesa 1,5 KB).
static void setEntry(float v){
  dtostrf(v, 0, 6, entry);
  char* dot = strchr(entry, '.');
  if(dot){
    char* e = entry + strlen(entry) - 1;
    while(e > dot && *e == '0') *e-- = 0;
    if(e == dot) *e = 0;
  }
  if(strlen(entry) > 13) entry[13] = 0;
}

static void applyPending(){
  float v = (float)atof(entry);
  switch(pending){
    case '+': acc += v; break;
    case '-': acc -= v; break;
    case '*': acc *= v; break;
    case '/': acc = (v != 0) ? acc / v : 0; break;
    default:  acc = v;  break;
  }
  setEntry(acc);
}

static void press(char c){
  if(c >= '0' && c <= '9'){
    if(fresh){ entry[0] = 0; fresh = false; }
    uint8_t n = strlen(entry);
    if(n < 12 && !(n == 1 && entry[0] == '0')){ entry[n] = c; entry[n + 1] = 0; }
    else if(n == 1 && entry[0] == '0'){ entry[0] = c; }
    return;
  }
  switch(c){
    case 'C': entry[0] = '0'; entry[1] = 0; acc = 0; pending = 0; fresh = true; break;
    case '.':
      if(fresh){ strcpy(entry, "0"); fresh = false; }
      if(!strchr(entry, '.') && strlen(entry) < 12) strcat(entry, ".");
      break;
    case '~':
      if(entry[0] == '-') memmove(entry, entry + 1, strlen(entry));
      else { memmove(entry + 1, entry, strlen(entry) + 1); entry[0] = '-'; }
      break;
    case '<': {
      uint8_t n = strlen(entry);
      if(n > 1) entry[n - 1] = 0;
      else { entry[0] = '0'; entry[1] = 0; fresh = true; }
    } break;
    case '%': setEntry((float)atof(entry) / 100.0f); fresh = true; break;
    case '=':
      applyPending();
      pending = 0;
      fresh = true;
      break;
    default:                                  // + - * /
      applyPending();
      pending = c;
      fresh = true;
      break;
  }
}

void appCalcEnter(){
  uiAppScreen(appNameP(APP_CALC));
  strcpy(entry, "0");
  acc = 0; pending = 0; fresh = true;
  drawDisplay();
  for(uint8_t i = 0; i < 20; i++) drawKey(i, false);
}

void appCalcTick(){
  if(!tp.tap) return;
  for(uint8_t i = 0; i < 20; i++){
    int16_t x, y;
    keyRect(i, x, y);
    if(!hitRect(tp.x, tp.y, x, y, CC_W, CC_H)) continue;
    drawKey(i, true);
    press((char)pgm_read_byte(&CKEYS[i]));
    drawDisplay();
    drawKey(i, false);
    return;
  }
}
