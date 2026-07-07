// ============================================================
// U-Baños — ESP32 38P — SENSORES + WI-FI + ENVÍO A API
// Soporta API local (http://IP:5000) y PythonAnywhere (https://...)
//
// ANTES DE CARGAR: completar WIFI_SSID, WIFI_PASS y API_URL.
//   Local:          "http://192.168.1.100:5000/api/lecturas"
//   PythonAnywhere: "https://TU_USUARIO.pythonanywhere.com/api/lecturas"
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ---------------- CONFIGURAR AQUÍ ----------------
const char* WIFI_SSID = "S24";
const char* WIFI_PASS = "123456789";
const char* API_URL   = "https://trifistin.pythonanywhere.com/api/lecturas";
const char* DISPOSITIVO = "esp32-01";

// ---------------- Pines ----------------
const int trigPin = 2;
const int echoPin = 15;
const int irPin   = 34;

// ---------------- Calibración ----------------
const float US_OFFSET           = 1.4;  // corrección del ultrasónico (cm)
const float ALTURA_SENSOR_FONDO = 20.0;
const float DIAMETRO_INTERNO    = 10.0;
const float DIST_SENSOR_EJE     = 20.0;
const float RADIO_LLENO         = 6.0;
const float RADIO_TUBO          = 2.2;
const float UMBRAL_BAJO         = 50.0;
const float UMBRAL_CRITICO      = 20.0;

const unsigned long INTERVALO_MEDICION_MS = 1000;
const unsigned long INTERVALO_ENVIO_MS    = 10000;
unsigned long ultimaMedicion = 0;
unsigned long ultimoEnvio    = 0;

// ============================================================
float leerUltrasonico() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  unsigned long duration = pulseIn(echoPin, HIGH, 30000UL);
  if (duration == 0) return -1.0;
  return (duration * 0.0343f) / 2.0f + US_OFFSET;
}

float leerInfrarrojo() {
  const int N = 15;
  long suma = 0;
  for (int i = 0; i < N; i++) {
    suma += analogRead(irPin);
    delayMicroseconds(500);
  }
  float adc = suma / (float)N;
  float volts = adc * (3.3f / 4095.0f);
  if (volts < 0.35f) return -1.0;
  float d = 27.728f * powf(volts, -1.2045f);
  if (d < 10.0f) d = 10.0f;
  if (d > 80.0f) d = 80.0f;
  return d;
}

const char* estadoDesde(float pct) {
  if (pct < UMBRAL_CRITICO) return "CRITICO";
  if (pct < UMBRAL_BAJO)    return "BAJO";
  return "DISPONIBLE";
}

float limitar(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

// ============================================================
// Envío HTTP/HTTPS POST a la API
// ============================================================
bool enviarLectura(const char* sensor, float dist, float cantidad,
                   float pct, const char* estado) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] sin conexion, no se envia");
    return false;
  }

  char json[220];
  snprintf(json, sizeof(json),
    "{\"dispositivo\":\"%s\",\"sensor\":\"%s\",\"distancia_cm\":%.1f,"
    "\"cantidad\":%.1f,\"porcentaje\":%.0f,\"estado\":\"%s\"}",
    DISPOSITIVO, sensor, dist, cantidad, pct, estado);

  HTTPClient http;
  int codigo = -1;
  bool esHttps = (strncmp(API_URL, "https", 5) == 0);

  if (esHttps) {
    WiFiClientSecure clienteSeguro;
    clienteSeguro.setInsecure(); // piloto: no valida certificado
    if (http.begin(clienteSeguro, API_URL)) {
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(6000);
      codigo = http.POST(json);
      http.end();
    }
  } else {
    if (http.begin(API_URL)) {
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(4000);
      codigo = http.POST(json);
      http.end();
    }
  }

  Serial.print("[API] POST ");
  Serial.print(sensor);
  Serial.print(" -> HTTP ");
  Serial.println(codigo);
  return (codigo == 201 || codigo == 200);
}

void conectarWifi() {
  Serial.print("[WIFI] Conectando a ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(500);
    Serial.print(".");
    intentos++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI] Conectado. IP del ESP32: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WIFI] No se pudo conectar (revisar SSID/clave, red 2.4 GHz)");
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  digitalWrite(trigPin, LOW);
  analogReadResolution(12);
  analogSetPinAttenuation(irPin, ADC_11db);
  delay(100);
  Serial.println("U-Banos: sensores + Wi-Fi + API");
  conectarWifi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long ultimoIntento = 0;
    if (millis() - ultimoIntento > 15000) {
      ultimoIntento = millis();
      conectarWifi();
    }
  }

  unsigned long ahora = millis();
  if (ahora - ultimaMedicion < INTERVALO_MEDICION_MS) return;
  ultimaMedicion = ahora;
  bool tocaEnviar = (ahora - ultimoEnvio >= INTERVALO_ENVIO_MS);

  // ---------- JABÓN (ultrasónico) ----------
  float distJabon = leerUltrasonico();
  if (distJabon < 0) {
    Serial.println("[JABON]   sin eco");
  } else {
    float nivel = limitar(ALTURA_SENSOR_FONDO - distJabon, 0, ALTURA_SENSOR_FONDO);
    float r = DIAMETRO_INTERNO / 2.0f;
    float ml = nivel * PI * r * r;
    float pct = limitar(100.0f * nivel / ALTURA_SENSOR_FONDO, 0, 100);
    const char* est = estadoDesde(pct);

    Serial.print("[JABON]   dist: "); Serial.print(distJabon, 1);
    Serial.print(" cm | "); Serial.print(ml, 0); Serial.print(" ml (");
    Serial.print(pct, 0); Serial.print("%) | "); Serial.println(est);

    if (tocaEnviar) enviarLectura("jabon", distJabon, ml, pct, est);
  }

  // ---------- CONFORT (infrarrojo) ----------
  float distConfort = leerInfrarrojo();
  if (distConfort < 0) {
    Serial.println("[CONFORT] fuera de rango");
  } else {
    float radio = limitar(DIST_SENSOR_EJE - distConfort, RADIO_TUBO, RADIO_LLENO);
    float pct = limitar(100.0f * (radio - RADIO_TUBO) / (RADIO_LLENO - RADIO_TUBO), 0, 100);
    const char* est = estadoDesde(pct);

    Serial.print("[CONFORT] dist: "); Serial.print(distConfort, 1);
    Serial.print(" cm | radio: "); Serial.print(radio, 2); Serial.print(" cm (");
    Serial.print(pct, 0); Serial.print("%) | "); Serial.println(est);

    if (tocaEnviar) enviarLectura("confort", distConfort, radio, pct, est);
  }

  if (tocaEnviar) ultimoEnvio = ahora;
}
