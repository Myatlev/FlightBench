// The flight screen's **Config** window (was "Firmware config"; product owner 2026-10-02: a developer tool — nothing implicit
// or read-only; core branch `config-layers`):
// - mode **Script ↔ Firmware**: left the script the core sends, by layer (LayersView: editable at the source), right the
//   firmware's current `diff all` (DiffView: Show cached / Refresh from firmware (reboots)… / Copy);
// - mode **Changes** (`{"type":"diff","against":"script"}`): what the configurator (or anything else) changed against
//   the script — changed settings (script → firmware), only in the firmware (added), only in the script (removed / back at
//   the default); selected rows go into the bench lines (layer 5) or disable the script's line: the developer's main tool;
// - operations, explicit, those that reboot confirmed first: **Apply script now (reboots)…** (`{"type":"config",
//   "apply":true}`, **full** = every line, not only the delta), **Re-run <function>…** (each environment function of the
//   aircraft), **Reset to first start…**, **Snapshot firmware config → bench lines…** (`{"type":"config","snapshot":"bench",
//   "path":<bench_cli>}`: the core takes `diff all` — reboots — and writes it as the bench lines), **Export as preset…**
//   (the bench JSON + bench lines + its own copies of modified files + a README with the layers, into a folder).
using System;
using System.Collections.Generic;
using System.Linq;
using Godot;

namespace FlightBench;

public partial class ConfigPanel : VBoxContainer
{
    public readonly LayersView Layers = new() { SizeFlagsHorizontal = SizeFlags.ExpandFill };
    public readonly DiffView Diff;
    public event Action<bool>? ApplyRequested;                 // full
    public event Action<string>? RerunRequested;               // function name
    public event Action? ResetRequested;
    public event Action? SnapshotRequested;
    public event Action<string>? ExportRequested;              // folder
    public event Action? ChangesRequested;
    public event Action<string>? Logged;

    private readonly TabBar _mode = new() { FocusMode = FocusModeEnum.None };
    private readonly HSplitContainer _split = new() { SizeFlagsVertical = SizeFlags.ExpandFill };
    private readonly VBoxContainer _changes = new() { SizeFlagsVertical = SizeFlags.ExpandFill, Visible = false };
    private readonly Label _layersHead = new() { AutowrapMode = TextServer.AutowrapMode.WordSmart };
    private readonly Label _status = new() { AutowrapMode = TextServer.AutowrapMode.WordSmart, Visible = false };
    // operations
    private readonly Button _apply = new() { Text = "Apply script now (reboots)…", FocusMode = FocusModeEnum.None };
    private readonly CheckBox _full = new() { Text = "full", FocusMode = FocusModeEnum.None, TooltipText = "every line of the script (full: true), not only the lines the firmware's config lacks" };
    private readonly HBoxContainer _rerun = new();
    private readonly Button _reset = new() { Text = "Reset to first start…", FocusMode = FocusModeEnum.None };
    private readonly Button _snapshot = new() { Text = "Snapshot firmware config → bench lines…", FocusMode = FocusModeEnum.None };
    private readonly Button _export = new() { Text = "Export as preset…", FocusMode = FocusModeEnum.None };
    // changes
    private readonly Label _changesHead = new() { AutowrapMode = TextServer.AutowrapMode.WordSmart, SizeFlagsHorizontal = SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(200, 0) };
    private readonly Button _compare = new() { Text = "Compare firmware with the script", FocusMode = FocusModeEnum.None };
    private readonly Tree _changesTree = new() { Columns = 2, HideRoot = true, SelectMode = Tree.SelectModeEnum.Multi, SizeFlagsVertical = SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(0, 200) };
    private readonly Button _take = new() { Text = "Selected → bench lines", FocusMode = FocusModeEnum.None };
    private readonly Button _takeAll = new() { Text = "All changes → bench lines", FocusMode = FocusModeEnum.None };
    private readonly Button _disableScript = new() { Text = "Disable the script's line", FocusMode = FocusModeEnum.None };
    private readonly Dictionary<TreeItem, (string Kind, string BenchLine, string ScriptLine)> _changeRows = new();
    public ConfigChanges? Changes { get; private set; }
    private Node? _dialogParent;
    private ConfirmationDialog? _confirm;
    private Action? _confirmed;
    private bool _connected;
    private bool? _armed;

    public ConfigPanel(DiffView diff)
    {
        Diff = diff;
        AddThemeConstantOverride("separation", 6);
        _mode.AddTab("Script ↔ Firmware");
        _mode.AddTab("Changes");
        _mode.SetTabTooltip(0, "left: the script the core sends, by layer (editable at the source); right: the firmware's diff all");
        _mode.SetTabTooltip(1, "diff against script: what the configurator changed, added, removed — take it into the bench lines");
        _mode.TabChanged += t => ShowMode((int)t);
        AddChild(_mode);
        // operations
        var ops = new HFlowContainer();
        ops.AddThemeConstantOverride("h_separation", 6); ops.AddThemeConstantOverride("v_separation", 4);
        _apply.TooltipText = "{\"type\":\"config\",\"apply\":true}: the core sends the script to the firmware now and saves (the firmware reboots); refused while armed";
        _apply.Pressed += () => Ask("Apply script now", "The core sends the bench's script to the firmware now and saves; the firmware REBOOTS (disarmed; re-arm yourself)."
                                    + (_full.ButtonPressed ? "\n\nfull: every line of the script is typed again (the configurator's changes to those settings are overwritten)." : "\n\nOnly the lines the firmware's config lacks (tick 'full' for every line)."), "Apply (reboot)", () => ApplyRequested?.Invoke(_full.ButtonPressed));
        _reset.TooltipText = "{\"type\":\"config\",\"reset\":\"first_start\"}: the core discards the bench's saved config and runs the first start again (the firmware reboots)";
        _reset.Pressed += () => { if (!_reset.Disabled) ResetRequested?.Invoke(); };
        _snapshot.TooltipText = "{\"type\":\"config\",\"snapshot\":\"bench\",\"path\":<the bench lines file>}: the core runs diff all (the firmware reboots) and writes it as this bench's bench lines (layer 5)";
        _snapshot.Pressed += () => Ask("Snapshot firmware config → bench lines", "The core runs `diff all` in the firmware's CLI (it leaves with exit: the firmware REBOOTS) and writes the result as this bench's bench lines (layer 5), replacing them.\n\nThe bench lines then carry the firmware's whole config as the configurator left it: the next first start types it last (it wins).", "Snapshot (reboot)", () => SnapshotRequested?.Invoke());
        _export.TooltipText = "a folder: the bench JSON, its bench lines, its own copies of modified aircraft / board files, a README with the layer list";
        _export.Pressed += () => { if (_dialogParent != null) Ui.PickDir(_dialogParent, null, dir => ExportRequested?.Invoke(dir)); };
        ops.AddChild(_apply); ops.AddChild(_full); ops.AddChild(new VSeparator()); ops.AddChild(_rerun); ops.AddChild(_reset);
        ops.AddChild(new VSeparator()); ops.AddChild(_snapshot); ops.AddChild(_export);
        AddChild(ops);
        _status.Modulate = new Color(1f, 0.75f, 0.4f);
        AddChild(_status);
        // script ↔ firmware
        var left = new VBoxContainer { SizeFlagsHorizontal = SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(360, 0) };
        _layersHead.Modulate = Ui.Soft;
        left.AddChild(_layersHead);
        var ls = new ScrollContainer { SizeFlagsVertical = SizeFlags.ExpandFill, HorizontalScrollMode = ScrollContainer.ScrollMode.Disabled };
        ls.AddChild(Layers);
        left.AddChild(ls);
        Diff.SizeFlagsHorizontal = SizeFlags.ExpandFill;
        Diff.CustomMinimumSize = new Vector2(300, 0);
        _split.AddChild(left);
        _split.AddChild(Diff);
        AddChild(_split);
        // changes
        _changesHead.Text = "Changes: not compared yet — Compare asks the core for the firmware's diff against the script it sent";
        _compare.TooltipText = "{\"type\":\"diff\",\"against\":\"script\"}: settings changed / added / removed against the script";
        _compare.Pressed += () => { if (!_compare.Disabled) ChangesRequested?.Invoke(); };
        var ch = new HBoxContainer(); ch.AddThemeConstantOverride("separation", 8);
        ch.AddChild(_changesHead); ch.AddChild(_compare);
        _changes.AddChild(ch);
        _changesTree.SetColumnTitle(0, "setting / line"); _changesTree.SetColumnTitle(1, "script → firmware");
        _changesTree.ColumnTitlesVisible = true;
        _changesTree.SetColumnExpand(0, true); _changesTree.SetColumnExpand(1, true);
        _changesTree.AddThemeFontSizeOverride("font_size", 14);
        _changesTree.MultiSelected += (_, _, _) => UpdateTakeButtons();
        _changes.AddChild(_changesTree);
        _take.TooltipText = "the firmware's value of the selected changed / added settings as bench lines (layer 5, typed last: it wins); saved with Save bench lines";
        _take.Pressed += () => TakeIntoBench(selectedOnly: true);
        _takeAll.Pressed += () => TakeIntoBench(selectedOnly: false);
        _disableScript.TooltipText = "a setting only the script has (the firmware is back at its default): skip the script's line (disabled_lines)";
        _disableScript.Pressed += DisableSelectedScriptLines;
        _changes.AddChild(Ui.Row(_take, _takeAll, _disableScript));
        _changes.AddChild(new Label { Text = "Then Script ↔ Firmware → Save bench lines, and Apply script now (or the next start) puts them in.", Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart });
        AddChild(_changes);
        Layers.Logged += s => Logged?.Invoke(s);
        SetState(false, null);
        UpdateTakeButtons();
    }

    public void SetWarnParent(Node parent) { _dialogParent = parent; Diff.SetWarnParent(parent); }

    /// "the script this core started with (session-check at the launch)" / "the bench as it is now — not sent yet…".
    public void SetLayersSource(string text) => _layersHead.Text = text;
    public string LayersSource => _layersHead.Text;

    private void ShowMode(int m)
    {
        _split.Visible = m == 0;
        _changes.Visible = m == 1;
        if (m == 1 && Changes == null && !_compare.Disabled) ChangesRequested?.Invoke();
    }

    /// Asks first (the operation reboots the firmware); without a dialog parent (tests) nothing happens until ConfirmPending.
    private void Ask(string title, string text, string ok, Action then)
    {
        _confirm?.QueueFree();
        _confirmed = then;
        _confirm = new ConfirmationDialog { Title = title, DialogText = text, OkButtonText = ok, Theme = Ui.BaseTheme };
        _confirm.Confirmed += ConfirmPending;
        _confirm.VisibilityChanged += () => { if (_confirm is { Visible: false } c) { c.QueueFree(); _confirm = null; } };
        if (_dialogParent == null) return;
        _dialogParent.AddChild(_confirm);
        _confirm.PopupCentered();
    }

    /// The confirmation's OK (tests call it as the user's click).
    public void ConfirmPending() { var a = _confirmed; _confirmed = null; a?.Invoke(); }
    public string PendingDialogText => _confirm?.DialogText ?? "";

    /// The aircraft's environment functions: one "Re-run <label>…" each (asks: a calibration may reboot / take minutes).
    public void SetFunctions(IEnumerable<ManifestFunction> fns)
    {
        foreach (var c in _rerun.GetChildren()) { _rerun.RemoveChild(c); c.QueueFree(); }
        foreach (var f in fns)
        {
            var name = f.Name;
            var b = new Button { Text = $"Re-run {f.Name}…", FocusMode = FocusModeEnum.None, TooltipText = $"{{\"type\":\"function\",\"name\":\"{name}\"}}: {f.DisplayLabel}{(f.AutoFirstStart ? " (runs at a first start)" : "")}" };
            b.Pressed += () => Ask($"Re-run {name}", $"Run '{f.DisplayLabel}' now? The core runs it as it defines it (poses / MSP / a save: the firmware may reboot; the model moves as the function says).", "Run", () => RerunRequested?.Invoke(name));
            _rerun.AddChild(b);
        }
        SetState(_connected, _armed);
    }

    /// Operations only while connected; the rebooting ones only while the firmware is known disarmed.
    public void SetState(bool connected, bool? armed)
    {
        _connected = connected; _armed = armed;
        bool safe = connected && armed == false;
        string why = !connected ? "not connected to the core" : armed == true ? "the firmware is armed: disarm first (this reboots it)" : armed == null ? "waiting for the firmware's state" : "";
        foreach (var b in new[] { _apply, _snapshot, _reset }) { b.Disabled = !safe; if (!safe) b.TooltipText = why; }
        foreach (var b in _rerun.GetChildren().OfType<Button>()) b.Disabled = !safe;
        if (safe)
        {
            _apply.TooltipText = "{\"type\":\"config\",\"apply\":true}: the core sends the script to the firmware now and saves (the firmware reboots)";
            _reset.TooltipText = "{\"type\":\"config\",\"reset\":\"first_start\"}: the core discards the bench's saved config and runs the first start again";
            _snapshot.TooltipText = "{\"type\":\"config\",\"snapshot\":\"bench\",\"path\":…}: diff all (reboots) → this bench's bench lines";
        }
        _compare.Disabled = !connected;
        _compare.TooltipText = connected ? "{\"type\":\"diff\",\"against\":\"script\"}: the core compares the firmware's diff all with the script (when it takes a fresh diff all the firmware reboots, as Refresh from firmware)" : "not connected to the core";
    }

    /// A refusal / result line under the operations (cleared by the next one).
    public void SetStatus(string text, bool warn = true)
    {
        _status.Text = text;
        _status.Visible = text != "";
        _status.Modulate = warn ? new Color(1f, 0.75f, 0.4f) : Ui.Soft;
    }
    public string StatusText => _status.Visible ? _status.Text : "";

    // ---- changes ----
    /// The core answered the comparison with something else (a core before config-layers).
    public void ChangesUnsupported(string why) { _changesHead.Text = "Changes: " + why; }

    public void ShowChanges(ConfigChanges c)
    {
        Changes = c;
        _changesTree.Clear();
        _changeRows.Clear();
        var root = _changesTree.CreateItem();
        if (c.Error != null) _changesHead.Text = $"Changes: not compared ({c.Error})";
        else _changesHead.Text = c.Total == 0 ? "Changes: none — the firmware's config is the script's" : $"Changes against the script: {c.ChangedSettings.Count} changed, {c.OnlyFirmware.Count} only in the firmware, {c.OnlyScript.Count} only in the script{(c.T is { } t ? $" (t = {t:0.00} s)" : "")}";
        TreeItem Group(string title, Color col)
        {
            var g = _changesTree.CreateItem(root);
            g.SetText(0, title); g.SetCustomColor(0, col); g.SetSelectable(0, false); g.SetSelectable(1, false);
            return g;
        }
        var gc = Group($"Changed by the configurator ({c.ChangedSettings.Count})", new Color(1f, 0.8f, 0.4f));
        foreach (var x in c.ChangedSettings)
        {
            var it = _changesTree.CreateItem(gc);
            it.SetText(0, x.Setting); it.SetText(1, $"{x.Script} → {x.Firmware}");
            _changeRows[it] = ("changed", ConfigChanges.BenchLineFor(x), x.Script.Trim());
        }
        var gf = Group($"Only in the firmware — added ({c.OnlyFirmware.Count})", new Color(0.5f, 0.9f, 0.6f));
        foreach (var x in c.OnlyFirmware)
        {
            var it = _changesTree.CreateItem(gf);
            it.SetText(0, x); it.SetText(1, "(not in the script) → firmware");
            _changeRows[it] = ("only_firmware", x, "");
        }
        var gs = Group($"Only in the script — removed / at the firmware's default ({c.OnlyScript.Count})", new Color(1f, 0.55f, 0.5f));
        foreach (var x in c.OnlyScript)
        {
            var it = _changesTree.CreateItem(gs);
            it.SetText(0, x); it.SetText(1, "script → (not in the firmware's diff)");
            _changeRows[it] = ("only_script", "", x);
        }
        UpdateTakeButtons();
    }

    private List<(string Kind, string BenchLine, string ScriptLine)> Selected(bool selectedOnly) =>
        _changeRows.Where(kv => !selectedOnly || kv.Key.IsSelected(0)).Select(kv => kv.Value).ToList();

    private void UpdateTakeButtons()
    {
        var sel = Selected(true);
        _take.Disabled = !sel.Any(s => s.Kind != "only_script");
        _takeAll.Disabled = !_changeRows.Values.Any(s => s.Kind != "only_script");
        _disableScript.Disabled = !sel.Any(s => s.Kind == "only_script");
    }

    /// The firmware's values of changed / added settings → the bench lines box (Script ↔ Firmware: Save bench lines).
    public int TakeIntoBench(bool selectedOnly)
    {
        var lines = Selected(selectedOnly).Where(s => s.Kind != "only_script").Select(s => s.BenchLine).Where(s => s != "").ToList();
        if (lines.Count == 0) return 0;
        Layers.AppendBenchLines(lines, $"from the firmware (Config → Changes, {DateTime.Now:yyyy-MM-dd HH:mm})");
        Logged?.Invoke($"config: {lines.Count} line(s) from the firmware into the bench lines (not saved yet: Script ↔ Firmware → Save bench lines)");
        _mode.CurrentTab = 0;
        return lines.Count;
    }

    private void DisableSelectedScriptLines()
    {
        foreach (var s in Selected(true).Where(s => s.Kind == "only_script"))
        {
            var l = Layers.Config?.Lines.FirstOrDefault(x => x.Text.Trim() == s.ScriptLine.Trim()) ?? new ConfigLine { Text = s.ScriptLine, Layer = "" };
            Layers.SetDisabled(l, true);
        }
    }

    // ---- self-tests ----
    public int Mode => _mode.CurrentTab;
    public void SetMode(int m) { if (_mode.CurrentTab != m) _mode.CurrentTab = m; else ShowMode(m); }
    public string ChangesHead => _changesHead.Text;
    public List<(string Group, string Text, string Detail)> ChangeRows() =>
        _changeRows.Keys.Select(it => (it.GetParent().GetText(0), it.GetText(0), it.GetText(1))).ToList();
    public void SelectChange(string text) { foreach (var it in _changeRows.Keys.Where(i => i.GetText(0) == text)) it.Select(0); UpdateTakeButtons(); }
    public void PressApply() { _apply.EmitSignal(BaseButton.SignalName.Pressed); }
    public void PressSnapshot() { _snapshot.EmitSignal(BaseButton.SignalName.Pressed); }
    public void PressReset() { _reset.EmitSignal(BaseButton.SignalName.Pressed); }
    public void PressCompare() { _compare.EmitSignal(BaseButton.SignalName.Pressed); }
    public void PressRerun(string name) { foreach (var b in _rerun.GetChildren().OfType<Button>().Where(b => b.Text == $"Re-run {name}…")) b.EmitSignal(BaseButton.SignalName.Pressed); }
    public bool Full { get => _full.ButtonPressed; set => _full.ButtonPressed = value; }
    public bool ApplyDisabled => _apply.Disabled;
    public bool SnapshotDisabled => _snapshot.Disabled;
    public List<string> RerunNames => _rerun.GetChildren().OfType<Button>().Select(b => b.Text).ToList();
    public List<string> OperationTexts => new[] { _apply.Text, _reset.Text, _snapshot.Text, _export.Text }.Concat(RerunNames).ToList();
}
