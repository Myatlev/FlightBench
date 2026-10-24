// Flight screen, the bench as a whole (product owner's test 2026-10-01):
// - Bench → Edit bench…: the wizard in a window over this screen (the core keeps running while it is open); Save & start
//   writes the bench and applies it in place — the core is stopped, the bench checked again (session-check) and started
//   with the same `bench-state=` file (BenchState), so the core restores this bench's firmware config and applies only
//   the changed lines of the config script (Status: "config: applying N new lines (delta)"). A changed board / aircraft /
//   visual needs a new 3D scene: then the screen is opened again on the edited bench, the running core handed over to it
//   to be stopped first. Cancel changes nothing.
// - Bench → Reset firmware config to first start…: asks, then `{"type":"config","reset":"first_start"}` (the core
//   discards the bench state and runs the full first start again).
// - Bench → Delete bench…: asks (naming the files), stops the app's core, deletes the bench file, its session file and its
//   bench-state files (BenchData.Delete; never a bundled preset), back to the main menu without writing the bench again.
// - The firmware's CLI mode (`state.fc.cli`, the `cli` event {open, by}): Status says "in the CLI — MSP blocked" with an
//   "exit CLI (reboots)" button, the Log / CLI window the same next to its CLI line, the configurator-address tooltip too;
//   the button sends `{"type":"cli","exit":true}` (the core sends `exit`; the firmware reboots).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using System.Text.Json;
using Godot;

namespace FlightBench;

public partial class Flight
{
    private Window? _wizardWin;
    private Wizard? _wizard;
    private bool _deleted;                          // the bench was deleted: nothing writes it again (_ExitTree, modes)
    private Task? _deleteStop;                      // Delete bench: the core being stopped before the files go
    private List<string>? _deleteFiles;
    /// A core handed over by the screen that reopened itself on an edited bench (board / aircraft changed): stopped before the new start.
    private static CoreProcess? _handoff;
    private Label? _cliLogLabel;
    private int _cliOpened, _cliClosed;             // `cli` events seen (self-tests)
    private Button? _cliExitBtn;

    /// Is the Edit bench wizard open over the screen (keys go to it, not to the bench)?
    public bool WizardOpen => _wizardWin != null;

    // ---- Edit bench in place ----
    /// Bench → Edit bench…: the layout into the bench, the wizard over the screen on a copy of it. The core keeps running.
    private void EditBench()
    {
        if (_wizardWin != null) { _wizardWin.GrabFocus(); return; }
        CommitEdits();
        if (DevEnv.SaveBench && !_deleted) _bench.Save();
        var vp = GetViewport().GetVisibleRect().Size;
        var size = new Vector2I((int)Math.Clamp(vp.X - 60, 560, 1240), (int)Math.Clamp(vp.Y - 90, 420, 940));
        var pos = new Vector2I(Math.Max(0, ((int)vp.X - size.X) / 2), Math.Max(34, ((int)vp.Y - size.Y) / 2));
        _wizard = Wizard.Over(_bench);
        _wizard.Saved += ApplyEditedBench;
        _wizard.Closed += CloseEditWizard;
        var win = _wizardWin = new Window { Title = $"Edit bench: {_bench.Name}", Transient = true, Exclusive = true, Theme = Ui.BaseTheme, Visible = false };
        win.AddChild(_wizard);
        win.CloseRequested += CloseEditWizard;
        _ui.AddChild(win);
        win.Popup(new Rect2I(pos, size));
        Log("app", "edit bench: the wizard over the bench (the core keeps running; Save & start restarts it with this bench's state, Cancel changes nothing)");
    }

    private void CloseEditWizard()
    {
        var w = _wizardWin;
        if (w == null) return;
        _wizardWin = null; _wizard = null;
        w.Hide();
        w.QueueFree();
    }

    /// Save & start in the wizard over the screen: the edited bench (already written by the wizard) becomes this screen's
    /// bench and its core is restarted with the same bench state; a new board / aircraft / visual reopens the screen.
    public void ApplyEditedBench(BenchData b)
    {
        CloseEditWizard();
        var old = _bench;
        bool sameBench = BenchData.SafeName(b.Name) == BenchData.SafeName(old.Name);
        // the window layout / camera / checked signals stay as they are on the screen (the wizard does not edit them)
        if (DevEnv.SaveBench)
        {
            _windows.SaveInto(b);
            if (_signals != null) b.Signals = _signals.CheckedIds();
            b.Save();
        }
        _bench = b;
        AppState.Bench = b;
        Log("app", $"edit bench: '{b.Name}' saved{(sameBench ? "" : $" (a new bench: '{old.Name}' stays as it was)")}; restarting with {(sameBench ? "the same" : "its own")} bench state {BenchState.PathFor(b.Name)}");
        if (b.Board != old.Board || b.Aircraft != old.Aircraft || b.Visual != old.Visual)
        {
            // a new 3D scene: the screen opens again on the edited bench; the running core is stopped by the new screen first
            Log("app", $"edit bench: board / aircraft changed ({old.Board}/{old.Aircraft} → {b.Board}/{b.Aircraft}): reopening the flight screen");
            AppState.Notes.Add($"edit bench: '{b.Name}' saved; the flight screen reopened for {b.Board} / {b.Aircraft}");
            _handoff = _proc; _proc = null;
            _deleted = true;                                  // this screen must not write the (old) bench on exit
            if (!EditInPlaceTest) GetTree().CallDeferred(SceneTree.MethodName.ChangeSceneToFile, "res://scenes/Flight.tscn");
            return;
        }
        // in place: what this screen built from the bench follows it
        _fwPath?.SetText(b.FirmwarePath);
        _watch?.SetPressedNoSignal(b.WatchFirmware);
        UpdateBenchLabel();
        DeriveSwitches();                                     // the Transmitter rows follow the edited transmitter model
        var m = Manifest.Current;
        if (_wiringView != null) { _wiringView.Setup(b, m.PadsFor(b.Board), m.ActuatorsFor(b.Aircraft), readOnly: true); _wiringView.Rebuild(); }
        _faults?.Build(m, WiredDevices());
        RefreshFunctionsCatalog();
        _plane.Config.Reset();
        UpdateCfgHint();
        DecideCore();
        if (_ownCore) { RestartCore(); return; }
        // connect only (remote / FB_CORE / self-test): the bench's address unless FB_CORE fixes this run's
        if (DevEnv.Core == null && (b.ConnectHost != _session.Host || b.CorePort != _session.Port)) _session.Connect(b.ConnectHost, b.CorePort);
        else { _session.Disconnect(); _session.Connect(); }
        UpdateStatus();
    }

    /// The core a previous screen handed over (edit bench with a new board / aircraft): the launch stops it first.
    private static CoreProcess? TakeHandoff() { var h = _handoff; _handoff = null; return h; }

    // ---- Reset firmware config ----
    private void AskResetConfig()
    {
        var dlg = new ConfirmationDialog
        {
            Title = "Reset firmware config", Theme = Ui.BaseTheme, OkButtonText = "Reset to first start",
            DialogText = $"Reset the firmware config of '{_bench.Name}' to its first start?\n\nThe core discards this bench's saved config "
                         + "(calibrations, modes and settings made in the configurator) and runs the first start again: the CLI script "
                         + "and the first-start calibrations (a few minutes). The firmware reboots.",
        };
        dlg.Confirmed += () => ResetConfig();
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        _ui.AddChild(dlg);
        dlg.PopupCentered();
    }

    /// Sends `{"type":"config","reset":"first_start"}` (only while connected).
    public bool ResetConfig()
    {
        bool sent = _session.Send(new { type = "config", reset = "first_start" });
        Log("app", sent ? "reset firmware config: asked the core to discard this bench's config and run the first start again"
                        : "reset firmware config: not connected, nothing sent");
        return sent;
    }

    // ---- Delete bench ----
    private void AskDeleteBench()
    {
        var dlg = MainMenu.DeleteDialog(_bench);
        dlg.Confirmed += DeleteBench;
        _ui.AddChild(dlg);
        dlg.PopupCentered();
    }

    /// Stops the app's core (on a task), then deletes the bench's files (PollDelete) and goes back to the main menu.
    public void DeleteBench()
    {
        if (_deleteStop != null) return;
        _deleted = true;
        Log("app", $"delete bench '{_bench.Name}': stopping the core, then deleting its files");
        _session.Disconnect();
        var proc = _proc; _proc = null;
        var launch = _launch; _launch = null;
        var kept = _kept != null && _kept.Key.StartsWith(System.IO.Path.GetFullPath(ProjectSettings.GlobalizePath(CoreLauncher.SessionFileFor(_bench.Name))) + "|", StringComparison.OrdinalIgnoreCase) ? _kept : null;
        if (kept != null) _kept = null;
        _deleteStop = Task.Run(async () =>
        {
            if (launch != null) { try { await launch.Task; } catch (Exception) { } launch.Process?.StopNow(5000); }
            proc?.StopNow(5000);
            kept?.StopNow(5000);
        });
    }

    /// Per frame: the core is gone → the files go → the main menu.
    private void PollDelete()
    {
        if (_deleteStop is not { IsCompleted: true }) return;
        _deleteStop = null;
        bool ok = BenchData.Delete(_bench.Name, out var gone, out var err);
        _deleteFiles = gone;
        Log("app", $"delete bench '{_bench.Name}': {(gone.Count > 0 ? "deleted " + string.Join(", ", gone) : "nothing on disk")}{(ok ? "" : "; " + err)}");
        if (!DeleteTest) GetTree().CallDeferred(SceneTree.MethodName.ChangeSceneToFile, "res://scenes/MainMenu.tscn");
    }

    // ---- the firmware's CLI mode ----
    public const string CliText = "in the CLI — MSP blocked";

    /// The `cli` event {open, by}: the Status line and the buttons follow at once (the state's fc.cli confirms it).
    private void OnCliEvent(JsonElement data)
    {
        if (data.ValueKind != JsonValueKind.Object || !Json.TryBool(data, "open", out var open)) return;
        _fc.Cli = open;
        if (open) _cliOpened++; else _cliClosed++;
        UpdateCliIndicator();
    }

    /// The Log / CLI window's indicator + exit button, the Status button, the configurator tooltip.
    private void UpdateCliIndicator()
    {
        bool cli = _session.Connected && _fc.Cli == true;
        if (_cliLogLabel != null) _cliLogLabel.Visible = cli;
        if (_cliExitBtn != null) _cliExitBtn.Disabled = !_session.Connected;
        _statusPanel?.SetCli(cli);
        UpdateCfgHint();
    }

    /// "exit CLI (reboots)" (Status and Log / CLI): `{"type":"cli","exit":true}` — the core sends `exit`, the firmware reboots.
    public bool ExitCli()
    {
        bool sent = _session.Send(new { type = "cli", exit = true });
        Log("app", sent ? "exit CLI: asked the core to leave the firmware's CLI (the firmware reboots; MSP answers again after the boot)" : "exit CLI: not connected, nothing sent");
        return sent;
    }

    /// Self-tests: the screen's delete / edit-in-place paths without leaving the screen.
    public static bool DeleteTest, EditInPlaceTest;
}
