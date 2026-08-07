// #############################################################
// ##  Flex OS UltraSingle  -  fos_apps.h
// ##  Registro de aplicaciones
// #############################################################
//
//  Flex OS Ultra tenia multitarea real con tareas de FreeRTOS,
//  ventanas flotantes y un conmutador con miniaturas en PSRAM.
//  Nada de eso existe en el Mega: aqui hay UNA aplicacion en
//  primer plano y punto. El "registro" es una tabla de punteros a
//  funcion en PROGMEM (24 B) que sustituye a todo aquel andamiaje.
//
//  Contrato de una aplicacion:
//    enter() -> se llama UNA vez al abrirla. Pinta la pantalla
//               completa y prepara su estado.
//    tick()  -> se llama en cada vuelta del bucle. Lee 'tp' y
//               repinta SOLO lo que haya cambiado.
//  Ninguna aplicacion reserva memoria dinamica ni deja nada vivo
//  al salir.
// #############################################################
#ifndef FOS_APPS_H
#define FOS_APPS_H

#include <Arduino.h>
#include "fos_config.h"

enum {
  APP_CLOCK = 0, APP_CALENDAR, APP_CALC, APP_NOTES, APP_PAINT, APP_GAMES,
  APP_TIMER, APP_LIGHT, APP_WELL, APP_MEMORY, APP_SYSINFO, APP_SETTINGS
};

typedef void (*FAppFn)();

struct FApp {
  FAppFn  enter;
  FAppFn  tick;
  uint8_t flags;
};

#define AF_NONE       0x00
#define AF_FULLSCREEN 0x01   // la app se pinta sin barras del sistema

extern const FApp APPS[APP_N] PROGMEM;

void    appOpen(uint8_t id);
void    appTick();
uint8_t appCurrent();
uint8_t appFlags(uint8_t id);

// Cada modulo de aplicacion expone este par.
void appClockEnter();     void appClockTick();
void appCalendarEnter();  void appCalendarTick();
void appCalcEnter();      void appCalcTick();
void appNotesEnter();     void appNotesTick();
void appPaintEnter();     void appPaintTick();
void appGamesEnter();     void appGamesTick();
void appTimerEnter();     void appTimerTick();
void appLightEnter();     void appLightTick();
void appWellEnter();      void appWellTick();
void appMemoryEnter();    void appMemoryTick();
void appSysinfoEnter();   void appSysinfoTick();
void appSettingsEnter();  void appSettingsTick();

#endif // FOS_APPS_H
