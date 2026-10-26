// Mario Kart 64 (recompiled) — browser host: boots the game, drives video timing,
// draws frames with WebGL2, and feeds controller input.
import { Renderer } from './gl.js';

const $ = id => document.getElementById(id);
const status = (t, err) => { const s = $('status'); s.textContent = t; s.hidden = !t; s.classList.toggle('err', !!err); };
const bc = new BroadcastChannel('mk64');
bc.onmessage = e => { if (e.data.fatal) status('The game stopped: ' + e.data.fatal, true); };

// ---- game files: your ROM + the recompiled game (mk64.wasm) ----
// Order: this device's cache (instant) -> files next to the page (PC play.bat) -> one-time setup.
const REPO = 'ZZ-games/Mario-Kart-64', DATA_BRANCH = 'game-data';
function idb() {
  return new Promise((res, rej) => { const r = indexedDB.open('mk64-files', 1); r.onupgradeneeded = () => r.result.createObjectStore('f'); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); });
}
async function cacheGet(k) { try { const db = await idb(); return await new Promise(res => { const q = db.transaction('f').objectStore('f').get(k); q.onsuccess = () => res(q.result || null); q.onerror = () => res(null); }); } catch { return null; } }
async function cachePut(k, v) { try { const db = await idb(); await new Promise(res => { const t = db.transaction('f', 'readwrite'); t.objectStore('f').put(v, k); t.oncomplete = res; t.onerror = res; }); } catch {} }
async function cacheClear() { try { const db = await idb(); db.transaction('f', 'readwrite').objectStore('f').clear(); } catch {} }

async function fetchLocal(name) {
  try { const r = await fetch(name, { cache: 'no-store' }); return r.ok ? new Uint8Array(await r.arrayBuffer()) : null; } catch { return null; }
}
async function fetchFromRepo(token, name) {
  const r = await fetch(`https://api.github.com/repos/${REPO}/contents/${name}?ref=${DATA_BRANCH}`,
    { headers: { Authorization: `Bearer ${token}`, Accept: 'application/vnd.github.raw' }, mode: 'cors', cache: 'no-store' });
  if (r.status === 401 || r.status === 403) throw new Error('GitHub didn\u2019t accept that key. Check that it can read ZZ-games/Mario-Kart-64.');
  if (!r.ok) throw new Error(`Couldn\u2019t download ${name} from GitHub (error ${r.status}).`);
  return new Uint8Array(await r.arrayBuffer());
}
const looksLikeRom = b => b && b.length > 0x1000 && b[0] === 0x80 && b[1] === 0x37;
const looksLikeWasm = b => b && b[0] === 0 && b[1] === 0x61 && b[2] === 0x73 && b[3] === 0x6d;

async function loadGameFiles() {
  let rom = await cacheGet('rom'), wasm = await cacheGet('wasm');
  if (looksLikeRom(rom) && looksLikeWasm(wasm)) return { rom, wasm };
  rom = await fetchLocal('mk64.z64'); wasm = await fetchLocal('mk64.wasm');
  if (looksLikeRom(rom) && looksLikeWasm(wasm)) return { rom, wasm };
  return await setup();
}

// one-time setup: download from the private repo with a read-only key, or pick the two files
function setup() {
  status('');
  const box = $('setup'); box.hidden = false;
  return new Promise(resolve => {
    const done = async (rom, wasm) => {
      if (!looksLikeRom(rom)) throw new Error('That isn\u2019t a Mario Kart 64 .z64 ROM.');
      if (!looksLikeWasm(wasm)) throw new Error('That isn\u2019t the mk64.wasm game file.');
      $('setupMsg').textContent = 'Saving on this device…';
      await cachePut('rom', rom); await cachePut('wasm', wasm);
      box.hidden = true; resolve({ rom, wasm });
    };
    $('ghGo').onclick = async () => {
      const token = $('ghToken').value.trim();
      if (!token) { $('setupMsg').textContent = 'Paste your access key first.'; return; }
      try {
        $('setupMsg').textContent = 'Downloading the game files (about 22 MB)…';
        const [rom, wasm] = await Promise.all([fetchFromRepo(token, 'mk64.z64'), fetchFromRepo(token, 'mk64.wasm')]);
        await done(rom, wasm);
      } catch (e) { $('setupMsg').textContent = e.message; }
    };
    $('pickFiles').onchange = async e => {
      let rom = null, wasm = null;
      for (const f of e.target.files) { const b = new Uint8Array(await f.arrayBuffer()); if (looksLikeRom(b)) rom = b; else if (looksLikeWasm(b)) wasm = b; }
      try { await done(rom, wasm); } catch (err) { $('setupMsg').textContent = err.message + ' Pick both mk64.z64 and mk64.wasm.'; }
    };
  });
}

async function start() {
  if (!crossOriginIsolated) throw new Error('This browser blocked the threads the game needs. Reload the page once; if it still fails, use Safari or Chrome.');
  status('Loading…');
  const { rom, wasm } = await loadGameFiles();
  status('Starting…');
  const module = await WebAssembly.compile(wasm);
  const memory = new WebAssembly.Memory({ initial: 2048, maximum: 2048, shared: true });
  const spawn = slot => { const w = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' }); w.postMessage({ module, memory, slot }); };
  const inst = await WebAssembly.instantiate(module, { env: {
    memory, js_log: (m, a, b) => console.log('[mk64]', m, a, b), js_fatal: (m, a, b) => status('The game stopped.', true),
    js_spawn: () => {}, js_gfx_task: () => {}, js_audio_task: () => {} } });
  const E = inst.exports; window.__mk = E;  // exposed for debugging
  new Uint8Array(memory.buffer, E.rt_rom_ptr(), rom.length).set(rom);
  E.rt_boot();
  for (let s = 0; s < 16; s++) spawn(s);  // one worker per N64 thread slot; each waits for its turn

  const R = new Renderer($('screen'));
  const MAXV = 240000, MAXB = 6000;
  const vbuf = [0, 1].map(i => new Float32Array(memory.buffer, E.rt_gfx_verts(i), MAXV * 12));
  const bbuf = [0, 1].map(i => new Int32Array(memory.buffer, E.rt_gfx_batches(i), MAXB * 32));
  const texInfo = new Int32Array(memory.buffer, E.rt_tex_info(), 8192 * 3);
  const texPool = new Uint32Array(memory.buffer, E.rt_tex_pool(), 6 * 1024 * 1024);
  const texGen = new Int32Array(memory.buffer, E.tex_gen.value, 1);
  const pads = new Uint16Array(memory.buffer, E.pad_buttons.value, 4);
  const sticks = [new Int8Array(memory.buffer, E.pad_x.value, 4), new Int8Array(memory.buffer, E.pad_y.value, 4)];
  const copy = new Float32Array(MAXV * 12);
  status('');
  document.body.classList.add('running');

  let acc = 0, last = performance.now(), lastFrame = -1, fps = 0, fpsT = last;
  const VI = 1000 / 60;
  function loop(t) {
    requestAnimationFrame(loop);
    acc += Math.min(t - last, 100); last = t;
    let n = 0;
    while (acc >= VI && n < 4) { readInput(pads, sticks); E.rt_vblank(); acc -= VI; n++; }
    const frame = E.rt_gfx_info(1);
    if (frame !== lastFrame) {
      const k = E.rt_gfx_info(0);
      if (k >= 0) {
        const nv = E.rt_gfx_info(2), nb = E.rt_gfx_info(3);
        copy.set(vbuf[k].subarray(0, nv * 12));
        R.draw(copy, nv, bbuf[k], nb, texGen[0], texInfo, texPool);
        fps++;
      }
      lastFrame = frame;
    }
    if (t - fpsT > 1000) { $('fps').textContent = fps + ' fps'; fps = 0; fpsT = t; }
  }
  requestAnimationFrame(loop);
}

// ---- input: keyboard + gamepads ----
const BTN = { A: 0x8000, B: 0x4000, Z: 0x2000, START: 0x1000, DU: 0x0800, DD: 0x0400, DL: 0x0200, DR: 0x0100, L: 0x0020, R: 0x0010, CU: 0x0008, CD: 0x0004, CL: 0x0002, CR: 0x0001 };
const KEYS = { KeyX: 'A', KeyC: 'B', KeyZ: 'Z', Enter: 'START', KeyQ: 'L', Space: 'R', ShiftLeft: 'R', KeyI: 'CU', KeyK: 'CD', KeyJ: 'CL', KeyL: 'CR' };
const held = new Set();
addEventListener('keydown', e => {
  if (e.code === 'KeyF') { toggleFull(); return; }
  if (e.code in KEYS || e.code.startsWith('Arrow')) { held.add(e.code); e.preventDefault(); }
});
addEventListener('keyup', e => { held.delete(e.code); });
addEventListener('blur', () => held.clear());
function readInput(pads, sticks) {
  let b = 0, x = 0, y = 0;
  for (const k of held) { if (KEYS[k]) b |= BTN[KEYS[k]]; }
  if (held.has('ArrowLeft')) x -= 80; if (held.has('ArrowRight')) x += 80;
  if (held.has('ArrowUp')) y += 80; if (held.has('ArrowDown')) y -= 80;
  b |= touch.buttons; if (touch.active) { x = touch.x; y = touch.y; }
  pads[0] = b; sticks[0][0] = x; sticks[1][0] = y;
  const gps = navigator.getGamepads ? navigator.getGamepads() : [];
  for (let i = 0, p = 0; i < gps.length && p < 4; i++) {
    const g = gps[i]; if (!g) continue;
    const pr = j => g.buttons[j] && g.buttons[j].pressed;
    let v = 0;
    if (pr(0)) v |= BTN.A; if (pr(2) || pr(1)) v |= BTN.B; if (pr(6)) v |= BTN.Z; if (pr(9)) v |= BTN.START;
    if (pr(4)) v |= BTN.L; if (pr(5) || pr(7)) v |= BTN.R; if (pr(3)) v |= BTN.CL;
    if (pr(12)) v |= BTN.DU; if (pr(13)) v |= BTN.DD; if (pr(14)) v |= BTN.DL; if (pr(15)) v |= BTN.DR;
    const ax = g.axes[0] || 0, ay = g.axes[1] || 0, cx = g.axes[2] || 0, cy = g.axes[3] || 0;
    if (cy < -0.5) v |= BTN.CU; if (cy > 0.5) v |= BTN.CD; if (cx < -0.5) v |= BTN.CL; if (cx > 0.5) v |= BTN.CR;
    const sx = Math.round(ax * 80), sy = Math.round(-ay * 80);
    if (p === 0) { pads[0] |= v; if (Math.abs(sx) > 8) sticks[0][0] = sx; if (Math.abs(sy) > 8) sticks[1][0] = sy; }
    else { pads[p] = v; sticks[0][p] = sx; sticks[1][p] = sy; }
    p++;
  }
}

function toggleFull() {
  const d = document, el = d.documentElement;
  if (d.fullscreenElement || d.webkitFullscreenElement) (d.exitFullscreen || d.webkitExitFullscreen).call(d);
  else { const f = el.requestFullscreen || el.webkitRequestFullscreen; if (f) Promise.resolve(f.call(el)).catch(() => {}); }
}

// ---- touch controls (iPad / phones) ----
const touch = { buttons: 0, x: 0, y: 0, active: false };
(function setupTouch() {
  const stick = $('stick'), knob = $('knob');
  let sid = null, cx = 0, cy = 0;
  const R = 60;
  const move = (px, py) => {
    let dx = px - cx, dy = py - cy; const d = Math.hypot(dx, dy);
    if (d > R) { dx *= R / d; dy *= R / d; }
    knob.style.transform = `translate(${dx}px, ${dy}px)`;
    touch.x = Math.round(dx / R * 80); touch.y = Math.round(-dy / R * 80); touch.active = true;
  };
  stick.addEventListener('pointerdown', e => { sid = e.pointerId; stick.setPointerCapture(sid); const r = stick.getBoundingClientRect(); cx = r.left + r.width / 2; cy = r.top + r.height / 2; move(e.clientX, e.clientY); e.preventDefault(); });
  stick.addEventListener('pointermove', e => { if (e.pointerId === sid) move(e.clientX, e.clientY); });
  const end = e => { if (e.pointerId !== sid) return; sid = null; knob.style.transform = ''; touch.x = touch.y = 0; touch.active = false; };
  stick.addEventListener('pointerup', end); stick.addEventListener('pointercancel', end);
  for (const el of document.querySelectorAll('[data-btn]')) {
    const bit = BTN[el.dataset.btn];
    const on = e => { touch.buttons |= bit; el.classList.add('down'); e.preventDefault(); };
    const off = () => { touch.buttons &= ~bit; el.classList.remove('down'); };
    el.addEventListener('pointerdown', on); el.addEventListener('pointerup', off); el.addEventListener('pointercancel', off); el.addEventListener('pointerleave', off);
  }
})();
$('screen').addEventListener('dblclick', toggleFull);
// go fullscreen on the first click or key press (browsers only allow it after one)
let autoFull = true;
const firstGesture = e => { if (autoFull && !document.fullscreenElement && !document.webkitFullscreenElement && !(e.target.closest && e.target.closest('#setup, .touch'))) toggleFull(); autoFull = false; };
addEventListener('pointerdown', firstGesture, { once: true });
addEventListener('keydown', e => { if (e.code !== 'KeyF') firstGesture(); }, { once: true });
$('full').addEventListener('click', toggleFull);
$('forget').addEventListener('click', async () => { await cacheClear(); location.reload(); });
start().catch(e => status(e.message, true));
