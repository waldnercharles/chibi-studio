const { spawn } = require('node:child_process');
const { setTimeout: delay } = require('node:timers/promises');

// The former C fixture owned a process group, including descendants left by a
// completed leader. Drain its pipes only after final group cleanup.
async function runOwned(program, args, { cwd, timeout = 10000 } = {}) {
    if (process.platform === 'win32') {
        throw new Error('Browser storage process fixtures require POSIX process groups');
    }
    const child = spawn(program, args, { cwd, detached: true, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '',
        stderr = '';
    child.stdout.setEncoding('utf8').on('data', (data) => {
        stdout += data;
    });
    child.stderr.setEncoding('utf8').on('data', (data) => {
        stderr += data;
    });
    const exited = new Promise((resolve) => {
        child.once('error', (error) => resolve({ error }));
        child.once('exit', (code, signal) => resolve({ code, signal }));
    });
    const closed = new Promise((resolve) => child.once('close', resolve));
    const signalGroup = (signal) => {
        if (!child.pid) return;
        try {
            process.kill(-child.pid, signal);
        } catch (error) {
            if (error.code !== 'ESRCH') throw error;
        }
    };
    const abort = (signal) => {
        signalGroup('SIGKILL');
        process.removeListener('SIGINT', interrupt);
        process.removeListener('SIGTERM', terminate);
        process.kill(process.pid, signal);
    };
    const interrupt = () => abort('SIGINT');
    const terminate = () => abort('SIGTERM');
    process.once('SIGINT', interrupt);
    process.once('SIGTERM', terminate);
    let deadline;
    let timedOut = false;
    try {
        const first = await Promise.race([
            exited,
            new Promise((resolve) => {
                deadline = setTimeout(() => resolve({ timedOut: true }), timeout);
            }),
        ]);
        timedOut = first.timedOut === true;
        if (timedOut) {
            signalGroup('SIGTERM');
            await Promise.race([exited, delay(250)]);
        }
    } finally {
        clearTimeout(deadline);
        signalGroup('SIGKILL');
        process.removeListener('SIGINT', interrupt);
        process.removeListener('SIGTERM', terminate);
    }
    const result = await exited;
    await closed;
    return { ...result, timedOut, stdout, stderr };
}

module.exports = { runOwned };
