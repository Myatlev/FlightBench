// Signals window: every value that goes into or out of the flight controller is a named signal.
// Sources: the `state` stream (flattened into ids like euler_deg.roll, pwm_us.1, fc.cpu_pct) and the
// core's own `signals` list, streamed as `samples` after `subscribe` (docs/protocol.md).
// The panel lists signals (filter, last value, unit), plots the checked ones on a shared virtual-time
// axis, and records the checked ones to CSV one row per state tick. Only checked signals keep history
// (ring buffers); the rest keep their last value. Virtual time going backwards (a restarted core),
// `hello` and a new connection clear the data; a CSV being recorded gets a "# restart" line.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.Json;
using Godot;

namespace FlightBench;

public sealed class SignalTrace
{
    public string Id;
    public string Unit = "";
    public string Source;                       // "state" or "core"
    private double[]? _t, _v;                   // ring buffer, only while the trace is plotted (Keep)
    private int _head, _count;
    public double Last = double.NaN;
    public SignalTrace(string id, string source) { Id = id; Source = source; }
    public int Count => _count;
    public bool HasBuffer => _t != null;
    /// Start keeping history (checked); `false` drops it (unchecked: last value only).
    public void Keep(bool on, int capacity)
    {
        if (on && _t == null) { _t = new double[capacity]; _v = new double[capacity]; _head = _count = 0; }
        else if (!on) { _t = _v = null; _head = _count = 0; }
    }
    public void Add(double t, double v)
    {
        Last = v;
        if (_t == null) return;
        _t[_head] = t; _v![_head] = v;
        _head = (_head + 1) % _t.Length;
        if (_count < _t.Length) _count++;
    }
    public void Clear() { _head = 0; _count = 0; Last = double.NaN; }
    /// i = 0 is the oldest sample (only while HasBuffer).
    public (double t, double v) At(int i) { var k = (_head - _count + i + _t!.Length) % _t.Length; return (_t[k], _v![k]); }
}

public sealed class SignalStore
{
    public const int Capacity = 30000;          // 5 min at 100 Hz per plotted trace
    public readonly Dictionary<string, SignalTrace> Traces = new();
    public event Action? ListChanged;
    /// Virtual time went backwards (from, to): the data was cleared before the new sample.
    public event Action<double, double>? Restarted;
    public double LastT { get; private set; }
    public bool HasT { get; private set; }

    /// Start of a new time line (hello, new connection): every trace's data cleared, time unknown.
    public void Reset()
    {
        foreach (var tr in Traces.Values) tr.Clear();
        LastT = 0; HasT = false;
    }

    /// Every sample's time goes through here: a step back means a restarted core.
    private void Advance(double t)
    {
        if (HasT && t < LastT - 1e-9)
        {
            var from = LastT;
            Reset();
            Restarted?.Invoke(from, t);
        }
        if (!HasT || t > LastT) LastT = t;
        HasT = true;
    }

    public SignalTrace Get(string id, string source, string unit = "")
    {
        if (!Traces.TryGetValue(id, out var tr))
        {
            tr = new SignalTrace(id, source) { Unit = unit };
            Traces[id] = tr;
            ListChanged?.Invoke();
        }
        else
        {
            if (unit != "" && tr.Unit == "") tr.Unit = unit;
            // a state field checked before the first state arrived was guessed as "core": the stream is authoritative
            if (source == "state" && tr.Source != "state") { tr.Source = "state"; ListChanged?.Invoke(); }
        }
        return tr;
    }

    /// The core's `signals` list: register ids and units, no data until subscribed.
    public void Register(JsonElement msg)
    {
        if (!msg.TryGetProperty("items", out var items)) return;
        foreach (var it in items.EnumerateArray())
        {
            var id = it.TryGetProperty("id", out var i) ? i.GetString() : null;
            if (string.IsNullOrEmpty(id)) continue;
            Get(id, "core", it.TryGetProperty("unit", out var u) ? u.GetString() ?? "" : "");
        }
    }

    /// `samples`: {"t":..,"v":{id:value}}
    public void Samples(JsonElement msg)
    {
        if (!Json.TryNum(msg, "t", out var t) || !msg.TryGetProperty("v", out var v) || v.ValueKind != JsonValueKind.Object) return;
        Advance(t);
        foreach (var p in v.EnumerateObject())
            if (p.Value.ValueKind == JsonValueKind.Number) Get(p.Name, "core").Add(t, p.Value.GetDouble());
    }

    private static readonly Dictionary<string, string[]> ArrayNames = new()
    {
        ["pos_ned"] = new[] { "n", "e", "d" }, ["vel_ned"] = new[] { "n", "e", "d" },
        ["euler_deg"] = new[] { "roll", "pitch", "yaw" }, ["gyro_dps"] = new[] { "x", "y", "z" },
        ["accel_g"] = new[] { "x", "y", "z" }, ["quat"] = new[] { "w", "x", "y", "z" },
    };

    /// `state`: every numeric/bool field becomes a signal; arrays get names (pos_ned.n) or 1-based indices
    /// (pwm_us.1). `flatten` false: only the time is taken (nobody looks at the values: skip the work).
    public void State(JsonElement s, bool flatten = true)
    {
        if (!Json.TryNum(s, "t", out var t)) return;
        Advance(t);
        if (!flatten || s.ValueKind != JsonValueKind.Object) return;
        foreach (var p in s.EnumerateObject())
        {
            if (p.NameEquals("type") || p.NameEquals("t")) continue;
            Flatten(Top(p), p.Value, t);
        }
    }

    // ids built once: top-level name → string, (parent id, child name / index) → "parent.child"
    private readonly Dictionary<string, string> _top = new();
    private readonly Dictionary<string, Dictionary<string, string>> _child = new();
    private readonly Dictionary<string, string[]> _indexIds = new();

    private string Top(JsonProperty p)
    {
        foreach (var (k, v) in _top) if (p.NameEquals(k)) return v;
        var name = p.Name;
        _top[name] = name;
        return name;
    }

    private string Child(string prefix, JsonProperty p)
    {
        if (!_child.TryGetValue(prefix, out var d)) _child[prefix] = d = new Dictionary<string, string>();
        foreach (var (k, v) in d) if (p.NameEquals(k)) return v;
        var name = p.Name;
        return d[name] = prefix + "." + name;
    }

    private string Index(string prefix, int i)
    {
        if (!_indexIds.TryGetValue(prefix, out var ids) || ids.Length <= i)
        {
            var last = prefix.Contains('.') ? prefix[(prefix.LastIndexOf('.') + 1)..] : prefix;
            ArrayNames.TryGetValue(last, out var names);
            var n = Math.Max(i + 1, ids?.Length ?? 0);
            var grown = new string[n];
            for (int k = 0; k < n; k++) grown[k] = ids != null && k < ids.Length ? ids[k] : prefix + "." + (names != null && k < names.Length ? names[k] : (k + 1).ToString(CultureInfo.InvariantCulture));
            _indexIds[prefix] = ids = grown;
        }
        return ids[i];
    }

    private void Flatten(string id, JsonElement e, double t)
    {
        switch (e.ValueKind)
        {
            case JsonValueKind.Number: Get(id, "state").Add(t, e.GetDouble()); break;
            case JsonValueKind.True: Get(id, "state").Add(t, 1); break;
            case JsonValueKind.False: Get(id, "state").Add(t, 0); break;
            case JsonValueKind.Object:
                foreach (var p in e.EnumerateObject()) Flatten(Child(id, p), p.Value, t);
                break;
            case JsonValueKind.Array:
                int i = 0;
                foreach (var it in e.EnumerateArray())
                {
                    if (it.ValueKind is JsonValueKind.Number or JsonValueKind.True or JsonValueKind.False)
                        Flatten(Index(id, i), it, t);
                    i++;
                }
                break;
        }
    }

    /// The Clear button: data of every trace dropped (time line kept).
    public void Clear() { foreach (var tr in Traces.Values) tr.Clear(); }
}

/// Multi-trace plot on a shared virtual-time axis. Each trace autoscales to its own range in the
/// window (or all share one scale). Min/max per pixel column, so spikes survive decimation.
public partial class PlotView : Control
{
    public readonly List<(SignalTrace trace, Color color)> Traces = new();
    public double WindowS = 10;                 // seconds shown; <= 0 → everything buffered
    public bool SharedScale;
    public bool Frozen;
    private double _frozenT = double.NaN;
    public double Now;                          // latest virtual time (right edge)
    private readonly Font _font = ThemeDB.FallbackFont;
    private const int FontSize = 12;
    // reused between frames
    private readonly List<(double lo, double hi)> _ranges = new();
    private float[] _mins = Array.Empty<float>(), _maxs = Array.Empty<float>();
    private readonly List<Vector2> _pts = new();
    private Vector2[] _ptsArr = Array.Empty<Vector2>();

    /// Plot data dropped (restart): the right edge goes back to `now`, a freeze is released.
    public void Restart(double now) { Now = now; _frozenT = double.NaN; }
    private const float PadL = 8, PadR = 8, PadT = 6, PadB = 20;

    public override void _Process(double delta) { if (Visible) QueueRedraw(); }

    public override void _Draw()
    {
        var size = Size;
        DrawRect(new Rect2(Vector2.Zero, size), new Color(0.08f, 0.09f, 0.11f));
        var plot = new Rect2(PadL, PadT, Math.Max(10, size.X - PadL - PadR), Math.Max(10, size.Y - PadT - PadB));
        if (Frozen && double.IsNaN(_frozenT)) _frozenT = Now; else if (!Frozen) _frozenT = double.NaN;
        var tEnd = Frozen && _frozenT <= Now ? _frozenT : Now;
        double tStart;
        if (WindowS > 0) { tStart = Math.Max(0, tEnd - WindowS); if (tEnd - tStart < 1) tStart = tEnd - 1; }   // fills from t = 0, then scrolls
        else
        {
            tStart = tEnd;
            foreach (var (tr, _) in Traces) if (tr.Count > 0) tStart = Math.Min(tStart, tr.At(0).t);
            if (tEnd - tStart < 1e-3) tStart = tEnd - 1;
        }
        var span = tEnd - tStart;
        // time grid
        var stepT = NiceStep(span / 6);
        for (var g = Math.Ceiling(tStart / stepT) * stepT; g <= tEnd + 1e-9; g += stepT)
        {
            var x = (float)(plot.Position.X + (g - tStart) / span * plot.Size.X);
            DrawLine(new Vector2(x, plot.Position.Y), new Vector2(x, plot.End.Y), new Color(1, 1, 1, 0.08f));
            DrawString(_font, new Vector2(x + 2, size.Y - 5), g.ToString(stepT < 1 ? "0.0" : "0", CultureInfo.InvariantCulture) + " s", HorizontalAlignment.Left, -1, FontSize, new Color(1, 1, 1, 0.5f));
        }
        for (int k = 1; k < 4; k++)
        {
            var y = plot.Position.Y + plot.Size.Y * k / 4f;
            DrawLine(new Vector2(plot.Position.X, y), new Vector2(plot.End.X, y), new Color(1, 1, 1, 0.06f));
        }
        if (Traces.Count == 0) { DrawString(_font, plot.Position + new Vector2(10, 20), "check signals in the list to plot them", HorizontalAlignment.Left, -1, FontSize, new Color(1, 1, 1, 0.5f)); return; }

        // ranges
        var ranges = _ranges; ranges.Clear();
        double allLo = double.MaxValue, allHi = double.MinValue;
        foreach (var (tr, _) in Traces)
        {
            double lo = double.MaxValue, hi = double.MinValue;
            for (int i = FirstIndex(tr, tStart); i < tr.Count; i++)
            {
                var (t, v) = tr.At(i);
                if (t > tEnd) break;
                if (v < lo) lo = v; if (v > hi) hi = v;
            }
            if (lo > hi) { lo = 0; hi = 1; }
            if (hi - lo < 1e-9) { lo -= 0.5; hi += 0.5; }
            ranges.Add((lo, hi));
            allLo = Math.Min(allLo, lo); allHi = Math.Max(allHi, hi);
        }
        if (SharedScale) { var pad = (allHi - allLo) * 0.05; for (int i = 0; i < ranges.Count; i++) ranges[i] = (allLo - pad, allHi + pad); }
        else for (int i = 0; i < ranges.Count; i++) { var (lo, hi) = ranges[i]; var pad = (hi - lo) * 0.05; ranges[i] = (lo - pad, hi + pad); }

        // traces: min/max per pixel column
        int cols = Math.Max(1, (int)plot.Size.X);
        if (_mins.Length != cols) { _mins = new float[cols]; _maxs = new float[cols]; }
        var mins = _mins; var maxs = _maxs;
        for (int ti = 0; ti < Traces.Count; ti++)
        {
            var (tr, color) = Traces[ti];
            var (lo, hi) = ranges[ti];
            Array.Fill(mins, float.NaN); Array.Fill(maxs, float.NaN);
            for (int i = FirstIndex(tr, tStart); i < tr.Count; i++)
            {
                var (t, v) = tr.At(i);
                if (t > tEnd) break;
                int c = (int)((t - tStart) / span * (cols - 1));
                if (c < 0 || c >= cols) continue;
                var y = (float)(plot.End.Y - (v - lo) / (hi - lo) * plot.Size.Y);
                if (float.IsNaN(mins[c]) || y < mins[c]) mins[c] = y;
                if (float.IsNaN(maxs[c]) || y > maxs[c]) maxs[c] = y;
            }
            var pts = _pts; pts.Clear();
            for (int c = 0; c < cols; c++)
            {
                if (float.IsNaN(mins[c])) continue;
                var x = plot.Position.X + c;
                pts.Add(new Vector2(x, mins[c]));
                if (maxs[c] - mins[c] > 0.5f) pts.Add(new Vector2(x, maxs[c]));
            }
            if (pts.Count >= 2)
            {
                if (_ptsArr.Length != pts.Count) _ptsArr = new Vector2[pts.Count];   // same length most frames
                pts.CopyTo(_ptsArr);
                DrawPolyline(_ptsArr, color, 1.5f, true);
            }
            else if (pts.Count == 1) DrawCircle(pts[0], 2, color);
            // legend line: id, current value, range
            var last = tr.Last;
            var text = $"{tr.Id} = {Fmt(last)}{(tr.Unit != "" ? " " + tr.Unit : "")}   [{Fmt(lo + (hi - lo) * 0.05 / 1.1)} … {Fmt(hi - (hi - lo) * 0.05 / 1.1)}]";
            DrawString(_font, plot.Position + new Vector2(6, 14 + ti * (FontSize + 3)), text, HorizontalAlignment.Left, -1, FontSize, color);
        }
        if (Frozen) DrawString(_font, new Vector2(plot.End.X - 60, plot.Position.Y + 14), "frozen", HorizontalAlignment.Left, -1, FontSize, new Color(1, 0.8f, 0.3f));
    }

    private static int FirstIndex(SignalTrace tr, double tStart)
    {
        // binary search on the ring (times are monotonic in virtual time)
        int lo = 0, hi = tr.Count;
        while (lo < hi) { var mid = (lo + hi) / 2; if (tr.At(mid).t < tStart) lo = mid + 1; else hi = mid; }
        return lo;
    }

    private static double NiceStep(double raw)
    {
        if (raw <= 0) return 1;
        var p = Math.Pow(10, Math.Floor(Math.Log10(raw)));
        var m = raw / p;
        return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p;
    }

    public static string Fmt(double v) => double.IsNaN(v) ? "-" : Math.Abs(v) >= 100 ? v.ToString("0", CultureInfo.InvariantCulture) : Math.Abs(v) >= 10 ? v.ToString("0.0", CultureInfo.InvariantCulture) : v.ToString("0.00", CultureInfo.InvariantCulture);
}

/// The window content: toolbar, signal list, plot. Owns the store; Flight feeds it messages.
public partial class SignalsPanel : VBoxContainer
{
    public readonly SignalStore Store = new();
    /// Called with the full set of checked core-published ids whenever it changes (→ `subscribe`).
    public event Action<List<string>>? SubscribeRequested;
    public event Action<string, string>? Log;

    private Tree _tree = null!;
    private TreeItem _root = null!;
    private LineEdit _filter = null!;
    private PlotView _plot = null!;
    private Label _status = null!;
    private Button _record = null!;
    private readonly Dictionary<string, TreeItem> _items = new();
    private readonly HashSet<string> _checked = new();
    private readonly Dictionary<string, Color> _colors = new();
    private static readonly Color[] Palette = {
        new(0.36f, 0.78f, 1f), new(1f, 0.64f, 0.3f), new(0.55f, 0.95f, 0.5f), new(1f, 0.45f, 0.6f),
        new(0.85f, 0.7f, 1f), new(1f, 0.9f, 0.4f), new(0.5f, 0.9f, 0.9f), new(0.9f, 0.9f, 0.9f) };
    private int _paletteNext;
    private bool _listDirty = true;
    private int _frame;
    // CSV
    private Godot.FileAccess? _csv;
    private string _csvPath = "";
    private List<string> _csvIds = new();
    public int CsvRows { get; private set; }
    public int SampleCount { get; private set; }

    public override void _Ready()
    {
        var bar = new HBoxContainer();
        _filter = new LineEdit { PlaceholderText = "filter (e.g. euler, pwm, fc.)", CustomMinimumSize = new Vector2(180, 0) };
        _filter.TextChanged += _ => _listDirty = true;
        bar.AddChild(_filter);
        var win = new OptionButton();
        foreach (var (label, s) in new[] { ("2 s", 2.0), ("5 s", 5.0), ("10 s", 10.0), ("30 s", 30.0), ("60 s", 60.0), ("all", 0.0) }) { win.AddItem(label); win.SetItemMetadata(win.ItemCount - 1, s); }
        win.Selected = 2;
        win.ItemSelected += i => _plot.WindowS = (double)win.GetItemMetadata((int)i);
        bar.AddChild(new Label { Text = " window " }); bar.AddChild(win);
        var shared = new CheckBox { Text = "same Y scale" }; shared.Toggled += v => _plot.SharedScale = v; bar.AddChild(shared);
        var freeze = new CheckBox { Text = "freeze" }; freeze.Toggled += v => _plot.Frozen = v; bar.AddChild(freeze);
        _record = new Button { Text = "● Record CSV" }; _record.Pressed += ToggleRecord; bar.AddChild(_record);
        var clear = new Button { Text = "Clear" }; clear.Pressed += () => Store.Clear(); bar.AddChild(clear);
        var uncheck = new Button { Text = "Uncheck all" }; uncheck.Pressed += () => { foreach (var id in _checked.ToList()) SetChecked(id, false); }; bar.AddChild(uncheck);
        bar.AddChild(new Control { SizeFlagsHorizontal = SizeFlags.ExpandFill });
        _status = new Label { Text = "no signals yet" };
        bar.AddChild(_status);
        AddChild(bar);

        var split = new HSplitContainer { SizeFlagsVertical = SizeFlags.ExpandFill };
        _tree = new Tree { Columns = 4, HideRoot = true, CustomMinimumSize = new Vector2(300, 0), SelectMode = Tree.SelectModeEnum.Row };
        _tree.SetColumnTitlesVisible(true);
        _tree.SetColumnTitle(0, ""); _tree.SetColumnTitle(1, "signal"); _tree.SetColumnTitle(2, "value"); _tree.SetColumnTitle(3, "unit");
        _tree.SetColumnExpand(0, false); _tree.SetColumnCustomMinimumWidth(0, 28);
        _tree.SetColumnExpand(1, true); _tree.SetColumnExpandRatio(1, 3);
        _tree.SetColumnExpand(2, true); _tree.SetColumnExpandRatio(2, 1);
        _tree.SetColumnExpand(3, false); _tree.SetColumnCustomMinimumWidth(3, 40);
        _tree.ItemEdited += OnItemEdited;
        _root = _tree.CreateItem();
        split.AddChild(_tree);
        _plot = new PlotView { SizeFlagsHorizontal = SizeFlags.ExpandFill, SizeFlagsVertical = SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(200, 120) };
        split.AddChild(_plot);
        AddChild(split);
        Store.ListChanged += () => _listDirty = true;
        Store.Restarted += (from, to) => OnRestart($"virtual time went back {from:0.00} -> {to:0.00} s");
    }

    /// A new time line from the core (hello / new connection): data cleared, plot back to t = 0.
    public void Restart(string why)
    {
        if (!Store.HasT) return;                   // nothing recorded yet
        Store.Reset();
        OnRestart(why);
    }

    private void OnRestart(string why)
    {
        _plot?.Restart(Store.LastT);
        if (_csv != null) { _csv.StoreLine($"# restart: {why}"); }
        Log?.Invoke("app", $"signals cleared: {why}");
    }

    // ---- feeding ----
    /// Anyone looking at the values: the window open, a checked (plotted) signal, a CSV being recorded.
    private bool Needed => _checked.Count > 0 || _csv != null || IsVisibleInTree();

    public void OnState(JsonElement s)
    {
        Store.State(s, Needed); _plot.Now = Store.LastT; SampleCount++; WriteCsvRow(Store.LastT);
        if (!_firstStateSeen) { _firstStateSeen = true; RequestSubscribe(); }   // sources are known now; drop state ids from the core subscription
    }
    private bool _firstStateSeen;
    private List<string> _lastSubscribe = new();
    private void RequestSubscribe()
    {
        if (!_firstStateSeen) return;   // until the first state arrives we cannot tell state ids from core ids
        var coreIds = _checked.Where(x => Store.Traces.TryGetValue(x, out var tr) && tr.Source == "core").OrderBy(x => x).ToList();
        if (coreIds.SequenceEqual(_lastSubscribe)) return;
        _lastSubscribe = coreIds;
        SubscribeRequested?.Invoke(coreIds);
    }
    public void OnSamples(JsonElement m) { Store.Samples(m); _plot.Now = Store.LastT; SampleCount++; }
    public void OnSignals(JsonElement m) { Store.Register(m); Log?.Invoke("app", $"core published {Store.Traces.Values.Count(x => x.Source == "core")} signals"); }

    /// Check ids programmatically (env FB_SIGNALS for headless runs, or the bench's saved set).
    public void Check(IEnumerable<string> ids) { foreach (var id in ids) SetChecked(id, true); }

    /// The checked ids, sorted (saved into the bench with the layout).
    public List<string> CheckedIds() => _checked.OrderBy(x => x, StringComparer.Ordinal).ToList();

    // ---- list ----
    private void RebuildList()
    {
        var filter = _filter.Text.Trim();
        _tree.Clear(); _items.Clear();
        _root = _tree.CreateItem();
        foreach (var tr in Store.Traces.Values.OrderBy(x => x.Source == "state" ? 0 : 1).ThenBy(x => x.Id, StringComparer.Ordinal))
        {
            if (filter != "" && !tr.Id.Contains(filter, StringComparison.OrdinalIgnoreCase)) continue;
            var it = _tree.CreateItem(_root);
            it.SetCellMode(0, TreeItem.TreeCellMode.Check); it.SetEditable(0, true); it.SetChecked(0, _checked.Contains(tr.Id));
            it.SetText(1, tr.Id); it.SetMetadata(1, tr.Id);
            it.SetText(3, tr.Unit);
            it.SetTooltipText(1, tr.Source == "core" ? "published by the core (streamed after subscribe)" : "from the state stream");
            if (_colors.TryGetValue(tr.Id, out var c)) it.SetCustomColor(1, c);
            else if (tr.Source == "core") it.SetCustomColor(1, new Color(0.8f, 0.85f, 1f));
            _items[tr.Id] = it;
        }
        _listDirty = false;
    }

    private void OnItemEdited()
    {
        var it = _tree.GetEdited();
        if (it == null) return;
        var id = it.GetMetadata(1).AsString();
        SetChecked(id, it.IsChecked(0));
    }

    private void SetChecked(string id, bool on)
    {
        if (on == _checked.Contains(id)) return;
        if (on)
        {
            _checked.Add(id);
            var tr = Store.Traces.TryGetValue(id, out var t) ? t : Store.Get(id, "core");
            tr.Keep(true, SignalStore.Capacity);                      // history only for plotted signals
            if (!_colors.ContainsKey(id)) _colors[id] = Palette[_paletteNext++ % Palette.Length];
            _plot.Traces.Add((tr, _colors[id]));
        }
        else
        {
            _checked.Remove(id);
            _plot.Traces.RemoveAll(x => x.trace.Id == id);
            if (Store.Traces.TryGetValue(id, out var t)) t.Keep(false, 0);
        }
        if (_items.TryGetValue(id, out var item)) { item.SetChecked(0, on); if (on) item.SetCustomColor(1, _colors[id]); else item.ClearCustomColor(1); }
        RequestSubscribe();
    }

    public override void _Process(double delta)
    {
        if (!IsVisibleInTree()) return;
        _frame++;
        if (_listDirty) RebuildList();
        if (_frame % 6 == 0)
        {
            foreach (var (id, it) in _items)
                if (Store.Traces.TryGetValue(id, out var tr)) it.SetText(2, PlotView.Fmt(tr.Last));
            var nState = Store.Traces.Values.Count(x => x.Source == "state");
            var nCore = Store.Traces.Count - nState;
            _status.Text = $"{nState} from state · {nCore} from core · {_checked.Count} plotted{(_csv != null ? $" · REC {CsvRows} rows" : "")}";
        }
    }

    // ---- CSV ----
    private void ToggleRecord()
    {
        if (_csv != null) { StopRecord(); return; }
        if (_checked.Count == 0) { Log?.Invoke("app", "CSV: check at least one signal first"); return; }
        StartRecord($"user://signals_{DateTime.Now:yyyyMMdd_HHmmss}.csv");
    }

    public void StartRecord(string path)
    {
        _csvIds = _checked.OrderBy(x => x).ToList();
        _csv = Godot.FileAccess.Open(path, Godot.FileAccess.ModeFlags.Write);
        if (_csv == null) { Log?.Invoke("app", "CSV: cannot open " + path); return; }
        _csvPath = path; CsvRows = 0;
        _csv.StoreLine("t," + string.Join(",", _csvIds));
        _record.Text = "■ Stop CSV";
        Log?.Invoke("app", $"CSV recording {_csvIds.Count} signals → {ProjectSettings.GlobalizePath(path)}");
    }

    public void StopRecord()
    {
        if (_csv == null) return;
        _csv.Close(); _csv = null;
        _record.Text = "● Record CSV";
        Log?.Invoke("app", $"CSV saved: {CsvRows} rows → {ProjectSettings.GlobalizePath(_csvPath)}");
    }

    private void WriteCsvRow(double t)
    {
        if (_csv == null) return;
        var sb = new StringBuilder(t.ToString("0.###", CultureInfo.InvariantCulture));
        foreach (var id in _csvIds)
        {
            sb.Append(',');
            if (Store.Traces.TryGetValue(id, out var tr) && !double.IsNaN(tr.Last)) sb.Append(tr.Last.ToString("0.######", CultureInfo.InvariantCulture));
        }
        _csv.StoreLine(sb.ToString());
        CsvRows++;
    }

    public override void _ExitTree() => StopRecord();
}
