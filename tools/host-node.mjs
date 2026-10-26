// Headless test host (Node): boots the recompiled game with one worker per N64 thread.
import { Worker, isMainThread, workerData } from 'worker_threads';
import fs from 'fs';

const WASM = new URL('../build/mk64.wasm', import.meta.url);

function makeImports(memory, ctx) {
  const cstr = p => { const b = new Uint8Array(memory.buffer); let s = ''; while (b[p]) s += String.fromCharCode(b[p++]); return s; };
  return {
    env: {
      memory,
      js_log: (m, a, b) => console.log(`[${ctx.name}] ${cstr(m)} ${a.toString(16)} ${b.toString(16)}`),
      js_fatal: (m, a, b) => { console.log(`[${ctx.name}] FATAL ${cstr(m)} ${(a >>> 0).toString(16)} ${(b >>> 0).toString(16)}`); process.exit(2); },
      js_spawn: slot => ctx.spawn(slot),
      js_gfx_task: task => {},
      js_audio_task: task => {},
    },
  };
}

function spawnWorker(module, memory, slot) {
  const w = new Worker(new URL(import.meta.url), { workerData: { module, memory, slot } });
  w.on('error', e => { console.log('worker', slot, 'error', e); process.exit(3); });
}

if (isMainThread) {
  const memory = new WebAssembly.Memory({ initial: 2048, maximum: 4096, shared: true });
  const module = await WebAssembly.compile(fs.readFileSync(WASM));
  const ctx = { name: 'main', spawn: s => spawnWorker(module, memory, s) };
  const inst = await WebAssembly.instantiate(module, makeImports(memory, ctx));
  ctx.exports = inst.exports;
  const E = inst.exports;
  const rom = fs.readFileSync(new URL('../rom/baserom.us.z64', import.meta.url));
  new Uint8Array(memory.buffer, E.rt_rom_ptr(), rom.length).set(rom);
  E.rt_boot();
  spawnWorker(module, memory, 0);
  const t0 = Date.now();
  let ticks = 0;
  const seconds = +(process.argv[2] || 8);
  const iv = setInterval(() => {
    E.rt_vblank(); ticks++;
    if (ticks % 60 === 0) {
      const st = i => E.rt_stat(i);
      let threads = [];
      for (let s = 0; s < st(5); s++) threads.push(`${s}:p${E.rt_slot_info(s, 1)}/st${E.rt_slot_info(s, 2)}`);
      console.log(`t=${ticks / 60}s gfx=${st(0)} audio=${st(1)} vi=${st(2)} fb=${(st(3) >>> 0).toString(16)} running=${st(4) | 0} threads ${threads.join(' ')}`);
    }
    if (ticks >= seconds * 60) { clearInterval(iv); process.exit(0); }
  }, 1000 / 60);
} else {
  const { module, memory, slot } = workerData;
  const ctx = { name: 'T' + slot, spawn: s => spawnWorker(module, memory, s) };
  const inst = await WebAssembly.instantiate(module, makeImports(memory, ctx));
  ctx.exports = inst.exports;
  inst.exports.__stack_pointer.value = inst.exports.rt_stack_top(slot);
  inst.exports.rt_thread_main(slot);
}
