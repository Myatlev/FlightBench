// Self-tests of the bench as a whole (FB_SELFTEST=all, or bench_delete,wizard_script_view,cli_indicator,edit_in_place):
//   bench_delete        a temp benches folder: a preset opened (copied), its session + bench-state files written next to
//                       another bench's; BenchData.Delete takes exactly that bench's files, the other bench and the bundled
//                       presets stay byte-identical; the confirmation names the files; on the flight screen Bench → Delete
//                       bench stops the core and deletes the files without writing the bench again.
//   wizard_script_view  LayeredConfig parses a CLI board's (`config_cli|`) and a blob board's (`defaults:`) report; the
//                       wizard over the flight screen shows the trainer preset's script from the core's session-check
//                       (FB_CORE_EXE; else "no core to ask") by layer (from the core, else inferred: inav.cli, the GPS lines).
//   cli_indicator       FcStatus: in the CLI → "firmware: in the CLI — MSP blocked" (not during the core's own first start);
//                       on the screen (fake core: a cli line opens the CLI with a `cli` event and fc.cli) the Status line,
//                       its "exit CLI (reboots)" button, the Log window's label and the configurator tooltip; the button
//                       sends {"type":"cli","exit":true} and the indicator goes when the core says the CLI closed.
//   edit_in_place       the launch passes the same bench-state file before and after an edit (StartArgs: `bench-state=` in -M,
//                       commas doubled; left out for a core without the option — checked on FB_CORE_EXE); on the screen
//                       Bench → Edit bench opens the wizard over the connected screen, Save & start writes the bench and
//                       applies it in place (reconnects), the delta events show in Status ("config: applying 4 new lines
//                       (delta)…", "restored (bench state)", removed lines logged), Reset firmware config sends the reset.
// The screen parts run on the modes screen (ModesTests.cs: an RS7 bench in memory, the plain fake core) after its own tests.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public static class BenchTests
{
    public static readonly string[] Names = { "bench_delete", "wizard_script_view", "cli_indicator", "edit_in_place" };
    /// The ones with a flight-screen part (the modes screen runs them after its own tests).
    public static readonly string[] ScreenNames = { "bench_delete", "cli_indicator", "edit_in_place" };
    public static bool Any => Names.Any(DevEnv.SelfTest);
    public static bool ScreenAny => ScreenNames.Any(DevEnv.SelfTest);

    private static void Check(List<string> f, bool ok, string what) { if (!ok) f.Add(what); }

    public static Dictionary<string, string> Hashes(string dir) => Directory.Exists(dir)
        ? Directory.GetFiles(dir, "*", SearchOption.AllDirectories).ToDictionary(x => x, x => Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(x))))
        : new();

    /// bench_delete, the model: a temp benches folder, a preset opened, its files, another bench; Delete; the dialog text.
    public static List<string> DeleteModel(List<string> info)
    {
        var f = new List<string>();
        var keep = BenchData.Dir;
        BenchData.Dir = $"user://selftest-delete-{Time.GetTicksUsec()}";
        var abs = ProjectSettings.GlobalizePath(BenchData.Dir);
        var bundle = Hashes(Presets.Dir);
        try
        {
            Directory.CreateDirectory(abs);
            var preset = Presets.List().FirstOrDefault(p => p.Id == "inav-trainer-launch") ?? Presets.List().FirstOrDefault();
            if (preset == null) { f.Add($"no preset in {Presets.Dir}"); return f; }
            var b = Presets.Open(preset, out _, out var note);
            Check(f, b != null && File.Exists(ProjectSettings.GlobalizePath(BenchData.PathFor(preset.Name))), $"preset '{preset.Name}' not copied ({note})");
            if (b == null) return f;
            CoreLauncher.WriteSession(b, CoreLocator.DataRoot, out var err);
            Check(f, err == "", "session not written: " + err);
            Check(f, BenchState.Dir.StartsWith(abs.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase), $"bench state dir {BenchState.Dir} not under the temp benches folder (FB_BENCH_DIR-like runs must leave the cfg-cache alone)");
            Directory.CreateDirectory(BenchState.Dir);
            File.WriteAllText(BenchState.PathFor(b.Name), "state");
            File.WriteAllText(Path.ChangeExtension(BenchState.PathFor(b.Name), ".json"), "{}");
            // another bench, with its own files: must stay
            var other = b.CopyAs(b.Name + " other");
            other.Save();
            CoreLauncher.WriteSession(other, CoreLocator.DataRoot, out _);
            File.WriteAllText(BenchState.PathFor(other.Name), "other state");
            var files = BenchData.FilesOf(b.Name);
            Check(f, files.Count == 4, $"FilesOf: {files.Count} files (want bench + session + state.bin + state.json): {string.Join(", ", files)}");
            var dlg = MainMenu.DeleteDialog(b);
            Check(f, dlg.DialogText.Contains(ProjectSettings.GlobalizePath(b.FilePath).Replace('\\', '/')) && files.All(x => dlg.DialogText.Contains(x)), "the confirmation does not name every file: " + dlg.DialogText);
            dlg.Free();
            Check(f, BenchData.IsBundled(preset.File) && !BenchData.IsBundled(files[0]), "IsBundled: the preset file is bundled, the bench's is not");
            var otherBefore = Hashes(abs).Where(kv => kv.Key.Contains("other", StringComparison.Ordinal)).ToDictionary(kv => kv.Key, kv => kv.Value);
            bool ok = BenchData.Delete(b.Name, out var gone, out var derr);
            Check(f, ok && gone.Count == 4 && files.All(x => !File.Exists(x)), $"Delete: ok {ok}, {gone.Count} gone, left {string.Join(", ", files.Where(File.Exists))} ({derr})");
            var otherAfter = Hashes(abs).Where(kv => kv.Key.Contains("other", StringComparison.Ordinal)).ToDictionary(kv => kv.Key, kv => kv.Value);
            Check(f, otherBefore.Count == 3 && otherBefore.All(kv => otherAfter.TryGetValue(kv.Key, out var h) && h == kv.Value), $"the other bench's files changed: {otherBefore.Count} → {otherAfter.Count}");
            Check(f, BenchData.ListAll().Select(x => x.Name).SequenceEqual(new[] { other.Name }), "benches left: " + string.Join(", ", BenchData.ListAll().Select(x => x.Name)));
            // the preset opens fresh again (a new copy)
            var again = Presets.Open(preset, out var existed, out _);
            Check(f, again != null && !existed, "after the delete the preset opens as a fresh copy");
            info.Add($"'{b.Name}': {gone.Count} files deleted, the other bench's 3 kept, preset reopened fresh");
        }
        catch (Exception e) { f.Add("threw " + e.GetType().Name + ": " + e.Message); }
        finally
        {
            BenchData.Dir = keep;
            try { Directory.Delete(abs, true); } catch (Exception) { }
        }
        var after = Hashes(Presets.Dir);
        Check(f, bundle.Count > 0 && bundle.Count == after.Count && bundle.All(kv => after.TryGetValue(kv.Key, out var h) && h == kv.Value), $"the bundled presets changed: {bundle.Count} → {after.Count} files");
        return f;
    }

    // a CLI board's report (the trainer, trimmed) and a blob board's (the RS7 with the OSD, trimmed)
    public const string CliReport = "crsf.uart        -> uart1              modelled\nublox.uart       -> uart3              modelled\n"
        + "machine: imu=on,crsf=uart1\nconfig: # ublox on uart3 (device config)\nconfig: feature GPS\nconfig: serial 2 +1\n"
        + "launch: icount_shift=2 realtime=off control=31250 msp=31235 machine=speedybee-f405-wing\nmanifest: {\"type\":\"manifest\"}\n"
        + "config_cli: 4 lines\nconfig_cli| set platform_type = AIRPLANE\nconfig_cli| aux 0 0 0 1700 2100\nconfig_cli| feature GPS\nconfig_cli| serial 2 +1\n"
        + "defaults: none\n";
    public const string BlobReport = "msp_osd.uart     -> uart4   (PORT-A)   modelled\nconfig: # msp_osd on uart4 (device config)\n"
        + "config: serial 3 1 115200 57600 0 115200\nconfig: set displayport_msp_serial = 3\n"
        + "launch: icount_shift=2 realtime=off control=31250 msp=31235 machine=nexus-xr\nmanifest: {\"type\":\"manifest\"}\n"
        + "config_cli: none\ndefaults: 63 bytes (max 6136)\n## Rotorflight Custom Defaults\nboard_name NEXUS_XR\nserial 3 1 115200 57600 0 115200\n\n";

    public static List<string> ScriptModel()
    {
        var f = new List<string>();
        var c = LayeredConfig.Parse(CliReport);
        Check(f, c.Kind == "cli" && c.Lines.Select(l => l.Text).SequenceEqual(new[] { "set platform_type = AIRPLANE", "aux 0 0 0 1700 2100", "feature GPS", "serial 2 +1" }), "CLI report: " + c.Kind + " / " + string.Join(" | ", c.Lines.Select(l => l.Text)));
        Check(f, c.DeviceLines.Count == 3 && !c.Layered && c.Summary.StartsWith("4 CLI lines typed into the firmware at its first start"), "CLI summary: " + c.Summary);
        var b = LayeredConfig.Parse(BlobReport.Replace("\n", "\r\n"));
        Check(f, b.Kind == "defaults" && b.DefaultsBytes == 63 && b.Lines.Select(l => l.Text).SequenceEqual(new[] { "## Rotorflight Custom Defaults", "board_name NEXUS_XR", "serial 3 1 115200 57600 0 115200" }), "blob report (CRLF): " + b.Kind + " / " + string.Join(" | ", b.Lines.Select(l => l.Text)));
        Check(f, b.Summary.StartsWith("custom defaults placed with the firmware (63 bytes, 3 lines)") && b.DeviceLines.Count == 3, "blob summary: " + b.Summary);
        var n = LayeredConfig.Parse("launch: control=1\nconfig_cli: none\ndefaults: none\n");
        Check(f, n.Kind == "none" && n.Lines.Count == 0 && n.Text == "", "a report with neither: " + n.Kind);
        return f;
    }

    public static List<string> CliModel()
    {
        var f = new List<string>();
        var fc = new FcStatus();
        var cfg = new ConfigProgress();
        using (var d = JsonDocument.Parse("{\"type\":\"state\",\"fc\":{\"msp\":false,\"cli\":true,\"armed\":false}}")) fc.ApplyState(d.RootElement, null);
        Check(f, FcStatus.FirmwareText(fc.Firmware(true, cfg)) == "firmware: " + Flight.CliText, "in the CLI: " + FcStatus.FirmwareText(fc.Firmware(true, cfg)));
        using (var d = JsonDocument.Parse("{\"state\":\"applying\",\"lines\":13}")) cfg.OnConfig(d.RootElement, 8.0);
        Check(f, FcStatus.FirmwareText(fc.Firmware(true, cfg)).StartsWith("firmware: busy with its first start"), "the core's own first start in the CLI: " + FcStatus.FirmwareText(fc.Firmware(true, cfg)));
        Check(f, FcStatus.FirmwareText(fc.Firmware(false, new ConfigProgress())) == "", "not connected: nothing");
        return f;
    }

    /// edit_in_place, the model: the same bench-state file for the same bench; StartArgs; an older core leaves it out.
    public static List<string> EditModel(List<string> info)
    {
        var f = new List<string>();
        var p = BenchState.PathFor("my bench");
        Check(f, p.EndsWith("/my_bench.state.bin", StringComparison.Ordinal) && BenchState.PathFor("my bench") == p && BenchState.PathFor("other") != p, "bench-state path per bench name: " + p);
        var a = CoreLauncher.StartArgs("C:/b/s.json", "C:/root", "127.0.0.1", CorePorts.From(31250, 31235), 2, false, "speedybee-f405-wing", "C:/cache,x/my_bench.state.bin");
        Check(f, a[1] == "speedybee-f405-wing,session=C:/b/s.json,root=C:/root,ctl=127.0.0.1:31250,bench-state=C:/cache,,x/my_bench.state.bin", "-M with bench-state (comma doubled): " + a[1]);
        var n = CoreLauncher.StartArgs("C:/b/s.json", "C:/root", "127.0.0.1", CorePorts.From(31250, 31235), 2, false);
        Check(f, !n[1].Contains("bench-state"), "no bench state → no option: " + n[1]);
        if (DevEnv.CoreExe is { } exe && File.Exists(exe))
        {
            bool bs = CoreLauncher.HasOption(exe, "speedybee-f405-wing", "bench-state"), cc = CoreLauncher.HasOption(exe, "speedybee-f405-wing", "cfg-cache");
            Check(f, cc, "HasOption does not find cfg-cache in the core's -M help");
            info.Add($"FB_CORE_EXE: cfg-cache {(cc ? "listed" : "missing")}, bench-state {(bs ? "listed: passed" : "not listed: left out (an older core)")}");
        }
        else info.Add("no FB_CORE_EXE: the option probe not run");
        return f;
    }
}

/// The wizard_script_view screen part: the wizard over the root (as over the flight screen) on the trainer preset, summary step.
public partial class SelfTestRunner
{
    private Window? _wsWin;
    private Wizard? _wsWiz;
    private ulong _wsDeadline;

    /// True when done (reported).
    private bool WizardScriptTest()
    {
        const string name = "wizard_script_view";
        if (_wsWiz == null)
        {
            var preset = Presets.List().FirstOrDefault(x => x.Id == "inav-trainer-launch");
            if (preset == null) { SelfTests.Report(name, false, "no preset inav-trainer-launch"); return true; }
            _wsWiz = Wizard.Over(preset.Bench);
            _wsWin = new Window { Title = "wizard_script_view", Size = new Vector2I(1200, 900), Visible = false };
            _wsWin.AddChild(_wsWiz);
            GetTree().Root.AddChild(_wsWin);
            _wsWin.Popup();
            _wsDeadline = Time.GetTicksMsec() + 45000;
            return false;
        }
        if (!_wsWiz.IsNodeReady()) return false;
        if (_wsWiz.Script == null && !_wsWiz.ScriptBusy && _wsWiz.ScriptInfo.StartsWith("(the core")) { _wsWiz.ShowStep(4); return false; }   // not asked yet
        if (_wsWiz.ScriptBusy && Time.GetTicksMsec() < _wsDeadline) return false;
        var f = BenchTests.ScriptModel();
        string detail;
        if (DevEnv.CoreExe is { } exe && File.Exists(exe))
        {
            var s = _wsWiz.Script;
            Check(f, s != null && s.Kind == "cli" && s.Lines.Count > 50, $"the trainer's script from the core: {s?.Kind} {s?.Lines.Count} lines; info '{_wsWiz.ScriptInfo}'");
            Check(f, _wsWiz.ScriptText.Contains("set platform_type = AIRPLANE") && _wsWiz.ScriptText.Contains("serial 2 +1") && _wsWiz.ScriptText.Contains("feature GPS"), "the script lacks the board / aircraft / GPS device lines");
            // layers: from the core, or inferred by the app for a core without them — the aircraft's inav.cli and the GPS's lines traced
            Check(f, s != null && (s.Layered || s.Inferred) && s.Count("aircraft") > 50 && s.Count("device") >= 2 && s.Lines.Any(l => l.Layer == "aircraft" && l.Source.StartsWith("models/trainer/inav.cli:")),
                  $"layers: {(s == null ? "none" : $"board {s.Count("board")}, aircraft {s.Count("aircraft")}, device {s.Count("device")}, bench {s.Count("bench")}, not traced {s.Lines.Count(l => l.Layer == "")}; {(s.Layered ? "from the core" : "inferred")}")}");
            var rows = _wsWiz.Layers.Rows();
            Check(f, rows.Count == s?.Lines.Count && rows.Any(r => r.Group.StartsWith("2 · aircraft") && r.Source.StartsWith("models/trainer/inav.cli:")) && rows.Any(r => r.Group.StartsWith("4 · device") && r.Text == "feature GPS"),
                  $"the tree: {rows.Count} rows, groups {string.Join(", ", rows.Select(r => r.Group).Distinct())}");
            detail = $"core session-check: {s?.Lines.Count} CLI lines, layers {(s?.Layered == true ? "from the core" : "inferred by the app")} (board {s?.Count("board")}, aircraft {s?.Count("aircraft")}, device {s?.Count("device")}, bench {s?.Count("bench")})";
        }
        else
        {
            Check(f, _wsWiz.ScriptInfo.StartsWith("not known: no core to ask"), "without a core: " + _wsWiz.ScriptInfo);
            detail = "no FB_CORE_EXE: 'no core to ask' shown";
        }
        Check(f, _wsWiz.Layers.BenchHeadText.StartsWith("5 · bench lines"), "the bench lines box: " + _wsWiz.Layers.BenchHeadText);
        Check(f, _wsWiz.Overlay, "the wizard is not in overlay mode");
        SelfTests.Report(name, f.Count == 0, f.Count == 0 ? $"CLI / blob reports parsed; wizard over the screen, summary: {detail}; layered tree, bench lines box, Copy" : string.Join("; ", f));
        _wsWin!.QueueFree(); _wsWin = null; _wsWiz = null;
        return true;
    }

    private static void Check(List<string> f, bool ok, string what) { if (!ok) f.Add(what); }
}

public partial class Flight
{
    private int _btStep;
    private ulong _btDeadline, _btFrame;
    private readonly List<string> _btSent = new();
    private readonly Dictionary<string, List<string>> _btFails = BenchTests.ScreenNames.ToDictionary(n => n, _ => new List<string>());
    private bool _btDone;
    private string? _btKeepDir, _btDir;
    private string _btStateBefore = "";
    private int _btDisc;

    /// On the modes screen after its own tests: cli_indicator, edit_in_place, bench_delete (in this order; delete last).
    private void BenchSelfTest()
    {
        void Check(string t, bool ok, string what) { if (!ok) _btFails[t].Add(what); }
        bool Waited(string t, bool cond, string what)
        {
            if (cond) return true;
            if (Time.GetTicksMsec() < _btDeadline) return false;
            _btFails[t].Add("timed out: " + what);
            return true;
        }
        void Next(int ms = 6000) { _btStep++; _btDeadline = Time.GetTicksMsec() + (ulong)ms; _btFrame = Engine.GetProcessFrames(); }
        switch (_btStep)
        {
            case 0:
                _core.Sending += line => { if (line.Contains("\"type\":\"cli\"") || line.Contains("\"type\":\"config\"")) _btSent.Add(line); };
                // cli_indicator: a CLI line opens the fake core's CLI (a `cli` event + fc.cli)
                Check("cli_indicator", _session.Connected && _fc.Cli != true, $"connected {_session.Connected}, fc.cli {_fc.Cli} before");
                _cli!.Sent!("status");
                Next();
                break;
            case 1:
                if (!Waited("cli_indicator", _fc.Cli == true && _cliOpened == 1, "the fake core's cli event / fc.cli")) break;
                UpdateStatus();
                Check("cli_indicator", _statusPanel!.FirmwareText == "firmware: " + CliText, "Status firmware line: " + _statusPanel.FirmwareText);
                Check("cli_indicator", _statusPanel.ExitCli.IsVisibleInTree() && _cliLogLabel!.IsVisibleInTree() && _cliExitBtn is { Disabled: false }, $"exit button in Status {_statusPanel.ExitCli.Visible}, Log label {_cliLogLabel?.Visible}");
                Check("cli_indicator", _cfgBtn!.TooltipText.Contains(CliText), "configurator tooltip: " + _cfgBtn.TooltipText);
                _statusPanel.ExitCli.EmitSignal(BaseButton.SignalName.Pressed);
                Next();
                break;
            case 2:
                if (!Waited("cli_indicator", _fc.Cli == false && _cliClosed == 1, "the CLI closed after exit")) break;
                UpdateStatus();
                Check("cli_indicator", _btSent.Contains("{\"type\":\"cli\",\"exit\":true}"), "exit sent: " + string.Join(" | ", _btSent));
                Check("cli_indicator", !_statusPanel!.ExitCli.Visible && !_cliLogLabel!.Visible && !_statusPanel.FirmwareText.Contains("CLI") && !_cfgBtn!.TooltipText.Contains(CliText), $"after exit: '{_statusPanel.FirmwareText}', button {_statusPanel.ExitCli.Visible}, label {_cliLogLabel?.Visible}");
                // edit_in_place: the bench written in a temp benches folder, then Bench → Edit bench over the connected screen
                _btKeepDir = BenchData.Dir;
                BenchData.Dir = _btDir = $"user://selftest-edit-{Time.GetTicksUsec()}";
                _bench.Save();
                _btStateBefore = LaunchFor("s.json", CoreLocator.DataRoot, null, external: false).BenchState ?? "";
                _btDisc = _disconnects;
                EditBench();
                Check("edit_in_place", WizardOpen && GetTree().CurrentScene == this && _session.Connected, $"wizard over the screen: open {WizardOpen}, still the flight screen {GetTree().CurrentScene == this}, connected {_session.Connected}");
                Next();
                break;
            case 3:
            {
                if (!Waited("edit_in_place", _wizard is { } w0 && w0.IsNodeReady(), "the wizard over the screen")) break;
                var wiz = _wizard!;
                Check("edit_in_place", wiz.Overlay && wiz.EditedBench.Name == _bench.Name && wiz.EditedBench != _bench, "the wizard edits a copy of this bench");
                Check("edit_in_place", _rc.KeysOff || Engine.GetProcessFrames() == _btFrame, "keyboard sticks still on while the wizard is open");
                wiz.EditedBench.BoardConfig = (wiz.EditedBench.BoardConfig.TrimEnd() + "\nset fbst_edit = 1").TrimStart();
                var before = _bench;
                wiz.SaveForTest();
                Check("edit_in_place", !WizardOpen && _bench != before && _bench.BoardConfig.Contains("fbst_edit") && GetTree().CurrentScene == this, $"after Save & start: wizard open {WizardOpen}, bench replaced {_bench != before}");
                var onDisk = BenchData.Load(BenchData.PathFor(_bench.Name));
                Check("edit_in_place", onDisk != null && onDisk.BoardConfig.Contains("fbst_edit"), "the edited bench is not on disk");
                var after = LaunchFor("s.json", CoreLocator.DataRoot, null, external: false).BenchState ?? "";
                Check("edit_in_place", after != "" && after == _btStateBefore && after == BenchState.PathFor(_bench.Name), $"bench-state before '{_btStateBefore}', after '{after}'");
                Check("edit_in_place", LogHas("restarting with the same bench state"), "the log does not say the restart keeps the bench state");
                Next(8000);
                break;
            }
            case 4:
                if (!Waited("edit_in_place", _session.Connected && _disconnects > _btDisc, "reconnected after Save & start")) break;
                // the bench-state core's events on the restart (injected: the fake core cannot know it restarted)
                _core.InjectForTest(PlaneTests.Event("config", 0.0, "{\"state\":\"restored\",\"source\":\"bench\",\"lines\":13,\"replay\":true}"));
                Check("edit_in_place", _statusPanel!.Config.Visible && _statusPanel.Config.Text == "config: restored (bench state)", "restored: " + _statusPanel.Config.Text);
                _core.InjectForTest(PlaneTests.Event("config", 7.9, "{\"state\":\"applying\",\"delta\":true,\"lines\":4}"));
                Check("edit_in_place", _statusPanel.Config.Visible && _statusPanel.Config.Text == "config: applying 4 new lines (delta)…", "delta: " + _statusPanel.Config.Text);
                Check("edit_in_place", LogHas("applying 4 new lines of the bench's script (delta)"), "the delta is not in the log");
                _core.InjectForTest(PlaneTests.Event("config", 8.0, "{\"removed\":[\"set old_line = 1\"]}"));
                Check("edit_in_place", LogHas("1 line no longer in the bench's script, left in the firmware (not reverted): set old_line = 1") && _statusPanel.Config.Text.Contains("delta"), "removed: " + _statusPanel.Config.Text);
                _core.InjectForTest(PlaneTests.Event("config", 9.1, "{\"state\":\"applied\",\"lines\":4,\"delta\":true}"));
                Check("edit_in_place", _statusPanel.Config.Text == "config: applied", "applied: " + _statusPanel.Config.Text);
                // Reset firmware config to first start: the command (the fake core answers with a full first start)
                RebuildBenchMenu();
                int ri = _benchPopup!.GetItemIndex(MenuResetConfig);
                Check("edit_in_place", ri >= 0 && !_benchPopup.IsItemDisabled(ri) && _benchPopup.GetItemIndex(MenuDelete) > ri, "Bench menu: Reset firmware config (enabled while connected) before Delete bench");
                Check("edit_in_place", ResetConfig(), "reset not sent");
                Next();
                break;
            case 5:
                if (!Waited("edit_in_place", _plane.Config.State == "applying" && !_plane.Config.Delta, "the first start after the reset")) break;
                Check("edit_in_place", _btSent.Contains("{\"type\":\"config\",\"reset\":\"first_start\"}"), "reset sent: " + string.Join(" | ", _btSent));
                Check("edit_in_place", _statusPanel!.Config.Text.StartsWith("config: applying the CLI script (13 lines)"), "after the reset: " + _statusPanel.Config.Text);
                // bench_delete (screen): the session / state files next to the bench, then Bench → Delete bench
                CoreLauncher.WriteSession(_bench, CoreLocator.DataRoot, out _);
                Directory.CreateDirectory(BenchState.Dir);
                File.WriteAllText(BenchState.PathFor(_bench.Name), "state");
                var keepOther = new BenchData { Name = "selftest other bench" };
                keepOther.Save();
                _btFiles = BenchData.FilesOf(_bench.Name);
                Check("bench_delete", _btFiles.Count == 3, "files before the delete: " + string.Join(", ", _btFiles));
                DeleteTest = true;
                DeleteBench();
                Next();
                break;
            case 6:
                if (!Waited("bench_delete", _deleteFiles != null, "the delete after the core stop")) break;
                Check("bench_delete", _btFiles!.All(x => !File.Exists(x)) && _deleteFiles!.Count == 3, "left after the delete: " + string.Join(", ", _btFiles!.Where(File.Exists)));
                Check("bench_delete", !_session.Connected || _session.State != CoreClient.ConnState.Connected, "still connected after the delete");
                SaveLayout(); CommitEdits();
                Check("bench_delete", !File.Exists(ProjectSettings.GlobalizePath(BenchData.PathFor(_bench.Name))), "the deleted bench was written again");
                Check("bench_delete", BenchData.ListAll().Any(b => b.Name == "selftest other bench"), "the other bench went too");
                Finish();
                break;
        }
    }
    private List<string>? _btFiles;

    private void Finish()
    {
        var info = new List<string>();
        var r = new Dictionary<string, List<string>>
        {
            ["cli_indicator"] = BenchTests.CliModel().Concat(_btFails["cli_indicator"]).ToList(),
            ["edit_in_place"] = BenchTests.EditModel(info).Concat(_btFails["edit_in_place"]).ToList(),
            ["bench_delete"] = BenchTests.DeleteModel(info).Concat(_btFails["bench_delete"]).ToList(),
        };
        var detail = new Dictionary<string, string>
        {
            ["cli_indicator"] = "FcStatus: in the CLI → 'firmware: in the CLI — MSP blocked', the core's own first start wins; screen (fake core): a CLI line → cli event + fc.cli → Status line + exit button, Log label, configurator tooltip; the button sent {\"type\":\"cli\",\"exit\":true} → cli closed → all hidden",
            ["edit_in_place"] = "bench-state path per bench, -M bench-state= (commas doubled), none without; " + string.Join("; ", info.Where(i => i.StartsWith("FB_CORE_EXE") || i.StartsWith("no FB_CORE_EXE"))) + "; screen: wizard over the connected screen, Save & start → written, applied in place, same bench-state file, reconnected; restored (bench state), applying 4 new lines (delta), removed logged, applied; Reset firmware config sent → first start",
            ["bench_delete"] = string.Join("; ", info.Where(i => i.StartsWith("'"))) + "; confirmation names every file; bundled presets unchanged; screen: Bench → Delete bench stopped / disconnected, bench + session + state gone, not written again, the other bench kept",
        };
        foreach (var n in BenchTests.ScreenNames.Where(DevEnv.SelfTest))
            SelfTests.Report(n, r[n].Count == 0, r[n].Count == 0 ? detail[n] : string.Join("; ", r[n]));
        if (_btKeepDir != null) BenchData.Dir = _btKeepDir;
        if (_btDir != null) try { Directory.Delete(ProjectSettings.GlobalizePath(_btDir), true); } catch (Exception) { }
        DeleteTest = false;
        _btDone = true;
        ModesTests.Done = true;
    }
}
