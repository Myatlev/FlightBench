// Two flight-screen panels built from the core's read-only views of the firmware (docs/ui.md):
// - FunctionsPanel ("Functions" window): the manifest's environment functions (calibrate_acc, calibrate_gyro,
//   launch…) as buttons that send {"type":"function","name":…}, each with its progress line from the core's
//   `function` messages (FunctionProgress), and a per-bench "run at first start" checkbox that mirrors the
//   catalog's `auto_first_start` (saved in the bench; informational: the protocol has no session field for it).
// - DiffView (the right half of the Config window, ConfigWindow.cs): the firmware's applied config as `diff all` text; the header
//   says its source (first start / saved snapshot / on request), when, and whether it is stale (and why). Show
//   cached ({"type":"diff","refresh":false}: the core's last diff, the firmware untouched) is the default; Refresh from
//   firmware ({"type":"diff"}: the core runs `diff all` in the CLI and leaves with `exit`, which REBOOTS the firmware)
//   asks first, and only while connected and disarmed. Copy. Nothing here edits the firmware.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

/// The progress of each environment function, from the core's `function` messages.
public sealed class FunctionProgress
{
    public sealed class Entry
    {
        public string Phase = "";          // start | step | done | failed
        public int Step, Steps;
        public string Detail = "";
        public double T = double.NaN;
    }

    private readonly Dictionary<string, Entry> _e = new();
    public Entry? Get(string name) => _e.TryGetValue(name, out var e) ? e : null;

    /// A `function` message (or an `event` "function" data object). Returns (name, log line) or null when it names no function.
    public (string Name, string Log)? Apply(JsonElement m)
    {
        if (m.ValueKind != JsonValueKind.Object || Json.Str(m, "name") is not { Length: > 0 } name) return null;
        var e = _e.TryGetValue(name, out var old) ? old : _e[name] = new Entry();
        var phase = Json.Str(m, "phase") ?? "step";
        if (phase == "start") { e.Step = 0; e.Steps = 0; e.Detail = ""; }
        e.Phase = phase;
        if (Json.TryNum(m, "step", out var k)) e.Step = (int)k;
        if (Json.TryNum(m, "steps", out var n)) e.Steps = (int)n;
        e.Detail = Json.Str(m, "detail") ?? (phase == "start" ? "" : e.Detail);
        e.T = Json.Num(m, "t", double.NaN);
        bool replay = Json.TryBool(m, "replay", out var rp) && rp;
        return (name, $"{name}: {Line(name)}{(replay ? " (replay: before this client connected)" : "")}");
    }

    public void Clear() => _e.Clear();

    /// The function the core is running now (its last message was start / step), else null.
    public string? Running => _e.FirstOrDefault(x => x.Value.Phase is "start" or "step").Key;

    /// The panel's progress line for a function ("not run in this session" before any message).
    public string Line(string name)
    {
        if (!_e.TryGetValue(name, out var e)) return "not run in this session";
        var t = double.IsNaN(e.T) ? "" : $" (t = {e.T.ToString("0.00", CultureInfo.InvariantCulture)} s)";
        var of = e.Steps > 0 ? $" {e.Step}/{e.Steps}" : e.Step > 0 ? $" {e.Step}" : "";
        var d = e.Detail != "" ? ": " + e.Detail : "";
        return e.Phase switch
        {
            "start" => $"running…{of}{d}{t}",
            "step" => $"step{of}{d}{t}",
            "done" => $"done{d}{t}",
            "failed" => $"FAILED{d}{t}",
            _ => $"{e.Phase}{of}{d}{t}",
        };
    }
}

public partial class FunctionsPanel : VBoxContainer
{
    /// Sends a message to the core; false = not sent (not connected).
    public Func<object, bool>? Send;
    /// A "run at first start" box changed (name, on): the flight screen stores it in the bench.
    public event Action<string, bool>? FirstStartChanged;
    public event Action<string>? Logged;

    public readonly FunctionProgress Progress = new();
    public IReadOnlyList<ManifestFunction> Catalog => _catalog;
    private List<ManifestFunction> _catalog = new();
    private readonly Dictionary<string, (Button Run, Label Line, CheckBox First)> _rows = new();
    private readonly Label _info = new() { AutowrapMode = TextServer.AutowrapMode.WordSmart, Modulate = Ui.Soft };
    private readonly GridContainer _grid = new() { Columns = 3 };
    private bool _connected;

    public FunctionsPanel()
    {
        AddThemeConstantOverride("separation", 6);
        AddChild(_info);
        _grid.AddThemeConstantOverride("h_separation", 10); _grid.AddThemeConstantOverride("v_separation", 4);
        AddChild(_grid);
        AddChild(new Label
        {
            Text = "Buttons run the function in the core (poses / MSP / scenario, as the core defines it). \"at first start\" (run it at a first start) is kept in the bench; the core decides the first-start set from the aircraft (informational). Launch (L) also stays in the bottom bar.",
            AutowrapMode = TextServer.AutowrapMode.WordSmart, Modulate = Ui.Soft,
        });
        SetCatalog(new List<ManifestFunction>(), null, "");
    }

    /// Buttons from the catalog; `firstStart` = the bench's choices (a missing name = the catalog's auto_first_start).
    public void SetCatalog(List<ManifestFunction> catalog, Dictionary<string, bool>? firstStart, string source)
    {
        _catalog = catalog.ToList();
        foreach (var c in _grid.GetChildren()) { _grid.RemoveChild(c); c.QueueFree(); }
        _rows.Clear();
        _info.Text = _catalog.Count == 0
            ? $"The core's manifest lists no environment functions for this bench{(source != "" ? $" ({source})" : "")}: a core before `functions`, or none defined for the aircraft."
            : $"{_catalog.Count} environment function(s){(source != "" ? $" from {source}" : "")}.";
        foreach (var f in _catalog)
        {
            var name = f.Name;
            var run = new Button { Text = f.DisplayLabel, FocusMode = FocusModeEnum.None, TooltipText = $"{{\"type\":\"function\",\"name\":\"{name}\"}}{(f.ParamsText != "" ? "\nparams: " + f.ParamsText : "")}", CustomMinimumSize = new Vector2(220, 0) };
            run.Pressed += () => RunFunction(name);
            var line = new Label { Text = Progress.Line(name), AutowrapMode = TextServer.AutowrapMode.WordSmart, CustomMinimumSize = new Vector2(220, 0), SizeFlagsHorizontal = SizeFlags.ExpandFill };
            var first = new CheckBox { Text = "at first start", FocusMode = FocusModeEnum.None, ButtonPressed = firstStart != null && firstStart.TryGetValue(name, out var on) ? on : f.AutoFirstStart,
                                       TooltipText = $"catalog default: {(f.AutoFirstStart ? "yes" : "no")} (auto_first_start); saved in the bench, informational" };
            first.Toggled += v => FirstStartChanged?.Invoke(name, v);
            _grid.AddChild(run); _grid.AddChild(line); _grid.AddChild(first);
            _rows[name] = (run, line, first);
        }
        SetConnected(_connected);
    }

    /// Run buttons: only while connected and no function is running (the core runs one at a time; its done / failed
    /// message, or a new connection, frees them).
    public void SetConnected(bool connected)
    {
        _connected = connected;
        var running = Progress.Running;
        foreach (var r in _rows.Values) r.Run.Disabled = !connected || running != null;
    }

    /// A button: {"type":"function","name":…} (only while connected).
    public bool RunFunction(string name)
    {
        if (Send == null || !_rows.ContainsKey(name)) return false;
        if (Progress.Running is { } busy) { Logged?.Invoke($"function {name}: not sent ({busy} is running)"); return false; }
        bool sent = Send(new { type = "function", name });
        Logged?.Invoke(sent ? $"function {name}: sent" : $"function {name}: not sent (not connected)");
        return sent;
    }

    /// A `function` message: its row's progress line (a function the catalog does not list is logged only).
    public string? OnFunction(JsonElement m)
    {
        if (Progress.Apply(m) is not { } r) return null;
        if (_rows.TryGetValue(r.Name, out var row)) row.Line.Text = Progress.Line(r.Name);
        SetConnected(_connected);                          // busy / free
        return r.Log;
    }

    public void ClearProgress()
    {
        Progress.Clear();
        foreach (var (n, r) in _rows) r.Line.Text = Progress.Line(n);
        SetConnected(_connected);
    }

    // self-tests
    public Button? RunButton(string name) => _rows.TryGetValue(name, out var r) ? r.Run : null;
    public string LineText(string name) => _rows.TryGetValue(name, out var r) ? r.Line.Text : "";
    public CheckBox? FirstStartBox(string name) => _rows.TryGetValue(name, out var r) ? r.First : null;
    public string InfoText => _info.Text;
}

public partial class DiffView : VBoxContainer
{
    public const string RebootWarning = "Refresh reboots the firmware — the model is disarmed and re-arms only by you";
    /// Show cached (false) or Refresh from firmware confirmed (true): the flight screen sends {"type":"diff","refresh":false} / {"type":"diff"}.
    public event Action<bool>? RefreshRequested;
    public event Action<string>? Logged;
    public FirmwareDiff? Diff { get; private set; }
    public string LastCopied { get; private set; } = "";

    private readonly Label _header = new() { AutowrapMode = TextServer.AutowrapMode.WordSmart };
    private readonly TextEdit _text = new() { Editable = false, SizeFlagsVertical = SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(200, 120), WrapMode = TextEdit.LineWrappingMode.None };
    private readonly Label _refusal = new() { Visible = false, Modulate = new Color(1f, 0.75f, 0.4f), AutowrapMode = TextServer.AutowrapMode.WordSmart };
    private readonly Button _cached = new() { Text = "Show cached", FocusMode = FocusModeEnum.None };
    private readonly Button _refresh = new() { Text = "Refresh from firmware (reboots)…", FocusMode = FocusModeEnum.None };
    private readonly Button _copy = new() { Text = "Copy to clipboard", FocusMode = FocusModeEnum.None };
    private Node? _dialogParent;
    private ConfirmationDialog? _confirm;

    public DiffView()
    {
        AddThemeConstantOverride("separation", 6);
        _header.Text = "no diff yet: the core sends the firmware's `diff all` after its config is applied (Show cached asks for the core's last one)";
        AddChild(_header);
        AddChild(_refusal);
        _text.AddThemeFontOverride("font", ThemeDB.FallbackFont);
        AddChild(_text);
        var row = new HFlowContainer();
        row.AddThemeConstantOverride("h_separation", 6);
        _cached.Pressed += () => { if (!_cached.Disabled) RefreshRequested?.Invoke(false); };
        _refresh.Pressed += AskFirmwareRefresh;
        _copy.Pressed += Copy;
        row.AddChild(_cached); row.AddChild(_refresh); row.AddChild(_copy);
        AddChild(row);
        AddChild(new Label { Text = RebootWarning + " (the core leaves the CLI with exit). This is the firmware's own config (the configurator changes it); Changes compares it with the script, the left side edits the script.", Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart });
        SetRefreshState(false, null);
    }

    /// Where the confirmation dialog goes (the flight screen's UI layer).
    public void SetWarnParent(Node parent) => _dialogParent = parent;

    /// Refresh from firmware: asks first (the firmware reboots); without a dialog parent (tests) nothing is sent.
    private void AskFirmwareRefresh()
    {
        if (_refresh.Disabled) return;
        _confirm?.QueueFree();
        _confirm = new ConfirmationDialog { Title = "Refresh from firmware", DialogText = RebootWarning + ".\n\nThe core enters the CLI, runs diff all and leaves with exit.", OkButtonText = "Refresh (reboot)", Theme = Ui.BaseTheme };
        _confirm.Confirmed += ConfirmFirmwareRefresh;
        _confirm.VisibilityChanged += () => { if (_confirm is { Visible: false } c) { c.QueueFree(); _confirm = null; } };
        if (_dialogParent == null) return;
        _dialogParent.AddChild(_confirm);
        _confirm.PopupCentered();
    }

    /// The dialog's OK: {"type":"diff"} (still only when allowed).
    public void ConfirmFirmwareRefresh()
    {
        if (!_refresh.Disabled) RefreshRequested?.Invoke(true);
    }

    /// The core refused / could not refresh (`error`): one line under the header until the next diff.
    public void ShowRefusal(string error)
    {
        _refusal.Text = error switch
        {
            "none" => "the core has no diff yet (it takes one at the first start, or on Refresh from firmware)",
            "armed" => "not refreshed: the firmware is armed (disarm first)",
            "calibrating" => "not refreshed: the firmware is calibrating (a few seconds after a boot) — try again",
            "cli" => "not refreshed: the firmware is in its CLI (exit CLI first)",
            "busy" => "not refreshed: the first start or a function is running",
            "not_ready" => "not refreshed: the firmware has not answered yet",
            "power_off" => "not refreshed: the battery is off",
            var e => $"not refreshed: {e}",
        };
        _refusal.Visible = true;
    }

    public void Show(FirmwareDiff? d, bool fromBench)
    {
        Diff = d;
        if (d == null) return;
        _header.Text = d.Header(fromBench);
        _header.Modulate = d.Stale ? new Color(1f, 0.75f, 0.4f) : Colors.White;
        _text.Text = d.Text;
        if (d.Error == null) _refusal.Visible = false;
    }

    /// Show cached while connected; Refresh from firmware only when connected and the firmware is known disarmed (it
    /// reboots the firmware; the core refuses it while armed).
    public void SetRefreshState(bool connected, bool? armed)
    {
        _cached.Disabled = !connected;
        _cached.TooltipText = connected ? "{\"type\":\"diff\",\"refresh\":false}: the core's last diff; the firmware is not touched" : "not connected to the core";
        _refresh.Disabled = !connected || armed != false;
        _refresh.TooltipText = !connected ? "not connected to the core"
                             : armed == true ? "the firmware is armed: disarm first (refreshing reboots the firmware)"
                             : armed == null ? "waiting for the firmware's state"
                             : "{\"type\":\"diff\"}: " + RebootWarning;
    }

    public void Copy()
    {
        LastCopied = _text.Text;
        DisplayServer.ClipboardSet(LastCopied);
        Logged?.Invoke($"firmware config: {LastCopied.Split('\n').Length} lines copied to the clipboard");
    }

    // self-tests
    public string HeaderText => _header.Text;
    public string BodyText => _text.Text;
    public bool Editable => _text.Editable;
    public bool RefreshDisabled => _refresh.Disabled;
    public bool CachedDisabled => _cached.Disabled;
    public string RefusalText => _refusal.Visible ? _refusal.Text : "";
    public string RefreshTooltip => _refresh.TooltipText;
    public void PressCached() => _cached.EmitSignal(BaseButton.SignalName.Pressed);
    /// Presses Refresh from firmware and confirms its dialog (as the user's OK).
    public void PressRefresh() { _refresh.EmitSignal(BaseButton.SignalName.Pressed); if (_confirm != null || _dialogParent == null) ConfirmFirmwareRefresh(); _confirm?.Hide(); }
}
