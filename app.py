"""
Mock server ESP32 Burner untuk preview UI di browser (tanpa hardware).

Struktur folder:
    v3/
      app.py            <- file ini
      static/           <- index.html, app.js, log.html, log.js, style.css, chart.umd.js
      mock_logs/        <- dibuat otomatis, isi CSV percobaan (meniru /logs di SD)

Jalankan:
    pip install flask
    python app.py
Buka http://localhost:5000

Tes modal SD:   buka http://localhost:5000/mock/sd/missing  lalu klik Nyalakan burner
Kembalikan SD:  http://localhost:5000/mock/sd/ok
"""
import os
import re
import threading
import time
from datetime import datetime

from flask import Flask, jsonify, request, Response, send_from_directory

BASE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(BASE, "static")
LOG_DIR = os.path.join(BASE, "mock_logs")
os.makedirs(LOG_DIR, exist_ok=True)

# ---- Parameter simulasi (ubah sesuka hati) ----
AMBIENT = 30.0     # suhu ruang (C)
HEAT_RATE = 6.0    # naik berapa C per detik saat pemanasan
COOL_RATE = 2.0    # turun berapa C per detik saat dingin
HOLD_SPEED = 10    # 1 detik nyata = 10 detik hold (supaya tes tidak lama)
TOL = 2.0
ON_T, OFF_T = 50.0, 48.0
LOG_HDR = "datetime,temp_c,burner_on_s,burner_status\n"
NAME_RE = re.compile(r"^[A-Za-z0-9_]{1,32}$")

app = Flask(__name__, static_folder=STATIC, static_url_path="")
L = threading.Lock()

S = dict(pv=AMBIENT, sp=0, run=False, phase="IDLE", valve=False, ign=0, burner=False,
         on_s=0, hold_total=0, hold_left=0, event="Boot", logging=False,
         log="", resume_sep=False, sd="ok")


def now():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def path_for(name):
    return os.path.join(LOG_DIR, name + ".csv")


def clear_active():
    S["log"] = ""
    S["resume_sep"] = False


def do_stop(reason):
    S.update(run=False, phase="IDLE", valve=False, ign=0, event=reason)


def write_row():
    if S["sd"] != "ok" or not S["log"]:
        return
    p = path_for(S["log"])
    new = not os.path.exists(p)
    with open(p, "a", encoding="utf-8", newline="") as f:
        if new:
            f.write(LOG_HDR)
        if S["resume_sep"]:
            f.write("# RESUME %s\n" % now())
            S["resume_sep"] = False
        f.write("%s,%.1f,%d,%s\n" % (now(), S["pv"], S["on_s"], "ON" if S["burner"] else "OFF"))


def tick_loop():
    while True:
        time.sleep(1)
        with L:
            if S["ign"] > 0:
                S["ign"] -= 1
            prev_valve = S["valve"]
            if S["run"]:
                if S["phase"] == "HEATING":
                    S["valve"] = True
                    S["pv"] = min(S["sp"], S["pv"] + HEAT_RATE)
                    if S["pv"] >= S["sp"] - TOL:
                        S["phase"] = "HOLD"
                        S["event"] = "Menahan SP"
                elif S["phase"] == "HOLD":
                    S["valve"] = False
                    S["hold_left"] = max(0, S["hold_left"] - HOLD_SPEED)
                    if S["hold_left"] <= 0:
                        do_stop("Tahan selesai (soak Omron)")
            else:
                S["valve"] = False
                S["pv"] = max(AMBIENT, S["pv"] - COOL_RATE)
            if S["valve"] and not prev_valve:
                S["ign"] = 4
            if not S["burner"] and S["pv"] > ON_T:
                S["burner"] = True
            elif S["burner"] and S["pv"] <= OFF_T:
                S["burner"] = False
            if S["burner"]:
                S["on_s"] += 1
            if S["logging"]:
                write_row()
                if not S["run"] and not S["burner"]:
                    S["logging"] = False
                    clear_active()


def err(msg, code=400, **extra):
    return jsonify(dict(ok=False, error=msg, **extra)), code


# ---------------- halaman ----------------
@app.route("/")
def index():
    return send_from_directory(STATIC, "index.html")


@app.route("/chart.umd.js")
def chart_js():
    # file upload kadang ter-rename jadi chart_umd.js; dua-duanya diterima
    for n in ("chart.umd.js", "chart_umd.js"):
        if os.path.exists(os.path.join(STATIC, n)):
            return send_from_directory(STATIC, n)
    return Response("chart.umd.js tidak ada di folder static", 404)


@app.after_request
def no_cache(r):
    r.headers["Cache-Control"] = "no-cache"
    return r


# ---------------- API (meniru firmware) ----------------
@app.get("/api/ping")
def ping():
    return jsonify(ok=True, heap=200000)


@app.get("/api/status")
def status():
    with L:
        return jsonify(
            pv=round(S["pv"], 1), sp=S["sp"], run=S["run"], phase=S["phase"],
            valve="ON" if S["valve"] else "OFF", igniter="ON" if S["ign"] > 0 else "OFF",
            burner="ON" if S["burner"] else "OFF", on_s=S["on_s"],
            hold_total_s=S["hold_total"], hold_left_s=S["hold_left"], sd=S["sd"],
            omron="ok", mb_err="0x00", sp_omron=S["sp"], werr="0x00", mv=0.0,
            lba="ok", lba_tmr=0, soak_rem=S["hold_left"] // 60,
            lba_omron="ok", sub1="ok", time_src="RTC", rtc="ok",
            log=S["log"], logging=S["logging"], event=S["event"], ts=now())


@app.post("/api/start")
def start():
    a = request.args
    try:
        sp = float(a.get("sp", ""))
    except ValueError:
        return err("Input tidak valid")
    hold = int(a.get("hold_min", "0") or 0)
    no_sd = "nosd" in a
    if not (0 < sp <= 1200):
        return err("SP harus 1-1200")
    if hold < 1 or hold > 5999:
        return err("Tahan minimal 1 menit, maks 99 jam 59 menit")
    with L:
        if S["run"]:
            return err("Burner sudah berjalan", 409, need_sd_confirm=False)
        if S["sd"] != "ok" and not no_sd:
            return err("SD card tidak terdeteksi", 409, need_sd_confirm=True)
        if S["sd"] == "ok" and not S["log"]:
            return err("Belum ada sesi log. Buat CSV dulu di halaman Log", 409, need_sd_confirm=False)
        with_log = S["sd"] == "ok"
        S.update(sp=sp, run=True, phase="HEATING", on_s=0, logging=with_log,
                 hold_total=hold * 60, hold_left=hold * 60)
        if with_log:
            S["event"] = "Start SP %.0f, tahan %02d:%02d" % (sp, hold // 60, hold % 60)
            write_row()
        else:
            clear_active()
            S["event"] = "Start SP %.0f, %02d:%02d, tanpa log" % (sp, hold // 60, hold % 60)
    return jsonify(ok=True)


@app.post("/api/stop")
def stop():
    with L:
        if S["run"]:
            do_stop("Manual stop")
    return jsonify(ok=True)


@app.post("/api/igniter")
def igniter():
    with L:
        if S["run"]:
            return err("Tidak bisa manual saat sesi berjalan", 409)
        if S["ign"] > 0:
            return err("Pematik masih menyala, tunggu selesai", 409)
        S["ign"] = 20   # meniru program Omron: pematik nyala ~20 dtk
    return jsonify(ok=True)


@app.post("/api/time")
def set_time():
    return jsonify(ok=True, set=True)


@app.post("/api/time/force")
def force_time():
    with L:
        if S["run"] or S["logging"]:
            return err("Sesi sedang berjalan", 409)
    return jsonify(ok=True)


@app.get("/api/logs")
def logs():
    files = []
    with L:
        if S["sd"] == "ok":
            for fn in os.listdir(LOG_DIR):
                if not fn.endswith(".csv") or not NAME_RE.match(fn[:-4]):
                    continue
                p = os.path.join(LOG_DIR, fn)
                last = ""
                with open(p, "r", encoding="utf-8") as f:
                    for line in f:
                        m = re.search(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d", line)
                        if m:
                            last = m.group(0)
                files.append(dict(name=fn[:-4], size=os.path.getsize(p), last=last))
        return jsonify(sd=S["sd"], active=S["log"], files=files)


@app.post("/api/log/init")
def log_init():
    name = request.args.get("name", "")
    resume = request.args.get("mode") == "resume"
    if not NAME_RE.match(name):
        return err("Nama: huruf, angka, underscore (1-32)")
    p = path_for(name)
    with L:
        if S["run"] or S["logging"]:
            return err("Sesi sedang berjalan", 409, exists=False)
        if S["sd"] != "ok":
            return err("SD card tidak terdeteksi", 503, exists=False)
        exists = os.path.exists(p)
        if resume:
            if not exists:
                return err("File tidak ditemukan", 404, exists=False)
            S["log"] = name
            S["resume_sep"] = True
        elif exists:
            return err("File sudah ada", 409, exists=True)
        else:
            with open(p, "w", encoding="utf-8", newline="") as f:
                f.write(LOG_HDR)
                f.write("# Sesi dibuat %s\n" % now())
            S["log"] = name
            S["resume_sep"] = False
    return jsonify(ok=True)


@app.get("/api/log")
def log_get():
    with L:
        name = request.args.get("file") or S["log"]
        if not NAME_RE.match(name or ""):
            return err("Pilih file log dulu")
        if S["sd"] != "ok":
            return err("SD card tidak terdeteksi", 503)
        p = path_for(name)
        if not os.path.exists(p):
            return err("File tidak ditemukan", 404)
        with open(p, "r", encoding="utf-8") as f:
            data = f.read()
    r = Response(data or LOG_HDR, mimetype="text/csv")
    if "dl" in request.args:
        r.headers["Content-Disposition"] = "attachment; filename=%s.csv" % name
    return r


@app.post("/api/log/delete")
def log_delete():
    name = request.args.get("file", "")
    if not NAME_RE.match(name):
        return err("Nama file tidak valid")
    with L:
        if S["logging"] and name.lower() == S["log"].lower():
            return err("File sedang dipakai logging", 409)
        if S["sd"] != "ok":
            return err("SD card tidak terdeteksi", 409)
        p = path_for(name)
        if os.path.exists(p):
            os.remove(p)
        if name.lower() == S["log"].lower():
            clear_active()
    return jsonify(ok=True)


# ---------------- kontrol khusus mock (tidak ada di firmware) ----------------
@app.get("/mock/sd/<state>")
def mock_sd(state):
    if state not in ("ok", "missing", "error"):
        return err("pakai ok / missing / error")
    with L:
        S["sd"] = state
    return "SD card diset ke: %s. Kembali ke <a href='/'>dashboard</a>." % state


@app.get("/mock/reset")
def mock_reset():
    with L:
        S.update(pv=AMBIENT, sp=0, run=False, phase="IDLE", valve=False, ign=0, burner=False,
                 on_s=0, hold_total=0, hold_left=0, event="Reset", logging=False, sd="ok")
        clear_active()
    return "Direset. Kembali ke <a href='/'>dashboard</a>."


if __name__ == "__main__":
    threading.Thread(target=tick_loop, daemon=True).start()
    print("Static :", STATIC)
    print("Logs   :", LOG_DIR)
    app.run(host="127.0.0.1", port=5000, debug=False)
