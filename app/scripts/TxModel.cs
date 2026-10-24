// The bench's transmitter model (`transmitter`, product decisions 2026-10-23: the Transmitter window is the radio, not the
// firmware's mode table). Every channel 5..16 has a type, stored in the bench (the meaning of a channel belongs to the
// aircraft; the joypad and the keys stay in settings.json):
//   auto     a switch derived from the firmware's modes (Modes.cs), exactly as before; no row until the firmware gives
//            ranges on the channel;
//   switch2  positions 1000 / 2000;  switch3  1000 / 1500 / 2000;
//   switch6  the ArduPilot flight-mode channel: 1165 / 1295 / 1425 / 1555 / 1685 / 1815, read back with ArduPilot's
//            thresholds (libraries/RC_Channel/RC_Channel.cpp, RC_Channel::read_6pos_switch: < 1231, < 1361, < 1491,
//            < 1621, < 1750, above).
// A static switch has a `label` (e.g. "throttle") and a `start` position (0 = lowest). The firmware's modes label its
// positions where their ranges cover the position's µs (the boxes active there, the same rule as the derived switches)
// and never rebuild it. JSON: {"5":{"type":"auto"},"6":{"type":"switch2","label":"throttle","start":0}}, keyed by the
// 1-based channel; an absent channel (or an absent block) = auto.
// Pure model: no Godot nodes; the flight screen (Flight.Modes.cs), the wizard and the self-tests use it.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Serialization;

namespace FlightBench;

/// One channel of the bench's transmitter model.
public sealed class TxChannel
{
    public string Type { get; set; } = TxModel.Auto;
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Label { get; set; }
    /// The position a static switch starts at (0 = lowest); left out of the file when 0.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingDefault)]
    public int Start { get; set; }
    [JsonIgnore] public bool IsStatic => Type != TxModel.Auto;
}

public static class TxModel
{
    public const string Auto = "auto", Switch2 = "switch2", Switch3 = "switch3", Switch6 = "switch6";
    public static readonly string[] Types = { Auto, Switch2, Switch3, Switch6 };
    /// The editor's names for the types (same order).
    public static readonly string[] TypeNames = { "auto", "switch 2", "switch 3", "switch 6 (ArduPilot)" };
    public const int FirstChannel = 5, LastChannel = 16;

    /// The µs a static switch sends per position.
    public static int[] Values(string type) => type switch
    {
        Switch2 => new[] { 1000, 2000 },
        Switch3 => new[] { 1000, 1500, 2000 },
        Switch6 => new[] { 1165, 1295, 1425, 1555, 1685, 1815 },
        _ => Array.Empty<int>(),
    };

    /// The position edges [e0, e1), [e1, e2)… over 900..2100 µs a channel value is read back with (switch6: ArduPilot's
    /// read_6pos_switch thresholds; switch2 / switch3: halfway between the values).
    public static int[] Edges(string type) => type switch
    {
        Switch2 => new[] { ModesSnapshot.MinUs, 1500, ModesSnapshot.MaxUs },
        Switch3 => new[] { ModesSnapshot.MinUs, 1250, 1750, ModesSnapshot.MaxUs },
        Switch6 => new[] { ModesSnapshot.MinUs, 1231, 1361, 1491, 1621, 1750, ModesSnapshot.MaxUs },
        _ => Array.Empty<int>(),
    };

    /// The names a static type's positions carry where the firmware's modes give none (owner, 2026-10-23): a throttle
    /// switch reads `low | high`, not `— | —` ("—" on a derived switch means "no box here"; on a radio switch it told the
    /// pilot nothing). switch6 positions are `1..6`, ArduPilot's FLTMODE1..6.
    public static string[] DefaultNames(string type) => type switch
    {
        Switch2 => new[] { "low", "high" },
        Switch3 => new[] { "low", "mid", "high" },
        Switch6 => new[] { "1", "2", "3", "4", "5", "6" },
        _ => Array.Empty<string>(),
    };

    public static string TypeName(string type) => Array.IndexOf(Types, type) is var i and >= 0 ? TypeNames[i] : type;

    /// The firmware family of a board (`boards/<board>/board.json` → `firmware_family`); "" unknown. Replaceable in tests.
    public static Func<string, string> FamilyOf = BoardFamily.Of;

    /// The model a new bench gets on this board (decision 3, 2026-10-23): RotorFlight → ch5 auto, ch6 switch2 "throttle"
    /// (high = throttle fully on: the governor spools the head; low = throttle hold); every other family: all auto (null).
    public static Dictionary<string, TxChannel>? DefaultFor(string board) =>
        string.Equals(FamilyOf(board), "rotorflight", StringComparison.OrdinalIgnoreCase)
            ? new Dictionary<string, TxChannel> { ["5"] = new() { Type = Auto }, ["6"] = new() { Type = Switch2, Label = "throttle", Start = 0 } }
            : null;

    /// The channel's entry (null = auto, no label).
    public static TxChannel? Entry(Dictionary<string, TxChannel>? model, int channel1) =>
        model != null && model.TryGetValue(channel1.ToString(), out var c) ? c : null;

    /// After loading: unknown channels dropped, an unknown type = auto, labels trimmed, start clamped to the type's
    /// positions. An explicit block stays a block even when empty (`"transmitter": {}` = all auto, written back as such),
    /// so a bench whose model was cleared on purpose is never migrated from its old `switches` on the next load.
    public static Dictionary<string, TxChannel>? Normalize(Dictionary<string, TxChannel>? model)
    {
        if (model == null) return null;
        var r = new Dictionary<string, TxChannel>();
        foreach (var (k, v) in model.OrderBy(kv => int.TryParse(kv.Key, out var n) ? n : 99))
        {
            if (v == null || !int.TryParse(k.Trim(), out var ch) || ch < FirstChannel || ch > LastChannel) continue;
            var type = (v.Type ?? Auto).Trim().ToLowerInvariant();
            if (Array.IndexOf(Types, type) < 0) type = Auto;
            var label = string.IsNullOrWhiteSpace(v.Label) ? null : v.Label.Trim();
            int n2 = Values(type).Length;
            r[ch.ToString()] = new TxChannel { Type = type, Label = label, Start = n2 == 0 ? 0 : Math.Clamp(v.Start, 0, n2 - 1) };
        }
        return r;
    }

    /// The model a bench is shown and driven with: its own block, else its board's family default (owner, 2026-10-23
    /// evening: a bench saved without the block — every bench copied from the RS7 preset before the change — gets the
    /// throttle switch like a fresh one). The file is not rewritten for it: the block is written only when the model is
    /// edited (Edit bench → Transmitter writes the effective model), so a bench file round-trips unchanged. An explicit
    /// empty block (`{}`) is "all auto on purpose" and gets no default.
    public static Dictionary<string, TxChannel>? Effective(Dictionary<string, TxChannel>? model, string board) => model ?? DefaultFor(board);

    /// Migration of a bench without the block but with an old `switches` list (saved before 2026-10-23): the board's
    /// family default first, then an old switch name is kept as the label of its channel where the default has no static
    /// switch there and no firmware range will come (the bench's `modes` snapshot has no range on it). No static switch
    /// is made from old switch data: the old layout was the firmware's modes anyway. Null without old switches (the
    /// family default then applies at view time, Effective) or when nothing is kept.
    public static Dictionary<string, TxChannel>? Migrate(string board, List<BenchSwitch>? switches, ModesSnapshot? modes)
    {
        if (switches == null) return null;
        var r = DefaultFor(board) ?? new Dictionary<string, TxChannel>();
        var ranged = modes?.Switches().Select(s => s.Channel).ToHashSet() ?? new HashSet<int>();
        foreach (var s in switches ?? new List<BenchSwitch>())
            if (s.Channel is >= FirstChannel and <= LastChannel && !ranged.Contains(s.Channel) && !string.IsNullOrWhiteSpace(s.Name)
                && !(r.TryGetValue(s.Channel.ToString(), out var e) && e.IsStatic))
                r[s.Channel.ToString()] = new TxChannel { Type = Auto, Label = s.Name.Trim() };
        return r.Count == 0 ? null : r;
    }

    /// The Transmitter window's rows, in channel order: a static switch on every static channel (its positions labelled
    /// with the firmware's boxes where its modes cover them), a derived switch on an auto channel the modes give ranges on.
    public static List<DerivedSwitch> Rows(Dictionary<string, TxChannel>? model, ModesSnapshot? modes)
    {
        var derived = modes?.Switches() ?? new List<DerivedSwitch>();
        var rows = new List<DerivedSwitch>();
        for (int ch = FirstChannel; ch <= LastChannel; ch++)
        {
            var e = Entry(model, ch);
            var d = derived.Find(s => s.Channel == ch);
            if (e is { IsStatic: true }) rows.Add(Static(ch, e, d));
            else if (d != null) { d.Label = e?.Label; rows.Add(d); }
        }
        return rows;
    }

    /// A static switch: its type's values; each position's boxes = those of the derived switch's position its µs falls in;
    /// a position with no box carries the type's own name (DefaultNames).
    public static DerivedSwitch Static(int channel1, TxChannel e, DerivedSwitch? fw)
    {
        var values = Values(e.Type);
        var edges = Edges(e.Type);
        var names = DefaultNames(e.Type);
        var sw = new DerivedSwitch { Aux = channel1 - 5, Rx = channel1 - 1, Static = true, Type = e.Type, Label = e.Label, Start = Math.Clamp(e.Start, 0, values.Length - 1) };
        for (int i = 0; i < values.Length; i++)
        {
            var p = new SwitchPosition { StartUs = edges[i], EndUs = edges[i + 1], Us = values[i] };
            if (fw != null)
            {
                var at = fw.Positions[fw.PositionOf(values[i])];
                p.BoxIds = new List<int>(at.BoxIds); p.Names = new List<string>(at.Names);
            }
            if (p.Names.Count == 0 && i < names.Length) p.Names = new List<string> { names[i] };
            sw.Positions.Add(p);
        }
        if (fw != null) sw.Ranges = fw.Ranges;
        return sw;
    }

    /// "ch6 throttle: bench switch" parts of the Transmitter header (static channels, "·" joined); "" when none.
    public static string StaticText(Dictionary<string, TxChannel>? model) =>
        string.Join(" · ", (model ?? new()).Where(kv => kv.Value.IsStatic).OrderBy(kv => int.Parse(kv.Key))
                                            .Select(kv => $"ch{kv.Key}{(kv.Value.Label != null ? " " + kv.Value.Label : "")}: bench switch"));
}

/// `boards/<board>/board.json` → `firmware_family` (cached per board).
public static class BoardFamily
{
    private static readonly Dictionary<string, string> Cache = new();
    public static string Of(string board)
    {
        if (string.IsNullOrEmpty(board)) return "";
        if (Cache.TryGetValue(board, out var f)) return f;
        f = "";
        try
        {
            var path = Repo.Path("boards", board, "board.json");
            if (System.IO.File.Exists(path))
            {
                using var doc = System.Text.Json.JsonDocument.Parse(System.IO.File.ReadAllText(path));
                f = Json.Str(doc.RootElement, "firmware_family") ?? "";
            }
        }
        catch (Exception) { f = ""; }
        return Cache[board] = f;
    }
}
