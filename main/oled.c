#include <ctype.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ssd1306.h"
#include "oled.h"

#define ANCHO 128
#define ALTO 64
#define DIR_OLED 0x3C

static const char *TAG = "oled";
static esp_lcd_panel_handle_t panel;
static bool lista = false;
static uint8_t fb[ANCHO * ALTO / 8];  // formato del SSD1306: cada byte = 8 pixeles verticales

// Fuente 5x7, caracteres ' ' a 'Z'. Cada byte es una columna; bit 0 = fila de arriba.
static const uint8_t FUENTE[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, // ' ' ! "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, // # $ %
    {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00}, // & ' (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08}, // ) * +
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, // , - .
    {0x20, 0x10, 0x08, 0x04, 0x02},                                                                 // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, // 0 1 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, // 3 4 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36}, // 6 7 8
    {0x06, 0x49, 0x49, 0x29, 0x1E},                                                                 // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, // : ; <
    {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, // = > ?
    {0x32, 0x49, 0x79, 0x41, 0x3E},                                                                 // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22}, // A B C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, // D E F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, // G H I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40}, // J K L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E}, // M N O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, // P Q R
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, // S T U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63}, // V W X
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},                                 // Y Z
};

bool oled_iniciar(int sda, int scl, bool girar)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return false;
    if (i2c_master_probe(bus, DIR_OLED, 50) != ESP_OK) {
        ESP_LOGW(TAG, "No se encontro la OLED en 0x3C; la bomba sigue sin pantalla");
        return false;
    }
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t ioc = {
        .dev_addr = DIR_OLED,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_ssd1306_config_t sc = {.height = ALTO};
    esp_lcd_panel_dev_config_t pc = {.bits_per_pixel = 1, .reset_gpio_num = -1, .vendor_config = &sc};
    if (esp_lcd_new_panel_io_i2c(bus, &ioc, &io) != ESP_OK ||
        esp_lcd_new_panel_ssd1306(io, &pc, &panel) != ESP_OK ||
        esp_lcd_panel_reset(panel) != ESP_OK ||
        esp_lcd_panel_init(panel) != ESP_OK) {
        ESP_LOGW(TAG, "Fallo al iniciar la OLED; la bomba sigue sin pantalla");
        return false;
    }
    if (girar) esp_lcd_panel_mirror(panel, true, true);
    esp_lcd_panel_disp_on_off(panel, true);
    lista = true;
    return true;
}

void oled_limpiar(void) { memset(fb, 0, sizeof fb); }

static void cuadro(int x, int y, int w, int h, bool relleno)
{
    for (int i = 0; i < w; i++)
        for (int j = 0; j < h; j++) {
            int px = x + i, py = y + j;
            if (px < 0 || px >= ANCHO || py < 0 || py >= ALTO) continue;
            if (relleno || i == 0 || j == 0 || i == w - 1 || j == h - 1)
                fb[px + (py / 8) * ANCHO] |= 1 << (py % 8);
        }
}

void oled_texto(int x, int y, int escala, const char *s)
{
    for (; *s; s++, x += 6 * escala) {
        int c = toupper((unsigned char)*s);
        if (c < ' ' || c > 'Z') c = '?';
        for (int col = 0; col < 5; col++)
            for (int fila = 0; fila < 7; fila++)
                if ((FUENTE[c - ' '][col] >> fila) & 1)
                    cuadro(x + col * escala, y + fila * escala, escala, escala, true);
    }
}

void oled_texto_centrado(int y, int escala, const char *s)
{
    int ancho = (int)strlen(s) * 6 * escala - escala;
    oled_texto((ANCHO - ancho) / 2, y, escala, s);
}

void oled_barra(int y, int porcentaje)
{
    cuadro(4, y, 120, 10, false);
    cuadro(6, y + 2, 116 * porcentaje / 100, 6, true);
}

void oled_circulo(int cx, int cy, int r)
{
    // círculo de punto medio: se calcula 1/8 y se refleja a los otros 7
    for (int x = r, y = 0, err = 1 - r; x >= y;) {
        int p[8][2] = {{x, y}, {y, x}, {-y, x}, {-x, y}, {-x, -y}, {-y, -x}, {y, -x}, {x, -y}};
        for (int i = 0; i < 8; i++) cuadro(cx + p[i][0], cy + p[i][1], 1, 1, true);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void oled_invertir(void)
{
    for (int i = 0; i < (int)sizeof fb; i++) fb[i] ^= 0xFF;
}

void oled_mostrar(void)
{
    if (lista) esp_lcd_panel_draw_bitmap(panel, 0, 0, ANCHO, ALTO, fb);
}
