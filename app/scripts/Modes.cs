// The firmware's flight modes as the core reads them over MSP (docs/protocol.md `modes`: MSP_BOXNAMES +
// MSP_BOXIDS + MSP_MODE_RANGES; read-only — the configurator writes them) and the transmitter switches the app
// derives from them. Rule (docs/ui.md "Switches from the firmware's modes"): per aux channel that has ranges,
// the channel's span 900..2100 µs is cut at every range start / end; each piece gets the set of boxes whose
// range covers it (a range is active for start ≤ µs < end, as iNav / Betaflight / RotorFlight test it);
// neighbouring pieces with the same set merge. What is left are the switch's positions (2 for one range at an
// end, 3 for a range in the middle or two stacked ranges, more when the ranges say so), each labelled with its
// boxes joined by "+" ("ARM", "ANGLE+NAV LAUNCH") or "—" for none. Position i of n is sent as a real switch gives
// it (1000 + 1000·i/(n−1): 1000 / 2000, 1000 / 1500 / 2000) when that lies inside it, else as the nearest of
// 1000 / 1500 / 2000 inside it, else as its middle — "inside" with a margin of EdgeUs from both ends, so a value
// never sits on a range's edge (the CRSF round trip is ±1 µs; a 3-position switch at 1500 against a range that
// starts at 1500 would be a coin toss). The switch's channel is the receiver channel the firmware reads the aux
// channel from: `rx_channel` (0-based, after the firmware's RX map) when the core gives it, else aux + 4 (ch5 =
// AUX1: iNav, and RotorFlight with the RS7's map AECR1T23). A range `linked` to another box (RotorFlight
// MODE_RANGES_EXTRA) follows that box, not the channel: left out of the positions. RotorFlight's `logic:"and"`: a box
// whose slots are AND-ed is active only while all of them are (its OR slots still turn it on alone); on a switch the
// box counts where all its AND ranges on that channel cover the piece, and its label names the other conditions:
// "ARM & ch8" (an AND slot on another channel), "ARM & ANGLE" (an AND slot linked to the ANGLE box).
// Pure model: no Godot nodes; the flight screen (Flight.Modes.cs) and the self-tests use it.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace FlightBench;

/// One mode box of the firmware: permanent id (stable across firmware versions), index (the firmware's order,
/// the bit in its status bitmask), name.
public sealed class ModeBox
{
    public int Id { get; set; }
    public int Index { get; set; }
    public string Name { get; set; } = "";
}

/// One mode activation range: aux channel (0-based: 0 = ch5), box by permanent id, [start, end) in µs.
public sealed class ModeRange
{
    public int Channel { get; set; }
    /// The receiver channel (0-based) the firmware reads this aux channel from (`rx_channel`; null: not given → aux + 4).
    [JsonPropertyName("rx_channel")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public int? RxChannel { get; set; }
    /// RotorFlight MODE_RANGES_EXTRA: the box this range follows (0 / null = none: a channel range).
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public int? Linked { get; set; }
    /// RotorFlight `logic`: "and" = AND-ed with the box's other AND slots (null / "or": any slot turns the box on).
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Logic { get; set; }
    /// The receiver channel (0-based) the app drives for this range.
    [JsonIgnore] public int Rx => RxChannel ?? Channel + 4;
    [JsonIgnore] public bool IsAnd => string.Equals(Logic, "and", StringComparison.OrdinalIgnoreCase);
    public int Box { get; set; }
    [JsonPropertyName("start_us")] public int StartUs { get; set; }
    [JsonPropertyName("end_us")] public int EndUs { get; set; }
}

/// The last `modes` message (also the bench's informational `modes` snapshot).
public sealed class ModesSnapshot
{
    public List<ModeBox> Boxes { get; set; } = new();
    public List<ModeRange> Ranges { get; set; } = new();
    /// The firmware family the core read the modes from (`variant`: "INAV", "RTFL", "BTFL"; null: not given).
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Variant { get; set; }
    /// Core time of the message (null: not given).
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public double? T { get; set; }
    /// When the app received it (local time).
    public DateTime Received { get; set; }

    public const int MinUs = 900, MaxUs = 2100;

    /// A `modes` message → snapshot (null: not a usable message). Unknown / malformed entries are skipped.
    public static ModesSnapshot? FromMessage(JsonElement m, DateTime now)
    {
        if (m.ValueKind != JsonValueKind.Object) return null;
        var s = new ModesSnapshot { Received = now };
        if (Json.TryNum(m, "t", out var t)) s.T = t;
        s.Variant = Json.Str(m, "variant");
        if (m.TryGetProperty("boxes", out var bx) && bx.ValueKind == JsonValueKind.Array)
            foreach (var b in bx.EnumerateArray())
            {
                if (b.ValueKind != JsonValueKind.Object || !Json.TryNum(b, "id", out var id)) continue;
                s.Boxes.Add(new ModeBox { Id = (int)id, Index = (int)Json.Num(b, "index", -1), Name = Json.Str(b, "name") ?? $"box {id}" });
            }
        else return null;
        if (m.TryGetProperty("ranges", out var rg) && rg.ValueKind == JsonValueKind.Array)
            foreach (var r in rg.EnumerateArray())
            {
                if (r.ValueKind != JsonValueKind.Object || !Json.TryNum(r, "channel", out var ch) || !Json.TryNum(r, "box", out var box)) continue;
                int? linked = Json.TryNum(r, "linked", out var ln) && ln > 0 ? (int)ln : null;
                bool hasUs = Json.TryNum(r, "start_us", out var st) & Json.TryNum(r, "end_us", out var en);
                if (!hasUs && linked == null) continue;                 // an unused slot (a linked one has no µs: kept for the AND labels)
                s.Ranges.Add(new ModeRange
                {
                    Channel = (int)ch, Box = (int)box, StartUs = hasUs ? (int)st : 0, EndUs = hasUs ? (int)en : 0,
                    RxChannel = Json.TryNum(r, "rx_channel", out var rx) ? (int)rx : null,
                    Linked = linked, Logic = Json.Str(r, "logic"),
                });
            }
        return s;
    }

    /// After loading from a bench file: nulls → empty.
    public ModesSnapshot Normalize()
    {
        Boxes = (Boxes ?? new()).Where(b => b != null).ToList();
        Ranges = (Ranges ?? new()).Where(r => r != null).ToList();
        foreach (var b in Boxes) b.Name ??= "";
        return this;
    }

    public string BoxName(int id) => Boxes.Find(b => b.Id == id) is { } b && b.Name != "" ? b.Name : $"box {id}";

    /// The ranges a switch can select: an aux channel on a receiver channel the app sends past the sticks (ch5..ch16),
    /// end above start, inside 900..2100, not linked to another box.
    public IEnumerable<ModeRange> UsableRanges() =>
        Ranges.Where(r => r.Channel >= 0 && r.Rx is >= 4 and <= 15 && (r.Linked is null or 0) && Math.Min(r.EndUs, MaxUs) > Math.Max(r.StartUs, MinUs));

    /// The switches: one per aux channel with usable ranges, in channel order.
    public List<DerivedSwitch> Switches()
    {
        var list = new List<DerivedSwitch>();
        foreach (var g in UsableRanges().GroupBy(r => r.Rx).OrderBy(g => g.Key))
        {
            var ranges = g.OrderBy(r => r.StartUs).ThenBy(r => r.EndUs).ToList();
            var cuts = new SortedSet<int> { MinUs, MaxUs };
            foreach (var r in ranges) { cuts.Add(Math.Clamp(r.StartUs, MinUs, MaxUs)); cuts.Add(Math.Clamp(r.EndUs, MinUs, MaxUs)); }
            var edges = cuts.ToList();
            var positions = new List<SwitchPosition>();
            var andOnly = new HashSet<int>();                    // boxes turned on here only through their AND slots
            for (int i = 0; i + 1 < edges.Count; i++)
            {
                int a = edges[i], b = edges[i + 1];
                // the boxes active over [a, b), each once, in the channel's range order: an OR range covering it, or all of
                // the box's AND ranges on this channel covering it
                bool Covers(ModeRange r) => r.StartUs <= a && r.EndUs >= b;
                var ids = new List<int>();
                foreach (var box in ranges.Select(r => r.Box).Distinct())
                {
                    var and = ranges.Where(r => r.Box == box && r.IsAnd).ToList();
                    bool or = ranges.Any(r => r.Box == box && !r.IsAnd && Covers(r));
                    bool andOn = and.Count > 0 && and.All(Covers);
                    if (or || andOn) ids.Add(box);
                    if (!or && andOn) andOnly.Add(box);
                }
                if (positions.Count > 0 && positions[^1].BoxIds.SequenceEqual(ids)) { positions[^1].EndUs = b; continue; }
                positions.Add(new SwitchPosition { StartUs = a, EndUs = b, BoxIds = ids });
            }
            for (int i = 0; i < positions.Count; i++)
            {
                var p = positions[i];
                p.Names = p.BoxIds.Select(id => andOnly.Contains(id) ? AndLabel(id, g.Key) : BoxName(id)).ToList();
                p.Us = SwitchPosition.SendValue(p.StartUs, p.EndUs, i, positions.Count);
            }
            list.Add(new DerivedSwitch { Aux = ranges[0].Channel, Rx = g.Key, Positions = positions, Ranges = ranges.Select(r => $"{BoxName(r.Box)} {r.StartUs}–{r.EndUs}").ToList() });
        }
        return list;
    }

    /// "ARM & ch8" / "ARM & ANGLE": a box with AND slots, named with its AND conditions elsewhere (another receiver channel,
    /// a linked box); just the name when all its AND slots are on this channel.
    public string AndLabel(int box, int rx)
    {
        var others = Ranges.Where(r => r.Box == box && r.IsAnd && (r.Linked is > 0 || r.Rx != rx))
                           .Select(r => r.Linked is int l && l > 0 ? BoxName(l) : $"ch{r.Rx + 1}").Distinct().ToList();
        return others.Count == 0 ? BoxName(box) : BoxName(box) + " & " + string.Join(" & ", others);
    }

    /// The bench's static `switches` refreshed from the snapshot: one per derived switch ("ch6 ANGLE / ANGLE+NAV LAUNCH").
    public List<BenchSwitch>? AsBenchSwitches()
    {
        var sw = Switches();
        if (sw.Count == 0) return null;
        return sw.Select(s => new BenchSwitch
        {
            Channel = s.Channel,
            Name = string.Join(" / ", s.Positions.Where(p => p.BoxIds.Count > 0).Select(p => p.Label).Distinct()),
            Note = "positions " + string.Join(" | ", s.Positions.Select(p => $"{p.Label} {p.StartUs}–{p.EndUs}")) + " (from the firmware's modes)",
        }).ToList();
    }

    /// The boxes that have a range, each once, in channel / range order (the Status window's mode row).
    public List<int> RangedBoxIds()
    {
        var ids = new List<int>();
        foreach (var s in Switches()) foreach (var p in s.Positions) foreach (var id in p.BoxIds) if (!ids.Contains(id)) ids.Add(id);
        return ids;
    }

    /// Active boxes of a `state` message as permanent ids: `fc.boxes` = the ids themselves (the core sends them once it
    /// has read the modes); else the names of `fc.mode` ("ANGLE+NAV LAUNCH", `ACRO` = none) + ARM from `fc.armed`, mapped
    /// through the snapshot's names. Null when the message says nothing about modes.
    public List<int>? ActiveIds(JsonElement msg)
    {
        if (Json.TryNode(msg, "fc.boxes", out var arr) && arr.ValueKind == JsonValueKind.Array)
            return arr.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.Number).Select(x => (int)x.GetDouble()).Distinct().ToList();
        var mode = Json.Str(msg, "fc.mode");
        bool hasArmed = Json.TryBool(msg, "fc.armed", out var armed);
        if (mode == null && !hasArmed) return null;
        var names = (mode ?? "").Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList();
        if (armed) names.Insert(0, "ARM");
        var ids = new List<int>();
        foreach (var n in names)
            if (Boxes.Find(b => string.Equals(b.Name, n, StringComparison.OrdinalIgnoreCase)) is { } b && !ids.Contains(b.Id)) ids.Add(b.Id);
        return ids;
    }

    /// "t = 12.34 s" / "" for the Transmitter header.
    public string TimeText() => T is { } t ? t.ToString("0.00", CultureInfo.InvariantCulture) : "?";
}

/// A switch derived from the modes: aux channel, positions low → high.
public sealed class DerivedSwitch
{
    public int Aux;                                   // 0-based aux index (0 = AUX1)
    public int Rx;                                    // 0-based receiver channel (4 = ch5)
    public int Channel => Rx + 1;                     // 1-based CRSF channel
    public List<SwitchPosition> Positions = new();
    public List<string> Ranges = new();               // "ARM 1700–2100" for the tooltip
    /// A static switch of the bench's transmitter model (TxModel: switch2 / switch3 / switch6): fixed positions, labelled
    /// by the firmware's boxes where its modes cover them; false = derived from the modes (`auto`).
    public bool Static;
    public string Type = TxModel.Auto;
    /// The bench's label of the channel ("throttle"); null = none.
    public string? Label;
    /// A static switch's start position (the bench's `start`).
    public int Start;

    /// The position a channel value falls in (clamped to the ends).
    public int PositionOf(int us)
    {
        for (int i = 0; i < Positions.Count; i++) if (us < Positions[i].EndUs) return i;
        return Positions.Count - 1;
    }

    /// Where a switch starts when the app drives it: the position of the current value, but never an ARM position
    /// that nothing asked for (a channel nobody drove sits at 1500): then the lowest position without ARM.
    public int StartPosition(int us, bool driven, int armId = 0)
    {
        int p = Static && !driven ? Math.Clamp(Start, 0, Positions.Count - 1) : PositionOf(us);   // a static switch starts at its `start`
        if (driven || !Positions[p].BoxIds.Contains(armId)) return p;
        int safe = Positions.FindIndex(x => !x.BoxIds.Contains(armId));
        return safe >= 0 ? safe : p;
    }

    public string Describe() => $"ch{Channel}{(Label != null ? " " + Label : "")} ({(Static ? $"bench {Type}" : $"AUX{Aux + 1}")}): " + string.Join(" | ", Positions.Select(p => p.Label));
}

public sealed class SwitchPosition
{
    public int StartUs, EndUs;                        // [start, end)
    public List<int> BoxIds = new();
    public List<string> Names = new();
    public int Us;                                    // the value sent for it
    public string Label => Names.Count == 0 ? "—" : string.Join("+", Names);

    /// Distance a sent value keeps from a position's ends (the CRSF round trip moves a value by up to 1 µs).
    public const int EdgeUs = 5;

    /// The value a real switch would give: position i of n at 1000 + 1000·i/(n−1) (2 positions: 1000 / 2000, 3: 1000 /
    /// 1500 / 2000) when that lies inside [start + EdgeUs, end − EdgeUs); else the one of 1000 / 1500 / 2000 inside that
    /// nearest to it; else the middle. Always inside [start, end) for end > start.
    public static int SendValue(int start, int end, int i, int n)
    {
        int want = n <= 1 ? 1500 : 1000 + (int)Math.Round(1000.0 * i / (n - 1));
        bool In(int v) => v >= start + EdgeUs && v < end - EdgeUs;
        if (In(want)) return want;
        int best = -1;
        foreach (var v in new[] { 1000, 1500, 2000 })
            if (In(v) && (best < 0 || Math.Abs(v - want) < Math.Abs(best - want))) best = v;
        return best >= 0 ? best : start + (end - start) / 2;
    }
}

/// The `osd` object of `hello` / `state` → the Status window's sensors line.
public static class OsdLine
{
    /// null = the message does not say (an older core: no line); else "OSD: …".
    public static string? From(JsonElement msg)
    {
        if (msg.ValueKind != JsonValueKind.Object || !msg.TryGetProperty("osd", out var o)) return null;
        if (o.ValueKind is JsonValueKind.Null or JsonValueKind.False) return "OSD: none";
        if (o.ValueKind == JsonValueKind.String) return Text(o.GetString() ?? "", 0, 0, null);
        if (o.ValueKind != JsonValueKind.Object) return null;
        bool? video = Json.TryBool(o, "video", out var v) ? v : null;
        return Text(Json.Str(o, "kind") ?? "", (int)Json.Num(o, "cols", 0), (int)Json.Num(o, "rows", 0), video);
    }

    public static string Text(string kind, int cols, int rows, bool? video)
    {
        var size = cols > 0 && rows > 0 ? $" {cols}×{rows}" : "";
        return kind.Trim().ToLowerInvariant() switch
        {
            "" or "none" => "OSD: none",
            "max7456" => $"OSD: MAX7456{size}{(video == true ? "" : " (no video in FlightBench)")}",
            "msp_displayport" or "msp" or "displayport" => $"OSD: MSP DisplayPort{size}",
            var k => $"OSD: {k}{size}",
        };
    }
}

/// The firmware's applied config as `diff all` text (the `diff` message); the bench keeps the last one (`firmware_diff`).
public sealed class FirmwareDiff
{
    public string Text { get; set; } = "";
    public bool Stale { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public double? T { get; set; }
    public DateTime Received { get; set; }
    /// Where the core took it: `first_start` (the first start's CLI session, what was saved), `snapshot` (restored with the
    /// config snapshot), `request` (a `diff` command); null: not said.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Source { get; set; }
    /// Why it could not be refreshed (`armed`, `calibrating`, `cli`, `busy`, `not_ready`…): the message's `error`; not saved.
    [JsonIgnore] public string? Error { get; set; }
    /// Sent again to a client that connected after it (`replay`); not saved.
    [JsonIgnore] public bool Replay { get; set; }

    public static string SourceText(string? source) => source switch
    {
        "first_start" => "first start",
        "snapshot" => "saved snapshot",
        "request" => "on request",
        null or "" => "",
        var x => x,
    };

    public static FirmwareDiff? FromMessage(JsonElement m, DateTime now)
    {
        if (m.ValueKind != JsonValueKind.Object || Json.Str(m, "text") is not { } text) return null;
        // `at` = when the dump was taken (a stale re-send keeps it), else the message's `t`
        double? when = Json.TryNum(m, "at", out var at) ? at : Json.TryNum(m, "t", out var t) ? t : null;
        return new FirmwareDiff
        {
            Text = text.Replace("\r\n", "\n"), Stale = Json.TryBool(m, "stale", out var s) && s, T = when, Received = now,
            Source = Json.Str(m, "source"), Error = Json.Str(m, "error"), Replay = Json.TryBool(m, "replay", out var rp) && rp,
        };
    }

    /// "diff all from the firmware (first start, t = 41.20 s, 2026-10-01 14:02), 38 lines" (+ " — STALE …").
    public string Header(bool fromBench)
    {
        var lines = Text.Split('\n').Count(l => l.Trim() != "");
        var when = Received == default ? "" : Received.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture);
        var t = T is { } tt ? $"t = {tt.ToString("0.00", CultureInfo.InvariantCulture)} s, " : "";
        var src = SourceText(Source) is { Length: > 0 } st ? st + ", " : "";
        var stale = !Stale ? "" : Error != null ? $" — STALE ({Error}): the core could not refresh it"
                                                : " — STALE: the firmware's config may have changed since (Refresh from firmware reads it again)";
        return $"diff all {(fromBench ? "from the bench (last seen " : "from the firmware (")}{src}{t}{when}), {lines} lines{stale}";
    }
}
