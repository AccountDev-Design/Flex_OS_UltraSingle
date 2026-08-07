// #############################################################
// ##  Flex OS UltraSingle  -  Arduino Mega 2560
// ##  fos_config.h  -  hardware, geometria y version
// #############################################################
//
//  Placa   : Arduino Mega 2560 (ATmega2560, 256 KB Flash / 8 KB SRAM)
//  Pantalla: TFT LCD Shield 3.5" - SKU MAR3501
//  Driver  : ILI9486 - bus paralelo de 8 bits - 320x480
//  Tactil  : resistivo de 4 hilos integrado en el shield
//
//  Este fichero es el UNICO sitio donde vive la definicion del
//  hardware. Ningun otro modulo habla de pines ni de calibracion.
// #############################################################
#ifndef FOS_CONFIG_H
#define FOS_CONFIG_H

#include <Arduino.h>

// ---------------- Identidad ----------------
#define FOS_NAME     "Flex OS UltraSingle"
#define FOS_SHORT    "UltraSingle"
#define FOS_VERSION  "1.0"
#define FOS_BOARD    "Arduino Mega 2560"
#define FOS_PANEL    "ILI9486 3.5\" MAR3501"

// ---------------- Geometria ----------------
// Retrato nativo del panel. Flex OS UltraSingle es SOLO retrato:
// el Modo PC (landscape) desaparecio con DeX.
#define SCR_W  320
#define SCR_H  480

// Alturas de las barras del sistema (mismo reparto visual que Flex OS Ultra,
// escalado de 480x800 a 320x480).
#define BAR_TOP_H  34   // barra de estado (hora + fecha, dos lineas)
#define BAR_NAV_H  38   // barra de navegacion
#define APP_HDR_H  40   // cabecera de una aplicacion
#define APP_Y0     (APP_HDR_H)
#define APP_Y1     (SCR_H - BAR_NAV_H)
#define APP_CH     (APP_Y1 - APP_Y0)

// ---------------- Tactil (SKU MAR3501) ----------------
// OFICIAL DEL PROYECTO. No cambiar: toda la interfaz esta calibrada aqui.
#define TS_XP  8
#define TS_XM  A2
#define TS_YP  A3
#define TS_YM  9

#define TS_LEFT  76
#define TS_RT    905
#define TS_TOP   951
#define TS_BOT   68

// Resistencia entre placas X (ohmios) del MAR3501. Solo se usa para
// discriminar presion real de ruido.
#define TS_RXPLATE   300
#define TS_PRESS_MIN 60
#define TS_PRESS_MAX 1000

// ---------------- Gestos ----------------
#define GEST_TAP_MAX_MS   350   // duracion maxima de un toque para ser "tap"
#define GEST_TAP_SLOP     10    // px que puede moverse un tap sin dejar de serlo
#define GEST_SWIPE_MIN    45    // px minimos de recorrido para un deslizamiento
#define GEST_LONG_MS      600   // pulsacion larga
#define GEST_DEBOUNCE_MS  12    // antirrebote del panel resistivo

// ---------------- Comportamiento ----------------
#define SPLASH_MS         1400UL
#define LOCK_PIN_LEN      4
#define AUTOLOCK_OPTIONS  5
#define TICK_MS           16     // periodo objetivo del bucle de animacion

// Numero de aplicaciones instaladas (ver fos_apps.h)
#define APP_N      12
#define HOME_SLOTS 12
#define DOCK_SLOTS 4

#endif // FOS_CONFIG_H
