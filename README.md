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
- Si el DHT22 deja de leer, se sigue usando la última lectura válida hasta **2 minutos** (el panel muestra `REINTENTANDO`); si en ese tiempo no vuelve, el humidificador se apaga por seguridad (`ERROR SENSOR`).

## Calefacción

Se configuran la temperatura mínima y máxima (de 0 a 50 °C, en pasos de 0,5) y el interruptor **Control activo** (por defecto desactivado, 20 °C y 24 °C). El control apunta al **medio** entre la mínima y la máxima y se adapta solo a cada carpa y calefactor:

1. **Aprendiendo** (la primera vez, o con el botón **Volver a aprender**): funciona como la humedad, prende por debajo de la mínima y apaga al llegar a la máxima. En cada ciclo mide cuánto tarda la carpa en notar el calefactor (el retardo que la hace pasarse de la máxima), qué tan rápido sube prendido y qué tan rápido baja apagado. Después de 2 ciclos completos pasa a regular.
2. **Regulando**: un control PI prende el calefactor una parte de cada ventana (por ejemplo 40 s de cada 4 min) en vez de tandas largas, así nunca junta tanto calor como para pasarse. La ventana y los parámetros salen de lo que aprendió (reglas SIMC); la parte integral compensa sola los cambios de temperatura de afuera. El panel muestra la potencia (% de cada ventana).

Siempre se cumple:

- Control desactivado → calefacción apagada.
- Temperatura igual o mayor a la máxima → calefacción apagada; por debajo de la mínima → prendida.
- Si se sale del rango 3 veces en 3 horas, se asume que lo aprendido ya no sirve (se cambió el calefactor, la carpa, etc.) y vuelve a aprender.
- Lo aprendido se guarda en el ESP32 y sobrevive reinicios.
- Si el DHT22 deja de leer, se sigue usando la última lectura válida hasta **2 minutos**; si en ese tiempo no vuelve, la calefacción se apaga por seguridad.

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
2. En el monitor serie aparece el nombre del equipo, por ejemplo `Dispositivo: fungi-a1b2c3`. Conectarse a la red WiFi con ese nombre (clave `12345678`). Se abre el portal de configuración; si no, entrar a `http://192.168.4.1`.
3. Elegir la red WiFi de la casa e ingresar su clave.
4. Abrir `http://fungi-a1b2c3.local/` (o la IP que aparece en el monitor serie).

Si al arrancar no logra conectarse a la red guardada (por ejemplo, después de un corte de luz, cuando el router todavía no volvió), lo reintenta 3 veces de hasta 15 s cada una. Si falla las 3 veces, abre el portal (con el nombre del equipo) durante 3 minutos y después se reinicia para volver a intentar.

## Actualizar por WiFi (OTA)

Después de la primera carga por USB, las versiones nuevas se pueden subir por WiFi desde la PC:

```
./upload-ota.sh carpa-1 carpa-2          # a esos equipos (nombre o IP)
./upload-ota.sh                          # busca los controladores de la red y pregunta antes de subir
```

- Compila una sola vez y sube a cada equipo en orden; al final muestra un resumen (OK / ERROR por equipo). Si uno falla, sigue con el siguiente.
- La PC tiene que estar en la misma WiFi y el equipo prendido. Sin argumentos, busca los equipos por mDNS (`avahi-browse`, si está instalado) o preguntando a cada IP de la red local.
- Durante la actualización (30–60 s) se **apagan el humidificador, la calefacción y el extractor**; al terminar el equipo se reinicia y vuelve a controlar solo. En el Registro aparece "Inicio (motivo: reinicio por software)".
- Si la actualización falla, el equipo sigue con la versión anterior.
- La versión que corre cada equipo (hash de git y fecha de compilación) se ve en la tarjeta **Dispositivo** del panel.
- **Sin clave:** cualquiera en la misma WiFi podría subirle un firmware. Usarlo solo en redes de confianza.
- **Importante:** para tener lugar para dos versiones del programa se usa la tabla de particiones `min_spiffs.csv`. Los equipos que tenían un firmware anterior a este cambio necesitan **una última carga por USB** (`./upload-and-monitor.sh`); la WiFi y la configuración guardadas se conservan. `upload-and-monitor.sh` sigue sirviendo para emergencias.

## Varios controladores

Se pueden tener varios equipos andando en la misma WiFi:

- Cada equipo arranca con un nombre único, `fungi-XXXXXX`, donde `XXXXXX` sale de la MAC del chip. Ese nombre es su dirección (`http://fungi-XXXXXX.local`) y el nombre de su red de configuración WiFi.
- Desde la tarjeta **Dispositivo** del panel se le puede poner otro nombre, por ejemplo `carpa-1` → `http://carpa-1.local`. Solo letras minúsculas, números y guiones (1 a 32). El cambio es inmediato y se guarda en el ESP32.
- La tarjeta **Dispositivos**, arriba de todo, muestra todos los controladores de la red con su temperatura, humedad y qué aparatos tienen prendidos (HUM, CAL, EXT). Cada nombre es un link a su panel. Los equipos se encuentran solos por mDNS; uno nuevo puede tardar hasta 30 s en aparecer.
- Cada equipo se configura desde su propio panel.

## Historial (InfluxDB)

Cada equipo puede guardar su historial en una base **InfluxDB 2.x**: temperatura y humedad cada tantos minutos (campo **Cada (minutos)**, de 1 a 60; por defecto 5) y cuántos segundos estuvieron encendidos el humidificador y la calefacción en cada intervalo. Se configura desde la tarjeta **Historial (InfluxDB)** del panel, en cualquier momento después de configurar el equipo.

### Preparar InfluxDB Cloud (recomendado)

1. Crear una cuenta gratis en InfluxDB Cloud y anotar la **URL de la región** (por ejemplo `https://us-east-1-1.aws.cloud2.influxdata.com`) y el nombre de la **organización** (suele ser el email de la cuenta).
2. **Load Data → Buckets → Create Bucket**: por ejemplo `cultivo`, con **Delete data: older than 7 days**. Así InfluxDB borra solo lo que tenga más de una semana.
3. **Load Data → API Tokens → Generate API Token → Custom API Token**: permiso de **lectura y escritura** sobre ese bucket (la lectura sirve para mostrar gráficos en el panel más adelante). Copiar el token, se ve una sola vez.
4. En el panel del equipo: completar URL, organización, bucket y token, activar **Guardar historial**, **Guardar** y después **Probar envío**. Tiene que decir "Envío correcto".

Alternativa local con Docker: `docker run -d -p 8086:8086 -v influxdb:/var/lib/influxdb2 influxdb:2`, hacer el setup inicial en `http://localhost:8086` (ahí se crean la organización y un bucket; ponerle retención de 7 días) y usar `http://<IP de esa PC>:8086` como URL.

### Qué se guarda

```
ambiente,device=carpa-1,id=0c2cc8 temperatura=21.3,humedad=85.2 <epoch>
actuadores,device=carpa-1,id=0c2cc8 humidificador_seg=120i,calefaccion_seg=0i <epoch>
```

- `device` es el nombre del equipo; `id` sale de la MAC y no cambia aunque se renombre.
- `ambiente` se omite si en ese momento la lectura del DHT22 es inválida.
- `humidificador_seg` / `calefaccion_seg` son los segundos encendido dentro de cada intervalo: sumándolos se obtiene el tiempo total de cualquier período.
- La hora se toma por NTP (UTC). Hasta sincronizarla, no se toman muestras.
- Si no hay conexión o InfluxDB no responde, las muestras quedan en la RAM del equipo (hasta ~12 h, máximo 144 muestras) y se envían en el próximo intento. Se pierden si el ESP32 se reinicia.
- El envío es por `https` cifrado pero sin verificar el certificado del servidor.
- El token nunca se devuelve al panel: para cambiarlo se escribe uno nuevo; si el campo queda vacío se mantiene el guardado.

Ejemplos de consulta (Flux), en **Data Explorer → Script Editor**:

```flux
// Temperatura y humedad de la última semana de un equipo
from(bucket: "cultivo")
  |> range(start: -7d)
  |> filter(fn: (r) => r._measurement == "ambiente" and r.device == "carpa-1")

// Horas encendido por día del humidificador y la calefacción
from(bucket: "cultivo")
  |> range(start: -7d)
  |> filter(fn: (r) => r._measurement == "actuadores" and r.device == "carpa-1")
  |> aggregateWindow(every: 1d, fn: sum)
  |> map(fn: (r) => ({ r with _value: float(v: r._value) / 3600.0 }))
```

## Gráficos

La tarjeta **Calefacción · historial** (debajo de la de Calefacción) muestra la temperatura de la última **1 h, 2 h, 6 h, 24 h o 7 días** en barras:

- **Barra azul:** en esa ventana la calefacción estuvo prendida. **Barra gris:** estuvo apagada.
- Las líneas punteadas son la mínima y la máxima configuradas.
- Pasando el mouse (o tocando) una barra se ve la hora, la temperatura promedio y cuántos minutos estuvo prendida.
- Cada barra es una ventana de 1 min (1 h, 2 h y 6 h), 5 min (24 h) o 30 min (7 d), o el intervalo de muestreo si es mayor. Para ver bien 1 h y 2 h conviene muestrear cada 1 min (con 5 min, 1 h son solo 12 barras).

Debajo, la **tabla de ciclos** (un ciclo = una racha de calefacción prendida), para entender cuánto calienta y cuánto mantiene:

| Columna | Qué es |
|---|---|
| Prendida | Tiempo que estuvo prendida en ese ciclo |
| Sube | Temperatura al empezar → la más alta alcanzada, incluida la que sigue subiendo después de apagarse (inercia) |
| °C/min prendida | Cuánto sube por minuto mientras está prendida |
| Se mantuvo | Cuánto tardó, desde que se apagó, en volver a necesitar calefacción |
| Cae | Cuántos °C por hora pierde desde el pico hasta volver a prender |

La última fila es el promedio. Los valores son aproximados al tamaño de la ventana: para más detalle, bajar el intervalo de muestreo (por ejemplo a 1 min) en la tarjeta Historial.

Los datos se leen de InfluxDB a través del propio ESP32 (el token no sale del equipo), así que hace falta el historial configurado y un token con permiso de **lectura** además de escritura. El gráfico no usa librerías externas: funciona aunque el celular no tenga internet, siempre que el ESP32 sí lo tenga.

## Registro

La tarjeta **Registro**, al final del panel, muestra los últimos 100 mensajes del equipo, los mismos que salen por el monitor serie: arranque, conexión WiFi, cambios de configuración, cuándo se prenden y apagan los aparatos y errores del sensor. Se actualiza cada 5 s.

- El primer mensaje indica el **motivo del último reinicio** (corte de luz, cuelgue, caída de tensión, etc.).
- Los mensajes repetidos seguidos se agrupan con un contador (por ejemplo `Error leyendo el DHT22 (t=nan h=nan) ×37`).
- Los errores se ven en naranja.
- **Pausar** congela la lista para leer tranquilo; **Copiar** copia todo el registro al portapapeles.
- Se guardan en RAM, así que **se borran al reiniciar** el ESP32.
- Los mensajes internos de WiFiManager (`*wm:...`) solo se ven por el monitor serie.

## Endpoints

| Ruta | Respuesta |
|---|---|
| `/` | Página con temperatura y humedad en vivo |
| `/status` | `{"status":"OK"}` |
| `GET /device` | Este equipo: `{"id":"a1b2c3","name":"carpa-1","version":"039601a 2026-09-24 13:43","ip":"192.168.1.142"}` |
| `POST /device` | Parámetro de formulario `name`; cambia el nombre (400 si no es válido) |
| `GET /logs?since=<seq>` | Registro: `{"now":123456,"last":42,"entries":[{"seq":42,"ms":120000,"repeat":1,"text":"Extractor ENCENDIDO"}]}` con los mensajes posteriores a `since` (`now` y `ms` son milisegundos desde el arranque) |
| `GET /history-config` | Historial: `{"enabled":true,"url":"…","org":"…","bucket":"…","tokenSet":true,"intervalMin":5,"pending":0,"lastOk":1760000000,"lastError":""}` (el token nunca se devuelve) |
| `POST /history-config` | Parámetros de formulario `enabled` (`1`/`0`), `url`, `org`, `bucket`, `token` (vacío = mantener el guardado) e `interval` (minutos, 1–60); 400 si no es válida |
| `GET /history?range=1h\|2h\|6h\|24h\|7d` | CSV de InfluxDB con `_time`, `temperatura` (promedio) y `calefaccion_seg` (suma) por ventana; el header `X-Window-Min` indica el tamaño de la ventana. 409 si el historial no está configurado, 502 si InfluxDB responde error |
| `POST /history-test` | Toma una muestra y la envía ya; responde el estado o `{"error":"…"}` con el motivo |
| `GET /devices` | Controladores encontrados en la red, este primero: `[{"name":"carpa-1","ip":"…","self":true}, …]` |
| `/sensors` | `{"temperature":24.3,"humidity":81.2,"ok":true,"retrying":false,"humidifier":false,"heater":false,"extractor":true,"extractorRemaining":12}` (`ok`: hay una lectura válida de hace menos de 2 min; `retrying`: la última lectura falló y se usa la anterior; `extractorRemaining`: segundos hasta el próximo cambio, `null` si el ciclo está desactivado). Permite lecturas desde otros equipos (CORS) |
| `GET /config` | Control de humedad: `{"enabled":true,"humMin":85,"humMax":95}` |
| `POST /config` | Parámetros de formulario `enabled` (`1`/`0`, opcional), `humMin` y `humMax`; responde la config guardada o 400 si no es válida |
| `GET /heater` | `{"enabled":false,"tempMin":20.0,"tempMax":24.0}` |
| `POST /heater` | Parámetros de formulario `enabled` (`1`/`0`), `tempMin` y `tempMax` (0 a 50); responde la config guardada o 400 si no es válida |
| `GET /extractor` | `{"enabled":true,"offSec":900,"onSec":30}` |
| `POST /extractor` | Parámetros de formulario `enabled` (`1`/`0`), `offSec` y `onSec` (1 a 86400); responde la config guardada o 400 si no es válida |
