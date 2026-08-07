// #############################################################
// ##  Flex OS UltraSingle  -  fos_ui.h
// ##  Cromo del sistema y controles compartidos
// #############################################################
//
//  Aqui vive TODO lo que se repite en varias pantallas: barra
//  superior, barra de navegacion, cabecera de aplicacion, filas de
//  lista, interruptores, botones, avisos, teclado numerico y
//  teclado de texto. Que sea un solo sitio es lo que permite que
//  las doce aplicaciones quepan en la Flash: ninguna repinta su
//  propio cromo.
// #############################################################
#ifndef FOS_UI_H
#define FOS_UI_H

#include <Arduino.h>
#include "fos_config.h"
#include "fos_gfx.h"
#include "fos_touch.h"

// ---------------- Barra superior ----------------
// overWall: true sobre el wallpaper (Inicio/Bloqueo), false sobre una app.
void uiStatusBar(bool overWall);
// Repinta solo el reloj de la barra (se llama al cambiar el minuto).
void uiStatusClock(bool overWall);

// ---------------- Barra de navegacion ----------------
void uiNavBar(bool overWall);
// -1 = fuera, 0 = atras, 1 = inicio, 2 = recientes.
int8_t uiNavHit(int16_t x, int16_t y);

// ---------------- Cabecera de aplicacion ----------------
// Pinta el fondo de pagina + la cabecera con el titulo y la flecha de vuelta.
void uiAppScreen(PGM_P title);
void uiAppHeader(PGM_P title);
bool uiBackHit(int16_t x, int16_t y);

// ---------------- Controles ----------------
#define ROW_H 46
// Fila de lista al estilo Ajustes de Flex OS Ultra.
void uiRow(int16_t y, PGM_P label, const char* value, bool chevron);
void uiRowP(int16_t y, PGM_P label, PGM_P value, bool chevron);
void uiRowToggle(int16_t y, PGM_P label, bool on);
void uiToggle(int16_t x, int16_t y, bool on);
void uiCard(int16_t x, int16_t y, int16_t w, int16_t h);
void uiButtonP(int16_t x, int16_t y, int16_t w, int16_t h, PGM_P label, bool primary);

// Aviso breve en la parte baja de la pantalla (equivale al toast de Ultra).
void uiToast(PGM_P msg);
void uiToastTick();

// ---------------- Teclado numerico (PIN) ----------------
#define PAD_X     55
#define PAD_Y     200
#define PAD_BTN   50
#define PAD_HSTEP 80
#define PAD_VSTEP 58
void   uiPinPad(uint16_t bg, bool overWall);
int8_t uiPinPadHit(int16_t x, int16_t y);   // 0..9 digito, -2 borrar, -1 nada

// ---------------- Teclado de texto ----------------
#define KB_H  152
#define KB_Y  (SCR_H - BAR_NAV_H - KB_H)
void uiKeyboard();
// Devuelve el caracter escrito, 8 = borrar, 13 = aceptar, 0 = nada.
char uiKeyboardHit(int16_t x, int16_t y);
void uiKeyboardShiftToggle();

#endif // FOS_UI_H
