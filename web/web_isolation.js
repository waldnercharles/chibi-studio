// The threaded bundle can run on static hosts without custom headers.
// Its service worker covers only this bundle's directory and stores no assets.
if (typeof document === 'undefined' && typeof self.skipWaiting !== 'function') {
    // The fast bundle requires both SIMD and shared-memory Wasm support.
    const features = new Uint8Array([
        0, 97, 115, 109, 1, 0, 0, 0,
        1, 4, 1, 96, 0, 0, 3, 2, 1, 0,
        5, 4, 1, 3, 1, 1,
        10, 23, 1, 21, 0, 253, 12,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        26, 11,
    ]);
    const supported = typeof WebAssembly !== 'undefined' && WebAssembly.validate(features);
    self.postMessage(supported ? 'chibi-worker-ready' : 'chibi-worker-unsupported');
} else if (typeof document === 'undefined') {
    self.addEventListener('install', event => event.waitUntil(self.skipWaiting()));
    self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));
    self.addEventListener('fetch', event => {
        if (new URL(event.request.url).origin !== self.location.origin) return;
        if (event.request.cache === 'only-if-cached' && event.request.mode !== 'same-origin') return;
        event.respondWith(fetch(event.request).then(response => {
            if (!response.status) return response;
            const headers = new Headers(response.headers);
            headers.set('Cross-Origin-Opener-Policy', 'same-origin');
            headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
            return new Response(response.body, {
                status: response.status, statusText: response.statusText, headers,
            });
        }));
    });
} else {
    const serial = () => {
        const current = new URL(window.location.href);
        const configured = current.searchParams.get('serial-url');
        if (!configured) {
            Module['showStopped']('This Studio build requires browser workers and cross-origin isolation. Open a serial Studio build to continue.');
            return;
        }
        const target = new URL(configured, current);
        if (target.origin !== current.origin || target.href === current.href) {
            Module['showStopped']('The serial Studio fallback must be another page on this origin.');
            return;
        }
        target.search = current.search;
        target.searchParams.delete('serial-url');
        target.searchParams.set('serial', '1');
        target.hash = current.hash;
        window.location.replace(target.href);
    };
    (async () => {
        if (typeof Worker === 'undefined') return serial();
        if (!window.crossOriginIsolated) {
            if (!window.isSecureContext || !navigator.serviceWorker) return serial();
            const worker = new URL('chibi-isolation.js', window.location.href);
            if (navigator.serviceWorker.controller?.scriptURL === worker.href) return serial();
            let changed;
            const controlled = new Promise(resolve => {
                changed = resolve;
                navigator.serviceWorker.addEventListener('controllerchange', changed);
            });
            let timeout;
            try {
                await Promise.race([
                    (async () => {
                        await navigator.serviceWorker.register(worker.href);
                        await navigator.serviceWorker.ready;
                        if (navigator.serviceWorker.controller?.scriptURL !== worker.href) await controlled;
                    })(),
                    new Promise((_, reject) => {
                        timeout = setTimeout(() => reject(new Error('Isolation timeout')), 4000);
                    }),
                ]);
            } finally {
                clearTimeout(timeout);
                navigator.serviceWorker.removeEventListener('controllerchange', changed);
            }
            window.location.reload();
            return;
        }
        if (typeof SharedArrayBuffer === 'undefined') return serial();
        // Emscripten allocates its pool before installing worker error handlers.
        // Probe the same-origin worker policy first so a blocked pool cannot hang.
        const probe = new Worker(new URL('chibi-isolation.js', window.location.href));
        let probeTimeout;
        try {
            await new Promise((resolve, reject) => {
                probe.onmessage = event => {
                    if (event.data === 'chibi-worker-ready') resolve();
                    else reject(new Error('Worker Wasm features unavailable'));
                };
                probe.onerror = reject;
                probeTimeout = setTimeout(() => reject(new Error('Worker timeout')), 4000);
            });
        } finally {
            clearTimeout(probeTimeout);
            probe.terminate();
        }
        let started = false;
        const abort = Module['onAbort'];
        const failed = () => { if (!started) serial(); };
        window.addEventListener('error', failed);
        window.addEventListener('unhandledrejection', failed);
        (Module['postRun'] ||= []).push(() => {
            started = true;
            window.removeEventListener('error', failed);
            window.removeEventListener('unhandledrejection', failed);
        });
        Module['onAbort'] = reason => {
            if (!started) return serial();
            abort?.(reason);
        };
        const script = document.createElement('script');
        script.src = 'chibi-studio.js';
        script.async = true;
        script.onerror = serial;
        document.body.append(script);
    })().catch(serial);
}
