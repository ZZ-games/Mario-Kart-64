// Adds the cross-origin isolation headers (COOP/COEP) that GitHub Pages can't set,
// so the game's threads (SharedArrayBuffer) are allowed. Same-origin files only.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.cache === 'only-if-cached' && req.mode !== 'same-origin') return;
  if (new URL(req.url).origin !== self.location.origin) return;  // e.g. api.github.com: pass through
  e.respondWith(fetch(req).then(res => {
    if (res.status === 0) return res;
    const h = new Headers(res.headers);
    h.set('Cross-Origin-Opener-Policy', 'same-origin');
    h.set('Cross-Origin-Embedder-Policy', 'require-corp');
    h.set('Cross-Origin-Resource-Policy', 'same-origin');
    return new Response(res.body, { status: res.status, statusText: res.statusText, headers: h });
  }).catch(err => new Response('Offline: ' + err.message, { status: 503 })));
});
