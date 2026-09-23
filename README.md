# fungi-controller

Controlador de fructificación basado en ESP32 DevKit (PlatformIO + Arduino).

## Conexiones

| Componente | Pin del componente | Pin del ESP32 | Notas |
|---|---|---|---|
| DHT22 | VCC (+) | 3V3 | |
| DHT22 | DATA (out) | GPIO4 | Si es el sensor suelto (4 patas, sin módulo), poner una resistencia de 10kΩ entre DATA y 3V3. Los módulos de 3 pines ya la traen. |
| DHT22 | GND (−) | GND | |
| Módulo MOSFET | TRIG/PWM | GPIO26 | Señal de encendido del humidificador (HIGH = encendido). |
| Módulo MOSFET | GND (junto a TRIG/PWM) | GND | GND común entre el ESP32 y la fuente del humidificador. |
| Módulo MOSFET | DC+ / DC− | Fuente 5V DC (+ / −) | Fuente aparte para el humidificador (≥1A). No usar el pin 5V/VIN del ESP32. |
| Módulo MOSFET | OUT+ / OUT− | Humidificador (+ / −) | Solo cargas de corriente continua (5–36V DC), no sirve para 220V. |
| Relé extractor | DC+ (VCC) | 3V3 | Módulo Tongling 3,3V DC (JQC-3FF-S-Z). |
| Relé extractor | DC− (GND) | GND | |
| Relé extractor | IN | GPIO27 | Señal del extractor. |
| Relé extractor | COM + NO | Cable de fase del extractor | Cortar el cable de fase y conectar un extremo a COM y el otro a NO. **220V: hacerlo sin tensión y bien aislado.** El relé soporta 10A / 250VAC. |
| Relé calefacción | DC+ (VCC) | 3V3 | Mismo modelo de módulo que el del extractor. |
| Relé calefacción | DC− (GND) | GND | |
| Relé calefacción | IN | GPIO25 | Señal del generador de calor. |
| Relé calefacción | COM + NO | Cable de fase del generador de calor | Igual que el extractor. **220V: hacerlo sin tensión y bien aislado.** Revisar que el consumo del calefactor no supere los 10A del relé. |
| Botón BOOT | — | GPIO0 | Ya viene en la placa. Mantenerlo apretado al encender borra las credenciales WiFi. |

Los pines se definen en `src/main.cpp` (`DHT_PIN`, `MOSFET_PIN`, `EXTRACTOR_PIN`, `HEATER_PIN`, `RESET_PIN`).

Se usa el contacto **NO** (normalmente abierto) de los relés para que, si el ESP32 está apagado, el extractor y la calefacción queden apagados. Los módulos usados se activan con señal baja (`RELAY_ACTIVE_HIGH = false`, vale para los dos relés). Si se cambian por unos que funcionen al revés (el aparato prende cuando el panel dice apagado), cambiar `RELAY_ACTIVE_HIGH` a `true` en `src/main.cpp` y volver a subir el firmware.

## Control de humedad

Desde la página se configuran el mínimo y el máximo de humedad y un interruptor **Control activo** que guarda al instante (se guardan en el ESP32 y sobreviven reinicios; por defecto activo, 85 % y 95 %):

- Control desactivado → humidificador apagado.

- Humedad por debajo del mínimo → humidificador encendido.
- Humedad igual o mayor al máximo → humidificador apagado.
- Entre los dos valores mantiene el estado anterior.
- Al guardar una configuración nueva se reevalúa desde cero: queda encendido solo si la humedad está por debajo del nuevo mínimo.
- Si falla la lectura del DHT22, el humidificador se apaga por seguridad.

## Calefacción

Mismo funcionamiento que el control de humedad, pero con la temperatura y el relé de calefacción. Se configuran la temperatura mínima y máxima (de 0 a 50 °C, en pasos de 0,5) y el interruptor **Control activo** (por defecto desactivado, 20 °C y 24 °C):

- Control desactivado → calefacción apagada.
- Temperatura por debajo de la mínima → calefacción encendida.
- Temperatura igual o mayor a la máxima → calefacción apagada.
- Entre los dos valores mantiene el estado anterior.
- Al guardar una configuración nueva se reevalúa desde cero: queda encendida solo si la temperatura está por debajo de la nueva mínima.
- Si falla la lectura del DHT22, la calefacción se apaga por seguridad.

## Extractor

Desde la página se configura un ciclo que alterna **tiempo apagado** y **tiempo encendido** (cada uno en segundos, minutos u horas, de 1 segundo a 24 horas), y un interruptor **Ciclo activo** que guarda al instante:

- El ciclo empieza siempre por la fase encendida (al activarlo, al guardar tiempos nuevos y al arrancar el ESP32).
- La configuración se guarda en el ESP32 y sobrevive cortes de luz. Por defecto: desactivado, 15 min apagado y 30 s encendido.
- La posición dentro del ciclo no se guarda: al volver la luz, el ciclo arranca de nuevo desde la fase encendida.
- Mientras el ESP32 se conecta al WiFi al arrancar, el extractor queda apagado; el ciclo empieza cuando ya está conectado.

## Primer uso

1. Subir el firmware y abrir el monitor serie:
   ```
   ./upload-and-monitor.sh
   ```
2. Conectarse a la red WiFi **`fungi-controller`** (clave `12345678`). Se abre el portal de configuración; si no, entrar a `http://192.168.4.1`.
3. Elegir la red WiFi de la casa e ingresar su clave.
4. Abrir `http://fungi-controller.local/` (o la IP que aparece en el monitor serie).

Si al arrancar no logra conectarse a la red guardada (por ejemplo, después de un corte de luz, cuando el router todavía no volvió), lo reintenta 3 veces de hasta 15 s cada una. Si falla las 3 veces, abre el portal `fungi-controller` durante 3 minutos y después se reinicia para volver a intentar.

## Endpoints

| Ruta | Respuesta |
|---|---|
| `/` | Página con temperatura y humedad en vivo |
| `/status` | `{"status":"OK"}` |
| `/sensors` | `{"temperature":24.3,"humidity":81.2,"ok":true,"humidifier":false,"heater":false,"extractor":true,"extractorRemaining":12}` (`extractorRemaining`: segundos hasta el próximo cambio, `null` si el ciclo está desactivado) |
| `GET /config` | Control de humedad: `{"enabled":true,"humMin":85,"humMax":95}` |
| `POST /config` | Parámetros de formulario `enabled` (`1`/`0`, opcional), `humMin` y `humMax`; responde la config guardada o 400 si no es válida |
| `GET /heater` | `{"enabled":false,"tempMin":20.0,"tempMax":24.0}` |
| `POST /heater` | Parámetros de formulario `enabled` (`1`/`0`), `tempMin` y `tempMax` (0 a 50); responde la config guardada o 400 si no es válida |
| `GET /extractor` | `{"enabled":true,"offSec":900,"onSec":30}` |
| `POST /extractor` | Parámetros de formulario `enabled` (`1`/`0`), `offSec` y `onSec` (1 a 86400); responde la config guardada o 400 si no es válida |
