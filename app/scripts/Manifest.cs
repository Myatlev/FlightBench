// The core's manifest (docs/protocol.md `manifest`): boards and their pads (printed label, firmware id,
// kind, flags, onboard devices; the older `slots` view as fallback), device models with their ports /
// props / faults / signals, aircraft with their actuators. It is the app's catalogue: the wiring
// graph's device palette, port kinds, board slots and actuator lists, the inspector's fields and the
// flight screen's Faults window are all built from Manifest.Current.
// Sources, in order: the live `manifest` from the core (cached to user://manifest.json when it
// arrives) → FB_MANIFEST=<path> (dev: an offline sample, e.g. `core/qemu/run.sh --manifest` output)
// → the cached user://manifest.json → the built-in fallback (the catalogue the app hard-coded before
// the manifest existed: 7 devices, no faults; slots from the board's unified config, actuators from
// the model README). Self-test runs (DevEnv.SelfTestRun) are pinned: FB_MANIFEST, else the bundled
// fixture res://tests/manifest-fixture.json, never the cache; a live manifest is ignored (not cached).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using Godot;

namespace FlightBench;

/// A device prop or a fault param. Types: int | float | bool | enum | actuator.
public sealed class ManifestParam
{
    public string Id { get; set; } = "";
    public string Label { get; set; } = "";
    public string Type { get; set; } = "";
    public double? Min { get; set; }
    public double? Max { get; set; }
    public JsonElement? Default { get; set; }
    public string[] Options { get; set; } = Array.Empty<string>();
    public bool Optional { get; set; }

    /// The default as the text stored in WiringEntry.Props ("" = none).
    [JsonIgnore]
    public string DefaultText => Default is not { } d ? "" : d.ValueKind switch
    {
        JsonValueKind.String => d.GetString() ?? "",
        JsonValueKind.True => "true",
        JsonValueKind.False => "false",
        JsonValueKind.Number => d.GetRawText(),
        _ => "",
    };

    /// A text value → the JSON value the core expects (number / bool / string).
    public object Typed(string v) => Type switch
    {
        "bool" => v == "true",
        "int" => long.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out var i) ? i : (object)v,
        "float" => double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out var f) ? f : (object)v,
        _ => v,
    };
}

public sealed class ManifestFault
{
    public string Id { get; set; } = "";
    public string Label { get; set; } = "";
    public List<ManifestParam> Params { get; set; } = new();
}

public sealed class ManifestDevice
{
    public string Id { get; set; } = "";
    public string Label { get; set; } = "";
    public string Category { get; set; } = "";
    public string Kind { get; set; } = "";          // slot kind it plugs into: spi | i2c | uart | motor | servo (| anything new)
    public List<ManifestParam> Props { get; set; } = new();
    public List<ManifestFault> Faults { get; set; } = new();
    public string[] Signals { get; set; } = Array.Empty<string>();
    public bool Implemented { get; set; } = true;

    /// The device's connections (manifest `ports`); a manifest without them → one port of the device's `kind`.
    public List<ManifestPort> Ports { get; set; } = new();
    /// false = `ports` came from the old `kind` (the session then leaves `port` out: the core takes the first).
    [JsonIgnore] public bool PortsDeclared { get; private set; } = true;

    [JsonIgnore] public PortKind Port => WiringCatalog.PortOf(Kind);
    /// The primary port (the first; its kind is the device's `kind`).
    [JsonIgnore] public ManifestPort Primary => Ports[0];
    /// Ports that get their own wire in the graph (a `bundled` one rides on the primary's pad).
    [JsonIgnore] public IEnumerable<ManifestPort> WiredPorts => Ports.Where(p => !p.Bundled);
    public ManifestPort? PortById(string id) => Ports.Find(p => p.Id == id);

    internal void NormalizePorts()
    {
        Kind ??= "";
        Ports = (Ports ?? new()).Where(p => p != null && !string.IsNullOrEmpty(p.Id)).ToList();
        foreach (var p in Ports) { p.Kind = string.IsNullOrEmpty(p.Kind) ? Kind : p.Kind; p.Dir = string.IsNullOrEmpty(p.Dir) ? "both" : p.Dir; p.Label ??= ""; }
        PortsDeclared = Ports.Count > 0;
        if (!PortsDeclared) Ports.Add(new ManifestPort { Id = Kind != "" ? Kind : Id, Kind = Kind, Dir = "both", Label = Kind });
        if (Kind == "") Kind = Ports[0].Kind;
    }

    /// Label for lists: "(not modelled yet)" when the core has not wired the model in.
    [JsonIgnore] public string DisplayLabel => Implemented ? Label : $"{Label} (not modelled yet)";
    public ManifestParam? Prop(string id) => Props.Find(p => p.Id == id);
}

/// A device's connection: `kind` = the pad kind it plugs into, `dir` seen from the device (in | out | both;
/// a UART port `out` needs the pad's RX, `in` its TX), `optional` = works without it, `bundled` = comes
/// with the primary port's pad (the gyro's INT), never wired alone.
public sealed class ManifestPort
{
    public string Id { get; set; } = "";
    public string Kind { get; set; } = "";
    public string Dir { get; set; } = "both";
    public string Label { get; set; } = "";
    public bool Optional { get; set; }
    public bool Bundled { get; set; }

    [JsonIgnore] public PortKind PortKind => WiringCatalog.PortOf(Kind);
}

/// A board pad: firmware id (`uart4`), printed label (`PORT-A`, null = not confirmed), kind, flags
/// (`rx_only`, `tx_only`, `5v`, `usb`), pins; an onboard device sits on it (`onboard:true` + `device`,
/// `detachable`, `part` — or `onboard:{device, detachable, part}`).
public sealed class ManifestPad
{
    public string Id { get; set; } = "";
    public string? Label { get; set; }
    public string Kind { get; set; } = "";
    public string[] Flags { get; set; } = Array.Empty<string>();
    public Dictionary<string, string>? Pins { get; set; }
    public string? Bus { get; set; }
    public string? Hint { get; set; }
    public string? Note { get; set; }
    public JsonElement? Onboard { get; set; }
    public string? Device { get; set; }
    public bool? Detachable { get; set; }
    public string? Part { get; set; }
    public string[] Devices { get; set; } = Array.Empty<string>();   // what the machine models on it

    /// The onboard device's id ("" = none).
    [JsonIgnore] public string OnboardDevice { get; private set; } = "";
    [JsonIgnore] public bool IsDetachable { get; private set; }
    [JsonIgnore] public string OnboardPart { get; private set; } = "";
    [JsonIgnore] public bool HasOnboard => OnboardDevice != "";
    [JsonIgnore] public PortKind PortKind => WiringCatalog.PortOf(Kind);
    public bool Has(string flag) => Flags.Contains(flag);

    /// Can this pad carry what the board sends (TX) / receives (RX)? From the pins when the manifest
    /// gives them, else from the rx_only / tx_only flags; USB carries both.
    [JsonIgnore] public bool HasTx => Has("usb") || (Pins is { Count: > 0 } ? Pins.ContainsKey("tx") : !Has("rx_only"));
    [JsonIgnore] public bool HasRx => Has("usb") || (Pins is { Count: > 0 } ? Pins.ContainsKey("rx") : !Has("tx_only"));

    /// "PORT-A · uart4" / "servo1 · S1?" / "motor1 · no label": the printed name first.
    [JsonIgnore] public string MainText => Label ?? Id;
    [JsonIgnore] public string SecondText => Label != null ? Id : Hint != null ? $"{Hint}?" : "no label";

    internal void Normalize()
    {
        Flags ??= Array.Empty<string>(); Devices ??= Array.Empty<string>(); Kind ??= "";
        if (Label != null && Label.Trim() == "") Label = null;
        OnboardDevice = ""; IsDetachable = false; OnboardPart = Part ?? "";
        if (Onboard is { } o)
        {
            if (o.ValueKind == JsonValueKind.True && !string.IsNullOrEmpty(Device)) { OnboardDevice = Device!; IsDetachable = Detachable ?? false; }
            else if (o.ValueKind == JsonValueKind.Object)
            {
                OnboardDevice = Json.Str(o, "device") ?? Device ?? "";
                IsDetachable = Json.TryBool(o, "detachable", out var dt) ? dt : Detachable ?? false;
                OnboardPart = Json.Str(o, "part") ?? OnboardPart;
            }
        }
    }
}

public sealed class ManifestSlot
{
    public string Id { get; set; } = "";
    public string Kind { get; set; } = "";
    public string[] Devices { get; set; } = Array.Empty<string>();   // what the machine models behind it (empty = nothing yet)
}

public sealed class ManifestBoard
{
    public string Id { get; set; } = "";
    public string Mcu { get; set; } = "";
    public List<ManifestSlot> Slots { get; set; } = new();
    public List<ManifestPad> Pads { get; set; } = new();
    /// Suggested wiring (optional; session-form entries: device, port, pad, props). Autofill uses it when present.
    public List<Dictionary<string, JsonElement>>? Defaults { get; set; }
    /// true = `pads` came from the manifest; false = synthesised from `slots` (older manifest / cache).
    [JsonIgnore] public bool PadsDeclared { get; set; }
    public ManifestPad? Pad(string idOrLabel) =>
        Pads.Find(p => p.Id == idOrLabel) ?? Pads.Find(p => p.Label != null && string.Equals(p.Label, idOrLabel, StringComparison.OrdinalIgnoreCase));
    /// The pad an onboard `device` sits on, or null.
    public ManifestPad? OnboardPadOf(string device) => Pads.Find(p => p.OnboardDevice == device);
}

/// An environment function (manifest `functions`, docs/protocol.md `function`): a calibration, the launch... run by the
/// core on `{"type":"function","name":...}`; `auto_first_start` = part of the first-start flow.
public sealed class ManifestFunction
{
    public string Name { get; set; } = "";
    public string Label { get; set; } = "";
    [JsonPropertyName("auto_first_start")] public bool AutoFirstStart { get; set; }
    public JsonElement? Params { get; set; }

    /// "speed_kmh 50, duration_s 0.5, pitch_deg 10" ("" = none).
    [JsonIgnore]
    public string ParamsText => Params is { ValueKind: JsonValueKind.Object } p ? string.Join(", ", p.EnumerateObject().Select(x => $"{x.Name} {x.Value.GetRawText().Trim('"')}")) : "";
    [JsonIgnore] public string DisplayLabel => string.IsNullOrWhiteSpace(Label) ? Name : Label;
}

public sealed class ManifestAircraft
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public string Class { get; set; } = "";
    public string Fdm { get; set; } = "";
    public string[] Actuators { get; set; } = Array.Empty<string>();
    /// The FDM inputs a servo may drive, in the machine's default servo order (servo1.. → these).
    [JsonPropertyName("fdm_inputs")] public string[] FdmInputs { get; set; } = Array.Empty<string>();
    /// The actuator the ESC's output carries ("" = not given).
    public string Throttle { get; set; } = "";
    public bool Implemented { get; set; } = true;
    /// The aircraft's environment functions (calibrations, launch).
    public List<ManifestFunction> Functions { get; set; } = new();
}

public sealed class Manifest
{
    public string Core { get; set; } = "";
    public string Version { get; set; } = "";
    public List<ManifestBoard> Boards { get; set; } = new();
    public List<ManifestDevice> Devices { get; set; } = new();
    public List<ManifestAircraft> Aircraft { get; set; } = new();

    /// Where this one came from: "live core", "FB_MANIFEST <path>", "cache user://manifest.json", "built-in fallback".
    [JsonIgnore] public string Source { get; private set; } = "";
    [JsonIgnore] public bool IsLive => Source == LiveSource;

    public const string CachePath = "user://manifest.json";
    /// The self-tests' manifest (a saved `core/qemu/run.sh --manifest`; SelfTests asserts fixed numbers for it).
    public const string FixturePath = "res://tests/manifest-fixture.json";
    private const string LiveSource = "live core";

    private static Manifest? _current;
    public static Manifest Current => _current ??= LoadOffline();
    /// Raised on the main thread when Current is replaced (the live manifest arrived).
    public static event Action? Changed;

    private static JsonSerializerOptions Opts => Json.Lenient;

    // ---- lookups ----
    public ManifestDevice? Device(string id) => Devices.Find(d => d.Id == id);

    /// Every fault of every device, in manifest order.
    public IEnumerable<(ManifestDevice Device, ManifestFault Fault)> AllFaults() =>
        Devices.SelectMany(d => d.Faults.Select(f => (d, f)));

    /// The board's slots from the manifest; a board it does not list falls back to the repo's unified config.
    public List<BoardSlot> SlotsFor(string board)
    {
        var b = Boards.Find(x => x.Id == board);
        if (b == null || b.Slots.Count == 0) return BoardInfo.Slots(BoardInfo.ReadConfig(board));
        return b.Slots.Select(s => new BoardSlot(s.Id, WiringCatalog.PortOf(s.Kind), s.Devices)).ToList();
    }

    /// The board's pads from the manifest (slots turned into pads for an older one); a board it does not
    /// list → pads from the repo's unified config (label-less, no onboard devices).
    public List<ManifestPad> PadsFor(string board)
    {
        var b = Boards.Find(x => x.Id == board);
        if (b != null && b.Pads.Count > 0) return b.Pads;
        return SlotsFor(board).Select(s => PadOfSlot(s.Name, WiringCatalog.KindName(s.Kind), s.Devices)).ToList();
    }

    public ManifestBoard? Board(string id) => Boards.Find(x => x.Id == id);

    internal static ManifestPad PadOfSlot(string id, string kind, string[]? devices)
    {
        var p = new ManifestPad { Id = id, Kind = kind, Devices = devices ?? Array.Empty<string>() };
        p.Normalize();
        return p;
    }

    /// The aircraft's actuator names from the manifest; one it does not list falls back to the model README.
    public string[] ActuatorsFor(string aircraft)
    {
        var a = Aircraft.Find(x => x.Id == aircraft);
        return a is { Actuators.Length: > 0 } ? a.Actuators : BoardInfo.Actuators(aircraft);
    }

    /// The servo actuators in the machine's default order (manifest `fdm_inputs`; else the RS7 order).
    public string[] ServoOrderFor(string aircraft) =>
        Aircraft.Find(x => x.Id == aircraft) is { FdmInputs.Length: > 0 } a ? a.FdmInputs : new[] { "swash_a", "swash_b", "swash_c", "tail" };

    /// The ESC's actuator (manifest `throttle`; else "motor" when the aircraft has it, else "").
    public string ThrottleFor(string aircraft) =>
        Aircraft.Find(x => x.Id == aircraft) is { Throttle.Length: > 0 } a ? a.Throttle : Array.IndexOf(ActuatorsFor(aircraft), "motor") >= 0 ? "motor" : "";

    /// The environment functions a bench offers: its aircraft's (`aircraft[].functions`, the only place the core lists
    /// them; a name once).
    public List<ManifestFunction> FunctionsFor(string aircraft)
    {
        var list = new List<ManifestFunction>();
        foreach (var f in Aircraft.Find(a => a.Id == aircraft)?.Functions ?? new()) if (!list.Any(x => x.Name == f.Name)) list.Add(f);
        return list;
    }

    public string Describe() =>
        $"{Source}{(Core != "" ? $" ({Core} {Version})" : "")}: {Devices.Count} devices ({Devices.Count(d => d.Implemented)} modelled, {Devices.Sum(d => d.PortsDeclared ? d.Ports.Count : 0)} ports), {AllFaults().Count()} faults, {Boards.Count} boards ({Boards.Sum(b => b.PadsDeclared ? b.Pads.Count : 0)} pads), {Aircraft.Count} aircraft";

    // ---- sources ----
    public static Manifest? Parse(string json, string source, out string error)
    {
        error = "";
        try
        {
            var m = JsonSerializer.Deserialize<Manifest>(json, Opts);
            if (m == null) { error = "empty"; return null; }
            m.Normalize();
            m.Source = source;
            return m;
        }
        catch (JsonException e) { error = e.Message; return null; }
    }

    /// The core's `manifest` message: becomes Current and is cached for offline use (wizard, settings).
    public static void SetLive(JsonElement root)
    {
        if (DevEnv.SelfTestRun) { GD.Print($"self-test run: the live manifest is ignored (pinned to {Current.Source}), the cache is left alone"); return; }
        var json = root.GetRawText();
        var m = Parse(json, LiveSource, out var err);
        if (m == null) { GD.PushError("bad manifest from core: " + err); return; }
        if (!Json.WriteAtomic(CachePath, json, out var werr)) GD.PushError("cannot cache the manifest: " + werr);
        _current = m;
        Changed?.Invoke();
    }

    private static Manifest LoadOffline()
    {
        var dev = DevEnv.Manifest;
        if (!string.IsNullOrEmpty(dev))
        {
            try
            {
                var m = File.Exists(dev) ? Parse(File.ReadAllText(dev), $"FB_MANIFEST {dev}", out var err) : null;
                if (m != null) return m;
                GD.PushError($"FB_MANIFEST={dev}: not readable, ignored");
            }
            catch (Exception e) { GD.PushError($"FB_MANIFEST={dev}: {e.Message}"); }
        }
        if (DevEnv.SelfTestRun)
        {
            using var fx = Godot.FileAccess.Open(FixturePath, Godot.FileAccess.ModeFlags.Read);
            var m = fx == null ? null : Parse(fx.GetAsText(), "fixture " + FixturePath, out _);
            if (m != null) return m;
            GD.PushError($"self-test run: {FixturePath} missing or bad, using the built-in catalogue (not the cache)");
            return Fallback();
        }
        using (var f = Godot.FileAccess.Open(CachePath, Godot.FileAccess.ModeFlags.Read))
        {
            if (f != null)
            {
                var m = Parse(f.GetAsText(), "cache " + CachePath, out var err);
                if (m != null) return m;
                GD.PushError("bad cached manifest, using the built-in catalogue: " + err);
            }
        }
        return Fallback();
    }

    /// The catalogue as the app hard-coded it before the manifest (devices + their inspector fields, no
    /// faults); boards from repo boards/*, aircraft from repo models/* (slots / actuators read from there).
    public static Manifest Fallback()
    {
        const string devices = """
        [{"id":"icm42688p","label":"ICM-42688-P gyro","category":"imu","kind":"spi"},
         {"id":"dps310","label":"DPS310 baro","category":"baro","kind":"i2c"},
         {"id":"crsf","label":"CRSF receiver","category":"receiver","kind":"uart"},
         {"id":"esc","label":"ESC","category":"esc","kind":"motor",
          "props":[{"id":"actuator","label":"aircraft actuator","type":"actuator"},
                   {"id":"telemetry","label":"RPM telemetry","type":"enum","options":["bidir","serial"],"default":"bidir"}]},
         {"id":"servo","label":"Servo","category":"servo","kind":"servo",
          "props":[{"id":"actuator","label":"aircraft actuator","type":"actuator"},{"id":"reversed","label":"linkage reversed","type":"bool","default":false}]},
         {"id":"gps","label":"GPS","category":"gps","kind":"uart"},
         {"id":"compass","label":"Compass","category":"compass","kind":"i2c"}]
        """;
        var m = new Manifest { Devices = JsonSerializer.Deserialize<List<ManifestDevice>>(devices, Opts) ?? new() };
        foreach (var board in Repo.Dirs("boards"))
            m.Boards.Add(new ManifestBoard
            {
                Id = board,
                Slots = BoardInfo.Slots(BoardInfo.ReadConfig(board)).Select(s => new ManifestSlot { Id = s.Name, Kind = WiringCatalog.KindName(s.Kind) }).ToList(),
            });
        foreach (var model in Repo.Dirs("models"))
            m.Aircraft.Add(new ManifestAircraft { Id = model, Name = model, Actuators = BoardInfo.Actuators(model) });
        m.Normalize();
        m.Source = "built-in fallback";
        return m;
    }

    /// JSON nulls → empty collections, so the rest of the app never checks.
    private void Normalize()
    {
        Boards ??= new(); Devices ??= new(); Aircraft ??= new();
        static List<ManifestFunction> Fns(List<ManifestFunction>? l) => (l ?? new()).Where(f => f != null && !string.IsNullOrWhiteSpace(f.Name)).Select(f => { f.Label ??= ""; return f; }).ToList();
        foreach (var a in Aircraft) a.Functions = Fns(a.Functions);
        Core ??= ""; Version ??= "";
        foreach (var b in Boards)
        {
            b.Slots ??= new(); foreach (var s in b.Slots) s.Devices ??= Array.Empty<string>();
            b.Pads = (b.Pads ?? new()).Where(p => p != null && !string.IsNullOrEmpty(p.Id)).ToList();
            b.PadsDeclared = b.Pads.Count > 0;
            if (!b.PadsDeclared) b.Pads = b.Slots.Select(s => PadOfSlot(s.Id, s.Kind, s.Devices)).ToList();
            foreach (var p in b.Pads) p.Normalize();
            b.Defaults = b.Defaults?.Where(d => d != null).ToList();
        }
        foreach (var d in Devices)
        {
            d.Props ??= new(); d.Faults ??= new(); d.Signals ??= Array.Empty<string>();
            d.Label = string.IsNullOrEmpty(d.Label) ? d.Id : d.Label;
            d.NormalizePorts();
            foreach (var p in d.Props) p.Options ??= Array.Empty<string>();
            foreach (var f in d.Faults) { f.Params ??= new(); f.Label = string.IsNullOrEmpty(f.Label) ? f.Id : f.Label; foreach (var p in f.Params) p.Options ??= Array.Empty<string>(); }
        }
        foreach (var a in Aircraft) { a.Actuators ??= Array.Empty<string>(); a.FdmInputs ??= Array.Empty<string>(); a.Throttle ??= ""; }
    }
}
