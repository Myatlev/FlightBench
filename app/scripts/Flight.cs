// Flight screen: 3D world + movable/resizable embedded windows (FlightWindows: layout saved into the
// bench), menu bar, bottom time bar, keys (dispatched through the Keybinds table), Faults window from the
// core's manifest. Talks to the core through CoreSession (docs/protocol.md). Buttons never take the
// keyboard focus (Space would re-press them); text fields do, and keep the sticks' keys while they have it.
// Bench menu: Edit bench… (the wizard in edit mode), Save as… (a copy under a new name, flown on at once),
// Save (what leaving the screen does), the Core group (a read-only "Core: running · pid N · control/msp ports" line,
// Restart core, Bench check, Core address…, Use local core when offered), Close bench, Functions / Config.
// Top bar: functional buttons left (Menu, Bench, Windows, View, Settings — the settings page over the bench —, Copy
// configurator address), the bench's name in the centre (not clickable), Disconnect right.
// Windows, one fact in one place (product decision 2026-10-01; docs/ui.md): Status = link / firmware / sensors /
// config progress (StatusPanel.cs), Transmitter = all input (sticks, channels, the radio's switches: the bench's
// transmitter model + the firmware's modes, the keyboard hints), Aircraft = the model's numbers + the crash banner
// (AircraftPanel.cs). Switches from the firmware's modes, the Functions and Config windows: Flight.Modes.cs, Flight.Config.cs.
// Dev hooks (FB_KEYS_SELFTEST, FB_FAULT_SCRIPT) are in Flight.Dev.cs.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public partial class Flight : Node3D
{
    private CoreSession _session = null!;
    private CoreClient _core => _session.Client;
    private readonly RcInput _rc = new(linkLoss: true);   // an unplugged radio = the receiver's link lost (Settings → Transmitter)
    private BenchData _bench = null!;
    private Aircraft? _aircraft, _modelAircraft;
    private Camera3D? _camera;
    private float _yaw = 0.6f, _pitch = 0.35f, _dist = 8f;
    // cameras: Pad (pilot on the ground), Orbit (mouse), FPV (nose), Chase (behind, follows heading)
    public enum CamMode { Pad, Orbit, Fpv, Chase }
    private static readonly string[] CamNames = { "pad", "orbit", "fpv", "chase" };
    private CamMode _cam = CamMode.Orbit;          // FB_CAMERA sets it for this run; the bench's Camera changes only by hand
    private bool _axes;                            // body axes shown (bench BodyAxes, or FB_AXES for this run)
    private PopupMenu? _viewMenu;
    // OSD: one buffer fed by `osd` messages, drawn by the OSD window's panel and by the FPV overlay (OsdPanel.cs)
    private readonly OsdBuffer _osd = new();
    private OsdPanel? _osdOverlayPanel;
    private OsdPanel? _osdWindowPanel;             // the OSD window's panel (self-tests)
    private bool _osdOverlay;                      // View → OSD overlay (bench OsdOverlay, or FB_OSD_OVERLAY for this run)
    private Vector3 _chasePos; private float _chaseYaw; private bool _chaseInit;
    private Quaternion _fpvQuat = Quaternion.Identity;
    private Label? _modelInfo;
    private static readonly Vector3 PadEye = new(0, 1.7f, 10f);   // pilot 10 m behind the pad (south), eyes at 1.7 m
    private Vector3 _target = Vector3.Zero;
    // from the state stream (JsonElements do not outlive the message: the values are copied here)
    private double _lastT;
    private int _stateCount;
    private bool _paused;
    private bool? _lastPower;                      // fc.power as last reported (the Battery toggle follows its changes)
    private double _roll, _pitchDeg, _heading; private bool _hasEuler;
    private FlightWindows _windows = null!;
    // window contents
    private Label? _timeLabel;
    private StatusPanel? _statusPanel;
    private readonly FcStatus _fc = new();
    private AircraftReadout _readout = null!;
    private AircraftPanel? _aircraftPanel;
    private Button? _cfgBtn;
    private double _sps;                           // states per second (host time), for the Status link line
    private int _spsCount; private ulong _spsAt;
    private ulong _cfgHideAt;                      // host ms: an `applied` / `restored` config line hides then
    private readonly ChannelBar[] _rcBars = new ChannelBar[16];
    private int _rcSends;
    private RichTextLabel? _log;
    private int _logLines;
    private const int LogMaxLines = 2000;
    private CliLine? _cli;
    private LineEdit? _fwPath;
    private StickView? _stickL, _stickR;
    private Button? _connectBtn;
    private SignalsPanel? _signals;
    private FaultsPanel? _faults;
    private WiringGraph? _wiringView;
    private CheckButton? _battery, _watch, _realtime;

    public override void _Ready()
    {
        _bench = AppState.Bench ?? BenchData.New();
        _cam = ParseCam(_bench.Camera) ?? CamMode.Orbit;
        if (ParseCam(DevEnv.Camera) is { } envCam) _cam = envCam;           // screenshots: this run only
        _axes = _bench.BodyAxes || DevEnv.Axes;
        _osdOverlay = _bench.OsdOverlay || DevEnv.OsdOverlay;
        if (DevEnv.Orbit is { } orbit)   // screenshots: "dist[,yaw_deg[,pitch_deg]]"
        {
            var f = orbit.Split(',');
            float Num(int i, float def) => i < f.Length && float.TryParse(f[i], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var v) ? v : def;
            _dist = Mathf.Clamp(Num(0, _dist), 0.5f, 200f); _yaw = Mathf.DegToRad(Num(1, Mathf.RadToDeg(_yaw))); _pitch = Mathf.Clamp(Mathf.DegToRad(Num(2, Mathf.RadToDeg(_pitch))), -1.4f, 1.4f);
        }
        // FB_CORE is for this run only: the session holds it, the bench keeps its own address
        var addr = CoreSession.ParseAddress(DevEnv.Core) ?? (_bench.ConnectHost, _bench.CorePort);
        _session = new CoreSession(addr.Host, addr.Port);
        _isPlane = AircraftInfo.IsPlane(_bench.Aircraft);
        _readout = new AircraftReadout(_plane);
        BuildWorld();
        BuildMenu();
        BuildWindows();
        foreach (var note in AppState.Notes) Log("app", note);             // e.g. from opening a preset (main menu)
        AppState.Notes.Clear();
        Ui.NoFocusButtons(this);
        Ui.ReleaseTextOnUnedit(this);                                         // Escape / Enter in a field gives the keys back
        _core.ConnectionChanged += s =>
        {
            Log("app", s);
            if (s == "connected") { _signals?.Restart("new connection"); _osd.Reset(); _fcMsp = null; _plane.NewConnection(); _fc.Clear(); _readout.Clear(); _sps = 0; }
            UpdateConnectButton(); UpdateStatus();
        };
        _session.Up += () =>
        {
            _rc.MarkDirty();                                                   // the new core gets the current sticks
            if (_rc.RadioLost) RadioLinkFault(true, "still unplugged on the new connection");
            else if (_radioFaultOn) RadioLinkFault(false, "back while disconnected");
            // a core the app started from this bench runs with the bench's time block; any other with its own defaults
            _paused = false; _realtime?.SetPressedNoSignal(_ownCore && DevEnv.BenchFile == null && _bench.Time.Realtime);
        };
        HookModes();                                                          // modes / diff / function / osd (Flight.Modes.cs)
        _core.HelloReceived += h =>
        {
            Log("core", "hello: " + h.GetRawText()); _signals?.Restart("hello");
            _helloFirmware = h.TryGetProperty("firmware", out var fw) && fw.ValueKind == JsonValueKind.String ? fw.GetString() : null;
            _fc.ApplyHello(h);
            UpdateCfgHint(); UpdateStatus();
        };
        _core.LogReceived += (src, text) =>
        {
            Log(src, text);
            // the firmware answered after this client connected: its name (hello.firmware said the file name then)
            if (src == "core" && ConfiguratorAddress.FromLog(text) is { } says && says != _helloFirmware)
            {
                _helloFirmware = says;
                UpdateCfgHint();
                Log("app", $"configurator address for the {ConfiguratorAddress.Name(CfgFirmware)}: {CfgAddress} (the firmware answered)");
            }
        };
        _core.EventReceived += OnCoreEvent;                  // formatted + logged, plane / config / crash into the Status rows (Flight.Plane.cs)
        _core.StateReceived += OnState;
        _core.SignalsReceived += m => _signals?.OnSignals(m);
        _core.ManifestReceived += Manifest.SetLive;          // → Manifest.Changed → OnManifestChanged (cached for the wizard)
        Manifest.Changed += OnManifestChanged;
        _core.SamplesReceived += m => _signals?.OnSamples(m);
        _core.OsdReceived += _osd.Apply;
        GD.Print($"FlightBench flight screen: bench '{_bench.Name}' ({_bench.Board}/{_bench.Aircraft}); manifest {Manifest.Current.Describe()}; FB_*: {DevEnv.Describe()}");
        DecideCore();                                        // a local bench: the app starts the core (Flight.Core.cs)
        if (!_ownCore) CheckLayers("the bench as it is (a session-check on this machine; the app does not start this core)");   // Config window (Flight.Config.cs)
        StartCore();
    }

    // ---- world ----
    private void BuildWorld()
    {
        AddChild(new WorldEnvironment { Environment = new Godot.Environment { BackgroundMode = Godot.Environment.BGMode.Color, BackgroundColor = new Color(0.07f, 0.16f, 0.32f), AmbientLightSource = Godot.Environment.AmbientSource.Color, AmbientLightColor = new Color(0.6f, 0.65f, 0.75f) } });
        AddChild(new DirectionalLight3D { RotationDegrees = new Vector3(-50, 30, 0), LightEnergy = 1.2f });
        // a plane flies hundreds of metres from the pad (the trainer: ~13 m/s): a larger floor and a farther grid fade
        var grid = new ShaderMaterial { Shader = GD.Load<Shader>("res://shaders/grid.gdshader") };
        if (_isPlane) grid.SetShaderParameter("fade_distance", 500.0f);
        var floor = new MeshInstance3D { Mesh = new PlaneMesh { Size = _isPlane ? new Vector2(3000, 3000) : new Vector2(400, 400) }, Name = "floor", MaterialOverride = grid };
        AddChild(floor);
        for (int i = 0; i < 4; i++)
            AddChild(new MeshInstance3D { Mesh = new BoxMesh { Size = Vector3.One }, Position = new Vector3(6 + i * 3, 0.5f, -6), MaterialOverride = new StandardMaterial3D { AlbedoColor = new Color(0.85f, 0.85f, 0.9f) } });
        AddChild(new MeshInstance3D { Mesh = new CylinderMesh { TopRadius = 1.5f, BottomRadius = 1.5f, Height = 0.02f }, Position = new Vector3(0, 0.01f, 0), MaterialOverride = new StandardMaterial3D { AlbedoColor = new Color(0.95f, 0.95f, 1.0f, 0.5f), Transparency = BaseMaterial3D.TransparencyEnum.Alpha } });
        // world axes: N (red, -z), E (green, +x), Up (blue)
        AddAxis(new Vector3(0, 0, -1), new Color(1, 0.3f, 0.3f)); AddAxis(new Vector3(1, 0, 0), new Color(0.3f, 1, 0.3f)); AddAxis(new Vector3(0, 1, 0), new Color(0.4f, 0.6f, 1));
        _aircraft = new Aircraft { Name = "aircraft", AircraftName = _bench.Aircraft, VisualPath = VisualPath() };
        AddChild(_aircraft);
        _aircraft.Position = new Vector3(0, _aircraft.CgHeight, 0);
        _target = _aircraft.Position;
        _aircraft.ShowAxes(_axes);
        _camera = new Camera3D { Current = true, Fov = 60 };
        AddChild(_camera);
        UpdateCamera(0);
    }

    /// The bench's `visual` resolved under the data root (null = the models/<aircraft>/visual/ convention).
    private string? VisualPath() => _bench.Visual is { } v ? BenchData.ResolvePath(v, CoreLocator.DataRoot) : null;

    private void AddAxis(Vector3 dir, Color c)
    {
        // a bar along `dir`: the box mesh is long on its local -Z, so orient -Z along dir
        var m = new MeshInstance3D { Mesh = new BoxMesh { Size = new Vector3(0.05f, 0.05f, 2f) }, MaterialOverride = new StandardMaterial3D { AlbedoColor = c, ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded } };
        var origin = new Vector3(-8, 0.05f, 8) + dir * 1f;
        var up = dir == Vector3.Up ? Vector3.Right : Vector3.Up;
        m.Transform = new Transform3D(Basis.LookingAt(dir, up), origin);
        AddChild(m);
    }

    // ---- menu + windows ----
    private CanvasLayer _ui = null!;
    private void BuildMenu()
    {
        _ui = new CanvasLayer();
        AddChild(_ui);
        // OSD over the FPV camera, like goggles: full width between the top and bottom bars, under them
        _osdOverlayPanel = new OsdPanel { Buffer = _osd, AnchorRight = 1, AnchorBottom = 1, OffsetTop = 34, OffsetBottom = -34, Visible = false };
        _ui.AddChild(_osdOverlayPanel);
        var bar = new PanelContainer { AnchorRight = 1, OffsetBottom = 34, Theme = Ui.BaseTheme };
        _ui.AddChild(bar);
        var h = new HBoxContainer();
        bar.AddChild(h);
        _topBar = h;
        var menuBtn = new Button { Text = "< Menu", Name = "menu" };
        menuBtn.Pressed += () => GetTree().ChangeSceneToFile("res://scenes/MainMenu.tscn");   // _ExitTree saves the layout and disconnects
        h.AddChild(menuBtn);
        var benchMenu = new MenuButton { Text = "Bench", Name = "bench" };
        benchMenu.SetDisableShortcuts(true);
        _benchPopup = benchMenu.GetPopup();
        _benchPopup.AboutToPopup += RebuildBenchMenu;          // the Core line is current every time the menu opens
        _benchPopup.IdPressed += OnBenchMenu;
        RebuildBenchMenu();
        h.AddChild(benchMenu);
        // menus show the binds as accelerators, but the menus' own shortcuts are off: every key goes
        // through the one Keybinds table in _UnhandledInput
        var win = new MenuButton { Text = "Windows", Name = "windows" };
        win.SetDisableShortcuts(true);
        var names = Keybinds.Windows;
        for (int i = 0; i < names.Length; i++) { win.GetPopup().AddItem(names[i]); win.GetPopup().SetItemAccelerator(i, Keybinds.AsKey(Keybinds.Bind(Keybinds.WindowId(names[i])))); }
        win.GetPopup().IndexPressed += i => _windows.Toggle(names[i]);
        h.AddChild(win);
        var view = new MenuButton { Text = "View", Name = "view" };
        view.SetDisableShortcuts(true);
        _viewMenu = view.GetPopup();
        string[] camLabels = { "Pad camera", "Orbit camera", "FPV camera", "Chase camera" };
        for (int i = 0; i < 4; i++) { _viewMenu.AddRadioCheckItem(camLabels[i], i); _viewMenu.SetItemAccelerator(i, Keybinds.AsKey(Keybinds.Bind(CamFunctions[i]))); }
        _viewMenu.AddSeparator();
        _viewMenu.AddCheckItem("Body axes + actuator labels", 10); _viewMenu.SetItemAccelerator(_viewMenu.GetItemIndex(10), Keybinds.AsKey(Keybinds.Bind("body_axes")));
        _viewMenu.AddCheckItem("OSD overlay (FPV camera)", 11); _viewMenu.SetItemTooltip(_viewMenu.GetItemIndex(11), "draw the OSD full-screen over the FPV camera, as in goggles (saved in the bench)");
        _viewMenu.IdPressed += id => { if (id < 4) SetCamera((CamMode)id); else if (id == 10) ToggleAxes(); else if (id == 11) ToggleOsdOverlay(); };
        h.AddChild(view);
        UpdateViewMenu();
        // Settings over the bench: the settings page in a window; Back returns here, the core stays connected
        _settingsBtn = new Button { Text = "Settings", Name = "settings", TooltipText = "interface, paths, transmitter, keys — over the bench (the core keeps running)" };
        _settingsBtn.Pressed += OpenSettings;
        h.AddChild(_settingsBtn);
        // configurator address: one click copies the firmware MSP/CLI UART's address in the form the firmware's configurator takes
        var cfg = _cfgBtn = new Button { Name = "cfg_address", Text = "Copy configurator address" };
        cfg.Pressed += () =>
        {
            var a = CfgAddress;
            DisplayServer.ClipboardSet(a);
            Log("app", $"copied {a} to the clipboard for the {ConfiguratorAddress.Name(CfgFirmware)} (MSP blocks arming while the configurator is connected)");
        };
        h.AddChild(cfg);
        // after a reboot, while the firmware does not answer MSP: when the configurator can connect again (Flight.Config.cs)
        _bootHint = new Label { Name = "boot_hint", Visible = false, Modulate = new Color(1f, 0.8f, 0.45f), TooltipText = BootHintTip, MouseFilter = Control.MouseFilterEnum.Pass };
        h.AddChild(_bootHint);
        UpdateCfgHint();
        // centre: what this bench is (a label: not clickable), between two equal spacers
        h.AddChild(new Control { SizeFlagsHorizontal = Control.SizeFlags.ExpandFill, MouseFilter = Control.MouseFilterEnum.Ignore });
        _benchLabel = new Label { Name = "title", MouseFilter = Control.MouseFilterEnum.Ignore, HorizontalAlignment = HorizontalAlignment.Center, Modulate = new Color(1, 1, 1, 0.85f) };
        UpdateBenchLabel();
        h.AddChild(_benchLabel);
        h.AddChild(new Control { SizeFlagsHorizontal = Control.SizeFlags.ExpandFill, MouseFilter = Control.MouseFilterEnum.Ignore });
        _connectBtn = new Button { Text = "Connect", Name = "connect", CustomMinimumSize = new Vector2(96, 0) };
        _connectBtn.Pressed += () => { if (_session.State == CoreClient.ConnState.Disconnected) _session.Connect(); else _session.Disconnect(); };
        h.AddChild(_connectBtn);

        // bottom time bar: commands are sent only while connected (a refused one leaves the local state alone)
        var bottom = new PanelContainer { AnchorTop = 1, AnchorRight = 1, AnchorBottom = 1, OffsetTop = -34, Theme = Ui.BaseTheme };
        _ui.AddChild(bottom);
        var hb = new HBoxContainer();
        bottom.AddChild(hb);
        var run = new Button { Text = "▶ Run" }; run.Pressed += () => { if (_session.Send(new { type = "time", mode = "run" })) _paused = false; };
        var pause = new Button { Text = "⏸ Pause" }; pause.Pressed += () => { if (_session.Send(new { type = "time", mode = "pause" })) _paused = true; };
        var step = new Button { Text = "⏭ Step" }; step.Pressed += () => Run("step");
        var rt = _realtime = new CheckButton { Text = "real-time" };
        rt.Toggled += v => { if (!_session.Send(new { type = "time", mode = _paused ? "pause" : "run", realtime = v })) rt.SetPressedNoSignal(!v); };
        var restart = new Button { Text = "Restart bench" + Keybinds.Suffix("restart_bench") }; restart.Pressed += () => Run("restart_bench");
        var battery = _battery = new CheckButton { Text = "Battery" + Keybinds.Suffix("battery"), ButtonPressed = true, TooltipText = "power on/off (follows the core's fc.power)" };
        battery.Toggled += v => { if (!_session.Send(new { type = "power", on = v })) battery.SetPressedNoSignal(!v); };
        var reset = new Button { Text = "Reset aircraft" + Keybinds.Suffix("reset_aircraft") }; reset.Pressed += () => Run("reset_aircraft");
        _timeLabel = new Label { Text = "t = 0.00 s" };
        hb.AddChild(run); hb.AddChild(pause); hb.AddChild(step); hb.AddChild(rt); hb.AddChild(new VSeparator());
        hb.AddChild(restart); hb.AddChild(battery); hb.AddChild(reset);
        BuildLaunchControls(hb);                              // Launch (L) + the overrides popover: plane benches only (Flight.Plane.cs)
        hb.AddChild(new Control { SizeFlagsHorizontal = Control.SizeFlags.ExpandFill }); hb.AddChild(_timeLabel);
    }

    private Label _benchLabel = null!;
    private HBoxContainer? _topBar;
    private Button? _settingsBtn;
    private void UpdateBenchLabel() => _benchLabel.Text = $"{_bench.Name}  ·  {_bench.Board} / {_bench.Aircraft}";

    // ---- Bench menu ----
    /// What the flight screen changed that is not yet in the bench object: the firmware path typed in the
    /// Firmware window, the window layout and the checked signals (camera, body axes, watch and the core address go in when they
    /// change). A run that may not write the bench (DevEnv.SaveBench) keeps its FB_* layout out of it.
    private void CommitEdits()
    {
        if (_fwPath != null) _bench.FirmwarePath = _fwPath.Text.Trim();
        if (!DevEnv.SaveBench) return;
        _windows.SaveInto(_bench);
        if (_signals != null) _bench.Signals = _signals.CheckedIds();
    }

    /// Bench → Save (and before Edit / after Save as): the bench file, only in a run that may write it.
    private bool WriteBench(string what)
    {
        CommitEdits();
        if (!DevEnv.SaveBench) { Log("app", $"{what}: not written (headless / FB_* layout knob / self-test run)"); return false; }
        _bench.Save();
        Log("app", $"{what}: bench '{_bench.Name}' saved to {_bench.FilePath}");
        return true;
    }

    /// Bench → Save as…: asks a name (another bench's name asks again before overwriting it).
    private void AskSaveAs()
    {
        var name = new LineEdit { Text = _bench.Name + "-copy", SelectAllOnFocus = true, CustomMinimumSize = new Vector2(360, 0) };
        var box = new VBoxContainer();
        box.AddChild(new Label { Text = $"Save a copy of '{_bench.Name}' as:" });
        box.AddChild(name);
        box.AddChild(new Label { Text = "wiring, board config, firmware, layout, camera, core address", Modulate = Ui.Soft });
        var dlg = new ConfirmationDialog { Title = "Save bench as", OkButtonText = "Save", Theme = Ui.BaseTheme };
        dlg.AddChild(box);
        dlg.RegisterTextEnter(name);
        dlg.Confirmed += () =>
        {
            var n = name.Text.Trim();
            if (n == "") { Log("app", "save as: no name given"); return; }
            if (Wizard.AsksBeforeSaving(n, _bench.Name)) AskOverwrite(n);
            else SaveAs(n);
        };
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        _ui.AddChild(dlg);
        dlg.PopupCentered();
        name.GrabFocus();
    }

    private void AskOverwrite(string n)
    {
        var dlg = new ConfirmationDialog { Title = "Bench exists", DialogText = $"A bench saved as '{BenchData.SafeName(n)}.json' already exists. Overwrite it?", OkButtonText = "Overwrite", Theme = Ui.BaseTheme };
        dlg.Confirmed += () => SaveAs(n);
        dlg.VisibilityChanged += () => { if (!dlg.Visible) dlg.QueueFree(); };
        _ui.AddChild(dlg);
        dlg.PopupCentered();
    }

    /// The copy becomes the flown bench (the original's file stays as it was on disk).
    private void SaveAs(string n)
    {
        if (n == _bench.Name) { WriteBench("save"); return; }
        CommitEdits();
        var from = _bench.Name;
        _bench = _bench.CopyAs(n);
        _bench.LastUsed = DateTime.Now;
        AppState.Bench = _bench;
        if (_wiringView != null) { _wiringView.Setup(_bench, Manifest.Current.PadsFor(_bench.Board), Manifest.Current.ActuatorsFor(_bench.Aircraft), readOnly: true); _wiringView.Rebuild(); }
        UpdateBenchLabel();
        WriteBench($"save as (copy of '{from}')");
    }

    private void UpdateConnectButton()
    {
        if (_connectBtn == null) return;
        _connectBtn.Text = _session.State switch { CoreClient.ConnState.Connected => "Disconnect", CoreClient.ConnState.Connecting => "Cancel", _ => "Connect" };
    }

    private void RestartBench()
    {
        if (_ownCore) { RestartCore(); return; }             // the app's own core: stop it and start the bench again
        if (!_session.Send(new { type = "reset", what = "all" })) return;
        if (!string.IsNullOrEmpty(_bench.FirmwarePath)) _session.Send(new { type = "firmware", path = _bench.FirmwarePath, watch = _bench.WatchFirmware });
        Log("app", "restart bench: reset all + firmware + board config");
    }

    private List<string> WiredDevices() => _bench.PresentDevices(Manifest.Current);   // wired + onboard not detached

    /// The live manifest arrived (or replaced the cached one): rebuild what is built from it.
    private void OnManifestChanged()
    {
        var m = Manifest.Current;
        Log("app", "manifest: " + m.Describe());
        _faults?.Build(m, WiredDevices());
        if (_wiringView != null) { _wiringView.Setup(_bench, m.PadsFor(_bench.Board), m.ActuatorsFor(_bench.Aircraft), readOnly: true); _wiringView.Rebuild(); }
        RefreshFunctionsCatalog();
    }

    private void BuildWindows()
    {
        _windows = new FlightWindows(_ui, _bench, HandleKeys);

        // a saved layout without the Aircraft window: Status compacted, Aircraft placed where its plane block was —
        // before either window is made, so both take the migrated rects (logged once the Log window exists)
        var migrated = _windows.MigrateAircraft();

        // Status: four lines, nothing to press (StatusPanel.cs)
        _statusPanel = new StatusPanel();
        _statusPanel.ExitCli.Pressed += () => ExitCli();         // the one control of Status, only while the firmware is in its CLI
        _windows.Make("Status", _statusPanel, DefaultPos["Status"], DefaultSize["Status"], openByDefault: true);

        // Aircraft: the model's numbers and the crash banner (AircraftPanel.cs)
        _aircraftPanel = new AircraftPanel { Model = _readout };
        _aircraftPanel.ResetRequested += () => Run("reset_aircraft");
        _windows.Make("Aircraft", _aircraftPanel, DefaultPos["Aircraft"], DefaultSize["Aircraft"], openByDefault: true, minSize: new Vector2I(260, 140));

        // Transmitter: all input — the radio's switches (the bench's transmitter model: static switches + the ones derived
        // from the firmware's modes), the sticks, the 16 channels, the keyboard hints
        var tx = new VBoxContainer();
        tx.AddChild(BuildModesSwitches());                   // the radio's switches: the bench's transmitter model + the firmware's modes (Flight.Modes.cs)
        var sticks = new HBoxContainer();
        _stickL = new StickView { CustomMinimumSize = new Vector2(120, 120) }; _stickR = new StickView { CustomMinimumSize = new Vector2(120, 120) };
        sticks.AddChild(_stickL); sticks.AddChild(_stickR);
        tx.AddChild(sticks);
        // 16 live channel bars: 1..8 left column, 9..16 right
        var grid = new GridContainer { Columns = 2, SizeFlagsHorizontal = Control.SizeFlags.ExpandFill };
        grid.AddThemeConstantOverride("h_separation", 6); grid.AddThemeConstantOverride("v_separation", 2);
        for (int i = 0; i < 16; i++)
        {
            int ch = (i % 2) * 8 + i / 2;
            _rcBars[ch] = new ChannelBar(RcNames.Short(ch), compact: true);
            _rcBars[ch].SetDimmed(ch >= 4 && RcInput.SourceOf(ch).Kind == "none");   // 1..4: the keyboard layer is always there
            grid.AddChild(_rcBars[ch]);
        }
        tx.AddChild(grid);
        _keysHint = new Label { AutowrapMode = TextServer.AutowrapMode.WordSmart, Modulate = Ui.Soft, MouseFilter = Control.MouseFilterEnum.Pass };
        _keysHint.AddThemeFontSizeOverride("font_size", 13);
        UpdateKeysHint();
        tx.AddChild(_keysHint);
        _windows.Make("Transmitter", tx, DefaultPos["Transmitter"], DefaultSize["Transmitter"], openByDefault: true, minSize: new Vector2I(300, 320));

        // Log / CLI
        var logBox = new VBoxContainer();
        _log = new RichTextLabel { ScrollFollowing = true, SizeFlagsVertical = Control.SizeFlags.ExpandFill, BbcodeEnabled = true };
        logBox.AddChild(_log);
        var cliRow = new HBoxContainer();
        _cli = new CliLine
        {
            PlaceholderText = "CLI command (enters CLI mode, blocks MSP) · " + CliLine.Hint, SizeFlagsHorizontal = Control.SizeFlags.ExpandFill,
            TooltipText = "A firmware CLI line (enters CLI mode; MSP clients cannot connect while in CLI).\nEnter: send · Up/Down: history (text typed first = prefix) · Escape: back to the typed text, again: leave\nCtrl+L: clear the log",
            Sent = text => _session.Send(new { type = "cli", text }),   // not sent: the text stays (and is not stored)
            ClearLog = ClearLog,
        };
        _cliLogLabel = new Label { Text = CliText, Visible = false, Modulate = new Color(1f, 0.72f, 0.35f), TooltipText = "the firmware sits in its CLI: MSP (the configurator, the core's polls) does not answer until it leaves it" };
        var exitCli = _cliExitBtn = new Button { Text = "exit CLI (reboots)", Name = "exit_cli", TooltipText = "{\"type\":\"cli\",\"exit\":true}: the core sends `exit`, the firmware leaves its CLI and reboots (a CLI `save` reboots too)" };
        exitCli.Pressed += () => ExitCli();
        cliRow.AddChild(_cli); cliRow.AddChild(_cliLogLabel); cliRow.AddChild(exitCli);
        logBox.AddChild(cliRow);
        _windows.Make("Log / CLI", logBox, DefaultPos["Log / CLI"], DefaultSize["Log / CLI"], openByDefault: true);
        if (migrated != null) Log("app", migrated);

        // Firmware
        var fw = new VBoxContainer();
        var fwRow = new HBoxContainer();
        _fwPath = new LineEdit { Text = _bench.FirmwarePath, SizeFlagsHorizontal = Control.SizeFlags.ExpandFill };
        var browse = new Button { Text = "Browse…" };
        browse.Pressed += () => Ui.PickFile(_ui, Wizard.FirmwareFilters, Ui.FirmwareDir(_fwPath.Text), p => { _fwPath.Text = p; _bench.FirmwarePath = p; });
        fwRow.AddChild(_fwPath); fwRow.AddChild(browse);
        fw.AddChild(fwRow);
        var fwBtns = new HBoxContainer();
        var replace = new Button { Text = "Replace firmware & restart" };
        replace.Pressed += () => { _bench.FirmwarePath = _fwPath!.Text.Trim(); RestartBench(); };
        _watch = new CheckButton { Text = "watch file (reload after make)", ButtonPressed = _bench.WatchFirmware };
        _watch.Toggled += v =>
        {
            if (_session.Send(new { type = "firmware", path = _bench.FirmwarePath, watch = v })) _bench.WatchFirmware = v;
            else _watch.SetPressedNoSignal(!v);
        };
        var session = new Button { Text = "Save session file…", TooltipText = "the session JSON for core/qemu/run.sh --session (wiring, board config, ports; repo paths of this machine)" };
        session.Pressed += () => Ui.SaveFile(_ui, new[] { "*.json ; session" }, AppState.RepoRoot, BenchData.SafeName(_bench.Name) + ".session.json", SaveSession);
        fwBtns.AddChild(replace); fwBtns.AddChild(_watch); fwBtns.AddChild(session);
        fw.AddChild(fwBtns);
        fw.AddChild(new Label { Text = "Replace = erase config sector, boot, apply the bench's board config. Battery off/on keeps the flash.", AutowrapMode = TextServer.AutowrapMode.Word, Modulate = new Color(1, 1, 1, 0.7f) });
        _windows.Make("Firmware", fw, new Vector2I(400, 370), new Vector2I(600, 160));

        // Signals: list + plots + CSV (Signals.cs); checked core-published ids → `subscribe`
        _signals = new SignalsPanel();
        _signals.Log += Log;
        _signals.SubscribeRequested += ids => _session.Subscribe(ids);
        _windows.Make("Signals", _signals, new Vector2I(400, 370), new Vector2I(900, 420));
        if (DevEnv.Signals is { } autoSignals) { _signals.Check(autoSignals.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)); _windows["Signals"].Visible = true; }
        else if (_bench.Signals.Count > 0)       // the bench's checked set (core ids are subscribed once the first state tells them apart)
        {
            _signals.Check(_bench.Signals);
            Log("app", $"signals: {_bench.Signals.Count} checked from the bench ({string.Join(", ", _bench.Signals)})");
        }
        if (DevEnv.Csv is { } autoCsv) _signals.StartRecord(autoCsv);   // headless: record the checked signals here

        // Wiring: the wizard's graph, read-only (no connect/disconnect/move)
        var wiring = _wiringView = new WiringGraph();
        wiring.Setup(_bench, Manifest.Current.PadsFor(_bench.Board), Manifest.Current.ActuatorsFor(_bench.Aircraft), readOnly: true);
        wiring.Rebuild();
        _windows.Make("Wiring", wiring, new Vector2I(860, 330), new Vector2I(700, 540), minSize: new Vector2I(560, 440));   // older benches saved the list's 400×220

        BuildModelWindow();

        // Faults: one row per manifest fault, lit from state.faults (FaultsPanel.cs)
        _faults = new FaultsPanel { Send = m => _session.Send(m) };
        _faults.Build(Manifest.Current, WiredDevices());
        _windows.Make("Faults", _faults, new Vector2I(1180, 360), new Vector2I(420, 460), minSize: new Vector2I(300, 200));

        // OSD: the firmware's DisplayPort canvas (same buffer as the FPV overlay)
        _osdWindowPanel = new OsdPanel { Buffer = _osd, ShowFrame = true };
        _windows.Make("OSD", _osdWindowPanel, new Vector2I(420, 380), new Vector2I(560, 320), minSize: new Vector2I(260, 150));

        BuildEnvWindows();                      // Functions + Config (Flight.Modes.cs)

        _windows.OpenFromEnv(DevEnv.Windows);   // screenshots: "all" or "Status,Signals"
        if (DevEnv.Osd) _windows["OSD"].Visible = true;

        // layouts are in UI units: a larger UI scale (or a smaller window) shrinks the visible area
        FitWindows();
        GetViewport().SizeChanged += FitWindows;
    }

    private void FitWindows() => _windows.Fit(GetViewport().GetVisibleRect().Size);

    /// The default layout (UI units) of the four main windows: Status compact top-left, Aircraft below it, Log top
    /// centre, Transmitter right (below the Model window's place). At 1600×900 nothing overlaps; a smaller visible area
    /// (a larger UI scale) moves the right-hand windows left, never onto Status / Aircraft.
    public static readonly Dictionary<string, Vector2I> DefaultPos = new()
    {
        ["Status"] = new(20, 70), ["Aircraft"] = new(20, 270), ["Log / CLI"] = new(420, 70), ["Transmitter"] = new(1220, 390),
    };
    public static readonly Dictionary<string, Vector2I> DefaultSize = new()
    {
        ["Status"] = new(380, 150), ["Aircraft"] = new(380, 300), ["Log / CLI"] = new(700, 280), ["Transmitter"] = new(360, 470),
    };

    private void UpdateCfgHint()
    {
        if (_cfgBtn != null)
            _cfgBtn.TooltipText = (_session.Connected && _fc.Cli == true ? $"the firmware is {CliText}: the configurator cannot connect until it leaves the CLI (exit CLI (reboots) in Status / Log)\n" : "")
                                + $"copies {CfgAddress} — the firmware MSP/CLI UART in the form the {ConfiguratorAddress.Name(CfgFirmware)} takes "
                                + "(RotorFlight: tcp://host:port in the port field; iNav: host:port, TCP, IP:Port field). MSP blocks arming while the configurator is connected; CLI mode blocks MSP.";
    }

    /// The firmware the core reported (hello, or its "firmware says" line when the firmware answered later), the modes'
    /// variant before both, else the bench's firmware file: picks the configurator address form.
    private string? _helloFirmware;
    private string? CfgFirmware => ConfiguratorAddress.Pick(_modes?.Variant, _helloFirmware, _bench.FirmwarePath);
    private string CfgAddress => ConfiguratorAddress.For(CfgFirmware, _session.Host, MspPort);

    /// This run's MSP port: the one the app's core listens on (next free ports), else the bench's.
    private int MspPort => _proc?.Ports.Msp ?? _runPorts?.Msp ?? _bench.MspPort;

    private void SaveSession(string path)
    {
        var json = JsonSerializer.Serialize(_bench.ToSession(AppState.RepoRoot), Json.Files);
        Log("app", Json.WriteAtomic(path, json, out var err) ? $"session saved to {path} (core/qemu/run.sh --session {System.IO.Path.GetFileName(path)})" : $"cannot write {path}: {err}");
    }

    /// Window layout (and camera / body axes chosen by hand) into the bench file — only in a run that may
    /// write it (DevEnv.SaveBench: not headless, no FB_WINDOWS / FB_SIGNALS / FB_FAULT_SCRIPT / FB_CAMERA /
    /// FB_AXES, no self-test). FB_CORE never reaches the bench (the session holds it).
    private void SaveLayout()
    {
        if (!DevEnv.SaveBench || _deleted) return;
        CommitEdits();
        _bench.Save();
    }

    // ---- state ----
    private void OnState(JsonElement s)
    {
        _stateCount++;
        _aircraft?.ApplyState(s);
        _modelAircraft?.ApplyState(s);
        _signals?.OnState(s);
        _faults?.OnState(s);
        if (DevEnv.FaultScript) FaultScriptState(s);
        if (Json.TryNum(s, "t", out var t)) _lastT = t;
        if (Json.TryArr3(s, "euler_deg", out var r, out var p, out var y)) { _roll = r; _pitchDeg = p; _heading = y; _hasEuler = true; }
        if (Json.TryBool(s, "fc.msp", out var msp)) { _fcMsp = msp; BootMsp(msp); }
        _readout.ApplyState(s);                                // the Aircraft window (state.plane into _plane too)
        _fc.ApplyState(s, _modes);                             // the Status window
        _spsCount++;
        // the Battery toggle follows the core's power (on every change it reports: another client, a script)
        if (Json.TryBool(s, "fc.power", out var power) && power != _lastPower)
        {
            _lastPower = power;
            _battery?.SetPressedNoSignal(power);
        }
        if (_aircraft != null) _target = _aircraft.Position;
        if (_stateCount % 10 == 0) { OnStateModes(s); UpdateStatus(); }
        // FB_SCREENSHOT_ALT: the screenshot once the aircraft is that high (evidence of a lift-off), then quit
        if (!double.IsNaN(DevEnv.ScreenshotAlt) && !_altShot && DevEnv.Screenshot is { } shot && _aircraft != null && _aircraft.Position.Y - _aircraft.CgHeight >= DevEnv.ScreenshotAlt)
        {
            _altShot = true;
            RenderingServer.FramePostDraw += SaveAltShot;
            void SaveAltShot()
            {
                RenderingServer.FramePostDraw -= SaveAltShot;
                GetViewport().GetTexture().GetImage().SavePng(shot);
                GD.Print($"[dev] FB_SCREENSHOT_ALT={DevEnv.ScreenshotAlt}: alt {_aircraft!.Position.Y - _aircraft.CgHeight:0.00} m at t = {_lastT:0.00} s, screenshot saved to {shot}; quitting");
                GetTree().Quit();
            }
        }
    }

    private bool _altShot;

    /// The Status lines, the Aircraft rows, the Launch button (every 10th state, on events and connection changes).
    private void UpdateStatus()
    {
        if (_statusPanel is { } p)
        {
            // the core's state only while it matters to the link (starting, exited, rejected); its pid / ports are the Bench menu's
            var note = _coreState.StartsWith("core running", StringComparison.Ordinal) ? "" : _coreState;
            p.SetLine(p.Link, FcStatus.LinkLine(_session.State, $"{_session.Host}:{_session.Port}", _fc.Rtf, _sps, note));
            p.SetFirmware(_fc.Firmware(_session.Connected, _plane.Config));
            p.SetLine(p.Sensors, _session.Connected ? _fc.SensorsLine() : "");
            p.SetLine(p.Config, _session.Connected ? _plane.Config.StatusLine(Time.GetTicksMsec(), _cfgHideAt) : "");
        }
        UpdateCliIndicator();
        _aircraftPanel?.Refresh();
        UpdateLaunchButton();
    }

    /// Per frame, cheap: the states/s figure once a second, an `applied` / `restored` config line hidden on time.
    private void StatusTick()
    {
        var now = Time.GetTicksMsec();
        if (_spsAt == 0) _spsAt = now;
        if (now - _spsAt >= 1000)
        {
            _sps = _spsCount * 1000.0 / (now - _spsAt);
            _spsCount = 0; _spsAt = now;
            UpdateStatus();
        }
        else if (_cfgHideAt != 0 && now >= _cfgHideAt) { _cfgHideAt = 0; UpdateStatus(); }
    }

    /// Ctrl+L in the CLI line: empties the log view (the history stays).
    private void ClearLog()
    {
        _log?.Clear();
        _logLines = 0;
    }

    /// Log window (last LogMaxLines lines) + stdout for the app's own lines (the core's log/cli/fc stream stays in the window).
    private void Log(string src, string text)
    {
        if (_log != null)
        {
            _log.AppendText($"[color=gray]{src}[/color] {text.Replace("[", "[lb]")}\n");
            _logLines += 1 + text.Count(ch => ch == '\n');
            if (_logLines > LogMaxLines + 200)                 // trim in batches
            {
                while (_log.GetParagraphCount() > LogMaxLines) _log.RemoveParagraph(0);
                _logLines = _log.GetParagraphCount();
            }
        }
        if (src is not ("core" or "fc" or "cli" or "core:")) GD.Print($"[{src}] {text}");
    }

    public override void _Process(double delta)
    {
        PollCore();
        PollConfig();
        PollDelete();
        _session.Poll();
        _rc.KeysOff = SettingsOpen || WizardOpen;                          // Settings over the bench: arrows / WASD / bind keys do not fly the aircraft
        _rc.Update(delta);
        if (_session.Connected && _rc.TakeSend() && !DevEnv.RcOff)
        {
            _session.Send(_rc.ToMessage());
            if (DevEnv.RcLog && _rcSends++ % 50 == 0) GD.Print($"[rc] send #{_rcSends} ch1..8 = {string.Join(" ", _rc.ChannelsUs[0..8])}");
        }
        if (_rc.TakeLinkChange() is { } lost) RadioLinkFault(lost, lost ? "unplugged" : "back, channels valid");
        for (int ch = 0; ch < 16; ch++) { _rcBars[ch]?.Set(_rc.ChannelsUs[ch]); _rcBars[ch]?.SetMark(RcInput.Mark(_rc.Owner[ch])); }   // who last drove it: joy / kb / ui
        UpdateDerivedSwitches();
        StatusTick();
        ConfigShotTick();
        BeepFlushTick();
        if (_stickL != null && _stickR != null) StickView.ShowSticks(_stickL, _stickR, _rc.Sticks, AppSettings.Current.Transmitter.Mode);
        if (_timeLabel != null) _timeLabel.Text = $"t = {_lastT:0.00} s{(_paused ? "  (paused)" : "")}";
        UpdateCamera(delta);
        if (_osdOverlayPanel != null) _osdOverlayPanel.Visible = _cam == CamMode.Fpv && _osdOverlay;
        UpdateModelInfo();
        if (PlaneTest != PlaneTestMode.None || ModesTest) { }            // the plane / modes screens run only their own tests (below)
        else if (DevEnv.KeysSelftest && !_keysDone) KeysSelfTest();
        else if (DevEnv.RadioSelftest && !_radioDone) RadioSelfTest();
        if (PlaneTest != PlaneTestMode.None && !_planeTestDone) PlaneSelfTest();
        if (ModesTest && !_mtDone) ModesSelfTest();
        else if (ModesTest && !_ltDone && LayersTests.ScreenAny) LayersSelfTest();        // the config layers (LayersTests.Screen.cs)
        else if (ModesTest && !_btDone && BenchTests.ScreenAny) BenchSelfTest();
        if (DevEnv.FaultScript) FaultScript();
        if (DevEnv.LiveCheck) LiveCheckTick();
        BenchDevTick();                                                   // FB_CLI_SEND, FB_DIALOG, FB_EDIT_RESTART (Flight.BenchDev.cs)
        if (DevEnv.Menu is { } menu && ++_menuFrames == 30)   // screenshots: open that top-bar menu
            foreach (var mb in _ui.FindChildren("*", nameof(MenuButton), true, false).OfType<MenuButton>())
                if (mb.Text == menu) mb.ShowPopup();
    }
    private int _menuFrames;

    public override void _UnhandledInput(InputEvent e)
    {
        if (e is InputEventMouseMotion m && (m.ButtonMask & MouseButtonMask.Left) != 0 && _cam == CamMode.Orbit)
        {
            _yaw -= m.Relative.X * 0.005f;
            _pitch = Mathf.Clamp(_pitch + m.Relative.Y * 0.005f, -1.4f, 1.4f);
        }
        if (e is InputEventMouseButton b && b.Pressed)
        {
            if (b.ButtonIndex == MouseButton.WheelUp) _dist = Mathf.Max(0.8f, _dist * 0.9f);
            if (b.ButtonIndex == MouseButton.WheelDown) _dist = Mathf.Min(200f, _dist * 1.1f);
        }
        HandleKeys(e, GetViewport());
    }

    /// Keys not taken by the GUI, from the main view or from a focused embedded window (InputRelay):
    /// Escape releases a text field (one in edit mode takes Escape itself: Ui.ReleaseTextOnUnedit); everything else goes through the Keybinds table (Settings → Keys),
    /// keys and joypad buttons alike.
    /// Stick functions (Keybinds.Sticks) go to the keyboard layer (RcInput.StickKey on the press, StickRelease on the release of
    /// the same key / joypad button), every other function to Run.
    private void HandleKeys(InputEvent e, Viewport vp)
    {
        if (SettingsOpen || WizardOpen) return;                                   // the Settings / Edit bench overlay has the keys
        if (e is InputEventKey k && k.Pressed && !k.Echo && k.Keycode == Key.Escape) { vp.GuiReleaseFocus(); return; }   // hand the keys back from any text field
        if (Keybinds.Match(e, Manifest.Current) is { } fn)
        {
            vp.SetInputAsHandled();
            int stick = Keybinds.StickIndex(fn);
            if (stick >= 0) _rc.StickKey(stick, Keybinds.HoldToken(e));
            else Run(fn);
        }
        else if (e is InputEventKey { Pressed: false } or InputEventJoypadButton { Pressed: false })
        {
            if (_rc.StickRelease(Keybinds.HoldToken(e))) vp.SetInputAsHandled();
        }
    }

    /// The window lost focus: no key release will come for a held stick key, so the keyboard layer lets go now.
    public override void _Notification(int what)
    {
        if (what == NotificationApplicationFocusOut) _rc.ReleaseKeys();
    }

    private static readonly string[] CamFunctions = { "camera_pad", "camera_orbit", "camera_fpv", "camera_chase" };

    /// Runs a bench function by id (Keybinds): the one place keys, joypad buttons and bar buttons end up.
    /// Core commands go out only while connected; a refused one changes nothing locally.
    private void Run(string id)
    {
        switch (id)
        {
            case "restart_bench": RestartBench(); break;
            case "pause": if (_session.Send(new { type = "time", mode = _paused ? "run" : "pause" })) _paused = !_paused; break;
            case "step": _session.Send(new { type = "time", mode = "step", step_s = 0.01 }); break;
            case "battery": if (_battery != null) _battery.ButtonPressed = !_battery.ButtonPressed; break;   // Toggled sends `power` (or flips back)
            case "reset_aircraft": if (_session.Send(new { type = "reset", what = "aircraft" })) { _plane.ClearCrash(); UpdateStatus(); } break;
            case "launch": DoLaunch(); break;
            case "switch_1" or "switch_2" or "switch_3" or "switch_4" or "switch_5": SwitchKey(id[^1] - '0'); break;
            case "screenshot": GetViewport().GetTexture().GetImage().SavePng($"user://screenshot_{DateTime.Now:yyyyMMdd_HHmmss}.png"); Log("app", "screenshot saved to user://"); break;
            case "body_axes": ToggleAxes(); break;
            default:
                int cam = Array.IndexOf(CamFunctions, id);
                if (cam >= 0) SetCamera((CamMode)cam);
                else if (id.StartsWith("window:")) _windows.Toggle(id["window:".Length..]);
                else if (id.StartsWith("fault:") && _faults != null && !_faults.Toggle(id["fault:".Length..])) Log("app", $"{id}: not in the manifest (or not modelled)");
                break;
        }
    }

    // ---- radio unplugged = receiver link lost (TransmitterSettings.FailLinkOnDisconnect, RcInput.RadioLost) ----
    private const string LinkFaultKey = "crsf.link_lost";
    private bool _radioFaultOn;                    // the core was sent link_lost on (and not yet off)

    /// Why `crsf.link_lost` cannot be sent now, or null.
    private string? LinkFaultBlocker()
    {
        var dev = Manifest.Current.Device("crsf");
        if (dev == null || !dev.Faults.Any(f => f.Id == "link_lost")) return $"the manifest ({Manifest.Current.Source}) has no crsf.link_lost";
        if (!dev.Implemented) return "the core does not model crsf";
        if (!WiredDevices().Contains("crsf")) return "no crsf receiver wired on this bench";
        if (!_session.Connected) return "not connected to the core";
        return null;
    }

    /// on = the radio went away: `fault crsf link_lost on` (the Faults row shows it); off = it is back and
    /// its channels were read (the rc frame goes first): `on:false`. Logged either way, with the reason when not sent.
    private void RadioLinkFault(bool on, string why)
    {
        var radio = $"radio '{AppSettings.Current.Transmitter.DeviceName}' {why}";
        if (!on && !_radioFaultOn) { Log("app", $"{radio}: no link_lost to clear"); return; }
        if (LinkFaultBlocker() is { } blocked) { Log("app", $"{radio}: fault {LinkFaultKey} {(on ? "on" : "off")} not sent ({blocked}){(on ? "; channels hold their last values" : "")}"); return; }
        if (!on && !DevEnv.RcOff) _session.Send(_rc.ToMessage());         // the core has the radio's channels before the link returns
        bool sent = _faults?.Set(LinkFaultKey, on) == true;
        if (sent) _radioFaultOn = on;
        Log("app", sent ? $"{radio}: fault {LinkFaultKey} {(on ? "on (receiver hears nothing: the firmware goes to failsafe)" : "off (link back)")}"
                        : $"{radio}: fault {LinkFaultKey} {(on ? "on" : "off")} not sent (Faults row missing or disabled)");
    }

    // ---- cameras ----
    private static CamMode? ParseCam(string? name)
    {
        int i = Array.IndexOf(CamNames, (name ?? "").Trim().ToLowerInvariant());
        return i < 0 ? null : (CamMode)i;
    }

    private void SetCamera(CamMode mode)
    {
        _cam = mode;
        _bench.Camera = CamNames[(int)mode];      // chosen by hand: saved with the layout
        _chaseInit = false;
        if (_aircraft != null) _fpvQuat = _aircraft.Quaternion;
        UpdateViewMenu();
    }

    /// View → Body axes / X: flips what is shown (FB_AXES only sets the start); the bench keeps the choice.
    private void ToggleAxes()
    {
        _axes = !_axes;
        _bench.BodyAxes = _axes;
        _aircraft?.ShowAxes(_axes);
        UpdateViewMenu();
    }

    /// View → OSD overlay: flips it (FB_OSD_OVERLAY only sets the start); the bench keeps the choice.
    private void ToggleOsdOverlay()
    {
        _osdOverlay = !_osdOverlay;
        _bench.OsdOverlay = _osdOverlay;
        UpdateViewMenu();
    }

    private void UpdateViewMenu()
    {
        if (_viewMenu == null) return;
        for (int i = 0; i < 4; i++) _viewMenu.SetItemChecked(_viewMenu.GetItemIndex(i), (int)_cam == i);
        _viewMenu.SetItemChecked(_viewMenu.GetItemIndex(10), _axes);
        _viewMenu.SetItemChecked(_viewMenu.GetItemIndex(11), _osdOverlay);
    }

    private void UpdateCamera(double delta)
    {
        if (_camera == null || _aircraft == null) return;
        float dt = (float)delta;
        switch (_cam)
        {
            case CamMode.Pad:
            {
                // fixed where the pilot stands; zooms in as the aircraft gets far (~3x the model's size of view across, at
                // least 5 m: the RS7 as before, the 1.9 m trainer ~5.6 m), FOV 10..60
                _camera.Near = 0.05f;
                _camera.Position = PadEye;
                var look = _target + new Vector3(0, 0.1f, 0);
                float d = Mathf.Max(0.5f, PadEye.DistanceTo(look));
                float across = Mathf.Max(5f, 3f * _aircraft.ViewSize);
                _camera.Fov = Mathf.Clamp(Mathf.RadToDeg(2f * Mathf.Atan(across / 2f / d)), 10f, 60f);
                if (!PadEye.IsEqualApprox(look)) _camera.LookAt(look, Vector3.Up);
                break;
            }
            case CamMode.Fpv:
            {
                // at the nose, looking forward, following the body attitude with a slight lag
                _camera.Near = 0.02f; _camera.Fov = 80f;
                _fpvQuat = dt <= 0 ? _aircraft.Quaternion : _fpvQuat.Slerp(_aircraft.Quaternion, 1f - Mathf.Exp(-25f * dt)).Normalized();
                _camera.Position = _aircraft.Position + _aircraft.Quaternion * _aircraft.FpvMount;
                _camera.Quaternion = _fpvQuat;
                break;
            }
            case CamMode.Chase:
            {
                // behind and above, following the heading (smoothed); distances scale with the model (RS7 as before)
                _camera.Near = 0.05f; _camera.Fov = 50f;
                float k = Mathf.Max(1f, _aircraft.ViewSize / 1.6f);
                float yaw = Aircraft.Heading(_aircraft.Quaternion);
                _chaseYaw = !_chaseInit || dt <= 0 ? yaw : Mathf.LerpAngle(_chaseYaw, yaw, 1f - Mathf.Exp(-2.5f * dt));
                var back = new Vector3(Mathf.Sin(_chaseYaw), 0, Mathf.Cos(_chaseYaw));   // opposite of the nose
                var want = _target + back * 3.2f * k + new Vector3(0, 1.0f * k, 0);
                _chasePos = !_chaseInit || dt <= 0 ? want : _chasePos.Lerp(want, 1f - Mathf.Exp(-8f * dt));
                _chaseInit = true;
                _camera.Position = _chasePos;
                _camera.LookAt(_target + new Vector3(0, 0.3f, 0), Vector3.Up);
                break;
            }
            default:
            {
                _camera.Near = 0.05f; _camera.Fov = 60f;
                var offset = new Vector3(Mathf.Cos(_pitch) * Mathf.Sin(_yaw), Mathf.Sin(_pitch), Mathf.Cos(_pitch) * Mathf.Cos(_yaw)) * _dist;
                var aim = _target + new Vector3(0, Mathf.Clamp((_dist - 1f) / 7f, 0f, 1f) * (0.5f - _aircraft.CgHeight), 0);   // 8 m and more: 0.5 m above the skids (the old aim); close up: the CG
                _camera.Position = aim + offset;
                _camera.LookAt(aim, Vector3.Up);
                break;
            }
        }
    }

    /// "Model" window: the aircraft alone in its own world, seen from the front-right at eye level,
    /// rotating with the attitude (roll/pitch; heading optional), whatever the main camera does.
    private void BuildModelWindow()
    {
        var box = new VBoxContainer();
        var container = new SubViewportContainer { Stretch = true, SizeFlagsVertical = Control.SizeFlags.ExpandFill, SizeFlagsHorizontal = Control.SizeFlags.ExpandFill, CustomMinimumSize = new Vector2(160, 120) };
        var vp = new SubViewport { OwnWorld3D = true, Msaa3D = Viewport.Msaa.Msaa4X, RenderTargetUpdateMode = SubViewport.UpdateMode.WhenVisible };
        container.AddChild(vp);
        box.AddChild(container);
        vp.AddChild(new WorldEnvironment { Environment = new Godot.Environment { BackgroundMode = Godot.Environment.BGMode.Color, BackgroundColor = new Color(0.36f, 0.5f, 0.7f),
                                                                                  AmbientLightSource = Godot.Environment.AmbientSource.Color, AmbientLightColor = new Color(0.6f, 0.62f, 0.7f) } });
        vp.AddChild(new DirectionalLight3D { RotationDegrees = new Vector3(-55, 40, 0), LightEnergy = 1.2f });
        // far ground plane: its edge is the horizon at eye level, a reference for bank and pitch
        vp.AddChild(new MeshInstance3D { Mesh = new PlaneMesh { Size = new Vector2(4000, 4000) }, Position = new Vector3(0, -8f, 0),
                                         MaterialOverride = new StandardMaterial3D { AlbedoColor = new Color(0.25f, 0.28f, 0.22f), ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded } });
        _modelAircraft = new Aircraft { Name = "model_aircraft", AircraftName = _bench.Aircraft, VisualPath = VisualPath(), AttitudeOnly = true, WithHeading = false };
        vp.AddChild(_modelAircraft);
        var aim = new Vector3(0, 0.05f, 0.2f);
        var cam = new Camera3D { Current = true, Fov = 40f, Near = 0.05f };
        vp.AddChild(cam);
        var eye = aim + new Vector3(1f, 0.12f, -1f).Normalized() * 3.0f;
        cam.Transform = new Transform3D(Basis.LookingAt(aim - eye, Vector3.Up), eye);   // not in the tree yet: no LookAt
        var row = new HBoxContainer();
        _modelInfo = new Label { Text = "", SizeFlagsHorizontal = Control.SizeFlags.ExpandFill, ClipText = true };
        var heading = new CheckBox { Text = "heading", TooltipText = "rotate with the heading too (off: roll and pitch only, the nose keeps pointing front-left in the view)" };
        heading.Toggled += v => { if (_modelAircraft != null) _modelAircraft.WithHeading = v; };
        row.AddChild(_modelInfo); row.AddChild(heading);
        box.AddChild(row);
        _windows.Make("Model", box, new Vector2I(1240, 50), new Vector2I(340, 300), minSize: new Vector2I(200, 170));
    }

    private void UpdateModelInfo()
    {
        if (_modelInfo == null || !_hasEuler || !_windows.IsOpen("Model")) return;
        _modelInfo.Text = $"roll {_roll:+0;-0;0}°  pitch {_pitchDeg:+0;-0;0}°  hdg {((_heading % 360) + 360) % 360:000}°";
    }

    public override void _ExitTree()
    {
        GD.Print($"FlightBench flight exiting: {_stateCount} states, last t = {_lastT:0.00} s, signals {_signals?.Store.Traces.Count ?? 0} traces / {_signals?.SampleCount ?? 0} samples, csv rows {_signals?.CsvRows ?? 0}, commands dropped {_core.Dropped}");
        Manifest.Changed -= OnManifestChanged;
        SaveLayout();
        _session.Dispose();
        StopCoreOnClose();
        _rc.Dispose();
    }
}
