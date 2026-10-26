// Runs one N64 thread of the recompiled game.
const bc = new BroadcastChannel('mk64');
function imports(memory, getExports) {
  const cstr = p => { const b = new Uint8Array(memory.buffer); let s = ''; while (b[p]) s += String.fromCharCode(b[p++]); return s; };
  return {
    env: {
      memory,
      js_log: (m, a, b) => console.log(`[mk64] ${cstr(m)} ${(a >>> 0).toString(16)} ${(b >>> 0).toString(16)}`),
      js_fatal: (m, a, b) => { const msg = `${cstr(m)} ${(a >>> 0).toString(16)} ${(b >>> 0).toString(16)}`; console.error('[mk64] FATAL', msg); bc.postMessage({ fatal: msg }); },
      js_spawn: () => {},
      js_gfx_task: () => {}, js_audio_task: () => {},
    },
  };
}
let mod, mem;
function spawn(slot) {
  const w = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' });
  w.postMessage({ module: mod, memory: mem, slot });
}
self.onmessage = async e => {
  const { module, memory, slot } = e.data;
  mod = module; mem = memory;
  const inst = await WebAssembly.instantiate(module, imports(memory));
  inst.exports.__stack_pointer.value = inst.exports.rt_stack_top(slot);
  try { inst.exports.rt_thread_main(slot); }
  catch (err) { bc.postMessage({ fatal: `thread ${slot}: ${err.message}` }); throw err; }
};
