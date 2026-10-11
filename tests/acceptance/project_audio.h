static bool project_sound_file()
{
    char path[1024];
    snprintf(path, sizeof path, "%s/content/sounds", project);
    REQUIRE(mkdir(path, 0777) == 0 || errno == EEXIST);
    snprintf(path, sizeof path, "%s/content/sounds/cue.wav", project);
    int size;
    unsigned char *wav = tone_wav((tone){ 440, 440, 0.03f, 0.4f, 0.5f, 0 }, &size);
    REQUIRE(wav && size > 44);
    FILE *file = fopen(path, "wb");
    REQUIRE(file);
    bool written = fwrite(wav, 1, (size_t)size, file) == (size_t)size;
    free(wav);
    REQUIRE(fclose(file) == 0 && written);
    return true;
}

static bool sound_was_queued(audio *queue)
{
    REQUIRE(audio_sound_count(queue) == 1);
    size_t size;
    const void *data = audio_sound(queue, 0, &size);
    REQUIRE(sound_format_of(data, size) == SOUND_WAV);
    REQUIRE(audio_count(queue) == 1);
    audio_voice voice;
    REQUIRE(audio_take(queue, &voice, 1) == 1);
    REQUIRE(voice.sound == 0 && voice.volume == 0.4f && voice.pan == 0.5f);
    return true;
}

static bool project_audio_frame(void *state)
{
    studio *s = studio_acceptance_studio(state);
    REQUIRE(s && s->scene);
    frames++;
    REQUIRE(frames <= 2);
    if (frames == 2) {
        return true;
    }
    app_scene_authoring *scene = s->scene;
    int played = app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY);
    if (played) {
        fprintf(stderr, "Project sound Play failed: %s\n", scene->message);
    }
    REQUIRE(played == 0);
    REQUIRE(sound_was_queued(engine_audio(scene->runtime)));
    uint64_t generation = audio_generation(engine_audio(scene->runtime));
    REQUIRE(app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_PAUSE) == 0);
    REQUIRE(!engine_is_playing(scene->runtime));
    REQUIRE(audio_generation(engine_audio(scene->runtime)) == generation);
    REQUIRE(app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    REQUIRE(audio_count(engine_audio(scene->runtime)) == 1);
    REQUIRE(app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    audio_voice voice;
    REQUIRE(audio_take(engine_audio(scene->runtime), &voice, 1) == 0);
    REQUIRE(app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_PLAY) == 0);
    REQUIRE(sound_was_queued(engine_audio(scene->runtime)));
    REQUIRE(app_scene_authoring_runtime_command(scene, APP_RUNTIME_SCENE, APP_RUNTIME_STOP) == 0);
    REQUIRE(app_scene_authoring_preview_prototype(scene, "Speaker") == 0);
    REQUIRE(sound_was_queued(
        engine_audio(prototype_preview_runtime(scene->prototype_preview))
    ));
    app_scene_authoring_stop_prototype_preview(scene);
    REQUIRE(app_scene_authoring_load_content(scene) == 0);
    REQUIRE(audio_sound_count(engine_audio(scene->runtime)) == 1);
    REQUIRE(
        audio_sound_count(engine_audio(prototype_preview_runtime(scene->prototype_preview))) == 1
    );
    return true;
}

TEST_CASE(project_audio)
{
    REQUIRE(project_sound_file());
    REQUIRE(
        write_content("behaviors", "ProjectSound.chibi", "behavior ProjectSound {\n    on Begin {\n        play_sound(\"cue\", 0.4)\n    }\n}\n")
    );
    REQUIRE(
        write_content("prototypes", "Speaker.kdl", "name \"Speaker\"\ncomponents { Transform; }\n(array)behaviors \"ProjectSound\"\n")
    );
    REQUIRE(
        write_content("scenes", "Start.kdl", "version 1\nname \"Start\"\n(array)entities { - name=\"Speaker\" prototype=\"Speaker\"; }\n")
    );
    char *args[] = { (char *)executable, "--headless",
                     "--frames",         "2",
                     "--fixed-dt",       "0.016666666666666666",
                     "--project-root",   (char *)project };
    REQUIRE(studio_acceptance_host_main(sizeof args / sizeof *args, args) == 0);
    REQUIRE(callback_ok && frames == 2);
    return true;
}
