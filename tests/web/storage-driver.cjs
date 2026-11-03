// Production browser storage with deterministic browser/FS seams.
const vm = require('node:vm'),
    host = require('node:fs');
const code = host.readFileSync(process.argv[2], 'utf8'),
    mode = process.argv[3];
const saving = mode.startsWith('save-'),
    shell = host
        .readFileSync(process.argv[2].replace(/web_storage\.js$/, 'web_shell.html'), 'utf8')
        .match(/<script>([\s\S]*?)<\/script>/)[1];
const files = new Map(),
    dirs = new Set(['/', '/projects', '/projects/starter']);
files.set('/projects/starter/project.kdl', new TextEncoder().encode('name Starter'));
let held = false,
    active,
    maxSyncs = 0;
const durable = new Map(),
    syncs = [];
const finish = (job, error) => {
    if (!error) {
        durable.clear();
        for (const [p, data] of job.files) durable.set(p, data);
    }
    job.done(error);
};
const FS = {
    filesystems: { IDBFS: {} },
    mount() {},
    syncfs(populate, done) {
        if (populate) {
            queueMicrotask(() => done(null));
            return;
        }
        const job = { done, files: new Map(files) };
        if (held) {
            syncs.push(job);
            maxSyncs = Math.max(maxSyncs, syncs.length);
            return;
        }
        queueMicrotask(() => finish(job, null));
    },
    analyzePath: (p) => ({ exists: files.has(p) || dirs.has(p) }),
    stat: (p) => ({ mode: dirs.has(p) ? 1 : 0 }),
    isDir: (m) => m === 1,
    mkdirTree(p) {
        let base = '';
        for (const part of p.split('/').filter(Boolean)) {
            base += '/' + part;
            dirs.add(base);
        }
    },
    writeFile(p, data) {
        if (mode === 'write-failure' && p.endsWith('/content/scene.kdl')) throw Error('disk full');
        files.set(
            p,
            typeof data === 'string' ? new TextEncoder().encode(data) : new Uint8Array(data),
        );
    },
    readFile(p, opt) {
        if (!files.has(p)) throw Error('missing ' + p);
        return opt?.encoding ? new TextDecoder().decode(files.get(p)) : files.get(p);
    },
    readdir(p) {
        return [
            '.',
            '..',
            ...new Set(
                [...files.keys(), ...dirs]
                    .filter((x) => x.startsWith(p + '/'))
                    .map((x) => x.slice(p.length + 1).split('/')[0]),
            ),
        ];
    },
    unlink: (p) => files.delete(p),
    rmdir: (p) => dirs.delete(p),
};
let downloads = 0,
    revision = 'a'.repeat(40),
    checksum,
    archive;
async function launch(search = '') {
    let ready,
        aborted = '',
        released = false;
    const done = new Promise((r) => (ready = r));
    const location = {
        origin: 'https://example.test',
        href: 'https://example.test/game/dev/' + search,
        search,
    };
    const handlers = {},
        inputs = [],
        timers = new Map(),
        status = {
            dataset: {},
            hidden: true,
            buttons: [],
            set textContent(text) {
                this.text = text;
                this.buttons = [];
            },
            get textContent() {
                return this.text || '';
            },
            append(button) {
                this.buttons.push(button);
            },
        },
        canvas = { addEventListener() {} };
    let nextTimer = 0;
    const context = {
        location,
        window: {
            location,
            addEventListener(name, run) {
                handlers[name] = run;
            },
        },
        document: {
            getElementById: (id) => (id === 'status' ? status : canvas),
            createElement: (tag) => {
                const element = { click() {} };
                if (tag === 'input') inputs.push(element);
                return element;
            },
        },
        console,
        navigator: { storage: {} },
        history: { replaceState() {} },
        URL,
        URLSearchParams,
        TextEncoder,
        TextDecoder,
        Uint8Array,
        DataView,
        ArrayBuffer,
        Blob,
        Response,
        crypto: require('node:crypto').webcrypto,
        AbortSignal,
        setTimeout(run) {
            timers.set(++nextTimer, run);
            return nextTimer;
        },
        clearTimeout: (id) => timers.delete(id),
        addRunDependency() {},
        removeRunDependency() {
            released = true;
            Module.onRuntimeInitialized();
            ready();
        },
        abort(message) {
            aborted = message;
            ready();
        },
        async fetch(url) {
            downloads++;
            if (mode === 'download') return new Response('', { status: 404 });
            if (url.pathname.endsWith('.json'))
                return mode === 'json'
                    ? new Response('{broken')
                    : Response.json({
                          format: 1,
                          id: 'celeste',
                          revision,
                          archive: 'project.zip',
                          sha256: mode === 'checksum' ? '0'.repeat(64) : checksum,
                      });
            return new Response(archive);
        },
    };
    vm.createContext(context);
    vm.runInContext(shell, context);
    const Module = context.Module;
    Object.assign(Module, { FS, chibiPublishedProject: 'project.json' });
    vm.runInContext(code, context);
    if (!archive) {
        archive = await Module.chibiZipFiles([
            ['project.kdl', new TextEncoder().encode('name Celeste')],
            [
                mode === 'traversal' ? '../outside' : 'content/scene.kdl',
                new TextEncoder().encode('original'),
            ],
        ]).arrayBuffer();
        checksum = require('node:crypto')
            .createHash('sha256')
            .update(Buffer.from(archive))
            .digest('hex');
    }
    Module.preRun[0]();
    await done;
    Module.setStatus('');
    active = { Module, handlers, inputs, timers, status, location };
    return { root: Module.arguments[1], aborted, released };
}
async function startImport() {
    const { Module, inputs } = active,
        encode = (text) => new TextEncoder().encode(text);
    const buffer = await Module.chibiZipFiles([
        ['project.kdl', encode('name Imported')],
        ['content/scene.kdl', encode('imported')],
    ]).arrayBuffer();
    Module.chibiImportProject();
    const input = inputs.at(-1);
    input.files = [{ name: 'imported.zip', arrayBuffer: async () => buffer }];
    active.importing = input.onchange();
    return '/workspace/imported/';
}
(async () => {
    if (mode === 'explicit' || saving) {
        FS.mkdirTree('/workspace/saved');
        FS.writeFile('/workspace/saved/project.kdl', 'name Saved');
    }
    if (saving) {
        FS.writeFile('/workspace/.last', 'saved');
        FS.writeFile('/workspace/saved/content/scene.kdl', 'original');
        for (const [p, data] of files) durable.set(p, data);
    }
    if (mode === 'conflict') {
        FS.mkdirTree('/workspace/published-celeste-' + revision);
        FS.writeFile('/workspace/published-celeste-' + revision + '/project.kdl', 'name Existing');
    }
    let outcome = await launch(
        mode === 'explicit' || saving
            ? '?project=saved'
            : mode === 'sample'
              ? '?project-root=projects/starter'
              : mode === 'new'
                ? '?new'
                : '',
    );
    if (mode === 'repeat' || mode === 'revision') {
        FS.writeFile(outcome.root + 'content/scene.kdl', 'edited');
        if (mode === 'revision') revision = 'b'.repeat(40);
        outcome = await launch();
    }
    if (saving) {
        const { Module, handlers, timers, status } = active,
            path = outcome.root + 'content/scene.kdl';
        const warning = () => {
            const event = {
                defaultPrevented: false,
                preventDefault() {
                    this.defaultPrevented = true;
                },
            };
            handlers.beforeunload(event);
            return event.defaultPrevented;
        };
        const timer = () => {
            const pending = [...timers.values()];
            timers.clear();
            for (const run of pending) run();
        };
        const settle = async () => {
            for (let i = 0; i < 12; i++) await Promise.resolve();
        };
        const saved = () => new TextDecoder().decode(durable.get(path));
        const save = (text) => {
            Module.chibiUnsaved = true;
            Module.chibiSaveAsset(path, text);
            FS.writeFile(path, text);
            Module.chibiUnsaved = false;
        };
        held = true;
        let created = '';
        if (mode === 'save-import-failure') {
            created = await startImport();
            await settle();
        } else if (mode === 'save-open-failure') {
            created = JSON.parse(Module.chibiCreateProject('Created', 1)).root;
            Module.chibiOpenProject(created, false);
        } else save('edited');
        const before = warning(),
            pendingStatus = status.textContent;
        timer();
        await settle();
        const during = warning();
        let retained = false,
            failure = '',
            first = '',
            afterFirst = false,
            retryLabel = '';
        if (mode === 'save-race') {
            save('second');
            timer();
            await settle();
        }
        if (mode === 'save-completion-race')
            queueMicrotask(() =>
                queueMicrotask(() => {
                    save('second');
                    Module.chibiOpenProject(outcome.root);
                }),
            );
        finish(syncs.shift(), mode.endsWith('-failure') ? Error('disk full') : null);
        await settle();
        await active.importing;
        if (mode === 'save-race' || mode === 'save-completion-race') {
            first = saved();
            afterFirst = warning();
            if (syncs.length) finish(syncs.shift(), null);
            await settle();
        }
        const openStatus = Module.chibiProjectOpenStatus(),
            opened = active.location.search,
            createdBefore = created ? durable.has(created + 'project.kdl') : false,
            fatal = 'error' in status.dataset;
        if (mode.endsWith('-failure')) {
            retained = warning();
            failure = status.textContent;
            first = saved();
            const retry = status.buttons[0];
            retryLabel = retry?.textContent || '';
            if (retry) {
                retry.onclick();
                await settle();
                finish(syncs.shift(), null);
            }
            await settle();
        }
        console.log(
            JSON.stringify({
                before,
                during,
                pendingStatus,
                after: warning(),
                saved: saved(),
                status: status.textContent,
                maxSyncs,
                retained,
                failure,
                first,
                afterFirst,
                retryLabel,
                openStatus,
                opened,
                createdBefore,
                createdAfter: created ? durable.has(created + 'project.kdl') : false,
                fatal,
                openedAfter: active.location.search,
                createdContent: created
                    ? new TextDecoder().decode(durable.get(created + 'content/scene.kdl'))
                    : '',
            }),
        );
        return;
    }
    const text = (p) => (files.has(p) ? FS.readFile(p, { encoding: 'utf8' }) : '');
    console.log(
        JSON.stringify({
            ...outcome,
            downloads,
            content: text(outcome.root + 'content/scene.kdl'),
            previous: text('/workspace/published-celeste-' + 'a'.repeat(40) + '/content/scene.kdl'),
            project: text(outcome.root + 'project.kdl'),
            outside: files.has('/workspace/outside'),
            partial: dirs.has('/workspace/published-celeste-' + revision),
        }),
    );
})().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
