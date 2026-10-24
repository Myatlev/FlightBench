// Self-tests of the firmware's read-only views (FB_SELFTEST=all, or modes_switches,functions_panel,topbar,osd_line,diff_view).
// Each test has a model part (pure: Modes.cs / EnvPanels.cs fed fixtures) and a screen part: the runner opens the flight
// screen once more with an in-memory RS7 bench (nothing written) connected to the local (fake) core, and the screen
// part injects the new messages (CoreClient.InjectForTest) or presses the new buttons:
//   modes_switches  fixture with 2- and 3-position channels, overlapping ranges, a lone middle range, an odd range, junk,
//                   an empty one, RotorFlight AND slots ("A & B"), touching (1300–1700 + 1700–2100) and overlapping ranges →
//                   positions, labels, µs; active boxes; the bench snapshot round trip; on the screen: an updated `modes`
//                   replaces the rows in place (no stale row); the modes came by subscription (subscribe {"modes":true} after hello), the
//                   Transmitter switches, header, a switch key cycling its channel, the active modes as Status tags,
//                   Update modes; the bench's transmitter model (TxModelScreen): static switch2 / switch3 / switch6 rows
//                   with derived ones in channel order, "—" / box labels, keys stepping 1000 → 2000 → 1000 and the six
//                   ArduPilot values, an auto channel without ranges → no row, new modes relabel a static switch;
//   functions_panel catalog → buttons, `function` progress → lines, first-start boxes, connected gating; manifest parse;
//                   on the screen: the Functions window, a click sends {"type":"function","name":…};
//   topbar          layout nodes in order (left group, centre title not clickable, Disconnect right); Settings opens over
//                   the bench and Back closes it: still connected, no disconnect in between;
//   osd_line        max7456 / msp_displayport / none / absent; on the screen: the Status sensors line from the core's
//                   hello, then a state's `osd`;
//   osd_font        on the screen: the fake core's `osd` messages name `font` rotorflight; its stream stopped (subscribe
//                   osd:false), the core's captured iNav canvas injected (font inav, frame 1026) into the OSD window, then
//                   deltas with gaps in `frame` and one without `font`, then font rotorflight mid-session: the window's panel
//                   redraws each time (its drawn version = the buffer's) in the named font, every cell from the atlas;
//                   the stream turned back on (the pure part is the `osd` self-test);
//   diff_view       the diff model (source, stale, error) + DiffView (read-only, copy, Show cached / Refresh from firmware
//                   gating, the reboot warning); on the screen: a `diff` → the window and the bench's firmware_diff, Show
//                   cached sends {"type":"diff","refresh":false}, Refresh from firmware {"type":"diff"} only when disarmed.
// The messages follow docs/protocol.md (`modes`, `diff`, `function`, `osd`); the screen parts run against
// tools/fake_core.py, which emits the same shapes; WindowsTests.cs checks them against the real core's captures.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public static class ModesTests
{
    public static readonly string[] Names = { "modes_switches", "functions_panel", "topbar", "osd_line", "diff_view", "osd_font" };
    public static bool Any => Names.Any(DevEnv.SelfTest);
    public static bool Done;

    private static void Check(List<string> f, bool ok, string what) { if (!ok) f.Add(what); }

    /// 2-position (AUX1 ARM), 3-position (AUX2 ANGLE / HORIZON), overlapping (AUX3 ANGLE + NAV LAUNCH), a lone middle
    /// range (AUX4 BEEPER), an odd range (AUX5 NAV LAUNCH 1200..1400), and junk (aux 12, empty range, unknown box).
    public const string Fixture = "{\"type\":\"modes\",\"t\":1.5,\"boxes\":[{\"id\":0,\"index\":0,\"name\":\"ARM\"},{\"id\":1,\"index\":1,\"name\":\"ANGLE\"},"
        + "{\"id\":2,\"index\":2,\"name\":\"HORIZON\"},{\"id\":13,\"index\":3,\"name\":\"BEEPER\"},{\"id\":36,\"index\":4,\"name\":\"NAV LAUNCH\"}],\"ranges\":["
        + "{\"channel\":0,\"box\":0,\"start_us\":1700,\"end_us\":2100},"
        + "{\"channel\":1,\"box\":2,\"start_us\":1700,\"end_us\":2100},{\"channel\":1,\"box\":1,\"start_us\":1300,\"end_us\":1700},"
        + "{\"channel\":2,\"box\":1,\"start_us\":1300,\"end_us\":2100},{\"channel\":2,\"box\":36,\"start_us\":1700,\"end_us\":2100},"
        + "{\"channel\":3,\"box\":13,\"start_us\":1300,\"end_us\":1700},"
        + "{\"channel\":4,\"box\":36,\"start_us\":1200,\"end_us\":1400},"
        + "{\"channel\":12,\"box\":1,\"start_us\":1300,\"end_us\":1700},{\"channel\":5,\"box\":1,\"start_us\":900,\"end_us\":900},{\"channel\":6,\"box\":99,\"start_us\":1700,\"end_us\":2100}]}";

    /// Touching / overlapping ranges: AUX1 ANGLE 1300–1700 + HORIZON 1700–2100; AUX2 ANGLE 1300–1700 + ANGLE 1700–2100;
    /// AUX3 ANGLE 1200–1800 + HORIZON 1600–2100 (iNav: ANGLE 1, HORIZON 2).
    public const string Touching = "{\"type\":\"modes\",\"t\":9.5,\"variant\":\"INAV\",\"boxes\":[{\"id\":0,\"index\":0,\"name\":\"ARM\"},{\"id\":1,\"index\":1,\"name\":\"ANGLE\"},{\"id\":2,\"index\":2,\"name\":\"HORIZON\"}],\"ranges\":["
        + "{\"slot\":0,\"channel\":0,\"rx_channel\":4,\"box\":1,\"start_us\":1300,\"end_us\":1700},{\"slot\":1,\"channel\":0,\"rx_channel\":4,\"box\":2,\"start_us\":1700,\"end_us\":2100},"
        + "{\"slot\":2,\"channel\":1,\"rx_channel\":5,\"box\":1,\"start_us\":1300,\"end_us\":1700},{\"slot\":3,\"channel\":1,\"rx_channel\":5,\"box\":1,\"start_us\":1700,\"end_us\":2100},"
        + "{\"slot\":4,\"channel\":2,\"rx_channel\":6,\"box\":1,\"start_us\":1200,\"end_us\":1800},{\"slot\":5,\"channel\":2,\"rx_channel\":6,\"box\":2,\"start_us\":1600,\"end_us\":2100}]}";

    public static ModesSnapshot? Parse(string json)
    {
        using var d = JsonDocument.Parse(json);
        return ModesSnapshot.FromMessage(d.RootElement, new DateTime(2026, 10, 1, 12, 0, 0));
    }

    public static List<string> ModesModel()
    {
        var f = new List<string>();
        var s = Parse(Fixture);
        if (s == null) { f.Add("fixture not parsed"); return f; }
        var sw = s.Switches();
        string D(DerivedSwitch x) => string.Join(" | ", x.Positions.Select(p => $"{p.Label}@{p.StartUs}-{p.EndUs}:{p.Us}"));
        Check(f, sw.Count == 6 && sw.Select(x => x.Channel).SequenceEqual(new[] { 5, 6, 7, 8, 9, 11 }), $"switch channels {string.Join(",", sw.Select(x => x.Channel))} (want 5,6,7,8,9,11: aux 12 and the empty range dropped)");
        if (sw.Count < 6) return f;
        Check(f, D(sw[0]) == "—@900-1700:1000 | ARM@1700-2100:2000", "2-position ARM: " + D(sw[0]));
        Check(f, D(sw[1]) == "—@900-1300:1000 | ANGLE@1300-1700:1500 | HORIZON@1700-2100:2000", "3-position ANGLE/HORIZON (ranges given high first): " + D(sw[1]));
        Check(f, D(sw[2]) == "—@900-1300:1000 | ANGLE@1300-1700:1500 | ANGLE+NAV LAUNCH@1700-2100:2000", "overlapping ANGLE + NAV LAUNCH: " + D(sw[2]));
        Check(f, D(sw[3]) == "—@900-1300:1000 | BEEPER@1300-1700:1500 | —@1700-2100:2000", "lone middle range: " + D(sw[3]));
        Check(f, D(sw[4]) == "—@900-1200:1000 | NAV LAUNCH@1200-1400:1300 | —@1400-2100:2000", "odd range (no 1000/1500/2000 inside: the middle): " + D(sw[4]));
        Check(f, D(sw[5]) == "—@900-1700:1000 | box 99@1700-2100:2000", "unknown box id named 'box 99': " + D(sw[5]));
        Check(f, sw[1].PositionOf(800) == 0 && sw[1].PositionOf(1299) == 0 && sw[1].PositionOf(1300) == 1 && sw[1].PositionOf(1700) == 2 && sw[1].PositionOf(2200) == 2, "PositionOf edges (start ≤ µs < end, clamped)");
        Check(f, sw[0].StartPosition(1500, driven: false) == 0 && sw[0].StartPosition(2000, driven: true) == 1 && sw[0].StartPosition(2000, driven: false) == 0, "start position: an undriven channel never starts on ARM");
        Check(f, sw[1].StartPosition(1500, driven: false) == 1, "start position of a non-ARM switch: where the channel is");
        Check(f, s.RangedBoxIds().SequenceEqual(new[] { 0, 1, 2, 36, 13, 99 }), "ranged boxes " + string.Join(",", s.RangedBoxIds()));
        // active boxes: permanent ids from fc.boxes, else fc.mode names + armed
        using (var d = JsonDocument.Parse("{\"type\":\"state\",\"fc\":{\"armed\":true,\"mode\":\"ANGLE\",\"boxes\":[0,36,36]}}"))
            Check(f, s.ActiveIds(d.RootElement)?.SequenceEqual(new[] { 0, 36 }) == true, "fc.boxes ids (wins over fc.mode, duplicates once)");
        using (var d = JsonDocument.Parse("{\"type\":\"state\",\"fc\":{\"armed\":true,\"mode\":\"ANGLE+NAV LAUNCH\"}}"))
            Check(f, s.ActiveIds(d.RootElement)?.SequenceEqual(new[] { 0, 1, 36 }) == true, "fc.mode names + armed → ids: " + string.Join(",", s.ActiveIds(d.RootElement) ?? new()));
        using (var d = JsonDocument.Parse("{\"type\":\"state\",\"t\":1}"))
            Check(f, s.ActiveIds(d.RootElement) == null, "a state without fc says nothing");
        // bench: the snapshot round trip, the static switches refreshed from it
        var bs = s.AsBenchSwitches()!;
        Check(f, bs.Count == 6 && bs[2].Channel == 7 && bs[2].Name == "ANGLE / ANGLE+NAV LAUNCH" && bs[0].Name == "ARM" && bs[2].Note!.Contains("— 900–1300"), $"bench switches: {string.Join("; ", bs.Select(b => $"ch{b.Channel} {b.Name}"))}");
        var bench = new BenchData { Name = "modes rt", Modes = s };
        var back = BenchData.Parse(JsonSerializer.Serialize(bench, Json.Files));
        Check(f, back.Modes != null && back.Modes.Switches().Select(D).SequenceEqual(sw.Select(D)) && back.Modes.T == 1.5, "bench `modes` round trip");
        Check(f, JsonSerializer.Serialize(bench, Json.Files).Contains("\"modes\": {"), "bench JSON field `modes`");
        // the real core's extra fields: rx_channel (after the RX map) picks the channel, a linked range is no position,
        // an unused slot (null µs) is skipped; a 3-position switch whose middle range starts at 1500 is not sent on its edge
        var rx = Parse("{\"type\":\"modes\",\"boxes\":[{\"id\":0,\"index\":0,\"name\":\"ARM\"},{\"id\":1,\"index\":1,\"name\":\"ANGLE\"},{\"id\":2,\"index\":2,\"name\":\"HORIZON\"},{\"id\":null,\"index\":3,\"name\":\"X\"}],\"ranges\":["
            + "{\"slot\":0,\"channel\":0,\"rc_channel\":5,\"rx_channel\":4,\"box\":0,\"start_us\":1700,\"end_us\":2100,\"logic\":\"or\"},"
            + "{\"slot\":1,\"channel\":1,\"rc_channel\":6,\"rx_channel\":7,\"box\":1,\"start_us\":1500,\"end_us\":1700},{\"slot\":2,\"channel\":1,\"rc_channel\":6,\"rx_channel\":7,\"box\":2,\"start_us\":1700,\"end_us\":2100},"
            + "{\"slot\":3,\"channel\":2,\"rx_channel\":6,\"box\":2,\"start_us\":1500,\"end_us\":2100,\"linked\":1},"
            + "{\"slot\":4,\"channel\":3,\"rx_channel\":9,\"box\":0,\"start_us\":null,\"end_us\":null}]}");
        var rsw = rx?.Switches() ?? new();
        Check(f, rsw.Select(x => x.Channel).SequenceEqual(new[] { 5, 8 }) && rsw[1].Aux == 1, "rx_channel picks the channel (ch5, ch8), linked / unused dropped: " + string.Join(",", rsw.Select(x => $"ch{x.Channel}/AUX{x.Aux + 1}")));
        if (rsw.Count == 2) Check(f, D(rsw[1]) == "—@900-1500:1000 | ANGLE@1500-1700:1600 | HORIZON@1700-2100:2000", "middle range starting at 1500: sent off its edge: " + D(rsw[1]));
        Check(f, rx?.Boxes.Count == 3, "a box without a permanent id is skipped");
        // RotorFlight AND slots: ARM on ch5 AND ch8 → "ARM & ch8" on ch5; RESCUE on ch6 AND linked to ANGLE → "RESCUE & ANGLE";
        // an AND box whose slots are all on one channel is active only where all of them cover (their intersection)
        var and = Parse("{\"type\":\"modes\",\"variant\":\"RTFL\",\"boxes\":[{\"id\":0,\"index\":0,\"name\":\"ARM\"},{\"id\":1,\"index\":1,\"name\":\"ANGLE\"},{\"id\":53,\"index\":2,\"name\":\"RESCUE\"},{\"id\":13,\"index\":3,\"name\":\"BEEPER\"}],\"ranges\":["
            + "{\"slot\":0,\"channel\":0,\"rx_channel\":4,\"box\":0,\"start_us\":1700,\"end_us\":2100,\"logic\":\"and\"},{\"slot\":1,\"channel\":3,\"rx_channel\":7,\"box\":0,\"start_us\":1500,\"end_us\":2100,\"logic\":\"and\"},"
            + "{\"slot\":2,\"channel\":1,\"rx_channel\":5,\"box\":53,\"start_us\":1700,\"end_us\":2100,\"logic\":\"and\"},{\"slot\":3,\"channel\":2,\"box\":53,\"linked\":1,\"start_us\":null,\"end_us\":null,\"logic\":\"and\"},"
            + "{\"slot\":4,\"channel\":4,\"rx_channel\":8,\"box\":13,\"start_us\":1300,\"end_us\":1900,\"logic\":\"and\"},{\"slot\":5,\"channel\":4,\"rx_channel\":8,\"box\":13,\"start_us\":1500,\"end_us\":2100,\"logic\":\"and\"}]}");
        var asw = and?.Switches() ?? new();
        Check(f, and?.Variant == "RTFL" && and.Ranges.Count == 6, $"AND fixture parsed (linked slot without µs kept): variant {and?.Variant}, {and?.Ranges.Count} ranges");
        Check(f, asw.Select(x => x.Channel).SequenceEqual(new[] { 5, 6, 8, 9 }) && D(asw[0]) == "—@900-1700:1000 | ARM & ch8@1700-2100:2000", "AND: channels " + string.Join(",", asw.Select(x => x.Channel)) + ", ch5 " + (asw.Count > 0 ? D(asw[0]) : "none"));
        if (asw.Count == 4)
        {
            Check(f, D(asw[1]) == "—@900-1700:1000 | RESCUE & ANGLE@1700-2100:2000", "AND linked: ch6 " + D(asw[1]));
            Check(f, D(asw[2]) == "—@900-1500:1000 | ARM & ch5@1500-2100:2000", "AND the other way: ch8 " + D(asw[2]));
            Check(f, D(asw[3]) == "—@900-1500:1000 | BEEPER@1500-1900:1700 | —@1900-2100:2000", "AND on one channel = intersection: ch9 " + D(asw[3]));
        }
        // the sent value always lies inside its position (and maps back to it), whatever the ranges: random layouts
        var rnd = new Random(7);
        int bad = 0; string? firstBad = null;
        for (int k = 0; k < 400; k++)
        {
            var rs = new List<ModeRange>();
            for (int j = rnd.Next(1, 6); j > 0; j--)
            {
                int a = 900 + 25 * rnd.Next(0, 48), b = 900 + 25 * rnd.Next(0, 49);
                rs.Add(new ModeRange { Channel = 0, Box = rnd.Next(0, 4), StartUs = Math.Min(a, b), EndUs = Math.Max(a, b) });
            }
            var ms = new ModesSnapshot { Boxes = new() { new() { Id = 0, Name = "ARM" }, new() { Id = 1, Name = "ANGLE" }, new() { Id = 2, Name = "HORIZON" }, new() { Id = 3, Name = "BEEPER" } }, Ranges = rs };
            foreach (var x in ms.Switches())
                for (int i = 0; i < x.Positions.Count; i++)
                {
                    var q = x.Positions[i];
                    bool ok = q.Us >= q.StartUs && q.Us < q.EndUs && x.PositionOf(q.Us) == i && (i == 0 || !x.Positions[i - 1].BoxIds.SequenceEqual(q.BoxIds));
                    if (!ok) { bad++; firstBad ??= string.Join(" ", rs.Select(r => $"{r.Box}:{r.StartUs}-{r.EndUs}")) + " → " + D(x); }
                }
        }
        Check(f, bad == 0, $"random layouts: {bad} position(s) whose value is outside it or not merged: {firstBad}");
        // touching and overlapping ranges (the product owner's 2026-10-01 test): 1300–1700 + 1700–2100 of two boxes = three
        // positions; of one box = one position; a partial overlap = four; start ≤ µs < end at the shared edge
        var touch = Parse(Touching);
        var tsw = touch?.Switches() ?? new();
        string L(DerivedSwitch x) => string.Join(" | ", x.Positions.Select(p => $"{p.Label}@{p.StartUs}-{p.EndUs}"));
        bool Inside(DerivedSwitch x) => x.Positions.Select((p, i) => p.Us >= p.StartUs && p.Us < p.EndUs && x.PositionOf(p.Us) == i).All(ok => ok);
        Check(f, tsw.Count == 3, $"touching fixture: {tsw.Count} switches (want 3)");
        if (tsw.Count == 3)
        {
            Check(f, L(tsw[0]) == "—@900-1300 | ANGLE@1300-1700 | HORIZON@1700-2100" && Inside(tsw[0]), "touching, two boxes: " + D(tsw[0]));
            Check(f, tsw[0].PositionOf(1699) == 1 && tsw[0].PositionOf(1700) == 2, "the shared edge 1700 belongs to the upper range");
            Check(f, L(tsw[1]) == "—@900-1300 | ANGLE@1300-2100" && Inside(tsw[1]), "touching, one box: merged: " + D(tsw[1]));
            Check(f, L(tsw[2]) == "—@900-1200 | ANGLE@1200-1600 | ANGLE+HORIZON@1600-1800 | HORIZON@1800-2100" && Inside(tsw[2]), "partial overlap: " + D(tsw[2]));
        }
        // empty and bad
        var e = Parse("{\"type\":\"modes\",\"boxes\":[{\"id\":0,\"index\":0,\"name\":\"ARM\"}],\"ranges\":[]}");
        Check(f, e != null && e.Switches().Count == 0 && e.AsBenchSwitches() == null && e.RangedBoxIds().Count == 0, "empty ranges → no switch");
        Check(f, Parse("{\"type\":\"modes\",\"ranges\":[]}") == null && Parse("{\"type\":\"modes\",\"error\":\"cli\"}") == null, "a message without boxes (e.g. the CLI error) → null");
        return f;
    }

    public const string Catalog = "[{\"name\":\"calibrate_acc\",\"label\":\"Calibrate accelerometer (6 positions)\",\"auto_first_start\":true,\"params\":{}},"
        + "{\"name\":\"calibrate_gyro\",\"label\":\"Calibrate gyro\",\"auto_first_start\":false},{\"name\":\"launch\",\"params\":{\"speed_kmh\":50,\"duration_s\":0.5,\"pitch_deg\":10}}]";

    public static List<ManifestFunction> CatalogList()
    {
        var m = Manifest.Parse("{\"aircraft\":[{\"id\":\"trainer\",\"functions\":" + Catalog + "}],\"boards\":[{\"id\":\"b\",\"functions\":[{\"name\":\"launch\"},{\"name\":\"board_fn\"}]}],\"functions\":[{\"name\":\"top_fn\"},{\"name\":\"\"}]}", "test", out _)!;
        return m.FunctionsFor("trainer");
    }

    public static List<string> FunctionsModel()
    {
        var f = new List<string>();
        var cat = CatalogList();
        Check(f, cat.Select(c => c.Name).SequenceEqual(new[] { "calibrate_acc", "calibrate_gyro", "launch" }), "FunctionsFor = the aircraft's only (board / top-level lists are not the core's: ignored): " + string.Join(",", cat.Select(c => c.Name)));
        Check(f, cat[0].AutoFirstStart && !cat[1].AutoFirstStart && cat[2].DisplayLabel == "launch" && cat[2].ParamsText == "speed_kmh 50, duration_s 0.5, pitch_deg 10", "catalog fields: " + cat[2].ParamsText);
        var sent = new List<string>();
        var p = new FunctionsPanel { Send = m => { sent.Add(JsonSerializer.Serialize(m)); return true; } };
        var first = new List<string>();
        p.FirstStartChanged += (n, on) => first.Add($"{n}={on}");
        p.SetCatalog(cat.Take(3).ToList(), new Dictionary<string, bool> { ["calibrate_gyro"] = true }, "test");
        Check(f, p.RunButton("calibrate_acc")?.Text == "Calibrate accelerometer (6 positions)" && p.RunButton("launch")?.Text == "launch" && p.RunButton("board_fn") == null, "buttons from the catalog");
        Check(f, p.RunButton("calibrate_acc")!.Disabled, "buttons disabled before connected");
        p.SetConnected(true);
        Check(f, !p.RunButton("calibrate_acc")!.Disabled, "buttons enabled when connected");
        Check(f, p.FirstStartBox("calibrate_acc")!.ButtonPressed && p.FirstStartBox("calibrate_gyro")!.ButtonPressed && !p.FirstStartBox("launch")!.ButtonPressed, "first-start boxes: catalog default, bench override");
        p.FirstStartBox("calibrate_acc")!.ButtonPressed = false;
        Check(f, first.SequenceEqual(new[] { "calibrate_acc=False" }), "first-start change reported: " + string.Join(",", first));
        p.RunButton("calibrate_acc")!.EmitSignal(BaseButton.SignalName.Pressed);
        Check(f, sent.SequenceEqual(new[] { "{\"type\":\"function\",\"name\":\"calibrate_acc\"}" }), "click sends: " + string.Join(" | ", sent));
        Check(f, p.LineText("calibrate_acc") == "not run in this session", "line before any progress: " + p.LineText("calibrate_acc"));
        string On(string json) { using var d = JsonDocument.Parse(json); return p.OnFunction(d.RootElement) ?? "(null)"; }
        On("{\"type\":\"function\",\"name\":\"calibrate_acc\",\"phase\":\"start\",\"steps\":6,\"t\":10.0}");
        Check(f, p.LineText("calibrate_acc") == "running… 0/6 (t = 10.00 s)", "start: " + p.LineText("calibrate_acc"));
        Check(f, p.RunButton("calibrate_acc")!.Disabled && p.RunButton("launch")!.Disabled && !p.RunFunction("launch") && sent.Count == 1, "buttons disabled while a function runs");
        var log = On("{\"type\":\"function\",\"name\":\"calibrate_acc\",\"phase\":\"step\",\"step\":3,\"steps\":6,\"detail\":\"left side down\",\"t\":12.5}");
        Check(f, p.LineText("calibrate_acc") == "step 3/6: left side down (t = 12.50 s)" && log == "calibrate_acc: step 3/6: left side down (t = 12.50 s)", "step: " + p.LineText("calibrate_acc") + " / " + log);
        On("{\"type\":\"function\",\"name\":\"calibrate_acc\",\"phase\":\"done\",\"step\":6,\"steps\":6,\"detail\":\"saved\",\"t\":20}");
        Check(f, p.LineText("calibrate_acc") == "done: saved (t = 20.00 s)", "done: " + p.LineText("calibrate_acc"));
        Check(f, !p.RunButton("calibrate_acc")!.Disabled && !p.RunButton("launch")!.Disabled, "buttons free again after done");
        On("{\"type\":\"function\",\"name\":\"calibrate_gyro\",\"phase\":\"failed\",\"detail\":\"moved\"}");
        Check(f, p.LineText("calibrate_gyro") == "FAILED: moved", "failed: " + p.LineText("calibrate_gyro"));
        Check(f, On("{\"type\":\"function\",\"name\":\"other\",\"phase\":\"done\"}") == "other: done" && On("{\"type\":\"function\"}") == "(null)", "a function outside the catalog logged only; no name ignored");
        p.ClearProgress();
        Check(f, p.LineText("calibrate_acc") == "not run in this session", "ClearProgress (new connection)");
        p.SetCatalog(new List<ManifestFunction>(), null, "fixture");
        Check(f, p.InfoText.Contains("no environment functions"), "empty catalog says so: " + p.InfoText);
        p.Free();
        return f;
    }

    public static List<string> OsdModel()
    {
        var f = new List<string>();
        string? L(string json) { using var d = JsonDocument.Parse(json); return OsdLine.From(d.RootElement); }
        Check(f, L("{\"type\":\"hello\",\"osd\":{\"kind\":\"max7456\",\"cols\":30,\"rows\":16,\"video\":false}}") == "OSD: MAX7456 30×16 (no video in FlightBench)", "max7456");
        Check(f, L("{\"type\":\"hello\",\"osd\":{\"kind\":\"msp_displayport\",\"cols\":50,\"rows\":18,\"video\":true}}") == "OSD: MSP DisplayPort 50×18", "msp_displayport");
        Check(f, L("{\"type\":\"hello\",\"osd\":null}") == "OSD: none" && L("{\"type\":\"status\",\"osd\":{\"kind\":\"none\"}}") == "OSD: none", "none");
        Check(f, L("{\"type\":\"hello\"}") == null, "absent → no line (older core)");
        Check(f, L("{\"type\":\"state\",\"osd\":{\"kind\":\"max7456\",\"cols\":30,\"rows\":13,\"video_system\":\"NTSC\",\"video\":false}}") == "OSD: MAX7456 30×13 (no video in FlightBench)", "state.osd (PAL/NTSC rows)");
        Check(f, L("{\"type\":\"state\",\"fc\":{\"osd\":{\"kind\":\"max7456\",\"cols\":30,\"rows\":13}}}") == null, "fc.osd is not the core's: no line");
        return f;
    }

    public static List<string> DiffModel()
    {
        var f = new List<string>();
        FirmwareDiff? D(string json) { using var d = JsonDocument.Parse(json); return FirmwareDiff.FromMessage(d.RootElement, new DateTime(2026, 10, 1, 14, 2, 0)); }
        var d = D("{\"type\":\"diff\",\"t\":41.2,\"text\":\"# diff all\\r\\nset a = 1\\r\\n\\r\\nset b = 2\\r\\n\"}")!;
        Check(f, d != null && d.Text == "# diff all\nset a = 1\n\nset b = 2\n" && !d.Stale, "diff text (CRLF → LF)");
        Check(f, d!.Header(false) == "diff all from the firmware (t = 41.20 s, 2026-10-01 14:02), 3 lines", "header: " + d.Header(false));
        var fs = D("{\"type\":\"diff\",\"t\":40.71,\"text\":\"x\",\"source\":\"first_start\",\"at\":39.91}")!;
        Check(f, fs.Header(false) == "diff all from the firmware (first start, t = 39.91 s, 2026-10-01 14:02), 1 lines" && fs.Source == "first_start", "source in the header: " + fs.Header(false));
        var er = D("{\"type\":\"diff\",\"text\":\"x\",\"stale\":true,\"source\":\"request\",\"at\":8.9,\"error\":\"cli\"}")!;
        Check(f, er.Error == "cli" && er.Header(false).Contains("STALE (cli)"), "refused with the last text: " + er.Header(false));
        var st = D("{\"type\":\"diff\",\"text\":\"x\",\"stale\":true}")!;
        Check(f, st.Stale && st.Header(true).StartsWith("diff all from the bench (last seen 2026-10-01 14:02), 1 lines — STALE"), "stale header: " + st.Header(true));
        Check(f, D("{\"type\":\"diff\"}") == null, "no text → null");
        var b = new BenchData { Name = "diff rt", FirmwareDiff = d };
        var json = JsonSerializer.Serialize(b, Json.Files);
        var back = BenchData.Parse(json);
        Check(f, json.Contains("\"firmware_diff\": {") && back.FirmwareDiff?.Text == d.Text && back.FirmwareDiff.T == 41.2, "bench firmware_diff round trip");
        Check(f, !JsonSerializer.Serialize(new BenchData(), Json.Files).Contains("firmware_diff"), "no diff → no field");
        var v = new DiffView();
        int refresh = 0, cached = 0; v.RefreshRequested += fw => { if (fw) refresh++; else cached++; };
        Check(f, v.RefreshDisabled && v.CachedDisabled && !v.Editable, "before: refresh off, read-only");
        v.Show(d, fromBench: false);
        Check(f, v.BodyText == d.Text && v.HeaderText == d.Header(false) && !v.Editable, "shown read-only");
        v.SetRefreshState(true, false); v.PressRefresh(); v.PressCached();
        Check(f, v.RefreshTooltip.Contains(DiffView.RebootWarning), "the reboot warning on Refresh from firmware: " + v.RefreshTooltip);
        v.SetRefreshState(true, true); v.PressRefresh(); v.PressCached();
        Check(f, v.RefreshTooltip.Contains("armed"), "armed: " + v.RefreshTooltip);
        v.SetRefreshState(false, false); v.PressRefresh(); v.PressCached();
        v.SetRefreshState(true, null); v.PressRefresh();
        Check(f, refresh == 1, $"refresh from firmware only when connected + disarmed: {refresh} (want 1)");
        Check(f, cached == 2, $"show cached whenever connected (armed too: the firmware is not touched): {cached} (want 2)");
        v.ShowRefusal("calibrating");
        Check(f, v.RefusalText.Contains("calibrating"), "refusal line: " + v.RefusalText);
        v.Show(d, fromBench: false);
        Check(f, v.RefusalText == "", "a diff without error clears the refusal line");
        v.Copy();
        Check(f, v.LastCopied == d.Text, "copy");
        v.Free();
        return f;
    }
}

public partial class Flight
{
    /// Set by the self-test runner before it opens the screen for the modes tests.
    public static bool ModesTest;
    private bool _mtDone;
    private int _mtFrame;
    private ulong _mtWait;
    private readonly List<string> _mtSent = new();
    private readonly Dictionary<string, List<string>> _mtFails = ModesTests.Names.ToDictionary(n => n, _ => new List<string>());
    private string[]? _mtSources;
    private Dictionary<string, string>? _mtKeys;
    private int _mtDisc;
    private int _mtCh6Before;
    private bool _mtOsdWasOpen;
    private int[,]? _mtOsdWant;
    private int _mtOsdFrame;
    private ulong _mtOsdHold;

    private void ModesSelfTest()
    {
        const string P = "[modes-selftest] ";
        void Check(string test, bool ok, string what) { if (!ok) _mtFails[test].Add(what); }
        if (_mtFrame == 0)
        {
            if (_mtWait == 0) _mtWait = Time.GetTicksMsec() + 8000;
            // the fake core's hello (osd) and its own modes (sent only because the app subscribed) / diff first
            if ((!_session.Connected || _modes == null || _osdText == null) && Time.GetTicksMsec() < _mtWait) return;
            _core.Sending += line => { if (line.Contains("\"type\":\"function\"") || line.Contains("\"type\":\"modes\"") || line.Contains("\"type\":\"diff\"")) _mtSent.Add(line); };
            // this run's channel sources / binds: ch5..ch9 driven by the app, switch keys on their defaults (restored at the end; settings not saved)
            var t = AppSettings.Current.Transmitter;
            _mtSources = (string[])t.Source!.Clone();
            for (int ch = 4; ch <= 8; ch++) t.Source[ch] = ch <= 6 ? "ui" : "none";
            _mtKeys = new Dictionary<string, string>(AppSettings.Current.Keys);
            for (int n = 1; n <= Keybinds.SwitchKeys; n++) AppSettings.Current.Keys.Remove(Keybinds.SwitchId(n));
            GD.Print(P + $"bench '{_bench.Name}', connected {_session.Connected}, fake-core modes {(_modes != null ? _switches.Count + " switch(es)" : "none")}, osd '{_osdText}'");
        }
        if (_mtFrame == 10 && Time.GetTicksMsec() < _mtOsdHold) return;     // osd_font: the fake core's last osd messages in flight land first
        switch (++_mtFrame)
        {
            case 1:
            {
                // osd_line (screen): the core's hello (the fake core: MSP DisplayPort 50×18), then a state's osd
                UpdateStatus();
                var sens = _statusPanel!.Sensors;
                Check("osd_line", sens.Visible && sens.Text.StartsWith("OSD: MSP DisplayPort 50×18"), $"Status sensors line from the core's hello: visible {sens.Visible}, '{sens.Text}'");
                _core.InjectForTest("{\"type\":\"state\",\"t\":1.9,\"osd\":{\"kind\":\"max7456\",\"cols\":30,\"rows\":16,\"video_system\":\"PAL\",\"video\":false},\"fc\":{\"msp\":true,\"baro_alt_m\":1.25}}");
                UpdateStatus();
                Check("osd_line", sens.Text == "OSD: MAX7456 30×16 (no video in FlightBench) · baro 1.25 m", $"sensors line after a state: '{sens.Text}'");
                // modes_switches (screen): the fake core's modes arrived — it sends them only to a subscriber (header, bench); then the fixture
                Check("modes_switches", _modesLive && _bench.Modes != null && (_modesHeader?.Text.StartsWith("modes: from firmware (t = ") ?? false), $"the fake core's modes: live {_modesLive}, header '{_modesHeader?.Text}'");
                Check("modes_switches", LogHas("modes: subscribed"), "the app did not subscribe to modes after hello (no 'modes: subscribed' from the fake core)");
                _core.InjectForTest(ModesTests.Fixture);
                Check("modes_switches", _switches.Count == 6 && _switchRows.Count == 6, $"Transmitter switch rows {_switchRows.Count} (want 6)");
                if (_switchRows.Count >= 3)
                {
                    Check("modes_switches", string.Join("|", _switchRows[2].Pos.Select(b => b.Text)) == "—|ANGLE|ANGLE+NAV LAUNCH", "ch7 buttons " + string.Join("|", _switchRows[2].Pos.Select(b => b.Text)));
                    Check("modes_switches", _switchRows[0].Key.Text == "[5]" && _switchRows[1].Key.Text == "[6]" && _switchRows[4].Key.Text == "[9]" && _switchRows[5].Key.Text == "", "switch keys 5..9: " + string.Join(",", _switchRows.Select(r => r.Key.Text)));
                    Check("modes_switches", !_switchRows[0].Pos.Any(b => b.Disabled) && !_switchRows[3].Pos.Any(b => b.Disabled), "ui / none channels are driven (buttons enabled)");
                }
                Check("modes_switches", _modesHeader!.Text == "modes: from firmware (t = 1.50 s)", "header: " + _modesHeader.Text);
                Check("modes_switches", !_switches.Any(x => x.Static) && _bench.Transmitter is { Count: 0 }, "the in-memory bench is all auto: every row derived from the modes");
                Check("modes_switches", _bench.Modes?.T == 1.5 && _bench.Switches?.Count == 6 && _bench.Switches[1].Name == "ANGLE / HORIZON", "bench snapshot + static switches refreshed: " + string.Join("; ", _bench.Switches?.Select(s => $"ch{s.Channel} {s.Name}") ?? new[] { "none" }));
                Check("modes_switches", _rc.Override[4] == 1000, $"ch5 (ARM switch) starts off ARM: override {_rc.Override[4]}");
                _mtCh6Before = _switches[1].PositionOf(_rc.Override[5]);
                GetTree().Root.PushInput(PtKey(Keybinds.Bind(Keybinds.SwitchId(2))));
                break;
            }
            case 4:
            {
                int now = _switches.Count > 1 ? _switches[1].PositionOf(_rc.ChannelsUs[5]) : -1;
                Check("modes_switches", now == (_mtCh6Before + 1) % 3 && _switchRows[1].Pos[now].ButtonPressed, $"switch_2 key: ch6 position {_mtCh6Before} → {now} (ch {_rc.ChannelsUs[5]})");
                _switchRows[2].Pos[2].EmitSignal(BaseButton.SignalName.Pressed);          // click ANGLE+NAV LAUNCH on ch7
                Check("modes_switches", _rc.Override[6] == 2000, $"click on ch7 ANGLE+NAV LAUNCH: override {_rc.Override[6]}");
                // the active modes in Status: text tags named from fc.boxes through the modes (a state without fc.mode)
                _core.InjectForTest("{\"type\":\"state\",\"t\":2.0,\"fc\":{\"msp\":true,\"armed\":false,\"boxes\":[1,36],\"arming_flags\":[],\"cpu_pct\":7}}");
                UpdateStatus();
                Check("modes_switches", _statusPanel!.FirmwareText == "disarmed · [ANGLE] [NAV LAUNCH] · cpu 7%" && _statusPanel.TagCount == 2, $"Status firmware line: '{_statusPanel.FirmwareText}' ({_statusPanel.TagCount} tags)");
                _updateModesBtn?.EmitSignal(BaseButton.SignalName.Pressed);
                // functions_panel (screen)
                Check("functions_panel", _windows.TryGet("Functions", out _) && _windows.TryGet("Config", out _), "Functions / Config windows exist");
                Check("functions_panel", _functions != null && _functions.InfoText.Contains("no environment functions"), $"the fixture manifest has no functions: '{_functions?.InfoText}'");
                _functions!.SetCatalog(ModesTests.CatalogList().Take(3).ToList(), _bench.FirstStartFunctions, "test");
                _functions.SetConnected(_session.Connected);
                _functions.RunButton("calibrate_gyro")?.EmitSignal(BaseButton.SignalName.Pressed);
                _core.InjectForTest("{\"type\":\"function\",\"name\":\"calibrate_acc\",\"phase\":\"step\",\"step\":2,\"steps\":6,\"detail\":\"inverted\",\"t\":5}");
                Check("functions_panel", _functions.LineText("calibrate_acc") == "step 2/6: inverted (t = 5.00 s)", "injected progress: " + _functions.LineText("calibrate_acc"));
                _functions.FirstStartBox("launch")!.ButtonPressed = true;
                Check("functions_panel", _bench.FirstStartFunctions?.GetValueOrDefault("launch") == true, "first-start box → bench");
                // diff_view (screen): the fake core's diff, then an injected one; refresh gating
                Check("diff_view", _bench.FirmwareDiff != null && (_diffView?.BodyText.Contains("# diff all") ?? false), "the fake core's diff in the window / bench");
                _core.InjectForTest("{\"type\":\"diff\",\"t\":3.25,\"text\":\"# diff all\\nset selftest = 1\\n\"}");
                Check("diff_view", _diffView!.BodyText == "# diff all\nset selftest = 1\n" && _bench.FirmwareDiff?.T == 3.25 && !_diffView.Editable, $"injected diff: '{_diffView.BodyText}'");
                _core.InjectForTest("{\"type\":\"state\",\"fc\":{\"armed\":false}}");
                _diffView.SetRefreshState(_session.Connected, _fcArmed);
                _diffView.PressRefresh();                                                // confirmed: {"type":"diff"}
                _diffView.PressCached();                                                 // {"type":"diff","refresh":false}
                _core.InjectForTest("{\"type\":\"state\",\"fc\":{\"armed\":true}}");
                _diffView.SetRefreshState(_session.Connected, _fcArmed);
                _diffView.PressRefresh();
                RequestDiff(true);                                                       // armed: refused and logged
                _core.InjectForTest("{\"type\":\"diff\",\"t\":4.0,\"text\":null,\"stale\":true,\"source\":null,\"at\":null,\"error\":\"armed\"}");
                Check("diff_view", _diffView.RefusalText.Contains("armed") && _bench.FirmwareDiff?.T == 3.25, $"a refusal without text: line '{_diffView.RefusalText}', the kept diff stays");
                break;
            }
            case 5:
                TxModelScreen(Check);
                break;
            case 6:
            {
                Check("modes_switches", _mtSent.Count(l => l == "{\"type\":\"modes\"}") == 1, "Update modes sent: " + string.Join(" | ", _mtSent));
                Check("functions_panel", _mtSent.Count(l => l == "{\"type\":\"function\",\"name\":\"calibrate_gyro\"}") == 1, "function click sent: " + string.Join(" | ", _mtSent));
                Check("diff_view", _mtSent.Count(l => l == "{\"type\":\"diff\"}") == 1 && _mtSent.Count(l => l == "{\"type\":\"diff\",\"refresh\":false}") == 1 && LogHas("armed — Refresh from firmware reboots it"), "refresh sent once (disarmed), cached once, refused armed: " + string.Join(" | ", _mtSent));
                // an updated `modes` replaces the switches in place: touching / overlapping ranges, then the same channels changed
                string Row(int i) => i < _switchRows.Count ? string.Join("|", _switchRows[i].Pos.Select(b => b.Text)) : "";
                _core.InjectForTest(ModesTests.Touching);
                Check("modes_switches", _switchRows.Count == 3 && _switchList!.GetChildCount() == 3 && Row(0) == "—|ANGLE|HORIZON" && Row(1) == "—|ANGLE" && Row(2) == "—|ANGLE|ANGLE+HORIZON|HORIZON",
                      $"touching / overlapping modes on screen: {_switchRows.Count} rows ({_switchList?.GetChildCount()} nodes): {Row(0)} ; {Row(1)} ; {Row(2)}");
                Check("modes_switches", _rc.Override[7] == 0 && _rc.Override[8] == 0, $"channels without a switch any more are released: ch8 {_rc.Override[7]}, ch9 {_rc.Override[8]}");
                _core.InjectForTest(ModesTests.Touching.Replace("\"box\":2,\"start_us\":1700,\"end_us\":2100},{\"slot\":2", "\"box\":0,\"start_us\":1700,\"end_us\":2100},{\"slot\":2").Replace("\"t\":9.5", "\"t\":9.9"));
                Check("modes_switches", _switchRows.Count == 3 && _switchList!.GetChildCount() == 3 && Row(0) == "—|ANGLE|ARM" && _modesHeader!.Text == "modes: from firmware (t = 9.90 s)",
                      $"an updated modes message: rows {_switchRows.Count} ({_switchList?.GetChildCount()} nodes), ch5 {Row(0)}, header '{_modesHeader?.Text}'");
                _core.InjectForTest(ModesTests.Fixture);                                   // back to the fixture for the checks that follow
                // topbar: the nodes in order
                var names = _topBar?.GetChildren().Select(c => c.Name.ToString()).Where(n => !n.StartsWith("@")).ToList() ?? new();
                Check("topbar", names.SequenceEqual(new[] { "menu", "bench", "windows", "view", "settings", "cfg_address", "boot_hint", "title", "connect" }), "top bar order: " + string.Join(",", names));
                var title = _topBar?.GetNodeOrNull<Label>("title");
                Check("topbar", title != null && title.MouseFilter == Control.MouseFilterEnum.Ignore && title.Text == $"{_bench.Name}  ·  {_bench.Board} / {_bench.Aircraft}", $"centre title: '{title?.Text}', mouse {title?.MouseFilter}");
                Check("topbar", _connectBtn?.Text == "Disconnect", "right: " + _connectBtn?.Text);
                _mtDisc = _disconnects;
                _settingsBtn!.EmitSignal(BaseButton.SignalName.Pressed);
                break;
            }
            case 8:
            {
                // osd_font (screen): the fake core's own messages name the font; stop its stream so the injected frames stay alone
                Check("osd_font", _osd.FontSaid == OsdFont.Rotorflight && _osd.Font == OsdFont.Rotorflight, $"the fake core's osd messages: font said '{_osd.FontSaid}', drawn with '{_osd.Font}'");
                _core.Send(new Dictionary<string, object> { ["type"] = "subscribe", ["osd"] = false });
                _mtOsdHold = Time.GetTicksMsec() + 400;
                _mtOsdWasOpen = _windows["OSD"].Visible;
                _windows["OSD"].Visible = true;
                break;
            }
            case 11:
            {
                var fx = SelfTests.LoadInavCanvas();
                if (fx is not { } canvas) { Check("osd_font", false, $"fixture {SelfTests.InavCanvasFixture} missing"); break; }
                _mtOsdWant = (int[,])canvas.Cells.Clone(); _mtOsdFrame = canvas.Frame;
                _core.InjectForTest(SelfTests.OsdMessage(SelfTests.CanvasCells(canvas.Cells), OsdFont.Inav, canvas.Frame, full: true));
                Check("osd_font", _osd.Font == OsdFont.Inav && _osd.GlyphAt(11, 25) == SelfTests.SymAhHEnd && _osd.GlyphAt(9, 25) == SelfTests.SymAhChCenter, $"injected iNav canvas: font {_osd.Font}, (11,25) 0x{_osd.GlyphAt(11, 25):X3}");
                break;
            }
            case 13:
            {
                var p = _osdWindowPanel!;
                int nonBlank = 0;
                if (_mtOsdWant != null) foreach (var x in _mtOsdWant) if (x != 0x20) nonBlank++;
                Check("osd_font", p.DrawnVersion == _osd.Version && p.DrawnFont == OsdFont.Inav && p.DrawnAtlas == nonBlank && p.DrawnFallback == 0,
                      $"the OSD window drew the iNav canvas: drawn v{p.DrawnVersion} / buffer v{_osd.Version}, font {p.DrawnFont}, atlas {p.DrawnAtlas} (want {nonBlank}), fallback {p.DrawnFallback}");
                // gaps: +5 (the horizon one row up), then +31 without `font` (the clock)
                var d1 = new List<(int, int, int)>();
                for (int c = 20; c <= 30; c++) { d1.Add((11, c, 0x20)); d1.Add((10, c, 0x150)); }
                _core.InjectForTest(SelfTests.OsdMessage(d1, OsdFont.Inav, _mtOsdFrame + 5, full: false));
                _core.InjectForTest(SelfTests.OsdMessage(new List<(int, int, int)> { (16, 45, '3'), (16, 46, '0') }, null, _mtOsdFrame + 31, full: false));
                break;
            }
            case 15:
            {
                var p = _osdWindowPanel!;
                Check("osd_font", _osd.GlyphAt(10, 25) == 0x150 && _osd.GlyphAt(11, 25) == 0x20 && _osd.RowText(16).Contains("00:30") && _osd.Font == OsdFont.Inav,
                      $"deltas after gaps applied: (10,25) 0x{_osd.GlyphAt(10, 25):X3}, row 16 '{_osd.RowText(16).Trim()}', font {_osd.Font}");
                Check("osd_font", p.DrawnVersion == _osd.Version && p.DrawnFont == OsdFont.Inav, $"redrawn after the gaps: drawn v{p.DrawnVersion} / v{_osd.Version}");
                _core.InjectForTest($"{{\"type\":\"osd\",\"frame\":{_mtOsdFrame + 40},\"clear\":false,\"link\":true,\"font\":\"rotorflight\",\"cells\":[]}}");
                break;
            }
            case 17:
            {
                var p = _osdWindowPanel!;
                Check("osd_font", _osd.Font == OsdFont.Rotorflight && p.DrawnFont == OsdFont.Rotorflight && p.DrawnVersion == _osd.Version && p.DrawnFallback == 0 && _osd.GlyphAt(10, 25) == 0x50,
                      $"font → rotorflight mid-session re-rendered: drawn font {p.DrawnFont}, v{p.DrawnVersion} / v{_osd.Version}, fallback {p.DrawnFallback}");
                _windows["OSD"].Visible = _mtOsdWasOpen;
                _core.Send(new Dictionary<string, object> { ["type"] = "subscribe", ["osd"] = true });
                break;
            }
            case 20:
            {
                var page = _settingsWin?.GetChildren().OfType<Settings>().FirstOrDefault();
                Check("topbar", SettingsOpen && _settingsWin!.Visible && page != null && page.Overlay, $"Settings over the bench: open {SettingsOpen}, page {page != null}");
                var back = page?.FindChildren("*", nameof(Button), true, false).OfType<Button>().FirstOrDefault(b => b.Text == "< Back to the bench");
                Check("topbar", back != null, "Settings' Back to the bench button");
                back?.EmitSignal(BaseButton.SignalName.Pressed);
                break;
            }
            case 30:
            {
                Check("topbar", !SettingsOpen && _session.Connected && _disconnects == _mtDisc, $"after Back: open {SettingsOpen}, connected {_session.Connected}, disconnects {_disconnects - _mtDisc}");
                Check("topbar", GetTree().CurrentScene == this, "still on the flight screen");
                Check("topbar", !_rc.KeysOff, "keyboard sticks back after Back");
                // again, closed with Escape this time; the keyboard sticks are off while it is open
                _settingsBtn!.EmitSignal(BaseButton.SignalName.Pressed);
                break;
            }
            case 40:
            {
                Check("topbar", SettingsOpen && _rc.KeysOff, $"reopened: open {SettingsOpen}, keyboard sticks off {_rc.KeysOff}");
                _settingsWin?.PushInput(new InputEventKey { Keycode = Key.Escape, PhysicalKeycode = Key.Escape, Pressed = true });
                break;
            }
            case 50:
            {
                Check("topbar", !SettingsOpen && _session.Connected && _disconnects == _mtDisc, $"after Escape: open {SettingsOpen}, connected {_session.Connected}, disconnects {_disconnects - _mtDisc}");
                var r = new Dictionary<string, List<string>>
                {
                    ["modes_switches"] = ModesTests.ModesModel().Concat(_mtFails["modes_switches"]).ToList(),
                    ["functions_panel"] = ModesTests.FunctionsModel().Concat(_mtFails["functions_panel"]).ToList(),
                    ["topbar"] = _mtFails["topbar"],
                    ["osd_line"] = ModesTests.OsdModel().Concat(_mtFails["osd_line"]).ToList(),
                    ["diff_view"] = ModesTests.DiffModel().Concat(_mtFails["diff_view"]).ToList(),
                    ["osd_font"] = _mtFails["osd_font"],
                };
                var detail = new Dictionary<string, string>
                {
                    ["modes_switches"] = "fixture: 2-pos ARM, 3-pos ANGLE/HORIZON, overlapping ANGLE+NAV LAUNCH, lone middle, odd range (middle µs), rx_channel / linked / null slots, a 1500-edge middle range sent off its edge, AND slots (ARM & ch8, RESCUE & ANGLE, one-channel intersection), 400 random layouts (value inside its position), touching 1300–1700 / 1700–2100 (two boxes: 3 positions, the edge to the upper; one box: merged), a partial overlap (4 positions), junk dropped, empty → none; active ids from fc.boxes / fc.mode; bench snapshot round trip; screen (fake core): modes by subscription, header from firmware, 6 rows with keys 5..9, key cycles ch6, click drives ch7, active modes as Status tags, Update modes sent, touching / overlapping modes and then an updated message replace the rows in place (no stale row, released channels); bench transmitter model on the screen: ch5 derived ARM, ch6 switch2 throttle (— | —, key 1000 → 2000 → 1000), ch7 switch3 on ANGLE / HORIZON ranges (1000 / 1500 / 2000 labelled), a pick forgotten when its row goes away (ch6 back at its start, not on), ch8 switch6 (1165…1815 by key), ch9 auto without ranges (no row), ch10 derived BEEPER on key 5 of 5; header names the bench switches; ranges moved onto ch6 relabel it",
                    ["functions_panel"] = "catalog = the aircraft's functions (board / top-level lists ignored), buttons gated on connected and on a running function, click sends function, start/step/done/failed lines, first-start default + bench override; screen: windows exist, fixture manifest has none, injected progress, click sent, first-start → bench",
                    ["topbar"] = "menu, bench, windows, view, settings, cfg_address | title (mouse ignored) | connect=Disconnect; Settings opened over the bench and closed with Back, then with Escape: connected throughout, 0 disconnects, keyboard sticks off while open",
                    ["osd_line"] = "max7456 30×16 (no video), MSP DisplayPort 50×18, none, absent → no line, state.osd rows, fc.osd ignored; screen: the Status sensors line from hello (fake core), then a state's osd + baro",
                    ["osd_font"] = "screen (fake core): its osd messages say font rotorflight; stream stopped; the captured iNav canvas (frame 1026) injected → the OSD window drew it in iNav's font, every cell from the atlas; deltas at frame +5 (horizon moved) and +31 without font → applied, redrawn, font kept; font rotorflight mid-session → the kept canvas redrawn in RotorFlight's font; stream back on",
                    ["diff_view"] = "CRLF → LF, header with source, stale (+ error), bench firmware_diff round trip, read-only view, Show cached when connected, Refresh from firmware only connected + disarmed with the reboot warning, refusal line, copy; screen: fake-core diff, injected diff → bench, refresh sent once + cached once, refused armed, a text-less refusal keeps the diff",
                };
                foreach (var n in ModesTests.Names.Where(DevEnv.SelfTest))
                    SelfTests.Report(n, r[n].Count == 0, r[n].Count == 0 ? detail[n] : string.Join("; ", r[n]));
                // the windows (WindowsTests.cs): model parts + this screen's checks
                var ws = WindowsScreenChecks();
                var layoutInfo = new List<string>(); var fixInfo = new List<string>();
                ws["status_compact"].InsertRange(0, WindowsTests.StatusModel());
                ws["aircraft_window"].InsertRange(0, WindowsTests.AircraftModel());
                ws["layout_migration"].InsertRange(0, WindowsTests.LayoutModel(layoutInfo));
                ws["core_fixtures"].InsertRange(0, WindowsTests.CoreFixtures(fixInfo));
                var wd = new Dictionary<string, string>
                {
                    ["status_compact"] = "FcStatus: link / firmware (disarmed · mode tags · flags · cpu; ARMED; booting; CLI; POWER OFF) / sensors (OSD, baro, GPS); config line applying → calibrating k/6 (function steps) → applied then hidden, replayed restored briefly, failed stays; screen: Status = exactly link / firmware / sensors / config, no control, config line shown while running and hidden on time",
                    ["aircraft_window"] = "AircraftReadout: heli rows (alt, speed, rotor head/motor rpm + throttle, servos), plane rows (alt, speed, air, motor, surfaces, launch), sparse state hides rows; screens: heli rows, plane rows + launch events, crash banner opens the window, Reset aircraft clears it",
                    ["transmitter_owns_switches"] = "Status has no toggle / button; the switch rows (channel order) and the keyboard hints are in the Transmitter window, no CheckButton toggle there (the ARM / mode / third toggles are gone); no switch row in any other window",
                    ["core_menu"] = "Bench menu: --Core-- read-only Core line, Restart core (own core only), Bench check, Core address…, Close bench; bad address refused; no pid / Restart / address field in Status",
                    ["layout_migration"] = $"old RS7 / trainer layouts → Aircraft placed off every open window, Status compacted ({string.Join("; ", layoutInfo)}); with Aircraft / empty left alone; default + presets' layouts: Status / Aircraft / Transmitter / Log do not overlap at 1600×900, 1280×720, 1536×864; Windows menu lists Aircraft; screen: FlightWindows over the old RS7 layout → Aircraft open there, saved",
                    ["core_fixtures"] = $"real core captures: {string.Join("; ", fixInfo)}; first-start Status lines (9) from config + function steps, applied functions/acc_calibrated, first_start diff; replayed config restored + snapshot diff; functions launch/gyro/acc done; diff refused ×3 calibrating then request; function replay; armed refusal without text; modes error cli; replay capture",
                };
                foreach (var n in WindowsTests.Names.Where(DevEnv.SelfTest))
                    SelfTests.Report(n, ws[n].Count == 0, ws[n].Count == 0 ? wd[n] : string.Join("; ", ws[n]));
                if (_mtSources != null) AppSettings.Current.Transmitter.Source = _mtSources;
                if (_mtKeys != null) AppSettings.Current.Keys = _mtKeys;
                _mtDone = true;
                if (!BenchTests.ScreenAny && !LayersTests.ScreenAny) ModesTests.Done = true;      // else the layers / bench tests run on this screen next
                break;
            }
        }
    }

    /// modes_switches, the bench's transmitter model on the screen (TxModelTests.Modes + TxModelTests.Mixed: ch5 derived
    /// ARM, ch6 switch2 throttle on a channel the modes do not mention, ch7 switch3 on ANGLE / HORIZON ranges, ch8 switch6,
    /// ch9 auto without ranges, ch10 derived BEEPER): rows and keys in channel order, "—" labels / box labels, the keys step
    /// the static switches through their values (1000 → 2000 → 1000; the six ArduPilot values), the header names the
    /// bench switches, the tooltips; then the bench back to all auto. Key presses go through the bind table (PushInput),
    /// handled at once.
    private void TxModelScreen(Action<string, bool, string> Check)
    {
        const string T = "modes_switches";
        for (int ch = 4; ch < 16; ch++) _switchSet[ch] = false;       // nothing asked for: static switches start at `start`
        _bench.Transmitter = TxModelTests.Mixed();
        _core.InjectForTest(TxModelTests.Modes);                       // modes → DeriveSwitches with the model
        string R(int i) => i < _switchRows.Count ? $"ch{_switchRows[i].Sw.Channel}:" + string.Join("|", _switchRows[i].Pos.Select(b => b.Text)) : "";
        var rows = Enumerable.Range(0, _switchRows.Count).Select(R).ToList();
        Check(T, rows.SequenceEqual(new[] { "ch5:—|ARM", "ch6:low|high", "ch7:low|ANGLE|HORIZON", "ch8:1|2|3|4|5|6", "ch10:—|BEEPER" }), "static + derived rows in channel order: " + string.Join(" ; ", rows));
        if (_switchRows.Count != 5) return;
        Check(T, string.Join(",", _switchRows.Select(r => r.Key.Text)) == "[5],[6],[7],[8],[9]", "keys 5..9 in channel order: " + string.Join(",", _switchRows.Select(r => r.Key.Text)));
        Check(T, _switches[1].Static && _switches[1].Label == "throttle" && !_switches[0].Static && !_switches[4].Static, "row kinds / label");
        Check(T, _modesHeader!.Text.StartsWith("modes: from firmware (t = 3.00 s)") && _modesHeader.Text.Contains("ch6 throttle: bench switch") && _modesHeader.Text.Contains("ch8 flight mode: bench switch"), "header: " + _modesHeader.Text);
        Check(T, _switchRows[1].Pos[1].TooltipText.Contains("2000 µs") && _switchRows[1].Pos[1].TooltipText.Contains("no firmware box") && _switchRows[2].Pos[1].TooltipText.Contains("1500 µs") && _switchRows[2].Pos[1].TooltipText.Contains("ANGLE"),
              $"static tooltips: '{_switchRows[1].Pos[1].TooltipText}' / '{_switchRows[2].Pos[1].TooltipText}'");
        Check(T, _rc.Override[4] == 1000 && _rc.Override[5] == 1000 && _rc.Override[6] == 1000 && _rc.Override[7] == 1165 && _rc.Override[8] == 0, $"start positions: ch5 {_rc.Override[4]}, ch6 {_rc.Override[5]}, ch7 {_rc.Override[6]}, ch8 {_rc.Override[7]}, ch9 {_rc.Override[8]} (no row: released)");
        // keys through the bind table: switch_2 = ch6 (throttle) 1000 → 2000 → 1000
        var seq6 = new List<int>();
        for (int k = 0; k < 2; k++) { GetTree().Root.PushInput(PtKey(Keybinds.Bind(Keybinds.SwitchId(2)))); _rc.Update(0); seq6.Add(_rc.ChannelsUs[5]); }
        Check(T, string.Join(",", seq6) == "2000,1000" && _switchRows[1].Pos[0].ButtonPressed, "switch_2 steps ch6 1000 → " + string.Join(" → ", seq6));
        // a pick is forgotten when its row goes away: ch6 put on (2000) by key, the bench's ch6 switch removed (no row:
        // released), then put back → it starts at its `start` (1000), not on again from the channel's idle 1500
        GetTree().Root.PushInput(PtKey(Keybinds.Bind(Keybinds.SwitchId(2)))); _rc.Update(0);
        int on6 = _rc.ChannelsUs[5];
        var without6 = TxModelTests.Mixed(); without6.Remove("6");
        _bench.Transmitter = without6; DeriveSwitches();
        int gone6 = _rc.Override[5];
        _bench.Transmitter = TxModelTests.Mixed(); DeriveSwitches(); _rc.Update(0);
        Check(T, on6 == 2000 && gone6 == 0 && _rc.ChannelsUs[5] == 1000 && _switchRows.Count == 5, $"ch6 on ({on6}) → row removed (override {gone6}) → row back: ch6 {_rc.ChannelsUs[5]} (its start, not on again)");
        // switch_3 = ch7 switch3: 1500, 2000, 1000 (the positions keep their µs whatever the ranges)
        var seq7 = new List<int>();
        for (int k = 0; k < 3; k++) { GetTree().Root.PushInput(PtKey(Keybinds.Bind(Keybinds.SwitchId(3)))); _rc.Update(0); seq7.Add(_rc.ChannelsUs[6]); }
        Check(T, string.Join(",", seq7) == "1500,2000,1000", "switch_3 steps ch7 1000 → " + string.Join(" → ", seq7));
        // switch_4 = ch8 switch6: the six ArduPilot values, wrapping
        var seq8 = new List<int>();
        for (int k = 0; k < 6; k++) { GetTree().Root.PushInput(PtKey(Keybinds.Bind(Keybinds.SwitchId(4)))); _rc.Update(0); seq8.Add(_rc.ChannelsUs[7]); }
        Check(T, string.Join(",", seq8) == "1295,1425,1555,1685,1815,1165", "switch_4 steps ch8 (switch6) 1165 → " + string.Join(" → ", seq8));
        // switch_5 = ch10, the derived BEEPER switch (row 5 in channel order, after the static ones)
        GetTree().Root.PushInput(PtKey(Keybinds.Bind(Keybinds.SwitchId(5)))); _rc.Update(0);
        Check(T, _rc.ChannelsUs[9] == 2000, $"switch_5 = ch10 BEEPER: {_rc.ChannelsUs[9]}");
        // a new modes message relabels the static switches, never rebuilds them: ranges now on ch6 (ARM 1700–2100)
        _core.InjectForTest(TxModelTests.Modes.Replace("\"channel\":0,\"rx_channel\":4", "\"channel\":1,\"rx_channel\":5"));
        Check(T, R(0) == "ch6:low|ARM" && _switches[0].Static && _switchRows.Count == 4 && _rc.ChannelsUs[5] == 1000, $"ranges moved to ch6: {string.Join(" ; ", Enumerable.Range(0, _switchRows.Count).Select(R))}, ch6 {_rc.ChannelsUs[5]}");
        // back to the in-memory bench's all-auto model (the checks that follow use the fixture's six derived rows)
        _bench.Transmitter = new();          // explicit empty: all auto on purpose, not the board's family default
        for (int ch = 4; ch < 16; ch++) _switchSet[ch] = false;
        _core.InjectForTest(ModesTests.Fixture);
    }

    private static JsonElement ParseEl(string json) { using var d = JsonDocument.Parse(json); return d.RootElement.Clone(); }
}
