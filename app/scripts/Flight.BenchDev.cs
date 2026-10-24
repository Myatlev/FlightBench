// Flight screen dev knobs of the bench features (screenshots and a live check against a real core):
// - FB_CLI_SEND=<line>: once connected (20 states in), send that CLI line — the firmware enters its CLI (Status: "in the CLI —
//   MSP blocked" + exit CLI (reboots)); for the CLI-indicator screenshot.
// - FB_DIALOG=delete|reset|edit: after 30 frames open Bench → Delete bench… / Reset firmware config… (the confirmations,
//   never confirmed) or Edit bench… (the wizard over the screen); for screenshots.
// - FB_DIALOG=apply|snapshot: the Config window's Apply script now / Snapshot confirmations (never confirmed).
// - FB_CONFIG=script|changes: once connected (50 states in) the Config window opens in that mode (changes: asks the core
//   for the diff against the script); for screenshots.
// - FB_EDIT_RESTART=1: the Edit bench → Save & start path without the wizard, against the core the app started: once the
//   firmware answers (fc.msp) the bench is applied again as edited (a copy, same name), the core is stopped, checked and
//   started again with the same bench state; once the firmware answers on the new core: `[edit-restart] PASS: …` (pids,
//   ports, the bench-state line) and quit. A run with it does not write the bench.
using System;
using System.Linq;
using Godot;

namespace FlightBench;

public partial class Flight
{
    private bool _devCliSent, _devDialogDone, _devConfigDone;
    private int _devEditStage;
    private int _devEditPid1;
    private ulong _devEditDeadline;
    private int _devDialogFrames;

    private void BenchDevTick()
    {
        if (DevEnv.CliSend is { } line && !_devCliSent && _session.Connected && _stateCount > 20)
        {
            _devCliSent = true;
            Log("app", $"FB_CLI_SEND: sending the CLI line '{line}'");
            _cli?.Sent?.Invoke(line);
        }
        if (DevEnv.Dialog is { } dlg && !_devDialogDone && ++_devDialogFrames == 30)
        {
            _devDialogDone = true;
            switch (dlg)
            {
                case "delete": AskDeleteBench(); break;
                case "reset": AskResetConfig(); break;
                case "edit": EditBench(); break;
                case "apply": _windows["Config"].Visible = true; _configPanel?.PressApply(); break;
                case "snapshot": _windows["Config"].Visible = true; _configPanel?.PressSnapshot(); break;
                default: Log("app", $"FB_DIALOG={dlg}: unknown (delete|reset|edit|apply|snapshot)"); break;
            }
        }
        if (DevEnv.ConfigMode is { } cm && !_devConfigDone && _session.Connected && _stateCount > 50)
        {
            _devConfigDone = true;
            var w = _windows["Config"];
            w.Position = new Vector2I(40, 60); w.Size = new Vector2I(1500, 800); w.Visible = true;
            _configPanel?.SetMode(cm == "changes" ? 1 : 0);
            Log("app", $"FB_CONFIG={cm}: the Config window open");
        }
        if (DevEnv.EditRestart) EditRestartTick();
    }

    private void EditRestartTick()
    {
        switch (_devEditStage)
        {
            case 0:
                if (_devEditDeadline == 0) _devEditDeadline = Time.GetTicksMsec() + 240000;
                if (!_ownCore) { GD.Print("[edit-restart] FAIL: the app does not start the core for this bench (remote / FB_CORE / no core)"); _devEditStage = 9; GetTree().Quit(1); return; }
                if (_proc is { HasExited: false } p && _session.Connected && _fcMsp == true && _stateCount > 50)
                {
                    _devEditPid1 = p.Pid;
                    GD.Print($"[edit-restart] first core pid {p.Pid} ({p.Ports}), firmware answers; applying the bench as edited (same name)");
                    _devEditStage = 1;
                    ApplyEditedBench(_bench.Clone());
                }
                break;
            case 1:
                if (_proc is { HasExited: false } q && q.Pid != _devEditPid1 && _session.Connected && _fcMsp == true && _stateCount > 50)
                {
                    bool firstGone = !System.Diagnostics.Process.GetProcesses().Any(x => x.Id == _devEditPid1 && x.ProcessName.StartsWith("qemu", StringComparison.OrdinalIgnoreCase));
                    GD.Print($"[edit-restart] {(firstGone ? "PASS" : "FAIL")}: first core pid {_devEditPid1} {(firstGone ? "gone" : "STILL RUNNING")}; restarted pid {q.Pid} on {q.Ports}, connected, the firmware answers; "
                             + $"bench state {(LogHas("bench state: ") ? "logged" : "not logged")}");
                    _devEditStage = 9;
                    GetTree().Quit(firstGone ? 0 : 1);
                }
                break;
        }
        if (_devEditStage < 9 && Time.GetTicksMsec() > _devEditDeadline)
        {
            GD.Print($"[edit-restart] FAIL: timed out at stage {_devEditStage} (core: {_coreState})");
            _devEditStage = 9;
            GetTree().Quit(1);
        }
    }
}
