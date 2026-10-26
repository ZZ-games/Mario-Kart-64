#!/bin/sh
cd "$(dirname "$0")/.." && mkdir -p dist && cp web/index.html web/main.js web/worker.js web/gl.js web/sw.js web/manifest.webmanifest dist/ && cp build/mk64.wasm dist/
