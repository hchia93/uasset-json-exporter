# Stream metric probe. Runs inside UnrealEditor via -ExecCmds="py stream_metric_probe.py".
# Replaces AlwaysLoaded sublevels with LevelStreamingDynamic in-memory (never saved),
# starts Simulate, per sublevel measures: unload wall ms, gc ms, load wall ms, worst frame ms.
# Output: Saved/StreamMetric/stream_metric_result.json (incremental, survives crash).

import json
import os
import time
import traceback

import unreal

SETTLE_SEC = 3.0
COOLDOWN_SEC = 2.0
PHASE_TIMEOUT_SEC = 180.0

RESULT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "StreamMetric")
RESULT_PATH = os.path.join(RESULT_DIR, "stream_metric_result.json")


def log(msg):
    unreal.log("[StreamMetric] " + str(msg))


def write_result(state):
    os.makedirs(RESULT_DIR, exist_ok=True)
    with open(RESULT_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(state, f, indent=1, ensure_ascii=False)


def resolve_persistent_level():
    explicit = os.environ.get("UAW_PERSISTENT_LEVEL", "").strip()
    if explicit:
        return explicit

    report_path = os.environ.get("UAW_TOPOLOGY_REPORT", "").strip()
    if not report_path or not os.path.isfile(report_path):
        raise RuntimeError(
            "no persistent level. Set UAW_PERSISTENT_LEVEL, or point UAW_TOPOLOGY_REPORT at an AuditLevelTopology report"
        )

    with open(report_path, encoding="utf-8") as f:
        report = json.load(f)

    hosts = [r["level"] for r in report.get("results", []) if r.get("role") == "PersistentHost"]
    if len(hosts) == 1:
        return hosts[0]
    if not hosts:
        raise RuntimeError("topology report lists no PersistentHost level: " + report_path)
    raise RuntimeError(
        "topology report lists %d PersistentHost levels, set UAW_PERSISTENT_LEVEL to pick one: %s"
        % (len(hosts), ", ".join(hosts))
    )


def set_loaded_visible(sl, value):
    # attr assignment routes through BlueprintSetter; set_editor_property does not
    sl.should_be_loaded = value
    sl.should_be_visible = value


class Probe(object):
    def __init__(self):
        self.persistent = resolve_persistent_level()
        self.result = {"persistent": self.persistent, "status": "running", "levels": []}
        self.level_names = []
        self.index = -1
        self.phase = "boot"
        self.phase_t0 = 0.0
        self.worst_frame_ms = 0.0
        self.current = None
        self.tick_handle = None
        self.game_world = None
        self.uuid_counter = 0

    # editor-side prep, before simulate
    def prepare(self):
        les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if not les.load_level(self.persistent):
            raise RuntimeError("load_level failed: " + self.persistent)
        ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
        world = ues.get_editor_world()

        # collect sublevel package paths, skip persistent
        packages = []
        for level in unreal.EditorLevelUtils.get_levels(world):
            pkg = level.get_path_name().split(".")[0]
            if pkg != self.persistent:
                packages.append(pkg)
        if not packages:
            raise RuntimeError("no sublevels found on " + self.persistent)

        # AlwaysLoaded hard-returns ShouldBeLoaded()==true; only class swap unlocks unloading.
        # remove + re-add as Dynamic, in-memory only, quit without saving.
        for pkg in packages:
            short = pkg.rsplit("/", 1)[-1]
            sl = unreal.GameplayStatics.get_streaming_level(world, short)
            cls = sl.get_class().get_name() if sl else "?"
            log("sublevel %s (%s)" % (short, cls))
            if cls == "LevelStreamingAlwaysLoaded":
                level_obj = sl.get_loaded_level()
                if not unreal.EditorLevelUtils.remove_level_from_world(level_obj):
                    raise RuntimeError("remove_level_from_world failed: " + pkg)
                if unreal.EditorLevelUtils.add_level_to_world(world, pkg, unreal.LevelStreamingDynamic) is None:
                    raise RuntimeError("add_level_to_world failed: " + pkg)
            self.level_names.append(short)

        self.result["level_count"] = len(self.level_names)
        write_result(self.result)
        les.editor_play_simulate()
        self.enter_phase("wait_world")
        self.tick_handle = unreal.register_slate_post_tick_callback(self.tick)

    def enter_phase(self, phase):
        self.phase = phase
        self.phase_t0 = time.perf_counter()
        self.worst_frame_ms = 0.0
        log("phase -> %s (level %d/%d)" % (phase, self.index + 1, len(self.level_names)))

    def elapsed(self):
        return time.perf_counter() - self.phase_t0

    def streaming_level(self):
        return unreal.GameplayStatics.get_streaming_level(self.game_world, self.level_names[self.index])

    def latent_info(self):
        self.uuid_counter += 1
        info = unreal.LatentActionInfo()
        info.uuid = self.uuid_counter
        info.linkage = 0
        info.callback_target = unreal.GameplayStatics.get_all_actors_of_class(self.game_world, unreal.WorldSettings)[0]
        return info

    # attr-setter route can silently fail to dirty the consideration list; after
    # STALL_SEC with no pending state, escalate to the latent kismet API once
    def escalate_if_stalled(self, sl, loading):
        if self.elapsed() < 10.0 or sl.is_streaming_state_pending():
            return
        key = "fallback_load" if loading else "fallback_unload"
        if self.current.get(key):
            return
        self.current[key] = True
        name = self.level_names[self.index]
        log("escalating to latent API (%s) for %s" % ("load" if loading else "unload", name))
        if loading:
            unreal.GameplayStatics.load_stream_level(self.game_world, name, True, False, self.latent_info())
        else:
            unreal.GameplayStatics.unload_stream_level(self.game_world, name, self.latent_info(), False)

    def next_level(self):
        self.index += 1
        if self.index >= len(self.level_names):
            self.finish("done")
            return
        self.current = {"name": self.level_names[self.index]}
        self.result["levels"].append(self.current)
        self.enter_phase("ensure_loaded")

    def finish(self, status, error=None):
        self.result["status"] = status
        if error:
            self.result["error"] = error
        write_result(self.result)
        log("finished: " + status)
        if self.tick_handle is not None:
            unreal.unregister_slate_post_tick_callback(self.tick_handle)
            self.tick_handle = None
        try:
            unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_request_end_play()
        except Exception:
            pass
        unreal.SystemLibrary.quit_editor()

    def tick(self, delta_seconds):
        try:
            self.tick_impl(delta_seconds)
        except Exception:
            self.finish("error", traceback.format_exc())

    def tick_impl(self, delta_seconds):
        frame_ms = delta_seconds * 1000.0
        self.worst_frame_ms = max(self.worst_frame_ms, frame_ms)

        if self.elapsed() > PHASE_TIMEOUT_SEC:
            self.finish("error", "timeout in phase %s at level %s" % (self.phase, self.level_names[self.index] if self.index >= 0 else "-"))
            return

        if self.phase == "wait_world":
            world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()
            if world is not None:
                self.game_world = world
                self.enter_phase("settle")
            return

        if self.phase == "settle":
            if self.elapsed() >= SETTLE_SEC:
                self.next_level()
            return

        if self.phase == "ensure_loaded":
            sl = self.streaming_level()
            if sl is None:
                self.current["error"] = "streaming level not found in game world"
                self.next_level()
                return
            if sl.is_level_visible():
                self.enter_phase("unload_start")
            else:
                # warmup load, unmeasured; real measurement is the load after unload
                set_loaded_visible(sl, True)
                self.enter_phase("warmup_loading")
            return

        if self.phase == "warmup_loading":
            sl = self.streaming_level()
            if sl.is_level_visible():
                self.enter_phase("unload_start")
            else:
                self.escalate_if_stalled(sl, True)
            return

        if self.phase == "unload_start":
            set_loaded_visible(self.streaming_level(), False)
            self.enter_phase("unloading")
            return

        if self.phase == "unloading":
            sl = self.streaming_level()
            if sl.is_level_loaded():
                self.escalate_if_stalled(sl, False)
            if not sl.is_level_loaded():
                self.current["unload_wall_ms"] = round(self.elapsed() * 1000.0, 1)
                self.current["unload_worst_frame_ms"] = round(self.worst_frame_ms, 1)
                gc_t0 = time.perf_counter()
                unreal.SystemLibrary.execute_console_command(self.game_world, "obj gc")
                self.current["gc_ms"] = round((time.perf_counter() - gc_t0) * 1000.0, 1)
                write_result(self.result)
                self.enter_phase("cooldown_after_unload")
            return

        if self.phase == "cooldown_after_unload":
            if self.elapsed() >= COOLDOWN_SEC:
                self.enter_phase("load_start")
            return

        if self.phase == "load_start":
            set_loaded_visible(self.streaming_level(), True)
            self.enter_phase("loading")
            return

        if self.phase == "loading":
            sl = self.streaming_level()
            if not sl.is_level_visible():
                self.escalate_if_stalled(sl, True)
            if sl.is_level_visible():
                self.current["load_wall_ms"] = round(self.elapsed() * 1000.0, 1)
                self.current["load_worst_frame_ms"] = round(self.worst_frame_ms, 1)
                write_result(self.result)
                self.enter_phase("cooldown_after_load")
            return

        if self.phase == "cooldown_after_load":
            if self.elapsed() >= COOLDOWN_SEC:
                self.next_level()
            return


def main():
    # Construction resolves the persistent level and can fail. Keep it inside the guard, an
    # uncaught throw here leaves the editor running until the launcher times out.
    probe = None
    try:
        probe = Probe()
        probe.prepare()
    except Exception:
        result = probe.result if probe else {"status": "error", "levels": []}
        result["status"] = "error"
        result["error"] = traceback.format_exc()
        write_result(result)
        log("probe failed before measurement started")
        unreal.SystemLibrary.quit_editor()


main()
