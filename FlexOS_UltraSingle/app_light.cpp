// #############################################################
// ##  Flex OS UltraSingle  -  app_light.cpp   (Linterna)
// #############################################################
//
//  El shield MAR3501 lleva la retroiluminacion cableada fija: no
//  hay PWM que regular. Asi que el "brillo" se consigue con el
//  propio panel, pintandolo de blanco o de gris -- que es lo que
//  de verdad cambia la luz que sale de la pantalla.
//
//  Es la unica app a pantalla completa (AF_FULLSCREEN): oculta las
//  barras del sistema y se sale tocando la franja inferior.
// #############################################################
#include "fos_apps.h"
#include "fos_gfx.h"
#include "fos_ui.h"
#include "fos_shell.h"
#include "fos_touch.h"

static const uint8_t LEVELS[3] PROGMEM = { 255, 170, 90 };
static uint8_t level = 0;

static const char LT_HINT[] PROGMEM = "Toca abajo para salir";

static void paint(){
  uint8_t v = pgm_read_byte(&LEVELS[level]);
  tft.fillScreen(RGB(v, v, v));
  gfxTextCP(SCR_W / 2, SCR_H - 30, LT_HINT, 1, RGB(v / 2, v / 2, v / 2));
  // Tres puntos que indican el nivel activo.
  for(uint8_t i = 0; i < 3; i++)
    tft.fillCircle(SCR_W / 2 - 16 + i * 16, SCR_H - 52, 4,
                   i == level ? RGB(60,130,246) : RGB(v * 3 / 4, v * 3 / 4, v * 3 / 4));
}

void appLightEnter(){
  level = 0;
  paint();
}

void appLightTick(){
  if(!tp.tap) return;
  if(tp.y > SCR_H - 70){ shellGoHome(); return; }
  level = (uint8_t)((level + 1) % 3);
  paint();
}
