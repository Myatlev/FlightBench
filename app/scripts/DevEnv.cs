// Every FB_* environment knob (scripted / headless runs, screenshots, self-tests), read once.
// One rule for on/off flags: set and not "0" / "false" / "off" (FB_FAKE_JOY=0 is off).
// Layout knobs (FB_WINDOWS, FB_SIGNALS, FB_FAULT_SCRIPT, FB_CAMERA, FB_AXES, FB_MENU, FB_OSD, FB_OSD_OVERLAY, FB_LIVE_CHECK) and self-tests make the
// run read-only for the bench: nothing they open or change may end up in user://benches.
using System;
using System.Collections.Generic;
using System.Linq;
using Godot;

namespace FlightBench;

public static class DevEnv
{
    private static string? Get(string name)
    {
        var v = System.Environment.GetEnvironmentVariable(name);
        return string.IsNullOrWhiteSpace(v) ? null : v.Trim();
    }

    private static bool Flag(string name) =>
        Get(name) is { } v && v != "0" && !v.Equals("false", StringComparison.OrdinalIgnoreCase) && !v.Equals("off", StringComparison.OrdinalIgnoreCase);

    private static int Int(string name, int def) => int.TryParse(Get(name), out var i) ? i : def;

    // ---- screens / startup ----
    public static readonly string? Bench = Get("FB_BENCH");
    public static readonly string? Preset = Get("FB_PRESET");             // main menu: open this bundled preset at once (file id or name)
    public static readonly string? Core = Get("FB_CORE");                 // host:port for this run only (connect only: no core is started)
    public static readonly string? CoreExe = Get("FB_CORE_EXE");          // the core executable, after the bundled one and Settings → CorePath
    public static readonly string? BenchFile = Get("FB_BENCH_FILE");      // a core session file started as is (flight screen, corelaunch self-test)
    /// FB_CORE_PORTS=control,msp,monitor,motion,pilot: the five ports of the core this run starts (not saved in the bench),
    /// e.g. a check beside a core already on the defaults; a bare number = control, the rest +1..+4.
    public static readonly CorePorts? CorePorts = ParsePorts(Get("FB_CORE_PORTS"));
    private static CorePorts? ParsePorts(string? v)
    {
        if (v == null) return null;
        var n = v.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).Select(x => int.TryParse(x, out var i) ? i : -1).ToArray();
        var p = n.Length == 1 ? new FlightBench.CorePorts(n[0], n[0] + 1, n[0] + 2, n[0] + 3, n[0] + 4) : n.Length == 5 ? new FlightBench.CorePorts(n[0], n[1], n[2], n[3], n[4]) : default;
        return p.Valid ? p : null;
    }
    public static readonly string? Screen = Get("FB_SCREEN");             // wizard | settings
    public static readonly int WizardStep = Int("FB_WIZARD_STEP", 0);
    public static readonly bool WizardAutofill = Flag("FB_WIZARD_AUTOFILL");
    public static readonly string? WizardSelect = Get("FB_WIZARD_SELECT");
    public static readonly int WizardScroll = Int("FB_WIZARD_SCROLL", 0);   // screenshots: the wizard page scrolled down this far (px)
    public static readonly string? WizardLine = Get("FB_WIZARD_LINE");     // screenshots: the summary's layered view selects the first line containing this text
    public static readonly string? WizardEdit = Get("FB_WIZARD_EDIT");   // bench name: the wizard edits it
    public static readonly string? BenchDir = Get("FB_BENCH_DIR");       // benches folder instead of user://benches
    public static readonly string? SettingsTab = Get("FB_SETTINGS_TAB");
    public static readonly string? SettingsScroll = Get("FB_SETTINGS_SCROLL");
    public static readonly string? UiScale = Get("FB_UI_SCALE");
    public static readonly string? Manifest = Get("FB_MANIFEST");
    public static readonly string? CliHistory = Get("FB_CLI_HISTORY");   // the CLI history file instead of user://cli_history.txt (a self-test run without it: in memory)
    // ---- screenshots ----
    public static readonly string? Screenshot = Get("FB_SCREENSHOT");
    public static readonly int ScreenshotFrame = Int("FB_SCREENSHOT_FRAME", 300);
    public static readonly double ScreenshotAlt = double.TryParse(Get("FB_SCREENSHOT_ALT"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var alt) ? alt : double.NaN;   // flight screen: FB_SCREENSHOT once the aircraft is this high (m)
    public static readonly string? ScreenshotConfig = Get("FB_SCREENSHOT_CONFIG");   // flight screen: a screenshot while the first-start ACC calibration runs (pose 3/6)
    // ---- flight screen ----
    public static readonly string? Windows = Get("FB_WINDOWS");           // all | Name,Name
    public static readonly string? Signals = Get("FB_SIGNALS");           // id,id
    public static readonly string? Csv = Get("FB_CSV");
    public static readonly string? Camera = Get("FB_CAMERA");
    public static readonly string? Orbit = Get("FB_ORBIT");
    public static readonly bool Axes = Flag("FB_AXES");
    public static readonly bool Osd = Flag("FB_OSD");                     // open the OSD window
    public static readonly bool OsdOverlay = Flag("FB_OSD_OVERLAY");      // OSD overlay on the FPV camera on
    public static readonly string? Menu = Get("FB_MENU");                 // open this top-bar menu (screenshots)
    public static readonly bool ActSweep = Flag("FB_ACT_SWEEP");
    public static readonly bool FaultScript = Flag("FB_FAULT_SCRIPT");
    public static readonly bool LiveCheck = Flag("FB_LIVE_CHECK");        // flight screen: the windows' live check against a real core (Flight.Dev.cs)
    public static readonly string? CliSend = Get("FB_CLI_SEND");          // flight screen: send this CLI line once connected (Flight.BenchDev.cs)
    public static readonly string? Dialog = Get("FB_DIALOG")?.Trim().ToLowerInvariant();   // delete | reset | edit: open that dialog after 30 frames (screenshots)
    public static readonly bool EditRestart = Flag("FB_EDIT_RESTART");
    public static readonly string? ConfigMode = Get("FB_CONFIG")?.Trim().ToLowerInvariant();   // script | changes: open the Config window in that mode once connected (screenshots)    // flight screen: the Edit bench → Save & start restart against the app's core, then quit
    public static readonly bool RcOff = Flag("FB_RC_OFF");
    public static readonly bool FakeJoy = Flag("FB_FAKE_JOY");
    public static readonly bool NoJoy = Flag("FB_NO_JOY");               // no joypad is read this run (the keys are the only input on channels 1..4): a PC without a pad
    public static readonly bool RcLog = FakeJoy || Flag("FB_RC_LOG");
    // ---- self-tests: FB_SELFTEST=all or a list (rcmath,cfg_address,normalize,settings,bench,bench_edit,bench_signals,signals,osd,presets,cli_history,corelaunch,corelaunch_restart,corelaunch_reject,corelaunch_portbusy,core,wiring,keys,radio,plane_state,plane_visual,launch_cmd,config_progress,modes_switches,functions_panel,topbar,osd_line,diff_view,status_compact,aircraft_window,transmitter_owns_switches,core_menu,layout_migration,core_fixtures,bench_delete,wizard_script_view,cli_indicator,edit_in_place,layers_view,bench_lines,config_changes,snapshot_export,boot_hint); the older single knobs still work ----
    public static readonly string[] SelfTests = (Get("FB_SELFTEST") ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Select(s => s.ToLowerInvariant()).ToArray();
    public static readonly string? OsdPng = Get("FB_OSD_PNG");             // osd self-test: dump symbol rows + atlas PNGs into this folder
    public static bool SelfTest(string name) => SelfTests.Contains("all") || SelfTests.Contains(name);
    public static readonly bool WiringSelftest = Flag("FB_WIRING_SELFTEST");
    public static readonly bool KeysSelftest = Flag("FB_KEYS_SELFTEST") || SelfTest("keys");
    public static readonly bool RadioSelftest = SelfTest("radio");
    /// A self-test run: the manifest is pinned (FB_MANIFEST, else the bundled fixture tests/manifest-fixture.json;
    /// never the user's cache, and a live manifest neither replaces it nor rewrites the cache).
    public static bool SelfTestRun => SelfTests.Length > 0 || WiringSelftest || KeysSelftest;

    /// A knob that changes what the flight screen shows is set: the bench's layout must not be saved.
    public static readonly bool AnyLayoutOverride = Windows != null || Signals != null || FaultScript || Camera != null || Axes || Menu != null || Osd || OsdOverlay || LiveCheck
                                                    || CliSend != null || Dialog != null || EditRestart || ConfigMode != null;

    public static bool Headless => DisplayServer.GetName() == "headless";

    /// May this run write the bench (layout, LastUsed)? Not headless, no layout knob, no self-test.
    public static bool SaveBench => !Headless && !AnyLayoutOverride && SelfTests.Length == 0 && !WiringSelftest && !KeysSelftest;

    /// May this run write settings.json on leaving a screen? Not headless, not a dev screen / self-test run.
    public static bool SaveSettings => !Headless && Screen == null && SelfTests.Length == 0 && !KeysSelftest;

    /// The knobs that are set, for the start-up log line.
    public static string Describe()
    {
        var set = new List<string>();
        foreach (System.Collections.DictionaryEntry e in System.Environment.GetEnvironmentVariables())
            if (e.Key is string k && k.StartsWith("FB_", StringComparison.Ordinal) && !string.IsNullOrWhiteSpace(e.Value as string)) set.Add($"{k}={e.Value}");
        set.Sort(StringComparer.Ordinal);
        return set.Count == 0 ? "none" : string.Join(" ", set);
    }
}
