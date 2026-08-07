// #############################################################
// ##  Flex OS UltraSingle  -  fos_home.cpp
// #############################################################
//
//  La misma composicion de Flex OS Ultra -- barra superior, dos
//  widgets, rejilla 4x3, dock de cuatro accesos y barra de
//  navegacion -- reescalada de 480x800 a 320x480.
//
//  Los widgets muestran DATOS REALES del equipo. En el P4 eran
//  maquetas de clima y noticias porque alli habia Wi-Fi previsto;
//  en un Mega sin radio, un widget de clima solo podria mentir, asi
//  que ocupan su mismo hueco con lo que el sistema si sabe: memoria
//  libre y la primera nota guardada.
// #############################################################
#include "fos_home.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_icons.h"
#include "fos_str.h"
#include "fos_time.h"
#include "fos_store.h"
#include "fos_apps.h"
#include "fos_shell.h"
#include "fos_touch.h"
#include <string.h>

// ---- Geometria ----
#define WG_Y    40
#define WG_H    76
#define WG_W    142
#define WG_XL   14
#define WG_XR   164

#define GR_X0   20
#define GR_Y0   128
#define GR_S    52
#define GR_CSTEP 76
#define GR_RSTEP 76

#define DOT_Y   362

#define DK_X    20
#define DK_Y    372
#define DK_W    280
#define DK_H    62
#define DK_S    44

static const uint8_t DOCK_APPS[DOCK_SLOTS] PROGMEM = {
  APP_SETTINGS, APP_CALC, APP_NOTES, APP_PAINT
};

static bool    edit     = false;   // Modo Edicion (reordenar iconos)
static int8_t  editSel  = -1;      // ranura seleccionada
static bool    qsOpen   = false;   // panel rapido desplegado
static bool    dirty    = true;

void homeInvalidate(){ dirty = true; }

// -------------------------------------------------------------
//  Widgets
// -------------------------------------------------------------
static const char W_MEM[]  PROGMEM = "Memoria";
static const char W_MEMB[] PROGMEM = "B libres";

static void drawWidgets(){
  // Izquierda: memoria libre, con barra de ocupacion.
  if(gGlass) glassRect(WG_XL, WG_Y, WG_W, WG_H, 14, C_ACCENT_DK, 150);
  else       tft.fillRoundRect(WG_XL, WG_Y, WG_W, WG_H, 14, C_ACCENT_DK);
  gfxTextP(WG_XL + 12, WG_Y + 10, W_MEM, 1, C_ON_WALL);
  char b[12];
  uint16_t fr = freeRam();
  fmtU16(b, fr);
  gfxText(WG_XL + 12, WG_Y + 26, b, 2, C_ON_WALL);
  gfxTextP(WG_XL + 12, WG_Y + 46, W_MEMB, 1, C_ON_WALL_LO);
  uint8_t pc = (uint8_t)(((uint32_t)fr * 100) / 8192);
  if(pc > 100) pc = 100;
  tft.fillRoundRect(WG_XL + 12, WG_Y + WG_H - 14, WG_W - 24, 5, 2, RGB(20,30,60));
  tft.fillRoundRect(WG_XL + 12, WG_Y + WG_H - 14, (WG_W - 24) * pc / 100, 5, 2, C_OK);

  // Derecha: primera nota guardada.
  if(gGlass) glassRect(WG_XR, WG_Y, WG_W, WG_H, 14, GLASS_CARD, 120);
  else       tft.fillRoundRect(WG_XR, WG_Y, WG_W, WG_H, 14, RGB(46,52,70));
  gfxTextP(WG_XR + 12, WG_Y + 10, appNameP(APP_NOTES), 1, C_ON_WALL);
  if(noteEmpty(0)){
    gfxTextCP(WG_XR + WG_W / 2, WG_Y + WG_H / 2 - 3, strP(S_EMPTY), 1, C_ON_WALL_LO);
  } else {
    char n[NOTE_LEN + 1];
    noteLoad(0, n);
    // Dos lineas de 21 caracteres: lo que cabe a tamano 1 en la tarjeta.
    char line[22];
    for(uint8_t r = 0; r < 2; r++){
      uint8_t o = r * 21;
      if(o >= strlen(n)) break;
      strncpy(line, n + o, 21);
      line[21] = 0;
      gfxText(WG_XR + 12, WG_Y + 30 + r * 12, line, 1, C_ON_WALL_LO);
    }
  }
}

// -------------------------------------------------------------
//  Rejilla y dock
// -------------------------------------------------------------
static void slotXY(uint8_t i, int16_t &x, int16_t &y){
  x = GR_X0 + (i % 4) * GR_CSTEP;
  y = GR_Y0 + (i / 4) * GR_RSTEP;
}

static void drawSlot(uint8_t i){
  int16_t x, y;
  slotXY(i, x, y);
  gfxWallRect(x - 4, y - 4, GR_S + 8, GR_S + 24);
  if(edit && editSel == (int8_t)i)
    tft.drawRoundRect(x - 4, y - 4, GR_S + 8, GR_S + 8, 14, C_WHITE);
  drawAppIcon(gHomeOrder[i], x, y, GR_S);
  gfxTextCP(x + GR_S / 2, y + GR_S + 5, appNameP(gHomeOrder[i]), 1, C_ON_WALL);
}

static void drawGrid(){ for(uint8_t i = 0; i < HOME_SLOTS; i++) drawSlot(i); }

static void drawDock(){
  if(gGlass) glassRect(DK_X, DK_Y, DK_W, DK_H, 20, GLASS_DOCK, 90);
  else       tft.fillRoundRect(DK_X, DK_Y, DK_W, DK_H, 20, RGB(40,46,64));
  for(uint8_t i = 0; i < DOCK_SLOTS; i++)
    drawAppIcon(pgm_read_byte(&DOCK_APPS[i]), DK_X + 12 + i * 70, DK_Y + (DK_H - DK_S) / 2, DK_S);
}

// -------------------------------------------------------------
//  Panel rapido (cortina)
// -------------------------------------------------------------
//  Es la cortina de Flex OS Ultra reducida a lo que el Mega puede
//  ofrecer de verdad: apariencia, vidrio, linterna y bloquear.
#define QS_Y 0
#define QS_H 210

static const char QS_T[] PROGMEM = "Panel r\xA0pido";
static const char QS_LOCK[] PROGMEM = "Bloquear";
static const char QS_LIGHT[] PROGMEM = "Linterna";

static void drawQuick(){
  glassRect(8, QS_Y + 8, SCR_W - 16, QS_H, 22, GLASS_CLOCK, 190);
  gfxTextP(28, QS_Y + 24, QS_T, 2, C_ON_WALL);
  int16_t y = QS_Y + 56;
  gfxTextP(28, y + 4, strP(S_DARKMODE), 1, C_ON_WALL);
  uiToggle(SCR_W - 80, y, gDark);
  y += 34;
  gfxTextP(28, y + 4, strP(S_GLASS), 1, C_ON_WALL);
  uiToggle(SCR_W - 80, y, gGlass);
  y += 34;
  gfxTextP(28, y + 4, QS_LIGHT, 1, C_ON_WALL);
  gfxChevron(SCR_W - 60, y + 10, 6, C_ON_WALL);
  y += 34;
  gfxTextP(28, y + 4, QS_LOCK, 1, C_ON_WALL);
  gfxChevron(SCR_W - 60, y + 10, 6, C_ON_WALL);
  drawHomeIndicator(QS_Y + QS_H + 8, C_ON_WALL);
}

static void closeQuick(){
  qsOpen = false;
  gfxWallRect(0, 0, SCR_W, QS_Y + QS_H + 16);
  uiStatusBar(true);
  drawWidgets();
  for(uint8_t i = 0; i < 4; i++) drawSlot(i);          // primera fila de la rejilla
  for(uint8_t i = 4; i < 8; i++) drawSlot(i);
}

// -------------------------------------------------------------
//  Pintado completo
// -------------------------------------------------------------
void homeEnter(){
  edit = false; editSel = -1; qsOpen = false; dirty = false;
  gfxWallpaper();
  uiStatusBar(true);
  drawWidgets();
  drawGrid();
  tft.fillCircle(SCR_W / 2, DOT_Y, 3, C_WHITE);
  drawDock();
  uiNavBar(true);
}

// -------------------------------------------------------------
//  Interaccion
// -------------------------------------------------------------
static bool hitIcon(int16_t px, int16_t py, uint8_t &slot, uint8_t &app,
                    int16_t &ix, int16_t &iy, uint8_t &is){
  for(uint8_t i = 0; i < HOME_SLOTS; i++){
    int16_t x, y;
    slotXY(i, x, y);
    if(hitRect(px, py, x - 6, y - 4, GR_S + 12, GR_S + 18)){
      slot = i; app = gHomeOrder[i]; ix = x; iy = y; is = GR_S;
      return true;
    }
  }
  for(uint8_t i = 0; i < DOCK_SLOTS; i++){
    int16_t x = DK_X + 12 + i * 70, y = DK_Y + (DK_H - DK_S) / 2;
    if(hitRect(px, py, x, y, DK_S, DK_S)){
      slot = 0xFF; app = pgm_read_byte(&DOCK_APPS[i]); ix = x; iy = y; is = DK_S;
      return true;
    }
  }
  return false;
}

void homeTick(){
  if(dirty){ homeEnter(); return; }
  if(gMinuteTick && !qsOpen) uiStatusClock(true);

  // --- Panel rapido ---
  if(qsOpen){
    if(tp.tap){
      int16_t y = QS_Y + 56;
      if(hitRect(tp.x, tp.y, 20, y - 4, SCR_W - 40, 30)){
        gDark = !gDark; storeSaveFlags(); drawQuick();
      } else if(hitRect(tp.x, tp.y, 20, y + 30, SCR_W - 40, 30)){
        gGlass = !gGlass; storeSaveFlags(); drawQuick();
      } else if(hitRect(tp.x, tp.y, 20, y + 64, SCR_W - 40, 30)){
        closeQuick(); shellOpenApp(APP_LIGHT, SCR_W / 2 - 26, 200, 52);
      } else if(hitRect(tp.x, tp.y, 20, y + 98, SCR_W - 40, 30)){
        closeQuick(); shellGoLock();
      } else if(tp.y > QS_Y + QS_H + 16){
        closeQuick();
      }
    } else if(tp.swipeUp){
      closeQuick();
    }
    return;
  }
  if(tp.swipeDown && tp.startY < 60 && !edit){
    qsOpen = true;
    drawQuick();
    return;
  }

  // --- Barra de navegacion ---
  if(tp.tap){
    int8_t nav = uiNavHit(tp.x, tp.y);
    if(nav == 0 || nav == 1){
      if(edit){ edit = false; editSel = -1; drawGrid(); }
      return;
    }
    if(nav == 2) return;                 // no hay conmutador de tareas: una app a la vez
  }

  // --- Modo Edicion: mantener pulsado un icono ---
  if(tp.longPress){
    uint8_t slot, app, is; int16_t ix, iy;
    if(hitIcon(tp.x, tp.y, slot, app, ix, iy, is) && slot != 0xFF){
      edit = true;
      int8_t prev = editSel;
      editSel = (int8_t)slot;
      if(prev >= 0) drawSlot((uint8_t)prev);
      drawSlot(slot);
      touchConsume();
    }
    return;
  }

  if(!tp.tap) return;

  uint8_t slot, app, is; int16_t ix, iy;
  if(!hitIcon(tp.x, tp.y, slot, app, ix, iy, is)){
    if(edit){ edit = false; int8_t s = editSel; editSel = -1; if(s >= 0) drawSlot((uint8_t)s); }
    return;
  }

  if(edit){
    if(slot == 0xFF) return;                       // el dock no se reordena
    if(editSel >= 0 && editSel != (int8_t)slot){
      uint8_t t = gHomeOrder[editSel];
      gHomeOrder[editSel] = gHomeOrder[slot];
      gHomeOrder[slot] = t;
      storeSaveHomeOrder();
      uint8_t a = (uint8_t)editSel;
      editSel = -1;
      edit = false;
      drawSlot(a);
      drawSlot(slot);
    } else {
      edit = false;
      editSel = -1;
      drawSlot(slot);
    }
    return;
  }

  shellOpenApp(app, ix, iy, is);
}
