// Bench functions and their binds: one table (Settings → Keys edits it, the flight screen dispatches
// through it). Three groups (Settings → Keys shows them in this order): Sticks (the keyboard layer over channels 1..4,
// RcInput: held keys, the collective an integrator), Transmitter switches (switch_N steps the N-th Transmitter row),
// Application (everything else). A bind is a key with modifiers as Godot names it ("F5", "Space", "Ctrl+N", "1") or a
// joypad button ("Joy button 3": any pad, so a radio switch in USB-joystick mode can fire a function).
// Stored in AppSettings.Keys by function id; a missing id = its default, "" = unbound (Clear).
// Fault functions come from the manifest: id "fault:<device>.<kind>", unbound by default.
using System.Collections.Generic;
using System.Linq;
using Godot;

namespace FlightBench;

public sealed record BenchFunction(string Id, string Label, string DefaultBind);

/// A stick function: the channel it moves (0-based: roll, pitch, collective, yaw = channels 1..4 as the Transmitter
/// window shows them) and the direction. Index in Keybinds.Sticks = channel · 2 + (plus ? 1 : 0).
public sealed record StickFunction(string Id, int Channel, int Sign, string Axis);

public static class Keybinds
{
    /// The flight screen's windows (Windows menu order); each has a "window:<name>" toggle function.
    public static readonly string[] Windows = { "Status", "Aircraft", "Transmitter", "Log / CLI", "Firmware", "Signals", "Wiring", "Model", "Faults", "OSD", "Functions", "Config" };

    /// The keyboard layer's stick keys (RcInput): roll / pitch / yaw spring back to the centre on release; the
    /// collective is an integrator (W / S move it, the value holds when released).
    public static readonly StickFunction[] Sticks =
    {
        new("stick_roll_minus", 0, -1, "roll"), new("stick_roll_plus", 0, +1, "roll"),
        new("stick_pitch_minus", 1, -1, "pitch"), new("stick_pitch_plus", 1, +1, "pitch"),
        new("stick_collective_minus", 2, -1, "collective"), new("stick_collective_plus", 2, +1, "collective"),
        new("stick_yaw_minus", 3, -1, "yaw"), new("stick_yaw_plus", 3, +1, "yaw"),
    };

    /// The index of a stick function in Sticks, or -1.
    public static int StickIndex(string id) => System.Array.FindIndex(Sticks, s => s.Id == id);

    public enum Group { Sticks, Switches, Application }
    public static Group GroupOf(string id) => id.StartsWith("stick_") ? Group.Sticks : id.StartsWith("switch_") ? Group.Switches : Group.Application;
    public static string GroupTitle(Group g) => g switch { Group.Sticks => "Sticks", Group.Switches => "Transmitter switches", _ => "Application" };

    private static readonly BenchFunction[] Fixed =
    {
        // sticks: channel order as the Transmitter window shows it (1 roll, 2 pitch, 3 collective, 4 yaw)
        new("stick_roll_minus", "Roll left (ch1 −)", "Left"),
        new("stick_roll_plus", "Roll right (ch1 +)", "Right"),
        new("stick_pitch_minus", "Pitch back (ch2 −)", "Down"),
        new("stick_pitch_plus", "Pitch forward (ch2 +)", "Up"),
        new("stick_collective_minus", "Collective / throttle down (ch3 −; integrator, holds)", "S"),
        new("stick_collective_plus", "Collective / throttle up (ch3 +; integrator, holds)", "W"),
        new("stick_yaw_minus", "Yaw left (ch4 −)", "A"),
        new("stick_yaw_plus", "Yaw right (ch4 +)", "D"),
        // keyboard as the transmitter's switches: switch N steps the N-th row of the Transmitter window (channel order: the
        // bench's static switches always, the ones derived from the firmware's modes once it gave ranges) to its next
        // position; on the RS7 5 = ARM (ch5), 6 = throttle (ch6). Settings over a bench names the row (SwitchLabel).
        new("switch_1", "Switch 1", "5"),
        new("switch_2", "Switch 2", "6"),
        new("switch_3", "Switch 3", "7"),
        new("switch_4", "Switch 4", "8"),
        new("switch_5", "Switch 5", "9"),
        new("restart_bench", "Restart bench", "F5"),
        new("pause", "Pause / run", "Space"),
        new("step", "Step (10 ms)", "N"),
        new("battery", "Battery off / on", "B"),
        new("reset_aircraft", "Reset aircraft", "R"),
        new("screenshot", "Screenshot", "F12"),
        new("camera_pad", "Camera 1: pad", "1"),
        new("camera_orbit", "Camera 2: orbit", "2"),
        new("camera_fpv", "Camera 3: FPV", "3"),
        new("camera_chase", "Camera 4: chase", "4"),
        new("body_axes", "Body axes + actuator labels", "X"),
        new("launch", "Launch (plane: hand / rail launch)", "L"),
    };

    /// Switch functions by number (1-based) and the ids older settings files stored their binds under.
    public const int SwitchKeys = 5;
    public static string SwitchId(int n) => $"switch_{n}";
    private static readonly Dictionary<string, string> Legacy = new() { ["switch_1"] = "switch_arm", ["switch_2"] = "switch_mode", ["switch_3"] = "switch_aux", ["window:Config"] = "window:Firmware config" };

    public static string FaultId(string device, string kind) => $"fault:{device}.{kind}";
    public static string WindowId(string name) => "window:" + name;

    /// Every function: fixed ones, window toggles, one per fault of the manifest.
    public static List<BenchFunction> All(Manifest m)
    {
        var list = new List<BenchFunction>(Fixed);
        list.AddRange(Windows.Select(w => new BenchFunction(WindowId(w), $"window: {w}", "")));
        list.AddRange(m.AllFaults().Select(x => new BenchFunction(FaultId(x.Device.Id, x.Fault.Id), $"fault: {x.Device.Id} {x.Fault.Id}", "")));
        return list;
    }

    private static string DefaultOf(string id) => System.Array.Find(Fixed, f => f.Id == id)?.DefaultBind ?? "";

    /// The bind of a function: its own, else the one an older settings file stored under the legacy id, else the default.
    public static string Bind(string id)
    {
        var keys = AppSettings.Current.Keys;
        if (keys.TryGetValue(id, out var v)) return v ?? "";
        if (Legacy.TryGetValue(id, out var old) && keys.TryGetValue(old, out var ov)) return ov ?? "";
        return DefaultOf(id);
    }

    /// Sets a bind; the same bind on another function is cleared there (one bind = one function).
    public static void Set(string id, string bind, Manifest m)
    {
        var keys = AppSettings.Current.Keys;
        if (bind != "")
            foreach (var f in All(m))
                if (f.Id != id && Bind(f.Id) == bind) keys[f.Id] = "";
        keys[id] = bind;
    }

    public static void Clear(string id) => AppSettings.Current.Keys[id] = "";

    /// The Transmitter window's hint line, built from the bind table (never a literal): the stick pairs as
    /// "minus/plus axis", the switch keys, the launch key on a plane bench. Unbound keys show as "—".
    public static string Hint(bool plane)
    {
        string B(string id) => Bind(id) is { Length: > 0 } b ? b : "—";
        var parts = new List<string>();
        for (int i = 0; i < Sticks.Length; i += 2)
            parts.Add($"{B(Sticks[i].Id)}/{B(Sticks[i + 1].Id)} {Sticks[i].Axis}{(Sticks[i].Channel == 2 ? " (holds)" : "")}");
        var sw = string.Join(" ", Enumerable.Range(1, SwitchKeys).Select(n => Bind(SwitchId(n))).Where(k => k != ""));
        if (sw != "") parts.Add("switches " + sw);
        if (plane && Bind("launch") is { Length: > 0 } l) parts.Add("launch " + l);
        return "keys: " + string.Join(" · ", parts);
    }

    /// What releases a held stick function: the key without modifiers ("W") or the joypad button ("Joy button 3");
    /// the same for the press and its release (null for echoes and everything else).
    public static string? HoldToken(InputEvent e)
    {
        switch (e)
        {
            case InputEventKey k when !k.Echo:
                var code = k.Keycode != Key.None ? k.Keycode : k.PhysicalKeycode;
                return code is Key.None or Key.Shift or Key.Ctrl or Key.Alt or Key.Meta ? null : OS.GetKeycodeString(code);
            case InputEventJoypadButton b:
                return $"Joy button {(int)b.ButtonIndex}";
            default:
                return null;
        }
    }

    /// " (F5)" for button labels, "" when unbound.
    public static string Suffix(string id) => Bind(id) is { Length: > 0 } b ? $" ({b})" : "";

    /// The bind name of a press (key with modifiers, or a joypad button); null for releases, echoes,
    /// bare modifiers and everything else.
    public static string? EventName(InputEvent e)
    {
        switch (e)
        {
            case InputEventKey k when k.Pressed && !k.Echo:
            {
                var code = k.Keycode != Key.None ? k.Keycode : k.PhysicalKeycode;
                if (code is Key.None or Key.Shift or Key.Ctrl or Key.Alt or Key.Meta) return null;
                return OS.GetKeycodeString((Key)((long)code | (long)k.GetModifiersMask()));
            }
            case InputEventJoypadButton b when b.Pressed:
                return $"Joy button {(int)b.ButtonIndex}";
            default:
                return null;
        }
    }

    /// The function bound to this press, if any.
    public static string? Match(InputEvent e, Manifest m)
    {
        var name = EventName(e);
        if (name == null) return null;
        // an explicit bind (Settings → Keys) wins over a default one on the same key: an older settings file may have put a
        // function on a key that became a stick default later (A, D, W, S, the arrows)
        var all = All(m);
        var keys = AppSettings.Current.Keys;
        foreach (var f in all) if (keys.ContainsKey(f.Id) && Bind(f.Id) == name) return f.Id;
        foreach (var f in all) if (Bind(f.Id) == name) return f.Id;
        return null;
    }

    /// A key bind as a Key (for menu accelerators); None for joypad binds / unbound.
    public static Key AsKey(string bind) => bind == "" || bind.StartsWith("Joy ") ? Key.None : OS.FindKeycodeFromString(bind);
}
