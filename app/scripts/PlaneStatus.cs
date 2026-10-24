// Models the self-tests can feed without a screen:
// - PlaneStatus: `state.plane` (docs/protocol.md: airspeed, α/β, thrust, surfaces in degrees, launch phase,
//   hold, scenario) and the plane's events (`launch` start/release, scenario step events, `scenario_end`,
//   `mix_warning`, `crash`), each event also turned into one log line; the Aircraft window draws them
//   (AircraftPanel.cs);
// - ConfigProgress: the core's first-start `config` events (a CLI-configured board: `applying` → `calibrating`
//   {function} (the first-start functions, e.g. iNav's 6-position ACC calibration: `function` messages step k/6, and a
//   `pose` event with an iNav `index` per position) → `applied` {functions, acc_calibrated} / `restored` / `failed`
//   {error, function}; a late client gets the last one again with `replay`) for the Status window's progress line, and
//   the firmware-wait text ("waiting for the firmware" while `fc.msp` is false).
//   There is no timeout here: the first start of a new firmware + script runs ~40 virtual s at the boot's
//   ~0.11x (about 4 host minutes); the line says what the core is doing instead of an error.
// CrashInfo is shared with the heli: every FDM's `crash` carries speed_mps since 2026-10-24 (sink_mps of older cores still read).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;

namespace FlightBench;

/// Numbers in the Status / log lines with a dot whatever the OS locale: `using var inv = new InvariantText();`.
public readonly struct InvariantText : IDisposable
{
    private readonly CultureInfo _old;
    public InvariantText() { _old = CultureInfo.CurrentCulture; CultureInfo.CurrentCulture = CultureInfo.InvariantCulture; }
    public void Dispose() => CultureInfo.CurrentCulture = _old;
}

public sealed record CrashInfo(double T, double Roll, double Pitch, double? Sink, double? Speed)
{
    public string Text { get { using var inv = new InvariantText(); return TextNow(); } }
    private string TextNow() => $"CRASH at t = {T:0.00} s: roll {Roll:+0.0;-0.0;0.0}°, pitch {Pitch:+0.0;-0.0;0.0}°"
                          + (Sink is { } s ? $", sink {s:0.0} m/s" : "") + (Speed is { } v ? $", speed {v:0.0} m/s" : "");
}

public sealed class ConfigProgress
{
    public const int Positions = 6;                        // iNav's accelerometer calibration positions
    public string State { get; private set; } = "";        // "" (none seen) | applying | calibrating | applied | restored | failed
    public int Lines { get; private set; }
    public bool AccCalibrated { get; private set; }
    public string FailText { get; private set; } = "";
    /// iNav positions the calibration reached so far (the `pose` events with an `index` while calibrating).
    public readonly List<int> Poses = new();
    public string LastPose { get; private set; } = "";
    public double LastT { get; private set; }
    /// The first-start function running (`calibrating` {function}), else "".
    public string Function { get; private set; } = "";
    /// Its progress from the core's `function` messages (k of n; 0 = none yet).
    public int FunctionStep { get; private set; }
    public int FunctionSteps { get; private set; }
    /// The first-start functions `applied` names (`functions`).
    public List<string> Functions { get; private set; } = new();
    /// The last `config` event was a replay (sent to this client because it connected after it happened).
    public bool Replay { get; private set; }
    /// Bench state (core branch bench-state): `applying` with `delta:true` = only the changed / added lines of the script
    /// on top of the bench's restored config; `restored` with `source` "bench" (the bench's own saved config) or "snapshot"
    /// (the first-start snapshot of the cfg-cache).
    public bool Delta { get; private set; }
    public string Source { get; private set; } = "";
    /// Lines no longer in the bench's script (`removed`): reported, not reverted (the configurator may have set them).
    public List<string> Removed { get; private set; } = new();

    /// The core is still configuring (the firmware is not flyable yet).
    public bool Active => State is "applying" or "calibrating";

    /// A `config` event. Returns the log line (without the `config:` prefix).
    public string OnConfig(JsonElement data, double t)
    {
        using var inv = new InvariantText();
        // `removed` alone (no state): the lines the bench no longer has, reported while the state stays
        var removed = data.TryGetProperty("removed", out var rm) && rm.ValueKind == JsonValueKind.Array
            ? rm.EnumerateArray().Select(x => x.ValueKind == JsonValueKind.String ? x.GetString() ?? "" : x.GetRawText()).Where(x => x != "").ToList() : null;
        if (Json.Str(data, "state") is not { } st && removed != null)
        {
            Removed = removed;
            return RemovedText(t);
        }
        State = Json.Str(data, "state") ?? "?";
        LastT = t;
        Replay = Json.TryBool(data, "replay", out var rp) && rp;
        if (Json.TryNum(data, "lines", out var n)) Lines = (int)n;
        else if (data.TryGetProperty("lines", out var la) && la.ValueKind == JsonValueKind.Array) Lines = la.GetArrayLength();   // config-layers: the lines with provenance
        Delta = Json.TryBool(data, "delta", out var dl) && dl;
        Source = Json.Str(data, "source") ?? "";
        if (State == "applying") Removed = new();
        if (removed != null) Removed = removed;
        switch (State)
        {
            case "applying": Poses.Clear(); LastPose = ""; AccCalibrated = false; FailText = ""; Function = ""; FunctionStep = FunctionSteps = 0; Functions = new(); break;
            case "calibrating": Poses.Clear(); LastPose = ""; Function = Json.Str(data, "function") ?? ""; FunctionStep = FunctionSteps = 0; break;
            case "applied":
                AccCalibrated = Json.TryBool(data, "acc_calibrated", out var ac) && ac;
                Functions = data.TryGetProperty("functions", out var fs) && fs.ValueKind == JsonValueKind.Array
                    ? fs.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.String).Select(x => x.GetString()!).ToList() : new();
                break;
            case "restored": AccCalibrated = false; break;
            case "failed":
                var parts = new List<string>();
                if (Json.Str(data, "function") is { Length: > 0 } fn) parts.Add($"function {fn}");
                if (Json.TryNum(data, "line", out var ln)) parts.Add($"line {ln:0}");
                if (Json.Str(data, "text") is { Length: > 0 } tx) parts.Add($"'{tx}'");
                if (Json.Str(data, "error") is { Length: > 0 } er) parts.Add(er);
                FailText = string.Join(": ", parts);
                break;
        }
        return (Replay ? "(replay: happened before this client connected) " : "") + State switch
        {
            "applying" when Delta => $"applying {Lines} new line{(Lines == 1 ? "" : "s")} of the bench's script (delta) on top of its restored config, then save (t = {t:0.00} s)"
                                     + (Removed.Count > 0 ? "; " + RemovedText(t) : ""),
            "applying" => $"applying the board's CLI script ({Lines} lines), then save (t = {t:0.00} s)",
            "calibrating" => $"script saved; ACC calibration: {Positions} poses, each held still while the firmware samples it (t = {t:0.00} s)",
            "applied" => $"applied{(Functions.Count > 0 ? $" with {string.Join(", ", Functions)}" : "")}{(AccCalibrated ? ", ACC calibrated" : "")}: the config sector is saved and kept as the snapshot (later starts restore it) (t = {t:0.00} s)",
            "restored" when Source == "bench" => $"restored this bench's saved config (calibrations, modes and settings as last saved; no script, no calibration) (t = {t:0.00} s)",
            "restored" => $"restored the saved config snapshot (no script, no calibration) (t = {t:0.00} s)",
            "failed" => $"FAILED{(FailText != "" ? " — " + FailText : "")}: the firmware runs its defaults (t = {t:0.00} s)",
            _ => $"{State} {data.GetRawText()} (t = {t:0.00} s)",
        };
    }

    private string RemovedText(double t) =>
        $"{Removed.Count} line{(Removed.Count == 1 ? "" : "s")} no longer in the bench's script, left in the firmware (not reverted): {string.Join(" | ", Removed)} (t = {t:0.00} s)";

    /// A `pose` (or a scenario step) event while calibrating: the position with its iNav index counts. Returns the log line or null.
    public string? OnPose(JsonElement data, double t)
    {
        using var inv = new InvariantText();
        if (State != "calibrating" || !Json.TryNum(data, "index", out var idx)) return null;
        int i = (int)idx;
        if (!Poses.Contains(i)) Poses.Add(i);
        LastPose = Json.Str(data, "label") ?? $"position {i}";
        LastT = t;
        return $"ACC calibration pose {Poses.Count}/{Positions}: {LastPose} (iNav position {i}) (t = {t:0.00} s)";
    }

    /// A `function` message while `calibrating`: the first-start function's step k/n (and its detail as the pose label).
    public void OnFunction(JsonElement m)
    {
        if (State != "calibrating" || m.ValueKind != JsonValueKind.Object) return;
        var name = Json.Str(m, "name") ?? "";
        if (Function != "" && name != Function) return;
        if (Function == "") Function = name;
        if (Json.TryNum(m, "step", out var k)) FunctionStep = (int)k;
        if (Json.TryNum(m, "steps", out var n)) FunctionSteps = (int)n;
        if (Json.Str(m, "phase") == "step" && Json.Str(m, "detail") is { Length: > 0 } d)
        {
            var label = d.Replace(" collected", "");
            int paren = label.IndexOf(" (iNav position", StringComparison.Ordinal);
            LastPose = paren > 0 ? label[..paren] : label;
        }
        LastT = Json.Num(m, "t", LastT);
    }

    /// Calibration progress k of n: the function's steps, or the `pose` events counted (whichever is further).
    public int Step => Math.Max(Poses.Count, FunctionStep);
    public int Steps => FunctionSteps > 0 ? FunctionSteps : Positions;

    /// The Status window's progress line ("" when nothing to say).
    public string Line { get { using var inv = new InvariantText(); return LineNow(); } }
    private string LineNow() => State switch
    {
        "applying" when Delta => $"config: applying {Lines} new line{(Lines == 1 ? "" : "s")} (delta)…",
        "applying" => $"config: applying the CLI script ({Lines} lines)… first start: configuring the board",
        "calibrating" when Function is "" or "calibrate_acc" =>
            Step == 0 ? $"config: calibrating the accelerometer… pose 0/{Steps} (first start, ~4 min at this speed)"
                      : $"config: calibrating the accelerometer… pose {Step}/{Steps}{(LastPose != "" ? $" ({LastPose})" : "")}",
        "calibrating" => $"config: first start: {Function}… step {Step}/{Steps}{(LastPose != "" ? $" ({LastPose})" : "")}",
        "applied" => $"config: applied{(AccCalibrated ? " · ACC calibrated" : "")}",
        "restored" when Source == "bench" => "config: restored (bench state)",
        "restored" => "config: restored (saved snapshot)",
        "failed" => $"config: FAILED{(FailText != "" ? " — " + FailText : "")} (the firmware runs its defaults)",
        _ => "",
    };

    /// The Status window's firmware line while the firmware does not answer MSP (fcMsp false): why; "" otherwise.
    public string WaitLine(bool? fcMsp)
    {
        if (fcMsp != false) return "";
        if (Active) return Delta ? "firmware: busy with the bench's new config lines (the config line below)" : "firmware: busy with its first start (the config line below)";
        return State == "" ? "firmware: waiting for it to answer (booting; a first start configures and calibrates the board: can take minutes)"
                           : "firmware: rebooting after the config save…";
    }

    /// The progress line the Status window shows: while the config runs, `applied` / `restored` until `hideAtMs`
    /// (a few seconds after they came), `failed` until the next connection; "" otherwise.
    public string StatusLine(ulong nowMs, ulong hideAtMs) =>
        Active || State == "failed" || (State is "applied" or "restored" && nowMs < hideAtMs) ? Line : "";

    public void Reset() { State = ""; Lines = 0; AccCalibrated = false; FailText = ""; Poses.Clear(); LastPose = ""; Function = ""; FunctionStep = FunctionSteps = 0; Functions = new(); Replay = false; Delta = false; Source = ""; Removed = new(); }
}

/// Repeated noisy events (`beep` on/off, `motor_beep`: the beeper toggles every few hundred ms while armed / on a
/// beacon) collapsed in the Log: the first of a burst is logged, the rest of each 1 s window become one line
/// "beep ×N in 1.0 s (…)" when the window closes (Take on the next event or Flush on a frame).
public sealed class EventCollapse
{
    public static readonly string[] Names = { "beep", "motor_beep" };
    public const ulong WindowMs = 1000;
    private string? _name;                 // the window's event name (null: no window open)
    private ulong _start;
    private int _more;                     // suppressed in the window
    private double _t0, _t1;               // their core time span

    public static bool Collapses(string name) => Array.IndexOf(Names, name) >= 0;

    /// A collapsible event at host time `nowMs`: (lines to log now: a closed window's summary and/or this event's line, or none).
    public List<string> Take(string name, string line, double t, ulong nowMs)
    {
        var outp = new List<string>();
        if (_name != null && (_name != name || nowMs - _start >= WindowMs)) { if (Summary() is { } s) outp.Add(s); _name = null; }
        if (_name == null) { _name = name; _start = nowMs; _more = 0; outp.Add(line); return outp; }
        if (_more == 0) _t0 = t;
        _more++; _t1 = t;
        return outp;
    }

    /// Per frame: the window's summary once it is over (null: nothing to say).
    public string? Flush(ulong nowMs)
    {
        if (_name == null || nowMs - _start < WindowMs) return null;
        var s = Summary();
        _name = null;
        return s;
    }

    private string? Summary()
    {
        if (_more == 0) return null;
        using var inv = new InvariantText();
        return double.IsNaN(_t0) || double.IsNaN(_t1) ? $"{_name} ×{_more} more (collapsed)"
                                                      : $"{_name} ×{_more} more (t = {_t0:0.00}..{_t1:0.00} s, collapsed)";
    }
}

public sealed class PlaneStatus
{
    public static readonly string[] Surfaces = { "aileron_l", "aileron_r", "elevator", "rudder" };
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    public bool HasPlane { get; private set; }
    public double Airspeed, Alpha, Beta, Thrust;
    public readonly Dictionary<string, double> SurfacesDeg = new();
    public string LaunchPhase = "none";                  // state.plane.launch: none | align | ramp
    public bool Held, Posed;
    public string? Scenario;                             // the running env.scenario (null: none / ended)
    public double[] Wind = new double[3];
    /// The last scenario step event: "pose 'launch attitude' (step 1 of launch)".
    public string Step { get; private set; } = "";
    /// The launch as its events tell it: "" | "ramp…" | "released …".
    public string LaunchEvent { get; private set; } = "";
    public CrashInfo? Crash { get; private set; }
    public string MixWarning { get; private set; } = "";
    public readonly ConfigProgress Config = new();

    /// Copies `state.plane` (the JsonElement does not outlive the message). False when the state has none.
    public bool ApplyState(JsonElement s)
    {
        if (!s.TryGetProperty("plane", out var p) || p.ValueKind != JsonValueKind.Object) return false;
        HasPlane = true;
        Airspeed = Json.Num(p, "airspeed_mps", Airspeed);
        Alpha = Json.Num(p, "alpha_deg", Alpha);
        Beta = Json.Num(p, "beta_deg", Beta);
        Thrust = Json.Num(p, "thrust_n", Thrust);
        if (p.TryGetProperty("surfaces_deg", out var sd) && sd.ValueKind == JsonValueKind.Object)
            foreach (var n in Surfaces) if (sd.TryGetProperty(n, out var v) && v.ValueKind == JsonValueKind.Number) SurfacesDeg[n] = v.GetDouble();
        LaunchPhase = Json.Str(p, "launch") ?? LaunchPhase;
        if (Json.TryBool(p, "held", out var h)) Held = h;
        if (Json.TryBool(p, "posed", out var ps)) Posed = ps;
        if (p.TryGetProperty("scenario", out var sc)) Scenario = sc.ValueKind == JsonValueKind.String ? sc.GetString() : null;
        if (Json.TryArr3(p, "wind_ned", out var wn, out var we, out var wd)) Wind = new[] { wn, we, wd };
        return true;
    }

    /// An `event` from the core: updates the model; returns (log source, log line) for the Log window.
    public (string Src, string Text) ApplyEvent(string name, JsonElement data, double t)
    {
        using var inv = new InvariantText();
        bool obj = data.ValueKind == JsonValueKind.Object;
        switch (name)
        {
            case "config":
                return ("config:", obj ? Config.OnConfig(data, t) : $"{data.GetRawText()} (t = {t:0.00} s)");
            case "launch":
            {
                var phase = obj ? Json.Str(data, "phase") ?? "?" : "?";
                if (phase == "start")
                {
                    LaunchEvent = "ramp…";
                    return ("event", $"launch: ramp… {Json.Num(data, "speed_mps", 0) * 3.6:0} km/h at {Json.Num(data, "a_mss", 0):0.0} m/s², pitch {Json.Num(data, "pitch_deg", 0):0}°, heading {Json.Num(data, "heading_deg", 0):0}° (t = {t:0.00} s)");
                }
                if (phase == "release")
                {
                    LaunchEvent = $"released at {Json.Num(data, "speed_mps", 0):0.0} m/s, {Json.Num(data, "height_m", 0):0.00} m";
                    return ("event", $"launch: {LaunchEvent} (t = {t:0.00} s)");
                }
                return ("event", $"launch {data.GetRawText()} (t = {t:0.00} s)");
            }
            case "crash":
            {
                double? D(string k) => obj && Json.TryNum(data, k, out var v) ? v : null;
                Crash = new CrashInfo(t, D("roll_deg") ?? 0, D("pitch_deg") ?? 0, D("sink_mps"), D("speed_mps"));
                return ("event", Crash.Text);
            }
            case "mix_warning":
                MixWarning = $"mix warning: both ailerons deflected the same way (L {Json.Num(data, "left", 0):+0.0;-0.0}°, R {Json.Num(data, "right", 0):+0.0;-0.0}°) — check the servo mix / `reversed` (t = {t:0.00} s)";
                return ("event", MixWarning);
            case "scenario_end":
                Step = $"scenario '{(obj ? Json.Str(data, "scenario") : null) ?? "?"}' ended";
                return ("event", $"{Step} (t = {t:0.00} s)");
            case "reset":
                Crash = null; MixWarning = ""; LaunchEvent = "";
                return ("event", $"reset {data.GetRawText()} (t = {t:0.00} s)");
        }
        // a scenario step's event (`pose`, `launch_command`, `done`, …): {label, index, step, scenario}; `pose` of a constraint: {roll_deg, …}
        if (obj && data.TryGetProperty("scenario", out _) && data.TryGetProperty("step", out _))
        {
            var label = Json.Str(data, "label");
            Step = $"{name}{(string.IsNullOrEmpty(label) ? "" : $" '{label}'")} (step {Json.Num(data, "step", 0):0} of {Json.Str(data, "scenario") ?? "?"})";
            var cal = Config.OnPose(data, t);
            return cal != null ? ("config:", cal) : ("event", $"{name} {data.GetRawText()} (t = {t:0.00} s)");
        }
        return ("event", $"{name}{(data.ValueKind is JsonValueKind.Undefined or JsonValueKind.Null ? "" : " " + data.GetRawText())} (t = {t:0.00} s)");
    }

    /// Reset aircraft pressed: the crash banner goes (the `reset` event clears it too).
    public void ClearCrash() => Crash = null;

    /// A new connection (Restart core, reconnect): the previous core's crash banner, launch and config lines go
    /// (a `failed` left over would otherwise read "rebooting after the config save…" while the new core boots).
    public void NewConnection() { Crash = null; MixWarning = ""; LaunchEvent = ""; Step = ""; Config.Reset(); }

    public string Deg(string n) => SurfacesDeg.TryGetValue(n, out var v) ? v.ToString("+0.0;-0.0;0.0", Inv) + "°" : "-";
}
