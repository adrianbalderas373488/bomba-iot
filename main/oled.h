// Pantalla OLED SSD1306 128x64 por I2C (dirección 0x3C)
#pragma once
#include <stdbool.h>

bool oled_iniciar(int sda, int scl, bool girar);  // false si no hay pantalla: la bomba sigue sin ella
void oled_limpiar(void);
void oled_texto(int x, int y, int escala, const char *s);
void oled_texto_centrado(int y, int escala, const char *s);
void oled_barra(int y, int porcentaje);
void oled_mostrar(void);
