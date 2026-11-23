// Limit offline storage to the public app files in this installation.
const prefix = 'quack-pocket:' + self.registration.scope + ':';
const cacheName = prefix + 'updates-0.0.31-tabs';
const assets = ['index.html', 'manifest.webmanifest', ...[180, 192, 512].map(size =>
  'icons/quack-pocket-' + size + '.png')].map(path => new URL(path, self.registration.scope).href);

self.addEventListener('install', event => {
  event.waitUntil(caches.open(cacheName).then(cache => cache.addAll(assets)));
});

self.addEventListener('activate', event => {
  event.waitUntil((async () => {
    for (const key of await caches.keys()) {
      if (key.startsWith(prefix) && key !== cacheName) await caches.delete(key);
    }
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', event => {
  const request = event.request;
  if (request.method !== 'GET') return;
  const url = new URL(request.url);
  url.search = '';
  const shell = request.mode === 'navigate' &&
    (url.href === self.registration.scope || url.href === assets[0]);
  if (!shell && !assets.includes(url.href)) return;
  event.respondWith((async () => {
    const cache = await caches.open(cacheName);
    const key = shell ? assets[0] : url.href;
    try {
      const response = await fetch(request);
      if (response.ok && !response.redirected) await cache.put(key, response.clone());
      return response;
    } catch {
      return (await cache.match(key)) || Response.error();
    }
  })());
});
