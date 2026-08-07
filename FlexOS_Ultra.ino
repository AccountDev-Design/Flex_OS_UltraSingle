// #############################################################
// ##  FlexOS Ultra  ·  ESP32-P4  ·  GUITION JC4880P443C_I_W
// ##  MIPI-DSI 480x800 (ST7701)  ·  GT911 tactil  ·  NATIVO
// #############################################################
//
//  QUE ES ESTE ARCHIVO
//  -------------------
//  Sistema operativo NUEVO, escrito DESDE CERO, a resolucion
//  NATIVA 480x800 (sin el modo puente 320x480 de ArduOS).
//
//  Lo UNICO que se reutiliza de ArduOS Z Ultra Pro v3.45-P4 es
//  la CAPA DE HARDWARE, porque son datos del fabricante que no
//  tiene sentido "reinventar" (y que ya estan probados en tu
//  placa). En concreto:
//
//    1) ENCENDER LA PANTALLA  -> flexPanelInit()
//         LDO canal 3 @2.5V (PHY MIPI) + bus DSI 2 lanes @500Mbps
//         + panel DPI 480x800 @34MHz + tabla DCS del ST7701.
//    2) MOSTRAR COLORES       -> el mecanismo de "presenter":
//         un framebuffer en PSRAM + una tarea en el core 0 que
//         sube las filas sucias al panel (esp_lcd_panel_draw_bitmap).
//         AQUI reescrito NATIVO (una sola resolucion, sin escalado).
//    3) HACER FUNCIONAR EL TACTIL -> gt* + flexTouchInit()
//         GT911 por I2C (SDA=7, SCL=8, RST=3), coords 0..479 x
//         0..799 ya calibradas de fabrica -> se usan DIRECTAS.
//
//  TODO LO DEMAS (motor grafico de alto nivel, fuentes, iconos,
//  gestos, arranque, OOBE, bloqueo, home, apps, ajustes...) es
//  original de FlexOS Ultra y NO proviene de ArduOS.
//
//  ENTORNO (identico a tu ArduOS-P4, no lo cambies):
//    Arduino IDE 2.3.10 · core arduino-esp32 v3.2.0 EXACTO
//    Board: ESP32P4 Dev Module · 360MHz · Flash 80MHz/QIO/16MB
//    PSRAM: Enabled · USB Mode: USB-OTG (TinyUSB)
//    Particion: cualquiera con zona de datos. FlexOS_FS monta
//               LittleFS sobre ella probando las etiquetas
//               habituales (spiffs / littlefs / ffat / storage),
//               asi que sirve el esquema que ya tengas elegido.
//               Ahi viven los dibujos de Paint y las notas.
//
//  DEPURACION SIN PC (trabajas solo desde el movil):
//    Si algo peta antes de dibujar, el motivo del ultimo reinicio
//    se muestra en una BANDA FORENSE en pantalla al bootear (abajo
//    del todo). Ver showBootBanner().
//
//  ESTADO: Milestone 1 (arranque + OOBE + bloqueo + escritorio).
//  Las apps y Ajustes llegan en los siguientes milestones. Ver el
//  bloque "HOJA DE RUTA" al final del archivo.
// #############################################################

#include <Wire.h>
#include <Preferences.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_system.h"          // esp_reset_reason() para la banda forense
#include <WiFi.h>                // pila WiFi (transporte hosted C6 por debajo, AUTO)
#include "esp_task_wdt.h"        // TWDT: esp_task_wdt_reset() en loop()
#include "esp_sleep.h"           // deep sleep real + ext1/timer como fuente de despertar (ESP32-P4)
#include "driver/gpio.h"         // gpio_hold_en / gpio_deep_sleep_hold_en (mantener RST del GT911 en sleep)

// SISTEMA GLOBAL DE ACTUALIZACIONES OTA. Solo la API publica: toda la
// logica (red, JSON, descarga por streaming, instalacion e interfaz One
// UI) vive en FlexOS_OTA.cpp, que es comun a las tres placas. La version
// de firmware y la URL del manifiesto se definen DENTRO de esa cabecera
// -- no aqui -- porque el .cpp es una unidad de traduccion aparte y un
// #define hecho en este .ino no llegaria hasta ella.
#include "FlexOS_OTA.h"

// SISTEMA DE ARCHIVOS REAL (LittleFS). Toda la logica de ficheros vive en
// FlexOS_FS.cpp -- comun a las tres placas -- para no tener tres copias de
// lo mismo que se desincronicen. Aqui solo se dibuja.
#include "FlexOS_FS.h"

// ---- DISPONIBILIDAD REAL DE BLE ------------------------------------------
// No se escribe a mano "el P4 no tiene BLE": se le pregunta al SDK. soc_caps.h
// define SOC_BLE_SUPPORTED solo en los chips que llevan radio Bluetooth, asi
// que el interruptor de BLE queda deshabilitado con "No disponible" en las
// placas que no pueden hacerlo, y funcional en las que si -- decidido en
// tiempo de COMPILACION, sin listas de modelos que mantener.
#include "soc/soc_caps.h"
#if defined(SOC_BLE_SUPPORTED) && SOC_BLE_SUPPORTED
  #define FLEXOS_BLE_HW 1
#else
  #define FLEXOS_BLE_HW 0
#endif
#ifndef FLEXOS_ENABLE_BLE
  #define FLEXOS_ENABLE_BLE FLEXOS_BLE_HW
#endif
#if FLEXOS_ENABLE_BLE
  #include <BLEDevice.h>
#endif

// -------------------------------------------------------------
//  Tipos usados como PARAMETRO de alguna funcion (FGlyph en fgPix/
//  drawGlyphScaled, PWin en pcDrawWindow, DexFit en dexHostFit/
//  dexHostScale, Touch en dexHostRun). Van AQUI ARRIBA DEL TODO
//  a proposito: el IDE de Arduino auto-genera prototipos de todas
//  las funciones y los inserta al inicio del archivo compilado; si
//  el tipo se define mas abajo, ese prototipo autogenerado no lo
//  conoce todavia y la compilacion falla con "does not name a type"
//  aunque en este .ino el tipo aparezca "antes" de usarse. Definir
//  estos tipos aqui arriba evita el problema pase lo que pase.
// -------------------------------------------------------------
typedef struct { uint8_t w, h; int8_t bx; int8_t topoff; uint8_t adv; uint16_t off; } FGlyph;
// PWin: estado de una ventana de Modo PC / DeX. Los campos nuevos (min, snap,
// rx/ry/rw/rh) viven AQUI ARRIBA por el mismo motivo que el resto del tipo: el
// IDE de Arduino autogenera los prototipos al inicio del archivo compilado.
//   mini          -> minimizada a la barra de tareas (sigue open). Se llama
//                    "mini" y no "min" porque Arduino define min() como macro.
//   snap          -> SNAP_FREE / mitades / cuadrantes / maximizada
//   rx,ry,rw,rh   -> geometria a la que restaurar al des-anclar
struct PWin { bool open; int x, y, w, h, app; bool mini; uint8_t snap; int rx, ry, rw, rh; };
// DexFit: encaje de una app hospedada dentro del area de cliente de su ventana
// (origen y tamano del contenido con la proporcion respetada, pasos de escalado
// en coma fija 16.16, y tamano del lienzo de la app). Vive aqui por la misma
// razon que PWin: dexHostFit y dexHostScale lo reciben por referencia.
struct DexFit { int ox, oy, ow, oh; uint32_t stepX, stepY; int aw, ah; bool land; bool flex; };
// Touch: estado tactil de alto nivel. Vive aqui porque dexHostRun lo recibe por
// puntero para INYECTAR un toque traducido en una app hospedada.
struct Touch {
  bool down=false, pressed=false, released=false, tap=false, moved=false;
  bool swipeUp=false, swipeDown=false, swipeLeft=false, swipeRight=false;
  int  x=0, y=0, startX=0, startY=0, dx=0, dy=0;
  unsigned long downMs=0, lastMs=0;
};

// #############################################################
// ##  TECLADO FLEXOS  ·  FASES A-G  ·  INTERRUPTORES MAESTROS
// ##  ------------------------------------------------------
// ##  Cada fase tiene SU PROPIO interruptor y se puede apagar
// ##  sola, sin tocar las demas (mismo patron que GLASS_*_ON,
// ##  KIOSK_ON o APPLOCK_ON). Si algo falla en la placa: baja
// ##  a 0 de la G hacia la A, recompila y mira cual era.
// ##
// ##    KB_SIZE_CONFIG_ON   Fase A - tamano de teclado configurable
// ##                        (Compacto/Normal/Grande). A 0 el teclado
// ##                        usa los valores fijos de siempre 43/48/4/6.
// ##    KB_MULTITOUCH_ON    Fase B - escritura rapida: la tecla se
// ##                        dispara al TOCAR (por ID de contacto del
// ##                        GT911). A 0 vuelve el disparo al soltar.
// ##    KB_TOOLBAR_ON       Fase C - barra superior de 5 accesos.
// ##    KB_CLIPBOARD_MULTI_ON Fase D - portapapeles de 12 ranuras con
// ##                        pin/borrado. A 0 vuelve el buffer unico.
// ##    KB_SETTINGS_ON      Fase E - pantalla de Ajustes del teclado.
// ##    KB_AUTOCOMPLETE_ON  Fase F - chips de autocompletado (lista
// ##                        local fija, NO es un modelo de IA).
// ##    KB_ANIM_POLISH_ON   Fase G - animaciones (apertura, tecla
// ##                        presionada, chips, toast). A 0 todo
// ##                        sigue funcionando, pero con cortes secos.
// #############################################################
#define KB_SIZE_CONFIG_ON     1
#define KB_MULTITOUCH_ON      1
#define KB_TOOLBAR_ON         1
#define KB_CLIPBOARD_MULTI_ON 1
#define KB_SETTINGS_ON        1
#define KB_AUTOCOMPLETE_ON    1
#define KB_ANIM_POLISH_ON     1

// FASE B - un punto de contacto del GT911 tal cual sale del chip.
//   id     -> track ID que asigna el propio GT911 (byte 7 del bloque).
//             Es lo que permite saber que "este dedo" es el mismo entre
//             frames aunque se muevan las coordenadas.
//   x, y   -> ya en coordenadas de FlexOS (mismos flags SWAP/FLIP que gtPoll)
//   active -> ese hueco del array tiene un dedo vivo en este frame
// Vive aqui arriba, con struct Touch, por la restriccion de ctags: el
// generador de prototipos de Arduino no puede ver un tipo definido a
// mitad de archivo.
struct TouchPoint { int id; int x, y; bool active; };
#define KB_MAXPOINTS 5
static TouchPoint gKbPoints[KB_MAXPOINTS];

// FASE D - una ranura del portapapeles.
//   pinned -> fijada por el usuario: no se descarta al llenarse y
//             SOBREVIVE al reinicio (se guarda en NVS).
//   used   -> la ranura tiene contenido
//   ts     -> millis() de cuando se copio (para saber cual es la mas vieja)
#define CLIP_SLOTS   12
#define CLIP_TXT_MAX 200
struct ClipItem { char text[CLIP_TXT_MAX]; bool pinned; bool used; uint32_t ts; };
static ClipItem gClip[CLIP_SLOTS];


// =============================================================
// GEOMETRIA NATIVA
// =============================================================
#define SCR_W   480
#define SCR_H   800
// Modo horizontal (PC): coords logicas landscape 800x480, rotadas 90 al panel
#define LW      SCR_H
#define LH      SCR_W

// =============================================================
// PINES (confirmados en la JC4880P443, reutilizados de ArduOS)
// =============================================================
#define PIN_LCD_RST   5     // reset del ST7701
#define PIN_LCD_BL    23    // backlight (encendido fijo)
#define PIN_TP_SDA    7     // GT911 SDA
#define PIN_TP_SCL    8     // GT911 SCL
#define PIN_TP_RST    3     // GT911 reset

// #############################################################
// ##  APAGADO DE PANTALLA  ·  interruptores maestros
// ##  ------------------------------------------------------
// ##  Cada sub-sistema se desactiva por separado (mismo patron que
// ##  GLASS_SHADOW_ON / KIOSK_ON / APPLOCK_ON) para poder aislar
// ##  cualquier problema en pruebas de hardware sin tocar el resto.
// #############################################################
#define SUSPEND_ON        1   // gesto de suspension (doble-tap 2 dedos) y de despertar (doble-tap 1 dedo)
#define SUSPEND_LOCK_ON   1   // al despertar de una suspension, caer en la pantalla de Bloqueo si el
                              // usuario tiene configurado PIN/contrasena (gLockType > 0). Sin clave
                              // configurada no cambia nada: se vuelve directo a donde estabas.
#define POWEROFF_ON       1   // apagado completo: icono en el Panel Rapido + confirmacion + deep sleep
#define POWEROFF_PIN_ON   1   // KILL-SWITCH de compilacion de la proteccion por PIN del apagado.
                              // El toggle real que ve el usuario vive en Ajustes -> Seguridad
                              // (gPoffPin, persistido en NVS). Con esta constante a 0 la
                              // proteccion no existe ni aunque el toggle este activado.
#define PANEL_DCS_SLEEP_ON 1  // comandos DCS de bajo consumo del ST7701 (0x28 DISPOFF / 0x10 SLPIN).
                              // Ponlo a 0 si tu panel no vuelve limpio de DISPON: el fundido de
                              // backlight por si solo ya deja la pantalla en negro absoluto.

// ---- Gesto de suspension / despertar (doble-tap) ------------------------
#define SUSP_TAP_WINDOW_MS 450  // ventana maxima entre el 1er y el 2o toque del doble-tap
#define SUSP_TAP_GAP_MS    45   // separacion MINIMA real entre los dos toques (filtra rebotes del GT911)
#define SUSP_TAP_MAX_MS    600  // duracion maxima de un toque para contar como "tap" (mas = long-press)
#define SUSP_TAP_FRAMES    2    // polls CONSECUTIVOS con n>=2 que confirman un toque de 2 dedos
#define SUSP_FADE_STEP_MS  10   // periodo de cada paso del fundido de backlight (no bloqueante)
#define SUSP_FADE_STEP     6    // puntos de brillo (0..100) por paso -> ~170 ms de fundido completo

// ---- Apagado completo: despertar desde deep sleep ------------------------
//
// PIN DE DESPERTAR (POFF_WAKE_GPIO)
// ---------------------------------
// Este proyecto NUNCA ha cableado la linea INT del GT911: el driver de arriba
// solo usa SDA(7) / SCL(8) / RST(3) y sondea por I2C. Como el numero de GPIO
// del INT en la JC4880P443C_I_W no esta confirmado en ninguna fuente que se
// haya podido verificar, NO se inventa aqui un pin: se deja en -1 y el codigo
// cae a la ruta alternativa (temporizador). Pon aqui el GPIO real del INT del
// GT911 de tu placa para activar la ruta buena.
//
// RESTRICCION DEL ESP32-P4 (verificada en soc_caps.h de ESP-IDF v5.4, que es
// la base de arduino-esp32 v3.2.0):
//   · SOC_DEEP_SLEEP_SUPPORTED    = 1  -> hay deep sleep real.
//   · SOC_PM_SUPPORT_EXT1_WAKEUP  = 1  -> ext1 SI existe en el P4.
//   · NO existe SOC_PM_SUPPORT_EXT0_WAKEUP -> ext0 NO existe en el P4.
//     (En el ESP32 clasico si; por eso la tarea decia "ext0/ext1 segun soporte".)
//   · SOC_RTCIO_PIN_COUNT         = 16 -> ext1 solo admite GPIO 0..15.
// Por eso el pin de despertar TIENE que estar en 0..15; el #if de abajo lo
// comprueba en compilacion y cae al temporizador si no lo esta.
#define POFF_WAKE_GPIO    -1  // GPIO del INT del GT911 (0..15). -1 = no cableado -> ruta por temporizador
#define POFF_WAKE_LEVEL    0  // nivel que despierta: 0 = flanco/nivel BAJO (INT del GT911 por defecto), 1 = ALTO
#define POFF_WAKE_POLL_MS 400 // ruta alternativa: cada cuanto despierta el temporizador a mirar el tactil
#define POFF_WAKE_HOLD_MS 3000 // presion sostenida necesaria para completar el arranque (requisito: ~3 s)
#define POFF_WAKE_GATE_MS 4200 // ventana total del filtro de arranque antes de rendirse y volver a dormir

// #############################################################
// ##  ISLA DINAMICA · tipos y estado  (FASE 1: solo la isla)
// ##  ------------------------------------------------------
// ##  Se definen ARRIBA (antes de cualquier funcion) a proposito:
// ##  las funciones de la isla toman punteros a estos structs, y
// ##  el auto-prototipado de Arduino (ctags) inserta prototipos al
// ##  principio del sketch. Si los tipos no existieran aun, esos
// ##  prototipos no compilarian. Definiendolos aqui, todo prototipo
// ##  generado ya conoce ModuleType/DetectedModule/Notification.
// #############################################################

// ---- Tipos de modulo (compartidos con la futura deteccion I2C, Fase 2) ----
enum ModuleType {
  MOD_UNKNOWN,
  MOD_ULTRASONIC,   // HC-SR04
  MOD_BME280,       // sensor I2C
  MOD_MPU6050,      // IMU I2C
  MOD_LED,
  MOD_BUTTON,
  MOD_SERVO,
  MOD_I2C_GENERIC
};

// Un modulo detectado (o simulado en Fase 1)
struct DetectedModule {
  ModuleType    type;
  char          name[24];       // "Sensor BME280"
  char          sub[28];        // "I2C 0x76 detectado"
  uint8_t       i2cAddr;        // 0 si no es I2C
  uint8_t       pins[4];        // reservado para Fase 2 (asignacion de pines)
  uint8_t       numPins;
  bool          active;
  unsigned long detectedAt;
};

// ---- Geometria de la isla ----
#define NOTIF_MAX         3
#define NOTIF_MARGIN_X    16
#define NOTIF_CARD_W      (SCR_W - 2 * NOTIF_MARGIN_X)                        // 448
#define NOTIF_CARD_H      64
#define NOTIF_GAP         10
#define NOTIF_RAD         28
#define NOTIF_Y0          56                                                  // borde sup. de la 1a tarjeta
#define NOTIF_ENTER_DROP  24                                                  // caida de la animacion de entrada (px)
#define NOTIF_HOLD_MS     5000                                                // ms visible antes de auto-descartarse
#define NOTIF_BAND_TOP    (NOTIF_Y0 - NOTIF_ENTER_DROP - 6)                   // 26
#define NOTIF_BAND_BOT    (NOTIF_Y0 + NOTIF_MAX * (NOTIF_CARD_H + NOTIF_GAP) + 6) // 284
#define NOTIF_BAND_H      (NOTIF_BAND_BOT - NOTIF_BAND_TOP)                   // 258

// ---- Fase de animacion de cada notificacion ----
enum NotifPhase { NP_IN, NP_IDLE, NP_DRAG, NP_OUT, NP_SPRING };

struct Notification {
  DetectedModule mod;
  bool           active;
  NotifPhase     phase;
  uint32_t       bornMs;        // inicio de la animacion de entrada (se fija al ARMARSE en Home)
  float          slideX;        // desplazamiento horizontal (descarte); 0 = en su sitio
  bool           armed;         // false = encolada pero aun no mostrada; se arma al verse en Home
};

// ---- Estado global de la cola ----
static Notification gNotifs[NOTIF_MAX];
static int          gNotifCount  = 0;
static bool         notifBandOn  = false;   // la banda tiene isla activa (o le debe un ultimo frame de limpieza)
static int          notifDragIdx = -1;      // tarjeta que el dedo esta arrastrando
static uint32_t     notifLastMs  = 0;       // throttle de animacion (~30 fps)
static bool         notifPaused    = false; // true mientras gState != ST_HOME (fases de la isla congeladas)
static uint32_t     notifPauseT0   = 0;     // millis() en que empezo la pausa (ver notifTick)

// ---- Deteccion de hardware I2C (FASE 2) ----
// Disparadores DEMO de la Fase 1 (notificacion falsa al llegar a Home + otra
// por cada tap en la esquina superior derecha).
//
// AHORA EN 0. La Fase 2 ya esta terminada y conectada: hwDetectTick() barre el
// bus I2C de verdad y i2cOnDevicePresent() empuja la notificacion real. Con los
// demos en 1 el sistema anunciaba al arrancar un "Sensor BME280 / I2C 0x76
// detectado", un "MPU6050", un "Servo"... de hardware que NO esta conectado, y
// ademas sacaba otro falso cada vez que tocabas cerca de los iconos de bateria
// y wifi (zona x>=SCR_W-52, y 36..56), sin nada que indicara que ese trozo de
// pantalla fuera pulsable. Eso es lo que hacia que la isla pareciera aleatoria
// y que lo que anunciaba no coincidiera con la placa.
//
// Ponlo en 1 solo si quieres volver a probar la isla sin sensores en el bus.
#define NOTIF_DEMO_TRIGGERS   0

#define MAX_MODULES_DETECTED  8
#define I2C_SCAN_LO           0x08     // rango 7-bit valido (evita direcciones reservadas)
#define I2C_SCAN_HI           0x77
#define I2C_SCAN_PER_TICK     8        // direcciones sondeadas por vuelta de loop (no bloquea)
static const uint32_t I2C_SWEEP_INTERVAL = 3000;   // ms entre barridos completos

static DetectedModule detectedModules[MAX_MODULES_DETECTED];
static int      detectedCount = 0;
static uint16_t modSweepId[MAX_MODULES_DETECTED];  // ultimo barrido en que se vio cada modulo
static uint8_t  i2cScanCursor = 0;                 // direccion actual dentro del barrido
static bool     i2cSweeping   = false;             // hay un barrido en curso
static uint32_t i2cLastSweep  = 0;                 // fin del ultimo barrido
static uint16_t i2cSweepId    = 0;                 // id del barrido (para reconciliar presencia)

// Radio (WiFi por el co-procesador ESP32-C6/esp-hosted). Declarado aqui
// ARRIBA -a proposito- porque Ajustes (mas abajo en el archivo, pero
// ANTES que la seccion de radio al final) necesita leerlo para mostrar
// el estado real de la conexion. La logica de arranque/escaneo/conexion
// vive toda junto a bootInitRadioSafe(), al final del archivo.
#define FLEXOS_ENABLE_WIFI 1
static volatile bool gNetOnline = false;   // true tras un WiFi.begin() exitoso; lo lee la UI (Ajustes, icono, etc.)

// Estado de las OTRAS dos radios, aqui arriba por el mismo motivo que
// gNetOnline: la pantalla de Ajustes lee los tres para pintar la categoria
// "Red e Internet", y esta ANTES en el archivo que la seccion de radio.
//   gAirplane -> modo avion (persistido en NVS, clave "airpl")
//   gBleOn    -> hay advertising BLE activo AHORA MISMO
static bool gAirplane = false;
static bool gBleOn    = false;

// #############################################################
// ##  CAPA DE HARDWARE  (reutilizada de ArduOS - datos del
// ##  fabricante de la placa)
// #############################################################

// ---- BRING-UP DEL PANEL: LDO + DSI + ST7701 + DPI -----------
static esp_lcd_panel_handle_t    flxPanel   = NULL;
static esp_lcd_panel_io_handle_t flxPanelIo = NULL;
static SemaphoreHandle_t         flxDpiSem  = NULL;

// Tabla de init del ST7701 (comandos DCS del vendor, BK0/BK1).
// Es la secuencia oficial del modelo JC4880P443 (misma que el
// demo LVGL del fabricante y la config ESPHome funcional).
typedef struct { uint8_t cmd; uint8_t n; const uint8_t* d; } FlxDcsRow;
static const uint8_t r01[] = {0x77,0x01,0x00,0x00,0x13};
static const uint8_t r02[] = {0x08};
static const uint8_t r03[] = {0x77,0x01,0x00,0x00,0x10};
static const uint8_t r04[] = {0x63,0x00};
static const uint8_t r05[] = {0x0D,0x02};
static const uint8_t r06[] = {0x10,0x08};
static const uint8_t r07[] = {0x10};
static const uint8_t r08[] = {0x80,0x09,0x53,0x0C,0xD0,0x07,0x0C,0x09,0x09,0x28,0x06,0xD4,0x13,0x69,0x2B,0x71};
static const uint8_t r09[] = {0x80,0x94,0x5A,0x10,0xD3,0x06,0x0A,0x08,0x08,0x25,0x03,0xD3,0x12,0x66,0x6A,0x0D};
static const uint8_t r10[] = {0x77,0x01,0x00,0x00,0x11};
static const uint8_t r11[] = {0x5D};
static const uint8_t r12[] = {0x58};
static const uint8_t r13[] = {0x87};
static const uint8_t r14[] = {0x80};
static const uint8_t r15[] = {0x4E};
static const uint8_t r16[] = {0x85};
static const uint8_t r17[] = {0x21};
static const uint8_t r18[] = {0x10,0x1F};
static const uint8_t r19[] = {0x03};
static const uint8_t r20[] = {0x00};
static const uint8_t r21[] = {0x78};
static const uint8_t r22[] = {0x78};
static const uint8_t r23[] = {0x88};
static const uint8_t r24[] = {0x00,0x3A,0x02};
static const uint8_t r25[] = {0x04,0xA0,0x00,0xA0,0x05,0xA0,0x00,0xA0,0x00,0x40,0x40};
static const uint8_t r26[] = {0x30,0x00,0x40,0x40,0x32,0xA0,0x00,0xA0,0x00,0xA0,0x00,0xA0,0x00};
static const uint8_t r27[] = {0x00,0x00,0x33,0x33};
static const uint8_t r28[] = {0x44,0x44};
static const uint8_t r29[] = {0x09,0x2E,0xA0,0xA0,0x0B,0x30,0xA0,0xA0,0x05,0x2A,0xA0,0xA0,0x07,0x2C,0xA0,0xA0};
static const uint8_t r30[] = {0x00,0x00,0x33,0x33};
static const uint8_t r31[] = {0x44,0x44};
static const uint8_t r32[] = {0x08,0x2D,0xA0,0xA0,0x0A,0x2F,0xA0,0xA0,0x04,0x29,0xA0,0xA0,0x06,0x2B,0xA0,0xA0};
static const uint8_t r33[] = {0x00,0x00,0x4E,0x4E,0x00,0x00,0x00};
static const uint8_t r34[] = {0x08,0x01};
static const uint8_t r35[] = {0xB0,0x2B,0x98,0xA4,0x56,0x7F,0xFF,0xFF,0xFF,0xFF,0xF7,0x65,0x4A,0x89,0xB2,0x0B};
static const uint8_t r36[] = {0x08,0x08,0x08,0x45,0x3F,0x54};
static const uint8_t r37[] = {0x77,0x01,0x00,0x00,0x00};
static const uint8_t rCM[] = {0x55};   // COLMOD: RGB565 (16 bit)
static const uint8_t rMA[] = {0x00};   // MADCTL: RGB, sin espejos

static const FlxDcsRow ST7701_INIT[] = {
  {0xFF,5,r01},{0xEF,1,r02},{0xFF,5,r03},{0xC0,2,r04},{0xC1,2,r05},
  {0xC2,2,r06},{0xCC,1,r07},{0xB0,16,r08},{0xB1,16,r09},{0xFF,5,r10},
  {0xB0,1,r11},{0xB1,1,r12},{0xB2,1,r13},{0xB3,1,r14},{0xB5,1,r15},
  {0xB7,1,r16},{0xB8,1,r17},{0xB9,2,r18},{0xBB,1,r19},{0xBC,1,r20},
  {0xC1,1,r21},{0xC2,1,r22},{0xD0,1,r23},{0xE0,3,r24},{0xE1,11,r25},
  {0xE2,13,r26},{0xE3,4,r27},{0xE4,2,r28},{0xE5,16,r29},{0xE6,4,r30},
  {0xE7,2,r31},{0xE8,16,r32},{0xEB,7,r33},{0xEC,2,r34},{0xED,16,r35},
  {0xEF,6,r36},{0xFF,5,r37},
  {0x3A,1,rCM},{0x36,1,rMA},{0x20,0,NULL},   // COLMOD + MADCTL + INVOFF
};

static bool flxDpiFlushDone(esp_lcd_panel_handle_t p,
                            esp_lcd_dpi_panel_event_data_t* e, void* ctx){
  BaseType_t hp = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &hp);
  return hp == pdTRUE;
}

// ---- Brillo real por PWM del backlight (lo controla el Panel Rapido) ----
static int  gBright = 80;      // 0..100
static bool gBlPwm  = false;   // true si el PWM se pudo enganchar
// Ultimo porcentaje REALMENTE escrito al PWM. No es lo mismo que gBright: el
// fundido de la suspension baja el PWM a 0 SIN tocar gBright (para no perder el
// brillo del usuario). Los fundidos arrancan desde aqui, no desde gBright --
// si arrancaran desde gBright, el fundido de vuelta empezaria ya en el valor
// final y la pantalla daria un fogonazo en vez de aparecer poco a poco.
static int  gBlPct  = 80;
static void setBacklight(int pct){
  if(pct < 5) pct = 5; if(pct > 100) pct = 100;
  gBright = pct; gBlPct = pct;
  if(gBlPwm) ledcWrite(PIN_LCD_BL, map(pct, 0, 100, 25, 255));
}
// Escribe el PWM del backlight para los FUNDIDOS. Mismo mecanismo de siempre
// (ledcWrite sobre PIN_LCD_BL); nunca se toca el pin a pelo con digitalWrite
// salvo en el mismo fallback "sin PWM" que ya usa flexPanelInit si ledcAttach
// falla. Dos diferencias deliberadas con setBacklight():
//
//  1) NO toca gBright. El brillo elegido por el usuario sigue intacto durante
//     toda la suspension, asi que restaurarlo al despertar es exacto y gratis.
//  2) Rampa LINEAL 0..100 -> duty 0..255, en vez del mapeo 25..255 de
//     setBacklight. Ese 25 es el suelo que impide dejar la pantalla invisible
//     desde el slider del Panel Rapido, pero para un fundido es justo lo que
//     sobra: por debajo de ese suelo el backlight todavia ilumina, asi que el
//     ultimo paso hasta 0 seria un corte seco en vez de un fundido. Con la
//     rampa lineal el negro se alcanza de verdad y de forma continua.
//     En el extremo alto las dos curvas practicamente coinciden (a 80% dan 204
//     y 209 de duty), y el fundido de vuelta termina llamando a setBacklight()
//     con el valor exacto, asi que no queda ninguna diferencia visible.
static void blWritePct(int pct){
  if(pct < 0) pct = 0; if(pct > 100) pct = 100;
  gBlPct = pct;
  if(!gBlPwm){ digitalWrite(PIN_LCD_BL, pct > 0 ? HIGH : LOW); return; }
  ledcWrite(PIN_LCD_BL, (uint32_t)(pct * 255 / 100));
}

static bool flexPanelInit(){
  // Backlight apagado durante el init (evita el flash blanco)
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);

  // 1) LDO interno del P4: alimenta el PHY MIPI (canal 3, 2.5V)
  esp_ldo_channel_config_t ldo = {};
  ldo.chan_id    = 3;
  ldo.voltage_mv = 2500;
  esp_ldo_channel_handle_t ldoH = NULL;
  if(esp_ldo_acquire_channel(&ldo, &ldoH) != ESP_OK){
    Serial.println(F("[HW] ERROR: LDO MIPI (canal 3) no disponible"));
    return false;
  }

  // 2) Bus DSI: 2 lanes @ 500 Mbps
  esp_lcd_dsi_bus_config_t bus = {};
  bus.bus_id             = 0;
  bus.num_data_lanes     = 2;
  bus.phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
  bus.lane_bit_rate_mbps = 500;
  esp_lcd_dsi_bus_handle_t dsiBus = NULL;
  if(esp_lcd_new_dsi_bus(&bus, &dsiBus) != ESP_OK){
    Serial.println(F("[HW] ERROR: esp_lcd_new_dsi_bus"));
    return false;
  }

  // 3) Canal de comandos DBI (para la tabla de init DCS)
  esp_lcd_dbi_io_config_t dbi = {};
  dbi.virtual_channel = 0;
  dbi.lcd_cmd_bits    = 8;
  dbi.lcd_param_bits  = 8;
  if(esp_lcd_new_panel_io_dbi(dsiBus, &dbi, &flxPanelIo) != ESP_OK){
    Serial.println(F("[HW] ERROR: esp_lcd_new_panel_io_dbi"));
    return false;
  }

  // 4) Panel DPI (el framebuffer de hardware que refresca solo)
  esp_lcd_dpi_panel_config_t dpi = {};
  dpi.virtual_channel    = 0;
  dpi.dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi.dpi_clock_freq_mhz = 34;
  dpi.pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565;
  dpi.num_fbs            = 1;
  dpi.video_timing.h_size            = SCR_W;
  dpi.video_timing.v_size            = SCR_H;
  dpi.video_timing.hsync_pulse_width = 12;
  dpi.video_timing.hsync_back_porch  = 42;
  dpi.video_timing.hsync_front_porch = 42;
  dpi.video_timing.vsync_pulse_width = 2;
  dpi.video_timing.vsync_back_porch  = 8;
  dpi.video_timing.vsync_front_porch = 166;
  dpi.flags.use_dma2d = true;
  if(esp_lcd_new_panel_dpi(dsiBus, &dpi, &flxPanel) != ESP_OK){
    Serial.println(F("[HW] ERROR: esp_lcd_new_panel_dpi"));
    return false;
  }

  // 5) Reset fisico del ST7701 (GPIO 5) y arranque del panel
  pinMode(PIN_LCD_RST, OUTPUT);
  digitalWrite(PIN_LCD_RST, HIGH); delay(5);
  digitalWrite(PIN_LCD_RST, LOW);  delay(10);
  digitalWrite(PIN_LCD_RST, HIGH); delay(120);
  if(esp_lcd_panel_init(flxPanel) != ESP_OK){
    Serial.println(F("[HW] ERROR: esp_lcd_panel_init"));
    return false;
  }

  // 6) Tabla del vendor + COLMOD/MADCTL/INVOFF + SLPOUT + DISPON
  for(size_t i = 0; i < sizeof(ST7701_INIT)/sizeof(ST7701_INIT[0]); i++){
    esp_lcd_panel_io_tx_param(flxPanelIo, ST7701_INIT[i].cmd,
                              ST7701_INIT[i].d, ST7701_INIT[i].n);
  }
  esp_lcd_panel_io_tx_param(flxPanelIo, 0x11, NULL, 0);  // SLPOUT
  delay(120);
  esp_lcd_panel_io_tx_param(flxPanelIo, 0x29, NULL, 0);  // DISPON
  delay(20);

  // 7) Callback de fin de flush (sincroniza el presenter)
  flxDpiSem = xSemaphoreCreateBinary();
  esp_lcd_dpi_panel_event_callbacks_t cbs = {};
  cbs.on_color_trans_done = flxDpiFlushDone;
  esp_lcd_dpi_panel_register_event_callbacks(flxPanel, &cbs, flxDpiSem);

  gBlPwm = ledcAttach(PIN_LCD_BL, 20000, 8);   // backlight ON con brillo PWM
  if(gBlPwm) setBacklight(gBright);
  else digitalWrite(PIN_LCD_BL, HIGH);         // fallback: encendido fijo
  Serial.println(F("[HW] Panel DSI 480x800 NATIVO OK"));
  return true;
}

// ---- Comandos DCS de bajo consumo del ST7701 -----------------
// La capa de driver de este proyecto SI expone DCS de bajo nivel: flxPanelIo es
// el canal DBI y flexPanelInit() ya manda 0x11 (SLPOUT) y 0x29 (DISPON) a pelo
// con esp_lcd_panel_io_tx_param. Se reutiliza ese mismo camino, sin librerias
// nuevas ni APIs sin confirmar.
//
//   0x28 DISPOFF -> el panel deja de driver la matriz. Reversible al instante
//                   con 0x29 y SIN reinicializar la tabla del vendor. Es lo que
//                   usa la SUSPENSION.
//   0x10 SLPIN   -> ademas apaga el generador interno. Mas ahorro, pero salir
//                   pide 0x11 + 120 ms. Solo se usa en el APAGADO COMPLETO,
//                   donde el chip se va a deep sleep y al volver flexPanelInit()
//                   rehace el panel entero de todas formas -> riesgo cero.
//
// SIEMPRE se llaman DESPUES de que el backlight haya llegado a 0, nunca antes:
// asi el usuario no llega a ver el efecto del panel entrando en el modo.
static void panelDisplayOff(){
#if PANEL_DCS_SLEEP_ON
  if(flxPanelIo) esp_lcd_panel_io_tx_param(flxPanelIo, 0x28, NULL, 0);   // DISPOFF
#endif
}
static void panelDisplayOn(){
#if PANEL_DCS_SLEEP_ON
  if(flxPanelIo) esp_lcd_panel_io_tx_param(flxPanelIo, 0x29, NULL, 0);   // DISPON
#endif
}
static void panelSleepIn(){
#if PANEL_DCS_SLEEP_ON
  if(!flxPanelIo) return;
  esp_lcd_panel_io_tx_param(flxPanelIo, 0x28, NULL, 0);                  // DISPOFF
  esp_lcd_panel_io_tx_param(flxPanelIo, 0x10, NULL, 0);                  // SLPIN
#endif
}

// ---- DRIVER TACTIL: GT911 capacitivo por I2C ----------------
// Coords ABSOLUTAS ya calibradas de fabrica (0..479 x 0..799,
// portrait). Sin presion Z, sin promediado, sin calibracion.
// Si tu lote sale espejado/cruzado, pon estos flags a 1:
#define GT911_SWAP_XY 0
#define GT911_FLIP_X  0
#define GT911_FLIP_Y  0

static uint8_t gtAddr = 0x5D;   // el GT911 puede ser 0x5D o 0x14
static bool    gtOk   = false;
// NUMERO DE DEDOS del ultimo frame valido del GT911. El byte de estado 0x814E
// ya trae la cuenta en (status & 0x0F); antes se leia y se tiraba. Lo unico que
// necesita el gesto de suspension es ESA cuenta, asi que NO se leen los bloques
// de los puntos extra (0x8158, 0x8160...): seguiria costando I2C en cada poll
// para unas coordenadas que nadie usa. gtPoll sigue devolviendo el primer punto
// exactamente igual que siempre -> el contrato de struct Touch no cambia.
static uint8_t  gtFingers   = 0;
static uint32_t gtFingersMs = 0;   // millis() del ultimo frame valido (para caducar la cuenta)

static bool gtWr(uint16_t reg, uint8_t val){
  Wire.beginTransmission(gtAddr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static bool gtRd(uint16_t reg, uint8_t* buf, uint8_t n){
  Wire.beginTransmission(gtAddr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if(Wire.endTransmission(false) != 0) return false;
  if(Wire.requestFrom((int)gtAddr, (int)n) != n) return false;
  for(uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

void flexTouchInit(){
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, LOW);  delay(10);
  digitalWrite(PIN_TP_RST, HIGH); delay(100);

  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);

  gtAddr = 0x5D;
  Wire.beginTransmission(gtAddr);
  if(Wire.endTransmission() != 0){
    gtAddr = 0x14;
    Wire.beginTransmission(gtAddr);
    if(Wire.endTransmission() != 0){
      Serial.println(F("[HW] GT911 NO detectado (0x5D/0x14)"));
      return;
    }
  }
  gtOk = true;
  uint8_t pid[4] = {0};
  gtRd(0x8140, pid, 4);
  Serial.printf("[HW] Touch GT911 en 0x%02X, ID: %c%c%c\n",
                gtAddr, pid[0], pid[1], pid[2]);
}

// Lee un frame del GT911. Devuelve 1=tocando (gx/gy validos),
// 0=soltado, -1=sin datos nuevos. Coords NATIVAS (0..479/0..799).
static int8_t gtPoll(uint16_t &gx, uint16_t &gy){
  if(!gtOk) return -1;
  uint8_t status = 0;
  if(!gtRd(0x814E, &status, 1)) return -1;
  if(!(status & 0x80)) return -1;
  uint8_t n = status & 0x0F;
  gtFingers = n; gtFingersMs = millis();   // cuenta de contactos para el gesto de suspension
  int8_t res = 0;
  if(n >= 1){
    uint8_t d[6];
    if(gtRd(0x8150, d, 6)){
      gx = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
      gy = (uint16_t)d[2] | ((uint16_t)d[3] << 8);
#if GT911_SWAP_XY
      { uint16_t t = gx; gx = gy; gy = t; }
#endif
#if GT911_FLIP_X
      gx = (SCR_W - 1) - gx;
#endif
#if GT911_FLIP_Y
      gy = (SCR_H - 1) - gy;
#endif
      if(gx > SCR_W - 1) gx = SCR_W - 1;
      if(gy > SCR_H - 1) gy = SCR_H - 1;
      res = 1;
    }
  }
  gtWr(0x814E, 0);            // limpiar buffer status
  return res;
}

// ---- FASE B: lectura MULTIPUNTO del GT911 (solo para el teclado) ----
// gtPoll() de arriba NO se toca: sigue siendo la unica fuente de struct Touch
// y todo el sistema (ventanas, gestos, Kiosk, notificaciones, juegos) sigue
// exactamente igual de single-touch que siempre. Esto es una ruta APARTE que
// solo usan las superficies de teclado.
//
// MAPA DE REGISTROS (hoja de datos del GT911, confirmado):
//   0x814E  estado: bit7 = hay frame nuevo, bits 3..0 = numero de dedos
//   0x8150  punto 1 (bloque de 8 bytes)   0x8158 punto 2   0x8160 punto 3
//   0x8168  punto 4                       0x8170 punto 5
//   dentro del bloque: [0..1] X (LSB primero), [2..3] Y (LSB primero),
//                      [4..5] tamano del contacto (no se usa), [6] reservado,
//                      [7] TRACK ID  <- lo que identifica al dedo entre frames
//
// POR QUE NO SE PELEA CON gtPoll(): esta funcion se llama desde el tick de la
// superficie, que corre en la MISMA vuelta del loop unos microsegundos despues
// de flexPollTouch(). A esas alturas gtPoll ya limpio el flag de "frame nuevo",
// asi que aqui casi siempre se entra por la rama de abajo: se reutiliza la
// cuenta de dedos de ESE mismo frame (gtFingers) y se leen los bloques de
// puntos, que conservan las coordenadas del ultimo frame reportado. Cero
// frames robados a gtPoll. En la rara vuelta en que el chip publica un frame
// justo en medio, se consume y se limpia el estado como manda la hoja de datos
// (gtPoll devolvera -1 esa vuelta, que es un caso que flexPollTouch ya
// contempla desde siempre).
//
// Devuelve el numero de puntos activos, o -1 si no habia dato utilizable.
static int gtPollMulti(){
  for(int i = 0; i < KB_MAXPOINTS; i++) gKbPoints[i].active = false;
  if(!gtOk) return -1;
  uint8_t status = 0;
  bool fresh = false;
  int n = -1;
  if(gtRd(0x814E, &status, 1) && (status & 0x80)){
    fresh = true;
    n = status & 0x0F;
    gtFingers = (uint8_t)n; gtFingersMs = millis();
  }
  if(n < 0){
    if(millis() - gtFingersMs > 120) return -1;   // la cuenta ya caduco: sin dato fiable
    n = gtFingers;
  }
  if(n > KB_MAXPOINTS) n = KB_MAXPOINTS;
  for(int i = 0; i < n; i++){
    uint8_t d[8];
    if(!gtRd((uint16_t)(0x8150 + i * 8), d, 8)) break;
    int px = (int)((uint16_t)d[0] | ((uint16_t)d[1] << 8));
    int py = (int)((uint16_t)d[2] | ((uint16_t)d[3] << 8));
#if GT911_SWAP_XY
    { int t = px; px = py; py = t; }
#endif
#if GT911_FLIP_X
    px = (SCR_W - 1) - px;
#endif
#if GT911_FLIP_Y
    py = (SCR_H - 1) - py;
#endif
    if(px < 0) px = 0; if(px > SCR_W - 1) px = SCR_W - 1;
    if(py < 0) py = 0; if(py > SCR_H - 1) py = SCR_H - 1;
    gKbPoints[i].id     = (int)d[7];
    gKbPoints[i].x      = px;
    gKbPoints[i].y      = py;
    gKbPoints[i].active = true;
  }
  if(fresh) gtWr(0x814E, 0);     // igual que gtPoll: se limpia el estado tras leer
  return n;
}
// #############################################################
// ##  FIN de la capa de hardware reutilizada
// #############################################################

// #############################################################
// ##  MOTOR GRAFICO NATIVO 480x800  (original de FlexOS)
// ##  Framebuffer en PSRAM + presenter en core 0 + primitivas
// #############################################################

// Tres capas en PSRAM (la placa tiene de sobra):
//   fb       -> lo que el presenter sube al panel
//   lockBuf  -> pantalla de bloqueo pre-renderizada (swipe fluido)
//   homeBuf  -> escritorio pre-renderizado
static uint16_t* fb      = NULL;
static uint16_t* bbuf    = NULL;   // back buffer: se compone el frame aqui y se vuelca de una vez (anti-flicker)
static uint16_t* lockBuf = NULL;
static uint16_t* homeBuf = NULL;

// Destino de dibujo actual (todas las primitivas escriben aqui)
static uint16_t* gBuf = NULL;

// ---- Redireccion del destino de render (hosting de apps en Modo PC) ----
// Toda app hace setBuf(fb) y termina con flxFlush(): esta cableada a la
// pantalla. Para poder ejecutar una app DE VERDAD dentro de una ventana de DeX
// hace falta desviarla a un lienzo propio sin tocar ni una linea de las 16 apps.
// Con gRtTarget != NULL:
//   · setBuf(fb) va al lienzo de la ventana (cualquier otro buffer se respeta),
//   · flxFlush/present no vuelcan nada al panel, solo anotan que hubo dibujo,
//   · fbCopyBand escribe en el lienzo en vez de en fb.
// Asi la app cree que pinta a pantalla completa y en realidad esta pintando su
// propio 480x800 fuera de pantalla, que luego DeX escala dentro del marco.
static uint16_t* gRtTarget = NULL;
static bool      gRtDirty  = false;   // la app pidio volcar algo desde el ultimo reset
static inline void setBuf(uint16_t* b){ gBuf = (gRtTarget && b == fb) ? gRtTarget : b; }

static volatile bool gReady = false;
// Banda de recorte vertical (para listas con scroll). Por defecto: toda la pantalla.
static int gClipY0 = 0, gClipY1 = SCR_H - 1;
static int gClipX0 = 0, gClipX1 = SCR_W - 1;   // recorte horizontal
static volatile int  gDirtyY0 = 0x7FFF, gDirtyY1 = -1;
static portMUX_TYPE  gMux = portMUX_INITIALIZER_UNLOCKED;
// EXCLUSION COMPOSICION <-> SUBIDA AL PANEL.
// El presenter sube fb con esp_lcd_panel_draw_bitmap, que es ASINCRONO: la DMA2D
// sigue LEYENDO fb despues de que la llamada vuelva (por eso hay una espera al
// semaforo de fin de DMA). Sin este candado, el hilo de UI podia estar
// escribiendo el cuadro N+1 en fb mientras la DMA todavia leia el cuadro N: la
// mitad de arriba salia con la posicion nueva y la de abajo con la vieja. Ese
// era el "vidrio liquido que se parte en lineas" del Panel Rapido -- y por eso
// dependia de la velocidad del gesto: cuanto mas rapido el dedo, mas distancia
// entre las dos posiciones y mas separadas se veian las costuras.
// El candado lo toma el presenter alrededor de (draw_bitmap + espera de DMA) y
// lo toma tambien todo volcado en bloque a fb (fbCopyBand/blitToFb/present), que
// es por donde pasan TODAS las animaciones compuestas del sistema. Resultado:
// un cuadro nunca se pisa a si mismo a medio subir, a cualquier velocidad.
static SemaphoreHandle_t flxFbMux = NULL;
static inline void fbLock(){   if(flxFbMux) xSemaphoreTake(flxFbMux, portMAX_DELAY); }
static inline void fbUnlock(){ if(flxFbMux) xSemaphoreGive(flxFbMux); }
// Handle del presenter. Sirve para DESPERTARLO en cuanto una banda queda lista,
// en vez de que descubra el trabajo en su siguiente sondeo periodico. Con esto
// baja la latencia de dibujo (antes: hasta ~11 ms de espera muerta) y se elimina
// el micro-stutter que aparecia al desfasar el ritmo de composicion de la UI
// contra la rejilla fija de 11 ms del presenter. NO cambia que pixeles se pintan
// -- solo CUANDO se suben al panel (siempre bandas ya terminadas en fb).
static TaskHandle_t  flxPresenterTask = NULL;

// FASE 4 del Modo Kiosco: se define mucho mas abajo (necesita las primitivas de
// dibujo), pero se declara aqui porque flxFlush -el unico punto por el que TODO
// acaba llegando al panel- tiene que llamarla antes de publicar la banda.
static void kioskStampBadge(int y0, int y1);

static void flxFlush(int y0, int y1){
  if(gRtTarget){ gRtDirty = true; return; }   // app hospedada: no toca el panel
  if(y0 < 0) y0 = 0; if(y1 >= SCR_H) y1 = SCR_H - 1;
  if(y0 > y1) return;
  // El candado del kiosco se estampa ANTES de marcar la banda como sucia: asi
  // ninguna banda llega nunca al presenter sin el, y no puede parpadear aunque
  // la app de encima repinte su esquina en cada frame. Escribe en fb, asi que
  // va bajo el mismo candado que el resto de la composicion (aqui nadie lo
  // tiene tomado todavia: fbCopyBand lo suelta antes de llamar a flxFlush).
  fbLock();
  kioskStampBadge(y0, y1);
  fbUnlock();
  portENTER_CRITICAL(&gMux);
  if(y0 < gDirtyY0) gDirtyY0 = y0;
  if(y1 > gDirtyY1) gDirtyY1 = y1;
  portEXIT_CRITICAL(&gMux);
  // Aviso al presenter. flxFlush SIEMPRE corre en contexto de tarea (nunca en
  // ISR: el callback de fin de DMA usa su propio semaforo, flxDpiSem), asi que
  // xTaskNotifyGive es correcto. La notificacion de FreeRTOS se "latchea": si
  // llega mientras el presenter todavia no esta bloqueado, se recuerda y el
  // frame no se pierde. Varios avisos seguidos colapsan en un solo despertar que
  // sube la UNION de las bandas sucias ya coalescida arriba -> sin volcados de mas.
  if(flxPresenterTask) xTaskNotifyGive(flxPresenterTask);
}
static inline void flxFlushAll(){ flxFlush(0, SCR_H - 1); }
// Vuelca la banda [y0,y1] del back buffer a fb de una sola pasada y la marca dirty.
// Componer en bbuf y presentar asi evita que el presenter muestre cuadros a medias.
// Copia la banda [y0,y1] de src a fb de una sola pasada por fila completa.
static void fbCopyBand(const uint16_t* src, int y0, int y1){
  if(!src) return;
  if(y0 < 0) y0 = 0;
  if(y1 >= SCR_H) y1 = SCR_H - 1;
  if(y0 > y1) return;
  uint16_t* dst = gRtTarget ? gRtTarget : fb;      // app hospedada: a su lienzo
  if(dst == src) return;
  // Solo hay carrera con la DMA cuando el destino es fb de verdad (el lienzo de
  // una ventana de DeX no lo lee nadie mas).
  bool guard = (dst == fb);
  if(guard) fbLock();
  memcpy(dst + (size_t)y0 * SCR_W, src + (size_t)y0 * SCR_W, (size_t)(y1 - y0 + 1) * SCR_W * 2);
  if(guard) fbUnlock();
}

static void present(int y0, int y1){
  if(y0 < 0) y0 = 0; if(y1 >= SCR_H) y1 = SCR_H - 1; if(y0 > y1) return;
  fbCopyBand(bbuf, y0, y1);
  flxFlush(y0, y1);
}

static void flxPresenter(void*){
  for(;;){
    // Duerme hasta que alguien ensucie una banda (flxFlush -> xTaskNotifyGive).
    // El timeout de 15 ms es una RED DE SEGURIDAD: aunque un aviso nunca deberia
    // perderse (la notificacion se latchea), si por lo que fuera se perdiera, el
    // presenter despierta igual y sube cualquier region pendiente -> jamas se
    // queda un frame "colgado". pdTRUE = limpia la cuenta al salir (se comporta
    // como un semaforo binario). Antes aqui habia un vTaskDelay(11) fijo que
    // dormia SIEMPRE, aunque hubiera un frame listo para subir de inmediato.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15));
    if(!gReady) continue;
    int y0, y1;
    portENTER_CRITICAL(&gMux);
    y0 = gDirtyY0; y1 = gDirtyY1;
    gDirtyY0 = 0x7FFF; gDirtyY1 = -1;
    portEXIT_CRITICAL(&gMux);
    if(y1 >= y0){
      // El candado cubre la subida ENTERA (llamada + fin de DMA): mientras la
      // DMA2D lee fb, ningun volcado de composicion puede reescribirlo debajo.
      fbLock();
      esp_lcd_panel_draw_bitmap(flxPanel, 0, y0, SCR_W, y1 + 1,
                                fb + (size_t)y0 * SCR_W);
      // Espera a que la DMA2D termine antes del siguiente volcado (serializa los
      // draws por fin-de-DMA: nunca se solapan ni "inundan" el bus del panel).
      xSemaphoreTake(flxDpiSem, pdMS_TO_TICKS(100));
      fbUnlock();
    }
  }
}

static bool flxGfxInit(){
  size_t bytes = (size_t)SCR_W * SCR_H * 2;
  // Alineados a 64 bytes = tamano de linea de cache de la PSRAM del P4.
  // El presenter vuelca BANDAS parciales (fb + y0*SCR_W) a la DMA2D, que
  // exige un write-back de cache limpio del origen antes de leer. Si fb
  // no arranca en una frontera de 64B, el offset y0*SCR_W de una banda
  // puede caer a mitad de linea de cache: el write-back deja sin
  // sincronizar el principio de esa fila y la DMA2D lee PSRAM vieja ahi
  // -> esa fila sale desplazada/con basura, y como esto se repite en
  // cada flush con distinto y0, el resultado visual es una costura
  // diagonal de pixeles de colores erraticos. Cada fila aqui son
  // 480*2=960 bytes (multiplo exacto de 64), asi que con el buffer
  // alineado TODOS los offsets de fila quedan tambien alineados.
  fb      = (uint16_t*)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  bbuf    = (uint16_t*)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  lockBuf = (uint16_t*)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  homeBuf = (uint16_t*)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!fb || !bbuf || !lockBuf || !homeBuf){
    Serial.println(F("[GFX] ERROR: sin PSRAM para framebuffers"));
    return false;
  }
  memset(fb, 0, bytes);
  setBuf(fb);
  // Antes de arrancar el presenter: si el mutex no existiera, fbLock/fbUnlock
  // son no-ops y el comportamiento seria el de siempre (sin proteccion), nunca
  // un cuelgue.
  flxFbMux = xSemaphoreCreateMutex();
  // Primer volcado en negro
  esp_lcd_panel_draw_bitmap(flxPanel, 0, 0, SCR_W, SCR_H, fb);
  xSemaphoreTake(flxDpiSem, pdMS_TO_TICKS(200));
  gReady = true;
  xTaskCreatePinnedToCore(flxPresenter, "flxPresenter", 4096, NULL, 3, &flxPresenterTask, 0);
  return true;
}

// ---------------- Color ----------------
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b){
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
// Division exacta por 255 SIN instruccion de division, para v en [0, 65534].
// Identidad clasica: v/255 == (v + 1 + (v>>8)) >> 8 en ese rango.
// Aqui el numerador maximo es 63*255 = 16065 (canal verde, 6 bits), muy por
// debajo del limite, asi que el resultado es BIT A BIT identico al de /255.
// Motivo: la division entera en el RISC-V del P4 cuesta decenas de ciclos y
// mix565 se ejecuta por PIXEL en cada alpha, cada panel de vidrio y cada blur.
#define DIV255(v)  (uint16_t)(((v) + 1u + ((v) >> 8)) >> 8)

// mezcla a<-b con peso t (0..255). t=0 => a, t=255 => b
static inline uint16_t mix565(uint16_t a, uint16_t b, uint8_t t){
  uint32_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  uint32_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  uint32_t it = 255u - t;
  uint32_t rr = ar * it + br * t;
  uint32_t rg = ag * it + bg * t;
  uint32_t rb = ab * it + bb * t;
  return (uint16_t)((DIV255(rr) << 11) | (DIV255(rg) << 5) | DIV255(rb));
}

// ---------------- Utilidades ----------------
// Raiz entera por restas binarias: SIN division (la version anterior hacia
// una division dentro del bucle de Newton). Se llama una vez por FILA de cada
// rectangulo redondeado, circulo y panel de vidrio. Resultado identico.
static inline int isqrt32(int v){
  if(v <= 0) return 0;
  uint32_t op = (uint32_t)v, res = 0, one = 1u << 30;
  while(one > op) one >>= 2;
  while(one){
    if(op >= res + one){ op -= res + one; res += one << 1; }
    res >>= 1; one >>= 2;
  }
  return (int)res;
}

// ---------------- Primitivas (escriben en gBuf) ----------------
// Rotacion landscape (Modo PC). Cuando gLand=true, las coords logicas
// (lx 0..799, ly 0..479) se escriben rotadas 90 sobre el panel portrait.
static bool gLand = false;
static inline void putPhys(int lx, int ly, uint16_t c){
  if((unsigned)lx >= SCR_H || (unsigned)ly >= SCR_W) return;
  int x = (SCR_W - 1) - ly, y = lx;
  if(y < gClipY0 || y > gClipY1) return;
  gBuf[(size_t)y * SCR_W + x] = c;
}
static inline void putPhysA(int lx, int ly, uint16_t c, uint8_t a){
  if((unsigned)lx >= SCR_H || (unsigned)ly >= SCR_W) return;
  int x = (SCR_W - 1) - ly, y = lx;
  if(y < gClipY0 || y > gClipY1) return;
  if(a >= 255){ gBuf[(size_t)y * SCR_W + x] = c; return; }
  if(a == 0) return;
  size_t i = (size_t)y * SCR_W + x; gBuf[i] = mix565(gBuf[i], c, a);
}
static inline void px(int x, int y, uint16_t c){
  if(gLand){ putPhys(x, y, c); return; }
  if((unsigned)x >= SCR_W || (unsigned)y >= SCR_H) return;
  if(y < gClipY0 || y > gClipY1 || x < gClipX0 || x > gClipX1) return;
  gBuf[(size_t)y * SCR_W + x] = c;
}
// pixel con alpha (0..255) sobre lo que ya hay en gBuf
static inline void pxA(int x, int y, uint16_t c, uint8_t a){
  if(gLand){ putPhysA(x, y, c, a); return; }
  if((unsigned)x >= SCR_W || (unsigned)y >= SCR_H) return;
  if(y < gClipY0 || y > gClipY1 || x < gClipX0 || x > gClipX1) return;
  if(a >= 255){ gBuf[(size_t)y * SCR_W + x] = c; return; }
  if(a == 0) return;
  size_t i = (size_t)y * SCR_W + x;
  gBuf[i] = mix565(gBuf[i], c, a);
}
static void hLine(int x, int y, int w, uint16_t c){
  if(w <= 0) return;
  if(gLand){ for(int i = 0; i < w; i++) putPhys(x + i, y, c); return; }
  if((unsigned)y >= SCR_H) return;
  if(y < gClipY0 || y > gClipY1) return;
  if(x < 0){ w += x; x = 0; }
  if(x + w > SCR_W) w = SCR_W - x;
  if(x < gClipX0){ w -= (gClipX0 - x); x = gClipX0; }     // recorte horizontal
  if(x + w > gClipX1 + 1) w = gClipX1 + 1 - x;
  if(w <= 0) return;
  uint16_t* p = gBuf + (size_t)y * SCR_W + x;
  for(int i = 0; i < w; i++) p[i] = c;
}
// Igual que hLine pero mezclando. Antes llamaba a pxA por pixel, lo que repetia
// TODOS los recortes (bordes + banda vertical + viewport) en cada pixel. Ahora
// recorta UNA vez y mezcla en linea recta: mismas reglas de recorte que hLine,
// mismo resultado, sin el coste por pixel. fillRectA/fillCircleA/fillRoundRectA
// cuelgan de aqui, asi que esto abarata todo el relleno translucido del sistema.
static void hLineA(int x, int y, int w, uint16_t c, uint8_t a){
  if(a >= 255){ hLine(x, y, w, c); return; }
  if(a == 0 || w <= 0) return;
  if(gLand){ for(int i = 0; i < w; i++) pxA(x + i, y, c, a); return; }   // landscape: ruta original
  if((unsigned)y >= SCR_H) return;
  if(y < gClipY0 || y > gClipY1) return;
  if(x < 0){ w += x; x = 0; }
  if(x + w > SCR_W) w = SCR_W - x;
  if(x < gClipX0){ w -= (gClipX0 - x); x = gClipX0; }
  if(x + w > gClipX1 + 1) w = gClipX1 + 1 - x;
  if(w <= 0) return;
  uint16_t* p = gBuf + (size_t)y * SCR_W + x;
  for(int i = 0; i < w; i++) p[i] = mix565(p[i], c, a);
}
static void vLine(int x, int y, int h, uint16_t c){
  if(h <= 0) return;
  if(gLand){ for(int i = 0; i < h; i++) putPhys(x, y + i, c); return; }
  if((unsigned)x >= SCR_W || x < gClipX0 || x > gClipX1) return;   // recorte horizontal
  if(y < 0){ h += y; y = 0; }
  if(y < gClipY0){ h -= (gClipY0 - y); y = gClipY0; }
  if(y + h > SCR_H) h = SCR_H - y;
  if(y + h > gClipY1 + 1) h = gClipY1 + 1 - y;
  if(h <= 0) return;
  uint16_t* p = gBuf + (size_t)y * SCR_W + x;
  for(int i = 0; i < h; i++){ *p = c; p += SCR_W; }
}
// --- Relleno rapido en LANDSCAPE (gLand) -------------------------------------
// La rotacion mapea (lx,ly) -> idx = lx*SCR_W + (SCR_W-1-ly). Es decir: para un
// lx FIJO, recorrer ly da direcciones CONSECUTIVAS. Una linea logica horizontal,
// en cambio, salta SCR_W*2 = 960 bytes por pixel, o sea UNA LINEA DE CACHE POR
// PIXEL contra PSRAM -- que es lo que hacia lento todo el Modo PC (un relleno de
// 430x268 son 115k escrituras dispersas).
// Por eso aqui se rellena por COLUMNA LOGICA: cada span queda secuencial. Mismo
// resultado pixel a pixel, mismo recorte (gClipY0/gClipY1 acotan filas fisicas,
// que en landscape son justo el eje lx).
static void fillSpanLand(int lx, int ly, int n, uint16_t c){
  if(n <= 0) return;
  if((unsigned)lx >= SCR_H) return;
  if(lx < gClipY0 || lx > gClipY1) return;
  if(ly < 0){ n += ly; ly = 0; }
  if(ly + n > SCR_W) n = SCR_W - ly;
  if(n <= 0) return;
  uint16_t* p = gBuf + (size_t)lx * SCR_W + (SCR_W - (ly + n));
  for(int i = 0; i < n; i++) p[i] = c;
}
static void fillSpanLandA(int lx, int ly, int n, uint16_t c, uint8_t a){
  if(a >= 255){ fillSpanLand(lx, ly, n, c); return; }
  if(a == 0 || n <= 0) return;
  if((unsigned)lx >= SCR_H) return;
  if(lx < gClipY0 || lx > gClipY1) return;
  if(ly < 0){ n += ly; ly = 0; }
  if(ly + n > SCR_W) n = SCR_W - ly;
  if(n <= 0) return;
  uint16_t* p = gBuf + (size_t)lx * SCR_W + (SCR_W - (ly + n));
  for(int i = 0; i < n; i++) p[i] = mix565(p[i], c, a);
}
static void fillRect(int x, int y, int w, int h, uint16_t c){
  if(gLand){ for(int i = 0; i < w; i++) fillSpanLand(x + i, y, h, c); return; }
  for(int j = 0; j < h; j++) hLine(x, y + j, w, c);
}
static void fillRectA(int x, int y, int w, int h, uint16_t c, uint8_t a){
  if(gLand){ for(int i = 0; i < w; i++) fillSpanLandA(x + i, y, h, c, a); return; }
  for(int j = 0; j < h; j++) hLineA(x, y + j, w, c, a);
}
static void drawRect(int x, int y, int w, int h, uint16_t c){
  hLine(x, y, w, c); hLine(x, y + h - 1, w, c);
  vLine(x, y, h, c); vLine(x + w - 1, y, h, c);
}

// Encaja un rect dentro de [0,limW) x [0,limH) respetando un tamano minimo y
// dejando SIEMPRE al menos `keep` px alcanzables dentro de la pantalla:
//   · en horizontal, `keep` px del rect siguen dentro por el lado que sea;
//   · en vertical, el borde SUPERIOR queda en [0, limH-keep], de modo que la
//     zona de agarre (barra de titulo, cabecera) nunca se va debajo del area.
// Es la puerta por la que debe pasar cualquier geometria movible: sin esto un
// arrastre o un resize puede dejar un elemento fuera de lo visible y volverlo
// imposible de tocar -- y con el, bloquear toda la interaccion.
// Devuelve true si hubo que corregir algo.
static bool flxClampRect(int &x, int &y, int &w, int &h,
                         int limW, int limH, int minW, int minH, int keep){
  int ox = x, oy = y, ow = w, oh = h;
  if(limW < 1) limW = 1;
  if(limH < 1) limH = 1;
  if(minW > limW) minW = limW;
  if(minH > limH) minH = limH;
  if(w < minW) w = minW;
  if(h < minH) h = minH;
  if(w > limW) w = limW;
  if(h > limH) h = limH;
  if(keep > w) keep = w;
  if(keep > h) keep = h;
  if(keep < 1) keep = 1;
  if(x > limW - keep) x = limW - keep;          // no se escapa por la derecha
  if(x + w < keep)    x = keep - w;             // ni por la izquierda
  if(y < 0) y = 0;
  int maxY = limH - keep; if(maxY < 0) maxY = 0;
  if(y > maxY) y = maxY;                        // el agarre siempre visible
  return x != ox || y != oy || w != ow || h != oh;
}

// Rectangulo redondeado relleno (esquinas suaves via inset por fila).
// En landscape se recorre por COLUMNA logica (mismo motivo que fillRect: asi
// cada span es memoria contigua). Es la TRANSPUESTA del mismo calculo, con lo
// que la silueta es la misma salvo, como mucho, 1 px en la diagonal de cada
// esquina -- y las dos variantes (opaca y alpha) usan la misma, asi que al
// superponerlas encajan exactamente.
static int rrInset(int k, int len, int r){
  if(k < r){ int d = r - 1 - k; return r - isqrt32(r * r - d * d); }
  if(k >= len - r){ int d = k - (len - r); return r - isqrt32(r * r - d * d); }
  return 0;
}
static void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c){
  if(w <= 0 || h <= 0) return;
  if(r < 0) r = 0;
  if(2 * r > w) r = w / 2;
  if(2 * r > h) r = h / 2;
  if(gLand){
    for(int i = 0; i < w; i++){
      int in = rrInset(i, w, r);
      fillSpanLand(x + i, y + in, h - 2 * in, c);
    }
    return;
  }
  for(int j = 0; j < h; j++){
    int inset = rrInset(j, h, r);
    hLine(x + inset, y + j, w - 2 * inset, c);
  }
}
static void fillRoundRectA(int x, int y, int w, int h, int r, uint16_t c, uint8_t a){
  if(w <= 0 || h <= 0) return;
  if(r < 0) r = 0;
  if(2 * r > w) r = w / 2;
  if(2 * r > h) r = h / 2;
  if(gLand){
    for(int i = 0; i < w; i++){
      int in = rrInset(i, w, r);
      fillSpanLandA(x + i, y + in, h - 2 * in, c, a);
    }
    return;
  }
  for(int j = 0; j < h; j++){
    int inset = rrInset(j, h, r);
    hLineA(x + inset, y + j, w - 2 * inset, c, a);
  }
}
// Borde redondeado (1 px) para tarjetas
static void drawRoundRect(int x, int y, int w, int h, int r, uint16_t c){
  if(2 * r > w) r = w / 2;
  if(2 * r > h) r = h / 2;
  hLine(x + r, y, w - 2 * r, c);
  hLine(x + r, y + h - 1, w - 2 * r, c);
  vLine(x, y + r, h - 2 * r, c);
  vLine(x + w - 1, y + r, h - 2 * r, c);
  // esquinas
  int f = 1 - r, ddx = 1, ddy = -2 * r, xx = 0, yy = r;
  while(xx < yy){
    if(f >= 0){ yy--; ddy += 2; f += ddy; }
    xx++; ddx += 2; f += ddx;
    px(x + r - xx, y + r - yy, c); px(x + w - r - 1 + xx, y + r - yy, c);
    px(x + r - yy, y + r - xx, c); px(x + w - r - 1 + yy, y + r - xx, c);
    px(x + r - xx, y + h - r - 1 + yy, c); px(x + w - r - 1 + xx, y + h - r - 1 + yy, c);
    px(x + r - yy, y + h - r - 1 + xx, c); px(x + w - r - 1 + yy, y + h - r - 1 + xx, c);
  }
}

static void fillCircle(int cx, int cy, int r, uint16_t c){
  if(r <= 0){ px(cx, cy, c); return; }
  for(int dy = -r; dy <= r; dy++){
    int dx = isqrt32(r * r - dy * dy);
    hLine(cx - dx, cy + dy, 2 * dx + 1, c);
  }
}
static void fillCircleA(int cx, int cy, int r, uint16_t c, uint8_t a){
  if(r <= 0){ pxA(cx, cy, c, a); return; }
  for(int dy = -r; dy <= r; dy++){
    int dx = isqrt32(r * r - dy * dy);
    hLineA(cx - dx, cy + dy, 2 * dx + 1, c, a);
  }
}
static void drawCircle(int cx, int cy, int r, uint16_t c){
  int f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
  px(cx, cy + r, c); px(cx, cy - r, c); px(cx + r, cy, c); px(cx - r, cy, c);
  while(x < y){
    if(f >= 0){ y--; ddy += 2; f += ddy; }
    x++; ddx += 2; f += ddx;
    px(cx + x, cy + y, c); px(cx - x, cy + y, c);
    px(cx + x, cy - y, c); px(cx - x, cy - y, c);
    px(cx + y, cy + x, c); px(cx - y, cy + x, c);
    px(cx + y, cy - x, c); px(cx - y, cy - x, c);
  }
}
// anillo de grosor t
static void fillRing(int cx, int cy, int rOut, int t, uint16_t c){
  int rin = rOut - t; if(rin < 0) rin = 0;
  for(int dy = -rOut; dy <= rOut; dy++){
    int dxo = isqrt32(rOut * rOut - dy * dy);
    int inr2 = rin * rin - dy * dy;
    if(inr2 > 0){
      int dxi = isqrt32(inr2);
      hLine(cx - dxo, cy + dy, dxo - dxi, c);
      hLine(cx + dxi + 1, cy + dy, dxo - dxi, c);
    } else {
      hLine(cx - dxo, cy + dy, 2 * dxo + 1, c);
    }
  }
}
static void lineTo(int x0, int y0, int x1, int y1, uint16_t c){
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for(;;){
    px(x0, y0, c);
    if(x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if(e2 >= dy){ err += dy; x0 += sx; }
    if(e2 <= dx){ err += dx; y0 += sy; }
  }
}
// trazo grueso con puntas redondeadas: estampa discos por el segmento
static void strokeSeg(float x0, float y0, float x1, float y1, int rad, uint16_t c){
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  int steps = (int)len + 1;
  for(int i = 0; i <= steps; i++){
    float t = (steps > 0) ? (float)i / steps : 0;
    fillCircle((int)(x0 + dx * t + 0.5f), (int)(y0 + dy * t + 0.5f), rad, c);
  }
}

// ---------------- Primitivas ANTI-ALIASING (bordes suaves) ----------------
// Cobertura por sub-pixel: los bordes se mezclan con alpha en vez de
// dibujarse "duros". Esto da curvas suaves al reloj y a los acentos.
static float distToSeg(float px, float py, float ax, float ay, float bx, float by){
  float dx = bx - ax, dy = by - ay;
  float l2 = dx * dx + dy * dy;
  float t = (l2 > 0.0f) ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0f;
  if(t < 0) t = 0; else if(t > 1) t = 1;
  float qx = ax + t * dx - px, qy = ay + t * dy - py;
  return sqrtf(qx * qx + qy * qy);
}
static void fillCircleAA(float cx, float cy, float r, uint16_t col){
  int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
  int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
  for(int y = y0; y <= y1; y++) for(int x = x0; x <= x1; x++){
    float dx = x - cx, dy = y - cy;
    float cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
    if(cov <= 0) continue; if(cov > 1) cov = 1;
    pxA(x, y, col, (uint8_t)(cov * 255));
  }
}
// Segmento grueso con puntas redondeadas y bordes suaves (para el reloj)
static void strokeSegAA(float x0, float y0, float x1, float y1, float rad, uint16_t col){
  int minx = (int)floorf(fminf(x0, x1) - rad - 1), maxx = (int)ceilf(fmaxf(x0, x1) + rad + 1);
  int miny = (int)floorf(fminf(y0, y1) - rad - 1), maxy = (int)ceilf(fmaxf(y0, y1) + rad + 1);
  for(int y = miny; y <= maxy; y++) for(int x = minx; x <= maxx; x++){
    float cov = rad + 0.5f - distToSeg((float)x, (float)y, x0, y0, x1, y1);
    if(cov <= 0) continue; if(cov > 1) cov = 1;
    pxA(x, y, col, (uint8_t)(cov * 255));
  }
}

// ---------------- Fondo (wallpaper) ----------------
// Degradado diagonal 3 paradas: verde (arriba-dcha) -> azul (centro)
// -> violeta (abajo-izq), igual que tus imagenes. Opcional: blobs.
// El degradado se repinta ENTERO en cada renderHome()/renderLock(), o sea una
// vez por minuto. La version anterior hacia, por cada uno de los 384.000
// pixeles: 2 divisiones + 1 mix565 (que a su vez hacia 3 divisiones mas).
// Dos observaciones lo tiran casi todo abajo:
//   1) 'ty' NO depende de x, pero se recalculaba 480 veces por fila.
//   2) el color solo depende de t = (tx+ty)/2, que vive en [0,255]: caben
//      los 256 colores posibles en una tabla y el bucle interior pasa a ser
//      una simple consulta. Coste: 992 B de RAM estatica, una sola vez.
// El resultado en pantalla es BIT A BIT el mismo que antes.
static void drawWallpaper(uint16_t* buf, bool blobs){
  const uint16_t green  = rgb565(80, 224, 74);    // arriba-derecha
  const uint16_t blue   = rgb565(40, 150, 245);   // centro
  const uint16_t purple = rgb565(112, 46, 230);   // abajo-izquierda
  static uint16_t gradLut[256];                   // color por t          (512 B)
  static uint8_t  txLut[SCR_W];                   // rampa horizontal      (480 B)
  static bool     lutReady = false;
  if(!lutReady){
    for(int t = 0; t < 256; t++)
      gradLut[t] = (t < 128) ? mix565(purple, blue,  (uint8_t)(t * 2))
                             : mix565(blue,   green, (uint8_t)((t - 128) * 2));
    for(int x = 0; x < SCR_W; x++) txLut[x] = (uint8_t)((x * 255) / (SCR_W - 1));
    lutReady = true;
  }
  uint16_t* old = gBuf; setBuf(buf);
  for(int y = 0; y < SCR_H; y++){
    // t=1 arriba-derecha, t=0 abajo-izquierda. 'ty' es invariante en x.
    int ty = ((SCR_H - 1 - y) * 255) / (SCR_H - 1);
    uint16_t* row = buf + (size_t)y * SCR_W;
    for(int x = 0; x < SCR_W; x++) row[x] = gradLut[(txLut[x] + ty) >> 1];
  }
  if(blobs){
    // dos manchas suaves mas claras (como el escritorio de tus imagenes)
    fillCircleA(360, 150, 220, rgb565(150, 235, 180), 60);
    fillCircleA(90,  560, 260, rgb565(150, 160, 240), 55);
  }
  setBuf(old);
}

// #############################################################
// ##  LIQUID GLASS (aproximacion iOS 26 en software)
// ##  Panel reutilizable: desenfoca el fondo real (box-blur),
// ##  tinte sutil y gradiente de grosor.
// #############################################################
static bool uiGlass = false;               // estilo activo (togglea en Ajustes)
// Modo de apariencia (Ajustes -> Pantalla -> Modo de apariencia). true = Modo
// oscuro (comportamiento de siempre, por defecto). false = Modo claro.
// ALCANCE: por ahora retematiza la app Ajustes (donde vive el selector),
// que es donde vive PAGE_BG y los colores de tarjeta/texto de esa app (ver
// mas abajo, junto a PAGE_BG). Inicio, Bloqueo, notificaciones, ventanas y
// el resto de apps tienen su propia paleta oscura, muy afinada a mano, y no
// se tocan en esta pasada -- retematizarlas de verdad necesita un diseno de
// colores propio por pantalla, no un cambio mecanico.
static bool gDark = true;
static int  gIconStyle = 0;                // estilo de iconos: 0 = Plano, 1 = Vidrio (fondo Liquid Glass en drawAppIcon)
static uint16_t* glassBuf = NULL;          // scratch de region (PSRAM)
// NOTA: aqui vivia 'wallBuf' ("wallpaper limpio para animar el brillo"). Era
// memoria muerta: se reservaban 768 KB y se copiaban enteros en CADA
// renderHome(), pero NINGUNA funcion lo leia jamas. Eliminado: -768 KB de
// PSRAM y -768 KB de memcpy por cada repintado del escritorio.
// Linea temporal para el blur. Se indexa por ANCHO (pasada horizontal) y
// por ALTO (pasada vertical) del panel de turno, asi que debe cubrir el
// mayor de los dos lados de la pantalla, no solo SCR_H, o un panel mas
// ancho que alto desbordaria este buffer y corromperia memoria vecina.
static uint16_t  glLine[(SCR_W > SCR_H ? SCR_W : SCR_H)];

static inline void un565(uint16_t c, int &r, int &g, int &b){ r = (c >> 11) & 0x1F; g = (c >> 5) & 0x3F; b = c & 0x1F; }
static inline uint16_t pk565(int r, int g, int b){ return (uint16_t)((r << 11) | (g << 5) | b); }

// Luma aproximada directamente en dominio 565, sin float y sin multiplicacion
// real: el compilador reduce *5 a (x<<2)+x y *2 a x<<1. Devuelve 0..266 en vez
// de 0..255, y NO se normaliza a proposito: solo se usa para comparar dos lumas
// entre si (la del fondo contra la del tinte), y ambas viven en este mismo
// dominio, asi que la resta es consistente. El error medio contra la luma
// perceptual real es de ~5 niveles sobre 255, de sobra para decidir cuanto
// tinte aplicar. Reutiliza un565 en vez de repetir el desempaquetado.
static inline int glassLuma(uint16_t c){
  int r, g, b; un565(c, r, g, b);
  return ((r + g) * 5 + b * 2) >> 1;
}

// box-blur (suma corrediza) sobre glassBuf de ancho w, alto h
static void glassBlur(int w, int h, int R){
  int r, g, b;
  for(int j = 0; j < h; j++){                         // horizontal
    uint16_t* row = glassBuf + (size_t)j * w;
    for(int i = 0; i < w; i++) glLine[i] = row[i];
    int sr = 0, sg = 0, sb = 0, win = 0;
    for(int i = 0; i <= R && i < w; i++){ un565(glLine[i], r, g, b); sr += r; sg += g; sb += b; win++; }
    for(int i = 0; i < w; i++){
      row[i] = pk565(sr / win, sg / win, sb / win);
      int add = i + R + 1, rem = i - R;
      if(add < w){ un565(glLine[add], r, g, b); sr += r; sg += g; sb += b; win++; }
      if(rem >= 0){ un565(glLine[rem], r, g, b); sr -= r; sg -= g; sb -= b; win--; }
    }
  }
  for(int i = 0; i < w; i++){                          // vertical
    for(int j = 0; j < h; j++) glLine[j] = glassBuf[(size_t)j * w + i];
    int sr = 0, sg = 0, sb = 0, win = 0;
    for(int j = 0; j <= R && j < h; j++){ un565(glLine[j], r, g, b); sr += r; sg += g; sb += b; win++; }
    for(int j = 0; j < h; j++){
      glassBuf[(size_t)j * w + i] = pk565(sr / win, sg / win, sb / win);
      int add = j + R + 1, rem = j - R;
      if(add < h){ un565(glLine[add], r, g, b); sr += r; sg += g; sb += b; win++; }
      if(rem >= 0){ un565(glLine[rem], r, g, b); sr -= r; sg -= g; sb -= b; win--; }
    }
  }
}
static int glInset(int j, int h, int rad){
  if(j < rad){ int dy = rad - 1 - j; return rad - isqrt32(rad * rad - dy * dy); }
  if(j >= h - rad){ int dy = j - (h - rad); return rad - isqrt32(rad * rad - dy * dy); }
  return 0;
}
// Panel Liquid Glass reutilizable (estatico: blur + tinte + gradiente).
// "Ex" permite fijar el radio del box-blur (blurR). glassBlur() es una suma
// corrediza O(w*h) que NO depende de blurR (ver mas arriba), asi que subir
// blurR no cuesta rendimiento extra -- solo cambia cuanto se difumina el
// fondo. drawLiquidGlassPanel() de siempre (abajo) sigue llamando a esta con
// blurR=6, es decir: mismo blur que antes en los ~19 sitios existentes que ya
// la usan. Se penso para el panel rapido, que quiere un vidrio mas
// "esmerilado" que el resto del sistema.
//
// TINTE ADAPTATIVO: el porcentaje de mezcla del tinte ya no es el 58 fijo de
// antes; se mueve dentro de [GLASS_TINT_MIN..GLASS_TINT_MAX] segun cuanto
// difiera la luminancia del tinte respecto a la del fondo que quedo debajo del
// panel. Esto SI cambia el aspecto de los ~19 sitios existentes (cambio pedido
// y aprobado a proposito, no un efecto colateral): el blur y la geometria son
// los de siempre, solo respira el tinte. GLASS_TINT_BASE es el valor historico
// y queda como respaldo defensivo por si no se pudo tomar ninguna muestra.
// GLASS_TINT_DIFF_MAX es potencia de dos a proposito: convierte la division
// del mapeo en un desplazamiento.
static const uint8_t GLASS_TINT_BASE = 58, GLASS_TINT_MIN = 46, GLASS_TINT_MAX = 70;
static const int     GLASS_TINT_DIFF_MAX = 128;
static void drawLiquidGlassPanelEx(int x, int y, int w, int h, int rad, uint16_t tint, int blurR){
  // GUARDA DE LANDSCAPE (Modo PC). Esta funcion lee y escribe el buffer con
  // indexacion VERTICAL directa (gBuf + (y+j)*SCR_W + x), asi que ignora por
  // completo la rotacion de gLand. En Modo PC cada drawAppIcon() de estilo
  // "Vidrio" pintaba su panel en coordenadas rotadas: por eso aparecian paneles
  // de cristal FANTASMA flotando por el escritorio (una columna a la derecha =
  // los iconos de la barra de tareas, una fila abajo = los del escritorio).
  // fillRoundRectA() si respeta la rotacion (pasa por putPhys), asi que en
  // landscape se usa el panel plano tintado: mismo sitio, sin fantasmas. El
  // cristal propio de Modo PC lo dibuja pcGlassPanel(), que si es landscape-safe.
  if(gLand){ fillRoundRectA(x, y, w, h, rad, tint, 210); return; }
  if(!glassBuf) glassBuf = (uint16_t*)heap_caps_malloc((size_t)SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!glassBuf){ fillRoundRectA(x, y, w, h, rad, tint, 210); return; }   // fallback sin PSRAM
  if(x < 0){ w += x; x = 0; } if(y < 0){ h += y; y = 0; }
  if(x + w > SCR_W) w = SCR_W - x; if(y + h > SCR_H) h = SCR_H - y;
  if(w <= 0 || h <= 0) return;
  if(2 * rad > w) rad = w / 2; if(2 * rad > h) rad = h / 2;
  // Sampleo del fondo para el tinte adaptativo. Se engancha al loop de memcpy
  // que YA existe -- sin pasada extra, sin buffer nuevo, solo dos acumuladores
  // en stack -- y lee 1 de cada 4 filas por 1 de cada 8 columnas, o sea 1/32 de
  // los pixeles. Se mide ANTES del blur, sobre la region recien copiada. Se
  // deja el memcpy en bloque en vez de acumular pixel a pixel porque copiar por
  // bytes seria mas caro que el propio muestreo disperso.
  uint32_t lumaSum = 0; int lumaN = 0;
  for(int j = 0; j < h; j++){
    uint16_t* row = glassBuf + (size_t)j * w;
    memcpy(row, gBuf + (size_t)(y + j) * SCR_W + x, w * 2);
    if((j & 3) == 0) for(int i = 0; i < w; i += 8){ lumaSum += (uint32_t)glassLuma(row[i]); lumaN++; }
  }
  // Cuanto MAS se parecen en luminancia el tinte y el fondo, MENOS tinte se
  // aplica: si ya comparten tono, cargarle tinte solo lo aplana en un bloque
  // liso, y conviene dejar ver el fondo. Cuanto mas difieren, mas tinte, para
  // que el panel afirme su propio color en vez de lavarse contra el fondo. Un
  // solo valor absoluto cubre los cuatro casos sin ramas: tinte oscuro sobre
  // fondo oscuro y tinte claro sobre fondo claro dan diferencia chica (poco
  // tinte); los dos cruzados dan diferencia grande (mas tinte). Todo esto se
  // calcula UNA vez por panel, no por pixel.
  uint8_t tintMix = GLASS_TINT_BASE;
  if(lumaN > 0){
    int dif = (int)(lumaSum / (uint32_t)lumaN) - glassLuma(tint);
    if(dif < 0) dif = -dif;
    if(dif > GLASS_TINT_DIFF_MAX) dif = GLASS_TINT_DIFF_MAX;
    tintMix = (uint8_t)(GLASS_TINT_MIN + (dif * (GLASS_TINT_MAX - GLASS_TINT_MIN)) / GLASS_TINT_DIFF_MAX);
  }
  glassBlur(w, h, blurR);
  for(int j = 0; j < h; j++){
    int yy = y + j; if(yy < gClipY0 || yy > gClipY1) continue;   // respeta la banda de recorte
    int ins = glInset(j, h, rad);
    uint16_t* src = glassBuf + (size_t)j * w;
    uint16_t* dst = gBuf + (size_t)yy * SCR_W + x;
    float fj = (float)j;
    for(int i = ins; i < w - ins; i++){
      uint16_t out = mix565(src[i], tint, tintMix);   // tinte adaptativo (ver arriba), antes fijo en 58
      if(fj < h * 0.45f) out = mix565(out, rgb565(255,255,255), (uint8_t)((1.0f - fj / (h * 0.45f)) * 26));
      else               out = mix565(out, rgb565(0,0,0), (uint8_t)(((fj - h * 0.45f) / (h * 0.55f)) * 30));
      dst[i] = out;
    }
    // Highlight direccional (luz simulada desde la esquina superior-izquierda):
    // mismo bcol de siempre por fila (blanco arriba, negro abajo), pero la
    // FUERZA de la mezcla se pondera distinto por lado en vez de usar 130 fijo
    // en los dos bordes. Asi las 4 esquinas quedan con peso propio (arriba-
    // izq. blanco fuerte, arriba-der. blanco tenue, abajo-izq. sombra tenue,
    // abajo-der. sombra fuerte) en vez de una franja horizontal identica en
    // ambos bordes. GLASS_CORNER_STRONG/WEAK promedian 130 (el valor de antes)
    // para no cambiar el "peso" total del borde, solo redistribuirlo: el delta
    // es de +-20% sobre ese 130. Sigue siendo funcion de j nada mas: mismos 2
    // pixeles por fila de siempre. Si hay que retocar la intensidad, mover los
    // dos valores de forma simetrica alrededor de 130 (STRONG = 130 + d,
    // WEAK = 130 - d) para que el borde no gane ni pierda peso total.
    const uint8_t GLASS_CORNER_STRONG = 156, GLASS_CORNER_WEAK = 104;
    bool topZone = (j < h / 2);
    uint8_t sL = topZone ? GLASS_CORNER_STRONG : GLASS_CORNER_WEAK;   // izquierda: blanco fuerte / sombra tenue
    uint8_t sR = topZone ? GLASS_CORNER_WEAK   : GLASS_CORNER_STRONG; // derecha: blanco tenue / sombra fuerte
    uint16_t bcol = (j < 3) ? rgb565(255,255,255) : (j < h / 2 ? rgb565(205,214,228) : rgb565(22,28,40));
    dst[ins] = mix565(dst[ins], bcol, sL);
    dst[w - 1 - ins] = mix565(dst[w - 1 - ins], bcol, sR);
  }
}
static void drawLiquidGlassPanel(int x, int y, int w, int h, int rad, uint16_t tint){
  drawLiquidGlassPanelEx(x, y, w, h, rad, tint, 6);   // blur original, sin cambios, para el resto del sistema
}

// #############################################################
// ##  TARJETA LIQUID GLASS CACHEADA (fondos PLANOS)
// ##  ------------------------------------------------------
// ##  Por que existe: drawLiquidGlassPanel copia la region, la desenfoca y la
// ##  mezcla. Con ocho tarjetas por cuadro eso era demasiado caro para seguir
// ##  al dedo, asi que las listas con scroll (Ajustes, Ajustes del teclado)
// ##  DESACTIVABAN el vidrio mientras se arrastraba y lo devolvian al soltar:
// ##  de ahi que "al hacer scroll el material perdiera el desenfoque y las
// ##  transparencias".
// ##  La observacion que lo arregla: sobre un fondo de color UNIFORME el
// ##  box-blur devuelve ese mismo color, asi que el resultado del panel no
// ##  depende de DONDE se dibuje -- solo de (w, h, radio, tinte, color de
// ##  fondo). Se calcula UNA vez, se guarda y a partir de ahi cada tarjeta es
// ##  un memcpy por fila. El resultado en pantalla es identico pixel a pixel
// ##  al de la version cara, asi que el vidrio ya puede quedarse encendido
// ##  durante todo el desplazamiento.
// ##  Si el usuario tiene el Liquid Glass DESACTIVADO no se llama aqui
// ##  siquiera: cada llamante conserva su rama plana de siempre.
// #############################################################
#define GLC_MAX_H 96                       // alto maximo de tarjeta cacheable (las filas miden ~52-62)
static uint16_t* glcScratch = NULL;        // lienzo de trabajo (stride SCR_W, GLC_MAX_H filas)
static uint16_t* glcCard    = NULL;        // tarjeta ya resuelta (w x h compactos)
static int       glcW = 0, glcH = 0, glcRad = -1;
static uint16_t  glcTint = 0, glcBg = 0;
static bool      glcValid = false;

static bool glcBuild(int w, int h, int rad, uint16_t tint, uint16_t bg){
  if(!glcScratch) glcScratch = (uint16_t*)heap_caps_malloc((size_t)SCR_W * GLC_MAX_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!glcCard)    glcCard    = (uint16_t*)heap_caps_malloc((size_t)SCR_W * GLC_MAX_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!glcScratch || !glcCard) return false;
  for(int j = 0; j < h; j++){                       // fondo plano: la premisa de todo esto
    uint16_t* r = glcScratch + (size_t)j * SCR_W;
    for(int i = 0; i < w; i++) r[i] = bg;
  }
  uint16_t* oBuf = gBuf;                            // gBuf directo, no setBuf: no debe desviarse a un lienzo de DeX
  int oc0 = gClipY0, oc1 = gClipY1, ox0 = gClipX0, ox1 = gClipX1;
  gBuf = glcScratch;
  gClipY0 = 0; gClipY1 = h - 1; gClipX0 = 0; gClipX1 = SCR_W - 1;
  drawLiquidGlassPanel(0, 0, w, h, rad, tint);
  gBuf = oBuf; gClipY0 = oc0; gClipY1 = oc1; gClipX0 = ox0; gClipX1 = ox1;
  for(int j = 0; j < h; j++)
    memcpy(glcCard + (size_t)j * w, glcScratch + (size_t)j * SCR_W, (size_t)w * 2);
  glcW = w; glcH = h; glcRad = rad; glcTint = tint; glcBg = bg; glcValid = true;
  return true;
}
// Tarjeta de vidrio sobre fondo plano. Cae al panel de siempre (mismo dibujo,
// solo mas caro) si el tamano no cabe en la cache o si estamos en landscape,
// donde la indexacion directa no vale.
static void drawGlassCardFlat(int x, int y, int w, int h, int rad, uint16_t tint, uint16_t bg){
  if(gLand || w <= 0 || h <= 0 || w > SCR_W || h > GLC_MAX_H){
    drawLiquidGlassPanel(x, y, w, h, rad, tint); return;
  }
  if(!glcValid || glcW != w || glcH != h || glcRad != rad || glcTint != tint || glcBg != bg){
    if(!glcBuild(w, h, rad, tint, bg)){ drawLiquidGlassPanel(x, y, w, h, rad, tint); return; }
  }
  for(int j = 0; j < h; j++){
    int yy = y + j;
    if(yy < 0 || yy >= SCR_H || yy < gClipY0 || yy > gClipY1) continue;
    int xs = x, xe = x + w - 1, sx = 0;
    if(xs < gClipX0){ sx = gClipX0 - xs; xs = gClipX0; }
    if(xe > gClipX1) xe = gClipX1;
    if(xs < 0){ sx += -xs; xs = 0; }
    if(xe > SCR_W - 1) xe = SCR_W - 1;
    if(xs > xe) continue;
    memcpy(gBuf + (size_t)yy * SCR_W + xs, glcCard + (size_t)j * w + sx, (size_t)(xe - xs + 1) * 2);
  }
}
// Wallpaper desenfocado reutilizable (fondo del desbloqueo y de Recientes, estilo iOS)
static uint16_t* blurBg = NULL;
static void ensureBlurBg(){
  if(blurBg) return;
  blurBg = (uint16_t*)heap_caps_malloc((size_t)SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!blurBg) return;
  uint16_t* old = gBuf;
  // Este buffer se compone UNA SOLA VEZ en toda la sesion (el early-return de
  // arriba). Si el primer llamante llega con gLand=true o con el recorte
  // estrechado -- p.ej. saliendo del Modo Kiosco desde Juegos, que deja
  // gLand=true en cada frame-- el fillRectA de abajo se aplicaria girado o a
  // media pantalla y el fondo quedaria roto PARA SIEMPRE. Se fuerza portrait y
  // recorte completo aqui, no en cada llamante.
  bool wl = gLand; gLand = false;
  int sx0 = gClipX0, sx1 = gClipX1, sy0 = gClipY0, sy1 = gClipY1;
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  drawWallpaper(blurBg, true);
  setBuf(blurBg);
  fillRectA(0, 0, SCR_W, SCR_H, rgb565(8,10,18), 70);
  setBuf(old);
  gLand = wl;
  gClipX0 = sx0; gClipX1 = sx1; gClipY0 = sy0; gClipY1 = sy1;
}

// #############################################################
// ##  TIPOGRAFIA + RELOJ VECTORIAL + TRIANGULOS  (original)
// #############################################################

// Fuente 5x7 (column-major, bit0 = arriba). ASCII 0x20..0x7E.
static const uint8_t FONT5x7[95][5] = {
  {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
  {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
  {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
  {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
  {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
  {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
  {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
  {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
  {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
  {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
  {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
  {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
  {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
  {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
  {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},
  {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
  {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
  {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
  {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
  {0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
  {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
  {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
  {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
  {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
  {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
  {0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08}
};
// Glifos extra: ¿ (invertido) y ¡ (invertido)
static const uint8_t GLYPH_INVQ[5]    = {0x30,0x48,0x45,0x40,0x20};
static const uint8_t GLYPH_INVEXCL[5] = {0x00,0x00,0x7D,0x00,0x00};

// Tipos de acento (se dibujan sobre/bajo el glifo base)
enum { ACC_NONE=0, ACC_ACUTE, ACC_GRAVE, ACC_TILDE, ACC_DIAER, ACC_CIRC, ACC_CED };

// Mapea un codepoint Unicode a (glifo base, acento). base 1=¿, 2=¡.
static void mapCP(uint32_t cp, uint8_t &base, uint8_t &acc){
  acc = ACC_NONE;
  if(cp < 0x80){ base = (cp >= 0x20 && cp <= 0x7E) ? (uint8_t)cp : '?'; return; }
  switch(cp){
    case 0xE1: base='a'; acc=ACC_ACUTE; return;   case 0xE9: base='e'; acc=ACC_ACUTE; return;
    case 0xED: base='i'; acc=ACC_ACUTE; return;   case 0xF3: base='o'; acc=ACC_ACUTE; return;
    case 0xFA: base='u'; acc=ACC_ACUTE; return;   case 0xFC: base='u'; acc=ACC_DIAER; return;
    case 0xF1: base='n'; acc=ACC_TILDE; return;
    case 0xC1: base='A'; acc=ACC_ACUTE; return;   case 0xC9: base='E'; acc=ACC_ACUTE; return;
    case 0xCD: base='I'; acc=ACC_ACUTE; return;   case 0xD3: base='O'; acc=ACC_ACUTE; return;
    case 0xDA: base='U'; acc=ACC_ACUTE; return;   case 0xD1: base='N'; acc=ACC_TILDE; return;
    case 0xDC: base='U'; acc=ACC_DIAER; return;
    case 0xE0: base='a'; acc=ACC_GRAVE; return;   case 0xE8: base='e'; acc=ACC_GRAVE; return;
    case 0xEC: base='i'; acc=ACC_GRAVE; return;   case 0xF2: base='o'; acc=ACC_GRAVE; return;
    case 0xF9: base='u'; acc=ACC_GRAVE; return;
    case 0xE2: base='a'; acc=ACC_CIRC; return;    case 0xEA: base='e'; acc=ACC_CIRC; return;
    case 0xEE: base='i'; acc=ACC_CIRC; return;    case 0xF4: base='o'; acc=ACC_CIRC; return;
    case 0xFB: base='u'; acc=ACC_CIRC; return;
    case 0xE3: base='a'; acc=ACC_TILDE; return;   case 0xF5: base='o'; acc=ACC_TILDE; return;
    case 0xE7: base='c'; acc=ACC_CED; return;     case 0xC7: base='C'; acc=ACC_CED; return;
    case 0xBF: base=1; return;                    case 0xA1: base=2; return;
    default:   base='?'; return;
  }
}

static void drawGlyphRaw(int x, int y, uint8_t base, int s, uint16_t col){
  const uint8_t* g;
  if(base == 1) g = GLYPH_INVQ;
  else if(base == 2) g = GLYPH_INVEXCL;
  else { int idx = (int)base - 0x20; if(idx < 0 || idx > 94) return; g = FONT5x7[idx]; }
  for(int c = 0; c < 5; c++){
    uint8_t bits = g[c];
    for(int r = 0; r < 7; r++){
      if(bits & (1 << r)){
        if(s == 1) px(x + c, y + r, col);
        else fillRect(x + c * s, y + r * s, s, s, col);
      }
    }
  }
}
static void drawAccent(int x, int y, int s, uint8_t acc, uint16_t col){
  float cx = x + 2.5f * s;
  float r = s * 0.55f; if(r < 1.0f) r = 1.0f;
  switch(acc){
    case ACC_ACUTE: strokeSegAA(cx - s, y - s, cx + s, y - 3 * s, r, col); break;
    case ACC_GRAVE: strokeSegAA(cx - s, y - 3 * s, cx + s, y - s, r, col); break;
    case ACC_CIRC:  strokeSegAA(cx - 1.6f * s, y - s, cx, y - 3 * s, r, col);
                    strokeSegAA(cx, y - 3 * s, cx + 1.6f * s, y - s, r, col); break;
    case ACC_TILDE: strokeSegAA(cx - 2 * s, y - 2 * s, cx - 0.4f * s, y - 2.9f * s, r, col);
                    strokeSegAA(cx - 0.4f * s, y - 2.9f * s, cx + 0.6f * s, y - 1.9f * s, r, col);
                    strokeSegAA(cx + 0.6f * s, y - 1.9f * s, cx + 2 * s, y - 2.7f * s, r, col); break;
    case ACC_DIAER: fillCircleAA(cx - s, y - 2 * s, r, col);
                    fillCircleAA(cx + s, y - 2 * s, r, col); break;
    case ACC_CED:   strokeSegAA(cx, y + 7 * s - s, cx - 1.2f * s, y + 7 * s + 0.6f * s, r, col); break;
  }
}

// Glifo del cuerpo con SUPERSAMPLING (3x3) -> bordes suaves.
// A tamano 1 (etiquetas diminutas) se dibuja nitido para no emborronar.
static void drawGlyphSmooth(int x, int y, uint8_t base, int s, uint16_t col, uint8_t alpha){
  const uint8_t* g;
  if(base == 1) g = GLYPH_INVQ;
  else if(base == 2) g = GLYPH_INVEXCL;
  else { int idx = (int)base - 0x20; if(idx < 0 || idx > 94) return; g = FONT5x7[idx]; }
  if(s <= 1){
    for(int c = 0; c < 5; c++){ uint8_t bits = g[c];
      for(int r = 0; r < 7; r++) if(bits & (1 << r)) pxA(x + c, y + r, col, alpha); }
    return;
  }
  const int SS = 3;
  int W = 5 * s, H = 7 * s;
  for(int ty = 0; ty < H; ty++){
    for(int tx = 0; tx < W; tx++){
      int cov = 0;
      for(int sy = 0; sy < SS; sy++) for(int sx = 0; sx < SS; sx++){
        int ix = (int)((tx + (sx + 0.5f) / SS) / s);
        int iy = (int)((ty + (sy + 0.5f) / SS) / s);
        if(ix >= 0 && ix < 5 && iy >= 0 && iy < 7 && (g[ix] & (1 << iy))) cov++;
      }
      if(cov) pxA(x + tx, y + ty, col, (uint8_t)((cov * alpha) / (SS * SS)));
    }
  }
}

// Ancho de texto en px (para centrar). Cada glifo avanza 6*s.

// #############################################################
// ##  FUENTE OUTFIT antialiased 4bpp  (reemplaza al 5x7 POR DEBAJO)
// ##  Mismas firmas publicas: drawText/drawTextC/drawTextR/textW.
// #############################################################
// Fuente Outfit-Regular antialiased 4bpp (generada). NO editar a mano.
#define FONT_LINEH 51
#define FONT_ASC 40
static const FGlyph FG[127] = {
  {0,0,0,0,8,0},{5,30,3,10,10,0},{12,10,2,11,16,90},{21,29,2,11,26,150},{19,37,2,7,24,469},{22,29,2,11,26,839},
  {24,29,2,11,26,1158},{5,10,2,11,9,1506},{10,36,2,9,12,1536},{10,36,1,9,12,1716},{16,17,2,9,20,1896},{17,19,2,17,22,2032},
  {6,11,2,35,11,2203},{13,4,3,27,18,2236},{5,5,3,35,12,2264},{16,32,0,10,16,2279},{23,29,2,11,26,2535},{10,29,1,11,14,2883},
  {19,29,1,11,22,3028},{19,29,1,11,22,3318},{22,29,1,11,24,3608},{20,29,0,11,22,3927},{19,29,2,11,23,4217},{18,29,1,11,21,4507},
  {19,29,2,11,22,4768},{20,29,2,11,23,5058},{5,18,3,22,11,5348},{6,24,2,22,11,5402},{17,19,2,17,22,5474},{17,13,2,20,22,5645},
  {17,19,2,17,22,5762},{18,30,1,10,20,5933},{26,27,2,17,30,6203},{27,29,1,11,28,6554},{20,29,3,11,25,6960},{25,29,2,11,28,7250},
  {24,29,3,11,30,7627},{19,29,3,11,24,7975},{18,29,3,11,23,8265},{27,29,2,11,31,8526},{22,29,3,11,28,8932},{4,29,3,11,10,9251},
  {16,29,1,11,20,9309},{23,29,3,11,27,9541},{18,29,3,11,22,9889},{27,29,3,11,34,10150},{22,29,3,11,28,10556},{28,29,2,11,32,10875},
  {19,29,3,11,24,11281},{30,31,2,11,33,11571},{21,29,3,11,25,12036},{19,29,1,11,22,12355},{23,29,1,11,25,12645},{22,29,3,11,27,12993},
  {26,29,1,11,28,13312},{37,29,1,11,39,13689},{26,29,1,11,28,14240},{25,29,0,11,27,14617},{20,29,2,11,23,14994},{9,33,3,11,14,15284},
  {16,32,0,10,16,15449},{9,33,1,11,14,15705},{14,11,2,10,18,15870},{19,4,1,41,20,15947},{9,9,1,9,11,15987},{19,20,1,20,23,16032},
  {19,29,3,11,23,16232},{18,20,1,20,20,16522},{19,29,1,11,23,16702},{19,20,1,20,21,16992},{18,30,0,10,16,17192},{19,28,1,20,23,17462},
  {17,29,3,11,22,17742},{5,29,2,11,9,18003},{13,37,-5,11,9,18090},{17,29,3,11,20,18349},{4,29,3,11,9,18610},{29,20,3,20,34,18668},
  {17,20,3,20,22,18968},{20,20,1,20,23,19148},{19,28,3,20,23,19348},{20,28,1,20,23,19628},{14,20,3,20,17,19908},{16,20,0,20,17,20048},
  {14,28,0,12,15,20208},{16,20,2,20,21,20404},{20,20,0,20,20,20564},{30,20,0,20,30,20764},{20,20,0,20,20,21064},{20,28,0,20,21,21264},
  {16,20,1,20,18,21544},{11,33,1,11,13,21704},{4,37,4,8,11,21902},{11,33,1,11,13,21976},{18,6,2,23,22,22174},{19,31,1,9,23,22228},
  {19,31,1,9,21,22538},{9,32,0,8,9,22848},{20,31,1,9,23,23008},{16,32,2,8,21,23318},{16,28,2,12,21,23574},{17,29,3,11,22,23798},
  {27,40,1,0,28,24059},{19,40,3,0,24,24619},{9,40,0,0,10,25019},{28,40,2,0,32,25219},{22,40,3,0,27,25779},{22,37,3,3,27,26219},
  {22,38,3,2,28,26626},{19,31,1,9,23,27044},{19,31,1,9,21,27354},{9,31,0,9,9,27664},{20,31,1,9,23,27819},{16,31,2,9,21,28129},
  {19,31,1,9,23,28377},{19,31,1,9,21,28687},{15,31,-3,9,9,28997},{20,31,1,9,23,29245},{16,31,2,9,21,29555},{19,29,1,11,23,29803},
  {20,29,1,11,23,30093},{18,29,1,20,20,30383},{25,38,2,11,28,30644},{16,28,2,20,20,31138},{5,28,3,20,11,31362},{12,13,2,11,16,31446},
  {5,5,0,23,5,31524},
};
static const uint8_t FBM[31539] = {
  15,255,176,15,255,176,15,255,176,15,255,176,15,255,176,15,255,176,15,255,
  176,15,255,176,15,255,176,15,255,176,15,255,176,15,255,176,15,255,176,15,
  255,176,15,255,176,15,255,176,15,255,176,15,255,176,15,255,176,15,255,176,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,44,252,32,207,255,
  192,255,255,240,207,255,192,44,252,32,63,255,160,10,255,244,47,255,144,9,
  255,242,15,255,128,8,255,241,14,255,96,6,255,240,13,255,80,5,255,208,
  12,255,64,4,255,192,11,255,48,3,255,176,9,255,16,1,255,144,8,255,
  0,0,255,128,7,254,0,0,239,112,0,0,0,95,250,0,0,13,255,32,
  0,0,0,0,127,248,0,0,31,255,0,0,0,0,0,159,246,0,0,63,
  253,0,0,0,0,0,207,244,0,0,95,251,0,0,0,0,0,239,242,0,
  0,127,249,0,0,0,0,1,255,240,0,0,159,246,0,0,0,0,3,255,
  192,0,0,191,244,0,0,0,0,5,255,160,0,0,223,242,0,0,9,153,
  156,255,217,153,153,255,249,153,144,15,255,255,255,255,255,255,255,255,255,240,
  15,255,255,255,255,255,255,255,255,255,240,15,255,255,255,255,255,255,255,255,
  255,240,0,0,31,254,0,0,9,255,96,0,0,0,0,63,252,0,0,11,
  255,64,0,0,0,0,95,250,0,0,14,255,32,0,0,0,0,143,248,0,
  0,31,255,0,0,0,0,0,175,246,0,0,63,253,0,0,0,255,255,255,
  255,255,255,255,255,255,255,0,255,255,255,255,255,255,255,255,255,255,0,255,
  255,255,255,255,255,255,255,255,255,0,153,155,255,233,153,153,239,250,153,153,
  0,0,6,255,160,0,0,239,242,0,0,0,0,8,255,128,0,1,255,240,
  0,0,0,0,10,255,96,0,3,255,192,0,0,0,0,12,255,48,0,5,
  255,160,0,0,0,0,14,255,16,0,7,255,128,0,0,0,0,31,254,0,
  0,10,255,96,0,0,0,0,79,252,0,0,12,255,64,0,0,0,0,111,
  250,0,0,14,255,32,0,0,0,0,0,0,0,13,255,0,0,0,0,0,
  0,0,0,13,255,0,0,0,0,0,0,0,0,13,255,0,0,0,0,0,
  0,0,0,13,255,0,0,0,0,0,0,0,57,207,255,200,48,0,0,0,
  0,43,255,255,255,255,250,16,0,0,2,239,255,255,255,255,255,211,0,0,
  13,255,255,222,255,239,255,254,32,0,111,255,228,13,255,6,239,254,48,0,
  191,255,64,13,255,0,61,227,0,0,239,253,0,13,255,0,2,48,0,0,
  255,251,0,13,255,0,0,0,0,0,239,253,0,13,255,0,0,0,0,0,
  207,255,64,13,255,0,0,0,0,0,143,255,228,13,255,0,0,0,0,0,
  30,255,255,174,255,0,0,0,0,0,5,255,255,255,255,16,0,0,0,0,
  0,77,255,255,255,233,48,0,0,0,0,1,125,255,255,255,249,16,0,0,
  0,0,0,94,255,255,255,193,0,0,0,0,0,13,255,239,255,251,0,0,
  0,0,0,13,255,24,255,255,64,0,0,0,0,13,255,0,143,255,160,0,
  0,0,0,13,255,0,30,255,208,0,0,0,0,13,255,0,12,255,240,0,
  18,0,0,13,255,0,11,255,240,1,204,16,0,13,255,0,14,255,208,29,
  255,193,0,13,255,0,127,255,160,62,255,254,80,13,255,24,255,255,80,4,
  255,255,254,190,255,255,255,251,0,0,78,255,255,255,255,255,255,193,0,0,
  2,191,255,255,255,255,249,16,0,0,0,3,140,239,255,183,32,0,0,0,
  0,0,0,13,255,0,0,0,0,0,0,0,0,13,255,0,0,0,0,0,
  0,0,0,13,255,0,0,0,0,0,0,0,0,6,136,0,0,0,0,0,
  41,223,217,32,0,0,0,6,255,243,3,239,255,255,227,0,0,0,30,255,
  144,30,255,254,255,254,16,0,0,159,254,16,143,253,32,61,255,128,0,3,
  255,246,0,223,244,0,4,255,208,0,12,255,192,0,255,240,0,0,255,240,
  0,111,255,48,0,255,240,0,0,255,224,1,239,249,0,0,207,244,0,5,
  255,192,9,255,225,0,0,127,253,48,78,255,112,63,255,96,0,0,29,255,
  255,255,253,16,207,252,0,0,0,2,223,255,255,210,6,255,243,0,0,0,
  0,24,206,200,16,30,255,144,0,0,0,0,0,0,0,0,159,254,16,0,
  0,0,0,0,0,0,3,255,246,0,0,0,0,0,0,0,0,12,255,176,
  0,0,0,0,0,0,0,0,111,255,48,0,0,0,0,0,0,0,1,239,
  249,0,0,0,0,0,0,0,0,9,255,225,1,124,236,129,0,0,0,0,
  63,255,80,45,255,255,253,32,0,0,0,207,251,0,223,255,255,255,209,0,
  0,6,255,243,7,255,228,4,223,247,0,0,30,255,128,12,255,80,0,79,
  252,0,0,159,254,16,14,255,16,0,15,255,0,3,255,245,0,14,255,16,
  0,15,255,0,12,255,176,0,12,255,80,0,79,252,0,111,255,48,0,7,
  255,211,2,223,248,1,239,248,0,0,1,223,255,239,255,225,9,255,209,0,
  0,0,62,255,255,254,48,63,255,80,0,0,0,1,157,253,146,0,0,0,
  0,5,173,255,217,48,0,0,0,0,0,0,2,207,255,255,255,250,16,0,
  0,0,0,0,46,255,255,255,255,255,193,0,0,0,0,0,207,255,252,154,
  223,255,252,0,0,0,0,6,255,254,64,0,5,239,251,0,0,0,0,11,
  255,244,0,0,0,62,144,0,0,0,0,14,255,208,0,0,0,3,0,0,
  0,0,0,15,255,176,0,0,0,0,0,0,0,0,0,14,255,192,0,0,
  0,0,0,0,0,0,0,12,255,241,0,0,0,0,0,0,0,0,0,7,
  255,249,0,0,0,0,0,0,0,0,0,1,239,255,80,0,0,0,0,0,
  0,0,0,0,111,255,227,0,0,0,0,0,0,0,0,4,223,255,254,32,
  0,0,0,0,0,0,0,111,255,255,255,209,0,0,0,0,0,0,5,255,
  254,108,255,252,16,0,0,0,0,0,30,255,227,1,223,255,176,0,0,0,
  0,0,127,255,96,0,45,255,249,0,0,0,0,0,191,254,16,0,3,239,
  255,128,0,0,0,0,239,252,0,0,0,63,255,246,0,0,0,0,255,251,
  0,0,0,4,255,255,80,0,0,0,239,253,0,0,0,0,111,255,227,0,
  0,0,207,255,48,0,0,0,7,255,254,32,0,0,127,255,193,0,0,0,
  0,191,255,209,0,0,30,255,252,64,0,0,42,255,255,252,16,0,6,255,
  255,253,169,172,255,255,255,255,176,0,0,143,255,255,255,255,255,255,124,255,
  250,0,0,5,223,255,255,255,255,179,1,207,255,128,0,0,5,173,255,236,
  131,0,0,45,255,246,63,255,160,47,255,144,15,255,128,14,255,96,13,255,
  80,12,255,64,11,255,48,9,255,16,8,255,0,7,254,0,0,0,0,3,
  0,0,0,0,127,128,0,0,7,255,246,0,0,79,255,160,0,1,239,252,
  0,0,10,255,226,0,0,63,255,112,0,0,175,254,16,0,2,255,248,0,
  0,8,255,242,0,0,13,255,192,0,0,47,255,128,0,0,111,255,64,0,
  0,159,255,16,0,0,191,254,0,0,0,223,253,0,0,0,239,252,0,0,
  0,255,251,0,0,0,255,251,0,0,0,239,251,0,0,0,223,252,0,0,
  0,207,254,0,0,0,175,255,16,0,0,127,255,48,0,0,63,255,96,0,
  0,14,255,160,0,0,10,255,225,0,0,4,255,246,0,0,0,223,252,0,
  0,0,127,255,80,0,0,13,255,208,0,0,5,255,248,0,0,0,175,255,
  80,0,0,28,255,244,0,0,1,223,193,0,0,0,24,16,0,48,0,0,
  0,27,228,0,0,0,159,255,64,0,0,28,255,226,0,0,2,239,251,0,
  0,0,95,255,96,0,0,11,255,208,0,0,3,255,246,0,0,0,207,253,
  0,0,0,111,255,48,0,0,31,255,128,0,0,12,255,208,0,0,8,255,
  241,0,0,6,255,244,0,0,3,255,247,0,0,2,255,249,0,0,1,255,
  250,0,0,0,255,250,0,0,0,255,251,0,0,0,255,250,0,0,1,255,
  249,0,0,3,255,248,0,0,5,255,245,0,0,7,255,243,0,0,11,255,
  224,0,0,14,255,160,0,0,79,255,96,0,0,175,254,16,0,1,255,249,
  0,0,8,255,242,0,0,47,255,144,0,0,207,254,32,0,9,255,246,0,
  0,143,255,144,0,0,62,250,0,0,0,2,128,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,10,179,0,0,0,60,16,0,30,255,96,0,
  3,239,160,0,111,254,16,0,10,255,247,0,191,245,0,0,0,143,255,66,
  255,176,0,0,0,5,255,233,255,32,0,0,0,0,61,255,247,0,0,0,
  0,0,41,255,255,255,255,247,0,91,255,255,255,255,255,245,142,255,255,143,
  241,89,223,243,159,255,195,15,244,0,2,97,47,231,0,15,248,0,0,0,
  6,32,0,15,252,0,0,0,0,0,0,15,255,16,0,0,0,0,0,15,
  255,80,0,0,0,0,0,10,134,32,0,0,0,0,0,15,255,176,0,0,
  0,0,0,0,15,255,176,0,0,0,0,0,0,15,255,176,0,0,0,0,
  0,0,15,255,176,0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,
  15,255,176,0,0,0,0,0,0,15,255,176,0,0,0,153,153,153,159,255,
  217,153,153,144,255,255,255,255,255,255,255,255,240,255,255,255,255,255,255,255,
  255,240,255,255,255,255,255,255,255,255,240,0,0,0,15,255,176,0,0,0,
  0,0,0,15,255,176,0,0,0,0,0,0,15,255,176,0,0,0,0,0,
  0,15,255,176,0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,15,
  255,176,0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,8,136,80,
  0,0,0,2,207,178,11,255,251,15,255,255,12,255,253,3,223,249,0,143,
  242,1,239,144,8,255,32,30,249,0,43,242,0,0,32,0,153,153,153,153,
  153,153,144,255,255,255,255,255,255,240,255,255,255,255,255,255,240,255,255,255,
  255,255,255,240,44,252,32,207,255,192,255,255,240,191,255,176,44,252,32,0,
  0,0,0,0,7,255,208,0,0,0,0,0,13,255,128,0,0,0,0,0,
  63,255,48,0,0,0,0,0,143,253,0,0,0,0,0,0,223,247,0,0,
  0,0,0,3,255,242,0,0,0,0,0,9,255,192,0,0,0,0,0,14,
  255,112,0,0,0,0,0,79,255,32,0,0,0,0,0,159,251,0,0,0,
  0,0,0,239,246,0,0,0,0,0,5,255,241,0,0,0,0,0,10,255,
  176,0,0,0,0,0,30,255,96,0,0,0,0,0,95,255,16,0,0,0,
  0,0,191,250,0,0,0,0,0,1,255,245,0,0,0,0,0,6,255,225,
  0,0,0,0,0,11,255,160,0,0,0,0,0,47,255,64,0,0,0,0,
  0,127,254,0,0,0,0,0,0,207,249,0,0,0,0,0,2,255,244,0,
  0,0,0,0,7,255,208,0,0,0,0,0,12,255,128,0,0,0,0,0,
  63,255,48,0,0,0,0,0,143,253,0,0,0,0,0,0,223,248,0,0,
  0,0,0,3,255,242,0,0,0,0,0,9,255,192,0,0,0,0,0,14,
  255,112,0,0,0,0,0,79,255,32,0,0,0,0,0,0,0,0,4,157,
  239,236,130,0,0,0,0,0,0,3,207,255,255,255,255,145,0,0,0,0,
  0,95,255,255,255,255,255,253,32,0,0,0,5,255,255,253,169,190,255,255,
  226,0,0,0,63,255,251,48,0,0,94,255,253,0,0,0,207,255,144,0,
  0,0,1,223,255,128,0,5,255,251,0,0,0,0,0,46,255,241,0,12,
  255,242,0,0,0,0,0,7,255,248,0,47,255,160,0,0,0,0,0,1,
  239,253,0,111,255,80,0,0,0,0,0,0,175,255,32,175,255,16,0,0,
  0,0,0,0,111,255,96,207,254,0,0,0,0,0,0,0,63,255,128,239,
  252,0,0,0,0,0,0,0,31,255,144,255,251,0,0,0,0,0,0,0,
  15,255,160,255,251,0,0,0,0,0,0,0,15,255,176,255,251,0,0,0,
  0,0,0,0,15,255,160,239,252,0,0,0,0,0,0,0,31,255,144,207,
  254,0,0,0,0,0,0,0,63,255,128,159,255,32,0,0,0,0,0,0,
  111,255,80,111,255,80,0,0,0,0,0,0,175,255,32,47,255,160,0,0,
  0,0,0,1,239,253,0,11,255,242,0,0,0,0,0,7,255,247,0,4,
  255,251,0,0,0,0,0,46,255,225,0,0,191,255,144,0,0,0,1,223,
  255,128,0,0,46,255,251,48,0,0,93,255,253,0,0,0,4,255,255,253,
  169,174,255,255,226,0,0,0,0,78,255,255,255,255,255,253,48,0,0,0,
  0,2,191,255,255,255,255,145,0,0,0,0,0,0,3,156,239,236,130,0,
  0,0,0,143,255,255,255,251,143,255,255,255,251,143,255,255,255,251,73,153,
  153,255,251,0,0,0,255,251,0,0,0,255,251,0,0,0,255,251,0,0,
  0,255,251,0,0,0,255,251,0,0,0,255,251,0,0,0,255,251,0,0,
  0,255,251,0,0,0,255,251,0,0,0,255,251,0,0,0,255,251,0,0,
  0,255,251,0,0,0,255,251,0,0,0,255,251,0,0,0,255,251,0,0,
  0,255,251,0,0,0,255,251,0,0,0,255,251,0,0,0,255,251,0,0,
  0,255,251,0,0,0,255,251,0,0,0,255,251,0,0,0,255,251,0,0,
  0,255,251,0,0,0,255,251,0,0,4,156,239,237,147,0,0,0,0,2,
  191,255,255,255,255,161,0,0,0,78,255,255,255,255,255,253,16,0,4,255,
  255,253,169,174,255,255,160,0,46,255,252,48,0,1,143,255,244,0,143,255,
  160,0,0,0,8,255,249,0,26,251,0,0,0,0,1,239,253,0,0,130,
  0,0,0,0,0,207,255,0,0,0,0,0,0,0,0,191,255,0,0,0,
  0,0,0,0,0,223,254,0,0,0,0,0,0,0,3,255,252,0,0,0,
  0,0,0,0,10,255,248,0,0,0,0,0,0,0,95,255,242,0,0,0,
  0,0,0,2,239,255,144,0,0,0,0,0,0,29,255,253,16,0,0,0,
  0,0,1,207,255,227,0,0,0,0,0,0,11,255,255,64,0,0,0,0,
  0,0,175,255,246,0,0,0,0,0,0,9,255,255,112,0,0,0,0,0,
  0,143,255,248,0,0,0,0,0,0,7,255,255,128,0,0,0,0,0,0,
  111,255,249,0,0,0,0,0,0,5,255,255,160,0,0,0,0,0,0,79,
  255,250,0,0,0,0,0,0,3,239,255,176,0,0,0,0,0,0,46,255,
  255,169,153,153,153,153,153,144,223,255,255,255,255,255,255,255,255,240,255,255,
  255,255,255,255,255,255,255,240,255,255,255,255,255,255,255,255,255,240,0,255,
  255,255,255,255,255,255,255,240,0,255,255,255,255,255,255,255,255,240,0,255,
  255,255,255,255,255,255,255,208,0,153,153,153,153,153,158,255,254,32,0,0,
  0,0,0,0,143,255,243,0,0,0,0,0,0,6,255,255,80,0,0,0,
  0,0,0,79,255,247,0,0,0,0,0,0,3,239,255,144,0,0,0,0,
  0,0,29,255,250,0,0,0,0,0,0,1,207,255,193,0,0,0,0,0,
  0,11,255,253,16,0,0,0,0,0,0,143,255,255,218,64,0,0,0,0,
  0,207,255,255,255,250,16,0,0,0,0,207,221,239,255,255,192,0,0,0,
  0,32,0,3,175,255,248,0,0,0,0,0,0,0,5,255,255,32,0,0,
  0,0,0,0,0,143,255,128,0,0,0,0,0,0,0,31,255,192,0,0,
  0,0,0,0,0,12,255,224,0,0,0,0,0,0,0,11,255,240,0,0,
  0,0,0,0,0,12,255,224,0,0,0,0,0,0,0,31,255,208,0,37,
  0,0,0,0,0,111,255,144,1,223,64,0,0,0,3,239,255,64,29,255,
  249,32,0,0,110,255,251,0,12,255,255,252,169,190,255,255,226,0,1,191,
  255,255,255,255,255,254,48,0,0,7,239,255,255,255,255,178,0,0,0,0,
  22,173,255,236,131,0,0,0,0,0,0,0,0,175,255,128,0,0,0,0,
  0,0,0,3,255,254,16,0,0,0,0,0,0,0,11,255,247,0,0,0,
  0,0,0,0,0,79,255,208,0,0,0,0,0,0,0,0,207,255,96,0,
  0,0,0,0,0,0,5,255,252,0,0,0,0,0,0,0,0,13,255,244,
  0,0,0,0,0,0,0,0,111,255,176,0,0,0,0,0,0,0,0,223,
  255,48,0,0,0,0,0,0,0,6,255,250,0,0,34,34,0,0,0,0,
  30,255,242,0,0,255,251,0,0,0,0,127,255,144,0,0,255,251,0,0,
  0,1,239,255,32,0,0,255,251,0,0,0,8,255,248,0,0,0,255,251,
  0,0,0,47,255,225,0,0,0,255,251,0,0,0,159,255,112,0,0,0,
  255,251,0,0,2,255,253,16,0,0,0,255,251,0,0,10,255,246,0,0,
  0,0,255,251,0,0,63,255,255,255,255,255,255,255,255,255,248,143,255,255,
  255,255,255,255,255,255,255,248,143,255,255,255,255,255,255,255,255,255,248,73,
  153,153,153,153,153,153,255,253,153,148,0,0,0,0,0,0,0,255,251,0,
  0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,
  0,255,251,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,
  0,0,0,255,251,0,0,0,5,255,255,255,255,255,255,255,80,0,7,255,
  255,255,255,255,255,255,80,0,8,255,255,255,255,255,255,255,80,0,9,255,
  233,153,153,153,153,153,48,0,11,255,160,0,0,0,0,0,0,0,12,255,
  144,0,0,0,0,0,0,0,14,255,112,0,0,0,0,0,0,0,15,255,
  96,0,0,0,0,0,0,0,31,255,64,0,0,0,0,0,0,0,63,255,
  48,0,0,0,0,0,0,0,79,255,39,206,255,218,64,0,0,0,111,255,
  239,255,255,255,252,32,0,0,127,255,255,255,255,255,255,227,0,0,79,255,
  253,185,155,255,255,253,16,0,5,251,48,0,0,24,255,255,128,0,0,48,
  0,0,0,0,111,255,225,0,0,0,0,0,0,0,10,255,245,0,0,0,
  0,0,0,0,4,255,248,0,0,0,0,0,0,0,1,255,250,0,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,1,255,250,0,0,0,
  0,0,0,0,5,255,248,0,22,0,0,0,0,0,12,255,244,1,191,112,
  0,0,0,0,143,255,208,11,255,250,48,0,0,42,255,255,80,10,255,255,
  253,169,172,255,255,249,0,0,175,255,255,255,255,255,255,144,0,0,5,223,
  255,255,255,255,229,0,0,0,0,5,173,239,253,165,16,0,0,0,0,0,
  0,0,207,255,96,0,0,0,0,0,0,8,255,251,0,0,0,0,0,0,
  0,63,255,226,0,0,0,0,0,0,1,223,255,96,0,0,0,0,0,0,
  9,255,250,0,0,0,0,0,0,0,79,255,225,0,0,0,0,0,0,1,
  223,255,80,0,0,0,0,0,0,9,255,249,0,0,0,0,0,0,0,95,
  255,209,0,0,0,0,0,0,1,239,255,152,152,81,0,0,0,0,10,255,
  255,255,255,254,112,0,0,0,95,255,255,255,255,255,251,16,0,1,239,255,
  255,255,255,255,255,192,0,8,255,255,213,16,38,223,255,248,0,30,255,250,
  0,0,0,10,255,255,32,111,255,192,0,0,0,0,207,255,112,175,255,64,
  0,0,0,0,79,255,176,223,254,0,0,0,0,0,14,255,224,255,251,0,
  0,0,0,0,11,255,240,255,251,0,0,0,0,0,11,255,240,239,253,0,
  0,0,0,0,13,255,208,191,255,32,0,0,0,0,47,255,160,127,255,144,
  0,0,0,0,159,255,96,47,255,245,0,0,0,5,255,254,16,8,255,255,
  112,0,1,127,255,246,0,0,207,255,254,169,174,255,255,160,0,0,28,255,
  255,255,255,255,251,16,0,0,1,159,255,255,255,255,112,0,0,0,0,2,
  140,239,236,113,0,0,0,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,254,153,153,153,153,153,153,
  154,255,249,0,0,0,0,0,0,6,255,243,0,0,0,0,0,0,12,255,
  208,0,0,0,0,0,0,63,255,112,0,0,0,0,0,0,143,255,32,0,
  0,0,0,0,0,239,251,0,0,0,0,0,0,5,255,245,0,0,0,0,
  0,0,11,255,225,0,0,0,0,0,0,31,255,144,0,0,0,0,0,0,
  127,255,64,0,0,0,0,0,0,223,253,0,0,0,0,0,0,3,255,248,
  0,0,0,0,0,0,9,255,242,0,0,0,0,0,0,30,255,192,0,0,
  0,0,0,0,111,255,96,0,0,0,0,0,0,191,254,16,0,0,0,0,
  0,2,255,250,0,0,0,0,0,0,8,255,244,0,0,0,0,0,0,13,
  255,208,0,0,0,0,0,0,79,255,128,0,0,0,0,0,0,175,255,32,
  0,0,0,0,0,1,255,252,0,0,0,0,0,0,6,255,246,0,0,0,
  0,0,0,12,255,241,0,0,0,0,0,0,63,255,160,0,0,0,0,0,
  0,143,255,64,0,0,0,0,0,0,3,140,239,236,131,0,0,0,0,1,
  159,255,255,255,255,145,0,0,0,28,255,255,255,255,255,252,16,0,0,175,
  255,254,169,174,255,255,160,0,3,255,255,129,0,1,143,255,243,0,8,255,
  249,0,0,0,9,255,248,0,10,255,242,0,0,0,2,255,250,0,11,255,
  240,0,0,0,0,255,251,0,9,255,242,0,0,0,2,255,249,0,5,255,
  249,0,0,0,9,255,245,0,0,223,255,129,0,1,143,255,208,0,0,63,
  255,254,169,174,255,255,48,0,0,4,223,255,255,255,255,228,0,0,0,4,
  223,255,255,255,255,195,0,0,0,127,255,255,255,255,255,255,80,0,6,255,
  255,164,16,37,207,255,243,0,30,255,245,0,0,0,9,255,252,0,127,255,
  112,0,0,0,0,191,255,48,207,254,16,0,0,0,0,79,255,112,239,252,
  0,0,0,0,0,31,255,160,255,251,0,0,0,0,0,15,255,176,239,253,
  0,0,0,0,0,63,255,160,207,255,64,0,0,0,0,143,255,112,127,255,
  209,0,0,0,3,255,255,48,30,255,253,64,0,0,110,255,251,0,6,255,
  255,253,169,174,255,255,226,0,0,143,255,255,255,255,255,254,64,0,0,5,
  223,255,255,255,255,178,0,0,0,0,5,173,255,236,148,0,0,0,0,0,
  1,107,239,253,165,0,0,0,0,0,94,255,255,255,255,212,0,0,0,9,
  255,255,255,255,255,255,112,0,0,159,255,255,185,172,255,255,247,0,5,255,
  255,145,0,0,59,255,255,48,13,255,246,0,0,0,0,175,255,176,95,255,
  160,0,0,0,0,13,255,243,175,255,32,0,0,0,0,6,255,247,223,253,
  0,0,0,0,0,2,255,250,255,251,0,0,0,0,0,0,255,251,255,252,
  0,0,0,0,0,1,255,250,239,254,0,0,0,0,0,4,255,249,191,255,
  80,0,0,0,0,9,255,246,127,255,209,0,0,0,0,63,255,241,30,255,
  252,16,0,0,3,239,255,176,7,255,255,231,48,19,159,255,255,48,0,175,
  255,255,255,255,255,255,250,0,0,10,255,255,255,255,255,255,225,0,0,0,
  93,255,255,255,255,255,64,0,0,0,0,71,153,126,255,249,0,0,0,0,
  0,0,0,143,255,193,0,0,0,0,0,0,4,255,255,48,0,0,0,0,
  0,0,30,255,247,0,0,0,0,0,0,0,191,255,176,0,0,0,0,0,
  0,6,255,254,16,0,0,0,0,0,0,63,255,245,0,0,0,0,0,0,
  1,223,255,144,0,0,0,0,0,0,9,255,253,16,0,0,0,0,0,0,
  95,255,243,0,0,0,0,0,44,252,32,207,255,192,255,255,240,207,255,192,
  44,252,32,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,44,252,32,207,255,192,255,255,240,207,255,192,44,
  252,32,2,207,194,12,255,252,15,255,255,12,255,252,2,207,194,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,2,191,178,11,255,251,15,255,255,12,255,253,3,223,249,0,111,242,0,
  223,144,7,255,32,30,250,0,26,242,0,0,32,0,0,0,0,0,0,0,
  0,5,192,0,0,0,0,0,0,6,223,240,0,0,0,0,0,23,239,255,
  240,0,0,0,0,40,239,255,255,224,0,0,0,57,255,255,255,214,16,0,
  0,75,255,255,255,180,0,0,0,92,255,255,255,147,0,0,0,109,255,255,
  253,113,0,0,0,0,255,255,252,80,0,0,0,0,0,255,255,230,16,0,
  0,0,0,0,191,255,255,232,32,0,0,0,0,3,175,255,255,250,48,0,
  0,0,0,2,159,255,255,252,80,0,0,0,0,1,142,255,255,253,113,0,
  0,0,0,1,125,255,255,254,144,0,0,0,0,0,92,255,255,240,0,0,
  0,0,0,0,75,255,240,0,0,0,0,0,0,0,58,240,0,0,0,0,
  0,0,0,0,32,255,255,255,255,255,255,255,255,240,255,255,255,255,255,255,
  255,255,240,255,255,255,255,255,255,255,255,240,153,153,153,153,153,153,153,153,
  144,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,153,153,153,153,153,153,153,153,144,255,255,255,255,255,
  255,255,255,240,255,255,255,255,255,255,255,255,240,255,255,255,255,255,255,255,
  255,240,197,0,0,0,0,0,0,0,0,255,214,0,0,0,0,0,0,0,
  255,255,231,16,0,0,0,0,0,239,255,255,232,32,0,0,0,0,22,223,
  255,255,249,48,0,0,0,0,4,191,255,255,251,64,0,0,0,0,3,159,
  255,255,252,80,0,0,0,0,1,125,255,255,253,96,0,0,0,0,0,92,
  255,255,240,0,0,0,0,0,22,239,255,240,0,0,0,0,40,239,255,255,
  176,0,0,0,58,255,255,255,163,0,0,0,92,255,255,255,146,0,0,1,
  125,255,255,254,129,0,0,0,158,255,255,253,113,0,0,0,0,255,255,252,
  80,0,0,0,0,0,255,251,64,0,0,0,0,0,0,250,48,0,0,0,
  0,0,0,0,32,0,0,0,0,0,0,0,0,0,0,4,157,239,236,130,
  0,0,0,2,191,255,255,255,255,112,0,0,78,255,255,255,255,255,250,0,
  2,239,255,253,169,191,255,255,112,11,255,252,48,0,2,191,255,225,10,255,
  193,0,0,0,12,255,246,0,142,32,0,0,0,4,255,249,0,2,0,0,
  0,0,1,255,251,0,0,0,0,0,0,0,255,250,0,0,0,0,0,0,
  3,255,249,0,0,0,0,0,0,8,255,245,0,0,0,0,0,0,79,255,
  225,0,0,0,0,0,56,255,255,112,0,0,0,0,255,255,255,250,0,0,
  0,0,0,255,255,255,144,0,0,0,0,0,255,255,180,0,0,0,0,0,
  0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,2,207,194,0,0,0,0,0,0,12,255,252,0,0,0,0,0,0,15,
  255,255,0,0,0,0,0,0,12,255,252,0,0,0,0,0,0,2,207,194,
  0,0,0,0,0,0,0,22,156,222,219,132,0,0,0,0,0,0,0,41,
  255,255,255,255,255,214,0,0,0,0,0,6,239,255,255,255,255,255,255,194,
  0,0,0,0,159,255,252,115,16,18,90,255,253,32,0,0,9,255,253,64,
  0,0,0,0,44,255,209,0,0,111,255,193,0,0,0,0,0,0,175,251,
  0,2,239,253,16,0,0,0,0,0,0,12,255,80,9,255,244,0,3,156,
  203,114,153,96,4,255,192,31,255,176,0,143,255,255,254,255,160,0,207,243,
  111,255,80,7,255,255,255,255,255,160,0,127,247,175,255,16,47,255,146,2,
  159,255,160,0,79,251,223,253,0,143,249,0,0,10,255,160,0,31,253,239,
  251,0,207,242,0,0,4,255,160,0,15,254,255,251,0,207,240,0,0,2,
  255,160,0,15,254,239,251,0,191,242,0,0,4,255,160,0,31,254,223,253,
  0,127,249,0,0,10,255,160,0,47,252,191,255,0,30,255,146,2,175,255,
  160,0,95,250,127,255,64,5,255,255,255,255,255,255,255,255,245,47,255,144,
  0,78,255,255,251,255,255,255,255,224,11,255,242,0,1,88,151,48,68,68,
  68,68,32,3,255,251,0,0,0,0,0,0,0,0,0,0,0,159,255,144,
  0,0,0,0,0,1,0,0,0,0,12,255,251,32,0,0,0,0,61,144,
  0,0,0,1,207,255,233,65,0,1,90,255,248,0,0,0,0,25,255,255,
  255,237,239,255,255,211,0,0,0,0,0,76,255,255,255,255,255,231,0,0,
  0,0,0,0,0,56,189,255,236,149,0,0,0,0,0,0,0,0,0,1,
  255,242,0,0,0,0,0,0,0,0,0,0,0,7,255,248,0,0,0,0,
  0,0,0,0,0,0,0,13,255,253,0,0,0,0,0,0,0,0,0,0,
  0,79,255,255,80,0,0,0,0,0,0,0,0,0,0,175,255,255,176,0,
  0,0,0,0,0,0,0,0,1,255,255,255,242,0,0,0,0,0,0,0,
  0,0,7,255,252,255,247,0,0,0,0,0,0,0,0,0,13,255,226,255,
  253,0,0,0,0,0,0,0,0,0,79,255,160,175,255,64,0,0,0,0,
  0,0,0,0,175,255,64,79,255,160,0,0,0,0,0,0,0,1,255,253,
  0,13,255,241,0,0,0,0,0,0,0,7,255,247,0,7,255,247,0,0,
  0,0,0,0,0,13,255,241,0,2,255,253,0,0,0,0,0,0,0,79,
  255,160,0,0,191,255,64,0,0,0,0,0,0,175,255,64,0,0,95,255,
  160,0,0,0,0,0,1,255,253,0,0,0,14,255,241,0,0,0,0,0,
  7,255,247,0,0,0,8,255,247,0,0,0,0,0,13,255,242,0,0,0,
  2,255,253,0,0,0,0,0,63,255,176,0,0,0,0,191,255,64,0,0,
  0,0,159,255,255,255,255,255,255,255,255,160,0,0,0,1,239,255,255,255,
  255,255,255,255,255,241,0,0,0,6,255,255,255,255,255,255,255,255,255,247,
  0,0,0,12,255,249,153,153,153,153,153,154,255,252,0,0,0,63,255,176,
  0,0,0,0,0,0,207,255,48,0,0,159,255,80,0,0,0,0,0,0,
  111,255,144,0,1,239,254,0,0,0,0,0,0,0,31,255,225,0,6,255,
  249,0,0,0,0,0,0,0,10,255,246,0,12,255,243,0,0,0,0,0,
  0,0,4,255,252,0,63,255,192,0,0,0,0,0,0,0,0,223,255,48,
  255,255,255,255,255,254,201,48,0,0,255,255,255,255,255,255,255,250,16,0,
  255,255,255,255,255,255,255,255,193,0,255,253,153,153,153,154,239,255,249,0,
  255,251,0,0,0,0,8,255,255,32,255,251,0,0,0,0,0,159,255,112,
  255,251,0,0,0,0,0,47,255,160,255,251,0,0,0,0,0,15,255,176,
  255,251,0,0,0,0,0,31,255,160,255,251,0,0,0,0,0,79,255,112,
  255,251,0,0,0,0,1,207,255,32,255,251,0,0,0,1,92,255,248,0,
  255,255,255,255,255,255,255,255,176,0,255,255,255,255,255,255,255,252,0,0,
  255,255,255,255,255,255,255,255,177,0,255,253,153,153,153,153,207,255,252,16,
  255,251,0,0,0,0,3,207,255,144,255,251,0,0,0,0,0,29,255,242,
  255,251,0,0,0,0,0,6,255,246,255,251,0,0,0,0,0,1,255,249,
  255,251,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,2,255,250,
  255,251,0,0,0,0,0,6,255,248,255,251,0,0,0,0,0,30,255,244,
  255,251,0,0,0,0,3,223,255,208,255,253,153,153,153,154,207,255,255,64,
  255,255,255,255,255,255,255,255,246,0,255,255,255,255,255,255,255,253,64,0,
  255,255,255,255,255,255,218,96,0,0,0,0,0,0,5,156,239,254,201,80,
  0,0,0,0,0,0,7,239,255,255,255,255,253,96,0,0,0,0,3,223,
  255,255,255,255,255,255,252,32,0,0,0,95,255,255,253,169,155,239,255,255,
  228,0,0,5,255,255,232,32,0,0,3,175,255,249,0,0,46,255,251,32,
  0,0,0,0,4,223,160,0,0,207,255,160,0,0,0,0,0,0,40,0,
  0,6,255,252,16,0,0,0,0,0,0,0,0,0,12,255,243,0,0,0,
  0,0,0,0,0,0,0,63,255,176,0,0,0,0,0,0,0,0,0,0,
  143,255,80,0,0,0,0,0,0,0,0,0,0,191,255,16,0,0,0,0,
  0,0,0,0,0,0,223,253,0,0,0,0,0,0,0,0,0,0,0,255,
  252,0,0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,
  0,0,0,0,0,239,252,0,0,0,0,0,0,0,0,0,0,0,223,253,
  0,0,0,0,0,0,0,0,0,0,0,191,255,16,0,0,0,0,0,0,
  0,0,0,0,143,255,80,0,0,0,0,0,0,0,0,0,0,63,255,176,
  0,0,0,0,0,0,0,0,0,0,12,255,243,0,0,0,0,0,0,0,
  0,0,0,5,255,252,16,0,0,0,0,0,0,0,0,0,0,207,255,160,
  0,0,0,0,0,0,25,16,0,0,46,255,251,32,0,0,0,0,2,207,
  193,0,0,4,255,255,232,32,0,0,3,159,255,252,0,0,0,95,255,255,
  253,169,155,223,255,255,246,0,0,0,3,223,255,255,255,255,255,255,253,64,
  0,0,0,0,7,239,255,255,255,255,254,113,0,0,0,0,0,0,5,156,
  239,254,201,80,0,0,0,255,255,255,255,255,254,201,80,0,0,0,0,255,
  255,255,255,255,255,255,254,113,0,0,0,255,255,255,255,255,255,255,255,253,
  64,0,0,255,253,153,153,153,154,223,255,255,246,0,0,255,251,0,0,0,
  0,2,142,255,255,80,0,255,251,0,0,0,0,0,2,191,255,243,0,255,
  251,0,0,0,0,0,0,10,255,252,0,255,251,0,0,0,0,0,0,1,
  207,255,96,255,251,0,0,0,0,0,0,0,63,255,208,255,251,0,0,0,
  0,0,0,0,11,255,243,255,251,0,0,0,0,0,0,0,5,255,248,255,
  251,0,0,0,0,0,0,0,1,255,251,255,251,0,0,0,0,0,0,0,
  0,223,253,255,251,0,0,0,0,0,0,0,0,191,254,255,251,0,0,0,
  0,0,0,0,0,191,255,255,251,0,0,0,0,0,0,0,0,191,255,255,
  251,0,0,0,0,0,0,0,0,223,253,255,251,0,0,0,0,0,0,0,
  1,255,251,255,251,0,0,0,0,0,0,0,5,255,248,255,251,0,0,0,
  0,0,0,0,11,255,243,255,251,0,0,0,0,0,0,0,63,255,208,255,
  251,0,0,0,0,0,0,1,207,255,96,255,251,0,0,0,0,0,0,10,
  255,252,0,255,251,0,0,0,0,0,2,191,255,243,0,255,251,0,0,0,
  0,2,142,255,255,96,0,255,253,153,153,153,154,223,255,255,246,0,0,255,
  255,255,255,255,255,255,255,253,64,0,0,255,255,255,255,255,255,255,254,129,
  0,0,0,255,255,255,255,255,254,201,81,0,0,0,0,255,255,255,255,255,
  255,255,255,255,128,255,255,255,255,255,255,255,255,255,128,255,255,255,255,255,
  255,255,255,255,128,255,253,153,153,153,153,153,153,153,64,255,251,0,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,253,153,153,153,
  153,153,153,144,0,255,255,255,255,255,255,255,255,240,0,255,255,255,255,255,
  255,255,255,240,0,255,255,255,255,255,255,255,255,240,0,255,251,0,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,0,0,255,253,153,153,153,153,153,153,153,64,255,255,255,255,255,
  255,255,255,255,128,255,255,255,255,255,255,255,255,255,128,255,255,255,255,255,
  255,255,255,255,128,255,255,255,255,255,255,255,255,248,255,255,255,255,255,255,
  255,255,248,255,255,255,255,255,255,255,255,248,255,253,153,153,153,153,153,153,
  148,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,
  0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,
  0,0,255,255,255,255,255,255,255,255,240,255,255,255,255,255,255,255,255,240,
  255,255,255,255,255,255,255,255,240,255,253,153,153,153,153,153,153,144,255,251,
  0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,
  0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,
  0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,
  0,0,0,0,0,0,0,0,0,0,4,140,239,254,201,81,0,0,0,0,
  0,0,0,6,223,255,255,255,255,254,129,0,0,0,0,0,2,207,255,255,
  255,255,255,255,254,80,0,0,0,0,78,255,255,253,185,154,223,255,255,247,
  0,0,0,4,255,255,248,32,0,0,2,142,255,255,96,0,0,46,255,252,
  32,0,0,0,0,1,191,255,80,0,0,191,255,176,0,0,0,0,0,0,
  9,245,0,0,5,255,252,16,0,0,0,0,0,0,0,64,0,0,12,255,
  243,0,0,0,0,0,0,0,0,0,0,0,63,255,176,0,0,0,0,0,
  0,0,0,0,0,0,127,255,80,0,0,0,0,0,0,0,0,0,0,0,
  191,255,16,0,0,0,0,0,0,0,0,0,0,0,223,253,0,0,0,0,
  0,0,0,0,0,0,0,0,255,251,0,0,0,0,4,153,153,153,153,153,
  153,144,255,251,0,0,0,0,7,255,255,255,255,255,255,240,239,251,0,0,
  0,0,7,255,255,255,255,255,255,224,223,253,0,0,0,0,7,255,255,255,
  255,255,255,208,191,255,16,0,0,0,0,0,0,0,0,13,255,176,127,255,
  80,0,0,0,0,0,0,0,0,31,255,144,63,255,176,0,0,0,0,0,
  0,0,0,95,255,80,12,255,244,0,0,0,0,0,0,0,0,191,255,16,
  4,255,253,16,0,0,0,0,0,0,4,255,250,0,0,191,255,176,0,0,
  0,0,0,0,46,255,243,0,0,46,255,251,32,0,0,0,0,3,223,255,
  144,0,0,4,255,255,232,32,0,0,3,159,255,253,16,0,0,0,78,255,
  255,253,169,155,223,255,255,210,0,0,0,0,2,207,255,255,255,255,255,255,
  252,32,0,0,0,0,0,6,223,255,255,255,255,253,96,0,0,0,0,0,
  0,0,5,156,239,254,200,64,0,0,0,0,255,251,0,0,0,0,0,0,
  0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,
  0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,
  0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,
  0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,
  255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,
  255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,
  0,0,255,251,255,253,153,153,153,153,153,153,153,255,251,255,255,255,255,255,
  255,255,255,255,255,251,255,255,255,255,255,255,255,255,255,255,251,255,255,255,
  255,255,255,255,255,255,255,251,255,251,0,0,0,0,0,0,0,255,251,255,
  251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,
  251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,
  0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,
  0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,
  0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,
  0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,
  255,251,0,0,0,0,0,0,0,255,251,255,251,255,251,255,251,255,251,255,
  251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,
  251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,
  251,255,251,255,251,255,251,255,251,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,250,0,0,0,
  0,0,2,255,249,0,0,0,0,0,7,255,246,1,163,0,0,0,29,255,
  242,27,254,80,0,2,207,255,160,175,255,253,169,191,255,254,32,45,255,255,
  255,255,255,244,0,1,191,255,255,255,253,64,0,0,4,157,239,235,97,0,
  0,255,251,0,0,0,0,0,0,159,255,245,0,255,251,0,0,0,0,0,
  9,255,255,80,0,255,251,0,0,0,0,0,159,255,245,0,0,255,251,0,
  0,0,0,8,255,255,80,0,0,255,251,0,0,0,0,143,255,245,0,0,
  0,255,251,0,0,0,8,255,255,80,0,0,0,255,251,0,0,0,143,255,
  245,0,0,0,0,255,251,0,0,8,255,255,80,0,0,0,0,255,251,0,
  0,143,255,245,0,0,0,0,0,255,251,0,8,255,255,80,0,0,0,0,
  0,255,251,0,143,255,245,0,0,0,0,0,0,255,251,8,255,255,96,0,
  0,0,0,0,0,255,251,143,255,246,0,0,0,0,0,0,0,255,254,255,
  255,96,0,0,0,0,0,0,0,255,254,255,255,160,0,0,0,0,0,0,
  0,255,251,111,255,249,0,0,0,0,0,0,0,255,251,7,255,255,128,0,
  0,0,0,0,0,255,251,0,143,255,247,0,0,0,0,0,0,255,251,0,
  9,255,255,96,0,0,0,0,0,255,251,0,0,175,255,245,0,0,0,0,
  0,255,251,0,0,10,255,255,64,0,0,0,0,255,251,0,0,0,191,255,
  244,0,0,0,0,255,251,0,0,0,28,255,254,48,0,0,0,255,251,0,
  0,0,1,223,255,226,0,0,0,255,251,0,0,0,0,45,255,253,32,0,
  0,255,251,0,0,0,0,2,239,255,209,0,0,255,251,0,0,0,0,0,
  62,255,252,16,0,255,251,0,0,0,0,0,3,239,255,193,0,255,251,0,
  0,0,0,0,0,79,255,251,0,255,251,0,0,0,0,0,0,0,255,251,
  0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,
  0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,
  0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,
  0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,
  0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,
  0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,
  255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,
  0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,253,153,153,153,153,
  153,153,148,255,255,255,255,255,255,255,255,248,255,255,255,255,255,255,255,255,
  248,255,255,255,255,255,255,255,255,248,255,209,0,0,0,0,0,0,0,0,
  0,1,223,240,255,247,0,0,0,0,0,0,0,0,0,7,255,240,255,254,
  32,0,0,0,0,0,0,0,0,46,255,240,255,255,160,0,0,0,0,0,
  0,0,0,175,255,240,255,255,244,0,0,0,0,0,0,0,4,255,255,240,
  255,255,252,0,0,0,0,0,0,0,12,255,255,240,255,255,255,96,0,0,
  0,0,0,0,111,255,255,240,255,255,255,225,0,0,0,0,0,1,239,255,
  255,240,255,254,255,249,0,0,0,0,0,9,255,255,255,240,255,251,191,255,
  48,0,0,0,0,63,255,203,255,240,255,251,63,255,176,0,0,0,0,191,
  255,59,255,240,255,251,9,255,245,0,0,0,5,255,249,11,255,240,255,251,
  1,239,253,16,0,0,29,255,225,11,255,240,255,251,0,111,255,128,0,0,
  143,255,112,11,255,240,255,251,0,12,255,242,0,2,255,253,0,11,255,240,
  255,251,0,4,255,250,0,10,255,244,0,11,255,240,255,251,0,0,175,255,
  64,79,255,160,0,11,255,240,255,251,0,0,46,255,192,207,255,32,0,11,
  255,240,255,251,0,0,7,255,252,255,248,0,0,11,255,240,255,251,0,0,
  0,223,255,255,209,0,0,11,255,240,255,251,0,0,0,95,255,255,80,0,
  0,11,255,240,255,251,0,0,0,11,255,251,0,0,0,11,255,240,255,251,
  0,0,0,2,255,243,0,0,0,11,255,240,255,251,0,0,0,0,17,16,
  0,0,0,11,255,240,255,251,0,0,0,0,0,0,0,0,0,11,255,240,
  255,251,0,0,0,0,0,0,0,0,0,11,255,240,255,251,0,0,0,0,
  0,0,0,0,0,11,255,240,255,251,0,0,0,0,0,0,0,0,0,11,
  255,240,255,251,0,0,0,0,0,0,0,0,0,11,255,240,255,225,0,0,
  0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,255,
  96,0,0,0,0,0,0,255,251,255,255,243,0,0,0,0,0,0,255,251,
  255,255,253,16,0,0,0,0,0,255,251,255,255,255,144,0,0,0,0,0,
  255,251,255,255,255,245,0,0,0,0,0,255,251,255,253,255,254,32,0,0,
  0,0,255,251,255,251,127,255,192,0,0,0,0,255,251,255,251,11,255,248,
  0,0,0,0,255,251,255,251,1,239,255,64,0,0,0,255,251,255,251,0,
  79,255,225,0,0,0,255,251,255,251,0,8,255,251,0,0,0,255,251,255,
  251,0,0,207,255,112,0,0,255,251,255,251,0,0,46,255,243,0,0,255,
  251,255,251,0,0,5,255,253,16,0,255,251,255,251,0,0,0,175,255,160,
  0,255,251,255,251,0,0,0,29,255,245,0,255,251,255,251,0,0,0,3,
  255,254,32,255,251,255,251,0,0,0,0,127,255,192,255,251,255,251,0,0,
  0,0,11,255,248,255,251,255,251,0,0,0,0,1,239,255,255,251,255,251,
  0,0,0,0,0,79,255,255,251,255,251,0,0,0,0,0,8,255,255,251,
  255,251,0,0,0,0,0,0,207,255,251,255,251,0,0,0,0,0,0,46,
  255,251,255,251,0,0,0,0,0,0,5,255,251,255,251,0,0,0,0,0,
  0,0,175,251,255,251,0,0,0,0,0,0,0,31,251,0,0,0,0,4,
  156,239,254,200,64,0,0,0,0,0,0,0,7,223,255,255,255,255,253,96,
  0,0,0,0,0,3,207,255,255,255,255,255,255,252,32,0,0,0,0,78,
  255,255,253,169,155,223,255,255,228,0,0,0,4,255,255,232,32,0,0,3,
  159,255,255,64,0,0,46,255,252,32,0,0,0,0,2,207,255,226,0,0,
  191,255,176,0,0,0,0,0,0,28,255,251,0,5,255,253,16,0,0,0,
  0,0,0,1,223,255,80,12,255,243,0,0,0,0,0,0,0,0,79,255,
  192,63,255,176,0,0,0,0,0,0,0,0,11,255,243,143,255,80,0,0,
  0,0,0,0,0,0,5,255,247,191,255,16,0,0,0,0,0,0,0,0,
  1,255,251,223,253,0,0,0,0,0,0,0,0,0,0,223,253,255,251,0,
  0,0,0,0,0,0,0,0,0,207,254,255,251,0,0,0,0,0,0,0,
  0,0,0,191,255,239,252,0,0,0,0,0,0,0,0,0,0,191,254,223,
  253,0,0,0,0,0,0,0,0,0,0,223,253,191,255,16,0,0,0,0,
  0,0,0,0,1,255,251,127,255,80,0,0,0,0,0,0,0,0,5,255,
  247,47,255,176,0,0,0,0,0,0,0,0,11,255,243,11,255,244,0,0,
  0,0,0,0,0,0,79,255,192,4,255,253,16,0,0,0,0,0,0,1,
  223,255,64,0,191,255,177,0,0,0,0,0,0,28,255,251,0,0,30,255,
  252,32,0,0,0,0,2,207,255,226,0,0,3,239,255,249,32,0,0,2,
  159,255,254,48,0,0,0,62,255,255,253,169,154,223,255,255,228,0,0,0,
  0,2,207,255,255,255,255,255,255,252,32,0,0,0,0,0,6,223,255,255,
  255,255,253,96,0,0,0,0,0,0,0,4,140,239,254,201,64,0,0,0,
  0,255,255,255,255,255,254,182,16,0,0,255,255,255,255,255,255,255,229,0,
  0,255,255,255,255,255,255,255,255,128,0,255,253,153,153,153,172,255,255,245,
  0,255,251,0,0,0,0,60,255,254,16,255,251,0,0,0,0,1,207,255,
  96,255,251,0,0,0,0,0,79,255,176,255,251,0,0,0,0,0,13,255,
  224,255,251,0,0,0,0,0,11,255,240,255,251,0,0,0,0,0,11,255,
  240,255,251,0,0,0,0,0,13,255,208,255,251,0,0,0,0,0,79,255,
  176,255,251,0,0,0,0,1,207,255,96,255,251,0,0,0,0,60,255,254,
  16,255,253,153,153,153,156,255,255,245,0,255,255,255,255,255,255,255,255,112,
  0,255,255,255,255,255,255,255,229,0,0,255,255,255,255,255,254,182,16,0,
  0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,
  0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,
  0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,
  0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,
  0,255,251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,
  0,255,251,0,0,0,0,0,0,0,0,0,0,0,0,4,156,239,254,201,
  64,0,0,0,0,0,0,0,0,6,223,255,255,255,255,253,96,0,0,0,
  0,0,0,2,207,255,255,255,255,255,255,252,32,0,0,0,0,0,78,255,
  255,253,169,155,223,255,255,228,0,0,0,0,4,255,255,232,32,0,0,2,
  159,255,255,64,0,0,0,46,255,252,32,0,0,0,0,2,207,255,226,0,
  0,0,191,255,176,0,0,0,0,0,0,27,255,251,0,0,5,255,253,16,
  0,0,0,0,0,0,1,223,255,80,0,12,255,243,0,0,0,0,0,0,
  0,0,79,255,192,0,63,255,176,0,0,0,0,0,0,0,0,11,255,243,
  0,127,255,80,0,0,0,0,0,0,0,0,5,255,247,0,191,255,16,0,
  0,0,0,0,0,0,0,1,255,251,0,223,253,0,0,0,0,0,0,0,
  0,0,0,223,253,0,255,251,0,0,0,0,0,0,0,0,0,0,207,254,
  0,255,251,0,0,0,0,0,0,0,0,0,0,191,255,0,239,252,0,0,
  0,0,0,0,182,0,0,0,191,254,0,223,253,0,0,0,0,0,11,255,
  96,0,0,223,253,0,191,255,16,0,0,0,0,111,255,246,0,1,255,251,
  0,127,255,96,0,0,0,0,9,255,255,80,5,255,247,0,47,255,192,0,
  0,0,0,0,159,255,245,11,255,243,0,11,255,244,0,0,0,0,0,9,
  255,255,143,255,192,0,4,255,253,16,0,0,0,0,0,159,255,255,255,80,
  0,0,175,255,177,0,0,0,0,0,9,255,255,251,0,0,0,29,255,252,
  48,0,0,0,0,2,239,255,246,0,0,0,3,239,255,249,48,0,0,2,
  143,255,255,255,64,0,0,0,62,255,255,253,185,154,223,255,255,255,255,228,
  0,0,0,2,191,255,255,255,255,255,255,252,58,255,254,48,0,0,0,5,
  223,255,255,255,255,253,96,0,191,255,227,0,0,0,0,4,140,239,254,201,
  64,0,0,11,255,246,0,0,0,0,0,0,0,0,0,0,0,0,0,191,
  112,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,255,255,255,255,
  255,254,200,32,0,0,0,255,255,255,255,255,255,255,248,0,0,0,255,255,
  255,255,255,255,255,255,176,0,0,255,253,153,153,153,155,239,255,249,0,0,
  255,251,0,0,0,0,24,255,255,48,0,255,251,0,0,0,0,0,159,255,
  144,0,255,251,0,0,0,0,0,31,255,208,0,255,251,0,0,0,0,0,
  12,255,224,0,255,251,0,0,0,0,0,11,255,240,0,255,251,0,0,0,
  0,0,13,255,224,0,255,251,0,0,0,0,0,63,255,176,0,255,251,0,
  0,0,0,1,207,255,112,0,255,251,0,0,0,2,109,255,254,16,0,255,
  255,255,255,255,255,255,255,245,0,0,255,255,255,255,255,255,255,255,96,0,
  0,255,255,255,255,255,255,255,179,0,0,0,255,253,153,223,255,249,98,0,
  0,0,0,255,251,0,46,255,251,0,0,0,0,0,255,251,0,4,255,255,
  128,0,0,0,0,255,251,0,0,127,255,245,0,0,0,0,255,251,0,0,
  10,255,254,32,0,0,0,255,251,0,0,1,207,255,209,0,0,0,255,251,
  0,0,0,46,255,251,0,0,0,255,251,0,0,0,4,255,255,128,0,0,
  255,251,0,0,0,0,127,255,245,0,0,255,251,0,0,0,0,10,255,254,
  32,0,255,251,0,0,0,0,1,207,255,209,0,255,251,0,0,0,0,0,
  46,255,250,0,255,251,0,0,0,0,0,4,255,255,112,0,0,0,57,206,
  254,217,64,0,0,0,0,43,255,255,255,255,252,64,0,0,2,239,255,255,
  255,255,255,247,0,0,13,255,255,218,154,223,255,255,96,0,111,255,228,0,
  0,4,207,255,64,0,191,255,48,0,0,0,10,244,0,0,239,252,0,0,
  0,0,0,48,0,0,255,251,0,0,0,0,0,0,0,0,239,253,0,0,
  0,0,0,0,0,0,207,255,80,0,0,0,0,0,0,0,143,255,246,0,
  0,0,0,0,0,0,46,255,255,197,0,0,0,0,0,0,6,255,255,255,
  232,32,0,0,0,0,0,94,255,255,255,251,64,0,0,0,0,1,158,255,
  255,255,250,16,0,0,0,0,1,108,255,255,255,210,0,0,0,0,0,0,
  57,255,255,252,0,0,0,0,0,0,0,42,255,255,80,0,0,0,0,0,
  0,0,175,255,160,0,0,0,0,0,0,0,31,255,208,0,0,0,0,0,
  0,0,12,255,240,0,32,0,0,0,0,0,11,255,240,3,231,0,0,0,
  0,0,14,255,208,62,255,96,0,0,0,0,111,255,160,143,255,251,48,0,
  0,24,255,255,80,10,255,255,253,169,155,239,255,251,0,0,159,255,255,255,
  255,255,255,193,0,0,5,223,255,255,255,255,249,16,0,0,0,5,156,239,
  253,183,32,0,0,143,255,255,255,255,255,255,255,255,255,255,240,143,255,255,
  255,255,255,255,255,255,255,255,240,143,255,255,255,255,255,255,255,255,255,255,
  240,73,153,153,153,153,255,253,153,153,153,153,144,0,0,0,0,0,255,251,
  0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,
  0,0,255,251,0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,
  0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,0,0,255,251,
  0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,
  0,0,255,251,0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,
  0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,0,0,255,251,
  0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,
  0,0,255,251,0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,
  0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,0,0,255,251,
  0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,
  0,0,255,251,0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,
  0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,0,0,255,251,
  0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,
  0,0,255,251,0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,
  0,0,0,0,0,0,255,251,0,0,0,0,0,255,251,0,0,0,0,0,
  0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,
  0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,
  0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,
  251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,
  251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,
  0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,
  0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,
  0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,
  0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,
  255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,
  255,251,239,253,0,0,0,0,0,0,2,255,250,191,255,16,0,0,0,0,
  0,5,255,247,127,255,112,0,0,0,0,0,11,255,243,47,255,226,0,0,
  0,0,0,79,255,208,10,255,252,16,0,0,0,3,239,255,96,2,239,255,
  230,0,0,1,127,255,251,0,0,79,255,255,235,154,191,255,255,210,0,0,
  4,239,255,255,255,255,255,252,32,0,0,0,42,255,255,255,255,255,129,0,
  0,0,0,0,56,206,255,219,113,0,0,0,159,255,128,0,0,0,0,0,
  0,0,14,255,242,63,255,208,0,0,0,0,0,0,0,95,255,176,12,255,
  244,0,0,0,0,0,0,0,191,255,80,6,255,249,0,0,0,0,0,0,
  1,255,254,0,1,255,254,16,0,0,0,0,0,7,255,248,0,0,175,255,
  96,0,0,0,0,0,13,255,242,0,0,79,255,176,0,0,0,0,0,63,
  255,192,0,0,13,255,242,0,0,0,0,0,159,255,96,0,0,8,255,247,
  0,0,0,0,0,239,254,16,0,0,2,255,253,0,0,0,0,5,255,249,
  0,0,0,0,191,255,64,0,0,0,11,255,243,0,0,0,0,95,255,144,
  0,0,0,47,255,208,0,0,0,0,14,255,225,0,0,0,127,255,112,0,
  0,0,0,9,255,245,0,0,0,223,255,16,0,0,0,0,3,255,251,0,
  0,3,255,250,0,0,0,0,0,0,207,255,32,0,9,255,244,0,0,0,
  0,0,0,127,255,112,0,30,255,208,0,0,0,0,0,0,31,255,208,0,
  95,255,128,0,0,0,0,0,0,10,255,244,0,191,255,32,0,0,0,0,
  0,0,4,255,249,2,255,251,0,0,0,0,0,0,0,0,223,254,23,255,
  245,0,0,0,0,0,0,0,0,143,255,93,255,225,0,0,0,0,0,0,
  0,0,47,255,223,255,144,0,0,0,0,0,0,0,0,11,255,255,255,48,
  0,0,0,0,0,0,0,0,6,255,255,252,0,0,0,0,0,0,0,0,
  0,1,239,255,246,0,0,0,0,0,0,0,0,0,0,159,255,241,0,0,
  0,0,0,0,0,0,0,0,63,255,160,0,0,0,0,0,0,0,0,0,
  0,12,255,64,0,0,0,0,0,159,255,48,0,0,0,0,0,14,255,32,
  0,0,0,0,0,14,255,192,79,255,128,0,0,0,0,0,63,255,96,0,
  0,0,0,0,79,255,112,14,255,192,0,0,0,0,0,143,255,176,0,0,
  0,0,0,143,255,32,9,255,242,0,0,0,0,0,207,255,241,0,0,0,
  0,0,223,253,0,5,255,246,0,0,0,0,2,255,255,245,0,0,0,0,
  3,255,248,0,1,239,251,0,0,0,0,6,255,255,249,0,0,0,0,7,
  255,243,0,0,175,255,16,0,0,0,11,255,255,254,0,0,0,0,12,255,
  208,0,0,111,255,80,0,0,0,31,255,207,255,48,0,0,0,31,255,144,
  0,0,31,255,144,0,0,0,95,255,62,255,128,0,0,0,111,255,64,0,
  0,11,255,224,0,0,0,159,253,10,255,192,0,0,0,175,254,0,0,0,
  6,255,244,0,0,0,239,249,6,255,242,0,0,1,239,250,0,0,0,2,
  255,248,0,0,3,255,244,1,255,246,0,0,5,255,245,0,0,0,0,207,
  253,0,0,8,255,225,0,207,251,0,0,9,255,241,0,0,0,0,127,255,
  32,0,12,255,160,0,127,255,16,0,14,255,176,0,0,0,0,63,255,112,
  0,47,255,96,0,63,255,80,0,63,255,96,0,0,0,0,13,255,176,0,
  111,255,16,0,13,255,144,0,143,255,16,0,0,0,0,8,255,241,0,191,
  252,0,0,9,255,224,0,207,252,0,0,0,0,0,4,255,246,1,255,247,
  0,0,4,255,243,2,255,247,0,0,0,0,0,0,239,250,5,255,243,0,
  0,0,239,248,7,255,242,0,0,0,0,0,0,159,254,9,255,208,0,0,
  0,175,252,11,255,208,0,0,0,0,0,0,95,255,78,255,144,0,0,0,
  95,255,63,255,128,0,0,0,0,0,0,30,255,207,255,64,0,0,0,31,
  255,191,255,48,0,0,0,0,0,0,10,255,255,254,16,0,0,0,11,255,
  255,253,0,0,0,0,0,0,0,5,255,255,250,0,0,0,0,7,255,255,
  249,0,0,0,0,0,0,0,1,255,255,246,0,0,0,0,2,255,255,244,
  0,0,0,0,0,0,0,0,191,255,242,0,0,0,0,0,223,255,224,0,
  0,0,0,0,0,0,0,111,255,192,0,0,0,0,0,143,255,160,0,0,
  0,0,0,0,0,0,47,255,112,0,0,0,0,0,79,255,80,0,0,0,
  0,0,0,0,0,12,255,48,0,0,0,0,0,14,255,16,0,0,0,0,
  29,255,248,0,0,0,0,0,0,0,159,255,160,4,255,255,48,0,0,0,
  0,0,4,255,254,16,0,159,255,209,0,0,0,0,0,29,255,244,0,0,
  29,255,249,0,0,0,0,0,159,255,144,0,0,3,255,255,64,0,0,0,
  4,255,253,16,0,0,0,127,255,209,0,0,0,29,255,244,0,0,0,0,
  12,255,250,0,0,0,159,255,128,0,0,0,0,2,239,255,80,0,5,255,
  252,0,0,0,0,0,0,111,255,226,0,30,255,243,0,0,0,0,0,0,
  10,255,251,0,175,255,112,0,0,0,0,0,0,1,239,255,101,255,252,0,
  0,0,0,0,0,0,0,95,255,238,255,226,0,0,0,0,0,0,0,0,
  9,255,255,255,96,0,0,0,0,0,0,0,0,1,223,255,251,0,0,0,
  0,0,0,0,0,0,0,191,255,252,0,0,0,0,0,0,0,0,0,6,
  255,255,255,112,0,0,0,0,0,0,0,0,46,255,255,255,243,0,0,0,
  0,0,0,0,0,191,255,104,255,252,0,0,0,0,0,0,0,7,255,251,
  0,207,255,112,0,0,0,0,0,0,63,255,226,0,63,255,242,0,0,0,
  0,0,0,223,255,80,0,8,255,252,0,0,0,0,0,9,255,250,0,0,
  1,223,255,112,0,0,0,0,79,255,209,0,0,0,79,255,242,0,0,0,
  1,223,255,64,0,0,0,9,255,252,0,0,0,10,255,248,0,0,0,0,
  1,223,255,112,0,0,95,255,208,0,0,0,0,0,79,255,226,0,2,239,
  255,48,0,0,0,0,0,9,255,251,0,11,255,247,0,0,0,0,0,0,
  1,223,255,112,127,255,192,0,0,0,0,0,0,0,79,255,226,30,255,246,
  0,0,0,0,0,0,0,159,255,160,6,255,254,16,0,0,0,0,0,3,
  255,254,32,0,191,255,144,0,0,0,0,0,12,255,247,0,0,47,255,243,
  0,0,0,0,0,111,255,192,0,0,8,255,252,0,0,0,0,1,239,255,
  64,0,0,1,223,255,96,0,0,0,10,255,249,0,0,0,0,95,255,225,
  0,0,0,79,255,225,0,0,0,0,10,255,249,0,0,0,223,255,96,0,
  0,0,0,2,239,255,64,0,7,255,251,0,0,0,0,0,0,127,255,192,
  0,46,255,242,0,0,0,0,0,0,13,255,247,0,175,255,128,0,0,0,
  0,0,0,4,255,254,36,255,253,16,0,0,0,0,0,0,0,175,255,173,
  255,244,0,0,0,0,0,0,0,0,30,255,255,255,160,0,0,0,0,0,
  0,0,0,6,255,255,254,32,0,0,0,0,0,0,0,0,0,207,255,247,
  0,0,0,0,0,0,0,0,0,0,63,255,208,0,0,0,0,0,0,0,
  0,0,0,15,255,176,0,0,0,0,0,0,0,0,0,0,15,255,176,0,
  0,0,0,0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,0,0,
  0,0,15,255,176,0,0,0,0,0,0,0,0,0,0,15,255,176,0,0,
  0,0,0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,0,0,0,
  0,15,255,176,0,0,0,0,0,0,0,0,0,0,15,255,176,0,0,0,
  0,0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,0,0,0,0,
  15,255,176,0,0,0,0,0,0,0,0,0,0,15,255,176,0,0,0,0,
  0,0,0,0,0,0,15,255,176,0,0,0,0,0,15,255,255,255,255,255,
  255,255,255,255,15,255,255,255,255,255,255,255,255,255,15,255,255,255,255,255,
  255,255,255,255,9,153,153,153,153,153,153,158,255,247,0,0,0,0,0,0,
  0,95,255,192,0,0,0,0,0,0,1,239,255,48,0,0,0,0,0,0,
  10,255,247,0,0,0,0,0,0,0,111,255,192,0,0,0,0,0,0,2,
  239,255,48,0,0,0,0,0,0,11,255,248,0,0,0,0,0,0,0,111,
  255,208,0,0,0,0,0,0,2,239,255,64,0,0,0,0,0,0,11,255,
  249,0,0,0,0,0,0,0,111,255,209,0,0,0,0,0,0,2,239,255,
  64,0,0,0,0,0,0,11,255,249,0,0,0,0,0,0,0,111,255,225,
  0,0,0,0,0,0,2,239,255,80,0,0,0,0,0,0,11,255,250,0,
  0,0,0,0,0,0,127,255,225,0,0,0,0,0,0,2,239,255,80,0,
  0,0,0,0,0,11,255,250,0,0,0,0,0,0,0,127,255,226,0,0,
  0,0,0,0,2,255,255,96,0,0,0,0,0,0,12,255,251,0,0,0,
  0,0,0,0,127,255,251,153,153,153,153,153,153,153,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,240,255,255,255,255,240,255,254,238,238,224,255,
  240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,
  240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,
  240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,
  240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,
  240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,
  240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,
  240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,255,255,255,240,255,
  255,255,255,240,255,255,255,255,240,79,255,32,0,0,0,0,0,14,255,112,
  0,0,0,0,0,9,255,192,0,0,0,0,0,3,255,242,0,0,0,0,
  0,0,223,248,0,0,0,0,0,0,143,253,0,0,0,0,0,0,63,255,
  48,0,0,0,0,0,12,255,128,0,0,0,0,0,7,255,208,0,0,0,
  0,0,2,255,244,0,0,0,0,0,0,207,249,0,0,0,0,0,0,127,
  254,0,0,0,0,0,0,47,255,64,0,0,0,0,0,11,255,160,0,0,
  0,0,0,6,255,225,0,0,0,0,0,1,255,245,0,0,0,0,0,0,
  191,250,0,0,0,0,0,0,95,255,16,0,0,0,0,0,30,255,96,0,
  0,0,0,0,10,255,176,0,0,0,0,0,5,255,241,0,0,0,0,0,
  0,239,246,0,0,0,0,0,0,159,251,0,0,0,0,0,0,79,255,32,
  0,0,0,0,0,14,255,112,0,0,0,0,0,9,255,192,0,0,0,0,
  0,3,255,242,0,0,0,0,0,0,223,247,0,0,0,0,0,0,143,253,
  0,0,0,0,0,0,63,255,48,0,0,0,0,0,13,255,128,0,0,0,
  0,0,7,255,208,255,255,255,255,240,255,255,255,255,240,238,238,238,255,240,
  0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,
  0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,
  0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,
  0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,
  0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,
  0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,
  0,0,0,255,240,0,0,0,255,240,0,0,0,255,240,255,255,255,255,240,
  255,255,255,255,240,255,255,255,255,240,0,0,2,136,48,0,0,0,0,11,
  255,192,0,0,0,0,79,255,245,0,0,0,0,207,255,253,0,0,0,5,
  255,184,255,96,0,0,13,255,49,239,225,0,0,111,249,0,127,248,0,1,
  239,226,0,30,255,32,8,255,128,0,6,255,144,46,254,16,0,0,223,243,
  159,246,0,0,0,95,251,153,153,153,153,153,153,153,153,153,144,255,255,255,
  255,255,255,255,255,255,240,255,255,255,255,255,255,255,255,255,240,255,255,255,
  255,255,255,255,255,255,240,0,3,0,0,0,0,143,64,0,0,8,255,226,
  0,0,46,255,252,0,0,3,223,255,144,0,0,44,255,246,0,0,1,175,
  255,48,0,0,8,255,64,0,0,0,85,0,0,0,3,140,239,235,96,11,
  255,240,0,1,159,255,255,255,253,59,255,240,0,28,255,255,255,255,255,253,
  255,240,0,207,255,255,185,172,255,255,255,240,8,255,255,129,0,0,42,255,
  255,240,30,255,246,0,0,0,0,143,255,240,127,255,160,0,0,0,0,13,
  255,240,191,255,32,0,0,0,0,7,255,240,239,253,0,0,0,0,0,4,
  255,240,255,251,0,0,0,0,0,2,255,240,255,251,0,0,0,0,0,3,
  255,240,239,253,0,0,0,0,0,5,255,240,191,255,32,0,0,0,0,8,
  255,240,127,255,160,0,0,0,0,13,255,240,31,255,246,0,0,0,0,159,
  255,240,8,255,255,145,0,0,42,255,255,240,1,207,255,255,185,156,255,255,
  255,240,0,44,255,255,255,255,255,253,255,240,0,1,159,255,255,255,253,75,
  255,240,0,0,3,156,239,235,97,11,255,240,255,240,0,0,0,0,0,0,
  0,0,255,240,0,0,0,0,0,0,0,0,255,240,0,0,0,0,0,0,
  0,0,255,240,0,0,0,0,0,0,0,0,255,240,0,0,0,0,0,0,
  0,0,255,240,0,0,0,0,0,0,0,0,255,240,0,0,0,0,0,0,
  0,0,255,240,0,0,0,0,0,0,0,0,255,240,0,0,0,0,0,0,
  0,0,255,240,1,107,239,236,130,0,0,0,255,240,77,255,255,255,255,145,
  0,0,255,245,255,255,255,255,255,252,16,0,255,255,255,252,169,191,255,255,
  192,0,255,255,250,32,0,1,143,255,248,0,255,255,144,0,0,0,6,255,
  255,16,255,253,0,0,0,0,0,175,255,112,255,246,0,0,0,0,0,47,
  255,176,255,242,0,0,0,0,0,13,255,224,255,240,0,0,0,0,0,11,
  255,240,255,240,0,0,0,0,0,11,255,240,255,242,0,0,0,0,0,13,
  255,224,255,246,0,0,0,0,0,63,255,176,255,253,0,0,0,0,0,175,
  255,112,255,255,144,0,0,0,6,255,255,32,255,255,250,32,0,1,159,255,
  248,0,255,255,255,252,169,191,255,255,193,0,255,246,255,255,255,255,255,253,
  32,0,255,240,94,255,255,255,255,145,0,0,255,240,1,107,239,236,147,0,
  0,0,0,0,1,123,239,253,165,0,0,0,0,126,255,255,255,255,212,0,
  0,27,255,255,255,255,255,255,112,0,191,255,255,185,156,255,255,209,7,255,
  255,145,0,0,25,253,32,30,255,246,0,0,0,0,82,0,111,255,160,0,
  0,0,0,0,0,191,255,32,0,0,0,0,0,0,239,253,0,0,0,0,
  0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,
  0,223,253,0,0,0,0,0,0,0,191,255,32,0,0,0,0,0,0,111,
  255,160,0,0,0,0,0,0,30,255,246,0,0,0,0,83,0,6,255,255,
  145,0,0,24,254,32,0,175,255,255,185,155,255,255,209,0,10,255,255,255,
  255,255,255,112,0,0,110,255,255,255,255,212,0,0,0,1,107,239,253,165,
  0,0,0,0,0,0,0,0,0,11,255,240,0,0,0,0,0,0,0,11,
  255,240,0,0,0,0,0,0,0,11,255,240,0,0,0,0,0,0,0,11,
  255,240,0,0,0,0,0,0,0,11,255,240,0,0,0,0,0,0,0,11,
  255,240,0,0,0,0,0,0,0,11,255,240,0,0,0,0,0,0,0,11,
  255,240,0,0,0,0,0,0,0,11,255,240,0,0,3,156,239,235,96,11,
  255,240,0,1,159,255,255,255,253,59,255,240,0,28,255,255,255,255,255,237,
  255,240,0,207,255,255,185,172,255,255,255,240,8,255,255,129,0,0,43,255,
  255,240,30,255,246,0,0,0,0,175,255,240,127,255,160,0,0,0,0,13,
  255,240,191,255,32,0,0,0,0,7,255,240,239,253,0,0,0,0,0,4,
  255,240,255,251,0,0,0,0,0,2,255,240,255,251,0,0,0,0,0,3,
  255,240,239,253,0,0,0,0,0,5,255,240,191,255,32,0,0,0,0,8,
  255,240,127,255,160,0,0,0,0,13,255,240,31,255,246,0,0,0,0,159,
  255,240,8,255,255,145,0,0,43,255,255,240,1,207,255,255,185,172,255,255,
  255,240,0,45,255,255,255,255,255,253,255,240,0,1,175,255,255,255,253,59,
  255,240,0,0,3,157,255,235,96,11,255,240,0,0,1,107,239,253,165,0,
  0,0,0,0,110,255,255,255,255,195,0,0,0,10,255,255,255,255,255,254,
  64,0,0,175,255,254,185,172,255,255,226,0,6,255,255,113,0,0,60,255,
  251,0,30,255,244,0,0,0,1,223,255,48,111,255,128,0,0,0,0,95,
  255,144,191,255,16,0,0,0,0,14,255,192,239,255,255,255,255,255,255,255,
  255,224,255,255,255,255,255,255,255,255,255,240,255,255,255,255,255,255,255,255,
  255,224,223,255,153,153,153,153,153,153,153,128,191,255,32,0,0,0,0,0,
  0,0,111,255,144,0,0,0,0,0,0,0,30,255,246,0,0,0,0,38,
  0,0,6,255,255,145,0,0,6,239,144,0,0,175,255,255,201,155,239,255,
  247,0,0,10,255,255,255,255,255,255,177,0,0,0,110,255,255,255,255,232,
  0,0,0,0,1,106,223,254,183,16,0,0,0,0,0,0,40,206,253,146,
  0,0,0,0,6,239,255,255,254,80,0,0,0,95,255,255,255,255,208,0,
  0,1,239,255,234,156,253,32,0,0,8,255,251,16,0,98,0,0,0,12,
  255,242,0,0,0,0,0,0,14,255,192,0,0,0,0,0,0,15,255,176,
  0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,15,255,176,0,0,
  0,0,143,255,255,255,255,255,255,128,0,143,255,255,255,255,255,255,128,0,
  143,255,255,255,255,255,255,128,0,73,153,159,255,217,153,153,64,0,0,0,
  15,255,176,0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,15,255,
  176,0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,15,255,176,0,
  0,0,0,0,0,15,255,176,0,0,0,0,0,0,15,255,176,0,0,0,
  0,0,0,15,255,176,0,0,0,0,0,0,15,255,176,0,0,0,0,0,
  0,15,255,176,0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,15,
  255,176,0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,15,255,176,
  0,0,0,0,0,0,15,255,176,0,0,0,0,0,0,15,255,176,0,0,
  0,0,0,0,4,173,255,218,64,11,255,240,0,1,191,255,255,255,251,27,
  255,240,0,45,255,255,255,255,255,204,255,240,1,223,255,254,185,174,255,255,
  255,240,9,255,255,112,0,0,110,255,255,240,47,255,244,0,0,0,4,255,
  255,240,127,255,144,0,0,0,0,143,255,240,191,255,32,0,0,0,0,47,
  255,240,239,253,0,0,0,0,0,13,255,240,255,251,0,0,0,0,0,11,
  255,240,255,251,0,0,0,0,0,11,255,240,239,253,0,0,0,0,0,13,
  255,240,191,255,32,0,0,0,0,31,255,240,127,255,144,0,0,0,0,143,
  255,240,47,255,244,0,0,0,3,255,255,240,9,255,255,112,0,0,94,255,
  255,240,1,223,255,254,185,173,255,255,255,240,0,45,255,255,255,255,255,204,
  255,240,0,2,191,255,255,255,251,27,255,240,0,0,4,173,255,218,64,11,
  255,240,0,0,0,0,0,0,0,14,255,208,0,0,0,0,0,0,0,79,
  255,144,3,214,0,0,0,0,1,223,255,64,95,255,163,0,0,0,93,255,
  251,0,62,255,255,219,153,190,255,255,226,0,4,239,255,255,255,255,255,254,
  48,0,0,42,255,255,255,255,255,161,0,0,0,0,40,190,255,236,130,0,
  0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,
  255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,
  0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,
  0,0,0,255,251,0,90,239,236,113,0,0,255,251,27,255,255,255,253,64,
  0,255,252,223,255,255,255,255,243,0,255,255,255,235,154,239,255,253,0,255,
  255,250,16,0,25,255,255,96,255,255,176,0,0,0,175,255,176,255,255,32,
  0,0,0,47,255,224,255,253,0,0,0,0,13,255,240,255,251,0,0,0,
  0,11,255,240,255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,11,
  255,240,255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,11,255,240,
  255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,11,255,240,255,251,
  0,0,0,0,11,255,240,255,251,0,0,0,0,11,255,240,255,251,0,0,
  0,0,11,255,240,255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,
  11,255,240,44,252,32,207,255,192,255,255,240,207,255,192,44,252,32,0,0,
  0,0,0,0,0,0,0,0,0,0,191,255,0,191,255,0,191,255,0,191,
  255,0,191,255,0,191,255,0,191,255,0,191,255,0,191,255,0,191,255,0,
  191,255,0,191,255,0,191,255,0,191,255,0,191,255,0,191,255,0,191,255,
  0,191,255,0,191,255,0,191,255,0,0,0,0,0,44,252,32,0,0,0,
  0,207,255,192,0,0,0,0,255,255,240,0,0,0,0,207,255,192,0,0,
  0,0,44,252,32,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,191,255,0,
  0,0,0,0,191,255,0,0,0,0,0,191,255,0,0,0,0,0,191,255,
  0,0,0,0,0,191,255,0,0,0,0,0,191,255,0,0,0,0,0,191,
  255,0,0,0,0,0,191,255,0,0,0,0,0,191,255,0,0,0,0,0,
  191,255,0,0,0,0,0,191,255,0,0,0,0,0,191,255,0,0,0,0,
  0,191,255,0,0,0,0,0,191,255,0,0,0,0,0,191,255,0,0,0,
  0,0,191,255,0,0,0,0,0,191,255,0,0,0,0,0,191,255,0,0,
  0,0,0,191,255,0,0,0,0,0,191,255,0,0,0,0,0,191,255,0,
  0,0,0,0,191,255,0,0,0,0,0,223,254,0,0,163,0,7,255,251,
  0,11,255,185,207,255,246,0,127,255,255,255,255,192,0,10,255,255,255,252,
  16,0,0,91,239,235,96,0,0,255,251,0,0,0,0,0,0,0,255,251,
  0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,
  0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,
  0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,
  0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,4,255,254,48,255,
  251,0,0,0,62,255,244,0,255,251,0,0,2,239,255,80,0,255,251,0,
  0,29,255,246,0,0,255,251,0,1,207,255,112,0,0,255,251,0,11,255,
  248,0,0,0,255,251,0,175,255,160,0,0,0,255,251,8,255,251,0,0,
  0,0,255,251,127,255,193,0,0,0,0,255,254,255,255,16,0,0,0,0,
  255,252,223,255,144,0,0,0,0,255,251,62,255,247,0,0,0,0,255,251,
  4,255,255,80,0,0,0,255,251,0,95,255,244,0,0,0,255,251,0,8,
  255,254,32,0,0,255,251,0,0,159,255,210,0,0,255,251,0,0,11,255,
  252,16,0,255,251,0,0,1,207,255,176,0,255,251,0,0,0,46,255,249,
  0,255,251,0,0,0,3,239,255,112,255,251,255,251,255,251,255,251,255,251,
  255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,
  255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,
  255,251,255,251,255,251,255,251,255,251,0,107,239,235,96,0,0,41,223,236,
  130,0,0,255,251,61,255,255,255,253,48,7,255,255,255,254,96,0,255,253,
  239,255,255,255,255,226,143,255,255,255,255,245,0,255,255,255,234,155,239,255,
  253,255,253,169,223,255,254,16,255,255,248,16,0,26,255,255,255,112,0,6,
  255,255,128,255,255,144,0,0,0,207,255,248,0,0,0,111,255,192,255,255,
  16,0,0,0,79,255,225,0,0,0,14,255,224,255,252,0,0,0,0,31,
  255,192,0,0,0,12,255,240,255,251,0,0,0,0,15,255,176,0,0,0,
  11,255,240,255,251,0,0,0,0,15,255,176,0,0,0,11,255,240,255,251,
  0,0,0,0,15,255,176,0,0,0,11,255,240,255,251,0,0,0,0,15,
  255,176,0,0,0,11,255,240,255,251,0,0,0,0,15,255,176,0,0,0,
  11,255,240,255,251,0,0,0,0,15,255,176,0,0,0,11,255,240,255,251,
  0,0,0,0,15,255,176,0,0,0,11,255,240,255,251,0,0,0,0,15,
  255,176,0,0,0,11,255,240,255,251,0,0,0,0,15,255,176,0,0,0,
  11,255,240,255,251,0,0,0,0,15,255,176,0,0,0,11,255,240,255,251,
  0,0,0,0,15,255,176,0,0,0,11,255,240,255,251,0,0,0,0,15,
  255,176,0,0,0,11,255,240,255,251,0,90,239,235,96,0,0,255,251,27,
  255,255,255,252,32,0,255,252,223,255,255,255,255,209,0,255,255,255,235,154,
  239,255,251,0,255,255,250,16,0,25,255,255,64,255,255,176,0,0,0,175,
  255,160,255,255,32,0,0,0,47,255,208,255,253,0,0,0,0,13,255,240,
  255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,11,255,240,255,251,
  0,0,0,0,11,255,240,255,251,0,0,0,0,11,255,240,255,251,0,0,
  0,0,11,255,240,255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,
  11,255,240,255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,11,255,
  240,255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,11,255,240,255,
  251,0,0,0,0,11,255,240,0,0,1,123,239,253,165,0,0,0,0,0,
  110,255,255,255,255,212,0,0,0,10,255,255,255,255,255,255,112,0,0,175,
  255,255,185,172,255,255,246,0,6,255,255,129,0,0,59,255,255,48,30,255,
  246,0,0,0,0,175,255,176,111,255,144,0,0,0,0,29,255,242,191,255,
  32,0,0,0,0,7,255,247,239,253,0,0,0,0,0,2,255,249,255,251,
  0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,1,255,251,223,253,
  0,0,0,0,0,3,255,249,175,255,48,0,0,0,0,7,255,246,111,255,
  160,0,0,0,0,29,255,242,30,255,246,0,0,0,0,175,255,160,6,255,
  255,145,0,0,59,255,254,32,0,175,255,255,185,172,255,255,246,0,0,10,
  255,255,255,255,255,255,96,0,0,0,110,255,255,255,255,212,0,0,0,0,
  1,123,239,253,165,0,0,0,255,240,1,107,239,236,130,0,0,0,255,240,
  77,255,255,255,255,145,0,0,255,245,255,255,255,255,255,252,16,0,255,255,
  255,252,169,191,255,255,192,0,255,255,250,32,0,1,143,255,248,0,255,255,
  144,0,0,0,6,255,255,16,255,253,0,0,0,0,0,175,255,112,255,246,
  0,0,0,0,0,47,255,176,255,242,0,0,0,0,0,13,255,224,255,240,
  0,0,0,0,0,11,255,240,255,240,0,0,0,0,0,11,255,240,255,242,
  0,0,0,0,0,13,255,224,255,246,0,0,0,0,0,63,255,176,255,253,
  0,0,0,0,0,175,255,112,255,255,144,0,0,0,6,255,255,32,255,255,
  250,32,0,1,159,255,248,0,255,255,255,252,169,191,255,255,193,0,255,246,
  255,255,255,255,255,253,32,0,255,240,94,255,255,255,255,145,0,0,255,240,
  1,107,239,236,147,0,0,0,255,240,0,0,0,0,0,0,0,0,255,240,
  0,0,0,0,0,0,0,0,255,240,0,0,0,0,0,0,0,0,255,240,
  0,0,0,0,0,0,0,0,255,240,0,0,0,0,0,0,0,0,255,240,
  0,0,0,0,0,0,0,0,255,240,0,0,0,0,0,0,0,0,255,240,
  0,0,0,0,0,0,0,0,0,0,2,140,239,235,97,2,255,248,0,1,
  159,255,255,255,253,66,255,248,0,28,255,255,255,255,255,246,255,248,0,207,
  255,255,185,172,255,255,255,248,8,255,255,129,0,0,43,255,255,248,31,255,
  246,0,0,0,0,175,255,248,127,255,160,0,0,0,0,30,255,248,191,255,
  32,0,0,0,0,8,255,248,239,253,0,0,0,0,0,4,255,248,255,251,
  0,0,0,0,0,3,255,248,255,251,0,0,0,0,0,3,255,248,239,253,
  0,0,0,0,0,4,255,248,191,255,32,0,0,0,0,8,255,248,127,255,
  160,0,0,0,0,30,255,248,47,255,246,0,0,0,0,191,255,248,8,255,
  255,145,0,0,43,255,255,248,1,207,255,255,185,172,255,255,255,248,0,45,
  255,255,255,255,255,246,255,248,0,1,159,255,255,255,253,66,255,248,0,0,
  3,156,239,235,97,2,255,248,0,0,0,0,0,0,0,2,255,248,0,0,
  0,0,0,0,0,2,255,248,0,0,0,0,0,0,0,2,255,248,0,0,
  0,0,0,0,0,2,255,248,0,0,0,0,0,0,0,2,255,248,0,0,
  0,0,0,0,0,2,255,248,0,0,0,0,0,0,0,2,255,248,0,0,
  0,0,0,0,0,2,255,248,255,251,2,157,255,199,16,255,251,95,255,255,
  255,210,255,253,255,255,255,255,244,255,255,255,234,155,255,112,255,255,247,0,
  0,55,0,255,255,128,0,0,0,0,255,255,16,0,0,0,0,255,252,0,
  0,0,0,0,255,251,0,0,0,0,0,255,251,0,0,0,0,0,255,251,
  0,0,0,0,0,255,251,0,0,0,0,0,255,251,0,0,0,0,0,255,
  251,0,0,0,0,0,255,251,0,0,0,0,0,255,251,0,0,0,0,0,
  255,251,0,0,0,0,0,255,251,0,0,0,0,0,255,251,0,0,0,0,
  0,255,251,0,0,0,0,0,0,0,23,190,255,218,64,0,0,3,223,255,
  255,255,251,32,0,46,255,255,255,255,255,210,0,159,255,251,154,223,255,247,
  0,223,255,48,0,4,223,128,0,255,251,0,0,0,22,0,0,239,254,16,
  0,0,0,0,0,175,255,213,0,0,0,0,0,46,255,255,234,81,0,0,
  0,3,223,255,255,254,146,0,0,0,5,191,255,255,254,64,0,0,0,1,
  107,255,255,225,0,0,0,0,0,44,255,247,0,0,0,0,0,2,255,250,
  0,75,16,0,0,1,255,251,5,255,212,0,0,8,255,249,12,255,255,218,
  154,223,255,244,1,207,255,255,255,255,255,176,0,24,255,255,255,255,250,16,
  0,0,39,206,255,217,64,0,0,0,15,255,176,0,0,0,0,15,255,176,
  0,0,0,0,15,255,176,0,0,0,0,15,255,176,0,0,0,0,15,255,
  176,0,0,0,0,15,255,176,0,0,0,0,15,255,176,0,0,0,0,15,
  255,176,0,0,143,255,255,255,255,255,248,143,255,255,255,255,255,248,143,255,
  255,255,255,255,248,73,153,159,255,217,153,148,0,0,15,255,176,0,0,0,
  0,15,255,176,0,0,0,0,15,255,176,0,0,0,0,15,255,176,0,0,
  0,0,15,255,176,0,0,0,0,15,255,176,0,0,0,0,15,255,176,0,
  0,0,0,15,255,176,0,0,0,0,15,255,176,0,0,0,0,15,255,176,
  0,0,0,0,15,255,176,0,0,0,0,15,255,176,0,0,0,0,15,255,
  176,0,0,0,0,15,255,176,0,0,0,0,15,255,176,0,0,0,0,15,
  255,176,0,0,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,
  255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,
  0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,
  255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,
  0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,
  239,252,0,0,0,0,207,254,207,255,16,0,0,1,255,252,143,255,128,0,
  0,8,255,248,63,255,246,0,0,111,255,243,9,255,255,218,157,255,255,144,
  1,207,255,255,255,255,252,16,0,26,255,255,255,255,161,0,0,0,73,223,
  253,164,0,0,95,255,160,0,0,0,0,1,255,252,13,255,241,0,0,0,
  0,7,255,245,7,255,247,0,0,0,0,13,255,208,1,239,253,0,0,0,
  0,95,255,112,0,159,255,80,0,0,0,191,255,16,0,63,255,176,0,0,
  2,255,249,0,0,11,255,242,0,0,8,255,243,0,0,4,255,248,0,0,
  30,255,176,0,0,0,223,254,0,0,111,255,64,0,0,0,111,255,96,0,
  207,253,0,0,0,0,30,255,192,3,255,246,0,0,0,0,8,255,243,9,
  255,225,0,0,0,0,2,255,249,31,255,128,0,0,0,0,0,175,254,143,
  255,32,0,0,0,0,0,79,255,255,250,0,0,0,0,0,0,12,255,255,
  244,0,0,0,0,0,0,6,255,255,192,0,0,0,0,0,0,0,239,255,
  96,0,0,0,0,0,0,0,127,254,0,0,0,0,0,0,0,0,31,247,
  0,0,0,0,79,255,112,0,0,0,3,255,48,0,0,0,7,255,244,13,
  255,208,0,0,0,9,255,144,0,0,0,13,255,208,8,255,243,0,0,0,
  14,255,224,0,0,0,63,255,128,2,255,248,0,0,0,79,255,244,0,0,
  0,143,255,32,0,191,253,0,0,0,159,255,249,0,0,0,223,251,0,0,
  111,255,64,0,0,239,255,254,0,0,4,255,246,0,0,30,255,144,0,4,
  255,255,255,64,0,9,255,225,0,0,9,255,224,0,10,255,186,255,160,0,
  14,255,144,0,0,4,255,244,0,30,255,85,255,225,0,79,255,64,0,0,
  0,223,250,0,95,254,16,239,245,0,175,253,0,0,0,0,143,254,16,175,
  250,0,159,250,1,239,248,0,0,0,0,47,255,81,255,244,0,79,255,21,
  255,242,0,0,0,0,11,255,166,255,208,0,13,255,106,255,192,0,0,0,
  0,5,255,252,255,128,0,8,255,207,255,96,0,0,0,0,1,239,255,255,
  48,0,2,255,255,254,16,0,0,0,0,0,159,255,252,0,0,0,207,255,
  250,0,0,0,0,0,0,79,255,247,0,0,0,111,255,244,0,0,0,0,
  0,0,13,255,241,0,0,0,31,255,208,0,0,0,0,0,0,7,255,176,
  0,0,0,11,255,128,0,0,0,0,0,0,2,255,80,0,0,0,5,255,
  32,0,0,0,9,255,251,0,0,0,0,111,255,192,1,223,255,112,0,0,
  2,239,254,32,0,63,255,243,0,0,11,255,245,0,0,7,255,252,0,0,
  111,255,160,0,0,0,207,255,128,2,239,253,16,0,0,0,46,255,243,11,
  255,244,0,0,0,0,5,255,253,127,255,128,0,0,0,0,0,175,255,255,
  252,0,0,0,0,0,0,29,255,255,243,0,0,0,0,0,0,4,255,255,
  128,0,0,0,0,0,0,9,255,255,209,0,0,0,0,0,0,95,255,255,
  250,0,0,0,0,0,2,239,254,223,255,80,0,0,0,0,11,255,245,79,
  255,225,0,0,0,0,127,255,160,9,255,251,0,0,0,3,255,253,16,1,
  223,255,96,0,0,12,255,244,0,0,79,255,226,0,0,143,255,144,0,0,
  9,255,251,0,4,255,253,16,0,0,1,223,255,112,29,255,244,0,0,0,
  0,63,255,243,47,255,208,0,0,0,0,4,255,250,11,255,243,0,0,0,
  0,10,255,244,5,255,249,0,0,0,0,47,255,192,0,239,254,16,0,0,
  0,143,255,96,0,143,255,96,0,0,0,239,254,0,0,47,255,192,0,0,
  5,255,247,0,0,10,255,243,0,0,11,255,225,0,0,4,255,249,0,0,
  47,255,144,0,0,0,223,254,0,0,159,255,32,0,0,0,127,255,80,1,
  239,251,0,0,0,0,31,255,176,6,255,244,0,0,0,0,9,255,242,12,
  255,192,0,0,0,0,3,255,248,63,255,96,0,0,0,0,0,207,254,175,
  254,0,0,0,0,0,0,111,255,255,247,0,0,0,0,0,0,30,255,255,
  241,0,0,0,0,0,0,9,255,255,144,0,0,0,0,0,0,2,255,255,
  32,0,0,0,0,0,0,1,255,251,0,0,0,0,0,0,0,8,255,244,
  0,0,0,0,0,0,0,30,255,192,0,0,0,0,0,0,0,127,255,96,
  0,0,0,0,0,0,1,239,253,0,0,0,0,0,0,0,7,255,247,0,
  0,0,0,0,0,0,13,255,225,0,0,0,0,0,0,0,111,255,128,0,
  0,0,0,0,0,0,223,255,32,0,0,0,0,0,0,5,255,250,0,0,
  0,0,0,0,15,255,255,255,255,255,255,255,15,255,255,255,255,255,255,255,
  15,255,255,255,255,255,255,251,9,153,153,153,153,255,255,225,0,0,0,0,
  8,255,255,64,0,0,0,0,63,255,248,0,0,0,0,1,223,255,192,0,
  0,0,0,10,255,254,32,0,0,0,0,95,255,245,0,0,0,0,2,239,
  255,144,0,0,0,0,11,255,252,0,0,0,0,0,127,255,226,0,0,0,
  0,3,255,255,80,0,0,0,0,29,255,249,0,0,0,0,0,159,255,209,
  0,0,0,0,5,255,255,48,0,0,0,0,46,255,253,153,153,153,153,144,
  191,255,255,255,255,255,255,240,255,255,255,255,255,255,255,240,255,255,255,255,
  255,255,255,240,0,0,23,206,255,128,0,2,223,255,255,128,0,11,255,255,
  238,112,0,63,255,113,0,0,0,111,250,0,0,0,0,143,247,0,0,0,
  0,143,247,0,0,0,0,111,248,0,0,0,0,95,249,0,0,0,0,63,
  251,0,0,0,0,47,252,0,0,0,0,31,253,0,0,0,0,15,254,0,
  0,0,0,31,253,0,0,0,1,143,249,0,0,0,255,255,210,0,0,0,
  255,255,64,0,0,0,221,255,227,0,0,0,0,111,250,0,0,0,0,15,
  253,0,0,0,0,15,254,0,0,0,0,31,253,0,0,0,0,47,252,0,
  0,0,0,79,251,0,0,0,0,95,249,0,0,0,0,111,248,0,0,0,
  0,143,247,0,0,0,0,143,247,0,0,0,0,111,251,0,0,0,0,63,
  255,129,0,0,0,11,255,255,255,112,0,1,223,255,255,128,0,0,23,206,
  255,128,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,
  255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,
  255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,
  255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,251,255,253,163,0,
  0,0,255,255,255,112,0,0,238,255,255,244,0,0,0,5,239,250,0,0,
  0,0,127,254,0,0,0,0,79,255,0,0,0,0,79,255,0,0,0,0,
  95,254,0,0,0,0,111,252,0,0,0,0,143,251,0,0,0,0,159,250,
  0,0,0,0,175,248,0,0,0,0,191,247,0,0,0,0,159,248,0,0,
  0,0,111,254,48,0,0,0,12,255,255,128,0,0,2,239,255,128,0,0,
  29,255,237,96,0,0,127,252,16,0,0,0,175,247,0,0,0,0,191,247,
  0,0,0,0,175,248,0,0,0,0,159,250,0,0,0,0,127,251,0,0,
  0,0,111,253,0,0,0,0,95,254,0,0,0,0,79,255,0,0,0,0,
  79,255,0,0,0,0,143,254,0,0,0,21,239,250,0,0,255,255,255,244,
  0,0,255,255,255,112,0,0,255,253,163,0,0,0,0,4,137,133,0,0,
  0,1,0,2,207,255,255,214,0,0,110,80,46,255,255,255,255,234,156,255,
  244,175,255,255,255,255,255,255,255,226,29,249,32,22,223,255,255,254,48,2,
  96,0,0,5,190,253,129,0,0,0,0,0,0,23,0,0,0,0,0,0,
  0,0,0,191,128,0,0,0,0,0,0,0,8,255,247,0,0,0,0,0,
  0,0,79,255,248,0,0,0,0,0,0,2,239,255,96,0,0,0,0,0,
  0,12,255,228,0,0,0,0,0,0,0,143,254,48,0,0,0,0,0,0,
  0,62,194,0,0,0,0,0,0,0,0,3,16,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  3,140,239,235,96,11,255,240,0,1,159,255,255,255,253,59,255,240,0,28,
  255,255,255,255,255,253,255,240,0,207,255,255,185,172,255,255,255,240,8,255,
  255,129,0,0,42,255,255,240,30,255,246,0,0,0,0,143,255,240,127,255,
  160,0,0,0,0,13,255,240,191,255,32,0,0,0,0,7,255,240,239,253,
  0,0,0,0,0,4,255,240,255,251,0,0,0,0,0,2,255,240,255,251,
  0,0,0,0,0,3,255,240,239,253,0,0,0,0,0,5,255,240,191,255,
  32,0,0,0,0,8,255,240,127,255,160,0,0,0,0,13,255,240,31,255,
  246,0,0,0,0,159,255,240,8,255,255,145,0,0,42,255,255,240,1,207,
  255,255,185,156,255,255,255,240,0,44,255,255,255,255,255,253,255,240,0,1,
  159,255,255,255,253,75,255,240,0,0,3,156,239,235,97,11,255,240,0,0,
  0,0,0,84,0,0,0,0,0,0,0,0,2,239,64,0,0,0,0,0,
  0,0,12,255,227,0,0,0,0,0,0,0,159,255,229,0,0,0,0,0,
  0,6,255,254,48,0,0,0,0,0,0,63,255,194,0,0,0,0,0,0,
  0,223,251,16,0,0,0,0,0,0,0,127,144,0,0,0,0,0,0,0,
  0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,1,107,239,253,165,0,0,0,0,0,
  110,255,255,255,255,195,0,0,0,10,255,255,255,255,255,254,64,0,0,175,
  255,254,185,172,255,255,226,0,6,255,255,113,0,0,60,255,251,0,30,255,
  244,0,0,0,1,223,255,48,111,255,128,0,0,0,0,95,255,144,191,255,
  16,0,0,0,0,14,255,192,239,255,255,255,255,255,255,255,255,224,255,255,
  255,255,255,255,255,255,255,240,255,255,255,255,255,255,255,255,255,224,223,255,
  153,153,153,153,153,153,153,128,191,255,32,0,0,0,0,0,0,0,111,255,
  144,0,0,0,0,0,0,0,30,255,246,0,0,0,0,38,0,0,6,255,
  255,145,0,0,6,239,144,0,0,175,255,255,201,155,239,255,247,0,0,10,
  255,255,255,255,255,255,177,0,0,0,110,255,255,255,255,232,0,0,0,0,
  1,106,223,254,183,16,0,0,0,0,0,16,0,0,0,8,177,0,0,0,
  95,251,16,0,3,239,255,160,0,29,255,252,32,0,191,255,161,0,8,255,
  248,0,0,30,255,80,0,0,3,195,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,15,255,176,0,0,15,255,176,0,0,15,
  255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,
  255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,
  255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,
  255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,
  255,176,0,0,15,255,176,0,0,0,0,0,0,53,0,0,0,0,0,0,
  0,0,1,223,80,0,0,0,0,0,0,0,11,255,245,0,0,0,0,0,
  0,0,143,255,246,0,0,0,0,0,0,5,255,254,64,0,0,0,0,0,
  0,46,255,210,0,0,0,0,0,0,0,223,252,16,0,0,0,0,0,0,
  0,127,160,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  1,123,239,253,165,0,0,0,0,0,110,255,255,255,255,212,0,0,0,10,
  255,255,255,255,255,255,112,0,0,175,255,255,185,172,255,255,246,0,6,255,
  255,129,0,0,59,255,255,48,30,255,246,0,0,0,0,175,255,176,111,255,
  144,0,0,0,0,29,255,242,191,255,32,0,0,0,0,7,255,247,239,253,
  0,0,0,0,0,2,255,249,255,251,0,0,0,0,0,0,255,251,255,251,
  0,0,0,0,0,1,255,251,223,253,0,0,0,0,0,3,255,249,175,255,
  48,0,0,0,0,7,255,246,111,255,160,0,0,0,0,29,255,242,30,255,
  246,0,0,0,0,175,255,160,6,255,255,145,0,0,59,255,254,32,0,175,
  255,255,185,172,255,255,246,0,0,10,255,255,255,255,255,255,96,0,0,0,
  110,255,255,255,255,212,0,0,0,0,1,123,239,253,165,0,0,0,0,0,
  0,0,1,0,0,0,0,0,0,0,108,16,0,0,0,0,0,3,255,176,
  0,0,0,0,0,29,255,249,0,0,0,0,0,159,255,193,0,0,0,0,
  5,255,251,16,0,0,0,0,46,255,160,0,0,0,0,0,143,248,0,0,
  0,0,0,0,11,112,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,251,0,0,0,0,
  191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,
  0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,
  191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,
  0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,
  191,255,255,251,0,0,0,0,191,255,239,252,0,0,0,0,207,254,207,255,
  16,0,0,1,255,252,143,255,128,0,0,8,255,248,63,255,246,0,0,111,
  255,243,9,255,255,218,157,255,255,144,1,207,255,255,255,255,252,16,0,26,
  255,255,255,255,161,0,0,0,73,223,253,164,0,0,0,44,252,32,0,44,
  252,32,0,207,255,192,0,207,255,192,0,255,255,240,0,255,255,240,0,207,
  255,192,0,207,255,192,0,44,252,32,0,44,252,32,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,251,
  0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,
  191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,
  0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,
  191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,
  0,0,0,0,191,255,255,251,0,0,0,0,191,255,239,252,0,0,0,0,
  207,254,207,255,16,0,0,1,255,252,143,255,128,0,0,8,255,248,63,255,
  246,0,0,111,255,243,9,255,255,218,157,255,255,144,1,207,255,255,255,255,
  252,16,0,26,255,255,255,255,161,0,0,0,73,223,253,164,0,0,0,0,
  1,16,0,0,0,0,0,0,42,255,251,48,0,5,32,0,3,239,255,255,
  250,68,143,210,0,13,255,255,255,255,255,255,248,0,4,237,66,57,255,255,
  255,144,0,0,49,0,0,58,239,198,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,
  251,0,90,239,235,96,0,0,255,251,27,255,255,255,252,32,0,255,252,223,
  255,255,255,255,209,0,255,255,255,235,154,239,255,251,0,255,255,250,16,0,
  25,255,255,64,255,255,176,0,0,0,175,255,160,255,255,32,0,0,0,47,
  255,208,255,253,0,0,0,0,13,255,240,255,251,0,0,0,0,11,255,240,
  255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,11,255,240,255,251,
  0,0,0,0,11,255,240,255,251,0,0,0,0,11,255,240,255,251,0,0,
  0,0,11,255,240,255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,
  11,255,240,255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,11,255,
  240,255,251,0,0,0,0,11,255,240,255,251,0,0,0,0,11,255,240,0,
  0,0,0,0,0,1,214,0,0,0,0,0,0,0,0,0,0,0,0,11,
  255,96,0,0,0,0,0,0,0,0,0,0,0,143,255,245,0,0,0,0,
  0,0,0,0,0,0,5,255,255,144,0,0,0,0,0,0,0,0,0,0,
  62,255,246,0,0,0,0,0,0,0,0,0,0,1,223,254,64,0,0,0,
  0,0,0,0,0,0,0,7,255,194,0,0,0,0,0,0,0,0,0,0,
  0,0,154,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,255,
  242,0,0,0,0,0,0,0,0,0,0,0,7,255,248,0,0,0,0,0,
  0,0,0,0,0,0,13,255,253,0,0,0,0,0,0,0,0,0,0,0,
  79,255,255,80,0,0,0,0,0,0,0,0,0,0,175,255,255,176,0,0,
  0,0,0,0,0,0,0,1,255,255,255,242,0,0,0,0,0,0,0,0,
  0,7,255,252,255,247,0,0,0,0,0,0,0,0,0,13,255,226,255,253,
  0,0,0,0,0,0,0,0,0,79,255,160,175,255,64,0,0,0,0,0,
  0,0,0,175,255,64,79,255,160,0,0,0,0,0,0,0,1,255,253,0,
  13,255,241,0,0,0,0,0,0,0,7,255,247,0,7,255,247,0,0,0,
  0,0,0,0,13,255,241,0,2,255,253,0,0,0,0,0,0,0,79,255,
  160,0,0,191,255,64,0,0,0,0,0,0,175,255,64,0,0,95,255,160,
  0,0,0,0,0,1,255,253,0,0,0,14,255,241,0,0,0,0,0,7,
  255,247,0,0,0,8,255,247,0,0,0,0,0,13,255,242,0,0,0,2,
  255,253,0,0,0,0,0,63,255,176,0,0,0,0,191,255,64,0,0,0,
  0,159,255,255,255,255,255,255,255,255,160,0,0,0,1,239,255,255,255,255,
  255,255,255,255,241,0,0,0,6,255,255,255,255,255,255,255,255,255,247,0,
  0,0,12,255,249,153,153,153,153,153,154,255,252,0,0,0,63,255,176,0,
  0,0,0,0,0,207,255,48,0,0,159,255,80,0,0,0,0,0,0,111,
  255,144,0,1,239,254,0,0,0,0,0,0,0,31,255,225,0,6,255,249,
  0,0,0,0,0,0,0,10,255,246,0,12,255,243,0,0,0,0,0,0,
  0,4,255,252,0,63,255,192,0,0,0,0,0,0,0,0,223,255,48,0,
  0,0,0,3,227,0,0,0,0,0,0,0,0,29,254,48,0,0,0,0,
  0,0,0,191,255,225,0,0,0,0,0,0,7,255,254,80,0,0,0,0,
  0,0,79,255,211,0,0,0,0,0,0,2,239,252,32,0,0,0,0,0,
  0,8,255,161,0,0,0,0,0,0,0,0,168,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,255,255,255,255,255,255,255,255,255,128,255,
  255,255,255,255,255,255,255,255,128,255,255,255,255,255,255,255,255,255,128,255,
  253,153,153,153,153,153,153,153,64,255,251,0,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,0,255,253,153,153,153,153,153,153,144,0,255,
  255,255,255,255,255,255,255,240,0,255,255,255,255,255,255,255,255,240,0,255,
  255,255,255,255,255,255,255,240,0,255,251,0,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,
  251,0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,255,
  253,153,153,153,153,153,153,153,64,255,255,255,255,255,255,255,255,255,128,255,
  255,255,255,255,255,255,255,255,128,255,255,255,255,255,255,255,255,255,128,0,
  0,8,177,0,0,0,95,251,0,0,2,239,255,144,0,28,255,252,32,0,
  175,255,161,0,6,255,248,0,0,13,255,96,0,0,2,212,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,255,176,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  0,0,0,0,0,0,39,0,0,0,0,0,0,0,0,0,0,0,0,1,
  223,128,0,0,0,0,0,0,0,0,0,0,0,10,255,247,0,0,0,0,
  0,0,0,0,0,0,0,127,255,248,0,0,0,0,0,0,0,0,0,0,
  4,255,255,96,0,0,0,0,0,0,0,0,0,0,46,255,228,0,0,0,
  0,0,0,0,0,0,0,0,207,252,32,0,0,0,0,0,0,0,0,0,
  0,0,111,161,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,156,239,
  254,200,64,0,0,0,0,0,0,0,7,223,255,255,255,255,253,96,0,0,
  0,0,0,3,207,255,255,255,255,255,255,252,32,0,0,0,0,78,255,255,
  253,169,155,223,255,255,228,0,0,0,4,255,255,232,32,0,0,3,159,255,
  255,64,0,0,46,255,252,32,0,0,0,0,2,207,255,226,0,0,191,255,
  176,0,0,0,0,0,0,28,255,251,0,5,255,253,16,0,0,0,0,0,
  0,1,223,255,80,12,255,243,0,0,0,0,0,0,0,0,79,255,192,63,
  255,176,0,0,0,0,0,0,0,0,11,255,243,143,255,80,0,0,0,0,
  0,0,0,0,5,255,247,191,255,16,0,0,0,0,0,0,0,0,1,255,
  251,223,253,0,0,0,0,0,0,0,0,0,0,223,253,255,251,0,0,0,
  0,0,0,0,0,0,0,207,254,255,251,0,0,0,0,0,0,0,0,0,
  0,191,255,239,252,0,0,0,0,0,0,0,0,0,0,191,254,223,253,0,
  0,0,0,0,0,0,0,0,0,223,253,191,255,16,0,0,0,0,0,0,
  0,0,1,255,251,127,255,80,0,0,0,0,0,0,0,0,5,255,247,47,
  255,176,0,0,0,0,0,0,0,0,11,255,243,11,255,244,0,0,0,0,
  0,0,0,0,79,255,192,4,255,253,16,0,0,0,0,0,0,1,223,255,
  64,0,191,255,177,0,0,0,0,0,0,28,255,251,0,0,30,255,252,32,
  0,0,0,0,2,207,255,226,0,0,3,239,255,249,32,0,0,2,159,255,
  254,48,0,0,0,62,255,255,253,169,154,223,255,255,228,0,0,0,0,2,
  207,255,255,255,255,255,255,252,32,0,0,0,0,0,6,223,255,255,255,255,
  253,96,0,0,0,0,0,0,0,4,140,239,254,201,64,0,0,0,0,0,
  0,0,0,0,8,193,0,0,0,0,0,0,0,0,0,95,252,16,0,0,
  0,0,0,0,0,3,239,255,176,0,0,0,0,0,0,0,29,255,253,32,
  0,0,0,0,0,0,0,191,255,177,0,0,0,0,0,0,0,8,255,248,
  0,0,0,0,0,0,0,0,30,255,96,0,0,0,0,0,0,0,0,3,
  211,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,
  255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,
  0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,
  0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,
  0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,
  251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,
  251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,
  0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,
  0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,
  0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,
  0,0,0,0,0,0,0,255,251,239,253,0,0,0,0,0,0,2,255,250,
  191,255,16,0,0,0,0,0,5,255,247,127,255,112,0,0,0,0,0,11,
  255,243,47,255,226,0,0,0,0,0,79,255,208,10,255,252,16,0,0,0,
  3,239,255,96,2,239,255,230,0,0,1,127,255,251,0,0,79,255,255,235,
  154,191,255,255,210,0,0,4,239,255,255,255,255,255,252,32,0,0,0,42,
  255,255,255,255,255,129,0,0,0,0,0,56,206,255,219,113,0,0,0,0,
  0,44,252,32,0,44,252,32,0,0,0,0,207,255,192,0,207,255,192,0,
  0,0,0,255,255,240,0,255,255,240,0,0,0,0,207,255,192,0,207,255,
  192,0,0,0,0,44,252,32,0,44,252,32,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,255,251,
  0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,
  255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,
  255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,
  0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,
  0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,
  0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,
  251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,
  251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,0,
  0,255,251,255,251,0,0,0,0,0,0,0,255,251,255,251,0,0,0,0,
  0,0,0,255,251,255,251,0,0,0,0,0,0,0,255,251,239,253,0,0,
  0,0,0,0,2,255,250,191,255,16,0,0,0,0,0,5,255,247,127,255,
  112,0,0,0,0,0,11,255,243,47,255,226,0,0,0,0,0,79,255,208,
  10,255,252,16,0,0,0,3,239,255,96,2,239,255,230,0,0,1,127,255,
  251,0,0,79,255,255,235,154,191,255,255,210,0,0,4,239,255,255,255,255,
  255,252,32,0,0,0,42,255,255,255,255,255,129,0,0,0,0,0,56,206,
  255,219,113,0,0,0,0,0,0,1,16,0,0,0,0,0,0,0,0,5,
  223,254,129,0,0,112,0,0,0,0,143,255,255,254,115,75,250,0,0,0,
  5,255,255,255,255,255,255,254,32,0,0,0,175,131,38,223,255,255,228,0,
  0,0,0,4,0,0,7,223,234,32,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,255,225,0,0,0,0,0,0,0,255,251,255,251,0,0,
  0,0,0,0,0,255,251,255,255,96,0,0,0,0,0,0,255,251,255,255,
  243,0,0,0,0,0,0,255,251,255,255,253,16,0,0,0,0,0,255,251,
  255,255,255,144,0,0,0,0,0,255,251,255,255,255,245,0,0,0,0,0,
  255,251,255,253,255,254,32,0,0,0,0,255,251,255,251,127,255,192,0,0,
  0,0,255,251,255,251,11,255,248,0,0,0,0,255,251,255,251,1,239,255,
  64,0,0,0,255,251,255,251,0,79,255,225,0,0,0,255,251,255,251,0,
  8,255,251,0,0,0,255,251,255,251,0,0,207,255,112,0,0,255,251,255,
  251,0,0,46,255,243,0,0,255,251,255,251,0,0,5,255,253,16,0,255,
  251,255,251,0,0,0,175,255,160,0,255,251,255,251,0,0,0,29,255,245,
  0,255,251,255,251,0,0,0,3,255,254,32,255,251,255,251,0,0,0,0,
  127,255,192,255,251,255,251,0,0,0,0,11,255,248,255,251,255,251,0,0,
  0,0,1,239,255,255,251,255,251,0,0,0,0,0,79,255,255,251,255,251,
  0,0,0,0,0,8,255,255,251,255,251,0,0,0,0,0,0,207,255,251,
  255,251,0,0,0,0,0,0,46,255,251,255,251,0,0,0,0,0,0,5,
  255,251,255,251,0,0,0,0,0,0,0,175,251,255,251,0,0,0,0,0,
  0,0,31,251,0,0,0,1,48,0,0,0,0,0,0,0,0,11,209,0,
  0,0,0,0,0,0,0,191,251,0,0,0,0,0,0,0,4,255,255,112,
  0,0,0,0,0,0,0,94,255,244,0,0,0,0,0,0,0,3,239,254,
  32,0,0,0,0,0,0,0,45,255,176,0,0,0,0,0,0,0,1,191,
  193,0,0,0,0,0,0,0,0,8,16,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,140,239,235,
  96,11,255,240,0,1,159,255,255,255,253,59,255,240,0,28,255,255,255,255,
  255,253,255,240,0,207,255,255,185,172,255,255,255,240,8,255,255,129,0,0,
  42,255,255,240,30,255,246,0,0,0,0,143,255,240,127,255,160,0,0,0,
  0,13,255,240,191,255,32,0,0,0,0,7,255,240,239,253,0,0,0,0,
  0,4,255,240,255,251,0,0,0,0,0,2,255,240,255,251,0,0,0,0,
  0,3,255,240,239,253,0,0,0,0,0,5,255,240,191,255,32,0,0,0,
  0,8,255,240,127,255,160,0,0,0,0,13,255,240,31,255,246,0,0,0,
  0,159,255,240,8,255,255,145,0,0,42,255,255,240,1,207,255,255,185,156,
  255,255,255,240,0,44,255,255,255,255,255,253,255,240,0,1,159,255,255,255,
  253,75,255,240,0,0,3,156,239,235,97,11,255,240,0,0,0,2,16,0,
  0,0,0,0,0,0,0,62,160,0,0,0,0,0,0,0,2,239,247,0,
  0,0,0,0,0,0,9,255,255,48,0,0,0,0,0,0,0,159,255,209,
  0,0,0,0,0,0,0,7,255,251,0,0,0,0,0,0,0,0,94,255,
  128,0,0,0,0,0,0,0,3,239,144,0,0,0,0,0,0,0,0,40,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,1,107,239,253,165,0,0,0,0,0,110,255,255,255,
  255,195,0,0,0,10,255,255,255,255,255,254,64,0,0,175,255,254,185,172,
  255,255,226,0,6,255,255,113,0,0,60,255,251,0,30,255,244,0,0,0,
  1,223,255,48,111,255,128,0,0,0,0,95,255,144,191,255,16,0,0,0,
  0,14,255,192,239,255,255,255,255,255,255,255,255,224,255,255,255,255,255,255,
  255,255,255,240,255,255,255,255,255,255,255,255,255,224,223,255,153,153,153,153,
  153,153,153,128,191,255,32,0,0,0,0,0,0,0,111,255,144,0,0,0,
  0,0,0,0,30,255,246,0,0,0,0,38,0,0,6,255,255,145,0,0,
  6,239,144,0,0,175,255,255,201,155,239,255,247,0,0,10,255,255,255,255,
  255,255,177,0,0,0,110,255,255,255,255,232,0,0,0,0,1,106,223,254,
  183,16,0,0,0,131,0,0,0,8,253,16,0,0,143,255,192,0,0,111,
  255,249,0,0,3,239,255,96,0,0,44,255,244,0,0,1,175,254,0,0,
  0,8,246,0,0,0,0,32,0,0,0,0,0,0,0,0,0,0,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,15,255,176,0,0,
  0,0,2,16,0,0,0,0,0,0,0,0,46,160,0,0,0,0,0,0,
  0,2,223,247,0,0,0,0,0,0,0,9,255,255,64,0,0,0,0,0,
  0,0,159,255,226,0,0,0,0,0,0,0,6,255,252,0,0,0,0,0,
  0,0,0,78,255,144,0,0,0,0,0,0,0,3,223,160,0,0,0,0,
  0,0,0,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,1,123,239,253,165,0,0,0,0,
  0,110,255,255,255,255,212,0,0,0,10,255,255,255,255,255,255,112,0,0,
  175,255,255,185,172,255,255,246,0,6,255,255,129,0,0,59,255,255,48,30,
  255,246,0,0,0,0,175,255,176,111,255,144,0,0,0,0,29,255,242,191,
  255,32,0,0,0,0,7,255,247,239,253,0,0,0,0,0,2,255,249,255,
  251,0,0,0,0,0,0,255,251,255,251,0,0,0,0,0,1,255,251,223,
  253,0,0,0,0,0,3,255,249,175,255,48,0,0,0,0,7,255,246,111,
  255,160,0,0,0,0,29,255,242,30,255,246,0,0,0,0,175,255,160,6,
  255,255,145,0,0,59,255,254,32,0,175,255,255,185,172,255,255,246,0,0,
  10,255,255,255,255,255,255,96,0,0,0,110,255,255,255,255,212,0,0,0,
  0,1,123,239,253,165,0,0,0,0,0,4,112,0,0,0,0,0,0,62,
  244,0,0,0,0,0,3,239,253,16,0,0,0,0,2,207,255,176,0,0,
  0,0,0,27,255,247,0,0,0,0,0,0,175,255,48,0,0,0,0,0,
  8,255,208,0,0,0,0,0,0,127,80,0,0,0,0,0,0,2,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,251,0,
  0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,
  255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,
  0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,
  255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,
  0,0,0,191,255,255,251,0,0,0,0,191,255,239,252,0,0,0,0,207,
  254,207,255,16,0,0,1,255,252,143,255,128,0,0,8,255,248,63,255,246,
  0,0,111,255,243,9,255,255,218,157,255,255,144,1,207,255,255,255,255,252,
  16,0,26,255,255,255,255,161,0,0,0,73,223,253,164,0,0,0,0,0,
  0,175,253,16,0,0,0,0,0,0,7,255,255,176,0,0,0,0,0,0,
  79,255,255,249,0,0,0,0,0,2,239,254,223,255,112,0,0,0,0,29,
  255,227,28,255,245,0,0,0,0,175,253,32,1,207,255,64,0,0,5,255,
  194,0,0,27,255,208,0,0,0,172,16,0,0,0,174,48,0,0,0,1,
  0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,3,140,239,235,96,11,255,240,0,1,159,
  255,255,255,253,59,255,240,0,28,255,255,255,255,255,253,255,240,0,207,255,
  255,185,172,255,255,255,240,8,255,255,129,0,0,42,255,255,240,30,255,246,
  0,0,0,0,143,255,240,127,255,160,0,0,0,0,13,255,240,191,255,32,
  0,0,0,0,7,255,240,239,253,0,0,0,0,0,4,255,240,255,251,0,
  0,0,0,0,2,255,240,255,251,0,0,0,0,0,3,255,240,239,253,0,
  0,0,0,0,5,255,240,191,255,32,0,0,0,0,8,255,240,127,255,160,
  0,0,0,0,13,255,240,31,255,246,0,0,0,0,159,255,240,8,255,255,
  145,0,0,42,255,255,240,1,207,255,255,185,156,255,255,255,240,0,44,255,
  255,255,255,255,253,255,240,0,1,159,255,255,255,253,75,255,240,0,0,3,
  156,239,235,97,11,255,240,0,0,0,1,223,249,0,0,0,0,0,0,0,
  12,255,255,112,0,0,0,0,0,0,159,255,255,244,0,0,0,0,0,6,
  255,253,255,254,32,0,0,0,0,79,255,177,78,255,209,0,0,0,2,239,
  250,0,3,239,251,0,0,0,11,255,144,0,0,45,255,112,0,0,2,215,
  0,0,0,2,219,16,0,0,0,16,0,0,0,0,17,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  107,239,253,165,0,0,0,0,0,110,255,255,255,255,195,0,0,0,10,255,
  255,255,255,255,254,64,0,0,175,255,254,185,172,255,255,226,0,6,255,255,
  113,0,0,60,255,251,0,30,255,244,0,0,0,1,223,255,48,111,255,128,
  0,0,0,0,95,255,144,191,255,16,0,0,0,0,14,255,192,239,255,255,
  255,255,255,255,255,255,224,255,255,255,255,255,255,255,255,255,240,255,255,255,
  255,255,255,255,255,255,224,223,255,153,153,153,153,153,153,153,128,191,255,32,
  0,0,0,0,0,0,0,111,255,144,0,0,0,0,0,0,0,30,255,246,
  0,0,0,0,38,0,0,6,255,255,145,0,0,6,239,144,0,0,175,255,
  255,201,155,239,255,247,0,0,10,255,255,255,255,255,255,177,0,0,0,110,
  255,255,255,255,232,0,0,0,0,1,106,223,254,183,16,0,0,0,0,0,
  104,132,0,0,0,0,0,7,255,255,64,0,0,0,0,95,255,255,226,0,
  0,0,4,255,255,255,253,16,0,0,62,255,229,143,255,193,0,2,223,253,
  48,5,239,251,0,12,255,177,0,0,45,255,128,4,233,0,0,0,1,172,
  16,0,32,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,
  0,0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,
  255,251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,
  0,0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,
  255,251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,
  0,0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,
  255,251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,
  0,0,0,0,255,251,0,0,0,0,0,0,255,251,0,0,0,0,0,0,
  255,251,0,0,0,0,0,0,1,223,250,0,0,0,0,0,0,0,11,255,
  255,112,0,0,0,0,0,0,159,255,255,244,0,0,0,0,0,6,255,253,
  239,254,48,0,0,0,0,79,255,193,62,255,209,0,0,0,2,239,251,16,
  3,223,251,0,0,0,10,255,144,0,0,45,255,96,0,0,2,216,0,0,
  0,1,204,16,0,0,0,16,0,0,0,0,17,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,123,239,
  253,165,0,0,0,0,0,110,255,255,255,255,212,0,0,0,10,255,255,255,
  255,255,255,112,0,0,175,255,255,185,172,255,255,246,0,6,255,255,129,0,
  0,59,255,255,48,30,255,246,0,0,0,0,175,255,176,111,255,144,0,0,
  0,0,29,255,242,191,255,32,0,0,0,0,7,255,247,239,253,0,0,0,
  0,0,2,255,249,255,251,0,0,0,0,0,0,255,251,255,251,0,0,0,
  0,0,1,255,251,223,253,0,0,0,0,0,3,255,249,175,255,48,0,0,
  0,0,7,255,246,111,255,160,0,0,0,0,29,255,242,30,255,246,0,0,
  0,0,175,255,160,6,255,255,145,0,0,59,255,254,32,0,175,255,255,185,
  172,255,255,246,0,0,10,255,255,255,255,255,255,96,0,0,0,110,255,255,
  255,255,212,0,0,0,0,1,123,239,253,165,0,0,0,0,0,0,191,251,
  0,0,0,0,0,8,255,255,144,0,0,0,0,111,255,255,246,0,0,0,
  4,255,253,223,255,64,0,0,46,255,194,28,255,226,0,1,223,251,16,1,
  191,253,16,6,255,160,0,0,10,255,96,0,136,0,0,0,0,153,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,
  251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,
  0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,
  251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,
  0,191,255,255,251,0,0,0,0,191,255,255,251,0,0,0,0,191,255,239,
  252,0,0,0,0,207,254,207,255,16,0,0,1,255,252,143,255,128,0,0,
  8,255,248,63,255,246,0,0,111,255,243,9,255,255,218,157,255,255,144,1,
  207,255,255,255,255,252,16,0,26,255,255,255,255,161,0,0,0,73,223,253,
  164,0,0,0,0,0,1,16,0,0,0,0,0,0,0,42,255,252,64,0,
  3,64,0,0,3,239,255,255,251,84,126,227,0,0,30,255,255,255,255,255,
  255,247,0,0,5,251,66,74,255,255,255,128,0,0,0,64,0,0,75,239,
  197,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,140,239,235,96,
  11,255,240,0,1,159,255,255,255,253,59,255,240,0,28,255,255,255,255,255,
  253,255,240,0,207,255,255,185,172,255,255,255,240,8,255,255,129,0,0,42,
  255,255,240,30,255,246,0,0,0,0,143,255,240,127,255,160,0,0,0,0,
  13,255,240,191,255,32,0,0,0,0,7,255,240,239,253,0,0,0,0,0,
  4,255,240,255,251,0,0,0,0,0,2,255,240,255,251,0,0,0,0,0,
  3,255,240,239,253,0,0,0,0,0,5,255,240,191,255,32,0,0,0,0,
  8,255,240,127,255,160,0,0,0,0,13,255,240,31,255,246,0,0,0,0,
  159,255,240,8,255,255,145,0,0,42,255,255,240,1,207,255,255,185,156,255,
  255,255,240,0,44,255,255,255,255,255,253,255,240,0,1,159,255,255,255,253,
  75,255,240,0,0,3,156,239,235,97,11,255,240,0,0,0,17,0,0,0,
  0,0,0,0,0,93,255,232,16,0,7,0,0,0,8,255,255,255,231,69,
  191,144,0,0,95,255,255,255,255,255,255,226,0,0,10,248,50,109,255,255,
  254,64,0,0,0,80,0,0,125,254,162,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,1,123,239,253,165,0,0,0,0,0,110,255,255,255,255,
  212,0,0,0,10,255,255,255,255,255,255,112,0,0,175,255,255,185,172,255,
  255,246,0,6,255,255,129,0,0,59,255,255,48,30,255,246,0,0,0,0,
  175,255,176,111,255,144,0,0,0,0,29,255,242,191,255,32,0,0,0,0,
  7,255,247,239,253,0,0,0,0,0,2,255,249,255,251,0,0,0,0,0,
  0,255,251,255,251,0,0,0,0,0,1,255,251,223,253,0,0,0,0,0,
  3,255,249,175,255,48,0,0,0,0,7,255,246,111,255,160,0,0,0,0,
  29,255,242,30,255,246,0,0,0,0,175,255,160,6,255,255,145,0,0,59,
  255,254,32,0,175,255,255,185,172,255,255,246,0,0,10,255,255,255,255,255,
  255,96,0,0,0,110,255,255,255,255,212,0,0,0,0,1,123,239,253,165,
  0,0,0,0,0,1,107,223,253,182,16,0,0,0,110,255,255,255,255,230,
  0,0,10,255,255,255,255,255,255,144,0,175,255,255,201,155,239,255,243,6,
  255,255,145,0,0,23,255,64,30,255,246,0,0,0,0,52,0,111,255,160,
  0,0,0,0,0,0,191,255,32,0,0,0,0,0,0,239,253,0,0,0,
  0,0,0,0,255,251,0,0,0,0,0,0,0,255,251,0,0,0,0,0,
  0,0,223,253,0,0,0,0,0,0,0,191,255,48,0,0,0,0,0,0,
  111,255,160,0,0,0,0,0,0,30,255,247,0,0,0,0,52,0,6,255,
  255,146,0,0,23,239,64,0,175,255,255,201,155,239,255,243,0,10,255,255,
  255,255,255,255,160,0,0,110,255,255,255,255,230,0,0,0,1,107,255,253,
  182,16,0,0,0,0,5,255,128,0,0,0,0,0,0,10,255,233,32,0,
  0,0,0,0,14,255,255,227,0,0,0,0,0,3,52,175,252,0,0,0,
  0,0,0,0,31,255,0,0,0,0,28,130,1,143,253,0,0,0,0,191,
  255,255,255,244,0,0,0,0,41,223,255,251,48,0,0,0,0,0,1,34,
  0,0,0,0,0,0,0,0,4,140,239,254,218,97,0,0,0,0,0,0,
  6,223,255,255,255,255,255,146,0,0,0,0,2,207,255,255,255,255,255,255,
  254,96,0,0,0,78,255,255,253,185,154,223,255,255,250,0,0,4,255,255,
  249,48,0,0,1,126,255,254,32,0,46,255,252,32,0,0,0,0,1,175,
  227,0,0,191,255,177,0,0,0,0,0,0,7,48,0,5,255,253,16,0,
  0,0,0,0,0,0,0,0,12,255,244,0,0,0,0,0,0,0,0,0,
  0,63,255,176,0,0,0,0,0,0,0,0,0,0,127,255,80,0,0,0,
  0,0,0,0,0,0,0,191,255,16,0,0,0,0,0,0,0,0,0,0,
  223,253,0,0,0,0,0,0,0,0,0,0,0,255,252,0,0,0,0,0,
  0,0,0,0,0,0,255,251,0,0,0,0,0,0,0,0,0,0,0,239,
  252,0,0,0,0,0,0,0,0,0,0,0,223,253,0,0,0,0,0,0,
  0,0,0,0,0,191,255,16,0,0,0,0,0,0,0,0,0,0,127,255,
  80,0,0,0,0,0,0,0,0,0,0,63,255,176,0,0,0,0,0,0,
  0,0,0,0,12,255,244,0,0,0,0,0,0,0,0,0,0,5,255,253,
  16,0,0,0,0,0,0,0,0,0,0,191,255,177,0,0,0,0,0,0,
  6,96,0,0,46,255,252,32,0,0,0,0,0,143,246,0,0,4,255,255,
  249,48,0,0,1,109,255,255,80,0,0,78,255,255,253,185,154,207,255,255,
  251,16,0,0,2,207,255,255,255,255,255,255,255,128,0,0,0,0,6,223,
  255,255,255,255,255,163,0,0,0,0,0,0,4,140,255,254,218,98,0,0,
  0,0,0,0,0,0,5,255,128,0,0,0,0,0,0,0,0,0,0,10,
  255,233,32,0,0,0,0,0,0,0,0,0,14,255,255,227,0,0,0,0,
  0,0,0,0,0,3,52,175,252,0,0,0,0,0,0,0,0,0,0,0,
  31,255,0,0,0,0,0,0,0,0,28,114,1,143,253,0,0,0,0,0,
  0,0,0,191,255,255,255,244,0,0,0,0,0,0,0,0,41,239,255,250,
  48,0,0,0,0,0,0,0,0,0,1,34,0,0,0,0,0,0,0,0,
  2,207,194,0,0,0,0,0,12,255,252,0,0,0,0,0,15,255,255,0,
  0,0,0,0,12,255,252,0,0,0,0,0,2,207,194,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,1,255,240,0,0,0,0,0,
  2,255,240,0,0,0,0,0,3,255,240,0,0,0,0,0,92,255,240,0,
  0,0,0,44,255,255,240,0,0,0,2,223,255,255,240,0,0,0,11,255,
  252,65,0,0,0,0,79,255,176,0,0,0,0,0,175,255,32,0,0,0,
  0,0,223,253,0,0,0,0,0,0,255,251,0,0,0,0,0,0,255,252,
  0,0,0,0,2,0,223,255,16,0,0,0,63,96,175,255,112,0,0,1,
  223,248,95,255,246,0,0,60,255,249,11,255,255,218,156,255,255,209,2,223,
  255,255,255,255,254,48,0,43,255,255,255,255,178,0,0,0,90,239,253,164,
  0,0,44,252,32,207,255,192,255,255,240,207,255,192,44,252,32,0,0,0,
  0,0,0,0,0,0,0,0,0,15,255,176,15,255,176,15,255,176,15,255,
  176,15,255,176,15,255,176,15,255,176,15,255,176,15,255,176,15,255,176,15,
  255,176,15,255,176,15,255,176,15,255,176,15,255,176,15,255,176,15,255,176,
  15,255,176,15,255,176,0,6,206,236,96,0,1,207,255,255,252,16,12,255,
  255,255,255,176,95,255,113,23,255,245,191,248,0,0,143,251,239,242,0,0,
  47,254,255,240,0,0,15,255,239,242,0,0,47,254,191,248,0,0,143,251,
  111,255,96,6,255,246,12,255,255,255,255,192,2,207,255,255,252,32,0,23,
  207,252,113,0,44,252,32,207,255,192,255,255,240,207,255,192,44,252,32,
};
static int fontIdx(uint32_t cp){
  if(cp>=0x20 && cp<=0x7E) return cp-0x20;
  switch(cp){
    case 0xE1: return 95;
    case 0xE9: return 96;
    case 0xED: return 97;
    case 0xF3: return 98;
    case 0xFA: return 99;
    case 0xFC: return 100;
    case 0xF1: return 101;
    case 0xC1: return 102;
    case 0xC9: return 103;
    case 0xCD: return 104;
    case 0xD3: return 105;
    case 0xDA: return 106;
    case 0xDC: return 107;
    case 0xD1: return 108;
    case 0xE0: return 109;
    case 0xE8: return 110;
    case 0xEC: return 111;
    case 0xF2: return 112;
    case 0xF9: return 113;
    case 0xE2: return 114;
    case 0xEA: return 115;
    case 0xEE: return 116;
    case 0xF4: return 117;
    case 0xFB: return 118;
    case 0xE3: return 119;
    case 0xF5: return 120;
    case 0xE7: return 121;
    case 0xC7: return 122;
    case 0xBF: return 123;
    case 0xA1: return 124;
    case 0xB0: return 125;
    case 0xB7: return 126;
    default: return 0x3F-0x20;
  }
}
#define FONT_HPS 8    // px de alto por unidad de 'size' logico
#define FONT_CAPOFF 11 // alinea el tope de mayusculas/digitos con y (como el 5x7)
static inline uint8_t fgPix(const FGlyph* g, int x, int y){
  if(x < 0 || y < 0 || x >= g->w || y >= g->h) return 0;
  int bpr = (g->w + 1) >> 1;
  uint8_t b = FBM[g->off + (uint32_t)y * bpr + (x >> 1)];
  return (x & 1) ? (b & 0x0F) : (b >> 4);
}
static inline float fontSc(int size){ return (float)(size * FONT_HPS) / (float)FONT_LINEH; }
static uint32_t nextCP(const char** ps){
  const char* s = *ps; uint8_t b = (uint8_t)*s++; uint32_t cp;
  if(b < 0x80) cp = b;
  else if((b & 0xE0) == 0xC0){ uint8_t b1 = *s ? (uint8_t)*s++ : 0; cp = ((b & 0x1F) << 6) | (b1 & 0x3F); }
  else if((b & 0xF0) == 0xE0){ if(*s) s++; if(*s) s++; cp = 0x3F; }
  else cp = 0x3F;
  *ps = s; return cp;
}
// Dibuja un glifo escalado (bilineal desde el master 4bpp)
static void drawGlyphScaled(int px0, int py0, const FGlyph* g, float sc, uint16_t col, uint8_t alpha){
  if(g->w == 0) return;
  int tw = (int)(g->w * sc + 0.999f), th = (int)(g->h * sc + 0.999f);
  for(int ty = 0; ty < th; ty++){
    float fy = ty / sc; int y0 = (int)fy; float dyf = fy - y0;
    for(int tx = 0; tx < tw; tx++){
      float fx = tx / sc; int x0 = (int)fx; float dxf = fx - x0;
      float a00 = fgPix(g, x0, y0),     a10 = fgPix(g, x0 + 1, y0);
      float a01 = fgPix(g, x0, y0 + 1), a11 = fgPix(g, x0 + 1, y0 + 1);
      float top = a00 * (1 - dxf) + a10 * dxf;
      float bot = a01 * (1 - dxf) + a11 * dxf;
      float cov = (top * (1 - dyf) + bot * dyf) * 17.0f;   // 0..255
      if(cov > 4.0f){
        int a = (int)(cov * alpha / 255.0f); if(a > 255) a = 255;
        pxA(px0 + tx, py0 + ty, col, (uint8_t)a);
      }
    }
  }
}

static int textW(const char* s, int size){
  if(size <= 1){                 // texto minusculo: bitmap 5x7 nitido (6px monoespaciado)
    int n = 0;
    while(*s){ uint8_t b = (uint8_t)*s++;
      if(b >= 0x80){ if((b & 0xE0) == 0xC0){ if(*s) s++; } else if((b & 0xF0) == 0xE0){ if(*s) s++; if(*s) s++; } }
      n++;
    }
    return n > 0 ? n * 6 - 1 : 0;
  }
  float sc = fontSc(size), w = 0;
  while(*s){ uint32_t cp = nextCP(&s); w += FG[fontIdx(cp)].adv * sc; }
  return (int)(w + 0.5f);
}
// Texto con alpha (workhorse). REGLA: vectorial Outfit para tamano >=2
// (curvas suaves), bitmap 5x7 NITIDO para size 1 (evita el emborronado
// de encoger demasiado una vectorial fina).
static int drawTextA(int x, int y, const char* s, int size, uint16_t col, uint8_t alpha){
  if(size <= 1){
    while(*s){
      uint8_t b = (uint8_t)*s++; uint32_t cp;
      if(b < 0x80) cp = b;
      else if((b & 0xE0) == 0xC0){ uint8_t b1 = *s ? (uint8_t)*s++ : 0; cp = ((b & 0x1F) << 6) | (b1 & 0x3F); }
      else if((b & 0xF0) == 0xE0){ if(*s) s++; if(*s) s++; cp = 0x3F; }
      else cp = 0x3F;
      uint8_t base, acc; mapCP(cp, base, acc);
      drawGlyphSmooth(x, y, base, 1, col, alpha);     // 1:1 = nitido
      if(acc) drawAccent(x, y, 1, acc, col);
      x += 6;
    }
    return x;
  }
  float sc = fontSc(size), penx = x;
  while(*s){
    uint32_t cp = nextCP(&s);
    const FGlyph* g = &FG[fontIdx(cp)];
    drawGlyphScaled((int)(penx + g->bx * sc + 0.5f), y + (int)((g->topoff - FONT_CAPOFF) * sc + 0.5f), g, sc, col, alpha);
    penx += g->adv * sc;
  }
  return (int)(penx + 0.5f);
}
static int  drawText(int x, int y, const char* s, int size, uint16_t col){ return drawTextA(x, y, s, size, col, 255); }
static void drawTextC(int cx, int y, const char* s, int size, uint16_t col){ drawTextA(cx - textW(s, size) / 2, y, s, size, col, 255); }
static void drawTextCA(int cx, int y, const char* s, int size, uint16_t col, uint8_t a){ drawTextA(cx - textW(s, size) / 2, y, s, size, col, a); }
static void drawTextR(int rx, int y, const char* s, int size, uint16_t col){ drawTextA(rx - textW(s, size), y, s, size, col, 255); }

// ---------------- Triangulo relleno (baricentrico) ----------------
static void fillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c){
  int minx = min(x0, min(x1, x2)), maxx = max(x0, max(x1, x2));
  int miny = min(y0, min(y1, y2)), maxy = max(y0, max(y1, y2));
  // En modo landscape (gLand) el lienzo LOGICO es 800x480, no 480x800: recortar
  // contra los limites correctos para no perder los triangulos con x logica >=480
  // (picos/naves/wave del juego Geo Dash). En portrait no cambia nada.
  int bcw = gLand ? SCR_H : SCR_W, bch = gLand ? SCR_W : SCR_H;
  if(minx < 0) minx = 0; if(miny < 0) miny = 0;
  if(maxx >= bcw) maxx = bcw - 1; if(maxy >= bch) maxy = bch - 1;
  for(int y = miny; y <= maxy; y++){
    for(int x = minx; x <= maxx; x++){
      long e0 = (long)(x - x0) * (y1 - y0) - (long)(y - y0) * (x1 - x0);
      long e1 = (long)(x - x1) * (y2 - y1) - (long)(y - y1) * (x2 - x1);
      long e2 = (long)(x - x2) * (y0 - y2) - (long)(y - y2) * (x0 - x2);
      bool neg = (e0 < 0) || (e1 < 0) || (e2 < 0);
      bool pos = (e0 > 0) || (e1 > 0) || (e2 > 0);
      if(!(neg && pos)) px(x, y, c);
    }
  }
}
static void fillQuad(int x0,int y0,int x1,int y1,int x2,int y2,int x3,int y3,uint16_t c){
  fillTriangle(x0,y0,x1,y1,x2,y2,c);
  fillTriangle(x0,y0,x2,y2,x3,y3,c);
}

// ---------------- Reloj vectorial (trazo grueso redondeado) -------
static float bigCharAdvance(char ch, int capH){
  if(ch == ':') return capH * 0.34f;
  return capH * 0.60f + capH * 0.12f;   // ancho + hueco
}
static void drawBigChar(char ch, int ox, int oy, int capH, int thick, uint16_t col){
  float DW = capH * 0.60f, DH = capH;
  float m = thick * 0.5f;
  float L = ox + m, R = ox + DW - m, T = oy + m, B = oy + DH - m;
  float MX = ox + DW * 0.5f, MY = oy + DH * 0.5f;
  int rad = thick / 2; if(rad < 1) rad = 1;
  float pr = 0.0174532925f;
  auto arc = [&](float cxx, float cyy, float rx, float ry, float a0, float a1){
    int steps = (int)(fabsf(a1 - a0) / 7.0f) + 2;
    float pxp = 0, pyp = 0; bool first = true;
    for(int i = 0; i <= steps; i++){
      float a = (a0 + (a1 - a0) * i / steps) * pr;
      float xx = cxx + rx * cosf(a), yy = cyy + ry * sinf(a);
      if(!first) strokeSegAA(pxp, pyp, xx, yy, (float)rad, col);
      pxp = xx; pyp = yy; first = false;
    }
  };
  auto seg = [&](float x0, float y0, float x1, float y1){ strokeSegAA(x0, y0, x1, y1, (float)rad, col); };
  float sx;
  switch(ch){
    case '0': arc(MX, MY, DW * 0.5f - m, DH * 0.5f - m, 0, 360); break;
    case '1':
      sx = MX + DW * 0.06f;
      seg(sx, T, sx, B);
      seg(MX - DW * 0.26f, T + DH * 0.16f, sx, T);
      seg(MX - DW * 0.30f, B, MX + DW * 0.34f, B);
      break;
    case '2': {
      arc(MX, T + DH * 0.24f, DW * 0.5f - m, DH * 0.24f, 180, 380);
      float ex = MX + (DW * 0.5f - m) * cosf(20 * pr);
      float ey = (T + DH * 0.24f) + (DH * 0.24f) * sinf(20 * pr);
      seg(ex, ey, L, B);
      seg(L, B, R, B);
    } break;
    case '3':
      // FIX: los angulos originales (200-430 / 290-520) hacian que ambos arcos
      // se pasaran ~50-70 grados mas alla del centro vertical del glifo, asi
      // que se superponian en una franja ancha del lado derecho -- eso era la
      // "raya horizontal que sobresale". Los angulos de abajo se resolvieron
      // para que el arco superior TERMINE exactamente donde el arco inferior
      // EMPIEZA (mismo punto, sin solape), verificado por simulacion en Python
      // (distancia entre los dos extremos = 0px). El resto del barrido (el
      // rizo de la izquierda en cada extremo) se dejo igual que el original.
      arc(MX - DW * 0.05f, T + DH * 0.27f, DW * 0.40f, DH * 0.27f, 200, 398);
      arc(MX - DW * 0.05f, B - DH * 0.27f, DW * 0.40f, DH * 0.27f, 322, 520);
      break;
    case '4':
      sx = MX + DW * 0.16f;
      seg(sx, T, L, T + DH * 0.64f);
      seg(L, T + DH * 0.64f, R, T + DH * 0.64f);
      seg(sx, T, sx, B);
      break;
    case '5':
      seg(R, T, L + DW * 0.04f, T);
      seg(L + DW * 0.04f, T, L + DW * 0.04f, T + DH * 0.40f);
      arc(MX - DW * 0.02f, B - DH * 0.28f, DW * 0.44f, DH * 0.28f, 190, 470);
      break;
    case '6':
      arc(MX, MY + DH * 0.02f, DW * 0.42f, DH * 0.44f, 300, 120);
      arc(MX, B - DH * 0.26f, DW * 0.42f, DH * 0.26f, 0, 360);
      break;
    case '7':
      seg(L, T, R, T);
      seg(R, T, MX - DW * 0.06f, B);
      break;
    case '8':
      arc(MX, T + DH * 0.25f, DW * 0.42f, DH * 0.25f, 0, 360);
      arc(MX, B - DH * 0.27f, DW * 0.46f, DH * 0.27f, 0, 360);
      break;
    case '9':
      arc(MX, T + DH * 0.26f, DW * 0.42f, DH * 0.26f, 0, 360);
      arc(MX, MY, DW * 0.42f, DH * 0.44f, 40, 200);
      break;
    case ':': {
      int dr = thick; if(dr < 2) dr = 2;
      fillCircleAA(ox + DW * 0.16f, T + DH * 0.32f, (float)dr, col);
      fillCircleAA(ox + DW * 0.16f, B - DH * 0.28f, (float)dr, col);
    } break;
  }
}
// Dibuja "H:MM" centrado horizontalmente en cx
static void drawBigClock(const char* s, int cx, int y, int capH, int thick, uint16_t col){
  float total = 0;
  for(const char* p = s; *p; p++) total += bigCharAdvance(*p, capH);
  float x = cx - total / 2;
  for(const char* p = s; *p; p++){
    drawBigChar(*p, (int)x, y, capH, thick, col);
    x += bigCharAdvance(*p, capH);
  }
}

// #############################################################
// ##  ICONOS VECTORIALES  (originales, basados en tus imagenes)
// #############################################################

// arco con trazo grueso (para wifi y detalles)
static void arcStroke(float cx, float cy, float r, float a0, float a1, int thick, uint16_t col){
  float pr = 0.0174532925f;
  int steps = (int)(fabsf(a1 - a0) / 8.0f) + 2;
  float pxp = 0, pyp = 0; bool first = true;
  int rad = thick / 2; if(rad < 1) rad = 1;
  for(int i = 0; i <= steps; i++){
    float a = (a0 + (a1 - a0) * i / steps) * pr;
    float xx = cx + r * cosf(a), yy = cy + r * sinf(a);
    if(!first) strokeSeg(pxp, pyp, xx, yy, rad, col);
    pxp = xx; pyp = yy; first = false;
  }
}

enum { IC_RELOJ, IC_GALERIA, IC_MULTIMEDIA, IC_ALMACEN, IC_MODOPC, IC_NOTAS,
       IC_EDU, IC_NAV, IC_CODE, IC_BIEN, IC_PAINT, IC_JUEGOS,
       IC_AJUSTES, IC_CALC, IC_CALEND, IC_CAMARA };

static void iconBase(int x, int y, int S, uint16_t bg, int rf100){
  int r = S * rf100 / 100;
  if(gIconStyle == 1){                     // estilo "Vidrio": fondo Liquid Glass (ver drawAppIcon)
    drawLiquidGlassPanel(x, y, S, S, r, bg);
  } else {                                 // estilo "Plano" (original)
    fillRoundRect(x, y, S, S, r, bg);
    // sutil brillo superior
    fillRoundRectA(x, y, S, S / 2, r, rgb565(255,255,255), 22);
  }
}

static void drawAppIcon(int id, int x, int y, int S){
  int cx = x + S / 2, cy = y + S / 2;
  int tk = S / 12; if(tk < 2) tk = 2;               // grosor de trazo generico
  uint16_t WHITE = rgb565(255,255,255);
  switch(id){
    case IC_RELOJ: {
      iconBase(x, y, S, rgb565(245,245,247), 22);
      fillRing(cx, cy, (int)(S * 0.36f), 2, rgb565(70,70,74));
      // ticks 12/3/6/9
      fillRect(cx - 1, y + (int)(S * 0.16f), 2, S / 12, rgb565(70,70,74));
      fillRect(cx - 1, y + S - (int)(S * 0.16f) - S / 12, 2, S / 12, rgb565(70,70,74));
      fillRect(x + (int)(S * 0.16f), cy - 1, S / 12, 2, rgb565(70,70,74));
      fillRect(x + S - (int)(S * 0.16f) - S / 12, cy - 1, S / 12, 2, rgb565(70,70,74));
      strokeSeg(cx, cy, cx - S * 0.14f, cy - S * 0.10f, tk / 2 + 1, rgb565(30,30,30)); // hora
      strokeSeg(cx, cy, cx + S * 0.12f, cy - S * 0.20f, tk / 2, rgb565(245,140,30));   // min
      fillCircle(cx, cy, tk / 2 + 1, rgb565(30,30,30));
    } break;
    case IC_GALERIA: {
      iconBase(x, y, S, WHITE, 22);
      uint16_t cols[8] = { rgb565(233,64,64), rgb565(240,150,40), rgb565(240,210,50),
                           rgb565(90,200,90), rgb565(50,190,190), rgb565(60,120,235),
                           rgb565(120,80,220), rgb565(220,80,200) };
      float d = S * 0.17f, pr = S * 0.135f;
      for(int k = 0; k < 8; k++){
        float a = k * 45 * 0.0174532925f;
        fillCircle((int)(cx + d * cosf(a)), (int)(cy + d * sinf(a)), (int)pr, cols[k]);
      }
      fillCircle(cx, cy, (int)(S * 0.11f), WHITE);
    } break;
    case IC_MULTIMEDIA: {
      iconBase(x, y, S, rgb565(27,95,217), 22);
      fillTriangle(cx - (int)(S * 0.12f), cy - (int)(S * 0.18f),
                   cx - (int)(S * 0.12f), cy + (int)(S * 0.18f),
                   cx + (int)(S * 0.22f), cy, WHITE);
    } break;
    case IC_ALMACEN: {
      iconBase(x, y, S, rgb565(59,123,217), 22);
      uint16_t fol = rgb565(225,236,250);
      fillRoundRect(x + (int)(S * 0.20f), y + (int)(S * 0.28f), (int)(S * 0.30f), (int)(S * 0.12f), 3, fol);
      fillRoundRect(x + (int)(S * 0.18f), y + (int)(S * 0.36f), (int)(S * 0.64f), (int)(S * 0.34f), 5, fol);
      // nubecita
      uint16_t cl = rgb565(205,222,245);
      fillCircle(x + (int)(S * 0.60f), y + (int)(S * 0.34f), (int)(S * 0.10f), cl);
      fillCircle(x + (int)(S * 0.72f), y + (int)(S * 0.36f), (int)(S * 0.08f), cl);
      fillRect(x + (int)(S * 0.58f), y + (int)(S * 0.36f), (int)(S * 0.18f), (int)(S * 0.07f), cl);
    } break;
    case IC_MODOPC: {
      iconBase(x, y, S, rgb565(30,58,110), 22);
      fillRoundRect(x + (int)(S * 0.16f), y + (int)(S * 0.24f), (int)(S * 0.68f), (int)(S * 0.40f), 4, rgb565(235,240,250));
      fillRect(x + (int)(S * 0.22f), y + (int)(S * 0.30f), (int)(S * 0.56f), (int)(S * 0.28f), rgb565(45,95,205));
      fillRect(cx - (int)(S * 0.05f), y + (int)(S * 0.64f), (int)(S * 0.10f), (int)(S * 0.08f), rgb565(200,210,225));
      fillRoundRect(cx - (int)(S * 0.16f), y + (int)(S * 0.72f), (int)(S * 0.32f), (int)(S * 0.06f), 2, rgb565(200,210,225));
    } break;
    case IC_NOTAS: {
      iconBase(x, y, S, rgb565(232,167,90), 22);
      fillRoundRect(x + (int)(S * 0.22f), y + (int)(S * 0.18f), (int)(S * 0.48f), (int)(S * 0.62f), 5, WHITE);
      for(int i = 0; i < 3; i++)
        fillRect(x + (int)(S * 0.30f), y + (int)(S * 0.30f) + i * (int)(S * 0.12f), (int)(S * 0.32f), 2, rgb565(180,180,185));
      strokeSeg(x + S * 0.56f, y + S * 0.66f, x + S * 0.78f, y + S * 0.40f, tk / 2 + 1, rgb565(120,90,40)); // lapiz
      fillCircle((int)(x + S * 0.78f), (int)(y + S * 0.40f), tk / 2 + 1, rgb565(245,210,90));
    } break;
    case IC_EDU: {
      iconBase(x, y, S, rgb565(79,179,196), 22);
      uint16_t cap = rgb565(28,52,96);
      fillQuad(cx, cy - (int)(S * 0.24f), cx + (int)(S * 0.28f), cy - (int)(S * 0.06f),
               cx, cy + (int)(S * 0.12f), cx - (int)(S * 0.28f), cy - (int)(S * 0.06f), cap);
      fillRoundRect(cx - (int)(S * 0.18f), cy + (int)(S * 0.08f), (int)(S * 0.36f), (int)(S * 0.16f), 3, WHITE);
      strokeSeg(cx + S * 0.26f, cy - S * 0.06f, cx + S * 0.26f, cy + S * 0.14f, 1, cap);
    } break;
    case IC_NAV: {
      iconBase(x, y, S, rgb565(46,155,230), 22);
      fillRing(cx, cy, (int)(S * 0.30f), 2, WHITE);
      vLine(cx, cy - (int)(S * 0.30f), (int)(S * 0.60f), WHITE);
      hLine(cx - (int)(S * 0.30f), cy, (int)(S * 0.60f), WHITE);
      arcStroke(cx, cy, S * 0.18f, 90, 270, 2, WHITE);   // meridiano
      arcStroke(cx, cy, S * 0.18f, -90, 90, 2, WHITE);
    } break;
    case IC_CODE: {
      iconBase(x, y, S, rgb565(154,160,166), 22);
      uint16_t dk = rgb565(55,58,66);
      strokeSeg(cx - S * 0.10f, cy - S * 0.15f, cx - S * 0.26f, cy, tk / 2 + 1, dk);
      strokeSeg(cx - S * 0.26f, cy, cx - S * 0.10f, cy + S * 0.15f, tk / 2 + 1, dk);
      strokeSeg(cx + S * 0.10f, cy - S * 0.15f, cx + S * 0.26f, cy, tk / 2 + 1, dk);
      strokeSeg(cx + S * 0.26f, cy, cx + S * 0.10f, cy + S * 0.15f, tk / 2 + 1, dk);
      strokeSeg(cx + S * 0.04f, cy - S * 0.17f, cx - S * 0.04f, cy + S * 0.17f, tk / 2, dk);
    } break;
    case IC_BIEN: {
      iconBase(x, y, S, rgb565(92,193,90), 30);
      strokeSeg(cx - S * 0.16f, cy + S * 0.02f, cx - S * 0.02f, cy + S * 0.16f, tk / 2 + 1, WHITE);
      strokeSeg(cx - S * 0.02f, cy + S * 0.16f, cx + S * 0.20f, cy - S * 0.14f, tk / 2 + 1, WHITE);
    } break;
    case IC_PAINT: {
      iconBase(x, y, S, rgb565(241,231,210), 22);
      fillCircle(cx - (int)(S * 0.04f), cy + (int)(S * 0.02f), (int)(S * 0.27f), rgb565(236,226,205));
      fillCircle(cx + (int)(S * 0.11f), cy + (int)(S * 0.11f), (int)(S * 0.06f), rgb565(241,231,210)); // hueco
      fillCircle(cx - (int)(S * 0.14f), cy - (int)(S * 0.06f), (int)(S * 0.045f), rgb565(230,70,70));
      fillCircle(cx - (int)(S * 0.02f), cy - (int)(S * 0.13f), (int)(S * 0.045f), rgb565(240,200,60));
      fillCircle(cx + (int)(S * 0.10f), cy - (int)(S * 0.07f), (int)(S * 0.045f), rgb565(70,130,235));
      fillCircle(cx - (int)(S * 0.16f), cy + (int)(S * 0.09f), (int)(S * 0.045f), rgb565(80,190,90));
      strokeSeg(cx + S * 0.02f, cy - S * 0.16f, cx + S * 0.26f, cy - S * 0.30f, tk / 2 + 1, rgb565(140,100,60));
    } break;
    case IC_JUEGOS: {
      iconBase(x, y, S, rgb565(142,30,30), 22);
      drawRoundRect(x + (int)(S * 0.14f), y + (int)(S * 0.34f), (int)(S * 0.72f), (int)(S * 0.30f), (int)(S * 0.13f), WHITE);
      drawRoundRect(x + (int)(S * 0.14f) + 1, y + (int)(S * 0.34f) + 1, (int)(S * 0.72f) - 2, (int)(S * 0.30f) - 2, (int)(S * 0.12f), WHITE);
      // dpad
      fillRect(cx - (int)(S * 0.22f) - 1, cy + (int)(S * 0.02f), (int)(S * 0.12f), 3, WHITE);
      fillRect(cx - (int)(S * 0.16f) - 1, cy - (int)(S * 0.04f), 3, (int)(S * 0.12f), WHITE);
      // botones
      fillCircle(cx + (int)(S * 0.14f), cy - (int)(S * 0.01f), 3, WHITE);
      fillCircle(cx + (int)(S * 0.22f), cy + (int)(S * 0.05f), 3, WHITE);
    } break;
    case IC_AJUSTES: {
      iconBase(x, y, S, rgb565(138,143,152), 22);
      uint16_t g = rgb565(70,74,84);
      fillCircle(cx, cy, (int)(S * 0.26f), g);
      for(int k = 0; k < 8; k++){
        float a = k * 45 * 0.0174532925f;
        fillCircle((int)(cx + S * 0.30f * cosf(a)), (int)(cy + S * 0.30f * sinf(a)), (int)(S * 0.075f), g);
      }
      fillCircle(cx, cy, (int)(S * 0.10f), rgb565(138,143,152));
    } break;
    case IC_CALC: {
      iconBase(x, y, S, rgb565(58,58,60), 22);
      fillRoundRect(x + (int)(S * 0.18f), y + (int)(S * 0.16f), (int)(S * 0.64f), (int)(S * 0.16f), 3, rgb565(210,210,215));
      for(int rr = 0; rr < 3; rr++) for(int cc = 0; cc < 4; cc++){
        uint16_t bc = (cc == 3) ? rgb565(245,150,30) : rgb565(150,150,155);
        fillRoundRect(x + (int)(S * 0.18f) + cc * (int)(S * 0.17f), y + (int)(S * 0.40f) + rr * (int)(S * 0.15f),
                      (int)(S * 0.12f), (int)(S * 0.10f), 2, bc);
      }
    } break;
    case IC_CALEND: {
      iconBase(x, y, S, WHITE, 22);
      fillRect(x + (int)(S * 0.16f), y + (int)(S * 0.18f), (int)(S * 0.68f), (int)(S * 0.16f), rgb565(232,70,70));
      drawBigChar('1', cx - (int)(S * 0.60f * 0.30f), y + (int)(S * 0.36f), (int)(S * 0.44f), 3, rgb565(60,60,64));
    } break;
    case IC_CAMARA: {
      iconBase(x, y, S, rgb565(74,74,78), 22);
      fillCircle(cx, cy, (int)(S * 0.27f), rgb565(30,30,32));
      fillRing(cx, cy, (int)(S * 0.27f), 3, rgb565(120,120,128));
      fillCircle(cx, cy, (int)(S * 0.16f), rgb565(60,72,95));
      fillCircle(cx - (int)(S * 0.06f), cy - (int)(S * 0.06f), (int)(S * 0.05f), rgb565(150,175,205));
      fillRoundRect(x + (int)(S * 0.66f), y + (int)(S * 0.18f), (int)(S * 0.10f), (int)(S * 0.06f), 2, rgb565(190,190,195));
    } break;
  }
}

// ---------------- Iconos de barra de estado ----------------
static void drawWifi(int cx, int by, int R, uint16_t col){
  arcStroke(cx, by, R,        225, 315, 2, col);
  arcStroke(cx, by, R * 0.66f, 225, 315, 2, col);
  arcStroke(cx, by, R * 0.33f, 225, 315, 2, col);
  fillCircle(cx, by, 2, col);
}
static void drawBattery(int x, int y, int w, int h, int level, uint16_t col){
  drawRoundRect(x, y, w, h, 2, col);
  drawRoundRect(x + 1, y + 1, w - 2, h - 2, 2, col);
  fillRect(x + w, y + h / 3, 2, h / 3, col);       // pin +
  int fw = (w - 6) * level / 100;
  if(fw > 0) fillRect(x + 3, y + 3, fw, h - 6, col);
}

// #############################################################
// ##  TACTIL DE ALTO NIVEL (gestos)  ·  original
// #############################################################
static Touch T;

// ---- FASE 4: Modo Kiosco (prestamo seguro) -------------------------------
// El estado vive AQUI ARRIBA, antes de flexPollTouch(), porque el filtro del
// area excluida tiene que actuar en el punto MAS ALTO del pipeline tactil: si
// se filtrara mas abajo, una app hospedada podria llegar a ver el toque.
#define KIOSK_ON 1                     // interruptor de toda la Fase 4
static bool kioskOn   = false;         // true = modo kiosco activo (NVS "kioskon")
static int  kioskApp  = -1;            // app "clavada" (indice de APP_REG, NVS "kioskapp")
static int  kioskExX  = 0, kioskExY = 0, kioskExW = 0, kioskExH = 0;  // area excluida (W=0 -> ninguna)
// Candado discreto + zona del gesto de salida. Coordenadas FISICAS del panel,
// no logicas: asi el gesto de salida funciona igual aunque la app corra en
// landscape (gLand).
//
// Va en la esquina SUPERIOR DERECHA, justo DEBAJO de la barra de estado. Antes
// estaba en (8,8) y se comia la hora, que renderHome() dibuja en (20,16). El
// hueco de aqui esta libre en todas las pantallas: el wifi ocupa x 403..425 y
// llega hasta y=28, la bateria x 434..466 e y 20..35, y los widgets del Home no
// empiezan hasta y=72. Con el candado en (448,44) quedan 9 px de margen bajo la
// bateria y 7 px hasta el borde derecho.
#define KIOSK_BADGE_X 448
#define KIOSK_BADGE_Y 44
#define KIOSK_BADGE_S 24
#define KIOSK_EXIT_PAD 14              // el hitbox del gesto es mas generoso que el dibujo
static bool kioskInExcluded(int px, int py){
  if(kioskExW <= 0 || kioskExH <= 0) return false;
  return px >= kioskExX && px < kioskExX + kioskExW && py >= kioskExY && py < kioskExY + kioskExH;
}
// Decide si un toque se descarta. Se define abajo del todo porque necesita
// gState, que se declara mas adelante; flexPollTouch la llama por prototipo.
static bool kioskTouchBlocked(int px, int py);
// El hitbox de salida SIEMPRE gana sobre el area excluida: si el usuario dibuja
// un rectangulo encima del candado, el dueno del telefono seguiria pudiendo
// salir. Sin esto, el propio modo kiosco se podria convertir en un ladrillo.
static bool kioskInExit(int px, int py){
  return px >= KIOSK_BADGE_X - KIOSK_EXIT_PAD && px <= KIOSK_BADGE_X + KIOSK_BADGE_S + KIOSK_EXIT_PAD &&
         py >= KIOSK_BADGE_Y - KIOSK_EXIT_PAD && py <= KIOSK_BADGE_Y + KIOSK_BADGE_S + KIOSK_EXIT_PAD;
}

// #############################################################
// ##  SUSPENSION (apagado NORMAL de pantalla, sin deep sleep)
// ##  ------------------------------------------------------
// ##  Entrar: doble-tap con DOS dedos en cualquier parte.
// ##  Salir : doble-tap con UN dedo en cualquier parte.
// ##
// ##  Que es y que NO es:
// ##   · El ESP32 NO se duerme. loop() sigue corriendo igual (WiFi,
// ##     reloj, animaciones internas). Lo unico que se apaga es la
// ##     SALIDA VISUAL: backlight a 0 por PWM + DISPOFF del panel.
// ##   · NO se toca gState. El sistema sigue "siendo" lo que era
// ##     (Home, App, Geo Dash, Modo PC...). La suspension es una capa
// ##     de driver superpuesta, no una pantalla de la aplicacion.
// ##   · NO se toca fb ni bbuf. La UI queda intacta en memoria, asi
// ##     que al despertar reaparece sola: el propio fundido del
// ##     backlight YA produce visualmente un fade-in desde negro
// ##     sobre lo que sigue estando en el framebuffer. Por eso NO se
// ##     compone ningun overlay negro de aparicion -- seria pintar
// ##     encima de la UI para conseguir un efecto que el backlight
// ##     regala gratis, y ademas obligaria a redibujar para limpiarlo.
// ##
// ##  El detector NO usa struct Touch: trabaja directamente sobre
// ##  gtFingers (cuenta de contactos del GT911). Asi es inmune a que
// ##  el filtro de abajo anule los flags de T, y el contrato de T
// ##  para el resto del sistema no cambia en absoluto.
// #############################################################
static bool gSuspOn      = false;   // suspension activa (desde el gesto hasta que termina el fundido de vuelta)
static bool gSuspDark    = false;   // backlight ya en 0 y DISPOFF enviado
static int  gSuspBright  = 80;      // brillo del usuario al suspender (para restaurarlo exacto)
static int  gSuspFade    = -1;      // -1 = sin fundido en curso; si no, brillo actual del fundido (0..100)
static int  gSuspFadeTo  = 0;       // destino del fundido
static uint32_t gSuspFadeMs = 0;    // millis() del ultimo paso del fundido
static bool gSuspSwallow = false;   // este poll pertenece al gesto -> no lo ve nadie mas

// ---- Estado del detector de doble-tap ----------------------------------
// Un "toque" (episodio) va desde que baja el primer dedo hasta que se levantan
// todos. Durante el episodio se anota cuantos dedos llego a haber.
static bool     gEpAct   = false;   // hay un episodio de toque en curso
static uint32_t gEpT0    = 0;       // millis() del inicio del episodio
static uint8_t  gEpRun2  = 0;       // polls CONSECUTIVOS con n>=2 dentro del episodio
static bool     gEpHad2  = false;   // el episodio quedo confirmado como "de 2 dedos"
static bool     gEpHad3  = false;   // llego a 3+ dedos -> no cuenta ni como 1 ni como 2
static uint32_t gTap2Ms  = 0;       // fin del ultimo toque valido de 2 dedos (0 = cadena vacia)
static uint32_t gTap1Ms  = 0;       // fin del ultimo toque valido de 1 dedo

// ---- Vuelta a donde estabas tras desbloquear ---------------------------
// Al despertar con clave configurada se cae en la pantalla de Bloqueo, pero el
// sitio donde estaba el usuario NO se pierde: se anota aqui y, al acertar el
// PIN, se restaura. -1 = nada pendiente.
static int  gSuspRetState = -1;     // gState que habia al suspender
static int  gSuspRetApp   = -1;     // gAppId que habia al suspender (si era ST_APP)
// true mientras corre una verificacion que SALIO de la pantalla de Bloqueo. Sin
// esto, cancelar el teclado de PIN caeria por la rama de lsuExit que lleva a
// ST_HOME, y el escritorio quedaria a la vista SIN haber desbloqueado -- justo
// el agujero que este cambio viene a cerrar.
static bool gLockVerifyLocked = false;
// Se define mucho mas abajo (necesita gState, renderLock y showLock, que aun no
// existen aqui). Mismo patron que kioskTouchBlocked: prototipo arriba, cuerpo
// abajo. Solo primitivos en la firma, como exige el auto-prototipado de Arduino.
static void suspWakeLockScreen();
// true mientras hay dedos sobre la rejilla del teclado. Se define abajo, con el
// teclado; aqui solo el prototipo (primitivos en la firma). Lo necesita el
// gesto de suspension: ver el VETO AL TECLEAR en suspGestureUpdate.
static bool kbTypingNow();

// Arranca un fundido de backlight NO bloqueante hacia 'to' (0..100).
static void suspFadeTo(int to){
  // Si ya habia un fundido en curso, sigue desde donde iba; si no, arranca desde
  // el brillo REAL del PWM (gBlPct), NO desde gBright. Al despertar gBright
  // sigue valiendo lo de siempre (p.ej. 80) mientras el PWM esta a 0: arrancar
  // desde gBright dejaria el fundido ya en su destino y la pantalla se
  // encenderia de golpe -- justo el fogonazo que hay que evitar.
  if(gSuspFade < 0) gSuspFade = gBlPct;
  gSuspFadeTo = to;
  gSuspFadeMs = millis();
}
static void suspEnter(){
  if(gSuspOn) return;
  gSuspBright = gBright;             // brillo del usuario, intacto (blWritePct no lo toca)
  gSuspOn = true; gSuspDark = false;
  suspFadeTo(0);
}
static void suspWake(){
  if(!gSuspOn) return;
  // ORDEN IMPORTANTE (privacidad): la pantalla de Bloqueo se compone MIENTRAS
  // sigue todo a oscuras -- backlight a 0 y el panel aun en DISPOFF. Asi lo que
  // habia antes de suspender no llega a verse ni un frame: cuando el backlight
  // empieza a subir, lo que hay en el framebuffer YA es el bloqueo.
  suspWakeLockScreen();
  if(gSuspDark){ panelDisplayOn(); gSuspDark = false; }   // revertir el DCS ANTES de subir el backlight
  suspFadeTo(gSuspBright);
}
// Un paso del fundido por vuelta de loop(). Sin delay(), igual de suave que el
// resto de animaciones del sistema (interpolado en varios ticks).
static void suspFadeTick(){
  if(gSuspFade < 0) return;
  uint32_t now = millis();
  if(now - gSuspFadeMs < SUSP_FADE_STEP_MS) return;
  gSuspFadeMs = now;
  if(gSuspFade > gSuspFadeTo){ gSuspFade -= SUSP_FADE_STEP; if(gSuspFade < gSuspFadeTo) gSuspFade = gSuspFadeTo; }
  else if(gSuspFade < gSuspFadeTo){ gSuspFade += SUSP_FADE_STEP; if(gSuspFade > gSuspFadeTo) gSuspFade = gSuspFadeTo; }
  blWritePct(gSuspFade);
  if(gSuspFade != gSuspFadeTo) return;
  gSuspFade = -1;                                   // fundido terminado
  if(gSuspFadeTo == 0){
    if(gSuspOn && !gSuspDark){ gSuspDark = true; panelDisplayOff(); }   // DCS DESPUES del negro
  } else {
    setBacklight(gSuspBright);      // deja gBright coherente y el PWM en su valor exacto
    gSuspOn = false;                // a partir de aqui el tactil vuelve a fluir normal
  }
}
// Detector de doble-tap. Se llama DESDE flexPollTouch(), al final, con gtFingers
// ya actualizado por gtPoll(). Decide ademas si este poll se lo traga el gesto.
static void suspGestureUpdate(){
#if SUSPEND_ON
  uint32_t now = millis();
  // Cuenta de dedos con la MISMA red de seguridad de 90 ms que usa flexPollTouch:
  // si el GT911 deja de reportar sin mandar el frame de "0 dedos", el episodio
  // no se queda colgado para siempre.
  int n = (now - gtFingersMs > 90) ? 0 : (int)gtFingers;

  // Caducidad de las cadenas de doble-tap (el 2o toque llego tarde -> se olvida).
  if(gTap2Ms && now - gTap2Ms > SUSP_TAP_WINDOW_MS) gTap2Ms = 0;
  if(gTap1Ms && now - gTap1Ms > SUSP_TAP_WINDOW_MS) gTap1Ms = 0;

  if(n > 0){
    if(!gEpAct){ gEpAct = true; gEpT0 = now; gEpRun2 = 0; gEpHad2 = false; gEpHad3 = false; }
    if(n >= 3) gEpHad3 = true;
    // ANTI FALSO-POSITIVO: n>=2 tiene que sostenerse SUSP_TAP_FRAMES polls
    // consecutivos. El instante en que el segundo dedo esta aterrizando es
    // ruidoso y puede dar un unico frame con n=2 espurio.
    // TOLERANCIA A DESINCRONIZACION: no se exige que los dos dedos bajen en el
    // mismo poll; basta con que en ALGUN momento del episodio n llegue a 2 el
    // numero de frames pedido, y la marca gEpHad2 ya no se pierde aunque luego
    // uno de los dedos se levante antes que el otro.
    if(n >= 2){ if(gEpRun2 < 255) gEpRun2++; if(gEpRun2 >= SUSP_TAP_FRAMES) gEpHad2 = true; }
    else gEpRun2 = 0;
  } else if(gEpAct){
    gEpAct = false;
    uint32_t dur = now - gEpT0;
    bool tooLong = (dur > SUSP_TAP_MAX_MS);          // long-press: no es un tap
    if(tooLong || gEpHad3){ gTap1Ms = 0; gTap2Ms = 0; }
    else if(gEpHad2){                                 // ---- toque valido de 2 dedos ----
      // VETO EN KIOSCO: mismo criterio que autoLockTick. En modo kiosco el
      // telefono esta prestado y quien lo tiene en la mano no sabe que existe el
      // gesto de despertar; una pantalla que se queda negra de golpe se lee como
      // "se ha roto" y el dueno acaba teniendo que rescatarlo. Ademas la
      // suspension no aporta nada ahi: el kiosco es para uso activo, no para
      // guardar el aparato.
      //
      // VETO AL TECLEAR (Fase B): escribir con los dos pulgares produce
      // continuamente episodios de 2 dedos. Sin esto pasaban dos cosas, las dos
      // malas: teclear a dos manos apagaba la pantalla sola, y -- peor -- el
      // gSuspSwallow de mas abajo anulaba TODOS los eventos de T mientras habia
      // dos dedos, asi que el teclado se quedaba mudo justo cuando se escribia
      // rapido. El gesto de suspender sigue existiendo igual en todas partes;
      // solo se calla mientras hay dedos sobre las teclas.
      bool veto = (KIOSK_ON && kioskOn) || kbTypingNow();
      if(gTap2Ms && (now - gTap2Ms) >= SUSP_TAP_GAP_MS){ gTap2Ms = 0; if(!gSuspOn && !veto) suspEnter(); }
      else gTap2Ms = now;
      gTap1Ms = 0;                                    // las dos cadenas son excluyentes
    } else {                                          // ---- toque valido de 1 dedo ----
      // El doble-tap de 1 dedo SOLO se escucha con la pantalla suspendida. Con la
      // pantalla encendida un doble-tap normal es un gesto legitimo de la UI y no
      // debe significar nada aqui.
      if(gSuspOn){
        if(gTap1Ms && (now - gTap1Ms) >= SUSP_TAP_GAP_MS){ gTap1Ms = 0; suspWake(); }
        else gTap1Ms = now;
      } else gTap1Ms = 0;
      gTap2Ms = 0;
    }
  }
  // Que toques se traga el gesto:
  //  · Suspendido: TODOS. Mientras la pantalla esta apagada, el doble-tap de 1
  //    dedo es lo unico que se lee; nadie mas debe ver el tactil, para que el
  //    toque de despertar no dispare de rebote algo de la UI que hay debajo.
  //  · Despierto: solo los episodios ya confirmados como de 2 dedos, para que el
  //    gesto de suspension no abra una app ni dispare el long-press de ST_CTX.
  //  · Tecleando: NUNCA. Ver el veto de arriba -- si el episodio de 2 dedos es
  //    de alguien escribiendo, T tiene que seguir llegando al teclado entero.
  gSuspSwallow = gSuspOn || (gEpHad2 && !kbTypingNow());
#else
  gSuspSwallow = false;
#endif
}

static void tDoRelease(unsigned long now){
  T.down = false; T.released = true;
  T.dx = T.x - T.startX; T.dy = T.y - T.startY;
  unsigned long dur = now - T.downMs;
  int adx = abs(T.dx), ady = abs(T.dy);
  if(adx < 16 && ady < 16 && dur < 550) T.tap = true;
  else if(ady > 55 && ady >= adx){ if(T.dy < 0) T.swipeUp = true; else T.swipeDown = true; }
  else if(adx > 55 && adx > ady){ if(T.dx < 0) T.swipeLeft = true; else T.swipeRight = true; }
}
static void flexPollTouch(){
  T.pressed = T.released = T.tap = false;
  T.swipeUp = T.swipeDown = T.swipeLeft = T.swipeRight = false;
  uint16_t gx = 0, gy = 0;
  int8_t ev = gtPoll(gx, gy);
  // FASE 4: descarte SILENCIOSO del area excluida, en el punto mas alto del
  // pipeline. Se convierte en "sin dato" (-1), no en "soltado" (0): asi, si el
  // dedo entro arrastrando desde fuera, el gesto muere por el timeout normal de
  // 90 ms que ya existe abajo, y ninguna capa de arriba -ni el framework, ni una
  // app hospedada- llega a ver nunca esas coordenadas.
  if(ev == 1 && kioskTouchBlocked((int)gx, (int)gy)) ev = -1;
  unsigned long now = millis();
  bool wasDown = T.down;
  if(ev == 1){
    T.x = gx; T.y = gy; T.lastMs = now;
    if(!wasDown){ T.down = true; T.pressed = true; T.startX = gx; T.startY = gy; T.downMs = now; T.moved = false; }
    else if(abs((int)gx - T.startX) > 12 || abs((int)gy - T.startY) > 12) T.moved = true;
  } else if(ev == 0){
    if(wasDown) tDoRelease(now);
  } else {
    if(wasDown && now - T.lastMs > 90) tDoRelease(now);
  }
  // SUSPENSION: el detector de doble-tap va AQUI, en el mismo punto alto del
  // pipeline que el filtro del kiosco y por el mismo motivo -- si filtrara mas
  // abajo, alguna capa ya habria visto el toque. Trabaja sobre gtFingers, no
  // sobre T, asi que anular los flags de T de abajo no le afecta.
  suspGestureUpdate();
  if(gSuspSwallow){
    // Se anulan TODOS los eventos, incluido T.down. Anular T.down tambien mata
    // el long-press del menu contextual (ST_CTX), que es justo lo que hace que
    // el gesto de 2 dedos y el long-press no puedan dispararse mutuamente: en
    // cuanto el episodio queda confirmado como de 2 dedos, para el resto del
    // sistema el dedo ya no esta abajo y el temporizador del long-press se
    // reinicia solo. Vale igual para el drag de la cortina de Ajustes rapidos
    // (necesita T.pressed) y para Geo Dash (lee T.down por flanco).
    T.pressed = T.released = T.tap = false;
    T.swipeUp = T.swipeDown = T.swipeLeft = T.swipeRight = false;
    T.down = false; T.moved = false;
  }
}

// #############################################################
// ##  ALMACENAMIENTO (NVS)  ·  reloj interno  ·  idiomas
// #############################################################
static Preferences prefs;
static bool  cfgOobeDone = false;
static int   cfgLang     = 0;                 // 0=ES 1=EN 2=FR 3=PT 4=IT 5=ZH
static bool  g24h        = false;             // formato de hora 24h
static int   gLockType   = 0;                 // 0 ninguno, 1 PIN, 2 contraseña
// ---- FASE 1: bloqueo global reforzado ------------------------------------
// Tres funciones INDEPENDIENTES, cada una con su interruptor. Poner un
// interruptor a 0 desactiva SOLO esa funcion y deja el resto del sistema de
// PIN/contrasena exactamente como estaba (mismo patron que GLASS_*_ON).
#define LOCK_FAILS_ON 1               // contador persistente + espera progresiva
#define LOCK_SHAKE_ON 1               // sacudida horizontal amortiguada al fallar
#define AUTOLOCK_ON   1               // bloqueo automatico por inactividad
// Umbrales de la espera progresiva. 1-3 fallos no cuestan nada; del 4 al 5 se
// cobran 30 s antes de poder reintentar; a partir del 6 son 5 min con mensaje
// explicito en pantalla.
#define LOCK_FAILS_SOFT     4
#define LOCK_FAILS_HARD     6
#define LOCK_WAIT_SOFT_MS   30000UL
#define LOCK_WAIT_HARD_MS   300000UL
// Opciones que ofrece Ajustes -> Seguridad -> Bloqueo de inactividad. El valor
// vivo es gAutoLockMs (NVS "autolockms"); 0 = nunca se bloquea solo.
#define AUTOLOCK_NOPT 6
static const uint32_t AUTOLOCK_OPTS[AUTOLOCK_NOPT]  = { 30000UL, 60000UL, 300000UL, 600000UL, 1800000UL, 0UL };
static const char*    AUTOLOCK_NAMES[AUTOLOCK_NOPT] = { "30 segundos", "1 minuto", "5 minutos", "10 minutos", "30 minutos", "Nunca" };
#define AUTOLOCK_DEFAULT_IDX 1                        // 1 minuto
#define AUTOLOCK_DEFAULT_MS  60000UL
static int      lockFails    = 0;                     // fallos acumulados (NVS "lockfails")
static uint32_t gAutoLockMs  = AUTOLOCK_DEFAULT_MS;   // ventana de inactividad
static int  autoLockIdx(){
  for(int i = 0; i < AUTOLOCK_NOPT; i++) if(AUTOLOCK_OPTS[i] == gAutoLockMs) return i;
  return AUTOLOCK_DEFAULT_IDX;
}
static const char* autoLockName(){ return AUTOLOCK_NAMES[autoLockIdx()]; }
// Ajusta a una de las opciones ofrecidas. Hace falta porque el valor anterior
// por defecto eran 2 minutos, que ya no esta en la lista: sin esto, una placa
// que ya tenga ese valor guardado mostraria "1 minuto" en Ajustes mientras se
// sigue bloqueando a los 2.
static void autoLockNormalize(){
  for(int i = 0; i < AUTOLOCK_NOPT; i++) if(AUTOLOCK_OPTS[i] == gAutoLockMs) return;
  gAutoLockMs = AUTOLOCK_OPTS[AUTOLOCK_DEFAULT_IDX];
}
static uint32_t gLastTouchMs = 0;                     // millis del ultimo contacto real
// ---- APAGADO SEGURO: preferencia de usuario (Ajustes -> Seguridad) -------
// IMPORTANTE -- no confundir dos cosas distintas:
//
//  · gPoffPin (esto) es la confirmacion por clave del APAGADO COMPLETO: evita
//    que alguien apague el aparato de un deslizamiento. Se aplica UNICA Y
//    EXCLUSIVAMENTE ahi. Ni suspEnter() ni suspWake() lo consultan jamas:
//    SUSPENDER no pide nada, es un gesto de un segundo.
//
//  · El BLOQUEO DE PANTALLA al despertar de una suspension (SUSPEND_LOCK_ON) es
//    otra cosa y depende de gLockType, la clave del dispositivo de toda la
//    vida. Ese si protege la privacidad: si alguien coge el aparato suspendido
//    y lo enciende, se encuentra el bloqueo, no lo que estabas haciendo.
static bool gPoffPin = false;                         // NVS "poffpin"
static bool gBootCleanOff = false;                    // este arranque viene de un apagado limpio (NVS "cleanoff")
// ---- FASES 2 y 3: menu contextual + bloqueo por app ----------------------
#define CTXMENU_ON 1                  // menu de long-press (0 = long-press va directo a Modo Edicion)
#define APPLOCK_ON 1                  // candado por app + verificacion al abrirla
// Una sola clave NVS con un bitmask en vez de 16 claves "applock_<i>": el
// IDENTIFICADOR UNICO de cada FlexApp ya es su indice en APP_REG (0..15, el
// mismo que usan homeOrder[], drawAppIcon() y enterApp()), y 16 apps caben
// exactas en un uint16_t. Un solo getInt/putInt, cero snprintf de claves.
static uint16_t gAppLock = 0;                         // bit i = app i bloqueada (NVS "applockm")
// Que hacer cuando la verificacion de PIN/contrasena ACIERTA. Es lo que permite
// reutilizar lsuStartVerify() para todo (pantalla, apps, kiosco) sin crear una
// segunda ruta de verificacion.
#define LSU_AFTER_UNLOCK    0         // desbloquear la pantalla (comportamiento de siempre)
#define LSU_AFTER_OPENAPP   1         // abrir la app bloqueada que se toco
#define LSU_AFTER_LOCKAPP   2         // confirmar que se pone el candado a una app
#define LSU_AFTER_UNLOCKAPP 3         // confirmar que se quita el candado a una app
#define LSU_AFTER_KIOSKOUT  4         // salir del Modo Kiosco
#define LSU_AFTER_POWEROFF  5         // apagar del todo (solo si el usuario activo "Apagado seguro")
static int lsuAfter    = LSU_AFTER_UNLOCK;
static int lsuAfterApp = -1;
#define LW_CLOCK   0x01   // reloj grande + fecha
#define LW_WEATHER 0x02   // clima (mock, sin datos reales aun)
#define LW_CAL     0x04   // calendario (mock, sin eventos reales aun)
#define LW_NOTIF   0x08   // notificaciones (datos reales: gNotifs[])
static uint8_t gLockWidgets = LW_CLOCK;       // widgets activos en Bloqueo (por defecto: solo el reloj, igual que hoy)
static int   gNavMode    = 0;                 // 0 = botones clasicos, 1 = gestos iOS
// Widgets redimensionables del Home (Fase 1): solo 2 tamanos, no continuo.
// bit0 = clima ancho (ocupa toda la fila), bit1 = noticias ancho. Mutuamente
// excluyentes -- si uno se ensancha, el otro se oculta para no tener que
// mover la rejilla de apps (que empieza justo debajo, a altura fija).
#define WW_CLIMA  0x01
#define WW_NOTICIAS 0x02
static uint8_t gWidgetWide = 0;               // por defecto: los dos normales, lado a lado (igual que hoy)
static int   gAnimStyle  = 0;                 // transicion al abrir/cerrar apps: 0=zoom, 1=fundido, 2=deslizar
static char  cfgName[24]  = "FlexOS Ultra";

// #############################################################
// ##  PREFERENCIAS DEL TECLADO (Fases A-G)  ·  todas en NVS
// ##  ------------------------------------------------------
// ##  Viven aqui, con el resto de la configuracion, para que
// ##  cfgLoad()/cfgSavePrefs() las traten EXACTAMENTE igual que
// ##  gDark o gAnimStyle: mismo sitio, mismo momento, misma
// ##  namespace "flexos". Los valores por defecto son los del
// ##  teclado de siempre, asi que una placa que actualice no
// ##  nota ningun cambio hasta que el usuario toque Ajustes.
// #############################################################
#define KB_SIZE_COMPACT 0
#define KB_SIZE_NORMAL  1
#define KB_SIZE_BIG     2
static int  gKbSize     = KB_SIZE_NORMAL;   // NVS "kbsize"   Fase A
static bool gKbFastType = true;             // NVS "kbfast"   Fase B (escritura rapida / multitoque)
static bool gKbToolbar  = true;             // NVS "kbtool"   Fase C (barra superior)
static bool gKbPredict  = true;             // NVS "kbpred"   Fase F (texto predictivo)
static bool gKbSpell    = false;            // NVS "kbspell"  Fase F (revision ortografica basica)
static bool gKbEmojiSug = false;            // NVS "kbemoji"  Fase F (sugerir emojis)
static bool gKbHiCon    = false;            // NVS "kbhicon"  Fase E (teclado de contraste alto)
static int  gKbOpacity  = 100;              // NVS "kbopa"    Fase E (opacidad del panel, 40..100)
static int  gKbStyle    = 0;                // NVS "kbstyle"  Fase E (0 redondeada, 1 cuadrada, 2 contorno)
static int  gKbFontSc   = 1;                // NVS "kbfont"   Fase E (0 pequena, 1 normal, 2 grande)
static int  gKbLpMs     = 500;              // NVS "kblp"     Fase E (umbral de long-press: 350/500/700)
static int  gKbFxMs     = 100;              // NVS "kbfx"     Fase E/G (duracion del destello de tecla)
// Fila de simbolos personalizados (Fase E). Se guardan como INDICES dentro de
// KB_SYM_POOL (ver mas abajo) y no como texto libre: asi es imposible acabar
// con un caracter que la fuente 5x7 no sepa dibujar.
#define KB_SYMS 4
static int gKbSym[KB_SYMS] = { 0, 1, 2, 3 };   // NVS "kbsyms" (4 bytes)
// Atajos de texto (Fase E, los usa la Fase F al completar palabra). Sin struct
// a proposito: dos matrices de char[] planas se guardan en NVS con un solo
// putBytes cada una y no obligan a declarar un tipo nuevo.
#define KB_SC_MAX  8
#define KB_SC_ABR  10
#define KB_SC_EXP  24
static char gKbScAbr[KB_SC_MAX][KB_SC_ABR];
static char gKbScExp[KB_SC_MAX][KB_SC_EXP];
// Atajos de fabrica: se escriben la primera vez que arranca (o al restablecer).
static void kbShortcutsDefaults(){
  memset(gKbScAbr, 0, sizeof(gKbScAbr));
  memset(gKbScExp, 0, sizeof(gKbScExp));
  snprintf(gKbScAbr[0], KB_SC_ABR, "xq");  snprintf(gKbScExp[0], KB_SC_EXP, "porque");
  snprintf(gKbScAbr[1], KB_SC_ABR, "q");   snprintf(gKbScExp[1], KB_SC_EXP, "que");
  snprintf(gKbScAbr[2], KB_SC_ABR, "tb");  snprintf(gKbScExp[2], KB_SC_EXP, "tambi\xC3\xA9n");
  snprintf(gKbScAbr[3], KB_SC_ABR, "pf");  snprintf(gKbScExp[3], KB_SC_EXP, "por favor");
}
static void kbPrefsNormalize(){
  if(gKbSize < 0 || gKbSize > 2) gKbSize = KB_SIZE_NORMAL;
  if(gKbStyle < 0 || gKbStyle > 2) gKbStyle = 0;
  if(gKbFontSc < 0 || gKbFontSc > 2) gKbFontSc = 1;
  if(gKbOpacity < 40) gKbOpacity = 40; if(gKbOpacity > 100) gKbOpacity = 100;
  if(gKbLpMs != 350 && gKbLpMs != 500 && gKbLpMs != 700) gKbLpMs = 500;
  if(gKbFxMs != 60 && gKbFxMs != 100 && gKbFxMs != 160) gKbFxMs = 100;
  for(int i = 0; i < KB_SYMS; i++) if(gKbSym[i] < 0 || gKbSym[i] > 15) gKbSym[i] = i;
  for(int i = 0; i < KB_SC_MAX; i++){ gKbScAbr[i][KB_SC_ABR - 1] = 0; gKbScExp[i][KB_SC_EXP - 1] = 0; }
}
static void kbPrefsLoad(){
  gKbSize     = prefs.getInt("kbsize", KB_SIZE_NORMAL);
  gKbFastType = prefs.getBool("kbfast", true);
  gKbToolbar  = prefs.getBool("kbtool", true);
  gKbPredict  = prefs.getBool("kbpred", true);
  gKbSpell    = prefs.getBool("kbspell", false);
  gKbEmojiSug = prefs.getBool("kbemoji", false);
  gKbHiCon    = prefs.getBool("kbhicon", false);
  gKbOpacity  = prefs.getInt("kbopa", 100);
  gKbStyle    = prefs.getInt("kbstyle", 0);
  gKbFontSc   = prefs.getInt("kbfont", 1);
  gKbLpMs     = prefs.getInt("kblp", 500);
  gKbFxMs     = prefs.getInt("kbfx", 100);
  { uint8_t sb[KB_SYMS]; size_t n = prefs.getBytes("kbsyms", sb, KB_SYMS);
    if(n == KB_SYMS) for(int i = 0; i < KB_SYMS; i++) gKbSym[i] = (int)sb[i]; }
  size_t na = prefs.getBytes("kbscabr", gKbScAbr, sizeof(gKbScAbr));
  size_t ne = prefs.getBytes("kbscexp", gKbScExp, sizeof(gKbScExp));
  if(na != sizeof(gKbScAbr) || ne != sizeof(gKbScExp)) kbShortcutsDefaults();
  kbPrefsNormalize();
}
static void kbPrefsSaveOpen(){        // se llama con prefs YA abierto en escritura
  prefs.putInt("kbsize", gKbSize);
  prefs.putBool("kbfast", gKbFastType);
  prefs.putBool("kbtool", gKbToolbar);
  prefs.putBool("kbpred", gKbPredict);
  prefs.putBool("kbspell", gKbSpell);
  prefs.putBool("kbemoji", gKbEmojiSug);
  prefs.putBool("kbhicon", gKbHiCon);
  prefs.putInt("kbopa", gKbOpacity);
  prefs.putInt("kbstyle", gKbStyle);
  prefs.putInt("kbfont", gKbFontSc);
  prefs.putInt("kblp", gKbLpMs);
  prefs.putInt("kbfx", gKbFxMs);
  { uint8_t sb[KB_SYMS]; for(int i = 0; i < KB_SYMS; i++) sb[i] = (uint8_t)gKbSym[i];
    prefs.putBytes("kbsyms", sb, KB_SYMS); }
  prefs.putBytes("kbscabr", gKbScAbr, sizeof(gKbScAbr));
  prefs.putBytes("kbscexp", gKbScExp, sizeof(gKbScExp));
}
// Guarda SOLO las preferencias del teclado (abre y cierra por su cuenta). Lo
// usan las filas de la pantalla de Ajustes del teclado, que cambian una cosa
// cada vez y no tienen por que reescribir las doce claves del sistema.
static void kbPrefsSave(){
  prefs.begin("flexos", false);
  kbPrefsSaveOpen();
  prefs.end();
}

static void cfgLoad(){
  prefs.begin("flexos", true);
  cfgOobeDone = prefs.getBool("oobe", false);
  cfgLang     = prefs.getInt("lang", 0);
  g24h        = prefs.getBool("h24", false);
  uiGlass     = prefs.getBool("glass", false);
  gDark       = prefs.getBool("dark", true);
  gIconStyle  = prefs.getInt("iconstyle", 0);
  gBright     = prefs.getInt("bright", 80);
  gLockType   = prefs.getInt("locktype", 0);
  // FASE 1: el contador de fallos vive en NVS a proposito -- reiniciar la placa
  // NO es una via de escape para saltarse la espera progresiva.
  lockFails   = prefs.getInt("lockfails", 0);
  if(lockFails < 0) lockFails = 0;                                        // prefs corruptas
  gAutoLockMs = (uint32_t)prefs.getInt("autolockms", (int)AUTOLOCK_DEFAULT_MS);
  autoLockNormalize();
  // APAGADO SEGURO: preferencia de usuario (Ajustes -> Seguridad). Por defecto
  // DESACTIVADA para no cambiar el comportamiento de nadie al actualizar.
  gPoffPin    = prefs.getBool("poffpin", false);
  gAppLock    = (uint16_t)prefs.getInt("applockm", 0);                    // FASE 2: candados por app
  // FASE 4: el kiosco sobrevive al reinicio A PROPOSITO -- apagar el telefono no
  // puede ser la via facil de escape. Al arrancar se vuelve a entrar en la misma
  // app y solo el PIN/contrasena permite salir.
  kioskOn     = prefs.getBool("kioskon", false);
  kioskApp    = prefs.getInt("kioskapp", -1);
  kioskExX    = prefs.getInt("kioskx", 0);
  kioskExY    = prefs.getInt("kiosky", 0);
  kioskExW    = prefs.getInt("kioskw", 0);
  kioskExH    = prefs.getInt("kioskh", 0);
  if(kioskApp < 0 || kioskApp > 15) { kioskOn = false; kioskApp = -1; }   // prefs corruptas
  if(gLockType == 0) kioskOn = false;   // sin clave configurada no habria forma de salir: no se activa
  gLockWidgets = (uint8_t)prefs.getInt("lockwidgets", LW_CLOCK);
  gNavMode    = prefs.getInt("navmode", 0);
  gWidgetWide = (uint8_t)prefs.getInt("widgetwide", 0);
  if((gWidgetWide & WW_CLIMA) && (gWidgetWide & WW_NOTICIAS)) gWidgetWide = 0;  // combinacion invalida (prefs corruptas): normaliza
  gAnimStyle  = prefs.getInt("animstyle", 0);
  kbPrefsLoad();                     // Fases A-G: preferencias del teclado
  String n = prefs.getString("name", "FlexOS Ultra");
  n.toCharArray(cfgName, sizeof(cfgName));
  prefs.end();
}
// Guarda las preferencias personalizables (idioma, formato, estilo, brillo)
static void cfgSavePrefs(){
  prefs.begin("flexos", false);
  prefs.putInt("lang", cfgLang);
  prefs.putBool("h24", g24h);
  prefs.putBool("glass", uiGlass);
  prefs.putBool("dark", gDark);
  prefs.putInt("iconstyle", gIconStyle);
  prefs.putInt("bright", gBright);
  prefs.putInt("lockwidgets", gLockWidgets);
  prefs.putInt("navmode", gNavMode);
  prefs.putInt("widgetwide", gWidgetWide);
  prefs.putInt("animstyle", gAnimStyle);
  prefs.putInt("autolockms", (int)gAutoLockMs);
  prefs.putBool("poffpin", gPoffPin);
  kbPrefsSaveOpen();                 // Fases A-G: preferencias del teclado
  prefs.end();
}
// FASE 1: se guarda SOLO el contador de fallos. Aparte de cfgSavePrefs porque se
// escribe en momentos muy distintos (cada fallo / cada acierto) y no queremos
// reescribir doce claves por cada digito equivocado.
static void lockFailsSave(){
  prefs.begin("flexos", false);
  prefs.putInt("lockfails", lockFails);
  prefs.end();
}
// ---- FASE 2: candado por app (bitmask indexado por indice de APP_REG) ----
static bool appLockGet(int id){
  if(!APPLOCK_ON || id < 0 || id > 15) return false;
  return (gAppLock & (uint16_t)(1u << id)) != 0;
}
static void appLockSet(int id, bool on){
  if(id < 0 || id > 15) return;
  if(on) gAppLock |=  (uint16_t)(1u << id);
  else   gAppLock &= (uint16_t)~(1u << id);
  prefs.begin("flexos", false);
  prefs.putInt("applockm", (int)gAppLock);
  prefs.end();
}
// ---- FASE 4: persistencia del Modo Kiosco --------------------------------
static void kioskSave(){
  prefs.begin("flexos", false);
  prefs.putBool("kioskon", kioskOn);
  prefs.putInt("kioskapp", kioskApp);
  prefs.putInt("kioskx", kioskExX);
  prefs.putInt("kiosky", kioskExY);
  prefs.putInt("kioskw", kioskExW);
  prefs.putInt("kioskh", kioskExH);
  prefs.end();
}
static void cfgSaveOobe(){
  prefs.begin("flexos", false);
  prefs.putBool("oobe", true);
  prefs.putInt("lang", cfgLang);
  prefs.putString("name", cfgName);
  prefs.end();
  cfgOobeDone = true;
}

// -------- Idiomas --------
#define NLANG 6
static const char* LANG_ENDONYM[NLANG] = {
  "Espa\xC3\xB1ol", "English", "Fran\xC3\xA7" "ais", "Portugu\xC3\xAas", "Italiano", "\xE4\xB8\xAD\xE6\x96\x87" };
// idx de arrays de fecha (ZH no tiene glifos -> usa EN)
static inline int LI(){ return (cfgLang == 5) ? 1 : cfgLang; }

// Cadenas de interfaz. Columnas: ES,EN,FR,PT,IT. ZH usa EN.
enum { S_SELLANG, S_CONTINUE, S_YOURNAME, S_NAMEHINT, S_START, S_SWIPE,
       S_WEATHER, S_NEWS, S_NONEWS, S_NOEVENTS, S_NOTIFS, S_NONOTIFS, S_SOON, S_M2, S_BACK, S_WELCOME, S_NSTR };
static const char* CH[S_NSTR][5] = {
  {"Selecciona tu idioma","Select your language","Choisis ta langue","Selecione o idioma","Seleziona la lingua"},
  {"Continuar","Continue","Continuer","Continuar","Continua"},
  {"\xC2\xBF" "C\xC3\xB3mo se llama el equipo?","Name your device","Nomme ton appareil","Nomeie o dispositivo","Nomina il dispositivo"},
  {"Toca para escribir","Tap to type","Touche pour \xC3\xA9" "crire","Toque para escrever","Tocca per scrivere"},
  {"Comenzar","Get started","Commencer","Come\xC3\xA7" "ar","Inizia"},
  {"Desliza arriba para desbloquear","Swipe up to unlock","Glisse vers le haut","Deslize para desbloquear","Scorri per sbloccare"},
  {"Clima","Weather","M\xC3\xA9t\xC3\xA9o","Clima","Meteo"},
  {"Noticias","News","Actualit\xC3\xA9s","Not\xC3\xAD" "cias","Notizie"},
  {"(No hay noticias que mostrar)","(No news to show)","(Aucune actualit\xC3\xA9)","(Sem not\xC3\xAD" "cias)","(Nessuna notizia)"},
  {"(Sin eventos)","(No events)","(Aucun \xC3\xA9v\xC3\xA9nement)","(Sem eventos)","(Nessun evento)"},
  {"Notificaciones","Notifications","Notifications","Notifica\xC3\xA7\xC3\xB5" "es","Notifiche"},
  {"(Sin notificaciones)","(No notifications)","(Aucune notification)","(Sem notifica\xC3\xA7\xC3\xB5" "es)","(Nessuna notifica)"},
  {"En construcci\xC3\xB3n","Coming soon","Bient\xC3\xB4t disponible","Em breve","Prossimamente"},
  {"Llega en el Milestone 2","Arrives in Milestone 2","Arrive au Milestone 2","Chega no Milestone 2","Arriva nel Milestone 2"},
  {"Volver","Back","Retour","Voltar","Indietro"},
  {"Bienvenido a","Welcome to","Bienvenue sur","Bem-vindo ao","Benvenuto in"},
};
static const char* t(int id){ return CH[id][LI()]; }

// Etiquetas de apps. Mismo orden que el enum IC_*.
static const char* APP[16][5] = {
  {"Reloj","Clock","Horloge","Rel\xC3\xB3gio","Orologio"},
  {"Galer\xC3\xAD" "a","Gallery","Galerie","Galeria","Galleria"},
  {"Multimedia","Media","Multim\xC3\xA9" "dia","Multim\xC3\xAD" "dia","Multimedia"},
  {"Almacenamiento","Storage","Stockage","Armazenamento","Archivi"},
  {"Modo PC","PC Mode","Mode PC","Modo PC","Modo PC"},
  {"Notas","Notes","Notes","Notas","Note"},
  {"Educaci\xC3\xB3n","Education","\xC3\x89" "ducation","Educa\xC3\xA7\xC3\xA3o","Istruzione"},
  {"Navegador","Browser","Navigateur","Navegador","Browser"},
  {"Code IDE","Code IDE","Code IDE","Code IDE","Code IDE"},
  {"Bienestar","Wellbeing","Bien-\xC3\xAatre","Bem-estar","Benessere"},
  {"Paint","Paint","Dessin","Paint","Disegno"},
  {"Juegos","Games","Jeux","Jogos","Giochi"},
  {"Ajustes","Settings","R\xC3\xA9glages","Ajustes","Impostazioni"},
  {"Calculadora","Calculator","Calculatrice","Calculadora","Calcolatrice"},
  {"Calendario","Calendar","Calendrier","Calend\xC3\xA1rio","Calendario"},
  {"C\xC3\xA1mara","Camera","Appareil","C\xC3\xA2mera","Fotocamera"},
};
static const char* appName(int id){ return APP[id][LI()]; }

// -------- Nombres de dias/meses --------
static const char* WD_FULL[5][7] = {
  {"Domingo","Lunes","Martes","Mi\xC3\xA9rcoles","Jueves","Viernes","S\xC3\xA1" "bado"},
  {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"},
  {"Dimanche","Lundi","Mardi","Mercredi","Jeudi","Vendredi","Samedi"},
  {"Domingo","Segunda","Ter\xC3\xA7" "a","Quarta","Quinta","Sexta","S\xC3\xA1" "bado"},
  {"Domenica","Luned\xC3\xAC","Marted\xC3\xAC","Mercoled\xC3\xAC","Gioved\xC3\xAC","Venerd\xC3\xAC","Sabato"},
};
static const char* WD_SHORT[5][7] = {
  {"dom","lun","mar","mi\xC3\xA9","jue","vie","s\xC3\xA1" "b"},
  {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"},
  {"dim","lun","mar","mer","jeu","ven","sam"},
  {"dom","seg","ter","qua","qui","sex","s\xC3\xA1" "b"},
  {"dom","lun","mar","mer","gio","ven","sab"},
};
static const char* MO_FULL[5][12] = {
  {"enero","febrero","marzo","abril","mayo","junio","julio","agosto","septiembre","octubre","noviembre","diciembre"},
  {"January","February","March","April","May","June","July","August","September","October","November","December"},
  {"janvier","f\xC3\xA9vrier","mars","avril","mai","juin","juillet","ao\xC3\xBBt","septembre","octobre","novembre","d\xC3\xA9" "cembre"},
  {"janeiro","fevereiro","mar\xC3\xA7" "o","abril","maio","junho","julho","agosto","setembro","outubro","novembro","dezembro"},
  {"gennaio","febbraio","marzo","aprile","maggio","giugno","luglio","agosto","settembre","ottobre","novembre","dicembre"},
};
static const char* MO_SHORT[5][12] = {
  {"ene","feb","mar","abr","may","jun","jul","ago","sep","oct","nov","dic"},
  {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"},
  {"jan","f\xC3\xA9v","mar","avr","mai","jui","jul","ao\xC3\xBB","sep","oct","nov","d\xC3\xA9" "c"},
  {"jan","fev","mar","abr","mai","jun","jul","ago","set","out","nov","dez"},
  {"gen","feb","mar","apr","mag","giu","lug","ago","set","ott","nov","dic"},
};

// -------- Reloj interno (sin NTP; se siembra a sab, 4 jul 13:23) --------
static int rtcY = 2026, rtcMo = 7, rtcD = 4, rtcWd = 6, rtcH = 13, rtcMin = 23;
static long          seedMinOfDay = 13 * 60 + 23;
static unsigned long clkBootMs = 0;
static long          clkLastMin = -1;

static int daysInMonth(int y, int m){
  static const int dm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if(m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return dm[m - 1];
}
static void clkSetDate(long addDays){
  int Y = 2026, Mo = 7, D = 4, Wd = 6;
  for(long i = 0; i < addDays; i++){
    D++; Wd = (Wd + 1) % 7;
    if(D > daysInMonth(Y, Mo)){ D = 1; Mo++; if(Mo > 12){ Mo = 1; Y++; } }
  }
  rtcY = Y; rtcMo = Mo; rtcD = D; rtcWd = Wd;
}
// devuelve true si cambio el minuto (para repintar el reloj)
static bool clkUpdate(){
  long mins = seedMinOfDay + (long)((millis() - clkBootMs) / 60000UL);
  long mod = mins % 1440; if(mod < 0) mod += 1440;
  int h = (int)(mod / 60), mi = (int)(mod % 60);
  if(mins == clkLastMin) return false;
  clkLastMin = mins;
  rtcH = h; rtcMin = mi;
  clkSetDate(mins / 1440);
  return true;
}
// Reloj "H:MM" pelado. Lo usa el RELOJ GIGANTE vectorial (bloqueo y app Reloj),
// que solo sabe dibujar digitos y ':' -> aqui NO puede entrar el AM/PM.
static void clkStr12(char* out, size_t n){
  if(g24h){ snprintf(out, n, "%d:%02d", rtcH, rtcMin); return; }
  int h12 = rtcH % 12; if(h12 == 0) h12 = 12;
  snprintf(out, n, "%d:%02d", h12, rtcMin);
}
// Reloj para las BARRAS DE ESTADO (fuente normal, sí admite letras).
// Antes todas las barras llamaban a clkStr12, y como en 12 h no se anadia
// AM/PM, el ajuste "Formato de hora" no cambiaba NADA visible entre las 00:00
// y las 12:59, y a partir de esa hora "1:23" podia ser la una de la tarde o de
// la madrugada. Ahora 24 h -> "13:23" y 12 h -> "1:23 PM".
// Reserva al menos 12 bytes: "12:34 PM" son 9 con el terminador.
static void clkStrBar(char* out, size_t n){
  if(g24h){ snprintf(out, n, "%d:%02d", rtcH, rtcMin); return; }
  int h12 = rtcH % 12; if(h12 == 0) h12 = 12;
  snprintf(out, n, "%d:%02d %s", h12, rtcMin, (rtcH < 12) ? "AM" : "PM");
}
// #############################################################
// ##  PANTALLAS + MAQUINA DE ESTADOS + ARRANQUE  (original)
// #############################################################

// ---- Declaraciones adelantadas ----
static void blitToFb(uint16_t* src);
static const char* resetReasonStr();
static void showBootBanner();
static void splashFrame(uint8_t a);
static void splashTick();
static void enterOobeLang();  static void renderOobeLang();  static void oobeLangTick();
static void enterOobeName();  static void drawKeyboard();    static void drawNameField();
static bool hitKey(int px, int py, int &code); static void oobeNameTick();
static void buildLongDate(char* out, size_t n); static void buildShortDate(char* out, size_t n);
static void renderLock(); static void showLock();
static void renderHome(); static void showHome(); static bool hitHomeIcon(int px, int py, int &id);
static void enterHome();  static void enterApp(int id); static void appTick();
static void swPushAndCapture(uint8_t id); static void activarMultitarea(); static void swTick();  // App Switcher
static void swPushNoThumb(uint8_t id);   // apps landscape: sin miniatura (ver appClose)
static void lsuEnter(); static void lsuTick();             // Seguridad -> Bloqueo (PIN/Contraseña)
static void lsuStartVerify();                              // pedir PIN/contraseña al desbloquear
static void composeUnlock(int off); static void animateTo(int from, int to);
static void lockTick(); static void homeTick();
static bool handleiOSGestures();                          // gestos de la barra inferior (modo iOS)
static void lsuStartVerifyFor(int what, int id);          // FASE 3: verificar y luego hacer algo (abrir app, kiosco...)
static void ctxOpen(int slot); static void ctxTick();     // FASE 2: menu contextual de long-press
static void kioskShowBadge();                             // FASE 4: candado discreto de "kiosco activo"
static void kioskSetTick();                               // FASE 4: pantalla de definir el area excluida
static void kioskExitNow();                               // FASE 4: salida ya verificada
static void poffEnter();                                  // APAGADO: pantalla de confirmacion ("desliza para apagar")
static void poffTick();                                   // APAGADO: arrastre del slider + cancelar
static void poffBeginAnim();                              // APAGADO: confirmado -> arranca la animacion final
static void poffAnimTick();                               // APAGADO: un paso de la animacion (fundido + "Flex OS" + deep sleep)
static void kbsEnter();                                   // FASE E: Ajustes -> Teclado (pantalla propia)
static void kbsTick();                                    // FASE E: su tick, desde loop()
static void noteRenderAll();                              // Notas: repintado completo (lo llaman el portapapeles y los chips)
static void noteInsert(const char* s);                    // Notas: insercion en el cursor (la usa el portapapeles)

// ---- Estado global ----
// ST_CTX y ST_KIOSKSET son de las Fases 2 y 4. Se anaden al FINAL del enum a
// proposito: los valores de los estados anteriores no se mueven. Lo mismo vale
// para los dos estados del apagado, anadidos al final por el mismo motivo.
// ST_KBSET (Fase E del teclado) se anade tambien al final, por identico motivo.
enum { ST_SPLASH = 0, ST_OOBE_LANG, ST_OOBE_NAME, ST_LOCK, ST_HOME, ST_APP, ST_SWITCHER, ST_LOCKSETUP, ST_WIFI, ST_CTX, ST_KIOSKSET,
       ST_POWEROFF_CONFIRM, ST_POWEROFF_ANIM, ST_KBSET,
       // Anadidos AL FINAL por el mismo motivo que los anteriores: no mover
       // el valor de ningun estado ya existente.
       ST_CONN,        // Conectividad: Wifi / BLE / Modo avion
       ST_FILES };     // Explorador de archivos real

static int  gState = ST_SPLASH;
static unsigned long splashStart = 0;
static int  lockOff = 0, lastLockOff = -1;
static int  oobeSel = 0;
static int  gAppId  = 0;
static bool editMode = false;                                   // Modo Edicion del Home
static uint8_t homeOrder[12] = { 0,1,2,3,4,5,6,7,8,9,10,11 };   // app id por slot (reordenable)
static bool gMinChanged = false;   // lo pone loop(): true cuando cambia el minuto

// ---- Invalidacion de caches de pantalla ----
// homeBuf (y la cortina qsBuf, que se compone de el) son escritorios YA
// pintados. Si cambia un ajuste que altera su aspecto -idioma, formato de
// hora, Liquid Glass, estilo de iconos- hay que volver a componerlos, o al
// salir de Ajustes se ve el escritorio VIEJO hasta que cambie el minuto.
// Ese era el motivo de que "algo no coincidiera" tras tocar un ajuste.
static bool gHomeDirty = false;    // homeBuf no refleja los ajustes actuales
static bool qsDirty    = true;     // qsBuf debe recomponerse
// teclado
static int  keyN = 0;
static int  kX[48], kY[48], kW[48], kH[48], kCode[48];

static void blitToFb(uint16_t* src){ fbCopyBand(src, 0, SCR_H - 1); }

// ---------------- Banda forense de arranque ----------------
static const char* resetReasonStr(){
  switch(esp_reset_reason()){
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (voltaje)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    default:                return "OTRO";
  }
}
// Solo se muestra tras un reinicio ANORMAL (crash/watchdog/brownout),
// para que puedas leer el motivo sin monitor serie. En un encendido
// normal NO aparece: el arranque va directo al splash limpio.
static void showBootBanner(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(0,0,0));
  drawTextC(SCR_W / 2, SCR_H / 2 - 24, "FlexOS Ultra", 3, rgb565(235,238,245));
  char b[72];
  snprintf(b, sizeof(b), "ultimo reinicio: %s", resetReasonStr());
  drawTextC(SCR_W / 2, SCR_H / 2 + 22, b, 1, rgb565(240,185,90));
  drawTextC(SCR_W / 2, SCR_H / 2 + 42, "P4 480x800 - modo offline", 1, rgb565(140,150,170));
  flxFlushAll();
  delay(2200);
}

// ---------------- SPLASH (fundido sobre NEGRO ABSOLUTO) ----------------
static void splashFrame(uint8_t a){
  int size = 6;
  int ty = SCR_H / 2 - 40;
  int ss = 2, sty = ty + size * 7 + 20;
  int y0 = ty - 10, y1 = sty + ss * 7 + 8;
  setBuf(fb);
  fillRect(0, y0, SCR_W, y1 - y0 + 1, rgb565(0,0,0));                  // banda negra
  drawTextCA(SCR_W / 2, ty, "FlexOS Ultra", size, rgb565(255,255,255), a);
  drawTextCA(SCR_W / 2, sty, "ESP32-P4", ss, rgb565(170,182,200), (uint8_t)(a * 7 / 10));
  flxFlush(y0, y1);
  // puntos de carga (estilo movil): uno se ilumina en secuencia
  int dy = SCR_H - 128, phase = (int)((millis() / 320) % 3);
  fillRect(0, dy - 7, SCR_W, 15, rgb565(0,0,0));
  for(int i = 0; i < 3; i++)
    fillCircleAA(SCR_W / 2 - 16 + i * 16, dy, 4.0f,
                 (i == phase) ? rgb565(255,255,255) : rgb565(70,74,82));
  flxFlush(dy - 8, dy + 8);
}
static void splashTick(){
  unsigned long e = millis() - splashStart;
  uint8_t a;
  if(e < 600)       a = (uint8_t)(e * 255 / 600);
  else if(e < 2000) a = 255;
  else if(e < 2600) a = (uint8_t)(255 - (e - 2000) * 255 / 600);
  else {
    if(!cfgOobeDone) enterOobeLang();
    // FASE 4: si el telefono se apago con el kiosco puesto, se vuelve a entrar en
    // la misma app SIN pasar por el escritorio. La unica salida sigue siendo el
    // gesto del candado + PIN/contrasena.
    else if(KIOSK_ON && kioskOn && kioskApp >= 0){
      renderHome();                      // winRevealAnim compone sobre homeBuf
      enterApp(kioskApp);
      kioskShowBadge();
    }
    else { renderHome(); renderLock(); showLock(); gState = ST_LOCK; lockOff = 0; lastLockOff = -1; }
    return;
  }
  splashFrame(a);
  delay(16);
}

// ---------------- OOBE: idioma ----------------
static void renderOobeLang(){
  drawWallpaper(fb, false); setBuf(fb);
  drawTextC(SCR_W / 2, 78, t(S_SELLANG), 3, rgb565(255,255,255));
  int rowH = 74, gap = 14, x = 44, w = SCR_W - 88, y0 = 158;
  for(int i = 0; i < NLANG; i++){
    int y = y0 + i * (rowH + gap);
    bool sel = (i == oobeSel);
    fillRoundRectA(x, y, w, rowH, 18, rgb565(255,255,255), sel ? 235 : 55);
    uint16_t tc = sel ? rgb565(28,28,38) : rgb565(255,255,255);
    const char* lbl = (i == 5) ? "Chinese" : LANG_ENDONYM[i];
    drawText(x + 28, y + rowH / 2 - 10, lbl, 3, tc);
    if(sel){
      int chx = x + w - 48, chy = y + rowH / 2;
      strokeSeg(chx - 8, chy, chx - 2, chy + 8, 2, rgb565(40,160,90));
      strokeSeg(chx - 2, chy + 8, chx + 12, chy - 10, 2, rgb565(40,160,90));
    }
  }
  int by = SCR_H - 96, bw = SCR_W - 88, bx = 44;
  fillRoundRect(bx, by, bw, 60, 30, rgb565(255,255,255));
  drawTextC(SCR_W / 2, by + 21, t(S_CONTINUE), 3, rgb565(40,80,200));
  flxFlushAll();
}
static void enterOobeLang(){ cfgLang = 0; oobeSel = 0; gState = ST_OOBE_LANG; renderOobeLang(); }
static void oobeLangTick(){
  if(!T.tap) return;
  int rowH = 74, gap = 14, x = 44, w = SCR_W - 88, y0 = 158;
  for(int i = 0; i < NLANG; i++){
    int y = y0 + i * (rowH + gap);
    if(T.x >= x && T.x <= x + w && T.y >= y && T.y <= y + rowH){
      oobeSel = i; cfgLang = i; renderOobeLang(); return;
    }
  }
  int by = SCR_H - 96, bw = SCR_W - 88, bx = 44;
  if(T.x >= bx && T.x <= bx + bw && T.y >= by && T.y <= by + 60){ enterOobeName(); return; }
}

// ---------------- OOBE: nombre (teclado QWERTY) ----------------
static void kbAdd(int x, int y, int w, int h, int code, const char* cap){
  kX[keyN] = x; kY[keyN] = y; kW[keyN] = w; kH[keyN] = h; kCode[keyN] = code; keyN++;
  fillRoundRect(x, y, w, h, 8, rgb565(250,250,252));
  if(cap) drawTextC(x + w / 2, y + h / 2 - 7, cap, 2, rgb565(28,28,38));
  else if(code == -2) fillRoundRect(x + w / 2 - 30, y + h / 2 - 2, 60, 4, 2, rgb565(90,90,100)); // barra espacio
}
static void drawKeyboard(){
  keyN = 0;
  const char* r1 = "QWERTYUIOP";
  const char* r2 = "ASDFGHJKL";
  const char* r3 = "ZXCVBNM";
  int kw = 40, kh = 52, g = 6, step = 60, kbTop = 544;
  { int n = 10; int sx = (SCR_W - (n * kw + (n - 1) * g)) / 2;
    for(int i = 0; i < n; i++){ char c[2] = { r1[i], 0 }; kbAdd(sx + i * (kw + g), kbTop, kw, kh, r1[i], c); } }
  { int n = 9; int sx = (SCR_W - (n * kw + (n - 1) * g)) / 2;
    for(int i = 0; i < n; i++){ char c[2] = { r2[i], 0 }; kbAdd(sx + i * (kw + g), kbTop + step, kw, kh, r2[i], c); } }
  { int n = 7; int backW = 2 * kw + g;
    int totalW = n * kw + (n - 1) * g + g + backW;
    int sx = (SCR_W - totalW) / 2;
    for(int i = 0; i < n; i++){ char c[2] = { r3[i], 0 }; kbAdd(sx + i * (kw + g), kbTop + 2 * step, kw, kh, r3[i], c); }
    kbAdd(sx + n * (kw + g), kbTop + 2 * step, backW, kh, -1, "<-"); }
  { int okW = 120, y = kbTop + 3 * step;
    int spX = 8 + kw, spW = SCR_W - spX - (okW + g) - 8;
    kbAdd(spX, y, spW, kh, -2, NULL);
    kbAdd(spX + spW + g, y, okW, kh, -3, "OK"); }
}
static void drawNameField(){
  int fx = 44, fy = 150, fw = SCR_W - 88, fh = 64;
  fillRoundRect(fx, fy, fw, fh, 16, rgb565(255,255,255));
  if(strlen(cfgName) == 0)
    drawText(fx + 20, fy + fh / 2 - 8, t(S_NAMEHINT), 2, rgb565(150,150,158));
  else {
    int ex = drawText(fx + 20, fy + fh / 2 - 10, cfgName, 3, rgb565(24,24,30));
    fillRect(ex + 2, fy + 16, 3, fh - 32, rgb565(55,120,240));
  }
  flxFlush(fy - 2, fy + fh + 2);
}
static void enterOobeName(){
  gState = ST_OOBE_NAME;
  cfgName[0] = 0;
  drawWallpaper(fb, false); setBuf(fb);
  drawTextC(SCR_W / 2, 70, t(S_YOURNAME), 3, rgb565(255,255,255));
  drawKeyboard();
  drawNameField();
  flxFlushAll();
}
static bool hitKey(int px, int py, int &code){
  for(int i = 0; i < keyN; i++)
    if(px >= kX[i] && px <= kX[i] + kW[i] && py >= kY[i] && py <= kY[i] + kH[i]){ code = kCode[i]; return true; }
  return false;
}
static void oobeNameTick(){
  if(!T.tap) return;
  int code;
  if(!hitKey(T.x, T.y, code)) return;
  int L = strlen(cfgName);
  if(code >= 32){ if(L < 20){ cfgName[L] = (char)code; cfgName[L + 1] = 0; drawNameField(); } }
  else if(code == -1){ if(L > 0){ cfgName[L - 1] = 0; drawNameField(); } }
  else if(code == -2){ if(L > 0 && L < 20){ cfgName[L] = ' '; cfgName[L + 1] = 0; drawNameField(); } }
  else if(code == -3){
    if(strlen(cfgName) == 0) strcpy(cfgName, "FlexOS Ultra");
    cfgSaveOobe();
    renderHome(); renderLock(); showLock();
    gState = ST_LOCK; lockOff = 0; lastLockOff = -1;
  }
}

// ---------------- Fechas localizadas ----------------
static void buildLongDate(char* out, size_t n){
  int li = LI();
  const char* wd = WD_FULL[li][rtcWd];
  const char* mo = MO_FULL[li][rtcMo - 1];
  switch(cfgLang){
    case 0: case 3: snprintf(out, n, "%s, %d de %s", wd, rtcD, mo); break; // ES, PT
    case 2: case 4: snprintf(out, n, "%s %d %s", wd, rtcD, mo); break;     // FR, IT
    default:        snprintf(out, n, "%s, %s %d", wd, mo, rtcD); break;    // EN / ZH
  }
}
static void buildShortDate(char* out, size_t n){
  int li = LI();
  snprintf(out, n, "%s, %d %s", WD_SHORT[li][rtcWd], rtcD, MO_SHORT[li][rtcMo - 1]);
}

// ---------------- LOCK ----------------
// Barra de gestos (estilo iOS): pildora fina centrada cerca del borde inferior.
// yBottom = borde inferior de referencia (normalmente SCR_H). col por defecto blanca.
static void drawHomeIndicator(int yBottom, uint8_t alpha, uint16_t col = rgb565(255,255,255)){
  int barW = 130, barH = 5, radius = 2;
  int x = (SCR_W - barW) / 2;
  int y = yBottom - 20 - barH;          // 20 px de margen desde el borde
  fillRoundRectA(x, y, barW, barH, radius, col, alpha);
}


// Glifos pequeños para las tarjetas de widgets del bloqueo. Copia minima e
// independiente de los de RI_* (Ajustes -> drawRowGlyph), que se definen
// mas abajo en el archivo; asi esta funcion no depende de nada definido
// despues de ella (evita sorpresas con el auto-prototipado de Arduino).
// kind: 0 = nube (clima), 1 = calendario, cualquier otro = campana (notif).
static void lockGlyph(int kind, int cx, int cy, uint16_t col){
  switch(kind){
    case 0:
      fillCircle(cx - 4, cy + 2, 5, col); fillCircle(cx + 4, cy + 2, 6, col);
      fillCircle(cx, cy - 2, 6, col); fillRect(cx - 8, cy + 2, 16, 5, col); break;
    case 1:
      drawRoundRect(cx - 9, cy - 8, 18, 17, 3, col); fillRect(cx - 9, cy - 8, 18, 5, col);
      fillCircle(cx - 4, cy + 2, 1, col); fillCircle(cx + 3, cy + 2, 1, col); break;
    default:   // campana (notificaciones) -- domo + cuerpo conico + reborde + badajo
      fillCircle(cx, cy - 6, 5, col);
      fillTriangle(cx - 8, cy + 4, cx + 8, cy + 4, cx, cy - 6, col);
      fillRect(cx - 9, cy + 3, 18, 3, col);
      fillCircle(cx, cy + 9, 2, col); break;
  }
}
// Tarjeta compacta para un widget opcional del bloqueo (clima/calendario/
// notificaciones). Estilo Vidrio u overlay plano segun uiGlass, igual que
// el resto de superficies de la app.
static void lockWidgetCard(int y, int kind, const char* title, const char* val, uint16_t accent){
  int x = 28, w = SCR_W - 56, h = 50;
  if(uiGlass){ drawLiquidGlassPanel(x, y, w, h, 16, rgb565(40,50,90)); }
  else fillRoundRectA(x, y, w, h, 16, rgb565(255,255,255), 45);
  lockGlyph(kind, x + 30, y + h / 2, accent);
  drawText(x + 54, y + 9, title, 2, rgb565(255,255,255));
  drawText(x + 54, y + 30, val, 1, rgb565(205,214,232));
}

static void renderLock(){
  drawWallpaper(lockBuf, false); setBuf(lockBuf);
  drawWifi(SCR_W - 66, 40, 12, rgb565(255,255,255));
  drawBattery(SCR_W - 46, 31, 30, 15, 82, rgb565(255,255,255));
  if(gLockWidgets & LW_CLOCK){
    if(uiGlass){ drawLiquidGlassPanel(28, 198, SCR_W - 56, 252, 28, rgb565(40,62,128)); }  // vidrio tras el reloj
    char cs[8]; clkStr12(cs, sizeof(cs));
    drawBigClock(cs, SCR_W / 2, 242, 140, 18, rgb565(255,255,255));
    char ds[64]; buildLongDate(ds, sizeof(ds));
    drawTextC(SCR_W / 2, 242 + 140 + 36, ds, 3, rgb565(255,255,255));
  }
  // widgets opcionales (clima/calendario/notificaciones), apilados debajo
  // del reloj -- o mas arriba si el reloj esta desactivado, para no dejar
  // media pantalla vacia. El panel del reloj (y=198,h=252) termina en
  // y=450 -- el primer widget tiene que empezar DESPUES de eso, con margen
  // (antes empezaba en 448 y se solapaba 2px con el panel: el corte raro
  // que se ve en la foto justo debajo de la fecha).
  int wy = (gLockWidgets & LW_CLOCK) ? 462 : 200;
  if(gLockWidgets & LW_WEATHER){
    lockWidgetCard(wy, 0, t(S_WEATHER), "21C, Lima, Peru", rgb565(90,170,235));  // mock: sin fuente de datos real aun
    wy += 60;
  }
  if(gLockWidgets & LW_CAL){
    lockWidgetCard(wy, 1, appName(IC_CALEND), t(S_NOEVENTS), rgb565(235,110,90));  // mock: sin eventos reales aun
    wy += 60;
  }
  if(gLockWidgets & LW_NOTIF){
    const char* val = gNotifCount > 0 ? gNotifs[gNotifCount - 1].mod.name : t(S_NONOTIFS);  // dato real
    lockWidgetCard(wy, 2, t(S_NOTIFS), val, rgb565(230,180,90));
    wy += 60;
  }
  fillRoundRect(SCR_W / 2 - 70, SCR_H - 150, 140, 10, 5, rgb565(255,255,255));
  drawTextC(SCR_W / 2, SCR_H - 118, t(S_SWIPE), 2, rgb565(255,255,255));
  setBuf(fb);
}
static void showLock(){ blitToFb(lockBuf); flxFlushAll(); }

// ---------------- HOME ----------------
// Widgets del escritorio (clima, noticias, dock) en estilo Liquid Glass
// o plano segun uiGlass.
// Calcula el rectangulo de cada widget del Home segun gWidgetWide. Mutuamente
// excluyente: si uno esta "ancho" ocupa toda la fila (mismo alto de siempre,
// 120px) y el otro no se dibuja ese frame -- asi la rejilla de apps de abajo
// (a altura fija, gy0=212) nunca se tiene que mover. Compartida entre
// drawHomeWidgets() (para pintar) y edTick()/edRender() (para el asa de
// resize en Modo Edicion), asi no hay dos copias de esta geometria.
static void widgetLayout(int &cx, int &cy, int &cw, int &ch, bool &climaVis,
                          int &nx, int &ny, int &nw, int &nh, bool &newsVis){
  cy = ny = 72; ch = nh = 120;
  if(gWidgetWide & WW_CLIMA){        cx = 24; cw = 432; climaVis = true;  newsVis = false; nx = ny = nw = nh = 0; }
  else if(gWidgetWide & WW_NOTICIAS){ nx = 24; nw = 432; newsVis = true;  climaVis = false; cx = cy = cw = ch = 0; }
  else { cx = 24; cw = 208; nx = 24 + 208 + 16; nw = 208; climaVis = newsVis = true; }
}
static void drawHomeWidgets(uint32_t tm){
  int wx, wy, cw, ch, nx, ny, nw, nh; bool climaVis, newsVis;
  widgetLayout(wx, wy, cw, ch, climaVis, nx, ny, nw, nh, newsVis);
  uint16_t W = rgb565(255,255,255);
  if(climaVis){
    if(uiGlass) drawLiquidGlassPanel(wx, wy, cw, ch, 20, rgb565(30,72,150));
    else fillRoundRect(wx, wy, cw, ch, 20, rgb565(28,58,120));
    drawText(wx + 16, wy + 16, t(S_WEATHER), 2, W);
    fillCircle(wx + cw - 42, wy + 34, 13, rgb565(250,205,60));
    fillCircle(wx + cw - 58, wy + 52, 11, rgb565(236,240,248));
    fillCircle(wx + cw - 42, wy + 55, 13, rgb565(236,240,248));
    fillRect(wx + cw - 58, wy + 53, 28, 10, rgb565(236,240,248));
    { int ex = drawText(wx + 16, wy + 48, "21", 4, W);
      drawCircle(ex + 7, wy + 52, 4, W); drawCircle(ex + 7, wy + 52, 3, W); }
    drawText(wx + 16, wy + ch - 22, "Lima, Peru", 1, rgb565(215,224,240));
  }
  if(newsVis){
    if(uiGlass) drawLiquidGlassPanel(nx, ny, nw, nh, 20, rgb565(205,212,226));
    else fillRoundRect(nx, ny, nw, nh, 20, rgb565(240,242,246));
    uint16_t ntxt = uiGlass ? W : rgb565(40,40,50), nsub = uiGlass ? rgb565(238,240,248) : rgb565(120,120,132);
    drawText(nx + 16, ny + 16, t(S_NEWS), 2, ntxt);
    drawTextC(nx + nw / 2, ny + nh / 2, t(S_NONEWS), 1, nsub);
  }
  int dkx = 24, dky = SCR_H - 176, dkw = SCR_W - 48, dkh = 96;
  if(uiGlass) drawLiquidGlassPanel(dkx, dky, dkw, dkh, 28, rgb565(180,186,206));
  else fillRoundRectA(dkx, dky, dkw, dkh, 28, W, 45);
  int dS = 64, inner = dkw - 32, dgap = (inner - 4 * dS) / 3;
  for(int i = 0; i < 4; i++){ int ix = dkx + 16 + i * (dS + dgap), iy = dky + (dkh - dS) / 2; drawAppIcon(12 + i, ix, iy, dS); }
}

static void renderHome(){
  drawWallpaper(homeBuf, true); setBuf(homeBuf);
  gHomeDirty = false;                      // homeBuf ya refleja los ajustes actuales
  qsDirty    = true;                       // la cortina se compone de homeBuf: invalidar su cache
  // barra de estado
  char cs[12]; clkStrBar(cs, sizeof(cs));
  drawText(20, 16, cs, 2, rgb565(255,255,255));
  char sd[48]; buildShortDate(sd, sizeof(sd));
  drawText(20, 40, sd, 1, rgb565(238,240,246));
  drawWifi(SCR_W - 66, 28, 11, rgb565(255,255,255));
  drawBattery(SCR_W - 46, 20, 30, 15, 82, rgb565(255,255,255));
  // widgets (clima, noticias) + dock: estilo Liquid Glass o plano
  drawHomeWidgets(millis());
  // rejilla de apps 4x3
  int S = 72, gx0 = 24, gy0 = 212, rowStep = 112;
  if(!editMode) for(int i = 0; i < 12; i++){                    // en Modo Edicion los pinta edRender()
    int c = i % 4, r = i / 4;
    int ix = gx0 + c * 120, iy = gy0 + r * rowStep;
    drawAppIcon(homeOrder[i], ix, iy, S);
    drawTextC(ix + S / 2, iy + S + 6, appName(homeOrder[i]), 2, rgb565(255,255,255));
  }
  // puntos de pagina
  int dotsY = gy0 + 2 * rowStep + S + 34;
  for(int i = 0; i < 3; i++)
    fillCircleA(SCR_W / 2 - 18 + i * 18, dotsY, 4, rgb565(255,255,255), i == 0 ? 255 : 110);
  // barra de navegacion: botones clasicos o barra de gestos (modo iOS)
  if(gNavMode == 0){
    int ny = SCR_H - 52; uint16_t nv = rgb565(255,255,255);
    int bx = SCR_W / 6;
    fillTriangle(bx - 10, ny + 8, bx + 8, ny - 2, bx + 8, ny + 18, nv);   // atras
    drawCircle(SCR_W / 2, ny + 8, 12, nv); drawCircle(SCR_W / 2, ny + 8, 11, nv); // inicio
    int rx = SCR_W * 5 / 6;
    drawRoundRect(rx - 11, ny - 3, 22, 22, 4, nv);                        // recientes
  } else {
    drawHomeIndicator(SCR_H, 220);                                        // barra de gestos
  }
  setBuf(fb);
}
static void showHome(){ blitToFb(homeBuf); flxFlushAll(); }
static bool hitHomeIcon(int px, int py, int &id){
  int S = 72, gx0 = 24, gy0 = 212, rowStep = 112;
  for(int i = 0; i < 12; i++){
    int c = i % 4, r = i / 4;
    int ix = gx0 + c * 120, iy = gy0 + r * rowStep;
    if(px >= ix - 6 && px <= ix + S + 6 && py >= iy && py <= iy + S + 16){ id = homeOrder[i]; return true; }
  }
  int dkx = 24, dky = SCR_H - 176, dkw = SCR_W - 48, dkh = 96, dS = 64, inner = dkw - 32, dgap = (inner - 4 * dS) / 3;
  for(int i = 0; i < 4; i++){
    int ix = dkx + 16 + i * (dS + dgap), iy = dky + (dkh - dS) / 2;
    if(px >= ix && px <= ix + dS && py >= iy && py <= iy + dS){ id = 12 + i; return true; }
  }
  return false;
}

// ---------------- Desbloqueo con fisica (composicion) ----------------
static void composeUnlock(int off){
  if(off < 0) off = 0; if(off > SCR_H) off = SCR_H;
  for(int y = 0; y < SCR_H; y++){
    if(y < SCR_H - off)
      memcpy(fb + (size_t)y * SCR_W, lockBuf + (size_t)(y + off) * SCR_W, SCR_W * 2);
    else
      memcpy(fb + (size_t)y * SCR_W, homeBuf + (size_t)y * SCR_W, SCR_W * 2);
  }
  flxFlushAll();
}
// Deslizamiento del bloqueo. Antes eran 14 pasos fijos con delay(14) en medio:
// 14 frames en ~200 ms pasara lo que pasara, con el procesador parado la mitad
// del tiempo. Ahora es la MISMA duracion pero basada en tiempo y sin delay, asi
// que el bucle mete todos los frames que el compositor sea capaz de dar. Mismo
// recorrido y mismo ease-out; solo cambia la cadencia.
#define UNLOCK_ANIM_MS 200
static void animateTo(int from, int to){
  uint32_t t0 = millis();
  for(;;){
    uint32_t e = millis() - t0; if(e > (uint32_t)UNLOCK_ANIM_MS) e = UNLOCK_ANIM_MS;
    float p = (float)e / (float)UNLOCK_ANIM_MS;
    p = 1 - (1 - p) * (1 - p);                               // ease-out
    composeUnlock(from + (int)((to - from) * p));
    if(e >= (uint32_t)UNLOCK_ANIM_MS) break;
  }
  composeUnlock(to);
}
// Arranca la verificacion DESDE la pantalla de Bloqueo.
// Si el bloqueo lo puso el despertar de una suspension y antes habia una app
// abierta, se pide la verificacion con destino "abrir esa app": al acertar el
// PIN se vuelve exactamente donde estaba el usuario, no al escritorio. Se
// reutiliza LSU_AFTER_OPENAPP tal cual, sin inventar una ruta nueva.
static void lockStartVerify(){
#if SUSPEND_ON && SUSPEND_LOCK_ON
  int ret = gSuspRetState, app = gSuspRetApp;
  gSuspRetState = -1; gSuspRetApp = -1;      // se consume: solo vale para este desbloqueo
  if(ret == ST_APP && app >= 0){
    lsuStartVerifyFor(LSU_AFTER_OPENAPP, app);
    gLockVerifyLocked = true;                // (va DESPUES: lsuStartVerify resetea estado)
    return;
  }
#endif
  lsuStartVerify();
  gLockVerifyLocked = true;
}
static void lockTick(){
  if(gLockType > 0){
    // Con bloqueo: deslizar arriba lleva DIRECTO a verificar (nunca se revela el escritorio)
    if(T.down && (T.startY - T.y) > 60){ lockStartVerify(); return; }
    if(T.released && T.swipeUp){ lockStartVerify(); return; }
    return;
  }
  if(T.down){
    int off = T.startY - T.y; if(off < 0) off = 0; if(off > SCR_H) off = SCR_H;
    if(off != lastLockOff){ composeUnlock(off); lastLockOff = off; }
    lockOff = off;
  } else if(T.released){
    if(lockOff > SCR_H / 3 || T.swipeUp){ animateTo(lockOff, SCR_H); enterHome(); }
    else { animateTo(lockOff, 0); lockOff = 0; lastLockOff = -1; showLock(); }
  }
}
// #############################################################
// ##  MODO EDICION del Home (long-press, jiggle, drag & drop)
// #############################################################
static float edCurX[12], edCurY[12];        // posiciones animadas (resorte)
static int   edDrag = -1, edHoverSlot = -1; // icono arrastrado / slot bajo el dedo
static float edDragX = 0, edDragY = 0;
// Posicion del icono arrastrado, acotada al area de rejilla. Antes este limite
// solo se aplicaba en los frames de MOVIMIENTO: en el frame del agarre se
// escribia T.x-36 en crudo y el icono podia dibujarse hasta 36 px fuera.
static void edSetDrag(int tx, int ty){
  float dx = (float)(tx - 36), dy = (float)(ty - 36);
  if(dx < 8) dx = 8;
  if(dx > SCR_W - 80) dx = SCR_W - 80;
  if(dy < 140) dy = 140;
  if(dy > 500) dy = 500;
  edDragX = dx; edDragY = dy;
}
static unsigned long edHoverMs = 0, edMs = 0;

static void homeOrderSave(){ prefs.begin("flexos", false); prefs.putBytes("hord", homeOrder, 12); prefs.end(); }
static void homeOrderLoad(){
  prefs.begin("flexos", true); size_t n = prefs.getBytes("hord", homeOrder, 12); prefs.end();
  if(n != 12){ for(int i = 0; i < 12; i++) homeOrder[i] = i; return; }
  bool seen[12] = { false };                 // valida: ids unicos 0..11 (por si prefs corruptas)
  for(int i = 0; i < 12; i++){ if(homeOrder[i] >= 12 || seen[homeOrder[i]]){ for(int j = 0; j < 12; j++) homeOrder[j] = j; return; } seen[homeOrder[i]] = true; }
}
static void edSlotXY(int slot, int &x, int &y){ int c = slot % 4, r = slot / 4; x = 24 + c * 120; y = 212 + r * 112; }
static int  edSlotAt(int px, int py){
  if(px < 24 || py < 212) return -1;
  int c = (px - 24) / 120, r = (py - 212) / 112;
  if(c < 0 || c > 3 || r < 0 || r > 2) return -1;
  int slot = r * 4 + c; return slot < 12 ? slot : -1;
}
static void edMove(int from, int to){        // reinserta el icono (desplaza los demas)
  if(from == to || from < 0 || to < 0 || from >= 12 || to >= 12) return;
  uint8_t v = homeOrder[from];
  if(from < to) for(int i = from; i < to; i++) homeOrder[i] = homeOrder[i + 1];
  else          for(int i = from; i > to; i--) homeOrder[i] = homeOrder[i - 1];
  homeOrder[to] = v;
}
// Asa de resize de los widgets del Home (Fase 1: alterna 2 tamanos --
// normal/ancho -- no arrastre continuo. Con solo 2 estados posibles, un
// toque en el asa es mas confiable que afinar un umbral de distancia de
// arrastre, y evita tocar la maquina de estados de edDrag/dwell de abajo).
static void drawWidgetHandle(int x, int y){
  fillRoundRect(x, y, 24, 24, 8, rgb565(255,255,255));
  strokeSegAA(x + 6, y + 16, x + 16, y + 6, 2.0f, rgb565(60,80,140));
  strokeSegAA(x + 6, y + 10, x + 10, y + 6, 2.0f, rgb565(60,80,140));
  strokeSegAA(x + 14, y + 18, x + 18, y + 14, 2.0f, rgb565(60,80,140));
}
static bool widgetHandleAt(int px, int py, int &which){
  int cx, cy, cw, ch, nx, ny, nw, nh; bool cv, nv;
  widgetLayout(cx, cy, cw, ch, cv, nx, ny, nw, nh, nv);
  if(cv && px >= cx + cw - 40 && px <= cx + cw - 4 && py >= cy + ch - 40 && py <= cy + ch - 4){ which = 0; return true; }
  if(nv && px >= nx + nw - 40 && px <= nx + nw - 4 && py >= ny + nh - 40 && py <= ny + nh - 4){ which = 1; return true; }
  return false;
}
static void widgetToggleSize(int which){
  if(which == 0) gWidgetWide = (gWidgetWide & WW_CLIMA)    ? 0 : WW_CLIMA;
  else           gWidgetWide = (gWidgetWide & WW_NOTICIAS) ? 0 : WW_NOTICIAS;
  cfgSavePrefs();
  renderHome();          // homeBuf tiene que reflejar el nuevo layout antes de que edRender() lo recomponga
}
static void edRender(){
  // Los iconos en Modo Edicion ahora usan el gIconStyle REAL (Vidrio si esta
  // activo en Ajustes) en vez de forzarse a Plano. Cada icono Vidrio pasa por
  // drawLiquidGlassPanel() -- un blur real, no gratis -- y aqui se dibujan
  // hasta 12 por frame. Para no trompicar el jiggle/arrastre en la P4, si el
  // estilo es Vidrio se limita el refresco de ESTA funcion a ~20 fps (50 ms).
  // Ojo: esto es un throttle LOCAL (reutiliza edMs, declarada mas arriba y
  // hasta ahora sin usar) -- a proposito NO se toca uiAnimMs, que es el
  // throttle compartido de qsPanel/ripple y no debe frenarse por esto.
  // En estilo Plano no hay throttle: se conserva el mismo refresco fluido de
  // siempre.
  if(gIconStyle == 1){
    unsigned long now = millis();
    if(now - edMs < 50) return;
    edMs = now;
  }
  setBuf(bbuf);
  for(int j = 120; j < 580; j++) memcpy(bbuf + (size_t)j * SCR_W, homeBuf + (size_t)j * SCR_W, SCR_W * 2);  // fondo (sin rejilla)
  uint32_t t = millis();
  { int cx, cy, cw, ch, nx, ny, nw, nh; bool cv, nv;                          // asas de resize de los widgets
    widgetLayout(cx, cy, cw, ch, cv, nx, ny, nw, nh, nv);
    if(cv) drawWidgetHandle(cx + cw - 30, cy + ch - 30);
    if(nv) drawWidgetHandle(nx + nw - 30, ny + nh - 30);
  }
  for(int i = 0; i < 12; i++){
    if(i == edDrag) continue;
    int tx, ty; edSlotXY(i, tx, ty);
    edCurX[i] += (tx - edCurX[i]) * 0.2f; edCurY[i] += (ty - edCurY[i]) * 0.2f;   // resorte
    float ph = i * 0.6f;
    int ox = (int)(2 * sinf(t * 0.02f + ph)), oy = (int)(2 * cosf(t * 0.017f + ph));  // temblor +-2px
    int s = 64, off = (72 - s) / 2;                                              // escala ~90%
    drawAppIcon(homeOrder[i], (int)edCurX[i] + off + ox, (int)edCurY[i] + off + oy, s);
  }
  if(edDrag >= 0){                                                              // icono arrastrado (translucido)
    int dx = (int)edDragX, dy = (int)edDragY, s = 72;
    if(uiGlass) drawLiquidGlassPanel(dx - 6, dy - 6, s + 12, s + 12, 16, rgb565(120,140,205));
    else fillRoundRectA(dx - 6, dy - 6, s + 12, s + 12, 16, rgb565(60,80,140), 150);
    drawAppIcon(homeOrder[edDrag], dx, dy, s);
  }
  drawTextC(SCR_W / 2, 176, "Arrastra los iconos - Inicio para salir", 1, rgb565(230,234,244));
  present(120, 580);
}
static void edEnter(){
  editMode = true;
  renderHome();                              // homeBuf sin rejilla (editMode salta el grid)
  for(int i = 0; i < 12; i++){ int x, y; edSlotXY(i, x, y); edCurX[i] = x; edCurY[i] = y; }
  edDrag = -1; edHoverSlot = -1;
}
static void edExit(){
  editMode = false; edDrag = -1;
  homeOrderSave();
  renderHome(); showHome();
}
static void edTick(){
  if(T.pressed){
    int which;
    if(widgetHandleAt(T.x, T.y, which)){ widgetToggleSize(which); edRender(); return; }
    edDrag = edSlotAt(T.x, T.y); edSetDrag(T.x, T.y); edHoverSlot = -1; edRender(); return;
  }
  if(T.down && edDrag >= 0){
    edSetDrag(T.x, T.y);
    int over = edSlotAt((int)edDragX + 36, (int)edDragY + 36);                  // slot bajo el centro
    if(over >= 0 && over != edDrag){
      if(over != edHoverSlot){ edHoverSlot = over; edHoverMs = millis(); }
      else if(millis() - edHoverMs > 400){ edMove(edDrag, over); edDrag = over; edHoverSlot = -1; }  // dwell 400ms
    } else edHoverSlot = -1;
    edRender(); return;
  }
  if(T.released){
    if(edDrag >= 0){ edDrag = -1; homeOrderSave(); edRender(); }                // soltar -> fija
    else if(T.tap) edExit();                                                    // toque en vacio/Inicio -> salir
    return;
  }
  // reposo: el jiggle continuo lo mueve uiTick()
}

// ---- Destello de reflejo al tocar un icono (estilo "Vidrio") ----
// Circulo blanco que crece y se desvanece (~0.5 s) desde el punto exacto
// donde se toco. Se activa en homeTick() (T.pressed sobre un icono) y se
// anima aqui; se llama desde uiTick() solo mientras gState==ST_HOME, asi
// que si se abre otra pantalla (enterApp) el destello deja de dibujarse
// de inmediato aunque el temporizador no haya terminado.
static bool     gRippleActive = false;
static int      gRippleX = 0, gRippleY = 0;
static uint32_t gRippleStart = 0;
static const uint32_t RIPPLE_DUR_MS = 500;
static const int      RIPPLE_MAX_R  = 70;
static void animateIconRipple(){
  if(gIconStyle != 1 || !bbuf || !homeBuf){ gRippleActive = false; return; }
  setBuf(bbuf);
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;   // recorte completo
  // REPINTADO PARCIAL. El destello es un circulo de radio <= RIPPLE_MAX_R
  // centrado en el punto tocado, que NO se mueve durante la animacion. Antes se
  // recopiaba/volcaba la banda entera 64..726; ahora solo la franja vertical que
  // el circulo puede alcanzar (centro +- radio maximo, acotada a la pantalla).
  // El resto de fb ya es correcto. Salida byte-identica, mucho menos memcpy por
  // frame durante el ~medio segundo del efecto.
  int y0 = gRippleY - RIPPLE_MAX_R; if(y0 < 64)  y0 = 64;
  int y1 = gRippleY + RIPPLE_MAX_R; if(y1 > 726) y1 = 726;
  for(int j = y0; j <= y1; j++) memcpy(bbuf + (size_t)j * SCR_W, homeBuf + (size_t)j * SCR_W, SCR_W * 2);
  uint32_t e = millis() - gRippleStart;
  if(e < RIPPLE_DUR_MS){
    float   p = (float)e / RIPPLE_DUR_MS;                // 0..1
    int     r = (int)(RIPPLE_MAX_R * p);                  // crece
    uint8_t a = (uint8_t)(160 * (1.0f - p));              // se desvanece
    if(r > 0 && a > 0) fillCircleA(gRippleX, gRippleY, r, rgb565(255,255,255), a);
  } else {
    gRippleActive = false;                                // termino: este frame sale limpio (sin circulo)
  }
  present(y0, y1);
}
static void homeTick(){
  if(editMode){ edTick(); return; }
  if(gNavMode == 1 && handleiOSGestures()) return;   // gestos iOS antes que los toques normales
  // destello Liquid Glass al posar el dedo sobre un icono (solo estilo "Vidrio")
  if(T.pressed && gIconStyle == 1){
    int rid;
    if(hitHomeIcon(T.x, T.y, rid)){ gRippleActive = true; gRippleX = T.x; gRippleY = T.y; gRippleStart = millis(); }
  }
  // pulsacion larga (>1000 ms sin mover) sobre un icono de la rejilla.
  // FASE 2: ya no salta directo a Modo Edicion -- abre primero el menu
  // contextual. Con CTXMENU_ON en 0 se recupera exactamente el comportamiento
  // anterior (jiggle + agarre del icono bajo el dedo).
  if(T.down && edSlotAt(T.startX, T.startY) >= 0 && (millis() - T.downMs) > 1000
     && abs(T.x - T.startX) < 12 && abs(T.y - T.startY) < 12){
    int slot = edSlotAt(T.startX, T.startY);
    if(CTXMENU_ON){ ctxOpen(slot); return; }
    edEnter(); edDrag = slot; edSetDrag(T.x, T.y); return;
  }
  if(T.tap){
    if(T.x > SCR_W * 2 / 3 && T.y > SCR_H - 72){ activarMultitarea(); return; }   // boton Recientes
    int id;
    if(hitHomeIcon(T.x, T.y, id)){
      // FASE 3: si la app tiene candado, la verificacion va ANTES de abrirla.
      // Se reutiliza lsuStartVerify (misma UI, mismo contador de fallos, misma
      // espera progresiva de la Fase 1); al acertar, lsuFinishAfter abre la app
      // por el camino normal (enterApp).
      if(APPLOCK_ON && appLockGet(id) && gLockType > 0){ lsuStartVerifyFor(LSU_AFTER_OPENAPP, id); return; }
      enterApp(id);
    }
  }
}

// #############################################################
// ##  FRAMEWORK DE VENTANAS / APPS  (Milestone 2 - base)
// #############################################################
// Cada app expone dos callbacks: enter() dibuja su contenido inicial en
// el AREA DE VENTANA, y tick() (opcional) actualiza por frame. El marco
// (barra de estado + cabecera con "atras" + barra de navegacion) y los
// gestos de cierre los gestiona el framework: las apps solo pintan su
// contenido. Para rellenar una app, se reemplaza su entrada en APP_REG.

// Area de contenido. Embebida en DeX no hay barra de estado ni barra de
// navegacion que esquivar, asi que el contenido ocupa TODO el lienzo: es lo que
// elimina las franjas vacias de arriba y abajo dentro de la ventana.
// Son macros que se expanden en el punto de uso, y todos los usos quedan por
// debajo de donde se declaran gHosted y gAppH (justo aqui abajo, con los flags
// de APP_REG), asi que la expansion siempre las conoce.
#define WIN_TOP (gHosted ? 0 : 96)
#define WIN_BOT (gHosted ? gAppH : (SCR_H - 64))
#define WIN_BG  rgb565(18, 20, 28)  // fondo de ventana (oscuro, profesional)

typedef struct { void (*enter)(); void (*tick)(); uint8_t flags; } FlexApp;
#define APP_CUSTOM_HEADER 1   // la app pinta su propia cabecera (no la centrada)
#define APP_OWN_TOUCH     2   // la app gestiona TODOS sus toques (solo swipe-derecha cierra)
#define APP_LAND          4   // la app dibuja en LANDSCAPE (pone gLand por su cuenta)
// true mientras se re-ejecuta enter() SOLO para volver a maquetar tras un
// cambio de tamano. Una app cuyo enter() tambien inicializa estado (la
// Calculadora pone el display a "0", Ajustes reinicia scroll y seleccion) debe
// saltarse esa parte: al redimensionar se re-dibuja, no se reinicia.
static bool gRelayout = false;
#define APP_FLEX          8   // la app maqueta contra gAppW/gAppH -> se le da un
                              // lienzo del TAMANO REAL de la ventana y se dibuja
                              // 1:1, sin escalar ni barras de letterbox.
// Lienzo LOGICO de la app en curso. A pantalla completa es la pantalla entera;
// dentro de una ventana de DeX, para una app APP_FLEX, es el area de cliente.
// Las apps adaptativas maquetan contra esto en vez de contra SCR_W/SCR_H.
static int gAppW = SCR_W, gAppH = SCR_H;
// Hosting en Modo PC: una app corriendo dentro de una ventana de DeX NO puede
// navegar por el sistema (cerrarse a Home, abrir otra app a pantalla completa o
// saltar al selector) -- eso desmontaria el escritorio que la contiene. Cuando
// gHosted esta activo, esas tres salidas se capturan como una PETICION que DeX
// atiende luego a su manera: cerrar la ventana, abrir otra ventana, o abrir
// Recientes de DeX. Es el unico punto donde el sistema y el hosting se tocan.
static bool gHosted    = false;
static int  gHostReq   = 0;      // 0 nada · 1 cerrar ventana · 2 abrir app · 3 recientes
static int  gHostReqApp = -1;
static void settingsEnter(); static void settingsTick();   // Ajustes (M3), abajo
// Navegacion interna de Ajustes: devuelve true si "atras" tenia una pantalla
// de categoria que cerrar (y la cierra). Lo consulta appTick para que el boton
// atras del sistema no salte directo al escritorio desde una subpantalla.
static bool settingsHandleBack();
static void wifiSettingsEnter(); static void wifiTick();    // Ajustes -> Red e Internet -> Wi-Fi, abajo
static void calcEnter(); static void calcTick();           // Calculadora (M2), abajo
static void pcEnter(); static void pcTick();               // Modo PC (M4), abajo
static void galEnter();                                    // Galeria (M2)
static void bienEnter(); static void bienTick();           // Bienestar (M2)
static void calEnter(); static void calTick();             // Calendario (M2)
static void vidEnter(); static void vidTick();             // Multimedia (esqueleto)
static void camEnter(); static void camTick();             // Camara (esqueleto)
static void noteEnter(); static void noteTick();           // Notas + teclado 4 capas
static void almEnter(); static void eduEnter(); static void navEnter();  // apps simples
static void ideEnter(); static void ideTick(); static void paintEnter(); static void paintTick();
static void almTick();                                     // Almacenamiento: tap en "Ver..."
static void filesTick();                                   // Explorador de archivos (ST_FILES)
static void connEnter(); static void connTick();           // Conectividad (ST_CONN)
static void flexBleStop(); static bool flexBleStart();     // radio BLE real
static void connWifiSub(char* out, size_t n);              // SSID real para Ajustes
static void connBleSub(char* out, size_t n);
static void geoEnter(); static void geoTick();   // Juegos -> Geo Dash (clon de Geometry Dash)

// Rect del icono en el escritorio (para animar la apertura desde el)
static void getIconRect(int id, int &rx, int &ry, int &rs){
  if(id < 12){
    // La rejilla se dibuja por SLOT (homeOrder[slot] = id de app), no por id.
    // Antes esto calculaba la casilla con id%4 e id/4, o sea daba por hecho que
    // cada app sigue en su casilla original. En cuanto se reordenaban los iconos
    // en Modo Edicion, la animacion de apertura crecia desde donde ESTABA la app
    // antes: mover Notas al hueco de la Calculadora hacia que Notas se abriera
    // desde el sitio de la Calculadora. Hay que buscar en que slot esta hoy.
    int slot = id;
    for(int s = 0; s < 12; s++) if(homeOrder[s] == id){ slot = s; break; }
    int S = 72, gx0 = 24, gy0 = 212, rowStep = 112;
    rx = gx0 + (slot % 4) * 120; ry = gy0 + (slot / 4) * rowStep; rs = S;
  } else {
    int dkx = 24, dky = SCR_H - 176, dkw = SCR_W - 48, dkh = 96, dS = 64;
    int inner = dkw - 32, dgap = (inner - 4 * dS) / 3, i = id - 12;
    rx = dkx + 16 + i * (dS + dgap); ry = dky + (dkh - dS) / 2; rs = dS;
  }
}

// Marco estandar de ventana (barra de estado + cabecera + nav bar)
static void appDrawChrome(int id){
  if(gHosted){ (void)id; return; }   // embebida: la ventana ya tiene su barra de titulo
  setBuf(fb);
  uint16_t W = rgb565(255,255,255);
  char cs[12]; clkStrBar(cs, sizeof(cs));
  drawText(20, 16, cs, 2, W);
  drawWifi(SCR_W - 66, 28, 11, W);
  drawBattery(SCR_W - 46, 20, 30, 15, 82, W);
  if(gNavMode == 0){
    int ny = SCR_H - 52;
    fillTriangle(SCR_W / 6 - 10, ny + 8, SCR_W / 6 + 8, ny - 2, SCR_W / 6 + 8, ny + 18, W);
    drawCircle(SCR_W / 2, ny + 8, 12, W); drawCircle(SCR_W / 2, ny + 8, 11, W);
    drawRoundRect(SCR_W * 5 / 6 - 11, ny - 3, 22, 22, 4, W);
  } else {
    drawHomeIndicator(SCR_H, 180);
  }
  (void)id;
}
// Cabecera estandar (chevron "atras" + titulo centrado). Las apps con
// APP_CUSTOM_HEADER se saltan esto y pintan su propia cabecera.
static void appDrawHeader(int id){
  if(gHosted) return;                // el nombre de la app lo pone la barra de titulo
  uint16_t W = rgb565(255,255,255);
  int hy = 50;
  strokeSegAA(30, hy + 16, 18, hy + 8, 2.4f, W);
  strokeSegAA(18, hy + 8, 30, hy, 2.4f, W);
  drawTextC(SCR_W / 2, hy + 3, appName(id), 3, W);
}


// #############################################################
// ##  LAYOUT RESPONSIVO  ·  toolkit compartido por las apps
// #############################################################
// Toda app APP_FLEX maqueta contra el LIENZO REAL: gAppW de ancho y
// [WIN_TOP..WIN_BOT] de alto. A pantalla completa eso es la pantalla menos su
// chrome; dentro de una ventana de DeX es EXACTAMENTE el area de cliente, y se
// dibuja 1:1. Ahi esta la diferencia con el camino antiguo: nada se dibuja a
// 480x800 para luego remuestrearlo al tamano de la ventana, que es lo que
// dejaba el texto emborronado ("como una imagen ampliada"). Al maquetar al
// tamano final, la fuente vectorial se rasteriza a su tamano real y sale
// nitida en cualquier ventana.

static void uiBox(int &x, int &y, int &w, int &h){
  x = 0; y = WIN_TOP; w = gAppW; h = WIN_BOT - WIN_TOP;
  if(w < 32) w = 32;
  if(h < 32) h = 32;
}
static inline int uiW(){ int x, y, w, h; uiBox(x, y, w, h); return w; }
static inline int uiH(){ int x, y, w, h; uiBox(x, y, w, h); return h; }
static inline int uiTop(){ int x, y, w, h; uiBox(x, y, w, h); return y; }
// Margen y separacion proporcionales, acotados para que no se coman la caja.
static int uiPad(){
  int w = uiW(), h = uiH(), m = (w < h ? w : h) / 24;
  if(m < 5) m = 5;
  if(m > 22) m = 22;
  return m;
}
static inline int uiGap(){ int g = uiPad() * 3 / 4; return g < 4 ? 4 : g; }
// Tamano de fuente mas grande que cabe en `maxw`, sin pasar de `maxSize`.
static int uiFontFit(const char* t, int maxw, int maxSize){
  int fs = maxSize; if(fs > 5) fs = 5; if(fs < 1) fs = 1;
  while(fs > 1 && textW(t, fs) > maxw) fs--;
  return fs;
}
// Tamano de fuente adecuado a una altura de linea.
static int uiFontH(int lineH){
  if(lineH >= 44) return 5;
  if(lineH >= 33) return 4;
  if(lineH >= 23) return 3;
  if(lineH >= 14) return 2;
  return 1;
}
// Altura aproximada de una linea de texto de tamano fs (para reservar sitio).
static inline int uiLineH(int fs){ return fs <= 1 ? 8 : fs * 9; }
// Titulo de seccion: se dibuja solo si cabe, y devuelve la Y siguiente.
static int uiTitle(int x, int y, int w, const char* t, uint16_t col, int maxSize){
  int fs = uiFontFit(t, w, maxSize);
  drawTextC(x + w / 2, y, t, fs, col);
  return y + uiLineH(fs) + uiGap() / 2;
}

// ---- Secciones opcionales con breakpoint y fundido ----
// Una seccion opcional aparece ENTERA cuando el lienzo da de si y desaparece
// ENTERA por debajo del umbral: nunca a medias ni interpolada de tamano. El
// umbral lo decide cada app segun lo que su seccion necesita para verse bien,
// no un numero arbitrario. Al cruzarlo en vivo (arrastrando el borde) se
// aplica un fundido corto para que no salte de golpe.
#define UI_FADE_MS 130
#define UI_SEC_MAX 12
struct UiSec { uint8_t app; uint8_t id; bool on; uint32_t t0; };
static UiSec  uiSecs[UI_SEC_MAX];
static uint8_t uiSecN = 0;
static bool    uiFading = false;      // hay algun fundido en curso (lo consulta DeX)

// Devuelve el alfa 0..255 con el que dibujar la seccion `id` de la app en
// curso. 0 = no dibujarla. `want` es el breakpoint ya evaluado por la app.
static uint8_t uiSection(uint8_t id, bool want){
  uint8_t app = (uint8_t)gAppId;
  UiSec* sc = NULL;
  for(uint8_t i = 0; i < uiSecN; i++) if(uiSecs[i].app == app && uiSecs[i].id == id){ sc = &uiSecs[i]; break; }
  if(!sc){
    if(uiSecN >= UI_SEC_MAX) return want ? 255 : 0;    // sin ranura: sin fundido, pero correcto
    sc = &uiSecs[uiSecN++];
    sc->app = app; sc->id = id; sc->on = want; sc->t0 = 0;
    return want ? 255 : 0;
  }
  if(want != sc->on){ sc->on = want; sc->t0 = millis(); }
  if(sc->t0 == 0) return sc->on ? 255 : 0;
  uint32_t e = millis() - sc->t0;
  if(e >= UI_FADE_MS){ sc->t0 = 0; return sc->on ? 255 : 0; }
  uiFading = true;
  uint32_t a = (uint32_t)255 * e / UI_FADE_MS;
  return (uint8_t)(sc->on ? a : 255 - a);
}
// Texto y relleno con alfa, para que las secciones opcionales puedan fundirse.
static inline void uiRectA(int x, int y, int w, int h, int r, uint16_t c, uint8_t a){
  if(a == 0) return;
  if(a >= 255) fillRoundRect(x, y, w, h, r, c);
  else fillRoundRectA(x, y, w, h, r, c, a);
}
static inline void uiText(int x, int y, const char* t, int fs, uint16_t c, uint8_t a){
  if(a == 0) return;
  drawTextA(x, y, t, fs, c, a);
}
static inline void uiTextC(int cx, int y, const char* t, int fs, uint16_t c, uint8_t a){
  if(a == 0) return;
  drawTextCA(cx, y, t, fs, c, a);
}
static inline void uiTextR(int rx, int y, const char* t, int fs, uint16_t c, uint8_t a){
  if(a == 0) return;
  drawTextA(rx - textW(t, fs), y, t, fs, c, a);
}

// ---- Contenido de apps ----
// (1) Placeholder para apps aun no implementadas (dentro de la ventana)
static void appPlaceholderEnter(){
  setBuf(fb);
  int cy = (WIN_TOP + WIN_BOT) / 2;
  if(uiGlass) drawLiquidGlassPanel(36, cy - 168, SCR_W - 72, 268, 26, rgb565(50,72,146));  // modal glass
  drawAppIcon(gAppId, SCR_W / 2 - 44, cy - 130, 88);
  drawTextC(SCR_W / 2, cy + 6, t(S_SOON), 3, rgb565(232,234,240));
  drawTextC(SCR_W / 2, cy + 48, t(S_M2), 2, rgb565(140,150,166));
}
// (2) App REAL de referencia: Reloj (prueba el patron completo)
// RELOJ · adaptativo.
//   Esencial   : reloj gigante, centrado, con el trazo escalado al lienzo.
//   Opcional 1 : fecha larga -- aparece cuando quedan >= 26 px bajo el reloj.
//   Opcional 2 : tarjetas de fecha corta / dia del ano -- aparecen cuando el
//                lienzo tiene >= 250 px de ancho Y >= 90 px libres debajo, que
//                es lo que necesitan para no quedar apretadas.
static void appRelojRender(){
  setBuf(fb);
  int bx, by, bw, bh; uiBox(bx, by, bw, bh);
  fillRect(bx, by, bw, bh, WIN_BG);
  int pad = uiPad();
  char cs[8]; clkStr12(cs, sizeof(cs));
  // El reloj se lleva ~40% del alto, y nunca mas ancho de lo que cabe.
  int capH = bh * 2 / 5;
  int maxByW = (bw - 2 * pad) * 10 / (int)(strlen(cs) * 7 + 2);
  if(capH > maxByW) capH = maxByW;
  if(capH > 150) capH = 150;
  if(capH < 22) capH = 22;
  int thick = capH / 8; if(thick < 3) thick = 3;
  int cy = by + pad + bh / 12;
  drawBigClock(cs, bx + bw / 2, cy, capH, thick, rgb565(255,255,255));
  int y = cy + capH + pad;
  int rest = (by + bh) - y - pad;
  char ds[64]; buildLongDate(ds, sizeof(ds));
  uint8_t aDate = uiSection(0, rest >= 26);
  if(aDate){
    int fs = uiFontFit(ds, bw - 2 * pad, uiFontH(rest / 3 > 30 ? 30 : rest));
    uiTextC(bx + bw / 2, y, ds, fs, rgb565(200,210,230), aDate);
    y += uiLineH(fs) + pad;
    rest = (by + bh) - y - pad;
  }
  // Panel opcional de tarjetas: solo con ancho y alto suficientes.
  uint8_t aCards = uiSection(1, bw >= 250 && rest >= 90);
  if(aCards){
    int n = 2, g = uiGap();
    int cw = (bw - 2 * pad - g) / n, chh = rest > 130 ? 130 : rest;
    char sd[40]; buildShortDate(sd, sizeof(sd));
    char doy[24]; snprintf(doy, sizeof(doy), "%s", g24h ? "24 h" : "12 h");
    const char* lbl[2] = { "Fecha", "Formato" };
    const char* val[2] = { sd, doy };
    for(int i = 0; i < n; i++){
      int x = bx + pad + i * (cw + g);
      uiRectA(x, y, cw, chh, uiPad(), rgb565(30,34,46), aCards);
      int fl = uiFontFit(lbl[i], cw - 16, 2);
      uiTextC(x + cw / 2, y + chh / 2 - uiLineH(fl) - 6, lbl[i], fl, rgb565(150,160,185), aCards);
      int fv = uiFontFit(val[i], cw - 16, 3);
      uiTextC(x + cw / 2, y + chh / 2 + 2, val[i], fv, rgb565(225,232,245), aCards);
    }
    y += chh + pad;
  }
  int fy = by + bh - pad - uiLineH(2);
  if(fy > y) drawTextC(bx + bw / 2, fy, "Reloj de FlexOS", uiFontFit("Reloj de FlexOS", bw - 2 * pad, 2), rgb565(120,132,152));
  flxFlush(WIN_TOP, WIN_BOT);
}
static void appRelojEnter(){ appRelojRender(); }
static void appRelojTick(){ if(gMinChanged) appRelojRender(); }

// ---- Registro de apps (indices = enum IC_*) ----
static FlexApp APP_REG[16] = {
  { appRelojEnter, appRelojTick, APP_FLEX },              // 0  Reloj  (REAL)
  { galEnter, NULL, APP_FLEX },                           // 1  Galeria (REAL, M2)
  { vidEnter, vidTick, APP_CUSTOM_HEADER | APP_OWN_TOUCH },  // 2  Multimedia (esqueleto)
  { almEnter, almTick, APP_FLEX },                        // 3  Almacenamiento (REAL: LittleFS + PSRAM)
  { pcEnter, pcTick, APP_CUSTOM_HEADER },                  // 4  Modo PC (REAL, M4) -- usa render landscape (gLand)
  { noteEnter, noteTick, APP_CUSTOM_HEADER | APP_OWN_TOUCH },// 5  Notas + teclado (REAL)
  { eduEnter, NULL, APP_FLEX },                           // 6  Educacion (REAL)
  { navEnter, NULL, APP_FLEX },                           // 7  Navegador (REAL)
  { ideEnter, ideTick, APP_FLEX },                        // 8  Code IDE (REAL + Asistente de Hardware)
  { bienEnter, bienTick, APP_FLEX },                      // 9  Bienestar (REAL, M2)
  { paintEnter, paintTick, APP_CUSTOM_HEADER | APP_OWN_TOUCH },// 10 Paint (REAL)
  { geoEnter, geoTick, APP_OWN_TOUCH | APP_CUSTOM_HEADER | APP_LAND }, // 11 Juegos (Geo Dash, REAL)
  { settingsEnter, settingsTick, APP_CUSTOM_HEADER },      // 12 Ajustes (REAL, M3) -- Wi-Fi/PIN cambian gState a pantalla completa
  { calcEnter, calcTick, APP_FLEX },               // 13 Calculadora (REAL, M2) -- app de referencia del modo embebido
  { calEnter, calTick, APP_FLEX },                        // 14 Calendario (REAL, M2)
  { camEnter, camTick, APP_CUSTOM_HEADER | APP_OWN_TOUCH },  // 15 Camara (esqueleto)
};

// Animacion de apertura/cierre: la ventana crece/encoge desde el icono
#define WIN_ANIM_MS 100                              // 0.1 s exactos, basado en tiempo
static void winRevealAnim(int id, bool opening){
  int ix, iy, is; getIconRect(id, ix, iy, is);
  uint16_t bg = (APP_REG[id].flags & APP_CUSTOM_HEADER) ? rgb565(244,247,251) : WIN_BG;
  uint32_t t0 = millis(), dur = WIN_ANIM_MS;
  // FLUIDEZ: antes cada frame recopiaba homeBuf ENTERO (768 KB de PSRAM leidos y
  // escritos, ~10 ms) y volcaba la pantalla completa, aunque la forma solo
  // ocupara una franja. A 0,2 s eso daba unos 20 frames; a 0,1 s habrian sido 10
  // y se veria a saltos. Ahora se recompone y se vuelca SOLO la union de la
  // franja del frame anterior y la de este -- lo unico que puede haber cambiado.
  // Los primeros frames del zoom son un rectangulo pequeno junto al icono y
  // cuestan casi nada, asi que el numero de frames sube mucho: la animacion es
  // mas corta Y mas suave a la vez. Al no haber delay(), el bucle va al maximo
  // que de el compositor.
  int prevY0 = 0, prevY1 = SCR_H - 1;
  bool first = true;
  for(;;){
    uint32_t e = millis() - t0; if(e > dur) e = dur;
    float tt = (float)e / dur;
    float p = opening ? tt : (1.0f - tt);
    p = 1 - (1 - p) * (1 - p) * (1 - p);              // ease-out cubico (mas suave)
    // Geometria de la forma de este frame y la franja vertical que ocupa.
    int x0 = 0, y0 = 0, x1 = SCR_W, y1 = SCR_H, rad = 0;
    if(gAnimStyle == 1){                               // fundido: cubre siempre toda la pantalla
      y0 = 0; y1 = SCR_H;
    } else if(gAnimStyle == 2){                        // deslizar: sube desde el borde inferior
      y0 = (int)(SCR_H * (1 - p)); y1 = SCR_H;
    } else {                                           // zoom: crece desde el icono
      x0  = (int)(ix * (1 - p));
      y0  = (int)(iy * (1 - p));
      x1  = (int)((ix + is) * (1 - p) + SCR_W * p);
      y1  = (int)((iy + is) * (1 - p) + SCR_H * p);
      rad = (int)(18 * (1 - p));
    }
    int by0 = y0 < prevY0 ? y0 : prevY0;
    int by1 = y1 > prevY1 ? y1 : prevY1;
    if(first){ by0 = 0; by1 = SCR_H - 1; first = false; }
    if(by0 < 0) by0 = 0; if(by1 > SCR_H - 1) by1 = SCR_H - 1;
    setBuf(bbuf);
    for(int j = by0; j <= by1; j++)                    // fondo: solo las filas que se tocan
      memcpy(bbuf + (size_t)j * SCR_W, homeBuf + (size_t)j * SCR_W, SCR_W * 2);
    if(gAnimStyle == 1)      fillRectA(0, 0, SCR_W, SCR_H, bg, (uint8_t)(255 * p));
    else if(gAnimStyle == 2) fillRect(0, y0, SCR_W, SCR_H - y0, bg);
    else                     fillRoundRect(x0, y0, x1 - x0, y1 - y0, rad, bg);
    present(by0, by1);                                 // vuelca de una vez (sin parpadeo)
    prevY0 = y0; prevY1 = y1;
    if(e >= dur) break;
  }
}

static void appClose(){
  // FASE 4: un unico candado cierra TODAS las salidas de la app -- boton atras,
  // chevron de la cabecera, gesto rapido de la barra iOS, y cualquier app que
  // llame a appClose desde su propio tick. Poniendolo aqui no hay que ir
  // parcheando cada camino por separado (y ninguno nuevo se escapa).
  if(KIOSK_ON && kioskOn) return;
  if(gHosted){ gHostReq = 1; return; }        // dentro de una ventana: cierra la VENTANA
  // El FRAMEWORK devuelve el motor a portrait, no la app. Antes cada app
  // landscape tenia que acordarse de hacer gLand=false por su cuenta (pcExit,
  // gamesExitApp); si se salia por cualquier otra via -- gesto de la barra,
  // boton de atras, o una app que llamara a appClose desde dentro de su propio
  // tick -- gLand se quedaba en true y TODO lo que se pintara despues pasaba por
  // putPhys: el escritorio salia girado 90 y recortado (ly solo llega a 479, asi
  // que la mitad inferior desaparecia), y de ahi ya no se recuperaba sin
  // reiniciar. Resetear aqui cierra esa clase entera de fallo de una vez.
  bool wasLand = gLand;
  gLand = false;
  gClipY0 = 0; gClipY1 = SCR_H - 1;
  gClipX0 = 0; gClipX1 = SCR_W - 1;
  setBuf(fb);
  if(wasLand) swPushNoThumb(gAppId);   // miniatura girada: mejor ninguna (ver swPushNoThumb)
  else        swPushAndCapture(gAppId);
  // winRevealAnim compone la animacion SOBRE homeBuf: si Ajustes lo dejo sucio,
  // hay que recomponerlo ANTES, o la animacion de cierre encoge hacia el
  // escritorio viejo y este cambia de golpe al terminar.
  if(gHomeDirty) renderHome();
  winRevealAnim(gAppId, false);
  enterHome();
}
static void enterApp(int id){
  // FASE 4: en kiosco solo se puede estar en la app clavada. Esto bloquea que
  // una app abra otra a pantalla completa y deje kioskApp apuntando a otro sitio.
  if(KIOSK_ON && kioskOn && id != kioskApp) return;
  if(gHosted){ gHostReq = 2; gHostReqApp = id; return; }   // -> otra ventana de DeX
  gAppId = id; gState = ST_APP;
  winRevealAnim(id, true);                        // crece desde el icono
  if(!(APP_REG[id].flags & APP_CUSTOM_HEADER)){   // apps normales: marco blanco
    appDrawChrome(id);
    appDrawHeader(id);
  }
  if(APP_REG[id].enter) APP_REG[id].enter();      // la app pinta su contenido (y su marco si es custom)
  flxFlushAll();
}
static void appTick(){
  if(gLand){ if(APP_REG[gAppId].tick) APP_REG[gAppId].tick(); return; }  // Modo PC: gestiona todo por su cuenta
  if(gNavMode == 1 && handleiOSGestures()) return;   // gestos iOS: swipe-arriba -> Home/multitarea
  // Cierre universal: tocar "atras" (nav; y cabecera en apps normales). Gesto swipe-to-close eliminado.
  bool back = false;  // antes: T.swipeRight -> deshabilitado a peticion, ya no cierra la app
  if(T.tap){
    int ny = SCR_H - 52;
    if(!(APP_REG[gAppId].flags & APP_OWN_TOUCH) && T.y >= ny - 10 && T.y <= ny + 22 && T.x < SCR_W / 3) back = true;               // nav atras
    if(!(APP_REG[gAppId].flags & APP_CUSTOM_HEADER) && T.y <= WIN_TOP && T.x < 72) back = true; // chevron
  }
  if(back){
    // Ajustes tiene navegacion propia (lista -> categoria): "atras" cierra
    // primero la pantalla de categoria y solo cierra la app desde la lista.
    if(gAppId == IC_AJUSTES && settingsHandleBack()) return;
    appClose(); return;
  }
  if(APP_REG[gAppId].tick) APP_REG[gAppId].tick();
}

static void enterHome(){
  gState = ST_HOME; lockOff = 0; lastLockOff = -1;
  // Antes se volcaba homeBuf tal cual. Si venias de Ajustes de cambiar idioma,
  // formato de hora, Liquid Glass o estilo de iconos, homeBuf seguia siendo el
  // ANTERIOR y el escritorio contradecia al ajuste que acababas de tocar,
  // hasta que el reloj cambiaba de minuto y lo repintaba por su cuenta.
  if(gHomeDirty) renderHome();
  blitToFb(homeBuf); flxFlushAll();
}

// Gestos de la barra inferior (modo iOS). Solo actua si el toque EMPEZO en los
// ultimos ~44 px de la pantalla (la zona de la barra). Al soltar:
//   · deslizamiento hacia arriba rapido (<300 ms) -> Home
//   · deslizamiento hacia arriba mantenido (>=300 ms) -> App Switcher
// Devuelve true si consumio el gesto (para que el tick no siga procesando).
static bool handleiOSGestures(){
  if(gNavMode != 1) return false;
  // FASE 4: en kiosco los gestos de la barra (Home y switcher) se ignoran. Se
  // devuelve false, no true: asi appTick sigue llamando al tick de la app y esta
  // no se congela -- solo pierde la via de escape.
  if(KIOSK_ON && kioskOn) return false;
  if(T.released && T.startY > SCR_H - 44){
    int dy = T.startY - T.y;                    // positivo si el dedo subio
    unsigned long dur = millis() - T.downMs;
    if(dy > 30){
      if(dur >= 300)            activarMultitarea();  // mantener -> multitarea
      else if(gState == ST_APP) appClose();           // rapido en app -> Home (guarda miniatura)
      else                      enterHome();          // rapido en Home -> refresca
      return true;
    }
  }
  return false;
}

// #############################################################
// ##  APP AJUSTES  ·  navegacion de telefono moderno
// ##  ------------------------------------------------------
// ##  Antes eran DOS paneles a la vez: barra lateral de categorias a la
// ##  izquierda y detalle a la derecha, con el detalle encajado en 232 px de
// ##  ancho. Ahora funciona como en Android/One UI/iOS:
// ##    · la pantalla principal es SOLO la lista de categorias, a todo lo ancho;
// ##    · tocar una categoria ABRE su propia pantalla, dedicada, tambien a todo
// ##      lo ancho (mas sitio para cada fila y para su valor);
// ##    · la transicion es un empuje horizontal con paralaje (la pantalla que
// ##      sale se va mas despacio que la que entra), interpolado por TIEMPO;
// ##    · para volver: el chevron de la cabecera, el boton "atras" de la barra
// ##      de navegacion o un arrastre desde el borde izquierdo.
// ##  COMPATIBILIDAD CON DeX: la app sigue siendo la MISMA -- mismo enter(),
// ##  mismo tick(), mismo lienzo 480x800, y la navegacion vive en una variable
// ##  propia (setView), no en gState. Por eso dentro de una ventana de Modo PC
// ##  se comporta exactamente igual que a pantalla completa. Lo unico que se
// ##  omite hospedada es la ANIMACION (un tick hospedado solo produce el ultimo
// ##  cuadro: animar ahi seria trabajo tirado), que es justo lo que hacen ya
// ##  las demas animaciones bloqueantes del sistema.
// #############################################################
#define SET_CARD_X   14
#define SET_CARD_W   (SCR_W - 28)           // tarjeta a todo el ancho (452)
#define DP_X         SET_CARD_X             // el contenido de detalle usa DP_X/DP_W
#define DP_W         SET_CARD_W
#define SET_LIST_TOP 104                    // primera tarjeta de la lista de categorias
#define SET_LIST_BOT (SCR_H - 70)
#define DLIST_TOP    128                    // primera fila de la pantalla de categoria
#define DLIST_BOT    (SCR_H - 70)           // 730
#define SET_NAV_MS   270                    // duracion de la transicion entre pantallas
#define SET_ANIM_Y0  40                     // banda que se desplaza (la barra de estado
#define SET_ANIM_Y1  (SCR_H - 60)           //  y la de navegacion se quedan quietas)
// PAGE_BG y la paleta de abajo dependen de gDark (Ajustes -> Pantalla ->
// Modo de apariencia). Colores de MARCA/acento (los puntitos de colores de
// cada fila, iconos como Wi-Fi o el reloj) se dejan igual en ambos modos a
// proposito -- es el fondo y el texto lo que define si algo "se ve" claro
// u oscuro, y es lo unico que cambia aqui.
#define PAGE_BG        (gDark ? rgb565(18,20,28)     : rgb565(244,247,251))   // fondo de pagina
#define SET_CARD_BG    (gDark ? rgb565(34,38,50)     : rgb565(255,255,255))   // fondo de tarjeta (fila/sidebar)
#define SET_CARD_GLASS (gDark ? rgb565(48,54,72)     : rgb565(246,248,252))   // tinte de vidrio de la tarjeta
#define SET_TXT_HI     (gDark ? rgb565(240,242,248)  : rgb565(20,22,30))      // texto principal
#define SET_TXT_LO     (gDark ? rgb565(160,166,182)  : rgb565(120,126,140))   // texto secundario / valor
#define SET_TXT_MUTE   (gDark ? rgb565(120,126,142)  : rgb565(140,146,160))   // texto de ayuda / pie
#define SET_CHEV       (gDark ? rgb565(110,116,132)  : rgb565(160,165,178))   // chevron
#define SET_SIDE_SUB   (gDark ? rgb565(150,156,172)  : rgb565(140,145,158))   // subtitulo de la barra lateral
#define SET_NAVPILL    (gDark ? rgb565(200,204,214)  : rgb565(60,64,74))      // pildora de gestos (modo iOS)

static const char* SET_CAT[12] = {
  "General","Pantalla","Sonido","Red e Internet","Dispositivos",
  "Personalizaci\xC3\xB3n","Seguridad","Bater\xC3\xAD" "a","Almacenamiento",
  "Desarrollador","Sistema","Acerca de" };
static const char* SET_SUB[12] = {
  "Idioma, fecha, hora","Brillo, fondo, tema","Volumen, tonos","WiFi, Bluetooth",
  "GPIO, perifericos","Temas, iconos","Bloqueo, permisos","Ahorro de energia",
  "Interna, SD","Opciones dev","Sistema, logs","Version, creditos" };
static const char* SET_DESC[12] = {
  "Configura las opciones basicas del sistema.","Brillo, fondo de pantalla y modo oscuro.",
  "Volumen, tonos y notificaciones.","Conexiones de red (offline por ahora).",
  "GPIO, modulos y perifericos.","Temas, iconos y estilo del sistema.",
  "Bloqueo, permisos y privacidad.","Estado de la bateria y ahorro de energia.",
  "Memoria interna y tarjeta SD.","Herramientas y diagnostico de desarrollo.",
  "Informacion del sistema y registros.","Version, hardware y creditos de FlexOS." };

// setView es TODA la maquina de navegacion de la app: 0 = lista de categorias,
// 1 = pantalla de una categoria. A proposito NO es un gState nuevo -- asi la
// navegacion interna sobrevive intacta dentro de una ventana de Modo PC, donde
// dexHostRun descarta cualquier cambio de gState que haga la app hospedada.
static int setView = 0;
static int setSel = 0, setScroll = 0, setContentH = 0;
static int setListScroll = 0, setListH = 0;    // scroll propio de la lista de categorias
static int  setDragY0 = 0, setDragS0 = 0;      // arrastre de la lista activa
static bool setDragging = false;
static bool setBackSwipe = false;              // arrastre desde el borde izquierdo (volver)

// Texto recortado por la derecha (evita que se salga del panel)
static int drawTextClip(int x, int y, const char* s, int size, uint16_t col, int maxRight){
  if(size <= 1){
    while(*s){
      if(x + 6 > maxRight) break;
      uint8_t b = (uint8_t)*s++; uint32_t cp;
      if(b < 0x80) cp = b;
      else if((b & 0xE0) == 0xC0){ uint8_t b1 = *s ? (uint8_t)*s++ : 0; cp = ((b & 0x1F) << 6) | (b1 & 0x3F); }
      else if((b & 0xF0) == 0xE0){ if(*s) s++; if(*s) s++; cp = 0x3F; }
      else cp = 0x3F;
      uint8_t base, acc; mapCP(cp, base, acc);
      drawGlyphSmooth(x, y, base, 1, col, 255);
      if(acc) drawAccent(x, y, 1, acc, col);
      x += 6;
    }
    return x;
  }
  float sc = fontSc(size), penx = x;
  while(*s){
    uint32_t cp = nextCP(&s);
    const FGlyph* g = &FG[fontIdx(cp)];
    if(penx + g->adv * sc > maxRight) break;
    drawGlyphScaled((int)(penx + g->bx * sc + 0.5f), y + (int)((g->topoff - FONT_CAPOFF) * sc + 0.5f), g, sc, col, 255);
    penx += g->adv * sc;
  }
  return (int)(penx + 0.5f);
}

static const char* langNameCur(){ return (cfgLang == 5) ? "Chinese" : LANG_ENDONYM[cfgLang]; }
static void settingsDateTimeStr(char* out, size_t n){
  int h12 = rtcH % 12; if(h12 == 0) h12 = 12;
  snprintf(out, n, "%02d/%02d/%04d  %d:%02d %s", rtcD, rtcMo, rtcY, h12, rtcMin, rtcH < 12 ? "AM" : "PM");
}
static void buildUptime(char* out, size_t n){
  unsigned long s = millis() / 1000UL;
  unsigned long d = s / 86400; s %= 86400;
  unsigned long h = s / 3600;  s %= 3600;
  unsigned long m = s / 60;
  if(d > 0)      snprintf(out, n, "%lud %luh %lum", d, h, m);
  else if(h > 0) snprintf(out, n, "%luh %lum", h, m);
  else           snprintf(out, n, "%lum", m);
}

// ---- iconos de fila (panel General) ----
enum { RI_GLOBE, RI_CAL, RI_CLOCK, RI_PIN, RI_REFRESH, RI_CLOUD, RI_RESET, RI_DOT };
static void drawRowGlyph(int k, int cx, int cy, uint16_t col){
  switch(k){
    case RI_GLOBE:
      drawCircle(cx, cy, 10, col); vLine(cx, cy - 10, 21, col); hLine(cx - 10, cy, 21, col);
      arcStroke(cx, cy, 5, 90, 270, 1, col); arcStroke(cx, cy, 5, -90, 90, 1, col); break;
    case RI_CAL:
      drawRoundRect(cx - 9, cy - 8, 18, 17, 3, col); fillRect(cx - 9, cy - 8, 18, 5, col);
      fillCircle(cx - 4, cy + 2, 1, col); fillCircle(cx + 3, cy + 2, 1, col); break;
    case RI_CLOCK:
      drawCircle(cx, cy, 10, col); strokeSegAA(cx, cy, cx, cy - 6, 1.4f, col);
      strokeSegAA(cx, cy, cx + 4, cy, 1.4f, col); break;
    case RI_PIN:
      fillCircle(cx, cy - 3, 7, col); fillTriangle(cx - 6, cy, cx + 6, cy, cx, cy + 9, col);
      fillCircle(cx, cy - 3, 3, rgb565(255,255,255)); break;
    case RI_REFRESH:
      arcStroke(cx, cy, 9, 30, 300, 2, col);
      fillTriangle(cx + 8, cy - 7, cx + 14, cy - 4, cx + 7, cy - 1, col); break;
    case RI_CLOUD:
      fillCircle(cx - 4, cy + 2, 5, col); fillCircle(cx + 4, cy + 2, 6, col);
      fillCircle(cx, cy - 2, 6, col); fillRect(cx - 8, cy + 2, 16, 5, col); break;
    case RI_RESET:
      arcStroke(cx, cy, 9, 40, 320, 2, col);
      fillTriangle(cx + 6, cy - 8, cx + 12, cy - 9, cx + 8, cy - 2, col); break;
    default: fillCircle(cx, cy, 4, col); break;
  }
}

// ---- iconos de categoria (barra lateral) ----
static void drawSetCatIcon(int cat, int x, int y, int S, uint16_t col){
  int cx = x + S / 2, cy = y + S / 2;
  switch(cat){
    case 0: // engranaje
      fillCircleAA(cx, cy, S * 0.18f, col);
      for(int k = 0; k < 8; k++){ float a = k * 0.7853982f;
        fillCircleAA(cx + cosf(a) * S * 0.30f, cy + sinf(a) * S * 0.30f, S * 0.065f, col); }
      fillCircleAA(cx, cy, S * 0.08f, rgb565(255,255,255)); break;
    case 1: // sol
      fillCircleAA(cx, cy, S * 0.15f, col);
      for(int k = 0; k < 8; k++){ float a = k * 0.7853982f;
        strokeSegAA(cx + cosf(a) * S * 0.24f, cy + sinf(a) * S * 0.24f,
                    cx + cosf(a) * S * 0.34f, cy + sinf(a) * S * 0.34f, 1.4f, col); } break;
    case 2: // altavoz
      fillRect((int)(cx - S * 0.22f), (int)(cy - S * 0.06f), (int)(S * 0.10f), (int)(S * 0.12f), col);
      fillTriangle((int)(cx - S * 0.12f), (int)(cy - S * 0.14f), (int)(cx - S * 0.12f), (int)(cy + S * 0.14f), (int)(cx + S * 0.02f), cy, col);
      arcStroke(cx - S * 0.02f, cy, S * 0.14f, -55, 55, 2, col); break;
    case 3: drawWifi(cx, (int)(cy + S * 0.14f), (int)(S * 0.28f), col); break;
    case 4: // cubo
      fillQuad(cx, (int)(cy - S * 0.22f), (int)(cx + S * 0.20f), (int)(cy - S * 0.10f), cx, (int)(cy + S * 0.02f), (int)(cx - S * 0.20f), (int)(cy - S * 0.10f), col);
      fillQuad(cx, (int)(cy + S * 0.02f), (int)(cx + S * 0.20f), (int)(cy - S * 0.10f), (int)(cx + S * 0.20f), (int)(cy + S * 0.14f), cx, (int)(cy + S * 0.26f), mix565(col, rgb565(0,0,0), 70));
      fillQuad(cx, (int)(cy + S * 0.02f), (int)(cx - S * 0.20f), (int)(cy - S * 0.10f), (int)(cx - S * 0.20f), (int)(cy + S * 0.14f), cx, (int)(cy + S * 0.26f), mix565(col, rgb565(0,0,0), 120)); break;
    case 5: // pincel
      strokeSegAA(cx - S * 0.16f, cy + S * 0.18f, cx + S * 0.10f, cy - S * 0.16f, 2.4f, col);
      fillCircleAA(cx - S * 0.18f, cy + S * 0.20f, S * 0.08f, col); break;
    case 6: // candado
      fillRoundRect((int)(cx - S * 0.16f), (int)(cy - S * 0.02f), (int)(S * 0.32f), (int)(S * 0.24f), 3, col);
      arcStroke(cx, cy - S * 0.02f, S * 0.12f, 180, 360, 2, col); break;
    case 7: drawBattery((int)(x + S * 0.24f), (int)(y + S * 0.34f), (int)(S * 0.5f), (int)(S * 0.3f), 80, col); break;
    case 8: // discos apilados
      for(int i = 0; i < 3; i++)
        fillRoundRect((int)(cx - S * 0.22f), (int)(cy - S * 0.16f + i * S * 0.14f), (int)(S * 0.44f), (int)(S * 0.09f), 2, col); break;
    case 9: // </>
      strokeSegAA(cx - S * 0.05f, cy - S * 0.14f, cx - S * 0.20f, cy, 2.0f, col);
      strokeSegAA(cx - S * 0.20f, cy, cx - S * 0.05f, cy + S * 0.14f, 2.0f, col);
      strokeSegAA(cx + S * 0.05f, cy - S * 0.14f, cx + S * 0.20f, cy, 2.0f, col);
      strokeSegAA(cx + S * 0.20f, cy, cx + S * 0.05f, cy + S * 0.14f, 2.0f, col); break;
    default: // info (i)
      drawCircle(cx, cy, (int)(S * 0.30f), col); drawCircle(cx, cy, (int)(S * 0.30f) - 1, col);
      fillCircle(cx, (int)(cy - S * 0.13f), 2, col);
      fillRect(cx - 1, (int)(cy - S * 0.03f), 3, (int)(S * 0.18f), col); break;
  }
}

// Registro de las filas realmente dibujadas en el panel de detalle de
// Ajustes (se resetea en settingsDetailContent() y lo llena cada
// setRowCard()). El tap-handler de settingsTick() lo consulta en vez de
// asumir que todas las filas miden 60px exactos y estan pegadas -- ese
// supuesto se rompia en cuanto habia un titulo de seccion o un texto de
// ayuda entre filas (bug: Pantalla->Bloqueo detectaba la fila equivocada).
#define SET_ROW_MAX 16
static int setRowY0[SET_ROW_MAX], setRowY1[SET_ROW_MAX], setRowN = 0;
// Tarjeta de fila (icono + titulo + valor + chevron). Devuelve la y siguiente.
// EL VIDRIO YA NO SE APAGA AL ARRASTRAR. Antes, mientras el dedo movia la lista,
// las tarjetas se pintaban planas porque drawLiquidGlassPanel (copiar + desenfocar
// + mezclar, por tarjeta y por cuadro) no daba para seguir al dedo: por eso "al
// hacer scroll se perdia el desenfoque y las transparencias". drawGlassCardFlat
// resuelve la tarjeta UNA vez y luego la vuelca por filas, asi que el material se
// mantiene durante todo el desplazamiento y el cuadro sigue siendo barato. Si el
// usuario tiene el Liquid Glass desactivado, la rama de abajo respeta su ajuste.
static int setRowCard(int y, int rIcon, uint16_t iCol, const char* title, const char* val, bool chevron){
  int rh = 64, mr = DP_X + DP_W - 30;
  if(setRowN < SET_ROW_MAX){ setRowY0[setRowN] = y; setRowY1[setRowN] = y + rh; setRowN++; }  // registra el rango real de esta fila
  if(uiGlass) drawGlassCardFlat(DP_X, y, DP_W, rh - 8, 14, SET_CARD_GLASS, PAGE_BG);          // tarjeta vidrio (cacheada)
  else fillRoundRect(DP_X, y, DP_W, rh - 8, 14, SET_CARD_BG);
  drawRowGlyph(rIcon, DP_X + 26, y + (rh - 8) / 2, iCol);
  drawTextClip(DP_X + 52, y + 10, title, 2, SET_TXT_HI, mr);
  if(val) drawTextClip(DP_X + 52, y + 34, val, 1, SET_TXT_LO, mr);
  if(chevron){ int chx = DP_X + DP_W - 20, chy = y + (rh - 8) / 2;
    strokeSegAA(chx - 3, chy - 6, chx + 3, chy, 2.0f, SET_CHEV);
    strokeSegAA(chx + 3, chy, chx - 3, chy + 6, 2.0f, SET_CHEV); }
  return y + rh;
}
static int drawInfoLine(int y, const char* label, const char* val){
  drawText(DP_X, y, label, 1, SET_TXT_LO);
  drawTextR(SCR_W - 12, y, val, 1, SET_TXT_HI);
  return y + 24;
}
static int drawDeviceInfo(int y){
  drawText(DP_X, y, "Dispositivo", 2, SET_TXT_HI); y += 30;
  char v[48];
  y = drawInfoLine(y, "Nombre", cfgName);
  y = drawInfoLine(y, "Modelo", "ESP32-P4 DevKit");
  y = drawInfoLine(y, "Version", "FlexOS Ultra 1.0");
  buildUptime(v, sizeof(v)); y = drawInfoLine(y, "Actividad", v);
  snprintf(v, sizeof(v), "%u KB libre", (unsigned)(esp_get_free_heap_size() / 1024)); y = drawInfoLine(y, "RAM", v);
  snprintf(v, sizeof(v), "%u / %u MB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576),
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1048576)); y = drawInfoLine(y, "PSRAM", v);
  y = drawInfoLine(y, "Flash", "16 MB");
  y = drawInfoLine(y, "CPU", "RISC-V dual 360 MHz");
  return y;
}

// Contenido del panel de detalle (con clip vertical activo)
static void settingsDetailContent(int cat){
  setRowN = 0;                       // reinicia el registro de filas (ver setRowCard)
  int base = DLIST_TOP - setScroll;
  int y = base + 6;
  char v[48];
  uint16_t ic = rgb565(70,120,225);
  if(cat == 0){
    y = setRowCard(y, RI_GLOBE,   rgb565(60,140,235), "Idioma", langNameCur(), true);
    settingsDateTimeStr(v, sizeof(v));
    y = setRowCard(y, RI_CAL,     rgb565(235,90,90),  "Fecha y hora", v, true);
    y = setRowCard(y, RI_CLOCK,   rgb565(90,120,230), "Formato de hora", g24h ? "24 horas" : "12 horas", true);
    y = setRowCard(y, RI_PIN,     rgb565(230,80,80),  "Zona horaria", "GMT-05:00 Lima", true);
    y = setRowCard(y, RI_REFRESH, rgb565(60,160,230), "Actualizaciones", flexOtaStatusText(), true);
    y = setRowCard(y, RI_CLOUD,   rgb565(120,160,230),"Copias de seguridad", "Proximamente", true);
    y = setRowCard(y, RI_RESET,   rgb565(220,80,80),  "Restablecer", "Opciones de fabrica", true);
    y += 12; y = drawDeviceInfo(y);
  } else if(cat == 11){
    y = drawDeviceInfo(y);
    y += 8; drawText(DP_X, y, "FlexOS Ultra - desde cero", 1, SET_TXT_MUTE); y += 22;
    drawText(DP_X, y, "para ESP32-P4 - 2026", 1, SET_TXT_MUTE); y += 22;
  } else if(cat == 1){                     // Pantalla (funcional)
    char bv[16]; snprintf(bv, sizeof(bv), "%d%%", gBright);
    y = setRowCard(y, RI_DOT, rgb565(240,170,50), "Brillo", bv, true);
    y = setRowCard(y, RI_DOT, rgb565(90,110,235), "Estilo", uiGlass ? "Liquid Glass" : "Plano", true);
    y = setRowCard(y, RI_DOT, gDark ? rgb565(120,130,220) : rgb565(240,170,50), "Modo de apariencia", gDark ? "Oscuro" : "Claro", true);
    y = setRowCard(y, RI_DOT, rgb565(100,180,240), "Barra de navegacion", gNavMode == 0 ? "Botones" : "Gestos iOS", true);
    y += 8; drawText(DP_X, y, "Toca una fila para cambiarla", 1, SET_TXT_MUTE); y += 24;
    y += 8; drawText(DP_X, y, "Bloqueo", 2, SET_TXT_HI); y += 30;
    y = setRowCard(y, RI_CLOCK, rgb565(90,120,230), "Reloj grande", (gLockWidgets & LW_CLOCK) ? "Activado" : "Desactivado", true);
    y = setRowCard(y, RI_CLOUD, rgb565(90,170,235), "Clima", (gLockWidgets & LW_WEATHER) ? "Activado" : "Desactivado", true);
    y = setRowCard(y, RI_CAL, rgb565(235,110,90), "Calendario", (gLockWidgets & LW_CAL) ? "Activado" : "Desactivado", true);
    y = setRowCard(y, RI_DOT, rgb565(230,180,90), "Notificaciones", (gLockWidgets & LW_NOTIF) ? "Activado" : "Desactivado", true);
    y += 8; drawText(DP_X, y, "Elige los widgets de la pantalla de bloqueo", 1, SET_TXT_MUTE); y += 24;
  } else if(cat == 5){                      // Personalizacion (funcional)
    y = setRowCard(y, RI_DOT, rgb565(90,110,235), "Personalizar UI", uiGlass ? "Liquid Glass" : "Plano", true);
    y = setRowCard(y, RI_DOT, rgb565(150,90,210), "Iconos", gIconStyle == 1 ? "Vidrio" : "Plano", true);
    const char* av = gAnimStyle == 1 ? "Fundido" : gAnimStyle == 2 ? "Deslizar" : "Zoom";
    y = setRowCard(y, RI_DOT, rgb565(90,200,160), "Transiciones", av, true);
#if KB_SETTINGS_ON
    // FASE E: puerta de entrada a los Ajustes del teclado (la otra es el
    // engranaje de la barra superior del propio teclado).
    y = setRowCard(y, RI_DOT, rgb565(235,150,60), "Teclado",
                   gKbSize == KB_SIZE_COMPACT ? "Compacto" : gKbSize == KB_SIZE_BIG ? "Grande" : "Normal", true);
#endif
    y += 8; drawText(DP_X, y, "Toca una fila para cambiar su estilo", 1, SET_TXT_MUTE); y += 24;
  } else if(cat == 3){                      // Red e Internet (datos REALES)
    char rv0[64]; connWifiSub(rv0, sizeof(rv0));
    // Se quitan los parentesis del subtitulo de la pantalla de Conectividad:
    // aqui la fila ya tiene su propio titulo delante.
    char* rvp = rv0; int rvl = strlen(rv0);
    if(rvl > 1 && rv0[0] == '(' && rv0[rvl - 1] == ')'){ rv0[rvl - 1] = 0; rvp = rv0 + 1; }
    char rv1[64]; connBleSub(rv1, sizeof(rv1));
    char* rbp = rv1; int rbl = strlen(rv1);
    if(rbl > 1 && rv1[0] == '(' && rv1[rbl - 1] == ')'){ rv1[rbl - 1] = 0; rbp = rv1 + 1; }
    y = setRowCard(y, RI_GLOBE, rgb565(60,150,235), "Conectividad",
                   gAirplane ? "Modo avi\xC3\xB3n activo" : "Wifi, BLE y modo avi\xC3\xB3n", true);
    y = setRowCard(y, RI_CLOUD, rgb565(70,120,225), "Wi-Fi", rvp, true);
    y = setRowCard(y, RI_DOT,   rgb565(90,110,235), "Bluetooth (BLE)", rbp, true);
    y += 8; drawText(DP_X, y, "Los interruptores encienden la radio de verdad", 1, SET_TXT_MUTE); y += 24;
  } else if(cat == 6){                      // Seguridad (funcional)
    const char* lt = gLockType == 1 ? "PIN configurado" : gLockType == 2 ? "Contrase\xC3\xB1" "a configurada" : "Deslizar";
    y = setRowCard(y, RI_DOT, rgb565(220,120,120), "Bloqueo", lt, true);
    y = setRowCard(y, RI_CLOCK, rgb565(120,150,235), "Bloqueo de inactividad", autoLockName(), true);
#if POWEROFF_ON && POWEROFF_PIN_ON
    // Apagado seguro: pide el PIN/contrasena antes de apagar del todo. NO afecta
    // a la suspension (doble-tap de 2 dedos), que nunca pide clave.
    y = setRowCard(y, RI_DOT, rgb565(220,120,120), "Apagado seguro",
                   gLockType == 0 ? "Configura antes un PIN" : (gPoffPin ? "Activado" : "Desactivado"), true);
#endif
    y += 8; drawText(DP_X, y, "Toca para configurar PIN o contrase\xC3\xB1" "a", 1, SET_TXT_MUTE); y += 24;
  } else {
    const char* rt[3] = {0,0,0}; const char* rv[3] = {0,0,0}; int rn = 0;
    switch(cat){
      case 2: rt[0]="Volumen";rv[0]="70%"; rt[1]="Tono";rv[1]="Predeterminado"; rn=2; break;
      // (la categoria 3 tiene ahora su propia rama, con datos reales)
      case 4: rt[0]="GPIO";rv[0]="Configurable"; rt[1]="Perifericos";rv[1]="Ninguno"; rn=2; break;
      case 6: rt[0]="Bloqueo";rv[0]="Deslizar"; rt[1]="PIN";rv[1]="No configurado"; rn=2; break;
      case 7: rt[0]="Nivel";rv[0]="82%"; rt[1]="Ahorro";rv[1]="Desactivado"; rn=2; break;
      case 8: {   // valores REALES de la particion de datos
        static char s8a[32], s8b[32];
        char u8[16], t8[16];
        flexFsFmtSize(flexFsUsedBytes(), u8, sizeof(u8));
        flexFsFmtSize(flexFsTotalBytes(), t8, sizeof(t8));
        snprintf(s8a, sizeof(s8a), "%s / %s", u8, t8);
        flexFsFmtSize(flexFsCatSize(FLEXFS_CAT_TRASH), s8b, sizeof(s8b));
        rt[0]="Interna"; rv[0]= flexFsReady() ? s8a : "No montada";
        rt[1]="Papelera"; rv[1]=s8b; rn=2; } break;
      case 9: rt[0]="Depuracion";rv[0]="En pantalla"; rt[1]="Banda reinicio";rv[1]="Solo crash"; rn=2; break;
      case 10: rt[0]="Version";rv[0]="FlexOS 1.0"; rt[1]="Logs";rv[1]="Puerto serie"; rn=2; break;
      default: rn=0; break;
    }
    for(int i = 0; i < rn; i++) y = setRowCard(y, RI_DOT, ic, rt[i], rv[i], true);
    y += 8; drawText(DP_X, y, "Mas opciones proximamente", 1, SET_TXT_MUTE); y += 24;
  }
  setContentH = (y - base) + 10;
}

// Acentos de cada categoria (los mismos de siempre, solo que ahora viven en la
// tarjeta de la lista principal en vez de en la barra lateral).
static const uint16_t SET_ACCENT[12] = {
  rgb565(70,120,235), rgb565(240,170,50), rgb565(70,120,235), rgb565(60,150,235),
  rgb565(80,180,120), rgb565(90,110,235), rgb565(90,95,110), rgb565(80,190,110),
  rgb565(150,90,210), rgb565(70,75,90), rgb565(70,120,235), rgb565(70,120,235) };

// ---- PANTALLA 1: lista de categorias (a todo el ancho, con scroll) ----
#define SET_CATCARD_H  66
#define SET_CATCARD_GAP 8
static int setCatY0[12], setCatY1[12];      // rango real de cada tarjeta (para el tap)
static void settingsDrawListHead(){
  drawText(16, 40, "Ajustes", 4, SET_TXT_HI);
}
static void settingsListContent(){
  int base = SET_LIST_TOP - setListScroll;
  for(int i = 0; i < 12; i++){
    int y = base + i * (SET_CATCARD_H + SET_CATCARD_GAP);
    setCatY0[i] = y; setCatY1[i] = y + SET_CATCARD_H;
    if(y > SET_LIST_BOT || y + SET_CATCARD_H < SET_LIST_TOP) continue;   // fuera de la ventana: ni se dibuja
    if(uiGlass) drawGlassCardFlat(SET_CARD_X, y, SET_CARD_W, SET_CATCARD_H, 16, SET_CARD_GLASS, PAGE_BG);
    else fillRoundRect(SET_CARD_X, y, SET_CARD_W, SET_CATCARD_H, 16, SET_CARD_BG);
    drawSetCatIcon(i, SET_CARD_X + 16, y + (SET_CATCARD_H - 32) / 2, 32, SET_ACCENT[i]);
    int tx = SET_CARD_X + 62, mr = SET_CARD_X + SET_CARD_W - 30;
    drawTextClip(tx, y + 14, SET_CAT[i], 2, SET_TXT_HI, mr);
    drawTextClip(tx, y + 38, SET_SUB[i], 1, SET_SIDE_SUB, mr);
    int chx = SET_CARD_X + SET_CARD_W - 20, chy = y + SET_CATCARD_H / 2;
    strokeSegAA(chx - 3, chy - 6, chx + 3, chy, 2.0f, SET_CHEV);
    strokeSegAA(chx + 3, chy, chx - 3, chy + 6, 2.0f, SET_CHEV);
  }
  setListH = 12 * (SET_CATCARD_H + SET_CATCARD_GAP) + 10;
}

// ---- PANTALLA 2: cabecera de la categoria abierta (con "atras") ----
#define SET_BACK_W 52                       // zona tactil del chevron de volver
static void settingsDrawDetailHead(){
  uint16_t c = SET_TXT_HI;
  strokeSegAA(30, 46, 18, 58, 2.6f, c);     // chevron "<" (centrado con el titulo)
  strokeSegAA(18, 58, 30, 70, 2.6f, c);
  drawTextClip(SET_BACK_W, 42, SET_CAT[setSel], 4, SET_TXT_HI, SCR_W - 12);
  drawTextClip(SET_BACK_W, 94, SET_DESC[setSel], 1, SET_TXT_LO, SCR_W - 12);
}
static void settingsDrawChromeDark(){
  if(gHosted) return;                // idem: hora, wifi, bateria y gestos son del UI principal
  uint16_t D = SET_TXT_HI;
  char cs[12]; clkStrBar(cs, sizeof(cs));
  drawText(16, 16, cs, 2, D);
  char sd[40]; buildShortDate(sd, sizeof(sd));
  drawText(16 + textW(cs, 2) + 14, 20, sd, 1, D);
  drawWifi(SCR_W - 66, 28, 11, D);
  drawBattery(SCR_W - 46, 20, 30, 15, 82, D);
  if(gNavMode == 0){
    int ny = SCR_H - 52;
    fillTriangle(SCR_W / 6 - 10, ny + 8, SCR_W / 6 + 8, ny - 2, SCR_W / 6 + 8, ny + 18, D);
    drawCircle(SCR_W / 2, ny + 8, 12, D); drawCircle(SCR_W / 2, ny + 8, 11, D);
    drawRoundRect(SCR_W * 5 / 6 - 11, ny - 3, 22, 22, 4, D);
  } else {
    drawHomeIndicator(SCR_H, 210, SET_NAVPILL);   // pildora de gestos, a juego con el fondo de la pagina
  }
}
// Ventana con scroll de la vista activa.
static inline int setBandTop(){ return setView == 0 ? SET_LIST_TOP : DLIST_TOP; }
static inline int setBandBot(){ return setView == 0 ? SET_LIST_BOT : DLIST_BOT; }
static inline int setMaxScroll(){
  int h = (setView == 0) ? setListH : setContentH;
  int m = h - (setBandBot() - setBandTop());
  return m > 0 ? m : 0;
}
// Pinta la PAGINA COMPLETA de una vista en el gBuf actual. Es la unica fuente de
// verdad del aspecto de la app: la usan el repintado normal y los dos lienzos de
// la transicion, asi que lo que se anima es exactamente lo que luego se ve.
static void settingsPaintPage(int view){
  fillRect(0, 0, SCR_W, SCR_H, PAGE_BG);
  settingsDrawChromeDark();
  int top, bot;
  if(view == 0){ settingsDrawListHead();   top = SET_LIST_TOP; bot = SET_LIST_BOT; }
  else         { settingsDrawDetailHead(); top = DLIST_TOP;    bot = DLIST_BOT;    }
  int c0 = gClipY0, c1 = gClipY1;
  gClipY0 = top; gClipY1 = bot - 1;
  if(view == 0) settingsListContent(); else settingsDetailContent(setSel);
  gClipY0 = c0; gClipY1 = c1;
}
// Repintado completo. Se compone en bbuf y se publica de una sola vez con
// present(): un cuadro entero o ninguno. Antes se dibujaba fila a fila DIRECTO
// sobre fb mientras el presenter podia estar subiendolo, que es de donde salian
// los parpadeos y las costuras al repintar.
static void settingsRender(){
  setBuf(bbuf);
  settingsPaintPage(setView);
  present(0, SCR_H - 1);
  setBuf(fb);
}
// Repintado de SOLO la banda con scroll (cada cuadro de un arrastre). Tambien
// via bbuf + present: el vidrio de las tarjetas se mantiene y no hay costuras.
static void settingsRenderBandOnly(){
  int top = setBandTop(), bot = setBandBot();
  setBuf(bbuf);
  fillRect(0, top, SCR_W, bot - top, PAGE_BG);
  int c0 = gClipY0, c1 = gClipY1;
  gClipY0 = top; gClipY1 = bot - 1;
  if(setView == 0) settingsListContent(); else settingsDetailContent(setSel);
  gClipY0 = c0; gClipY1 = c1;
  present(top, bot - 1);
  setBuf(fb);
}
// Nombre historico: lo siguen llamando las acciones de fila (settingsRowAction).
static void settingsRenderDetailOnly(){ settingsRenderBandOnly(); }

// ---- TRANSICION ENTRE PANTALLAS (empuje horizontal con paralaje) ----
// Dos lienzos de pagina en PSRAM. Se reservan la primera vez que se navega y se
// conservan: reservarlos y liberarlos en cada transicion solo fragmentaria el
// monton. Si no hay PSRAM, la navegacion sigue funcionando -- solo se pierde la
// animacion, nunca la funcion.
static uint16_t *setPgOut = NULL, *setPgIn = NULL;
static bool settingsPagesReady(){
  if(!setPgOut) setPgOut = (uint16_t*)heap_caps_malloc((size_t)SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!setPgIn)  setPgIn  = (uint16_t*)heap_caps_malloc((size_t)SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return setPgOut && setPgIn;
}
// Una fila desplazada 'off' px, recortada a la pantalla.
static inline void setRowShift(uint16_t* dst, const uint16_t* src, int off){
  int ds = off, ss = 0, n = SCR_W;
  if(ds < 0){ ss = -ds; n = SCR_W + ds; ds = 0; }
  else if(ds > 0) n = SCR_W - ds;
  if(n > 0) memcpy(dst + ds, src + ss, (size_t)n * 2);
}
// dir = +1 al ABRIR una categoria (la nueva entra desde la derecha),
// dir = -1 al VOLVER (la de detalle se va por la derecha y descubre la lista).
static void settingsAnimate(int fromView, int toView, int dir){
  // Hospedada en una ventana de Modo PC no se anima: de un tick hospedado solo
  // se ve el ultimo cuadro. El cambio de pantalla es instantaneo, como antes.
  if(gHosted || !settingsPagesReady()){ setView = toView; settingsRender(); return; }
  int keep = setView;
  setView = fromView; setBuf(setPgOut); settingsPaintPage(fromView);
  setView = toView;   setBuf(setPgIn);  settingsPaintPage(toView);
  setView = keep;
  setBuf(fb);
  const int y0 = SET_ANIM_Y0, y1 = SET_ANIM_Y1 - 1;
  const int PARA = (SCR_W * 28) / 100;               // paralaje: la que sale recorre un 28%
  int lastO = 0x7FFF, lastI = 0x7FFF;                // ultimo par de desplazamientos ya pintado
  uint32_t t0 = millis();
  for(;;){
    uint32_t e = millis() - t0; if(e > (uint32_t)SET_NAV_MS) e = SET_NAV_MS;
    float p = (float)e / (float)SET_NAV_MS;
    float ip = 1.0f - p;
    p = (p < 0.5f) ? (4.0f * p * p * p) : (1.0f - 4.0f * ip * ip * ip);   // ease-in-out cubica
    int oOff, iOff;
    if(dir > 0){ oOff = (int)(-PARA * p);            // sale hacia la izquierda, despacio
                 iOff = (int)(SCR_W * (1.0f - p)); }  // entra desde la derecha
    else       { oOff = (int)(SCR_W * p);            // sale hacia la derecha
                 iOff = (int)(-PARA * (1.0f - p)); }  // la de debajo vuelve a su sitio
    if(oOff == lastO && iOff == lastI){          // el reloj aun no ha movido nada
      if(e < (uint32_t)SET_NAV_MS){ delay(1); continue; }
      break;
    }
    lastO = oOff; lastI = iOff;
    for(int j = y0; j <= y1; j++){
      uint16_t* d = bbuf + (size_t)j * SCR_W;
      // Orden de pintado = orden de profundidad. Al abrir, la nueva va ENCIMA;
      // al volver, la que se va es la de encima y descubre a la de abajo.
      if(dir > 0){ setRowShift(d, setPgOut + (size_t)j * SCR_W, oOff);
                   setRowShift(d, setPgIn  + (size_t)j * SCR_W, iOff); }
      else       { setRowShift(d, setPgIn  + (size_t)j * SCR_W, iOff);
                   setRowShift(d, setPgOut + (size_t)j * SCR_W, oOff); }
      // Sombra de 8 px en el borde de la pagina de encima: da profundidad y
      // tapa la costura entre las dos capas.
      int edge = (dir > 0) ? iOff : oOff;
      if(edge > 0 && edge <= SCR_W){
        for(int k = 1; k <= 8; k++){
          int x = edge - k; if(x < 0) break;
          d[x] = mix565(d[x], rgb565(0,0,0), (uint8_t)(70 - k * 8));
        }
      }
    }
    present(y0, y1);
    if(e >= (uint32_t)SET_NAV_MS) break;
  }
  setView = toView;
  settingsRender();                                   // estado final limpio, sin restos del paralaje
}
static void settingsOpenCat(int cat){
  if(cat < 0 || cat > 11) return;
  setSel = cat; setScroll = 0; setDragging = false;
  settingsAnimate(0, 1, +1);
}
// Volver a la lista. Devuelve false si ya estabamos en la lista: asi el boton
// "atras" del sistema cierra la app solo cuando no queda pantalla que cerrar
// (igual que en Android).
static bool settingsHandleBack(){
  if(setView != 1) return false;
  setDragging = false; setBackSwipe = false;
  settingsAnimate(1, 0, -1);
  return true;
}
static void settingsEnter(){
  setView = 0; setSel = 0; setScroll = 0; setListScroll = 0;
  setDragging = false; setBackSwipe = false;
  settingsRender();
}
// Accion al tocar una fila del panel de detalle (ajustes funcionales)
// Todo ajuste que cambie el ASPECTO del escritorio marca gHomeDirty: homeBuf es
// una cache ya pintada y hay que recomponerla antes de volver a mostrarla.
// (gBright no lo hace: es PWM del backlight, no repinta nada.)
static void settingsRowAction(int cat, int idx){
  if(cat == 0){
    if(idx == 0){ cfgLang = (cfgLang + 1) % 6; cfgSavePrefs(); gHomeDirty = true; settingsRender(); }         // idioma (cicla)
    else if(idx == 2){ g24h = !g24h; cfgSavePrefs(); gHomeDirty = true; settingsRenderDetailOnly(); }         // formato 12/24
    else if(idx == 4) flexOtaOpenSettings();                                                                 // Actualizaciones -> pantalla OTA
  } else if(cat == 1){
    if(idx == 0){ gBright += 25; if(gBright > 100) gBright = 25; setBacklight(gBright); cfgSavePrefs(); settingsRenderDetailOnly(); }  // brillo real
    else if(idx == 1){ uiGlass = !uiGlass; cfgSavePrefs(); gHomeDirty = true; settingsRenderDetailOnly(); }    // estilo Liquid Glass
    else if(idx == 2){ gDark = !gDark; cfgSavePrefs(); settingsRender(); }  // Modo de apariencia: oscuro <-> claro (aplica ya, sin reiniciar)
    else if(idx == 3){ gNavMode = (gNavMode == 0) ? 1 : 0; cfgSavePrefs(); gHomeDirty = true; settingsRender(); } // barra: botones <-> gestos (redibuja tambien la barra inferior)
    else if(idx == 4){ gLockWidgets ^= LW_CLOCK;   cfgSavePrefs(); settingsRenderDetailOnly(); }  // Bloqueo: reloj grande
    else if(idx == 5){ gLockWidgets ^= LW_WEATHER; cfgSavePrefs(); settingsRenderDetailOnly(); }  // Bloqueo: clima
    else if(idx == 6){ gLockWidgets ^= LW_CAL;     cfgSavePrefs(); settingsRenderDetailOnly(); }  // Bloqueo: calendario
    else if(idx == 7){ gLockWidgets ^= LW_NOTIF;   cfgSavePrefs(); settingsRenderDetailOnly(); }  // Bloqueo: notificaciones
  } else if(cat == 5){
    if(idx == 0){ uiGlass = !uiGlass; cfgSavePrefs(); gHomeDirty = true; settingsRenderDetailOnly(); }         // Personalizar UI
    else if(idx == 1){ gIconStyle = (gIconStyle == 0) ? 1 : 0; cfgSavePrefs(); gHomeDirty = true; settingsRenderDetailOnly(); }  // estilo de iconos: Plano <-> Vidrio
    else if(idx == 2){ gAnimStyle = (gAnimStyle + 1) % 3; cfgSavePrefs(); settingsRenderDetailOnly(); }  // transiciones: zoom -> fundido -> deslizar -> zoom
#if KB_SETTINGS_ON
    else if(idx == 3) kbsEnter();                                                                       // FASE E: Ajustes del teclado
#endif
  } else if(cat == 3){
    if(idx == 0) connEnter();                                                            // Conectividad (Wifi/BLE/Modo avion)
    else if(idx == 1) wifiSettingsEnter();                                               // Red e Internet -> Wi-Fi
    else if(idx == 2){                                                                   // Bluetooth (BLE): conmuta de verdad
      if(!FLEXOS_ENABLE_BLE || gAirplane) return;
      if(gBleOn) flexBleStop(); else flexBleStart();
      settingsRenderDetailOnly();
    }
  } else if(cat == 6){
    if(idx == 0) lsuEnter();                                                                 // Seguridad -> Bloqueo (PIN/Contraseña)
    else if(idx == 1){                                                                       // Seguridad -> Bloqueo de inactividad
      // Cicla 30 s -> 1 min -> 5 min -> 10 min -> 30 min -> Nunca -> 30 s, igual
      // que hacen las demas filas de opcion de Ajustes (Transiciones, Iconos...).
      gAutoLockMs = AUTOLOCK_OPTS[(autoLockIdx() + 1) % AUTOLOCK_NOPT];
      gLastTouchMs = millis();                    // el temporizador nuevo cuenta desde ahora
      cfgSavePrefs();
      settingsRenderDetailOnly();
    }
#if POWEROFF_ON && POWEROFF_PIN_ON
    else if(idx == 2){                                                                   // Seguridad -> Apagado seguro
      // Sin PIN/contrasena configurada no hay nada que pedir: activarlo seria una
      // proteccion de mentira. Se deja tal cual y la fila ya avisa ("Configura
      // antes un PIN").
      if(gLockType == 0) return;
      gPoffPin = !gPoffPin;
      cfgSavePrefs();
      settingsRenderDetailOnly();
    }
#endif
  }
}
static void settingsTick(){
  int top = setBandTop(), bot = setBandBot(), maxS = setMaxScroll();

  // ---- Volver: chevron de la cabecera o arrastre desde el borde izquierdo ----
  if(setView == 1){
    if(T.tap && T.x < SET_BACK_W && T.y >= 20 && T.y <= 84){ settingsHandleBack(); return; }
    // Gesto de retroceso: empieza pegado al borde izquierdo y se lleva el dedo a
    // la derecha. Se decide al soltar (no a media pulsacion) para que un scroll
    // que empiece cerca del borde no lo dispare por accidente.
    if(T.pressed && T.startX < 28) setBackSwipe = true;
    if(setBackSwipe && T.released){
      setBackSwipe = false;
      if((T.x - T.startX) > 70 && abs(T.y - T.startY) < 90){ settingsHandleBack(); return; }
    }
    if(!T.down) setBackSwipe = false;
  }

  // ---- Tap ----
  if(T.tap && !setDragging && T.y >= top && T.y <= bot){
    if(setView == 0){
      for(int i = 0; i < 12; i++)
        if(T.y >= setCatY0[i] && T.y < setCatY1[i]){ settingsOpenCat(i); return; }
    } else {
      for(int i = 0; i < setRowN; i++)
        if(T.y >= setRowY0[i] && T.y < setRowY1[i]){ settingsRowAction(setSel, i); return; }
    }
  }

  // ---- Scroll de la vista activa ----
  // Arrastre real: el contenido va pegado al dedo cuadro a cuadro, con umbral de
  // 6 px para no confundir un toque con un arrastre. Ahora vale para las DOS
  // pantallas (la lista de categorias tambien se desplaza) y ya no apaga el
  // vidrio de las tarjetas mientras dura.
  int* scroll = (setView == 0) ? &setListScroll : &setScroll;
  if(T.pressed && T.y >= top - 24 && T.y <= bot){ setDragY0 = T.y; setDragS0 = *scroll; setDragging = false; }
  if(T.down && maxS > 0 && T.startY >= top - 24 && T.startY <= bot){
    int dy = setDragY0 - T.y;
    if(!setDragging && abs(dy) > 6) setDragging = true;
    if(setDragging){
      int ns = setDragS0 + dy;
      if(ns < 0) ns = 0; if(ns > maxS) ns = maxS;
      if(ns != *scroll){ *scroll = ns; settingsRenderBandOnly(); }
      return;
    }
  }
  if(T.released && setDragging){ setDragging = false; return; }
}

// #############################################################
// ##  APP CALCULADORA  (Milestone 2)  ·  app normal (marco estandar)
// #############################################################
static char   calcDisp[24] = "0";
static double calcAcc = 0;
static char   calcOp = 0;          // 0,'+','-','x','/'
static bool   calcFresh = true;    // el proximo digito empieza entrada nueva

static const char* CALC_LBL[5][4] = {
  {"C","+/-","%","/"}, {"7","8","9","x"}, {"4","5","6","-"},
  {"1","2","3","+"},   {"0",".","=","DEL"} };

static bool calcErr = false;       // el display muestra "Error" (division por cero / desbordamiento)

// Finito sin depender de macros de math.h (a prueba de .ino):
// NaN falla (v == v); los infinitos fallan el rango.
static inline bool calcFinite(double v){ return (v == v) && (v > -1.0e308) && (v < 1.0e308); }

static void calcFmt(double v){
  if(!calcFinite(v)){ snprintf(calcDisp, sizeof(calcDisp), "Error"); calcErr = true; return; }
  calcErr = false;
  if(v == 0) v = 0;                 // evita "-0"
  snprintf(calcDisp, sizeof(calcDisp), "%g", v);
}
static double calcCompute(double a, double b, char op){
  // Antes 5/0 devolvia 0 en silencio: una respuesta falsa presentada como buena.
  // Ahora se propaga un NaN y calcFmt lo convierte en "Error".
  switch(op){ case '+': return a + b; case '-': return a - b;
              case 'x': return a * b; case '/': return (b != 0) ? (a / b) : (double)NAN; }
  return b;
}
static void calcKey(char k){
  // Con "Error" en pantalla, el estado numerico no vale: cualquier entrada de
  // numero limpia primero. Los operadores se ignoran (no hay operando valido).
  if(calcErr){
    if(k == 'c' || (k >= '0' && k <= '9') || k == '.'){
      strcpy(calcDisp, "0"); calcAcc = 0; calcOp = 0; calcFresh = true; calcErr = false;
      if(k == 'c') return;
    } else return;
  }
  int L = strlen(calcDisp);
  if(k >= '0' && k <= '9'){
    if(calcFresh || (L == 1 && calcDisp[0] == '0')){ calcDisp[0] = k; calcDisp[1] = 0; }
    else if(L < 16){ calcDisp[L] = k; calcDisp[L + 1] = 0; }
    calcFresh = false;
  } else if(k == '.'){
    if(calcFresh){ strcpy(calcDisp, "0."); calcFresh = false; }
    else if(!strchr(calcDisp, '.') && L < 15){ calcDisp[L] = '.'; calcDisp[L + 1] = 0; }
  } else if(k == 'c'){ strcpy(calcDisp, "0"); calcAcc = 0; calcOp = 0; calcFresh = true; }
  else if(k == '\b'){ if(!calcFresh && L > 0){ calcDisp[L - 1] = 0; if(calcDisp[0] == 0) strcpy(calcDisp, "0"); } }
  else if(k == 'n'){
    if(strcmp(calcDisp, "0") != 0){
      if(calcDisp[0] == '-') memmove(calcDisp, calcDisp + 1, strlen(calcDisp));
      else { memmove(calcDisp + 1, calcDisp, strlen(calcDisp) + 1); calcDisp[0] = '-'; }
    }
  } else if(k == '%'){ calcFmt(atof(calcDisp) / 100.0); calcFresh = true; }
  else if(k == '+' || k == '-' || k == 'x' || k == '/'){
    double cur = atof(calcDisp);
    calcAcc = (calcOp && !calcFresh) ? calcCompute(calcAcc, cur, calcOp) : cur;
    calcOp = k; calcFresh = true; calcFmt(calcAcc);
  } else if(k == '='){
    if(calcOp){ calcAcc = calcCompute(calcAcc, atof(calcDisp), calcOp); calcFmt(calcAcc); calcOp = 0; }
    calcFresh = true;
  }
}
static void calcKeyFromLabel(const char* t){
  char k;
  if(!strcmp(t, "C")) k = 'c';
  else if(!strcmp(t, "+/-")) k = 'n';
  else if(!strcmp(t, "DEL")) k = '\b';
  else k = t[0];   // digitos, '.', '%', '/', 'x', '-', '+', '='
  calcKey(k);
}
// ---- Layout ADAPTATIVO (app de referencia del modo embebido) ----
// Nada de constantes de pantalla completa: todo sale del lienzo logico
// (gAppW x [WIN_TOP..WIN_BOT]), que a pantalla completa es la pantalla menos su
// chrome y dentro de una ventana de DeX es el area de cliente. La misma
// funcion sirve para las dos situaciones y para cualquier tamano intermedio.
// CALCULADORA · adaptativa (app de referencia).
//   Esencial   : rejilla 5x4 de teclas -- tiene PRIORIDAD sobre el display.
//   Opcional 1 : display -- cede alto a la rejilla y llega a omitirse si el
//                lienzo no da para las 5 filas.
//   Opcional 2 : panel lateral de memoria e historial -- aparece cuando el
//                ancho da para la rejilla con teclas de >= 44 px MAS 150 px de
//                panel. Por debajo de ese umbral desaparece entero, nunca
//                encogido a medias.
#define CALC_SIDE_MIN 150
#define CALC_KEY_MIN   44
// Ancho que reserva el panel lateral (0 si no toca mostrarlo).
static int calcSideW(){
  int w = gAppW, pad = w / 30; if(pad < 4) pad = 4; if(pad > 16) pad = 16;
  int gap = w / 40; if(gap < 3) gap = 3; if(gap > 12) gap = 12;
  int need = 4 * CALC_KEY_MIN + 3 * gap + 2 * pad + CALC_SIDE_MIN + gap;
  if(w < need) return 0;
  int sw = w / 4; if(sw < CALC_SIDE_MIN) sw = CALC_SIDE_MIN; if(sw > 260) sw = 260;
  return sw;
}
static void calcBox(int &bx, int &by, int &bw, int &bh){
  bx = 0; by = WIN_TOP; bw = gAppW; bh = WIN_BOT - WIN_TOP;
  int sw = calcSideW();
  if(sw > 0) bw -= sw;                          // la calculadora cede sitio al panel
  if(bw < 40) bw = 40;
  if(bh < 60) bh = 60;
}
// Reparto vertical. La rejilla tiene PRIORIDAD: primero se asegura de que las 5
// filas caben, y el display se queda con lo que sobre (hasta desaparecer en
// lienzos absurdamente bajos). Al reves -- display con alto minimo fijo -- la
// rejilla se salia por debajo del marco en ventanas achatadas.
static void calcLayout(int &m, int &gap, int &dh, int &bwv, int &bhv){
  int bx, by, bw, bh; calcBox(bx, by, bw, bh);
  (void)bx; (void)by;
  m = gAppW / 30; if(m < 4) m = 4; if(m > 16) m = 16;
  if(4 * m > bh){ m = bh / 8; if(m < 2) m = 2; }
  gap = gAppW / 40; if(gap < 3) gap = 3; if(gap > 12) gap = 12;
  int avail = bh - 2 * m; if(avail < 20) avail = 20;
  int need = 5 * 6 + 4 * gap;                   // minimo vital de la rejilla
  while(gap > 2 && need > avail * 3 / 4){ gap--; need = 5 * 6 + 4 * gap; }
  dh = avail / 5;                               // el display aspira a ~1/5
  if(dh > 120) dh = 120;
  int maxDh = avail - m - need;                 // ...pero nunca a costa de la rejilla
  if(dh > maxDh) dh = maxDh;
  if(dh < 0) dh = 0;
  int gridH = avail - dh - (dh > 0 ? m : 0);
  bhv = (gridH - 4 * gap) / 5; if(bhv < 6) bhv = 6;
  bwv = (bw - 2 * m - 3 * gap) / 4; if(bwv < 6) bwv = 6;
}
static void calcDispRect(int &x, int &y, int &w, int &h){
  int bx, by, bw, bh; calcBox(bx, by, bw, bh);
  int m, gap, dh, bwv, bhv; calcLayout(m, gap, dh, bwv, bhv);
  (void)bh;
  x = bx + m; y = by + m; w = bw - 2 * m; h = dh;
}
static void calcGrid(int &gx, int &gy, int &bw, int &bh, int &gap){
  int bx, by, bwx, bhx; calcBox(bx, by, bwx, bhx);
  (void)bwx; (void)bhx;
  int m, dh, bwv, bhv; calcLayout(m, gap, dh, bwv, bhv);
  gx = bx + m;
  gy = by + m + dh + (dh > 0 ? m : 0);
  bw = bwv; bh = bhv;
}
// Tamano de fuente segun el boton, para que la etiqueta nunca se salga.
static int calcFontFor(int bw, int bh, const char* t){
  int lim = (bw < bh) ? bw : bh;
  int fs = lim >= 56 ? 4 : lim >= 38 ? 3 : lim >= 24 ? 2 : 1;
  if(strlen(t) > 1 && fs > 1) fs--;
  while(fs > 1 && textW(t, fs) > bw - 6) fs--;
  return fs;
}
static int calcKeyY0 = 0, calcKeyY1 = 0;
static void calcRender(){
  // A pantalla completa se sigue componiendo en lockBuf y volcando de una
  // pasada (anti-parpadeo, igual que antes). Embebida NO: el lienzo de la
  // ventana ya se compone entero fuera de pantalla y se vuelca de golpe, y
  // ademas fbCopyBand copia FILAS FISICAS, que con el lienzo rotado de una
  // ventana apaisada no corresponden a las filas logicas.
  bool host = gHosted;
  setBuf(host ? fb : lockBuf);                 // hospedada, fb ya apunta al lienzo
  // Se limpia el LIENZO ENTERO, no solo la caja de la calculadora. calcBox le
  // resta el ancho del panel lateral, asi que limpiar solo esa caja dejaba sin
  // tocar la franja del panel: al cruzar el breakpoint quedaban ahi las
  // tarjetas del frame anterior, y el fundido se mezclaba contra esa basura en
  // vez de contra el fondo. Eso era el ghosting.
  int fx, fy, fw, fh; uiBox(fx, fy, fw, fh);
  fillRect(fx, fy, fw, fh, WIN_BG);
  int bx, by, bw0, bh0; calcBox(bx, by, bw0, bh0);
  int dx, dy, dw, dh; calcDispRect(dx, dy, dw, dh);
  if(dh > 0){
    int drad = dh / 5; if(drad > 14) drad = 14; if(drad < 2) drad = 2;
    fillRoundRect(dx, dy, dw, dh, drad, rgb565(28,31,40));
    int dfs = dh >= 90 ? 5 : dh >= 64 ? 4 : dh >= 40 ? 3 : 2;
    while(dfs > 1 && textW(calcDisp, dfs) > dw - 20) dfs--;
    drawTextR(dx + dw - 10, dy + dh / 2 - dfs * 4, calcDisp, dfs, rgb565(255,255,255));
  }
  int gx, gy, bw, bh, gap; calcGrid(gx, gy, bw, bh, gap);
  calcKeyY0 = gy - 4; calcKeyY1 = gy + 5 * (bh + gap) + 4;
  if(calcKeyY1 > gAppH) calcKeyY1 = gAppH;
  int rad = bw / 6; if(rad > 14) rad = 14; if(rad < 3) rad = 3;
  for(int r = 0; r < 5; r++) for(int c = 0; c < 4; c++){
    int x = gx + c * (bw + gap), y = gy + r * (bh + gap);
    const char* tl = CALC_LBL[r][c];
    uint16_t bg;
    if(c == 3 || (r == 4 && c == 2)) bg = rgb565(245,150,40);
    else if(r == 0)                  bg = rgb565(70,74,86);
    else                             bg = rgb565(92,96,110);
    // drawLiquidGlassPanel solo es correcto en portrait sin rotar; con lienzo
    // apaisado (ventana ancha) se usa el relleno plano.
    if(uiGlass && !gLand) drawLiquidGlassPanel(x, y, bw, bh, rad, bg);
    else fillRoundRect(x, y, bw, bh, rad, bg);
    int fs = calcFontFor(bw, bh, tl);
    drawTextC(x + bw / 2, y + bh / 2 - fs * 4 + 1, tl, fs, rgb565(248,248,252));
  }
  // Panel lateral opcional: memoria e historial. Aparece/desaparece entero.
  int sw = calcSideW();
  uint8_t aSide = uiSection(0, sw > 0);
  if(aSide && sw > 0){
    int pad, gapL, dhL, bwL, bhL; calcLayout(pad, gapL, dhL, bwL, bhL);
    (void)gapL; (void)dhL; (void)bwL; (void)bhL;
    int sx = bx + bw0, sy = by, shh = bh0;
    uiRectA(sx + pad / 2, sy + pad, sw - pad, shh - 2 * pad, pad, rgb565(30,34,46), aSide);
    int ix = sx + pad, iw = sw - 2 * pad, iy = sy + pad * 2;
    uiTextC(sx + sw / 2, iy, "Memoria", uiFontFit("Memoria", iw, 3), rgb565(150,160,190), aSide);
    iy += uiLineH(3) + pad;
    const char* mk[3] = { "MC", "MR", "M+" };
    int mh = (shh / 8) < 30 ? 30 : (shh / 8);
    for(int i = 0; i < 3; i++){
      uiRectA(ix, iy, iw, mh, mh / 4, rgb565(70,74,86), aSide);
      uiTextC(sx + sw / 2, iy + mh / 2 - uiLineH(2), mk[i], uiFontFit(mk[i], iw - 8, 3), rgb565(240,244,252), aSide);
      iy += mh + pad / 2;
    }
    iy += pad;
    uiTextC(sx + sw / 2, iy, "Resultado", uiFontFit("Resultado", iw, 2), rgb565(150,160,190), aSide);
    iy += uiLineH(2) + 4;
    uiTextC(sx + sw / 2, iy, calcDisp, uiFontFit(calcDisp, iw, 3), rgb565(200,230,255), aSide);
  }
  if(!host){ setBuf(fb); fbCopyBand(lockBuf, WIN_TOP, WIN_BOT - 1); }
  flxFlush(WIN_TOP, WIN_BOT);
}
static void calcRenderDisplay(){                       // solo el display (al teclear) -> responsivo
  // Dos dibujos SEPARADOS directo en
  // fb (fillRoundRect + drawTextR) antes de un solo flxFlush. Como esta
  // funcion se llama en CADA tecla tocada, era el candidato mas probable
  // para el "parpadeo al hacer algo en una app" -- se nota mucho mas que
  // el tirador porque pasa constantemente, no cada 120ms. Se compone en
  // lockBuf (igual que calcRender() ya hace) y se vuelca de una pasada.
  bool host = gHosted;
  int dx, dy, dw, dh; calcDispRect(dx, dy, dw, dh);
  if(dh <= 0) return;                            // lienzo sin sitio para el display
  int y0 = dy - 2, y1 = dy + dh + 2;
  setBuf(host ? fb : lockBuf);
  fillRect(dx - 2, y0, dw + 4, y1 - y0, WIN_BG);  // borra el valor anterior
  int drad = dh / 5; if(drad > 14) drad = 14; if(drad < 2) drad = 2;
  fillRoundRect(dx, dy, dw, dh, drad, rgb565(28,31,40));
  int dfs = dh >= 90 ? 5 : dh >= 64 ? 4 : dh >= 40 ? 3 : 2;
  while(dfs > 1 && textW(calcDisp, dfs) > dw - 20) dfs--;
  drawTextR(dx + dw - 10, dy + dh / 2 - dfs * 4, calcDisp, dfs, rgb565(255,255,255));
  if(!host){ fbCopyBand(lockBuf, y0, y1); setBuf(fb); }
  flxFlush(y0, y1);
}
static void calcEnter(){
  if(!gRelayout){ strcpy(calcDisp, "0"); calcAcc = 0; calcOp = 0; calcFresh = true; }
  calcRender();                                   // re-maquetado: conserva el display
}
static void calcTick(){
  if(!T.tap) return;
  int gx, gy, bw, bh, gap; calcGrid(gx, gy, bw, bh, gap);
  for(int r = 0; r < 5; r++) for(int c = 0; c < 4; c++){
    int x = gx + c * (bw + gap), y = gy + r * (bh + gap);
    if(T.x >= x && T.x <= x + bw && T.y >= y && T.y <= y + bh){
      calcKeyFromLabel(CALC_LBL[r][c]); calcRenderDisplay(); return;
    }
  }
}

// #############################################################
// ##  APPS M2: Calendario, Bienestar, Galeria (marco estandar)
// #############################################################

// ---- Calendario: vista de mes con el dia de hoy resaltado ----
// CALENDARIO · adaptativo.
//   Esencial   : rejilla del mes; celda y fuente escalan con el lienzo y la
//                rejilla siempre cabe entera (6 filas posibles).
//   Opcional 1 : panel lateral "Hoy" con el dia grande y la fecha larga --
//                aparece cuando el lienzo pasa de 430 px de ancho, que es lo
//                que necesita la rejilla (>= 28 px por celda) mas el panel
//                (>= 150 px) sin apretar ninguno de los dos.
static void calRender(){
  setBuf(fb);
  int bx, by, bw, bh; uiBox(bx, by, bw, bh);
  fillRect(bx, by, bw, bh, WIN_BG);
  int pad = uiPad(), gap = uiGap();
  char hdr[40]; snprintf(hdr, sizeof(hdr), "%s %d", MO_FULL[LI()][rtcMo - 1], rtcY);

  // El panel lateral solo si quedan >= 28 px por celda para la rejilla.
  int sideW = bw / 3; if(sideW > 210) sideW = 210;
  uint8_t aSide = uiSection(0, bw >= 430 && (bw - sideW - gap - 2 * pad) / 7 >= 28);
  int gridW = aSide ? (bw - sideW - gap - 2 * pad) : (bw - 2 * pad);

  int y = by + pad;
  int fsH = uiFontFit(hdr, gridW, uiFontH(bh / 12));
  drawTextC(bx + pad + gridW / 2, y, hdr, fsH, rgb565(255,255,255));
  y += uiLineH(fsH) + gap / 2;

  const char* wd[7] = { "D", "L", "M", "M", "J", "V", "S" };
  int cw = gridW / 7;
  int fsW = uiFontFit("W", cw - 2, 2);
  for(int i = 0; i < 7; i++) drawTextC(bx + pad + i * cw + cw / 2, y, wd[i], fsW, rgb565(150,160,190));
  y += uiLineH(fsW) + gap / 2;

  int fw = ((rtcWd - (rtcD - 1)) % 7 + 7) % 7;
  int dim = daysInMonth(rtcY, rtcMo);
  int rows = (fw + dim + 6) / 7; if(rows < 1) rows = 1;
  int availH = (by + bh) - y - pad;
  int ch = availH / (rows > 0 ? rows : 1);
  if(ch < 12) ch = 12;
  int fsD = uiFontH(ch * 2 / 3);
  int rad = (cw < ch ? cw : ch) / 2 - 2; if(rad < 6) rad = 6;
  for(int d = 1; d <= dim; d++){
    int cell = fw + d - 1, r = cell / 7, c = cell % 7;
    int cx = bx + pad + c * cw + cw / 2, cy = y + r * ch;
    if(cy + ch > by + bh) break;                       // nunca fuera del marco
    if(d == rtcD) fillCircle(cx, cy + ch / 2, rad, rgb565(58,120,235));
    char ds[4]; snprintf(ds, sizeof(ds), "%d", d);
    drawTextC(cx, cy + ch / 2 - uiLineH(fsD) / 2, ds, fsD, d == rtcD ? rgb565(255,255,255) : rgb565(220,224,235));
  }
  if(aSide){
    int sx = bx + pad + gridW + gap, sy = by + pad;
    int shh = bh - 2 * pad;
    uiRectA(sx, sy, sideW, shh, pad, rgb565(30,34,46), aSide);
    uiTextC(sx + sideW / 2, sy + pad, "Hoy", uiFontFit("Hoy", sideW - 16, 3), rgb565(150,160,190), aSide);
    char dd[8]; snprintf(dd, sizeof(dd), "%d", rtcD);
    int fsBig = uiFontFit(dd, sideW - 24, uiFontH(shh / 3));
    uiTextC(sx + sideW / 2, sy + shh / 2 - uiLineH(fsBig), dd, fsBig, rgb565(120,200,255), aSide);
    char ld[64]; buildLongDate(ld, sizeof(ld));
    uiTextC(sx + sideW / 2, sy + shh / 2 + uiLineH(2), ld,
            uiFontFit(ld, sideW - 16, 2), rgb565(210,218,235), aSide);
  }
  flxFlush(WIN_TOP, WIN_BOT);
}
static void calEnter(){ calRender(); }
static void calTick(){ if(gMinChanged) calRender(); }

// ---- Bienestar: tiempo encendido + uso de memoria ----
// BIENESTAR · adaptativo.
//   Esencial   : tiempo encendido + barra de PSRAM.
//   Opcional 1 : columna derecha con RAM interna y frecuencia -- aparece
//                cuando el lienzo pasa de 400 px de ancho (dos columnas de
//                >= 190 px, que es lo minimo para que la etiqueta y el valor
//                no se pisen).
//   Opcional 2 : pie de consejo -- aparece si sobran >= 20 px al fondo.
static void bienRender(){
  setBuf(fb);
  int bx0, by, bw0, bh; uiBox(bx0, by, bw0, bh);
  fillRect(bx0, by, bw0, bh, WIN_BG);
  int pad = uiPad(), gap = uiGap();
  int y = by + pad;
  y = uiTitle(bx0, y, bw0, "Bienestar del equipo", rgb565(255,255,255), uiFontH(bh / 12));
  char up[40]; buildUptime(up, sizeof(up));
  int fsUp = uiFontFit(up, bw0 - 2 * pad, uiFontH(bh / 6));
  drawTextC(bx0 + bw0 / 2, y, up, fsUp, rgb565(120,200,255));
  y += uiLineH(fsUp) + 2;
  drawTextC(bx0 + bw0 / 2, y, "tiempo encendido", uiFontFit("tiempo encendido", bw0 - 2 * pad, 2), rgb565(150,160,185));
  y += uiLineH(2) + gap;

  size_t pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM), pt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  int usedp = pt > 0 ? (int)(100 - (uint64_t)pf * 100 / pt) : 0;
  char v[40];
  uint8_t aCol = uiSection(0, bw0 >= 400);
  int colW = aCol ? (bw0 - 2 * pad - gap) / 2 : (bw0 - 2 * pad);
  int cx1 = bx0 + pad;
  int barH = bh / 16; if(barH < 10) barH = 10; if(barH > 20) barH = 20;
  int fsL = uiFontFit("RAM interna libre", colW / 2, 2);
  drawText(cx1, y, "PSRAM", fsL, rgb565(220,224,235));
  snprintf(v, sizeof(v), "%d%% en uso", usedp);
  drawTextR(cx1 + colW, y, v, fsL, rgb565(180,188,205));
  int barY = y + uiLineH(fsL) + 6;
  fillRoundRect(cx1, barY, colW, barH, barH / 2, rgb565(48,52,66));
  fillRoundRect(cx1, barY, colW * usedp / 100, barH, barH / 2, rgb565(90,180,120));
  if(aCol){
    int cx2 = cx1 + colW + gap;
    uiText(cx2, y, "RAM interna", fsL, rgb565(220,224,235), aCol);
    snprintf(v, sizeof(v), "%u KB", (unsigned)(esp_get_free_heap_size() / 1024));
    uiTextR(cx2 + colW, y, v, fsL, rgb565(180,188,205), aCol);
    int fr = (int)getCpuFrequencyMhz();
    uiRectA(cx2, barY, colW, barH, barH / 2, rgb565(48,52,66), aCol);
    int pctF = fr > 360 ? 100 : fr * 100 / 360;
    uiRectA(cx2, barY, colW * pctF / 100, barH, barH / 2, rgb565(120,150,240), aCol);
    snprintf(v, sizeof(v), "CPU %d MHz", fr);
    uiText(cx2, barY + barH + 6, v, uiFontFit(v, colW, 2), rgb565(160,170,195), aCol);
  }
  y = barY + barH + uiLineH(2) + gap;
  uint8_t aFoot = uiSection(1, (by + bh) - y - pad >= 20);
  if(aFoot){
    const char* tip = "Recuerda descansar la vista";
    uiTextC(bx0 + bw0 / 2, by + bh - pad - uiLineH(2), tip,
            uiFontFit(tip, bw0 - 2 * pad, 2), rgb565(150,160,185), aFoot);
  }
  flxFlush(WIN_TOP, WIN_BOT);
}
static void bienEnter(){ bienRender(); }
static void bienTick(){ if(gMinChanged) bienRender(); }

// ---- Galeria: cuadricula de miniaturas (mini-paisajes generados) ----
// GALERIA · adaptativa.
//   Esencial   : rejilla de miniaturas. El NUMERO DE COLUMNAS se calcula con el
//                ancho real (miniatura minima legible de 92 px), asi que al
//                ensanchar la ventana no queda hueco: entran mas columnas.
//   Opcional 1 : pie con el recuento de elementos -- aparece si sobran >= 18 px.
static void galRender(){
  setBuf(fb);
  int bx, by, bw, bh; uiBox(bx, by, bw, bh);
  fillRect(bx, by, bw, bh, WIN_BG);
  int pad = uiPad(), gap = uiGap();
  int y0 = by + pad;
  const char* ttl = "Galer\xC3\xAD" "a";
  int fsT = uiFontFit(ttl, bw - 2 * pad, uiFontH(bh / 12));
  drawTextC(bx + bw / 2, y0, ttl, fsT, rgb565(255,255,255));
  int gy = y0 + uiLineH(fsT) + gap;
  int cols = (bw - 2 * pad + gap) / (92 + gap); if(cols < 2) cols = 2; if(cols > 6) cols = 6;
  int tw = (bw - 2 * pad - (cols - 1) * gap) / cols;
  int th = tw * 3 / 4;
  int gx = bx + pad;
  uint16_t sky[6] = { rgb565(120,180,235), rgb565(250,200,120), rgb565(180,150,220),
                      rgb565(120,210,190), rgb565(240,160,170), rgb565(150,170,235) };
  uint16_t sun[6] = { rgb565(255,240,150), rgb565(255,120,80), rgb565(255,230,180),
                      rgb565(255,255,210), rgb565(255,210,120), rgb565(255,245,190) };
  uint16_t mtn[6] = { rgb565(60,110,90), rgb565(120,80,60), rgb565(80,70,110),
                      rgb565(50,110,110), rgb565(120,70,90), rgb565(70,90,130) };
  int rowsFit = ((by + bh) - gy - pad + gap) / (th + gap); if(rowsFit < 1) rowsFit = 1;
  int shown = cols * rowsFit; if(shown > 12) shown = 12;
  int rad = tw / 10; if(rad < 3) rad = 3;
  for(int i = 0; i < shown; i++){
    int c = i % cols, r = i / cols, x = gx + c * (tw + gap), y = gy + r * (th + gap), k = i % 6;
    fillRoundRect(x, y, tw, th, rad, sky[k]);
    fillCircle(x + tw - tw / 5, y + th / 5, tw / 9 + 1, sun[k]);
    fillTriangle(x + tw / 20, y + th - th / 20, x + tw / 2 - tw / 14, y + th - th * 2 / 5,
                 x + tw - tw / 5, y + th - th / 20, mtn[k]);
    fillTriangle(x + tw / 2, y + th - th / 20, x + tw - tw / 6, y + th - th / 3,
                 x + tw - tw / 20, y + th - th / 20, mix565(mtn[k], rgb565(0,0,0), 60));
  }
  int fy = gy + ((shown + cols - 1) / cols) * (th + gap);
  uint8_t aFoot = uiSection(0, (by + bh) - fy - pad >= 18);
  if(aFoot){
    char cnt[40]; snprintf(cnt, sizeof(cnt), "%d de 12 elementos", shown);
    uiTextC(bx + bw / 2, by + bh - pad - uiLineH(2), cnt,
            uiFontFit(cnt, bw - 2 * pad, 2), rgb565(150,160,185), aFoot);
  }
  flxFlush(WIN_TOP, WIN_BOT);
}
static void galEnter(){ galRender(); }

// #############################################################
// ##  MODO PC  (Milestone 4)  ·  Samsung DeX STANDALONE
// ##  Escritorio LANDSCAPE 800x480 dibujado rotado sobre el panel
// ##  portrait (gLand). Clon del DeX que corre en la PROPIA pantalla
// ##  de las Galaxy Tab S: aqui NO hay salida a monitor externo,
// ##  cable DeX/HDMI ni casting -- todo vive en este panel.
// ##
// ##  Mapa del subsistema (prefijo dex*; los pc* que ya existian se
// ##  conservan como puntos de entrada de APP_REG):
// ##    · Geometria .............. dexWorkBottom/dexTbY/dexSnapRect
// ##    · Animador interrumpible . dexAnimStart*/dexAnimCur/dexAnimTick
// ##    · Ventanas ............... dexDrawWindow/dexOpenFrom/dexCloseWin/
// ##                               dexMinimize/dexRestore/dexApplySnap
// ##    · Barra de tareas ........ dexTbLayout/dexTaskbar (+auto-ocultar)
// ##    · Cajon de apps / Finder . dexDrawerDraw/dexFinderDraw
// ##    · Notificaciones ......... dexNotifDraw (cortina)
// ##    · Recientes .............. dexRecentsDraw
// ##    · Menu contextual ........ dexMenuDraw/dexMenuRun
// ##    · Touchpad virtual ....... dexPadDraw/dexCursorDraw
// ##    · Composicion + banda .... dexCompose/dexPaint/pcRender
// ##    · Entrada ................ dexPointer/dexInput/pcTick
// #############################################################

// ---- Geometria ----
#define DEX_TB_H       58          // alto de la barra de tareas
#define DEX_TTL_H      38          // alto de la barra de titulo de ventana
#define DEX_BTN_W      38          // ancho de cada control (min/max/cerrar).
                                   // Los controles se AGRANDAN de verdad en vez de
                                   // que su hitbox invada al vecino: ensanchar la
                                   // zona de toque hacia los lados hacia que un
                                   // toque cerca de "cerrar" activara "maximizar".
#define DEX_MIN_W      240         // tamano minimo de ventana
#define DEX_MIN_H      140
#define DEX_GRIP       22          // margen de agarre EXTERIOR para redimensionar.
                                   // Con un dedo sobre un panel capacitivo, 14 px
                                   // fuera del borde eran casi imposibles de
                                   // acertar; 22 da un blanco realista sin robarle
                                   // area util a la ventana (hacia DENTRO sigue
                                   // siendo minimo, ver dexResizeMask).
#define DEX_ANIM_MS    180         // duracion base de las animaciones
#define DEX_TB_ANIM    150         // slide de auto-ocultar la barra
#define DEX_LONG_MS    560         // mantener pulsado -> menu contextual.
                                   // >550 A PROPOSITO: tDoRelease() solo marca
                                   // T.tap por debajo de 550 ms, asi que una
                                   // pulsacion larga NUNCA dispara ademas un tap.
#define DEX_DTAP_MS    400         // ventana del doble toque
#define DEX_SNAP_EDGE  12          // franja de borde que dispara el snap
#define DEX_TB_IDLE_MS 1800        // inactividad antes de auto-ocultar la barra
#define DEX_TB_REVEAL  18          // franja inferior que vuelve a mostrar la barra
#define DEX_PAD_W      210         // touchpad virtual
#define DEX_PAD_H      140
// Tamano de los pop-ups. Viven aqui arriba porque dexOvMark() los necesita para
// calcular la banda sucia de cada overlay, bastante antes de donde se dibujan.
#define DEX_DRW_W      672         // cajon de apps
#define DEX_DRW_H      404
#define DEX_FND_W      576         // buscador tipo Finder
#define DEX_FND_H      404
#define DEX_NP_W       360         // panel de notificaciones / ajustes rapidos

// ---- Paleta. Sigue el MISMO gDark del resto de FlexOS Ultra: DeX no tiene
//      tema propio, usa el del sistema (requisito explicito). ----
#define DEX_ACCENT   rgb565(60,130,246)
#define DEX_TB_BG    (gDark ? rgb565(16,19,28)    : rgb565(240,243,249))
#define DEX_TB_GLASS (gDark ? rgb565(20,25,38)    : rgb565(246,248,252))
#define DEX_PANEL    (gDark ? rgb565(30,35,48)    : rgb565(252,253,255))
#define DEX_PANEL2   (gDark ? rgb565(42,48,64)    : rgb565(234,238,246))
#define DEX_TXT_HI   (gDark ? rgb565(238,241,249) : rgb565(24,27,36))
#define DEX_TXT_LO   (gDark ? rgb565(150,157,175) : rgb565(110,116,132))
#define DEX_BORDER   (gDark ? rgb565(66,74,94)    : rgb565(202,210,224))
#define DEX_WIN_BG   (gDark ? rgb565(30,34,46)    : rgb565(250,251,253))
#define DEX_WIN_BODY (gDark ? rgb565(24,27,37)    : rgb565(252,253,255))
#define DEX_TTL_ACT  (gDark ? rgb565(54,62,82)    : rgb565(224,230,240))
#define DEX_TTL_INA  (gDark ? rgb565(38,43,58)    : rgb565(240,242,247))

enum { SNAP_FREE = 0, SNAP_L, SNAP_R, SNAP_TL, SNAP_TR, SNAP_BL, SNAP_BR, SNAP_MAX };
enum { DXA_NONE = 0, DXA_OPEN, DXA_CLOSE, DXA_MIN, DXA_RESTORE, DXA_GEOM };
enum { DXO_NONE = 0, DXO_DRAWER, DXO_FINDER, DXO_NOTIF, DXO_RECENTS };
enum { DXG_NONE = 0, DXG_MOVE, DXG_RESIZE };

static PWin pwins[4];
static bool pcStartOpen = false;              // se conserva (lo tocaba el menu Inicio viejo)

static uint8_t dexOrder[4] = { 0, 1, 2, 3 };  // z-order: [0] atras ... [3] al frente
static int     dexFocus = -1;                 // ventana activa (indice en pwins)

static bool  dexTbAuto   = false;             // auto-ocultar la barra de tareas
static bool  dexBigIcons = false;             // iconos grandes en la barra
static float dexTbOff    = 0;                 // 0 = visible, DEX_TB_H = oculta
static int   dexTbTgt    = 0;
static float dexTbFrom   = 0;
static uint32_t dexTbT0  = 0;
static uint32_t dexTbIdle = 0;
static uint8_t dexWall   = 0;                 // variante de fondo de DeX (0..2)

static uint8_t  dexOv = DXO_NONE;             // overlay activo
static bool     dexOvClosing = false;
static bool     dexOvDone = false;            // el overlay ya termino de abrirse
static uint32_t dexOvT0 = 0;

static bool dexExiting = false;               // se pidio salir de Modo PC (ver pcExit)
// Fondo de DeX cacheado. El degradado se pintaba con hLine por fila LOGICA, y en
// landscape eso son 384.000 putPhys por frame, cada uno en una linea de cache
// distinta de la PSRAM: por si solo se comia el presupuesto de frame entero.
// Cacheado, el fondo de cada frame es un memcpy por fila fisica.
static uint16_t* dexBg = NULL;
static uint8_t   dexBgWall = 0xFF;            // variante ya cacheada
static bool      dexBgDark = false;           // gDark con el que se cacheo

static char dexQuery[20] = { 0 };             // texto del buscador (cajon y Finder)
static int  dexQLen = 0;

static bool    dexMenuOn = false;             // menu contextual
static uint8_t dexMenuKind = 0;               // 0 escritorio · 1 barra · 2 barra de titulo
static int     dexMenuX = 0, dexMenuY = 0, dexMenuWin = -1;

static bool dexPadOn = false;                 // touchpad virtual (mejora condicional)
static int  dexCurX = LW / 2, dexCurY = LH / 2;
static bool dexPadGrab = false;
static int  dexPadLX = 0, dexPadLY = 0;

// Animador: UNA sola ranura. Con pwins[4] no hay dos ventanas animando a la vez
// en la practica; si se pide otra, la anterior se cierra en su estado FINAL
// (dexAnimFinish) -- nunca queda una a medias.
static uint8_t  dexAK = DXA_NONE;
static int      dexAW = -1;
static int      dexAF[4], dexAT[4];           // rect origen / destino (x,y,w,h)
static uint32_t dexAT0 = 0, dexADur = DEX_ANIM_MS;

static uint8_t dexGrab = DXG_NONE;            // arrastre / redimension
static int     dexGrabWin = -1, dexGrabDX = 0, dexGrabDY = 0;
static uint8_t dexRzMask = 0;                 // bit0 izq · bit1 der · bit2 arriba · bit3 abajo
static int     dexRzX0, dexRzY0, dexRzW0, dexRzH0;
static uint8_t dexSnapGhost = SNAP_FREE;      // contorno fantasma del snap
static int     dexRecDrag = -1;               // tarjeta de Recientes que se arrastra
static int     dexRecDY = 0, dexRecY0 = 0;

// Banda sucia en X LOGICA. La rotacion de gLand mapea lx -> fila FISICA
// (putPhys: y = lx), asi que un rango de lx es exactamente un rango de filas del
// panel: es el UNICO eje por el que se puede acotar el volcado. Por eso arrastrar
// una ventana solo sube su franja de columnas en vez de los 800 px.
static int  dexBX0 = 0, dexBX1 = LW - 1;
static bool dexDirty = true;
static uint32_t dexFrameMs = 0;

// Puntero normalizado: sale del tactil directo o del touchpad virtual, de modo
// que TODA la logica de abajo es identica con y sin touchpad.
static int  pX = 0, pY = 0;
static bool pDown = false, pPressed = false, pReleased = false;
static bool pTap = false, pLong = false, pDTap = false;
static bool dexLongFired = false;
static uint32_t dexTapMs = 0;
static int  dexTapX = 0, dexTapY = 0;
static int  dexPressX = 0, dexPressY = 0;   // origen del gesto en curso

static const uint8_t DEX_PIN[6] = { IC_NAV, IC_NOTAS, IC_CALC, IC_AJUSTES, IC_ALMACEN, IC_GALERIA };
#define DEX_PINN 6

// Entradas de Ajustes que indexa el buscador tipo Finder. Solo apps y ajustes:
// este OS no tiene agenda de contactos y fabricar una lista falsa iria contra el
// criterio del resto del archivo (por eso mismo se quitaron los toggles de
// Wi-Fi/BT del panel rapido: no habia radio real detras).
static const char* DEX_SET[8] = { "Pantalla", "Sonido", "Red e Internet", "Bateria",
                                  "Aplicaciones", "Almacenamiento", "Seguridad", "Acerca de" };
static const char* DEX_KB[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };

// Rects calculados por dexTbLayout() y reutilizados por DIBUJO y TACTIL, para que
// la geometria viva en un solo sitio. Formato { x, y, w, h }.
static int dexRDrw[4], dexRFnd[4], dexRPad[4], dexRBell[4], dexRGear[4], dexRBat[4];
static int dexRClkX = 0, dexRWifiX = 0;

static void pcRender();
static void pcExit();
static void dexOpenFrom(int app, int sx, int sy, int ss);
static void dexOpen(int app);
// Hosting de apps reales en ventanas (definido mas abajo; se usa desde el ciclo
// de vida de las ventanas, que va antes).
static void dexHostOpen(int i);
static void dexHostClose(int i);
static void dexHostRun(int i, bool doEnter, bool doTick, const Touch* inject);
static void dexHostServe(int i);
static void dexClientRect(int i, int &cx, int &cy, int &cw, int &ch);
static void dexHostMinSize(int app, int &mw, int &mh);
static void dexHostDefaultSize(int app, int &w, int &h);

// -------------------------------------------------------------
//  Utilidades
// -------------------------------------------------------------
static inline bool dexIn(int x, int y, const int* r){
  return x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
}
static inline bool dexInBox(int x, int y, int bx, int by, int bw, int bh){
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}
static inline float dexEase(float t){ t = 1.0f - t; return 1.0f - t * t * t; }   // ease-out cubico

static void dexMark(int x0, int x1){
  if(x0 < dexBX0) dexBX0 = x0;
  if(x1 > dexBX1) dexBX1 = x1;
}
static inline void dexMarkAll(){ dexBX0 = 0; dexBX1 = LW - 1; }
static inline void dexMarkWin(int x, int w){ dexMark(x - 10, x + w + 10); }

// gClipY0/gClipY1 son, en landscape, la banda de X LOGICA que se esta pintando.
// dexCull descarta de golpe lo que cae fuera; dexBand recorta un tramo horizontal
// a la banda. Sin esto el bucle de cada hLine recorreria los 800 px igualmente
// (putPhys rechaza pixel a pixel, pero el bucle se paga entero).
static inline int dexBandLo(){ return gClipY0 < 0 ? 0 : gClipY0; }
static inline int dexBandHi(){ return gClipY1 > LW - 1 ? LW - 1 : gClipY1; }
static inline bool dexCull(int x, int w){ return (x + w < dexBandLo()) || (x > dexBandHi()); }
static bool dexBand(int &x, int &w){
  int b0 = dexBandLo(), b1 = dexBandHi();
  if(x < b0){ w -= (b0 - x); x = b0; }
  if(x + w > b1 + 1) w = b1 + 1 - x;
  return w > 0;
}

// Texto recortado con "..": las primitivas solo recortan contra el lienzo logico,
// no contra la ventana, asi que un nombre largo en una ventana estrecha se
// saldria por el borde. Corta SIN partir una secuencia UTF-8.
static void dexTextFit(int x, int y, const char* s, int size, uint16_t col, int maxw){
  if(maxw <= 0) return;
  if(textW(s, size) <= maxw){ drawText(x, y, s, size, col); return; }
  char b[48];
  int n = 0; while(s[n] && n < (int)sizeof(b) - 3) n++;
  for(int len = n; len > 0; len--){
    if(((uint8_t)s[len] & 0xC0) == 0x80) continue;      // no cortar a mitad de un caracter
    memcpy(b, s, len); b[len] = '.'; b[len + 1] = '.'; b[len + 2] = 0;
    if(textW(b, size) <= maxw){ drawText(x, y, b, size, col); return; }
  }
}
static void dexTextFitC(int cx, int y, const char* s, int size, uint16_t col, int maxw){
  if(textW(s, size) <= maxw){ drawTextC(cx, y, s, size, col); return; }
  dexTextFit(cx - maxw / 2, y, s, size, col, maxw);
}
static inline char dexLower(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
// Subcadena sin distinguir mayusculas (byte a byte: las consultas se teclean en
// ASCII, los acentos del nombre simplemente no casan).
static bool dexMatch(const char* name, const char* q, int qn){
  if(qn <= 0) return true;
  for(int i = 0; name[i]; i++){
    int k = 0;
    while(k < qn && name[i + k] && dexLower(name[i + k]) == dexLower(q[k])) k++;
    if(k == qn) return true;
  }
  return false;
}
static int dexFilterApps(int* out, int maxn){
  int n = 0;
  for(int i = 0; i < 16 && n < maxn; i++) if(dexMatch(appName(i), dexQuery, dexQLen)) out[n++] = i;
  return n;
}

// -------------------------------------------------------------
//  Geometria del escritorio
// -------------------------------------------------------------
static inline int dexTbY(){ return LH - DEX_TB_H + (int)dexTbOff; }
// Con auto-ocultar el area util llega abajo del todo (la barra se superpone al
// aparecer); sin el, termina justo encima de la barra.
static inline int dexWorkBottom(){ return dexTbAuto ? LH : LH - DEX_TB_H; }

static void dexSnapRect(uint8_t s, int &x, int &y, int &w, int &h){
  int H = dexWorkBottom(), W = LW;
  switch(s){
    case SNAP_L:  x = 0;     y = 0;     w = W / 2; h = H;     break;
    case SNAP_R:  x = W / 2; y = 0;     w = W / 2; h = H;     break;
    case SNAP_TL: x = 0;     y = 0;     w = W / 2; h = H / 2; break;
    case SNAP_TR: x = W / 2; y = 0;     w = W / 2; h = H / 2; break;
    case SNAP_BL: x = 0;     y = H / 2; w = W / 2; h = H / 2; break;
    case SNAP_BR: x = W / 2; y = H / 2; w = W / 2; h = H / 2; break;
    default:      x = 0;     y = 0;     w = W;     h = H;     break;   // SNAP_MAX
  }
}
// Que anclaje sugiere la posicion del puntero (estilo Aero Snap).
static uint8_t dexSnapHit(int lx, int ly){
  int H = dexWorkBottom(), cw = 170, ch = 110;
  if(ly <= DEX_SNAP_EDGE){
    if(lx <= cw) return SNAP_TL;
    if(lx >= LW - cw) return SNAP_TR;
    return SNAP_MAX;
  }
  if(lx <= DEX_SNAP_EDGE){
    if(ly <= ch) return SNAP_TL;
    if(ly >= H - ch) return SNAP_BL;
    return SNAP_L;
  }
  if(lx >= LW - DEX_SNAP_EDGE){
    if(ly <= ch) return SNAP_TR;
    if(ly >= H - ch) return SNAP_BR;
    return SNAP_R;
  }
  if(ly >= H - DEX_SNAP_EDGE){
    if(lx <= cw) return SNAP_BL;
    if(lx >= LW - cw) return SNAP_BR;
  }
  return SNAP_FREE;
}

// px de ventana que quedan SIEMPRE agarrables dentro del area util.
#define DEX_KEEP 96
// Unica puerta de saneado de la geometria de una ventana: la usan la apertura,
// el arrastre, el resize, el fin de animacion y el cambio de area util.
static void dexClampWin(PWin* wn){
  int mw = DEX_MIN_W, mh = DEX_MIN_H;
  // El suelo de legibilidad solo se impone cuando la ventana es libre. Si esta
  // anclada (mitad, cuadrante, maximizada) manda el anclaje: el letterbox se
  // encarga de que siga sin deformarse, solo se ve mas pequena.
  if(wn->snap == SNAP_FREE) dexHostMinSize(wn->app, mw, mh);
  flxClampRect(wn->x, wn->y, wn->w, wn->h, LW, dexWorkBottom(), mw, mh, DEX_KEEP);
}
// El area util cambia de alto al activar/desactivar el auto-ocultar de la barra
// (dexWorkBottom). Sin re-encajar, una ventana colocada abajo con la barra
// oculta se quedaba DEBAJO de la barra al volver a fijarla: sin barra de titulo
// accesible, no habia forma de moverla ni cerrarla salvo por Recientes.
static void dexClampAll(){
  int H = dexWorkBottom();
  for(int i = 0; i < 4; i++){
    if(!pwins[i].open) continue;
    if(pwins[i].snap != SNAP_FREE)
      dexSnapRect(pwins[i].snap, pwins[i].x, pwins[i].y, pwins[i].w, pwins[i].h);
    else dexClampWin(&pwins[i]);
    flxClampRect(pwins[i].rx, pwins[i].ry, pwins[i].rw, pwins[i].rh,
                 LW, H, DEX_MIN_W, DEX_MIN_H, DEX_KEEP);   // tambien el rect de restaurar
  }
  dexMarkAll(); dexDirty = true;
}

static int dexTopWin(){                        // ventana visible mas al frente
  for(int k = 3; k >= 0; k--){ int i = dexOrder[k]; if(pwins[i].open && !pwins[i].mini) return i; }
  return -1;
}
static void dexRaise(int i){
  if(i < 0 || i > 3) return;
  int at = -1; for(int k = 0; k < 4; k++) if(dexOrder[k] == i){ at = k; break; }
  if(at >= 0) for(int k = at; k < 3; k++) dexOrder[k] = dexOrder[k + 1];
  dexOrder[3] = (uint8_t)i;
  dexFocus = i;
  dexMarkAll();
}

// -------------------------------------------------------------
//  Barra de tareas: geometria (un solo sitio para dibujo y tactil)
// -------------------------------------------------------------
static inline int dexTbIconS(){ return dexBigIcons ? 40 : 32; }
static inline int dexTbStep(){ return dexTbIconS() + 20; }

// Lista de la barra: primero las FIJADAS, despues las abiertas que no lo esten.
static int dexTbItems(int* app, bool* open){
  int n = 0;
  for(int i = 0; i < DEX_PINN; i++){ app[n] = DEX_PIN[i]; open[n] = false; n++; }
  for(int i = 0; i < 4; i++) if(pwins[i].open){
    bool found = false;
    for(int k = 0; k < n; k++) if(app[k] == pwins[i].app){ open[k] = true; found = true; break; }
    if(!found && n < 10){ app[n] = pwins[i].app; open[n] = true; n++; }
  }
  return n;
}
// El grupo central va centrado en pantalla, pero NO puede invadir ni los iconos
// de la izquierda ni el bloque de estado de la derecha. Con las 6 fijadas mas
// hasta 4 ventanas abiertas, el ancho nominal se pasaba del hueco disponible y
// los iconos acababan pisando la campana y el reloj. Aqui el paso (y, si hace
// falta, el tamano del icono) se estrecha hasta caber, y el grupo se desplaza
// solo cuando ya no puede seguir centrado.
static void dexTbGroup(int n, int &x0, int &step, int &s){
  int lft = dexRFnd[0] + dexRFnd[2] + 16;
  int rgt = dexRPad[0] - 16;
  int avail = rgt - lft; if(avail < 80) avail = 80;
  step = dexTbStep();
  if(n > 0 && n * step > avail) step = avail / n;
  s = dexTbIconS();
  if(step < s + 4){ s = step - 4; if(s < 18) s = 18; }
  int tot = n * step;
  x0 = LW / 2 - tot / 2;
  if(x0 + tot > rgt) x0 = rgt - tot;
  if(x0 < lft) x0 = lft;
}
static void dexTbItemRect(int idx, int n, int &x, int &y, int &s){
  int x0, step; dexTbGroup(n, x0, step, s);
  x = x0 + idx * step + (step - s) / 2;
  y = dexTbY() + (DEX_TB_H - s) / 2 - 4;
}
// Rect del icono de una app en la barra: ORIGEN/DESTINO de las animaciones de
// abrir, minimizar y restaurar.
static bool dexTbAppRect(int appId, int &x, int &y, int &s){
  int app[10]; bool op[10];
  int n = dexTbItems(app, op);
  for(int i = 0; i < n; i++) if(app[i] == appId){ dexTbItemRect(i, n, x, y, s); return true; }
  s = dexTbIconS(); x = LW / 2 - s / 2; y = dexTbY() + (DEX_TB_H - s) / 2 - 3;
  return false;
}
static void dexTbLayout(){
  int ty = dexTbY(), h = DEX_TB_H;
  const int LS = 34, RS = 30;                       // iconos: izquierda / estado
  dexRDrw[0] = 12; dexRDrw[1] = ty + (h - LS) / 2; dexRDrw[2] = LS; dexRDrw[3] = LS;
  dexRFnd[0] = 12 + LS + 10; dexRFnd[1] = ty + (h - LS) / 2; dexRFnd[2] = LS; dexRFnd[3] = LS;
  char cs[12]; clkStrBar(cs, sizeof(cs));
  char sd[40]; buildShortDate(sd, sizeof(sd));
  int cw = textW(cs, 2), dw = textW(sd, 1); if(dw > cw) cw = dw;
  int rx = LW - 16;
  dexRClkX = rx;  rx -= cw + 18;
  dexRBat[0] = rx - 30; dexRBat[1] = ty + (h - 15) / 2; dexRBat[2] = 30; dexRBat[3] = 15; rx -= 44;
  dexRWifiX = rx - 11; rx -= 32;
  int riy = ty + (h - RS) / 2;
  dexRGear[0] = rx - RS; dexRGear[1] = riy; dexRGear[2] = RS; dexRGear[3] = RS; rx -= RS + 8;
  dexRBell[0] = rx - RS; dexRBell[1] = riy; dexRBell[2] = RS; dexRBell[3] = RS; rx -= RS + 8;
  dexRPad[0]  = rx - RS; dexRPad[1]  = riy; dexRPad[2]  = RS; dexRPad[3]  = RS;
}

// -------------------------------------------------------------
//  Animador (interrumpible)
// -------------------------------------------------------------
static float dexAP(){
  if(dexAK == DXA_NONE) return 1.0f;
  uint32_t e = millis() - dexAT0;
  if(e >= dexADur) return 1.0f;
  return dexEase((float)e / (float)dexADur);
}
static inline bool dexAnimIs(int w){ return dexAK != DXA_NONE && dexAW == w; }
// Banda que barre la animacion: union del rect de origen y el de destino. Antes
// marcaba dexMarkAll() en CADA frame, asi que animar una ventana de 430 px
// obligaba a recomponer y volcar los 800 -- casi el doble de trabajo por frame.
static void dexAnimMark(){
  int x0 = dexAF[0] < dexAT[0] ? dexAF[0] : dexAT[0];
  int e1 = dexAF[0] + dexAF[2], e2 = dexAT[0] + dexAT[2];
  int x1 = e1 > e2 ? e1 : e2;
  dexMark(x0 - 12, x1 + 12);
}
static void dexAnimCur(int &x, int &y, int &w, int &h, uint8_t &a){
  float p = dexAP();
  x = dexAF[0] + (int)((dexAT[0] - dexAF[0]) * p + 0.5f);
  y = dexAF[1] + (int)((dexAT[1] - dexAF[1]) * p + 0.5f);
  w = dexAF[2] + (int)((dexAT[2] - dexAF[2]) * p + 0.5f);
  h = dexAF[3] + (int)((dexAT[3] - dexAF[3]) * p + 0.5f);
  float fa = (dexAK == DXA_CLOSE || dexAK == DXA_MIN) ? (1.0f - p)
           : (dexAK == DXA_GEOM ? 1.0f : p);
  int v = 40 + (int)(215 * fa); if(v > 255) v = 255; if(v < 0) v = 0;
  a = (uint8_t)v;
}
// Cierra la animacion en curso APLICANDO su estado final. Se llama al terminar y
// tambien al interrumpirla con otra ventana: nunca queda una a medio camino.
static void dexAnimFinish(){
  if(dexAK == DXA_NONE) return;
  int w = dexAW; uint8_t k = dexAK;
  dexAK = DXA_NONE; dexAW = -1;
  if(w < 0 || w > 3) return;
  if(k == DXA_CLOSE){
    pwins[w].open = false; pwins[w].mini = false;
    dexHostClose(w);                                // libera el lienzo de la app
    if(dexFocus == w) dexFocus = dexTopWin();
  } else if(k == DXA_MIN){
    pwins[w].mini = true;
    if(dexFocus == w) dexFocus = dexTopWin();
  } else {
    if(k == DXA_RESTORE) pwins[w].mini = false;
    pwins[w].x = dexAT[0]; pwins[w].y = dexAT[1];
    pwins[w].w = dexAT[2]; pwins[w].h = dexAT[3];
  }
  dexMark(dexAF[0] - 12, dexAF[0] + dexAF[2] + 12);   // solo lo que toco la animacion
  dexMark(dexAT[0] - 12, dexAT[0] + dexAT[2] + 12);
  if(w >= 0 && w <= 3) dexClampWin(&pwins[w]);        // el destino nunca queda fuera
  // Con Recientes abierto, cerrar o minimizar una ventana CAMBIA la lista, y las
  // tarjetas van centradas: todas se recolocan de golpe. Marcar solo la banda de
  // la animacion (que es la de la VENTANA, no la de las tarjetas) dejaba mitades
  // de tarjeta congeladas donde estaban antes -- la tarjeta fantasma. La lista
  // solo cambia aqui, y es un evento puntual, asi que se recompone entera.
  if(dexOv == DXO_RECENTS) dexMarkAll();
}
static void dexAnimStartFT(uint8_t kind, int win,
                           int fx, int fy, int fw, int fh,
                           int tx, int ty, int tw, int th, uint32_t dur){
  if(dexAK != DXA_NONE && dexAW != win) dexAnimFinish();
  dexAK = kind; dexAW = win;
  dexAF[0] = fx; dexAF[1] = fy; dexAF[2] = fw; dexAF[3] = fh;
  dexAT[0] = tx; dexAT[1] = ty; dexAT[2] = tw; dexAT[3] = th;
  dexAT0 = millis(); dexADur = dur ? dur : 1;
  dexAnimMark(); dexDirty = true;
}
// Version "hacia": el origen es el rect ACTUAL. Si esa misma ventana ya estaba
// animando se toma su rect INTERPOLADO de este instante, de modo que la nueva
// animacion continua desde donde iba en vez de reiniciar (requisito de "todas
// deben poder interrumpirse a medio camino").
static void dexAnimStartTo(uint8_t kind, int win, int tx, int ty, int tw, int th, uint32_t dur){
  int fx, fy, fw, fh;
  if(dexAnimIs(win)){ uint8_t a; dexAnimCur(fx, fy, fw, fh, a); }
  else { fx = pwins[win].x; fy = pwins[win].y; fw = pwins[win].w; fh = pwins[win].h; }
  dexAnimStartFT(kind, win, fx, fy, fw, fh, tx, ty, tw, th, dur);
}
static void dexAnimTick(){
  if(dexAK == DXA_NONE) return;
  dexAnimMark(); dexDirty = true;
  if(millis() - dexAT0 >= dexADur) dexAnimFinish();
}

// -------------------------------------------------------------
//  Ciclo de vida de las ventanas
// -------------------------------------------------------------
static void dexRestore(int i){
  if(i < 0 || i > 3 || !pwins[i].open) return;
  bool wasMin = pwins[i].mini;
  dexRaise(i);
  if(!wasMin){ dexDirty = true; return; }
  int ix, iy, is; dexTbAppRect(pwins[i].app, ix, iy, is);
  dexAnimStartFT(DXA_RESTORE, i, ix, iy, is, is,
                 pwins[i].x, pwins[i].y, pwins[i].w, pwins[i].h, DEX_ANIM_MS);
}
static void dexMinimize(int i){
  if(i < 0 || i > 3 || !pwins[i].open || pwins[i].mini) return;
  int ix, iy, is; dexTbAppRect(pwins[i].app, ix, iy, is);
  dexAnimStartTo(DXA_MIN, i, ix, iy, is, is, DEX_ANIM_MS);
}
static void dexCloseWin(int i){
  if(i < 0 || i > 3 || !pwins[i].open) return;
  int ix, iy, is; dexTbAppRect(pwins[i].app, ix, iy, is);
  dexAnimStartTo(DXA_CLOSE, i, ix, iy, is, is, DEX_ANIM_MS);
}
static void dexApplySnap(int i, uint8_t s){
  PWin* wn = &pwins[i];
  if(wn->snap == SNAP_FREE){ wn->rx = wn->x; wn->ry = wn->y; wn->rw = wn->w; wn->rh = wn->h; }
  wn->snap = s;
  int x, y, w, h; dexSnapRect(s, x, y, w, h);
  dexAnimStartTo(DXA_GEOM, i, x, y, w, h, DEX_ANIM_MS);
}
static void dexToggleMax(int i){
  PWin* wn = &pwins[i];
  if(wn->snap != SNAP_FREE){
    wn->snap = SNAP_FREE;
    dexAnimStartTo(DXA_GEOM, i, wn->rx, wn->ry, wn->rw, wn->rh, DEX_ANIM_MS);
  } else dexApplySnap(i, SNAP_MAX);
}
static void dexOpenFrom(int app, int sx, int sy, int ss){
  for(int i = 0; i < 4; i++) if(pwins[i].open && pwins[i].app == app){
    if(pwins[i].mini) dexRestore(i); else dexRaise(i);
    dexDirty = true; return;
  }
  int slot = -1;
  for(int i = 0; i < 4; i++) if(!pwins[i].open){ slot = i; break; }
  if(slot < 0){                                     // tope de pwins[4]: recicla la mas antigua
    slot = (int)dexOrder[0];
    if(slot < 0 || slot > 3) slot = 0;              // pwins[4]: nunca escribir fuera del array
    if(dexAnimIs(slot)) dexAnimFinish();            // no dejar una animacion apuntando al slot reciclado
    dexHostClose(slot);                             // y libera el lienzo del que se recicla
    pwins[slot].open = false; pwins[slot].mini = false;
  }
  int n = 0; for(int j = 0; j < 4; j++) if(pwins[j].open) n++;
  PWin* wn = &pwins[slot];
  wn->open = true; wn->mini = false; wn->app = app; wn->snap = SNAP_FREE;
  dexHostDefaultSize(app, wn->w, wn->h);            // nace con la proporcion de la app
  wn->x = 66 + n * 38; wn->y = 30 + n * 26;
  int H = dexWorkBottom();
  if(wn->x + wn->w > LW - 10) wn->x = LW - 10 - wn->w;
  if(wn->y + wn->h > H - 10)  wn->y = H - 10 - wn->h;
  if(wn->x < 10) wn->x = 10;
  if(wn->y < 8)  wn->y = 8;
  dexClampWin(wn);
  wn->rx = wn->x; wn->ry = wn->y; wn->rw = wn->w; wn->rh = wn->h;
  dexRaise(slot);
  dexHostOpen(slot);                                // arranca la app REAL en la ventana
  if(ss <= 0) dexTbAppRect(app, sx, sy, ss);        // sin origen -> icono de la barra
  dexAnimStartFT(DXA_OPEN, slot, sx, sy, ss, ss, wn->x, wn->y, wn->w, wn->h, DEX_ANIM_MS);
}
static void dexOpen(int app){ dexOpenFrom(app, 0, 0, 0); }

static void dexCascade(){                           // menu contextual del escritorio
  int n = 0, H = dexWorkBottom();
  for(int k = 0; k < 4; k++){
    int i = dexOrder[k];
    if(!pwins[i].open) continue;
    pwins[i].mini = false; pwins[i].snap = SNAP_FREE;
    dexHostDefaultSize(pwins[i].app, pwins[i].w, pwins[i].h);
    pwins[i].x = 66 + n * 44; pwins[i].y = 26 + n * 30;
    if(pwins[i].x + pwins[i].w > LW - 10) pwins[i].x = LW - 10 - pwins[i].w;
    if(pwins[i].y + pwins[i].h > H - 10)  pwins[i].y = H - 10 - pwins[i].h;
    dexClampWin(&pwins[i]);
    pwins[i].rx = pwins[i].x; pwins[i].ry = pwins[i].y;
    pwins[i].rw = pwins[i].w; pwins[i].rh = pwins[i].h;
    n++;
  }
  dexAnimFinish(); dexMarkAll(); dexDirty = true;
}

// -------------------------------------------------------------
//  Overlays (cajon, Finder, notificaciones, recientes)
// -------------------------------------------------------------
// Banda que ocupa cada overlay. Marcar solo lo suyo (y no toda la pantalla) es
// lo que hace que abrir el cajon o la cortina no cueste un frame completo.
static void dexOvMark(uint8_t o){
  int w;
  switch(o){
    case DXO_DRAWER: w = DEX_DRW_W; break;
    case DXO_FINDER: w = DEX_FND_W; break;
    case DXO_NOTIF:  dexMark(LW - DEX_NP_W - 20, LW - 1); return;
    default:         dexMarkAll(); return;              // Recientes atenua todo
  }
  dexMark((LW - w) / 2 - 10, (LW + w) / 2 + 10);
}
static void dexOvOpen(uint8_t o){
  if(dexOv == o && !dexOvClosing) return;
  uint8_t prev = dexOv;
  dexOv = o; dexOvClosing = false; dexOvDone = false; dexOvT0 = millis();
  dexQLen = 0; dexQuery[0] = 0;
  if(prev != DXO_NONE) dexOvMark(prev);
  dexOvMark(o); dexDirty = true;
}
static void dexOvClose(){
  if(dexOv == DXO_NONE || dexOvClosing) return;
  dexOvClosing = true; dexOvDone = false; dexOvT0 = millis();
  dexOvMark(dexOv); dexDirty = true;
}
static float dexOvProg(){
  if(dexOv == DXO_NONE) return 0;
  uint32_t e = millis() - dexOvT0;
  float p = e >= DEX_ANIM_MS ? 1.0f : dexEase((float)e / (float)DEX_ANIM_MS);
  return dexOvClosing ? 1.0f - p : p;
}
static inline bool dexOvSettled(){
  return dexOv != DXO_NONE && !dexOvClosing && millis() - dexOvT0 >= DEX_ANIM_MS;
}
static void dexOvTick(){
  if(dexOv == DXO_NONE) return;
  if(millis() - dexOvT0 < DEX_ANIM_MS){       // creciendo o encogiendo
    dexOvMark(dexOv); dexDirty = true; dexOvDone = false; return;
  }
  if(dexOvClosing){
    uint8_t o = dexOv;
    dexOv = DXO_NONE; dexOvClosing = false; dexOvDone = false;
    dexOvMark(o); dexDirty = true; return;
  }
  // Ya asentado. Hace falta UN frame mas, y este es el motivo: dexPopupFrame
  // solo pinta el contenido cuando el progreso llega a 1, pero el ultimo frame
  // que se pedia era el de progreso < 1 -- o sea la caja vacia. Sin este pulso,
  // el cajon se quedaba en blanco hasta que otra cosa ensuciara la pantalla
  // (abrir una app), que es exactamente el sintoma que se veia.
  if(!dexOvDone){ dexOvDone = true; dexOvMark(dexOv); dexDirty = true; }
}

// -------------------------------------------------------------
//  Auto-ocultar la barra de tareas
// -------------------------------------------------------------
static void dexTbShow(){
  dexTbIdle = millis();
  if(dexTbTgt != 0){ dexTbFrom = dexTbOff; dexTbTgt = 0; dexTbT0 = millis(); }
}
static void dexTbHide(){
  if(!dexTbAuto || dexTbTgt == DEX_TB_H) return;
  dexTbFrom = dexTbOff; dexTbTgt = DEX_TB_H; dexTbT0 = millis();
}
static void dexTbAnimTick(){
  if(!dexTbAuto && dexTbTgt != 0){ dexTbFrom = dexTbOff; dexTbTgt = 0; dexTbT0 = millis(); }
  float d = dexTbOff - (float)dexTbTgt; if(d < 0) d = -d;
  if(d > 0.4f){
    uint32_t e = millis() - dexTbT0;
    float p = e >= DEX_TB_ANIM ? 1.0f : dexEase((float)e / (float)DEX_TB_ANIM);
    dexTbOff = dexTbFrom + ((float)dexTbTgt - dexTbFrom) * p;
    if(p >= 1.0f) dexTbOff = (float)dexTbTgt;
    dexMarkAll(); dexDirty = true;                  // la barra ocupa TODO el ancho logico
  }
  if(dexTbAuto && dexTbTgt == 0 && dexOv == DXO_NONE && !dexMenuOn &&
     dexGrab == DXG_NONE && millis() - dexTbIdle > DEX_TB_IDLE_MS) dexTbHide();
}

// #############################################################
// ##  DIBUJO
// #############################################################

// Panel glass para LANDSCAPE (Modo PC): drawLiquidGlassPanel escribe en coords
// portrait y no rota, asi que aqui uso primitivas rotacion-aware
// (fillRoundRectA). Translucido + borde, sin blur.
static void pcGlassPanel(int x, int y, int w, int h, int rad, uint16_t tint){
  // Mismo resguardo que ya tienen fillRoundRect/fillRoundRectA/
  // drawLiquidGlassPanelEx: con una ventana lo bastante chica (o un rad grande a
  // proposito) un rad sin recortar dejaria manchas en las esquinas en vez de la
  // curva limpia.
  if(rad < 0) rad = 0;
  if(2 * rad > w) rad = w / 2;
  if(2 * rad > h) rad = h / 2;
  fillRoundRectA(x, y, w, h, rad, tint, 205);
  drawRoundRect(x, y, w, h, rad, gDark ? rgb565(96,106,130) : rgb565(210,220,240));
}
// Superficie estandar de DeX: sigue el MISMO flag uiGlass del resto del sistema.
static void dexSurface(int x, int y, int w, int h, int rad, uint16_t tint){
  if(uiGlass) pcGlassPanel(x, y, w, h, rad, tint);
  else { fillRoundRect(x, y, w, h, rad, tint); drawRoundRect(x, y, w, h, rad, DEX_BORDER); }
}

// ---- Fondo propio del modo DeX (distinto al wallpaper del Home) ----
// dexWallpaperDraw pinta el fondo en el buffer activo. Es CARO (degradado a lo
// alto + dos arcos), asi que solo se ejecuta al construir la cache: una vez al
// entrar y cada vez que cambia la variante o el modo claro/oscuro.
static void dexWallpaperDraw(int bx, int bw){
  uint16_t a, b;
  if(dexWall == 0){      a = gDark ? rgb565(10,16,38)  : rgb565(150,186,236);
                         b = gDark ? rgb565(28,64,124) : rgb565(226,236,250); }
  else if(dexWall == 1){ a = gDark ? rgb565(28,12,44)  : rgb565(214,196,238);
                         b = gDark ? rgb565(86,36,96)  : rgb565(246,236,252); }
  else {                 a = gDark ? rgb565(6,30,32)   : rgb565(176,222,214);
                         b = gDark ? rgb565(16,78,84)  : rgb565(232,246,242); }
  for(int ly = 0; ly < LH; ly++)
    hLine(bx, ly, bw, mix565(a, b, (uint8_t)(ly * 255 / (LH - 1))));
  uint16_t g = rgb565(255,255,255);                 // dos arcos suaves: profundidad sin blur
  if(!dexCull(LW - 270, 300)) fillCircle(LW - 120, 70, 150, mix565(b, g, 18));
  if(!dexCull(-100, 380))     fillCircle(90, LH - 90, 190, mix565(a, g, 12));
}
// Construye (o reconstruye) la cache del fondo. Si no hay PSRAM para el buffer
// se sigue funcionando: dexWallpaper cae al camino procedural de siempre.
static void dexBgBuild(){
  if(!dexBg){
    dexBg = (uint16_t*)heap_caps_aligned_alloc(64, (size_t)SCR_W * SCR_H * 2,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!dexBg){ dexBgWall = 0xFF; return; }
  }
  uint16_t* old = gBuf;
  int c0 = gClipY0, c1 = gClipY1;
  bool land = gLand;
  setBuf(dexBg); gLand = true; gClipY0 = 0; gClipY1 = SCR_H - 1;
  dexWallpaperDraw(0, LW);
  setBuf(old); gLand = land; gClipY0 = c0; gClipY1 = c1;
  dexBgWall = dexWall; dexBgDark = gDark;
}
static void dexBgFree(){                            // no retener 768 KB fuera de DeX
  if(dexBg){ heap_caps_free(dexBg); dexBg = NULL; }
  dexBgWall = 0xFF;
}
// Fondo de un frame: con cache es una copia de filas fisicas (secuencial y sin
// mezcla); la banda sucia son justo esas filas, asi que solo se copia lo que se
// va a volcar.
static void dexWallpaper(){
  int bx = 0, bw = LW;
  if(!dexBand(bx, bw)) return;
  if(dexBg && (dexBgWall != dexWall || dexBgDark != gDark)) dexBgBuild();   // variante o tema cambiados
  if(dexBg && dexBgWall == dexWall && dexBgDark == gDark){
    int b0 = bx, b1 = bx + bw - 1;
    if(b1 > SCR_H - 1) b1 = SCR_H - 1;
    for(int y = b0; y <= b1; y++)
      memcpy(gBuf + (size_t)y * SCR_W, dexBg + (size_t)y * SCR_W, (size_t)SCR_W * 2);
    return;
  }
  dexWallpaperDraw(bx, bw);                         // respaldo sin cache
}

// ---- Iconos vectoriales de la barra ----
static void dexIcoChip(int x, int y, int s, bool on, uint16_t &fgOut){
  uint16_t bg = on ? DEX_ACCENT : (gDark ? rgb565(44,50,66) : rgb565(224,229,240));
  fillRoundRect(x, y, s, s, s / 4, bg);
  fgOut = on ? rgb565(255,255,255) : DEX_TXT_HI;
}
static void dexIcoGrid(int x, int y, int s, bool on){            // cajon de apps
  uint16_t c; dexIcoChip(x, y, s, on, c);
  int d = s / 6, g = (s - 3 * d) / 4;
  for(int r = 0; r < 3; r++) for(int q = 0; q < 3; q++)
    fillRoundRect(x + g + q * (d + g), y + g + r * (d + g), d, d, 1, c);
}
static void dexIcoSearch(int x, int y, int s, bool on){          // buscador tipo Finder
  uint16_t c; dexIcoChip(x, y, s, on, c);
  int cx = x + s * 44 / 100, cy = y + s * 44 / 100, r = s / 4;
  drawCircle(cx, cy, r, c); drawCircle(cx, cy, r - 1, c);
  strokeSegAA((float)(cx + r * 7 / 10), (float)(cy + r * 7 / 10),
              (float)(x + s * 80 / 100), (float)(y + s * 80 / 100), 1.7f, c);
}
static void dexIcoBell(int x, int y, int s, bool badge){         // notificaciones
  uint16_t c; dexIcoChip(x, y, s, false, c);
  int cx = x + s / 2, top = y + s / 4;
  fillRoundRect(cx - s / 5, top, 2 * (s / 5), s / 2, s / 6, c);
  fillRoundRect(cx - s / 3, top + s / 2 - 2, 2 * (s / 3), 3, 1, c);
  fillRect(cx - 1, top - 3, 3, 3, c);
  fillCircle(cx, top + s / 2 + 3, 2, c);
  if(badge) fillCircle(x + s - 5, y + 5, 4, rgb565(235,80,80));
}
static void dexIcoGear(int x, int y, int s, bool on){            // ajustes rapidos
  uint16_t c; dexIcoChip(x, y, s, on, c);
  uint16_t bg = on ? DEX_ACCENT : (gDark ? rgb565(44,50,66) : rgb565(224,229,240));
  int cx = x + s / 2, cy = y + s / 2, r = s / 4;
  fillCircle(cx, cy, r, c);
  fillCircle(cx, cy, r / 2, bg);
  for(int k = 0; k < 4; k++){
    int dx = (k == 2) ? -r - 2 : (k == 3) ? r + 2 : 0;
    int dy = (k == 0) ? -r - 2 : (k == 1) ? r + 2 : 0;
    fillRoundRect(cx + dx - 2, cy + dy - 2, 4, 4, 1, c);
  }
}
static void dexIcoPad(int x, int y, int s, bool on){             // touchpad virtual
  uint16_t c; dexIcoChip(x, y, s, on, c);
  int m = s / 5;
  drawRoundRect(x + m, y + m + 1, s - 2 * m, s - 2 * m - 2, 3, c);
  vLine(x + s / 2, y + s - m - 8, 6, c);
}

// ---- Ventana ----
static void dexWinBtnRect(int x, int y, int w, int k, int &bx, int &by, int &bw, int &bh){
  bw = DEX_BTN_W; bh = DEX_TTL_H - 8; by = y + 4;
  bx = x + w - (3 - k) * DEX_BTN_W - 4;
}
// Zona de TOQUE de los controles, mas grande que la dibujada: ocupa toda la
// altura de la barra de titulo y se ensancha hasta la mitad del hueco entre
// botones (asi crecen sin solaparse entre si). El boton dibujado era de 34x26 y
// se fallaba constantemente; con esto el blanco real es de 40x34.
static void dexWinBtnHit(int x, int y, int w, int k, int &bx, int &by, int &bw, int &bh){
  dexWinBtnRect(x, y, w, k, bx, by, bw, bh);
  by = y; bh = DEX_TTL_H;                         // toda la altura de la barra
  if(k == 2) bw += 4;                             // cerrar llega hasta el borde
}
static void dexWinButtons(int i, int x, int y, int w, bool active){
  uint16_t fg = active ? DEX_TXT_HI : DEX_TXT_LO;
  for(int k = 0; k < 3; k++){
    int bx, by, bw, bh; dexWinBtnRect(x, y, w, k, bx, by, bw, bh);
    if(k == 2) fillRoundRectA(bx, by, bw, bh, 5, rgb565(226,76,76), active ? 200 : 90);
    int cx = bx + bw / 2, cy = by + bh / 2;
    uint16_t c = (k == 2) ? rgb565(255,255,255) : fg;
    if(k == 0){                                                  // minimizar
      fillRect(cx - 5, cy + 3, 11, 2, c);
    } else if(k == 1){                                           // maximizar / restaurar
      if(pwins[i].snap != SNAP_FREE){
        drawRoundRect(cx - 5, cy - 2, 9, 8, 1, c);
        drawRoundRect(cx - 2, cy - 5, 9, 8, 1, c);
      } else drawRoundRect(cx - 5, cy - 4, 11, 9, 1, c);
    } else {                                                     // cerrar
      strokeSegAA((float)(cx - 4), (float)(cy - 4), (float)(cx + 4), (float)(cy + 4), 1.5f, c);
      strokeSegAA((float)(cx - 4), (float)(cy + 4), (float)(cx + 4), (float)(cy - 4), 1.5f, c);
    }
  }
}

// -------------------------------------------------------------
//  HOSTING DE APPS REALES DENTRO DE UNA VENTANA
//  ---------------------------------------------------------
//  Cada ventana tiene su propio lienzo 480x800 (el mismo tamano que la
//  pantalla). La app corre EXACTAMENTE igual que a pantalla completa -- su
//  enter() y su tick() de APP_REG, sin tocar una linea de las 16 apps -- pero
//  con gRtTarget apuntando a ese lienzo, asi que su setBuf(fb) y sus flxFlush
//  no llegan al panel. Despues DeX escala el lienzo dentro del area de cliente
//  de la ventana.
//  Por que un lienzo por ventana y no uno compartido: las apps dibujan de forma
//  INCREMENTAL sobre un framebuffer persistente (un tick que solo repinta una
//  fila cuenta con que el resto sigue ahi). Con un lienzo compartido habria que
//  llamar a enter() en cada refresco, y para varias apps enter() ademas
//  reinicia estado (settingsEnter pone setSel y setScroll a 0), o sea que el
//  scroll y la seleccion se perderian en cada frame.
// -------------------------------------------------------------
struct DexHost {
  uint16_t* surf;      // lienzo 480x800 donde dibuja la app (incremental)
  uint16_t* cache;     // resultado YA escalado, en orden de volcado
  size_t    cap;       // capacidad de cache, en pixeles
  int       fw, fh;    // tamano del contenido con el que se construyo la cache
  int       rw, rh;    // lienzo logico con el que la app dibujo por ultima vez
  uint32_t  reMs;      // ultimo re-render por cambio de tamano (limita la cadencia)
  bool      scaled;    // la cache refleja el lienzo actual
  bool      live;
};
static DexHost dexHost[4];
static bool dexHostBusy = false;              // reentrada: una app no puede hospedar a otra
// Depuracion del mapeo tactil: con DEX_TOUCH_DEBUG a 1 se pinta un punto en la
// coordenada TRADUCIDA, dentro del propio lienzo de la app. Si el punto cae bajo
// el dedo, la traduccion es correcta; si se desvia, el fallo esta en el mapeo.
#define DEX_TOUCH_DEBUG 0
static int dexHostTouchX = -1, dexHostTouchY = -1, dexHostTouchWin = -1;

static void dexClientRect(int i, int &cx, int &cy, int &cw, int &ch){
  cx = pwins[i].x + 1;  cy = pwins[i].y + DEX_TTL_H;
  cw = pwins[i].w - 2;  ch = pwins[i].h - DEX_TTL_H - 1;
  if(cw < 1) cw = 1;
  if(ch < 1) ch = 1;
}
// Modo PC no puede hospedarse a si mismo (seria recursion infinita: pcEnter
// dentro de pcTick). Su ventana se queda con el panel informativo.
static inline bool dexHostable(int app){ return app != IC_MODOPC; }

// Encaje de la app dentro del area de cliente, CONSERVANDO LA PROPORCION
// (letterbox). Es la unica fuente de verdad de la geometria: la usan el
// escalado y el mapeo de toque con los MISMOS pasos en coma fija, asi que no
// puede haber deriva de 1 px entre lo que se ve y lo que se toca.
//
// Antes se estiraba el lienzo a cualquier w/h del marco. Una app de 480x800
// metida en un cliente de 446x249 se comprimia 3.2x en vertical y solo 1.08x en
// horizontal: de ahi la deformacion, y de ahi que un boton de 44x44 acabara
// midiendo 40x13 px en pantalla. Trece pixeles de alto no se aciertan con el
// dedo -- por eso "solo Paint respondia": Paint es un lienzo libre, cualquier
// posicion vale, no tiene que acertar un boton.
static void dexHostFit(int i, int cx, int cy, int cw, int ch, DexFit &f){
  uint8_t fl = APP_REG[pwins[i].app].flags;
  if(cw < 1) cw = 1;
  if(ch < 1) ch = 1;
  f.flex = (fl & APP_FLEX) != 0;
  if(f.flex){
    // App adaptativa: se le da un lienzo del TAMANO REAL del area de cliente y
    // se dibuja 1:1. Ni escalado (nitidez perfecta) ni barras de letterbox (el
    // "espacio vacio"): la app se remaqueta sola contra gAppW/gAppH.
    // La orientacion del lienzo sigue a la forma de la ventana, porque el buffer
    // es de 480x800: en horizontal se usa rotado (hasta 800x480) y en vertical
    // sin rotar (hasta 480x800). Asi cualquier ventana del area util cabe.
    f.land = (cw > ch);
    int maxW = f.land ? LW : SCR_W, maxH = f.land ? LH : SCR_H;
    f.ow = cw < maxW ? cw : maxW;
    f.oh = ch < maxH ? ch : maxH;
    f.aw = f.ow; f.ah = f.oh;                  // lienzo == contenido -> escala 1:1
    f.ox = cx + (cw - f.ow) / 2;
    f.oy = cy + (ch - f.oh) / 2;
    f.stepX = f.stepY = 1u << 16;              // paso exacto: dexStep(idx) == idx
    return;
  }
  f.land = (fl & APP_LAND) != 0;
  f.aw = f.land ? LW : SCR_W;
  f.ah = f.land ? LH : SCR_H;
  f.ow = cw;
  f.oh = (int)(((int64_t)cw * f.ah) / f.aw);
  if(f.oh > ch){ f.oh = ch; f.ow = (int)(((int64_t)ch * f.aw) / f.ah); }
  if(f.ow < 1) f.ow = 1;
  if(f.oh < 1) f.oh = 1;
  f.ox = cx + (cw - f.ow) / 2;                  // centrado: barras iguales a los lados
  f.oy = cy + (ch - f.oh) / 2;
  f.stepX = ((uint32_t)f.aw << 16) / (uint32_t)f.ow;
  f.stepY = ((uint32_t)f.ah << 16) / (uint32_t)f.oh;
}
// Indice de origen para un indice de destino. Coma fija 16.16 y recorte al
// ultimo pixel valido: cierra el off-by-one de la ultima fila y columna, que
// con la division entera podia dar exactamente aw/ah y salirse por uno.
static inline int dexStep(int idx, uint32_t step, int lim){
  int v = (int)(((uint32_t)idx * step) >> 16);
  if(v < 0) v = 0;
  if(v > lim - 1) v = lim - 1;
  return v;
}
// Tamano minimo de una ventana que hospeda una app. Por debajo de ~0.4 de
// escala el texto deja de leerse y los botones bajan de 13 px: la ventana
// existe pero la app es inservible. El suelo se expresa en tamano de CLIENTE y
// se convierte a tamano de ventana con la proporcion de la propia app.
static void dexHostMinSize(int app, int &mw, int &mh){
  mw = DEX_MIN_W; mh = DEX_MIN_H;
  if(!dexHostable(app)) return;
  bool land = (APP_REG[app].flags & APP_LAND) != 0;
  int aw = land ? LW : SCR_W, ah = land ? LH : SCR_H;
  int H = dexWorkBottom();
  int cch = land ? 240 : 330;
  int ccw = (int)(((int64_t)cch * aw) / ah);
  if(cch + DEX_TTL_H + 1 > H){ cch = H - DEX_TTL_H - 1; ccw = (int)(((int64_t)cch * aw) / ah); }
  if(ccw + 2 > LW){ ccw = LW - 2; cch = (int)(((int64_t)ccw * ah) / aw); }
  mw = ccw + 2; mh = cch + DEX_TTL_H + 1;
  if(mw < DEX_MIN_W) mw = DEX_MIN_W;
  if(mh < DEX_MIN_H) mh = DEX_MIN_H;
}
// Tamano inicial: la ventana nace con la PROPORCION de la app, no con un
// 448x284 fijo. Es lo que hace que la escala util (~0.5) no dependa de que el
// usuario acierte a redimensionar bien.
static void dexHostDefaultSize(int app, int &w, int &h){
  bool land = (APP_REG[app].flags & APP_LAND) != 0;
  int aw = land ? LW : SCR_W, ah = land ? LH : SCR_H;
  int H = dexWorkBottom();
  int cch = H - DEX_TTL_H - 21;
  int ccw = (int)(((int64_t)cch * aw) / ah);
  if(ccw > LW - 80){ ccw = LW - 80; cch = (int)(((int64_t)ccw * ah) / aw); }
  w = ccw + 2; h = cch + DEX_TTL_H + 1;
  if(!dexHostable(app)){ w = 448; h = 284; }
}

// Ejecuta enter() y/o tick() de la app con TODO el estado global desviado, y lo
// restaura pase lo que pase. Restaurar gState es lo que impide que una app que
// navega a una subpantalla completa (Ajustes -> Wi-Fi, -> bloqueo) se lleve por
// delante el escritorio: el cambio se descarta y DeX sigue en pie.
static void dexHostRun(int i, bool doEnter, bool doTick, const Touch* inject){
  if(i < 0 || i > 3 || !dexHost[i].surf || dexHostBusy) return;
  int app = pwins[i].app;
  if(!dexHostable(app)) return;

  uint16_t* oBuf = gBuf; bool oLand = gLand;
  int oC0 = gClipY0, oC1 = gClipY1, oX0 = gClipX0, oX1 = gClipX1;
  int oApp = gAppId, oState = gState;
  int oAW = gAppW, oAH = gAppH;
  Touch oT = T;                                      // el T REAL del sistema

  dexHostBusy = true;
  gHosted = true; gHostReq = 0; gHostReqApp = -1;
  gRtTarget = dexHost[i].surf; gRtDirty = false;
  {                                                  // lienzo logico de esta ventana
    int cx, cy, cw, ch; dexClientRect(i, cx, cy, cw, ch);
    DexFit f; dexHostFit(i, cx, cy, cw, ch, f);
    gLand = f.land;                                  // Juegos rota; FLEX rota si la ventana es apaisada
    gAppW = f.aw; gAppH = f.ah;                      // lo que la app usa para maquetar
  }
  gClipY0 = 0; gClipY1 = SCR_H - 1; gClipX0 = 0; gClipX1 = SCR_W - 1;
  gAppId = app; gState = ST_APP;
  if(inject) T = *inject;                            // toque ya traducido a la app
  setBuf(fb);                                        // -> redirigido al lienzo

  if(doEnter){
    if(!(APP_REG[app].flags & APP_CUSTOM_HEADER)){   // mismo marco que enterApp()
      appDrawChrome(app);
      appDrawHeader(app);
    }
    if(APP_REG[app].enter) APP_REG[app].enter();
    gRtDirty = true;
  }
  if(doTick && APP_REG[app].tick) APP_REG[app].tick();
#if DEX_TOUCH_DEBUG
  if(inject && dexHostTouchWin == i){               // punto en la coordenada TRADUCIDA
    fillCircle(dexHostTouchX, dexHostTouchY, 5, rgb565(255,0,0));
    fillCircle(dexHostTouchX, dexHostTouchY, 2, rgb565(255,255,255));
    gRtDirty = true;
  }
#endif

  // El lienzo con el que ACABA de dibujar la app se anota ANTES de restaurar
  // gAppW/gAppH. Anotarlo despues guardaba 480x800 (el valor restaurado) en vez
  // del tamano real del cliente, asi que dexHostRelayout veia siempre "el
  // tamano ha cambiado" y relanzaba enter() en CADA tick. Para la Calculadora
  // eso significaba volver a poner el display a "0" inmediatamente despues de
  // cada tecla: el toque SI llegaba, pero el resultado se borraba antes de
  // verse. De ahi el "el touch dejo de responder".
  dexHost[i].rw = gAppW; dexHost[i].rh = gAppH;

  gRtTarget = NULL;
  gHosted = false;
  gState = oState; gAppId = oApp;
  gAppW = oAW; gAppH = oAH;
  T = oT;                                            // restaura el T REAL, no el inyectado
  gLand = oLand; gClipY0 = oC0; gClipY1 = oC1; gClipX0 = oX0; gClipX1 = oX1;
  gBuf = oBuf;
  dexHostBusy = false;

  if(gRtDirty){ dexHost[i].scaled = false; dexMarkWin(pwins[i].x, pwins[i].w); dexDirty = true; }
}

static void dexHostClose(int i){
  if(i < 0 || i > 3) return;
  if(dexHost[i].surf){ heap_caps_free(dexHost[i].surf); dexHost[i].surf = NULL; }
  if(dexHost[i].cache){ heap_caps_free(dexHost[i].cache); dexHost[i].cache = NULL; }
  dexHost[i].cap = 0; dexHost[i].fw = dexHost[i].fh = 0;
  dexHost[i].rw = dexHost[i].rh = 0; dexHost[i].reMs = 0;
  dexHost[i].scaled = false; dexHost[i].live = false;
}
// Arranca la app de la ventana i. Si no hay PSRAM para el lienzo se sigue
// adelante sin hosting: la ventana cae al panel decorativo, no se rompe nada.
static void dexHostOpen(int i){
  if(i < 0 || i > 3) return;
  dexHostClose(i);
  if(!dexHostable(pwins[i].app)) return;
  dexHost[i].surf = (uint16_t*)heap_caps_aligned_alloc(64, (size_t)SCR_W * SCR_H * 2,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!dexHost[i].surf) return;
  memset(dexHost[i].surf, 0, (size_t)SCR_W * SCR_H * 2);
  dexHost[i].live = true;
  dexHostRun(i, true, false, NULL);
}

// Construye la version escalada del lienzo. Se ejecuta SOLO cuando la app ha
// dibujado algo o cuando cambia el tamano del encaje -- no por frame. Eso es lo
// que permite pagar un filtro de caja: al reducir a menos de dos tercios, el
// vecino mas cercano se salta filas enteras y el texto de las apps se rompe;
// promediando el bloque de origen se mantiene legible.
// La cache se guarda YA EN ORDEN DE VOLCADO (columna logica, fila descendente),
// de modo que componer un frame es un memcpy por columna: es la ruta que corre
// durante el arrastre y las animaciones, y asi es la mas barata posible.
static void dexHostScale(int i, const DexFit &f){
  DexHost &H = dexHost[i];
  size_t need = (size_t)f.ow * (size_t)f.oh;
  if(!H.cache || H.cap < need){
    if(H.cache){ heap_caps_free(H.cache); H.cache = NULL; H.cap = 0; }
    H.cache = (uint16_t*)heap_caps_aligned_alloc(64, need * 2,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    H.cap = H.cache ? need : 0;
  }
  if(!H.cache) return;
  const uint16_t* src = H.surf;
  bool land = f.land;
  bool box  = (f.stepX > (3u << 15)) || (f.stepY > (3u << 15));   // reduccion > 1.5x
  for(int k = 0; k < f.ow; k++){
    int sx  = dexStep(k, f.stepX, f.aw);
    int sx2 = dexStep(k + 1, f.stepX, f.aw);
    if(sx2 < sx) sx2 = sx;
    uint16_t* d = H.cache + (size_t)k * (size_t)f.oh;
    for(int j = 0; j < f.oh; j++){
      int sy  = dexStep(j, f.stepY, f.ah);
      int sy2 = dexStep(j + 1, f.stepY, f.ah);
      if(sy2 < sy) sy2 = sy;
      uint16_t v;
      if(box){
        uint16_t a = land ? src[(size_t)sx  * SCR_W + (SCR_W - 1 - sy)]  : src[(size_t)sy  * SCR_W + sx];
        uint16_t b = land ? src[(size_t)sx2 * SCR_W + (SCR_W - 1 - sy)]  : src[(size_t)sy  * SCR_W + sx2];
        uint16_t c = land ? src[(size_t)sx  * SCR_W + (SCR_W - 1 - sy2)] : src[(size_t)sy2 * SCR_W + sx];
        uint16_t e = land ? src[(size_t)sx2 * SCR_W + (SCR_W - 1 - sy2)] : src[(size_t)sy2 * SCR_W + sx2];
        v = mix565(mix565(a, b, 128), mix565(c, e, 128), 128);
      } else {
        v = land ? src[(size_t)sx * SCR_W + (SCR_W - 1 - sy)] : src[(size_t)sy * SCR_W + sx];
      }
      d[f.oh - 1 - j] = v;                       // orden de volcado (fila fisica ascendente)
    }
  }
  H.fw = f.ow; H.fh = f.oh; H.scaled = true;
}

static void dexHostBlit(int i, int cx, int cy, int cw, int ch){
  DexHost &H = dexHost[i];
  if(!H.surf || cw < 1 || ch < 1) return;
  DexFit f; dexHostFit(i, cx, cy, cw, ch, f);

  // Barras del letterbox. Se pintan SIEMPRE: al reducir la ventana el contenido
  // encoge, y sin esto quedaria asomando el escalado anterior por los lados.
  uint16_t bar = DEX_WIN_BODY;
  if(f.ox > cx)                  fillRect(cx, cy, f.ox - cx, ch, bar);
  if(f.ox + f.ow < cx + cw)      fillRect(f.ox + f.ow, cy, (cx + cw) - (f.ox + f.ow), ch, bar);
  if(f.oy > cy)                  fillRect(f.ox, cy, f.ow, f.oy - cy, bar);
  if(f.oy + f.oh < cy + ch)      fillRect(f.ox, f.oy + f.oh, f.ow, (cy + ch) - (f.oy + f.oh), bar);

  if(H.fw != f.ow || H.fh != f.oh) H.scaled = false;      // se redimensiono -> reescalar
  if(!H.scaled) dexHostScale(i, f);

  int b0 = dexBandLo(), b1 = dexBandHi();
  if(!H.cache){                                          // sin PSRAM: vecino mas cercano al vuelo
    bool land = f.land;
    for(int k = 0; k < f.ow; k++){
      int lx = f.ox + k;
      if(lx < b0 || lx > b1 || (unsigned)lx >= SCR_H) continue;
      int sx = dexStep(k, f.stepX, f.aw);
      uint16_t* d = gBuf + (size_t)lx * SCR_W;
      for(int j = 0; j < f.oh; j++){
        int ly = f.oy + j;
        if((unsigned)ly >= SCR_W) continue;
        int sy = dexStep(j, f.stepY, f.ah);
        d[(SCR_W - 1) - ly] = land ? H.surf[(size_t)sx * SCR_W + (SCR_W - 1 - sy)]
                                   : H.surf[(size_t)sy * SCR_W + sx];
      }
    }
    return;
  }
  for(int k = 0; k < f.ow; k++){                          // ruta normal: memcpy por columna
    int lx = f.ox + k;
    if(lx < b0 || lx > b1 || (unsigned)lx >= SCR_H) continue;
    int y0 = f.oy, n = f.oh;
    if(y0 < 0){ n += y0; y0 = 0; }                        // recorte contra el lienzo fisico
    if(y0 + n > SCR_W) n = SCR_W - y0;
    if(n <= 0) continue;
    int skip = y0 - f.oy;
    memcpy(gBuf + (size_t)lx * SCR_W + (size_t)(SCR_W - (y0 + n)),
           H.cache + (size_t)k * (size_t)f.oh + (size_t)(f.oh - skip - n),
           (size_t)n * 2);
  }
}

// Inversa EXACTA del encaje: mismo dexHostFit, mismos pasos, misma funcion
// dexStep. No puede desviarse de lo que se ve porque es literalmente el mismo
// calculo. Devuelve false si el punto cae en las barras del letterbox: ahi no
// hay app, y entregar ese toque haria que la app reaccionara a algo que el
// usuario no ha tocado.
static bool dexHostMapT(int i, int cx, int cy, int cw, int ch, int px, int py, int &tx, int &ty){
  DexFit f; dexHostFit(i, cx, cy, cw, ch, f);
  if(px < f.ox || px >= f.ox + f.ow || py < f.oy || py >= f.oy + f.oh) return false;
  int ax = dexStep(px - f.ox, f.stepX, f.aw);
  int ay = dexStep(py - f.oy, f.stepY, f.ah);
  if(f.flex){
    // La app adaptativa dibuja en coordenadas de lienzo, asi que el toque va en
    // esas mismas: sin intercambio de ejes. La rotacion, si el lienzo es
    // horizontal, ya la resuelven las primitivas via gLand.
    tx = ax; ty = ay;
  } else if(f.land){
    tx = (SCR_W - 1) - ay;                      // inversa de lx = T.y, ly = SCR_W-1-T.x
    ty = ax;
  } else { tx = ax; ty = ay; }
  int limX = f.flex ? f.aw : SCR_W, limY = f.flex ? f.ah : SCR_H;
  if(!f.flex && f.land){ limX = SCR_W; limY = SCR_H; }
  if(tx < 0) tx = 0;
  if(tx > limX - 1) tx = limX - 1;
  if(ty < 0) ty = 0;
  if(ty > limY - 1) ty = limY - 1;
  return true;
}
static void dexHostTouch(int i, int cx, int cy, int cw, int ch){
  if(!dexHost[i].surf) return;
  int tx, ty;
  if(!dexHostMapT(i, cx, cy, cw, ch, pX, pY, tx, ty)) return;   // barra: no es la app
  int sx = tx, sy = ty;
  dexHostMapT(i, cx, cy, cw, ch, dexPressX, dexPressY, sx, sy);
  // ---------------------------------------------------------------------
  // ESTE evento se construye en una variable LOCAL y se inyecta. Antes se
  // escribia directamente sobre la T global antes de llamar a dexHostRun, y
  // dexHostRun guardaba "la T de entrada" para restaurarla... que ya era la
  // modificada. Resultado: T.startX/startY se quedaban en coordenadas de APP
  // despues de cada toque. En la vuelta siguiente flexPollTouch compara la
  // posicion FISICA del dedo contra ese startX de otro espacio de coordenadas:
  //     else if(abs(gx - T.startX) > 12 ...) T.moved = true;
  // la diferencia siempre pasa de 12 px, asi que T.moved se quedaba en true, y
  // con moved=true pTap NUNCA se activa. De ahi que ninguna app respondiera a
  // toques y que solo Paint diera senales de vida: Paint usa T.down y posicion,
  // no T.tap. Manteniendo el evento fuera de la T global el ciclo se rompe.
  // ---------------------------------------------------------------------
  Touch e;
  e.x = tx; e.y = ty;
  e.startX = sx; e.startY = sy;
  e.dx = tx - sx; e.dy = ty - sy;
  e.down = pDown; e.pressed = pPressed; e.released = pReleased;
  e.tap = pTap; e.moved = T.moved;
  e.downMs = T.downMs; e.lastMs = T.lastMs;
  e.swipeUp = e.swipeDown = e.swipeLeft = e.swipeRight = false;
  dexHostTouchX = tx; dexHostTouchY = ty; dexHostTouchWin = i;   // depuracion
  dexHostRun(i, false, true, &e);
}
// Una app APP_FLEX maqueta contra el tamano de SU ventana, asi que al
// redimensionar hay que volver a ejecutar su enter() con el lienzo nuevo -- si
// no, se veria el layout viejo estirado. Se limita la cadencia para que
// arrastrar el borde no dispare un render completo por frame, y siempre se hace
// uno final cuando el tamano se estabiliza.
// Ademas mantiene vivos los fundidos de las secciones opcionales: mientras
// uiFading este activo se sigue re-renderizando, que es lo que hace que la
// aparicion/desaparicion se vea como un fundido y no como un salto.
#define DEX_RELAYOUT_MS 45
static void dexHostRelayout(int i){
  if(i < 0 || i > 3 || !pwins[i].open || pwins[i].mini || !dexHost[i].surf) return;
  int cx, cy, cw, ch; dexClientRect(i, cx, cy, cw, ch);
  DexFit f; dexHostFit(i, cx, cy, cw, ch, f);
  bool sizeChanged = (f.aw != dexHost[i].rw || f.ah != dexHost[i].rh);
  if(!sizeChanged && !uiFading) return;
  uint32_t now = millis();
  if(sizeChanged && dexHost[i].reMs && now - dexHost[i].reMs < DEX_RELAYOUT_MS) return;
  dexHost[i].reMs = now;
  gRelayout = true;                               // enter() debe re-dibujar, no re-inicializar
  dexHostRun(i, true, false, NULL);
  gRelayout = false;
}
// Atiende lo que la app pidio (cerrarse, abrir otra, Recientes). Se hace FUERA
// de dexHostRun para no reentrar en la app desde su propia pila.
static void dexHostServe(int i){
  int req = gHostReq, reqApp = gHostReqApp;
  gHostReq = 0; gHostReqApp = -1;
  if(req == 1) dexCloseWin(i);
  else if(req == 2 && reqApp >= 0 && reqApp < 16){
    int ix, iy, is; dexTbAppRect(reqApp, ix, iy, is);
    dexOpenFrom(reqApp, ix, iy, is);
  }
  else if(req == 3) dexOvOpen(DXO_RECENTS);
}

static void dexWinContent(int i, int app, int x, int y, int w, int h, bool active){
  // Con la app hospedada viva, el contenido ES la app real escalada al marco.
  if(dexHost[i].surf){ dexHostBlit(i, x, y, w, h); return; }
  if(w < 30 || h < 20) return;
  fillRect(x, y, w, h, DEX_WIN_BODY);
  uint16_t lo = active ? DEX_TXT_LO : mix565(DEX_TXT_LO, DEX_WIN_BODY, 80);
  int S = h * 3 / 5; if(S > 96) S = 96;
  if(S >= 30 && w > S + 150 && h > S + 20) drawAppIcon(app, x + w - S - 22, y + h - S - 16, S);
  int tw = w - 40;
  if(h > 50)  dexTextFit(x + 20, y + 16, appName(app), 4, active ? DEX_TXT_HI : DEX_TXT_LO, tw);
  if(h > 84)  dexTextFit(x + 20, y + 56, "Ventana de Samsung DeX", 2, lo, tw);
  if(h > 112) dexTextFit(x + 20, y + 86, "Arrastra la barra de titulo a un borde", 1, lo, tw);
  if(h > 128) dexTextFit(x + 20, y + 100, "para anclarla a media pantalla.", 1, lo, tw);
}
static void dexDrawWindow(int i, int x, int y, int w, int h, bool active){
  if(w < 20 || h < 20 || dexCull(x, w)) return;
  int rad = 14; if(2 * rad > w) rad = w / 2; if(2 * rad > h) rad = h / 2;
  fillRoundRectA(x + 3, y + 6, w, h, rad, rgb565(4,6,12), active ? 100 : 62);    // sombra
  dexSurface(x, y, w, h, rad, DEX_WIN_BG);                                       // cuerpo
  uint16_t tc = active ? DEX_TTL_ACT : DEX_TTL_INA;                              // barra de titulo
  int th = DEX_TTL_H; if(th > h) th = h;
  fillRoundRect(x, y, w, th, rad, tc);
  if(th > rad) fillRect(x, y + th - rad, w, rad, tc);
  hLine(x, y + th - 1, w, DEX_BORDER);
  if(w > 60){
    drawAppIcon(pwins[i].app, x + 10, y + (th - 20) / 2, 20);
    dexTextFit(x + 38, y + (th - 15) / 2, appName(pwins[i].app), 2,
               active ? DEX_TXT_HI : DEX_TXT_LO, w - 52 - 3 * DEX_BTN_W);
    dexWinButtons(i, x, y, w, active);
  }
  dexWinContent(i, pwins[i].app, x + 1, y + th, w - 2, h - th - 1, active);
  // Ventana inactiva: velo sutil (la "transicion de opacidad" activa/inactiva).
  if(!active) fillRoundRectA(x, y, w, h, rad, gDark ? rgb565(0,0,0) : rgb565(20,26,44), 34);
}
// Version simplificada para abrir/cerrar/minimizar: crece o encoge con fundido.
// A proposito NO pinta el contenido -- seria tirar trabajo a la basura en cada
// uno de los ~6 frames de la animacion.
static void dexDrawWinAnim(int x, int y, int w, int h, int app, uint8_t a){
  if(w < 6 || h < 6 || dexCull(x, w)) return;
  int rad = 14; if(2 * rad > w) rad = w / 2; if(2 * rad > h) rad = h / 2;
  fillRoundRectA(x + 2, y + 4, w, h, rad, rgb565(4,6,12), (uint8_t)(a * 70 / 255));
  fillRoundRectA(x, y, w, h, rad, DEX_WIN_BG, a);
  int th = DEX_TTL_H * h / 260; if(th < 6) th = 6;
  if(th > DEX_TTL_H) th = DEX_TTL_H;
  if(th > h) th = h;
  fillRoundRectA(x, y, w, th, rad, DEX_TTL_ACT, a);
  if(th > rad) fillRectA(x, y + th - rad, w, rad, DEX_TTL_ACT, a);
  int S = h / 3; if(S > 56) S = 56;
  if(a >= 210 && S >= 18 && w > S + 20 && h > th + S + 8)
    drawAppIcon(app, x + (w - S) / 2, y + th + (h - th - S) / 2, S);
}
static void dexDrawGhost(){                          // contorno fantasma del snap
  if(dexSnapGhost == SNAP_FREE) return;
  int x, y, w, h; dexSnapRect(dexSnapGhost, x, y, w, h);
  if(dexCull(x, w)) return;
  fillRoundRectA(x + 6, y + 6, w - 12, h - 12, 16, rgb565(190,214,255), 70);
  drawRoundRect(x + 6, y + 6, w - 12, h - 12, 16, rgb565(150,192,255));
  drawRoundRect(x + 7, y + 7, w - 14, h - 14, 15, rgb565(118,168,250));
}
// Se conserva la firma original (PWin*): es la razon por la que el tipo PWin vive
// arriba del todo del archivo (ver el comentario de los tipos).
static void pcDrawWindow(PWin* wn){
  int i = (int)(wn - pwins);
  if(i < 0 || i > 3) return;
  dexDrawWindow(i, wn->x, wn->y, wn->w, wn->h, i == dexFocus);
}

// ---- Barra de tareas ----
static void dexTaskbar(){
  int ty = dexTbY(), h = DEX_TB_H;
  if(ty >= LH) return;                                          // oculta del todo
  // La barra se dibuja SIEMPRE con su geometria completa (0..LW) y es el recorte
  // de banda quien decide que columnas se tocan. Antes se le pasaba el rect ya
  // recortado a la banda, y pcGlassPanel -> drawRoundRect remata el rect con un
  // vLine en CADA extremo: es decir, pintaba dos lineas verticales claras justo
  // en los bordes de la banda. Como la banda se mueve con el puntero, cada
  // repintado dejaba dos rayas nuevas -> las columnas claras de la captura.
  // Con la geometria completa esos vLine caen en lx=0 y lx=LW-1, donde deben.
  if(uiGlass) pcGlassPanel(0, ty, LW, h, 0, DEX_TB_GLASS);
  else fillRect(0, ty, LW, h, DEX_TB_BG);
  hLine(0, ty, LW, DEX_BORDER);
  if(!dexCull(dexRDrw[0], dexRDrw[2]))
    dexIcoGrid(dexRDrw[0], dexRDrw[1], dexRDrw[2], dexOv == DXO_DRAWER && !dexOvClosing);
  if(!dexCull(dexRFnd[0], dexRFnd[2]))
    dexIcoSearch(dexRFnd[0], dexRFnd[1], dexRFnd[2], dexOv == DXO_FINDER && !dexOvClosing);
  // Grupo CENTRADO: fijadas + abiertas, con indicador de "abierta".
  int app[10]; bool op[10];
  int n = dexTbItems(app, op);
  for(int i = 0; i < n; i++){
    int x, y, s; dexTbItemRect(i, n, x, y, s);
    if(dexCull(x - 6, s + 12)) continue;
    bool foc = false;
    for(int k = 0; k < 4; k++)
      if(pwins[k].open && pwins[k].app == app[i] && k == dexFocus && !pwins[k].mini) foc = true;
    if(op[i]) fillRoundRectA(x - 6, y - 4, s + 12, s + 8, 8, DEX_ACCENT, foc ? 76 : 34);
    drawAppIcon(app[i], x, y, s);
    if(op[i]){
      int iw = foc ? 16 : 8;
      fillRoundRect(x + s / 2 - iw / 2, y + s + 6, iw, 3, 1, DEX_ACCENT);
    }
  }
  // Extremo derecho: touchpad · campana · ajustes · wifi · bateria · reloj/fecha
  if(!dexCull(dexRPad[0], dexRPad[2]))   dexIcoPad(dexRPad[0], dexRPad[1], dexRPad[2], dexPadOn);
  if(!dexCull(dexRBell[0], dexRBell[2])) dexIcoBell(dexRBell[0], dexRBell[1], dexRBell[2], gNotifCount > 0);
  if(!dexCull(dexRGear[0], dexRGear[2])) dexIcoGear(dexRGear[0], dexRGear[1], dexRGear[2],
                                                    dexOv == DXO_NOTIF && !dexOvClosing);
  if(!dexCull(dexRWifiX - 11, 22)) drawWifi(dexRWifiX, ty + h / 2 + 6, 11, DEX_TXT_HI);
  if(!dexCull(dexRBat[0], dexRBat[2] + 2))
    drawBattery(dexRBat[0], dexRBat[1], dexRBat[2], dexRBat[3], 82, DEX_TXT_HI);
  if(!dexCull(dexRClkX - 110, 112)){
    char cs[12]; clkStrBar(cs, sizeof(cs));
    char sd[40]; buildShortDate(sd, sizeof(sd));
    drawTextR(dexRClkX, ty + 12, cs, 2, DEX_TXT_HI);
    drawTextR(dexRClkX, ty + 34, sd, 1, DEX_TXT_LO);
  }
}

// ---- Teclado compacto del buscador ----
#define DEX_KB_RH  32
#define DEX_KB_GAP 5
static void dexKbRect(int kx, int ky, int kw, int row, int col, int &x, int &y, int &w, int &h){
  int n = (row == 0) ? 10 : 9;
  int kwid = (kw - (n - 1) * DEX_KB_GAP) / n;
  int rowW = n * kwid + (n - 1) * DEX_KB_GAP;
  x = kx + (kw - rowW) / 2 + col * (kwid + DEX_KB_GAP);
  y = ky + row * (DEX_KB_RH + DEX_KB_GAP);
  w = kwid; h = DEX_KB_RH;
}
static void dexKbDraw(int kx, int ky, int kw){
  for(int r = 0; r < 3; r++){
    int n = (r == 0) ? 10 : 9;
    for(int c = 0; c < n; c++){
      int x, y, w, h; dexKbRect(kx, ky, kw, r, c, x, y, w, h);
      fillRoundRect(x, y, w, h, 6, DEX_PANEL2);
      const char* row = DEX_KB[r];
      int rl = (int)strlen(row);
      if(c < rl){ char b[2] = { row[c], 0 }; drawTextC(x + w / 2, y + h / 2 - 8, b, 2, DEX_TXT_HI); }
      else if(r == 2 && c == 7) fillRect(x + w / 2 - 6, y + h / 2 + 5, 13, 2, DEX_TXT_LO);   // espacio
      else if(r == 2 && c == 8){                                                            // borrar
        int mx = x + w / 2, my = y + h / 2;
        strokeSegAA((float)(mx - 6), (float)my, (float)(mx + 6), (float)my, 1.6f, DEX_TXT_HI);
        strokeSegAA((float)(mx - 6), (float)my, (float)(mx - 1), (float)(my - 5), 1.6f, DEX_TXT_HI);
        strokeSegAA((float)(mx - 6), (float)my, (float)(mx - 1), (float)(my + 5), 1.6f, DEX_TXT_HI);
      }
    }
  }
}
static bool dexKbTap(int kx, int ky, int kw, int px, int py){
  for(int r = 0; r < 3; r++){
    int n = (r == 0) ? 10 : 9;
    for(int c = 0; c < n; c++){
      int x, y, w, h; dexKbRect(kx, ky, kw, r, c, x, y, w, h);
      if(!dexInBox(px, py, x, y, w, h)) continue;
      const char* row = DEX_KB[r];
      int rl = (int)strlen(row);
      if(c < rl){ if(dexQLen < (int)sizeof(dexQuery) - 1){ dexQuery[dexQLen++] = row[c]; dexQuery[dexQLen] = 0; } }
      else if(r == 2 && c == 7){ if(dexQLen < (int)sizeof(dexQuery) - 1){ dexQuery[dexQLen++] = ' '; dexQuery[dexQLen] = 0; } }
      else if(r == 2 && c == 8){ if(dexQLen > 0) dexQuery[--dexQLen] = 0; }
      return true;
    }
  }
  return false;
}
static void dexSearchField(int x, int y, int w, int h, const char* ph){
  fillRoundRect(x, y, w, h, h / 2, DEX_PANEL2);
  drawRoundRect(x, y, w, h, h / 2, DEX_BORDER);
  int cx = x + 20, cy = y + h / 2, r = 6;
  drawCircle(cx, cy - 1, r, DEX_TXT_LO);
  strokeSegAA((float)(cx + 4), (float)(cy + 3), (float)(cx + 9), (float)(cy + 8), 1.6f, DEX_TXT_LO);
  if(dexQLen > 0){
    dexTextFit(x + 38, y + h / 2 - 8, dexQuery, 2, DEX_TXT_HI, w - 76);
    int bx = x + w - 26, by = y + h / 2;                       // limpiar
    fillCircle(bx, by, 9, DEX_BORDER);
    strokeSegAA((float)(bx - 4), (float)(by - 4), (float)(bx + 4), (float)(by + 4), 1.5f, DEX_PANEL);
    strokeSegAA((float)(bx - 4), (float)(by + 4), (float)(bx + 4), (float)(by - 4), 1.5f, DEX_PANEL);
  } else dexTextFit(x + 38, y + h / 2 - 8, ph, 2, DEX_TXT_LO, w - 56);
}
// Marco animado de un pop-up: mientras crece/encoge solo se pinta la caja con
// fundido; el contenido aparece cuando ya esta abierto del todo. Devuelve true
// cuando toca dibujar el contenido.
static bool dexPopupFrame(int &x, int &y, int w, int h, int rad){
  float p = dexOvProg();
  if(p >= 0.999f){
    x = (LW - w) / 2; y = (dexWorkBottom() - h) / 2;
    if(dexCull(x, w)) return false;
    fillRoundRectA(x + 4, y + 8, w, h, rad, rgb565(4,6,12), 110);
    dexSurface(x, y, w, h, rad, DEX_PANEL);
    return true;
  }
  float sc = 0.86f + 0.14f * p;
  int aw = (int)(w * sc), ah = (int)(h * sc);
  int ax = (LW - aw) / 2, ay = (dexWorkBottom() - ah) / 2;
  x = ax; y = ay;
  if(dexCull(ax, aw)) return false;
  uint8_t a = (uint8_t)(235 * p);
  fillRoundRectA(ax + 4, ay + 8, aw, ah, rad, rgb565(4,6,12), (uint8_t)(90 * p));
  fillRoundRectA(ax, ay, aw, ah, rad, DEX_PANEL, a);
  return false;
}

// ---- Cajon de apps: pop-up centrado (NO pantalla completa) ----
#define DEX_DRW_KBY 292                      // offset del teclado dentro del pop-up
static void dexDrawerGrid(int fx, int fy, int idx, int &x, int &y, int &s){
  int cols = 6, cw = (DEX_DRW_W - 28) / cols, rh = 76;
  s = 52;
  x = fx + 14 + (idx % cols) * cw + (cw - s) / 2;
  y = fy + 62 + (idx / cols) * rh;
}
static void dexDrawerDraw(){
  int fx, fy;
  if(!dexPopupFrame(fx, fy, DEX_DRW_W, DEX_DRW_H, 20)) return;
  dexSearchField(fx + 18, fy + 14, DEX_DRW_W - 36, 36, "Buscar aplicaciones");
  int list[18]; int n = dexFilterApps(list, 18);
  int cw = (DEX_DRW_W - 28) / 6;
  for(int i = 0; i < n; i++){
    int x, y, s; dexDrawerGrid(fx, fy, i, x, y, s);
    drawAppIcon(list[i], x, y, s);
    dexTextFitC(x + s / 2, y + s + 6, appName(list[i]), 1, DEX_TXT_HI, cw - 6);
  }
  if(n == 0) drawTextC(fx + DEX_DRW_W / 2, fy + 130, "Sin resultados", 2, DEX_TXT_LO);
  dexKbDraw(fx + 14, fy + DEX_DRW_KBY, DEX_DRW_W - 28);
}

// ---- Buscador tipo Finder: apps + ajustes a la vez ----
#define DEX_FND_ROW  27
#define DEX_FND_KBY  292
#define DEX_FND_APPS 4
#define DEX_FND_SETS 3
static void dexFinderRows(int &nApps, int &nSet, int* apps, int* sets){
  nApps = dexFilterApps(apps, DEX_FND_APPS);
  nSet = 0;
  for(int i = 0; i < 8 && nSet < DEX_FND_SETS; i++)
    if(dexMatch(DEX_SET[i], dexQuery, dexQLen)) sets[nSet++] = i;
}
static void dexFinderDraw(){
  int fx, fy;
  if(!dexPopupFrame(fx, fy, DEX_FND_W, DEX_FND_H, 20)) return;
  dexSearchField(fx + 18, fy + 14, DEX_FND_W - 36, 36, "Buscar apps y ajustes");
  int apps[DEX_FND_APPS], sets[DEX_FND_SETS], na, ns;
  dexFinderRows(na, ns, apps, sets);
  int y = fy + 62;
  if(na > 0){
    drawText(fx + 22, y, "Aplicaciones", 1, DEX_TXT_LO); y += 15;
    for(int i = 0; i < na; i++){
      drawAppIcon(apps[i], fx + 20, y + 2, 20);
      dexTextFit(fx + 48, y + 5, appName(apps[i]), 2, DEX_TXT_HI, DEX_FND_W - 78);
      y += DEX_FND_ROW;
    }
    y += 6;
  }
  if(ns > 0){
    drawText(fx + 22, y, "Ajustes", 1, DEX_TXT_LO); y += 15;
    for(int i = 0; i < ns; i++){
      fillRoundRect(fx + 20, y + 2, 20, 20, 6, DEX_PANEL2);
      fillCircle(fx + 30, y + 12, 5, DEX_ACCENT);
      dexTextFit(fx + 48, y + 5, DEX_SET[sets[i]], 2, DEX_TXT_HI, DEX_FND_W - 78);
      y += DEX_FND_ROW;
    }
  }
  if(na == 0 && ns == 0) drawTextC(fx + DEX_FND_W / 2, fy + 120, "Sin resultados", 2, DEX_TXT_LO);
  dexKbDraw(fx + 14, fy + DEX_FND_KBY, DEX_FND_W - 28);
}

// ---- Panel de notificaciones / ajustes rapidos (cortina) ----
#define DEX_NP_TH   58                       // alto de cada tile
#define DEX_NP_STEP 68                       // paso vertical entre filas de tiles
static void dexNpRect(int &x, int &y, int &w, int &h){
  w = DEX_NP_W; x = LW - w - 12; y = 10; h = dexWorkBottom() - 22;
}
static void dexNpTile(int idx, int nx, int ny, int &x, int &y, int &w, int &h){
  w = (DEX_NP_W - 36) / 2; h = DEX_NP_TH;
  x = nx + 12 + (idx % 2) * (w + 12);
  y = ny + 42 + (idx / 2) * DEX_NP_STEP;
}
static inline int dexNpBrightY(int ny){ return ny + 42 + 2 * DEX_NP_STEP + 6; }
// Boton de salida SIEMPRE visible en el panel. La barra de tareas de DeX ya no
// lleva el boton "Salir" que tenia el Modo PC viejo (ahora ese hueco es del
// grupo centrado de apps), asi que la salida vive aqui y ademas en el menu
// contextual de la barra: sin una de las dos, se podria quedar uno encerrado.
#define DEX_NP_EXIT_H 34
static inline int dexNpExitY(int ny, int nh){ return ny + nh - DEX_NP_EXIT_H - 12; }
static bool dexNpState(int idx){
  switch(idx){
    case 0:  return uiGlass;
    case 1:  return gDark;
    case 2:  return dexTbAuto;
    default: return dexPadOn;
  }
}
static const char* dexNpLabel(int idx){
  switch(idx){
    case 0:  return "Liquid Glass";
    case 1:  return "Modo oscuro";
    case 2:  return "Barra auto";
    default: return "Touchpad";
  }
}
static void dexNotifDraw(){
  float p = dexOvProg();
  if(p <= 0.01f) return;
  int nx, ny, nw, nh; dexNpRect(nx, ny, nw, nh);
  if(dexCull(nx, nw)) return;
  int visH = (int)(nh * p);                       // cortina: se despliega hacia abajo
  if(visH < 8) return;
  fillRoundRectA(nx + 4, ny + 6, nw, visH, 18, rgb565(4,6,12), (uint8_t)(110 * p));
  if(p < 0.999f){                                 // aun desplegandose: solo la caja
    fillRoundRectA(nx, ny, nw, visH, 18, DEX_PANEL, (uint8_t)(255 * p));
    return;
  }
  dexSurface(nx, ny, nw, visH, 18, DEX_PANEL);
  drawText(nx + 16, ny + 14, "Ajustes rapidos", 2, DEX_TXT_HI);
  for(int i = 0; i < 4; i++){
    int x, y, w, h; dexNpTile(i, nx, ny, x, y, w, h);
    bool on = dexNpState(i);
    fillRoundRect(x, y, w, h, 12, on ? DEX_ACCENT : DEX_PANEL2);
    uint16_t tc = on ? rgb565(255,255,255) : DEX_TXT_HI;
    fillCircle(x + 20, y + 20, 8, tc);
    fillCircle(x + 20, y + 20, on ? 3 : 5, on ? DEX_ACCENT : DEX_PANEL2);
    dexTextFit(x + 10, y + 36, dexNpLabel(i), 1, tc, w - 20);
  }
  // Brillo: PWM REAL del backlight (setBacklight), igual que el panel rapido.
  int by = dexNpBrightY(ny), bx = nx + 12, bw = nw - 24, bh = 26;
  drawText(nx + 16, by, "Brillo", 1, DEX_TXT_LO);
  fillRoundRect(bx, by + 14, bw, bh, bh / 2, DEX_PANEL2);
  int fw = bw * gBright / 100; if(fw < bh) fw = bh;
  fillRoundRect(bx, by + 14, fw, bh, bh / 2, DEX_ACCENT);
  char pb[8]; snprintf(pb, sizeof(pb), "%d%%", gBright);
  drawTextR(bx + bw - 10, by + 14 + bh / 2 - 8, pb, 2, DEX_TXT_HI);
  // Notificaciones REALES (gNotifs[]); no se inventan tarjetas de relleno.
  int ly = by + 14 + bh + 16, listBot = dexNpExitY(ny, nh) - 10;
  drawText(nx + 16, ly, "Notificaciones", 1, DEX_TXT_LO); ly += 16;
  if(gNotifCount == 0) drawText(nx + 16, ly + 6, "Sin notificaciones", 2, DEX_TXT_LO);
  else for(int i = 0; i < gNotifCount && i < NOTIF_MAX; i++){
    if(ly + 46 > listBot) break;
    fillRoundRect(nx + 12, ly, nw - 24, 42, 12, DEX_PANEL2);
    fillCircle(nx + 34, ly + 21, 10, DEX_ACCENT);
    dexTextFit(nx + 54, ly + 8, gNotifs[i].mod.name, 2, DEX_TXT_HI, nw - 74);
    dexTextFit(nx + 54, ly + 26, "Modulo detectado", 1, DEX_TXT_LO, nw - 74);
    ly += 48;
  }
  int ey = dexNpExitY(ny, nh);
  fillRoundRect(nx + 12, ey, nw - 24, DEX_NP_EXIT_H, 12, rgb565(180,60,60));
  drawTextC(nx + nw / 2, ey + DEX_NP_EXIT_H / 2 - 8, "Salir de Modo PC", 2, rgb565(255,255,255));
}

// ---- Recientes (selector de tareas propio de este modo) ----
// Mismo PATRON de tarjetas que el App Switcher del resto del OS (swRenderCards:
// miniatura + nombre, arrastrar hacia arriba para cerrar), pero con geometria
// landscape y sobre las VENTANAS de DeX. No se reutiliza swTasks[] a proposito:
// ese array se declara mas abajo en el archivo (no seria visible aqui) y guarda
// apps a pantalla completa, que no es lo mismo que una ventana de DeX.
#define DEX_REC_W 190
#define DEX_REC_H 148
static int dexRecList(int* out){
  int n = 0;
  for(int k = 3; k >= 0; k--){ int i = dexOrder[k]; if(pwins[i].open) out[n++] = i; }
  return n;
}
static void dexRecCard(int idx, int n, int &x, int &y){
  int gap = 18, tot = n * DEX_REC_W + (n - 1) * gap;
  x = (LW - tot) / 2 + idx * (DEX_REC_W + gap);
  y = (dexWorkBottom() - DEX_REC_H) / 2;
}
static void dexRecentsDraw(){
  float p = dexOvProg();
  if(p <= 0.01f) return;
  int dx = 0, dw = LW;
  if(dexBand(dx, dw)) fillRectA(dx, 0, dw, dexWorkBottom(), rgb565(4,6,14), (uint8_t)(170 * p));
  int list[4]; int n = dexRecList(list);
  if(n == 0){
    drawTextC(LW / 2, dexWorkBottom() / 2 - 10, "Sin ventanas abiertas", 3, rgb565(226,232,244));
    return;
  }
  for(int i = 0; i < n; i++){
    int x, y; dexRecCard(i, n, x, y);
    if(i == dexRecDrag) y += dexRecDY;
    int w = DEX_REC_W, h = DEX_REC_H;
    if(dexCull(x, w)) continue;
    fillRoundRectA(x + 3, y + 6, w, h, 14, rgb565(2,4,10), (uint8_t)(120 * p));
    fillRoundRect(x, y, w, h, 14, DEX_WIN_BG);
    drawRoundRect(x, y, w, h, 14, DEX_BORDER);
    fillRoundRect(x, y, w, 24, 14, DEX_TTL_ACT);
    fillRect(x, y + 10, w, 14, DEX_TTL_ACT);
    drawAppIcon(pwins[list[i]].app, x + 6, y + 4, 16);
    dexTextFit(x + 26, y + 6, appName(pwins[list[i]].app), 1, DEX_TXT_HI, w - 56);
    int S = 54;
    drawAppIcon(pwins[list[i]].app, x + (w - S) / 2, y + 24 + (h - 24 - S - 22) / 2, S);
    dexTextFitC(x + w / 2, y + h - 20, pwins[list[i]].mini ? "Minimizada" : "En ejecucion",
                1, DEX_TXT_LO, w - 16);
    int cx = x + w - 14, cy = y + 12;                          // cerrar
    strokeSegAA((float)(cx - 4), (float)(cy - 4), (float)(cx + 4), (float)(cy + 4), 1.5f, DEX_TXT_HI);
    strokeSegAA((float)(cx - 4), (float)(cy + 4), (float)(cx + 4), (float)(cy - 4), 1.5f, DEX_TXT_HI);
  }
  drawTextC(LW / 2, dexWorkBottom() - 40,
            "Arrastra una tarjeta hacia arriba para cerrarla", 1, rgb565(198,206,222));
}

// ---- Menu contextual ----
#define DEX_MENU_W  216
#define DEX_MENU_IH 30
static int dexMenuCount(uint8_t k){ return k == 2 ? 5 : 4; }
static const char* dexMenuLabel(uint8_t k, int i){
  if(k == 0){
    switch(i){ case 0:  return "Cambiar fondo";
               case 1:  return "Organizar en cascada";
               case 2:  return "Recientes";
               default: return "Cajon de apps"; }
  }
  if(k == 1){
    switch(i){ case 0:  return dexTbAuto ? "Barra: auto-ocultar ON" : "Barra: auto-ocultar OFF";
               case 1:  return dexBigIcons ? "Iconos grandes" : "Iconos normales";
               case 2:  return "Recientes";
               default: return "Salir de Modo PC"; }
  }
  switch(i){ case 0:  return "Minimizar";
             case 1:  return (dexMenuWin >= 0 && pwins[dexMenuWin].snap != SNAP_FREE) ? "Restaurar" : "Maximizar";
             case 2:  return "Anclar a la izquierda";
             case 3:  return "Anclar a la derecha";
             default: return "Cerrar"; }
}
static void dexMenuGeom(int &x, int &y, int &w, int &h){
  int n = dexMenuCount(dexMenuKind);
  w = DEX_MENU_W; h = 10 + n * DEX_MENU_IH;
  x = dexMenuX; y = dexMenuY;
  if(x + w > LW - 6) x = LW - 6 - w;
  if(x < 6) x = 6;
  if(y + h > dexWorkBottom() - 6) y = dexWorkBottom() - 6 - h;
  if(y < 6) y = 6;
}
static void dexMenuDraw(){
  int x, y, w, h; dexMenuGeom(x, y, w, h);
  if(dexCull(x, w)) return;
  fillRoundRectA(x + 3, y + 5, w, h, 12, rgb565(4,6,12), 120);
  dexSurface(x, y, w, h, 12, DEX_PANEL);
  int n = dexMenuCount(dexMenuKind);
  for(int i = 0; i < n; i++){
    int iy = y + 5 + i * DEX_MENU_IH;
    bool danger = (dexMenuKind == 2 && i == 4) || (dexMenuKind == 1 && i == 3);
    dexTextFit(x + 14, iy + 8, dexMenuLabel(dexMenuKind, i), 2,
               danger ? rgb565(236,110,110) : DEX_TXT_HI, w - 28);
  }
}

// ---- Touchpad virtual + cursor ----
static void dexPadGeom(int &x, int &y, int &w, int &h){
  w = DEX_PAD_W; h = DEX_PAD_H;
  x = LW - w - 14; y = dexWorkBottom() - h - 14;
}
static void dexPadDraw(){
  int x, y, w, h; dexPadGeom(x, y, w, h);
  if(dexCull(x, w)) return;
  fillRoundRectA(x, y, w, h, 14, gDark ? rgb565(10,14,24) : rgb565(210,218,232), 190);
  drawRoundRect(x, y, w, h, 14, DEX_BORDER);
  hLine(x + 10, y + h - 30, w - 20, DEX_BORDER);
  drawTextC(x + w / 2, y + 12, "Touchpad", 1, DEX_TXT_LO);
  drawTextC(x + w / 2, y + h - 22, "Toca = clic · Manten = menu", 1, DEX_TXT_LO);
}
// Forma del cursor segun el contexto: flecha / manita / cursor de texto.
static uint8_t dexCursorShape(){
  int x = dexCurX, y = dexCurY;
  if(dexOv == DXO_DRAWER || dexOv == DXO_FINDER){
    int w = (dexOv == DXO_DRAWER) ? DEX_DRW_W : DEX_FND_W;
    int h = (dexOv == DXO_DRAWER) ? DEX_DRW_H : DEX_FND_H;
    int fx = (LW - w) / 2, fy = (dexWorkBottom() - h) / 2;
    if(dexInBox(x, y, fx + 16, fy + 12, w - 32, 34)) return 2;      // campo de busqueda
    if(dexInBox(x, y, fx, fy, w, h)) return 1;
  }
  if(y >= dexTbY()) return 1;
  if(dexMenuOn) return 1;
  for(int k = 3; k >= 0; k--){
    int i = dexOrder[k];
    if(!pwins[i].open || pwins[i].mini) continue;
    if(dexInBox(x, y, pwins[i].x, pwins[i].y, pwins[i].w, DEX_TTL_H)) return 1;
  }
  return 0;
}
static void dexCursorDraw(){
  int x = dexCurX, y = dexCurY;
  if(dexCull(x - 8, 26)) return;
  uint16_t W = rgb565(255,255,255), K = rgb565(24,28,38);
  uint8_t sh = dexCursorShape();
  if(sh == 2){                                    // cursor de texto
    fillRect(x - 1, y - 9, 3, 19, W);
    fillRect(x - 4, y - 10, 9, 2, W);
    fillRect(x - 4, y + 9, 9, 2, W);
    return;
  }
  if(sh == 1){                                    // manita
    fillRoundRect(x - 6, y - 1, 13, 15, 5, W);
    drawRoundRect(x - 6, y - 1, 13, 15, 5, K);
    fillRoundRect(x - 2, y - 11, 5, 12, 2, W);
    drawRoundRect(x - 2, y - 11, 5, 12, 2, K);
    return;
  }
  fillTriangle(x, y, x, y + 16, x + 11, y + 11, W);              // flecha
  fillTriangle(x, y, x + 11, y + 11, x + 12, y + 12, W);
  strokeSegAA((float)x, (float)y, (float)x, (float)(y + 16), 1.1f, K);
  strokeSegAA((float)x, (float)y, (float)(x + 12), (float)(y + 12), 1.1f, K);
  strokeSegAA((float)x, (float)(y + 16), (float)(x + 7), (float)(y + 11), 1.1f, K);
  strokeSegAA((float)(x + 7), (float)(y + 11), (float)(x + 12), (float)(y + 12), 1.1f, K);
}

// -------------------------------------------------------------
//  Composicion + presentacion con banda sucia
// -------------------------------------------------------------
static void dexCompose(){
  dexWallpaper();                                  // escritorio limpio: SIN iconos sueltos
  for(int k = 0; k < 4; k++){                      // ventanas, de atras hacia delante
    int i = dexOrder[k];
    if(!pwins[i].open) continue;
    if(dexAnimIs(i)){
      int ax, ay, aw, ah; uint8_t al; dexAnimCur(ax, ay, aw, ah, al);
      if(dexAK == DXA_GEOM) dexDrawWindow(i, ax, ay, aw, ah, i == dexFocus);
      else dexDrawWinAnim(ax, ay, aw, ah, pwins[i].app, al);
    } else if(!pwins[i].mini){
      dexDrawWindow(i, pwins[i].x, pwins[i].y, pwins[i].w, pwins[i].h, i == dexFocus);
    }
  }
  if(dexGrab == DXG_MOVE) dexDrawGhost();
  if(dexOv == DXO_RECENTS) dexRecentsDraw();       // Recientes va BAJO la barra
  dexTaskbar();
  if(dexOv == DXO_DRAWER)      dexDrawerDraw();
  else if(dexOv == DXO_FINDER) dexFinderDraw();
  else if(dexOv == DXO_NOTIF)  dexNotifDraw();
  if(dexMenuOn) dexMenuDraw();
  if(dexPadOn){ dexPadDraw(); dexCursorDraw(); }
}
// Compone en bbuf y sube SOLO la banda sucia. La banda es un rango de lx que,
// tras la rotacion, es un rango de filas fisicas: gClipY0/gClipY1 recortan justo
// ese eje, y dexCull/dexBand evitan ademas recorrer lo que cae fuera.
static void dexPaint(bool full){
  if(full) dexMarkAll();
  dexDirty = false;
  if(dexBX1 < dexBX0) return;
  int b0 = dexBX0, b1 = dexBX1;
  dexBX0 = 0x7FFF; dexBX1 = -1;
  if(b0 < 0) b0 = 0;
  if(b1 > LW - 1) b1 = LW - 1;
  gLand = true; setBuf(bbuf);
  gClipY0 = b0; gClipY1 = b1;
  dexCompose();
  gClipY0 = 0; gClipY1 = SCR_H - 1;
  setBuf(fb);
  present(b0, b1);
}
static void pcRender(){ dexPaint(true); }

// #############################################################
// ##  ENTRADA
// #############################################################
static void dexPointer(){
  pPressed = pReleased = pTap = pLong = pDTap = false;
  int lx = T.y, ly = (SCR_W - 1) - T.x;            // fisico -> landscape
  bool wasDown = pDown;

  if(dexPadOn){
    int px, py, pw, ph; dexPadGeom(px, py, pw, ph);
    if(T.down && !wasDown && dexInBox(lx, ly, px, py, pw, ph)){
      dexPadGrab = true; dexPadLX = lx; dexPadLY = ly; dexLongFired = false;
      dexPressX = dexCurX; dexPressY = dexCurY;
    }
    if(dexPadGrab){
      if(T.down){                                  // movimiento RELATIVO del cursor
        int ncx = dexCurX + (int)((lx - dexPadLX) * 1.45f);
        int ncy = dexCurY + (int)((ly - dexPadLY) * 1.45f);
        if(ncx < 0) ncx = 0; if(ncx > LW - 1) ncx = LW - 1;
        if(ncy < 0) ncy = 0; if(ncy > LH - 1) ncy = LH - 1;
        if(ncx != dexCurX || ncy != dexCurY){
          dexMark(dexCurX - 16, dexCurX + 20);        // borrar donde estaba
          dexCurX = ncx; dexCurY = ncy;
          dexMark(dexCurX - 16, dexCurX + 20);        // pintar donde esta
          dexDirty = true;
        }
        dexPadLX = lx; dexPadLY = ly;
      }
      pX = dexCurX; pY = dexCurY;
      // El pad NO arrastra ventanas: sintetiza clic (toque) y clic derecho
      // (mantener). Arrastrar y redimensionar siguen siendo con el dedo directo.
      if(!dexLongFired && T.down && !T.moved && millis() - T.downMs > DEX_LONG_MS){
        dexLongFired = true; pLong = true;
      }
      if(!T.down && wasDown){
        if(!T.moved && !dexLongFired) pTap = true;    // mismo criterio de clic que arriba
        dexPadGrab = false; dexLongFired = false;
      }
      pDown = T.down;
      return;
    }
  }

  pX = lx; pY = ly;
  if(dexPadOn && (dexCurX != lx || dexCurY != ly) && T.down){    // el cursor sigue al dedo
    dexMark(dexCurX - 16, dexCurX + 20);
    dexCurX = lx; dexCurY = ly;
    dexMark(dexCurX - 16, dexCurX + 20);
    dexDirty = true;
  }
  pDown = T.down;
  if(T.down && !wasDown){ pPressed = true; dexLongFired = false; dexPressX = pX; dexPressY = pY; }
  if(!T.down && wasDown) pReleased = true;
  if(!dexLongFired && T.down && !T.moved && millis() - T.downMs > DEX_LONG_MS){
    dexLongFired = true; pLong = true;
  }
  // Clic = soltar sin haber arrastrado y sin que haya saltado la pulsacion larga.
  // A PROPOSITO no se usa T.tap: ese exige ademas dur < 550 ms, asi que una
  // pulsacion un poco mas lenta sobre una tecla del buscador o un icono de la
  // barra se perdia ENTERA y habia que repetirla. Aqui no hay limite superior:
  // lo que separa un clic de una pulsacion larga es dexLongFired, no el reloj.
  if(pReleased && !T.moved && !dexLongFired){
    pTap = true;
    if(millis() - dexTapMs < DEX_DTAP_MS && abs(pX - dexTapX) < 26 && abs(pY - dexTapY) < 26){
      pDTap = true; dexTapMs = 0;
    } else { dexTapMs = millis(); dexTapX = pX; dexTapY = pY; }
  }
  if(!T.down) dexLongFired = false;
}

static int dexWinAt(int x, int y){
  for(int k = 3; k >= 0; k--){
    int i = dexOrder[k];
    if(!pwins[i].open || pwins[i].mini) continue;
    if(x >= pwins[i].x - DEX_GRIP && x < pwins[i].x + pwins[i].w + DEX_GRIP &&
       y >= pwins[i].y - DEX_GRIP && y < pwins[i].y + pwins[i].h + DEX_GRIP) return i;
  }
  return -1;
}
// 8 zonas de agarre. Arriba solo cuentan los primeros px, para no robarle el
// arrastre a la barra de titulo.
static uint8_t dexResizeMask(int i, int x, int y){
  PWin* w = &pwins[i]; uint8_t m = 0;
  // Asimetrico a proposito: hacia FUERA se agarra con holgura, hacia DENTRO
  // solo unos pixeles. Con +-DEX_GRIP a ambos lados, el borde se comia 14 px de
  // area de cliente por cada lado y esos toques nunca llegaban a la app.
  const int GIN = 6;                              // hacia dentro, lo justo para no
                                                  // robarle area de cliente a la app
  if(x >= w->x - DEX_GRIP && x <= w->x + GIN) m |= 1;
  if(x >= w->x + w->w - GIN && x <= w->x + w->w + DEX_GRIP) m |= 2;
  if(y >= w->y - DEX_GRIP && y <= w->y + GIN) m |= 4;
  if(y >= w->y + w->h - GIN && y <= w->y + w->h + DEX_GRIP) m |= 8;
  if((m & 1) && (m & 2)) m &= ~2;
  if((m & 4) && (m & 8)) m &= ~8;
  return m;
}

static void dexMenuRun(uint8_t k, int i){
  dexMenuOn = false; dexMarkAll(); dexDirty = true;
  if(k == 0){
    if(i == 0) dexWall = (uint8_t)((dexWall + 1) % 3);
    else if(i == 1) dexCascade();
    else if(i == 2) dexOvOpen(DXO_RECENTS);
    else dexOvOpen(DXO_DRAWER);
    return;
  }
  if(k == 1){
    if(i == 0){ dexTbAuto = !dexTbAuto; if(dexTbAuto) dexTbIdle = millis(); else dexTbShow(); dexClampAll(); }
    else if(i == 1) dexBigIcons = !dexBigIcons;
    else if(i == 2) dexOvOpen(DXO_RECENTS);
    else pcExit();
    return;
  }
  int w = dexMenuWin;
  if(w < 0 || w > 3 || !pwins[w].open) return;
  if(i == 0) dexMinimize(w);
  else if(i == 1) dexToggleMax(w);
  else if(i == 2) dexApplySnap(w, SNAP_L);
  else if(i == 3) dexApplySnap(w, SNAP_R);
  else dexCloseWin(w);
}
static void dexMenuOpen(uint8_t kind, int x, int y, int win){
  dexMenuOn = true; dexMenuKind = kind; dexMenuX = x; dexMenuY = y; dexMenuWin = win;
  dexMarkAll(); dexDirty = true;
}

// ---- Toques dentro de cada overlay (siempre devuelven true: el overlay
//      captura el toque, como en DeX real) ----
static bool dexDrawerTouch(){
  int w = DEX_DRW_W, h = DEX_DRW_H;
  int fx = (LW - w) / 2, fy = (dexWorkBottom() - h) / 2;
  if(pTap && !dexInBox(pX, pY, fx, fy, w, h)){ dexOvClose(); return true; }   // fuera -> cerrar
  if(!pTap) return true;
  if(dexQLen > 0 && dexInBox(pX, pY, fx + w - 44, fy + 14, 32, 36)){          // limpiar
    dexQLen = 0; dexQuery[0] = 0; dexOvMark(DXO_DRAWER); dexDirty = true; return true;
  }
  if(dexKbTap(fx + 14, fy + DEX_DRW_KBY, w - 28, pX, pY)){ dexOvMark(DXO_DRAWER); dexDirty = true; return true; }
  int list[18]; int n = dexFilterApps(list, 18);
  for(int i = 0; i < n; i++){
    int x, y, s; dexDrawerGrid(fx, fy, i, x, y, s);
    if(dexInBox(pX, pY, x - 8, y - 6, s + 16, s + 24)){
      dexOvClose();
      dexOpenFrom(list[i], x, y, s);                 // crece DESDE el icono del cajon
      return true;
    }
  }
  return true;
}
static bool dexFinderTouch(){
  int w = DEX_FND_W, h = DEX_FND_H;
  int fx = (LW - w) / 2, fy = (dexWorkBottom() - h) / 2;
  if(pTap && !dexInBox(pX, pY, fx, fy, w, h)){ dexOvClose(); return true; }
  if(!pTap) return true;
  if(dexQLen > 0 && dexInBox(pX, pY, fx + w - 44, fy + 14, 32, 36)){
    dexQLen = 0; dexQuery[0] = 0; dexOvMark(DXO_FINDER); dexDirty = true; return true;
  }
  if(dexKbTap(fx + 14, fy + DEX_FND_KBY, w - 28, pX, pY)){ dexOvMark(DXO_FINDER); dexDirty = true; return true; }
  int apps[DEX_FND_APPS], sets[DEX_FND_SETS], na, ns;
  dexFinderRows(na, ns, apps, sets);
  int y = fy + 62;
  if(na > 0){
    y += 15;
    for(int i = 0; i < na; i++){
      if(dexInBox(pX, pY, fx + 14, y, w - 28, DEX_FND_ROW)){
        dexOvClose(); dexOpenFrom(apps[i], fx + 20, y + 2, 20); return true;
      }
      y += DEX_FND_ROW;
    }
    y += 6;
  }
  if(ns > 0){
    y += 15;
    for(int i = 0; i < ns; i++){
      if(dexInBox(pX, pY, fx + 14, y, w - 28, DEX_FND_ROW)){
        dexOvClose(); dexOpen(IC_AJUSTES); return true;       // los ajustes viven en su app
      }
      y += DEX_FND_ROW;
    }
  }
  return true;
}
static bool dexNotifTouch(){
  int nx, ny, nw, nh; dexNpRect(nx, ny, nw, nh);
  if(pTap && !dexInBox(pX, pY, nx, ny, nw, nh)){ dexOvClose(); return true; }
  if(!dexOvSettled()) return true;
  for(int i = 0; i < 4; i++){
    int x, y, w, h; dexNpTile(i, nx, ny, x, y, w, h);
    if(pTap && dexInBox(pX, pY, x, y, w, h)){
      if(i == 0){ uiGlass = !uiGlass; cfgSavePrefs(); gHomeDirty = true; }
      else if(i == 1){ gDark = !gDark; cfgSavePrefs(); gHomeDirty = true; }
      else if(i == 2){ dexTbAuto = !dexTbAuto; if(dexTbAuto) dexTbIdle = millis(); else dexTbShow(); dexClampAll(); }
      else { dexPadOn = !dexPadOn; if(dexPadOn){ dexCurX = LW / 2; dexCurY = LH / 2 - 40; } }
      dexMarkAll(); dexDirty = true; return true;
    }
  }
  if(pTap && dexInBox(pX, pY, nx + 12, dexNpExitY(ny, nh), nw - 24, DEX_NP_EXIT_H)){
    dexOvClose(); pcExit(); return true;                       // salida visible de DeX
  }
  int by = dexNpBrightY(ny), bx = nx + 12, bw = nw - 24, bh = 26;
  if(pDown && dexInBox(pX, pY, bx, by + 14, bw, bh)){          // brillo REAL (PWM)
    int v = (pX - bx) * 100 / bw;
    if(v < 5) v = 5; if(v > 100) v = 100;
    if(v != gBright){ setBacklight(v); cfgSavePrefs(); dexOvMark(DXO_NOTIF); dexDirty = true; }
  }
  return true;
}
static bool dexRecentsTouch(){
  int list[4]; int n = dexRecList(list);
  if(pPressed){
    dexRecDrag = -1; dexRecDY = 0; dexRecY0 = pY;
    for(int i = 0; i < n; i++){
      int x, y; dexRecCard(i, n, x, y);
      if(dexInBox(pX, pY, x, y, DEX_REC_W, DEX_REC_H)){ dexRecDrag = i; break; }
    }
    if(dexRecDrag < 0){ dexOvClose(); return true; }
    return true;
  }
  if(pDown && dexRecDrag >= 0){
    int dy = pY - dexRecY0; if(dy > 0) dy = 0;
    if(dy != dexRecDY){
      int cx, cy; dexRecCard(dexRecDrag, n, cx, cy);
      dexRecDY = dy; dexMark(cx - 10, cx + DEX_REC_W + 10); dexDirty = true;
    }
    return true;
  }
  if(pReleased && dexRecDrag >= 0){
    int idx = dexRecDrag, dy = dexRecDY;
    dexRecDrag = -1; dexRecDY = 0;
    dexMarkAll(); dexDirty = true;
    if(idx >= n) return true;
    if(dy < -45){ dexCloseWin(list[idx]); if(n <= 1) dexOvClose(); return true; }   // arriba = cerrar
    if(pTap){
      int x, y; dexRecCard(idx, n, x, y);
      if(dexInBox(pX, pY, x + DEX_REC_W - 26, y, 26, 24)){                          // X
        dexCloseWin(list[idx]); if(n <= 1) dexOvClose();
      } else { dexOvClose(); dexRestore(list[idx]); }
    }
  }
  return true;
}

static void dexInput(){
  dexPointer();
  if(!pDown && !pPressed && !pReleased && !pTap && !pLong) return;
  // La barra reaparece al acercarse al borde inferior (o al tocarla), no con
  // cualquier toque: si no, "auto-ocultar" no ocultaria nunca.
  if(pDown && (pY >= LH - DEX_TB_REVEAL || pY >= dexTbY())) dexTbShow();

  // 1) Menu contextual: se lo come todo (y se cierra al tocar fuera)
  if(dexMenuOn){
    if(!pTap) return;
    int x, y, w, h; dexMenuGeom(x, y, w, h);
    if(!dexInBox(pX, pY, x, y, w, h)){ dexMenuOn = false; dexMarkAll(); dexDirty = true; return; }
    int i = (pY - y - 5) / DEX_MENU_IH;
    if(i >= 0 && i < dexMenuCount(dexMenuKind)) dexMenuRun(dexMenuKind, i);
    return;
  }

  // 2) Arrastre / redimension en curso
  if(dexGrab == DXG_MOVE){
    PWin* w = &pwins[dexGrabWin];
    if(pDown){
      int nx = pX - dexGrabDX, ny = pY - dexGrabDY;
      int nw = w->w, nh = w->h;
      // Clamp en CADA paso del arrastre, no solo al soltar.
      flxClampRect(nx, ny, nw, nh, LW, dexWorkBottom(), DEX_MIN_W, DEX_MIN_H, DEX_KEEP);
      if(nx != w->x || ny != w->y || nw != w->w || nh != w->h){
        dexMarkWin(w->x, w->w); dexMarkWin(nx, nw);
        w->x = nx; w->y = ny; w->w = nw; w->h = nh; dexDirty = true;
      }
      uint8_t g = dexSnapHit(pX, pY);
      if(g != dexSnapGhost){ dexSnapGhost = g; dexMarkAll(); dexDirty = true; }
      return;
    }
    uint8_t g = dexSnapGhost;
    int win = dexGrabWin;
    dexGrab = DXG_NONE; dexSnapGhost = SNAP_FREE;
    if(g != SNAP_FREE) dexApplySnap(win, g);
    else { w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h; }
    dexMarkAll(); dexDirty = true;
    return;
  }
  if(dexGrab == DXG_RESIZE){
    PWin* w = &pwins[dexGrabWin];
    if(pDown){
      int dx = pX - dexGrabDX, dy = pY - dexGrabDY, H = dexWorkBottom();
      int nx = dexRzX0, ny = dexRzY0, nw = dexRzW0, nh = dexRzH0;
      if(dexRzMask & 1){ nx = dexRzX0 + dx; nw = dexRzW0 - dx; }
      if(dexRzMask & 2){ nw = dexRzW0 + dx; }
      if(dexRzMask & 4){ ny = dexRzY0 + dy; nh = dexRzH0 - dy; }
      if(dexRzMask & 8){ nh = dexRzH0 + dy; }
      if(nx < 0){ nw += nx; nx = 0; }
      if(ny < 0){ nh += ny; ny = 0; }
      if(nw < DEX_MIN_W){ if(dexRzMask & 1) nx = dexRzX0 + dexRzW0 - DEX_MIN_W; nw = DEX_MIN_W; }
      if(nh < DEX_MIN_H){ if(dexRzMask & 4) ny = dexRzY0 + dexRzH0 - DEX_MIN_H; nh = DEX_MIN_H; }
      // Forzar el minimo podia volver a sacar la ventana: con nx alto, nw subia a
      // DEX_MIN_W DESPUES de recortar contra LW y el borde derecho se salia.
      // Aqui se acota el rect entero y luego se reencaja dentro del area util:
      // redimensionar nunca debe empujar la ventana fuera de lo visible.
      flxClampRect(nx, ny, nw, nh, LW, H, DEX_MIN_W, DEX_MIN_H, DEX_KEEP);
      if(nx + nw > LW) nx = LW - nw;
      if(ny + nh > H)  ny = H - nh;
      if(nx < 0) nx = 0;
      if(ny < 0) ny = 0;
      if(nx != w->x || ny != w->y || nw != w->w || nh != w->h){
        dexMarkWin(w->x, w->w); dexMarkWin(nx, nw);
        w->x = nx; w->y = ny; w->w = nw; w->h = nh; dexDirty = true;
      }
      return;
    }
    w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
    dexGrab = DXG_NONE; dexMarkAll(); dexDirty = true;
    return;
  }

  // 3) Overlays
  if(dexOv != DXO_NONE && !dexOvClosing){
    if(dexOv == DXO_DRAWER  && dexDrawerTouch())  return;
    if(dexOv == DXO_FINDER  && dexFinderTouch())  return;
    if(dexOv == DXO_NOTIF   && dexNotifTouch())   return;
    if(dexOv == DXO_RECENTS && dexRecentsTouch()) return;
  }

  // 4) Barra de tareas
  int ty = dexTbY();
  if(pY >= ty && ty < LH){
    if(pLong){ dexMenuOpen(1, pX - DEX_MENU_W / 2, ty - 12 - (10 + 4 * DEX_MENU_IH), -1); return; }
    if(!pTap) return;
    if(dexIn(pX, pY, dexRDrw)){
      if(dexOv == DXO_DRAWER && !dexOvClosing) dexOvClose(); else dexOvOpen(DXO_DRAWER);
      return;
    }
    if(dexIn(pX, pY, dexRFnd)){
      if(dexOv == DXO_FINDER && !dexOvClosing) dexOvClose(); else dexOvOpen(DXO_FINDER);
      return;
    }
    if(dexIn(pX, pY, dexRPad)){
      dexPadOn = !dexPadOn;
      if(dexPadOn){ dexCurX = LW / 2; dexCurY = LH / 2 - 40; }
      dexMarkAll(); dexDirty = true; return;
    }
    if(dexIn(pX, pY, dexRBell) || dexIn(pX, pY, dexRGear)){
      if(dexOv == DXO_NOTIF && !dexOvClosing) dexOvClose(); else dexOvOpen(DXO_NOTIF);
      return;
    }
    if(pX >= dexRClkX - 100 && pX <= dexRClkX){ dexOvOpen(DXO_RECENTS); return; }   // reloj -> Recientes
    int app[10]; bool op[10];
    int n = dexTbItems(app, op);
    for(int i = 0; i < n; i++){
      int x, y, s; dexTbItemRect(i, n, x, y, s);
      if(!dexInBox(pX, pY, x - 8, y - 6, s + 16, s + 14)) continue;
      int win = -1;
      for(int k = 0; k < 4; k++) if(pwins[k].open && pwins[k].app == app[i]) win = k;
      if(win < 0) dexOpenFrom(app[i], x, y, s);
      else if(pwins[win].mini) dexRestore(win);
      else if(win == dexFocus) dexMinimize(win);                // tocar la activa = minimizar
      else dexRaise(win);
      dexDirty = true;
      return;
    }
    return;
  }

  // 5) Ventanas
  int hit = dexWinAt(pX, pY);
  if(hit >= 0){
    PWin* w = &pwins[hit];
    if(pPressed){
      uint8_t m = dexResizeMask(hit, pX, pY);
      if(m){                                                    // redimensionar (8 zonas)
        dexRaise(hit);
        w->snap = SNAP_FREE;
        dexGrab = DXG_RESIZE; dexGrabWin = hit; dexRzMask = m;
        dexGrabDX = pX; dexGrabDY = pY;
        dexRzX0 = w->x; dexRzY0 = w->y; dexRzW0 = w->w; dexRzH0 = w->h;
        dexDirty = true; return;
      }
      // Zona de arrastre: la barra de titulo mas un margen por encima, para que
      // no haya una franja muerta entre el borde de la ventana y el titulo.
      if(dexInBox(pX, pY, w->x - 4, w->y - 6, w->w + 8, DEX_TTL_H + 6)){
        bool onBtn = false;
        for(int k = 0; k < 3; k++){
          int bx, by, bw, bh; dexWinBtnHit(w->x, w->y, w->w, k, bx, by, bw, bh);
          if(dexInBox(pX, pY, bx, by, bw, bh)) onBtn = true;
        }
        if(!onBtn){                                             // arrastrar por la barra de titulo
          dexRaise(hit);
          if(w->snap != SNAP_FREE){                             // "despegar" de su anclaje
            float fr = (w->w > 0) ? (float)(pX - w->x) / (float)w->w : 0.5f;
            w->w = w->rw; w->h = w->rh; w->snap = SNAP_FREE;
            w->x = pX - (int)(fr * w->w);
            w->y = pY - DEX_TTL_H / 2;
            dexClampWin(w);                       // el rect restaurado puede venir de otra area util
          }
          dexGrab = DXG_MOVE; dexGrabWin = hit;
          dexGrabDX = pX - w->x; dexGrabDY = pY - w->y;
          dexSnapGhost = SNAP_FREE;
          dexDirty = true; return;
        }
      }
      if(hit != dexFocus){ dexRaise(hit); dexDirty = true; }
    }
    if(pLong && dexInBox(pX, pY, w->x, w->y, w->w, DEX_TTL_H)){ dexMenuOpen(2, pX, pY, hit); return; }
    // AREA DE CLIENTE -> la app real. Nada de la barra de titulo, de sus tres
    // controles ni del marco de agarre llega aqui: para cuando se evalua esto,
    // esos casos ya han hecho return mas arriba.
    {
      int cx, cy, cw, ch; dexClientRect(hit, cx, cy, cw, ch);
      if(dexGrab == DXG_NONE && dexHost[hit].surf && dexInBox(pX, pY, cx, cy, cw, ch)){
        if(hit != dexFocus){ dexRaise(hit); dexDirty = true; }
        dexHostTouch(hit, cx, cy, cw, ch);
        dexHostServe(hit);
        return;
      }
    }
    if(pTap){
      for(int k = 0; k < 3; k++){
        int bx, by, bw, bh; dexWinBtnHit(w->x, w->y, w->w, k, bx, by, bw, bh);
        if(!dexInBox(pX, pY, bx, by, bw, bh)) continue;
        if(k == 0) dexMinimize(hit);
        else if(k == 1) dexToggleMax(hit);
        else dexCloseWin(hit);
        return;
      }
      // doble toque en la barra de titulo -> maximizar / restaurar
      if(pDTap && dexInBox(pX, pY, w->x, w->y, w->w, DEX_TTL_H)){ dexToggleMax(hit); return; }
    }
    return;
  }

  // 6) Escritorio
  if(pLong){ dexMenuOpen(0, pX, pY, -1); return; }
  if(pTap && dexOv != DXO_NONE) dexOvClose();
}

// #############################################################
// ##  PUNTOS DE ENTRADA DEL FRAMEWORK DE APPS (APP_REG)
// #############################################################
static void pcExit(){
  // pcExit se llama SIEMPRE desde dentro de dexInput() (menu de la barra o boton
  // del panel), o sea desde dentro de pcTick. appClose() ya nos deja en ST_HOME
  // con el escritorio pintado... y al volver, pcTick seguia su curso y llamaba a
  // dexPaint(), que repintaba la banda sucia de DeX ENCIMA del launcher y volvia
  // a poner gLand=true. De ahi salian las dos cosas a la vez: las franjas de la
  // barra/ventanas de DeX pegadas sobre el Home, y el launcher girado del que ya
  // no se salia. dexExiting corta el tick en seco en cuanto se pide la salida.
  dexExiting = true;
  gLand = false;
  pcStartOpen = false;
  dexOv = DXO_NONE; dexOvClosing = false;
  dexMenuOn = false; dexGrab = DXG_NONE;
  dexPadOn = false; dexPadGrab = false;
  dexSnapGhost = SNAP_FREE;
  dexDirty = false; dexBX0 = 0x7FFF; dexBX1 = -1;
  gClipY0 = 0; gClipY1 = SCR_H - 1;
  setBuf(fb);
  for(int i = 0; i < 4; i++) dexHostClose(i);       // libera los lienzos de las apps
  dexBgFree();                                      // 768 KB de PSRAM no se retienen fuera de DeX
  appClose();
}
static void pcOpen(int app){ dexOpen(app); }        // se conserva la firma original

static void pcEnter(){
  for(int i = 0; i < 4; i++){
    pwins[i].open = false; pwins[i].mini = false; pwins[i].snap = SNAP_FREE;
    dexOrder[i] = (uint8_t)i;
  }
  dexFocus = -1;
  dexOv = DXO_NONE; dexOvClosing = false;
  dexMenuOn = false; pcStartOpen = false;
  dexGrab = DXG_NONE; dexGrabWin = -1; dexSnapGhost = SNAP_FREE;
  dexRecDrag = -1; dexRecDY = 0;
  dexQLen = 0; dexQuery[0] = 0;
  dexAK = DXA_NONE; dexAW = -1;
  dexTbOff = 0; dexTbTgt = 0; dexTbFrom = 0; dexTbIdle = millis();
  dexPadOn = false; dexPadGrab = false;
  dexCurX = LW / 2; dexCurY = LH / 2;
  pDown = false; dexLongFired = false; dexTapMs = 0;
  for(int i = 0; i < 4; i++) dexHostClose(i);       // sin lienzos colgando de una sesion previa
  dexExiting = false; dexOvDone = false;
  gLand = true;
  dexBgBuild();                                     // cache del fondo (ver dexWallpaper)
  dexTbLayout();
  dexOpen(IC_MODOPC);                               // ventana de bienvenida
  pcRender();
}

static void pcTick(){
  if(dexExiting) return;                    // ya se pidio salir: no pintar NADA mas
  dexAnimTick();
  dexOvTick();
  dexTbAnimTick();
  dexTbLayout();
  dexInput();
  // dexInput() puede haber llamado a pcExit() (menu de la barra, boton del
  // panel). En ese caso ya estamos en ST_HOME y el escritorio esta pintado:
  // cualquier dibujo de aqui en adelante seria basura encima del launcher.
  if(dexExiting || !gLand) return;
  // Tick periodico de las apps hospedadas: es lo que mantiene vivo el reloj, el
  // calendario, Bienestar... Solo cuando cambia el minuto, no por frame: las
  // apps interactivas ya se refrescan en dexHostTouch.
  // Re-maquetado por cambio de tamano y avance de los fundidos.
  {
    bool wasFading = uiFading;
    uiFading = false;
    for(int k = 0; k < 4; k++) dexHostRelayout(dexOrder[k]);
    if(uiFading || wasFading) dexDirty = true;     // sigue pidiendo frames mientras funde
  }
  if(gMinChanged){
    for(int k = 0; k < 4; k++){
      int i = dexOrder[k];
      if(pwins[i].open && !pwins[i].mini && dexHost[i].surf){
        dexHostRun(i, false, true, NULL);
        dexHostServe(i);
      }
    }
    dexMarkAll(); dexDirty = true;
  }
  if(!dexDirty) return;
  // Pasos discretos ligados a millis(): ~33 fps de techo. No bloquea el loop ni
  // el tactil -- si aun no toca frame se sale, y se repinta en la siguiente
  // vuelta con la banda sucia ya acumulada.
  if(millis() - dexFrameMs < 30) return;
  dexFrameMs = millis();
  dexPaint(false);
}

// #############################################################
// ##  PANEL RAPIDO (cortina deslizable estilo Android/iOS)
// ##  Arrastre desde el borde superior + glassmorphism + 4 controles.
// #############################################################
static int  qsPanelY = 0;        // 0 = oculto, SCR_H = abierto del todo
static bool qsDragging = false;
static bool qsPower = false;             // Ahorro Ultra -- el UNICO bool de estado que hacia falta (Wi-Fi/BT se quitaron: no tenian radio real detras, ver resumen de cambios)
static int  qsFlashIdx = -1;             // control con destello de toque activo (-1 = ninguno). Usa el mismo indice que qsTileIcon: 3=Modo PC, 4=Ahorro Ultra, 5=Ajustes
static unsigned long qsFlashMs = 0;      // millis() del toque que disparo el destello
#define QS_FLASH_DUR_MS 220              // duracion del destello de feedback al tocar

// Geometria centralizada del panel rapido, estilo Control Center de iOS.
// A PROPOSITO solo hay 4 controles -- son los UNICOS respaldados por una
// funcion real en este archivo (ver qsApplyPower/enterApp/setBacklight
// mas abajo). No se incluyen Wi-Fi, Bluetooth ni bateria (%) porque ese
// codigo no controla ningun radio real ni lee ningun sensor real todavia.
#define QS_CAP_X 24                      // capsula vertical de Brillo (PWM real)
#define QS_CAP_Y 92
#define QS_CAP_W 140
#define QS_CAP_H 330
#define QS_CAP_R (QS_CAP_W / 2)
#define QS_CIRC_D 90                     // columna de circulos: Modo PC / Ajustes
#define QS_CIRC_R (QS_CIRC_D / 2)
#define QS_CIRC_CX (QS_CAP_X + QS_CAP_W + 18 + (SCR_W - 24 - (QS_CAP_X + QS_CAP_W + 18)) / 2)
// Los dos circulos se centran VERTICALMENTE contra la capsula de Brillo (que
// va de QS_CAP_Y a QS_CAP_Y+QS_CAP_H): con la columna de 3 de antes bastaba
// con apilarlos desde arriba, pero con 2 eso dejaba un hueco muerto abajo.
// Con POWEROFF_ON el tercer circulo es Apagar. La formula de QS_CIRC_CY sigue
// centrando la columna contra la capsula sea cual sea N, asi que desactivar el
// apagado devuelve el panel exactamente al layout de 2 que habia antes.
#define QS_CIRC_N (POWEROFF_ON ? 3 : 2)
#define QS_CIRC_GAP 120
#define QS_CIRC_CY(i) (QS_CAP_Y + QS_CAP_H / 2 - ((QS_CIRC_N - 1) * QS_CIRC_GAP) / 2 + (i) * QS_CIRC_GAP)
#define QS_PILL_X 24                     // pastilla Ahorro Ultra (cambia la frecuencia real de la CPU)
#define QS_PILL_Y (QS_CAP_Y + QS_CAP_H + 18)
#define QS_PILL_W (SCR_W - 48)
#define QS_PILL_H 72
#define QS_PILL_R (QS_PILL_H / 2)

static void qsTileIcon(int idx, int cx, int cy, uint16_t col){
  switch(idx){
    case 3:                                                      // Modo PC (monitor)
      drawRoundRect(cx - 14, cy - 11, 28, 20, 3, col);
      fillRect(cx - 4, cy + 9, 8, 4, col); fillRect(cx - 10, cy + 13, 20, 3, col); break;
    case 4:                                                      // Ahorro (bateria+rayo)
      drawRoundRect(cx - 12, cy - 8, 22, 16, 3, col); fillRect(cx + 10, cy - 3, 3, 6, col);
      fillTriangle(cx - 2, cy - 6, cx + 3, cy - 6, cx - 1, cy, col);
      fillTriangle(cx - 1, cy, cx + 4, cy, cx - 2, cy + 6, col); break;
    case 5: drawSetCatIcon(0, cx - 14, cy - 14, 28, col); break; // Ajustes (engranaje)
    case 6: {                                                    // Apagar (simbolo IEC 5009)
      // Arco abierto por arriba + barra vertical.
      // OJO: arcStroke toma los angulos en GRADOS (multiplica por pi/180 por
      // dentro), no en radianes -- igual que el resto de llamadas del archivo.
      // 0 grados apunta a la derecha y 270 hacia ARRIBA (la Y crece hacia
      // abajo), asi que el arco va de -68 a 248 y el hueco que queda (248..292)
      // cae centrado justo en 270 = arriba, que es donde entra la barra.
      arcStroke(cx, cy, 11, -68, 248, 3, col);
      fillRect(cx - 1, cy - 14, 3, 13, col);
      break;
    }
  }
}
// Boton circular de accion (Modo PC / Ajustes). idx usa la misma
// numeracion que qsTileIcon. rad = d/2 en un cuadro d x d = circulo perfecto
// (mismo truco que usan los iconos redondos en el resto del sistema).
static void qsCircleBtn(int idx, int cx, int cy){
  int d = QS_CIRC_D, x = cx - d / 2, y = cy - d / 2;
  bool danger = (idx == 6);                                                // Apagar: acento rojo, como en iOS/Android
  fillRoundRectA(x + 2, y + 3, d, d, d / 2, rgb565(0,0,0), 55);            // sombra sutil
  if(uiGlass) drawLiquidGlassPanel(x, y, d, d, d / 2, danger ? rgb565(96,44,50) : rgb565(56,62,86));
  else fillRoundRect(x, y, d, d, d / 2, danger ? rgb565(72,32,38) : rgb565(38,42,56));
  drawRoundRect(x, y, d, d, d / 2, danger ? rgb565(190,90,90) : rgb565(90,98,120));   // borde sutil
  qsTileIcon(idx, cx, cy - 10, danger ? rgb565(255,120,110) : rgb565(220,224,236));
  const char* lab = idx == 3 ? "Modo PC" : idx == 6 ? "Apagar" : "Ajustes";
  drawTextC(cx, cy + 20, lab, 1, danger ? rgb565(240,170,170) : rgb565(190,196,212));
}
// Pastilla ancha de toggle (Ahorro Ultra -- el unico toggle real que queda).
static void qsTogglePill(int x, int y, int w, int h, bool on){
  fillRoundRectA(x + 2, y + 4, w, h, h / 2, rgb565(0,0,0), 55);
  if(uiGlass) drawLiquidGlassPanel(x, y, w, h, h / 2, on ? rgb565(255,150,40) : rgb565(56,62,86));
  else fillRoundRect(x, y, w, h, h / 2, on ? rgb565(210,110,20) : rgb565(38,42,56));
  drawRoundRect(x, y, w, h, h / 2, on ? rgb565(255,190,110) : rgb565(90,98,120));
  qsTileIcon(4, x + h / 2, y + h / 2, rgb565(255,255,255));
  drawText(x + h + 12, y + 14, "Ahorro Ultra", 2, rgb565(255,255,255));
  drawText(x + h + 12, y + 40, on ? "Activado - 160 MHz" : "Desactivado - 360 MHz", 1, rgb565(220,224,236));
}
static uint16_t* qsBuf = NULL;      // cortina precompuesta (para arrastre fluido)
static int  qsLastY = 0;
// qsDirty se declara arriba, junto a gHomeDirty (invalidacion de caches).

// dibuja titulo + tiles + etiqueta y pista del slider en el gBuf actual (sin relleno/perilla)
static void qsDrawContent(){
  drawText(24, 40, "Ajustes r\xC3\xA1pidos", 3, rgb565(240,244,252));
  // Brillo: pista de la capsula vertical (el relleno ambar es dinamico, se
  // pinta cada frame en qsRender porque cambia con el arrastre / PWM real)
  fillRoundRectA(QS_CAP_X + 3, QS_CAP_Y + 5, QS_CAP_W, QS_CAP_H, QS_CAP_R, rgb565(0,0,0), 55);
  if(uiGlass) drawLiquidGlassPanel(QS_CAP_X, QS_CAP_Y, QS_CAP_W, QS_CAP_H, QS_CAP_R, rgb565(40,46,64));
  else fillRoundRect(QS_CAP_X, QS_CAP_Y, QS_CAP_W, QS_CAP_H, QS_CAP_R, rgb565(32,36,50));
  drawRoundRect(QS_CAP_X, QS_CAP_Y, QS_CAP_W, QS_CAP_H, QS_CAP_R, rgb565(80,86,106));
  // Modo PC / Ajustes: columna de 2 circulos (acciones reales)
  qsCircleBtn(3, QS_CIRC_CX, QS_CIRC_CY(0));
  qsCircleBtn(5, QS_CIRC_CX, QS_CIRC_CY(1));
#if POWEROFF_ON
  qsCircleBtn(6, QS_CIRC_CX, QS_CIRC_CY(2));      // Apagar -> pantalla de confirmacion
#endif
  // Ahorro Ultra: pastilla ancha (toggle real, cambia la frecuencia de la CPU)
  qsTogglePill(QS_PILL_X, QS_PILL_Y, QS_PILL_W, QS_PILL_H, qsPower);
}
// compone la cortina COMPLETA una sola vez en qsBuf (parte cara)
static void qsCompose(){
  if(!qsBuf) qsBuf = (uint16_t*)heap_caps_malloc((size_t)SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!qsBuf) return;
  memcpy(qsBuf, homeBuf, (size_t)SCR_W * SCR_H * 2);
  setBuf(qsBuf);
  int c0 = gClipY0, c1 = gClipY1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  if(uiGlass){
    drawLiquidGlassPanelEx(0, 0, SCR_W, SCR_H, 0, rgb565(26,34,60), 11);   // cortina Liquid Glass (blur mas fuerte que el resto del sistema; ver drawLiquidGlassPanelEx)
    fillRectA(0, 0, SCR_W, SCR_H, rgb565(12,14,24), 140);                       // oscurecer para legibilidad (sin tocar: preserva el contraste ya afinado)
  } else {
    fillRectA(0, 0, SCR_W, SCR_H, rgb565(14,16,26), 234);                       // glassmorphism plano
    fillRectA(0, 0, SCR_W, 130, rgb565(30,42,74), 46);                          // tinte superior
  }
  qsDrawContent();
  gClipY0 = c0; gClipY1 = c1;
  setBuf(fb);
  qsDirty = false;
}
// por-frame: SOLO copia la banda revelada (memcpy) + relleno del slider + flush de banda
static void qsRender(){
  if(qsPanelY <= 0){ blitToFb(homeBuf); flxFlushAll(); qsLastY = 0; return; }
  if(qsDirty || !qsBuf) qsCompose();
  if(!qsBuf){ blitToFb(homeBuf); flxFlushAll(); return; }
  setBuf(bbuf);
  int py = qsPanelY < SCR_H ? qsPanelY : SCR_H;
  for(int j = 0; j < py; j++) memcpy(bbuf + (size_t)j * SCR_W, qsBuf + (size_t)j * SCR_W, SCR_W * 2);
  int maxY = py > qsLastY ? py : qsLastY; if(maxY > SCR_H) maxY = SCR_H;
  for(int j = py; j < maxY; j++) memcpy(bbuf + (size_t)j * SCR_W, homeBuf + (size_t)j * SCR_W, SCR_W * 2);
  uint32_t t = millis();                        // la usan el destello de toque y la sombra de abajo
  // RECORTE AL BORDE DE LA CORTINA. La capsula de Brillo ocupa y=92..422 y solo
  // se comprobaba "if(QS_CAP_Y < py)": con la cortina a medio abrir (py~100) se
  // pintaban >300 filas POR DEBAJO del borde, encima del escritorio. Eso producia
  // (a) la barra ambar saliendose de la tarjeta, (b) la pastilla amarilla flotando
  // sobre los iconos y (c) restos que se quedaban PEGADOS: qsRender solo restaura
  // hasta max(py, qsLastY), asi que lo pintado mas abajo nunca se limpiaba.
  // Recortando a py, ningun elemento de la cortina puede salirse de ella.
  const int oCY1 = gClipY1, oCY0 = gClipY0, oCX0 = gClipX0, oCX1 = gClipX1;
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = py - 1;
  if(qsFlashIdx >= 0){                                               // destello de feedback al tocar (3=Modo PC,4=Ahorro Ultra,5=Ajustes)
    uint32_t e = t - qsFlashMs;
    if(e < QS_FLASH_DUR_MS){
      float p = 1.0f - (float)e / QS_FLASH_DUR_MS;                    // se desvanece
      uint8_t a = (uint8_t)(120 * p);
      if(qsFlashIdx == 4){
        if(QS_PILL_Y < py) fillRoundRectA(QS_PILL_X, QS_PILL_Y, QS_PILL_W, QS_PILL_H, QS_PILL_R, rgb565(255,255,255), a);
      } else {
        int ci = qsFlashIdx == 5 ? 1 : qsFlashIdx == 6 ? 2 : 0;        // 3->0, 5->1, 6->2
        int cy = QS_CIRC_CY(ci);
        if(cy - QS_CIRC_R < py) fillRoundRectA(QS_CIRC_CX - QS_CIRC_R, cy - QS_CIRC_R, QS_CIRC_D, QS_CIRC_D, QS_CIRC_R, rgb565(255,255,255), a);
      }
    } else qsFlashIdx = -1;
  }
  // Brillo: relleno ambar dinamico (cambia con el arrastre / PWM real) + icono + %
  if(QS_CAP_Y < py){
    int fillH = QS_CAP_H * gBright / 100;
    if(fillH > 0) fillRoundRect(QS_CAP_X, QS_CAP_Y + QS_CAP_H - fillH, QS_CAP_W, fillH, QS_CAP_R, rgb565(255,190,40));
    drawSetCatIcon(1, QS_CAP_X + QS_CAP_W / 2 - 14, QS_CAP_Y + QS_CAP_H - 44, 28, rgb565(70,50,10));   // sol: casi siempre cae sobre el relleno
    char pb[8]; snprintf(pb, sizeof(pb), "%d%%", gBright);
    drawTextC(QS_CAP_X + QS_CAP_W / 2, QS_CAP_Y + QS_CAP_H - 74, pb, 2, gBright >= 25 ? rgb565(70,50,10) : rgb565(255,255,255));
  }
  gClipY0 = oCY0; gClipY1 = oCY1; gClipX0 = oCX0; gClipX1 = oCX1;      // fin del recorte a la cortina
  int shY = py, shEnd = shY + 18; if(shEnd > maxY) shEnd = maxY;       // sombra suave bajo el borde movil de la cortina
  for(int yy = shY; yy < shEnd; yy++){
    uint8_t a = (uint8_t)(70 * (1.0f - (float)(yy - shY) / 18.0f));
    if(a > 0) hLineA(0, yy, SCR_W, rgb565(0,0,0), a);
  }
  fillRoundRect(SCR_W / 2 - 28, py - 14, 56, 5, 2, rgb565(180,185,200));
  present(0, maxY);
  qsLastY = py;
}
// Posicion interpolada de la cortina en coma flotante. La comparte el arrastre
// (suavizado) y la animacion, asi que soltar el dedo continua desde donde
// estaba EXACTAMENTE, sin el micro-salto de reenganche.
static float qsPosF = 0;
// Animacion de la cortina, BASADA EN TIEMPO (antes: 16 pasos fijos con
// delay(12), o sea una duracion que dependia de lo que costara cada cuadro).
// Curva ease-in-out cubica sin rebote: el easeOutBack de antes se pasaba del
// destino y habia que recortarlo contra 0 y SCR_H, lo que producia un frenazo
// seco justo al cerrar. Ahora arranca y asienta suave, y como el avance sale
// del reloj el recorrido dura lo mismo a 30 o a 90 fps.
static void qsAnimTo(int target){
  int from = qsPanelY;
  if(target < 0) target = 0; if(target > SCR_H) target = SCR_H;
  int dist = target > from ? target - from : from - target;
  if(dist > 0){
    uint32_t dur = 150 + (uint32_t)((uint32_t)dist * 150u / (uint32_t)SCR_H);   // 150..300 ms segun recorrido
    uint32_t t0 = millis();
    for(;;){
      uint32_t e = millis() - t0; if(e > dur) e = dur;
      float p = (float)e / (float)dur;
      float ip = 1.0f - p;
      p = (p < 0.5f) ? (4.0f * p * p * p) : (1.0f - 4.0f * ip * ip * ip);       // ease-in-out cubica
      int ny = from + (int)((target - from) * p + (target > from ? 0.5f : -0.5f));
      if(ny < 0) ny = 0; if(ny > SCR_H) ny = SCR_H;
      // Sin delay fijo: el ritmo lo marca el panel. Si el reloj todavia no ha
      // movido la posicion, se cede el turno en vez de repetir un cuadro
      // identico (mismo recorrido, sin quemar ciclos ni calentar de balde).
      if(ny != qsPanelY){ qsPanelY = ny; qsRender(); }
      else if(e < dur) delay(1);
      if(e >= dur) break;
    }
  }
  qsPanelY = target; qsPosF = (float)target; qsRender();
  if(target <= 0){ qsPanelY = 0; blitToFb(homeBuf); flxFlushAll(); }
}
static void qsApplyPower(){ setCpuFrequencyMhz(qsPower ? 160 : 360); }
static bool qsTapTile(int px, int py){
#if POWEROFF_ON
  const int idxOf[QS_CIRC_N] = { 3, 5, 6 };            // Modo PC, Ajustes, Apagar
#else
  const int idxOf[QS_CIRC_N] = { 3, 5 };               // Modo PC, Ajustes
#endif
  for(int i = 0; i < QS_CIRC_N; i++){
    int cy = QS_CIRC_CY(i), dx = px - QS_CIRC_CX, dy = py - cy;
    if(dx * dx + dy * dy <= QS_CIRC_R * QS_CIRC_R){     // hit-test circular real (no la caja cuadrada)
      qsFlashIdx = idxOf[i]; qsFlashMs = millis();       // destello de feedback al tocar (ver overlay en qsRender)
      switch(idxOf[i]){
        case 3: qsPanelY = 0; enterApp(IC_MODOPC); return true;      // -> Modo PC
        case 5: qsPanelY = 0; enterApp(IC_AJUSTES); return true;     // -> Ajustes
        // Apagar NO apaga aqui: lleva a la pantalla de confirmacion (ST_POWEROFF_CONFIRM).
        case 6: qsPanelY = 0; qsFlashIdx = -1; poffEnter(); return true;
      }
    }
  }
  if(px >= QS_PILL_X && px <= QS_PILL_X + QS_PILL_W && py >= QS_PILL_Y && py <= QS_PILL_Y + QS_PILL_H){
    qsFlashIdx = 4; qsFlashMs = millis();
    qsPower = !qsPower; qsApplyPower();
    qsDirty = true; qsRender(); return true;
  }
  return false;
}
// Devuelve true si la cortina consumio el toque (esta activa)
// ARRASTRE DE LA CORTINA (reescrito).
//   · Antes: qsPanelY = T.y en crudo. Con la cortina ABIERTA (py = 800) y el
//     dedo agarrando el borde superior, el primer cuadro del gesto la mandaba
//     de 800 a ~20 de golpe: un salto de casi toda la pantalla en un solo
//     cuadro, imposible de seguir para el compositor y visible como trozos de
//     vidrio "sueltos" a media pantalla.
//   · Ahora: se guarda la posicion al AGARRAR y el dedo mueve un DELTA (1:1).
//     Abrir y cerrar usan la misma formula y ninguna de las dos salta.
//   · La velocidad se mide en px/ms y el suavizado usa una constante de TIEMPO,
//     no un factor por cuadro: la respuesta es identica a cualquier cadencia y
//     el umbral de "flick" ya no depende de cuantos cuadros diera el sistema.
#define QS_SMOOTH_TAU 26.0f      // ms: constante del suavizado de posicion
#define QS_VEL_TAU    45.0f      // ms: constante del filtro de velocidad
#define QS_FLICK      0.45f      // px/ms (~450 px/s) para considerarlo un lanzamiento
static float qsVel = 0; static int qsPrevY = 0;
static uint32_t qsPrevMs = 0;
static int qsDragBase = 0, qsDragY0 = 0;
static bool qsDragMoved = false;         // el gesto ya paso de umbral: es arrastre, no toque
// Zona por la que se puede agarrar la cortina ABIERTA para cerrarla: el borde
// superior de siempre y toda el area vacia de debajo de la pastilla (donde esta
// el asa y donde ya se podia tocar para cerrar). Los controles quedan fuera, asi
// que arrastrar sobre un boton sigue siendo tocar ese boton.
static inline bool qsGrabZone(int y){ return y < 30 || y > QS_PILL_Y + QS_PILL_H; }
// Punto de agarre: fija el origen del delta y sincroniza el interpolador con la
// posicion actual, para que el gesto arranque sin escalon.
static void qsGrab(){
  qsDragging = true; qsDragMoved = false;
  qsDragBase = qsPanelY; qsDragY0 = T.y;
  qsPosF = (float)qsPanelY;
  qsPrevY = T.y; qsPrevMs = millis(); qsVel = 0;
}
static bool qsHandle(){
  if(qsDragging){
    if(T.down){
      // Hasta que el dedo no pasa de 6 px no se mueve nada: asi un toque en el
      // area vacia sigue siendo un toque y no un arrastre de 1 px.
      if(!qsDragMoved){
        if(abs(T.y - qsDragY0) <= 6){ qsPrevY = T.y; qsPrevMs = millis(); return true; }
        qsDragMoved = true;
      }
      uint32_t now = millis();
      float dt = (float)(now - qsPrevMs);
      if(dt < 1.0f) dt = 1.0f; if(dt > 100.0f) dt = 100.0f;   // acota el dt de un cuadro perdido
      int target = qsDragBase + (T.y - qsDragY0);             // 1:1 con el dedo, sin saltos
      if(target < 0) target = 0; if(target > SCR_H) target = SCR_H;
      float inst = (float)(T.y - qsPrevY) / dt;               // px/ms
      qsVel += (inst - qsVel) * (dt / (dt + QS_VEL_TAU));
      qsPrevY = T.y; qsPrevMs = now;
      float a = 1.0f - expf(-dt / QS_SMOOTH_TAU);             // suavizado corregido por tiempo
      qsPosF += ((float)target - qsPosF) * a;
      if(fabsf((float)target - qsPosF) < 0.75f) qsPosF = (float)target;   // asienta exacto
      int ny = (int)(qsPosF + 0.5f);
      if(ny < 0) ny = 0; if(ny > SCR_H) ny = SCR_H;
      if(ny != qsPanelY){ qsPanelY = ny; qsRender(); }
    } else {
      qsDragging = false;
      if(!qsDragMoved){
        // No llego a ser arrastre: se resuelve como toque, exactamente igual que
        // antes (boton del panel, o cerrar tocando el area vacia).
        qsPanelY = qsDragBase; qsPosF = (float)qsDragBase;
        if(T.tap && qsPanelY >= SCR_H){
          if(!qsTapTile(T.x, T.y) && T.y > QS_PILL_Y + QS_PILL_H) qsAnimTo(0);
        } else if(qsPanelY < SCR_H && qsPanelY > 0) qsAnimTo(qsPanelY < SCR_H / 2 ? 0 : SCR_H);
        else qsRender();
        return true;
      }
      if(qsVel > QS_FLICK) qsAnimTo(SCR_H);                 // lanzamiento hacia abajo -> abrir
      else if(qsVel < -QS_FLICK) qsAnimTo(0);               // lanzamiento hacia arriba -> cerrar
      else if(qsPanelY < SCR_H / 2) qsAnimTo(0); else qsAnimTo(SCR_H);   // si no, por posicion
    }
    return true;
  }
  if(qsPanelY >= SCR_H){
    if(T.down && T.x >= QS_CAP_X - 14 && T.x <= QS_CAP_X + QS_CAP_W + 14 && T.y >= QS_CAP_Y - 14 && T.y <= QS_CAP_Y + QS_CAP_H + 14){
      int v = (QS_CAP_Y + QS_CAP_H - T.y) * 100 / QS_CAP_H; if(v < 0) v = 0; if(v > 100) v = 100;   // arriba = 100%, abajo = 0%
      setBacklight(v); qsRender(); return true;                 // brillo real (PWM)
    }
    // Cerrar arrastrando: el asa de abajo (o el borde de arriba) mueve la
    // cortina 1:1 con el dedo. Antes solo se podia agarrar arriba y la cortina
    // saltaba de 800 a ~20 en un cuadro; ese salto es el que se veia como
    // "trozos de vidrio sueltos" a mitad de pantalla.
    if(T.pressed && qsGrabZone(T.startY)){ qsGrab(); return true; }
    if(T.swipeUp){ qsAnimTo(0); return true; }
    if(T.tap){ if(!qsTapTile(T.x, T.y) && T.y > QS_PILL_Y + QS_PILL_H) qsAnimTo(0); return true; }
    return true;                                                // consume todo mientras abierto
  }
  // cerrado: capturar arrastre desde la zona caliente (borde superior 0..30 px).
  // qsPanelY NO se salta a T.y: el gesto empieza en 0 y la cortina sale del
  // borde siguiendo el delta del dedo (misma formula que al cerrar).
  if(T.pressed && T.startY < 30){ qsDirty = true; qsGrab(); qsRender(); return true; }
  return false;
}

// #############################################################
// ##  APP MULTIMEDIA (ESQUELETO reproductor de video)
// ##  Doble buffer ping-pong en PSRAM + control de FPS por millis().
// ##  FUENTE = patron sintetico. Para video real, sustituir
// ##  vidDecodeFrame() por: leer MJPEG/.bin de la SD + decodificar.
// #############################################################
#define VID_W     448
#define VID_H     252
#define VID_RX    16
#define VID_RY    64
#define VID_TOTAL 300           // frames simulados (10 s @ 30 fps)
#define VID_FPS_MS 33           // 33 ms ~ 30 fps

static uint16_t *vidBufA = NULL, *vidBufB = NULL, *vidFront = NULL, *vidBack = NULL;
static bool vidPlaying = false;
static int  vidFrame = 0;
static unsigned long vidLastMs = 0;

// <<< PUNTO DE PORTABILIDAD >>> aqui iria: SD.open(archivo) + JPEGDEC/esp_jpeg
// para decodificar el frame f dentro de 'buf'. Ahora: patron animado.
static void vidDecodeFrame(uint16_t* buf, int f){
  for(int y = 0; y < VID_H; y++){
    uint16_t* row = buf + (size_t)y * VID_W;
    for(int x = 0; x < VID_W; x++)
      row[x] = rgb565((uint8_t)(x + f * 3), (uint8_t)(y * 2 + f * 2), (uint8_t)((x + y) / 2 + f * 4));
  }
}
static void vidBlit(){                 // vuelca el buffer FRONT al area de render
  const int cx0 = 0, cx1 = SCR_W - 1;
  const int cy0 = 0, cy1 = SCR_H - 1;
  int drawn0 = SCR_H, drawn1 = -1;
  for(int y = 0; y < VID_H; y++){
    const int dy = VID_RY + y;
    if(dy < cy0 || dy > cy1) continue;
    const uint16_t* srcRow = vidFront + (size_t)y * VID_W;
    int x0 = VID_RX, x1 = x0 + VID_W - 1;
    if(x0 < cx0){ srcRow += (size_t)(cx0 - x0); x0 = cx0; }   // recorta por la izquierda (avanza el origen)
    if(x1 > cx1) x1 = cx1;                                     // recorta por la derecha
    if(x0 > x1) continue;
    memcpy(fb + (size_t)dy * SCR_W + x0, srcRow, (size_t)(x1 - x0 + 1) * 2);
    if(dy < drawn0) drawn0 = dy;
    if(dy > drawn1) drawn1 = dy;
  }
  if(drawn1 >= drawn0) flxFlush(drawn0, drawn1);              // solo lo realmente escrito
}
static void vidDrawSeek(){
  int sbx = 24, sby = 434, sbw = SCR_W - 48;
  setBuf(fb);
  fillRoundRect(sbx, sby, sbw, 8, 4, rgb565(50,54,68));
  fillRoundRect(sbx, sby, sbw * vidFrame / VID_TOTAL, 8, 4, rgb565(80,160,240));
  fillCircle(sbx + sbw * vidFrame / VID_TOTAL, sby + 4, 10, rgb565(255,255,255));
  char tc[24]; snprintf(tc, sizeof(tc), "%d / %d", vidFrame, VID_TOTAL);
  fillRect(sbx, sby + 18, 160, 20, rgb565(10,12,18));
  drawText(sbx, sby + 18, tc, 2, rgb565(150,158,180));
  flxFlush(sby - 12, sby + 40);
}
static void vidDrawControls(){
  setBuf(fb);
  int pcx = SCR_W / 2, pcy = 366;
  fillCircle(pcx, pcy, 30, rgb565(50,110,235));           // play/pausa
  if(vidPlaying){ fillRect(pcx - 9, pcy - 12, 6, 24, rgb565(255,255,255)); fillRect(pcx + 3, pcy - 12, 6, 24, rgb565(255,255,255)); }
  else fillTriangle(pcx - 8, pcy - 12, pcx - 8, pcy + 12, pcx + 12, pcy, rgb565(255,255,255));
  int scx = pcx + 92;
  fillCircle(scx, pcy, 22, rgb565(60,64,78));             // stop
  fillRect(scx - 8, pcy - 8, 16, 16, rgb565(230,90,90));
  flxFlush(pcy - 34, pcy + 34);
}
static void vidRenderAll(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(10,12,18));
  strokeSegAA(30, 26, 18, 18, 2.4f, rgb565(255,255,255));  // back (esq. sup-izq)
  strokeSegAA(18, 18, 30, 10, 2.4f, rgb565(255,255,255));
  drawTextC(SCR_W / 2, 14, "Multimedia", 3, rgb565(255,255,255));
  drawRoundRect(VID_RX - 2, VID_RY - 2, VID_W + 4, VID_H + 4, 6, rgb565(40,44,58));
  vidBlit();
  vidDrawControls();
  vidDrawSeek();
  drawTextC(SCR_W / 2, 476, "Fuente: patron de prueba (enchufa SD + MJPEG)", 1, rgb565(120,128,150));
  flxFlushAll();
}
static void vidEnter(){
  if(!vidBufA){                       // doble buffer en PSRAM (una sola vez)
    size_t bytes = (size_t)VID_W * VID_H * 2;
    vidBufA = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    vidBufB = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  vidFront = vidBufA; vidBack = vidBufB;
  vidPlaying = false; vidFrame = 0; vidLastMs = millis();
  if(vidFront) vidDecodeFrame(vidFront, 0);
  vidRenderAll();
}
static void vidTick(){
  // Reproduccion no-bloqueante: un frame exacto cada VID_FPS_MS
  if(vidPlaying && vidFront && (millis() - vidLastMs) >= VID_FPS_MS){
    vidLastMs = millis();
    vidFrame++; if(vidFrame >= VID_TOTAL) vidFrame = 0;
    vidDecodeFrame(vidBack, vidFrame);                    // decodifica en BACK...
    uint16_t* t = vidFront; vidFront = vidBack; vidBack = t;  // ...intercambia (ping-pong)...
    vidBlit();                                            // ...y dibuja FRONT
    vidDrawSeek();
  }
  if(!T.tap) return;
  if(T.x < 48 && T.y < 48){ appClose(); return; }         // back
  int pcx = SCR_W / 2, pcy = 366, scx = pcx + 92;
  int sbx = 24, sby = 434, sbw = SCR_W - 48;
  if(T.x >= pcx - 34 && T.x <= pcx + 34 && T.y >= pcy - 34 && T.y <= pcy + 34){
    vidPlaying = !vidPlaying; vidLastMs = millis(); vidDrawControls();
  } else if(T.x >= scx - 26 && T.x <= scx + 26 && T.y >= pcy - 26 && T.y <= pcy + 26){
    vidPlaying = false; vidFrame = 0;
    if(vidFront) vidDecodeFrame(vidFront, 0);
    vidBlit(); vidDrawControls(); vidDrawSeek();           // stop: libera/rebobina
  } else if(T.x >= sbx - 12 && T.x <= sbx + sbw + 12 && T.y >= sby - 14 && T.y <= sby + 16){
    int fr = (T.x - sbx) * VID_TOTAL / sbw; if(fr < 0) fr = 0; if(fr >= VID_TOTAL) fr = VID_TOTAL - 1;
    vidFrame = fr;                                         // seek: mueve el puntero
    if(vidFront) vidDecodeFrame(vidFront, vidFrame);
    vidBlit(); vidDrawSeek();
  }
}

// #############################################################
// ##  APP CAMARA (ESQUELETO estilo iPhone)
// ##  Zoom digital x1..x50 por recorte-central + reescalado (funciona
// ##  sobre una ESCENA SINTETICA en PSRAM). EIS = scaffold (offset).
// ##  FUENTE = escena generada. Para camara real, sustituir camGenScene()
// ##  por la captura del sensor (esp_cam / DVP) hacia camScene.
// #############################################################
#define CAM_SW SCR_W
#define CAM_SH SCR_H
static uint16_t* camScene = NULL;   // "sensor" simulado en PSRAM
static float camZoom = 1.0f;        // x1..x50
static bool  camRec = false, camNight = false, camRaw = false;
static int   camMode = 0;           // 0 FOTO,1 VIDEO,2 CINE,3 ACCION,4 PRORES
static int   camExpo = 50;          // 0..100
static int   camEisX = 0, camEisY = 0;   // <<< EIS: offset suavizado (real: de vectores de movimiento)

// <<< HOOK DE CAMARA REAL >>>
// Esta placa (JC4880P443C) SI tiene camara (MIPI-CSI en el ESP32-P4). Para
// capturar de verdad hace falta el driver del sensor (esp_video / esp_cam_sensor
// de ESP-IDF) y el MODELO exacto del sensor + su pinout, que no puedo adivinar.
// Cuando lo tengas: pon CAM_HAS_SENSOR en 1 e implementa camCapture() para volcar
// un frame del sensor frontal en 'dst' (CAM_SW x CAM_SH, RGB565). El resto del
// pipeline (zoom, EIS, UI, grabacion) ya esta listo y usara esos frames reales.
#define CAM_HAS_SENSOR 0
static bool camCapture(uint16_t* dst){
#if CAM_HAS_SENSOR
  // TODO: capturar frame del sensor aqui -> dst ; return true si es valido
  (void)dst; return false;
#else
  (void)dst; return false;   // sin sensor configurado -> patron de prueba
#endif
}
// Patron de prueba honesto (NO es la camara): barras de color + anillos para
// el zoom + aviso "SIN SENAL". Se muestra hasta cablear camCapture().
static void camGenScene(){
  gClipY0 = 0; gClipY1 = SCR_H - 1;
  setBuf(camScene);
  uint16_t bars[7] = { rgb565(200,200,200), rgb565(210,210,0), rgb565(0,200,210),
                       rgb565(0,200,0), rgb565(210,0,210), rgb565(210,0,0), rgb565(0,0,210) };
  int bw = CAM_SW / 7;
  for(int i = 0; i < 7; i++) fillRect(i * bw, 0, (i == 6 ? CAM_SW - 6 * bw : bw), CAM_SH, bars[i]);
  for(int r = 20; r < 300; r += 20) drawCircle(CAM_SW / 2, CAM_SH / 2, r, rgb565(255,255,255));  // detalle para zoom
  strokeSeg(CAM_SW / 2 - 30, CAM_SH / 2, CAM_SW / 2 + 30, CAM_SH / 2, 2, rgb565(255,255,255));
  strokeSeg(CAM_SW / 2, CAM_SH / 2 - 30, CAM_SW / 2, CAM_SH / 2 + 30, 2, rgb565(255,255,255));
  fillRoundRect(CAM_SW / 2 - 150, CAM_SH / 2 - 58, 300, 116, 16, rgb565(14,14,20));
  drawTextC(CAM_SW / 2, CAM_SH / 2 - 40, "SIN SE\xC3\x91" "AL", 4, rgb565(255,255,255));
  drawTextC(CAM_SW / 2, CAM_SH / 2 + 8, "esperando sensor de camara", 2, rgb565(200,205,215));
  setBuf(fb);
}
// Bucle de procesamiento de pixeles: recorta la region central segun el
// zoom y la reescala a pantalla completa (zoom digital real).
static void camRenderPreview(){
  int cw = (int)(CAM_SW / camZoom); if(cw < 4) cw = 4;
  int ch = (int)(CAM_SH / camZoom); if(ch < 4) ch = 4;
  int cx0 = (CAM_SW - cw) / 2 + camEisX, cy0 = (CAM_SH - ch) / 2 + camEisY;
  if(cx0 < 0) cx0 = 0; if(cx0 + cw > CAM_SW) cx0 = CAM_SW - cw;
  if(cy0 < 0) cy0 = 0; if(cy0 + ch > CAM_SH) cy0 = CAM_SH - ch;
  for(int py = 0; py < SCR_H; py++){
    int sy = cy0 + py * ch / SCR_H;
    uint16_t* srow = camScene + (size_t)sy * CAM_SW;
    uint16_t* drow = fb + (size_t)py * SCR_W;
    for(int px = 0; px < SCR_W; px++){ int sx = cx0 + px * cw / SCR_W; drow[px] = srow[sx]; }
  }
  setBuf(fb);
  if(camNight) fillRectA(0, 0, SCR_W, SCR_H, rgb565(10,20,45), 120);          // modo noche
  if(camExpo > 55) fillRectA(0, 0, SCR_W, SCR_H, rgb565(255,255,255), (camExpo - 55) * 3);
  else if(camExpo < 45) fillRectA(0, 0, SCR_W, SCR_H, rgb565(0,0,0), (45 - camExpo) * 3);
}
static void camDrawUI(){
  setBuf(fb);
  uint16_t W = rgb565(255,255,255);
  strokeSegAA(30, 26, 18, 18, 2.4f, W); strokeSegAA(18, 18, 30, 10, 2.4f, W);   // back
  fillRoundRectA(60, 8, 40, 36, 10, camNight ? rgb565(60,110,235) : rgb565(0,0,0), camNight ? 255 : 90);  // noche
  fillCircle(80, 26, 9, camNight ? rgb565(255,255,255) : rgb565(205,210,220));
  fillCircle(84, 22, 3, camNight ? rgb565(60,110,235) : rgb565(0,0,0));
  fillRoundRectA(108, 8, 50, 36, 10, camRaw ? rgb565(240,160,40) : rgb565(0,0,0), camRaw ? 255 : 90);     // RAW
  drawTextC(133, 16, "RAW", 2, camRaw ? rgb565(30,30,30) : rgb565(220,220,225));
  { char z[12]; snprintf(z, sizeof(z), "%.1fx", (double)camZoom);
    fillRoundRectA(SCR_W / 2 - 34, 8, 68, 32, 16, rgb565(0,0,0), 110); drawTextC(SCR_W / 2, 14, z, 2, W); }
  { int ex = SCR_W - 26, ey = 130, eh = 280;                                    // exposicion (vertical)
    fillRoundRectA(ex - 8, ey - 22, 32, eh + 44, 14, rgb565(0,0,0), 80);
    fillCircle(ex + 8, ey - 10, 4, rgb565(255,230,120));
    vLine(ex + 8, ey, eh, rgb565(120,124,140));
    int ky = ey + eh - (camExpo * eh / 100); fillCircle(ex + 8, ky, 9, W); }
  { const char* lb[5] = { "0.5x", "1x", "2x", "5x", "50x" }; float lv[5] = { 0.5f, 1, 2, 5, 50 };
    int bw = 56, g = 8, tot = 5 * bw + 4 * g, sx = (SCR_W - tot) / 2, y = SCR_H - 192;
    for(int i = 0; i < 5; i++){ int x = sx + i * (bw + g); bool on = fabsf(camZoom - lv[i]) < 0.05f;
      fillRoundRectA(x, y, bw, 34, 16, on ? rgb565(240,200,60) : rgb565(0,0,0), on ? 255 : 100);
      drawTextC(x + bw / 2, y + 9, lb[i], 2, on ? rgb565(30,30,30) : W); } }
  { int zx = 40, zy = SCR_H - 150, zw = SCR_W - 80;                             // zoom manual (horizontal)
    fillRoundRect(zx, zy, zw, 6, 3, rgb565(70,74,88));
    fillCircle(zx + (int)((camZoom - 1) / 49.0f * zw), zy + 3, 9, W); }
  { const char* md[5] = { "FOTO", "VIDEO", "CINE", "ACCION", "PRORES" }; int y = SCR_H - 30, cw2 = SCR_W / 5;
    for(int i = 0; i < 5; i++) drawTextC(cw2 * i + cw2 / 2, y, md[i], 2, i == camMode ? rgb565(255,220,60) : rgb565(170,176,190)); }
  { int cbx = SCR_W / 2, cby = SCR_H - 84; bool vidmode = (camMode >= 1);        // boton captura
    drawCircle(cbx, cby, 34, W); drawCircle(cbx, cby, 33, W);
    if(camRec && vidmode) fillRoundRect(cbx - 12, cby - 12, 24, 24, 5, rgb565(230,60,60));
    else fillCircle(cbx, cby, 27, vidmode ? rgb565(230,60,60) : W); }
  if(camRec){ fillCircle(SCR_W / 2 - 42, 60, 6, rgb565(230,60,60)); drawText(SCR_W / 2 - 30, 54, "REC", 2, rgb565(230,60,60)); }
}
static void camRenderAll(){ gClipY0 = 0; gClipY1 = SCR_H - 1; camRenderPreview(); camDrawUI(); flxFlushAll(); }
static void camEnter(){
  if(!camScene){
    camScene = (uint16_t*)heap_caps_malloc((size_t)CAM_SW * CAM_SH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(camScene && !camCapture(camScene)) camGenScene();   // camara real si hay sensor; si no, patron
  }
  camZoom = 1.0f; camRec = false; camMode = 0; camNight = false; camRaw = false; camExpo = 50; camEisX = 0; camEisY = 0;
  if(camScene) camRenderAll();
}
static void camTick(){
  if(!camScene) return;
#if CAM_HAS_SENSOR
  static unsigned long camMs = 0;                       // streaming en vivo (cuando haya sensor)
  if(millis() - camMs > 33){ camMs = millis(); if(camCapture(camScene)) camRenderAll(); }
#endif
  int ex = SCR_W - 26, ey = 130, eh = 280;
  if(T.down && T.x >= ex - 18 && T.x <= ex + 26 && T.y >= ey - 12 && T.y <= ey + eh + 12){
    int v = (ey + eh - T.y) * 100 / eh; if(v < 0) v = 0; if(v > 100) v = 100; camExpo = v; camRenderAll(); return;
  }
  int zx = 40, zy = SCR_H - 150, zw = SCR_W - 80;
  if(T.down && T.y >= zy - 14 && T.y <= zy + 18 && T.x >= zx - 14 && T.x <= zx + zw + 14){
    float z = 1 + (float)(T.x - zx) / zw * 49.0f; if(z < 1) z = 1; if(z > 50) z = 50; camZoom = z; camRenderAll(); return;
  }
  if(!T.tap) return;
  if(T.x < 48 && T.y < 48){ appClose(); return; }
  if(T.x >= 60 && T.x <= 100 && T.y >= 8 && T.y <= 44){ camNight = !camNight; camRenderAll(); return; }
  if(T.x >= 108 && T.x <= 158 && T.y >= 8 && T.y <= 44){ camRaw = !camRaw; camRenderAll(); return; }
  { float lv[5] = { 0.5f, 1, 2, 5, 50 }; int bw = 56, g = 8, tot = 5 * bw + 4 * g, sx = (SCR_W - tot) / 2, y = SCR_H - 192;
    for(int i = 0; i < 5; i++){ int x = sx + i * (bw + g); if(T.x >= x && T.x <= x + bw && T.y >= y && T.y <= y + 34){ camZoom = lv[i]; camRenderAll(); return; } } }
  { int y = SCR_H - 30, cw2 = SCR_W / 5; if(T.y >= y - 8 && T.y <= y + 22){ int m = T.x / cw2; if(m >= 0 && m < 5){ camMode = m; if(camMode == 0) camRec = false; camRenderAll(); return; } } }
  { int cbx = SCR_W / 2, cby = SCR_H - 84; if(T.x >= cbx - 34 && T.x <= cbx + 34 && T.y >= cby - 34 && T.y <= cby + 34){
      if(camMode >= 1) camRec = !camRec; camRenderAll(); return; } }
}

// #############################################################
// ##  APP NOTAS + TECLADO 4 CAPAS (ES/EN/NUM/EMOJI)
// ##  Punteros dinamicos (mapaActivo), buffer UTF-8 seguro,
// ##  long-press para acentos y mapeo por cuadricula tactil.
// #############################################################
#define KB_COLS 10
#define KB_ROWS 3

// 4 matrices independientes de cadenas (const char*). La N con "\xC3\xB1".
// Suben aqui arriba (antes estaban debajo de la geometria) porque la altura de
// la franja de chips depende de QUE capa esta activa: en la capa numerica esa
// franja muestra los simbolos personalizados de la Fase E.
static const char* LAYOUT_ES[KB_ROWS][KB_COLS] = {
  {"q","w","e","r","t","y","u","i","o","p"},
  {"a","s","d","f","g","h","j","k","l","\xC3\xB1"},
  {"z","x","c","v","b","n","m",",",".","?"} };
static const char* LAYOUT_EN[KB_ROWS][KB_COLS] = {
  {"q","w","e","r","t","y","u","i","o","p"},
  {"a","s","d","f","g","h","j","k","l",";"},
  {"z","x","c","v","b","n","m",",",".","?"} };
static const char* LAYOUT_NUM[KB_ROWS][KB_COLS] = {
  {"1","2","3","4","5","6","7","8","9","0"},
  {"@","#","$","%","&","-","_","(",")","/"},
  {"*","\"","'",":",";","!","?","+","=","."} };
static const char* LAYOUT_EMOJI[KB_ROWS][KB_COLS] = {   // emoticones de texto (la fuente los dibuja)
  {":)",":D",":(",";)",":P","xD",":o",":|","<3",":3"},
  {"^^","o_o",">:(",":'(","B)","-_-","=)","D:",":v",":c"},
  {"uwu",":*","<_<",">_>","(y)","!!",":]","[:","T_T","o/"} };

static const char* (*mapaActivo)[KB_COLS] = LAYOUT_ES;   // <<< puntero maestro
static bool kbShift = false, kbLangEs = true;

// #############################################################
// ##  FASE A · GEOMETRIA DEL TECLADO EN VARIABLES
// ##  ------------------------------------------------------
// ##  Antes KB_X/KB_KW/KB_KH/KB_GAP eran #define fijos: no habia
// ##  forma de cambiar el tamano sin recompilar. Ahora son
// ##  VARIABLES que salen de gKbSize, y los nombres KB_* se
// ##  conservan como macros para que las ~80 referencias que ya
// ##  existian en Notas, Bloqueo y Wi-Fi sigan leyendose igual.
// ##
// ##  Los tres tamanos (ancho de tecla / alto / separacion / margen):
// ##    Compacto  39 / 42 / 4 / 27   -> 10*39 + 9*4 = 426 px de rejilla
// ##    Normal    43 / 48 / 4 / 6    -> 466 px  (EXACTAMENTE lo de siempre)
// ##    Grande    45 / 56 / 2 / 6    -> 468 px
// ##  Los tres caben en SCR_W=480 con margen a los dos lados, y la
// ##  cuarta fila (funciones) termina siempre por encima del borde
// ##  inferior. Ver kbSizeCheck().
// #############################################################
#if KB_SIZE_CONFIG_ON
static int kbKW = 45, kbKH = 60, kbGap = 2, kbX = 6;
// Recalcula la geometria a partir de gKbSize. Se llama al cargar las
// preferencias y cada vez que el usuario cambia el tamano en Ajustes: aplica
// YA, sin reiniciar, porque toda la geometria se consulta en cada repintado.
//
// POR QUE ESTOS NUMEROS (revisados tras probar en la placa: el teclado
// anterior salia pequeno e incomodo):
//  · El ANCHO esta topado por las 10 columnas. Con 480 px de pantalla, el
//    maximo razonable es 45 px de tecla y 2 px de separacion (450+18=468, o
//    sea 6 px de margen a cada lado). Por ahi ya no se puede crecer mas.
//  · El ALTO es donde SI hay sitio, y es lo que de verdad se nota con el dedo:
//    la pantalla tiene 800 px de alto y las 4 filas ocupan como mucho ~300.
//    Por eso el tamano crece sobre todo hacia abajo.
//      Compacto  43 x 50   (lo que antes era "Normal")
//      Normal    45 x 60   (por defecto: 25% mas alto que antes)
//      Grande    45 x 72   (50% mas alto que antes)
static void kbApplySize(){
  if(gKbSize == KB_SIZE_COMPACT){ kbKW = 43; kbKH = 50; kbGap = 4; }
  else if(gKbSize == KB_SIZE_BIG){ kbKW = 45; kbKH = 72; kbGap = 2; }
  else { kbKW = 45; kbKH = 60; kbGap = 2; }
  int gw = KB_COLS * kbKW + (KB_COLS - 1) * kbGap;    // ancho real de la rejilla
  kbX = (SCR_W - gw) / 2; if(kbX < 2) kbX = 2;        // centrada, nunca pegada al borde
}
#else
// Interruptor a 0: exactamente los numeros de siempre, y kbApplySize no hace nada.
static const int kbKW = 43, kbKH = 48, kbGap = 4, kbX = 6;
static void kbApplySize(){}
#endif
#define KB_KW   kbKW
#define KB_KH   kbKH
#define KB_GAP  kbGap
#define KB_X    kbX

// Extras que se dibujan ENCIMA de las teclas (barra superior de la Fase C y
// franja de chips de la Fase F). Solo los muestra la superficie que los pide
// (hoy: Notas). El Bloqueo y el Wi-Fi ponen kbExtrasOn=false a proposito --
// un boton de "portapapeles" o de "ajustes" accesible desde la pantalla de
// contrasena seria un agujero de seguridad, no una comodidad.
static bool kbExtrasOn = false;
static int  kbToolbarH(){ return (KB_TOOLBAR_ON && kbExtrasOn && gKbToolbar) ? 56 : 0; }
// La franja fina de arriba tiene DOS inquilinos que nunca coinciden: los chips
// de autocompletado (capas de letras) y los simbolos personalizados de la Fase E
// (capa numerica). Sugerir palabras mientras se teclean numeros no tendria
// sentido, asi que se reparten la misma franja en vez de sumar dos.
static bool kbChipsWant(){
  if(!kbExtrasOn) return false;
  if(mapaActivo == LAYOUT_NUM) return KB_SETTINGS_ON ? true : false;
  return KB_AUTOCOMPLETE_ON && gKbPredict;
}
static int  kbChipsH(){   return kbChipsWant() ? 32 : 0; }
static int  kbTopH(){     return kbToolbarH() + kbChipsH(); }
// Y de la PRIMERA FILA DE TECLAS. Es lo que siempre significo KB_Y, y sigue
// significando lo mismo: los extras crecen hacia ARRIBA, no empujan las teclas.
static int  kbRowsTop(){  return SCR_H - 4 * (KB_KH + KB_GAP) - 6; }
#define KB_Y kbRowsTop()
// Y donde empieza el PANEL entero (con barra y chips incluidos). Es el limite
// de abajo del area de texto y el borde superior de la banda a volcar.
static int  kbPanelTop(){ return kbRowsTop() - 4 - kbTopH(); }
static int  kbToolbarY(){ return kbPanelTop() + 4; }
static int  kbChipsY(){   return kbToolbarY() + kbToolbarH(); }
// Y de la fila de funciones (shift, capa, idioma, espacio, borrar, enter).
static int  kbFuncY(){    return kbRowsTop() + 3 * (KB_KH + KB_GAP); }

// ---- Fila de funciones: geometria proporcional a la rejilla ----
// Antes las 6 teclas tenian x y ancho HARDCODEADOS (6, 68, 126, 178, 372, 420)
// en cuatro sitios distintos, y el hit-test los repetia como numeros sueltos
// ("if(px < 64) ... else if(px < 122)"). Con la rejilla ya no fija habria que
// tocarlos en todos lados; peor: cambiar uno y olvidar otro deja teclas que se
// ven en un sitio y responden en otro. Ahora salen de una sola tabla de pesos.
#define KB_FKEYS 6
static const float KB_FW[KB_FKEYS] = { 0.135f, 0.125f, 0.110f, 0.420f, 0.100f, 0.110f };
static int kbFKeyW(int i){
  if(i < 0 || i >= KB_FKEYS) return 0;
  int gw = KB_COLS * KB_KW + (KB_COLS - 1) * KB_GAP;
  int usable = gw - (KB_FKEYS - 1) * KB_GAP;
  int w = (int)(usable * KB_FW[i] + 0.5f);
  return w < 20 ? 20 : w;
}
static int kbFKeyX(int i){
  int x = KB_X;
  for(int k = 0; k < i; k++) x += kbFKeyW(k) + KB_GAP;
  return x;
}
// Devuelve 0..5 (shift, capa, idioma, espacio, borrar, enter) o -1.
// Mismo criterio que kbCellAt: cada tecla se queda con la separacion que tiene
// a su derecha, asi que no hay franjas muertas entre botones.
static int kbFRowHit(int px, int py){
  int fy = kbFuncY();
  if(py < fy - KB_GAP || py > fy + KB_KH + KB_GAP) return -1;
  if(px < KB_X - KB_GAP) return -1;
  for(int i = 0; i < KB_FKEYS; i++){
    int x = kbFKeyX(i);
    if(px <= x + kbFKeyW(i) + KB_GAP) return i;
  }
  return -1;
}
// Comprobacion de que NINGUN tamano se sale de la pantalla. No dibuja nada: es
// una asercion barata que corre una vez al arrancar y deja rastro en el log.
static bool kbSizeCheck(){
  int gw = KB_COLS * KB_KW + (KB_COLS - 1) * KB_GAP;
  int fw = kbFKeyX(KB_FKEYS - 1) + kbFKeyW(KB_FKEYS - 1);
  int bot = kbFuncY() + KB_KH;
  return (KB_X >= 0) && (KB_X + gw <= SCR_W) && (fw <= SCR_W) && (bot <= SCR_H - 4) && (kbPanelTop() > 120);
}

// ---- Colores del teclado (Fase E: contraste alto + opacidad + estilo) ----
// Se resuelven en funcion de gKbHiCon en vez de estar escritos a mano en cada
// funcion de dibujo, que era lo que hacia imposible anadir un tema.
static uint16_t kbColKey(){     return gKbHiCon ? rgb565(0,0,0)       : rgb565(52,56,70); }
static uint16_t kbColKeyTxt(){  return gKbHiCon ? rgb565(255,255,255) : rgb565(240,242,248); }
static uint16_t kbColFn(){      return gKbHiCon ? rgb565(24,24,24)    : rgb565(66,70,86); }
static uint16_t kbColFnOn(){    return gKbHiCon ? rgb565(255,210,0)   : rgb565(60,110,235); }
static uint16_t kbColFnOnTxt(){ return gKbHiCon ? rgb565(0,0,0)       : rgb565(240,242,248); }
static uint16_t kbColPanel(){   return gKbHiCon ? rgb565(0,0,0)       : rgb565(36,40,58); }
static uint16_t kbColEdge(){    return gKbHiCon ? rgb565(255,255,255) : rgb565(96,102,124); }
static uint16_t kbColPress(){   return gKbHiCon ? rgb565(255,210,0)   : rgb565(96,132,235); }
// Tamano de letra de las teclas (Fase E). El sistema de tallas de drawText es
// entero (1,2,3...), asi que "Tamano de fuente" mueve esa talla, no un factor.
static int kbFontSize(){ return gKbFontSc == 0 ? 1 : gKbFontSc == 2 ? 3 : 2; }
// Alto de linea aproximado de esa talla, para centrar el glifo en la tecla.
static int kbFontDy(){   return gKbFontSc == 0 ? 4 : gKbFontSc == 2 ? 12 : 8; }
static int kbRadius(){   return gKbStyle == 1 ? 0 : 6; }   // 1 = "Cuadrada"

// Pinta UNA tecla con el estilo elegido (Fase E) y el destello de pulsada
// (Fase G). Solo primitivos en la firma, como manda el proyecto.
static void kbPaintKey(int x, int y, int w, int h, const char* label, int fontSize, uint16_t bg, uint16_t txt, bool pressed){
  int r = kbRadius();
  uint16_t fill = pressed ? kbColPress() : bg;
  if(gKbStyle == 2){                       // "Contorno": sin relleno, solo borde
    if(pressed) fillRoundRect(x, y, w, h, r, fill);
    drawRoundRect(x, y, w, h, r, kbColEdge());
  } else {
    fillRoundRect(x, y, w, h, r, fill);
  }
  if(label && label[0]) drawTextC(x + w / 2, y + h / 2 - kbFontDy(), label, fontSize, txt);
}

// Fondo del panel del teclado. Un solo sitio para las tres superficies, con la
// opacidad de la Fase E aplicada: al 100% es el vidrio/relleno de siempre; por
// debajo se usa un relleno con alfa para que se transparente lo que hay detras
// (drawLiquidGlassPanel no admite alfa, asi que a opacidad parcial se cambia a
// la ruta plana con alfa -- documentado, no es un olvido).
static void kbPaintPanel(int y0, uint16_t tint){
  int h = SCR_H - y0;
  if(h <= 0) return;
  uint8_t a = (uint8_t)(gKbOpacity * 255 / 100);
  if(uiGlass && gKbOpacity >= 100){ drawLiquidGlassPanel(0, y0, SCR_W, h, 0, tint); return; }
  if(gKbOpacity >= 100) fillRect(0, y0, SCR_W, h, tint);
  else                  fillRectA(0, y0, SCR_W, h, tint, a);
}

// ---- Mapeo por cuadricula: (x,y) -> celda (fila*COLS+col) o -1 ----
// Sube aqui (antes vivia dentro del bloque de Notas) porque ahora la usan
// tambien la ruta multitoque de la Fase B y el editor de atajos de la Fase E,
// que se definen antes que la app.
// El area sensible de cada tecla es su PASO COMPLETO (tecla + separacion), no
// solo el rectangulo pintado. Antes, caer en los 2-4 px de separacion no era
// ninguna tecla: el toque se perdia en silencio y la sensacion era la de un
// teclado que "no responde bien". Ahora no hay huecos muertos: cada punto de la
// rejilla pertenece a alguna tecla. Lo que se ve sigue siendo igual; lo que
// cambia es lo que se puede tocar.
static int kbCellAt(int px, int py){
  int pitchX = KB_KW + KB_GAP, pitchY = KB_KH + KB_GAP;
  int dx = px - KB_X, dy = py - KB_Y;
  if(dx < 0 || dy < 0) return -1;
  int c = dx / pitchX, r = dy / pitchY;
  if(c < 0 || c >= KB_COLS || r < 0 || r >= KB_ROWS) return -1;
  return r * KB_COLS + c;
}

// ---- FASE E: paleta de simbolos personalizables ----
// El usuario elige 4 de estos 16. Se guardan como indice, no como texto, para
// que sea IMPOSIBLE acabar con un caracter que la fuente 5x7 no dibuje.
#define KB_SYM_POOL_N 16
static const char* KB_SYM_POOL[KB_SYM_POOL_N] = {
  "@","#","$","%","&","*","+","=","/","\\","(",")","[","]","<",">" };
static const char* kbSymAt(int i){
  if(i < 0 || i >= KB_SYMS) return "";
  int k = gKbSym[i]; if(k < 0 || k >= KB_SYM_POOL_N) k = 0;
  return KB_SYM_POOL[k];
}

// #############################################################
// ##  FASE B · SEGUIMIENTO DE CONTACTOS DEL TECLADO
// ##  ------------------------------------------------------
// ##  Esto NO sustituye a struct Touch: es una via rapida que
// ##  corre EN PARALELO y solo dentro de las superficies de
// ##  teclado. Todo lo demas (arrastre de manijas, long-press
// ##  de acentos, menu contextual, gestos) sigue leyendo T.
// ##
// ##  Idea: cada dedo que el GT911 reporta trae su TRACK ID. Un
// ##  id que aparece por primera vez sobre una tecla = "key
// ##  down" -> la tecla se escribe YA, sin esperar a que ese
// ##  dedo se levante. Cuando el id desaparece se libera su
// ##  hueco y no se escribe nada otra vez. Asi, apoyar la
// ##  siguiente tecla mientras la anterior aun se esta soltando
// ##  no se traba ni pierde pulsaciones (sensacion de rollover).
// #############################################################
#define KB_TRACK_MAX KB_MAXPOINTS
#define KB_EV_MAX    8
static int  kbTrkId[KB_TRACK_MAX]   = { -1, -1, -1, -1, -1 };
static int  kbTrkCell[KB_TRACK_MAX] = { -1, -1, -1, -1, -1 };
static int  kbEvCell[KB_EV_MAX];      // celda de la rejilla del evento i (-1 si no la hay)
static int  kbEvFn[KB_EV_MAX];        // tecla de funcion del evento i (-1 si no la hay)
static int  kbEvN = 0;
// kbMtOk: la via rapida ha demostrado que FUNCIONA en esta superficie (ha
// disparado al menos una tecla desde que se entro). Es lo que decide quien
// escribe: mientras sea false manda la ruta clasica de soltar, y en cuanto se
// pone a true la ruta de soltar deja de escribir del todo. Asi es IMPOSIBLE que
// las dos escriban la misma tecla (letras duplicadas en la hoja), y si el panel
// no diera puntos multiples el teclado seguiria funcionando igual que siempre.
static bool kbMtOk     = false;
static int  kbMtMaxPts = 0;           // maximo de dedos vistos a la vez (diagnostico de Ajustes)
static uint32_t gKbTypingMs = 0;      // millis del ultimo frame con dedos sobre el teclado

static void kbMtReset(){
  for(int t = 0; t < KB_TRACK_MAX; t++){ kbTrkId[t] = -1; kbTrkCell[t] = -1; }
  kbEvN = 0;
}
// Reinicio COMPLETO al entrar en una superficie de teclado: ademas de olvidar
// los contactos, se vuelve a poner en duda si la via rapida funciona. Asi cada
// pantalla decide por si misma quien escribe, y una placa cuyo panel no diera
// puntos multiples cae sola en el comportamiento de siempre.
static void kbMtSurfaceReset(){ kbMtReset(); kbMtOk = false; }
// Devuelve cuantas teclas NUEVAS se han tocado en este frame (0 si ninguna).
//
// ORDEN DE ESCRITURA (esto es lo que se pidio: quien toca primero, escribe
// primero). Se resuelve en dos niveles:
//  1) Entre FRAMES: la tecla se dispara en el mismo frame en que aparece su
//     dedo, y los frames se procesan en orden. El GT911 publica cada ~10 ms, o
//     sea que dos toques separados por 20 ms caen en frames distintos y salen
//     SIEMPRE en el orden correcto.
//  2) Dentro de un MISMO frame (dos dedos a menos de ~10 ms): el instante real
//     ya no se puede saber -- el chip los reporta juntos, sin marca de tiempo.
//     Se respeta el orden en que los publica el propio GT911, que es el orden
//     en que su firmware los detecto. No se inventa nada mejor que eso.
static int kbMtPoll(){
  kbEvN = 0;
  if(!KB_MULTITOUCH_ON || !gKbFastType) return 0;
  int n = gtPollMulti();
  if(n < 0) return 0;                       // sin dato utilizable: no se toca el seguimiento
  int live = 0;
  for(int p = 0; p < KB_MAXPOINTS; p++) if(gKbPoints[p].active) live++;
  if(live > kbMtMaxPts) kbMtMaxPts = live;  // diagnostico: cuantos puntos da de verdad este panel
  bool seen[KB_TRACK_MAX];
  for(int t = 0; t < KB_TRACK_MAX; t++) seen[t] = false;
  for(int p = 0; p < KB_MAXPOINTS; p++){
    if(!gKbPoints[p].active) continue;
    int id = gKbPoints[p].id, slot = -1;
    for(int t = 0; t < KB_TRACK_MAX; t++) if(kbTrkId[t] == id){ slot = t; break; }
    if(slot >= 0){ seen[slot] = true; gKbTypingMs = millis(); continue; }   // dedo ya conocido
    int cell = kbCellAt(gKbPoints[p].x, gKbPoints[p].y);
    int fn   = kbFRowHit(gKbPoints[p].x, gKbPoints[p].y);
    if(cell < 0 && fn < 0) continue;                       // fuera del teclado: eso es cosa de T
    for(int t = 0; t < KB_TRACK_MAX; t++) if(kbTrkId[t] < 0){ slot = t; break; }
    if(slot < 0) continue;                                 // los 5 huecos ocupados
    kbTrkId[slot] = id; kbTrkCell[slot] = cell; seen[slot] = true;
    gKbTypingMs = millis();
    if(kbEvN < KB_EV_MAX){ kbEvCell[kbEvN] = cell; kbEvFn[kbEvN] = fn; kbEvN++; }
  }
  for(int t = 0; t < KB_TRACK_MAX; t++) if(kbTrkId[t] >= 0 && !seen[t]){ kbTrkId[t] = -1; kbTrkCell[t] = -1; }
  if(kbEvN > 0) kbMtOk = true;
  return kbEvN;
}
// true mientras se este tecleando de verdad (hay dedos sobre la rejilla). Lo
// consulta el gesto de suspension: ver suspGestureUpdate. La marca la ponen
// tanto la via rapida como los ticks de Notas y Bloqueo, asi que vale tambien
// con la escritura rapida desactivada.
static bool kbTypingNow(){ return (millis() - gKbTypingMs) < 500; }
static void kbTypingMark(){ gKbTypingMs = millis(); }
// QUIEN ESCRIBE. true = manda la via rapida (al tocar) y la ruta de soltar NO
// escribe ni una letra. Esta es la regla que hace imposible que salgan letras
// duplicadas en la hoja: nunca hay dos caminos activos a la vez.
static bool kbFastActive(){ return KB_MULTITOUCH_ON && gKbFastType && kbMtOk; }
// true si esa celda tiene ahora mismo un dedo encima (para pintarla hundida).
static bool kbCellHeld(int cell){
  if(cell < 0) return false;
  for(int t = 0; t < KB_TRACK_MAX; t++) if(kbTrkId[t] >= 0 && kbTrkCell[t] == cell) return true;
  return false;
}

// #############################################################
// ##  FASE G · DESTELLO DE TECLA PRESIONADA
// ##  Se repinta SOLO el rectangulo de esa tecla y se vuelca su
// ##  banda: ni pantalla completa, ni fillScreen, ni parpadeo.
// #############################################################
static int      kbFxCell = -1;      // celda con destello vivo
static uint32_t kbFxT0   = 0;
static void kbFxStart(int cell){
  if(!KB_ANIM_POLISH_ON || cell < 0) return;
  kbFxCell = cell; kbFxT0 = millis();
}
static bool kbFxActive(){ return KB_ANIM_POLISH_ON && kbFxCell >= 0; }
// Progreso 0..255 del destello (255 = recien tocada, 0 = apagado).
static int kbFxLevel(int cell){
  if(!kbFxActive() || cell != kbFxCell) return 0;
  uint32_t dt = millis() - kbFxT0;
  if((int)dt >= gKbFxMs) return 0;
  return 255 - (int)(dt * 255 / (uint32_t)gKbFxMs);
}
// Repinta UNA celda ya, en su sitio, sin tocar el resto del teclado. Es la
// pieza que hace que el destello no cueste un repintado entero.
static void kbPaintCellNow(int cell, bool pressed, uint16_t bg, uint16_t txt){
  if(cell < 0) return;
  int r = cell / KB_COLS, c = cell % KB_COLS;
  int x = KB_X + c * (KB_KW + KB_GAP), y = KB_Y + r * (KB_KH + KB_GAP);
  const char* k = mapaActivo[r][c];
  char up[6];
  if(kbShift && k[1] == 0 && k[0] >= 'a' && k[0] <= 'z'){ up[0] = (char)(k[0] - 32); up[1] = 0; k = up; }
  setBuf(fb);
  kbPaintKey(x, y, KB_KW, KB_KH, k, kbFontSize(), bg, txt, pressed);
  flxFlush(y - 1, y + KB_KH + 1);
}
// Enciende el destello Y lo pinta en el acto. Es la ruta para cuando la
// escritura rapida esta apagada: ahi el feedback tiene que salir en T.pressed,
// porque la tecla no se escribe hasta soltar y sin esto no habria ninguna
// senal de que el toque llego.
static void kbFxPress(int cell, uint16_t bg, uint16_t txt){
  if(!KB_ANIM_POLISH_ON || cell < 0) return;
  kbFxStart(cell);
  kbPaintCellNow(cell, true, bg, txt);
}
// Repinta la tecla del destello cuando se apaga. La llaman los ticks de las
// superficies con SUS colores (primitivos en la firma, como pide el proyecto).
static bool kbFxTick(uint16_t bg, uint16_t txt){
  if(!kbFxActive()) return false;
  if((int)(millis() - kbFxT0) < gKbFxMs) return false;
  int cell = kbFxCell; kbFxCell = -1;
  kbPaintCellNow(cell, false, bg, txt);
  return true;
}

// #############################################################
// ##  FASE D · PORTAPAPELES DE VARIAS RANURAS
// ##  ------------------------------------------------------
// ##  PERSISTENCIA (decision explicita): solo sobreviven al
// ##  reinicio las ranuras FIJADAS. El resto son de sesion. Se
// ##  guardan en NVS con una clave por ranura ("clip0".."clip11")
// ##  dentro de la misma namespace "flexos" del resto del
// ##  sistema. Motivo: el portapapeles de trabajo cambia
// ##  constantemente y escribir flash en cada copia gastaria
// ##  ciclos de NVS para nada; lo que el usuario marca con el
// ##  pin es justo lo que dice "esto quiero conservarlo".
// #############################################################
static char clipboard[512] = "";      // <<< buffer clasico: sigue existiendo (KB_CLIPBOARD_MULTI_ON 0) >>>

static void clipSavePinned(){
  if(!KB_CLIPBOARD_MULTI_ON) return;
  prefs.begin("flexos", false);
  char key[10];
  for(int i = 0; i < CLIP_SLOTS; i++){
    snprintf(key, sizeof(key), "clip%d", i);
    if(gClip[i].used && gClip[i].pinned) prefs.putString(key, gClip[i].text);
    else                                 prefs.remove(key);
  }
  prefs.end();
}
static void clipLoadPinned(){
  if(!KB_CLIPBOARD_MULTI_ON) return;
  prefs.begin("flexos", true);
  char key[10];
  for(int i = 0; i < CLIP_SLOTS; i++){
    snprintf(key, sizeof(key), "clip%d", i);
    String s = prefs.getString(key, "");
    if(s.length() > 0){
      s.toCharArray(gClip[i].text, CLIP_TXT_MAX);
      gClip[i].used = true; gClip[i].pinned = true; gClip[i].ts = 0;
    }
  }
  prefs.end();
}
static int clipCount(){
  int n = 0;
  for(int i = 0; i < CLIP_SLOTS; i++) if(gClip[i].used) n++;
  return n;
}
// Inserta un texto nuevo. Si no hay hueco libre, sobrescribe la ranura NO
// FIJADA mas antigua -- una fijada no se descarta jamas.
static void clipPush(const char* s){
  if(!s || !s[0]) return;
  if(s != clipboard){ strncpy(clipboard, s, sizeof(clipboard) - 1); clipboard[sizeof(clipboard) - 1] = 0; }
  if(!KB_CLIPBOARD_MULTI_ON) return;
  for(int i = 0; i < CLIP_SLOTS; i++)                     // repetido: solo se refresca la fecha
    if(gClip[i].used && !strncmp(gClip[i].text, s, CLIP_TXT_MAX - 1)){ gClip[i].ts = millis(); return; }
  int slot = -1;
  for(int i = 0; i < CLIP_SLOTS; i++) if(!gClip[i].used){ slot = i; break; }
  if(slot < 0){
    uint32_t oldest = 0xFFFFFFFFu;
    for(int i = 0; i < CLIP_SLOTS; i++) if(!gClip[i].pinned && gClip[i].ts <= oldest){ oldest = gClip[i].ts; slot = i; }
    if(slot < 0) return;                                  // las 12 estan fijadas: no se toca ninguna
  }
  strncpy(gClip[slot].text, s, CLIP_TXT_MAX - 1);
  gClip[slot].text[CLIP_TXT_MAX - 1] = 0;
  gClip[slot].used = true; gClip[slot].pinned = false; gClip[slot].ts = millis();
}
static void clipDel(int i){
  if(i < 0 || i >= CLIP_SLOTS) return;
  bool wasPinned = gClip[i].pinned;
  gClip[i].used = false; gClip[i].pinned = false; gClip[i].text[0] = 0; gClip[i].ts = 0;
  if(wasPinned) clipSavePinned();
}
static void clipTogglePin(int i){
  if(i < 0 || i >= CLIP_SLOTS || !gClip[i].used) return;
  gClip[i].pinned = !gClip[i].pinned;
  clipSavePinned();
}
static void clipClearUnpinned(){
  for(int i = 0; i < CLIP_SLOTS; i++) if(gClip[i].used && !gClip[i].pinned){ gClip[i].used = false; gClip[i].text[0] = 0; gClip[i].ts = 0; }
}

// #############################################################
// ##  FASE F · AUTOCOMPLETADO SIMULADO (LISTA LOCAL FIJA)
// ##  ------------------------------------------------------
// ##  QUE ES: dos listas de palabras frecuentes, una por idioma,
// ##  escritas a mano en el propio .ino. Se busca por PREFIJO y
// ##  se ofrecen hasta 3 coincidencias.
// ##  QUE NO ES: un modelo de lenguaje. No aprende, no entiende
// ##  el contexto y no predice la palabra siguiente. La pantalla
// ##  "Sobre teclado" lo dice con esas mismas palabras -- vender
// ##  esto como "IA" seria mentir.
// ##  COSTE: ~250 entradas por idioma, unos 3 KB de flash cada
// ##  lista. La busqueda es un recorrido lineal que corta en
// ##  cuanto junta 3 resultados: microsegundos por tecla.
// #############################################################
static const char* KB_DICT_ES[] = {
  "a","abajo","abrir","acaso","aceptar","acuerdo","adelante","adem\xC3\xA1s","agua","ahora",
  "algo","alguien","alguno","alli","alto","amigo","amor","antes","a\xC3\xB1o","apagar",
  "aplicacion","aprender","aqui","archivo","arriba","asi","ayer","ayuda","bajar","bastante",
  "bien","borrar","brillo","buenas","bueno","buscar","caja","calle","cambiar","camino",
  "cargar","casa","caso","celular","cerca","cerrar","cielo","ciudad","claro","codigo",
  "color","comenzar","comida","como","completo","compartir","comprar","con","conectar","conocer",
  "contacto","contra","copiar","correo","cosa","crear","cuando","cuenta","dar","datos",
  "deber","decir","dejar","delante","dentro","desde","despu\xC3\xA9s","dia","dinero","dispositivo",
  "donde","dormir","durante","el","ella","empezar","encender","encontrar","entender","entonces",
  "entrar","enviar","error","escribir","escuchar","espacio","esperar","est\xC3\xA1","este","estar",
  "falta","familia","favor","fecha","final","forma","foto","fuera","fuerte","funcionar",
  "gente","grande","gracias","guardar","gustar","haber","hablar","hacer","hasta","hecho",
  "hola","hombre","hora","hoy","idea","idioma","imagen","importante","informacion","instalar",
  "internet","ir","juego","jugar","junto","lado","largo","leer","lento","letra",
  "libro","limpiar","llamar","llegar","llevar","luego","lugar","luz","madre","mal",
  "mandar","manera","ma\xC3\xB1" "ana","mano","mas","mayor","mejor","memoria","menos","mensaje",
  "mes","mientras","minuto","mirar","mismo","modo","momento","mostrar","mover","mucho",
  "mujer","mundo","musica","muy","nada","necesitar","ni\xC3\xB1" "o","noche","nombre","normal",
  "nosotros","noticia","nuevo","numero","nunca","ocurrir","oir","opcion","orden","otro",
  "padre","pagina","palabra","pantalla","papel","para","parecer","parte","pasar","pedir",
  "pelicula","pensar","peque\xC3\xB1o","perder","pero","persona","poco","poder","poner","porque",
  "posible","primero","probar","problema","pronto","propio","punto","quedar","querer","qui\xC3\xA9n",
  "quitar","rapido","razon","recibir","reiniciar","respuesta","resultado","saber","salir","seguir",
  "segundo","seguro","seleccionar","semana","sentir","se\xC3\xB1" "al","ser","servicio","siempre","siguiente",
  "silencio","sistema","sitio","sobre","solo","sonido","tama\xC3\xB1o","tambi\xC3\xA9n","tarde","teclado",
  "telefono","tema","tener","texto","tiempo","tipo","tocar","todo","tomar","trabajo",
  "traer","tratar","ultimo","usar","usuario","valor","venir","ventana","ver","verdad",
  "vez","viaje","vida","volver","voz","ya","zona" };
static const char* KB_DICT_EN[] = {
  "about","above","accept","account","add","after","again","against","all","allow",
  "almost","also","always","and","another","answer","any","app","apply","are",
  "around","ask","away","back","battery","because","become","been","before","begin",
  "behind","being","believe","below","best","better","between","big","bit","book",
  "both","bring","build","button","buy","call","camera","can","cancel","car",
  "care","carry","case","change","check","child","choose","city","clean","clear",
  "click","close","code","cold","color","come","company","computer","connect","contact",
  "continue","copy","could","country","create","cut","dark","data","day","delete",
  "device","different","display","does","done","door","down","download","draw","drive",
  "during","each","early","easy","edit","email","end","enough","enter","error",
  "even","ever","every","example","exit","face","fact","fail","family","far",
  "fast","feel","field","file","fill","find","fine","first","folder","follow",
  "font","food","for","force","form","free","friend","from","full","game",
  "get","give","good","great","group","hand","happen","happy","hard","have",
  "head","hear","help","here","high","hold","home","hope","hour","house",
  "how","idea","image","import","inside","install","just","keep","key","keyboard",
  "kind","know","language","large","last","late","learn","leave","left","less",
  "let","letter","level","life","light","like","line","list","little","live",
  "load","local","lock","long","look","love","made","make","many","mark",
  "may","mean","memory","menu","message","might","mind","minute","miss","mode",
  "money","month","more","morning","most","move","much","music","must","name",
  "near","need","network","never","new","news","next","nice","night","none",
  "note","nothing","now","number","off","offer","often","once","only","open",
  "option","order","other","over","page","paper","part","password","people","phone",
  "photo","pick","place","play","please","point","power","press","print","question",
  "quick","quit","read","ready","real","reason","record","remove","repeat","reply",
  "report","reset","rest","result","return","right","room","run","same","save",
  "say","screen","search","second","see","select","send","server","service","set",
  "settings","share","short","should","show","side","sign","since","size","small",
  "some","soon","sound","space","speak","start","state","stay","step","still",
  "stop","store","story","study","such","support","sure","system","table","take",
  "talk","tell","test","text","than","thank","that","their","them","then",
  "there","these","they","thing","think","this","time","today","together","too",
  "tool","touch","try","turn","type","under","until","update","upload","use",
  "user","very","view","wait","walk","want","watch","water","way","week",
  "well","what","when","where","which","while","white","who","why","will",
  "window","with","word","work","world","would","write","year","yes","your" };
#define KB_DICT_ES_N ((int)(sizeof(KB_DICT_ES) / sizeof(KB_DICT_ES[0])))
#define KB_DICT_EN_N ((int)(sizeof(KB_DICT_EN) / sizeof(KB_DICT_EN[0])))

// Devuelve el siguiente caracter "plegado": minuscula y sin acento. Asi
// escribir "mas" encuentra "m\xC3\xA1s" y "man" encuentra "ma\xC3\xB1" "ana", que es lo que
// espera cualquiera que escriba rapido sin pararse a poner tildes.
static char kbFoldCh(const char** ps){
  const char* s = *ps;
  unsigned char c = (unsigned char)s[0];
  if(c == 0){ return 0; }
  if(c < 0x80){ *ps = s + 1; return (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); }
  if(c == 0xC3 && s[1]){
    unsigned char d = (unsigned char)s[1];
    *ps = s + 2;
    if((d >= 0x80 && d <= 0x85) || (d >= 0xA0 && d <= 0xA5)) return 'a';
    if((d >= 0x88 && d <= 0x8B) || (d >= 0xA8 && d <= 0xAB)) return 'e';
    if((d >= 0x8C && d <= 0x8F) || (d >= 0xAC && d <= 0xAF)) return 'i';
    if((d >= 0x92 && d <= 0x96) || (d >= 0xB2 && d <= 0xB6)) return 'o';
    if((d >= 0x99 && d <= 0x9C) || (d >= 0xB9 && d <= 0xBC)) return 'u';
    if(d == 0x91 || d == 0xB1) return 'n';                 // N/n con virgulilla
    return '?';
  }
  s++; while(((unsigned char)*s & 0xC0) == 0x80) s++;      // otro multibyte: se salta entero
  *ps = s; return '?';
}
static bool kbStartsWith(const char* word, const char* pref){
  const char* w = word; const char* p = pref;
  for(;;){
    const char* pp = p; char pc = kbFoldCh(&pp);
    if(pc == 0) return true;                                // el prefijo se acabo: encaja
    const char* ww = w; char wc = kbFoldCh(&ww);
    if(wc == 0 || wc != pc) return false;
    p = pp; w = ww;
  }
}
static bool kbSameWord(const char* a, const char* b){
  const char* x = a; const char* y = b;
  for(;;){
    const char* xx = x; char ac = kbFoldCh(&xx);
    const char* yy = y; char bc = kbFoldCh(&yy);
    if(ac != bc) return false;
    if(ac == 0) return true;
    x = xx; y = yy;
  }
}
// Revision ortografica basica (Fase E/F): "conocida" = esta en el diccionario
// del idioma activo o es uno de los atajos del usuario. Es EXACTAMENTE eso, no
// un corrector gramatical.
static bool kbDictHas(const char* w){
  if(!w || !w[0]) return true;
  const char* const* d = kbLangEs ? KB_DICT_ES : KB_DICT_EN;
  int n = kbLangEs ? KB_DICT_ES_N : KB_DICT_EN_N;
  for(int i = 0; i < n; i++) if(kbSameWord(d[i], w)) return true;
  for(int i = 0; i < KB_SC_MAX; i++) if(gKbScAbr[i][0] && kbSameWord(gKbScAbr[i], w)) return true;
  return false;
}
// Emojis sugeridos (solo glifos que la fuente ya dibuja, los mismos de
// LAYOUT_EMOJI). Es una tabla disparador->emoticon, no un clasificador.
#define KB_EMOSUG_N 10
static const char* KB_EMOSUG_W[KB_EMOSUG_N] = { "risa","amor","triste","guino","abrazo","laugh","love","sad","wink","hug" };
static const char* KB_EMOSUG_E[KB_EMOSUG_N] = { ":D",  "<3",  ":(",    ";)",    "(y)",   ":D",   "<3",  ":(", ";)",  "(y)" };

// Busca hasta maxn coincidencias por prefijo. Orden: primero los atajos de
// texto del usuario (una abreviacion escrita entera gana a cualquier palabra),
// luego el diccionario, luego el emoji sugerido si toca.
static int kbSuggest(const char* pref, const char** out, int maxn){
  int n = 0;
  if(!KB_AUTOCOMPLETE_ON || !gKbPredict || !pref || !pref[0] || maxn <= 0) return 0;
  for(int i = 0; i < KB_SC_MAX && n < maxn; i++)
    if(gKbScAbr[i][0] && gKbScExp[i][0] && kbSameWord(gKbScAbr[i], pref)) out[n++] = gKbScExp[i];
  const char* const* d = kbLangEs ? KB_DICT_ES : KB_DICT_EN;
  int dn = kbLangEs ? KB_DICT_ES_N : KB_DICT_EN_N;
  for(int i = 0; i < dn && n < maxn; i++){
    if(!kbStartsWith(d[i], pref)) continue;
    bool dup = false;
    for(int k = 0; k < n; k++) if(kbSameWord(out[k], d[i])) dup = true;
    if(!dup) out[n++] = d[i];
  }
  if(gKbEmojiSug && n < maxn)
    for(int i = 0; i < KB_EMOSUG_N && n < maxn; i++)
      if(kbStartsWith(KB_EMOSUG_W[i], pref)){ out[n++] = KB_EMOSUG_E[i]; break; }
  return n;
}
// Palabra en construccion: del ultimo espacio/salto hasta el cursor. Copia a
// out (primitivos en la firma) y devuelve su longitud en bytes.
static int kbCurrentWord(const char* buf, int cur, char* out, int outsz){
  out[0] = 0;
  if(!buf || cur <= 0 || outsz <= 1) return 0;
  int a = cur;
  while(a > 0){
    unsigned char c = (unsigned char)buf[a - 1];
    if(c == ' ' || c == '\n' || c == '\t') break;
    a--;
  }
  int n = cur - a; if(n > outsz - 1) n = outsz - 1;
  memcpy(out, buf + a, n); out[n] = 0;
  return n;
}

// ---- Memoria de trabajo del editor de Notas -------------------------------
// El texto que se esta editando es un buffer de TRABAJO, no el fichero: vive
// en PSRAM cuando la placa la tiene (Ultra / Ultra S3), porque son 4 KB que no
// hacen ninguna falta en la RAM interna, que es el recurso escaso. El fichero
// real en /Notas se escribe al salir del editor y 2 s despues de la ultima
// tecla (ver noteSave). En una placa sin PSRAM (Pro) se usa el arreglo
// estatico de siempre: menos capacidad, pero exactamente el mismo
// comportamiento.
#define NOTE_BUF_PSRAM 4096
static char   noteBufStatic[512] = "";
static char*  noteBuffer = noteBufStatic;
static size_t noteBufMax = sizeof(noteBufStatic);
static char   noteTitleBar[FLEXFS_NAME_MAX] = "Notas";   // titulo del editor = nombre real del fichero
static void noteBufInit(){
  if(noteBuffer != noteBufStatic) return;                // ya se amplio
  if(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) return;
  char* p = (char*)heap_caps_malloc(NOTE_BUF_PSRAM, MALLOC_CAP_SPIRAM);
  if(!p) return;
  memcpy(p, noteBufStatic, sizeof(noteBufStatic));
  noteBuffer = p; noteBufMax = NOTE_BUF_PSRAM;
}
// estado del pop-up de acentos
static int kbLpKey = -1, kbPopX = 0, kbPopY = 0, kbPopN = 0, kbPopW = 40, kbPopG = 4;
static bool kbPopup = false;

// ---- Insercion/borrado UTF-8 seguros ----
// ---- Modelo de texto editable (cursor + seleccion) + PORTAPAPELES GLOBAL ----
static int  noteCur = 0;                          // cursor (indice en bytes)
static int  noteSelA = -1, noteSelB = -1;         // seleccion A..B en bytes (-1 = ninguna)
static bool noteMenu = false;                     // menu contextual visible
static int  noteHandleDrag = 0;                   // 0 no, 1 manija izq, 2 der
// (el portapapeles vive ahora arriba: buffer clasico + las 12 ranuras de la Fase D)

static int  utf8Prev(const char* s, int i){ if(i <= 0) return 0; i--; while(i > 0 && (s[i] & 0xC0) == 0x80) i--; return i; }
static int  utf8Next(const char* s, int i){ int L = strlen(s); if(i >= L) return L; i++; while(i < L && (s[i] & 0xC0) == 0x80) i++; return i; }
static bool isWordByte(unsigned char c){ return c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
static bool noteHasSel(){ return noteSelA >= 0 && noteSelB > noteSelA; }
static void noteClearSel(){ noteSelA = noteSelB = -1; noteMenu = false; }
static void noteDeleteSel(){
  if(!noteHasSel()) return;
  int a = noteSelA, b = noteSelB, L = strlen(noteBuffer);
  memmove(noteBuffer + a, noteBuffer + b, L - b + 1);
  noteCur = a; noteClearSel();
}
static void noteInsert(const char* s){            // inserta en el cursor (reemplaza seleccion si hay)
  if(noteHasSel()) noteDeleteSel();
  int L = strlen(noteBuffer), sl = strlen(s);
  if(L + sl >= (int)noteBufMax - 1) return;
  if(noteCur < 0) noteCur = 0; if(noteCur > L) noteCur = L;
  memmove(noteBuffer + noteCur + sl, noteBuffer + noteCur, L - noteCur + 1);
  memcpy(noteBuffer + noteCur, s, sl);
  noteCur += sl; noteMenu = false;
}
static void noteBackspace(){                       // borra antes del cursor (multibyte) o la seleccion
  if(noteHasSel()){ noteDeleteSel(); return; }
  if(noteCur <= 0) return;
  int p = utf8Prev(noteBuffer, noteCur), L = strlen(noteBuffer);
  memmove(noteBuffer + p, noteBuffer + noteCur, L - noteCur + 1);
  noteCur = p; noteMenu = false;
}
// FASE G: chip flotante "Copiado". No bloquea nada -- es una marca de tiempo
// que el tick de Notas mira y borra sola a los ~1.2 s.
static uint32_t kbToastMs = 0;
static char     kbToastTxt[24] = "";
static void kbToast(const char* t){
  snprintf(kbToastTxt, sizeof(kbToastTxt), "%s", t);
  kbToastMs = millis();
  if(!KB_ANIM_POLISH_ON) kbToastMs = 0;      // sin animaciones: ni toast ni corte, simplemente no sale
}
// FASE D: copiar ahora ALIMENTA las 12 ranuras ademas del buffer clasico, asi
// que clipboard[] sigue siendo valido aunque el interruptor este a 0.
static void clipCopy(){
  if(!noteHasSel()) return;
  int a = noteSelA, b = noteSelB, n = b - a;
  if(n >= (int)sizeof(clipboard)) n = sizeof(clipboard) - 1;
  memcpy(clipboard, noteBuffer + a, n); clipboard[n] = 0;
  clipPush(clipboard);
  kbToast("Copiado");
}
static void clipCut(){ clipCopy(); noteDeleteSel(); }
static void clipPaste(){ if(clipboard[0]) noteInsert(clipboard); }
static void selectAllTxt(){ noteSelA = 0; noteSelB = strlen(noteBuffer); noteCur = noteSelB; noteMenu = noteHasSel(); }
static void selectWordAt(int bi){
  int L = strlen(noteBuffer); if(L == 0) return;
  if(bi >= L) bi = utf8Prev(noteBuffer, L);
  int a = bi; while(a > 0){ int p = utf8Prev(noteBuffer, a); if(!isWordByte((unsigned char)noteBuffer[p])) break; a = p; }
  int b = bi; while(b < L){ if(!isWordByte((unsigned char)noteBuffer[b])) break; b = utf8Next(noteBuffer, b); }
  if(b > a){ noteSelA = a; noteSelB = b; noteCur = b; noteMenu = true; }
}
static void kbPressChar(const char* s){
  if(kbShift && s[1] == 0 && s[0] >= 'a' && s[0] <= 'z'){ char u[2] = { (char)(s[0] - 32), 0 }; noteInsert(u); kbShift = false; }
  else if(kbShift && !strcmp(s, "\xC3\xB1")){ noteInsert("\xC3\x91"); kbShift = false; }
  else noteInsert(s);
}
// variantes acentuadas (solo las que tiene la fuente). Devuelve el numero.
static int kbGetVariants(char b, const char* var[4]){
  switch(b){
    case 'a': var[0]="\xC3\xA1"; var[1]="\xC3\xA0"; var[2]="\xC3\xA2"; var[3]="\xC3\xA3"; return 4;
    case 'e': var[0]="\xC3\xA9"; var[1]="\xC3\xA8"; var[2]="\xC3\xAA"; return 3;
    case 'i': var[0]="\xC3\xAD"; var[1]="\xC3\xAC"; var[2]="\xC3\xAE"; return 3;
    case 'o': var[0]="\xC3\xB3"; var[1]="\xC3\xB2"; var[2]="\xC3\xB4"; var[3]="\xC3\xB5"; return 4;
    case 'u': var[0]="\xC3\xBA"; var[1]="\xC3\xB9"; var[2]="\xC3\xBB"; var[3]="\xC3\xBC"; return 4;
  }
  return 0;
}
static bool kbIsVowelCell(int cell){
  if(cell < 0 || !(mapaActivo == LAYOUT_ES || mapaActivo == LAYOUT_EN)) return false;
  const char* k = mapaActivo[cell / KB_COLS][cell % KB_COLS];
  return k[1] == 0 && (k[0]=='a'||k[0]=='e'||k[0]=='i'||k[0]=='o'||k[0]=='u');
}

// Limite de abajo del AREA DE TEXTO. Antes era KB_Y-8 escrito a mano en cinco
// sitios; ahora sale del panel del teclado, que puede crecer hacia arriba si
// estan la barra de la Fase C o los chips de la Fase F.
static int noteTxtBot(){ return kbPanelTop() - 8; }
static int curSX, curSY, hASX, hASY, hBSX, hBSY, noteMenuX, noteMenuY;
static void noteDrawText(){
  setBuf(fb);
  fillRect(8, 48, SCR_W - 16, noteTxtBot() - 48, rgb565(24,26,34));
  int x = 18, y = 60, maxX = SCR_W - 18, lh = 26, size = 2;
  float sc = fontSc(size);
  const char* s = noteBuffer; int bi = 0;
  bool hasSel = noteHasSel();
  int yBreak = noteTxtBot() - 22;
  curSX = 18; curSY = 60; hASX = hBSX = 18; hASY = hBSY = 60;
  // FASE F - revision ortografica basica: se sigue la palabra en curso (donde
  // empezo y en que linea) y al cerrarla se subraya con puntitos si NO esta en
  // el diccionario del idioma activo. Es eso y nada mas: comparacion contra una
  // lista local, sin gramatica ni sugerencias de correccion.
  bool spellOn = (KB_AUTOCOMPLETE_ON && gKbSpell);
  int wsX = x, wsY = y, wsBi = 0; bool inWord = false;
  while(*s){
    if(*s == '\n'){
      if(spellOn && inWord){
        char wtmp[40]; int wn = bi - wsBi; if(wn > 39) wn = 39;
        memcpy(wtmp, noteBuffer + wsBi, wn); wtmp[wn] = 0;
        if(wsY == y && wn >= 3 && !kbDictHas(wtmp)) for(int px = wsX; px < x; px += 3) fillRect(px, y + lh - 7, 2, 2, rgb565(226,96,96));
        inWord = false;
      }
      if(bi == noteCur){ curSX = x; curSY = y; } if(bi == noteSelA){ hASX = x; hASY = y; } if(bi == noteSelB){ hBSX = x; hBSY = y; }
      s++; bi++; x = 18; y += lh; if(y > yBreak) break; continue;
    }
    const char* save = s; uint32_t cp = nextCP(&s); int nb = s - save;
    int w = (int)(FG[fontIdx(cp)].adv * sc + 0.5f);
    if(x + w > maxX){ x = 18; y += lh; if(y > yBreak) break; }
    if(spellOn){
      bool wb = isWordByte((unsigned char)*save);
      if(wb && !inWord){ inWord = true; wsX = x; wsY = y; wsBi = bi; }
      else if(!wb && inWord){
        char wtmp[40]; int wn = bi - wsBi; if(wn > 39) wn = 39;
        memcpy(wtmp, noteBuffer + wsBi, wn); wtmp[wn] = 0;
        if(wsY == y && wn >= 3 && !kbDictHas(wtmp)) for(int px = wsX; px < x; px += 3) fillRect(px, y + lh - 7, 2, 2, rgb565(226,96,96));
        inWord = false;
      }
    }
    if(bi == noteCur){ curSX = x; curSY = y; } if(bi == noteSelA){ hASX = x; hASY = y; } if(bi == noteSelB){ hBSX = x; hBSY = y; }
    if(hasSel && bi >= noteSelA && bi < noteSelB) fillRect(x - 1, y - 2, w + 1, lh - 4, rgb565(48,92,168));  // resaltado
    char one[6]; int n = nb; if(n > 5) n = 5; for(int i = 0; i < n; i++) one[i] = save[i]; one[n] = 0;
    drawText(x, y, one, size, rgb565(235,238,246));
    x += w; bi += nb;
  }
  // Ultima palabra del texto. Si el cursor esta justo ahi es que se esta
  // escribiendo TODAVIA: marcarla en rojo mientras se teclea seria ruido puro,
  // asi que esa se deja en paz hasta que se cierre con un espacio.
  if(spellOn && inWord && noteCur != bi){
    char wtmp[40]; int wn = bi - wsBi; if(wn > 39) wn = 39;
    memcpy(wtmp, noteBuffer + wsBi, wn); wtmp[wn] = 0;
    if(wsY == y && wn >= 3 && !kbDictHas(wtmp)) for(int px = wsX; px < x; px += 3) fillRect(px, y + lh - 7, 2, 2, rgb565(226,96,96));
  }
  if(bi == noteCur){ curSX = x; curSY = y; } if(bi == noteSelA){ hASX = x; hASY = y; } if(bi == noteSelB){ hBSX = x; hBSY = y; }
  if(hasSel){                                       // manijas (gotas arrastrables)
    vLine(hASX, hASY - 2, 24, rgb565(90,150,240)); fillCircle(hASX, hASY + 24, 7, rgb565(90,150,240));
    vLine(hBSX, hBSY - 2, 24, rgb565(90,150,240)); fillCircle(hBSX, hBSY + 24, 7, rgb565(90,150,240));
  } else {
    fillRect(curSX + 1, curSY - 2, 2, 22, rgb565(90,150,240));   // cursor
  }
  if(noteMenu){                                     // menu contextual flotante
    const char* it[4] = { "Cortar", "Copiar", "Pegar", "Todo" };
    int bw = 92, gap = 4, tot = 4 * bw + 3 * gap, mx = (SCR_W - tot) / 2, my = hASY - 44; if(my < 50) my = 50;
    noteMenuX = mx; noteMenuY = my;
    fillRoundRect(mx - 6, my - 6, tot + 12, 40, 8, rgb565(38,42,56));
    for(int i = 0; i < 4; i++){ int bx = mx + i * (bw + gap); fillRoundRect(bx, my, bw, 28, 6, rgb565(58,64,84)); drawTextC(bx + bw / 2, my + 7, it[i], 2, rgb565(240,244,252)); }
  }
  // FASE G - chip flotante "Copiado": vive DENTRO del area de texto, asi que se
  // borra solo con el siguiente repintado de esta misma banda. Se desvanece en
  // el ultimo tercio en vez de desaparecer de golpe.
  if(KB_ANIM_POLISH_ON && kbToastMs){
    uint32_t dt = millis() - kbToastMs;
    if(dt < 1200){
      uint8_t a = (dt < 800) ? 230 : (uint8_t)(230 - (dt - 800) * 230 / 400);
      int tw = textW(kbToastTxt, 2) + 34, tx = (SCR_W - tw) / 2, ty = noteTxtBot() - 46;
      fillRoundRectA(tx, ty, tw, 32, 16, rgb565(28,32,44), a);
      drawTextCA(SCR_W / 2, ty + 9, kbToastTxt, 2, rgb565(235,240,250), a);
    }
  }
  // Se vuelca hasta el borde mismo del panel del teclado: entre el final del
  // area de texto y kbPanelTop() hay unos pixeles de fondo que si no quedarian
  // en tierra de nadie (ni esta banda ni la del teclado los publicaria).
  flxFlush(44, kbPanelTop() - 1);
}
// mapea un toque (px,py) al indice de byte mas cercano en el texto
static int noteLayoutHit(int px, int py){
  int x = 18, y = 60, maxX = SCR_W - 18, lh = 26, size = 2; float sc = fontSc(size);
  const char* s = noteBuffer; int bi = 0, best = 0; long bestd = 1L << 30;
  while(*s){
    if(*s == '\n'){ long d = (long)abs(px - x) + (long)abs(py - (y + 8)) * 2; if(d < bestd){ bestd = d; best = bi; } s++; bi++; x = 18; y += lh; continue; }
    const char* save = s; uint32_t cp = nextCP(&s); int nb = s - save;
    int w = (int)(FG[fontIdx(cp)].adv * sc + 0.5f);
    if(x + w > maxX){ x = 18; y += lh; }
    long d0 = (long)abs(px - x) + (long)abs(py - (y + 8)) * 2; if(d0 < bestd){ bestd = d0; best = bi; }
    long d1 = (long)abs(px - (x + w)) + (long)abs(py - (y + 8)) * 2; if(d1 < bestd){ bestd = d1; best = bi + nb; }
    x += w; bi += nb;
  }
  long d = (long)abs(px - x) + (long)abs(py - (y + 8)) * 2; if(d < bestd){ bestd = d; best = bi; }
  return best;
}
static int noteMenuHit(int px, int py){
  if(!noteMenu || py < noteMenuY || py > noteMenuY + 28) return -1;
  int bw = 92, gap = 4;
  for(int i = 0; i < 4; i++){ int bx = noteMenuX + i * (bw + gap); if(px >= bx && px <= bx + bw) return i; }
  return -1;
}
// Tecla de funcion. Misma firma de siempre (la usan Bloqueo y Wi-Fi), pero por
// dentro ya pasa por kbPaintKey: hereda estilo, contraste alto y tamano de
// fuente de la Fase E sin que esas tres superficies tengan que enterarse.
static void kbFKey(int x, int fy, int w, const char* label, bool on){
  kbPaintKey(x, fy, w, KB_KH, label, kbFontSize() > 2 ? 2 : kbFontSize(),
             on ? kbColFnOn() : kbColFn(), on ? kbColFnOnTxt() : kbColKeyTxt(), false);
}
// Etiqueta de la tecla de capa (?123 / emoji / ABC), en un solo sitio.
static const char* kbLayerLabel(){
  return (mapaActivo == LAYOUT_NUM) ? "emoji" : (mapaActivo == LAYOUT_EMOJI) ? "ABC" : "?123";
}
static void noteDrawFuncRow(int yoff){
  int fy = kbFuncY() + yoff;
  const char* lb[KB_FKEYS] = { "shift", kbLayerLabel(), kbLangEs ? "ES" : "EN", "espacio", "<-", "ent" };
  for(int i = 0; i < KB_FKEYS; i++) kbFKey(kbFKeyX(i), fy, kbFKeyW(i), lb[i], (i == 0) && kbShift);
}

// #############################################################
// ##  FASE C · BARRA SUPERIOR DE ACCESOS RAPIDOS
// ##  emoji · idioma · portapapeles · ajustes · mas opciones
// ##  ------------------------------------------------------
// ##  Iconos VECTORIALES con las primitivas del propio motor
// ##  (nada de assets nuevos). Solo se dibuja en Notas: en la
// ##  pantalla de contrasena un boton de portapapeles o de
// ##  ajustes seria una via de escape, no una comodidad.
// #############################################################
#define KB_TB_EMOJI 0
#define KB_TB_LANG  1
#define KB_TB_CLIP  2
#define KB_TB_SET   3
#define KB_TB_MORE  4
#define KB_TB_N     5
static int kbToolX(int i){                 // centro X del boton i
  int step = SCR_W / KB_TB_N;
  return step / 2 + i * step;
}
static int kbToolHit(int px, int py){
  if(kbToolbarH() == 0) return -1;
  int y0 = kbToolbarY();
  if(py < y0 || py > y0 + 52) return -1;
  for(int i = 0; i < KB_TB_N; i++) if(abs(px - kbToolX(i)) <= 26) return i;
  return -1;
}
static void kbToolIcon(int idx, int cx, int cy, uint16_t col){
  switch(idx){
    case KB_TB_EMOJI:                                   // carita
      drawCircle(cx, cy, 11, col); drawCircle(cx, cy, 10, col);
      fillCircle(cx - 4, cy - 3, 2, col); fillCircle(cx + 4, cy - 3, 2, col);
      arcStroke(cx, cy + 1, 6, 20, 160, 2, col); break;
    case KB_TB_LANG:                                    // globo (cambiar idioma)
      drawCircle(cx, cy, 11, col);
      hLine(cx - 11, cy, 22, col);
      arcStroke(cx, cy, 11, 250, 290, 2, col); arcStroke(cx, cy, 11, 70, 110, 2, col);
      vLine(cx, cy - 11, 22, col); break;
    case KB_TB_CLIP:                                    // portapapeles
      drawRoundRect(cx - 8, cy - 10, 16, 21, 3, col);
      fillRoundRect(cx - 5, cy - 13, 10, 5, 2, col);
      hLine(cx - 4, cy - 1, 8, col); hLine(cx - 4, cy + 4, 8, col); break;
    case KB_TB_SET:                                     // engranaje
      drawCircle(cx, cy, 5, col);
      for(int k = 0; k < 6; k++){ float a = k * 1.0471976f;
        strokeSegAA(cx + cosf(a) * 7, cy + sinf(a) * 7, cx + cosf(a) * 11, cy + sinf(a) * 11, 2.2f, col); }
      break;
    default:                                            // mas opciones
      fillCircle(cx - 8, cy, 2, col); fillCircle(cx, cy, 2, col); fillCircle(cx + 8, cy, 2, col); break;
  }
}
static void kbDrawToolbar(int yoff){
  if(kbToolbarH() == 0) return;
  int y0 = kbToolbarY() + yoff, cy = y0 + 26;
  for(int i = 0; i < KB_TB_N; i++){
    int cx = kbToolX(i);
    fillCircle(cx, cy, 21, gKbHiCon ? rgb565(24,24,24) : rgb565(52,58,76));
    kbToolIcon(i, cx, cy, kbColKeyTxt());
  }
}

// #############################################################
// ##  FASE F · FRANJA DE CHIPS (sugerencias / simbolos)
// ##  En capas de letras: hasta 3 palabras del diccionario local.
// ##  En la capa numerica: los 4 simbolos personalizados (Fase E),
// ##  o sea "un tap extra" desde ?123, como se pidio.
// #############################################################
static const char* kbChipTxt[4];
static int      kbChipN = 0;
static uint32_t kbChipMs = 0;          // cuando cambio la lista (para el fundido de la Fase G)
static void kbChipsBuild(){
  const char* prev[4]; int prevN = kbChipN;
  for(int i = 0; i < prevN && i < 4; i++) prev[i] = kbChipTxt[i];
  kbChipN = 0;
  if(!kbChipsWant()) return;
  if(mapaActivo == LAYOUT_NUM){
    for(int i = 0; i < KB_SYMS; i++) kbChipTxt[kbChipN++] = kbSymAt(i);
  } else {
    char w[40];
    if(kbCurrentWord(noteBuffer, noteCur, w, sizeof(w)) > 0) kbChipN = kbSuggest(w, kbChipTxt, 3);
  }
  bool changed = (kbChipN != prevN);
  for(int i = 0; i < kbChipN && !changed; i++) if(kbChipTxt[i] != prev[i]) changed = true;
  if(changed) kbChipMs = millis();
}
static int kbChipHit(int px, int py){
  if(kbChipsH() == 0 || kbChipN <= 0) return -1;
  int y0 = kbChipsY();
  if(py < y0 || py > y0 + 32) return -1;
  int cw = (SCR_W - 12) / kbChipN;
  for(int i = 0; i < kbChipN; i++){ int x = 6 + i * cw; if(px >= x && px <= x + cw) return i; }
  return -1;
}
static void kbDrawChips(int yoff){
  if(kbChipsH() == 0) return;
  int y0 = kbChipsY() + yoff;
  // FASE G: los chips no aparecen de golpe, entran con un fundido corto.
  uint8_t a = 255;
  if(KB_ANIM_POLISH_ON && kbChipMs){
    uint32_t dt = millis() - kbChipMs;
    if(dt < 140) a = (uint8_t)(60 + dt * 195 / 140);
  }
  if(kbChipN <= 0) return;
  int cw = (SCR_W - 12) / kbChipN;
  for(int i = 0; i < kbChipN; i++){
    int x = 6 + i * cw;
    if(i > 0) fillRectA(x, y0 + 8, 1, 16, kbColEdge(), 120);
    drawTextCA(x + cw / 2, y0 + 8, kbChipTxt[i], 2, kbColKeyTxt(), a);
  }
}

// Dibuja el teclado completo (panel + extras + teclas) desplazado yoff pixeles
// hacia abajo. yoff != 0 solo durante la animacion de apertura de la Fase G.
static void noteRenderKeyboard(int yoff){
  setBuf(fb);
  int top = kbPanelTop(), py = top + yoff;
  // BORRADO OBLIGATORIO DE LA BANDA, SIEMPRE. Esto arregla el "teclado fantasma
  // borroso detras del teclado" y los chips que se emborronaban mas con cada
  // tecla:
  //   drawLiquidGlassPanel COPIA lo que hay debajo del panel, lo desenfoca y lo
  //   mezcla. Como aqui se dibuja directamente sobre fb, "lo que hay debajo" era
  //   el teclado del CUADRO ANTERIOR. Resultado: cada repintado desenfocaba el
  //   teclado anterior y lo dejaba pegado al fondo, y al repintar otra vez
  //   desenfocaba el desenfoque... por eso empeoraba tecla a tecla.
  //   Con el fondo plano debajo, el vidrio muestrea siempre lo mismo que cuando
  //   se repinta la pantalla entera: identico aspecto, cero acumulacion.
  fillRect(0, top - 2, SCR_W, SCR_H - (top - 2), rgb565(12,14,20));
  kbPaintPanel(py, uiGlass ? kbColPanel() : (gKbHiCon ? rgb565(0,0,0) : rgb565(18,20,28)));
  kbDrawToolbar(yoff);
  kbDrawChips(yoff);
  int fs = kbFontSize();
  for(int r = 0; r < KB_ROWS; r++) for(int c = 0; c < KB_COLS; c++){
    int x = KB_X + c * (KB_KW + KB_GAP), y = KB_Y + r * (KB_KH + KB_GAP) + yoff;
    const char* k = mapaActivo[r][c];
    char u[6];
    if(kbShift && k[1] == 0 && k[0] >= 'a' && k[0] <= 'z'){ u[0] = (char)(k[0] - 32); u[1] = 0; k = u; }
    int cell = r * KB_COLS + c;
    bool hot = kbCellHeld(cell) || kbFxLevel(cell) > 0;
    kbPaintKey(x, y, KB_KW, KB_KH, k, fs, kbColKey(), kbColKeyTxt(), hot);
  }
  noteDrawFuncRow(yoff);
  flxFlush(top - 2, SCR_H - 1);
}
static void noteRenderAll(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(12,14,20));
  strokeSegAA(30, 26, 18, 18, 2.4f, rgb565(255,255,255));
  strokeSegAA(18, 18, 30, 10, 2.4f, rgb565(255,255,255));
  drawTextC(SCR_W / 2, 14, noteTitleBar, 3, rgb565(255,255,255));   // nombre REAL del fichero abierto
  kbChipsBuild();
  noteDrawText();
  noteRenderKeyboard(0);
  flxFlushAll();
}
static void kbRenderPopup(int cell){
  int r = cell / KB_COLS, c = cell % KB_COLS;
  int kx = KB_X + c * (KB_KW + KB_GAP), ky = KB_Y + r * (KB_KH + KB_GAP);
  const char* var[4]; int n = kbGetVariants(mapaActivo[r][c][0], var);
  if(n == 0) return;
  int pw = 40, ph = 46, gap = 4, totw = n * pw + (n - 1) * gap;
  int px0 = kx + KB_KW / 2 - totw / 2; if(px0 < 4) px0 = 4; if(px0 + totw > SCR_W - 4) px0 = SCR_W - 4 - totw;
  int py0 = ky - ph - 10;
  kbPopX = px0; kbPopY = py0; kbPopN = n; kbPopW = pw; kbPopG = gap;
  setBuf(fb);
  fillRoundRect(px0 - 6, py0 - 6, totw + 12, ph + 12, 10, rgb565(40,44,58));
  for(int i = 0; i < n; i++){
    int x = px0 + i * (pw + gap);
    fillRoundRect(x, py0, pw, ph, 8, rgb565(64,68,86));
    drawTextC(x + pw / 2, py0 + ph / 2 - 12, var[i], 3, rgb565(255,255,255));
  }
  flxFlush(py0 - 8, ky + KB_KH);
}
static int kbPopupHit(int px, int py){
  for(int i = 0; i < kbPopN; i++){ int x = kbPopX + i * (kbPopW + kbPopG); if(px >= x && px <= x + kbPopW && py >= kbPopY && py <= kbPopY + 46) return i; }
  return -1;
}
// Accion de una tecla de FUNCION (0..5). Un solo sitio para las dos rutas de
// entrada: el disparo rapido de la Fase B y el disparo clasico al soltar.
static void noteFuncKey(int i){
  if(i == 0) kbShift = !kbShift;
  else if(i == 1){                                       // cicla ABC -> NUM -> EMOJI
    if(mapaActivo == LAYOUT_NUM) mapaActivo = LAYOUT_EMOJI;
    else if(mapaActivo == LAYOUT_EMOJI) mapaActivo = kbLangEs ? LAYOUT_ES : LAYOUT_EN;
    else mapaActivo = LAYOUT_NUM;
  }
  else if(i == 2){ kbLangEs = !kbLangEs; if(mapaActivo == LAYOUT_ES || mapaActivo == LAYOUT_EN) mapaActivo = kbLangEs ? LAYOUT_ES : LAYOUT_EN; }
  else if(i == 3) noteInsert(" ");
  else if(i == 4) noteBackspace();
  else if(i == 5) noteInsert("\n");
}
// FASE F: aceptar un chip. En la capa numerica el chip es un simbolo y se
// inserta tal cual; en las de letras SUSTITUYE la palabra en construccion y
// deja un espacio detras.
static void kbApplyChip(int i){
  if(i < 0 || i >= kbChipN) return;
  if(mapaActivo == LAYOUT_NUM){ noteInsert(kbChipTxt[i]); return; }
  const char* w = kbChipTxt[i];
  char tmp[40];
  while(kbCurrentWord(noteBuffer, noteCur, tmp, sizeof(tmp)) > 0) noteBackspace();
  noteInsert(w); noteInsert(" ");
}

// #############################################################
// ##  FASE D · PANEL DE PORTAPAPELES (rejilla de 2 columnas)
// ##  tap = pegar · pin = fijar/soltar · x = borrar esa ficha
// ##  cabecera: volver · filtro de fijados · vaciar (con aviso)
// #############################################################
static bool clipPanelOn   = false;
static bool clipFilterPin = false;
static bool clipAskClear  = false;      // se pidio vaciar: la cabecera pide confirmacion
static int  clipVis[CLIP_SLOTS], clipVisN = 0;
static void clipBuildVis(){
  clipVisN = 0;
  for(int i = 0; i < CLIP_SLOTS; i++){
    if(!gClip[i].used) continue;
    if(clipFilterPin && !gClip[i].pinned) continue;
    clipVis[clipVisN++] = i;
  }
}
static void clipCardRect(int k, int &x, int &y, int &w, int &h){
  int col = k % 2, row = k / 2;
  w = (SCR_W - 3 * 12) / 2; h = 116;
  x = 12 + col * (w + 12);
  y = 108 + row * (h + 12);
}
static void clipRenderPanel(){
  clipBuildVis();
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(12,14,20));
  // Cabecera, al espiritu de la captura: icono de teclado a la izquierda, pin y
  // papelera a la derecha.
  fillRect(0, 0, SCR_W, 64, rgb565(20,23,32));
  drawRoundRect(14, 22, 26, 20, 4, rgb565(235,238,246));
  for(int i = 0; i < 3; i++) fillRect(19 + i * 7, 28, 4, 3, rgb565(235,238,246));
  hLine(20, 36, 14, rgb565(235,238,246));
  drawText(54, 20, "Portapapeles", 3, rgb565(255,255,255));
  { int cx = SCR_W - 96, cy = 32;                                  // pin (filtra fijados)
    fillCircle(cx, cy, 20, clipFilterPin ? rgb565(60,110,235) : rgb565(40,45,60));
    fillRect(cx - 2, cy - 2, 4, 12, rgb565(240,242,248));
    fillRoundRect(cx - 7, cy - 11, 14, 9, 3, rgb565(240,242,248)); }
  { int cx = SCR_W - 42, cy = 32;                                  // papelera (vaciar no fijados)
    fillCircle(cx, cy, 20, clipAskClear ? rgb565(200,70,70) : rgb565(40,45,60));
    fillRoundRect(cx - 8, cy - 6, 16, 15, 3, rgb565(240,242,248));
    fillRect(cx - 10, cy - 9, 20, 3, rgb565(240,242,248));
    fillRect(cx - 3, cy - 12, 6, 3, rgb565(240,242,248)); }
  if(clipAskClear){
    drawTextC(SCR_W / 2, 74, "Toca otra vez la papelera para vaciar (los fijados se quedan)", 1, rgb565(240,180,120));
  } else {
    char sub[64]; snprintf(sub, sizeof(sub), "%d visibles - %d de %d ranuras en uso%s",
                           clipVisN, clipCount(), CLIP_SLOTS, clipFilterPin ? " - filtro: fijadas" : "");
    drawTextC(SCR_W / 2, 76, sub, 1, rgb565(150,158,178));
  }
  if(clipVisN == 0){
    drawTextC(SCR_W / 2, 300, "Nada copiado todavia", 2, rgb565(150,158,178));
    drawTextC(SCR_W / 2, 330, "Selecciona texto y toca Copiar", 1, rgb565(110,118,138));
  }
  for(int k = 0; k < clipVisN && k < 8; k++){
    int x, y, w, h; clipCardRect(k, x, y, w, h);
    int i = clipVis[k];
    if(uiGlass) drawLiquidGlassPanel(x, y, w, h, 12, rgb565(44,50,68));
    else fillRoundRect(x, y, w, h, 12, rgb565(34,38,50));
    // Texto recortado a 3 lineas con elipsis (la ficha no crece: la rejilla es fija)
    const char* s = gClip[i].text;
    int ty = y + 10, lx = x + 10, maxr = x + w - 34, lines = 0;
    char ln[40]; int lp = 0;
    for(int p = 0; s[p] && lines < 3; p++){
      ln[lp++] = s[p]; ln[lp] = 0;
      bool last = (s[p + 1] == 0);
      if(textW(ln, 1) > maxr - lx - 6 || lp >= 38 || s[p] == '\n' || last){
        if(!last && lines == 2){ ln[lp > 2 ? lp - 2 : 0] = 0; strncat(ln, "...", sizeof(ln) - strlen(ln) - 1); }
        drawTextClip(lx, ty, ln, 1, rgb565(228,232,242), maxr);
        ty += 16; lines++; lp = 0; ln[0] = 0;
      }
    }
    { int px2 = x + w - 20, py2 = y + 16;                          // pin de la ficha
      fillRect(px2 - 2, py2 - 1, 4, 9, gClip[i].pinned ? rgb565(90,170,255) : rgb565(96,102,120));
      fillRoundRect(px2 - 6, py2 - 9, 12, 8, 2, gClip[i].pinned ? rgb565(90,170,255) : rgb565(96,102,120)); }
    { int px2 = x + w - 20, py2 = y + h - 18;                      // borrar esta ficha
      strokeSegAA(px2 - 5, py2 - 5, px2 + 5, py2 + 5, 2.0f, rgb565(200,110,110));
      strokeSegAA(px2 + 5, py2 - 5, px2 - 5, py2 + 5, 2.0f, rgb565(200,110,110)); }
  }
  drawTextC(SCR_W / 2, SCR_H - 44, "Toca una ficha para pegarla en el cursor", 1, rgb565(120,128,148));
  flxFlushAll();
}
// Devuelve true si el toque era para el panel (siempre que este abierto).
static bool clipPanelTick(){
  if(!clipPanelOn) return false;
  if(!T.tap) return true;
  if(T.y < 64){
    if(T.x < 50){ clipPanelOn = false; clipAskClear = false; noteRenderAll(); return true; }        // volver
    if(abs(T.x - (SCR_W - 96)) <= 22){ clipFilterPin = !clipFilterPin; clipAskClear = false; clipRenderPanel(); return true; }
    if(abs(T.x - (SCR_W - 42)) <= 22){                                                              // vaciar (2 toques)
      if(clipAskClear){ clipClearUnpinned(); clipAskClear = false; }
      else clipAskClear = true;
      clipRenderPanel(); return true;
    }
    return true;
  }
  clipAskClear = false;
  for(int k = 0; k < clipVisN && k < 8; k++){
    int x, y, w, h; clipCardRect(k, x, y, w, h);
    if(T.x < x || T.x > x + w || T.y < y || T.y > y + h) continue;
    int i = clipVis[k];
    if(T.x > x + w - 34 && T.y < y + 34){ clipTogglePin(i); clipRenderPanel(); return true; }        // pin
    if(T.x > x + w - 34 && T.y > y + h - 34){ clipDel(i); clipRenderPanel(); return true; }          // borrar
    noteInsert(gClip[i].text);                                                                      // pegar
    clipPanelOn = false; noteRenderAll(); return true;
  }
  return true;
}

// ---- FASE C: menu "mas opciones" (2 acciones reales, sin relleno) ----
static bool kbMoreOn = false;
static int  kbMoreX = 0, kbMoreY = 0;
#define KB_MORE_N 2
static const char* KB_MORE_LBL[KB_MORE_N] = { "Seleccionar todo", "Insertar fecha y hora" };
static void kbDrawMore(){
  if(!kbMoreOn) return;
  int w = 250, h = KB_MORE_N * 38 + 12;
  kbMoreX = SCR_W - w - 10; kbMoreY = kbToolbarY() - h - 6;
  if(kbMoreY < 60) kbMoreY = 60;
  setBuf(fb);
  fillRoundRect(kbMoreX, kbMoreY, w, h, 12, rgb565(38,42,56));
  for(int i = 0; i < KB_MORE_N; i++)
    drawText(kbMoreX + 14, kbMoreY + 10 + i * 38 + 8, KB_MORE_LBL[i], 2, rgb565(238,242,250));
  flxFlush(kbMoreY - 2, kbMoreY + h + 2);
}
static int kbMoreHit(int px, int py){
  if(!kbMoreOn) return -1;
  int w = 250;
  if(px < kbMoreX || px > kbMoreX + w) return -1;
  for(int i = 0; i < KB_MORE_N; i++){ int y = kbMoreY + 10 + i * 38; if(py >= y && py < y + 38) return i; }
  return -1;
}

// true si el caracter que hay JUSTO ANTES del cursor es (ignorando mayusculas y
// tildes) el mismo que s. Se usa para no borrar de mas al elegir un acento.
static bool kbLastCharIs(const char* s){
  if(noteCur <= 0 || noteCur > (int)strlen(noteBuffer)) return false;
  const char* a = noteBuffer + utf8Prev(noteBuffer, noteCur);
  const char* b = s;
  return kbFoldCh(&a) == kbFoldCh(&b);
}
static void handleKeyRelease(int px, int py){
  if(px < 48 && py < 48){ appClose(); return; }
  // Con la via rapida confirmada, ESTA ruta ya no escribe: la tecla se escribio
  // al tocar. Sin esta linea la misma pulsacion entraria dos veces.
  if(kbFastActive()) return;
  int fi = kbFRowHit(px, py);
  if(fi >= 0){ noteFuncKey(fi); noteRenderAll(); return; }
  int cell = kbCellAt(px, py);
  if(cell >= 0){ kbFxStart(cell); kbPressChar(mapaActivo[cell / KB_COLS][cell % KB_COLS]); noteRenderAll(); return; }
}
// FASE G: animacion de apertura del teclado en Notas (0.3 s, interpolada,
// mismo espiritu que lsuKbAnim del Bloqueo, que ya la tenia).
static uint32_t noteKbAnim = 0;
static void noteEditorEnter(){
  mapaActivo = LAYOUT_ES; kbLangEs = true; kbShift = false; kbLpKey = -1; kbPopup = false;
  noteCur = strlen(noteBuffer); noteClearSel(); noteHandleDrag = 0;
  kbExtrasOn = true;                     // Notas SI muestra barra y chips
  kbApplySize(); kbMtSurfaceReset();
  clipPanelOn = false; kbMoreOn = false; clipAskClear = false; kbToastMs = 0;
  kbChipsBuild();
  noteKbAnim = KB_ANIM_POLISH_ON ? millis() : 0;
  noteRenderAll();
}
static void noteEditorTick(){
  int txTop = 48, txBot = noteTxtBot();

  // 0) FASE G - apertura del teclado: se interpola el desplazamiento vertical y
  // se vuelca SOLO la banda del teclado. Mientras dura, no se lee entrada.
  if(noteKbAnim){
    float p = (millis() - noteKbAnim) / 300.0f;
    if(p >= 1){ p = 1; noteKbAnim = 0; }
    noteRenderKeyboard((int)((1.0f - p) * (SCR_H - kbPanelTop())));
    return;
  }

  // Panel de portapapeles abierto: se lo come todo hasta que se cierre.
  if(clipPanelOn){ clipPanelTick(); return; }

  // FASE G - apagar el destello de la ultima tecla cuando cumple su tiempo.
  kbFxTick(kbColKey(), kbColKeyTxt());
  // FASE G - el chip "Copiado" se borra solo repintando su banda al caducar.
  if(KB_ANIM_POLISH_ON && kbToastMs && millis() - kbToastMs >= 1200){ kbToastMs = 0; noteDrawText(); }

  // 0-bis) FASE B - VIA RAPIDA: la tecla se escribe al TOCAR, por ID de
  // contacto, sin esperar a que el dedo se levante. Convive con T: todo lo de
  // abajo (manijas, long-press, menu) sigue siendo exactamente igual que antes.
  if(T.down && T.y >= kbPanelTop()) kbTypingMark();   // veto del gesto de suspension mientras se teclea
  if(KB_MULTITOUCH_ON && gKbFastType && !noteMenu && !kbPopup && !kbMoreOn){
    int n = kbMtPoll();
    for(int e = 0; e < n; e++){
      if(kbEvCell[e] >= 0){ kbFxStart(kbEvCell[e]); kbPressChar(mapaActivo[kbEvCell[e] / KB_COLS][kbEvCell[e] % KB_COLS]); }
      else if(kbEvFn[e] >= 0) noteFuncKey(kbEvFn[e]);
    }
    if(n > 0){ kbChipsBuild(); noteDrawText(); noteRenderKeyboard(0); }
  } else {
    // Con la via rapida en pausa (menu abierto, popup de acentos, menu "mas")
    // se OLVIDAN los contactos seguidos. Si no, al reanudar quedarian ids
    // "vivos" de dedos que ya se levantaron y la siguiente pulsacion de ese
    // mismo id no dispararia nada. Mientras tanto sigue funcionando la ruta
    // clasica de soltar, asi que no se pierde ninguna tecla.
    kbMtReset();
  }

  // 1) Menu contextual: intercepta toques en el area de texto
  if(noteMenu && T.released && T.tap && T.y < kbPanelTop()){
    int mi = noteMenuHit(T.x, T.y);
    if(mi >= 0){
      if(mi == 0) clipCut(); else if(mi == 1) clipCopy(); else if(mi == 2) clipPaste(); else selectAllTxt();
      if(mi != 3) noteMenu = false;
      noteRenderAll(); return;
    }
    noteMenu = false; noteClearSel(); noteCur = noteLayoutHit(T.x, T.y); noteRenderAll(); return;
  }

  // 2) Inicio de gesto: manija de seleccion o long-press de tecla
  if(T.pressed){
    noteHandleDrag = 0; kbLpKey = -1; kbPopup = false;
    if(noteHasSel() && T.y >= txTop && T.y < txBot + 30){
      if(abs(T.x - hASX) < 24 && abs(T.y - (hASY + 20)) < 28) noteHandleDrag = 1;
      else if(abs(T.x - hBSX) < 24 && abs(T.y - (hBSY + 20)) < 28) noteHandleDrag = 2;
    }
    if(!noteHandleDrag && T.y >= KB_Y){
      int cell = kbCellAt(T.x, T.y);
      kbLpKey = kbIsVowelCell(cell) ? cell : -1;
      // FASE G: con la escritura rapida APAGADA la tecla no se escribe hasta
      // soltar, asi que el destello es la unica senal de que el toque entro.
      if(!(KB_MULTITOUCH_ON && gKbFastType)) kbFxPress(cell, kbColKey(), kbColKeyTxt());
    }
    return;
  }

  // 3) Arrastre de manija -> extender seleccion
  if(noteHandleDrag && T.down){
    int bi = noteLayoutHit(T.x, T.y);
    if(noteHandleDrag == 1){ if(bi >= 0 && bi < noteSelB) noteSelA = bi; }
    else { if(bi > noteSelA) noteSelB = bi; }
    noteCur = (noteHandleDrag == 1) ? noteSelA : noteSelB;
    noteRenderAll(); return;
  }

  // 4) Long-press en texto -> seleccionar palabra
  if(!noteHandleDrag && kbLpKey < 0 && T.down && !noteHasSel()
     && T.startY >= txTop && T.startY < txBot && (millis() - T.downMs) > 500
     && abs(T.x - T.startX) < 12 && abs(T.y - T.startY) < 12){
    selectWordAt(noteLayoutHit(T.startX, T.startY)); noteRenderAll(); return;
  }

  // 5) Long-press en tecla -> popup de acentos (umbral configurable, Fase E)
  if(kbLpKey >= 0 && T.down && !kbPopup && (millis() - T.downMs) > (unsigned long)gKbLpMs){ kbPopup = true; kbRenderPopup(kbLpKey); }

  // 6) Soltar
  if(T.released){
    if(noteHandleDrag){ noteHandleDrag = 0; noteMenu = true; noteRenderAll(); return; }
    if(kbPopup){
      int v = kbPopupHit(T.x, T.y); const char* var[4];
      const char* base = mapaActivo[kbLpKey / KB_COLS][kbLpKey % KB_COLS];
      // Con la via rapida, la letra base YA se escribio al tocar. Se comprueba
      // que el caracter anterior al cursor sea EXACTAMENTE esa letra antes de
      // borrar nada: asi, si por lo que fuera no llego a escribirse, no se come
      // el caracter de al lado.
      bool baseYaEscrita = kbFastActive() && kbLastCharIs(base);
      if(v >= 0){
        if(baseYaEscrita) noteBackspace();
        kbGetVariants(base[0], var); noteInsert(var[v]);
      }
      else if(!baseYaEscrita) kbPressChar(base);
      kbPopup = false; kbLpKey = -1; noteRenderAll(); return;
    }
    if(T.tap){
      // FASE C - barra superior (solo si esta visible)
      if(kbMoreOn){
        int mi = kbMoreHit(T.x, T.y);
        kbMoreOn = false;
        if(mi == 0){ selectAllTxt(); noteRenderAll(); return; }
        if(mi == 1){ char d[48], h[12]; buildShortDate(d, sizeof(d)); clkStrBar(h, sizeof(h));
                     char ln[64]; snprintf(ln, sizeof(ln), "%s %s", d, h); noteInsert(ln); noteRenderAll(); return; }
        noteRenderAll(); return;
      }
      int ti = kbToolHit(T.x, T.y);
      if(ti >= 0){
        if(ti == KB_TB_EMOJI){ mapaActivo = LAYOUT_EMOJI; noteRenderAll(); }
        else if(ti == KB_TB_LANG){ noteFuncKey(2); noteRenderAll(); }              // misma logica que la tecla ES/EN
        else if(ti == KB_TB_CLIP){ if(KB_CLIPBOARD_MULTI_ON){ clipPanelOn = true; clipAskClear = false; clipRenderPanel(); } else { clipPaste(); noteRenderAll(); } }
        else if(ti == KB_TB_SET){ if(KB_SETTINGS_ON) kbsEnter(); }
        else { kbMoreOn = true; kbDrawMore(); }
        return;
      }
      int ci = kbChipHit(T.x, T.y);
      if(ci >= 0){ kbApplyChip(ci); noteRenderAll(); return; }
      if(T.y >= txTop && T.y < txBot){              // tap en texto -> posicionar cursor
        noteClearSel(); noteCur = noteLayoutHit(T.x, T.y); noteRenderAll(); return;
      }
    }
    handleKeyRelease(T.x, T.y); kbLpKey = -1;         // teclado
  }
}

// #############################################################
// ##  KIT DE ARCHIVOS  ·  piezas de interfaz compartidas por
// ##  Notas, Paint y el Explorador de archivos
// ##  ------------------------------------------------------
// ##  Las tres pantallas hacen LO MISMO sobre ficheros
// ##  distintos: una rejilla o lista de elementos, el menu
// ##  contextual de 4 acciones (Seleccionar / Eliminar /
// ##  Renombrar / Papelera) y un dialogo de nombre con teclado.
// ##  Escribirlo tres veces era garantizar que las tres se
// ##  comportaran distinto ante el mismo error, asi que vive
// ##  aqui una sola vez.
// ##
// ##  REGLA DEL KIT: estas piezas NO simulan nada. Cada accion
// ##  llama a flexFs* (que toca el fichero real) y devuelve lo
// ##  que de verdad paso; quien la usa vuelve a leer el
// ##  directorio y repinta con lo que hay. Si un borrado falla,
// ##  el elemento sigue en la lista -- porque sigue en el disco.
// #############################################################
#define FK_ACT_SEL    0
#define FK_ACT_DEL    1
#define FK_ACT_REN    2
#define FK_ACT_TRASH  3
#define FK_MENU_W    258
#define FK_MENU_RH    46
#define FK_MENU_PAD   10

static const char* FK_MENU_LBL[4] = { "Seleccionar", "Eliminar", "Renombrar", "Papelera" };

// ---- Texto ajustado a una caja (para la vista previa REAL de una nota) ----
// Corta por caracteres, no por palabras, a proposito: el contenido de una nota
// puede ser una sola cadena larguisima sin espacios (como en las capturas) y un
// ajuste por palabras la dejaria fuera de la tarjeta.
static void fkTextBox(int x, int y, int w, int h, const char* s, int size, uint16_t col){
  if(!s || !*s) return;
  int lh = uiLineH(size) + 4, cy = y, li = 0;
  char line[80];
  const char* p = s;
  while(*p && cy + lh <= y + h){
    unsigned char b = (unsigned char)*p;
    if(b == '\n' || b == '\r'){ line[li] = 0; if(li) drawText(x, cy, line, size, col); cy += lh; li = 0; p++; continue; }
    int cl = 1;
    if((b & 0xE0) == 0xC0) cl = 2; else if((b & 0xF0) == 0xE0) cl = 3; else if((b & 0xF8) == 0xF0) cl = 4;
    if(li + cl >= (int)sizeof(line) - 1){ line[li] = 0; drawText(x, cy, line, size, col); cy += lh; li = 0; }
    memcpy(line + li, p, cl); line[li + cl] = 0;
    if(textW(line, size) > w){
      if(li == 0){ p += cl; continue; }        // un solo caracter mas ancho que la caja: se descarta
      line[li] = 0; drawText(x, cy, line, size, col); cy += lh; li = 0; continue;
    }
    li += cl; p += cl;
  }
  if(li && cy + lh <= y + h){ line[li] = 0; drawText(x, cy, line, size, col); }
}

// ---- Iconos del menu contextual (vectoriales, como el resto del sistema) ----
static void fkMenuGlyph(int k, int cx, int cy){
  uint16_t w = rgb565(255,255,255), red = rgb565(228,60,60), gr = rgb565(90,96,110);
  if(k == FK_ACT_SEL){                                  // mano que pulsa
    fillRoundRect(cx - 5, cy - 10, 10, 14, 4, gr);
    strokeSegAA(cx - 11, cy - 12, cx - 14, cy - 16, 1.8f, gr);
    strokeSegAA(cx,      cy - 14, cx,      cy - 19, 1.8f, gr);
    strokeSegAA(cx + 11, cy - 12, cx + 14, cy - 16, 1.8f, gr);
    fillRoundRect(cx - 8, cy + 2, 16, 8, 3, gr);
  } else if(k == FK_ACT_DEL){                           // papelera roja (borrado definitivo)
    fillRect(cx - 10, cy - 12, 20, 3, red);
    fillRect(cx - 4,  cy - 16, 8,  3, red);
    fillRoundRect(cx - 8, cy - 8, 16, 20, 3, red);
    fillRect(cx - 3, cy - 4, 2, 12, rgb565(255,255,255));
    fillRect(cx + 1, cy - 4, 2, 12, rgb565(255,255,255));
  } else if(k == FK_ACT_REN){                           // campo de texto con cursor
    fillRoundRect(cx - 14, cy - 7, 20, 14, 3, rgb565(40,44,56));
    fillRect(cx + 8, cy - 10, 2, 20, rgb565(40,44,56));
    fillRect(cx + 5, cy - 10, 8, 2,  rgb565(40,44,56));
    fillRect(cx + 5, cy + 8,  8, 2,  rgb565(40,44,56));
  } else {                                              // papelera blanca (mover a Papelera)
    fillRect(cx - 10, cy - 12, 20, 3, w);
    fillRect(cx - 4,  cy - 16, 8,  3, w);
    drawRoundRect(cx - 8, cy - 8, 16, 20, 3, w);
    fillRect(cx - 3, cy - 4, 2, 12, w);
    fillRect(cx + 1, cy - 4, 2, 12, w);
  }
}

static bool fkMenuOn = false;
static int  fkMenuX = 0, fkMenuY = 0;

static void fkMenuGeom(int &x, int &y, int &w, int &h){
  w = FK_MENU_W;
  h = 4 * FK_MENU_RH + 2 * FK_MENU_PAD;
  x = fkMenuX; y = fkMenuY;
  if(x + w > SCR_W - 8) x = SCR_W - 8 - w;
  if(x < 8) x = 8;
  if(y + h > SCR_H - 70) y = SCR_H - 70 - h;
  if(y < 30) y = 30;
}

static void fkMenuDraw(){
  int x, y, w, h; fkMenuGeom(x, y, w, h);
  if(uiGlass) drawLiquidGlassPanel(x, y, w, h, 18, rgb565(210,214,222));
  else        fillRoundRect(x, y, w, h, 18, rgb565(206,210,218));
  for(int i = 0; i < 4; i++){
    int ry = y + FK_MENU_PAD + i * FK_MENU_RH;
    drawText(x + 16, ry + 10, FK_MENU_LBL[i], 3, rgb565(16,18,24));
    fkMenuGlyph(i, x + w - 32, ry + FK_MENU_RH / 2);
  }
  flxFlush(y - 2, y + h + 2);
}

static void fkMenuOpen(int px, int py){ fkMenuOn = true; fkMenuX = px; fkMenuY = py; fkMenuDraw(); }

// -1 = toque fuera del panel (cierra sin accion); 0..3 = accion elegida.
static int fkMenuHit(int px, int py){
  int x, y, w, h; fkMenuGeom(x, y, w, h);
  if(px < x || px > x + w || py < y || py > y + h) return -1;
  int i = (py - y - FK_MENU_PAD) / FK_MENU_RH;
  if(i < 0) i = 0; if(i > 3) i = 3;
  return i;
}

// #############################################################
// ##  DIALOGO DE NOMBRE (renombrar / crear con nombre propio)
// ##  Reutiliza EL MISMO teclado del sistema (mapaActivo,
// ##  kbPaintKey, kbCellAt...) que usan Notas y la clave de
// ##  Wi-Fi: no hay un segundo teclado que mantener.
// #############################################################
static bool fkNameOn = false;
static char fkNameBuf[FLEXFS_NAME_MAX] = "";
static char fkNameTitle[40] = "";

static void fkNameDraw(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, kbPanelTop(), rgb565(14,16,24));
  drawTextC(SCR_W / 2, 40, fkNameTitle, 3, rgb565(255,255,255));
  strokeSegAA(30, 46, 18, 38, 2.4f, rgb565(255,255,255));      // chevron: cancelar
  strokeSegAA(18, 38, 30, 30, 2.4f, rgb565(255,255,255));
  int fy = 130;
  fillRoundRect(24, fy, SCR_W - 48, 56, 14, rgb565(30,34,48));
  drawTextClip(38, fy + 16, fkNameBuf, 2, rgb565(240,242,248), SCR_W - 40);
  int cw = textW(fkNameBuf, 2);
  fillRect(38 + cw + 2, fy + 14, 2, 28, rgb565(120,170,250));       // cursor
  drawTextC(SCR_W / 2, fy + 76, "Escribe el nombre y pulsa Guardar", 1, rgb565(140,148,168));

  int ky = KB_Y;
  if(uiGlass) drawLiquidGlassPanel(0, ky - 4, SCR_W, SCR_H - (ky - 4), 0, rgb565(36,40,58));
  else        fillRect(0, ky - 4, SCR_W, SCR_H - (ky - 4), rgb565(18,20,28));
  int fs = kbFontSize();
  for(int r = 0; r < KB_ROWS; r++) for(int c = 0; c < KB_COLS; c++){
    int x = KB_X + c * (KB_KW + KB_GAP), y = ky + r * (KB_KH + KB_GAP);
    const char* k = mapaActivo[r][c];
    char u[6];
    if(kbShift && k[1] == 0 && k[0] >= 'a' && k[0] <= 'z'){ u[0] = (char)(k[0] - 32); u[1] = 0; k = u; }
    kbPaintKey(x, y, KB_KW, KB_KH, k, fs, kbColKey(), kbColKeyTxt(), false);
  }
  int fry = ky + 3 * (KB_KH + KB_GAP);
  const char* lb[KB_FKEYS] = { "shift", kbLayerLabel(), kbLangEs ? "ES" : "EN", "espacio", "<-", "Guardar" };
  for(int i = 0; i < KB_FKEYS; i++) kbFKey(kbFKeyX(i), fry, kbFKeyW(i), lb[i], (i == 0) && kbShift);
  flxFlushAll();
}

static void fkNameOpen(const char* title, const char* initial){
  fkNameOn = true;
  strncpy(fkNameTitle, title, sizeof(fkNameTitle) - 1); fkNameTitle[sizeof(fkNameTitle) - 1] = 0;
  strncpy(fkNameBuf, initial ? initial : "", sizeof(fkNameBuf) - 1); fkNameBuf[sizeof(fkNameBuf) - 1] = 0;
  mapaActivo = LAYOUT_ES; kbLangEs = true; kbShift = false;
  kbExtrasOn = false; kbApplySize(); kbMtSurfaceReset();
  fkNameDraw();
}

static void fkNameAppend(const char* s){
  int L = strlen(fkNameBuf), sl = strlen(s);
  // '/' partiria la ruta y dejaria el fichero en otra carpeta sin avisar.
  if(strchr(s, '/')) return;
  if(L + sl < (int)sizeof(fkNameBuf) - 1){ memcpy(fkNameBuf + L, s, sl); fkNameBuf[L + sl] = 0; }
}
static void fkNameBack(){
  int L = strlen(fkNameBuf);
  if(L > 0){ int q = L - 1; while(q > 0 && (fkNameBuf[q] & 0xC0) == 0x80) q--; fkNameBuf[q] = 0; }
}

// 0 = sigue abierto, 1 = aceptado (fkNameBuf es valido), -1 = cancelado.
static int fkNameTick(){
  if(!T.released) return 0;
  int fi = kbFRowHit(T.x, T.y);
  if(fi >= 0){
    if(fi == 0){ kbShift = !kbShift; fkNameDraw(); return 0; }
    if(fi == 1){ mapaActivo = (mapaActivo == LAYOUT_NUM) ? LAYOUT_EMOJI
                             : (mapaActivo == LAYOUT_EMOJI) ? (kbLangEs ? LAYOUT_ES : LAYOUT_EN)
                             : LAYOUT_NUM; fkNameDraw(); return 0; }
    if(fi == 2){ kbLangEs = !kbLangEs;
                 if(mapaActivo == LAYOUT_ES || mapaActivo == LAYOUT_EN) mapaActivo = kbLangEs ? LAYOUT_ES : LAYOUT_EN;
                 fkNameDraw(); return 0; }
    if(fi == 3){ fkNameAppend(" "); fkNameDraw(); return 0; }
    if(fi == 4){ fkNameBack(); fkNameDraw(); return 0; }
    if(fi == 5){ fkNameOn = false; return strlen(fkNameBuf) > 0 ? 1 : -1; }
    return 0;
  }
  if(T.tap && T.y < 90 && T.x < 60){ fkNameOn = false; return -1; }   // esquina sup. izq. = cancelar
  int cell = kbCellAt(T.x, T.y);
  if(cell >= 0){
    const char* k = mapaActivo[cell / KB_COLS][cell % KB_COLS];
    char u[6];
    if(kbShift && k[1] == 0 && k[0] >= 'a' && k[0] <= 'z'){ u[0] = (char)(k[0] - 32); u[1] = 0; k = u; kbShift = false; }
    fkNameAppend(k);
    fkNameDraw();
  }
  return 0;
}

// #############################################################
// ##  CONFIRMACION (borrado definitivo, vaciar papelera...)
// ##  Un borrado real no se pregunta con un toast: si el
// ##  usuario dice que si, el fichero desaparece del disco y no
// ##  hay vuelta atras.
// #############################################################
static bool fkAskOn = false;
static char fkAskMsg[72] = "";
static char fkAskSub[72] = "";

static void fkAskGeom(int &x, int &y, int &w, int &h){ w = SCR_W - 72; h = 220; x = 36; y = (SCR_H - h) / 2; }

static void fkAskDraw(){
  int x, y, w, h; fkAskGeom(x, y, w, h);
  setBuf(fb);
  if(uiGlass) drawLiquidGlassPanel(x, y, w, h, 24, rgb565(60,64,88));
  else        fillRoundRect(x, y, w, h, 24, rgb565(34,38,50));
  drawTextC(SCR_W / 2, y + 26, fkAskMsg, 2, rgb565(255,255,255));
  if(fkAskSub[0]) drawTextC(SCR_W / 2, y + 62, fkAskSub, 1, rgb565(170,178,196));
  int by = y + h - 76, bw = (w - 48) / 2;
  fillRoundRect(x + 16, by, bw, 56, 16, rgb565(70,74,90));
  drawTextC(x + 16 + bw / 2, by + 18, "Cancelar", 2, rgb565(240,242,248));
  fillRoundRect(x + 32 + bw, by, bw, 56, 16, rgb565(220,70,70));
  drawTextC(x + 32 + bw + bw / 2, by + 18, "Borrar", 2, rgb565(255,255,255));
  flxFlush(y - 2, y + h + 2);
}

static void fkAskOpen(const char* msg, const char* sub){
  fkAskOn = true;
  strncpy(fkAskMsg, msg, sizeof(fkAskMsg) - 1); fkAskMsg[sizeof(fkAskMsg) - 1] = 0;
  strncpy(fkAskSub, sub ? sub : "", sizeof(fkAskSub) - 1); fkAskSub[sizeof(fkAskSub) - 1] = 0;
  fkAskDraw();
}

// 0 = sigue abierto, 1 = confirmado, -1 = cancelado.
static int fkAskTick(){
  if(!T.tap) return 0;
  int x, y, w, h; fkAskGeom(x, y, w, h);
  int by = y + h - 76, bw = (w - 48) / 2;
  if(T.y >= by && T.y <= by + 56){
    if(T.x >= x + 16 && T.x <= x + 16 + bw){ fkAskOn = false; return -1; }
    if(T.x >= x + 32 + bw && T.x <= x + 32 + 2 * bw){ fkAskOn = false; return 1; }
  }
  if(T.x < x || T.x > x + w || T.y < y || T.y > y + h){ fkAskOn = false; return -1; }
  return 0;
}

// ---- Aviso de "sin almacenamiento" ---------------------------
// Cuando LittleFS no monta, estas pantallas NO tienen nada real que
// ensenar. Antes que inventar una lista vacia bonita, se dice el
// motivo exacto y como arreglarlo.
static void fkNoFsScreen(const char* titulo){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(14,16,24));
  strokeSegAA(30, 26, 18, 18, 2.4f, rgb565(255,255,255));
  strokeSegAA(18, 18, 30, 10, 2.4f, rgb565(255,255,255));
  drawTextC(SCR_W / 2, 40, titulo, 3, rgb565(255,255,255));
  drawTextC(SCR_W / 2, 300, "Sin almacenamiento", 3, rgb565(240,140,140));
  drawTextC(SCR_W / 2, 344, flexFsError(), 1, rgb565(170,178,196));
  drawTextC(SCR_W / 2, 380, "Arduino IDE > Herramientas > Partition Scheme", 1, rgb565(140,148,168));
  flxFlushAll();
}

// #############################################################
// ##  PAPELERA  ·  navegador compartido de /Papelera
// ##  ------------------------------------------------------
// ##  Lo abren Notas, Paint y el Explorador. Lista lo que hay
// ##  DE VERDAD en /Papelera y ofrece las dos unicas cosas que
// ##  se pueden hacer con un fichero tirado: devolverlo a su
// ##  sitio o borrarlo para siempre.
// ##
// ##  La ruta original no se guarda en un indice aparte: viaja
// ##  DENTRO del nombre del fichero ('/' se codifica como '@'),
// ##  asi que restaurar es exacto aunque el sistema se apague a
// ##  media operacion. Ver flexFsTrash() en FlexOS_FS.cpp.
// #############################################################
#define FK_TRASH_MAX  16
#define FK_TRASH_TOP  120
#define FK_TRASH_RH    66

static bool        fkTrashOn = false;
static FlexFsEntry fkTrashList[FK_TRASH_MAX];
static int         fkTrashN = 0;
static int         fkTrashSel = -1;
static int         fkTrashScroll = 0;
static int         fkTrashDragY0 = 0, fkTrashDragS0 = 0;
static bool        fkTrashDragging = false;

static void fkTrashReload(){
  fkTrashN = flexFsList(FLEXFS_DIR_TRASH, fkTrashList, FK_TRASH_MAX);
  if(fkTrashSel >= fkTrashN) fkTrashSel = -1;
}

static int fkTrashRowY(int i){ return FK_TRASH_TOP + i * FK_TRASH_RH - fkTrashScroll; }

static int fkTrashMaxScroll(){
  int need = FK_TRASH_TOP + fkTrashN * FK_TRASH_RH + 30;
  int m = need - (SCR_H - 140);
  return m > 0 ? m : 0;
}

static void fkTrashRender(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(14,16,24));
  strokeSegAA(30, 26, 18, 18, 2.4f, rgb565(255,255,255));
  strokeSegAA(18, 18, 30, 10, 2.4f, rgb565(255,255,255));
  drawTextC(SCR_W / 2, 34, "Papelera", 4, rgb565(255,255,255));

  char hdr[64];
  uint32_t used = flexFsDirSize(FLEXFS_DIR_TRASH);
  char sz[24]; flexFsFmtSize(used, sz, sizeof(sz));
  snprintf(hdr, sizeof(hdr), "%d elementos  ·  %s", fkTrashN, sz);
  drawTextC(SCR_W / 2, 86, hdr, 1, rgb565(150,158,178));

  if(fkTrashN == 0){
    drawTextC(SCR_W / 2, 320, "La papelera est\xC3\xA1 vac\xC3\xAD" "a", 2, rgb565(150,158,178));
  }
  for(int i = 0; i < fkTrashN; i++){
    int y = fkTrashRowY(i);
    if(y + FK_TRASH_RH < FK_TRASH_TOP - 40 || y > SCR_H - 130) continue;
    bool sel = (i == fkTrashSel);
    fillRoundRect(16, y, SCR_W - 32, FK_TRASH_RH - 8, 14, sel ? rgb565(46,56,84) : rgb565(30,34,48));
    // Se muestra la ruta ORIGINAL decodificada: es lo unico que le dice al
    // usuario de donde salio ese fichero.
    char origen[FLEXFS_PATH_MAX];
    if(!flexFsTrashOrigin(fkTrashList[i].name, origen, sizeof(origen)))
      snprintf(origen, sizeof(origen), "%s", fkTrashList[i].name);
    const char* nm = strrchr(origen, '/');
    nm = nm ? nm + 1 : origen;
    drawTextClip(30, y + 8, nm, 2, rgb565(240,242,248), SCR_W - 40);
    char sub[FLEXFS_PATH_MAX + 24], s2[24];
    flexFsFmtSize(fkTrashList[i].size, s2, sizeof(s2));
    snprintf(sub, sizeof(sub), "%s  ·  %s", origen, s2);
    drawTextClip(30, y + 34, sub, 1, rgb565(140,148,168), SCR_W - 40);
  }

  int by = SCR_H - 122;
  if(fkTrashSel >= 0){
    int bw = (SCR_W - 48) / 2;
    fillRoundRect(16, by, bw, 56, 16, rgb565(60,150,110));
    drawTextC(16 + bw / 2, by + 18, "Restaurar", 2, rgb565(255,255,255));
    fillRoundRect(32 + bw, by, bw, 56, 16, rgb565(200,60,60));
    drawTextC(32 + bw + bw / 2, by + 18, "Borrar", 2, rgb565(255,255,255));
  } else if(fkTrashN > 0){
    fillRoundRect(SCR_W / 2 - 120, by, 240, 56, 16, rgb565(70,74,90));
    drawTextC(SCR_W / 2, by + 18, "Vaciar papelera", 2, rgb565(250,190,190));
  }
  if(fkAskOn) fkAskDraw();
  flxFlushAll();
}

static void fkTrashOpen(){
  fkTrashOn = true; fkTrashSel = -1; fkTrashScroll = 0; fkAskOn = false;
  fkTrashReload();
  fkTrashRender();
}

// Devuelve true mientras la papelera siga abierta (el llamante no debe
// hacer nada mas); false cuando el usuario ha salido.
static bool fkTrashTick(){
  if(!fkTrashOn) return false;

  if(fkAskOn){
    int r = fkAskTick();
    if(r == 1){
      if(fkTrashSel >= 0 && fkTrashSel < fkTrashN){
        char p[FLEXFS_PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", FLEXFS_DIR_TRASH, fkTrashList[fkTrashSel].name);
        flexFsDelete(p);                       // definitivo: ya no hay vuelta atras
      } else {
        flexFsEmptyTrash();
      }
      fkTrashSel = -1; fkTrashReload();
    }
    if(r != 0) fkTrashRender();
    return true;
  }

  int maxS = fkTrashMaxScroll();
  if(T.pressed){ fkTrashDragY0 = T.y; fkTrashDragS0 = fkTrashScroll; fkTrashDragging = false; }
  if(T.down && maxS > 0){
    int dy = fkTrashDragY0 - T.y;
    if(!fkTrashDragging && abs(dy) > 8) fkTrashDragging = true;
    if(fkTrashDragging){
      int ns = fkTrashDragS0 + dy;
      if(ns < 0) ns = 0; if(ns > maxS) ns = maxS;
      if(ns != fkTrashScroll){ fkTrashScroll = ns; fkTrashRender(); }
      return true;
    }
  }
  if(T.released && fkTrashDragging){ fkTrashDragging = false; return true; }
  if(!T.tap) return true;

  if(T.x < 60 && T.y < 60){ fkTrashOn = false; return false; }      // volver

  int by = SCR_H - 122;
  if(T.y >= by && T.y <= by + 56){
    if(fkTrashSel >= 0){
      int bw = (SCR_W - 48) / 2;
      if(T.x <= 16 + bw){
        char nm[FLEXFS_NAME_MAX];
        strncpy(nm, fkTrashList[fkTrashSel].name, sizeof(nm) - 1); nm[sizeof(nm) - 1] = 0;
        flexFsRestore(nm);                     // vuelve a su carpeta original, de verdad
        fkTrashSel = -1; fkTrashReload(); fkTrashRender(); return true;
      }
      if(T.x >= 32 + bw){
        char stem[FLEXFS_NAME_MAX]; flexFsStem(fkTrashList[fkTrashSel].name, stem, sizeof(stem));
        fkAskOpen("\xC2\xBF" "Borrar definitivamente?", stem);
        return true;
      }
    } else if(fkTrashN > 0 && T.x > SCR_W / 2 - 120 && T.x < SCR_W / 2 + 120){
      fkAskOpen("\xC2\xBF" "Vaciar la papelera?", "Se borrar\xC3\xA1 todo su contenido");
      return true;
    }
  }
  for(int i = 0; i < fkTrashN; i++){
    int y = fkTrashRowY(i);
    if(T.y >= y && T.y < y + FK_TRASH_RH - 8){ fkTrashSel = (fkTrashSel == i) ? -1 : i; fkTrashRender(); return true; }
  }
  return true;
}

// #############################################################
// ##  APP NOTAS  ·  lista de notas REALES en /Notas
// ##  ------------------------------------------------------
// ##  Cada tarjeta es un fichero .txt de verdad: el titulo sale
// ##  del NOMBRE del fichero y la vista previa de sus primeros
// ##  bytes, leidos del disco. No hay texto de relleno en
// ##  ninguna parte -- una nota vacia se ve vacia.
// ##
// ##  El editor sigue siendo el de siempre (teclado de 4 capas,
// ##  seleccion, portapapeles): lo unico que cambia es que
// ##  ahora esta ATADO a un fichero. Se carga al abrirlo y se
// ##  guarda al salir y 2 s despues de la ultima tecla, para
// ##  que un apagon no se lleve lo escrito.
// #############################################################
#define NOTE_MAX_LIST   16
#define NOTE_PREV_LEN    96
#define NOTE_COLS        2
#define NOTE_CARD_TOP   96
#define NOTE_AUTOSAVE_MS 2000

static FlexFsEntry noteList[NOTE_MAX_LIST];
static char        notePrev[NOTE_MAX_LIST][NOTE_PREV_LEN];
static int         noteListN = 0;
static int         noteView  = 0;                        // 0 = lista, 1 = editor
static int         noteSelIdx = -1;                      // elemento sobre el que actua el menu
static char        notePath[FLEXFS_PATH_MAX] = "";       // fichero abierto en el editor
static int         noteScroll = 0;
static bool        noteMulti = false;                    // modo "Seleccionar"
static uint32_t    noteMask = 0;                         // marcados en modo seleccion
static uint32_t    noteDirtyMs = 0;                      // ultima tecla (autoguardado)
static bool        noteLongFired = false;
static int         noteDragY0 = 0, noteDragS0 = 0;
static bool        noteDragging = false;

static void noteRenderList();
static void noteEditorEnter();
static void noteEditorTick();

// Relee /Notas del disco. Se llama al entrar y despues de CADA
// operacion: la lista siempre es un reflejo de lo que hay, nunca un
// estado en RAM que se actualiza "a mano" y se puede desincronizar.
static void noteReload(){
  noteListN = flexFsList(FLEXFS_DIR_NOTAS, noteList, NOTE_MAX_LIST);
  for(int i = 0; i < noteListN; i++){
    notePrev[i][0] = 0;
    if(noteList[i].dir) continue;
    char p[FLEXFS_PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", FLEXFS_DIR_NOTAS, noteList[i].name);
    flexFsReadText(p, notePrev[i], NOTE_PREV_LEN);      // contenido REAL del fichero
  }
  if(noteScroll > noteListN) noteScroll = 0;
}

static int noteCardH(){
  // Proporcional a la pantalla: el Pro tiene 640 px de alto logico y con una
  // altura fija de 300 solo cabia una fila.
  int h = (SCR_H - NOTE_CARD_TOP - 90) / 2 - 44;
  if(h < 120) h = 120;
  if(h > 300) h = 300;
  return h;
}
static void noteCardRect(int i, int &x, int &y, int &w, int &h){
  int col = i % NOTE_COLS, row = i / NOTE_COLS;
  w = (SCR_W - 3 * 16) / NOTE_COLS;
  h = noteCardH();
  x = 16 + col * (w + 16);
  y = NOTE_CARD_TOP + row * (h + 44) - noteScroll;
}

static int noteMaxScroll(){
  int rows = (noteListN + NOTE_COLS - 1) / NOTE_COLS;
  int need = NOTE_CARD_TOP + rows * (noteCardH() + 44) + 40;
  int m = need - (SCR_H - 60);
  return m > 0 ? m : 0;
}

// Boton flotante de nueva nota (abajo a la derecha, como en la captura).
static void noteFabRect(int &x, int &y, int &r){ r = 56; x = SCR_W - 84; y = SCR_H - 150; }

static void noteDrawFab(){
  int fx, fy, fr; noteFabRect(fx, fy, fr);
  fillCircle(fx, fy, fr, rgb565(16,18,24));
  fillCircle(fx, fy, fr - 5, rgb565(250,250,252));
  fillRoundRect(fx - 22, fy - 24, 34, 44, 4, rgb565(20,22,30));
  for(int i = 0; i < 3; i++) fillRect(fx - 17, fy - 17 + i * 10, 24 - i * 6, 4, rgb565(250,250,252));
  strokeSegAA(fx + 4, fy + 18, fx + 24, fy - 6, 5.0f, rgb565(20,22,30));   // lapiz
  strokeSegAA(fx + 22, fy - 8, fx + 27, fy - 13, 4.0f, rgb565(20,22,30));
}

static void noteRenderList(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(214,214,214));
  drawText(16, 24, "Notas:", 5, rgb565(16,18,24));
  // Tres puntos: menu del elemento seleccionado (o de la app si no hay ninguno).
  for(int i = 0; i < 3; i++) fillCircle(SCR_W - 28, 34 + i * 16, 5, rgb565(16,18,24));
  strokeSegAA(30, 26, 18, 18, 2.4f, rgb565(16,18,24));       // chevron de volver
  strokeSegAA(18, 18, 30, 10, 2.4f, rgb565(16,18,24));

  if(noteListN == 0){
    drawTextC(SCR_W / 2, 320, "No hay notas todav\xC3\xAD" "a", 3, rgb565(90,94,104));
    drawTextC(SCR_W / 2, 364, "Pulsa el boton de abajo para crear una", 1, rgb565(120,124,136));
  }
  for(int i = 0; i < noteListN; i++){
    int x, y, w, h; noteCardRect(i, x, y, w, h);
    if(y + h < 60 || y > SCR_H) continue;
    char title[FLEXFS_NAME_MAX];
    flexFsStem(noteList[i].name, title, sizeof(title));      // titulo = nombre REAL del fichero
    drawTextC(x + w / 2, y - 34, title, 2, rgb565(16,18,24));
    fillRoundRect(x, y, w, h, 14, rgb565(255,255,255));
    if(noteMulti && (noteMask & (1UL << i))){
      drawRoundRect(x, y, w, h, 14, rgb565(60,120,235));
      drawRoundRect(x + 1, y + 1, w - 2, h - 2, 13, rgb565(60,120,235));
      fillCircle(x + w - 20, y + 20, 11, rgb565(60,120,235));
      strokeSegAA(x + w - 26, y + 20, x + w - 22, y + 25, 2.4f, rgb565(255,255,255));
      strokeSegAA(x + w - 22, y + 25, x + w - 14, y + 14, 2.4f, rgb565(255,255,255));
    }
    if(notePrev[i][0]) fkTextBox(x + 10, y + 10, w - 20, h - 20, notePrev[i], 2, rgb565(24,26,34));
    else               drawText(x + 10, y + 10, "(vac\xC3\xAD" "a)", 2, rgb565(170,174,184));
  }
  if(noteMulti){
    // Barra de acciones del modo seleccion: opera sobre TODOS los marcados.
    int by = SCR_H - 128;
    fillRoundRect(12, by, SCR_W - 24, 60, 16, rgb565(30,34,46));
    drawText(28, by + 20, "Selecci\xC3\xB3n", 2, rgb565(240,242,248));
    drawTextR(SCR_W - 140, by + 20, "Papelera", 2, rgb565(250,210,120));
    drawTextR(SCR_W - 28,  by + 20, "Salir", 2, rgb565(180,188,205));
  } else {
    noteDrawFab();
  }
  if(fkMenuOn) fkMenuDraw();
  flxFlushAll();
}

// Ruta completa del elemento i de la lista.
static void notePathOf(int i, char* out, size_t n){
  snprintf(out, n, "%s/%s", FLEXFS_DIR_NOTAS, noteList[i].name);
}

// Abre el editor sobre un fichero REAL: lo lee entero a la memoria de
// trabajo (PSRAM) y deja el cursor al final.
static void noteOpen(int i){
  if(i < 0 || i >= noteListN) return;
  notePathOf(i, notePath, sizeof(notePath));
  noteBufInit();
  flexFsReadText(notePath, noteBuffer, noteBufMax);
  flexFsStem(noteList[i].name, noteTitleBar, sizeof(noteTitleBar));
  noteView = 1; noteDirtyMs = 0;
  noteEditorEnter();
}

// Guardado REAL del editor al fichero abierto.
static void noteSave(){
  if(noteView != 1 || !notePath[0]) return;
  flexFsWriteText(notePath, noteBuffer);
  noteDirtyMs = 0;
}

static void noteBackToList(){
  noteSave();
  noteView = 0; notePath[0] = 0;
  noteReload();
  noteRenderList();
}

// Nueva nota: crea el FICHERO ya, vacio, con el primer numero libre de
// la carpeta. Si el fichero no se crea (disco lleno), no aparece nada en
// la lista -- porque no existe.
static void noteNew(){
  char full[FLEXFS_PATH_MAX];
  if(!flexFsNewName(FLEXFS_DIR_NOTAS, "Sin t\xC3\xAD" "tulo", FLEXFS_EXT_NOTE, full, sizeof(full))) return;
  if(!flexFsWriteText(full, "")) return;
  noteReload();
  for(int i = 0; i < noteListN; i++){
    char p[FLEXFS_PATH_MAX]; notePathOf(i, p, sizeof(p));
    if(!strcmp(p, full)){ noteOpen(i); return; }
  }
  noteRenderList();
}

// Accion del menu contextual sobre la nota seleccionada. TODAS tocan el
// fichero real; ninguna se limita a cambiar la pantalla.
static void noteMenuAction(int act){
  char p[FLEXFS_PATH_MAX];
  if(noteSelIdx >= 0 && noteSelIdx < noteListN) notePathOf(noteSelIdx, p, sizeof(p));
  else p[0] = 0;
  if(act == FK_ACT_SEL){
    noteMulti = true; noteMask = 0;
    if(noteSelIdx >= 0) noteMask |= (1UL << noteSelIdx);
  } else if(act == FK_ACT_DEL){
    if(p[0]){
      char stem[FLEXFS_NAME_MAX]; flexFsStem(noteList[noteSelIdx].name, stem, sizeof(stem));
      fkAskOpen("\xC2\xBF" "Borrar definitivamente?", stem);
      return;                                  // el borrado ocurre al confirmar
    }
  } else if(act == FK_ACT_REN){
    if(p[0]){
      char stem[FLEXFS_NAME_MAX]; flexFsStem(noteList[noteSelIdx].name, stem, sizeof(stem));
      fkNameOpen("Renombrar nota", stem);
      return;
    }
  } else if(act == FK_ACT_TRASH){
    if(p[0]){ flexFsTrash(p); noteSelIdx = -1; noteReload(); }  // a /Papelera de verdad
    else { fkTrashOpen(); return; }            // sin seleccion: abre la papelera
  }
  noteRenderList();
}

static void noteListTick(){
  // --- Dialogos modales: se comen el toque hasta que se cierran ---
  if(fkTrashOn){ if(!fkTrashTick()){ noteReload(); noteRenderList(); } return; }
  if(fkAskOn){
    int r = fkAskTick();
    if(r == 1 && noteSelIdx >= 0 && noteSelIdx < noteListN){
      char p[FLEXFS_PATH_MAX]; notePathOf(noteSelIdx, p, sizeof(p));
      flexFsDelete(p);                          // borrado DEFINITIVO real
      noteSelIdx = -1; noteReload();
    }
    if(r != 0) noteRenderList();
    return;
  }
  if(fkNameOn){
    int r = fkNameTick();
    if(r == 1 && noteSelIdx >= 0 && noteSelIdx < noteListN){
      char p[FLEXFS_PATH_MAX]; notePathOf(noteSelIdx, p, sizeof(p));
      flexFsRename(p, fkNameBuf);               // renombrado REAL en el disco
      noteSelIdx = -1; noteReload();
    }
    if(r != 0) noteRenderList();
    return;
  }
  if(fkMenuOn){
    if(T.tap){
      int a = fkMenuHit(T.x, T.y);
      fkMenuOn = false;
      if(a >= 0) noteMenuAction(a);
      else       noteRenderList();
    }
    return;
  }

  // --- Scroll ---
  // T.dx/T.dy solo se rellenan AL SOLTAR (ver tDoRelease), asi que el arrastre
  // se sigue con el mismo patron que la lista de Ajustes: ancla al presionar y
  // delta vivo contra esa ancla. Umbral de 8 px para no confundir toque con
  // arrastre.
  int maxS = noteMaxScroll();
  if(T.pressed){ noteDragY0 = T.y; noteDragS0 = noteScroll; noteDragging = false; }
  if(T.down && maxS > 0){
    int dy = noteDragY0 - T.y;
    if(!noteDragging && abs(dy) > 8) noteDragging = true;
    if(noteDragging){
      int ns = noteDragS0 + dy;
      if(ns < 0) ns = 0; if(ns > maxS) ns = maxS;
      if(ns != noteScroll){ noteScroll = ns; noteRenderList(); }
      noteLongFired = true;
      return;
    }
  }
  if(T.released && noteDragging){ noteDragging = false; return; }

  // --- Long-press sobre una tarjeta -> menu contextual ---
  if(T.down && !noteLongFired && (millis() - T.downMs) > 550
     && abs(T.x - T.startX) < 14 && abs(T.y - T.startY) < 14){
    for(int i = 0; i < noteListN; i++){
      int x, y, w, h; noteCardRect(i, x, y, w, h);
      if(T.startX >= x && T.startX <= x + w && T.startY >= y && T.startY <= y + h){
        noteLongFired = true; noteSelIdx = i;
        fkMenuOpen(T.x, T.y - 40);
        return;
      }
    }
    noteLongFired = true;
  }
  if(!T.down) noteLongFired = false;

  if(!T.tap) return;

  // --- Tres puntos de la cabecera ---
  if(T.x > SCR_W - 60 && T.y < 80){ noteSelIdx = -1; fkMenuOpen(SCR_W - 40, 60); return; }
  // --- Volver al escritorio ---
  if(T.x < 60 && T.y < 60){ appClose(); return; }

  // --- Barra del modo seleccion ---
  if(noteMulti){
    int by = SCR_H - 128;
    if(T.y >= by && T.y <= by + 60){
      if(T.x > SCR_W - 100){ noteMulti = false; noteMask = 0; noteRenderList(); return; }
      if(T.x > SCR_W - 230){
        for(int i = 0; i < noteListN; i++) if(noteMask & (1UL << i)){
          char p[FLEXFS_PATH_MAX]; notePathOf(i, p, sizeof(p));
          flexFsTrash(p);                        // a la papelera, de verdad, uno a uno
        }
        noteMulti = false; noteMask = 0; noteReload(); noteRenderList(); return;
      }
    }
  } else {
    int fx, fy, fr; noteFabRect(fx, fy, fr);
    long ddx = T.x - fx, ddy = T.y - fy;
    if(ddx * ddx + ddy * ddy <= (long)fr * fr){ noteNew(); return; }
  }

  // --- Tarjetas ---
  for(int i = 0; i < noteListN; i++){
    int x, y, w, h; noteCardRect(i, x, y, w, h);
    if(T.x >= x && T.x <= x + w && T.y >= y && T.y <= y + h){
      if(noteMulti){ noteMask ^= (1UL << i); noteRenderList(); }
      else noteOpen(i);
      return;
    }
  }
}

// ---- Puntos de entrada de la app (los que ve APP_REG) ----
static void noteEnter(){
  noteBufInit();
  if(!flexFsReady()){ fkNoFsScreen("Notas"); return; }
  noteView = 0; noteMulti = false; noteMask = 0; noteSelIdx = -1;
  fkMenuOn = false; fkNameOn = false; fkAskOn = false; fkTrashOn = false;
  noteReload();
  noteRenderList();
}

static void noteTick(){
  if(!flexFsReady()){ if(T.tap && T.x < 60 && T.y < 60) appClose(); return; }
  if(noteView == 0){ noteListTick(); return; }

  // Editor: la esquina superior izquierda vuelve a la lista GUARDANDO.
  if(T.tap && T.x < 60 && T.y < 44 && !clipPanelOn && !noteMenu && !kbPopup && !kbMoreOn){
    noteBackToList(); return;
  }
  int lenBefore = strlen(noteBuffer);
  noteEditorTick();
  // Autoguardado: 2 s despues de la ultima modificacion real del texto.
  if((int)strlen(noteBuffer) != lenBefore) noteDirtyMs = millis();
  if(noteDirtyMs && millis() - noteDirtyMs > NOTE_AUTOSAVE_MS) noteSave();
}

// #############################################################
// ##  FASE E · AJUSTES DEL TECLADO  (pantalla propia, navegable)
// ##  ------------------------------------------------------
// ##  Se llega desde Ajustes -> Personalizacion -> Teclado y
// ##  desde el engranaje de la barra de la Fase C.
// ##
// ##  POR QUE UNA PANTALLA PROPIA Y NO UNA 13a CATEGORIA EN LA
// ##  BARRA LATERAL DE AJUSTES: esa barra tiene 12 tarjetas de
// ##  52 px desde y=100, o sea que termina en y=724. Una 13a
// ##  caeria encima de la barra de navegacion (y=748). Meterla
// ##  ahi obligaba a rehacer el layout de Ajustes entero, que no
// ##  es lo que se pidio. Las tarjetas, colores y el patron de
// ##  filas son los mismos de setRowCard, asi que se ve como una
// ##  seccion mas de Ajustes.
// ##
// ##  LO QUE NO SE IMPLEMENTA Y POR QUE (se muestra, atenuado,
// ##  en vez de esconderlo):
// ##   · Entrada de voz -> necesita reconocimiento en la nube.
// ##   · Guardar capturas en portapapeles -> este FlexOS no tiene
// ##     funcion de captura de pantalla de usuario.
// ##   · Respuesta hapticas -> la placa no lleva motor vibrador.
// ##   · Dividir teclado -> con 480 px de ancho no caben dos
// ##     mitades usables; en su lugar esta el selector de tamano.
// #############################################################
#define KBS_MAIN   0
#define KBS_DESIGN 1
#define KBS_TOUCH  2
#define KBS_SYMS   3
#define KBS_SHORT  4
#define KBS_SCEDIT 5
#define KBS_ABOUT  6
#define KBS_LANG   7
#define KBS_ROW_MAX 20
static int  kbsPage = KBS_MAIN;
static int  kbsScroll = 0, kbsContentH = 0;
static int  kbsRowY0[KBS_ROW_MAX], kbsRowY1[KBS_ROW_MAX], kbsRowN = 0;
static int  kbsRet = 0;               // 0 = volver a Notas, 1 = volver a Ajustes
static int  kbsScSel = 0;             // atajo que se esta editando
static int  kbsScField = 0;           // 0 = abreviacion, 1 = expansion
static char kbsScA[KB_SC_ABR], kbsScE[KB_SC_EXP];
static int  kbsDragY0 = 0, kbsDragS0 = 0;   // arrastre de la lista
static bool kbsDragging = false;
#define KBS_TOP    100
#define KBS_BOT    (SCR_H - 24)

static uint16_t kbsBg(){    return PAGE_BG; }
static uint16_t kbsCard(){  return uiGlass ? SET_CARD_GLASS : SET_CARD_BG; }
// Fila a todo el ancho, con el mismo lenguaje visual que setRowCard (tarjeta,
// titulo, valor y chevron). "on" atenua el texto cuando la fila esta apagada
// por hardware (voz, capturas, haptica).
static int kbsRow(int y, const char* title, const char* val, bool chevron, bool enabled){
  int rh = 62, x = 12, w = SCR_W - 24;
  if(kbsRowN < KBS_ROW_MAX){ kbsRowY0[kbsRowN] = y; kbsRowY1[kbsRowN] = y + rh; kbsRowN++; }
  // El vidrio ya NO se apaga durante el arrastre (drawGlassCardFlat lo resuelve
  // una vez y lo vuelca por filas): el material se mantiene mientras se hace
  // scroll, tal cual esta configurado.
  if(uiGlass) drawGlassCardFlat(x, y, w, rh - 8, 12, kbsCard(), kbsBg());
  else fillRoundRect(x, y, w, rh - 8, 12, kbsCard());
  uint16_t tc = enabled ? SET_TXT_HI : SET_TXT_MUTE;
  uint16_t vc = enabled ? SET_TXT_LO : SET_TXT_MUTE;
  drawTextClip(x + 16, y + 8, title, 2, tc, x + w - 28);
  if(val) drawTextClip(x + 16, y + 32, val, 1, vc, x + w - 28);
  if(chevron && enabled){
    int chx = x + w - 18, chy = y + (rh - 8) / 2;
    strokeSegAA(chx - 3, chy - 6, chx + 3, chy, 2.0f, SET_CHEV);
    strokeSegAA(chx + 3, chy, chx - 3, chy + 6, 2.0f, SET_CHEV);
  }
  return y + rh;
}
static int kbsSection(int y, const char* t){
  drawText(16, y, t, 2, SET_TXT_HI);
  return y + 30;
}
static const char* kbsSizeName(){ return gKbSize == KB_SIZE_COMPACT ? "Compacto" : gKbSize == KB_SIZE_BIG ? "Grande" : "Normal"; }
static const char* kbsStyleName(){ return gKbStyle == 1 ? "Cuadrada" : gKbStyle == 2 ? "Contorno" : "Redondeada"; }
static const char* kbsFontName(){ return gKbFontSc == 0 ? "Peque\xC3\xB1" "a" : gKbFontSc == 2 ? "Grande" : "Normal"; }
static const char* kbsFxName(){ return gKbFxMs == 60 ? "Corta" : gKbFxMs == 160 ? "Larga" : "Normal"; }
static const char* kbsOnOff(bool b){ return b ? "Activado" : "Desactivado"; }

// ---- Teclado incrustado del editor de atajos ----
// Es el MISMO teclado (misma geometria, mismos colores, mismo kbCellAt), solo
// que escribe en el campo enfocado en vez de en una nota.
static void kbsEditorKb(){
  kbPaintPanel(KB_Y - 4, uiGlass ? kbColPanel() : rgb565(18,20,28));
  int fs = kbFontSize();
  for(int r = 0; r < KB_ROWS; r++) for(int c = 0; c < KB_COLS; c++){
    int x = KB_X + c * (KB_KW + KB_GAP), y = KB_Y + r * (KB_KH + KB_GAP);
    const char* k = mapaActivo[r][c];
    char u[6];
    if(kbShift && k[1] == 0 && k[0] >= 'a' && k[0] <= 'z'){ u[0] = (char)(k[0] - 32); u[1] = 0; k = u; }
    kbPaintKey(x, y, KB_KW, KB_KH, k, fs, kbColKey(), kbColKeyTxt(), false);
  }
  int fy = kbFuncY();
  const char* lb[KB_FKEYS] = { "shift", kbLayerLabel(), kbLangEs ? "ES" : "EN", "espacio", "<-", "OK" };
  for(int i = 0; i < KB_FKEYS; i++) kbFKey(kbFKeyX(i), fy, kbFKeyW(i), lb[i], (i == 0) && kbShift);
}
static void kbsAppendField(const char* s){
  char* dst = kbsScField == 0 ? kbsScA : kbsScE;
  int cap = kbsScField == 0 ? KB_SC_ABR : KB_SC_EXP;
  int L = strlen(dst), sl = strlen(s);
  if(L + sl >= cap - 1) return;
  memcpy(dst + L, s, sl); dst[L + sl] = 0;
}
static void kbsBackField(){
  char* dst = kbsScField == 0 ? kbsScA : kbsScE;
  int L = strlen(dst);
  if(L <= 0) return;
  int q = L - 1; while(q > 0 && (dst[q] & 0xC0) == 0x80) q--;
  dst[q] = 0;
}

// ---- Contenido de cada pagina ----
static void kbsContent(){
  kbsRowN = 0;
  int y = KBS_TOP - kbsScroll;
  char v[64];
  if(kbsPage == KBS_MAIN){
    y = kbsRow(y, "Idiomas y tipos", kbLangEs ? "Espa\xC3\xB1ol (ES) - activo" : "English (EN) - activo", true, true);
    y = kbsRow(y, "Texto predictivo", kbsOnOff(gKbPredict), false, true);
    y = kbsRow(y, "Revisi\xC3\xB3n ortogr\xC3\xA1" "fica b\xC3\xA1sica", kbsOnOff(gKbSpell), false, true);
    y = kbsRow(y, "Sugerir emojis", kbsOnOff(gKbEmojiSug), false, true);
    y = kbsRow(y, "Atajos de texto", "Abreviaci\xC3\xB3n -> expansi\xC3\xB3n", true, true);
    y += 6; y = kbsSection(y, "Teclado");
    y = kbsRow(y, "Barra de herramientas del teclado", kbsOnOff(gKbToolbar), false, true);
    y = kbsRow(y, "Teclado de contraste alto", kbsOnOff(gKbHiCon), false, true);
    // Diagnostico honesto al lado del interruptor: cuantos dedos a la vez ha
    // llegado a reportar el GT911 desde que arranco la placa. Si aqui pone "1
    // dedo" por mucho que se teclee con dos, el problema NO es el firmware: es
    // que ese panel solo esta dando un punto. Sin PC no hay otra forma de saberlo.
    if(gKbFastType) snprintf(v, sizeof(v), "Activado - el panel ha dado %d dedo%s a la vez",
                             kbMtMaxPts, kbMtMaxPts == 1 ? "" : "s");
    else snprintf(v, sizeof(v), "Desactivado - se escribe al soltar");
    y = kbsRow(y, "Escritura r\xC3\xA1pida (multitoque)", v, false, true);
    y = kbsRow(y, "Dise\xC3\xB1o y tama\xC3\xB1o", KB_SIZE_CONFIG_ON ? kbsSizeName() : kbsStyleName(), true, true);
    y = kbsRow(y, "Deslizar, tocar y respuesta t\xC3\xA1" "ctil", kbsFxName(), true, true);
    y += 6; y = kbsSection(y, "No disponible en este hardware");
    y = kbsRow(y, "Entrada de voz", "Necesita reconocimiento en la nube", false, false);
    y = kbsRow(y, "Guardar capturas en portapapeles", "FlexOS no tiene captura de pantalla", false, false);
    y += 6; y = kbsSection(y, "Otros");
    y = kbsRow(y, "Restablecer ajustes del teclado", "Vuelve a los valores por defecto", false, true);
    y = kbsRow(y, "Sobre teclado", "Teclado FlexOS", true, true);
  } else if(kbsPage == KBS_LANG){
    y = kbsRow(y, "Espa\xC3\xB1ol (ES)", kbLangEs ? "Activo" : "Toca para activar", false, true);
    y = kbsRow(y, "English (EN)", kbLangEs ? "Toca para activar" : "Activo", false, true);
    y = kbsRow(y, "Capa num\xC3\xA9rica y de s\xC3\xADmbolos", "Siempre disponible (?123)", false, false);
    y = kbsRow(y, "Emoticonos de texto", "Capa emoji, glifos de la fuente", false, false);
    y += 8;
    drawTextClip(16, y, "El teclado soporta estos dos idiomas: son los que", 1, SET_TXT_MUTE, SCR_W - 16); y += 18;
    drawTextClip(16, y, "tienen mapa de teclas y diccionario propios.", 1, SET_TXT_MUTE, SCR_W - 16); y += 22;
  } else if(kbsPage == KBS_DESIGN){
#if KB_SIZE_CONFIG_ON
    // Con KB_SIZE_CONFIG_ON a 0 esta fila NI SE DIBUJA: el teclado usa los
    // valores fijos de siempre y ofrecer un selector que no hace nada seria
    // mentir. Las filas de abajo se recolocan solas (ver kbsRowAction).
    y = kbsRow(y, "Tama\xC3\xB1o de teclado", kbsSizeName(), false, true);
#endif
    snprintf(v, sizeof(v), "%d%% de opacidad", gKbOpacity);
    y = kbsRow(y, "Tama\xC3\xB1o y transparencia", v, false, true);
    y = kbsRow(y, "Dise\xC3\xB1o", kbsStyleName(), false, true);
    y = kbsRow(y, "Tama\xC3\xB1o de fuente", kbsFontName(), false, true);
    snprintf(v, sizeof(v), "%s %s %s %s", kbSymAt(0), kbSymAt(1), kbSymAt(2), kbSymAt(3));
    y = kbsRow(y, "S\xC3\xADmbolos personalizados", v, true, true);
    y += 8;
    drawTextClip(16, y, "Sin \"dividir teclado\": con 480 px de ancho no caben", 1, SET_TXT_MUTE, SCR_W - 16); y += 18;
    drawTextClip(16, y, "dos mitades usables. En su lugar, los tres tamanos.", 1, SET_TXT_MUTE, SCR_W - 16); y += 22;
  } else if(kbsPage == KBS_TOUCH){
    y = kbsRow(y, "Animaci\xC3\xB3n de tecla presionada", kbsFxName(), false, true);
    snprintf(v, sizeof(v), "%d ms", gKbLpMs);
    y = kbsRow(y, "Pulsaci\xC3\xB3n larga (acentos)", v, false, true);
    y = kbsRow(y, "Respuesta h\xC3\xA1ptica", "La placa no tiene motor vibrador", false, false);
    y += 8;
    drawTextClip(16, y, "Todo el retorno de esta pantalla es VISUAL.", 1, SET_TXT_MUTE, SCR_W - 16); y += 22;
  } else if(kbsPage == KBS_SYMS){
    for(int i = 0; i < KB_SYMS; i++){
      char t[24]; snprintf(t, sizeof(t), "S\xC3\xADmbolo %d", i + 1);
      y = kbsRow(y, t, kbSymAt(i), false, true);
    }
    y += 8;
    drawTextClip(16, y, "Toca una fila para cambiar el simbolo. Salen en la", 1, SET_TXT_MUTE, SCR_W - 16); y += 18;
    drawTextClip(16, y, "franja de arriba al entrar en la capa ?123.", 1, SET_TXT_MUTE, SCR_W - 16); y += 22;
  } else if(kbsPage == KBS_SHORT){
    for(int i = 0; i < KB_SC_MAX; i++){
      if(gKbScAbr[i][0] && gKbScExp[i][0]) snprintf(v, sizeof(v), "%s -> %s", gKbScAbr[i], gKbScExp[i]);
      else snprintf(v, sizeof(v), "Vacio - toca para crear");
      char t[24]; snprintf(t, sizeof(t), "Atajo %d", i + 1);
      y = kbsRow(y, t, v, true, true);
    }
    y += 8;
    drawTextClip(16, y, "Al escribir la abreviacion, el chip de sugerencia", 1, SET_TXT_MUTE, SCR_W - 16); y += 18;
    drawTextClip(16, y, "ofrece la expansion completa.", 1, SET_TXT_MUTE, SCR_W - 16); y += 22;
  } else if(kbsPage == KBS_ABOUT){
    y = kbsRow(y, "Teclado FlexOS", "Version 1.0", false, false);
    snprintf(v, sizeof(v), "%d palabras ES / %d EN", KB_DICT_ES_N, KB_DICT_EN_N);
    y = kbsRow(y, "Diccionario local", v, false, false);
    y = kbsRow(y, "Idiomas", "Espa\xC3\xB1ol, English", false, false);
    y += 10;
    drawTextClip(16, y, "El autocompletado es una LISTA LOCAL FIJA escrita", 1, SET_TXT_MUTE, SCR_W - 16); y += 18;
    drawTextClip(16, y, "en el propio firmware. No es un modelo de IA: no", 1, SET_TXT_MUTE, SCR_W - 16); y += 18;
    drawTextClip(16, y, "aprende, no entiende el contexto y no predice la", 1, SET_TXT_MUTE, SCR_W - 16); y += 18;
    drawTextClip(16, y, "palabra siguiente. Solo busca por prefijo.", 1, SET_TXT_MUTE, SCR_W - 16); y += 24;
    drawTextClip(16, y, "La revision ortografica compara contra esa misma", 1, SET_TXT_MUTE, SCR_W - 16); y += 18;
    drawTextClip(16, y, "lista: subraya lo que no encuentra, nada mas.", 1, SET_TXT_MUTE, SCR_W - 16); y += 22;
  } else if(kbsPage == KBS_SCEDIT){
    // Dos campos + teclado real debajo. El campo enfocado lleva borde azul.
    for(int f = 0; f < 2; f++){
      int fy = KBS_TOP + f * 76;
      const char* lbl = f == 0 ? "Abreviaci\xC3\xB3n (lo que escribes)" : "Expansi\xC3\xB3n (lo que aparece)";
      drawText(16, fy, lbl, 1, SET_TXT_LO);
      fillRoundRect(12, fy + 18, SCR_W - 24, 42, 10, uiGlass ? SET_CARD_GLASS : SET_CARD_BG);
      if(kbsScField == f) drawRoundRect(12, fy + 18, SCR_W - 24, 42, 10, rgb565(70,130,240));
      drawTextClip(24, fy + 30, f == 0 ? kbsScA : kbsScE, 2, SET_TXT_HI, SCR_W - 30);
    }
    { int by = KBS_TOP + 160;
      fillRoundRect(12, by, (SCR_W - 36) / 2, 46, 12, rgb565(60,110,235));
      drawTextC(12 + (SCR_W - 36) / 4, by + 14, "Guardar", 2, rgb565(255,255,255));
      fillRoundRect(24 + (SCR_W - 36) / 2, by, (SCR_W - 36) / 2, 46, 12, rgb565(150,60,60));
      drawTextC(24 + (SCR_W - 36) / 2 + (SCR_W - 36) / 4, by + 14, "Borrar", 2, rgb565(255,255,255)); }
    kbsEditorKb();
  }
  kbsContentH = (y + kbsScroll) - KBS_TOP + 20;
}
static const char* kbsTitle(){
  switch(kbsPage){
    case KBS_DESIGN: return "Dise\xC3\xB1o y tama\xC3\xB1o";
    case KBS_TOUCH:  return "Deslizar y tocar";
    case KBS_SYMS:   return "S\xC3\xADmbolos";
    case KBS_SHORT:  return "Atajos de texto";
    case KBS_SCEDIT: return "Editar atajo";
    case KBS_ABOUT:  return "Sobre teclado";
    case KBS_LANG:   return "Idiomas y tipos";
    default:         return "Teclado";
  }
}
// Pinta la pantalla completa en el buffer que toque (fb directo, o bbuf cuando
// se va a animar la transicion).
static void kbsPaint(){
  fillRect(0, 0, SCR_W, SCR_H, kbsBg());
  strokeSegAA(30, 40, 18, 32, 2.4f, SET_TXT_HI);            // flecha de volver
  strokeSegAA(18, 32, 30, 24, 2.4f, SET_TXT_HI);
  drawText(52, 20, kbsTitle(), 3, SET_TXT_HI);
  int c0 = gClipY0, c1 = gClipY1;
  gClipY0 = KBS_TOP - 8; gClipY1 = KBS_BOT;
  kbsContent();
  gClipY0 = c0; gClipY1 = c1;
}
// Se compone en bbuf y se publica de una vez: el cuadro llega entero al panel,
// nunca a medias (ver el candado de composicion en flxPresenter).
static void kbsRender(){
  setBuf(bbuf);
  kbsPaint();
  present(0, SCR_H - 1);
  setBuf(fb);
}
// Repintado SOLO de la banda de la lista. Es lo que se usa cuadro a cuadro
// mientras el dedo arrastra: ni cabecera ni fondo completo, solo la banda. El
// vidrio de las tarjetas SI se mantiene (drawGlassCardFlat lo resuelve una vez
// y lo vuelca por filas), asi que el scroll va pegado al dedo sin perder el
// material ni parpadear.
static void kbsRenderList(){
  setBuf(bbuf);
  fillRect(0, KBS_TOP - 10, SCR_W, KBS_BOT - (KBS_TOP - 10), kbsBg());
  int c0 = gClipY0, c1 = gClipY1;
  gClipY0 = KBS_TOP - 10; gClipY1 = KBS_BOT;
  kbsContent();
  gClipY0 = c0; gClipY1 = c1;
  present(KBS_TOP - 10, KBS_BOT);
  setBuf(fb);
}
// FASE G - transicion entre pantallas del teclado. NO se inventa un estilo
// nuevo: se reusa gAnimStyle (0 zoom, 1 fundido, 2 deslizar), el mismo ajuste
// que ya gobierna la apertura de apps. Se compone en bbuf y se vuelca; nunca
// se dibuja a medias en pantalla.
static void kbsRenderAnim(){
  if(!KB_ANIM_POLISH_ON || !bbuf){ kbsRender(); return; }
  setBuf(bbuf);
  kbsPaint();
  setBuf(fb);
  const int steps = 6;
  for(int s = 1; s <= steps; s++){
    float p = (float)s / steps;
    if(gAnimStyle == 2){                                   // deslizar desde la derecha
      int off = (int)((1.0f - p) * SCR_W);
      for(int y = 0; y < SCR_H; y++){
        uint16_t* d = fb + (size_t)y * SCR_W;
        const uint16_t* sp = bbuf + (size_t)y * SCR_W;
        for(int x = 0; x < off; x++) d[x] = kbsBg();
        memcpy(d + off, sp, (size_t)(SCR_W - off) * 2);
      }
    } else if(gAnimStyle == 1){                            // fundido
      uint8_t a = (uint8_t)(p * 255);
      for(int y = 0; y < SCR_H; y++){
        uint16_t* d = fb + (size_t)y * SCR_W;
        const uint16_t* sp = bbuf + (size_t)y * SCR_W;
        for(int x = 0; x < SCR_W; x++) d[x] = mix565(d[x], sp[x], a);
      }
    } else {                                               // zoom (del 88% al 100%)
      float k = 0.88f + 0.12f * p;
      int cw = (int)(SCR_W * k), ch = (int)(SCR_H * k);
      int ox = (SCR_W - cw) / 2, oy = (SCR_H - ch) / 2;
      for(int y = 0; y < SCR_H; y++){
        uint16_t* d = fb + (size_t)y * SCR_W;
        int sy = (y - oy) * SCR_H / (ch > 0 ? ch : 1);
        if(y < oy || y >= oy + ch || sy < 0 || sy >= SCR_H){ for(int x = 0; x < SCR_W; x++) d[x] = kbsBg(); continue; }
        const uint16_t* sp = bbuf + (size_t)sy * SCR_W;
        for(int x = 0; x < SCR_W; x++){
          int sx = (x - ox) * SCR_W / (cw > 0 ? cw : 1);
          d[x] = (x < ox || x >= ox + cw || sx < 0 || sx >= SCR_W) ? kbsBg() : sp[sx];
        }
      }
    }
    flxFlushAll();
    delay(12);
  }
  // Fotograma final EXACTO (las interpolaciones dejan redondeos): se copia el
  // buffer bueno tal cual, para que la pantalla que queda no sea la aproximada.
  fbCopyBand(bbuf, 0, SCR_H - 1);
  flxFlushAll();
}
static void kbsGo(int page){
  kbsPage = page; kbsScroll = 0;
  kbsDragging = false;                         // cambiar de pagina cancela cualquier arrastre vivo
  kbsRenderAnim();
}
static void kbsEnter(){
  if(!KB_SETTINGS_ON) return;
  kbsRet = (gState == ST_APP && gAppId == 12) ? 1 : 0;   // 12 = app Ajustes
  kbExtrasOn = false;                                    // aqui el teclado no lleva barra ni chips
  kbApplySize(); kbMtSurfaceReset();
  gState = ST_KBSET; kbsPage = KBS_MAIN; kbsScroll = 0;
  kbsDragging = false;
  kbsRender();
}
static void kbsExit(){
  if(kbsRet == 1){ gState = ST_APP; settingsRender(); return; }
  gState = ST_APP; kbExtrasOn = true; kbApplySize(); kbChipsBuild(); noteRenderAll();
}
static void kbsResetDefaults(){
  gKbSize = KB_SIZE_NORMAL; gKbFastType = true; gKbToolbar = true; gKbPredict = true;
  gKbSpell = false; gKbEmojiSug = false; gKbHiCon = false; gKbOpacity = 100;
  gKbStyle = 0; gKbFontSc = 1; gKbLpMs = 500; gKbFxMs = 100;
  for(int i = 0; i < KB_SYMS; i++) gKbSym[i] = i;
  kbShortcutsDefaults();
  kbApplySize(); kbPrefsSave();
}
// Accion al tocar la fila idx de la pagina actual.
static void kbsRowAction(int idx){
  if(kbsPage == KBS_MAIN){
    switch(idx){
      case 0: kbsGo(KBS_LANG); return;
      case 1: gKbPredict = !gKbPredict; break;
      case 2: gKbSpell = !gKbSpell; break;
      case 3: gKbEmojiSug = !gKbEmojiSug; break;
      case 4: kbsGo(KBS_SHORT); return;
      case 5: gKbToolbar = !gKbToolbar; break;
      case 6: gKbHiCon = !gKbHiCon; break;
      case 7: gKbFastType = !gKbFastType; kbMtSurfaceReset(); break;
      case 8: kbsGo(KBS_DESIGN); return;
      case 9: kbsGo(KBS_TOUCH); return;
      case 10: case 11: return;                       // filas informativas (hardware)
      case 12: kbsResetDefaults(); kbsRender(); return;
      case 13: kbsGo(KBS_ABOUT); return;
      default: return;
    }
    kbPrefsSave(); kbsRender(); return;
  }
  if(kbsPage == KBS_LANG){
    if(idx == 0 || idx == 1){
      kbLangEs = (idx == 0);
      if(mapaActivo == LAYOUT_ES || mapaActivo == LAYOUT_EN) mapaActivo = kbLangEs ? LAYOUT_ES : LAYOUT_EN;
      kbsRender();
    }
    return;
  }
  if(kbsPage == KBS_DESIGN){
    int k = idx + (KB_SIZE_CONFIG_ON ? 0 : 1);      // sin fila de tamano, todo sube un puesto
    if(k == 0){ gKbSize = (gKbSize + 1) % 3; kbApplySize(); }
    else if(k == 1){ gKbOpacity -= 15; if(gKbOpacity < 40) gKbOpacity = 100; }
    else if(k == 2){ gKbStyle = (gKbStyle + 1) % 3; }
    else if(k == 3){ gKbFontSc = (gKbFontSc + 1) % 3; }
    else if(k == 4){ kbsGo(KBS_SYMS); return; }
    else return;
    kbPrefsSave(); kbsRender(); return;
  }
  if(kbsPage == KBS_TOUCH){
    if(idx == 0){ gKbFxMs = (gKbFxMs == 60) ? 100 : (gKbFxMs == 100) ? 160 : 60; }
    else if(idx == 1){ gKbLpMs = (gKbLpMs == 350) ? 500 : (gKbLpMs == 500) ? 700 : 350; }
    else return;
    kbPrefsSave(); kbsRender(); return;
  }
  if(kbsPage == KBS_SYMS){
    if(idx >= 0 && idx < KB_SYMS){
      gKbSym[idx] = (gKbSym[idx] + 1) % KB_SYM_POOL_N;
      kbPrefsSave(); kbsRender();
    }
    return;
  }
  if(kbsPage == KBS_SHORT){
    if(idx >= 0 && idx < KB_SC_MAX){
      kbsScSel = idx; kbsScField = 0;
      snprintf(kbsScA, sizeof(kbsScA), "%s", gKbScAbr[idx]);
      snprintf(kbsScE, sizeof(kbsScE), "%s", gKbScExp[idx]);
      kbsGo(KBS_SCEDIT);
    }
    return;
  }
}
static void kbsTick(){
  if(!KB_SETTINGS_ON){ gState = ST_APP; return; }
  if(T.tap && T.x < 48 && T.y < 52){                      // volver
    if(kbsPage == KBS_MAIN) kbsExit();
    else if(kbsPage == KBS_SCEDIT) kbsGo(KBS_SHORT);
    else if(kbsPage == KBS_SYMS) kbsGo(KBS_DESIGN);
    else kbsGo(KBS_MAIN);
    return;
  }
  if(kbsPage == KBS_SCEDIT){
    if(!T.tap) return;
    for(int f = 0; f < 2; f++){
      int fy = KBS_TOP + f * 76;
      if(T.y >= fy + 18 && T.y <= fy + 60){ kbsScField = f; kbsRender(); return; }
    }
    { int by = KBS_TOP + 160, hw = (SCR_W - 36) / 2;
      if(T.y >= by && T.y <= by + 46){
        if(T.x >= 12 && T.x <= 12 + hw){                                  // Guardar
          if(kbsScA[0] && kbsScE[0]){ snprintf(gKbScAbr[kbsScSel], KB_SC_ABR, "%s", kbsScA); snprintf(gKbScExp[kbsScSel], KB_SC_EXP, "%s", kbsScE); }
          kbPrefsSave(); kbsGo(KBS_SHORT); return;
        }
        if(T.x >= 24 + hw){                                               // Borrar el atajo
          gKbScAbr[kbsScSel][0] = 0; gKbScExp[kbsScSel][0] = 0;
          kbPrefsSave(); kbsGo(KBS_SHORT); return;
        }
      } }
    int fi = kbFRowHit(T.x, T.y);
    if(fi >= 0){
      if(fi == 0) kbShift = !kbShift;
      else if(fi == 1) mapaActivo = (mapaActivo == LAYOUT_NUM) ? LAYOUT_EMOJI : (mapaActivo == LAYOUT_EMOJI) ? (kbLangEs ? LAYOUT_ES : LAYOUT_EN) : LAYOUT_NUM;
      else if(fi == 2){ kbLangEs = !kbLangEs; if(mapaActivo == LAYOUT_ES || mapaActivo == LAYOUT_EN) mapaActivo = kbLangEs ? LAYOUT_ES : LAYOUT_EN; }
      else if(fi == 3) kbsAppendField(" ");
      else if(fi == 4) kbsBackField();
      else { if(kbsScA[0] && kbsScE[0]){ snprintf(gKbScAbr[kbsScSel], KB_SC_ABR, "%s", kbsScA); snprintf(gKbScExp[kbsScSel], KB_SC_EXP, "%s", kbsScE); kbPrefsSave(); kbsGo(KBS_SHORT); return; } }
      kbsRender(); return;
    }
    int cell = kbCellAt(T.x, T.y);
    if(cell >= 0){
      const char* k = mapaActivo[cell / KB_COLS][cell % KB_COLS];
      if(kbShift && k[1] == 0 && k[0] >= 'a' && k[0] <= 'z'){ char u[2] = { (char)(k[0] - 32), 0 }; kbsAppendField(u); kbShift = false; }
      else kbsAppendField(k);
      kbsRender();
    }
    return;
  }
  // ARRASTRE REAL de la lista (antes eran saltos de 140 px al soltar). El
  // contenido sigue al dedo cuadro a cuadro, repintando solo su banda y con las
  // tarjetas planas mientras dura; al soltar, un repintado bueno con vidrio.
  int vp = KBS_BOT - KBS_TOP, maxS = kbsContentH - vp; if(maxS < 0) maxS = 0;
  if(T.pressed){ kbsDragY0 = T.y; kbsDragS0 = kbsScroll; kbsDragging = false; }
  if(T.down && maxS > 0){
    int dy = kbsDragY0 - T.y;
    if(!kbsDragging && abs(dy) > 6){ kbsDragging = true; }
    if(kbsDragging){
      int ns = kbsDragS0 + dy;
      if(ns < 0) ns = 0; if(ns > maxS) ns = maxS;
      if(ns != kbsScroll){ kbsScroll = ns; kbsRenderList(); }
      return;
    }
  }
  if(T.released && kbsDragging){ kbsDragging = false; kbsRender(); return; }
  if(T.tap && !kbsDragging && T.y >= KBS_TOP - 8 && T.y <= KBS_BOT){
    for(int i = 0; i < kbsRowN; i++) if(T.y >= kbsRowY0[i] && T.y < kbsRowY1[i]){ kbsRowAction(i); return; }
  }
}

// #############################################################
// ##  APPS SIMPLES: Almacenamiento, Educacion, Navegador,
// ##  Code IDE, Paint (funcional), Juegos
// #############################################################
// Barra etiqueta+valor+progreso, dimensionada al lienzo (la usan Almacenamiento
// y cualquier app que quiera una fila de medidor).
static int simpBar(int y, const char* label, const char* val, int pct, uint16_t col){
  int bxx, byy, bww, bhh; uiBox(bxx, byy, bww, bhh);
  int pad = uiPad();
  int bx = bxx + pad * 2, bw = bww - pad * 4;
  if(bw < 60){ bx = bxx + pad; bw = bww - 2 * pad; }
  int fs = uiFontFit(label, bw / 2, 2);
  drawText(bx, y, label, fs, rgb565(225,229,240));
  drawTextR(bx + bw, y, val, uiFontFit(val, bw / 2, 2), rgb565(180,188,205));
  int barH = bhh / 22; if(barH < 9) barH = 9; if(barH > 20) barH = 20;
  int byr = y + uiLineH(fs) + 6;
  fillRoundRect(bx, byr, bw, barH, barH / 2, rgb565(48,52,66));
  if(pct > 0) fillRoundRect(bx, byr, bw * pct / 100, barH, barH / 2, col);
  return byr + barH + uiPad();
}
// #############################################################
// ##  ALMACENAMIENTO  ·  todo lo que muestra sale de leer el
// ##  sistema de archivos y el SDK, en el momento de pintarlo
// ##  ------------------------------------------------------
// ##  De donde sale cada numero:
// ##    · "Almacenamiento interno" -> LittleFS.totalBytes() y
// ##      LittleFS.usedBytes() (via flexFsTotalBytes/UsedBytes).
// ##      Es el espacio de la PARTICION DE DATOS, que es el
// ##      unico que el usuario puede llenar; no el tamano del
// ##      chip de flash, que incluye el propio firmware y no se
// ##      puede usar para guardar nada.
// ##    · "PSRAM" -> heap_caps_get_total_size/free_size con
// ##      MALLOC_CAP_SPIRAM. En una placa sin PSRAM (Pro) el
// ##      total es 0 y la fila lo dice, no finge un porcentaje.
// ##    · Categorias -> suma recursiva de los ficheros de cada
// ##      carpeta real (flexFsCatSize).
// ##    · "Archivos grandes" -> recorrido completo del arbol
// ##      quedandose con los mayores (flexFsLargest).
// #############################################################
#define ALM_BIG_MAX 3

static FlexFsBig almBig[ALM_BIG_MAX];
static int       almBigN = 0;
static uint32_t  almCat[FLEXFS_CAT_N];
static int       almVerY0 = 0, almVerY1 = 0;      // zona pulsable real de "Ver..."

static const char* ALM_CAT_NAME[FLEXFS_CAT_N] = { "Documentos", "Sistema", "Aplicaciones", "Papelera" };
static const uint16_t ALM_CAT_COL[FLEXFS_CAT_N] = {
  rgb565(245,85,85), rgb565(250,215,95), rgb565(95,225,110), rgb565(60,205,240) };

static void filesEnter();                          // explorador (ST_FILES), mas abajo

// Relee TODO lo que se muestra. Se llama al entrar a la app y al volver
// del explorador: si el usuario borro algo alli, aqui se ve al instante.
static void almScan(){
  for(int i = 0; i < FLEXFS_CAT_N; i++) almCat[i] = flexFsCatSize(i);
  almBigN = flexFsLargest(almBig, ALM_BIG_MAX);
}

// Icono de carpeta (el mismo que usa el explorador).
static void almFolderIcon(int x, int y, int s){
  uint16_t body = rgb565(250,205,90), tab = rgb565(240,175,60);
  fillRoundRect(x, y + s / 5, s, s * 3 / 4, s / 8, tab);
  fillRoundRect(x, y + s / 5, s * 5 / 9, s / 4, s / 10, tab);
  fillRoundRect(x + s / 10, y + s / 3, s - s / 10, s * 3 / 5, s / 9, body);
}

static void almEnter(){
  setBuf(fb);
  int bx, by, bw, bh; uiBox(bx, by, bw, bh);
  fillRect(bx, by, bw, bh, WIN_BG);
  int pad = uiPad(), gap = uiGap();
  int y = by + pad;
  y = uiTitle(bx, y, bw, "Almacenamiento", rgb565(255,255,255), uiFontH(bh / 12));
  y += gap / 2;

  if(!flexFsReady()){
    drawTextC(bx + bw / 2, y + 40, "Sin almacenamiento", 3, rgb565(240,140,140));
    drawTextC(bx + bw / 2, y + 84, flexFsError(), 1, rgb565(170,178,196));
    drawTextC(bx + bw / 2, y + 112, "Elige un Partition Scheme con SPIFFS", 1, rgb565(140,148,168));
    almVerY0 = almVerY1 = 0;
    flxFlush(WIN_TOP, WIN_BOT);
    return;
  }
  almScan();

  // ---- Barra 1: particion de datos (lo unico que el usuario llena) ----
  uint32_t tot = flexFsTotalBytes(), usd = flexFsUsedBytes();
  int pctFs = tot ? (int)((uint64_t)usd * 100 / tot) : 0;
  char vt[48], su[24], st[24];
  flexFsFmtSize(usd, su, sizeof(su));
  flexFsFmtSize(tot, st, sizeof(st));
  snprintf(vt, sizeof(vt), "%s / %s  (%d%%)", su, st, pctFs);
  y = simpBar(y, "Almacenamiento interno", vt, pctFs, rgb565(90,160,240));

  // ---- Barra 2: PSRAM ----
  size_t pt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  size_t pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if(pt > 0){
    int pctPs = (int)(100 - (uint64_t)pf * 100 / pt);
    char vp[48];
    snprintf(vp, sizeof(vp), "%u / %u MB  (%d%%)",
             (unsigned)((pt - pf) / 1048576u), (unsigned)(pt / 1048576u), pctPs);
    y = simpBar(y, "PSRAM", vp, pctPs, rgb565(90,180,120));
  } else {
    // Sin PSRAM (ESP32 clasico): se dice, no se pinta una barra a 0 que
    // parezca una PSRAM vacia.
    y = simpBar(y, "PSRAM", "No disponible en esta placa", 0, rgb565(120,124,140));
  }

  // ---- Categorias: tamano REAL de cada conjunto de carpetas ----
  y += gap / 2;
  int rowH = uiLineH(2) + 12;
  for(int i = 0; i < FLEXFS_CAT_N; i++){
    if(y + rowH > by + bh - pad) break;
    fillCircle(bx + pad * 2 + 8, y + rowH / 2 - 2, 8, ALM_CAT_COL[i]);
    drawText(bx + pad * 2 + 26, y, ALM_CAT_NAME[i], 2, rgb565(228,232,242));
    char sz[24]; flexFsFmtSize(almCat[i], sz, sizeof(sz));
    drawTextR(bx + bw - pad * 2, y, sz, 2, rgb565(160,168,186));
    y += rowH;
  }

  // ---- Archivos grandes: los mayores del sistema, de verdad ----
  y += gap;
  if(y + 40 < by + bh - pad){
    drawText(bx + pad * 2, y, "Archivos grandes", 2, rgb565(240,242,248));
    y += uiLineH(2) + 6;
    if(almBigN == 0){
      drawText(bx + pad * 2, y, "No hay archivos guardados", 1, rgb565(140,148,168));
      y += 22;
    }
    for(int i = 0; i < almBigN; i++){
      if(y + 44 > by + bh - pad) break;
      almFolderIcon(bx + pad * 2, y, 34);
      const char* nm = strrchr(almBig[i].path, '/');
      nm = nm ? nm + 1 : almBig[i].path;
      drawTextClip(bx + pad * 2 + 46, y, nm, 2, rgb565(228,232,242), bx + bw - pad * 2);
      char sz[24], ln[64]; flexFsFmtSize(almBig[i].size, sz, sizeof(sz));
      snprintf(ln, sizeof(ln), "%s  ·  %s", almBig[i].path, sz);
      drawTextClip(bx + pad * 2 + 46, y + 22, ln, 1, rgb565(150,158,178), bx + bw - pad);
      y += 46;
    }
  }

  // ---- Fila "Todos los archivos ... Ver..." -> explorador REAL ----
  y += gap / 2;
  if(y + 46 <= by + bh - pad / 2){
    fillRoundRect(bx + pad, y, bw - 2 * pad, 44, 12, rgb565(34,38,50));
    drawText(bx + pad * 2, y + 12, "Todos los archivos", 2, rgb565(228,232,242));
    drawTextR(bx + bw - pad * 2, y + 12, "Ver...", 2, rgb565(120,170,250));
    almVerY0 = y; almVerY1 = y + 44;
  } else {
    almVerY0 = almVerY1 = 0;
  }
  flxFlush(WIN_TOP, WIN_BOT);
}

static void almTick(){
  if(!T.tap) return;
  if(almVerY1 > almVerY0 && T.y >= almVerY0 && T.y <= almVerY1) filesEnter();
}

// #############################################################
// ##  EXPLORADOR DE ARCHIVOS  (ST_FILES)
// ##  ------------------------------------------------------
// ##  Lista lo que hay. El numero de elementos de cada carpeta
// ##  se obtiene ABRIENDO la carpeta y contando (flexFsCount
// ##  dentro de flexFsList), no de una tabla fija: por eso un
// ##  "6 elementos" baja a 5 en cuanto se borra uno.
// ##
// ##  El menu contextual es el mismo kit que usan Notas y Paint,
// ##  asi que las cuatro acciones se comportan igual en las tres
// ##  pantallas y operan sobre el fichero real.
// #############################################################
#define FILES_MAX     24
#define FILES_TOP    128
#define FILES_RH      70

static FlexFsEntry filesList[FILES_MAX];
static int         filesN = 0;
static char        filesDir[FLEXFS_PATH_MAX] = "/";
static int         filesSelIdx = -1;
static int         filesScroll = 0;
static int         filesDragY0 = 0, filesDragS0 = 0;
static bool        filesDragging = false, filesLongFired = false;
static bool        filesMulti = false;
static uint32_t    filesMask = 0;

static void filesRender();

static void filesReload(){
  filesN = flexFsList(filesDir, filesList, FILES_MAX);
  if(filesSelIdx >= filesN) filesSelIdx = -1;
  int maxRows = filesN;
  if(filesScroll > maxRows * FILES_RH) filesScroll = 0;
}

static void filesPathOf(int i, char* out, size_t n){
  if(!strcmp(filesDir, "/")) snprintf(out, n, "/%s", filesList[i].name);
  else                       snprintf(out, n, "%s/%s", filesDir, filesList[i].name);
}

// Fila 0 = ".." cuando no estamos en la raiz. Se cuenta en el layout para
// que el indice de la lista y la fila dibujada no puedan desalinearse.
static bool filesHasUp(){ return strcmp(filesDir, "/") != 0; }
static int  filesRowY(int row){ return FILES_TOP + row * FILES_RH - filesScroll; }
static int  filesMaxScroll(){
  int rows = filesN + (filesHasUp() ? 1 : 0);
  int need = FILES_TOP + rows * FILES_RH + 30;
  int m = need - (SCR_H - 80);
  return m > 0 ? m : 0;
}

static void filesRender(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(214,214,214));
  drawText(16, 22, "Archivos:", 5, rgb565(16,18,24));
  strokeSegAA(30, 26, 18, 18, 2.4f, rgb565(16,18,24));
  strokeSegAA(18, 18, 30, 10, 2.4f, rgb565(16,18,24));
  for(int i = 0; i < 3; i++) fillCircle(SCR_W - 26, 32 + i * 15, 5, rgb565(16,18,24));
  drawTextClip(16, 84, filesDir, 2, rgb565(60,64,74), SCR_W - 60);

  int row = 0;
  if(filesHasUp()){
    int y = filesRowY(row++);
    if(y > 40 && y < SCR_H - 60){
      fillRoundRect(12, y, SCR_W - 24, FILES_RH - 8, 12, rgb565(178,178,178));
      almFolderIcon(28, y + 12, 38);
      drawText(84, y + 18, "..", 3, rgb565(16,18,24));
      drawTextR(SCR_W - 28, y + 22, "subir", 1, rgb565(70,74,84));
    }
  }
  for(int i = 0; i < filesN; i++){
    int y = filesRowY(row++);
    if(y + FILES_RH < 40 || y > SCR_H - 50) continue;
    bool sel = filesMulti && (filesMask & (1UL << i));
    fillRoundRect(12, y, SCR_W - 24, FILES_RH - 8, 12, sel ? rgb565(150,180,235) : rgb565(178,178,178));
    if(filesList[i].dir) almFolderIcon(28, y + 12, 38);
    else {
      fillRoundRect(30, y + 10, 30, 42, 5, rgb565(250,250,252));
      fillRect(30, y + 10, 30, 8, rgb565(200,204,214));
    }
    drawTextClip(84, y + 10, filesList[i].name, 3, rgb565(16,18,24), SCR_W - 150);
    char sub[32];
    if(filesList[i].dir) snprintf(sub, sizeof(sub), "%u elementos", (unsigned)filesList[i].items);
    else                 flexFsFmtSize(filesList[i].size, sub, sizeof(sub));
    drawTextR(SCR_W - 28, y + 40, sub, 2, rgb565(50,54,64));
  }
  if(filesN == 0 && !filesHasUp())
    drawTextC(SCR_W / 2, 320, "Carpeta vac\xC3\xAD" "a", 3, rgb565(70,74,84));

  if(filesMulti){
    int by = SCR_H - 128;
    fillRoundRect(12, by, SCR_W - 24, 60, 16, rgb565(30,34,46));
    drawText(28, by + 20, "Selecci\xC3\xB3n", 2, rgb565(240,242,248));
    drawTextR(SCR_W - 140, by + 20, "Papelera", 2, rgb565(250,210,120));
    drawTextR(SCR_W - 28,  by + 20, "Salir", 2, rgb565(180,188,205));
  }
  if(fkMenuOn) fkMenuDraw();
  flxFlushAll();
}

static void filesEnter(){
  if(!flexFsReady()){ fkNoFsScreen("Archivos"); gState = ST_FILES; return; }
  gState = ST_FILES;
  snprintf(filesDir, sizeof(filesDir), "/");
  filesSelIdx = -1; filesScroll = 0; filesMulti = false; filesMask = 0;
  fkMenuOn = false; fkNameOn = false; fkAskOn = false; fkTrashOn = false;
  filesReload();
  filesRender();
}

static void filesExit(){
  // Volver a Almacenamiento REPINTANDO: si desde aqui se borro o renombro
  // algo, los tamanos de la pantalla anterior ya no valen. Ademas hay que
  // rehacer el marco de la app entero (barra de estado y nav): el explorador
  // dibuja a pantalla completa y se lo ha llevado por delante.
  gState = ST_APP;
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, WIN_BG);
  appDrawChrome(gAppId);
  appDrawHeader(gAppId);
  almEnter();
  flxFlushAll();
}

static void filesGoUp(){
  char* s = strrchr(filesDir, '/');
  if(!s || s == filesDir){ snprintf(filesDir, sizeof(filesDir), "/"); }
  else *s = 0;
  filesScroll = 0; filesSelIdx = -1; filesMulti = false; filesMask = 0;
  filesReload(); filesRender();
}

static void filesMenuAction(int act){
  char p[FLEXFS_PATH_MAX];
  if(filesSelIdx >= 0 && filesSelIdx < filesN) filesPathOf(filesSelIdx, p, sizeof(p));
  else p[0] = 0;
  if(act == FK_ACT_SEL){
    filesMulti = true; filesMask = 0;
    if(filesSelIdx >= 0) filesMask |= (1UL << filesSelIdx);
  } else if(act == FK_ACT_DEL){
    if(p[0]){ fkAskOpen("\xC2\xBF" "Borrar definitivamente?", filesList[filesSelIdx].name); return; }
  } else if(act == FK_ACT_REN){
    if(p[0]){
      char stem[FLEXFS_NAME_MAX]; flexFsStem(filesList[filesSelIdx].name, stem, sizeof(stem));
      fkNameOpen("Renombrar", stem);
      return;
    }
  } else if(act == FK_ACT_TRASH){
    if(p[0]){ flexFsTrash(p); filesSelIdx = -1; filesReload(); }
    else { fkTrashOpen(); return; }               // sin seleccion: abre la papelera
  }
  filesRender();
}

static void filesTick(){
  if(!flexFsReady()){ if(T.tap && T.x < 60 && T.y < 60) filesExit(); return; }
  if(fkTrashOn){ if(!fkTrashTick()){ filesReload(); filesRender(); } return; }
  if(fkAskOn){
    int r = fkAskTick();
    if(r == 1 && filesSelIdx >= 0 && filesSelIdx < filesN){
      char p[FLEXFS_PATH_MAX]; filesPathOf(filesSelIdx, p, sizeof(p));
      flexFsDelete(p);
      filesSelIdx = -1; filesReload();
    }
    if(r != 0) filesRender();
    return;
  }
  if(fkNameOn){
    int r = fkNameTick();
    if(r == 1 && filesSelIdx >= 0 && filesSelIdx < filesN){
      char p[FLEXFS_PATH_MAX]; filesPathOf(filesSelIdx, p, sizeof(p));
      flexFsRename(p, fkNameBuf);
      filesSelIdx = -1; filesReload();
    }
    if(r != 0) filesRender();
    return;
  }
  if(fkMenuOn){
    if(T.tap){
      int a = fkMenuHit(T.x, T.y);
      fkMenuOn = false;
      if(a >= 0) filesMenuAction(a);
      else       filesRender();
    }
    return;
  }

  int maxS = filesMaxScroll();
  if(T.pressed){ filesDragY0 = T.y; filesDragS0 = filesScroll; filesDragging = false; }
  if(T.down && maxS > 0){
    int dy = filesDragY0 - T.y;
    if(!filesDragging && abs(dy) > 8) filesDragging = true;
    if(filesDragging){
      int ns = filesDragS0 + dy;
      if(ns < 0) ns = 0; if(ns > maxS) ns = maxS;
      if(ns != filesScroll){ filesScroll = ns; filesRender(); }
      filesLongFired = true;
      return;
    }
  }
  if(T.released && filesDragging){ filesDragging = false; return; }

  int base = filesHasUp() ? 1 : 0;
  if(T.down && !filesLongFired && (millis() - T.downMs) > 550
     && abs(T.x - T.startX) < 14 && abs(T.y - T.startY) < 14){
    for(int i = 0; i < filesN; i++){
      int y = filesRowY(base + i);
      if(T.startY >= y && T.startY < y + FILES_RH - 8){
        filesLongFired = true; filesSelIdx = i;
        fkMenuOpen(T.x, T.y - 40);
        return;
      }
    }
    filesLongFired = true;
  }
  if(!T.down) filesLongFired = false;
  if(!T.tap) return;

  if(T.x > SCR_W - 60 && T.y < 76){ filesSelIdx = -1; fkMenuOpen(SCR_W - 40, 56); return; }
  if(T.x < 60 && T.y < 60){ filesExit(); return; }

  if(filesMulti){
    int by = SCR_H - 128;
    if(T.y >= by && T.y <= by + 60){
      if(T.x > SCR_W - 100){ filesMulti = false; filesMask = 0; filesRender(); return; }
      if(T.x > SCR_W - 230){
        for(int i = 0; i < filesN; i++) if(filesMask & (1UL << i)){
          char p[FLEXFS_PATH_MAX]; filesPathOf(i, p, sizeof(p));
          flexFsTrash(p);
        }
        filesMulti = false; filesMask = 0; filesReload(); filesRender(); return;
      }
    }
  }
  if(filesHasUp()){
    int y = filesRowY(0);
    if(T.y >= y && T.y < y + FILES_RH - 8){ filesGoUp(); return; }
  }
  for(int i = 0; i < filesN; i++){
    int y = filesRowY(base + i);
    if(T.y >= y && T.y < y + FILES_RH - 8){
      if(filesMulti){ filesMask ^= (1UL << i); filesRender(); return; }
      if(filesList[i].dir){
        char p[FLEXFS_PATH_MAX]; filesPathOf(i, p, sizeof(p));
        strncpy(filesDir, p, sizeof(filesDir) - 1); filesDir[sizeof(filesDir) - 1] = 0;
        filesScroll = 0; filesSelIdx = -1;
        filesReload(); filesRender();
      } else {
        filesSelIdx = i;
        fkMenuOpen(SCR_W / 2 - 60, y + 30);       // un fichero suelto: acciones sobre el
      }
      return;
    }
  }
}

// EDUCACION (y cualquier app de lista de tarjetas) · adaptativa.
//   Esencial   : la lista de tarjetas, repartida en COLUMNAS segun el ancho
//                (una columna necesita >= 220 px), de modo que al ensanchar la
//                ventana no queda medio lienzo en blanco: pasa a 2 o 3 columnas.
//   Opcional 1 : subtitulo "Proximamente" dentro de cada tarjeta -- aparece
//                cuando la tarjeta tiene >= 56 px de alto (si no, solo el
//                titulo, que es lo esencial).
static void simpCards(const char* title, const char* items[], int n){
  setBuf(fb);
  int bx, by, bw, bh; uiBox(bx, by, bw, bh);
  fillRect(bx, by, bw, bh, WIN_BG);
  int pad = uiPad(), gap = uiGap();
  int y0 = by + pad;
  int fsT = uiFontFit(title, bw - 2 * pad, uiFontH(bh / 12));
  drawTextC(bx + bw / 2, y0, title, fsT, rgb565(255,255,255));
  y0 += uiLineH(fsT) + gap;
  int cols = (bw - 2 * pad + gap) / (220 + gap); if(cols < 1) cols = 1; if(cols > 3) cols = 3;
  int rows = (n + cols - 1) / cols;
  int cw = (bw - 2 * pad - (cols - 1) * gap) / cols;
  int availH = (by + bh) - y0 - pad;
  int chh = (availH - (rows - 1) * gap) / (rows > 0 ? rows : 1);
  if(chh > 110) chh = 110;
  if(chh < 26) chh = 26;
  uint8_t aSub = uiSection(0, chh >= 56);
  int rad = uiPad();
  for(int i = 0; i < n; i++){
    int c = i % cols, r = i / cols;
    int x = bx + pad + c * (cw + gap), y = y0 + r * (chh + gap);
    if(y + chh > by + bh) break;                     // nunca fuera del marco
    if(uiGlass && !gLand) drawLiquidGlassPanel(x, y, cw, chh, rad, rgb565(60,80,150));
    else fillRoundRect(x, y, cw, chh, rad, rgb565(40,44,58));
    int fsI = uiFontFit(items[i], cw - 2 * pad, uiFontH(chh / 2));
    int ty = aSub ? (y + chh / 2 - uiLineH(fsI)) : (y + chh / 2 - uiLineH(fsI) / 2);
    drawText(x + pad, ty, items[i], fsI, rgb565(255,255,255));
    if(aSub) uiText(x + pad, ty + uiLineH(fsI) + 4, "Proximamente", 1, rgb565(150,158,180), aSub);
  }
  flxFlush(WIN_TOP, WIN_BOT);
}
static void eduEnter(){ const char* it[4] = { "Electr\xC3\xB3nica b\xC3\xA1sica", "Programaci\xC3\xB3n C++", "Redes y WiFi", "Sensores I2C" }; simpCards("Educaci\xC3\xB3n", it, 4); }
// #############################################################
// ##  JUEGO: "Geo Dash" -- selector de niveles + 4 niveles REALES
// ##  (Stereo Madness, Clutterfunk, Can't Let Go, Blast Processing)
// ##  con 5 formas (Cubo/Ship/Ball/Wave/UFO), portales de forma /
// ##  tamano / gravedad / velocidad, Modo Practica con checkpoints
// ##  y progreso persistente por nivel en Preferences ("flexos").
// ##
// ##  ORIENTACION HORIZONTAL: se dibuja rotado 90 sobre el panel
// ##  portrait usando el modo landscape ya existente (gLand). El
// ##  lienzo LOGICO es 800x480 (LW x LH). Igual que el Modo PC, la
// ##  app pone gLand=true y es la unica que gestiona la pantalla;
// ##  al salir se restaura gLand=false.
// ##
// ##  Patron: APP_OWN_TOUCH | APP_CUSTOM_HEADER -- posee TODA la
// ##  pantalla y TODOS los toques, compone en bbuf y vuelca con
// ##  present() (anti-flicker), y trae su propio boton de salir.
// ##
// ##  Todo el estado es static a nivel de archivo (sin heap).
// ##  NINGUNA firma de funcion usa los tipos GeoObstacle/GeoTrail/
// ##  GeoPart/GeoLevel, asi los prototipos que auto-genera el IDE
// ##  (insertados ARRIBA del archivo, antes de estas definiciones)
// ##  nunca referencian un tipo desconocido -> compila limpio.
// #############################################################

// ---- Geometria del escenario (coords LOGICAS landscape: ancho LW=800, alto LH=480) ----
#define GEO_TS       34          // tamano de baldosa (pincho/bloque)
#define GEO_PL       34          // lado del jugador a tamano NORMAL (mini = la mitad)
#define GEO_PLX      140         // X fija del jugador
#define GEO_HUD_H    40          // banda superior (salir + progreso)
#define GEO_FLOOR_Y  400         // Y del suelo (superficie superior de la franja de suelo)
#define GEO_CEIL_Y   84          // Y del techo (superficie en gravedad invertida / corredores)

// ---- Fisica (px/seg; dt real via millis()) ----
#define GEO_GRAV     2600.0f     // gravedad (cubo / ball)
#define GEO_JUMP     720.0f      // impulso de salto del cubo (apice ~100 px)
#define GEO_FALLCAP  1300.0f     // tope de velocidad de caida (cubo / ball)
#define GEO_SPEED    230.0f      // velocidad base de scroll del mundo
#define GEO_ROT      3.6f        // vel. de giro cosmetico (rad/s)
#define GEO_SHIP_GRAV 1500.0f    // gravedad reducida del ship
#define GEO_SHIP_ACC  3200.0f    // empuje del ship al mantener tocado
#define GEO_SHIP_VMAX 520.0f     // tope de velocidad vertical del ship
#define GEO_UFO_GRAV  1500.0f    // gravedad del ufo (flappy)
#define GEO_UFO_IMP   470.0f     // impulso corto del ufo por cada tap
#define GEO_UFO_VMAX  660.0f     // tope de velocidad vertical del ufo
#define GEO_LANDTOL  30          // holgura de "aterrizar desde arriba" sobre un bloque
#define GEO_FRAME_MS 33          // throttle de render (~30 FPS estables)
#define GEO_DTMAX    0.05f       // dt maximo por frame (tope si el loop se traba)
#define GEO_SUBSTEP  0.02f       // paso fijo de la fisica (evita tuneles a cualquier FPS)
#define GEO_RESPAWN_MS 520       // espera tras morir antes de reaparecer

// ---- Estela y particulas ----
#define GEO_TRAIL_N   22
#define GEO_TRAIL_FADE 120.0f    // distancia (px de scroll) tras la que la estela se apaga
#define GEO_PART_N    18

// ---- Paleta base (los colores de identidad de cada nivel se aplican aparte) ----
#define GEO_INK      rgb565(10,12,26)      // relleno oscuro de obstaculos
#define GEO_TXT      rgb565(232,236,248)
#define GEO_AMBER    rgb565(250,200,70)    // portal de gravedad
#define GEO_CYAN     rgb565(90,220,240)    // portal de velocidad
#define GEO_PURP     rgb565(200,120,255)   // portal de forma
#define GEO_LIME     rgb565(150,240,120)   // portal de tamano

// ---- Tipos de obstaculo (los primeros 6 conservan su valor original) ----
enum { OBS_PICO = 0, OBS_BLOQUE, OBS_HUECO, OBS_PICO_T,
       OBS_PORTAL_GRAV, OBS_PORTAL_VEL,
       OBS_P_CUBO, OBS_P_SHIP, OBS_P_BALL, OBS_P_WAVE, OBS_P_UFO,
       OBS_P_MINI, OBS_P_NORMAL, OBS_BLOQUE_F };

// x = posicion en el MUNDO (px); tipo; param = altura en baldosas (bloque),
// ancho en baldosas (hueco) o subtipo (portal de velocidad).
struct GeoObstacle { int16_t x; uint8_t tipo; uint8_t param; };

// ---- Formas del jugador ----
enum { FRM_CUBO = 0, FRM_SHIP, FRM_BALL, FRM_WAVE, FRM_UFO };

// #############################################################
// ##  DATOS DE LOS 4 NIVELES (nivel = DATA, no dibujo a mano)
// #############################################################

// NIVEL 1 -- STEREO MADNESS (1*): cubo tutorial + un tramo de ship a la mitad
// y otro corto cerca del final. Progresion MUY gradual, gaps generosos.
static const GeoObstacle LVL0[] = {
  {  560, OBS_PICO,     0 },                 // pista de arranque larga
  {  780, OBS_PICO,     0 },
  { 1000, OBS_BLOQUE,   1 },                 // bloque simple (saltar por encima/encima)
  { 1240, OBS_PICO,     0 },
  { 1480, OBS_BLOQUE,   2 },                 // plataforma alta
  { 1740, OBS_HUECO,    2 },                 // hueco (saltar por encima)
  { 2000, OBS_PICO,     0 },
  { 2220, OBS_P_SHIP,   0 },                 // --> SHIP (zona espaciosa)
  { 2480, OBS_PICO_T,   0 },
  { 2600, OBS_PICO,     0 },
  { 2820, OBS_PICO_T,   0 },
  { 3020, OBS_P_CUBO,   0 },                 // --> volver a CUBO
  { 3220, OBS_PICO,     0 },
  { 3420, OBS_BLOQUE,   1 },
  { 3620, OBS_PICO,     0 },
  { 3800, OBS_P_SHIP,   0 },                 // --> SHIP corto final
  { 4020, OBS_PICO_T,   0 },
  { 4140, OBS_PICO,     0 },
  { 4340, OBS_P_CUBO,   0 },
  { 4520, OBS_PICO,     0 },
};

// NIVEL 2 -- CLUTTERFUNK (11*, el mas dificil): portal de TAMANO (mini/normal),
// inversion de gravedad frecuente, ship angosto y un tramo de ball. Denso.
static const GeoObstacle LVL1[] = {
  {  480, OBS_PICO,        0 },
  {  640, OBS_PICO,        0 },
  {  780, OBS_BLOQUE,      1 },
  {  920, OBS_PICO,        0 },
  { 1060, OBS_PORTAL_GRAV, 0 },              // invertir -> al techo
  { 1240, OBS_PICO_T,      0 },
  { 1380, OBS_PICO_T,      0 },
  { 1520, OBS_PORTAL_GRAV, 0 },              // volver a normal
  { 1660, OBS_PICO,        0 },
  { 1800, OBS_P_MINI,      0 },              // --> MINI (hitbox a la mitad)
  { 1920, OBS_PICO,        0 },              // obstaculos mas juntos en mini
  { 2020, OBS_PICO,        0 },
  { 2130, OBS_BLOQUE,      1 },
  { 2250, OBS_PICO,        0 },
  { 2380, OBS_P_NORMAL,    0 },              // --> tamano NORMAL
  { 2540, OBS_P_SHIP,      0 },              // --> SHIP angosto
  { 2740, OBS_PICO_T,      0 },
  { 2840, OBS_PICO,        0 },
  { 3000, OBS_PICO_T,      0 },
  { 3100, OBS_PICO,        0 },
  { 3260, OBS_P_BALL,      0 },              // --> BALL (tap invierte gravedad)
  { 3440, OBS_PICO,        0 },
  { 3580, OBS_PICO_T,      0 },
  { 3740, OBS_PICO,        0 },
  { 3900, OBS_PORTAL_VEL,  2 },              // acelerar (tramo final)
  { 4060, OBS_P_CUBO,      0 },
  { 4200, OBS_PICO,        0 },
  { 4340, OBS_BLOQUE,      2 },
  { 4500, OBS_PICO,        0 },
};

// NIVEL 3 -- CAN'T LET GO (6*): introduce el modo WAVE (zigzag diagonal en
// corredores con picos arriba Y abajo). Cubo y ship mas ajustados que Stereo.
static const GeoObstacle LVL2[] = {
  {  500, OBS_PICO,     0 },
  {  700, OBS_PICO,     0 },
  {  860, OBS_BLOQUE,   1 },
  { 1040, OBS_PICO,     0 },
  { 1200, OBS_PICO,     0 },                 // picos mas seguidos
  { 1380, OBS_P_WAVE,   0 },                 // --> WAVE (corredor 1)
  { 1540, OBS_PICO_T,   0 },
  { 1640, OBS_PICO,     0 },
  { 1760, OBS_PICO_T,   0 },
  { 1860, OBS_PICO,     0 },
  { 2000, OBS_P_CUBO,   0 },                 // --> CUBO
  { 2180, OBS_PICO,     0 },
  { 2340, OBS_P_SHIP,   0 },                 // --> SHIP ajustado
  { 2540, OBS_PICO_T,   0 },
  { 2640, OBS_PICO,     0 },
  { 2820, OBS_P_CUBO,   0 },                 // --> CUBO
  { 2980, OBS_PICO,     0 },
  { 3120, OBS_BLOQUE,   2 },
  { 3280, OBS_P_WAVE,   0 },                 // --> WAVE (corredor 2)
  { 3440, OBS_PICO_T,   0 },
  { 3540, OBS_PICO,     0 },
  { 3660, OBS_PICO_T,   0 },
  { 3780, OBS_P_CUBO,   0 },                 // --> CUBO
  { 3960, OBS_PICO,     0 },
  { 4140, OBS_PICO,     0 },
};

// NIVEL 4 -- BLAST PROCESSING (10*): usa las 5 formas (Cubo->Wave->Ship->Ball->
// UFO), un tramo de wave largo y fino (lo mas dificil) y un final traicionero
// con bloques reales/falsos (visualmente iguales, solo algunos solidos).
static const GeoObstacle LVL3[] = {
  {  460, OBS_PICO,     0 },
  {  660, OBS_BLOQUE,   1 },
  {  840, OBS_PICO,     0 },
  { 1000, OBS_P_WAVE,   0 },                 // --> WAVE largo (seccion mas dificil)
  { 1140, OBS_PICO_T,   0 },
  { 1240, OBS_PICO,     0 },
  { 1340, OBS_PICO_T,   0 },
  { 1440, OBS_PICO,     0 },
  { 1540, OBS_PICO_T,   0 },
  { 1640, OBS_PICO,     0 },
  { 1780, OBS_P_SHIP,   0 },                 // --> SHIP
  { 1980, OBS_PICO_T,   0 },
  { 2080, OBS_PICO,     0 },
  { 2260, OBS_P_BALL,   0 },                 // --> BALL
  { 2440, OBS_PICO,     0 },
  { 2580, OBS_PICO_T,   0 },
  { 2760, OBS_P_UFO,    0 },                 // --> UFO (tap = salto corto)
  { 2940, OBS_PICO_T,   0 },
  { 3040, OBS_PICO,     0 },
  { 3220, OBS_PICO_T,   0 },
  { 3400, OBS_P_CUBO,   0 },                 // --> CUBO (final)
  { 3560, OBS_BLOQUE,   1 },                 // bloque real
  { 3700, OBS_BLOQUE_F, 1 },                 // bloque FALSO (no solido)
  { 3820, OBS_PICO,     0 },
  { 3960, OBS_BLOQUE_F, 1 },                 // falso
  { 4080, OBS_BLOQUE,   1 },                 // real
  { 4220, OBS_PICO,     0 },
  { 4360, OBS_BLOQUE,   1 },
};

#define LVLN(a) ((uint8_t)(sizeof(a) / sizeof(a[0])))
#define GEO_TRIG_MAX 48          // >= max de obstaculos de cualquier nivel

// Metadatos + paleta de identidad de cada nivel (name/stars/skin/sky/floor/neon).
struct GeoLevel {
  const char* name; uint8_t stars;
  uint16_t skin, sky, floorc, neon;
  const GeoObstacle* obs; uint8_t obsN; uint16_t len;
};
static const GeoLevel GEO_LEVELS[4] = {
  { "Stereo Madness", 1,  rgb565(70,150,240),  rgb565(26,54,168),  rgb565(10,16,64),  rgb565(120,200,255), LVL0, LVLN(LVL0), 4600 },
  { "Clutterfunk",   11,  rgb565(232,66,204),  rgb565(176,40,168), rgb565(70,10,72),  rgb565(255,130,240), LVL1, LVLN(LVL1), 4600 },
  { "Can't Let Go",   6,  rgb565(214,204,86),  rgb565(150,158,32), rgb565(58,58,12),  rgb565(232,240,120), LVL2, LVLN(LVL2), 4240 },
  { "Blast Processing",10, rgb565(244,112,58), rgb565(24,150,150), rgb565(8,58,60),   rgb565(120,240,232), LVL3, LVLN(LVL3), 4500 },
};

// Multiplicadores de los portales de velocidad (indexados por param)
static const float GEO_VELMUL[4] = { 0.75f, 1.0f, 1.35f, 1.7f };

// #############################################################
// ##  ESTADO DE LA PARTIDA (todo static, sin heap)
// #############################################################
enum { GEO_PLAY = 0, GEO_DEAD, GEO_WIN };     // estado dentro de una partida
enum { GS_SELECT = 0, GS_GAME };              // pantalla activa de la app
static int    gScreen    = GS_SELECT;         // selector <-> juego
static int    gCurLevel  = 0;                 // nivel elegido en el carrusel

static int    geoState   = GEO_PLAY;
static float  geoScroll  = 0;                 // desplazamiento del mundo hacia la izquierda
static float  geoPlayerY = 0;                 // Y del borde superior del jugador
static float  geoPrevBot = 0;                 // borde inferior del frame anterior (aterrizaje)
static float  geoVelY    = 0;
static float  geoAngle   = 0;                 // giro cosmetico (cubo/ball)
static int    geoGravDir = 1;                 // +1 normal, -1 invertida
static float  geoSpeedMul = 1.0f;
static bool   geoGrounded = true;
static int    geoAttempts = 1;
static int    gForma     = FRM_CUBO;          // forma actual del jugador
static bool   gMini      = false;             // tamano mini activo
static bool   gPractice  = false;             // Modo Practica activo
static bool   geoDownPrev = false;            // estado de T.down del tick anterior (flanco)
static bool   geoHeldLatch = false;           // hubo dedo apoyado desde el ultimo update fisico
static bool   geoTapLatch  = false;           // hubo un flanco de nuevo toque (ball/ufo)
static uint32_t geoFrameMs = 0;               // millis() del frame anterior (para dt)
static uint32_t geoDeadMs  = 0;
static bool   geoTrig[GEO_TRIG_MAX];          // portales ya disparados (evita re-disparo)

// Punteros/estado del nivel activo (se fijan al empezar; asi ninguna firma de
// funcion necesita el tipo GeoObstacle/GeoLevel -> compila limpio).
static const GeoObstacle* gObs = 0;
static int      gObsN = 0;
static int      gLen  = 4500;
static uint16_t gSky, gFloorC, gNeon, gSkin;

// Progreso persistente (cache en RAM; se lee de Preferences al entrar).
static int  geoBest[4] = { 0, 0, 0, 0 };
static bool geoDone[4] = { false, false, false, false };

// Checkpoint de Modo Practica (solo el ultimo alcanzado; NO se guarda en NVS).
static bool  geoCPset = false;
static float geoCPscroll = 0, geoCPy = 0;
static uint8_t geoCPform = FRM_CUBO, geoCPmini = 0;
static int8_t  geoCPgrav = 1;
static float   geoCPspeed = 1.0f;
static float   geoNextCP = 0;                 // umbral de scroll para el proximo checkpoint

// Estela: buffer circular de posiciones (se apagan al alejarse por el scroll)
struct GeoTrail { float scrollAt; int16_t y; bool used; };
static GeoTrail geoTrail[GEO_TRAIL_N];
static int      geoTrailHead = 0;

// Particulas de la explosion de muerte
struct GeoPart { float x, y, vx, vy, life; bool used; };
static GeoPart  geoPart[GEO_PART_N];

// PRNG propio (xorshift32) -- sin dependencias, semilla desde millis()
static uint32_t geoRngState = 0x9e3779b9u;
static inline uint32_t geoRand(){
  uint32_t x = geoRngState; x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return (geoRngState = x);
}
static inline float geoRandf(){ return (geoRand() & 0xFFFF) / 65535.0f; }

// AABB (rectangulo contra rectangulo)
static inline bool geoAABB(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh){
  return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

// Touch fisico (portrait) -> coords LOGICAS landscape (igual que hace el Modo PC).
static inline int geoLX(){ return T.y; }                  // logica X 0..799
static inline int geoLY(){ return (SCR_W - 1) - T.x; }    // logica Y 0..479

// #############################################################
// ##  FAST-FILL landscape: rellena un rect LOGICO escribiendo runs
// ##  CONTIGUOS en memoria fisica (una hLine logica seria dispersa,
// ##  pero una columna logica es una fila fisica contigua). Esto es
// ##  lo que mantiene el FPS estable pese a la rotacion: el fondo,
// ##  suelo y HUD son el grueso del pintado y salen casi a costo
// ##  portrait. Escribe DIRECTO al buffer destino (sin gClip: el
// ##  juego ocupa toda la pantalla), respetando el mismo mapeo que
// ##  putPhys -> encaja pixel a pixel con el resto de primitivas gLand.
// #############################################################
static void geoFillL(uint16_t* buf, int lx, int ly, int w, int h, uint16_t col){
  if(lx < 0){ w += lx; lx = 0; }
  if(ly < 0){ h += ly; ly = 0; }
  if(lx + w > LW) w = LW - lx;
  if(ly + h > LH) h = LH - ly;
  if(w <= 0 || h <= 0) return;
  int px0 = (SCR_W - 1) - (ly + h - 1);          // columna fisica minima del run
  for(int i = 0; i < w; i++){
    uint16_t* p = buf + (size_t)(lx + i) * SCR_W + px0;
    for(int k = 0; k < h; k++) p[k] = col;       // run horizontal fisico = contiguo
  }
}

// #############################################################
// ##  PROGRESO PERSISTENTE (Preferences, namespace "flexos")
// ##  Mismo mecanismo que el resto del sistema. Claves por nivel:
// ##  "g0_best".. "g3_best" (int %), "g0_done".."g3_done" (bool).
// #############################################################
static void geoLoadProgress(){
  prefs.begin("flexos", true);
  char k[12];
  for(int i = 0; i < 4; i++){
    snprintf(k, sizeof(k), "g%d_best", i); geoBest[i] = prefs.getInt(k, 0);
    snprintf(k, sizeof(k), "g%d_done", i); geoDone[i] = prefs.getBool(k, false);
  }
  prefs.end();
}
static void geoSaveProgress(int lvl, int pct, bool done){
  if(pct < 0) pct = 0; if(pct > 100) pct = 100;
  bool changed = false;
  if(pct > geoBest[lvl]){ geoBest[lvl] = pct; changed = true; }
  if(done && !geoDone[lvl]){ geoDone[lvl] = true; changed = true; }
  if(!changed) return;                            // no escribir NVS si nada mejoro
  prefs.begin("flexos", false);
  char k[12];
  snprintf(k, sizeof(k), "g%d_best", lvl); prefs.putInt(k, geoBest[lvl]);
  if(geoDone[lvl]){ snprintf(k, sizeof(k), "g%d_done", lvl); prefs.putBool(k, true); }
  prefs.end();
}

// Fija los punteros/paleta del nivel activo desde la tabla (por indice).
static void geoBindLevel(int idx){
  gCurLevel = idx;
  gObs   = GEO_LEVELS[idx].obs;
  gObsN  = GEO_LEVELS[idx].obsN;
  gLen   = GEO_LEVELS[idx].len;
  gSkin  = GEO_LEVELS[idx].skin;
  gSky   = GEO_LEVELS[idx].sky;
  gFloorC= GEO_LEVELS[idx].floorc;
  gNeon  = GEO_LEVELS[idx].neon;
}

// ---- Reinicio del nivel al estado inicial (no toca intentos) ----
static void geoResetLevel(){
  geoScroll = 0; geoVelY = 0; geoAngle = 0;
  geoGravDir = 1; geoSpeedMul = 1.0f;
  gForma = FRM_CUBO; gMini = false;
  geoPlayerY = GEO_FLOOR_Y - GEO_PL; geoPrevBot = geoPlayerY + GEO_PL;
  geoGrounded = true;
  for(int i = 0; i < GEO_TRIG_MAX; i++) geoTrig[i] = false;
  for(int i = 0; i < GEO_TRAIL_N; i++) geoTrail[i].used = false;
  for(int i = 0; i < GEO_PART_N; i++)  geoPart[i].used = false;
  geoTrailHead = 0;
  geoFrameMs = millis();
}

// ---- Cambio de forma (portal): conserva Y; las formas de vuelo arrancan en el aire ----
static void geoSetForma(int f){
  gForma = f;
  geoAngle = 0;
  if(f == FRM_SHIP || f == FRM_WAVE || f == FRM_UFO) geoGrounded = false;
}

// ---- Cambio de tamano (portal mini/normal): mantiene el borde inferior ----
static void geoSetMini(bool m){
  if(gMini == m) return;
  int oldPL = gMini ? (GEO_PL / 2) : GEO_PL;
  gMini = m;
  int newPL = gMini ? (GEO_PL / 2) : GEO_PL;
  geoPlayerY += (oldPL - newPL);                 // el borde inferior no se mueve
  if(geoPlayerY < GEO_CEIL_Y) geoPlayerY = GEO_CEIL_Y;
}

// ---- Checkpoints de Practica: guardar / restaurar / re-armar portales ----
static void geoSaveCP(){
  geoCPset = true;
  geoCPscroll = geoScroll; geoCPy = geoPlayerY;
  geoCPform = (uint8_t)gForma; geoCPmini = gMini ? 1 : 0;
  geoCPgrav = (int8_t)geoGravDir; geoCPspeed = geoSpeedMul;
}
static void geoRestoreCP(){
  geoScroll = geoCPscroll; geoPlayerY = geoCPy; geoVelY = 0;
  gForma = geoCPform; gMini = (geoCPmini != 0);
  geoGravDir = geoCPgrav; geoSpeedMul = geoCPspeed; geoGrounded = false;
  geoAngle = 0;
  // Re-armar portales: los que quedaron ANTES del checkpoint no deben re-disparar
  // (su efecto ya viene reflejado en el estado guardado); los de despues, si.
  int cp = (int)geoCPscroll;
  for(int i = 0; i < gObsN; i++){
    int sx = gObs[i].x - cp;                      // X en pantalla al momento del checkpoint
    geoTrig[i] = (sx < GEO_PLX + 16);             // ya atravesado -> marcado como disparado
  }
  for(int i = 0; i < GEO_TRAIL_N; i++) geoTrail[i].used = false;
  geoTrailHead = 0;
}

// ---- Explosion de particulas en la posicion del jugador ----
static void geoSpawnParticles(){
  int gPL = gMini ? (GEO_PL / 2) : GEO_PL;
  float cx = GEO_PLX + gPL / 2.0f, cy = geoPlayerY + gPL / 2.0f;
  for(int i = 0; i < GEO_PART_N; i++){
    float ang = geoRandf() * 6.2831853f;
    float sp  = 90.0f + geoRandf() * 260.0f;
    geoPart[i].x = cx; geoPart[i].y = cy;
    geoPart[i].vx = cosf(ang) * sp;
    geoPart[i].vy = sinf(ang) * sp - 70.0f;
    geoPart[i].life = 0.45f + geoRandf() * 0.35f;
    geoPart[i].used = true;
  }
}
static void geoDie(){
  if(geoState != GEO_PLAY) return;
  geoState = GEO_DEAD; geoDeadMs = millis();
  geoVelY = 0;
  geoSpawnParticles();
  // El % guardado del Normal Mode se actualiza al morir (nunca baja).
  if(!gPractice){
    int pct = (int)(geoScroll / (float)gLen * 100.0f);
    geoSaveProgress(gCurLevel, pct, false);
  }
}
static void geoUpdateParticles(float dt){
  for(int i = 0; i < GEO_PART_N; i++){
    if(!geoPart[i].used) continue;
    geoPart[i].x += geoPart[i].vx * dt;
    geoPart[i].y += geoPart[i].vy * dt;
    geoPart[i].vy += 900.0f * dt;
    geoPart[i].life -= dt;
    if(geoPart[i].life <= 0) geoPart[i].used = false;
  }
}

// #############################################################
// ##  FISICA + COLISION + PORTALES + MUERTE (un substep)
// ##  held: dedo apoyado en la zona de juego (latcheado en geoTick).
// ##  Los efectos de "un solo tap" (ball/ufo) se aplican FUERA, en
// ##  geoTick, para no repetirse en cada substep.
// #############################################################
static void geoUpdate(float dt, bool held){
  int gPL = gMini ? (GEO_PL / 2) : GEO_PL;

  // --- Impulso continuo del cubo (salta al mantener si esta en el piso) ---
  if(gForma == FRM_CUBO && geoGrounded && held){
    geoVelY = -geoGravDir * GEO_JUMP;
    geoGrounded = false;
  }
  bool prevGrounded = geoGrounded;

  // --- Scroll del mundo + estela ---
  geoScroll += GEO_SPEED * geoSpeedMul * dt;
  geoTrail[geoTrailHead].scrollAt = geoScroll;
  geoTrail[geoTrailHead].y = (int16_t)(geoPlayerY + gPL / 2);
  geoTrail[geoTrailHead].used = true;
  geoTrailHead = (geoTrailHead + 1) % GEO_TRAIL_N;

  // --- Integracion vertical (depende de la forma) ---
  geoPrevBot = geoPlayerY + gPL;
  if(gForma == FRM_CUBO || gForma == FRM_BALL){
    geoVelY += geoGravDir * GEO_GRAV * dt;
    if(geoVelY >  GEO_FALLCAP) geoVelY =  GEO_FALLCAP;
    if(geoVelY < -GEO_FALLCAP) geoVelY = -GEO_FALLCAP;
  } else if(gForma == FRM_SHIP){
    geoVelY += geoGravDir * GEO_SHIP_GRAV * dt;
    if(held) geoVelY -= geoGravDir * GEO_SHIP_ACC * dt;   // empuje sostenido
    if(geoVelY >  GEO_SHIP_VMAX) geoVelY =  GEO_SHIP_VMAX;
    if(geoVelY < -GEO_SHIP_VMAX) geoVelY = -GEO_SHIP_VMAX;
  } else if(gForma == FRM_UFO){
    geoVelY += geoGravDir * GEO_UFO_GRAV * dt;
    if(geoVelY >  GEO_UFO_VMAX) geoVelY =  GEO_UFO_VMAX;
    if(geoVelY < -GEO_UFO_VMAX) geoVelY = -GEO_UFO_VMAX;
  } else { // FRM_WAVE: diagonal instantanea, sin inercia (45 grados con el scroll)
    float wv = GEO_SPEED * geoSpeedMul;
    geoVelY = held ? -wv : wv;
  }
  geoPlayerY += geoVelY * dt;

  int scroll = (int)geoScroll;
  int pl = GEO_PLX, pr = GEO_PLX + gPL;

  // --- Disparadores de portal (colision de "atravesar", una sola vez) ---
  for(int i = 0; i < gObsN; i++){
    uint8_t tp = gObs[i].tipo;
    if(tp < OBS_PORTAL_GRAV || tp > OBS_P_NORMAL) continue;   // no es portal
    if(geoTrig[i]) continue;
    int sx = gObs[i].x - scroll;
    int bx = sx + GEO_TS / 2 - 15;
    if(geoAABB(pl, (int)geoPlayerY, gPL, gPL, bx, GEO_CEIL_Y, 30, GEO_FLOOR_Y - GEO_CEIL_Y)){
      geoTrig[i] = true;
      switch(tp){
        case OBS_PORTAL_GRAV: geoGravDir = -geoGravDir; break;
        case OBS_PORTAL_VEL:  geoSpeedMul = GEO_VELMUL[gObs[i].param & 3]; break;
        case OBS_P_CUBO:   geoSetForma(FRM_CUBO); break;
        case OBS_P_SHIP:   geoSetForma(FRM_SHIP); break;
        case OBS_P_BALL:   geoSetForma(FRM_BALL); break;
        case OBS_P_WAVE:   geoSetForma(FRM_WAVE); break;
        case OBS_P_UFO:    geoSetForma(FRM_UFO);  break;
        case OBS_P_MINI:   geoSetMini(true);  break;
        case OBS_P_NORMAL: geoSetMini(false); break;
      }
    }
  }

  // --- Resolucion vertical segun forma ---
  geoGrounded = false;
  if(gForma == FRM_CUBO){
    if(geoVelY >= 0){                                        // cayendo: suelo o techo de bloque
      bool overGap = false;
      for(int i = 0; i < gObsN; i++){
        if(gObs[i].tipo != OBS_HUECO) continue;
        int gx0 = gObs[i].x - scroll, gx1 = gx0 + gObs[i].param * GEO_TS;
        if(pr > gx0 && pl < gx1){ overGap = true; break; }
      }
      float top = overGap ? 100000.0f : (float)GEO_FLOOR_Y;
      for(int i = 0; i < gObsN; i++){
        if(gObs[i].tipo != OBS_BLOQUE) continue;
        int bx = gObs[i].x - scroll, by = GEO_FLOOR_Y - gObs[i].param * GEO_TS;
        if(pr > bx && pl < bx + GEO_TS && geoPrevBot <= by + GEO_LANDTOL && by < top) top = by;
      }
      if(geoPlayerY + gPL >= top){ geoPlayerY = top - gPL; geoVelY = 0; geoGrounded = true; }
    }
    if(geoVelY <= 0){                                        // subiendo: techo (grav invertida)
      if(geoPlayerY <= GEO_CEIL_Y){ geoPlayerY = GEO_CEIL_Y; geoVelY = 0; geoGrounded = true; }
    }
  } else if(gForma == FRM_BALL){                             // se pega a piso o techo
    if(geoPlayerY + gPL >= GEO_FLOOR_Y){ geoPlayerY = GEO_FLOOR_Y - gPL; if(geoVelY > 0) geoVelY = 0; geoGrounded = true; }
    if(geoPlayerY <= GEO_CEIL_Y){        geoPlayerY = GEO_CEIL_Y;         if(geoVelY < 0) geoVelY = 0; geoGrounded = true; }
  } else {                                                   // ship/wave/ufo: deslizan en los bordes
    if(geoPlayerY < GEO_CEIL_Y){         geoPlayerY = GEO_CEIL_Y;         if(geoVelY < 0) geoVelY = 0; }
    if(geoPlayerY > GEO_FLOOR_Y - gPL){  geoPlayerY = GEO_FLOOR_Y - gPL;  if(geoVelY > 0) geoVelY = 0; }
  }

  // --- Giro cosmetico ---
  if(gForma == FRM_CUBO){
    if(geoGrounded && !prevGrounded){ float q = 1.5707963f; geoAngle = q * roundf(geoAngle / q); }
    if(!geoGrounded) geoAngle += GEO_ROT * dt;
  } else if(gForma == FRM_BALL){
    geoAngle += (geoGravDir > 0 ? GEO_ROT : -GEO_ROT) * dt * 1.5f;   // rueda
  } else {
    geoAngle = 0;
  }
  while(geoAngle >= 6.2831853f) geoAngle -= 6.2831853f;
  while(geoAngle < 0)           geoAngle += 6.2831853f;

  // --- Muerte: picos (piso/techo), interior de bloque SOLIDO, caer en hueco ---
  int hx = gMini ? 5 : 8, hy = gMini ? 12 : 22, hw = gMini ? 12 : 18;  // hitbox de pico segun tamano
  for(int i = 0; i < gObsN; i++){
    int sx = gObs[i].x - scroll;
    uint8_t tp = gObs[i].tipo;
    if(tp == OBS_PICO){
      if(geoAABB(pl, (int)geoPlayerY, gPL, gPL, sx + hx, GEO_FLOOR_Y - hy - 2, hw, hy)){ geoDie(); return; }
    } else if(tp == OBS_PICO_T){
      if(geoAABB(pl, (int)geoPlayerY, gPL, gPL, sx + hx, GEO_CEIL_Y + 2, hw, hy)){ geoDie(); return; }
    } else if(tp == OBS_BLOQUE){
      int by = GEO_FLOOR_Y - gObs[i].param * GEO_TS;
      if(pr > sx + 2 && pl < sx + GEO_TS - 2 && geoPlayerY + gPL > by + 6 && geoPlayerY < GEO_FLOOR_Y){ geoDie(); return; }
    }
    // OBS_BLOQUE_F: bloque FALSO -> se dibuja igual pero NO colisiona ni sostiene.
  }
  if((gForma == FRM_CUBO || gForma == FRM_BALL) && geoPlayerY > GEO_FLOOR_Y + 30){ geoDie(); return; }

  // --- Fin del nivel ---
  if(geoScroll >= gLen){
    geoState = GEO_WIN;
    if(!gPractice) geoSaveProgress(gCurLevel, 100, true);
  }
}

// #############################################################
// ##  DIBUJO DEL JUEGO (coords LOGICAS 800x480; escribe en bbuf)
// ##  Los grandes rellenos usan geoFillL (runs contiguos) para no
// ##  perder FPS por la rotacion. Obstaculos y jugador usan las
// ##  primitivas gLand normales (triangulos/quads/circulos/texto).
// #############################################################

// ---- Fondo: cielo + parallax + techo solido si la gravedad esta invertida ----
static void geoDrawBackground(){
  geoFillL(bbuf, 0, GEO_HUD_H, LW, GEO_FLOOR_Y - GEO_HUD_H, gSky);
  // Parallax: paneles verticales que derivan a 0.30x del scroll (profundidad).
  uint16_t pane = mix565(gSky, GEO_INK, 60);
  int off = (int)(geoScroll * 0.30f) % 130; if(off < 0) off += 130;
  for(int x = -off; x < LW; x += 130){
    geoFillL(bbuf, x + 14, GEO_HUD_H + 24, 84, GEO_FLOOR_Y - GEO_HUD_H - 60, pane);
  }
  if(geoGravDir < 0){                                        // superficie de techo
    geoFillL(bbuf, 0, GEO_HUD_H, LW, GEO_CEIL_Y - GEO_HUD_H, gFloorC);
    hLine(0, GEO_CEIL_Y,     LW, gNeon);
    hLine(0, GEO_CEIL_Y + 1, LW, mix565(gNeon, GEO_INK, 90));
  }
}

// ---- Suelo con textura de velocidad + huecos ----
static void geoDrawFloor(){
  geoFillL(bbuf, 0, GEO_FLOOR_Y, LW, LH - GEO_FLOOR_Y, gFloorC);
  uint16_t voidc = mix565(gFloorC, rgb565(0,0,0), 120);
  int off = (int)geoScroll % GEO_TS; if(off < 0) off += GEO_TS;
  for(int x = -off; x < LW; x += GEO_TS) vLine(x, GEO_FLOOR_Y, LH - GEO_FLOOR_Y, voidc);
  hLine(0, GEO_FLOOR_Y,     LW, gNeon);
  hLine(0, GEO_FLOOR_Y - 1, LW, mix565(gNeon, GEO_INK, 90));
  // Huecos: recortar el suelo (pozo) y remarcar bordes.
  int scroll = (int)geoScroll;
  for(int i = 0; i < gObsN; i++){
    if(gObs[i].tipo != OBS_HUECO) continue;
    int gx0 = gObs[i].x - scroll, w = gObs[i].param * GEO_TS;
    if(gx0 > LW || gx0 + w < 0) continue;
    geoFillL(bbuf, gx0, GEO_FLOOR_Y - 1, w, LH - GEO_FLOOR_Y + 1, voidc);
    vLine(gx0,     GEO_FLOOR_Y, 36, gNeon);
    vLine(gx0 + w, GEO_FLOOR_Y, 36, gNeon);
  }
}

// ---- Un bloque (real o falso: se dibujan IGUAL) ----
static void geoDrawBlock(int sx, int ntiles){
  int bh = ntiles * GEO_TS, by = GEO_FLOOR_Y - bh;
  fillRect(sx, by, GEO_TS, bh, GEO_INK);
  drawRect(sx, by, GEO_TS, bh, mix565(gNeon, GEO_INK, 90));
  hLine(sx, by, GEO_TS, gNeon);
  for(int k = 1; k < ntiles; k++) hLine(sx, by + k * GEO_TS, GEO_TS, mix565(gNeon, GEO_INK, 90));
  vLine(sx + GEO_TS / 2, by, bh, mix565(gNeon, GEO_INK, 90));
}

// ---- Obstaculos visibles (picos, bloques, portales) ----
static void geoDrawObstacles(){
  int scroll = (int)geoScroll;
  for(int i = 0; i < gObsN; i++){
    int sx = gObs[i].x - scroll;
    if(sx < -GEO_TS * 3 || sx > LW + GEO_TS) continue;       // recorte
    uint8_t tp = gObs[i].tipo;
    if(tp == OBS_PICO){
      fillTriangle(sx, GEO_FLOOR_Y, sx + GEO_TS, GEO_FLOOR_Y, sx + GEO_TS / 2, GEO_FLOOR_Y - GEO_TS, gNeon);
      fillTriangle(sx + 3, GEO_FLOOR_Y - 1, sx + GEO_TS - 3, GEO_FLOOR_Y - 1, sx + GEO_TS / 2, GEO_FLOOR_Y - GEO_TS + 6, GEO_INK);
    } else if(tp == OBS_PICO_T){
      fillTriangle(sx, GEO_CEIL_Y, sx + GEO_TS, GEO_CEIL_Y, sx + GEO_TS / 2, GEO_CEIL_Y + GEO_TS, gNeon);
      fillTriangle(sx + 3, GEO_CEIL_Y + 1, sx + GEO_TS - 3, GEO_CEIL_Y + 1, sx + GEO_TS / 2, GEO_CEIL_Y + GEO_TS - 6, GEO_INK);
    } else if(tp == OBS_BLOQUE || tp == OBS_BLOQUE_F){
      geoDrawBlock(sx, gObs[i].param);                       // real y falso: identicos
    } else if(tp == OBS_PORTAL_GRAV || tp == OBS_PORTAL_VEL ||
              (tp >= OBS_P_CUBO && tp <= OBS_P_NORMAL)){
      uint16_t col;
      if(tp == OBS_PORTAL_GRAV)      col = GEO_AMBER;
      else if(tp == OBS_PORTAL_VEL)  col = GEO_CYAN;
      else if(tp == OBS_P_MINI || tp == OBS_P_NORMAL) col = GEO_LIME;
      else                           col = GEO_PURP;         // portales de forma
      int px_ = sx + GEO_TS / 2;
      fillRectA(px_ - 15, GEO_CEIL_Y, 30, GEO_FLOOR_Y - GEO_CEIL_Y, col, 70);
      drawRect(px_ - 15, GEO_CEIL_Y, 30, GEO_FLOOR_Y - GEO_CEIL_Y, col);
      drawRect(px_ - 14, GEO_CEIL_Y + 1, 28, GEO_FLOOR_Y - GEO_CEIL_Y - 2, col);
      int cy = (GEO_CEIL_Y + GEO_FLOOR_Y) / 2;
      if(tp == OBS_PORTAL_GRAV){                              // flechas arriba/abajo
        fillTriangle(px_, cy - 26, px_ - 9, cy - 12, px_ + 9, cy - 12, col);
        fillTriangle(px_, cy + 26, px_ - 9, cy + 12, px_ + 9, cy + 12, col);
      } else if(tp == OBS_PORTAL_VEL){                        // chevrones (velocidad)
        fillTriangle(px_ - 10, cy - 14, px_ - 10, cy + 14, px_ + 4, cy, col);
        fillTriangle(px_ + 2,  cy - 14, px_ + 2,  cy + 14, px_ + 16, cy, col);
      } else if(tp == OBS_P_MINI || tp == OBS_P_NORMAL){      // simbolo de tamano
        int s = (tp == OBS_P_MINI) ? 6 : 12;
        drawRect(px_ - s, cy - s, 2 * s, 2 * s, col);
        fillRect(px_ - s + 2, cy - s + 2, 2 * s - 4, 2 * s - 4, mix565(col, GEO_INK, 120));
      } else {                                                // letra de la forma
        const char* g = "C";
        if(tp == OBS_P_SHIP) g = "S"; else if(tp == OBS_P_BALL) g = "B";
        else if(tp == OBS_P_WAVE) g = "W"; else if(tp == OBS_P_UFO) g = "U";
        drawTextC(px_, cy - 8, g, 2, col);
      }
    }
  }
}

// ---- Estela (cuadraditos que se desvanecen hacia atras) ----
static void geoDrawTrail(){
  int gPL = gMini ? (GEO_PL / 2) : GEO_PL;
  for(int i = 0; i < GEO_TRAIL_N; i++){
    if(!geoTrail[i].used) continue;
    float dist = geoScroll - geoTrail[i].scrollAt;
    if(dist >= GEO_TRAIL_FADE){ geoTrail[i].used = false; continue; }
    int sx = GEO_PLX + gPL / 2 - (int)dist;
    if(sx < -10){ geoTrail[i].used = false; continue; }
    float f = 1.0f - dist / GEO_TRAIL_FADE;
    int sz = (int)(3 + f * 11);
    uint8_t a = (uint8_t)(f * 150);
    fillRectA(sx - sz / 2, geoTrail[i].y - sz / 2, sz, sz, gSkin, a);
  }
}

// ---- Cuadrado rotado por sus 4 esquinas (cubo) ----
static void geoQuadRot(int cx, int cy, float h, float s, float c, uint16_t col){
  int x0 = cx + (int)(-h * c + h * s), y0 = cy + (int)(-h * s - h * c);
  int x1 = cx + (int)( h * c + h * s), y1 = cy + (int)( h * s - h * c);
  int x2 = cx + (int)( h * c - h * s), y2 = cy + (int)( h * s + h * c);
  int x3 = cx + (int)(-h * c - h * s), y3 = cy + (int)(-h * s + h * c);
  fillQuad(x0, y0, x1, y1, x2, y2, x3, y3, col);
}

// ---- Jugador: dibujo por forma (cx,cy = centro; r = radio segun tamano) ----
static void geoDrawPlayer(int cx, int cy, int r){
  uint16_t hi = gSkin, dk = mix565(gSkin, GEO_INK, 150), eye = rgb565(240,246,255);
  if(gForma == FRM_CUBO){
    float s = sinf(geoAngle), c = cosf(geoAngle);
    geoQuadRot(cx, cy, r,          s, c, hi);
    geoQuadRot(cx, cy, r * 0.62f,  s, c, dk);
    geoQuadRot(cx, cy, r * 0.28f,  s, c, eye);
  } else if(gForma == FRM_BALL){
    fillCircle(cx, cy, r, hi);
    fillCircle(cx, cy, (int)(r * 0.60f), dk);
    // marca que gira (sensacion de rodar)
    int mx = cx + (int)(cosf(geoAngle) * r * 0.55f);
    int my = cy + (int)(sinf(geoAngle) * r * 0.55f);
    fillCircle(mx, my, r > 12 ? 4 : 2, eye);
    drawCircle(cx, cy, r, mix565(hi, rgb565(255,255,255), 90));
  } else if(gForma == FRM_SHIP){
    // inclinacion segun velocidad vertical
    float tilt = geoVelY / GEO_SHIP_VMAX; if(tilt > 1) tilt = 1; if(tilt < -1) tilt = -1;
    int ty = (int)(tilt * r * 0.5f);
    fillTriangle(cx - r, cy - r + ty, cx - r, cy + r - ty, cx + r + 4, cy, hi);   // casco
    fillTriangle(cx - r + 3, cy - r + 3 + ty, cx - r + 3, cy + r - 3 - ty, cx + r, cy, dk);
    fillCircle(cx - 2, cy - r + 6, r > 12 ? 6 : 3, eye);                          // cabina
    fillCircle(cx - 2, cy - r + 6, r > 12 ? 3 : 2, gSkin);
  } else if(gForma == FRM_WAVE){
    // rombo que apunta en la direccion del movimiento
    int dy = (geoVelY < 0) ? -r : r;
    fillTriangle(cx - r, cy, cx + r, cy, cx, cy + dy, hi);
    fillTriangle(cx - r, cy, cx + r, cy, cx, cy - dy / 2, dk);
  } else { // FRM_UFO
    fillCircle(cx, cy - 1, r, hi);                                                // cupula
    geoFillL(bbuf, cx - r, cy, 2 * r, r / 2 + 2, dk);                             // base
    fillCircle(cx, cy - r / 2, r > 12 ? 4 : 2, eye);
    drawCircle(cx, cy - 1, r, mix565(hi, rgb565(255,255,255), 90));
  }
}

static void geoDrawParticles(){
  for(int i = 0; i < GEO_PART_N; i++){
    if(!geoPart[i].used) continue;
    float f = geoPart[i].life / 0.8f; if(f > 1) f = 1; if(f < 0) f = 0;
    int sz = 2 + (int)(f * 6);
    uint8_t a = (uint8_t)(f * 235);
    uint16_t col = (i & 1) ? gSkin : rgb565(240,246,255);
    fillRectA((int)geoPart[i].x - sz / 2, (int)geoPart[i].y - sz / 2, sz, sz, col, a);
  }
}

// ---- Banderita del ultimo checkpoint (solo Practica, si esta en pantalla) ----
static void geoDrawCheckpoint(){
  if(!gPractice || !geoCPset) return;
  int sx = GEO_PLX + (int)(geoCPscroll - geoScroll);         // deriva hacia la izquierda
  if(sx < -6 || sx > LW) return;
  vLine(sx, GEO_FLOOR_Y - 34, 34, rgb565(255,255,255));
  fillTriangle(sx, GEO_FLOOR_Y - 34, sx + 16, GEO_FLOOR_Y - 28, sx, GEO_FLOOR_Y - 22, rgb565(120,255,140));
}

// ---- HUD (salir + progreso + intentos + boton reiniciar CP en Practica) ----
static void geoDrawHUD(){
  geoFillL(bbuf, 0, 0, LW, GEO_HUD_H, rgb565(18,18,34));
  // Boton salir (vuelve al selector) -- esquina superior izquierda.
  fillRoundRect(10, 6, 46, 28, 8, rgb565(58,58,96));
  strokeSeg(36, 12, 26, 20, 2, GEO_TXT);
  strokeSeg(26, 20, 36, 28, 2, GEO_TXT);
  // Barra de progreso.
  int bx = 66, bxr = gPractice ? (LW - 180) : (LW - 90);
  int bw = bxr - bx, by = 13, bh = 12;
  fillRoundRect(bx, by, bw, bh, 6, rgb565(38,38,64));
  float pr = geoScroll / (float)gLen; if(pr < 0) pr = 0; if(pr > 1) pr = 1;
  if(pr > 0.01f) fillRoundRect(bx, by, (int)(bw * pr), bh, 6, gNeon);
  char p[10]; snprintf(p, sizeof(p), "%d%%", (int)(pr * 100));
  drawTextR(bxr + (gPractice ? 40 : 78), by + 1, p, 1, GEO_TXT);
  // Modo + intentos.
  char b[26]; snprintf(b, sizeof(b), "%s  #%d", gPractice ? "Practica" : "Normal", geoAttempts);
  drawTextR(LW - 8, 28, b, 1, mix565(GEO_TXT, gSky, 60));
  // Boton "Reiniciar CP" (solo Practica).
  if(gPractice){
    fillRoundRect(LW - 132, 6, 122, 28, 8, rgb565(70,54,110));
    drawTextC(LW - 71, 13, "Reiniciar CP", 1, GEO_TXT);
  }
}

// ---- Composicion de un frame de JUEGO (en bbuf) y volcado atomico ----
static void geoRenderGame(){
  gLand = true; setBuf(bbuf);
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  int gPL = gMini ? (GEO_PL / 2) : GEO_PL;
  geoDrawBackground();
  geoDrawFloor();
  geoDrawObstacles();
  geoDrawCheckpoint();
  if(geoState != GEO_DEAD){
    geoDrawTrail();
    geoDrawPlayer(GEO_PLX + gPL / 2, (int)geoPlayerY + gPL / 2, gPL / 2);
  } else {
    geoDrawParticles();
  }
  geoDrawHUD();
  if(geoState == GEO_DEAD){
    drawTextC(LW / 2, GEO_FLOOR_Y - 96, "Fallaste", 4, GEO_TXT);
  } else if(geoState == GEO_WIN){
    drawTextC(LW / 2, GEO_FLOOR_Y - 118, "\xC2\xA1Nivel completado!", 4, gNeon);
    drawTextC(LW / 2, GEO_FLOOR_Y - 78, "Toca para volver al selector", 2, GEO_TXT);
  }
  present(0, SCR_H - 1);
}

// #############################################################
// ##  SELECTOR DE NIVEL (carrusel, como el selector real de GD)
// #############################################################

// Estrellita de 4 puntas (dificultad) -- barata, sin depender de la fuente.
static void geoStar(int cx, int cy, int r, uint16_t col){
  int t = r / 3;
  fillTriangle(cx, cy - r, cx - t, cy, cx + t, cy, col);
  fillTriangle(cx, cy + r, cx - t, cy, cx + t, cy, col);
  fillTriangle(cx - r, cy, cx, cy - t, cx, cy + t, col);
  fillTriangle(cx + r, cy, cx, cy - t, cx, cy + t, col);
}

// Barra de progreso del selector (verde Normal / celeste Practica).
static void geoSelBar(int x, int y, int w, int h, uint16_t fillc, int pct, bool done, const char* fixed){
  fillRoundRect(x, y, w, h, h / 2, rgb565(28,30,44));
  drawRoundRect(x, y, w, h, h / 2, mix565(fillc, rgb565(255,255,255), 60));
  if(pct < 0) pct = 0; if(pct > 100) pct = 100;
  int fw = (done ? w : (w * pct / 100));
  if(fw > h) fillRoundRect(x, y, fw, h, h / 2, fillc);
  else if(fw > 0) fillRect(x + h / 2, y, fw, h, fillc);
  char t[16];
  if(fixed) snprintf(t, sizeof(t), "%s", fixed);
  else if(done) snprintf(t, sizeof(t), "COMPLETADO");
  else snprintf(t, sizeof(t), "%d%%", pct);
  drawTextC(x + w / 2, y + h / 2 - 7, t, 2, rgb565(255,255,255));
}

static void geoRenderSelect(){
  gLand = true; setBuf(bbuf);
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  uint16_t sky = GEO_LEVELS[gCurLevel].sky;
  uint16_t neon = GEO_LEVELS[gCurLevel].neon;
  uint16_t skin = GEO_LEVELS[gCurLevel].skin;
  geoFillL(bbuf, 0, 0, LW, LH, sky);
  // Franja decorativa superior (bloques estilo menu GD).
  geoFillL(bbuf, 0, 0, LW, 12, mix565(sky, rgb565(255,255,255), 40));
  for(int x = 0; x < LW; x += 60) geoFillL(bbuf, x + 6, 2, 42, 20, mix565(neon, sky, 120));

  // Boton salir (cierra la app) -- esquina superior izquierda.
  fillRoundRect(10, 8, 46, 28, 8, mix565(sky, GEO_INK, 110));
  strokeSeg(36, 14, 26, 22, 2, GEO_TXT);
  strokeSeg(26, 22, 36, 30, 2, GEO_TXT);

  // Flechas del carrusel.
  fillTriangle(44, 240, 84, 210, 84, 270, rgb565(255,255,255));
  fillTriangle(52, 240, 82, 216, 82, 264, mix565(sky, GEO_INK, 90));
  fillTriangle(756, 240, 716, 210, 716, 270, rgb565(255,255,255));
  fillTriangle(748, 240, 718, 216, 718, 264, mix565(sky, GEO_INK, 90));

  // Tarjeta central.
  int x0 = 168, y0 = 60, w = 464, h = 316;
  fillRoundRect(x0, y0, w, h, 18, mix565(sky, GEO_INK, 120));
  drawRoundRect(x0, y0, w, h, 18, mix565(neon, sky, 110));

  // "Cara del cubo" (cuadrado de color) + ojos, como el icono del nivel.
  int fx = x0 + 26, fy = y0 + 24, fs = 64;
  fillRoundRect(fx, fy, fs, fs, 12, skin);
  drawRoundRect(fx, fy, fs, fs, 12, mix565(skin, rgb565(255,255,255), 80));
  fillRect(fx + 16, fy + 24, 10, 12, rgb565(20,22,34));
  fillRect(fx + fs - 26, fy + 24, 10, 12, rgb565(20,22,34));

  // Nombre + dificultad.
  drawText(x0 + 104, y0 + 34, GEO_LEVELS[gCurLevel].name, 3, rgb565(255,255,255));
  char st[6]; snprintf(st, sizeof(st), "%d", GEO_LEVELS[gCurLevel].stars);
  int sw = textW(st, 2);
  drawText(x0 + w - 24 - sw - 18, y0 + 20, st, 2, rgb565(250,210,80));
  geoStar(x0 + w - 22, y0 + 26, 9, rgb565(250,210,80));

  // Barras Normal / Practica.
  drawTextC(x0 + w / 2, y0 + 118, "Normal Mode", 2, rgb565(230,236,250));
  geoSelBar(x0 + 44, y0 + 142, w - 88, 34, rgb565(70,220,90),
            geoBest[gCurLevel], geoDone[gCurLevel], NULL);
  drawTextC(x0 + w / 2, y0 + 202, "Practice Mode", 2, rgb565(230,236,250));
  geoSelBar(x0 + 44, y0 + 226, w - 88, 34, rgb565(90,205,240), 0, false, "Practicar");

  // Puntos indicadores del carrusel.
  for(int i = 0; i < 4; i++){
    int dx = LW / 2 - 3 * 22 / 2 + i * 22;
    if(i == gCurLevel) fillCircle(dx, 432, 6, rgb565(255,255,255));
    else               fillCircle(dx, 432, 4, mix565(sky, rgb565(255,255,255), 120));
  }
  drawTextC(LW / 2, 448, "Toca una barra para jugar", 1, mix565(rgb565(255,255,255), sky, 90));

  present(0, SCR_H - 1);
}

// #############################################################
// ##  MENU DE JUEGOS  (capa por encima de Geo Dash / Futbol)
// ##  ------------------------------------------------------
// ##  La app "Juegos" (slot 11 de APP_REG) ya NO entra directa a
// ##  Geo Dash: geoEnter() abre PRIMERO este menu, que deja elegir
// ##  "Geometry Dash" (motor original, intacto) o "Futbol". El
// ##  routing esta en geoTick() (mas abajo). El motor de Geo Dash
// ##  no se toca -> CERO regresiones; solo cambia el DESTINO del
// ##  boton salir de su selector (antes cerraba la app; ahora
// ##  vuelve a este menu). Todo lo nuevo lleva prefijo games*/fut*.
// #############################################################
enum { GM_MENU = 0, GM_GEO, GM_FUT };
static int gGameMode = GM_MENU;

// #############################################################
// ##  FUTBOL ARCADE  (modo landscape, camara alta con scroll)
// ##  ------------------------------------------------------
// ##  Mismo patron que Geo Dash: lienzo LOGICO 800x480 (LW x LH),
// ##  compone en bbuf y vuelca con present() (anti-flicker), ~30
// ##  fps con throttle por millis() (GEO_FRAME_MS) y fisica con
// ##  substep fijo (GEO_SUBSTEP), watchdog seguro (sin delays).
// ##  TODO el estado es static (sin heap): no se aloca ningun
// ##  framebuffer nuevo -- se dibuja directo sobre bbuf, igual que
// ##  geoRenderGame(). NINGUN struct nuevo (FutP) se usa como
// ##  PARAMETRO de funcion: se indexa por int, asi los prototipos
// ##  que auto-genera el IDE nunca referencian un tipo desconocido.
// ##
// ##  HARDWARE: input tactil de UN SOLO punto (struct Touch T). No
// ##  hay joystick ni botones fisicos, asi que los controles son
// ##  ZONAS tactiles dibujadas: un stick analogico virtual (mueve
// ##  al jugador con balon / mas cercano) y botones Sprint/Pase/
// ##  Tiro/Cambio. Al ser un solo dedo, mover y pulsar boton son
// ##  excluyentes por diseno: el stick vive en la mitad izquierda
// ##  y los botones en la derecha; Sprint es un TOGGLE y Pase/Tiro/
// ##  Cambio usan la ULTIMA direccion del stick (futFace) como
// ##  orientacion. Pseudo-3D barato: escalado por profundidad (Y
// ##  en cancha) con tablas Q8 precalculadas, sin trig por sprite.
// #############################################################

// ---- Geometria del campo (coords de MUNDO) ----
#define FUT_LEN     1400    // largo del campo (eje X mundo; porteria en 0 y en FUT_LEN)
#define FUT_WID     300     // ancho del campo (eje Y mundo; 0=fondo lejano, FUT_WID=cerca)
#define FUT_MIDY    150     // centro del ancho (FUT_WID/2)
#define FUT_GHALF   46      // medio-ancho de la porteria (en Y de mundo, centrada en FUT_MIDY)
#define FUT_CX      400     // X de pantalla que sigue la camara (LW/2)
#define FUT_HORIZON 116     // Y de pantalla de la touchline LEJANA (fondo del campo)
#define FUT_BOTTOM  452     // Y de pantalla de la touchline CERCANA

// ---- Fisica (unidades de mundo/seg; dt real via millis()) ----
#define FUT_MATCH_SEC   150.0f   // segundos reales de un partido completo (-> 90' de juego)
#define FUT_BASE_SPD    118.0f   // velocidad base de carrera de un jugador
#define FUT_SPRINT_MUL  1.62f    // multiplicador de Sprint
#define FUT_CARRY_SPD   108.0f   // velocidad del portador (algo mas lento que corriendo)
#define FUT_GK_SPD      96.0f    // velocidad del portero
#define FUT_PASS_SPD    380.0f   // velocidad del balon en un pase
#define FUT_SHOOT_SPD   540.0f   // velocidad del balon en un tiro
#define FUT_DRIBBLE     15.0f    // distancia del balon por delante del portador
#define FUT_CTRL_R      15.0f    // radio de control del balon suelto (jugador de campo)
#define FUT_GK_CTRL_R   26.0f    // radio de control/atajada del portero (mas amplio)
#define FUT_TACKLE_R    15.0f    // radio para intentar quitar el balon
#define FUT_SHOOT_RANGE 380.0f   // distancia a porteria a la que la IA se plantea tirar
#define FUT_BALL_G      460.0f   // gravedad del balon en el aire (altura Z)
#define FUT_BALL_FRIC   1.7f     // rozamiento del balon suelto en el suelo (por seg)

// ---- Controles virtuales (coords LOGICAS landscape) ----
#define FUT_STK_CX   118    // stick analogico: centro X
#define FUT_STK_CY   372    // stick analogico: centro Y
#define FUT_STK_R    66     // radio del stick
#define FUT_STK_DEAD 12     // zona muerta
#define FUT_BTN_R    30     // radio de los botones
enum { FBT_SPR = 0, FBT_PAS, FBT_TIR, FBT_CAM };
static const int16_t FUT_BTNX[4] = { 696, 696, 764, 628 };   // SPR arriba, PAS abajo, TIR der, CAM izq
static const int16_t FUT_BTNY[4] = { 306, 438, 372, 372 };

// ---- Roles y estados de IA (maquina de estados barata) ----
enum { FR_GK = 0, FR_DEF, FR_MID, FR_FWD };
enum { FA_HOLD = 0, FA_CHASE, FA_SUPPORT, FA_MARK, FA_GK, FA_CARRY };

// ---- Formacion base 4-3-3 del equipo 0 (ataca +X). El equipo 1 es
//      el espejo (x' = FUT_LEN - x). 11 jugadores por equipo. ----
static const int16_t FUT_FORM_X[11] = {  60, 300,300,300,300, 560,600,560, 860,940,860 };
static const int16_t FUT_FORM_Y[11] = { 150,  60,110,190,240,  90,150,210,  80,150,220 };
static const uint8_t FUT_FORM_R[11] = { FR_GK, FR_DEF,FR_DEF,FR_DEF,FR_DEF, FR_MID,FR_MID,FR_MID, FR_FWD,FR_FWD,FR_FWD };

// #############################################################
// ##  EQUIPOS (24-32) EN FLASH (.rodata) -- NO en RAM/PSRAM.
// ##  Struct compacto pedido: { char nombre[16]; uint8_t rating;
// ##  uint16_t colorA; uint16_t colorB; }. Sin escudos/logos con
// ##  derechos: el escudo se genera en runtime (forma + colores).
// ##  Incluye selecciones, clubes top y clubes peruanos.
// #############################################################
struct FutTeam { char nombre[16]; uint8_t rating; uint16_t colorA; uint16_t colorB; };
static const FutTeam FUT_TEAMS[] = {
  { "Peru",         78, rgb565(220,40,60),   rgb565(245,245,245) },  // 0  (por defecto)
  { "Argentina",    90, rgb565(110,190,235), rgb565(245,245,245) },
  { "Brasil",       91, rgb565(245,215,40),  rgb565(20,110,60)   },
  { "Uruguay",      84, rgb565(90,170,225),  rgb565(20,24,40)    },
  { "Chile",        79, rgb565(210,40,50),   rgb565(30,60,140)   },
  { "Colombia",     83, rgb565(245,210,40),  rgb565(30,70,160)   },
  { "Mexico",       81, rgb565(20,130,70),   rgb565(245,245,245) },
  { "Espana",       90, rgb565(200,30,40),   rgb565(30,40,120)   },
  { "Francia",      92, rgb565(40,70,170),   rgb565(245,245,245) },
  { "Alemania",     90, rgb565(240,240,245), rgb565(20,24,34)    },
  { "Inglaterra",   88, rgb565(245,245,245), rgb565(30,40,120)   },
  { "Italia",       88, rgb565(40,90,190),   rgb565(245,245,245) },
  { "Portugal",     89, rgb565(190,30,50),   rgb565(20,110,70)   },
  { "Paises Bajos", 86, rgb565(240,130,30),  rgb565(245,245,245) },
  { "Alianza Lima", 76, rgb565(30,60,150),   rgb565(245,245,245) },  // 14  peruano
  { "Universitario",76, rgb565(235,230,215), rgb565(140,30,50)   },  // 15  peruano (crema)
  { "S. Cristal",   75, rgb565(90,180,225),  rgb565(245,245,245) },  // 16  peruano (celeste)
  { "Barcelona",    90, rgb565(140,30,60),   rgb565(30,50,120)   },  // 17  (grana + azul)
  { "Real Madrid",  91, rgb565(245,245,245), rgb565(210,180,60)  },
  { "Man City",     90, rgb565(120,195,225), rgb565(245,245,245) },
  { "Liverpool",    88, rgb565(210,40,50),   rgb565(245,245,245) },
  { "Bayern",       90, rgb565(210,30,50),   rgb565(245,245,245) },
  { "PSG",          88, rgb565(30,40,90),    rgb565(210,40,60)   },
  { "Juventus",     86, rgb565(240,240,245), rgb565(20,22,30)    },
  { "Milan",        85, rgb565(200,30,45),   rgb565(20,22,30)    },
  { "Boca Juniors", 80, rgb565(30,50,130),   rgb565(230,190,60)  },
  { "River Plate",  80, rgb565(240,240,245), rgb565(210,40,60)   },
  { "Flamengo",     82, rgb565(210,40,50),   rgb565(20,22,30)    },
};
#define FUT_NTEAMS ((int)(sizeof(FUT_TEAMS) / sizeof(FUT_TEAMS[0])))

// #############################################################
// ##  ESTADO DEL PARTIDO (todo static, sin heap)
// #############################################################
#define FUT_NP 22
struct FutP { float x, y, vx, vy; uint8_t team; uint8_t role; uint8_t st; float dcd; };
static FutP futP[FUT_NP];

enum { FS_TEAM = 0, FS_OPP, FS_PLAY, FS_END };   // pantalla activa del modo Futbol
static int   futScreen  = FS_TEAM;
static int   futTeamSel[2] = { 0, 1 };           // [0]=tu equipo, [1]=rival (indices en FUT_TEAMS)
static int   futScoreA = 0, futScoreB = 0;
static float futPlaySec = 0;                     // tiempo de JUEGO acumulado (0..5400 = 0'..90')

static float futBx, futBy, futBz;                // balon: posicion mundo + altura Z
static float futBvx, futBvy, futBvz;             // balon: velocidad
static int   futOwner   = -1;                    // -1 = suelto, else indice del portador
static int   futCtrl    = 0;                     // jugador controlado (siempre del equipo 0)
static int   futLastTouch = -1;                  // ultimo que toco el balon (para cooldown de pase)
static float futTouchCd = 0;                     // cooldown para que el pasador no reciba su propio pase
static float futCam     = FUT_CX;                // X de mundo bajo la camara (sigue al balon)

static float   futMoveX = 0, futMoveY = 0;       // vector del stick (-1..1)
static float   futFaceX = 1, futFaceY = 0;       // ultima direccion (orientacion para pase/tiro)
static int     futStkKnobX = 0, futStkKnobY = 0; // desplazamiento visual de la perilla
static bool    futSprint = false;                // toggle de Sprint
static bool    futReqPass = false, futReqShoot = false, futReqSwitch = false, futReqSprint = false;

static uint32_t futFrameMs = 0;                  // millis() del frame anterior (dt)
static float    futDt = 0;                       // dt del frame actual (para la IA)
static uint32_t futFreezeUntil = 0;              // congela juego (saque / celebracion de gol)
static bool     futPending = false;              // hay un saque pendiente tras un gol
static int      futKickNext = 0;                 // equipo que saca tras el gol
static char     futFlash[16] = "";               // rotulo grande temporal ("GOL")

// ---- Tablas Q8 de perspectiva (pseudo-3D): profundidad d=wy/FUT_WID.
//      Se calculan UNA vez (powf/interp) -> por sprite solo hay lookups. ----
#define FUT_DSTEPS 64
static uint16_t futSyLUT[FUT_DSTEPS];   // Y de pantalla de los PIES a cada profundidad
static uint8_t  futPHt [FUT_DSTEPS];    // alto del sprite (px) a cada profundidad
static uint8_t  futXsLUT[FUT_DSTEPS];   // Q8: compresion horizontal (trapecio) * 256
static bool     futLUTready = false;

static void futBuildLUT(){
  for(int i = 0; i < FUT_DSTEPS; i++){
    float t = (float)i / (FUT_DSTEPS - 1);          // 0=lejos(arriba) .. 1=cerca(abajo)
    float ty = powf(t, 1.22f);                       // leve curva de perspectiva en Y
    futSyLUT[i] = (uint16_t)(FUT_HORIZON + ty * (FUT_BOTTOM - FUT_HORIZON));
    futPHt[i]   = (uint8_t)(12 + t * 34);            // 12 px (lejos) .. 46 px (cerca)
    float xs    = 0.60f + t * 0.40f;                 // 0.60 (lejos) .. 1.00 (cerca) -> trapecio
    futXsLUT[i] = (uint8_t)(xs * 256.0f + 0.5f);
  }
  futLUTready = true;
}

// ---- PRNG: reutiliza el xorshift de Geo Dash (geoRand/geoRandf) ----
static inline float futErrf(){ return geoRandf() * 2.0f - 1.0f; }   // [-1..1]

// #############################################################
// ##  TRANSFORMACIONES MUNDO -> PANTALLA (afines + LUT, sin trig)
// #############################################################
static inline int futDIdx(float wy){
  int d = (int)(wy * (FUT_DSTEPS - 1) / FUT_WID);
  if(d < 0) d = 0; if(d > FUT_DSTEPS - 1) d = FUT_DSTEPS - 1;
  return d;
}
static inline int futGX(float wx, float wy){          // X de pantalla en el suelo
  int d = futDIdx(wy);
  return FUT_CX + (int)(((long)((int)(wx - futCam)) * futXsLUT[d]) >> 8);
}
static inline int futGY(float wy){ return futSyLUT[futDIdx(wy)]; }   // Y de pantalla (pies)

static inline float futDist2(int i, int j){
  float dx = futP[i].x - futP[j].x, dy = futP[i].y - futP[j].y;
  return dx * dx + dy * dy;
}
static inline float futHomeX(int i){
  int k = i % 11; float x = FUT_FORM_X[k];
  return (i >= 11) ? (FUT_LEN - x) : x;
}
static inline float futHomeY(int i){ return FUT_FORM_Y[i % 11]; }
static inline float futGoalX(int team){ return (team == 0) ? (float)FUT_LEN : 0.0f; }   // porteria que ATACA
static inline float futOwnX (int team){ return (team == 0) ? 0.0f : (float)FUT_LEN; }   // porteria que DEFIENDE
static inline float futRating(int team){ return FUT_TEAMS[futTeamSel[team]].rating / 99.0f; }  // 0..1

// #############################################################
// ##  PRIMITIVAS DE DIBUJO PROPIAS DEL FUTBOL
// #############################################################
// Elipse rellena y aplastada (sombra bajo pies / balon)
static void futShadow(int cx, int cy, int rx, int ry, uint16_t col, uint8_t a){
  if(rx < 1) rx = 1; if(ry < 1) ry = 1;
  for(int dy = -ry; dy <= ry; dy++){
    int dx = (int)(rx * sqrtf(1.0f - (float)dy * dy / ((float)ry * ry)));
    hLineA(cx - dx, cy + dy, 2 * dx + 1, col, a);
  }
}
// Elipse (contorno) para el circulo central
static void futEllipse(int cx, int cy, int rx, int ry, uint16_t col){
  if(rx < 2) rx = 2; if(ry < 1) ry = 1;
  int pxp = cx + rx, pyp = cy;
  for(int a = 1; a <= 40; a++){
    float th = a * (6.2831853f / 40);
    int x = cx + (int)(rx * cosf(th)), y = cy + (int)(ry * sinf(th));
    strokeSeg(pxp, pyp, x, y, 1, col); pxp = x; pyp = y;
  }
}
// Escudo generado en runtime (forma pentagonal + franja) con los colores del equipo
static void futCrest(int cx, int cy, int w, int h, uint16_t colA, uint16_t colB){
  int x0 = cx - w / 2, y0 = cy - h / 2, hr = h * 3 / 5;
  fillRect(x0, y0, w, hr, colA);
  fillTriangle(x0, y0 + hr, x0 + w, y0 + hr, cx, y0 + h, colA);
  int sw = w / 3; if(sw < 3) sw = 3;
  fillRect(cx - sw / 2, y0, sw, hr, colB);
  fillTriangle(cx - sw / 2, y0 + hr, cx + sw / 2, y0 + hr, cx, y0 + h - 3, colB);
  uint16_t edge = rgb565(238,240,246);
  drawRect(x0, y0, w, hr, edge);
  strokeSeg(x0, y0 + hr, cx, y0 + h, 1, edge);
  strokeSeg(x0 + w, y0 + hr, cx, y0 + h, 1, edge);
}

// #############################################################
// ##  BUSQUEDAS AUXILIARES (nearest, etc.)
// #############################################################
static int futNearestOutfield(int team, float x, float y){   // el mas cercano (sin portero)
  int best = -1; float bd = 1e18f;
  for(int k = 0; k < 11; k++){
    int i = team * 11 + k;
    if(futP[i].role == FR_GK) continue;
    float dx = futP[i].x - x, dy = futP[i].y - y, d = dx * dx + dy * dy;
    if(d < bd){ bd = d; best = i; }
  }
  return best;
}
static int futNearestToBall(int team){ return futNearestOutfield(team, futBx, futBy); }
static int futNearestOpp(int i){
  int opp = (futP[i].team == 0) ? 1 : 0, best = -1; float bd = 1e18f;
  for(int k = 0; k < 11; k++){
    int j = opp * 11 + k; float d = futDist2(i, j);
    if(d < bd){ bd = d; best = j; }
  }
  return best;
}

// #############################################################
// ##  PASE / TIRO / CAMBIO / QUITE  (acciones)
// #############################################################
static void futSetLoose(int from, float dirx, float diry, float speed, float upz){
  float d = sqrtf(dirx * dirx + diry * diry); if(d < 0.0001f){ dirx = futFaceX; diry = futFaceY; d = 1; }
  futBvx = dirx / d * speed; futBvy = diry / d * speed; futBvz = upz;
  futOwner = -1; futLastTouch = from; futTouchCd = 0.22f; futBz = 0;
}
// Pase del jugador humano (solo si su equipo tiene el balon)
static void futDoPass(){
  if(futOwner < 0 || futP[futOwner].team != 0) return;
  int o = futOwner, best = -1; float bestSc = -1e18f;
  for(int k = 0; k < 11; k++){
    int r = k; if(r == o || futP[r].role == FR_GK) continue;
    float dx = futP[r].x - futP[o].x, dy = futP[r].y - futP[o].y;
    float dist = sqrtf(dx * dx + dy * dy);
    if(dist < 28 || dist > 380) continue;
    float dot = (dx * futFaceX + dy * futFaceY) / dist;         // alineado con la direccion apuntada
    float fwd = (futGoalX(0) > futP[o].x) ? (dx > 0 ? 0.4f : -0.3f) : 0;
    float sc = dot + fwd - dist * 0.0012f;
    if(sc > bestSc){ bestSc = sc; best = r; }
  }
  if(best < 0) return;
  // Precision del pase: escala con el rating propio, con margen de error MINIMO (>=10%).
  float err = 0.10f + (1.0f - futRating(0)) * 0.16f;
  float lead = 0.12f;
  float tx = futP[best].x + futP[best].vx * lead, ty = futP[best].y + futP[best].vy * lead;
  float dx = tx - futBx + futErrf() * err * 90, dy = ty - futBy + futErrf() * err * 90;
  futSetLoose(o, dx, dy, FUT_PASS_SPD, 0);
}
// Tiro del jugador humano
static void futDoShoot(){
  if(futOwner < 0 || futP[futOwner].team != 0) return;
  int o = futOwner;
  float gx = futGoalX(0), gy = FUT_MIDY;
  float dx = gx - futBx, dy = gy - futBy;
  dx += futFaceX * 60; dy += futFaceY * 40;                     // sesga con lo apuntado
  float err = 0.10f + (1.0f - futRating(0)) * 0.14f;
  dy += futErrf() * err * 120;
  futSetLoose(o, dx, dy, FUT_SHOOT_SPD, 70 + geoRandf() * 40);  // con algo de altura
  futFlash[0] = 0;
}
// Cambiar de jugador controlado (util cuando NO tienes el balon)
static void futDoSwitch(){
  int start = futCtrl, best = -1; float bd = 1e18f;
  for(int k = 0; k < 11; k++){
    int i = k; if(i == start || futP[i].role == FR_GK) continue;
    float dx = futP[i].x - futBx, dy = futP[i].y - futBy, d = dx * dx + dy * dy;
    if(d < bd){ bd = d; best = i; }
  }
  if(best >= 0) futCtrl = best;
}

// #############################################################
// ##  IA (maquina de estados barata; sin pathfinding)
// #############################################################
static void futSteer(int i, float tx, float ty, float maxspd){
  float dx = tx - futP[i].x, dy = ty - futP[i].y, d = sqrtf(dx * dx + dy * dy);
  if(d > 1.5f){ futP[i].vx = dx / d * maxspd; futP[i].vy = dy / d * maxspd; }
  else { futP[i].vx = 0; futP[i].vy = 0; }
}
// Tiro/pase de un portador controlado por la IA (equipo del portador)
static void futAIShoot(int i){
  int t = futP[i].team; float gx = futGoalX(t);
  float dx = gx - futBx, dy = FUT_MIDY - futBy;
  float err = 0.10f + (1.0f - futRating(t)) * 0.16f;            // margen MINIMO 10% aun en top
  dy += futErrf() * err * 130;
  futSetLoose(i, dx, dy, FUT_SHOOT_SPD, 60 + geoRandf() * 40);
}
static void futAIPass(int i){
  int t = futP[i].team, best = -1; float bestSc = -1e18f;
  for(int k = 0; k < 11; k++){
    int r = t * 11 + k; if(r == i || futP[r].role == FR_GK) continue;
    float dx = futP[r].x - futP[i].x, dy = futP[r].y - futP[i].y, dist = sqrtf(dx * dx + dy * dy);
    if(dist < 30 || dist > 340) continue;
    float toward = (futGoalX(t) - futP[i].x);                   // preferir hacia el ataque
    float fwd = ((dx > 0) == (toward > 0)) ? 0.5f : -0.2f;
    // penaliza si hay un rival cerca del receptor
    int op = futNearestOpp(r); float opd = op >= 0 ? sqrtf(futDist2(r, op)) : 999;
    float sc = fwd + opd * 0.004f - dist * 0.0011f;
    if(sc > bestSc){ bestSc = sc; best = r; }
  }
  if(best < 0){ futAIShoot(i); return; }
  float err = 0.10f + (1.0f - futRating(t)) * 0.15f;
  float dx = futP[best].x - futBx + futErrf() * err * 90;
  float dy = futP[best].y - futBy + futErrf() * err * 90;
  futSetLoose(i, dx, dy, FUT_PASS_SPD, 0);
}
// Decision del portador IA: regatear / tirar / pasar
static void futAICarry(int i){
  int t = futP[i].team; float skill = futRating(t);
  float gx = futGoalX(t), gy = FUT_MIDY;
  float dgx = gx - futP[i].x, dgy = gy - futP[i].y, dg = sqrtf(dgx * dgx + dgy * dgy);
  int op = futNearestOpp(i); float pd = op >= 0 ? sqrtf(futDist2(i, op)) : 999;
  futP[i].dcd -= futDt;
  if(futP[i].dcd <= 0){
    bool inRange = (dg < FUT_SHOOT_RANGE);
    if(inRange && geoRandf() < (0.28f + 0.55f * skill)){ futAIShoot(i); futP[i].dcd = 0.7f; return; }
    if(pd < 28 && geoRandf() < (0.35f + 0.45f * skill)){ futAIPass(i); futP[i].dcd = 0.6f; return; }
    futP[i].dcd = 0.14f + (1.0f - skill) * 0.34f;               // reaccion: menos rating -> repiensa mas lento
  }
  // Regate hacia porteria, esquivando levemente la presion
  float tx = gx, ty = gy;
  if(pd < 40 && op >= 0){ ty += (futP[i].y < futP[op].y) ? -50 : 50; }
  futSteer(i, tx, ty, FUT_CARRY_SPD * (0.9f + 0.2f * skill));
  futP[i].st = FA_CARRY;
}
static void futGKTarget(int i, float* tx, float* ty){
  int t = futP[i].team; float ogx = futOwnX(t);
  *tx = ogx + (t == 0 ? 42 : -42);
  float y = futBy; if(y < FUT_MIDY - 72) y = FUT_MIDY - 72; if(y > FUT_MIDY + 72) y = FUT_MIDY + 72;
  *ty = y;
}
static void futAI(){
  int poss = (futOwner >= 0) ? futP[futOwner].team : -1;
  if(futOwner >= 0 && futP[futOwner].team == 0) futCtrl = futOwner;   // tu equipo con balon -> controlas al portador
  int chaser0 = futNearestToBall(0), chaser1 = futNearestToBall(1);
  for(int i = 0; i < FUT_NP; i++){
    if(i == futCtrl && futP[i].team == 0) continue;                  // al controlado lo mueve el humano
    int t = futP[i].team, role = futP[i].role;
    if(role == FR_GK){ float tx, ty; futGKTarget(i, &tx, &ty); futSteer(i, tx, ty, FUT_GK_SPD); futP[i].st = FA_GK; continue; }
    float spd = (role == FR_FWD) ? 126 : (role == FR_MID ? 118 : 112);
    if(poss == t){
      if(futOwner == i){ futAICarry(i); continue; }                  // portador IA
      // apoyo: empuja hacia el ataque y abre espacios
      float tx = futHomeX(i) * 0.40f + futBx * 0.28f + futGoalX(t) * 0.32f;
      float ty = futHomeY(i) * 0.45f + futBy * 0.55f;
      futSteer(i, tx, ty, spd); futP[i].st = FA_SUPPORT;
    } else if(poss == (1 - t)){
      int chaser = (t == 0) ? chaser0 : chaser1;
      if(i == chaser){ futSteer(i, futBx, futBy, spd * 1.14f); futP[i].st = FA_CHASE; }
      else {           // marca zonal: posicion base desplazada hacia el balon
        float tx = futHomeX(i) * 0.62f + futBx * 0.24f + futOwnX(t) * 0.14f;
        float ty = futHomeY(i) * 0.50f + futBy * 0.50f;
        futSteer(i, tx, ty, spd * 0.96f); futP[i].st = FA_MARK;
      }
    } else {           // balon suelto: el mas cercano de cada equipo va a por el
      int chaser = (t == 0) ? chaser0 : chaser1;
      if(i == chaser){ futSteer(i, futBx, futBy, spd * 1.08f); futP[i].st = FA_CHASE; }
      else { float tx = futHomeX(i) * 0.66f + futBx * 0.20f; float ty = futHomeY(i) * 0.55f + futBy * 0.45f;
             futSteer(i, tx, ty, spd * 0.9f); futP[i].st = FA_HOLD; }
    }
  }
}

// #############################################################
// ##  FISICA + COLISION (un substep)  ·  watchdog-safe (sin delay)
// #############################################################
static int futBallController(){
  int best = -1; float bd = 1e18f;
  for(int i = 0; i < FUT_NP; i++){
    if(i == futLastTouch && futTouchCd > 0) continue;               // el pasador no recibe su propio pase
    float r = (futP[i].role == FR_GK) ? FUT_GK_CTRL_R : FUT_CTRL_R;
    float dx = futP[i].x - futBx, dy = futP[i].y - futBy, d2 = dx * dx + dy * dy;
    if(d2 <= r * r && d2 < bd){ bd = d2; best = i; }
  }
  return best;
}
static void futGoalKick(int defTeam){                                // saque de porteria
  int gk = defTeam * 11 + 0;
  futBx = futP[gk].x; futBy = futP[gk].y; futBz = 0;
  futBvx = futBvy = futBvz = 0; futOwner = gk; futLastTouch = gk; futTouchCd = 0;
  if(defTeam == 0) futCtrl = gk;
}
static void futGoal(int team){
  if(team == 0) futScoreA++; else futScoreB++;
  snprintf(futFlash, sizeof(futFlash), "GOL");
  futFreezeUntil = millis() + 1500;
  futPending = true; futKickNext = 1 - team;                        // saca el que encajo
  futOwner = -1;
}
static void futStep(float dt){
  // --- Quite: rivales cerca del portador intentan robar ---
  if(futOwner >= 0){
    int o = futOwner, ot = futP[o].team;
    for(int k = 0; k < 11; k++){
      int j = (1 - ot) * 11 + k;
      if(futP[j].role == FR_GK) continue;
      if(futDist2(o, j) <= FUT_TACKLE_R * FUT_TACKLE_R){
        float sk = futRating(futP[j].team);
        float prob = (1.4f + 2.2f * sk) * dt;                       // por segundo, escalado por rating
        if(geoRandf() < prob){
          futOwner = j; futLastTouch = j; futTouchCd = 0.14f;
          if(futP[j].team == 0) futCtrl = j;
          break;
        }
      }
    }
  }
  // --- Integracion de jugadores ---
  for(int i = 0; i < FUT_NP; i++){
    futP[i].x += futP[i].vx * dt; futP[i].y += futP[i].vy * dt;
    if(futP[i].x < 4) futP[i].x = 4; if(futP[i].x > FUT_LEN - 4) futP[i].x = FUT_LEN - 4;
    if(futP[i].y < 6) futP[i].y = 6; if(futP[i].y > FUT_WID - 6) futP[i].y = FUT_WID - 6;
  }
  if(futTouchCd > 0) futTouchCd -= dt;
  // --- Balon ---
  if(futOwner >= 0){
    int o = futOwner; float fdx, fdy;
    if(o == futCtrl && futP[o].team == 0){ fdx = futFaceX; fdy = futFaceY; }
    else { float d = sqrtf(futP[o].vx * futP[o].vx + futP[o].vy * futP[o].vy);
           if(d > 4){ fdx = futP[o].vx / d; fdy = futP[o].vy / d; }
           else { fdx = (futGoalX(futP[o].team) > futP[o].x) ? 1 : -1; fdy = 0; } }
    float fn = sqrtf(fdx * fdx + fdy * fdy); if(fn < 0.001f){ fdx = 1; fdy = 0; fn = 1; }
    futBx = futP[o].x + fdx / fn * FUT_DRIBBLE;
    futBy = futP[o].y + fdy / fn * FUT_DRIBBLE;
    // Mantener el balon dentro del campo aunque el portador este pegado a una
    // linea: si no, un tiro calculado desde una X fuera de rango saldria invertido.
    if(futBx < 2) futBx = 2; if(futBx > FUT_LEN - 2) futBx = FUT_LEN - 2;
    if(futBy < 2) futBy = 2; if(futBy > FUT_WID - 2) futBy = FUT_WID - 2;
    futBz = 0; futBvx = futBvy = futBvz = 0;
  } else {
    futBx += futBvx * dt; futBy += futBvy * dt;
    float fr = 1.0f - FUT_BALL_FRIC * dt; if(fr < 0) fr = 0;
    futBvx *= fr; futBvy *= fr;
    if(futBz > 0 || futBvz != 0){
      futBz += futBvz * dt; futBvz -= FUT_BALL_G * dt;
      if(futBz <= 0){ futBz = 0; if(futBvz < 0){ futBvz = -futBvz * 0.42f; if(futBvz < 24) futBvz = 0; } }
    }
    if(futBy < 4){ futBy = 4; futBvy = -futBvy * 0.5f; }            // rebote en las bandas
    if(futBy > FUT_WID - 4){ futBy = FUT_WID - 4; futBvy = -futBvy * 0.5f; }
    // --- Linea de gol / porteria ---
    bool inMouth = (futBy > FUT_MIDY - FUT_GHALF && futBy < FUT_MIDY + FUT_GHALF && futBz < 46);
    if(futBx >= FUT_LEN - 2){
      if(inMouth){ futGoal(0); return; } else { futGoalKick(1); return; }
    } else if(futBx <= 2){
      if(inMouth){ futGoal(1); return; } else { futGoalKick(0); return; }
    }
    int g = futBallController();
    if(g >= 0){ futOwner = g; futBvx = futBvy = futBvz = 0; futBz = 0; if(futP[g].team == 0) futCtrl = g; }
  }
}

// ---- Colocar la formacion (saque). kickTeam pone un jugador al centro. ----
static void futResetFormation(int kickTeam){
  for(int t = 0; t < 2; t++){
    for(int k = 0; k < 11; k++){
      int i = t * 11 + k;
      float x = FUT_FORM_X[k], y = FUT_FORM_Y[k];
      if(t == 1) x = FUT_LEN - x;
      if(t == 0 && x > 688) x = 688;                               // en el saque, cada equipo en su campo
      if(t == 1 && x < 712) x = 712;
      futP[i].x = x; futP[i].y = y; futP[i].vx = 0; futP[i].vy = 0;
      futP[i].team = (uint8_t)t; futP[i].role = FUT_FORM_R[k]; futP[i].st = FA_HOLD; futP[i].dcd = 0;
    }
  }
  int kick = kickTeam * 11 + 6;                                    // el mediocentro saca
  futP[kick].x = (kickTeam == 0) ? 686 : 714; futP[kick].y = FUT_MIDY;
  futBx = 700; futBy = FUT_MIDY; futBz = 0; futBvx = futBvy = futBvz = 0;
  futOwner = -1; futLastTouch = -1; futTouchCd = 0; futCam = 700;
  futFaceX = (kickTeam == 0) ? 1 : -1; futFaceY = 0;
  futCtrl = futNearestToBall(0);
}

// #############################################################
// ##  DIBUJO DEL CAMPO (coords LOGICAS 800x480; escribe en bbuf)
// #############################################################
static void futDrawMarkings(){
  uint16_t ln = rgb565(232,240,236);
  int c00x = futGX(0,0),        c00y = futGY(0);
  int c10x = futGX(FUT_LEN,0),  c10y = futGY(0);
  int c01x = futGX(0,FUT_WID),  c01y = futGY(FUT_WID);
  int c11x = futGX(FUT_LEN,FUT_WID), c11y = futGY(FUT_WID);
  strokeSeg(c00x, c00y, c10x, c10y, 1, ln);                        // touchline lejana
  strokeSeg(c01x, c01y, c11x, c11y, 1, ln);                        // touchline cercana
  strokeSeg(c00x, c00y, c01x, c01y, 1, ln);                        // fondo izq (linea de gol)
  strokeSeg(c10x, c10y, c11x, c11y, 1, ln);                        // fondo der
  // Linea de medio campo + circulo central
  int mid = FUT_LEN / 2;
  strokeSeg(futGX(mid,0), futGY(0), futGX(mid,FUT_WID), futGY(FUT_WID), 1, ln);
  int ccx = futGX(mid, FUT_MIDY), ccy = futGY(FUT_MIDY);
  int crx = (futGX(mid + 70, FUT_MIDY) - futGX(mid - 70, FUT_MIDY)) / 2;
  int cry = (futGY(FUT_MIDY + 42) - futGY(FUT_MIDY - 42)) / 2;
  futEllipse(ccx, ccy, crx, cry, ln);
  fillCircle(ccx, ccy, 2, ln);
  // Areas (una en cada porteria): tres lineas del rectangulo
  for(int s = 0; s < 2; s++){
    float bx = (s == 0) ? 0 : (FUT_LEN - 160);
    float bx2 = (s == 0) ? 160 : FUT_LEN;
    float edge = (s == 0) ? bx2 : bx;                              // linea vertical interior del area
    strokeSeg(futGX(edge, FUT_MIDY - 100), futGY(FUT_MIDY - 100),
              futGX(edge, FUT_MIDY + 100), futGY(FUT_MIDY + 100), 1, ln);
    strokeSeg(futGX(bx, FUT_MIDY - 100), futGY(FUT_MIDY - 100),
              futGX(bx2, FUT_MIDY - 100), futGY(FUT_MIDY - 100), 1, ln);
    strokeSeg(futGX(bx, FUT_MIDY + 100), futGY(FUT_MIDY + 100),
              futGX(bx2, FUT_MIDY + 100), futGY(FUT_MIDY + 100), 1, ln);
  }
  // Porterias (postes + larguero + red simple)
  for(int s = 0; s < 2; s++){
    float gx = (s == 0) ? 0 : FUT_LEN;
    int d = futDIdx(FUT_MIDY); int gh = futPHt[d] + 8;             // alto en pantalla
    int p0x = futGX(gx, FUT_MIDY - FUT_GHALF), p0y = futGY(FUT_MIDY - FUT_GHALF);
    int p1x = futGX(gx, FUT_MIDY + FUT_GHALF), p1y = futGY(FUT_MIDY + FUT_GHALF);
    uint16_t net = rgb565(220,226,230);
    // red
    for(int n = 1; n < 5; n++){
      int yy0 = p0y - gh * n / 5, yy1 = p1y - gh * n / 5;
      strokeSeg(p0x, yy0, p1x, yy1, 1, mix565(net, rgb565(60,110,70), 150));
    }
    strokeSeg(p0x, p0y, p0x, p0y - gh, 1, rgb565(245,245,245));    // poste 1
    strokeSeg(p1x, p1y, p1x, p1y - gh, 1, rgb565(245,245,245));    // poste 2
    strokeSeg(p0x, p0y - gh, p1x, p1y - gh, 1, rgb565(245,245,245)); // larguero
  }
}
static void futDrawField(){
  // Cielo + tribuna sobre el horizonte
  geoFillL(bbuf, 0, 0, LW, FUT_HORIZON, rgb565(58,68,104));
  geoFillL(bbuf, 0, FUT_HORIZON - 30, LW, 30, rgb565(38,42,58));
  int soff = ((int)futCam) % 16; if(soff < 0) soff += 16;
  for(int x = -soff; x < LW; x += 16)                              // publico (motas)
    geoFillL(bbuf, x + 3, FUT_HORIZON - 26, 6, 20, rgb565(70,78,104));
  // Cesped base
  geoFillL(bbuf, 0, FUT_HORIZON, LW, LH - FUT_HORIZON, rgb565(38,150,54));
  // Franjas de cortado (ancladas al mundo -> hacen scroll con la camara)
  int sw = 48;
  int n0 = (((int)futCam - LW / 2) / sw) - 2;
  for(int n = n0; ; n++){
    int sx0 = FUT_CX + (n * sw - (int)futCam);
    if(sx0 > LW) break;
    int a = sx0 < 0 ? 0 : sx0, b = sx0 + sw; if(b > LW) b = LW;
    if(b > a){ uint16_t g = (n & 1) ? rgb565(44,164,60) : rgb565(33,138,48);
               geoFillL(bbuf, a, FUT_HORIZON, b - a, LH - FUT_HORIZON, g); }
  }
  futDrawMarkings();
}

// ---- Un jugador (por indice; sin pasar el struct como parametro) ----
static void futDrawPlayer(int i){
  int d = futDIdx(futP[i].y);
  int sx = futGX(futP[i].x, futP[i].y);
  int fy = futSyLUT[d], ph = futPHt[d];
  if(sx < -30 || sx > LW + 30) return;
  int tid = futTeamSel[futP[i].team];
  uint16_t colA = FUT_TEAMS[tid].colorA, colB = FUT_TEAMS[tid].colorB;
  if(futP[i].role == FR_GK){ colA = rgb565(40,220,130); colB = rgb565(18,40,30); }   // portero distinto
  futShadow(sx, fy + 1, ph * 42 / 100 + 3, ph * 15 / 100 + 2, rgb565(12,32,14), 95);
  int legH = ph * 30 / 100, bodyH = ph * 30 / 100, shortH = ph * 22 / 100;
  int bw = ph * 46 / 100; if(bw < 4) bw = 4;
  int legW = ph * 12 / 100; if(legW < 1) legW = 1;
  int headR = ph * 16 / 100; if(headR < 2) headR = 2;
  int topBody = fy - legH - shortH - bodyH;
  fillRect(sx - legW - 1, fy - legH, legW, legH, rgb565(28,30,40));           // piernas
  fillRect(sx + 1, fy - legH, legW, legH, rgb565(28,30,40));
  fillRect(sx - bw / 2, fy - legH - shortH, bw, shortH, colB);                // short
  fillRect(sx - bw / 2, topBody, bw, bodyH, colA);                            // camiseta
  drawRect(sx - bw / 2, topBody, bw, bodyH, mix565(colA, rgb565(0,0,0), 90));
  fillCircle(sx, topBody - headR + 1, headR, rgb565(226,182,142));            // cabeza
  if(i == futCtrl && futP[i].team == 0){                                      // indicador "1UP" (flecha)
    int ay = topBody - headR * 2 - 6 - ((millis() / 300) % 2) * 3;
    fillTriangle(sx - 7, ay - 8, sx + 7, ay - 8, sx, ay, rgb565(245,60,60));
    drawTextC(sx, ay - 20, "1UP", 1, rgb565(245,240,120));
  }
}
static void futDrawBall(){
  int d = futDIdx(futBy);
  int sx = futGX(futBx, futBy), fy = futSyLUT[d];
  int r = futPHt[d] * 13 / 100; if(r < 2) r = 2;
  int zoff = (int)(futBz * (futPHt[d] / 42.0f));
  futShadow(sx, fy + 1, r + 1, r / 2 + 1, rgb565(12,32,14), 80);
  fillCircle(sx, fy - r - zoff, r, rgb565(245,245,248));
  drawCircle(sx, fy - r - zoff, r, rgb565(60,64,70));
}

// ---- HUD arcade: marcador arriba, tiempo al centro, nombres ----
static void futFmtClock(char* out, int n){
  int gs = (int)futPlaySec; if(gs > 5400) gs = 5400;
  snprintf(out, n, "%02d:%02d", gs / 60, gs % 60);
}
static void futDrawHUD(){
  fillRectA(0, 0, LW, 46, rgb565(12,14,26), 214);
  hLineA(0, 46, LW, rgb565(0,0,0), 120);
  // Boton salir / pausa (esquina superior izquierda)
  fillRoundRect(6, 6, 34, 22, 6, rgb565(78,86,126));
  fillRect(15, 10, 4, 14, rgb565(240,242,248)); fillRect(23, 10, 4, 14, rgb565(240,242,248));
  int idA = futTeamSel[0], idB = futTeamSel[1];
  uint16_t gold = rgb565(250,214,90), txt = rgb565(238,242,250);
  // Izquierda: escudo + nombre + marcador A
  futCrest(62, 24, 24, 28, FUT_TEAMS[idA].colorA, FUT_TEAMS[idA].colorB);
  drawText(80, 6, FUT_TEAMS[idA].nombre, 1, txt);
  char s[8]; snprintf(s, sizeof(s), "%d", futScoreA); drawText(80, 18, s, 3, gold);
  // Derecha: escudo + nombre + marcador B
  futCrest(LW - 62, 24, 24, 28, FUT_TEAMS[idB].colorA, FUT_TEAMS[idB].colorB);
  drawTextR(LW - 80, 6, FUT_TEAMS[idB].nombre, 1, txt);
  snprintf(s, sizeof(s), "%d", futScoreB); drawTextR(LW - 80, 18, s, 3, gold);
  // Centro: cronometro (tiempo de juego 0'..90')
  char tm[8]; futFmtClock(tm, sizeof(tm));
  drawTextC(LW / 2, 6, tm, 3, gold);
}

// ---- Controles virtuales (stick + botones), estilo del sistema ----
static void futDrawBtn(int id, const char* label, bool active, uint16_t col){
  int cx = FUT_BTNX[id], cy = FUT_BTNY[id];
  fillCircleA(cx, cy, FUT_BTN_R, col, active ? 236 : 150);
  if(uiGlass) fillCircleA(cx, cy - FUT_BTN_R / 3, FUT_BTN_R / 2, rgb565(255,255,255), 46);  // brillo Liquid Glass
  drawCircle(cx, cy, FUT_BTN_R, mix565(col, rgb565(255,255,255), 120));
  drawTextC(cx, cy - 6, label, 1, rgb565(255,255,255));
}
static void futDrawControls(){
  // Stick
  fillCircleA(FUT_STK_CX, FUT_STK_CY, FUT_STK_R, rgb565(20,24,42), 120);
  drawCircle(FUT_STK_CX, FUT_STK_CY, FUT_STK_R, rgb565(150,168,206));
  if(uiGlass) fillCircleA(FUT_STK_CX, FUT_STK_CY - FUT_STK_R / 3, FUT_STK_R / 2, rgb565(255,255,255), 30);
  int kx = FUT_STK_CX + futStkKnobX, ky = FUT_STK_CY + futStkKnobY;
  fillCircleA(kx, ky, 26, rgb565(184,204,244), 210);
  drawCircle(kx, ky, 26, rgb565(232,240,255));
  // Botones
  futDrawBtn(FBT_SPR, "SPR", futSprint, rgb565(90,200,120));
  futDrawBtn(FBT_PAS, "PAS", false,     rgb565(90,150,240));
  futDrawBtn(FBT_TIR, "TIR", false,     rgb565(240,92,92));
  futDrawBtn(FBT_CAM, "CAM", false,     rgb565(240,200,84));
}

// ---- Composicion de un frame de PARTIDO (en bbuf) + volcado ----
static void futRenderPlay(){
  gLand = true; setBuf(bbuf);
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  futDrawField();
  // Orden por profundidad (wy ascendente = lejos primero) para que los
  // cercanos tapen a los lejanos. Insercion sobre 22 indices: barato.
  int order[FUT_NP];
  for(int i = 0; i < FUT_NP; i++) order[i] = i;
  for(int a = 1; a < FUT_NP; a++){
    int v = order[a]; float vy = futP[v].y; int b = a - 1;
    while(b >= 0 && futP[order[b]].y > vy){ order[b + 1] = order[b]; b--; }
    order[b + 1] = v;
  }
  bool ballDrawn = false;
  for(int a = 0; a < FUT_NP; a++){
    if(!ballDrawn && futBy <= futP[order[a]].y){ futDrawBall(); ballDrawn = true; }
    futDrawPlayer(order[a]);
  }
  if(!ballDrawn) futDrawBall();
  futDrawHUD();
  futDrawControls();
  if(millis() < futFreezeUntil && futFlash[0]){
    drawTextC(LW / 2, FUT_HORIZON + 30, futFlash, 6, rgb565(250,230,90));
    int sc = (futKickNext == 1) ? 0 : 1;                            // marco quien acaba de marcar
    drawTextC(LW / 2, FUT_HORIZON + 92, FUT_TEAMS[futTeamSel[sc]].nombre, 3, rgb565(240,244,252));
  }
  present(0, SCR_H - 1);
}

// ---- Lectura del stick + acciones (un solo dedo) ----
static void futReadStick(int lx, int ly){
  bool stick = T.down && lx < 380 && ly > 190;
  if(stick){
    float ox = lx - FUT_STK_CX, oy = ly - FUT_STK_CY, d = sqrtf(ox * ox + oy * oy);
    if(d > FUT_STK_DEAD){
      float dd = (d > FUT_STK_R) ? FUT_STK_R : d;
      float mx = ox / d, my = oy / d;
      float mag = (dd - FUT_STK_DEAD) / (FUT_STK_R - FUT_STK_DEAD);
      futMoveX = mx * mag; futMoveY = my * mag;
      futFaceX = mx; futFaceY = my;
      futStkKnobX = (int)(mx * dd); futStkKnobY = (int)(my * dd);
      return;
    }
  }
  futMoveX = futMoveY = 0; futStkKnobX = 0; futStkKnobY = 0;
}

// ---- Tick del partido: throttle + fisica con substep + render ----
static void futPlayTick(){
  uint32_t now = millis();
  int lx = geoLX(), ly = geoLY();
  // Salir a la seleccion de equipo -- en TODOS los ticks (antes del throttle).
  if(T.tap && lx < 46 && ly < 30){ futScreen = FS_TEAM; futRenderTeamSel(false); return; }
  bool frozen = (now < futFreezeUntil);
  // Latcheo de taps de boton (transitorios) antes del throttle, para no perderlos.
  if(T.tap && !frozen){
    for(int b = 0; b < 4; b++){
      int dx = lx - FUT_BTNX[b], dy = ly - FUT_BTNY[b];
      if(dx * dx + dy * dy <= (FUT_BTN_R + 7) * (FUT_BTN_R + 7)){
        if(b == FBT_SPR) futReqSprint = true; else if(b == FBT_PAS) futReqPass = true;
        else if(b == FBT_TIR) futReqShoot = true; else if(b == FBT_CAM) futReqSwitch = true;
        break;
      }
    }
  }
  if(now - futFrameMs < GEO_FRAME_MS) return;                       // ~30 fps
  float dt = (now - futFrameMs) / 1000.0f; futFrameMs = now;
  if(dt > GEO_DTMAX) dt = GEO_DTMAX;
  futDt = dt;
  if(!frozen && futPending && now >= futFreezeUntil){ futPending = false; futResetFormation(futKickNext); futFlash[0] = 0; }
  if(!frozen){
    futReadStick(lx, ly);
    if(futReqSprint){ futSprint = !futSprint; futReqSprint = false; }
    if(futReqPass){ futDoPass(); futReqPass = false; }
    if(futReqShoot){ futDoShoot(); futReqShoot = false; }
    if(futReqSwitch){ futDoSwitch(); futReqSwitch = false; }
    // Velocidad del jugador controlado desde el stick
    float spd = FUT_BASE_SPD * (futSprint ? FUT_SPRINT_MUL : 1.0f);
    futP[futCtrl].vx = futMoveX * spd; futP[futCtrl].vy = futMoveY * spd;
    futAI();
    float remain = dt;
    while(remain > 0.0001f){ float s = (remain > GEO_SUBSTEP) ? GEO_SUBSTEP : remain; futStep(s); remain -= s; }
    futCam += (futBx - futCam) * (dt * 3.0f > 1 ? 1 : dt * 3.0f);
    if(futCam < FUT_CX) futCam = FUT_CX; if(futCam > FUT_LEN - FUT_CX) futCam = FUT_LEN - FUT_CX;
    futPlaySec += dt * (5400.0f / FUT_MATCH_SEC);
    if(futPlaySec >= 5400){ futPlaySec = 5400; futScreen = FS_END; futRenderEnd(); return; }
  } else {
    futMoveX = futMoveY = 0; futStkKnobX = futStkKnobY = 0;
  }
  futRenderPlay();
}

// #############################################################
// ##  SELECTOR DE EQUIPO  (tu equipo -> rival -> saque)
// #############################################################
static void futDrawTeamCell(int x, int y, int w, int h, int idx, bool hl){
  uint16_t a = FUT_TEAMS[idx].colorA, b = FUT_TEAMS[idx].colorB;
  fillRoundRect(x, y, w, h, 8, mix565(rgb565(20,24,42), a, 55));
  fillRect(x + 8, y + 8, 16, 18, a); fillRect(x + 24, y + 8, 9, 18, b);
  drawRect(x + 8, y + 8, 25, 18, rgb565(10,12,20));
  futCrest(x + w - 20, y + 20, 20, 24, a, b);
  drawTextC(x + w / 2, y + h - 30, FUT_TEAMS[idx].nombre, 1, rgb565(236,240,250));
  char rt[6]; snprintf(rt, sizeof(rt), "%d", FUT_TEAMS[idx].rating);
  drawText(x + 8, y + h - 16, rt, 1, rgb565(250,214,92));
  drawRoundRect(x, y, w, h, 8, hl ? rgb565(250,230,90) : mix565(a, rgb565(255,255,255), 46));
}
static void futRenderTeamSel(bool opp){
  gLand = true; setBuf(bbuf);
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  geoFillL(bbuf, 0, 0, LW, LH, rgb565(16,20,36));
  geoFillL(bbuf, 0, 0, LW, 54, rgb565(24,30,54));
  drawTextC(LW / 2, 14, opp ? "ELIGE RIVAL" : "ELIGE TU EQUIPO", 3, rgb565(240,244,252));
  fillRoundRect(10, 12, 44, 28, 8, rgb565(60,66,100));
  if(opp){ strokeSeg(38, 18, 26, 26, 2, rgb565(240,240,240)); strokeSeg(26, 26, 38, 34, 2, rgb565(240,240,240)); }  // <- volver
  else   { strokeSeg(24, 18, 40, 34, 2, rgb565(240,240,240)); strokeSeg(40, 18, 24, 34, 2, rgb565(240,240,240)); }  // X salir
  int cols = 7, cw = 106, ch = 76, x0 = 18, y0 = 70, gx = 2, gy = 6;
  for(int i = 0; i < FUT_NTEAMS; i++){
    int c = i % cols, r = i / cols;
    int x = x0 + c * (cw + gx), y = y0 + r * (ch + gy);
    bool hl = opp ? (i == futTeamSel[1]) : (i == futTeamSel[0]);
    futDrawTeamCell(x, y, cw - 4, ch - 4, i, hl);
  }
  drawTextC(LW / 2, LH - 16, opp ? "Toca el equipo rival" : "Toca tu equipo (se guarda)", 1, rgb565(160,170,190));
  present(0, SCR_H - 1);
}
static void futStartMatch(){
  futScoreA = 0; futScoreB = 0; futPlaySec = 0; futSprint = false;
  futReqPass = futReqShoot = futReqSwitch = futReqSprint = false;
  futPending = false; futFlash[0] = 0;
  futResetFormation(0);                                            // saca el jugador (equipo 0)
  futFreezeUntil = millis() + 800;                                 // breve pausa de saque
  futFrameMs = millis();
  futScreen = FS_PLAY;
  futRenderPlay();
}
static void futTeamSelTick(bool opp){
  if(!T.tap) return;
  int lx = geoLX(), ly = geoLY();
  if(lx >= 10 && lx <= 54 && ly >= 12 && ly <= 40){
    if(opp){ futScreen = FS_TEAM; futRenderTeamSel(false); }
    else   { gGameMode = GM_MENU; gamesRenderMenu(); }             // salir del futbol al menu de juegos
    return;
  }
  int cols = 7, cw = 106, ch = 76, x0 = 18, y0 = 70, gx = 2, gy = 6;
  if(ly < y0 || lx < x0) return;
  int c = (lx - x0) / (cw + gx), r = (ly - y0) / (ch + gy);
  if(c < 0 || c >= cols || r < 0) return;
  int idx = r * cols + c; if(idx >= FUT_NTEAMS) return;
  int cx = x0 + c * (cw + gx), cy = y0 + r * (ch + gy);
  if(lx > cx + cw - 4 || ly > cy + ch - 4) return;                 // en la separacion
  if(!opp){ futTeamSel[0] = idx; futSaveTeam(idx); futScreen = FS_OPP; futRenderTeamSel(true); }
  else    { futTeamSel[1] = idx; futStartMatch(); }
}

// ---- Pantalla final del partido ----
static void futRenderEnd(){
  gLand = true; setBuf(bbuf);
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  geoFillL(bbuf, 0, 0, LW, LH, rgb565(10,14,28));
  drawTextC(LW / 2, 78, "FINAL", 5, rgb565(250,230,90));
  char sc[48];
  snprintf(sc, sizeof(sc), "%s  %d - %d  %s",
           FUT_TEAMS[futTeamSel[0]].nombre, futScoreA, futScoreB, FUT_TEAMS[futTeamSel[1]].nombre);
  drawTextC(LW / 2, 190, sc, 3, rgb565(240,244,252));
  const char* res = (futScoreA > futScoreB) ? "\xC2\xA1Ganaste!" : (futScoreA < futScoreB ? "Perdiste" : "Empate");
  drawTextC(LW / 2, 256, res, 3, (futScoreA > futScoreB) ? rgb565(120,240,140) : rgb565(240,180,180));
  drawTextC(LW / 2, 366, "Toca para continuar", 2, rgb565(170,180,200));
  present(0, SCR_H - 1);
}
static void futEndTick(){
  if(T.tap){ futScreen = FS_TEAM; futRenderTeamSel(false); }
}

// ---- NVS: ultimo equipo elegido (namespace "flexos", clave "fut_team") ----
static void futLoadTeam(){
  prefs.begin("flexos", true);
  int v = prefs.getInt("fut_team", 0);
  prefs.end();
  if(v < 0 || v >= FUT_NTEAMS) v = 0;
  futTeamSel[0] = v;
}
static void futSaveTeam(int idx){
  if(idx < 0 || idx >= FUT_NTEAMS) return;
  prefs.begin("flexos", false);
  prefs.putInt("fut_team", idx);
  prefs.end();
}

// ---- Entrada al modo Futbol (desde el menu de juegos) ----
static void futEnter(){
  gGameMode = GM_FUT;
  if(!futLUTready) futBuildLUT();
  geoRngState = 0x9e3779b9u ^ millis();
  futLoadTeam();
  if(futTeamSel[1] == futTeamSel[0]) futTeamSel[1] = (futTeamSel[0] + 1) % FUT_NTEAMS;
  futScreen = FS_TEAM;
  futRenderTeamSel(false);
}
// ---- Router del modo Futbol ----
static void futTick(){
  if(futScreen == FS_TEAM){ futTeamSelTick(false); return; }
  if(futScreen == FS_OPP){  futTeamSelTick(true);  return; }
  if(futScreen == FS_END){  futEndTick();          return; }
  futPlayTick();
}

// #############################################################
// ##  MENU DE JUEGOS  (Geometry Dash / Futbol)
// #############################################################
static void gamesMenuCard(int x, int y, int w, int h, uint16_t accent, const char* label, int kind){
  fillRoundRectA(x, y, w, h, 20, mix565(rgb565(18,22,40), accent, 70), 235);
  if(uiGlass) fillRoundRectA(x + 6, y + 6, w - 12, h / 3, 16, rgb565(255,255,255), 26);
  drawRoundRect(x, y, w, h, 20, mix565(accent, rgb565(255,255,255), 90));
  int icx = x + w / 2, icy = y + h / 2 - 24;
  if(kind == 0){                                                   // icono Geo Dash: cubo con "ojos"
    int s = 46;
    fillRoundRect(icx - s, icy - s, 2 * s, 2 * s, 10, accent);
    fillRoundRect(icx - s + 6, icy - s + 6, 2 * s - 12, 2 * s - 12, 8, mix565(accent, rgb565(0,0,0), 90));
    fillRect(icx - 20, icy - 8, 12, 16, rgb565(240,244,252));
    fillRect(icx + 8, icy - 8, 12, 16, rgb565(240,244,252));
  } else {                                                         // icono Futbol: balon + cesped
    fillRoundRect(icx - 52, icy + 30, 104, 16, 6, rgb565(40,160,60));
    fillCircle(icx, icy, 40, rgb565(245,245,248));
    drawCircle(icx, icy, 40, rgb565(50,54,60));
    fillTriangle(icx, icy - 16, icx - 15, icy + 6, icx + 15, icy + 6, rgb565(30,32,40));  // pentagono
    fillCircle(icx, icy, 4, rgb565(30,32,40));
  }
  drawTextC(icx, y + h - 52, label, 3, rgb565(244,247,252));
}
static void gamesRenderMenu(){
  gLand = true; setBuf(bbuf);
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  geoFillL(bbuf, 0, 0, LW, LH, rgb565(18,22,40));
  geoFillL(bbuf, 0, 0, LW, 58, rgb565(26,32,58));
  drawTextC(LW / 2, 16, "JUEGOS", 4, rgb565(240,244,252));
  fillRoundRect(12, 14, 46, 30, 8, rgb565(58,64,98));              // X salir de la app
  strokeSeg(24, 20, 44, 38, 2, rgb565(240,240,240)); strokeSeg(44, 20, 24, 38, 2, rgb565(240,240,240));
  int cw = 320, ch = 300, gap = 40, y0 = 112;
  int x1 = (LW - (2 * cw + gap)) / 2, x2 = x1 + cw + gap;
  gamesMenuCard(x1, y0, cw, ch, rgb565(70,150,240), "Geometry Dash", 0);
  gamesMenuCard(x2, y0, cw, ch, rgb565(40,170,80),  "Futbol", 1);
  drawTextC(LW / 2, LH - 22, "Toca un juego para empezar", 2, rgb565(160,170,190));
  present(0, SCR_H - 1);
}
// Cierra la app "Juegos" -> Home. NO toca gLand: lo hace appClose(), que es
// quien lo tiene documentado como responsabilidad del framework. Ponerlo a false
// aqui antes de llamar hacia que appClose viera wasLand=false y guardara una
// miniatura GIRADA en Recientes (el caso exacto que avisa su comentario), y en
// Modo Kiosco, donde appClose se niega a cerrar, dejaba el motor en portrait
// mientras el juego seguia dibujando en landscape.
static void gamesExitApp(){ appClose(); }
static void gamesEnterGeo(){                                       // abre Geo Dash (motor original, intacto)
  gGameMode = GM_GEO;
  geoLoadProgress();
  gCurLevel = 0;
  geoEnterSelect();
}
static void gamesMenuTick(){
  if(!T.tap) return;
  int lx = geoLX(), ly = geoLY();
  if(lx >= 12 && lx <= 58 && ly >= 14 && ly <= 44){ gamesExitApp(); return; }
  int cw = 320, ch = 300, gap = 40, y0 = 112;
  int x1 = (LW - (2 * cw + gap)) / 2, x2 = x1 + cw + gap;
  if(ly >= y0 && ly <= y0 + ch){
    if(lx >= x1 && lx <= x1 + cw){ gamesEnterGeo(); return; }
    if(lx >= x2 && lx <= x2 + cw){ futEnter(); return; }
  }
}

// ---- Salir del selector de Geo Dash: vuelve al menu de juegos ----
static void geoExit(){ gGameMode = GM_MENU; gamesRenderMenu(); }

// ---- Empezar un nivel (Normal o Practica) ----
static void geoStartLevel(int idx, bool practice){
  geoBindLevel(idx);
  gPractice = practice;
  geoRngState = 0x9e3779b9u ^ millis();
  geoAttempts = 1;
  geoState = GEO_PLAY;
  geoResetLevel();
  geoCPset = false;
  geoNextCP = gLen * 0.22f;                     // primer checkpoint ~22%
  geoDownPrev = false; geoHeldLatch = false; geoTapLatch = false;
  gScreen = GS_GAME;
  geoRenderGame();
}

// ---- Volver al selector (recarga el progreso mostrado) ----
static void geoEnterSelect(){
  gScreen = GS_SELECT;
  geoBindLevel(gCurLevel);
  geoRenderSelect();
}

// ---- Toques del selector ----
static void geoSelectTick(){
  if(!T.tap) return;
  int lx = geoLX(), ly = geoLY();
  int x0 = 168, y0 = 60, w = 464;
  if(lx >= 10 && lx <= 56 && ly >= 8 && ly <= 40){ geoExit(); return; }         // salir de la app
  if(lx >= 28 && lx <= 96  && ly >= 196 && ly <= 288){ gCurLevel = (gCurLevel + 3) % 4; geoRenderSelect(); return; }  // <
  if(lx >= 704 && lx <= 772 && ly >= 196 && ly <= 288){ gCurLevel = (gCurLevel + 1) % 4; geoRenderSelect(); return; } // >
  // Barra Normal.
  if(lx >= x0 + 44 && lx <= x0 + w - 44 && ly >= y0 + 142 && ly <= y0 + 176){ geoStartLevel(gCurLevel, false); return; }
  // Barra Practica.
  if(lx >= x0 + 44 && lx <= x0 + w - 44 && ly >= y0 + 226 && ly <= y0 + 260){ geoStartLevel(gCurLevel, true); return; }
}

// ---- Tick de JUEGO (throttle + fisica con substep + render) ----
static void geoGameTick(){
  uint32_t now = millis();
  int lx = geoLX(), ly = geoLY();
  // Salir al selector -- en TODOS los ticks (antes del throttle) para no perderlo.
  if(T.tap && lx >= 10 && lx <= 56 && ly >= 6 && ly <= 36){ geoEnterSelect(); return; }
  // Reiniciar checkpoints (solo Practica): reinicia el nivel desde 0%.
  if(gPractice && T.tap && lx >= LW - 132 && lx <= LW - 10 && ly >= 6 && ly <= 36){
    geoCPset = false; geoNextCP = gLen * 0.22f;
    geoAttempts = 1; geoResetLevel(); geoState = GEO_PLAY;
    geoRenderGame(); return;
  }
  // Latcheo de input de juego (zona bajo el HUD): held sostenido + flanco de tap.
  bool inPlay = T.down && ly > GEO_HUD_H;
  if(inPlay) geoHeldLatch = true;
  if(inPlay && !geoDownPrev) geoTapLatch = true;
  geoDownPrev = T.down;

  // Throttle: no avanzar fisica/render mas rapido que ~GEO_FRAME_MS.
  if(now - geoFrameMs < GEO_FRAME_MS) return;
  float dt = (now - geoFrameMs) / 1000.0f;
  geoFrameMs = now;
  if(dt > GEO_DTMAX) dt = GEO_DTMAX;

  bool held = geoHeldLatch, tapEdge = geoTapLatch;
  geoHeldLatch = false; geoTapLatch = false;

  if(geoState == GEO_PLAY){
    // Efectos de "un solo tap" (una vez por frame, fuera del substep).
    if(tapEdge){
      if(gForma == FRM_BALL) geoGravDir = -geoGravDir;                       // invertir gravedad
      else if(gForma == FRM_UFO){ geoVelY = -geoGravDir * GEO_UFO_IMP; geoGrounded = false; }  // salto corto
    }
    // Substepping: fisica en pasos fijos <= GEO_SUBSTEP (sin tuneles a cualquier FPS).
    float remain = dt;
    while(remain > 0.0001f && geoState == GEO_PLAY){
      float step = remain > GEO_SUBSTEP ? GEO_SUBSTEP : remain;
      geoUpdate(step, held);
      remain -= step;
    }
    // Checkpoint automatico en Practica (~cada 22% de avance).
    if(gPractice && geoState == GEO_PLAY && geoScroll >= geoNextCP){
      geoSaveCP();
      geoNextCP += gLen * 0.22f;
    }
  } else if(geoState == GEO_DEAD){
    geoUpdateParticles(dt);
    if(now - geoDeadMs > GEO_RESPAWN_MS){
      geoAttempts++;
      if(gPractice && geoCPset){ geoRestoreCP(); geoState = GEO_PLAY; }        // reaparece en el checkpoint
      else { geoResetLevel(); geoState = GEO_PLAY; }                           // Normal: desde 0%
    }
  } else { // GEO_WIN
    if(T.tap && ly > GEO_HUD_H){ geoEnterSelect(); return; }
  }
  geoRenderGame();
}

// ---- Entrada de la app "Juegos": abre el menu de seleccion de juego ----
static void geoEnter(){
  gLand = true;
  gGameMode = GM_MENU;
  gamesRenderMenu();
}

// ---- Tick de la app: enruta menu / Geo Dash / Futbol ----
static void geoTick(){
  if(gGameMode == GM_MENU){ gamesMenuTick(); return; }
  if(gGameMode == GM_FUT){ futTick(); return; }
  // GM_GEO -> motor original de Geometry Dash (intacto)
  if(gScreen == GS_SELECT){ geoSelectTick(); return; }
  geoGameTick();
}
// NAVEGADOR · adaptativo.
//   Esencial   : barra de direcciones + estado de conexion.
//   Opcional 1 : barra de pestanas -- aparece cuando el lienzo pasa de 360 px
//                de ancho (una pestana legible necesita >= 110 px y queremos
//                al menos tres).
//   Opcional 2 : globo/ilustracion -- aparece solo si quedan >= 120 px libres
//                bajo la barra; por debajo de eso se omite entera en vez de
//                encogerla hasta no distinguirse.
static void navEnter(){
  setBuf(fb);
  int bx, by, bw, bh; uiBox(bx, by, bw, bh);
  fillRect(bx, by, bw, bh, WIN_BG);
  int pad = uiPad(), gap = uiGap();
  int y = by + pad;
  int fsT = uiFontFit("Navegador", bw - 2 * pad, uiFontH(bh / 12));
  drawTextC(bx + bw / 2, y, "Navegador", fsT, rgb565(255,255,255));
  y += uiLineH(fsT) + gap;
  uint8_t aTabs = uiSection(0, bw >= 360);
  if(aTabs){
    int nt = 3, tw = (bw - 2 * pad - (nt - 1) * gap) / nt;
    int thh = bh / 14; if(thh < 20) thh = 20; if(thh > 34) thh = 34;
    const char* tabs[3] = { "Inicio", "Marcadores", "Historial" };
    for(int i = 0; i < nt; i++){
      int x = bx + pad + i * (tw + gap);
      uiRectA(x, y, tw, thh, thh / 3, i == 0 ? rgb565(58,86,150) : rgb565(34,38,50), aTabs);
      uiTextC(x + tw / 2, y + thh / 2 - uiLineH(2) / 2, tabs[i],
              uiFontFit(tabs[i], tw - 10, 2), rgb565(225,232,245), aTabs);
    }
    y += thh + gap;
  }
  int barH = bh / 10; if(barH < 26) barH = 26; if(barH > 48) barH = 48;
  fillRoundRect(bx + pad, y, bw - 2 * pad, barH, barH / 3, rgb565(240,242,248));
  drawText(bx + pad * 2, y + barH / 2 - uiLineH(2) / 2, "https://",
           uiFontFit("https://", bw - 4 * pad, 2), rgb565(120,126,140));
  y += barH + gap;
  int rest = (by + bh) - y - pad;
  uint8_t aGlobe = uiSection(1, rest >= 120);
  int cy;
  if(aGlobe){
    int rr = (rest - 40) / 2; if(rr > bw / 4) rr = bw / 4; if(rr > 70) rr = 70;
    cy = y + rr + 6;
    uint16_t gc = mix565(WIN_BG, rgb565(80,120,200), aGlobe);
    drawCircle(bx + bw / 2, cy, rr, gc); drawCircle(bx + bw / 2, cy, rr - 1, gc);
    vLine(bx + bw / 2, cy - rr, 2 * rr, gc);
    hLine(bx + bw / 2 - rr, cy, 2 * rr, gc);
    y = cy + rr + gap;
  }
  const char* st = "Sin conexi\xC3\xB3n - modo offline";
  int fy = by + bh - pad - uiLineH(2);
  if(fy < y) fy = y;
  drawTextC(bx + bw / 2, fy, st, uiFontFit(st, bw - 2 * pad, 2), rgb565(160,168,188));
  flxFlush(WIN_TOP, WIN_BOT);
}
// #############################################################
// ##  ASISTENTE DE HARDWARE  (FASE 3, dentro de Code IDE)
// ##  ------------------------------------------------------
// ##  Panel de SOLO LECTURA (no se refactoriza el editor, que
// ##  hoy es una demo). Lista los modulos I2C detectados en la
// ##  Fase 2 (detectedModules[]) y, al elegir uno, genera el
// ##  codigo de inicializacion y lo muestra como texto para
// ##  copiar al portapapeles global (clipboard[]). Se dibuja
// ##  a fb con flxFlush una sola vez por cambio de estado
// ##  (nada lo redibuja por frame durante ST_APP con el IDE),
// ##  asi que no hay parpadeo ni conflicto con el presenter.
// #############################################################

// Estado del asistente
static bool  hwWizardActive = false;   // panel abierto
static int   hwSelModule    = -1;      // -1 = lista; >=0 = indice en detectedModules (vista codigo)
static bool  hwCopied       = false;   // feedback del boton "Copiar"
static char  hwCode[512]    = "";      // codigo generado (tambien va al portapapeles)

// Geometria: boton del editor
#define HW_BTN_X   (SCR_W / 2 - 140)
#define HW_BTN_Y   (WIN_BOT - 104)
#define HW_BTN_W   280
#define HW_BTN_H   46
// Geometria: lista de modulos
#define HW_LIST_Y0 (WIN_TOP + 56)
#define HW_ROW_H   56
#define HW_CARD_X  24
#define HW_CARD_W  (SCR_W - 48)
#define HW_CARD_H  48
// Geometria: boton "Cerrar" (vista lista)
#define HW_CLOSE_X (SCR_W / 2 - 80)
#define HW_CLOSE_Y (WIN_BOT - 60)
#define HW_CLOSE_W 160
#define HW_CLOSE_H 44
// Geometria: botones "Volver"/"Copiar" (vista codigo)
#define HW_BACK_X  24
#define HW_COPY_X  (SCR_W - 24 - 150)
#define HW_ACT_Y   (WIN_BOT - 60)
#define HW_ACT_W   150
#define HW_ACT_H   44

// drawModuleIcon() se define mas abajo (bloque de la isla); forward-decl para
// poder reutilizar el mismo mapeo tipo->icono aqui.
static void drawModuleIcon(ModuleType type, int x, int y, int S);

// CODE IDE · adaptativo.
//   Esencial   : listado de codigo con numeros de linea + boton del Asistente.
//   Opcional 1 : minimapa / panel de simbolos a la derecha -- aparece cuando el
//                lienzo pasa de 470 px de ancho (el codigo necesita >= 300 px
//                para no cortarse y el panel >= 140 px para leerse).
//   Opcional 2 : pie de aclaracion -- aparece si sobran >= 16 px.
static void ideEnter(){
  setBuf(fb);
  int bx, by, bw, bh; uiBox(bx, by, bw, bh);
  fillRect(bx, by, bw, bh, rgb565(20,22,30));
  int pad = uiPad(), gap = uiGap();
  int y = by + pad;
  int fsT = uiFontFit("Code IDE", bw - 2 * pad, uiFontH(bh / 12));
  drawTextC(bx + bw / 2, y, "Code IDE", fsT, rgb565(255,255,255));
  y += uiLineH(fsT) + gap;
  int sideW = bw / 4; if(sideW > 190) sideW = 190;
  uint8_t aSide = uiSection(0, bw >= 470);
  int codeW = aSide ? (bw - 2 * pad - sideW - gap) : (bw - 2 * pad);
  const char* code[8] = {
    "#include <FlexOS.h>", "", "void setup() {", "  screen.begin();",
    "  ui.drawHome();", "}", "", "void loop() { ui.tick(); }" };
  int btnH = bh / 9; if(btnH < 30) btnH = 30; if(btnH > 50) btnH = 50;
  int codeBot = by + bh - pad - btnH - gap;
  int lineH = (codeBot - y) / 8; if(lineH < 9) lineH = 9;
  int fsC = lineH >= 20 ? 2 : 1;
  int gut = textW("88", fsC) + 8;
  for(int i = 0; i < 8; i++){
    int ly = y + i * lineH;
    if(ly + lineH > codeBot) break;
    char ln[8]; snprintf(ln, sizeof(ln), "%2d", i + 1);
    drawText(bx + pad, ly, ln, fsC, rgb565(90,96,112));
    drawText(bx + pad + gut, ly, code[i], uiFontFit(code[i], codeW - gut, fsC), rgb565(150,220,180));
  }
  if(aSide){
    int sx = bx + pad + codeW + gap;
    uiRectA(sx, y, sideW, codeBot - y, pad, rgb565(26,29,40), aSide);
    uiTextC(sx + sideW / 2, y + pad, "Simbolos", uiFontFit("Simbolos", sideW - 12, 2), rgb565(150,160,190), aSide);
    const char* sym[3] = { "setup()", "loop()", "FlexOS.h" };
    int syy = y + pad + uiLineH(2) + gap;
    for(int i = 0; i < 3; i++){
      uiText(sx + pad, syy, sym[i], uiFontFit(sym[i], sideW - 2 * pad, 2), rgb565(200,215,235), aSide);
      syy += uiLineH(2) + 6;
    }
  }
  hwWizardActive = false; hwSelModule = -1; hwCopied = false;
  int bwn = bw - 2 * pad; if(bwn > 320) bwn = 320;
  int bxn = bx + (bw - bwn) / 2, byn = by + bh - pad - btnH;
  fillRoundRect(bxn, byn, bwn, btnH, btnH / 4, rgb565(60,110,235));
  drawTextC(bx + bw / 2, byn + btnH / 2 - uiLineH(2), "Asistente de Hardware",
            uiFontFit("Asistente de Hardware", bwn - 12, 3), rgb565(255,255,255));
  flxFlush(WIN_TOP, WIN_BOT);
}

// Indices de los modulos activos (para mapear filas de la lista <-> detectedModules)
static int hwActiveList(int* out, int maxn){
  int c = 0;
  for(int i = 0; i < detectedCount && c < maxn; i++)
    if(detectedModules[i].active) out[c++] = i;
  return c;
}

// Genera el codigo de inicializacion del modulo en hwCode[] (char[] + snprintf)
static void hwGenCode(const DetectedModule* m){
  size_t n = 0;
  hwCode[0] = 0;
  #define HWCAT(...) do{ int _w = snprintf(hwCode + n, sizeof(hwCode) - n, __VA_ARGS__); \
                         if(_w > 0){ n += (size_t)_w; if(n >= sizeof(hwCode)) n = sizeof(hwCode) - 1; } }while(0)
  HWCAT("// Inicializacion automatica\n");
  HWCAT("// Modulo: %s", m->name);
  if(m->i2cAddr) HWCAT(" (0x%02X)", m->i2cAddr);
  HWCAT("\n#include <Wire.h>\n\n");
  HWCAT("void setup() {\n");
  HWCAT("  Wire.begin(7, 8);   // SDA=7 SCL=8\n");
  if(m->i2cAddr){
    HWCAT("  Wire.beginTransmission(0x%02X);\n", m->i2cAddr);
    HWCAT("  bool ok = (Wire.endTransmission() == 0);\n");
  }
  switch(m->type){
    case MOD_BME280:  HWCAT("  // Lib sugerida: Adafruit_BME280\n"); break;
    case MOD_MPU6050: HWCAT("  // Lib sugerida: MPU6050 (I2Cdev)\n"); break;
    default: break;
  }
  HWCAT("}\n");
  #undef HWCAT
}

// Dibuja el panel del asistente (una sola vez por cambio de estado)
static void hwDrawWizard(){
  setBuf(fb);
  fillRect(0, WIN_TOP, SCR_W, WIN_BOT - WIN_TOP, WIN_BG);

  if(hwSelModule < 0){
    // ---- Vista LISTA ----
    drawTextC(SCR_W / 2, WIN_TOP + 16, "Asistente de Hardware", 3, rgb565(255, 255, 255));
    int idxs[MAX_MODULES_DETECTED];
    int nc = hwActiveList(idxs, MAX_MODULES_DETECTED);
    if(nc == 0){
      drawTextC(SCR_W / 2, WIN_TOP + 120, "No hay modulos I2C detectados", 2, rgb565(170, 178, 196));
      drawTextC(SCR_W / 2, WIN_TOP + 150, "Conecta un sensor al bus (SDA=7, SCL=8)", 1, rgb565(130, 138, 158));
    } else {
      for(int r = 0; r < nc; r++){
        DetectedModule* m = &detectedModules[idxs[r]];
        int cy = HW_LIST_Y0 + r * HW_ROW_H;
        drawLiquidGlassPanel(HW_CARD_X, cy, HW_CARD_W, HW_CARD_H, 12, rgb565(40, 60, 130));
        drawModuleIcon(m->type, HW_CARD_X + 8, cy + 8, 32);
        char label[48];
        if(m->i2cAddr) snprintf(label, sizeof(label), "%s (0x%02X)", m->name, m->i2cAddr);
        else           snprintf(label, sizeof(label), "%s", m->name);
        drawText(HW_CARD_X + 50, cy + 16, label, 2, rgb565(240, 242, 248));
        drawTextC(HW_CARD_X + HW_CARD_W - 46, cy + 18, "Config", 1, rgb565(180, 200, 255));
      }
    }
    fillRoundRect(HW_CLOSE_X, HW_CLOSE_Y, HW_CLOSE_W, HW_CLOSE_H, 14, rgb565(70, 74, 90));
    drawTextC(SCR_W / 2, HW_CLOSE_Y + 14, "Cerrar", 2, rgb565(255, 255, 255));
  } else {
    // ---- Vista CODIGO (solo lectura) ----
    DetectedModule* m = &detectedModules[hwSelModule];
    char t[40]; snprintf(t, sizeof(t), "Codigo: %s", m->name);
    drawTextC(SCR_W / 2, WIN_TOP + 16, t, 2, rgb565(255, 255, 255));

    int px = 16, py = WIN_TOP + 52, pw = SCR_W - 32, ph = (HW_ACT_Y - 12) - (WIN_TOP + 52);
    drawLiquidGlassPanel(px, py, pw, ph, 12, rgb565(24, 40, 80));

    // Volcar hwCode linea a linea (split por '\n')
    int ly = py + 12, lineNo = 1;
    const char* p = hwCode;
    char line[80];
    while(*p){
      int li = 0;
      while(*p && *p != '\n' && li < 78){ line[li++] = *p++; }
      line[li] = 0;
      if(*p == '\n') p++;
      char num[6]; snprintf(num, sizeof(num), "%2d", lineNo++);
      drawText(px + 10, ly, num,  1, rgb565(90, 96, 112));
      drawText(px + 38, ly, line, 1, rgb565(150, 220, 180));
      ly += 18;
      if(ly > py + ph - 16) break;
    }

    fillRoundRect(HW_BACK_X, HW_ACT_Y, HW_ACT_W, HW_ACT_H, 12, rgb565(70, 74, 90));
    drawTextC(HW_BACK_X + HW_ACT_W / 2, HW_ACT_Y + 14, "Volver", 2, rgb565(255, 255, 255));
    fillRoundRect(HW_COPY_X, HW_ACT_Y, HW_ACT_W, HW_ACT_H, 12, hwCopied ? rgb565(46, 160, 90) : rgb565(60, 110, 235));
    drawTextC(HW_COPY_X + HW_ACT_W / 2, HW_ACT_Y + 14, hwCopied ? "Copiado" : "Copiar", 2, rgb565(255, 255, 255));
  }
  flxFlush(WIN_TOP, WIN_BOT);
}

// tick del Code IDE: gestiona el boton del editor y los toques del asistente.
// El marco (chevron/atras/nav) lo sigue cerrando el framework -> cierra la app.
static void ideTick(){
  if(hwWizardActive){
    if(!T.tap) return;
    if(hwSelModule < 0){
      // Vista lista: tap en una tarjeta -> generar codigo y pasar a vista codigo
      int idxs[MAX_MODULES_DETECTED];
      int nc = hwActiveList(idxs, MAX_MODULES_DETECTED);
      for(int r = 0; r < nc; r++){
        int cy = HW_LIST_Y0 + r * HW_ROW_H;
        if(T.x >= HW_CARD_X && T.x <= HW_CARD_X + HW_CARD_W && T.y >= cy && T.y <= cy + HW_CARD_H){
          hwSelModule = idxs[r]; hwCopied = false;
          hwGenCode(&detectedModules[hwSelModule]);
          hwDrawWizard();
          return;
        }
      }
      // Cerrar -> volver al editor
      if(T.x >= HW_CLOSE_X && T.x <= HW_CLOSE_X + HW_CLOSE_W && T.y >= HW_CLOSE_Y && T.y <= HW_CLOSE_Y + HW_CLOSE_H){
        hwWizardActive = false;
        ideEnter();
        return;
      }
    } else {
      // Vista codigo: Volver
      if(T.x >= HW_BACK_X && T.x <= HW_BACK_X + HW_ACT_W && T.y >= HW_ACT_Y && T.y <= HW_ACT_Y + HW_ACT_H){
        hwSelModule = -1; hwCopied = false; hwDrawWizard();
        return;
      }
      // Copiar al portapapeles global
      if(T.x >= HW_COPY_X && T.x <= HW_COPY_X + HW_ACT_W && T.y >= HW_ACT_Y && T.y <= HW_ACT_Y + HW_ACT_H){
        strncpy(clipboard, hwCode, sizeof(clipboard) - 1);
        clipboard[sizeof(clipboard) - 1] = 0;
        hwCopied = true; hwDrawWizard();
        return;
      }
    }
    return;
  }
  // Editor: abrir el asistente
  if(T.tap && T.x >= HW_BTN_X && T.x <= HW_BTN_X + HW_BTN_W && T.y >= HW_BTN_Y && T.y <= HW_BTN_Y + HW_BTN_H){
    hwWizardActive = true; hwSelModule = -1; hwCopied = false;
    hwDrawWizard();
  }
}


// #############################################################
// ##  APP PAINT  ·  galeria + lienzo, sobre ficheros REALES
// ##  ------------------------------------------------------
// ##  FORMATO: cada dibujo es un .fxp en /Paint, o sea la lista
// ##  de trazos serializada (ver el bloque de justificacion en
// ##  FlexOS_FS.h). Resumen del porque: un bitmap RGB565 del
// ##  lienzo son ~542 KB por dibujo -- no cabe ni uno en la
// ##  particion de datos del Pro y llena la del Ultra con
// ##  cuatro. Un dibujo por trazos ronda los 3-8 KB, se
// ##  redibuja a cualquier escala (que es exactamente lo que
// ##  necesita la MINIATURA de la galeria: los mismos trazos
// ##  reproducidos mas pequenos, no una imagen aparte) y se
// ##  puede ampliar trazo a trazo sin reescribir el fichero.
// ##
// ##  GUARDADO: al levantar el dedo. No hay boton "guardar" que
// ##  se pueda olvidar, y un apagon solo se lleva el trazo que
// ##  estaba a medias.
// ##
// ##  RENDER: el lienzo NUNCA se repinta entero mientras se
// ##  dibuja. Cada segmento vuelca solo la franja de filas que
// ##  ha tocado (flxFlush(y0, y1)), igual que hacia la version
// ##  anterior de esta app y que el resto del sistema.
// #############################################################
#define P_TOP           96
#define P_BOT           (SCR_H - 66)
#define PAINT_CX        8
#define PAINT_CW        (SCR_W - 16)
#define PAINT_CH        (P_BOT - P_TOP)
#define PAINT_MAX_PTS   512            // puntos por trazo (buffer de trabajo)
#define PAINT_MAX_LIST  16
#define PAINT_COLS       2
#define PAINT_CARD_TOP  118

static const uint16_t P_PAL[6] = { rgb565(30,30,40), rgb565(230,60,60), rgb565(240,150,40),
                                   rgb565(240,210,50), rgb565(80,180,120), rgb565(60,120,235) };
static const uint8_t  P_SIZES[3] = { 3, 6, 12 };

static int       paintView    = 0;                 // 0 = galeria, 1 = lienzo
static FlexFsEntry paintList[PAINT_MAX_LIST];
static int       paintListN   = 0;
static int       paintSelIdx  = -1;
static char      paintPath[FLEXFS_PATH_MAX] = "";
static int       paintScroll  = 0;
static int       paintDragY0 = 0, paintDragS0 = 0;
static bool      paintDragging = false, paintLongFired = false;
static bool      paintMulti   = false;
static uint32_t  paintMask    = 0;

static uint16_t  pColor  = 0;
static int       pSizeIx = 1;
static int16_t*  pStroke = NULL;                   // trazo en curso (PSRAM)
static int       pStrokeN = 0;
static int       pPrevX = -1, pPrevY = -1;

static void paintRenderGallery();
static void paintRenderCanvas();

// Buffer de TRABAJO del trazo en curso. Va a PSRAM cuando la placa la
// tiene (Ultra y Ultra S3): son 2 KB que no hacen falta en la RAM
// interna, que es el recurso escaso. En el Pro, sin PSRAM, cae al heap
// normal -- y si tampoco hubiera, la app lo dice en vez de dibujar sin
// poder guardar.
static void paintBufInit(){
  if(pStroke) return;
  size_t bytes = (size_t)PAINT_MAX_PTS * 2 * sizeof(int16_t);
  if(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0)
    pStroke = (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if(!pStroke) pStroke = (int16_t*)malloc(bytes);
}

// ---- Reproduccion de trazos -------------------------------------------
// El MISMO callback sirve para el lienzo a tamano real y para las
// miniaturas: solo cambia la escala con la que flexPaintReplay entrega
// los segmentos. Por eso una miniatura no puede "no parecerse" al
// dibujo: es el dibujo.
static void paintSegCb(int x0, int y0, int x1, int y1, uint16_t color, int radius, void* user){
  (void)user;
  if(x0 == x1 && y0 == y1) fillCircleAA((float)x0, (float)y0, (float)radius, color);
  else                     strokeSegAA((float)x0, (float)y0, (float)x1, (float)y1, (float)radius, color);
}

static void paintPathOf(int i, char* out, size_t n){
  snprintf(out, n, "%s/%s", FLEXFS_DIR_PAINT, paintList[i].name);
}

static void paintReload(){
  paintListN = flexFsList(FLEXFS_DIR_PAINT, paintList, PAINT_MAX_LIST);
  if(paintScroll > paintListN) paintScroll = 0;
}

// ---- Galeria ----------------------------------------------------------
static void paintCardRect(int i, int &x, int &y, int &w, int &h){
  int col = i % PAINT_COLS, row = i / PAINT_COLS;
  w = (SCR_W - 3 * 16) / PAINT_COLS;
  h = (int)(w * (float)PAINT_CH / (float)PAINT_CW);      // misma proporcion que el lienzo
  if(h > 340) h = 340;
  x = 16 + col * (w + 16);
  y = PAINT_CARD_TOP + row * (h + 46) - paintScroll;
}

static int paintMaxScroll(){
  int x, y, w, h; paintCardRect(0, x, y, w, h);
  int rows = (paintListN + PAINT_COLS - 1) / PAINT_COLS;
  int need = PAINT_CARD_TOP + rows * (h + 46) + 40;
  int m = need - (SCR_H - 60);
  return m > 0 ? m : 0;
}

static void paintFabRect(int &x, int &y, int &r){ r = 52; x = SCR_W - 76; y = SCR_H - 146; }

static void paintRenderGallery(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(158,158,158));
  drawText(16, 22, "Paint", 5, rgb565(16,18,24));
  strokeSegAA(30, 26, 18, 18, 2.4f, rgb565(16,18,24));
  strokeSegAA(18, 18, 30, 10, 2.4f, rgb565(16,18,24));
  for(int i = 0; i < 3; i++) fillCircle(SCR_W - 26, 32 + i * 15, 5, rgb565(16,18,24));

  if(paintListN == 0){
    drawTextC(SCR_W / 2, 320, "No hay dibujos todav\xC3\xAD" "a", 3, rgb565(40,44,54));
    drawTextC(SCR_W / 2, 364, "Pulsa + para crear uno", 1, rgb565(60,64,74));
  }
  for(int i = 0; i < paintListN; i++){
    int x, y, w, h; paintCardRect(i, x, y, w, h);
    if(y + h < 60 || y > SCR_H) continue;
    char title[FLEXFS_NAME_MAX];
    flexFsStem(paintList[i].name, title, sizeof(title));
    drawTextC(x + w / 2, y - 34, title, 2, rgb565(16,18,24));
    fillRoundRect(x, y, w, h, 12, rgb565(255,255,255));

    // MINIATURA REAL: se reproducen los trazos del fichero a escala.
    char p[FLEXFS_PATH_MAX]; paintPathOf(i, p, sizeof(p));
    float sc = (float)(w - 8) / (float)PAINT_CW;
    int savedX0 = gClipX0, savedX1 = gClipX1, savedY0 = gClipY0, savedY1 = gClipY1;
    gClipX0 = x + 4; gClipX1 = x + w - 4; gClipY0 = y + 4; gClipY1 = y + h - 4;
    flexPaintReplay(p, sc, x + 4, y + 4, paintSegCb, NULL);
    gClipX0 = savedX0; gClipX1 = savedX1; gClipY0 = savedY0; gClipY1 = savedY1;

    if(paintMulti && (paintMask & (1UL << i))){
      drawRoundRect(x, y, w, h, 12, rgb565(60,120,235));
      drawRoundRect(x + 1, y + 1, w - 2, h - 2, 11, rgb565(60,120,235));
      fillCircle(x + w - 18, y + 18, 10, rgb565(60,120,235));
      strokeSegAA(x + w - 23, y + 18, x + w - 20, y + 22, 2.2f, rgb565(255,255,255));
      strokeSegAA(x + w - 20, y + 22, x + w - 13, y + 13, 2.2f, rgb565(255,255,255));
    }
  }
  if(paintMulti){
    int by = SCR_H - 128;
    fillRoundRect(12, by, SCR_W - 24, 60, 16, rgb565(30,34,46));
    drawText(28, by + 20, "Selecci\xC3\xB3n", 2, rgb565(240,242,248));
    drawTextR(SCR_W - 140, by + 20, "Papelera", 2, rgb565(250,210,120));
    drawTextR(SCR_W - 28,  by + 20, "Salir", 2, rgb565(180,188,205));
  } else {
    int fx, fy, fr; paintFabRect(fx, fy, fr);
    drawTextR(fx - fr - 6, fy - 34, "Nuevo dibujo", 2, rgb565(16,18,24));
    fillCircle(fx, fy, fr, rgb565(16,18,24));
    fillCircle(fx, fy, fr - 5, rgb565(255,255,255));
    fillRoundRect(fx - 22, fy - 5, 44, 10, 4, rgb565(16,18,24));
    fillRoundRect(fx - 5, fy - 22, 10, 44, 4, rgb565(16,18,24));
  }
  if(fkMenuOn) fkMenuDraw();
  flxFlushAll();
}

// ---- Lienzo -----------------------------------------------------------
static void paintTools(){
  setBuf(fb);
  int y = SCR_H - 58, sw = 34, gap = 6, x0 = 10;
  fillRect(0, P_BOT, SCR_W, SCR_H - P_BOT, rgb565(18,20,28));
  for(int i = 0; i < 6; i++){
    int cx = x0 + i * (sw + gap) + sw / 2;
    fillCircle(cx, y + sw / 2, sw / 2 - 2, P_PAL[i]);
    if(P_PAL[i] == pColor){
      drawCircle(cx, y + sw / 2, sw / 2, rgb565(255,255,255));
      drawCircle(cx, y + sw / 2, sw / 2 - 1, rgb565(255,255,255));
    }
  }
  // Grosor: tres puntos de tamano real (lo que se ve es lo que se pinta).
  int gx = x0 + 6 * (sw + gap) + 8;
  fillRoundRect(gx, y, 44, sw, 8, rgb565(40,44,58));
  fillCircle(gx + 22, y + sw / 2, P_SIZES[pSizeIx], rgb565(240,242,248));
  fillRoundRect(gx + 52, y, 76, sw, 8, rgb565(60,64,78));
  drawTextC(gx + 90, y + 9, "Deshacer", 1, rgb565(240,242,248));
  fillRoundRect(gx + 134, y, 66, sw, 8, rgb565(120,54,54));
  drawTextC(gx + 167, y + 9, "Limpiar", 1, rgb565(255,235,235));
  flxFlush(P_BOT, SCR_H - 1);
}

static void paintRenderCanvas(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, P_TOP, rgb565(16,18,26));
  strokeSegAA(30, 26, 18, 18, 2.4f, rgb565(255,255,255));
  strokeSegAA(18, 18, 30, 10, 2.4f, rgb565(255,255,255));
  char title[FLEXFS_NAME_MAX];
  flexFsStem(paintPath, title, sizeof(title));
  drawTextC(SCR_W / 2, 30, title, 3, rgb565(255,255,255));
  FlexPaintHdr hd;
  if(flexPaintHeader(paintPath, &hd)){
    char sub[48];
    snprintf(sub, sizeof(sub), "%u trazos guardados", (unsigned)hd.strokes);
    drawTextC(SCR_W / 2, 66, sub, 1, rgb565(150,158,178));
  }
  fillRect(PAINT_CX, P_TOP, PAINT_CW, PAINT_CH, rgb565(250,250,252));
  // El lienzo se reconstruye desde el FICHERO, no desde un buffer en RAM:
  // lo que se ve al abrir un dibujo es exactamente lo que hay guardado.
  int sx0 = gClipX0, sx1 = gClipX1, sy0 = gClipY0, sy1 = gClipY1;
  gClipX0 = PAINT_CX; gClipX1 = PAINT_CX + PAINT_CW - 1;
  gClipY0 = P_TOP;    gClipY1 = P_BOT - 1;
  flexPaintReplay(paintPath, 1.0f, PAINT_CX, P_TOP, paintSegCb, NULL);
  gClipX0 = sx0; gClipX1 = sx1; gClipY0 = sy0; gClipY1 = sy1;
  paintTools();
  flxFlushAll();
}

static void paintOpen(int i){
  if(i < 0 || i >= paintListN) return;
  paintPathOf(i, paintPath, sizeof(paintPath));
  paintBufInit();
  pStrokeN = 0; pPrevX = pPrevY = -1;
  paintView = 1;
  paintRenderCanvas();
}

// Nuevo dibujo: crea el FICHERO ya (cabecera .fxp con 0 trazos) con el
// primer numero libre de /Paint, y entra a el. Aparece en la lista
// porque existe en el disco, no porque se haya anadido a un array.
static void paintNew(){
  char full[FLEXFS_PATH_MAX];
  if(!flexFsNewName(FLEXFS_DIR_PAINT, "Dibujo", FLEXFS_EXT_PAINT, full, sizeof(full))) return;
  if(!flexPaintCreate(full, PAINT_CW, PAINT_CH)) return;
  paintReload();
  for(int i = 0; i < paintListN; i++){
    char p[FLEXFS_PATH_MAX]; paintPathOf(i, p, sizeof(p));
    if(!strcmp(p, full)){ paintOpen(i); return; }
  }
  paintRenderGallery();
}

// Cierra el trazo en curso escribiendolo en el fichero.
static void paintFlushStroke(){
  if(pStrokeN > 0 && pStroke && paintPath[0])
    flexPaintAppend(paintPath, pColor, P_SIZES[pSizeIx], pStroke, (uint16_t)pStrokeN);
  pStrokeN = 0; pPrevX = pPrevY = -1;
}

static void paintMenuAction(int act){
  char p[FLEXFS_PATH_MAX];
  if(paintSelIdx >= 0 && paintSelIdx < paintListN) paintPathOf(paintSelIdx, p, sizeof(p));
  else p[0] = 0;
  if(act == FK_ACT_SEL){
    paintMulti = true; paintMask = 0;
    if(paintSelIdx >= 0) paintMask |= (1UL << paintSelIdx);
  } else if(act == FK_ACT_DEL){
    if(p[0]){
      char stem[FLEXFS_NAME_MAX]; flexFsStem(paintList[paintSelIdx].name, stem, sizeof(stem));
      fkAskOpen("\xC2\xBF" "Borrar definitivamente?", stem);
      return;
    }
  } else if(act == FK_ACT_REN){
    if(p[0]){
      char stem[FLEXFS_NAME_MAX]; flexFsStem(paintList[paintSelIdx].name, stem, sizeof(stem));
      fkNameOpen("Renombrar dibujo", stem);
      return;
    }
  } else if(act == FK_ACT_TRASH){
    if(p[0]){ flexFsTrash(p); paintSelIdx = -1; paintReload(); }
    else { fkTrashOpen(); return; }               // sin seleccion: abre la papelera
  }
  paintRenderGallery();
}

static void paintGalleryTick(){
  if(fkTrashOn){ if(!fkTrashTick()){ paintReload(); paintRenderGallery(); } return; }
  if(fkAskOn){
    int r = fkAskTick();
    if(r == 1 && paintSelIdx >= 0 && paintSelIdx < paintListN){
      char p[FLEXFS_PATH_MAX]; paintPathOf(paintSelIdx, p, sizeof(p));
      flexFsDelete(p);
      paintSelIdx = -1; paintReload();
    }
    if(r != 0) paintRenderGallery();
    return;
  }
  if(fkNameOn){
    int r = fkNameTick();
    if(r == 1 && paintSelIdx >= 0 && paintSelIdx < paintListN){
      char p[FLEXFS_PATH_MAX]; paintPathOf(paintSelIdx, p, sizeof(p));
      flexFsRename(p, fkNameBuf);
      paintSelIdx = -1; paintReload();
    }
    if(r != 0) paintRenderGallery();
    return;
  }
  if(fkMenuOn){
    if(T.tap){
      int a = fkMenuHit(T.x, T.y);
      fkMenuOn = false;
      if(a >= 0) paintMenuAction(a);
      else       paintRenderGallery();
    }
    return;
  }

  int maxS = paintMaxScroll();
  if(T.pressed){ paintDragY0 = T.y; paintDragS0 = paintScroll; paintDragging = false; }
  if(T.down && maxS > 0){
    int dy = paintDragY0 - T.y;
    if(!paintDragging && abs(dy) > 8) paintDragging = true;
    if(paintDragging){
      int ns = paintDragS0 + dy;
      if(ns < 0) ns = 0; if(ns > maxS) ns = maxS;
      if(ns != paintScroll){ paintScroll = ns; paintRenderGallery(); }
      paintLongFired = true;
      return;
    }
  }
  if(T.released && paintDragging){ paintDragging = false; return; }

  if(T.down && !paintLongFired && (millis() - T.downMs) > 550
     && abs(T.x - T.startX) < 14 && abs(T.y - T.startY) < 14){
    for(int i = 0; i < paintListN; i++){
      int x, y, w, h; paintCardRect(i, x, y, w, h);
      if(T.startX >= x && T.startX <= x + w && T.startY >= y && T.startY <= y + h){
        paintLongFired = true; paintSelIdx = i;
        fkMenuOpen(T.x, T.y - 40);
        return;
      }
    }
    paintLongFired = true;
  }
  if(!T.down) paintLongFired = false;
  if(!T.tap) return;

  if(T.x > SCR_W - 60 && T.y < 76){ paintSelIdx = -1; fkMenuOpen(SCR_W - 40, 56); return; }
  if(T.x < 60 && T.y < 60){ appClose(); return; }

  if(paintMulti){
    int by = SCR_H - 128;
    if(T.y >= by && T.y <= by + 60){
      if(T.x > SCR_W - 100){ paintMulti = false; paintMask = 0; paintRenderGallery(); return; }
      if(T.x > SCR_W - 230){
        for(int i = 0; i < paintListN; i++) if(paintMask & (1UL << i)){
          char p[FLEXFS_PATH_MAX]; paintPathOf(i, p, sizeof(p));
          flexFsTrash(p);
        }
        paintMulti = false; paintMask = 0; paintReload(); paintRenderGallery(); return;
      }
    }
  } else {
    int fx, fy, fr; paintFabRect(fx, fy, fr);
    long ddx = T.x - fx, ddy = T.y - fy;
    if(ddx * ddx + ddy * ddy <= (long)fr * fr){ paintNew(); return; }
  }
  for(int i = 0; i < paintListN; i++){
    int x, y, w, h; paintCardRect(i, x, y, w, h);
    if(T.x >= x && T.x <= x + w && T.y >= y && T.y <= y + h){
      if(paintMulti){ paintMask ^= (1UL << i); paintRenderGallery(); }
      else paintOpen(i);
      return;
    }
  }
}

static void paintCanvasTick(){
  // ---- Trazo: interpolacion suave y volcado SOLO de la franja tocada ----
  if(T.down && T.y >= P_TOP && T.y < P_BOT && T.x >= PAINT_CX && T.x < PAINT_CX + PAINT_CW){
    int rad = P_SIZES[pSizeIx];
    setBuf(fb);
    int y0, y1;
    if(pPrevX >= 0){
      // Se descartan los puntos casi pegados: el GT911 entrega muestras muy
      // juntas y guardarlas todas engorda el fichero sin cambiar el trazo.
      int ddx = T.x - pPrevX, ddy = T.y - pPrevY;
      if(ddx * ddx + ddy * ddy < 4) return;
      strokeSegAA((float)pPrevX, (float)pPrevY, (float)T.x, (float)T.y, (float)rad, pColor);
      y0 = min(pPrevY, T.y); y1 = max(pPrevY, T.y);
    } else {
      fillCircleAA((float)T.x, (float)T.y, (float)rad, pColor);
      y0 = y1 = T.y;
    }
    if(pStroke && pStrokeN < PAINT_MAX_PTS){
      pStroke[pStrokeN * 2]     = (int16_t)(T.x - PAINT_CX);   // coordenadas del LIENZO
      pStroke[pStrokeN * 2 + 1] = (int16_t)(T.y - P_TOP);
      pStrokeN++;
    } else if(pStroke && pStrokeN >= PAINT_MAX_PTS){
      // Trazo larguisimo: se cierra y se empieza otro, sin perder nada.
      int lx = pPrevX, ly = pPrevY;
      paintFlushStroke();
      pStroke[0] = (int16_t)(lx - PAINT_CX); pStroke[1] = (int16_t)(ly - P_TOP);
      pStrokeN = 1;
    }
    pPrevX = T.x; pPrevY = T.y;
    flxFlush(y0 - rad - 1, y1 + rad + 1);
    return;
  }
  // ---- Dedo levantado: el trazo se ESCRIBE en el fichero ----
  if(!T.down && pStrokeN > 0) paintFlushStroke();
  if(!T.down){ pPrevX = pPrevY = -1; }

  if(!T.tap) return;
  if(T.x < 48 && T.y < 48){                       // volver a la galeria
    paintFlushStroke();
    paintView = 0; paintReload(); paintRenderGallery();
    return;
  }
  int y = SCR_H - 58, sw = 34, gap = 6, x0 = 10;
  if(T.y >= y - 4 && T.y <= y + sw + 4){
    for(int i = 0; i < 6; i++){
      int cx = x0 + i * (sw + gap) + sw / 2;
      if(T.x >= cx - sw / 2 && T.x <= cx + sw / 2){ pColor = P_PAL[i]; paintTools(); return; }
    }
    int gx = x0 + 6 * (sw + gap) + 8;
    if(T.x >= gx && T.x < gx + 44){ pSizeIx = (pSizeIx + 1) % 3; paintTools(); return; }
    if(T.x >= gx + 52 && T.x < gx + 128){         // DESHACER real: recorta el fichero
      if(flexPaintUndo(paintPath)) paintRenderCanvas();
      return;
    }
    if(T.x >= gx + 134){                          // LIMPIAR real: deja el .fxp a 0 trazos
      if(flexPaintClear(paintPath)) paintRenderCanvas();
      return;
    }
  }
}

static void paintEnter(){
  paintBufInit();
  if(!flexFsReady()){ fkNoFsScreen("Paint"); return; }
  if(!pStroke){                                   // sin memoria no se puede guardar: se dice
    setBuf(fb);
    fillRect(0, 0, SCR_W, SCR_H, rgb565(14,16,24));
    drawTextC(SCR_W / 2, 320, "Sin memoria para el lienzo", 2, rgb565(240,140,140));
    flxFlushAll();
    return;
  }
  pColor = P_PAL[0];
  paintView = 0; paintMulti = false; paintMask = 0; paintSelIdx = -1;
  fkMenuOn = false; fkNameOn = false; fkAskOn = false; fkTrashOn = false;
  paintReload();
  paintRenderGallery();
}

static void paintTick(){
  if(!flexFsReady() || !pStroke){ if(T.tap && T.x < 60 && T.y < 60) appClose(); return; }
  if(paintView == 0) paintGalleryTick();
  else               paintCanvasTick();
}

// #############################################################
// ##  APP SWITCHER (Multitarea) · carrusel horizontal estilo iOS
// ##  Tarjetas con mini-captura en PSRAM. Arrastre + inercia,
// ##  swipe-arriba para cerrar (free), toque para maximizar.
// #############################################################
#define SW_MAX  6
#define TH_W    150
#define TH_H    250
#define SW_CW   260
#define SW_CH   430
#define SW_STEP 288
#define SW_TOP  150

struct AppTask { uint8_t appID; bool used; uint16_t* thumb; };   // estado suspendido + miniatura
static AppTask swTasks[SW_MAX];
static int   swCount = 0;
static float swScrollPx = 0, swVel = 0, swLiftY = 0;
static int   swLiftCard = -1, swGesture = 0;                     // 0 nada, 1 horizontal, 2 vertical
static float swStartX, swStartY, swLastX2, swLastY2;

static uint16_t* swAllocThumb(){ return (uint16_t*)heap_caps_malloc((size_t)TH_W * TH_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void captureThumb(uint16_t* dst){                          // reduce fb 480x800 -> 150x250
  for(int j = 0; j < TH_H; j++){
    int sy = j * SCR_H / TH_H; uint16_t* d = dst + (size_t)j * TH_W;
    for(int i = 0; i < TH_W; i++) d[i] = fb[(size_t)sy * SCR_W + i * SCR_W / TH_W];
  }
}
static void swPush(uint8_t id){                                   // mueve al frente (o inserta)
  int at = -1; for(int i = 0; i < swCount; i++) if(swTasks[i].appID == id){ at = i; break; }
  if(at >= 0){ AppTask tmp = swTasks[at]; for(int i = at; i > 0; i--) swTasks[i] = swTasks[i - 1]; swTasks[0] = tmp; return; }
  if(swCount < SW_MAX){ for(int i = swCount; i > 0; i--) swTasks[i] = swTasks[i - 1]; swCount++; }
  else { if(swTasks[SW_MAX - 1].thumb) free(swTasks[SW_MAX - 1].thumb); for(int i = SW_MAX - 1; i > 0; i--) swTasks[i] = swTasks[i - 1]; }
  swTasks[0].appID = id; swTasks[0].used = true; swTasks[0].thumb = NULL;
}
static void swPushAndCapture(uint8_t id){
  swPush(id);
  if(!swTasks[0].thumb) swTasks[0].thumb = swAllocThumb();
  if(swTasks[0].thumb) captureThumb(swTasks[0].thumb);
}
// Para apps que dibujan en LANDSCAPE (Modo PC, Juegos): captureThumb reduce fb
// tal cual, y fb contiene el frame ya ROTADO. Guardarlo daria una miniatura
// girada 90 dentro de una tarjeta vertical -- que es justo lo que se veia en el
// selector de recientes. Aqui se entra en la lista SIN miniatura (y se tira la
// que hubiera, que seria de una sesion anterior), asi swRenderCards cae a su
// respaldo: tarjeta + icono de la app, que si esta bien orientado.
static void swPushNoThumb(uint8_t id){
  swPush(id);
  if(swTasks[0].thumb){ free(swTasks[0].thumb); swTasks[0].thumb = NULL; }
}
static void swCloseCard(int idx){                                 // libera PSRAM y reordena
  if(idx < 0 || idx >= swCount) return;
  if(swTasks[idx].thumb){ free(swTasks[idx].thumb); swTasks[idx].thumb = NULL; }
  for(int i = idx; i < swCount - 1; i++) swTasks[i] = swTasks[i + 1];
  swCount--;
}
static uint16_t swSxLUT[SW_CW], swSyLUT[SW_CH]; static bool swLUTdone = false;
static void swBuildLUT(){
  int dw = SW_CW - 16, dh = SW_CH - 52;
  for(int i = 0; i < dw; i++) swSxLUT[i] = (uint16_t)(i * TH_W / dw);
  for(int j = 0; j < dh; j++) swSyLUT[j] = (uint16_t)(j * TH_H / dh);
  swLUTdone = true;
}
static void blitThumbScaled(uint16_t* th, int dx, int dy, int dw, int dh){
  bool lut = (dw == SW_CW - 16 && dh == SW_CH - 52);        // ruta rapida (tamano fijo)
  if(lut && !swLUTdone) swBuildLUT();
  for(int j = 0; j < dh; j++){ int yy = dy + j; if((unsigned)yy >= SCR_H) continue;
    int sy = lut ? swSyLUT[j] : j * TH_H / dh;
    uint16_t* s = th + (size_t)sy * TH_W; uint16_t* d = gBuf + (size_t)yy * SCR_W;
    int x0 = dx < 0 ? 0 : dx, x1 = dx + dw > SCR_W ? SCR_W : dx + dw;
    for(int xx = x0; xx < x1; xx++) d[xx] = s[lut ? swSxLUT[xx - dx] : (xx - dx) * TH_W / dw];
  }
}
// Marco tipo vidrio (barato: sobre el fondo oscuro uniforme el blur no aporta,
// asi el carrusel corre fluido). drawLiquidGlassPanel se reserva para superficies con contenido detras.
static void swCardFrame(int x, int y, int w, int h, int rad){
  fillRoundRect(x, y, w, h, rad, rgb565(26,30,44));
  drawRoundRect(x, y, w, h, rad, rgb565(95,105,138));
}
static void swRender(float scale){                        // completo (solo animacion de entrada)
  setBuf(bbuf);
  if(blurBg) memcpy(bbuf, blurBg, (size_t)SCR_W * SCR_H * 2); else fillRect(0, 0, SCR_W, SCR_H, rgb565(8,10,16));
  drawTextC(SCR_W / 2, 6, "Recientes", 3, rgb565(240,244,252));
  if(swCount == 0) drawTextC(SCR_W / 2, SCR_H / 2, "Sin apps recientes", 2, rgb565(150,158,180));
  int cw = (int)(SW_CW * scale), ch = (int)(SW_CH * scale);
  for(int i = 0; i < swCount; i++){
    int cx = SCR_W / 2 + i * SW_STEP - (int)swScrollPx;
    if(cx < -SW_CW || cx > SCR_W + SW_CW) continue;
    int x = cx - cw / 2, y = SW_TOP + (SW_CH - ch) / 2;
    if(i == swLiftCard) y -= (int)swLiftY;
    swCardFrame(x, y, cw, ch, 22);
    if(swTasks[i].thumb) blitThumbScaled(swTasks[i].thumb, x + 8, y + 8, cw - 16, ch - 52);
    else { fillRoundRect(x + 8, y + 8, cw - 16, ch - 52, 14, rgb565(28,32,44)); drawAppIcon(swTasks[i].appID, x + cw / 2 - 30, y + ch / 2 - 70, 60); }
    drawTextC(x + cw / 2, y + ch - 32, appName(swTasks[i].appID), 2, rgb565(255,255,255));
  }
  drawTextC(SCR_W / 2, SCR_H - 28, "Desliza una tarjeta arriba para cerrar", 1, rgb565(130,138,158));
  present(0, SCR_H - 1);
}
// por-frame: SOLO repinta y vuelca la banda de las tarjetas (mucho mas ligero)
static void swRenderCards(){
  setBuf(bbuf);
  if(blurBg){ for(int j = 32; j < 604; j++) memcpy(bbuf + (size_t)j * SCR_W, blurBg + (size_t)j * SCR_W, SCR_W * 2); }
  else fillRect(0, 32, SCR_W, 572, rgb565(8,10,16));
  if(swCount == 0) drawTextC(SCR_W / 2, SCR_H / 2, "Sin apps recientes", 2, rgb565(150,158,180));
  for(int i = 0; i < swCount; i++){
    int cx = SCR_W / 2 + i * SW_STEP - (int)swScrollPx;
    if(cx < -SW_CW || cx > SCR_W + SW_CW) continue;
    int x = cx - SW_CW / 2, y = SW_TOP;
    if(i == swLiftCard) y -= (int)swLiftY;
    swCardFrame(x, y, SW_CW, SW_CH, 22);
    if(swTasks[i].thumb) blitThumbScaled(swTasks[i].thumb, x + 8, y + 8, SW_CW - 16, SW_CH - 52);
    else { fillRoundRect(x + 8, y + 8, SW_CW - 16, SW_CH - 52, 14, rgb565(28,32,44)); drawAppIcon(swTasks[i].appID, x + SW_CW / 2 - 30, y + SW_CH / 2 - 70, 60); }
    drawTextC(x + SW_CW / 2, y + SW_CH - 32, appName(swTasks[i].appID), 2, rgb565(255,255,255));
  }
  present(32, 604);
}
static int swCardIndexAt(int px){
  for(int i = 0; i < swCount; i++){ int cx = SCR_W / 2 + i * SW_STEP - (int)swScrollPx; if(px >= cx - SW_CW / 2 && px <= cx + SW_CW / 2) return i; }
  return -1;
}
static int swCenterIndex(){ int i = (int)roundf(swScrollPx / SW_STEP); if(i < 0) i = 0; if(i >= swCount) i = swCount - 1; return i; }
static void swExitToHome(){ gState = ST_HOME; renderHome(); showHome(); }
static void swMaximize(int idx){ if(idx >= 0 && idx < swCount) enterApp(swTasks[idx].appID); }  // restaura a pantalla completa

// Congela la app activa, cambia a MODO_MULTITAREA y hace la animacion elastica de entrada.
static void activarMultitarea(){
  if(KIOSK_ON && kioskOn) return;             // FASE 4: sin selector de apps en kiosco
  if(gHosted){ gHostReq = 3; return; }        // -> Recientes de DeX
  // Red de seguridad igual que en appClose: el selector se dibuja en portrait.
  // Si se llegara aqui con gLand=true, las tarjetas saldrian rotadas y a medias.
  gLand = false;
  gClipY0 = 0; gClipY1 = SCR_H - 1;
  gClipX0 = 0; gClipX1 = SCR_W - 1;
  setBuf(fb);
  ensureBlurBg();
  gState = ST_SWITCHER;
  swScrollPx = 0; swVel = 0; swLiftCard = -1; swLiftY = 0; swGesture = 0;
  for(int s = 1; s <= 10; s++){                         // spring scale-in (ease-out-back)
    float p = s / 10.0f, pm = p - 1.0f, eob = 1.0f + 2.6f * pm * pm * pm + 1.6f * pm * pm;
    swRender(0.6f + 0.4f * eob); delay(14);
  }
  swRender(1.0f);
}
static void swTick(){
  if(T.pressed){
    swStartX = T.x; swStartY = T.y; swLastX2 = T.x; swLastY2 = T.y; swVel = 0; swGesture = 0;
    swLiftCard = swCardIndexAt(T.x); swLiftY = 0;
    return;
  }
  if(T.down){
    float dx = T.x - swLastX2, dy = T.y - swLastY2; (void)dy;
    if(swGesture == 0){
      if(fabsf(T.x - swStartX) > 12) swGesture = 1;
      else if(fabsf(T.y - swStartY) > 14) swGesture = 2;
    }
    if(swGesture == 1){                                  // scroll horizontal + velocidad
      swScrollPx -= dx; swVel = -dx;
      float mn = -90, mx = (swCount - 1) * SW_STEP + 90; if(swScrollPx < mn) swScrollPx = mn; if(swScrollPx > mx) swScrollPx = mx;
      swLiftY = 0; swRenderCards();
    } else if(swGesture == 2 && swLiftCard >= 0){        // levantar tarjeta (cerrar)
      swLiftY = swStartY - T.y; if(swLiftY < 0) swLiftY = 0; if(swLiftY > 116) swLiftY = 116; swRenderCards();
    }
    swLastX2 = T.x; swLastY2 = T.y;
    return;
  }
  if(T.released){
    if(swGesture == 2 && swLiftCard >= 0 && swLiftY > 110){   // swipe-arriba -> cerrar (free)
      swCloseCard(swLiftCard); swLiftCard = -1; swLiftY = 0;
      float mx = (swCount > 0 ? (swCount - 1) * SW_STEP : 0); if(swScrollPx > mx) swScrollPx = mx; if(swScrollPx < 0) swScrollPx = 0;
      swRenderCards(); return;
    }
    if(swGesture == 0){                                  // toque
      if(T.y > SCR_H - 60){ swExitToHome(); return; }
      int idx = swCardIndexAt(T.x);
      if(idx >= 0){ if(idx == swCenterIndex()) swMaximize(idx); else { swScrollPx = idx * SW_STEP; swRenderCards(); } return; }
      swExitToHome(); return;
    }
    swLiftCard = -1; swLiftY = 0;
    return;
  }
  // reposo: inercia + enganche elastico a la tarjeta mas cercana
  if(fabsf(swVel) > 0.4f){
    swScrollPx += swVel; swVel *= 0.90f;
    float mx = (swCount > 0 ? (swCount - 1) * SW_STEP : 0);
    if(swScrollPx < 0){ swScrollPx = 0; swVel = 0; } if(swScrollPx > mx){ swScrollPx = mx; swVel = 0; }
    swRenderCards();
  } else {
    int tgt = swCenterIndex() * SW_STEP;
    if((int)swScrollPx != tgt){ swScrollPx += (tgt - swScrollPx) * 0.25f; if(fabsf(tgt - swScrollPx) < 1) swScrollPx = tgt; swRenderCards(); }
  }
}

// #############################################################
// ##  SEGURIDAD -> BLOQUEO (PIN / Contraseña)
// ##  Todo compone en bbuf y presenta de una vez (anti-flicker).
// #############################################################
enum { LSU_SEL = 0, LSU_PIN, LSU_PASS };
static int lsuMode = LSU_SEL;
static char lsuPin[12] = "", lsuPass[64] = "";
static int lsuPress = -1; static uint32_t lsuPressMs = 0;
static uint32_t lsuKbAnim = 0;                       // millis al abrir el teclado (para el slide de 0.3s)
static uint32_t lsuAnimMs = 0;
static const char* PIN_KEYS[12] = { "1","2","3","4","5","6","7","8","9","<","0","OK" };
static bool lsuVerify = false;                       // true = desbloquear (verificar), false = crear
static char lsuSaved[64] = "";                       // clave guardada a comparar
static uint32_t lsuWrong = 0;                        // millis del ultimo error (para el flash rojo)
// ---- Tema de las pantallas de clave (reutiliza la paleta de Ajustes) ----
// REGLA: gDark solo manda cuando el fondo es el color SOLIDO de pagina, o sea
// al CREAR la clave. Al VERIFICAR, el fondo es el wallpaper desenfocado -- una
// foto-- y ahi el modo claro pondria texto oscuro y tarjetas blancas encima que
// no se leerian. En ese caso se conservan exactamente los colores de siempre.
// El uiGlass, en cambio, ya se respetaba y se sigue respetando en los dos casos.
static uint16_t lsuBgCol()   { return lsuVerify ? rgb565(12,14,22)    : PAGE_BG; }
static uint16_t lsuCardCol() { return lsuVerify ? rgb565(44,54,92)    : SET_CARD_BG; }
static uint16_t lsuGlassCol(){ return lsuVerify ? rgb565(48,60,110)   : SET_CARD_GLASS; }
static uint16_t lsuKbBgCol() { return lsuVerify ? rgb565(18,20,28)    : PAGE_BG; }
static uint16_t lsuKbGlass() { return lsuVerify ? rgb565(36,40,58)    : SET_CARD_GLASS; }
static uint16_t lsuKeyCol()  { return lsuVerify ? rgb565(52,56,70)    : SET_CARD_BG; }
static uint16_t lsuTxtHi()   { return lsuVerify ? rgb565(255,255,255) : SET_TXT_HI; }
static uint16_t lsuTxtLo()   { return lsuVerify ? rgb565(150,158,180) : SET_TXT_LO; }
static uint16_t lsuKeyTxt()  { return lsuVerify ? rgb565(240,242,248) : SET_TXT_HI; }

static void lsuBg(){
  if(lsuVerify && blurBg) memcpy(gBuf, blurBg, (size_t)SCR_W * SCR_H * 2);   // wallpaper borroso
  else fillRect(0, 0, SCR_W, SCR_H, lsuBgCol());
}

// #############################################################
// ##  TRANSICION PREVIA AL BLOQUEO DE SEGURIDAD
// ##  ------------------------------------------------------
// ##  Antes, pedir la clave era un CORTE SECO: el cuadro que hubiera en
// ##  pantalla (el escritorio, la app que se estaba bloqueando, la pantalla de
// ##  Bloqueo) se sustituia de golpe por el teclado de la clave.
// ##  Ahora son dos tiempos, como en One UI o iOS:
// ##    1) la interfaz actual se DESVANECE hacia el fondo de la pantalla de
// ##       autenticacion (fade out);
// ##    2) el metodo de seguridad que el usuario tenga configurado -- PIN,
// ##       contrasena o el que se anada despues -- APARECE encima (fade in).
// ##  Vive en el camino comun (lsuStartVerify + las dos rutinas que pintan el
// ##  primer cuadro de cada metodo), asi que TODAS las rutas que piden clave la
// ##  heredan: bloquear/desbloquear una app, salir del kiosco, apagado seguro y
// ##  el desbloqueo de la pantalla.
// ##  Los dos fundidos son por TIEMPO (no por numero de pasos): duran lo mismo
// ##  vaya el sistema rapido o lento, y meten tantos cuadros como pueda dar el
// ##  compositor. Cada cuadro se publica entero con present(), o sea que no
// ##  puede aparecer a medias ni partido.
// #############################################################
#define AUTH_FADE_OUT_MS 190
#define AUTH_FADE_IN_MS  230
static uint16_t* authSnap = NULL;        // instantanea de la interfaz que se va
static bool authFadePending = false;     // el fade out ya corrio: toca el fade in

// Fundido de salida hacia el fondo de la pantalla de clave. Deja preparado el
// fade in. Si algo no esta disponible (PSRAM, landscape, app hospedada) no se
// anima y la verificacion sigue igual que siempre: la transicion es un adorno,
// nunca un requisito para poder introducir la clave.
static void authFadeOut(){
  authFadePending = false;
  if(gHosted || gLand) return;
  ensureBlurBg();
  if(!blurBg) return;
  if(!authSnap) authSnap = (uint16_t*)heap_caps_malloc((size_t)SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!authSnap) return;
  memcpy(authSnap, fb, (size_t)SCR_W * SCR_H * 2);
  uint32_t t0 = millis();
  int last = -1;
  for(;;){
    uint32_t e = millis() - t0; if(e > (uint32_t)AUTH_FADE_OUT_MS) e = AUTH_FADE_OUT_MS;
    float p = (float)e / (float)AUTH_FADE_OUT_MS;
    p = p * p * (3.0f - 2.0f * p);                       // suavizado en las dos puntas
    uint8_t a = (uint8_t)(p * 255.0f);
    if((int)a == last){                                  // el reloj aun no ha movido el fundido
      if(e < (uint32_t)AUTH_FADE_OUT_MS){ delay(1); continue; }
      break;
    }
    last = a;
    for(int j = 0; j < SCR_H; j++){
      uint16_t* d = bbuf + (size_t)j * SCR_W;
      const uint16_t* s0 = authSnap + (size_t)j * SCR_W;
      const uint16_t* s1 = blurBg   + (size_t)j * SCR_W;
      for(int i = 0; i < SCR_W; i++) d[i] = mix565(s0[i], s1[i], a);
    }
    present(0, SCR_H - 1);
    if(e >= (uint32_t)AUTH_FADE_OUT_MS) break;
  }
  authFadePending = true;
}
// Fundido de entrada del metodo de seguridad ya compuesto en 'target'.
// Devuelve false si no habia transicion en curso, para que el llamante publique
// su cuadro como siempre.
static bool authFadeIn(const uint16_t* target){
  if(!authFadePending) return false;
  authFadePending = false;
  if(!target || !blurBg) return false;
  uint32_t t0 = millis();
  int last = -1;
  for(;;){
    uint32_t e = millis() - t0; if(e > (uint32_t)AUTH_FADE_IN_MS) e = AUTH_FADE_IN_MS;
    float p = (float)e / (float)AUTH_FADE_IN_MS;
    p = p * p * (3.0f - 2.0f * p);
    uint8_t a = (uint8_t)(p * 255.0f);
    if((int)a == last){
      if(e < (uint32_t)AUTH_FADE_IN_MS){ delay(1); continue; }
      break;
    }
    last = a;
    for(int j = 0; j < SCR_H; j++){
      uint16_t* d = bbuf + (size_t)j * SCR_W;
      const uint16_t* s0 = blurBg + (size_t)j * SCR_W;
      const uint16_t* s1 = target + (size_t)j * SCR_W;
      for(int i = 0; i < SCR_W; i++) d[i] = mix565(s0[i], s1[i], a);
    }
    present(0, SCR_H - 1);
    if(e >= (uint32_t)AUTH_FADE_IN_MS) break;
  }
  memcpy(bbuf, target, (size_t)SCR_W * SCR_H * 2);       // cuadro final exacto (sin redondeos del fundido)
  present(0, SCR_H - 1);
  return true;
}

// #############################################################
// ##  FASE 1 - BLOQUEO GLOBAL REFORZADO
// ##  (a) sacudida amortiguada al fallar   LOCK_SHAKE_ON
// ##  (b) contador persistente + espera progresiva  LOCK_FAILS_ON
// ##  (c) auto-bloqueo por inactividad     AUTOLOCK_ON
// ##  Va aqui, entre lsuBg() y el resto del LSU, porque necesita conocer
// ##  lsuMode/lsuVerify (declarados justo arriba) y lo usan lsuTick /
// ##  lsuUnlock / lsuStartVerify (justo abajo).
// #############################################################

// ---- (a) Sacudida horizontal amortiguada -------------------------------
// Un fallo NUNCA se comunica con un parpadeo ni con un cambio brusco de color:
// se comunica moviendo. El offset es una senoidal de 3 ciclos cuya amplitud
// decae linealmente hasta 0, evaluada a ~30 ms por frame -> unos 6 frames.
// Al expirar devuelve exactamente 0, asi que el ultimo frame de la animacion
// deja la pantalla en su sitio sin ningun salto.
#define LSU_SHAKE_MS   180
#define LSU_SHAKE_AMP  12.0f
#define LSU_SHAKE_CYC  3.0f
static uint32_t lsuShakeMs = 0;                  // millis de inicio (0 = quieto)
static void lsuShakeStart(){
  if(!LOCK_SHAKE_ON) return;
  lsuShakeMs = millis();
  if(!lsuShakeMs) lsuShakeMs = 1;                // 0 es el centinela de "quieto"
}
static int lsuShakeOff(){
  if(!LOCK_SHAKE_ON || !lsuShakeMs) return 0;
  uint32_t e = millis() - lsuShakeMs;
  if(e >= (uint32_t)LSU_SHAKE_MS){ lsuShakeMs = 0; return 0; }
  float p = (float)e / (float)LSU_SHAKE_MS;                       // 0..1
  return (int)(LSU_SHAKE_AMP * (1.0f - p) * sinf(p * LSU_SHAKE_CYC * 6.2831853f));
}

// ---- (b) Espera progresiva tras intentos fallidos ----------------------
// Geometria de la zona del contador. Cae en el hueco que ya existe entre los
// puntos del PIN (y=150) y la primera fila del teclado numerico (y=300), asi
// que no tapa nada y sirve igual en la pantalla de contrasena.
#define LW_BAND_Y0  180
#define LW_BAND_Y1  276
#define LW_MSG_Y    186
#define LW_MSG2_Y   206
#define LW_NUM_Y0   226                      // banda que se repinta por diffing
#define LW_NUM_Y1   274
#define LW_NUM_Y    232
static uint32_t lockWaitUntil    = 0;        // millis en que expira la espera (0 = ninguna)
static bool     lockPenaltyServed = false;   // ya se cobro la espera del contador actual
static bool     lockWaitPainted  = false;    // el mensaje fijo ya esta en pantalla
static int      lockWaitLastSec  = -1;       // ultimo valor pintado (diffing)

static uint32_t lockPenaltyMs(){
  if(lockFails >= LOCK_FAILS_HARD) return LOCK_WAIT_HARD_MS;   // 6+  -> 5 min
  if(lockFails >= LOCK_FAILS_SOFT) return LOCK_WAIT_SOFT_MS;   // 4-5 -> 30 s
  return 0;                                                    // 1-3 -> sin espera
}
// La resta se hace en int32 con signo a proposito: asi sigue siendo correcta
// cuando millis() da la vuelta a los ~49 dias.
static bool lockWaitActive(){
  if(!LOCK_FAILS_ON || !lockWaitUntil) return false;
  if((int32_t)(millis() - lockWaitUntil) >= 0){
    lockWaitUntil = 0; lockPenaltyServed = true;               // cumplida
    return false;
  }
  return true;
}
// Restaura la banda [y0,y1] en bbuf desde la capa que corresponda: la base
// estatica del teclado numerico (lockBuf, que ya la tiene compuesta) o el
// wallpaper borroso de la pantalla de contrasena. NUNCA recompone la pantalla
// entera: solo estas filas.
static void lockWaitBase(int y0, int y1){
  setBuf(bbuf);
  // Recorte completo: si veniamos de una lista con scroll de Ajustes, gClip*
  // podria estar estrechado y el contador no se dibujaria -- y sin monitor
  // serie eso solo se ve como "la cuenta atras no aparece".
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  if(lsuMode == LSU_PIN){
    for(int j = y0; j <= y1; j++) memcpy(bbuf + (size_t)j * SCR_W, lockBuf + (size_t)j * SCR_W, SCR_W * 2);
  } else if(lsuVerify && blurBg){
    for(int j = y0; j <= y1; j++) memcpy(bbuf + (size_t)j * SCR_W, blurBg + (size_t)j * SCR_W, SCR_W * 2);
  } else {
    fillRect(0, y0, SCR_W, y1 - y0 + 1, lsuBgCol());
  }
}
// Contador regresivo con DIFFING: si el segundo que toca mostrar es el mismo
// que ya esta pintado, esta funcion no toca ni un pixel. Cuando cambia,
// repinta solo la franja del numero (LW_NUM_*), no la pantalla.
static void lockWaitTick(){
  uint32_t rem = lockWaitUntil - millis();                     // >0: lo garantiza lockWaitActive()
  int secs = (int)((rem + 999) / 1000);                        // redondea hacia arriba
  if(lockWaitPainted && secs == lockWaitLastSec) return;       // nada cambio
  bool first = !lockWaitPainted;
  int y0 = first ? LW_BAND_Y0 : LW_NUM_Y0;
  int y1 = first ? LW_BAND_Y1 : LW_NUM_Y1;
  lockWaitBase(y0, y1);
  if(first){
    const char* m1 = "Demasiados intentos fallidos";
    drawTextC(SCR_W / 2, LW_MSG_Y, m1, uiFontFit(m1, SCR_W - 40, 2), rgb565(240,160,150));
    if(lockFails >= LOCK_FAILS_HARD){                          // mensaje explicito del tramo largo
      const char* m2 = "Bloqueo temporal de 5 minutos";
      drawTextC(SCR_W / 2, LW_MSG2_Y, m2, uiFontFit(m2, SCR_W - 40, 2), rgb565(205,150,145));
    }
  }
  char cd[16];
  if(secs >= 60) snprintf(cd, sizeof(cd), "%d:%02d", secs / 60, secs % 60);
  else           snprintf(cd, sizeof(cd), "%d s", secs);
  drawTextC(SCR_W / 2, LW_NUM_Y, cd, 4, rgb565(255,255,255));
  present(y0, y1);
  setBuf(fb);
  lockWaitLastSec = secs; lockWaitPainted = true;
}
// Olvida lo que hay pintado del contador SIN dibujar nada: quien llama a esto
// va a recomponer la zona igualmente (lsuAnimPin, lsuRenderPass o lsuExit), y
// asi el borrado viaja en ese mismo present() en vez de en un volcado propio.
// No toca lockWaitUntil: salir de la pantalla no perdona la espera.
static void lockWaitReset(){
  lockWaitPainted = false; lockWaitLastSec = -1;
}
static void lockOnFail(){
  if(!LOCK_FAILS_ON) return;
  if(lockFails < 9999) lockFails++;
  lockFailsSave();                                  // persiste ANTES de cualquier espera
  lockPenaltyServed = false;
  uint32_t pen = lockPenaltyMs();
  lockWaitUntil = pen ? (millis() + pen) : 0;
  lockWaitPainted = false; lockWaitLastSec = -1;
}
static void lockOnSuccess(){
  lockWaitUntil = 0; lockWaitPainted = false; lockWaitLastSec = -1;
  lockPenaltyServed = true;
  if(!LOCK_FAILS_ON) return;
  if(lockFails != 0){ lockFails = 0; lockFailsSave(); }         // acierto -> contador a cero
}
// Se llama al ABRIR la pantalla de verificacion. Si el contador ya venia alto
// -por ejemplo porque se reinicio la placa para intentar saltarse la espera-
// la espera se cobra aqui, antes del primer intento. lockPenaltyServed arranca
// en false en cada arranque, asi que el castigo se aplica una vez tras el
// reinicio y no se repite cada vez que se entra y se sale de la pantalla.
static void lockArmPendingPenalty(){
  if(!LOCK_FAILS_ON || lockWaitUntil || lockPenaltyServed) return;
  uint32_t pen = lockPenaltyMs();
  if(!pen) return;
  lockWaitUntil = millis() + pen;
  lockWaitPainted = false; lockWaitLastSec = -1;
}

// ---- (c) Auto-bloqueo por inactividad ---------------------------------
// No duplica NADA de la logica de bloqueo: cierra la app con appClose() (su
// animacion normal, que ya deja miniatura en Recientes y devuelve a ST_HOME) y
// luego deja caer el bloqueo con animateTo()/composeUnlock(), el mismo
// mecanismo interpolado del desbloqueo por gesto pero al reves.
static void autoLockNow(){
  if(editMode) edExit();                    // guarda el orden de iconos y repinta el Home
  if(gState == ST_APP) appClose();           // -> ST_HOME, con su animacion de cierre
  gRippleActive = false;
  renderHome();                              // capa de abajo de composeUnlock, al dia
  renderLock();                              // capa de arriba, con la hora actual
  gState = ST_LOCK;
  animateTo(SCR_H, 0);                       // el bloqueo baja interpolado (cero parpadeo)
  lockOff = 0; lastLockOff = -1;
  showLock();
  gLastTouchMs = millis();
}
static void autoLockTick(){
  if(!AUTOLOCK_ON) return;
  // FASE 4: en kiosco NO se auto-bloquea. El telefono esta prestado y en uso; y
  // ademas dejar caer el bloqueo sobre el kiosco mezclaria dos modos que se
  // pisan (appClose esta vetado en kiosco, asi que el cierre quedaria a medias).
  if(KIOSK_ON && kioskOn) return;
  // SUSPENSION Y AUTO-BLOQUEO SON INDEPENDIENTES, NO SE PISAN.
  // Mientras la pantalla esta suspendida no se deja caer el bloqueo desde aqui:
  // hacerlo contra una pantalla apagada seria trabajo invisible (animateTo
  // compone y vuelca un frame entero que nadie ve) y dejaria lockOff
  // desincronizado con lo que hay en el framebuffer.
  // No hace falta: del bloqueo al despertar ya se encarga suspWakeLockScreen(),
  // que lo pone SIEMPRE que haya clave configurada -- sin esperar a que venza
  // ninguna ventana de inactividad. Asi que suspender es, de hecho, mas estricto
  // que el auto-bloqueo, no menos.
  // Tambien se aplaza durante el apagado completo: ahi el destino ya esta
  // decidido y bloquear a medias solo puede romper la animacion.
  if(SUSPEND_ON && gSuspOn) return;
  if(POWEROFF_ON && (gState == ST_POWEROFF_CONFIRM || gState == ST_POWEROFF_ANIM)) return;
  // NUNCA auto-bloquear con una actualizacion en marcha. Una descarga no
  // genera toques, asi que el temporizador de inactividad vencia a mitad
  // de la OTA (30 s por defecto): caia la pantalla de bloqueo, se
  // repintaba a pantalla completa peleandose con la de progreso, y el
  // usuario se encontraba pidiendo el PIN mientras se estaba flasheando.
  // Tambien se respeta cualquier capa OTA a pantalla completa.
  if(flexOtaBusy() || flexOtaOwnsScreen()){ gLastTouchMs = millis(); return; }
  // Cualquier contacto, en CUALQUIER pantalla, rearma el temporizador. Va antes
  // del filtro de estados para que al volver del PIN al escritorio el contador
  // no arranque ya vencido y vuelva a bloquear al instante.
  if(T.down || T.pressed || T.released){ gLastTouchMs = millis(); return; }
  if(!gAutoLockMs) return;
  if(gState != ST_HOME && gState != ST_APP) return;
  if(gLand || gHosted) return;                // Modo PC / app hospedada: no se toca
  if(qsPanelY != 0) return;                   // cortina abierta: no bloquear a media interaccion
  if(!gLastTouchMs){ gLastTouchMs = millis(); return; }
  if(millis() - gLastTouchMs < gAutoLockMs) return;
  autoLockNow();
}

// #############################################################
// ##  FASE 4 - MODO KIOSCO (prestamo seguro)
// ##  El estado y el filtro tactil estan arriba, junto a flexPollTouch.
// ##  Aqui va todo lo que necesita dibujar o navegar.
// #############################################################

// ---- Candado discreto de "kiosco activo" -------------------------------
// El candado NO se refresca por temporizador. Se ESTAMPA dentro de flxFlush(),
// que es el unico punto por el que absolutamente todo acaba llegando al panel,
// justo antes de publicar la banda sucia. Asi el candado esta en fb POR
// CONSTRUCCION antes de cualquier subida DMA.
//
// Por que no basta con repintarlo desde kioskTick: geoRenderGame (Geo Dash)
// compone en bbuf y hace present(0, SCR_H-1) en cada frame, y el presenter sube
// fb de forma ASINCRONA desde el core 0. Repintando despues del present hay una
// ventana en la que el presenter ya empezo a subir la banda sin el candado -- se
// vea mas o menos seguido, sigue parpadeando. Estampando dentro de flxFlush esa
// ventana no existe: la banda nunca se publica sin el candado dentro.
// Recuadro que envuelve al candado con margen. Es la banda que se comprueba en
// flxFlush y la que publica kioskShowBadge, asi que tiene que seguir a
// KIOSK_BADGE_X/Y: candado en 448..471 x 44..67, recuadro 444..479 x 40..71.
#define KIOSK_BOX_X 444
#define KIOSK_BOX_Y 40
#define KIOSK_BOX_W 36
#define KIOSK_BOX_H 32
static void kioskBadgePaint(){
  int bx = KIOSK_BADGE_X, by = KIOSK_BADGE_Y, bs = KIOSK_BADGE_S;
  fillRoundRectA(bx, by, bs, bs, 7, rgb565(16,18,26), 200);
  fillCircleA(bx + bs / 2, by + 9, 5, rgb565(238,241,248), 255);            // arco del candado
  fillCircleA(bx + bs / 2, by + 9, 3, rgb565(16,18,26), 255);
  fillRoundRectA(bx + 5, by + 11, bs - 10, 9, 2, rgb565(238,241,248), 255); // cuerpo
}
// Llamada desde flxFlush con la banda que se va a publicar. Sale enseguida en el
// caso normal (kiosco apagado, o banda que no toca la esquina del candado), asi
// que no encarece el camino de dibujo del resto del sistema. Dibuja y ya: NO
// llama a flxFlush, de modo que no hay recursion.
static void kioskStampBadge(int y0, int y1){
  if(!KIOSK_ON || !kioskOn) return;
  if(gState != ST_APP) return;                                   // solo sobre la app clavada
  if(y1 < KIOSK_BOX_Y || y0 > KIOSK_BOX_Y + KIOSK_BOX_H - 1) return;
  // gLand fuera y recorte completo: el candado va en la esquina FISICA del panel,
  // pase lo que pase con la orientacion logica de la app (Juegos dibuja en
  // landscape y pone gLand=true en cada frame).
  uint16_t* ob = gBuf; bool wl = gLand;
  int sx0 = gClipX0, sx1 = gClipX1, sy0 = gClipY0, sy1 = gClipY1;
  gLand = false;
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  setBuf(fb);
  kioskBadgePaint();
  setBuf(ob); gLand = wl;
  gClipX0 = sx0; gClipX1 = sx1; gClipY0 = sy0; gClipY1 = sy1;
}
// Publica la banda del candado al ENTRAR en kiosco, para que aparezca de
// inmediato aunque la app no vuelva a dibujar por su cuenta. El estampado en si
// lo hace flxFlush. A partir de ahi el candado se mantiene solo.
static void kioskShowBadge(){
  if(!KIOSK_ON || !kioskOn) return;
  flxFlush(KIOSK_BOX_Y, KIOSK_BOX_Y + KIOSK_BOX_H - 1);
}

// ---- Entrar / salir ----------------------------------------------------
static void kioskStart(int id, int ex, int ey, int ew, int eh){
  if(!KIOSK_ON || gLockType == 0 || id < 0 || id > 15) return;   // sin clave no habria salida: no se activa
  kioskOn = true; kioskApp = id;
  kioskExX = ex; kioskExY = ey; kioskExW = ew; kioskExH = eh;
  kioskSave();
  renderHome();                 // winRevealAnim compone sobre homeBuf
  enterApp(id);                 // apertura con la animacion normal del sistema
  kioskShowBadge();
}
static void kioskExitNow(){
  kioskOn = false; kioskApp = -1;
  kioskExX = kioskExY = kioskExW = kioskExH = 0;
  kioskSave();
  gLand = false;                                       // misma red de seguridad que appClose
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  setBuf(fb);
  renderHome();
  enterHome();                                         // deja gState en ST_HOME y vuelca el escritorio
  gLastTouchMs = millis();
}
// Gesto de salida: mantener pulsado el candado > 1 s. Es el mismo gesto de
// long-press con el que se entro (desde el menu contextual), y solo abre la
// verificacion: sin PIN/contrasena correcta no se sale.
// El area excluida SOLO filtra mientras la app clavada esta en pantalla.
//
// Antes filtraba en cualquier estado, y eso convertia el telefono en un ladrillo
// irrecuperable: si el usuario dibujaba el area encima del teclado del PIN (o
// simplemente grande), al pedir la salida del kiosco la pantalla de verificacion
// aparecia pero el teclado no respondia -- y como el kiosco persiste en NVS,
// reiniciar volvia a entrar en el mismo sitio. No habia ninguna salida.
//
// El area excluida existe para que la APP no reciba esos toques; la UI del
// propio sistema (verificacion, menus) nunca fue su objetivo.
static bool kioskTouchBlocked(int px, int py){
  if(!KIOSK_ON || !kioskOn) return false;
  if(gState != ST_APP) return false;              // verificacion del PIN, menus, etc: sin filtro
  if(!kioskInExcluded(px, py)) return false;
  return !kioskInExit(px, py);                    // el candado de salida siempre gana
}
static bool kioskExitFired = false;                    // ya se disparo con ESTE contacto
static void kioskTick(){
  if(!KIOSK_ON || !kioskOn){ kioskExitFired = false; return; }
  // El rearme va ANTES del filtro de estado: mientras se escribe la clave
  // seguimos en ST_LOCKSETUP, y si el dedo se levanta ahi hay que poder volver a
  // intentarlo. Sin esto, un dedo que siguiera apoyado al cancelar la salida
  // reabriria la verificacion en bucle.
  if(!T.down) kioskExitFired = false;
  if(gState != ST_APP) return;
  // Ya no repinta el candado: de eso se encarga kioskStampBadge desde flxFlush.
  // Aqui solo queda escuchar el gesto de salida.
  if(!kioskExitFired && T.down && kioskInExit(T.startX, T.startY) && (millis() - T.downMs) > 1000
     && abs(T.x - T.startX) < 12 && abs(T.y - T.startY) < 12){
    kioskExitFired = true;
    lsuStartVerifyFor(LSU_AFTER_KIOSKOUT, kioskApp);
  }
}

// ---- Pantalla para definir el area excluida (ST_KIOSKSET) --------------
// El fondo (titulo, icono, textos y botones) es ESTATICO y se compone una sola
// vez en lockBuf -- el mismo uso de scratch que ya hace lsuComposePin. Cada
// frame del arrastre solo copia esa base y dibuja el rectangulo encima: un
// unico present() por frame, cero parpadeo.
#define KS_BTN_Y (SCR_H - 96)
#define KS_BTN_H 64
#define KS_BTN_W 180
static int  kioskSetApp = -1;
static int  kioskSetX0 = 0, kioskSetY0 = 0, kioskSetX1 = 0, kioskSetY1 = 0;
static bool kioskSetHas = false;
static uint32_t kioskSetMs = 0;
static void kioskSetBase(int id){
  memcpy(lockBuf, homeBuf, (size_t)SCR_W * SCR_H * 2);
  setBuf(lockBuf);
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  // Velo del color de pagina (gDark) + TARJETA de vidrio con el contenido, que
  // es como usa el vidrio el resto del sistema. Antes el velo era un gris
  // azulado fijo, que es justo por lo que parecia que aqui "se quitaba" el
  // Liquid Glass. El vidrio va en la tarjeta y no a pantalla completa a
  // proposito: drawLiquidGlassPanelEx reparte su degradado de luz sobre TODA la
  // altura del panel, y a 800 px eso seria una banda blanca-a-negra enorme en
  // vez de un cristal.
  fillRectA(0, 0, SCR_W, SCR_H, PAGE_BG, 208);
  if(uiGlass) drawLiquidGlassPanel(24, 40, SCR_W - 48, 350, 24, SET_CARD_GLASS);
  else        fillRoundRectA(24, 40, SCR_W - 48, 350, 24, SET_CARD_BG, 235);
  drawTextC(SCR_W / 2, 64, "Modo Kiosco", 4, SET_TXT_HI);
  const char* s1 = "Arrastra para excluir una zona del tactil";
  drawTextC(SCR_W / 2, 122, s1, uiFontFit(s1, SCR_W - 40, 2), SET_TXT_LO);
  const char* s2 = "Si no arrastras, toda la pantalla queda activa";
  drawTextC(SCR_W / 2, 146, s2, uiFontFit(s2, SCR_W - 40, 2), SET_TXT_MUTE);
  drawAppIcon(id, SCR_W / 2 - 36, 196, 72);
  drawTextC(SCR_W / 2, 282, appName(id), 3, SET_TXT_HI);
  const char* s3 = "Para salir: manten pulsado el candado de la";
  drawTextC(SCR_W / 2, 344, s3, uiFontFit(s3, SCR_W - 40, 2), SET_TXT_LO);
  const char* s4 = "esquina y escribe tu clave del sistema";
  drawTextC(SCR_W / 2, 366, s4, uiFontFit(s4, SCR_W - 40, 2), SET_TXT_LO);
  // "Cancelar" es una tarjeta normal (tema), "Iniciar" conserva el azul de
  // acento: los colores de MARCA no cambian con gDark, igual que en Ajustes.
  if(uiGlass) drawLiquidGlassPanel(30, KS_BTN_Y, KS_BTN_W, KS_BTN_H, 20, SET_CARD_GLASS);
  else        fillRoundRect(30, KS_BTN_Y, KS_BTN_W, KS_BTN_H, 20, SET_CARD_BG);
  drawTextC(30 + KS_BTN_W / 2, KS_BTN_Y + KS_BTN_H / 2 - 9, "Cancelar", 2, SET_TXT_HI);
  fillRoundRect(SCR_W - 30 - KS_BTN_W, KS_BTN_Y, KS_BTN_W, KS_BTN_H, 20, rgb565(46,82,182));
  drawTextC(SCR_W - 30 - KS_BTN_W / 2, KS_BTN_Y + KS_BTN_H / 2 - 9, "Iniciar", 2, rgb565(255,255,255));
  setBuf(fb);
}
static void kioskSetRender(){
  memcpy(bbuf, lockBuf, (size_t)SCR_W * SCR_H * 2);
  setBuf(bbuf);
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  if(kioskSetHas){
    int x = kioskSetX0 < kioskSetX1 ? kioskSetX0 : kioskSetX1;
    int y = kioskSetY0 < kioskSetY1 ? kioskSetY0 : kioskSetY1;
    int w = kioskSetX1 - kioskSetX0; if(w < 0) w = -w;
    int h = kioskSetY1 - kioskSetY0; if(h < 0) h = -h;
    fillRectA(x, y, w, h, rgb565(235,90,80), 95);
    drawRoundRect(x, y, w, h, 4, rgb565(246,150,140));
    char b[24]; snprintf(b, sizeof(b), "%d x %d", w, h);
    drawTextC(x + w / 2, y + h / 2 - 9, b, 2, rgb565(255,255,255));
  }
  present(0, SCR_H - 1);
  setBuf(fb);
}
static void kioskSetEnter(int id){
  kioskSetApp = id; kioskSetHas = false; kioskSetMs = 0;
  kioskSetX0 = kioskSetY0 = kioskSetX1 = kioskSetY1 = 0;
  gState = ST_KIOSKSET;
  kioskSetBase(id);
  kioskSetRender();
}
static void kioskSetTick(){
  // El arrastre solo cuenta si EMPIEZA por encima de la fila de botones, asi que
  // pulsar "Iniciar" o "Cancelar" nunca se interpreta como dibujar.
  bool dragZone = (T.startY < KS_BTN_Y - 8);
  if(T.pressed && dragZone){
    kioskSetX0 = kioskSetX1 = T.x; kioskSetY0 = kioskSetY1 = T.y;
    kioskSetHas = false; return;
  }
  if(T.down && dragZone){
    kioskSetX1 = T.x; kioskSetY1 = T.y;
    int w = kioskSetX1 - kioskSetX0; if(w < 0) w = -w;
    int h = kioskSetY1 - kioskSetY0; if(h < 0) h = -h;
    kioskSetHas = (w >= 20 && h >= 20);                 // por debajo de eso es un toque, no un area
    if(millis() - kioskSetMs > 30){ kioskSetMs = millis(); kioskSetRender(); }
    return;
  }
  if(T.released && dragZone){ kioskSetRender(); return; }   // fija el rectangulo dibujado
  if(T.tap && T.y >= KS_BTN_Y && T.y <= KS_BTN_Y + KS_BTN_H){
    if(T.x >= 30 && T.x <= 30 + KS_BTN_W){                  // Cancelar
      gState = ST_HOME; renderHome(); showHome(); return;
    }
    if(T.x >= SCR_W - 30 - KS_BTN_W && T.x <= SCR_W - 30){  // Iniciar
      int ex = 0, ey = 0, ew = 0, eh = 0;
      if(kioskSetHas){
        ex = kioskSetX0 < kioskSetX1 ? kioskSetX0 : kioskSetX1;
        ey = kioskSetY0 < kioskSetY1 ? kioskSetY0 : kioskSetY1;
        ew = kioskSetX1 - kioskSetX0; if(ew < 0) ew = -ew;
        eh = kioskSetY1 - kioskSetY0; if(eh < 0) eh = -eh;
      }
      kioskStart(kioskSetApp, ex, ey, ew, eh);
      return;
    }
  }
}

// #############################################################
// ##  FASE 2 - MENU CONTEXTUAL DE LONG-PRESS (estilo action sheet)
// ##  Aparece con escala+fundido interpolados sobre el escritorio. NO
// ##  recompone la pantalla entera: solo la banda inferior donde vive.
// #############################################################
// Panel unico anclado al icono, estilo hoja de acciones de iOS: una sola
// tarjeta redondeada, texto a la IZQUIERDA, glifo a la DERECHA y filas separadas
// por una linea de 1 px. No lleva fila "Cancelar": se cierra tocando fuera.
#define CTX_ROWS     3
#define CTX_ROW_H    58
#define CTX_W        244
#define CTX_RAD      20
#define CTX_GLYPH_S  26
#define CTX_PAD_L    18              // margen del texto
#define CTX_PAD_R    14              // margen del glifo
#define CTX_MARGIN   8               // aire minimo contra cualquier borde
#define CTX_GAPX     12              // separacion entre el icono y el panel
#define CTX_ICON_S   72              // lado del icono en la rejilla del Home
#define CTX_ANIM_MS  150
#define CTX_PANEL_H  (CTX_ROWS * CTX_ROW_H)
static int      ctxApp = -1, ctxAction = -1;
static bool     ctxClosing = false;
static uint32_t ctxAnimMs = 0;
static int      ctxPx = 0, ctxPy = 0;              // esquina del panel ya recortada
static int      ctxBandY0 = 0, ctxBandY1 = 0;      // banda que se recompone por frame
static uint16_t ctxPanelCol(){ return uiGlass ? SET_CARD_GLASS : SET_CARD_BG; }
// Fila 0 = candado de app, 1 = Modo edicion, 2 = Modo kiosco. Las dos que
// necesitan una clave del sistema con la que verificar se dibujan atenuadas y
// son inertes si no hay ninguna configurada: se ve por que no se pueden usar,
// en vez de no hacer nada al tocarlas.
static bool ctxRowEnabled(int i){
  if(i == 0) return APPLOCK_ON && gLockType > 0;
  if(i == 2) return KIOSK_ON   && gLockType > 0;
  return true;
}
static const char* ctxLabel(int i){
  switch(i){
    case 0:  return appLockGet(ctxApp) ? "Desbloquear app" : "Bloquear app";
    case 1:  return "Modo edici\xC3\xB3" "n";
    default: return "Modo kiosko";
  }
}
// Glifos vectoriales de 26x26, dibujados con las primitivas que ya existen: no
// hacen falta bitmaps ni una fuente de iconos. El "hueco" de cada uno se pinta
// del color del panel, asi que siguen al tema sin logica aparte.
static void ctxGlyph(int kind, int x, int y, int s, uint8_t a){
  uint16_t hole = ctxPanelCol();
  if(kind == 0){                                        // candado
    uint16_t c = rgb565(235,80,80);
    fillCircleA(x + s / 2, y + s / 3, s / 4, c, a);     // arco
    fillCircleA(x + s / 2, y + s / 3, s / 6, hole, a);
    fillRoundRectA(x + 3, y + s / 2 - 2, s - 6, s / 2 + 1, 3, c, a);   // cuerpo
  } else if(kind == 1){                                 // rejilla de iconos (Modo edicion)
    uint16_t c = rgb565(140,150,172);
    int q = (s - 5) / 2;
    fillRoundRectA(x,             y,             q, q, 2, c, a);
    fillRoundRectA(x + q + 5,     y,             q, q, 2, c, a);
    fillRoundRectA(x,             y + q + 5,     q, q, 2, c, a);
    fillRoundRectA(x + q + 5,     y + q + 5,     q, q, 2, c, a);
  } else {                                              // pantalla con candado (Modo kiosco)
    uint16_t c = rgb565(110,200,155);
    fillRoundRectA(x, y + 1, s, s - 7, 3, c, a);        // marco
    fillRoundRectA(x + 3, y + 4, s - 6, s - 13, 2, hole, a);
    fillRoundRectA(x + s / 2 - 3, y + s / 2 - 5, 6, 7, 1, c, a);       // candado dentro
    fillRoundRectA(x + s / 3, y + s - 5, s / 3, 3, 1, c, a);           // pie
  }
}
static void ctxRender(float p){
  if(p < 0) p = 0; if(p > 1) p = 1;
  float ease = 1 - (1 - p) * (1 - p);                 // ease-out
  float sc = 0.88f + 0.12f * ease;                    // escala
  uint8_t a = (uint8_t)(255.0f * ease);               // fundido
  setBuf(bbuf);
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  // Solo la banda que ocupa el panel, no la pantalla entera. La banda se calcula
  // en ctxOpen contra la posicion FINAL (escala 1); como la escala encoge el
  // panel hacia su centro, ningun frame intermedio se sale de ella.
  for(int j = ctxBandY0; j <= ctxBandY1; j++)
    memcpy(bbuf + (size_t)j * SCR_W, homeBuf + (size_t)j * SCR_W, SCR_W * 2);
  float ccx = (float)ctxPx + CTX_W / 2.0f;
  float ccy = (float)ctxPy + CTX_PANEL_H / 2.0f;
  int px = (int)(ccx + ((float)ctxPx - ccx) * sc);
  int py = (int)(ccy + ((float)ctxPy - ccy) * sc);
  int pw = (int)(CTX_W * sc), ph = (int)(CTX_PANEL_H * sc);
  int rad = (int)(CTX_RAD * sc);
  // UNA sola tarjeta para las tres filas. drawLiquidGlassPanel no acepta alpha,
  // asi que mientras crece se usa su MISMO tinte como color plano y solo el
  // frame final pasa al vidrio real: lo unico que aparece entonces es el
  // desenfoque, no un salto de color.
  fillRoundRectA(px, py, pw, ph, rad, ctxPanelCol(), (uint8_t)(238 * (int)a / 255));
  if(uiGlass && p >= 1.0f) drawLiquidGlassPanel(px, py, pw, ph, rad, SET_CARD_GLASS);
  int rh = ph / CTX_ROWS;
  int textMax = CTX_W - CTX_PAD_L - CTX_GLYPH_S - CTX_PAD_R - 10;
  for(int i = 0; i < CTX_ROWS; i++){
    int ry = py + i * rh;
    if(i > 0) fillRectA(px + 14, ry, pw - 28, 1, SET_TXT_MUTE, (uint8_t)(95 * (int)a / 255));  // separador
    bool en = ctxRowEnabled(i);
    const char* lb = ctxLabel(i);
    // El tamano se calcula contra el ancho FINAL, no el escalado: si se
    // recalculara por frame podria saltar a mitad de la animacion.
    int fs = uiFontFit(lb, textMax, 3);
    drawTextA(px + CTX_PAD_L, ry + rh / 2 - uiLineH(fs) / 2, lb, fs,
              en ? SET_TXT_HI : SET_TXT_MUTE, a);
    ctxGlyph(i, px + pw - CTX_PAD_R - CTX_GLYPH_S, ry + rh / 2 - CTX_GLYPH_S / 2,
             CTX_GLYPH_S, en ? a : (uint8_t)((int)a * 110 / 255));
  }
  present(ctxBandY0, ctxBandY1);
  setBuf(fb);
}
static void ctxOpen(int slot){
  if(!CTXMENU_ON || slot < 0 || slot > 11) return;
  ctxApp = homeOrder[slot];
  // Geometria REAL del icono pulsado: la misma rejilla que pinta renderHome()
  // (gx0=24, gy0=212, paso de columna 120, paso de fila 112, icono de 72).
  int ix = 24 + (slot % 4) * 120;
  int iy = 212 + (slot / 4) * 112;
  // Lado: se prefiere la DERECHA del icono, pero solo si el panel cabe entero
  // ahi; si no, la izquierda. Si no cabe en ninguno de los dos (panel mas ancho
  // de la cuenta), se elige el lado con MAS sitio antes de recortar -- asi el
  // recorte de abajo nunca acaba dejando el panel encima del icono pulsado, que
  // es justo el que el usuario necesita seguir viendo.
  int roomR = (SCR_W - CTX_MARGIN) - (ix + CTX_ICON_S + CTX_GAPX);
  int roomL = (ix - CTX_GAPX) - CTX_MARGIN;
  bool toRight = (roomR >= CTX_W) ? true : (roomL >= CTX_W ? false : (roomR >= roomL));
  int px = toRight ? (ix + CTX_ICON_S + CTX_GAPX) : (ix - CTX_GAPX - CTX_W);
  int py = iy;                                  // alineado con el borde superior del icono
  // RECORTE FINAL, incondicional: pase lo que pase con el lado elegido, el panel
  // entero queda dentro de pantalla. Es lo que garantiza que ningun icono de la
  // ultima fila o columna deje el menu a medias o fuera de alcance.
  if(px < CTX_MARGIN) px = CTX_MARGIN;
  if(px > SCR_W - CTX_MARGIN - CTX_W) px = SCR_W - CTX_MARGIN - CTX_W;
  if(py < CTX_MARGIN) py = CTX_MARGIN;
  if(py > SCR_H - CTX_MARGIN - CTX_PANEL_H) py = SCR_H - CTX_MARGIN - CTX_PANEL_H;
  ctxPx = px; ctxPy = py;
  ctxBandY0 = py - 2; if(ctxBandY0 < 0) ctxBandY0 = 0;
  ctxBandY1 = py + CTX_PANEL_H + 2; if(ctxBandY1 > SCR_H - 1) ctxBandY1 = SCR_H - 1;
  ctxAction = -1; ctxClosing = false;
  ctxAnimMs = millis(); if(!ctxAnimMs) ctxAnimMs = 1;
  gRippleActive = false;
  gState = ST_CTX;
}
// La accion NO se ejecuta al tocar: se guarda y se ejecuta cuando termina la
// animacion de cierre, para que el panel nunca desaparezca de golpe.
static void ctxClose(int action){
  ctxAction = action; ctxClosing = true;
  ctxAnimMs = millis(); if(!ctxAnimMs) ctxAnimMs = 1;
}
static void ctxFinish(){
  int a = ctxAction, app = ctxApp;
  ctxAction = -1; ctxClosing = false; ctxAnimMs = 0; ctxApp = -1;
  gState = ST_HOME;
  showHome();                          // escritorio limpio en un solo volcado
  // a < 0 = cancelado (toque fuera del panel): no hay nada que hacer.
  if(a == 0 && ctxRowEnabled(0)){
    // Poner Y quitar el candado exigen clave: asi nadie desbloquea la app de
    // otro con solo tocar el icono. Misma ruta de verificacion que todo lo demas.
    lsuStartVerifyFor(appLockGet(app) ? LSU_AFTER_UNLOCKAPP : LSU_AFTER_LOCKAPP, app);
  } else if(a == 1){
    // Modo Edicion: el comportamiento de siempre, pero SIN agarrar el icono --
    // cuando se elige esta fila el dedo ya se levanto del icono hace rato.
    edEnter();
  } else if(a == 2 && ctxRowEnabled(2)){
    kioskSetEnter(app);
  }
}
static void ctxTick(){
  if(ctxAnimMs){
    uint32_t e = millis() - ctxAnimMs;
    float p = (float)e / (float)CTX_ANIM_MS; if(p > 1.0f) p = 1.0f;
    ctxRender(ctxClosing ? (1.0f - p) : p);
    if(e >= (uint32_t)CTX_ANIM_MS){
      ctxAnimMs = 0;
      if(ctxClosing) ctxFinish();
    }
    return;
  }
  if(T.tap){
    for(int i = 0; i < CTX_ROWS; i++){
      int y = ctxPy + i * CTX_ROW_H;
      if(T.x >= ctxPx && T.x <= ctxPx + CTX_W && T.y >= y && T.y <= y + CTX_ROW_H){
        if(!ctxRowEnabled(i)) return;                 // inerte: ni siquiera cierra el menu
        ctxClose(i); return;
      }
    }
    ctxClose(-1);                                     // fuera del panel -> cancelar
  }
}

// #############################################################
// ##  FASE 3 - QUE HACER TRAS UNA VERIFICACION CORRECTA
// ##  Un unico punto de salida para las cuatro rutas nuevas, asi
// ##  lsuStartVerify sigue siendo la UNICA ruta de verificacion.
// #############################################################
static void lsuFinishAfter(){
  int what = lsuAfter, id = lsuAfterApp;
  lsuAfter = LSU_AFTER_UNLOCK; lsuAfterApp = -1;
  if(what == LSU_AFTER_KIOSKOUT){ kioskExitNow(); return; }
  // Apagado seguro: PIN correcto -> se continua con la animacion de apagado. No
  // se repinta el escritorio de abajo a proposito: la animacion arranca haciendo
  // un fundido a negro DESDE lo que ya hay en pantalla.
  if(what == LSU_AFTER_POWEROFF){ poffBeginAnim(); return; }
  gState = ST_HOME;
  if(what == LSU_AFTER_LOCKAPP)        appLockSet(id, true);
  else if(what == LSU_AFTER_UNLOCKAPP) appLockSet(id, false);
  renderHome(); showHome();                            // borra la pantalla de verificacion
  if(what == LSU_AFTER_OPENAPP && id >= 0) enterApp(id);
}
static void lsuStartVerifyFor(int what, int id){
  if(gLockType == 0) return;                           // sin clave no hay nada que verificar
  lsuStartVerify();                                    // deja lsuAfter en UNLOCK; se ajusta justo aqui
  lsuAfter = what; lsuAfterApp = id;
}

static int  utf8Count(const char* s){ int n = 0; while(*s){ if((*s & 0xC0) != 0x80) n++; s++; } return n; }
static void lsuPassAppend(const char* s){ int L = strlen(lsuPass), sl = strlen(s); if(L + sl < (int)sizeof(lsuPass) - 1){ memcpy(lsuPass + L, s, sl); lsuPass[L + sl] = 0; } }
static void lsuExit(){
  // FASES 3 y 4: cancelar una verificacion de app o de kiosco NO debe bloquear la
  // pantalla (que es lo que hacia la rama de abajo). Y sobre todo: cancelar la
  // salida del kiosco tiene que devolver A LA APP, nunca al escritorio -- si no,
  // la propia flecha de "atras" seria la via de escape del modo kiosco.
  // SEGURIDAD: si la verificacion salio de la pantalla de Bloqueo, cancelar
  // vuelve al BLOQUEO, nunca al escritorio. Va lo PRIMERO porque el despertar de
  // una suspension usa LSU_AFTER_OPENAPP (para volver a la app donde estabas), y
  // sin esta rama caeria por la de abajo, que termina en ST_HOME + showHome():
  // el escritorio a la vista sin haber introducido la clave.
  if(lsuVerify && gLockVerifyLocked){
    gLockVerifyLocked = false;
    lsuVerify = false; lsuAfter = LSU_AFTER_UNLOCK; lsuAfterApp = -1;
    lsuShakeMs = 0; lockWaitReset();
    gState = ST_LOCK; lockOff = 0; lastLockOff = -1;
    renderLock(); showLock();
    return;
  }
  if(lsuVerify && lsuAfter != LSU_AFTER_UNLOCK){
    bool wasKiosk = (lsuAfter == LSU_AFTER_KIOSKOUT);
    bool wasPoff  = (lsuAfter == LSU_AFTER_POWEROFF);
    lsuVerify = false; lsuAfter = LSU_AFTER_UNLOCK; lsuAfterApp = -1;
    lsuShakeMs = 0; lockWaitReset();
    // Cancelar la verificacion del apagado NO apaga y NO se va al escritorio:
    // devuelve a la pantalla de confirmacion, con el slider otra vez en reposo.
    if(wasPoff && POWEROFF_ON){ poffEnter(); return; }
    if(wasKiosk && KIOSK_ON && kioskOn && kioskApp >= 0){
      renderHome();                       // winRevealAnim compone sobre homeBuf
      enterApp(kioskApp);
      kioskShowBadge();
    } else {
      gState = ST_HOME; renderHome(); showHome();
    }
    return;
  }
  if(lsuVerify){ lsuVerify = false; gState = ST_LOCK; lockOff = 0; lastLockOff = -1; renderLock(); showLock(); }
  else { gState = ST_APP; settingsRender(); }
}
static void lsuUnlock(){
  lockOnSuccess();                       // FASE 1: acierto -> contador de fallos a cero
  lsuShakeMs = 0;
  gLockVerifyLocked = false;             // clave correcta: ya no estamos "detras del bloqueo"
  lsuVerify = false; lsuWrong = 0; lockOff = 0; lastLockOff = -1;
  // FASES 3 y 4: si la verificacion no era para desbloquear la PANTALLA, el
  // destino lo decide lsuFinishAfter (abrir app, poner/quitar candado, salir del
  // kiosco). La animacion de revelado del escritorio de abajo no aplica ahi.
  if(lsuAfter != LSU_AFTER_UNLOCK){ lsuFinishAfter(); return; }
  gState = ST_HOME;
  renderHome();                          // compone el home en homeBuf
  uint32_t t0 = millis(), dur = 400;     // 0.4s: aparecer desvanecido + leve temblor
  for(;;){
    uint32_t e = millis() - t0; if(e > dur) e = dur;
    float p = (float)e / dur;
    uint8_t a = (uint8_t)(p * 255);
    int sh = (int)((1.0f - p) * 6.0f * sinf(e * 0.05f));   // temblor que decae
    for(int j = 0; j < SCR_H; j++){
      uint16_t* d  = bbuf + (size_t)j * SCR_W;
      uint16_t* bg = (blurBg ? blurBg : homeBuf) + (size_t)j * SCR_W;
      uint16_t* hm = homeBuf + (size_t)j * SCR_W;
      for(int i = 0; i < SCR_W; i++){
        int si = i - sh; if(si < 0) si = 0; if(si >= SCR_W) si = SCR_W - 1;
        d[i] = mix565(bg[i], hm[si], a);
      }
    }
    present(0, SCR_H - 1);
    if(e >= dur) break;
  }
  showHome();
}
static void lsuSavePin(){ prefs.begin("flexos", false); prefs.putString("lockpin", lsuPin); prefs.putInt("locktype", 1); prefs.end(); gLockType = 1; lsuExit(); }
static void lsuSavePass(){ prefs.begin("flexos", false); prefs.putString("lockpass", lsuPass); prefs.putInt("locktype", 2); prefs.end(); gLockType = 2; lsuExit(); }
static void lsuBack(){ uint16_t c = lsuTxtHi(); strokeSegAA(30, 26, 18, 18, 2.4f, c); strokeSegAA(18, 18, 30, 10, 2.4f, c); }

// ---- Selector PIN / Contraseña ----
static void lsuRenderSel(){
  setBuf(bbuf);
  fillRect(0, 0, SCR_W, SCR_H, lsuBgCol());
  lsuBack();
  drawTextC(SCR_W / 2, 74, "Bloqueo de pantalla", 3, lsuTxtHi());
  drawTextC(SCR_W / 2, 118, "Elige un metodo", 2, lsuTxtLo());
  int bw = SCR_W - 80, bh = 120, y1 = 220, y2 = y1 + bh + 30;
  const char* lbl[2] = { "PIN", "Contrase\xC3\xB1" "a" }; int ys[2] = { y1, y2 };
  for(int k = 0; k < 2; k++){
    if(uiGlass){ drawLiquidGlassPanel(40, ys[k], bw, bh, 22, rgb565(50,90,200)); }
    else fillRoundRect(40, ys[k], bw, bh, 22, rgb565(46,82,182));
    drawTextC(SCR_W / 2, ys[k] + bh / 2 - 18, lbl[k], 4, rgb565(255,255,255));
  }
  present(0, SCR_H - 1);
}

// ---- Pantalla PIN (teclado numerico Liquid Glass + feedback de tecleo) ----
static void lsuPinRect(int i, int &x, int &y, int &w, int &h){
  int c = i % 3, r = i / 3, bw = 132, bh = 82, gap = 12;
  int tot = 3 * bw + 2 * gap, x0 = (SCR_W - tot) / 2, y0 = 300;
  x = x0 + c * (bw + gap); y = y0 + r * (bh + gap); w = bw; h = bh;
}
static void lsuComposePin(){                          // base de vidrio en lockBuf (SOLO una vez)
  setBuf(lockBuf);
  lsuBg();
  lsuBack();
  drawTextC(SCR_W / 2, 60, lsuVerify ? "Introduce el PIN" : "Crear PIN", lsuVerify ? 3 : 4, lsuTxtHi());
  for(int i = 0; i < 12; i++){
    int x, y, w, h; lsuPinRect(i, x, y, w, h);
    if(uiGlass){ drawLiquidGlassPanel(x, y, w, h, 16, lsuGlassCol()); }
    else fillRoundRect(x, y, w, h, 16, lsuCardCol());
    uint16_t col = (i == 9) ? rgb565(230,180,90) : (i == 11) ? rgb565(120,220,150) : lsuTxtHi();
    drawTextC(x + w / 2, y + h / 2 - 12, PIN_KEYS[i], 3, col);
  }
  setBuf(bbuf);
}
// El primer cuadro del PIN entra con el fundido de la transicion de seguridad
// (si la hubo). authFadeIn devuelve false cuando no hay transicion pendiente
// -- p.ej. al CREAR el PIN desde Ajustes -- y entonces se publica de una vez,
// exactamente como antes.
static void lsuShowPin(){
  lsuComposePin();
  if(authFadeIn(lockBuf)) return;
  memcpy(bbuf, lockBuf, (size_t)SCR_W * SCR_H * 2); present(0, SCR_H - 1);
}
static void lsuAnimPin(){                              // puntos dinamicos + destello + flash (NO re-desenfoca)
  setBuf(bbuf);
  // FASE 1: sh != 0 solo durante los ~6 frames de la sacudida. La banda se
  // copia DESPLAZADA en horizontal, asi que puntos y teclado se mueven juntos
  // sin volver a dibujar ni un panel de vidrio; los bordes se rellenan
  // repitiendo la columna extrema (clamp), nunca con negro.
  int sh = lsuShakeOff();
  if(sh == 0){
    for(int j = 120; j < 700; j++) memcpy(bbuf + (size_t)j * SCR_W, lockBuf + (size_t)j * SCR_W, SCR_W * 2);
  } else {
    for(int j = 120; j < 700; j++){
      uint16_t* d = bbuf + (size_t)j * SCR_W;
      const uint16_t* s = lockBuf + (size_t)j * SCR_W;
      for(int i = 0; i < SCR_W; i++){
        int si = i - sh; if(si < 0) si = 0; if(si >= SCR_W) si = SCR_W - 1;
        d[i] = s[si];
      }
    }
  }
  int n = strlen(lsuPin);
  uint16_t dc = (lsuWrong && millis() - lsuWrong < 500) ? rgb565(235,70,70) : rgb565(90,150,240);
  for(int i = 0; i < 8; i++){ int cx = SCR_W / 2 - 4 * 28 + 14 + i * 28 + sh; if(i < n) fillCircle(cx, 150, 8, dc); else drawCircle(cx, 150, 8, rgb565(90,100,130)); }
  for(int i = 0; i < 12; i++){
    int x, y, w, h; lsuPinRect(i, x, y, w, h);
    if(i == lsuPress){ float p = (millis() - lsuPressMs) / 200.0f; if(p < 1) fillRoundRectA(x + sh, y, w, h, 16, rgb565(255,255,255), (uint8_t)((1 - p) * 90)); else lsuPress = -1; }
  }
  present(120, 700);
}

// ---- Teclado alfanumerico para contraseña (con offset para el slide) ----
// xoff = sacudida horizontal de la FASE 1. El panel de fondo se dibuja SIN
// desplazar y solo las teclas se mueven dentro de el, asi que la sacudida nunca
// deja una franja vacia en los bordes de la pantalla.
static void lsuDrawKb(int yoff, int xoff){
  int ky = KB_Y + yoff;
  // Aqui NO hay barra superior ni chips (kbExtrasOn queda en false en esta
  // pantalla), asi que el panel empieza justo encima de las teclas, igual que
  // siempre. Lo unico que cambia con la Fase A es que el tamano ya no es fijo.
  if(uiGlass){ drawLiquidGlassPanel(0, ky - 4, SCR_W, SCR_H - (ky - 4), 0, lsuKbGlass()); }
  else fillRect(0, ky - 4, SCR_W, SCR_H - (ky - 4), lsuKbBgCol());
  int fs = kbFontSize();
  for(int r = 0; r < KB_ROWS; r++) for(int c = 0; c < KB_COLS; c++){
    int x = KB_X + c * (KB_KW + KB_GAP) + xoff, y = ky + r * (KB_KH + KB_GAP);
    const char* k = mapaActivo[r][c];
    char u[6];
    if(kbShift && k[1] == 0 && k[0] >= 'a' && k[0] <= 'z'){ u[0] = (char)(k[0] - 32); u[1] = 0; k = u; }
    int cell = r * KB_COLS + c;
    kbPaintKey(x, y, KB_KW, KB_KH, k, fs, lsuKeyCol(), lsuKeyTxt(), kbCellHeld(cell) || kbFxLevel(cell) > 0);
  }
  int fy = ky + 3 * (KB_KH + KB_GAP);
  const char* lb[KB_FKEYS] = { "shift", kbLayerLabel(), kbLangEs ? "ES" : "EN", "espacio", "<-", "OK" };
  for(int i = 0; i < KB_FKEYS; i++) kbFKey(kbFKeyX(i) + xoff, fy, kbFKeyW(i), lb[i], (i == 0) && kbShift);
}
// Pintado de la pantalla de contrasena en el gBuf ACTUAL (sin publicar). Se
// separo del volcado para poder componerla fuera de pantalla y usarla como
// destino del fundido de entrada de la transicion de seguridad.
static void lsuPaintPass(int yoff, int xoff){
  lsuBg();
  lsuBack();
  drawTextC(SCR_W / 2, 50, lsuVerify ? "Introduce contrase\xC3\xB1" "a" : "Crear contrase\xC3\xB1" "a", 3, lsuTxtHi());
  int cnt = utf8Count(lsuPass);
  for(int i = 0; i < cnt && i < 18; i++) fillCircle(30 + i * 24 + xoff, 120, 7, rgb565(90,150,240));
  lsuDrawKb(yoff, xoff);
}
static void lsuRenderPass(int yoff, int xoff){
  setBuf(bbuf);
  lsuPaintPass(yoff, xoff);
  present(0, SCR_H - 1);
}
// Primer cuadro de la contrasena: se compone con el teclado todavia FUERA de
// pantalla, se funde desde el fondo y a partir de ahi arranca el deslizamiento
// del teclado de siempre. Asi la aparicion es fundido + deslizamiento, no un
// salto seco.
static void lsuShowPassFirst(){
  if(authFadePending && authSnap){
    uint16_t* old = gBuf;
    gBuf = authSnap;                                  // lienzo fuera de pantalla (ya no hace falta la instantanea)
    lsuPaintPass(SCR_H - KB_Y, 0);
    gBuf = old;
    authFadeIn(authSnap);
  }
  lsuKbAnim = millis();
}

static void lsuEnter(){
  // FASE 4: en kiosco NO se puede cambiar la clave del sistema. Si la app clavada
  // fuera Ajustes, quien tenga el telefono entraria en Seguridad -> Bloqueo, se
  // pondria un PIN nuevo y saldria con el: la unica llave del kiosco es la clave
  // que ya estaba puesta antes de prestarlo.
  if(KIOSK_ON && kioskOn) return;
  gState = ST_LOCKSETUP; lsuMode = LSU_SEL; lsuPin[0] = 0; lsuPass[0] = 0; lsuPress = -1; lsuKbAnim = 0;
  mapaActivo = LAYOUT_ES; kbLangEs = true; kbShift = false;
  kbExtrasOn = false; kbApplySize(); kbMtSurfaceReset();   // sin barra ni chips en la pantalla de clave
  lsuRenderSel();
}
static void lsuTick(){
  if(lsuMode == LSU_SEL){
    // El selector es ESTATICO (lo pinta lsuEnter): no hay nada que animar aqui.
    if(T.tap){
      if(T.x < 48 && T.y < 48){ lsuExit(); return; }
      int bw = SCR_W - 80, y1 = 220, bh = 120, y2 = y1 + bh + 30;
      if(T.x >= 40 && T.x <= 40 + bw && T.y >= y1 && T.y <= y1 + bh){ lsuMode = LSU_PIN; lsuShowPin(); return; }
      if(T.x >= 40 && T.x <= 40 + bw && T.y >= y2 && T.y <= y2 + bh){ lsuMode = LSU_PASS; lsuKbAnim = millis(); return; }
    }
    return;
  }
  if(lsuMode == LSU_PIN){
    // FASE 1 - la sacudida va PRIMERO: mientras dura (~6 frames) el teclado no
    // acepta pulsaciones, y solo cuando termina aparece el contador de espera.
    if(lsuShakeMs){
      if(millis() - lsuAnimMs > 30){ lsuAnimMs = millis(); lsuAnimPin(); }
      return;
    }
    // FASE 1 - espera forzada: el teclado esta inerte y solo se anima el
    // contador regresivo por diffing. Sin delay() bloqueante: esto es un
    // estado con marca de tiempo que se evalua en cada vuelta del loop.
    if(lockWaitActive()){
      // Se permite salir con la flecha: la espera NO se pierde al salir (sigue
      // viva en lockWaitUntil), asi que esto no es una via de escape -- solo
      // evita quedarse cinco minutos atrapado en una pantalla inerte.
      if(T.tap && T.x < 48 && T.y < 48){ lockWaitReset(); lsuExit(); return; }
      lockWaitTick();
      return;
    }
    // Se cumplio la espera: el contador se borra en el MISMO present() con el
    // que lsuAnimPin repinta su banda (120..700, que ya contiene esta zona),
    // no en un volcado aparte.
    if(lockWaitPainted){ lockWaitReset(); lsuAnimMs = 0; }
    if(T.tap){
      if(T.x < 48 && T.y < 48){ lsuExit(); return; }
      for(int i = 0; i < 12; i++){ int x, y, w, h; lsuPinRect(i, x, y, w, h);
        if(T.x >= x && T.x <= x + w && T.y >= y && T.y <= y + h){
          lsuPress = i; lsuPressMs = millis();
          if(i == 9){ int L = strlen(lsuPin); if(L > 0) lsuPin[L - 1] = 0; }                         // borrar
          else if(i == 11){                                                                          // OK
            if(lsuVerify){ if(!strcmp(lsuPin, lsuSaved)) lsuUnlock(); else { lsuWrong = millis(); lsuPin[0] = 0; lockOnFail(); lsuShakeStart(); } return; }
            else if(strlen(lsuPin) >= 4){ lsuSavePin(); return; }
          }
          else if(strlen(lsuPin) < 8){                                                               // digito
            int L = strlen(lsuPin); lsuPin[L] = PIN_KEYS[i][0]; lsuPin[L + 1] = 0;
            if(lsuVerify && (int)strlen(lsuPin) == (int)strlen(lsuSaved) && strlen(lsuSaved) > 0){
              if(!strcmp(lsuPin, lsuSaved)) lsuUnlock(); else { lsuWrong = millis(); lsuPin[0] = 0; lockOnFail(); lsuShakeStart(); }
              return;
            }
          }
          break;
        }
      }
    }
    if(millis() - lsuAnimMs > 30){ lsuAnimMs = millis(); lsuAnimPin(); }        // anim con throttle (responsivo)
    return;
  }
  // LSU_PASS
  if(lsuKbAnim){                                        // animacion de apertura del teclado = 0.3s EXACTOS
    float p = (millis() - lsuKbAnim) / 300.0f; if(p >= 1){ p = 1; lsuKbAnim = 0; }
    int kbh = SCR_H - KB_Y;
    lsuRenderPass((int)((1.0f - p) * kbh), 0);
    return;
  }
  // FASE 1 - misma secuencia que en el PIN: primero sacudir, luego esperar.
  // Aqui lo que se mueve son las TECLAS dentro de su panel (el campo de puntos
  // queda vacio al fallar, asi que sacudirlo no se veria).
  if(lsuShakeMs){
    if(millis() - lsuAnimMs > 30){ lsuAnimMs = millis(); lsuRenderPass(0, lsuShakeOff()); }
    return;
  }
  if(lockWaitActive()){
    if(T.tap && T.x < 48 && T.y < 48){ lockWaitReset(); lsuExit(); return; }
    lockWaitTick();
    return;
  }
  // Aqui el borrado sale gratis en el mismo volcado: lsuRenderPass recompone la
  // pantalla completa de una vez.
  if(lockWaitPainted){ lockWaitReset(); lsuRenderPass(0, 0); }
  // FASE G - apagar el destello de la ultima tecla al cumplir su tiempo.
  kbFxTick(lsuKeyCol(), lsuKeyTxt());
  // FASE B - via rapida tambien aqui: escribir la contrasena rapido no deberia
  // perder letras. Las teclas de FUNCION que confirman o borran (OK, <-) NO
  // entran por esta via: una confirmacion tiene que salir de un toque
  // deliberado, no de un roce mientras el dedo anterior se levanta.
  if(T.down && T.y >= KB_Y - 8) kbTypingMark();   // veto del gesto de suspension mientras se teclea
  if(KB_MULTITOUCH_ON && gKbFastType){
    int n = kbMtPoll();
    bool wrote = false;
    for(int e = 0; e < n; e++){
      int cell = kbEvCell[e];
      if(cell < 0) continue;
      const char* k = mapaActivo[cell / KB_COLS][cell % KB_COLS];
      if(kbShift && k[1] == 0 && k[0] >= 'a' && k[0] <= 'z'){ char u[2] = { (char)(k[0] - 32), 0 }; lsuPassAppend(u); kbShift = false; }
      else lsuPassAppend(k);
      kbFxStart(cell); wrote = true;
    }
    if(wrote) lsuRenderPass(0, 0);
  }
  // FASE G: mismo criterio que en Notas -- sin escritura rapida, el destello va
  // en el flanco de PRESIONAR, que si no la tecla no da ninguna senal.
  if(T.pressed && !(KB_MULTITOUCH_ON && gKbFastType)) kbFxPress(kbCellAt(T.x, T.y), lsuKeyCol(), lsuKeyTxt());
  if(T.tap){
    if(T.x < 48 && T.y < 48){ lsuExit(); return; }
    int fi = kbFRowHit(T.x, T.y);
    if(fi >= 0){
      if(fi == 0) kbShift = !kbShift;
      else if(fi == 1) mapaActivo = (mapaActivo == LAYOUT_NUM) ? LAYOUT_EMOJI : (mapaActivo == LAYOUT_EMOJI) ? (kbLangEs ? LAYOUT_ES : LAYOUT_EN) : LAYOUT_NUM;
      else if(fi == 2){ kbLangEs = !kbLangEs; if(mapaActivo == LAYOUT_ES || mapaActivo == LAYOUT_EN) mapaActivo = kbLangEs ? LAYOUT_ES : LAYOUT_EN; }
      else if(fi == 3) lsuPassAppend(" ");
      else if(fi == 4){ int L = strlen(lsuPass); if(L > 0){ int q = L - 1; while(q > 0 && (lsuPass[q] & 0xC0) == 0x80) q--; lsuPass[q] = 0; } }
      else { if(lsuVerify){ if(!strcmp(lsuPass, lsuSaved)) lsuUnlock(); else { lsuWrong = millis(); lsuPass[0] = 0; lockOnFail(); lsuShakeStart(); lsuRenderPass(0, 0); } return; } else if(strlen(lsuPass) >= 4){ lsuSavePass(); return; } }
      lsuRenderPass(0, 0); return;
    }
    if(kbFastActive()) return;                          // ya la escribio la via rapida
    int cell = kbCellAt(T.x, T.y);
    if(cell >= 0){
      const char* k = mapaActivo[cell / KB_COLS][cell % KB_COLS];
      if(kbShift && k[1] == 0 && k[0] >= 'a' && k[0] <= 'z'){ char u[2] = { (char)(k[0] - 32), 0 }; lsuPassAppend(u); kbShift = false; }
      else lsuPassAppend(k);
      kbFxStart(cell);
      lsuRenderPass(0, 0);
    }
  }
}

static void lsuStartVerify(){
  // RED DE SEGURIDAD (misma que appClose/activarMultitarea): la UI de
  // verificacion SIEMPRE se compone en portrait y a pantalla completa. Sin esto,
  // llegar aqui desde una app landscape -Juegos es la unica con APP_LAND, y
  // geoRenderGame pone gLand=true en CADA frame- pintaba el teclado girado y
  // recortado; y como el remapeo del tactil tambien depende de gLand, los
  // digitos no caian donde se veian: no habia forma de escribir el PIN ni de
  // salir de esa pantalla. No se restaura al terminar A PROPOSITO: quien vuelva
  // a una app landscape lo hace via enterApp -> geoEnter, que pone su gLand=true
  // por su cuenta, en vez de heredarlo de la pantalla de verificacion.
  gLand = false;
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  setBuf(fb);
  prefs.begin("flexos", true);
  String s = (gLockType == 1) ? prefs.getString("lockpin", "") : prefs.getString("lockpass", "");
  prefs.end();
  s.toCharArray(lsuSaved, sizeof(lsuSaved));
  lsuVerify = true; lsuWrong = 0; lsuPin[0] = 0; lsuPass[0] = 0; lsuPress = -1;
  mapaActivo = LAYOUT_ES; kbLangEs = true; kbShift = false;
  kbExtrasOn = false; kbApplySize(); kbMtSurfaceReset();   // sin barra ni chips en la verificacion
  ensureBlurBg();
  gState = ST_LOCKSETUP;
  // Por defecto esta verificacion es la de la PANTALLA. lsuStartVerifyFor lo
  // reajusta despues de llamar aqui, asi que ninguna ruta puede heredar por
  // accidente el destino de una verificacion anterior.
  lsuAfter = LSU_AFTER_UNLOCK; lsuAfterApp = -1;
  lsuShakeMs = 0;
  lockWaitPainted = false; lockWaitLastSec = -1;
  lockArmPendingPenalty();               // FASE 1: contador ya alto -> se cobra antes del primer intento
  // TRANSICION: primero se va la interfaz actual, luego entra el metodo de
  // seguridad configurado. Va justo aqui, despues de dejar listo el estado y
  // ANTES de pintar el primer cuadro de la clave.
  authFadeOut();
  if(gLockType == 1){ lsuMode = LSU_PIN; lsuShowPin(); }
  else { lsuMode = LSU_PASS; lsuShowPassFirst(); }
}

// #############################################################
// ##  SUSPENSION + BLOQUEO
// ##  ------------------------------------------------------
// ##  Cuerpo de suspWakeLockScreen(), cuyo prototipo esta arriba
// ##  junto al detector de doble-tap. Va AQUI ABAJO porque necesita
// ##  gState, editMode, gRippleActive, renderLock y showLock, que
// ##  todavia no existen alla arriba.
// ##
// ##  La llama suspWake() con la pantalla TODAVIA a oscuras, antes
// ##  del DISPON y antes de que el backlight empiece a subir. Por
// ##  eso el contenido anterior nunca llega a verse: cuando hay luz,
// ##  lo que hay en el framebuffer ya es el bloqueo.
// #############################################################
static void suspWakeLockScreen(){
#if SUSPEND_ON && SUSPEND_LOCK_ON
  // Sin PIN/contrasena configurada NO se bloquea nada: seria pedirle al usuario
  // que "desbloquee" con una clave que no existe. Se despierta donde estaba,
  // que es el comportamiento de siempre.
  if(gLockType == 0) return;
  // Ya estaba en el bloqueo (o metiendo la clave) al suspender: nada que hacer y
  // nada que restaurar.
  if(gState == ST_LOCK || gState == ST_LOCKSETUP){ gSuspRetState = -1; gSuspRetApp = -1; return; }
  // A donde volver cuando acierte la clave (lo consume lockStartVerify).
  gSuspRetState = gState;
  gSuspRetApp   = gAppId;
  // Dejar el sistema en un estado limpio antes de tapar con el bloqueo. NO se
  // llama a appClose(): cerrar la app aqui es justo lo que haria perder el sitio
  // -- la app se deja viva y se vuelve a ella con enterApp() tras desbloquear.
  if(editMode) edExit();
  gRippleActive = false;
  qsPanelY = 0;                     // la cortina no puede quedar a medio abrir bajo el bloqueo
  gLand = false;                    // el bloqueo SIEMPRE se compone en portrait (apps landscape)
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  gState = ST_LOCK; lockOff = 0; lastLockOff = -1;
  renderLock();
  showLock();
#endif
}

// #############################################################
// ##  APAGADO COMPLETO (deep sleep real)
// ##  ------------------------------------------------------
// ##  Ruta: Panel Rapido -> circulo "Apagar" -> ST_POWEROFF_CONFIRM
// ##  (slider "desliza para apagar" + Cancelar) -> [PIN opcional]
// ##  -> ST_POWEROFF_ANIM (fundido a negro + "Flex OS" + fundido del
// ##  texto) -> backlight a 0 -> DCS de bajo consumo -> deep sleep.
// ##
// ##  Todo interpolado por millis(), cero delay() bloqueante, cero
// ##  fillScreen() de pantalla completa como mecanismo de apagado, y
// ##  toda composicion pasa por bbuf + present().
// #############################################################

// ---- Geometria de la pantalla de confirmacion ---------------------------
#define POFF_TRACK_X   40                      // pista del slider
#define POFF_TRACK_Y   352
#define POFF_TRACK_W   (SCR_W - 80)            // 400
#define POFF_TRACK_H   96
#define POFF_TRACK_R   (POFF_TRACK_H / 2)
#define POFF_KNOB_PAD  6                       // margen del pomo dentro de la pista
#define POFF_KNOB_D    (POFF_TRACK_H - 2 * POFF_KNOB_PAD)   // 84
#define POFF_KNOB_R    (POFF_KNOB_D / 2)
#define POFF_RUN       (POFF_TRACK_W - 2 * POFF_KNOB_PAD - POFF_KNOB_D)   // recorrido util del pomo
#define POFF_DONE_PCT  92                      // % del recorrido que cuenta como "completado"
#define POFF_BAND_Y0   (POFF_TRACK_Y - 8)      // banda que se repinta cada frame
#define POFF_BAND_Y1   (POFF_TRACK_Y + POFF_TRACK_H + 8)
#define POFF_BAND_H    (POFF_BAND_Y1 - POFF_BAND_Y0 + 1)
#define POFF_CAN_X     (SCR_W / 2 - 110)       // boton Cancelar
#define POFF_CAN_Y     560
#define POFF_CAN_W     220
#define POFF_CAN_H     72
#define POFF_CAN_R     (POFF_CAN_H / 2)

// ---- Tiempos de la animacion de apagado (todo interpolado) --------------
#define POFF_FADE_MS   520                     // 1) fundido a negro de la pantalla de confirmacion
#define POFF_TIN_MS    260                     // 2) aparicion del texto "Flex OS"
#define POFF_HOLD_MS   700                     //    ... y cuanto se queda quieto
#define POFF_TOUT_MS   620                     // 3) disolucion del texto
#define POFF_TXT_Y     (SCR_H / 2 - 26)        //    linea base del texto centrado
#define POFF_TXT_SZ    5
// Banda del texto: lo UNICO que cambia en las fases 2 y 3. A size 5 la caja de
// linea mide FONT_LINEH * fontSc(5) = 51 * 8*5/51 = 40 px; se deja margen de
// sobra por arriba (ascendentes) y por abajo (descendentes) para que ningun
// glifo pueda quedar recortado por el clip.
#define POFF_TXT_BY0   (POFF_TXT_Y - 24)
#define POFF_TXT_BY1   (POFF_TXT_Y + 80)

// ---- Estado ------------------------------------------------------------
static uint16_t* poffBand   = NULL;   // cache de la banda ESTATICA del slider (pista vacia + vidrio)
static int   poffKnob   = 0;          // posicion actual del pomo (0..POFF_RUN), interpolada
static int   poffTarget = 0;          // objetivo del pomo
static bool  poffDrag   = false;      // arrastre en curso
static int   poffGrab   = 0;          // offset dedo-pomo al agarrar (evita el salto inicial)
static int   poffLastKnob = -1;       // ultimo pomo dibujado (para no repintar de balde)
static uint8_t poffPhase = 0;         // 0=fundido 1=texto in 2=hold 3=texto out 4=backlight 5=dormir
static uint32_t poffPhaseMs = 0;      // millis() del inicio de la fase actual

// La proteccion por PIN aplica SOLO aqui. Ver el comentario de gPoffPin: la
// SUSPENSION nunca la consulta.
static bool poffPinRequired(){
#if POWEROFF_PIN_ON
  return gPoffPin && gLockType > 0;
#else
  return false;
#endif
}

// ---- Render ------------------------------------------------------------
// Dibuja la parte ESTATICA (fondo de vidrio, titulo, pista vacia, Cancelar) en
// el buffer activo. Se llama una sola vez por entrada a la pantalla.
static void poffDrawStatic(){
  // Fondo: wallpaper borroso ya cacheado (el mismo que usa la verificacion de
  // PIN) + un velo oscuro para que el panel de vidrio tenga contra que destacar.
  if(blurBg) memcpy(gBuf, blurBg, (size_t)SCR_W * SCR_H * 2);
  else       fillRect(0, 0, SCR_W, SCR_H, rgb565(10,12,20));
  fillRectA(0, 0, SCR_W, SCR_H, rgb565(8,10,18), 150);

  // Panel Liquid Glass envolvente: se REUTILIZA drawLiquidGlassPanelEx tal cual
  // (con su sombra/blur variable/especular/refraccion segun las flags GLASS_*),
  // no se reinventa ningun sistema de vidrio nuevo.
  drawLiquidGlassPanelEx(28, 232, SCR_W - 56, 424, 44, rgb565(40,30,42), 9);

  drawTextC(SCR_W / 2, 268, "\xC2\xBF" "Apagar FlexOS?", 3, rgb565(245,238,240));
  drawTextC(SCR_W / 2, 306, "El sistema entrar\xC3\xA1 en reposo profundo", 1, rgb565(198,190,196));

  // Pista del slider (vacia). El relleno y el pomo son dinamicos.
  fillRoundRectA(POFF_TRACK_X + 2, POFF_TRACK_Y + 4, POFF_TRACK_W, POFF_TRACK_H, POFF_TRACK_R, rgb565(0,0,0), 60);
  drawLiquidGlassPanelEx(POFF_TRACK_X, POFF_TRACK_Y, POFF_TRACK_W, POFF_TRACK_H, POFF_TRACK_R, rgb565(52,74,70), 7);
  drawRoundRect(POFF_TRACK_X, POFF_TRACK_Y, POFF_TRACK_W, POFF_TRACK_H, POFF_TRACK_R, rgb565(120,150,144));

  // Boton Cancelar (vuelve al estado previo sin ningun efecto secundario).
  fillRoundRectA(POFF_CAN_X + 2, POFF_CAN_Y + 4, POFF_CAN_W, POFF_CAN_H, POFF_CAN_R, rgb565(0,0,0), 60);
  drawLiquidGlassPanelEx(POFF_CAN_X, POFF_CAN_Y, POFF_CAN_W, POFF_CAN_H, POFF_CAN_R, rgb565(52,58,80), 7);
  drawRoundRect(POFF_CAN_X, POFF_CAN_Y, POFF_CAN_W, POFF_CAN_H, POFF_CAN_R, rgb565(120,128,150));
  drawTextC(SCR_W / 2, POFF_CAN_Y + POFF_CAN_H / 2 - 9, "Cancelar", 2, rgb565(235,238,248));
}
// Pinta pomo + relleno + rotulo sobre la banda ya restaurada del buffer activo.
static void poffDrawKnob(){
  int kx = POFF_TRACK_X + POFF_KNOB_PAD + poffKnob;          // esquina izq. del pomo
  int p  = POFF_RUN > 0 ? (poffKnob * 100 / POFF_RUN) : 0;   // % del recorrido

  // Estela: el trozo de pista ya recorrido se tine de rojo, cada vez mas solido.
  int fw = poffKnob + POFF_KNOB_D + 2 * POFF_KNOB_PAD;
  if(fw > POFF_TRACK_W) fw = POFF_TRACK_W;
  if(poffKnob > 0)
    fillRoundRectA(POFF_TRACK_X, POFF_TRACK_Y, fw, POFF_TRACK_H, POFF_TRACK_R,
                   rgb565(200,60,55), (uint8_t)(40 + p * 130 / 100));

  // Rotulo "desliza para apagar": se desvanece conforme el pomo avanza (a los
  // 100% ya no estorba al pomo, que esta justo encima).
  int lblA = 235 - p * 2;
  if(lblA > 0)
    drawTextCA(POFF_TRACK_X + POFF_TRACK_W / 2 + 18, POFF_TRACK_Y + POFF_TRACK_H / 2 - 9,
               "desliza para apagar", 2, rgb565(236,240,238), (uint8_t)lblA);

  // Pomo blanco con el simbolo de apagado en rojo (referencia: iOS).
  fillCircleA(kx + POFF_KNOB_R + 1, POFF_TRACK_Y + POFF_TRACK_H / 2 + 2, POFF_KNOB_R, rgb565(0,0,0), 70);
  fillCircle(kx + POFF_KNOB_R, POFF_TRACK_Y + POFF_TRACK_H / 2, POFF_KNOB_R, rgb565(252,252,252));
  qsTileIcon(6, kx + POFF_KNOB_R, POFF_TRACK_Y + POFF_TRACK_H / 2, rgb565(226,46,40));
}
// Un frame de la pantalla de confirmacion. SOLO se recompone la banda del
// slider: el resto (titulo, vidrio, Cancelar) es estatico y ya esta en fb desde
// poffEnter(), asi que no hay nada que repintar ni nada que pueda parpadear.
static void poffRenderBand(){
  // Sin la cache no se puede BORRAR el pomo anterior, asi que se prefiere no
  // animar antes que dejar un reguero de pomos pegados. La pantalla sigue
  // siendo usable: el boton Cancelar se atiende en poffTick, antes de llegar
  // aqui. (Solo pasaria si PSRAM se quedara sin los ~135 KB del buffer.)
  if(!poffBand) return;
  if(poffKnob == poffLastKnob) return;              // nada que hacer este frame
  poffLastKnob = poffKnob;
  memcpy(bbuf + (size_t)POFF_BAND_Y0 * SCR_W, poffBand, (size_t)POFF_BAND_H * SCR_W * 2);
  uint16_t* old = gBuf; setBuf(bbuf);
  int c0 = gClipY0, c1 = gClipY1;
  gClipY0 = POFF_BAND_Y0; gClipY1 = POFF_BAND_Y1;   // nada puede salirse de la banda
  poffDrawKnob();
  gClipY0 = c0; gClipY1 = c1;
  setBuf(old);
  present(POFF_BAND_Y0, POFF_BAND_Y1);
}

static void poffEnter(){
#if !POWEROFF_ON
  return;
#else
  // Misma red de seguridad que lsuStartVerify: esta pantalla SIEMPRE se compone
  // en portrait y a pantalla completa, venga de donde venga (Modo PC o una app
  // landscape dejan gLand=true y el tactil remapeado).
  gLand = false;
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  ensureBlurBg();
  poffKnob = poffTarget = 0; poffDrag = false; poffGrab = 0; poffLastKnob = -1;

  setBuf(bbuf);
  poffDrawStatic();
  // Cache de la banda del slider TAL CUAL queda de fabrica (pista vacia). Se
  // reserva UNA sola vez en toda la sesion, fuera del loop de render, en PSRAM
  // -- igual que qsBuf. ~135 KB (480 x 112 x 2), no un framebuffer entero.
  if(!poffBand) poffBand = (uint16_t*)heap_caps_malloc((size_t)POFF_BAND_H * SCR_W * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(poffBand) memcpy(poffBand, bbuf + (size_t)POFF_BAND_Y0 * SCR_W, (size_t)POFF_BAND_H * SCR_W * 2);
  poffDrawKnob();                       // pomo en reposo, dentro del mismo frame
  setBuf(fb);
  present(0, SCR_H - 1);                // un unico volcado atomico: cero parpadeo
  poffLastKnob = 0;
  gState = ST_POWEROFF_CONFIRM;
#endif
}

// Arrastre del slider. Mismo enfoque que el drag de la cortina de Ajustes
// rapidos (qsHandle): mientras el dedo esta abajo el pomo SIGUE al dedo 1:1, y
// al soltar sin completar el recorrido vuelve a 0 interpolado.
static void poffTick(){
#if POWEROFF_ON
  int kx = POFF_TRACK_X + POFF_KNOB_PAD + poffKnob;
  if(T.pressed && !poffDrag){
    // Agarre generoso: todo el alto de la pista, y en X desde el pomo hacia la
    // izquierda un poco, para que empezar el gesto no exija precision.
    if(T.y >= POFF_TRACK_Y && T.y <= POFF_TRACK_Y + POFF_TRACK_H &&
       T.x >= kx - 24 && T.x <= kx + POFF_KNOB_D + 24){
      poffDrag = true; poffGrab = T.x - kx;
    }
  }
  if(poffDrag){
    if(T.down){
      int v = T.x - poffGrab - (POFF_TRACK_X + POFF_KNOB_PAD);
      if(v < 0) v = 0; if(v > POFF_RUN) v = POFF_RUN;
      poffTarget = poffKnob = v;                       // 1:1 con el dedo
    } else {
      poffDrag = false;
      if(POFF_RUN > 0 && poffKnob * 100 / POFF_RUN >= POFF_DONE_PCT){
        poffKnob = poffTarget = POFF_RUN;              // completado
        poffRenderBand();
        // PIN opcional ANTES de proceder. Si falla o se cancela, se vuelve aqui
        // (ver lsuExit) y no se apaga nada.
        if(poffPinRequired()){ lsuStartVerifyFor(LSU_AFTER_POWEROFF, -1); return; }
        poffBeginAnim(); return;
      }
      poffTarget = 0;                                  // no llego: vuelve solo
    }
  } else if(T.tap && T.x >= POFF_CAN_X && T.x <= POFF_CAN_X + POFF_CAN_W &&
                     T.y >= POFF_CAN_Y && T.y <= POFF_CAN_Y + POFF_CAN_H){
    // Cancelar: sin ningun efecto secundario. Se vuelve al escritorio, que es de
    // donde se llega siempre (el icono vive en el Panel Rapido, que solo se abre
    // desde ST_HOME).
    gState = ST_HOME; renderHome(); showHome(); return;
  }
  // Interpolacion de vuelta (mismo estilo que el resto de resortes del sistema:
  // acercamiento proporcional + enganche final para que termine de verdad).
  if(!poffDrag && poffKnob != poffTarget){
    int d = poffTarget - poffKnob;
    poffKnob += (d > 0 ? (d + 3) / 4 : (d - 3) / 4);
    if(abs(poffTarget - poffKnob) < 3) poffKnob = poffTarget;
  }
  poffRenderBand();
#endif
}

// ---- Deep sleep --------------------------------------------------------
// Arma la fuente de despertar y entra en deep sleep. NO retorna nunca.
//
// FUENTE DE DESPERTAR -- ver el bloque POFF_WAKE_GPIO de arriba del archivo:
//   · Ruta buena (INT del GT911 cableado y en GPIO 0..15): ext1. Es la unica
//     forma soportada en el ESP32-P4 de despertar por un pin -- ext0 NO existe
//     en este chip (no hay SOC_PM_SUPPORT_EXT0_WAKEUP en su soc_caps.h).
//   · Ruta alternativa (pin no configurado): temporizador. El chip despierta
//     cada POFF_WAKE_POLL_MS, mira el tactil y se vuelve a dormir si no hay
//     dedo. Funciona en cualquier placa sin saber ningun pin, pero consume
//     bastante mas que el deep sleep de verdad porque el chip arranca a cada
//     rato. Es un modo DEGRADADO, no el objetivo.
static void poffEnterDeepSleep(){
  // El GT911 tiene que seguir escaneando mientras el P4 duerme, o su INT no
  // llegaria nunca. Su linea de reset la maneja PIN_TP_RST (GPIO 3), que esta
  // dentro del rango LP/RTC del P4 (0..15) y por tanto admite hold: se congela
  // en alto para que el tactil no se quede reseteado durante el sueno.
  //
  // OJO -- diferencia real del ESP32-P4 frente al ESP32 clasico: aqui NO se
  // llama a gpio_deep_sleep_hold_en(). Esa funcion NO EXISTE en este chip: en
  // soc_caps.h del P4 esta SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP=1, y
  // driver/gpio.h declara gpio_deep_sleep_hold_en/dis dentro de un
  // "#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP" -> compilar una llamada a
  // ellas en el P4 es un error de compilacion. En este chip gpio_hold_en() por
  // si sola ya mantiene el pin durante el deep sleep, que es justo lo que dice
  // esa capability. (Espressif la marca como valida en silicio P4 rev >= 3.0.)
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, HIGH);
  gpio_hold_en((gpio_num_t)PIN_TP_RST);

#if (POFF_WAKE_GPIO >= 0) && (POFF_WAKE_GPIO <= 15)
  esp_sleep_enable_ext1_wakeup_io(1ULL << POFF_WAKE_GPIO,
                                  POFF_WAKE_LEVEL ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ANY_LOW);
#else
  esp_sleep_enable_timer_wakeup((uint64_t)POFF_WAKE_POLL_MS * 1000ULL);
#endif
  Serial.println(F("[PWR] deep sleep"));
  Serial.flush();
  esp_deep_sleep_start();               // no retorna
}
// Marca en NVS que este apagado fue LIMPIO (lo pidio el usuario), para que
// setup() pueda distinguir un arranque normal de un despertar de deep sleep sin
// depender solo de esp_reset_reason().
static void poffSaveCleanFlag(){
  prefs.begin("flexos", false);
  prefs.putBool("cleanoff", true);
  prefs.putInt("bright", gBright);      // el brillo del usuario, para restaurarlo al encender
  prefs.end();
}

// ---- Animacion de apagado ----------------------------------------------
static void poffBeginAnim(){
#if POWEROFF_ON
  gLand = false;
  gClipX0 = 0; gClipX1 = SCR_W - 1; gClipY0 = 0; gClipY1 = SCR_H - 1;
  // Instantanea del frame actual para el fundido a negro. Se usa lockBuf (la
  // cache de la pantalla de bloqueo) en vez de reservar 750 KB mas: renderLock()
  // la regenera entera cada vez que hace falta, y desde aqui ya no se vuelve a
  // ninguna pantalla -- el destino es el deep sleep.
  if(lockBuf) memcpy(lockBuf, fb, (size_t)SCR_W * SCR_H * 2);
  poffPhase = 0; poffPhaseMs = millis();
  gState = ST_POWEROFF_ANIM;
#endif
}
static void poffAnimTick(){
#if POWEROFF_ON
  uint32_t e = millis() - poffPhaseMs;
  switch(poffPhase){
    case 0: {           // 1) fundido a negro (overlay compuesto en bbuf, NUNCA fillScreen)
      if(e > POFF_FADE_MS) e = POFF_FADE_MS;
      uint8_t a = (uint8_t)(e * 255 / POFF_FADE_MS);
      const uint16_t* src = lockBuf ? lockBuf : fb;
      for(int j = 0; j < SCR_H; j++){
        const uint16_t* s = src  + (size_t)j * SCR_W;
        uint16_t*       d = bbuf + (size_t)j * SCR_W;
        for(int i = 0; i < SCR_W; i++) d[i] = mix565(s[i], 0, a);   // 0 = negro en RGB565
      }
      present(0, SCR_H - 1);
      if(e >= POFF_FADE_MS){ poffPhase = 1; poffPhaseMs = millis(); }
      break;
    }
    case 1:             // 2) el texto "Flex OS" aparece sobre el negro
    case 2:             //    ... se queda quieto
    case 3: {           // 3) ... y se disuelve (alpha decreciente)
      uint8_t a = 255;
      if(poffPhase == 1){ if(e > POFF_TIN_MS) e = POFF_TIN_MS; a = (uint8_t)(e * 255 / POFF_TIN_MS); }
      else if(poffPhase == 3){ if(e > POFF_TOUT_MS) e = POFF_TOUT_MS; a = (uint8_t)(255 - e * 255 / POFF_TOUT_MS); }
      // Solo se recompone la banda del texto: el resto de la pantalla ya es
      // negro solido en fb desde la fase 0 y no cambia.
      for(int j = POFF_TXT_BY0; j <= POFF_TXT_BY1; j++)
        memset(bbuf + (size_t)j * SCR_W, 0, SCR_W * 2);
      uint16_t* old = gBuf; setBuf(bbuf);
      int c0 = gClipY0, c1 = gClipY1;
      gClipY0 = POFF_TXT_BY0; gClipY1 = POFF_TXT_BY1;
      if(a) drawTextCA(SCR_W / 2, POFF_TXT_Y, "Flex OS", POFF_TXT_SZ, rgb565(240,242,250), a);
      gClipY0 = c0; gClipY1 = c1;
      setBuf(old);
      present(POFF_TXT_BY0, POFF_TXT_BY1);
      uint32_t dur = poffPhase == 1 ? POFF_TIN_MS : poffPhase == 2 ? POFF_HOLD_MS : POFF_TOUT_MS;
      if(millis() - poffPhaseMs >= dur){ poffPhase++; poffPhaseMs = millis(); }
      break;
    }
    case 4:             // 4) fundido del backlight a 0 (mismo mecanismo que la suspension)
      if(gSuspFade < 0 && !gSuspOn){ gSuspOn = true; gSuspDark = false; gSuspBright = gBright; suspFadeTo(0); }
      if(gSuspDark){ poffPhase = 5; poffPhaseMs = millis(); }
      break;
    case 5:             // 5) DCS de bajo consumo -> NVS -> deep sleep
      panelSleepIn();
      poffSaveCleanFlag();
      poffEnterDeepSleep();   // no retorna
      break;
  }
#endif
}

// -------------------------------------------------------------
//  ALIMENTACION DEFENSIVA DEL TASK WATCHDOG
//  -------------------------------------------------------------
//  esp_task_wdt_reset() solo es valido si la tarea que llama esta
//  SUSCRITA al TWDT. Si no lo esta devuelve ESP_ERR_NOT_FOUND y el
//  componente task_wdt imprime un ERROR por CADA llamada:
//
//      E (17336) task_wdt: esp_task_wdt_reset(705): task not found
//
//  Desde loop(), que da unas 200 vueltas por segundo, eso satura el
//  puerto serie y deja el log inservible para diagnosticar nada mas.
//
//  Por que se puede perder la suscripcion: levantar la pila WiFi (en el
//  P4, el enlace esp-hosted/SDIO con el C6) reinicializa el TWDT y con
//  el se va la lista de suscriptores. Antes no se notaba porque la
//  radio no se encendia nunca sola; en cuanto el arranque empezo a
//  levantarla, el bucle aparecio en cada boot.
//
//  Diseño: en el caso normal esto es UNA sola llamada por vuelta, igual
//  de barato que antes. En cuanto falla UNA vez se deja de llamar y se
//  pasa a reintentar la suscripcion cada 5 s -- asi el log ve una linea
//  en la transicion y como mucho una cada 5 s, nunca un bucle. Y si el
//  TWDT vuelve a estar disponible, la vigilancia se recupera sola: no
//  se abandona nunca de forma permanente.
// -------------------------------------------------------------
static void flexFeedWdt(){
  static bool     subscribed = true;      // Arduino suscribe el loopTask al arrancar
  static uint32_t nextTry    = 0;
  if(subscribed){
    if(esp_task_wdt_reset() == ESP_OK) return;          // caso normal
    subscribed = false;                                 // se perdio la suscripcion
    nextTry    = millis() + 5000;
    Serial.println(F("[WDT] loopTask ya no esta suscrito al Task Watchdog; reintentando cada 5 s"));
    return;
  }
  uint32_t now = millis();
  if(now < nextTry) return;                             // sin insistir: eso es lo que inundaba
  nextTry = now + 5000;
  if(esp_task_wdt_add(NULL) == ESP_OK){
    subscribed = true;
    Serial.println(F("[WDT] loopTask resuscrito al Task Watchdog"));
    esp_task_wdt_reset();
  }
}

// ---- Filtro de arranque: 3 segundos de presion sostenida ----------------
// POR QUE ASI (es el punto mas delicado de todo el apagado):
// En deep sleep el loop() no existe, asi que NO hay forma de cronometrar los 3
// segundos con la logica habitual de millis() -- el chip esta apagado. El pin
// de despertar tampoco sirve para medir duracion: el INT del GT911 es un PULSO
// por reporte, no un nivel que se mantenga mientras el dedo esta apoyado, asi
// que ext1 solo puede decir "alguien ha tocado", nunca "lleva 3 s tocando".
// Solucion (la que sugeria la propia tarea, y la unica soportada de verdad):
//   despertar al PRIMER contacto -> verificar la duracion YA DESPIERTO, por I2C,
//   en los primeros instantes de setup(), con el panel y el backlight todavia
//   apagados -> si no se sostiene, volver a dormir sin llegar a arrancar.
// El usuario nunca ve un destello: nada de esto ocurre despues de flexPanelInit.
static void poffWakeGate(){
#if POWEROFF_ON
  if(esp_reset_reason() != ESP_RST_DEEPSLEEP) return;    // no venimos de deep sleep
  // Liberar el hold del reset del GT911. La API pide dejar el pin ya
  // configurado en el mismo nivel ANTES de soltar el hold, o el pad daria un
  // glitch al pasar a su estado por defecto (y eso resetearia el tactil justo
  // cuando lo necesitamos). gpio_deep_sleep_hold_dis() NO se llama: no existe
  // en el P4 (ver poffEnterDeepSleep).
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, HIGH);
  gpio_hold_dis((gpio_num_t)PIN_TP_RST);
  flexTouchInit();                                       // I2C + GT911 (el panel sigue apagado)
  if(!gtOk) return;                                      // sin tactil no hay forma de confirmar: se arranca
  uint32_t t0 = millis(), heldFrom = 0;
  while(millis() - t0 < POFF_WAKE_GATE_MS){
    flexFeedWdt();                                       // el TWDT se alimenta igual que en loop() (defensivo)
    uint16_t gx = 0, gy = 0;
    gtPoll(gx, gy);
    int n = (millis() - gtFingersMs > 120) ? 0 : (int)gtFingers;
    if(n >= 1){
      if(!heldFrom) heldFrom = millis();
      if(millis() - heldFrom >= POFF_WAKE_HOLD_MS) return;   // 3 s sostenidos -> arranque completo
    } else heldFrom = 0;
    delay(10);
  }
  poffEnterDeepSleep();                                  // no se sostuvo: de vuelta a dormir
#endif
}

// #############################################################
// ##  setup / loop
// #############################################################
// -------------------------------------------------------------
//  Radio (WiFi via co-procesador ESP32-C6 / esp-hosted por SDIO)
//  EL ARRANQUE YA NUNCA TOCA LA RADIO. Antes, bootInitRadioSafe()
//  lanzaba un intento de conexion automatico en cada boot (con SSID/
//  PASS fijos en el codigo) en cuanto FLEXOS_ENABLE_WIFI valia 1. Eso
//  es lo que producia el bucle de "PANIC (crash)": si el enlace SDIO
//  con el C6 fallaba (placa sin C6 operativo en esos pines, firmware
//  slave desactualizado, etc.), fallaba EN CADA arranque, de forma
//  determinista, antes de que pudieras hacer nada.
//
//  Ahora la radio es 100% bajo demanda: no se toca ni una sola vez
//  durante setup()/loop() salvo que el usuario entre a Ajustes -> Red
//  e Internet -> Wi-Fi y pulse "Buscar redes". Si el C6 falla ahi, el
//  fallo se ve como un error EN PANTALLA dentro de esa app (o, en el
//  peor caso, como un reinicio aislado y reproducible en ese momento
//  exacto), nunca como un cuelgue silencioso del arranque completo.
//
//  1) El enlace P4<->C6 de referencia es SDIO (CLK/CMD/D0-D3 + reset
//     del esclavo), NO SPI, y en arduino-esp32 3.2.0 esos pines los
//     fija el "variant" de la placa en tiempo de COMPILACION.
//  2) WiFi.begin()/WiFi.scanNetworks() disparan por debajo el
//     transporte hosted (esp_wifi_remote); nunca se llama a
//     esp_hosted_init() a mano.
//  3) El C6 necesita firmware "slave" de esp-hosted flasheado aparte;
//     si por Serial ves version v0.0.0, no hay enlace real posible.
//
//  FLEXOS_ENABLE_WIFI y gNetOnline estan declarados arriba del todo
//  del archivo (junto a los PINES) porque Ajustes los necesita antes
//  de llegar aqui. Si tu placa NO tiene el C6 operativo, pon
//  FLEXOS_ENABLE_WIFI a 0: la pantalla de Wi-Fi lo respeta y deja de
//  ofrecer "Buscar redes" sin que toques nada mas.
// -------------------------------------------------------------
#define FLEXOS_WIFI_TIMEOUT_MS 15000

// Definido mas abajo (necesita wifiSavedSSID y el resto del bloque Wi-Fi).
static bool wifiCredsLoad();

// Cuanto se espera desde el arranque antes del PRIMER intento de
// reconexion automatica. No es un adorno: da tiempo a que el panel, el
// tactil y el escritorio esten en marcha, de modo que si el enlace SDIO
// con el C6 falla, el fallo se vea con el sistema ya vivo y no a mitad
// del arranque.
#define FLEXOS_WIFI_AUTOCONN_DELAY_MS 6000

// -------------------------------------------------------------
//  setup() NO TOCA LA RADIO. NI UNA SOLA VEZ.
//  -------------------------------------------------------------
//  Esto es una regla del sistema, no una preferencia (ver el bloque
//  "EL ARRANQUE YA NUNCA TOCA LA RADIO" mas arriba): encender la pila
//  WiFi dentro de setup() es exactamente lo que producia el bucle de
//  "PANIC (crash)" en cada arranque cuando el enlace esp-hosted/SDIO
//  con el C6 no respondia. La version anterior de este fichero volvio
//  a meter ahi el intento de reconexion y reprodujo el problema, esta
//  vez en forma de inundacion del TWDT desde el primer boot.
//
//  Aqui solo se leen las credenciales de NVS -- eso es flash, no radio,
//  y es seguro. El intento real lo dispara wifiAutoReconnectTick()
//  desde loop(), ya con el sistema arrancado.
// -------------------------------------------------------------
static void bootInitRadioSafe(){
  bool saved = wifiCredsLoad();          // solo NVS: no despierta el C6
  if(saved) Serial.println(F("[C6] hay red guardada -> se intentara reconectar tras el arranque (nunca dentro de setup)"));
  else      Serial.println(F("[C6] radio en modo bajo demanda -> se activa solo desde Ajustes > Red e Internet > Wi-Fi"));
}


// ---- BLE opcional (tambien via C6, requiere firmware slave con BT) ----
// NOTA: el P4 NO tiene controlador Bluetooth propio, asi que aqui NO se
// llama a esp_bt_controller_mem_release(): esa API es para liberar RAM
// de Bluetooth Clasico en un chip con radio LOCAL (S3, C3...), y el P4
// no tiene radio local ni Bluetooth Clasico que liberar. El C6 ademas
// solo ofrece BLE (no Classic). Descomenta esto SOLO despues de
// confirmar que WiFi ya enlaza por el mismo C6.
#if 0
#include <NimBLEDevice.h>
static void bootInitBleSafe(){
  NimBLEDevice::init("FlexOS");
  // ... tu logica de advertising / GATT aqui
}
#endif

// #############################################################
// ##  AJUSTES -> RED E INTERNET -> WI-FI
// ##  Escaneo y conexion corren en su propia tarea del Core 1
// ##  (igual patron que arriba): loop() nunca se bloquea, y un
// ##  fallo del C6 se queda contenido en esta pantalla.
// #############################################################
#define WIFI_MAX_NETS 16
struct WifiNet { char ssid[33]; int8_t rssi; bool secure; };
static WifiNet      wifiNets[WIFI_MAX_NETS];
static volatile int wifiNetCount = 0;
static portMUX_TYPE wifiMux = portMUX_INITIALIZER_UNLOCKED;

enum { WUI_LIST = 0, WUI_SCANNING, WUI_PASS, WUI_CONNECTING, WUI_OK, WUI_FAIL };
static volatile int wifiUIState = WUI_LIST;
static int      wifiSel = -1;
static char     wifiPass[64] = "";
static uint32_t wifiKbAnim = 0;
static char     wifiConnSSID[33] = "";
static char     wifiConnPass[64] = "";
static char     wifiConnIP[24]   = "";

// #############################################################
// ##  CREDENCIALES GUARDADAS (NVS) + RECONEXION AUTOMATICA
// ##  ------------------------------------------------------
// ##  Antes, la red elegida a mano solo vivia en RAM: cada
// ##  apagado obligaba a repetir escaneo + seleccion + clave.
// ##  Ahora, la PRIMERA conexion correcta guarda SSID y clave en
// ##  NVS (namespace propio "flexos_wifi", separado de "flexos"
// ##  para que un borrado de ajustes no arrastre la red y al
// ##  reves), y el arranque las reutiliza en segundo plano.
// #############################################################
#define WIFI_NVS_NS    "flexos_wifi"
#define WIFI_NVS_SSID  "ssid"
#define WIFI_NVS_PASS  "pass"

static char          wifiSavedSSID[33] = "";
static char          wifiSavedPass[64] = "";
static volatile bool gWifiAutoBusy  = false;   // intento automatico en curso
static volatile bool gWifiAutoDone  = false;   // ya se intento (con exito o no)

static bool wifiCredsExist(){ return wifiSavedSSID[0] != 0; }

// Carga las credenciales de NVS a RAM. Se llama una vez en el arranque.
static bool wifiCredsLoad(){
  Preferences p;
  if(!p.begin(WIFI_NVS_NS, true)){ wifiSavedSSID[0] = 0; wifiSavedPass[0] = 0; return false; }
  String s = p.getString(WIFI_NVS_SSID, "");
  String w = p.getString(WIFI_NVS_PASS, "");
  p.end();
  s.toCharArray(wifiSavedSSID, sizeof(wifiSavedSSID));
  w.toCharArray(wifiSavedPass, sizeof(wifiSavedPass));
  return wifiCredsExist();
}

// Guarda (solo si algo cambio: escribir NVS sin necesidad desgasta la flash).
static void wifiCredsSave(const char* ssid, const char* pass){
  if(!ssid || !ssid[0]) return;
  if(!strcmp(wifiSavedSSID, ssid) && !strcmp(wifiSavedPass, pass ? pass : "")) return;
  Preferences p;
  if(!p.begin(WIFI_NVS_NS, false)) return;
  p.putString(WIFI_NVS_SSID, ssid);
  p.putString(WIFI_NVS_PASS, pass ? pass : "");
  p.end();
  strncpy(wifiSavedSSID, ssid, sizeof(wifiSavedSSID) - 1); wifiSavedSSID[sizeof(wifiSavedSSID) - 1] = 0;
  strncpy(wifiSavedPass, pass ? pass : "", sizeof(wifiSavedPass) - 1); wifiSavedPass[sizeof(wifiSavedPass) - 1] = 0;
  Serial.printf("[WiFi] red guardada: %s\n", wifiSavedSSID);
}

// Olvida la red (cambio de router). Borra NVS y la copia en RAM.
static void wifiCredsForget(){
  Preferences p;
  if(p.begin(WIFI_NVS_NS, false)){ p.clear(); p.end(); }
  wifiSavedSSID[0] = 0; wifiSavedPass[0] = 0;
  gWifiAutoDone = true;                      // no reintentar en esta sesion
  Serial.println(F("[WiFi] red guardada borrada"));
}

// GUARD DE MODO. Todo intento de escaneo o conexion pasa por aqui: el
// driver debe estar en STA antes de tocarlo. Llamar a WiFi.mode() cuando
// ya se esta en el modo pedido es innecesario y en el transporte hosted
// del C6 reinicia la interfaz sin motivo, asi que solo se cambia si hace
// falta de verdad.
static void wifiEnsureStaMode(){
  if(WiFi.getMode() != WIFI_STA) WiFi.mode(WIFI_STA);
}

#if FLEXOS_ENABLE_WIFI
static void wifiScanTask(void*){
  wifiEnsureStaMode();
  int n = WiFi.scanNetworks();                // bloqueante, pero en su PROPIA tarea: loop() sigue vivo
  // ANTI-CRASH: construir la lista FUERA de toda seccion critica. La version
  // anterior copiaba dentro de portENTER_CRITICAL(&wifiMux), pero WiFi.SSID()
  // devuelve un String (malloc) y ademas toca el driver WiFi. Hacer malloc con
  // las interrupciones deshabilitadas y un spinlock tomado puede (a) bloquear
  // el lock interno del heap -> deadlock, o (b) mantener las IRQ apagadas
  // demasiado tiempo -> disparar el watchdog de interrupciones (INT_WDT) y
  // reiniciar el ESP32. wifiNets[] SOLO lo escribe esta tarea; la UI lee unica-
  // mente indices [0, wifiNetCount). Por eso basta con publicar wifiNetCount AL
  // FINAL, bajo una seccion critica minima (barrera de memoria): la UI nunca ve
  // una entrada a medio escribir y no hay ninguna asignacion bajo el spinlock.
  int cnt = 0;
  if(n > 0){
    for(int i = 0; i < n && cnt < WIFI_MAX_NETS; i++){
      String ss = WiFi.SSID(i);
      if(ss.length() == 0) continue;                                // oculta redes sin nombre
      bool dup = false;
      for(int k = 0; k < cnt; k++) if(!strcmp(wifiNets[k].ssid, ss.c_str())){ dup = true; break; }
      if(dup) continue;                                             // mismo SSID visto en varios canales
      ss.toCharArray(wifiNets[cnt].ssid, sizeof(wifiNets[cnt].ssid));
      wifiNets[cnt].rssi   = (int8_t)WiFi.RSSI(i);
      wifiNets[cnt].secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      cnt++;
    }
  }
  portENTER_CRITICAL(&wifiMux); wifiNetCount = cnt; portEXIT_CRITICAL(&wifiMux);  // publicacion atomica del contador
  WiFi.scanDelete();
  wifiUIState = WUI_LIST;                     // n<=0 -> lista vacia (mensaje "sin redes"), no es un error fatal
  vTaskDelete(NULL);
}
static void wifiConnTask(void*){
  wifiEnsureStaMode();
  WiFi.begin(wifiConnSSID, wifiConnPass);
  uint32_t t0 = millis();
  while(WiFi.status() != WL_CONNECTED && millis() - t0 < FLEXOS_WIFI_TIMEOUT_MS){
    vTaskDelay(pdMS_TO_TICKS(200));           // cede CPU -> no molesta a nadie, no dispara TWDT
  }
  if(WiFi.status() == WL_CONNECTED){
    gNetOnline = true;
    IPAddress ip = WiFi.localIP();
    String ips = ip.toString();
    ips.toCharArray(wifiConnIP, sizeof(wifiConnIP));
    // PERSISTENCIA: solo se guarda lo que YA se ha comprobado que
    // funciona. Guardar antes de confirmar dejaria una clave erronea
    // fija en NVS y el equipo reintentaria con ella en cada arranque.
    wifiCredsSave(wifiConnSSID, wifiConnPass);
    wifiUIState = WUI_OK;
  } else {
    gNetOnline = false;
    WiFi.disconnect(true, true);              // libera el intento fallido, no deja el C6 a medias
    wifiUIState = WUI_FAIL;
  }
  vTaskDelete(NULL);
}

// ---- RECONEXION AUTOMATICA AL ARRANCAR ----------------------------
// Misma forma que wifiConnTask (tarea propia en el Core 1, 8 KB de pila,
// cesiones con vTaskDelay), pero NO toca wifiUIState: corre de fondo con
// la pantalla de Wi-Fi cerrada, y esa variable pertenece a esa pantalla.
// Si tocara wifiUIState, abrir Ajustes > Wi-Fi a mitad del intento
// mostraria un "Conectando..." que el usuario no ha pedido.
static void wifiAutoConnTask(void*){
  Serial.printf("[WiFi] reconexion automatica a \"%s\"...\n", wifiSavedSSID);
  wifiEnsureStaMode();
  WiFi.begin(wifiSavedSSID, wifiSavedPass);
  uint32_t t0 = millis();
  while(WiFi.status() != WL_CONNECTED && millis() - t0 < FLEXOS_WIFI_TIMEOUT_MS){
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if(WiFi.status() == WL_CONNECTED){
    gNetOnline = true;
    IPAddress ip = WiFi.localIP();
    String ips = ip.toString();
    ips.toCharArray(wifiConnIP, sizeof(wifiConnIP));
    strncpy(wifiConnSSID, wifiSavedSSID, sizeof(wifiConnSSID) - 1);
    wifiConnSSID[sizeof(wifiConnSSID) - 1] = 0;
    Serial.printf("[WiFi] reconectado. IP %s\n", wifiConnIP);
  } else {
    // Fallo (router apagado, clave cambiada, fuera de alcance): se suelta
    // la radio y NO se borra nada. Las credenciales siguen guardadas para
    // el proximo arranque; el usuario puede entrar a Ajustes > Wi-Fi y
    // configurar a mano, que es el camino de siempre.
    gNetOnline = false;
    WiFi.disconnect(true, true);
    Serial.println(F("[WiFi] reconexion automatica fallida -> queda la configuracion manual"));
  }
  gWifiAutoBusy = false;
  gWifiAutoDone = true;
  vTaskDelete(NULL);
}

// Lanza el intento automatico si hay algo guardado. No bloquea el arranque.
static void wifiTryAutoConnect(){
  if(gWifiAutoBusy || gWifiAutoDone) return;
  if(!wifiCredsExist()){
    gWifiAutoDone = true;
    Serial.println(F("[WiFi] sin red guardada -> configuracion manual"));
    return;
  }
  gWifiAutoBusy = true;
  xTaskCreatePinnedToCore(wifiAutoConnTask, "wifiAuto", 8192, NULL, 1, NULL, 1);
}
#endif   // FLEXOS_ENABLE_WIFI

// -------------------------------------------------------------
//  RECONEXION AUTOMATICA DIFERIDA  (se llama desde loop())
//  -------------------------------------------------------------
//  Se ejecuta en cada vuelta pero solo hace algo UNA vez, y nunca
//  antes de que el sistema este operativo: escritorio o pantalla de
//  bloqueo, y unos segundos despues del encendido.
//
//  El retraso NO es cosmetico. La radio de esta placa cuelga de un
//  co-procesador C6 por SDIO, y levantarla dentro de setup() es lo que
//  producia el bucle de "PANIC (crash)" que documenta el bloque "EL
//  ARRANQUE YA NUNCA TOCA LA RADIO". Disparandola desde aqui, el
//  enlace se abre en la misma ventana contenida y observable que la
//  ruta manual: si el C6 falla, el sistema ya esta vivo y el fallo se
//  ve, en vez de llevarse por delante el arranque entero.
// -------------------------------------------------------------
static void wifiAutoReconnectTick(){
#if FLEXOS_ENABLE_WIFI
  if(gWifiAutoDone || gWifiAutoBusy) return;
  if(gState != ST_HOME && gState != ST_LOCK) return;      // aun arrancando: esperar
  if(millis() < FLEXOS_WIFI_AUTOCONN_DELAY_MS) return;
  if(!wifiCredsExist()){ gWifiAutoDone = true; return; }
  Serial.println(F("[WiFi] sistema listo -> lanzando reconexion automatica"));
  wifiTryAutoConnect();
#endif
}

static void wifiStartScan(){
#if FLEXOS_ENABLE_WIFI
  wifiEnsureStaMode();
  wifiUIState = WUI_SCANNING;
  portENTER_CRITICAL(&wifiMux); wifiNetCount = 0; portEXIT_CRITICAL(&wifiMux);
  xTaskCreatePinnedToCore(wifiScanTask, "wifiScan", 8192, NULL, 1, NULL, 1);   // 8KB: scanNetworks() + String necesitan mas que 6KB (evita stack overflow)
#endif
}
static void wifiStartConnect(){
#if FLEXOS_ENABLE_WIFI
  if(wifiSel < 0 || wifiSel >= WIFI_MAX_NETS){ wifiUIState = WUI_LIST; return; }  // indice invalido -> nunca leer wifiNets[] fuera de rango
  wifiEnsureStaMode();
  strncpy(wifiConnSSID, wifiNets[wifiSel].ssid, sizeof(wifiConnSSID) - 1); wifiConnSSID[sizeof(wifiConnSSID) - 1] = 0;
  strncpy(wifiConnPass, wifiPass, sizeof(wifiConnPass) - 1); wifiConnPass[sizeof(wifiConnPass) - 1] = 0;
  wifiUIState = WUI_CONNECTING;
  xTaskCreatePinnedToCore(wifiConnTask, "wifiConn", 8192, NULL, 1, NULL, 1);   // 8KB: WiFi.begin() + pila del driver necesitan mas que 6KB
#endif
}
static void wifiPassAppend(const char* s){ int L = strlen(wifiPass), sl = strlen(s); if(L + sl < (int)sizeof(wifiPass) - 1){ memcpy(wifiPass + L, s, sl); wifiPass[L + sl] = 0; } }
static void wifiPassBackspace(){ int L = strlen(wifiPass); if(L > 0){ int q = L - 1; while(q > 0 && (wifiPass[q] & 0xC0) == 0x80) q--; wifiPass[q] = 0; } }
static void wifiBack(){ strokeSegAA(30, 26, 18, 18, 2.4f, rgb565(255,255,255)); strokeSegAA(18, 18, 30, 10, 2.4f, rgb565(255,255,255)); }

static int wifiRowY(int i){ return 150 + i * 66; }

// Geometria de los botones del pie. UNA sola fuente para el dibujo y para
// el tap: si el "Olvidar red" aparece o desaparece segun haya red guardada,
// las dos rutas cambian a la vez y la zona pulsable no puede desalinearse
// de lo pintado (mismo criterio que setRowCard/setRowY0 en Ajustes).
static void wifiBtnRects(int& by, int& sx, int& sw, int& fx, int& fw){
  by = SCR_H - 90;
  if(wifiCredsExist()){                       // dos botones lado a lado
    int w = (SCR_W - 48 - 12) / 2;
    sx = 24;              sw = w;
    fx = 24 + w + 12;     fw = w;
  } else {                                    // solo "Buscar redes", centrado
    sx = SCR_W / 2 - 110; sw = 220;
    fx = 0;               fw = 0;
  }
}

static void wifiRenderList(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(14,16,24));
  wifiBack();
  drawTextC(SCR_W / 2, 60, "Wi-Fi", 4, rgb565(255,255,255));
  // Estado de la red recordada, justo bajo el titulo.
  if(wifiCredsExist()){
    char sv[64];
    bool on = (WiFi.status() == WL_CONNECTED);
    snprintf(sv, sizeof(sv), "%s %s", on ? "Conectado a" : "Red guardada:", wifiSavedSSID);
    drawTextC(SCR_W / 2, 112, sv, 1, on ? rgb565(120,220,160) : rgb565(150,158,178));
  } else if(gWifiAutoBusy){
    drawTextC(SCR_W / 2, 112, "Reconectando...", 1, rgb565(150,158,178));
  }
  int cnt; portENTER_CRITICAL(&wifiMux); cnt = wifiNetCount; portEXIT_CRITICAL(&wifiMux);
  if(wifiUIState == WUI_SCANNING){
    drawTextC(SCR_W / 2, 300, "Buscando redes...", 2, rgb565(160,170,196));
  } else if(cnt == 0){
    drawTextC(SCR_W / 2, 300, "No se encontraron redes", 2, rgb565(160,170,196));
  } else {
    for(int i = 0; i < cnt; i++){
      int y = wifiRowY(i); if(y > SCR_H - 60) break;
      fillRoundRect(24, y, SCR_W - 48, 56, 14, rgb565(30,34,48));
      drawWifi(56, y + 28, 13, rgb565(255,255,255));
      drawTextClip(88, y + 12, wifiNets[i].ssid, 2, rgb565(240,242,248), SCR_W - 100);
      if(wifiNets[i].secure){
        fillRoundRect(SCR_W - 80, y + 20, 16, 14, 3, rgb565(180,186,204));
        arcStroke(SCR_W - 72, y + 20, 6, 180, 360, 2, rgb565(180,186,204));
      }
    }
  }
  int by, sx, sw, fx, fw;
  wifiBtnRects(by, sx, sw, fx, fw);
  fillRoundRect(sx, by, sw, 56, 16, rgb565(60,110,235));       // "Buscar redes" (reescanear)
  drawTextC(sx + sw / 2, by + 18, wifiUIState == WUI_SCANNING ? "Buscando..." : "Buscar redes", 2, rgb565(255,255,255));
  if(fw > 0){                                                   // "Olvidar red" (cambio de router)
    fillRoundRect(fx, by, fw, 56, 16, rgb565(58,62,80));
    drawTextC(fx + fw / 2, by + 18, "Olvidar red", 2, rgb565(240,180,180));
  }
  flxFlushAll();
}
static void wifiRenderPass(int yoff){
  setBuf(bbuf);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(14,16,24));
  wifiBack();
  char title[48]; snprintf(title, sizeof(title), "Contrase\xC3\xB1" "a de %s", wifiNets[wifiSel].ssid);
  drawTextC(SCR_W / 2, 50, title, 2, rgb565(255,255,255));
  int cnt = utf8Count(wifiPass);
  for(int i = 0; i < cnt && i < 18; i++) fillCircle(30 + i * 24, 120, 7, rgb565(90,150,240));
  int ky = KB_Y + yoff;
  if(uiGlass){ drawLiquidGlassPanel(0, ky - 4, SCR_W, SCR_H - (ky - 4), 0, rgb565(36,40,58)); }
  else fillRect(0, ky - 4, SCR_W, SCR_H - (ky - 4), rgb565(18,20,28));
  int fs = kbFontSize();
  for(int r = 0; r < KB_ROWS; r++) for(int c = 0; c < KB_COLS; c++){
    int x = KB_X + c * (KB_KW + KB_GAP), y = ky + r * (KB_KH + KB_GAP);
    const char* k = mapaActivo[r][c];
    char u[6];
    if(kbShift && k[1] == 0 && k[0] >= 'a' && k[0] <= 'z'){ u[0] = (char)(k[0] - 32); u[1] = 0; k = u; }
    kbPaintKey(x, y, KB_KW, KB_KH, k, fs, kbColKey(), kbColKeyTxt(), false);
  }
  int fy = ky + 3 * (KB_KH + KB_GAP);
  const char* lb[KB_FKEYS] = { "shift", kbLayerLabel(), kbLangEs ? "ES" : "EN", "espacio", "<-", "Conectar" };
  for(int i = 0; i < KB_FKEYS; i++) kbFKey(kbFKeyX(i), fy, kbFKeyW(i), lb[i], (i == 0) && kbShift);
  present(0, SCR_H - 1);
}
static void wifiRenderStatus(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(14,16,24));
  wifiBack();
  if(wifiUIState == WUI_OK){
    drawCircle(SCR_W / 2, 280, 46, rgb565(90,220,140)); drawCircle(SCR_W / 2, 280, 45, rgb565(90,220,140));
    strokeSegAA(SCR_W / 2 - 20, 280, SCR_W / 2 - 4, 300, 4.0f, rgb565(90,220,140));
    strokeSegAA(SCR_W / 2 - 4, 300, SCR_W / 2 + 26, 260, 4.0f, rgb565(90,220,140));
    drawTextC(SCR_W / 2, 350, "Conectado", 3, rgb565(255,255,255));
    char ipl[40]; snprintf(ipl, sizeof(ipl), "IP: %s", wifiConnIP);
    drawTextC(SCR_W / 2, 390, ipl, 2, rgb565(160,170,196));
    fillRoundRect(SCR_W / 2 - 100, SCR_H - 120, 200, 56, 16, rgb565(60,110,235));
    drawTextC(SCR_W / 2, SCR_H - 102, "Listo", 2, rgb565(255,255,255));
  } else if(wifiUIState == WUI_CONNECTING){
    drawTextC(SCR_W / 2, 280, "Conectando...", 3, rgb565(255,255,255));
    char sub[48]; snprintf(sub, sizeof(sub), "a %s", wifiConnSSID);
    drawTextC(SCR_W / 2, 320, sub, 2, rgb565(160,170,196));
  } else {                                     // WUI_FAIL
    drawCircle(SCR_W / 2, 280, 46, rgb565(230,90,90)); drawCircle(SCR_W / 2, 280, 45, rgb565(230,90,90));
    strokeSegAA(SCR_W / 2 - 14, 264, SCR_W / 2 + 14, 296, 4.0f, rgb565(230,90,90));
    strokeSegAA(SCR_W / 2 + 14, 264, SCR_W / 2 - 14, 296, 4.0f, rgb565(230,90,90));
    drawTextC(SCR_W / 2, 350, "No se pudo conectar", 3, rgb565(255,255,255));
    drawTextC(SCR_W / 2, 388, "Contrase\xC3\xB1" "a incorrecta o red fuera de rango", 1, rgb565(160,170,196));
    fillRoundRect(SCR_W / 2 - 210, SCR_H - 120, 200, 56, 16, rgb565(70,74,90));
    drawTextC(SCR_W / 2 - 110, SCR_H - 102, "Cancelar", 2, rgb565(255,255,255));
    fillRoundRect(SCR_W / 2 + 10, SCR_H - 120, 200, 56, 16, rgb565(60,110,235));
    drawTextC(SCR_W / 2 + 110, SCR_H - 102, "Reintentar", 2, rgb565(255,255,255));
  }
  flxFlushAll();
}
static void wifiRenderUnavail(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(14,16,24));
  wifiBack();
  drawTextC(SCR_W / 2, 60, "Wi-Fi", 4, rgb565(255,255,255));
  drawTextC(SCR_W / 2, 300, "Wi-Fi desactivado en este build", 2, rgb565(160,170,196));
  drawTextC(SCR_W / 2, 334, "(FLEXOS_ENABLE_WIFI = 0 en el .ino)", 1, rgb565(120,128,150));
  flxFlushAll();
}
static void wifiExit(){ gState = ST_APP; settingsRender(); }

static void wifiSettingsEnter(){
  gState = ST_WIFI;
#if FLEXOS_ENABLE_WIFI
  wifiSel = -1; wifiPass[0] = 0;
  mapaActivo = LAYOUT_ES; kbLangEs = true; kbShift = false;
  kbExtrasOn = false; kbApplySize(); kbMtSurfaceReset();   // el teclado de Wi-Fi solo hereda el TAMANO (Fase A)
  wifiStartScan();
  wifiRenderList();
#else
  wifiRenderUnavail();
#endif
}
static void wifiTick(){
#if !FLEXOS_ENABLE_WIFI
  if(T.tap && T.x < 48 && T.y < 48) wifiExit();
  return;
#else
  // Repinta UNA vez cuando el estado cambia por causas externas (la
  // tarea de escaneo/conexion en Core 1 termino). Las transiciones que
  // dispara el propio tap ya pintan de inmediato mas abajo.
  static int wifiUIStateShown = -1;
  if(wifiUIState != wifiUIStateShown){
    wifiUIStateShown = wifiUIState;
    if(wifiUIState == WUI_LIST || wifiUIState == WUI_SCANNING) wifiRenderList();
    else if(wifiUIState == WUI_CONNECTING || wifiUIState == WUI_OK || wifiUIState == WUI_FAIL) wifiRenderStatus();
  }
  switch(wifiUIState){
    case WUI_LIST: {
      if(T.tap){
        if(T.x < 48 && T.y < 48){ wifiExit(); return; }
        int by, sx, sw, fx, fw;
        wifiBtnRects(by, sx, sw, fx, fw);
        if(T.y >= by && T.y <= by + 56){
          if(T.x >= sx && T.x <= sx + sw){ wifiStartScan(); return; }
          if(fw > 0 && T.x >= fx && T.x <= fx + fw){       // Olvidar red guardada
            wifiCredsForget();
            WiFi.disconnect(true, true);                   // suelta tambien la sesion en curso
            gNetOnline = false;
            wifiRenderList();                              // el boton desaparece al no haber red
            return;
          }
        }
        int cnt; portENTER_CRITICAL(&wifiMux); cnt = wifiNetCount; portEXIT_CRITICAL(&wifiMux);
        for(int i = 0; i < cnt; i++){
          int y = wifiRowY(i); if(y > SCR_H - 60) break;
          if(T.y >= y && T.y <= y + 56 && T.x >= 24 && T.x <= SCR_W - 24){
            wifiSel = i;
            if(!wifiNets[i].secure) wifiStartConnect();                          // red abierta: conecta directo
            else { wifiPass[0] = 0; wifiUIState = WUI_PASS; wifiKbAnim = millis(); }
            return;
          }
        }
      }
      break;
    }
    case WUI_SCANNING: break;    // pantalla estatica "Buscando..."; el repintado de arriba cambia a LIST solo
    case WUI_PASS: {
      if(wifiKbAnim){
        float p = (millis() - wifiKbAnim) / 300.0f; if(p >= 1){ p = 1; wifiKbAnim = 0; }
        wifiRenderPass((int)((1.0f - p) * (SCR_H - KB_Y)));
        return;
      }
      if(T.tap){
        if(T.x < 48 && T.y < 48){ wifiUIState = WUI_LIST; wifiRenderList(); return; }
        int fi = kbFRowHit(T.x, T.y);
        if(fi >= 0){
          if(fi == 0) kbShift = !kbShift;
          else if(fi == 1) mapaActivo = (mapaActivo == LAYOUT_NUM) ? LAYOUT_EMOJI : (mapaActivo == LAYOUT_EMOJI) ? (kbLangEs ? LAYOUT_ES : LAYOUT_EN) : LAYOUT_NUM;
          else if(fi == 2){ kbLangEs = !kbLangEs; if(mapaActivo == LAYOUT_ES || mapaActivo == LAYOUT_EN) mapaActivo = kbLangEs ? LAYOUT_ES : LAYOUT_EN; }
          else if(fi == 3) wifiPassAppend(" ");
          else if(fi == 4) wifiPassBackspace();
          else if(strlen(wifiPass) > 0){ wifiStartConnect(); return; }
          wifiRenderPass(0); return;
        }
        int cell = kbCellAt(T.x, T.y);
        if(cell >= 0){
          const char* k = mapaActivo[cell / KB_COLS][cell % KB_COLS];
          if(kbShift && k[1] == 0 && k[0] >= 'a' && k[0] <= 'z'){ char u[2] = { (char)(k[0] - 32), 0 }; wifiPassAppend(u); kbShift = false; }
          else wifiPassAppend(k);
          wifiRenderPass(0);
        }
      }
      break;
    }
    case WUI_CONNECTING: break;  // pantalla estatica "Conectando..."; el repintado de arriba cambia a OK/FAIL solo
    case WUI_OK: {
      if(T.tap){
        if(T.x < 48 && T.y < 48){ wifiExit(); return; }
        if(T.y >= SCR_H - 120 && T.y <= SCR_H - 64 && T.x >= SCR_W/2 - 100 && T.x <= SCR_W/2 + 100){ wifiExit(); return; }
      }
      break;
    }
    case WUI_FAIL: {
      if(T.tap){
        if(T.x < 48 && T.y < 48){ wifiUIState = WUI_LIST; wifiRenderList(); return; }
        if(T.y >= SCR_H - 120 && T.y <= SCR_H - 64){
          if(T.x >= SCR_W/2 - 210 && T.x <= SCR_W/2 - 10){ wifiUIState = WUI_LIST; wifiRenderList(); return; }             // cancelar
          if(T.x >= SCR_W/2 + 10 && T.x <= SCR_W/2 + 210){ wifiPass[0] = 0; wifiUIState = WUI_PASS; wifiKbAnim = millis(); return; }  // reintentar
        }
      }
      break;
    }
  }
#endif
}

// #############################################################
// ##  ISLA DINAMICA · logica y render  (FASE 1, parche anti-flicker)
// ##  ------------------------------------------------------
// ##  FIX aplicado tras el bug de parpadeo + tarjetas pegadas:
// ##  la isla ya NO compone sobre fb (el buffer que flxPresenter
// ##  lee por DMA en otro core). Ahora restaura la banda limpia
// ##  copiando desde homeBuf hacia bbuf, dibuja las tarjetas sobre bbuf, y cruza
// ##  a fb con un unico present() atomico. bbuf es de un solo
// ##  escritor (el loop task); nadie mas lo lee, asi que el
// ##  presenter nunca puede capturar un frame a medio pintar.
// ##  Por eso el compose (restore+dibujar+present) solo corre con
// ##  gState==ST_HOME: homeBuf solo es un fondo valido ahi. El
// ##  avance de fases sigue sin condicion (es aritmetica pura).
// ##
// ##  En Fase 1 NO hay deteccion I2C real: las notificaciones se
// ##  disparan con un trigger de prueba (demo al primer Home + tap
// ##  arriba-derecha) para validar render/animacion/descarte de
// ##  forma AISLADA.
// ##
// ##  DESVIACION DELIBERADA respecto al plan original: el vidrio
// ##  se RE-HORNEA cada frame (drawLiquidGlassPanel) en vez de
// ##  "hornear una vez". Se hace asi solo porque son <=3 tarjetas
// ##  pequenas (448x64) y el coste es bajo; permite que el
// ##  deslizamiento reutilice el mismo camino sin cachear un buffer
// ##  por tarjeta. El blur costoso (pantalla completa) se sigue evitando.
// #############################################################

// #############################################################
// ##  CONECTIVIDAD  ·  Wi-Fi / BLE / Modo avion   (ST_CONN)
// ##  ------------------------------------------------------
// ##  Los tres interruptores encienden y apagan radio DE
// ##  VERDAD. Ninguno es un booleano que solo cambia de color:
// ##
// ##   · Wi-Fi     -> WiFi.mode(WIFI_STA) + WiFi.begin() con la
// ##                  red guardada, o WiFi.scanNetworks() (en su
// ##                  tarea) si aun no hay ninguna. Al apagar,
// ##                  WiFi.disconnect(true,true) + WIFI_OFF.
// ##                  El subtitulo es WiFi.SSID(): el nombre
// ##                  REAL de la red a la que se esta conectado.
// ##   · BLE       -> BLEDevice::init() + advertising real (el
// ##                  equipo aparece como "FlexOS" en cualquier
// ##                  movil) y BLEDevice::deinit(true) al
// ##                  apagar. En una placa SIN radio Bluetooth
// ##                  el interruptor queda deshabilitado con la
// ##                  etiqueta "No disponible", y eso se decide
// ##                  en COMPILACION mirando SOC_BLE_SUPPORTED
// ##                  del propio SDK -- no esta escrito a mano
// ##                  para un chip concreto.
// ##   · Modo avion-> apaga las dos radios llamando a las
// ##                  funciones reales de apagado y deja los
// ##                  otros dos interruptores bloqueados
// ##                  mientras este activo. Se guarda en NVS y,
// ##                  si estaba activo, el arranque NO lanza la
// ##                  reconexion automatica de Wi-Fi.
// #############################################################
#define CONN_CARD_X   14
#define CONN_CARD_W   (SCR_W - 28)
#define CONN_ROW_H    112
#define CONN_CARD1_Y  110
#define CONN_CARD2_Y  (CONN_CARD1_Y + 2 * CONN_ROW_H + 26)

// (gAirplane y gBleOn se declaran arriba del todo, junto a gNetOnline: la
//  pantalla de Ajustes -- que esta ANTES en el archivo -- necesita leerlos.)

// ---- BLE real -------------------------------------------------
// FLEXOS_BLE_HW lo decide el SDK (soc_caps.h) segun el chip que se
// esta compilando. FLEXOS_ENABLE_BLE permite ademas apagarlo a mano
// cuando la placa SI tiene radio pero no sobra flash para la pila BLE
// (ver la cabecera del sketch de la placa correspondiente).
static bool flexBleStart(){
#if FLEXOS_ENABLE_BLE
  if(gBleOn) return true;
  BLEDevice::init("FlexOS");
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  if(!adv){ BLEDevice::deinit(true); return false; }
  adv->setScanResponse(true);
  adv->start();                      // desde aqui el equipo es visible de verdad
  gBleOn = true;
  Serial.println(F("[BLE] advertising activo como \"FlexOS\""));
  return true;
#else
  return false;
#endif
}

static void flexBleStop(){
#if FLEXOS_ENABLE_BLE
  if(!gBleOn) return;
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  if(adv) adv->stop();
  BLEDevice::deinit(true);           // libera el controlador, no solo la capa alta
  gBleOn = false;
  Serial.println(F("[BLE] apagado"));
#endif
}

// ---- Wi-Fi real -----------------------------------------------
static bool connWifiOn(){
#if FLEXOS_ENABLE_WIFI
  return WiFi.getMode() != WIFI_OFF;
#else
  return false;
#endif
}

static void connWifiSet(bool on){
#if FLEXOS_ENABLE_WIFI
  if(on){
    if(gAirplane) return;                       // bloqueo real, no visual
    wifiEnsureStaMode();                        // enciende la pila en modo estacion
    if(wifiCredsExist()){
      // Hay red guardada -> WiFi.begin() real contra ella, en su tarea
      // (loop() no se bloquea; misma ruta que la reconexion de arranque).
      gWifiAutoDone = false; gWifiAutoBusy = false;
      wifiTryAutoConnect();
    } else {
      wifiStartScan();                          // sin red guardada -> escaneo real
    }
  } else {
    WiFi.disconnect(true, true);                // suelta la conexion y libera la STA
    WiFi.mode(WIFI_OFF);                        // y apaga la radio
    gNetOnline = false;
    gWifiAutoDone = true;                       // que no vuelva a encenderse sola
  }
#else
  (void)on;
#endif
}

// Subtitulo del Wi-Fi: SSID REAL, nunca un nombre de ejemplo.
static void connWifiSub(char* out, size_t n){
#if !FLEXOS_ENABLE_WIFI
  snprintf(out, n, "(No disponible)");
#else
  if(gAirplane){ snprintf(out, n, "(Modo avi\xC3\xB3n)"); return; }
  if(!connWifiOn()){ snprintf(out, n, "(Desactivado)"); return; }
  if(WiFi.status() == WL_CONNECTED){
    String s = WiFi.SSID();
    if(s.length() > 0) snprintf(out, n, "(%s)", s.c_str());
    else               snprintf(out, n, "(Conectado)");
    return;
  }
  if(gWifiAutoBusy)               { snprintf(out, n, "(Conectando...)"); return; }
  if(wifiUIState == WUI_SCANNING) { snprintf(out, n, "(Buscando redes...)"); return; }
  snprintf(out, n, "(No conectado)");
#endif
}

static void connBleSub(char* out, size_t n){
#if !FLEXOS_BLE_HW
  // El chip que se esta compilando no tiene radio Bluetooth (caso del
  // ESP32-P4, que delega el Wi-Fi en un C6 pero no expone BLE).
  snprintf(out, n, "(No disponible)");
#elif !FLEXOS_ENABLE_BLE
  snprintf(out, n, "(Desactivado al compilar)");
#else
  if(gAirplane)   snprintf(out, n, "(Modo avi\xC3\xB3n)");
  else if(gBleOn) snprintf(out, n, "(Visible como \"FlexOS\")");
  else            snprintf(out, n, "(Desactivado)");
#endif
}

static void connSaveState(){
  prefs.begin("flexos", false);
  prefs.putBool("airpl", gAirplane);
  prefs.end();
}

// Se llama desde setup(). Solo lee NVS (flash, no radio): sigue en pie la
// regla de que el arranque NO toca la radio.
static void connBootRestore(){
  prefs.begin("flexos", true);
  gAirplane = prefs.getBool("airpl", false);
  prefs.end();
#if FLEXOS_ENABLE_WIFI
  // Si el equipo se apago en modo avion, el arranque no debe reconectar
  // solo: seria encender una radio que el usuario dejo apagada.
  if(gAirplane) gWifiAutoDone = true;
#endif
  if(gAirplane) Serial.println(F("[RADIO] modo avion activo desde el arranque"));
}

static void connAirplaneSet(bool on){
  gAirplane = on;
  if(on){
    flexBleStop();          // apagado real de las dos radios
    connWifiSet(false);
  }
  connSaveState();
}

// ---- Interruptor (pastilla) -----------------------------------
static void connPill(int x, int y, int w, int h, bool on, bool enabled){
  uint16_t track = !enabled ? rgb565(150,152,158) : (on ? rgb565(20,215,90) : rgb565(150,152,158));
  fillRoundRect(x, y, w, h, h / 2, track);
  int r = h / 2 - 4;
  int cx = on ? (x + w - h / 2) : (x + h / 2);
  fillCircle(cx, y + h / 2, r, rgb565(255,255,255));
  if(!enabled){                                  // aspa tenue: no se puede tocar
    strokeSegAA(cx - 6, y + h / 2 - 6, cx + 6, y + h / 2 + 6, 1.6f, rgb565(190,192,198));
    strokeSegAA(cx + 6, y + h / 2 - 6, cx - 6, y + h / 2 + 6, 1.6f, rgb565(190,192,198));
  }
}

// Geometria de las tres filas. UNA sola fuente para dibujo y para el
// tap: no puede desalinearse lo pintado de lo pulsable.
static void connRowRect(int i, int &x, int &y, int &w, int &h){
  x = CONN_CARD_X; w = CONN_CARD_W; h = CONN_ROW_H;
  if(i == 0)      y = CONN_CARD1_Y;
  else if(i == 1) y = CONN_CARD1_Y + CONN_ROW_H;
  else            y = CONN_CARD2_Y;
}

static bool connRowEnabled(int i){
  if(i == 2) return true;                        // modo avion siempre se puede tocar
  if(gAirplane) return false;                    // con el avion puesto, los otros dos no
  if(i == 0) return FLEXOS_ENABLE_WIFI ? true : false;
  return FLEXOS_ENABLE_BLE ? true : false;
}

static bool connRowOn(int i){
  if(i == 0) return connWifiOn();
  if(i == 1) return gBleOn;
  return gAirplane;
}

// Solo el CONTENIDO de la fila (titulo, subtitulo y pastilla). El fondo lo
// pinta connPaintCards con esquinas redondeadas: si cada fila se rellenara a si
// misma con un rectangulo recto, se comeria las esquinas de la tarjeta.
static void connDrawRow(int i){
  int x, y, w, h; connRowRect(i, x, y, w, h);
  uint16_t tx = rgb565(16,18,24), sb = rgb565(60,64,74);
  const char* title = (i == 0) ? "Wifi" : (i == 1) ? "BLE" : "Modo avi\xC3\xB3n";
  char sub[64]; sub[0] = 0;
  if(i == 0) connWifiSub(sub, sizeof(sub));
  else if(i == 1) connBleSub(sub, sizeof(sub));
  bool en = connRowEnabled(i);
  drawText(x + 22, y + (sub[0] ? 16 : 36), title, 5, en ? tx : rgb565(110,114,124));
  if(sub[0]) drawTextClip(x + 22, y + 62, sub, 3, en ? sb : rgb565(130,134,144), x + w - 130);
  connPill(x + w - 116, y + h / 2 - 22, 100, 44, connRowOn(i), en);
}

// Las dos tarjetas, enteras. Se pinta primero el fondo redondeado y despues
// el contenido de cada fila: asi el repintado parcial (connRefresh) deja
// exactamente el mismo resultado que el completo.
static void connPaintCards(){
  fillRoundRect(CONN_CARD_X, CONN_CARD1_Y, CONN_CARD_W, 2 * CONN_ROW_H, 24, rgb565(214,214,214));
  connDrawRow(0);
  connDrawRow(1);
  fillRect(CONN_CARD_X + 8, CONN_CARD1_Y + CONN_ROW_H - 1, CONN_CARD_W - 16, 2, rgb565(20,22,28));
  fillRoundRect(CONN_CARD_X, CONN_CARD2_Y, CONN_CARD_W, CONN_ROW_H, 24, rgb565(214,214,214));
  connDrawRow(2);
}

static void connRender(){
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(244,244,244));
  strokeSegAA(30, 26, 18, 18, 2.4f, rgb565(16,18,24));
  strokeSegAA(18, 18, 30, 10, 2.4f, rgb565(16,18,24));
  drawTextC(SCR_W / 2, 30, "Conectividad", 3, rgb565(16,18,24));
  connPaintCards();
  if(gAirplane)
    drawTextC(SCR_W / 2, CONN_CARD2_Y + CONN_ROW_H + 24,
              "Con el modo avi\xC3\xB3n activo, Wifi y BLE quedan apagados", 1, rgb565(90,94,104));
  flxFlushAll();
}

// Repinta SOLO la banda de las tarjetas. Se usa cuando el estado cambia por
// causas externas (la tarea de conexion termino y ya hay SSID real): no se
// repinta la pantalla entera por un cambio de subtitulo.
static void connRefresh(){
  setBuf(fb);
  connPaintCards();
  flxFlush(CONN_CARD1_Y - 2, CONN_CARD2_Y + CONN_ROW_H + 2);
}

static char     connLastSub[64] = "";
static uint32_t connPollMs = 0;

static void connEnter(){
  gState = ST_CONN;
  connLastSub[0] = 0;
  connRender();
}

static void connExit(){ gState = ST_APP; settingsRender(); }

static void connTick(){
  // Refresco por cambio REAL de estado: se compara el subtitulo que tocaria
  // pintar con el ultimo pintado. Mientras no cambie nada, no se toca la
  // pantalla (ni un solo flxFlush de balde).
  //
  // La comprobacion va limitada a 4 veces por segundo A PROPOSITO: WiFi.SSID()
  // devuelve un String (malloc + free) y hacerlo en cada vuelta de loop() -que
  // corre cada ~5 ms- seria fragmentar el heap para nada. El SSID no cambia
  // 200 veces por segundo.
  if(millis() - connPollMs > 250){
    connPollMs = millis();
    char now[64]; connWifiSub(now, sizeof(now));
    if(strcmp(now, connLastSub)){
      strncpy(connLastSub, now, sizeof(connLastSub) - 1);
      connLastSub[sizeof(connLastSub) - 1] = 0;
      connRefresh();
    }
  }

  if(!T.tap) return;
  if(T.x < 60 && T.y < 60){ connExit(); return; }
  for(int i = 0; i < 3; i++){
    int x, y, w, h; connRowRect(i, x, y, w, h);
    if(T.x < x || T.x > x + w || T.y < y || T.y > y + h) continue;
    if(!connRowEnabled(i)) return;                       // deshabilitado: no hace nada
    if(i == 0)      connWifiSet(!connWifiOn());
    else if(i == 1){ if(gBleOn) flexBleStop(); else flexBleStart(); }
    else            connAirplaneSet(!gAirplane);
    connLastSub[0] = 0;                                  // fuerza el refresco de la banda
    connRender();
    return;
  }
}

// Icono del modulo: reutiliza los iconos de app existentes (mapeo simple)
static void drawModuleIcon(ModuleType type, int x, int y, int S){
  int id = IC_AJUSTES;
  switch(type){
    case MOD_ULTRASONIC:  id = IC_NAV;     break;
    case MOD_BME280:      id = IC_BIEN;    break;
    case MOD_MPU6050:     id = IC_JUEGOS;  break;
    case MOD_LED:         id = IC_CALC;    break;
    case MOD_BUTTON:      id = IC_NOTAS;   break;
    case MOD_SERVO:       id = IC_MODOPC;  break;
    case MOD_I2C_GENERIC: id = IC_ALMACEN; break;
    default:              id = IC_AJUSTES; break;
  }
  drawAppIcon(id, x, y, S);
}

// Restaura la banda limpia EN bbuf, copiando desde homeBuf (siempre al dia:
// se recompone solo en cada cambio de minuto, al salir de edicion, etc.).
// homeBuf es la fuente y bbuf el lienzo de trabajo. Ya no hace falta
// snapshot manual (notifSnapshotBg desaparece).
static void notifRestoreBg(){
  if(!homeBuf || !bbuf) return;
  memcpy(bbuf + (size_t)NOTIF_BAND_TOP * SCR_W, homeBuf + (size_t)NOTIF_BAND_TOP * SCR_W,
         (size_t)SCR_W * NOTIF_BAND_H * 2);
}

// Encola una notificacion a partir de un modulo
static void notifPush(const DetectedModule* m){
  if(gNotifCount >= NOTIF_MAX) return;           // cola llena: se descarta (Fase 1)
  Notification* n = &gNotifs[gNotifCount++];
  n->mod    = *m;
  n->active = true;
  n->phase  = NP_IN;
  n->bornMs = millis();
  n->slideX = 0.0f;
  n->armed  = false;            // se arma (entrada + 5 s) al hacerse visible en Home
}

// Elimina la ranura idx y compacta la cola
static void notifRemove(int idx){
  if(idx < 0 || idx >= gNotifCount) return;
  for(int j = idx; j < gNotifCount - 1; j++) gNotifs[j] = gNotifs[j + 1];
  gNotifCount--;
  gNotifs[gNotifCount].active = false;
  if(notifDragIdx == idx)      notifDragIdx = -1;
  else if(notifDragIdx > idx)  notifDragIdx--;
}

// Ease-out cubica (0..1)
static inline float notifEaseOut(float p){ float q = 1.0f - p; return 1.0f - q * q * q; }

// Cola de burbuja de chat: un triangulo apuntando hacia ARRIBA, porque las
// tarjetas de notificacion caen desde el borde superior de la pantalla (no
// hay un icono de app en el Home al que apuntar -- estas son detecciones de
// hardware I2C via hwDetectTick(), no notificaciones que vengan de una app
// abierta). Solido, no vidrio: es demasiado pequeña para que el blur se
// note, y agrandar el panel solo para la cola no vale la pena.
static void notifDrawTail(int cx, int topY, uint16_t col){
  fillTriangle(cx - 8, topY, cx + 8, topY, cx, topY - 9, col);
}
// Dibuja una tarjeta en la coordenada Y dada (aplica su slideX horizontal)
static void notifDrawCard(Notification* n, int cardY){
  int x = NOTIF_MARGIN_X + (int)n->slideX;       // al deslizar a la izq, x se vuelve negativo
  int y = cardY, w = NOTIF_CARD_W, h = NOTIF_CARD_H;
  notifDrawTail(x + w / 2, y, rgb565(90, 120, 200));   // primero: la tarjeta se dibuja justo debajo, sin solaparla
  // Vidrio base (blur). drawLiquidGlassPanel recorta x<0
  // conservando el borde derecho -> el deslizamiento a la izquierda sale natural.
  drawLiquidGlassPanel(x, y, w, h, NOTIF_RAD, rgb565(40, 60, 130));
  // Degradado extra estilo burbuja (mas claro arriba, mas oscuro abajo).
  // Fila a fila con glInset() -- igual que drawLiquidGlassPanel -- para no
  // salirse de las esquinas redondeadas (una fillRectA plana sí se saldría).
  for(int j = 0; j < h; j++){
    int ins = glInset(j, h, NOTIF_RAD);
    uint8_t a = (uint8_t)(42 - 42 * j / h);
    if(a > 0) hLineA(x + ins, y + j, w - 2 * ins, rgb565(255,255,255), a);
  }
  drawRoundRect(x, y, w, h, NOTIF_RAD, rgb565(200, 210, 230));
  // Icono 40x40 (las primitivas acotan coords negativas: seguro fuera de pantalla)
  drawModuleIcon(n->mod.type, x + 12, y + (h - 40) / 2, 40);
  // Textos
  drawText(x + 62, y + 14, n->mod.name, 2, rgb565(255, 255, 255));
  drawText(x + 62, y + 38, n->mod.sub,  1, rgb565(205, 214, 232));
  // Boton cerrar (X)
  int cx = x + w - 22, cy = y + 20;
  strokeSegAA(cx - 5, cy - 5, cx + 5, cy + 5, 1.8f, rgb565(255, 255, 255));
  strokeSegAA(cx - 5, cy + 5, cx + 5, cy - 5, 1.8f, rgb565(255, 255, 255));
}

// ---- Trigger de PRUEBA (solo Fase 1; se retira/reemplaza en Fase 2) ----
// Compilado SOLO si NOTIF_DEMO_TRIGGERS = 1. Sus dos unicos llamadores estan
// bajo el mismo #if, asi que dejarlo siempre presente daba un aviso de funcion
// estatica sin usar en cada compilacion.
#if NOTIF_DEMO_TRIGGERS
// Empuja una notificacion demo, rotando por tipos para ver todos los iconos.
static void notifPushDemo(){
  static uint8_t k = 0;
  DetectedModule m;
  memset(&m, 0, sizeof(m));
  m.active = true; m.detectedAt = millis();
  switch(k % 5){
    case 0: m.type = MOD_BME280;      m.i2cAddr = 0x76;
            snprintf(m.name, sizeof(m.name), "Sensor BME280");
            snprintf(m.sub,  sizeof(m.sub),  "I2C 0x76 detectado"); break;
    case 1: m.type = MOD_MPU6050;     m.i2cAddr = 0x68;
            snprintf(m.name, sizeof(m.name), "MPU6050");
            snprintf(m.sub,  sizeof(m.sub),  "IMU - I2C 0x68"); break;
    case 2: m.type = MOD_ULTRASONIC;
            snprintf(m.name, sizeof(m.name), "Ultrasonido");
            snprintf(m.sub,  sizeof(m.sub),  "HC-SR04 detectado"); break;
    case 3: m.type = MOD_SERVO;
            snprintf(m.name, sizeof(m.name), "Servo");
            snprintf(m.sub,  sizeof(m.sub),  "Actuador PWM"); break;
    default:m.type = MOD_I2C_GENERIC; m.i2cAddr = 0x3C;
            snprintf(m.name, sizeof(m.name), "Dispositivo I2C");
            snprintf(m.sub,  sizeof(m.sub),  "0x3C detectado"); break;
  }
  k++;
  notifPush(&m);
}
#endif   // NOTIF_DEMO_TRIGGERS

// ---- Toque de la isla: intercepta SOLO dentro de las tarjetas ----
// Se llama en loop() justo despues de flexPollTouch() y antes del switch de
// estado. Consume unicamente los flags de evento que usa (tap/pressed/released/
// swipeLeft); NUNCA toca T.down (lo gestiona flexPollTouch) para no corromper la
// maquina de estados del tactil.
static void notifHandleTouch(){
  // La isla solo recibe toques cuando es visible (Home principal desbloqueado).
  if(gState != ST_HOME || qsPanelY != 0 || editMode){ notifDragIdx = -1; return; }
  // Toques en tarjetas (cerrar, flick, iniciar arrastre)
  for(int i = 0; i < gNotifCount; i++){
    if(!gNotifs[i].active || gNotifs[i].phase == NP_OUT) continue;
    int cardY = NOTIF_Y0 + i * (NOTIF_CARD_H + NOTIF_GAP);
    int x0 = NOTIF_MARGIN_X, x1 = NOTIF_MARGIN_X + NOTIF_CARD_W;
    int y0 = cardY, y1 = cardY + NOTIF_CARD_H;
    // Boton cerrar (X) arriba-derecha
    int cx = NOTIF_MARGIN_X + NOTIF_CARD_W - 22, cy = cardY + 20;
    if(T.tap && abs(T.x - cx) < 16 && abs(T.y - cy) < 16){
      gNotifs[i].phase = NP_OUT; T.tap = false; T.pressed = false; return;
    }
    // Flick rapido a la izquierda sobre la tarjeta
    if(T.swipeLeft && T.startY >= y0 && T.startY <= y1){
      gNotifs[i].phase = NP_OUT; T.swipeLeft = false; T.tap = false; return;
    }
    // Iniciar arrastre (dedo dentro de la tarjeta)
    if(T.pressed && T.x >= x0 && T.x <= x1 && T.y >= y0 && T.y <= y1){
      notifDragIdx = i; T.pressed = false;
    }
  }
  // Arrastre en curso
  if(notifDragIdx >= 0 && notifDragIdx < gNotifCount){
    Notification* n = &gNotifs[notifDragIdx];
    if(T.down){
      float dx = (float)(T.x - T.startX);
      if(dx > 0) dx = 0;                                    // solo hacia la izquierda
      if(dx < -(NOTIF_CARD_W + 40)) dx = -(NOTIF_CARD_W + 40);
      n->slideX = dx; n->phase = NP_DRAG;
      T.tap = false; T.swipeLeft = false;                  // no propagar a la pantalla
    } else {
      // Soltar: descartar si paso el umbral, si no volver a su sitio
      if(n->slideX < -NOTIF_CARD_W / 4) n->phase = NP_OUT;
      else                              n->phase = NP_SPRING;
      notifDragIdx = -1;
      T.tap = false; T.released = false;
    }
  }
#if NOTIF_DEMO_TRIGGERS
  // Re-trigger de PRUEBA: tap arriba-derecha, fuera de la zona caliente de la
  // cortina (que captura startY<30) y solo en Home. Genera la siguiente demo.
  if(T.tap && gState == ST_HOME && qsPanelY == 0 && !editMode &&
     T.x >= SCR_W - 52 && T.y >= 36 && T.y <= 56){
    notifPushDemo();
    T.tap = false; T.pressed = false;
  }
#endif
}

// ---- Tick de la isla: anima y compone (se llama al final de loop) ----
static void notifTick(){
  // Trigger de prueba: primera demo al llegar a Home
#if NOTIF_DEMO_TRIGGERS
  static bool bootDemo = false;
  if(!bootDemo && gState == ST_HOME && millis() > 1200){ bootDemo = true; notifPushDemo(); }
#endif

  // Throttle ~30 fps
  if(millis() - notifLastMs < 33) return;
  notifLastMs = millis();

  // Nada que mostrar y banda ya limpia -> salida barata
  if(gNotifCount == 0 && !notifBandOn) return;

  // La isla SOLO vive en el Home principal desbloqueado (sin cortina ni edicion).
  // Fuera de ahi no avanzamos fases ni dibujamos: las notificaciones detectadas
  // durante el bloqueo esperan congeladas y su animacion de entrada + los 5 s
  // arrancan al llegar aqui. Asi tambien evitamos el conflicto de dibujo con
  // otras pantallas (que son quienes deben poseer el fb en ese momento).
  if(gState != ST_HOME || qsPanelY != 0 || editMode){
    if(!notifPaused){ notifPaused = true; notifPauseT0 = millis(); }   // marca el inicio de la pausa (p.ej. se abrio una app)
    return;
  }
  if(notifPaused){
    // Reanudando tras una pausa (p.ej. se cerro la app que se abrio encima):
    // sumar el tiempo pausado a bornMs de cada tarjeta activa para que
    // conserven el tiempo que les quedaba, en vez de que millis()-bornMs se
    // dispare de golpe y todas pasen de fase (y se reindexen) en el mismo
    // frame -- eso era el parpadeo/"se queda bugeado" al volver de una app.
    uint32_t paused = millis() - notifPauseT0;
    for(int i = 0; i < gNotifCount; i++) gNotifs[i].bornMs += paused;
    notifPaused = false;
  }
  if(gNotifCount > 0) notifBandOn = true;

  // Armar la entrada de las notificaciones aun no mostradas
  for(int i = 0; i < gNotifCount; i++){
    if(!gNotifs[i].armed){
      gNotifs[i].armed  = true;
      gNotifs[i].phase  = NP_IN;
      gNotifs[i].bornMs = millis();
      gNotifs[i].slideX = 0.0f;
    }
  }

  // Avanzar fases de animacion
  for(int i = 0; i < gNotifCount; i++){
    Notification* n = &gNotifs[i];
    switch(n->phase){
      case NP_IN:
        if(millis() - n->bornMs >= 280) n->phase = NP_IDLE;
        break;
      case NP_IDLE:
        if(millis() - n->bornMs >= NOTIF_HOLD_MS) n->phase = NP_OUT;   // auto-descarte a los 5 s
        break;
      case NP_SPRING:
        n->slideX += (0.0f - n->slideX) * 0.35f;           // muelle de vuelta
        if(n->slideX > -0.5f){ n->slideX = 0.0f; n->phase = NP_IDLE; }
        break;
      case NP_OUT:
        n->slideX -= (NOTIF_CARD_W + NOTIF_MARGIN_X) * 0.18f + 6.0f;  // sale por la izquierda
        if(n->slideX < -(NOTIF_CARD_W + NOTIF_MARGIN_X + 4)){ notifRemove(i); i--; continue; }
        break;
      default: break;
    }
  }

  // Recorte completo (por si una app lo dejo estrecho) antes de componer
  gClipY0 = 0; gClipY1 = SCR_H - 1; gClipX0 = 0; gClipX1 = SCR_W - 1;

  // Componer en bbuf (nadie mas lo lee): restaurar fondo limpio y dibujar las
  // tarjetas encima. Nadie mas presenta esta banda -> sin parpadeo.
  setBuf(bbuf);
  notifRestoreBg();
  for(int i = 0; i < gNotifCount; i++){
    if(!gNotifs[i].active) continue;
    int cardY = NOTIF_Y0 + i * (NOTIF_CARD_H + NOTIF_GAP);
    if(gNotifs[i].phase == NP_IN){
      float p = (millis() - gNotifs[i].bornMs) / 280.0f; if(p > 1.0f) p = 1.0f;
      cardY -= (int)((1.0f - notifEaseOut(p)) * NOTIF_ENTER_DROP);   // entrada: cae desde arriba
    }
    notifDrawCard(&gNotifs[i], cardY);
  }
  // Volcado atomico bbuf->fb de una banda ya terminada. El presenter nunca ve
  // un fb a medio pintar.
  present(NOTIF_BAND_TOP, NOTIF_BAND_BOT - 1);

  // Banda vaciada: el frame de limpieza ya se compuso y volco arriba.
  if(gNotifCount == 0) notifBandOn = false;
}


// #############################################################
// ##  DETECCION DE HARDWARE I2C  (FASE 2)
// ##  ------------------------------------------------------
// ##  CLAVE DE SEGURIDAD: el escaneo corre en el MISMO contexto
// ##  que flexPollTouch() -el loop task, Core 1- llamando a
// ##  hwDetectTick() en cada vuelta. El GT911 tactil vive en el
// ##  mismo bus Wire; al no haber una segunda tarea tocando Wire,
// ##  las transacciones NUNCA se solapan y no hace falta mutex.
// ##  (Esto es a proposito lo contrario del plan original, que
// ##  ponia una tarea de escaneo en Core 1: eso compartia Wire
// ##  con el tactil sin proteccion -> corrupcion del bus/crash.)
// ##
// ##  Ademas el barrido es INCREMENTAL: sondea I2C_SCAN_PER_TICK
// ##  direcciones por vuelta, para no anadir latencia perceptible
// ##  al tactil ni forzar el watchdog. Los dispositivos nuevos
// ##  avisan por la isla dinamica de la Fase 1 (notifPush).
// ##
// ##  ALCANCE HONESTO: solo I2C, que es fiable. La deteccion de
// ##  modulos por GPIO (pulsadores, HC-SR04, servos) NO se hace
// ##  aqui porque no es distinguible sin falsos positivos; esos
// ##  llegaran por asignacion manual de pines en el asistente
// ##  (Fase 3), no por auto-deteccion.
// #############################################################

// Mapea una direccion I2C a un tipo de modulo conocido
static ModuleType identifyI2CDevice(uint8_t addr){
  switch(addr){
    case 0x76: case 0x77: return MOD_BME280;    // BME280 / BMP280
    case 0x68: case 0x69: return MOD_MPU6050;   // MPU6050 / MPU9250
    default:              return MOD_I2C_GENERIC;
  }
}

// Rellena name/sub descriptivos de un modulo I2C
static void i2cDescribe(DetectedModule* m){
  switch(m->type){
    case MOD_BME280:
      snprintf(m->name, sizeof(m->name), "Sensor BME280");
      snprintf(m->sub,  sizeof(m->sub),  "I2C 0x%02X detectado", m->i2cAddr);
      break;
    case MOD_MPU6050:
      snprintf(m->name, sizeof(m->name), "MPU6050");
      snprintf(m->sub,  sizeof(m->sub),  "IMU - I2C 0x%02X", m->i2cAddr);
      break;
    default:
      snprintf(m->name, sizeof(m->name), "Dispositivo I2C");
      snprintf(m->sub,  sizeof(m->sub),  "0x%02X detectado", m->i2cAddr);
      break;
  }
}

// ¿La direccion es la del GT911 tactil? (nunca notificar el propio panel)
static inline bool i2cIsTouch(uint8_t addr){ return addr == gtAddr || addr == 0x5D || addr == 0x14; }

// Indice de un modulo por direccion (o -1)
static int i2cFindByAddr(uint8_t addr){
  for(int i = 0; i < detectedCount; i++)
    if(detectedModules[i].i2cAddr == addr) return i;
  return -1;
}

// Marca presencia de una direccion; si es NUEVA la registra y avisa por la isla
static void i2cOnDevicePresent(uint8_t addr){
  if(i2cIsTouch(addr)) return;
  int idx = i2cFindByAddr(addr);
  if(idx >= 0){
    modSweepId[idx] = i2cSweepId;                 // sigue presente en este barrido
    if(!detectedModules[idx].active){             // reaparecio tras haberse desconectado
      detectedModules[idx].active = true;
      detectedModules[idx].detectedAt = millis();
      notifPush(&detectedModules[idx]);
    }
    return;
  }
  if(detectedCount >= MAX_MODULES_DETECTED) return;
  DetectedModule m;
  memset(&m, 0, sizeof(m));
  m.i2cAddr = addr;
  m.type    = identifyI2CDevice(addr);
  m.active  = true;
  m.numPins = 0;
  m.detectedAt = millis();
  i2cDescribe(&m);
  int slot = detectedCount++;
  detectedModules[slot] = m;
  modSweepId[slot] = i2cSweepId;
  notifPush(&detectedModules[slot]);
}

// Cierra un barrido completo: lo no visto -> inactivo (permite re-aviso al reconectar)
static void i2cEndSweep(){
  for(int i = 0; i < detectedCount; i++)
    if(detectedModules[i].active && modSweepId[i] != i2cSweepId)
      detectedModules[i].active = false;
  i2cSweepId++;
  i2cLastSweep = millis();
}

// Tick de deteccion I2C. Llamar en loop() en el mismo contexto que flexPollTouch.
static void hwDetectTick(){
  if(!gtOk) return;                                          // sin I2C inicializado, nada
  if(!i2cSweeping){
    if(millis() - i2cLastSweep < I2C_SWEEP_INTERVAL) return; // espera entre barridos
    i2cSweeping   = true;
    i2cScanCursor = I2C_SCAN_LO;
  }
  int probes = 0;
  while(i2cSweeping && probes < I2C_SCAN_PER_TICK){
    uint8_t addr = i2cScanCursor;
    if(!i2cIsTouch(addr)){
      Wire.beginTransmission(addr);
      if(Wire.endTransmission() == 0) i2cOnDevicePresent(addr);   // ACK -> hay dispositivo
    }
    probes++;
    if(i2cScanCursor >= I2C_SCAN_HI){ i2cSweeping = false; i2cEndSweep(); }
    else i2cScanCursor++;
  }
}

// Puente del modulo OTA. Va AQUI, y no arriba, a proposito: implementa
// las funciones otaHost* llamando a las primitivas graficas del sistema
// (fillRect, drawText, present, setBuf...), que son `static` y por tanto
// solo existen a partir del punto del fichero en que se definieron.
#include "FlexOS_OTA_Bridge.h"


void setup(){
  Serial.begin(115200);
  delay(60);
  Serial.println(F("\n=== FlexOS Ultra (ESP32-P4) arrancando ==="));

  // (Ya NO se toca el TWDT aqui.) La version anterior llamaba a
  // esp_task_wdt_reconfigure() en cada arranque para dar margen al
  // bring-up del panel. Era innecesario -nada en setup() bloquea mas
  // de una fraccion de segundo, y la radio ya no corre aqui- y es
  // codigo nuevo no verificado contra el estado real del TWDT en esta
  // placa, asi que se retira: menos superficie para un crash en cada
  // boot. Sigue en pie esp_task_wdt_reset() en loop() (ver mas abajo).

  // FILTRO DE ENCENDIDO desde apagado completo. Va ANTES de flexPanelInit() a
  // proposito: si el toque no se sostiene 3 s, el chip se vuelve a dormir sin
  // haber encendido nunca ni el panel ni el backlight, asi que un roce
  // accidental en el bolsillo no produce ni un destello. Ver poffWakeGate().
  poffWakeGate();

  // Panel: reintento acotado. Si de verdad no enciende (cableado DSI),
  // parpadeo del backlight como SOS -> la placa sigue viva, no muerta.
  bool panelOk = flexPanelInit();
  if(!panelOk){ delay(150); panelOk = flexPanelInit(); }
  if(!panelOk){
    Serial.println(F("[FATAL] el panel DSI no responde (revisa cableado)"));
    pinMode(PIN_LCD_BL, OUTPUT);
    for(;;){ digitalWrite(PIN_LCD_BL, HIGH); delay(150); digitalWrite(PIN_LCD_BL, LOW); delay(150); }
  }
  if(!flxGfxInit()){
    Serial.println(F("[FATAL] sin PSRAM (activa 'PSRAM: Enabled' en el IDE)"));
    for(;;) delay(1000);
  }

  flexTouchInit();       // GT911: fallo suave (si no aparece, se sigue sin tactil)
  bootInitRadioSafe();   // WiFi/C6: BYPASS -> nunca bloquea el arranque
  flexOtaBegin();        // OTA: crea la tarea de fondo (prioridad baja). No conecta ni descarga nada aqui.
  cfgLoad();

  // SISTEMA DE ARCHIVOS. Es flash, no radio: montarlo aqui NO viola la regla
  // de "setup() no toca la radio". Si falla, las pantallas que dependen de el
  // lo dicen en pantalla (fkNoFsScreen) en vez de ensenar datos inventados.
  if(!flexFsBegin()) Serial.printf("[FS] no montado: %s\n", flexFsError());
  else Serial.printf("[FS] LittleFS: %lu / %lu bytes usados\n",
                     (unsigned long)flexFsUsedBytes(), (unsigned long)flexFsTotalBytes());
  connBootRestore();              // modo avion guardado (solo lee NVS, no toca radio)
  setBacklight(gBright);          // aplica el brillo guardado
  homeOrderLoad();                // orden de iconos del Home
  // TECLADO (Fases A-D): geometria del tamano guardado, ranuras fijadas del
  // portapapeles y una comprobacion barata de que ese tamano cabe en pantalla.
  kbApplySize();
  kbMtSurfaceReset();
  clipLoadPinned();
  Serial.printf("[KB] tamano=%d (%dx%d gap=%d x=%d) cabe=%s\n",
                gKbSize, KB_KW, KB_KH, KB_GAP, KB_X, kbSizeCheck() ? "si" : "NO");

  clkBootMs = millis();
  seedMinOfDay = 13 * 60 + 23;      // siembra: sab 4 jul, 13:23 (como tus imagenes)
  clkLastMin = -1;
  clkUpdate();

  // Pantalla de diagnostico SOLO si el reinicio fue ANORMAL
  // (crash / watchdog / brownout). En encendido normal, arranque limpio.
  esp_reset_reason_t rr = esp_reset_reason();
  // Despertar de un apagado completo es un arranque NORMAL, no un crash: sin
  // esta rama la banda forense saldria en cada encendido desde deep sleep.
  // gBootCleanOff distingue "el usuario apago a proposito" de un deep sleep que
  // no salio de aqui; la bandera se consume (se borra) para que solo valga para
  // este arranque.
  bool fromDeep = (rr == ESP_RST_DEEPSLEEP);
  if(fromDeep){
    prefs.begin("flexos", false);
    gBootCleanOff = prefs.getBool("cleanoff", false);
    if(gBootCleanOff) prefs.putBool("cleanoff", false);
    prefs.end();
    Serial.printf("[PWR] arranque desde deep sleep (apagado limpio: %s)\n", gBootCleanOff ? "si" : "no");
  }
  bool abnormal = !(rr == ESP_RST_POWERON || rr == ESP_RST_SW || fromDeep);
  if(abnormal) showBootBanner();

  // Fondo NEGRO ABSOLUTO para el splash (como un movil comercial)
  setBuf(fb);
  fillRect(0, 0, SCR_W, SCR_H, rgb565(0,0,0));
  flxFlushAll();

  splashStart = millis();
  gState = ST_SPLASH;
}

// Bucle de animacion continuo de la UI — corre pase lo que pase, no solo
// cuando hay un tap. Compone off-screen (anti-flicker) via cada render.
static unsigned long uiAnimMs = 0;
static void uiTick(){
  // El ripple del icono es una animacion puramente TEMPORAL (su posicion es
  // funcion de millis(), no un paso fijo por frame) y, tras el repintado
  // parcial de animateIconRipple, cuesta muy poco por frame. Por eso ESA ruta
  // se refresca a ~60 fps: restaura exactamente la franja que dibuja, asi que
  // es correcta por construccion y muy barata. El resto (edicion, cortina)
  // conserva su cadencia de ~26 fps -- algunas llevan pasos por-frame (p.ej.
  // el resorte de iconos en Modo Edicion) y acelerarlas cambiaria su
  // VELOCIDAD, no solo su suavidad; por eso no se tocan.
  // LA CORTINA SI PASA A LA VIA RAPIDA. Ya no tiene ningun paso por-cuadro: su
  // posicion sale del dedo (con suavizado por constante de TIEMPO) o del reloj
  // (qsAnimTo), y el destello de sus botones tambien es funcion de millis().
  // Subir su cadencia a ~60 fps solo la hace mas suave, no mas rapida -- y a 26
  // fps un gesto rapido avanzaba tanto entre cuadro y cuadro que el movimiento
  // se veia a saltos.
  bool fastPath = (gState == ST_HOME && !editMode && (gRippleActive || qsPanelY > 0));
  unsigned long interval = fastPath ? 16 : 38;
  if(millis() - uiAnimMs < interval) return;
  uiAnimMs = millis();
  if(gState == ST_HOME){
    if(qsPanelY > 0) qsRender();                // cortina visible: anima el destello de toque (incluso durante el drag)
    else if(editMode) edRender();               // jiggle continuo
    else if(gRippleActive) animateIconRipple(); // destello del icono tocado (Vidrio), ~0.5s
  }
}

void loop(){
  flexFeedWdt();          // alimenta el TWDT solo si loopTask sigue suscrito (ver arriba)
  flexPollTouch();        // (aqui dentro corre tambien el detector de doble-tap de la suspension)
  suspFadeTick();         // SUSPENSION/APAGADO: un paso del fundido de backlight (no bloqueante)
  autoLockTick();         // FASE 1: bloqueo por inactividad (lee T sin filtrar, antes de que nadie consuma el toque)
  notifHandleTouch();     // la isla intercepta toques dentro de sus tarjetas (Fase 1)
  flexOtaTouchBridge();   // OTA: si hay overlay visible, se queda el toque antes que nadie
  hwDetectTick();         // deteccion I2C incremental, mismo contexto que el tactil (Fase 2)
  wifiAutoReconnectTick();// reconexion WiFi diferida (la radio NUNCA se toca en setup(); ver bootInitRadioSafe)
  bool minChanged = clkUpdate();
  gMinChanged = minChanged;

  // -----------------------------------------------------------
  //  PANTALLA EN EXCLUSIVA PARA EL OTA
  //  ---------------------------------------------------------
  //  Con una capa OTA a pantalla completa (changelog, progreso o
  //  ajustes de actualizacion), NINGUN otro subsistema dibuja.
  //
  //  Por que: el overlay no cambia gState -- durante una descarga
  //  seguimos en ST_HOME. Sin este corte, en cada vuelta se
  //  ejecutaban igualmente homeTick(), kioskTick(), uiTick() y
  //  notifTick(), y todos ellos componen en el MISMO bbuf y
  //  publican su banda con present(). Entre dos repintados del OTA
  //  se colaba una banda con el fondo del escritorio -- un degradado
  //  azul/verde -- justo encima de la pantalla de progreso: ese era
  //  el "parpadeo cian" en cada 1%. No era una carrera entre
  //  nucleos (el presenter es el unico que habla con el panel y ya
  //  estaba protegido por flxFbMux): era que nadie tenia la
  //  propiedad exclusiva de la pantalla.
  //
  //  El tactil, el TWDT, la deteccion I2C y la radio SIGUEN
  //  corriendo arriba: aqui solo se corta el DIBUJO.
  // -----------------------------------------------------------
  if(flexOtaOwnsScreen()){
    flexOtaRender();
    delay(5);
    return;
  }

  switch(gState){
    case ST_SPLASH:    splashTick(); break;
    case ST_OOBE_LANG: oobeLangTick(); break;
    case ST_OOBE_NAME: oobeNameTick(); break;
    case ST_LOCK:
      if(minChanged){ renderLock(); if(lockOff == 0) showLock(); }
      lockTick();
      break;
    case ST_HOME:
      if(minChanged && qsPanelY == 0 && !editMode){
        renderHome();                    // refresca el cache homeBuf (offscreen: setBuf(homeBuf)...setBuf(fb), sin tocar pantalla)
        showHome();
      }
      if(editMode || !qsHandle()) homeTick();     // en edicion, saltar la cortina
      break;
    case ST_APP:       appTick(); break;
    case ST_SWITCHER:  swTick(); break;
    case ST_LOCKSETUP: lsuTick(); break;
    case ST_WIFI:      wifiTick(); break;
    case ST_CTX:       ctxTick(); break;        // FASE 2: menu contextual de long-press
    case ST_KIOSKSET:  kioskSetTick(); break;   // FASE 4: definir el area excluida
    case ST_POWEROFF_CONFIRM: poffTick(); break;    // APAGADO: slider "desliza para apagar"
    case ST_POWEROFF_ANIM:    poffAnimTick(); break;// APAGADO: animacion final (no vuelve)
    case ST_KBSET:            kbsTick(); break;     // FASE E: Ajustes del teclado
    case ST_CONN:             connTick(); break;    // Conectividad: Wifi / BLE / Modo avion
    case ST_FILES:            filesTick(); break;   // Explorador de archivos real
  }
  kioskTick();            // FASE 4: refresca el candado y escucha el gesto de salida
  uiTick();               // animacion continua del vidrio
  notifTick();            // isla dinamica: anima y compone sobre la pantalla activa (Fase 1)
  flexOtaRender();        // OTA: ULTIMA capa del pipeline grafico (nunca toca el fb de una app)
  delay(5);
}

// #############################################################
// ##  HOJA DE RUTA  (lo que llega despues del Milestone 1)
// #############################################################
//
//  Milestone 1 (ESTE archivo) — COMPLETO:
//    · Capa HW nativa 480x800 (panel ST7701 + GT911) reusada de ArduOS
//    · Motor grafico propio (framebuffers PSRAM + presenter core 0)
//    · Fuente 5x7 con acentos UTF-8 (es/fr/pt/it) + reloj vectorial
//    · Splash con fundido · OOBE (6 idiomas + teclado QWERTY)
//    · Bloqueo con reloj gigante y desbloqueo con fisica (swipe-up)
//    · Escritorio: barra de estado, 2 widgets, rejilla 4x3, dock, nav
//    · Banda forense de reinicio (depuracion sin PC)
//
//  Milestone 2 — Framework de apps + apps reales:
//    [HECHO] Sistema de ventanas: apertura/cierre animado desde el icono,
//            marco estandar (estado + cabecera "atras" + nav), registro
//            APP_REG enchufable, gestos de cierre. App de referencia: Reloj.
//    [PENDIENTE] Rellenar el resto (reemplazar entradas de APP_REG):
//      Galeria, Multimedia, Almacenamiento, Modo PC, Notas, Educacion,
//      Navegador, Code IDE, Bienestar, Paint, Juegos, Calculadora,
//      Calendario, Camara.
//
//  Milestone 3 — Ajustes (imagen 3): [HECHO] dos paneles (barra lateral
//    de 12 categorias + panel de detalle con scroll). General y Acerca de
//    con datos reales del dispositivo; resto con filas representativas.
//    Motor: se anadio recorte vertical (clip) para listas con scroll.
//
//  Milestone 4 — Modo PC estilo Windows 11 (imagen 4): barra de
//    tareas, ventanas flotantes, escritorio horizontal.
//
//  Pendientes de plataforma (cuando toque):
//    · Fuente CJK (archivo de fuente en SPIFFS) para chino real.
//    · WiFi/NTP: hoy OFF por la inestabilidad del co-procesador C6
//      (esp-hosted). Reactivar tras actualizar su firmware/core.
//    · Bateria real por ADC y brillo por PWM del backlight.
//    · [HECHO] Almacenamiento REAL en LittleFS (FlexOS_FS.h/.cpp):
//      carpetas /Paint, /Notas, /System, /Documentos y /Papelera,
//      con Paint, Notas, Almacenamiento y el Explorador de archivos
//      operando sobre ficheros de verdad.
//    · Pendiente: tarjeta SD como volumen adicional y fondos de
//      pantalla cargados desde fichero.
// #############################################################
