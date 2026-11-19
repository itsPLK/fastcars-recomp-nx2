"use strict";
// The installer's steps. Talks to installer/wizard/server.py over a small JSON API.

const STEPS = [
  ["version", "Version"], ["checks", "Checks"], ["files", "Game files"], ["console", "Console"],
  ["summary", "Summary"], ["run", "Build & copy"], ["firststart", "First start"], ["done", "Done"],
];
const $ = (id) => document.getElementById(id);
const S = { st: null, releases: [], choice: null, cinfo: null, mode: "ftp", plan: [], after: null };

async function api(path, body) {
  const opt = body === undefined ? {} : { method: "POST", body: JSON.stringify(body),
    headers: { "Content-Type": "application/json" } };
  const r = await fetch(path, opt);
  const j = await r.json().catch(() => ({ error: `HTTP ${r.status}` }));
  if (!r.ok && !j.error) j.error = `HTTP ${r.status}`;
  return j;
}

function gb(bytes) { return bytes == null ? "unknown" : (bytes / 2 ** 30).toFixed(1) + " GB"; }

// Version comparison ("v1.1.0-rc1[-commit]"), as installer/core/releases.py does it.
function vkey(v) {
  const m = /^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?/.exec(v || "");
  if (!m) return null;
  let pre = (m[4] || "").replace(/(^|-)[0-9a-f]{7,40}(-dirty)?$/, "").replace(/(^|-)dirty$/, "");
  const ids = (pre.match(/\d+|[A-Za-z]+/g) || []).map((t) => /^\d+$/.test(t) ? [0, +t, ""] : [1, 0, t.toLowerCase()]);
  return [+m[1], +m[2], +m[3], pre ? 0 : 1, ids];
}
function vbase(v) {
  const m = /^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?/.exec(v || "");
  if (!m) return v;
  const pre = (m[4] || "").replace(/(^|-)[0-9a-f]{7,40}(-dirty)?$/, "").replace(/(^|-)dirty$/, "");
  return `v${m[1]}.${m[2]}.${m[3]}` + (pre ? "-" + pre : "");
}
function vcmp(a, b) {
  const x = vkey(a), y = vkey(b);
  if (!x || !y) return 0;
  const cmp = (p, q) => {
    if (Array.isArray(p)) {
      for (let i = 0; i < Math.max(p.length, q.length); i++) {
        if (p[i] === undefined) return -1;
        if (q[i] === undefined) return 1;
        const c = cmp(p[i], q[i]);
        if (c) return c;
      }
      return 0;
    }
    return p < q ? -1 : p > q ? 1 : 0;
  };
  return cmp(x, y);
}

// ---------------------------------------------------------------------------------------------
// Navigation

function show(step) {
  document.querySelectorAll("section").forEach((s) => s.classList.toggle("on", s.dataset.step === step));
  const idx = STEPS.findIndex(([k]) => k === step);
  $("steps").innerHTML = STEPS.map(([k, t], i) =>
    `<li class="${i === idx ? "on" : i < idx ? "done" : ""}">${t}</li>`).join("");
  S.step = step;
  window.scrollTo(0, 0);
}
const ORDER = STEPS.map(([k]) => k);
document.querySelectorAll("button.back").forEach((b) => b.addEventListener("click", () => {
  const i = ORDER.indexOf(S.step);
  let prev = ORDER[Math.max(0, i - 1)];
  if (prev === "run") prev = "summary";
  show(prev);
}));

async function refresh() {
  S.st = await api("/api/state");
  $("ver").textContent = S.st.version;
  return S.st;
}

// ---------------------------------------------------------------------------------------------
// 1. Version

async function loadReleases() {
  const r = await api("/api/releases");
  S.releases = r.releases || [];
  $("rel-status").textContent = r.error ? r.error + " You can still install this folder's version." :
    S.releases.length ? "" : "No releases are published yet: this folder's version is installed.";
  const last = (S.st.config || {}).last_installed;
  if (last && vkey(last) && vkey(last)[3] === 0) $("pre").checked = true;  // a tester stays on test versions
  if (S.st.from_release && vkey(S.st.base) && vkey(S.st.base)[3] === 0) $("pre").checked = true;  // running one
  renderReleases();
}

function renderReleases() {
  const pre = $("pre").checked;
  const list = S.releases.filter((r) => pre || !r.prerelease);
  const local = { tag: "local", name: `This folder as it is (${S.st.version})`, notes: "" };
  const items = [...list, local];
  if (!S.choice || !items.some((r) => r.tag === S.choice)) {
    const stable = list.find((r) => !r.prerelease);
    S.choice = (stable || list[0] || local).tag;
    if (S.st.from_release) S.choice = S.st.base;  // already switched to a release: keep it
  }
  $("rel-list").innerHTML = items.map((r) => `
    <div class="choice ${r.tag === S.choice ? "sel" : ""}" data-tag="${r.tag}">
      <span class="tag">${r.tag === "local" ? "" : r.tag}</span>
      <span>${r.tag === "local" ? r.name : (r.name !== r.tag ? r.name : "")}</span>
      ${r.prerelease ? '<span class="badge">test version</span>' : ""}
      ${r.tag !== "local" && r.tag === S.st.base ? '<span class="badge">this folder</span>' : ""}
      <span class="muted small">${r.date || ""}</span>
    </div>`).join("");
  document.querySelectorAll("#rel-list .choice").forEach((el) => el.addEventListener("click", () => {
    S.choice = el.dataset.tag; renderReleases();
  }));
  const sel = items.find((r) => r.tag === S.choice);
  $("rel-notes-box").hidden = !(sel && sel.notes);
  $("rel-notes").textContent = sel ? sel.notes : "";
}
$("pre").addEventListener("change", renderReleases);

$("version-next").addEventListener("click", async () => {
  if (S.choice !== "local" && !(S.choice === S.st.base && !S.st.version.endsWith("-dirty"))) {
    $("version-next").disabled = true;
    $("rel-status").textContent = `Getting ${S.choice}…`;
    const r = await api("/api/use", { tag: S.choice });
    if (r.error) { $("rel-status").textContent = r.error; $("version-next").disabled = false; return; }
    if (r.restarting) {
      $("rel-status").textContent = `Starting the installer of ${S.choice}…`;
      for (let i = 0; i < 120; i++) {
        await new Promise((res) => setTimeout(res, 1000));
        try {
          const st = await api("/api/state");
          if (st.base === S.choice) { location.reload(); return; }
        } catch (e) { /* restarting */ }
      }
      $("rel-status").textContent = "The installer didn't come back. Check its window.";
      return;
    }
  }
  showChecks();
});

// ---------------------------------------------------------------------------------------------
// 2. Checks

async function showChecks() {
  await refresh();
  const d = S.st.docker;
  $("docker-state").innerHTML = d === "running" ? "✅ Docker is running." :
    d === "stopped" ? "⚠️ Docker is installed but not running. Start Docker Desktop (or the docker service), then check again." :
    `❌ Docker is not installed. <a href="${S.st.docker_url}" target="_blank" rel="noopener">Get Docker</a>, install it, start it, then check again.`;
  $("docker-help").hidden = !S.st.apple_silicon;
  $("docker-help").textContent = "On a Mac with Apple silicon: in Docker Desktop's settings, turn on " +
    "\"Use Rosetta for x86/amd64 emulation on Apple Silicon\". The build runs under emulation and takes longer.";
  $("work").textContent = S.st.work;
  const need = S.st.data_installed ? 8 : 60;
  const free = S.st.work_free;
  $("work-free").textContent = `${gb(free)} free here` +
    (free != null && free < need * 2 ** 30 ? ` — about ${need} GB are needed: choose another drive.` : ".");
  $("checks-next").disabled = d !== "running";
  show("checks");
}
$("docker-recheck").addEventListener("click", showChecks);
$("work-change").addEventListener("click", async () => {
  const r = await api("/api/pick", { kind: "folder", title: "Choose the work folder", initial: S.st.work });
  let path = r.path;
  if (r.unavailable) path = prompt("The work folder's path:", S.st.work);
  if (!path) return;
  await api("/api/config", { work: path });
  showChecks();
});
$("checks-next").addEventListener("click", () => {
  $("files-done").hidden = !S.st.data_installed;
  $("files-form").hidden = S.st.data_installed;
  const c = S.st.config || {};
  for (const k of ["disc1", "disc2", "tu"]) $(k).value = c[k] || "";
  show("files");
});

// ---------------------------------------------------------------------------------------------
// 3. Game files

document.querySelectorAll("button.pick").forEach((b) => b.addEventListener("click", async () => {
  const field = b.closest(".field");
  const input = field.querySelector("input");
  const r = await api("/api/pick", { kind: b.dataset.kind, title: field.querySelector("label").textContent,
                                    initial: input.value });
  if (r.unavailable) { input.focus(); input.placeholder = "Type or paste the path here"; return; }
  if (r.path) input.value = r.path;
}));
$("files-next").addEventListener("click", async () => {
  if (!S.st.data_installed) {
    const missing = ["disc1", "disc2", "tu"].filter((k) => !$(k).value.trim());
    if (missing.length) { alert("Choose Disc 1, Disc 2 and the title update first."); return; }
    await api("/api/config", { disc1: $("disc1").value.trim(), disc2: $("disc2").value.trim(), tu: $("tu").value.trim() });
  }
  showConsole();
});

async function showConsole() {
  const c = (await refresh()).config || {};
  setMode(c.mode || "ftp");
  $("ftp").value = c.ftp || "";
  $("sd").value = c.sd || "";
  $("console-state").textContent = "";
  show("console");
}
$("manage").addEventListener("click", () => { S.manage = true; showConsole(); });

// ---------------------------------------------------------------------------------------------
// 4. Console

function setMode(m) {
  S.mode = m;
  document.querySelectorAll(".tab").forEach((t) => t.classList.toggle("sel", t.dataset.mode === m));
  $("mode-ftp").hidden = m !== "ftp";
  $("mode-sd").hidden = m !== "sd";
}
document.querySelectorAll(".tab").forEach((t) => t.addEventListener("click", () => setMode(t.dataset.mode)));

function choiceList(el, items, onPick) {
  el.innerHTML = items.map((x) => `<div class="choice" data-v="${x}">${x}</div>`).join("");
  el.querySelectorAll(".choice").forEach((c) => c.addEventListener("click", () => {
    el.querySelectorAll(".choice").forEach((o) => o.classList.remove("sel"));
    c.classList.add("sel"); onPick(c.dataset.v);
  }));
}
$("ftp-find").addEventListener("click", async () => {
  $("ftp-find").disabled = true;
  $("ftp-found").innerHTML = '<p class="muted">Searching…</p>';
  const r = await api("/api/consoles", {});
  $("ftp-find").disabled = false;
  if (!(r.consoles || []).length) {
    $("ftp-found").innerHTML = '<p class="muted">None found. Is OunceShell open, and the game closed? Otherwise type the address OunceShell shows.</p>';
    return;
  }
  choiceList($("ftp-found"), r.consoles.map((ip) => ip + ":1337"), (v) => { $("ftp").value = v; });
  if (r.consoles.length === 1) $("ftp").value = r.consoles[0] + ":1337";
});
$("sd-find").addEventListener("click", async () => {
  const r = await api("/api/sdcards", {});
  if (!(r.cards || []).length) { $("sd-found").innerHTML = '<p class="muted">No SD card with a switch folder found.</p>'; return; }
  choiceList($("sd-found"), r.cards, (v) => { $("sd").value = v; });
  if (r.cards.length === 1) $("sd").value = r.cards[0];
});
$("sd-pick").addEventListener("click", async () => {
  const r = await api("/api/pick", { kind: "folder", title: "Choose the SD card", initial: $("sd").value });
  if (r.path) $("sd").value = r.path;
  if (r.unavailable) $("sd").focus();
});

function consoleBody() {
  return S.mode === "sd" ? { mode: "sd", sd: $("sd").value.trim() } : { mode: "ftp", ftp: $("ftp").value.trim() };
}
$("console-next").addEventListener("click", async () => {
  const body = consoleBody();
  if (!(body.ftp || body.sd)) { $("console-state").textContent = "Enter the console's address or the SD card first."; return; }
  $("console-next").disabled = true;
  $("console-state").textContent = "Connecting…";
  const r = await api("/api/console", body);
  $("console-next").disabled = false;
  if (!r.ok) { $("console-state").innerHTML = `<span class="err">${r.error}</span>`; return; }
  S.cinfo = r;
  await api("/api/config", Object.assign({ last_installed: r.installed || null }, body));
  if (S.manage) showMaintenance(); else showSummary();
});

// ---------------------------------------------------------------------------------------------
// 5. Summary

function showSummary() {
  const st = S.st, c = S.cinfo, v = st.version;
  let what;
  if (!c.installed) what = c.has_app ? `Install ${vbase(v)} (over the files already on the console)` : `Install ${vbase(v)}`;
  else {
    const d = vcmp(v, c.installed);
    what = d > 0 ? `Update ${vbase(c.installed)} → ${vbase(v)}` :
      d < 0 ? `Downgrade ${vbase(c.installed)} → ${vbase(v)} (older than what the console has)` : `Reinstall ${vbase(v)}`;
  }
  $("summary-what").textContent = what;
  const needBuild = st.built !== v || v.endsWith("-dirty");
  S.plan = [];
  const li = [];
  if (needBuild) {
    S.plan.push("build");
    li.push(st.data_installed ? "Build the game (a rebuild takes a while; the first one about an hour)." :
      "Copy and check your game files, then build the game (about an hour; longer on a Mac).");
  } else li.push("The game is already built for this version.");
  S.plan.push("install");
  li.push(`Copy to the console (${c.describe}): about 16 GB the first time, then only what changed. Saves are never overwritten.`);
  if (c.settings) li.push("Your settings are kept.");
  li.push(c.shaders ? `The ${c.shaders} translated shaders on the console are kept.` :
    "Shaders: the ones translated earlier on this computer are copied along; without any, a first " +
    "start on the console records them and they're translated here.");
  $("summary-list").innerHTML = li.map((x) => `<li>${x}</li>`).join("");
  $("reset-box").hidden = !c.settings;
  $("reset-settings").checked = false;
  show("summary");
}
$("go").addEventListener("click", () => runPlan());

// ---------------------------------------------------------------------------------------------
// 6. Build & copy

async function runJob(body, title) {
  $("run-title").textContent = title;
  $("run-stage").textContent = "";
  $("run-bar").style.width = "0";
  $("run-log").textContent = "";
  $("run-error").hidden = true;
  $("run-retry").hidden = true;
  $("run-back").hidden = true;
  show("run");
  if (body) {
    const s = await api("/api/job", body);
    if (s.error) return fail(s.error);
  }
  return poll((j) => {
    $("run-stage").textContent = j.stage || "";
    $("run-bar").style.width = Math.round(j.progress * 100) + "%";
    $("run-stop").hidden = !(j.running && ["build", "shaders", "clean"].includes(j.action));
  });
}
$("run-stop").addEventListener("click", async () => {
  $("run-stop").disabled = true;
  await api("/api/job/cancel", {});
  $("run-stop").disabled = false;
});

function fail(msg) {
  $("run-stop").hidden = true;
  $("run-error").textContent = msg;
  $("run-error").hidden = false;
  $("run-retry").hidden = false;
  $("run-back").hidden = false;
  return null;
}

async function poll(onUpdate) {
  let since = 0;
  for (;;) {
    const j = await api(`/api/job?since=${since}`);
    if (!j.action) return null;
    since = j.next;
    if (j.lines && j.lines.length) {
      const log = $("run-log");
      log.textContent += j.lines.join("\n") + "\n";
      log.scrollTop = log.scrollHeight;
    }
    onUpdate(j);
    if (!j.running) {
      if (j.error) return fail(j.error);
      return j.result || {};
    }
    await new Promise((r) => setTimeout(r, 1000));
  }
}

async function runPlan() {
  const c = Object.assign({}, S.st.config, consoleBody());
  for (const step of S.plan) {
    let r;
    if (step === "build") {
      $("run-hint").hidden = false;
      r = await runJob({ action: "build", disc1: c.disc1, disc2: c.disc2, tu: c.tu }, "Building");
    } else {
      $("run-hint").hidden = true;
      r = await runJob(Object.assign({ action: "install", reset_settings: $("reset-settings").checked }, consoleBody()),
                       "Copying to the console");
    }
    if (!r) { S.retry = runPlan; return; }
    if (step === "build") { await refresh(); S.plan = S.plan.filter((s) => s !== "build"); }
    // The copy may have brought shaders translated earlier (kept in the work folder).
    if (step === "install" && r.shaders_on_console) S.cinfo.shaders = S.cinfo.shaders || 1;
  }
  await refresh();
  if (r_shaders_needed()) showFirstStart(); else showDone();
}
function r_shaders_needed() { return !S.cinfo.shaders; }
$("run-retry").addEventListener("click", () => (S.retry || runPlan)());

// ---------------------------------------------------------------------------------------------
// 7. First start

async function showFirstStart() {
  show("firststart");
  $("fs-bar").style.width = "0";
  if (S.mode === "sd") {
    $("fs-state").textContent = "When you're back from the first start, put the SD card in this computer and press Fetch shaders now.";
    return;
  }
  $("fs-state").textContent = "Waiting for the game to start on the console…";
  const s = await api("/api/job", Object.assign({ action: "wait_first_start" }, consoleBody()));
  if (s.error) { $("fs-state").textContent = s.error; return; }
  const r = await poll((j) => {
    $("fs-state").textContent = j.stage === "Waiting for the first start" ?
      "Waiting for the game to start on the console…" : j.stage;
    $("fs-bar").style.width = Math.round(j.progress * 100) + "%";
  });
  if (r && r.shaders != null) { S.shadersDone = r.shaders; showDone(); }
  else if (S.step === "run") show("firststart");
}
$("fs-fetch").addEventListener("click", async () => {
  await api("/api/job/stop", {});
  await new Promise((r) => setTimeout(r, 1500));
  const r = await runJob(Object.assign({ action: "shaders" }, consoleBody()), "Shaders");
  S.retry = () => $("fs-fetch").click();
  if (r) { S.shadersDone = r.shaders; showDone(); }
});
$("fs-skip").addEventListener("click", async () => { await api("/api/job/stop", {}); showDone(); });

// ---------------------------------------------------------------------------------------------
// 8. Done

function showDone() {
  $("done-what").textContent = `${vbase(S.st.version)} is on your console` +
    (S.shadersDone != null ? `, with ${S.shadersDone} shaders translated.` : ".");
  show("done");
  renderDisk(document.querySelector('[data-step="done"] .disk'));
}
$("done-shaders").addEventListener("click", () => $("fs-fetch").click());
$("done-restart").addEventListener("click", () => location.reload());

// ---------------------------------------------------------------------------------------------

(async function start() {
  await refresh();
  show("version");
  if (S.st.job && S.st.job.running) {  // the page was reloaded during a step
    const r = await runJob(null, "Working");
    if (r) show("done");
    return;
  }
  loadReleases();
})();

// ---------------------------------------------------------------------------------------------
// Maintenance

async function showMaintenance(message) {
  const c = S.cinfo;
  $("m-what").textContent = c.installed ? `${vbase(c.installed)} is installed (${c.describe}).` :
    c.has_app ? `A game folder is on the console (${c.describe}), without install records.` :
    `Nothing is installed yet (${c.describe}).`;
  $("m-log").value = (c.settings && c.settings.log) || "error";
  $("m-result").hidden = !message;
  $("m-result").textContent = message || "";
  show("maintenance");
  renderDisk(document.querySelector('[data-step="maintenance"] .disk'));
}

async function pickFolder(title) {
  const r = await api("/api/pick", { kind: "folder", title, initial: (S.st.config || {}).last_dest || "" });
  let path = r.path;
  if (r.unavailable) path = prompt(title, (S.st.config || {}).last_dest || "");
  if (path) await api("/api/config", { last_dest: path });
  return path;
}

async function maintenanceJob(body, title, describe) {
  const r = await runJob(Object.assign(body, consoleBody()), title);
  if (!r) { S.retry = () => maintenanceJob(body, title, describe); return; }
  const info = await api("/api/console", consoleBody());
  if (info.ok) S.cinfo = info;
  showMaintenance(describe(r));
}

$("m-backup").addEventListener("click", async () => {
  const dest = await pickFolder("Where should the backup go?");
  if (dest) maintenanceJob({ action: "backup", dest }, "Backing up saves", (r) => `Saves backed up to ${r.path}`);
});
$("m-logs").addEventListener("click", async () => {
  const dest = await pickFolder("Where should the logs go?");
  if (dest) maintenanceJob({ action: "logs", dest }, "Collecting logs", (r) => `Logs saved as ${r.path}`);
});
$("m-log-set").addEventListener("click", async () => {
  const r = await api("/api/settings", Object.assign({ changes: { log: $("m-log").value } }, consoleBody()));
  showMaintenance(r.error ? r.error : `Log level set to ${$("m-log").value}. It applies from the next start.`);
});
$("m-shaders").addEventListener("click", () =>
  maintenanceJob({ action: "shaders" }, "Shaders", (r) => `${r.shaders} shaders fetched and translated.`));
$("m-uninstall").addEventListener("click", () => {
  const all = $("m-delete-saves").checked;
  if (!confirm(all ? "Remove the game AND your saves from the console? The saves can't be recovered." :
                     "Remove the game from the console? Your saves stay.")) return;
  maintenanceJob({ action: "uninstall", delete_saves: all }, "Uninstalling",
                 () => all ? "Removed, saves included." : "Removed. Your saves stay on the console.");
});
$("m-back").addEventListener("click", () => { S.manage = false; show("version"); });
$("m-install").addEventListener("click", () => { S.manage = false; showSummary(); });

// ---------------------------------------------------------------------------------------------
// Disk space on this computer

const CLEANUPS = [
  ["tidy", "Tidy up", "Removes the build tools of other versions. Updates stay fast."],
  ["build", "Delete build files", "Also the build files and this version's build tools: the next build takes about an hour again."],
  ["all", "Delete everything", "Also your extracted game files: the next build needs your discs again."],
];

async function renderDisk(el) {
  el.innerHTML = '<p class="muted">Measuring…</p>';
  const u = await api("/api/storage");
  if (u.error) { el.innerHTML = `<p class="err">${u.error}</p>`; return; }
  const rows = u.parts.map((p) => [p.what, p.size, p.cost]).concat(
    u.images.map((i) => [`Build tools (Docker image${i.current ? ", this version" : ", another version"})`, i.size, ""]));
  const total = rows.reduce((a, r) => a + r[1], 0);
  el.innerHTML = `<table>${rows.map((r) => `<tr><td>${r[0]}${r[2] ? `<div class="muted small">Deleting it: ${r[2]}</div>` : ""}</td>
    <td class="size">${gb(r[1])}</td></tr>`).join("")}<tr><td><b>Total</b></td><td class="size"><b>${gb(total)}</b></td></tr></table>
    <div class="cleanups">${CLEANUPS.map(([k, t]) => `<button class="secondary" data-level="${k}">${t}</button>`).join("")}
    <label class="check small"><input type="checkbox" class="del-shaders"> with "Delete everything": also the saved shaders</label></div>
    <p class="muted small">Folder: <code>${u.work}</code></p>`;
  el.querySelectorAll("button[data-level]").forEach((b) => b.addEventListener("click", async () => {
    const [, title, what] = CLEANUPS.find(([k]) => k === b.dataset.level);
    if (!confirm(`${title}? ${what}`)) return;
    const back = S.step;
    const r = await runJob({ action: "clean", level: b.dataset.level,
                             delete_shaders: el.querySelector(".del-shaders").checked }, title);
    if (!r) return;
    await refresh();
    if (back === "maintenance") showMaintenance(`Freed about ${gb(r.freed)}.`);
    else { show(back); renderDisk(el); }
  }));
}
