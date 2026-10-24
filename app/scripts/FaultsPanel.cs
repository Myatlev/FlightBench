// Faults window of the flight screen: one row per fault of the manifest, grouped by device — the
// devices present on the bench (wired, or onboard and not detached) first; the rest in a collapsed
// "other devices" group, greyed and disabled ("not wired": the core rejects a fault for them). A row: toggle
// (sends `fault` on/off, with `params` for faults that have them), its params (required int/float →
// slider, optional → check box + spin box), the keybind (Settings → Keys). A row is lit while the
// core lists the fault in `state.faults` — the core's word, not the toggle's.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public partial class FaultsPanel : VBoxContainer
{
    /// Sends a `fault` message (Dictionary so the params stay typed); false = not sent (not connected):
    /// the toggle goes back.
    public Func<Dictionary<string, object>, bool>? Send;

    private sealed class Row
    {
        public string Device = "", Kind = "";
        public ManifestFault Fault = null!;
        public PanelContainer Panel = null!;
        public CheckButton Toggle = null!;
        public Label Bind = null!;
        public bool Group;                                   // in the "other devices" group
        public ulong PendingUntil;                            // after a click: ignore state.faults until it agrees or this passes
        public readonly Dictionary<string, Func<object?>> Params = new();   // param id → value (null = leave out)
        public readonly Dictionary<string, Action<double>> SetParam = new();
    }

    private readonly Dictionary<string, Row> _rows = new();   // "device.kind"
    private HashSet<string> _active = new(), _next = new();    // swapped on every state (no allocation)
    private VBoxContainer _list = null!, _other = null!;
    private Button? _otherHeader;
    private int _otherCount;
    private Label _foot = null!;
    private StyleBoxFlat _litBox = null!, _idleBox = null!;

    public IReadOnlyCollection<string> Active => _active;

    public FaultsPanel()
    {
        SizeFlagsVertical = SizeFlags.ExpandFill;
        _litBox = Ui.Box(new Color(0.45f, 0.24f, 0.04f, 0.95f), new Color(1f, 0.62f, 0.2f), 4, 8, 4);
        _idleBox = Ui.Box(new Color(0.05f, 0.10f, 0.20f, 0.6f), new Color(0.3f, 0.4f, 0.55f, 0.35f), 4, 8, 4);
        _list = new VBoxContainer { SizeFlagsHorizontal = SizeFlags.ExpandFill };
        _list.AddThemeConstantOverride("separation", 4);
        var scroll = Ui.Scroll(_list);
        scroll.SizeFlagsVertical = SizeFlags.ExpandFill;
        AddChild(scroll);
        _foot = new Label { Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart };
        AddChild(_foot);
    }

    /// (Re)builds the rows from the manifest; `wired` = device ids present on the bench (BenchData.PresentDevices).
    public void Build(Manifest m, IEnumerable<string> wired)
    {
        var keep = _rows.ToDictionary(kv => kv.Key, kv => kv.Value.Params.ToDictionary(p => p.Key, p => p.Value()));
        foreach (var c in _list.GetChildren()) { _list.RemoveChild(c); c.QueueFree(); }
        _rows.Clear(); _otherHeader = null; _otherCount = 0;
        var wiredSet = wired.ToList();
        var withFaults = m.Devices.Where(d => d.Faults.Count > 0).ToList();
        var mine = withFaults.Where(d => wiredSet.Contains(d.Id)).OrderBy(d => wiredSet.IndexOf(d.Id)).ToList();
        var rest = withFaults.Where(d => !wiredSet.Contains(d.Id)).ToList();
        foreach (var d in mine) AddGroup(_list, d, false);
        if (rest.Count > 0)
        {
            _other = new VBoxContainer { Visible = false };
            _other.AddThemeConstantOverride("separation", 4);
            _otherCount = rest.Sum(d => d.Faults.Count);
            _otherHeader = new Button { Flat = true, Alignment = HorizontalAlignment.Left, TooltipText = "faults of devices that are not wired on this bench (disabled: the core rejects them)" };
            _otherHeader.Pressed += () => { _other.Visible = !_other.Visible; UpdateOtherHeader(); };
            _list.AddChild(_otherHeader);
            _list.AddChild(_other);
            foreach (var d in rest) AddGroup(_other, d, true);
            UpdateOtherHeader();
        }
        if (withFaults.Count == 0)
            _list.AddChild(new Label { Text = "the core declares no faults", Modulate = Ui.Soft });
        // params typed in before a rebuild (the live manifest replacing the cached one) survive it
        foreach (var (key, vals) in keep)
            if (_rows.TryGetValue(key, out var r))
                foreach (var (id, v) in vals)
                    if (v is IConvertible c && r.SetParam.TryGetValue(id, out var set)) set(c.ToDouble(CultureInfo.InvariantCulture));
        _foot.Text = $"manifest: {m.Source}  ·  keys: Settings → Keys";
        Apply();
    }

    private void AddGroup(VBoxContainer parent, ManifestDevice d, bool other)
    {
        var head = new Label { Text = $"{d.Id} · {d.DisplayLabel}{(other ? "  (not wired)" : "")}" };
        head.AddThemeFontSizeOverride("font_size", 15);
        head.Modulate = other ? new Color(0.6f, 0.62f, 0.68f) : new Color(0.75f, 0.85f, 1f);
        parent.AddChild(head);
        foreach (var f in d.Faults) parent.AddChild(MakeRow(d, f, other));
    }

    private Control MakeRow(ManifestDevice d, ManifestFault f, bool other)
    {
        var r = new Row { Device = d.Id, Kind = f.Id, Fault = f, Group = other };
        var key = $"{d.Id}.{f.Id}";
        r.Panel = new PanelContainer();
        r.Panel.AddThemeStyleboxOverride("panel", _idleBox);
        var col = new VBoxContainer(); col.AddThemeConstantOverride("separation", 2);
        r.Panel.AddChild(col);
        var top = new HBoxContainer();
        r.Toggle = new CheckButton { FocusMode = FocusModeEnum.None, Text = f.Label, SizeFlagsHorizontal = SizeFlags.ExpandFill, TooltipText = $"{f.Label}\n{key}: sends {{\"type\":\"fault\",\"device\":\"{d.Id}\",\"kind\":\"{f.Id}\",\"on\":…}}", ClipText = true };
        if (!d.Implemented) { r.Toggle.Disabled = true; r.Toggle.TooltipText = "the core does not model this device yet"; }
        else if (other) { r.Toggle.Disabled = true; r.Toggle.Text = f.Label + "  · not wired"; r.Toggle.TooltipText = $"{d.Id} is not wired on this bench (nor onboard): wire it in the bench (Bench → Edit bench…) to inject its faults"; }
        if (other) r.Panel.Modulate = new Color(1, 1, 1, 0.55f);
        r.Toggle.Toggled += on => SendFault(r, on);
        r.Bind = new Label { CustomMinimumSize = new Vector2(70, 0), HorizontalAlignment = HorizontalAlignment.Right, Modulate = Ui.Soft, TooltipText = "keybind (Settings → Keys)", MouseFilter = MouseFilterEnum.Pass };
        top.AddChild(r.Toggle); top.AddChild(r.Bind);
        col.AddChild(top);
        foreach (var p in f.Params) col.AddChild(ParamEditor(r, p));
        _rows[key] = r;
        RefreshBind(r);
        return r.Panel;
    }

    private Control ParamEditor(Row r, ManifestParam p)
    {
        var inv = CultureInfo.InvariantCulture;
        bool isInt = p.Type == "int";
        double min = p.Min ?? 0, max = p.Max ?? 100;
        double start = double.TryParse(p.DefaultText, NumberStyles.Float, inv, out var dv) ? dv : Math.Round((min + max) / 2);
        var row = new HBoxContainer();
        row.AddChild(new Control { CustomMinimumSize = new Vector2(16, 0) });
        object Val(double v) => isInt ? (object)(long)Math.Round(v) : v;
        if (!p.Optional)
        {
            // required: slider (a param without default must be sent when turning the fault on)
            var name = new Label { Text = p.Label != "" ? p.Label : p.Id, CustomMinimumSize = new Vector2(120, 0), Modulate = Ui.Soft };
            var slider = new HSlider { MinValue = min, MaxValue = max, Step = isInt ? 1 : (max - min) / 200, Value = start, SizeFlagsHorizontal = SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(120, 0), SizeFlagsVertical = SizeFlags.ShrinkCenter };
            var val = new Label { Text = Fmt(start), CustomMinimumSize = new Vector2(44, 0), HorizontalAlignment = HorizontalAlignment.Right };
            bool dragging = false;
            slider.DragStarted += () => dragging = true;
            slider.DragEnded += changed => { dragging = false; if (changed) Resend(r); };
            slider.ValueChanged += v => { val.Text = Fmt(v); if (!dragging) Resend(r); };
            r.Params[p.Id] = () => Val(slider.Value);
            r.SetParam[p.Id] = v => slider.SetValueNoSignal(v);
            r.SetParam[p.Id] += v => val.Text = Fmt(v);
            row.AddChild(name); row.AddChild(slider); row.AddChild(val);
        }
        else
        {
            // optional: sent only when checked (unchecked = the device keeps its normal value)
            var use = new CheckBox { Text = p.Label != "" ? p.Label : p.Id, CustomMinimumSize = new Vector2(120, 0), TooltipText = "optional: sent only when checked" };
            var spin = new SpinBox { MinValue = min, MaxValue = max, Step = isInt ? 1 : 0.01, Rounded = isInt, Value = start, Editable = false, CustomMinimumSize = new Vector2(90, 0) };
            use.Toggled += on => { spin.Editable = on; Resend(r); };
            spin.ValueChanged += _ => Resend(r);
            r.Params[p.Id] = () => use.ButtonPressed ? Val(spin.Value) : null;
            r.SetParam[p.Id] = v => { use.SetPressedNoSignal(true); spin.Editable = true; spin.SetValueNoSignal(v); };
            row.AddChild(use); row.AddChild(spin);
        }
        return row;
        string Fmt(double v) => isInt ? ((long)Math.Round(v)).ToString(inv) : v.ToString("0.##", inv);
    }

    /// The `fault` message: params only when turning on.
    public static Dictionary<string, object> Message(string device, string kind, bool on, Dictionary<string, object>? prms)
    {
        var m = new Dictionary<string, object> { ["type"] = "fault", ["device"] = device, ["kind"] = kind, ["on"] = on };
        if (on && prms is { Count: > 0 }) m["params"] = prms;
        return m;
    }

    private Dictionary<string, object> ParamsOf(Row r)
    {
        var d = new Dictionary<string, object>();
        foreach (var (id, get) in r.Params) if (get() is { } v) d[id] = v;
        return d;
    }

    private bool SendFault(Row r, bool on)
    {
        if (Send?.Invoke(Message(r.Device, r.Kind, on, ParamsOf(r))) != true)
        {
            r.Toggle.SetPressedNoSignal(_active.Contains($"{r.Device}.{r.Kind}"));   // not sent: show what the core has
            return false;
        }
        r.PendingUntil = Time.GetTicksMsec() + 600;
        Paint(r, on);
        return true;
    }

    /// Param changed while the fault is on: send it again with the new values.
    private void Resend(Row r) { if (r.Toggle.ButtonPressed) SendFault(r, true); }

    /// Flips a fault (keybind / script): on unless the core (or a click just now) says it is on.
    public bool Toggle(string key)
    {
        if (!_rows.TryGetValue(key, out var r) || r.Toggle.Disabled) return false;
        bool on = !r.Toggle.ButtonPressed;
        r.Toggle.SetPressedNoSignal(on);
        SendFault(r, on);
        return true;
    }

    /// Switches a fault on or off (the radio-unplugged link loss); false = no such row, disabled, or not sent.
    public bool Set(string key, bool on)
    {
        if (!_rows.TryGetValue(key, out var r) || r.Toggle.Disabled) return false;
        r.Toggle.SetPressedNoSignal(on);
        return SendFault(r, on);
    }

    /// Dev/script: set a param of a fault row (as if the slider / spin box had been moved, without sending).
    public void SetParam(string key, string param, double value)
    {
        if (_rows.TryGetValue(key, out var r) && r.SetParam.TryGetValue(param, out var set)) set(value);
    }

    /// Can this fault row be switched (device present and modelled)? Self-tests / scripts.
    public bool IsEnabled(string key) => _rows.TryGetValue(key, out var r) && !r.Toggle.Disabled;

    public bool IsOn(string key) => _rows.TryGetValue(key, out var r) && r.Toggle.ButtonPressed;

    /// `state.faults` → lit rows and toggle states.
    public void OnState(JsonElement s)
    {
        if (!s.TryGetProperty("faults", out var f) || f.ValueKind != JsonValueKind.Array) return;
        bool changed = f.GetArrayLength() != _active.Count;
        if (!changed)
            foreach (var x in f.EnumerateArray())
                if (x.ValueKind != JsonValueKind.String || !ContainsName(_active, x)) { changed = true; break; }
        if (changed)
        {
            _next.Clear();
            foreach (var x in f.EnumerateArray()) if (x.ValueKind == JsonValueKind.String) _next.Add(x.GetString()!);
            (_active, _next) = (_next, _active);
        }
        if (changed || AnyPending()) Apply();
    }

    private static bool ContainsName(HashSet<string> set, JsonElement x)
    {
        foreach (var k in set) if (x.ValueEquals(k)) return true;
        return false;
    }

    private bool AnyPending()
    {
        foreach (var r in _rows.Values) if (r.PendingUntil != 0) return true;
        return false;
    }

    private void Apply()
    {
        var t = Time.GetTicksMsec();
        bool otherLit = false;
        foreach (var (key, r) in _rows)
        {
            bool on = _active.Contains(key);
            if (r.PendingUntil != 0)
            {
                if (on == r.Toggle.ButtonPressed || t > r.PendingUntil) r.PendingUntil = 0;
                else continue;                                     // the core has not caught up with the click yet
            }
            r.Toggle.SetPressedNoSignal(on);
            Paint(r, on);
            if (on && r.Group) otherLit = true;
        }
        if (otherLit && _otherHeader != null && !_other.Visible) _other.Visible = true;   // an active fault is never hidden
        UpdateOtherHeader();
    }

    private void Paint(Row r, bool lit) => r.Panel.AddThemeStyleboxOverride("panel", lit && _active.Contains($"{r.Device}.{r.Kind}") ? _litBox : _idleBox);

    private void UpdateOtherHeader()
    {
        if (_otherHeader == null) return;
        int lit = _rows.Values.Count(r => r.Group && _active.Contains($"{r.Device}.{r.Kind}"));
        _otherHeader.Text = $"{(_other.Visible ? "▾" : "▸")} other devices ({_otherCount} faults{(lit > 0 ? $", {lit} active" : "")})";
    }

    private static void RefreshBind(Row r)
    {
        var b = Keybinds.Bind(Keybinds.FaultId(r.Device, r.Kind));
        r.Bind.Text = b != "" ? b : "—";
    }

    public void RefreshBinds() { foreach (var r in _rows.Values) RefreshBind(r); }
}
