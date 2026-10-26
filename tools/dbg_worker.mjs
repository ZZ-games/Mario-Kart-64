import { workerData } from 'worker_threads';
const { module, memory, slot } = workerData;
const env = { memory, js_log: (m, a, b) => console.log('log', slot, (a>>>0).toString(16), (b>>>0).toString(16)), js_fatal: (m, a, b) => { console.log('FATAL', slot, (a >>> 0).toString(16), (b >>> 0).toString(16)); process.exit(2); }, js_spawn: () => {}, js_gfx_task: () => {}, js_audio_task: () => {} };
const I = await WebAssembly.instantiate(module, { env });
I.exports.__stack_pointer.value = I.exports.rt_stack_top(slot);
I.exports.rt_thread_main(slot);
