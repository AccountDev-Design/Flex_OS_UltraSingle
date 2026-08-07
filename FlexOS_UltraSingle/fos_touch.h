// #############################################################
// ##  Flex OS UltraSingle  -  fos_touch.h
// ##  Tactil resistivo del shield MAR3501 + gestos
// #############################################################
#ifndef FOS_TOUCH_H
#define FOS_TOUCH_H

#include <Arduino.h>
#include "fos_config.h"

// Estado tactil de alto nivel. Mismo contrato que el struct Touch de
// Flex OS Ultra, para que la logica de las pantallas sea la misma.
//   down      -> hay un dedo en la pantalla AHORA
//   pressed   -> flanco de bajada (este frame empezo el toque)
//   released  -> flanco de subida
//   tap       -> toque corto y sin arrastre (se evalua en released)
//   longPress -> se mantuvo pulsado mas de GEST_LONG_MS sin moverse
//   swipe*    -> deslizamiento reconocido (se evalua en released)
struct FTouch {
  bool     down, pressed, released, tap, moved, longPress;
  bool     swipeUp, swipeDown, swipeLeft, swipeRight;
  int16_t  x, y, startX, startY, dx, dy;
  uint32_t downMs;
};

extern FTouch tp;

void touchInit();
void touchPoll();
// Descarta el gesto en curso (tras cambiar de pantalla, para que el mismo
// toque no active tambien un control de la pantalla nueva).
void touchConsume();

// Instante del ultimo contacto: lo usa el bloqueo automatico.
uint32_t touchLastActivity();

// Impacto de un punto contra un rectangulo.
bool hitRect(int16_t px, int16_t py, int16_t x, int16_t y, int16_t w, int16_t h);

#endif // FOS_TOUCH_H
