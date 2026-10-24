// Aircraft in the 3D world: loads the aircraft's visual scene from the repo
// (models/<name>/visual/<name>.tscn, see AircraftVisual.cs) and falls back to a placeholder of boxes
// when it is missing. Pose comes from the core in NED; Godot is Y-up right-handed, so
// NED (n, e, d) -> Godot (x = e, y = -d, z = -n) and the quaternion is remapped the same way.
// The FDM puts the skids (a plane: its ground contact) at d = 0; the scene origin is the CG, `cg_height` above them.
// `view_size` (m, root metadata; default 1.6) is the model's size: the cameras scale their distances by it.
// VisualPath (a bench's `visual`) replaces the models/<name>/visual/<name>.tscn convention.
// AttitudeOnly (Model window): no position, optionally no heading, same visual.
using System;
using System.IO;
using System.Text.Json;
using Godot;

namespace FlightBench;

public partial class Aircraft : Node3D
{
    public string AircraftName = "rs7";
    /// The visual scene to load (absolute); null / empty = models/<AircraftName>/visual/<AircraftName>.tscn under the repo root.
    public string? VisualPath;
    public bool AttitudeOnly;
    public bool WithHeading = true;          // AttitudeOnly: keep the yaw, or show roll/pitch only
    public string Source { get; private set; } = "";
    public float CgHeight { get; private set; }
    /// The model's size in metres (scene metadata `view_size`; RS7 rotor 1.58 m, trainer span 1.88 m).
    public float ViewSize { get; private set; } = 1.6f;
    /// The visual's C# object (null for a scene whose class was not found).
    public IAircraftVisual? Visual => _visual;
    /// Camera mount at the nose in body-local coordinates (the scene's `fpv` node if present).
    public Vector3 FpvMount { get; private set; } = new(0, 0.03f, -0.5f);
    /// Attitude as received (world), independent of AttitudeOnly's heading removal.
    public Quaternion Attitude { get; private set; } = Quaternion.Identity;
    public double Rpm { get; private set; }

    private IAircraftVisual? _visual;
    // FB_ACT_SWEEP=1: the visual gets slow actuator sines instead of the core's values (linkage check
    // against a core whose actuators do not move, e.g. tools/fake_core.py); pose and rpm stay the core's
    private static bool Sweep => DevEnv.ActSweep;
    private Node3D? _visualRoot, _axes;

    public override void _Ready()
    {
        LoadVisual();
        BuildAxes();
        ShowAxes(false);
    }

    private void LoadVisual()
    {
        var path = (string.IsNullOrWhiteSpace(VisualPath) ? Path.Combine(AppState.RepoRoot, "models", AircraftName, "visual", AircraftName + ".tscn") : VisualPath).Replace('\\', '/');
        Node3D? root = null;
        if (File.Exists(path))
        {
            var scene = ResourceLoader.Load<PackedScene>(path, null, ResourceLoader.CacheMode.Reuse);
            root = scene?.Instantiate() as Node3D;
            if (root == null) GD.PushWarning($"aircraft visual {path}: cannot load, using the placeholder");
        }
        if (root != null)
        {
            var cls = root.HasMeta("visual_class") ? root.GetMeta("visual_class").AsString() : "";
            var type = string.IsNullOrEmpty(cls) ? null : Type.GetType(cls);
            if (type != null && typeof(IAircraftVisual).IsAssignableFrom(type)) _visual = (IAircraftVisual?)Activator.CreateInstance(type);
            else GD.PushWarning($"aircraft visual {path}: visual_class '{cls}' not found in the app assembly (models/*/visual/*.cs compiled?) — nodes only, no animation");
            CgHeight = root.HasMeta("cg_height") ? (float)root.GetMeta("cg_height").AsDouble() : 0f;
            if (root.HasMeta("view_size")) ViewSize = Mathf.Max(0.2f, (float)root.GetMeta("view_size").AsDouble());
            Source = path;
        }
        else
        {
            root = new Node3D { Name = "placeholder" };
            _visual = new PlaceholderVisual();
            CgHeight = 0.13f;
            Source = "placeholder";
        }
        AddChild(root);
        _visualRoot = root;
        _visual?.Build(root);
        if (root.GetNodeOrNull<Node3D>("fpv") is { } fpv) FpvMount = fpv.Position;
        SetLabels(false);
        if (!AttitudeOnly) GD.Print($"[app] aircraft visual: {Source}");
    }

    /// Body axes FRD: forward red, right green, down blue.
    private void BuildAxes()
    {
        _axes = new Node3D { Name = "body_axes" };
        AddChild(_axes);
        void Arrow(Vector3 dir, Color c, string label)
        {
            var mat = new StandardMaterial3D { AlbedoColor = c, ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded, NoDepthTest = true };
            const float len = 0.4f;
            var shaft = new MeshInstance3D { Mesh = new CylinderMesh { TopRadius = 0.008f, BottomRadius = 0.008f, Height = len }, MaterialOverride = mat };
            var head = new MeshInstance3D { Mesh = new CylinderMesh { TopRadius = 0f, BottomRadius = 0.025f, Height = 0.07f }, MaterialOverride = mat, Position = new Vector3(0, len / 2 + 0.035f, 0) };
            var arrow = new Node3D();
            arrow.AddChild(shaft); arrow.AddChild(head);
            var y = dir; var x = Mathf.Abs(y.Dot(Vector3.Up)) > 0.9f ? Vector3.Right : y.Cross(Vector3.Up).Normalized(); var z = x.Cross(y);
            arrow.Transform = new Transform3D(new Basis(x, y, z), dir * len / 2);
            _axes.AddChild(arrow);
            _axes.AddChild(new Label3D { Text = label, Position = dir * (len + 0.1f), Billboard = BaseMaterial3D.BillboardModeEnum.Enabled, PixelSize = 0.0012f, FontSize = 48, OutlineSize = 12, Modulate = c, NoDepthTest = true });
        }
        Arrow(Vector3.Forward, new Color(1, 0.3f, 0.3f), "F");
        Arrow(Vector3.Right, new Color(0.3f, 1, 0.3f), "R");
        Arrow(Vector3.Down, new Color(0.4f, 0.6f, 1), "D");
    }

    /// Body axes and the visual's actuator labels (Label3D nodes) together.
    public void ShowAxes(bool on)
    {
        if (_axes != null) _axes.Visible = on;
        SetLabels(on);
    }

    private void SetLabels(bool on)
    {
        if (_visualRoot == null) return;
        foreach (var n in _visualRoot.FindChildren("*", "Label3D", true, false)) ((Label3D)n).Visible = on;
    }

    public void ApplyState(JsonElement state)
    {
        if (!AttitudeOnly && Json.TryArr3(state, "pos_ned", out var n, out var e, out var d))
            Position = new Vector3((float)e, (float)-d + CgHeight, (float)-n);
        if (Json.TryArr4(state, "quat", out var qw, out var qx, out var qy, out var qz))
        {
            // body(FRD)->NED quaternion (w,x,y,z). Godot body: x = right(=body y), y = up(=-body z), z = back(=-body x).
            // Remap axes NED->Godot for the rotation: (x,y,z) -> (y, -z, -x)
            var q = new Quaternion((float)qy, (float)-qz, (float)-qx, (float)qw);
            if (q.LengthSquared() > 1e-6f)
            {
                Attitude = q.Normalized();
                Quaternion = AttitudeOnly && !WithHeading ? WithoutHeading(Attitude) : Attitude;
            }
        }
        if (Json.TryNum(state, "rpm", out var r)) Rpm = r;
        if (Sweep && _visual != null)
        {
            double t = Time.GetTicksMsec() / 1000.0;
            string F(double v) => v.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture);
            var json = $"{{\"rpm\":{F(Rpm)},\"actuators\":{{\"swash_a\":{F(0.2 + 0.7 * Math.Sin(t * 1.3))},\"swash_b\":{F(0.2 + 0.7 * Math.Sin(t * 1.3 + 2.094))},"
                     + $"\"swash_c\":{F(0.2 + 0.7 * Math.Sin(t * 1.3 + 4.189))},\"tail\":{F(0.9 * Math.Sin(t * 0.9))},\"motor\":0}}}}";
            using var doc = JsonDocument.Parse(json);
            _visual.ApplyState(doc.RootElement);
            return;
        }
        _visual?.ApplyState(state);
    }

    /// Heading angle about world up (0 = nose north = Godot -Z), radians, positive to the left (Godot +Y rotation).
    public static float Heading(Quaternion q)
    {
        var f = q * Vector3.Forward;
        return Mathf.Atan2(-f.X, -f.Z);
    }

    /// q with the rotation about world up removed (roll and pitch kept).
    public static Quaternion WithoutHeading(Quaternion q) => (new Quaternion(Vector3.Up, -Heading(q)) * q).Normalized();

    public override void _Process(double delta) => _visual?.Tick(delta);
}

/// Boxes for a bench without an aircraft visual: body, boom, one blade, tail blade.
public sealed class PlaceholderVisual : IAircraftVisual
{
    private Node3D _rotor = null!, _tailRotor = null!;
    private double _rpm;

    public void Build(Node3D root)
    {
        VisualKit.Box(root, "body", new Vector3(0.25f, 0.25f, 1.2f), Vector3.Zero, new Color(0.9f, 0.55f, 0.1f));
        VisualKit.Box(root, "boom", new Vector3(0.06f, 0.06f, 1.0f), new Vector3(0, 0.05f, 1.0f), new Color(0.8f, 0.8f, 0.8f));
        _rotor = new Node3D { Name = "rotor", Position = new Vector3(0, 0.35f, 0) };
        root.AddChild(_rotor);
        VisualKit.Box(_rotor, "blade", new Vector3(1.6f, 0.01f, 0.06f), Vector3.Zero, new Color(0.2f, 0.2f, 0.2f));
        _tailRotor = new Node3D { Name = "tail", Position = new Vector3(0.06f, 0.1f, 1.5f) };
        root.AddChild(_tailRotor);
        VisualKit.Box(_tailRotor, "blade", new Vector3(0.01f, 0.3f, 0.04f), Vector3.Zero, new Color(0.2f, 0.2f, 0.2f));
    }

    public void ApplyState(JsonElement state)
    {
        if (state.TryGetProperty("rpm", out var r) && r.ValueKind == JsonValueKind.Number) _rpm = r.GetDouble();
    }

    public void Tick(double delta)
    {
        _rotor.RotateY((float)(_rpm / 60.0 * Mathf.Tau * delta) * 0.25f);   // slowed for the eye
        _tailRotor.RotateX((float)(_rpm / 60.0 * Mathf.Tau * delta) * 1.2f);
    }
}
