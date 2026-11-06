const $ = id => document.getElementById(id);
const sel = $("sel");
for (let v = 50; v <= 1200; v += 50) sel.add(new Option(v + " °C", v));
const p2 = n => String(n).padStart(2, "0");
const fmt = s => [s / 3600 | 0, (s % 3600) / 60 | 0, s % 60].map(p2).join(":");
const TSRC = { RTC: "RTC", HP: "HP", none: "Belum" };
const RTCS = { ok: "OK", lost: "Jam hilang", none: "Tidak ada" };
const PH = { IDLE: "Siap", HEATING: "Pemanasan", HOLD: "Menahan SP" };

async function api(path, body) {
  const opt = body === undefined ? {} : { method: "POST" };   // parameter lewat query string
  return (await fetch(path, opt)).json();
}

let fails = 0;
async function poll() {
  try {
    const d = await api("/api/status");
    $("pv").textContent = d.pv.toFixed(1);
    $("sp").textContent = d.sp + " °C";
    $("ph").textContent = PH[d.phase] || d.phase;
    $("st").textContent = d.burner; $("st").className = d.burner;
    $("vl").textContent = d.valve; $("vl").className = d.valve;
    $("ig").textContent = d.igniter ?? "--"; $("ig").className = d.igniter || "";
    $("on").textContent = fmt(d.on_s);
    $("hl").textContent = d.phase === "HOLD" ? fmt(d.hold_left_s)
      : d.run ? (d.hold_total_s ? "Belum mulai" : "Tanpa tahan") : "--";
    $("sd").textContent = d.sd;
    $("sd").className = d.sd === "ok" ? "good" : "FAULT";
    // Sesi log: nama file aktif; "Tanpa log" bila sesi jalan tanpa logging
    const noLog = d.run && !d.logging;
    $("lg").textContent = noLog ? "Tanpa log" : (d.log || "Belum diisi");
    $("lg").className = "small " + (noLog ? "FAULT" : d.log ? "good" : "OFF");
    const lba = d.lba_omron === "alarm", sub = d.sub1 === "alarm";
    $("al").textContent = lba ? "LBA" : sub ? "SUB1" : "Normal";
    $("al").className = lba || sub ? "FAULT" : "good";
    $("alarm").hidden = !(lba || sub);
    $("alarm").textContent = lba ? "ALARM LBA: loop kontrol Omron tidak merespons (cek heater, sensor, valve)"
      : "ALARM Omron aktif (SUB1: PRG.E / LBA)";
    $("raw").textContent = "Status Omron: " + (d.stop_omron === "stop" ? "STOP" : "RUN") +
      " | 0x2001=" + d.st_up + " 0x2002=" + d.st_lo + (d.burner_src === "suhu" ? " | bit STOP belum diatur (pakai suhu)" : "");
    $("tsrc").textContent = TSRC[d.time_src] || "--";
    $("tsrc").className = d.time_src === "none" ? "FAULT" : "";
    $("rtc").textContent = RTCS[d.rtc] || "--";
    $("rtc").className = d.rtc === "ok" ? "good" : "FAULT";
    $("ev").textContent = "Event terakhir: " + d.event + " (" + d.ts + ")";
    ["start", "sel", "hh", "mm", "tsync"].forEach(id => $(id).disabled = d.run);
    $("ign_btn").disabled = d.run || d.igniter === "ON";   // pematik dikunci selama menyala (program Omron)
    fails = 0; $("conn").hidden = true;
  } catch (e) {
    if (++fails >= 3) $("conn").hidden = false;   // 3x gagal berturut-turut: data di layar tidak terbaru
  }
}

// ---- Start: kalau SD tidak terbaca, server minta konfirmasi -> modal ----
async function doStart(noSd) {
  const hold = (+$("hh").value || 0) * 60 + (+$("mm").value || 0);
  const r = await api("/api/start?sp=" + sel.value + "&hold_min=" + hold + (noSd ? "&nosd=1" : ""), {});
  if (r.need_sd_confirm) { openModal(); return; }
  $("msg").className = "err"; $("msg").textContent = r.ok ? "" : r.error; poll();
}
$("start").onclick = () => doStart(false);

// ---- Modal konfirmasi: tombol "lanjut" terkunci 10 detik ----
let cdTimer = null;
const LOCK_S = 10;
function openModal() {
  let left = LOCK_S;
  const yes = $("myes");
  yes.disabled = true;
  yes.textContent = "Lanjut tanpa logging (" + left + ")";
  $("modal").hidden = false;
  $("mno").focus();
  clearInterval(cdTimer);
  cdTimer = setInterval(() => {
    left--;
    if (left > 0) { yes.textContent = "Lanjut tanpa logging (" + left + ")"; return; }
    clearInterval(cdTimer);
    yes.disabled = false;
    yes.textContent = "Ya, lanjut tanpa logging";
  }, 1000);
}
function closeModal() {
  clearInterval(cdTimer);
  $("modal").hidden = true;
  $("start").focus();
}
$("mno").onclick = closeModal;
$("myes").onclick = async () => {
  if ($("myes").disabled) return;
  closeModal();
  await doStart(true);
};
document.addEventListener("keydown", e => { if (e.key === "Escape" && !$("modal").hidden) closeModal(); });

$("tsync").onclick = async () => {
  const r = await api("/api/time/force?epoch=" + Math.floor(Date.now() / 1000), {});
  $("msg").className = r.ok ? "err ok" : "err";
  $("msg").textContent = r.ok ? "Jam disinkronkan dari perangkat ini" : r.error; poll();
};
$("stop").onclick = async () => { await api("/api/stop", {}); poll(); };
$("ign_btn").onclick = async () => {
  const r = await api("/api/igniter", {});
  $("msg").className = r.ok ? "err ok" : "err";
  $("msg").textContent = r.ok ? "Pematik dipicu. Lama nyala diatur program Omron." : r.error;
  poll();
};

api("/api/time?epoch=" + Math.floor(Date.now() / 1000), {}).catch(() => {});
setInterval(poll, 1000); poll();

// Layar lebar (laptop): tampilkan grafik di halaman utama. Chart.js dan log.js baru dimuat di sini.
if (matchMedia("(min-width:1000px)").matches) {
  const s = document.createElement("script");
  s.src = "chart.umd.js";
  s.onload = () => {
    const t = document.createElement("script");
    t.src = "log.js";
    document.body.appendChild(t);
  };
  document.body.appendChild(s);
}
