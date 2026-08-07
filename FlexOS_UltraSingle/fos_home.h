// #############################################################
// ##  Flex OS UltraSingle  -  fos_home.h
// ##  Escritorio (widgets + rejilla + dock + panel rapido)
// #############################################################
#ifndef FOS_HOME_H
#define FOS_HOME_H

#include <Arduino.h>

void homeEnter();     // pintado completo
void homeTick();      // entrada + repintado de las zonas que cambian
void homeInvalidate(); // un ajuste cambio el aspecto: repintar al volver

#endif // FOS_HOME_H
