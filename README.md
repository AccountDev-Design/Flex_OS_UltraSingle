# 🟢 Flex OS UltraSingle

**Flex OS UltraSingle** es la edición de **Flex OS Ultra** para **Arduino Mega 2560**.

Mismo sistema operativo por fuera —mismo fondo, misma barra superior, misma
pantalla de bloqueo, mismos iconos y colores— reconstruido por dentro para
funcionar con **256 KB de Flash y 8 KB de SRAM** en lugar de un ESP32-P4 con
PSRAM.

---

## 📦 Hardware

| | |
|---|---|
| Placa | Arduino Mega 2560 (ATmega2560 @ 16 MHz) |
| Pantalla | TFT LCD Shield 3.5" · SKU **MAR3501** |
| Driver | **ILI9486** · bus paralelo de 8 bits |
| Resolución | 320 × 480 (retrato) |
| Táctil | Resistivo de 4 hilos integrado |

### Táctil (calibración oficial del proyecto)

```cpp
const int XP = 8;
const int XM = A2;
const int YP = A3;
const int YM = 9;

const int TS_LEFT = 76;
const int TS_RT   = 905;
const int TS_TOP  = 951;
const int TS_BOT  = 68;

x = map(p.x, TS_LEFT, TS_RT, 0, 320);
y = map(p.y, TS_TOP, TS_BOT, 0, 480);
```

Estos valores viven en `fos_config.h` y no deben modificarse.

---

## 🚀 Compilar

1. Abre `FlexOS_UltraSingle/FlexOS_UltraSingle.ino` en el Arduino IDE.
2. Instala estas tres bibliotecas desde el gestor:
   - **MCUFRIEND_kbv** (David Prentice)
   - **Adafruit GFX Library** (Adafruit)
   - **Adafruit TouchScreen** (Adafruit)
3. Placa: *Arduino Mega or Mega 2560* · Procesador: *ATmega2560*.

### Consumo actual

```
Flash :  66,5 KB de 256 KB   (25 %)
SRAM  :   2,0 KB de   8 KB   (25 %)
```

Queda margen de sobra para seguir creciendo.

---

## 📱 Aplicaciones

| App | Qué hace |
|---|---|
| Reloj | Esfera analógica vectorial + hora digital |
| Calendario | Rejilla mensual, hoy resaltado, meses deslizables |
| Calculadora | Cuatro operaciones, porcentaje y signo |
| Notas | Tres notas de 96 caracteres en EEPROM, con teclado |
| Paint | Lienzo directo, 8 colores y dos grosores |
| Juegos | Serpiente, con récord guardado |
| Cronómetro | Precisión de centésimas |
| Linterna | Tres niveles, a pantalla completa |
| Bienestar | Tiempo de uso y toques acumulados |
| Memoria | Uso real de SRAM, Flash y EEPROM |
| Sistema | Ficha del equipo y del panel |
| Ajustes | Pantalla, bloqueo, idioma, fecha y hora, acerca de |

Además: escritorio con widgets y dock, modo edición para reordenar iconos,
panel rápido desplegable, pantalla de bloqueo con PIN y bloqueo automático,
y asistente de idioma en el primer arranque.

---

## 🧱 Arquitectura

```
FlexOS_UltraSingle/
├── FlexOS_UltraSingle.ino   setup / loop
├── fos_config.h             hardware, geometría, constantes
├── fos_theme.h              paleta heredada de Flex OS Ultra
├── fos_gfx.*                motor gráfico directo sobre la GRAM
├── fos_touch.*              táctil resistivo y gestos
├── fos_str.*                textos en PROGMEM (español / inglés)
├── fos_time.*               reloj interno sin RTC
├── fos_store.*              persistencia en EEPROM
├── fos_icons.*              iconos vectoriales
├── fos_ui.*                 barras, filas, teclados, avisos
├── fos_fx.*                 transiciones
├── fos_home.*               escritorio y panel rápido
├── fos_lock.*               bloqueo y PIN
├── fos_shell.*              arranque, OOBE y bucle principal
├── fos_apps.*               registro de aplicaciones
└── app_*.cpp                las doce aplicaciones
```

### Decisiones de diseño

- **Sin framebuffer.** Flex OS Ultra componía la pantalla en tres buffers de
  768 KB en PSRAM. Aquí se dibuja directamente en la GRAM del ILI9486 y solo se
  repintan las zonas que cambian. El único buffer del sistema es una **línea de
  barrido de 640 B**.
- **Translucidez sin leer la pantalla.** El fondo es un degradado *analítico*:
  el color de cualquier píxel se calcula en vez de leerse del panel (leer la
  GRAM del ILI9486 es lentísimo). Como el desenfoque de un degradado lineal es
  el propio degradado, el efecto *Liquid Glass* se reduce a mezclar el tinte con
  el fondo calculado — mismo resultado, coste cero en memoria.
- **Sin `String`, sin `new`/`malloc`, sin recursión.** Todos los textos y tablas
  viven en PROGMEM; el único uso de coma flotante está en la Calculadora.
- **Un solo bucle cooperativo**, sin FreeRTOS ni tareas de fondo.

---

## ❌ Qué se eliminó respecto a Flex OS Ultra

Todo lo que dependía del ESP32 o de hardware que el Mega no tiene:

DeX / Modo PC · OTA · Wi-Fi · Bluetooth Classic · BLE · Navegador · Cámara ·
Tienda de aplicaciones · Juegos en red · Comunicación por Internet · Servicios
en segundo plano · Multitarea real · Ventanas flotantes · LittleFS · NVS ·
FreeRTOS.

No queda código muerto ni funciones deshabilitadas: se eliminaron por completo.

---

## 🔵 Flex OS Ultra (ESP32-P4 / C6)

La edición para ESP32 continúa su desarrollo en su propia rama.

---

## 📜 Licencia

MIT

## ❤️ Proyecto

Flex OS es un proyecto personal de **Ricardo Enrique Alvarez Santos**, con el
objetivo de llevar una experiencia de sistema operativo moderno a
microcontroladores.
