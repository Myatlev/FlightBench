// Contract between the app and an aircraft's visual (models/<name>/visual/<name>.tscn + a C# class).
// The scene is a tree of named Node3Ds (actuator names from the model README: swash_a, swash_b,
// swash_c, tail, motor, plus rotor / tail_rotor / swashplate / fpv); its root carries metadata:
//   visual_class = full C# type name implementing IAircraftVisual (compiled into the app through
//                  FlightBench.csproj's <Compile Include="../models/*/visual/*.cs" />)
//   cg_height    = metres from the skids to the scene origin (the FDM puts the skids at d = 0)
// The class is a plain C# object (not a Godot script), so the scene can live outside res:// and be
// loaded by absolute path; it builds meshes under the named nodes and animates them from `state`.
// Local frame: Godot body axes, x = right, y = up, z = back (nose towards -z), origin at the CG.
using System.Text.Json;
using Godot;

namespace FlightBench;

public interface IAircraftVisual
{
    /// Build meshes under the instantiated scene's nodes (root = the scene root).
    void Build(Node3D root);
    /// Latest `state` message (actuators normalised -1..1 by name, rpm = head speed).
    void ApplyState(JsonElement state);
    /// Per-frame animation (spin, smoothing).
    void Tick(double delta);
}

/// Small helpers for procedural visuals: primitive meshes with flat colours, rods between points.
public static class VisualKit
{
    public static StandardMaterial3D Mat(Color c, float metallic = 0f, float roughness = 0.6f)
    {
        var m = new StandardMaterial3D { AlbedoColor = c, Metallic = metallic, Roughness = roughness };
        if (c.A < 1f) { m.Transparency = BaseMaterial3D.TransparencyEnum.Alpha; m.CullMode = BaseMaterial3D.CullModeEnum.Disabled; }
        return m;
    }

    public static MeshInstance3D Box(Node3D parent, string name, Vector3 size, Vector3 pos, Color c, float metallic = 0f)
    {
        var m = new MeshInstance3D { Name = name, Mesh = new BoxMesh { Size = size }, Position = pos, MaterialOverride = Mat(c, metallic) };
        parent.AddChild(m);
        return m;
    }

    /// Cylinder along the local Y axis (use `rot` in degrees to lay it along X or Z).
    public static MeshInstance3D Cyl(Node3D parent, string name, float radius, float height, Vector3 pos, Color c, Vector3 rot = default, float metallic = 0f, int segments = 24)
    {
        var m = new MeshInstance3D { Name = name, Mesh = new CylinderMesh { TopRadius = radius, BottomRadius = radius, Height = height, RadialSegments = segments, Rings = 1 },
                                     Position = pos, RotationDegrees = rot, MaterialOverride = Mat(c, metallic) };
        parent.AddChild(m);
        return m;
    }

    public static MeshInstance3D Ellipsoid(Node3D parent, string name, Vector3 semiAxes, Vector3 pos, Color c)
    {
        var m = new MeshInstance3D { Name = name, Mesh = new SphereMesh { Radius = 1f, Height = 2f, RadialSegments = 32, Rings = 16 }, Position = pos, Scale = semiAxes, MaterialOverride = Mat(c) };
        parent.AddChild(m);
        return m;
    }

    /// A thin rod (unit-height cylinder of the given radius) to be placed between two points with `Place`.
    public static MeshInstance3D Rod(Node3D parent, string name, float radius, Color c)
    {
        var m = new MeshInstance3D { Name = name, Mesh = new CylinderMesh { TopRadius = radius, BottomRadius = radius, Height = 1f, RadialSegments = 8, Rings = 1 }, MaterialOverride = Mat(c, 0.6f, 0.3f) };
        parent.AddChild(m);
        return m;
    }

    /// Stretches a unit rod from a to b (both in the rod's parent frame).
    public static void Place(Node3D rod, Vector3 a, Vector3 b)
    {
        var d = b - a;
        float len = d.Length();
        if (len < 1e-6f) { rod.Visible = false; return; }
        rod.Visible = true;
        var y = d / len;
        var x = Mathf.Abs(y.Dot(Vector3.Up)) > 0.9f ? y.Cross(Vector3.Right).Normalized() : y.Cross(Vector3.Up).Normalized();
        var z = x.Cross(y);
        rod.Transform = new Transform3D(new Basis(x, y * len, z), (a + b) * 0.5f);
    }

    public static Label3D Label(Node3D parent, string text, Vector3 pos, Color c)
    {
        var l = new Label3D { Name = "label_" + text, Text = text, Position = pos, Billboard = BaseMaterial3D.BillboardModeEnum.Enabled, PixelSize = 0.0007f,
                              FontSize = 40, OutlineSize = 10, Modulate = c, NoDepthTest = true, FixedSize = false };
        parent.AddChild(l);
        return l;
    }

    public static double Actuator(JsonElement state, string name)
    {
        if (state.TryGetProperty("actuators", out var a) && a.ValueKind == JsonValueKind.Object && a.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number)
            return Mathf.Clamp(v.GetDouble(), -1.0, 1.0);
        return 0.0;
    }
}
