// #############################################################
// ##  Flex OS UltraSingle  -  fos_store.h
// ##  Persistencia en la EEPROM interna (4 KB del ATmega2560)
// #############################################################
//
//  Sustituye a Preferences/NVS y a LittleFS de Flex OS Ultra. No
//  hay sistema de ficheros: hay un MAPA FIJO de la EEPROM, sin
//  asignacion dinamica ni indices, que es lo unico razonable con
//  4 KB y ~100.000 ciclos de escritura por celda.
//
//  Todas las escrituras pasan por EEPROM.update(), que no gasta un
//  ciclo si el byte ya vale lo que se le pide.
// #############################################################
#ifndef FOS_STORE_H
#define FOS_STORE_H

#include <Arduino.h>
#include "fos_config.h"

#define NOTE_SLOTS 3
#define NOTE_LEN   96      // caracteres utiles por nota (+1 de terminador)

// ---- Mapa de la EEPROM ----
#define EE_MAGIC     0     // 2 B  "FS"
#define EE_VER       2     // 1 B
#define EE_LANG      3     // 1 B
#define EE_FLAGS     4     // 1 B  bit0 dark, bit1 glass, bit2 iconGlass, bit3 h24, bit4 navGest
#define EE_AUTOLOCK  5     // 1 B  indice
#define EE_PINSET    6     // 1 B
#define EE_PIN       7     // 4 B
#define EE_HOMEORD   11    // 12 B
#define EE_BEST      23    // 2 B  record de Juegos
#define EE_TOUCHES   25    // 4 B  contador de toques (Bienestar)
#define EE_MINUTES   29    // 4 B  minutos de uso acumulados (Bienestar)
#define EE_TIME      33    // 5 B  ultima hora conocida (h, m, dia, mes, ano-2000)
#define EE_NOTES     40    // NOTE_SLOTS * (NOTE_LEN+1)
#define EE_END       (EE_NOTES + NOTE_SLOTS * (NOTE_LEN + 1))

// Ajustes vivos del sistema (los que Flex OS Ultra guardaba en NVS).
extern bool    gGlass;       // estilo Liquid Glass en superficies
extern bool    gIconGlass;   // estilo de icono: vidrio (true) o plano (false)
extern bool    gNavGest;     // barra de gestos en vez de botones
extern uint8_t gAutoLockIdx;
extern bool    gPinSet;
extern uint8_t gPin[LOCK_PIN_LEN];
extern uint8_t gHomeOrder[HOME_SLOTS];
extern uint16_t gBest;
extern uint32_t gTouches;
extern uint32_t gMinutes;
extern bool     gFirstRun;   // la EEPROM estaba virgen: hay que pasar por el OOBE

void storeInit();          // carga o inicializa a valores de fabrica
void storeSaveFlags();
void storeSaveLang();
void storeSavePin();
void storeSaveHomeOrder();
void storeSaveAutoLock();
void storeSaveBest();
void storeSaveUsage();
void storeSaveTime();
void storeLoadTime();
void storeFactoryReset();

void noteLoad(uint8_t slot, char* dst);          // dst >= NOTE_LEN+1
void noteSave(uint8_t slot, const char* src);
bool noteEmpty(uint8_t slot);

// SRAM libre real (hueco entre el final del heap y el puntero de pila).
uint16_t freeRam();

#endif // FOS_STORE_H
