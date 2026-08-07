// #############################################################
// ##  Flex OS UltraSingle  -  fos_gfx.cpp
// ##  Motor grafico directo sobre ILI9486 (8 bits paralelo)
// #############################################################
#include "fos_gfx.h"
#include <string.h>

MCUFRIEND_kbv tft;
bool gDark = true;                       // Modo oscuro por defecto (como Flex OS Ultra)

// UNICO buffer grande del sistema: una linea de barrido. Se reutiliza para el
// wallpaper y para todos los paneles de vidrio, asi que el coste es fijo (640 B)
// y no crece con el numero de superficies en pantalla.
static uint16_t sline[SCR_W];

// -------------------------------------------------------------
//  Color
// -------------------------------------------------------------
// Division por 255 sin instruccion de division (el ATmega no tiene divisor
// hardware): v/255 == (v + 1 + (v>>8)) >> 8 para v en [0, 65534].
#define DIV255(v) (uint16_t)(((v) + 1u + ((v) >> 8)) >> 8)

uint16_t mix565(uint16_t a, uint16_t b, uint8_t t){
  uint16_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  uint16_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  uint16_t it = 255u - t;
  return (uint16_t)((DIV255((uint16_t)(ar * it + br * t)) << 11) |
                    (DIV255((uint16_t)(ag * it + bg * t)) << 5)  |
                     DIV255((uint16_t)(ab * it + bb * t)));
}

uint8_t isqrt16(uint16_t v){
  uint32_t op = v, res = 0, one = 1UL << 16;
  while(one > op) one >>= 2;
  while(one){
    if(op >= res + one){ op -= res + one; res += one << 1; }
    res >>= 1; one >>= 2;
  }
  return (uint8_t)res;
}

// -------------------------------------------------------------
//  Wallpaper
// -------------------------------------------------------------
//  Degradado diagonal de 3 paradas identico al de Flex OS Ultra:
//  violeta (abajo-izquierda) -> azul (centro) -> verde (arriba-derecha).
static inline uint16_t gradAt(uint8_t t){
  return (t < 128) ? mix565(WALL_PURPLE, WALL_BLUE,  (uint8_t)(t << 1))
                   : mix565(WALL_BLUE,   WALL_GREEN, (uint8_t)((t - 128) << 1));
}

// Paso del degradado horizontal en punto fijo 8.8: el indice avanza
// 255/(SCR_W-1) por pixel. Se acumula en 16 bits en vez de dividir en cada
// punto -- 320*WALL_STEP no llega a desbordar -- y de paso evita el error
// real que tenia la version directa: 319*255 se sale de un uint16_t.
#define WALL_STEP ((uint16_t)((255UL << 8) / (SCR_W - 1)))

uint16_t wallAt(int16_t x, int16_t y){
  uint8_t tx = (uint8_t)(((uint32_t)x * 255UL) / (SCR_W - 1));
  uint8_t ty = (uint8_t)(((uint32_t)(SCR_H - 1 - y) * 255UL) / (SCR_H - 1));
  return gradAt((uint8_t)(((uint16_t)tx + ty) >> 1));
}

// Rellena sline[0..w-1] con la fila 'y' del wallpaper a partir de x0.
// Los 320 pixeles de una fila solo contienen ~128 colores distintos (el indice
// del degradado avanza 1 cada 2,5 px), asi que se detecta la RACHA y mix565
// se llama una de cada dos o tres veces en vez de una por pixel.
static void wallRow(int16_t y, int16_t x0, int16_t w){
  uint16_t ty  = (uint16_t)(((uint32_t)(SCR_H - 1 - y) * 255UL) / (SCR_H - 1));
  uint16_t txq = (uint16_t)x0 * WALL_STEP;
  int16_t  last = -1;
  uint16_t c = 0;
  for(int16_t i = 0; i < w; i++){
    uint8_t t = (uint8_t)(((txq >> 8) + ty) >> 1);
    txq += WALL_STEP;
    if(t != last){ last = t; c = gradAt(t); }
    sline[i] = c;
  }
}

// Recorta un rectangulo a la pantalla. Devuelve false si queda vacio.
static bool clipRect(int16_t &x, int16_t &y, int16_t &w, int16_t &h){
  if(x < 0){ w += x; x = 0; }
  if(y < 0){ h += y; y = 0; }
  if(x + w > SCR_W) w = SCR_W - x;
  if(y + h > SCR_H) h = SCR_H - y;
  return (w > 0 && h > 0);
}

void gfxWallRect(int16_t x, int16_t y, int16_t w, int16_t h){
  if(!clipRect(x, y, w, h)) return;
  tft.setAddrWindow(x, y, x + w - 1, y + h - 1);
  for(int16_t j = 0; j < h; j++){
    wallRow(y + j, x, w);
    tft.pushColors(sline, w, j == 0);
  }
}

void gfxWallpaper(){ gfxWallRect(0, 0, SCR_W, SCR_H); }

// -------------------------------------------------------------
//  Superficies translucidas
// -------------------------------------------------------------
// Sangrado horizontal de la fila 'j' de un rectangulo redondeado.
static uint8_t rrInset(int16_t j, int16_t h, int16_t r){
  int16_t dy;
  if(j < r)            dy = r - 1 - j;
  else if(j >= h - r)  dy = j - (h - r);
  else                 return 0;
  uint16_t rr = (uint16_t)r * r, dd = (uint16_t)dy * dy;
  return (uint8_t)(r - isqrt16(rr > dd ? (uint16_t)(rr - dd) : 0));
}

static void surface(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                    uint16_t tint, uint8_t alpha, bool overWall, uint16_t bg){
  if(!clipRect(x, y, w, h)) return;
  if(2 * r > w) r = w / 2;
  if(2 * r > h) r = h / 2;
  if(r < 0) r = 0;
  tft.setAddrWindow(x, y, x + w - 1, y + h - 1);
  for(int16_t j = 0; j < h; j++){
    if(overWall) wallRow(y + j, x, w);
    else         for(int16_t i = 0; i < w; i++) sline[i] = bg;
    uint8_t ins = rrInset(j, h, r);
    // Un panel de vidrio no es un tinte plano: Flex OS Ultra le da un
    // gradiente de grosor (mas denso arriba). Se reproduce variando alpha
    // un 15% a lo largo del alto, que es lo que se percibe en pantalla.
    uint16_t a = (uint16_t)alpha - (uint16_t)((uint32_t)alpha * 15 * j) / (100u * (h ? h : 1));
    for(int16_t i = ins; i < w - ins; i++) sline[i] = mix565(sline[i], tint, (uint8_t)a);
    tft.pushColors(sline, w, j == 0);
  }
}

void glassRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
               uint16_t tint, uint8_t alpha){
  surface(x, y, w, h, r, tint, alpha, true, 0);
}

void tintRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
              uint16_t tint, uint8_t alpha, uint16_t bg){
  surface(x, y, w, h, r, tint, alpha, false, bg);
}

// -------------------------------------------------------------
//  Texto
// -------------------------------------------------------------
int gfxTextW(const char* s, uint8_t size){
  uint8_t n = (uint8_t)strlen(s);
  return n ? (int)n * TXT_ADV(size) - size : 0;
}
int gfxTextWP(PGM_P s, uint8_t size){
  uint8_t n = (uint8_t)strlen_P(s);
  return n ? (int)n * TXT_ADV(size) - size : 0;
}

int gfxText(int16_t x, int16_t y, const char* s, uint8_t size, uint16_t col){
  tft.setTextColor(col);
  tft.setTextSize(size);
  tft.setCursor(x, y);
  tft.print(s);
  return x + gfxTextW(s, size);
}
int gfxTextP(int16_t x, int16_t y, PGM_P s, uint8_t size, uint16_t col){
  tft.setTextColor(col);
  tft.setTextSize(size);
  tft.setCursor(x, y);
  tft.print((const __FlashStringHelper*)s);
  return x + gfxTextWP(s, size);
}
void gfxTextC(int16_t cx, int16_t y, const char* s, uint8_t size, uint16_t col){
  gfxText(cx - gfxTextW(s, size) / 2, y, s, size, col);
}
void gfxTextCP(int16_t cx, int16_t y, PGM_P s, uint8_t size, uint16_t col){
  gfxTextP(cx - gfxTextWP(s, size) / 2, y, s, size, col);
}
void gfxTextR(int16_t rx, int16_t y, const char* s, uint8_t size, uint16_t col){
  gfxText(rx - gfxTextW(s, size), y, s, size, col);
}

// -------------------------------------------------------------
//  Trazos
// -------------------------------------------------------------
void gfxThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t tk, uint16_t col){
  if(tk < 1) tk = 1;
  int16_t dx = x1 - x0, dy = y1 - y0;
  // Desplaza el trazo perpendicularmente: para lineas mas horizontales que
  // verticales se apila en Y, y al reves. Evita trigonometria en coma flotante.
  bool horiz = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
  int8_t half = (int8_t)(tk / 2);
  for(int8_t k = -half; k <= half - (int8_t)((tk & 1) ? 0 : 1); k++){
    if(horiz) tft.drawLine(x0, y0 + k, x1, y1 + k, col);
    else      tft.drawLine(x0 + k, y0, x1 + k, y1, col);
  }
}

void gfxRing(int16_t cx, int16_t cy, int16_t r, uint8_t tk, uint16_t col){
  for(uint8_t k = 0; k < tk; k++) tft.drawCircle(cx, cy, r - k, col);
}

void gfxChevron(int16_t x, int16_t y, uint8_t s, uint16_t col){
  gfxThickLine(x, y - s, x + s, y, 2, col);
  gfxThickLine(x + s, y, x, y + s, 2, col);
}

// -------------------------------------------------------------
//  Reloj grande vectorial (7 segmentos con extremos redondeados)
// -------------------------------------------------------------
//  bit0=A(arriba) 1=B(sup-dcha) 2=C(inf-dcha) 3=D(abajo)
//  bit4=E(inf-izq) 5=F(sup-izq)  6=G(centro)
static const uint8_t SEG7[10] PROGMEM = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static void seg7(int16_t x, int16_t y, uint8_t dw, uint8_t tk, uint8_t mask, uint16_t col){
  int16_t dh = dw * 2, hh = dh / 2, hr = tk / 2;
  int16_t bw = dw - tk, bh = hh - tk;
  if(bw < 1) bw = 1;
  if(bh < 1) bh = 1;
  if(mask & 0x01) tft.fillRoundRect(x + hr,          y,               bw, tk, hr, col);
  if(mask & 0x40) tft.fillRoundRect(x + hr,          y + hh - hr,     bw, tk, hr, col);
  if(mask & 0x08) tft.fillRoundRect(x + hr,          y + dh - tk,     bw, tk, hr, col);
  if(mask & 0x20) tft.fillRoundRect(x,               y + hr,          tk, bh, hr, col);
  if(mask & 0x02) tft.fillRoundRect(x + dw - tk,     y + hr,          tk, bh, hr, col);
  if(mask & 0x10) tft.fillRoundRect(x,               y + hh,          tk, bh, hr, col);
  if(mask & 0x04) tft.fillRoundRect(x + dw - tk,     y + hh,          tk, bh, hr, col);
}

int gfxBigClockW(const char* txt, uint8_t dw){
  int w = 0;
  for(const char* p = txt; *p; p++) w += (*p == ':') ? (dw / 2 + 4) : (dw + 6);
  return w > 0 ? w - 6 : 0;
}

void gfxBigClock(const char* txt, int16_t cx, int16_t cy, uint8_t dw, uint8_t tk, uint16_t col){
  int16_t dh = dw * 2;
  int16_t x  = cx - gfxBigClockW(txt, dw) / 2;
  int16_t y  = cy - dh / 2;
  for(const char* p = txt; *p; p++){
    if(*p == ':'){
      int16_t r = tk / 2;
      tft.fillCircle(x + dw / 4, y + dh / 3,     r, col);
      tft.fillCircle(x + dw / 4, y + 2 * dh / 3, r, col);
      x += dw / 2 + 4;
    } else if(*p >= '0' && *p <= '9'){
      seg7(x, y, dw, tk, pgm_read_byte(&SEG7[*p - '0']), col);
      x += dw + 6;
    } else {
      x += dw / 2 + 4;
    }
  }
}

// -------------------------------------------------------------
//  Formateo numerico (sin String, sin printf flotante)
// -------------------------------------------------------------
char* fmtU16(char* dst, uint16_t v){
  char tmp[6];
  uint8_t n = 0;
  do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while(v);
  while(n) *dst++ = tmp[--n];
  *dst = 0;
  return dst;
}

char* fmtPad2(char* dst, uint8_t v){
  dst[0] = (char)('0' + (v / 10) % 10);
  dst[1] = (char)('0' + (v % 10));
  dst[2] = 0;
  return dst + 2;
}

void fmtClock(char* dst, uint8_t h, uint8_t m, bool h24){
  if(!h24){
    uint8_t hh = h % 12;
    if(hh == 0) hh = 12;
    dst = fmtU16(dst, hh);
  } else {
    dst = fmtPad2(dst, h);
  }
  *dst++ = ':';
  fmtPad2(dst, m);
}

// -------------------------------------------------------------
//  Arranque del panel
// -------------------------------------------------------------
bool gfxInit(){
  uint16_t id = tft.readID();
  // El MAR3501 se identifica como ILI9486. Algunos lotes devuelven 0x0000 o
  // 0xD3D3 por el pin de lectura: en ese caso se fuerza el driver correcto en
  // vez de rendirse, que es lo que hacia fallar el arranque en frio.
  if(id == 0x0000 || id == 0xFFFF || id == 0xD3D3) id = 0x9486;
  tft.begin(id);
  tft.setRotation(0);                    // retrato 320x480
  tft.fillScreen(C_BLACK);
  return true;
}
