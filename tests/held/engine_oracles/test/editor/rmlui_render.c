#include "editor/ui/kit.h"
#include "platform/cute.h"
#include "test/suite_internal.h"
#include "ui/rml.h"
#include "ui/rml_backend.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static bool has_magenta(CF_Canvas canvas)
{
    CF_Readback readback = cf_canvas_readback(canvas);
    assert(readback.id);
    int size = cf_readback_size(readback);
    assert(size > 0 && size % (int)sizeof(CF_Pixel) == 0);
    CF_Pixel *pixels = malloc((size_t)size);
    assert(pixels);
    uint64_t start = cf_get_ticks();
    while (!cf_readback_ready(readback)) {
        assert(cf_get_ticks() - start < cf_get_tick_frequency() * 10);
        cf_sleep(1);
    }
    assert(cf_readback_data(readback, pixels, size) == size);
    bool found = false;
    for (int i = 0; i < size / (int)sizeof(CF_Pixel); i++) {
        CF_Pixel p = pixels[i];
        if (p.colors.r > 220 && p.colors.b > 220 && p.colors.g < 40 && p.colors.a > 220) {
            found = true;
            break;
        }
    }
    free(pixels);
    cf_destroy_readback(readback);
    return found;
}

static void queue_quad(CF_Color color)
{
    cf_draw_push_layer(0);
    cf_draw_push_color(color);
    cf_draw_quad_fill2(
        (CF_V2){ -16, -16 },
        (CF_V2){ 16, -16 },
        (CF_V2){ 16, 16 },
        (CF_V2){ -16, 16 },
        0
    );
    cf_draw_pop_color();
    cf_draw_pop_layer();
}

static void queue_witness_geometry(void)
{
    queue_quad(cf_make_color_rgba(255, 0, 255, 255));
}

/* A retained UI scissors to its whole target, as a game's HUD does inside the
 * Scene view, so it must draw whatever scissor an earlier pass left behind. */
static void check_scissor_left_behind()
{
    enum
    {
        WIDTH = 96,
        HEIGHT = 64
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    rml_element *doc = rml_context_load_document_from_memory(
        rml_backend_context(ui),
        "<rml><body style=\"width: 100%; height: 100%; "
        "background-color: #ff00ff;\"></body></rml>",
        "scissor"
    );
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    CF_Canvas clipped = cf_make_canvas(cf_canvas_defaults(256, 256));
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    assert(clipped.id && target.id);
    cf_app_update(NULL);
    /* Clipped to a pixel outside the target, as the editor clips a glyph. */
    cf_draw_push_scissor((CF_Rect){ 200, 200, 1, 1 });
    queue_quad(cf_make_color_rgba(0, 255, 0, 255));
    cf_render_to(clipped, true);
    cf_draw_pop_scissor();
    /* Unclipped into a canvas of the UI's size, as the world renders. */
    queue_quad(cf_make_color_rgba(0, 255, 0, 255));
    cf_render_to(target, true);
    rml_backend_update(ui, 1.0 / 60.0);
    rml_backend_render(ui);
    cf_draw_push();
    rml_backend_present(ui);
    cf_render_to(target, false);
    cf_draw_pop();
    cf_app_draw_onto_screen(true);
    assert(has_magenta(target));
    cf_destroy_canvas(clipped);
    cf_destroy_canvas(target);
    rml_document_close(doc);
    rml_backend_destroy(ui);
}

/* Verify the public CSS effect on real rendered pixels, including its hole
   around an opaque control and the soft outer tail. Two frames also exercise
   cached callback textures and release of temporary shadow geometry. */
static void check_box_shadows()
{
    enum
    {
        WIDTH = 128,
        HEIGHT = 96
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    rml_element *doc = rml_context_load_document_from_memory(
        rml_backend_context(ui),
        "<rml><body style=\"width:100%;height:100%;\">"
        "<div "
        "style=\"position:absolute;left:24px;top:24px;width:24px;height:24px;"
        "background-color:#ffffff;box-shadow:0px 0px 0px 3px "
        "#ff000080;\"></div>"
        "<div "
        "style=\"position:absolute;left:80px;top:24px;width:24px;height:24px;"
        "background-color:#ffffff;box-shadow:0px 4px 8px 0px "
        "#0000ff80,0px 2px 4px 0px #0000ff40;\"></div>"
        "</body></rml>",
        "shadows"
    );
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    for (int frame = 0; frame < 2; frame++) {
        cf_app_update(NULL);
        rml_backend_update(ui, 1.0 / 60.0);
        rml_backend_render(ui);
        cf_draw_push();
        rml_backend_present(ui);
        cf_render_to(target, true);
        cf_draw_pop();
        cf_app_draw_onto_screen(true);
        CF_Readback readback = cf_canvas_readback(target);
        CF_Pixel pixels[WIDTH * HEIGHT];
        uint64_t start = cf_get_ticks();
        while (!cf_readback_ready(readback)) {
            assert(cf_get_ticks() - start < cf_get_tick_frequency() * 10);
            cf_sleep(1);
        }
        assert(cf_readback_data(readback, pixels, sizeof pixels) == sizeof pixels);
        cf_destroy_readback(readback);
        CF_Pixel ring = pixels[36 * WIDTH + 22];
        CF_Pixel center = pixels[36 * WIDTH + 36];
        CF_Pixel outside = pixels[36 * WIDTH + 19];
        CF_Pixel near = pixels[54 * WIDTH + 92];
        CF_Pixel far = pixels[60 * WIDTH + 92];
        assert(
            ring.colors.r > 220 && ring.colors.g < 20 && ring.colors.a > 100 &&
            ring.colors.a < 150
        );
        assert(
            center.colors.r > 240 && center.colors.g > 240 &&
            center.colors.b > 240 && center.colors.a > 240
        );
        assert(outside.colors.a < 5);
        assert(near.colors.b > 200 && near.colors.a > 15 && near.colors.a < 100);
        assert(far.colors.a < near.colors.a && far.colors.a < 15);
    }
    cf_destroy_canvas(target);
    rml_document_close(doc);
    rml_backend_destroy(ui);
}

static void read_pixels(CF_Canvas canvas, CF_Pixel *pixels, int size)
{
    CF_Readback readback = cf_canvas_readback(canvas);
    assert(readback.id && cf_readback_size(readback) == size);
    uint64_t start = cf_get_ticks();
    while (!cf_readback_ready(readback)) {
        assert(cf_get_ticks() - start < cf_get_tick_frequency() * 10);
        cf_sleep(1);
    }
    assert(cf_readback_data(readback, pixels, size) == size);
    cf_destroy_readback(readback);
}

static void render_pixels(rml_backend *ui, CF_Canvas target, CF_Pixel *pixels, int size)
{
    cf_app_update(NULL);
    rml_backend_update(ui, 1.0 / 60.0);
    /* A world pass can leave a nonidentity draw transform on entry. */
    cf_draw3d_push();
    cf_draw3d_translate(cf_v3(37, 19, 0));
    rml_backend_render(ui);
    cf_draw3d_pop();
    cf_draw_push();
    rml_backend_present(ui);
    cf_render_to(target, true);
    cf_draw_pop();
    cf_app_draw_onto_screen(true);
    read_pixels(target, pixels, size);
}

static void same_work(rml_backend_stats before, rml_backend_stats after)
{
    assert(after.geometry_compiles == before.geometry_compiles);
    assert(after.uploaded_bytes == before.uploaded_bytes);
    assert(after.render_passes == before.render_passes);
    assert(after.static_layer_updates == before.static_layer_updates);
    assert(after.static_partial_updates == before.static_partial_updates);
    assert(after.static_commands_replayed == before.static_commands_replayed);
    assert(after.output_updates == before.output_updates);
    assert(after.texture_composites == before.texture_composites);
    assert(after.cached_texture_bytes == before.cached_texture_bytes);
}

static rml_element *show_document(rml_backend *ui, const char *body, const char *name)
{
    char source[4096];
    snprintf(source, sizeof source, "<rml><body style=\"margin:0;width:100%%;height:100%%;\">%s</body></rml>", body);
    rml_element *doc = rml_context_load_document_from_memory(rml_backend_context(ui), source, name);
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    return doc;
}

static bool near_color(CF_Pixel p, int r, int g, int b, int a)
{
    return abs(p.colors.r - r) <= 24 && abs(p.colors.g - g) <= 24 &&
           abs(p.colors.b - b) <= 24 && abs(p.colors.a - a) <= 24;
}

/* Rounded overflow clips children at the corners, a host surface included,
   and a nested rounded container intersects its parent's mask. */
static void check_clip_masks()
{
    enum
    {
        WIDTH = 96,
        HEIGHT = 56
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    CF_Texture texture = cf_make_texture(cf_texture_defaults(1, 1));
    CF_Pixel red = { .colors = { .r = 255, .a = 255 } };
    cf_texture_update(texture, &red, sizeof red);
    rml_backend_set_texture_revision(ui, "clipped", texture.id, 1);
    rml_element *doc = show_document(
        ui,
        "<div "
        "style=\"position:absolute;left:8px;top:8px;width:32px;height:32px;"
        "border-radius:12px;overflow:hidden;\">"
        "<div "
        "style=\"display:block;width:32px;height:32px;background-color:#00ff00;"
        "\">"
        "</div></div>"
        "<div "
        "style=\"position:absolute;left:48px;top:8px;width:40px;height:40px;"
        "border-radius:16px;overflow:hidden;\">"
        "<img src=\"kit:clipped\" "
        "style=\"position:absolute;left:0;top:0;width:40px;"
        "height:40px;\"/>"
        "<div "
        "style=\"position:absolute;left:20px;top:20px;width:20px;height:20px;"
        "border-radius:8px;overflow:hidden;\">"
        "<div "
        "style=\"display:block;width:20px;height:20px;background-color:#0000ff;"
        "\">"
        "</div>"
        "</div></div>",
        "clip-masks"
    );
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    CF_Pixel pixels[WIDTH * HEIGHT];
    for (int frame = 0; frame < 2; frame++) {
        rml_backend_stats before = rml_backend_get_stats(ui);
        render_pixels(ui, target, pixels, sizeof pixels);
        if (frame) {
            same_work(before, rml_backend_get_stats(ui));
        }
        assert(pixels[9 * WIDTH + 9].colors.a < 5);
        assert(near_color(pixels[24 * WIDTH + 24], 0, 255, 0, 255));
        assert(near_color(pixels[24 * WIDTH + 9], 0, 255, 0, 255));
        /* The host surface's corner clips, while its middle shows. */
        assert(pixels[9 * WIDTH + 49].colors.a < 5);
        assert(near_color(pixels[20 * WIDTH + 60], 255, 0, 0, 255));
        /* Outside the nested corner the surface shows; outside both, nothing. */
        assert(near_color(pixels[29 * WIDTH + 69], 255, 0, 0, 255));
        assert(near_color(pixels[38 * WIDTH + 78], 0, 0, 255, 255));
        assert(near_color(pixels[38 * WIDTH + 82], 0, 0, 255, 255));
        assert(pixels[46 * WIDTH + 86].colors.a < 5);
    }
    cf_destroy_canvas(target);
    rml_document_close(doc);
    rml_backend_destroy(ui);
    cf_destroy_texture(texture);
}

/* Filters composite a layer through the GPU passes: blur spreads alpha past
   its source, at full size and downsampled, a drop shadow lands at its
   offset, opacity fades and grayscale mixes channels. */
static void check_filters()
{
    enum
    {
        WIDTH = 224,
        HEIGHT = 96
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    rml_element *doc = show_document(
        ui,
        "<div "
        "style=\"position:absolute;left:16px;top:16px;width:16px;height:16px;"
        "background-color:#ff0000;filter:blur(3px);\"></div>"
        "<div "
        "style=\"position:absolute;left:56px;top:16px;width:16px;height:16px;"
        "background-color:#ff0000;filter:drop-shadow(#0000ff 8px 8px "
        "0px);\"></div>"
        "<div "
        "style=\"position:absolute;left:96px;top:16px;width:16px;height:16px;"
        "background-color:#ff0000;filter:grayscale(1);\"></div>"
        "<div "
        "style=\"position:absolute;left:136px;top:16px;width:16px;height:16px;"
        "background-color:#ff0000;filter:opacity(0.5);\"></div>"
        "<div "
        "style=\"position:absolute;left:176px;top:40px;width:24px;height:24px;"
        "background-color:#ff0000;filter:blur(12px);\"></div>",
        "filters"
    );
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    CF_Pixel pixels[WIDTH * HEIGHT];
    for (int frame = 0; frame < 2; frame++) {
        rml_backend_stats before = rml_backend_get_stats(ui);
        render_pixels(ui, target, pixels, sizeof pixels);
        if (frame) {
            same_work(before, rml_backend_get_stats(ui));
        }
        CF_Pixel middle = pixels[24 * WIDTH + 24], spread = pixels[24 * WIDTH + 13];
        assert(middle.colors.r > 200 && middle.colors.a > 150);
        assert(
            spread.colors.r > 200 && spread.colors.a > 10 &&
            spread.colors.a < middle.colors.a
        );
        assert(pixels[24 * WIDTH + 3].colors.a < 5);
        assert(near_color(pixels[24 * WIDTH + 64], 255, 0, 0, 255));
        assert(near_color(pixels[36 * WIDTH + 76], 0, 0, 255, 255));
        assert(pixels[36 * WIDTH + 60].colors.a < 5);
        CF_Pixel gray = pixels[24 * WIDTH + 104];
        assert(gray.colors.a > 240 && abs(gray.colors.r - gray.colors.g) < 8);
        assert(gray.colors.r > 40 && gray.colors.r < 70);
        CF_Pixel faded = pixels[24 * WIDTH + 144];
        assert(faded.colors.r > 240 && faded.colors.a > 110 && faded.colors.a < 145);
        CF_Pixel wide = pixels[52 * WIDTH + 188], tail = pixels[52 * WIDTH + 166];
        assert(wide.colors.r > 200 && wide.colors.a > 60);
        assert(
            tail.colors.r > 200 && tail.colors.a > 5 &&
            tail.colors.a < wide.colors.a
        );
    }
    cf_destroy_canvas(target);
    rml_document_close(doc);
    rml_backend_destroy(ui);
}

/* A box-shadow renders its texture at the window origin, outside the cropped
   paint run beside a host surface, and the whole texture must survive. */
static void check_shadow_beside_surface()
{
    enum
    {
        WIDTH = 128,
        HEIGHT = 96
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    CF_Texture texture = cf_make_texture(cf_texture_defaults(1, 1));
    CF_Pixel red = { .colors = { .r = 255, .a = 255 } };
    cf_texture_update(texture, &red, sizeof red);
    rml_backend_set_texture_revision(ui, "beside", texture.id, 1);
    rml_element *doc = show_document(
        ui,
        "<img src=\"kit:beside\" "
        "style=\"position:absolute;left:0;top:0;width:16px;"
        "height:16px;\"/>"
        "<div "
        "style=\"position:absolute;left:72px;top:56px;width:24px;height:24px;"
        "background-color:#ffffff;box-shadow:0px 4px 8px 0px "
        "#0000ff80;\"></div>",
        "shadow-beside-surface"
    );
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    CF_Pixel pixels[WIDTH * HEIGHT];
    render_pixels(ui, target, pixels, sizeof pixels);
    assert(near_color(pixels[8 * WIDTH + 8], 255, 0, 0, 255));
    assert(near_color(pixels[57 * WIDTH + 73], 255, 255, 255, 255));
    assert(near_color(pixels[68 * WIDTH + 84], 255, 255, 255, 255));
    CF_Pixel shadow = pixels[84 * WIDTH + 84];
    assert(shadow.colors.b > 200 && shadow.colors.a > 15 && shadow.colors.a < 100);
    cf_destroy_canvas(target);
    rml_document_close(doc);
    rml_backend_destroy(ui);
    cf_destroy_texture(texture);
}

/* Studio's overlay elevation: a wide, downsampled black blur with negative
   spread darkens below the element and leaves the far field untouched. */
static void check_overlay_shadow()
{
    enum
    {
        WIDTH = 160,
        HEIGHT = 128
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    rml_element *doc = rml_context_load_document_from_memory(
        rml_backend_context(ui),
        "<rml><head><style>"
        "body { margin: 0; width: 100%; height: 100%; background-color: "
        "#ffffff; "
        "--lift: 0px 8px 24px -4px #00000073, 0px 1px 2px 0px #00000059; }"
        "div { position: absolute; left: 48px; top: 24px; width: 64px; height: "
        "48px; "
        "background-color: #ffffff; border-radius: 8px; box-shadow: "
        "var(--lift); }"
        "</style></head><body><div/></body></rml>",
        "overlay-shadow"
    );
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    CF_Pixel pixels[WIDTH * HEIGHT];
    render_pixels(ui, target, pixels, sizeof pixels);
    CF_Pixel below = pixels[80 * WIDTH + 80], far = pixels[120 * WIDTH + 4];
    CF_Pixel inside = pixels[48 * WIDTH + 80];
    assert(inside.colors.r > 250 && far.colors.r > 250);
    /* CSS draws 213 here: the blur keeps the light the box hides. */
    assert(
        below.colors.r > 200 && below.colors.r < 225 &&
        below.colors.r == below.colors.b
    );
    /* RmlUi keeps the shadow texture: the next frame drops the commands that
       made it, and from then on frames repeat no work. */
    render_pixels(ui, target, pixels, sizeof pixels);
    assert(pixels[80 * WIDTH + 80].colors.r == below.colors.r);
    rml_backend_stats settled = rml_backend_get_stats(ui);
    render_pixels(ui, target, pixels, sizeof pixels);
    same_work(settled, rml_backend_get_stats(ui));
    cf_destroy_canvas(target);
    rml_document_close(doc);
    rml_backend_destroy(ui);
}

/* Gradient decorators draw through the gradient program, and a gradient
   mask image fades an element through a saved mask layer. */
static void check_gradients()
{
    enum
    {
        WIDTH = 96,
        HEIGHT = 80
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    rml_element *doc = rml_context_load_document_from_memory(
        rml_backend_context(ui),
        "<rml><head><style>"
        "body { margin: 0; width: 100%; height: 100%; }"
        "div { position: absolute; }"
        "#linear { left: 8px; top: 8px; width: 64px; height: 16px; "
        "decorator: linear-gradient(90deg, #ff0000, #0000ff); }"
        "#radial { left: 8px; top: 28px; width: 24px; height: 24px; "
        "decorator: radial-gradient(circle, #ffffff, #000000); }"
        "#stripes { left: 40px; top: 28px; width: 24px; height: 24px; "
        "decorator: repeating-linear-gradient(90deg, #ff0000 0px, #ff0000 4px, "
        "#00ff00 4px, #00ff00 8px); }"
        "#masked { left: 72px; top: 28px; width: 16px; height: 24px; "
        "background-color: #ff0000; "
        "mask-image: linear-gradient(90deg, #000000ff, #00000000); }"
        "#many { left: 8px; top: 56px; width: 64px; height: 12px; "
        "decorator: linear-gradient(90deg, #ff0000 0%, #00ff00 20%, "
        "#0000ff 40%, #ffff00 60%, #ff00ff 80%, #00ffff 100%); }"
        "</style></head><body>"
        "<div id=\"linear\"/><div id=\"radial\"/><div id=\"stripes\"/><div "
        "id=\"masked\"/><div id=\"many\"/>"
        "</body></rml>",
        "gradients"
    );
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    CF_Pixel pixels[WIDTH * HEIGHT];
    render_pixels(ui, target, pixels, sizeof pixels);
    CF_Pixel left = pixels[16 * WIDTH + 9], right = pixels[16 * WIDTH + 70];
    CF_Pixel middle = pixels[16 * WIDTH + 40];
    assert(left.colors.r > 220 && left.colors.b < 40 && left.colors.a > 240);
    assert(right.colors.b > 220 && right.colors.r < 40);
    assert(
        middle.colors.r > 90 && middle.colors.r < 170 && middle.colors.b > 90 &&
        middle.colors.b < 170
    );
    CF_Pixel center = pixels[40 * WIDTH + 20], corner = pixels[29 * WIDTH + 9];
    assert(center.colors.r > 200 && corner.colors.r < 60 && corner.colors.a > 240);
    assert(near_color(pixels[40 * WIDTH + 42], 255, 0, 0, 255));
    assert(near_color(pixels[40 * WIDTH + 46], 0, 255, 0, 255));
    assert(near_color(pixels[40 * WIDTH + 50], 255, 0, 0, 255));
    /* Six stops cross the packed UBO vec4 boundary and exercise static lookup
       for both stop positions and colors on GLES. */
    assert(near_color(pixels[62 * WIDTH + 8], 255, 0, 0, 255));
    assert(near_color(pixels[62 * WIDTH + 21], 0, 255, 0, 255));
    assert(near_color(pixels[62 * WIDTH + 34], 0, 0, 255, 255));
    assert(near_color(pixels[62 * WIDTH + 47], 255, 255, 0, 255));
    assert(near_color(pixels[62 * WIDTH + 59], 255, 0, 255, 255));
    CF_Pixel final_stop = pixels[62 * WIDTH + 71];
    assert(
        final_stop.colors.r < 45 && final_stop.colors.g > 210 &&
        final_stop.colors.b > 230
    );
    /* A mask image fades the element as its gradient's alpha does. */
    CF_Pixel opaque = pixels[40 * WIDTH + 73], faded = pixels[40 * WIDTH + 86];
    assert(opaque.colors.r > 240 && opaque.colors.a > 220);
    assert(faded.colors.a < 40 && faded.colors.a < opaque.colors.a);
    cf_destroy_canvas(target);
    rml_document_close(doc);
    rml_backend_destroy(ui);
}

/* An unchanged document reuses its pixels, not merely the canvas handle.
   Transform, visibility, target size, and document removal invalidate it. */
static void check_static_output_cache()
{
    enum
    {
        WIDTH = 64,
        HEIGHT = 48,
        RESIZED_WIDTH = 80,
        RESIZED_HEIGHT = 64
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    rml_element *doc = rml_context_load_document_from_memory(
        rml_backend_context(ui),
        "<rml><body "
        "style=\"margin:0;width:100%;height:100%;background-color:#00ff00;\">"
        "<div id=\"moving\" style=\"position:absolute;left:8px;top:8px;"
        "width:12px;height:12px;background-color:#0000ff;\"></div>"
        "</body></rml>",
        "static-cache"
    );
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    assert(target.id);
    CF_Pixel pixels[RESIZED_WIDTH * RESIZED_HEIGHT];
    render_pixels(ui, target, pixels, WIDTH * HEIGHT * sizeof *pixels);
    assert(pixels[12 * WIDTH + 12].colors.b > 240);
    assert(pixels[4 * WIDTH + 4].colors.g > 240);
    rml_backend_stats first = rml_backend_get_stats(ui);
    uint64_t revision = rml_backend_texture_revision(ui);
    assert(first.geometry_compiles && first.uploaded_bytes && first.render_passes);
    assert(first.static_layer_updates && first.output_updates && revision);
    render_pixels(ui, target, pixels, WIDTH * HEIGHT * sizeof *pixels);
    same_work(first, rml_backend_get_stats(ui));
    assert(rml_backend_texture_revision(ui) == revision);
    assert(pixels[12 * WIDTH + 12].colors.b > 240);

    rml_element *moving = rml_element_query_selector(doc, "#moving");
    assert(moving && rml_element_set_property(moving, "transform", "translateX(16px)"));
    render_pixels(ui, target, pixels, WIDTH * HEIGHT * sizeof *pixels);
    assert(pixels[12 * WIDTH + 12].colors.g > 240);
    assert(pixels[12 * WIDTH + 28].colors.b > 240);
    assert(rml_backend_texture_revision(ui) > revision);
    rml_backend_stats moved = rml_backend_get_stats(ui);
    assert(moved.static_layer_updates > first.static_layer_updates);
    assert(moved.output_updates > first.output_updates);
    render_pixels(ui, target, pixels, WIDTH * HEIGHT * sizeof *pixels);
    same_work(moved, rml_backend_get_stats(ui));

    assert(rml_element_set_property(moving, "display", "none"));
    render_pixels(ui, target, pixels, WIDTH * HEIGHT * sizeof *pixels);
    assert(pixels[12 * WIDTH + 28].colors.g > 240);
    rml_backend_resize(ui, RESIZED_WIDTH, RESIZED_HEIGHT, 1);
    cf_destroy_canvas(target);
    target = cf_make_canvas(cf_canvas_defaults(RESIZED_WIDTH, RESIZED_HEIGHT));
    assert(target.id);
    render_pixels(ui, target, pixels, sizeof pixels);
    assert(pixels[55 * RESIZED_WIDTH + 70].colors.g > 240);
    rml_backend_stats resized = rml_backend_get_stats(ui);
    render_pixels(ui, target, pixels, sizeof pixels);
    same_work(resized, rml_backend_get_stats(ui));
    rml_document_close(doc);
    render_pixels(ui, target, pixels, sizeof pixels);
    assert(pixels[12 * RESIZED_WIDTH + 12].colors.a < 5);
    rml_backend_stats empty = rml_backend_get_stats(ui);
    assert(empty.cached_texture_bytes == 0);
    render_pixels(ui, target, pixels, sizeof pixels);
    same_work(empty, rml_backend_get_stats(ui));
    cf_destroy_canvas(target);
    rml_backend_destroy(ui);
}

/* Repainting a host texture under the same GPU id recomposes ordered layers,
   preserving static geometry, clipping, translucent blending, and foreground. */
static void check_external_output_cache()
{
    enum
    {
        WIDTH = 64,
        HEIGHT = 48
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    CF_Texture texture = cf_make_texture(cf_texture_defaults(1, 1));
    assert(texture.id);
    CF_Pixel red = { .colors = { .r = 128, .g = 0, .b = 0, .a = 128 } };
    cf_texture_update(texture, &red, sizeof red);
    rml_backend_set_texture_revision(ui, "preview", texture.id, 1);
    rml_element *doc = rml_context_load_document_from_memory(
        rml_backend_context(ui),
        "<rml><body "
        "style=\"margin:0;width:100%;height:100%;background-color:#00ff00;\">"
        "<div "
        "style=\"position:absolute;left:8px;top:8px;width:32px;height:24px;"
        "overflow:hidden;clip:always;\">"
        "<img src=\"kit:preview\" "
        "style=\"position:absolute;left:0;top:0;width:48px;height:24px;\"/>"
        "</div><div "
        "style=\"position:absolute;left:24px;top:12px;width:8px;height:8px;"
        "background-color:#0000ff;\"></div></body></rml>",
        "external-cache"
    );
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    assert(target.id);
    CF_Pixel pixels[WIDTH * HEIGHT];
    render_pixels(ui, target, pixels, sizeof pixels);
    CF_Pixel blend = pixels[12 * WIDTH + 12];
    CF_Pixel foreground = pixels[16 * WIDTH + 28];
    CF_Pixel outside = pixels[12 * WIDTH + 44];
    if (!(blend.colors.r > 100 && blend.colors.r < 155 &&
          blend.colors.g > 100 && blend.colors.g < 155 && blend.colors.a > 240 &&
          foreground.colors.b > 240 && outside.colors.g > 240)) {
        fprintf(
            stderr,
            "external-cache first frame: blend (12,12) RGBA %u %u %u %u; "
            "foreground (28,16) RGBA %u %u %u %u; "
            "outside clip (44,12) RGBA %u %u %u %u; "
            "background (44,4) RGBA %u %u %u %u; "
            "background (44,20) RGBA %u %u %u %u; "
            "background (60,40) RGBA %u %u %u %u\n",
            blend.colors.r,
            blend.colors.g,
            blend.colors.b,
            blend.colors.a,
            foreground.colors.r,
            foreground.colors.g,
            foreground.colors.b,
            foreground.colors.a,
            outside.colors.r,
            outside.colors.g,
            outside.colors.b,
            outside.colors.a,
            pixels[4 * WIDTH + 44].colors.r,
            pixels[4 * WIDTH + 44].colors.g,
            pixels[4 * WIDTH + 44].colors.b,
            pixels[4 * WIDTH + 44].colors.a,
            pixels[20 * WIDTH + 44].colors.r,
            pixels[20 * WIDTH + 44].colors.g,
            pixels[20 * WIDTH + 44].colors.b,
            pixels[20 * WIDTH + 44].colors.a,
            pixels[40 * WIDTH + 60].colors.r,
            pixels[40 * WIDTH + 60].colors.g,
            pixels[40 * WIDTH + 60].colors.b,
            pixels[40 * WIDTH + 60].colors.a
        );
    }
    assert(blend.colors.r > 100 && blend.colors.r < 155);
    assert(blend.colors.g > 100 && blend.colors.g < 155 && blend.colors.a > 240);
    assert(pixels[16 * WIDTH + 28].colors.b > 240);
    assert(pixels[12 * WIDTH + 44].colors.g > 240);
    rml_backend_stats first = rml_backend_get_stats(ui);
    uint64_t revision = rml_backend_texture_revision(ui);
    assert(first.static_layer_updates >= 2 && first.texture_composites);
    rml_backend_set_texture_revision(ui, "preview", texture.id, 1);
    render_pixels(ui, target, pixels, sizeof pixels);
    same_work(first, rml_backend_get_stats(ui));
    assert(rml_backend_texture_revision(ui) == revision);

    CF_Pixel yellow = { .colors = { .r = 128, .g = 128, .b = 0, .a = 128 } };
    cf_texture_update(texture, &yellow, sizeof yellow);
    rml_backend_set_texture_revision(ui, "preview", texture.id, 2);
    render_pixels(ui, target, pixels, sizeof pixels);
    blend = pixels[12 * WIDTH + 12];
    assert(blend.colors.r > 100 && blend.colors.r < 155 && blend.colors.g > 240);
    assert(pixels[16 * WIDTH + 28].colors.b > 240);
    assert(pixels[12 * WIDTH + 44].colors.g > 240);
    rml_backend_stats changed = rml_backend_get_stats(ui);
    assert(changed.geometry_compiles == first.geometry_compiles);
    assert(changed.uploaded_bytes == first.uploaded_bytes);
    assert(changed.static_layer_updates == first.static_layer_updates);
    assert(changed.render_passes > first.render_passes);
    assert(changed.output_updates > first.output_updates);
    assert(changed.texture_composites > first.texture_composites);
    assert(rml_backend_texture_revision(ui) > revision);
    render_pixels(ui, target, pixels, sizeof pixels);
    same_work(changed, rml_backend_get_stats(ui));

    /* Unversioned producers must conservatively invalidate same-id contents. */
    cf_texture_update(texture, &red, sizeof red);
    rml_backend_set_texture(ui, "preview", texture.id);
    render_pixels(ui, target, pixels, sizeof pixels);
    assert(
        pixels[12 * WIDTH + 12].colors.g > 100 && pixels[12 * WIDTH + 12].colors.g < 155
    );
    assert(rml_backend_get_stats(ui).static_layer_updates == first.static_layer_updates);
    rml_backend_set_texture_revision(ui, "preview", 0, 3);
    render_pixels(ui, target, pixels, sizeof pixels);
    assert(pixels[12 * WIDTH + 12].colors.g > 240);
    assert(pixels[12 * WIDTH + 12].colors.r < 5);
    assert(pixels[16 * WIDTH + 28].colors.b > 240);
    assert(rml_backend_get_stats(ui).static_layer_updates == first.static_layer_updates);
    cf_destroy_canvas(target);
    rml_document_close(doc);
    rml_backend_destroy(ui);
    cf_destroy_texture(texture);
}

static void check_two_external_surfaces()
{
    enum
    {
        WIDTH = 80,
        HEIGHT = 56
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    CF_Texture a = cf_make_texture(cf_texture_defaults(1, 1));
    CF_Texture b = cf_make_texture(cf_texture_defaults(1, 1));
    assert(a.id && b.id);
    CF_Pixel red = { .colors = { .r = 255, .g = 0, .b = 0, .a = 255 } };
    CF_Pixel yellow = { .colors = { .r = 255, .g = 255, .b = 0, .a = 255 } };
    cf_texture_update(a, &red, sizeof red);
    cf_texture_update(b, &yellow, sizeof yellow);
    rml_backend_set_texture_revision(ui, "surface-a", a.id, 1);
    rml_backend_set_texture_revision(ui, "surface-b", b.id, 1);
    rml_element *doc = rml_context_load_document_from_memory(
        rml_backend_context(ui),
        "<rml><body "
        "style=\"margin:0;width:100%;height:100%;background-color:#00ff00;\">"
        "<div "
        "style=\"position:absolute;left:8px;top:8px;width:40px;height:24px;"
        "overflow:hidden;clip:always;z-index:1;\">"
        "<img id=\"surface-a\" src=\"kit:surface-a\" "
        "style=\"position:absolute;left:0;top:0;width:40px;height:24px;\"/>"
        "</div><div "
        "style=\"position:absolute;left:16px;top:8px;width:40px;height:24px;"
        "overflow:hidden;clip:always;z-index:2;\">"
        "<div "
        "style=\"display:block;width:48px;height:24px;background-color:rgba(0,"
        "0,255,128);\"></div>"
        "</div><img src=\"kit:surface-b\" "
        "style=\"position:absolute;left:32px;top:16px;width:32px;height:24px;"
        "z-index:3;\"/>"
        "<div "
        "style=\"position:absolute;left:40px;top:20px;width:8px;height:8px;"
        "background-color:#ffffff;z-index:4;\"></div>"
        "</body></rml>",
        "two-surfaces"
    );
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    assert(target.id);
    CF_Pixel pixels[WIDTH * HEIGHT];
    render_pixels(ui, target, pixels, sizeof pixels);
    assert(pixels[12 * WIDTH + 12].colors.r > 240);
    CF_Pixel overlay = pixels[12 * WIDTH + 20];
    if (!(overlay.colors.r > 100 && overlay.colors.r < 155 && overlay.colors.b > 100 &&
          overlay.colors.b < 155 && overlay.colors.g < 5)) {
        const int probes[][2] = { { 12, 12 }, { 20, 12 }, { 52, 12 },
                                  { 36, 20 }, { 44, 24 }, { 60, 12 } };
        for (size_t i = 0; i < sizeof probes / sizeof *probes; i++) {
            int x = probes[i][0], y = probes[i][1];
            CF_Pixel p = pixels[y * WIDTH + x];
            fprintf(
                stderr,
                "two-surfaces first frame: (%d,%d) RGBA %u %u %u %u\n",
                x,
                y,
                p.colors.r,
                p.colors.g,
                p.colors.b,
                p.colors.a
            );
        }
    }
    assert(overlay.colors.r > 100 && overlay.colors.r < 155);
    assert(overlay.colors.b > 100 && overlay.colors.b < 155 && overlay.colors.g < 5);
    assert(
        pixels[20 * WIDTH + 36].colors.r > 240 &&
        pixels[20 * WIDTH + 36].colors.g > 240 && pixels[20 * WIDTH + 36].colors.b < 5
    );
    assert(
        pixels[24 * WIDTH + 44].colors.r > 240 && pixels[24 * WIDTH + 44].colors.b > 240
    );
    assert(
        pixels[12 * WIDTH + 60].colors.g > 240 && pixels[12 * WIDTH + 60].colors.b < 5
    );
    rml_backend_stats first = rml_backend_get_stats(ui);
    assert(first.static_layer_updates >= 3);
    /* Only the body fills the target; the two small static runs are cropped. */
    uint64_t target_bytes = (uint64_t)WIDTH * HEIGHT * sizeof(CF_Pixel);
    assert(first.cached_texture_bytes > target_bytes);
    assert(first.cached_texture_bytes < 2 * target_bytes);
    render_pixels(ui, target, pixels, sizeof pixels);
    same_work(first, rml_backend_get_stats(ui));

    CF_Pixel cyan = { .colors = { .r = 0, .g = 255, .b = 255, .a = 255 } };
    cf_texture_update(a, &cyan, sizeof cyan);
    rml_backend_set_texture_revision(ui, "surface-a", a.id, 2);
    render_pixels(ui, target, pixels, sizeof pixels);
    assert(
        pixels[12 * WIDTH + 12].colors.g > 240 && pixels[12 * WIDTH + 12].colors.b > 240
    );
    overlay = pixels[12 * WIDTH + 20];
    assert(
        overlay.colors.r < 5 && overlay.colors.g > 100 &&
        overlay.colors.g < 155 && overlay.colors.b > 240
    );
    assert(
        pixels[20 * WIDTH + 36].colors.r > 240 && pixels[20 * WIDTH + 36].colors.b < 5
    );
    assert(
        pixels[24 * WIDTH + 44].colors.r > 240 && pixels[24 * WIDTH + 44].colors.b > 240
    );
    assert(
        pixels[12 * WIDTH + 60].colors.g > 240 && pixels[12 * WIDTH + 60].colors.b < 5
    );
    rml_backend_stats changed = rml_backend_get_stats(ui);
    assert(changed.geometry_compiles == first.geometry_compiles);
    assert(changed.uploaded_bytes == first.uploaded_bytes);
    assert(changed.static_layer_updates == first.static_layer_updates);
    assert(changed.output_updates > first.output_updates);

    rml_element *surface_a = rml_element_query_selector(doc, "#surface-a");
    assert(surface_a && rml_element_set_property(surface_a, "transform", "translateX(8px)"));
    render_pixels(ui, target, pixels, sizeof pixels);
    assert(
        pixels[12 * WIDTH + 12].colors.g > 240 && pixels[12 * WIDTH + 12].colors.b < 5
    );
    assert(pixels[12 * WIDTH + 20].colors.b > 240);
    /* A's moved edge is clipped to its parent, while the static overlay remains. */
    overlay = pixels[12 * WIDTH + 52];
    assert(
        overlay.colors.g > 100 && overlay.colors.g < 155 &&
        overlay.colors.b > 100 && overlay.colors.b < 155
    );
    assert(
        pixels[20 * WIDTH + 36].colors.r > 240 && pixels[20 * WIDTH + 36].colors.b < 5
    );
    assert(
        pixels[24 * WIDTH + 44].colors.r > 240 && pixels[24 * WIDTH + 44].colors.b > 240
    );
    rml_backend_stats moved = rml_backend_get_stats(ui);
    assert(moved.static_layer_updates == changed.static_layer_updates);
    assert(moved.output_updates > changed.output_updates);
    render_pixels(ui, target, pixels, sizeof pixels);
    same_work(moved, rml_backend_get_stats(ui));

    assert(rml_element_set_property(surface_a, "display", "none"));
    render_pixels(ui, target, pixels, sizeof pixels);
    overlay = pixels[12 * WIDTH + 20];
    assert(
        overlay.colors.r < 5 && overlay.colors.g > 100 &&
        overlay.colors.g < 155 && overlay.colors.b > 100 && overlay.colors.b < 155
    );
    assert(
        pixels[20 * WIDTH + 36].colors.r > 240 && pixels[20 * WIDTH + 36].colors.b < 5
    );
    assert(
        pixels[24 * WIDTH + 44].colors.r > 240 && pixels[24 * WIDTH + 44].colors.b > 240
    );
    assert(
        pixels[12 * WIDTH + 60].colors.g > 240 && pixels[12 * WIDTH + 60].colors.b < 5
    );
    assert(rml_backend_get_stats(ui).output_updates > moved.output_updates);
    rml_backend_stats hidden = rml_backend_get_stats(ui);
    render_pixels(ui, target, pixels, sizeof pixels);
    same_work(hidden, rml_backend_get_stats(ui));
    rml_document_close(doc);
    render_pixels(ui, target, pixels, sizeof pixels);
    assert(pixels[24 * WIDTH + 44].colors.a < 5);
    assert(rml_backend_get_stats(ui).cached_texture_bytes == 0);
    cf_destroy_canvas(target);
    rml_backend_destroy(ui);
    cf_destroy_texture(a);
    cf_destroy_texture(b);
}

/* Compare incremental updates with an independent first render. The large
   unchanged anchors keep each run's crop stable while small alpha boxes change. */
static rml_element *incremental_document(rml_backend *ui)
{
    rml_element *doc = rml_context_load_document_from_memory(
        rml_backend_context(ui),
        "<rml><body "
        "style=\"margin:0;width:100%;height:100%;background-color:#204020;\">"
        "<img id=\"a\" src=\"kit:a\" "
        "style=\"position:absolute;left:8px;top:8px;width:48px;height:40px;z-"
        "index:1;\"/>"
        "<div id=\"middle\" "
        "style=\"position:absolute;left:4px;top:4px;width:80px;height:52px;z-"
        "index:2;\">"
        "<div "
        "style=\"position:absolute;left:0;top:0;width:80px;height:52px;"
        "background-color:rgba(0,0,255,32);\"></div>"
        "<div id=\"change\" "
        "style=\"position:absolute;left:12px;top:10px;width:12px;height:14px;"
        "background-color:rgba(255,0,0,128);\"></div>"
        "<div "
        "style=\"position:absolute;left:18px;top:14px;width:20px;height:16px;"
        "background-color:rgba(255,255,0,96);\"></div>"
        "<div id=\"extra\" "
        "style=\"position:absolute;left:66px;top:40px;width:6px;height:6px;"
        "background-color:#ffffff;\"></div>"
        "</div>"
        "<img id=\"b\" src=\"kit:b\" "
        "style=\"position:absolute;left:40px;top:20px;width:40px;height:32px;z-"
        "index:3;\"/>"
        "<div "
        "style=\"position:absolute;left:12px;top:12px;width:80px;height:48px;z-"
        "index:4;\">"
        "<div id=\"anchor\" "
        "style=\"position:absolute;left:0;top:0;width:72px;height:40px;"
        "background-color:rgba(255,255,255,16);\"></div>"
        "<div "
        "style=\"position:absolute;left:36px;top:12px;width:18px;height:14px;"
        "background-color:rgba(0,255,255,128);\"></div>"
        "<div "
        "style=\"position:absolute;left:60px;top:30px;width:6px;height:6px;"
        "background-color:#ffffff;\"></div>"
        "</div></body></rml>",
        "incremental-static"
    );
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    return doc;
}

static void incremental_state(rml_element *doc, int state)
{
    rml_element *box = rml_element_query_selector(doc, "#change");
    assert(box);
    const char *widths[] = { "12px", "30px", "8px", "8px",
                             "8px",  "8px",  "8px", "8px" };
    const char *lefts[] = { "12px", "12px", "12px", "30px",
                            "30px", "30px", "30px", "30px" };
    assert(rml_element_set_property(box, "width", widths[state]));
    assert(rml_element_set_property(box, "left", lefts[state]));
    if (state >= 4) {
        assert(
            rml_element_set_property(rml_element_query_selector(doc, "#extra"), "display", "none")
        );
    }
    if (state >= 5) {
        assert(
            rml_element_set_property(rml_element_query_selector(doc, "#anchor"), "width", "80px")
        );
    }
    if (state >= 6) {
        assert(rml_element_set_property(rml_element_query_selector(doc, "#a"), "display", "none"));
    }
    if (state >= 7) {
        assert(
            rml_element_set_property(rml_element_query_selector(doc, "#middle"), "z-index", "5")
        );
    }
}

static rml_backend_stats incremental_reference(
    CF_Texture a,
    CF_Texture b,
    uint64_t revision,
    int state,
    CF_Canvas target,
    const CF_Pixel *actual
)
{
    enum
    {
        WIDTH = 96,
        HEIGHT = 64
    };
    rml_backend *fresh = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(fresh);
    rml_backend_set_texture_revision(fresh, "a", a.id, revision);
    rml_backend_set_texture_revision(fresh, "b", b.id, 1);
    rml_element *doc = incremental_document(fresh);
    incremental_state(doc, state);
    CF_Pixel expected[WIDTH * HEIGHT];
    render_pixels(fresh, target, expected, sizeof expected);
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        const unsigned char *got = (const unsigned char *)&actual[i];
        const unsigned char *want = (const unsigned char *)&expected[i];
        for (size_t channel = 0; channel < sizeof(CF_Pixel); channel++) {
            if (abs((int)got[channel] - (int)want[channel]) > 1) {
                fprintf(
                    stderr,
                    "incremental state %d pixel (%d,%d) channel %zu: %u != "
                    "%u\n",
                    state,
                    i % WIDTH,
                    i / WIDTH,
                    channel,
                    got[channel],
                    want[channel]
                );
                assert(false);
            }
        }
    }
    rml_backend_stats stats = rml_backend_get_stats(fresh);
    rml_document_close(doc);
    rml_backend_destroy(fresh);
    return stats;
}

static void check_incremental_static_layers()
{
    enum
    {
        WIDTH = 96,
        HEIGHT = 64
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    CF_Texture a = cf_make_texture(cf_texture_defaults(1, 1));
    CF_Texture b = cf_make_texture(cf_texture_defaults(1, 1));
    assert(a.id && b.id);
    CF_Pixel red = { .colors = { .r = 255, .a = 255 } };
    CF_Pixel green = { .colors = { .g = 255, .a = 255 } };
    cf_texture_update(a, &red, sizeof red);
    cf_texture_update(b, &green, sizeof green);
    rml_backend_set_texture_revision(ui, "a", a.id, 1);
    rml_backend_set_texture_revision(ui, "b", b.id, 1);
    rml_element *doc = incremental_document(ui);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    assert(target.id);
    CF_Pixel pixels[WIDTH * HEIGHT];
    render_pixels(ui, target, pixels, sizeof pixels);
    assert(
        pixels[12 * WIDTH + 12].colors.r > 150 && pixels[12 * WIDTH + 12].colors.g < 50
    );
    assert(
        pixels[24 * WIDTH + 44].colors.g > 200 && pixels[24 * WIDTH + 44].colors.r < 50
    );
    assert(
        pixels[44 * WIDTH + 74].colors.r > 240 && pixels[44 * WIDTH + 74].colors.b > 240
    );
    incremental_reference(a, b, 1, 0, target, pixels);
    rml_backend_stats before = rml_backend_get_stats(ui);
    render_pixels(ui, target, pixels, sizeof pixels);
    same_work(before, rml_backend_get_stats(ui));

    /* Growth, shrinkage, and movement must erase old coverage and preserve
       overlapping translucent paint in its original order. */
    for (int state = 1; state <= 3; state++) {
        incremental_state(doc, state);
        render_pixels(ui, target, pixels, sizeof pixels);
        rml_backend_stats after = rml_backend_get_stats(ui);
        rml_backend_stats fresh = incremental_reference(a, b, 1, state, target, pixels);
        assert(after.static_partial_updates == before.static_partial_updates + 1);
        assert(after.static_commands_replayed > before.static_commands_replayed);
        assert(
            after.static_commands_replayed - before.static_commands_replayed <
            fresh.static_commands_replayed
        );
        before = after;
    }
    cf_texture_update(a, &green, sizeof green);
    rml_backend_set_texture_revision(ui, "a", a.id, 2);
    render_pixels(ui, target, pixels, sizeof pixels);
    incremental_reference(a, b, 2, 3, target, pixels);
    rml_backend_stats external = rml_backend_get_stats(ui);
    assert(external.static_partial_updates == before.static_partial_updates);
    assert(external.static_commands_replayed == before.static_commands_replayed);
    assert(external.static_layer_updates == before.static_layer_updates);
    assert(external.output_updates > before.output_updates);
    before = external;

    /* A changed command count, changed crop, changed external partition, or
       reordered runs requires full refresh rather than partial damage replay. */
    for (int state = 4; state <= 7; state++) {
        incremental_state(doc, state);
        render_pixels(ui, target, pixels, sizeof pixels);
        incremental_reference(a, b, 2, state, target, pixels);
        rml_backend_stats after = rml_backend_get_stats(ui);
        assert(after.static_partial_updates == before.static_partial_updates);
        assert(after.static_commands_replayed > before.static_commands_replayed);
        assert(after.static_layer_updates > before.static_layer_updates);
        before = after;
    }
    render_pixels(ui, target, pixels, sizeof pixels);
    same_work(before, rml_backend_get_stats(ui));
    rml_document_close(doc);
    rml_backend_destroy(ui);
    cf_destroy_canvas(target);
    cf_destroy_texture(a);
    cf_destroy_texture(b);
}

static void retained_pixels(rml_backend *ui, CF_Canvas target, double delta, bool dirty, CF_Pixel *pixels, int size)
{
    cf_app_update(NULL);
    cf_draw3d_push();
    cf_draw3d_translate(cf_v3(37, 19, 0));
    rml_backend_frame_retained(ui, delta, dirty);
    cf_draw3d_pop();
    cf_draw_push();
    rml_backend_present(ui);
    cf_render_to(target, true);
    cf_draw_pop();
    cf_app_draw_onto_screen(true);
    read_pixels(target, pixels, size);
}

/* Authoring highlights must not cover modal controls, even when emitted later.
   The UI Inspector must still be able to inspect the modal itself. */
static void check_guide_modal_layers()
{
    enum
    {
        WIDTH = 128,
        HEIGHT = 96
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    kit *k = test_kit_create(rml_backend_context(ui), "studio.rml");
    assert(k);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    assert(target.id);
    CF_Pixel pixels[WIDTH * HEIGHT];
    kit_frame_input in = { .dp_ratio = 1, .width = WIDTH, .height = HEIGHT };
    kit_rect box = { 20, 30, 40, 40 };
    kit_highlight_desc highlight = {
        .margin = box,
        .border = box,
        .padding = box,
        .content = box,
        .label = "Selection",
    };
    for (int phase = 0; phase < 4; phase++) {
        kit_begin(k, &in);
        kit_root(k, "app");
        if (phase == 1 || phase == 2) {
            kit_modal_begin(k, "dialog");
            kit_style(k, "background-color", "#00ff00");
            kit_modal_end(k);
        }
        kit_guides_begin(k, "guides");
        kit_highlight(k, "selection", &highlight, NULL);
        kit_guides_end(k);
        if (phase == 2) {
            kit_inspect_begin(k, "inspector", false);
            kit_highlight(k, "inspection", &highlight, NULL);
            kit_inspect_end(k);
        }
        kit_end(k);
        retained_pixels(ui, target, 1.0 / 60, true, pixels, sizeof pixels);
        CF_Pixel content = pixels[40 * WIDTH + 32];
        CF_Pixel label = pixels[18 * WIDTH + 32];
        if (phase == 1) {
            assert(
                content.colors.r < 5 && content.colors.g > 250 && content.colors.b < 5
            );
            assert(label.colors.r < 5 && label.colors.g > 250 && label.colors.b < 5);
        } else {
            assert(content.colors.b > 100);
        }
    }
    kit_destroy(k);
    rml_backend_destroy(ui);
    cf_destroy_canvas(target);
}

/* Retention skips traversal itself, yet input and an animation deadline still
   produce changed pixels. Caller-owned DOM edits and resize must wake it too. */
static void check_retained_frame_wakes()
{
    enum
    {
        WIDTH = 64,
        HEIGHT = 48,
        WIDE = 80,
        TALL = 64
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    rml_element *doc = rml_context_load_document_from_memory(
        rml_backend_context(ui),
        "<rml><head><style>#moving { "
        "background-color:#0000ff;transition:background-color 0.2s linear-in; }"
        "#moving:hover { background-color:#ff0000; }</style></head>"
        "<body "
        "style=\"margin:0;width:100%;height:100%;background-color:#00ff00;\">"
        "<div id=\"moving\" "
        "style=\"position:absolute;left:8px;top:8px;width:12px;height:12px;\"><"
        "/div>"
        "</body></rml>",
        "retained-wakes"
    );
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    assert(target.id);
    CF_Pixel pixels[WIDE * TALL];
    retained_pixels(ui, target, 1.0 / 60, true, pixels, WIDTH * HEIGHT * sizeof *pixels);
    assert(pixels[12 * WIDTH + 12].colors.b > 240);
    rml_element *moving = rml_element_query_selector(doc, "#moving");
    rml_transition transition;
    assert(moving && rml_element_transitions(moving, &transition, 1) == 1);
    assert(transition.duration > .19f && transition.duration < .21f);
    rml_backend_stats first = rml_backend_get_stats(ui);
    assert(first.context_updates && first.render_captures);
    for (int i = 0; i < 4; i++) {
        retained_pixels(ui, target, 1.0 / 60, false, pixels, WIDTH * HEIGHT * sizeof *pixels);
        assert(pixels[12 * WIDTH + 12].colors.b > 240);
    }
    rml_backend_stats idle = rml_backend_get_stats(ui);
    same_work(first, idle);
    assert(idle.context_updates == first.context_updates);
    assert(idle.render_captures == first.render_captures);
    assert(idle.retained_frames == first.retained_frames + 4);
    rml_backend_process_input(ui, &(rml_input){ .type = RML_INPUT_POINTER_MOVE, .x = 12, .y = 12 });
    retained_pixels(ui, target, 1.0 / 60, false, pixels, WIDTH * HEIGHT * sizeof *pixels);
    rml_backend_stats input = rml_backend_get_stats(ui);
    assert(input.context_updates > idle.context_updates);
    assert(input.render_captures > idle.render_captures);
    CF_Pixel initial_hover = pixels[12 * WIDTH + 12];
    double hover_time = rml_elapsed_time();
    double hover_delay = rml_context_next_update_delay(rml_backend_context(ui));
    bool hovered = rml_element_is_pseudo_class_set(moving, "hover");
    /* RmlUi limits animation progress to 0.1s per update. Advance ordinary
       frames across the transition instead of treating a long stall as completion. */
    for (int frame = 0; frame < 15; frame++) {
        retained_pixels(ui, target, 1.0 / 60, false, pixels, WIDTH * HEIGHT * sizeof *pixels);
        if (frame == 5) {
            assert(
                pixels[12 * WIDTH + 12].colors.r > 5 &&
                pixels[12 * WIDTH + 12].colors.b > 5
            );
        }
    }
    if (!(pixels[12 * WIDTH + 12].colors.r > 240 &&
          pixels[12 * WIDTH + 12].colors.b < 5)) {
        CF_Pixel p = pixels[12 * WIDTH + 12];
        char color[128] = { 0 };
        rml_element_property(moving, "background-color", color, sizeof color);
        rml_backend_stats current = rml_backend_get_stats(ui);
        fprintf(
            stderr,
            "retained hover: initial RGBA %u %u %u %u, hover %d, time %.6f, "
            "delay %.6f; "
            "later RGBA %u %u %u %u, hover %d, time %.6f, delay %.6f, color "
            "%s; "
            "updates %llu->%llu, captures %llu->%llu\n",
            initial_hover.colors.r,
            initial_hover.colors.g,
            initial_hover.colors.b,
            initial_hover.colors.a,
            hovered,
            hover_time,
            hover_delay,
            p.colors.r,
            p.colors.g,
            p.colors.b,
            p.colors.a,
            rml_element_is_pseudo_class_set(moving, "hover"),
            rml_elapsed_time(),
            rml_context_next_update_delay(rml_backend_context(ui)),
            color,
            (unsigned long long)input.context_updates,
            (unsigned long long)current.context_updates,
            (unsigned long long)input.render_captures,
            (unsigned long long)current.render_captures
        );
    }
    assert(
        pixels[12 * WIDTH + 12].colors.r > 240 && pixels[12 * WIDTH + 12].colors.b < 5
    );
    rml_backend_stats animated = rml_backend_get_stats(ui);
    assert(animated.context_updates > input.context_updates);
    assert(animated.render_captures > input.render_captures);
    assert(animated.output_updates > idle.output_updates);

    assert(moving && rml_element_set_property(moving, "left", "24px"));
    retained_pixels(ui, target, 1.0 / 60, true, pixels, WIDTH * HEIGHT * sizeof *pixels);
    assert(pixels[12 * WIDTH + 12].colors.g > 240);
    assert(
        pixels[12 * WIDTH + 28].colors.g < 5 && pixels[12 * WIDTH + 28].colors.a > 240
    );
    rml_backend_resize(ui, WIDE, TALL, 1);
    cf_destroy_canvas(target);
    target = cf_make_canvas(cf_canvas_defaults(WIDE, TALL));
    assert(target.id);
    retained_pixels(ui, target, .25, false, pixels, sizeof pixels);
    assert(pixels[55 * WIDE + 70].colors.g > 240);
    assert(rml_backend_get_stats(ui).context_updates > animated.context_updates);
    /* Removing the document releases captured geometry. A later retained frame
       must neither dereference its old meshes nor resurrect its pixels. */
    rml_document_close(doc);
    retained_pixels(ui, target, 1.0 / 60, true, pixels, sizeof pixels);
    assert(pixels[12 * WIDE + 28].colors.a < 5);
    rml_backend_stats closed = rml_backend_get_stats(ui);
    retained_pixels(ui, target, 1.0 / 60, false, pixels, sizeof pixels);
    assert(pixels[12 * WIDE + 28].colors.a < 5);
    same_work(closed, rml_backend_get_stats(ui));
    assert(rml_backend_get_stats(ui).context_updates == closed.context_updates);
    assert(rml_backend_get_stats(ui).render_captures == closed.render_captures);
    cf_destroy_canvas(target);
    rml_backend_destroy(ui);
}

static int white_pixels(const CF_Pixel *pixels, int count)
{
    int white = 0;
    for (int i = 0; i < count; i++) {
        CF_Pixel p = pixels[i];
        white += p.colors.r > 220 && p.colors.g > 220 && p.colors.b > 220 &&
                 p.colors.a > 220;
    }
    return white;
}

static void check_retained_caret_deadline()
{
    enum
    {
        WIDTH = 64,
        HEIGHT = 48
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    rml_element *doc = rml_context_load_document_from_memory(
        rml_backend_context(ui),
        "<rml><body "
        "style=\"margin:0;width:100%;height:100%;background-color:#000000;\">"
        "<input id=\"text\" type=\"text\" value=\"\" "
        "style=\"position:absolute;left:8px;top:8px;"
        "width:48px;height:24px;border-width:0;background-color:#000000;color:#"
        "ffffff;"
        "font-family:geist;font-size:16px;line-height:18px;\"/></body></rml>",
        "retained-caret"
    );
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    assert(target.id);
    CF_Pixel pixels[WIDTH * HEIGHT];
    retained_pixels(ui, target, 1.0 / 60, true, pixels, sizeof pixels);
    rml_element *input = rml_element_query_selector(doc, "#text");
    assert(input && rml_element_focus(input, true));
    retained_pixels(ui, target, 1.0 / 60, true, pixels, sizeof pixels);
    int visible = white_pixels(pixels, WIDTH * HEIGHT);
    assert(visible > 0);
    rml_backend_stats focused = rml_backend_get_stats(ui);
    retained_pixels(ui, target, .05, false, pixels, sizeof pixels);
    assert(white_pixels(pixels, WIDTH * HEIGHT) == visible);
    rml_backend_stats before_blink = rml_backend_get_stats(ui);
    assert(before_blink.context_updates == focused.context_updates);
    assert(before_blink.render_captures == focused.render_captures);
    /* RmlUi requests a 0.7s caret deadline. Crossing it must update and repaint
       even though the caller changed neither DOM nor producer textures. */
    retained_pixels(ui, target, .75, false, pixels, sizeof pixels);
    assert(white_pixels(pixels, WIDTH * HEIGHT) == 0);
    rml_backend_stats blinked = rml_backend_get_stats(ui);
    assert(blinked.context_updates > before_blink.context_updates);
    assert(blinked.render_captures > before_blink.render_captures);
    assert(blinked.output_updates > before_blink.output_updates);
    retained_pixels(ui, target, .7, false, pixels, sizeof pixels);
    assert(white_pixels(pixels, WIDTH * HEIGHT) == visible);
    rml_document_close(doc);
    retained_pixels(ui, target, 1.0 / 60, true, pixels, sizeof pixels);
    assert(pixels[12 * WIDTH + 12].colors.a < 5);
    cf_destroy_canvas(target);
    rml_backend_destroy(ui);
}

static void check_retained_external_frames()
{
    enum
    {
        WIDTH = 80,
        HEIGHT = 56
    };
    rml_backend *ui = rml_backend_create(WIDTH, HEIGHT, 1);
    assert(ui);
    CF_Texture a = cf_make_texture(cf_texture_defaults(1, 1));
    CF_Texture b = cf_make_texture(cf_texture_defaults(1, 1));
    CF_Texture replacement = cf_make_texture(cf_texture_defaults(1, 1));
    assert(a.id && b.id && replacement.id);
    CF_Pixel red = { .colors = { .r = 255, .g = 0, .b = 0, .a = 255 } };
    CF_Pixel yellow = { .colors = { .r = 255, .g = 255, .b = 0, .a = 255 } };
    CF_Pixel cyan = { .colors = { .r = 0, .g = 255, .b = 255, .a = 255 } };
    cf_texture_update(a, &red, sizeof red);
    cf_texture_update(b, &yellow, sizeof yellow);
    cf_texture_update(replacement, &cyan, sizeof cyan);
    rml_backend_set_texture_revision(ui, "a", a.id, 1);
    rml_backend_set_texture_revision(ui, "b", b.id, 1);
    rml_element *doc = rml_context_load_document_from_memory(
        rml_backend_context(ui),
        "<rml><body "
        "style=\"margin:0;width:100%;height:100%;background-color:#00ff00;\">"
        "<div "
        "style=\"position:absolute;left:8px;top:8px;width:32px;height:24px;"
        "overflow:hidden;clip:always;z-index:1;\">"
        "<img src=\"kit:a\" "
        "style=\"position:absolute;left:0;top:0;width:48px;height:24px;\"/></"
        "div>"
        "<div "
        "style=\"position:absolute;left:16px;top:8px;width:40px;height:24px;"
        "background-color:rgba(0,0,255,128);z-index:2;\"></div>"
        "<img src=\"kit:b\" "
        "style=\"position:absolute;left:32px;top:16px;width:32px;height:24px;z-"
        "index:3;\"/>"
        "<div "
        "style=\"position:absolute;left:40px;top:20px;width:8px;height:8px;"
        "background-color:#ffffff;z-index:4;\"></div>"
        "</body></rml>",
        "retained-external"
    );
    assert(doc);
    rml_document_show(doc, RML_MODAL_NONE, RML_FOCUS_NONE, RML_SCROLL_FLAG_AUTO);
    CF_Canvas target = cf_make_canvas(cf_canvas_defaults(WIDTH, HEIGHT));
    assert(target.id);
    CF_Pixel pixels[WIDTH * HEIGHT];
    retained_pixels(ui, target, 1.0 / 60, true, pixels, sizeof pixels);
    assert(
        pixels[12 * WIDTH + 20].colors.r > 100 && pixels[12 * WIDTH + 20].colors.r < 155
    );
    rml_backend_stats first = rml_backend_get_stats(ui);
    uint64_t revision = rml_backend_texture_revision(ui);
    rml_backend_set_texture_revision(ui, "a", replacement.id, 1);
    retained_pixels(ui, target, 1.0 / 60, false, pixels, sizeof pixels);
    CF_Pixel overlay = pixels[12 * WIDTH + 20];
    assert(
        overlay.colors.r < 5 && overlay.colors.g > 100 &&
        overlay.colors.g < 155 && overlay.colors.b > 240
    );
    assert(
        pixels[20 * WIDTH + 36].colors.r > 240 && pixels[20 * WIDTH + 36].colors.b < 5
    );
    assert(
        pixels[24 * WIDTH + 44].colors.r > 240 && pixels[24 * WIDTH + 44].colors.b > 240
    );
    assert(
        pixels[12 * WIDTH + 44].colors.g > 100 &&
        pixels[12 * WIDTH + 44].colors.g < 155 &&
        pixels[12 * WIDTH + 44].colors.b > 100 && pixels[12 * WIDTH + 44].colors.b < 155
    );
    rml_backend_stats rebound = rml_backend_get_stats(ui);
    assert(
        rebound.context_updates == first.context_updates &&
        rebound.render_captures == first.render_captures
    );
    assert(
        rebound.geometry_compiles == first.geometry_compiles &&
        rebound.uploaded_bytes == first.uploaded_bytes
    );
    assert(rebound.static_layer_updates == first.static_layer_updates);
    assert(
        rebound.output_updates > first.output_updates &&
        rml_backend_texture_revision(ui) > revision
    );
    /* Release the old producer after rebinding; retained commands resolve the
       current handle rather than borrowing the released producer texture. */
    cf_destroy_texture(a);
    cf_texture_update(replacement, &red, sizeof red);
    rml_backend_set_texture_revision(ui, "a", replacement.id, 2);
    retained_pixels(ui, target, 1.0 / 60, false, pixels, sizeof pixels);
    assert(
        pixels[12 * WIDTH + 20].colors.r > 100 && pixels[12 * WIDTH + 20].colors.r < 155
    );
    rml_backend_stats repainted = rml_backend_get_stats(ui);
    assert(
        repainted.context_updates == first.context_updates &&
        repainted.render_captures == first.render_captures
    );
    assert(
        repainted.static_layer_updates == first.static_layer_updates &&
        repainted.uploaded_bytes == first.uploaded_bytes
    );
    assert(repainted.output_updates > rebound.output_updates);
    retained_pixels(ui, target, 1.0 / 60, false, pixels, sizeof pixels);
    same_work(repainted, rml_backend_get_stats(ui));
    assert(rml_backend_get_stats(ui).context_updates == first.context_updates);
    assert(rml_backend_get_stats(ui).render_captures == first.render_captures);
    rml_document_close(doc);
    retained_pixels(ui, target, 1.0 / 60, true, pixels, sizeof pixels);
    assert(pixels[24 * WIDTH + 44].colors.a < 5);
    cf_destroy_canvas(target);
    rml_backend_destroy(ui);
    cf_destroy_texture(b);
    cf_destroy_texture(replacement);
}

int rmlui_render_run(const char *program, bool gles)
{
    CF_Result result = cf_make_app(
        "RmlUi render smoke",
        0,
        0,
        0,
        1024,
        768,
        CF_APP_OPTIONS_NO_AUDIO_BIT | CF_APP_OPTIONS_HIDDEN_BIT |
            (gles ? CF_APP_OPTIONS_GFX_OPENGL_BIT : 0),
        program
    );
    if (cf_is_error(result) && gles) {
        /* Desktop GLES needs a driver such as Mesa's. On macOS, put ANGLE's
           libEGL.dylib and libGLESv2.dylib on DYLD_FALLBACK_LIBRARY_PATH. */
        printf("RmlUi render SKIP: no GLES window: %s\n", result.details);
        return 0;
    }
    if (cf_is_error(result)) {
        fprintf(stderr, "RmlUi render app: %s\n", result.details);
        return 1;
    }
    int width = cf_app_get_width(), height = cf_app_get_height();
    rml_backend *backend = rml_backend_create(width, height, cf_app_get_pixel_scale());
    assert(backend);
    kit *k = test_kit_create(rml_backend_context(backend), "studio.rml");
    assert(k);
    kit_frame_input in = { .dp_ratio = cf_app_get_pixel_scale(),
                           .width = (float)width,
                           .height = (float)height };
    CF_Canvas witness = cf_make_canvas(cf_canvas_defaults(128, 128));
    assert(witness.id);
    for (int frame = 0; frame < 3; frame++) {
        cf_app_update(NULL);
        kit_begin(k, &in);
        kit_root(k, "app");
        kit_label(k, "label", NULL, "Studio");
        kit_end(k);
        rml_backend_update(backend, 1.0 / 60.0);
        queue_witness_geometry();
        rml_backend_render(backend);
        /* Rendering the retained UI must leave ordinary Cute layer-0 work queued. */
        cf_render_layers_to(witness, 0, 0, true);
        rml_backend_present(backend);
        /* Submit the frame first: on Vulkan a readback only sees submitted work. */
        cf_app_draw_onto_screen(true);
        assert(has_magenta(witness));
    }
    cf_destroy_canvas(witness);
    check_scissor_left_behind();
    check_box_shadows();
    check_clip_masks();
    check_filters();
    check_gradients();
    check_shadow_beside_surface();
    check_overlay_shadow();
    check_static_output_cache();
    check_external_output_cache();
    check_two_external_surfaces();
    check_incremental_static_layers();
    check_retained_frame_wakes();
    check_retained_caret_deadline();
    check_retained_external_frames();
    check_guide_modal_layers();
    kit_destroy(k);
    rml_backend_destroy(backend);
    cf_destroy_app();
    printf(
        "RmlUi render PASS%s: retained UI renders beside queued layer-0 draws "
        "under a scissor another pass left, and renders focus rings, blurred "
        "shadows, rounded clip masks, filters and gradients\n",
        gles ? " on GLES" : ""
    );
    return 0;
}
