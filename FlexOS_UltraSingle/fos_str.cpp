// #############################################################
// ##  Flex OS UltraSingle  -  fos_str.cpp
// ##  Tablas de texto en PROGMEM
// #############################################################
#include "fos_str.h"

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
