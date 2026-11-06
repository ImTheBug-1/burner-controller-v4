#!/usr/bin/env python3
"""
serial_logger.py - Backup logging burner lewat USB serial ESP32-S3 (sistem logging ke-2, di laptop).

Firmware mengirim 1 baris per detik:
    $LOG,2026-11-02 12:00:00,48.0,50.0,ON
    ($LOG, datetime, temp_c, setpoint_c, burner ON/OFF)
Baris lain (boot, debug heartbeat) diabaikan. Jika jam ESP belum ter-set ("NOTIME"),
datetime diganti jam laptop dan kolom ts_source diisi "pc".

Pakai:
    pip install pyserial
    python serial_logger.py                # auto-pilih port
    python serial_logger.py --list         # lihat daftar port
    python serial_logger.py --port COM5
    python serial_logger.py --port COM5 --out D:\\backup_log --show-other

Output: <out>/serial_log_YYYYMMDD.csv (1 file per hari, di-append, di-flush tiap baris).
Tutup Serial Monitor Arduino IDE dulu, karena satu port hanya bisa dibuka satu program.
Ctrl+C untuk berhenti.
"""
import argparse
import csv
import os
import re
import sys
import time
from datetime import datetime

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial belum terpasang. Jalankan: pip install pyserial")

LINE_RE = re.compile(
    r"^\$LOG,(?P<ts>[^,]+),(?P<pv>-?\d+(?:\.\d+)?),(?P<sp>-?\d+(?:\.\d+)?),(?P<st>ON|OFF)$"
)
HEADER = ["datetime", "temp_c", "setpoint_c", "burner_status", "ts_source"]
ESP_VID = 0x303A            # Espressif (USB native ESP32-S3)
TS_FMT = "%Y-%m-%d %H:%M:%S"


def parse_line(line):
    """Kembalikan [datetime, temp, sp, status, ts_source] atau None bila bukan baris $LOG valid."""
    m = LINE_RE.match(line.strip())
    if not m:
        return None
    ts, src = m.group("ts"), "esp"
    if ts == "NOTIME":
        ts, src = datetime.now().strftime(TS_FMT), "pc"
    else:
        try:
            datetime.strptime(ts, TS_FMT)
        except ValueError:
            return None
    return [ts, "%.1f" % float(m.group("pv")), "%.1f" % float(m.group("sp")), m.group("st"), src]


def pick_port(arg):
    if arg:
        return arg
    ports = list(list_ports.comports())
    if not ports:
        return None
    esp = [p for p in ports if p.vid == ESP_VID]
    if len(esp) == 1:
        return esp[0].device
    if len(ports) == 1:
        return ports[0].device
    print("Beberapa port ditemukan:")
    for i, p in enumerate(ports):
        print("  [%d] %s  %s" % (i, p.device, p.description))
    while True:
        try:
            return ports[int(input("Pilih nomor port: "))].device
        except (ValueError, IndexError):
            print("Nomor tidak valid.")


def open_serial(port, baud):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 1
    ser.rts = False          # RTS sering terhubung ke EN pada USB ESP32: jaga supaya board tidak ter-reset saat dibuka
    ser.open()
    return ser


class CsvSink:
    """CSV per hari; ganti file otomatis saat lewat tengah malam."""

    def __init__(self, folder):
        self.folder = folder
        self.day = None
        self.f = None
        self.w = None
        os.makedirs(folder, exist_ok=True)

    def _open(self, day):
        self.close()
        path = os.path.join(self.folder, "serial_log_%s.csv" % day)
        new = not os.path.exists(path) or os.path.getsize(path) == 0
        self.f = open(path, "a", newline="", encoding="utf-8")
        self.w = csv.writer(self.f)
        if new:
            self.w.writerow(HEADER)
        self.day = day
        print("[file] %s" % os.path.abspath(path))

    def write(self, row):
        day = datetime.now().strftime("%Y%m%d")
        if day != self.day:
            self._open(day)
        self.w.writerow(row)
        self.f.flush()
        os.fsync(self.f.fileno())

    def close(self):
        if self.f:
            self.f.close()
            self.f = self.w = None


def main():
    ap = argparse.ArgumentParser(description="Backup logger serial ESP32-S3 burner")
    ap.add_argument("--port", help="contoh: COM5 (kosong = auto)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default="serial_logs", help="folder output CSV")
    ap.add_argument("--list", action="store_true", help="tampilkan daftar port lalu keluar")
    ap.add_argument("--show-other", action="store_true", help="tampilkan juga baris serial non-$LOG")
    args = ap.parse_args()

    if args.list:
        for p in list_ports.comports():
            print("%s  %s  (VID:PID=%s:%s)" % (
                p.device, p.description,
                "%04X" % p.vid if p.vid else "----", "%04X" % p.pid if p.pid else "----"))
        return

    sink = CsvSink(args.out)
    count = 0
    try:
        while True:
            port = pick_port(args.port)
            if not port:
                print("Tidak ada port serial. Mencoba lagi...")
                time.sleep(2)
                continue
            try:
                ser = open_serial(port, args.baud)
            except serial.SerialException as e:
                print("Gagal buka %s (%s). Mencoba lagi..." % (port, e))
                time.sleep(2)
                continue

            print("[ok] terhubung ke %s. Ctrl+C untuk berhenti." % port)
            last_rx, warned = time.time(), False
            try:
                while True:
                    raw = ser.readline()
                    if not raw:
                        if time.time() - last_rx > 10 and not warned:
                            print("[!] belum ada data selama 10 detik (cek kabel / board ter-reset?)")
                            warned = True
                        continue
                    last_rx, warned = time.time(), False
                    line = raw.decode("utf-8", "replace").strip()
                    row = parse_line(line)
                    if row:
                        sink.write(row)
                        count += 1
                        print("%s  PV %7s  SP %7s  %-3s  [%d baris]%s" % (
                            row[0][11:], row[1], row[2], row[3], count,
                            "  (jam laptop)" if row[4] == "pc" else ""))
                    elif args.show_other and line:
                        print("   | " + line)
            except (serial.SerialException, OSError) as e:
                print("[!] koneksi terputus (%s). Menyambung ulang..." % e)
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(2)
    except KeyboardInterrupt:
        print("\nBerhenti. Total %d baris tersimpan di %s" % (count, os.path.abspath(args.out)))
    finally:
        sink.close()


if __name__ == "__main__":
    main()
