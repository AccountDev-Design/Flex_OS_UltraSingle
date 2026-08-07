// #############################################################
// ##  Flex OS UltraSingle
// ##  Arduino Mega 2560  ·  TFT Shield 3.5" ILI9486 (MAR3501)
// #############################################################
//
//  QUE ES ESTE PROYECTO
//  --------------------
//  La edicion de Flex OS Ultra para Arduino Mega 2560. Mismo
//  sistema operativo -- mismo wallpaper, misma barra superior,
//  misma pantalla de bloqueo, mismos iconos, mismos colores --
//  reconstruido por dentro para funcionar con 256 KB de Flash y
//  8 KB de SRAM en vez de un ESP32-P4 con PSRAM.
//
//  QUE CAMBIA POR DENTRO
//  ---------------------
//   · Sin framebuffer. Flex OS Ultra componia la pantalla en tres
//     buffers de 768 KB en PSRAM y los subia con un "presenter" en
//     el core 0. Aqui se dibuja DIRECTAMENTE en la GRAM del ILI9486
//     y solo se repintan las zonas que cambian. El unico buffer del
//     sistema es una linea de barrido de 640 B (fos_gfx.cpp).
//   · Sin FreeRTOS ni multitarea. Un solo bucle cooperativo y una
//     sola aplicacion en primer plano (fos_apps.cpp).
//   · Sin radio. Se elimino por completo Wi-Fi, Bluetooth Classic,
//     BLE, OTA, el navegador, la camara, la tienda, los juegos en
//     red y cualquier servicio en segundo plano: en esta placa no
//     existe el hardware que los sostenia.
//   · Sin DeX / Modo PC ni ventanas flotantes.
//   · Sin LittleFS ni NVS: la persistencia es un mapa fijo sobre la
//     EEPROM interna de 4 KB (fos_store.cpp).
//   · Sin String, sin new/malloc y sin coma flotante fuera de la
//     Calculadora. Todos los textos y tablas viven en PROGMEM.
//
//  HARDWARE (fijo, definido en fos_config.h)
//  -----------------------------------------
//    Placa    : Arduino Mega 2560
//    Pantalla : TFT LCD Shield 3.5" SKU MAR3501
//    Driver   : ILI9486, bus paralelo de 8 bits, 320x480
//    Tactil   : resistivo de 4 hilos, XP=8 XM=A2 YP=A3 YM=9
//    Calibrado: LEFT 76 · RT 905 · TOP 951 · BOT 68
//
//  BIBLIOTECAS NECESARIAS
//  ----------------------
//    MCUFRIEND_kbv        (David Prentice)
//    Adafruit GFX Library (Adafruit)
//    Adafruit TouchScreen (Adafruit)
//
//  MAPA DEL PROYECTO
//  -----------------
//    fos_config.h   hardware, geometria y constantes
//    fos_theme.h    paleta heredada de Flex OS Ultra
//    fos_gfx.*      motor grafico directo (wallpaper, vidrio, texto)
//    fos_touch.*    tactil resistivo y gestos
//    fos_str.*      textos en PROGMEM (espanol / ingles)
//    fos_time.*     reloj interno sin RTC
//    fos_store.*    persistencia en EEPROM
//    fos_icons.*    iconos vectoriales
//    fos_ui.*       cromo del sistema y controles comunes
//    fos_fx.*       transiciones
//    fos_home.*     escritorio y panel rapido
//    fos_lock.*     bloqueo y PIN
//    fos_shell.*    arranque, OOBE y bucle principal
//    fos_apps.*     registro de aplicaciones
//    app_*.cpp      las doce aplicaciones
// #############################################################

#include "fos_shell.h"

void setup(){
  shellSetup();
}

void loop(){
  shellLoop();
}
