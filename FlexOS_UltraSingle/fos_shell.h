// #############################################################
// ##  Flex OS UltraSingle  -  fos_shell.h
// ##  Maquina de estados del sistema
// #############################################################
//
//  Flex OS Ultra tenia 16 estados (Modo PC, Wi-Fi, OTA, kiosco,
//  apagado, conmutador de tareas...). Al quitar todo lo que no
//  existe en el Mega quedan los seis que de verdad forman la
//  experiencia: arranque, eleccion de idioma, bloqueo, PIN,
//  escritorio y aplicacion en primer plano.
// #############################################################
#ifndef FOS_SHELL_H
#define FOS_SHELL_H

#include <Arduino.h>

enum { ST_SPLASH = 0, ST_OOBE_LANG, ST_LOCK, ST_AUTH, ST_HOME, ST_APP };

extern uint8_t gState;
extern bool    gMinuteTick;    // true durante el frame en que cambia el minuto

void shellSetup();
void shellLoop();

void shellGoHome();            // vuelve al escritorio (con transicion)
void shellGoLock();
void shellOpenApp(uint8_t id, int16_t ix, int16_t iy, uint8_t is);

// Motivo por el que se pide el PIN.
enum { AUTH_UNLOCK = 0, AUTH_SETPIN };
void shellRequestAuth(uint8_t reason);

#endif // FOS_SHELL_H
