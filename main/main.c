// Bomba IoT "plantar / desactivar" — ESP32 (ESP-IDF) + Adafruit IO (MQTT sobre TLS)
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "oled.h"
#include "config.h"  // copia config.example.h -> config.h (no se sube a git)

// ---------- Pines ----------
#define PIN_ARMAR       GPIO_NUM_18  // botón rojo a GND
#define PIN_DESACTIVAR  GPIO_NUM_19  // botón verde a GND
#define PIN_BUZZER      GPIO_NUM_23  // buzzer activo
#define PIN_LED_ROJO    GPIO_NUM_25  // LED rojo + 220 ohm
#define PIN_LED_VERDE   GPIO_NUM_26  // LED verde + 220 ohm (PWM: brilla más conforme se desactiva)
#define PIN_SDA         GPIO_NUM_21  // OLED
#define PIN_SCL         GPIO_NUM_22  // OLED
#define OLED_GIRAR      false        // ponlo en true si el texto se ve de cabeza

// ---------- Tiempos (ms) ----------
#define T_PLANTAR_MS      3000
#define T_BOMBA_MS        40000
#define T_DESACTIVAR_MS   5000
#define T_REBOTE_MS       30
#define T_ENTRE_ENVIOS_MS 2200  // Adafruit IO gratis: máx 30 msg/min

#define TOPIC(feed) AIO_USERNAME "/feeds/" feed

static const char *TAG = "bomba";

typedef enum { ESPERA, PLANTADA, DESACTIVADA, EXPLOTO } estado_t;
static const char *NOMBRE[] = {"espera", "plantada", "desactivada", "exploto"};
typedef enum { CMD_ARMAR, CMD_REINICIAR } comando_t;

static esp_mqtt_client_handle_t mqtt;
static volatile bool mqtt_conectado = false;
static QueueHandle_t cola_cmd;  // comandos de la nube -> tarea del juego

// Estado del juego (solo lo toca el bucle principal)
static estado_t estado;
static int64_t t_estado, t_ultimo_bip, buzzer_hasta;
static int tiempo_s, progreso;
static bool estado_pendiente;

static int64_t ahora_ms(void) { return esp_timer_get_time() / 1000; }

// ======================= Red =======================
static void wifi_evento(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // 201 = red no encontrada, 15/202/204 = contraseña mal (lista: esp_wifi_types.h)
        ESP_LOGW(TAG, "WiFi caido (motivo %d), reintentando",
                 ((wifi_event_sta_disconnected_t *)data)->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi conectado");
    }
}

static void wifi_iniciar(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evento, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evento, NULL));
    wifi_config_t wc = {.sta = {.ssid = WIFI_SSID, .password = WIFI_PASS}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void mqtt_evento(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT conectado");
        esp_mqtt_client_subscribe_single(mqtt, TOPIC("bomba-cmd"), 0);
        // Adafruit IO avisa aquí si rechaza un dato (feed inexistente, límite excedido...)
        esp_mqtt_client_subscribe_single(mqtt, AIO_USERNAME "/errors", 0);
        esp_mqtt_client_subscribe_single(mqtt, AIO_USERNAME "/throttle", 0);
        mqtt_conectado = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT desconectado (reintenta solo)");
        mqtt_conectado = false;
        break;
    case MQTT_EVENT_DATA: {
        comando_t c;
        if (e->topic_len < 9 || memcmp(e->topic + e->topic_len - 9, "bomba-cmd", 9)) {
            ESP_LOGE(TAG, "Adafruit IO: %.*s", e->data_len, e->data);
            break;
        }
        if (e->data_len == 5 && !memcmp(e->data, "armar", 5)) c = CMD_ARMAR;
        else if (e->data_len == 9 && !memcmp(e->data, "reiniciar", 9)) c = CMD_REINICIAR;
        else break;  // cualquier otro valor se ignora (p. ej. el "release" del botón)
        xQueueSend(cola_cmd, &c, 0);
        break;
    }
    default:
        break;
    }
}

static void mqtt_iniciar(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = "mqtts://" AIO_SERVER ":8883",
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,  // verifica el certificado del servidor
        .credentials.username = AIO_USERNAME,
        .credentials.authentication.password = AIO_KEY,
    };
    mqtt = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_evento, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));
}

// Encola sin bloquear; la tarea MQTT lo envía. Devuelve true si se aceptó.
static bool publicar(const char *topic, const char *valor, int qos)
{
    return esp_mqtt_client_enqueue(mqtt, topic, valor, 0, qos, 0, true) >= 0;
}

// Un mensaje cada T_ENTRE_ENVIOS_MS como máximo. Prioridad: estado > progreso > tiempo.
static void telemetria(int64_t t)
{
    static int64_t t_envio = -T_ENTRE_ENVIOS_MS;
    static int tiempo_env = -1, progreso_env = -1;
    static bool estaba_conectado = false;
    char v[8];

    if (!mqtt_conectado) { estaba_conectado = false; return; }
    if (!estaba_conectado) {  // tras (re)conectar, reenviar todo
        estaba_conectado = true;
        estado_pendiente = true;
        tiempo_env = progreso_env = -1;
    }
    if (t - t_envio < T_ENTRE_ENVIOS_MS) return;

    if (estado_pendiente) {
        if (!publicar(TOPIC("bomba-estado"), NOMBRE[estado], 1)) return;
        estado_pendiente = false;
    } else if (progreso != progreso_env) {
        snprintf(v, sizeof v, "%d", progreso);
        if (!publicar(TOPIC("bomba-progreso"), v, 0)) return;
        ESP_LOGI(TAG, "PUB progreso=%s", v);
        progreso_env = progreso;
    } else if (tiempo_s != tiempo_env) {
        snprintf(v, sizeof v, "%d", tiempo_s);
        if (!publicar(TOPIC("bomba-tiempo"), v, 0)) return;
        ESP_LOGI(TAG, "PUB tiempo=%s", v);
        tiempo_env = tiempo_s;
    } else {
        return;
    }
    t_envio = t;
}

// ======================= Juego =======================
typedef struct {
    gpio_num_t pin;
    bool presionado, lectura;
    int64_t t_cambio, t_inicio;
} boton_t;

// ms que lleva presionado (0 = suelto), con antirrebote
static int64_t ms_presionado(boton_t *b, int64_t t)
{
    bool l = gpio_get_level(b->pin) == 0;
    if (l != b->lectura) { b->lectura = l; b->t_cambio = t; }
    if (t - b->t_cambio >= T_REBOTE_MS && l != b->presionado) {
        b->presionado = l; b->t_inicio = t;
        ESP_LOGI(TAG, "BTN,GPIO%d,%s", b->pin, l ? "presionado" : "suelto");
    }
    return b->presionado ? t - b->t_inicio + 1 : 0;
}

static void cambiar_estado(estado_t e, int64_t t)
{
    estado = e;
    t_estado = t;
    estado_pendiente = true;
    ESP_LOGI(TAG, "EVT,%lld,%s", t, NOMBRE[e]);  // log CSV para las pruebas
    switch (e) {
    case ESPERA:      tiempo_s = T_BOMBA_MS / 1000; progreso = 0; break;
    case PLANTADA:    t_ultimo_bip = t; buzzer_hasta = t + 300; break;
    case DESACTIVADA: progreso = 100; buzzer_hasta = t + 600; break;
    case EXPLOTO:     tiempo_s = 0; progreso = 0; buzzer_hasta = t + 2000; break;
    }
}

static void juego_paso(int64_t t, int64_t armar, int64_t desact)
{
    comando_t c;
    while (xQueueReceive(cola_cmd, &c, 0)) {
        ESP_LOGI(TAG, "CMD,%lld,%s", t, c == CMD_ARMAR ? "armar" : "reiniciar");
        if (c == CMD_REINICIAR) cambiar_estado(ESPERA, t);
        else if (estado != PLANTADA) cambiar_estado(PLANTADA, t);
    }

    switch (estado) {
    case ESPERA:
        if (armar >= T_PLANTAR_MS) cambiar_estado(PLANTADA, t);
        break;
    case PLANTADA: {
        int64_t transcurrido = t - t_estado;
        // solo cuenta lo presionado después de plantar (evita dejarlo apretado desde antes)
        int64_t d = desact < transcurrido ? desact : transcurrido;
        int64_t restante = T_BOMBA_MS - transcurrido;
        if (d >= T_DESACTIVAR_MS) { cambiar_estado(DESACTIVADA, t); break; }
        if (restante <= 0) { cambiar_estado(EXPLOTO, t); break; }
        progreso = d * 100 / T_DESACTIVAR_MS;  // soltar el botón lo regresa a 0
        tiempo_s = (restante + 999) / 1000;
        int64_t intervalo = 100 + restante * 900 / T_BOMBA_MS;  // bip cada 1 s -> 0.1 s
        if (t - t_ultimo_bip >= intervalo) { t_ultimo_bip = t; buzzer_hasta = t + 50; }
        break;
    }
    default:  // DESACTIVADA / EXPLOTO: el botón armar reinicia
        if (armar) cambiar_estado(ESPERA, t);
    }
}

// LED rojo: parpadea con cada bip; LED verde: sube de brillo con el progreso de desactivación
static void indicadores(int64_t t)
{
    bool rojo = false;
    int verde = 0;  // 0-255
    switch (estado) {
    case PLANTADA:    rojo = t < buzzer_hasta; verde = progreso * progreso * 255 / 10000; break;  // al cuadrado: el ojo lo ve lineal
    case DESACTIVADA: verde = 255; break;
    case EXPLOTO:     rojo = t - t_estado >= 3000 || (t / 100) % 2; break;
    default: break;
    }
    gpio_set_level(PIN_LED_ROJO, rojo);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, verde);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void pantalla(int64_t t, int64_t armar)
{
    static int64_t t_pantalla;
    if (t - t_pantalla < 100) return;  // 10 cuadros/s; cada envío por I2C tarda ~25 ms
    t_pantalla = t;
    char s[24];
    oled_limpiar();
    oled_texto(0, 0, 1, NOMBRE[estado]);
    oled_texto(104, 0, 1, mqtt_conectado ? "NUBE" : "----");
    switch (estado) {
    case ESPERA:
        if (armar) {
            oled_texto_centrado(18, 2, "PLANTANDO");
            oled_barra(44, armar * 100 / T_PLANTAR_MS);
        } else {
            oled_texto_centrado(18, 3, "LISTA");
            oled_texto_centrado(52, 1, "MANTEN EL ROJO");
        }
        break;
    case PLANTADA:
        snprintf(s, sizeof s, "%d:%02d", tiempo_s / 60, tiempo_s % 60);
        oled_texto_centrado(14, 4, s);
        if (progreso) oled_barra(50, progreso);
        else oled_texto_centrado(52, 1, "MANTEN EL VERDE");
        break;
    case DESACTIVADA:
        oled_texto_centrado(18, 3, "A SALVO");
        oled_texto_centrado(52, 1, "ROJO: OTRA RONDA");
        break;
    case EXPLOTO:
        oled_texto_centrado(18, 3, "BOOM!");
        oled_texto_centrado(52, 1, "ROJO: OTRA RONDA");
        break;
    }
    oled_mostrar();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_config_t entradas = {
        .pin_bit_mask = (1ULL << PIN_ARMAR) | (1ULL << PIN_DESACTIVAR),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&entradas));
    gpio_config_t salida = {.pin_bit_mask = (1ULL << PIN_BUZZER) | (1ULL << PIN_LED_ROJO), .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&salida));

    ledc_timer_config_t lt = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&lt));
    ledc_channel_config_t lc = {
        .gpio_num = PIN_LED_VERDE,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&lc));

    oled_iniciar(PIN_SDA, PIN_SCL, OLED_GIRAR);  // si no hay pantalla, solo avisa en el log


    cola_cmd = xQueueCreate(4, sizeof(comando_t));
    wifi_iniciar();
    mqtt_iniciar();  // la red corre en sus propias tareas: el juego funciona aunque no haya internet

    boton_t b_armar = {.pin = PIN_ARMAR}, b_desact = {.pin = PIN_DESACTIVAR};
    cambiar_estado(ESPERA, ahora_ms());
    while (1) {
        int64_t t = ahora_ms();
        int64_t armar = ms_presionado(&b_armar, t);
        int64_t desact = ms_presionado(&b_desact, t);
        juego_paso(t, armar, desact);
        gpio_set_level(PIN_BUZZER, t < buzzer_hasta);
        indicadores(t);
        pantalla(t, armar);
        telemetria(t);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
