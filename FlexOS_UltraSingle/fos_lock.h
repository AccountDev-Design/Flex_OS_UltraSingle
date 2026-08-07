// #############################################################
// ##  Flex OS UltraSingle  -  fos_lock.h
// ##  Pantalla de bloqueo y verificacion por PIN
// #############################################################
#ifndef FOS_LOCK_H
#define FOS_LOCK_H

#include <Arduino.h>

void lockEnter();
void lockTick();

void authEnter(uint8_t reason);
void authTick();

// Bloqueo automatico: milisegundos de inactividad del ajuste activo
// (0 = nunca).
uint32_t autoLockMs();

#endif // FOS_LOCK_H
