// The flight screen's embedded windows: creation from the bench's layout (a snapshot taken when the
// screen opened), FB_WINDOWS, fitting into the visible area, and the layout written back into the bench.
// Fitting never changes what is saved: every window keeps the rect the user wanted (from the layout or
// the last move/resize by hand) apart from the rect it was fitted to; a window the user has not touched
// since the last fit is saved with its wanted rect. A saved layout from before the Aircraft window (2026-10-01: the
// plane block, crash banner and model numbers moved out of Status) gets it opened where that block was
// (MigrateAircraft / AircraftPlacement), and its Status window compacted.
using System;
using System.Collections.Generic;
using System.Linq;
using Godot;

namespace FlightBench;

public sealed class FlightWindows
{
    private sealed class Entry
    {
        public Window W = null!;
        public Rect2I Wanted, Fitted;
    }

    private readonly Node _parent;
    private readonly Dictionary<string, WindowLayout> _loaded;
    private readonly Dictionary<string, Entry> _e = new();
    private readonly Action<InputEvent, Viewport> _keys;

    /// `keys`: a focused window's unhandled input (keys its GUI did not take) goes here.
    public FlightWindows(Node parent, BenchData bench, Action<InputEvent, Viewport> keys)
    {
        _parent = parent; _keys = keys;
        _loaded = bench.Layout.ToDictionary(kv => kv.Key, kv => new WindowLayout { Open = kv.Value.Open, X = kv.Value.X, Y = kv.Value.Y, W = kv.Value.W, H = kv.Value.H });
    }

    public IEnumerable<string> Names => _e.Keys;

    /// The Status window's height in a migrated layout (four lines).
    public const int CompactStatusH = 180;

    /// Where the Aircraft window goes in a saved layout that has Status but no Aircraft: below the compacted Status, else
    /// right of Status / right of the Transmitter / below the Log — the first spot that overlaps no open saved window
    /// (title bars counted) and fits 1600×900; `status` is the compacted Status. Null: nothing to migrate.
    public static WindowLayout? AircraftPlacement(Dictionary<string, WindowLayout> saved, out WindowLayout? status)
    {
        status = null;
        if (saved.Count == 0 || saved.ContainsKey("Aircraft") || !saved.TryGetValue("Status", out var st)) return null;
        status = new WindowLayout { Open = st.Open, X = st.X, Y = st.Y, W = st.W, H = Math.Min(st.H, CompactStatusH) };
        int w = Math.Max(340, st.W), h = 280;
        const int title = 30, gap = 10;
        var cands = new List<(int X, int Y)> { (status.X, status.Y + status.H + title + gap), (status.X + status.W + 20, status.Y) };
        if (saved.TryGetValue("Transmitter", out var tx) && tx.Open) cands.Add((tx.X + tx.W + 20, tx.Y));
        if (saved.TryGetValue("Log / CLI", out var lg) && lg.Open) cands.Add((lg.X, lg.Y + lg.H + title + gap));
        var others = saved.Where(kv => kv.Key != "Status" && kv.Value.Open).Select(kv => new Rect2I(kv.Value.X, kv.Value.Y - title, kv.Value.W, kv.Value.H + title)).ToList();
        others.Add(new Rect2I(status.X, status.Y - title, status.W, status.H + title));
        foreach (var (x, y) in cands)
        {
            var r = new Rect2I(x, y - title, w, h + title);
            if (x + w <= 1600 && y + h <= 860 && !others.Any(o => o.Intersects(r))) return new WindowLayout { Open = true, X = x, Y = y, W = w, H = h };
        }
        return new WindowLayout { Open = true, X = cands[0].X, Y = cands[0].Y, W = w, H = h };
    }

    /// A saved layout without the Aircraft window: Status compacted, Aircraft placed (AircraftPlacement); the log line, or null.
    /// Call it before the first Make.
    public string? MigrateAircraft()
    {
        // the windows take their rects when made: a migration after them would change only what is saved
        if (_e.Count > 0) throw new InvalidOperationException("MigrateAircraft before any Make");
        if (AircraftPlacement(_loaded, out var st) is not { } a || st == null) return null;
        var old = _loaded["Status"];
        _loaded["Status"] = st;
        _loaded["Aircraft"] = a;
        return $"layout: this bench's saved layout has no Aircraft window (the model's numbers and the crash banner moved out of Status): Aircraft opened at {a.X},{a.Y} {a.W}×{a.H}, Status {old.W}×{old.H} → {st.W}×{st.H} (saved with the bench)";
    }
    public Window this[string name] => _e[name].W;
    public bool TryGet(string name, out Window w) { var ok = _e.TryGetValue(name, out var e); w = e?.W!; return ok; }
    public bool IsOpen(string name) => _e.TryGetValue(name, out var e) && e.W.Visible;

    /// A window with `content`; position/size/open from the loaded layout, else the defaults. A saved size
    /// below `minSize` (older benches) is raised to the default size.
    public Window Make(string name, Control content, Vector2I pos, Vector2I size, bool openByDefault = false, Vector2I? minSize = null)
    {
        var w = new Window { Title = name, Visible = false, Unresizable = false, Transient = false, Exclusive = false, Theme = Ui.BaseTheme };
        var margin = new MarginContainer(); margin.SetAnchorsPreset(Control.LayoutPreset.FullRect);
        foreach (var side in new[] { "margin_left", "margin_right", "margin_top", "margin_bottom" }) margin.AddThemeConstantOverride(side, 6);
        margin.AddChild(content);
        w.AddChild(margin);
        w.CloseRequested += () => w.Visible = false;
        w.AddChild(new InputRelay { Handler = _keys });
        bool open = openByDefault;
        var def = size;
        if (_loaded.TryGetValue(name, out var l)) { pos = new Vector2I(l.X, l.Y); size = new Vector2I(Math.Max(120, l.W), Math.Max(80, l.H)); open = l.Open; }
        if (minSize is { } min)
        {
            w.MinSize = min;
            if (size.X < min.X || size.Y < min.Y) size = new Vector2I(Math.Max(size.X, def.X), Math.Max(size.Y, def.Y));
        }
        w.Position = pos; w.Size = size; w.Visible = open;
        _parent.AddChild(w);
        var r = Rect(w);
        _e[name] = new Entry { W = w, Wanted = r, Fitted = r };
        return w;
    }

    public void Toggle(string name) { if (_e.TryGetValue(name, out var e)) e.W.Visible = !e.W.Visible; }

    /// FB_WINDOWS: "all" or "Name,Name" opened regardless of the layout (this run only; saving is off then).
    public void OpenFromEnv(string? spec)
    {
        if (string.IsNullOrEmpty(spec)) return;
        var names = spec.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
        foreach (var (name, e) in _e) if (spec == "all" || names.Contains(name)) e.W.Visible = true;
    }

    private static Rect2I Rect(Window w) => new(w.Position, w.Size);

    /// Keeps every window inside the visible area (below the menu bar, above the time bar). Layouts are
    /// in UI units: a larger UI scale or a smaller window shrinks the area.
    public void Fit(Vector2 area)
    {
        if (area.X < 400 || area.Y < 300) return;    // headless or not laid out yet: leave the layout alone
        foreach (var e in _e.Values)
        {
            var w = e.W;
            if (Rect(w) != e.Fitted) e.Wanted = Rect(w);                 // moved / resized by hand since the last fit
            var size = FitSize(e.Wanted.Size, area);
            if (size != w.Size) w.Size = size;
            var pos = FitPos(e.Wanted.Position, w.Size, area);           // MinSize may have raised the size
            if (pos != w.Position) w.Position = pos;
            e.Fitted = Rect(w);
        }
    }

    private const int Top = 40, Bottom = 40, TitleH = 30;
    public static Vector2I FitSize(Vector2I wanted, Vector2 area) =>
        new(Math.Min(wanted.X, Math.Max(120, (int)area.X - 8)), Math.Min(wanted.Y, Math.Max(80, (int)area.Y - Top - Bottom - TitleH)));
    public static Vector2I FitPos(Vector2I wanted, Vector2I size, Vector2 area) =>
        new(Math.Clamp(wanted.X, 4, Math.Max(4, (int)area.X - size.X - 4)), Math.Clamp(wanted.Y, Top + TitleH, Math.Max(Top + TitleH, (int)area.Y - Bottom - size.Y)));
    /// Where a window wanted at `r` ends up in `area` (the Fit rule; the size not raised by a MinSize), title bar included.
    public static Rect2I Fitted(Rect2I r, Vector2 area)
    {
        var size = FitSize(r.Size, area);
        var pos = FitPos(r.Position, size, area);
        return new Rect2I(pos.X, pos.Y - TitleH, size.X, size.Y + TitleH);
    }

    /// Writes the layout into the bench: open state, and per window its wanted rect (or where the user put it since).
    public void SaveInto(BenchData bench)
    {
        foreach (var (name, e) in _e)
        {
            var cur = Rect(e.W);
            var r = cur == e.Fitted ? e.Wanted : cur;
            bench.Layout[name] = new WindowLayout { Open = e.W.Visible, X = r.Position.X, Y = r.Position.Y, W = r.Size.X, H = r.Size.Y };
        }
    }
}

/// Child of an embedded window: hands the window's unhandled input (keys its GUI did not take) to the flight screen.
public partial class InputRelay : Node
{
    public Action<InputEvent, Viewport>? Handler;
    public override void _UnhandledInput(InputEvent e) => Handler?.Invoke(e, GetViewport());
}
