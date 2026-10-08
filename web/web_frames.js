// The game's frame log, for finding and comparing hitches in the browser:
// Module.chibiFrames() lists the recent frames, Module.chibiFrameSummary()
// reduces them to percentiles, and Module.chibiDownloadFrames() saves a CSV.
(() => {
    if (typeof window === 'undefined') return;

    const csv = () => {
        const pointer = Module['_chibi_frame_log_csv']();
        const text = Module['UTF8ToString'](pointer);
        Module['_free'](pointer);
        return text;
    };
    const frames = () => {
        const [header, ...rows] = csv().trim().split('\n');
        const names = header.split(',');
        return rows.map(row => Object.fromEntries(row.split(',').map((value, i) => [names[i], Number(value)])));
    };
    // Nearest-rank percentiles of each timing across the logged frames.
    const summary = (list = frames()) => {
        const result = { frames: list.length };
        for (const name of [
            'interval_ms', 'steps', 'dropped_ms', 'behavior_ms', 'physics_ms', 'systems_ms', 'rendering_ms',
        ]) {
            const sorted = list.map(frame => frame[name]).sort((a, b) => a - b);
            const rank = p => sorted[Math.max(0, Math.ceil(p / 100 * sorted.length) - 1)];
            result[name] = sorted.length
                ? { p50: rank(50), p95: rank(95), p99: rank(99), max: sorted[sorted.length - 1] }
                : { p50: null, p95: null, p99: null, max: null };
        }
        const stepCounts = new Map();
        for (const frame of list) {
            stepCounts.set(frame.steps, (stepCounts.get(frame.steps) || 0) + 1);
        }
        result.step_counts = Object.fromEntries([...stepCounts].sort(([a], [b]) => a - b));
        result.catch_up_frames = list.filter(frame => frame.steps > 1).length;
        result.dropped_frames = list.filter(frame => frame.dropped_ms > 0).length;
        result.dropped_ms_total = list.reduce((total, frame) => total + frame.dropped_ms, 0);
        return result;
    };
    const download = () => {
        const link = document.createElement('a');
        link.href = URL.createObjectURL(new Blob([csv()], { type: 'text/csv' }));
        link.download = 'chibi-frames.csv';
        link.click();
        // Revoking right away can cancel the download in some browsers.
        setTimeout(() => URL.revokeObjectURL(link.href), 40000);
    };
    Module['chibiFrames'] = frames;
    Module['chibiFrameSummary'] = summary;
    Module['chibiDownloadFrames'] = download;
})();
