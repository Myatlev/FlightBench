// RS7 (Goosky RS7 Ultra, 700 class) procedural visual: meshes are built under the named pivots of
// rs7.tscn and animated from the core's `state` (docs/protocol.md). Plain C# class implementing
// FlightBench.IAircraftVisual; compiled into the app via FlightBench.csproj (../models/*/visual/*.cs).
//
// Layout after the RS7 Ultra manual (servo page): open frame, no canopy; motor ahead of the main shaft
// (axis vertical, pinion on the main gear at the shaft base), ESC on an angled plate at the front,
// battery slung under the frame between the skids.
// Swash servos, 120 deg CCPM as on the RS7, named after RotorFlight's SWASH_TYPE_120 mixer (servo 1 =
// swash_a is the single pitch servo: 0.5*SC - SP; servos 2/3 = swash_b/c the roll pair, +-0.866*SR):
// swash_a on the centre line behind the main shaft (plate ball at 180 deg), swash_b left-front (-60 deg),
// swash_c right-front (+60 deg), mirror images. Each case stands upright; its horn (ArmLen, direction =
// the node's `horn` metadata) points at the ball's spot from outside, so the horn tip sits exactly under
// its plate ball and the pushrod is vertical at neutral; the case sits beside the horn along the output
// shaft (`case` metadata: which way along the shaft). The horn is horizontal at neutral and ROTATES about
// the output shaft in a vertical plane by theta_i = actuators[swash_i] * MaxArmDeg, so its ball
// describes an arc (+-ArmLen*sin 30 = +-10 mm up/down plus a little in/out). The pushrod is a rigid
// link of length PushLen from the horn ball to the plate ball (kept at radius BallR on its azimuth):
// ball height = tip.y + sqrt(PushLen^2 - horizontal offset^2). Plate = plane through the three balls:
// height h0 = mean(h_i), slopes g = 2/(3 BallR) * sum(h_i u_i) (u_i = horizontal unit vector to ball i),
// normal = (-g.x, 1, -g.z). Blade pitch = the plate height under the blade's pitch link (leading side),
// scaled to +-MaxPitchDeg.
// Tail: the servo sits on the frame at the boom root, output shaft across the boom (x); its horn swings
// fore/aft, the pushrod runs under the boom to the bell crank at the tail case (pivot vertical), whose
// second arm moves the pitch slider on the tail shaft; tail blades pitch +-MaxTailPitchDeg.
// Spin: the blades turn at the real head speed `rpm` (no blur disc); the tail rotor at TailRatio x the
// head; the motor can at `motor_rpm` (or MotorRatio x the head when the core does not send it).
// The frame is open (side plates drawn as outlines) so servos, arms, pushrods and the plate stay visible.
using System;
using System.Text.Json;
using Godot;

namespace FlightBench.Models;

public sealed class Rs7Visual : IAircraftVisual
{
    const float RotorR = 0.79f;          // 1.58 m disc
    const float TailR = 0.14f;
    const float ArmLen = 0.02f;          // servo horn: +-30 deg -> +-10 mm of ball travel
    const float TailHorn = 0.025f;       // tail servo horn (long, standing up)
    const float SidePlateX = 0.06f;      // side plates (outline rods) at x = +-SidePlateX
    const float CrankIn = 0.015f, CrankOut = 0.025f;   // bell crank: pushrod arm, slider arm
    const float BallR = 0.045f;          // swashplate ball radius
    const float MaxArmDeg = 30f;
    const float MaxPitchDeg = 12f;
    const float MaxTailPitchDeg = 20f;
    const float TailRatio = 4.5f;
    const double MotorRatio = 9.0;       // head -> motor when `motor_rpm` is missing

    static readonly string[] Swash = { "swash_a", "swash_b", "swash_c" };
    static readonly Color[] SwashColor = { new(0.95f, 0.25f, 0.2f), new(0.25f, 0.85f, 0.3f), new(0.3f, 0.5f, 1f) };
    static readonly Color Carbon = new(0.11f, 0.11f, 0.12f), Alu = new(0.72f, 0.73f, 0.76f), Canopy = new(1f, 0.52f, 0.08f), Blade = new(0.08f, 0.08f, 0.09f);
    static readonly Color TailYellow = new(0.95f, 0.8f, 0.2f), ServoCase = new(0.15f, 0.15f, 0.17f);

    Node3D _rotor = null!, _plate = null!, _tailRotor = null!, _tail = null!, _tailServo = null!, _motor = null!, _gear = null!;
    readonly Node3D[] _arms = new Node3D[3];
    readonly Basis[] _armRest = new Basis[3];
    readonly Vector3[] _u = new Vector3[3];
    readonly MeshInstance3D[] _pushrods = new MeshInstance3D[3];
    readonly Node3D[] _grips = new Node3D[2], _bladePitch = new Node3D[2], _tailPitch = new Node3D[2];
    readonly MeshInstance3D[] _pitchLinks = new MeshInstance3D[2];
    MeshInstance3D _tailRod = null!, _sliderLink = null!;
    Node3D _slider = null!;
    Vector3 _plateBase;
    float _pushLen;
    double _rpm, _motorRpm = -1, _tailCmd, _rotAng, _tailAng, _motAng;
    readonly double[] _swash = new double[3];

    public void Build(Node3D root)
    {
        Node3D N(string n) => root.GetNodeOrNull<Node3D>(n) ?? AddPivot(root, n);
        var body = N("body");
        _rotor = N("rotor"); _plate = N("swashplate"); _tailRotor = N("tail_rotor"); _tail = N("tail"); _tailServo = N("tail_servo"); _motor = N("motor");
        _plateBase = _plate.Position;

        // ---- no canopy: open carbon frame, angled ESC plate at the front, battery under the frame ----
        var escPlate = new Node3D { Name = "esc_plate", Position = new Vector3(0, 0.02f, -0.19f), RotationDegrees = new Vector3(-35f, 0, 0) };   // front edge down
        body.AddChild(escPlate);
        VisualKit.Box(escPlate, "plate", new Vector3(0.11f, 0.004f, 0.15f), Vector3.Zero, Carbon);
        VisualKit.Box(escPlate, "esc", new Vector3(0.06f, 0.026f, 0.085f), new Vector3(0, 0.015f, 0.005f), new Color(0.12f, 0.2f, 0.45f));
        VisualKit.Box(escPlate, "esc_cap", new Vector3(0.05f, 0.012f, 0.012f), new Vector3(0, 0.034f, 0.035f), new Color(0.15f, 0.15f, 0.17f));
        foreach (var x in new[] { -0.05f, 0.05f })   // plate braces down to the frame's lower front corner
            VisualKit.Place(VisualKit.Rod(body, "esc_brace", 0.003f, Carbon), new Vector3(x, -0.1f, -0.13f), new Vector3(x, -0.02f, -0.245f));
        VisualKit.Box(body, "battery", new Vector3(0.09f, 0.065f, 0.2f), new Vector3(0, -0.14f, -0.02f), new Color(0.2f, 0.2f, 0.22f));
        VisualKit.Box(body, "battery_label", new Vector3(0.091f, 0.02f, 0.12f), new Vector3(0, -0.14f, -0.02f), new Color(0.85f, 0.7f, 0.15f));
        foreach (var z in new[] { -0.08f, 0.04f })   // straps
            VisualKit.Box(body, "battery_strap", new Vector3(0.094f, 0.069f, 0.012f), new Vector3(0, -0.14f, z), new Color(0.1f, 0.1f, 0.1f));
        // open frame: each side plate as its outline (4 edges + a diagonal), cross members at the corners
        const float y0 = -0.1f, y1 = 0.07f, z0 = -0.13f, z1 = 0.12f;
        foreach (var x in new[] { -0.06f, 0.06f })
        {
            Vector3 a = new(x, y0, z0), b = new(x, y0, z1), c = new(x, y1, z1), d = new(x, y1, z0);
            foreach (var (p, q) in new[] { (a, b), (b, c), (c, d), (d, a), (a, c) })
                VisualKit.Place(VisualKit.Rod(body, "frame_edge", 0.003f, Carbon), p, q);
        }
        foreach (var (y, z) in new[] { (y0, z0), (y0, z1), (y1, z1) })
            VisualKit.Place(VisualKit.Rod(body, "frame_cross", 0.003f, Carbon), new Vector3(-0.06f, y, z), new Vector3(0.06f, y, z));
        VisualKit.Box(body, "bearing_block", new Vector3(0.12f, 0.012f, 0.05f), new Vector3(0, 0.07f, 0), Alu, 0.6f);
        VisualKit.Cyl(body, "main_shaft", 0.006f, 0.3f, new Vector3(0, 0.07f, 0), Alu, metallic: 0.8f);
        // main gear at the shaft base (turns with the head)
        _gear = AddPivot(root, "main_gear");
        _gear.Position = new Vector3(0, -0.065f, 0);
        VisualKit.Cyl(_gear, "gear", 0.06f, 0.008f, Vector3.Zero, new Color(0.75f, 0.75f, 0.7f), segments: 48);
        VisualKit.Cyl(_gear, "gear_hub", 0.018f, 0.014f, Vector3.Zero, Alu, metallic: 0.7f);
        VisualKit.Box(_gear, "gear_mark", new Vector3(0.05f, 0.009f, 0.008f), new Vector3(0.03f, 0, 0), new Color(0.2f, 0.2f, 0.2f));
        VisualKit.Box(body, "boom_block", new Vector3(0.05f, 0.05f, 0.07f), new Vector3(0, 0.05f, 0.14f), Carbon);
        VisualKit.Cyl(body, "boom", 0.013f, 1.1f, new Vector3(0, 0.05f, 0.12f + 0.55f), Alu, new Vector3(90, 0, 0), 0.7f);
        foreach (var x in new[] { -0.06f, 0.06f }) VisualKit.Place(VisualKit.Rod(body, "boom_support", 0.004f, Carbon), new Vector3(x, -0.09f, 0.1f), new Vector3(0, 0.04f, 0.62f));
        VisualKit.Box(body, "h_stab", new Vector3(0.22f, 0.003f, 0.06f), new Vector3(0, 0.05f, 0.95f), Carbon);
        VisualKit.Box(body, "v_fin", new Vector3(0.003f, 0.18f, 0.1f), new Vector3(0, -0.05f, 1.23f), Carbon);
        VisualKit.Box(body, "v_fin_tip", new Vector3(0.004f, 0.03f, 0.1f), new Vector3(0, -0.125f, 1.23f), Canopy);
        VisualKit.Box(body, "tail_gearbox", new Vector3(0.03f, 0.04f, 0.05f), new Vector3(0, 0.05f, 1.235f), Alu, 0.6f);
        VisualKit.Cyl(body, "tail_shaft", 0.004f, 0.04f, new Vector3(-0.02f, 0.05f, 1.25f), Alu, new Vector3(0, 0, 90), 0.8f);
        foreach (var x in new[] { -0.14f, 0.14f })
        {
            VisualKit.Cyl(body, "skid", 0.006f, 0.56f, new Vector3(x, -0.194f, 0.02f), Carbon, new Vector3(90, 0, 0), segments: 10);
            foreach (var z in new[] { -0.13f, 0.12f })
                VisualKit.Place(VisualKit.Rod(body, "strut", 0.006f, Carbon), new Vector3(x, -0.194f, z), new Vector3(x * 0.43f, -0.1f, z));
        }

        // ---- swash servos: upright cases, horn tips under the plate balls, output shafts horizontal ----
        _pushLen = _plateBase.Y - N(Swash[0]).Position.Y;       // horn ball straight below the plate ball at neutral
        for (int i = 0; i < 3; i++)
        {
            var arm = N(Swash[i]);                                   // = the output shaft centre
            _arms[i] = arm;
            var p = arm.Position;
            var horn = arm.HasMeta("horn") ? arm.GetMeta("horn").AsVector3().Normalized() : Vector3.Back;   // neutral horn direction (horizontal)
            var ball = p + horn * ArmLen;
            _u[i] = new Vector3(ball.X, 0, ball.Z).Normalized();    // plate ball azimuth
            var shaftAxis = horn.Cross(Vector3.Up);
            _armRest[i] = new Basis(horn, Vector3.Up, shaftAxis);    // local x = horn, local z = output shaft
            arm.Basis = _armRest[i];
            var caseDir = arm.HasMeta("case") ? arm.GetMeta("case").AsVector3().Normalized() : shaftAxis;
            float cs = Mathf.Sign(caseDir.Dot(shaftAxis)); if (cs == 0) cs = 1;
            // case: 20 mm along the horn, 40 mm tall (shaft 10 mm below the top), 36 mm along the shaft, beside the horn
            VisualKit.Box(root, Swash[i] + "_servo", new Vector3(0.02f, 0.04f, 0.036f), p + _armRest[i] * new Vector3(0, -0.01f, cs * 0.022f), SwashColor[i].Darkened(0.35f)).Basis = _armRest[i];
            VisualKit.Box(root, Swash[i] + "_servo_ears", new Vector3(0.026f, 0.052f, 0.004f), p + _armRest[i] * new Vector3(0, -0.01f, cs * 0.034f), SwashColor[i].Darkened(0.5f)).Basis = _armRest[i];
            VisualKit.Cyl(root, Swash[i] + "_output_shaft", 0.003f, 0.008f, p + _armRest[i] * new Vector3(0, 0, cs * 0.003f), Alu, default, 0.8f).Basis = _armRest[i] * new Basis(Vector3.Right, Mathf.Pi / 2);
            // mounting: side servos on a post of the side-plate outline, the rear one on a cross member
            var earsAt = p + _armRest[i] * new Vector3(0, 0, cs * 0.034f);
            if (Mathf.Abs(p.X) > 0.02f)
                VisualKit.Place(VisualKit.Rod(body, "servo_post", 0.003f, Carbon), new Vector3(Mathf.Sign(p.X) * SidePlateX, -0.1f, earsAt.Z), new Vector3(Mathf.Sign(p.X) * SidePlateX, 0.07f, earsAt.Z));
            else
                VisualKit.Place(VisualKit.Rod(body, "servo_cross", 0.003f, Carbon), new Vector3(-SidePlateX, p.Y - 0.032f, p.Z), new Vector3(SidePlateX, p.Y - 0.032f, p.Z));
            // horn: hub disc on the shaft + arm to the ball link (rotates with the shaft)
            VisualKit.Cyl(arm, "horn_hub", 0.007f, 0.004f, Vector3.Zero, SwashColor[i], new Vector3(90, 0, 0));
            VisualKit.Box(arm, "horn_arm", new Vector3(ArmLen + 0.004f, 0.006f, 0.004f), new Vector3(ArmLen / 2, 0, 0), SwashColor[i]);
            arm.AddChild(new MeshInstance3D { Name = "horn_ball", Mesh = new SphereMesh { Radius = 0.0035f, Height = 0.007f }, Position = new Vector3(ArmLen, 0, 0), MaterialOverride = VisualKit.Mat(Alu, 0.8f, 0.3f) });
            _pushrods[i] = VisualKit.Rod(root, Swash[i] + "_pushrod", 0.0025f, Alu);
            VisualKit.Label(root, Swash[i], p - horn * 0.05f + new Vector3(0, 0.05f, 0), SwashColor[i]);
        }

        // ---- swashplate ----
        VisualKit.Cyl(_plate, "lower_ring", BallR + 0.004f, 0.008f, new Vector3(0, -0.005f, 0), Alu, metallic: 0.7f);
        VisualKit.Cyl(_plate, "upper_ring", 0.036f, 0.008f, new Vector3(0, 0.005f, 0), new Color(0.8f, 0.15f, 0.12f), metallic: 0.5f);
        for (int i = 0; i < 3; i++)
            _plate.AddChild(new MeshInstance3D { Name = "ball_" + Swash[i], Mesh = new SphereMesh { Radius = 0.005f, Height = 0.01f }, Position = _u[i] * BallR, MaterialOverride = VisualKit.Mat(SwashColor[i]) });

        // ---- main rotor: hub, grips, two blades (tip tape on one), pitch links ----
        VisualKit.Box(_rotor, "hub", new Vector3(0.09f, 0.02f, 0.025f), Vector3.Zero, Alu, 0.7f);
        VisualKit.Cyl(_rotor, "head_block", 0.014f, 0.03f, Vector3.Zero, Carbon);
        for (int b = 0; b < 2; b++)
        {
            var grip = AddPivot(_rotor, "grip_" + (b + 1));
            grip.RotationDegrees = new Vector3(0, b * 180, 0);
            _grips[b] = grip;
            var pitch = AddPivot(grip, "blade_" + (b + 1));
            pitch.Position = new Vector3(0.045f, 0, 0);
            _bladePitch[b] = pitch;
            VisualKit.Box(pitch, "grip", new Vector3(0.05f, 0.022f, 0.022f), new Vector3(0.01f, 0, 0), Alu, 0.7f);
            VisualKit.Box(pitch, "pitch_horn", new Vector3(0.008f, 0.006f, 0.028f), new Vector3(-0.01f, -0.006f, 0.02f), Alu, 0.7f);
            float span = RotorR - 0.065f;
            VisualKit.Box(pitch, "blade", new Vector3(span, 0.01f, 0.062f), new Vector3(span / 2 + 0.02f, 0, 0), Blade);
            VisualKit.Box(pitch, "tip", new Vector3(0.05f, 0.011f, 0.063f), new Vector3(span - 0.005f, 0, 0), b == 0 ? new Color(1f, 0.85f, 0.1f) : new Color(0.9f, 0.9f, 0.9f));
            _pitchLinks[b] = VisualKit.Rod(root, "pitch_link_" + (b + 1), 0.002f, Alu);
        }

        // ---- tail rotor (left side), tail pitch arm, tail servo on the boom ----
        VisualKit.Cyl(_tailRotor, "tail_hub", 0.008f, 0.02f, Vector3.Zero, Alu, new Vector3(0, 0, 90), 0.7f);
        for (int b = 0; b < 2; b++)
        {
            var holder = AddPivot(_tailRotor, "tail_grip_" + (b + 1));
            holder.RotationDegrees = new Vector3(b * 180, 0, 0);
            var pitch = AddPivot(holder, "tail_blade_" + (b + 1));
            _tailPitch[b] = pitch;
            VisualKit.Box(pitch, "tail_blade", new Vector3(0.003f, TailR - 0.02f, 0.026f), new Vector3(0, (TailR - 0.02f) / 2 + 0.02f, 0), Blade);
            VisualKit.Box(pitch, "tail_tip", new Vector3(0.004f, 0.02f, 0.027f), new Vector3(0, TailR - 0.01f, 0), b == 0 ? new Color(1f, 0.85f, 0.1f) : new Color(0.9f, 0.9f, 0.9f));
        }
        // tail servo on the right side plate behind the main shaft, under the boom: shaft across the frame,
        // long horn standing up, swinging fore/aft; pushrod aft under the boom
        var ts = _tailServo.Position;
        VisualKit.Box(root, "tail_servo_case", new Vector3(SidePlateX - ts.X - 0.004f, 0.04f, 0.02f), new Vector3((ts.X + 0.004f + SidePlateX) / 2, ts.Y - 0.01f, ts.Z), ServoCase);
        VisualKit.Box(root, "tail_servo_ears", new Vector3(0.004f, 0.052f, 0.024f), new Vector3(SidePlateX, ts.Y - 0.01f, ts.Z), ServoCase.Darkened(0.3f));
        VisualKit.Place(VisualKit.Rod(body, "servo_post", 0.003f, Carbon), new Vector3(SidePlateX, -0.1f, ts.Z), new Vector3(SidePlateX, 0.07f, ts.Z));
        VisualKit.Cyl(root, "tail_servo_shaft", 0.003f, 0.006f, ts + new Vector3(0.003f, 0, 0), Alu, new Vector3(0, 0, 90), 0.8f);
        VisualKit.Cyl(_tailServo, "horn_hub", 0.006f, 0.004f, Vector3.Zero, TailYellow, new Vector3(0, 0, 90));
        VisualKit.Box(_tailServo, "horn_arm", new Vector3(0.004f, TailHorn + 0.004f, 0.006f), new Vector3(0, TailHorn / 2, 0), TailYellow);
        _tailRod = VisualKit.Rod(root, "tail_pushrod", 0.0018f, Alu);
        VisualKit.Label(root, "tail_servo", _tailServo.Position + new Vector3(0.05f, -0.03f, 0), TailYellow);
        // bell crank at the tail case (`tail` node, pivot vertical): arm 1 (+x) takes the pushrod, arm 2 (+z) drives the slider
        VisualKit.Cyl(_tail, "crank_pivot", 0.004f, 0.01f, Vector3.Zero, Alu, default, 0.8f);
        VisualKit.Box(_tail, "crank_in", new Vector3(CrankIn + 0.004f, 0.004f, 0.006f), new Vector3(CrankIn / 2, 0, 0), TailYellow);
        VisualKit.Box(_tail, "crank_out", new Vector3(0.006f, 0.004f, CrankOut + 0.004f), new Vector3(0, 0, CrankOut / 2), TailYellow);
        VisualKit.Label(root, "tail", _tail.Position + new Vector3(0.05f, -0.03f, 0), TailYellow);
        _slider = AddPivot(root, "tail_slider");
        VisualKit.Cyl(_slider, "ring", 0.008f, 0.006f, Vector3.Zero, TailYellow, new Vector3(0, 0, 90), 0.5f);
        _sliderLink = VisualKit.Rod(root, "tail_slider_link", 0.0015f, Alu);

        // ---- motor ahead of the main shaft, axis vertical, finned can (spins), pinion on the main gear ----
        VisualKit.Cyl(root, "motor_mount", 0.036f, 0.004f, _motor.Position + new Vector3(0, -0.04f, 0), Carbon);
        VisualKit.Cyl(_motor, "can", 0.032f, 0.075f, Vector3.Zero, new Color(0.62f, 0.63f, 0.66f), metallic: 0.6f, segments: 32);
        for (int k = 0; k < 6; k++)
            VisualKit.Cyl(_motor, "fin", 0.0345f, 0.004f, new Vector3(0, -0.028f + k * 0.011f, 0), new Color(0.45f, 0.46f, 0.5f), metallic: 0.6f, segments: 32);
        VisualKit.Box(_motor, "stripe", new Vector3(0.006f, 0.07f, 0.01f), new Vector3(0.034f, 0, 0), new Color(0.9f, 0.3f, 0.2f));
        VisualKit.Cyl(_motor, "top_cap", 0.02f, 0.004f, new Vector3(0, 0.039f, 0), new Color(0.2f, 0.2f, 0.22f));
        VisualKit.Cyl(_motor, "motor_shaft", 0.003f, 0.03f, new Vector3(0, -0.05f, 0), Alu, metallic: 0.8f);
        VisualKit.Cyl(_motor, "pinion", 0.008f, 0.01f, new Vector3(0, _gear.Position.Y - _motor.Position.Y, 0), Alu, metallic: 0.8f, segments: 12);
        VisualKit.Label(root, "motor", _motor.Position + new Vector3(0, 0.06f, -0.03f), new Color(0.9f, 0.9f, 0.9f));

        Tick(0);
    }

    static Node3D AddPivot(Node3D parent, string name) { var n = new Node3D { Name = name }; parent.AddChild(n); return n; }

    public void ApplyState(JsonElement s)
    {
        if (s.TryGetProperty("rpm", out var r) && r.ValueKind == JsonValueKind.Number) _rpm = r.GetDouble();
        _motorRpm = s.TryGetProperty("motor_rpm", out var mr) && mr.ValueKind == JsonValueKind.Number ? mr.GetDouble() : -1;
        for (int i = 0; i < 3; i++) _swash[i] = VisualKit.Actuator(s, Swash[i]);
        _tailCmd = VisualKit.Actuator(s, "tail");
    }

    public void Tick(double delta)
    {
        // spin (main rotor clockwise seen from above = negative about +Y)
        double w = _rpm / 60.0 * Math.Tau;
        double wm = _motorRpm > 0 ? _motorRpm / 60.0 * Math.Tau : w * MotorRatio;
        _rotAng = (_rotAng - w * delta) % Math.Tau; _tailAng = (_tailAng + w * TailRatio * delta) % Math.Tau; _motAng = (_motAng + wm * delta) % Math.Tau;
        _rotor.Basis = new Basis(Vector3.Up, (float)_rotAng);
        _gear.Basis = new Basis(Vector3.Up, (float)_rotAng);
        _tailRotor.Basis = new Basis(Vector3.Right, (float)_tailAng);
        _motor.Basis = new Basis(Vector3.Up, (float)_motAng);

        // horns rotate on the output shafts -> horn balls on arcs -> rigid pushrods -> ball heights -> plane
        float h0 = 0; var g = Vector3.Zero;
        for (int i = 0; i < 3; i++)
        {
            float th = Mathf.DegToRad((float)_swash[i] * MaxArmDeg);
            _arms[i].Basis = _armRest[i] * new Basis(Vector3.Back, th);   // about local z = the output shaft
            var tip = _arms[i].Transform * new Vector3(ArmLen, 0, 0);
            var ballXz = _u[i] * BallR;
            float off2 = new Vector2(tip.X - ballXz.X, tip.Z - ballXz.Z).LengthSquared();
            float h = tip.Y + Mathf.Sqrt(Mathf.Max(0f, _pushLen * _pushLen - off2)) - _plateBase.Y;
            h0 += h / 3f;
            g += _u[i] * h;
        }
        g *= 2f / (3f * BallR);
        var n = new Vector3(-g.X, 1f, -g.Z).Normalized();
        _plate.Transform = new Transform3D(new Basis(new Quaternion(Vector3.Up, n)), _plateBase + new Vector3(0, h0, 0));
        for (int i = 0; i < 3; i++)
            VisualKit.Place(_pushrods[i], _arms[i].Transform * new Vector3(ArmLen, 0, 0), _plate.Transform * (_u[i] * BallR));

        // blade pitch from the plate height under each pitch link (links on the leading side, +z of the blade)
        float hMax = ArmLen * Mathf.Sin(Mathf.DegToRad(MaxArmDeg));
        for (int b = 0; b < 2; b++)
        {
            var horn = _rotor.Transform * (_grips[b].Transform * (_bladePitch[b].Position + new Vector3(-0.01f, -0.008f, 0.032f)));
            var dir = new Vector3(horn.X, 0, horn.Z).Normalized();
            var bottom = _plate.Transform * (dir * 0.036f + new Vector3(0, 0.008f, 0));
            float hLink = bottom.Y - _plateBase.Y - 0.008f;
            float pitchDeg = Mathf.Clamp(hLink / hMax, -1.5f, 1.5f) * MaxPitchDeg;
            _bladePitch[b].Rotation = new Vector3(-Mathf.DegToRad(pitchDeg), 0, 0);   // leading edge (+z) up
            VisualKit.Place(_pitchLinks[b], bottom, horn);
        }

        // tail: horn swings fore/aft about the servo shaft (x), pushrod under the boom, bell crank turns, slider moves
        float tt = Mathf.DegToRad((float)_tailCmd * MaxArmDeg);
        _tailServo.Rotation = new Vector3(tt, 0, 0);                   // horn up, +tt moves the ball aft (+z)
        var hornTip = _tailServo.Transform * new Vector3(0, TailHorn, 0);
        float dz = hornTip.Z - _tailServo.Position.Z;                  // pushrod travel
        float phi = -Mathf.Asin(Mathf.Clamp(dz / CrankIn, -1f, 1f));   // crank arm 1 (+x) follows the rod
        _tail.Rotation = new Vector3(0, phi, 0);
        VisualKit.Place(_tailRod, hornTip, _tail.Transform * new Vector3(CrankIn, 0, 0));
        var crankOut = _tail.Transform * new Vector3(0, 0, CrankOut);
        _slider.Position = new Vector3(-0.024f + (crankOut.X - _tail.Position.X) * 0.5f, _tailRotor.Position.Y, _tailRotor.Position.Z);
        VisualKit.Place(_sliderLink, crankOut, _slider.Position + new Vector3(0, -0.008f, 0));
        float tp = Mathf.DegToRad((float)_tailCmd * MaxTailPitchDeg);
        for (int b = 0; b < 2; b++) _tailPitch[b].Rotation = new Vector3(0, b == 0 ? tp : -tp, 0);
    }
}
