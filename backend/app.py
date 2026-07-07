# ============================================================
# U-Baños — Backend (Flask + SQLite)  [Controlador + Modelo del MVC]
#
# Endpoints:
#   POST /api/lecturas   <- el ESP32 envía aquí cada lectura (JSON)
#   GET  /api/estado     -> última lectura de cada sensor
#   GET  /api/historial?sensor=jabon&n=50 -> últimas n lecturas
#   GET  /               -> página simple de estado (auto-refresca)
#
# Local:            pip install -r requirements.txt && python app.py
# PythonAnywhere:   ver README (sección Despliegue)
# ============================================================

import os
import sqlite3
from datetime import datetime
from flask import Flask, request, jsonify, g

# En PythonAnywhere el working dir puede variar: usar ruta absoluta
DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ubanos.db")
app = Flask(__name__)

# ---------------- Base de datos (Modelo) ----------------

def get_db():
    if "db" not in g:
        g.db = sqlite3.connect(DB_PATH)
        g.db.row_factory = sqlite3.Row
    return g.db

@app.teardown_appcontext
def close_db(_exc):
    db = g.pop("db", None)
    if db is not None:
        db.close()

def init_db():
    con = sqlite3.connect(DB_PATH)
    con.execute("""
        CREATE TABLE IF NOT EXISTS lecturas (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts TEXT NOT NULL,
            dispositivo TEXT NOT NULL,
            sensor TEXT NOT NULL,          -- 'jabon' o 'confort'
            distancia_cm REAL,
            cantidad REAL,                 -- ml (jabon) o cm de radio (confort)
            porcentaje REAL,
            estado TEXT                    -- DISPONIBLE / BAJO / CRITICO
        )
    """)
    con.commit()
    con.close()

init_db()  # se ejecuta también al importar desde WSGI (PythonAnywhere)

# CORS: permite que el frontend (GitHub Pages) consulte esta API
@app.after_request
def agregar_cors(resp):
    resp.headers["Access-Control-Allow-Origin"] = "*"
    resp.headers["Access-Control-Allow-Headers"] = "Content-Type"
    resp.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
    return resp

# ---------------- Endpoints (Controlador) ----------------

@app.route("/api/lecturas", methods=["POST", "OPTIONS"])
def recibir_lectura():
    if request.method == "OPTIONS":
        return "", 204
    data = request.get_json(silent=True)
    if not data:
        return jsonify({"ok": False, "error": "JSON invalido"}), 400

    requeridos = ["sensor", "estado"]
    if any(k not in data for k in requeridos):
        return jsonify({"ok": False, "error": f"faltan campos {requeridos}"}), 400

    db = get_db()
    db.execute(
        "INSERT INTO lecturas (ts, dispositivo, sensor, distancia_cm, cantidad, porcentaje, estado) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        (
            datetime.now().isoformat(timespec="seconds"),
            data.get("dispositivo", "esp32-01"),
            data["sensor"],
            data.get("distancia_cm"),
            data.get("cantidad"),
            data.get("porcentaje"),
            data["estado"],
        ),
    )
    db.commit()
    return jsonify({"ok": True}), 201

@app.route("/api/estado")
def estado():
    db = get_db()
    filas = db.execute("""
        SELECT l.* FROM lecturas l
        JOIN (SELECT sensor, MAX(id) AS max_id FROM lecturas GROUP BY sensor) u
          ON l.id = u.max_id
    """).fetchall()
    return jsonify({f["sensor"]: dict(f) for f in filas})

@app.route("/api/historial")
def historial():
    sensor = request.args.get("sensor")
    n = int(request.args.get("n", 50))
    db = get_db()
    if sensor:
        filas = db.execute(
            "SELECT * FROM lecturas WHERE sensor = ? ORDER BY id DESC LIMIT ?",
            (sensor, n)).fetchall()
    else:
        filas = db.execute(
            "SELECT * FROM lecturas ORDER BY id DESC LIMIT ?", (n,)).fetchall()
    return jsonify([dict(f) for f in filas])

@app.route("/")
def index():
    db = get_db()
    filas = db.execute("""
        SELECT l.* FROM lecturas l
        JOIN (SELECT sensor, MAX(id) AS max_id FROM lecturas GROUP BY sensor) u
          ON l.id = u.max_id
    """).fetchall()
    colores = {"DISPONIBLE": "#2e7d32", "BAJO": "#f9a825", "CRITICO": "#c62828"}
    tarjetas = ""
    for f in filas:
        c = colores.get(f["estado"], "#555")
        nombre = "Jabón" if f["sensor"] == "jabon" else "Confort"
        unidad = "ml" if f["sensor"] == "jabon" else "cm de radio"
        tarjetas += f"""
        <div style="border:2px solid {c};border-radius:12px;padding:16px;margin:8px;
                    min-width:220px;display:inline-block;font-family:sans-serif">
          <h2 style="margin:0">{nombre}</h2>
          <p style="font-size:2em;margin:8px 0;color:{c}"><b>{f['estado']}</b></p>
          <p style="margin:0">{f['cantidad']} {unidad} ({f['porcentaje']}%)</p>
          <small>Última lectura: {f['ts']}</small>
        </div>"""
    if not tarjetas:
        tarjetas = "<p style='font-family:sans-serif'>Aún no llegan lecturas del ESP32.</p>"
    return f"""<html><head><meta http-equiv="refresh" content="5">
        <title>U-Baños — Piloto</title></head>
        <body style="background:#fafafa"><h1 style="font-family:sans-serif">U-Baños — Estado en vivo</h1>
        {tarjetas}</body></html>"""

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
