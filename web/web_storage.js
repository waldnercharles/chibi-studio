// Bundle downloads are also available with the local-server storage bridge.
if (typeof window !== 'undefined') {
    const crcTable = Array.from({length: 256}, (_, n) => {
        for (let k = 0; k < 8; k++) n = n & 1 ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
        return n >>> 0;
    });
    const crc32 = bytes => {
        let crc = ~0;
        for (const byte of bytes) crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
        return ~crc >>> 0;
    };
    // An uncompressed zip: small, and every archiver opens it.
    const zip = files => {
        const now = new Date();
        const time = (now.getHours() << 11) | (now.getMinutes() << 5) | (now.getSeconds() >> 1);
        const date = ((now.getFullYear() - 1980) << 9) | ((now.getMonth() + 1) << 5) | now.getDate();
        const parts = [], directory = [];
        let offset = 0;
        const header = (size, fields) => {
            const view = new DataView(new ArrayBuffer(size));
            fields.forEach(([at, bytes, value]) => bytes === 2 ? view.setUint16(at, value, true) : view.setUint32(at, value, true));
            return new Uint8Array(view.buffer);
        };
        for (const [name, data] of files) {
            const path = new TextEncoder().encode(name);
            const crc = crc32(data);
            // UTF-8 names, stored, then time, date, CRC, both sizes and the name length.
            const shared = [[6, 2, 0x0800], [10, 2, time], [12, 2, date], [14, 4, crc], [18, 4, data.length], [22, 4, data.length], [26, 2, path.length]];
            const local = header(30, [[0, 4, 0x04034b50], [4, 2, 20], ...shared]);
            const central = header(46, [[0, 4, 0x02014b50], [4, 2, 20], [6, 2, 20], ...shared.map(([at, bytes, value]) => [at + 2, bytes, value]), [42, 4, offset]]);
            parts.push(local, path, data);
            directory.push(central, path);
            offset += local.length + path.length + data.length;
        }
        const size = directory.reduce((total, part) => total + part.length, 0);
        const end = header(22, [[0, 4, 0x06054b50], [8, 2, files.length], [10, 2, files.length], [12, 4, size], [16, 4, offset]]);
        return new Blob([...parts, ...directory, end], {type: 'application/zip'});
    };
    Module['chibiZipFiles'] = zip;
    Module['chibiDownloadModelBundle'] = (directory, name) => {
        try {
            const files = ['model.mesh.json', 'model.obj', 'model.mtl', 'provenance.json']
                .map(file => [file, Module['FS'].readFile(`${directory}/${file}`)]);
            const archive = zip(files);
            const link = document.createElement('a');
            link.href = URL.createObjectURL(archive);
            link.download = `${name.replace(/[^a-z0-9_-]/gi, '_') || 'model'}-mesh.zip`;
            link.click();
            setTimeout(() => URL.revokeObjectURL(link.href), 40000);
            return true;
        } catch (error) {
            Module['showError']?.(`Could not download model bundle: ${error.message || error}`);
            return false;
        }
    };
}

// Without the local server bridge, projects persist in IndexedDB, one folder each under /workspace.
// A project is its folder: project.kdl plus everything beside it.
if (typeof window !== 'undefined' && !Module['chibiSaveAsset']) {
    const launch = new URLSearchParams(window.location.search);
    const workspace = '/workspace';
    const last = workspace + '/.last';
    // The project is chosen once IndexedDB has loaded; main reads this same array.
    const args = Module['arguments'] = ['--project-root', ''];
    let project;
    const fs = () => Module['FS'];
    const report = error => {
        if (error) Module['showError'](`Browser storage failed: ${error.message || error}`);
    };
    const sync = () => new Promise((done, fail) => fs().syncfs(false, error => (error ? fail(error) : done())));
    let timer;
    const persist = () => {
        clearTimeout(timer);
        timer = setTimeout(() => sync().catch(report), 250);
    };
    // Browsers may evict site storage; ask once the user has made data of their own.
    let kept;
    const keep = () => {
        kept ||= navigator.storage?.persist?.();
    };
    const exists = path => fs().analyzePath(path).exists;
    const kdlString = value => JSON.stringify(value).replace(/\\u([0-9a-f]{4})/gi, (_, hex) => `\\u{${hex}}`);
    const slug = name => String(name).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
    const manifest = 'project.kdl';
    let projectRoot;
    const found = id => /^[a-z0-9-]+$/.test(id) && exists(`${workspace}/${id}/${manifest}`);
    const unique = name => {
        const base = slug(name) || 'project';
        let id = base;
        for (let n = 2; exists(`${workspace}/${id}`); n++) id = `${base}-${n}`;
        return id;
    };
    const walk = (path, visit, name = '') => {
        if (!fs().isDir(fs().stat(path).mode)) {
            visit(name, fs().readFile(path));
            return;
        }
        for (const child of fs().readdir(path)) {
            if (child !== '.' && child !== '..') walk(`${path}/${child}`, visit, name ? `${name}/${child}` : child);
        }
    };
    const write = (path, data) => {
        fs().mkdirTree(path.slice(0, path.lastIndexOf('/')));
        fs().writeFile(path, data);
    };
    Module['chibiCreateProject'] = (name, version) => {
        const title = String(name).trim();
        if (!title || title.length > 128) return JSON.stringify({error: 'Enter a project name of 1 to 128 characters'});
        if (!Number.isInteger(version) || version < 1) return JSON.stringify({error: 'Invalid project version'});
        let root;
        try {
            const id = unique(title);
            root = `${workspace}/${id}`;
            const manifestPath = `${root}/${manifest}`;
            const scenePath = `${root}/content/scenes/Start.kdl`;
            write(manifestPath, `version ${version}\nname ${kdlString(title)}\ncontent "content"\nstart_scene "scenes/Start.kdl"\n`);
            write(scenePath, "version 1\nname \"Start\"\n(array)entities\n");
            keep();
            persist();
            return JSON.stringify({root: `${root}/`});
        } catch (error) {
            if (root) {
                try {
                    for (const path of [`${root}/content/scenes/Start.kdl`, `${root}/${manifest}`]) {
                        if (exists(path)) fs().unlink(path);
                    }
                    for (const path of [`${root}/content/scenes`, `${root}/content`, root]) {
                        if (exists(path)) fs().rmdir(path);
                    }
                } catch {
                    // Keep reporting the error that caused project creation to fail.
                }
            }
            return JSON.stringify({error: error.message || String(error)});
        }
    };
    // Samples are bundled as project folders under /projects; the Studio starter opens by default.
    const samples = '/projects';
    const bundled = `/${launch.get('project-root') || 'projects/starter'}`.replace(/\/+/g, '/').replace(/\/$/, '');
    const sample = bundled.slice(bundled.lastIndexOf('/') + 1);
    const copySample = id => {
        walk(bundled, (name, data) => write(`${workspace}/${id}/${name}`, data));
        persist();
        return id;
    };
    const lastOpened = () => (exists(last) ? fs().readFile(last, {encoding: 'utf8'}) : '');
    // ?project= opens a stored project and ?new copies the sample. Until samples open
    // read-only, launching one opens the user's own copy of it.
    const chooseProject = () => {
        const requested = launch.get('project');
        if (requested !== null && !found(requested)) throw new Error(`Project not found: ${requested}`);
        if (requested) return requested;
        if (launch.has('new')) {
            const id = copySample(unique(sample));
            history.replaceState(null, '', `?project=${id}`);
            return id;
        }
        const previous = lastOpened();
        if (!launch.has('project-root') && found(previous)) return previous;
        const id = slug(sample) || 'project';
        return found(id) ? id : copySample(unique(id));
    };

    const inflate = async data => new Uint8Array(
        await new Response(new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'))).arrayBuffer()
    );
    const unzip = async buffer => {
        const view = new DataView(buffer);
        let end = buffer.byteLength - 22;
        while (end >= 0 && view.getUint32(end, true) !== 0x06054b50) end--;
        if (end < 0) throw new Error('Not a zip archive');
        const files = [];
        for (let i = 0, at = view.getUint32(end + 16, true); i < view.getUint16(end + 10, true); i++) {
            const method = view.getUint16(at + 10, true);
            const size = view.getUint32(at + 20, true);
            const length = view.getUint16(at + 28, true);
            const local = view.getUint32(at + 42, true);
            const name = new TextDecoder().decode(new Uint8Array(buffer, at + 46, length));
            at += 46 + length + view.getUint16(at + 30, true) + view.getUint16(at + 32, true);
            if (name.endsWith('/')) continue;
            if (method !== 0 && method !== 8) throw new Error(`Unsupported compression in ${name}`);
            const start = local + 30 + view.getUint16(local + 26, true) + view.getUint16(local + 28, true);
            const data = new Uint8Array(buffer, start, size);
            files.push([name, method === 8 ? await inflate(data) : data]);
        }
        return files;
    };
    // The shallowest project.kdl marks the project folder, at the archive root or in one
    // top folder; the project takes that folder's name, or the archive's.
    const importFiles = (archive, file) => {
        const manifestName = archive.map(([name]) => name)
            .filter(name => name === manifest || name.endsWith('/' + manifest))
            .sort((a, b) => a.split('/').length - b.split('/').length)[0];
        if (!manifestName) throw new Error(`The archive has no ${manifest}`);
        const prefix = manifestName.slice(0, -manifest.length);
        const id = unique(prefix.split('/').at(-2) || file.replace(/\.zip$/i, ''));
        for (const [name, data] of archive) {
            const path = name.slice(prefix.length);
            const inside = name.startsWith(prefix) && !name.startsWith('__MACOSX/') && !path.split('/').includes('..');
            if (inside) write(`${workspace}/${id}/${path}`, data);
        }
        return id;
    };

    Module['chibiSaveAsset'] = () => {
        keep();
        persist();
        return 0;
    };
    // The editor has already removed the file from the mounted workspace.
    Module['chibiDeleteAsset'] = Module['chibiSaveAsset'];
    const exportProject = () => {
        const files = [];
        walk(Module['chibiCollabStorage'].root.replace(/\/$/, ''), (name, data) => files.push([`${project}/${name}`, data]));
        const link = document.createElement('a');
        link.href = URL.createObjectURL(Module['chibiZipFiles'](files));
        link.download = `${project}.zip`;
        link.click();
        // Revoking right away can cancel the download in some browsers.
        setTimeout(() => URL.revokeObjectURL(link.href), 40000);
    };
    const newProject = () => {
        const search = new URLSearchParams({new: ''});
        if (launch.has('project-root')) search.set('project-root', launch.get('project-root'));
        location.search = `?${search}`;
    };
    // Studio's Open Project list: the user's projects, then each sample not yet
    // copied into one. Opening one reloads the page onto it, as Import does.
    const projectName = path => {
        const value = fs().readFile(path, {encoding: 'utf8'}).match(/^\s*name\s+("(?:[^"\\]|\\.)*"|[^\s;{}]+)/m)?.[1];
        if (!value) return undefined;
        if (!value.startsWith('"')) return value;
        const json = value.replace(/\\u\{([0-9a-f]{1,6})\}/gi, (_, hex) => {
            const code = Number.parseInt(hex, 16);
            return JSON.stringify(String.fromCodePoint(code)).slice(1, -1);
        }).replace(/\\s/g, ' ');
        try { return JSON.parse(json); } catch { return undefined; }
    };
    const folders = path => (exists(path) ? fs().readdir(path).filter(name => !name.startsWith('.')) : []);
    Module['chibiProjects'] = () => JSON.stringify([
        ...folders(workspace).filter(found).map(id => ({
            name: projectName(`${workspace}/${id}/${manifest}`) || id,
            root: `${workspace}/${id}/`,
        })),
        ...folders(samples).filter(sample => !found(slug(sample)) && exists(`${samples}/${sample}/${manifest}`)).map(sample => ({
            name: projectName(`${samples}/${sample}/${manifest}`) || sample,
            root: `${samples}/${sample}/`,
        })),
    ]);
    let projectOpenStatus = '';
    Module['chibiProjectOpenStatus'] = () => projectOpenStatus;
    Module['chibiOpenProject'] = (root, reportFailure = true) => {
        const [, folder, name] = root.match(/^\/([^/]+)\/([^/]+)\/$/);
        const search = folder === 'projects' ? `?project-root=projects/${name}` : `?project=${name}`;
        clearTimeout(timer);
        projectOpenStatus = 'pending';
        sync().then(() => {
            projectOpenStatus = 'done';
            // Studio already asked before discarding unsaved edits.
            Module['chibiUnsaved'] = false;
            location.search = search;
        }).catch(error => {
            projectOpenStatus = `error:${error.message || error}`;
            if (reportFailure) report(error);
        });
    };
    // The shell offers these once Chibi stops, so a project that cannot load never traps the user.
    Module['recovery'] = [['New Project', newProject]];
    Module['chibiExportProject'] = exportProject;
    Module['chibiImportProject'] = () => {
        const input = document.createElement('input');
        input.type = 'file';
        input.accept = '.zip,application/zip';
        input.onchange = async () => {
            try {
                const file = input.files[0];
                const id = importFiles(await unzip(await file.arrayBuffer()), file.name);
                fs().writeFile(last, id);
                keep();
                await sync();
                location.search = `?project=${id}`;
            } catch (error) {
                Module['showError'](`Could not import project: ${error.message}`);
            }
        };
        input.click();
    };
    (Module['preRun'] ||= []).push(() => {
        fs().mkdirTree(workspace);
        fs().mount(fs().filesystems.IDBFS, {}, workspace);
        addRunDependency('chibi-storage');
        fs().syncfs(true, error => {
            report(error);
            removeRunDependency('chibi-storage');
        });
    });
    // Bundled samples are readable only once every run dependency has resolved.
    Module['onRuntimeInitialized'] = () => {
        const previous = lastOpened();
        project = chooseProject();
        projectRoot = `${workspace}/${project}/`;
        args[1] = projectRoot;
        Module['recovery'].unshift(['Export Project', exportProject]);
        if (previous === project) return;
        fs().writeFile(last, project);
        persist();
    };
}
