// Trainer plane (a 1.9 m trainer placeholder) procedural visual: meshes built under the named pivots of
// trainer.tscn and animated from the core's `state` (docs/protocol.md). Plain C# class implementing
// FlightBench.IAircraftVisual; compiled into the app via FlightBench.csproj (../models/*/visual/*.cs).
//
// Boxes and slabs only: fuselage, a high wing (1.88 m span, 0.24 m chord, as aircraft.json's b_m / c_m),
// horizontal and vertical tail, a tractor prop disc at the nose, a small FRD axis gizmo at the CG.
// Surfaces: each pivot sits on its hinge line and the surface runs back from it to the trailing edge, so a
// rotation of the pivot moves the trailing edge. Deflection in degrees from `state.plane.surfaces_deg`
// (the FDM's surface positions after servo delay and slew, the wiring's `reversed` applied):
//   aileron_l / aileron_r  + = trailing edge down   -> rotation about +x by +deg
//   elevator               + = trailing edge up     -> rotation about +x by -deg
//   rudder                 + = trailing edge right  -> rotation about +y by +deg
// A state without `plane` (an older core) falls back to `actuators` x ServoDeflectionDeg for the names it
// carries; names it does not carry keep their last value. The prop spins at `rpm` (the prop's rpm with the
// plane FDM); the disc's opacity grows with it (blur). Labels (the actuator names) show with body axes.
using System;
using System.Collections.Generic;
using System.Text.Json;
using Godot;

namespace FlightBench.Models;

public sealed class TrainerVisual : IAircraftVisual
{
    public static readonly string[] Surfaces = { "aileron_l", "aileron_r", "elevator", "rudder" };
    const float ServoDeflectionDeg = 20f;          // aircraft.json fdm_params.servo_deflection_deg: +-20 deg at full travel

    static readonly Color Foam = new(0.93f, 0.93f, 0.9f), Accent = new(0.95f, 0.45f, 0.12f), SurfaceColor = new(0.95f, 0.72f, 0.2f);
    static readonly Color Dark = new(0.12f, 0.12f, 0.14f), Canopy = new(0.2f, 0.35f, 0.55f);

    readonly Dictionary<string, Node3D> _pivots = new();
    readonly Dictionary<string, double> _deg = new();
    Node3D _prop = null!;
    MeshInstance3D _disc = null!;
    StandardMaterial3D _discMat = null!;
    double _rpm, _propAngle;

    /// Current deflection by surface name in degrees (what the pivots show).
    public double Deflection(string name) => _deg.TryGetValue(name, out var d) ? d : 0.0;
    /// Prop rpm last applied and the accumulated prop angle (rad): the tests check the spin.
    public double Rpm => _rpm;
    public double PropAngle => _propAngle;
    /// The pivot of a surface, or "prop" (null when the scene lacks it).
    public Node3D? Pivot(string name) => name == "prop" ? _prop : _pivots.TryGetValue(name, out var p) ? p : null;

    public void Build(Node3D root)
    {
        Node3D N(string n) => root.GetNodeOrNull<Node3D>(n) ?? AddPivot(root, n);
        static float F(Node3D n, string key, float def) => n.HasMeta(key) ? (float)n.GetMeta(key).AsDouble() : def;
        var body = N("body");

        // fuselage: nose at z = -0.47 (motor), tail boom to z = +0.70; high wing on a pylon, canopy
        VisualKit.Box(body, "fuselage", new Vector3(0.1f, 0.13f, 0.62f), new Vector3(0, -0.02f, -0.16f), Foam);
        VisualKit.Box(body, "tail_boom", new Vector3(0.05f, 0.06f, 0.56f), new Vector3(0, 0.0f, 0.42f), Foam);
        VisualKit.Box(body, "canopy", new Vector3(0.08f, 0.05f, 0.18f), new Vector3(0, 0.06f, -0.27f), Canopy);
        VisualKit.Box(body, "stripe", new Vector3(0.102f, 0.02f, 0.62f), new Vector3(0, -0.02f, -0.16f), Accent);

        // wing: 1.88 m span, 0.24 m chord in total; the fixed part is 0.18 m (LE z = -0.10 .. rear spar z = +0.08),
        // the trailing 0.06 m is fixed in the middle and at the tips, the ailerons in between
        const float span = 1.88f, wingY = 0.07f, le = -0.10f, spar = 0.08f, te = 0.14f;
        VisualKit.Box(body, "wing", new Vector3(span, 0.018f, spar - le), new Vector3(0, wingY, (le + spar) / 2), Foam);
        var ail = N("aileron_l");
        float ailSpan = F(ail, "span", 0.56f), ailOut = Mathf.Abs(ail.Position.X) + ailSpan / 2;
        float innerHalf = Mathf.Abs(ail.Position.X) - ailSpan / 2;
        VisualKit.Box(body, "wing_te_centre", new Vector3(2 * innerHalf, 0.012f, te - spar), new Vector3(0, wingY, (spar + te) / 2), Foam);
        if (span / 2 > ailOut + 0.005f)
            foreach (var s in new[] { -1f, 1f })
                VisualKit.Box(body, s < 0 ? "wing_te_tip_l" : "wing_te_tip_r", new Vector3(span / 2 - ailOut, 0.012f, te - spar), new Vector3(s * (ailOut + span / 2) / 2, wingY, (spar + te) / 2), Foam);
        VisualKit.Box(body, "wing_tip_l", new Vector3(0.02f, 0.022f, te - le), new Vector3(-span / 2, wingY, (le + te) / 2), new Color(0.9f, 0.2f, 0.2f));   // nav colours: left red,
        VisualKit.Box(body, "wing_tip_r", new Vector3(0.02f, 0.022f, te - le), new Vector3(span / 2, wingY, (le + te) / 2), new Color(0.2f, 0.75f, 0.3f));   // right green
        VisualKit.Box(body, "pylon", new Vector3(0.06f, 0.05f, 0.2f), new Vector3(0, 0.04f, -0.01f), Foam);

        // tail: horizontal stabiliser (the fixed part ahead of the elevator hinge), fin ahead of the rudder hinge
        var elev = N("elevator");
        float hSpan = F(elev, "span", 0.52f), hChord = 0.1f;
        VisualKit.Box(body, "stabiliser", new Vector3(hSpan, 0.012f, hChord), new Vector3(0, elev.Position.Y, elev.Position.Z - hChord / 2), Foam);
        var rud = N("rudder");
        float fin = F(rud, "height", 0.17f), finChord = 0.1f;
        VisualKit.Box(body, "fin", new Vector3(0.012f, fin, finChord), new Vector3(0, rud.Position.Y, rud.Position.Z - finChord / 2), Foam);

        // the four surfaces: a slab behind each hinge line, coloured, with its name label
        foreach (var name in Surfaces)
        {
            var p = N(name);
            _pivots[name] = p;
            _deg[name] = 0;
            float chord = F(p, "chord", 0.06f);
            if (name == "rudder")
            {
                float h = F(p, "height", 0.17f);
                VisualKit.Box(p, "surface", new Vector3(0.01f, h, chord), new Vector3(0, 0, chord / 2), SurfaceColor);
                VisualKit.Label(p, name, new Vector3(0.06f, h / 2 + 0.04f, chord), SurfaceColor);
                VisualKit.Cyl(p, "hinge", 0.004f, h, Vector3.Zero, Dark);
            }
            else
            {
                float w = F(p, "span", 0.5f);
                VisualKit.Box(p, "surface", new Vector3(w, 0.01f, chord), new Vector3(0, 0, chord / 2), SurfaceColor);
                VisualKit.Label(p, name, new Vector3(0, 0.06f, chord), SurfaceColor);
                VisualKit.Cyl(p, "hinge", 0.004f, w, Vector3.Zero, Dark, new Vector3(0, 0, 90));
            }
        }

        // motor can and the prop: spinner, two blades, a blur disc (axis along z)
        var motor = N("motor");
        VisualKit.Cyl(motor, "can", 0.028f, 0.04f, Vector3.Zero, new Color(0.7f, 0.7f, 0.74f), new Vector3(90, 0, 0), 0.6f);
        VisualKit.Label(motor, "motor", new Vector3(0, 0.07f, 0), Accent);
        _prop = N("prop");
        float r = F(_prop, "radius", 0.14f);
        VisualKit.Ellipsoid(_prop, "spinner", new Vector3(0.022f, 0.022f, 0.035f), Vector3.Zero, Dark);
        VisualKit.Box(_prop, "blade_1", new Vector3(r, 0.018f, 0.006f), new Vector3(r / 2, 0, 0), Dark);
        VisualKit.Box(_prop, "blade_2", new Vector3(r, 0.018f, 0.006f), new Vector3(-r / 2, 0, 0), Dark);
        _discMat = new StandardMaterial3D { AlbedoColor = new Color(0.85f, 0.85f, 0.9f, 0.0f), Transparency = BaseMaterial3D.TransparencyEnum.Alpha, CullMode = BaseMaterial3D.CullModeEnum.Disabled, ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded };
        _disc = new MeshInstance3D { Name = "prop_disc", Mesh = new CylinderMesh { TopRadius = r, BottomRadius = r, Height = 0.002f, RadialSegments = 32, Rings = 1 }, RotationDegrees = new Vector3(90, 0, 0), MaterialOverride = _discMat, Visible = false };
        root.AddChild(_disc);
        _disc.Position = _prop.Position + new Vector3(0, 0, -0.004f);
        VisualKit.Label(_prop, "prop", new Vector3(0, r + 0.04f, 0), Accent);

        // axis gizmo at the CG (always shown, small): forward red, right green, down blue (FRD, the FDM's body axes)
        var gizmo = new Node3D { Name = "axis_gizmo" };
        root.AddChild(gizmo);
        void Axis(string n, Vector3 dir, Color c)
        {
            var rod = VisualKit.Rod(gizmo, n, 0.004f, c);
            rod.MaterialOverride = new StandardMaterial3D { AlbedoColor = c, ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded };
            VisualKit.Place(rod, Vector3.Zero, dir * 0.18f);
        }
        Axis("gizmo_f", Vector3.Forward, new Color(1f, 0.3f, 0.3f));
        Axis("gizmo_r", Vector3.Right, new Color(0.3f, 1f, 0.3f));
        Axis("gizmo_d", Vector3.Down, new Color(0.4f, 0.6f, 1f));
        ApplySurfaces();
    }

    static Node3D AddPivot(Node3D root, string name)
    {
        var n = new Node3D { Name = name };
        root.AddChild(n);
        return n;
    }

    public void ApplyState(JsonElement state)
    {
        if (state.TryGetProperty("rpm", out var r) && r.ValueKind == JsonValueKind.Number) _rpm = r.GetDouble();
        bool fromPlane = false;
        if (state.TryGetProperty("plane", out var p) && p.ValueKind == JsonValueKind.Object
            && p.TryGetProperty("surfaces_deg", out var sd) && sd.ValueKind == JsonValueKind.Object)
        {
            foreach (var name in Surfaces)
                if (sd.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number) { _deg[name] = v.GetDouble(); fromPlane = true; }
        }
        if (!fromPlane && state.TryGetProperty("actuators", out var a) && a.ValueKind == JsonValueKind.Object)
            foreach (var name in Surfaces)
                if (a.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number) _deg[name] = Math.Clamp(v.GetDouble(), -1.0, 1.0) * ServoDeflectionDeg;
        ApplySurfaces();
    }

    void ApplySurfaces()
    {
        foreach (var (name, pivot) in _pivots)
        {
            float d = (float)Deflection(name);
            pivot.RotationDegrees = name switch
            {
                "rudder" => new Vector3(0, d, 0),        // + = trailing edge right (+x)
                "elevator" => new Vector3(-d, 0, 0),     // + = trailing edge up
                _ => new Vector3(d, 0, 0),               // ailerons: + = trailing edge down
            };
        }
    }

    public void Tick(double delta)
    {
        // the real prop speed (a 60 Hz view aliases it: the disc carries the blur)
        _propAngle = (_propAngle + _rpm / 60.0 * Math.Tau * delta) % Math.Tau;
        _prop.Rotation = new Vector3(0, 0, (float)_propAngle);
        var c = _discMat.AlbedoColor;
        c.A = (float)Math.Clamp(_rpm / 6000.0, 0.0, 1.0) * 0.35f;
        _discMat.AlbedoColor = c;
        _disc.Visible = c.A > 0.01f;
    }
}
