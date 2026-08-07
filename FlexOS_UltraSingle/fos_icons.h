// #############################################################
// ##  Flex OS UltraSingle  -  fos_icons.h
// ##  Iconos vectoriales del sistema
// #############################################################
//
//  Los mismos iconos de Flex OS Ultra, redibujados con aritmetica
//  ENTERA: la version P4 usaba cosf/sinf y flotantes en cada
//  icono, y en AVR eso arrastra la biblioteca matematica entera
//  (varios KB de Flash) ademas de ser lentisimo. Las posiciones
//  circulares salen ahora de una tabla de 8 direcciones en PROGMEM.
// #############################################################
#ifndef FOS_ICONS_H
#define FOS_ICONS_H

#include <Arduino.h>

// Icono de aplicacion completo (fondo + simbolo), lado S.
void drawAppIcon(uint8_t id, int16_t x, int16_t y, uint8_t S);

// Iconos de la barra de estado / navegacion.
void drawBattery(int16_t x, int16_t y, uint8_t w, uint8_t h, uint8_t pct, uint16_t col);
void drawSignal(int16_t x, int16_t yBase, uint8_t h, uint16_t col);
void drawNavBar(int16_t y, uint16_t col);
void drawHomeIndicator(int16_t yBottom, uint16_t col);

#endif // FOS_ICONS_H
