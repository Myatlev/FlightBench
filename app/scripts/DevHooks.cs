// Autoload: development hooks driven by the FB_* knobs (DevEnv.cs: read once, one on/off rule),
// active on every screen. Also applies the UI scale at startup (Ui.ApplyScale) — it is the one node
// present on every screen.
// FB_SCREENSHOT=path.png [FB_SCREENSHOT_FRAME=300] saves the viewport at that frame.
// FB_SCREEN=wizard|settings opens that screen on the first frame (screenshots without clicking).
// FB_WIZARD_STEP=n (1-based) jumps the wizard to that step once it is open.
// FB_WIZARD_EDIT=<bench name> with FB_SCREEN=wizard: the wizard edits that bench (edit mode).
// FB_WIZARD_AUTOFILL=1 replaces the wizard's wiring with the board-config autofill (screenshots of a filled graph).
// FB_WIZARD_SELECT=<device id> selects that device's node (inspector screenshots).
// FB_WIRING_SELFTEST=1 in the wizard: autofill → bench JSON → rebuild the graph → print whether its wires equal the entries.
// FB_SELFTEST=all|list: the headless self-tests (SelfTests.cs), run by a SelfTestRunner child.
// FB_FAKE_JOY=1 fakes a joystick (device 0): axes 0..7 slow sines with different periods, button 0
//   toggling every 2 s, fed as InputEventJoypadMotion/Button through Input.ParseInputEvent; RcInput
//   reads it through Joy.Reader = FakeJoyReader (Godot cannot register a fake device as "connected").
// Also removes joypad events from the built-in ui_* actions: a transmitter's sticks and switches
// must not move the GUI focus or press buttons.
using System.Linq;
using Godot;

namespace FlightBench;

public partial class DevHooks : Node
{
    private int _frames;
    private Wizard? _wizardShot;                     // FB_WIZARD_SCROLL / FB_WIZARD_LINE: applied once the session-check answered
    private int _wizardStep = DevEnv.WizardStep;
    private bool _wizardAutofill = DevEnv.WizardAutofill, _wiringSelftest = DevEnv.WiringSelftest;

    private double _fakeT;
    private static readonly float[] FakePeriods = { 3.1f, 4.3f, 5.7f, 7.0f, 2.3f, 9.1f, 3.7f, 6.1f };
    private readonly float[] _fakeFed = new float[8], _fakePrev = new float[8];

    public override void _Ready()
    {
        foreach (var action in InputMap.GetActions())
        {
            if (!action.ToString().StartsWith("ui_")) continue;
            foreach (var ev in InputMap.ActionGetEvents(action))
                if (ev is InputEventJoypadButton or InputEventJoypadMotion) InputMap.ActionEraseEvent(action, ev);
        }
        if (DevEnv.NoJoy && !DevEnv.FakeJoy)
        {
            Joy.Reader = new NoJoyReader();
            Joy.RaiseChanged();
            GD.Print($"[dev] FB_NO_JOY: no joypad read this run ({Input.GetConnectedJoypads().Count} connected, ignored): the keys are the only input on channels 1..4");
        }
        if (DevEnv.FakeJoy)
        {
            Joy.Reader = new FakeJoyReader();
            Joy.RaiseChanged();
            GD.Print("[dev] FB_FAKE_JOY: fake joystick on device 0 (axes 0..7 sines, button 0 every 2 s)");
        }
        Ui.ApplyScale(GetTree());
        GD.Print($"[app] repo root {AppSettings.Current.RepoRoot}");
        GD.Print($"[dev] UI scale {Ui.FormatScale(Ui.CurrentScale)} (setting '{AppSettings.Current.UiScale}'{(Ui.ScaleOverride != null ? $", FB_UI_SCALE={Ui.ScaleOverride}" : "")}, screen dpi {DisplayServer.ScreenGetDpi()})");
        if (DevEnv.SelfTests.Length > 0) AddChild(new SelfTestRunner { Name = "selftest" });
        // the core: where it is; a fresh install (no manifest yet) reads the catalogue from it for the wizard
        var exe = CoreLocator.Find(out var src);
        GD.Print($"[app] core {(exe != null ? exe + " (" + src + ")" : "not found: " + src)}; data root {CoreLocator.DataRoot}");
        if (exe != null) CoreManifest.Ensure(exe, CoreLocator.DataRoot);
        System.AppDomain.CurrentDomain.ProcessExit += (_, _) => CoreProcess.StopAll(2000);
        // SIGTERM / SIGINT (a `timeout`, a kill, Ctrl+C in the terminal) on macOS / Linux: CoreCLR's default handler calls
        // exit() on its own thread while Godot's main loop still runs, and Godot's static destructors then abort on a
        // dead mutex (a SIGABRT crash report instead of an exit). Cancel that and quit through the scene tree: the
        // flight screen's _ExitTree stops the core, the app leaves cleanly.
        if (!System.OperatingSystem.IsWindows())
        {
            foreach (var sig in new[] { System.Runtime.InteropServices.PosixSignal.SIGTERM, System.Runtime.InteropServices.PosixSignal.SIGINT })
                _signals.Add(System.Runtime.InteropServices.PosixSignalRegistration.Create(sig, ctx =>
                {
                    ctx.Cancel = true;
                    if (System.Threading.Interlocked.Exchange(ref _quitRequested, 1) != 0) return;   // a second signal: already on the way out
                    Callable.From(() => { GD.Print($"[app] {ctx.Signal}: quitting"); GetTree().Quit(); }).CallDeferred();
                }));
        }
    }

    /// App exit: every core the app started is stopped (keep-running applies to closing a bench only).
    private readonly System.Collections.Generic.List<System.IDisposable> _signals = new();
    private int _quitRequested;
    public override void _ExitTree() => CoreProcess.StopAll();

    private void FeedFakeJoy(double delta)
    {
        _fakeT += delta;
        for (int a = 0; a < 8; a++)
        {
            float v = Mathf.Sin((float)(_fakeT * Mathf.Tau / FakePeriods[a]) + a);
            _fakePrev[a] = _fakeFed[a]; _fakeFed[a] = v;
            Input.ParseInputEvent(new InputEventJoypadMotion { Device = 0, Axis = (JoyAxis)a, AxisValue = v });
        }
        bool pressed = ((int)(_fakeT / 2.0)) % 2 == 1;
        if (pressed != Input.IsJoyButtonPressed(0, JoyButton.A))
            Input.ParseInputEvent(new InputEventJoypadButton { Device = 0, ButtonIndex = JoyButton.A, Pressed = pressed });
        if (_frames == 60)
        {
            // parsed events are flushed with the next frame's input, so GetJoyAxis may show the previous frame's value
            float got = Input.GetJoyAxis(0, JoyAxis.LeftX), got7 = Input.GetJoyAxis(0, (JoyAxis)7);
            bool now = Mathf.IsEqualApprox(got, _fakeFed[0]) && Mathf.IsEqualApprox(got7, _fakeFed[7]);
            bool prev = Mathf.IsEqualApprox(got, _fakePrev[0]) && Mathf.IsEqualApprox(got7, _fakePrev[7]);
            GD.Print($"[dev] fake joy check: fed a0={_fakeFed[0]:0.000} a7={_fakeFed[7]:0.000} (previous frame {_fakePrev[0]:0.000} {_fakePrev[7]:0.000}) -> Input.GetJoyAxis a0={got:0.000} a7={got7:0.000} -> {(now ? "ParseInputEvent drives GetJoyAxis (same frame)" : prev ? "ParseInputEvent drives GetJoyAxis (one frame later)" : "MISMATCH")}");
        }
    }

    public override void _Process(double delta)
    {
        _frames++;
        CoreManifest.Poll();
        if (DevEnv.FakeJoy) FeedFakeJoy(delta);
        if (_frames == 1 && DevEnv.Screen is { } screen)
        {
            var scene = screen switch { "wizard" => "res://scenes/Wizard.tscn", "settings" => "res://scenes/Settings.tscn", _ => null };
            if (scene == null) GD.Print($"[dev] FB_SCREEN={screen}: unknown (wizard|settings)");
            else if (screen == "wizard" && DevEnv.WizardEdit is { } edit)
            {
                var b = BenchData.ListAll().Find(x => x.Name == edit) ?? BenchData.Load(BenchData.PathFor(edit));
                if (b == null) GD.Print($"[dev] FB_WIZARD_EDIT={edit}: bench not found in {BenchData.Dir}");
                Wizard.Open(GetTree(), b ?? BenchData.New(), edit: b != null);
            }
            else
            {
                if (screen == "wizard") AppState.Bench ??= BenchData.New();
                GetTree().CallDeferred(SceneTree.MethodName.ChangeSceneToFile, scene);
            }
        }
        if ((_wizardStep > 0 || _wizardAutofill || _wiringSelftest) && GetTree().CurrentScene is Wizard wiz && wiz.IsNodeReady())
        {
            if (_wizardStep > 0) wiz.ShowStep(_wizardStep - 1);
            if (_wizardAutofill) { wiz.ShowStep(2); wiz.AutofillWiring(); GD.Print("[dev] FB_WIZARD_AUTOFILL: wiring autofilled from the board config"); }
            if (DevEnv.WizardSelect is { } sel) wiz.SelectDevice(sel);
            if (DevEnv.WizardScroll > 0 || DevEnv.WizardLine != null) _wizardShot = wiz;
            if (_wiringSelftest) wiz.WiringSelfTest();
            _wizardStep = 0; _wizardAutofill = false; _wiringSelftest = false;
        }
        if (_wizardShot is { } ws && IsInstanceValid(ws) && !ws.ScriptBusy && ws.Script != null)
        {
            if (DevEnv.WizardLine is { } lt)
            {
                var line = ws.Script.Lines.FirstOrDefault(l => l.Text.Contains(lt, System.StringComparison.Ordinal));
                if (line != null) ws.Layers.SelectLine(line);
            }
            ws.ScrollPage(DevEnv.WizardScroll);
            _wizardShot = null;
        }
        if (_frames == DevEnv.ScreenshotFrame && DevEnv.Screenshot is { } shot && !DevEnv.FaultScript && double.IsNaN(DevEnv.ScreenshotAlt))   // the fault script / FB_SCREENSHOT_ALT take their own
        {
            GetViewport().GetTexture().GetImage().SavePng(shot);
            GD.Print("[dev] screenshot saved to " + shot);
        }
    }
}

/// Joy.Reader for FB_NO_JOY: no pad connected (a PC without a joypad: the keyboard layer of RcInput is the only input on channels 1..4).
public sealed class NoJoyReader : IJoyReader
{
    public int[] Pads() => System.Array.Empty<int>();
    public string Name(int device) => "";
    public float Axis(int device, int axis) => 0;
    public bool Button(int device, int button) => false;
}

/// Joy.Reader for FB_FAKE_JOY: device 0 is "connected"; values come from Godot's Input, fed by DevHooks.
public sealed class FakeJoyReader : IJoyReader
{
    public int[] Pads() => new[] { 0 };
    public string Name(int device) => device == 0 ? "FlightBench fake joystick" : "";
    public float Axis(int device, int axis) => Input.GetJoyAxis(device, (JoyAxis)axis);
    public bool Button(int device, int button) => Input.IsJoyButtonPressed(device, (JoyButton)button);
}
