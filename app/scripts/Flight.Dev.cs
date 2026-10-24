// Flight screen dev hooks: FB_KEYS_SELFTEST (also part of FB_SELFTEST=all), the `radio` self-test, FB_FAULT_SCRIPT and
// FB_LIVE_CHECK (the windows against a real core: once the firmware answers disarmed, its modes came by subscription and
// any first start is over — a screenshot of the layout (FB_SCREENSHOT), then the Functions and Config windows
// opened, Show cached, else Refresh from firmware (it reboots the firmware), until a real diff is shown; a second
// screenshot (<FB_SCREENSHOT>-views.png) and one `[live-check]` report line; then, on a heli bench, the take-off from the
// keyboard through the real key path (synthetic key events through the bind table, as the keys self-test): key switch_1
// (5) → the `armed` event, key switch_2 (6) → the head spools past TakeoffRpm (<FB_SCREENSHOT>-spool.png), W held (the
// keyboard collective) until altitude ≥ 0.5 m → `[live-check] PASS: take-off …` + <FB_SCREENSHOT>-airborne.png, else
// FAIL with the reason and the last state; quits; 8 min for the views, 4 more for the take-off).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public partial class Flight
{
    private int _selftestFrame;
    private ulong _selftestWaitUntil;
    private readonly List<string> _sentLines = new();
    private Dictionary<string, string>? _keysBackup;
    private bool _axesBefore;
    private string _lastFaultsText = "";

    private static InputEventKey Synthetic(Key key) => new() { Keycode = key, PhysicalKeycode = key, CtrlPressed = true, Pressed = true };

    /// Binds functions through the Keybinds table (as Settings → Keys does), pushes synthetic key presses
    /// into the root viewport (Viewport.PushInput, so they reach _UnhandledInput like real ones), checks
    /// the effect, prints the `fault` lines sent. Starts once connected (or after 5 s without a core).
    /// Restores the binds and the body-axes state; saves nothing.
    private void KeysSelfTest()
    {
        const string P = "[keys-selftest] ";
        var m = Manifest.Current;
        if (_selftestFrame == 0)
        {
            if (_selftestWaitUntil == 0) _selftestWaitUntil = Time.GetTicksMsec() + 5000;
            if (!_session.Connected && Time.GetTicksMsec() < _selftestWaitUntil) return;
            if (!_session.Connected) GD.Print(P + "no core connected: the fault lines will not be sent");
        }
        switch (++_selftestFrame)
        {
            case 10:
                GD.Print(P + $"manifest source: {m.Source}{(m.Core != "" ? $" ({m.Core} {m.Version})" : "")}");
                GD.Print(P + $"devices: {m.Devices.Count} ({m.Devices.Count(d => d.Implemented)} modelled, not modelled: {string.Join(", ", m.Devices.Where(d => !d.Implemented).Select(d => d.Id))}), faults: {m.AllFaults().Count()} ({string.Join(", ", m.AllFaults().Select(x => x.Device.Id + "." + x.Fault.Id))}), functions in the table: {Keybinds.All(m).Count}");
                _keysBackup = new Dictionary<string, string>(AppSettings.Current.Keys);
                _core.Sending += line => { if (line.Contains("\"type\":\"fault\"")) _sentLines.Add(line); };
                _axesBefore = _axes;
                Keybinds.Set("body_axes", Keybinds.EventName(Synthetic(Key.F9))!, m);
                GD.Print(P + $"bind body_axes = '{Keybinds.Bind("body_axes")}' via the table (default was X); X now dispatches to: {Keybinds.Match(new InputEventKey { Keycode = Key.X, Pressed = true }, m) ?? "nothing"}");
                GetTree().Root.PushInput(Synthetic(Key.F9));
                break;
            case 14:
                _keysAxesOk = _axes != _axesBefore;
                GD.Print(P + $"synthetic InputEventKey Ctrl+F9 -> body axes {_axesBefore} -> {_axes}: {(_keysAxesOk ? "PASS" : "FAIL")}");
                GetTree().Root.PushInput(Synthetic(Key.F9));   // back
                break;
            case 18:
            {
                _keysAxesOk &= _axes == _axesBefore;
                GD.Print(P + $"second press -> body axes {_axes} (restored: {(_axes == _axesBefore ? "yes" : "NO")})");
                // a fault with params of a device present on this bench (other devices' rows are disabled: an extra device in the manifest must not be picked)
                var present = WiredDevices();
                var lq = m.AllFaults().FirstOrDefault(x => x.Fault.Params.Count > 0 && x.Device.Implemented && present.Contains(x.Device.Id));
                if (lq.Device == null) { GD.Print(P + $"no modelled fault with params on a present device ({string.Join(",", present)}) in this manifest: fault part skipped"); _keysFaultOk = true; _selftestFrame = 25; break; }
                var id = Keybinds.FaultId(lq.Device.Id, lq.Fault.Id);
                Keybinds.Set(id, Keybinds.EventName(Synthetic(Key.F10))!, m);
                _faults?.RefreshBinds();
                _faults?.SetParam($"{lq.Device.Id}.{lq.Fault.Id}", lq.Fault.Params[0].Id, 30);
                _keysParam = lq.Fault.Params[0].Id;
                GD.Print(P + $"bind {id} = '{Keybinds.Bind(id)}', param {_keysParam} = 30; synthetic Ctrl+F10 (twice: on, then off)");
                GetTree().Root.PushInput(Synthetic(Key.F10));
                break;
            }
            case 22: GetTree().Root.PushInput(Synthetic(Key.F10)); break;
            case 26:
                foreach (var l in _sentLines) GD.Print(P + "sent: " + l);
                if (_keysParam != null)
                {
                    _keysFaultOk = _sentLines.Count == 2 && _sentLines[0].Contains("\"on\":true") && _sentLines[0].Contains($"\"params\":{{\"{_keysParam}\":30") && _sentLines[1].Contains("\"on\":false") && !_sentLines[1].Contains("params");
                    GD.Print(P + $"fault toggle via keybind: {(_keysFaultOk ? "PASS" : "FAIL")} ({_sentLines.Count} fault lines)");
                }
                // switch keys step the Transmitter rows in channel order (bench transmitter model): a static switch2 on ch9
                // here; its row's key (switch_N, N = its place among the rows) steps it 1000 → 2000 → 1000
                _bench.Transmitter = new() { ["9"] = new() { Type = TxModel.Switch2, Label = "selftest" } };
                DeriveSwitches();
                int row9 = _switches.FindIndex(x => x.Channel == 9);
                var seq9 = new List<int> { _rc.ChannelsUs[8] };
                if (row9 >= 0 && row9 < Keybinds.SwitchKeys)
                    for (int k = 0; k < 2; k++) { GetTree().Root.PushInput(PtKey(Keybinds.Bind(Keybinds.SwitchId(row9 + 1)))); _rc.Update(0); seq9.Add(_rc.ChannelsUs[8]); }
                _keysSwitchOk = row9 >= 0 && string.Join(",", seq9) == "1000,2000,1000" && _switchRows[row9].Key.Text == $"[{Keybinds.Bind(Keybinds.SwitchId(row9 + 1))}]";
                GD.Print(P + $"static switch2 on ch9 = row {row9 + 1} of {_switches.Count} ({string.Join(", ", _switches.Select(x => $"ch{x.Channel}"))}), key '{Keybinds.Bind(Keybinds.SwitchId(row9 + 1))}': ch9 {string.Join(" → ", seq9)}: {(_keysSwitchOk ? "PASS" : "FAIL")}");
                _bench.Transmitter = new();          // explicit empty = all auto on purpose (null would take the board's family default)
                _switchSet[8] = false;
                DeriveSwitches();
                _keysStickOk = KeysStickTest(P, m);
                if (_keysBackup != null) AppSettings.Current.Keys = _keysBackup;   // not saved: settings.json untouched
                UpdateKeysHint();
                GD.Print(P + $"binds restored (body_axes = '{Keybinds.Bind("body_axes")}'), settings not saved");
                SelfTests.Report("keys", _keysAxesOk && _keysFaultOk && _keysSwitchOk && _keysStickOk);
                _keysDone = true;
                break;
        }
    }
    private bool _keysAxesOk, _keysFaultOk, _keysSwitchOk, _keysStickOk, _keysDone, _radioDone;

    /// A key event for a bind, pressed or released, pushed into the root viewport as a real one (→ HandleKeys).
    private void PushKey(string bind, bool pressed) { var k = PtKey(bind); k.Pressed = pressed; GetTree().Root.PushInput(k); }

    /// The keyboard layer through the bind table (part of `keys`): every stick key moves its channel and lets go on
    /// release; stick_collective_plus rebound from W to I: I raises the collective, W does nothing; a key pressed while a
    /// text field has the focus moves nothing; the Transmitter hint line equals the bind table (`keys_hint`).
    private bool KeysStickTest(string P, Manifest m)
    {
        var fails = new List<string>();
        void Check(bool ok, string what) { if (!ok) fails.Add(what); }
        // 1. every stick function: press → its channel to its end, owner kb; release → centre (collective: holds)
        var seen = new List<string>();
        for (int i = 0; i < Keybinds.Sticks.Length; i++)
        {
            var f = Keybinds.Sticks[i];
            var bind = Keybinds.Bind(f.Id);
            int before = _rc.ChannelsUs[f.Channel];
            PushKey(bind, true); _rc.Update(f.Channel == 2 ? 0.2 : 0);
            int pressed = _rc.ChannelsUs[f.Channel];
            var owner = _rc.Owner[f.Channel];
            PushKey(bind, false); _rc.Update(0);
            int released = _rc.ChannelsUs[f.Channel];
            bool ok = pressed == (f.Channel == 2 ? Math.Clamp(before + f.Sign * 100, 1000, 2000) : 1500 + 500 * f.Sign);   // collective: 0.2 s at 1 / s
            ok &= owner == RcOwner.Kb && (f.Channel == 2 ? released == pressed : released == 1500) && !_rc.StickHeld(i);
            Check(ok, $"{f.Id} '{bind}': ch{f.Channel + 1} {before} → {pressed} (owner {owner}) → {released}");
            seen.Add($"{f.Id}={bind}: ch{f.Channel + 1} {before}→{pressed}→{released}");
        }
        GD.Print(P + "stick keys through the table: " + string.Join("; ", seen));
        // 2. rebind the collective's W to I (as Settings → Keys does): I moves it, W does not
        var free = Keybinds.EventName(new InputEventKey { Keycode = Key.I, PhysicalKeycode = Key.I, Pressed = true })!;
        Keybinds.Set("stick_collective_plus", free, m);
        int c0 = _rc.ChannelsUs[2];
        PushKey("W", true); _rc.Update(0.2); int cW = _rc.ChannelsUs[2]; PushKey("W", false); _rc.Update(0);
        PushKey(free, true); _rc.Update(0.2); int cI = _rc.ChannelsUs[2]; PushKey(free, false); _rc.Update(0);
        bool rebindOk = cW == c0 && cI == Math.Min(2000, c0 + 100) && Keybinds.Match(PtKey("W"), m) == null;
        Check(rebindOk, $"rebound to '{free}': W {c0} → {cW}, {free} → {cI}");
        GD.Print(P + $"stick_collective_plus rebound W → '{free}': W 0.2 s: ch3 {c0} → {cW}; '{free}' 0.2 s: ch3 → {cI}: {(rebindOk ? "PASS" : "FAIL")}");
        // 3. the hint line is the bind table (with the rebind in it)
        UpdateKeysHint();
        var want = Keybinds.Hint(_isPlane);
        bool hintOk = _keysHint != null && _keysHint.Text == want && want.Contains($"S/{free} collective")
                      && Keybinds.Sticks.All(s => want.Contains(Keybinds.Bind(s.Id))) && Enumerable.Range(1, Keybinds.SwitchKeys).All(n => want.Contains(Keybinds.Bind(Keybinds.SwitchId(n))));
        SelfTests.Report("keys_hint", hintOk, $"'{_keysHint?.Text}'{(hintOk ? "" : $" (want '{want}')")}");
        // 4. a text field has the focus (the CLI line in the Log / CLI window): the stick key moves nothing
        var logWin = _windows["Log / CLI"];
        bool logOpen = logWin.Visible;
        logWin.Visible = true; logWin.GrabFocus();
        _cli!.GrabFocus();
        int r0 = _rc.ChannelsUs[0];
        PushKey(Keybinds.Bind("stick_roll_plus"), true); _rc.Update(0);
        int r1 = _rc.ChannelsUs[0];
        bool direct = _rc.StickKey(Keybinds.StickIndex("stick_roll_plus"), "Right");
        _rc.Update(0);
        bool focusOk = Ui.TextFocus() && r1 == r0 && !direct && _rc.ChannelsUs[0] == r0 && !_rc.StickHeld(Keybinds.StickIndex("stick_roll_plus"));
        PushKey(Keybinds.Bind("stick_roll_plus"), false);
        logWin.GuiReleaseFocus();
        logWin.Visible = logOpen;
        _rc.Update(0);
        Check(focusOk, $"text field focused (TextFocus {Ui.TextFocus()}): '{Keybinds.Bind("stick_roll_plus")}' moved ch1 {r0} → {r1}, StickKey taken {direct}");
        GD.Print(P + $"text field focused: '{Keybinds.Bind("stick_roll_plus")}' → ch1 {r0} → {r1}: {(focusOk ? "PASS" : "FAIL")}");
        if (fails.Count > 0) GD.Print(P + "stick FAIL: " + string.Join("; ", fails));
        return fails.Count == 0 && hintOk;
    }

    /// Radio unplugged = link lost, through the joypad reader interface: a named test radio at index 0 is
    /// read, removed (Joy.RaiseChanged, as a hot-plug) → channels must hold and `fault crsf link_lost on`
    /// must be sent (and the Faults row on); put back → `on:false`. Settings (name, sources, option) and
    /// Joy.Reader restored; nothing saved. The fault part needs a connected core, crsf wired and the fault
    /// in the manifest; otherwise it checks that nothing was sent and says why.
    private sealed class TestRadio : IJoyReader
    {
        public const string Label = "FlightBench selftest radio";
        public bool Present = true;
        public readonly float[] Axes = { 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f };
        public int[] Pads() => Present ? new[] { 0 } : Array.Empty<int>();
        public string Name(int device) => device == 0 ? Label : "";
        public float Axis(int device, int axis) => Present && device == 0 && axis >= 0 && axis < Axes.Length ? Axes[axis] : 0f;
        public bool Button(int device, int button) => false;
    }
    private int _radioFrame;
    private ulong _radioWaitUntil;
    private TestRadio? _radio;
    private IJoyReader? _radioReaderBackup;
    private (string Name, int Device, bool Option, string[]? Source, float Deadzone, bool[] Reverse) _radioSettingsBackup;
    private readonly List<string> _radioSteps = new();
    private readonly List<string> _radioLines = new();
    private ushort[] _radioHeld = Array.Empty<ushort>();
    private readonly List<string> _radioFails = new();
    private string? _radioBlocked;

    private void RadioSelfTest()
    {
        const string P = "[radio-selftest] ";
        var t = AppSettings.Current.Transmitter;
        if (_radioFrame == 0)
        {
            if (_radioWaitUntil == 0) _radioWaitUntil = Time.GetTicksMsec() + 5000;
            if ((!_session.Connected || _stateCount == 0) && Time.GetTicksMsec() < _radioWaitUntil) return;
        }
        void Check(bool ok, string what) { if (!ok) _radioFails.Add(what); }
        switch (++_radioFrame)
        {
            case 1:
                _radioReaderBackup = Joy.Reader;
                _radioSettingsBackup = (t.DeviceName, t.Device, t.FailLinkOnDisconnect, t.Source, t.Deadzone, t.Reverse);
                _radio = new TestRadio();
                _radio.Axes[0] = 0.6f; _radio.Axes[3] = 0.4f;                       // ch1 at 1800, ch3 (axis 3) at 1700
                Joy.Reader = _radio;
                t.DeviceName = TestRadio.Label; t.Device = 0; t.FailLinkOnDisconnect = true;
                t.Deadzone = 0; t.Reverse = new bool[16];                            // the µs are exact: 1500 + 500 · value
                t.Source = (string[])t.Source!.Clone(); t.Source[0] = "axis 0"; t.Source[2] = "axis 3";   // ch1, ch3 on the pad
                _core.Sending += line => { if (line.Contains("\"type\":\"fault\"")) _radioLines.Add(line); };
                _radioBlocked = LinkFaultBlocker();
                GD.Print(P + $"test radio plugged at index 0{(_radioBlocked != null ? $"; fault part not checked: {_radioBlocked}" : "")}");
                Joy.RaiseChanged();
                break;
            case 5:
            {
                Check(_rc.ActiveDevice == 0 && !_rc.RadioLost, $"test radio not read (active {_rc.ActiveDevice}, lost {_rc.RadioLost})");
                // two hands on one channel: the last input that changed it holds it
                void Step(string what, int ch, int want, RcOwner owner)
                {
                    bool ok = _rc.ChannelsUs[ch] == want && _rc.Owner[ch] == owner;
                    _radioSteps.Add($"{what}: ch{ch + 1} {_rc.ChannelsUs[ch]} {RcInput.Mark(_rc.Owner[ch])}{(ok ? "" : $" (want {want} {RcInput.Mark(owner)})")}");
                    Check(ok, $"{what}: ch{ch + 1} = {_rc.ChannelsUs[ch]} owner {_rc.Owner[ch]}, want {want} {owner}");
                }
                var right = Keybinds.Bind("stick_roll_plus"); var up = Keybinds.Bind("stick_collective_plus");
                Step("pad holds ch1", 0, 1800, RcOwner.Joy);
                PushKey(right, true); _rc.Update(0);
                Step($"'{right}' pressed", 0, 2000, RcOwner.Kb);
                PushKey(right, false); _rc.Update(0);
                Step($"'{right}' released (pad at rest)", 0, 1500, RcOwner.Kb);
                _rc.Update(0);
                Step("pad not moved", 0, 1500, RcOwner.Kb);
                // the pad's reference is where it was when it last held the channel (1800): a drift from there of up to
                // JoyTakeUs leaves the channel with the keys, one step more and the pad takes it
                _radio!.Axes[0] = 0.75f; _rc.Update(0);
                Step("pad drifted 75 µs (keys keep it)", 0, 1500, RcOwner.Kb);
                _radio.Axes[0] = 0.8f; _rc.Update(0);
                Step("pad drifted 100 µs (keys keep it)", 0, 1500, RcOwner.Kb);          // the threshold itself does not take
                _radio.Axes[0] = 0.9f; _rc.Update(0);
                Step("pad moved 150 µs", 0, 1950, RcOwner.Joy);
                PushKey(right, true); _rc.Update(0);
                Step($"'{right}' pressed again", 0, 2000, RcOwner.Kb);
                _radio.Axes[0] = 0.5f; _rc.Update(0);
                Step($"pad moved to 0.5 while '{right}' held", 0, 1750, RcOwner.Joy);    // 200 µs from 1950, where it last held
                PushKey(right, false); _rc.Update(0);
                Step($"'{right}' released after the pad took ch1 (no snap)", 0, 1750, RcOwner.Joy);
                _radio.Axes[0] = 0.2f; _rc.Update(0);
                Step("pad back to 0.2", 0, 1600, RcOwner.Joy);
                // the collective's integrator continues from the pad's value
                Step("pad holds ch3", 2, 1700, RcOwner.Joy);
                PushKey(up, true); _rc.Update(0.1);
                Step($"'{up}' held 0.1 s", 2, 1750, RcOwner.Kb);
                PushKey(up, false); _rc.Update(0);
                Step($"'{up}' released", 2, 1750, RcOwner.Kb);
                _radio.Axes[3] = 0.0f; _rc.Update(0);
                Step("pad collective moved to 0", 2, 1500, RcOwner.Joy);
                GD.Print(P + "ownership: " + string.Join(" · ", _radioSteps));
                break;
            }
            case 7:
                _radioHeld = (ushort[])_rc.ChannelsUs.Clone();
                _radio!.Present = false;
                GD.Print(P + $"unplug (ch1 = {_rc.ChannelsUs[0]})");
                Joy.RaiseChanged();
                break;
            case 13:
                Check(_rc.RadioLost, "RadioLost not set after the unplug");
                Check(_rc.ChannelsUs.AsSpan().SequenceEqual(_radioHeld), $"channels did not hold: ch1..4 {string.Join(" ", _rc.ChannelsUs[0..4])} (held {string.Join(" ", _radioHeld[0..4])})");
                if (_radioBlocked == null)
                {
                    Check(_radioLines.Count == 1 && _radioLines[0].Contains("\"device\":\"crsf\"") && _radioLines[0].Contains("\"kind\":\"link_lost\"") && _radioLines[0].Contains("\"on\":true"), "fault on line: " + string.Join(" | ", _radioLines));
                    Check(_faults?.IsOn(LinkFaultKey) == true, "Faults row not on");
                }
                else Check(_radioLines.Count == 0, "fault sent although blocked: " + string.Join(" | ", _radioLines));
                _radio!.Present = true;
                GD.Print(P + "plug back");
                Joy.RaiseChanged();
                break;
            case 19:
                Check(!_rc.RadioLost && _rc.ActiveDevice == 0, "radio not read again after the replug");
                Check(_rc.ChannelsUs[0] == 1600 && _rc.Owner[0] == RcOwner.Joy && _rc.ChannelsUs[2] == 1500 && _rc.Owner[2] == RcOwner.Joy, $"after the replug ch1 {_rc.ChannelsUs[0]} {_rc.Owner[0]}, ch3 {_rc.ChannelsUs[2]} {_rc.Owner[2]} (want the pad's 1600 / 1500)");
                if (_radioBlocked == null)
                    Check(_radioLines.Count == 2 && _radioLines[1].Contains("\"kind\":\"link_lost\"") && _radioLines[1].Contains("\"on\":false"), "fault off line: " + string.Join(" | ", _radioLines));
                else Check(_radioLines.Count == 0, "fault sent although blocked: " + string.Join(" | ", _radioLines));
                foreach (var l in _radioLines) GD.Print(P + "sent: " + l);
                Joy.Reader = _radioReaderBackup!;
                (t.DeviceName, t.Device, t.FailLinkOnDisconnect, t.Source, t.Deadzone, t.Reverse) = _radioSettingsBackup;
                Joy.RaiseChanged();
                SelfTests.Report("radio", _radioFails.Count == 0, _radioFails.Count > 0 ? string.Join("; ", _radioFails)
                    : (_radioBlocked == null ? "unplug → channels held, fault crsf link_lost on sent + Faults row on; replug → on:false" : $"channels held; fault lines not checked ({_radioBlocked}), none sent")
                      + "; ownership: " + string.Join(" · ", _radioSteps));
                _radioDone = true;
                break;
        }
    }
    private string? _keysParam;

    private int _lcStage;
    private ulong _lcAt, _lcDeadline;
    private int _lcTries;
    // the take-off part (heli benches)
    private double _lcArmedT = double.NaN, _lcStageT, _lcMaxAlt, _lcMaxRpm;
    private int _lcArmTries;                                         // switch_1 cycled off/on after an ARMSWITCH latch
    private bool _lcHooked, _lcHoldW, _lcFailed;
    /// Head speed the take-off waits for before raising the collective: 1600 rpm ≈ 90 % of the RS7 profile's
    /// gov_headspeed 1800 (models/rs7/qemu-profile.txt); flight.py's liftoff plan waits 17 virtual s after the throttle
    /// switch for the same reason.
    public const double TakeoffRpm = 1600, TakeoffAlt = 0.5;

    private void LiveCheckTick()
    {
        const string P = "[live-check] ";
        var now = Time.GetTicksMsec();
        if (_lcDeadline == 0) _lcDeadline = now + 8 * 60 * 1000;
        if (now > _lcDeadline && _lcStage < 9) { GD.Print(P + $"FAIL: timed out at stage {_lcStage} (connected {_session.Connected}, modes live {_modesLive}, msp {_fc.Msp}, armed {_fc.Armed}, config '{_plane.Config.State}', diff live {_diffLive})"); _lcStage = 9; GetTree().Quit(1); return; }
        void Shot(string suffix)
        {
            if (DevEnv.Screenshot is not { } shot) return;
            var path = suffix == "" ? shot : System.IO.Path.ChangeExtension(shot, null) + suffix + ".png";
            RenderingServer.FramePostDraw += Save;
            void Save() { RenderingServer.FramePostDraw -= Save; GetViewport().GetTexture().GetImage().SavePng(path); GD.Print(P + "screenshot " + path); }
        }
        switch (_lcStage)
        {
            case 0:                                      // ready: connected, firmware answering disarmed, modes from the core, no first start running
                if (!(_session.Connected && _modesLive && _fc.Msp == true && _fc.Armed == false && !_plane.Config.Active && _stateCount > 0)) break;
                _lcStage = 1; _lcAt = now + 3000; break;
            case 1 when now >= _lcAt:
                UpdateStatus(); Shot(""); _lcStage = 2; _lcAt = now + 1500; break;
            case 2 when now >= _lcAt:
                // both over the Log's place, side by side with the four main windows (this run only: nothing is saved)
                var fnw = _windows["Functions"]; fnw.Position = new Vector2I(420, 620); fnw.Size = new Vector2I(780, 210); fnw.Visible = true;
                var dfw = _windows["Config"]; dfw.Position = new Vector2I(420, 70); dfw.Size = new Vector2I(780, 510); dfw.Visible = true;
                if (!_diffLive) RequestDiff(false);
                _lcStage = 3; _lcAt = now + 2000; break;
            case 3 when now >= _lcAt:
                if (_diffLive) { _lcStage = 5; _lcAt = now + 1000; break; }
                if (_fc.Armed == false && _lcTries++ < 10) { Log("app", "[live-check] no diff kept by the core: Refresh from firmware (reboots the firmware)"); RequestDiff(true); }
                _lcStage = 4; _lcAt = now + 4000; break;
            case 4 when now >= _lcAt:
                _lcStage = _diffLive ? 5 : 3; _lcAt = now + (_diffLive ? 6000UL : 0UL); break;   // after the refresh's reboot the core reads the modes again
            case 5 when now >= _lcAt && _modesLive && _fc.Msp == true:
            {
                UpdateStatus();
                Shot("-views");
                var sw = string.Join("; ", _switches.Select(s => s.Describe()));
                var fns = string.Join(", ", _functions?.Catalog.Select(f => f.Name) ?? Enumerable.Empty<string>());
                GD.Print(P + $"PASS: Transmitter '{_modesHeader?.Text}' switches [{sw}] (variant {_modes?.Variant}); Functions [{fns}] from {Manifest.Current.Source}; "
                         + $"Config '{_diffView?.HeaderText}'; Status '{_statusPanel?.Link.Text}' / '{_statusPanel?.FirmwareText}' / '{_statusPanel?.Sensors.Text}'; "
                         + $"Aircraft rows {string.Join(",", _aircraftPanel?.VisibleRows ?? Enumerable.Empty<string>())}; configurator {CfgAddress}; core {CoreMenuLine()}");
                _lcStage = 6; _lcAt = now + 1500; break;
            }
            case 6 when now >= _lcAt:
                if (_isPlane) { _lcStage = 9; GetTree().Quit(); break; }
                _lcStage = 10; _lcDeadline = now + 4 * 60 * 1000; break;
            case >= 10 and < 30:
                TakeoffTick(P, Shot);
                break;
        }
    }

    /// The take-off of FB_LIVE_CHECK through the keyboard: switch keys and W through the same input path a user's keys take.
    private void TakeoffTick(string P, Action<string> Shot)
    {
        using var inv = new InvariantText();
        var now = Time.GetTicksMsec();
        double t = _lastT, rpm = _readout.Rpm ?? 0, alt = _readout.Alt ?? 0;
        _lcMaxAlt = Math.Max(_lcMaxAlt, alt); _lcMaxRpm = Math.Max(_lcMaxRpm, rpm);
        string Last() => $"t = {t:0.00} s, armed {_fc.Armed}, arming flags [{string.Join(" ", _fc.Flags)}], ch5 {_rc.ChannelsUs[4]} µs, ch6 (throttle) {_rc.ChannelsUs[5]} µs, ch3 (collective) {_rc.ChannelsUs[2]} µs, head {rpm:0} rpm (max {_lcMaxRpm:0}), alt {alt:0.00} m (max {_lcMaxAlt:0.00}), rows [{string.Join("; ", _switches.Select(x => x.Describe()))}], {JoypadState()}";
        void Fail(string why)
        {
            HoldW(false);
            GD.Print(P + $"FAIL: take-off: {why}; last state: {Last()}");
            Shot("-airborne");
            _lcStage = 19; _lcAt = now + 1500; _lcFailed = true;
        }
        void Key(int n)
        {
            var bind = Keybinds.Bind(Keybinds.SwitchId(n));
            Log("app", $"[live-check] key '{bind}' ({Keybinds.SwitchId(n)}) pressed");
            GetTree().Root.PushInput(PtKey(bind));
        }
        if (_lcHoldW) HoldW(true);                                   // re-asserted every frame (a focus change releases keys)
        if (now > _lcDeadline && _lcStage < 19) { Fail($"timed out at stage {_lcStage}"); return; }
        switch (_lcStage)
        {
            case 10:
            {
                if (!_lcHooked) { _lcHooked = true; _core.EventReceived += (name, _, et) => { if (name == "armed") _lcArmedT = double.IsNaN(et) ? _lastT : et; }; }
                _windows["Functions"].Visible = false; _windows["Config"].Visible = false;   // the Transmitter / Aircraft windows in view (this run only)
                int r5 = _switches.FindIndex(x => x.Channel == 5), r6 = _switches.FindIndex(x => x.Channel == 6);
                GD.Print(P + $"take-off: rows [{string.Join("; ", _switches.Select(x => x.Describe()))}], header '{_modesHeader?.Text}', {JoypadState()}, keys switch_1 '{Keybinds.Bind("switch_1")}' switch_2 '{Keybinds.Bind("switch_2")}' collective+ '{Keybinds.Bind("stick_collective_plus")}'");
                if (r5 != 0 || r6 != 1) { Fail($"the Transmitter rows do not put ARM (ch5) on switch_1 and the throttle (ch6) on switch_2 (ch5 row {r5 + 1}, ch6 row {r6 + 1})"); return; }
                if (!_switches[1].Static) { Fail("ch6 is not a bench switch (the bench's transmitter model has no ch6 switch2)"); return; }
                Key(1);
                _lcStage = 11; _lcStageT = t;
                break;
            }
            case 11:
                if (!double.IsNaN(_lcArmedT))
                {
                    GD.Print(P + $"take-off: armed at t = {_lcArmedT:0.00} s (key '{Keybinds.Bind("switch_1")}', ch5 {_rc.ChannelsUs[4]} µs)");
                    Key(2);
                    _lcStage = 12; _lcStageT = t;
                }
                else if (t - _lcStageT > 3 && _lcArmTries < 3 && _fc.Flags.Contains("ARMSWITCH"))
                {
                    /* the firmware latches ARMSWITCH when the arm switch is on while arming is blocked (right after its
                     * reboot: boot grace, gyro calibration); like a pilot, cycle the switch off and on again */
                    _lcArmTries++;
                    Log("app", $"[live-check] ARMSWITCH latched after the reboot: switch_1 off and on again ({_lcArmTries})");
                    Key(1);
                    _lcStage = 15; _lcStageT = t;
                }
                else if (t - _lcStageT > 20) Fail("no `armed` event 20 virtual s after key switch_1");
                break;
            case 15:
                if (t - _lcStageT > 0.6) { Key(1); _lcStage = 11; _lcStageT = t; }
                break;
            case 12:
                if (_fc.Armed == false) { Fail("disarmed while spooling"); return; }
                // the key itself must have put the throttle on (the 2026-10-23 bug: ch6 stuck at 1000, nothing could drive it)
                if (_rc.ChannelsUs[5] != 2000) { Fail($"key switch_2 did not put ch6 (throttle) at 2000 µs: {_rc.ChannelsUs[5]} µs"); return; }
                if (rpm >= TakeoffRpm)
                {
                    GD.Print(P + $"take-off: head {rpm:0} rpm at t = {t:0.00} s ({t - _lcStageT:0.0} virtual s after the throttle switch, ch6 {_rc.ChannelsUs[5]} µs); holding W");
                    Shot("-spool");
                    _lcCollectiveFrom = _rc.ChannelsUs[2];
                    HoldW(true);
                    _lcStage = 13; _lcStageT = t;
                }
                else if (t - _lcStageT > 45) Fail($"the head did not reach {TakeoffRpm:0} rpm 45 virtual s after key switch_2");
                break;
            case 13:
                if (_fc.Armed == false) { Fail("disarmed while climbing"); return; }
                if (alt >= TakeoffAlt)
                {
                    HoldW(false);
                    UpdateStatus();
                    GD.Print(P + $"PASS: take-off from the keyboard: armed at t = {_lcArmedT:0.00} s (key '{Keybinds.Bind("switch_1")}' → ch5 {_rc.ChannelsUs[4]} µs), throttle on (key '{Keybinds.Bind("switch_2")}' → ch6 {_rc.ChannelsUs[5]} µs), "
                             + $"{Keybinds.Bind("stick_collective_plus")} held {t - _lcStageT:0.0} virtual s (ch3 {_lcCollectiveFrom} → {_rc.ChannelsUs[2]} µs, held by {RcInput.Mark(_rc.Owner[2])}) → altitude {alt:0.00} m at t = {t:0.00} s, head {rpm:0} rpm; Aircraft '{_aircraftPanel?.RowText("power")}'; {JoypadState()}");
                    Shot("-airborne");
                    _lcStage = 19; _lcAt = now + 1500;
                }
                else if (t - _lcStageT > 20) Fail($"altitude below {TakeoffAlt} m 20 virtual s after W");
                break;
            // Settings over the bench, for the evidence: Keys (the catalogue, the switch rows named after this bench's rows)
            // and Transmitter (the joypad line); then quit
            case 19 when now >= _lcAt:
                if (DevEnv.Screenshot == null) { _lcStage = 29; _lcAt = now; break; }
                OpenSettings(); _settingsPage?.ShowTab("Keys");
                _lcStage = 21; _lcAt = now + 1500; break;
            case 21 when now >= _lcAt:
                GD.Print(P + $"Settings → Keys: '{_settingsPage?.KeyRowLabel("switch_1")}', '{_settingsPage?.KeyRowLabel("switch_2")}', '{_settingsPage?.KeyRowLabel("stick_collective_plus")}' = {Keybinds.Bind("stick_collective_plus")}");
                Shot("-settings-keys"); _lcStage = 22; _lcAt = now + 1000; break;
            case 22 when now >= _lcAt:
                _settingsPage?.ShowTab("Transmitter"); _lcStage = 23; _lcAt = now + 1500; break;
            case 23 when now >= _lcAt:
                GD.Print(P + $"Settings → Transmitter: '{Settings.JoypadLine(_rc.ActiveDevice)}'");
                Shot("-settings-transmitter"); _lcStage = 24; _lcAt = now + 1000; break;
            case 24 when now >= _lcAt:
                CloseSettings(); _lcStage = 29; _lcAt = now + 500; break;
            case 29 when now >= _lcAt:
                _lcStage = 30; GetTree().Quit(_lcFailed ? 1 : 0); break;
        }
    }

    private int _lcCollectiveFrom;

    /// The collective-up key (stick_collective_plus, W by default) held / released as the keyboard sends it
    /// (Input.ParseInputEvent → the viewport → HandleKeys → the bind table → RcInput.StickKey / StickRelease).
    private void HoldW(bool on)
    {
        if (!on && !_lcHoldW) return;
        _lcHoldW = on;
        var k = PtKey(Keybinds.Bind("stick_collective_plus"));
        k.Pressed = on;
        Input.ParseInputEvent(k);
    }

    /// The joypad as read: name, the raw axes of channels 1..4's sources and who holds each of them (live check).
    private string JoypadState()
    {
        int dev = _rc.ActiveDevice;
        var owners = string.Join(" ", Enumerable.Range(0, 4).Select(c => $"ch{c + 1}={(RcInput.Mark(_rc.Owner[c]) is { Length: > 0 } o ? o : "-")}"));
        if (dev < 0) return $"joypad none (channels 1..4 from the keys; owners {owners})";
        var axes = string.Join(" ", Enumerable.Range(0, 4).Select(c => RcInput.SourceOf(c) is { Kind: "axis" } s ? $"ch{c + 1}=a{s.A} {Joy.Reader.Axis(dev, s.A).ToString("+0.000;-0.000;0.000", System.Globalization.CultureInfo.InvariantCulture)}" : $"ch{c + 1}={RcInput.SourceOf(c)}"));
        return $"joypad {dev} '{Joy.Reader.Name(dev)}' (raw {axes}; owners {owners})";
    }

    // FB_FAULT_SCRIPT: short live check against a real core — crsf.lq on (lq 30, rssi -95) → off →
    // crsf.freeze on → off, logging state.faults and the fault events; FB_SCREENSHOT is taken while
    // crsf.lq is lit; quits itself once everything it turned on is off again (no --quit-after needed).
    private double _scriptT = -1;
    private int _scriptStage;

    private void FaultScriptState(JsonElement s)
    {
        if (s.TryGetProperty("faults", out var fl) && fl.GetRawText() is var ft && ft != _lastFaultsText)
        {
            _lastFaultsText = ft;
            Log("app", $"state.faults = {ft}  (t = {Json.Num(s, "t", 0):0.00})");
        }
    }

    private void FaultScript()
    {
        if (_faults == null) return;
        if (_scriptT < 0) { if (_stateCount > 0 && (Manifest.Current.IsLive || _stateCount > 300)) { _scriptT = 0; Log("app", "[fault-script] start, manifest " + Manifest.Current.Source); } return; }
        _scriptT += GetProcessDeltaTime();
        const string lq = "crsf.lq", fr = "crsf.freeze";
        bool lit(string k) => _faults.Active.Contains(k);
        switch (_scriptStage)
        {
            case 0 when _scriptT > 0.5:
                _windows["Faults"].Visible = true;
                _faults.SetParam(lq, "lq", 30); _faults.SetParam(lq, "rssi_dbm", -95);
                _faults.Toggle(lq); _scriptStage++; _scriptT = 0; break;
            case 1 when lit(lq) && _scriptT > 1.5 || _scriptT > 6:
                if (DevEnv.Screenshot is { } shot)
                { GetViewport().GetTexture().GetImage().SavePng(shot); Log("app", $"[fault-script] screenshot ({lq} lit: {lit(lq)}) saved to {shot}"); }
                _scriptStage++; _scriptT = 0; break;
            case 2 when _scriptT > 1.5:
                _faults.Toggle(lq); _scriptStage++; _scriptT = 0; break;
            case 3 when !lit(lq) && _scriptT > 0.5 || _scriptT > 5:
                _faults.Toggle(fr); _scriptStage++; _scriptT = 0; break;
            case 4 when lit(fr) && _scriptT > 1.0 || _scriptT > 5:
                _faults.Toggle(fr); _scriptStage++; _scriptT = 0; break;
            case 5 when !lit(fr) && _scriptT > 0.5 || _scriptT > 5:
                Log("app", $"[fault-script] done; state.faults now {_lastFaultsText}");
                _scriptStage++; _scriptT = 0; break;
            case 6 when _scriptT > 0.5:
                GetTree().Quit(); _scriptStage++; break;
        }
    }
}
