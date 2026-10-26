#!/bin/sh
# Build the recompiled game + runtime into build/mk64.wasm (shared memory, threads via workers).
set -e
cd "$(dirname "$0")/.."
OPT=${OPT:--O2}
CFLAGS="--target=wasm32 $OPT $EXTRA -matomics -mbulk-memory -mnontrapping-fptoint -nostdlib -ffreestanding -fno-strict-overflow -Iruntime -Ibuild/gen -w"
mkdir -p build/obj
pids=""
for f in build/gen/*.c runtime/*.c; do
  o=build/obj/$(basename "$f" .c).o
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ runtime/rt.h -nt "$o" ]; then
    clang $CFLAGS -c "$f" -o "$o" & pids="$pids $!"
  fi
done
for p in $pids; do wait $p; done
EXPORTS=""
for e in rt_boot rt_thread_main rt_vblank rt_task_done rt_stat rt_slot_info rt_rom_ptr rt_rdram_ptr rt_stack_top \
         rt_gfx_verts rt_gfx_batches rt_gfx_info rt_tex_info rt_tex_pool tex_gen gfx_unknown_ops rt_gfx_unknown rt_cur_fn rt_depth rt_stk pad_buttons pad_x pad_y pad_mask eeprom eeprom_dirty vi_cur_fb rom_size __stack_pointer; do EXPORTS="$EXPORTS -Wl,--export=$e"; done
clang $CFLAGS build/obj/*.o -o build/mk64.wasm -Wl,--no-entry -Wl,--import-memory -Wl,--shared-memory \
  -Wl,--max-memory=134217728 -Wl,--initial-memory=134217728 -Wl,--allow-undefined $EXPORTS
ls -la build/mk64.wasm
