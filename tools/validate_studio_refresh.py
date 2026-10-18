#!/usr/bin/env python3
"""Temporary paired prototype bridge evidence; reviewed hosted validation PR only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
# Deliberately fail closed until coordinator supplies the reviewed published pair.
FINAL = '613ecd0bdfda72a5dd33a68228bceb6bb854b332'
ENGINE = '7bc6aa3cd69b29cfb006aab94d50d695007d9abe'
PRIOR_ENGINE_EVIDENCE = {'run_id': 39102355375,
 'validation_revision': 'aba0cc5e0fd6745f6e1dbd100b50c0073051df2b',
 'engine_revision': '7bc6aa3cd69b29cfb006aab94d50d695007d9abe',
 'native_artifact_sha256': '3c94d7a5015a617c7ffea05db6efd8bc7bfa8d4108eb9f04785bc50b85b9da77',
 'native_log_sha256': {'engine-green': 'bcdae5a815f879d296cf007b0ca60721cb01abbca7d01b82c4554bfe7171088a',
                       'engine-asan': '02e9b7c25996727745d4118612659e31377a4cc6fca6c90bcaa2e04468c73e08',
                       'engine-red': 'e923b9d4998cece98f0b3ee7d80ccb404ce635d2a90140ef61b4629832940193'},
 'wasm_job_id': 117601042506,
 'wasm_job_log_sha256': '2ce3d3246f8423db1b46f93679bf22f5fe5cec3b6d9a3c6c3a89c34d433ccf92',
 'completed_contracts': ['installed Debug SDK green: 8 cases / 79 assertions',
                         'root ASan and fatal UBSan green: 8 cases / 79 assertions',
                         'prior-writer red: intended duplicate-detach assertion at source_assets.c:1110',
                         'serial Wasm green: 8 cases / 79 assertions'],
 'scope': 'Prior run evidence only; these engine invocations are not repeated by this Studio resume.'}
PRODUCT = 'src/editor/studio/studio_shell.c'
SUCCESS = 'Studio source edit accepted: frames=4 edits=1 undo=1 redo=1'
PROTOTYPE_SUCCESS = 'Studio prototype source ownership accepted: frames=2 prototypes=4 actors=3 checkpoints=1'
EDGE_SUCCESS = 'Prototype source edge controls: deleted_source_retained=1 queued_commands_quiet=1'
REVIEWED_FILES = {'.github/engine-ref': '0aa450a62d7457af7dab66b47d1555806707bbd621c6d7a2c909dfb4fe967875',
 'EXTRACTION.json': '6ba07d702b911204cfb26fe9c257f03dd93f82a8308acdd22e36f32619d84bd9',
 'SDK_HEADERS.json': '56e445f1f418a6f5e838ecb05645748c40167e3ec5a146871e78ddedb02d874b',
 'src/editor/documents/asset_code.c': '88d4fc52daae2127fc78b212c8999b3adbc65ea88c8ec8899809093f6d700909',
 'src/editor/documents/asset_code.h': '4b616b73004ef470c3337bbf67ee2605aa2df210225398e227e8c6636395b619',
 'src/editor/documents/scene_disk.c': '61df3e4276a2d24f311727aea2056a5e314a15f6f3ea9eb0366f320e78db5e65',
 'src/editor/documents/scene_documents.c': '829844d7c5f026835c33b608f2a4611586bf8b87959674a5586ca2c0a87acee2',
 'src/editor/documents/scene_history.c': 'c85f0c20e977bb1e679a6904b0812eee01e148b2f1390469f6a509355bccc7ab',
 'src/editor/documents/scene_history.h': 'b4ff9050dd63cb59ad410efb7aa9a384eca64ff549766597535a8eba2e63946d',
 'src/editor/documents/scene_prototype_actions.c': '4a55ebdaf715daeed794bd822a3351885be67a806c0fb83f6b12aede1ad352e2',
 'src/editor/documents/scene_prototypes.c': '26c08d26cde9b9449accdfaf8b756778d317db3006ab08c9f22b10b15cca41bb',
 'src/editor/studio/studio_code.c': '9f26268c88307106361751c353c9e82cccd5975159cd7e8a717feace28bb6182',
 'src/editor/studio/studio_internal.h': '54b4f8f26d01c397d4894f79328a94acaf1b4f59510791e6accedd05cb99d29d',
 'src/editor/studio/studio_properties.c': 'c669abd6d4e6e8af5320f1777e77a58c2d0bb1930ee4563d5758a7754e9b4135',
 'src/editor/studio/studio_prototype_actions.c': '61f768872d015bb3d9ea9e140a077434b86cb5e29e88607cd408fd730fb5373f',
 'src/editor/studio/studio_shell.c': '755390847f70234eb8a1c23037ef05b506aba0623bb8b3614e44564cf09d2595',
 'src/editor/workspace/scene_actors.c': '2e18d5ca42dcfb77671a0937bca2d0018f865239e27db13dfdac2caa4296f9b4',
 'src/editor/workspace/scene_authoring.c': 'f93a8765c7453c3baa53e2bc733b9d640d0634204b6fd7c049e9478d23c55f39',
 'src/editor/workspace/scene_checkpoint.c': 'b2062bf8a02e5404a72e467ec6b44972d48b3900b931422e0553bcb39b711dc0',
 'src/editor/workspace/scene_internal.h': '78f8b3fc4c404b0f758074a6db44d922c4847847f2095b8eee35c65d6b52481d',
 'src/editor/workspace/scene_tabs.c': '50db79500f0ee4ec32835da2a9b61592c63da0b056ce8e6fdf766abacbdecf80',
 'tests/acceptance/README.md': 'bcb64567844980b9263248513f37428ccbcc7a17bc7e0fa07a6edb3f01038a48',
 'tests/acceptance/main.c': '6385e9bc9cd9d43cd96c3ca6a2299eb82722d8ba1051489507d528726f0db4ab',
 'tests/acceptance/prototype_source.h': 'cfdf9387c01484c89fc8af1d483248d7422020f8e6fad98d4b5d7a8f54099049',
 'tests/acceptance/scene_move.h': '9a84ba7af42c74f384f0bd4db432b4d29aad66a580317c22a3f35bcdb29e07f2',
 'tests/acceptance/source_discard.h': '38eb2fb91b2385b8e98e0694e78fda83ee68e7ab62979712169663c5af55187b',
 'src/editor/collab/actions_scene.c': '08ac118bcd628864a736e991a5c60607a2ede32bc367af3544e353c4c751e8ed',
 'src/editor/workspace/scene_authoring.h': '73d7890233f253eca58066ce9aa1b44b4ac2c47f9a30a4ec42ff457dd89e3c47'}
ENGINE_REVIEWED_FILES = {'src/authoring/source_prototype_edit.h': '94f01e0783d40fe3b574c94ba07850eb89356692d083d575c6a338ee6195017e',
 'src/authoring/source_prototype_edit.c': 'f980580b3f3e93a0b5cf52ce50990ca4a9604c336458b5e632379029901061bc',
 'sdk/source_authoring.cmake': 'd9f20ed5ea7f59e68cbc8681d52de1934e2af3af0514f853d0b5a0911191e61c',
 'test/language/source_assets.c': 'ad88162a2898936e3331a557de2e46236b13f420bd6aebc888cda9e9b3915750',
 'test/language/main.c': 'b3e03cb31493e89cd9b421a42828373a4adf540161a1c05c596843cd44c966f0',
 'tools/test_policy.py': '899913e620109fc172e9c882a7d529df2996efbb5a581f95e641c239f50c0f6a',
 'docs/source-prototype-edits.md': '608a97f3334eae719909f9a88a4200bedb07461999d402f503049d82cbeba9b1'}
ENGINE_CASES = ['source_edit_replaces_only_the_normalized_value_span', 'source_edit_sparse_component_keeps_other_base_fields_live', 'source_edit_nested_axis_obeys_existing_whole_record_semantics', 'source_edit_add_uses_explicit_creation_payload_and_local_remove', 'source_edit_inherited_remove_preserves_children_links_and_behavior_order', 'source_edit_behavior_splices_and_inherited_detach', 'source_edit_typed_batch_and_null_symbolic_link', 'source_edit_batch_rejections_have_no_partial_result']
ENGINE_WRITER = 'src/authoring/source_prototype_edit.c'
ENGINE_MUTATION = {'replacements': [{'before': '    }\n'
                             '    return replace(c, s, (int)ts_node_start_byte(node), '
                             '(int)ts_node_end_byte(node), "");\n'
                             '}\n'
                             'static char *remove_behavior(context *c, const snapshot *s, av_value *array, '
                             'const char *name)\n'
                             '{\n'
                             '    TSNode node = value_node(s, array);\n'
                             '    text_edit *edits = NULL;\n'
                             '    size_t index = 0;\n'
                             '    bool removed = false;\n'
                             '    for (uint32_t i = 0; i < ts_node_child_count(node); i++) {\n'
                             '        TSNode child = ts_node_child(node, i);\n'
                             '        if (is(child, "comment")) {\n'
                             '            continue;\n'
                             '        }\n'
                             '        if (ts_node_is_named(child)) {\n'
                             '            if (index >= av_arr_size(array)) {\n'
                             '                afree(edits);\n'
                             '                fail(c, "Behavior array has inconsistent source items");\n'
                             '                return NULL;\n'
                             '            }\n'
                             '            removed = !strcmp(av_get_str(av_arr_get(array, index++)), name);\n'
                             '        } else if (!is(child, ",")) {\n'
                             '            continue;\n'
                             '        }\n'
                             '        if (removed) {\n'
                             '            apush(edits, ((text_edit){ (int)ts_node_start_byte(child), '
                             '(int)ts_node_end_byte(child), "" }));\n'
                             '        }\n'
                             '    }\n'
                             '    /* A comma belongs to its preceding value, so each deleted value/comma is\n'
                             '     * visited once. A surviving last value may retain a legal trailing '
                             'comma.\n'
                             '     * Comments and surviving values stay byte-for-byte in their original '
                             'order. */\n'
                             '    if (index != av_arr_size(array) || !acount(edits)) {\n'
                             '        afree(edits);\n'
                             '        fail(c, "Behavior array has no matching source items");\n'
                             '        return NULL;\n'
                             '    }\n'
                             '    char *source = text_edits_apply(s->source, edits, acount(edits));\n'
                             '    afree(edits);\n'
                             '    return source;\n'
                             '}\n'
                             'static bool write_path(void *data, const type_desc *type, const char *path, '
                             'pin_value value)\n'
                             '{\n',
                   'after': '    }\n'
                            '    return replace(c, s, (int)ts_node_start_byte(node), '
                            '(int)ts_node_end_byte(node), "");\n'
                            '}\n'
                            'static char *remove_array_item(context *c, const snapshot *s, av_value *array, '
                            'size_t index)\n'
                            '{\n'
                            '    TSNode node = value_node(s, array);\n'
                            '    av_source at = av_get_source(av_arr_get(array, index));\n'
                            '    int start = (int)at.start, end = (int)at.end;\n'
                            '    TSNode comma_before = { 0 }, comma_after = { 0 };\n'
                            '    for (uint32_t i = 0; i < ts_node_child_count(node); i++) {\n'
                            '        TSNode child = ts_node_child(node, i);\n'
                            '        if (is(child, ",") && ts_node_end_byte(child) <= at.start) {\n'
                            '            comma_before = child;\n'
                            '        }\n'
                            '        if (is(child, ",") && ts_node_start_byte(child) >= at.end) {\n'
                            '            comma_after = child;\n'
                            '            break;\n'
                            '        }\n'
                            '    }\n'
                            '    /* Delete punctuation separately so comments between values survive. */\n'
                            '    text_edit edits[2] = { { start, end, "" } };\n'
                            '    TSNode comma = !ts_node_is_null(comma_after) ? comma_after : comma_before;\n'
                            '    int count = 1;\n'
                            '    if (!ts_node_is_null(comma)) {\n'
                            '        edits[count++] = (text_edit){ (int)ts_node_start_byte(comma),\n'
                            '                                      (int)ts_node_end_byte(comma),\n'
                            '                                      "" };\n'
                            '    }\n'
                            '    (void)c;\n'
                            '    return text_edits_apply(s->source, edits, count);\n'
                            '}\n'
                            'static bool write_path(void *data, const type_desc *type, const char *path, '
                            'pin_value value)\n'
                            '{\n'},
                  {'before': '                *materialized = true;\n'
                             '            }\n'
                             '            av_value *behaviors = av_obj_get(av_doc_root(s->doc), '
                             '"behaviors");\n'
                             '            source = remove_behavior(c, s, behaviors, op->behavior);\n'
                             '            break;\n'
                             '        }\n'
                             '    }\n',
                   'after': '                *materialized = true;\n'
                            '            }\n'
                            '            av_value *behaviors = av_obj_get(av_doc_root(s->doc), '
                            '"behaviors");\n'
                            '            for (size_t i = 0; i < av_arr_size(behaviors); i++) {\n'
                            '                if (!strcmp(av_get_str(av_arr_get(behaviors, i)), '
                            'op->behavior)) {\n'
                            '                    source = remove_array_item(c, s, behaviors, i);\n'
                            '                    break;\n'
                            '                }\n'
                            '            }\n'
                            '            break;\n'
                            '        }\n'
                            '    }\n'}],
 'sha256': '745a7f422fe218a5b23053f0e367c0abdddd0d6948912b4b578636fe93620110',
 'git_blob': '1586d46923a741f1ccbf9f60d3f9323b2079d2f8',
 'reference_revision': 'c4a8b09a837a737126b8b560b3ff67af9b7af9c2'}
ENGINE_ASSERTION = 'source_assets\\.c \\(1110\\): !prototype_has_graph\\(bridge_result\\.value, "First"\\)\\)'
STUDIO_CONTROL = {'product': 'src/editor/studio/studio_properties.c',
 'before': '            int result = scene_prototype_source_edit(\n'
           '                s->scene,\n'
           '                asset,\n'
           '                &change,\n'
           '                1,\n'
           '                !strcmp(s->prototype_edit, key)\n'
           '            );',
 'after': '            (void)change;\n'
          '            int result = prototype_write_field(asset->prototype, desc->name, field->name, '
          'value);\n'
          '            asset->dirty = 1;',
 'sha256': '744608a305b19d66f56b08e4998901cbed119f19fd68a9db27cd2364b0f8471d',
 'assertion': 'prototype_source\\.h \\(221\\): source_owned_gesture\\)'}
EDGE_CONTROL = {'files': {'src/editor/documents/asset_code.c': {'replacements': [{'before': '    if (c->file_missing) {\n'
                                                                             '        return true;\n'
                                                                             '    }\n',
                                                                   'after': ''}],
                                                 'sha256': '0ea4457eff2412b302778b6ac4df411c57a72094bbf73c280a4024b808b6f100',
                                                 'git_blob': 'c18d862521bdc15765bc37006e5f38cf99120e89'},
           'src/editor/studio/studio_properties.c': {'replacements': [{'before': '        if (added == '
                                                                                 'APP_PROTOTYPE_EDIT_NOT_APPLIED) '
                                                                                 '{',
                                                                       'after': '        '
                                                                                'scene_prototype_asset '
                                                                                '*asset = '
                                                                                'find_prototype(scene, '
                                                                                'target);\n'
                                                                                '        if (added > 0 && '
                                                                                'asset && '
                                                                                'asset->source_not_applied) '
                                                                                '{'},
                                                                      {'before': '    if (result == '
                                                                                 'APP_PROTOTYPE_EDIT_PENDING) '
                                                                                 '{\n'
                                                                                 '        return;\n'
                                                                                 '    }\n'
                                                                                 '    if (result < 0) {',
                                                                       'after': '    if (result < 0 || '
                                                                                '(result > 0 && '
                                                                                '!asset->source_not_applied)) '
                                                                                '{'},
                                                                      {'before': '    if (result == '
                                                                                 'APP_PROTOTYPE_EDIT_NOT_APPLIED) '
                                                                                 '{',
                                                                       'after': '    if (result > 0) {'}],
                                                     'sha256': '1d9f1bacedd2e8e5a1efb4c76fefb4158720afc3b511213c956fd4415e4fefa2',
                                                     'git_blob': 'f22f83217e288a875d9bdf3a05937170a3298de1'}},
 'assertion': 'prototype_source\\.h \\(693\\): deleted_source_retained && queued_commands_quiet\\)',
 'required_markers': ['Prototype source edge controls: deleted_source_retained=0 queued_commands_quiet=0']}
CONTROLS = {
    'ownership-red': {'files': {STUDIO_CONTROL['product']: STUDIO_CONTROL}, 'assertion': STUDIO_CONTROL['assertion']},
    'edges-red': EDGE_CONTROL,
}
PROHIBITED = ('Studio UI:', 'runtime frame failed', 'Could not initialize')
INFRA = {'.github/workflows/ci.yml', 'tools/validate_studio_refresh.py'}
POLICY = ('tools/agent.py', 'tools/test_policy.py', 'test/execution_selectors.h')
# The sole tracked link in FINAL's reviewed Git tree; never dereference it in evidence.
SOURCE_LINKS = {'CLAUDE.md': {'type': 'symlink', 'git_mode': '120000',
                             'target': 'AGENTS.md',
                             'git_blob': '47dc3e3d863cfb5727b87d785d09abf9743c0a72'}}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def canonical(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def git(root, *arguments):
    return subprocess.check_output(['git', '-C', str(root), *arguments], text=True).strip()


def clean_checkout(root, revision):
    require(git(root, 'rev-parse', 'HEAD') == revision, f'Wrong checkout: {root}')
    require(not git(root, 'status', '--porcelain', '--untracked-files=normal'), f'Dirty checkout: {root}')


def snapshot(root, names, links=None):
    names = set(names)
    if links is not None:
        require(set(links) <= names, 'Missing reviewed source link')
        for directory, directories, files in os.walk(root, followlinks=False):
            if Path(directory) == root:
                directories[:] = [name for name in directories if name != '.git']
            for name in directories + files:
                path = Path(directory) / name
                if path.is_symlink():
                    relative = path.relative_to(root).as_posix()
                    require(relative in names and relative in links,
                            'Unreviewed or untracked source link: ' + relative)
    result = {}
    for name in sorted(names):
        path = root / name
        if links is not None and name in links:
            expected = links[name]
            require(path.is_symlink(), 'Source link type changed: ' + name)
            target = os.readlink(path)
            require(target == expected['target'], 'Source link target changed: ' + name)
            destination = path.parent / target
            require(not Path(target).is_absolute() and '..' not in Path(target).parts and
                    destination.relative_to(root).as_posix() in names and
                    destination.is_file() and not destination.is_symlink() and
                    root.resolve() in destination.resolve().parents,
                    'Source link target is not a tracked in-tree regular file: ' + name)
            content = os.fsencode(target)
            blob = hashlib.sha1(b'blob ' + str(len(content)).encode() + b'\0' + content).hexdigest()
            require(blob == expected['git_blob'], 'Source link Git blob changed: ' + name)
            result[name] = dict(expected)
            continue
        require(not path.is_symlink() and path.is_file(), f'Non-regular source: {name}')
        result[name] = digest(path)
    return result


def tracked(root):
    return set(filter(None, git(root, 'ls-files', '-z').split('\0')))


def assertions_enabled(arguments):
    defined = False
    tokens = iter(arguments)
    for token in tokens:
        if token in ('-D', '-U'):
            token += next(tokens, '')
        if token == '-DNDEBUG' or token.startswith('-DNDEBUG='):
            defined = True
        elif token == '-UNDEBUG':
            defined = False
    return not defined


def extraction(source):
    value = json.loads((source / 'EXTRACTION.json').read_text())
    for name, row in value['files'].items():
        # sha256 is historical extraction provenance; current_source_sha256 hashes
        # raw engine bytes before consumer replacements. Only consumer_sha256
        # asserts current Studio bytes; the complete source has its own Git snapshot.
        if 'consumer_sha256' not in row:
            continue
        checksum = row['consumer_sha256']
        require(isinstance(checksum, str) and re.fullmatch(r'[0-9a-f]{64}', checksum),
                'Malformed consumer checksum: ' + name)
        require(digest(source / name) == checksum,
                f'Extraction checksum differs: {name}')
    return value


def reviewed_pins():
    for name, value in (('Studio', FINAL), ('engine', ENGINE)):
        require(re.fullmatch(r'[0-9a-f]{40}', value), 'Final reviewed ' + name + ' SHA has not been supplied')
    require(REVIEWED_FILES and ENGINE_REVIEWED_FILES, 'Reviewed source identities have not been supplied')
    for variant in CONTROLS.values():
        require(variant['files'] and variant['assertion'] != 'PENDING', 'Studio control is not frozen')
        require(all(marker != 'PENDING' for marker in variant.get('required_markers', [])),
                'Studio control diagnostics are not frozen')
    for control in (ENGINE_MUTATION, *(mutation for variant in CONTROLS.values()
                                      for mutation in variant['files'].values())):
        require(re.fullmatch(r'[0-9a-f]{64}', control['sha256']), 'Reviewed mutation has not been supplied')
        for change in control.get('replacements', [control]):
            require(change['before'] != change['after'] and change['before'] != 'PENDING', 'Mutation is not frozen')
    require(STUDIO_CONTROL['assertion'] != 'PENDING', 'Ownership negative assertion is not frozen')


def mutate(content, control):
    result = content
    for change in control.get('replacements', [control]):
        require(result.count(change['before']) == 1, 'Mutation requires exactly one reviewed original span')
        result = result.replace(change['before'], change['after'])
    data = result.encode()
    require(hashlib.sha256(data).hexdigest() == control['sha256'], 'Mutation bytes differ from review')
    if 'git_blob' in control:
        blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        require(blob == control['git_blob'], 'Counterfactual writer differs from published Git blob')
    return result


def source_inputs(args, controls=True):
    reviewed_pins()
    clean_checkout(args.source, FINAL)
    clean_checkout(args.engine, ENGINE)
    require((args.source / '.github/engine-ref').read_text().strip() == ENGINE, 'Engine pin changed')
    for root, reviewed in ((args.source, REVIEWED_FILES), (args.engine, ENGINE_REVIEWED_FILES)):
        for name, checksum in reviewed.items():
            require(digest(root / name) == checksum, 'Unreviewed source: ' + name)
    source_names = tracked(args.source)
    final_files = snapshot(args.source, source_names, SOURCE_LINKS)
    validation_files = snapshot(args.validation, tracked(args.validation), SOURCE_LINKS)
    require({k: v for k, v in final_files.items() if k not in INFRA} ==
            {k: v for k, v in validation_files.items() if k not in INFRA},
            'Validation child changes files outside its two infrastructure files')
    require(digest(Path(__file__).resolve()) == validation_files['tools/validate_studio_refresh.py'],
            'Running helper differs from validation checkout')
    final_manifest = extraction(args.source)
    for name in REVIEWED_FILES.keys() & final_manifest['files'].keys():
        require(final_manifest['files'][name].get('consumer_sha256') == digest(args.source / name),
                'Edited extracted file lacks current consumer checksum: ' + name)
    if not controls:
        save(args.output / 'evidence/source-identity.json', {
            'final_revision': FINAL, 'final_tree': git(args.source, 'rev-parse', 'HEAD^{tree}'),
            'engine_revision': ENGINE, 'engine_tree': git(args.engine, 'rev-parse', 'HEAD^{tree}'),
            'engine_reviewed_files': ENGINE_REVIEWED_FILES, 'final_files': final_files})
        return {}, final_files
    sources = {}
    records = {}
    for label, control in CONTROLS.items():
        red = args.output / (label + '-source')
        red.mkdir()
        for name in source_names:
            target = red / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.source / name, target, follow_symlinks=False)
        manifest = json.loads(json.dumps(final_manifest))
        for product, mutation in control['files'].items():
            require(product in source_names and product in final_manifest['files'],
                    'Studio control lacks tracked extraction provenance: ' + product)
            (red / product).write_text(mutate((red / product).read_text(), mutation))
            manifest['files'][product]['consumer_sha256'] = mutation['sha256']
        save(red / 'EXTRACTION.json', manifest)
        require(extraction(red) == manifest, 'Studio counterfactual extraction audit failed')
        red_files = snapshot(red, source_names, SOURCE_LINKS)
        changed = {name for name in source_names if red_files[name] != final_files[name]}
        require(changed == set(control['files']) | {'EXTRACTION.json'},
                'Studio red has an unintended source difference')
        records[label] = {'files': red_files, 'counterfactual': True, 'dirty': True,
                          'changed_files': sorted(changed), 'control': control}
        sources[label] = red
    save(args.output / 'evidence/source-identity.json', {
        'final_revision': FINAL, 'final_tree': git(args.source, 'rev-parse', 'HEAD^{tree}'),
        'engine_revision': ENGINE, 'engine_tree': git(args.engine, 'rev-parse', 'HEAD^{tree}'),
        'engine_reviewed_files': ENGINE_REVIEWED_FILES, 'final_files': final_files,
        'studio_controls': records,
    })
    return sources, final_files


def sdk_identity(args):
    # Existing production package verifier checks identity, all files and symlinks.
    from package_sdk import previous_package
    manifest = previous_package(args.sdk, 'sdk')
    require(manifest['source']['revision'] == ENGINE and manifest['source']['dirty'] is False,
            'SDK source is not the exact clean engine pin')
    require(manifest['requirements']['system'] == 'Linux', 'SDK is not native Linux')
    require(manifest['build']['configuration'] == 'Debug', 'Acceptance SDK must preserve Debug assertions')
    require(manifest['build']['native_backend'] is True, 'SDK lacks actual SLJIT backend')
    engine_objects = [row for row in manifest['build']['inputs']['objects'].values()
                      if any(arg.startswith('@source/src/') for arg in row['arguments'])]
    require(engine_objects and all(assertions_enabled(row['arguments']) for row in engine_objects),
            'SDK engine object assertions are disabled or unproved')
    require(all(not any(arg.startswith('-fsanitize=') for arg in row['arguments']) for row in engine_objects),
            'Unexpected sanitized SDK for bounded acceptance')
    result = {'build_id': manifest['build_id'], 'manifest_sha256': digest(args.sdk / 'share/chibi/package.json'),
              'verified_files': len(manifest['files']), 'verified_engine_objects': len(engine_objects),
              'source': manifest['source'], 'configuration': manifest['build']['configuration'],
              'sdk_config_sha256': digest(args.sdk / 'lib/cmake/Chibi/ChibiConfig.cmake')}
    save(args.output / 'evidence/sdk-identity.json', result)
    return result


def build_command(args, name, command):
    path = args.output / 'logs' / (name + '.log')
    with path.open('w') as log:
        print('+ ' + shlex.join(command), flush=True)
        code = subprocess.run(command, cwd=args.engine, stdout=log, stderr=subprocess.STDOUT).returncode
    if code:
        print(path.read_text()[-8000:], end='', flush=True)
    require(code == 0, f'{name} failed with exit {code}; see its build log')
    return code


def within(path, root):
    return path == root or root in path.parents


def evidence_bounds(evidence, compile_bytes):
    require(0 < evidence['binary_bytes'] <= 64 * 1024 * 1024, 'Acceptance binary bound')
    require(0 < compile_bytes <= 64 * 1024 * 1024, 'Compilation evidence bound')
    require(8 <= len(evidence['files']) <= 32 and
            all(0 <= row['bytes'] <= 256 * 1024 for row in evidence['files'].values()),
            'Acceptance source inventory bound')
    resources = evidence['resources']
    require(set(resources) == {'studio', 'ui'}, 'Unexpected resource inventory')
    for row in resources.values():
        require(isinstance(row, list) and len(row) == 3 and
                type(row[0]) is int and 0 < row[0] <= 320 and
                type(row[1]) is int and 0 < row[1] <= 1024 * 1024 and
                isinstance(row[2], str) and re.fullmatch(r'[0-9a-f]{64}', row[2]),
                'Resource preflight bound')
    require(len((json.dumps(evidence, indent=2, sort_keys=True) + '\n').encode()) <= 1024 * 1024,
            'Acceptance evidence bound')


def build_variant(args, name, source, sdk):
    build = args.output / (name + '-build')
    configured = build_command(args, name + '-configure', [
        'cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Debug', '-DCHIBI_STUDIO_BUILD_ACCEPTANCE=ON',
        '-DCMAKE_EXE_LINKER_FLAGS=-Wl,--strip-debug',
        '-DChibi_DIR=' + str(args.sdk / 'lib/cmake/Chibi')])
    built = build_command(args, name + '-build', [
        'cmake', '--build', str(build), '--target', 'studio-acceptance', '--parallel', '2'])
    compile_file = build / 'compile_commands.json'
    require(compile_file.is_file() and not compile_file.is_symlink() and
            0 < compile_file.stat().st_size <= 64 * 1024 * 1024, 'Compilation evidence bound')
    commands = json.loads(compile_file.read_text())
    require(commands, 'Missing compilation evidence')
    own_entries = 0
    for entry in commands:
        arguments = entry.get('arguments') or shlex.split(entry['command'])
        file = (Path(entry['directory']) / entry['file']).resolve()
        if within(file, source):
            own_entries += 1
            require(assertions_enabled(arguments), 'Studio assertions disabled')
        require(not within(file, args.engine) or within(file, args.sdk), 'Private engine source compiled')
        tokens = iter(arguments)
        for token in tokens:
            include = None
            include_flags = ('-isystem', '-iquote', '-idirafter', '-include', '-imacros', '-I')
            require(not token.startswith('@'), 'Unexpanded compiler response file')
            if token in include_flags:
                include = next(tokens, '')
            else:
                for flag in include_flags:
                    if token.startswith(flag) and len(token) > len(flag):
                        include = token[len(flag):]
                        break
            if include is not None:
                path = (Path(entry['directory']) / include).resolve()
                require(not within(path, args.engine) or within(path, args.sdk),
                        f'Private engine source include: {path}')
    require(own_entries, 'No Studio compilation entries')
    link = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands',
                                    'bin/studio-acceptance'], text=True)
    require('-Wl,--strip-debug' in link.splitlines()[-1], 'Missing link-time debug stripping')
    (args.output / 'evidence' / (name + '-build-commands.txt')).write_text(link)
    shutil.copy2(build / 'compile_commands.json', args.output / 'evidence' / (name + '-compile_commands.json'))
    binary = build / 'bin/studio-acceptance'
    require(binary.is_file() and not binary.is_symlink() and 0 < binary.stat().st_size <= 64 * 1024 * 1024,
            'Acceptance binary exceeds unchanged 64 MiB preflight bound')
    paths = list((source / 'tests/acceptance').rglob('*'))
    require(not any(p.is_symlink() for p in paths), 'Acceptance symlink')
    files = {p.relative_to(source).as_posix(): {'bytes': p.stat().st_size, 'sha256': digest(p)}
             for p in paths if p.is_file()}
    # Restore before freezing the resource identity; the fixture migrates this copy.
    shutil.copy2(source / 'projects/studio/project.kdl', build / 'share/chibi/studio/project.kdl')
    resources = {}
    for resource in ('studio', 'ui'):
        root = build / 'share/chibi' / resource
        paths = list(root.rglob('*'))
        require(not any(p.is_symlink() for p in paths), 'Resource symlink')
        require(all(0 < p.stat().st_size <= 1024 * 1024 for p in paths if p.is_file()),
                'Resource file bound')
        members = {p.relative_to(root).as_posix(): digest(p) for p in paths if p.is_file()}
        resources[resource] = [len(members), sum((root / p).stat().st_size for p in members), canonical(members)]
    native = {'source': str(source), 'sdk': str(args.sdk), 'configure_exit': configured,
              'build_exit': built, 'private_engine_source_include': False,
              'sdk_config_sha256': sdk['sdk_config_sha256']}
    evidence = {'source': str(source), 'build': str(build), 'sdk': str(args.sdk),
                'binary': str(binary), 'binary_sha256': digest(binary),
                'binary_bytes': binary.stat().st_size, 'extraction_sha256': digest(source / 'EXTRACTION.json'),
                'compile_commands_sha256': digest(build / 'compile_commands.json'),
                'frames': 4, 'hidden': True, 'window': [1920, 1080], 'success': SUCCESS,
                'files': files, 'resources': resources, 'candidate_revision': FINAL,
                'counterfactual': name in CONTROLS, 'source_dirty': name in CONTROLS, 'variant': name,
                'product_sha256': digest(source / PRODUCT), 'sdk_build_id': sdk['build_id'],
                'assertions_enabled': True, 'private_engine_source_include': False}
    evidence_bounds(evidence, compile_file.stat().st_size)
    evidence_path = args.output / 'evidence' / (name + '-acceptance.json')
    save(evidence_path, evidence)
    save(args.output / 'evidence' / (name + '-native.json'), native)
    return build, evidence_path, native, evidence


def fixture_directories(source):
    prefixes = set()
    for path in (source / 'tests/acceptance').rglob('*'):
        if path.is_file() and path.suffix in ('.c', '.h'):
            prefixes.update(re.findall(r'"/tmp/([^"/]*?)XXXXXX"', path.read_text()))
    return {str(path) for prefix in prefixes for path in Path('/tmp').glob(prefix + '*')}


def supervised(args, name, commands, env):
    from test_policy import run_commands
    path = args.output / 'logs' / (name + '.log')
    fixtures_before = fixture_directories(args.source)
    with path.open('w') as log:
        code = run_commands(commands, seconds=60, env=env, output=log)
    output = path.read_text()
    print(output, end='', flush=True)
    new_fixtures = sorted(fixture_directories(args.source) - fixtures_before)
    save(args.output / 'evidence' / (name + '-result.json'), {
        'exit_code': code, 'log_sha256': digest(path), 'new_hardcoded_tmp_fixtures': new_fixtures})
    require(not new_fixtures, 'Test left scoped /tmp fixtures (retained, not deleted): ' + ', '.join(new_fixtures))
    return code, output


def graphics_environment(args):
    env = os.environ.copy()
    for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'SDL_VIDEODRIVER'):
        env.pop(key, None)
    paths = subprocess.check_output(['dpkg-query', '-L', 'mesa-vulkan-drivers'], text=True).splitlines()
    icds = [Path(p) for p in paths if re.fullmatch(r'lvp_icd.*\.json', Path(p).name)]
    require(len(icds) == 1 and icds[0].is_file(), 'Expected one installed Mesa Lavapipe ICD')
    for directory in ('scratch', 'cache', 'prefs'):
        (args.output / directory).mkdir()
    env.update(VK_DRIVER_FILES=str(icds[0]), SDL_GPU_DRIVER='vulkan', SDL_VIDEO_DRIVER='x11',
               SDL_AUDIODRIVER='dummy', SDL_LOGGING='gpu=info', TMPDIR=str(args.output / 'scratch'),
               XDG_CACHE_HOME=str(args.output / 'cache'), MESA_SHADER_CACHE_DIR=str(args.output / 'cache'),
               CHIBI_PREFS_DIR=str(args.output / 'prefs'))
    code, output = supervised(args, 'vulkan-discovery', [[sys.executable, 'tools/agent.py', 'doctor']], env)
    require(code == 0, 'Vulkan discovery failed; no backend fallback')
    discoveries = []
    for line in output.splitlines():
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(row, dict) and row.get('scope') == 'discovery_only_not_render_validation':
            discoveries.append(row)
    require(len(discoveries) == 1, 'Missing or ambiguous Vulkan doctor result')
    health = discoveries[0]
    require(health.get('backend') == 'Vulkan' and health.get('status') == 'enumerated' and
            health.get('acceleration') == 'software' and health.get('devices') and
            all(row.get('type') == 4 for row in health['devices']),
            'Expected an enumerated software Vulkan device')
    save(args.output / 'evidence/backend.json', {'icd': str(icds[0]), 'icd_sha256': digest(icds[0]),
         'mesa_package': subprocess.check_output(['dpkg-query', '-W', '-f=${Version}', 'mesa-vulkan-drivers'], text=True),
         'environment': {k: env[k] for k in ('VK_DRIVER_FILES', 'SDL_GPU_DRIVER', 'SDL_VIDEO_DRIVER')},
         'discovery': health, 'scope': 'discovery only; acceptance must actually render'})
    return env


def gpu_test(args, name, source, artifact, base_env):
    build, path, native, evidence = artifact
    record = args.engine / '.chibi/architecture/external-studio-native-acceptance.json'
    record.parent.mkdir(parents=True, exist_ok=True)
    save(record, native)
    require(digest(source / 'projects/studio/project.kdl') ==
            digest(build / 'share/chibi/studio/project.kdl'), 'Bundled manifest changed after resource audit')
    require(digest(build / 'bin/studio-acceptance') == evidence['binary_sha256'], 'Binary changed after audit')
    env = dict(base_env, CHIBI_STUDIO_SOURCE=str(source), CHIBI_STUDIO_BUILD=str(build),
               CHIBI_STUDIO_ACCEPTANCE_EVIDENCE=str(path))
    code, output = supervised(args, name + '-source-edit', [[
        'xvfb-run', '-a', sys.executable, 'tools/agent.py', 'gpu-test', '--', '--studio-source-edit']], env)
    reports = []
    for line in output.splitlines():
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(row, dict) and row.get('coverage') == 'focused-iteration':
            reports.append(row)
    require(len(reports) == 1, 'Missing or ambiguous agent execution result')
    report = reports[0]
    require(not any(marker in output for marker in PROHIBITED), 'UI/backend failure is not regression evidence')
    require(code == 0 and report.get('status') == 'passed' and
            report.get('reason') == 'admitted_studio_acceptance_completed' and
            report.get('execution_evidence') == {'frames': 4, 'edits': 1, 'undo': 1, 'redo': 1},
            'Green source-edit did not pass all existing/new assertions')
    require(not any((args.output / 'scratch').iterdir()), 'Supervised test left temporary fixture state')


def cpu_inventory(selector, code, output, cases, assertions):
    require(code == 0 and not any(marker in output for marker in PROHIBITED), selector + ' failed')
    plain = re.sub(r'\x1b\[[0-9;]*m', '', output)
    summaries = re.findall(r'Summary: Passed: (\d+) Failed: (\d+) Total: (\d+) Suites: (\d+) Asserts: (\d+)', plain)
    require(len(summaries) == 1, selector + ' has no unique pico summary')
    passed, failed, total, suites, observed_assertions = map(int, summaries[0])
    require((passed, failed, total, observed_assertions) == (cases, 0, cases, assertions),
            selector + ' case/assertion inventory differs from reviewed admission')
    return {'passed': passed, 'failed': failed, 'total': total, 'suites': suites, 'assertions': observed_assertions}


def compilation(build, sources, sanitized=False):
    commands = json.loads((build / 'compile_commands.json').read_text())
    selected = {}
    for entry in commands:
        file = (Path(entry['directory']) / entry['file']).resolve()
        if file not in sources:
            continue
        arguments = entry.get('arguments') or shlex.split(entry['command'])
        require(assertions_enabled(arguments), 'Selected C assertions are disabled')
        require(not any(arg.startswith('@') for arg in arguments), 'Unexpanded selected compiler command')
        instrumentation = set()
        for arg in arguments:
            if arg.startswith('-fsanitize='):
                instrumentation.update(arg.split('=', 1)[1].split(','))
            require(not arg.startswith('-fno-sanitize='), 'Sanitizer disable flag in selected object')
        require({'address', 'undefined'} <= instrumentation if sanitized else not instrumentation,
                'Selected object instrumentation differs from admitted configuration')
        require(file not in selected, 'Ambiguous selected C compilation')
        selected[file] = entry
    require(set(selected) == sources, 'Selected C source was not compiled')
    return {str(file): entry for file, entry in selected.items()}


def pico_cases(code, output, names, negative_assertion=None):
    plain = re.sub(r'\x1b\[[0-9;]*m', '', output)
    require(not any(marker in plain for marker in (*PROHIBITED, 'ERROR: AddressSanitizer',
                'UndefinedBehaviorSanitizer', 'runtime error:')), 'Infrastructure/sanitizer failure is not assertion evidence')
    observed = re.findall(r'Running: ([A-Za-z0-9_]+)', plain)
    require(observed == names, 'Compiled selector case identity differs from reviewed workload')
    summaries = re.findall(r'Summary: Passed: (\d+) Failed: (\d+) Total: (\d+) Suites: (\d+) Asserts: (\d+)', plain)
    require(len(summaries) == 1, 'Missing unique pico summary')
    passed, failed, total, suites, assertions = map(int, summaries[0])
    require(total == len(names) and passed + failed == total and assertions > 0, 'Wrong pico inventory')
    if negative_assertion is None:
        require(code == 0 and failed == 0 and '(FAILED:' not in plain, 'Candidate assertion workload failed')
    else:
        require(code == 1 and failed > 0 and re.search(negative_assertion, plain),
                'Negative control did not reproduce its reviewed assertion')
    return {'cases': names, 'passed': passed, 'failed': failed, 'total': total,
            'suites': suites, 'assertions': assertions, 'negative_assertion': negative_assertion}


def native_driver(args, label, sdk):
    build = args.output / (label + '-build')
    build_command(args, label + '-configure', [
        'cmake', '-S', str(args.engine / 'test/language'), '-B', str(build), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DCHIBI_INSTALLED_SDK=ON', '-DChibi_DIR=' + str(sdk / 'lib/cmake/Chibi'),
        '-DCMAKE_EXE_LINKER_FLAGS=-Wl,--gc-sections'])
    build_command(args, label + '-build', ['cmake', '--build', str(build), '--target', 'test_language', '--parallel', '2'])
    selected = compilation(build, {args.engine / 'test/language/main.c', args.engine / 'test/language/source_assets.c'})
    source_command = selected[str(args.engine / 'test/language/source_assets.c')]
    arguments = source_command.get('arguments') or shlex.split(source_command['command'])
    require('-DCHIBI_TEST_SOURCE_ASSETS' in arguments, 'Source API dispatcher was compiled out')
    # Only the new public-API fixture is asserted to avoid private headers. The
    # existing whole dispatcher includes deliberate unrelated private tests.
    tokens = iter(arguments)
    for token in tokens:
        include = next(tokens, '') if token in ('-I', '-isystem', '-iquote') else None
        for prefix in ('-isystem', '-iquote', '-I'):
            if token.startswith(prefix) and len(token) > len(prefix):
                include = token[len(prefix):]
                break
        if include is not None:
            path = (Path(source_command['directory']) / include).resolve()
            require(not within(path, args.engine / 'src'), 'Public API fixture includes private engine source')
    link = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'test_language'], text=True)
    require(str(sdk / 'lib/libchibi_source_catalog.a') in link.splitlines()[-1], 'Native dispatcher did not link selected installed source catalog')
    require('-Wl,--gc-sections' in link.splitlines()[-1], 'Missing existing-dispatcher dead stripping')
    binary = build / 'test_language'
    record = {'binary_sha256': digest(binary), 'binary': str(binary), 'sdk': str(sdk),
              'source_catalog_archive_sha256': digest(sdk / 'lib/libchibi_source_catalog.a'),
              'selected_compilation': selected, 'compile_commands_sha256': digest(build / 'compile_commands.json'),
              'counterfactual': label == 'engine-red', 'selected_cases': ENGINE_CASES}
    save(args.output / 'evidence' / (label + '-build.json'), record)
    (args.output / 'evidence' / (label + '-link.txt')).write_text(link.splitlines()[-1] + '\n')
    return build, binary, record


def native_run(args, label, artifact, env, red=False):
    build, binary, record = artifact
    require(digest(binary) == record['binary_sha256'], 'Native dispatcher changed after compilation audit')
    test_env = dict(env, CHIBI_LANGUAGE_BUILD_DIR=str(binary.parent))
    code, output = supervised(args, label, [[sys.executable, 'tools/test_policy.py', 'tool', 'source-prototype-edits']], test_env)
    inventory = pico_cases(code, output, ENGINE_CASES, ENGINE_ASSERTION if red else None)
    save(args.output / 'evidence' / (label + '-inventory.json'), inventory)


def tree_files(root):
    return {p.relative_to(root).as_posix(): {'link': os.readlink(p)} if p.is_symlink() else digest(p)
            for p in root.rglob('*') if p.is_symlink() or p.is_file()}


def mutant_sdk(args):
    # Counterfactual package copy: its untouched manifest deliberately remains
    # invalid for the changed archive. Never label this as a verified SDK.
    red = args.output / 'engine-red-sdk'
    before = tree_files(args.sdk)
    shutil.copytree(args.sdk, red, symlinks=True)
    writer = args.engine / ENGINE_WRITER
    original = writer.read_bytes()
    build = args.output / 'engine-red-catalog-build'
    reflect = args.output / 'engine-red-reflect-build'
    try:
        writer.write_text(mutate(original.decode(), ENGINE_MUTATION))
        build_command(args, 'engine-red-reflect-build', ['make', 'sdk-reflect-build',
            'SDK_REFLECT_BUILD_DIR=' + str(reflect), 'LIBCLANG_ROOT=/usr/lib/llvm-18', 'JOBS=2'])
        # This is the unchanged package-sdk Debug/nonsanitized configuration,
        # building only the existing source-catalog archive for the control.
        build_command(args, 'engine-red-catalog-configure', ['sh', 'tools/configure.sh', 'sdk-native', str(build),
            '-DCMAKE_BUILD_TYPE=Debug', '-DCHIBI_ENABLE_SANITIZERS=OFF',
            '-DCHIBI_SDK_BUILD_COMPILER=ON', '-DCHIBI_SDK_BUILD_NATIVE=ON', '-DCHIBI_SDK_BUILD_DESKTOP=ON',
            '-DCHIBI_REFLECT_EXECUTABLE=' + str(reflect / 'reflect'), '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
        build_command(args, 'engine-red-catalog-build', ['cmake', '--build', str(build),
            '--target', 'chibi_source_catalog', '--parallel', '2'])
        selected = compilation(build, {writer})
        require(git(args.engine, 'diff', '--name-only').splitlines() == [ENGINE_WRITER],
                'Counterfactual engine changed more than the reviewed writer')
        require(digest(writer) == ENGINE_MUTATION['sha256'], 'Compiled mutant writer changed')
        rows = (build / 'chibi_package_targets-Debug.txt').read_text().splitlines()
        require(rows[0] == '1', 'Unexpected package-target inventory format')
        archives = [Path(row.split('\t')[2]) for row in rows[1:]
                    if row.startswith('STATIC_LIBRARY\tchibi_source_catalog\t')]
        require(len(archives) == 1 and archives[0].is_file() and within(archives[0], build),
                'Missing unique configured source-catalog artifact')
        member = 'lib/libchibi_source_catalog.a'
        require(member in before and not (red / member).is_symlink(), 'Installed source catalog is not a regular archive')
        shutil.copy2(archives[0], red / member)
        after = tree_files(red)
        require(set(after) == set(before) and {p for p in before if after[p] != before[p]} == {member},
                'Counterfactual SDK changed anything except source-catalog archive')
        save(args.output / 'evidence/engine-red-sdk.json', {
            'counterfactual': True, 'package_verified': False, 'source_revision': ENGINE, 'source_dirty': True,
            'writer_sha256': digest(writer), 'mutation': ENGINE_MUTATION,
            'original_sdk_manifest_sha256': digest(args.sdk / 'share/chibi/package.json'),
            'changed_archive': member, 'before_sha256': before[member], 'after_sha256': after[member],
            'configured_artifact': str(archives[0]), 'writer_compilation': selected,
            'remaining_package_files_unchanged': True})
    finally:
        writer.write_bytes(original)
    clean_checkout(args.engine, ENGINE)
    return red, build, reflect


def sanitizer_environment(env):
    # Fail rather than silently normalize inherited sanitizer overrides.
    # Leaving ASAN_OPTIONS/LSAN_OPTIONS unset preserves default leak detection.
    for name in ('ASAN_OPTIONS', 'LSAN_OPTIONS', 'UBSAN_OPTIONS'):
        require(not env.get(name), 'Root sanitizer check rejects inherited ' + name)
    result = dict(env)
    for name in ('ASAN_OPTIONS', 'LSAN_OPTIONS', 'UBSAN_OPTIONS',
                 'ASAN_SYMBOLIZER_PATH', 'LSAN_SYMBOLIZER_PATH', 'UBSAN_SYMBOLIZER_PATH',
                 'LLVM_SYMBOLIZER_PATH'):
        result.pop(name, None)
    symbolizer = shutil.which('llvm-symbolizer-18') or shutil.which('llvm-symbolizer')
    require(symbolizer, 'Root sanitizer check requires an available llvm-symbolizer')
    result['ASAN_SYMBOLIZER_PATH'] = symbolizer
    result['LLVM_SYMBOLIZER_PATH'] = symbolizer
    result['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1:external_symbolizer_path=' + symbolizer
    return result


def sanitized_native(args, env):
    env = sanitizer_environment(env)
    build = args.output / 'engine-asan-build'
    # Existing root native defaults are ASan+UBSan. Never override them to make
    # the ownership workload pass; compilation and linking are checked below.
    build_command(args, 'engine-asan-build', ['make', 'build', 'BUILD_DIR=' + str(build),
        'BUILD_TARGET=test_language', 'LIBCLANG_ROOT=/usr/lib/llvm-18', 'JOBS=2'])
    selected = compilation(build, {args.engine / ENGINE_WRITER, args.engine / 'test/language/main.c',
                                  args.engine / 'test/language/source_assets.c'}, sanitized=True)
    link = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'test_language'], text=True).splitlines()[-1]
    require('-fsanitize=address,undefined' in link and '-fno-sanitize=' not in link,
            'Root dispatcher link lacks unchanged ASan/UBSan')
    binary = build / 'test/language/test_language'
    record = {'binary_sha256': digest(binary), 'binary': str(binary), 'selected_compilation': selected,
              'counterfactual': False, 'selected_cases': ENGINE_CASES, 'instrumentation': ['address', 'undefined'],
              'runtime_environment': {name: env.get(name) for name in ('ASAN_OPTIONS', 'LSAN_OPTIONS',
                  'UBSAN_OPTIONS', 'ASAN_SYMBOLIZER_PATH', 'LLVM_SYMBOLIZER_PATH')},
              'asan_leak_detection': 'runtime default, no inherited disabling or suppression options'}
    save(args.output / 'evidence/engine-asan-build.json', record)
    (args.output / 'evidence/engine-asan-link.txt').write_text(link + '\n')
    native_run(args, 'engine-asan', (build, binary, record), env)
    return build


def prototype_child(arguments):
    # Fixture cloning and cleanup share the inherited production deadline with
    # this single two-frame invocation; no fresh budget for its descendant.
    import tempfile
    engine, source, binary, scratch = map(Path, arguments)
    sys.path.insert(0, str(engine / 'tools'))
    from test_policy import run_commands
    from agent import clone_project
    with tempfile.TemporaryDirectory(prefix='prototype-', dir=scratch) as directory:
        fixture = Path(directory) / 'project'
        clone_project(source / 'tests/acceptance/project', fixture)
        return run_commands([['xvfb-run', '-a', str(binary), '--prototype-source', '--project-root', str(fixture)]], seconds=60)


def prototype_test(args, label, source, artifact, env):
    build, _, native, evidence = artifact
    require(digest(build / 'bin/studio-acceptance') == evidence['binary_sha256'], 'Prototype binary changed')
    require(digest(build / 'compile_commands.json') == evidence['compile_commands_sha256'], 'Prototype build inputs changed')
    from agent import studio_acceptance_sources, studio_resource_preflight, external_project
    studio_acceptance_sources(source, evidence['files'])
    studio_resource_preflight(build, evidence['resources'])
    fixture = source / 'tests/acceptance/project'
    members = [p for p in fixture.rglob('*') if p.is_file()]
    require(len(members) == 3 and not any(p.is_symlink() for p in fixture.rglob('*')),
            'Prototype host fixture membership changed')
    require(external_project((fixture / 'project.kdl').read_bytes()), 'Prototype host project changed')
    expected = {
        'content/scenes/Start.kdl': b'version 1\nname "Start"\n(array)entities\n',
        'content/behaviors/Counter.chibi': b'Counter :: behavior {\n    count: retain int = 1\n    on Tick {\n        count = count + 1\n    }\n}\n'}
    require(all((fixture / p).read_bytes() == value for p, value in expected.items()), 'Prototype host source changed')
    code, output = supervised(args, label + '-prototype-source', [[sys.executable, str(Path(__file__).resolve()),
        '--prototype-child', str(args.engine), str(source), str(build / 'bin/studio-acceptance'), str(args.output / 'scratch')]], env)
    red = label in CONTROLS
    control = CONTROLS.get(label)
    inventory = pico_cases(code, output, ['prototype_source_ownership'], control['assertion'] if red else None)
    markers = control.get('required_markers', []) if red else [EDGE_SUCCESS]
    observed_edges = [line for line in output.splitlines() if line.startswith('Prototype source edge controls:')]
    require(observed_edges == markers, 'Studio edge diagnostics differ from the unique reviewed result')
    require(output.splitlines().count(PROTOTYPE_SUCCESS) == (0 if red else 1), 'Prototype completion marker differs')
    save(args.output / 'evidence' / (label + '-prototype-inventory.json'), {
        **inventory, 'selector': '--prototype-source', 'frames_limit': 2,
        'success': PROTOTYPE_SUCCESS, 'required_control_markers': markers,
        'binary_sha256': evidence['binary_sha256'],
        'source_sha256': evidence['files']['tests/acceptance/prototype_source.h']['sha256']})
    require(not any((args.output / 'scratch').iterdir()), 'Prototype fixture cleanup incomplete')
    # Opening Studio can migrate its disposable bundled project manifest. Only
    # this known input is restored before another selector uses the same build.
    shutil.copy2(source / 'projects/studio/project.kdl', build / 'share/chibi/studio/project.kdl')
    studio_resource_preflight(build, evidence['resources'])


def wasm_sdk_identity(args):
    from package_sdk import previous_package
    manifest = previous_package(args.sdk, 'sdk')
    require(manifest['source']['revision'] == ENGINE and manifest['source']['dirty'] is False,
            'Wasm SDK is not the exact clean engine pin')
    require(manifest['requirements']['system'] == 'Emscripten' and
            manifest['requirements']['web_threads'] is False and
            manifest['requirements']['pointer_bits'] == 32 and
            manifest['build']['configuration'] == 'Release' and
            manifest['build']['native_backend'] is False,
            'Wasm SDK differs from the existing serial Release product configuration')
    # Preserve normal Release SDK flags, including NDEBUG. Fixture target
    # assertions are checked independently; root native ASan covers ownership.
    result = {'source': manifest['source'], 'build_id': manifest['build_id'],
              'manifest_sha256': digest(args.sdk / 'share/chibi/package.json'),
              'requirements': {key: manifest['requirements'][key] for key in ('system', 'web_threads', 'pointer_bits')},
              'configuration': manifest['build']['configuration'], 'native_backend': False,
              'configuration_c_flags': manifest['build']['configuration_c_flags'],
              'source_catalog_archive_sha256': digest(args.sdk / 'lib/libchibi_source_catalog.a')}
    save(args.output / 'evidence/wasm-sdk-identity.json', result)
    return result


def wasm_validate(args, final_files, policy_hashes):
    sdk = wasm_sdk_identity(args)
    build = args.output / 'wasm-language-build'
    build_command(args, 'wasm-language-configure', ['sh', 'tools/web_env.sh', 'emcmake', 'cmake',
        '-S', str(args.engine / 'test/language'), '-B', str(build), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DCHIBI_WEB_THREADS=OFF',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DCHIBI_INSTALLED_SDK=ON',
        '-DChibi_DIR=' + str(args.sdk / 'lib/cmake/Chibi')])
    build_command(args, 'wasm-language-build', ['cmake', '--build', str(build),
        '--target', 'test_language', '--parallel', '2'])
    selected = compilation(build, {args.engine / 'test/language/main.c', args.engine / 'test/language/source_assets.c'})
    row = selected[str(args.engine / 'test/language/source_assets.c')]
    arguments = row.get('arguments') or shlex.split(row['command'])
    require('-DCHIBI_TEST_SOURCE_ASSETS' in arguments, 'Wasm source API cases compiled out')
    link = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'test_language'], text=True).splitlines()[-1]
    require(str(args.sdk / 'lib/libchibi_source_catalog.a') in link and '-pthread' not in shlex.split(link),
            'Wasm test did not link the reviewed installed serial SDK')
    javascript, wasm = build / 'test_language.js', build / 'test_language.wasm'
    require(wasm.read_bytes()[:8] == b'\x00asm\x01\x00\x00\x00', 'Missing compiled WebAssembly module')
    files = {p.name: digest(p) for p in (javascript, wasm)}
    save(args.output / 'evidence/wasm-language-build.json', {
        'source_revision': ENGINE, 'sdk_build_id': sdk['build_id'], 'selected_cases': ENGINE_CASES,
        'source_catalog_archive_sha256': sdk['source_catalog_archive_sha256'],
        'selected_compilation': selected, 'compiled_files': files,
        'fixture_assertions_enabled': True, 'counterfactual': False,
        'node_version': subprocess.check_output(['node', '--version'], text=True).strip()})
    (args.output / 'evidence/wasm-language-link.txt').write_text(link + '\n')
    code, output = supervised(args, 'wasm-source-prototype-edits',
        [['node', str(javascript), '--source-prototype-edits']], os.environ.copy())
    save(args.output / 'evidence/wasm-inventory.json', pico_cases(code, output, ENGINE_CASES))
    require(all(digest(build / name) == checksum for name, checksum in files.items()), 'Wasm binary changed during test')
    require(snapshot(args.source, final_files, SOURCE_LINKS) == final_files, 'Studio source changed during Wasm validation')
    clean_checkout(args.source, FINAL)
    clean_checkout(args.engine, ENGINE)
    require(snapshot(args.engine, POLICY) == policy_hashes, 'Production supervisor/admission changed')
    require(wasm_sdk_identity(args) == sdk, 'Installed serial web SDK changed')
    save(args.output / 'summary.json', {'passed': True, 'engine_revision': ENGINE, 'studio_revision': FINAL,
        'sdk_build_id': sdk['build_id'], 'scope': 'one candidate-only serial Wasm/Node eight-case invocation',
        'seconds_limit': 60, 'browser': 'not run', 'full_suite': 'not run', 'product_configuration': 'unchanged Release'})
    shutil.rmtree(build)
    print('Serial installed-SDK Wasm prototype source cases passed; no browser/full suite run.', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'engine', 'sdk', 'validation', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    for name in ('source', 'engine', 'sdk', 'validation', 'output'):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(exist_ok=False)
    for directory in ('logs', 'evidence'):
        (args.output / directory).mkdir()
    os.chdir(args.engine)
    sys.path.insert(0, str(args.engine / 'tools'))
    policy_hashes = snapshot(args.engine, POLICY)
    controls, final_files = source_inputs(args)
    require(ENGINE == PRIOR_ENGINE_EVIDENCE['engine_revision'], 'Prior engine evidence does not cover this pin')
    save(args.output / 'evidence/prior-engine-evidence.json', PRIOR_ENGINE_EVIDENCE)
    sdk = sdk_identity(args)
    env = graphics_environment(args)
    control_builds = []
    for label, source in controls.items():
        artifact = build_variant(args, label, source, sdk)
        control_builds.append(artifact[0])
        prototype_test(args, label, source, artifact, env)
    green_artifact = build_variant(args, 'green', args.source, sdk)
    prototype_test(args, 'green', args.source, green_artifact, env)
    gpu_test(args, 'green', args.source, green_artifact, env)
    binary = green_artifact[0] / 'bin/studio-acceptance'
    policy_source = args.source / 'projects/studio/content/libraries/OutlinerRowPolicies.chibi'
    for selector, extra, cases, assertions in (
            ('--project-open', [], 1, 50), ('--scene-read', [], 6, 1141),
            ('--outliner-row-policies', ['--source', str(policy_source)], 1, 1848)):
        require(digest(binary) == green_artifact[3]['binary_sha256'], 'Green binary changed before CPU selector')
        code, output = supervised(args, selector[2:], [[str(binary), selector, *extra]], env)
        save(args.output / 'evidence' / (selector[2:] + '-inventory.json'),
             cpu_inventory(selector, code, output, cases, assertions))
    require(snapshot(args.source, final_files, SOURCE_LINKS) == final_files,
            'Final source changed during validation')
    clean_checkout(args.source, FINAL)
    clean_checkout(args.engine, ENGINE)
    require(snapshot(args.engine, POLICY) == policy_hashes, 'Production admission/supervisor changed')
    require(sdk_identity(args) == sdk, 'Installed SDK changed during validation')
    require(not any((args.output / 'scratch').iterdir()), 'Temporary fixture cleanup incomplete')
    save(args.output / 'summary.json', {'passed': True, 'final_revision': FINAL, 'engine_revision': ENGINE,
         'scope': 'Studio-only resume',
         'red_controls': ['Studio source ownership', 'Studio deleted-source and queued-command edges'],
         'prior_engine_evidence': PRIOR_ENGINE_EVIDENCE,
         'prototype_source': PROTOTYPE_SUCCESS,
         'green': 'four-frame source-edit accepted',
         'cpu': ['project-open', 'scene-read', 'outliner-row-policies'], 'full_suite': 'not run',
         'rendering': 'Xvfb/X11 with forced Vulkan and verified Mesa software ICD',
         'policy_sha256': policy_hashes, 'sdk_build_id': sdk['build_id'],
         'logging': 'native/prototype raw logs; existing source-edit agent failure tail and success JSON'})
    for path in (*controls.values(), *control_builds, green_artifact[0], args.output / 'scratch',
                 args.output / 'cache', args.output / 'prefs'):
        shutil.rmtree(path)
    print('Reviewed Studio resume controls and clean candidate checks passed; prior engine checks were not repeated.', flush=True)


if __name__ == '__main__':
    try:
        if len(sys.argv) == 6 and sys.argv[1] == '--prototype-child':
            sys.exit(prototype_child(sys.argv[2:]))
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print('Validation blocked/failed: ' + str(error), file=sys.stderr, flush=True)
        sys.exit(1)
