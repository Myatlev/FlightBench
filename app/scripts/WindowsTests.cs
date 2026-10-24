// Self-tests of the flight screen's windows after the 2026-10-01 reorganisation (one fact in one place) and of the app
// against the real core's captures (FB_SELFTEST=all, or status_compact,aircraft_window,transmitter_owns_switches,
// core_menu,layout_migration,core_fixtures). Model parts are pure; screen parts run on the modes screen (an in-memory
// RS7 bench connected to the local fake core, ModesTests.cs) and, for the plane's rows, on the plane screen (PlaneTests.cs):
//   status_compact            FcStatus lines (link, firmware with mode tags, sensors) and the config line's life
//                             (applying → calibrating k/6 → applied, hidden after; replayed restored; failed stays); on
//                             the screen: the Status window holds exactly link / firmware / sensors / config, no control,
//                             the config line shows only while it runs and hides on time;
//   aircraft_window           AircraftReadout rows for a plane and a heli state (rows the state lacks hidden); on the
//                             screens: the Aircraft window open with the heli's / plane's rows, the crash banner opens it,
//                             Reset aircraft clears it;
//   transmitter_owns_switches no toggle / button in Status; the switch rows (channel order) and the keyboard hints in
//                             the Transmitter window, no toggle there (the old ARM / mode / third toggles are gone);
//   core_menu                 the Bench menu's Core group (read-only Core line, Restart core, Bench check, Core address…,
//                             Close bench); nothing of the core in Status;
//   layout_migration          an old layout (no Aircraft) gets Aircraft opened off every other window, Status compacted;
//                             the default layout and the presets' layouts: Status / Aircraft / Transmitter / Log do not
//                             overlap at 1600×900 and at UI scale 1.25; on the screen: a FlightWindows over an old layout;
//   core_fixtures             the real core's messages (core/qemu/evidence/modes/*.json, the manifest in
//                             app/tests/manifest-core-2026-09-29.json) through the app's models: modes (variant,
//                             rx_channel, logic), config (first start with function steps, applied {functions,
//                             acc_calibrated}, replay), function progress + replay, diff (source, refusals), the
//                             manifest's functions, the configurator address from the variant.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public static class WindowsTests
{
    public static readonly string[] Names = { "status_compact", "aircraft_window", "transmitter_owns_switches", "core_menu", "layout_migration", "core_fixtures" };
    public static bool Any => Names.Any(DevEnv.SelfTest);
    /// The plane screen's aircraft_window findings (reported with the rest on the modes screen).
    public static readonly List<string> PlaneFails = new();
    public static bool PlaneRan;

    private static void Check(List<string> f, bool ok, string what) { if (!ok) f.Add(what); }
    private static JsonElement El(string json) { using var d = JsonDocument.Parse(json); return d.RootElement.Clone(); }

    // ---- status_compact ----
    public static List<string> StatusModel()
    {
        var f = new List<string>();
        var fc = new FcStatus();
        var cfg = new ConfigProgress();
        Check(f, FcStatus.LinkLine(CoreClient.ConnState.Connected, "127.0.0.1:31250", 0.39, 97.4, "") == "connected · realtime 0.39× · 97 states/s", "link connected: " + FcStatus.LinkLine(CoreClient.ConnState.Connected, "127.0.0.1:31250", 0.39, 97.4, ""));
        Check(f, FcStatus.LinkLine(CoreClient.ConnState.Disconnected, "127.0.0.1:31250", null, 0, "core exited (code 1)") == "not connected (127.0.0.1:31250) · core exited (code 1)", "link down with the core's note");
        Check(f, FcStatus.FirmwareText(fc.Firmware(false, cfg)) == "", "not connected: no firmware line");
        Check(f, FcStatus.FirmwareText(fc.Firmware(true, cfg)) == "firmware: no state yet", "no state yet: " + FcStatus.FirmwareText(fc.Firmware(true, cfg)));
        fc.ApplyState(El("{\"type\":\"state\",\"fc\":{\"msp\":false,\"armed\":false}}"), null);
        Check(f, FcStatus.FirmwareText(fc.Firmware(true, cfg)).StartsWith("firmware: waiting for it to answer"), "booting: " + FcStatus.FirmwareText(fc.Firmware(true, cfg)));
        fc.ApplyState(El("{\"type\":\"state\",\"osd\":{\"kind\":\"max7456\",\"cols\":30,\"rows\":16,\"video\":false},\"timing\":{\"realtime_factor\":0.5},"
                         + "\"fc\":{\"msp\":true,\"armed\":false,\"mode\":\"ANGLE+NAV LAUNCH\",\"boxes\":[1,36],\"arming_flags\":[\"CAL\",\"NOPREARM\"],\"cpu_pct\":12,\"cli\":false,\"power\":true,\"baro_alt_m\":3.214}}"), null);
        var fw = fc.Firmware(true, cfg);
        Check(f, FcStatus.FirmwareText(fw) == "disarmed · [ANGLE] [NAV LAUNCH] · flags: CAL NOPREARM · cpu 12%" && fw.Tags.Count == 2, "firmware line: " + FcStatus.FirmwareText(fw));
        Check(f, fc.SensorsLine() == "OSD: MAX7456 30×16 (no video in FlightBench) · baro 3.21 m" && fc.Rtf == 0.5, "sensors: " + fc.SensorsLine());
        fc.ApplyState(El("{\"type\":\"state\",\"fc\":{\"msp\":true,\"armed\":true,\"mode\":\"ACRO\",\"arming_flags\":[],\"cpu_pct\":6}}"), null);
        Check(f, FcStatus.FirmwareText(fc.Firmware(true, cfg)) == "ARMED · [ACRO] · cpu 6%", "armed: " + FcStatus.FirmwareText(fc.Firmware(true, cfg)));
        Check(f, fc.SensorsLine() == "OSD: MAX7456 30×16 (no video in FlightBench)", "a state without the baro drops it: " + fc.SensorsLine());
        fc.ApplyState(El("{\"type\":\"state\",\"fc\":{\"cli\":true}}"), null);
        Check(f, FcStatus.FirmwareText(fc.Firmware(true, cfg)).StartsWith("firmware: " + Flight.CliText), "cli: " + FcStatus.FirmwareText(fc.Firmware(true, cfg)));
        fc.ApplyState(El("{\"type\":\"state\",\"fc\":{\"cli\":false,\"power\":false}}"), null);
        Check(f, FcStatus.FirmwareText(fc.Firmware(true, cfg)) == "firmware: POWER OFF (Battery)", "power off");
        fc.ApplyState(El("{\"type\":\"state\",\"gps\":{\"fix\":3,\"num_sat\":12}}"), null);
        Check(f, fc.SensorsLine().EndsWith("GPS 3D fix 12 sat"), "GPS when the state has it: " + fc.SensorsLine());
        fc.Clear();
        Check(f, fc.SensorsLine() == "" && fc.Osd == null, "Clear");
        // the config line's life
        void Ev(string json) { var d = El(json); cfg.OnConfig(d, 1); }
        Ev("{\"state\":\"applying\",\"lines\":13}");
        Check(f, cfg.StatusLine(0, 0).StartsWith("config: applying"), "applying shown: " + cfg.StatusLine(0, 0));
        Ev("{\"state\":\"calibrating\",\"lines\":13,\"function\":\"calibrate_acc\"}");
        cfg.OnFunction(El("{\"type\":\"function\",\"name\":\"calibrate_acc\",\"phase\":\"step\",\"step\":3,\"steps\":6,\"detail\":\"left side down (iNav position 5) collected\"}"));
        Check(f, cfg.StatusLine(0, 0) == "config: calibrating the accelerometer… pose 3/6 (left side down)", "calibrating k/6 from the function: " + cfg.StatusLine(0, 0));
        Ev("{\"state\":\"applied\",\"lines\":13,\"functions\":[\"calibrate_acc\"],\"acc_calibrated\":true}");
        Check(f, cfg.StatusLine(10, 20) == "config: applied · ACC calibrated" && cfg.StatusLine(30, 20) == "", "applied: shown, then hidden");
        Ev("{\"state\":\"restored\",\"lines\":13,\"replay\":true}");
        Check(f, cfg.Replay && cfg.StatusLine(10, 20) == "config: restored (saved snapshot)" && cfg.StatusLine(30, 20) == "", "replayed restored: briefly");
        Ev("{\"state\":\"failed\",\"lines\":13,\"error\":\"not collected within 2.5 s\",\"function\":\"calibrate_acc\"}");
        Check(f, cfg.StatusLine(99999, 0) == "config: FAILED — function calibrate_acc: not collected within 2.5 s (the firmware runs its defaults)", "failed function stays: " + cfg.StatusLine(99999, 0));
        return f;
    }

    // ---- aircraft_window ----
    public const string HeliState = "{\"type\":\"state\",\"t\":12.345,\"pos_ned\":[0.0,0.0,-3.2],\"vel_ned\":[0.3,0.4,-0.5],\"euler_deg\":[0.5,-4.8,120.3],"
        + "\"rpm\":1800,\"motor_rpm\":16062,\"actuators\":{\"swash_a\":0.32,\"swash_b\":0.31,\"swash_c\":0.30,\"tail\":-0.05,\"motor\":0.62},\"on_ground\":false,"
        + "\"heli\":{\"coll_deg\":6.21,\"thrust_n\":31.4},"
        + "\"fc\":{\"msp\":true,\"armed\":true,\"mode\":\"ANGLE\",\"boxes\":[0,1]}}";

    public static List<string> AircraftModel()
    {
        var f = new List<string>();
        var heli = new AircraftReadout(new PlaneStatus());
        Check(f, heli.Rows().Count == 0, "no state: no rows");
        heli.ApplyState(El(HeliState));
        var ids = heli.Rows().Select(r => r.Id).ToList();
        Check(f, ids.SequenceEqual(new[] { "alt", "speed", "power", "surfaces" }), "heli rows: " + string.Join(",", ids));
        var t = heli.Text();
        Check(f, t.Contains("head 1800 rpm") && t.Contains("collective +6.2°") && t.Contains("thrust 31 N"), "heli rotor row (state.heli): " + t);
        foreach (var want in new[] { "altitude: 3.20 m", "ground 0.5 m/s · climb +0.5 m/s", "rotor: head 1800 rpm · motor 16062 rpm · collective +6.2° · thrust 31 N · throttle 62%", "servos: swash_a +32% · swash_b +31% · swash_c +30% · tail -5%" })
            Check(f, t.Contains(want), $"heli lacks '{want}': {t.Replace("\n", " | ")}");
        heli.ApplyState(El("{\"type\":\"state\",\"pos_ned\":[0,0,0],\"on_ground\":true}"));
        Check(f, heli.Rows().Select(r => r.Id).SequenceEqual(new[] { "alt" }) && heli.Text() == "altitude: 0.00 m · on the ground", "a sparse state hides the rows it lacks: " + heli.Text());
        var plane = new AircraftReadout(new PlaneStatus());
        plane.ApplyState(El(PlaneTests.StateSample));
        ids = plane.Rows().Select(r => r.Id).ToList();
        Check(f, ids.SequenceEqual(new[] { "alt", "speed", "air", "power", "surfaces", "launch" }), "plane rows: " + string.Join(",", ids));
        t = plane.Text();
        foreach (var want in new[] { "motor: thrust 21.4 N · prop 6000 rpm · throttle 60%", "surfaces: ail L +10.0° R -10.0° · elev +5.0° · rud -7.0°", "launch: ramp · held · scenario launch" })
            Check(f, t.Contains(want), $"plane lacks '{want}': {t.Replace("\n", " | ")}");
        return f;
    }

    // ---- layout_migration ----
    /// The presets' layouts before the Aircraft window (main, 2026-09-29).
    public static Dictionary<string, WindowLayout> OldLayout(bool trainer)
    {
        WindowLayout W(bool open, int x, int y, int w, int h) => new() { Open = open, X = x, Y = y, W = w, H = h };
        return new Dictionary<string, WindowLayout>
        {
            ["Status"] = W(true, 20, 50, 360, trainer ? 470 : 300),
            ["Transmitter"] = trainer ? W(true, 1240, 370, 340, 450) : W(true, 20, 390, 310, 330),
            ["Log / CLI"] = W(true, 400, 50, 700, 300),
            ["Firmware"] = W(false, 400, 370, 600, 160),
            ["Signals"] = W(false, 400, 370, 900, 420),
            ["Wiring"] = W(false, 860, 330, 700, 540),
            ["Model"] = W(trainer, 1240, 50, 340, 300),
            ["Faults"] = W(false, 1180, 360, 420, 460),
            ["OSD"] = W(false, 420, 380, 560, 320),
        };
    }

    private static readonly Vector2[] Areas = { new(1600, 900), new(1280, 720), new(1536, 864) };   // 1600×900 at 1 and 1.25, 1920×1080 at 1.25

    /// Pairwise overlaps of the main windows after fitting into each area (title bars counted).
    private static void NoOverlap(List<string> f, string what, Dictionary<string, Rect2I> wanted)
    {
        string[][] pairs = { new[] { "Status", "Transmitter" }, new[] { "Aircraft", "Transmitter" }, new[] { "Status", "Aircraft" }, new[] { "Status", "Log / CLI" }, new[] { "Aircraft", "Log / CLI" } };
        foreach (var area in Areas)
            foreach (var p in pairs)
            {
                if (!wanted.TryGetValue(p[0], out var a) || !wanted.TryGetValue(p[1], out var b)) continue;
                var fa = FlightWindows.Fitted(a, area); var fb = FlightWindows.Fitted(b, area);
                Check(f, !fa.Intersects(fb), $"{what}: {p[0]} {fa} overlaps {p[1]} {fb} in {area.X}×{area.Y}");
            }
    }

    public static List<string> LayoutModel(List<string> info)
    {
        var f = new List<string>();
        foreach (var trainer in new[] { false, true })
        {
            var old = OldLayout(trainer);
            var a = FlightWindows.AircraftPlacement(old, out var st);
            Check(f, a != null && st != null && st.H <= FlightWindows.CompactStatusH, $"{(trainer ? "trainer" : "rs7")} old layout: not migrated");
            if (a == null || st == null) continue;
            var r = new Rect2I(a.X, a.Y - 30, a.W, a.H + 30);
            foreach (var (n, w) in old.Where(kv => kv.Value.Open && kv.Key != "Status").Append(new KeyValuePair<string, WindowLayout>("Status", st)))
                Check(f, !r.Intersects(new Rect2I(w.X, w.Y - 30, w.W, w.H + 30)), $"migrated Aircraft {a.X},{a.Y} {a.W}×{a.H} overlaps {n}");
            Check(f, a.X + a.W <= 1600 && a.Y + a.H <= 900 && a.Open, "migrated Aircraft off the 1600×900 screen / closed");
            info.Add($"{(trainer ? "trainer" : "rs7")}: Aircraft at {a.X},{a.Y}");
        }
        var with = OldLayout(false); with["Aircraft"] = new WindowLayout { Open = false, X = 1, Y = 2, W = 300, H = 200 };
        Check(f, FlightWindows.AircraftPlacement(with, out _) == null && FlightWindows.AircraftPlacement(new(), out _) == null, "a layout with Aircraft / an empty one is left alone");
        // the default layout and the presets' layouts
        NoOverlap(f, "default layout", Flight.DefaultPos.ToDictionary(kv => kv.Key, kv => new Rect2I(kv.Value, Flight.DefaultSize[kv.Key])));
        foreach (var p in Presets.List())
        {
            var l = p.Bench.Layout;
            Check(f, l.TryGetValue("Aircraft", out var ac) && ac.Open && l["Status"].Open && l["Transmitter"].Open, $"preset {p.Id}: Aircraft / Status / Transmitter not all open");
            Check(f, l.TryGetValue("Status", out var ps) && ps.H <= FlightWindows.CompactStatusH, $"preset {p.Id}: Status not compact");
            NoOverlap(f, "preset " + p.Id, l.Where(kv => kv.Value.Open).ToDictionary(kv => kv.Key, kv => new Rect2I(kv.Value.X, kv.Value.Y, kv.Value.W, kv.Value.H)));
        }
        Check(f, Keybinds.Windows.Contains("Aircraft") && Array.IndexOf(Keybinds.Windows, "Aircraft") == 1, "Windows menu: Aircraft after Status");
        return f;
    }

    // ---- core_fixtures ----
    public static string EvidenceDir => Path.Combine(AppState.RepoRoot, "core", "qemu", "evidence", "modes");
    public const string CoreManifestFixture = "res://tests/manifest-core-2026-09-29.json";

    private static List<JsonElement> Messages(string file)
    {
        using var d = JsonDocument.Parse(File.ReadAllText(Path.Combine(EvidenceDir, file)));
        return d.RootElement.ValueKind == JsonValueKind.Array ? d.RootElement.EnumerateArray().Select(x => x.Clone()).ToList() : new() { d.RootElement.Clone() };
    }

    public static List<string> CoreFixtures(List<string> info)
    {
        var f = new List<string>();
        if (!File.Exists(Path.Combine(EvidenceDir, "modes-rs7-2026-09-29.json"))) { info.Add($"skipped: no captures under {EvidenceDir} (not a repo run)"); return f; }
        string D(DerivedSwitch x) => $"ch{x.Channel}:" + string.Join("|", x.Positions.Select(p => $"{p.Label}@{p.Us}"));
        // modes: variant, rx_channel (never the firmware's rc_channel / rx_map), logic
        var rs7 = ModesSnapshot.FromMessage(Messages("modes-rs7-2026-09-29.json")[0], DateTime.Now)!;
        var rsw = rs7.Switches();
        Check(f, rs7.Variant == "RTFL" && rs7.Boxes.Count == 18 && rs7.Ranges.Count == 1 && rs7.Ranges[0].Rx == 4 && rs7.Ranges[0].Logic == "or", $"RS7 modes: variant {rs7.Variant}, {rs7.Boxes.Count} boxes, rx {rs7.Ranges.FirstOrDefault()?.Rx}, logic {rs7.Ranges.FirstOrDefault()?.Logic}");
        Check(f, rsw.Count == 1 && D(rsw[0]) == "ch5:—@1000|ARM@2000", "RS7 switches: " + string.Join("; ", rsw.Select(D)));
        var tr = ModesSnapshot.FromMessage(Messages("modes-trainer-2026-09-29.json")[0], DateTime.Now)!;
        var tsw = tr.Switches();
        Check(f, tr.Variant == "INAV" && tsw.Select(D).SequenceEqual(new[] { "ch5:—@1000|ARM@2000", "ch6:—@1000|ANGLE@2000", "ch7:—@1000|NAV LAUNCH@2000" }), "trainer switches: " + string.Join("; ", tsw.Select(D)));
        // the configurator address follows the variant, whatever the file / hello said
        Check(f, ConfiguratorAddress.For(ConfiguratorAddress.Pick(tr.Variant, "firmware.hex", "fw.hex"), "127.0.0.1", 31235) == "127.0.0.1:31235"
                 && ConfiguratorAddress.For(ConfiguratorAddress.Pick(rs7.Variant, null, "inav.hex"), "127.0.0.1", 31235) == "tcp://127.0.0.1:31235", "configurator address from the variant");
        info.Add($"modes RTFL 18 boxes → {D(rsw[0])}, INAV {tr.Boxes.Count} boxes → {tsw.Count} switches");
        // first start (trainer): config + function + diff as the core sent them
        var plane = new PlaneStatus();
        var fns = new FunctionProgress();
        var lines = new List<string>();
        FirmwareDiff? diff = null;
        void Feed(JsonElement m)
        {
            switch (Json.Str(m, "type"))
            {
                case "event":
                    m.TryGetProperty("data", out var data);
                    plane.ApplyEvent(Json.Str(m, "name") ?? "", data, Json.Num(m, "t", double.NaN));
                    if (plane.Config.StatusLine(0, 1) is { Length: > 0 } l && (lines.Count == 0 || lines[^1] != l)) lines.Add(l);
                    break;
                case "function":
                    fns.Apply(m); plane.Config.OnFunction(m);
                    if (plane.Config.StatusLine(0, 1) is { Length: > 0 } l2 && (lines.Count == 0 || lines[^1] != l2)) lines.Add(l2);
                    break;
                case "diff": if (FirmwareDiff.FromMessage(m, DateTime.Now) is { } dd) diff = dd; break;
            }
        }
        foreach (var m in Messages("messages-trainer-first-2026-09-29.json")) Feed(m);
        var want = new[]
        {
            "config: applying the CLI script (13 lines)… first start: configuring the board",
            "config: calibrating the accelerometer… pose 0/6 (first start, ~4 min at this speed)",
            "config: calibrating the accelerometer… pose 1/6 (level)", "config: calibrating the accelerometer… pose 2/6 (inverted)",
            "config: calibrating the accelerometer… pose 3/6 (left side down)", "config: calibrating the accelerometer… pose 4/6 (right side down)",
            "config: calibrating the accelerometer… pose 5/6 (nose up)", "config: calibrating the accelerometer… pose 6/6 (nose down)",
            "config: applied · ACC calibrated",
        };
        Check(f, lines.SequenceEqual(want), "first-start Status lines: " + string.Join(" | ", lines));
        Check(f, plane.Config.Functions.SequenceEqual(new[] { "calibrate_acc" }) && plane.Config.AccCalibrated && plane.Config.Function == "calibrate_acc", $"applied: functions {string.Join(",", plane.Config.Functions)}, acc {plane.Config.AccCalibrated}");
        Check(f, fns.Line("calibrate_acc").StartsWith("done: all 6 positions") && fns.Running == null, "calibrate_acc: " + fns.Line("calibrate_acc"));
        Check(f, diff?.Source == "first_start" && !diff.Stale && diff.Header(false).Contains("(first start, t = 39.91 s"), "first-start diff: " + diff?.Header(false));
        // a later client (replay) + the second session's functions and diff refusals
        var p2 = new PlaneStatus(); var f2 = new FunctionProgress(); var diffs = new List<FirmwareDiff>(); var errs = new List<string>();
        foreach (var m in Messages("messages-trainer-second-2026-09-29.json"))
        {
            switch (Json.Str(m, "type"))
            {
                case "event": m.TryGetProperty("data", out var data); p2.ApplyEvent(Json.Str(m, "name") ?? "", data, Json.Num(m, "t", double.NaN)); break;
                case "function": f2.Apply(m); break;
                case "diff": if (FirmwareDiff.FromMessage(m, DateTime.Now) is { } dd) diffs.Add(dd); if (Json.Str(m, "error") is { } e) errs.Add(e); break;
            }
            if (diffs.Count == 1 && Json.Str(m, "type") == "diff")
                Check(f, p2.Config.Replay && p2.Config.StatusLine(0, 1) == "config: restored (saved snapshot)" && diffs[0].Replay && diffs[0].Source == "snapshot", $"replay: config replay {p2.Config.Replay} '{p2.Config.StatusLine(0, 1)}', diff replay {diffs[0].Replay} {diffs[0].Source}");
        }
        Check(f, f2.Line("launch").StartsWith("done: released at 13.89 m/s, 0.60 m") && f2.Line("calibrate_gyro").StartsWith("done: the firmware reports the sensors calibrated") && f2.Line("calibrate_acc").StartsWith("done:"), $"functions: launch '{f2.Line("launch")}', gyro '{f2.Line("calibrate_gyro")}'");
        Check(f, errs.Count(e => e == "calibrating") == 3 && diffs.Last().Source == "request" && !diffs.Last().Stale, $"diff refusals while calibrating: {string.Join(",", errs)}, last {diffs.Last().Source}");
        var cal = diffs.First(d => d.Error == "calibrating");
        Check(f, cal.Stale && cal.Header(false).Contains("STALE (calibrating)"), "refusal with the last text: " + cal.Header(false));
        // a function replayed to a late client: its progress line
        var f3 = new FunctionProgress();
        var rp = f3.Apply(El("{\"type\":\"function\",\"t\":14.79,\"name\":\"calibrate_acc\",\"phase\":\"step\",\"step\":1,\"steps\":6,\"detail\":\"level (iNav position 0) collected\",\"replay\":true}"));
        Check(f, f3.Running == "calibrate_acc" && rp?.Log.Contains("(replay") == true, "function replay: " + rp?.Log);
        // RS7: a refusal without text, the modes' CLI error
        var rsMsgs = Messages("messages-rs7-2026-09-29.json");
        var armed = rsMsgs.First(m => Json.Str(m, "type") == "diff" && Json.Str(m, "error") == "armed");
        Check(f, FirmwareDiff.FromMessage(armed, DateTime.Now) == null, "diff refused while armed (text null) keeps the kept diff");
        var cli = rsMsgs.First(m => Json.Str(m, "type") == "modes" && Json.Str(m, "error") == "cli");
        Check(f, ModesSnapshot.FromMessage(cli, DateTime.Now) == null, "modes error cli: no snapshot (the switches stay)");
        Check(f, rsMsgs.Where(m => Json.Str(m, "type") == "modes" && Json.Str(m, "error") == null).All(m => ModesSnapshot.FromMessage(m, DateTime.Now)!.Switches().Select(D).SequenceEqual(rsw.Select(D))), "every RS7 modes after a reboot gives the same switch");
        // the replay capture (a client connecting at t = 8.8)
        var rep = Messages("replay-trainer-2026-09-29.json");
        Check(f, rep.Count == 2 && Json.TryBool(rep[0], "data.replay", out var r0) && r0 && Json.Str(rep[1], "source") == "snapshot", "replay capture: config restored + diff snapshot");
        // the manifest: functions per aircraft only
        using var mf = Godot.FileAccess.Open(CoreManifestFixture, Godot.FileAccess.ModeFlags.Read);
        var man = mf == null ? null : Manifest.Parse(mf.GetAsText(), "core fixture", out _);
        if (man == null) f.Add($"{CoreManifestFixture} not read");
        else
        {
            var tf = man.FunctionsFor("trainer"); var hf = man.FunctionsFor("rs7");
            Check(f, tf.Select(x => x.Name).SequenceEqual(new[] { "calibrate_acc", "calibrate_gyro", "launch" }) && tf[0].AutoFirstStart && !tf[2].AutoFirstStart && hf.Select(x => x.Name).SequenceEqual(new[] { "calibrate_acc", "calibrate_gyro" }) && !hf[0].AutoFirstStart,
                  $"manifest functions: trainer {string.Join(",", tf.Select(x => x.Name))}, rs7 {string.Join(",", hf.Select(x => x.Name))}");
            info.Add($"manifest: trainer {tf.Count} functions, rs7 {hf.Count}");
        }
        return f;
    }
}

public partial class Flight
{
    /// The windows' screen checks on the modes screen (RS7 in memory, fake core); synchronous. Name → failures.
    private Dictionary<string, List<string>> WindowsScreenChecks()
    {
        var r = WindowsTests.Names.ToDictionary(n => n, _ => new List<string>());
        void Check(string t, bool ok, string what) { if (!ok) r[t].Add(what); }
        static IEnumerable<Node> All(Node n) { foreach (var c in n.GetChildren()) { yield return c; foreach (var d in All(c)) yield return d; } }
        var status = _windows["Status"];
        var tx = _windows["Transmitter"];
        // status_compact
        var p = _statusPanel!;
        UpdateStatus();
        Check("status_compact", p.GetParent()?.GetParent() == status && p.GetChildren().SequenceEqual(new Node[] { p.Link, p.FirmwareRow, p.Sensors, p.Config }), "the Status window holds exactly link / firmware / sensors / config");
        // the one exception: "exit CLI (reboots)", shown only while the firmware is in its CLI (cli_indicator)
        static bool Shown(Node n) => n is Control c && c.Visible;
        Check("status_compact", !All(status).Any(n => n is BaseButton or LineEdit && Shown(n)), "a control in Status: " + string.Join(",", All(status).Where(n => n is BaseButton or LineEdit && Shown(n)).Select(n => n.GetType().Name)));
        Check("status_compact", All(status).Count(n => n is BaseButton) == 1 && p.ExitCli is { Visible: false } && p.FirmwareRow.IsAncestorOf(p.ExitCli), "Status's only button is the hidden exit CLI next to the firmware line");
        Check("status_compact", p.Link.Text.StartsWith("connected · realtime ") && p.Link.Text.EndsWith(" states/s") && p.FirmwareRow.Visible, $"link '{p.Link.Text}', firmware visible {p.FirmwareRow.Visible}");
        Check("status_compact", !p.Config.Visible, "config line visible without a config: " + p.Config.Text);
        _core.InjectForTest(PlaneTests.Event("config", 7.9, "{\"state\":\"applying\",\"lines\":13}"));
        Check("status_compact", p.Config.Visible && p.Config.Text.StartsWith("config: applying"), "applying: " + p.Config.Text);
        _core.InjectForTest(PlaneTests.Event("config", 9.3, "{\"state\":\"calibrating\",\"lines\":13,\"function\":\"calibrate_acc\"}"));
        _core.InjectForTest("{\"type\":\"function\",\"t\":19.35,\"name\":\"calibrate_acc\",\"phase\":\"step\",\"step\":2,\"steps\":6,\"detail\":\"inverted (iNav position 1) collected\"}");
        Check("status_compact", p.Config.Visible && p.Config.Text == "config: calibrating the accelerometer… pose 2/6 (inverted)", "calibrating: " + p.Config.Text);
        _core.InjectForTest(PlaneTests.Event("config", 40.97, "{\"state\":\"applied\",\"lines\":13,\"functions\":[\"calibrate_acc\"],\"acc_calibrated\":true}"));
        Check("status_compact", p.Config.Visible && p.Config.Text == "config: applied · ACC calibrated" && _cfgHideAt > Time.GetTicksMsec(), "applied: " + p.Config.Text);
        _cfgHideAt = 1; StatusTick(); StatusTick();
        Check("status_compact", !p.Config.Visible, "applied not hidden after its time");
        _core.InjectForTest(PlaneTests.Event("config", 8.8, "{\"state\":\"restored\",\"lines\":13,\"replay\":true}"));
        Check("status_compact", p.Config.Visible && p.Config.Text == "config: restored (saved snapshot)" && LogHas("(replay: happened before this client connected) restored"), "replayed restored: " + p.Config.Text);
        _cfgHideAt = 1; StatusTick(); StatusTick();
        Check("status_compact", !p.Config.Visible, "restored not hidden after its time");
        // aircraft_window (heli)
        var ap = _aircraftPanel!;
        UpdateStatus();
        Check("aircraft_window", _windows.IsOpen("Aircraft") && ap.GetParent()?.GetParent() == _windows["Aircraft"], "the Aircraft window is not open by default");
        var rows = ap.VisibleRows.ToList();
        Check("aircraft_window", rows.Contains("alt") && rows.Contains("speed") && rows.Contains("power") && rows.Contains("surfaces") && !rows.Contains("air") && !rows.Contains("launch"), "heli rows: " + string.Join(",", rows));
        Check("aircraft_window", ap.RowText("power").StartsWith("head ") && ap.RowText("surfaces").Contains("swash_a"), $"heli power '{ap.RowText("power")}', servos '{ap.RowText("surfaces")}'");
        _windows["Aircraft"].Visible = false;
        _core.InjectForTest(PlaneTests.Event("crash", 12.0, "{\"roll_deg\":-68.8,\"pitch_deg\":11.8,\"speed_mps\":4.40}"));
        Check("aircraft_window", ap.CrashVisible && ap.CrashText == "CRASH at t = 12.00 s: roll -68.8°, pitch +11.8°, speed 4.4 m/s" && _windows.IsOpen("Aircraft"), $"crash: banner {ap.CrashVisible} '{ap.CrashText}', window open {_windows.IsOpen("Aircraft")}");
        ap.PressReset();
        Check("aircraft_window", !ap.CrashVisible, "Reset aircraft did not clear the banner");
        if (!WindowsTests.PlaneRan && PlaneTests.Any) r["aircraft_window"].Add("the plane screen's part did not run");
        r["aircraft_window"].AddRange(WindowsTests.PlaneFails);
        // transmitter_owns_switches
        UpdateKeysHint();
        Check("transmitter_owns_switches", !All(status).Any(n => n is CheckButton || (n is Button b && b != p.ExitCli)), "a toggle / button in Status");
        Check("transmitter_owns_switches", _switchList != null && tx.IsAncestorOf(_switchList) && _switchRows.Count > 0 && _switchRows.All(x => tx.IsAncestorOf(x.Pos[0])), "the switch rows are not in the Transmitter window");
        Check("transmitter_owns_switches", !All(tx).Any(n => n is CheckButton), "a toggle (CheckButton) in the Transmitter window: the old ARM / mode / third toggles are gone (decisions 2026-10-23)");
        Check("transmitter_owns_switches", _switchRows.Select(x => x.Sw.Channel).SequenceEqual(_switchRows.Select(x => x.Sw.Channel).OrderBy(c => c)), "rows not in channel order: " + string.Join(",", _switchRows.Select(x => x.Sw.Channel)));
        Check("transmitter_owns_switches", _keysHint != null && tx.IsAncestorOf(_keysHint) && _keysHint.Text == Keybinds.Hint(_isPlane) && _keysHint.Text.Contains("S/W collective") && _keysHint.Text.Contains("switches 5 6 7 8 9"), "keyboard hints: " + _keysHint?.Text);
        foreach (var name in _windows.Names.Where(n => n != "Transmitter"))
            Check("transmitter_owns_switches", _switchRows.All(x => !_windows[name].IsAncestorOf(x.Pos[0])), $"a switch row in {name}");
        // core_menu
        RebuildBenchMenu();
        var bp = _benchPopup!;
        var items = Enumerable.Range(0, bp.ItemCount).Select(i => (Id: bp.GetItemId(i), Text: bp.GetItemText(i), Sep: bp.IsItemSeparator(i), Off: bp.IsItemDisabled(i))).ToList();
        int core = items.FindIndex(x => x.Sep && x.Text == "Core");
        var group = core < 0 ? new List<int>() : items.Skip(core + 1).TakeWhile(x => !x.Sep).Select(x => x.Id).ToList();
        Check("core_menu", group.Take(4).SequenceEqual(new[] { MenuCoreLine, MenuRestartCore, MenuBenchCheck, MenuCoreAddress }) && group.Contains(MenuClose), "Bench menu Core group: " + string.Join(" | ", items.Select(x => x.Sep ? $"--{x.Text}--" : x.Text)));
        var line = items.FirstOrDefault(x => x.Id == MenuCoreLine);
        Check("core_menu", line.Text != null && line.Text.StartsWith("Core: ") && line.Text.Contains($"{_session.Port}") && line.Off, $"Core line '{line.Text}' (read-only {line.Off})");
        Check("core_menu", items.First(x => x.Id == MenuRestartCore).Off == !_ownCore, "Restart core enabled only for the app's own core");
        Check("core_menu", !ApplyCoreAddress("no port here") && LogHas("core address must be host:port"), "a bad core address accepted");
        var statusText = p.Link.Text + p.FirmwareText + p.Sensors.Text + p.Config.Text;
        Check("core_menu", !statusText.Contains("pid") && !statusText.Contains("Restart") && !All(status).Any(n => n is LineEdit), "the core in Status: " + statusText);
        // layout_migration (screen): an old RS7 layout through a FlightWindows of its own
        var host = new Node { Name = "migration_test" };
        _ui.AddChild(host);
        var old = new BenchData { Name = "old layout" };
        foreach (var (k, v) in WindowsTests.OldLayout(false)) old.Layout[k] = v;
        var fw = new FlightWindows(host, old, (_, _) => { });
        var note = fw.MigrateAircraft();
        fw.Make("Status", new Label(), DefaultPos["Status"], DefaultSize["Status"], openByDefault: true);
        fw.Make("Aircraft", new Label(), DefaultPos["Aircraft"], DefaultSize["Aircraft"], openByDefault: true);
        fw.Make("Transmitter", new Label(), DefaultPos["Transmitter"], DefaultSize["Transmitter"], openByDefault: true);
        var want = FlightWindows.AircraftPlacement(WindowsTests.OldLayout(false), out _)!;
        Check("layout_migration", note != null && fw.IsOpen("Aircraft") && fw["Aircraft"].Position == new Vector2I(want.X, want.Y) && fw["Status"].Size.Y <= FlightWindows.CompactStatusH, $"migrated: note '{note}', Aircraft open {fw.IsOpen("Aircraft")} at {fw["Aircraft"].Position}, Status {fw["Status"].Size}");
        Check("layout_migration", !new Rect2I(fw["Aircraft"].Position, fw["Aircraft"].Size).Intersects(new Rect2I(fw["Transmitter"].Position, fw["Transmitter"].Size)), "migrated Aircraft over the Transmitter");
        var saved = new BenchData(); fw.SaveInto(saved);
        Check("layout_migration", saved.Layout.TryGetValue("Aircraft", out var sa) && sa.Open && sa.X == want.X, "the migrated layout is saved with Aircraft");
        host.QueueFree();
        return r;
    }
}
