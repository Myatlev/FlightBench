// Self-tests of the config layers (FB_SELFTEST=all, or layers_view,bench_lines,config_changes,snapshot_export,boot_hint):
//   layers_view      fixture reports → LayeredConfig: the core's config_json (layers array, lines with `ref`, a
//                    top-level `disabled` array, conflicts by `wins`), a config_json block over several lines,
//                    `config:` (unchanged, device lines) beside `config_src` / `config_disabled` / `config_conflict`
//                    without config_json; SettingKey (set / feature / serial / aux, profile context); a report
//                    without layers attributed by the app (the trainer's inav.cli lines, the GPS's device lines, a line it
//                    cannot trace); conflicts computed (later layer wins); the LayersView tree (groups, sources, check
//                    boxes, the conflicts group), a selected line's actions; the edit policy (a preset's file: copy; a
//                    bench of one's own: in place in the data root).
//   bench_lines      a temp benches folder, the trainer preset as a bench, `tools/fake_core.py --session-check` as the
//                    core's check (python): bench lines typed in the box → Save → `./<bench>.cli` + benchCli → the next
//                    session-check has them as layer 5, last, winning its conflict; a device line disabled → disabledLines
//                    → skipped by the next check; an aircraft line edited → the aircraft copied next to the bench
//                    (aircraftFile, firmware_cli → the copy), the repo's inav.cli untouched → the check reads the copy; the
//                    board group's file edited → the bench's copy (boardConfigFile); rename → the files copied; export →
//                    import into another benches folder (firmware_cli re-pointed); delete takes the bench's files.
//   config_changes   a fixture `diff against script` → ConfigChanges (three lists), BenchLineFor; the Config window's
//                    Changes tree and "→ bench lines"; on the screen the fake core's answer to the request.
//   snapshot_export  a spawned `fake_core.py --disarmed` (its own port, stopped by PID): snapshot → the file written
//                    atomically with the diff's lines, `config {snapshot:"written"}`; export; apply events; on the screen:
//                    Snapshot sends the bench's bench_cli path, a written answer reloads layer 5 in the Config window and
//                    in the Edit-bench wizard; Export as preset writes the folder (bench JSON, bench lines, README with
//                    the core's layers).
//   boot_hint        on the screen, the fake core's reboot (exit CLI): "firmware booting — the configurator can connect
//                    once it answers", measured, then "in ~N s"; the tooltip names the green dot; in the top bar.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using Godot;

namespace FlightBench;

public static class LayersTests
{
    public static readonly string[] Names = { "layers_view", "bench_lines", "config_changes", "snapshot_export", "boot_hint" };
    public static readonly string[] ScreenNames = { "config_changes", "snapshot_export", "boot_hint" };
    public static bool ScreenAny => ScreenNames.Any(DevEnv.SelfTest);
    public static readonly Dictionary<string, List<string>> Fails = Names.ToDictionary(n => n, _ => new List<string>());
    public static readonly Dictionary<string, List<string>> Info = Names.ToDictionary(n => n, _ => new List<string>());

    private static void Check(string t, bool ok, string what) { if (!ok) Fails[t].Add(what); }

    public static string FakeCore => Path.Combine(AppState.RepoRoot, "tools", "fake_core.py");

    /// Runs `python tools/fake_core.py <args>` and captures it (the core's check stand-in).
    public static async Task<CoreCheck> RunFake(params string[] args)
    {
        var psi = new ProcessStartInfo("python") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = new UTF8Encoding(false) };
        psi.ArgumentList.Add(FakeCore);
        foreach (var a in args) psi.ArgumentList.Add(a);
        try
        {
            using var p = Process.Start(psi)!;
            var so = p.StandardOutput.ReadToEndAsync();
            var se = p.StandardError.ReadToEndAsync();
            if (!await Task.Run(() => p.WaitForExit(30000))) { try { p.Kill(); } catch (Exception) { } return new CoreCheck { RunError = "fake_core --session-check timed out" }; }
            return CoreCheck.Parse(p.ExitCode, await so, await se);
        }
        catch (Exception e) { return new CoreCheck { RunError = "cannot run python: " + e.Message }; }
    }

    public static Task<CoreCheck> FakeCheck(string session, string root) => RunFake("--session-check", session, root);

    // ---- fixtures ----
    // docs/protocol.md "Validate first": `config:` stays the device lines, unchanged; `config_src:` / `config_disabled:`
    // / `config_conflict:` are the layered textual form; `config_json` is the structured, authoritative one (`ref`
    // 1-based, a separate top-level `disabled` array, `conflicts[].wins` a ref).
    public const string CoreReport = "config: # ublox on uart3 (device config)\nconfig: feature GPS\nconfig: set gps_provider = UBLOX\n"
        + "launch: icount_shift=2 realtime=off control=31250 msp=31235 machine=speedybee-f405-wing\nmanifest: {\"type\":\"manifest\"}\n"
        + "config_cli: 5 lines\nconfig_cli| set looptime = 500\nconfig_cli| set platform_type = AIRPLANE\nconfig_cli| set looptime = 1000\nconfig_cli| feature GPS\nconfig_cli| set gps_provider = MSP\n"
        + "config_src: [board:boards/speedybee-f405-wing/defaults.cli:10] set looptime = 500\n"
        + "config_src: [aircraft:models/trainer/inav.cli:16] set platform_type = AIRPLANE\n"
        + "config_src: [aircraft:models/trainer/inav.cli:18] set looptime = 1000\n"
        + "config_src: [device:device ublox on uart3] feature GPS\n"
        + "config_disabled: [device:device ublox on uart3] set gps_provider = UBLOX (disabled_lines 'set gps_provider = UBLOX')\n"
        + "config_src: [bench:./trainer.cli:1] set gps_provider = MSP\n"
        + "config_conflict: set looptime: [board:boards/speedybee-f405-wing/defaults.cli:10] set looptime = 500 -> [aircraft:models/trainer/inav.cli:18] set looptime = 1000 (the last wins)\n"
        + "config_json: {\"kind\":\"cli\",\"layers\":["
        + "{\"layer\":\"board\",\"file\":\"boards/speedybee-f405-wing/defaults.cli\",\"path\":\"/x/boards/speedybee-f405-wing/defaults.cli\"},"
        + "{\"layer\":\"aircraft\",\"file\":\"models/trainer/inav.cli\",\"path\":\"/x/models/trainer/inav.cli\"},"
        + "{\"layer\":\"bench\",\"file\":\"./trainer.cli\",\"path\":\"/x/bench/trainer.cli\"}],\"lines\":["
        + "{\"text\":\"set looptime = 500\",\"layer\":\"board\",\"source\":\"boards/speedybee-f405-wing/defaults.cli:10\",\"ref\":\"board:1\"},"
        + "{\"text\":\"set platform_type = AIRPLANE\",\"layer\":\"aircraft\",\"source\":\"models/trainer/inav.cli:16\",\"ref\":\"aircraft:1\"},"
        + "{\"text\":\"set looptime = 1000\",\"layer\":\"aircraft\",\"source\":\"models/trainer/inav.cli:18\",\"ref\":\"aircraft:2\"},"
        + "{\"text\":\"feature GPS\",\"layer\":\"device\",\"source\":\"device ublox on uart3\",\"ref\":\"device:1\"},"
        + "{\"text\":\"set gps_provider = MSP\",\"layer\":\"bench\",\"source\":\"./trainer.cli:1\",\"ref\":\"bench:1\"}],"
        + "\"disabled\":[{\"text\":\"set gps_provider = UBLOX\",\"layer\":\"device\",\"source\":\"device ublox on uart3\",\"ref\":\"device:2\",\"by\":\"set gps_provider = UBLOX\"}],"
        + "\"conflicts\":[{\"setting\":\"set looptime\",\"lines\":["
        + "{\"text\":\"set looptime = 500\",\"layer\":\"board\",\"source\":\"boards/speedybee-f405-wing/defaults.cli:10\",\"ref\":\"board:1\"},"
        + "{\"text\":\"set looptime = 1000\",\"layer\":\"aircraft\",\"source\":\"models/trainer/inav.cli:18\",\"ref\":\"aircraft:2\"}],\"wins\":\"aircraft:2\"}]}\n"
        + "defaults: none\n";

    public const string ChangesMessage = "{\"type\":\"diff\",\"t\":12.5,\"against\":\"script\",\"source\":\"request\",\"at\":12.4,\"stale\":false,"
        + "\"sent\":[{\"setting\":\"set nav_fw_bank_angle\",\"value\":\"35\",\"text\":\"set nav_fw_bank_angle = 35\"},{\"setting\":\"set nav_wp_radius\",\"value\":\"800\",\"text\":\"set nav_wp_radius = 800\"},{\"setting\":\"aux 0\",\"value\":\"0 0 1700 2100\",\"text\":\"aux 0 0 0 1700 2100\"}],"
        + "\"current\":\"# diff all\\nset nav_fw_bank_angle = 40\\nset nav_fw_launch_climb_angle = 20\\naux 0 0 0 1800 2100\\n\","
        + "\"changed\":[{\"setting\":\"set nav_fw_bank_angle\",\"script\":\"set nav_fw_bank_angle = 35\",\"firmware\":\"set nav_fw_bank_angle = 40\"},{\"setting\":\"aux 0\",\"script\":\"aux 0 0 0 1700 2100\",\"firmware\":\"aux 0 0 0 1800 2100\"}],"
        + "\"only_firmware\":[\"set nav_fw_launch_climb_angle = 20\",{\"setting\":\"set small_angle\",\"firmware\":\"set small_angle = 25\"}],\"only_script\":[\"set nav_wp_radius = 800\"]}";

    public static void RunPure()
    {
        if (DevEnv.SelfTest("layers_view")) LayersView_();
        if (DevEnv.SelfTest("config_changes")) ChangesModel();
    }

    // ---- layers_view ----
    private static void LayersView_()
    {
        const string t = "layers_view";
        var c = LayeredConfig.Parse(CoreReport);
        Check(t, c.Kind == "cli" && c.Layered && c.Lines.Count == 6, $"config_json: kind {c.Kind}, layered {c.Layered}, {c.Lines.Count} lines");
        // config_json's `lines` (enabled, in script order) come first, its `disabled` array last
        Check(t, c.Lines.Select(l => l.Layer).SequenceEqual(new[] { "board", "aircraft", "aircraft", "device", "bench", "device" }), "layers: " + string.Join(",", c.Lines.Select(l => l.Layer)));
        Check(t, c.Lines[1].Source == "models/trainer/inav.cli:16" && c.Lines[1].File == "models/trainer/inav.cli" && c.Lines[1].FileLine == 16 && c.Lines[3].File == null && c.Lines[3].Source == "device ublox on uart3",
              $"sources: '{c.Lines[1].Source}' → {c.Lines[1].File}:{c.Lines[1].FileLine}; device '{c.Lines[3].Source}' → file {c.Lines[3].File ?? "none"}");
        Check(t, c.Lines[5].Disabled && c.Lines[5].DisabledBy == "set gps_provider = UBLOX" && !c.Text.Contains("gps_provider = UBLOX") && c.Lines[2].Ref == "aircraft:2", $"disabled line skipped in the text, ref {c.Lines[2].Ref}");
        Check(t, c.Conflicts.Count == 1 && c.Conflicts[0].Setting == "set looptime" && c.Conflicts[0].Winner == "aircraft" && !c.ConflictsComputed && c.Lines[0].Overridden && !c.Lines[2].Overridden && c.Lines[2].Conflict == "set looptime",
              $"the core's conflicts: {string.Join("; ", c.Conflicts.Select(x => x.Describe()))}");
        Check(t, c.Summary.Contains("board 1, aircraft 2, profile 0, device 1, bench 1") && c.Summary.Contains("1 disabled") && c.Summary.Contains("layers from the core") && c.Summary.Contains("1 conflict(s), later layer wins"), "summary: " + c.Summary);
        Check(t, c.LayerFiles.Any(x => x.Layer == "aircraft" && x.File == "models/trainer/inav.cli"), "config_json's layers: " + string.Join(", ", c.LayerFiles.Select(x => $"{x.Layer}={x.File}")));
        // a block over several lines
        var block = CoreReport.Replace("config_json: {\"kind\":\"cli\",\"layers\":[", "config_json: {\"kind\":\"cli\",\n\"layers\":[\n");
        var cb = LayeredConfig.Parse(block);
        Check(t, cb.Lines.Count == 6 && cb.Layered && cb.Conflicts.Count == 1, $"config_json block: {cb.Lines.Count} lines, {cb.Conflicts.Count} conflicts");
        // config_src / config_disabled / config_conflict without config_json (an older or stand-in core)
        var ann = LayeredConfig.Parse("config_cli: 3 lines\nconfig_cli| set a = 1\nconfig_cli| feature GPS\nconfig_cli| set a = 2\n"
                                      + "config_src: [board:boards/x/defaults.cli:3] set a = 1\nconfig_src: [aircraft:models/y/inav.cli:9] feature GPS\nconfig_src: [bench:./b.cli:1] set a = 2\n"
                                      + "config_conflict: set a: [board:boards/x/defaults.cli:3] set a = 1 -> [bench:./b.cli:1] set a = 2 (the last wins)\ndefaults: none\n");
        Check(t, ann.Layered && ann.Lines.Select(l => $"{l.Layer}|{l.Source}|{l.Text}").SequenceEqual(new[] { "board|boards/x/defaults.cli:3|set a = 1", "aircraft|models/y/inav.cli:9|feature GPS", "bench|./b.cli:1|set a = 2" })
                  && ann.Conflicts.Count == 1 && ann.Conflicts[0].Winner == "bench", "config_src lines: " + string.Join(" / ", ann.Lines.Select(l => $"{l.Layer}|{l.Source}|{l.Text}")) + $"; conflicts {ann.Conflicts.Count}");
        // `config:` (unchanged: the wired devices' own lines) beside `config_src` / `config_disabled` (the layered view)
        var core = LayeredConfig.Parse("config: # ublox on uart3 (device config)\nconfig: serial 2 +1\nlaunch: control=1 msp=2\nmanifest: {}\n"
                                       + "config_cli: 3 lines\nconfig_cli| set a = 1\nconfig_cli| feature GPS\nconfig_cli| set a = 2\n"
                                       + "config_src: [board:boards/x/defaults.cli:3] set a = 1\nconfig_src: [aircraft:C:/m/inav.cli:9] feature GPS\n"
                                       + "config_disabled: [device:device ublox on uart3] serial 2 +1 (disabled_lines 'serial 2 +1')\nconfig_src: [bench:./b.cli:1] set a = 2\n"
                                       + "config_conflict: set a: [board:boards/x/defaults.cli:3] set a = 1 -> [bench:./b.cli:1] set a = 2 (the last wins)\ndefaults: none\n");
        Check(t, core.Layered && core.Lines.Count == 4 && core.Lines[1].Source == "C:/m/inav.cli:9" && core.Lines[1].File == "C:/m/inav.cli" && core.Lines[3].Disabled && core.DeviceLines.SequenceEqual(new[] { "# ublox on uart3 (device config)", "serial 2 +1" })
                 && core.Conflicts.Count == 1 && core.Conflicts[0].Setting == "set a" && core.Conflicts[0].Winner == "bench",
              "`config:` / `config_src`: " + string.Join(" / ", core.Lines.Select(l => $"{l.Layer}|{l.Source}|{l.Text}{(l.Disabled ? "|off" : "")}")) + $"; conflicts {string.Join("; ", core.Conflicts.Select(x => x.Describe()))}");
        // SettingKey
        var ctx = new Dictionary<string, string>();
        var keys = new[] { "set nav_rth_altitude = 5000", "feature -GPS", "serial 2 +1", "aux 0 0 0 1700 2100", "control_profile 2", "set rc_expo = 30", "control_profile 1", "set rc_expo = 30", "mmix reset", "# c" }
            .Select(x => LayeredConfig.SettingKey(x, ctx) ?? "-").ToList();
        Check(t, keys.SequenceEqual(new[] { "set nav_rth_altitude", "feature GPS", "serial 2", "aux 0", "-", "set rc_expo @control_profile 2", "-", "set rc_expo", "-", "-" }), "setting keys: " + string.Join(", ", keys));
        // a report without layers, attributed by the app: the trainer preset (inav.cli, the GPS's device lines), a line nobody has
        var preset = Presets.List().FirstOrDefault(p => p.Id == "inav-trainer-launch");
        if (preset == null) Check(t, false, "no preset inav-trainer-launch");
        else
        {
            var b = preset.Bench.Clone();
            b.BoardConfig = "set nav_fw_bank_angle = 30\n";
            var inav = File.ReadAllLines(Path.Combine(CoreLocator.DataRoot, "models", "trainer", "inav.cli")).Select(LayeredConfig.Clean).Where(x => x != null).Take(12).ToList();
            var rep = "config: # ublox on uart3 (device config)\nconfig: feature GPS\nconfig: serial 2 +1\nlaunch: control=31250 msp=31235\nmanifest: {}\n"
                      + $"config_cli: {inav.Count + 4} lines\n" + string.Concat(inav.Select(x => $"config_cli| {x}\n")) + "config_cli| set nav_fw_bank_angle = 30\nconfig_cli| feature GPS\nconfig_cli| serial 2 +1\nconfig_cli| set mystery = 1\ndefaults: none\n";
            var inf = LayeredConfig.Parse(rep);
            inf.Infer(b, CoreLocator.DataRoot);
            var firstSet = inf.Lines.First();
            Check(t, !inf.Layered && inf.Inferred && firstSet.Layer == "aircraft" && firstSet.Source.StartsWith("models/trainer/inav.cli:") && firstSet.Inferred, $"inferred: first line {firstSet.Layer} {firstSet.Source}");
            Check(t, inf.Lines.Count(l => l.Layer == "device" && l.Source == "device ublox on uart3") == 2 && inf.Lines.Single(l => l.Text == "set mystery = 1").Layer == "" && inf.Lines.Single(l => l.Text == "set nav_fw_bank_angle = 30").Source == "bench board config (step 4):1",
                  "inferred device / bench / untraced: " + string.Join(" | ", inf.Lines.Skip(inav.Count).Select(l => $"{l.Layer}:{l.Source}:{l.Text}")));
            // inav.cli's 11th line is `set nav_fw_bank_angle = 35`: the pasted board config's 30 (bench, later) wins
            var inf2 = LayeredConfig.Parse(rep);
            inf2.Infer(b, CoreLocator.DataRoot);
            var cf = inf2.Conflicts.FirstOrDefault(x => x.Setting == "set nav_fw_bank_angle");
            Check(t, inf2.ConflictsComputed && cf != null && cf.Lines.Count == 2 && cf.Winner == "bench" && cf.Lines[0].Layer == "aircraft" && cf.Lines[0].Overridden,
                  "computed conflict: " + (cf?.Describe() ?? "none") + " / " + string.Join("; ", inf2.Conflicts.Select(x => x.Describe())));
            Check(t, inf2.Summary.Contains("(found by the app)") && inf2.Summary.Contains("layers inferred by the app"), "inferred summary: " + inf2.Summary);
            // a core without layers ignores disabled_lines: said, not shown as skipped
            b.DisabledLines = new List<string> { "feature GPS" };
            var inf3 = LayeredConfig.Parse(rep); inf3.Infer(b, CoreLocator.DataRoot);
            Check(t, !inf3.Lines.Any(l => l.Disabled) && inf3.Notes.Any(n => n.Contains("does not know disabled_lines")), "a core without layers + disabledLines: " + string.Join(" | ", inf3.Notes));
            // the edit policy: a preset's bench copies, a bench of one's own writes in place
            var pb = preset.Bench.Clone(); pb.Preset = preset.Id; pb.Name = "not-a-preset-name";
            var own = preset.Bench.Clone(); own.Name = "my own bench"; own.Preset = null;
            var inavAbs = Path.Combine(CoreLocator.DataRoot, "models", "trainer", "inav.cli").Replace('\\', '/');
            var (cp, why) = BenchFiles.EditPolicy(pb, inavAbs, CoreLocator.DataRoot);
            var (cp2, why2) = BenchFiles.EditPolicy(own, inavAbs, CoreLocator.DataRoot);
            Check(t, cp && why.Contains("bundled preset") && !cp2 && why2.Contains("in the data root") && why2.Contains("aircraft"), $"edit policy: preset → copy {cp} ('{why}'); own → copy {cp2} ('{why2}')");
        }
        // the tree
        var v = new LayersView { BenchOf = () => new BenchData { Name = "tree test" }, Root = CoreLocator.DataRoot };
        v.Show(c, "");
        var rows = v.Rows();
        Check(t, rows.Count == 6 && rows.Count(r => r.Group.StartsWith("2 · aircraft")) == 2 && rows.Any(r => r.Group.StartsWith("4 · device") && r.Text.StartsWith("✕") && !r.On)
                 && rows.Any(r => r.Group.StartsWith("5 · bench") && r.Source == "./trainer.cli:1") && rows.Count(r => r.Conflict) == 2,
              "tree rows: " + string.Join(" | ", rows.Select(r => $"{r.Group.Split(' ')[0]}:{r.Text}:{r.Source}:{(r.On ? "on" : "off")}{(r.Conflict ? ":conflict" : "")}")));
        v.SelectLine(c.Lines[0]);
        Check(t, v.SelectionText.Contains("conflict") && v.SelectionText.Contains("does NOT win"), "selected overridden line: " + v.SelectionText);
        v.Free();
        Info[t].Add("config_json (+ block, layers array), `config:` (unchanged) beside `config_src`/`config_disabled`/`config_conflict` (a skipped one), setting keys with profiles, inferred layers (trainer), computed conflicts, the tree, the edit policy");
        Report(t);
    }

    // ---- config_changes (model) ----
    private static void ChangesModel()
    {
        const string t = "config_changes";
        using var d = JsonDocument.Parse(ChangesMessage);
        var m = d.RootElement;
        Check(t, ConfigChanges.IsChanges(m), "the message is not recognised as a diff against the script");
        using (var plain = JsonDocument.Parse("{\"type\":\"diff\",\"text\":\"# diff all\\n\",\"source\":\"request\"}")) Check(t, !ConfigChanges.IsChanges(plain.RootElement), "a plain diff taken for changes");
        var c = ConfigChanges.FromMessage(m);
        Check(t, c.ChangedSettings.Count == 2 && c.ChangedSettings[0] == new ConfigChanges.Changed("set nav_fw_bank_angle", "set nav_fw_bank_angle = 35", "set nav_fw_bank_angle = 40") && c.OnlyFirmware.SequenceEqual(new[] { "set nav_fw_launch_climb_angle = 20", "set small_angle = 25" })
                 && c.OnlyScript.SequenceEqual(new[] { "set nav_wp_radius = 800" }) && c.Sent.Split('\n').Length == 3 && c.Current.Contains("nav_fw_launch_climb_angle"),
              $"lists: changed {c.ChangedSettings.Count}, only firmware [{string.Join(", ", c.OnlyFirmware)}], only script [{string.Join(", ", c.OnlyScript)}]");
        Check(t, ConfigChanges.BenchLineFor(c.ChangedSettings[0]) == "set nav_fw_bank_angle = 40" && ConfigChanges.BenchLineFor(c.ChangedSettings[1]) == "aux 0 0 0 1800 2100",
              $"bench lines: {ConfigChanges.BenchLineFor(c.ChangedSettings[0])} / {ConfigChanges.BenchLineFor(c.ChangedSettings[1])}");
        var p = new ConfigPanel(new DiffView());
        p.Layers.BenchOf = () => new BenchData { Name = "changes test" };
        p.ShowChanges(c);
        var rows = p.ChangeRows();
        Check(t, p.ChangesHead.StartsWith("Changes against the script: 2 changed, 2 only in the firmware, 1 only in the script") && rows.Count == 5
                 && rows.Count(r => r.Group.StartsWith("Changed by the configurator (2)")) == 2 && rows.Any(r => r.Group.StartsWith("Only in the firmware") && r.Text == "set small_angle = 25")
                 && rows.Any(r => r.Text == "set nav_fw_bank_angle" && r.Detail == "set nav_fw_bank_angle = 35 → set nav_fw_bank_angle = 40"), "Changes tree: " + p.ChangesHead + " / " + string.Join(" | ", rows.Select(r => $"{r.Text}: {r.Detail}")));
        p.SelectChange("set nav_fw_bank_angle");
        int n = p.TakeIntoBench(selectedOnly: true);
        Check(t, n == 1 && p.Layers.BenchLinesText.Contains("set nav_fw_bank_angle = 40") && !p.Layers.BenchLinesText.Contains("climb_angle") && p.Mode == 0, $"selected → bench lines ({n}): '{p.Layers.BenchLinesText}'");
        n = p.TakeIntoBench(selectedOnly: false);
        Check(t, n == 4 && p.Layers.BenchLinesText.Contains("set nav_fw_launch_climb_angle = 20") && !p.Layers.BenchLinesText.Contains("nav_wp_radius"), $"all → bench lines ({n}), removed ones not: '{p.Layers.BenchLinesText}'");
        p.Free();
        Info[t].Add("fixture: 2 changed / 2 only firmware / 1 only script, BenchLineFor, the Changes tree, selected / all → bench lines");
    }

    /// Reports a test (its model part and its screen part together).
    public static void Report(string name, string extra = "")
    {
        var f = Fails[name];
        SelfTests.Report(name, f.Count == 0, f.Count == 0 ? string.Join("; ", Info[name].Append(extra).Where(x => x != "")) : string.Join("; ", f));
    }
}

/// bench_lines and the snapshot / export model: runner stages (a python fake core, several frames).
public sealed class LayersRun
{
    private int _step;
    private ulong _deadline;
    private string _keepDir = "", _dir = "", _dir2 = "", _exportDir = "";
    private BenchData _b = null!;
    private LayersView _v = null!;
    private Task<(LayeredConfig? Config, string Error)>? _task;
    private int _checks;
    private Dictionary<string, string> _repoHashes = new();
    public bool Done { get; private set; }
    // snapshot (a spawned fake core)
    private Process? _proc;
    private CoreClient? _cc;
    private readonly List<JsonElement> _got = new();
    private readonly List<string> _events = new();
    private string _snapPath = "";
    private int _port;

    private static void Check(string t, bool ok, string what) { if (!ok) LayersTests.Fails[t].Add(what); }

    private Dictionary<string, string> RepoHashes()
    {
        var files = new[] { "models/trainer/inav.cli", "models/trainer/aircraft.json", "boards/speedybee-f405-wing/defaults.cli" };
        return files.ToDictionary(f => f, f => Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(Path.Combine(CoreLocator.DataRoot, f)))));
    }

    public void Poll()
    {
        if (Done) return;
        bool bl = DevEnv.SelfTest("bench_lines"), se = DevEnv.SelfTest("snapshot_export");
        if (!bl && !se) { Done = true; return; }
        try { if (bl && _step < 100) BenchLines(); else if (se && _step < 200) { if (_step < 100) _step = 100; Snapshot(); } else Finish(); }
        catch (Exception e) { LayersTests.Fails[_step < 100 ? "bench_lines" : "snapshot_export"].Add($"threw at step {_step}: {e.GetType().Name}: {e.Message}"); _step = _step < 100 ? (se ? 100 : 200) : 200; Cleanup(); }
    }

    private bool Wait(string t, bool cond, string what)
    {
        if (cond) return true;
        if (Time.GetTicksMsec() < _deadline) return false;
        LayersTests.Fails[t].Add("timed out: " + what);
        _step = t == "bench_lines" ? 90 : 190;
        return false;
    }

    private void StartCheck() { _checks++; _task = LayeredConfig.CheckAsync(_b.Clone(), null, CoreLocator.DataRoot); _deadline = Time.GetTicksMsec() + 30000; }

    private void BenchLines()
    {
        const string t = "bench_lines";
        switch (_step)
        {
            case 0:
            {
                _repoHashes = RepoHashes();
                _keepDir = BenchData.Dir;
                BenchData.Dir = _dir = $"user://selftest-layers-{Time.GetTicksUsec()}";
                Directory.CreateDirectory(BenchFiles.Dir);
                LayeredConfig.Checker = LayersTests.FakeCheck;
                var preset = Presets.List().First(p => p.Id == "inav-trainer-launch");
                _b = preset.Bench.Clone();
                _b.Name = "layers bench"; _b.Preset = preset.Id;
                _v = new LayersView { BenchOf = () => _b, Root = CoreLocator.DataRoot };
                int requested = 0;
                _v.CheckRequested += () => requested++;
                _v.BenchLinesText = "set small_angle = 25\nset nav_fw_launch_climb_angle = 20";
                bool saved = _v.SaveBenchLines();
                var cli = BenchFiles.CliPath(_b);
                Check(t, saved && _b.BenchCli == "./layers_bench.cli" && File.Exists(cli) && File.ReadAllText(cli) == "set small_angle = 25\nset nav_fw_launch_climb_angle = 20\n" && requested == 1,
                      $"Save bench lines: saved {saved}, benchCli {_b.BenchCli}, file {cli} ({(File.Exists(cli) ? File.ReadAllText(cli).Length + " chars" : "missing")}), check requested {requested}");
                var ses = JsonSerializer.Serialize(_b.ToSession(CoreLocator.DataRoot));
                Check(t, ses.Contains("\"bench_cli\":\"" + cli.Replace('\\', '/') + "\""), "session bench_cli: " + ses);
                StartCheck();
                _step = 1;
                break;
            }
            case 1:
            {
                if (!Wait(t, _task!.IsCompleted, "session-check 1")) break;
                var (c, err) = _task.Result;
                if (c == null) { Check(t, false, "session-check 1: " + err); _step = 90; break; }
                var bench = c.Lines.Where(l => l.Layer == "bench").ToList();
                var en = c.Enabled.ToList();
                Check(t, c.Layered && bench.Select(l => l.Text).SequenceEqual(new[] { "set small_angle = 25", "set nav_fw_launch_climb_angle = 20" }) && bench.All(l => l.Source.Contains("layers_bench.cli:"))
                         && en[^2].Text == "set small_angle = 25" && en[^1].Text == "set nav_fw_launch_climb_angle = 20",
                      $"layer 5 in the next check: [{string.Join(", ", bench.Select(l => $"{l.Text} ({l.Source})"))}], last two: {en[^2].Text} / {en[^1].Text}");
                var cf = c.Conflicts.FirstOrDefault(x => x.Setting == "set small_angle");
                Check(t, cf != null && cf.Winner == "bench" && cf.Lines[0].Layer == "aircraft" && cf.Lines[0].Overridden, "small_angle conflict: " + (cf?.Describe() ?? "none"));
                _v.Show(c, "");
                Check(t, _v.Rows().Any(r => r.Group.StartsWith("5 · bench — 2 lines") && r.Source.StartsWith("./layers_bench.cli:")), "the tree's layer 5: " + string.Join(" | ", _v.Rows().Where(r => r.Group.StartsWith("5")).Select(r => r.Source)));
                // disable a device line
                var dev = c.Lines.First(l => l.Layer == "device" && l.Text == "set gps_ublox_nav_hz = 10");
                _v.SetDisabled(dev, true);
                Check(t, _b.DisabledLines?.SequenceEqual(new[] { "set gps_ublox_nav_hz = 10" }) == true && JsonSerializer.Serialize(_b.ToSession(CoreLocator.DataRoot)).Contains("\"disabled_lines\":[\"set gps_ublox_nav_hz = 10\"]"), "disabledLines / session: " + string.Join(",", _b.DisabledLines ?? new()));
                StartCheck();
                _step = 2;
                break;
            }
            case 2:
            {
                if (!Wait(t, _task!.IsCompleted, "session-check 2")) break;
                var (c, err) = _task.Result;
                if (c == null) { Check(t, false, "session-check 2: " + err); _step = 90; break; }
                var dl = c.Lines.FirstOrDefault(l => l.Text == "set gps_ublox_nav_hz = 10");
                Check(t, dl != null && dl.Disabled && !c.Text.Contains("gps_ublox_nav_hz") && c.Summary.Contains("1 disabled"), $"the disabled line in the next check: {(dl == null ? "missing" : dl.Disabled ? "disabled" : "ON")}; summary {c.Summary}");
                _v.Show(c, "");
                // edit an aircraft line: a preset's aircraft → copied next to the bench
                var air = c.Lines.First(l => l.Layer == "aircraft" && l.Text == "set nav_fw_bank_angle = 35");
                _v.OpenEditor(air);
                Check(t, _v.EditorOpen && _v.EditorHead.Contains("bundled preset's file") && _v.EditorCaretLine == air.FileLine - 1, $"editor: open {_v.EditorOpen}, head '{_v.EditorHead}', caret {_v.EditorCaretLine} (line {air.FileLine})");
                _v.EditorText = _v.EditorText.Replace("set nav_fw_bank_angle = 35", "set nav_fw_bank_angle = 33");
                var written = _v.SaveEditor() ?? "";
                var copyJson = BenchFiles.Abs("layers_bench.files/models/trainer/aircraft.json");
                string? fwCli = null;
                try { using var d = JsonDocument.Parse(File.ReadAllText(copyJson)); fwCli = Json.Str(d.RootElement, "firmware_cli"); } catch (Exception) { }
                Check(t, _b.AircraftFile == "./layers_bench.files/models/trainer/aircraft.json" && written.EndsWith("layers_bench.files/models/trainer/inav.cli") && File.ReadAllText(written).Contains("set nav_fw_bank_angle = 33")
                         && fwCli != null && BenchFiles.Same(fwCli, written) && File.Exists(BenchFiles.Abs("layers_bench.files/models/trainer/env/launch.json")) && !Directory.Exists(BenchFiles.Abs("layers_bench.files/models/trainer/visual")),
                      $"aircraft copy-on-write: aircraftFile {_b.AircraftFile}, written {written}, copy's firmware_cli {fwCli}");
                Check(t, RepoHashes().SequenceEqual(_repoHashes), "the repo's inav.cli / aircraft.json / defaults.cli changed");
                Check(t, BenchFiles.Same((string)_b.ToSession(CoreLocator.DataRoot)["aircraft"], copyJson), "session aircraft: " + _b.ToSession(CoreLocator.DataRoot)["aircraft"]);
                StartCheck();
                _step = 3;
                break;
            }
            case 3:
            {
                if (!Wait(t, _task!.IsCompleted, "session-check 3")) break;
                var (c, err) = _task.Result;
                if (c == null) { Check(t, false, "session-check 3: " + err); _step = 90; break; }
                var ed = c.Lines.FirstOrDefault(l => l.Text == "set nav_fw_bank_angle = 33");
                Check(t, ed != null && ed.Layer == "aircraft" && ed.Source.Contains("layers_bench.files/models/trainer/inav.cli:") && !c.Lines.Any(l => l.Text == "set nav_fw_bank_angle = 35"), $"the check reads the copy: {ed?.Source ?? "no edited line"}");
                _v.Show(c, "");
                // the board group's file (defaults.cli: comments only): the bench's copy
                var board = new ConfigLine { Layer = "board", Source = "boards/speedybee-f405-wing/defaults.cli", Text = "" };
                _v.OpenEditor(board);
                _v.EditorText = _v.EditorText.TrimEnd() + "\nset looptime = 500\n";
                var bw = _v.SaveEditor() ?? "";
                Check(t, _b.BoardConfigFile == "./layers_bench.files/boards/speedybee-f405-wing/defaults.cli" && File.Exists(bw) && BenchFiles.Owned(bw), $"board copy-on-write: boardConfigFile {_b.BoardConfigFile}, written {bw}");
                StartCheck();
                _step = 4;
                break;
            }
            case 4:
            {
                if (!Wait(t, _task!.IsCompleted, "session-check 4")) break;
                var (c, err) = _task.Result;
                if (c == null) { Check(t, false, "session-check 4: " + err); _step = 90; break; }
                var cf = c.Conflicts.FirstOrDefault(x => x.Setting == "set looptime");
                Check(t, c.Count("board") == 1 && cf != null && cf.Winner == "aircraft" && cf.Lines[0].Layer == "board", $"board line from the copy, conflict with the aircraft's looptime (later layer wins): {cf?.Describe() ?? "none"}");
                // rename = a new bench with copies of the files
                Wizard.Commit(_b, "layers bench 2");
                var newCli = BenchFiles.Abs("layers_bench_2.cli");
                var newJson = BenchFiles.Abs("layers_bench_2.files/models/trainer/aircraft.json");
                string? fw2 = null;
                try { using var d = JsonDocument.Parse(File.ReadAllText(newJson)); fw2 = Json.Str(d.RootElement, "firmware_cli"); } catch (Exception) { }
                Check(t, _b.BenchCli == "./layers_bench_2.cli" && File.Exists(newCli) && File.Exists(BenchFiles.Abs("layers_bench.cli")) && _b.AircraftFile == "./layers_bench_2.files/models/trainer/aircraft.json"
                         && _b.BoardConfigFile.StartsWith("./layers_bench_2.files/") && fw2 != null && fw2.Replace('\\', '/').Contains("layers_bench_2.files/models/trainer/"),
                      $"rename: benchCli {_b.BenchCli}, aircraftFile {_b.AircraftFile}, boardConfigFile {_b.BoardConfigFile}, copy's firmware_cli {fw2}");
                // export, then import into another benches folder
                _exportDir = ProjectSettings.GlobalizePath($"user://selftest-export-{Time.GetTicksUsec()}");
                var files = BenchFiles.ExportPreset(_b, c, "# diff all\nset x = 1\n", _exportDir, CoreLocator.DataRoot);
                var names = files.Select(f => f.Replace('\\', '/')[(_exportDir.Replace('\\', '/').Length + 1)..]).ToList();
                var readme = File.ReadAllText(Path.Combine(_exportDir, "layers_bench_2.README.md"));
                Check(t, names.Contains("layers_bench_2.json") && names.Contains("layers_bench_2.cli") && names.Contains("layers_bench_2.README.md") && names.Contains("layers_bench_2.files/models/trainer/inav.cli")
                         && names.Contains("layers_bench_2.files/boards/speedybee-f405-wing/defaults.cli") && !names.Any(n => n.Contains("visual/")),
                      "export files: " + string.Join(", ", names));
                Check(t, readme.Contains("## Config layers") && readme.Contains("### 2 · aircraft") && readme.Contains("### 5 · bench") && readme.Contains("Conflicts (later layer wins)") && readme.Contains("set x = 1"), "README: " + readme[..Math.Min(300, readme.Length)]);
                var exported = BenchData.Parse(File.ReadAllText(Path.Combine(_exportDir, "layers_bench_2.json")));
                Check(t, exported.BenchCli == "./layers_bench_2.cli" && exported.AircraftFile == "./layers_bench_2.files/models/trainer/aircraft.json" && exported.Preset == null && exported.LastUsed == DateTime.MinValue,
                      $"exported bench: benchCli {exported.BenchCli}, aircraftFile {exported.AircraftFile}");
                // import (Presets.Open) into a second benches folder
                BenchData.Dir = _dir2 = $"user://selftest-import-{Time.GetTicksUsec()}";
                var opened = Presets.Open(new Preset(Path.Combine(_exportDir, "layers_bench_2.json"), exported), out var existed, out var note);
                string? fw3 = null;
                var impJson = BenchFiles.Abs("layers_bench_2.files/models/trainer/aircraft.json");
                try { using var d = JsonDocument.Parse(File.ReadAllText(impJson)); fw3 = Json.Str(d.RootElement, "firmware_cli"); } catch (Exception) { }
                Check(t, opened != null && !existed && File.Exists(BenchFiles.Abs("layers_bench_2.cli")) && fw3 != null && File.Exists(fw3) && BenchFiles.Owned(fw3) && File.ReadAllText(fw3).Contains("set nav_fw_bank_angle = 33"),
                      $"import: {note}; firmware_cli {fw3}");
                BenchData.Dir = _dir;
                // delete takes the bench's own files
                _b.Save();
                var own = BenchData.FilesOf(_b.Name);
                bool ok = BenchData.Delete(_b.Name, out var gone, out var derr);
                Check(t, ok && own.Any(f => f.EndsWith(".cli")) && own.Any(f => f.Contains("layers_bench_2.files/")) && !File.Exists(newCli) && !Directory.Exists(BenchFiles.Abs("layers_bench_2.files")) && File.Exists(BenchFiles.Abs("layers_bench.cli")),
                      $"delete: {gone.Count} files ({derr}); the first bench's files stay");
                LayersTests.Info[t].Add($"fake core session-check ×{_checks}: bench lines → ./layers_bench.cli → layer 5 last, wins set small_angle; a device line disabled → skipped; inav.cli edited → the aircraft copied next to the bench (firmware_cli → the copy), the repo untouched, the check reads the copy; defaults.cli → the bench's copy; rename copies; export ({files.Count} files) → import re-points firmware_cli; delete takes the bench's files");
                _step = 90;
                break;
            }
            case 90:
                Cleanup();
                LayersTests.Report(t);
                _step = 100;
                break;
        }
    }

    private void Snapshot()
    {
        const string t = "snapshot_export";
        switch (_step)
        {
            case 100:
            {
                _port = Enumerable.Range(31268, 20).FirstOrDefault(p => CoreLauncher.CanBind(p));
                var psi = new ProcessStartInfo("python") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
                psi.ArgumentList.Add(LayersTests.FakeCore); psi.ArgumentList.Add("--port"); psi.ArgumentList.Add(_port.ToString()); psi.ArgumentList.Add("--disarmed");
                _proc = Process.Start(psi);
                _proc!.OutputDataReceived += (_, _) => { }; _proc.ErrorDataReceived += (_, _) => { };
                _proc.BeginOutputReadLine(); _proc.BeginErrorReadLine();
                _snapPath = Path.Combine(ProjectSettings.GlobalizePath($"user://selftest-snap-{Time.GetTicksUsec()}"), "snap bench.cli").Replace('\\', '/');
                _deadline = Time.GetTicksMsec() + 10000;
                _step = 101;
                break;
            }
            case 101:
                if (!Wait(t, !CoreLauncher.CanBind(_port), $"the fake core on {_port}")) break;
                _cc = new CoreClient();
                _cc.ConfigReceived += m => _got.Add(m.Clone());
                _cc.EventReceived += (name, d, _) => _events.Add(name + (d.ValueKind == JsonValueKind.Object ? " " + d.GetRawText() : ""));
                _cc.Connect("127.0.0.1", _port);
                _deadline = Time.GetTicksMsec() + 10000;
                _step = 102;
                break;
            case 102:
                _cc!.Poll();
                if (!Wait(t, _cc.Connected, "connected to the spawned fake core")) break;
                _cc.Send(new { type = "config", snapshot = "bench", path = _snapPath });
                _deadline = Time.GetTicksMsec() + 10000;
                _step = 103;
                break;
            case 103:
            {
                _cc!.Poll();
                var w = _got.FirstOrDefault(m => Json.Str(m, "snapshot") == "written");
                if (!Wait(t, w.ValueKind == JsonValueKind.Object, "config {snapshot:\"written\"}")) break;
                var lines = File.Exists(_snapPath) ? File.ReadAllLines(_snapPath).Where(l => LayeredConfig.Clean(l) != null).ToList() : new List<string>();
                Check(t, Json.Str(w, "path") == _snapPath && (int)Json.Num(w, "lines", -1) == lines.Count && lines.Count > 5 && lines.Contains("set tail_center_trim = 137") && !File.Exists(_snapPath + ".tmp"),
                      $"snapshot: answer path {Json.Str(w, "path")} lines {Json.Num(w, "lines", -1)}; file {(File.Exists(_snapPath) ? lines.Count + " lines" : "missing")}");
                _cc.Send(new { type = "config", export = true });
                _cc.Send(new { type = "config", apply = true, full = true });
                _deadline = Time.GetTicksMsec() + 10000;
                _step = 104;
                break;
            }
            case 104:
            {
                _cc!.Poll();
                var ex = _got.FirstOrDefault(m => m.TryGetProperty("export", out var e) && e.ValueKind == JsonValueKind.Object);
                if (!Wait(t, ex.ValueKind == JsonValueKind.Object && _events.Any(e => e.StartsWith("config {\"state\": \"applied\"") || e.StartsWith("config {\"state\":\"applied\"")), "export answer + apply events")) break;
                var exp = ex.GetProperty("export");
                var lc = new LayeredConfig(); lc.ApplyJson(exp.GetProperty("assembly"));
                Check(t, lc.Layered && lc.Count("aircraft") > 0 && Json.Str(exp.GetProperty("diff"), "text")?.Contains("# diff all") == true, $"export: {lc.Lines.Count} lines, layered {lc.Layered}");
                Check(t, _events.Any(e => e.StartsWith("config") && e.Contains("applying") && e.Contains("\"full\": true")) && _events.Any(e => e.StartsWith("reboot")), "apply events: " + string.Join(" | ", _events));
                LayersTests.Info[t].Add($"spawned fake core (--disarmed, port {_port}): snapshot written atomically ({File.ReadAllLines(_snapPath).Length} lines), export by layer, apply full → applying / applied / reboot");
                _step = 190;
                break;
            }
            case 190:
                Cleanup();
                _step = 200;
                break;
        }
    }

    private void Cleanup()
    {
        if (_v != null && GodotObject.IsInstanceValid(_v)) { _v.Free(); _v = null!; }
        LayeredConfig.Checker = null;
        if (_keepDir != "") { BenchData.Dir = _keepDir; _keepDir = ""; }
        foreach (var d in new[] { _dir, _dir2 }) if (d != "") try { Directory.Delete(ProjectSettings.GlobalizePath(d), true); } catch (Exception) { }
        if (_exportDir != "") try { Directory.Delete(_exportDir, true); } catch (Exception) { }
        _dir = _dir2 = _exportDir = "";
        _cc?.Dispose(); _cc = null;
        if (_proc != null)
        {
            var pid = _proc.Id;
            try { _proc.Kill(); _proc.WaitForExit(5000); } catch (Exception) { }
            Check("snapshot_export", _proc.HasExited, $"the spawned fake core (pid {pid}) did not stop");
            _proc.Dispose(); _proc = null;
        }
        if (_snapPath != "") try { Directory.Delete(Path.GetDirectoryName(_snapPath)!, true); } catch (Exception) { }
        _snapPath = "";
    }

    private void Finish() { Cleanup(); Done = true; }
}
