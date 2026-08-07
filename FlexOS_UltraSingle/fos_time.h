// #############################################################
// ##  Flex OS UltraSingle  -  fos_time.h
// ##  Reloj interno por millis() (el Mega no lleva RTC)
// #############################################################
//
//  Flex OS Ultra ya funcionaba SIN NTP: sembraba la hora y contaba
//  con el temporizador del chip. Aqui es lo mismo, con el cristal
//  del Mega. La hora se ajusta a mano desde Ajustes > Fecha y hora
//  y se conserva en EEPROM entre encendidos (con el desfase propio
//  de no tener bateria de respaldo).
// #############################################################
#ifndef FOS_TIME_H
#define FOS_TIME_H

#include <Arduino.h>

struct FosTime {
  uint8_t  s, m, h;
  uint8_t  day, mon;      // mon: 0..11
  uint16_t year;
  uint8_t  dow;           // 0 = domingo
};

extern FosTime gTime;
extern bool    gH24;      // formato de 24 horas

void timeInit();
// Avanza el reloj. Devuelve true cuando cambia el MINUTO (unica senal que
// obliga a repintar la hora en pantalla).
bool timeTick();
void timeSet(uint8_t h, uint8_t m, uint8_t day, uint8_t mon, uint16_t year);

bool    isLeap(uint16_t y);
uint8_t daysInMonth(uint16_t y, uint8_t mon);
uint8_t dowOf(uint16_t y, uint8_t mon, uint8_t day);

// "13:23" en el formato activo. dst >= 8 bytes.
void timeStr(char* dst);
// "sab, 4 jul" -> dst >= 24 bytes.
void dateShortStr(char* dst);
// "Sabado, 4 de julio" -> dst >= 40 bytes.
void dateLongStr(char* dst);

// Segundos encendido desde el arranque.
uint32_t uptimeSec();

#endif // FOS_TIME_H
