// ============================================================
// U-Baños — ESP32 38P — MODO VALIDACIÓN
// Igual que el sketch principal, pero con captura automática
// de 30 lecturas por nivel para el plan de validación.
//
// USO (Monitor Serie a 115200, "Nueva línea" activado):
//   v 20      -> valida AMBOS sensores contra distancia real 20 cm
//   vi 30     -> valida solo el INFRARROJO contra 30 cm
//   vu 15     -> valida solo el ULTRASÓNICO contra 15 cm
//   (cualquier otra cosa) -> sigue midiendo normal cada 1 s
//
// Por cada validación imprime: promedio, error medio, desviación
// estándar y las 30 lecturas, listo para copiar a la bitácora.
// ============================================================

const int trigPin = 2;
const int echoPin = 15;
const int irPin   = 34;

const int N_VALIDACION = 30; // lecturas por nivel (plan Bitácora 7)

// --- Calibración (igual que el sketch principal, ajustar al montar) ---
const float ALTURA_SENSOR_FONDO = 20.0;
const float DIAMETRO_INTERNO    = 10.0;
const float DIST_SENSOR_EJE     = 20.0;
const float RADIO_LLENO         = 6.0;
const float RADIO_TUBO          = 2.2;
const float UMBRAL_BAJO         = 50.0;
const float UMBRAL_CRITICO      = 20.0;

const unsigned long INTERVALO_MS = 1000;
unsigned long ultimaMedicion = 0;

// ============================================================
float leerUltrasonico() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  unsigned long duration = pulseIn(echoPin, HIGH, 30000UL);
  if (duration == 0) return -1.0;
  return (duration * 0.0343f) / 2.0f;
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
// Validación: captura N lecturas y reporta estadísticas
// sensor: 0 = infrarrojo, 1 = ultrasónico
// ============================================================
void validarSensor(int sensor, float distReal) {
  const char* nombre = (sensor == 0) ? "INFRARROJO" : "ULTRASONICO";
  float lecturas[N_VALIDACION];
  int validas = 0;

  Serial.println();
  Serial.print("=== VALIDACION ");
  Serial.print(nombre);
  Serial.print(" | distancia real: ");
  Serial.print(distReal, 1);
  Serial.println(" cm ===");
  Serial.println("Manten el objeto quieto...");

  for (int i = 0; i < N_VALIDACION; i++) {
    float d = (sensor == 0) ? leerInfrarrojo() : leerUltrasonico();
    if (d >= 0) {
      lecturas[validas++] = d;
    }
    Serial.print(d, 2);
    Serial.print((i + 1) % 10 == 0 ? "\n" : "  ");
    delay(200); // ~6 segundos en total
  }
  Serial.println();

  if (validas < 5) {
    Serial.println("!! Muy pocas lecturas validas, revisar sensor/montaje.");
    return;
  }

  // Estadísticas
  float suma = 0;
  for (int i = 0; i < validas; i++) suma += lecturas[i];
  float prom = suma / validas;

  float sumaSq = 0;
  for (int i = 0; i < validas; i++) {
    float dif = lecturas[i] - prom;
    sumaSq += dif * dif;
  }
  float desv = sqrtf(sumaSq / (validas - 1));

  float errorCm  = prom - distReal;
  float errorPct = 100.0f * errorCm / distReal;

  Serial.println("---- RESULTADOS (copiar a bitacora) ----");
  Serial.print("Sensor: ");            Serial.println(nombre);
  Serial.print("Distancia real: ");    Serial.print(distReal, 1);  Serial.println(" cm");
  Serial.print("Lecturas validas: ");  Serial.print(validas);      Serial.print(" / "); Serial.println(N_VALIDACION);
  Serial.print("Promedio medido: ");   Serial.print(prom, 2);      Serial.println(" cm");
  Serial.print("Error medio: ");       Serial.print(errorCm, 2);   Serial.print(" cm (");
  Serial.print(errorPct, 1);           Serial.println(" %)");
  Serial.print("Desv. estandar: ");    Serial.print(desv, 2);      Serial.println(" cm");
  Serial.println("-----------------------------------------");
  Serial.println();
}

// ============================================================
// Interpreta comandos del Monitor Serie
// ============================================================
void revisarComandos() {
  if (!Serial.available()) return;
  String linea = Serial.readStringUntil('\n');
  linea.trim();
  linea.toLowerCase();
  if (linea.length() == 0) return;

  if (linea.startsWith("vi ")) {
    validarSensor(0, linea.substring(3).toFloat());
  } else if (linea.startsWith("vu ")) {
    validarSensor(1, linea.substring(3).toFloat());
  } else if (linea.startsWith("v ")) {
    float d = linea.substring(2).toFloat();
    validarSensor(0, d);
    validarSensor(1, d);
  } else {
    Serial.println("Comandos: 'v <cm>' ambos | 'vi <cm>' IR | 'vu <cm>' ultrasonico");
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
  Serial.println("U-Banos MODO VALIDACION");
  Serial.println("Comandos: 'v 20' | 'vi 30' | 'vu 15' (distancia real en cm)");
  Serial.println("---------------------------------------------------");
}

void loop() {
  revisarComandos();

  unsigned long ahora = millis();
  if (ahora - ultimaMedicion < INTERVALO_MS) return;
  ultimaMedicion = ahora;

  // Medición normal continua (igual que el sketch principal)
  float distJabon = leerUltrasonico();
  if (distJabon < 0) {
    Serial.println("[JABON]   sin eco");
  } else {
    float nivel = limitar(ALTURA_SENSOR_FONDO - distJabon, 0, ALTURA_SENSOR_FONDO);
    float r = DIAMETRO_INTERNO / 2.0f;
    float ml = nivel * PI * r * r;
    float pct = limitar(100.0f * nivel / ALTURA_SENSOR_FONDO, 0, 100);
    Serial.print("[JABON]   dist: "); Serial.print(distJabon, 1);
    Serial.print(" cm | "); Serial.print(ml, 0); Serial.print(" ml (");
    Serial.print(pct, 0); Serial.print("%) | "); Serial.println(estadoDesde(pct));
  }

  float distConfort = leerInfrarrojo();
  if (distConfort < 0) {
    Serial.println("[CONFORT] fuera de rango");
  } else {
    float radio = limitar(DIST_SENSOR_EJE - distConfort, RADIO_TUBO, RADIO_LLENO);
    float pct = limitar(100.0f * (radio - RADIO_TUBO) / (RADIO_LLENO - RADIO_TUBO), 0, 100);
    Serial.print("[CONFORT] dist: "); Serial.print(distConfort, 1);
    Serial.print(" cm | radio: "); Serial.print(radio, 2); Serial.print(" cm (");
    Serial.print(pct, 0); Serial.print("%) | "); Serial.println(estadoDesde(pct));
  }
}
