// #############################################################
// ##  Flex OS UltraSingle  -  fos_theme.h
// ##  Paleta del sistema (heredada de Flex OS Ultra)
// #############################################################
//
//  Los valores son EXACTAMENTE los de Flex OS Ultra: la identidad
//  visual no se toca al bajar de ESP32-P4 a Arduino Mega, solo la
//  forma de pintarlos. Todo son constantes de compilacion (RGB565
//  ya resuelto por el preprocesador), asi que no ocupan SRAM.
// #############################################################
#ifndef FOS_THEME_H
#define FOS_THEME_H

#include <Arduino.h>

// RGB888 -> RGB565 en tiempo de compilacion.
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Modo de apariencia. Vive en fos_gfx.cpp y lo cambia Ajustes.
extern bool gDark;

// ---------------- Wallpaper (degradado diagonal de 3 paradas) ----------------
#define WALL_GREEN   RGB(80, 224, 74)    // arriba-derecha
#define WALL_BLUE    RGB(40, 150, 245)   // centro
#define WALL_PURPLE  RGB(112, 46, 230)   // abajo-izquierda

// ---------------- Neutros ----------------
#define C_WHITE      RGB(255, 255, 255)
#define C_BLACK      RGB(0, 0, 0)
#define C_INK        RGB(20, 22, 30)

// Texto sobre el wallpaper (Inicio / Bloqueo): siempre claro.
#define C_ON_WALL    RGB(255, 255, 255)
#define C_ON_WALL_LO RGB(215, 224, 240)

// ---------------- Acento del sistema ----------------
#define C_ACCENT     RGB(60, 130, 246)
#define C_ACCENT_DK  RGB(28, 58, 120)
#define C_OK         RGB(80, 200, 120)
#define C_WARN       RGB(240, 170, 60)
#define C_DANGER     RGB(230, 80, 80)

// ---------------- Superficies de app (Ajustes, listas, ventanas) ----------------
#define PAGE_BG      (gDark ? RGB(18, 20, 28)    : RGB(244, 247, 251))
#define CARD_BG      (gDark ? RGB(34, 38, 50)    : RGB(255, 255, 255))
#define CARD_ALT     (gDark ? RGB(48, 54, 72)    : RGB(238, 242, 248))
#define TXT_HI       (gDark ? RGB(240, 242, 248) : RGB(20, 22, 30))
#define TXT_LO       (gDark ? RGB(160, 166, 182) : RGB(120, 126, 140))
#define TXT_MUTE     (gDark ? RGB(120, 126, 142) : RGB(140, 146, 160))
#define CHEVRON      (gDark ? RGB(110, 116, 132) : RGB(160, 165, 178))
#define BORDER       (gDark ? RGB(66, 74, 94)    : RGB(202, 210, 224))

// ---------------- Barras del sistema ----------------
#define BAR_TXT      RGB(255, 255, 255)
#define NAV_TINT     RGB(255, 255, 255)

// ---------------- Vidrio (Liquid Glass) ----------------
// Tintes de los paneles translucidos, tal cual en Flex OS Ultra.
#define GLASS_CLOCK  RGB(40, 62, 128)
#define GLASS_CARD   RGB(40, 50, 90)
#define GLASS_DOCK   RGB(180, 186, 206)
#define GLASS_ICON   RGB(120, 140, 200)
#define GLASS_A      170   // opacidad base del tinte (0..255)

#endif // FOS_THEME_H
