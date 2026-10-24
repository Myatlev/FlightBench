// Self-test `tx_model` (FB_SELFTEST=all, or tx_model; pure: no screen): the bench's transmitter model (TxModel.cs,
// decisions 2026-10-23) — the bench JSON round trip of the `transmitter` block, its normalisation (bad channels / types,
// start clamped), the migration of a bench with the old `switches` and without the block (every channel auto, old names
// kept as labels only where the bench's modes snapshot has no range; no static switch invented), the wizard's new-bench
// default per the board's `firmware_family` (boards/*/board.json: nexus-xr = rotorflight → ch5 auto + ch6 switch2
// "throttle"; speedybee-f405-wing = inav → all auto; an edited bench untouched), the static positions (switch2 / switch3 /
// switch6 values, ArduPilot's switch6 read-back thresholds) and the rows (static + derived in channel order, labels from
// the firmware's ranges, an auto channel without ranges → no row), and the bundled presets' blocks.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace FlightBench;

public static class TxModelTests
{
    private static void Check(List<string> f, bool ok, string what) { if (!ok) f.Add(what); }

    /// ARM on ch5, ANGLE 1300–1700 + HORIZON 1700–2100 on ch7, BEEPER 1700–2100 on ch10 (rx_channel given).
    public const string Modes = "{\"type\":\"modes\",\"t\":3.0,\"variant\":\"RTFL\",\"boxes\":[{\"id\":0,\"index\":0,\"name\":\"ARM\"},{\"id\":1,\"index\":1,\"name\":\"ANGLE\"},{\"id\":2,\"index\":2,\"name\":\"HORIZON\"},{\"id\":13,\"index\":3,\"name\":\"BEEPER\"}],\"ranges\":["
        + "{\"slot\":0,\"channel\":0,\"rx_channel\":4,\"box\":0,\"start_us\":1700,\"end_us\":2100},"
        + "{\"slot\":1,\"channel\":2,\"rx_channel\":6,\"box\":1,\"start_us\":1300,\"end_us\":1700},{\"slot\":2,\"channel\":2,\"rx_channel\":6,\"box\":2,\"start_us\":1700,\"end_us\":2100},"
        + "{\"slot\":3,\"channel\":5,\"rx_channel\":9,\"box\":13,\"start_us\":1700,\"end_us\":2100}]}";

    /// The mixed model of the screen check (ModesTests): ch6 switch2 throttle (no range there), ch7 switch3 (ranges),
    /// ch8 switch6, ch9 auto (no range: no row); ch5 / ch10 auto with ranges.
    public static Dictionary<string, TxChannel> Mixed() => new()
    {
        ["6"] = new() { Type = TxModel.Switch2, Label = "throttle" },
        ["7"] = new() { Type = TxModel.Switch3, Label = "mode" },
        ["8"] = new() { Type = TxModel.Switch6, Label = "flight mode" },
        ["9"] = new() { Type = TxModel.Auto, Label = "beeper?" },
    };

    public static void Run()
    {
        var f = new List<string>();
        // round trip
        var b = new BenchData { Name = "tx rt", Transmitter = new() { ["5"] = new() { Type = "auto" }, ["6"] = new() { Type = "switch2", Label = "throttle", Start = 1 }, ["8"] = new() { Type = "switch6", Label = "fm", Start = 3 } } };
        var json = JsonSerializer.Serialize(b, Json.Files);
        var back = BenchData.Parse(json);
        string M(Dictionary<string, TxChannel>? m) => m == null ? "null" : string.Join(";", m.Select(kv => $"{kv.Key}={kv.Value.Type}/{kv.Value.Label}/{kv.Value.Start}"));
        Check(f, M(back.Transmitter) == "5=auto//0;6=switch2/throttle/1;8=switch6/fm/3", "round trip: " + M(back.Transmitter));
        Check(f, json.Contains("\"transmitter\": {") && json.Contains("\"6\": {") && json.Contains("\"type\": \"switch2\"") && json.Contains("\"label\": \"throttle\""), "JSON shape: " + json.Substring(Math.Max(0, json.IndexOf("\"transmitter\"")), Math.Min(200, json.Length - Math.Max(0, json.IndexOf("\"transmitter\"")))));
        var json2 = JsonSerializer.Serialize(back, Json.Files);
        Check(f, JsonSerializer.Serialize(BenchData.Parse(json2), Json.Files) == json2, "a second round trip changes the file");
        Check(f, !JsonSerializer.Serialize(new BenchData { Name = "none" }, Json.Files).Contains("\"transmitter\""), "no block written for an all-auto bench without labels");
        // normalisation: channel 4 / 17 / junk dropped, unknown type → auto, start clamped, label trimmed
        var n = BenchData.Parse("{\"name\":\"n\",\"transmitter\":{\"4\":{\"type\":\"switch2\"},\"17\":{\"type\":\"switch2\"},\"x\":{\"type\":\"switch2\"},\"7\":{\"type\":\"Switch3\",\"label\":\"  mode \",\"start\":9},\"9\":{\"type\":\"slider\",\"start\":2}}}");
        Check(f, M(n.Transmitter) == "7=switch3/mode/2;9=auto//0", "normalised: " + M(n.Transmitter));
        // migration: old `switches`, no block → auto everywhere; labels only on channels without a firmware range
        var oldTrainer = "{\"name\":\"old\",\"board\":\"speedybee-f405-wing\",\"switches\":[{\"channel\":5,\"name\":\"ARM\"},{\"channel\":6,\"name\":\"ANGLE\"},{\"channel\":7,\"name\":\"NAV LAUNCH\",\"note\":\"high = launch\"}]}";
        var mg = BenchData.Parse(oldTrainer);
        Check(f, M(mg.Transmitter) == "5=auto/ARM/0;6=auto/ANGLE/0;7=auto/NAV LAUNCH/0" && mg.Transmitter!.Values.All(c => !c.IsStatic), "migration without modes: " + M(mg.Transmitter));
        var snap = ModesTests.Parse(Modes)!;
        var mg2 = TxModel.Migrate("speedybee-f405-wing", mg.Switches, snap);
        Check(f, M(mg2) == "6=auto/ANGLE/0", "migration with modes (ranges on ch5 / ch7 → no label there): " + M(mg2));
        Check(f, TxModel.Migrate("speedybee-f405-wing", null, snap) == null, "no switches, iNav board → no block");
        Check(f, TxModel.Migrate("nexus-xr", null, snap) == null, "no switches, RotorFlight board → no block written (the default applies at view time)");
        Check(f, M(TxModel.Effective(null, "nexus-xr")) == "5=auto//0;6=switch2/throttle/0" && TxModel.Effective(null, "speedybee-f405-wing") == null, "effective model of a bench without the block: the family default: " + M(TxModel.Effective(null, "nexus-xr")));
        Check(f, TxModel.Effective(new(), "nexus-xr") is { Count: 0 } && M(TxModel.Effective(new() { ["7"] = new() { Type = "switch3" } }, "nexus-xr")) == "7=switch3//0", "an explicit block (even empty) is used as is, no default on top");
        var plainRs7 = BenchData.Parse("{\"name\":\"plain\",\"board\":\"nexus-xr\"}");
        Check(f, plainRs7.Transmitter == null && !JsonSerializer.Serialize(plainRs7, Json.Files).Contains("\"transmitter\"") && TxModel.Rows(TxModel.Effective(plainRs7.Transmitter, plainRs7.Board), null).Count == 1, "a bench file without the block stays without it, yet is driven with the throttle row");
        var oldRs7 = new BenchData { Name = "old rs7", Switches = new() { new() { Channel = 5, Name = "ARM" }, new() { Channel = 6, Name = "ANGLE" }, new() { Channel = 9, Name = "BEEPER" } }, Modes = snap };
        var mg3 = BenchData.Parse(JsonSerializer.Serialize(oldRs7, Json.Files).Replace("\"transmitter\"", "\"_t\""));
        Check(f, M(mg3.Transmitter) == "5=auto//0;6=switch2/throttle/0;9=auto/BEEPER/0", "an old RS7 bench (nexus-xr) with switches + modes, no block → the family default (ch6 throttle wins over the old ANGLE label) + labels where no range: " + M(mg3.Transmitter));
        var ex = BenchData.Parse("{\"name\":\"e\",\"switches\":[{\"channel\":6,\"name\":\"ANGLE\"}],\"transmitter\":{\"6\":{\"type\":\"switch2\",\"label\":\"throttle\"}}}");
        Check(f, M(ex.Transmitter) == "6=switch2/throttle/0", "an explicit block wins over the old switches: " + M(ex.Transmitter));
        // an explicit empty block = all auto on purpose: kept (no labels from the old switches), written back as {} and
        // still empty after a reload (it must not turn into a migrated block on the next load)
        var emp = BenchData.Parse("{\"name\":\"e0\",\"switches\":[{\"channel\":6,\"name\":\"ANGLE\"}],\"transmitter\":{}}");
        var empJson = JsonSerializer.Serialize(emp, Json.Files);
        var emp2 = BenchData.Parse(empJson);
        Check(f, emp.Transmitter is { Count: 0 } && empJson.Contains("\"transmitter\": {}") && emp2.Transmitter is { Count: 0 }, $"an explicit empty block: {M(emp.Transmitter)} → reload {M(emp2.Transmitter)}");
        // the old toggle-channel settings are ignored
        var st = AppSettings.Parse("{\"transmitter\":{\"armChannel\":9,\"modeChannel\":10,\"rescueChannel\":11}}");
        Check(f, st.Transmitter.Source![4] == "ui" && st.Transmitter.Source[5] == "ui" && st.Transmitter.Source[6] == "ui" && st.Transmitter.Source[9] == "none", "old ArmChannel / ModeChannel / RescueChannel ignored: " + string.Join(",", st.Transmitter.Source.Skip(4).Take(8)));
        // the wizard's default per firmware_family (the real board.json files)
        Check(f, BoardFamily.Of("nexus-xr") == "rotorflight" && BoardFamily.Of("speedybee-f405-wing") == "inav", $"firmware_family: nexus-xr '{BoardFamily.Of("nexus-xr")}', speedybee-f405-wing '{BoardFamily.Of("speedybee-f405-wing")}'");
        Check(f, M(TxModel.DefaultFor("nexus-xr")) == "5=auto//0;6=switch2/throttle/0" && TxModel.DefaultFor("speedybee-f405-wing") == null && TxModel.DefaultFor("no-such-board") == null, $"defaults: rf {M(TxModel.DefaultFor("nexus-xr"))}, inav {M(TxModel.DefaultFor("speedybee-f405-wing"))}");
        var nb = new BenchData { Board = "nexus-xr" };
        Wizard.ApplyBoardDefault(nb, editing: false);
        Check(f, M(nb.Transmitter) == "5=auto//0;6=switch2/throttle/0", "wizard, new RotorFlight bench: " + M(nb.Transmitter));
        nb.Board = "speedybee-f405-wing"; Wizard.ApplyBoardDefault(nb, editing: false);
        Check(f, nb.Transmitter == null, "wizard, the new bench's board changed to iNav: back to all auto: " + M(nb.Transmitter));
        var eb = new BenchData { Board = "nexus-xr" };
        Wizard.ApplyBoardDefault(eb, editing: true);
        Check(f, eb.Transmitter == null, "wizard, an edited bench keeps its model (no default forced): " + M(eb.Transmitter));
        var cust = new BenchData { Board = "nexus-xr", Transmitter = new() { ["7"] = new() { Type = "switch3" } } };
        Wizard.ApplyBoardDefault(cust, editing: false);
        Check(f, M(cust.Transmitter) == "7=switch3//0", "wizard, a model the user set is not replaced: " + M(cust.Transmitter));
        // static positions
        Check(f, string.Join(",", TxModel.Values("switch2")) == "1000,2000" && string.Join(",", TxModel.Values("switch3")) == "1000,1500,2000" && string.Join(",", TxModel.Values("switch6")) == "1165,1295,1425,1555,1685,1815", "static values");
        var s6 = TxModel.Static(8, new TxChannel { Type = "switch6" }, null);
        Check(f, s6.PositionOf(1230) == 0 && s6.PositionOf(1231) == 1 && s6.PositionOf(1360) == 1 && s6.PositionOf(1361) == 2 && s6.PositionOf(1490) == 2 && s6.PositionOf(1491) == 3 && s6.PositionOf(1620) == 3 && s6.PositionOf(1621) == 4 && s6.PositionOf(1749) == 4 && s6.PositionOf(1750) == 5,
              "switch6 read-back thresholds (ArduPilot ≤1230 / ≤1360 / ≤1490 / ≤1620 / ≤1749 / above)");
        Check(f, s6.Positions.Select((p, i) => s6.PositionOf(p.Us) == i).All(x => x), "every static value reads back as its own position");
        // rows: static + derived in channel order
        var rows = TxModel.Rows(Mixed(), snap);
        string R(DerivedSwitch x) => $"ch{x.Channel}{(x.Static ? "s" : "d")}:" + string.Join("|", x.Positions.Select(p => $"{p.Label}@{p.Us}"));
        var want = new[] { "ch5d:—@1000|ARM@2000", "ch6s:low@1000|high@2000", "ch7s:low@1000|ANGLE@1500|HORIZON@2000", "ch8s:1@1165|2@1295|3@1425|4@1555|5@1685|6@1815", "ch10d:—@1000|BEEPER@2000" };
        Check(f, rows.Select(R).SequenceEqual(want), "rows: " + string.Join(" ; ", rows.Select(R)));
        Check(f, rows.Count > 2 && rows[1].Label == "throttle" && rows[2].Ranges.Count == 2, "labels / ranges of the rows");
        Check(f, TxModel.Rows(null, null).Count == 0 && TxModel.Rows(new() { ["9"] = new() { Type = "auto" } }, snap).All(r => r.Channel != 9), "an auto channel without ranges → no row");
        Check(f, TxModel.StaticText(Mixed()) == "ch6 throttle: bench switch · ch7 mode: bench switch · ch8 flight mode: bench switch", "header text: " + TxModel.StaticText(Mixed()));
        // a static switch starts at its start (never on an ARM position nobody asked for)
        var armStatic = TxModel.Static(5, new TxChannel { Type = "switch2", Start = 1 }, snap.Switches()[0]);
        Check(f, armStatic.Positions[1].Label == "ARM" && armStatic.StartPosition(1500, driven: false, armId: 0) == 0 && armStatic.StartPosition(2000, driven: true, armId: 0) == 1, "a static switch on ARM: start 1 refused (ARM nobody asked for), kept when driven");
        var thr = TxModel.Static(6, new TxChannel { Type = "switch2", Start = 1 }, null);
        Check(f, thr.StartPosition(1000, driven: false) == 1 && thr.StartPosition(1000, driven: true) == 0, "start position: `start` when not driven, the channel's position when driven");
        // the bundled presets
        foreach (var file in Directory.Exists(Presets.Dir) ? Directory.GetFiles(Presets.Dir, "*.json") : Array.Empty<string>())
        {
            var p = BenchData.Parse(File.ReadAllText(file));
            var id = Path.GetFileNameWithoutExtension(file);
            if (id.StartsWith("rs7")) Check(f, M(p.Transmitter) == "5=auto//0;6=switch2/throttle/0", $"{id}: {M(p.Transmitter)}");
            else Check(f, p.Transmitter == null || p.Transmitter.Values.All(c => !c.IsStatic), $"{id}: static switches on an iNav preset: {M(p.Transmitter)}");
        }
        SelfTests.Report("tx_model", f.Count == 0, f.Count == 0
            ? "bench JSON round trip (transmitter {\"6\":{\"type\":\"switch2\",\"label\":\"throttle\"}…}), normalisation, migration of old `switches` (auto + labels where no range; with modes: ch6 only), explicit block wins, explicit empty block kept through a reload, old toggle channels ignored, wizard default per firmware_family (nexus-xr rotorflight → ch6 switch2 throttle; inav → all auto; edited / customised kept), switch2/3/6 values + ArduPilot thresholds, rows in channel order (static + derived, labels from ranges, auto without ranges → none), start position, presets' blocks"
            : string.Join("; ", f));
    }
}
