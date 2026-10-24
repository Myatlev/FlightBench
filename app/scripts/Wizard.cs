// New/edit bench wizard: Board → Aircraft → Wiring → Board config → Summary (+ Core: local / remote, core time). Reads boards/ and
// models/ from the repo root; step 3 is the wiring graph (WiringGraph.cs): catalogue, pads (printed
// labels, onboard devices), device ports and actuators from the core's manifest (Manifest.cs); wiring
// seeded with the board's onboard devices + the manifest's suggested defaults (else the unified config).
// Edit mode (Wizard.Open with edit: main menu → Edit bench, flight screen → Bench → Edit bench…): the
// pages start from a copy of that bench; saving under its own name overwrites it, another name makes a
// new bench and leaves it as it was. Save & start never overwrites another bench silently: a name whose
// file exists asks first (save under a free name / overwrite / back).
// Over the flight screen (Wizard.Over: Bench → Edit bench…): the same pages in a window; Cancel raises Closed, Save & start
// writes the bench and raises Saved (the flight screen restarts the core in place with the same bench state).
// Summary: "Firmware config at first start" — the script the core assembles for this bench as LAYERS (1 board · 2 aircraft ·
// 3 device · 4 bench), each line with its source, editable at the source (WizardScript.cs, LayersView.cs): a board /
// aircraft line opens its file (a preset's bundled file is copied next to the bench first), a device line can be disabled
// or overridden, the bench lines are the `bench_cli` file next to the bench; conflicts: the later layer wins.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public partial class Wizard : Control
{
    private BenchData _b = null!;
    private int _step;
    private readonly List<Control> _pages = new();
    private Label _title = null!;
    private Button _back = null!, _next = null!;
    private VBoxContainer _pageHost = null!;
    // page widgets
    private ItemList _boards = null!, _aircrafts = null!;
    private LineEdit _fw = null!, _name = null!, _cfgFile = null!, _profileFile = null!;
    private Label _fwStatus = null!, _cfgFileStatus = null!, _profileStatus = null!;
    private WiringGraph _graph = null!;
    private Label _wiringWarn = null!;
    private TextEdit _cfg = null!;
    private Label _summary = null!;
    private List<string> _boardNames = new(), _aircraftNames = new();

    private string? _editing;                     // edit mode: the name of the bench being edited (overwriting its file is fine)
    /// Opened over the flight screen (Over): Cancel / Save & start raise Closed / Saved instead of changing the scene.
    public bool Overlay { get; private set; }
    private BenchData? _overBench;
    private string? _overEditing;
    /// Over the flight screen: the bench was written (Save & start); the flight screen applies it.
    public event Action<BenchData>? Saved;
    /// Over the flight screen: Cancel (nothing changed).
    public event Action? Closed;

    /// The wizard in edit mode on a copy of `b`, to be put into a window over the flight screen.
    public static Wizard Over(BenchData b) => new() { Overlay = true, _overBench = b.Clone(), _overEditing = b.Name };

    /// Opens the wizard on `b`: a new bench, or (edit) a copy of an existing one that saves back over it.
    public static void Open(SceneTree tree, BenchData b, bool edit)
    {
        AppState.Bench = edit ? b.Clone() : b;
        AppState.Editing = edit ? b.Name : null;
        tree.CallDeferred(SceneTree.MethodName.ChangeSceneToFile, "res://scenes/Wizard.tscn");
    }

    public override void _Ready()
    {
        if (Overlay) { _b = _overBench!; _editing = _overEditing; }
        else
        {
            _b = AppState.Bench ?? BenchData.New();
            _editing = AppState.Editing;
            AppState.Editing = null;              // taken: the next wizard (New bench, a self-test) starts fresh
        }
        ApplyBoardDefault(_b, _editing != null);  // a new bench: the transmitter model of its board's firmware family
        var root = Ui.Screen(this, _editing != null ? $"Edit bench: {_editing}" : "New bench", 1200, 900);
        _title = Ui.Heading(""); root.AddChild(_title);
        // pages scroll inside the card; the step navigation stays at the card's bottom
        _pageHost = new VBoxContainer { SizeFlagsVertical = SizeFlags.ExpandFill };
        root.AddChild(Ui.Scroll(_pageHost));
        _back = Ui.Btn("< Back", () => Show(_step - 1));
        _next = Ui.Btn("Next >", OnNext);
        root.AddChild(Ui.Row(Ui.Btn(Overlay ? "Cancel (back to the bench)" : "Cancel", () => { if (Overlay) Closed?.Invoke(); else GetTree().ChangeSceneToFile("res://scenes/MainMenu.tscn"); }), Ui.Spacer(), _back, _next));

        _pages.Add(BuildBoardPage());
        _pages.Add(BuildAircraftPage());
        _pages.Add(BuildWiringPage());
        _pages.Add(BuildConfigPage());
        _pages.Add(BuildSummaryPage());
        foreach (var p in _pages) { p.Visible = false; p.SizeFlagsVertical = SizeFlags.ExpandFill; p.AddThemeConstantOverride("separation", 8); _pageHost.AddChild(p); }
        Show(0);
    }

    private static readonly string[] Titles = { "1 · Board and firmware", "2 · Aircraft", "3 · Wiring", "4 · Board config (diff all)", "5 · Summary" };

    /// Screenshots (FB_WIZARD_SCROLL): the page scrolled down.
    public void ScrollPage(int px) { if (_pageHost.GetParent() is ScrollContainer s) s.ScrollVertical = px; }

    /// Jump to a page (0-based); used by DevHooks for FB_WIZARD_STEP.
    public void ShowStep(int step) => Show(step);

    private void Show(int step)
    {
        _step = Mathf.Clamp(step, 0, _pages.Count - 1);
        for (int i = 0; i < _pages.Count; i++) _pages[i].Visible = i == _step;
        _title.Text = Titles[_step];
        _back.Disabled = _step == 0;
        _next.Text = _step == _pages.Count - 1 ? "Save & start" : "Next >";
        if (_step == 0) UpdatePathStatus();
        if (_step == 2) SeedWiring();
        if (_step == 3) { UpdateProfileButton(); UpdatePathStatus(); }
        if (_step == 4) { FillSummary(); StartScriptCheck(); }
    }

    private void OnNext()
    {
        if (_step == 0) { if (_boards.GetSelectedItems().Length > 0) _b.Board = _boardNames[_boards.GetSelectedItems()[0]]; _b.FirmwarePath = BenchData.SessionPath(_fw.Text); ApplyBoardDefault(_b, _editing != null); RefreshTxRows(); }
        if (_step == 1) { if (_aircrafts.GetSelectedItems().Length > 0) _b.Aircraft = _aircraftNames[_aircrafts.GetSelectedItems()[0]]; }
        if (_step == 3) { _b.BoardConfig = _cfg.Text; _b.BoardConfigFile = BenchData.SessionPath(_cfgFile.Text); _b.ProfileFile = BenchData.SessionPath(_profileFile.Text); }
        if (_step == _pages.Count - 1)
        {
            if (_b.IsRemote && string.IsNullOrWhiteSpace(_b.CoreHost)) { _coreWarn.Visible = true; return; }   // a remote core needs a host
            var name = string.IsNullOrWhiteSpace(_name.Text) ? $"{_b.Board}-{_b.Aircraft}" : _name.Text.Trim();
            if (AsksBeforeSaving(name, _editing)) AskOverwrite(name);
            else SaveAndStart(name);
            return;
        }
        Show(_step + 1);
    }

    /// Would saving under `name` replace another bench's file? Not when it is the bench being edited
    /// (`editing`, same file name); then it asks (save under a free name / overwrite / back).
    public static bool AsksBeforeSaving(string name, string? editing) =>
        (editing == null || BenchData.PathFor(name) != BenchData.PathFor(editing)) && Godot.FileAccess.FileExists(BenchData.PathFor(name));

    /// Save & start's write: the name, last used, the file.
    public static void Commit(BenchData b, string name, bool newBench = false)
    {
        var old = b.Name;
        b.Name = name;
        // a new name = a new bench: its bench lines / file copies are copied to it (a bench new in this wizard: moved — the
        // "new bench" files must not be left for the next new bench)
        BenchFiles.Rehome(b, old, move: newBench);
        b.LastUsed = DateTime.Now;
        b.Save();
    }

    private void SaveAndStart(string name)
    {
        Commit(_b, name, newBench: _editing == null);
        if (Overlay) { Saved?.Invoke(_b); return; }
        AppState.Bench = _b;
        GetTree().ChangeSceneToFile("res://scenes/Flight.tscn");
    }

    /// Self-tests: Save & start as the button does (on the summary step; the name field's text).
    public void SaveForTest() { Show(_pages.Count - 1); OnNext(); }

    /// The bench being edited (a copy; self-tests change it as the pages would).
    public BenchData EditedBench => _b;

    /// First name "name-2", "name-3"… whose file does not exist.
    private static string FreeName(string name)
    {
        for (int n = 2; ; n++) if (!Godot.FileAccess.FileExists(BenchData.PathFor($"{name}-{n}"))) return $"{name}-{n}";
    }

    /// Another bench already has this file name: save under a free name (default), overwrite it, or go back.
    private void AskOverwrite(string name)
    {
        var free = FreeName(name);
        var other = BenchData.Load(BenchData.PathFor(name));
        var dlg = new ConfirmationDialog
        {
            Title = "Bench exists",
            DialogText = $"A bench saved as '{BenchData.SafeName(name)}.json' already exists{(other != null ? $" ('{other.Name}': {other.Board} / {other.Aircraft})" : "")}.\nSave this one as '{free}', or overwrite it?",
            OkButtonText = $"Save as {free}",
        };
        dlg.AddButton("Overwrite", true, "overwrite");
        dlg.Confirmed += () => SaveAndStart(free);
        dlg.CustomAction += a => { if (a == "overwrite") { dlg.Hide(); SaveAndStart(name); } };
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        AddChild(dlg);
        dlg.PopupCentered();
    }

    // ---- pages ----
    private Control BuildBoardPage()
    {
        var v = new VBoxContainer();
        v.AddChild(new Label { Text = "Supported boards (from repo boards/):" });
        _boards = new ItemList { CustomMinimumSize = new Vector2(0, 120), SizeFlagsVertical = SizeFlags.ExpandFill };
        _boardNames = Repo.Dirs("boards");
        foreach (var n in _boardNames) _boards.AddItem(n + "   " + Repo.Title(Repo.Path("boards", n, "README.md")));
        int sel = _boardNames.IndexOf(_b.Board); if (sel >= 0) _boards.Select(sel);
        v.AddChild(_boards);
        v.AddChild(new Label { Text = "Firmware file (ELF preferred; hex accepted):" });
        var row = new HBoxContainer();
        _fw = new LineEdit { Text = _b.FirmwarePath, SizeFlagsHorizontal = SizeFlags.ExpandFill, PlaceholderText = "path to rotorflight_STM32F7X2_OSD.elf" };
        var browse = Ui.Btn("Browse…", () => Ui.PickFile(this, FirmwareFilters, Ui.FirmwareDir(BenchData.ResolvePath(_fw.Text, CoreLocator.DataRoot)), p => _fw.Text = BenchData.RelativeTo(p, CoreLocator.DataRoot)));
        row.AddChild(_fw); row.AddChild(browse);
        v.AddChild(row);
        _fwStatus = new Label { AutowrapMode = TextServer.AutowrapMode.WordSmart };
        _fw.TextChanged += _ => UpdatePathStatus();
        v.AddChild(_fwStatus);
        v.AddChild(new Label { Text = "MSP on a UART is needed (build with OPTIONS=USE_MSP_UART); the board config is inserted as custom defaults by the core.", AutowrapMode = TextServer.AutowrapMode.Word, Modulate = Ui.Soft });
        return v;
    }

    private Control BuildAircraftPage()
    {
        var v = new VBoxContainer();
        v.AddChild(new Label { Text = "Aircraft (from repo models/):" });
        _aircrafts = new ItemList { CustomMinimumSize = new Vector2(0, 120), SizeFlagsVertical = SizeFlags.ExpandFill };
        _aircraftNames = Repo.Dirs("models");
        foreach (var n in _aircraftNames) _aircrafts.AddItem(n + "   " + Repo.Title(Repo.Path("models", n, "README.md")));
        int sel = _aircraftNames.IndexOf(_b.Aircraft); if (sel >= 0) _aircrafts.Select(sel);
        v.AddChild(_aircrafts);
        v.AddChild(new Label { Text = "The flight screen shows the aircraft's visual from models/<name>/visual/ (placeholder boxes when it has none).", Modulate = Ui.Soft });
        return v;
    }

    private Control BuildWiringPage()
    {
        var v = new VBoxContainer();
        v.AddChild(new Label { Text = "Drag a device port onto a board pad (printed label first, firmware id second; matching kinds and directions only). Grey = onboard: soldered ones stay, detachable ones can be disconnected. Delete or × removes a device; click one to edit it on the right.", AutowrapMode = TextServer.AutowrapMode.Word, Modulate = Ui.Soft });
        // palette = the manifest's devices; not-modelled ones greyed (still wirable: the core ignores them)
        var add = new MenuButton { Text = "+ device", Flat = false, CustomMinimumSize = new Vector2(120, 34) };
        var devices = WiringCatalog.Devices.ToList();
        var pop = add.GetPopup();
        for (int i = 0; i < devices.Count; i++)
        {
            var k = devices[i];
            pop.AddItem($"{k.Id}  ({k.DisplayLabel}: {string.Join(", ", k.WiredPorts.Select(p => $"{p.Id} {WiringCatalog.Label(p.PortKind)}"))})");
            pop.SetItemIcon(i, Dot(k.Implemented ? WiringCatalog.ColorOf(k.Port) : new Color(0.45f, 0.45f, 0.5f)));
            if (!k.Implemented) pop.SetItemTooltip(i, "listed by the core, not modelled yet: can be wired, the core ignores it");
        }
        pop.IndexPressed += i => _graph.AddDevice(devices[(int)i].Id);
        _wiringWarn = new Label();
        v.AddChild(Ui.Row(add, Ui.Btn("Autofill from board config", AutofillWiring), Ui.Btn("Re-layout", () => _graph.Relayout()), Ui.Spacer(), _wiringWarn));

        var h = new HBoxContainer { SizeFlagsVertical = SizeFlags.ExpandFill };
        h.AddThemeConstantOverride("separation", 10);
        _graph = new WiringGraph { CustomMinimumSize = new Vector2(0, 420) };
        _graph.Selected += Inspect;
        _graph.Changed += () => { UpdateWiringWarning(); Inspect(_inspected); };
        _graph.Refused += why => { _wiringWarn.Text = "refused: " + why; _wiringWarn.Modulate = new Color(1f, 0.6f, 0.4f); _wiringWarn.TooltipText = why; };
        h.AddChild(_graph);
        h.AddChild(BuildInspector());
        v.AddChild(h);
        return v;
    }

    // inspector: the selected device's manifest props, written straight into its WiringEntry
    // (bool → check box, enum → dropdown, int/float → spin box, actuator → the aircraft's actuators)
    private WiringEntry? _inspected;
    private Label _inspTitle = null!, _inspSlot = null!, _inspWarn = null!;
    private HBoxContainer _inspButtons = null!;
    private VBoxContainer _inspProps = null!;
    private string[] _actuators = Array.Empty<string>();

    private Control BuildInspector()
    {
        var panel = new PanelContainer { CustomMinimumSize = new Vector2(250, 0) };
        panel.AddThemeStyleboxOverride("panel", Ui.Box(new Color(0.02f, 0.06f, 0.14f), new Color(0.30f, 0.42f, 0.62f, 0.6f), 4, 12, 10));
        var col = new VBoxContainer(); col.AddThemeConstantOverride("separation", 8);
        panel.AddChild(col);
        _inspTitle = new Label { AutowrapMode = TextServer.AutowrapMode.Word }; _inspTitle.AddThemeFontSizeOverride("font_size", 18);
        _inspSlot = new Label { AutowrapMode = TextServer.AutowrapMode.Word, Modulate = Ui.Soft };
        _inspWarn = new Label { AutowrapMode = TextServer.AutowrapMode.WordSmart, Modulate = new Color(1f, 0.72f, 0.35f), Visible = false };
        _inspButtons = new HBoxContainer();
        col.AddChild(_inspTitle); col.AddChild(_inspWarn); col.AddChild(_inspSlot); col.AddChild(_inspButtons);
        _inspProps = new VBoxContainer(); _inspProps.AddThemeConstantOverride("separation", 8);
        col.AddChild(_inspProps);
        Inspect(null);
        return panel;
    }

    private static Control Labeled(string text, Control c)
    {
        var v = new VBoxContainer();
        v.AddChild(new Label { Text = text, Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart });
        v.AddChild(c);
        return v;
    }

    private static Texture2D Dot(Color c)
    {
        var img = Image.CreateEmpty(12, 12, false, Image.Format.Rgba8);
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 12; x++)
                img.SetPixel(x, y, (x - 5.5f) * (x - 5.5f) + (y - 5.5f) * (y - 5.5f) <= 25f ? c : new Color(0, 0, 0, 0));
        return ImageTexture.CreateFromImage(img);
    }

    private void Inspect(WiringEntry? w)
    {
        _inspected = w;
        var kind = w == null ? null : WiringCatalog.Find(w.Device);
        _inspTitle.Text = w == null ? "No device selected" : kind?.DisplayLabel ?? w.Device;
        _inspSlot.Text = w == null ? "Click a device node to edit its properties." : WiresText(w, kind);
        _inspWarn.Visible = false;
        foreach (var c in _inspButtons.GetChildren()) { _inspButtons.RemoveChild(c); c.QueueFree(); }
        foreach (var c in _inspProps.GetChildren()) { _inspProps.RemoveChild(c); c.QueueFree(); }
        if (w == null) return;
        if (_graph.OnboardPad(w) is { } onPad)
        {
            bool detached = _graph.IsDetached(w);
            var part = onPad.OnboardPart != "" ? $" ({onPad.OnboardPart})" : "";
            _inspWarn.Visible = true;
            if (detached) _inspWarn.Text = WiringGraph.DetachedWarning;
            else if (onPad.IsDetachable) { _inspWarn.Text = $"onboard on {onPad.MainText}{part}: detachable"; _inspWarn.Modulate = Ui.Soft; }
            else { _inspWarn.Text = $"soldered to the board on {onPad.MainText}{part}: it cannot be removed"; _inspWarn.Modulate = Ui.Soft; }
            if (detached) _inspWarn.Modulate = new Color(1f, 0.72f, 0.35f);
            if (onPad.IsDetachable)
            {
                var entry = w;
                _inspButtons.AddChild(Ui.Btn(detached ? $"Put back on {onPad.MainText}" : "Detach from the board", () => _graph.SetDetached(entry, !detached)));
            }
        }
        if (kind == null) { _inspProps.AddChild(new Label { Text = "not in the core's manifest: no properties", Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart }); return; }
        if (!kind.Implemented) _inspProps.AddChild(new Label { Text = "the core lists this device but does not model it yet: the wire is kept in the bench, the core ignores it", Modulate = new Color(1f, 0.78f, 0.35f), AutowrapMode = TextServer.AutowrapMode.WordSmart });
        foreach (var p in kind.Props) _inspProps.AddChild(PropEditor(w, p));
        if (kind.Faults.Count > 0)
            _inspProps.AddChild(new Label { Text = "faults (flight screen → Faults): " + string.Join(", ", kind.Faults.Select(f => f.Id)), Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart });
    }

    /// One editor per manifest prop; values go through WiringEntry.SetProp (mapped fields or Props).
    private Control PropEditor(WiringEntry w, ManifestParam p)
    {
        var inv = System.Globalization.CultureInfo.InvariantCulture;
        var label = p.Label != "" ? p.Label : p.Id;
        string cur = w.GetProp(p.Id);
        void Set(string v) { w.SetProp(p.Id, v); _graph.Refresh(w); }
        switch (p.Type)
        {
            case "bool":
            {
                var cb = new CheckBox { Text = label, ButtonPressed = (cur != "" ? cur : p.DefaultText) == "true" };
                cb.Toggled += on => Set(on ? "true" : "false");
                return cb;
            }
            case "enum":
            case "actuator":
            {
                var ob = new OptionButton { SizeFlagsHorizontal = SizeFlags.ExpandFill };
                var items = new List<string>();
                if (p.Type == "actuator") { items.Add(""); items.AddRange(_actuators); }
                else items.AddRange(p.Options);
                if (cur != "" && !items.Contains(cur)) items.Add(cur);
                foreach (var it in items) ob.AddItem(it == "" ? "(none)" : it);
                var shown = cur != "" ? cur : p.Type == "enum" ? p.DefaultText : "";
                ob.Selected = Math.Max(0, items.IndexOf(shown));
                ob.ItemSelected += i => Set(items[(int)i]);
                return Labeled(label, ob);
            }
            case "int":
            case "float":
            {
                bool isInt = p.Type == "int";
                var sb = new SpinBox { MinValue = p.Min ?? -1e6, MaxValue = p.Max ?? 1e6, Step = isInt ? 1 : 0.0001, Rounded = isInt, SizeFlagsHorizontal = SizeFlags.ExpandFill, CustomArrowStep = isInt ? 1 : 0.01 };
                var text = cur != "" ? cur : p.DefaultText;
                sb.Value = double.TryParse(text, System.Globalization.NumberStyles.Float, inv, out var v) ? v : sb.MinValue;
                var reset = new Button { Text = "default", TooltipText = $"unset: the core's default ({(p.DefaultText != "" ? p.DefaultText : "none")})", Disabled = cur == "" };
                sb.ValueChanged += d => { Set(isInt ? ((long)Math.Round(d)).ToString(inv) : d.ToString("0.######", inv)); reset.Disabled = false; };
                reset.Pressed += () => { Set(""); Inspect(w); };
                var row = new HBoxContainer(); row.AddChild(sb); row.AddChild(reset);
                var range = p.Min != null || p.Max != null ? $"  ({p.Min?.ToString(inv) ?? "…"} … {p.Max?.ToString(inv) ?? "…"})" : "";
                return Labeled(label + range, row);
            }
            default:
            {
                var le = new LineEdit { Text = cur, PlaceholderText = p.DefaultText, SizeFlagsHorizontal = SizeFlags.ExpandFill };
                le.TextChanged += t => Set(t.Trim());
                return Labeled($"{label} ({p.Type})", le);
            }
        }
    }

    /// The inspector's wire list: one line per device port (bundled ones with the primary), the pad by its printed label + firmware id.
    private string WiresText(WiringEntry w, ManifestDevice? kind)
    {
        if (kind == null) return w.IsWired ? string.Join("\n", w.Wires().Select(x => $"{(x.Port != "" ? x.Port : "port")} → {PadText(x.Pad)}")) : "not connected (not part of the wiring yet)";
        var lines = new List<string>();
        foreach (var p in kind.Ports)
        {
            var pad = p.Bundled ? (w.PadOf(kind.Primary.Id) is { Length: > 0 } pp ? $"with {kind.Primary.Id} on {PadText(pp)}" : $"with {kind.Primary.Id}") : w.PadOf(p.Id) is { Length: > 0 } x ? PadText(x) : "not wired";
            lines.Add($"{p.Id} → {pad}");
            lines.Add($"    {WiringCatalog.Label(p.PortKind)}{(p.Dir != "both" ? " " + p.Dir : "")}{(p.Optional ? ", optional" : "")}{(p.Bundled ? ", bundled" : "")}{(p.Label != "" ? " · " + p.Label : "")}");
        }
        if (!w.IsWired && !_graph.IsDetached(w)) lines.Add("not connected (not part of the wiring yet)");
        return string.Join("\n", lines);
    }

    private string PadText(string id) => _graph.Pads.FirstOrDefault(p => p.Id == id) is { Label: { } l } ? $"{l} · {id}" : id;

    private void UpdateWiringWarning()
    {
        var m = Manifest.Current;
        bool gyro = _b.Wiring.Any(w => w.IsWired && m.Device(w.Device)?.Category == "imu");
        int wires = _b.Wiring.Sum(w => w.Wires().Count());
        int assumed = _b.Wiring.Count(w => w.Assumed && w.IsWired);
        _wiringWarn.Text = (gyro ? $"{wires} wires{(_b.Detached.Count > 0 ? $", detached: {string.Join(", ", _b.Detached)}" : "")}" : "no gyro wired: the firmware will not find its gyro")
                           + (assumed > 0 ? $"  ·  {assumed} assumed (old bench; saved as ordinary entries)" : "");
        _wiringWarn.TooltipText = assumed > 0 ? WiringGraph.AssumedTip : "";
        _wiringWarn.Modulate = gyro ? Ui.Soft : new Color(1f, 0.78f, 0.35f);
    }

    private string _graphFor = "";

    /// Entering step 3: (re)bind the graph to the chosen board/aircraft; an empty wiring is autofilled.
    private void SeedWiring()
    {
        if (_b.Wiring.Count == 0 && _b.Detached.Count == 0) { AutofillWiring(); return; }
        if (_graphFor == _b.Board + "|" + _b.Aircraft) return;
        BindGraph();
        _graph.Rebuild();
        UpdateWiringWarning();
    }

    private void BindGraph()
    {
        _graphFor = _b.Board + "|" + _b.Aircraft;
        _actuators = Manifest.Current.ActuatorsFor(_b.Aircraft);
        _graph.Setup(_b, Manifest.Current.PadsFor(_b.Board), _actuators, readOnly: false);
    }

    /// Replaces the wiring with the board's onboard devices + the manifest's suggested defaults (else what
    /// the board config implies) and re-lays out the graph.
    public void AutofillWiring()
    {
        if (_graphFor != _b.Board + "|" + _b.Aircraft) BindGraph();
        BoardInfo.AutofillBench(_b, _actuators, Manifest.Current);
        _b.WiringLayout.Clear();
        _graph.Rebuild();
        UpdateWiringWarning();
    }

    /// Dev (FB_WIZARD_SELECT): select a device node so the inspector shows it.
    public void SelectDevice(string deviceId) => _graph.SelectDevice(deviceId);

    /// FB_WIRING_SELFTEST / FB_SELFTEST=wiring: drives the graph's own handlers (as a mouse drag would) and
    /// the bench JSON — onboard devices pre-wired; ESC tlm / rpm wired by port; bench JSON → back → rebuilt
    /// graph equals what went in (wires by device.port → pad, fields, node labels, a saved position);
    /// the session in the new form; direction / kind / onboard-pad refusals; an I2C bus shared; a
    /// detachable onboard device detached (→ `detached` session entry, inspector warning) and put back; a
    /// soldered one refused; the two board nodes; old benches (`slot`) migrated as the core reads them (the
    /// smoke bench: gyro + the onboard devices the old-form rule keeps + assumed ESC/servos, new form once
    /// saved; expectations derived from the manifest, fixed numbers asserted on the bundled fixture); an
    /// omitted detachable onboard device detached by the old-form rule; the OSD device (msp_osd) in the
    /// catalogue with its in/out UART port. Pad checks need a manifest with pads (self-test runs: FB_MANIFEST,
    /// else the fixture tests/manifest-fixture.json); without them they are skipped.
    /// Returns PASS; leaves the wiring autofilled.
    public bool WiringSelfTest()
    {
        var m = Manifest.Current;
        var fails = new List<string>();
        void Check(bool ok, string what) { if (!ok) fails.Add(what); GD.Print($"[selftest]   {(ok ? "ok  " : "FAIL")} {what}"); }
        (int Wires, int Assumed, List<string> Detached) smokeNumbers = (-1, -1, new List<string>());
        ShowStep(2);
        AutofillWiring();
        bool pads = m.Board(_b.Board) is { PadsDeclared: true };
        GD.Print($"[selftest] wiring: manifest {m.Source}; board {_b.Board}: {(pads ? $"{_graph.Pads.Count} pads" : "no pads in the manifest: pad checks skipped")}");
        string Prim(string dev) => m.Device(dev)?.Primary.Id ?? "";
        int Pad(string id) => _graph.PadIndex(id);

        // onboard devices sit on their pads without anyone wiring them, on the "on board" node; the rest go to "ports"
        if (pads)
        {
            foreach (var p in _graph.Pads.Where(p => p.HasOnboard))
                Check(_graph.ConnectedWires().Contains((p.OnboardDevice, Prim(p.OnboardDevice), p.Id)), $"onboard {p.OnboardDevice} pre-wired on {p.MainText} ({(p.IsDetachable ? "detachable" : "soldered")})");
            var tg = _graph.WireTargets();
            Check(_graph.BoardNodeCount == 2 && tg.All(t => t.Node == (_graph.Pads.First(p => p.Id == t.Pad).HasOnboard ? "onboard" : "board")),
                  $"two board nodes: onboard pads on 'on board', the rest on 'ports' ({string.Join(", ", tg.Select(t => $"{t.Device}→{t.Node}:{t.Pad}"))})");
        }

        // ESC's other ports by port id: tlm → TELEM (RX only; the port talks: fine), rpm → RPM
        var escNode = _graph.NodeOfDevice("esc", wiredOnly: true);
        if (pads && escNode != null && m.Device("esc")?.PortById("tlm") != null)
        {
            Check(_graph.TryConnectPad(escNode, _graph.PortIndex(escNode, "tlm"), "uart2") == null, "esc.tlm → TELEM · uart2 accepted");
            if (Pad("freq1") >= 0) Check(_graph.TryConnectPad(escNode, _graph.PortIndex(escNode, "rpm"), "freq1") == null, "esc.rpm → RPM · freq1 accepted");
        }

        // round trip: a reversed servo, a gyro prop, a saved board position → bench JSON → back → rebuild
        var servo = _b.Wiring.FirstOrDefault(w => w.Device == "servo" && w.IsWired);
        if (servo != null) servo.Reversed = true;
        var gyro = _b.Wiring.FirstOrDefault(w => m.Device(w.Device)?.Category == "imu" && w.IsWired);
        gyro?.SetProp("gyro_noise_dps", "0.5");
        _b.WiringLayout["board"] = new NodePos { X = 640, Y = 24 };
        var before = _b.Wiring.Select(Describe).OrderBy(x => x, StringComparer.Ordinal).ToList();
        var json = JsonSerializer.Serialize(_b, Json.Files);
        var back = BenchData.Parse(json);
        _b.Wiring = back.Wiring; _b.WiringLayout = back.WiringLayout; _b.Detached = back.Detached;
        _graph.Rebuild();
        var got = _graph.ConnectedWires();
        var want = _b.Wiring.SelectMany(w => w.Wires().Select(x => (w.Device, x.Port != "" ? x.Port : Prim(w.Device), x.Pad))).ToList();
        GD.Print($"[selftest] wiring: {_b.Wiring.Count} entries, {want.Count} wires -> {json.Length} bytes of bench JSON -> rebuilt graph: {_graph.DeviceNodeCount} device nodes, {got.Count} wires");
        foreach (var w in want) GD.Print($"[selftest]     {w.Device}.{w.Item2} -> {w.Pad}: {(got.Contains(w) ? "wire present" : "MISSING")}");
        Check(want.Count > 0 && got.Count == want.Count && want.All(got.Contains), "round trip: every wire (device.port → pad) back in the graph, none extra");
        Check(!json.Contains("\"slot\""), "bench JSON has no `slot`");
        Check(before.SequenceEqual(_b.Wiring.Select(Describe).OrderBy(x => x, StringComparer.Ordinal)), "fields (port/pad/links/actuator/reversed/option/props) equal after the round trip");
        Check((servo == null || _b.Wiring.Where(w => w.Device == "servo" && w.Pad == servo.Pad).Any(w => _graph.SummaryOf(w)?.Contains("reversed") == true))
              && (gyro == null || _b.Wiring.Where(w => w.Device == gyro.Device).Any(w => _graph.SummaryOf(w)?.Contains("gyro_noise_dps 0.5") == true)), "node labels show reversed / the gyro prop");
        Check(_graph.BoardPosition is { } bp && bp.IsEqualApprox(new Vector2(640, 24)), "saved board position applied");
        var ses = JsonSerializer.Serialize(_b.ToSession("/repo"));
        Check(!ses.Contains("\"slot\"") && ses.Contains("\"pad\":") && (!pads || ses.Contains("\"port\":\"tlm\",\"pad\":\"uart2\"")), "session in the new form (device/port/pad, no slot)");
        GD.Print("[selftest]     session wiring: " + JsonSerializer.Serialize(_b.SessionWiring(m)));

        if (pads)
        {
            // direction: a receiver (uart both) cannot go to the RX-only TELEM; nor onto the onboard receiver's pad; PORT-A is fine
            int n0 = _b.Wiring.Count;
            var rx = _graph.AddDevice("crsf");
            var why = rx == null ? "no crsf" : _graph.TryConnectPad(rx, 0, "uart2");
            Check(why != null && why.Contains("RX") && _b.Wiring.Count == n0, $"crsf → TELEM (rx only) refused: {why}");
            why = rx == null ? "no crsf" : _graph.TryConnectPad(rx, 0, "servo1");
            Check(why != null && _b.Wiring.Count == n0, $"crsf → servo1 (wrong kind) refused: {why}");
            if (_graph.Pads.Any(p => p.Id == "uart5" && p.OnboardDevice == "crsf"))
            {
                why = rx == null ? "no crsf" : _graph.TryConnectPad(rx, 0, "uart5");
                Check(why != null && why.Contains("onboard"), $"crsf → uart5 (holds the onboard receiver) refused: {why}");
            }
            why = rx == null ? "no crsf" : _graph.TryConnectPad(rx, 0, "uart4");
            Check(why == null && _graph.ConnectedWires().Contains(("crsf", Prim("crsf"), "uart4")), $"crsf → PORT-A · uart4 accepted: {why ?? "wired"}; wires {string.Join(",", _graph.ConnectedWires().Where(x => x.Device == "crsf"))}");
            if (rx != null) _graph.Remove(rx);
            Check(_b.Wiring.Count == n0, "removed again");

            // i2c: the compass shares the onboard baro's bus
            var i2c = _graph.Pads.FirstOrDefault(p => p.PortKind == PortKind.I2c && p.HasOnboard);
            var cmp = _graph.AddDevice("compass");
            if (i2c != null && cmp != null)
            {
                why = _graph.TryConnectPad(cmp, 0, i2c.Id);
                var ws = _graph.ConnectedWires();
                Check(why == null && ws.Contains(("compass", Prim("compass"), i2c.Id)) && ws.Contains((i2c.OnboardDevice, Prim(i2c.OnboardDevice), i2c.Id)), $"compass + onboard {i2c.OnboardDevice} share {i2c.MainText}: {why ?? "both wired"}");
                _graph.Remove(cmp);
            }

            // detach a detachable onboard device: Detached, `detached` in the session, the inspector's warning; then put it back
            var det = _graph.Pads.FirstOrDefault(p => p.HasOnboard && p.IsDetachable);
            if (det != null && _graph.NodeOfDevice(det.OnboardDevice, wiredOnly: true) is { } dn)
            {
                var e = _graph.EntryOfNode(dn)!;
                why = _graph.TryDisconnectPad(dn, 0, det.Id);
                var sw = JsonSerializer.Serialize(_b.SessionWiring(m));
                Inspect(e);
                Check(why == null && _b.Detached.Contains(det.OnboardDevice) && !_b.Wiring.Contains(e) && sw.Contains($"{{\"device\":\"{det.OnboardDevice}\",\"detached\":true}}")
                      && !_graph.ConnectedWires().Any(x => x.Device == det.OnboardDevice), $"detach {det.OnboardDevice}: {sw}");
                Check(_inspWarn.Visible && _inspWarn.Text == WiringGraph.DetachedWarning, $"inspector warns: '{_inspWarn.Text}'");
                Check(!BenchData.Parse(JsonSerializer.Serialize(_b, Json.Files)).Wiring.Any(w => w.Device == det.OnboardDevice), "detached survives the bench JSON (not re-added)");
                why = _graph.SetDetached(e, false);
                Check(why == null && !_b.Detached.Contains(det.OnboardDevice) && _b.Wiring.Contains(e) && _graph.ConnectedWires().Contains((det.OnboardDevice, Prim(det.OnboardDevice), det.Id)), $"put back on {det.MainText}: {why ?? "wired"}");
            }
            else Check(false, "no detachable onboard device to detach");

            // a soldered one cannot be removed
            var sold = _graph.Pads.FirstOrDefault(p => p.HasOnboard && !p.IsDetachable);
            if (sold != null && _graph.NodeOfDevice(sold.OnboardDevice, wiredOnly: true) is { } sn)
            {
                why = _graph.TryDisconnectPad(sn, 0, sold.Id);
                Check(why != null && why.Contains("soldered") && _graph.ConnectedWires().Contains((sold.OnboardDevice, Prim(sold.OnboardDevice), sold.Id)) && !_b.Detached.Contains(sold.OnboardDevice), $"soldered {sold.OnboardDevice} refused: {why}");
            }
            else Check(false, "no soldered onboard device");

            // a user device on an on-board pad: only once its detachable onboard device is detached
            var rxPad = _graph.Pads.FirstOrDefault(p => p.HasOnboard && p.IsDetachable && p.PortKind == PortKind.Uart && p.OnboardDevice != "gps");
            if (rxPad != null && m.Device("gps") != null && _graph.NodeOfDevice(rxPad.OnboardDevice, wiredOnly: true) is { } rn)
            {
                var gps = _graph.AddDevice("gps")!;
                why = _graph.TryConnectPad(gps, 0, rxPad.Id);
                Check(why != null && why.Contains("detach it first"), $"gps → {rxPad.MainText} while the onboard {rxPad.OnboardDevice} is there refused: {why}");
                var re = _graph.EntryOfNode(rn)!;
                _graph.SetDetached(re, true);
                why = _graph.TryConnectPad(gps, 0, rxPad.Id);
                Check(why == null && _graph.WireTargets().Contains(("gps", "onboard", rxPad.Id)), $"gps → {rxPad.MainText} after detaching {rxPad.OnboardDevice} accepted (on the on-board node): {why ?? "wired"}");
                _graph.Remove(gps);
                why = _graph.SetDetached(re, false);
                Check(why == null && !_b.Detached.Contains(rxPad.OnboardDevice), $"{rxPad.OnboardDevice} put back: {why ?? "ok"}");
            }

            // an old bench (gyro + ESC + tail servo, `slot`) read as the core reads an old-form session:
            // slot → pad, port → primary; the onboard receiver stays (no crsf elsewhere), nothing detached;
            // the servo pads it leaves free are assumed with the aircraft's default actuators
            var order = m.ServoOrderFor(_b.Aircraft);
            // expectations from the manifest (the old-form rule, BenchData.MigrateSlots): an onboard device the list
            // does not name on its pad is detached when detachable, except the receiver, which stays while its pad
            // is free and no receiver is listed on a UART that models one; soldered ones always stay
            var boardPads = m.PadsFor(_b.Board);
            var onboard = boardPads.Where(p => p.HasOnboard).ToList();
            bool IsRx(string dev) => m.Device(dev)?.Category == "receiver";
            List<string> Omitted(params string[] listed) =>
                onboard.Where(p => p.IsDetachable && !IsRx(p.OnboardDevice) && !listed.Contains(p.OnboardDevice)).Select(p => p.OnboardDevice).OrderBy(x => x, StringComparer.Ordinal).ToList();
            int AssumedFor(params string[] usedPads) =>
                (boardPads.Any(p => p.Id == BenchData.OldEscPad) && !usedPads.Contains(BenchData.OldEscPad) ? 1 : 0) + BenchData.OldServoPads.Count(sp => boardPads.Any(p => p.Id == sp) && !usedPads.Contains(sp));
            static string L(IEnumerable<string> x) => string.Join(",", x);
            var oldJson = "{\"name\":\"old\",\"board\":\"" + _b.Board + "\",\"aircraft\":\"rs7\",\"wiring\":[{\"device\":\"icm42688p\",\"slot\":\"gyro1\"},{\"device\":\"esc\",\"slot\":\"motor1\",\"actuator\":\"motor\",\"option\":\"bidir\"},{\"device\":\"servo\",\"slot\":\"servo4\",\"actuator\":\"tail\",\"reversed\":true}],\"wiringLayout\":{\"servo@servo4\":{\"x\":5,\"y\":7}}}";
            var old = BenchData.Parse(oldJson);
            var oe = old.Wiring.Where(w => !w.Assumed).Select(w => $"{w.Device}.{w.Port}@{w.Pad}").ToList();
            Check(oe.SequenceEqual(new[] { $"icm42688p.{Prim("icm42688p")}@gyro1", $"esc.{Prim("esc")}@motor1", $"servo.{Prim("servo")}@servo4" }) && old.Wiring.All(w => w.Slot == null), "old bench: slot → pad, port → primary: " + string.Join(", ", oe));
            var oldDet = Omitted("icm42688p");
            Check(old.Detached.OrderBy(x => x, StringComparer.Ordinal).SequenceEqual(oldDet), $"old bench: omitted detachable onboard devices detached (want [{L(oldDet)}]), the receiver stays (no crsf elsewhere) [detached: {L(old.Detached)}]");
            var oa = old.Wiring.Where(w => w.Assumed).Select(w => $"{w.Device}@{w.Pad}/{w.Actuator}").ToList();
            Check(oa.SequenceEqual(new[] { $"servo@servo1/{order[0]}", $"servo@servo2/{order[1]}", $"servo@servo3/{order[2]}" }), "old bench: free servo1..3 assumed with the default actuators: " + string.Join(", ", oa));
            var oldSes = JsonSerializer.Serialize(old.ToSession("/repo"));
            Check(!oldSes.Contains("\"slot\"") && oldSes.Contains("\"pad\":\"servo4\"") && oldSes.Contains("\"pad\":\"servo1\"") && oldDet.All(d => oldSes.Contains($"{{\"device\":\"{d}\",\"detached\":true}}")) && !oldSes.Contains("\"device\":\"crsf\",\"detached\""),
                  $"old bench → new-form session with the assumed servos, detached only [{L(oldDet)}]");
            Check(!JsonSerializer.Serialize(old, Json.Files).Contains("\"slot\""), "old bench re-serialised without `slot`");

            // an old list without the gyro, the receiver on PORT-A: gyro off (detached), the onboard receiver detached, ESC + servo1..4 assumed
            var old2 = BenchData.Parse("{\"name\":\"old2\",\"board\":\"" + _b.Board + "\",\"aircraft\":\"rs7\",\"wiring\":[{\"device\":\"crsf\",\"slot\":\"uart4\"}]}");
            var d2 = old2.Detached.OrderBy(x => x, StringComparer.Ordinal).ToList();
            var want2 = Omitted().Concat(onboard.Where(p => p.IsDetachable && p.OnboardDevice == "crsf").Select(p => p.OnboardDevice)).OrderBy(x => x, StringComparer.Ordinal).ToList();
            var a2 = old2.Wiring.Where(w => w.Assumed).Select(w => $"{w.Device}@{w.Pad}/{w.Actuator}{(w.Option != "" ? "/" + w.Option : "")}").ToList();
            Check(d2.SequenceEqual(want2) && a2.Count == AssumedFor() && a2[0].StartsWith("esc@motor1/"), $"old list without gyro, crsf on uart4: detached [{L(d2)}] (want [{L(want2)}]), assumed {a2.Count} (want {AssumedFor()}): {string.Join(", ", a2)}");

            // the user's smoke bench as the app wrote it before the wiring model (old form, gyro only), from a bench folder
            var keepDir = BenchData.Dir;
            BenchData.Dir = $"user://selftest-smoke-{Time.GetTicksUsec()}";
            var abs = ProjectSettings.GlobalizePath(BenchData.Dir);
            try
            {
                Directory.CreateDirectory(abs);
                File.WriteAllText(Path.Combine(abs, "smoke.json"), "{\"name\":\"smoke\",\"board\":\"" + _b.Board + "\",\"firmwarePath\":\"\",\"aircraft\":\"rs7\",\"wiring\":[{\"device\":\"icm42688p\",\"slot\":\"gyro1\",\"actuator\":\"\",\"reversed\":false,\"option\":\"\",\"props\":{}}],\"wiringLayout\":{},\"boardConfig\":\"\",\"coreHost\":\"192.168.1.10\",\"corePort\":31250,\"mspPort\":31235,\"layout\":{},\"camera\":\"orbit\"}");
                var sm = BenchData.Load(BenchData.PathFor("smoke"))!;
                Check(sm.Core == BenchData.CoreRemote && sm.IsRemote, $"smoke (no `core`, coreHost 192.168.1.10) → core {sm.Core} (connect only, the Status banner offers the local core)");
                var sa = sm.Wiring.Where(w => w.Assumed).Select(w => $"{w.Device}@{w.Pad}/{w.Actuator}{(w.Option != "" ? "/" + w.Option : "")}").ToList();
                // wires after the migration: the listed gyro + the onboard devices the rule keeps (soldered, the receiver) + the assumed ones
                var smDet = Omitted("icm42688p");
                int smAssumed = AssumedFor("gyro1");
                int smWires = 1 + onboard.Count(p => p.OnboardDevice != "icm42688p" && !smDet.Contains(p.OnboardDevice)) + smAssumed;
                smokeNumbers = (smWires, smAssumed, smDet);
                Check(sm.Detached.OrderBy(x => x, StringComparer.Ordinal).SequenceEqual(smDet) && sa.SequenceEqual(new[] { $"esc@motor1/{m.ThrottleFor("rs7")}/bidir" }.Concat(BenchData.OldServoPads.Select((sp, i) => $"servo@{sp}/{order[i]}"))),
                      $"smoke (old form, gyro only): detached [{L(sm.Detached)}] (want [{L(smDet)}]), assumed {string.Join(", ", sa)}");
                _b.Wiring = sm.Wiring; _b.Detached = sm.Detached; _b.WiringLayout = sm.WiringLayout;
                _graph.Rebuild();
                var sw2 = _graph.ConnectedWires();
                bool Has(string dev, string pad) => sw2.Any(x => x.Device == dev && x.Pad == pad);
                bool rxOnUart5 = onboard.Any(p => p.Id == BenchData.OldRxPad && IsRx(p.OnboardDevice));
                Check(sw2.Count == smWires && Has("icm42688p", "gyro1") && (!rxOnUart5 || Has("crsf", "uart5")) && Has("esc", "motor1") && BenchData.OldServoPads.All(sp => Has("servo", sp))
                      && smDet.All(d => !sw2.Any(x => x.Device == d)) && _graph.AssumedNodeCount == smAssumed,
                      $"smoke in the graph: gyro + receiver attached, [{L(smDet)}] off, ESC + 4 servos, {sw2.Count} wires (want {smWires}), {_graph.AssumedNodeCount} 'assumed' badges (want {smAssumed})");
                sm.Save();
                var text = File.ReadAllText(Path.Combine(abs, "smoke.json"));
                var re = BenchData.Parse(text);
                Check(!text.Contains("\"slot\"") && !text.Contains("\"assumed\"") && text.Contains("\"pad\": \"servo4\"") && re.Wiring.Count(w => w.IsWired) == smWires && re.Wiring.All(w => !w.Assumed) && re.Detached.OrderBy(x => x, StringComparer.Ordinal).SequenceEqual(smDet) && re.MigrateSlots(m) == 0,
                      $"smoke saved in the new form: {re.Wiring.Count(w => w.IsWired)} wired entries, no slot, no assumed");
                _graph.Rebuild();
                Check(_graph.AssumedNodeCount == 0 && _graph.ConnectedWires().Count == smWires, $"after save the assumed entries are ordinary (no badge), {_graph.ConnectedWires().Count} wires (want {smWires})");
            }
            finally { BenchData.Dir = keepDir; try { Directory.Delete(abs, true); } catch (Exception) { } }

            // the old-form rule per detachable onboard device (not the receiver): omitted from an old list → detached; listed on its pad → attached
            foreach (var p in onboard.Where(p => p.IsDetachable && !IsRx(p.OnboardDevice)))
            {
                var without = BenchData.Parse("{\"board\":\"" + _b.Board + "\",\"aircraft\":\"rs7\",\"wiring\":[{\"device\":\"esc\",\"slot\":\"motor1\"}]}");
                var with = BenchData.Parse("{\"board\":\"" + _b.Board + "\",\"aircraft\":\"rs7\",\"wiring\":[{\"device\":\"" + p.OnboardDevice + "\",\"slot\":\"" + p.Id + "\"}]}");
                Check(without.Detached.Contains(p.OnboardDevice) && !without.PresentDevices(m).Contains(p.OnboardDevice) && !with.Detached.Contains(p.OnboardDevice) && with.PresentDevices(m).Contains(p.OnboardDevice),
                      $"old-form rule: detachable onboard {p.OnboardDevice} on {p.MainText} omitted → detached [{L(without.Detached)}], listed → attached [{L(with.Detached)}]");
            }

            // the OSD device: in the catalogue (palette), its UART port both ways (listens and talks: TX+RX), refused on the RX-only TELEM, accepted on PORT-A
            if (m.Device("msp_osd") is { } osd)
            {
                var up = osd.Ports.FirstOrDefault(x => x.PortKind == PortKind.Uart);
                Check(WiringCatalog.Devices.Contains(osd) && osd.Category == "osd" && up is { Dir: "both" }, $"msp_osd in the catalogue: category {osd.Category}, ports {string.Join(",", osd.Ports.Select(x => $"{x.Id}:{x.Kind}/{x.Dir}"))}");
                var on = _graph.AddDevice("msp_osd");
                why = on == null ? "no node" : _graph.TryConnectPad(on, 0, "uart2");
                Check(why != null && why.Contains("TX"), $"msp_osd → TELEM (rx only) refused: {why}");
                why = on == null ? "no node" : _graph.TryConnectPad(on, 0, "uart4");
                Check(why == null && _graph.ConnectedWires().Contains(("msp_osd", up?.Id ?? "", "uart4")), $"msp_osd → PORT-A · uart4 accepted: {why ?? "wired"}");
                if (on != null) _graph.Remove(on);
            }

            // fixed numbers for the bundled fixture (app/tests/manifest-fixture.json). To refresh it: save the
            // current core's manifest over it (ssh <homelab> "cd ~/flightbench && core/qemu/run.sh --manifest"
            // > app/tests/manifest-fixture.json), run FB_SELFTEST=all, update these from the log lines above.
            if (SelfTests.OnFixture(m))
            {
                var fxDet = onboard.Where(p => p.IsDetachable).Select(p => p.OnboardDevice).OrderBy(x => x, StringComparer.Ordinal).ToList();
                var fxSol = onboard.Where(p => !p.IsDetachable).Select(p => p.OnboardDevice).ToList();
                Check(fxDet.SequenceEqual(new[] { "crsf", "dps310", "icm42688p" }) && fxSol.SequenceEqual(new[] { "spiflash" }), $"fixture: onboard detachable [{L(fxDet)}] (want crsf,dps310,icm42688p), soldered [{L(fxSol)}] (want spiflash)");
                Check(smokeNumbers is { Wires: 8, Assumed: 5 } && smokeNumbers.Detached.SequenceEqual(new[] { "dps310" }),
                      $"fixture: smoke = {smokeNumbers.Wires} wires (want 8: gyro, crsf, spiflash, ESC, 4 servos), {smokeNumbers.Assumed} assumed (want 5), detached [{L(smokeNumbers.Detached)}] (want dps310)");
                Check(m.Devices.Count == 9 && m.Device("msp_osd") is { Category: "osd", Implemented: true } o && o.Ports.Count == 1 && o.Primary is { Id: "uart", Kind: "uart", Dir: "both" } && o.Faults.Select(f => f.Id).SequenceEqual(new[] { "link_lost" }),
                      $"fixture: {m.Devices.Count} devices (want 9), msp_osd osd / uart both / link_lost");
            }

            // Faults window: rows only for devices present (wired, or onboard and not detached)
            var fp = new FaultsPanel();
            fp.Build(m, old.PresentDevices(m));
            Check(fp.IsEnabled("icm42688p.freeze") && fp.IsEnabled("esc.telemetry_lost") && fp.IsEnabled("crsf.link_lost"), $"faults (old bench): gyro/ESC/receiver rows enabled (present: {string.Join(",", old.PresentDevices(m))})");
            var detFaults = oldDet.SelectMany(d => m.Device(d)?.Faults.Select(f => $"{d}.{f.Id}") ?? Enumerable.Empty<string>()).ToList();
            var osdFaults = m.Device("msp_osd")?.Faults.Select(f => $"msp_osd.{f.Id}").ToList() ?? new List<string>();
            Check(detFaults.All(k => !fp.IsEnabled(k)) && osdFaults.All(k => !fp.IsEnabled(k)), $"faults (old bench): detached [{L(detFaults)}] and unwired msp_osd [{L(osdFaults)}] rows greyed");
            fp.Free();
            var fp2 = new FaultsPanel();
            fp2.Build(m, old2.PresentDevices(m));
            Check(!fp2.IsEnabled("icm42688p.freeze") && fp2.IsEnabled("crsf.link_lost"), $"faults (old list without gyro): gyro rows greyed 'not wired', receiver on uart4 enabled (present: {string.Join(",", old2.PresentDevices(m))})");
            fp2.Free();
        }

        // the summary page's Core row: local by default (host / port greyed, what local resolves to), remote as a click picks it, back
        {
            var keepMode = _b.Core; var keepHost = _b.CoreHost;
            PickCore(false);
            var st = CoreRowState();
            Check(st is { Local: true, Remote: false, HostEditable: false, PortEditable: false } && st.Resolves.StartsWith(LocalCoreText()) && !_b.IsRemote,
                  $"wizard Core row, local: host/port editable {st.HostEditable}/{st.PortEditable}, resolves '{st.Resolves}'");
            Check(CoreLocator.Find(out _) != null || st.Resolves.StartsWith("no core found — set it in Settings → Paths"), "no local core → the row says where to set it");
            PickCore(true);
            st = CoreRowState();
            Check(st is { Local: false, Remote: true, HostEditable: true, PortEditable: true } && _b.Core == BenchData.CoreRemote, $"wizard Core row, remote: bench core {_b.Core}, host/port editable {st.HostEditable}/{st.PortEditable}");
            _coreHost.Text = "192.168.1.10"; _coreHost.EmitSignal(LineEdit.SignalName.TextChanged, _coreHost.Text);
            var rt = BenchData.Parse(JsonSerializer.Serialize(_b, Json.Files));
            Check(rt.IsRemote && rt.CoreHost == "192.168.1.10", $"wizard Core row → bench JSON: core {rt.Core}, host {rt.CoreHost}");
            PickCore(false);
            Check(!_b.IsRemote && !CoreRowState().HostEditable && _b.CoreHost == "192.168.1.10", "back to local: host greyed and remembered");
            _b.Core = keepMode; _b.CoreHost = keepHost; _coreHost.Text = keepHost;
            if (_b.IsRemote) _coreRemote.ButtonPressed = true; else _coreLocal.ButtonPressed = true;
        }

        bool ok = fails.Count == 0;
        GD.Print($"[selftest] wiring round trip: {(ok ? "PASS" : "FAIL: " + string.Join("; ", fails))}");
        AutofillWiring();
        return ok;

        static string Describe(WiringEntry w) =>
            $"{w.Device}.{w.Port} -> {w.Pad}{string.Concat(w.Links.OrderBy(l => l.Key).Select(l => $" +{l.Key}->{l.Value}"))}{(w.Actuator != "" ? " (" + w.Actuator + ")" : "")}{(w.Reversed ? " reversed" : "")}{(w.Option != "" ? " [" + w.Option + "]" : "")}{string.Concat(w.Props.OrderBy(p => p.Key).Select(p => $" {p.Key}={p.Value}"))}";
    }

    private Control BuildConfigPage()
    {
        var v = new VBoxContainer();
        v.AddChild(new Label { Text = "Paste the real board's `diff all` (applied as custom defaults on every bench restart). Reversals of servo linkages belong to wiring, not here.", AutowrapMode = TextServer.AutowrapMode.Word });
        var row = new HBoxContainer();
        _loadProfile = Ui.Btn("Load profile", () =>
        {
            if (ProfileFile(_b.Aircraft) is { } p) _cfg.Text = File.ReadAllText(p);
        });
        var browse = Ui.Btn("Browse…", () => Ui.PickFile(this, new[] { "*.txt ; diff all" }, null, p => _cfg.Text = File.ReadAllText(p)));
        row.AddChild(_loadProfile); row.AddChild(browse);
        row.AddThemeConstantOverride("separation", 10);
        v.AddChild(row);
        _cfg = new TextEdit { Text = _b.BoardConfig, SizeFlagsVertical = SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(0, 160) };
        v.AddChild(_cfg);
        // files the core reads itself (session board_config / profile): relative = under the data root
        v.AddChild(new Label { Text = "Files the core applies before the text above (relative paths are under the data root, as the core reads them):", Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart });
        (_cfgFile, _cfgFileStatus) = PathRow(v, "Board config file", _b.BoardConfigFile, "(the board's own boards/<board>/*.config)", new[] { "*.config ; board config", "*.txt ; text" });
        (_profileFile, _profileStatus) = PathRow(v, "Profile file", _b.ProfileFile, "(none) e.g. models/rs7/qemu-profile.txt", new[] { "*.txt ; CLI profile" });
        return v;
    }

    private Control BuildSummaryPage()
    {
        var v = new VBoxContainer();
        var row = new HBoxContainer();
        row.AddChild(new Label { Text = "Bench name: " });
        _name = new LineEdit { Text = _b.Name == "new bench" ? "" : _b.Name, SizeFlagsHorizontal = SizeFlags.ExpandFill, PlaceholderText = "rs7-nexus" };
        row.AddChild(_name);
        v.AddChild(row);
        _summary = new Label { AutowrapMode = TextServer.AutowrapMode.Word };
        v.AddChild(_summary);
        v.AddChild(BuildTransmitterGroup());
        v.AddChild(BuildScriptGroup());
        v.AddChild(BuildCoreGroup());
        v.AddChild(BuildTimeGroup());
        return v;
    }

    // ---- Transmitter (the bench's transmitter model, TxModel.cs; decisions 2026-10-23) ----

    /// A new bench (not an edit) gets its board's default model (TxModel.DefaultFor: RotorFlight → ch6 switch2 "throttle",
    /// others all auto) while its model is still a default one (none, or the RotorFlight default); a model the user set,
    /// or any edited bench, is left alone.
    public static void ApplyBoardDefault(BenchData b, bool editing)
    {
        if (editing) return;
        static string K(Dictionary<string, TxChannel>? m) => m == null ? "" : string.Join(";", m.Select(kv => $"{kv.Key}={kv.Value.Type}/{kv.Value.Label}/{kv.Value.Start}"));
        var rf = new Dictionary<string, TxChannel> { ["5"] = new() { Type = TxModel.Auto }, ["6"] = new() { Type = TxModel.Switch2, Label = "throttle" } };
        if (b.Transmitter == null || K(b.Transmitter) == K(rf)) b.Transmitter = TxModel.DefaultFor(b.Board);
    }

    private readonly List<(int Ch, OptionButton Type, LineEdit Label, SpinBox Start, Control Row)> _txRows = new();
    private bool _txFilling, _txMore;
    private Button? _txMoreBtn;

    /// Edit bench → Transmitter: channels 5..12 (13..16 on request) with a type (auto / switch 2 / switch 3 / switch 6
    /// (ArduPilot)), a label and the start position of a static switch; edits _b.Transmitter directly.
    private Control BuildTransmitterGroup()
    {
        var panel = new PanelContainer { Name = "transmitter" };
        panel.AddThemeStyleboxOverride("panel", Ui.Box(new Color(0.02f, 0.06f, 0.14f), new Color(0.30f, 0.42f, 0.62f, 0.6f), 4, 12, 10));
        var v = new VBoxContainer();
        v.AddThemeConstantOverride("separation", 4);
        panel.AddChild(v);
        v.AddChild(new Label { Text = "Transmitter (the radio's switches on this bench)" });
        v.AddChild(new Label { Text = "auto = a switch from the firmware's modes (a row once the firmware gives ranges on the channel); switch 2 / 3 / 6 = a switch of the radio the firmware may not name (switch 6 = ArduPilot's flight-mode channel). The flight screen's Transmitter window shows a row per channel that has a switch; keys 5..9 step them in channel order.", Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart });
        var grid = new GridContainer { Columns = 4 };
        grid.AddThemeConstantOverride("h_separation", 10); grid.AddThemeConstantOverride("v_separation", 2);
        foreach (var h in new[] { "channel", "type", "label", "start position" }) grid.AddChild(new Label { Text = h, Modulate = Ui.Soft });
        v.AddChild(grid);
        var more = new Button { Text = "More channels (13–16)", FocusMode = Control.FocusModeEnum.None };
        for (int ch = TxModel.FirstChannel; ch <= TxModel.LastChannel; ch++)
        {
            int c = ch;
            var name = new Label { Text = $"ch{c}", CustomMinimumSize = new Vector2(60, 0) };
            var type = new OptionButton { CustomMinimumSize = new Vector2(190, 0) };
            foreach (var t in TxModel.TypeNames) type.AddItem(t);
            var label = new LineEdit { PlaceholderText = c == 6 ? "e.g. throttle" : "", CustomMinimumSize = new Vector2(180, 0) };
            var start = new SpinBox { MinValue = 0, MaxValue = 5, Step = 1, CustomMinimumSize = new Vector2(110, 0), TooltipText = "the position the switch starts at when the bench opens (0 = lowest); never an ARM position nobody asked for" };
            type.ItemSelected += i => SetTx(c, TxModel.Types[(int)i], label.Text, (int)start.Value);
            label.TextChanged += t => SetTx(c, TxModel.Types[type.Selected], t, (int)start.Value);
            start.ValueChanged += x => SetTx(c, TxModel.Types[type.Selected], label.Text, (int)x);
            foreach (var w in new Control[] { name, type, label, start }) grid.AddChild(w);
            _txRows.Add((c, type, label, start, name));
        }
        _txMoreBtn = more;
        more.Pressed += () => { _txMore = true; RefreshTxRows(); };
        v.AddChild(more);
        RefreshTxRows();
        return panel;
    }

    private void SetTxVisible((int Ch, OptionButton Type, LineEdit Label, SpinBox Start, Control Row) r, bool on)
    {
        r.Row.Visible = on; r.Type.Visible = on; r.Label.Visible = on; r.Start.Visible = on;
    }

    /// One channel edited: an auto channel without a label is dropped from the block (absent = auto).
    private void SetTx(int ch, string type, string label, int start)
    {
        if (_txFilling) return;
        var m = TxModel.Effective(_b.Transmitter, _b.Board) ?? new Dictionary<string, TxChannel>();   // an edit starts from what the bench is driven with
        var lbl = string.IsNullOrWhiteSpace(label) ? null : label.Trim();
        int n = TxModel.Values(type).Length;
        if (type == TxModel.Auto && lbl == null) m.Remove(ch.ToString());
        else m[ch.ToString()] = new TxChannel { Type = type, Label = lbl, Start = n == 0 ? 0 : Math.Clamp(start, 0, n - 1) };
        _b.Transmitter = TxModel.Normalize(m);
        RefreshTxRows();
    }

    /// The editor rows from _b.Transmitter (the start spin box only for static types, up to the type's last position).
    private void RefreshTxRows()
    {
        if (_txRows.Count == 0) return;
        _txFilling = true;
        bool anyHigh = false;
        foreach (var r in _txRows)
        {
            var e = TxModel.Entry(TxModel.Effective(_b.Transmitter, _b.Board), r.Ch);
            var type = e?.Type ?? TxModel.Auto;
            r.Type.Selected = Math.Max(0, Array.IndexOf(TxModel.Types, type));
            if (!r.Label.HasFocus() && r.Label.Text != (e?.Label ?? "")) r.Label.Text = e?.Label ?? "";
            int n = TxModel.Values(type).Length;
            r.Start.Editable = n > 0;
            r.Start.MaxValue = Math.Max(0, n - 1);
            r.Start.SetValueNoSignal(e?.Start ?? 0);
            r.Start.Modulate = n > 0 ? Colors.White : new Color(1, 1, 1, 0.35f);
            if (r.Ch > 12 && e != null) anyHigh = true;
        }
        foreach (var r in _txRows) if (r.Ch > 12) SetTxVisible(r, _txMore || anyHigh);
        if (_txMoreBtn != null) _txMoreBtn.Visible = !(_txMore || anyHigh);
        _txFilling = false;
    }

    private CheckBox _coreLocal = null!, _coreRemote = null!;
    private LineEdit _coreHost = null!;
    private SpinBox _corePort = null!;
    private Label _coreResolves = null!, _coreWarn = null!;

    /// Core (bench `core`): Local (bundled) = the app starts the core on the bench's ports; Remote host:port =
    /// connect only. Edits _b directly; the host / port fields are live only for remote.
    private Control BuildCoreGroup()
    {
        var panel = new PanelContainer();
        panel.AddThemeStyleboxOverride("panel", Ui.Box(new Color(0.02f, 0.06f, 0.14f), new Color(0.30f, 0.42f, 0.62f, 0.6f), 4, 12, 10));
        var v = new VBoxContainer();
        v.AddThemeConstantOverride("separation", 4);
        panel.AddChild(v);
        v.AddChild(new Label { Text = "Core" });
        var group = new ButtonGroup();
        _coreLocal = new CheckBox { Text = "Local (bundled)", ButtonGroup = group, ButtonPressed = !_b.IsRemote, TooltipText = "the app starts the core on this machine on the bench's ports and stops it when the bench closes" };
        _coreRemote = new CheckBox { Text = "Remote host:port", ButtonGroup = group, ButtonPressed = _b.IsRemote, TooltipText = "connect only to a core someone else runs (e.g. the homelab over tailnet); the app starts nothing" };
        _coreHost = new LineEdit { Text = _b.CoreHost, PlaceholderText = "192.168.1.10", SizeFlagsHorizontal = SizeFlags.ExpandFill };
        _corePort = new SpinBox { MinValue = 1, MaxValue = 65535, Step = 1, Value = _b.CorePort, CustomMinimumSize = new Vector2(120, 0) };
        _coreHost.TextChanged += t => { _b.CoreHost = t.Trim(); UpdateCoreRow(); };
        _corePort.ValueChanged += x => _b.CorePort = (int)x;
        _coreLocal.Toggled += on => { if (on) { _b.Core = BenchData.CoreLocal; UpdateCoreRow(); } };
        _coreRemote.Toggled += on => { if (on) { _b.Core = BenchData.CoreRemote; UpdateCoreRow(); } };
        v.AddChild(_coreLocal);
        _coreResolves = new Label { Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart };
        var m = new MarginContainer();
        m.AddThemeConstantOverride("margin_left", 28);
        m.AddChild(_coreResolves);
        v.AddChild(m);
        var row = new HBoxContainer();
        row.AddChild(_coreRemote); row.AddChild(_coreHost); row.AddChild(new Label { Text = ":" }); row.AddChild(_corePort);
        row.AddThemeConstantOverride("separation", 8);
        v.AddChild(row);
        _coreWarn = new Label { Modulate = new Color(1f, 0.6f, 0.45f), Visible = false };
        v.AddChild(_coreWarn);
        UpdateCoreRow();
        return panel;
    }

    /// What the local core resolves to (CoreLocator.Find), for the Core row.
    public static string LocalCoreText() =>
        CoreLocator.Find(out var src) is { } exe ? $"{exe} ({src})" : "no core found — set it in Settings → Paths";

    private void UpdateCoreRow()
    {
        bool remote = _b.IsRemote;
        _coreHost.Editable = remote; _corePort.Editable = remote;
        _coreResolves.Text = LocalCoreText() + (remote ? "" : $" · ports: control {_b.CorePort}, msp {_b.MspPort}");
        _coreWarn.Visible = remote && string.IsNullOrWhiteSpace(_b.CoreHost);
        _coreWarn.Text = "a remote core needs a host";
    }

    /// Self-tests: the Core row as shown (local picked, host / port editable, what local resolves to).
    public (bool Local, bool Remote, bool HostEditable, bool PortEditable, string Resolves) CoreRowState() =>
        (_coreLocal.ButtonPressed, _coreRemote.ButtonPressed, _coreHost.Editable, _corePort.Editable, _coreResolves.Text);

    /// Self-tests: pick a mode as a click would.
    public void PickCore(bool remote) { if (remote) _coreRemote.ButtonPressed = true; else _coreLocal.ButtonPressed = true; }

    /// The first CPU speed option = shift 1; index i = shift i + 1.
    public static readonly string[] ShiftOptions =
    {
        "shift 1 · 500 MIPS · twice a real F722 (slowest emulation, lowest firmware load)",
        "shift 2 · 250 MIPS · ≈ real F722 (honest default)",
        "shift 3 · 125 MIPS · half a real F722 (fastest emulation, firmware sees double load)",
    };

    /// Core time (session `time` block): edits _b.Time directly; takes effect when the core starts from the session.
    private Control BuildTimeGroup()
    {
        var panel = new PanelContainer();
        panel.AddThemeStyleboxOverride("panel", Ui.Box(new Color(0.02f, 0.06f, 0.14f), new Color(0.30f, 0.42f, 0.62f, 0.6f), 4, 12, 10));
        var v = new VBoxContainer();
        v.AddThemeConstantOverride("separation", 4);
        panel.AddChild(v);
        v.AddChild(new Label { Text = "Core time (applies when the core is started from this bench's session)" });
        var speed = new OptionButton { SizeFlagsHorizontal = SizeFlags.ExpandFill };
        foreach (var o in ShiftOptions) speed.AddItem(o);
        speed.Selected = _b.Time.IcountShift - CoreTime.MinShift;
        speed.ItemSelected += i => _b.Time.IcountShift = (int)i + CoreTime.MinShift;
        var row = new HBoxContainer();
        row.AddChild(new Label { Text = "Virtual CPU speed" });
        row.AddChild(speed);
        row.AddThemeConstantOverride("separation", 10);
        v.AddChild(row);
        var idle = new CheckBox { Text = "Skip the firmware's idle loop (idle skip)", ButtonPressed = _b.Time.IdleSkip };
        idle.Toggled += on => _b.Time.IdleSkip = on;
        v.AddChild(idle);
        v.AddChild(Hint("same flight, ~2x faster; off = every instruction executed"));
        var rt = new CheckBox { Text = "Real-time (never faster than wall clock)", ButtonPressed = _b.Time.Realtime };
        rt.Toggled += on => _b.Time.Realtime = on;
        v.AddChild(rt);
        v.AddChild(Hint("on by default, for flying by hand; off = as fast as the host can run it"));
        return panel;
    }

    private static Control Hint(string text)
    {
        var m = new MarginContainer();
        m.AddThemeConstantOverride("margin_left", 28);   // under the checkbox text
        m.AddChild(new Label { Text = text, Modulate = Ui.Soft });
        return m;
    }

    /// A label + path field + Browse (a picked file under the data root becomes relative) + its status line.
    private (LineEdit, Label) PathRow(VBoxContainer v, string label, string value, string placeholder, string[] filters)
    {
        var le = new LineEdit { Text = value, PlaceholderText = placeholder, SizeFlagsHorizontal = SizeFlags.ExpandFill };
        var browse = Ui.Btn("Browse…", () => Ui.PickFile(this, filters, null, p => le.Text = BenchData.RelativeTo(p, CoreLocator.DataRoot)));
        var row = new HBoxContainer();
        row.AddThemeConstantOverride("separation", 10);
        row.AddChild(new Label { Text = label, CustomMinimumSize = new Vector2(150, 0) }); row.AddChild(le); row.AddChild(browse);
        v.AddChild(row);
        var st = new Label { AutowrapMode = TextServer.AutowrapMode.WordSmart };
        v.AddChild(st);
        le.TextChanged += _ => UpdatePathStatus();
        return (le, st);
    }

    /// "→ <resolved> · found" / "· MISSING" for a bench path, resolved against the data root (as the core does),
    /// never against the app's working directory. "" → `empty`.
    public static (string Text, bool Ok) PathState(string path, string empty)
    {
        if (string.IsNullOrWhiteSpace(path)) return (empty, true);
        var root = CoreLocator.DataRoot;
        var r = BenchData.ResolvePath(path, root);
        bool ok = File.Exists(r);
        var where = BenchData.IsRootRelative(path) ? $" (relative to the data root {root.Replace('\\', '/')})" : "";
        return ($"→ {r} · {(ok ? "found" : "MISSING")}{(ok ? "" : where)}", ok);
    }

    private void UpdatePathStatus()
    {
        void Set(Label? l, string path, string empty)
        {
            if (l == null) return;
            var (t, ok) = PathState(path, empty);
            l.Text = t;
            l.Modulate = ok ? Ui.Soft : new Color(1f, 0.6f, 0.45f);
        }
        Set(_fwStatus, _fw?.Text ?? "", "no firmware yet (a local core needs one to boot)");
        Set(_cfgFileStatus, _cfgFile?.Text ?? "", BoardInfo.ConfigPath(_b.Board) is { } c ? $"→ {c.Replace('\\', '/')} (the board's own)" : "the board has no config file");
        Set(_profileStatus, _profileFile?.Text ?? "", "no profile file");
    }

    private void FillSummary()
    {
        string Fp(string p, string empty) => string.IsNullOrWhiteSpace(p) ? empty : $"{p}  {PathState(p, empty).Text}";
        _summary.Text = $"Board: {_b.Board}\nFirmware: {Fp(_b.FirmwarePath, "(none yet)")}\nAircraft: {_b.Aircraft}\n" +
                        $"Wiring: {string.Join(", ", _b.Wiring.SelectMany(w => w.Wires().Select(x => $"{w.Device}{(x.Port != "" ? "." + x.Port : "")}→{PadText(x.Pad)}{(x.Pad == w.Pad && w.Actuator != "" ? "/" + w.Actuator : "")}")))}\n" +
                        (_b.Detached.Count > 0 ? $"Detached onboard devices: {string.Join(", ", _b.Detached)}\n" : "") +
                        (_b.BoardConfigFile != "" ? $"Board config file: {Fp(_b.BoardConfigFile, "")}\n" : "") +
                        (_b.ProfileFile != "" ? $"Profile file: {Fp(_b.ProfileFile, "")}\n" : "") +
                        $"Board config (step 4): {BoardConfigLines(_b.BoardConfig)}\n" +
                        $"Bench lines (layer 4): {BenchLinesText()}{(_b.DisabledLines is { Count: > 0 } dl ? $" · {dl.Count} line(s) disabled" : "")}{(_b.AircraftFile != null ? $" · aircraft: this bench's copy {_b.AircraftFile}" : "")}\n" +
                        $"Saved to: {BenchData.Dir}/{(BenchData.Dir.StartsWith("user://") ? "  (Godot user dir)" : "")}" +
                        (_editing == null ? "" : $"\nEditing '{_editing}': the same name saves over it, another name makes a new bench (the original stays)");
    }

    /// "none" for an empty board config, else "N lines" (non-blank).
    public static string BoardConfigLines(string text)
    {
        int n = (text ?? "").Split('\n').Count(l => l.Trim() != "");
        return n == 0 ? "none" : n == 1 ? "1 line" : $"{n} lines";
    }

    private string BenchLinesText()
    {
        int n = BenchFiles.ReadBenchLines(_b).Split('\n').Count(l => LayeredConfig.Clean(l) != null);
        return n == 0 ? "none" : $"{n} line{(n == 1 ? "" : "s")} ({BenchFiles.CliPath(_b)})";
    }

    // ---- helpers ----
    public static readonly string[] FirmwareFilters = { "*.elf ; ELF", "*.hex ; Intel HEX", "*.bin ; raw binary" };

    private Button _loadProfile = null!;

    /// The aircraft's CLI profile in the repo: models/<a>/**/<a>-profile.txt, else the first *profile*.txt there.
    private static string? ProfileFile(string aircraft)
    {
        try
        {
            var dir = Repo.Path("models", aircraft);
            if (!Directory.Exists(dir)) return null;
            var all = Directory.GetFiles(dir, "*profile*.txt", SearchOption.AllDirectories).OrderBy(f => f, StringComparer.Ordinal).ToList();
            return all.FirstOrDefault(f => Path.GetFileName(f) == $"{aircraft}-profile.txt") ?? all.FirstOrDefault();
        }
        catch (Exception) { return null; }
    }

    private void UpdateProfileButton()
    {
        var p = ProfileFile(_b.Aircraft);
        var rel = p == null ? null : Path.GetRelativePath(AppState.RepoRoot, p).Replace('\\', '/');
        _loadProfile.Text = rel != null ? $"Load profile from {rel}" : $"no profile in models/{_b.Aircraft}/";
        _loadProfile.Disabled = p == null;
    }
}
