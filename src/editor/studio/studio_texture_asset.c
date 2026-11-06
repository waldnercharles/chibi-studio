#include "editor/studio/studio_texture_asset.h"

#include "authoring/image_import_commit.h"
#include "editor/documents/asset_code.h"
#include "editor/studio/studio_internal.h"
#include "runtime/images/catalog.h"
#include "support/array.h"
#include "support/xalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char *key;
    asset_code *code;
    uint64_t revision;
    char status[512];
} texture_document;

struct studio_texture_asset
{
    char *content_root, *document_id, *graph_document_id;
    app_editor_document_kind document_kind, graph_document_kind;
    texture_document *documents;
    uint64_t revision;
    int active;
};

static studio_texture_asset *current(const studio *s)
{
    studio_texture_asset *asset = s->texture_asset;
    return asset && !strcmp(asset->content_root, s->scene->content_root) ? asset : NULL;
}

static texture_document *selected(const studio *s)
{
    studio_texture_asset *asset = current(s);
    if (asset && asset->active >= 0 &&
        (asset->document_kind != s->scene->document.kind ||
         asset->graph_document_kind != s->scene->graph_document_kind ||
         strcmp(asset->document_id, s->scene->document_id ? s->scene->document_id : "") ||
         strcmp(asset->graph_document_id, s->scene->graph_document_id ? s->scene->graph_document_id : ""))) {
        asset->active = -1;
        asset->revision++;
    }
    return asset && asset->active >= 0 ? &asset->documents[asset->active] : NULL;
}

uint64_t studio_texture_revision(const studio *s)
{
    texture_document *document = selected(s);
    studio_texture_asset *asset = current(s);
    return asset ? (asset->revision * 1099511628211ull) ^
                       (document ? document->revision + 1 : 0)
                 : 0;
}

bool studio_texture_active(const studio *s)
{
    return selected(s) != NULL;
}

const char *studio_texture_key(const studio *s)
{
    texture_document *document = selected(s);
    return document ? document->key : NULL;
}

void studio_texture_close(studio *s)
{
    if (s->texture_asset && s->texture_asset->active >= 0) {
        s->texture_asset->active = -1;
        s->texture_asset->revision++;
    }
}

void studio_texture_asset_free(studio_texture_asset *asset)
{
    if (!asset) {
        return;
    }
    for (int i = 0; i < acount(asset->documents); i++) {
        free(asset->documents[i].key);
        asset_code_free(asset->documents[i].code);
    }
    afree(asset->documents);
    free(asset->content_root);
    free(asset->document_id);
    free(asset->graph_document_id);
    free(asset);
}

bool studio_texture_open(studio *s, const char *key)
{
    if (!key || !*key) {
        studio_texture_close(s);
        return false;
    }
    if (s->texture_asset && !current(s)) {
        studio_texture_asset_free(s->texture_asset);
        s->texture_asset = NULL;
    }
    if (!s->texture_asset) {
        s->texture_asset = xcalloc(1, sizeof *s->texture_asset);
        s->texture_asset->content_root = xstrdup(s->scene->content_root);
        s->texture_asset->active = -1;
    }
    studio_texture_asset *asset = s->texture_asset;
    asset->revision++;
    free(asset->document_id);
    free(asset->graph_document_id);
    asset->document_id = xstrdup(s->scene->document_id ? s->scene->document_id : "");
    asset->graph_document_id = xstrdup(
        s->scene->graph_document_id ? s->scene->graph_document_id : ""
    );
    asset->document_kind = s->scene->document.kind;
    asset->graph_document_kind = s->scene->graph_document_kind;
    int existing = -1;
    for (int i = 0; i < acount(asset->documents); i++) {
        if (!strcmp(asset->documents[i].key, key)) {
            existing = asset->active = i;
            asset_code *code = asset->documents[i].code;
            if (code) {
                if (!asset_code_dirty(code)) {
                    asset_code_refresh(code);
                }
                return true;
            }
            break;
        }
    }
    texture_document document = { .key = xstrdup(key) };
    image_import_texture texture = { 0 };
    /* The SDK validates the virtual key and sidecar before a document opens. */
    if (image_import_texture_read(
            s->scene->content_root,
            key,
            &texture,
            document.status,
            sizeof document.status
        )) {
        size_t size = strlen(s->scene->content_root) + strlen(key) +
                      sizeof IMAGE_IMPORT_TEXTURE_SUFFIX + 2;
        char *path = xmalloc(size);
        snprintf(path, size, "%s/%s%s", s->scene->content_root, key + 8, IMAGE_IMPORT_TEXTURE_SUFFIX);
        document.code = asset_code_open(path, document.status, sizeof document.status);
        free(path);
    }
    image_import_texture_free(&texture);
    if (existing >= 0) {
        free(asset->documents[existing].key);
        asset->documents[existing] = document;
    } else {
        apush(asset->documents, document);
        asset->active = acount(asset->documents) - 1;
    }
    return document.code != NULL;
}

bool studio_texture_dirty(const studio *s)
{
    texture_document *document = selected(s);
    return document && document->code && asset_code_dirty(document->code);
}

bool studio_texture_unsaved(const studio *s)
{
    studio_texture_asset *asset = current(s);
    for (int i = 0; asset && i < acount(asset->documents); i++) {
        if (asset->documents[i].code && asset_code_dirty(asset->documents[i].code)) {
            return true;
        }
    }
    return false;
}

bool studio_texture_history(studio *s, bool redo, bool apply)
{
    texture_document *document = selected(s);
    if (!document || !document->code || !asset_code_can_replay(document->code, redo)) {
        return false;
    }
    if (!apply) {
        return true;
    }
    if (!asset_code_replay(document->code, redo, false)) {
        return false;
    }
    document->status[0] = 0;
    document->revision++;
    return true;
}

static bool parse(texture_document *document, image_import_texture *texture)
{
    if (!document->code) {
        return false;
    }
    char error[512];
    if (!image_import_texture_parse(asset_code_text(document->code), texture, error, sizeof error)) {
        if (strcmp(document->status, error)) {
            snprintf(document->status, sizeof document->status, "%s", error);
            document->revision++;
        }
        return false;
    }
    return true;
}

static bool save_document(texture_document *document)
{
    document->revision++;
    image_import_texture texture = { 0 };
    if (!parse(document, &texture)) {
        return false;
    }
    image_import_texture_free(&texture);
    if (!asset_code_save(document->code, document->status, sizeof document->status)) {
        return false;
    }
    snprintf(document->status, sizeof document->status, "Settings saved. Reimport to update the texture.");
    return true;
}

bool studio_texture_save(studio *s)
{
    texture_document *document = selected(s);
    if (!document) {
        return false;
    }
    bool saved = save_document(document);
    if (saved) {
        studio_saved(s, "Texture settings saved", document->key);
    } else {
        studio_error(s, "Could not save texture settings", document->status);
    }
    return saved;
}

bool studio_texture_save_all(studio *s)
{
    studio_texture_asset *asset = current(s);
    bool ok = true;
    for (int i = 0; asset && i < acount(asset->documents); i++) {
        texture_document *document = &asset->documents[i];
        if (document->code && asset_code_dirty(document->code) &&
            !save_document(document)) {
            studio_error(s, "Could not save texture settings", document->status);
            ok = false;
        }
    }
    return ok;
}

static bool settings(texture_document *document, const char *resolution, const char *semantic)
{
    document->revision++;
    image_import_texture texture = { 0 };
    if (!parse(document, &texture)) {
        return false;
    }
    bool valid = true;
    if (!strcmp(resolution, "source")) {
        texture.options.max_side = 0;
    } else if (!strcmp(resolution, "1k")) {
        texture.options.max_side = 1024;
    } else if (!strcmp(resolution, "2k")) {
        texture.options.max_side = 2048;
    } else {
        valid = false;
    }
    if (!strcmp(semantic, "color")) {
        texture.options.semantic = IMAGE_IMPORT_COLOR;
    } else if (!strcmp(semantic, "scalar")) {
        texture.options.semantic = IMAGE_IMPORT_SCALAR;
    } else if (!strcmp(semantic, "normal")) {
        texture.options.semantic = IMAGE_IMPORT_NORMAL;
    } else {
        valid = false;
    }
    if (!valid) {
        image_import_texture_free(&texture);
        snprintf(document->status, sizeof document->status, "Choose a resolution and image semantic.");
        return false;
    }
    char *next = image_import_texture_serialize(
        &texture,
        document->status,
        sizeof document->status
    );
    image_import_texture_free(&texture);
    if (!next) {
        return false;
    }
    const char *before = asset_code_text(document->code);
    bool ok = !strcmp(before, next) ||
              asset_code_replace(document->code, before, next);
    free(next);
    if (ok) {
        document->status[0] = 0;
    }
    return ok;
}

static bool measure(
    void *context,
    const image_import_proposal *plans,
    int count,
    authoring_visual_budget *budget,
    char *error,
    size_t size
)
{
    (void)context;
    return authoring_visual_measure(NULL, 0, plans, count, budget, error, size);
}

static void reimport(studio *s, texture_document *document)
{
    document->revision++;
    if (!studio_editable(s)) {
        snprintf(document->status, sizeof document->status, "Stop the active session before reimporting.");
        return;
    }
    if (!save_document(document)) {
        return;
    }
    image_import_file_plan plan = { 0 };
    image_import_budget_host host = { .context = s, .measure = measure };
    if (image_import_file_reimport_prepare(
            s->scene->content_root,
            document->key,
            host,
            &plan,
            document->status,
            sizeof document->status
        ) &&
        image_import_file_commit(
            s->scene->content_root,
            &plan,
            host,
            document->status,
            sizeof document->status
        )) {
        char error[384];
        bool reloaded = render_image_reload(document->key, error, sizeof error);
        app_scene_authoring_refresh_content(s->scene);
        if (reloaded) {
            snprintf(
                document->status,
                sizeof document->status,
                "Reimported %s",
                document->key
            );
        } else {
            snprintf(document->status, sizeof document->status, "Reimported on disk; preview reload failed: %s", error);
        }
    }
    image_import_file_plan_free(&plan);
}

static pin_value text(arena *storage, const char *value)
{
    char *copy = arena_alloc(storage, strlen(value) + 1);
    strcpy(copy, value);
    return (pin_value){ .type = PIN_STR, .str = copy };
}

const char *studio_texture_asset_run(studio *s, const pin_value *in, arena *storage, pin_value *out)
{
    texture_document *document = selected(s);
    const char *operation = in[0].str ? in[0].str : "read";
    if (document) {
        if (!strcmp(operation, "edit")) {
            settings(document, in[1].str ? in[1].str : "", in[2].str ? in[2].str : "");
        } else if (!strcmp(operation, "undo")) {
            studio_texture_history(s, false, true);
        } else if (!strcmp(operation, "redo")) {
            studio_texture_history(s, true, true);
        } else if (!strcmp(operation, "save")) {
            studio_texture_save(s);
        } else if (!strcmp(operation, "reimport")) {
            reimport(s, document);
        } else if (strcmp(operation, "read")) {
            document->revision++;
            snprintf(document->status, sizeof document->status, "Unknown texture settings operation.");
        }
    }
    image_import_texture texture = { 0 };
    bool valid = document && parse(document, &texture);
    const char *resolution = texture.options.max_side == 1024 ? "1k"
                           : texture.options.max_side == 2048 ? "2k"
                                                              : "source";
    const char *semantic = texture.options.semantic == IMAGE_IMPORT_NORMAL
                             ? "normal"
                         : texture.options.semantic == IMAGE_IMPORT_SCALAR
                             ? "scalar"
                             : "color";
    const char *source = valid ? texture.source : "";
    char info[128] = "";
    int width, height;
    if (document && render_image_info(document->key, &width, &height)) {
        snprintf(info, sizeof info, "%d × %d", width, height);
    }
    out[0] = (pin_value){ .type = PIN_BOOL, .b = document != NULL };
    out[1] = text(storage, document ? document->key : "");
    out[2] = text(storage, source);
    out[3] = text(storage, resolution);
    out[4] = text(storage, semantic);
    out[5] = (pin_value){ .type = PIN_BOOL, .b = studio_texture_dirty(s) };
    out[6] = text(storage, document ? document->status : "");
    out[7] = text(storage, info);
    image_import_texture_free(&texture);
    return NULL;
}
