// The firmware's config script as layers (ConfigLayers.cs), editable at the source — the wizard's summary step and the
// left half of the flight screen's Config window:
// - a tree, one group per layer (1 board · 2 aircraft · 3 profile · 4 device · 5 bench, and "not traced" when the app cannot say),
//   one row per line: the text, its source (file:line, "device ublox on uart3"), a check box on device lines (on =
//   typed; off = `disabled_lines`); a line whose setting a later layer also writes is amber ("later layer wins") and
//   says which line wins;
// - the selected line's actions: **Edit <file> at line N** (board / aircraft lines: the file in an inline editor;
//   Save writes it in the data root, or — a preset's bundled aircraft / board file — copies it next to the bench
//   first and points the bench at the copy, as the editor's header says before saving), **Disable / Enable** (any
//   line: `disabled_lines`, by its exact text), **Override in bench lines** (copies the line into layer 5 to edit);
// - **Bench lines** (layer 5): an editable box = the bench's `bench_cli` file next to the bench (Save bench lines
//   writes it and checks again).
// The owner (wizard / Config window) runs the session-check (CheckRequested) and keeps the bench (BenchChanged).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Godot;

namespace FlightBench;

public partial class LayersView : VBoxContainer
{
    /// The bench it edits (the wizard's copy / the flight screen's bench).
    public Func<BenchData>? BenchOf;
    /// Run the session-check again (a file, the bench lines or a disabled line changed).
    public event Action? CheckRequested;
    /// A bench field changed (disabledLines, benchCli, aircraftFile, boardConfigFile): the owner keeps / saves it.
    public event Action? BenchChanged;
    public event Action<string>? Logged;

    public LayeredConfig? Config { get; private set; }
    public string Root = "";

    private readonly Label _info = new() { AutowrapMode = TextServer.AutowrapMode.WordSmart, Modulate = Ui.Soft };
    private readonly Tree _tree = new() { Columns = 3, HideRoot = true, SelectMode = Tree.SelectModeEnum.Row, CustomMinimumSize = new Vector2(0, 340), SizeFlagsVertical = SizeFlags.ExpandFill };
    private readonly Label _sel = new() { AutowrapMode = TextServer.AutowrapMode.WordSmart };
    private readonly HFlowContainer _actions = new();
    private readonly Button _edit = new() { Text = "Edit file", FocusMode = FocusModeEnum.None };
    private readonly Button _toggle = new() { Text = "Disable line", FocusMode = FocusModeEnum.None };
    private readonly Button _override = new() { Text = "Override in bench lines", FocusMode = FocusModeEnum.None };
    // the inline file editor
    private readonly PanelContainer _editor = new() { Visible = false };
    private readonly Label _editorHead = new() { AutowrapMode = TextServer.AutowrapMode.WordSmart };
    private readonly TextEdit _editorText = new() { CustomMinimumSize = new Vector2(0, 220), WrapMode = TextEdit.LineWrappingMode.None };
    private readonly Button _editorSave = new() { Text = "Save file", FocusMode = FocusModeEnum.None };
    private string? _editorFile;
    // layer 5
    private readonly Label _benchHead = new() { AutowrapMode = TextServer.AutowrapMode.WordSmart };
    private readonly TextEdit _bench = new() { Name = "bench_lines", CustomMinimumSize = new Vector2(0, 110), WrapMode = TextEdit.LineWrappingMode.None, PlaceholderText = "set some_setting = value   (one CLI line each; typed after the board's, the aircraft's and the devices' lines: a later layer wins)" };
    private readonly Button _benchSave = new() { Text = "Save bench lines", FocusMode = FocusModeEnum.None };
    private string _benchLoaded = "";
    private readonly Dictionary<TreeItem, ConfigLine> _rows = new();
    private readonly Dictionary<TreeItem, ConfigLine> _conflictRows = new();
    private readonly Dictionary<TreeItem, ConfigLine> _groupRows = new();

    /// A source as shown: a file next to the benches `./<file>`, one under the data root relative to it.
    private string Short(string source)
    {
        var s = source.Replace('\\', '/');
        var bd = BenchFiles.Dir + "/";
        if (s.StartsWith(bd, StringComparison.OrdinalIgnoreCase)) return "./" + s[bd.Length..];
        var root = Root.Replace('\\', '/').TrimEnd('/') + "/";
        if (Root != "" && s.StartsWith(root, StringComparison.OrdinalIgnoreCase)) return s[root.Length..];
        return s;
    }
    private ConfigLine? _selected;

    public LayersView()
    {
        AddThemeConstantOverride("separation", 4);
        AddChild(_info);
        _tree.SetColumnTitle(0, "text"); _tree.SetColumnTitle(1, "source"); _tree.SetColumnTitle(2, "on");
        _tree.ColumnTitlesVisible = true;
        _tree.SetColumnExpand(0, true); _tree.SetColumnExpandRatio(0, 3);
        _tree.SetColumnExpand(1, true); _tree.SetColumnExpandRatio(1, 2);
        _tree.SetColumnExpand(2, false); _tree.SetColumnCustomMinimumWidth(2, 44);
        _tree.AddThemeFontSizeOverride("font_size", 14);
        _tree.ItemSelected += () => Select(_tree.GetSelected() is { } it && (_rows.TryGetValue(it, out var l) || _conflictRows.TryGetValue(it, out l) || _groupRows.TryGetValue(it, out l)) ? l : null);
        _tree.ItemEdited += OnItemEdited;
        _tree.ItemActivated += () => { if (_selected != null && CanEdit(_selected)) OpenEditor(_selected); };
        AddChild(_tree);
        _sel.Modulate = Ui.Soft;
        AddChild(_sel);
        _actions.AddThemeConstantOverride("h_separation", 6);
        _edit.Pressed += () => { if (_selected != null) OpenEditor(_selected); };
        _toggle.Pressed += () => { if (_selected != null) SetDisabled(_selected, !_selected.Disabled); };
        _override.Pressed += () => { if (_selected != null) OverrideInBench(_selected); };
        _actions.AddChild(_edit); _actions.AddChild(_toggle); _actions.AddChild(_override);
        AddChild(_actions);
        // editor
        _editor.AddThemeStyleboxOverride("panel", Ui.Box(new Color(0.03f, 0.08f, 0.16f), new Color(0.45f, 0.65f, 1f, 0.7f), 4, 8, 6));
        var ev = new VBoxContainer(); ev.AddThemeConstantOverride("separation", 4);
        ev.AddChild(_editorHead);
        _editorText.AddThemeFontSizeOverride("font_size", 14);
        ev.AddChild(_editorText);
        var er = new HBoxContainer(); er.AddThemeConstantOverride("separation", 6);
        _editorSave.Pressed += () => SaveEditor();
        var close = new Button { Text = "Close (discard)", FocusMode = FocusModeEnum.None };
        close.Pressed += () => { _editor.Visible = false; _editorFile = null; };
        er.AddChild(_editorSave); er.AddChild(close);
        ev.AddChild(er);
        _editor.AddChild(ev);
        AddChild(_editor);
        // layer 5
        AddChild(_benchHead);
        _bench.AddThemeFontSizeOverride("font_size", 14);
        _bench.TextChanged += UpdateBenchHead;
        AddChild(_bench);
        var br = new HBoxContainer(); br.AddThemeConstantOverride("separation", 6);
        _benchSave.Pressed += () => SaveBenchLines();
        var revert = new Button { Text = "Revert", FocusMode = FocusModeEnum.None, TooltipText = "the bench lines file as it is on disk" };
        revert.Pressed += () => { _bench.Text = _benchLoaded; UpdateBenchHead(); };
        br.AddChild(_benchSave); br.AddChild(revert);
        AddChild(br);
        Show(null, "");
    }

    /// Shows a layered config (null: `error` says why: no core, a rejected bench…), and the bench's lines.
    public void Show(LayeredConfig? c, string error)
    {
        Config = c;
        _info.Text = c != null ? c.Summary : error != "" ? "not known: " + error : "(the core's session-check says what goes in)";
        _info.Modulate = c != null || error == "" ? Ui.Soft : new Color(1f, 0.6f, 0.45f);
        RebuildTree();
        if (_bench.Text == _benchLoaded) ReloadBenchLines();      // unsaved bench lines stay in the box
        else UpdateBenchHead();
    }

    /// The owner started a session-check: the info line says so (the tree stays until the answer).
    public void SetBusy() { _info.Text = "asking the core (session-check)…"; _info.Modulate = Ui.Soft; }

    /// The bench lines box from the file (after a snapshot wrote it, or another bench).
    public void ReloadBenchLines()
    {
        if (BenchOf?.Invoke() is not { } b) { _benchHead.Text = $"{LayeredConfig.LayerTitle("bench")} lines"; return; }
        _benchLoaded = BenchFiles.ReadBenchLines(b);
        _bench.Text = _benchLoaded;
        UpdateBenchHead();
    }

    private void UpdateBenchHead()
    {
        var b = BenchOf?.Invoke();
        var path = b != null ? BenchFiles.CliPath(b) : "";
        int n = _bench.Text.Split('\n').Count(l => LayeredConfig.Clean(l) != null);
        bool dirty = _bench.Text != _benchLoaded;
        _benchHead.Text = $"{LayeredConfig.LayerTitle("bench")} lines (bench_cli, a later layer wins) — {n} lines · {Short(path)}{(File.Exists(path) ? "" : " (no file yet: Save bench lines creates it)")}{(dirty ? " · not saved" : "")}";
        _benchHead.Modulate = LayeredConfig.LayerColor("bench");
        _benchSave.Disabled = !dirty || b == null;
    }

    private void RebuildTree()
    {
        _tree.Clear();
        _rows.Clear();
        _conflictRows.Clear();
        _groupRows.Clear();
        var root = _tree.CreateItem();
        if (Config == null) { Select(null); return; }
        var b = BenchOf?.Invoke();
        // the conflicts first: one row per setting, its lines under it (the last one wins)
        if (Config.Conflicts.Count > 0)
        {
            var cg = _tree.CreateItem(root);
            cg.SetText(0, $"⚠ conflicts — {Config.Conflicts.Count} setting(s) in two layers, the later layer wins{(Config.ConflictsComputed ? " (found by the app)" : "")}");
            cg.SetCustomColor(0, new Color(1f, 0.75f, 0.35f));
            cg.SetSelectable(0, false); cg.SetSelectable(1, false); cg.SetSelectable(2, false);
            foreach (var c in Config.Conflicts)
            {
                var ci = _tree.CreateItem(cg);
                ci.SetText(0, c.Setting);
                ci.SetText(1, $"{string.Join(" → ", c.Lines.Select(l => LayeredConfig.LayerTitle(l.Layer)))} wins: {LayeredConfig.LayerTitle(c.Winner)}");
                ci.SetCustomColor(0, new Color(1f, 0.8f, 0.45f));
                ci.SetSelectable(0, false); ci.SetSelectable(1, false); ci.SetSelectable(2, false);
                for (int i = 0; i < c.Lines.Count; i++)
                {
                    var l = c.Lines[i];
                    var it = _tree.CreateItem(ci);
                    it.SetText(0, (i == c.Lines.Count - 1 ? "★ " : "⚠ ") + l.Text);
                    it.SetText(1, $"{LayeredConfig.LayerTitle(l.Layer)} · {Short(l.Source)}");
                    it.SetCustomColor(1, LayeredConfig.LayerColor(l.Layer));
                    it.SetTooltipText(0, i == c.Lines.Count - 1 ? "wins: the last line of this setting" : "overridden by a later layer");
                    _conflictRows[it] = l;
                }
            }
        }
        foreach (var layer in LayeredConfig.Layers.Append(""))
        {
            var ls = Config.Lines.Where(l => l.Layer == layer).ToList();
            if (ls.Count == 0 && layer == "") continue;
            var g = _tree.CreateItem(root);
            var files = ls.Select(l => l.File ?? (l.Layer == "device" ? null : l.Source)).Where(x => !string.IsNullOrEmpty(x)).Select(x => Short(x!)).Distinct().ToList();
            string LayerFile(string ly) => string.Join(", ", Config.LayerFiles.Where(x => x.Layer == ly).Select(x => Short(x.File)).Distinct());
            var from = layer switch
            {
                "board" when LayerFile("board") is { Length: > 0 } bl => bl,
                "board" => b != null && BenchFiles.BoardFile(b, Root) is { } bf ? bf.Shown : string.Join(", ", files),
                "aircraft" when LayerFile("aircraft") is { Length: > 0 } al => al,
                "aircraft" => b != null && BenchFiles.AircraftCli(b, Root) is { } ac ? ac.Shown : string.Join(", ", files),
                "profile" when LayerFile("profile") is { Length: > 0 } pl => pl,
                "profile" => string.Join(", ", files),
                "device" => string.Join(", ", ls.Select(l => l.Source).Distinct().Where(s => s != "")),
                "bench" when LayerFile("bench") is { Length: > 0 } bl2 => bl2,
                "bench" => string.Join(", ", files.DefaultIfEmpty(b != null ? b.BenchCli ?? BenchFiles.CliRel(b.Name) : "")),
                _ => "the app could not match these to a file",
            };
            g.SetText(0, $"{LayeredConfig.LayerTitle(layer)} — {ls.Count(l => !l.Disabled)} lines{(ls.Any(l => l.Disabled) ? $", {ls.Count(l => l.Disabled)} disabled" : "")}");
            g.SetText(1, from);
            g.SetCustomColor(0, LayeredConfig.LayerColor(layer));
            g.SetCustomColor(1, LayeredConfig.LayerColor(layer));
            // a board / aircraft / bench group opens its file (also with no line in it: defaults.cli may be comments only)
            var groupFile = layer switch { "board" => b != null ? BenchFiles.BoardFile(b, Root)?.Shown : null, "aircraft" => b != null ? BenchFiles.AircraftCli(b, Root)?.Shown : null, _ => null };
            if (groupFile != null) _groupRows[g] = new ConfigLine { Layer = layer, Source = groupFile, Text = "" };
            else { g.SetSelectable(0, false); g.SetSelectable(1, false); }
            g.SetSelectable(2, false);
            g.SetText(1, Short(g.GetText(1)));
            g.Collapsed = ls.Count > 40 && layer != "device" && layer != "bench";      // the conflicts group above shows their lines
            foreach (var l in ls)
            {
                var it = _tree.CreateItem(g);
                it.SetText(0, (l.Disabled ? "✕ " : "") + l.Text);
                it.SetText(1, Short(l.Source) + (l.Inferred ? " (inferred)" : ""));
                if (l.Disabled) { it.SetCustomColor(0, new Color(0.6f, 0.6f, 0.6f)); it.SetCustomColor(1, new Color(0.6f, 0.6f, 0.6f)); }
                else it.SetCustomColor(1, LayeredConfig.LayerColor(l.Layer).Lerp(Colors.White, 0.35f));
                it.SetCellMode(2, TreeItem.TreeCellMode.Check);
                it.SetChecked(2, !l.Disabled);
                it.SetEditable(2, true);
                it.SetTooltipText(2, l.Disabled ? "off: skipped (disabled_lines)" : "on: typed into the firmware; untick to skip it (disabled_lines)");
                if (l.Conflict != null && !l.Disabled)
                {
                    var bg = l.Overridden ? new Color(0.45f, 0.3f, 0.05f) : new Color(0.25f, 0.35f, 0.1f);
                    it.SetCustomBgColor(0, bg); it.SetCustomBgColor(1, bg);
                    var c = Config.Conflicts.FirstOrDefault(x => x.Setting == l.Conflict);
                    var tip = c != null ? $"conflict: {c.Describe()}" : $"conflict on {l.Conflict}";
                    it.SetTooltipText(0, (l.Overridden ? "overridden — " : "wins — ") + tip);
                    it.SetText(0, (l.Overridden ? "⚠ " : "★ ") + it.GetText(0));
                }
                _rows[it] = l;
            }
        }
        Select(null);
    }

    private bool CanEdit(ConfigLine l) => l.File != null && l.Layer is "board" or "aircraft" or "bench" or "";

    private void Select(ConfigLine? l)
    {
        _selected = l;
        _actions.Visible = l != null;
        if (l == null) { _sel.Text = Config == null ? "" : "select a line: edit its file, disable it, or override it in the bench lines; double-click edits"; return; }
        var c = l.Conflict != null ? Config?.Conflicts.FirstOrDefault(x => x.Setting == l.Conflict) : null;
        _sel.Text = $"{LayeredConfig.LayerTitle(l.Layer)} · {Short(l.Source)}{(l.Inferred ? " (inferred by the app)" : "")}{(l.Text != "" ? $" · `{l.Text}`" : " (the layer's file)")}"
                    + (c != null ? $"\nconflict — {c.Describe()}{(l.Overridden ? ": this line does NOT win" : ": this line wins")}" : "");
        _edit.Visible = CanEdit(l);
        _edit.Text = l.File != null ? $"Edit {Path.GetFileName(l.File)}{(l.FileLine > 0 ? $" at line {l.FileLine}" : "")}" : "Edit file";
        _toggle.Text = l.Disabled ? "Enable line" : "Disable line";
        _toggle.TooltipText = "disabled_lines (saved in the bench): the core skips this exact line";
        _override.Visible = l.Layer != "bench" && l.Text != "";
        _toggle.Visible = l.Text != "";
        _override.TooltipText = "copy the line into the bench lines (layer 5, typed last: it wins) to change its value there";
    }

    private void OnItemEdited()
    {
        var it = _tree.GetEdited();
        if (it == null || !_rows.TryGetValue(it, out var l)) return;
        SetDisabled(l, !it.IsChecked(2));
    }

    /// Disable / enable a line: the bench's `disabledLines` (by the line's exact text), then check again.
    public void SetDisabled(ConfigLine l, bool disabled)
    {
        if (BenchOf?.Invoke() is not { } b) return;
        var list = b.DisabledLines ?? new List<string>();
        var key = l.Text.Trim();
        list.RemoveAll(x => x == key || x == l.Ref);
        if (disabled) list.Add(key);
        b.DisabledLines = list.Count > 0 ? list : null;
        l.Disabled = disabled;
        Logged?.Invoke($"config: {(disabled ? "disabled" : "enabled")} '{key}' ({LayeredConfig.LayerTitle(l.Layer)}, {l.Source}) — disabledLines: {b.DisabledLines?.Count ?? 0}");
        BenchChanged?.Invoke();
        CheckRequested?.Invoke();
    }

    /// Adds the line to the bench lines box (not saved yet: edit it, then Save bench lines).
    public void OverrideInBench(ConfigLine l)
    {
        var t = _bench.Text;
        if (t != "" && !t.EndsWith('\n')) t += "\n";
        _bench.Text = t + $"# overrides {l.Source}\n{l.Text}\n";
        _bench.SetCaretLine(_bench.GetLineCount() - 2);
        _bench.GrabFocus();
        UpdateBenchHead();
        Logged?.Invoke($"config: '{l.Text}' copied into the bench lines — change it there, then Save bench lines");
    }

    /// Adds lines to the bench lines box (the Config window's Changes: a configurator change → a bench line).
    public void AppendBenchLines(IEnumerable<string> lines, string comment)
    {
        var t = _bench.Text;
        if (t != "" && !t.EndsWith('\n')) t += "\n";
        _bench.Text = t + $"# {comment}\n" + string.Join("\n", lines) + "\n";
        UpdateBenchHead();
    }

    /// Save bench lines: the `bench_cli` file next to the bench, the bench pointed at it, then check again.
    public bool SaveBenchLines()
    {
        if (BenchOf?.Invoke() is not { } b) return false;
        bool had = b.BenchCli != null;
        if (!BenchFiles.WriteBenchLines(b, _bench.Text, out var err)) { Logged?.Invoke($"config: bench lines not written: {err}"); return false; }
        _benchLoaded = BenchFiles.ReadBenchLines(b);
        _bench.Text = _benchLoaded;
        UpdateBenchHead();
        Logged?.Invoke($"config: bench lines written to {BenchFiles.CliPath(b)} ({_benchLoaded.Split('\n').Count(x => LayeredConfig.Clean(x) != null)} lines)");
        if (!had) BenchChanged?.Invoke();
        else BenchChanged?.Invoke();
        CheckRequested?.Invoke();
        return true;
    }

    /// Opens the line's file in the editor, the caret on its line; the header says what Save will do.
    public void OpenEditor(ConfigLine l)
    {
        if (l.File == null || BenchOf?.Invoke() is not { } b) return;
        if (l.Layer == "bench" && b.BenchCli != null && BenchFiles.Same(BenchFiles.ResolveSource(l.File, Root), BenchFiles.CliPath(b)))
        {
            _bench.GrabFocus();
            if (l.FileLine > 0) _bench.SetCaretLine(Math.Min(l.FileLine - 1, _bench.GetLineCount() - 1));
            return;
        }
        var abs = BenchFiles.ResolveSource(l.File, Root);
        string text;
        try { text = File.ReadAllText(abs).Replace("\r\n", "\n"); }
        catch (Exception e) { Logged?.Invoke($"config: cannot read {abs}: {e.Message}"); return; }
        _editorFile = abs;
        var (copy, why) = BenchFiles.EditPolicy(b, abs, Root);
        _editorHead.Text = $"{LayeredConfig.LayerTitle(l.Layer)} · {abs}\nSave: {why}";
        _editorHead.Modulate = copy ? new Color(1f, 0.8f, 0.45f) : Colors.White;
        _editorSave.Text = copy ? "Save as this bench's copy" : "Save file";
        _editorText.Text = text;
        _editor.Visible = true;
        if (l.FileLine > 0) { _editorText.SetCaretLine(l.FileLine - 1); _editorText.CenterViewportToCaret(); }
        _editorText.GrabFocus();
    }

    /// Save in the editor: writes the file (or the bench's copy of it), then checks again.
    public string? SaveEditor()
    {
        if (_editorFile == null || BenchOf?.Invoke() is not { } b) return null;
        try
        {
            var before = (b.AircraftFile, b.BoardConfigFile);
            var written = BenchFiles.SaveSource(b, _editorFile, _editorText.Text, Root, out var note);
            Logged?.Invoke($"config: {written} written{(note != "" ? " — " + note : "")}");
            _editor.Visible = false; _editorFile = null;
            if (before != (b.AircraftFile, b.BoardConfigFile)) BenchChanged?.Invoke();
            CheckRequested?.Invoke();
            return written;
        }
        catch (Exception e) { Logged?.Invoke($"config: not saved: {e.Message}"); return null; }
    }

    // ---- self-tests / screenshots ----
    public string InfoText => _info.Text;
    public string BenchLinesText { get => _bench.Text; set { _bench.Text = value; UpdateBenchHead(); } }
    public string BenchHeadText => _benchHead.Text;
    public bool EditorOpen => _editor.Visible;
    public string EditorHead => _editorHead.Text;
    public string EditorText { get => _editorText.Text; set => _editorText.Text = value; }
    public int EditorCaretLine => _editorText.GetCaretLine();
    public string SelectionText => _sel.Text;
    /// What the tree shows: per line row (layer group title, text cell, source cell, checked, amber).
    public List<(string Group, string Text, string Source, bool On, bool Conflict)> Rows()
    {
        var list = new List<(string, string, string, bool, bool)>();
        foreach (var (it, l) in _rows) list.Add((it.GetParent().GetText(0), it.GetText(0), it.GetText(1), it.IsChecked(2), l.Conflict != null && !l.Disabled));
        return list;
    }
    public void SelectLine(ConfigLine l)
    {
        var it = _rows.FirstOrDefault(kv => kv.Value == l).Key;
        if (it != null) { it.GetParent().Collapsed = false; it.Select(0); _tree.ScrollToItem(it); }
        Select(l);
    }
    /// Unticks / ticks a line's check box as a click would.
    public void ClickCheck(ConfigLine l)
    {
        var it = _rows.FirstOrDefault(kv => kv.Value == l).Key;
        if (it == null) return;
        it.SetChecked(2, !it.IsChecked(2));
        SetDisabled(l, !it.IsChecked(2));
    }
}
