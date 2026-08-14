# Flex OS Super Single

Edición de Flex OS para **Arduino Uno (ATmega328P)** y el mismo TFT LCD Shield
3.5" MAR3501/ILI9486 usado por Flex OS UltraSingle.

La versión Mega permanece intacta. Super Single vive en su propia carpeta y
fija el controlador en `0x9486` para que el optimizador LTO elimine todo el
soporte de pantallas que este hardware no utiliza.

## Hardware

| Componente | Configuración |
|---|---|
| Placa | Arduino Uno, ATmega328P a 16 MHz |
| Flash / SRAM / EEPROM | 32 KB / 2 KB / 1 KB |
| Pantalla | TFT LCD Shield 3.5", SKU MAR3501 |
| Controlador | ILI9486, paralelo de 8 bits |
| Resolución | 320 × 480, retrato |
| Táctil | Resistivo de 4 hilos |

La calibración se conserva exactamente:

```cpp
XP = 8; XM = A2; YP = A3; YM = 9;
LEFT = 76; RT = 905; TOP = 951; BOT = 68;
```

## Compilar

1. Abre `FlexOS_SuperSingle.ino` en Arduino IDE.
2. Instala `MCUFRIEND_kbv`, `Adafruit GFX Library` y `Adafruit TouchScreen`.
3. Selecciona **Arduino Uno**. Mantén LTO y la optimización de tamaño activos.
4. Verifica y carga el sketch.

Compilación verificada con Arduino AVR Boards 1.8.6, MCUFRIEND_kbv 3.1.0,
Adafruit GFX 1.12.6 y Adafruit TouchScreen 1.1.6:

```text
Flash: 31.144 / 32.256 bytes (96 %)
SRAM :    330 /  2.048 bytes (16 %), 1.718 bytes libres
```

## Qué conserva

- La paleta verde/azul/morada, superficies de vidrio y modo oscuro.
- La pantalla de bloqueo, reloj grande, tarjetas y gesto para desbloquear.
- El escritorio con dos widgets, rejilla 4×3, doce iconos, dock y panel rápido.
- Reloj, calendario, calculadora entera, Paint, cronómetro, linterna, bienestar,
  memoria, información del sistema, ajustes, bloqueo y acerca de.
- Hora, formato 24 h y preferencias visuales en EEPROM.

## Recortes necesarios para Arduino Uno

Se retiraron el PIN, Notas con teclado, Juegos, segundo idioma, reordenamiento
de iconos, efectos de pulsación y todas las transiciones. La calculadora usa
aritmética entera y el reloj no depende de RTC.

No hay framebuffer ni línea de barrido. El fondo se genera por mosaicos y el
vidrio por filas directamente en la GRAM del ILI9486. Durante `loop()` solo se
repintan la región modificada: reloj, widget, resultado, tecla lógica, paleta o
dígitos del cronómetro. Un pintado completo solo ocurre al entrar en otra vista
o cuando Linterna cambia el color de toda la pantalla por definición.
