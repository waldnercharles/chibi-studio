// Collaboration project files live in MEMFS. The IDBFS/browser cache remains
// the local project source and is never used for room snapshots.
(() => {
    if (typeof window === 'undefined') return;

    const normalizeRoot = value => {
        const root = `/${String(value || '').split('/').filter(Boolean).join('/')}`;
        return root === '/' ? '/' : `${root}/`;
    };
    const factory = initialRoot => {
        const fs = Module['FS'];
        const localRoot = normalizeRoot(initialRoot);
        let activeRoot = localRoot, roomBase, checkpoint = 0;
        const exists = path => fs.analyzePath(path).exists;
        const removeTree = path => {
            if (!exists(path)) return;
            if (!fs.isDir(fs.stat(path).mode)) {
                fs.unlink(path);
                return;
            }
            for (const child of fs.readdir(path)) {
                if (child !== '.' && child !== '..') removeTree(`${path}/${child}`);
            }
            fs.rmdir(path);
        };
        const walk = (path, visit, name = '') => {
            path = path.replace(/\/$/, '');
            if (!fs.isDir(fs.stat(path).mode)) {
                visit(name, fs.readFile(path));
                return;
            }
            for (const child of fs.readdir(path)) {
                if (child !== '.' && child !== '..') walk(`${path}/${child}`, visit, name ? `${name}/${child}` : child);
            }
        };
        const write = (path, data) => {
            fs.mkdirTree(path.slice(0, path.lastIndexOf('/')));
            fs.writeFile(path, data);
        };
        const validateFiles = files => {
            if (!Array.isArray(files) || !files.length) throw new Error('Snapshot has no project files');
            const seen = new Set();
            for (const file of files) {
                if (!file || typeof file.path !== 'string' || !file.path || file.path.startsWith('/') ||
                    file.path.includes('\\') || file.path.includes('\0') ||
                    file.path.split('/').some(part => !part || part === '.' || part === '..') || seen.has(file.path) ||
                    !(file.data instanceof Uint8Array)) throw new Error('Snapshot contains an unsafe project path');
                seen.add(file.path);
            }
            if (!seen.has('project.kdl')) throw new Error('Snapshot is missing project.kdl');
        };
        return {
            localRoot,
            get root() { return activeRoot; },
            exportFiles(root = activeRoot) {
                const files = [];
                walk(root, (path, data) => files.push({path, data}));
                return files;
            },
            enterRoom(room, copyCurrent = false) {
                if (!/^[a-zA-Z0-9_-]{1,96}$/.test(room)) throw new Error('Invalid collaboration room ID');
                roomBase = `/collab/${room}`;
                const root = `${roomBase}/project`;
                if (copyCurrent && activeRoot !== `${root}/`) {
                    removeTree(root);
                    walk(activeRoot, (path, data) => write(`${root}/${path}`, data));
                } else {
                    fs.mkdirTree(root);
                }
                return `${root}/`;
            },
            stageFiles(files) {
                if (!roomBase) throw new Error('Prepare the collaboration room before installing a snapshot');
                validateFiles(files);
                let root;
                do { root = `${roomBase}/checkpoint-${++checkpoint}/project`; } while (exists(root));
                try {
                    fs.mkdirTree(root);
                    for (const file of files) write(`${root}/${file.path}`, file.data);
                    return `${root}/`;
                } catch (error) {
                    removeTree(root);
                    throw error;
                }
            },
            installFiles(files) {
                if (!roomBase || !activeRoot.startsWith(`${roomBase}/`)) {
                    throw new Error('Enter the collaboration room before installing a snapshot');
                }
                validateFiles(files);
                removeTree(activeRoot.replace(/\/$/, ''));
                for (const file of files) write(`${activeRoot}${file.path}`, file.data);
            },
            activateRoot(root) {
                if (!roomBase || !root.startsWith(`${roomBase}/`) || !root.endsWith('/')) {
                    throw new Error('Snapshot root is outside the room workspace');
                }
                activeRoot = root;
            },
            discardRoot(root) {
                const path = root.replace(/\/$/, '');
                if (activeRoot !== root && roomBase && path.startsWith(`${roomBase}/checkpoint-`)) removeTree(path);
            },
            leaveRoom() {
                activeRoot = localRoot;
                roomBase = undefined;
                checkpoint = 0;
            },
        };
    };

    // Run after web storage has chosen the local project. Room pages work on a
    // copy in MEMFS and leave the user's persistent project untouched.
    const initialize = Module['onRuntimeInitialized'];
    Module['onRuntimeInitialized'] = () => {
        initialize?.();
        const storage = Module['chibiCollabStorage'] ||= factory(Module['arguments'][1]);
        const room = new URLSearchParams(window.location.search).get('room');
        if (!room) return;
        const root = storage.enterRoom(room, true);
        storage.activateRoot(root);
        Module['arguments'][1] = root;
    };
})();
