// Pantalla OLED SSD1306 128x64 por I2C (deteccion en 0x3C / 0x3D)
#pragma once
#include <stdbool.h>

bool oled_iniciar(int sda, int scl, bool girar);  // false si no hay pantalla: la bomba sigue sin ella
void oled_limpiar(void);
void oled_texto(int x, int y, int escala, const char *s);
void oled_texto_centrado(int y, int escala, const char *s);
void oled_barra(int y, int porcentaje);
void oled_circulo(int cx, int cy, int r);
void oled_invertir(void);  // blanco <-> negro en todo lo dibujado
bool oled_mostrar(void);  // false si no esta lista o falla el envio
bool oled_prueba(void);  // patrones blanco/negro/texto; requiere confirmacion visual
