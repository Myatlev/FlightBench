// A bench (стенд): board + firmware file + aircraft + wiring + board config + core time + window layout + camera + checked signals.
// Saved as JSON under user://benches/<name>.json (through a temp file; FB_BENCH_DIR moves the folder for
// dev runs and self-tests). Editable presets (wizard: new / edit; flight screen: Save / Save as); the app
// remembers the recent ones. ToSession = the session file the core starts from (Firmware window → Save session).
// Wiring = one entry per device instance: its main wire (Port on Pad, pad by firmware id) + its other
// ports' wires (Links); onboard devices are entries on their own pad (EnsureOnboard adds them), the ones
// the user removed are in Detached. Older benches (`slot`) are migrated on load (MigrateSlots) the way the
// core reads an old-form session: the devices it assumes present are added, marked Assumed until saved.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using Godot;

namespace FlightBench;

/// A transmitter switch of the bench's model (`switches`): CRSF channel 1..16 and what the firmware does with it.
public sealed class BenchSwitch
{
    public int Channel { get; set; }            // 1-based, as the firmware and the radio name it (ch5 = AUX1)
    public string Name { get; set; } = "";      // ARM, ANGLE, NAV LAUNCH
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Note { get; set; }           // e.g. "high = armed (1700..2100)"
}

public sealed class WiringEntry
{
    public string Device { get; set; } = "";     // icm42688p, dps310, crsf, esc, servo, gps, compass
    public string Port { get; set; } = "";       // the main wire: device port (manifest `ports[].id`; "" = the device's first)…
    public string Pad { get; set; } = "";        // …on this board pad, by firmware id (gyro1, uart4, motor1, servo4..); "" = not wired
    public Dictionary<string, string> Links { get; set; } = new();   // the device's other ports: port id → pad id (ESC tlm → uart2, rpm → freq1)
    /// Old benches: the board slot (= the pad id). Read on load and moved to Pad (BenchData.MigrateSlots); never written.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Slot { get; set; }
    public string Actuator { get; set; } = "";   // for servo/esc: aircraft actuator name
    public bool Reversed { get; set; }           // servo linkage reversed on the airframe
    public string Option { get; set; } = "";     // e.g. esc telemetry "bidir" / "serial"
    // other manifest props (id → invariant text), e.g. gyro_noise_dps; a bench file may also give numbers / booleans (the
    // trainer preset's `accel_offset_mg: 20`, as the core's session form): read as their invariant text
    [JsonConverter(typeof(PropTextConverter))]
    public Dictionary<string, string> Props { get; set; } = new();
    /// Added by the old-form migration (the core assumes it present: ESC on motor1, servo1..4); the graph
    /// badges it "assumed". Kept through Clone (the wizard edits a copy), cleared by Save: never in a bench file.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingDefault)]
    public bool Assumed { get; set; }

    // manifest prop id ↔ field: `actuator` → Actuator, `reversed` → Reversed, `telemetry` → Option (older benches keep working), the rest → Props
    public string GetProp(string id) => id switch
    {
        "actuator" => Actuator,
        "reversed" => Reversed ? "true" : "false",
        "telemetry" => Option,
        _ => Props.TryGetValue(id, out var v) ? v : "",
    };

    /// In the wiring: some port of it is on a pad.
    [JsonIgnore] public bool IsWired => Pad != "" || Links.Count > 0;

    /// The pad of one of its ports ("" = not wired); `port` "" or the main port → Pad.
    public string PadOf(string port) => port == "" || port == Port ? Pad : Links.TryGetValue(port, out var p) ? p : "";

    /// Every wire: (port, pad), the main one first.
    public IEnumerable<(string Port, string Pad)> Wires()
    {
        if (Pad != "") yield return (Port, Pad);
        foreach (var (k, v) in Links) yield return (k, v);
    }

    /// Any prop set (actuator, reversed, telemetry, others)?
    [JsonIgnore] public bool HasProps => Actuator != "" || Reversed || Option != "" || Props.Count > 0;

    /// After deserialising: nulls in the file (`"props": null`) → empty.
    public void Normalize()
    {
        Device ??= ""; Port ??= ""; Pad ??= ""; if (Slot == "") Slot = null; Actuator ??= ""; Option ??= "";
        Props ??= new(); Links ??= new();
        foreach (var k in Props.Where(kv => kv.Value == null).Select(kv => kv.Key).ToList()) Props.Remove(k);
        foreach (var k in Links.Where(kv => string.IsNullOrEmpty(kv.Value) || kv.Key == "").Select(kv => kv.Key).ToList()) Links.Remove(k);
    }

    public void SetProp(string id, string value)
    {
        switch (id)
        {
            case "actuator": Actuator = value; break;
            case "reversed": Reversed = value == "true"; break;
            case "telemetry": Option = value; break;
            default: if (value == "") Props.Remove(id); else Props[id] = value; break;
        }
    }
}

/// Position of a node in the wiring graph (graph units). Keys: "board", "device@pad" ("#n" for duplicates; older benches' "device@slot" is the same key).
public sealed class NodePos
{
    public float X { get; set; }
    public float Y { get; set; }
}

public sealed class WindowLayout
{
    public bool Open { get; set; }
    public int X { get; set; }
    public int Y { get; set; }
    public int W { get; set; }
    public int H { get; set; }
}

/// The core's time settings (session `time` block, docs/protocol.md): taken when the core is started from the session.
public sealed class CoreTime
{
    public const int MinShift = 1, MaxShift = 3;
    public int IcountShift { get; set; } = 2;     // -icount shift=N: 1 = 500 MIPS, 2 = 250 (≈ a real F722), 3 = 125
    public bool IdleSkip { get; set; } = true;    // skip the firmware's idle loop (same flight, ~2x faster)
    public bool Realtime { get; set; } = true;    // throttle to wall clock (flying by hand); on by default (2026-09-29), an explicit value in a bench is kept

    public CoreTime Normalize() { IcountShift = Math.Clamp(IcountShift, MinShift, MaxShift); return this; }
}

public sealed class BenchData
{
    public string Name { get; set; } = "new bench";
    public string Board { get; set; } = "nexus-xr";
    /// One line for the Presets list (bundled presets; optional, left out of the file when unset).
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Description { get; set; }
    /// Firmware file: absolute, `~/...`, or relative to the data root (the core's `root=`; kept relative in the session).
    public string FirmwarePath { get; set; } = "";
    public bool WatchFirmware { get; set; }
    public string Aircraft { get; set; } = "rs7";
    /// The aircraft's visual scene (optional; left out of the file when unset): a path like FirmwarePath (relative =
    /// under the data root); unset = `models/<aircraft>/visual/<aircraft>.tscn` (app/scripts/Aircraft.cs).
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Visual { get; set; }
    /// The old switch layout (one entry per switch channel, e.g. ch5 ARM, ch6 ANGLE, ch7 NAV LAUNCH): informational, refreshed
    /// from the firmware's modes; read once by the `transmitter` migration (TxModel.Migrate). The Transmitter window's rows
    /// come from `transmitter` + the modes.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<BenchSwitch>? Switches { get; set; }
    /// One line: the order to use the switches in (the Transmitter window shows it under the switches).
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? SwitchOrder { get; set; }
    /// The transmitter model (TxModel.cs, decisions 2026-10-23): per channel 5..16 (key = 1-based channel) its type
    /// (`auto` = from the firmware's modes, `switch2` / `switch3` / `switch6` = a static switch of the radio), label and
    /// start position. Null / an absent channel = auto.
    [JsonPropertyName("transmitter")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public Dictionary<string, TxChannel>? Transmitter { get; set; }
    /// The firmware's modes as last read (informational; docs/protocol.md `modes`): the flight screen saves every
    /// `modes` message here and shows its switches before a core answers ("from bench snapshot"). Never sent anywhere.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public ModesSnapshot? Modes { get; set; }
    /// The firmware's applied config as last read (`diff all` from the `diff` message; informational, read-only).
    [JsonPropertyName("firmware_diff")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public FirmwareDiff? FirmwareDiff { get; set; }
    /// Environment functions (manifest `functions`) to run at a first start: name -> on. Missing = the catalog's
    /// `auto_first_start`. Informational: the protocol has no session field for it yet (the core decides from aircraft.json).
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public Dictionary<string, bool>? FirstStartFunctions { get; set; }
    public List<WiringEntry> Wiring { get; set; } = new();
    public List<string> Detached { get; set; } = new();   // onboard devices the user removed from the board (device ids)
    public Dictionary<string, NodePos> WiringLayout { get; set; } = new();   // wiring-graph node positions (empty = auto-layout)
    public string BoardConfig { get; set; } = "";         // diff all text
    /// The board config file (session `board_config`): "" = the board's own `boards/<board>/*.config`; else a path
    /// like FirmwarePath (relative = under the data root, kept relative in the session).
    public string BoardConfigFile { get; set; } = "";
    /// A CLI profile file applied after the board config (session `profile`, e.g. `models/rs7/qemu-profile.txt`);
    /// "" = none. A path like FirmwarePath. BoardConfig (the pasted diff) comes after it as `profile_text`.
    public string ProfileFile { get; set; } = "";
    /// Layer 4 of the firmware's config script, "bench lines" (session `bench_cli`): a CLI file next to the bench,
    /// `./<bench>.cli` (relative to the benches folder), typed after the board's, the aircraft's and the devices' lines
    /// (a later layer wins). Null = none yet (the wizard / Config window write it; the core's snapshot fills it).
    [JsonPropertyName("benchCli")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? BenchCli { get; set; }
    /// Lines of the assembled script the core skips (session `disabled_lines`): an exact line text or `layer:index`.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<string>? DisabledLines { get; set; }
    /// The aircraft's file when the bench uses its own copy (copy-on-write of a preset's aircraft,
    /// `./<bench>.files/models/<aircraft>/aircraft.json`); null = `models/<aircraft>/aircraft.json` under the data root.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? AircraftFile { get; set; }
    /// The bundled preset this bench was copied from (Presets.Open): its aircraft / board files are read-only bundles,
    /// an edit copies them next to the bench first (BenchFiles). Null = a bench of the user's own.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Preset { get; set; }
    public CoreTime Time { get; set; } = new();           // core time settings (older benches: the defaults)
    /// Which core this bench runs on: `local` = the app starts the bundled / configured core on the bench's
    /// ports (and connects to 127.0.0.1); `remote` = connect only, to CoreHost:CorePort. Older benches
    /// (no field, or null) are migrated on load: loopback / empty CoreHost → local, any other host → remote.
    /// Null only on a bench built in code and not yet normalised (counts as local; Save migrates it first).
    public string? Core { get; set; }
    public string CoreHost { get; set; } = "127.0.0.1";   // the remote core's host (kept while the bench is local)
    public int CorePort { get; set; } = 31250;
    public int MspPort { get; set; } = 31235;          // the core exposes the firmware MSP/CLI UART here (configurator: tcp://host:port, iNav host:port)
    public Dictionary<string, WindowLayout> Layout { get; set; } = new();
    public List<string> Signals { get; set; } = new();   // ids checked in the Signals window (saved with the layout, re-checked on open)
    public string Camera { get; set; } = "orbit";      // flight-screen camera: pad | orbit | fpv | chase
    public bool BodyAxes { get; set; }                 // body axes + actuator labels on the aircraft
    public bool OsdOverlay { get; set; } = true;       // the OSD drawn over the FPV camera (View → OSD overlay)
    public DateTime LastUsed { get; set; } = DateTime.MinValue;

    [JsonIgnore] public string FilePath => PathFor(Name);

    public const string CoreLocal = "local", CoreRemote = "remote";
    /// Connect only to CoreHost:CorePort (a null / unknown mode counts as local).
    [JsonIgnore] public bool IsRemote => Core == CoreRemote;
    /// The address the flight screen connects to: the remote host, or this machine for a local core.
    [JsonIgnore] public string ConnectHost => IsRemote ? CoreHost : "127.0.0.1";

    /// The mode an older bench without `core` gets: the address it connected to decides.
    public static string MigrateCore(string? host) => CoreLocator.IsLocal(host) ? CoreLocal : CoreRemote;

    /// The benches' folder: user://benches, or FB_BENCH_DIR (dev runs; self-tests point it at a temp folder).
    public static string Dir = (DevEnv.BenchDir ?? "user://benches").Replace('\\', '/').TrimEnd('/');

    public static string PathFor(string name) => $"{Dir}/{SafeName(name)}.json";

    /// The bench's file name (= its id: BenchState, its `./<id>.cli` / `./<id>.files`): letters, digits, `-`, `_`;
    /// every other run of characters becomes one `_` ("RS7 — first flight" → "RS7_first_flight"); never empty.
    public static string SafeName(string n)
    {
        var s = System.Text.RegularExpressions.Regex.Replace(n ?? "", @"[^\p{L}\p{N}\-_]+", "_").Trim('_');
        return s == "" ? "bench" : s;
    }

    /// The part after `./` of a bench-relative path when it stays inside the benches folder (no `..` segment, no
    /// rooted or drive path); null otherwise — a preset naming `./../x` must not read or write outside its folder.
    public static string? OwnRel(string? path)
    {
        if (!IsBenchRelative(path)) return null;
        var rel = SessionPath(path!)[2..];
        if (rel == "" || Path.IsPathRooted(rel) || (rel.Length >= 2 && rel[1] == ':')) return null;
        return rel.Split('/').Any(seg => seg == "..") ? null : rel;
    }

    /// A new bench with the settings' defaults (local core, core port).
    public static BenchData New() => new() { Core = CoreLocal, CorePort = AppSettings.Current.DefaultCorePort };

    /// Writes the bench (new form; the migration's assumed entries become ordinary ones).
    public void Save()
    {
        foreach (var w in Wiring) w.Assumed = false;
        Core ??= MigrateCore(CoreHost);
        if (!Json.WriteAtomic(FilePath, JsonSerializer.Serialize(this, Json.Files), out var err)) GD.PushError($"cannot write {FilePath}: {err}");
    }

    /// A deep copy (through JSON): the wizard edits one, so Cancel leaves the bench as it was.
    public BenchData Clone() => Parse(JsonSerializer.Serialize(this, Json.Files));

    /// Save as: a copy of everything (wiring, board config, firmware, layout, camera, core address) under `name`.
    public BenchData CopyAs(string name)
    {
        var c = Clone();
        c.Name = name;
        BenchFiles.Rehome(c, Name);                     // its bench lines / own file copies go with it (copied)
        return c;
    }

    public static BenchData? Load(string path)
    {
        if (Json.ReadText(path) is not { } text) return null;
        try { return Parse(text); }
        catch (Exception e) { GD.PushError($"bad bench {path}: {e.Message}"); return null; }
    }

    /// A bench from JSON text, nulls in the file replaced by the defaults. Throws on bad JSON / a wrong shape.
    public static BenchData Parse(string text) => (JsonSerializer.Deserialize<BenchData>(text, Json.Files) ?? new()).Normalize();

    /// After deserialising: null fields / collections / entries (`"layout": null`) → the defaults, so the
    /// screens can use them as-is.
    public BenchData Normalize()
    {
        var d = new BenchData();
        Name ??= d.Name; Board ??= d.Board; FirmwarePath ??= d.FirmwarePath; Aircraft ??= d.Aircraft;
        BoardConfig ??= d.BoardConfig; CoreHost ??= d.CoreHost; Camera ??= d.Camera;
        FirmwarePath = SessionPath(FirmwarePath); BoardConfigFile = SessionPath(BoardConfigFile); ProfileFile = SessionPath(ProfileFile);
        if (string.IsNullOrWhiteSpace(Description)) Description = null;
        if (string.IsNullOrWhiteSpace(Visual)) Visual = null; else Visual = SessionPath(Visual);
        if (string.IsNullOrWhiteSpace(SwitchOrder)) SwitchOrder = null;
        Switches = Switches?.Where(x => x != null && x.Channel is >= 1 and <= 16 && !string.IsNullOrWhiteSpace(x.Name)).ToList();
        if (Switches is { Count: 0 }) Switches = null;
        Modes = Modes?.Normalize();
        // no `transmitter` block (benches before 2026-10-23): stays null — shown and driven with the board's family default
        // (TxModel.Effective) without rewriting the file; with an old `switches` list: the default + old names as labels
        // where the firmware's modes have no range (TxModel.Migrate); an explicit block is kept as written (normalised)
        Transmitter = Transmitter == null ? TxModel.Migrate(Board, Switches, Modes) : TxModel.Normalize(Transmitter);
        if (FirmwareDiff != null) FirmwareDiff.Text ??= "";
        if (FirstStartFunctions is { Count: 0 }) FirstStartFunctions = null;
        if (string.IsNullOrWhiteSpace(BenchCli)) BenchCli = null; else BenchCli = SessionPath(BenchCli);
        if (string.IsNullOrWhiteSpace(AircraftFile)) AircraftFile = null; else AircraftFile = SessionPath(AircraftFile);
        if (string.IsNullOrWhiteSpace(Preset)) Preset = null;
        DisabledLines = DisabledLines?.Where(x => !string.IsNullOrWhiteSpace(x)).Select(x => x.Trim()).Distinct().ToList();
        if (DisabledLines is { Count: 0 }) DisabledLines = null;
        Core = Core?.Trim().ToLowerInvariant() switch { CoreLocal => CoreLocal, CoreRemote => CoreRemote, _ => MigrateCore(CoreHost) };
        Time = (Time ?? new()).Normalize();
        Wiring = (Wiring ?? new()).Where(w => w != null).ToList();
        foreach (var w in Wiring) w.Normalize();
        Detached = (Detached ?? new()).Where(x => !string.IsNullOrEmpty(x)).Distinct().ToList();
        MigrateSlots(Manifest.Current);
        WiringLayout ??= new();
        foreach (var k in WiringLayout.Where(kv => kv.Value == null).Select(kv => kv.Key).ToList()) WiringLayout.Remove(k);
        Layout ??= new();
        Signals = (Signals ?? new()).Where(x => !string.IsNullOrWhiteSpace(x)).Select(x => x.Trim()).Distinct().ToList();
        foreach (var k in Layout.Where(kv => kv.Value == null).Select(kv => kv.Key).ToList()) Layout.Remove(k);
        // the "Firmware config" window became "Config" (2026-10-02): its saved place goes with it
        if (Layout.Remove("Firmware config", out var oldCfg) && !Layout.ContainsKey("Config")) Layout["Config"] = oldCfg;
        return this;
    }

    /// Pads the old-form rule assumes (core/qemu/scripts/wiring.py OLD_RX_PAD / OLD_ESC_PAD / SERVO_PADS).
    public const string OldRxPad = "uart5", OldEscPad = "motor1";
    public static readonly string[] OldServoPads = { "servo1", "servo2", "servo3", "servo4" };

    /// An older bench (entries with `slot`, none with `pad`/`port`, no `detached`) = the core's old-form
    /// session, migrated to what the core runs for it: slot → Pad (a slot name is the pad's firmware id),
    /// Port → the device's first port; onboard devices stay, except a detachable one the list omits
    /// (the gyro: the old `imu=off` rule) — the onboard receiver counts as present unless the list puts a
    /// crsf on a UART that models one (then that one is the receiver) or puts another device on its pad;
    /// then the devices the core assumes are added, marked Assumed: the ESC on motor1 (the aircraft's
    /// throttle, telemetry default) unless an ESC is listed or motor1 is taken, and a servo on each free
    /// servo1..4 with the aircraft's default actuator (`fdm_inputs` in order, else swash_a/b/c, tail; left
    /// empty when the list already drives it); a board without an onboard receiver gets one assumed on
    /// uart5 unless the list names one. A list mixing entries with and without `slot` only gets
    /// slot → pad. Returns the number of entries migrated.
    public int MigrateSlots(Manifest m)
    {
        var old = Wiring.Where(w => !string.IsNullOrEmpty(w.Slot) && w.Pad == "" && w.Links.Count == 0).ToList();
        if (old.Count == 0) { foreach (var w in Wiring) w.Slot = null; return 0; }
        bool wholeOld = Wiring.All(w => w.Pad == "" && w.Port == "" && w.Links.Count == 0) && Detached.Count == 0;
        foreach (var w in old)
        {
            w.Pad = w.Slot!;
            w.Port = m.Device(w.Device)?.Primary.Id ?? "";
            w.Slot = null;
        }
        if (!wholeOld) return old.Count;
        var pads = m.PadsFor(Board);
        bool Used(string pad) => Wiring.Any(w => w.Pad == pad);
        // onboard devices the list does not name
        foreach (var p in pads.Where(p => p.HasOnboard && p.IsDetachable))
        {
            if (Wiring.Any(w => w.Device == p.OnboardDevice && w.Pad == p.Id)) continue;
            bool keep = false;
            if (m.Device(p.OnboardDevice)?.Category == "receiver")
            {
                bool rxElsewhere = Wiring.Any(w => w.Device == p.OnboardDevice && pads.Find(x => x.Id == w.Pad) is { } wp
                                               && wp.PortKind == PortKind.Uart && (wp.Devices.Length > 0 ? wp.Devices.Contains(w.Device) : !wp.Has("usb")));
                keep = !rxElsewhere && !Used(p.Id);
            }
            if (!keep && !Detached.Contains(p.OnboardDevice)) Detached.Add(p.OnboardDevice);
        }
        // a board without an onboard receiver: the core assumes one on uart5 unless the list places one
        string rxDev = BoardInfo.DeviceFor(m, "receiver", "uart", "crsf");
        if (!pads.Any(p => p.HasOnboard && m.Device(p.OnboardDevice)?.Category == "receiver") && !Wiring.Any(w => w.Device == rxDev)
            && pads.Any(p => p.Id == OldRxPad) && !Used(OldRxPad))
            Wiring.Add(new WiringEntry { Device = rxDev, Port = m.Device(rxDev)?.Primary.Id ?? "", Pad = OldRxPad, Assumed = true });
        // the ESC on motor1 and servo1..4, assumed present
        string esc = BoardInfo.DeviceFor(m, "esc", "motor", "esc"), servo = BoardInfo.DeviceFor(m, "servo", "servo", "servo");
        bool HasPad(string id) => pads.Any(p => p.Id == id);
        if (!Wiring.Any(w => w.Device == esc) && HasPad(OldEscPad) && !Used(OldEscPad))
        {
            var tel = m.Device(esc)?.Prop("telemetry") is { } tp ? (tp.DefaultText != "" ? tp.DefaultText : tp.Options.FirstOrDefault() ?? "") : "bidir";
            Wiring.Add(new WiringEntry { Device = esc, Port = m.Device(esc)?.Primary.Id ?? "", Pad = OldEscPad, Actuator = m.ThrottleFor(Aircraft), Option = tel, Assumed = true });
        }
        var order = m.ServoOrderFor(Aircraft);
        for (int i = 0; i < OldServoPads.Length; i++)
        {
            var sp = OldServoPads[i];
            if (!HasPad(sp) || Used(sp)) continue;
            var act = i < order.Length ? order[i] : "";
            if (act != "" && Wiring.Any(w => w.Actuator == act)) act = "";
            Wiring.Add(new WiringEntry { Device = servo, Port = m.Device(servo)?.Primary.Id ?? "", Pad = sp, Actuator = act, Assumed = true });
        }
        return old.Count;
    }

    /// Is this entry the board's onboard device on its own pad?
    public static bool IsOnboard(WiringEntry w, IEnumerable<ManifestPad> pads) =>
        w.Pad != "" && pads.Any(p => p.Id == w.Pad && p.OnboardDevice == w.Device);

    /// Adds an entry for every onboard device of the board that is neither in the wiring nor detached
    /// (it is on the board whether or not the list names it). Returns how many were added.
    public int EnsureOnboard(Manifest m)
    {
        int n = 0;
        foreach (var p in m.PadsFor(Board).Where(p => p.HasOnboard))
        {
            if (Detached.Contains(p.OnboardDevice) || Wiring.Any(w => w.Device == p.OnboardDevice && w.Pad == p.Id)) continue;
            Wiring.Add(new WiringEntry { Device = p.OnboardDevice, Port = m.Device(p.OnboardDevice)?.Primary.Id ?? "", Pad = p.Id });
            n++;
        }
        return n;
    }

    /// Devices present on this bench: wired ones + onboard ones not detached (the Faults window enables these).
    public List<string> PresentDevices(Manifest m)
    {
        var list = Wiring.Where(w => w.IsWired).Select(w => w.Device).ToList();
        list.AddRange(m.PadsFor(Board).Where(p => p.HasOnboard && !Detached.Contains(p.OnboardDevice)).Select(p => p.OnboardDevice));
        return list.Distinct().ToList();
    }

    /// The files that belong to the bench `name`, absolute: its bench file, its session file (CoreLauncher.SessionFileFor)
    /// and its bench-state files (BenchState) — the existing ones.
    public static List<string> FilesOf(string name)
    {
        var list = new List<string>();
        foreach (var p in new[] { PathFor(name), CoreLauncher.SessionFileFor(name) })
        {
            var abs = ProjectSettings.GlobalizePath(p).Replace('\\', '/');
            if (File.Exists(abs)) list.Add(abs);
        }
        list.AddRange(BenchState.Files(name));
        list.AddRange(BenchFiles.OwnFiles(name));
        return list;
    }

    /// Is `path` inside the bundled presets' folder (never deleted or written)?
    public static bool IsBundled(string path)
    {
        try
        {
            var full = Path.GetFullPath(ProjectSettings.GlobalizePath(path));
            var dir = Path.GetFullPath(Presets.Dir).TrimEnd('\\', '/') + Path.DirectorySeparatorChar;
            return full.StartsWith(dir, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);
        }
        catch (Exception) { return false; }
    }

    /// Deletes the bench `name`: its bench file, session file and bench-state files (FilesOf). A file inside the bundled
    /// presets is never touched. `deleted` lists what went, `error` what could not (null = all gone).
    public static bool Delete(string name, out List<string> deleted, out string? error)
    {
        deleted = new List<string>();
        var errors = new List<string>();
        foreach (var f in FilesOf(name))
        {
            if (IsBundled(f)) { errors.Add($"{f}: a bundled preset file, left alone"); continue; }
            try { File.Delete(f); deleted.Add(f); }
            catch (Exception e) { errors.Add($"{f}: {e.Message}"); }
        }
        BenchFiles.RemoveEmptyFolder(name);
        error = errors.Count > 0 ? string.Join("; ", errors) : null;
        return error == null;
    }

    public static List<BenchData> ListAll()
    {
        var list = new List<BenchData>();
        var dir = DirAccess.Open(Dir);
        if (dir == null) return list;
        foreach (var name in dir.GetFiles())
        {
            if (!name.EndsWith(".json")) continue;
            var b = Load($"{Dir}/{name}");
            if (b != null) list.Add(b);
        }
        return list.OrderByDescending(b => b.LastUsed).ToList();
    }

    /// The session description the core starts from (docs/protocol.md `--session`), repo paths resolved
    /// under `repoRoot` (the core machine's checkout when the core runs elsewhere). The bench's own paths
    /// (firmware, board config file, profile file) go in as written: a relative one stays relative and the
    /// core resolves it against its `root=` (the data root the app passes), never against a working directory.
    public Dictionary<string, object> ToSession(string repoRoot)
    {
        string P(params string[] parts) => Path.Combine(new[] { repoRoot }.Concat(parts).ToArray()).Replace('\\', '/');
        var d = new Dictionary<string, object> { ["board"] = Board, ["firmware"] = SessionPath(FirmwarePath) };
        if (BoardConfigFile != "") d["board_config"] = OwnPath(BoardConfigFile, repoRoot);
        else if (BoardInfo.ConfigPath(Board) is { } cfg) d["board_config"] = P("boards", Board, Path.GetFileName(cfg));
        if (ProfileFile != "") d["profile"] = OwnPath(ProfileFile, repoRoot);
        if (BoardConfig.Trim() != "") d["profile_text"] = BoardConfig;
        d["aircraft"] = AircraftFile != null ? ResolvePath(AircraftFile, repoRoot) : P("models", Aircraft, "aircraft.json");
        // layer 4 and the skipped lines (core branch `config-layers`); a bench file path is absolute (it lives with the
        // bench, not under the data root), and only when the file is there
        if (BenchCli != null && File.Exists(ResolvePath(BenchCli, repoRoot))) d["bench_cli"] = ResolvePath(BenchCli, repoRoot);
        if (DisabledLines is { Count: > 0 }) d["disabled_lines"] = DisabledLines.ToList();
        d["wiring"] = SessionWiring(Manifest.Current);
        d["time"] = new Dictionary<string, object> { ["icount_shift"] = Time.IcountShift, ["realtime"] = Time.Realtime, ["idle_skip"] = Time.IdleSkip };
        d["ports"] = new Dictionary<string, object> { ["control"] = CorePort, ["msp"] = MspPort };
        return d;
    }

    /// A bench path as the session carries it: trimmed, `/` separators; relative stays relative (the core: against `root=`).
    public static string SessionPath(string path) => (path ?? "").Trim().Replace('\\', '/');

    /// A path into the benches folder (`./x`: a file the bench owns, e.g. its copy of a preset's board script)?
    public static bool IsBenchRelative(string? path) => SessionPath(path ?? "").StartsWith("./", StringComparison.Ordinal);

    /// The session's form of a bench path: `./x` → absolute (the benches folder), the rest as written.
    private static string OwnPath(string path, string root) => IsBenchRelative(path) ? ResolvePath(path, root) : SessionPath(path);

    /// Is this a path the core resolves against the data root (not absolute, not `~`)?
    public static bool IsRootRelative(string path)
    {
        var p = (path ?? "").Trim();
        return p != "" && !p.StartsWith('~') && !Path.IsPathRooted(p) && !(p.Length >= 2 && p[1] == ':') && !IsBenchRelative(p);
    }

    /// Where the core finds a bench path, resolved as the core does (docs/protocol.md): relative → under `root`
    /// (the data root), `~` / `~/...` → the home directory (Windows: USERPROFILE, then HOME), absolute as is. "" → "".
    public static string ResolvePath(string path, string root)
    {
        var p = SessionPath(path);
        if (p == "") return "";
        if (p.StartsWith("./", StringComparison.Ordinal)) return BenchFiles.Abs(p[2..]);   // a file next to the bench
        if (p == "~" || p.StartsWith("~/", StringComparison.Ordinal))
        {
            var home = (OperatingSystem.IsWindows() ? System.Environment.GetEnvironmentVariable("USERPROFILE") : null) ?? System.Environment.GetEnvironmentVariable("HOME") ?? "";
            return home.Replace('\\', '/').TrimEnd('/') + p[1..];
        }
        if (!IsRootRelative(p)) return p;
        try { return Path.GetFullPath(Path.Combine(root, p)).Replace('\\', '/'); }
        catch (Exception) { return Path.Combine(root, p).Replace('\\', '/'); }
    }

    /// A picked file under `root` as a root-relative path (a bench / preset stays movable with its data), else as is (`/` separators).
    public static string RelativeTo(string path, string root)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(path) || string.IsNullOrWhiteSpace(root)) return path;
            var full = Path.GetFullPath(path);
            var r = Path.GetFullPath(root).TrimEnd('\\', '/') + Path.DirectorySeparatorChar;
            if (full.StartsWith(r, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
                return full[r.Length..].Replace('\\', '/');
        }
        catch (Exception) { }
        return string.IsNullOrWhiteSpace(path) ? path : SessionPath(path);
    }

    /// The session's `wiring` in the new form (docs/protocol.md): per wire `{device, port, pad}` (pad by
    /// firmware id; `port` left out when the manifest declares no ports), the props on the entry's first
    /// wire (`actuator`, `reversed`, `telemetry`, the rest typed by the manifest); an onboard device only
    /// when it carries props; `{device, detached:true}` for each onboard device removed (those the
    /// manifest lists as detachable on this board, or all when it does not know the board's pads).
    public List<Dictionary<string, object>> SessionWiring(Manifest m)
    {
        var pads = m.PadsFor(Board);
        var list = new List<Dictionary<string, object>>();
        foreach (var w in Wiring.Where(w => w.IsWired))
        {
            var dev = m.Device(w.Device);
            bool onboard = IsOnboard(w, pads);
            if (onboard && !w.HasProps && w.Links.Count == 0) continue;     // on the board anyway
            bool first = true;
            foreach (var (port, pad) in w.Wires())
            {
                var e = new Dictionary<string, object> { ["device"] = w.Device };
                var portId = port != "" ? port : dev?.Primary.Id ?? "";
                bool main = port == "" || port == w.Port;
                if (portId != "" && (dev is not { PortsDeclared: false } || !main)) e["port"] = portId;
                e["pad"] = pad;
                if (first) AddProps(e, w, dev);
                first = false;
                list.Add(e);
            }
        }
        var b = m.Board(Board);
        foreach (var id in Detached)
            if (b is not { PadsDeclared: true } || b.Pads.Any(p => p.OnboardDevice == id && p.IsDetachable))
                list.Add(new Dictionary<string, object> { ["device"] = id, ["detached"] = true });
        return list;
    }

    private static void AddProps(Dictionary<string, object> e, WiringEntry w, ManifestDevice? dev)
    {
        if (w.Actuator != "") e["actuator"] = w.Actuator;
        if (w.Reversed || dev?.Prop("reversed") != null) e["reversed"] = w.Reversed;
        if (w.Option != "") e["telemetry"] = w.Option;
        foreach (var (k, v) in w.Props) e[k] = dev?.Prop(k)?.Typed(v) ?? Untyped(v);
    }

    /// A prop the manifest does not type (an older / cached manifest): a number or true/false goes to the core as such, the rest as text.
    private static object Untyped(string v)
    {
        if (v is "true" or "false") return v == "true";
        if (double.TryParse(v, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var d) && !double.IsNaN(d) && !double.IsInfinity(d)) return d;
        return v;
    }
}

/// `props` values: strings as they are, numbers as their invariant text, true/false; null dropped (Normalize). Written as strings.
public sealed class PropTextConverter : JsonConverter<Dictionary<string, string>>
{
    public override Dictionary<string, string> Read(ref Utf8JsonReader r, Type type, JsonSerializerOptions o)
    {
        var d = new Dictionary<string, string>();
        if (r.TokenType == JsonTokenType.Null) return d;
        if (r.TokenType != JsonTokenType.StartObject) throw new JsonException("props: an object expected");
        while (r.Read() && r.TokenType != JsonTokenType.EndObject)
        {
            var k = r.GetString() ?? "";
            r.Read();
            switch (r.TokenType)
            {
                case JsonTokenType.String: d[k] = r.GetString() ?? ""; break;
                case JsonTokenType.Number: d[k] = r.GetDouble().ToString("R", System.Globalization.CultureInfo.InvariantCulture); break;
                case JsonTokenType.True: d[k] = "true"; break;
                case JsonTokenType.False: d[k] = "false"; break;
                case JsonTokenType.Null: break;
                default: throw new JsonException($"props.{k}: a string, number or boolean expected");
            }
        }
        return d;
    }

    public override void Write(Utf8JsonWriter w, Dictionary<string, string> v, JsonSerializerOptions o)
    {
        w.WriteStartObject();
        foreach (var (k, x) in v) w.WriteString(k, x);
        w.WriteEndObject();
    }
}

/// Cross-scene state: the bench being edited/flown.
public static class AppState
{
    public static BenchData? Bench;
    public static string? Editing;           // name of the bench the wizard edits (Wizard.Open; the wizard takes and clears it)
    public static string RepoRoot => AppSettings.Current.RepoRoot;
    /// Lines for the next flight screen's log (e.g. "a bench of that name exists: opened it"); the flight screen takes them.
    public static readonly List<string> Notes = new();
}
