// The Aircraft window (docs/ui.md "Flight screen windows"): everything the core says about the model itself, from the
// FDM / core state — altitude, ground speed and climb, air data (plane: airspeed, α / β), power (plane: thrust, prop
// rpm, throttle; heli: head rpm, motor rpm, throttle), the control surfaces (plane, degrees) or the servos (heli,
// −100..100 % of travel), the launch phase / hold / scenario and its current step, the mix warning, and the crash
// banner with Reset aircraft. A row the state does not carry is hidden (the heli has no α / β, the plane no head
// rpm; nothing before the first state), so one window fits both aircraft classes.
// AircraftReadout is the pure model (the self-tests feed it states); AircraftPanel draws its rows.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public sealed class AircraftReadout
{
    /// The plane's `state.plane` and events (launch, scenario steps, mix warning, crash) and the first-start config.
    public readonly PlaneStatus Plane;
    public bool HasState { get; private set; }
    public double? Alt, GroundSpeed, Climb, Rpm, MotorRpm, Throttle;
    /// `state.heli` (the heli FDM): blade collective pitch, deg; rotor thrust, N.
    public double? Coll, Thrust;
    public bool? OnGround;
    /// Servo actuators (all but the motor), −1..1 by name, in the state's order.
    public readonly List<(string Name, double Value)> Servos = new();

    public AircraftReadout(PlaneStatus plane) { Plane = plane; }

    /// Copies what the rows need from a `state` (the JsonElement does not outlive the message).
    public void ApplyState(JsonElement s)
    {
        if (s.ValueKind != JsonValueKind.Object) return;
        HasState = true;
        Alt = GroundSpeed = Climb = null;                                                    // each state says it again (or not)
        if (Json.TryArr3(s, "pos_ned", out _, out _, out var d)) Alt = -d + 0.0;             // + 0.0: no "-0.00 m" on the ground
        if (Json.TryArr3(s, "vel_ned", out var vn, out var ve, out var vd)) { GroundSpeed = Math.Sqrt(vn * vn + ve * ve); Climb = -vd; }
        Rpm = Json.TryNum(s, "rpm", out var r) ? r : null;
        MotorRpm = Json.TryNum(s, "motor_rpm", out var mr) ? mr : null;
        OnGround = Json.TryBool(s, "on_ground", out var og) ? og : null;
        Throttle = null;
        Servos.Clear();
        if (s.TryGetProperty("actuators", out var a) && a.ValueKind == JsonValueKind.Object)
            foreach (var p in a.EnumerateObject())
            {
                if (p.Value.ValueKind != JsonValueKind.Number) continue;
                if (p.Name is "motor" or "throttle") Throttle = p.Value.GetDouble();
                else Servos.Add((p.Name, p.Value.GetDouble()));
            }
        Coll = Thrust = null;
        if (s.TryGetProperty("heli", out var h) && h.ValueKind == JsonValueKind.Object)
        {
            Coll = Json.TryNum(h, "coll_deg", out var cd) ? cd : null;
            Thrust = Json.TryNum(h, "thrust_n", out var tn) ? tn : null;
        }
        Plane.ApplyState(s);
    }

    /// A new connection: the previous core's values go.
    public void Clear()
    {
        HasState = false; Alt = GroundSpeed = Climb = Rpm = MotorRpm = Throttle = Coll = Thrust = null; OnGround = null; Servos.Clear();
    }

    public static readonly string[] RowIds = { "alt", "speed", "air", "power", "surfaces", "launch", "step", "mix" };

    /// The rows as (id, label, text); a row the state lacks is absent. Order: RowIds.
    public List<(string Id, string Label, string Text)> Rows()
    {
        using var inv = new InvariantText();
        var rows = new List<(string, string, string)>();
        if (!HasState) return rows;
        bool plane = Plane.HasPlane;
        if (Alt is { } alt) rows.Add(("alt", "altitude", $"{alt:0.00} m{(OnGround == true ? " · on the ground" : "")}"));
        if (GroundSpeed is { } gs) rows.Add(("speed", "speed", $"ground {gs:0.0} m/s · climb {Climb ?? 0:+0.0;-0.0;0.0} m/s"));
        if (plane) rows.Add(("air", "air", $"airspeed {Plane.Airspeed:0.0} m/s ({Plane.Airspeed * 3.6:0} km/h) · α {Plane.Alpha:+0.0;-0.0;0.0}° · β {Plane.Beta:+0.0;-0.0;0.0}°"));
        var power = new List<string>();
        if (plane)
        {
            power.Add($"thrust {Plane.Thrust:0.0} N");
            if (Rpm is { } pr) power.Add($"prop {pr:0} rpm");
        }
        else
        {
            if (Rpm is { } hr) power.Add($"head {hr:0} rpm");
            if (MotorRpm is { } m) power.Add($"motor {m:0} rpm");
            if (Coll is { } cd) power.Add($"collective {cd:+0.0;-0.0;0.0}°");     // state.heli (the FDM's blade pitch)
            if (Thrust is { } tn) power.Add($"thrust {tn:0} N");
        }
        if (Throttle is { } th) power.Add($"throttle {th * 100:0}%");
        if (power.Count > 0) rows.Add(("power", plane ? "motor" : "rotor", string.Join(" · ", power)));
        if (plane && Plane.SurfacesDeg.Count > 0)
            rows.Add(("surfaces", "surfaces", $"ail L {Plane.Deg("aileron_l")} R {Plane.Deg("aileron_r")} · elev {Plane.Deg("elevator")} · rud {Plane.Deg("rudder")}"));
        else if (!plane && Servos.Count > 0)
            rows.Add(("surfaces", "servos", string.Join(" · ", Servos.Select(x => $"{x.Name} {x.Value * 100:+0;-0;0}%"))));
        if (plane)
        {
            var hold = Plane.Held ? (Plane.Posed ? " · held (posed)" : " · held") : "";
            rows.Add(("launch", "launch", $"{Plane.LaunchPhase}{(Plane.LaunchEvent != "" ? " · " + Plane.LaunchEvent : "")}{hold} · scenario {Plane.Scenario ?? "none"}"));
        }
        if (Plane.Step != "") rows.Add(("step", "step", Plane.Step));
        if (Plane.MixWarning != "") rows.Add(("mix", "warning", Plane.MixWarning));
        return rows;
    }

    /// All rows as text (logs, self-tests).
    public string Text() => string.Join("\n", Rows().Select(r => $"{r.Label}: {r.Text}"));
}

public partial class AircraftPanel : VBoxContainer
{
    /// Reset aircraft pressed on the crash banner.
    public event Action? ResetRequested;
    public AircraftReadout? Model;

    private readonly GridContainer _grid = new() { Columns = 2 };
    private readonly Dictionary<string, (Label Name, Label Value)> _rows = new();
    private readonly Label _empty = new() { Text = "no state from the core yet", Modulate = Ui.Soft };
    private readonly HFlowContainer _crash = new() { Visible = false };
    private readonly Label _crashLabel = new() { Modulate = new Color(1f, 0.4f, 0.35f), AutowrapMode = TextServer.AutowrapMode.WordSmart, CustomMinimumSize = new Vector2(200, 0) };
    private readonly Button _reset = new() { FocusMode = FocusModeEnum.None, TooltipText = "back to the pad (the default scenario runs again)" };

    public AircraftPanel()
    {
        AddThemeConstantOverride("separation", 4);
        _crash.AddThemeConstantOverride("h_separation", 8);
        _reset.Text = "Reset aircraft" + Keybinds.Suffix("reset_aircraft");
        _reset.Pressed += () => ResetRequested?.Invoke();
        _crash.AddChild(_crashLabel); _crash.AddChild(_reset);
        AddChild(_crash);                                            // on top: the first thing seen after a crash
        AddChild(_empty);
        _grid.AddThemeConstantOverride("h_separation", 10); _grid.AddThemeConstantOverride("v_separation", 2);
        foreach (var id in AircraftReadout.RowIds)
        {
            var n = new Label { Modulate = Ui.Soft, Visible = false };
            var v = new Label { AutowrapMode = TextServer.AutowrapMode.WordSmart, SizeFlagsHorizontal = SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(160, 0), Visible = false };
            if (id == "mix") v.Modulate = new Color(1f, 0.82f, 0.45f);
            _grid.AddChild(n); _grid.AddChild(v);
            _rows[id] = (n, v);
        }
        AddChild(_grid);
    }

    /// Rows and the crash banner from the model.
    public void Refresh()
    {
        if (Model == null) return;
        var rows = Model.Rows();
        var seen = new HashSet<string>();
        foreach (var (id, label, text) in rows)
        {
            if (!_rows.TryGetValue(id, out var r)) continue;
            seen.Add(id);
            if (r.Name.Text != label) r.Name.Text = label;
            if (r.Value.Text != text) r.Value.Text = text;
            r.Name.Visible = r.Value.Visible = true;
        }
        foreach (var (id, r) in _rows) if (!seen.Contains(id)) r.Name.Visible = r.Value.Visible = false;
        _empty.Visible = rows.Count == 0;
        var crash = Model.Plane.Crash;
        _crash.Visible = crash != null;
        _crashLabel.Text = crash?.Text ?? "";
    }

    // self-tests
    public bool CrashVisible => _crash.Visible;
    public string CrashText => _crashLabel.Text;
    public void PressReset() => _reset.EmitSignal(BaseButton.SignalName.Pressed);
    public IEnumerable<string> VisibleRows => _rows.Where(kv => kv.Value.Value.Visible).Select(kv => kv.Key);
    public string RowText(string id) => _rows.TryGetValue(id, out var r) && r.Value.Visible ? r.Value.Text : "";
}
