// #############################################################
// ##  Flex OS UltraSingle  -  fos_icons.cpp
// #############################################################
#include "fos_icons.h"
#include "fos_gfx.h"
#include "fos_store.h"
#include "fos_time.h"
#include "fos_apps.h"

// Porcentaje de S, entero.
#define P(v, pc) (int16_t)(((int32_t)(v) * (pc)) / 100)

// cos/sin x100 en 8 direcciones (0, 45, 90 ... 315 grados). Sustituye a
// cosf/sinf: mismas posiciones, sin biblioteca matematica.
static const int8_t DIR8[8][2] PROGMEM = {
  { 100, 0 }, { 71, 71 }, { 0, 100 }, { -71, 71 },
  { -100, 0 }, { -71, -71 }, { 0, -100 }, { 71, -71 }
};

static void iconBase(int16_t x, int16_t y, uint8_t S, uint16_t bg){
  int16_t r = P(S, 24);
  if(gIconGlass){
    // Estilo "Vidrio": el fondo del icono deja ver el wallpaper.
    glassRect(x, y, S, S, r, bg, 190);
  } else {
    tft.fillRoundRect(x, y, S, S, r, bg);
    // Brillo superior sutil (alpha 22 sobre el propio fondo del icono).
    tft.fillRoundRect(x, y, S, S / 2, r, mix565(bg, C_WHITE, 22));
  }
}

void drawAppIcon(uint8_t id, int16_t x, int16_t y, uint8_t S){
  int16_t cx = x + S / 2, cy = y + S / 2;
  uint8_t tk = S / 12; if(tk < 2) tk = 2;
  const uint16_t W = C_WHITE;

  switch(id){
    case APP_CLOCK: {
      iconBase(x, y, S, RGB(245,245,247));
      uint16_t ink = RGB(70,70,74);
      gfxRing(cx, cy, P(S,36), 2, ink);
      tft.fillRect(cx - 1, y + P(S,16), 2, S/12, ink);
      tft.fillRect(cx - 1, y + S - P(S,16) - S/12, 2, S/12, ink);
      tft.fillRect(x + P(S,16), cy - 1, S/12, 2, ink);
      tft.fillRect(x + S - P(S,16) - S/12, cy - 1, S/12, 2, ink);
      gfxThickLine(cx, cy, cx - P(S,14), cy - P(S,10), tk/2 + 1, RGB(30,30,30));
      gfxThickLine(cx, cy, cx + P(S,12), cy - P(S,20), tk/2 + 1, RGB(245,140,30));
      tft.fillCircle(cx, cy, tk/2 + 1, RGB(30,30,30));
    } break;

    case APP_CALENDAR: {
      iconBase(x, y, S, W);
      tft.fillRect(x + P(S,16), y + P(S,18), P(S,68), P(S,16), RGB(232,70,70));
      char d[4]; fmtU16(d, gTime.day);
      gfxTextC(cx, y + P(S,42), d, S >= 44 ? 3 : 2, RGB(60,60,64));
    } break;

    case APP_CALC: {
      iconBase(x, y, S, RGB(58,58,60));
      tft.fillRoundRect(x + P(S,18), y + P(S,16), P(S,64), P(S,16), 3, RGB(210,210,215));
      for(uint8_t r = 0; r < 3; r++) for(uint8_t c = 0; c < 4; c++)
        tft.fillRoundRect(x + P(S,18) + c * P(S,17), y + P(S,40) + r * P(S,15),
                          P(S,12), P(S,10), 2,
                          (c == 3) ? RGB(245,150,30) : RGB(150,150,155));
    } break;

    case APP_NOTES: {
      iconBase(x, y, S, RGB(232,167,90));
      tft.fillRoundRect(x + P(S,22), y + P(S,18), P(S,48), P(S,62), 4, W);
      for(uint8_t i = 0; i < 3; i++)
        tft.fillRect(x + P(S,30), y + P(S,30) + i * P(S,12), P(S,32), 2, RGB(180,180,185));
      gfxThickLine(x + P(S,56), y + P(S,66), x + P(S,78), y + P(S,40), tk/2 + 1, RGB(120,90,40));
      tft.fillCircle(x + P(S,78), y + P(S,40), tk/2 + 1, RGB(245,210,90));
    } break;

    case APP_PAINT: {
      iconBase(x, y, S, RGB(241,231,210));
      tft.fillCircle(cx - P(S,4), cy + P(S,2), P(S,27), RGB(236,226,205));
      tft.fillCircle(cx + P(S,11), cy + P(S,11), P(S,6), RGB(241,231,210));
      tft.fillCircle(cx - P(S,14), cy - P(S,6),  P(S,5), RGB(230,70,70));
      tft.fillCircle(cx - P(S,2),  cy - P(S,13), P(S,5), RGB(240,200,60));
      tft.fillCircle(cx + P(S,10), cy - P(S,7),  P(S,5), RGB(70,130,235));
      tft.fillCircle(cx - P(S,16), cy + P(S,9),  P(S,5), RGB(80,190,90));
      gfxThickLine(cx + P(S,2), cy - P(S,16), cx + P(S,26), cy - P(S,30), tk/2 + 1, RGB(140,100,60));
    } break;

    case APP_GAMES: {
      iconBase(x, y, S, RGB(142,30,30));
      tft.drawRoundRect(x + P(S,14), y + P(S,34), P(S,72), P(S,30), P(S,13), W);
      tft.drawRoundRect(x + P(S,14) + 1, y + P(S,34) + 1, P(S,72) - 2, P(S,30) - 2, P(S,12), W);
      tft.fillRect(cx - P(S,22) - 1, cy + P(S,2), P(S,12), 3, W);
      tft.fillRect(cx - P(S,16) - 1, cy - P(S,4), 3, P(S,12), W);
      tft.fillCircle(cx + P(S,14), cy - P(S,1), 3, W);
      tft.fillCircle(cx + P(S,22), cy + P(S,5), 3, W);
    } break;

    case APP_TIMER: {
      iconBase(x, y, S, RGB(40,44,56));
      tft.fillRect(cx - P(S,8), y + P(S,12), P(S,16), P(S,7), RGB(230,235,245));
      gfxRing(cx, cy + P(S,4), P(S,32), 3, RGB(230,235,245));
      gfxThickLine(cx, cy + P(S,4), cx + P(S,16), cy - P(S,12), tk/2 + 1, RGB(245,150,30));
      tft.fillCircle(cx, cy + P(S,4), tk/2 + 1, RGB(230,235,245));
    } break;

    case APP_LIGHT: {
      iconBase(x, y, S, RGB(70,76,92));
      uint16_t lamp = RGB(232,236,246);
      tft.fillRoundRect(cx - P(S,10), y + P(S,20), P(S,20), P(S,14), 3, lamp);
      tft.fillRoundRect(cx - P(S,7),  y + P(S,32), P(S,14), P(S,34), 3, RGB(180,188,205));
      tft.fillTriangle(cx - P(S,22), y + P(S,16), cx + P(S,22), y + P(S,16),
                       cx, y + P(S,2), RGB(250,220,110));
    } break;

    case APP_WELL: {
      iconBase(x, y, S, RGB(92,193,90));
      gfxThickLine(cx - P(S,16), cy + P(S,2),  cx - P(S,2), cy + P(S,16), tk/2 + 2, W);
      gfxThickLine(cx - P(S,2),  cy + P(S,16), cx + P(S,20), cy - P(S,14), tk/2 + 2, W);
    } break;

    case APP_MEMORY: {
      iconBase(x, y, S, RGB(59,123,217));
      uint16_t fol = RGB(225,236,250);
      tft.fillRoundRect(x + P(S,20), y + P(S,28), P(S,30), P(S,12), 3, fol);
      tft.fillRoundRect(x + P(S,18), y + P(S,36), P(S,64), P(S,34), 4, fol);
      tft.fillRect(x + P(S,26), y + P(S,46), P(S,48), 3, RGB(140,175,225));
      tft.fillRect(x + P(S,26), y + P(S,54), P(S,30), 3, RGB(140,175,225));
    } break;

    case APP_SYSINFO: {
      iconBase(x, y, S, RGB(48,54,72));
      uint16_t chip = RGB(120,200,240);
      tft.fillRoundRect(x + P(S,26), y + P(S,26), P(S,48), P(S,48), 4, chip);
      tft.fillRoundRect(x + P(S,36), y + P(S,36), P(S,28), P(S,28), 2, RGB(30,36,50));
      for(uint8_t i = 0; i < 3; i++){
        int16_t d = P(S,34) + i * P(S,16);                     // patillas del chip
        tft.fillRect(x + d,        y + P(S,16), 3, P(S,10), chip);
        tft.fillRect(x + d,        y + P(S,74), 3, P(S,10), chip);
        tft.fillRect(x + P(S,16),  y + d,       P(S,10), 3, chip);
        tft.fillRect(x + P(S,74),  y + d,       P(S,10), 3, chip);
      }
    } break;

    case APP_SETTINGS:
    default: {
      iconBase(x, y, S, RGB(138,143,152));
      uint16_t g = RGB(70,74,84);
      tft.fillCircle(cx, cy, P(S,26), g);
      for(uint8_t k = 0; k < 8; k++){
        int8_t dx = (int8_t)pgm_read_byte(&DIR8[k][0]);
        int8_t dy = (int8_t)pgm_read_byte(&DIR8[k][1]);
        tft.fillCircle(cx + P(S,30) * dx / 100, cy + P(S,30) * dy / 100, P(S,8), g);
      }
      tft.fillCircle(cx, cy, P(S,10), RGB(138,143,152));
    } break;
  }
}

// -------------------------------------------------------------
//  Barra de estado / navegacion
// -------------------------------------------------------------
void drawBattery(int16_t x, int16_t y, uint8_t w, uint8_t h, uint8_t pct, uint16_t col){
  tft.drawRoundRect(x, y, w, h, 2, col);
  tft.fillRect(x + w, y + h / 3, 2, h / 3, col);
  uint8_t inner = (uint8_t)(((uint16_t)(w - 4) * pct) / 100);
  if(inner) tft.fillRect(x + 2, y + 2, inner, h - 4, col);
}

// El Mega no tiene radio: donde Flex OS Ultra pintaba el icono de Wi-Fi,
// UltraSingle pinta el indicador de actividad del sistema (tres barras que
// reflejan la SRAM libre). Mismo hueco, misma silueta, dato real.
void drawSignal(int16_t x, int16_t yBase, uint8_t h, uint16_t col){
  uint16_t fr = freeRam();
  uint8_t lit = fr > 3000 ? 3 : (fr > 1500 ? 2 : 1);
  for(uint8_t i = 0; i < 3; i++){
    uint8_t bh = (uint8_t)(h * (i + 1) / 3);
    uint16_t c = (i < lit) ? col : mix565(col, C_BLACK, 150);
    tft.fillRect(x + i * 4, yBase - bh, 3, bh, c);
  }
}

void drawHomeIndicator(int16_t yBottom, uint16_t col){
  int16_t bw = 90, bh = 4;
  tft.fillRoundRect((SCR_W - bw) / 2, yBottom - 12 - bh, bw, bh, 2, col);
}

// Botones clasicos (atras / inicio / recientes), como en Flex OS Ultra.
void drawNavBar(int16_t y, uint16_t col){
  int16_t bx = SCR_W / 6;
  tft.fillTriangle(bx - 8, y + 7, bx + 6, y - 1, bx + 6, y + 15, col);
  tft.drawCircle(SCR_W / 2, y + 7, 9, col);
  tft.drawCircle(SCR_W / 2, y + 7, 8, col);
  int16_t rx = SCR_W * 5 / 6;
  tft.drawRoundRect(rx - 9, y - 2, 18, 18, 3, col);
}
