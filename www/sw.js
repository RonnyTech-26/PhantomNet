/* Phantom Net - service worker
   Makes the dashboard installable as a real mobile app, keeps it working
   offline, and lets alerts reach the phone's notification shade / lock
   screen while the app is in the background.                                */

const CACHE = 'phantomnet-v1';
const FILES = [
  './',
  './PhantomNet_Dashboard.html',
  './manifest.webmanifest',
  './icon-192.png',
  './icon-512.png'
];

self.addEventListener('install', e => {
  self.skipWaiting();
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(FILES).catch(() => {})));
});

self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys().then(ks => Promise.all(ks.filter(k => k !== CACHE)
                                           .map(k => caches.delete(k))))
          .then(() => self.clients.claim())
  );
});

/* network first, cache as the fallback - the log data is always live */
self.addEventListener('fetch', e => {
  if(e.request.method !== 'GET') return;
  e.respondWith(
    fetch(e.request)
      .then(r => {
        const copy = r.clone();
        caches.open(CACHE).then(c => c.put(e.request, copy)).catch(() => {});
        return r;
      })
      .catch(() => caches.match(e.request).then(r => r || caches.match('./PhantomNet_Dashboard.html')))
  );
});

/* tapping the alert notification brings the dashboard to the front */
self.addEventListener('notificationclick', e => {
  e.notification.close();
  e.waitUntil(
    self.clients.matchAll({type:'window', includeUncontrolled:true}).then(cs => {
      for(const c of cs){ if('focus' in c) return c.focus(); }
      if(self.clients.openWindow) return self.clients.openWindow('./PhantomNet_Dashboard.html');
    })
  );
});

/* if a push service is ever wired up, the payload rings the same alert */
self.addEventListener('push', e => {
  let d = {};
  try{ d = e.data ? e.data.json() : {}; }catch(err){ d = {}; }
  e.waitUntil(self.registration.showNotification('🚨 ' + (d.title || 'MAN DOWN'), {
    body: d.body || 'Phantom Net node needs help.',
    tag: 'phantomnet-alert', renotify: true, requireInteraction: true,
    vibrate: [700,300,700,300,700,900],
    icon: './icon-192.png', badge: './icon-192.png'
  }));
});
