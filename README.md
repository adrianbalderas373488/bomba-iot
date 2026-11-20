# Bomba IoT: plantar y desactivar (ESP32 + Adafruit IO)

Una "bomba" de juego (para airsoft, gotcha o team building) hecha con un ESP32.
- Un jugador **planta** la bomba manteniendo el botón rojo **3 s** (o desde el dashboard).
- Empieza una cuenta regresiva de **40 s**. El buzzer pita cada vez más rápido.
- El otro equipo la **desactiva** manteniendo el botón verde **5 s**. Si lo suelta, el progreso regresa a 0.
- La tira de LEDs muestra el progreso: naranja al plantar, rojo con el tiempo restante y azul al desactivar.
- El ESP32 publica el estado, el tiempo y el progreso en **Adafruit IO por MQTT sobre TLS**. Así un árbitro sigue la partida en vivo desde el celular o la PC, arma o reinicia la bomba a distancia y tiene el historial de las rondas.

## Arquitectura

```mermaid
flowchart LR
  subgraph ESP32["ESP32 (ESP-IDF, FreeRTOS)"]
    BTN[Botones ARMAR / DESACTIVAR] --> GAME[Máquina de estados<br/>bucle 10 ms]
    GAME --> LED[Tira WS2812 x8]
    GAME --> BUZ[Buzzer]
    GAME -- "cola (no bloquea)" --> MQTTC[Cliente esp-mqtt<br/>tarea propia]
    MQTTC -- cola de comandos --> GAME
  end
  MQTTC <-- "MQTT 3.1.1 sobre TLS :8883" --> AIO[(Adafruit IO<br/>broker + feeds)]
  AIO <-- HTTPS --> DASH[Dashboard web/móvil<br/>árbitro]
```

**Estados:** `espera → plantada → desactivada | exploto → espera`

| Feed (Adafruit IO) | Dirección | Valores | QoS |
|---|---|---|---|
| `bomba-estado`   | ESP32 → nube | `espera`, `plantada`, `desactivada`, `exploto` | 1 |
| `bomba-tiempo`   | ESP32 → nube | 0–40 (s) | 0 |
| `bomba-progreso` | ESP32 → nube | 0–100 (%) | 0 |
| `bomba-cmd`      | nube → ESP32 | `armar`, `reiniciar` | 0 |

**Decisiones de diseño (manejo de errores):**
- El juego **nunca espera a la red**: la red corre en otras tareas y las publicaciones se encolan. Sin WiFi la bomba funciona igual (modo offline).
- WiFi y MQTT **se reconectan solos**. Al reconectar se reenvía el estado actual.
- **Límite de envío:** como máximo 1 mensaje cada 2.2 s, porque el plan gratis de Adafruit IO permite 30 por minuto. El estado tiene prioridad sobre la telemetría.
- **TLS con verificación de certificado** (bundle de certificados de ESP-IDF).
- **Antirrebote** de 30 ms en los botones. Si el botón verde ya estaba apretado antes de plantar, no cuenta.

## Hardware (BOM)

| Componente | Cant. | Notas |
|---|---|---|
| ESP32 DevKit V1 (ESP32-WROOM-32) | 1 | |
| Tira o anillo WS2812B de 8 LEDs | 1 | |
| Botón pulsador grande (rojo) | 1 | ARMAR |
| Botón pulsador grande (verde) | 1 | DESACTIVAR |
| Buzzer **activo** de 5 V o 3.3 V | 1 | Suena solo con darle voltaje |
| Resistencia de 330 Ω | 1 | En serie con el DIN de los LEDs |
| Protoboard, jumpers y power bank USB | — | |
| (Opcional) caja, cinta, "dinamita" de cartón | — | Para la estética |

## Conexiones

| ESP32 | Va a |
|---|---|
| GPIO18 | Botón rojo → GND (usa pull-up interno, no lleva resistencia) |
| GPIO19 | Botón verde → GND |
| GPIO23 | Buzzer (+). El (−) va a GND |
| GPIO16 | 330 Ω → DIN de la tira WS2812 |
| VIN (5 V) | 5V de la tira |
| GND | GND de la tira, botones y buzzer (tierra común) |

## Compilar y flashear

Requisitos: ESP-IDF **v5.5** (extensión de VS Code o terminal "ESP-IDF").

```bash
git clone <este-repo> && cd bomba_iot
cp main/config.example.h main/config.h   # y edita WiFi + usuario/AIO key
idf.py set-target esp32
idf.py build
idf.py -p COM3 flash monitor             # cambia COM3 por tu puerto
```
La primera vez que compiles, ESP-IDF descarga el componente `espressif/led_strip` (necesita internet).

## Configurar Adafruit IO

1. Crea una cuenta en https://io.adafruit.com. Tu usuario y tu **AIO Key** están en el icono de la llave; ponlos en `config.h`.
2. En **Feeds**, crea `bomba-estado`, `bomba-tiempo`, `bomba-progreso` y `bomba-cmd`.
3. En **Dashboards**, crea "Bomba" con estos bloques:
   - **Text** o **Indicator** → `bomba-estado`
   - **Gauge** de 0 a 40 → `bomba-tiempo`
   - **Gauge** de 0 a 100 → `bomba-progreso`
   - **Momentary Button**: press value `armar`, release value `-` → `bomba-cmd`
   - **Momentary Button**: press value `reiniciar`, release value `-` → `bomba-cmd`
   - **Line chart** → `bomba-tiempo` y `bomba-progreso` (historial)

## Logs para pruebas

El monitor serie imprime eventos en formato CSV:
```
I (12345) bomba: EVT,12345,plantada
I (30000) bomba: CMD,30000,reiniciar
```
Además, Adafruit IO guarda cada dato con su hora del servidor. Se puede descargar en CSV desde la página de cada feed.
