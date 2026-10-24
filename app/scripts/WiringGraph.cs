// Wiring graph (GraphEdit) over BenchData.Wiring — the list stays the one data model, the graph is
// a view/editor of it. Device nodes on the left, one per device instance, with one output port per
// device port (ESC: signal / tlm / rpm; gyro: spi, its bundled INT shown with it); the board on the
// right as two nodes with one input per pad (printed label first, firmware id second; the manifest's
// pads, else its slots, else the board's unified config): "<board> · ports" = the external pads, below
// it "<board> · on board" = the pads that carry an onboard device (gyro, baro bus, ELRS UART, flash).
// A port's type is its PortKind, so GraphEdit itself only lets equal kinds connect; the handler also
// refuses a UART direction the pad cannot carry (a receiver on the RX-only TELEM), a user device on an
// on-board pad unless the pad is an I2C bus or its onboard device is detachable and detached, and moving
// an onboard device off its pad. Onboard devices (manifest `onboard`) sit pre-wired on their pad, grey
// with a badge; a detachable one can be disconnected (→ BenchData.Detached; its node stays where it is,
// unwired, badge "detached", so it can be put back), a soldered one cannot. Entries the old-form
// migration added (BenchData.MigrateSlots) carry an "assumed" badge until the bench is saved. Editable
// in the wizard (step 3), read-only in the flight screen's Wiring window.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.RegularExpressions;
using Godot;

namespace FlightBench;

/// Port types of the graph (GraphNode slot type = (int)PortKind). The five known kinds are fixed;
/// a pad/port kind the manifest introduces (capture, gpio …) gets the next number (WiringCatalog.PortOf).
public enum PortKind { Spi = 0, I2c = 1, Uart = 2, Motor = 3, Servo = 4 }

/// A board slot of the old model: name, kind, and what the core models behind it (manifest `devices`).
public sealed record BoardSlot(string Name, PortKind Kind, string[]? Devices = null);

/// Device catalogue = Manifest.Current (Manifest.cs); this holds the port-kind helpers.
public static class WiringCatalog
{
    public static IReadOnlyList<ManifestDevice> Devices => Manifest.Current.Devices;

    public static ManifestDevice? Find(string id) => Manifest.Current.Device(id);

    private static readonly List<string> KindNames = new() { "spi", "i2c", "uart", "motor", "servo" };

    /// Manifest kind string → port kind (unknown kinds are registered on first sight).
    public static PortKind PortOf(string kind)
    {
        var k = (kind ?? "").Trim().ToLowerInvariant();
        int i = KindNames.IndexOf(k);
        if (i < 0) { KindNames.Add(k); i = KindNames.Count - 1; }
        return (PortKind)i;
    }

    public static string KindName(PortKind k) => (int)k >= 0 && (int)k < KindNames.Count ? KindNames[(int)k] : "?";

    /// Several devices may share a pad only on a bus.
    public static bool IsBus(PortKind k) => k == PortKind.I2c;

    public static string Label(PortKind k) => k switch { PortKind.Spi => "SPI", PortKind.I2c => "I2C", PortKind.Uart => "UART", PortKind.Motor => "motor", PortKind.Servo => "servo", _ => KindName(k) };

    public static Color ColorOf(PortKind k) => k switch
    {
        PortKind.Spi => new Color(0.95f, 0.55f, 0.30f),
        PortKind.I2c => new Color(0.45f, 0.85f, 0.55f),
        PortKind.Uart => new Color(0.40f, 0.70f, 1.00f),
        PortKind.Motor => new Color(0.95f, 0.40f, 0.45f),
        PortKind.Servo => new Color(0.85f, 0.75f, 0.35f),
        _ when KindName(k) == "capture" => new Color(0.75f, 0.55f, 0.95f),
        _ => new Color(0.70f, 0.70f, 0.75f),
    };

    /// Slot name prefix ↔ kind, the board-config rule (BoardInfo.Slots names slots with it).
    public static readonly (string Prefix, PortKind Kind)[] SlotPrefixes =
        { ("gyro", PortKind.Spi), ("i2c", PortKind.I2c), ("uart", PortKind.Uart), ("motor", PortKind.Motor), ("servo", PortKind.Servo) };

    /// A pad's kind: as the manifest declares it on any board, else from its name prefix (servo when unknown).
    public static PortKind KindOfPad(string pad)
    {
        foreach (var b in Manifest.Current.Boards)
            foreach (var p in b.Pads)
                if (p.Id == pad) return PortOf(p.Kind);
        foreach (var (prefix, kind) in SlotPrefixes) if (pad.StartsWith(prefix, StringComparison.Ordinal)) return kind;
        return PortKind.Servo;
    }

    /// The kind of an entry's port: from its device's port in the manifest, else its pad's.
    public static PortKind KindOfPort(WiringEntry w, string port, string pad)
    {
        var d = Find(w.Device);
        var p = d == null ? null : port == "" ? d.Primary : d.PortById(port);
        return p?.PortKind ?? KindOfPad(pad);
    }

    /// Can a device port with this direction (seen from the device) be wired to this pad? null = yes,
    /// else why not. Only UART pads have a direction (in = the device listens: the pad needs TX; out = it
    /// talks: RX; both = TX and RX); `usb` carries both.
    public static string? DirectionProblem(ManifestPort port, ManifestPad pad)
    {
        if (pad.PortKind != PortKind.Uart || port.PortKind != PortKind.Uart) return null;
        var miss = new List<string>();
        if (port.Dir is "in" or "both" && !pad.HasTx) miss.Add("TX");
        if (port.Dir is "out" or "both" && !pad.HasRx) miss.Add("RX");
        return miss.Count == 0 ? null
            : $"{port.Id} ({port.Dir}) needs {(port.Dir == "both" ? "TX+RX" : port.Dir == "in" ? "TX" : "RX")} on the board side; pad {pad.MainText} has no {string.Join("/", miss)}{(pad.Has("rx_only") ? " (RX only)" : pad.Has("tx_only") ? " (TX only)" : "")}";
    }
}

/// What the wizard reads from the repo: board slots (unified config), aircraft actuator names, autofill.
public static class BoardInfo
{
    /// The board's unified config: boards/<board>/*.config (the first by name), or null.
    public static string? ConfigPath(string board)
    {
        try
        {
            var dir = Repo.Path("boards", board);
            return Directory.Exists(dir) ? Directory.GetFiles(dir, "*.config").OrderBy(f => f, StringComparer.Ordinal).FirstOrDefault() : null;
        }
        catch (Exception) { return null; }
    }

    public static string ReadConfig(string board)
    {
        try { return ConfigPath(board) is { } p ? File.ReadAllText(p) : ""; }
        catch (Exception) { return ""; }
    }

    private static IEnumerable<int> Numbers(string cfg, string pattern) =>
        Regex.Matches(cfg, pattern, RegexOptions.Multiline).Select(m => int.Parse(m.Groups[1].Value)).Distinct().OrderBy(n => n);

    /// Slots in board order: gyroN (GYRO_CS), i2cN (I2C_SCL), uartN (SERIAL_TX/RX), motorN, servoN
    /// (names from WiringCatalog.SlotPrefixes, same order).
    private static readonly string[] SlotResources = { "GYRO_CS", "I2C_SCL", "SERIAL_(?:TX|RX)", "MOTOR", "SERVO" };

    public static List<BoardSlot> Slots(string cfg)
    {
        var s = new List<BoardSlot>();
        for (int i = 0; i < SlotResources.Length; i++)
        {
            var (prefix, kind) = WiringCatalog.SlotPrefixes[i];
            s.AddRange(Numbers(cfg, "^resource " + SlotResources[i] + @" (\d+) ").Select(n => new BoardSlot($"{prefix}{n}", kind)));
        }
        return s;
    }

    /// Actuator names from the model README ("Actuators …: `swash_a`, …"); RS7 names as fallback.
    public static string[] Actuators(string aircraft)
    {
        try
        {
            var p = Repo.Path("models", aircraft, "README.md");
            if (File.Exists(p))
            {
                var text = File.ReadAllText(p);
                var i = text.IndexOf("Actuators", StringComparison.Ordinal);
                if (i >= 0)
                {
                    var end = text.IndexOf("\n- ", i, StringComparison.Ordinal);
                    var block = end > i ? text[i..end] : text[i..];
                    var names = Regex.Matches(block, @"`(\w+)`").Select(m => m.Groups[1].Value).Distinct().ToArray();
                    if (names.Length > 0) return names;
                }
            }
        }
        catch (Exception) { }
        return new[] { "swash_a", "swash_b", "swash_c", "tail", "motor" };
    }

    /// The manifest's device for a role: category + kind, modelled ones first; `fallback` when it lists none.
    public static string DeviceFor(Manifest m, string category, string kind, string fallback) =>
        m.Devices.Where(d => d.Category == category && d.Kind == kind).OrderBy(d => d.Implemented ? 0 : 1).Select(d => d.Id).FirstOrDefault() ?? fallback;

    private static WiringEntry On(Manifest m, string device, string pad) => new() { Device = device, Port = m.Device(device)?.Primary.Id ?? "", Pad = pad };

    /// Entries from the board config (the RS7 rule): servos (the aircraft's actuators in order), motors
    /// (ESC on the `motor` actuator when the aircraft has one, its telemetry default), gyro, baro,
    /// receiver; device ids from the manifest by category/kind; pads = the config's resource names.
    public static List<WiringEntry> Autofill(string cfg, string[] actuators, Manifest m)
    {
        var list = new List<WiringEntry>();
        string servo = DeviceFor(m, "servo", "servo", "servo"), esc = DeviceFor(m, "esc", "motor", "esc");
        string imu = DeviceFor(m, "imu", "spi", "icm42688p"), baro = DeviceFor(m, "baro", "i2c", "dps310"), rxDev = DeviceFor(m, "receiver", "uart", "crsf");
        var motorAct = Array.IndexOf(actuators, "motor") >= 0 ? "motor" : "";
        var servoActs = actuators.Where(a => a != "motor").ToArray();
        var tel = m.Device(esc)?.Prop("telemetry") is { } tp ? (tp.DefaultText != "" ? tp.DefaultText : tp.Options.FirstOrDefault() ?? "") : "bidir";
        int servoIdx = 0;
        foreach (Match x in Regex.Matches(cfg, @"^resource SERVO (\d+) \w+", RegexOptions.Multiline))
        {
            var e = On(m, servo, $"servo{x.Groups[1].Value}");
            e.Actuator = servoIdx < servoActs.Length ? servoActs[servoIdx++] : "";
            list.Add(e);
        }
        foreach (Match x in Regex.Matches(cfg, @"^resource MOTOR (\d+) \w+", RegexOptions.Multiline))
        {
            var e = On(m, esc, $"motor{x.Groups[1].Value}");
            e.Actuator = motorAct; e.Option = tel;
            list.Add(e);
        }
        if (Regex.IsMatch(cfg, @"^resource GYRO_CS 1", RegexOptions.Multiline)) list.Add(On(m, imu, "gyro1"));
        var b = Regex.Match(cfg, @"^set baro_i2c_device = (\d+)", RegexOptions.Multiline);
        if (b.Success) list.Add(On(m, baro, $"i2c{b.Groups[1].Value}"));
        var rx = Regex.Match(cfg, @"^serial (\d+) 64 ", RegexOptions.Multiline);
        if (rx.Success) list.Add(On(m, rxDev, $"uart{int.Parse(rx.Groups[1].Value) + 1}"));
        return list;
    }

    /// The manifest's suggested wiring for the board (`boards[].defaults`, session-form entries), or null.
    public static List<WiringEntry>? Defaults(Manifest m, string board)
    {
        if (m.Board(board)?.Defaults is not { Count: > 0 } defs) return null;
        var byDevice = new Dictionary<string, WiringEntry>();
        var list = new List<WiringEntry>();
        foreach (var d in defs)
        {
            string S(string k) => d.TryGetValue(k, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
            var dev = S("device");
            if (dev == "" || (d.TryGetValue("detached", out var det) && det.ValueKind == JsonValueKind.True)) continue;
            var padRef = S("pad") != "" ? S("pad") : S("slot");
            var pad = m.Board(board)?.Pad(padRef)?.Id ?? padRef;
            var port = S("port") != "" ? S("port") : m.Device(dev)?.Primary.Id ?? "";
            bool main = port == (m.Device(dev)?.Primary.Id ?? port);
            if (!main && byDevice.TryGetValue(dev, out var owner)) { owner.Links[port] = pad; continue; }   // a second port of the device before
            var e = new WiringEntry { Device = dev, Port = port, Pad = pad };
            foreach (var (k, v) in d)
            {
                if (k is "device" or "port" or "pad" or "slot" or "detached") continue;
                e.SetProp(k, v.ValueKind switch { JsonValueKind.String => v.GetString() ?? "", JsonValueKind.True => "true", JsonValueKind.False => "false", _ => v.GetRawText() });
            }
            list.Add(e);
            if (main) byDevice[dev] = e;
        }
        return list;
    }

    /// Autofill: the manifest's suggested defaults when it has them, else the board-config rule; then the
    /// board's onboard devices on their pads (an autofilled entry that is one of them becomes it; one that
    /// would take an onboard device's non-bus pad is dropped). Clears Detached.
    public static void AutofillBench(BenchData b, string[] actuators, Manifest m)
    {
        var pads = m.PadsFor(b.Board);
        var list = Defaults(m, b.Board) ?? Autofill(ReadConfig(b.Board), actuators, m);
        list = list.Where(w =>
        {
            var p = pads.Find(x => x.Id == w.Pad);
            return p == null || !p.HasOnboard || p.OnboardDevice == w.Device || WiringCatalog.IsBus(p.PortKind);
        }).ToList();
        b.Wiring = list;
        b.Detached = new();
        b.EnsureOnboard(m);
    }
}

public partial class WiringGraph : GraphEdit
{
    private const string BoardName = "board", OnboardName = "onboard";   // node names = their wiringLayout keys
    private const float DeviceX = 0, BoardX = 420, Gap = 12, ColumnGap = 70, BoardGap = 36;
    private const int RowFont = 15;                 // every row label of every node (board pads = device ports)
    private const int TwoColumnsAbove = 8;          // more auto-placed devices than this: two device columns
    private const float FitZoomMin = 0.6f, FitZoomMax = 1.5f;
    private static readonly Color OnboardGrey = new(0.50f, 0.53f, 0.58f);

    public const string DetachedWarning = "onboard device detached: the firmware will not find it";

    private BenchData _b = new();
    private List<ManifestPad> _pads = new();
    private bool _readOnly = true;
    private GraphNode? _board, _onboardNode;                                    // "· ports" / "· on board" (null: the board has no onboard pads)
    private List<ManifestPad> _portPads = new(), _onPads = new();               // the two board nodes' inputs (slot index = list index)
    private readonly Dictionary<string, WiringEntry> _entryOf = new();          // device node name → its entry
    private readonly Dictionary<string, List<ManifestPort>> _portsOf = new();   // device node name → its wired ports (= output port index)
    private readonly Dictionary<string, Label> _summaryOf = new();
    private readonly HashSet<string> _autoPlaced = new();                       // nodes placed by auto-layout (restacked once sized)
    private readonly HashSet<WiringEntry> _detachedNodes = new();               // entries of the detached onboard devices' nodes (unwired)
    private int _nextId, _fitFrames;
    private bool _fitView;                          // the pending layout pass also fits the view (open / Re-layout / Autofill)
    private float _fitZoom = -1;                    // what the last fit set: a resize refits only while the user has not zoomed/scrolled since
    private Vector2 _fitScroll;
    private bool _signalsBound;

    /// Selected device's entry (null = none / board).
    public event Action<WiringEntry?>? Selected;
    /// Wiring list changed (connect / disconnect / delete / detach).
    public event Action? Changed;
    /// A connect / disconnect / delete the rules refused, with the reason (the wizard shows it).
    public event Action<string>? Refused;

    public IReadOnlyList<ManifestPad> Pads => _pads;

    private static bool IsBoardNode(string n) => n == BoardName || n == OnboardName;

    /// The board node + input index showing this pad (node "" = no such pad).
    public (string Node, int Slot) PadSlot(string pad)
    {
        int i = _portPads.FindIndex(p => p.Id == pad);
        if (i >= 0) return (BoardName, i);
        i = _onPads.FindIndex(p => p.Id == pad);
        return i >= 0 ? (OnboardName, i) : ("", -1);
    }

    /// The pad behind a board node's input (null = none).
    private ManifestPad? PadAt(string node, int slot)
    {
        var list = node == BoardName ? _portPads : node == OnboardName ? _onPads : null;
        return list != null && slot >= 0 && slot < list.Count ? list[slot] : null;
    }

    private string[] _actuators = Array.Empty<string>();

    public void Setup(BenchData bench, List<ManifestPad> pads, string[] actuators, bool readOnly)
    {
        _b = bench; _pads = new List<ManifestPad>(pads); _actuators = actuators; _readOnly = readOnly;
        ShowArrangeButton = false; ShowGridButtons = false; ShowMinimapButton = false; MinimapEnabled = false; ShowZoomLabel = false;
        RightDisconnects = false;
        ConnectionLinesThickness = 3;
        SizeFlagsHorizontal = SizeFlags.ExpandFill; SizeFlagsVertical = SizeFlags.ExpandFill;
        if (_signalsBound) return;                  // Setup may run again (another board): bind the signals once
        _signalsBound = true;
        Resized += RefitUnlessMoved;
        VisibilityChanged += RefitUnlessMoved;
        if (readOnly) return;
        ConnectionRequest += (f, fp, t, tp) => TryConnect(f, (int)fp, t, (int)tp);
        DisconnectionRequest += (f, fp, t, tp) => TryDisconnect(f, (int)fp, t, (int)tp);
        DeleteNodesRequest += names => { foreach (var n in names) DeleteDevice(n.ToString()); };
        NodeSelected += n => Selected?.Invoke(n is GraphNode g && _entryOf.TryGetValue(g.Name, out var e) ? e : null);
        NodeDeselected += _ => Selected?.Invoke(null);
        EndNodeMove += SavePositions;
    }

    // ---- onboard ----
    /// The pad this entry's device sits on as an onboard part of the board (null = not an onboard device).
    /// A wired entry is onboard when it is the device on its own onboard pad; an unwired one only when it
    /// is a detached onboard device's node (a second receiver added from the palette is not onboard).
    public ManifestPad? OnboardPad(WiringEntry w) =>
        _pads.Find(p => p.HasOnboard && p.OnboardDevice == w.Device && (w.Pad != "" ? w.Pad == p.Id : !w.IsWired && _detachedNodes.Contains(w)));

    /// An onboard device the user removed (its node stays, unwired).
    public bool IsDetached(WiringEntry w) => !w.IsWired && _detachedNodes.Contains(w) && _b.Detached.Contains(w.Device);

    // ---- build ----
    /// Recreates every node and connection from BenchData.Wiring (+ onboard devices, detached ones, saved positions).
    public void Rebuild() => Rebuild(fit: true);

    /// `fit`: fit the view to the content afterwards (false = keep the user's zoom / scroll, e.g. after a detach).
    public void Rebuild(bool fit)
    {
        ClearConnections();
        foreach (var c in GetChildren()) if (c is GraphNode) { RemoveChild(c); c.QueueFree(); }
        _entryOf.Clear(); _portsOf.Clear(); _summaryOf.Clear(); _autoPlaced.Clear(); _detachedNodes.Clear();
        _b.EnsureOnboard(Manifest.Current);
        // pads the wiring uses but the board does not declare still get a port
        foreach (var w in _b.Wiring)
            foreach (var (port, pad) in w.Wires())
                if (_pads.All(p => p.Id != pad))
                    _pads.Add(Manifest.PadOfSlot(pad, WiringCatalog.KindName(WiringCatalog.KindOfPort(w, port, pad)), null));
        _portPads = _pads.Where(p => !p.HasOnboard).ToList();
        _onPads = _pads.Where(p => p.HasOnboard).ToList();
        _board = MakeBoardNode(BoardName, $"{_b.Board}  ·  ports", _portPads);
        AddChild(_board);
        _board.PositionOffset = SavedPos(BoardName) ?? AutoPos(_board, new Vector2(BoardX, 0));
        _onboardNode = null;
        if (_onPads.Count > 0)
        {
            _onboardNode = MakeBoardNode(OnboardName, $"{_b.Board}  ·  on board", _onPads);
            AddChild(_onboardNode);
            _onboardNode.PositionOffset = SavedPos(OnboardName) ?? AutoPos(_onboardNode, new Vector2(BoardX, 1e5f));
        }
        // detached onboard devices keep a node (unwired) in their pad's place in the order
        var entries = _b.Wiring.Where(w => w.IsWired).ToList();
        foreach (var dev in _b.Detached)
            if (_pads.Find(p => p.OnboardDevice == dev) is { } p && !_b.Wiring.Any(w => w.Device == dev && w.Pad == p.Id))
            {
                var w = new WiringEntry { Device = dev, Port = Manifest.Current.Device(dev)?.Primary.Id ?? "" };
                _detachedNodes.Add(w);
                entries.Add(w);
            }
        var keys = new Dictionary<string, int>();
        foreach (var w in entries.OrderBy(FirstPadIndex).ToList())
        {
            var node = AddDeviceNode(w);
            node.PositionOffset = SavedPos(KeyOf(w, keys)) ?? AutoPos(node, new Vector2(DeviceX, 0));
            foreach (var (port, pad) in w.Wires())
                if (PadSlot(pad) is { Node.Length: > 0 } t) ConnectNode(node.Name, PortIndex(node.Name, port), t.Node, t.Slot);
        }
        _fitFrames = 2;
        _fitView |= fit;
        Selected?.Invoke(null);
    }

    private GraphNode MakeBoardNode(string name, string title, List<ManifestPad> pads)
    {
        var g = new GraphNode { Name = name, Title = title, Draggable = !_readOnly, Selectable = !_readOnly };
        Style(g, name == OnboardName ? OnboardGrey.Lightened(0.15f) : new Color(0.55f, 0.65f, 0.80f));
        for (int i = 0; i < pads.Count; i++)
        {
            var p = pads[i];
            var row = new HBoxContainer { CustomMinimumSize = new Vector2(210, 0), TooltipText = PadTooltip(p), MouseFilter = Control.MouseFilterEnum.Pass };
            row.AddThemeConstantOverride("separation", 6);
            var main = RowLabel(new Label { Text = p.MainText, MouseFilter = Control.MouseFilterEnum.Pass });
            if (p.Label == null) main.Modulate = new Color(0.85f, 0.88f, 0.95f);
            var second = RowLabel(new Label { Text = "· " + p.SecondText, Modulate = p.Label == null ? new Color(1f, 0.85f, 0.5f, 0.75f) : Ui.Soft, MouseFilter = Control.MouseFilterEnum.Pass });
            var flags = p.Flags.Select(f => f.Replace('_', ' ')).ToList();
            var kind = RowLabel(new Label { Text = WiringCatalog.Label(p.PortKind) + (flags.Count > 0 ? " " + string.Join(" ", flags) : ""), Modulate = Ui.Soft, HorizontalAlignment = HorizontalAlignment.Right, SizeFlagsHorizontal = SizeFlags.ExpandFill, MouseFilter = Control.MouseFilterEnum.Pass });
            row.AddChild(main); row.AddChild(second); row.AddChild(kind);
            g.AddChild(row);
            g.SetSlot(i, true, (int)p.PortKind, WiringCatalog.ColorOf(p.PortKind), false, 0, Colors.White);
        }
        return g;
    }

    private static string PadTooltip(ManifestPad p)
    {
        var lines = new List<string> { p.Label != null ? $"{p.Label}  (firmware: {p.Id})" : $"{p.Id}  (no printed label confirmed{(p.Hint != null ? $"; probably \"{p.Hint}\"" : "")})" };
        lines.Add($"kind: {p.Kind}{(p.Flags.Length > 0 ? "   flags: " + string.Join(", ", p.Flags) : "")}{(p.Bus != null ? "   bus: " + p.Bus : "")}");
        if (p.Pins is { Count: > 0 }) lines.Add("pins: " + string.Join(", ", p.Pins.Select(kv => $"{kv.Key} {kv.Value}")));
        if (p.HasOnboard) lines.Add($"onboard: {p.OnboardDevice}{(p.OnboardPart != "" ? $" ({p.OnboardPart})" : "")}, {(p.IsDetachable ? "detachable" : "soldered")}");
        lines.Add(p.Devices.Length > 0 ? "modelled here: " + string.Join(", ", p.Devices) : "the core models nothing on this pad yet");
        if (p.Note != null) lines.Add(p.Note);
        if (WiringCatalog.IsBus(p.PortKind)) lines.Add("a bus: several devices may share it");
        return string.Join("\n", lines);
    }

    /// The device's ports as the node shows them (one output each; a bundled port rides on the primary).
    private static List<ManifestPort> NodePorts(WiringEntry w)
    {
        var d = WiringCatalog.Find(w.Device);
        if (d != null) return d.WiredPorts.ToList();
        var pad = w.Pad != "" ? w.Pad : w.Links.Values.FirstOrDefault() ?? "";
        var kind = WiringCatalog.KindName(WiringCatalog.KindOfPad(pad));
        return new List<ManifestPort> { new() { Id = w.Port, Kind = kind, Dir = "both", Label = kind } };
    }

    private GraphNode AddDeviceNode(WiringEntry w)
    {
        var kind = WiringCatalog.Find(w.Device);
        var ports = NodePorts(w);
        var onPad = OnboardPad(w);
        bool detached = onPad != null && !w.IsWired && _b.Detached.Contains(w.Device);
        bool modelled = kind?.Implemented ?? false;
        var tip = kind == null ? $"{w.Device}: not in the core's manifest" : modelled ? kind.Label : $"{kind.Label}: the core lists it but does not model it yet (wiring it does nothing)";
        if (onPad != null)
            tip += onPad.IsDetachable
                ? $"\nonboard on {onPad.MainText}{(onPad.OnboardPart != "" ? $" ({onPad.OnboardPart})" : "")}: detachable{(detached ? " — detached, drag its port back onto the pad to restore it" : " (disconnect or × removes it)")}"
                : $"\nsoldered to the board on {onPad.MainText}{(onPad.OnboardPart != "" ? $" ({onPad.OnboardPart})" : "")}: it cannot be removed";
        var g = new GraphNode { Name = $"dev{_nextId++}", Title = modelled ? w.Device : $"{w.Device} (not modelled yet)", Draggable = !_readOnly, Selectable = !_readOnly, TooltipText = tip };
        for (int j = 0; j < ports.Count; j++)
        {
            var p = ports[j];
            var bundled = kind?.Ports.Where(x => x.Bundled).Select(x => x.Id).ToList() ?? new List<string>();
            var text = p.Id + (j == 0 && bundled.Count > 0 ? $" (+{string.Join(", +", bundled)})" : "") + $"  {WiringCatalog.Label(p.PortKind)}" + (p.Dir != "both" ? $" {p.Dir}" : "") + (p.Optional ? " · optional" : "");
            g.AddChild(RowLabel(new Label { Text = text, HorizontalAlignment = HorizontalAlignment.Right, CustomMinimumSize = new Vector2(170, 0), TooltipText = p.Label != "" ? p.Label : p.Id, MouseFilter = Control.MouseFilterEnum.Pass }));
            g.SetSlot(j, false, 0, Colors.White, true, (int)p.PortKind, WiringCatalog.ColorOf(p.PortKind));
        }
        var summary = RowLabel(new Label { Text = Summary(w), HorizontalAlignment = HorizontalAlignment.Right, Modulate = Ui.Soft, CustomMinimumSize = new Vector2(170, 0) });
        g.AddChild(summary);
        var tint = onPad != null ? OnboardGrey : modelled ? WiringCatalog.ColorOf(ports[0].PortKind) : new Color(0.5f, 0.52f, 0.56f);
        Style(g, tint);
        if (onPad != null || !modelled) g.Modulate = new Color(0.78f, 0.78f, 0.82f, detached ? 0.6f : 0.92f);   // greyed: onboard / not modelled
        var bar = g.GetTitlebarHBox();
        if (onPad != null)
            bar.AddChild(Chip(detached ? "detached" : onPad.IsDetachable ? "onboard" : "soldered", tip, detached ? new Color(1f, 0.7f, 0.35f) : new Color(0.85f, 0.88f, 0.95f)));
        if (w.Assumed)
            bar.AddChild(Chip("assumed", AssumedTip, new Color(0.55f, 0.85f, 1f)));
        bool removable = onPad == null || (onPad.IsDetachable && !detached);
        if (!_readOnly && removable)
        {
            var close = new Button { Text = "×", TooltipText = onPad != null ? "detach the onboard device (the firmware will not find it)" : "remove device (Delete)", FocusMode = FocusModeEnum.None };
            foreach (var st in new[] { "normal", "hover", "pressed", "focus", "hover_pressed" }) close.AddThemeStyleboxOverride(st, new StyleBoxEmpty());
            close.AddThemeColorOverride("font_hover_color", new Color(1f, 0.5f, 0.5f));
            var name = g.Name.ToString();
            close.Pressed += () => DeleteDevice(name);
            bar.AddChild(close);
        }
        AddChild(g);
        _entryOf[g.Name] = w;
        _portsOf[g.Name] = ports;
        _summaryOf[g.Name] = summary;
        return g;
    }

    public const string AssumedTip = "not in the old bench's list: the core assumes it present for an old-form bench (receiver, ESC on motor1, servo1..4), so it is shown here; saving the bench keeps it as an ordinary entry";

    /// A small badge for a node's title bar.
    private static PanelContainer Chip(string text, string tip, Color color)
    {
        var badge = new Label { Text = text, TooltipText = tip, MouseFilter = Control.MouseFilterEnum.Pass };
        badge.AddThemeFontSizeOverride("font_size", 12);
        badge.AddThemeColorOverride("font_color", color);
        var chip = new PanelContainer { MouseFilter = Control.MouseFilterEnum.Pass, TooltipText = tip, SizeFlagsVertical = SizeFlags.ShrinkCenter };
        chip.AddThemeStyleboxOverride("panel", Ui.Box(new Color(0.18f, 0.20f, 0.24f), new Color(color, 0.8f), 6, 5, 0));
        chip.AddChild(badge);
        return chip;
    }

    private static Label RowLabel(Label l) { l.AddThemeFontSizeOverride("font_size", RowFont); return l; }

    /// Compact node look: title bar tinted by the port kind, tight margins (more nodes fit at a readable zoom).
    private static void Style(GraphNode g, Color tint)
    {
        StyleBoxFlat Box(Color bg, Color border, int bw, bool title)
        {
            var b = new StyleBoxFlat { BgColor = bg, BorderColor = border };
            b.SetBorderWidthAll(bw);
            if (title) { b.CornerRadiusTopLeft = b.CornerRadiusTopRight = 5; b.BorderWidthBottom = 0; }
            else { b.CornerRadiusBottomLeft = b.CornerRadiusBottomRight = 5; b.BorderWidthTop = 0; }
            b.ContentMarginLeft = b.ContentMarginRight = 10;
            b.ContentMarginTop = title ? 3 : 4; b.ContentMarginBottom = title ? 3 : 6;
            return b;
        }
        var border = new Color(tint, 0.55f);
        var sel = new Color(1, 1, 1, 0.9f);
        var bodyBg = new Color(0.05f, 0.10f, 0.20f, 0.96f);
        var titleBg = tint.Darkened(0.55f);
        g.AddThemeStyleboxOverride("titlebar", Box(titleBg, border, 1, true));
        g.AddThemeStyleboxOverride("titlebar_selected", Box(titleBg.Lightened(0.15f), sel, 2, true));
        g.AddThemeStyleboxOverride("panel", Box(bodyBg, border, 1, false));
        g.AddThemeStyleboxOverride("panel_selected", Box(bodyBg, sel, 2, false));
        g.AddThemeConstantOverride("separation", 2);
    }

    private static string Summary(WiringEntry w)
    {
        var parts = new List<string>();
        if (w.Actuator != "") parts.Add(w.Actuator);
        if (w.Reversed) parts.Add("reversed");
        if (w.Option != "") parts.Add(w.Option);
        foreach (var (k, v) in w.Props) if (v != "") parts.Add($"{k} {v}");
        return string.Join(" · ", parts);
    }

    /// Adds an unconnected device of that kind at the bottom of the device column (enters the wiring once
    /// connected). Returns its node name (null = not in the manifest).
    public string? AddDevice(string kindId)
    {
        var kind = WiringCatalog.Find(kindId);
        if (kind == null) return null;
        // mapped props start at their defaults (the ESC on the aircraft's "motor" actuator, as before); other props stay unset = the core's default
        var w = new WiringEntry { Device = kindId, Port = kind.Primary.Id };
        if (kind.Prop("actuator") != null && kind.Port == PortKind.Motor && Array.IndexOf(_actuators, "motor") >= 0) w.Actuator = "motor";
        if (kind.Prop("telemetry") is { } tel) w.Option = tel.DefaultText != "" ? tel.DefaultText : tel.Options.FirstOrDefault() ?? "";
        var node = AddDeviceNode(w);
        float bottom = _entryOf.Keys.Where(n => n != node.Name).Select(n => GetNode<GraphNode>(n.ToString())).Select(g => g.PositionOffset.Y + Math.Max(g.Size.Y, 60)).DefaultIfEmpty(0).Max();
        node.PositionOffset = new Vector2(DeviceX, bottom + Gap);
        SetSelected(node);
        _fitFrames = 2;
        return node.Name;
    }

    /// Removes a device node as Delete / × would (self-test).
    public void Remove(string node) => DeleteDevice(node);

    /// Re-labels a device node after its entry was edited in the inspector.
    public void Refresh(WiringEntry w)
    {
        if (NodeOf(w) is { } n && _summaryOf.TryGetValue(n, out var l)) l.Text = Summary(w);
    }

    private string? NodeOf(WiringEntry w) => _entryOf.FirstOrDefault(kv => ReferenceEquals(kv.Value, w)).Key;

    /// Selects the first node of that device (dev: FB_WIZARD_SELECT, to show the inspector).
    public void SelectDevice(string deviceId)
    {
        foreach (var (name, e) in _entryOf)
            if (e.Device == deviceId && GetNodeOrNull<GraphNode>(name.ToString()) is { } g) { SetSelected(g); Selected?.Invoke(e); return; }
    }

    /// Clears saved positions and stacks the nodes again.
    public void Relayout() { _b.WiringLayout.Clear(); Rebuild(); }

    // ---- editing ----
    private string? Refuse(string why) { Refused?.Invoke(why); return why; }

    /// Wires device node `from`'s output port `fromPort` to this pad (by firmware id) — as a drag would.
    public string? TryConnectPad(string from, int fromPort, string pad) =>
        PadSlot(pad) is { Node.Length: > 0 } t ? TryConnect(from, fromPort, t.Node, t.Slot) : Refuse($"no pad {pad} on the board");

    /// Takes device node `from`'s wire from `fromPort` to this pad off — as a disconnect would.
    public string? TryDisconnectPad(string from, int fromPort, string pad) =>
        PadSlot(pad) is { Node.Length: > 0 } t ? TryDisconnect(from, fromPort, t.Node, t.Slot) : "no such pad";

    /// Wires device node `from`'s output port `fromPort` to board node `to`'s input `toPort`. null = done,
    /// else why not (also raised as Refused). The rules: same kind; a UART direction the pad can carry; an
    /// onboard device only back onto its own pad (= un-detach); an on-board pad takes a user device only
    /// when it is an I2C bus or its onboard device is detachable and detached; otherwise a new wire
    /// replaces the pad's old one (whose device leaves the wiring when it has no wire left) and the port's
    /// own old wire.
    public string? TryConnect(StringName from, int fromPort, StringName to, int toPort)
    {
        if (!_entryOf.TryGetValue(from, out var e) || PadAt(to.ToString(), toPort) is not { } pad) return "not a device → board pad wire";
        var ports = _portsOf[from];
        if (fromPort < 0 || fromPort >= ports.Count) return "no such port";
        var port = ports[fromPort];
        if (port.PortKind != pad.PortKind) return Refuse($"{e.Device}.{port.Id} is a {port.Kind} port, pad {pad.MainText} is {pad.Kind}");
        if (WiringCatalog.DirectionProblem(port, pad) is { } dir) return Refuse($"{e.Device}.{dir}");
        var onPad = OnboardPad(e);
        if (onPad != null)
        {
            if (pad.Id != onPad.Id || fromPort != 0) return Refuse($"{e.Device} is onboard on {onPad.MainText}: it cannot be wired elsewhere");
            if (e.IsWired) return null;                                         // already there
        }
        bool bus = WiringCatalog.IsBus(pad.PortKind);
        if (!bus && onPad == null && pad.HasOnboard && pad.OnboardDevice != e.Device && (!pad.IsDetachable || !_b.Detached.Contains(pad.OnboardDevice)))
            return Refuse($"pad {pad.MainText} holds the onboard {pad.OnboardDevice}{(pad.IsDetachable ? ": detach it first" : " (soldered)")}");
        if (!bus)
            foreach (var (other, oPort) in WiresOnPad(pad.Id).ToList())
            {
                if (ReferenceEquals(other, e)) { if (oPort == PortIdOf(e, port)) return null; continue; }
                if (BenchData.IsOnboard(other, _pads)) return Refuse($"pad {pad.MainText} holds the onboard {other.Device}{(pad.IsDetachable ? ": detach it first" : " (soldered)")}");
            }
        // take the pad (non-bus: its previous device's wire goes) and the port (one wire per port)
        if (!bus) foreach (var (other, oPort) in WiresOnPad(pad.Id).ToList()) ClearWire(other, oPort);
        ClearWire(e, PortIdOf(e, port));
        SetWire(e, PortIdOf(e, port), pad.Id);
        // the device back on its onboard pad (its own node, or the same model added again) = not detached any more
        bool back = pad.OnboardDevice == e.Device && _b.Detached.Remove(e.Device);
        _detachedNodes.Remove(e);
        if (!_b.Wiring.Contains(e)) _b.Wiring.Add(e);
        ConnectNode(from, fromPort, to, toPort);
        if (onPad != null || back) RestyleLater();
        SavePositions();
        Changed?.Invoke();
        return null;
    }

    /// Takes a wire off. An onboard device: a detachable one is detached (Detached, node kept in place), a
    /// soldered one refuses. null = done, else why not.
    public string? TryDisconnect(StringName from, int fromPort, StringName to, int toPort)
    {
        if (!_entryOf.TryGetValue(from, out var e)) return "not a device";
        var ports = _portsOf[from];
        if (fromPort < 0 || fromPort >= ports.Count) return "no such port";
        var onPad = OnboardPad(e);
        if (onPad != null && e.IsWired)
        {
            if (!onPad.IsDetachable) return Refuse($"{e.Device} is soldered to the board on {onPad.MainText}: it cannot be removed");
            Detach(from, e);
            return null;
        }
        DisconnectNode(from, fromPort, to, toPort);
        ClearWire(e, PortIdOf(e, ports[fromPort]));
        if (!e.IsWired) _b.Wiring.Remove(e);
        SavePositions();
        Changed?.Invoke();
        return null;
    }

    /// Detach (onboard, detachable) / put back — the inspector's buttons.
    public string? SetDetached(WiringEntry e, bool detach)
    {
        if (NodeOf(e) is not { } name || OnboardPad(e) is not { } p) return "not an onboard device";
        return detach ? TryDisconnectPad(name, 0, p.Id) : TryConnectPad(name, 0, p.Id);
    }

    private void Detach(string node, WiringEntry e)
    {
        foreach (var c in Wires().Where(c => c.From == node)) DisconnectNode(c.From, c.FromPort, c.To, c.ToPort);
        e.Pad = ""; e.Links.Clear();
        _b.Wiring.Remove(e);
        _detachedNodes.Add(e);
        if (!_b.Detached.Contains(e.Device)) _b.Detached.Add(e.Device);
        RestyleLater();
        SavePositions();
        Changed?.Invoke();
        Selected?.Invoke(e);                                                   // the inspector shows the warning
    }

    /// Onboard nodes change their badge / × when (un)detached: rebuild after the signal returns.
    private void RestyleLater() => Callable.From(() => { var sel = _entryOf.Where(kv => GetNodeOrNull<GraphNode>(kv.Key) is { Selected: true }).Select(kv => kv.Value).FirstOrDefault(); Rebuild(fit: false); if (sel != null) ReSelect(sel); }).CallDeferred();

    private void ReSelect(WiringEntry old)
    {
        var match = _entryOf.FirstOrDefault(kv => kv.Value.Device == old.Device && (kv.Value.Pad == old.Pad || !kv.Value.IsWired)).Key;
        if (match != null && GetNodeOrNull<GraphNode>(match) is { } g) { SetSelected(g); Selected?.Invoke(_entryOf[match]); }
    }

    private static string PortIdOf(WiringEntry e, ManifestPort p) => p.Id;

    private static void SetWire(WiringEntry e, string port, string pad)
    {
        if (e.Port == "" ) e.Port = port;
        if (port == e.Port) e.Pad = pad; else e.Links[port] = pad;
    }

    private static void ClearWire(WiringEntry e, string port)
    {
        if (port == e.Port || port == "") e.Pad = ""; else e.Links.Remove(port);
    }

    /// The wires (entry, port) on a pad, from the model.
    private IEnumerable<(WiringEntry Entry, string Port)> WiresOnPad(string pad) =>
        _b.Wiring.SelectMany(w => w.Wires().Where(x => x.Pad == pad).Select(x => (w, x.Port)));

    private void DeleteDevice(string name)
    {
        if (IsBoardNode(name) || !_entryOf.TryGetValue(name, out var e)) return;
        if (OnboardPad(e) is { } p)
        {
            if (!p.IsDetachable) { Refuse($"{e.Device} is soldered to the board on {p.MainText}: it cannot be removed"); return; }
            if (e.IsWired) Detach(name, e);
            return;
        }
        foreach (var c in Wires().Where(c => c.From == name)) DisconnectNode(c.From, c.FromPort, c.To, c.ToPort);
        _b.Wiring.Remove(e);
        _entryOf.Remove(name); _portsOf.Remove(name); _summaryOf.Remove(name);
        if (GetNodeOrNull(name) is { } n) { RemoveChild(n); n.QueueFree(); }
        SavePositions();
        Selected?.Invoke(null);
        Changed?.Invoke();
    }

    // ---- connections / positions ----
    private readonly record struct Conn(string From, int FromPort, string To, int ToPort);

    private List<Conn> Wires() =>
        GetConnectionList().Select(d => new Conn(d["from_node"].AsString(), d["from_port"].AsInt32(), d["to_node"].AsString(), d["to_port"].AsInt32())).ToList();

    /// (device, port, pad) of every wire in the graph — for the self-tests.
    public List<(string Device, string Port, string Pad)> ConnectedWires() =>
        Wires().Where(c => _entryOf.ContainsKey(c.From) && PadAt(c.To, c.ToPort) != null && c.FromPort < _portsOf[c.From].Count)
               .Select(c => (_entryOf[c.From].Device, _portsOf[c.From][c.FromPort].Id, PadAt(c.To, c.ToPort)!.Id)).ToList();

    /// (device, board node, pad) of every wire — the self-test checks onboard pads land on the on-board node.
    public List<(string Device, string Node, string Pad)> WireTargets() =>
        Wires().Where(c => _entryOf.ContainsKey(c.From) && PadAt(c.To, c.ToPort) != null)
               .Select(c => (_entryOf[c.From].Device, c.To, PadAt(c.To, c.ToPort)!.Id)).ToList();

    /// Device nodes showing the "assumed" badge.
    public int AssumedNodeCount => _entryOf.Values.Count(w => w.Assumed);

    /// Board nodes (ports, on board) — the self-test checks the split.
    public int BoardNodeCount => _onboardNode != null ? 2 : 1;

    public int DeviceNodeCount => _entryOf.Count;

    /// The node name showing this device (first one), for scripted edits in the self-test.
    public string? NodeOfDevice(string device, bool wiredOnly = false) =>
        _entryOf.FirstOrDefault(kv => kv.Value.Device == device && (!wiredOnly || kv.Value.IsWired)).Key;

    public WiringEntry? EntryOfNode(string node) => _entryOf.TryGetValue(node, out var e) ? e : null;

    /// The board node's position (self-test: a saved layout is applied).
    public Vector2? BoardPosition => _board?.PositionOffset;

    /// The summary text of the node showing this entry (self-test: props appear on it).
    public string? SummaryOf(WiringEntry w) => NodeOf(w) is { } n && _summaryOf.TryGetValue(n, out var l) ? l.Text : null;

    /// A pad's place in the board's top-to-bottom order (ports node, then the on-board node); -1 = none.
    public int PadIndex(string pad)
    {
        var (node, slot) = PadSlot(pad);
        return node == BoardName ? slot : node == OnboardName ? _portPads.Count + slot : -1;
    }

    public int PortIndex(string node, string port)
    {
        if (!_portsOf.TryGetValue(node, out var ports)) return 0;
        int i = ports.FindIndex(p => p.Id == port);
        return i < 0 ? 0 : i;
    }

    /// Order of the device nodes: by their first pad's place (a detached onboard device by its own pad's).
    private int FirstPadIndex(WiringEntry w) =>
        !w.IsWired && OnboardPad(w) is { } op ? PadIndex(op.Id) : w.Wires().Select(x => PadIndex(x.Pad)).DefaultIfEmpty(int.MaxValue).Min();

    /// The layout key: "device@pad" (a detached onboard device keeps its pad's key, so detaching moves nothing).
    private string KeyOf(WiringEntry w, Dictionary<string, int> seen)
    {
        var pad = w.Pad != "" ? w.Pad : w.Links.Values.FirstOrDefault() ?? (OnboardPad(w)?.Id ?? "");
        var k = $"{w.Device}@{pad}";
        var n = seen.TryGetValue(k, out var c) ? c + 1 : 0;
        seen[k] = n;
        return n == 0 ? k : $"{k}#{n}";
    }

    private Vector2? SavedPos(string key) => _b.WiringLayout.TryGetValue(key, out var p) ? new Vector2(p.X, p.Y) : null;

    private Vector2 AutoPos(GraphNode g, Vector2 p) { _autoPlaced.Add(g.Name); return p; }

    /// Stores every node's position in the bench (devices by "device@pad", detached onboard ones by their
    /// pad too, the board nodes as "board" / "onboard").
    private void SavePositions()
    {
        if (_readOnly || _board == null) return;
        _b.WiringLayout.Clear();
        _b.WiringLayout[BoardName] = new NodePos { X = _board.PositionOffset.X, Y = _board.PositionOffset.Y };
        if (_onboardNode != null) _b.WiringLayout[OnboardName] = new NodePos { X = _onboardNode.PositionOffset.X, Y = _onboardNode.PositionOffset.Y };
        var keys = new Dictionary<string, int>();
        foreach (var (name, w) in _entryOf.OrderBy(kv => FirstPadIndex(kv.Value)))
        {
            if (!w.IsWired && !IsDetached(w)) continue;                       // a new, unwired device: not in the bench
            if (GetNodeOrNull<GraphNode>(name) is not { } g) continue;
            _b.WiringLayout[KeyOf(w, keys)] = new NodePos { X = g.PositionOffset.X, Y = g.PositionOffset.Y };
        }
    }

    /// Once nodes have their real sizes: stack auto-placed devices (two columns when there are more than
    /// TwoColumnsAbove, so the graph is wider than tall), the ports node top-right of them and the
    /// on-board node below it, fit the view (open / Re-layout / Autofill; a later resize refits only while
    /// the user has not zoomed or scrolled).
    public override void _Process(double delta)
    {
        if (_fitFrames <= 0 || !IsVisibleInTree() || Size.X < 50 || Size.Y < 50) return;
        if (--_fitFrames > 0) return;
        var nodes = GetChildren().OfType<GraphNode>().ToList();
        if (nodes.Any(n => n.Size.Y <= 0)) { _fitFrames = 1; return; }
        if (_autoPlaced.Count > 0)
        {
            var devs = nodes.Where(n => !IsBoardNode(n.Name) && _autoPlaced.Contains(n.Name)).ToList();   // added in pad order
            // two columns: the devices on the ports node far left, the onboard ones next to the board at the
            // on-board node's height (short wires, few crossings); else the first / second half in pad order
            bool OnBoardDev(GraphNode n) => _entryOf.TryGetValue(n.Name, out var e) && FirstPadIndex(e) is var i && i >= _portPads.Count && i != int.MaxValue;
            var outer = devs.Where(n => !OnBoardDev(n)).ToList();
            var inner = devs.Where(OnBoardDev).ToList();
            float onboardY = _onboardNode == null || _board == null ? 0
                : _autoPlaced.Contains(_onboardNode.Name) ? (_autoPlaced.Contains(_board.Name) ? 0 : _board.PositionOffset.Y) + _board.Size.Y + BoardGap : _onboardNode.PositionOffset.Y;
            var cols = new List<(List<GraphNode> Nodes, float Y0)>();
            if (devs.Count <= TwoColumnsAbove) cols.Add((devs, 0));
            else if (_onboardNode != null && outer.Count > 0 && inner.Count > 0 && outer.Count <= TwoColumnsAbove) { cols.Add((outer, 0)); cols.Add((inner, onboardY)); }
            else
            {
                int perCol = (devs.Count + 1) / 2;
                cols.Add((devs.Take(perCol).ToList(), 0)); cols.Add((devs.Skip(perCol).ToList(), 0));
            }
            float colX = DeviceX;
            foreach (var (col, y0) in cols)
            {
                float y = y0, w = col.Select(n => n.Size.X).DefaultIfEmpty(180).Max();
                foreach (var n in col)
                {
                    n.PositionOffset = new Vector2(colX + w - n.Size.X, y);   // right-aligned: the outputs line up
                    y += n.Size.Y + Gap;
                }
                colX += w + ColumnGap;
            }
            float right = nodes.Where(n => !IsBoardNode(n.Name)).Select(n => n.PositionOffset.X + n.Size.X).DefaultIfEmpty(DeviceX + 180).Max();
            if (_board != null && _autoPlaced.Contains(_board.Name)) _board.PositionOffset = new Vector2(right + 180, 0);
            if (_onboardNode != null && _autoPlaced.Contains(_onboardNode.Name) && _board != null)
                _onboardNode.PositionOffset = _board.PositionOffset + new Vector2(0, _board.Size.Y + BoardGap);
            _autoPlaced.Clear();
        }
        if (_fitView) FitView(nodes);
        _fitView = false;
    }

    private void RefitUnlessMoved()
    {
        if (UserMovedView()) return;
        _fitView = true;
        _fitFrames = 2;
    }

    /// The view is not where the last fit left it: the user zoomed or scrolled.
    private bool UserMovedView() => _fitZoom > 0 && (!Mathf.IsEqualApprox(Zoom, _fitZoom) || ScrollOffset.DistanceTo(_fitScroll) > 2);

    /// Zoom (0.6..1.5) and scroll so the bounding box of all nodes fills the area below the toolbar,
    /// centred; content larger than the area at the minimum zoom starts at its top-left.
    private void FitView(List<GraphNode> nodes)
    {
        if (nodes.Count == 0) return;
        var r = new Rect2(nodes[0].PositionOffset, nodes[0].Size);
        foreach (var n in nodes) r = r.Merge(new Rect2(n.PositionOffset, n.Size));
        const float pad = 24, top = 44;   // top: GraphEdit's own toolbar
        var avail = Size - new Vector2(2 * pad, 2 * pad + top);
        if (ZoomMin > FitZoomMin) ZoomMin = FitZoomMin;
        if (ZoomMax < FitZoomMax) ZoomMax = FitZoomMax;
        Zoom = Mathf.Clamp(Mathf.Min(avail.X / r.Size.X, avail.Y / r.Size.Y), FitZoomMin, FitZoomMax);
        var content = r.Size * Zoom;
        var off = new Vector2(Math.Max(pad, (Size.X - content.X) / 2), top + Math.Max(pad, (Size.Y - top - content.Y) / 2));
        ScrollOffset = r.Position * Zoom - off;
        _fitZoom = Zoom; _fitScroll = ScrollOffset;
    }
}
