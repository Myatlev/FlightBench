// Headless self-tests: FB_SELFTEST=all (or a list: rcmath,cfg_address,normalize,settings,bench,bench_edit,bench_signals,signals,osd,presets,cli_history,corelaunch,corelaunch_restart,corelaunch_reject,corelaunch_portbusy,core,wiring,keys,radio,plane_state,plane_visual,launch_cmd,config_progress,modes_switches,functions_panel,topbar,osd_line,diff_view,status_compact,aircraft_window,transmitter_owns_switches,core_menu,layout_migration,core_fixtures,
// bench_delete,wizard_script_view,cli_indicator,edit_in_place — BenchTests.cs; layers_view,bench_lines,config_changes,snapshot_export,
// boot_hint — LayersTests.cs).
// The firmware's read-only views (ModesTests.cs) and the windows (WindowsTests.cs) open the flight screen once more with an
// RS7 bench after the plane screens.
// The plane tests (PlaneTests.cs) open the flight screen with the trainer preset and then an RS7 bench (in memory), after the keys/radio screen.
// Each test judges its own result and prints "[selftest] <name>: PASS|FAIL (…)"; the runner (a child of
// the DevHooks autoload, so it survives scene changes) runs the pure ones at once, then the core
// connection test (local core only: FB_CORE or 127.0.0.1:31250 — it sends `cli` lines the fake core
// echoes), then the wizard's wiring round trip, then the flight screen's keys test against the same
// core with an in-memory bench (nothing written) and, on the same screen, the radio-unplugged test
// (radio: a test joypad reader, `fault crsf link_lost` on/off), prints a summary and quits with the failure count.
// FB_SELFTEST=reconnect (not part of all): bare connect → disconnect → connect → disconnect to FB_CORE,
// any core (sends nothing).
// Manifest: a self-test run never reads the user's cache (Manifest.LoadOffline): FB_MANIFEST, else the bundled
// fixture app/tests/manifest-fixture.json (a saved `core/qemu/run.sh --manifest`); expectations are derived
// from the manifest, plus one fixed set asserted only on the fixture (Wizard.WiringSelfTest says how to refresh it).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public static class SelfTests
{
    private static readonly List<(string Name, bool Ok)> Results = new();
    public static IReadOnlyList<(string Name, bool Ok)> All => Results;

    /// Records a test's verdict (and prints it).
    public static void Report(string name, bool ok, string detail = "")
    {
        Results.Add((name, ok));
        GD.Print($"[selftest] {name}: {(ok ? "PASS" : "FAIL")}{(detail != "" ? "  (" + detail + ")" : "")}");
    }

    private static void Check(List<string> fails, bool ok, string what) { if (!ok) fails.Add(what); }

    /// Is this the bundled fixture (loaded as the self-test default, or named by FB_MANIFEST)? Its fixed numbers are asserted then.
    public static bool OnFixture(Manifest m)
    {
        if (m.Source == "fixture " + Manifest.FixturePath) return true;
        if (DevEnv.Manifest is not { } dev || !m.Source.StartsWith("FB_MANIFEST ", StringComparison.Ordinal)) return false;
        try { return string.Equals(System.IO.Path.GetFullPath(dev), System.IO.Path.GetFullPath(ProjectSettings.GlobalizePath(Manifest.FixturePath)), StringComparison.OrdinalIgnoreCase); }
        catch (Exception) { return false; }
    }

    // ---- RcMath.Map ----
    public static void RcMath_()
    {
        var f = new List<string>();
        int n = 0;
        void Eq(float raw, int min, int c, int max, bool rev, float dz, int want, string what)
        {
            n++;
            int got = RcMath.Map(raw, min, c, max, rev, dz);
            Check(f, got == want, $"{what}: raw {raw} [{min}/{c}/{max}{(rev ? " rev" : "")}{(dz > 0 ? $" dz {dz}" : "")}] -> {got}, want {want}");
        }
        // two segments, identity calibration
        Eq(-1, 1000, 1500, 2000, false, 0, 1000, "identity low"); Eq(0, 1000, 1500, 2000, false, 0, 1500, "identity centre");
        Eq(1, 1000, 1500, 2000, false, 0, 2000, "identity high"); Eq(0.5f, 1000, 1500, 2000, false, 0, 1750, "identity half");
        // off-centre stick: 1100 / 1450 / 1900 → 1000 / 1500 / 2000, each half linear on its own
        Eq(-0.8f, 1100, 1450, 1900, false, 0, 1000, "off-centre low end");      // raw 1100 µs
        Eq(-0.1f, 1100, 1450, 1900, false, 0, 1500, "off-centre centre");       // raw 1450 µs
        Eq(0.8f, 1100, 1450, 1900, false, 0, 2000, "off-centre high end");      // raw 1900 µs
        Eq(0.35f, 1100, 1450, 1900, false, 0, 1750, "off-centre upper half");   // raw 1675 µs
        Eq(-0.45f, 1100, 1450, 1900, false, 0, 1250, "off-centre lower half");  // raw 1275 µs
        // reverse mirrors around the centre
        Eq(1, 1000, 1500, 2000, true, 0, 1000, "reverse high"); Eq(-0.5f, 1000, 1500, 2000, true, 0, 1750, "reverse low half");
        // deadzone around the centre only
        Eq(0.01f, 1000, 1500, 2000, false, 0.02f, 1500, "inside deadzone"); Eq(0.01f, 1000, 1500, 2000, false, 0, 1505, "no deadzone");
        Eq(0.5f, 1000, 1500, 2000, false, 0.02f, 1750, "outside deadzone");
        // clamp to 800..2200
        Eq(1, 1000, 1500, 1600, false, 0, 2200, "clamp high"); Eq(-1, 1400, 1500, 2000, false, 0, 800, "clamp low");
        // inverted / degenerate segments → identity
        Eq(0.5f, 1600, 1500, 2000, false, 0, 1750, "min above centre"); Eq(0.5f, 1000, 2100, 2000, false, 0, 1750, "centre above max");
        Eq(-0.5f, 1500, 1500, 2000, false, 0, 1250, "empty lower segment"); Eq(1, 2000, 1500, 1000, false, 0, 2000, "min/max swapped");
        Report("rcmath", f.Count == 0, f.Count == 0 ? $"{n} cases" : string.Join("; ", f));
    }

    // ---- ConfiguratorAddress: the form each configurator's connect parses ----
    public static void CfgAddress_()
    {
        var f = new List<string>();
        void Eq(string? fw, string want)
        {
            string got = ConfiguratorAddress.For(fw, "127.0.0.1", 31235);
            Check(f, got == want, $"'{fw}' -> {got}, want {want}");
        }
        // iNav Configurator splits on ':' (a tcp:// prefix = host "tcp", port NaN: "Failed to open MSP connection")
        Eq("inav 9.1.0 SPEEDYBEEF405WING", "127.0.0.1:31235");                              // hello.firmware
        Eq("firmware/speedybee-f405-wing/inav_9.1.0_SPEEDYBEEF405WING.hex", "127.0.0.1:31235");  // bench, before a hello
        Eq(@"D:\fb\firmware\INAV_9.1.0_SPEEDYBEEF405WING.hex", "127.0.0.1:31235");
        // RotorFlight configurator takes only tcp://host[:port]
        Eq("rotorflight 4.6.0 STM32F7X2_OSD (NEXUS_XR)", "tcp://127.0.0.1:31235");
        Eq("~/flightbench-spike/fw-msp/obj/main/rotorflight_STM32F7X2_OSD.elf", "tcp://127.0.0.1:31235");
        Eq("", "tcp://127.0.0.1:31235"); Eq(null, "tcp://127.0.0.1:31235");
        // a directory named inav does not make the firmware iNav
        Eq("/home/x/inav/rotorflight_STM32F7X2_OSD.elf", "tcp://127.0.0.1:31235");
        // odd names: "inav" anywhere in the file name (not with rotorflight / betaflight); an iNav file without it → tcp:// (the default)
        Eq("firmware/SPEEDYBEEF405WING_INAV_9.1.hex", "127.0.0.1:31235");
        Eq("firmware/rotorflight_inav_compare.elf", "tcp://127.0.0.1:31235");
        Eq("firmware/fw.hex", "tcp://127.0.0.1:31235");
        // the best source first: the modes' variant, then the core's firmware (hello / "firmware says"), then the bench's file
        string Pk(string? v, string? core, string? file) => ConfiguratorAddress.For(ConfiguratorAddress.Pick(v, core, file), "127.0.0.1", 31235);
        Check(f, Pk("INAV", "firmware.hex", "fw.hex") == "127.0.0.1:31235" && Pk("RTFL", "inav_x.hex", null) == "tcp://127.0.0.1:31235", "variant wins");
        Check(f, Pk(null, "inav 9.1.0 SPEEDYBEEF405WING", "fw.hex") == "127.0.0.1:31235" && Pk(null, "", "inav_9.1.hex") == "127.0.0.1:31235" && Pk("??", null, "fw.hex") == "tcp://127.0.0.1:31235", "core firmware, then the file");
        Check(f, ConfiguratorAddress.FromLog("firmware says: inav 9.1.0 SPEEDYBEEF405WING (SPEEDYBEEF405WING)") == "inav 9.1.0 SPEEDYBEEF405WING (SPEEDYBEEF405WING)" && ConfiguratorAddress.FromLog("fault crsf.lq ON") == null, "the core's 'firmware says' log line");
        Report("cfg_address", f.Count == 0, f.Count == 0 ? "iNav: host:port (variant / hello / firmware says / file name), RotorFlight and unknown: tcp://host:port" : string.Join("; ", f));
    }

    // ---- TransmitterSettings.Normalize migrations ----
    public static void Normalize_()
    {
        var f = new List<string>();
        // no Source, no legacy: gamepad default
        var a = new TransmitterSettings(); a.Normalize();
        Check(f, a.Source![0] == "axis 0" && a.Source[1] == "axis 1" && a.Source[2] == "axis 3" && a.Source[3] == "axis 2", "default axes 0,1,3,2: " + string.Join(",", a.Source.Take(4)));
        Check(f, a.Source[4] == "ui" && a.Source[5] == "ui" && a.Source[6] == "ui" && a.Source[7] == "none", "default ui on 5/6/7");
        Check(f, a.Reverse[1] && !a.Reverse[0], "default pitch reversed");
        Check(f, a.CenterUs!.All(c => c == 1500), "default centre 1500");
        // legacy AxisForChannel: sources from it, old in-code pitch inversion kept as Reverse, calibration reset
        var b = new TransmitterSettings { AxisForChannel = new[] { 2, 0, 1 }, Reverse = new bool[16], MinUs = Enumerable.Repeat(1100, 16).ToArray(), MaxUs = Enumerable.Repeat(1900, 16).ToArray() };
        b.Normalize();
        Check(f, b.Source![0] == "axis 2" && b.Source[1] == "axis 0" && b.Source[2] == "axis 1" && b.Source[3] == "none", "legacy map: " + string.Join(",", b.Source.Take(4)));
        Check(f, b.Reverse[1], "legacy pitch: old inversion becomes Reverse");
        Check(f, b.MinUs[0] == 1000 && b.MaxUs[0] == 2000 && b.CenterUs![0] == 1500, "legacy calibration reset to identity");
        Check(f, b.AxisForChannel == null, "legacy field cleared");
        var c = new TransmitterSettings { AxisForChannel = new[] { 0, 1 }, Reverse = new bool[16] }; c.Reverse[1] = true; c.Normalize();
        Check(f, !c.Reverse[1], "legacy reversed pitch flips back");
        // short arrays padded, bad centre / blank source repaired, existing sources kept
        var d = new TransmitterSettings { Source = new[] { "axis 5", " ", "buttons 1,2" }, Reverse = new[] { true }, MinUs = new[] { 1200 }, MaxUs = new[] { 1800 }, CenterUs = new[] { 1900 } };
        d.Normalize();
        Check(f, d.Source!.Length == 16 && d.Reverse.Length == 16 && d.MinUs.Length == 16 && d.MaxUs.Length == 16 && d.CenterUs!.Length == 16, "padded to 16");
        Check(f, d.Source[0] == "axis 5" && d.Source[1] == "none" && d.Source[2] == "buttons 1,2" && d.Source[15] == "none", "sources kept / blank → none");
        Check(f, d.CenterUs[0] == 1500 && d.MinUs[1] == 1000 && d.MaxUs[1] == 2000 && d.CenterUs[1] == 1500, $"centre repaired: {d.CenterUs[0]}");
        Check(f, d.Reverse[0] && !d.Reverse[1], "reverse kept");
        var e = new TransmitterSettings { Source = Enumerable.Repeat("none", 16).ToArray(), MinUs = Enumerable.Repeat(1500, 16).ToArray(), MaxUs = Enumerable.Repeat(1500, 16).ToArray() };
        e.Normalize();
        Check(f, e.MinUs[0] == 1000 && e.MaxUs[0] == 2000 && e.CenterUs![0] == 1500, "min = max → identity");
        // an old settings file end to end
        try
        {
            var s = AppSettings.Parse("{\"uiScale\":\"auto\",\"transmitter\":{\"device\":0,\"axisForChannel\":[0,1,3,2],\"reverse\":[false,false],\"minUs\":[1000],\"maxUs\":[2000]}}");
            Check(f, s.Transmitter.Source![2] == "axis 3" && s.Transmitter.Reverse[1] && s.Transmitter.Source[4] == "ui" && s.Keys.Count == 0, "old settings.json migrated");
        }
        catch (JsonException ex) { f.Add("old settings.json: " + ex.Message); }
        Report("normalize", f.Count == 0, f.Count == 0 ? "5 migrations + old file" : string.Join("; ", f));
    }

    // ---- settings.json that loads badly: nulls, bad shape, unreadable, missing ----
    public static void Settings_()
    {
        var f = new List<string>();
        // valid JSON with nulls everywhere: loads (nothing thrown), defaults in place of the nulls
        try
        {
            var s = AppSettings.Parse("{\"uiScale\":null,\"repoRoot\":null,\"firmwareDir\":null,\"keys\":{\"pause\":null,\"step\":\"\"},\"transmitter\":{\"deviceName\":null,\"source\":null,\"reverse\":null,\"minUs\":null,\"centerUs\":null,\"maxUs\":null}}");
            var t = s.Transmitter;
            Check(f, s.UiScale == "auto" && s.RepoRoot == "" && s.FirmwareDir == "" && t.DeviceName == "", "null strings → defaults");
            Check(f, !s.Keys.ContainsKey("pause") && s.Keys.TryGetValue("step", out var st) && st == "", "null key bind dropped, \"\" kept");
            Check(f, t.Source!.Length == 16 && t.Reverse.Length == 16 && t.MinUs.Length == 16 && t.MaxUs.Length == 16 && t.CenterUs!.Length == 16, "null arrays → 16");
            Check(f, t.Source[0] == "axis 0" && t.Reverse[1] && t.MinUs[0] == 1000 && t.MaxUs[0] == 2000 && t.CenterUs![0] == 1500, "null arrays → gamepad defaults");
            var n = AppSettings.Parse("{\"transmitter\":null,\"keys\":null}");
            Check(f, n.Transmitter.Source![0] == "axis 0" && n.Keys.Count == 0, "null transmitter / keys → defaults");
        }
        catch (Exception ex) { f.Add("nulls threw " + ex.GetType().Name + ": " + ex.Message); }
        // the `keyboard` source is gone (decisions 2026-10-23): an older file's `keyboard` reads as `none`, other sources kept
        try
        {
            var k = AppSettings.Parse("{\"transmitter\":{\"source\":[\"keyboard\",\"axis 1\",\" Keyboard \",\"axis 2\",\"ui\"]}}");
            var src = k.Transmitter.Source!;
            Check(f, src[0] == "none" && src[1] == "axis 1" && src[2] == "none" && src[3] == "axis 2" && src[4] == "ui" && k.Transmitter.MigratedKeyboard.SequenceEqual(new[] { 1, 3 }),
                  $"keyboard → none: [{string.Join(", ", src[0..5])}], migrated ch [{string.Join(",", k.Transmitter.MigratedKeyboard)}]");
            Check(f, RcSource.Parse("keyboard").Kind == "none", "RcSource.Parse(\"keyboard\") is not none");
        }
        catch (Exception ex) { f.Add("keyboard migration threw " + ex.GetType().Name + ": " + ex.Message); }
        // files: a bad one is copied aside (then saving may overwrite it), a missing one is just defaults,
        // an unreadable one (a folder here) is kept: Save must not replace it
        var dir = $"user://selftest-settings-{Time.GetTicksUsec()}";
        var abs = ProjectSettings.GlobalizePath(dir);
        try
        {
            System.IO.Directory.CreateDirectory(abs);
            var bad = dir + "/nulls-in-array.json";
            System.IO.File.WriteAllText(ProjectSettings.GlobalizePath(bad), "{\"uiScale\":\"1.5\",\"transmitter\":{\"minUs\":[null,1000]}}");
            var s = AppSettings.LoadFile(bad, out var keep, out var backup);
            Check(f, backup != null && System.IO.File.Exists(backup) && !keep && s.UiScale == "auto" && s.Transmitter.MinUs[0] == 1000, $"bad file: backup {backup ?? "none"}, keep {keep}, uiScale {s.UiScale}");
            s = AppSettings.LoadFile(dir + "/missing.json", out keep, out backup);
            Check(f, backup == null && !keep && s.Transmitter.Source![0] == "axis 0", $"missing file: backup {backup ?? "none"}, keep {keep}");
            var folder = dir + "/a-folder.json";
            System.IO.Directory.CreateDirectory(ProjectSettings.GlobalizePath(folder));
            s = AppSettings.LoadFile(folder, out keep, out backup);
            Check(f, backup == null && keep && s.Transmitter.Source![0] == "axis 0", $"unreadable file: backup {backup ?? "none"}, keep {keep}");
        }
        catch (Exception ex) { f.Add("files threw " + ex.GetType().Name + ": " + ex.Message); }
        finally { try { System.IO.Directory.Delete(abs, true); } catch (Exception) { } }
        Report("settings", f.Count == 0, f.Count == 0 ? "nulls → defaults; `keyboard` source → none; bad file backed up; missing = defaults; unreadable kept" : string.Join("; ", f));
    }

    // ---- bench JSON round trip (wiring fields, layouts) ----
    public static void Bench_()
    {
        var f = new List<string>();
        var b = new BenchData { Name = "selftest bench", Board = "nexus-xr", Aircraft = "rs7", BoardConfig = "set a = 1\n", CorePort = 31299, Camera = "fpv", BodyAxes = true,
                           Core = BenchData.CoreRemote, CoreHost = "192.168.1.10",
                           Time = new CoreTime { IcountShift = 3, IdleSkip = false, Realtime = true } };
        b.Wiring.Add(new WiringEntry { Device = "servo", Port = "pwm", Pad = "servo1", Actuator = "swash_a", Reversed = true });
        var esc = new WiringEntry { Device = "esc", Port = "signal", Pad = "motor1", Actuator = "motor", Option = "serial" }; esc.Links["tlm"] = "uart2"; b.Wiring.Add(esc);
        var g = new WiringEntry { Device = "icm42688p", Port = "spi", Pad = "gyro1" }; g.SetProp("gyro_noise_dps", "0.5"); g.SetProp("accel_noise_g", "0.01"); b.Wiring.Add(g);
        b.Wiring.Add(new WiringEntry { Device = "gps", Port = "uart" });
        b.Detached.Add("crsf");
        b.WiringLayout["board"] = new NodePos { X = 640, Y = 24.5f }; b.WiringLayout["servo@servo1"] = new NodePos { X = 0, Y = 50 };
        b.Layout["Signals"] = new WindowLayout { Open = true, X = 16, Y = 440, W = 1244, H = 420 };
        var json = JsonSerializer.Serialize(b, Json.Files);
        var r = BenchData.Parse(json);
        string W(WiringEntry w) => $"{w.Device}.{w.Port}@{w.Pad}{string.Concat(w.Links.Select(l => $"+{l.Key}@{l.Value}"))}:{w.Actuator}:{w.Reversed}:{w.Option}:{string.Join(",", w.Props.OrderBy(p => p.Key).Select(p => p.Key + "=" + p.Value))}";
        Check(f, b.Wiring.Select(W).SequenceEqual(r.Wiring.Select(W)), "wiring entries differ: " + string.Join(" | ", r.Wiring.Select(W)));
        Check(f, r.WiringLayout.Count == 2 && r.WiringLayout["board"].X == 640 && r.WiringLayout["board"].Y == 24.5f && r.WiringLayout["servo@servo1"].Y == 50, "wiring layout");
        Check(f, r.Layout["Signals"] is { Open: true, X: 16, Y: 440, W: 1244, H: 420 }, "window layout");
        Check(f, r.Detached.SequenceEqual(new[] { "crsf" }), "detached list");
        Check(f, r.Name == b.Name && r.CorePort == 31299 && r.Camera == "fpv" && r.BodyAxes && r.BoardConfig == b.BoardConfig, "bench fields");
        Check(f, r.Time is { IcountShift: 3, IdleSkip: false, Realtime: true }, "core time round trip");
        Check(f, json.Contains("\"core\": \"remote\"") && r.Core == BenchData.CoreRemote && r.IsRemote && r.ConnectHost == "192.168.1.10" && r.CoreHost == "192.168.1.10", "core mode round trip (remote): " + r.Core);
        // core mode: explicit values kept (a local bench keeps the remote host it remembers), migration of benches without the field
        var lk = BenchData.Parse(JsonSerializer.Serialize(new BenchData { Core = BenchData.CoreLocal, CoreHost = "192.168.1.10" }, Json.Files));
        Check(f, lk.Core == BenchData.CoreLocal && !lk.IsRemote && lk.ConnectHost == "127.0.0.1" && lk.CoreHost == "192.168.1.10", "core local round trip keeps the remote host, connects to 127.0.0.1");
        string Mig(string js) => BenchData.Parse(js).Core ?? "null";
        Check(f, Mig("{}") == "local" && Mig("{\"coreHost\":\"127.0.0.1\"}") == "local" && Mig("{\"coreHost\":\"localhost\"}") == "local" && Mig("{\"coreHost\":\"\"}") == "local" && Mig("{\"coreHost\":null}") == "local" && Mig("{\"core\":null,\"coreHost\":\"::1\"}") == "local",
              "migration: no core + loopback / empty host → local");
        Check(f, Mig("{\"coreHost\":\"192.168.1.10\",\"corePort\":31250}") == "remote" && Mig("{\"coreHost\":\"homelab\"}") == "remote" && Mig("{\"core\":\"bogus\",\"coreHost\":\"10.0.0.2\"}") == "remote",
              "migration: no core (or unknown) + another host → remote");
        Check(f, Mig("{\"core\":\"local\",\"coreHost\":\"192.168.1.10\"}") == "local" && Mig("{\"core\":\"Remote\",\"coreHost\":\"127.0.0.1\"}") == "remote", "explicit core mode wins over the host");
        Check(f, BenchData.New().Core == BenchData.CoreLocal && !new BenchData().IsRemote, "a new bench is local");
        Check(f, Flight.OffersLocalCore(r, "core.exe") == (DevEnv.Core == null) && !Flight.OffersLocalCore(r, null) && !Flight.OffersLocalCore(lk, "core.exe"), "Use local core banner: remote bench + local core found (not under FB_CORE)");
        Check(f, JsonSerializer.Serialize(r, Json.Files) == json, "second serialisation differs");
        // mapped props: actuator / reversed / telemetry → fields, the rest → Props
        Check(f, g.GetProp("gyro_noise_dps") == "0.5" && r.Wiring[1].GetProp("telemetry") == "serial" && r.Wiring[0].GetProp("reversed") == "true", "GetProp mapping");
        // session (new form): wired entries only, one per wire (esc signal + tlm), props typed on the first, telemetry, detached
        var ses = JsonSerializer.Serialize(r.ToSession("/repo"));
        Check(f, ses.Contains("\"reversed\":true") && ses.Contains("\"telemetry\":\"serial\"") && ses.Contains("\"msp\":31235") && !ses.Contains("\"gps\"") && !ses.Contains("\"slot\""), "session: " + ses);
        Check(f, ses.Contains("\"time\":{\"icount_shift\":3,\"realtime\":true,\"idle_skip\":false}"), "session time block: " + ses);
        var servoPort = Manifest.Current.Device("servo") is { PortsDeclared: false } ? "" : "\"port\":\"pwm\",";   // an old manifest without ports: the core takes the first
        Check(f, ses.Contains("{\"device\":\"servo\"," + servoPort + "\"pad\":\"servo1\",\"actuator\":\"swash_a\",\"reversed\":true}") && ses.Contains("{\"device\":\"esc\",\"port\":\"tlm\",\"pad\":\"uart2\"}")
                 && ses.Contains("{\"device\":\"crsf\",\"detached\":true}"), "session entries (port/pad, link, detached): " + ses);
        bool typed = Manifest.Current.Device("icm42688p")?.Prop("gyro_noise_dps") != null;   // a prop the manifest types → number
        Check(f, ses.Contains(typed ? "\"gyro_noise_dps\":0.5" : "\"gyro_noise_dps\":\"0.5\""), $"session props typed by the manifest ({(typed ? "number" : "text")})");
        // a file with nulls: collections / entries / props come back empty, never null
        try
        {
            var n = BenchData.Parse("{\"name\":null,\"board\":null,\"layout\":null,\"wiringLayout\":{\"board\":null},\"wiring\":[null,{\"device\":\"esc\",\"slot\":\"motor1\",\"actuator\":null,\"props\":null},{\"device\":\"servo\",\"props\":{\"x\":null}}]}");
            Check(f, n.Layout.Count == 0 && n.WiringLayout.Count == 0 && n.Wiring.Count(w => !w.Assumed) == 2 && n.Wiring.All(w => w.Props.Count == 0 && w.Links.Count == 0 && w.Actuator != null && w.Pad != null), "null collections / entries");
            Check(f, n.Name == "new bench" && n.Board == "nexus-xr" && n.FilePath != "", "null fields → defaults");
            Check(f, n.Time is { IcountShift: 2, IdleSkip: true, Realtime: true } && JsonSerializer.Serialize(n.ToSession("/repo")).Contains("\"time\":{\"icount_shift\":2,\"realtime\":true,\"idle_skip\":true}"), "old bench without time → defaults in the session (real-time on)");
            Check(f, BenchData.New().Time.Realtime && !BenchData.Parse("{\"time\":{\"realtime\":false}}").Time.Realtime && BenchData.Parse("{\"time\":{\"icountShift\":2}}").Time.Realtime, "real-time: on for a new bench, an explicit false kept");
            Check(f, BenchData.Parse("{\"time\":{\"icountShift\":9}}").Time.IcountShift == 3 && BenchData.Parse("{\"time\":null}").Time.IcountShift == 2, "time: shift clamped, null → defaults");
            Check(f, JsonSerializer.Serialize(n.ToSession("/repo")).Contains("\"pad\":\"motor1\""), "session from a bench with nulls (old slot → pad)");
            var e = BenchData.Parse("{\"wiring\":null}");
            Check(f, e.Wiring.Count == 0 && e.Layout != null && e.WiringLayout != null, "null wiring");
            // props as numbers / booleans (the trainer preset's accel_offset_mg: 20): read as text, written as text, sent typed —
            // by the manifest when it knows the prop (gyro_noise_dps float, signal_timeout_ms int), else numbers / booleans as such, text as text
            var tp = BenchData.Parse("{\"board\":\"nexus-xr\",\"wiring\":[{\"device\":\"icm42688p\",\"port\":\"spi\",\"pad\":\"gyro1\",\"props\":{\"accel_offset_mg\":20,\"accel_scale_pct\":0.5,\"gyro_noise_dps\":0.25,\"x_on\":true,\"x_text\":\"abc\",\"x_null\":null}},"
                                   + "{\"device\":\"esc\",\"port\":\"signal\",\"pad\":\"motor1\",\"actuator\":\"motor\",\"option\":\"bidir\",\"props\":{\"signal_timeout_ms\":250}}]}");
            var tpp = tp.Wiring[0].Props;
            Check(f, tpp.GetValueOrDefault("accel_offset_mg") == "20" && tpp.GetValueOrDefault("accel_scale_pct") == "0.5" && tpp.GetValueOrDefault("x_on") == "true" && tpp.GetValueOrDefault("x_text") == "abc" && !tpp.ContainsKey("x_null"),
                  "number / boolean props read as text: " + string.Join(",", tpp.Select(kv => kv.Key + "=" + kv.Value)));
            Check(f, JsonSerializer.Serialize(tp, Json.Files).Contains("\"accel_offset_mg\": \"20\""), "props written as text");
            var tses = JsonSerializer.Serialize(tp.ToSession("/repo"));
            Check(f, tses.Contains("\"accel_offset_mg\":20,") && tses.Contains("\"accel_scale_pct\":0.5") && tses.Contains("\"gyro_noise_dps\":0.25") && tses.Contains("\"x_on\":true") && tses.Contains("\"x_text\":\"abc\"")
                     && tses.Contains("\"telemetry\":\"bidir\"") && tses.Contains("\"signal_timeout_ms\":250"), "props typed in the session: " + tses);
        }
        catch (Exception ex) { f.Add("bench with nulls threw " + ex.GetType().Name + ": " + ex.Message); }
        // file names and bench-relative paths: readable ids, nothing escapes the benches folder
        Check(f, BenchData.SafeName("RS7 — first flight") == "RS7_first_flight" && BenchData.SafeName("  ") == "bench"
                 && BenchData.SafeName("Тренер (iNav) v2") == "Тренер_iNav_v2" && BenchData.SafeName("a__b") == "a__b",
              $"SafeName: {BenchData.SafeName("RS7 — first flight")} / {BenchData.SafeName("  ")} / {BenchData.SafeName("Тренер (iNav) v2")}");
        Check(f, BenchData.OwnRel("./x.cli") == "x.cli" && BenchData.OwnRel("./a.files/b/c.json") == "a.files/b/c.json"
                 && BenchData.OwnRel("./../x.cli") == null && BenchData.OwnRel("./a/../../x") == null && BenchData.OwnRel("./C:/x") == null
                 && BenchData.OwnRel("./") == null && BenchData.OwnRel("boards/x/y.cli") == null && BenchData.OwnRel(null) == null,
              "OwnRel: `./x` stays, `./../x` / rooted / empty / root-relative are refused");
        Report("bench", f.Count == 0, f.Count == 0 ? $"{json.Length} bytes, 4 entries incl. port/pad/link, reversed / option / props, detached, layouts, core time, core mode (round trip, migration loopback → local / other → remote, banner rule), new-form session + time block; nulls / no time → defaults" : string.Join("; ", f));
    }

    // ---- checked signals saved in the bench: panel → bench JSON → panel ----
    public static void BenchSignals_()
    {
        var f = new List<string>();
        var root = ((SceneTree)Engine.GetMainLoop()).Root;
        SignalsPanel? a = null, c = null;
        try
        {
            a = new SignalsPanel(); root.AddChild(a);                   // _Ready builds the plot the checks go into
            var ids = new[] { "euler_deg.roll", "rpm", "fdm.alt_m", "out.servo1_us" };
            a.Check(ids); a.Check(new[] { "rpm" });
            var b = new BenchData { Name = "selftest signals" };
            b.Signals = a.CheckedIds();
            var json = JsonSerializer.Serialize(b, Json.Files);
            var r = BenchData.Parse(json);
            Check(f, r.Signals.SequenceEqual(ids.OrderBy(x => x, StringComparer.Ordinal)), "bench signals after the round trip: " + string.Join(",", r.Signals));
            c = new SignalsPanel(); root.AddChild(c);
            c.Check(r.Signals);
            Check(f, c.CheckedIds().SequenceEqual(a.CheckedIds()), "re-checked set differs: " + string.Join(",", c.CheckedIds()));
            Check(f, BenchData.Parse("{\"signals\":null}").Signals.Count == 0 && BenchData.Parse("{}").Signals.Count == 0, "null / missing → empty");
            Check(f, BenchData.Parse("{\"signals\":[\"rpm\",\"\",\" rpm \",\"t\"]}").Signals.SequenceEqual(new[] { "rpm", "t" }), "blank / duplicate ids dropped");
        }
        catch (Exception ex) { f.Add("threw " + ex.GetType().Name + ": " + ex.Message); }
        finally { a?.QueueFree(); c?.QueueFree(); }
        Report("bench_signals", f.Count == 0, f.Count == 0 ? "4 checked ids → bench JSON → re-checked in a new Signals panel; null / blank / duplicates → clean list" : string.Join("; ", f));
    }

    // ---- edit / save as in a temp benches folder: the original's file is never touched ----
    public static void BenchEdit_()
    {
        var f = new List<string>();
        var keepDir = BenchData.Dir;
        BenchData.Dir = $"user://selftest-benches-{Time.GetTicksUsec()}";
        var abs = ProjectSettings.GlobalizePath(BenchData.Dir);
        string Bytes(string name) => System.IO.File.ReadAllText(ProjectSettings.GlobalizePath(BenchData.PathFor(name)));
        try
        {
            System.IO.Directory.CreateDirectory(abs);
            // the original: wiring, layouts, camera, core address, firmware
            var a = new BenchData { Name = "orig", FirmwarePath = "/fw/a.elf", WatchFirmware = true, CoreHost = "192.168.1.10", CorePort = 31251, Camera = "chase", BoardConfig = "set x = 1\n" };
            a.Wiring.Add(new WiringEntry { Device = "icm42688p", Port = "spi", Pad = "gyro1" });
            a.Wiring.Add(new WiringEntry { Device = "servo", Port = "pwm", Pad = "servo1", Actuator = "swash_a" });
            a.WiringLayout["board"] = new NodePos { X = 600, Y = 20 };
            a.Layout["Status"] = new WindowLayout { Open = true, X = 20, Y = 50, W = 360, H = 300 };
            Wizard.Commit(a, "orig");
            var origBytes = Bytes("orig");
            // flight screen → Save as: a copy of everything under a new name
            var copy = a.CopyAs("copy");
            copy.Save();
            var c0 = BenchData.Load(BenchData.PathFor("copy"));
            Check(f, c0 != null && c0.Name == "copy", "copy saved under its name");
            if (c0 != null) { c0.Name = "orig"; Check(f, JsonSerializer.Serialize(c0, Json.Files) == origBytes, "copy = original apart from the name"); }
            Check(f, Bytes("orig") == origBytes, "original changed by save as");
            // wizard edit mode on the copy (Wizard.Open: a clone, editing = its name): new wiring, same name
            var e = BenchData.Load(BenchData.PathFor("copy"))!.Clone();
            e.Wiring.RemoveAll(w => w.Device == "servo");
            e.Wiring.Add(new WiringEntry { Device = "crsf", Port = "uart", Pad = "uart1" });
            Check(f, !Wizard.AsksBeforeSaving("copy", "copy"), "its own name asks before saving (collision path)");
            Check(f, Wizard.AsksBeforeSaving("orig", "copy"), "another bench's name does not ask");
            Check(f, !Wizard.AsksBeforeSaving("fresh", "copy") && Wizard.AsksBeforeSaving("copy", null), "free name asks / new bench over an existing one does not");
            Wizard.Commit(e, "copy");
            var c1 = BenchData.Load(BenchData.PathFor("copy"));
            Check(f, c1 != null && c1.Wiring.Select(w => w.Device + "@" + w.Pad).SequenceEqual(new[] { "icm42688p@gyro1", "crsf@uart1" }), "edited wiring in the copy: " + string.Join(",", c1?.Wiring.Select(w => w.Device + "@" + w.Pad) ?? Array.Empty<string>()));
            Check(f, c1 != null && c1.Layout.ContainsKey("Status") && c1.CoreHost == "192.168.1.10" && c1.Core == BenchData.CoreRemote && c1.FirmwarePath == "/fw/a.elf" && c1.WatchFirmware && c1.Camera == "chase", "copy kept layout / core / firmware / camera");
            // edit the copy's name: a new file, the copy's file stays
            var copyBytes = Bytes("copy");
            var r = BenchData.Load(BenchData.PathFor("copy"))!.Clone();
            Check(f, !Wizard.AsksBeforeSaving("renamed", "copy"), "rename to a free name asks");
            Wizard.Commit(r, "renamed");
            Check(f, Bytes("copy") == copyBytes, "copy changed by saving the edit under another name");
            Check(f, BenchData.Load(BenchData.PathFor("renamed"))?.Wiring.Count == 2, "renamed bench written");
            Check(f, Bytes("orig") == origBytes, "original not byte-identical at the end");
            // an old bench file without `core` (the remote smoke bench as written before the field): migrated on load, the field written on save
            System.IO.File.WriteAllText(System.IO.Path.Combine(abs, "oldremote.json"), "{\"name\":\"oldremote\",\"coreHost\":\"192.168.1.10\",\"corePort\":31250}");
            System.IO.File.WriteAllText(System.IO.Path.Combine(abs, "oldlocal.json"), "{\"name\":\"oldlocal\",\"coreHost\":\"127.0.0.1\",\"corePort\":31250}");
            var orm = BenchData.Load(BenchData.PathFor("oldremote")); var olc = BenchData.Load(BenchData.PathFor("oldlocal"));
            Check(f, orm?.Core == BenchData.CoreRemote && olc?.Core == BenchData.CoreLocal, $"old bench files: remote host → {orm?.Core}, loopback → {olc?.Core}");
            orm!.Core = BenchData.CoreLocal; orm.Save();                       // Status → Use local core
            Check(f, Bytes("oldremote").Contains("\"core\": \"local\"") && BenchData.Load(BenchData.PathFor("oldremote"))?.IsRemote == false && BenchData.Load(BenchData.PathFor("oldremote"))?.CoreHost == "192.168.1.10", "use local core: saved as local, remote host kept");
            System.IO.File.Delete(System.IO.Path.Combine(abs, "oldremote.json")); System.IO.File.Delete(System.IO.Path.Combine(abs, "oldlocal.json"));
            Check(f, BenchData.ListAll().Select(b => b.Name).OrderBy(n => n, StringComparer.Ordinal).SequenceEqual(new[] { "copy", "orig", "renamed" }), "benches in the folder: " + string.Join(",", BenchData.ListAll().Select(b => b.Name)));
        }
        catch (Exception ex) { f.Add("threw " + ex.GetType().Name + ": " + ex.Message); }
        finally { BenchData.Dir = keepDir; try { System.IO.Directory.Delete(abs, true); } catch (Exception) { } }
        Report("bench_edit", f.Count == 0, f.Count == 0 ? "save as = copy; edit under its own name overwrites without asking; rename = new file; original byte-identical; old files without `core` migrated, saved with it" : string.Join("; ", f));
    }

    // ---- Signals: time going backwards, buffers only for plotted traces ----
    public static void Signals_()
    {
        var f = new List<string>();
        var st = new SignalStore();
        int restarts = 0; double from = 0, to = 0;
        st.Restarted += (a, b) => { restarts++; from = a; to = b; };
        void State(double t, double roll)
        {
            using var doc = JsonDocument.Parse($"{{\"type\":\"state\",\"t\":{t.ToString(System.Globalization.CultureInfo.InvariantCulture)},\"euler_deg\":[{roll.ToString(System.Globalization.CultureInfo.InvariantCulture)},0,0],\"fc\":{{\"armed\":true}}}}");
            st.State(doc.RootElement);
        }
        State(1, 1);
        st.Traces["euler_deg.roll"].Keep(true, SignalStore.Capacity);        // "checked"
        State(2, 2); State(3, 3);
        var roll = st.Traces["euler_deg.roll"]; var pitch = st.Traces["euler_deg.pitch"];
        Check(f, roll.Count == 2 && roll.Last == 3 && st.LastT == 3, $"before: roll {roll.Count} samples, last {roll.Last}, t {st.LastT}");
        Check(f, !pitch.HasBuffer && pitch.Count == 0 && pitch.Last == 0, "an unchecked trace keeps its last value only");
        Check(f, st.Traces.ContainsKey("fc.armed") && st.Traces["fc.armed"].Last == 1, "bool flattened");
        State(0.5, 7);                                                           // core restarted
        Check(f, restarts == 1 && from == 3 && to == 0.5, $"restart event: {restarts} ({from} -> {to})");
        Check(f, roll.Count == 1 && roll.At(0).t == 0.5 && roll.Last == 7 && st.LastT == 0.5, $"after: roll {roll.Count} samples, t {st.LastT}");
        State(0.6, 8);
        Check(f, restarts == 1 && roll.Count == 2, "forward again: no restart");
        using (var s = JsonDocument.Parse("{\"type\":\"samples\",\"t\":0.2,\"v\":{\"fdm.alt_m\":1.5}}")) st.Samples(s.RootElement);
        Check(f, restarts == 2 && st.LastT == 0.2 && roll.Count == 0, "samples going back restart too");
        st.Reset();
        Check(f, !st.HasT && double.IsNaN(roll.Last), "reset (hello / new connection) clears");
        Report("signals", f.Count == 0, f.Count == 0 ? "t back → cleared once, unchecked = last value only" : string.Join("; ", f));
    }

    // ---- OsdBuffer: clear, cells, partial update, clipping, attr (page / blink / version), 0x80-0xFF codes, resize ----
    public static void Osd_()
    {
        var f = new List<string>();
        var b = new OsdBuffer();
        ulong clock = 1000; b.Now = () => clock;
        void Feed(string json) { using var d = JsonDocument.Parse(json); b.Apply(d.RootElement); }
        void Row(int r, string want, string what) { var got = b.RowText(r).TrimEnd(); Check(f, got == want, $"{what}: row {r} '{got}', want '{want}'"); }
        Check(f, b.Rows == 18 && b.Cols == 50, $"default {b.Rows}x{b.Cols}");
        Feed("{\"type\":\"osd\",\"rows\":18,\"cols\":50,\"clear\":true,\"cells\":[{\"r\":0,\"c\":1,\"s\":\"ACRO\",\"attr\":0},{\"r\":0,\"c\":44,\"s\":\"16.8V\",\"attr\":0},{\"r\":17,\"c\":1,\"s\":\"1800 RPM\",\"attr\":0}]}");
        Row(0, " ACRO                                       16.8V", "first frame");
        Row(17, " 1800 RPM", "first frame");
        Feed("{\"type\":\"osd\",\"rows\":18,\"cols\":50,\"clear\":false,\"cells\":[{\"r\":0,\"c\":1,\"s\":\"ANGL\",\"attr\":0},{\"r\":17,\"c\":2,\"s\":\"9\",\"attr\":0}]}");
        Row(0, " ANGL                                       16.8V", "partial update keeps the rest");
        Row(17, " 1900 RPM", "partial update mid-string");
        Feed("{\"type\":\"osd\",\"cells\":[{\"r\":8,\"c\":23,\"s\":\"ARM\",\"attr\":64},{\"r\":9,\"c\":0,\"s\":\"X\",\"attr\":129},{\"r\":1,\"c\":47,\"s\":\"ABCDEF\",\"attr\":0},{\"r\":99,\"c\":0,\"s\":\"no\"}]}");
        Row(8, "                       ARM", "blink cell text");
        Check(f, b.BlinkCells == 3 && (b.AttrAt(8, 24) & OsdBuffer.AttrBlink) != 0, $"blink attr: {b.BlinkCells} cells");
        Check(f, b.AttrAt(9, 0) == 129 && OsdBuffer.Page(b.AttrAt(9, 0)) == 1 && b.CharAt(9, 0) == 'X' && OsdBuffer.PageLogged(1), "page 1 + version bit: stored raw, page logged, char kept");
        Check(f, b.BlinkCells == 3, "0x80 / page bits are not blink");
        // blink phase from the write: shown 0..249 ms, hidden 250..499, shown 500..
        ulong w0 = b.WrittenAt(8, 23);
        Check(f, w0 == 1000 && OsdBuffer.BlinkShown(w0, 1000) && OsdBuffer.BlinkShown(w0, 1249) && !OsdBuffer.BlinkShown(w0, 1250) && !OsdBuffer.BlinkShown(w0, 1499) && OsdBuffer.BlinkShown(w0, 1500), "blink 2 Hz phase from write");
        clock = 1300; Feed("{\"type\":\"osd\",\"cells\":[{\"r\":8,\"c\":23,\"s\":\"ARM\",\"attr\":64}]}");   // firmware re-sends it (own blink): phase restarts
        Check(f, OsdBuffer.BlinkShown(b.WrittenAt(8, 23), 1300) && b.BlinkCells == 3, "re-send restarts the blink phase (no double blink)");
        // non-ASCII codes arrive as \u00NN code points = glyph index
        Feed("{\"type\":\"osd\",\"cells\":[{\"r\":2,\"c\":0,\"s\":\"\\u0096\\u00ff\\u0006A\",\"attr\":0}]}");
        char decoded96 = b.CharAt(2, 0);
        Check(f, b.CharAt(2, 0) == (char)0x96 && b.CharAt(2, 1) == (char)0xFF && b.CharAt(2, 2) == (char)0x06 && b.CharAt(2, 3) == 'A', $"\\u00NN decode: {(int)b.CharAt(2, 0):X2} {(int)b.CharAt(2, 1):X2} {(int)b.CharAt(2, 2):X2}");
        Row(1, "                                               ABC", "clipped at the row end");
        Feed("{\"type\":\"osd\",\"cells\":[{\"r\":8,\"c\":23,\"s\":\"   \",\"attr\":0}]}");
        Check(f, b.BlinkCells == 0, $"blink cleared by overwrite: {b.BlinkCells}");
        Check(f, OsdBuffer.Display('A') == 'A' && OsdBuffer.Display((char)0x01) == OsdBuffer.Placeholder && OsdBuffer.Display((char)0xA0) == OsdBuffer.Placeholder, "symbol codes -> placeholder");
        // link:false keeps the canvas (dimmed by the panel), the next frame (link absent) ends it
        Check(f, b.Link, "link true by default");
        Feed("{\"type\":\"osd\",\"clear\":true,\"cells\":[],\"link\":false}");
        Check(f, !b.Link && b.CharAt(2, 0) == (char)0x96 && b.CharAt(2, 3) == 'A', $"link:false -> flag set, canvas kept: link {b.Link}, (2,3) '{b.CharAt(2, 3)}'");
        Feed("{\"type\":\"osd\",\"clear\":true,\"cells\":[]}");
        Check(f, b.Link, "next frame (link absent) -> link back");
        Feed("{\"type\":\"osd\",\"clear\":false,\"cells\":[],\"link\":false}"); Feed("{\"type\":\"osd\",\"clear\":true,\"cells\":[],\"link\":true}");
        Check(f, b.Link, "link:true -> link back");
        Row(0, "", "clear wipes"); Check(f, b.BlinkCells == 0, "clear resets blink");
        Feed("{\"type\":\"osd\",\"rows\":16,\"cols\":30,\"cells\":[{\"r\":15,\"c\":26,\"s\":\"12:34\",\"attr\":0}]}");
        Check(f, b.Rows == 16 && b.Cols == 30, $"resize {b.Rows}x{b.Cols}");
        Row(15, "                          12:3", "resized grid clips");
        var g = OsdPanel.GridRect(new Vector2(1000, 1000), 18, 50);
        Check(f, Mathf.IsEqualApprox(g.Size.X, 1000) && Mathf.IsEqualApprox(g.Size.Y, 540) && Mathf.IsEqualApprox(g.Position.Y, 230), $"fit: {g}");
        // symbol font (MCM → atlas)
        var t0 = Time.GetTicksUsec();
        var font = OsdFont.Load(OsdFont.DefaultPath);
        var ms = (Time.GetTicksUsec() - t0) / 1000.0;
        string fontInfo = "no font";
        if (font == null) Check(f, false, $"font {OsdFont.DefaultPath} missing or bad");
        else
        {
            var a = font.Ink('A'); var z = font.Ink(0x00); var sp = font.Ink(' ');
            Check(f, font.Count == OsdFont.Glyphs, $"glyphs {font.Count}");
            Check(f, a.White > 0, $"'A' white pixels {a.White}");
            Check(f, z.White == 0 && z.Black == 0, $"0x00 not blank: {z}");
            Check(f, sp.White == 0 && sp.Black == 0, $"space not blank: {sp}");
            Check(f, font.Ink(0x90).White > 0 && font.Ink(0x73).White > 0, "battery / crosshair glyphs empty");
            Check(f, ReferenceEquals(font, OsdFont.Load(OsdFont.DefaultPath)), "font not cached");
            var inv = font.Atlas.GetPixelv(Vector2I.Zero);   // border stays transparent
            Check(f, inv.A == 0, "atlas border not transparent");
            // a 0x80-0xFF glyph (battery 0x96, as decoded above) maps to its atlas cell: col 6, row 9, same ink as the font data
            var s96 = font.Src(decoded96); int atlasWhite = 0;
            for (int y = 0; y < OsdFont.GlyphH; y++) for (int x = 0; x < OsdFont.GlyphW; x++)
            {
                var p = font.Atlas.GetPixel((int)s96.Position.X + x, (int)s96.Position.Y + y);
                if (p.A > 0 && p.R > 0.5f) atlasWhite++;
            }
            Check(f, s96.Position == new Vector2(6 * (OsdFont.GlyphW + 2) + 1, 9 * (OsdFont.GlyphH + 2) + 1) && atlasWhite > 0 && atlasWhite == font.Ink(0x96).White,
                $"0x96 atlas cell {s96.Position}, white {atlasWhite} vs {font.Ink(0x96).White}");
            Check(f, font.Atlas.GetWidth() == 16 * (OsdFont.GlyphW + 2), $"atlas width {font.Atlas.GetWidth()} (one page, no inverted copy)");
            Check(f, OsdFont.Load("res://assets/osd/none.mcm") == null, "missing font -> null (text fallback)");
            bool bad = false; try { OsdFont.Parse("NOPE\n01010101", "x"); } catch (FormatException) { bad = true; }
            Check(f, bad, "bad header accepted");
            fontInfo = $"font '{font.Name}' {font.Count} glyphs, 'A' {a.White}w/{a.Black}b, loaded in {ms:F1} ms";
            if (DevEnv.OsdPng is { } dir)
            {
                // RotorFlight osd_symbols.h: 0x97 main batt, 0x06 volt, 0x90..0x96 battery, 0x9A amp, 0x07 mAh,
                // 0x03 0x72 0x73 0x74 0x02 crosshair (AH left/centre line/centre/right), 0x60/0x64/0x68/0x6C arrows S/E/N/W,
                // 0x75/0x76 small arrows, 0x18..0x1B heading N/S/E/W, 0x01 RSSI, 0x7B LQ, 0x7A temp, 0x0E °C, 0x9C fly minutes
                string row = "\u0097 16.8\u0006 \u0090\u0092\u0094\u0096 12.3\u009A 850\u0007 \u0003\u0072\u0073\u0074\u0002 \u0060\u0064\u0068\u006C \u0075\u0076 \u0018\u0019\u001A\u001B \u0001 99 \u007B \u007A45\u000E \u009C 03:21";
                System.IO.Directory.CreateDirectory(dir);
                var bg = new Color(0.35f, 0.45f, 0.55f);
                var both = font.RenderRow(row, bg);
                var rowPath = System.IO.Path.Combine(dir, "osd_symbols_row.png");
                var atlasPath = System.IO.Path.Combine(dir, "osd_atlas.png");
                var atlas = (Image)font.Atlas.Duplicate(); atlas.Resize(atlas.GetWidth() * 2, atlas.GetHeight() * 2, Image.Interpolation.Nearest);
                var abg = Image.CreateEmpty(atlas.GetWidth(), atlas.GetHeight(), false, Image.Format.Rgba8); abg.Fill(bg);
                abg.BlendRect(atlas, new Rect2I(Vector2I.Zero, atlas.GetSize()), Vector2I.Zero);
                Check(f, both.SavePng(rowPath) == Error.Ok && abg.SavePng(atlasPath) == Error.Ok, $"PNG dump into {dir}");
                fontInfo += $"; dumped {rowPath}, {atlasPath}";
            }
        }
        string inavInfo = OsdInav(f);
        Report("osd", f.Count == 0, f.Count == 0 ? "clear + cells, partial update, attr page/blink(0x40)/version, blink phase, link:false kept + cleared, \\u00NN codes, clipping, placeholder, resize, fit; " + fontInfo + "; " + inavInfo : string.Join("; ", f));
    }

    // ---- iNav's OSD: the 512-glyph font, page-1 glyph indices, the captured canvas, frame gaps, font switching ----
    /// iNav 9.1.0 osd_symbols.h: the HUD on page 1 and a few page-0 symbols that are lower case in ASCII.
    public const int SymAhHStart = 0x14C, SymAhHEnd = 0x154, SymAhDecoration = 0x131, SymAhChLeft = 0x13A, SymAhChCenter = 0x166, SymAhChRight = 0x13B,
                     SymBattFull = 0x63, SymAmp = 0x6A, SymAltM = 0x76, SymSatL = 0x08, SymVolt = 0x1F;
    public const string InavCanvasFixture = "res://tests/osd-inav-boot-canvas.txt";

    /// The core's captured iNav canvas (core branch inav-osd, core/qemu/evidence/inav-osd/stream-boot.canvas.txt: 18 rows ×
    /// 50 cells of `attr << 8 | byte` in hex, after DRAW `frame`); null when the file is not in this build.
    public static (int Frame, int[,] Cells)? LoadInavCanvas()
    {
        if (!FileAccess.FileExists(InavCanvasFixture)) return null;
        var lines = FileAccess.GetFileAsString(InavCanvasFixture).Replace("\r", "").Split('\n');
        int frame = -1; var rows = new List<int[]>();
        foreach (var l in lines)
        {
            if (l.StartsWith("#") || l.Trim().Length == 0) continue;
            if (l.StartsWith("frame ")) { frame = int.Parse(l[6..].Trim()); continue; }
            rows.Add(l.Split(' ', StringSplitOptions.RemoveEmptyEntries).Select(x => Convert.ToInt32(x, 16)).ToArray());
        }
        if (rows.Count == 0) return null;
        var cells = new int[rows.Count, rows[0].Length];
        for (int r = 0; r < rows.Count; r++) for (int c = 0; c < rows[r].Length; c++) cells[r, c] = rows[r][c];
        return (frame, cells);
    }

    /// An `osd` message as the core builds it: runs of consecutive cells of one row with one attr; `full` = clear:true
    /// with the non-blank cells, else every listed cell. Cells are `attr << 8 | byte`; `font` null = field absent.
    public static string OsdMessage(IEnumerable<(int R, int C, int Cell)> cells, string? font, int frame, bool full, int rows = 18, int cols = 50)
    {
        var runs = new List<string>();
        var list = cells.Where(x => !full || x.Cell != 0x20).OrderBy(x => x.R).ThenBy(x => x.C).ToList();
        for (int i = 0; i < list.Count;)
        {
            int j = i + 1;
            while (j < list.Count && list[j].R == list[i].R && list[j].C == list[j - 1].C + 1 && list[j].Cell >> 8 == list[i].Cell >> 8) j++;
            var text = new string(list.Skip(i).Take(j - i).Select(x => (char)(x.Cell & 0xFF)).ToArray());
            runs.Add($"{{\"r\":{list[i].R},\"c\":{list[i].C},\"s\":{JsonSerializer.Serialize(text)},\"attr\":{list[i].Cell >> 8}}}");
            i = j;
        }
        var fontField = font == null ? "" : $",\"font\":\"{font}\"";
        return $"{{\"type\":\"osd\",\"frame\":{frame},\"rows\":{rows},\"cols\":{cols},\"clear\":{(full ? "true" : "false")},\"link\":true{fontField},\"cells\":[{string.Join(",", runs)}]}}";
    }

    public static IEnumerable<(int R, int C, int Cell)> CanvasCells(int[,] g)
    {
        for (int r = 0; r < g.GetLength(0); r++) for (int c = 0; c < g.GetLength(1); c++) yield return (r, c, g[r, c]);
    }

    private static string OsdInav(List<string> f)
    {
        // the font: 512 glyphs, 32 atlas rows; RotorFlight's keeps 256
        var inav = OsdFont.For(OsdFont.Inav);
        var rf = OsdFont.For(OsdFont.Rotorflight);
        var rfNull = OsdFont.For(null);
        if (inav == null) { Check(f, false, $"iNav font {OsdFont.InavPath} missing or bad"); return "no iNav font"; }
        Check(f, inav.Count == 512 && inav.Capacity == 512 && inav.Atlas.GetHeight() == 32 * (OsdFont.GlyphH + 2) && inav.Atlas.GetWidth() == 16 * (OsdFont.GlyphW + 2),
            $"iNav font: {inav.Count} glyphs, capacity {inav.Capacity}, atlas {inav.Atlas.GetSize()}");
        Check(f, rf != null && rf.Capacity == 256 && rf.Count == 256 && ReferenceEquals(rf, rfNull) && ReferenceEquals(inav, OsdFont.For("inav")), $"RotorFlight's font stays 256 (capacity {rf?.Capacity}); For(null) = RotorFlight's; cached");
        Check(f, !ReferenceEquals(inav, rf) && inav.Name == "inav-default", "two fonts: " + inav.Name);
        // glyph index: iNav (attr & 3) << 8 | byte, RotorFlight the byte
        Check(f, OsdFont.GlyphIndex(OsdFont.Inav, 'T', 1) == 0x154 && OsdFont.GlyphIndex(OsdFont.Inav, 'T', 0) == 0x54 && OsdFont.GlyphIndex(OsdFont.Inav, (char)0x31, 0x41) == 0x131
              && OsdFont.GlyphIndex(OsdFont.Rotorflight, 'T', 1) == 0x54 && OsdFont.GlyphIndex(null, (char)0x96, 0) == 0x96, "glyph index by font");
        // page-1 glyphs resolve: in the font (ink), in the atlas's second half, and differ from the page-0 glyph of the same byte
        var hud = new List<int> { SymAhDecoration, SymAhChLeft, SymAhChCenter, SymAhChRight };
        for (int g = SymAhHStart; g <= SymAhHEnd; g++) hud.Add(g);
        foreach (var g in hud)
        {
            var ink = inav.Ink(g); var src = inav.Src(g);
            Check(f, inav.Has(g) && ink.White + ink.Black > 0 && src.Position.Y >= 16 * (OsdFont.GlyphH + 2), $"page-1 glyph 0x{g:X3}: ink {ink}, atlas {src.Position}");
            bool same = true;
            for (int y = 0; y < OsdFont.GlyphH && same; y++) for (int x = 0; x < OsdFont.GlyphW; x++) if (inav.Pixel(g, x, y) != inav.Pixel(g & 0xFF, x, y)) { same = false; break; }
            Check(f, !same, $"0x{g:X3} is the same glyph as page 0's 0x{g & 0xFF:X2}");
        }
        foreach (var g in new[] { SymBattFull, SymAmp, SymAltM, SymSatL, SymVolt, (int)'A', (int)'0' })
            Check(f, inav.Ink(g).White > 0, $"iNav glyph 0x{g:X2} has no white pixels");
        Check(f, inav.Ink(0x20).White == 0 && inav.Ink(0x20).Black == 0, "iNav space not blank");
        Check(f, !inav.Has(0x200) && !rf!.Has(0x100) && rf.Has(0xFF), "codes outside the font");
        // the core's captured canvas (DRAW 1026 of the trainer's boot) as a full frame, then deltas with gaps in `frame`
        var fx = LoadInavCanvas();
        if (fx is not { } canvas) { Check(f, FileAccess.FileExists(InavCanvasFixture) == false, "fixture unreadable"); return "iNav font 512 glyphs, page-1 HUD glyphs; canvas fixture not in this build (skipped)"; }
        var g0 = canvas.Cells;
        var b = new OsdBuffer();
        void Feed(string json) { using var d = JsonDocument.Parse(json); b.Apply(d.RootElement); }
        int CellOf(int r, int c) => b.AttrAt(r, c) << 8 | b.CharAt(r, c);
        int Mismatches(int[,] want) { int n = 0; for (int r = 0; r < 18; r++) for (int c = 0; c < 50; c++) if (CellOf(r, c) != want[r, c]) n++; return n; }
        Check(f, b.Font == OsdFont.Rotorflight, "a new buffer draws RotorFlight's font");
        int v0 = b.Version;
        Feed(OsdMessage(CanvasCells(g0), OsdFont.Inav, canvas.Frame, full: true));
        Check(f, b.Font == OsdFont.Inav && b.Version > v0, $"font:inav taken: {b.Font}");
        Check(f, Mismatches(g0) == 0, $"the captured canvas rebuilt: {Mismatches(g0)} cells differ");
        Check(f, b.RowText(1).Contains("ACRO") && b.RowText(15).Contains("THROTTLE IS NOT LOW"), "ACRO / THROTTLE IS NOT LOW rows");
        Check(f, b.GlyphAt(9, 24) == SymAhChLeft && b.GlyphAt(9, 25) == SymAhChCenter && b.GlyphAt(9, 26) == SymAhChRight, $"crosshairs (9,24..26): {b.GlyphAt(9, 24):X3} {b.GlyphAt(9, 25):X3} {b.GlyphAt(9, 26):X3}");
        Check(f, Enumerable.Range(20, 11).All(c => b.GlyphAt(11, c) == SymAhHEnd) && Enumerable.Range(6, 7).All(r => b.GlyphAt(r, 18) == SymAhDecoration && b.GlyphAt(r, 32) == SymAhDecoration),
            "horizon 0x154 on row 11 cols 20..30, sidebars 0x131 at cols 18 / 32 rows 6..12");
        Check(f, b.GlyphAt(16, 2) == SymBattFull && b.GlyphAt(2, 2) == SymSatL && b.GlyphAt(16, 6) == SymVolt, "battery / sats / volt glyphs");
        Check(f, b.BlinkCells == 0, "iNav sets no blink");
        // gaps: frame 1026 → 1031 (the horizon one row up: its old cells blanked, new drawn), → 1057 (the clock), → 1044 (backwards:
        // taken as it comes), → a message without `frame`. Every one applies; nothing waits for the missing numbers.
        var want = (int[,])g0.Clone();
        var d1 = new List<(int, int, int)>();
        for (int c = 20; c <= 30; c++) { d1.Add((11, c, 0x20)); d1.Add((10, c, SymAhHStart + 4)); want[11, c] = 0x20; want[10, c] = SymAhHStart + 4; }
        int v1 = b.Version;
        Feed(OsdMessage(d1, OsdFont.Inav, canvas.Frame + 5, full: false));
        Check(f, Mismatches(want) == 0 && b.Version > v1 && b.GlyphAt(10, 25) == 0x150 && b.GlyphAt(11, 25) == 0x20, $"delta after a gap of 5 frames: {Mismatches(want)} cells differ");
        var d2 = new List<(int, int, int)> { (16, 45, '3'), (16, 46, '0') };                                  // the clock 00:29 → 00:30
        foreach (var (r, c, x) in d2) want[r, c] = x;
        Feed(OsdMessage(d2, null, canvas.Frame + 31, full: false));      // font absent: kept
        Check(f, Mismatches(want) == 0 && b.Font == OsdFont.Inav && b.RowText(16).Contains("00:30"), $"sparse delta, font absent → kept ({b.Font}), clock 00:30: {Mismatches(want)} differ");
        var d3 = new List<(int, int, int)> { (1, 44, 0x01), (1, 45, '7'), (1, 46, '5') };
        foreach (var (r, c, x) in d3) want[r, c] = x;
        Feed(OsdMessage(d3, OsdFont.Inav, canvas.Frame + 18, full: false));
        Feed("{\"type\":\"osd\",\"cells\":[{\"r\":14,\"c\":3,\"s\":\"ARM \",\"attr\":0}]}");
        for (int i = 0; i < 4; i++) want[14, 3 + i] = "ARM "[i];
        Check(f, Mismatches(want) == 0, $"a frame number going back and a message without one: both applied ({Mismatches(want)} differ)");
        Check(f, b.Link, "no link loss from gaps");
        // font switching mid-session: same canvas, glyph indices follow the font, the version moves (panels redraw), back again
        int v2 = b.Version;
        Feed("{\"type\":\"osd\",\"frame\":2000,\"clear\":false,\"font\":\"rotorflight\",\"cells\":[]}");
        Check(f, b.Font == OsdFont.Rotorflight && b.Version > v2 && b.GlyphAt(11, 25) == 0x20 && b.GlyphAt(10, 25) == 0x50 && b.GlyphAt(9, 25) == 0x66 && Mismatches(want) == 0,
            $"font → rotorflight: kept canvas, glyphs by byte ({b.GlyphAt(10, 25):X3})");
        var imgRf = OsdFont.RenderBuffer(b, Colors.Black, 1);
        int v3 = b.Version;
        Feed("{\"type\":\"osd\",\"frame\":2001,\"clear\":false,\"font\":\"inav\",\"cells\":[]}");
        var imgInav = OsdFont.RenderBuffer(b, Colors.Black, 1);
        Check(f, b.Font == OsdFont.Inav && b.Version > v3 && b.GlyphAt(10, 25) == 0x150, "font → inav again");
        Check(f, !imgRf.GetData().AsSpan().SequenceEqual(imgInav.GetData()), "the render differs between the two fonts");
        int v4 = b.Version;
        Feed("{\"type\":\"osd\",\"frame\":2002,\"clear\":false,\"font\":\"inav\",\"cells\":[]}");
        Check(f, b.Version == v4, "the same font again: no redraw");
        b.Reset();
        Check(f, b.Font == OsdFont.Rotorflight && b.RowText(1).Trim() == "", "Reset (a new connection): wiped, RotorFlight's font");
        // the fixture render: the horizon's cells carry ink in the image (page 1 drawn from the atlas, not 'T')
        var fb = new OsdBuffer();
        using (var d = JsonDocument.Parse(OsdMessage(CanvasCells(g0), OsdFont.Inav, canvas.Frame, full: true))) fb.Apply(d.RootElement);
        var shot = OsdFont.RenderBuffer(fb, new Color(0.35f, 0.45f, 0.55f), 1);
        bool horizonInk = false;
        for (int y = 0; y < OsdFont.GlyphH && !horizonInk; y++) for (int x = 0; x < OsdFont.GlyphW; x++)
        {
            var px = shot.GetPixel(25 * OsdFont.GlyphW + x, 11 * OsdFont.GlyphH + y);
            if (px.R > 0.9f && px.G > 0.9f) { horizonInk = true; break; }
        }
        Check(f, horizonInk && shot.GetSize() == new Vector2I(50 * OsdFont.GlyphW, 18 * OsdFont.GlyphH), "the horizon cell (11,25) has white pixels in the render");
        if (DevEnv.OsdPng is { } dir)
        {
            System.IO.Directory.CreateDirectory(dir);
            var path = System.IO.Path.Combine(dir, "osd_inav_canvas.png");
            var big = OsdFont.RenderBuffer(fb, new Color(0.35f, 0.45f, 0.55f), 2);
            var atlas = (Image)inav.Atlas.Duplicate();
            var abg = Image.CreateEmpty(atlas.GetWidth(), atlas.GetHeight(), false, Image.Format.Rgba8); abg.Fill(new Color(0.35f, 0.45f, 0.55f));
            abg.BlendRect(atlas, new Rect2I(Vector2I.Zero, atlas.GetSize()), Vector2I.Zero);
            abg.Resize(abg.GetWidth() * 2, abg.GetHeight() * 2, Image.Interpolation.Nearest);
            Check(f, big.SavePng(path) == Error.Ok && abg.SavePng(System.IO.Path.Combine(dir, "osd_inav_atlas.png")) == Error.Ok, "iNav PNG dump into " + dir);
        }
        return $"iNav font {inav.Count} glyphs (atlas {inav.Atlas.GetWidth()}x{inav.Atlas.GetHeight()}), RotorFlight's {rf!.Count}; page-1 HUD glyphs 0x131 0x13A 0x166 0x13B 0x14C..0x154 resolve; "
             + $"captured canvas (frame {canvas.Frame}) rebuilt 900/900; deltas after gaps (+5, +31, back to +18, no frame) applied; font switch rotorflight ↔ inav keeps the canvas and re-renders; Reset → rotorflight";
    }

    // ---- bundled presets: parse, paths under the data root, session-check, open = copy (bundle untouched) ----
    public static void Presets_()
    {
        var f = new List<string>();
        var info = new List<string>();
        var root = CoreLocator.DataRoot;
        var errors = new List<string>();
        var list = Presets.List(errors);
        GD.Print($"[selftest] presets: {list.Count} in {Presets.Dir} (data root {root}): {string.Join(", ", list.Select(x => $"{x.Id} '{x.Name}'"))}");
        Check(f, list.Count > 0, $"no presets in {Presets.Dir}");
        Check(f, errors.Count == 0, "presets that do not parse as a bench: " + string.Join("; ", errors));
        var keepDir = BenchData.Dir;
        BenchData.Dir = $"user://selftest-presets-{Time.GetTicksUsec()}";
        var abs = ProjectSettings.GlobalizePath(BenchData.Dir);
        // the bundle's bytes before anything is opened
        Dictionary<string, string> Snapshot() => System.IO.Directory.Exists(Presets.Dir)
            ? System.IO.Directory.GetFiles(Presets.Dir, "*", System.IO.SearchOption.AllDirectories).ToDictionary(x => x, x => Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.IO.File.ReadAllBytes(x))))
            : new();
        var before = Snapshot();
        var cwd = System.IO.Directory.GetCurrentDirectory();
        string? exe = DevEnv.CoreExe is { } e && System.IO.File.Exists(e) ? System.IO.Path.GetFullPath(e) : null;
        if (DevEnv.CoreExe != null && exe == null) f.Add($"FB_CORE_EXE={DevEnv.CoreExe} does not exist");
        try
        {
            System.IO.Directory.CreateDirectory(abs);
            foreach (var p in list)
            {
                var b = p.Bench;
                Check(f, !string.IsNullOrWhiteSpace(p.Description), $"{p.Id}: no description");
                Check(f, !b.IsRemote && b.Time is { IcountShift: 2, IdleSkip: true, Realtime: true }, $"{p.Id}: core {b.Core}, time {b.Time.IcountShift}/{b.Time.IdleSkip}/{b.Time.Realtime} (want local, 2/idle skip/real-time)");
                Check(f, System.IO.Directory.Exists(System.IO.Path.Combine(root, "boards", b.Board)) && System.IO.File.Exists(System.IO.Path.Combine(root, "models", b.Aircraft, "aircraft.json")), $"{p.Id}: board {b.Board} / aircraft {b.Aircraft} not under the data root");
                var paths = Presets.Paths(b, root);
                Check(f, paths.Any(x => x.What == "firmware"), $"{p.Id}: no firmware");
                foreach (var x in paths)
                {
                    Check(f, BenchData.IsRootRelative(x.Path), $"{p.Id}: {x.What} '{x.Path}' is not relative to the data root");
                    Check(f, x.Exists, $"{p.Id}: {x.What} '{x.Path}' missing under the data root ({x.Resolved})");
                }
                // the wizard's status line resolves against the data root, whatever the working directory is
                System.IO.Directory.SetCurrentDirectory(System.IO.Path.GetTempPath());
                var (fwText, fwOk) = Wizard.PathState(b.FirmwarePath, "");
                System.IO.Directory.SetCurrentDirectory(cwd);
                Check(f, fwOk && fwText.Contains("found") && fwText.Contains(BenchData.ResolvePath(b.FirmwarePath, root)), $"{p.Id}: wizard firmware status with another working directory: '{fwText}'");
                // the session keeps the relative paths relative; the core resolves them against root=
                var ses = CoreLauncher.WriteSession(b, root, out var err);
                var sj = System.IO.File.Exists(ses) ? System.IO.File.ReadAllText(ses) : "";
                Check(f, err == "" && sj != "", $"{p.Id}: session not written ({err})");
                using (var doc = JsonDocument.Parse(sj == "" ? "{}" : sj))
                {
                    string S(string k) => doc.RootElement.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
                    Check(f, S("firmware") == b.FirmwarePath && S("board_config") == b.BoardConfigFile && S("profile") == b.ProfileFile,
                          $"{p.Id}: session paths firmware '{S("firmware")}', board_config '{S("board_config")}', profile '{S("profile")}' (want them as in the preset)");
                }
                if (exe == null) { info.Add($"{p.Id}: session-check skipped: no core exe"); continue; }
                // the board's machine must be in this core (the Windows core before the F405: nexus-xr only)
                if (!CoreLauncher.HasMachine(exe, b.Board)) { info.Add($"{p.Id}: session-check skipped: the core has no '{b.Board}' machine"); continue; }
                var t = System.Threading.Tasks.Task.Run(() => CoreLauncher.CheckAsync(exe, ses, root, 30000));   // off the main thread's context: safe to wait
                if (!t.Wait(40000)) { f.Add($"{p.Id}: session-check timed out"); continue; }
                var c = t.Result;
                var rep = c.Stdout.Replace("\r", "").Split('\n');
                string Line(string key) => rep.FirstOrDefault(l => l.StartsWith(key + ": ", StringComparison.Ordinal))?[(key.Length + 2)..].Trim() ?? "";
                string N(string x) => x.Replace('\\', '/').TrimEnd('/');
                bool Under(string x) => N(x).StartsWith(N(root) + "/", StringComparison.OrdinalIgnoreCase);
                var launch = rep.FirstOrDefault(l => l.StartsWith("launch: ", StringComparison.Ordinal)) ?? "";
                GD.Print($"[selftest]   {p.Id}: session-check exit {c.ExitCode}; {launch}; firmware: {Line("firmware")}; profile: {Line("profile")}");
                Check(f, c.ExitCode == 0 && c.Ok && c.HasLaunchLine, $"{p.Id}: session-check {c.Summary}");
                // a CLI board (the F405 WING) has no board config file and a preset may have no profile: those lines only when the bench names one
                bool cfgOk = b.BoardConfigFile == "" || Under(Line("board_config")), profOk = b.ProfileFile == "" || Under(Line("profile"));
                Check(f, Under(Line("firmware")) && cfgOk && profOk && System.IO.File.Exists(Line("firmware")),
                      $"{p.Id}: core resolved firmware '{Line("firmware")}', board_config '{Line("board_config")}', profile '{Line("profile")}' (want under {root})");
                Check(f, c.Machine == b.Board, $"{p.Id}: launch machine '{c.Machine}' (want the board's '{b.Board}')");
                if (b.Wiring.Any(w => w.Device == "msp_osd")) Check(f, rep.Any(l => l.StartsWith("msp_osd.", StringComparison.Ordinal) && l.Contains("uart4")), $"{p.Id}: msp_osd on uart4 not in the assembly");
                // the trainer presets: the GPS module (u-blox + its compass), the battery and the onboard SPL06 are modelled
                string Asm(string dev) => rep.FirstOrDefault(l => l.StartsWith(dev + ".", StringComparison.Ordinal)) ?? "";
                if (p.Id.StartsWith("inav-trainer", StringComparison.Ordinal))
                    foreach (var dev in new[] { "ublox", "qmc5883l", "battery", "spl06" })
                        Check(f, Asm(dev).Contains(" modelled"), $"{p.Id}: {dev} not modelled in the assembly: '{Asm(dev)}'");
                // the iNav OSD preset: msp_osd on the F405's free UART4 (serial 3); a core that models it there must type iNav's
                // `serial 3 33554432 …` (FUNCTION_MSP_OSD, bit 25) from the device descriptor into the script
                if (b.Board == "speedybee-f405-wing" && b.Wiring.Any(w => w.Device == "msp_osd"))
                {
                    var osd = Asm("msp_osd");
                    bool serialLine = rep.Any(l => l.StartsWith("config_cli| serial 3 ", StringComparison.Ordinal) && l.Split(' ').Skip(3).FirstOrDefault() is { } fn
                                                   && long.TryParse(fn.TrimStart('+'), out var bits) && (bits & 33554432) != 0);
                    if (osd.Contains(" modelled")) Check(f, serialLine, $"{p.Id}: msp_osd modelled but no `serial 3 …33554432…` line in the script");
                    else info.Add($"{p.Id}: NOTE this core does not model msp_osd on {b.Board} ('{osd.Trim()}'; no `serial 3 33554432` line from the descriptor): the OSD stays dark until the core does");
                    Check(f, b.Layout.TryGetValue("OSD", out var ow) && ow.Open && b.Camera == "fpv" && b.OsdOverlay, $"{p.Id}: OSD window / FPV camera / overlay not on");
                }
                info.Add($"{p.Id}: {launch}");
            }
            // opening a preset copies it into the (temp) benches folder; a second open takes the existing bench as it is
            if (list.FirstOrDefault() is { } first)
            {
                var o1 = Presets.Open(first, out var ex1, out var n1);
                var file = ProjectSettings.GlobalizePath(BenchData.PathFor(first.Name));
                Check(f, o1 != null && !ex1 && System.IO.File.Exists(file) && o1.Name == first.Name, $"open '{first.Name}': copied to {file} ({n1})");
                var copy = BenchData.Load(BenchData.PathFor(first.Name));
                Check(f, copy != null && copy.FirmwarePath == first.Bench.FirmwarePath && copy.ProfileFile == first.Bench.ProfileFile && copy.BoardConfigFile == first.Bench.BoardConfigFile
                         && copy.Wiring.Count == first.Bench.Wiring.Count && copy.Description == first.Bench.Description && copy.Layout.Count == first.Bench.Layout.Count,
                      "the copy carries the preset's paths / wiring / layout / description");
                copy!.Camera = "chase"; copy.Save();                                    // the user changed their bench
                var o2 = Presets.Open(first, out var ex2, out var n2);
                Check(f, o2 != null && ex2 && o2.Camera == "chase" && n2.Contains("exists"), $"second open: the existing bench opened as it is ({n2})");
                Check(f, BenchData.ListAll().Count == 1, "one bench in the folder after two opens");
            }
            var after = Snapshot();
            Check(f, before.Count == after.Count && before.All(kv => after.TryGetValue(kv.Key, out var h) && h == kv.Value), $"the bundle changed: {before.Count} → {after.Count} files");
        }
        catch (Exception ex) { f.Add("threw " + ex.GetType().Name + ": " + ex.Message); }
        finally
        {
            System.IO.Directory.SetCurrentDirectory(cwd);
            BenchData.Dir = keepDir;
            try { System.IO.Directory.Delete(abs, true); } catch (Exception) { }
        }
        Report("presets", f.Count == 0, f.Count == 0 ? $"{list.Count} presets parse, paths relative + present under {root}, session paths kept relative; {string.Join("; ", info)}; open = copy into the benches folder, again = the existing bench, bundle unchanged" : string.Join("; ", f));
    }

    // ---- CLI history: a temp file (never user://cli_history.txt), append / duplicates / cap / prefix recall / reload, and the input line's keys ----
    public static void CliHistory_()
    {
        var f = new List<string>();
        var dir = ProjectSettings.GlobalizePath($"user://selftest-clihistory-{Time.GetTicksUsec()}");
        var file = System.IO.Path.Combine(dir, "cli_history.txt");
        string[] Lines() => System.IO.File.Exists(file) ? System.IO.File.ReadAllLines(file) : Array.Empty<string>();
        string J(IEnumerable<string?> x) => string.Join("|", x.Select(e => e ?? "null"));
        CliLine? line = null;
        try
        {
            Check(f, CliHistory.DefaultPath() != ProjectSettings.GlobalizePath(CliHistory.UserPath), $"a self-test run must not use the real history (default path {CliHistory.DefaultPath() ?? "in memory"})");
            // append, consecutive duplicates, empty / whitespace, trimming; every Add is on disk at once
            var h = new CliHistory(file);
            Check(f, h.Entries.Count == 0 && !System.IO.File.Exists(file), "a missing file = empty, not created by loading");
            foreach (var c in new[] { "status", "status", "set gyro_lpf1_type = PT1", "   ", "", "get gyro", " status ", "set motor_pwm_rate = 480", "set gyro_lpf1_type = PT1", "version" }) h.Add(c);
            var want = new[] { "status", "set gyro_lpf1_type = PT1", "get gyro", "status", "set motor_pwm_rate = 480", "set gyro_lpf1_type = PT1", "version" };
            Check(f, h.Entries.SequenceEqual(want), "entries after Add: " + J(h.Entries));
            Check(f, Lines().SequenceEqual(want), "file after Add: " + J(Lines()));
            Check(f, !System.IO.File.ReadAllBytes(file).Take(3).SequenceEqual(new byte[] { 0xEF, 0xBB, 0xBF }), "file starts with a BOM");
            // prefix recall: newest first, each distinct command once, the typed prefix kept; oldest = stays; Down past the newest = the typed text
            var seq = new List<string?> { h.Up("set"), h.Up("ignored while browsing"), h.Up("x") };
            Check(f, J(seq) == "set gyro_lpf1_type = PT1|set motor_pwm_rate = 480|null" && h.Browsing, "Up with prefix 'set': " + J(seq));
            seq = new List<string?> { h.Down(), h.Down(), h.Down() };
            Check(f, J(seq) == "set gyro_lpf1_type = PT1|set|null" && !h.Browsing, "Down past the newest: " + J(seq));
            seq = new List<string?> { h.Up(""), h.Up(""), h.Up(""), h.Up(""), h.Up(""), h.Up("") };
            Check(f, J(seq) == "version|set gyro_lpf1_type = PT1|set motor_pwm_rate = 480|status|get gyro|null", "Up with no prefix: " + J(seq));
            Check(f, h.Cancel() == "" && !h.Browsing && h.Cancel() == null && h.Down() == null, "Cancel = the typed text, then not browsing");
            Check(f, h.Up("zzz") == null && !h.Browsing, "no match: nothing recalled, not browsing");
            Check(f, h.Up("status") == null && !h.Browsing, "the typed text itself is not a match");
            h.Up("get"); h.Add("help");
            Check(f, !h.Browsing && h.Up("") == "help", "Add ends browsing; the new command is the newest");
            h.EndBrowse();
            // persistence: a new instance on the same file
            var h2 = new CliHistory(file);
            Check(f, h2.Entries.SequenceEqual(h.Entries), "reload: " + J(h2.Entries));
            // cap: a longer file is compacted on load (newest kept, consecutive duplicates collapsed); Add keeps the cap in memory
            var big = Enumerable.Range(0, CliHistory.Cap + 50).Select(i => $"cmd {i}").ToList();
            big.Insert(10, "cmd 9");
            System.IO.File.WriteAllText(file, string.Join("\n", big) + "\n");
            var h3 = new CliHistory(file);
            Check(f, h3.Entries.Count == CliHistory.Cap && h3.Entries[0] == "cmd 50" && h3.Entries[^1] == $"cmd {CliHistory.Cap + 49}", $"cap on load: {h3.Entries.Count} entries, {h3.Entries.FirstOrDefault()} .. {h3.Entries.LastOrDefault()}");
            Check(f, Lines().SequenceEqual(h3.Entries), $"compacted file: {Lines().Length} lines");
            h3.Add("newest");
            Check(f, h3.Entries.Count == CliHistory.Cap && h3.Entries[0] == "cmd 51" && h3.Entries[^1] == "newest" && Lines().Length == CliHistory.Cap + 1, $"Add over the cap: {h3.Entries.Count} in memory, {Lines().Length} lines in the file");
            var h4 = new CliHistory(file);
            Check(f, h4.Entries.SequenceEqual(h3.Entries) && Lines().Length == CliHistory.Cap, $"compacted again on the next load: {Lines().Length} lines");
            // a file whose last line has no line break (hand-edited): the next Add starts a new line
            System.IO.File.WriteAllText(file, "status\nget b");
            new CliHistory(file).Add("version");
            Check(f, J(Lines()) == "status|get b|version", "append after a file without a final newline: " + J(Lines()));
            // the input line: real key events through the viewport, as typed
            System.IO.File.WriteAllText(file, "status\nset a = 1\nget b\nset c = 2\n");
            var sent = new List<string>();
            int clears = 0;
            line = new CliLine { History = new CliHistory(file), Sent = t => { sent.Add(t); return true; }, ClearLog = () => clears++ };
            var root = ((SceneTree)Engine.GetMainLoop()).Root;
            root.AddChild(line);
            line.GrabFocus(); line.Edit();
            void Press(Key k, bool ctrl = false)
            {
                root.PushInput(new InputEventKey { Keycode = k, PhysicalKeycode = k, CtrlPressed = ctrl, Pressed = true });
                root.PushInput(new InputEventKey { Keycode = k, PhysicalKeycode = k, CtrlPressed = ctrl, Pressed = false });
            }
            line.Text = "set"; line.CaretColumn = 1;
            Press(Key.Up);
            Check(f, line.Text == "set c = 2" && line.CaretColumn == line.Text.Length, $"Up: '{line.Text}' caret {line.CaretColumn}");
            Press(Key.Up); line.CaretColumn = 3; Press(Key.Up);
            Check(f, line.Text == "set a = 1" && line.CaretColumn == 3, $"Up at the oldest match: '{line.Text}' caret {line.CaretColumn} (want 'set a = 1', caret not moved)");
            Press(Key.Left); Press(Key.Home);
            Check(f, line.CaretColumn == 0 && line.Text == "set a = 1", $"Left / Home still move the caret: {line.CaretColumn}");
            Press(Key.End); Press(Key.Down); Press(Key.Down);
            Check(f, line.Text == "set", $"Down past the newest: '{line.Text}' (want the typed 'set')");
            Press(Key.Up); Press(Key.Escape);
            Check(f, line.Text == "set" && !line.History.Browsing && line.IsEditing(), $"Escape while browsing: '{line.Text}', editing {line.IsEditing()}");
            Press(Key.Up); Press(Key.Enter);
            Check(f, J(sent) == "set c = 2" && line.Text == "" && line.History.Entries[^1] == "set c = 2" && Lines().Length == 4, $"Enter on a recalled command: sent {J(sent)}, field '{line.Text}', file {Lines().Length} lines (a repeat of the newest is not stored)");
            line.Text = "   "; Press(Key.Enter);
            Check(f, sent.Count == 1, "whitespace only: nothing sent");
            line.Text = "diff all"; line.CaretColumn = 8; Press(Key.Enter);
            Check(f, J(sent) == "set c = 2|diff all" && Lines().LastOrDefault() == "diff all", $"a new command sent and appended: {J(Lines())}");
            Press(Key.L, ctrl: true);
            Check(f, clears == 1 && line.History.Entries.Count == 5, $"Ctrl+L: clear log called {clears}x, history kept ({line.History.Entries.Count})");
        }
        catch (Exception ex) { f.Add("threw " + ex.GetType().Name + ": " + ex.Message); }
        finally
        {
            if (line != null) { line.GetParent()?.RemoveChild(line); line.Free(); }
            try { System.IO.Directory.Delete(dir, true); } catch (Exception) { }
        }
        Report("cli_history", f.Count == 0, f.Count == 0 ? "temp file: append, consecutive duplicates / blank not stored, no BOM; prefix recall newest first (distinct), Down past the newest / Escape = typed text; reload; cap 1000 + compaction on load; input line: Up/Down/Escape/Enter/Ctrl+L via the viewport, caret kept, Left/Home untouched" : string.Join("; ", f));
    }

    public static void RunPure()
    {
        if (DevEnv.SelfTest("rcmath")) RcMath_();
        if (DevEnv.SelfTest("cfg_address")) CfgAddress_();
        if (DevEnv.SelfTest("normalize")) Normalize_();
        if (DevEnv.SelfTest("settings")) Settings_();
        if (DevEnv.SelfTest("bench")) Bench_();
        if (DevEnv.SelfTest("bench_edit")) BenchEdit_();
        if (DevEnv.SelfTest("tx_model")) TxModelTests.Run();       // the bench's transmitter model: TxModelTests.cs
        if (DevEnv.SelfTest("bench_signals")) BenchSignals_();
        if (DevEnv.SelfTest("signals")) Signals_();
        if (DevEnv.SelfTest("osd")) Osd_();
        if (DevEnv.SelfTest("presets")) Presets_();
        if (DevEnv.SelfTest("cli_history")) CliHistory_();
        LayersTests.RunPure();                                      // layers_view, config_changes (model): LayersTests.cs
    }
}

/// Runs the self-tests across frames and screens (a child of the DevHooks autoload).
public partial class SelfTestRunner : Node
{
    private enum Stage { Pure, CoreLaunch, Core, Wiring, WiringWait, WizardScript, Layers, Keys, KeysWait, Plane, PlaneWait, PlaneHeli, PlaneHeliWait, Modes, ModesWait, Reconnect, Done }
    private Stage _stage = Stage.Pure;
    private ulong _deadline;
    private CoreClient? _c;
    private readonly List<string> _logs = new();
    private int _step;
    private (string Host, int Port) _addr;
    private readonly CoreLaunchTests _coreLaunch = new();
    private readonly LayersRun _layersRun = new();

    public override void _Ready()
    {
        _addr = CoreSession.ParseAddress(DevEnv.Core) ?? ("127.0.0.1", 31250);
        GD.Print($"[selftest] FB_SELFTEST={string.Join(",", DevEnv.SelfTests)}; core {_addr.Host}:{_addr.Port}; manifest {Manifest.Current.Describe()}");
    }

    private bool Timeout(string name)
    {
        if (Time.GetTicksMsec() < _deadline) return false;
        SelfTests.Report(name, false, "timed out at step " + _step);
        return true;
    }

    public override void _Process(double delta)
    {
        switch (_stage)
        {
            case Stage.Pure:
                SelfTests.RunPure();
                _stage = Stage.CoreLaunch;
                break;
            case Stage.CoreLaunch:                                    // corelaunch* (CoreLaunchTests.cs)
                _coreLaunch.Poll();
                if (!_coreLaunch.Done) break;
                _stage = DevEnv.SelfTest("core") ? Stage.Core : Stage.Wiring;
                _step = 0; _deadline = Time.GetTicksMsec() + 15000;
                break;
            case Stage.Core: CoreTest(); break;
            case Stage.Wiring:
                if (!DevEnv.SelfTest("wiring")) { _stage = Stage.WizardScript; break; }
                AppState.Bench = new BenchData { Name = "selftest (not saved)" };
                GetTree().ChangeSceneToFile("res://scenes/Wizard.tscn");
                _stage = Stage.WiringWait; _deadline = Time.GetTicksMsec() + 15000;
                break;
            case Stage.WiringWait:
                if (GetTree().CurrentScene is Wizard wiz && wiz.IsNodeReady())
                {
                    SelfTests.Report("wiring", wiz.WiringSelfTest(), "onboard pre-wired, ports/pads round trip, new-form session, direction/kind/onboard refusals, i2c shared, detach/put back, soldered refused, two board nodes, old-bench migration (smoke wires / assumed / detached from the manifest, fixed on the fixture; new form once saved), omitted detachable onboard → detached, msp_osd in/out uart, faults gating");
                    _stage = Stage.WizardScript;
                }
                else if (Timeout("wiring")) _stage = Stage.WizardScript;
                break;
            case Stage.WizardScript:                                  // the wizard's first-start script (BenchTests.cs)
                if (!DevEnv.SelfTest("wizard_script_view") || WizardScriptTest()) _stage = Stage.Layers;
                break;
            case Stage.Layers:                                        // bench_lines, snapshot_export (model): a python fake core (LayersTests.cs)
                _layersRun.Poll();
                if (_layersRun.Done) _stage = Stage.Keys;
                break;
            case Stage.Keys:
                if (!DevEnv.SelfTest("keys") && !DevEnv.RadioSelftest) { _stage = Stage.Plane; break; }
                AppState.Bench = new BenchData { Name = "selftest (not saved)", CoreHost = _addr.Host, CorePort = _addr.Port };
                // the receiver wired (its faults are enabled only for a present device; onboard on the Nexus-XR, but an older manifest does not say so)
                AppState.Bench.Wiring.Add(new WiringEntry { Device = "crsf", Port = Manifest.Current.Device("crsf")?.Primary.Id ?? "", Pad = "uart5" });
                GetTree().ChangeSceneToFile("res://scenes/Flight.tscn");          // Flight runs KeysSelfTest (DevEnv.KeysSelftest), then RadioSelfTest, and reports
                _stage = Stage.KeysWait; _deadline = Time.GetTicksMsec() + 30000;
                break;
            case Stage.KeysWait:
                if ((!DevEnv.KeysSelftest || SelfTests.All.Any(r => r.Name == "keys")) && (!DevEnv.RadioSelftest || SelfTests.All.Any(r => r.Name == "radio"))) _stage = Stage.Plane;
                else if (Timeout(DevEnv.KeysSelftest && !SelfTests.All.Any(r => r.Name == "keys") ? "keys" : "radio")) _stage = Stage.Plane;
                break;
            case Stage.Plane:
            {
                if (!PlaneTests.Any) { _stage = Stage.Modes; break; }
                // the bundled trainer preset as an in-memory bench (renamed, never saved), connected to the local (fake) core
                var preset = Presets.List().FirstOrDefault(x => x.Id == "inav-trainer-launch");
                if (preset == null) { foreach (var n in PlaneTests.Names.Where(DevEnv.SelfTest)) SelfTests.Report(n, false, $"no preset inav-trainer-launch in {Presets.Dir}"); _stage = Stage.Modes; break; }
                var b = preset.Bench.Clone();
                b.Name = "selftest plane (not saved)"; b.Core = BenchData.CoreRemote; b.CoreHost = _addr.Host; b.CorePort = _addr.Port;
                AppState.Bench = b;
                Flight.PlaneTest = PlaneTestMode.Plane;
                GetTree().ChangeSceneToFile("res://scenes/Flight.tscn");
                _stage = Stage.PlaneWait; _deadline = Time.GetTicksMsec() + 40000;
                break;
            }
            case Stage.PlaneWait:
                if (PlaneTests.LaunchFails != null) { Flight.PlaneTest = PlaneTestMode.None; _stage = DevEnv.SelfTest("launch_cmd") ? Stage.PlaneHeli : Stage.Modes; }
                else if (Time.GetTicksMsec() >= _deadline)
                {
                    foreach (var n in PlaneTests.Names.Where(n => DevEnv.SelfTest(n) && !SelfTests.All.Any(r => r.Name == n))) SelfTests.Report(n, false, "timed out on the plane screen");
                    _stage = Stage.Modes;
                }
                break;
            case Stage.PlaneHeli:
                AppState.Bench = new BenchData { Name = "selftest heli (not saved)", Aircraft = "rs7", Core = BenchData.CoreRemote, CoreHost = _addr.Host, CorePort = _addr.Port };
                Flight.PlaneTest = PlaneTestMode.Heli;
                GetTree().ChangeSceneToFile("res://scenes/Flight.tscn");
                _stage = Stage.PlaneHeliWait; _deadline = Time.GetTicksMsec() + 20000;
                break;
            case Stage.PlaneHeliWait:
                if (PlaneTests.HeliDone) { Flight.PlaneTest = PlaneTestMode.None; _stage = Stage.Modes; }
                else if (Time.GetTicksMsec() >= _deadline) { SelfTests.Report("launch_cmd", false, "timed out on the heli screen"); Flight.PlaneTest = PlaneTestMode.None; _stage = Stage.Modes; }
                break;
            case Stage.Modes:
                // the firmware's read-only views (ModesTests.cs) and the windows (WindowsTests.cs): an RS7 bench in memory,
                // connected to the local (fake) core
                if (!ModesTests.Any && !WindowsTests.Any && !BenchTests.ScreenAny && !LayersTests.ScreenAny) { _stage = Stage.Reconnect; break; }
                AppState.Bench = new BenchData { Name = "selftest modes (not saved)", Aircraft = "rs7", Core = BenchData.CoreRemote, CoreHost = _addr.Host, CorePort = _addr.Port, Transmitter = new() };   // explicit empty: all auto on purpose (null = the board's family default)
                Flight.ModesTest = true;
                GetTree().ChangeSceneToFile("res://scenes/Flight.tscn");
                _stage = Stage.ModesWait; _deadline = Time.GetTicksMsec() + 90000;
                break;
            case Stage.ModesWait:
                if (ModesTests.Done) { Flight.ModesTest = false; _stage = Stage.Reconnect; }
                else if (Time.GetTicksMsec() >= _deadline)
                {
                    foreach (var n in ModesTests.Names.Concat(WindowsTests.Names).Concat(BenchTests.ScreenNames).Concat(LayersTests.ScreenNames).Where(n => DevEnv.SelfTest(n) && !SelfTests.All.Any(r => r.Name == n))) SelfTests.Report(n, false, "timed out on the modes screen");
                    Flight.ModesTest = false; _stage = Stage.Reconnect;
                }
                break;
            case Stage.Reconnect:
                if (!DevEnv.SelfTests.Contains("reconnect")) { _stage = Stage.Done; break; }
                ReconnectTest();
                break;
            case Stage.Done:
                int fail = SelfTests.All.Count(r => !r.Ok);
                GD.Print($"[selftest] summary: {SelfTests.All.Count(r => r.Ok)} PASS, {fail} FAIL ({string.Join(", ", SelfTests.All.Select(r => r.Name + (r.Ok ? "" : " FAIL")))})");
                _stage = (Stage)99;
                GetTree().Quit(fail);
                break;
        }
    }

    /// Outbox policy against a local core: what is sent while disconnected or connecting is dropped (the
    /// core never echoes it, also not after connecting / reconnecting); what is sent while connected arrives.
    private void CoreTest()
    {
        const string name = "core";
        if (_step == 0)
        {
            if (_addr.Host is not ("127.0.0.1" or "localhost"))
            {
                SelfTests.Report(name, false, $"skipped: {_addr.Host} is not a local core (this test sends cli lines)");
                _stage = Stage.Wiring; return;
            }
            _c = new CoreClient();
            _c.LogReceived += (src, text) => _logs.Add(src + " " + text);
            _c.ConnectionChanged += s => _logs.Add("conn " + s);
            bool q1 = _c.Send(new { type = "cli", text = "fbst-queued-while-disconnected" });
            _c.Connect(_addr.Host, _addr.Port);
            bool q2 = _c.Send(new { type = "cli", text = "fbst-sent-while-connecting" });
            if (q1 || q2 || _c.Dropped != 2) { Finish(false, $"Send returned {q1}/{q2}, dropped {_c.Dropped} (want false/false, 2)"); return; }
            _step = 1;
        }
        _c!.Poll();
        if (Timeout(name)) { Cleanup(); _stage = Stage.Wiring; return; }
        switch (_step)
        {
            case 1 when _c.Connected:
                if (!_c.Send(new { type = "cli", text = "fbst-live-1" })) { Finish(false, "Send while connected returned false"); return; }
                _step = 2; break;
            case 2 when Seen("fbst-live-1"):
                _c.Disconnect();
                bool q3 = _c.Send(new { type = "cli", text = "fbst-queued-after-disconnect" });
                if (q3) { Finish(false, "Send after Disconnect returned true"); return; }
                _c.Connect(_addr.Host, _addr.Port);
                _step = 3; break;
            case 3 when _c.Connected:
                _c.Send(new { type = "cli", text = "fbst-live-2" });
                _step = 4; _settle = Time.GetTicksMsec() + 300; break;
            case 4 when Seen("fbst-live-2") && Time.GetTicksMsec() > _settle:
            {
                var leaked = _logs.Where(l => l.Contains("fbst-queued") || l.Contains("fbst-sent-while")).ToList();
                bool ok = leaked.Count == 0 && _c.Dropped == 3;
                Finish(ok, ok ? "3 lines sent while not connected were dropped and never reached the core; lines sent while connected arrived, also after a reconnect"
                              : $"leaked: {string.Join(" | ", leaked)}; dropped {_c.Dropped}");
                break;
            }
        }
    }
    private ulong _settle;

    private bool Seen(string marker) => _logs.Any(l => l.Contains(marker));

    private void Finish(bool ok, string detail)
    {
        SelfTests.Report("core", ok, detail);
        Cleanup();
        _stage = Stage.Wiring;
    }

    private void Cleanup() { _c?.Dispose(); _c = null; }

    /// FB_SELFTEST=reconnect: connect → disconnect → connect → disconnect to FB_CORE; nothing is sent.
    private void ReconnectTest()
    {
        const string name = "reconnect";
        if (_c == null) { _c = new CoreClient(); _c.ConnectionChanged += s => GD.Print("[selftest] reconnect: " + s); _c.Connect(_addr.Host, _addr.Port); _step = 1; _deadline = Time.GetTicksMsec() + 15000; }
        _c.Poll();
        if (Timeout(name)) { Cleanup(); _stage = Stage.Done; return; }
        switch (_step)
        {
            case 1 when _c.Connected: _c.Disconnect(); _c.Connect(_addr.Host, _addr.Port); _step = 2; break;
            case 2 when _c.Connected:
                _c.Disconnect();
                SelfTests.Report(name, _c.Dropped == 0 && !_c.Connected, $"{_addr.Host}:{_addr.Port}: connected, disconnected, connected again, disconnected; nothing sent");
                Cleanup(); _stage = Stage.Done; break;
        }
    }
}
