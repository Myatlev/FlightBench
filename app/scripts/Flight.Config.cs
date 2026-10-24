// Flight screen, the firmware's config as a developer tool (product owner 2026-10-02; core branch `config-layers`,
// docs/protocol.md `config` / `diff`; ConfigWindow.cs is the window):
// - the script by layer: at the launch the session-check's report (the script this core started with); after an edit
//   (a file, the bench lines, a disabled line) a session-check of the bench as it is now ("not sent yet": Apply script
//   now, or the next start, sends it); a bench the app does not start (remote / FB_CORE): a local session-check when
//   there is a core executable, and the core's `{"type":"config","export":true}` when connected;
// - Changes: `{"type":"diff","against":"script"}` → `{sent, current, changed:[{setting,script,firmware}], only_firmware,
//   only_script}`;
// - operations: Apply script now (`{"type":"config","apply":true[,"full":true]}`; the session file is written again first,
//   so a core that re-reads the bench gets the edits), Re-run <function> (`function`), Reset to first start (the existing
//   `config reset`), Snapshot firmware config → bench lines (`{"type":"config","snapshot":"bench","path":<bench_cli>}` →
//   `config {snapshot:"written", path, lines}`: the Config window and an open Edit-bench wizard show the new layer 5),
//   Export as preset (`{"type":"config","export":true}` when connected, for the core's layered assembly + current diff;
//   the app's own view after 3 s without an answer);
// - the configurator's reboot window: after a `reboot` event while `state.fc.msp` is false, "firmware booting — the
//   configurator can connect in ~N s" next to the Copy-configurator-address button (N = the last boot the app measured:
//   reboot → fc.msp true, host seconds).
using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using Godot;

namespace FlightBench;

public partial class Flight
{
    private Task<(LayeredConfig? Config, string Error)>? _layersTask;
    private string _layersWhy = "";
    private bool _layersAgain;
    private string? _exportFolder;                  // Export as preset waiting for the core's export answer
    private ulong _exportDeadline;
    private LayeredConfig? _coreExport;             // the core's layered assembly from `config export`
    private string? _coreExportDiff;
    private int _snapshotsWritten;                  // `config {snapshot:"written"}` seen (self-tests)
    private bool _changesPending;                   // a diff against the script asked, no answer yet
    // boot hint
    private Label? _bootHint;
    private ulong _rebootAt;                        // host ms of the last `reboot` event (0 = none pending)
    private double _lastBootS = double.NaN;         // reboot → fc.msp true, host seconds (the last one measured)

    private void BuildConfigPanel()
    {
        var p = _configPanel = new ConfigPanel(_diffView!);
        p.SetWarnParent(_ui);
        p.Logged += s => Log("app", s);
        p.Layers.BenchOf = () => _bench;
        p.Layers.Root = CoreLocator.DataRoot;
        p.Layers.CheckRequested += () => CheckLayers("the bench as it is now — not sent to the firmware yet: Apply script now (or the next start) sends it");
        p.Layers.BenchChanged += () => { if (DevEnv.SaveBench && !_deleted) _bench.Save(); };
        p.ApplyRequested += full => ApplyScript(full);
        p.RerunRequested += name => { if (_functions?.RunFunction(name) != true) Log("app", $"re-run {name}: not sent"); };
        p.ResetRequested += AskResetConfig;
        p.SnapshotRequested += () => SnapshotToBench();
        p.ExportRequested += ExportPreset;
        p.ChangesRequested += () => RequestChanges();
        p.SetLayersSource("the script: waiting for the bench check at the core's start");
        _core.ConfigReceived += OnConfigMessage;
        _core.ConnectionChanged += s =>
        {
            // a core the app does not start: ask it for its layered assembly
            if (s == "connected" && !_ownCore && _session.Send(new { type = "config", export = true })) Log("app", "config: asked the core for its layered script (config export)");
        };
    }

    /// A session-check of the bench as it is now → the Config window's left side.
    private void CheckLayers(string why)
    {
        if (_configPanel == null) return;
        if (_layersTask != null) { _layersAgain = true; _layersWhy = why; return; }
        var exe = _coreExe ?? CoreLocator.Find(out _);
        if (exe == null && LayeredConfig.Checker == null)
        {
            _configPanel.Layers.Show(_coreExport, _coreExport == null ? "no core executable on this machine to run the session-check (connect: the core's own export is used)" : "");
            return;
        }
        _layersWhy = why;
        _configPanel.Layers.SetBusy();
        _layersTask = LayeredConfig.CheckAsync(_bench.Clone(), exe, CoreLocator.DataRoot);
    }

    /// Per frame: the layers check, the export waiting for the core, the boot hint.
    private void PollConfig()
    {
        if (_layersTask is { IsCompleted: true } t)
        {
            _layersTask = null;
            _configPanel?.Layers.Show(t.Result.Config, t.Result.Error);
            _configPanel?.SetLayersSource(_layersWhy);
            if (_layersAgain) { _layersAgain = false; CheckLayers(_layersWhy); }
        }
        if (_exportFolder != null && Time.GetTicksMsec() > _exportDeadline)
        {
            Log("app", "export: no config export answer from the core within 3 s — the app's own view of the layers is used");
            FinishExport();
        }
        UpdateBootHint();
    }

    /// The launch's bench check: the script this core starts with (layers from the core, or inferred).
    private void LayersFromLaunch(CoreCheck c)
    {
        if (_configPanel == null || c.ExitCode != 0 || c.RunError != null) return;
        var lc = LayeredConfig.Parse(c.Stdout);
        lc.Infer(_bench, CoreLocator.DataRoot);
        _configPanel.Layers.Show(lc, "");
        _configPanel.SetLayersSource($"the script this core started with (its session-check at the launch, {DateTime.Now:HH:mm:ss}){(DevEnv.BenchFile != null ? " — FB_BENCH_FILE's session, not this bench's" : "")}");
    }

    // ---- operations ----
    /// Apply script now: the session file written again (a core that re-reads the bench takes the edits), then
    /// `{"type":"config","apply":true[,"full":true]}` — only while disarmed.
    public bool ApplyScript(bool full)
    {
        if (_fcArmed != false) { _configPanel?.SetStatus("not applied: " + (_fcArmed == true ? "the firmware is armed (disarm first)" : "the firmware's state is not known yet")); return false; }
        if (_ownCore && DevEnv.BenchFile == null)
        {
            var path = CoreLauncher.WriteSession(_bench, CoreLocator.DataRoot, out var err);
            Log("app", err == "" ? $"apply: the session file written again ({path})" : $"apply: the session file not written: {err}");
        }
        bool sent = full ? _session.Send(new { type = "config", apply = true, full = true }) : _session.Send(new { type = "config", apply = true });
        Log("app", sent ? $"apply script now{(full ? " (full)" : "")}: sent — the core types the script and saves; the firmware reboots" : "apply script now: not connected, nothing sent");
        _configPanel?.SetStatus(sent ? $"apply{(full ? " (full)" : "")} sent — watch Status for applying → applied" : "not connected", warn: !sent);
        return sent;
    }

    /// Snapshot firmware config → bench lines: the bench's `bench_cli` path (set and saved first), the core writes it.
    public bool SnapshotToBench()
    {
        if (_fcArmed != false) { _configPanel?.SetStatus("no snapshot: " + (_fcArmed == true ? "the firmware is armed (disarm first)" : "the firmware's state is not known yet")); return false; }
        _bench.BenchCli ??= BenchFiles.CliRel(_bench.Name);
        if (DevEnv.SaveBench && !_deleted) _bench.Save();
        var path = BenchFiles.CliPath(_bench);
        bool sent = _session.Send(new { type = "config", snapshot = "bench", path });
        Log("app", sent ? $"snapshot: asked the core to write the firmware's diff all as the bench lines {path} (the firmware reboots)" : "snapshot: not connected, nothing sent");
        _configPanel?.SetStatus(sent ? "snapshot requested — the core runs diff all (reboot) and writes the bench lines" : "not connected", warn: !sent);
        return sent;
    }

    /// Changes: `{"type":"diff","against":"script"}`.
    public bool RequestChanges()
    {
        bool sent = _session.Send(new { type = "diff", against = "script" });
        _changesPending = sent;
        Log("app", sent ? "config changes: asked the core for the firmware's diff against the script" : "config changes: not connected");
        return sent;
    }

    /// A plain `diff` while a comparison is pending: a core before config-layers took `against` for a refresh (it ran
    /// diff all — the firmware rebooted) — said in the Changes view, not guessed.
    private void ChangesNotUnderstood(JsonElement m)
    {
        if (!_changesPending || Json.Str(m, "source") != "request") return;
        _changesPending = false;
        _configPanel?.ChangesUnsupported("this core does not know `diff against script` (a core before config-layers): it ran a plain diff all instead (the firmware rebooted) — the right side of Script ↔ Firmware shows it");
        Log("app", "config changes: the core answered with a plain diff (no `against`): it does not compare with the script");
    }

    private void OnChanges(JsonElement m)
    {
        _changesPending = false;
        var c = ConfigChanges.FromMessage(m);
        _configPanel?.ShowChanges(c);
        if (c.Error != null) Log("core", $"config changes: not compared ({c.Error})");
        else Log("core", $"config changes against the script: {c.ChangedSettings.Count} changed, {c.OnlyFirmware.Count} only in the firmware, {c.OnlyScript.Count} only in the script — Windows → Config → Changes");
    }

    /// `{"type":"config",…}` answers (docs/protocol.md `config`): `snapshot:"written"|"failed"` (+ path, lines/error),
    /// `export:{bench,root,assembly:<config_json>,script,applied,diff:{text,stale,source,at}}`, or `error` alone.
    private void OnConfigMessage(JsonElement m)
    {
        var err = Json.Str(m, "error");
        if (Json.Str(m, "snapshot") is { } snap)
        {
            if (snap == "written" && err == null)
            {
                _snapshotsWritten++;
                var path = Json.Str(m, "path") ?? BenchFiles.CliPath(_bench);
                int lines = (int)Json.Num(m, "lines", -1);
                Log("core", $"snapshot: the firmware's config written as the bench lines {path}{(lines >= 0 ? $" ({lines} lines)" : "")} — layer 5 now carries it");
                _configPanel?.SetStatus($"snapshot written: {path}{(lines >= 0 ? $", {lines} lines" : "")} (bench lines, layer 5)", warn: false);
                _configPanel?.Layers.ReloadBenchLines();
                if (_wizard != null) { _wizard.EditedBench.BenchCli ??= _bench.BenchCli; _wizard.Layers.ReloadBenchLines(); }
                CheckLayers("the bench as it is now (after the snapshot: its bench lines = the firmware's diff) — not sent yet");
            }
            else { Log("core", $"snapshot: not written ({err ?? snap})"); _configPanel?.SetStatus($"snapshot refused: {err ?? snap}"); }
            return;
        }
        if (m.TryGetProperty("export", out var ex) && ex.ValueKind == JsonValueKind.Object)
        {
            var lc = new LayeredConfig();
            if (ex.TryGetProperty("assembly", out var asm) && asm.ValueKind == JsonValueKind.Object) lc.ApplyJson(asm);
            if (lc.Conflicts.Count == 0) lc.ComputeConflicts();
            _coreExport = lc;
            _coreExportDiff = ex.TryGetProperty("diff", out var df) && df.ValueKind == JsonValueKind.Object ? Json.Str(df, "text") : null;
            var applied = Json.TryBool(ex, "applied", out var ap) ? (ap ? "the sector holds exactly this script" : "edited since the sector was written") : "";
            Log("core", $"config export: {lc.Lines.Count} lines by layer ({string.Join(", ", LayeredConfig.Layers.Select(l => $"{l} {lc.Count(l)}"))}){(applied != "" ? $", {applied}" : "")}{(_coreExportDiff != null ? $", the current diff {_coreExportDiff.Split('\n').Length} lines" : "")}");
            if (!_ownCore || _configPanel?.Layers.Config == null) { _configPanel?.Layers.Show(lc, ""); _configPanel?.SetLayersSource("the script as the core assembles it (config export)"); }
            if (_exportFolder != null) FinishExport();
            return;
        }
        if (err != null) { Log("core", $"config: refused ({err})"); _configPanel?.SetStatus($"refused by the core: {err}"); return; }
        Log("core", "config: " + m.GetRawText());
    }

    /// Export as preset… into `folder`: waits up to 3 s for the core's export when connected, then writes the files.
    public void ExportPreset(string folder)
    {
        _exportFolder = folder;
        if (_session.Send(new { type = "config", export = true })) { _exportDeadline = Time.GetTicksMsec() + 3000; Log("app", $"export: asked the core for its layered script and current diff (config export), then {folder}"); }
        else FinishExport();
    }

    private void FinishExport()
    {
        var folder = _exportFolder;
        _exportFolder = null;
        if (folder == null) return;
        try
        {
            var files = BenchFiles.ExportPreset(_bench, _coreExport ?? _configPanel?.Layers.Config, _coreExportDiff ?? _bench.FirmwareDiff?.Text, folder, CoreLocator.DataRoot);
            Log("app", $"export as preset: {files.Count} files in {folder}: {string.Join(", ", files.Select(Path.GetFileName))}");
            _configPanel?.SetStatus($"exported to {folder}: {string.Join(", ", files.Select(Path.GetFileName))}", warn: false);
            LastExport = files;
        }
        catch (Exception e) { Log("app", $"export as preset: failed: {e.Message}"); _configPanel?.SetStatus("export failed: " + e.Message); }
    }
    /// The files the last export wrote (self-tests).
    public System.Collections.Generic.List<string>? LastExport { get; private set; }

    // ---- the configurator's reboot window ----
    private void OnRebootEvent()
    {
        _rebootAt = Time.GetTicksMsec();
        UpdateBootHint();
    }

    /// fc.msp from a state: true after a reboot = the boot is over (measured).
    private void BootMsp(bool msp)
    {
        if (msp && _rebootAt != 0)
        {
            _lastBootS = (Time.GetTicksMsec() - _rebootAt) / 1000.0;
            _rebootAt = 0;
            Log("app", $"firmware answered {_lastBootS:0.0} s after the reboot (the configurator can connect again)");
        }
    }

    public const string BootHintTip = "after \"Save and Reboot\" in the configurator wait for the green dot: the firmware reboots and answers MSP again only once it has booted";

    /// "firmware booting — the configurator can connect in ~N s" while a reboot is pending and fc.msp is false.
    private void UpdateBootHint()
    {
        if (_bootHint == null) return;
        bool on = _rebootAt != 0 && _session.Connected && _fcMsp != true;
        _bootHint.Visible = on;
        if (!on) return;
        var elapsed = (Time.GetTicksMsec() - _rebootAt) / 1000.0;
        _bootHint.Text = double.IsNaN(_lastBootS) ? "firmware booting — the configurator can connect once it answers"
                       : $"firmware booting — the configurator can connect in ~{Math.Max(1, (int)Math.Ceiling(_lastBootS - elapsed))} s";
    }

    public string BootHintText => _bootHint is { Visible: true } h ? h.Text : "";
    public string BootHintTooltip => _bootHint?.TooltipText ?? "";
}
