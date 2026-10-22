#include "graph/authoring/graph_edit.h"
#include "editor/workspace/editor_internal.h"
#include "graph/nodes/behavior_expression.h"
#include "graph/shader/shader_graph.h"
#include "test/suite_internal.h"

void run_graph_document_checks()
{
    eval_context eval_ctx;
    eval_context_init(&eval_ctx);
    const node_desc *add = registry_node_by_name("Add");
    const node_desc *make = registry_node_by_name("Make_CF_V3");
    /* 8. serialization round trip: constants + add + edges */
    {
        graph g2;
        graph_init(&g2);
        const node_desc *cc = registry_node_by_name("Real");
        test_check(cc != NULL, "registry exposes Real literal");

        node *rc1 = graph_add_node(&g2, cc);
        graph_set_pin_default(
            &g2,
            rc1,
            0,
            0,
            &(pin_value){ .type = PIN_F64, .type_name = "double", .d = 2.0 }
        );
        node *rc2 = graph_add_node(&g2, cc);
        graph_set_pin_default(
            &g2,
            rc2,
            0,
            0,
            &(pin_value){ .type = PIN_F64, .type_name = "double", .d = 3.0 }
        );
        node *radd = graph_add_node(&g2, add);
        test_check(graph_connect(&g2, rc1, 0, radd, 0) == 0, "roundtrip: c1 -> add.a");
        test_check(graph_connect(&g2, rc2, 0, radd, 1) == 0, "roundtrip: c2 -> add.b");

        rc1->x = -220.0f;
        rc1->y = 40.0f;
        radd->x = 100.0f;
        radd->y = -30.0f;

        av_doc *native = av_doc_new();
        av_value *native_graph = graph_to_value(native, &g2);
        graph *native_copy = graph_from_value(native_graph);
        test_check(
            native_copy && acount(native_copy->nodes) == 3 &&
                graph_node_by_id(native_copy, rc1->id)->input_pins[0].id ==
                    rc1->input_pins[0].id &&
                graph_node_by_id(native_copy, radd->id)->output_pins[0].type == PIN_F64,
            "native graph roundtrip preserves stable IDs and inferred types"
        );
        av_doc_free(native);
        if (native_copy) {
            node *copied_literal = graph_node_by_id(native_copy, rc1->id);
            graph_set_pin_default(
                native_copy,
                copied_literal,
                0,
                0,
                &(pin_value){ .type = PIN_F64, .type_name = "double", .d = 9.0 }
            );
            test_check(
                rc1->input_pins[0].default_value.d == 2.0 &&
                    copied_literal->input_pins[0].default_value.d == 9.0,
                "native graph copies own their defaults after the asset tree "
                "is freed"
            );
            graph_free(native_copy);
            free(native_copy);
        }

        char *json = graph_save(&g2);
        test_check(json != NULL, "roundtrip: serialize to code");
        if (json) {
            printf("\n--- serialized graph ---\n%s--- end ---\n", json);

            graph *g3 = graph_load(json);
            test_check(g3 != NULL, "roundtrip: load from code");
            if (g3) {
                test_check(acount(g3->nodes) == 3, "roundtrip: node count survives");
                test_check(acount(g3->edges) == 2, "roundtrip: edge count survives");
                test_check(g3->nodes[0]->x == -220.0f && g3->nodes[0]->y == 40.0f, "roundtrip: node position survives");
                test_check(g3->nodes[2]->x == 100.0f && g3->nodes[2]->y == -30.0f, "roundtrip: second node position survives");

                arena a2;
                arena_init(&a2, 4096);
                pin_value out;
                test_check(
                    graph_eval_output(g3, g3->nodes[2], 0, &a2, &out, &eval_ctx) == 0,
                    "roundtrip: eval loaded add node"
                );
                test_check(out.type == PIN_F64 && out.d == 5.0, "roundtrip: 2.0 + 3.0 == 5.0");
                char *json2 = graph_save(g3);
                graph *g4 = json2 ? graph_load(json2) : NULL;
                char *json3 = g4 ? graph_save(g4) : NULL;
                test_check(json2 && json3 && strcmp(json2, json3) == 0, "load/save/load/save is byte-identical");
                if (g4) {
                    graph_free(g4);
                    free(g4);
                }
                free(json2);
                free(json3);
                arena_free(&a2);
                graph_free(g3);
                free(g3);
            }
            free(json);
        }
        graph_free(&g2);
    }

    /* Keyed extension fields retain identity when text editors reorder them. */
    {
        graph source;
        graph_init(&source);
        node *sender = graph_add_node(&source, registry_node_by_name("Send Event"));
        pin_spec payload = { .name = "Amount", .type = PIN_I32, .kind = PIN_DATA };
        int amount = graph_add_pin(&source, sender, 0, &payload);
        payload.name = "Source";
        int origin = graph_add_pin(&source, sender, 0, &payload);
        av_doc *doc = av_doc_new();
        av_value *written = graph_to_value(doc, &source);
        av_value *node_value = av_arr_get(av_obj_get(written, "nodes"), 0);
        av_value *inputs = av_obj_get(node_value, "inputs");
        av_value *amount_value = av_obj_get(inputs, "Amount");
        av_obj_remove_key(inputs, "Amount");
        av_obj_set(doc, inputs, "Amount", amount_value);
        graph *loaded = graph_from_value(written);
        node *copy = loaded ? graph_node_by_id(loaded, sender->id) : NULL;
        int copied_amount = copy ? serialize_pin_by_key(copy, 0, "Amount") : -1;
        int copied_origin = copy ? serialize_pin_by_key(copy, 0, "Source") : -1;
        test_check(
            copy && copied_amount >= 0 && copied_origin >= 0 &&
                copy->input_pins[copied_amount].id == sender->input_pins[amount].id &&
                copy->input_pins[copied_origin].id == sender->input_pins[origin].id &&
                copy->output_pins[0].id == sender->output_pins[0].id,
            "Reordering keyed payload fields preserves pin IDs and "
            "continuation identity"
        );
        if (loaded) {
            graph_free(loaded);
            free(loaded);
        }
        av_doc_free(doc);
        graph_free(&source);
    }

    /* Editor pin labels persist separately from internal identifiers and pin IDs. */
    {
        graph source;
        graph_init(&source);
        node *sum = graph_add_node(&source, registry_node_by_name("Add"));
        uint64_t input_id = sum->input_pins[0].id;
        uint64_t output_id = sum->output_pins[0].id;
        const char *output_name = sum->output_pins[0].name;
        test_check(
            graph_set_pin_display_name(&source, sum, 0, 0, "projectile.age") == 0,
            "graph pins accept a presentation-only display label"
        );
        char label[64];
        ed_pin_label(sum, &sum->input_pins[0], label, sizeof label);
        test_check(!strcmp(label, "projectile.age"), "ordinary input pins show their authored display label verbatim");
        test_check(
            graph_set_pin_display_name(&source, sum, 1, 0, "calculated total") == 0,
            "graph output pins accept a presentation-only display label"
        );
        ed_pin_label(sum, &sum->output_pins[0], label, sizeof label);
        test_check(!strcmp(label, "calculated total"), "ordinary output pins show their authored display label verbatim");
        test_check(
            sum->input_pins[0].id == input_id && !strcmp(sum->input_pins[0].name, "A") &&
                sum->output_pins[0].id == output_id &&
                !strcmp(sum->output_pins[0].name, output_name),
            "setting display labels keeps input/output names and IDs unchanged"
        );
        test_check(
            graph_set_pin_display_name(&source, sum, 0, 0, "") == 0,
            "an empty display label resets to the pin's internal-name fallback"
        );
        ed_pin_label(sum, &sum->input_pins[0], label, sizeof label);
        test_check(!strcmp(label, "A"), "empty display labels retain ordinary identifier formatting");
        test_check(
            graph_set_pin_display_name(&source, sum, 0, 0, "projectile.age") == 0,
            "display label is restored for the serialization check"
        );
        char *before = graph_save(&source);
        graph *copy = before ? graph_load(before) : NULL;
        node *copied_sum = copy ? graph_node_by_id(copy, sum->id) : NULL;
        char *after = copy ? graph_save(copy) : NULL;
        test_check(
            copied_sum && copied_sum->input_pins[0].id == input_id &&
                !strcmp(copied_sum->input_pins[0].name, "A") &&
                copied_sum->input_pins[0].display_name &&
                !strcmp(copied_sum->input_pins[0].display_name, "projectile.age") &&
                copied_sum->output_pins[0].id == output_id &&
                !strcmp(copied_sum->output_pins[0].name, output_name) &&
                copied_sum->output_pins[0].display_name &&
                !strcmp(copied_sum->output_pins[0].display_name, "calculated total"),
            "graph documents roundtrip display labels without changing pin "
            "identity"
        );
        test_check(before && after && !strcmp(before, after), "display label graph save/load/save is deterministic");
        free(before);
        free(after);
        if (copy) {
            graph_free(copy);
            free(copy);
        }
        graph_free(&source);
    }

    /* Behavior Expression pin labels retain their language-facing names. */
    {
        behavior_asset *asset = behavior_asset_new("PinLabels");
        behavior_expression_error error = { 0 };
        node *expression = behavior_expression_add(asset, asset->graph, "speed * 2", &error);
        char label[64];
        test_check(expression && expression->input_count == 1, "expression pin label fixture is created");
        if (expression && expression->input_count == 1) {
            ed_pin_label(expression, &expression->input_pins[0], label, sizeof label);
            test_check(!strcmp(label, "speed"), "expression input labels preserve source identifiers");
            test_check(
                graph_set_pin_display_name(asset->graph, expression, 0, 0, "velocity source") ==
                    0,
                "expression inputs accept presentation labels"
            );
            ed_pin_label(expression, &expression->input_pins[0], label, sizeof label);
            test_check(!strcmp(label, "velocity source"), "expression input display labels remain verbatim");
        }
        behavior_asset_free(asset);
    }

    /* 9. comprehensive v3 authored-field round trip. */
    {
        graph full;
        graph_init(&full);
        test_check(graph_set_metadata(&full, "RoundTrip", "all authored fields") == 0, "behavior_asset metadata is authored");
        /* In-memory authored defaults across every pin width on a hand-written
         * all-types fixture. The fixture is not registry-loadable, so the
         * byte-identical round trip below uses a separate loadable graph. */
        node *all = graph_add_node(&full, &g_all_types_desc);
        test_check(all != NULL, "comprehensive fixture node created");
        CF_V3 *vec = graph_own_block(&full, sizeof *vec);
        if (vec) {
            *vec = (CF_V3){ 1.25f, 2.5f, 3.75f };
        }
        pin_value values[] = {
            { .type = PIN_BOOL, .b = true },
            { .type = PIN_I8, .i8 = -8 },
            { .type = PIN_U8, .u8 = 8 },
            { .type = PIN_I16, .i16 = -16 },
            { .type = PIN_U16, .u16 = 16 },
            { .type = PIN_I32, .i = -32 },
            { .type = PIN_U32, .u = 32 },
            { .type = PIN_I64, .i64 = -64 },
            { .type = PIN_U64, .u64 = 64 },
            { .type = PIN_F32, .f = 9.5f },
            { .type = PIN_F64, .d = 10.5 },
            { .type = PIN_STR, .str = "authored" },
            { .type = PIN_STRUCT, .type_name = "CF_V3", .ptr = vec },
            { .type = PIN_ENUM, .e = 3 },
        };
        for (int i = 0; all && i < 14; i++) {
            test_check(graph_set_pin_default(&full, all, 1, i, &values[i]) == 0, "every authored pin default is set");
        }
        if (all) {
            test_check(graph_set_node_reference(&full, all, 4242, "Target") == 0, "node reference fields are authored");
            test_check(graph_set_node_disabled(&full, all, 1) == 0, "disabled state is authored");
            all->x = 12.25f;
            all->y = -7.5f;
        }
        /* The all-widths fixture is not registry-loadable, so drop it before
         * the byte-identical round trip, which uses loadable nodes only. */
        test_check(graph_remove_node(&full, all) == 0, "all-widths fixture removed before round trip");

        /* Loadable fixture: corrected literals, a Make node for the struct
         * authored default, both edge kinds, an extended group, and node
         * fields, all registry-loadable for the byte-identical round trip. */
        node *bool_lit = graph_add_node(&full, registry_node_by_name("Bool"));
        node *byte_lit = graph_add_node(&full, registry_node_by_name("Byte"));
        node *int_lit = graph_add_node(&full, registry_node_by_name("Integer"));
        node *int64_lit = graph_add_node(&full, registry_node_by_name("Integer64"));
        node *real_lit = graph_add_node(&full, registry_node_by_name("Real"));
        node *name_lit = graph_add_node(&full, registry_node_by_name("Name"));
        node *str_lit = graph_add_node(&full, registry_node_by_name("String"));
        node *make_node = graph_add_node(&full, make);
        node *event = graph_add_node(&full, registry_node_by_name("Event"));
        node *sequence = graph_add_node(&full, registry_node_by_name("Sequence"));
        node *sink = graph_add_node(&full, registry_node_by_name("Print String"));
        node *sum = graph_add_node(&full, add);
        test_check(
            bool_lit && byte_lit && int_lit && int64_lit && real_lit && name_lit &&
                str_lit && make_node && event && sequence && sink && sum,
            "comprehensive fixture nodes created"
        );
        node *literal_nodes[] = {
            bool_lit, byte_lit, int_lit, int64_lit, real_lit, name_lit, str_lit,
        };
        pin_value lit_defaults[] = {
            { .type = PIN_BOOL, .b = true },
            { .type = PIN_U8, .u8 = 8 },
            { .type = PIN_I32, .i = -32 },
            { .type = PIN_I64, .i64 = -64 },
            { .type = PIN_F64, .d = 10.5 },
            { .type = PIN_NAME, .str = "AuthoredName" },
            { .type = PIN_STR, .str = "authored" },
        };
        for (int i = 0; i < 7; i++) {
            test_check(
                graph_set_pin_default(&full, literal_nodes[i], 0, 0, &lit_defaults[i]) == 0,
                "every authored pin default is set"
            );
        }
        test_check(graph_set_pin_default(&full, make_node, 1, 0, &values[12]) == 0, "struct authored default is set");
        if (bool_lit) {
            test_check(graph_set_node_reference(&full, bool_lit, 4242, "Target") == 0, "node reference fields are authored");
            test_check(graph_set_node_disabled(&full, bool_lit, 1) == 0, "disabled state is authored");
            bool_lit->x = 12.25f;
            bool_lit->y = -7.5f;
        }
        test_check(graph_set_event_name(&full, event, "Full") == 0, "event reference name is serialized");
        pin_spec added = { "Then4", PIN_FLOW, NULL, PIN_EXEC };
        int added_index = sequence ? graph_add_pin(&full, sequence, 1, &added) : -1;
        test_check(added_index >= 2, "extended Sequence pin is present");
        test_check(
            graph_connect(&full, real_lit, 0, sum, 0) == 0 &&
                graph_connect(&full, str_lit, 0, sink, 1) == 0,
            "data edge endpoints use pin IDs"
        );
        test_check(
            graph_connect_exec(&full, event, 0, sequence, 0, 11) == 0 &&
                graph_connect_exec(&full, sequence, added_index, sink, 0, 13) == 0,
            "exec edge order is serialized"
        );
        char *full_a = graph_save(&full);
        graph *full_b = full_a ? graph_load(full_a) : NULL;
        char *full_c = full_b ? graph_save(full_b) : NULL;
        graph *full_d = full_c ? graph_load(full_c) : NULL;
        char *full_e = full_d ? graph_save(full_d) : NULL;
        test_check(
            full_b && full_d && full_a && full_c && full_e &&
                strcmp(full_a, full_c) == 0 && strcmp(full_c, full_e) == 0,
            "all pin types, edges, groups, and node fields round-trip "
            "byte-identically"
        );
        if (full_b) {
            node *loaded_bool = graph_node_by_id(full_b, bool_lit->id);
            node *loaded_str = graph_node_by_id(full_b, str_lit->id);
            node *loaded_make = graph_node_by_id(full_b, make_node->id);
            test_check(
                loaded_bool && loaded_bool->reference_id == 4242 &&
                    strcmp(loaded_bool->reference_name, "Target") == 0 &&
                    loaded_bool->disabled && loaded_bool->x == 12.25f,
                "optional node fields survive first load"
            );
            test_check(
                loaded_bool && loaded_str && loaded_make &&
                    loaded_bool->input_pins[0].default_value.b &&
                    loaded_str->input_pins[0].default_value.str &&
                    strcmp(loaded_str->input_pins[0].default_value.str, "authored") == 0 &&
                    loaded_make->output_pins[0].default_value.ptr &&
                    ((CF_V3 *)loaded_make->output_pins[0].default_value.ptr)->y == 2.5f,
                "scalar, string, and struct instance pin fields survive "
                "first load"
            );
        }
        node *saved_first = full.nodes[0];
        node *saved_second = full.nodes[1];
        full.nodes[0] = saved_second;
        full.nodes[1] = saved_first;
        char *reordered = graph_save(&full);
        graph *reordered_loaded = reordered ? graph_load(reordered) : NULL;
        test_check(
            reordered && full_a && strcmp(reordered, full_a) == 0 && reordered_loaded,
            "node array position carries no reference meaning"
        );
        test_check(full_a && reordered && strcmp(full_a, reordered) == 0, "deterministic output is byte-identical");
        full.nodes[0] = saved_first;
        full.nodes[1] = saved_second;
        if (reordered_loaded) {
            graph_free(reordered_loaded);
            free(reordered_loaded);
        }
        free(reordered);
        if (full_d) {
            graph_free(full_d);
            free(full_d);
        }
        if (full_b) {
            graph_free(full_b);
            free(full_b);
        }
        free(full_a);
        free(full_c);
        free(full_e);
        graph_free(&full);
    }

    /* 10. string constant round trip */
    {
        graph g4;
        graph_init(&g4);
        const node_desc *cs = registry_node_by_name("String");
        test_check(cs != NULL, "registry exposes String literal");
        node *s1 = graph_add_node(&g4, cs);
        pin_value hello = { .type = PIN_STR,
                            .type_name = "const char *",
                            .str = "hello, world" };
        test_check(graph_set_pin_default(&g4, s1, 0, 0, &hello) == 0, "string const: authored value set");

        char *json = graph_save(&g4);
        test_check(json != NULL, "string const: serialize");
        if (json) {
            graph *g5 = graph_load(json);
            test_check(g5 && acount(g5->nodes) == 1, "string const: load");
            if (g5) {
                test_check(
                    g5->nodes[0]->input_pins[0].type == PIN_STR &&
                        strcmp(g5->nodes[0]->input_pins[0].default_value.str, "hello, world") ==
                            0,
                    "string const: value round-trips"
                );
                graph_free(g5);
                free(g5);
            }
            free(json);
        }
        graph_free(&g4);
    }

    /* 10. typed struct arrays, including nested vector color values. */
    {
        graph ramp_graph;
        graph_init(&ramp_graph);
        node *ramp = graph_add_node(&ramp_graph, registry_node_by_name("Shader Color Ramp"));
        shader_ramp_stop stops[] = { { 0.0f, { 0.12f, 0.34f, 0.56f, 0.78f } },
                                     { 1.0f, { 0.91f, 0.73f, 0.55f, 0.37f } } };
        test_check(ramp && shader_node_ramp_set(&ramp_graph, ramp, stops, 2), "typed struct array default is authored");
        char *json = graph_save(&ramp_graph);
        graph *loaded = json ? graph_load(json) : NULL;
        shader_ramp_stop actual[SHADER_RAMP_MAX];
        int actual_count = loaded ? shader_node_ramp_get(loaded->nodes[0], actual) : 0;
        test_check(loaded && actual_count == 2, "typed struct array default reloads");
        test_check(
            loaded && actual_count == 2 && actual[0].position == stops[0].position &&
                actual[0].color[2] == stops[0].color[2] &&
                actual[1].position == stops[1].position &&
                actual[1].color[3] == stops[1].color[3],
            "nested vector values survive array serialization"
        );
        graph_free(loaded);
        free(loaded);
        free(json);
        graph_free(&ramp_graph);
    }

    /* 10. load error handling: malformed source, missing fields, unknown type,
     * out-of-range pin. */
    {
        const char *bad[] = {
            "package asset\npub Build :: () -> byte[] {",
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"nodes\", data.List([]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"version\", data.Value.Int(3)),\n"
            "            data.Field.Named(\"id_counter\", data.Value.Int(1)),\n"
            "            data.Field.Named(\"name\", data.Value.Text(\"\")),\n"
            "            data.Field.Named(\"description\", "
            "data.Value.Text(\"\")),\n"
            "            data.Field.Named(\"nodes\", data.List([\n"
            "                data.Object([\n"
            "                    data.Field.Named(\"type\", "
            "data.Value.Text(\"nope\"))\n"
            "                ])\n"
            "            ])),\n"
            "            data.Field.Named(\"edges\", data.List([]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
            "package asset\n"
            "\n"
            "import data \"game:assets\"\n"
            "\n"
            "pub Build :: () -> byte[] {\n"
            "    return data.Encode(\n"
            "        data.Object([\n"
            "            data.Field.Named(\"version\", data.Value.Int(3)),\n"
            "            data.Field.Named(\"nodes\", data.List([\n"
            "                data.Object([\n"
            "                    data.Field.Named(\"type\", "
            "data.Value.Text(\"Add\"))\n"
            "                ])\n"
            "            ])),\n"
            "            data.Field.Named(\"edges\", data.List([]))\n"
            "        ])\n"
            "    )\n"
            "}\n",
        };
        test_check(graph_load(bad[0]) == NULL && graph_last_error()[0] != '\0', "malformed JSON rejected");
        test_check(graph_load(bad[1]) == NULL && graph_last_error()[0] != '\0', "missing edges array rejected");
        test_check(graph_load(bad[2]) == NULL && strstr(graph_last_error(), "unknown"), "unknown node type rejected");
        test_check(graph_load(bad[3]) == NULL, "malformed pin list rejected");
        test_check(
            graph_load(
                "package asset\n"
                "\n"
                "import data \"game:assets\"\n"
                "\n"
                "pub Build :: () -> byte[] {\n"
                "    return data.Encode(\n"
                "        data.Object([\n"
                "            data.Field.Named(\"version\", "
                "data.Value.Int(2)),\n"
                "            data.Field.Named(\"nodes\", data.List([])),\n"
                "            data.Field.Named(\"edges\", data.List([]))\n"
                "        ])\n"
                "    )\n"
                "}\n"
            ) == NULL &&
                strstr(graph_last_error(), "version"),
            "unsupported schema version rejected"
        );
    }

    /* 11. pin_value_to_string formatting */
    {
        pin_value fv = { .type = PIN_F32, .f = 2.5f };
        pin_value five = { .type = PIN_F32, .f = 5.0f };
        pin_value sv = { .type = PIN_STR, .str = "hello" };
        pin_value bv = { .type = PIN_BOOL, .b = true };
        pin_value iv = { .type = PIN_I32, .i = -42 };

        const char *s_f32 = pin_value_to_string(&fv);
        const char *s_five = pin_value_to_string(&five);
        const char *s_str = pin_value_to_string(&sv);
        const char *s_bool = pin_value_to_string(&bv);
        printf("  fmt samples: f32 2.5 -> \"%s\", f32 5.0 -> \"%s\", str -> \"%s\", bool -> \"%s\"\n", s_f32, s_five, s_str, s_bool);

        test_check(strcmp(s_f32, "2.5") == 0, "fmt: f32 2.5 -> \"2.5\"");
        test_check(strcmp(s_five, "5") == 0, "fmt: f32 5.0 -> \"5\"");
        test_check(strcmp(s_str, "hello") == 0, "fmt: str -> \"hello\"");
        test_check(strcmp(s_bool, "true") == 0, "fmt: bool true -> \"true\"");
        test_check(strcmp(pin_value_to_string(&iv), "-42") == 0, "fmt: i32 -42 -> \"-42\"");
    }

    /* 14. subset serialization: only selected nodes + their inter-edges */
    {
        graph g2;
        graph_init(&g2);

        const node_desc *real_desc = registry_node_by_name("Real");
        node *c1 = graph_add_node(&g2, real_desc);
        graph_set_pin_default(
            &g2,
            c1,
            0,
            0,
            &(pin_value){ .type = PIN_F64, .type_name = "double", .d = 2.0 }
        );
        node *c2 = graph_add_node(&g2, real_desc);
        graph_set_pin_default(
            &g2,
            c2,
            0,
            0,
            &(pin_value){ .type = PIN_F64, .type_name = "double", .d = 3.0 }
        );
        node *ad = graph_add_node(&g2, add);
        graph_connect(&g2, c1, 0, ad, 0);
        graph_connect(&g2, c2, 0, ad, 1);
        c1->x = 10.0f;
        c1->y = 20.0f;
        c2->x = 30.0f;
        c2->y = 40.0f;
        ad->x = 50.0f;
        ad->y = 60.0f;

        /* Expanded, so c2 stays outside the subset instead of folding into ad. */
        graph_set_node_presentation(&g2, c1, NODE_PRESENTATION_EXPANDED);
        graph_set_node_presentation(&g2, c2, NODE_PRESENTATION_EXPANDED);
        node *subset[] = { c1, ad }; /* c2 is outside the subset */
        char *json = graph_save_nodes(&g2, subset, 2);
        test_check(json != NULL, "subset: serialize");
        if (json) {
            printf("\n--- serialized subset ---\n%s--- end ---\n", json);

            graph *g3 = graph_load(json);
            test_check(g3 != NULL, "subset: load");
            if (g3) {
                test_check(acount(g3->nodes) == 2, "subset: node count is 2");
                test_check(acount(g3->edges) == 1, "subset: crossing edge dropped");
                test_check(g3->nodes[0]->x == 10.0f && g3->nodes[0]->y == 20.0f, "subset: node 0 position");
                test_check(g3->nodes[1]->x == 50.0f && g3->nodes[1]->y == 60.0f, "subset: node 1 position");
                test_check(g3->nodes[0]->input_pins[0].default_value.d == 2.0, "subset: literal value survives");
                graph_free(g3);
                free(g3);
            }
            free(json);
        }
        graph_free(&g2);
    }

    /* 14. append: paste a serialized subgraph with a position offset */
    {
        graph g4;
        graph_init(&g4);

        const node_desc *real_desc = registry_node_by_name("Real");
        node *c1 = graph_add_node(&g4, real_desc);
        graph_set_pin_default(
            &g4,
            c1,
            0,
            0,
            &(pin_value){ .type = PIN_F64, .type_name = "double", .d = 2.0 }
        );
        node *c2 = graph_add_node(&g4, real_desc);
        graph_set_pin_default(
            &g4,
            c2,
            0,
            0,
            &(pin_value){ .type = PIN_F64, .type_name = "double", .d = 3.0 }
        );
        node *ad = graph_add_node(&g4, add);
        graph_connect(&g4, c1, 0, ad, 0);
        graph_connect(&g4, c2, 0, ad, 1);
        c1->x = 10.0f;
        c1->y = 20.0f;
        ad->x = 50.0f;
        ad->y = 60.0f;
        graph_set_node_presentation(&g4, c1, NODE_PRESENTATION_EXPANDED);
        graph_set_node_presentation(&g4, c2, NODE_PRESENTATION_EXPANDED);

        node *subset[] = { c1, ad };
        char *json = graph_save_nodes(&g4, subset, 2);
        test_check(json != NULL, "append: serialize source subset");
        if (json) {
            graph g5;
            graph_init(&g5);
            test_check(acount(g5.nodes) == 0, "append: fresh graph starts empty");
            test_check(graph_append(&g5, json, 100.0f, 200.0f) == 0, "append: returns 0");
            test_check(acount(g5.nodes) == 2, "append: node count");
            test_check(acount(g5.edges) == 1, "append: edge count");
            test_check(g5.nodes[0]->x == 110.0f && g5.nodes[0]->y == 220.0f, "append: node 0 offset");
            test_check(g5.nodes[1]->x == 150.0f && g5.nodes[1]->y == 260.0f, "append: node 1 offset");
            test_check(g5.nodes[0]->input_pins[0].default_value.d == 2.0, "append: literal survives");

            int n_before = acount(g5.nodes);
            test_check(
                graph_append(&g5, "package asset\npub Build :: () -> byte[] {", 0.0f, 0.0f) ==
                    -1,
                "append: bad json fails"
            );
            test_check(acount(g5.nodes) == n_before, "append: graph untouched on error");

            graph_free(&g5);
            free(json);
        }
        graph_free(&g4);
    }

    eval_context_free(&eval_ctx);
}
