// Browser transport for the C collaboration model. Room credentials live in
// this tab's session storage and never enter the persistent project workspace.
(() => {
    if (typeof window === 'undefined') return;

    const key = 'chibi.collab.invite';
    const config = Module['collabConfig'] || {};
    const status = (text, error = false) => {
        const fn = Module['_collab_feedback'];
        const toString = Module['stringToNewUTF8'];
        const free = Module['_free'];
        if (!fn || !toString || !free) return;
        const pointer = toString(String(text));
        try { fn(pointer, error ? 1 : 0); } finally { free(pointer); }
    };
    const callC = (name, ...values) => {
        const fn = Module[`_${name}`];
        const toString = Module['stringToNewUTF8'];
        const free = Module['_free'];
        if (!fn || !toString || !free) return false;
        const pointers = values.map(value => toString(String(value)));
        try {
            fn(...pointers);
        } finally {
            for (const pointer of pointers) free(pointer);
        }
        return true;
    };
    const readInvite = value => {
        const parse = text => {
            try {
                const invite = JSON.parse(text);
                if (invite && invite.room && invite.client && invite.role && invite.token && invite.ws_url && invite.expires_at) return invite;
            } catch {}
            return null;
        };
        if (!value) return null;
        if (value.startsWith('http://') || value.startsWith('https://')) {
            const url = new URL(value);
            const encoded = new URLSearchParams(url.hash.slice(1)).get('collab');
            return encoded && parse(encoded);
        }
        const encoded = new URLSearchParams(value.replace(/^#/, '')).get('collab');
        return encoded ? parse(encoded) : parse(value);
    };
    const validateInvite = invite => {
        if (!invite || !/^[A-Za-z0-9_-]{1,96}$/.test(invite.room || '') ||
            !/^[A-Za-z0-9_-]{1,96}$/.test(invite.client || '') ||
            !['host', 'editor', 'viewer'].includes(invite.role) || typeof invite.token !== 'string' || !invite.token) {
            throw new Error('The room invite is invalid');
        }
        const expiry = typeof invite.expires_at === 'number'
            ? invite.expires_at * 1000 : Date.parse(invite.expires_at);
        if (!Number.isFinite(expiry) || expiry <= Date.now()) throw new Error('The room invite has expired');
        if (!config.roomUrl) throw new Error('This build has no trusted room service configured');
        const service = new URL(config.roomUrl);
        if (!['http:', 'https:'].includes(service.protocol) || service.username || service.password ||
            service.search || service.hash || service.pathname !== '/') {
            throw new Error('The configured room service URL is invalid');
        }
        const expected = new URL(`/v1/rooms/${encodeURIComponent(invite.room)}/ws`, service);
        expected.protocol = service.protocol === 'https:' ? 'wss:' : 'ws:';
        const actual = new URL(invite.ws_url);
        if (actual.origin !== expected.origin || actual.pathname !== expected.pathname ||
            actual.username || actual.password || actual.search || actual.hash) {
            throw new Error('The room invite points to an untrusted service');
        }
        return invite;
    };
    const roomUrl = invite => {
        const url = new URL(location.href);
        url.searchParams.set('room', invite.room);
        url.searchParams.set('role', invite.role);
        url.searchParams.delete('new');
        url.hash = `collab=${encodeURIComponent(JSON.stringify(invite))}`;
        return url;
    };
    const saveInvite = invite => {
        sessionStorage.setItem(key, JSON.stringify(invite));
        history.replaceState(null, '', `${location.pathname}${location.search}`);
    };
    const storedInvite = () => {
        try {
            const invite = validateInvite(JSON.parse(sessionStorage.getItem(key) || 'null'));
            if (!invite) {
                sessionStorage.removeItem(key);
                return null;
            }
            return invite;
        } catch {
            sessionStorage.removeItem(key);
            return null;
        }
    };
    const baseUrl = () => {
        if (!config.roomUrl) throw new Error('Collaboration is not configured for this build');
        return config.roomUrl.replace(/\/$/, '');
    };
    const request = async (path, token, body) => {
        const headers = {'Content-Type': 'application/json'};
        if (token) headers.Authorization = `Bearer ${token}`;
        const response = await fetch(`${baseUrl()}${path}`, {
            method: 'POST', headers, body: JSON.stringify(body || {}),
        });
        if (!response.ok) throw new Error((await response.text()) || `Room service returned ${response.status}`);
        return response.json();
    };
    const canonicalFiles = files => {
        if (!Array.isArray(files)) throw new Error('Snapshot has no file archive');
        const encoder = new TextEncoder();
        const compare = (a, b) => {
            const left = encoder.encode(a.path), right = encoder.encode(b.path);
            const length = Math.min(left.length, right.length);
            for (let i = 0; i < length; i++) if (left[i] !== right[i]) return left[i] - right[i];
            return left.length - right.length;
        };
        const sorted = [...files].sort(compare);
        if (files.some((file, index) => file.path !== sorted[index].path)) throw new Error('Snapshot file paths are not canonical');
        return files;
    };
    const sortedFiles = files => {
        const encoder = new TextEncoder();
        const compare = (a, b) => {
            const left = encoder.encode(a.path), right = encoder.encode(b.path);
            const length = Math.min(left.length, right.length);
            for (let i = 0; i < length; i++) if (left[i] !== right[i]) return left[i] - right[i];
            return left.length - right.length;
        };
        return [...files].sort(compare);
    };
    const digest = async value => {
        if (!globalThis.crypto?.subtle) throw new Error('This browser cannot verify collaboration snapshots');
        const bytes = new TextEncoder().encode(value);
        const hash = await globalThis.crypto.subtle.digest('SHA-256', bytes);
        return [...new Uint8Array(hash)].map(byte => byte.toString(16).padStart(2, '0')).join('');
    };
    const decodeBase64 = value => {
        if (typeof value !== 'string' || value.length % 4 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) {
            throw new Error('Snapshot has invalid base64 data');
        }
        const binary = atob(value);
        const bytes = Uint8Array.from(binary, char => char.charCodeAt(0));
        let canonical = '';
        for (let offset = 0; offset < bytes.length; offset += 0x8000) {
            canonical += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
        }
        if (btoa(canonical) !== value) throw new Error('Snapshot has noncanonical base64 data');
        return bytes;
    };
    const rewriteRoot = (value, remote, local) => {
        if (typeof value === 'string') return value.split(remote).join(local);
        if (Array.isArray(value)) return value.map(item => rewriteRoot(item, remote, local));
        if (value && typeof value === 'object') {
            return Object.fromEntries(Object.entries(value).map(([name, item]) => [name, rewriteRoot(item, remote, local)]));
        }
        return value;
    };
    const prepareSnapshot = async envelope => {
        const payload = envelope.payload;
        const files = canonicalFiles(payload.files);
        const hashInput = JSON.stringify({
            checkpoint_version: payload.checkpoint_version,
            workspace: payload.workspace,
            files,
        });
        const actual = await digest(hashInput);
        if (actual !== payload.sha256) throw new Error('Snapshot checksum does not match');
        if (payload.checkpoint_version !== 1 || typeof payload.workspace !== 'string') throw new Error('Unsupported collaboration snapshot');
        const workspace = JSON.parse(payload.workspace);
        if (typeof workspace.project !== 'string' || !workspace.project) throw new Error('Snapshot has no project root');
        const localRoot = Module['chibiCollabStorage']?.root;
        if (!localRoot) throw new Error('This page was not launched with an isolated room workspace');
        const decoded = files.map(file => ({path: file.path, data: decodeBase64(file.data)}));
        Module['chibiCollabStorage'].installFiles(decoded);
        const remoteRoot = workspace.project.endsWith('/') ? workspace.project : `${workspace.project}/`;
        const rewritten = rewriteRoot(workspace, remoteRoot, localRoot);
        rewritten.project = localRoot;
        envelope.payload.workspace = JSON.stringify(rewritten);
        envelope.payload.project_root = localRoot;
        return envelope;
    };

    let socket, invite, manualClose = false, reconnectTimer, attempts = 0, revision = 0, sendQueue = Promise.resolve();
    const sendRaw = value => {
        if (!socket || socket.readyState !== WebSocket.OPEN) return false;
        try { socket.send(JSON.stringify(value)); return true; } catch { return false; }
    };
    const notifyC = envelope => callC('collab_receive', JSON.stringify(envelope));
    const inspect = () => {
        const fn = Module['_collab_inspect'];
        const toText = Module['UTF8ToString'];
        const free = Module['_free'];
        if (!fn || !toText || !free) return null;
        const pointer = fn();
        if (!pointer) return null;
        try { return JSON.parse(toText(pointer)); } finally { free(pointer); }
    };
    const handshake = ws => {
        if (!callC('collab_start', invite.room, invite.client, invite.role)) {
            status('Collaboration support is unavailable in this build.', true);
            ws.close();
            return;
        }
        sendRaw({protocol: 1, room: invite.room, client: invite.client, seq: 0, revision: 0,
            kind: 'hello', payload: {token: invite.token}});
        sendRaw({protocol: 1, room: invite.room, client: invite.client, seq: 0, revision: 0,
            kind: 'join', payload: {last_revision: currentRevision()}});
        status(`Connected to room ${invite.room}; synchronizing...`);
        attempts = 0;
    };
    const currentRevision = () => {
        const state = inspect();
        return Number.isSafeInteger(state?.revision) ? state.revision : revision;
    };
    const connect = session => {
        invite = session;
        manualClose = false;
        status(`Connecting to room ${invite.room}...`);
        let ws;
        try { ws = new WebSocket(invite.ws_url); } catch (error) { status(error.message, true); return; }
        socket = ws;
        ws.onopen = () => { if (socket === ws) handshake(ws); };
        ws.onmessage = async event => {
            if (socket !== ws || typeof event.data !== 'string') return;
            let envelope;
            try { envelope = JSON.parse(event.data); } catch { status('Room service sent invalid JSON.', true); return; }
            if (!envelope || envelope.protocol !== 1 || envelope.room !== invite.room ||
                typeof envelope.kind !== 'string' || !envelope.payload) {
                status('Room service sent an invalid message.', true);
                return;
            }
            try {
                if (envelope.kind === 'snapshot') envelope = await prepareSnapshot(envelope);
                if (envelope.kind === 'joined') status(`Room ${invite.room} · ${envelope.payload.role || invite.role}`);
                if (envelope.kind === 'room_closed') status('The room has closed. You can rejoin with an active invite.', true);
                notifyC(envelope);
                if (envelope.kind === 'error') {
                    const reason = typeof envelope.payload.reason === 'string'
                        ? envelope.payload.reason : 'Room service error';
                    status(reason, true);
                }
            } catch (error) {
                status(`Could not verify the room snapshot: ${error.message}`, true);
                callC('collab_request_snapshot');
                callC('collab_disconnect', 'Snapshot verification failed');
            }
        };
        ws.onerror = () => status('Room connection failed; reconnecting...');
        ws.onclose = event => {
            if (socket !== ws) return;
            socket = undefined;
            if (manualClose) return;
            callC('collab_disconnect', event.reason || `WebSocket closed (${event.code})`);
            status('Connection lost; reconnecting...');
            clearTimeout(reconnectTimer);
            const delay = Math.min(15000, 500 * 2 ** Math.min(attempts++, 5));
            reconnectTimer = setTimeout(() => connect(invite), delay);
        };
    };
    const send = json => {
        if (!socket || socket.readyState !== WebSocket.OPEN) return false;
        const target = socket;
        let envelope;
        try { envelope = JSON.parse(json); } catch { status('C sent an invalid collaboration message.', true); return false; }
        sendQueue = sendQueue.then(async () => {
            if (socket !== target || target.readyState !== WebSocket.OPEN) return;
            if (envelope.kind === 'snapshot') {
                const files = Module['chibiCollabStorage']?.exportFiles();
                if (!files) throw new Error('No isolated room workspace is available');
                const encoded = files.map(file => {
                    let binary = '';
                    for (let offset = 0; offset < file.data.length; offset += 0x8000) {
                        binary += String.fromCharCode(...file.data.subarray(offset, offset + 0x8000));
                    }
                    return {path: file.path, data: btoa(binary)};
                });
                const sorted = sortedFiles(encoded);
                envelope.payload.files = sorted;
                envelope.payload.sha256 = await digest(JSON.stringify({
                    checkpoint_version: envelope.payload.checkpoint_version,
                    workspace: envelope.payload.workspace,
                    files: sorted,
                }));
            }
            if (envelope.kind === 'snapshot_ack' && Number.isSafeInteger(envelope.revision)) {
                revision = Math.max(revision, envelope.revision);
            }
            if (socket === target && target.readyState === WebSocket.OPEN) target.send(JSON.stringify(envelope));
        }).catch(error => {
            status(`Could not send collaboration message: ${error.message}`, true);
            callC('collab_disconnect', error.message);
        });
        return true;
    };
    const bootstrap = async session => {
        if (!session || !session.room || !session.client || !session.role || !session.token || !session.ws_url) {
            throw new Error('The room invite is incomplete');
        }
        session = validateInvite(session);
        invite = session;
        saveInvite(session);
        const url = new URL(location.href);
        if (url.searchParams.get('room') !== session.room || url.searchParams.get('role') !== session.role) {
            location.href = roomUrl(session).href;
            return;
        }
        if (!Module['chibiCollabStorage']) throw new Error('Reload the page to create an isolated room workspace');
        revision = 0;
        connect(session);
    };
    const clipboardInvite = async inviteData => {
        const link = roomUrl(inviteData).href;
        await navigator.clipboard.writeText(link);
        status(`${inviteData.role} invite copied to clipboard.`);
    };
    const control = async (command, argument = '') => {
        try {
            if (command === 'host') {
                // Production creation may require an operator credential. It is used
                // only for this request and is never retained by the browser bridge.
                status('Creating room...');
                const created = await request('/v1/rooms', argument || null, {});
                return await bootstrap(created);
            }
            if (command === 'join') {
                status('Joining room...');
                const session = readInvite(argument) || readInvite(location.hash) || storedInvite();
                if (!session) throw new Error('Paste a room invite link to join. A room code alone has no access token.');
                return await bootstrap(session);
            }
            if (command === 'leave') {
                status('Leaving room...');
                manualClose = true;
                clearTimeout(reconnectTimer);
                if (!callC('collab_leave')) close();
                sessionStorage.removeItem(key);
                status('Left the collaboration room.');
                return;
            }
            if (command === 'reconnect') {
                status('Reconnecting...');
                const session = storedInvite() || invite;
                if (!session) throw new Error('There is no active room invite to reconnect with');
                if (socket) socket.close();
                connect(session);
                return;
            }
            if (command === 'invite_viewer' || command === 'invite_editor') {
                if (!invite || invite.role !== 'host') throw new Error('Only the host can create room invites');
                const created = await request(`/v1/rooms/${encodeURIComponent(invite.room)}/invites`, invite.token,
                    {role: command === 'invite_editor' ? 'editor' : 'viewer'});
                await clipboardInvite(created);
                return;
            }
            throw new Error(`Unknown collaboration command: ${command}`);
        } catch (error) {
            status(error.message || error, true);
        }
    };
    const close = () => {
        manualClose = true;
        clearTimeout(reconnectTimer);
        if (socket) socket.close(1000, 'Leaving room');
        socket = undefined;
    };

    Module['chibiCollab'] = {connect, send, close, control, inspect};

    // An invite opened from a room link is copied into per-tab memory before its
    // fragment is removed, so refresh can reconnect without writing to IDBFS.
    const linked = readInvite(location.hash);
    if (linked) saveInvite(linked);
    const initialize = Module['onRuntimeInitialized'];
    Module['onRuntimeInitialized'] = () => {
        initialize?.();
        const session = linked || storedInvite();
        if (session && new URLSearchParams(location.search).get('room') === session.room) {
            queueMicrotask(() => bootstrap(session));
        }
    };
})();
