// Transmitter as a joystick → 16 channel values in µs (index i = CRSF channel i+1), sent to the core
// as `rc`. The app is the "receiver": all switch logic stays in the radio's model. Per-channel
// source / calibration / reverse from AppSettings.Transmitter (sources name the joypad only, or the Transmitter window):
//   none            → 1500, unless a Transmitter row or the keys drive it
//   axis <n>        → joypad axis n (−1..1)
//   button <n>      → pressed = high, else low
//   buttons <n>,<m> → 3-position: n = low, m = high, neither (or both) = centre
//   ui              → the Transmitter window's switch for this channel (Override: a static switch of the bench's
//                     transmitter model or one derived from the firmware's modes; also on a `none` channel); else 1500
// (`keyboard` of older settings files reads as `none`, TransmitterSettings.Normalize.)
// The keyboard is a layer, not a source (decisions 2026-10-23): always on over channels 1..4 (except while a text field
// has the keys or KeysOff), in parallel with the joypad. Its keys are the stick functions of the bind table
// (Keybinds.Sticks, Settings → Keys; dispatched by the flight screen's HandleKeys → StickKey / StickRelease):
// roll / pitch / yaw = −1 / 0 / +1 while held (centre on release), the collective an integrator (1 / s while held,
// the value holds). Ownership (Owner[], shown on the Transmitter window's bars): the last input that changed a channel
// holds it — a joypad source takes it when its value moves by more than JoyTakeUs (100 µs, a deliberate move: a stick's
// slow drift never takes a channel from the keys) from where the pad was when it last held it, a key takes it when
// pressed or released. When a key takes the collective from another input the integrator starts from the channel's value (W nudges
// from where the stick left it). No input holds a channel → a Transmitter row's value if it has one, else on 1..4
// the key layer at rest (centre; the collective's integrator, which starts at its low end as before), else 1500.
// A pad that appears (start, hot-plug, replug after a loss) takes its channels, keys held or not (owner, 2026-10-24: a
// development bench, a restart is cheap; not worth a rule).
// Pipeline for axis/button/ui (RcMath.Map): raw µs = 1500 + 500·value → normalise with the channel's
// min/centre/max (two segments) → deadzone (axis only) → reverse → 1500 + 500·n, clamped 800..2200.
// Keyboard values are already in channel space (up = forward = high) and skip calibration/reverse.
// A named radio that is not plugged in means no joypad (the keys / centre), never another pad.
// Radio lost (flight screen's reader, TransmitterSettings.FailLinkOnDisconnect, default on): the named radio was being read and
// disappears → RadioLost; the channels hold their last values (a receiver hearing nothing; the keys not read either)
// and TakeLinkChange() reports it once, so the flight screen fails the core's receiver link;
// when the radio is back, the first Update reads it and TakeLinkChange() reports the return. A radio that
// is absent from the start is not a loss (the keys / centre as before).
// Pitch sign: no hard-coded inversion any more. EdgeTX outputs CH2 = +100 % for elevator stick forward
// and its USB joystick reports that as the axis maximum (+1) — what RotorFlight wants (high = forward),
// so the EdgeTX preset has no reverse. A gamepad reports stick-up as −1, so the gamepad-style default
// (and old settings files, which had the inversion in code) get Reverse on channel 2 instead.
using System;
using System.Diagnostics;
using Godot;

namespace FlightBench;

/// Where joypad values come from: Godot's Input by default; DevHooks swaps in a fake (FB_FAKE_JOY=1).
public interface IJoyReader
{
    int[] Pads();
    string Name(int device);
    float Axis(int device, int axis);
    bool Button(int device, int button);
}

public sealed class GodotJoyReader : IJoyReader
{
    public int[] Pads() { var a = Input.GetConnectedJoypads(); var r = new int[a.Count]; for (int i = 0; i < r.Length; i++) r[i] = a[i]; return r; }
    public string Name(int device) => Input.GetJoyName(device);
    public float Axis(int device, int axis) => Input.GetJoyAxis(device, (JoyAxis)axis);
    public bool Button(int device, int button) => Input.IsJoyButtonPressed(device, (JoyButton)button);
}

public static class Joy
{
    public static IJoyReader Reader = new GodotJoyReader();
    public const int Axes = 10, Buttons = 32;
    /// Raised on hot-plug (Godot's Input.JoyConnectionChanged; the fake raises it once at start).
    public static event Action? Changed;
    private static bool _hooked;
    public static void Hook()
    {
        if (_hooked) return;
        _hooked = true;
        Input.JoyConnectionChanged += (device, connected) =>
        {
            GD.Print($"[rc] joypad {device} {(connected ? "connected" : "disconnected")}: {Input.GetJoyName((int)device)}");
            Changed?.Invoke();
        };
    }
    public static void RaiseChanged() => Changed?.Invoke();
    public static bool IsConnected(int device) => Array.IndexOf(Reader.Pads(), device) >= 0;
}

/// A parsed channel source ("axis 3", "buttons 1,2", …).
public readonly record struct RcSource(string Kind, int A = 0, int B = 0)
{
    public static RcSource Parse(string? s)
    {
        s = (s ?? "").Trim().ToLowerInvariant();
        var parts = s.Split(' ', 2, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        string kind = parts.Length > 0 ? parts[0] : "none";
        string arg = parts.Length > 1 ? parts[1] : "";
        switch (kind)
        {
            case "axis" when int.TryParse(arg, out var n) && n >= 0 && n < Joy.Axes: return new("axis", n);
            case "button" when int.TryParse(arg, out var n) && n >= 0 && n < Joy.Buttons: return new("button", n);
            case "buttons":
                var ab = arg.Split(',', StringSplitOptions.TrimEntries);
                if (ab.Length == 2 && int.TryParse(ab[0], out var a) && int.TryParse(ab[1], out var b)
                    && a >= 0 && b >= 0 && a < Joy.Buttons && b < Joy.Buttons) return new("buttons", a, b);
                break;
            case "ui": return new("ui");
        }
        return new("none");
    }
    public override string ToString() => Kind switch { "axis" => $"axis {A}", "button" => $"button {A}", "buttons" => $"buttons {A},{B}", _ => Kind };
}

/// Who last drove a channel (the Transmitter window's bar marks: `joy` / `kb` / `ui`; None = nobody yet).
public enum RcOwner : byte { None, Joy, Kb, Ui }

public sealed class RcInput : IDisposable
{
    public readonly ushort[] ChannelsUs = new ushort[16];
    /// The Transmitter window's switch on that channel (Flight.Modes.cs): the us to send (0 = none). Taken on channels
    /// whose source is `ui` or `none` (nothing else drives it).
    public readonly ushort[] Override = new ushort[16];
    /// Who set Override on a channel: Ui (a click on the row) or Kb (its switch key); the bar's mark.
    public readonly RcOwner[] OverrideBy = new RcOwner[16];
    /// Who last drove each channel (the ownership rule above).
    public readonly RcOwner[] Owner = new RcOwner[16];
    /// No keyboard sticks while set (the flight screen: Settings is open over the bench, its keys are for it).
    public bool KeysOff;
    private float _kbThrottle = -1f;         // -1..1: the collective's integrator (starts low)
    /// A joypad source takes its channel back when it moved more than this (µs) from where it was when it last held it:
    /// a tenth of the stick's travel, a deliberate move (owner, 2026-10-24; 20 µs let a stick without a deadzone drift
    /// the collective away from the keys mid-climb).
    public const int JoyTakeUs = 100;
    private bool _dirty = true, _rescan = true;
    private readonly Stopwatch _sinceSend = Stopwatch.StartNew();
    public const double MinSendIntervalMs = 4;

    /// Does this reader treat an unplugged radio as a lost link (the flight screen's; Settings previews do not)?
    private readonly bool _linkLoss;

    public RcInput(bool linkLoss = false)
    {
        _linkLoss = linkLoss;
        Array.Fill(ChannelsUs, (ushort)1500);
        Array.Fill(_joyRef, -1);
        Joy.Hook();
        Joy.Changed += OnJoyChanged;
    }

    public void Dispose() => Joy.Changed -= OnJoyChanged;
    private void OnJoyChanged() => _rescan = true;

    /// Normalised stick values -1..1 for the transmitter preview: roll, pitch, yaw, collective.
    public float[] Sticks { get; } = new float[4];

    /// The device actually read (follows the configured name across replugs); -1 = none connected.
    public int ActiveDevice { get; private set; } = -2;       // -2 = not resolved yet

    /// The named radio was read and is gone (FailLinkOnDisconnect): channels hold, the keys not read.
    public bool RadioLost { get; private set; }
    private string? _readName;               // the named radio last read (a loss is that name going missing, not a rename)
    private string? _lostName;               // the radio whose loss is being held
    private bool? _linkChange;               // true = just lost, false = back and read once; taken by TakeLinkChange

    /// Once per change: true = the radio was unplugged, false = it is back and the channels are its values again.
    public bool? TakeLinkChange() { var c = _linkChange; _linkChange = null; return c; }

    // parsed sources, re-parsed when the settings' string (or the array) changes
    private static string?[]? _srcArray;
    private static readonly string?[] _srcText = new string?[16];
    private static readonly RcSource[] _srcParsed = new RcSource[16];

    public static RcSource SourceOf(int ch)
    {
        var arr = AppSettings.Current.Transmitter.Source!;
        var text = arr[ch];
        if (!ReferenceEquals(arr, _srcArray)) { _srcArray = arr; Array.Clear(_srcText); }
        if (!ReferenceEquals(text, _srcText[ch])) { _srcText[ch] = text; _srcParsed[ch] = RcSource.Parse(text); }
        return _srcParsed[ch];
    }

    /// Is this channel's source `ui` (the Transmitter window's switch)?
    public static bool IsUi(int ch) => ch >= 0 && ch < 16 && SourceOf(ch).Kind == "ui";

    /// Which pad to read (on start and hot-plug only): the configured one by name, found at any index; a
    /// named radio that is not connected → none (the keys / centre) and a log line, not some other pad.
    /// With no name yet the first pad is taken and remembered.
    private void ResolveDevice(TransmitterSettings t)
    {
        _rescan = false;
        var pads = Joy.Reader.Pads();
        if (!string.IsNullOrEmpty(t.DeviceName))
        {
            ActiveDevice = -1;
            if (Array.IndexOf(pads, t.Device) >= 0 && Joy.Reader.Name(t.Device) == t.DeviceName) ActiveDevice = t.Device;
            else
                foreach (var p in pads)
                    if (Joy.Reader.Name(p) == t.DeviceName)
                    {
                        GD.Print($"[rc] '{t.DeviceName}' found at index {p} (was {t.Device})");
                        t.Device = p; ActiveDevice = p; break;
                    }
            bool held = _linkLoss && t.FailLinkOnDisconnect && (_readName == t.DeviceName || RadioLost);   // a loss: Update holds the channels
            if (ActiveDevice < 0)
                GD.Print($"[rc] '{t.DeviceName}' not connected{(pads.Length > 0 ? $" ({pads.Length} other pad(s) ignored; pick one in Settings → Transmitter)" : "")}: {(held ? "channels hold (link lost)" : "the keys / centre")}");
            return;
        }
        ActiveDevice = pads.Length == 0 ? -1 : Array.IndexOf(pads, t.Device) >= 0 ? t.Device : pads[0];
        if (ActiveDevice >= 0) { t.Device = ActiveDevice; t.DeviceName = Joy.Reader.Name(ActiveDevice); }
    }

    private readonly float[] _kb = new float[4];
    private readonly string?[] _held = new string?[8];       // per stick function (Keybinds.Sticks): the token that releases it
    private readonly bool[] _kbTook = new bool[4];
    private readonly bool[] _seenDown = new bool[8];         // Input saw the held stick key down (ReleaseLostKeys)           // a stick key was pressed / released on that channel since the last Update
    private readonly int[] _joyRef = new int[16];            // the pad's value (µs) when it last held the channel; -1 = not read
    private bool _joyFresh = true;                            // a pad (re)appeared: it takes its channels nobody holds

    /// A stick function pressed (Keybinds.Sticks index; `token` = Keybinds.HoldToken of the press). Ignored while a text
    /// field has the keys or KeysOff. Returns whether it was taken.
    public bool StickKey(int fn, string? token)
    {
        if (fn < 0 || fn >= _held.Length || KeysOff || Ui.TextFocus()) return false;
        int ch = Keybinds.Sticks[fn].Channel;
        if (ch == 2 && Owner[2] != RcOwner.Kb) _kbThrottle = RcMath.ToNorm(ChannelsUs[2]);   // W nudges from where the channel is
        _held[fn] = token ?? "";
        _kbTook[ch] = true;
        return true;
    }

    /// A key / joypad button released: the stick functions it held let go (the channel taken: centre, the collective
    /// holds). Returns whether it released any.
    public bool StickRelease(string? token)
    {
        if (token == null) return false;
        bool any = false;
        for (int i = 0; i < _held.Length; i++)
            if (_held[i] == token) { ReleaseStick(i); any = true; }
        return any;
    }

    /// Every held stick key lets go (the window lost focus or a text field took the keys: no release will come).
    public void ReleaseKeys() { for (int i = 0; i < _held.Length; i++) if (_held[i] != null) ReleaseStick(i); }

    private void ReleaseStick(int i)
    {
        int ch = Keybinds.Sticks[i].Channel;
        _held[i] = null;
        _seenDown[i] = false;
        // a release changes the channel only while the keys hold it: a pad that moved meanwhile keeps it (the release of a key
        // that no longer drives the channel changes nothing); a press not applied yet (_kbTook, a tap within one frame) stays
        if (Owner[ch] == RcOwner.Kb) _kbTook[ch] = true;
    }

    /// A held stick key whose release went elsewhere (a popup / dialog window had the focus, not the flight view or an
    /// embedded window with InputRelay): Input saw it down and now sees it up → it lets go, with a log line. Holds Input
    /// never saw (the self-tests push events into the viewport) are left to their release event.
    private void ReleaseLostKeys()
    {
        for (int i = 0; i < _held.Length; i++)
        {
            if (_held[i] is not { Length: > 0 } tok || tok.StartsWith("Joy button")) continue;
            var code = OS.FindKeycodeFromString(tok);
            if (code == Key.None) continue;
            if (Input.IsKeyPressed(code) || Input.IsPhysicalKeyPressed(code)) _seenDown[i] = true;
            else if (_seenDown[i])
            {
                GD.Print($"[rc] '{tok}' released outside the flight view: {Keybinds.Sticks[i].Id} lets go");
                ReleaseStick(i);
            }
        }
    }

    /// Is this stick function held now (self-tests)?
    public bool StickHeld(int fn) => fn >= 0 && fn < _held.Length && _held[fn] != null;

    private float HeldAxis(int ch) => (_held[ch * 2 + 1] != null ? 1 : 0) - (_held[ch * 2] != null ? 1 : 0);

    /// The joypad's value for a channel (µs), or -1 when its source is not the joypad or no pad is read.
    private static int JoyUs(TransmitterSettings t, RcSource src, int dev, int ch)
    {
        if (dev < 0) return -1;
        switch (src.Kind)
        {
            case "axis": return RcMath.Map(t, ch, Joy.Reader.Axis(dev, src.A), true);
            case "button": return RcMath.Map(t, ch, Joy.Reader.Button(dev, src.A) ? 1 : -1, false);
            case "buttons":
                bool lo = Joy.Reader.Button(dev, src.A), hi = Joy.Reader.Button(dev, src.B);
                return RcMath.Map(t, ch, lo == hi ? 0 : hi ? 1 : -1, false);
            default: return -1;
        }
    }

    /// Updates ChannelsUs; returns true when any channel changed. Call TakeSend() to rate-limit sending.
    public bool Update(double delta)
    {
        var t = AppSettings.Current.Transmitter;
        if (_rescan)                                       // Joy.Changed (hot-plug) sets it
        {
            int before = ActiveDevice;
            ResolveDevice(t);
            if (ActiveDevice >= 0 && ActiveDevice != before) _joyFresh = true;
        }
        int dev = ActiveDevice;
        bool named = !string.IsNullOrEmpty(t.DeviceName);
        if (dev >= 0) _readName = named ? t.DeviceName : null;
        else if (named && _readName == t.DeviceName && _linkLoss && t.FailLinkOnDisconnect && !RadioLost)
        {
            RadioLost = true; _lostName = t.DeviceName; _readName = null; _linkChange = true;
            GD.Print($"[rc] '{t.DeviceName}' unplugged: receiver link lost, channels hold their last values");
        }
        if (RadioLost)
        {
            if (dev < 0 && t.FailLinkOnDisconnect && t.DeviceName == _lostName) return false;   // hold: nothing heard, nothing read (no keys)
            RadioLost = false;                                    // back (or the option turned off): read it below
            _linkChange = _linkChange == true ? null : false;     // lost and back before anyone looked: no change
            _joyFresh = true;                                     // the radio is back: its channels are its values again
            for (int ch = 0; ch < 16; ch++) if (Owner[ch] == RcOwner.Joy) Owner[ch] = RcOwner.None;
        }
        // the key layer: not while typing (held keys let go) or while KeysOff
        var kb = _kb;
        if (KeysOff || Ui.TextFocus()) ReleaseKeys();
        else ReleaseLostKeys();
        kb[0] = HeldAxis(0);
        kb[1] = HeldAxis(1);
        if (Owner[2] == RcOwner.Kb || _kbTook[2]) _kbThrottle = Mathf.Clamp(_kbThrottle + (float)delta * HeldAxis(2), -1f, 1f);
        kb[2] = _kbThrottle;
        kb[3] = HeldAxis(3);
        bool fresh = _joyFresh && dev >= 0;
        if (fresh) _joyFresh = false;
        bool changed = false;
        for (int ch = 0; ch < 16; ch++)
        {
            var src = SourceOf(ch);
            int joy = JoyUs(t, src, dev, ch);
            var owner = Owner[ch];
            if (joy < 0) { if (owner == RcOwner.Joy) owner = RcOwner.None; _joyRef[ch] = -1; }
            else if (fresh || (_joyRef[ch] < 0 && owner == RcOwner.None)) owner = RcOwner.Joy;   // a pad that appears takes its channels (a joypad source just set: if nobody holds it)
            else if (owner != RcOwner.Joy && _joyRef[ch] >= 0 && Math.Abs(joy - _joyRef[ch]) > JoyTakeUs) owner = RcOwner.Joy;   // the pad moved
            if (ch < 4 && _kbTook[ch]) { owner = RcOwner.Kb; _kbTook[ch] = false; }   // a key pressed / released: the key's
            else if (Owner[ch] == RcOwner.Kb && owner == RcOwner.Joy)                     // shown, not hidden: the pad took a channel from the keys
                GD.Print($"[rc] ch{ch + 1}: the joypad took it from the keys ({(fresh ? "the pad appeared" : $"moved {_joyRef[ch]} → {joy} µs")})");
            if (joy >= 0 && (owner == RcOwner.Joy || _joyRef[ch] < 0)) _joyRef[ch] = joy;   // where the pad is while it holds (or first seen)
            bool ov = Override[ch] != 0 && src.Kind is "ui" or "none";
            var ovBy = OverrideBy[ch] == RcOwner.None ? RcOwner.Ui : OverrideBy[ch];
            ushort us;
            if (owner == RcOwner.Joy) us = (ushort)joy;
            else if (owner == RcOwner.Kb && ch < 4) us = (ushort)RcMath.ToUs(kb[ch]);
            else if (ov) { us = Override[ch]; owner = ovBy; }                       // a Transmitter row (click: ui, its key: kb)
            else { us = ch < 4 ? (ushort)RcMath.ToUs(kb[ch]) : (ushort)1500; owner = RcOwner.None; }   // the key layer at rest / nothing
            Owner[ch] = owner;
            if (ChannelsUs[ch] != us) { ChannelsUs[ch] = us; changed = true; }
        }
        Sticks[0] = Norm(ChannelsUs[0]); Sticks[1] = Norm(ChannelsUs[1]);
        Sticks[2] = Norm(ChannelsUs[3]); Sticks[3] = Norm(ChannelsUs[2]);
        if (changed) _dirty = true;
        return changed;
    }

    /// The bar mark of a channel's owner ("" = nobody yet).
    public static string Mark(RcOwner o) => o switch { RcOwner.Joy => "joy", RcOwner.Kb => "kb", RcOwner.Ui => "ui", _ => "" };

    /// True (once) when a channel changed since the last send and ≥ 4 ms have passed since it.
    public bool TakeSend()
    {
        if (!_dirty || _sinceSend.Elapsed.TotalMilliseconds < MinSendIntervalMs) return false;
        _dirty = false; _sinceSend.Restart();
        return true;
    }

    /// Force the next TakeSend (e.g. right after connecting, so the core gets the current sticks).
    public void MarkDirty() => _dirty = true;

    private static float Norm(ushort us) => RcMath.ToNorm(us);

    public object ToMessage() => new { type = "rc", ch_us = ChannelsUs };
}
