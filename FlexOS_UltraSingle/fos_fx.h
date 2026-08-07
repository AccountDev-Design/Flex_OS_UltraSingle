// #############################################################
// ##  Flex OS UltraSingle  -  fos_fx.h
// ##  Transiciones del sistema
// #############################################################
//
//  Flex OS Ultra animaba componiendo framebuffers en PSRAM (el
//  desbloqueo con fisica, el zoom de apertura de app, la cortina).
//  Sin memoria para eso, UltraSingle conserva la SENSACION con
//  transiciones que se dibujan directamente sobre la GRAM: un
//  barrido de wallpaper y una apertura por escalado de rectangulo.
//  Son pocos pasos a proposito -- el bus de 8 bits del Mega no da
//  para mas -- pero el sistema deja de "saltar" entre pantallas.
// #############################################################
#ifndef FOS_FX_H
#define FOS_FX_H

#include <Arduino.h>

// Barrido de wallpaper: limpia la pantalla en bandas. up = de abajo a arriba.
void fxWipe(bool up);
// Apertura de aplicacion: un rectangulo redondeado crece desde el icono
// hasta ocupar la pantalla, como en Flex OS Ultra.
void fxOpenApp(int16_t ix, int16_t iy, uint8_t is);
// Cierre: el fondo vuelve desde los bordes hacia el centro.
void fxCloseApp();

#endif // FOS_FX_H
