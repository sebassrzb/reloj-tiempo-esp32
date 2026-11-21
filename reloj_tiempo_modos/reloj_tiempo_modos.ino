/*
  Reloj + tiempo + temporizador + pomodoro
  2 OLED SSD1306 0.96" (128x64) + ESP32 (DevKit clásico) + 3 pulsadores

  OLED 1 (reloj / temporizador / pomodoro) -> I2C 0: SDA=21, SCL=22
  OLED 2 (previsión del tiempo, siempre)   -> I2C 1: SDA=18, SCL=19

  Botones (un pin al GPIO y el otro a GND, usan pull-up interno):
    MODO  -> GPIO 23
    START -> GPIO 5
    AJUSTE-> GPIO 4
  Zumbador (pasivo por defecto) -> GPIO 2 y GND (BUZZER_PIN = -1 si no lo tienes)

  Controles
    MODO  (corto): cambia Reloj -> Temporizador -> Pomodoro
    MODO  (largo): vuelve al reloj
    START (corto): iniciar / pausar     START (largo): reiniciar
    AJUSTE: Reloj = bajar/subir brillo
            Temporizador = siguiente tiempo (largo = anterior)
            Pomodoro = saltar a la siguiente fase
    Cuando suena la alarma, cualquier botón la para.

  Librerías: Adafruit SSD1306, Adafruit GFX Library, ArduinoJson (v7)
  Datos del tiempo: Open-Meteo (gratis, sin API key)
*/

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// ---------- CONFIGURACIÓN ----------
// >>>>>>>>>> CAMBIA ESTOS DOS VALORES POR LOS DE TU WIFI <<<<<<<<<<
// IMPORTANTE: el ESP32 SOLO funciona con redes de 2,4 GHz. NO se conecta a 5 GHz.
// Si tu router tiene las dos bandas con el mismo nombre, usa (o crea) una red de 2,4 GHz.
const char* WIFI_SSID = "TU_WIFI";          // <- nombre de tu red WiFi (2,4 GHz)
const char* WIFI_PASS = "TU_PASSWORD";      // <- contraseña de tu red WiFi
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>

// >>>>>>>>>> CAMBIA LA UBICACIÓN POR LA TUYA <<<<<<<<<<
// Los valores de abajo son solo un ejemplo. Pon las coordenadas de tu ciudad
// (en Google Maps: clic derecho sobre tu ciudad y copia los dos números).
// Latitud: positiva = norte. Longitud: negativa = oeste (en España casi siempre es negativa).
const float LAT = 40.4168;    // <- tu latitud
const float LON = -3.7038;    // <- tu longitud
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>

// Zona horaria España peninsular (cambio horario automático)
const char* TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";

const unsigned long WEATHER_EVERY_MS = 30UL * 60UL * 1000UL;  // cada 30 min

// Pines I2C
#define SDA_1 21
#define SCL_1 22
#define SDA_2 18
#define SCL_2 19

// Botones y zumbador
#define BTN_MODE_PIN 23
#define BTN_START_PIN 5
#define BTN_ADJ_PIN 4
const int8_t BUZZER_PIN = 2;          // -1 = sin zumbador
const bool BUZZER_PASSIVE = true;     // true = zumbador pasivo, false = activo
const uint16_t BUZZER_FREQ = 2700;    // frecuencia del tono (solo pasivo); prueba 2000-3500

// Pomodoro
const uint8_t POMO_WORK_MIN = 25;
const uint8_t POMO_SHORT_MIN = 5;
const uint8_t POMO_LONG_MIN = 15;
const uint8_t POMO_ROUNDS = 4;        // pomodoros hasta el descanso largo
const bool AUTO_START_NEXT = false;   // true = la siguiente fase arranca sola

// Alarma
const unsigned long ALARM_MAX_MS = 20000UL;  // se para sola a los 20 s
// -----------------------------------

Adafruit_SSD1306 clockOled(128, 64, &Wire, -1);
Adafruit_SSD1306 weatherOled(128, 64, &Wire1, -1);

const char* DIAS_LARGO[] = {"domingo", "lunes", "martes", "miercoles", "jueves", "viernes", "sabado"};
const char* DIAS_CORTO[] = {"Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab"};
const char* MESES[] = {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"};

struct Weather {
  bool valid = false;
  float temp = 0;
  int code = 0;
  bool isDay = true;
  float tmax[4], tmin[4];
  int rain[4];
  char day[4][4];
} w;

unsigned long lastWeather = 0;
bool weatherDirty = true;

// ---------- Botones ----------
struct Button {
  uint8_t pin = 0;
  bool stable = HIGH, lastRead = HIGH, longFired = false;
  uint32_t lastChange = 0, pressedAt = 0;

  void begin(uint8_t p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
  }

  // 0 = nada, 1 = pulsación corta (al soltar), 2 = pulsación larga
  uint8_t update(uint32_t now) {
    bool r = digitalRead(pin);
    if (r != lastRead) {
      lastRead = r;
      lastChange = now;
    }
    uint8_t ev = 0;
    if (now - lastChange > 30 && r != stable) {
      stable = r;
      if (stable == LOW) {
        pressedAt = now;
        longFired = false;
      } else if (!longFired) {
        ev = 1;
      }
    }
    if (stable == LOW && !longFired && now - pressedAt > 800) {
      longFired = true;
      ev = 2;
    }
    return ev;
  }
};

Button btnMode, btnStart, btnAdj;

// ---------- Estado de los modos ----------
enum UiMode { M_CLOCK, M_TIMER, M_POMO };
UiMode mode = M_CLOCK;

bool needRedraw = true;
int32_t lastSig = -2;
bool dimmed = false;

// Temporizador
const uint16_t TIMER_PRESETS_S[] = {30, 60, 120, 180, 300, 600, 900, 1200, 1500, 1800, 2700, 3600};
const uint8_t N_PRESETS = sizeof(TIMER_PRESETS_S) / sizeof(TIMER_PRESETS_S[0]);
uint8_t timerPreset = 4;  // empieza en 5 min
uint32_t timerSetMs = 300000UL;
uint32_t timerRemaining = 300000UL;
uint32_t timerEndAt = 0;
bool timerRunning = false;

// Pomodoro
enum Phase { P_WORK, P_SHORT, P_LONG };
Phase pomoPhase = P_WORK;
uint8_t pomoDone = 0;
bool pomoRunning = false;
uint32_t pomoEnd = 0;
uint32_t pomoRemaining = POMO_WORK_MIN * 60000UL;

// Alarma
bool alarmActive = false;
bool alarmOn = false;
uint32_t alarmStart = 0;
const char* alarmMsg = "";

// ---------- Iconos del tiempo (centrados en cx, cy; ~20x20 px) ----------
void iconSun(Adafruit_SSD1306& d, int cx, int cy) {
  d.fillCircle(cx, cy, 5, WHITE);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    d.drawLine(cx + cos(a) * 8, cy + sin(a) * 8, cx + cos(a) * 11, cy + sin(a) * 11, WHITE);
  }
}

void iconMoon(Adafruit_SSD1306& d, int cx, int cy) {
  d.fillCircle(cx, cy, 8, WHITE);
  d.fillCircle(cx + 5, cy - 4, 7, BLACK);
}

void iconCloud(Adafruit_SSD1306& d, int cx, int cy) {
  d.fillCircle(cx - 5, cy + 2, 4, WHITE);
  d.fillCircle(cx + 1, cy - 1, 5, WHITE);
  d.fillCircle(cx + 6, cy + 2, 4, WHITE);
  d.fillRect(cx - 5, cy + 2, 12, 5, WHITE);
}

void iconPartly(Adafruit_SSD1306& d, int cx, int cy) {
  d.fillCircle(cx - 4, cy - 4, 4, WHITE);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    d.drawLine(cx - 4 + cos(a) * 6, cy - 4 + sin(a) * 6, cx - 4 + cos(a) * 9, cy - 4 + sin(a) * 9, WHITE);
  }
  d.fillCircle(cx + 1, cy + 3, 4, WHITE);
  d.fillCircle(cx + 7, cy + 1, 5, WHITE);
  d.fillRect(cx + 1, cy + 3, 11, 5, WHITE);
}

void iconPartlyNight(Adafruit_SSD1306& d, int cx, int cy) {
  d.fillCircle(cx - 4, cy - 4, 6, WHITE);
  d.fillCircle(cx - 1, cy - 7, 5, BLACK);
  d.fillCircle(cx + 1, cy + 3, 4, WHITE);
  d.fillCircle(cx + 7, cy + 1, 5, WHITE);
  d.fillRect(cx + 1, cy + 3, 11, 5, WHITE);
}

void iconRain(Adafruit_SSD1306& d, int cx, int cy) {
  iconCloud(d, cx, cy - 4);
  for (int i = -1; i <= 1; i++) d.drawLine(cx + i * 5 + 1, cy + 5, cx + i * 5 - 1, cy + 9, WHITE);
}

void iconSnow(Adafruit_SSD1306& d, int cx, int cy) {
  iconCloud(d, cx, cy - 4);
  for (int i = -1; i <= 1; i++) d.fillCircle(cx + i * 5, cy + 7, 1, WHITE);
}

void iconStorm(Adafruit_SSD1306& d, int cx, int cy) {
  iconCloud(d, cx, cy - 4);
  d.drawLine(cx + 2, cy + 4, cx - 2, cy + 8, WHITE);
  d.drawLine(cx - 2, cy + 8, cx + 3, cy + 8, WHITE);
  d.drawLine(cx + 3, cy + 8, cx - 1, cy + 12, WHITE);
}

void iconFog(Adafruit_SSD1306& d, int cx, int cy) {
  for (int i = -1; i <= 1; i++) d.drawFastHLine(cx - 9, cy + i * 5, 18, WHITE);
}

void drawIcon(Adafruit_SSD1306& d, int cx, int cy, int code, bool isDay) {
  if (code == 0) { if (isDay) iconSun(d, cx, cy); else iconMoon(d, cx, cy); }
  else if (code <= 2) { if (isDay) iconPartly(d, cx, cy); else iconPartlyNight(d, cx, cy); }
  else if (code == 3) iconCloud(d, cx, cy);
  else if (code == 45 || code == 48) iconFog(d, cx, cy);
  else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) iconRain(d, cx, cy);
  else if ((code >= 71 && code <= 77) || code == 85 || code == 86) iconSnow(d, cx, cy);
  else if (code >= 95) iconStorm(d, cx, cy);
  else iconCloud(d, cx, cy);
}

// ---------- Utilidades de dibujo ----------
void printCentered(Adafruit_SSD1306& d, const char* s, int y, uint8_t size) {
  int16_t x1, y1;
  uint16_t tw, th;
  d.setTextSize(size);
  d.getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  d.setCursor((128 - tw) / 2, y);
  d.print(s);
}

void drawBar(Adafruit_SSD1306& d, int y, float frac) {
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  d.drawRect(0, y, 128, 6, WHITE);
  d.fillRect(0, y, (int)(frac * 128), 6, WHITE);
}

// ---------- Cálculo de tiempos restantes ----------
uint32_t timerRemainingNow(uint32_t now) {
  if (timerRunning) {
    long r = (long)(timerEndAt - now);
    return r > 0 ? (uint32_t)r : 0;
  }
  return timerRemaining;
}

uint32_t phaseMs(Phase p) {
  switch (p) {
    case P_WORK: return POMO_WORK_MIN * 60000UL;
    case P_SHORT: return POMO_SHORT_MIN * 60000UL;
    default: return POMO_LONG_MIN * 60000UL;
  }
}

uint32_t pomoRemainingNow(uint32_t now) {
  if (pomoRunning) {
    long r = (long)(pomoEnd - now);
    return r > 0 ? (uint32_t)r : 0;
  }
  return pomoRemaining;
}

void advancePhase(bool completed, uint32_t now) {
  if (pomoPhase == P_WORK) {
    if (completed) pomoDone++;
    pomoPhase = (completed && pomoDone % POMO_ROUNDS == 0) ? P_LONG : P_SHORT;
  } else {
    pomoPhase = P_WORK;
  }
  pomoRemaining = phaseMs(pomoPhase);
  pomoRunning = completed && AUTO_START_NEXT;
  if (pomoRunning) pomoEnd = now + pomoRemaining;
}

// ---------- Alarma ----------
// Enciende o apaga el zumbador (tono si es pasivo, nivel alto/bajo si es activo)
void buzzer(bool on) {
  if (BUZZER_PIN < 0) return;
  if (BUZZER_PASSIVE) ledcWriteTone(BUZZER_PIN, on ? BUZZER_FREQ : 0);
  else digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
}

void startAlarm(const char* msg, uint32_t now) {
  alarmActive = true;
  alarmOn = false;
  alarmStart = now;
  alarmMsg = msg;
  needRedraw = true;
}

void stopAlarm() {
  alarmActive = false;
  alarmOn = false;
  clockOled.invertDisplay(false);
  buzzer(false);
  needRedraw = true;
}

void tickAlarm(uint32_t now) {
  if (!alarmActive) return;
  if (now - alarmStart > ALARM_MAX_MS) {
    stopAlarm();
    return;
  }
  bool on = ((now - alarmStart) / 400) % 2 == 0;
  if (on != alarmOn) {
    alarmOn = on;
    clockOled.invertDisplay(on);
    buzzer(on);
  }
}

// ---------- Pantallas del OLED 1 ----------
void drawClockMode() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return;

  Adafruit_SSD1306& d = clockOled;
  d.clearDisplay();
  d.setTextColor(WHITE);

  char date[32];
  snprintf(date, sizeof(date), "%s %d %s", DIAS_LARGO[t.tm_wday], t.tm_mday, MESES[t.tm_mon]);
  printCentered(d, date, 2, 1);

  char hm[6];
  snprintf(hm, sizeof(hm), (t.tm_sec % 2 == 0) ? "%02d:%02d" : "%02d %02d", t.tm_hour, t.tm_min);
  d.setTextSize(4);
  d.setCursor(4, 18);
  d.print(hm);

  d.drawRect(0, 58, 128, 6, WHITE);
  d.fillRect(0, 58, (t.tm_sec * 128) / 59, 6, WHITE);

  d.display();
}

void drawTimerMode(uint32_t now) {
  Adafruit_SSD1306& d = clockOled;
  uint32_t rem = timerRemainingNow(now);
  uint32_t secs = (rem + 999) / 1000;

  d.clearDisplay();
  d.setTextColor(WHITE);
  printCentered(d, "TEMPORIZADOR", 2, 1);

  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
  printCentered(d, buf, 18, 4);

  if (timerRunning) {
    drawBar(d, 58, 1.0f - (float)rem / (float)timerSetMs);
  } else if (rem < timerSetMs) {
    printCentered(d, "PAUSA  ST:seguir", 54, 1);
  } else {
    printCentered(d, "AJ:tiempo  ST:inicio", 54, 1);
  }
  d.display();
}

void drawPomoMode(uint32_t now) {
  Adafruit_SSD1306& d = clockOled;
  uint32_t rem = pomoRemainingNow(now);
  uint32_t secs = (rem + 999) / 1000;

  d.clearDisplay();
  d.setTextColor(WHITE);

  const char* name = pomoPhase == P_WORK ? "TRABAJO" : (pomoPhase == P_SHORT ? "DESCANSO" : "DESCANSO LARGO");
  printCentered(d, name, 2, 1);
  if (!pomoRunning) {
    d.setTextSize(1);
    d.setCursor(116, 2);
    d.print("||");
  }

  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
  printCentered(d, buf, 14, 3);

  // Puntos: pomodoros completados en la ronda actual
  uint8_t filled = (pomoPhase == P_LONG) ? POMO_ROUNDS : (pomoDone % POMO_ROUNDS);
  for (uint8_t i = 0; i < POMO_ROUNDS; i++) {
    int cx = 64 + (int)((i - (POMO_ROUNDS - 1) / 2.0f) * 14);
    if (i < filled) d.fillCircle(cx, 47, 3, WHITE);
    else d.drawCircle(cx, 47, 3, WHITE);
  }

  drawBar(d, 57, 1.0f - (float)rem / (float)phaseMs(pomoPhase));
  d.display();
}

void drawAlarm() {
  Adafruit_SSD1306& d = clockOled;
  d.clearDisplay();
  d.setTextColor(WHITE);
  printCentered(d, alarmMsg, 4, 1);
  printCentered(d, "FIN!", 20, 4);
  printCentered(d, "pulsa un boton", 54, 1);
  d.display();
}

void drawClockScreen(uint32_t now) {
  if (alarmActive) {
    drawAlarm();
    return;
  }
  switch (mode) {
    case M_CLOCK: drawClockMode(); break;
    case M_TIMER: drawTimerMode(now); break;
    case M_POMO: drawPomoMode(now); break;
  }
}

// ---------- Pantalla del tiempo (OLED 2) ----------
void drawWeather() {
  Adafruit_SSD1306& d = weatherOled;
  d.clearDisplay();
  d.setTextColor(WHITE);

  if (!w.valid) {
    d.setTextSize(1);
    d.setCursor(20, 28);
    d.print("Sin datos del tiempo");
    d.display();
    return;
  }

  drawIcon(d, 14, 15, w.code, w.isDay);

  d.setTextSize(2);
  d.setCursor(34, 2);
  d.print((int)round(w.temp));
  d.write(248);  // símbolo de grado (CP437)
  d.print("C");

  d.setTextSize(1);
  d.setCursor(34, 21);
  d.printf("%d/%d  %d%%", (int)round(w.tmax[0]), (int)round(w.tmin[0]), w.rain[0]);

  d.drawFastHLine(0, 33, 128, WHITE);

  for (int i = 1; i <= 3; i++) {
    int x = (i - 1) * 43 + 2;
    d.setCursor(x, 37);
    d.print(w.day[i]);
    d.setCursor(x, 47);
    d.printf("%d/%d", (int)round(w.tmax[i]), (int)round(w.tmin[i]));
    d.setCursor(x, 56);
    d.printf("%d%%", w.rain[i]);
    if (i < 3) d.drawFastVLine(x + 39, 36, 28, WHITE);
  }

  d.display();
}

// ---------- Descarga de la previsión ----------
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + String(LAT, 4) +
               "&longitude=" + String(LON, 4) +
               "&current=temperature_2m,weather_code,is_day" +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max" +
               "&timezone=auto&forecast_days=4";

  WiFiClientSecure client;
  client.setInsecure();  // sin verificar certificado (suficiente para un dato de tiempo)
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.useHTTP10(true);  // evita la respuesta "chunked", que rompe la lectura del JSON
  http.setTimeout(5000);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("Open-Meteo HTTP %d (%s)\n", code, HTTPClient::errorToString(code).c_str());
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.println(err.c_str());
    return false;
  }

  w.temp = doc["current"]["temperature_2m"] | 0.0f;
  w.code = doc["current"]["weather_code"] | 0;
  w.isDay = (doc["current"]["is_day"] | 1) == 1;

  for (int i = 0; i < 4; i++) {
    w.tmax[i] = doc["daily"]["temperature_2m_max"][i] | 0.0f;
    w.tmin[i] = doc["daily"]["temperature_2m_min"][i] | 0.0f;
    w.rain[i] = doc["daily"]["precipitation_probability_max"][i] | 0;

    const char* s = doc["daily"]["time"][i] | "";
    int y, m, dd;
    struct tm tmDay = {};
    if (sscanf(s, "%d-%d-%d", &y, &m, &dd) == 3) {
      tmDay.tm_year = y - 1900;
      tmDay.tm_mon = m - 1;
      tmDay.tm_mday = dd;
      tmDay.tm_hour = 12;
      mktime(&tmDay);
      strcpy(w.day[i], DIAS_CORTO[tmDay.tm_wday]);
    } else {
      strcpy(w.day[i], "---");
    }
  }

  w.valid = true;
  return true;
}

// ---------- Botones: qué hace cada uno ----------
void handleButtons(uint8_t evMode, uint8_t evStart, uint8_t evAdj, uint32_t now) {
  if (!evMode && !evStart && !evAdj) return;

  if (alarmActive) {  // cualquier botón para la alarma
    stopAlarm();
    return;
  }
  needRedraw = true;

  if (evMode == 1) {
    mode = (UiMode)((mode + 1) % 3);
    return;
  }
  if (evMode == 2) {
    mode = M_CLOCK;
    return;
  }

  if (mode == M_CLOCK) {
    if (evAdj == 1) {
      dimmed = !dimmed;
      clockOled.dim(dimmed);
      weatherOled.dim(dimmed);
    }
  } else if (mode == M_TIMER) {
    if (evStart == 1) {
      if (timerRunning) {
        timerRemaining = timerRemainingNow(now);
        timerRunning = false;
      } else {
        if (timerRemaining == 0) timerRemaining = timerSetMs;
        timerEndAt = now + timerRemaining;
        timerRunning = true;
      }
    } else if (evStart == 2) {
      timerRunning = false;
      timerRemaining = timerSetMs;
    }
    if (evAdj && !timerRunning) {
      if (evAdj == 1) timerPreset = (timerPreset + 1) % N_PRESETS;
      else timerPreset = (timerPreset + N_PRESETS - 1) % N_PRESETS;
      timerSetMs = TIMER_PRESETS_S[timerPreset] * 1000UL;
      timerRemaining = timerSetMs;
    }
  } else {  // M_POMO
    if (evStart == 1) {
      if (pomoRunning) {
        pomoRemaining = pomoRemainingNow(now);
        pomoRunning = false;
      } else {
        pomoEnd = now + pomoRemaining;
        pomoRunning = true;
      }
    } else if (evStart == 2) {
      pomoPhase = P_WORK;
      pomoDone = 0;
      pomoRunning = false;
      pomoRemaining = phaseMs(P_WORK);
    }
    if (evAdj == 1) advancePhase(false, now);  // saltar fase
  }
}

// Los temporizadores siguen corriendo aunque estés en otro modo
void tickTimers(uint32_t now) {
  if (timerRunning && (long)(timerEndAt - now) <= 0) {
    timerRunning = false;
    timerRemaining = timerSetMs;
    startAlarm("TEMPORIZADOR", now);
  }
  if (pomoRunning && (long)(pomoEnd - now) <= 0) {
    advancePhase(true, now);
    startAlarm(pomoPhase == P_WORK ? "A TRABAJAR" : "A DESCANSAR", now);
  }
}

// Valor que, al cambiar, obliga a redibujar el OLED 1
int32_t currentSig(uint32_t now) {
  if (mode == M_TIMER) return (timerRemainingNow(now) + 999) / 1000;
  if (mode == M_POMO) return (pomoRemainingNow(now) + 999) / 1000;
  struct tm t;
  if (getLocalTime(&t, 10)) return t.tm_sec;
  return -1;
}

// ---------- Utilidades de arranque ----------
void showMessage(Adafruit_SSD1306& d, const char* msg) {
  d.clearDisplay();
  d.setTextColor(WHITE);
  d.setTextSize(1);
  d.setCursor(0, 28);
  d.print(msg);
  d.display();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  showMessage(clockOled, "Conectando WiFi...");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
}

void setup() {
  Serial.begin(115200);

  btnMode.begin(BTN_MODE_PIN);
  btnStart.begin(BTN_START_PIN);
  btnAdj.begin(BTN_ADJ_PIN);
  if (BUZZER_PIN >= 0) {
    if (BUZZER_PASSIVE) {
      ledcAttach(BUZZER_PIN, 2000, 8);
      ledcWriteTone(BUZZER_PIN, 0);
    } else {
      pinMode(BUZZER_PIN, OUTPUT);
      digitalWrite(BUZZER_PIN, LOW);
    }
  }

  Wire.begin(SDA_1, SCL_1);
  Wire1.begin(SDA_2, SCL_2);

  if (!clockOled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED reloj no encontrada");
  if (!weatherOled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED tiempo no encontrada");
  clockOled.cp437(true);
  weatherOled.cp437(true);

  connectWiFi();

  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  showMessage(clockOled, "Sincronizando hora...");
  struct tm t;
  getLocalTime(&t, 10000);

  if (fetchWeather()) lastWeather = millis();
  weatherDirty = true;
  needRedraw = true;
}

void loop() {
  // Reconectar si se cae el WiFi
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry > 30000) {
      lastTry = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }

  // Actualizar previsión (si falla, reintenta a los 2 min).
  // Durante la descarga los botones pueden tardar un par de segundos en responder.
  if (millis() - lastWeather > WEATHER_EVERY_MS || (!w.valid && millis() - lastWeather > 120000UL)) {
    lastWeather = millis();
    if (fetchWeather()) weatherDirty = true;
  }

  if (weatherDirty) {
    drawWeather();
    weatherDirty = false;
  }

  uint32_t now = millis();

  uint8_t eM = btnMode.update(now);
  uint8_t eS = btnStart.update(now);
  uint8_t eA = btnAdj.update(now);
  handleButtons(eM, eS, eA, now);

  tickTimers(now);
  tickAlarm(now);

  int32_t sig = currentSig(now);
  if (needRedraw || sig != lastSig) {
    lastSig = sig;
    needRedraw = false;
    drawClockScreen(now);
  }

  delay(10);
}
