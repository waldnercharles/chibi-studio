// Restore disposable compiled assets into staging: a late IndexedDB response
// must never overwrite records written by the running compiler after fallback.
if (typeof window !== 'undefined') {
    const cache = '/cache/chibi-assets';
    const storage = '/cache/chibi-assets-storage';
    const fs = () => Module['FS'];
    const managed = name => /^[A-Za-z0-9_-]{1,160}\.bin$/.test(name);
    const records = root => {
        const names = fs().readdir(root);
        if (names.length > 1040)
            throw new Error('Cache directory exceeds scan limit');
        const entries = [];
        let bytes = 0;
        for (const name of names) {
            if (!managed(name)) continue;
            const info = fs().lstat(`${root}/${name}`);
            if (!fs().isFile(info.mode) || !Number.isSafeInteger(info.size) ||
                info.size < 0)
                throw new Error('Invalid cache record');
            bytes += info.size;
            if (entries.length == 1024 || bytes > 256 * 1024 * 1024)
                throw new Error('Cache records exceed storage limit');
            entries.push(name);
        }
        return entries;
    };
    const copy = (source, target) => {
        const incoming = records(source);
        const previous = records(target);
        for (const name of previous) fs().unlink(`${target}/${name}`);
        for (const name of incoming)
            fs().writeFile(`${target}/${name}`, fs().readFile(`${source}/${name}`));
    };
    let mount, idbfs, ready = false, dirty = false, flushing = false, timer;
    const flush = () => {
        if (!ready || flushing || !dirty) return;
        dirty = false;
        flushing = true;
        const finished = error => {
            flushing = false;
            if (error) ready = false;
            else if (dirty) schedule();
        };
        try {
            copy(cache, storage);
            idbfs.syncfs(mount, false, finished);
        } catch (error) {
            finished(error);
        }
    };
    const schedule = () => {
        clearTimeout(timer);
        timer = setTimeout(flush, 500);
    };
    Module['chibiAssetCacheDirty'] = () => {
        if (!ready) return;
        dirty = true;
        schedule();
    };
    (Module['preRun'] ||= []).push(() => {
        addRunDependency('chibi-asset-cache');
        let completed = false;
        const deadline = Date.now() + 5000;
        let timeout;
        const restored = error => {
            if (completed) return;
            completed = true;
            clearTimeout(timeout);
            try {
                if (!error && Date.now() <= deadline) {
                    copy(storage, cache);
                    ready = true;
                }
            } catch {
                // Invalid or oversized records remain disposable misses.
            } finally {
                removeRunDependency('chibi-asset-cache');
            }
        };
        timeout = setTimeout(() => restored(true), 5000);
        try {
            fs().mkdirTree(cache); // Active cache remains MEMFS for the session.
            fs().mkdirTree(storage);
            idbfs = fs().filesystems.IDBFS;
            // Global project/thumbnail syncfs calls do not own this mount.
            const isolated = {
                mount: value => idbfs.mount(value),
                syncfs: (_mount, _populate, done) => done(null),
            };
            mount = fs().mount(isolated, {}, storage).mount;
            idbfs.syncfs(mount, true, restored);
        } catch (error) {
            restored(error);
        }
    });
}
