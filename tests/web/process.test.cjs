const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { runOwned } = require('./process.cjs');

const descendant = `
process.on('SIGTERM', () => {});
require('node:fs').writeFileSync(process.argv[1], String(process.pid));
process.stdout.write('ready');
setInterval(() => {}, 1000);
`;
const leader = `
const fs = require('node:fs');
fs.writeFileSync(process.argv[1], String(process.pid));
process.on('SIGTERM', () => {});
const child = require('node:child_process').spawn(process.execPath,
    ['-e', ${JSON.stringify(descendant)}, process.argv[2]], {stdio: ['ignore', 'pipe', 'inherit']});
child.stdout.once('data', () => {
    process.stdout.write('ready');
    if (process.argv[3] === 'exit') process.exit(0);
});
`;

for (const mode of ['wait', 'exit']) {
    test(`owned group stops TERM-ignoring descendants after ${mode}`, async () => {
        const work = fs.mkdtempSync(path.join(os.tmpdir(), 'chibi-web-group-'));
        const parent = path.join(work, 'parent');
        const child = path.join(work, 'child');
        let watchdog;
        let complete = false;
        const running = runOwned(process.execPath, ['-e', leader, parent, child, mode], {
            timeout: 1000,
        });
        try {
            const result = await Promise.race([
                running,
                new Promise((_, reject) => {
                    watchdog = setTimeout(
                        () => reject(new Error('owned group did not close')),
                        4000,
                    );
                }),
            ]);
            assert.equal(result.stdout, 'ready');
            assert.equal(result.timedOut, mode === 'wait');
            if (mode === 'wait') assert.equal(result.signal, 'SIGKILL');
            else assert.equal(result.code, 0);
            const pid = fs.readFileSync(child, 'utf8');
            const status = await runOwned('ps', ['-p', pid, '-o', 'stat=']);
            // An exited orphan may await init's reap; it must not be running.
            assert.ok(status.code === 1 || status.stdout.trim().startsWith('Z'), status.stdout);
            complete = true;
        } finally {
            clearTimeout(watchdog);
            if (!complete) {
                // Keep the negative control bounded even if group cleanup breaks.
                for (const file of [parent, child]) {
                    if (!fs.existsSync(file)) continue;
                    const pid = Number(fs.readFileSync(file, 'utf8'));
                    // Only the leader owns a group; the descendant owns a PID.
                    for (const target of file === parent ? [-pid, pid] : [pid]) {
                        try {
                            process.kill(target, 'SIGKILL');
                        } catch (error) {
                            if (error.code !== 'ESRCH') throw error;
                        }
                    }
                }
                await running;
            }
            fs.rmSync(work, { recursive: true, force: true });
        }
    });
}
