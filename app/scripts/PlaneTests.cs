// Self-tests of the plane on the flight screen (FB_SELFTEST=all, or plane_state,plane_visual,launch_cmd,config_progress):
// the runner opens the flight screen twice with in-memory benches (nothing written), connected to the local
// (fake) core like the keys test:
//   1. the trainer (the bundled preset `inav-trainer-launch`, renamed): plane_state (PlaneStatus + AircraftReadout fed
//      a `state.plane` sample + the plane's events, then the same lines injected into the screen: the Aircraft window's
//      rows, crash banner, Reset aircraft clears it), config_progress (ConfigProgress: applying → calibrating with poses
//      k/6 → applied, the firmware-wait line, failed; then injected into the screen with pauses: the Status config line
//      follows, the connection stays, nothing times out), plane_visual (models/trainer/visual/trainer.tscn loaded, the four
//      surface pivots + prop resolved, a state's surfaces_deg applied with the trailing edges moving the right way,
//      the prop spinning), launch_cmd part 1 (the Launch button and the `launch` key each send exactly
//      {"type":"launch"}, the popover's override adds the three fields; the trainer's channels are all `auto` and its modes are ignored on this
//      screen, so the Transmitter has no row and a switch key sends nothing — no toggle of the app's own any more);
//   2. an RS7 bench: launch_cmd part 2 (the Launch button hidden + disabled, the key sends nothing) → report.
// Messages are injected with CoreClient.InjectForTest (as if the core had sent them).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Godot;
using FlightBench.Models;

namespace FlightBench;

public enum PlaneTestMode { None, Plane, Heli }

public static class PlaneTests
{
    public static readonly string[] Names = { "plane_state", "plane_visual", "launch_cmd", "config_progress" };
    public static bool Any => Names.Any(DevEnv.SelfTest);

    /// launch_cmd's part 1 (plane screen) waiting for part 2 (heli screen).
    public static List<string>? LaunchFails;
    public static string LaunchDetail = "";
    public static bool HeliDone;

    public const string StateSample = "{\"type\":\"state\",\"t\":24.6,\"pos_ned\":[0,0,-1.0],\"vel_ned\":[13,0,-2.4],\"quat\":[0.9962,0,0.0872,0],\"euler_deg\":[0,10,0],"
        + "\"rpm\":6000,\"actuators\":{\"aileron_l\":0.05,\"aileron_r\":-0.05,\"elevator\":0.08,\"rudder\":0,\"motor\":0.6},\"on_ground\":false,"
        + "\"fc\":{\"msp\":true,\"armed\":true,\"mode\":\"ANGLE+NAV LAUNCH\",\"arming_flags\":[]},"
        + "\"plane\":{\"airspeed_mps\":13.9,\"alpha_deg\":2.1,\"beta_deg\":-0.4,\"thrust_n\":21.4,\"surfaces_deg\":{\"aileron_l\":10.0,\"aileron_r\":-10.0,\"elevator\":5.0,\"rudder\":-7.0},"
        + "\"launch\":\"ramp\",\"held\":true,\"posed\":false,\"scenario\":\"launch\",\"wind_ned\":[0,0,0]}}";

    public static string Event(string name, double t, string data) => $"{{\"type\":\"event\",\"t\":{t.ToString(System.Globalization.CultureInfo.InvariantCulture)},\"name\":\"{name}\",\"data\":{data}}}";

    /// The trainer's config sequence as the core sends it on a first start (core/qemu/evidence/plane: stage C).
    public static readonly (string Line, string Expect)[] ConfigSequence =
    {
        (Event("config", 7.938, "{\"state\":\"applying\",\"lines\":13}"), "applying the CLI script (13 lines)"),
        (Event("config", 9.268, "{\"state\":\"calibrating\",\"lines\":13}"), "pose 0/6"),
        (Event("pose", 14.791, "{\"label\":\"level\",\"index\":0,\"step\":1,\"scenario\":\"acc-calibration\"}"), "pose 1/6 (level)"),
        (Event("pose", 18.791, "{\"label\":\"inverted\",\"index\":1,\"step\":2,\"scenario\":\"acc-calibration\"}"), "pose 2/6 (inverted)"),
        (Event("pose", 22.791, "{\"label\":\"left side down\",\"index\":5,\"step\":3,\"scenario\":\"acc-calibration\"}"), "pose 3/6 (left side down)"),
        (Event("pose", 26.791, "{\"label\":\"right side down\",\"index\":4,\"step\":4,\"scenario\":\"acc-calibration\"}"), "pose 4/6 (right side down)"),
        (Event("pose", 30.791, "{\"label\":\"nose up\",\"index\":2,\"step\":5,\"scenario\":\"acc-calibration\"}"), "pose 5/6 (nose up)"),
        (Event("pose", 34.791, "{\"label\":\"nose down\",\"index\":3,\"step\":6,\"scenario\":\"acc-calibration\"}"), "pose 6/6 (nose down)"),
        (Event("pose", 38.791, "{\"label\":\"back to level\",\"step\":7,\"scenario\":\"acc-calibration\"}"), "pose 6/6 (nose down)"),
        (Event("scenario_end", 39.79, "{\"scenario\":\"acc-calibration\"}"), "pose 6/6 (nose down)"),
        (Event("config", 40.963, "{\"state\":\"applied\",\"lines\":13,\"acc_calibrated\":true}"), "config: applied · ACC calibrated"),
    };

    private static void Check(List<string> f, bool ok, string what) { if (!ok) f.Add(what); }

    private static (string Name, JsonElement Data, double T) Parse(string line, out JsonDocument doc)
    {
        doc = JsonDocument.Parse(line);
        var r = doc.RootElement;
        r.TryGetProperty("data", out var d);
        return (Json.Str(r, "name") ?? "", d, Json.Num(r, "t", double.NaN));
    }

    /// PlaneStatus alone: the state sample and every plane event.
    public static List<string> PlaneModel()
    {
        var f = new List<string>();
        var p = new PlaneStatus();
        var ro = new AircraftReadout(p);
        using (var doc = JsonDocument.Parse(StateSample)) { ro.ApplyState(doc.RootElement); Check(f, p.HasPlane, "state.plane not taken"); }
        var s = ro.Text();
        using (var doc = JsonDocument.Parse("{\"type\":\"state\",\"t\":1,\"rpm\":0}")) Check(f, !p.ApplyState(doc.RootElement) && p.Airspeed == 13.9, "a state without plane changed the model");
        foreach (var want in new[] { "airspeed 13.9 m/s (50 km/h)", "α +2.1°", "β -0.4°", "thrust 21.4 N", "ail L +10.0° R -10.0°", "elev +5.0°", "rud -7.0°", "launch: ramp", "held", "scenario launch" })
            Check(f, s.Contains(want), $"Aircraft rows lack '{want}': {s.Replace("\n", " | ")}");
        using (var doc = JsonDocument.Parse(StateSample)) ro.ApplyState(doc.RootElement);
        (string Src, string Text) Ev(string name, double t, string data)
        {
            var (n, d, tt) = Parse(Event(name, t, data), out var doc);
            using (doc) return p.ApplyEvent(n, d, tt);
        }
        var st = Ev("launch", 24.0, "{\"phase\":\"start\",\"a_mss\":27.78,\"speed_mps\":13.89,\"heading_deg\":0,\"pitch_deg\":10}");
        Check(f, st.Text.StartsWith("launch: ramp… 50 km/h at 27.8 m/s²") && p.LaunchEvent == "ramp…" && ro.Text().Contains("launch: ramp · ramp…"), $"launch start: '{st.Text}' / '{p.LaunchEvent}'");
        var rel = Ev("launch", 24.5, "{\"phase\":\"release\",\"speed_mps\":13.89,\"height_m\":0.61}");
        Check(f, rel.Text.StartsWith("launch: released at 13.9 m/s, 0.61 m") && ro.Text().Contains("released at 13.9 m/s"), $"launch release: '{rel.Text}'");
        Ev("pose", 1.0, "{\"label\":\"launch attitude\",\"step\":1,\"scenario\":\"launch\"}");
        Check(f, p.Step == "pose 'launch attitude' (step 1 of launch)" && ro.Text().Contains("step: pose 'launch attitude'"), $"step: '{p.Step}'");
        Ev("launch_command", 24.0, "{\"label\":\"\",\"step\":2,\"scenario\":\"launch\"}");
        Check(f, p.Step == "launch_command (step 2 of launch)", $"launch_command step: '{p.Step}'");
        Ev("scenario_end", 24.0, "{\"scenario\":\"launch\"}");
        Check(f, p.Step == "scenario 'launch' ended", $"scenario_end: '{p.Step}'");
        var mix = Ev("mix_warning", 30.0, "{\"left\":5.0,\"right\":5.0}");
        Check(f, mix.Text.Contains("both ailerons") && mix.Text.Contains("L +5.0°, R +5.0°") && ro.Text().Contains("mix warning"), $"mix_warning: '{mix.Text}'");
        var cr = Ev("crash", 42.0, "{\"roll_deg\":-35.2,\"pitch_deg\":-10.9,\"sink_mps\":2.6}");
        Check(f, p.Crash != null && cr.Text == "CRASH at t = 42.00 s: roll -35.2°, pitch -10.9°, sink 2.6 m/s", $"plane crash: '{cr.Text}'");
        var rs = Ev("reset", 43.0, "{\"what\":\"aircraft\",\"firmware\":false}");
        Check(f, p.Crash == null && p.MixWarning == "" && rs.Src == "event", "reset did not clear the crash / mix warning");
        var hc = Ev("crash", 12.0, "{\"roll_deg\":-68.8,\"pitch_deg\":11.8,\"speed_mps\":4.40}");
        Check(f, hc.Text == "CRASH at t = 12.00 s: roll -68.8°, pitch +11.8°, speed 4.4 m/s", $"heli crash: '{hc.Text}'");
        p.ClearCrash();
        Check(f, p.Crash == null, "ClearCrash");
        var other = Ev("failsafe", 34.84, "{\"on\":true,\"rxloss\":false}");
        Check(f, other.Text == "failsafe {\"on\":true,\"rxloss\":false} (t = 34.84 s)", $"other event: '{other.Text}'");
        // beep on/off every 0.1 s for 2.5 s: the first line of each 1 s window + one count line when it closes
        var ec = new EventCollapse();
        var beepLog = new List<string>();
        for (int i = 0; i < 25; i++)
        {
            ulong now = 5000 + (ulong)i * 100;
            if (ec.Flush(now) is { } fl) beepLog.Add(fl);
            beepLog.AddRange(ec.Take("beep", $"beep {i}", 50 + i * 0.1, now));
        }
        if (ec.Flush(9000) is { } last) beepLog.Add(last);
        Check(f, EventCollapse.Collapses("beep") && EventCollapse.Collapses("motor_beep") && !EventCollapse.Collapses("crash"), "EventCollapse names");
        Check(f, beepLog.Count == 6 && beepLog[0] == "beep 0" && beepLog[1] == "beep ×9 more (t = 50.10..50.90 s, collapsed)" && beepLog[2] == "beep 10"
                 && beepLog[5] == "beep ×4 more (t = 52.10..52.40 s, collapsed)" && ec.Flush(10000) == null,
              $"beep collapse: {string.Join(" | ", beepLog)}");
        return f;
    }

    /// ConfigProgress alone: the first-start sequence, the firmware-wait lines, a failure.
    public static List<string> ConfigModel()
    {
        var f = new List<string>();
        var p = new PlaneStatus();
        var c = p.Config;
        Check(f, c.WaitLine(false).StartsWith("firmware: waiting for it to answer") && c.WaitLine(null) == "" && c.WaitLine(true) == "" && c.StatusLine(0, 0) == "", $"before any config: '{c.WaitLine(false)}'");
        var logs = new List<string>();
        foreach (var (line, expect) in ConfigSequence)
        {
            var (n, d, t) = Parse(line, out var doc);
            using (doc) { var (src, text) = p.ApplyEvent(n, d, t); logs.Add(src + " " + text); }
            Check(f, c.Line.Contains(expect), $"after {n} t={t}: line '{c.Line}' (want '{expect}')");
            if (c.Active) Check(f, c.StatusLine(0, 0) == c.Line && c.WaitLine(false).StartsWith("firmware: busy with its first start") && c.WaitLine(true) == "", "while configuring: the Status config line is the progress line, the firmware line says it is busy");
            Check(f, !c.Line.Contains("error", StringComparison.OrdinalIgnoreCase) && !c.Line.Contains("timed out", StringComparison.OrdinalIgnoreCase), $"an error in the progress line: '{c.Line}'");
        }
        Check(f, logs.Count(l => l.StartsWith("config: ")) == 9, $"config: log lines {logs.Count(l => l.StartsWith("config: "))} (want 3 config + 6 poses): {string.Join(" | ", logs)}");
        Check(f, logs[4] == "config: ACC calibration pose 3/6: left side down (iNav position 5) (t = 22.79 s)", $"pose log line: '{logs[4]}'");
        Check(f, !c.Active && c.AccCalibrated && c.Poses.Count == 6, $"after applied: active {c.Active}, acc {c.AccCalibrated}, poses {c.Poses.Count}");
        Check(f, c.WaitLine(false) == "firmware: rebooting after the config save…" && c.StatusLine(100, 200) == "config: applied · ACC calibrated" && c.StatusLine(300, 200) == "", $"after applied: '{c.WaitLine(false)}' / '{c.StatusLine(100, 200)}' (shown until it hides, then gone)");
        // a pose outside the calibration (the launch attitude) does not count
        var (pn, pd, pt) = Parse(Event("pose", 41.0, "{\"label\":\"launch attitude\",\"index\":0,\"step\":1,\"scenario\":\"launch\"}"), out var pdoc);
        using (pdoc) p.ApplyEvent(pn, pd, pt);
        Check(f, c.Poses.Count == 6 && c.Line == "config: applied · ACC calibrated", "a pose after the calibration changed the progress");
        // later start: restored; a failure names the line
        var q = new ConfigProgress();
        using (var d1 = JsonDocument.Parse("{\"state\":\"restored\",\"lines\":13}")) q.OnConfig(d1.RootElement, 9.1);
        Check(f, q.Line == "config: restored (saved snapshot)" && !q.Active, $"restored: '{q.Line}'");
        using (var d2 = JsonDocument.Parse("{\"state\":\"failed\",\"line\":4,\"text\":\"set foo = 1\",\"error\":\"### ERROR: INVALID NAME\"}")) q.OnConfig(d2.RootElement, 9.5);
        Check(f, q.Line == "config: FAILED — line 4: 'set foo = 1': ### ERROR: INVALID NAME (the firmware runs its defaults)" && q.StatusLine(99999, 0) == q.Line, $"failed (stays): '{q.Line}'");
        return f;
    }
}

public partial class Flight
{
    /// Set by the self-test runner before it opens the screen for the plane tests.
    public static PlaneTestMode PlaneTest = PlaneTestMode.None;
    private bool _planeTestDone;
    private int _ptFrame;
    private ulong _ptWaitUntil;
    private readonly List<string> _ptLaunchLines = new();
    private readonly List<string> _ptConfigFails = new(), _ptStateFails = new(), _ptVisualFails = new(), _ptLaunchFails = new();
    private int _ptConfigIndex;
    private bool _ptHooked;
    private string _ptLaunchKey = "";
    private Dictionary<string, string>? _ptKeysBackup;

    private void PtInject(string line) => _core.InjectForTest(line);

    private static InputEventKey PtKey(string bind)
    {
        var code = OS.FindKeycodeFromString(bind);
        long c = (long)code, m = (long)KeyModifierMask.CodeMask;
        var key = (Key)(c & m);
        var k = new InputEventKey { Keycode = key, PhysicalKeycode = key, Pressed = true };
        k.CtrlPressed = (c & (long)KeyModifierMask.MaskCtrl) != 0; k.ShiftPressed = (c & (long)KeyModifierMask.MaskShift) != 0; k.AltPressed = (c & (long)KeyModifierMask.MaskAlt) != 0;
        return k;
    }

    private void PlaneSelfTest()
    {
        const string P = "[plane-selftest] ";
        if (_ptFrame == 0)
        {
            if (_ptWaitUntil == 0) _ptWaitUntil = Time.GetTicksMsec() + 5000;
            if (!_session.Connected && Time.GetTicksMsec() < _ptWaitUntil) return;
            if (!_ptHooked)
            {
                _ptHooked = true;
                _core.Sending += line => { if (line.Contains("\"type\":\"launch\"")) _ptLaunchLines.Add(line); };
                // the test's binds: launch / switches on their defaults for this run (restored at the end; settings not saved)
                _ptKeysBackup = new Dictionary<string, string>(AppSettings.Current.Keys);
                foreach (var id in new[] { "launch", "switch_1", "switch_2", "switch_3", "switch_arm", "switch_mode", "switch_aux" }) AppSettings.Current.Keys.Remove(id);
                _ptLaunchKey = Keybinds.Bind("launch");
                GD.Print(P + $"{(PlaneTest == PlaneTestMode.Plane ? "trainer" : "heli")} bench '{_bench.Name}' ({_bench.Aircraft}, plane {_isPlane}), connected {_session.Connected}, launch key '{_ptLaunchKey}'");
            }
        }
        if (PlaneTest == PlaneTestMode.Heli) { HeliLaunchTest(P); return; }
        void Check(List<string> f, bool ok, string what) { if (!ok) f.Add(what); }
        var tv = _aircraft?.Visual as TrainerVisual;
        switch (++_ptFrame)
        {
            case 1:
            {
                // plane_visual: the scene, its pivots, a state applied
                Check(_ptVisualFails, _isPlane, $"'{_bench.Aircraft}' not recognised as a plane (fdm '{AircraftInfo.Fdm(_bench.Aircraft)}')");
                Check(_ptVisualFails, _aircraft != null && _aircraft.Source.Replace('\\', '/').EndsWith("models/trainer/visual/trainer.tscn"), $"visual source '{_aircraft?.Source}'");
                Check(_ptVisualFails, tv != null, $"visual class {_aircraft?.Visual?.GetType().FullName ?? "none"} (want FlightBench.Models.TrainerVisual)");
                Check(_ptVisualFails, _modelAircraft?.Visual is TrainerVisual, "the Model window's aircraft is not the trainer visual");
                if (tv != null)
                {
                    foreach (var n in TrainerVisual.Surfaces.Append("prop"))
                        Check(_ptVisualFails, tv.Pivot(n) is { } pv && pv.Name == n && pv.GetParent() != null && pv.GetChildCount() > 0, $"pivot '{n}' not resolved / empty");
                    Check(_ptVisualFails, Mathf.IsEqualApprox(_aircraft!.CgHeight, 0.1f) && Mathf.IsEqualApprox(_aircraft.ViewSize, 1.88f), $"cg_height {_aircraft.CgHeight} / view_size {_aircraft.ViewSize}");
                }
                PtInject(PlaneTests.StateSample);
                if (tv != null)
                {
                    // trailing edge = the pivot's local +z (the surface runs back from the hinge)
                    Vector3 Te(string n) => tv.Pivot(n)!.Basis * new Vector3(0, 0, 0.06f);
                    Check(_ptVisualFails, Mathf.IsEqualApprox(tv.Pivot("aileron_l")!.RotationDegrees.X, 10f, 0.01f) && Te("aileron_l").Y < -0.005f, $"aileron_l +10° (TE down): rot {tv.Pivot("aileron_l")!.RotationDegrees}, TE {Te("aileron_l")}");
                    Check(_ptVisualFails, Mathf.IsEqualApprox(tv.Pivot("aileron_r")!.RotationDegrees.X, -10f, 0.01f) && Te("aileron_r").Y > 0.005f, $"aileron_r -10° (TE up): rot {tv.Pivot("aileron_r")!.RotationDegrees}, TE {Te("aileron_r")}");
                    Check(_ptVisualFails, Mathf.IsEqualApprox(tv.Pivot("elevator")!.RotationDegrees.X, -5f, 0.01f) && Te("elevator").Y > 0.002f, $"elevator +5° (TE up): rot {tv.Pivot("elevator")!.RotationDegrees}, TE {Te("elevator")}");
                    Check(_ptVisualFails, Mathf.IsEqualApprox(tv.Pivot("rudder")!.RotationDegrees.Y, -7f, 0.01f) && Te("rudder").X < -0.003f, $"rudder -7° (TE left): rot {tv.Pivot("rudder")!.RotationDegrees}, TE {Te("rudder")}");
                    Check(_ptVisualFails, tv.Deflection("aileron_l") == 10 && tv.Deflection("rudder") == -7, "deflections not stored");
                    _ptProp0 = tv.PropAngle;
                }
                // plane_state on the screen: the Aircraft rows follow the injected state (UpdateStatus runs on every 10th state: force it)
                UpdateStatus();
                var ap = _aircraftPanel!;
                string Rows() => string.Join(" | ", ap.VisibleRows.Select(ap.RowText));
                Check(_ptStateFails, ap.RowText("air").Contains("airspeed 13.9 m/s (50 km/h)") && ap.RowText("surfaces").Contains("ail L +10.0° R -10.0°") && ap.RowText("launch").StartsWith("ramp"),
                      $"Aircraft rows: '{Rows()}'");
                PtInject(PlaneTests.Event("launch", 24.0, "{\"phase\":\"start\",\"a_mss\":27.78,\"speed_mps\":13.89,\"heading_deg\":0,\"pitch_deg\":10}"));
                PtInject(PlaneTests.Event("launch", 24.5, "{\"phase\":\"release\",\"speed_mps\":13.89,\"height_m\":0.61}"));
                Check(_ptStateFails, ap.RowText("launch").Contains("released at 13.9 m/s, 0.61 m"), $"launch events not in the Aircraft window: '{Rows()}'");
                Check(_ptStateFails, LogHas("launch: ramp…") && LogHas("launch: released at 13.9 m/s"), "launch events not logged");
                PtInject(PlaneTests.Event("crash", 42.0, "{\"roll_deg\":-35.2,\"pitch_deg\":-10.9,\"sink_mps\":2.6}"));
                Check(_ptStateFails, ap.CrashVisible && ap.CrashText.StartsWith("CRASH at t = 42.00 s") && _windows.IsOpen("Aircraft"), $"crash banner: visible {ap.CrashVisible}, '{ap.CrashText}', Aircraft open {_windows.IsOpen("Aircraft")}");
                ap.PressReset();                                // the banner's button: sends reset (connected) and clears the banner
                Check(_ptStateFails, !ap.CrashVisible, "Reset aircraft did not clear the crash banner");
                WindowsTests.PlaneRan = true;
                WindowsTests.PlaneFails.AddRange(_ptStateFails.Select(x => "plane screen: " + x));
                foreach (var id in new[] { "alt", "speed", "air", "power", "surfaces", "launch" })
                    if (!ap.VisibleRows.Contains(id)) WindowsTests.PlaneFails.Add($"plane screen: row '{id}' hidden ({Rows()})");
                break;
            }
            case 20:
                if (tv != null) Check(_ptVisualFails, tv.Rpm > 0 && Math.Abs(tv.PropAngle - _ptProp0) > 1e-3, $"prop not spinning: rpm {tv.Rpm}, angle {_ptProp0} → {tv.PropAngle}");
                // config_progress on the screen: the sequence with pauses (host time passes, nothing times out)
                _ptConfigIndex = 0; _ptNextFrame = 21; _ptConfigStart = Time.GetTicksMsec();
                break;
            case > 20 and < 1000 when _ptConfigIndex < PlaneTests.ConfigSequence.Length:
            {
                if (_ptFrame < _ptNextFrame || Time.GetTicksMsec() < _ptNextMs) break;   // one event every 25 frames, a 3 s pause mid-calibration
                var (line, expect) = PlaneTests.ConfigSequence[_ptConfigIndex++];
                PtInject(line);
                UpdateStatus();
                var cfgLine = _statusPanel!.Config;
                Check(_ptConfigFails, cfgLine.Visible && cfgLine.Text.Contains(expect), $"step {_ptConfigIndex}: Status config line '{cfgLine.Text}' (visible {cfgLine.Visible}, want '{expect}')");
                Check(_ptConfigFails, _session.Connected, $"step {_ptConfigIndex}: disconnected");
                Check(_ptConfigFails, !_statusPanel.Link.Text.Contains("error", StringComparison.OrdinalIgnoreCase) && !_coreState.Contains("error", StringComparison.OrdinalIgnoreCase), $"step {_ptConfigIndex}: an error in Status: '{_statusPanel.Link.Text}' / '{_coreState}'");
                _ptNextFrame = _ptFrame + (_ptConfigIndex == 5 ? 200 : 25);
                if (_ptConfigIndex == 5) _ptNextMs = Time.GetTicksMsec() + 3000;
                if (_ptConfigIndex == PlaneTests.ConfigSequence.Length) _ptFrame = 399;
                break;
            }
            case 400:
            {
                Check(_ptConfigFails, LogHas("config: applying the board's CLI script (13 lines)") && LogHas("config: ACC calibration pose 6/6: nose down") && LogHas("config: applied, ACC calibrated"), "config: lines missing from the log");
                Check(_ptConfigFails, _statusPanel!.Config.Visible && _statusPanel.Config.Text == "config: applied · ACC calibrated", $"final Status config line '{_statusPanel.Config.Text}' (visible {_statusPanel.Config.Visible})");
                // launch_cmd, the plane part: button and key send exactly {"type":"launch"}
                Check(_ptLaunchFails, _launchBtn is { Visible: true, Disabled: false }, $"Launch button on a connected plane bench: visible {_launchBtn?.Visible}, disabled {_launchBtn?.Disabled}");
                Check(_ptLaunchFails, _launchBtn?.Text == "Launch" + Keybinds.Suffix("launch"), $"button text '{_launchBtn?.Text}'");
                _launchBtn?.EmitSignal(BaseButton.SignalName.Pressed);
                if (_ptLaunchKey != "") GetTree().Root.PushInput(PtKey(_ptLaunchKey));
                else _ptLaunchFails.Add("the launch function has no key");
                break;
            }
            case 406:
            {
                Check(_ptLaunchFails, _ptLaunchLines.Count == 2 && _ptLaunchLines.All(l => l == "{\"type\":\"launch\"}"), $"sent: {string.Join(" | ", _ptLaunchLines)} (want {{\"type\":\"launch\"}} twice: button, key '{_ptLaunchKey}')");
                _launchOverride!.ButtonPressed = true; _launchSpeed!.Value = 60; _launchDuration!.Value = 0.5; _launchPitch!.Value = 12;
                var ov = JsonSerializer.Serialize(LaunchMessage());
                Check(_ptLaunchFails, ov == "{\"type\":\"launch\",\"speed_kmh\":60,\"duration_s\":0.5,\"pitch_deg\":12}", $"override message {ov}");
                _launchOverride.ButtonPressed = false;
                // the trainer's channels are all `auto` (the preset's old `switches` names kept as labels) and this screen ignores
                // the modes: no Transmitter row, and a switch key sends nothing (no toggle of the app's own: decisions 2026-10-23)
                Check(_ptLaunchFails, _switches.Count == 0 && _switchRows.Count == 0, $"Transmitter rows without modes on an all-auto bench: {_switches.Count}");
                Check(_ptLaunchFails, TxModel.Entry(_bench.Transmitter, 7)?.Label == "NAV LAUNCH" && TxModel.Entry(_bench.Transmitter, 7)?.IsStatic == false, "the preset's old switch names → auto labels: " + string.Join(", ", (_bench.Transmitter ?? new()).Select(kv => $"ch{kv.Key} {kv.Value.Type} {kv.Value.Label}")));
                _ptCh7Before = _rc.ChannelsUs[6];
                GetTree().Root.PushInput(PtKey(Keybinds.Bind("switch_3")));
                break;
            }
            case 410:
            {
                Check(_ptLaunchFails, _rc.ChannelsUs[6] == _ptCh7Before && _rc.Override[6] == 0 && LogHas("switch_3: no row 3"), $"switch_3 without a row: ch7 {_ptCh7Before} → {_rc.ChannelsUs[6]}, override {_rc.Override[6]}, logged {LogHas("switch_3: no row 3")}");
                var sf = _ptStateFails.Concat(PlaneTests.PlaneModel()).ToList();
                if (DevEnv.SelfTest("plane_state")) SelfTests.Report("plane_state", sf.Count == 0, sf.Count == 0 ? "PlaneStatus + AircraftReadout: state.plane sample -> airspeed/alpha/beta/thrust/surfaces/launch/hold/scenario rows; launch start/release, step events, scenario_end, mix_warning, plane + heli crash, reset, other events logged; on the screen: Aircraft rows, launch events in the Aircraft window + Log, crash banner, Reset aircraft clears it" : string.Join("; ", sf));
                var cf = PlaneTests.ConfigModel().Concat(_ptConfigFails).ToList();
                if (DevEnv.SelfTest("config_progress")) SelfTests.Report("config_progress", cf.Count == 0, cf.Count == 0 ? $"ConfigProgress: waiting -> applying -> calibrating pose 1..6/6 -> applied, ACC calibrated (9 config: log lines; shown, then hidden), restored, failed with the line (stays); screen: {PlaneTests.ConfigSequence.Length} events over {((Time.GetTicksMsec() - _ptConfigStart) / 1000.0).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture)} s incl. a pause mid-calibration, Status follows, still connected, no error" : string.Join("; ", cf));
                if (DevEnv.SelfTest("plane_visual")) SelfTests.Report("plane_visual", _ptVisualFails.Count == 0, _ptVisualFails.Count == 0 ? "trainer.tscn + TrainerVisual (main view and Model window), pivots aileron_l/aileron_r/elevator/rudder/prop, surfaces_deg applied (ailerons ±10° TE down/up, elevator +5° TE up, rudder −7° TE left), cg_height 0.1, view_size 1.88, prop spinning" : string.Join("; ", _ptVisualFails));
                PlaneTests.LaunchFails = new List<string>(_ptLaunchFails);
                PlaneTests.LaunchDetail = $"plane: button + key '{_ptLaunchKey}' sent {{\"type\":\"launch\"}} twice, override adds speed/duration/pitch, all-auto bench without modes: no Transmitter row, switch_3 sends nothing";
                if (_ptKeysBackup != null) AppSettings.Current.Keys = _ptKeysBackup;
                _planeTestDone = true;
                break;
            }
        }
    }
    private double _ptProp0;
    private int _ptNextFrame;
    private ulong _ptNextMs;
    private ulong _ptConfigStart;
    private int _ptCh7Before;

    /// launch_cmd, the heli part: no Launch button (hidden and disabled), the launch key sends nothing.
    private void HeliLaunchTest(string P)
    {
        switch (++_ptFrame)
        {
            case 1:
                if (_ptLaunchKey != "") GetTree().Root.PushInput(PtKey(_ptLaunchKey));
                Run("launch");
                break;
            case 6:
            {
                var f = PlaneTests.LaunchFails ?? new List<string> { "the plane part did not run" };
                if (_isPlane) f.Add($"'{_bench.Aircraft}' taken for a plane");
                if (_launchBtn is not { Visible: false, Disabled: true }) f.Add($"heli bench: Launch button visible {_launchBtn?.Visible}, disabled {_launchBtn?.Disabled} (want hidden + disabled)");
                if (_ptLaunchLines.Count != 0) f.Add("heli bench: launch sent " + string.Join(" | ", _ptLaunchLines));
                if (!LogHas("is not a plane")) f.Add("heli bench: the refused launch was not logged");
                if (DevEnv.SelfTest("launch_cmd")) SelfTests.Report("launch_cmd", f.Count == 0, f.Count == 0 ? PlaneTests.LaunchDetail + $"; heli ({_bench.Aircraft}): button hidden + disabled, key and Run(launch) send nothing (logged)" : string.Join("; ", f));
                if (_ptKeysBackup != null) AppSettings.Current.Keys = _ptKeysBackup;
                _planeTestDone = true;
                PlaneTests.HeliDone = true;
                break;
            }
        }
    }

    private bool LogHas(string text) => _log != null && _log.GetParsedText().Contains(text);
}
