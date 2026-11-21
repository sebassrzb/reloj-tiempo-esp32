# Reloj + tiempo + temporizador + pomodoro (ESP32 + 2 OLED)

Reloj de sobremesa con un ESP32 y dos pantallas OLED SSD1306 de 0,96" (128x64):

- **Pantalla 2:** previsión del tiempo siempre visible (temperatura actual con icono de día/noche, máxima/mínima y probabilidad de lluvia de hoy, y los 3 días siguientes). Datos de [Open-Meteo](https://open-meteo.com/) (gratis, sin API key), actualizados cada 30 min.
- **Pantalla 1:** tres modos que se cambian con un botón: **reloj** (hora por NTP con cambio horario automático), **temporizador** y **pomodoro**.

Los temporizadores siguen corriendo aunque estés en otro modo. Al terminar, la pantalla parpadea en invertido y suena el zumbador.

## Material

- ESP32 DevKit clásico
- 2 pantallas OLED SSD1306 0,96" I2C (128x64)
- 3 pulsadores
- Zumbador (pasivo por defecto; configurable como activo)
- Breadboard y cables

## Conexiones

Las dos pantallas van en buses I2C distintos, así ambas pueden usar la dirección `0x3C`.

| Elemento | Pin ESP32 |
|---|---|
| OLED 1 (reloj/modos) SDA / SCL | GPIO 21 / 22 |
| OLED 2 (tiempo) SDA / SCL | GPIO 18 / 19 |
| Botón MODO | GPIO 23 |
| Botón START | GPIO 5 |
| Botón AJUSTE | GPIO 4 |
| Zumbador | GPIO 2 |

Alimenta las dos pantallas a 3V3 y GND. Cada botón va entre su GPIO y GND (usa el pull-up interno, no hacen falta resistencias). El zumbador va entre el GPIO 2 y GND.

> En placas WROVER los GPIO 16 y 17 los usa la PSRAM; los pines de arriba no los tocan.

## Controles

| Botón | Pulsación corta | Pulsación larga |
|---|---|---|
| MODO | Cambia Reloj → Temporizador → Pomodoro | Vuelve al reloj |
| START | Inicia / pausa | Reinicia |
| AJUSTE | Reloj: baja/sube el brillo. Temporizador: siguiente tiempo. Pomodoro: salta de fase | Temporizador: tiempo anterior |

Con la alarma sonando, cualquier botón la para (si no, se para sola a los 20 s).

Tiempos del temporizador: 30 s, 1, 2, 3, 5, 10, 15, 20, 25, 30, 45 y 60 min. Pomodoro: 25 min de trabajo, 5 de descanso y 15 de descanso largo cada 4 pomodoros.

## Instalación

1. Instala el soporte de ESP32 en el IDE de Arduino y elige la placa **ESP32 Dev Module**.
2. Instala estas librerías desde el Gestor de librerías:
   - Adafruit SSD1306
   - Adafruit GFX Library
   - ArduinoJson (v7)
3. Abre `reloj_tiempo_modos/reloj_tiempo_modos.ino`.
4. **Cambia el WiFi:** busca estas dos líneas al principio del archivo y pon el nombre y la contraseña de tu red:
   ```cpp
   const char* WIFI_SSID = "TU_WIFI";
   const char* WIFI_PASS = "TU_PASSWORD";
   ```
   > ⚠️ **Hay que cambiarlos, si no el reloj no se conecta y no tendrá hora ni tiempo.**
   > ⚠️ **El ESP32 solo funciona con redes de 2,4 GHz. No se conecta a redes de 5 GHz.** Si tu router tiene las dos bandas con el mismo nombre, usa (o crea) una red de 2,4 GHz.
5. **Cambia la ubicación de la previsión:** los valores que trae son solo un ejemplo. Busca estas líneas y pon las coordenadas de tu ciudad (en Google Maps, clic derecho sobre tu ciudad y copia los dos números):
   ```cpp
   const float LAT = 40.4168;
   const float LON = -3.7038;
   ```
   > ⚠️ **Si no lo cambias, el reloj te mostrará el tiempo de la ubicación de ejemplo.** La zona horaria (`TZ_INFO`) está en hora de España peninsular; cámbiala si vives en otra zona horaria.
6. Sube el sketch a la placa.

## Configuración

Al principio del `.ino` puedes cambiar:

- `LAT` / `LON`: ubicación de la previsión (**cámbiala por la tuya**).
- `TZ_INFO`: zona horaria (por defecto España peninsular).
- `BUZZER_PIN`, `BUZZER_PASSIVE`, `BUZZER_FREQ`: pin y tipo de zumbador (`-1` si no tienes). Con un zumbador activo, pon `BUZZER_PASSIVE = false`.
- `POMO_*` y `AUTO_START_NEXT`: duraciones del pomodoro y si la siguiente fase arranca sola.
- `TIMER_PRESETS_S`: lista de tiempos del temporizador, en segundos.

## Problemas típicos

- **"Sin datos del tiempo":** comprueba que has puesto tu WiFi en el sketch, que es de 2,4 GHz (no vale 5 GHz) y mira el Monitor Serie a 115200 baudios. Si falla al arrancar, reintenta a los 2 minutos.
- **Una pantalla no enciende:** revisa pines y dirección I2C (`0x3C` o `0x3D`).
- **Zumbador flojo o con solo un clic:** si es pasivo, prueba otras frecuencias (2000-3500 Hz); si es activo, usa `BUZZER_PASSIVE = false`.
