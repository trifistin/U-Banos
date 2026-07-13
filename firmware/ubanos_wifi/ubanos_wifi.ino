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
#define MODO_DEMO  // envío cada 2 s. Comentar esta línea = vuelve a 10 s.

// ---------------- CONFIGURAR AQUÍ ----------------
const char* WIFI_SSID = "S24";
const char* WIFI_PASS = "123456789";
const char* API_URL   = "https://trifistin.pythonanywhere.com/api/lecturas";
const char* DISPOSITIVO = "esp32-01";

// ---------------- Pines ----------------
const int trigPin = 2;
const int echoPin = 15;
const int irPin   = 34;

// ---------------- Calibración: JABÓN (ultrasónico) ----------------
const float US_OFFSET           = 1.4;   // corrección del ultrasónico (cm)
const float ALTURA_SENSOR_FONDO = 20.0;
const float DIAMETRO_INTERNO    = 10.0;

// ---------------- Calibración: CONFORT (infrarrojo Sharp) ----------------
// El GP2Y0A21YK0F solo está especificado para 10–80 cm. Nuestro montaje
// trabaja a 0,5–7,5 cm, así que la ecuación del datasheet NO aplica.
// En su lugar: TABLA DE CALIBRACIÓN EMPÍRICA (medida 13-07-2026 con regla
// y superficie del rollo real). La curva resultó MONÓTONA creciente en
// todo el rango -> es invertible por interpolación lineal.
//
//   DIST_LLENO = 0,5 cm -> rollo nuevo  -> 100 %
//   DIST_VACIO = 7,5 cm -> cono         ->   0 %
const float DIST_LLENO = 0.5;
const float DIST_VACIO = 7.5;
const float ESPESOR_PAPEL = DIST_VACIO - DIST_LLENO;   // 7.0 cm útiles

// Tabla ADC -> distancia (ambos deben quedar en orden CRECIENTE de ADC).
const int   N_CAL = 7;
const float CAL_ADC[N_CAL]  = { 1575, 1840, 1975, 2680, 3298, 3670, 3997 };
const float CAL_DIST[N_CAL] = {  0.5,  1.5,  2.5,  3.5,  4.5,  5.5,  7.0 };

// Zona corrupta: a 7 cm el ADC ya está en 3997, a solo 98 cuentas del techo
// (4095). Más allá el sensor satura / la curva se pliega -> dato inválido.
const float ADC_MAX_VALIDO = 4010.0;   // sobre esto: descartar lectura
const float ADC_MIN_VALIDO = 1200.0;   // bajo esto: sin objeto / desconectado

// Filtro de coherencia física: el papel solo baja de a poco; el único salto
// legítimo hacia arriba es una recarga (rollo nuevo -> vuelve a ~100 %).
// Un salto grande hacia ABAJO en 1 s es físicamente imposible -> se descarta.
const float SALTO_MAX_PCT     = 25.0;  // caída máx. creíble entre mediciones
const int   CONFIRMACIONES    = 3;     // lecturas seguidas para aceptar un salto

// ---------------- Umbrales ----------------
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

// Devuelve el ADC promediado del IR (o -1 si está fuera de la ventana válida).
float leerInfrarrojoADC() {
  const int N = 15;
  long suma = 0;
  for (int i = 0; i < N; i++) {
    suma += analogRead(irPin);
    delayMicroseconds(500);
  }
  float adc = suma / (float)N;
  if (adc < ADC_MIN_VALIDO || adc > ADC_MAX_VALIDO) return -1.0;
  return adc;
}

// Interpolación lineal ADC -> distancia sobre la tabla de calibración.
float adcADistancia(float adc) {
  if (adc <= CAL_ADC[0])         return CAL_DIST[0];          // más lleno que lleno: clamp
  if (adc >= CAL_ADC[N_CAL - 1]) return CAL_DIST[N_CAL - 1];  // borde de tabla: clamp a 7.0
  for (int i = 0; i < N_CAL - 1; i++) {
    if (adc <= CAL_ADC[i + 1]) {
      float t = (adc - CAL_ADC[i]) / (CAL_ADC[i + 1] - CAL_ADC[i]);
      return CAL_DIST[i] + t * (CAL_DIST[i + 1] - CAL_DIST[i]);
    }
  }
  return CAL_DIST[N_CAL - 1];
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

  // ---------- CONFORT (infrarrojo + tabla + filtro de coherencia) ----------
  // Estado persistente del filtro:
  static float pctValida       = -1.0;  // última medición aceptada (-1 = aún ninguna)
  static float pctCandidata    = -1.0;  // valor "sospechoso" en verificación
  static int   contConfirma    = 0;     // lecturas seguidas que confirman el salto

  float adcConfort = leerInfrarrojoADC();

  if (adcConfort < 0) {
    // Zona corrupta (saturación >7 cm) o sensor desconectado.
    // Nos quedamos con la última medición coherente, como pide el diseño.
    if (pctValida >= 0) {
      const char* est = estadoDesde(pctValida);
      Serial.print("[CONFORT] ADC fuera de ventana -> manteniendo ultima valida: ");
      Serial.print(pctValida, 0); Serial.print("% | "); Serial.println(est);
      if (tocaEnviar) enviarLectura("confort", -1.0, 0.0, pctValida, est);
    } else {
      Serial.println("[CONFORT] ADC fuera de ventana y sin historial -> sin dato");
    }
  } else {
    float distConfort = adcADistancia(adcConfort);
    float espesor = limitar(DIST_VACIO - distConfort, 0, ESPESOR_PAPEL);
    float pctNueva = limitar(100.0f * espesor / ESPESOR_PAPEL, 0, 100);

    // ---- Filtro de coherencia física ----
    // El papel no puede caer >SALTO_MAX_PCT en 1 s. Una caída así es un
    // artefacto (reflejo, mano, borde de saturación). Solo se acepta si se
    // repite CONFIRMACIONES veces seguidas (entonces es real).
    // Las subidas se aceptan siempre: recarga de rollo = evento legítimo.
    bool aceptar = true;
    if (pctValida >= 0 && (pctValida - pctNueva) > SALTO_MAX_PCT) {
      if (pctCandidata >= 0 && fabsf(pctNueva - pctCandidata) < 10.0f) {
        contConfirma++;
      } else {
        pctCandidata = pctNueva;
        contConfirma = 1;
      }
      if (contConfirma < CONFIRMACIONES) {
        aceptar = false;
        Serial.print("[CONFORT] salto sospechoso (");
        Serial.print(pctValida, 0); Serial.print("% -> ");
        Serial.print(pctNueva, 0); Serial.print("%), verificando ");
        Serial.print(contConfirma); Serial.print("/");
        Serial.println(CONFIRMACIONES);
      }
    }

    if (aceptar) {
      pctValida    = pctNueva;
      pctCandidata = -1.0;
      contConfirma = 0;

      const char* est = estadoDesde(pctValida);
      Serial.print("[CONFORT] ADC: ");   Serial.print(adcConfort, 0);
      Serial.print(" | dist: ");         Serial.print(distConfort, 2);
      Serial.print(" cm | papel: ");     Serial.print(espesor, 2);
      Serial.print(" cm (");             Serial.print(pctValida, 0);
      Serial.print("%) | ");             Serial.println(est);

      if (tocaEnviar) enviarLectura("confort", distConfort, espesor, pctValida, est);
    } else if (tocaEnviar && pctValida >= 0) {
      // Toca enviar pero la lectura está en verificación: se reporta la válida.
      enviarLectura("confort", -1.0, 0.0, pctValida, estadoDesde(pctValida));
    }
  }

  if (tocaEnviar) ultimoEnvio = ahora;
}
