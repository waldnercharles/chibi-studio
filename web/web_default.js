// Prefer the paired worker build when available; a standalone serial build
// remains usable. The explicit serial URL also breaks isolation retry loops.
(async () => {
    const current = new URL(window.location.href);
    const start = () => {
        const script = document.createElement('script');
        script.src = 'chibi-studio.js';
        script.async = true;
        document.body.append(script);
    };
    if (current.searchParams.has('serial') || typeof Worker === 'undefined' ||
        (!window.crossOriginIsolated && (!window.isSecureContext || !navigator.serviceWorker))) {
        start();
        return;
    }
    const threaded = new URL('threads/' + (current.pathname.split('/').pop() || 'index.html'), current);
    threaded.search = current.search;
    threaded.hash = current.hash;
    threaded.searchParams.set('serial-url', current.href);
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 2000);
    try {
        const response = await fetch(threaded, { method: 'HEAD', signal: controller.signal });
        if (response.ok) {
            window.location.replace(threaded.href);
            return;
        }
    } catch (_) {
        // Offline, unsupported, or unpaired builds use the serial entry.
    } finally {
        clearTimeout(timeout);
    }
    start();
})();
