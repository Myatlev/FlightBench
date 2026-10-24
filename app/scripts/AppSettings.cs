// Global settings (not per bench): interface, paths, transmitter mapping, keys. Stored in user://settings.json
// (written through a temp file; a file that does not load — bad JSON, wrong shape, or unreadable — is
// copied aside and never overwritten unless that copy exists). Also Repo: the repo's boards/ and models/ folders.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using Godot;

namespace FlightBench;

/// Transmitter → channels, like a configurator's Receiver tab. Per channel (index = CRSF channel − 1):
///   Source:  "none" | "axis <n>" | "button <n>" | "buttons <n>,<m>" | "ui"   (see RcInput; the keyboard is a layer over
///            channels 1..4, not a source — an older file's "keyboard" reads as "none", one log line)
///   MinUs / CenterUs / MaxUs: input calibration — the µs the source actually produces at its low /
///     centre / high end (raw µs = 1500 + 500 · value, value −1..1); mapped onto 1000 / 1500 / 2000
///     in two linear segments, so an off-centre stick is honoured. Defaults 1000/1500/2000 = identity.
///   Reverse: mirrors the calibrated value around the centre.
public sealed class TransmitterSettings
{
    public int Device { get; set; } = 0;
    public string DeviceName { get; set; } = "";      // for re-matching after a replug at another index
    public string[]? Source { get; set; }             // null in pre-2026-09-25 files → migrated from AxisForChannel
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public int[]? AxisForChannel { get; set; }        // legacy (read only, migrated by Normalize)
    public bool[] Reverse { get; set; } = new bool[16];
    public int[] MinUs { get; set; } = Filled(1000);
    public int[]? CenterUs { get; set; }
    public int[] MaxUs { get; set; } = Filled(2000);
    public int Mode { get; set; } = 2;           // transmitter mode; only affects the stick drawing
    public float Deadzone { get; set; } = 0.02f;
    // (ArmChannel / ModeChannel / RescueChannel of older files are ignored since 2026-10-23: what a channel means is the
    // bench's transmitter model, TxModel.cs; `ui` = the Transmitter window's switch on that channel)
    /// The named radio unplugged during a flight = the receiver lost the radio: the flight screen sends
    /// `fault crsf link_lost` (channels hold, the keys not read) and clears it once the radio is back.
    public bool FailLinkOnDisconnect { get; set; } = true;
    private static int[] Filled(int v) { var a = new int[16]; Array.Fill(a, v); return a; }

    /// Gamepad-style default: axes 0,1,3,2 → roll, pitch, collective, yaw; pitch reversed because a
    /// gamepad reports stick-up as −1 (EdgeTX sends CH2 forward as +1 — its preset does not reverse).
    public static readonly int[] DefaultAxes = { 0, 1, 3, 2 };

    /// Fills missing fields after loading (old files) and pads arrays to 16.
    public void Normalize()
    {
        DeviceName ??= "";
        Reverse = Pad(Reverse, false); MinUs = Pad(MinUs, 1000); MaxUs = Pad(MaxUs, 2000);
        if (Source == null)
        {
            var legacy = AxisForChannel;
            Source = new string[16];
            for (int i = 0; i < 16; i++)
            {
                int a = legacy != null ? (i < legacy.Length ? legacy[i] : -1) : (i < 4 ? DefaultAxes[i] : -1);
                Source[i] = a >= 0 ? $"axis {a}" : "none";
            }
            for (int c = 4; c <= 6; c++) Source[c] = "ui";      // ch5..ch7: the Transmitter window's switches
            if (legacy != null)
            {
                // the old code inverted channel 2 in code and used MinUs/MaxUs as output endpoints:
                // keep the old pitch direction, start the (now input) calibration from identity
                Reverse[1] = !Reverse[1];
                MinUs = Filled(1000); MaxUs = Filled(2000);
            }
            else Reverse[1] = true;
            AxisForChannel = null;
        }
        Source = Pad(Source, "none");
        CenterUs = Pad(CenterUs ?? new int[0], 0);
        var keyboard = new List<int>();
        for (int i = 0; i < 16; i++)
        {
            if (string.IsNullOrWhiteSpace(Source[i])) Source[i] = "none";
            // the `keyboard` source is gone (decisions 2026-10-23: the keyboard is a layer over channels 1..4, always on)
            if (Source[i].Trim().Equals("keyboard", StringComparison.OrdinalIgnoreCase)) { Source[i] = "none"; keyboard.Add(i + 1); }
            if (MaxUs[i] - MinUs[i] < 2) { MinUs[i] = RcMath.MinUs; MaxUs[i] = RcMath.MaxUs; }   // no room for a centre: identity
            if (CenterUs[i] <= MinUs[i] || CenterUs[i] >= MaxUs[i]) CenterUs[i] = (MinUs[i] + MaxUs[i]) / 2;
        }
        MigratedKeyboard = keyboard.ToArray();
        if (keyboard.Count > 0)
            GD.Print($"[settings] channel source `keyboard` on ch{string.Join(", ch", keyboard)} reads as `none`: the keyboard is a layer over channels 1..4 now (Settings → Keys → Sticks), not a source");
    }

    /// The channels (1-based) whose `keyboard` source the last Normalize turned into `none` (self-tests).
    [JsonIgnore] public int[] MigratedKeyboard { get; private set; } = Array.Empty<int>();

    /// Preset for EdgeTX in USB Joystick (classic) mode: the model's CH1..CH8 are axes 0..7.
    public void ApplyEdgeTxClassic()
    {
        Source ??= new string[16];
        for (int i = 0; i < 16; i++)
        {
            Source[i] = i < 8 ? $"axis {i}" : "none";
            Reverse[i] = false; MinUs[i] = 1000; MaxUs[i] = 2000;
        }
        CenterUs = Filled(1500);
    }

    /// Pads / cuts to 16; null (a `"reverse": null` in the file) counts as empty.
    private static T[] Pad<T>(T[]? a, T fill)
    {
        a ??= Array.Empty<T>();
        if (a.Length == 16) return a;
        var r = new T[16]; Array.Fill(r, fill); Array.Copy(a, r, Math.Min(16, a.Length)); return r;
    }
}

public sealed class AppSettings
{
    public string UiScale { get; set; } = "auto";   // "auto" (screen DPI / 96) or a factor: "1", "1.25" … "2"
    public string RepoRoot { get; set; } = "";
    public string FirmwareDir { get; set; } = "";
    public int DefaultCorePort { get; set; } = 31250;
    /// The core executable (qemu-system-arm); "" = auto (CoreLocator: bundled data/core, then this, then FB_CORE_EXE).
    public string CorePath { get; set; } = "";
    /// Leaving the flight screen leaves the core the app started running (reused when the same bench opens
    /// again); off = it is stopped. App exit always stops it.
    public bool KeepCoreRunning { get; set; }
    public TransmitterSettings Transmitter { get; set; } = new();
    /// Binds by function id (Keybinds.cs); a missing id = the function's default there, "" = unbound.
    public Dictionary<string, string> Keys { get; set; } = new();

    public const string FilePath = "user://settings.json";
    /// Set when settings.json exists but did not load (unreadable, or did not parse and could not be copied
    /// aside): Save() then refuses to overwrite it.
    private static bool _keepBadFile;

    public static AppSettings Current { get; private set; } = LoadOrDefault();

    private static AppSettings LoadOrDefault()
    {
        var s = LoadFile(FilePath, out _keepBadFile, out _);
        if (string.IsNullOrEmpty(s.RepoRoot)) s.RepoRoot = DefaultRepoRoot();
        return s;
    }

    /// Editor / project run: the app lives in <repo>/app, so the parent when it has boards/. Exported build
    /// (res:// is inside the .pck and does not globalize): <exe dir>/data (or Contents/Resources/data in FlightBench.app)
    /// when it has both boards/ and models/ (copied there by app/tools/export.ps1 / export.sh), else the nearest folder above the exe that has both —
    /// <repo>/dist/FlightBench/FlightBench.exe → <repo>; none → the exe's folder (the user sets Settings → Paths).
    public static string DefaultRepoRoot()
    {
        if (OS.HasFeature("editor"))
        {
            var projDir = ProjectSettings.GlobalizePath("res://");
            var parent = Path.GetFullPath(Path.Combine(projDir, ".."));
            return Directory.Exists(Path.Combine(parent, "boards")) ? parent : projDir;
        }
        var exeDir = Path.GetDirectoryName(OS.GetExecutablePath()) ?? "";
        static bool HasData(string d) => Directory.Exists(Path.Combine(d, "boards")) && Directory.Exists(Path.Combine(d, "models"));
        if (CoreLocator.BundledDataDir() is { } bundled) return bundled;   // <exe dir>/data, or Contents/Resources/data on macOS
        for (var d = exeDir; !string.IsNullOrEmpty(d); d = Path.GetDirectoryName(d))
            if (HasData(d)) return d;
        return exeDir;
    }

    /// Settings from a file (RepoRoot not resolved); never throws. Missing → defaults. Unreadable → defaults,
    /// `keepFile` (the file must not be overwritten). Does not load (any exception) → copied aside to `backup`,
    /// defaults; `keepFile` when the copy failed.
    public static AppSettings LoadFile(string path, out bool keepFile, out string? backup)
    {
        AppSettings s = new();
        keepFile = false; backup = null;
        switch (Json.ReadText(path, out var text, out var readErr))
        {
            case Json.Read.Missing: break;
            case Json.Read.Unreadable:
                keepFile = true;
                GD.PushError($"cannot read {path} ({readErr}): defaults used, the file is left alone");
                break;
            default:
                try { s = Parse(text!); }
                catch (Exception e)
                {
                    s = new();
                    backup = Json.BackUpBad(path);
                    keepFile = backup == null;
                    GD.PushError($"bad {path} ({e.GetType().Name}: {e.Message}): {(backup != null ? "copied to " + backup + ", defaults used" : "could not copy it aside: defaults used, the file is left alone")}");
                }
                break;
        }
        s.Transmitter.Normalize();
        return s;
    }

    /// Settings from JSON text (nulls → defaults, keys and transmitter normalised; RepoRoot not resolved).
    /// Throws on bad JSON / a wrong shape.
    public static AppSettings Parse(string text)
    {
        var s = JsonSerializer.Deserialize<AppSettings>(text, Json.Files) ?? new();
        s.UiScale ??= "auto";
        s.RepoRoot ??= "";
        s.FirmwareDir ??= "";
        s.CorePath ??= "";
        s.Keys ??= new();
        foreach (var k in s.Keys.Where(kv => kv.Value == null).Select(kv => kv.Key).ToList()) s.Keys.Remove(k);   // null = the default bind
        s.Transmitter ??= new();
        s.Transmitter.Normalize();
        return s;
    }

    public void Save()
    {
        if (_keepBadFile) { GD.PushWarning("settings not saved: settings.json did not parse and could not be copied aside"); return; }
        if (!Json.WriteAtomic(FilePath, JsonSerializer.Serialize(this, Json.Files), out var err)) GD.PushError("cannot write settings.json: " + err);
    }

    public static void Reload() => Current = LoadOrDefault();
}

/// The repo's folders (AppSettings.RepoRoot): boards/<id>/, models/<id>/.
public static class Repo
{
    public static string Path(params string[] parts) => System.IO.Path.Combine(new[] { AppState.RepoRoot }.Concat(parts).ToArray());

    /// Sub-folder names of repo/<sub> (sorted, no dot folders); empty when missing.
    public static List<string> Dirs(string sub)
    {
        try
        {
            var p = Path(sub);
            return Directory.Exists(p) ? Directory.GetDirectories(p).Select(System.IO.Path.GetFileName).Where(n => n != null && !n.StartsWith('.')).Select(n => n!).OrderBy(n => n, StringComparer.Ordinal).ToList() : new();
        }
        catch (Exception) { return new(); }
    }

    /// The first "# " heading of a README, or "".
    public static string Title(string readme)
    {
        try { return File.Exists(readme) ? File.ReadLines(readme).FirstOrDefault(l => l.StartsWith("# "))?[2..] ?? "" : ""; }
        catch (Exception) { return ""; }
    }
}
