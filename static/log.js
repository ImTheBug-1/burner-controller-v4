// Dipakai di dua tempat:
//  - log.html  : init sesi, pilih file CSV, resume, grafik per file
//  - index.html: grafik file sesi aktif (atau file terakhir yang aktif)
// Semua dibungkus IIFE supaya tidak bentrok dengan variabel global di app.js.
(() => {
  const g = id => document.getElementById(id);
  const MAXPTS = 600;
  const NAME_RE = /^[A-Za-z0-9_]{1,32}$/;
  const HINT = "Huruf, angka, dan underscore saja (maks 32 karakter). Spasi diganti underscore.";
  const isLogPage = !!g("file");
  let chart = null, files = [], sdState = "ok", busy = false, lockedTo = "";

  Chart.defaults.color = "#5a6a78";

  const p2 = n => String(n).padStart(2, "0");
  const phoneNow = () => {
    const d = new Date();
    return d.getFullYear() + "-" + p2(d.getMonth() + 1) + "-" + p2(d.getDate()) + " " +
           p2(d.getHours()) + ":" + p2(d.getMinutes()) + ":" + p2(d.getSeconds());
  };
  const fmtSize = b => b < 1024 ? b + " B" : b < 1048576 ? (b / 1024).toFixed(1) + " KB" : (b / 1048576).toFixed(1) + " MB";
  const post = async path => (await fetch(path, { method: "POST" })).json();

  const lastSeen = {
    get() { try { return sessionStorage.getItem("lastLog") || ""; } catch (e) { return ""; } },
    set(v) { try { sessionStorage.setItem("lastLog", v); } catch (e) {} }
  };

  // File yang digrafikkan: di halaman Log = pilihan dropdown, di Dashboard = sesi aktif / terakhir aktif
  async function current() {
    if (isLogPage) return g("file").value;
    let a = "";
    try { a = (await (await fetch("/api/status")).json()).log || ""; } catch (e) {}
    if (a) lastSeen.set(a);
    return a || lastSeen.get();
  }

  async function draw() {
    const info = g("info");
    const f = await current();
    if (g("dl")) g("dl").href = f ? "/api/log?dl=1&file=" + encodeURIComponent(f) : "#";
    if (!f) {
      if (chart) { chart.destroy(); chart = null; }
      info.textContent = isLogPage ? "Belum ada file dipilih." : "Belum ada sesi log. Buat CSV di halaman Log.";
      return;
    }
    let t;
    try {
      const r = await fetch("/api/log?file=" + encodeURIComponent(f));
      if (!r.ok) { info.textContent = "Gagal memuat " + f + " (SD tidak terbaca atau file hilang)."; return; }
      t = await r.text();
    } catch (e) { info.textContent = "Gagal memuat " + f + "."; return; }
    const rows = t.trim().split("\n").slice(1).map(l => l.split(",")).filter(r => r.length >= 4);
    const step = Math.max(1, Math.ceil(rows.length / MAXPTS));
    const pts = rows.filter((r, i) => i % step === 0 || i === rows.length - 1);
    info.textContent = f + ": " + rows.length + " baris, grafik menampilkan " + pts.length + " titik";
    if (chart) chart.destroy();
    chart = new Chart(g("c"), {
      type: "line",
      data: {
        labels: pts.map(r => r[0].slice(11)),
        datasets: [{ label: "Suhu (°C) - " + f, data: pts.map(r => +r[1]),
                     borderColor: "#c8430b", pointRadius: 0, tension: 0.2 }]
      },
      options: { responsive: true, maintainAspectRatio: false, animation: false,
                 scales: { x: { ticks: { maxTicksLimit: 8 } } } }
    });
  }

  g("upd").onclick = draw;

  if (!isLogPage) { draw(); return; }   // Dashboard: cukup grafik

  // ================= Halaman Log =================
  const setHint = (txt, cls) => { const h = g("hint"); h.textContent = txt; h.className = "hint" + (cls ? " " + cls : ""); };
  const nameOk = () => NAME_RE.test(g("nm").value);

  function applyLock() {
    g("nm").disabled = busy;
    g("mk").disabled = busy || !nameOk();
    g("file").disabled = busy;
    g("resume").disabled = busy || !g("file").value;
  }

  function showFileInfo() {
    const f = files.find(x => x.name === g("file").value);
    g("fileinfo").textContent =
      sdState !== "ok" ? "SD card: " + sdState + ". Daftar file tidak tersedia."
      : !files.length ? "Belum ada file CSV. Buat percobaan baru di atas."
      : f ? f.name + ".csv, " + fmtSize(f.size) + (f.last ? ", data terakhir " + f.last : ", belum ada data") : "";
  }

  async function loadFiles(prefer) {
    let j;
    try { j = await (await fetch("/api/logs")).json(); }
    catch (e) { g("fileinfo").textContent = "Gagal memuat daftar file."; return; }
    sdState = j.sd;
    files = (j.files || []).slice().sort((a, b) => (b.last || "").localeCompare(a.last || "") || a.name.localeCompare(b.name));
    const s = g("file");
    const keep = (prefer || s.value || "").toLowerCase();
    s.innerHTML = "";
    files.forEach(f => s.add(new Option(
      f.name + (f.name === j.active ? " (aktif)" : "") + "  |  " + fmtSize(f.size) + "  |  " + (f.last || "kosong"), f.name)));
    const hit = files.find(f => f.name.toLowerCase() === keep);
    if (hit) s.value = hit.name;
    showFileInfo();
    applyLock();
  }

  async function refreshStatus() {
    try {
      const d = await (await fetch("/api/status")).json();
      g("tsnow").value = d.ts && d.ts !== "NOTIME" ? d.ts : phoneNow();
      const nowBusy = d.run || d.logging;
      g("actinfo").textContent = nowBusy
        ? "Percobaan berjalan" + (d.log ? ", mencatat ke " + d.log : "") + ". Pilihan file dikunci sampai selesai."
        : d.log ? "Sesi aktif: " + d.log + ". Siap dinyalakan dari Dashboard." : "Belum ada sesi aktif.";
      if (nowBusy !== busy) { busy = nowBusy; if (!busy) { lockedTo = ""; await loadFiles(); } }
      if (busy && d.log && lockedTo !== d.log) { lockedTo = d.log; await loadFiles(d.log); draw(); }
      applyLock();
    } catch (e) { /* perangkat sedang tidak terjangkau; coba lagi di siklus berikut */ }
  }

  g("nm").oninput = () => {
    const v = g("nm").value;
    if (v && !nameOk()) setHint("Hanya huruf, angka, dan underscore. Ganti spasi dengan _ (contoh: percobaan_1).", "bad");
    else setHint(HINT, "");
    applyLock();
  };

  g("mk").onclick = async () => {
    const n = g("nm").value;
    if (!NAME_RE.test(n)) return;
    g("mk").disabled = true;
    let r;
    try { r = await post("/api/log/init?mode=new&name=" + encodeURIComponent(n)); }
    catch (e) { setHint("Tidak terhubung ke perangkat.", "bad"); applyLock(); return; }
    if (r.ok) {
      setHint("Sesi " + n + " siap. Kembali ke Dashboard lalu tekan Nyalakan burner.", "ok");
      g("nm").value = "";
      await loadFiles(n); await refreshStatus(); draw();
    } else if (r.exists) {
      setHint("File " + n + " sudah ada. Pilih file itu di daftar File CSV untuk dilanjutkan, atau ganti dengan nama baru.", "bad");
      await loadFiles(n); g("file").focus(); draw();
    } else {
      setHint(r.error || "Gagal membuat file.", "bad");
    }
    applyLock();
  };

  g("resume").onclick = async () => {
    const n = g("file").value;
    if (!n) return;
    if (!confirm("Lanjutkan menulis ke " + n + ".csv? Data baru ditambahkan di bawah penanda RESUME.")) return;
    let r;
    try { r = await post("/api/log/init?mode=resume&name=" + encodeURIComponent(n)); }
    catch (e) { setHint("Tidak terhubung ke perangkat.", "bad"); return; }
    setHint(r.ok ? "Sesi " + n + " aktif (lanjut). Kembali ke Dashboard lalu tekan Nyalakan burner." : (r.error || "Gagal."), r.ok ? "ok" : "bad");
    await loadFiles(n); await refreshStatus();
  };

  g("file").onchange = () => { showFileInfo(); applyLock(); draw(); };

  setHint(HINT, "");
  g("tsnow").value = phoneNow();
  loadFiles().then(draw);
  refreshStatus();
  setInterval(refreshStatus, 2000);
})();
