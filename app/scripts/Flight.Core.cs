// Flight screen ↔ the local core (CoreLauncher.cs): a bench in `core: local` mode (BenchData.Core) and a core
// executable found (CoreLocator) → the screen writes the bench as a session file, validates it
// (session-check), starts the core with the launch contract's command line on the bench's ports, waits for
// the control port and connects to 127.0.0.1; restart bench / Restart core = stop it and start it again;
// leaving the screen stops it (Settings → "keep the core running after closing the bench" keeps it for the
// same bench); app exit stops every core the app started (DevHooks). A `remote` bench (CoreHost:CorePort),
// FB_CORE or a self-test run: connect only. A remote bench opened on a machine that has a local core is
// never connected silently: the Bench menu offers "Use local core" (switches the bench to local, saves it, starts the
// core) and the log says so. The Bench menu's Core group (product decision 2026-10-01: the core is not the Status
// window's): a read-only line "Core: running · pid N · control C, msp M" (or what the core is doing / why the app
// does not start one), Restart core, Bench check, Core address… (the address field: typing a non-loopback host
// makes the bench remote), Use local core (when offered), Close bench. Nothing here blocks the main thread:
// launches, checks and stops run on tasks, polled from _Process.
using System;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using Godot;

namespace FlightBench;

public partial class Flight
{
    private bool _ownCore;                         // this screen starts / stops the core
    private string? _coreExe;
    private CoreLaunch? _launch;
    private CoreProcess? _proc;                    // the core this screen runs
    private long _procCursor;
    private string _coreState = "";                // Status window line
    private bool _crashReported;
    private CorePorts? _runPorts;                  // this run's ports (the bench's, or the next free ones the user took)
    private bool _waitManifest;                    // a start waits for the core's manifest (the app had none)
    private CorePorts? _waitPorts;
    private Task<CoreCheck>? _benchCheck;          // Bench → Bench check running
    private Task<CorePorts?>? _nextFree;           // "use the next free ports" looking
    private PopupMenu? _benchPopup;
    /// A core kept running after its flight screen closed (Settings → KeepCoreRunning): reused for the same bench.
    private static CoreProcess? _kept;

    /// Decides whether this screen owns the core (bench `local`, exe found, no FB_CORE, no self-test).
    private void DecideCore()
    {
        _coreExe = CoreLocator.Find(out var src);
        _ownCore = false;
        string why;
        _coreState = "";
        if (DevEnv.Core != null) why = $"FB_CORE={DevEnv.Core}: connecting only, no core started";
        else if (DevEnv.SelfTestRun) why = "self-test run: connecting only, no core started";
        else if (_bench.IsRemote)
        {
            why = $"remote core {_bench.CoreHost}:{_bench.CorePort} (bench core: remote): connecting only";
            _coreState = $"remote core {_bench.CoreHost}:{_bench.CorePort}";
            if (OffersLocalCore(_bench, _coreExe)) why += $"; a local core is available ({_coreExe}): Bench → Use local core switches this bench to it";
        }
        else if (_coreExe == null) why = $"no core executable ({src}): connecting only (Settings → Paths → core)";
        else
        {
            _ownCore = true; why = $"core {_coreExe} ({src}), data root {CoreLocator.DataRoot}";
            if (DevEnv.CorePorts is { } dp && _runPorts == null) { _runPorts = dp; why += $"; FB_CORE_PORTS: this run's ports {dp} (the bench keeps its own)"; }
        }
        Log("app", why);
        UpdateCoreRow();
    }

    /// The Bench menu's Use local core rule: a remote bench (not an FB_CORE run) on a machine where a local core was found.
    public static bool OffersLocalCore(BenchData b, string? localExe) => b.IsRemote && localExe != null && DevEnv.Core == null;

    /// "Use local core": the bench becomes local (saved when the run may write it), the core is started here.
    private void UseLocalCore()
    {
        if (!_bench.IsRemote) return;                    // already switched (the menu item goes with it)
        Log("app", $"use local core: bench '{_bench.Name}' switched from remote {_bench.CoreHost}:{_bench.CorePort} to local");
        _bench.Core = BenchData.CoreLocal;
        WriteBench("use local core");
        _session.Disconnect();
        DecideCore();
        UpdateStatus();
        StartCore();
    }

    /// Starts (or reuses) the core and connects; without an own core: just connects.
    private void StartCore(CorePorts? ports = null)
    {
        if (!_ownCore) { _session.Connect(); return; }
        if (_launch is { Outcome: LaunchOutcome.Running }) { Log("app", "core: a start is already running"); return; }
        _session.Disconnect();
        var root = CoreLocator.DataRoot;
        // the session is written from the manifest: a fresh install (built-in catalogue) asks the core for it first
        if (DevEnv.BenchFile == null && CoreManifest.Needed && _coreExe != null)
        {
            CoreManifest.Ensure(_coreExe, root);
            _waitManifest = true; _waitPorts = ports;
            _coreState = "reading the core's manifest…"; UpdateStatus();
            return;
        }
        string bench;
        if (DevEnv.BenchFile is { } bf) bench = System.IO.Path.GetFullPath(bf);
        else
        {
            CommitEdits();                                   // the firmware path typed in the Firmware window
            bench = CoreLauncher.WriteSession(_bench, root, out var err);
            if (err != "") { ShowError("Cannot write the session file", $"{bench}: {err}"); _coreState = "session file not written"; UpdateStatus(); return; }
        }
        ports ??= _runPorts;
        // a core kept running for this very bench (and ports): connect to it
        if (_kept is { } k && !k.HasExited && k.Key == CoreLauncher.KeyOf(bench, ports ?? k.Ports))
        {
            _proc = k; _kept = null; _runPorts = k.Ports; _procCursor = 0; _crashReported = false;
            Log("app", $"core: reusing the core kept running (pid {k.Pid}, {k.Ports})");
            CoreUp();
            return;
        }
        var prev = _proc ?? _kept ?? TakeHandoff();
        // the cursor counts the previous core's lines: the new process starts at line 0 (a stale cursor past its
        // end made ReadNew throw every frame, so a restart never reached OnLaunched / connect)
        _kept = null; _proc = null; _crashReported = false; _procCursor = 0;
        Log("app", $"core: {bench} (root {root}){(ports is { } pp ? $", ports {pp}" : "")}");
        _launch = CoreLaunch.Run(LaunchFor(bench, root, ports), prev);
        _coreState = _launch.Stage + "…";
        UpdateStatus();
    }

    /// The launch of this bench: its session file, the data root, this run's ports and the bench's config state file
    /// (`bench-state=`: the same file on every start of the same bench — Restart core, Edit bench → Save & start).
    /// `external`: the session is FB_BENCH_FILE's, not this bench's: no bench state.
    public LaunchRequest LaunchFor(string sessionFile, string root, CorePorts? ports, bool? external = null) =>
        new() { Exe = _coreExe ?? "", Bench = sessionFile, Root = root, Ports = ports, BenchState = (external ?? DevEnv.BenchFile != null) ? null : BenchState.PathFor(_bench.Name) };

    /// Restart bench / Restart core with an own core: stop it, check the bench again, start, connect.
    private void RestartCore()
    {
        if (!_ownCore) { Log("app", "restart core: this bench's core is not started by the app (remote / FB_CORE / no exe)"); return; }
        Log("app", "restart core: stopping it and starting the bench again");
        StartCore();
    }

    private void CoreUp()
    {
        if (_proc == null) return;
        _coreState = $"core running · pid {_proc.Pid} · control {_proc.Ports.Control}, msp {_proc.Ports.Msp}";
        _session.Connect("127.0.0.1", _proc.Ports.Control);
        UpdateCfgHint(); UpdateStatus(); UpdateCoreRow();
    }

    /// Every frame: the manifest fetch, the launch, the core's output, a crash.
    private void PollCore()
    {
        CoreManifest.Poll();
        if (_waitManifest && !CoreManifest.Busy)
        {
            _waitManifest = false;
            if (CoreManifest.Needed) Log("app", "core: no manifest from the core (" + CoreManifest.LastError + "): the session uses the built-in catalogue");
            CoreManifest.GiveUp();
            StartCore(_waitPorts);
        }
        if (_launch != null)
        {
            if (_launch.Process is { } lp) DrainCore(lp);
            if (_launch.Outcome == LaunchOutcome.Running)
            {
                var st = _launch.Stage + "…";
                if (st != _coreState) { _coreState = st; UpdateStatus(); }
            }
            else { var l = _launch; _launch = null; OnLaunched(l); }
        }
        if (_proc != null)
        {
            DrainCore(_proc);
            if (!_crashReported && !_proc.StopRequested && _proc.HasExited) OnCoreExited(_proc);
        }
        if (_benchCheck is { IsCompleted: true } bc) { _benchCheck = null; ShowBenchCheck(bc.Result); }
        if (_nextFree is { IsCompleted: true } nf)
        {
            _nextFree = null;
            if (nf.Result is { } p) { _runPorts = p; Log("app", $"core: using the next free ports for this run: {p}"); StartCore(p); }
            else ShowError("No free ports", "no free set of five ports within 200 above the bench's");
        }
    }

    private void DrainCore(CoreProcess p)
    {
        foreach (var line in p.ReadNew(ref _procCursor)) Log("core:", line);
    }

    private void OnLaunched(CoreLaunch l)
    {
        if (l.Check is { } c)
        {
            Log("app", "bench check: " + c.Summary);
            LayersFromLaunch(c);                             // the Config window: the script this core starts with
            foreach (var s in c.SessionLines) Log("core:", s);
        }
        if (l.BenchStateUsed is { } bs) Log("app", $"bench state: {bs} (the firmware config of this bench, kept across restarts; Bench → Reset firmware config to first start… discards it)");
        else if (l.BenchStateNote != "") Log("app", "bench state: " + l.BenchStateNote);
        switch (l.Outcome)
        {
            case LaunchOutcome.Started:
                _proc = l.Process; _runPorts = l.Ports;
                Log("app", $"core started: pid {_proc!.Pid}, {l.Ports}");
                CoreUp();
                break;
            case LaunchOutcome.Rejected:
                _coreState = "bench rejected by the core (nothing started)";
                Log("app", "core: bench rejected:\n" + l.Message);
                ShowError("Bench rejected", "The core rejected the bench (exit 2); nothing was started.\n\n" + l.Message);
                break;
            case LaunchOutcome.PortBusy:
                _coreState = l.Message.Split('\n')[0];
                Log("app", "core: " + l.Message);
                AskPortBusy(l);
                break;
            default:
                _coreState = "core not started";
                Log("app", "core: " + l.Message);
                ShowError("Core not started", l.Message);
                break;
        }
        UpdateStatus(); UpdateCoreRow();
    }

    private void OnCoreExited(CoreProcess p)
    {
        _crashReported = true;
        _coreState = $"core exited (code {p.ExitCode?.ToString() ?? "?"})";
        Log("app", $"{_coreState}; its last 20 lines:");
        foreach (var line in p.Tail(20)) Log("core:", line);
        if (_windows.TryGet("Log / CLI", out var w)) w.Visible = true;
        UpdateStatus(); UpdateCoreRow();
    }

    /// "port N is in use": use the next free ports for this run / connect to the core that answers / cancel.
    private void AskPortBusy(CoreLaunch l)
    {
        var dlg = new ConfirmationDialog
        {
            Title = "Port in use", Theme = Ui.BaseTheme, OkButtonText = "Use the next free ports",
            DialogText = l.Message.Split('\n')[0] + $"\n\nThis bench's ports: {l.Ports}.\nUse the next free ports (all five moved together) for this run?"
                         + (l.BusyAnswers ? "\nOr connect to the core that answers there (the app does not own it and will not stop it)." : ""),
        };
        dlg.Confirmed += () => { _coreState = "looking for free ports…"; UpdateStatus(); _nextFree = CoreLauncher.NextFreeAsync(l.Ports); };
        if (l.BusyAnswers)
        {
            dlg.AddButton("Connect to it", true, "connect");
            dlg.CustomAction += a =>
            {
                if (a.ToString() != "connect") return;
                dlg.Hide();
                _coreState = $"connected to a core the app did not start (port {l.Ports.Control})";
                _session.Connect("127.0.0.1", l.Ports.Control);
                UpdateStatus();
            };
        }
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        _ui.AddChild(dlg);
        dlg.PopupCentered();
    }

    /// Bench → Bench check: session-check on this bench, the report in a window.
    private void RunBenchCheck()
    {
        if (_benchCheck != null) return;
        var exe = _coreExe ?? CoreLocator.Find(out _);
        if (exe == null) { ShowError("Bench check", "No core executable found (Settings → Paths → core, or FB_CORE_EXE)."); return; }
        var root = CoreLocator.DataRoot;
        string bench;
        if (DevEnv.BenchFile is { } bf) bench = System.IO.Path.GetFullPath(bf);
        else
        {
            CommitEdits();
            bench = CoreLauncher.WriteSession(_bench, root, out var err);
            if (err != "") { ShowError("Bench check", $"cannot write {bench}: {err}"); return; }
        }
        Log("app", $"bench check: {bench}");
        _benchCheck = CoreLauncher.CheckAsync(exe, bench, root);
    }

    private void ShowBenchCheck(CoreCheck c)
    {
        Log("app", "bench check: " + c.Summary);
        var text = new TextEdit { Text = c.Report, Editable = false, CustomMinimumSize = new Vector2(860, 460), WrapMode = TextEdit.LineWrappingMode.None };
        var dlg = new AcceptDialog { Title = c.Ok ? "Bench check: OK" : c.Rejected ? "Bench check: rejected" : "Bench check: failed", Theme = Ui.BaseTheme };
        var box = new VBoxContainer();
        box.AddChild(new Label { Text = c.Summary, AutowrapMode = TextServer.AutowrapMode.WordSmart, CustomMinimumSize = new Vector2(860, 0) });
        box.AddChild(text);
        dlg.AddChild(box);
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        _ui.AddChild(dlg);
        dlg.PopupCentered();
    }

    private void ShowError(string title, string text)
    {
        var dlg = new AcceptDialog { Title = title, Theme = Ui.BaseTheme };
        dlg.AddChild(new Label { Text = text, AutowrapMode = TextServer.AutowrapMode.WordSmart, CustomMinimumSize = new Vector2(620, 0) });
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        _ui.AddChild(dlg);
        dlg.PopupCentered();
        GD.Print($"[app] {title}: {text.Replace("\n", " | ")}");
    }

    // ---- Bench menu: Edit / Save as / Save, the Core group, Close bench, Functions / Config ----
    public const int MenuEdit = 0, MenuSaveAs = 1, MenuSave = 2, MenuBenchCheck = 3, MenuRestartCore = 4, MenuClose = 5,
                     MenuFunctions = 6, MenuFirmwareConfig = 7, MenuCoreLine = 10, MenuCoreAddress = 11, MenuUseLocal = 12,
                     MenuDelete = 13, MenuResetConfig = 14;

    /// "Core: running · pid 1234 · control 31250, msp 31235" / what the app's core is doing / a core it does not start.
    public string CoreMenuLine()
    {
        if (_proc is { HasExited: false } p && _coreState.StartsWith("core running", StringComparison.Ordinal))
            return $"Core: running · pid {p.Pid} · control {p.Ports.Control}, msp {p.Ports.Msp}";
        if (_ownCore) return "Core: " + (_coreState != "" ? _coreState : "not started yet");
        var where = $"{_session.Host}:{_session.Port}";
        if (DevEnv.Core != null) return $"Core: {where} (FB_CORE: connect only)";
        if (_bench.IsRemote) return $"Core: remote {where} (connect only)";
        return $"Core: {where} (not started by the app: {(_coreExe == null ? "no core executable" : "connect only")})";
    }

    /// The Bench menu, rebuilt each time it opens (the Core line and what is offered follow the core).
    private void RebuildBenchMenu()
    {
        var bp = _benchPopup;
        if (bp == null) return;
        bp.Clear();
        void Item(string text, int id, string tip, bool disabled = false)
        {
            bp.AddItem(text, id);
            int i = bp.GetItemIndex(id);
            bp.SetItemTooltip(i, tip);
            bp.SetItemDisabled(i, disabled);
        }
        Item("Edit bench…", MenuEdit, "the wizard over this screen (the core keeps running meanwhile); Save & start restarts the core with the same bench state: only the changed config lines are applied");
        Item("Save as…", MenuSaveAs, "a copy of this bench (with the layout, camera, core address, firmware) under a new name; the screen goes on with the copy");
        Item("Save", MenuSave, "write the bench now (leaving the screen does the same)");
        bp.AddSeparator("Core");
        Item(CoreMenuLine(), MenuCoreLine, "the core this bench flies on (read-only)", disabled: true);
        Item("Restart core", MenuRestartCore, _ownCore ? "stop the core the app started, check the bench again and start it (connects when up)" : "only for a core the app started (this bench: connect only)", disabled: !_ownCore);
        Item("Bench check", MenuBenchCheck, "run the core's session-check on this bench and show the report (wiring, options, ports, warnings)");
        Item("Core address…", MenuCoreAddress, "the core's host:port (a remote core, e.g. the homelab over tailnet); a non-loopback address makes the bench remote (connect only)");
        if (OffersLocalCore(_bench, _coreExe) && !DevEnv.SelfTestRun)
            Item("Use local core", MenuUseLocal, $"this bench is remote ({_bench.CoreHost}:{_bench.CorePort}) and a local core is available: switch the bench to it, save it and start the core here");
        Item("Close bench", MenuClose, "back to the main menu; the core the app started is stopped (Settings: keep it running)");
        bp.AddSeparator();
        Item("Functions…", MenuFunctions, "the environment functions of this bench (calibrations, launch) with their progress");
        Item("Config…", MenuFirmwareConfig, "the firmware's config: the script by layer (editable at the source), the firmware's diff, Changes (configurator vs script), Apply / Snapshot / Export");
        Item("Reset firmware config to first start…", MenuResetConfig, _session.Connected
            ? "asks first; the core discards this bench's saved config and runs the first start again (the CLI script + the calibrations)"
            : "only while connected to the core", disabled: !_session.Connected);
        bp.AddSeparator();
        Item("Delete bench…", MenuDelete, "asks first; stops the core, deletes the bench file, its session file and its saved firmware config, back to the main menu");
    }

    private void OnBenchMenu(long id)
    {
        switch ((int)id)
        {
            case MenuEdit: EditBench(); break;
            case MenuSaveAs: AskSaveAs(); break;
            case MenuSave: WriteBench("save"); break;
            case MenuBenchCheck: RunBenchCheck(); break;
            case MenuRestartCore: RestartCore(); break;
            case MenuCoreAddress: AskCoreAddress(); break;
            case MenuUseLocal: UseLocalCore(); break;
            case MenuClose: GetTree().ChangeSceneToFile("res://scenes/MainMenu.tscn"); break;
            case MenuFunctions: _windows["Functions"].Visible = true; break;
            case MenuFirmwareConfig: _windows["Config"].Visible = true; break;
            case MenuResetConfig: AskResetConfig(); break;
            case MenuDelete: AskDeleteBench(); break;
        }
    }

    /// The core's state changed: an open Bench menu shows it at once (a closed one is rebuilt when it opens).
    private void UpdateCoreRow()
    {
        if (_benchPopup is { Visible: true }) RebuildBenchMenu();
    }

    /// Bench → Core address…: host:port; Enter / OK applies it (ApplyCoreAddress).
    private void AskCoreAddress()
    {
        var edit = new LineEdit { Text = $"{_session.Host}:{_session.Port}", PlaceholderText = "host:port", SelectAllOnFocus = true, CustomMinimumSize = new Vector2(360, 0) };
        var box = new VBoxContainer();
        box.AddChild(new Label { Text = "Core address (control port):" });
        box.AddChild(edit);
        box.AddChild(new Label { Text = "a non-loopback host makes this bench remote (connect only); Bench → Use local core switches back", Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart, CustomMinimumSize = new Vector2(360, 0) });
        var dlg = new ConfirmationDialog { Title = "Core address", OkButtonText = "Connect", Theme = Ui.BaseTheme };
        dlg.AddChild(box);
        dlg.RegisterTextEnter(edit);
        dlg.Confirmed += () => ApplyCoreAddress(edit.Text);
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        _ui.AddChild(dlg);
        dlg.PopupCentered();
        edit.GrabFocus();
    }

    /// A typed core address: the bench remembers it; a non-loopback one makes the bench remote; connects there.
    public bool ApplyCoreAddress(string text)
    {
        if (CoreSession.ParseAddress(text) is not { } a) { Log("app", "core address must be host:port"); return false; }
        _bench.CoreHost = a.Host; _bench.CorePort = a.Port;
        // a non-loopback address makes the bench remote; a loopback one typed on a remote bench does not make it local
        // (typing connects, it starts nothing): Bench → Use local core does that
        bool toRemote = !CoreLocator.IsLocal(a.Host) && !_bench.IsRemote;
        if (toRemote) { _bench.Core = BenchData.CoreRemote; Log("app", $"core address {a.Host}:{a.Port} typed: this bench is now 'remote' (connect only)"); }
        if (DevEnv.SaveBench) _bench.Save();
        if (toRemote) DecideCore();                  // Restart core off, Use local core offered (a core the app started runs until the bench closes)
        _session.Connect(a.Host, a.Port);
        UpdateCfgHint(); UpdateStatus();
        return true;
    }

    /// Main menu → Delete…: a core kept running for that bench (Settings → keep the core running) is stopped first.
    public static void ReleaseKeptCore(string benchName)
    {
        if (_kept is not { } k || k.HasExited) return;
        var ses = System.IO.Path.GetFullPath(ProjectSettings.GlobalizePath(CoreLauncher.SessionFileFor(benchName)));
        if (!k.Key.StartsWith(ses + "|", StringComparison.OrdinalIgnoreCase)) return;
        GD.Print($"[app] delete bench '{benchName}': stopping the core kept running for it (pid {k.Pid})");
        _kept = null;
        k.StopNow(5000);
    }

    /// Leaving the screen: stop the core (or keep it for the same bench), and a start still running.
    private void StopCoreOnClose()
    {
        if (_launch is { } l)
        {
            _launch = null;
            _ = l.Task.ContinueWith(_ => l.Process?.StopNow(3000));
        }
        if (_proc == null || _proc.HasExited) return;
        if (AppSettings.Current.KeepCoreRunning)
        {
            _kept = _proc;
            GD.Print($"[app] core pid {_proc.Pid} kept running after closing the bench (Settings → keep the core running)");
        }
        else
        {
            GD.Print($"[app] closing the bench: stopping core pid {_proc.Pid}");
            _ = _proc.StopAsync();
        }
        _proc = null;
    }
}

/// The core's manifest when the app has none (a fresh install: built-in catalogue): read from
/// `session-check=on` once, set as the live manifest (and cached for the wizard).
public static class CoreManifest
{
    private static Task<CoreCheck>? _task;
    private static bool _gaveUp;
    public static string LastError { get; private set; } = "";
    public static bool Needed => !_gaveUp && Manifest.Current.Source == "built-in fallback" && !DevEnv.SelfTestRun;
    public static bool Busy => _task != null;

    public static void Ensure(string exe, string workDir)
    {
        if (_task != null || !Needed) return;
        GD.Print("[app] no manifest yet: reading it from the core");
        _task = CoreLauncher.ManifestAsync(exe, workDir);
    }

    public static void GiveUp() => _gaveUp = true;

    /// Main thread: applies a finished fetch.
    public static void Poll()
    {
        if (_task is not { IsCompleted: true } t) return;
        _task = null;
        var c = t.Result;
        if (c.ManifestJson is not { } json || c.ExitCode != 0) { LastError = c.RunError ?? $"exit {c.ExitCode}, no manifest line"; _gaveUp = true; return; }
        try
        {
            using var doc = JsonDocument.Parse(json);
            Manifest.SetLive(doc.RootElement);
            GD.Print("[app] manifest from the core: " + Manifest.Current.Describe());
        }
        catch (JsonException e) { LastError = e.Message; _gaveUp = true; }
    }
}
