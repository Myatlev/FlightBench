// Settings screen: interface (UI scale), paths, transmitter (receiver-tab style: the joypad as a fact — connected or not —,
// device, calibration, per-channel source / reverse / min-centre-max with live bars, raw axes), keys (the complete
// catalogue in three groups — Sticks, Transmitter switches, Application — every function with its bind: Bind = the next
// key or joypad button, Clear = unbound; Keybinds.cs; over a bench the switch rows say what they are on it). Saved on Back,
// on every bind change and when the screen goes away (window closed). Also opened over the flight screen
// (top bar → Settings: `Overlay`, a window over the bench; Back closes it, the core stays connected).
using System;
using System.Collections.Generic;
using System.Linq;
using Godot;

namespace FlightBench;

public partial class Settings : Control
{
    private readonly RcInput _rc = new();
    private StickView _left = null!, _right = null!;
    private AppSettings S => AppSettings.Current;

    public override void _Ready()
    {
        var root = Ui.Screen(this, "Settings", 1120, 860);
        var tabs = new TabContainer { SizeFlagsVertical = SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(0, 200) };
        root.AddChild(tabs);
        tabs.AddChild(Ui.Scroll(BuildInterface())); tabs.SetTabTitle(0, "Interface");
        tabs.AddChild(Ui.Scroll(BuildPaths())); tabs.SetTabTitle(1, "Paths");
        tabs.AddChild(BuildTransmitter()); tabs.SetTabTitle(2, "Transmitter");
        var keysScroll = Ui.Scroll(BuildKeys());
        tabs.AddChild(keysScroll); tabs.SetTabTitle(3, "Keys");
        // FB_SETTINGS_SCROLL=end (dev): scroll the Keys table to its end (the fault rows) for screenshots
        if (DevEnv.SettingsScroll == "end")
            GetTree().CreateTimer(0.5).Timeout += () => keysScroll.ScrollVertical = (int)keysScroll.GetVScrollBar().MaxValue;
        // FB_SETTINGS_TAB=<title> (dev): open that tab, e.g. Transmitter
        _tabs = tabs;
        ShowTab(DevEnv.SettingsTab);
        // from the main menu: Back returns there; opened over the flight screen (Overlay): Back closes the overlay, the bench keeps running
        root.AddChild(Ui.Row(Ui.Btn(Overlay ? "< Back to the bench" : "< Back", () => { if (Overlay) Closed?.Invoke(); else GetTree().ChangeSceneToFile("res://scenes/MainMenu.tscn"); })));   // _ExitTree saves
    }

    private TabContainer? _tabs;

    /// Shows the tab with that title (dev: FB_SETTINGS_TAB, the live check's screenshots); unknown / empty: no change.
    public void ShowTab(string? title)
    {
        for (int i = 0; _tabs != null && i < _tabs.GetTabCount() && !string.IsNullOrEmpty(title); i++)
            if (string.Equals(_tabs.GetTabTitle(i), title, StringComparison.OrdinalIgnoreCase)) _tabs.CurrentTab = i;
    }

    /// Opened over the flight screen (Flight.OpenSettings): Back raises Closed instead of changing the scene.
    public bool Overlay;
    public event Action? Closed;

    private Control BuildInterface()
    {
        var v = new VBoxContainer(); v.AddThemeConstantOverride("separation", 6);
        var scale = new OptionButton();
        var applied = new Label { Modulate = Ui.Soft };
        void Refresh()
        {
            scale.SetItemText(0, $"auto ({Ui.FormatScale(Ui.AutoScale())})");
            applied.Text = Ui.ScaleOverride != null
                ? $"applied: {Ui.FormatScale(Ui.CurrentScale)}  (FB_UI_SCALE={Ui.ScaleOverride} overrides this setting)"
                : $"applied: {Ui.FormatScale(Ui.CurrentScale)}";
        }
        scale.AddItem("auto");
        foreach (var f in Ui.ScaleSteps) scale.AddItem(Ui.FormatScale(f));
        var cur = S.UiScale == "auto" ? 0 : Array.FindIndex(Ui.ScaleSteps, f => Mathf.IsEqualApprox(f, Ui.ResolveScale(S.UiScale))) + 1;
        scale.Selected = Math.Max(0, cur);
        scale.ItemSelected += i =>
        {
            S.UiScale = i == 0 ? "auto" : Ui.FormatScale(Ui.ScaleSteps[i - 1]);
            S.Save();
            Ui.ApplyScale(GetTree());
            Refresh();
        };
        Refresh();
        v.AddChild(Ui.Row(new Label { Text = "UI scale:" }, scale, applied));
        v.AddChild(new Label { Text = "auto = screen DPI / 96, rounded to 0.25 (1.0 … 2.0). Scales text, controls and the flight screen's windows; the 3D view keeps the full resolution.", Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart });
        return v;
    }

    private Control BuildPaths()
    {
        var v = new VBoxContainer(); v.AddThemeConstantOverride("separation", 6);
        v.AddChild(new Label { Text = "Repo root (boards/, models/):" });
        var repo = new LineEdit { Text = S.RepoRoot }; repo.TextChanged += t => S.RepoRoot = t; v.AddChild(repo);
        v.AddChild(new Label { Text = "Firmware directory (where the firmware file browsers start; empty = the current file's folder):" });
        var fw = new LineEdit { Text = S.FirmwareDir }; fw.TextChanged += t => S.FirmwareDir = t.Trim(); v.AddChild(fw);
        v.AddChild(new Label { Text = "Default core port (new benches):" });
        var port = new SpinBox { MinValue = 1024, MaxValue = 65535, Value = S.DefaultCorePort, SizeFlagsHorizontal = SizeFlags.ShrinkBegin }; port.ValueChanged += d => S.DefaultCorePort = (int)d; v.AddChild(port);
        // the core the app starts for a local bench (CoreLocator): bundled data/core, then this path, then FB_CORE_EXE
        v.AddChild(new HSeparator());
        v.AddChild(new Label { Text = "Core (qemu-system-arm; empty = auto):" });
        var found = new Label { Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart };
        void Found()
        {
            var exe = CoreLocator.Find(out var src);
            found.Text = exe != null ? $"{(S.CorePath.Trim() == "" ? "auto: " : "")}{exe}  ({src})" : $"not found: {src}";
        }
        var core = new LineEdit { Text = S.CorePath, PlaceholderText = "auto", SizeFlagsHorizontal = SizeFlags.ExpandFill };
        core.TextChanged += t => { S.CorePath = t.Trim(); Found(); };
        var browse = Ui.Btn("Browse…", () => Ui.PickFile(this, new[] { "qemu-system-arm*" }, System.IO.Path.GetDirectoryName(CoreLocator.Find(out _) ?? ""), p => core.Text = p));
        var auto = Ui.Btn("Auto", () => core.Text = "");
        v.AddChild(Ui.Row(core, browse, auto));
        Found();
        v.AddChild(found);
        v.AddChild(new Label { Text = "Data root (boards/, models/; the core's root=):" });
        v.AddChild(new Label { Text = CoreLocator.DataRoot, Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.Arbitrary });
        var keep = new CheckBox { Text = "Keep the core running after closing the bench (reused when the same bench opens again; app exit always stops it)", ButtonPressed = S.KeepCoreRunning };
        keep.Toggled += on => S.KeepCoreRunning = on;
        v.AddChild(keep);
        v.AddChild(new Label { Text = "A bench whose core address is 127.0.0.1 / localhost: the app checks it (session-check), starts the core and stops it when the bench closes. A remote address: connect only.", Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart });
        return v;
    }

    // ---- Transmitter (receiver-tab style) ----
    private OptionButton _device = null!;
    private bool _devicesDirty = true;
    private readonly ChannelBar[] _bars = new ChannelBar[16];
    private readonly List<Action> _rowRefresh = new();
    private readonly Label[] _raw = new Label[Joy.Axes];
    private Label _rawButtons = null!, _calInfo = null!, _joyLine = null!;
    private Button _calBtn = null!;
    private bool _calibrating;
    private readonly float[] _calMin = new float[Joy.Axes], _calMax = new float[Joy.Axes];
    private int _detectCh = -1;
    private Button? _detectBtn;
    private readonly float[] _detectAxes = new float[Joy.Axes];
    private readonly bool[] _detectButtons = new bool[Joy.Buttons];
    private const int SrcAxis0 = 2, SrcButton0 = SrcAxis0 + Joy.Axes, SrcPair = SrcButton0 + Joy.Buttons;

    private static int SourceIndex(RcSource s) => s.Kind switch
    {
        "ui" => 1, "axis" => SrcAxis0 + s.A, "button" => SrcButton0 + s.A, "buttons" => SrcPair, _ => 0,
    };

    /// The Transmitter page's joypad line (a fact, not a choice): the pad read, or none and where the channels come from.
    public static string JoypadLine(int dev) => dev >= 0
        ? $"Joypad: {Joy.Reader.Name(dev)} — connected"
        : "Joypad: — not plugged in (channels from the keys and the Transmitter window)";

    private void OnJoyChanged() => _devicesDirty = true;

    private Control BuildTransmitter()
    {
        var t = S.Transmitter;
        var col = new VBoxContainer(); col.AddThemeConstantOverride("separation", 8);
        _joyLine = new Label { Text = JoypadLine(_rc.ActiveDevice >= 0 ? _rc.ActiveDevice : -1), Name = "joypad_line" };
        col.AddChild(_joyLine);

        // top: sticks | device, calibration, presets, raw axes
        var top = new HBoxContainer(); top.AddThemeConstantOverride("separation", 20);
        col.AddChild(top);
        _left = new StickView { CustomMinimumSize = new Vector2(150, 150), SizeFlagsVertical = SizeFlags.ShrinkBegin };
        _right = new StickView { CustomMinimumSize = new Vector2(150, 150), SizeFlagsVertical = SizeFlags.ShrinkBegin };
        top.AddChild(_left); top.AddChild(_right);
        var side = new VBoxContainer { SizeFlagsHorizontal = SizeFlags.ExpandFill }; side.AddThemeConstantOverride("separation", 6);
        top.AddChild(side);

        _device = new OptionButton { SizeFlagsHorizontal = SizeFlags.ExpandFill, ClipText = true };
        _device.ItemSelected += i =>
        {
            int id = _device.GetItemId((int)i);
            if (id < 0) return;
            t.Device = id; t.DeviceName = Joy.Reader.Name(id);
            Joy.RaiseChanged();
        };
        var mode = new OptionButton(); mode.AddItem("Mode 1"); mode.AddItem("Mode 2"); mode.AddItem("Mode 3"); mode.AddItem("Mode 4");
        mode.Selected = Math.Clamp(t.Mode - 1, 0, 3); mode.ItemSelected += i => t.Mode = (int)i + 1;
        side.AddChild(Ui.Row(new Label { Text = "Joypad:" }, _device, new Label { Text = "Sticks:" }, mode));

        var dz = new SpinBox { MinValue = 0, MaxValue = 0.2, Step = 0.005, Value = t.Deadzone, CustomMinimumSize = new Vector2(90, 0) };
        dz.ValueChanged += d => t.Deadzone = (float)d;
        _calBtn = Ui.Btn("Calibrate", ToggleCalibrate);
        _calBtn.TooltipText = "Calibrate: move all sticks and switches to their extremes, then centre them and press Done";
        var edgetx = Ui.Btn("EdgeTX classic: CH1..CH8 → axes 0..7", () => { t.ApplyEdgeTxClassic(); RefreshRows(); });
        var gamepad = Ui.Btn("Gamepad default", () =>
        {
            var d = new TransmitterSettings();
            d.Normalize();
            t.Source = d.Source; t.Reverse = d.Reverse; t.MinUs = d.MinUs; t.CenterUs = d.CenterUs; t.MaxUs = d.MaxUs;
            RefreshRows();
        });
        side.AddChild(Ui.Row(_calBtn, edgetx, gamepad, new Label { Text = "deadzone" }, dz));
        _calInfo = new Label { Text = "EdgeTX: USB mode Joystick; classic mode sends the model's CH1..CH8 as axes 0..7. Switch logic stays in the radio.", Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart };
        side.AddChild(_calInfo);
        var linkLoss = new CheckBox { Text = "When the radio (joypad) disconnects, fail the receiver link (crsf.link_lost)", ButtonPressed = t.FailLinkOnDisconnect,
                                      TooltipText = "flight screen: the named radio unplugged → fault crsf link_lost on (channels hold, the keys not read; the firmware goes to failsafe), off again once it is back" };
        linkLoss.Toggled += v => t.FailLinkOnDisconnect = v;
        side.AddChild(linkLoss);

        // raw strip: every axis of the device as the OS reports it (−1..1), and pressed buttons
        side.AddChild(new Label { Text = "raw from the device:", Modulate = Ui.Soft });
        var raw = new GridContainer { Columns = 5 }; raw.AddThemeConstantOverride("h_separation", 18);
        for (int a = 0; a < Joy.Axes; a++) { _raw[a] = new Label { Text = $"a{a}  -", CustomMinimumSize = new Vector2(96, 0) }; raw.AddChild(_raw[a]); }
        side.AddChild(raw);
        _rawButtons = new Label { Text = "buttons: -", Modulate = Ui.Soft };
        side.AddChild(_rawButtons);

        // channel rows
        var rows = new GridContainer { Columns = 8 };
        rows.AddThemeConstantOverride("h_separation", 8); rows.AddThemeConstantOverride("v_separation", 2);
        foreach (var txt in new[] { "channel", "source", "", "reverse", "min µs", "centre µs", "max µs", "output" })
            rows.AddChild(new Label { Text = txt, Modulate = Ui.Soft });
        var scroll = Ui.Scroll(rows);
        scroll.CustomMinimumSize = new Vector2(0, 160);
        col.AddChild(scroll);
        col.AddChild(new Label { Text = "source " + UiSourceText, Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart });
        for (int ch = 0; ch < 16; ch++) BuildChannelRow(rows, ch);
        Joy.Changed += OnJoyChanged;
        return col;
    }

    /// What the `ui` source means (the source dropdown's tooltip; docs/ui.md).
    public const string UiSourceText = "ui: the Transmitter window's switch for this channel (its type is set per bench: Edit bench → Transmitter)";

    private void BuildChannelRow(GridContainer grid, int c)
    {
        var t = S.Transmitter;
        var name = new Label { Text = RcNames.Long(c), CustomMinimumSize = new Vector2(150, 0) };
        var src = new OptionButton { CustomMinimumSize = new Vector2(128, 0), FitToLongestItem = false };
        src.AddItem("none"); src.AddItem("ui");
        src.SetItemTooltip(0, "no joypad input: 1500 µs, unless the Transmitter window's switch (channels 5..16) or the keys (channels 1..4, Settings → Keys → Sticks) drive it");
        src.SetItemTooltip(1, UiSourceText);
        for (int a = 0; a < Joy.Axes; a++) src.AddItem($"axis {a}");
        for (int b = 0; b < Joy.Buttons; b++) src.AddItem($"button {b}");
        src.AddItem("buttons n,m");
        var pair = new LineEdit { PlaceholderText = "n,m", CustomMinimumSize = new Vector2(60, 0), TooltipText = "3-position from two buttons: n = low, m = high, neither = centre" };
        var detect = new Button { Text = "Detect", CustomMinimumSize = new Vector2(76, 0), TooltipText = "then move a stick/switch: the next axis that moves by > 0.5 or the next button pressed becomes the source" };
        var rev = new CheckBox { SizeFlagsHorizontal = SizeFlags.ShrinkCenter };
        SpinBox Spin() => new() { MinValue = 800, MaxValue = 2200, CustomMinimumSize = new Vector2(92, 0) };
        var min = Spin(); var cen = Spin(); var max = Spin();
        var bar = new ChannelBar();
        _bars[c] = bar;

        void Refresh()
        {
            var s = RcSource.Parse(t.Source![c]);
            src.Selected = SourceIndex(s);
            src.TooltipText = s.Kind == "ui" ? UiSourceText : "";
            pair.Visible = s.Kind == "buttons";
            if (s.Kind == "buttons") pair.Text = $"{s.A},{s.B}";
            rev.SetPressedNoSignal(t.Reverse[c]);
            min.SetValueNoSignal(t.MinUs[c]); cen.SetValueNoSignal(t.CenterUs![c]); max.SetValueNoSignal(t.MaxUs[c]);
            name.Text = RcNames.Long(c);
            bar.SetDimmed(c >= 4 && s.Kind == "none");   // 1..4: the keyboard layer is always there
        }
        void SetSource(string s) { t.Source![c] = RcSource.Parse(s).ToString(); Refresh(); }
        src.ItemSelected += i =>
        {
            int k = (int)i;
            if (k == SrcPair) { SetSource($"buttons {(string.IsNullOrWhiteSpace(pair.Text) ? "0,1" : pair.Text)}"); pair.GrabFocus(); }
            else SetSource(k == 0 ? "none" : k == 1 ? "ui" : k < SrcButton0 ? $"axis {k - SrcAxis0}" : $"button {k - SrcButton0}");
        };
        pair.TextSubmitted += x => SetSource($"buttons {x}");
        pair.FocusExited += () => { if (pair.Visible) SetSource($"buttons {pair.Text}"); };
        detect.Pressed += () => StartDetect(c, detect);
        rev.Toggled += v => t.Reverse[c] = v;
        // keep min < centre < max: a value that would cross its neighbour stops one µs short of it
        min.ValueChanged += d => { t.MinUs[c] = Math.Min((int)d, t.CenterUs![c] - 1); Refresh(); };
        cen.ValueChanged += d => { t.CenterUs![c] = Math.Clamp((int)d, t.MinUs[c] + 1, t.MaxUs[c] - 1); Refresh(); };
        max.ValueChanged += d => { t.MaxUs[c] = Math.Max((int)d, t.CenterUs![c] + 1); Refresh(); };
        _rowRefresh.Add(Refresh);
        Refresh();
        var srcCell = new HBoxContainer(); srcCell.AddChild(src); srcCell.AddChild(pair);   // pair shown for "buttons n,m"
        foreach (var w in new Control[] { name, srcCell, detect, rev, min, cen, max, bar }) grid.AddChild(w);
    }

    private void RefreshRows() { foreach (var r in _rowRefresh) r(); }

    private void RefreshDevices()
    {
        _devicesDirty = false;
        _device.Clear();
        var pads = Joy.Reader.Pads();
        if (pads.Length == 0) { _device.AddItem("no joypad", -1); _device.SetItemDisabled(0, true); _device.Selected = 0; return; }
        foreach (var p in pads) _device.AddItem($"{p}: {Joy.Reader.Name(p)}", p);
        int sel = _device.GetItemIndex(_rc.ActiveDevice >= 0 ? _rc.ActiveDevice : S.Transmitter.Device);
        _device.Selected = Math.Max(0, sel);
    }

    private void ToggleCalibrate()
    {
        int dev = _rc.ActiveDevice;
        if (!_calibrating)
        {
            if (dev < 0) { _calInfo.Text = "no joypad connected: nothing to calibrate"; return; }
            _calibrating = true;
            for (int a = 0; a < Joy.Axes; a++) _calMin[a] = _calMax[a] = Joy.Reader.Axis(dev, a);
            _calBtn.Text = "Done";
            _calInfo.Text = "Calibrating: move all sticks and switches to their extremes, then centre them and press Done.";
            return;
        }
        _calibrating = false;
        _calBtn.Text = "Calibrate";
        var t = S.Transmitter;
        var done = new List<string>();
        for (int a = 0; a < Joy.Axes; a++)
        {
            float c = dev >= 0 ? Joy.Reader.Axis(dev, a) : 0f;
            int lo = RcMath.ToUs(_calMin[a]), mid = RcMath.ToUs(c), hi = RcMath.ToUs(_calMax[a]);
            if (hi - lo < 100) continue;                 // axis not moved (or unused)
            // a switch parked at an end during Done: centre = middle of the range
            if (mid - lo < 20 || hi - mid < 20) mid = (lo + hi) / 2;
            bool used = false;
            for (int ch = 0; ch < 16; ch++)
            {
                var s = RcSource.Parse(t.Source![ch]);
                if (s.Kind != "axis" || s.A != a) continue;
                t.MinUs[ch] = lo; t.CenterUs![ch] = mid; t.MaxUs[ch] = hi; used = true;
            }
            if (used) done.Add($"a{a} {lo}/{mid}/{hi}");
        }
        _calInfo.Text = done.Count > 0 ? "calibrated (min/centre/max µs): " + string.Join(", ", done) : "calibration: no mapped axis moved";
        RefreshRows();
    }

    private void StartDetect(int ch, Button btn)
    {
        if (_detectBtn != null) _detectBtn.Text = "Detect";
        if (_detectCh == ch) { _detectCh = -1; _detectBtn = null; return; }   // second click cancels
        int dev = _rc.ActiveDevice;
        if (dev < 0) { _calInfo.Text = "no joypad connected: nothing to detect"; return; }
        _detectCh = ch; _detectBtn = btn; btn.Text = "move…";
        for (int a = 0; a < Joy.Axes; a++) _detectAxes[a] = Joy.Reader.Axis(dev, a);
        for (int b = 0; b < Joy.Buttons; b++) _detectButtons[b] = Joy.Reader.Button(dev, b);
    }

    private void PollDetect(int dev)
    {
        if (_detectCh < 0 || dev < 0) return;
        string? found = null;
        for (int a = 0; a < Joy.Axes && found == null; a++) if (Mathf.Abs(Joy.Reader.Axis(dev, a) - _detectAxes[a]) > 0.5f) found = $"axis {a}";
        for (int b = 0; b < Joy.Buttons && found == null; b++)
        {
            bool p = Joy.Reader.Button(dev, b);
            if (p && !_detectButtons[b]) found = $"button {b}";
            _detectButtons[b] = p;
        }
        if (found == null) return;
        S.Transmitter.Source![_detectCh] = found;
        _calInfo.Text = $"channel {_detectCh + 1} ← {found}";
        if (_detectBtn != null) _detectBtn.Text = "Detect";
        _detectCh = -1; _detectBtn = null;
        RefreshRows();
    }

    private void UpdateTransmitterTab()
    {
        if (_devicesDirty) RefreshDevices();
        int dev = _rc.ActiveDevice;
        for (int a = 0; a < Joy.Axes; a++)
        {
            if (dev < 0) { _raw[a].Text = $"a{a}  -"; continue; }
            float v = Joy.Reader.Axis(dev, a);
            if (_calibrating) { _calMin[a] = Mathf.Min(_calMin[a], v); _calMax[a] = Mathf.Max(_calMax[a], v); }
            _raw[a].Text = $"a{a}  " + v.ToString("+0.00;-0.00; 0.00", System.Globalization.CultureInfo.InvariantCulture);
        }
        if (dev >= 0)
        {
            var pressed = new List<int>();
            for (int b = 0; b < Joy.Buttons; b++) if (Joy.Reader.Button(dev, b)) pressed.Add(b);
            _rawButtons.Text = "buttons pressed: " + (pressed.Count > 0 ? string.Join(" ", pressed) : "none");
        }
        else _rawButtons.Text = "buttons: -";
        _joyLine.Text = JoypadLine(dev);
        PollDetect(dev);
        for (int ch = 0; ch < 16; ch++) { _bars[ch].Set(_rc.ChannelsUs[ch]); _bars[ch].SetMark(RcInput.Mark(_rc.Owner[ch])); }
    }

    // ---- Keys: function | bind | Bind | Clear ----
    private string? _capturing;                          // function id waiting for its next key / joypad button
    private readonly Dictionary<string, (Label Bind, Button Btn)> _keyRows = new();
    private Label _keysInfo = null!;

    private Control BuildKeys()
    {
        var v = new VBoxContainer(); v.AddThemeConstantOverride("separation", 6);
        var m = Manifest.Current;
        v.AddChild(new Label { Text = "Bind: press it, then the key (with Ctrl/Shift/Alt if wanted) or a joypad button — a radio switch in USB-joystick mode can fire a fault. Escape cancels. A bind used elsewhere moves here.", Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart });
        // one grid per group (Sticks / Transmitter switches / Application), under its title; the same column widths
        GridContainer? grid = null;
        string? lastGroup = null;
        Keybinds.Group? lastTitle = null;
        foreach (var f in Keybinds.All(m).OrderBy(f => (int)Keybinds.GroupOf(f.Id)))   // stable: the table's order inside a group
        {
            var g = Keybinds.GroupOf(f.Id);
            if (g != lastTitle || grid == null)
            {
                var title = new Label { Text = Keybinds.GroupTitle(g), Name = "keys_group_" + g };
                title.AddThemeColorOverride("font_color", new Color(0.95f, 0.6f, 0.15f));
                var head = Ui.Row(title, new Label { Text = GroupNote(g), Modulate = Ui.Soft });
                if (lastTitle != null) v.AddChild(new Control { CustomMinimumSize = new Vector2(0, 6) });
                v.AddChild(head);
                grid = new GridContainer { Columns = 4 };
                grid.AddThemeConstantOverride("h_separation", 18); grid.AddThemeConstantOverride("v_separation", 2);
                v.AddChild(grid);
                lastTitle = g; lastGroup = null;
            }
            var group = f.Id.StartsWith("fault:") ? "faults" : f.Id.StartsWith("window:") ? "windows" : "bench";
            if (group != lastGroup && lastGroup != null)
                for (int i = 0; i < 4; i++) grid.AddChild(new Control { CustomMinimumSize = new Vector2(0, 8) });
            lastGroup = group;
            var id = f.Id;
            var name = new Label { Text = KeyLabel(f), CustomMinimumSize = new Vector2(440, 0), TooltipText = id, MouseFilter = MouseFilterEnum.Pass };
            if (id.StartsWith("fault:") && m.AllFaults().FirstOrDefault(x => Keybinds.FaultId(x.Device.Id, x.Fault.Id) == id) is { Device: not null } fx)
                name.TooltipText = $"{id}: {fx.Fault.Label}{(fx.Device.Implemented ? "" : " (device not modelled yet)")}";
            var bind = new Label { CustomMinimumSize = new Vector2(130, 0) };
            var btn = new Button { Text = "Bind", CustomMinimumSize = new Vector2(110, 0) };
            btn.Pressed += () => StartCapture(id);
            var clear = new Button { Text = "Clear" };
            clear.Pressed += () => { Keybinds.Clear(id); _capturing = null; RefreshKeys(); S.Save(); };
            grid.AddChild(name); grid.AddChild(bind); grid.AddChild(btn); grid.AddChild(clear);
            _keyNames[id] = name;
            _keyRows[id] = (bind, btn);
        }
        _keysInfo = new Label { Text = $"faults from the manifest ({m.Source}); also: mouse drag = orbit, wheel = zoom, Escape = leave a text field", Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart };
        v.AddChild(_keysInfo);
        RefreshKeys();
        return v;
    }

    /// Opened over a bench (Flight.OpenSettings): its Transmitter rows, so switch_N says what it is there.
    public List<DerivedSwitch>? BenchSwitches;

    private static string GroupNote(Keybinds.Group g) => g switch
    {
        Keybinds.Group.Sticks => "channels 1..4, beside the joypad: the last input that moved a channel holds it",
        Keybinds.Group.Switches => "switch N steps the N-th Transmitter row",
        _ => "",
    };

    /// A function's label; switch_N over a bench names its row: `Switch 1 (ch5 ARM)`, plain `Switch N` otherwise.
    private string KeyLabel(BenchFunction f) => Keybinds.GroupOf(f.Id) == Keybinds.Group.Switches && BenchSwitches != null ? SwitchLabel(f, BenchSwitches) : f.Label;

    public static string SwitchLabel(BenchFunction f, List<DerivedSwitch> rows)
    {
        int n = f.Id[^1] - '0';
        if (n < 1 || n > rows.Count) return f.Label;
        var sw = rows[n - 1];
        var what = sw.Label ?? string.Join("/", sw.Positions.Select(p => p.Label).Where(l => l != "—" && l != "").Distinct());
        return $"{f.Label} (ch{sw.Channel}{(what != "" ? " " + what : "")})";
    }

    /// The label a Keys row shows now (self-tests).
    public string? KeyRowLabel(string id) => _keyNames.TryGetValue(id, out var l) ? l.Text : null;
    private readonly Dictionary<string, Label> _keyNames = new();

    private void StartCapture(string id)
    {
        _capturing = _capturing == id ? null : id;   // second click cancels
        RefreshKeys();
    }

    private void RefreshKeys()
    {
        foreach (var (id, row) in _keyRows)
        {
            var b = Keybinds.Bind(id);
            row.Bind.Text = b != "" ? b : "—";
            row.Bind.Modulate = b != "" ? Colors.White : Ui.Dim;
            row.Btn.Text = _capturing == id ? "press a key…" : "Bind";
        }
    }

    /// While a Bind is waiting: the next key press / joypad button becomes the bind (before the GUI sees it).
    public override void _Input(InputEvent e)
    {
        if (_capturing == null) return;
        if (e is InputEventKey { Pressed: true, Keycode: Key.Escape }) { _capturing = null; RefreshKeys(); GetViewport().SetInputAsHandled(); return; }
        var name = Keybinds.EventName(e);
        if (name == null) return;
        GetViewport().SetInputAsHandled();
        var id = _capturing;
        _capturing = null;
        Keybinds.Set(id, name, Manifest.Current);
        _keysInfo.Text = $"{id} = {name}";
        RefreshKeys();
        S.Save();
    }

    /// Over the bench (Overlay): Escape not taken by a field / popup / Bind closes the overlay (as Back does).
    public override void _UnhandledInput(InputEvent e)
    {
        if (!Overlay || _capturing != null || e is not InputEventKey { Pressed: true, Echo: false, Keycode: Key.Escape }) return;
        GetViewport().SetInputAsHandled();
        if (Ui.TextFocus()) { GetViewport().GuiReleaseFocus(); return; }   // first Escape leaves a text field
        Closed?.Invoke();
    }

    public override void _Process(double delta)
    {
        _rc.Update(delta);
        StickView.ShowSticks(_left, _right, _rc.Sticks, S.Transmitter.Mode);
        UpdateTransmitterTab();
    }

    public override void _ExitTree()
    {
        Joy.Changed -= OnJoyChanged;
        _rc.Dispose();
        if (DevEnv.SaveSettings) S.Save();   // Back, or the window closed on this screen
    }
}

/// A stick gimbal: square with cross-hairs and the stick position.
public partial class StickView : Control
{
    private float _x, _y;
    public void Set(float x, float y) { _x = x; _y = y; QueueRedraw(); }
    /// sticks = roll, pitch, yaw, collective (RcInput.Sticks) placed per transmitter mode.
    /// Mode 2: left = collective/yaw, right = pitch/roll. Mode 1: left = pitch/yaw, right = collective/roll.
    public static void ShowSticks(StickView left, StickView right, float[] s, int mode)
    {
        switch (mode)
        {
            case 1: left.Set(s[2], s[1]); right.Set(s[0], s[3]); break;
            case 3: left.Set(s[0], s[3]); right.Set(s[2], s[1]); break;
            case 4: left.Set(s[0], s[1]); right.Set(s[2], s[3]); break;
            default: left.Set(s[2], s[3]); right.Set(s[0], s[1]); break;
        }
    }
    public override void _Draw()
    {
        var r = new Rect2(Vector2.Zero, Size);
        DrawRect(r, new Color(0.15f, 0.2f, 0.3f));
        DrawRect(r, new Color(0.6f, 0.7f, 0.9f), false, 2);
        DrawLine(new Vector2(Size.X / 2, 0), new Vector2(Size.X / 2, Size.Y), new Color(0.4f, 0.45f, 0.55f));
        DrawLine(new Vector2(0, Size.Y / 2), new Vector2(Size.X, Size.Y / 2), new Color(0.4f, 0.45f, 0.55f));
        var p = new Vector2(Size.X / 2 + _x * Size.X / 2 * 0.92f, Size.Y / 2 - _y * Size.Y / 2 * 0.92f);
        DrawCircle(p, 8, new Color(0.95f, 0.6f, 0.15f));
    }
}
