const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { runOwned } = require('./process.cjs');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');
const storage = path.join(root, 'web/web_storage.js');
const driver = path.join(__dirname, 'storage-driver.cjs');

async function run(mode) {
    const result = await runOwned(process.execPath, [driver, storage, mode], {
        cwd: root,
    });
    assert.equal(result.timedOut, false, 'child process exceeded the fixture deadline');
    assert.equal(result.code, 0, result.error?.message || result.stderr);
    return JSON.parse(result.stdout);
}

test('published_source_opens_instead_of_starter', async () => {
    const value = await run('initial');
    assert.equal(value.released, true);
    assert.equal(value.aborted, '');
    assert.equal(value.project, 'name Celeste');
    assert.equal(value.content, 'original');
    assert.ok(value.root.startsWith('/workspace/published-celeste-'));
});

test('reopening_a_revision_preserves_edits_without_redownloading', async () => {
    const value = await run('repeat');
    assert.equal(value.content, 'edited');
    assert.equal(value.downloads, 3);
});

test('new_revision_retains_the_previous_workspace', async () => {
    const value = await run('revision');
    assert.equal(value.content, 'original');
    assert.equal(value.previous, 'edited');
    assert.equal(value.downloads, 4);
});

test('explicit_workspace_overrides_published_default', async () => {
    const value = await run('explicit');
    assert.equal(value.root, '/workspace/saved/');
    assert.equal(value.project, 'name Saved');
    assert.equal(value.downloads, 0);
});

test('explicit_sample_overrides_published_default', async () => {
    const value = await run('sample');
    assert.equal(value.project, 'name Starter');
    assert.equal(value.downloads, 0);
});

test('new_project_overrides_published_default', async () => {
    const value = await run('new');
    assert.equal(value.project, 'name Starter');
    assert.equal(value.downloads, 0);
});

test('invalid_manifest_stops_startup', async () => {
    const value = await run('json');
    assert.notEqual(value.aborted, '');
    assert.equal(value.released, false);
});

test('workspace_without_marker_is_preserved', async () => {
    const value = await run('conflict');
    assert.ok(value.aborted.includes('conflicts'));
    assert.equal(value.partial, true);
    assert.equal(value.downloads, 1);
});

test('failed_import_is_removed', async () => {
    const value = await run('write-failure');
    assert.ok(value.aborted.includes('disk full'));
    assert.equal(value.partial, false);
    assert.equal(value.released, false);
});

test('checksum_failure_stops_startup', async () => {
    const value = await run('checksum');
    assert.ok(value.aborted.includes('checksum'));
    assert.equal(value.released, false);
    assert.equal(value.root, '');
});

test('download_failure_stops_startup', async () => {
    const value = await run('download');
    assert.ok(value.aborted.includes('HTTP 404'));
    assert.equal(value.released, false);
});

test('archive_cannot_escape_its_workspace', async () => {
    const value = await run('traversal');
    assert.ok(value.aborted.includes('Invalid published project path'));
    assert.equal(value.outside, false);
    assert.equal(value.released, false);
});

test('saved_document_warns_before_unload_until_persistence_completes', async () => {
    const value = await run('save-reload');
    assert.equal(value.before, true);
    assert.equal(value.during, true);
    assert.equal(value.pendingStatus, 'Saving project...');
    assert.equal(value.after, false);
    assert.equal(value.saved, 'edited');
    assert.equal(value.status, '');
});

test('save_during_persistence_waits_for_the_newer_contents', async () => {
    const value = await run('save-race');
    assert.equal(value.before, true);
    assert.equal(value.afterFirst, true);
    assert.equal(value.first, 'edited');
    assert.equal(value.maxSyncs, 1);
    assert.equal(value.after, false);
    assert.equal(value.saved, 'second');
});

test('save_after_flush_completion_starts_a_new_flush_before_opening', async () => {
    const value = await run('save-completion-race');
    assert.equal(value.first, 'edited');
    assert.equal(value.afterFirst, true);
    assert.equal(value.after, false);
    assert.equal(value.saved, 'second');
});

test('failed_persistence_warns_until_a_successful_retry', async () => {
    const value = await run('save-failure');
    assert.equal(value.retained, true);
    assert.ok(value.failure.includes('disk full'));
    assert.equal(value.retryLabel, 'Retry');
    assert.equal(value.first, 'original');
    assert.equal(value.after, false);
    assert.equal(value.saved, 'edited');
    assert.equal(value.status, '');
});

test('created_project_failure_keeps_storage_retry_after_inline_error', async () => {
    const value = await run('save-open-failure');
    assert.equal(value.retained, true);
    assert.ok(value.failure.includes('disk full'));
    assert.equal(value.retryLabel, 'Retry');
    assert.equal(value.openStatus, 'error:disk full');
    assert.equal(value.opened, '?project=saved');
    assert.equal(value.createdBefore, false);
    assert.equal(value.createdAfter, true);
    assert.equal(value.after, false);
    assert.equal(value.status, '');
});

test('imported_project_failure_keeps_storage_retry', async () => {
    const value = await run('save-import-failure');
    assert.equal(value.retained, true);
    assert.ok(value.failure.includes('disk full'));
    assert.equal(value.fatal, false);
    assert.equal(value.retryLabel, 'Retry');
    assert.equal(value.opened, '?project=saved');
    assert.equal(value.openedAfter, '?project=saved');
    assert.equal(value.createdBefore, false);
    assert.equal(value.createdAfter, true);
    assert.equal(value.createdContent, 'imported');
    assert.equal(value.after, false);
    assert.equal(value.status, '');
});

test('browser_export_does_not_require_native_process_apis', async () => {
    const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'chibi-web-export-'));
    try {
        const include = path.join(workspace, 'editor/studio');
        fs.mkdirSync(include, { recursive: true });
        fs.writeFileSync(
            path.join(include, 'studio_internal.h'),
            '#include <stdbool.h>\ntypedef struct studio studio;\n',
        );
        // Compile the real browser branch against only its opaque Studio type.
        // Native SDL process APIs are absent from this include tree.
        const result = await runOwned(
            process.env.CC || 'cc',
            [
                '-std=c2x',
                '-Wall',
                '-Wextra',
                '-Werror',
                '-fsyntax-only',
                '-D__EMSCRIPTEN__',
                '-I',
                workspace,
                path.join(root, 'src/editor/studio/studio_export.c'),
            ],
            { cwd: root },
        );
        assert.equal(result.timedOut, false, 'child process exceeded the fixture deadline');
        assert.equal(result.code, 0, result.error?.message || result.stderr);
    } finally {
        fs.rmSync(workspace, { recursive: true, force: true });
    }
});
