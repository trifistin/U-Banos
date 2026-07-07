# 🚻 U-Baños — Monitoreo IoT de insumos en baños de la FCFM

**EL3105 — Seminario de Ingeniería Eléctrica e Innovación Tecnológica · Universidad de Chile**
**Grupo 1:** Pedro Riquelme · Diego Vicencio · Dante Ruiz

Ecosistema de monitoreo en tiempo real del estado de los baños del campus:
sensores de bajo costo miden el nivel de jabón y confort (papel) y lo reportan
a una API central, eliminando la incertidumbre de los usuarios y permitiendo
una gestión de mantenimiento basada en datos.

## Arquitectura (MVC)

```
 [ESP32 + sensores]  --HTTP/JSON-->  [API Flask]  <-->  [SQLite]
   (adquisición)                    (Controlador)      (Modelo)
                                          |
                                          v
                              [Dashboard web / GitHub Pages]
                                        (Vista)
```

| Capa MVC    | Implementación                                  | Carpeta      |
|-------------|--------------------------------------------------|--------------|
| Modelo      | SQLite, tabla `lecturas`                         | `backend/`   |
| Vista       | Dashboard HTML/JS (GitHub Pages) + página `/`    | `frontend/`  |
| Controlador | Endpoints Flask (`/api/lecturas`, `/api/estado`) | `backend/`   |
| Adquisición | ESP32 + HC-SR04 + Sharp GP2Y0A21YK               | `firmware/`  |

## Hardware

| Componente            | Función                          | Pin ESP32 (38P) |
|-----------------------|----------------------------------|-----------------|
| HC-SR04 (ultrasónico) | Nivel de jabón (ml)              | TRIG G2, ECHO G15* |
| Sharp GP2Y0A21YK (IR) | Radio restante del rollo (cm)    | Vo → G34 (ADC1) |

\* El ECHO del HC-SR04 a 5 V requiere divisor resistivo (2× 1 kΩ) hacia el ESP32.
El Sharp se alimenta de 5 V (V5) pero su salida (≤3.1 V) va directa a G34.
Calibración: offset ultrasónico +1.4 cm (validado con 30 lecturas por nivel).

## Estructura del repositorio

```
firmware/
  ubanos_wifi/         Sketch principal: sensores + Wi-Fi + POST a la API
  ubanos_validacion/   Modo validación: 30 lecturas por nivel + estadísticas
backend/
  app.py               API Flask + SQLite (Controlador + Modelo)
  requirements.txt
frontend/
  index.html           Dashboard estático (Vista, para GitHub Pages)
docs/
  validacion/          Resultados del plan de validación
```

## Despliegue

### Backend en PythonAnywhere
1. Crear cuenta gratuita en pythonanywhere.com.
2. **Files** → subir `backend/app.py` (o clonar el repo desde una consola Bash:
   `git clone https://github.com/trifistin/U-Banos.git`).
3. **Web** → *Add a new web app* → Flask → Python 3.10.
4. En *Code*, apuntar el archivo WSGI a `app.py`: editar el WSGI file para que
   importe `from app import app as application` con la ruta correcta
   (ej. `path = '/home/TU_USUARIO/U-Banos/backend'`).
5. *Reload*. La API queda en `https://TU_USUARIO.pythonanywhere.com`.

### Firmware
1. Abrir `firmware/ubanos_wifi/ubanos_wifi.ino` en Arduino IDE.
2. Completar `WIFI_SSID`, `WIFI_PASS` y `API_URL`
   (`https://TU_USUARIO.pythonanywhere.com/api/lecturas`).
3. Cargar a la placa (ESP32 Dev Module) y verificar en el Monitor Serie
   (115200) que aparezca `[API] POST jabon -> HTTP 201`.

### Frontend en GitHub Pages
1. Editar `frontend/index.html`: reemplazar `TU_USUARIO` en la constante `API`.
2. En GitHub: **Settings → Pages → Source: main / carpeta `/frontend`**
   (si Pages no permite subcarpeta, mover `index.html` a la raíz o a `/docs`).
3. El dashboard queda publicado en `https://trifistin.github.io/U-Banos/`.

## API

| Método | Ruta                        | Descripción                        |
|--------|-----------------------------|------------------------------------|
| POST   | `/api/lecturas`             | Recibe lectura JSON del ESP32      |
| GET    | `/api/estado`               | Última lectura por sensor          |
| GET    | `/api/historial?sensor=&n=` | Últimas n lecturas                 |

Ejemplo de payload del ESP32:
```json
{"dispositivo":"esp32-01","sensor":"jabon","distancia_cm":8.5,
 "cantidad":901,"porcentaje":57,"estado":"DISPONIBLE"}
```
