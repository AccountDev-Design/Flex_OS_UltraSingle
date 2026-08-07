// #############################################################
// ##  Flex OS UltraSingle  -  app_games.cpp   (Juegos)
// #############################################################
//
//  Flex OS Ultra traia dos juegos con motor propio (plataformas y
//  futbol) que ocupaban miles de lineas y decenas de KB de datos.
//  Nada de eso entra en 256 KB de Flash junto al resto del sistema,
//  y los modos por red desaparecen con la radio.
//
//  UltraSingle conserva la app "Juegos" con un clasico que si cabe
//  y se juega bien con tactil resistivo: serpiente, con el record
//  guardado en EEPROM. Se controla deslizando, no con botones, para
//  no robarle espacio al tablero.
// #############################################################
#include "fos_apps.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_str.h"
#include "fos_store.h"
#include "fos_touch.h"

#define G_CELL 20
#define G_COLS 16
#define G_Y0   84
#define G_ROWS 17
#define G_MAX  80
#define G_MS   220UL

static uint8_t  sx[G_MAX], sy[G_MAX];
static uint8_t  slen;
static int8_t   dx, dy;
static uint8_t  fx, fy;
static uint16_t score;
static bool     alive;
static uint32_t lastStep;

static void cellFill(uint8_t c, uint8_t r, uint16_t col, uint8_t inset){
  tft.fillRoundRect(c * G_CELL + inset, G_Y0 + r * G_CELL + inset,
                    G_CELL - 2 * inset, G_CELL - 2 * inset, 4, col);
}

static void drawHud(){
  tft.fillRect(0, APP_HDR_H, SCR_W, G_Y0 - APP_HDR_H, PAGE_BG);
  char b[8];
  int16_t x = gfxTextP(12, 56, strP(S_SCORE), 1, TXT_LO);
  fmtU16(b, score);
  gfxText(x + 8, 52, b, 2, TXT_HI);
  x = gfxTextP(SCR_W - 110, 56, strP(S_BEST), 1, TXT_LO);
  fmtU16(b, gBest);
  gfxText(x + 8, 52, b, 2, C_ACCENT);
}

static void placeFood(){
  // Busca una celda libre a partir de una posicion pseudoaleatoria: sin
  // bucle "while(ocupada)" que pudiera no terminar con el tablero lleno.
  uint16_t start = (uint16_t)(random(G_COLS * G_ROWS));
  for(uint16_t k = 0; k < (uint16_t)(G_COLS * G_ROWS); k++){
    uint16_t p = (start + k) % (G_COLS * G_ROWS);
    uint8_t c = (uint8_t)(p % G_COLS), r = (uint8_t)(p / G_COLS);
    bool busy = false;
    for(uint8_t i = 0; i < slen; i++) if(sx[i] == c && sy[i] == r){ busy = true; break; }
    if(!busy){ fx = c; fy = r; return; }
  }
}

static void newGame(){
  slen = 3;
  for(uint8_t i = 0; i < slen; i++){ sx[i] = 8 - i; sy[i] = 8; }
  dx = 1; dy = 0;
  score = 0;
  alive = true;
  lastStep = millis();
  tft.fillRect(0, G_Y0, SCR_W, G_ROWS * G_CELL, gDark ? RGB(20,22,30) : RGB(232,236,244));
  placeFood();
  cellFill(fx, fy, C_DANGER, 3);
  for(uint8_t i = 0; i < slen; i++) cellFill(sx[i], sy[i], i ? C_OK : C_ACCENT, 1);
  drawHud();
}

static void gameOver(){
  alive = false;
  if(score > gBest){ gBest = score; storeSaveBest(); }
  int16_t y = G_Y0 + (G_ROWS * G_CELL) / 2 - 30;
  tft.fillRoundRect(30, y, SCR_W - 60, 60, 14, CARD_BG);
  gfxTextCP(SCR_W / 2, y + 12, strP(S_GAMEOVER), 2, TXT_HI);
  gfxTextCP(SCR_W / 2, y + 38, strP(S_TAPSTART), 1, TXT_LO);
  drawHud();
}

static void step(){
  int8_t nc = (int8_t)sx[0] + dx;
  int8_t nr = (int8_t)sy[0] + dy;
  if(nc < 0 || nc >= G_COLS || nr < 0 || nr >= G_ROWS){ gameOver(); return; }
  for(uint8_t i = 0; i < slen; i++)
    if(sx[i] == (uint8_t)nc && sy[i] == (uint8_t)nr){ gameOver(); return; }

  bool grew = ((uint8_t)nc == fx && (uint8_t)nr == fy);
  if(!grew){
    // Borra la cola antes de desplazar el cuerpo.
    cellFill(sx[slen - 1], sy[slen - 1], gDark ? RGB(20,22,30) : RGB(232,236,244), 0);
  } else if(slen < G_MAX){
    slen++;
  }
  for(uint8_t i = slen - 1; i > 0; i--){ sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
  sx[0] = (uint8_t)nc; sy[0] = (uint8_t)nr;
  cellFill(sx[1], sy[1], C_OK, 1);
  cellFill(sx[0], sy[0], C_ACCENT, 1);
  if(grew){
    score++;
    drawHud();
    placeFood();
    cellFill(fx, fy, C_DANGER, 3);
  }
}

void appGamesEnter(){
  uiAppScreen(appNameP(APP_GAMES));
  randomSeed(millis() ^ (uint32_t)analogRead(A5));
  newGame();
}

void appGamesTick(){
  if(!alive){
    if(tp.tap && tp.y > G_Y0) newGame();
    return;
  }
  if(tp.swipeLeft  && dx == 0){ dx = -1; dy = 0; }
  else if(tp.swipeRight && dx == 0){ dx = 1; dy = 0; }
  else if(tp.swipeUp    && dy == 0){ dx = 0; dy = -1; }
  else if(tp.swipeDown  && dy == 0){ dx = 0; dy = 1; }

  if(millis() - lastStep < G_MS) return;
  lastStep += G_MS;
  step();
}
