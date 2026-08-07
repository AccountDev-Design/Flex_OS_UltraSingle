// #############################################################
// ##  Flex OS UltraSingle  -  fos_gfx.h
// ##  Motor grafico (dibujo DIRECTO sobre el ILI9486)
// #############################################################
//
//  DIFERENCIA CLAVE CON FLEX OS ULTRA
//  ----------------------------------
//  En el ESP32-P4 habia framebuffers completos en PSRAM (768 KB
//  cada uno) y un "presenter" que subia las filas sucias al panel.
//  En el Mega no hay memoria para un solo pixel de mas: 320x480x2
//  son 300 KB y aqui hay 8 KB de SRAM en total.
//
//  Por eso todo se dibuja DIRECTAMENTE en la GRAM del ILI9486. El
//  unico buffer del sistema es una LINEA de barrido (640 B) que se
//  usa para los degradados y los paneles de vidrio; sin ella habria
//  que hacer una transaccion de bus por pixel y el sistema se
//  arrastraria.
//
//  TRANSLUCIDEZ SIN LEER LA PANTALLA
//  ---------------------------------
//  El wallpaper de Flex OS Ultra es un degradado analitico: el color
//  de cualquier pixel se puede CALCULAR con wallAt(x, y) en vez de
//  leerlo del panel (leer la GRAM del ILI9486 es lentisimo). Como el
//  desenfoque de un degradado lineal es el propio degradado, el
//  "Liquid Glass" se reduce a mezclar el tinte con wallAt(): mismo
//  resultado en pantalla, coste cero en memoria.
// #############################################################
#ifndef FOS_GFX_H
#define FOS_GFX_H

#include <Arduino.h>
#include <MCUFRIEND_kbv.h>
#include "fos_config.h"
#include "fos_theme.h"

extern MCUFRIEND_kbv tft;

// ---------------- Ciclo de vida ----------------
bool gfxInit();

// ---------------- Color ----------------
uint16_t mix565(uint16_t a, uint16_t b, uint8_t t);
uint8_t  isqrt16(uint16_t v);

// ---------------- Wallpaper ----------------
uint16_t wallAt(int16_t x, int16_t y);
void     gfxWallpaper();                                   // todo el fondo
void     gfxWallRect(int16_t x, int16_t y, int16_t w, int16_t h);   // solo esa zona

// ---------------- Superficies ----------------
// Panel translucido SOBRE el wallpaper (Liquid Glass).
void glassRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
               uint16_t tint, uint8_t alpha);
// Panel translucido sobre un color plano conocido (dentro de una app).
void tintRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
              uint16_t tint, uint8_t alpha, uint16_t bg);

// ---------------- Texto (fuente 5x7 clasica, escalable) ----------------
#define TXT_ADV(sz)  (6 * (sz))
#define TXT_H(sz)    (7 * (sz))

int  gfxTextW(const char* s, uint8_t size);
int  gfxTextWP(PGM_P s, uint8_t size);
int  gfxText(int16_t x, int16_t y, const char* s, uint8_t size, uint16_t col);
int  gfxTextP(int16_t x, int16_t y, PGM_P s, uint8_t size, uint16_t col);
void gfxTextC(int16_t cx, int16_t y, const char* s, uint8_t size, uint16_t col);
void gfxTextCP(int16_t cx, int16_t y, PGM_P s, uint8_t size, uint16_t col);
void gfxTextR(int16_t rx, int16_t y, const char* s, uint8_t size, uint16_t col);

// ---------------- Utilidades de trazo ----------------
void gfxThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t tk, uint16_t col);
void gfxRing(int16_t cx, int16_t cy, int16_t r, uint8_t tk, uint16_t col);
void gfxChevron(int16_t x, int16_t y, uint8_t s, uint16_t col);   // ">" de lista

// Reloj grande vectorial (7 segmentos redondeados) de la pantalla de bloqueo.
// 'txt' admite digitos y ':'. 'dw' = ancho de digito, 'tk' = grosor de trazo.
void gfxBigClock(const char* txt, int16_t cx, int16_t cy, uint8_t dw, uint8_t tk, uint16_t col);
int  gfxBigClockW(const char* txt, uint8_t dw);

// ---------------- Numeros sin String ----------------
// Formateadores minimos: el sistema NUNCA usa String ni sprintf de coma
// flotante (cada uno arrastra kilobytes de Flash en AVR).
char* fmtU16(char* dst, uint16_t v);                       // devuelve fin
char* fmtPad2(char* dst, uint8_t v);                       // "07"
void  fmtClock(char* dst, uint8_t h, uint8_t m, bool h24); // "07:05" / "7:05"

#endif // FOS_GFX_H
