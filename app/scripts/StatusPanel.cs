// The Status window (docs/ui.md "Flight screen windows", product decision 2026-10-01 "one fact in one place"): four
// lines and nothing to press —
//   link      connected · realtime 0.39× · 97 states/s  (not connected: where, and what the app's core is doing)
//   firmware  armed / disarmed · the active modes as text tags [ANGLE] [NAV LAUNCH] · flags · cpu
//             (while the firmware does not answer: why — booting, first start, rebooting; POWER OFF; in the CLI — MSP blocked,
//             with the one control Status has: "exit CLI (reboots)", shown only then)
//   sensors   the OSD (hello / state `osd`), the baro altitude (fc.baro_alt_m), GPS (when the state has one)
//   config    the first-start progress, only while it runs: applying → calibrating k/6 → applied (then hidden after a
//             few seconds; a replayed `restored` shows "config: restored (saved snapshot)" the same way; failed stays).
// Switches are the Transmitter's, the model's numbers the Aircraft window's, the core process the Bench menu's.
// FcStatus is the pure model (the self-tests feed it states); StatusPanel draws it.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Godot;

namespace FlightBench;

public sealed class FcStatus
{
    public bool? Msp, Armed, Power, Cli;
    public double? Cpu, Rtf, BaroAlt;
    public readonly List<string> Flags = new();
    /// The active flight modes (not ARM), the firmware's names: `fc.mode` split at '+', else `fc.boxes` named by the modes.
    public readonly List<string> Modes = new();
    public string? Osd;
    public string? Gps;

    /// `hello`: the OSD.
    public void ApplyHello(JsonElement h) { if (OsdLine.From(h) is { } o) Osd = o; }

    /// Copies what the lines need from a `state` (the JsonElement does not outlive the message).
    public void ApplyState(JsonElement s, ModesSnapshot? modes)
    {
        if (OsdLine.From(s) is { } o) Osd = o;
        if (Json.TryNum(s, "timing.realtime_factor", out var rf)) Rtf = rf;
        if (Json.TryNode(s, "gps", out var g) && g.ValueKind == JsonValueKind.Object)
        {
            var fix = Json.TryNum(g, "fix", out var fx) ? (fx >= 3 ? "3D fix" : fx >= 2 ? "2D fix" : "no fix") : null;
            var sats = Json.TryNum(g, "num_sat", out var ns) ? $"{ns:0} sat" : null;
            Gps = "GPS " + string.Join(" ", new[] { fix, sats }.Where(x => x != null));
        }
        if (!Json.TryNode(s, "fc", out var f) || f.ValueKind != JsonValueKind.Object) return;
        if (Json.TryBool(f, "msp", out var msp)) Msp = msp;
        if (Json.TryBool(f, "armed", out var a)) Armed = a;
        if (Json.TryBool(f, "power", out var p)) Power = p;
        if (Json.TryBool(f, "cli", out var c)) Cli = c;
        if (Json.TryNum(f, "cpu_pct", out var cpu)) Cpu = cpu;
        BaroAlt = Json.TryNum(f, "baro_alt_m", out var ba) ? ba : null;
        if (f.TryGetProperty("arming_flags", out var af) && af.ValueKind == JsonValueKind.Array)
        {
            Flags.Clear();
            Flags.AddRange(af.EnumerateArray().Select(x => x.ValueKind == JsonValueKind.String ? x.GetString() ?? "" : x.GetRawText()).Where(x => x != ""));
        }
        if (Json.Str(f, "mode") is { } mode)
        {
            Modes.Clear();
            Modes.AddRange(mode.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Where(x => x != "ARM"));
        }
        else if (modes?.ActiveIds(s) is { } ids)
        {
            Modes.Clear();
            Modes.AddRange(ids.Select(modes.BoxName).Where(x => x != "ARM"));
        }
    }

    /// A new connection: the previous core's values go (the OSD comes again with the hello).
    public void Clear()
    {
        Msp = Armed = Power = Cli = null; Cpu = Rtf = BaroAlt = null; Flags.Clear(); Modes.Clear(); Osd = null; Gps = null;
    }

    public static string LinkLine(CoreClient.ConnState st, string where, double? rtf, double statesPerSec, string coreNote)
    {
        using var inv = new InvariantText();
        return st switch
        {
            CoreClient.ConnState.Connected => $"connected · realtime {(rtf is { } r ? $"{r:0.00}×" : "–")} · {statesPerSec:0} states/s",
            CoreClient.ConnState.Connecting => $"connecting to {where}…{(coreNote != "" ? " · " + coreNote : "")}",
            _ => $"not connected ({where}){(coreNote != "" ? " · " + coreNote : "")}",
        };
    }

    /// The firmware line: (head, tags, tail); "" head and no tags = nothing to say. Text() joins it with the tags in brackets.
    public (string Head, List<string> Tags, string Tail) Firmware(bool connected, ConfigProgress cfg)
    {
        using var inv = new InvariantText();
        var none = new List<string>();
        if (!connected) return ("", none, "");
        if (Power == false) return ("firmware: POWER OFF (Battery)", none, "");
        // the firmware in its CLI (fc.cli / the `cli` event): said plainly, not guessed — unless it is the core's own first start
        if (Cli == true && !cfg.Active) return ("firmware: " + Flight.CliText, none, "");
        if (cfg.WaitLine(Msp) is { Length: > 0 } wait) return (wait, none, "");
        if (Msp == null) return ("firmware: no state yet", none, "");
        var tail = new List<string>();
        if (Flags.Count > 0) tail.Add("flags: " + string.Join(" ", Flags));
        if (Cpu is { } c) tail.Add($"cpu {c:0}%");
        return (Armed == true ? "ARMED" : "disarmed", Modes.ToList(), string.Join(" · ", tail));
    }

    public static string FirmwareText((string Head, List<string> Tags, string Tail) fw) =>
        string.Join(" · ", new[] { fw.Head, string.Join(" ", fw.Tags.Select(t => $"[{t}]")), fw.Tail }.Where(x => x != ""));

    public string SensorsLine()
    {
        using var inv = new InvariantText();
        var parts = new List<string>();
        if (Osd != null) parts.Add(Osd);
        if (BaroAlt is { } b) parts.Add($"baro {b:0.00} m");
        if (Gps != null) parts.Add(Gps);
        return string.Join(" · ", parts);
    }
}

/// Four lines: link, firmware (head + mode tags + tail), sensors, config progress (only while it runs).
public partial class StatusPanel : VBoxContainer
{
    public readonly Label Link = new() { Text = "not connected", AutowrapMode = TextServer.AutowrapMode.WordSmart };
    public readonly HFlowContainer FirmwareRow = new() { Visible = false, TooltipText = "the firmware as the core polls it over MSP (fc.*): armed, the active flight modes, arming flags, cpu" };
    public readonly Label Sensors = new() { Visible = false, Modulate = Ui.Soft, AutowrapMode = TextServer.AutowrapMode.WordSmart, TooltipText = "OSD (the wiring's), the firmware's baro altitude, GPS" };
    public readonly Label Config = new() { Visible = false, Modulate = new Color(1f, 0.85f, 0.5f), AutowrapMode = TextServer.AutowrapMode.WordSmart };
    private readonly Label _head = new(), _tail = new() { Modulate = Ui.Soft };
    /// The one control of the Status window, shown only while the firmware is in its CLI.
    public readonly Button ExitCli = new() { Text = "exit CLI (reboots)", Name = "exit_cli", Visible = false, FocusMode = Control.FocusModeEnum.None,
                                              TooltipText = "the firmware sits in its CLI: MSP (the configurator, the core's polls) is blocked; this sends exit — the firmware leaves the CLI and reboots" };
    private readonly HBoxContainer _tags = new();
    private string _fwText = "";

    private static readonly StyleBoxFlat TagStyle = new()
    {
        BgColor = new Color(0.25f, 0.45f, 0.75f, 0.9f), CornerRadiusTopLeft = 3, CornerRadiusTopRight = 3, CornerRadiusBottomLeft = 3, CornerRadiusBottomRight = 3,
        ContentMarginLeft = 5, ContentMarginRight = 5, ContentMarginTop = 0, ContentMarginBottom = 0,
    };

    public StatusPanel()
    {
        AddThemeConstantOverride("separation", 3);
        FirmwareRow.AddThemeConstantOverride("h_separation", 6);
        _tags.AddThemeConstantOverride("separation", 4);
        FirmwareRow.AddChild(_head); FirmwareRow.AddChild(_tags); FirmwareRow.AddChild(_tail); FirmwareRow.AddChild(ExitCli);
        AddChild(Link); AddChild(FirmwareRow); AddChild(Sensors); AddChild(Config);
    }

    public void SetFirmware((string Head, List<string> Tags, string Tail) fw)
    {
        var text = FcStatus.FirmwareText(fw);
        FirmwareRow.Visible = text != "";
        if (text == _fwText) return;
        _fwText = text;
        _head.Text = fw.Head;
        _head.Modulate = fw.Head == "ARMED" ? new Color(1f, 0.45f, 0.35f) : fw.Head.StartsWith("firmware:") ? new Color(1f, 0.85f, 0.5f) : Colors.White;
        foreach (var c in _tags.GetChildren()) { _tags.RemoveChild(c); c.QueueFree(); }
        foreach (var t in fw.Tags)
        {
            var l = new Label { Text = t };
            l.AddThemeStyleboxOverride("normal", TagStyle);
            _tags.AddChild(l);
        }
        _tags.Visible = fw.Tags.Count > 0;
        _tail.Text = fw.Tail;
        _tail.Visible = fw.Tail != "";
    }

    /// In the CLI: the exit button next to the firmware line.
    public void SetCli(bool inCli) { if (ExitCli.Visible != inCli) ExitCli.Visible = inCli; }

    public void SetLine(Label l, string text)
    {
        if (l.Text != text) l.Text = text;
        l.Visible = text != "";
    }

    // self-tests
    public string FirmwareText => _fwText;
    public int TagCount => _tags.GetChildCount();
}
