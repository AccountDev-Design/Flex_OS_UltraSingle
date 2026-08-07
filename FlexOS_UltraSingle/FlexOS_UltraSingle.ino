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
//     sistema es una linea de barrido de 640 B (seccion FOS_GFX).
//   · Sin FreeRTOS ni multitarea. Un solo bucle cooperativo y una
//     sola aplicacion en primer plano (seccion FOS_APPS).
//   · Sin radio. Se elimino por completo Wi-Fi, Bluetooth Classic,
//     BLE, OTA, el navegador, la camara, la tienda, los juegos en
//     red y cualquier servicio en segundo plano: en esta placa no
//     existe el hardware que los sostenia.
//   · Sin DeX / Modo PC ni ventanas flotantes.
//   · Sin LittleFS ni NVS: la persistencia es un mapa fijo sobre la
//     EEPROM interna de 4 KB (seccion FOS_STORE).
//   · Sin String, sin new/malloc y sin coma flotante fuera de la
//     Calculadora. Todos los textos y tablas viven en PROGMEM.
//
//  HARDWARE (fijo, definido en la seccion FOS_CONFIG)
//  --------------------------------------------------
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
//  MAPA DEL ARCHIVO UNICO
//  ----------------------
//    FOS_CONFIG   hardware, geometria y constantes
//    FOS_THEME    paleta heredada de Flex OS Ultra
//    FOS_GFX      motor grafico directo (wallpaper, vidrio, texto)
//    FOS_TOUCH    tactil resistivo y gestos
//    FOS_STR      textos en PROGMEM (espanol / ingles)
//    FOS_TIME     reloj interno sin RTC
//    FOS_STORE    persistencia en EEPROM
//    FOS_ICONS    iconos vectoriales
//    FOS_UI       cromo del sistema y controles comunes
//    FOS_FX       transiciones
//    FOS_HOME     escritorio y panel rapido
//    FOS_LOCK     bloqueo y PIN
//    FOS_SHELL    arranque, OOBE y bucle principal
//    FOS_APPS     registro y codigo de las doce aplicaciones
// #############################################################


// #############################################################
// ##  EDICION UNIFICADA
// ##  ------------------------------------------------------
// ##  Todo el codigo propio de Flex OS UltraSingle vive en este
// ##  unico archivo INO. Solo siguen siendo externas las librerias
// ##  instalables desde Arduino IDE.
// #############################################################

#include <Arduino.h>
#include <MCUFRIEND_kbv.h>
#include <TouchScreen.h>
#include <EEPROM.h>
#include <string.h>
#include <stdlib.h>

// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_config.h
// =============================================================
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


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_theme.h
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_theme.h
// ##  Paleta del sistema (heredada de Flex OS Ultra)
// #############################################################
//
//  Los valores son EXACTAMENTE los de Flex OS Ultra: la identidad
//  visual no se toca al bajar de ESP32-P4 a Arduino Mega, solo la
//  forma de pintarlos. Todo son constantes de compilacion (RGB565
//  ya resuelto por el preprocesador), asi que no ocupan SRAM.
// #############################################################


// RGB888 -> RGB565 en tiempo de compilacion.
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Modo de apariencia. Vive en fos_gfx.cpp y lo cambia Ajustes.
extern bool gDark;

// ---------------- Wallpaper (degradado diagonal de 3 paradas) ----------------
#define WALL_GREEN   RGB(80, 224, 74)    // arriba-derecha
#define WALL_BLUE    RGB(40, 150, 245)   // centro
#define WALL_PURPLE  RGB(112, 46, 230)   // abajo-izquierda

// ---------------- Neutros ----------------
#define C_WHITE      RGB(255, 255, 255)
#define C_BLACK      RGB(0, 0, 0)
#define C_INK        RGB(20, 22, 30)

// Texto sobre el wallpaper (Inicio / Bloqueo): siempre claro.
#define C_ON_WALL    RGB(255, 255, 255)
#define C_ON_WALL_LO RGB(215, 224, 240)

// ---------------- Acento del sistema ----------------
#define C_ACCENT     RGB(60, 130, 246)
#define C_ACCENT_DK  RGB(28, 58, 120)
#define C_OK         RGB(80, 200, 120)
#define C_WARN       RGB(240, 170, 60)
#define C_DANGER     RGB(230, 80, 80)

// ---------------- Superficies de app (Ajustes, listas, ventanas) ----------------
#define PAGE_BG      (gDark ? RGB(18, 20, 28)    : RGB(244, 247, 251))
#define CARD_BG      (gDark ? RGB(34, 38, 50)    : RGB(255, 255, 255))
#define CARD_ALT     (gDark ? RGB(48, 54, 72)    : RGB(238, 242, 248))
#define TXT_HI       (gDark ? RGB(240, 242, 248) : RGB(20, 22, 30))
#define TXT_LO       (gDark ? RGB(160, 166, 182) : RGB(120, 126, 140))
#define TXT_MUTE     (gDark ? RGB(120, 126, 142) : RGB(140, 146, 160))
#define CHEVRON      (gDark ? RGB(110, 116, 132) : RGB(160, 165, 178))
#define BORDER       (gDark ? RGB(66, 74, 94)    : RGB(202, 210, 224))

// ---------------- Barras del sistema ----------------
#define BAR_TXT      RGB(255, 255, 255)
#define NAV_TINT     RGB(255, 255, 255)

// ---------------- Vidrio (Liquid Glass) ----------------
// Tintes de los paneles translucidos, tal cual en Flex OS Ultra.
#define GLASS_CLOCK  RGB(40, 62, 128)
#define GLASS_CARD   RGB(40, 50, 90)
#define GLASS_DOCK   RGB(180, 186, 206)
#define GLASS_ICON   RGB(120, 140, 200)
#define GLASS_A      170   // opacidad base del tinte (0..255)


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_touch.h
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_touch.h
// ##  Tactil resistivo del shield MAR3501 + gestos
// #############################################################


// Estado tactil de alto nivel. Mismo contrato que el struct Touch de
// Flex OS Ultra, para que la logica de las pantallas sea la misma.
//   down      -> hay un dedo en la pantalla AHORA
//   pressed   -> flanco de bajada (este frame empezo el toque)
//   released  -> flanco de subida
//   tap       -> toque corto y sin arrastre (se evalua en released)
//   longPress -> se mantuvo pulsado mas de GEST_LONG_MS sin moverse
//   swipe*    -> deslizamiento reconocido (se evalua en released)
struct FTouch {
  bool     down, pressed, released, tap, moved, longPress;
  bool     swipeUp, swipeDown, swipeLeft, swipeRight;
  int16_t  x, y, startX, startY, dx, dy;
  uint32_t downMs;
};

extern FTouch tp;

void touchInit();
void touchPoll();
// Descarta el gesto en curso (tras cambiar de pantalla, para que el mismo
// toque no active tambien un control de la pantalla nueva).
void touchConsume();

// Instante del ultimo contacto: lo usa el bloqueo automatico.
uint32_t touchLastActivity();

// Impacto de un punto contra un rectangulo.
bool hitRect(int16_t px, int16_t py, int16_t x, int16_t y, int16_t w, int16_t h);


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_gfx.h
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_gfx.h
// ##  Motor grafico (dibujo DIRECTO sobre el ILI9486)
// #############################################################
//
//  DIFERENCIA CLAVE CON FLEX OS ULTRA
//  ----------------------------------
//  En el ESP32-P4 habia framebuffers completos en PSRAM (768 KB
//  cada uno) y un "presenter" que subia las filas sucias al panel.
//  En el Mega no hay memoria para un solo pixel de mas: 320x480x2
//  son 300 KB y aqui hay 8 KB de SRAM en total.
//
//  Por eso todo se dibuja DIRECTAMENTE en la GRAM del ILI9486. El
//  unico buffer del sistema es una LINEA de barrido (640 B) que se
//  usa para los degradados y los paneles de vidrio; sin ella habria
//  que hacer una transaccion de bus por pixel y el sistema se
//  arrastraria.
//
//  TRANSLUCIDEZ SIN LEER LA PANTALLA
//  ---------------------------------
//  El wallpaper de Flex OS Ultra es un degradado analitico: el color
//  de cualquier pixel se puede CALCULAR con wallAt(x, y) en vez de
//  leerlo del panel (leer la GRAM del ILI9486 es lentisimo). Como el
//  desenfoque de un degradado lineal es el propio degradado, el
//  "Liquid Glass" se reduce a mezclar el tinte con wallAt(): mismo
//  resultado en pantalla, coste cero en memoria.
// #############################################################


extern MCUFRIEND_kbv tft;

// ---------------- Ciclo de vida ----------------
bool gfxInit();

// ---------------- Color ----------------
uint16_t mix565(uint16_t a, uint16_t b, uint8_t t);
uint8_t  isqrt16(uint16_t v);

// ---------------- Wallpaper ----------------
uint16_t wallAt(int16_t x, int16_t y);
void     gfxWallpaper();                                   // todo el fondo
void     gfxWallRect(int16_t x, int16_t y, int16_t w, int16_t h);   // solo esa zona

// ---------------- Superficies ----------------
// Panel translucido SOBRE el wallpaper (Liquid Glass).
void glassRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
               uint16_t tint, uint8_t alpha);
// Panel translucido sobre un color plano conocido (dentro de una app).
void tintRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
              uint16_t tint, uint8_t alpha, uint16_t bg);

// ---------------- Texto (fuente 5x7 clasica, escalable) ----------------
#define TXT_ADV(sz)  (6 * (sz))
#define TXT_H(sz)    (7 * (sz))

int  gfxTextW(const char* s, uint8_t size);
int  gfxTextWP(PGM_P s, uint8_t size);
int  gfxText(int16_t x, int16_t y, const char* s, uint8_t size, uint16_t col);
int  gfxTextP(int16_t x, int16_t y, PGM_P s, uint8_t size, uint16_t col);
void gfxTextC(int16_t cx, int16_t y, const char* s, uint8_t size, uint16_t col);
void gfxTextCP(int16_t cx, int16_t y, PGM_P s, uint8_t size, uint16_t col);
void gfxTextR(int16_t rx, int16_t y, const char* s, uint8_t size, uint16_t col);

// ---------------- Utilidades de trazo ----------------
void gfxThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t tk, uint16_t col);
void gfxRing(int16_t cx, int16_t cy, int16_t r, uint8_t tk, uint16_t col);
void gfxChevron(int16_t x, int16_t y, uint8_t s, uint16_t col);   // ">" de lista

// Reloj grande vectorial (7 segmentos redondeados) de la pantalla de bloqueo.
// 'txt' admite digitos y ':'. 'dw' = ancho de digito, 'tk' = grosor de trazo.
void gfxBigClock(const char* txt, int16_t cx, int16_t cy, uint8_t dw, uint8_t tk, uint16_t col);
int  gfxBigClockW(const char* txt, uint8_t dw);

// ---------------- Numeros sin String ----------------
// Formateadores minimos: el sistema NUNCA usa String ni sprintf de coma
// flotante (cada uno arrastra kilobytes de Flash en AVR).
char* fmtU16(char* dst, uint16_t v);                       // devuelve fin
char* fmtPad2(char* dst, uint8_t v);                       // "07"
void  fmtClock(char* dst, uint8_t h, uint8_t m, bool h24); // "07:05" / "7:05"


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_str.h
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_str.h
// ##  Textos del sistema (todos en PROGMEM)
// #############################################################
//
//  Flex OS Ultra llevaba 5 idiomas. En 256 KB de Flash eso son
//  kilobytes que hacen falta para las apps, asi que UltraSingle
//  conserva los dos idiomas del selector inicial (espanol e ingles)
//  con la misma pantalla de eleccion y el mismo ajuste.
//
//  NINGUNA cadena vive en SRAM: la tabla es const __FlashStringHelper
//  y se lee con pgm_read_word. La fuente es CP437, asi que los
//  acentos se escriben con su codigo (\xA0 = a acentuada, etc.).
// #############################################################


enum { LANG_ES = 0, LANG_EN, LANG_N };
extern uint8_t gLang;

enum {
  S_CONTINUE = 0, S_LANGUAGE, S_WELCOME, S_SWIPEUNLOCK, S_WEATHER, S_NEWS,
  S_NONEWS, S_NOEVENTS, S_SETTINGS, S_DISPLAY, S_DARKMODE, S_GLASS,
  S_ICONSTYLE, S_LOCKSCREEN, S_PIN, S_AUTOLOCK, S_DATETIME, S_H24, S_ABOUT,
  S_ON, S_OFF, S_BACK, S_SAVE, S_CLEAR, S_DELETE, S_OK, S_CANCEL,
  S_ENTERPIN, S_WRONGPIN, S_SETPIN, S_NOPIN, S_FLAT, S_GLASSY, S_FREE,
  S_USED, S_UPTIME, S_TOUCHES, S_RAM, S_FLASH, S_EEPROM, S_BOARD, S_PANEL,
  S_VERSION, S_EMPTY, S_COLOR, S_BRUSH, S_NEWGAME, S_SCORE, S_BEST,
  S_GAMEOVER, S_TAPSTART, S_START, S_STOP, S_RESET, S_HOUR, S_MINUTE,
  S_DAY, S_MONTH, S_YEAR, S_APPS, S_SYSTEM, S_NEVER, S_TOTAL, S_NOTEHINT,
  S_N
};

// Puntero PROGMEM a la cadena 'id' en el idioma activo.
PGM_P strP(uint8_t id);
// Nombre de la aplicacion 'app' en el idioma activo.
PGM_P appNameP(uint8_t app);
// Dias y meses localizados.
PGM_P dayShortP(uint8_t d);      // 0 = domingo
PGM_P dayFullP(uint8_t d);
PGM_P monthShortP(uint8_t m);    // 0 = enero
PGM_P monthFullP(uint8_t m);


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_time.h
// =============================================================
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


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_store.h
// =============================================================
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


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_apps.h
// =============================================================
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


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_icons.h
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_icons.h
// ##  Iconos vectoriales del sistema
// #############################################################
//
//  Los mismos iconos de Flex OS Ultra, redibujados con aritmetica
//  ENTERA: la version P4 usaba cosf/sinf y flotantes en cada
//  icono, y en AVR eso arrastra la biblioteca matematica entera
//  (varios KB de Flash) ademas de ser lentisimo. Las posiciones
//  circulares salen ahora de una tabla de 8 direcciones en PROGMEM.
// #############################################################


// Icono de aplicacion completo (fondo + simbolo), lado S.
void drawAppIcon(uint8_t id, int16_t x, int16_t y, uint8_t S);

// Iconos de la barra de estado / navegacion.
void drawBattery(int16_t x, int16_t y, uint8_t w, uint8_t h, uint8_t pct, uint16_t col);
void drawSignal(int16_t x, int16_t yBase, uint8_t h, uint16_t col);
void drawNavBar(int16_t y, uint16_t col);
void drawHomeIndicator(int16_t yBottom, uint16_t col);


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_ui.h
// =============================================================
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


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_fx.h
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_fx.h
// ##  Transiciones del sistema
// #############################################################
//
//  Flex OS Ultra animaba componiendo framebuffers en PSRAM (el
//  desbloqueo con fisica, el zoom de apertura de app, la cortina).
//  Sin memoria para eso, UltraSingle conserva la SENSACION con
//  transiciones que se dibujan directamente sobre la GRAM: un
//  barrido de wallpaper y una apertura por escalado de rectangulo.
//  Son pocos pasos a proposito -- el bus de 8 bits del Mega no da
//  para mas -- pero el sistema deja de "saltar" entre pantallas.
// #############################################################


// Barrido de wallpaper: limpia la pantalla en bandas. up = de abajo a arriba.
void fxWipe(bool up);
// Apertura de aplicacion: un rectangulo redondeado crece desde el icono
// hasta ocupar la pantalla, como en Flex OS Ultra.
void fxOpenApp(int16_t ix, int16_t iy, uint8_t is);
// Cierre: el fondo vuelve desde los bordes hacia el centro.
void fxCloseApp();


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_home.h
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_home.h
// ##  Escritorio (widgets + rejilla + dock + panel rapido)
// #############################################################


void homeEnter();     // pintado completo
void homeTick();      // entrada + repintado de las zonas que cambian
void homeInvalidate(); // un ajuste cambio el aspecto: repintar al volver


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_lock.h
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_lock.h
// ##  Pantalla de bloqueo y verificacion por PIN
// #############################################################


void lockEnter();
void lockTick();

void authEnter(uint8_t reason);
void authTick();

// Bloqueo automatico: milisegundos de inactividad del ajuste activo
// (0 = nunca).
uint32_t autoLockMs();


// =============================================================
// DECLARACIONES INTEGRADAS DESDE fos_shell.h
// =============================================================
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


// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_gfx.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_gfx.cpp
// ##  Motor grafico directo sobre ILI9486 (8 bits paralelo)
// #############################################################

MCUFRIEND_kbv tft;
bool gDark = true;                       // Modo oscuro por defecto (como Flex OS Ultra)

// UNICO buffer grande del sistema: una linea de barrido. Se reutiliza para el
// wallpaper y para todos los paneles de vidrio, asi que el coste es fijo (640 B)
// y no crece con el numero de superficies en pantalla.
static uint16_t sline[SCR_W];

// -------------------------------------------------------------
//  Color
// -------------------------------------------------------------
// Division por 255 sin instruccion de division (el ATmega no tiene divisor
// hardware): v/255 == (v + 1 + (v>>8)) >> 8 para v en [0, 65534].
#define DIV255(v) (uint16_t)(((v) + 1u + ((v) >> 8)) >> 8)

uint16_t mix565(uint16_t a, uint16_t b, uint8_t t){
  uint16_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  uint16_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  uint16_t it = 255u - t;
  return (uint16_t)((DIV255((uint16_t)(ar * it + br * t)) << 11) |
                    (DIV255((uint16_t)(ag * it + bg * t)) << 5)  |
                     DIV255((uint16_t)(ab * it + bb * t)));
}

uint8_t isqrt16(uint16_t v){
  uint32_t op = v, res = 0, one = 1UL << 16;
  while(one > op) one >>= 2;
  while(one){
    if(op >= res + one){ op -= res + one; res += one << 1; }
    res >>= 1; one >>= 2;
  }
  return (uint8_t)res;
}

// -------------------------------------------------------------
//  Wallpaper
// -------------------------------------------------------------
//  Degradado diagonal de 3 paradas identico al de Flex OS Ultra:
//  violeta (abajo-izquierda) -> azul (centro) -> verde (arriba-derecha).
static inline uint16_t gradAt(uint8_t t){
  return (t < 128) ? mix565(WALL_PURPLE, WALL_BLUE,  (uint8_t)(t << 1))
                   : mix565(WALL_BLUE,   WALL_GREEN, (uint8_t)((t - 128) << 1));
}

// Paso del degradado horizontal en punto fijo 8.8: el indice avanza
// 255/(SCR_W-1) por pixel. Se acumula en 16 bits en vez de dividir en cada
// punto -- 320*WALL_STEP no llega a desbordar -- y de paso evita el error
// real que tenia la version directa: 319*255 se sale de un uint16_t.
#define WALL_STEP ((uint16_t)((255UL << 8) / (SCR_W - 1)))

uint16_t wallAt(int16_t x, int16_t y){
  uint8_t tx = (uint8_t)(((uint32_t)x * 255UL) / (SCR_W - 1));
  uint8_t ty = (uint8_t)(((uint32_t)(SCR_H - 1 - y) * 255UL) / (SCR_H - 1));
  return gradAt((uint8_t)(((uint16_t)tx + ty) >> 1));
}

// Rellena sline[0..w-1] con la fila 'y' del wallpaper a partir de x0.
// Los 320 pixeles de una fila solo contienen ~128 colores distintos (el indice
// del degradado avanza 1 cada 2,5 px), asi que se detecta la RACHA y mix565
// se llama una de cada dos o tres veces en vez de una por pixel.
static void wallRow(int16_t y, int16_t x0, int16_t w){
  uint16_t ty  = (uint16_t)(((uint32_t)(SCR_H - 1 - y) * 255UL) / (SCR_H - 1));
  uint16_t txq = (uint16_t)x0 * WALL_STEP;
  int16_t  last = -1;
  uint16_t c = 0;
  for(int16_t i = 0; i < w; i++){
    uint8_t t = (uint8_t)(((txq >> 8) + ty) >> 1);
    txq += WALL_STEP;
    if(t != last){ last = t; c = gradAt(t); }
    sline[i] = c;
  }
}

// Recorta un rectangulo a la pantalla. Devuelve false si queda vacio.
static bool clipRect(int16_t &x, int16_t &y, int16_t &w, int16_t &h){
  if(x < 0){ w += x; x = 0; }
  if(y < 0){ h += y; y = 0; }
  if(x + w > SCR_W) w = SCR_W - x;
  if(y + h > SCR_H) h = SCR_H - y;
  return (w > 0 && h > 0);
}

void gfxWallRect(int16_t x, int16_t y, int16_t w, int16_t h){
  if(!clipRect(x, y, w, h)) return;
  tft.setAddrWindow(x, y, x + w - 1, y + h - 1);
  for(int16_t j = 0; j < h; j++){
    wallRow(y + j, x, w);
    tft.pushColors(sline, w, j == 0);
  }
}

void gfxWallpaper(){ gfxWallRect(0, 0, SCR_W, SCR_H); }

// -------------------------------------------------------------
//  Superficies translucidas
// -------------------------------------------------------------
// Sangrado horizontal de la fila 'j' de un rectangulo redondeado.
static uint8_t rrInset(int16_t j, int16_t h, int16_t r){
  int16_t dy;
  if(j < r)            dy = r - 1 - j;
  else if(j >= h - r)  dy = j - (h - r);
  else                 return 0;
  uint16_t rr = (uint16_t)r * r, dd = (uint16_t)dy * dy;
  return (uint8_t)(r - isqrt16(rr > dd ? (uint16_t)(rr - dd) : 0));
}

static void surface(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                    uint16_t tint, uint8_t alpha, bool overWall, uint16_t bg){
  if(!clipRect(x, y, w, h)) return;
  if(2 * r > w) r = w / 2;
  if(2 * r > h) r = h / 2;
  if(r < 0) r = 0;
  tft.setAddrWindow(x, y, x + w - 1, y + h - 1);
  for(int16_t j = 0; j < h; j++){
    if(overWall) wallRow(y + j, x, w);
    else         for(int16_t i = 0; i < w; i++) sline[i] = bg;
    uint8_t ins = rrInset(j, h, r);
    // Un panel de vidrio no es un tinte plano: Flex OS Ultra le da un
    // gradiente de grosor (mas denso arriba). Se reproduce variando alpha
    // un 15% a lo largo del alto, que es lo que se percibe en pantalla.
    uint16_t a = (uint16_t)alpha - (uint16_t)((uint32_t)alpha * 15 * j) / (100u * (h ? h : 1));
    for(int16_t i = ins; i < w - ins; i++) sline[i] = mix565(sline[i], tint, (uint8_t)a);
    tft.pushColors(sline, w, j == 0);
  }
}

void glassRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
               uint16_t tint, uint8_t alpha){
  surface(x, y, w, h, r, tint, alpha, true, 0);
}

void tintRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
              uint16_t tint, uint8_t alpha, uint16_t bg){
  surface(x, y, w, h, r, tint, alpha, false, bg);
}

// -------------------------------------------------------------
//  Texto
// -------------------------------------------------------------
int gfxTextW(const char* s, uint8_t size){
  uint8_t n = (uint8_t)strlen(s);
  return n ? (int)n * TXT_ADV(size) - size : 0;
}
int gfxTextWP(PGM_P s, uint8_t size){
  uint8_t n = (uint8_t)strlen_P(s);
  return n ? (int)n * TXT_ADV(size) - size : 0;
}

int gfxText(int16_t x, int16_t y, const char* s, uint8_t size, uint16_t col){
  tft.setTextColor(col);
  tft.setTextSize(size);
  tft.setCursor(x, y);
  tft.print(s);
  return x + gfxTextW(s, size);
}
int gfxTextP(int16_t x, int16_t y, PGM_P s, uint8_t size, uint16_t col){
  tft.setTextColor(col);
  tft.setTextSize(size);
  tft.setCursor(x, y);
  tft.print((const __FlashStringHelper*)s);
  return x + gfxTextWP(s, size);
}
void gfxTextC(int16_t cx, int16_t y, const char* s, uint8_t size, uint16_t col){
  gfxText(cx - gfxTextW(s, size) / 2, y, s, size, col);
}
void gfxTextCP(int16_t cx, int16_t y, PGM_P s, uint8_t size, uint16_t col){
  gfxTextP(cx - gfxTextWP(s, size) / 2, y, s, size, col);
}
void gfxTextR(int16_t rx, int16_t y, const char* s, uint8_t size, uint16_t col){
  gfxText(rx - gfxTextW(s, size), y, s, size, col);
}

// -------------------------------------------------------------
//  Trazos
// -------------------------------------------------------------
void gfxThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t tk, uint16_t col){
  if(tk < 1) tk = 1;
  int16_t dx = x1 - x0, dy = y1 - y0;
  // Desplaza el trazo perpendicularmente: para lineas mas horizontales que
  // verticales se apila en Y, y al reves. Evita trigonometria en coma flotante.
  bool horiz = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
  int8_t half = (int8_t)(tk / 2);
  for(int8_t k = -half; k <= half - (int8_t)((tk & 1) ? 0 : 1); k++){
    if(horiz) tft.drawLine(x0, y0 + k, x1, y1 + k, col);
    else      tft.drawLine(x0 + k, y0, x1 + k, y1, col);
  }
}

void gfxRing(int16_t cx, int16_t cy, int16_t r, uint8_t tk, uint16_t col){
  for(uint8_t k = 0; k < tk; k++) tft.drawCircle(cx, cy, r - k, col);
}

void gfxChevron(int16_t x, int16_t y, uint8_t s, uint16_t col){
  gfxThickLine(x, y - s, x + s, y, 2, col);
  gfxThickLine(x + s, y, x, y + s, 2, col);
}

// -------------------------------------------------------------
//  Reloj grande vectorial (7 segmentos con extremos redondeados)
// -------------------------------------------------------------
//  bit0=A(arriba) 1=B(sup-dcha) 2=C(inf-dcha) 3=D(abajo)
//  bit4=E(inf-izq) 5=F(sup-izq)  6=G(centro)
static const uint8_t SEG7[10] PROGMEM = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static void seg7(int16_t x, int16_t y, uint8_t dw, uint8_t tk, uint8_t mask, uint16_t col){
  int16_t dh = dw * 2, hh = dh / 2, hr = tk / 2;
  int16_t bw = dw - tk, bh = hh - tk;
  if(bw < 1) bw = 1;
  if(bh < 1) bh = 1;
  if(mask & 0x01) tft.fillRoundRect(x + hr,          y,               bw, tk, hr, col);
  if(mask & 0x40) tft.fillRoundRect(x + hr,          y + hh - hr,     bw, tk, hr, col);
  if(mask & 0x08) tft.fillRoundRect(x + hr,          y + dh - tk,     bw, tk, hr, col);
  if(mask & 0x20) tft.fillRoundRect(x,               y + hr,          tk, bh, hr, col);
  if(mask & 0x02) tft.fillRoundRect(x + dw - tk,     y + hr,          tk, bh, hr, col);
  if(mask & 0x10) tft.fillRoundRect(x,               y + hh,          tk, bh, hr, col);
  if(mask & 0x04) tft.fillRoundRect(x + dw - tk,     y + hh,          tk, bh, hr, col);
}

int gfxBigClockW(const char* txt, uint8_t dw){
  int w = 0;
  for(const char* p = txt; *p; p++) w += (*p == ':') ? (dw / 2 + 4) : (dw + 6);
  return w > 0 ? w - 6 : 0;
}

void gfxBigClock(const char* txt, int16_t cx, int16_t cy, uint8_t dw, uint8_t tk, uint16_t col){
  int16_t dh = dw * 2;
  int16_t x  = cx - gfxBigClockW(txt, dw) / 2;
  int16_t y  = cy - dh / 2;
  for(const char* p = txt; *p; p++){
    if(*p == ':'){
      int16_t r = tk / 2;
      tft.fillCircle(x + dw / 4, y + dh / 3,     r, col);
      tft.fillCircle(x + dw / 4, y + 2 * dh / 3, r, col);
      x += dw / 2 + 4;
    } else if(*p >= '0' && *p <= '9'){
      seg7(x, y, dw, tk, pgm_read_byte(&SEG7[*p - '0']), col);
      x += dw + 6;
    } else {
      x += dw / 2 + 4;
    }
  }
}

// -------------------------------------------------------------
//  Formateo numerico (sin String, sin printf flotante)
// -------------------------------------------------------------
char* fmtU16(char* dst, uint16_t v){
  char tmp[6];
  uint8_t n = 0;
  do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while(v);
  while(n) *dst++ = tmp[--n];
  *dst = 0;
  return dst;
}

char* fmtPad2(char* dst, uint8_t v){
  dst[0] = (char)('0' + (v / 10) % 10);
  dst[1] = (char)('0' + (v % 10));
  dst[2] = 0;
  return dst + 2;
}

void fmtClock(char* dst, uint8_t h, uint8_t m, bool h24){
  if(!h24){
    uint8_t hh = h % 12;
    if(hh == 0) hh = 12;
    dst = fmtU16(dst, hh);
  } else {
    dst = fmtPad2(dst, h);
  }
  *dst++ = ':';
  fmtPad2(dst, m);
}

// -------------------------------------------------------------
//  Arranque del panel
// -------------------------------------------------------------
bool gfxInit(){
  uint16_t id = tft.readID();
  // El MAR3501 se identifica como ILI9486. Algunos lotes devuelven 0x0000 o
  // 0xD3D3 por el pin de lectura: en ese caso se fuerza el driver correcto en
  // vez de rendirse, que es lo que hacia fallar el arranque en frio.
  if(id == 0x0000 || id == 0xFFFF || id == 0xD3D3) id = 0x9486;
  tft.begin(id);
  tft.setRotation(0);                    // retrato 320x480
  tft.fillScreen(C_BLACK);
  return true;
}
#undef DIV255
#undef WALL_STEP

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_touch.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_touch.cpp
// #############################################################
//
//  El tactil resistivo del MAR3501 COMPARTE pines con el bus de
//  datos del LCD (XM=A2 y YP=A3 son ademas lineas de control del
//  ILI9486). TouchScreen los reconfigura como entradas analogicas
//  para medir, asi que hay que devolverlos a OUTPUT en cuanto se
//  termina la lectura o el siguiente dibujo sale corrupto. Ese es
//  el motivo de los pinMode() al final de touchPoll().
//
//  La calibracion es la OFICIAL del proyecto (fos_config.h) y no
//  se toca.
// #############################################################

static TouchScreen ts = TouchScreen(TS_XP, TS_YP, TS_XM, TS_YM, TS_RXPLATE);

FTouch tp;

static bool     rawDown   = false;   // estado crudo filtrado por antirrebote
static uint32_t edgeMs    = 0;       // instante del ultimo cambio de estado
static bool     longFired = false;
static uint32_t lastAct   = 0;

void touchInit(){
  memset(&tp, 0, sizeof(tp));
  lastAct = millis();
}

uint32_t touchLastActivity(){ return lastAct; }

bool hitRect(int16_t px, int16_t py, int16_t x, int16_t y, int16_t w, int16_t h){
  return px >= x && px < x + w && py >= y && py < y + h;
}

void touchConsume(){
  tp.pressed = tp.released = tp.tap = tp.longPress = false;
  tp.swipeUp = tp.swipeDown = tp.swipeLeft = tp.swipeRight = false;
  longFired = true;                 // no dispares una pulsacion larga heredada
}

void touchPoll(){
  tp.pressed = tp.released = tp.tap = tp.longPress = false;
  tp.swipeUp = tp.swipeDown = tp.swipeLeft = tp.swipeRight = false;

  TSPoint p = ts.getPoint();
  // Devolver el bus del LCD a su estado normal (ver cabecera del fichero).
  pinMode(TS_XM, OUTPUT);
  pinMode(TS_YP, OUTPUT);

  bool now = (p.z > TS_PRESS_MIN && p.z < TS_PRESS_MAX);
  uint32_t ms = millis();

  int16_t mx = 0, my = 0;
  if(now){
    // Calibracion oficial, retrato 320x480.
    long cx = map(p.x, TS_LEFT, TS_RT,  0, SCR_W);
    long cy = map(p.y, TS_TOP,  TS_BOT, 0, SCR_H);
    if(cx < 0) cx = 0;
    if(cx > SCR_W - 1) cx = SCR_W - 1;
    if(cy < 0) cy = 0;
    if(cy > SCR_H - 1) cy = SCR_H - 1;
    mx = (int16_t)cx; my = (int16_t)cy;
  }

  // Antirrebote: un cambio de estado solo se acepta si se mantiene.
  if(now != rawDown){
    if(ms - edgeMs >= GEST_DEBOUNCE_MS){ rawDown = now; edgeMs = ms; }
    else if(!now) return;            // rebote a la baja: ignora este frame
  } else {
    edgeMs = ms;
  }

  if(rawDown){
    lastAct = ms;
    if(!tp.down){                    // flanco de bajada
      tp.down = true;
      tp.pressed = true;
      tp.startX = mx; tp.startY = my;
      tp.downMs = ms;
      tp.moved = false;
      longFired = false;
    }
    tp.x = mx; tp.y = my;
    tp.dx = mx - tp.startX;
    tp.dy = my - tp.startY;
    int16_t ax = tp.dx < 0 ? -tp.dx : tp.dx;
    int16_t ay = tp.dy < 0 ? -tp.dy : tp.dy;
    if(ax > GEST_TAP_SLOP || ay > GEST_TAP_SLOP) tp.moved = true;
    if(!longFired && !tp.moved && (ms - tp.downMs) >= GEST_LONG_MS){
      tp.longPress = true;
      longFired = true;
    }
  } else if(tp.down){                // flanco de subida
    tp.down = false;
    tp.released = true;
    lastAct = ms;
    int16_t ax = tp.dx < 0 ? -tp.dx : tp.dx;
    int16_t ay = tp.dy < 0 ? -tp.dy : tp.dy;
    if(!tp.moved && (ms - tp.downMs) <= GEST_TAP_MAX_MS && !longFired){
      tp.tap = true;
    } else if(ax >= GEST_SWIPE_MIN || ay >= GEST_SWIPE_MIN){
      if(ax > ay){ if(tp.dx > 0) tp.swipeRight = true; else tp.swipeLeft = true; }
      else       { if(tp.dy > 0) tp.swipeDown  = true; else tp.swipeUp   = true; }
    }
  }
}

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_str.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_str.cpp
// ##  Tablas de texto en PROGMEM
// #############################################################

uint8_t gLang = LANG_ES;

#define S(name, es, en) \
  static const char name##_es[] PROGMEM = es; \
  static const char name##_en[] PROGMEM = en;

S(k00, "Continuar",              "Continue")
S(k01, "Idioma",                 "Language")
S(k02, "Bienvenido",             "Welcome")
S(k03, "Desliza para desbloquear","Swipe to unlock")
S(k04, "Clima",                  "Weather")
S(k05, "Noticias",               "News")
S(k06, "Sin noticias",           "No news")
S(k07, "Sin eventos",            "No events")
S(k08, "Ajustes",                "Settings")
S(k09, "Pantalla",               "Display")
S(k10, "Modo oscuro",            "Dark mode")
S(k11, "Vidrio",                 "Glass")
S(k12, "Estilo de iconos",       "Icon style")
S(k13, "Bloqueo",                "Lock screen")
S(k14, "PIN",                    "PIN")
S(k15, "Bloqueo autom\xA0tico",  "Auto-lock")
S(k16, "Fecha y hora",           "Date and time")
S(k17, "Formato 24 h",           "24 h format")
S(k18, "Acerca de",              "About")
S(k19, "Activado",               "On")
S(k20, "Desactivado",            "Off")
S(k21, "Atr\xA0s",               "Back")
S(k22, "Guardar",                "Save")
S(k23, "Limpiar",                "Clear")
S(k24, "Borrar",                 "Delete")
S(k25, "Aceptar",                "OK")
S(k26, "Cancelar",               "Cancel")
S(k27, "Introduce el PIN",       "Enter PIN")
S(k28, "PIN incorrecto",         "Wrong PIN")
S(k29, "Nuevo PIN",              "New PIN")
S(k30, "Sin PIN",                "No PIN")
S(k31, "Plano",                  "Flat")
S(k32, "Vidrio",                 "Glass")
S(k33, "Libre",                  "Free")
S(k34, "Usado",                  "Used")
S(k35, "Encendido",              "Uptime")
S(k36, "Toques",                 "Touches")
S(k37, "SRAM",                   "SRAM")
S(k38, "Flash",                  "Flash")
S(k39, "EEPROM",                 "EEPROM")
S(k40, "Placa",                  "Board")
S(k41, "Panel",                  "Panel")
S(k42, "Versi\xA2n",             "Version")
S(k43, "Vac\xA1o",               "Empty")
S(k44, "Color",                  "Color")
S(k45, "Trazo",                  "Brush")
S(k46, "Nueva partida",          "New game")
S(k47, "Puntos",                 "Score")
S(k48, "R\x82" "cord",           "Best")
S(k49, "Fin de partida",         "Game over")
S(k50, "Toca para jugar",        "Tap to play")
S(k51, "Iniciar",                "Start")
S(k52, "Parar",                  "Stop")
S(k53, "Reiniciar",              "Reset")
S(k54, "Hora",                   "Hour")
S(k55, "Minuto",                 "Minute")
S(k56, "D\xA1" "a",              "Day")
S(k57, "Mes",                    "Month")
S(k58, "A\xA4o",                 "Year")
S(k59, "Aplicaciones",           "Apps")
S(k60, "Sistema",                "System")
S(k61, "Nunca",                  "Never")
S(k62, "Total",                  "Total")
S(k63, "Toca para escribir",     "Tap to type")

static const char* const TBL_ES[S_N] PROGMEM = {
  k00_es,k01_es,k02_es,k03_es,k04_es,k05_es,k06_es,k07_es,k08_es,k09_es,
  k10_es,k11_es,k12_es,k13_es,k14_es,k15_es,k16_es,k17_es,k18_es,k19_es,
  k20_es,k21_es,k22_es,k23_es,k24_es,k25_es,k26_es,k27_es,k28_es,k29_es,
  k30_es,k31_es,k32_es,k33_es,k34_es,k35_es,k36_es,k37_es,k38_es,k39_es,
  k40_es,k41_es,k42_es,k43_es,k44_es,k45_es,k46_es,k47_es,k48_es,k49_es,
  k50_es,k51_es,k52_es,k53_es,k54_es,k55_es,k56_es,k57_es,k58_es,k59_es,
  k60_es,k61_es,k62_es,k63_es
};
static const char* const TBL_EN[S_N] PROGMEM = {
  k00_en,k01_en,k02_en,k03_en,k04_en,k05_en,k06_en,k07_en,k08_en,k09_en,
  k10_en,k11_en,k12_en,k13_en,k14_en,k15_en,k16_en,k17_en,k18_en,k19_en,
  k20_en,k21_en,k22_en,k23_en,k24_en,k25_en,k26_en,k27_en,k28_en,k29_en,
  k30_en,k31_en,k32_en,k33_en,k34_en,k35_en,k36_en,k37_en,k38_en,k39_en,
  k40_en,k41_en,k42_en,k43_en,k44_en,k45_en,k46_en,k47_en,k48_en,k49_en,
  k50_en,k51_en,k52_en,k53_en,k54_en,k55_en,k56_en,k57_en,k58_en,k59_en,
  k60_en,k61_en,k62_en,k63_en
};

PGM_P strP(uint8_t id){
  if(id >= S_N) id = 0;
  const char* const* t = (gLang == LANG_EN) ? TBL_EN : TBL_ES;
  return (PGM_P)pgm_read_word(&t[id]);
}

// ---------------- Nombres de aplicacion ----------------
S(a00, "Reloj",       "Clock")
S(a01, "Calendario",  "Calendar")
S(a02, "Calculadora", "Calculator")
S(a03, "Notas",       "Notes")
S(a04, "Paint",       "Paint")
S(a05, "Juegos",      "Games")
S(a06, "Cron\xA2metro","Stopwatch")
S(a07, "Linterna",    "Flashlight")
S(a08, "Bienestar",   "Wellbeing")
S(a09, "Memoria",     "Memory")
S(a10, "Sistema",     "System")
S(a11, "Ajustes",     "Settings")

static const char* const APP_ES[12] PROGMEM = {
  a00_es,a01_es,a02_es,a03_es,a04_es,a05_es,a06_es,a07_es,a08_es,a09_es,a10_es,a11_es };
static const char* const APP_EN[12] PROGMEM = {
  a00_en,a01_en,a02_en,a03_en,a04_en,a05_en,a06_en,a07_en,a08_en,a09_en,a10_en,a11_en };

PGM_P appNameP(uint8_t app){
  if(app > 11) app = 0;
  const char* const* t = (gLang == LANG_EN) ? APP_EN : APP_ES;
  return (PGM_P)pgm_read_word(&t[app]);
}

// ---------------- Dias y meses ----------------
static const char DS_ES[] PROGMEM = "Dom\0Lun\0Mar\0Mi\x82\0Jue\0Vie\0S\xA0" "b";
static const char DS_EN[] PROGMEM = "Sun\0Mon\0Tue\0Wed\0Thu\0Fri\0Sat";
static const char DF_ES[] PROGMEM = "Domingo\0Lunes\0Martes\0Mi\x82rcoles\0Jueves\0Viernes\0S\xA0" "bado";
static const char DF_EN[] PROGMEM = "Sunday\0Monday\0Tuesday\0Wednesday\0Thursday\0Friday\0Saturday";
static const char MS_ES[] PROGMEM = "Ene\0Feb\0Mar\0Abr\0May\0Jun\0Jul\0Ago\0Sep\0Oct\0Nov\0Dic";
static const char MS_EN[] PROGMEM = "Jan\0Feb\0Mar\0Apr\0May\0Jun\0Jul\0Aug\0Sep\0Oct\0Nov\0Dec";
static const char MF_ES[] PROGMEM = "Enero\0Febrero\0Marzo\0Abril\0Mayo\0Junio\0Julio\0Agosto\0Septiembre\0Octubre\0Noviembre\0Diciembre";
static const char MF_EN[] PROGMEM = "January\0February\0March\0April\0May\0June\0July\0August\0September\0October\0November\0December";

// Recorre una lista de cadenas contiguas terminadas en NUL dentro de PROGMEM.
// Evita una tabla de punteros por idioma (2 bytes por entrada x 4 tablas).
static PGM_P pgmNth(PGM_P base, uint8_t n){
  while(n--){ while(pgm_read_byte(base)) base++; base++; }
  return base;
}

PGM_P dayShortP(uint8_t d)  { return pgmNth(gLang == LANG_EN ? DS_EN : DS_ES, d % 7); }
PGM_P dayFullP(uint8_t d)   { return pgmNth(gLang == LANG_EN ? DF_EN : DF_ES, d % 7); }
PGM_P monthShortP(uint8_t m){ return pgmNth(gLang == LANG_EN ? MS_EN : MS_ES, m % 12); }
PGM_P monthFullP(uint8_t m) { return pgmNth(gLang == LANG_EN ? MF_EN : MF_ES, m % 12); }
#undef S

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_time.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_time.cpp
// #############################################################

FosTime gTime;
bool    gH24 = false;

static uint32_t lastMs   = 0;    // ultimo instante consumido de millis()
static uint32_t bootMs   = 0;

bool isLeap(uint16_t y){ return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

static const uint8_t MDAYS[12] PROGMEM = { 31,28,31,30,31,30,31,31,30,31,30,31 };

uint8_t daysInMonth(uint16_t y, uint8_t mon){
  mon %= 12;
  uint8_t d = pgm_read_byte(&MDAYS[mon]);
  if(mon == 1 && isLeap(y)) d = 29;
  return d;
}

// Congruencia de Sakamoto: dia de la semana sin tablas de calendario.
uint8_t dowOf(uint16_t y, uint8_t mon, uint8_t day){
  static const uint8_t T[12] PROGMEM = { 0,3,2,5,0,3,5,1,4,6,2,4 };
  uint16_t yy = y;
  if(mon < 2) yy--;
  return (uint8_t)((yy + yy/4 - yy/100 + yy/400 + pgm_read_byte(&T[mon % 12]) + day) % 7);
}

void timeSet(uint8_t h, uint8_t m, uint8_t day, uint8_t mon, uint16_t year){
  gTime.h = h % 24; gTime.m = m % 60; gTime.s = 0;
  gTime.mon = mon % 12;
  if(day < 1) day = 1;
  uint8_t dm = daysInMonth(year, gTime.mon);
  gTime.day = day > dm ? dm : day;
  gTime.year = year;
  gTime.dow = dowOf(year, gTime.mon, gTime.day);
  lastMs = millis();
}

void timeInit(){
  // Siembra por defecto, igual que Flex OS Ultra: sabado 4 de julio, 13:23.
  timeSet(13, 23, 4, 6, 2026);
  bootMs = millis();
  lastMs = bootMs;
}

bool timeTick(){
  uint32_t now = millis();
  uint32_t diff = now - lastMs;             // seguro ante el desbordamiento
  if(diff < 1000UL) return false;
  uint16_t secs = (uint16_t)(diff / 1000UL);
  lastMs += (uint32_t)secs * 1000UL;
  bool minuteChanged = false;
  while(secs--){
    if(++gTime.s < 60) continue;
    gTime.s = 0;
    minuteChanged = true;
    if(++gTime.m < 60) continue;
    gTime.m = 0;
    if(++gTime.h < 24) continue;
    gTime.h = 0;
    gTime.dow = (uint8_t)((gTime.dow + 1) % 7);
    if(++gTime.day <= daysInMonth(gTime.year, gTime.mon)) continue;
    gTime.day = 1;
    if(++gTime.mon < 12) continue;
    gTime.mon = 0;
    gTime.year++;
  }
  return minuteChanged;
}

uint32_t uptimeSec(){ return (millis() - bootMs) / 1000UL; }

void timeStr(char* dst){ fmtClock(dst, gTime.h, gTime.m, gH24); }

static char* appendP(char* dst, PGM_P src){
  char c;
  while((c = (char)pgm_read_byte(src++))) *dst++ = c;
  *dst = 0;
  return dst;
}

void dateShortStr(char* dst){
  dst = appendP(dst, dayShortP(gTime.dow));
  *dst++ = ','; *dst++ = ' ';
  dst = fmtU16(dst, gTime.day);
  *dst++ = ' ';
  appendP(dst, monthShortP(gTime.mon));
}

void dateLongStr(char* dst){
  dst = appendP(dst, dayFullP(gTime.dow));
  *dst++ = ','; *dst++ = ' ';
  dst = fmtU16(dst, gTime.day);
  *dst++ = ' ';
  // "4 de julio" en espanol, "4 July" en ingles.
  if(gLang == LANG_ES){ *dst++ = 'd'; *dst++ = 'e'; *dst++ = ' '; }
  appendP(dst, monthFullP(gTime.mon));
}

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_store.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_store.cpp
// #############################################################

#define EE_MAGIC0 'F'
#define EE_MAGIC1 'S'
#define EE_VERSION 1

bool     gGlass      = true;
bool     gIconGlass  = false;
bool     gNavGest    = false;
uint8_t  gAutoLockIdx = 1;
bool     gPinSet     = false;
uint8_t  gPin[LOCK_PIN_LEN] = { 0, 0, 0, 0 };
uint8_t  gHomeOrder[HOME_SLOTS] = { 0,1,2,3,4,5,6,7,8,9,10,11 };
uint16_t gBest    = 0;
uint32_t gTouches = 0;
uint32_t gMinutes = 0;
bool     gFirstRun = false;

static void put32(int addr, uint32_t v){
  for(uint8_t i = 0; i < 4; i++) EEPROM.update(addr + i, (uint8_t)(v >> (8 * i)));
}
static uint32_t get32(int addr){
  uint32_t v = 0;
  for(uint8_t i = 0; i < 4; i++) v |= (uint32_t)EEPROM.read(addr + i) << (8 * i);
  return v;
}

void storeSaveFlags(){
  uint8_t f = 0;
  if(gDark)      f |= 0x01;
  if(gGlass)     f |= 0x02;
  if(gIconGlass) f |= 0x04;
  if(gH24)       f |= 0x08;
  if(gNavGest)   f |= 0x10;
  EEPROM.update(EE_FLAGS, f);
}
void storeSaveLang()     { EEPROM.update(EE_LANG, gLang); }
void storeSaveAutoLock() { EEPROM.update(EE_AUTOLOCK, gAutoLockIdx); }
void storeSaveBest()     { EEPROM.update(EE_BEST, (uint8_t)gBest); EEPROM.update(EE_BEST + 1, (uint8_t)(gBest >> 8)); }
void storeSaveUsage()    { put32(EE_TOUCHES, gTouches); put32(EE_MINUTES, gMinutes); }

void storeSavePin(){
  EEPROM.update(EE_PINSET, gPinSet ? 1 : 0);
  for(uint8_t i = 0; i < LOCK_PIN_LEN; i++) EEPROM.update(EE_PIN + i, gPin[i]);
}
void storeSaveHomeOrder(){
  for(uint8_t i = 0; i < HOME_SLOTS; i++) EEPROM.update(EE_HOMEORD + i, gHomeOrder[i]);
}
void storeSaveTime(){
  EEPROM.update(EE_TIME + 0, gTime.h);
  EEPROM.update(EE_TIME + 1, gTime.m);
  EEPROM.update(EE_TIME + 2, gTime.day);
  EEPROM.update(EE_TIME + 3, gTime.mon);
  EEPROM.update(EE_TIME + 4, (uint8_t)(gTime.year - 2000));
}
void storeLoadTime(){
  uint8_t h = EEPROM.read(EE_TIME + 0), m = EEPROM.read(EE_TIME + 1);
  uint8_t d = EEPROM.read(EE_TIME + 2), mo = EEPROM.read(EE_TIME + 3);
  uint8_t y = EEPROM.read(EE_TIME + 4);
  if(h < 24 && m < 60 && d >= 1 && d <= 31 && mo < 12 && y < 100)
    timeSet(h, m, d, mo, (uint16_t)2000 + y);
}

void storeFactoryReset(){
  EEPROM.update(EE_MAGIC + 0, EE_MAGIC0);
  EEPROM.update(EE_MAGIC + 1, EE_MAGIC1);
  EEPROM.update(EE_VER, EE_VERSION);
  gLang = LANG_ES;
  gDark = true; gGlass = true; gIconGlass = false; gH24 = false; gNavGest = false;
  gAutoLockIdx = 1;
  gPinSet = false;
  for(uint8_t i = 0; i < LOCK_PIN_LEN; i++) gPin[i] = 0;
  for(uint8_t i = 0; i < HOME_SLOTS; i++)   gHomeOrder[i] = i;
  gBest = 0; gTouches = 0; gMinutes = 0;
  storeSaveLang(); storeSaveFlags(); storeSaveAutoLock(); storeSavePin();
  storeSaveHomeOrder(); storeSaveBest(); storeSaveUsage(); storeSaveTime();
  for(uint8_t s = 0; s < NOTE_SLOTS; s++) EEPROM.update(EE_NOTES + s * (NOTE_LEN + 1), 0);
}

void storeInit(){
  if(EEPROM.read(EE_MAGIC) != EE_MAGIC0 || EEPROM.read(EE_MAGIC + 1) != EE_MAGIC1 ||
     EEPROM.read(EE_VER)   != EE_VERSION){
    storeFactoryReset();
    gFirstRun = true;
    return;
  }
  uint8_t l = EEPROM.read(EE_LANG);
  gLang = (l < LANG_N) ? l : (uint8_t)LANG_ES;
  uint8_t f = EEPROM.read(EE_FLAGS);
  gDark      = f & 0x01;
  gGlass     = f & 0x02;
  gIconGlass = f & 0x04;
  gH24       = f & 0x08;
  gNavGest   = f & 0x10;
  gAutoLockIdx = EEPROM.read(EE_AUTOLOCK);
  if(gAutoLockIdx >= AUTOLOCK_OPTIONS) gAutoLockIdx = 1;
  gPinSet = EEPROM.read(EE_PINSET) == 1;
  for(uint8_t i = 0; i < LOCK_PIN_LEN; i++){
    uint8_t d = EEPROM.read(EE_PIN + i);
    gPin[i] = (d < 10) ? d : 0;
  }
  bool ordOk = true;
  uint16_t seen = 0;
  for(uint8_t i = 0; i < HOME_SLOTS; i++){
    uint8_t v = EEPROM.read(EE_HOMEORD + i);
    if(v >= APP_N || (seen & (1u << v))){ ordOk = false; break; }
    seen |= (1u << v);
    gHomeOrder[i] = v;
  }
  if(!ordOk) for(uint8_t i = 0; i < HOME_SLOTS; i++) gHomeOrder[i] = i;
  gBest    = (uint16_t)EEPROM.read(EE_BEST) | ((uint16_t)EEPROM.read(EE_BEST + 1) << 8);
  gTouches = get32(EE_TOUCHES);
  gMinutes = get32(EE_MINUTES);
}

// ---------------- Notas ----------------
static int noteAddr(uint8_t slot){ return EE_NOTES + (int)slot * (NOTE_LEN + 1); }

void noteLoad(uint8_t slot, char* dst){
  if(slot >= NOTE_SLOTS){ dst[0] = 0; return; }
  int a = noteAddr(slot);
  uint8_t i = 0;
  for(; i < NOTE_LEN; i++){
    char c = (char)EEPROM.read(a + i);
    if(c == 0 || (uint8_t)c == 0xFF) break;
    dst[i] = c;
  }
  dst[i] = 0;
}

void noteSave(uint8_t slot, const char* src){
  if(slot >= NOTE_SLOTS) return;
  int a = noteAddr(slot);
  uint8_t i = 0;
  for(; i < NOTE_LEN && src[i]; i++) EEPROM.update(a + i, (uint8_t)src[i]);
  EEPROM.update(a + i, 0);
}

bool noteEmpty(uint8_t slot){
  if(slot >= NOTE_SLOTS) return true;
  uint8_t c = EEPROM.read(noteAddr(slot));
  return c == 0 || c == 0xFF;
}

// ---------------- SRAM libre ----------------
uint16_t freeRam(){
  extern int  __heap_start;
  extern int* __brkval;
  int v;
  return (uint16_t)((int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval));
}
#undef EE_MAGIC0
#undef EE_MAGIC1
#undef EE_VERSION

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_icons.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_icons.cpp
// #############################################################

// Porcentaje de S, entero.
#define P(v, pc) (int16_t)(((int32_t)(v) * (pc)) / 100)

// cos/sin x100 en 8 direcciones (0, 45, 90 ... 315 grados). Sustituye a
// cosf/sinf: mismas posiciones, sin biblioteca matematica.
static const int8_t DIR8[8][2] PROGMEM = {
  { 100, 0 }, { 71, 71 }, { 0, 100 }, { -71, 71 },
  { -100, 0 }, { -71, -71 }, { 0, -100 }, { 71, -71 }
};

static void iconBase(int16_t x, int16_t y, uint8_t S, uint16_t bg){
  int16_t r = P(S, 24);
  if(gIconGlass){
    // Estilo "Vidrio": el fondo del icono deja ver el wallpaper.
    glassRect(x, y, S, S, r, bg, 190);
  } else {
    tft.fillRoundRect(x, y, S, S, r, bg);
    // Brillo superior sutil (alpha 22 sobre el propio fondo del icono).
    tft.fillRoundRect(x, y, S, S / 2, r, mix565(bg, C_WHITE, 22));
  }
}

void drawAppIcon(uint8_t id, int16_t x, int16_t y, uint8_t S){
  int16_t cx = x + S / 2, cy = y + S / 2;
  uint8_t tk = S / 12; if(tk < 2) tk = 2;
  const uint16_t W = C_WHITE;

  switch(id){
    case APP_CLOCK: {
      iconBase(x, y, S, RGB(245,245,247));
      uint16_t ink = RGB(70,70,74);
      gfxRing(cx, cy, P(S,36), 2, ink);
      tft.fillRect(cx - 1, y + P(S,16), 2, S/12, ink);
      tft.fillRect(cx - 1, y + S - P(S,16) - S/12, 2, S/12, ink);
      tft.fillRect(x + P(S,16), cy - 1, S/12, 2, ink);
      tft.fillRect(x + S - P(S,16) - S/12, cy - 1, S/12, 2, ink);
      gfxThickLine(cx, cy, cx - P(S,14), cy - P(S,10), tk/2 + 1, RGB(30,30,30));
      gfxThickLine(cx, cy, cx + P(S,12), cy - P(S,20), tk/2 + 1, RGB(245,140,30));
      tft.fillCircle(cx, cy, tk/2 + 1, RGB(30,30,30));
    } break;

    case APP_CALENDAR: {
      iconBase(x, y, S, W);
      tft.fillRect(x + P(S,16), y + P(S,18), P(S,68), P(S,16), RGB(232,70,70));
      char d[4]; fmtU16(d, gTime.day);
      gfxTextC(cx, y + P(S,42), d, S >= 44 ? 3 : 2, RGB(60,60,64));
    } break;

    case APP_CALC: {
      iconBase(x, y, S, RGB(58,58,60));
      tft.fillRoundRect(x + P(S,18), y + P(S,16), P(S,64), P(S,16), 3, RGB(210,210,215));
      for(uint8_t r = 0; r < 3; r++) for(uint8_t c = 0; c < 4; c++)
        tft.fillRoundRect(x + P(S,18) + c * P(S,17), y + P(S,40) + r * P(S,15),
                          P(S,12), P(S,10), 2,
                          (c == 3) ? RGB(245,150,30) : RGB(150,150,155));
    } break;

    case APP_NOTES: {
      iconBase(x, y, S, RGB(232,167,90));
      tft.fillRoundRect(x + P(S,22), y + P(S,18), P(S,48), P(S,62), 4, W);
      for(uint8_t i = 0; i < 3; i++)
        tft.fillRect(x + P(S,30), y + P(S,30) + i * P(S,12), P(S,32), 2, RGB(180,180,185));
      gfxThickLine(x + P(S,56), y + P(S,66), x + P(S,78), y + P(S,40), tk/2 + 1, RGB(120,90,40));
      tft.fillCircle(x + P(S,78), y + P(S,40), tk/2 + 1, RGB(245,210,90));
    } break;

    case APP_PAINT: {
      iconBase(x, y, S, RGB(241,231,210));
      tft.fillCircle(cx - P(S,4), cy + P(S,2), P(S,27), RGB(236,226,205));
      tft.fillCircle(cx + P(S,11), cy + P(S,11), P(S,6), RGB(241,231,210));
      tft.fillCircle(cx - P(S,14), cy - P(S,6),  P(S,5), RGB(230,70,70));
      tft.fillCircle(cx - P(S,2),  cy - P(S,13), P(S,5), RGB(240,200,60));
      tft.fillCircle(cx + P(S,10), cy - P(S,7),  P(S,5), RGB(70,130,235));
      tft.fillCircle(cx - P(S,16), cy + P(S,9),  P(S,5), RGB(80,190,90));
      gfxThickLine(cx + P(S,2), cy - P(S,16), cx + P(S,26), cy - P(S,30), tk/2 + 1, RGB(140,100,60));
    } break;

    case APP_GAMES: {
      iconBase(x, y, S, RGB(142,30,30));
      tft.drawRoundRect(x + P(S,14), y + P(S,34), P(S,72), P(S,30), P(S,13), W);
      tft.drawRoundRect(x + P(S,14) + 1, y + P(S,34) + 1, P(S,72) - 2, P(S,30) - 2, P(S,12), W);
      tft.fillRect(cx - P(S,22) - 1, cy + P(S,2), P(S,12), 3, W);
      tft.fillRect(cx - P(S,16) - 1, cy - P(S,4), 3, P(S,12), W);
      tft.fillCircle(cx + P(S,14), cy - P(S,1), 3, W);
      tft.fillCircle(cx + P(S,22), cy + P(S,5), 3, W);
    } break;

    case APP_TIMER: {
      iconBase(x, y, S, RGB(40,44,56));
      tft.fillRect(cx - P(S,8), y + P(S,12), P(S,16), P(S,7), RGB(230,235,245));
      gfxRing(cx, cy + P(S,4), P(S,32), 3, RGB(230,235,245));
      gfxThickLine(cx, cy + P(S,4), cx + P(S,16), cy - P(S,12), tk/2 + 1, RGB(245,150,30));
      tft.fillCircle(cx, cy + P(S,4), tk/2 + 1, RGB(230,235,245));
    } break;

    case APP_LIGHT: {
      iconBase(x, y, S, RGB(70,76,92));
      uint16_t lamp = RGB(232,236,246);
      tft.fillRoundRect(cx - P(S,10), y + P(S,20), P(S,20), P(S,14), 3, lamp);
      tft.fillRoundRect(cx - P(S,7),  y + P(S,32), P(S,14), P(S,34), 3, RGB(180,188,205));
      tft.fillTriangle(cx - P(S,22), y + P(S,16), cx + P(S,22), y + P(S,16),
                       cx, y + P(S,2), RGB(250,220,110));
    } break;

    case APP_WELL: {
      iconBase(x, y, S, RGB(92,193,90));
      gfxThickLine(cx - P(S,16), cy + P(S,2),  cx - P(S,2), cy + P(S,16), tk/2 + 2, W);
      gfxThickLine(cx - P(S,2),  cy + P(S,16), cx + P(S,20), cy - P(S,14), tk/2 + 2, W);
    } break;

    case APP_MEMORY: {
      iconBase(x, y, S, RGB(59,123,217));
      uint16_t fol = RGB(225,236,250);
      tft.fillRoundRect(x + P(S,20), y + P(S,28), P(S,30), P(S,12), 3, fol);
      tft.fillRoundRect(x + P(S,18), y + P(S,36), P(S,64), P(S,34), 4, fol);
      tft.fillRect(x + P(S,26), y + P(S,46), P(S,48), 3, RGB(140,175,225));
      tft.fillRect(x + P(S,26), y + P(S,54), P(S,30), 3, RGB(140,175,225));
    } break;

    case APP_SYSINFO: {
      iconBase(x, y, S, RGB(48,54,72));
      uint16_t chip = RGB(120,200,240);
      tft.fillRoundRect(x + P(S,26), y + P(S,26), P(S,48), P(S,48), 4, chip);
      tft.fillRoundRect(x + P(S,36), y + P(S,36), P(S,28), P(S,28), 2, RGB(30,36,50));
      for(uint8_t i = 0; i < 3; i++){
        int16_t d = P(S,34) + i * P(S,16);                     // patillas del chip
        tft.fillRect(x + d,        y + P(S,16), 3, P(S,10), chip);
        tft.fillRect(x + d,        y + P(S,74), 3, P(S,10), chip);
        tft.fillRect(x + P(S,16),  y + d,       P(S,10), 3, chip);
        tft.fillRect(x + P(S,74),  y + d,       P(S,10), 3, chip);
      }
    } break;

    case APP_SETTINGS:
    default: {
      iconBase(x, y, S, RGB(138,143,152));
      uint16_t g = RGB(70,74,84);
      tft.fillCircle(cx, cy, P(S,26), g);
      for(uint8_t k = 0; k < 8; k++){
        int8_t dx = (int8_t)pgm_read_byte(&DIR8[k][0]);
        int8_t dy = (int8_t)pgm_read_byte(&DIR8[k][1]);
        tft.fillCircle(cx + P(S,30) * dx / 100, cy + P(S,30) * dy / 100, P(S,8), g);
      }
      tft.fillCircle(cx, cy, P(S,10), RGB(138,143,152));
    } break;
  }
}

// -------------------------------------------------------------
//  Barra de estado / navegacion
// -------------------------------------------------------------
void drawBattery(int16_t x, int16_t y, uint8_t w, uint8_t h, uint8_t pct, uint16_t col){
  tft.drawRoundRect(x, y, w, h, 2, col);
  tft.fillRect(x + w, y + h / 3, 2, h / 3, col);
  uint8_t inner = (uint8_t)(((uint16_t)(w - 4) * pct) / 100);
  if(inner) tft.fillRect(x + 2, y + 2, inner, h - 4, col);
}

// El Mega no tiene radio: donde Flex OS Ultra pintaba el icono de Wi-Fi,
// UltraSingle pinta el indicador de actividad del sistema (tres barras que
// reflejan la SRAM libre). Mismo hueco, misma silueta, dato real.
void drawSignal(int16_t x, int16_t yBase, uint8_t h, uint16_t col){
  uint16_t fr = freeRam();
  uint8_t lit = fr > 3000 ? 3 : (fr > 1500 ? 2 : 1);
  for(uint8_t i = 0; i < 3; i++){
    uint8_t bh = (uint8_t)(h * (i + 1) / 3);
    uint16_t c = (i < lit) ? col : mix565(col, C_BLACK, 150);
    tft.fillRect(x + i * 4, yBase - bh, 3, bh, c);
  }
}

void drawHomeIndicator(int16_t yBottom, uint16_t col){
  int16_t bw = 90, bh = 4;
  tft.fillRoundRect((SCR_W - bw) / 2, yBottom - 12 - bh, bw, bh, 2, col);
}

// Botones clasicos (atras / inicio / recientes), como en Flex OS Ultra.
void drawNavBar(int16_t y, uint16_t col){
  int16_t bx = SCR_W / 6;
  tft.fillTriangle(bx - 8, y + 7, bx + 6, y - 1, bx + 6, y + 15, col);
  tft.drawCircle(SCR_W / 2, y + 7, 9, col);
  tft.drawCircle(SCR_W / 2, y + 7, 8, col);
  int16_t rx = SCR_W * 5 / 6;
  tft.drawRoundRect(rx - 9, y - 2, 18, 18, 3, col);
}
#undef P

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_ui.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_ui.cpp
// #############################################################

// -------------------------------------------------------------
//  Barra superior
// -------------------------------------------------------------
//  Mismo reparto que Flex OS Ultra: hora grande y fecha corta a la
//  izquierda, indicadores a la derecha.
static void barBg(bool overWall){
  if(overWall) gfxWallRect(0, 0, SCR_W, BAR_TOP_H);
  else         tft.fillRect(0, 0, SCR_W, BAR_TOP_H, PAGE_BG);
}

void uiStatusClock(bool overWall){
  uint16_t fg  = overWall ? C_ON_WALL    : TXT_HI;
  uint16_t fg2 = overWall ? C_ON_WALL_LO : TXT_LO;
  if(overWall) gfxWallRect(0, 0, 150, BAR_TOP_H);
  else         tft.fillRect(0, 0, 150, BAR_TOP_H, PAGE_BG);
  char b[24];
  timeStr(b);
  gfxText(10, 4, b, 2, fg);
  dateShortStr(b);
  gfxText(10, 21, b, 1, fg2);
}

void uiStatusBar(bool overWall){
  barBg(overWall);
  uint16_t fg = overWall ? C_ON_WALL : TXT_HI;
  uiStatusClock(overWall);
  drawSignal(SCR_W - 62, 22, 12, fg);
  drawBattery(SCR_W - 40, 10, 26, 13, 82, fg);
}

// -------------------------------------------------------------
//  Barra de navegacion
// -------------------------------------------------------------
void uiNavBar(bool overWall){
  int16_t y = SCR_H - BAR_NAV_H;
  if(overWall) gfxWallRect(0, y, SCR_W, BAR_NAV_H);
  else         tft.fillRect(0, y, SCR_W, BAR_NAV_H, PAGE_BG);
  uint16_t col = overWall ? NAV_TINT : TXT_LO;
  if(gNavGest) drawHomeIndicator(SCR_H, col);
  else         drawNavBar(y + 10, col);
}

int8_t uiNavHit(int16_t x, int16_t y){
  if(y < SCR_H - BAR_NAV_H) return -1;
  if(gNavGest) return 1;                 // en modo gestos, toda la barra vuelve a Inicio
  if(x < SCR_W / 3)          return 0;
  if(x < SCR_W * 2 / 3)      return 1;
  return 2;
}

// -------------------------------------------------------------
//  Cabecera de aplicacion
// -------------------------------------------------------------
void uiAppHeader(PGM_P title){
  tft.fillRect(0, 0, SCR_W, APP_HDR_H, PAGE_BG);
  // Flecha de vuelta: punta a la IZQUIERDA + asta horizontal.
  const int16_t ay = APP_HDR_H / 2;
  gfxThickLine(24, ay - 9, 15, ay, 2, TXT_HI);
  gfxThickLine(15, ay, 24, ay + 9, 2, TXT_HI);
  tft.fillRect(15, ay - 1, 16, 2, TXT_HI);
  gfxTextCP(SCR_W / 2, APP_HDR_H / 2 - 7, title, 2, TXT_HI);
  tft.drawFastHLine(0, APP_HDR_H - 1, SCR_W, BORDER);
}

void uiAppScreen(PGM_P title){
  tft.fillRect(0, 0, SCR_W, SCR_H, PAGE_BG);
  uiAppHeader(title);
  uiNavBar(false);
}

bool uiBackHit(int16_t x, int16_t y){ return y < APP_HDR_H && x < 60; }

// -------------------------------------------------------------
//  Controles
// -------------------------------------------------------------
void uiCard(int16_t x, int16_t y, int16_t w, int16_t h){
  if(gGlass) tintRect(x, y, w, h, 10, CARD_ALT, 120, PAGE_BG);
  else       tft.fillRoundRect(x, y, w, h, 10, CARD_BG);
}

void uiToggle(int16_t x, int16_t y, bool on){
  const int16_t w = 42, h = 22;
  tft.fillRoundRect(x, y, w, h, h / 2, on ? C_ACCENT : (gDark ? RGB(70,76,92) : RGB(200,206,218)));
  tft.fillCircle(on ? x + w - h / 2 : x + h / 2, y + h / 2, h / 2 - 3, C_WHITE);
}

static void rowBase(int16_t y, PGM_P label){
  uiCard(12, y, SCR_W - 24, ROW_H - 6);
  gfxTextP(26, y + (ROW_H - 6) / 2 - 7, label, 2, TXT_HI);
}

void uiRow(int16_t y, PGM_P label, const char* value, bool chevron){
  rowBase(y, label);
  int16_t rx = SCR_W - 24 - (chevron ? 20 : 6);
  if(value) gfxTextR(rx, y + (ROW_H - 6) / 2 - 4, value, 1, TXT_LO);
  if(chevron) gfxChevron(SCR_W - 32, y + (ROW_H - 6) / 2, 5, CHEVRON);
}

void uiRowP(int16_t y, PGM_P label, PGM_P value, bool chevron){
  rowBase(y, label);
  int16_t rx = SCR_W - 24 - (chevron ? 20 : 6);
  if(value) gfxTextP(rx - gfxTextWP(value, 1), y + (ROW_H - 6) / 2 - 4, value, 1, TXT_LO);
  if(chevron) gfxChevron(SCR_W - 32, y + (ROW_H - 6) / 2, 5, CHEVRON);
}

void uiRowToggle(int16_t y, PGM_P label, bool on){
  rowBase(y, label);
  uiToggle(SCR_W - 24 - 42 - 8, y + (ROW_H - 6) / 2 - 11, on);
}

void uiButtonP(int16_t x, int16_t y, int16_t w, int16_t h, PGM_P label, bool primary){
  tft.fillRoundRect(x, y, w, h, h / 3, primary ? C_ACCENT : CARD_ALT);
  gfxTextCP(x + w / 2, y + h / 2 - 7, label, 2, primary ? C_WHITE : TXT_HI);
}

// -------------------------------------------------------------
//  Aviso breve
// -------------------------------------------------------------
static PGM_P   toastMsg = 0;
static uint32_t toastT0 = 0;
#define TOAST_MS 1400UL
#define TOAST_H  32
#define TOAST_Y  (SCR_H - BAR_NAV_H - TOAST_H - 10)

void uiToast(PGM_P msg){
  toastMsg = msg;
  toastT0  = millis();
  int16_t w = gfxTextWP(msg, 1) + 32;
  if(w > SCR_W - 40) w = SCR_W - 40;
  tft.fillRoundRect((SCR_W - w) / 2, TOAST_Y, w, TOAST_H, TOAST_H / 2, C_ACCENT);
  gfxTextCP(SCR_W / 2, TOAST_Y + TOAST_H / 2 - 4, msg, 1, C_WHITE);
}

void uiToastTick(){
  if(!toastMsg) return;
  if(millis() - toastT0 < TOAST_MS) return;
  tft.fillRect(0, TOAST_Y, SCR_W, TOAST_H, PAGE_BG);
  toastMsg = 0;
}

// -------------------------------------------------------------
//  Teclado numerico (PIN)
// -------------------------------------------------------------
//  Rejilla 3x4: 1..9, borrar, 0. Se usa en la pantalla de bloqueo y al
//  cambiar el PIN, con el mismo aspecto de circulos translucidos de
//  Flex OS Ultra.
static const uint8_t PAD_MAP[12] PROGMEM = { 1,2,3, 4,5,6, 7,8,9, 200,0,201 };

void uiPinPad(uint16_t bg, bool overWall){
  for(uint8_t i = 0; i < 12; i++){
    uint8_t k = pgm_read_byte(&PAD_MAP[i]);
    if(k == 201) continue;                        // hueco
    int16_t x = PAD_X + (i % 3) * PAD_HSTEP;
    int16_t y = PAD_Y + (i / 3) * PAD_VSTEP;
    if(k == 200){                                  // borrar: sin circulo, solo el glifo
      uint16_t fg = overWall ? C_ON_WALL : TXT_HI;
      tft.fillTriangle(x + 10, y + PAD_BTN/2, x + 24, y + PAD_BTN/2 - 12,
                       x + 24, y + PAD_BTN/2 + 12, fg);
      tft.fillRect(x + 24, y + PAD_BTN/2 - 12, 22, 24, fg);
      gfxThickLine(x + 30, y + PAD_BTN/2 - 6, x + 42, y + PAD_BTN/2 + 6, 2, overWall ? C_ACCENT_DK : PAGE_BG);
      gfxThickLine(x + 42, y + PAD_BTN/2 - 6, x + 30, y + PAD_BTN/2 + 6, 2, overWall ? C_ACCENT_DK : PAGE_BG);
      continue;
    }
    if(overWall) glassRect(x, y, PAD_BTN, PAD_BTN, PAD_BTN / 2, GLASS_CARD, 130);
    else         tft.fillCircle(x + PAD_BTN/2, y + PAD_BTN/2, PAD_BTN/2, CARD_ALT);
    char d[2] = { (char)('0' + k), 0 };
    gfxTextC(x + PAD_BTN/2, y + PAD_BTN/2 - 10, d, 3, overWall ? C_ON_WALL : TXT_HI);
  }
  (void)bg;
}

int8_t uiPinPadHit(int16_t x, int16_t y){
  for(uint8_t i = 0; i < 12; i++){
    uint8_t k = pgm_read_byte(&PAD_MAP[i]);
    if(k == 201) continue;
    int16_t bx = PAD_X + (i % 3) * PAD_HSTEP;
    int16_t by = PAD_Y + (i / 3) * PAD_VSTEP;
    if(hitRect(x, y, bx, by, PAD_BTN, PAD_BTN)) return (k == 200) ? -2 : (int8_t)k;
  }
  return -1;
}

// -------------------------------------------------------------
//  Teclado de texto
// -------------------------------------------------------------
//  Cuatro filas, aspecto de teclas redondeadas como el teclado de
//  Flex OS Ultra pero sin sus fases B-G (multitoque, portapapeles de
//  12 ranuras, autocompletado): en 8 KB de SRAM eso no cabe y ademas
//  no aporta nada a las dos apps que escriben texto.
static const char KB_R0[] PROGMEM = "qwertyuiop";
static const char KB_R1[] PROGMEM = "asdfghjkl";
static const char KB_R2[] PROGMEM = "zxcvbnm";
static const char KB_N0[] PROGMEM = "1234567890";
static const char KB_N1[] PROGMEM = "-/:;()$&@\"";
static const char KB_N2[] PROGMEM = ".,?!'+*=%";

static bool kbShift = false;
static bool kbNum   = false;

#define KEY_H   32
#define KEY_GAP 3
#define KEY_W   ((SCR_W - 11 * KEY_GAP) / 10)

void uiKeyboardShiftToggle(){ kbShift = !kbShift; }

static PGM_P kbRow(uint8_t r){
  if(kbNum) return r == 0 ? KB_N0 : (r == 1 ? KB_N1 : KB_N2);
  return r == 0 ? KB_R0 : (r == 1 ? KB_R1 : KB_R2);
}

static void kbKey(int16_t x, int16_t y, int16_t w, char c, PGM_P lbl, bool accent){
  tft.fillRoundRect(x, y, w, KEY_H, 6, accent ? C_ACCENT : CARD_ALT);
  if(lbl) gfxTextCP(x + w / 2, y + KEY_H / 2 - 4, lbl, 1, accent ? C_WHITE : TXT_HI);
  else {
    char s[2] = { c, 0 };
    gfxTextC(x + w / 2, y + KEY_H / 2 - 7, s, 2, TXT_HI);
  }
}

static const char L_SH[]  PROGMEM = "^";
static const char L_DEL[] PROGMEM = "<-";
static const char L_NUM[] PROGMEM = "123";
static const char L_ABC[] PROGMEM = "ABC";
static const char L_OK[]  PROGMEM = "OK";

void uiKeyboard(){
  tft.fillRect(0, KB_Y, SCR_W, KB_H, gDark ? RGB(26,29,38) : RGB(226,230,238));
  for(uint8_t r = 0; r < 3; r++){
    PGM_P row = kbRow(r);
    uint8_t n = (uint8_t)strlen_P(row);
    int16_t total = n * KEY_W + (n - 1) * KEY_GAP;
    int16_t x0 = (SCR_W - total) / 2;
    int16_t y  = KB_Y + 4 + r * (KEY_H + KEY_GAP);
    for(uint8_t i = 0; i < n; i++){
      char c = (char)pgm_read_byte(row + i);
      if(kbShift && !kbNum && c >= 'a' && c <= 'z') c = (char)(c - 32);
      kbKey(x0 + i * (KEY_W + KEY_GAP), y, KEY_W, c, 0, false);
    }
  }
  // Cuarta fila: mayusculas, numeros, espacio, borrar, aceptar.
  int16_t y = KB_Y + 4 + 3 * (KEY_H + KEY_GAP);
  int16_t w2 = KEY_W * 2 + KEY_GAP;
  kbKey(KEY_GAP,                       y, w2, 0, kbShift ? L_SH : L_SH, kbShift);
  kbKey(KEY_GAP * 2 + w2,              y, w2, 0, kbNum ? L_ABC : L_NUM, false);
  kbKey(KEY_GAP * 3 + w2 * 2,          y, SCR_W - (KEY_GAP * 5 + w2 * 4), 0, 0, false);
  kbKey(SCR_W - KEY_GAP * 2 - w2 * 2,  y, w2, 0, L_DEL, false);
  kbKey(SCR_W - KEY_GAP - w2,          y, w2, 0, L_OK, true);
}

char uiKeyboardHit(int16_t x, int16_t y){
  if(y < KB_Y) return 0;
  for(uint8_t r = 0; r < 3; r++){
    PGM_P row = kbRow(r);
    uint8_t n = (uint8_t)strlen_P(row);
    int16_t total = n * KEY_W + (n - 1) * KEY_GAP;
    int16_t x0 = (SCR_W - total) / 2;
    int16_t ky = KB_Y + 4 + r * (KEY_H + KEY_GAP);
    if(y < ky || y >= ky + KEY_H) continue;
    for(uint8_t i = 0; i < n; i++){
      int16_t kx = x0 + i * (KEY_W + KEY_GAP);
      if(x >= kx && x < kx + KEY_W){
        char c = (char)pgm_read_byte(row + i);
        if(kbShift && !kbNum && c >= 'a' && c <= 'z') c = (char)(c - 32);
        return c;
      }
    }
    return 0;
  }
  int16_t ky = KB_Y + 4 + 3 * (KEY_H + KEY_GAP);
  if(y < ky || y >= ky + KEY_H) return 0;
  int16_t w2 = KEY_W * 2 + KEY_GAP;
  if(x < KEY_GAP + w2)                    { kbShift = !kbShift; uiKeyboard(); return 0; }
  if(x < KEY_GAP * 2 + w2 * 2)            { kbNum = !kbNum; kbShift = false; uiKeyboard(); return 0; }
  if(x >= SCR_W - KEY_GAP - w2)           return 13;
  if(x >= SCR_W - KEY_GAP * 2 - w2 * 2)   return 8;
  return ' ';
}
#undef TOAST_MS
#undef TOAST_H
#undef TOAST_Y
#undef KEY_H
#undef KEY_GAP
#undef KEY_W

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_fx.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_fx.cpp
// #############################################################

#define FX_BAND 32

void fxWipe(bool up){
  if(up){
    for(int16_t y = SCR_H - FX_BAND; y >= 0; y -= FX_BAND) gfxWallRect(0, y, SCR_W, FX_BAND);
  } else {
    for(int16_t y = 0; y < SCR_H; y += FX_BAND) gfxWallRect(0, y, SCR_W, FX_BAND);
  }
}

// Interpolacion lineal entera entre a y b con peso k/n.
static inline int16_t lerpi(int16_t a, int16_t b, uint8_t k, uint8_t n){
  return (int16_t)(a + ((int32_t)(b - a) * k) / n);
}

void fxOpenApp(int16_t ix, int16_t iy, uint8_t is){
  const uint8_t STEPS = 5;
  for(uint8_t k = 1; k <= STEPS; k++){
    int16_t x = lerpi(ix, 0, k, STEPS);
    int16_t y = lerpi(iy, 0, k, STEPS);
    int16_t w = lerpi(is, SCR_W, k, STEPS);
    int16_t h = lerpi(is, SCR_H, k, STEPS);
    int16_t r = lerpi(is / 4, 0, k, STEPS);
    tft.fillRoundRect(x, y, w, h, r, PAGE_BG);
  }
}

void fxCloseApp(){
  // El wallpaper reaparece desde los bordes hacia el centro: es el gesto
  // inverso a fxOpenApp y cuesta cuatro rectangulos por paso.
  const uint8_t STEPS = 5;
  for(uint8_t k = 1; k <= STEPS; k++){
    int16_t w = (int16_t)((int32_t)SCR_W * k / (2 * STEPS));
    int16_t h = (int16_t)((int32_t)SCR_H * k / (2 * STEPS));
    gfxWallRect(0, 0, w, SCR_H);
    gfxWallRect(SCR_W - w, 0, w, SCR_H);
    gfxWallRect(0, 0, SCR_W, h);
    gfxWallRect(0, SCR_H - h, SCR_W, h);
  }
  gfxWallpaper();
}
#undef FX_BAND

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE app_clock.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  app_clock.cpp   (Reloj)
// #############################################################
//
//  Esfera analogica vectorial + hora digital, con el mismo trazo
//  grueso redondeado del reloj de Flex OS Ultra. Las agujas usan
//  una tabla de senos de 60 entradas en PROGMEM (60 B) en vez de
//  sinf/cosf: en AVR eso ahorra la biblioteca matematica entera.
//
//  Solo se repinta la esfera cuando cambia el SEGUNDO, y solo la
//  zona del dial -- el marco, los indices y la fecha se dibujan
//  una vez al entrar.
// #############################################################

#define CK_CX  (SCR_W / 2)
#define CK_CY  180
#define CK_R   96

// 127 * sin(2*pi*i/60)
static const int8_t SIN60[60] PROGMEM = {
     0,   13,   26,   39,   52,   63,   75,   85,   94,  103,
   110,  116,  121,  124,  126,  127,  126,  124,  121,  116,
   110,  103,   94,   85,   75,   63,   52,   39,   26,   13,
     0,  -13,  -26,  -39,  -52,  -63,  -75,  -85,  -94, -103,
  -110, -116, -121, -124, -126, -127, -126, -124, -121, -116,
  -110, -103,  -94,  -85,  -75,  -63,  -52,  -39,  -26,  -13
};

static inline int8_t sin60(uint8_t i){ return (int8_t)pgm_read_byte(&SIN60[i % 60]); }
static inline int8_t cos60(uint8_t i){ return sin60((uint8_t)(i + 15)); }

// Punto sobre la esfera a distancia 'len' del centro, en la marca 'tick'
// (0 = las 12, avanzando en sentido horario).
static void dialPoint(uint8_t tick, int16_t len, int16_t &x, int16_t &y){
  x = CK_CX + (int16_t)(((int32_t)sin60(tick) * len) / 127);
  y = CK_CY - (int16_t)(((int32_t)cos60(tick) * len) / 127);
}

static uint8_t lastSec = 0xFF;

static void drawFace(){
  tft.fillCircle(CK_CX, CK_CY, CK_R + 6, CARD_BG);
  gfxRing(CK_CX, CK_CY, CK_R + 6, 2, BORDER);
  for(uint8_t i = 0; i < 60; i += 5){
    int16_t x0, y0, x1, y1;
    dialPoint(i, CK_R - (i % 15 == 0 ? 16 : 9), x0, y0);
    dialPoint(i, CK_R - 2, x1, y1);
    gfxThickLine(x0, y0, x1, y1, (i % 15 == 0) ? 3 : 1, i % 15 == 0 ? TXT_HI : TXT_MUTE);
  }
}

static void drawHands(){
  // Borra solo el interior del dial (los indices viven fuera de este radio).
  tft.fillCircle(CK_CX, CK_CY, CK_R - 18, CARD_BG);
  uint8_t hTick = (uint8_t)(((gTime.h % 12) * 5 + gTime.m / 12) % 60);
  int16_t x, y;
  dialPoint(hTick, CK_R - 46, x, y);
  gfxThickLine(CK_CX, CK_CY, x, y, 6, TXT_HI);
  dialPoint(gTime.m, CK_R - 28, x, y);
  gfxThickLine(CK_CX, CK_CY, x, y, 4, TXT_HI);
  dialPoint(gTime.s, CK_R - 24, x, y);
  gfxThickLine(CK_CX, CK_CY, x, y, 2, RGB(245,140,30));
  tft.fillCircle(CK_CX, CK_CY, 5, RGB(245,140,30));
}

static void drawDigital(){
  tft.fillRect(0, 300, SCR_W, 60, PAGE_BG);
  char b[40];
  timeStr(b);
  gfxTextC(SCR_W / 2, 304, b, 4, TXT_HI);
  dateLongStr(b);
  gfxTextC(SCR_W / 2, 342, b, 1, TXT_LO);
}

void appClockEnter(){
  uiAppScreen(appNameP(APP_CLOCK));
  drawFace();
  drawHands();
  drawDigital();
  lastSec = gTime.s;
}

void appClockTick(){
  if(gTime.s == lastSec) return;
  lastSec = gTime.s;
  drawHands();
  if(gTime.s == 0) drawDigital();
}
#undef CK_CX
#undef CK_CY
#undef CK_R

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE app_calendar.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  app_calendar.cpp   (Calendario)
// #############################################################
//
//  Rejilla mensual con la cabecera, los flechados de mes y el dia
//  de hoy resaltado en el color de acento, igual que en Flex OS
//  Ultra. Sin eventos: no hay donde guardarlos en 4 KB de EEPROM
//  compartidos con las notas y los ajustes, y una agenda que se
//  pierde al reiniciar seria peor que no tenerla.
// #############################################################

#define CAL_X0   14
#define CAL_CW   ((SCR_W - 2 * CAL_X0) / 7)
#define CAL_HY   96                      // fila de iniciales de dia
#define CAL_Y0   118                      // primera fila de numeros
#define CAL_RH   44

static uint8_t vMon;
static uint16_t vYear;

static void drawHeader(){
  tft.fillRect(0, APP_HDR_H, SCR_W, CAL_Y0 - APP_HDR_H, PAGE_BG);
  char b[20];
  int16_t x = 60;
  x = gfxTextP(x, 56, monthFullP(vMon), 2, TXT_HI);
  char* p = b;
  *p++ = ' ';
  fmtU16(p, vYear);
  gfxText(x, 56, b, 2, TXT_LO);
  // Flechas de mes (izquierda / derecha).
  tft.fillTriangle(34, 54, 34, 70, 22, 62, C_ACCENT);
  tft.fillTriangle(SCR_W - 34, 54, SCR_W - 34, 70, SCR_W - 22, 62, C_ACCENT);
  for(uint8_t d = 0; d < 7; d++){
    char ini[2] = { (char)pgm_read_byte(dayShortP(d)), 0 };
    gfxTextC(CAL_X0 + d * CAL_CW + CAL_CW / 2, CAL_HY, ini, 1,
             (d == 0 || d == 6) ? C_DANGER : TXT_LO);
  }
}

static void calendarDrawGrid(){
  tft.fillRect(0, CAL_Y0, SCR_W, APP_Y1 - CAL_Y0, PAGE_BG);
  uint8_t first = dowOf(vYear, vMon, 1);
  uint8_t dim   = daysInMonth(vYear, vMon);
  for(uint8_t d = 1; d <= dim; d++){
    uint8_t idx = first + d - 1;
    int16_t cx = CAL_X0 + (idx % 7) * CAL_CW + CAL_CW / 2;
    int16_t cy = CAL_Y0 + (idx / 7) * CAL_RH + CAL_RH / 2;
    bool today = (d == gTime.day && vMon == gTime.mon && vYear == gTime.year);
    if(today) tft.fillCircle(cx, cy, 16, C_ACCENT);
    char b[4];
    fmtU16(b, d);
    gfxTextC(cx, cy - 7, b, 2, today ? C_WHITE : TXT_HI);
  }
}

void appCalendarEnter(){
  vMon = gTime.mon;
  vYear = gTime.year;
  uiAppScreen(appNameP(APP_CALENDAR));
  drawHeader();
  calendarDrawGrid();
}

static void stepMonth(int8_t d){
  if(d < 0){ if(vMon == 0){ vMon = 11; vYear--; } else vMon--; }
  else      { if(vMon == 11){ vMon = 0; vYear++; } else vMon++; }
  drawHeader();
  calendarDrawGrid();
}

void appCalendarTick(){
  // Deslizar horizontalmente cambia de mes, como en Flex OS Ultra.
  if(tp.swipeLeft)  { stepMonth(+1); return; }
  if(tp.swipeRight) { stepMonth(-1); return; }
  if(!tp.tap) return;
  if(tp.y >= 46 && tp.y <= 78){
    if(tp.x < 60)          { stepMonth(-1); return; }
    if(tp.x > SCR_W - 60)  { stepMonth(+1); return; }
    // Tocar el titulo vuelve al mes actual.
    if(vMon != gTime.mon || vYear != gTime.year){
      vMon = gTime.mon; vYear = gTime.year;
      drawHeader(); calendarDrawGrid();
    }
  }
}
#undef CAL_X0
#undef CAL_CW
#undef CAL_HY
#undef CAL_Y0
#undef CAL_RH

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE app_calc.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  app_calc.cpp   (Calculadora)
// #############################################################
//
//  Misma disposicion que la calculadora de Flex OS Ultra: visor
//  alineado a la derecha y rejilla 4x5 con la columna de operadores
//  en el naranja del sistema.
//
//  Solo se repinta la TECLA pulsada y el visor: en el bus de 8 bits
//  del Mega, redibujar las veinte teclas por pulsacion se notaria.
// #############################################################

#define CC_X0   12
#define CC_Y0   140
#define CC_W    69
#define CC_H    54
#define CC_CS   75
#define CC_RS   60
#define DISP_Y  56
#define DISP_H  70

// '~' = cambio de signo, '<' = borrar el ultimo digito.
static const char CKEYS[20] PROGMEM = {
  'C','~','%','/',
  '7','8','9','*',
  '4','5','6','-',
  '1','2','3','+',
  '0','.','<','='
};

static char  entry[14];
static float calcAcc     = 0;
static char  pending = 0;
static bool  fresh   = true;    // el visor muestra un resultado, no una entrada

static void keyRect(uint8_t i, int16_t &x, int16_t &y){
  x = CC_X0 + (i % 4) * CC_CS;
  y = CC_Y0 + (i / 4) * CC_RS;
}

static void drawKey(uint8_t i, bool down){
  int16_t x, y;
  keyRect(i, x, y);
  char c = (char)pgm_read_byte(&CKEYS[i]);
  bool oper = (i % 4 == 3) || c == 'C' || c == '~' || c == '%';
  uint16_t bg = oper ? ((i % 4 == 3) ? RGB(245,150,30) : RGB(150,150,155)) : CARD_ALT;
  if(down) bg = mix565(bg, C_WHITE, 90);
  tft.fillRoundRect(x, y, CC_W, CC_H, 14, bg);
  char s[3];
  if(c == '~')      { s[0] = '+'; s[1] = '/'; s[2] = 0; }
  else if(c == '<') { s[0] = '<'; s[1] = '-'; s[2] = 0; }
  else              { s[0] = c;   s[1] = 0; }
  gfxTextC(x + CC_W / 2, y + CC_H / 2 - 10, s, s[1] ? 2 : 3, oper ? C_WHITE : TXT_HI);
}

static void calcDrawDisplay(){
  tft.fillRect(0, DISP_Y, SCR_W, DISP_H, PAGE_BG);
  uint8_t sz = strlen(entry) > 10 ? 2 : 3;
  gfxTextR(SCR_W - 16, DISP_Y + DISP_H - 12 - TXT_H(sz), entry, sz, TXT_HI);
  if(pending) tft.fillCircle(20, DISP_Y + 20, 4, RGB(245,150,30));
}

// Formatea 'v' sin arrastrar el printf de coma flotante (que en AVR pesa 1,5 KB).
static void setEntry(float v){
  dtostrf(v, 0, 6, entry);
  char* dot = strchr(entry, '.');
  if(dot){
    char* e = entry + strlen(entry) - 1;
    while(e > dot && *e == '0') *e-- = 0;
    if(e == dot) *e = 0;
  }
  if(strlen(entry) > 13) entry[13] = 0;
}

static void applyPending(){
  float v = (float)atof(entry);
  switch(pending){
    case '+': calcAcc += v; break;
    case '-': calcAcc -= v; break;
    case '*': calcAcc *= v; break;
    case '/': calcAcc = (v != 0) ? calcAcc / v : 0; break;
    default:  calcAcc = v;  break;
  }
  setEntry(calcAcc);
}

static void press(char c){
  if(c >= '0' && c <= '9'){
    if(fresh){ entry[0] = 0; fresh = false; }
    uint8_t n = strlen(entry);
    if(n < 12 && !(n == 1 && entry[0] == '0')){ entry[n] = c; entry[n + 1] = 0; }
    else if(n == 1 && entry[0] == '0'){ entry[0] = c; }
    return;
  }
  switch(c){
    case 'C': entry[0] = '0'; entry[1] = 0; calcAcc = 0; pending = 0; fresh = true; break;
    case '.':
      if(fresh){ strcpy(entry, "0"); fresh = false; }
      if(!strchr(entry, '.') && strlen(entry) < 12) strcat(entry, ".");
      break;
    case '~':
      if(entry[0] == '-') memmove(entry, entry + 1, strlen(entry));
      else { memmove(entry + 1, entry, strlen(entry) + 1); entry[0] = '-'; }
      break;
    case '<': {
      uint8_t n = strlen(entry);
      if(n > 1) entry[n - 1] = 0;
      else { entry[0] = '0'; entry[1] = 0; fresh = true; }
    } break;
    case '%': setEntry((float)atof(entry) / 100.0f); fresh = true; break;
    case '=':
      applyPending();
      pending = 0;
      fresh = true;
      break;
    default:                                  // + - * /
      applyPending();
      pending = c;
      fresh = true;
      break;
  }
}

void appCalcEnter(){
  uiAppScreen(appNameP(APP_CALC));
  strcpy(entry, "0");
  calcAcc = 0; pending = 0; fresh = true;
  calcDrawDisplay();
  for(uint8_t i = 0; i < 20; i++) drawKey(i, false);
}

void appCalcTick(){
  if(!tp.tap) return;
  for(uint8_t i = 0; i < 20; i++){
    int16_t x, y;
    keyRect(i, x, y);
    if(!hitRect(tp.x, tp.y, x, y, CC_W, CC_H)) continue;
    drawKey(i, true);
    press((char)pgm_read_byte(&CKEYS[i]));
    calcDrawDisplay();
    drawKey(i, false);
    return;
  }
}
#undef CC_X0
#undef CC_Y0
#undef CC_W
#undef CC_H
#undef CC_CS
#undef CC_RS
#undef DISP_Y
#undef DISP_H

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE app_notes.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  app_notes.cpp   (Notas)
// #############################################################
//
//  Tres notas de 96 caracteres guardadas en la EEPROM. Flex OS
//  Ultra escribia ficheros en LittleFS; aqui el "sistema de
//  archivos" son tres ranuras de tamano fijo, que es lo que cabe
//  sin asignacion dinamica y sin fragmentar 4 KB de EEPROM.
// #############################################################

#define NT_ROW_Y  56
#define ED_Y0     50
#define ED_COLS   24
#define ED_LINEH  20

static uint8_t mode = 0;        // 0 = lista, 1 = editor
static uint8_t slot = 0;
static char    notesBuf[NOTE_LEN + 1];
static uint8_t len = 0;

// ---------------- Lista ----------------
static void drawList(){
  uiAppScreen(appNameP(APP_NOTES));
  char prev[NOTE_LEN + 1];
  for(uint8_t i = 0; i < NOTE_SLOTS; i++){
    int16_t y = NT_ROW_Y + i * (ROW_H + 8);
    uiCard(12, y, SCR_W - 24, ROW_H);
    char t[4] = { (char)('1' + i), 0 };
    tft.fillCircle(34, y + ROW_H / 2, 13, C_ACCENT);
    gfxTextC(34, y + ROW_H / 2 - 7, t, 2, C_WHITE);
    if(noteEmpty(i)){
      gfxTextP(60, y + ROW_H / 2 - 4, strP(S_EMPTY), 1, TXT_MUTE);
    } else {
      noteLoad(i, prev);
      if(strlen(prev) > 30) prev[30] = 0;
      gfxText(60, y + ROW_H / 2 - 4, prev, 1, TXT_HI);
    }
    gfxChevron(SCR_W - 32, y + ROW_H / 2, 5, CHEVRON);
  }
  gfxTextCP(SCR_W / 2, NT_ROW_Y + NOTE_SLOTS * (ROW_H + 8) + 20, strP(S_NOTEHINT), 1, TXT_MUTE);
}

// ---------------- Editor ----------------
static void drawText(){
  tft.fillRect(0, ED_Y0, SCR_W, KB_Y - ED_Y0 - 6, PAGE_BG);
  char line[ED_COLS + 1];
  uint8_t row = 0;
  for(uint8_t o = 0; o < len || o == 0; o += ED_COLS){
    uint8_t n = (uint8_t)(len - o);
    if(n > ED_COLS) n = ED_COLS;
    memcpy(line, notesBuf + o, n);
    line[n] = 0;
    gfxText(12, ED_Y0 + 6 + row * ED_LINEH, line, 2, TXT_HI);
    row++;
    if(o + ED_COLS >= len) break;
  }
  // Cursor al final del texto.
  int16_t cx = 12 + (len % ED_COLS) * TXT_ADV(2);
  int16_t cy = ED_Y0 + 6 + (len / ED_COLS) * ED_LINEH;
  tft.fillRect(cx, cy, 2, TXT_H(2) + 2, C_ACCENT);
}

static void drawEditor(){
  tft.fillRect(0, 0, SCR_W, SCR_H, PAGE_BG);
  uiAppHeader(appNameP(APP_NOTES));
  // Contador de caracteres restantes en la cabecera.
  char c[6];
  fmtU16(c, (uint16_t)(NOTE_LEN - len));
  gfxTextR(SCR_W - 14, APP_HDR_H / 2 - 4, c, 1, TXT_MUTE);
  drawText();
  uiKeyboard();
  uiNavBar(false);
}

static void saveAndBack(){
  noteSave(slot, notesBuf);
  mode = 0;
  drawList();
}

void appNotesEnter(){
  mode = 0;
  drawList();
}

void appNotesTick(){
  if(mode == 0){
    if(!tp.tap) return;
    for(uint8_t i = 0; i < NOTE_SLOTS; i++){
      int16_t y = NT_ROW_Y + i * (ROW_H + 8);
      if(!hitRect(tp.x, tp.y, 12, y, SCR_W - 24, ROW_H)) continue;
      slot = i;
      noteLoad(i, notesBuf);
      len = (uint8_t)strlen(notesBuf);
      mode = 1;
      drawEditor();
      return;
    }
    return;
  }

  // --- Editor ---
  if(tp.tap && uiBackHit(tp.x, tp.y)){ saveAndBack(); return; }
  if(!tp.tap) return;
  char c = uiKeyboardHit(tp.x, tp.y);
  if(c == 0) return;
  if(c == 13){ saveAndBack(); return; }
  if(c == 8){
    if(len) notesBuf[--len] = 0;
  } else if(len < NOTE_LEN){
    notesBuf[len++] = c;
    notesBuf[len] = 0;
  }
  drawText();
  char cc[6];
  tft.fillRect(SCR_W - 40, APP_HDR_H / 2 - 6, 26, 10, PAGE_BG);
  fmtU16(cc, (uint16_t)(NOTE_LEN - len));
  gfxTextR(SCR_W - 14, APP_HDR_H / 2 - 4, cc, 1, TXT_MUTE);
}
#undef NT_ROW_Y
#undef ED_Y0
#undef ED_COLS
#undef ED_LINEH

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE app_paint.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  app_paint.cpp   (Paint)
// #############################################################
//
//  Lienzo directo: en el P4 Paint guardaba los trazos en PSRAM y
//  los volcaba a LittleFS. Aqui el lienzo ES la memoria del panel
//  (320x480 en la GRAM del ILI9486), asi que se dibuja sin gastar
//  un byte de SRAM. A cambio no hay "guardar": el dibujo vive
//  mientras la app este abierta, que es lo unico honesto con
//  8 KB de RAM y 4 KB de EEPROM.
// #############################################################

#define PT_BAR_Y   44
#define PT_BAR_H   36
#define PT_CAN_Y   84
#define PT_CAN_H   (APP_Y1 - PT_CAN_Y)
#define PT_SW      26
#define PT_SWSTEP  28

static const uint16_t PALETTE[8] PROGMEM = {
  RGB(30,30,40),   RGB(230,60,60),  RGB(240,150,40), RGB(240,210,50),
  RGB(80,190,90),  RGB(60,190,200), RGB(70,130,235), RGB(245,245,247)
};

static uint8_t  colIdx  = 1;
static uint8_t  brush   = 3;
static int16_t  lastX   = -1, lastY = -1;

static uint16_t curCol(){ return pgm_read_word(&PALETTE[colIdx]); }

static const char PT_CLR[] PROGMEM = "C";

static void drawBar(){
  tft.fillRect(0, PT_BAR_Y, SCR_W, PT_BAR_H, PAGE_BG);
  for(uint8_t i = 0; i < 8; i++){
    int16_t x = 6 + i * PT_SWSTEP;
    tft.fillRoundRect(x, PT_BAR_Y + 4, PT_SW, PT_SW, 6, pgm_read_word(&PALETTE[i]));
    if(i == colIdx) tft.drawRoundRect(x - 2, PT_BAR_Y + 2, PT_SW + 4, PT_SW + 4, 8, C_ACCENT);
  }
  // Dos grosores de trazo y el boton de limpiar.
  for(uint8_t b = 0; b < 2; b++){
    int16_t x = 238 + b * 28;
    tft.fillRoundRect(x, PT_BAR_Y + 4, 24, PT_SW, 6, CARD_ALT);
    tft.fillCircle(x + 12, PT_BAR_Y + 4 + PT_SW / 2, b == 0 ? 3 : 7, TXT_HI);
    if((b == 0 && brush <= 3) || (b == 1 && brush > 3))
      tft.drawRoundRect(x - 2, PT_BAR_Y + 2, 28, PT_SW + 4, 8, C_ACCENT);
  }
  tft.fillRoundRect(294, PT_BAR_Y + 4, 22, PT_SW, 6, C_DANGER);
  gfxTextCP(305, PT_BAR_Y + 4 + PT_SW / 2 - 7, PT_CLR, 2, C_WHITE);
}

static void clearCanvas(){
  tft.fillRect(0, PT_CAN_Y, SCR_W, PT_CAN_H, gDark ? RGB(24,26,34) : C_WHITE);
}

void appPaintEnter(){
  tft.fillRect(0, 0, SCR_W, SCR_H, PAGE_BG);
  uiAppHeader(appNameP(APP_PAINT));
  drawBar();
  clearCanvas();
  uiNavBar(false);
  lastX = lastY = -1;
}

void appPaintTick(){
  // --- Barra de herramientas ---
  if(tp.tap && tp.y >= PT_BAR_Y && tp.y < PT_BAR_Y + PT_BAR_H){
    for(uint8_t i = 0; i < 8; i++){
      int16_t x = 6 + i * PT_SWSTEP;
      if(hitRect(tp.x, tp.y, x, PT_BAR_Y, PT_SW, PT_BAR_H)){ colIdx = i; drawBar(); return; }
    }
    if(hitRect(tp.x, tp.y, 238, PT_BAR_Y, 24, PT_BAR_H)){ brush = 3; drawBar(); return; }
    if(hitRect(tp.x, tp.y, 266, PT_BAR_Y, 24, PT_BAR_H)){ brush = 7; drawBar(); return; }
    if(hitRect(tp.x, tp.y, 294, PT_BAR_Y, 22, PT_BAR_H)){ clearCanvas(); return; }
    return;
  }

  // --- Lienzo ---
  if(!tp.down || tp.y < PT_CAN_Y || tp.y >= APP_Y1){ lastX = lastY = -1; return; }
  uint16_t c = curCol();
  if(lastX >= 0){
    // Traza el segmento entre la muestra anterior y la actual: el tactil
    // resistivo se lee a ~60 Hz y sin esto los trazos rapidos saldrian
    // como una fila de puntos sueltos.
    gfxThickLine(lastX, lastY, tp.x, tp.y, brush * 2, c);
  }
  tft.fillCircle(tp.x, tp.y, brush, c);
  lastX = tp.x;
  lastY = tp.y;
}
#undef PT_BAR_Y
#undef PT_BAR_H
#undef PT_CAN_Y
#undef PT_CAN_H
#undef PT_SW
#undef PT_SWSTEP

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE app_games.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  app_games.cpp   (Juegos)
// #############################################################
//
//  Flex OS Ultra traia dos juegos con motor propio (plataformas y
//  futbol) que ocupaban miles de lineas y decenas de KB de datos.
//  Nada de eso entra en 256 KB de Flash junto al resto del sistema,
//  y los modos por red desaparecen con la radio.
//
//  UltraSingle conserva la app "Juegos" con un clasico que si cabe
//  y se juega bien con tactil resistivo: serpiente, con el record
//  guardado en EEPROM. Se controla deslizando, no con botones, para
//  no robarle espacio al tablero.
// #############################################################

#define G_CELL 20
#define G_COLS 16
#define G_Y0   84
#define G_ROWS 17
#define G_MAX  80
#define G_MS   220UL

static uint8_t  sx[G_MAX], sy[G_MAX];
static uint8_t  slen;
static int8_t   dx, dy;
static uint8_t  fx, fy;
static uint16_t score;
static bool     alive;
static uint32_t lastStep;

static void cellFill(uint8_t c, uint8_t r, uint16_t col, uint8_t inset){
  tft.fillRoundRect(c * G_CELL + inset, G_Y0 + r * G_CELL + inset,
                    G_CELL - 2 * inset, G_CELL - 2 * inset, 4, col);
}

static void drawHud(){
  tft.fillRect(0, APP_HDR_H, SCR_W, G_Y0 - APP_HDR_H, PAGE_BG);
  char b[8];
  int16_t x = gfxTextP(12, 56, strP(S_SCORE), 1, TXT_LO);
  fmtU16(b, score);
  gfxText(x + 8, 52, b, 2, TXT_HI);
  x = gfxTextP(SCR_W - 110, 56, strP(S_BEST), 1, TXT_LO);
  fmtU16(b, gBest);
  gfxText(x + 8, 52, b, 2, C_ACCENT);
}

static void placeFood(){
  // Busca una celda libre a partir de una posicion pseudoaleatoria: sin
  // bucle "while(ocupada)" que pudiera no terminar con el tablero lleno.
  uint16_t start = (uint16_t)(random(G_COLS * G_ROWS));
  for(uint16_t k = 0; k < (uint16_t)(G_COLS * G_ROWS); k++){
    uint16_t p = (start + k) % (G_COLS * G_ROWS);
    uint8_t c = (uint8_t)(p % G_COLS), r = (uint8_t)(p / G_COLS);
    bool busy = false;
    for(uint8_t i = 0; i < slen; i++) if(sx[i] == c && sy[i] == r){ busy = true; break; }
    if(!busy){ fx = c; fy = r; return; }
  }
}

static void newGame(){
  slen = 3;
  for(uint8_t i = 0; i < slen; i++){ sx[i] = 8 - i; sy[i] = 8; }
  dx = 1; dy = 0;
  score = 0;
  alive = true;
  lastStep = millis();
  tft.fillRect(0, G_Y0, SCR_W, G_ROWS * G_CELL, gDark ? RGB(20,22,30) : RGB(232,236,244));
  placeFood();
  cellFill(fx, fy, C_DANGER, 3);
  for(uint8_t i = 0; i < slen; i++) cellFill(sx[i], sy[i], i ? C_OK : C_ACCENT, 1);
  drawHud();
}

static void gameOver(){
  alive = false;
  if(score > gBest){ gBest = score; storeSaveBest(); }
  int16_t y = G_Y0 + (G_ROWS * G_CELL) / 2 - 30;
  tft.fillRoundRect(30, y, SCR_W - 60, 60, 14, CARD_BG);
  gfxTextCP(SCR_W / 2, y + 12, strP(S_GAMEOVER), 2, TXT_HI);
  gfxTextCP(SCR_W / 2, y + 38, strP(S_TAPSTART), 1, TXT_LO);
  drawHud();
}

static void step(){
  int8_t nc = (int8_t)sx[0] + dx;
  int8_t nr = (int8_t)sy[0] + dy;
  if(nc < 0 || nc >= G_COLS || nr < 0 || nr >= G_ROWS){ gameOver(); return; }
  for(uint8_t i = 0; i < slen; i++)
    if(sx[i] == (uint8_t)nc && sy[i] == (uint8_t)nr){ gameOver(); return; }

  bool grew = ((uint8_t)nc == fx && (uint8_t)nr == fy);
  if(!grew){
    // Borra la cola antes de desplazar el cuerpo.
    cellFill(sx[slen - 1], sy[slen - 1], gDark ? RGB(20,22,30) : RGB(232,236,244), 0);
  } else if(slen < G_MAX){
    slen++;
  }
  for(uint8_t i = slen - 1; i > 0; i--){ sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
  sx[0] = (uint8_t)nc; sy[0] = (uint8_t)nr;
  cellFill(sx[1], sy[1], C_OK, 1);
  cellFill(sx[0], sy[0], C_ACCENT, 1);
  if(grew){
    score++;
    drawHud();
    placeFood();
    cellFill(fx, fy, C_DANGER, 3);
  }
}

void appGamesEnter(){
  uiAppScreen(appNameP(APP_GAMES));
  randomSeed(millis() ^ (uint32_t)analogRead(A5));
  newGame();
}

void appGamesTick(){
  if(!alive){
    if(tp.tap && tp.y > G_Y0) newGame();
    return;
  }
  if(tp.swipeLeft  && dx == 0){ dx = -1; dy = 0; }
  else if(tp.swipeRight && dx == 0){ dx = 1; dy = 0; }
  else if(tp.swipeUp    && dy == 0){ dx = 0; dy = -1; }
  else if(tp.swipeDown  && dy == 0){ dx = 0; dy = 1; }

  if(millis() - lastStep < G_MS) return;
  lastStep += G_MS;
  step();
}
#undef G_CELL
#undef G_COLS
#undef G_Y0
#undef G_ROWS
#undef G_MAX
#undef G_MS

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE app_timer.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  app_timer.cpp   (Cronometro)
// #############################################################
//
//  Cronometro con el reloj vectorial grande del sistema. Se repinta
//  a 10 Hz y SOLO la banda de los digitos, no la pantalla entera:
//  a 60 Hz el bus de 8 bits no daria abasto y el contador se
//  quedaria atras.
// #############################################################

#define TM_Y      130
#define TM_H      110
#define TM_BTN_Y  300
#define TM_BTN_H  52

static bool     running = false;
static uint32_t t0      = 0;      // instante de arranque
static uint32_t acc     = 0;      // milisegundos acumulados en pausas
static uint32_t lastDraw = 0;

static uint32_t elapsed(){ return acc + (running ? (millis() - t0) : 0); }

static void drawDigits(){
  uint32_t ms = elapsed();
  uint8_t  mm = (uint8_t)((ms / 60000UL) % 100);
  uint8_t  ss = (uint8_t)((ms / 1000UL) % 60);
  uint8_t  cs = (uint8_t)((ms / 10UL) % 100);
  char b[10];
  char* p = fmtPad2(b, mm);
  *p++ = ':';
  p = fmtPad2(p, ss);
  tft.fillRect(0, TM_Y, SCR_W, TM_H, PAGE_BG);
  gfxBigClock(b, SCR_W / 2, TM_Y + 46, 36, 8, TXT_HI);
  fmtPad2(b, cs);
  gfxTextC(SCR_W / 2, TM_Y + 84, b, 3, C_ACCENT);
}

static void drawButtons(){
  tft.fillRect(0, TM_BTN_Y, SCR_W, TM_BTN_H, PAGE_BG);
  uiButtonP(24, TM_BTN_Y, 130, TM_BTN_H, strP(running ? S_STOP : S_START), true);
  uiButtonP(166, TM_BTN_Y, 130, TM_BTN_H, strP(S_RESET), false);
}

void appTimerEnter(){
  uiAppScreen(appNameP(APP_TIMER));
  running = false;
  acc = 0;
  drawDigits();
  drawButtons();
  lastDraw = millis();
}

void appTimerTick(){
  if(tp.tap){
    if(hitRect(tp.x, tp.y, 24, TM_BTN_Y, 130, TM_BTN_H)){
      if(running){ acc += millis() - t0; running = false; }
      else       { t0 = millis(); running = true; }
      drawButtons();
      drawDigits();
      return;
    }
    if(hitRect(tp.x, tp.y, 166, TM_BTN_Y, 130, TM_BTN_H)){
      running = false;
      acc = 0;
      drawButtons();
      drawDigits();
      return;
    }
  }
  if(!running) return;
  if(millis() - lastDraw < 100) return;
  lastDraw = millis();
  drawDigits();
}
#undef TM_Y
#undef TM_H
#undef TM_BTN_Y
#undef TM_BTN_H

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE app_light.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  app_light.cpp   (Linterna)
// #############################################################
//
//  El shield MAR3501 lleva la retroiluminacion cableada fija: no
//  hay PWM que regular. Asi que el "brillo" se consigue con el
//  propio panel, pintandolo de blanco o de gris -- que es lo que
//  de verdad cambia la luz que sale de la pantalla.
//
//  Es la unica app a pantalla completa (AF_FULLSCREEN): oculta las
//  barras del sistema y se sale tocando la franja inferior.
// #############################################################

static const uint8_t LEVELS[3] PROGMEM = { 255, 170, 90 };
static uint8_t level = 0;

static const char LT_HINT[] PROGMEM = "Toca abajo para salir";

static void paint(){
  uint8_t v = pgm_read_byte(&LEVELS[level]);
  tft.fillScreen(RGB(v, v, v));
  gfxTextCP(SCR_W / 2, SCR_H - 30, LT_HINT, 1, RGB(v / 2, v / 2, v / 2));
  // Tres puntos que indican el nivel activo.
  for(uint8_t i = 0; i < 3; i++)
    tft.fillCircle(SCR_W / 2 - 16 + i * 16, SCR_H - 52, 4,
                   i == level ? RGB(60,130,246) : RGB(v * 3 / 4, v * 3 / 4, v * 3 / 4));
}

void appLightEnter(){
  level = 0;
  paint();
}

void appLightTick(){
  if(!tp.tap) return;
  if(tp.y > SCR_H - 70){ shellGoHome(); return; }
  level = (uint8_t)((level + 1) % 3);
  paint();
}

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE app_system.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  app_system.cpp
// ##  Bienestar, Memoria e Informacion del sistema
// #############################################################
//
//  Tres aplicaciones pequenas de solo lectura en un unico modulo:
//  comparten las mismas primitivas (fila de dato y barra de uso) y
//  separarlas en tres ficheros solo duplicaria codigo.
//
//  Todos los numeros son REALES, medidos en el propio equipo: SRAM
//  libre por el hueco entre el heap y la pila, Flash por el simbolo
//  del enlazador, EEPROM por el mapa fijo de fos_store.
// #############################################################

// Fin de la imagen en Flash (codigo + datos inicializados): lo publica el
// enlazador de avr-gcc, asi que el dato es exacto para esta compilacion.
extern char __data_load_end;

#define SY_Y0 56

// ---------------- Primitivas compartidas ----------------
static void statRow(int16_t y, PGM_P label, const char* value){
  uiCard(12, y, SCR_W - 24, ROW_H - 6);
  gfxTextP(26, y + (ROW_H - 6) / 2 - 7, label, 2, TXT_HI);
  gfxTextR(SCR_W - 26, y + (ROW_H - 6) / 2 - 4, value, 1, TXT_LO);
}

static void statRowP(int16_t y, PGM_P label, PGM_P value){
  uiCard(12, y, SCR_W - 24, ROW_H - 6);
  gfxTextP(26, y + (ROW_H - 6) / 2 - 7, label, 2, TXT_HI);
  gfxTextP(SCR_W - 26 - gfxTextWP(value, 1), y + (ROW_H - 6) / 2 - 4, value, 1, TXT_LO);
}

static void usageBar(int16_t y, PGM_P label, uint32_t used, uint32_t total, uint16_t col){
  uiCard(12, y, SCR_W - 24, 56);
  gfxTextP(26, y + 10, label, 2, TXT_HI);
  // "usado/total" en la unidad que evita truncar: bytes si el total cabe en
  // 16 bits, kilobytes si no (la Flash del Mega son 248 KB utiles).
  char b[24];
  uint32_t u = used, t = total;
  char unit = 'B';
  if(t > 9999){ u >>= 10; t >>= 10; unit = 'K'; }
  char* p = fmtU16(b, (uint16_t)u);
  *p++ = '/';
  p = fmtU16(p, (uint16_t)t);
  *p++ = unit;
  *p = 0;
  gfxTextR(SCR_W - 26, y + 12, b, 1, TXT_LO);
  int16_t bw = SCR_W - 24 - 28;
  uint8_t pc = total ? (uint8_t)((uint32_t)used * 100 / total) : 0;
  if(pc > 100) pc = 100;
  tft.fillRoundRect(26, y + 34, bw, 8, 4, gDark ? RGB(50,56,72) : RGB(214,220,232));
  if(pc) tft.fillRoundRect(26, y + 34, (int16_t)((int32_t)bw * pc / 100), 8, 4, col);
}

// #############################################################
//  Bienestar
// #############################################################
static const char BW_SESS[] PROGMEM = "Sesi\xA2n";
static const char BW_MIN[]  PROGMEM = "min";

void appWellEnter(){
  uiAppScreen(appNameP(APP_WELL));
  char b[24];
  uint32_t up = uptimeSec();
  char* p = fmtU16(b, (uint16_t)(up / 3600));
  *p++ = 'h'; *p++ = ' ';
  p = fmtU16(p, (uint16_t)((up / 60) % 60));
  *p++ = 'm'; *p = 0;
  statRow(SY_Y0, BW_SESS, b);

  p = fmtU16(b, (uint16_t)(gMinutes > 65535 ? 65535 : gMinutes));
  *p++ = ' ';
  strcpy_P(p, BW_MIN);
  statRow(SY_Y0 + ROW_H + 6, strP(S_TOTAL), b);

  fmtU16(b, (uint16_t)(gTouches > 65535 ? 65535 : gTouches));
  statRow(SY_Y0 + 2 * (ROW_H + 6), strP(S_TOUCHES), b);

  // Uso del dia frente a una referencia de 4 horas, en la misma barra que
  // usa Memoria: el objetivo es que se lea de un vistazo, no ser exacto.
  usageBar(SY_Y0 + 3 * (ROW_H + 6) + 8, strP(S_UPTIME), up / 60, 240, C_OK);
}

void appWellTick(){}

// #############################################################
//  Memoria
// #############################################################
void appMemoryEnter(){
  uiAppScreen(appNameP(APP_MEMORY));
  uint16_t fr = freeRam();
  usageBar(SY_Y0,           strP(S_RAM),    (uint32_t)(8192 - fr), 8192UL,   C_ACCENT);
  usageBar(SY_Y0 + 66,      strP(S_FLASH),  (uint32_t)&__data_load_end, 253952UL, C_WARN);
  usageBar(SY_Y0 + 132,     strP(S_EEPROM), (uint32_t)EE_END, 4096UL,   C_OK);

  char b[16];
  fmtU16(b, fr);
  statRow(SY_Y0 + 204, strP(S_FREE), b);
}

void appMemoryTick(){}

// #############################################################
//  Informacion del sistema
// #############################################################
static const char SI_OS[]    PROGMEM = FOS_NAME;
static const char SI_BRD[]   PROGMEM = FOS_BOARD;
static const char SI_PAN[]   PROGMEM = FOS_PANEL;
static const char SI_VER[]   PROGMEM = FOS_VERSION;
static const char SI_MCU[]   PROGMEM = "MCU";
static const char SI_MCUV[]  PROGMEM = "ATmega2560 16 MHz";
static const char SI_RES[]   PROGMEM = "Resoluci\xA2n";
static const char SI_RESV[]  PROGMEM = "320x480";
static const char SI_BUS[]   PROGMEM = "Bus";
static const char SI_BUSV[]  PROGMEM = "8 bits paralelo";
static const char SI_TCH[]   PROGMEM = "T\xA0" "ctil";
static const char SI_TCHV[]  PROGMEM = "Resistivo 4 hilos";

void appSysinfoEnter(){
  uiAppScreen(appNameP(APP_SYSINFO));
  gfxTextCP(SCR_W / 2, 52, SI_OS, 2, TXT_HI);
  gfxTextCP(SCR_W / 2, 74, SI_VER, 1, C_ACCENT);
  int16_t y = 100;
  statRowP(y, strP(S_BOARD), SI_BRD);            y += ROW_H;
  statRowP(y, SI_MCU,        SI_MCUV);           y += ROW_H;
  statRowP(y, strP(S_PANEL), SI_PAN);            y += ROW_H;
  statRowP(y, SI_RES,        SI_RESV);           y += ROW_H;
  statRowP(y, SI_BUS,        SI_BUSV);           y += ROW_H;
  statRowP(y, SI_TCH,        SI_TCHV);           y += ROW_H;
  char b[16];
  fmtU16(b, freeRam());
  statRow(y, strP(S_RAM), b);
}

void appSysinfoTick(){}
#undef SY_Y0

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE app_settings.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  app_settings.cpp   (Ajustes)
// #############################################################
//
//  La app Ajustes de Flex OS Ultra tenia doce categorias, muchas de
//  ellas dedicadas a hardware que aqui no existe (Wi-Fi, Bluetooth,
//  OTA, Modo PC, camara). UltraSingle conserva la estructura -- un
//  indice de categorias y paginas con filas de tarjeta -- con las
//  cinco que si tienen sentido en un Mega.
// #############################################################

enum { PG_INDEX = 0, PG_DISPLAY, PG_LOCK, PG_LANG, PG_TIME, PG_ABOUT };

#define ST_Y0   56
#define ST_STEP (ROW_H + 6)

static uint8_t page = PG_INDEX;

static const char AL_0[] PROGMEM = "30 s";
static const char AL_1[] PROGMEM = "1 min";
static const char AL_2[] PROGMEM = "5 min";
static const char AL_3[] PROGMEM = "10 min";
static const char* const AL_TBL[AUTOLOCK_OPTIONS - 1] PROGMEM = { AL_0, AL_1, AL_2, AL_3 };

static PGM_P autoLockName(){
  if(gAutoLockIdx >= AUTOLOCK_OPTIONS - 1) return strP(S_NEVER);
  return (PGM_P)pgm_read_word(&AL_TBL[gAutoLockIdx]);
}

static const char SET_GEST[]  PROGMEM = "Barra de gestos";
static const char SET_RESET[] PROGMEM = "Restablecer";
static const char SET_PINON[] PROGMEM = "****";

// -------------------------------------------------------------
//  Fila con controles - / +
// -------------------------------------------------------------
static void adjRow(int16_t y, PGM_P label, const char* value){
  uiCard(12, y, SCR_W - 24, ROW_H - 6);
  gfxTextP(26, y + (ROW_H - 6) / 2 - 7, label, 2, TXT_HI);
  int16_t cy = y + (ROW_H - 6) / 2;
  tft.fillCircle(SCR_W - 88, cy, 14, CARD_ALT);
  tft.fillRect(SCR_W - 95, cy - 1, 14, 3, TXT_HI);
  tft.fillCircle(SCR_W - 30, cy, 14, CARD_ALT);
  tft.fillRect(SCR_W - 37, cy - 1, 14, 3, TXT_HI);
  tft.fillRect(SCR_W - 32, cy - 7, 3, 14, TXT_HI);
  gfxTextC(SCR_W - 59, cy - 4, value, 1, TXT_LO);
}

static int8_t adjHit(int16_t y){
  int16_t cy = y + (ROW_H - 6) / 2;
  if(tp.y < cy - 16 || tp.y > cy + 16) return 0;
  if(tp.x > SCR_W - 106 && tp.x < SCR_W - 72) return -1;
  if(tp.x > SCR_W - 48  && tp.x < SCR_W - 12) return 1;
  return 0;
}

// -------------------------------------------------------------
//  Paginas
// -------------------------------------------------------------
static void drawIndex(){
  uiAppScreen(strP(S_SETTINGS));
  static const uint8_t IDS[5] = { S_DISPLAY, S_LOCKSCREEN, S_LANGUAGE, S_DATETIME, S_ABOUT };
  for(uint8_t i = 0; i < 5; i++)
    uiRow(ST_Y0 + i * ST_STEP, strP(IDS[i]), 0, true);
}

static void drawDisplay(){
  uiAppScreen(strP(S_DISPLAY));
  uiRowToggle(ST_Y0,               strP(S_DARKMODE), gDark);
  uiRowToggle(ST_Y0 + ST_STEP,     strP(S_GLASS),    gGlass);
  uiRowP(ST_Y0 + 2 * ST_STEP,      strP(S_ICONSTYLE), strP(gIconGlass ? S_GLASSY : S_FLAT), true);
  uiRowToggle(ST_Y0 + 3 * ST_STEP, SET_GEST,         gNavGest);
}

static void drawLock(){
  uiAppScreen(strP(S_LOCKSCREEN));
  uiRowP(ST_Y0,           strP(S_PIN),      gPinSet ? SET_PINON : strP(S_NOPIN), true);
  uiRowP(ST_Y0 + ST_STEP, strP(S_AUTOLOCK), autoLockName(), true);
}

static void drawLang(){
  uiAppScreen(strP(S_LANGUAGE));
  static const char L_ES[] PROGMEM = "Espa\xA4ol";
  static const char L_EN[] PROGMEM = "English";
  for(uint8_t i = 0; i < 2; i++){
    int16_t y = ST_Y0 + i * ST_STEP;
    uiCard(12, y, SCR_W - 24, ROW_H - 6);
    gfxTextP(26, y + (ROW_H - 6) / 2 - 7, i == 0 ? L_ES : L_EN, 2, TXT_HI);
    if(gLang == i) tft.fillCircle(SCR_W - 34, y + (ROW_H - 6) / 2, 8, C_ACCENT);
    else           gfxRing(SCR_W - 34, y + (ROW_H - 6) / 2, 8, 2, CHEVRON);
  }
}

static void drawTime(){
  uiAppScreen(strP(S_DATETIME));
  char b[8];
  uiRowToggle(ST_Y0, strP(S_H24), gH24);
  fmtPad2(b, gTime.h);   adjRow(ST_Y0 + ST_STEP,     strP(S_HOUR),   b);
  fmtPad2(b, gTime.m);   adjRow(ST_Y0 + 2 * ST_STEP, strP(S_MINUTE), b);
  fmtU16(b, gTime.day);  adjRow(ST_Y0 + 3 * ST_STEP, strP(S_DAY),    b);
  fmtU16(b, gTime.mon + 1); adjRow(ST_Y0 + 4 * ST_STEP, strP(S_MONTH), b);
  fmtU16(b, gTime.year); adjRow(ST_Y0 + 5 * ST_STEP, strP(S_YEAR),   b);
}

static void drawAbout(){
  uiAppScreen(strP(S_ABOUT));
  static const char A_OS[]  PROGMEM = FOS_NAME;
  static const char A_VER[] PROGMEM = FOS_VERSION;
  static const char A_BRD[] PROGMEM = FOS_BOARD;
  gfxTextCP(SCR_W / 2, 70, A_OS, 2, TXT_HI);
  gfxTextCP(SCR_W / 2, 96, A_VER, 1, C_ACCENT);
  gfxTextCP(SCR_W / 2, 120, A_BRD, 1, TXT_LO);
  uiButtonP(40, 200, SCR_W - 80, 50, SET_RESET, false);
}

static void redraw(){
  switch(page){
    case PG_DISPLAY: drawDisplay(); break;
    case PG_LOCK:    drawLock();    break;
    case PG_LANG:    drawLang();    break;
    case PG_TIME:    drawTime();    break;
    case PG_ABOUT:   drawAbout();   break;
    default:         drawIndex();   break;
  }
}

void appSettingsEnter(){
  page = PG_INDEX;
  redraw();
}

// -------------------------------------------------------------
//  Interaccion
// -------------------------------------------------------------
static void tickIndex(){
  static const uint8_t PGS[5] = { PG_DISPLAY, PG_LOCK, PG_LANG, PG_TIME, PG_ABOUT };
  for(uint8_t i = 0; i < 5; i++){
    if(hitRect(tp.x, tp.y, 12, ST_Y0 + i * ST_STEP, SCR_W - 24, ROW_H - 6)){
      page = PGS[i];
      redraw();
      return;
    }
  }
}

static void tickDisplay(){
  if(hitRect(tp.x, tp.y, 12, ST_Y0, SCR_W - 24, ROW_H - 6)){
    gDark = !gDark; storeSaveFlags(); homeInvalidate(); redraw(); return;
  }
  if(hitRect(tp.x, tp.y, 12, ST_Y0 + ST_STEP, SCR_W - 24, ROW_H - 6)){
    gGlass = !gGlass; storeSaveFlags(); homeInvalidate(); redraw(); return;
  }
  if(hitRect(tp.x, tp.y, 12, ST_Y0 + 2 * ST_STEP, SCR_W - 24, ROW_H - 6)){
    gIconGlass = !gIconGlass; storeSaveFlags(); homeInvalidate(); redraw(); return;
  }
  if(hitRect(tp.x, tp.y, 12, ST_Y0 + 3 * ST_STEP, SCR_W - 24, ROW_H - 6)){
    gNavGest = !gNavGest; storeSaveFlags(); homeInvalidate(); redraw(); return;
  }
}

static void tickLock(){
  if(hitRect(tp.x, tp.y, 12, ST_Y0, SCR_W - 24, ROW_H - 6)){
    if(gPinSet){ gPinSet = false; storeSavePin(); redraw(); }
    else       { shellRequestAuth(AUTH_SETPIN); }
    return;
  }
  if(hitRect(tp.x, tp.y, 12, ST_Y0 + ST_STEP, SCR_W - 24, ROW_H - 6)){
    gAutoLockIdx = (uint8_t)((gAutoLockIdx + 1) % AUTOLOCK_OPTIONS);
    storeSaveAutoLock();
    redraw();
  }
}

static void tickLang(){
  for(uint8_t i = 0; i < 2; i++){
    if(hitRect(tp.x, tp.y, 12, ST_Y0 + i * ST_STEP, SCR_W - 24, ROW_H - 6)){
      gLang = i;
      storeSaveLang();
      homeInvalidate();
      redraw();
      return;
    }
  }
}

static void tickTime(){
  if(hitRect(tp.x, tp.y, 12, ST_Y0, SCR_W - 24, ROW_H - 6)){
    gH24 = !gH24; storeSaveFlags(); homeInvalidate(); redraw(); return;
  }
  int8_t d;
  uint8_t h = gTime.h, m = gTime.m, day = gTime.day, mon = gTime.mon;
  uint16_t yr = gTime.year;
  if((d = adjHit(ST_Y0 + ST_STEP)))     h   = (uint8_t)((h + 24 + d) % 24);
  else if((d = adjHit(ST_Y0 + 2 * ST_STEP))) m = (uint8_t)((m + 60 + d) % 60);
  else if((d = adjHit(ST_Y0 + 3 * ST_STEP))){
    uint8_t dm = daysInMonth(yr, mon);
    day = (uint8_t)(day + d);
    if(day < 1) day = dm;
    if(day > dm) day = 1;
  }
  else if((d = adjHit(ST_Y0 + 4 * ST_STEP))) mon = (uint8_t)((mon + 12 + d) % 12);
  else if((d = adjHit(ST_Y0 + 5 * ST_STEP))) yr  = (uint16_t)(yr + d);
  else return;
  timeSet(h, m, day, mon, yr);
  storeSaveTime();
  homeInvalidate();
  redraw();
}

static void tickAbout(){
  if(hitRect(tp.x, tp.y, 40, 200, SCR_W - 80, 50)){
    storeFactoryReset();
    homeInvalidate();
    shellGoHome();
  }
}

void appSettingsTick(){
  if(!tp.tap) return;
  if(uiBackHit(tp.x, tp.y)){
    if(page == PG_INDEX) return;         // la barra de navegacion cierra la app
    page = PG_INDEX;
    redraw();
    return;
  }
  switch(page){
    case PG_DISPLAY: tickDisplay(); break;
    case PG_LOCK:    tickLock();    break;
    case PG_LANG:    tickLang();    break;
    case PG_TIME:    tickTime();    break;
    case PG_ABOUT:   tickAbout();   break;
    default:         tickIndex();   break;
  }
}
#undef ST_Y0
#undef ST_STEP

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_apps.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_apps.cpp
// ##  Tabla de aplicaciones (PROGMEM)
// #############################################################

const FApp APPS[APP_N] PROGMEM = {
  { appClockEnter,    appClockTick,    AF_NONE       },  // APP_CLOCK
  { appCalendarEnter, appCalendarTick, AF_NONE       },  // APP_CALENDAR
  { appCalcEnter,     appCalcTick,     AF_NONE       },  // APP_CALC
  { appNotesEnter,    appNotesTick,    AF_NONE       },  // APP_NOTES
  { appPaintEnter,    appPaintTick,    AF_NONE       },  // APP_PAINT
  { appGamesEnter,    appGamesTick,    AF_NONE       },  // APP_GAMES
  { appTimerEnter,    appTimerTick,    AF_NONE       },  // APP_TIMER
  { appLightEnter,    appLightTick,    AF_FULLSCREEN },  // APP_LIGHT
  { appWellEnter,     appWellTick,     AF_NONE       },  // APP_WELL
  { appMemoryEnter,   appMemoryTick,   AF_NONE       },  // APP_MEMORY
  { appSysinfoEnter,  appSysinfoTick,  AF_NONE       },  // APP_SYSINFO
  { appSettingsEnter, appSettingsTick, AF_NONE       }   // APP_SETTINGS
};

static uint8_t curApp = 0xFF;

uint8_t appCurrent(){ return curApp; }

uint8_t appFlags(uint8_t id){
  if(id >= APP_N) return AF_NONE;
  return pgm_read_byte(&APPS[id].flags);
}

void appOpen(uint8_t id){
  if(id >= APP_N) return;
  curApp = id;
  FAppFn fn = (FAppFn)pgm_read_word(&APPS[id].enter);
  if(fn) fn();
}

void appTick(){
  if(curApp >= APP_N) return;
  FAppFn fn = (FAppFn)pgm_read_word(&APPS[curApp].tick);
  if(fn) fn();
}

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_home.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_home.cpp
// #############################################################
//
//  La misma composicion de Flex OS Ultra -- barra superior, dos
//  widgets, rejilla 4x3, dock de cuatro accesos y barra de
//  navegacion -- reescalada de 480x800 a 320x480.
//
//  Los widgets muestran DATOS REALES del equipo. En el P4 eran
//  maquetas de clima y noticias porque alli habia Wi-Fi previsto;
//  en un Mega sin radio, un widget de clima solo podria mentir, asi
//  que ocupan su mismo hueco con lo que el sistema si sabe: memoria
//  libre y la primera nota guardada.
// #############################################################

// ---- Geometria ----
#define WG_Y    40
#define WG_H    76
#define WG_W    142
#define WG_XL   14
#define WG_XR   164

#define GR_X0   20
#define GR_Y0   128
#define GR_S    52
#define GR_CSTEP 76
#define GR_RSTEP 76

#define DOT_Y   362

#define DK_X    20
#define DK_Y    372
#define DK_W    280
#define DK_H    62
#define DK_S    44

static const uint8_t DOCK_APPS[DOCK_SLOTS] PROGMEM = {
  APP_SETTINGS, APP_CALC, APP_NOTES, APP_PAINT
};

static bool    edit     = false;   // Modo Edicion (reordenar iconos)
static int8_t  editSel  = -1;      // ranura seleccionada
static bool    qsOpen   = false;   // panel rapido desplegado
static bool    dirty    = true;

void homeInvalidate(){ dirty = true; }

// -------------------------------------------------------------
//  Widgets
// -------------------------------------------------------------
static const char W_MEM[]  PROGMEM = "Memoria";
static const char W_MEMB[] PROGMEM = "B libres";

static void drawWidgets(){
  // Izquierda: memoria libre, con barra de ocupacion.
  if(gGlass) glassRect(WG_XL, WG_Y, WG_W, WG_H, 14, C_ACCENT_DK, 150);
  else       tft.fillRoundRect(WG_XL, WG_Y, WG_W, WG_H, 14, C_ACCENT_DK);
  gfxTextP(WG_XL + 12, WG_Y + 10, W_MEM, 1, C_ON_WALL);
  char b[12];
  uint16_t fr = freeRam();
  fmtU16(b, fr);
  gfxText(WG_XL + 12, WG_Y + 26, b, 2, C_ON_WALL);
  gfxTextP(WG_XL + 12, WG_Y + 46, W_MEMB, 1, C_ON_WALL_LO);
  uint8_t pc = (uint8_t)(((uint32_t)fr * 100) / 8192);
  if(pc > 100) pc = 100;
  tft.fillRoundRect(WG_XL + 12, WG_Y + WG_H - 14, WG_W - 24, 5, 2, RGB(20,30,60));
  tft.fillRoundRect(WG_XL + 12, WG_Y + WG_H - 14, (WG_W - 24) * pc / 100, 5, 2, C_OK);

  // Derecha: primera nota guardada.
  if(gGlass) glassRect(WG_XR, WG_Y, WG_W, WG_H, 14, GLASS_CARD, 120);
  else       tft.fillRoundRect(WG_XR, WG_Y, WG_W, WG_H, 14, RGB(46,52,70));
  gfxTextP(WG_XR + 12, WG_Y + 10, appNameP(APP_NOTES), 1, C_ON_WALL);
  if(noteEmpty(0)){
    gfxTextCP(WG_XR + WG_W / 2, WG_Y + WG_H / 2 - 3, strP(S_EMPTY), 1, C_ON_WALL_LO);
  } else {
    char n[NOTE_LEN + 1];
    noteLoad(0, n);
    // Dos lineas de 21 caracteres: lo que cabe a tamano 1 en la tarjeta.
    char line[22];
    for(uint8_t r = 0; r < 2; r++){
      uint8_t o = r * 21;
      if(o >= strlen(n)) break;
      strncpy(line, n + o, 21);
      line[21] = 0;
      gfxText(WG_XR + 12, WG_Y + 30 + r * 12, line, 1, C_ON_WALL_LO);
    }
  }
}

// -------------------------------------------------------------
//  Rejilla y dock
// -------------------------------------------------------------
static void slotXY(uint8_t i, int16_t &x, int16_t &y){
  x = GR_X0 + (i % 4) * GR_CSTEP;
  y = GR_Y0 + (i / 4) * GR_RSTEP;
}

static void drawSlot(uint8_t i){
  int16_t x, y;
  slotXY(i, x, y);
  gfxWallRect(x - 4, y - 4, GR_S + 8, GR_S + 24);
  if(edit && editSel == (int8_t)i)
    tft.drawRoundRect(x - 4, y - 4, GR_S + 8, GR_S + 8, 14, C_WHITE);
  drawAppIcon(gHomeOrder[i], x, y, GR_S);
  gfxTextCP(x + GR_S / 2, y + GR_S + 5, appNameP(gHomeOrder[i]), 1, C_ON_WALL);
}

static void drawGrid(){ for(uint8_t i = 0; i < HOME_SLOTS; i++) drawSlot(i); }

static void drawDock(){
  if(gGlass) glassRect(DK_X, DK_Y, DK_W, DK_H, 20, GLASS_DOCK, 90);
  else       tft.fillRoundRect(DK_X, DK_Y, DK_W, DK_H, 20, RGB(40,46,64));
  for(uint8_t i = 0; i < DOCK_SLOTS; i++)
    drawAppIcon(pgm_read_byte(&DOCK_APPS[i]), DK_X + 12 + i * 70, DK_Y + (DK_H - DK_S) / 2, DK_S);
}

// -------------------------------------------------------------
//  Panel rapido (cortina)
// -------------------------------------------------------------
//  Es la cortina de Flex OS Ultra reducida a lo que el Mega puede
//  ofrecer de verdad: apariencia, vidrio, linterna y bloquear.
#define QS_Y 0
#define QS_H 210

static const char QS_T[] PROGMEM = "Panel r\xA0pido";
static const char QS_LOCK[] PROGMEM = "Bloquear";
static const char QS_LIGHT[] PROGMEM = "Linterna";

static void drawQuick(){
  glassRect(8, QS_Y + 8, SCR_W - 16, QS_H, 22, GLASS_CLOCK, 190);
  gfxTextP(28, QS_Y + 24, QS_T, 2, C_ON_WALL);
  int16_t y = QS_Y + 56;
  gfxTextP(28, y + 4, strP(S_DARKMODE), 1, C_ON_WALL);
  uiToggle(SCR_W - 80, y, gDark);
  y += 34;
  gfxTextP(28, y + 4, strP(S_GLASS), 1, C_ON_WALL);
  uiToggle(SCR_W - 80, y, gGlass);
  y += 34;
  gfxTextP(28, y + 4, QS_LIGHT, 1, C_ON_WALL);
  gfxChevron(SCR_W - 60, y + 10, 6, C_ON_WALL);
  y += 34;
  gfxTextP(28, y + 4, QS_LOCK, 1, C_ON_WALL);
  gfxChevron(SCR_W - 60, y + 10, 6, C_ON_WALL);
  drawHomeIndicator(QS_Y + QS_H + 8, C_ON_WALL);
}

static void closeQuick(){
  qsOpen = false;
  gfxWallRect(0, 0, SCR_W, QS_Y + QS_H + 16);
  uiStatusBar(true);
  drawWidgets();
  for(uint8_t i = 0; i < 4; i++) drawSlot(i);          // primera fila de la rejilla
  for(uint8_t i = 4; i < 8; i++) drawSlot(i);
}

// -------------------------------------------------------------
//  Pintado completo
// -------------------------------------------------------------
void homeEnter(){
  edit = false; editSel = -1; qsOpen = false; dirty = false;
  gfxWallpaper();
  uiStatusBar(true);
  drawWidgets();
  drawGrid();
  tft.fillCircle(SCR_W / 2, DOT_Y, 3, C_WHITE);
  drawDock();
  uiNavBar(true);
}

// -------------------------------------------------------------
//  Interaccion
// -------------------------------------------------------------
static bool hitIcon(int16_t px, int16_t py, uint8_t &slot, uint8_t &app,
                    int16_t &ix, int16_t &iy, uint8_t &is){
  for(uint8_t i = 0; i < HOME_SLOTS; i++){
    int16_t x, y;
    slotXY(i, x, y);
    if(hitRect(px, py, x - 6, y - 4, GR_S + 12, GR_S + 18)){
      slot = i; app = gHomeOrder[i]; ix = x; iy = y; is = GR_S;
      return true;
    }
  }
  for(uint8_t i = 0; i < DOCK_SLOTS; i++){
    int16_t x = DK_X + 12 + i * 70, y = DK_Y + (DK_H - DK_S) / 2;
    if(hitRect(px, py, x, y, DK_S, DK_S)){
      slot = 0xFF; app = pgm_read_byte(&DOCK_APPS[i]); ix = x; iy = y; is = DK_S;
      return true;
    }
  }
  return false;
}

void homeTick(){
  if(dirty){ homeEnter(); return; }
  if(gMinuteTick && !qsOpen) uiStatusClock(true);

  // --- Panel rapido ---
  if(qsOpen){
    if(tp.tap){
      int16_t y = QS_Y + 56;
      if(hitRect(tp.x, tp.y, 20, y - 4, SCR_W - 40, 30)){
        gDark = !gDark; storeSaveFlags(); drawQuick();
      } else if(hitRect(tp.x, tp.y, 20, y + 30, SCR_W - 40, 30)){
        gGlass = !gGlass; storeSaveFlags(); drawQuick();
      } else if(hitRect(tp.x, tp.y, 20, y + 64, SCR_W - 40, 30)){
        closeQuick(); shellOpenApp(APP_LIGHT, SCR_W / 2 - 26, 200, 52);
      } else if(hitRect(tp.x, tp.y, 20, y + 98, SCR_W - 40, 30)){
        closeQuick(); shellGoLock();
      } else if(tp.y > QS_Y + QS_H + 16){
        closeQuick();
      }
    } else if(tp.swipeUp){
      closeQuick();
    }
    return;
  }
  if(tp.swipeDown && tp.startY < 60 && !edit){
    qsOpen = true;
    drawQuick();
    return;
  }

  // --- Barra de navegacion ---
  if(tp.tap){
    int8_t nav = uiNavHit(tp.x, tp.y);
    if(nav == 0 || nav == 1){
      if(edit){ edit = false; editSel = -1; drawGrid(); }
      return;
    }
    if(nav == 2) return;                 // no hay conmutador de tareas: una app a la vez
  }

  // --- Modo Edicion: mantener pulsado un icono ---
  if(tp.longPress){
    uint8_t slot, app, is; int16_t ix, iy;
    if(hitIcon(tp.x, tp.y, slot, app, ix, iy, is) && slot != 0xFF){
      edit = true;
      int8_t prev = editSel;
      editSel = (int8_t)slot;
      if(prev >= 0) drawSlot((uint8_t)prev);
      drawSlot(slot);
      touchConsume();
    }
    return;
  }

  if(!tp.tap) return;

  uint8_t slot, app, is; int16_t ix, iy;
  if(!hitIcon(tp.x, tp.y, slot, app, ix, iy, is)){
    if(edit){ edit = false; int8_t s = editSel; editSel = -1; if(s >= 0) drawSlot((uint8_t)s); }
    return;
  }

  if(edit){
    if(slot == 0xFF) return;                       // el dock no se reordena
    if(editSel >= 0 && editSel != (int8_t)slot){
      uint8_t t = gHomeOrder[editSel];
      gHomeOrder[editSel] = gHomeOrder[slot];
      gHomeOrder[slot] = t;
      storeSaveHomeOrder();
      uint8_t a = (uint8_t)editSel;
      editSel = -1;
      edit = false;
      drawSlot(a);
      drawSlot(slot);
    } else {
      edit = false;
      editSel = -1;
      drawSlot(slot);
    }
    return;
  }

  shellOpenApp(app, ix, iy, is);
}
#undef WG_Y
#undef WG_H
#undef WG_W
#undef WG_XL
#undef WG_XR
#undef GR_X0
#undef GR_Y0
#undef GR_S
#undef GR_CSTEP
#undef GR_RSTEP
#undef DOT_Y
#undef DK_X
#undef DK_Y
#undef DK_W
#undef DK_H
#undef DK_S
#undef QS_Y
#undef QS_H

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_lock.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_lock.cpp
// #############################################################
//
//  La pantalla de bloqueo de Flex OS Ultra: reloj vectorial grande
//  sobre panel de vidrio, fecha larga, tarjetas de widget y la
//  pildora inferior con "Desliza para desbloquear".
//
//  Se conserva el contador de fallos con espera progresiva del
//  original (LOCK_FAILS_SOFT/HARD), que es proteccion de verdad y
//  no cuesta memoria; se elimina la sacudida animada, que en el P4
//  recomponia el framebuffer entero por fotograma.
// #############################################################

// ---- Geometria ----
#define LK_PANEL_X  18
#define LK_PANEL_Y  110
#define LK_PANEL_W  284
#define LK_PANEL_H  150
#define LK_CARD_X   18
#define LK_CARD_W   284
#define LK_CARD_H   48
#define LK_CARD1_Y  276
#define LK_CARD2_Y  332

// Espera progresiva tras fallos consecutivos (igual que en Flex OS Ultra).
#define LOCK_FAILS_SOFT   4
#define LOCK_FAILS_HARD   6
#define LOCK_WAIT_SOFT_MS 30000UL
#define LOCK_WAIT_HARD_MS 300000UL

static uint8_t  failCount = 0;
static uint32_t waitUntil = 0;

// Opciones de bloqueo automatico (indice guardado en EEPROM).
static const uint32_t AUTOLOCK_MS[AUTOLOCK_OPTIONS] PROGMEM = {
  30000UL, 60000UL, 300000UL, 600000UL, 0UL
};
uint32_t autoLockMs(){
  uint8_t i = gAutoLockIdx < AUTOLOCK_OPTIONS ? gAutoLockIdx : 1;
  return pgm_read_dword(&AUTOLOCK_MS[i]);
}

// -------------------------------------------------------------
//  Pantalla de bloqueo
// -------------------------------------------------------------
static const char LK_UP[] PROGMEM = "Encendido";

static void lockCard(int16_t y, PGM_P title, const char* val, uint16_t accent){
  if(gGlass) glassRect(LK_CARD_X, y, LK_CARD_W, LK_CARD_H, 14, GLASS_CARD, 130);
  else       tft.fillRoundRect(LK_CARD_X, y, LK_CARD_W, LK_CARD_H, 14, RGB(40,46,64));
  tft.fillCircle(LK_CARD_X + 26, y + LK_CARD_H / 2, 9, accent);
  gfxTextP(LK_CARD_X + 46, y + 10, title, 1, C_ON_WALL);
  if(val) gfxText(LK_CARD_X + 46, y + 26, val, 1, C_ON_WALL_LO);
}

static void lockClock(){
  if(gGlass) glassRect(LK_PANEL_X, LK_PANEL_Y, LK_PANEL_W, LK_PANEL_H, 24, GLASS_CLOCK, 150);
  else       gfxWallRect(LK_PANEL_X, LK_PANEL_Y, LK_PANEL_W, LK_PANEL_H);
  char b[40];
  timeStr(b);
  gfxBigClock(b, SCR_W / 2, LK_PANEL_Y + 62, 40, 9, C_ON_WALL);
  dateLongStr(b);
  gfxTextC(SCR_W / 2, LK_PANEL_Y + 118, b, 1, C_ON_WALL_LO);
}

void lockEnter(){
  gfxWallpaper();
  drawSignal(SCR_W - 62, 26, 12, C_ON_WALL);
  drawBattery(SCR_W - 40, 14, 26, 13, 82, C_ON_WALL);
  lockClock();

  char b[24];
  uint32_t up = uptimeSec();
  char* p = fmtU16(b, (uint16_t)(up / 3600));
  *p++ = 'h'; *p++ = ' ';
  p = fmtU16(p, (uint16_t)((up / 60) % 60));
  *p++ = 'm'; *p = 0;
  lockCard(LK_CARD1_Y, LK_UP, b, C_OK);

  if(noteEmpty(0)){
    lockCard(LK_CARD2_Y, appNameP(APP_NOTES), 0, C_WARN);
    gfxTextP(LK_CARD_X + 46, LK_CARD2_Y + 26, strP(S_EMPTY), 1, C_ON_WALL_LO);
  } else {
    char n[NOTE_LEN + 1];
    noteLoad(0, n);
    if(strlen(n) > 30) n[30] = 0;
    lockCard(LK_CARD2_Y, appNameP(APP_NOTES), n, C_WARN);
  }

  tft.fillRoundRect(SCR_W / 2 - 46, SCR_H - 92, 92, 7, 3, C_WHITE);
  gfxTextCP(SCR_W / 2, SCR_H - 72, strP(S_SWIPEUNLOCK), 1, C_ON_WALL);
}

void lockTick(){
  if(gMinuteTick) lockClock();
  if(tp.swipeUp || (tp.tap && tp.y > SCR_H - 120)){
    if(gPinSet) shellRequestAuth(AUTH_UNLOCK);
    else        shellGoHome();
  }
}

// -------------------------------------------------------------
//  Verificacion por PIN
// -------------------------------------------------------------
static uint8_t authReason = AUTH_UNLOCK;
static uint8_t entered    = 0;
static uint8_t buf[LOCK_PIN_LEN];
static uint8_t stage      = 0;   // AUTH_SETPIN: 0 = teclear, 1 = repetir
static uint8_t first[LOCK_PIN_LEN];

#define DOT_ROW_Y 155

static void drawDots(bool error){
  gfxWallRect(SCR_W / 2 - 70, DOT_ROW_Y - 12, 140, 24);
  for(uint8_t i = 0; i < LOCK_PIN_LEN; i++){
    int16_t x = SCR_W / 2 - 45 + i * 30;
    if(i < entered) tft.fillCircle(x, DOT_ROW_Y, 8, error ? C_DANGER : C_WHITE);
    else            gfxRing(x, DOT_ROW_Y, 8, 2, error ? C_DANGER : C_WHITE);
  }
}

static void drawTitle(){
  gfxWallRect(0, 96, SCR_W, 26);
  PGM_P t;
  if(authReason == AUTH_SETPIN) t = strP(S_SETPIN);
  else                          t = strP(S_ENTERPIN);
  gfxTextCP(SCR_W / 2, 100, t, 2, C_ON_WALL);
}

void authEnter(uint8_t reason){
  authReason = reason;
  entered = 0;
  stage = 0;
  gfxWallpaper();
  drawTitle();
  drawDots(false);
  uiPinPad(0, true);
  if(!gPinSet && reason == AUTH_UNLOCK) shellGoHome();
}

static void authFail(){
  failCount++;
  drawDots(true);
  gfxWallRect(0, DOT_ROW_Y + 20, SCR_W, 20);
  gfxTextCP(SCR_W / 2, DOT_ROW_Y + 22, strP(S_WRONGPIN), 1, C_DANGER);
  if(failCount >= LOCK_FAILS_HARD)      waitUntil = millis() + LOCK_WAIT_HARD_MS;
  else if(failCount >= LOCK_FAILS_SOFT) waitUntil = millis() + LOCK_WAIT_SOFT_MS;
  entered = 0;
}

static void authAccept(){
  if(authReason == AUTH_SETPIN){
    if(stage == 0){
      memcpy(first, buf, LOCK_PIN_LEN);
      stage = 1;
      entered = 0;
      drawDots(false);
      gfxWallRect(0, DOT_ROW_Y + 20, SCR_W, 20);
      gfxTextCP(SCR_W / 2, DOT_ROW_Y + 22, strP(S_OK), 1, C_ON_WALL_LO);
      return;
    }
    if(memcmp(first, buf, LOCK_PIN_LEN) == 0){
      memcpy(gPin, buf, LOCK_PIN_LEN);
      gPinSet = true;
      storeSavePin();
      shellGoHome();
    } else {
      stage = 0;
      entered = 0;
      drawDots(true);
    }
    return;
  }
  if(memcmp(gPin, buf, LOCK_PIN_LEN) == 0){
    failCount = 0;
    waitUntil = 0;
    shellGoHome();
  } else {
    authFail();
  }
}

void authTick(){
  if(waitUntil && (int32_t)(millis() - waitUntil) < 0){
    if(tp.tap){
      gfxWallRect(0, DOT_ROW_Y + 20, SCR_W, 20);
      char b[16];
      char* p = fmtU16(b, (uint16_t)((waitUntil - millis()) / 1000UL));
      *p++ = 's'; *p = 0;
      gfxTextC(SCR_W / 2, DOT_ROW_Y + 22, b, 1, C_DANGER);
    }
    return;
  }
  if(!tp.tap) return;
  if(uiNavHit(tp.x, tp.y) == 0 || uiNavHit(tp.x, tp.y) == 1){
    if(authReason == AUTH_SETPIN){ shellGoHome(); return; }
  }
  int8_t k = uiPinPadHit(tp.x, tp.y);
  if(k == -1) return;
  if(k == -2){
    if(entered) entered--;
    drawDots(false);
    return;
  }
  if(entered < LOCK_PIN_LEN){
    buf[entered++] = (uint8_t)k;
    drawDots(false);
    if(entered == LOCK_PIN_LEN) authAccept();
  }
}
#undef LK_PANEL_X
#undef LK_PANEL_Y
#undef LK_PANEL_W
#undef LK_PANEL_H
#undef LK_CARD_X
#undef LK_CARD_W
#undef LK_CARD_H
#undef LK_CARD1_Y
#undef LK_CARD2_Y
#undef LOCK_FAILS_SOFT
#undef LOCK_FAILS_HARD
#undef LOCK_WAIT_SOFT_MS
#undef LOCK_WAIT_HARD_MS
#undef DOT_ROW_Y

// =============================================================
// IMPLEMENTACION INTEGRADA DESDE fos_shell.cpp
// =============================================================
// #############################################################
// ##  Flex OS UltraSingle  -  fos_shell.cpp
// ##  Arranque, OOBE y bucle principal
// #############################################################

uint8_t gState     = ST_SPLASH;
bool    gMinuteTick = false;

static uint32_t splashT0 = 0;
static uint32_t lastFrame = 0;

// -------------------------------------------------------------
//  Arranque
// -------------------------------------------------------------
static const char SP_NAME[] PROGMEM = "Flex OS";
static const char SP_EDIT[] PROGMEM = FOS_SHORT;
static const char SP_BRD[]  PROGMEM = FOS_BOARD;

static void splashDraw(){
  tft.fillScreen(C_BLACK);
  // Aparicion progresiva del anillo de marca (sin framebuffer: se dibuja
  // circulo a circulo directamente sobre la GRAM).
  for(uint8_t r = 4; r <= 34; r += 3){
    tft.drawCircle(SCR_W / 2, 180, r, mix565(C_BLACK, C_ACCENT, (uint8_t)(60 + r * 5)));
  }
  tft.fillCircle(SCR_W / 2, 180, 12, C_ACCENT);
  gfxTextCP(SCR_W / 2, 250, SP_NAME, 3, C_WHITE);
  gfxTextCP(SCR_W / 2, 282, SP_EDIT, 2, C_ACCENT);
  gfxTextCP(SCR_W / 2, SCR_H - 40, SP_BRD, 1, RGB(110,116,132));
}

// -------------------------------------------------------------
//  OOBE: eleccion de idioma
// -------------------------------------------------------------
static const char OO_ES[] PROGMEM = "Espa\xA4ol";
static const char OO_EN[] PROGMEM = "English";

#define OO_Y0 200
#define OO_H  64

static void oobeDraw(){
  gfxWallpaper();
  gfxTextCP(SCR_W / 2, 120, strP(S_LANGUAGE), 3, C_ON_WALL);
  for(uint8_t i = 0; i < 2; i++){
    int16_t y = OO_Y0 + i * (OO_H + 16);
    bool sel = (gLang == i);
    if(gGlass) glassRect(30, y, SCR_W - 60, OO_H, 18, sel ? C_ACCENT : GLASS_CARD, sel ? 200 : 120);
    else       tft.fillRoundRect(30, y, SCR_W - 60, OO_H, 18, sel ? C_ACCENT : RGB(44,50,68));
    gfxTextCP(SCR_W / 2, y + OO_H / 2 - 7, i == 0 ? OO_ES : OO_EN, 2, C_WHITE);
  }
  uiButtonP(60, 380, SCR_W - 120, 46, strP(S_CONTINUE), true);
}

static void oobeTick(){
  if(!tp.tap) return;
  for(uint8_t i = 0; i < 2; i++){
    int16_t y = OO_Y0 + i * (OO_H + 16);
    if(hitRect(tp.x, tp.y, 30, y, SCR_W - 60, OO_H)){
      gLang = i;
      storeSaveLang();
      oobeDraw();
      return;
    }
  }
  if(hitRect(tp.x, tp.y, 60, 380, SCR_W - 120, 46)){
    storeSaveLang();
    gState = ST_HOME;
    touchConsume();
    homeEnter();
  }
}

// -------------------------------------------------------------
//  Transiciones de estado
// -------------------------------------------------------------
void shellGoHome(){
  gState = ST_HOME;
  touchConsume();
  homeInvalidate();
  homeEnter();
}

void shellGoLock(){
  gState = ST_LOCK;
  touchConsume();
  fxWipe(false);
  lockEnter();
}

void shellRequestAuth(uint8_t reason){
  gState = ST_AUTH;
  touchConsume();
  authEnter(reason);
}

void shellOpenApp(uint8_t id, int16_t ix, int16_t iy, uint8_t is){
  fxOpenApp(ix, iy, is);
  gState = ST_APP;
  touchConsume();
  appOpen(id);
}

// -------------------------------------------------------------
//  setup / loop
// -------------------------------------------------------------
void shellSetup(){
  gfxInit();
  tft.cp437(true);                 // acentos correctos en la fuente 5x7
  storeInit();
  timeInit();
  storeLoadTime();
  touchInit();
  splashDraw();
  splashT0 = millis();
  lastFrame = millis();
}

void shellLoop(){
  touchPoll();
  if(tp.pressed) gTouches++;

  gMinuteTick = timeTick();
  if(gMinuteTick){
    gMinutes++;
    // La EEPROM tiene ~100.000 ciclos por celda: la hora y el contador de uso
    // se vuelcan cada 15 minutos, no cada minuto.
    if((gMinutes % 15) == 0){ storeSaveTime(); storeSaveUsage(); }
  }

  switch(gState){
    case ST_SPLASH:
      if(millis() - splashT0 >= SPLASH_MS){
        if(gFirstRun){ gState = ST_OOBE_LANG; oobeDraw(); }
        else         { gState = ST_LOCK; lockEnter(); }
      }
      break;

    case ST_OOBE_LANG:
      oobeTick();
      break;

    case ST_LOCK:
      lockTick();
      break;

    case ST_AUTH:
      authTick();
      break;

    case ST_HOME:
      homeTick();
      break;

    case ST_APP: {
      uint8_t f = appFlags(appCurrent());
      if(!(f & AF_FULLSCREEN) && tp.tap){
        int8_t n = uiNavHit(tp.x, tp.y);
        if(n == 0 || n == 1){
          fxCloseApp();
          shellGoHome();
          return;
        }
        if(n == 2) return;             // sin conmutador de tareas
      }
      appTick();
      if(!(f & AF_FULLSCREEN)) uiToastTick();
    } break;
  }

  // Bloqueo automatico por inactividad.
  if((gState == ST_HOME || gState == ST_APP) && autoLockMs()){
    if(millis() - touchLastActivity() >= autoLockMs()) shellGoLock();
  }

  // Ritmo del bucle: sin esto el tactil resistivo se sobremuestrea y el
  // sistema gasta el bus en repintados que nadie ve.
  uint32_t now = millis();
  if(now - lastFrame < TICK_MS) delay(TICK_MS - (now - lastFrame));
  lastFrame = millis();
}
#undef OO_Y0
#undef OO_H

void setup(){
  shellSetup();
}

void loop(){
  shellLoop();
}
