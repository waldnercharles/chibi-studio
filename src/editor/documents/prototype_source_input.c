#include "editor/documents/prototype_source_input.h"

#include "editor/collab/actions_scene.h"
#include "editor/collab/collab.h"
#include "editor/collab/protocol.h"
#include "editor/documents/asset_code.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOURCE_INPUT_GROUPS 32
#define SOURCE_INPUT_BYTES (4u * 1024u * 1024u)

typedef struct
{
    char *text, *kind, *key, *group;
    int history;
    bool live, coalesce;
    asset_code_stage disk;
    bool disk_stage, reload;
} source_input_group;

typedef struct scene_source_input
{
    char *root, *path, *name;
    uint64_t identity;
    char *base, *draft, *sent_before;
    prototype *preview;
    source_input_group *groups;
    collab_action_identity sent;
    uint64_t observed;
    bool waiting, conflict;
    char active_kind[32], active_key[192], active_group[96];
    char message[256];
} scene_source_input;

static const char *json_text(yyjson_val *root, const char *key)
{
    yyjson_val *value = yyjson_obj_get(root, key);
    const char *text = yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
    return text && yyjson_get_len(value) == strlen(text) ? text : NULL;
}

static bool fail(char *error, size_t size, const char *message)
{
    snprintf(error, size, "%s", message);
    return false;
}

static scene_source_input *find_input(app_scene_authoring *scene, const char *path)
{
    for (int i = 0; path && i < acount(scene->source_inputs); i++) {
        scene_source_input *input = &scene->source_inputs[i];
        if (!strcmp(input->root, scene->content_root) && !strcmp(input->path, path)) {
            return input;
        }
    }
    return NULL;
}

static scene_prototype_asset *input_asset(app_scene_authoring *scene, const scene_source_input *input)
{
    if (strcmp(input->root, scene->content_root)) {
        return NULL;
    }
    scene_prototype_asset *asset = scene_prototype_for_path(scene, input->path);
    return asset && scene_prototype_source_owned(asset) &&
                   !strcmp(asset->prototype->name, input->name)
             ? asset
             : NULL;
}

static scene_source_input *ensure_input(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    scene_source_input *input = find_input(scene, asset->path);
    if (input) {
        return !strcmp(input->name, asset->prototype->name) ? input : NULL;
    }
    scene_source_input value = { .root = xstrdup(scene->content_root),
                                 .path = xstrdup(asset->path),
                                 .name = xstrdup(asset->prototype->name),
                                 .identity = ++scene->source_input_generation };
    apush(scene->source_inputs, value);
    return &scene->source_inputs[acount(scene->source_inputs) - 1];
}

static void group_free(source_input_group *group)
{
    free(group->text);
    free(group->kind);
    free(group->key);
    free(group->group);
    asset_code_stage_free(&group->disk);
    memset(group, 0, sizeof *group);
}

static void groups_clear(scene_source_input *input)
{
    for (int i = 0; i < acount(input->groups); i++) {
        group_free(&input->groups[i]);
    }
    afree(input->groups);
}

static void input_clean(scene_source_input *input)
{
    groups_clear(input);
    free(input->base);
    free(input->draft);
    free(input->sent_before);
    prototype_free(input->preview);
    input->preview = NULL;
    input->base = input->draft = input->sent_before = NULL;
    input->waiting = input->conflict = false;
    memset(&input->sent, 0, sizeof input->sent);
    input->message[0] = 0;
}

void scene_source_input_free(app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->source_inputs); i++) {
        scene_source_input *input = &scene->source_inputs[i];
        input_clean(input);
        free(input->root);
        free(input->path);
        free(input->name);
    }
    afree(scene->source_inputs);
}

static void prepare_preview(app_scene_authoring *scene, scene_source_input *input)
{
    prototype_free(input->preview);
    input->preview = NULL;
    scene_prototype_asset *asset = input_asset(scene, input);
    if (!asset || !asset->source || !input->draft) {
        return;
    }
    authoring_prototype_edit_result candidate = { 0 };
    char error[256];
    if (authoring_prototype_source_edit(
            input->draft,
            &scene->registry,
            asset_code_catalog(asset->source),
            NULL,
            0,
            &candidate,
            error,
            sizeof error
        ) &&
        !strcmp(candidate.value->name, input->name)) {
        input->preview = candidate.value;
        candidate.value = NULL;
    }
    authoring_prototype_edit_result_free(&candidate);
}

const prototype *scene_source_input_preview(app_scene_authoring *scene, const scene_prototype_asset *asset)
{
    scene_source_input *input = find_input(scene, asset->path);
    return input && input->preview && !strcmp(input->name, asset->prototype->name)
             ? input->preview
             : asset->prototype;
}

static void show_input(app_scene_authoring *scene, scene_source_input *input)
{
    scene_prototype_asset *asset = input_asset(scene, input);
    if (asset && asset->source) {
        asset_code_set_local_dirty(asset->source, input->draft != NULL);
        asset_code_show_text(
            asset->source,
            input->draft ? input->draft : asset_code_text(asset->source)
        );
        scene_prototype_source_capture(scene, asset);
    }
}

static void conflict(scene_source_input *input, const char *message)
{
    input->conflict = true;
    input->active_group[0] = 0;
    snprintf(input->message, sizeof input->message, "%s", message);
}

void scene_source_input_rebind(app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->source_inputs); i++) {
        scene_source_input *input = &scene->source_inputs[i];
        if (input->draft && !input_asset(scene, input)) {
            conflict(input, "The shared source is unavailable or changed identity; your local draft is retained.");
        }
        prepare_preview(scene, input);
        show_input(scene, input);
    }
}

bool scene_source_input_any(const app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->source_inputs); i++) {
        if (scene->source_inputs[i].draft) {
            return true;
        }
    }
    return false;
}

bool scene_source_input_blocked(app_scene_authoring *scene, const char *path)
{
    scene_source_input *input = find_input(scene, path);
    if (!input || !input->draft) {
        return false;
    }
    snprintf(scene->prototype_message, sizeof scene->prototype_message, "%s", input->conflict ? "Resolve the retained source draft with Retry Draft or Use Shared first." : "Waiting for shared edit acceptance. Your local draft is retained.");
    return true;
}

const char *scene_source_input_text(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    scene_source_input *input = find_input(scene, asset->path);
    return input && input->draft ? input->draft : asset_code_text(asset->source);
}

void scene_source_input_end(app_scene_authoring *scene, const char *path)
{
    scene_source_input *input = find_input(scene, path);
    if (input) {
        input->active_group[0] = 0;
    }
}

static bool same_action(const collab_action_identity *a, const collab_action_identity *b)
{
    return a->id && a->id == b->id && a->seq == b->seq &&
           !strcmp(a->room, b->room) && !strcmp(a->client, b->client);
}

static const char *relative_path(const app_scene_authoring *scene, const char *path)
{
    size_t length = strlen(scene->content_root);
    return !strncmp(path, scene->content_root, length) && path[length] == '/'
             ? path + length + 1
             : NULL;
}

static char *input_action(app_scene_authoring *scene, scene_source_input *input)
{
    const char *path = relative_path(scene, input->path);
    if (!path || !acount(input->groups)) {
        return NULL;
    }
    source_input_group *group = &input->groups[0];
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root = scene_action_root(scene, "prototype.source", &doc);
    yyjson_mut_obj_add_strcpy(doc, root, "prototype", input->name);
    yyjson_mut_obj_add_strcpy(doc, root, "path", path);
    yyjson_mut_obj_add_uint(doc, root, "base_revision", collab_revision());
    yyjson_mut_obj_add_strcpy(doc, root, "before", input->base);
    yyjson_mut_obj_add_strcpy(doc, root, "after", group->text);
    yyjson_mut_obj_add_strcpy(doc, root, "input_kind", group->kind);
    yyjson_mut_obj_add_strcpy(doc, root, "input_key", group->key);
    yyjson_mut_obj_add_strcpy(doc, root, "input_group", group->group);
    yyjson_mut_obj_add_int(doc, root, "history", group->history);
    yyjson_mut_obj_add_bool(doc, root, "live", group->live);
    yyjson_mut_obj_add_bool(doc, root, "coalesce", group->coalesce);
    return scene_action_write(doc);
}

bool scene_source_input_apply(app_scene_authoring *scene, yyjson_val *root, char *error, size_t size)
{
    if (collab_active() || collab_replaying()) {
        char *encoded = yyjson_val_write(root, 0, NULL);
        bool bounded = encoded && strlen(encoded) <= COLLAB_MAX_ACTION;
        free(encoded);
        if (!bounded) {
            return fail(error, size, "Prototype source action exceeds the room action limit");
        }
    }
    const char *name = json_text(root, "prototype"),
               *path = json_text(root, "path"), *before = json_text(root, "before"),
               *after = json_text(root, "after"),
               *kind = json_text(root, "input_kind"),
               *key = json_text(root, "input_key"),
               *group = json_text(root, "input_group");
    yyjson_val *revision = yyjson_obj_get(root, "base_revision"),
               *history_value = yyjson_obj_get(root, "history"),
               *live_value = yyjson_obj_get(root, "live"),
               *coalesce_value = yyjson_obj_get(root, "coalesce");
    if (!name || !path || !before || !after || !kind || !key || !group ||
        !yyjson_is_uint(revision) || yyjson_get_uint(revision) != collab_revision() ||
        !yyjson_is_int(history_value) || !yyjson_is_bool(live_value) ||
        !yyjson_is_bool(coalesce_value) || !*group || strlen(group) >= 96 ||
        strlen(key) >= 192 ||
        (strcmp(kind, "raw") && strcmp(kind, "properties") &&
         strcmp(kind, "format") && strcmp(kind, "disk") && strcmp(kind, "reload"))) {
        return fail(error, size, "Prototype source action is malformed or stale");
    }
    int64_t history_int = yyjson_get_sint(history_value);
    if ((yyjson_is_uint(history_value) && yyjson_get_uint(history_value) > 1) ||
        history_int < -1 || history_int > 1 || (history_int && strcmp(kind, "raw"))) {
        return fail(error, size, "Prototype source history intent is invalid");
    }
    int history = (int)history_int;
    scene_prototype_asset *asset = find_prototype(scene, name);
    const char *expected_path = asset && asset->path
                                  ? relative_path(scene, asset->path)
                                  : NULL;
    if (!asset || !scene_prototype_source_owned(asset) || !expected_path ||
        strcmp(path, expected_path)) {
        return fail(error, size, "Prototype source target no longer matches");
    }
    bool private = asset->source == NULL;
    asset_code *code = private
                         ? scene_prototype_source_prepare(scene, asset, error, size)
                         : asset->source;
    if (!code || strcmp(before, asset_code_text(code))) {
        if (private) {
            asset_code_free(code);
        }
        return fail(error, size, "Prototype source changed before acceptance");
    }
    if (!strcmp(kind, "properties") || !strcmp(kind, "format")) {
        authoring_prototype_edit_result prepared = { 0 };
        bool valid = authoring_prototype_source_edit(
            after,
            &scene->registry,
            asset_code_catalog(code),
            NULL,
            0,
            &prepared,
            error,
            size
        );
        if (valid && strcmp(prepared.value->name, name)) {
            valid = fail(error, size, "Prototype source identity changed");
        }
        authoring_prototype_edit_result_free(&prepared);
        if (!valid) {
            if (private) {
                asset_code_free(code);
            }
            return false;
        }
    }
    const char *origin = collab_replaying() ? collab_apply_client() : collab_client();
    bool own = !collab_replaying() || !strcmp(origin, collab_client());
    asset_code_provenance provenance = { .origin = *origin ? origin : "local",
                                         .kind = kind,
                                         .key = key,
                                         .group = group,
                                         .coalesce = yyjson_get_bool(coalesce_value) };
    if (!asset_code_source_commit(code, before, after, &provenance, own ? history : (history ? 2 : 0))) {
        if (private) {
            asset_code_free(code);
        }
        return fail(error, size, "Prototype source history destination changed before acceptance");
    }
    if (private) {
        scene_prototype_source_adopt_session(scene, asset, code);
    }
    bool live = yyjson_get_bool(live_value);
    if (!live && strcmp(before, after)) {
        asset_code_hold_runtime(code, true);
    }
    asset_code_validate(code, NULL);
    (void)scene_prototype_source_sync(scene, asset, live);
    scene_source_input *input = find_input(scene, asset->path);
    if (own && input && acount(input->groups)) {
        source_input_group *pending = &input->groups[0];
        if (pending->disk_stage && !strcmp(pending->kind, kind) &&
            !strcmp(pending->key, key) && !strcmp(pending->group, group) &&
            !strcmp(pending->text, after) && !strcmp(input->base, before)) {
            /* The exact accepted action still owns this local disk stage. */
            if (!asset_code_accept_disk(code, &pending->disk, pending->reload)) {
                conflict(input, "Accepted source, but its local disk baseline changed; the draft is retained.");
            }
            pending->disk_stage = false;
        }
    }
    /* Invalid raw text is an accepted draft; the previous model/overlay stays. */
    scene_prototype_source_capture(scene, asset);
    return true;
}

static void retire_first(app_scene_authoring *scene, scene_source_input *input)
{
    scene_prototype_asset *asset = input_asset(scene, input);
    if (!asset || !asset->source || !acount(input->groups)) {
        conflict(input, "The source changed identity while its edit was pending; your draft is retained.");
        return;
    }
    source_input_group *group = &input->groups[0];
    char *accepted = xstrdup(group->text);
    group_free(group);
    for (int i = 1; i < acount(input->groups); i++) {
        input->groups[i - 1] = input->groups[i];
    }
    apop(input->groups);
    free(input->base);
    input->base = accepted;
    free(input->sent_before);
    input->sent_before = NULL;
    input->waiting = false;
    memset(&input->sent, 0, sizeof input->sent);
    if (!acount(input->groups) && !input->conflict) {
        input_clean(input);
    }
    show_input(scene, input);
}

static int submit(app_scene_authoring *scene, scene_source_input *input)
{
    if (input->waiting || input->conflict || !acount(input->groups)) {
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    scene_prototype_asset *asset = input_asset(scene, input);
    if (!asset || !asset->source ||
        strcmp(input->base, asset_code_text(asset->source))) {
        conflict(input, "The shared source changed; review it before Retry Draft or Use Shared.");
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    if (collab_active() && !collab_can_edit()) {
        if (collab_role_current() == COLLAB_ROLE_VIEWER ||
            collab_state_current() != COLLAB_READY) {
            conflict(input, "Room editing is unavailable; your local draft is retained.");
        }
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    char *json = input_action(scene, input);
    if (!json || (collab_active() && strlen(json) > COLLAB_MAX_ACTION)) {
        free(json);
        conflict(input, "This source edit exceeds the room action limit; your local draft is retained.");
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    collab_action_gate gate = scene_action_begin(scene, json);
    if (gate == COLLAB_ACTION_PENDING) {
        input->waiting = collab_pending_action(&input->sent);
        if (!input->waiting) {
            conflict(input, "The source proposal identity is unavailable; your draft is retained.");
        }
        input->sent_before = xstrdup(input->base);
        free(json);
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    if (gate != COLLAB_ACTION_APPLY) {
        free(json);
        conflict(input, "Room editing rejected this proposal; your local draft is retained.");
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    char error[256] = "";
    bool accepted = scene_source_input_apply(scene, yyjson_doc_get_root(doc), error, sizeof error);
    yyjson_doc_free(doc);
    bool published = scene_action_finish(scene, json, accepted);
    free(json);
    if (!accepted) {
        conflict(input, error);
        return -1;
    }
    int result = asset->source_not_applied ? APP_PROTOTYPE_EDIT_NOT_APPLIED : 0;
    if (collab_active()) {
        input->waiting = published && collab_pending_action(&input->sent);
        if (!input->waiting) {
            conflict(input, "The accepted local edit could not be published; your draft is retained.");
        }
        input->sent_before = xstrdup(input->base);
    } else {
        retire_first(scene, input);
    }
    return result;
}

static void reconcile(app_scene_authoring *scene, scene_source_input *input)
{
    if (!input->draft) {
        return;
    }
    scene_prototype_asset *asset = input_asset(scene, input);
    if (!asset || !asset->source) {
        conflict(input, "The shared source is unavailable or changed identity; your local draft is retained.");
        return;
    }
    const char *canonical = asset_code_text(asset->source);
    if (input->waiting || input->conflict || !strcmp(input->base, canonical)) {
        return;
    }
    char *old_base = xstrdup(input->base), *new_base = xstrdup(canonical);
    for (int i = 0; i < acount(input->groups); i++) {
        source_input_group *group = &input->groups[i];
        char *merged = asset_code_merge_source(old_base, group->text, new_base);
        if (!merged || group->history || group->disk_stage) {
            free(merged);
            conflict(input, "Your draft overlaps newer shared source; choose Retry Draft or Use Shared.");
            break;
        }
        free(old_base);
        old_base = group->text;
        group->text = merged;
        free(new_base);
        new_base = xstrdup(merged);
    }
    if (!input->conflict) {
        free(input->base);
        input->base = xstrdup(canonical);
        free(input->draft);
        input->draft = xstrdup(new_base);
        prepare_preview(scene, input);
        show_input(scene, input);
    }
    free(old_base);
    free(new_base);
}

static int enqueue_mode(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    const char *text,
    const char *kind,
    const char *key,
    bool coalesce,
    int history,
    bool live,
    asset_code_stage *disk,
    bool reload,
    bool defer
)
{
    const char *current = scene_source_input_text(scene, asset);
    if (!history && !disk && !strcmp(current, text)) {
        return scene_source_input_blocked(scene, asset->path) ? APP_PROTOTYPE_EDIT_PENDING
             : asset->source_not_applied ? APP_PROTOTYPE_EDIT_NOT_APPLIED
                                         : 0;
    }
    scene_source_input *input = ensure_input(scene, asset);
    if (!input) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "A retained draft belongs to the prior source identity; resolve it first.");
        return -1;
    }
    if (!input->base) {
        input->base = xstrdup(asset_code_text(asset->source));
    }
    free(input->draft);
    input->draft = xstrdup(text);
    prepare_preview(scene, input);
    if (input->conflict) {
        show_input(scene, input);
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    bool new_group = !coalesce || !*input->active_group ||
                     strcmp(input->active_kind, kind) ||
                     strcmp(input->active_key, key);
    if (new_group) {
        snprintf(
            input->active_group,
            sizeof input->active_group,
            "%llu:%llu",
            (unsigned long long)collab_revision(),
            (unsigned long long)++scene->source_input_generation
        );
        snprintf(input->active_kind, sizeof input->active_kind, "%s", kind);
        snprintf(input->active_key, sizeof input->active_key, "%s", key);
    }
    int count = acount(input->groups);
    bool amend = count > (input->waiting ? 1 : 0) && !history && !disk &&
                 !strcmp(input->groups[count - 1].group, input->active_group);
    bool group_coalesce = amend ? input->groups[count - 1].coalesce
                                : coalesce && !new_group;
    size_t bytes = 0;
    for (int i = input->waiting ? 1 : 0; i < count - (amend ? 1 : 0); i++) {
        bytes += strlen(input->groups[i].text);
    }
    if ((!amend && count - (input->waiting ? 1 : 0) >= SOURCE_INPUT_GROUPS) ||
        bytes > SOURCE_INPUT_BYTES) {
        /* Preserve the exact current document; no truncated group is sent. */
        if (!input->waiting) {
            groups_clear(input);
        }
        conflict(input, "The pending input limit was reached. Retry Draft explicitly submits the retained text as one edit.");
        show_input(scene, input);
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    if (amend) {
        group_free(&input->groups[count - 1]);
        apop(input->groups);
    }
    source_input_group value = { .text = xstrdup(text),
                                 .kind = xstrdup(kind),
                                 .key = xstrdup(key),
                                 .group = xstrdup(input->active_group),
                                 .history = history,
                                 .live = live,
                                 .coalesce = group_coalesce,
                                 .reload = reload };
    if (disk) {
        value.disk = *disk;
        memset(disk, 0, sizeof *disk);
        value.disk_stage = true;
    }
    apush(input->groups, value);
    int result = defer ? APP_PROTOTYPE_EDIT_PENDING : submit(scene, input);
    show_input(scene, input);
    return result;
}

static int enqueue(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    const char *text,
    const char *kind,
    const char *key,
    bool coalesce,
    int history,
    bool live,
    asset_code_stage *disk,
    bool reload
)
{
    return enqueue_mode(scene, asset, text, kind, key, coalesce, history, live, disk, reload, false);
}

void scene_source_input_harvest(app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->prototypes); i++) {
        scene_prototype_asset *asset = &scene->prototypes[i];
        char *before = NULL, *text = NULL;
        if (!asset->source ||
            !asset_code_take_unreported(asset->source, &before, &text)) {
            continue;
        }
        const char *current = scene_source_input_text(scene, asset);
        char *merged = asset_code_merge_source(before, text, current);
        if (!merged) {
            scene_source_input *input = ensure_input(scene, asset);
            if (input) {
                conflict(input, "Your latest typing overlaps a source replacement; the local draft is retained.");
            }
        }
        (void)enqueue_mode(
            scene,
            asset,
            merged ? merged : text,
            "raw",
            "typing",
            false,
            0,
            !asset_code_runtime_held(asset->source),
            NULL,
            false,
            true
        );
        free(merged);
        free(before);
        free(text);
    }
}

int scene_source_input_replace(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    const char *before,
    const char *after,
    bool coalesce,
    bool live
)
{
    if (!asset || !asset->source || !before || !after) {
        return -1;
    }
    const char *current = scene_source_input_text(scene, asset);
    if (!strcmp(before, current)) {
        return enqueue(scene, asset, after, "raw", "typing", coalesce, 0, live, NULL, false);
    }
    char *merged = asset_code_merge_source(before, after, current);
    if (merged) {
        int result = enqueue(scene, asset, merged, "raw", "typing", coalesce, 0, live, NULL, false);
        free(merged);
        return result;
    }
    scene_source_input *input = ensure_input(scene, asset);
    if (!input) {
        return -1;
    }
    conflict(input, "Your latest typing overlaps newer shared source; choose Retry Draft or Use Shared.");
    return enqueue(scene, asset, after, "raw", "typing", false, 0, live, NULL, false);
}

int scene_source_input_text_edit(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    const char *text,
    bool coalesce,
    bool live
)
{
    if (!asset || !asset->source || !text) {
        return -1;
    }
    return enqueue(scene, asset, text, "raw", "typing", coalesce, 0, live, NULL, false);
}

int scene_source_input_edit(
    app_scene_authoring *scene,
    scene_prototype_asset *asset,
    const authoring_prototype_edit *ops,
    int count,
    const char *key,
    bool coalesce
)
{
    if (!asset || !scene_prototype_source_owned(asset)) {
        return -1;
    }
    scene_source_input_harvest(scene);
    char error[256] = "";
    bool private = asset->source == NULL;
    asset_code *code = private
                         ? scene_prototype_source_prepare(scene, asset, error, sizeof error)
                         : asset->source;
    scene_source_input *input = find_input(scene, asset->path);
    const char *before = input && input->draft ? input->draft
                       : code                  ? asset_code_text(code)
                                               : NULL;
    authoring_prototype_edit_result candidate = { 0 };
    if (!code ||
        !authoring_prototype_source_edit(
            before,
            &scene->registry,
            asset_code_catalog(code),
            ops,
            count,
            &candidate,
            error,
            sizeof error
        ) ||
        strcmp(candidate.value->name, asset->prototype->name)) {
        snprintf(
            scene->prototype_message,
            sizeof scene->prototype_message,
            "%s",
            *error ? error : "Prototype source identity changed"
        );
        authoring_prototype_edit_result_free(&candidate);
        if (private) {
            asset_code_free(code);
        }
        return -1;
    }
    if (private) {
        scene_prototype_source_adopt_session(scene, asset, code);
    }
    int result = enqueue(scene, asset, candidate.source, "properties", key, coalesce, 0, true, NULL, false);
    authoring_prototype_edit_result_free(&candidate);
    return result;
}

int scene_source_input_history(app_scene_authoring *scene, scene_prototype_asset *asset, bool redo)
{
    scene_source_input_harvest(scene);
    if (scene_source_input_blocked(scene, asset->path)) {
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    char *target = asset_code_history_target(asset->source, redo);
    if (!target) {
        return -1;
    }
    int result = enqueue(
        scene,
        asset,
        target,
        "raw",
        redo ? "redo" : "undo",
        false,
        redo ? 1 : -1,
        !asset_code_runtime_held(asset->source),
        NULL,
        false
    );
    free(target);
    return result;
}

void scene_source_input_tick(app_scene_authoring *scene)
{
    collab_action_outcome outcome = { 0 };
    bool have = collab_latest_outcome(&outcome);
    for (int i = 0; i < acount(scene->source_inputs); i++) {
        scene_source_input *input = &scene->source_inputs[i];
        if (input->waiting) {
            if (have && outcome.serial > input->observed &&
                same_action(&input->sent, &outcome.action)) {
                input->observed = outcome.serial;
                if (outcome.kind == COLLAB_ACTION_ACCEPTED) {
                    retire_first(scene, input);
                } else {
                    conflict(input, outcome.message);
                    if (outcome.kind == COLLAB_ACTION_REJECTED) {
                        input->waiting = false;
                    }
                }
            } else {
                collab_action_identity pending = { 0 };
                if (!collab_pending_action(&pending) ||
                    !same_action(&input->sent, &pending)) {
                    input->waiting = false;
                    conflict(input, "The proposal outcome was interrupted; your local draft is retained.");
                }
            }
        }
        reconcile(scene, input);
        if (input->draft && !input->waiting && !input->conflict) {
            (void)submit(scene, input);
        }
        scene_prototype_asset *asset = input_asset(scene, input);
        if (asset && asset->source &&
            asset_code_local_dirty(asset->source) != (input->draft != NULL)) {
            show_input(scene, input);
        }
    }
    for (int i = 0; i < acount(scene->prototypes); i++) {
        scene_prototype_asset *asset = &scene->prototypes[i];
        if (!asset->source || !scene_prototype_source_owned(asset) ||
            scene_source_input_blocked(scene, asset->path)) {
            continue;
        }
        asset_code_stage disk = { 0 };
        char error[256] = "";
        if (asset_code_prepare_refresh(asset->source, &disk, error, sizeof error) &&
            disk.text) {
            (void)enqueue(
                scene,
                asset,
                disk.text,
                "disk",
                "disk",
                false,
                0,
                !asset_code_runtime_held(asset->source),
                &disk,
                false
            );
        }
        asset_code_stage_free(&disk);
    }
}

const char *scene_source_input_conflict(app_scene_authoring *scene, char *message, size_t size)
{
    for (int i = 0; i < acount(scene->source_inputs); i++) {
        scene_source_input *input = &scene->source_inputs[i];
        if (input->draft && input->conflict) {
            snprintf(
                message,
                size,
                "%.150s Shared revision %llu. Your local draft is retained.",
                input->message,
                (unsigned long long)collab_revision()
            );
            return input->path;
        }
    }
    return NULL;
}

static scene_source_input *recovery_path(app_scene_authoring *scene, const char *path)
{
    for (int i = 0; i < acount(scene->source_inputs); i++) {
        scene_source_input *input = &scene->source_inputs[i];
        if (input->draft && !strcmp(input->path, path)) {
            return input;
        }
    }
    return NULL;
}

uint64_t scene_source_input_conflict_identity(app_scene_authoring *scene)
{
    for (int i = 0; i < acount(scene->source_inputs); i++) {
        if (scene->source_inputs[i].draft && scene->source_inputs[i].conflict) {
            return scene->source_inputs[i].identity;
        }
    }
    return 0;
}

static scene_source_input *recovery_identity(app_scene_authoring *scene, uint64_t identity)
{
    for (int i = 0; i < acount(scene->source_inputs); i++) {
        if (scene->source_inputs[i].identity == identity) {
            return &scene->source_inputs[i];
        }
    }
    return NULL;
}

int scene_source_input_retry_identity(app_scene_authoring *scene, uint64_t identity)
{
    scene_source_input_harvest(scene);
    scene_source_input_tick(scene);
    scene_source_input *input = recovery_identity(scene, identity);
    if (!input || !input->draft || input->waiting) {
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    scene_prototype_asset *asset = input_asset(scene, input);
    if (!asset || !asset->source) {
        return -1;
    }
    char *draft = xstrdup(input->draft);
    input_clean(input);
    input->active_group[0] = 0;
    int result = enqueue(
        scene,
        asset,
        draft,
        "raw",
        "retry",
        false,
        0,
        !asset_code_runtime_held(asset->source),
        NULL,
        false
    );
    free(draft);
    return result;
}

bool scene_source_input_use_shared_identity(app_scene_authoring *scene, uint64_t identity)
{
    scene_source_input_harvest(scene);
    scene_source_input_tick(scene);
    scene_source_input *input = recovery_identity(scene, identity);
    if (!input || input->waiting) {
        return false;
    }
    input_clean(input);
    input->active_group[0] = 0;
    show_input(scene, input);
    return true;
}

int scene_source_input_retry(app_scene_authoring *scene, const char *path)
{
    scene_source_input *input = recovery_path(scene, path);
    return input ? scene_source_input_retry_identity(scene, input->identity) : -1;
}

bool scene_source_input_use_shared(app_scene_authoring *scene, const char *path)
{
    scene_source_input *input = recovery_path(scene, path);
    return input && scene_source_input_use_shared_identity(scene, input->identity);
}

int scene_source_input_save(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    scene_source_input_harvest(scene);
    if (scene_source_input_blocked(scene, asset->path)) {
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    asset_code_stage stage = { 0 };
    char error[256] = "";
    if (!asset_code_prepare_save(asset->source, &stage, error, sizeof error)) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "%s", error);
        return -1;
    }
    if (strcmp(stage.before, stage.text)) {
        int result = enqueue(scene, asset, stage.text, "format", "save", false, 0, true, NULL, false);
        asset_code_stage_free(&stage);
        if (result < 0 || scene_source_input_blocked(scene, asset->path)) {
            return result < 0 ? result : APP_PROTOTYPE_EDIT_PENDING;
        }
    } else {
        asset_code_stage_free(&stage);
    }
    if (!asset_code_save_committed(asset->source, error, sizeof error)) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "%s", error);
        return -1;
    }
    return 0;
}

int scene_source_input_reload(app_scene_authoring *scene, scene_prototype_asset *asset)
{
    scene_source_input_harvest(scene);
    if (scene_source_input_blocked(scene, asset->path)) {
        return APP_PROTOTYPE_EDIT_PENDING;
    }
    char error[256] = "";
    asset_code_stage stage = { 0 };
    if (!asset_code_prepare_reload(asset->source, &stage, error, sizeof error)) {
        snprintf(scene->prototype_message, sizeof scene->prototype_message, "%s", error);
        return -1;
    }
    int result = enqueue(scene, asset, stage.text, "reload", "reload", false, 0, true, &stage, true);
    asset_code_stage_free(&stage);
    return result;
}
