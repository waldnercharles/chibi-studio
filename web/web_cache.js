// Editor caches persist in IndexedDB, as projects do: /thumbnails holds Studio's
// rendered thumbnails (src/editor/studio/studio_thumbnail_cache.c). They load
// before main, and each change is written back a moment after the last.
if (typeof window !== 'undefined') {
    const cache = '/thumbnails';
    const fs = () => Module['FS'];
    let loaded = false;
    let timer;
    Module['chibiPersistCache'] = () => {
        if (!loaded) return;
        clearTimeout(timer);
        timer = setTimeout(() => fs().syncfs(false, () => {}), 1000);
    };
    (Module['preRun'] ||= []).push(() => {
        fs().mkdirTree(cache);
        fs().mount(fs().filesystems.IDBFS, {}, cache);
        addRunDependency('chibi-cache');
        fs().syncfs(true, error => {
            // Without IndexedDB, as in a private window, pictures last the session.
            loaded = !error;
            removeRunDependency('chibi-cache');
        });
    });
}
