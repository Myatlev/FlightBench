// Flight screen, the plane and the core's events: Launch (bottom bar button + the `launch` key; enabled for a
// plane bench while connected; sends {"type":"launch"}, the scenario's defaults, or the popover's overrides), the
// core's events into PlaneStatus (the Aircraft window's launch / step / mix / crash rows and banner; the first-start
// `config` progress line in Status), and the Transmitter window's keyboard hint and order line (`switchOrder`).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

/// Aircraft facts the app needs before any core speaks: the FDM kind from the manifest, else models/<id>/aircraft.json.
public static class AircraftInfo
{
    public static string Fdm(string aircraft)
    {
        if (Manifest.Current.Aircraft.Find(a => a.Id == aircraft) is { Fdm.Length: > 0 } m) return m.Fdm;
        try
        {
            var f = Path.Combine(CoreLocator.DataRoot, "models", aircraft, "aircraft.json");
            if (!File.Exists(f)) return "";
            using var doc = JsonDocument.Parse(File.ReadAllText(f));
            return Json.Str(doc.RootElement, "fdm") ?? "";
        }
        catch (Exception) { return ""; }
    }

    public static bool IsPlane(string aircraft) => Fdm(aircraft) == "plane";
}

public partial class Flight
{
    private readonly PlaneStatus _plane = new();
    private bool _isPlane;                         // the bench's aircraft has the plane FDM: Launch is offered
    private bool? _fcMsp;                          // state fc.msp: the firmware answers MSP (false while booting / configuring)
    private Button? _launchBtn, _launchOptsBtn;
    private PopupPanel? _launchPopup;
    private CheckBox? _launchOverride;
    private SpinBox? _launchSpeed, _launchDuration, _launchPitch;
    private Label? _keysHint;
    private ulong _configShotFrame;                // FB_SCREENSHOT_CONFIG: the frame to save it at (0 = not armed)
    private bool _configShotDone;
    private readonly EventCollapse _beeps = new();   // beep / motor_beep: one Log line per burst + a count per second

    /// Launch popover (optional overrides; unchecked = {"type":"launch"}: the running scenario's `launch` defaults).
    private void BuildLaunchControls(HBoxContainer hb)
    {
        _launchBtn = new Button { Text = "Launch" + Keybinds.Suffix("launch"), TooltipText = "throw the plane: {\"type\":\"launch\"} (the scenario's launch: 50 km/h in 0.5 s at 10° unless overridden ▾).\nBefore: ANGLE + NAV LAUNCH on, ARM with the throttle low, throttle up." };
        _launchBtn.Pressed += () => Run("launch");
        _launchOptsBtn = new Button { Text = "▾", TooltipText = "launch parameters (override the scenario's defaults)" };
        _launchPopup = new PopupPanel { Theme = Ui.BaseTheme };
        var box = new VBoxContainer();
        _launchOverride = new CheckBox { Text = "override the scenario's launch" };
        box.AddChild(_launchOverride);
        SpinBox Row(string label, double min, double max, double step, double val, string suffix)
        {
            var r = new HBoxContainer();
            r.AddChild(new Label { Text = label, CustomMinimumSize = new Vector2(110, 0) });
            var sb = new SpinBox { MinValue = min, MaxValue = max, Step = step, Value = val, Suffix = suffix, CustomMinimumSize = new Vector2(120, 0) };
            r.AddChild(sb);
            box.AddChild(r);
            return sb;
        }
        _launchSpeed = Row("speed", 1, 360, 1, 50, "km/h");
        _launchDuration = Row("duration", 0.05, 10, 0.05, 0.5, "s");
        _launchPitch = Row("pitch", -60, 60, 1, 10, "°");
        box.AddChild(new Label { Text = "iNav detects a throw above 18.63 m/s² for 40 ms:\nkeep speed/duration ≥ 22.4 m/s² (50 km/h: ≤ 0.62 s)", Modulate = Ui.Soft });
        _launchPopup.AddChild(box);
        _launchOptsBtn.Pressed += () =>
        {
            var r = _launchOptsBtn.GetGlobalRect();
            _launchPopup.Popup(new Rect2I((Vector2I)(r.Position - new Vector2(0, 230)), new Vector2I(300, 220)));
        };
        _ui.AddChild(_launchPopup);
        hb.AddChild(_launchBtn); hb.AddChild(_launchOptsBtn);
        UpdateLaunchButton();
    }

    /// The `launch` message: exactly {"type":"launch"} unless the popover's override is on.
    public object LaunchMessage()
    {
        if (_launchOverride?.ButtonPressed == true && _launchSpeed != null && _launchDuration != null && _launchPitch != null)
            return new { type = "launch", speed_kmh = _launchSpeed.Value, duration_s = _launchDuration.Value, pitch_deg = _launchPitch.Value };
        return new { type = "launch" };
    }

    /// Launch (button / key): only a plane bench, only while connected; the core answers with `launch` events.
    private void DoLaunch()
    {
        if (!_isPlane) { Log("app", $"launch: '{_bench.Aircraft}' is not a plane (no launch)"); return; }
        if (!_session.Connected) { Log("app", "launch: not connected to the core"); return; }
        var m = LaunchMessage();
        if (_session.Send(m)) Log("app", "launch sent: " + JsonSerializer.Serialize(m));
    }

    private void UpdateLaunchButton()
    {
        bool on = _isPlane && _session.Connected;
        if (_launchBtn != null)
        {
            _launchBtn.Disabled = !on;
            _launchBtn.Visible = _isPlane;              // a heli bench: no Launch in the bar (disabled + hidden)
        }
        if (_launchOptsBtn != null) { _launchOptsBtn.Disabled = !on; _launchOptsBtn.Visible = _isPlane; }
    }

    /// A config `applied` / `restored` line stays this long in Status, then hides (host ms).
    public const ulong ConfigShowMs = 6000;

    /// Every `event` of the core: into the model, one log line, Status / Aircraft refreshed.
    private void OnCoreEvent(string name, JsonElement data, double t)
    {
        if (name == "cli") OnCliEvent(data);
        if (name == "reboot") OnRebootEvent();                  // the configurator's reboot window (Flight.Config.cs)
        if (name == "config" && data.ValueKind == JsonValueKind.Object && data.TryGetProperty("snapshot", out _)) OnConfigMessage(data);
        var (src, text) = _plane.ApplyEvent(name, data, t);
        if (EventCollapse.Collapses(name)) foreach (var l in _beeps.Take(name, text, t, Time.GetTicksMsec())) Log(src, l);
        else Log(src, text);
        if (name == "crash" && _windows.TryGet("Aircraft", out var w)) w.Visible = true;
        if (name == "config" && _plane.Config.State is "applied" or "restored") _cfgHideAt = Time.GetTicksMsec() + ConfigShowMs;
        if (name == "config" || _plane.Config.State == "calibrating") ArmConfigShot();
        UpdateStatus();
    }

    /// FB_SCREENSHOT_CONFIG=path: one screenshot while the calibration runs (the third pose), for the first-start evidence.
    private void ArmConfigShot()
    {
        if (DevEnv.ScreenshotConfig == null || _configShotDone || _configShotFrame != 0) return;
        if (_plane.Config.State == "calibrating" && _plane.Config.Poses.Count >= 3) _configShotFrame = Engine.GetProcessFrames() + 20;
    }

    /// Per frame: a closed beep window's count into the Log.
    private void BeepFlushTick()
    {
        if (_beeps.Flush(Time.GetTicksMsec()) is { } beeps) Log("event", beeps);
    }

    private void ConfigShotTick()
    {
        if (_configShotFrame == 0 || _configShotDone || Engine.GetProcessFrames() < _configShotFrame || DevEnv.ScreenshotConfig is not { } path) return;
        _configShotDone = true;
        GetViewport().GetTexture().GetImage().SavePng(path);
        GD.Print($"[dev] FB_SCREENSHOT_CONFIG: {_plane.Config.Line} at t = {_lastT:0.00} s, saved to {path}");
    }

    /// The keyboard as the transmitter: which keys move what (Settings → Keys / Transmitter change them).
    private void UpdateKeysHint()
    {
        if (_keysHint == null) return;
        _keysHint.Text = Keybinds.Hint(_isPlane);                    // from the bind table, never a literal
        _keysHint.TooltipText = "the keyboard is a layer over channels 1..4, always on beside the joypad: the last input that moved a channel holds it "
                              + "(the bars' marks: joy / kb / ui); the collective / throttle keys are an integrator (the value holds); the switch keys step the rows above. Settings → Keys lists every bind";
    }

    /// The bench's `switchOrder` under the switches (the order to use them in); null when the bench has none.
    private Control? BuildOrderLine() =>
        _bench.SwitchOrder is { } order ? new Label { Text = "order: " + order, AutowrapMode = TextServer.AutowrapMode.WordSmart, Modulate = Ui.Soft } : null;
}
