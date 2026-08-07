// #############################################################
// ##  Flex OS UltraSingle  -  app_calendar.cpp   (Calendario)
// #############################################################
//
//  Rejilla mensual con la cabecera, los flechados de mes y el dia
//  de hoy resaltado en el color de acento, igual que en Flex OS
//  Ultra. Sin eventos: no hay donde guardarlos en 4 KB de EEPROM
//  compartidos con las notas y los ajustes, y una agenda que se
//  pierde al reiniciar seria peor que no tenerla.
// #############################################################
#include "fos_apps.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_str.h"
#include "fos_time.h"
#include "fos_touch.h"

#define CAL_X0   14
#define CAL_CW   ((SCR_W - 2 * CAL_X0) / 7)
#define CAL_HY   96                      // fila de iniciales de dia
#define CAL_Y0   118                      // primera fila de numeros
#define CAL_RH   44

static uint8_t vMon;
static uint16_t vYear;

static void drawHeader(){
  tft.fillRect(0, APP_HDR_H, SCR_W, CAL_Y0 - APP_HDR_H, PAGE_BG);
  char b[20];
  int16_t x = 60;
  x = gfxTextP(x, 56, monthFullP(vMon), 2, TXT_HI);
  char* p = b;
  *p++ = ' ';
  fmtU16(p, vYear);
  gfxText(x, 56, b, 2, TXT_LO);
  // Flechas de mes (izquierda / derecha).
  tft.fillTriangle(34, 54, 34, 70, 22, 62, C_ACCENT);
  tft.fillTriangle(SCR_W - 34, 54, SCR_W - 34, 70, SCR_W - 22, 62, C_ACCENT);
  for(uint8_t d = 0; d < 7; d++){
    char ini[2] = { (char)pgm_read_byte(dayShortP(d)), 0 };
    gfxTextC(CAL_X0 + d * CAL_CW + CAL_CW / 2, CAL_HY, ini, 1,
             (d == 0 || d == 6) ? C_DANGER : TXT_LO);
  }
}

static void drawGrid(){
  tft.fillRect(0, CAL_Y0, SCR_W, APP_Y1 - CAL_Y0, PAGE_BG);
  uint8_t first = dowOf(vYear, vMon, 1);
  uint8_t dim   = daysInMonth(vYear, vMon);
  for(uint8_t d = 1; d <= dim; d++){
    uint8_t idx = first + d - 1;
    int16_t cx = CAL_X0 + (idx % 7) * CAL_CW + CAL_CW / 2;
    int16_t cy = CAL_Y0 + (idx / 7) * CAL_RH + CAL_RH / 2;
    bool today = (d == gTime.day && vMon == gTime.mon && vYear == gTime.year);
    if(today) tft.fillCircle(cx, cy, 16, C_ACCENT);
    char b[4];
    fmtU16(b, d);
    gfxTextC(cx, cy - 7, b, 2, today ? C_WHITE : TXT_HI);
  }
}

void appCalendarEnter(){
  vMon = gTime.mon;
  vYear = gTime.year;
  uiAppScreen(appNameP(APP_CALENDAR));
  drawHeader();
  drawGrid();
}

static void stepMonth(int8_t d){
  if(d < 0){ if(vMon == 0){ vMon = 11; vYear--; } else vMon--; }
  else      { if(vMon == 11){ vMon = 0; vYear++; } else vMon++; }
  drawHeader();
  drawGrid();
}

void appCalendarTick(){
  // Deslizar horizontalmente cambia de mes, como en Flex OS Ultra.
  if(tp.swipeLeft)  { stepMonth(+1); return; }
  if(tp.swipeRight) { stepMonth(-1); return; }
  if(!tp.tap) return;
  if(tp.y >= 46 && tp.y <= 78){
    if(tp.x < 60)          { stepMonth(-1); return; }
    if(tp.x > SCR_W - 60)  { stepMonth(+1); return; }
    // Tocar el titulo vuelve al mes actual.
    if(vMon != gTime.mon || vYear != gTime.year){
      vMon = gTime.mon; vYear = gTime.year;
      drawHeader(); drawGrid();
    }
  }
}
