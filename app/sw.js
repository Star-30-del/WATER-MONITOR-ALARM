/* Service worker — app shell caching for the Water Monitor PWA.
   - HTML/navigation: network-first (so a new deploy shows up immediately,
     falls back to cache when offline).
   - Static assets (mqtt lib, icons, manifest): cache-first with runtime cache.
   - Device API calls (/api/*) are never cached. */
var CACHE = 'water-monitor-v2';
var SHELL = [
  './',
  './index.html',
  './manifest.webmanifest',
  './vendor/mqtt.min.js',
  './icons/icon-192.png',
  './icons/icon-512.png'
];

self.addEventListener('install', function(e){
  e.waitUntil(caches.open(CACHE).then(function(c){ return c.addAll(SHELL); }).then(function(){ return self.skipWaiting(); }));
});

self.addEventListener('activate', function(e){
  e.waitUntil(
    caches.keys().then(function(keys){
      return Promise.all(keys.filter(function(k){ return k!==CACHE; }).map(function(k){ return caches.delete(k); }));
    }).then(function(){ return self.clients.claim(); })
  );
});

function putCache(req, res){
  if(res && res.ok && new URL(req.url).origin === self.location.origin){
    var copy = res.clone();
    caches.open(CACHE).then(function(c){ c.put(req, copy); });
  }
  return res;
}

self.addEventListener('fetch', function(e){
  var req = e.request;
  if(req.method !== 'GET') return;
  var url = new URL(req.url);
  if(url.pathname.indexOf('/api/') !== -1) return;   // device API — always network

  var isDoc = req.mode === 'navigate' || url.pathname.endsWith('.html') || url.pathname.endsWith('/');
  if(isDoc){
    e.respondWith(
      fetch(req).then(function(res){ return putCache(req, res); })
                .catch(function(){ return caches.match(req).then(function(h){ return h || caches.match('./index.html'); }); })
    );
    return;
  }
  // static assets: cache-first, populate on miss
  e.respondWith(
    caches.match(req).then(function(hit){
      return hit || fetch(req).then(function(res){ return putCache(req, res); });
    })
  );
});
