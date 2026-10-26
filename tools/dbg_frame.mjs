// Run the game headless for N seconds, then dump draw batches and textures of the latest frame.
import { Worker } from 'worker_threads';
import fs from 'fs'; import zlib from 'zlib';
const secs = +(process.argv[2] || 3), out = process.argv[3] || '/tmp';
const memory = new WebAssembly.Memory({ initial: 2048, maximum: 4096, shared: true });
const module = await WebAssembly.compile(fs.readFileSync(new URL('../build/mk64.wasm', import.meta.url)));
const env = { memory, js_log: (m, a, b) => {}, js_fatal: (m, a, b) => { console.log('FATAL', a.toString(16), b.toString(16)); process.exit(2); }, js_spawn: () => {}, js_gfx_task: () => {}, js_audio_task: () => {} };
const E = (await WebAssembly.instantiate(module, { env })).exports;
const rom = fs.readFileSync(new URL('../rom/baserom.us.z64', import.meta.url));
new Uint8Array(memory.buffer, E.rt_rom_ptr(), rom.length).set(rom);
E.rt_boot();
for (let s = 0; s < 16; s++) new Worker(new URL('./dbg_worker.mjs', import.meta.url), { workerData: { module, memory, slot: s } });
const pads = new Uint16Array(memory.buffer, E.pad_buttons.value, 4);
let n = 0; const presses = (process.env.PRESS || '').split(',').filter(Boolean).map(Number);
await new Promise(res => { const iv = setInterval(() => { pads[0] = presses.some(t => n >= t * 60 && n < t * 60 + 6) ? 0x1000 : 0; E.rt_vblank(); if (++n >= secs * 60) { clearInterval(iv); res(); } }, 16); });
const k = E.rt_gfx_info(0), nb = E.rt_gfx_info(3), nv = E.rt_gfx_info(2);
const B = new Int32Array(memory.buffer, E.rt_gfx_batches(k), nb * 32);
const V = new Float32Array(memory.buffer, E.rt_gfx_verts(k), nv * 12);
{ const U = new Uint32Array(memory.buffer, E.rt_gfx_unknown(), 256); const l = []; U.forEach((v, i) => { if (v) l.push(i.toString(16) + ':' + v); }); console.log('unknown ops', l.join(' ')); }
{ const st = []; for (let i = 0; i < E.rt_stat(5); i++) st.push(`${i}:pri${E.rt_slot_info(i,1)} state${E.rt_slot_info(i,2)} q${(E.rt_slot_info(i,3)>>>0).toString(16)} entry${(E.rt_slot_info(i,4)>>>0).toString(16)}`); console.log('threads', st.join(' | '), 'gfx', E.rt_stat(0), 'audio', E.rt_stat(1)); }
{ const cf = new Uint32Array(memory.buffer, E.rt_cur_fn.value, 17); const seen = new Map(); for (let i = 0; i < 4000; i++) { const v = 'T5:' + cf[5].toString(16) + ' T4:' + cf[4].toString(16); seen.set(v, (seen.get(v) || 0) + 1); } console.log('cur fn samples', [...seen].sort((a,b)=>b[1]-a[1]).slice(0,12).map(([k, v]) => k + ':' + v).join(' ')); }
if (process.env.SNDDUMP) { const R = new Uint8Array(memory.buffer, E.rt_rdram_ptr(), 0x800000); const rb = a => R[(a & 0x7fffff) ^ 3];
  for (let bk = 0; bk < 6; bk++) { const base = 0x80191420 + bk * 960; const nx = [], pv = [], st = []; for (let i = 0; i < 20; i++) { nx.push(rb(base + i * 48 + 43)); pv.push(rb(base + i * 48 + 42)); st.push(rb(base + i * 48 + 41)); } console.log('bank', bk, 'next', nx.join(','), '| prev', pv.join(','), '| b41', st.join(',')); } }
if (E.rt_depth) { const D = new Int32Array(memory.buffer, E.rt_depth.value, 16), K = new Uint32Array(memory.buffer, E.rt_stk.value, 16 * 128);
  const counts = new Map(); for (let i = 0; i < 300; i++) { const d = Math.min(D[5], 128); const st = []; for (let j = 0; j < d; j++) st.push(K[5 * 128 + j].toString(16)); const key = st.join('>'); counts.set(key, (counts.get(key) || 0) + 1); }
  for (const [k, v] of [...counts].sort((a, b) => b[1] - a[1]).slice(0, 5)) console.log('T5 stack', v, k); }
console.log('frame', E.rt_gfx_info(1), 'batches', nb, 'verts', nv, 'unknown ops', new Uint32Array(memory.buffer, E.gfx_unknown_ops.value, 1)[0]);
const hx = v => (v >>> 0).toString(16).padStart(8, '0');
for (let i = 0; i < Math.min(nb, +(process.argv[4] || 12)); i++) {
  const b = B.subarray(i * 32, i * 32 + 32);
  console.log(`#${i} kind${b[22]} first${b[0]} n${b[1]} tex${b[2]}/${b[3]} fl${b[4]} cmb ${hx(b[6])}:${hx(b[7])} oh ${hx(b[8])} ol ${hx(b[9])} geom ${hx(b[10])} prim ${hx(b[11])} env ${hx(b[12])} vp ${b[14]},${b[15]},${b[16]},${b[17]}`);
  if (b[1]) { const v = V.subarray(b[0] * 12, b[0] * 12 + 12); console.log('    v0', Array.from(v).map(x => x.toFixed(2)).join(' ')); }
}
const info = new Int32Array(memory.buffer, E.rt_tex_info(), 8192 * 3), pool = new Uint8Array(memory.buffer, E.rt_tex_pool(), 24 << 20);
const used = new Set(); for (let i = 0; i < nb; i++) { const t = B[i * 32 + 2]; if (t >= 0) used.add(t); }
for (const id of [...used].slice(0, 6)) {
  const off = info[id * 3], w = info[id * 3 + 1], h = info[id * 3 + 2];
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 4 + 1)] = 0; pool.slice((off + y * w) * 4, (off + y * w + w) * 4).forEach((x, j) => raw[y * (w * 4 + 1) + 1 + j] = x); }
  const crcT = []; for (let q = 0; q < 256; q++) { let c = q; for (let j = 0; j < 8; j++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; crcT[q] = c >>> 0; }
  const crc = b2 => { let c = ~0; for (const x of b2) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (~c) >>> 0; };
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ih = Buffer.alloc(13); ih.writeUInt32BE(w, 0); ih.writeUInt32BE(h, 4); ih[8] = 8; ih[9] = 6;
  fs.writeFileSync(`${out}/tex${id}.png`, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
  console.log('tex', id, w + 'x' + h);
}
process.exit(0);
