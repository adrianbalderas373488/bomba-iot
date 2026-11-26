#include <ctype.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ssd1306.h"
#include "oled.h"

#define ANCHO 128
#define ALTO 64
#define I2C_HZ 100000  // velocidad conservadora para diagnosticar cables/protoboard

static const char *TAG = "oled";
static esp_lcd_panel_handle_t panel;
static i2c_master_bus_handle_t bus;
static esp_lcd_panel_io_handle_t io;
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

static bool comprobar(esp_err_t err, const char *paso)
{
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OK: %s", paso);
        return true;
    }
    ESP_LOGE(TAG, "FALLO: %s -> %s (0x%x)", paso, esp_err_to_name(err), (unsigned int)err);
    return false;
}

static void liberar(void)
{
    lista = false;
    if (panel) { esp_lcd_panel_del(panel); panel = NULL; }
    if (io) { esp_lcd_panel_io_del(io); io = NULL; }
    if (bus) { i2c_del_master_bus(bus); bus = NULL; }
}

bool oled_iniciar(int sda, int scl, bool girar)
{
    liberar();
    ESP_LOGI(TAG, "=== DIAGNOSTICO OLED: SDA=GPIO%d SCL=GPIO%d, %d Hz ===", sda, scl, I2C_HZ);
    ESP_LOGI(TAG, "Cableado esperado: VCC->3V3, GND->GND; verifica las etiquetas del modulo");
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (!comprobar(i2c_new_master_bus(&bc, &bus), "crear bus I2C")) goto fallo;
    // El controlador configura los pines como open-drain; habilitar su lectura
    // permite comprobar si alguna linea permanece en bajo estando en reposo.
    if (!comprobar(gpio_input_enable(sda), "habilitar lectura SDA") ||
        !comprobar(gpio_input_enable(scl), "habilitar lectura SCL")) goto fallo;
    vTaskDelay(pdMS_TO_TICKS(200));  // estabilizacion de alimentacion de la pantalla
    int nivel_sda = gpio_get_level(sda), nivel_scl = gpio_get_level(scl);
    ESP_LOGI(TAG, "Bus en reposo: SDA=%d SCL=%d (se espera 1 y 1)", nivel_sda, nivel_scl);
    if (!nivel_sda || !nivel_scl) {
        ESP_LOGE(TAG, "Linea en bajo: revisar corto a GND, cableado, alimentacion y pull-ups");
        goto fallo;
    }

    int direccion = -1, encontrados = 0;
    ESP_LOGI(TAG, "Escaneando direcciones I2C 0x08..0x77...");
    for (int dir = 0x08; dir <= 0x77; dir++) {
        esp_err_t err = i2c_master_probe(bus, dir, 50);
        if (err == ESP_OK) {
            encontrados++;
            ESP_LOGI(TAG, "ACK: dispositivo en 0x%02X%s", dir,
                     (dir == 0x3C || dir == 0x3D) ? " (posible OLED)" : "");
            if (direccion < 0 && (dir == 0x3C || dir == 0x3D)) direccion = dir;
        } else if (err != ESP_ERR_NOT_FOUND) {
            comprobar(err, "escanear bus I2C");
            ESP_LOGE(TAG, "Error al probar 0x%02X; revisar SDA/SCL y pull-ups a 3V3", dir);
            goto fallo;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (direccion < 0) {
        ESP_LOGE(TAG, "SIN OLED en 0x3C/0x3D; dispositivos encontrados: %d", encontrados);
        ESP_LOGE(TAG, "Revisar 3V3-GND en el modulo, tierra comun, SDA/SCL y continuidad de jumpers/rieles");
        ESP_LOGW(TAG, "SDA=SCL=1 no prueba continuidad ni alimentacion: los pull-ups internos tambien las elevan");
        goto fallo;
    }
    ESP_LOGI(TAG, "Usando 0x%02X; un ACK confirma respuesta I2C, no el modelo ni los pixeles", direccion);
    esp_lcd_panel_io_i2c_config_t ioc = {
        .dev_addr = direccion,
        .scl_speed_hz = I2C_HZ,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_ssd1306_config_t sc = {.height = ALTO};
    esp_lcd_panel_dev_config_t pc = {.bits_per_pixel = 1, .reset_gpio_num = -1, .vendor_config = &sc};
    if (!comprobar(esp_lcd_new_panel_io_i2c(bus, &ioc, &io), "crear IO de pantalla") ||
        !comprobar(esp_lcd_new_panel_ssd1306(io, &pc, &panel), "crear controlador SSD1306") ||
        !comprobar(esp_lcd_panel_reset(panel), "reset de pantalla") ||
        !comprobar(esp_lcd_panel_init(panel), "inicializar SSD1306")) goto fallo;
    if (girar && !comprobar(esp_lcd_panel_mirror(panel, true, true), "girar pantalla")) goto fallo;
    if (!comprobar(esp_lcd_panel_disp_on_off(panel, true), "encender pantalla")) goto fallo;
    lista = true;
    return true;

fallo:
    liberar();
    ESP_LOGW(TAG, "OLED no disponible; consulta arriba el paso que fallo");
    return false;
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

bool oled_mostrar(void)
{
    if (!lista) return false;
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, 0, ANCHO, ALTO, fb);
    if (err != ESP_OK) {
        comprobar(err, "enviar imagen a OLED");
        ESP_LOGE(TAG, "Se detienen los envios; revisar falso contacto/alimentacion y reiniciar");
        lista = false;  // evita saturar el monitor con el mismo error cada 100 ms
        return false;
    }
    return true;
}

bool oled_prueba(void)
{
    if (!lista) return false;
    ESP_LOGI(TAG, "PRUEBA 1/3: pantalla BLANCA durante 2 segundos");
    memset(fb, 0xFF, sizeof fb);
    if (!oled_mostrar()) return false;
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "PRUEBA 2/3: pantalla NEGRA durante 1 segundo");
    oled_limpiar();
    if (!oled_mostrar()) return false;
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "PRUEBA 3/3: marco y texto OLED / PRUEBA I2C durante 3 segundos");
    cuadro(0, 0, ANCHO, ALTO, false);
    oled_texto_centrado(12, 3, "OLED");
    oled_texto_centrado(44, 1, "PRUEBA I2C");
    if (!oled_mostrar()) return false;
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Envios de prueba OK. Confirma visualmente si viste los tres patrones");
    ESP_LOGW(TAG, "Si sigue negra: verificar voltaje en VCC/GND y modelo SSD1306 128x64 (SH1106 requiere otro driver)");
    return true;
}
