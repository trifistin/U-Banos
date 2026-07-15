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
#include <Preferences.h>

// ---------------- CONFIGURAR AQUÍ ----------------
const char* WIFI_SSID = "S24";
const char* WIFI_PASS = "123456789";
const char* API_URL   = "https://trifistin.pythonanywhere.com/api/lecturas";
const char* DISPOSITIVO = "esp32-01";

// Persiste solo el cruce confirmado del pico. No se escribe en cada lectura,
// por lo que no se desgasta la flash por la medición periódica.
Preferences memoriaConfort;
bool memoriaConfortLista = false;

// ---------------- Pines ----------------
const int trigPin = 2;
const int echoPin = 15;
const int irPin   = 34;

// ---------------- Calibración: JABÓN (ultrasónico HC-SR04) ----------------
// Tabla empírica medida con el recipiente real (14-07-2026). Como el envase
// no tiene sección uniforme, se interpola por tramos entre TODOS los puntos
// medidos en vez de asumir una cantidad fija de ml por centímetro.
//
// Las distancias de la planilla se registraron en la misma referencia que
// entrega leerUltrasonico(), es decir, DESPUÉS de aplicar US_OFFSET. Por eso
// las anclas se copian directamente y NO se les vuelve a sumar el offset.
// Sumarlo también a la tabla desplazaba la curva 1,4 cm y sobreestimaba el
// contenido (120-125 ml -> ~23%; 250 ml -> ~48%).
//
// No se midió el recipiente vacío. El punto de 0 ml se estima prolongando el
// último tramo medido (120-180 ml): 10,24 + 2*(10,24-8,51) = 13,70 cm.
// El % se define por VOLUMEN: pct = ml / 720 * 100.
const float US_OFFSET = 1.4;   // se aplica una sola vez en leerUltrasonico()

// v10: el recipiente es de sección UNIFORME: 720 ml en 10,0 cm de columna
// (72 ml/cm), sensor ~2 cm sobre el borde. La tabla empírica anterior
// implicaba densidades imposibles para un envase uniforme (31 ml/cm en un
// tramo y 182 ml/cm en otro) y la validación volumétrica del 14-07 mostró
// errores de hasta 21 puntos. Se reemplaza por el modelo lineal geométrico.
//
// Las anclas están en la MISMA referencia que entrega leerUltrasonico()
// (es decir, ya con US_OFFSET aplicado). CALIBRACIÓN EN 1 PASO:
//   recipiente VACÍO -> leer "dist filtrada" en el serial -> escribir ese
//   valor en D_VACIO_J_CM. El lleno se deriva restando la profundidad.
const float PROFUNDIDAD_J_CM = 10.0;   // columna útil de jabón
const float D_VACIO_J_CM     = 11.0;   // dist al fondo (AJUSTAR con envase vacío)
const float D_LLENO_J_CM     = D_VACIO_J_CM - PROFUNDIDAD_J_CM;  // = 2.0
const float VOLUMEN_UTIL_ML  = 720.0;
const float ML_POR_CM        = VOLUMEN_UTIL_ML / PROFUNDIDAD_J_CM; // 72

// Ventana de plausibilidad del eco: fuera de esto la lectura se descarta
// (eco fantasma, rebote lateral, envase ausente).
const float US_MIN_PLAUSIBLE = 1.0;                    // cerca del lleno
const float US_MAX_PLAUSIBLE = D_VACIO_J_CM + 1.5;     // fondo + margen
// Filtro robusto del jabón:
//  1. Mediana móvil de 5 muestras: un pico aislado no cambia la salida.
//  2. Si la mediana salta más de 10 puntos porcentuales, el nuevo nivel debe
//     repetirse 3 veces dentro de una banda de +/-5 puntos antes de aceptarse.
// Con mediciones cada 1 s, un cambio grande real tarda ~5 s en confirmarse.
const int   N_FILTRO_JABON              = 5;
const float SALTO_JABON_PCT             = 10.0;
const int   CONFIRMACIONES_JABON        = 3;
const float TOLERANCIA_CANDIDATO_J_PCT  = 5.0;

struct MuestraJabon {
  float dist;
  float ml;
  float pct;
};

// ---------------- Calibración: CONFORT (infrarrojo Sharp) ----------------
// El GP2Y0A21 está especificado para 10 a 80 cm; este montaje trabaja cerca
// de 0,5 a 7 cm. En este rango la curva no es monótona y la reflectividad del
// papel altera la salida, por lo que NO se publica una falsa distancia ni un
// porcentaje continuo.
//
// Los límites son puntos medios conservadores de las validaciones con el rollo
// real: 2250 (lleno), 2475 (2,5 cm), 3236 (3,5 cm), 3875 (4,5 cm) y el pico
// 4030-4040. Cada estado debe confirmarse tres veces; una muestra aislada no
// modifica la salida. El porcentaje enviado es solo un valor representativo
// para conservar la compatibilidad con el panel existente.
enum NivelConfort {
  CONFORT_LLENO,
  CONFORT_CASI_LLENO,
  CONFORT_MEDIO,
  CONFORT_BAJO,
  CONFORT_CRITICO
};

// ---- GEOMETRÍA DEL ROLLO: medir UNA VEZ con regla y ajustar ----
const float D_LLENO_CM  = 1.5;   // sensor -> superficie, rollo NUEVO
const float D_VACIO_CM  = 4.5;   // sensor -> cartón del núcleo vacío
const float R_NUCLEO_CM = 2.25;  // radio EXTERIOR del tubo de cartón

// ---- Cortes de nivel, definidos sobre % de PAPEL REAL ----
// pct = (R^2 - r_nucleo^2) / (R_lleno^2 - r_nucleo^2): el papel es
// proporcional al AREA del anillo, no a la distancia. Con cortes lineales
// en distancia, un rollo al 40% se clasificaba CASI_LLENO (validado
// 14-07: 3,0 cm / ~2800 ADC / rollo real 40-45%).
const float PCT_CORTE_LLENO  = 80.0;  // LLENO:      >= 80 %
const float PCT_CORTE_CASI   = 55.0;  // CASI_LLENO: 55-80 %
const float PCT_CORTE_MEDIO  = 30.0;  // MEDIO:      30-55 %
const float PCT_CORTE_BAJO   = 15.0;  // BAJO: 15-30 % | CRITICO: < 15 %

// ---- Tabla ADC <-> distancia CON el condensador de 100 uF ----
// PROVISIONAL: anclas previas desplazadas -56 ADC según el único punto
// medido post-condensador (3,0 cm -> 2800). RE-MEDIR y reemplazar.
const int N_CAL_C = 5;
const float CAL_C_DIST[N_CAL_C] = { 1.5,  2.5,  3.0,  3.5,  4.5 };
const float CAL_C_ADC[N_CAL_C]  = { 2194, 2419, 2800, 3180, 3819 };

// Umbrales ADC: se CALCULAN en setup(). No editar a mano.
float ADC_LLENO_MAX      = 0;
float ADC_CASI_LLENO_MAX = 0;
float ADC_MEDIO_MAX      = 0;
float ADC_BAJO_MAX       = 0;

const float ADC_PICO_LATCH   = 3950.0;  // verificar tras re-medir anclas
const float ADC_MIN_VALIDO   = 1200.0;
const int   CONFIRMACIONES_NIVEL = 3;

// ---------------- Umbrales ----------------
const float UMBRAL_BAJO         = 50.0;
const float UMBRAL_CRITICO      = 20.0;

// ---------------- Modo demo ----------------
// Para la exposición: envío cada 2 s. Para operación normal: comentar la línea.
#define MODO_DEMO

const unsigned long INTERVALO_MEDICION_MS = 1000;
#ifdef MODO_DEMO
const unsigned long INTERVALO_ENVIO_MS    = 2000;   // demo: latencia mínima
#else
const unsigned long INTERVALO_ENVIO_MS    = 10000;  // normal
#endif
unsigned long ultimaMedicion = 0;
unsigned long ultimoEnvio    = 0;
// Distancia a la que el rollo tiene el pct de papel indicado.
// R(d) = R_NUCLEO + (D_VACIO - d); se invierte el área del anillo.
float distanciaDesdePctPapel(float pct) {
  float rLleno   = R_NUCLEO_CM + (D_VACIO_CM - D_LLENO_CM);
  float areaUtil = rLleno * rLleno - R_NUCLEO_CM * R_NUCLEO_CM;
  float radio = sqrtf((pct / 100.0f) * areaUtil + R_NUCLEO_CM * R_NUCLEO_CM);
  return R_NUCLEO_CM + D_VACIO_CM - radio;
}

// ADC esperado a una distancia, interpolando la tabla empírica.
float adcDesdeDistanciaConfort(float dist) {
  if (dist <= CAL_C_DIST[0])           return CAL_C_ADC[0];
  if (dist >= CAL_C_DIST[N_CAL_C - 1]) return CAL_C_ADC[N_CAL_C - 1];
  for (int i = 0; i < N_CAL_C - 1; i++) {
    if (dist <= CAL_C_DIST[i + 1]) {
      float t = (dist - CAL_C_DIST[i]) / (CAL_C_DIST[i + 1] - CAL_C_DIST[i]);
      return CAL_C_ADC[i] + t * (CAL_C_ADC[i + 1] - CAL_C_ADC[i]);
    }
  }
  return CAL_C_ADC[N_CAL_C - 1];
}

void calcularUmbralesConfort() {
  ADC_LLENO_MAX      = adcDesdeDistanciaConfort(distanciaDesdePctPapel(PCT_CORTE_LLENO));
  ADC_CASI_LLENO_MAX = adcDesdeDistanciaConfort(distanciaDesdePctPapel(PCT_CORTE_CASI));
  ADC_MEDIO_MAX      = adcDesdeDistanciaConfort(distanciaDesdePctPapel(PCT_CORTE_MEDIO));
  ADC_BAJO_MAX       = adcDesdeDistanciaConfort(distanciaDesdePctPapel(PCT_CORTE_BAJO));

  Serial.println("[CONFORT] Umbrales ADC (geometria de anillo):");
  Serial.print("  LLENO      hasta "); Serial.println(ADC_LLENO_MAX, 0);
  Serial.print("  CASI_LLENO hasta "); Serial.println(ADC_CASI_LLENO_MAX, 0);
  Serial.print("  MEDIO      hasta "); Serial.println(ADC_MEDIO_MAX, 0);
  Serial.print("  BAJO       hasta "); Serial.println(ADC_BAJO_MAX, 0);
  Serial.print("  CRITICO    sobre eso, o pico latch >= ");
  Serial.println(ADC_PICO_LATCH, 0);
}
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

// Mediana de muestras separadas en el tiempo. El Sharp entrega una medida
// nueva aproximadamente cada 38 ms; promediar muestras tomadas en 7,5 ms no
// reduce picos. La mediana elimina un valor aislado sin deformar el nivel.
float leerInfrarrojoADC() {
  const int N = 7;
  int muestras[N];
  for (int i = 0; i < N; i++) {
    muestras[i] = analogRead(irPin);
    if (i < N - 1) delay(40);
  }

  for (int i = 1; i < N; i++) {
    int valor = muestras[i];
    int j = i - 1;
    while (j >= 0 && muestras[j] > valor) {
      muestras[j + 1] = muestras[j];
      j--;
    }
    muestras[j + 1] = valor;
  }

  float adc = muestras[N / 2];
  if (adc < ADC_MIN_VALIDO) return -1.0;
  return adc;
}

NivelConfort nivelDesdeAdc(float adc, bool ramaLejana) {
  if (ramaLejana) return CONFORT_CRITICO;
  if (adc <= ADC_LLENO_MAX)      return CONFORT_LLENO;
  if (adc <= ADC_CASI_LLENO_MAX) return CONFORT_CASI_LLENO;
  if (adc <= ADC_MEDIO_MAX)      return CONFORT_MEDIO;
  if (adc <= ADC_BAJO_MAX)       return CONFORT_BAJO;

  // Menos de 15% de papel, aun en rama ascendente: CRITICO legitimo
  // aunque el pico no se haya cruzado. La alerta llega ANTES de agotarse.
  return CONFORT_CRITICO;

}

const char* etiquetaNivelConfort(NivelConfort nivel) {
  switch (nivel) {
    case CONFORT_LLENO:      return "LLENO";
    case CONFORT_CASI_LLENO: return "CASI_LLENO";
    case CONFORT_MEDIO:      return "MEDIO";
    case CONFORT_BAJO:       return "BAJO";
    default:                 return "CRITICO";
  }
}

// Compatibilidad temporal con el panel actual, que aún muestra porcentaje.
// Estos números son etiquetas visuales; no son una estimación de papel real.
float porcentajeRepresentativoConfort(NivelConfort nivel) {
  switch (nivel) {
    case CONFORT_LLENO:      return 100.0;
    case CONFORT_CASI_LLENO: return 75.0;
    case CONFORT_MEDIO:      return 50.0;
    case CONFORT_BAJO:       return 30.0;
    default:                 return 15.0;
  }
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

// Conserva la muestra central de las últimas mediciones y retiene los saltos
// grandes hasta que varias medianas consecutivas confirmen el nuevo nivel.
// Se filtran juntos distancia y ml para que el JSON enviado sea coherente.
void filtrarJabon(float distNueva, float mlNueva,
                  float &distSalida, float &mlSalida) {
  static MuestraJabon ventana[N_FILTRO_JABON];
  static int posicion = 0;
  static int cantidad = 0;

  static bool tieneEstable = false;
  static MuestraJabon estable;
  static MuestraJabon candidata;
  static int contCandidata = 0;

  MuestraJabon nueva;
  nueva.dist = distNueva;
  nueva.ml   = mlNueva;
  nueva.pct  = limitar(100.0f * mlNueva / VOLUMEN_UTIL_ML, 0, 100);

  ventana[posicion] = nueva;
  posicion = (posicion + 1) % N_FILTRO_JABON;
  if (cantidad < N_FILTRO_JABON) cantidad++;

  // Copia y ordena por porcentaje. Como son solo 5 elementos, una inserción
  // simple evita memoria dinámica y es suficiente para el ESP32.
  MuestraJabon ordenadas[N_FILTRO_JABON];
  for (int i = 0; i < cantidad; i++) ordenadas[i] = ventana[i];
  for (int i = 1; i < cantidad; i++) {
    MuestraJabon clave = ordenadas[i];
    int j = i - 1;
    while (j >= 0 && ordenadas[j].pct > clave.pct) {
      ordenadas[j + 1] = ordenadas[j];
      j--;
    }
    ordenadas[j + 1] = clave;
  }
  MuestraJabon mediana = ordenadas[cantidad / 2];

  if (!tieneEstable) {
    estable = mediana;
    tieneEstable = true;
  } else {
    float diferencia = fabsf(mediana.pct - estable.pct);

    if (diferencia <= SALTO_JABON_PCT) {
      // Variación normal: la mediana de la ventana ya la corroboró.
      estable = mediana;
      contCandidata = 0;
    } else {
      // Cambio grande: debe repetirse alrededor del mismo nuevo valor.
      if (contCandidata == 0 ||
          fabsf(mediana.pct - candidata.pct) > TOLERANCIA_CANDIDATO_J_PCT) {
        candidata = mediana;
        contCandidata = 1;
      } else {
        candidata = mediana;
        contCandidata++;
      }

      if (contCandidata >= CONFIRMACIONES_JABON) {
        estable = candidata;
        contCandidata = 0;
        Serial.println("[JABON]   cambio sostenido confirmado");
      } else {
        Serial.print("[JABON]   salto en verificacion: ");
        Serial.print(estable.pct, 0); Serial.print("% -> ");
        Serial.print(mediana.pct, 0); Serial.print("% (");
        Serial.print(contCandidata); Serial.print("/");
        Serial.print(CONFIRMACIONES_JABON);
        Serial.println(") -> manteniendo valor estable");
      }
    }
  }

  distSalida = estable.dist;
  mlSalida   = estable.ml;
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
  memoriaConfortLista = memoriaConfort.begin("u-banos", false);
  if (!memoriaConfortLista) {
    Serial.println("[CONFORT] no se pudo abrir memoria persistente");
  }
  delay(100);
  Serial.println("U-Banos: sensores + Wi-Fi + API");
  calcularUmbralesConfort();
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

  // ---------- JABÓN (ultrasónico + tabla empírica) ----------
  float distJabon = leerUltrasonico();
  if (distJabon < 0) {
    Serial.println("[JABON]   sin eco");
  } else if (distJabon < US_MIN_PLAUSIBLE || distJabon > US_MAX_PLAUSIBLE) {
    Serial.print("[JABON]   eco implausible ("); Serial.print(distJabon, 1);
    Serial.println(" cm) -> descartado");
  } else {
    // Modelo lineal: envase uniforme, 72 ml por cm de columna.
    float ml = (D_VACIO_J_CM - distJabon) * ML_POR_CM;
    ml = limitar(ml, 0, VOLUMEN_UTIL_ML);
    
    float distJabonFiltrada;
    float mlFiltrado;
    filtrarJabon(distJabon, ml, distJabonFiltrada, mlFiltrado);

    float pct = limitar(100.0f * mlFiltrado / VOLUMEN_UTIL_ML, 0, 100);
    const char* est = estadoDesde(pct);

    Serial.print("[JABON]   dist filtrada: "); Serial.print(distJabonFiltrada, 1);
    Serial.print(" cm | "); Serial.print(mlFiltrado, 0); Serial.print(" ml (");
    Serial.print(pct, 0); Serial.print("%) | "); Serial.println(est);

    if (tocaEnviar) enviarLectura("jabon", distJabonFiltrada, mlFiltrado, pct, est);
  }

  // ---------- CONFORT (Sharp: niveles discretos, mediana y confirmación) ----------
  static bool tieneNivel = false;
  static NivelConfort nivelEstable = CONFORT_MEDIO;
  static NivelConfort nivelCandidato = CONFORT_MEDIO;
  static int contCandidato = 0;
  static bool ramaLejana = false;  // tras el pico, el ADC vuelve a ser ambiguo
  static int contPico = 0;
  static int contRecarga = 0;
  static bool estadoInicializado = false;

  if (!estadoInicializado) {
    if (memoriaConfortLista) {
      ramaLejana = memoriaConfort.getBool("rama-lejana", false);
      if (ramaLejana) {
        nivelEstable = CONFORT_CRITICO;
        tieneNivel = true;
        Serial.println("[CONFORT] rama critica restaurada desde memoria");
      }
    }
    estadoInicializado = true;
  }

  float adcConfort = leerInfrarrojoADC();

  if (adcConfort < 0) {
    // Sensor sin señal útil: jamás inventar un estado nuevo.
    if (tieneNivel) {
      const char* estado = etiquetaNivelConfort(nivelEstable);
      float pct = porcentajeRepresentativoConfort(nivelEstable);
      Serial.print("[CONFORT] ADC bajo minimo -> manteniendo ");
      Serial.println(estado);
      if (tocaEnviar) enviarLectura("confort", -1.0, 0.0, pct, estado);
    } else {
      Serial.println("[CONFORT] ADC bajo minimo y sin historial -> sin dato");
    }
  } else {
    // El pico debe persistir: un salto aislado hacia 4095 no debe disparar
    // CRITICO. Una vez cruzado de verdad, la rama descendente no permite
    // distinguir por ADC entre poco papel y un nivel medio.
    if (!ramaLejana) {
      if (adcConfort >= ADC_PICO_LATCH) {
        contPico++;
        if (contPico >= CONFIRMACIONES_NIVEL) {
          ramaLejana = true;
          contPico = 0;
          contRecarga = 0;
          nivelEstable = CONFORT_CRITICO;
          tieneNivel = true;
          contCandidato = 0;
          if (memoriaConfortLista) memoriaConfort.putBool("rama-lejana", true);
          Serial.println("[CONFORT] pico confirmado -> rama lejana / CRITICO");
        }
      } else {
        contPico = 0;
      }
    } else if (adcConfort <= ADC_LLENO_MAX) {
      // Solo tres medianas consecutivas en la zona llena desenganchan la
      // rama: equivale a corroborar una recarga del rollo.
      contRecarga++;
      if (contRecarga >= CONFIRMACIONES_NIVEL) {
        ramaLejana = false;
        contRecarga = 0;
        contPico = 0;
        nivelEstable = CONFORT_LLENO;
        tieneNivel = true;
        contCandidato = 0;
        if (memoriaConfortLista) memoriaConfort.putBool("rama-lejana", false);
        Serial.println("[CONFORT] recarga confirmada -> rama cercana");
      }
    } else {
      contRecarga = 0;
    }

    NivelConfort nivelNuevo = nivelDesdeAdc(adcConfort, ramaLejana);
    if (!tieneNivel) {
      nivelEstable = nivelNuevo;
      tieneNivel = true;
    } else if (nivelNuevo == nivelEstable) {
      contCandidato = 0;
    } else if (nivelNuevo == nivelCandidato) {
      contCandidato++;
    } else {
      nivelCandidato = nivelNuevo;
      contCandidato = 1;
    }

    if (tieneNivel && nivelNuevo != nivelEstable &&
        contCandidato >= CONFIRMACIONES_NIVEL) {
      nivelEstable = nivelCandidato;
      contCandidato = 0;
      Serial.print("[CONFORT] nivel confirmado -> ");
      Serial.println(etiquetaNivelConfort(nivelEstable));
    }

    const char* estado = etiquetaNivelConfort(nivelEstable);
    float pct = porcentajeRepresentativoConfort(nivelEstable);
    Serial.print("[CONFORT] ADC mediano: "); Serial.print(adcConfort, 0);
    Serial.print(" | nivel: "); Serial.print(estado);
    Serial.print(" | valor visual: "); Serial.print(pct, 0);
    Serial.println("%");

    // No se envía distancia ni espesor: serían valores inventados fuera del
    // rango válido del sensor. El campo porcentaje es una etiqueta visual.
    if (tocaEnviar) enviarLectura("confort", -1.0, 0.0, pct, estado);
  }

  if (tocaEnviar) ultimoEnvio = ahora;
}
