// #############################################################
// ##  Flex OS UltraSingle  -  fos_str.h
// ##  Textos del sistema (todos en PROGMEM)
// #############################################################
//
//  Flex OS Ultra llevaba 5 idiomas. En 256 KB de Flash eso son
//  kilobytes que hacen falta para las apps, asi que UltraSingle
//  conserva los dos idiomas del selector inicial (espanol e ingles)
//  con la misma pantalla de eleccion y el mismo ajuste.
//
//  NINGUNA cadena vive en SRAM: la tabla es const __FlashStringHelper
//  y se lee con pgm_read_word. La fuente es CP437, asi que los
//  acentos se escriben con su codigo (\xA0 = a acentuada, etc.).
// #############################################################
#ifndef FOS_STR_H
#define FOS_STR_H

#include <Arduino.h>

enum { LANG_ES = 0, LANG_EN, LANG_N };
extern uint8_t gLang;

enum {
  S_CONTINUE = 0, S_LANGUAGE, S_WELCOME, S_SWIPEUNLOCK, S_WEATHER, S_NEWS,
  S_NONEWS, S_NOEVENTS, S_SETTINGS, S_DISPLAY, S_DARKMODE, S_GLASS,
  S_ICONSTYLE, S_LOCKSCREEN, S_PIN, S_AUTOLOCK, S_DATETIME, S_H24, S_ABOUT,
  S_ON, S_OFF, S_BACK, S_SAVE, S_CLEAR, S_DELETE, S_OK, S_CANCEL,
  S_ENTERPIN, S_WRONGPIN, S_SETPIN, S_NOPIN, S_FLAT, S_GLASSY, S_FREE,
  S_USED, S_UPTIME, S_TOUCHES, S_RAM, S_FLASH, S_EEPROM, S_BOARD, S_PANEL,
  S_VERSION, S_EMPTY, S_COLOR, S_BRUSH, S_NEWGAME, S_SCORE, S_BEST,
  S_GAMEOVER, S_TAPSTART, S_START, S_STOP, S_RESET, S_HOUR, S_MINUTE,
  S_DAY, S_MONTH, S_YEAR, S_APPS, S_SYSTEM, S_NEVER, S_TOTAL, S_NOTEHINT,
  S_N
};

// Puntero PROGMEM a la cadena 'id' en el idioma activo.
PGM_P strP(uint8_t id);
// Nombre de la aplicacion 'app' en el idioma activo.
PGM_P appNameP(uint8_t app);
// Dias y meses localizados.
PGM_P dayShortP(uint8_t d);      // 0 = domingo
PGM_P dayFullP(uint8_t d);
PGM_P monthShortP(uint8_t m);    // 0 = enero
PGM_P monthFullP(uint8_t m);

#endif // FOS_STR_H
