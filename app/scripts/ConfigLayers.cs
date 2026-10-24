// The firmware's config script as explicit LAYERS with provenance (product owner 2026-10-02: "we build a developer tool —
// nothing implicit or read-only"; docs/protocol.md "Validate first" / `config` / `diff`, authoritative):
//   1 board     the board's base script (boards/<board>/defaults.cli, a blob board's *.config)     file:line
//   2 aircraft  the aircraft's script (aircraft.json `firmware_cli`, e.g. models/trainer/inav.cli)  file:line
//   3 profile   the `profile` files / `profile_text`                                                file:line
//   4 device    the wired devices' lines ("device ublox on uart3")                                  toggle / override
//   5 bench     the bench's own lines (`bench_cli`, a file next to the bench)                       editable
// in that order; the same setting in two layers is a conflict and the later layer wins.
// - LayeredConfig.Parse: a session-check report → lines (layer, source, text, disabled) + conflicts. The report's
//   `config_json:` (one line, or a block until the JSON closes; `{kind,layers,lines,disabled,conflicts}`) is the
//   source; `config_src:` / `config_disabled:` / `config_conflict:` (their textual form) when a core gives no JSON;
//   `config_cli|` lines (unannotated) and the `defaults:` blob otherwise. A report without layers (a core before
//   config-layers) is attributed by the app (Infer: the bench's files matched line by line, marked "inferred").
//   Conflicts: the core's `conflicts` (by `wins`), else computed here (SettingKey), marked so.
// - ConfigChanges: `{"type":"diff","against":"script"}`'s answer — settings the configurator changed, added, removed.
// - BenchFiles: the files a bench owns next to its JSON (the bench lines `./<bench>.cli`, copies of a preset's aircraft /
//   board files `./<bench>.files/…`): copy-on-write, rename, delete, export as a preset folder.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using Godot;

namespace FlightBench;

/// One line of the assembled script with where it comes from.
public sealed class ConfigLine
{
    public string Text = "";
    /// board | aircraft | device | bench | "" (not traced).
    public string Layer = "";
    /// `boards/speedybee-f405-wing/defaults.cli:12`, `device ublox on uart3`, `bench board config (step 4)`.
    public string Source = "";
    /// Its index within its layer (the `layer:index` form of `disabled_lines`); -1 unknown.
    public int LayerIndex = -1;
    /// Skipped by `disabled_lines` (the core still lists it).
    public bool Disabled;
    /// The layer / source were matched by the app, not reported by the core.
    public bool Inferred;
    /// The setting this line writes (SettingKey) when another layer writes it too.
    public string? Conflict;
    /// A later line (a later layer) writes the same setting: this one does not win.
    public bool Overridden;
    /// The `disabled_lines` entry that matched this line (the core's `by`, or `config_disabled:`'s quoted entry).
    public string? DisabledBy;

    /// The file of a `file:line` source (null for a device / text source).
    public string? File => SplitSource(Source).File;
    /// The 1-based line of a `file:line` source (0 unknown).
    public int FileLine => SplitSource(Source).Line;

    public string Ref => LayerIndex >= 0 && Layer != "" ? $"{Layer}:{LayerIndex}" : Text;

    public static (string? File, int Line) SplitSource(string source)
    {
        var s = (source ?? "").Trim();
        if (s == "" || s.StartsWith("device ", StringComparison.Ordinal) || s.StartsWith("bench ", StringComparison.Ordinal)) return (null, 0);
        int c = s.LastIndexOf(':');
        if (c > 1 && int.TryParse(s[(c + 1)..], out var n)) return (s[..c], n);
        return s.Contains('/') || s.Contains('\\') || s.EndsWith(".cli", StringComparison.OrdinalIgnoreCase) ? (s, 0) : (null, 0);
    }
}

/// Two or more lines of different layers writing one setting; the last one wins.
public sealed class ConfigConflict
{
    public string Setting = "";
    public List<ConfigLine> Lines = new();
    public string Winner => Lines.Count > 0 ? Lines[^1].Layer : "";
    /// Computed by the app (the core did not report conflicts).
    public bool Computed;
    public string Describe() =>
        $"{Setting}: " + string.Join(" → ", Lines.Select(l => $"{(l.Layer != "" ? l.Layer : "?")} '{l.Text}'")) + $" — later layer wins ({Winner})";
}

public sealed class LayeredConfig
{
    // docs/protocol.md: the script's layers, in order — board, aircraft, profile (the `profile` files / `profile_text`),
    // device (the wired devices' own lines) and bench (the bench's own lines, `bench_cli`); the fifth, read-only layer
    // (the firmware's current config, `diff all`) is not one of these — it is the right side of Script ↔ Firmware.
    public static readonly string[] Layers = { "board", "aircraft", "profile", "device", "bench" };
    public static int LayerNo(string layer) => Array.IndexOf(Layers, layer) + 1;       // 1..5, 0 = not traced
    public static string LayerTitle(string layer) => layer switch
    {
        "board" => "1 · board", "aircraft" => "2 · aircraft", "profile" => "3 · profile", "device" => "4 · device", "bench" => "5 · bench", _ => "? · not traced",
    };
    public static Color LayerColor(string layer) => layer switch
    {
        "board" => new Color(0.45f, 0.65f, 1f), "aircraft" => new Color(0.45f, 0.9f, 0.55f), "profile" => new Color(0.5f, 0.85f, 0.9f),
        "device" => new Color(1f, 0.72f, 0.35f), "bench" => new Color(0.85f, 0.6f, 1f), _ => new Color(0.7f, 0.7f, 0.7f),
    };
    /// The files each layer reads, in order (the core's `config_json.layers`: `{layer,file,path}`) — when the core reports
    /// them; more authoritative than the app's own guess at the layer's file (`LayersView`'s fallback).
    public List<(string Layer, string File, string Path)> LayerFiles = new();

    /// "cli" (typed into the CLI), "defaults" (a blob board's custom defaults), "none".
    public string Kind = "none";
    public List<ConfigLine> Lines = new();
    public List<ConfigConflict> Conflicts = new();
    /// The report's `config:` lines (the wired devices' own lines, `# <device> on <pad>` comments included).
    public List<string> DeviceLines = new();
    public int DefaultsBytes;
    /// The core reported layers (config_json / annotated lines); false = none, or inferred by the app (Inferred).
    public bool Layered;
    public bool Inferred;
    /// The conflicts were computed by the app.
    public bool ConflictsComputed;
    /// What the app noticed about this report (a core that ignores bench_cli / disabled_lines…), shown with the summary.
    public List<string> Notes = new();

    public IEnumerable<ConfigLine> Enabled => Lines.Where(l => !l.Disabled);
    public string Text => string.Join("\n", Enabled.Select(l => l.Text)) + (Enabled.Any() ? "\n" : "");
    public int Count(string layer) => Lines.Count(l => l.Layer == layer && !l.Disabled);

    /// One line above the view: what, how many lines per layer, where the layers come from.
    public string Summary
    {
        get
        {
            if (Kind == "none") return "the core reported no first-start config for this bench";
            var per = string.Join(", ", Layers.Select(l => $"{l} {Count(l)}")) + (Lines.Any(l => l.Layer == "" && !l.Disabled) ? $", not traced {Lines.Count(l => l.Layer == "" && !l.Disabled)}" : "");
            var dis = Lines.Count(l => l.Disabled) is > 0 and var d ? $", {d} disabled" : "";
            var what = Kind == "cli" ? $"{Enabled.Count()} CLI lines typed into the firmware at its first start, then save"
                                     : $"custom defaults placed with the firmware ({DefaultsBytes} bytes, {Enabled.Count()} lines)";
            var src = Layered ? "layers from the core" : Inferred ? "layers inferred by the app (this core does not report them)" : "no layers";
            var conf = Conflicts.Count > 0 ? $"; {Conflicts.Count} conflict(s), later layer wins{(ConflictsComputed ? " (found by the app)" : "")}" : "";
            return $"{what} — {per}{dis}; {src}{conf}" + string.Concat(Notes.Select(n => "\n⚠ " + n));
        }
    }

    // ---- parsing ----
    // docs/protocol.md "Validate first": the session-check report of a CLI board has, after `manifest:`, the raw
    // script (`config_cli: N lines` / `config_cli| <line>`, unannotated), then its layers: `config_src: [<layer>:
    // <source>] <text>` for every line sent, `config_disabled: [<layer>:<source>] <text> (disabled_lines '<entry>')`
    // in its place for a disabled one, `config_conflict: <setting>: [<layer>:<source>] <text> -> [<layer>:<source>]
    // <text> (the last wins)` per conflict, then one line `config_json: {...}` (every board — the structured, and
    // authoritative, form of the above: `{kind,layers:[{layer,file,path}],lines:[{text,layer,source,ref}],
    // disabled:[{text,layer,source,ref,by}],conflicts:[{setting,lines:[…],wins}]}`). The older `config: <line>`
    // lines (after `machine:`) are unchanged: the wired devices' own lines, disabled ones included, not annotated.
    public static LayeredConfig Parse(string stdout)
    {
        var c = new LayeredConfig();
        var lines = stdout.Replace("\r\n", "\n").Split('\n');
        string? json = null;
        var conflictLines = new List<string>();
        var srcLines = new List<ConfigLine>();
        var disabledLines = new List<ConfigLine>();
        var cliLines = new List<ConfigLine>();
        for (int i = 0; i < lines.Length; i++)
        {
            var l = lines[i];
            if (l.StartsWith("config: ", StringComparison.Ordinal)) c.DeviceLines.Add(l["config: ".Length..]);
            else if (l.StartsWith("config_src: ", StringComparison.Ordinal))
            {
                if (ParseAnnotated(l["config_src: ".Length..]) is { } cl) srcLines.Add(cl);
            }
            else if (l.StartsWith("config_disabled: ", StringComparison.Ordinal))
            {
                var rest = l["config_disabled: ".Length..];
                var dm = Regex.Match(rest, @"^(.*?)\s*\(disabled_lines '(.*)'\)\s*$");
                var body = dm.Success ? dm.Groups[1].Value : rest;
                if (ParseAnnotated(body) is { } cl) { cl.Disabled = true; if (dm.Success) cl.DisabledBy = dm.Groups[2].Value; disabledLines.Add(cl); }
            }
            else if (l.StartsWith("config_conflict:", StringComparison.Ordinal)) conflictLines.Add(l["config_conflict:".Length..].Trim());
            else if (l.StartsWith("config_json:", StringComparison.Ordinal))
            {
                var sb = new StringBuilder(l["config_json:".Length..].Trim());
                // a block: more lines until the JSON parses
                int j = i;
                while (!ParsesAsObject(sb.ToString()) && j + 1 < lines.Length && j - i < 20000) { j++; sb.Append('\n').Append(lines[j]); }
                if (ParsesAsObject(sb.ToString())) { json = sb.ToString(); i = j; }
            }
            else if (l.StartsWith("config_cli| ", StringComparison.Ordinal) || l == "config_cli|")
            {
                c.Kind = "cli";
                cliLines.Add(new ConfigLine { Text = l.Length > 12 ? l[12..] : "" });
            }
            else if (l.StartsWith("defaults: ", StringComparison.Ordinal) && c.Kind != "cli")
            {
                var head = l["defaults: ".Length..].Trim();
                if (head == "none") continue;
                int.TryParse(head.Split(' ')[0], out c.DefaultsBytes);
                c.Kind = "defaults";
                // the rest of the report is the blob (free text): its lines as they are, the trailing blank ones dropped
                var rest = lines.Skip(i + 1).ToList();
                while (rest.Count > 0 && rest[^1].Trim() == "") rest.RemoveAt(rest.Count - 1);
                c.Lines.AddRange(rest.Select(x => new ConfigLine { Text = x }));
                break;
            }
        }
        // a blob board's raw defaults (captured above from `defaults:`) is what "Copy" / the layered view shows for it;
        // its `config_json` (`kind:"blob"`) carries only the device lines with provenance — informational (`layers`),
        // not a replacement for the blob's bytes.
        if (json != null) c.ApplyJson(json, keepLines: c.Kind == "defaults");
        else if (srcLines.Count > 0)
        {
            // config_src / config_disabled / config_conflict without config_json (an older or stand-in core): the
            // same information in its textual form.
            if (c.Kind == "none") c.Kind = "cli";
            c.Lines = srcLines.Concat(disabledLines).ToList();
            c.Layered = true;
            c.DeviceLines.AddRange(srcLines.Where(a => a.Layer == "device").Select(a => a.Text));
        }
        else if (cliLines.Count > 0)
        {
            // the raw script only, no provenance at all (a core before config-layers): untraced — Infer() attributes it.
            c.Lines = cliLines;
        }
        if (c.Conflicts.Count == 0 && conflictLines.Count > 0) c.ConflictsFromLines(conflictLines);
        c.NumberLayers();
        return c;
    }

    /// `[<layer>:<source>] <text>` (`config_src:` / `config_disabled:`'s body).
    private static ConfigLine? ParseAnnotated(string s)
    {
        var m = Regex.Match(s, @"^\[(board|aircraft|profile|device|bench):([^\]]*)\]\s?(.*)$");
        return m.Success ? new ConfigLine { Layer = m.Groups[1].Value, Source = m.Groups[2].Value.Trim(), Text = m.Groups[3].Value } : null;
    }

    private static bool ParsesAsObject(string s)
    {
        if (!s.TrimStart().StartsWith('{')) return false;
        try { using var d = JsonDocument.Parse(s); return d.RootElement.ValueKind == JsonValueKind.Object; }
        catch (JsonException) { return false; }
    }

    /// The core's `config_json` (or a `config export`'s `assembly`, the same shape): `{"kind","layers":[{layer,file,
    /// path}],"lines":[{text,layer,source,ref}],"disabled":[{text,layer,source,ref,by}],"conflicts":[{setting,
    /// lines:[…],wins}]}`. `ref` is `"<layer>:<n>"`, 1-based within its layer.
    public void ApplyJson(string json, bool keepLines = false)
    {
        using var d = JsonDocument.Parse(json);
        ApplyJson(d.RootElement, keepLines);
    }

    /// `keepLines`: a blob board whose raw defaults bytes are already `Lines` (from `defaults:`) — its `config_json`
    /// (`kind:"blob"`) only adds `layers` / informational data, it does not replace the blob's text.
    public void ApplyJson(JsonElement r, bool keepLines = false)
    {
        if (!keepLines && Json.Str(r, "kind") is { Length: > 0 } k) Kind = k;
        if (r.TryGetProperty("layers", out var lys) && lys.ValueKind == JsonValueKind.Array)
        {
            LayerFiles = new List<(string, string, string)>();
            foreach (var e in lys.EnumerateArray())
                if (e.ValueKind == JsonValueKind.Object)
                    LayerFiles.Add((Json.Str(e, "layer") ?? "", Json.Str(e, "file") ?? "", Json.Str(e, "path") ?? ""));
        }
        if (!keepLines && r.TryGetProperty("lines", out var arr) && arr.ValueKind == JsonValueKind.Array)
        {
            Lines = new List<ConfigLine>();
            foreach (var e in arr.EnumerateArray())
            {
                if (e.ValueKind == JsonValueKind.String) { Lines.Add(new ConfigLine { Text = e.GetString() ?? "" }); continue; }
                if (e.ValueKind != JsonValueKind.Object) continue;
                Lines.Add(LineFromJson(e));
            }
            Layered = Lines.Any(l => l.Layer != "");
            if (Kind == "none" && Lines.Count > 0) Kind = "cli";
        }
        if (!keepLines && r.TryGetProperty("disabled", out var dis) && dis.ValueKind == JsonValueKind.Array)
            foreach (var e in dis.EnumerateArray())
                if (e.ValueKind == JsonValueKind.Object)
                {
                    var line = LineFromJson(e);
                    line.Disabled = true;
                    line.DisabledBy = Json.Str(e, "by");
                    Lines.Add(line);
                    Layered = true;
                }
        if (!keepLines && r.TryGetProperty("conflicts", out var cs) && cs.ValueKind == JsonValueKind.Array)
        {
            Conflicts = new List<ConfigConflict>();
            foreach (var e in cs.EnumerateArray())
            {
                if (e.ValueKind != JsonValueKind.Object) continue;
                var cf = new ConfigConflict { Setting = Json.Str(e, "setting") ?? Json.Str(e, "key") ?? "" };
                var wins = Json.Str(e, "wins") ?? Json.Str(e, "winner");
                if (e.TryGetProperty("lines", out var ls) && ls.ValueKind == JsonValueKind.Array)
                    foreach (var x in ls.EnumerateArray())
                    {
                        var text = x.ValueKind == JsonValueKind.String ? x.GetString() ?? "" : Json.Str(x, "text") ?? "";
                        var layer = x.ValueKind == JsonValueKind.Object ? Json.Str(x, "layer") ?? "" : "";
                        var xref = x.ValueKind == JsonValueKind.Object ? Json.Str(x, "ref") : null;
                        var src = x.ValueKind == JsonValueKind.Object ? Json.Str(x, "source") ?? "" : "";
                        // the line object itself when it is in the list (matched by ref, else text + layer), else a detached copy
                        var existing = (xref != null ? Lines.LastOrDefault(l => l.Layer == layer && l.Ref == xref) : null)
                                       ?? Lines.LastOrDefault(l => l.Text == text && (layer == "" || l.Layer == layer));
                        cf.Lines.Add(existing ?? new ConfigLine { Text = text, Layer = layer, Source = src, LayerIndex = RefIndex(xref) });
                    }
                // the core names the winner by ref (`wins`); make sure it is last (Winner / MarkConflicts read the last line)
                if (wins != null)
                {
                    var wi = cf.Lines.FindIndex(l => l.Ref == wins || (l.Layer + ":" + l.LayerIndex) == wins);
                    if (wi >= 0 && wi != cf.Lines.Count - 1) { var w = cf.Lines[wi]; cf.Lines.RemoveAt(wi); cf.Lines.Add(w); }
                }
                if (cf.Lines.Count > 0) Conflicts.Add(cf);
            }
            MarkConflicts(computed: false);
        }
    }

    private static ConfigLine LineFromJson(JsonElement e) => new()
    {
        Text = Json.Str(e, "text") ?? "", Layer = Json.Str(e, "layer") ?? "", Source = Json.Str(e, "source") ?? "",
        LayerIndex = RefIndex(Json.Str(e, "ref")) is var ri && ri >= 0 ? ri : (Json.TryNum(e, "layer_index", out var li) ? (int)li : -1),
    };

    /// The 1-based number after the colon of a `"<layer>:<n>"` ref (-1 when not that form).
    private static int RefIndex(string? r) => r != null && Regex.Match(r, @":(\d+)$") is { Success: true } m && int.TryParse(m.Groups[1].Value, out var n) ? n : -1;

    /// `config_conflict: <setting>: [<layer>:<source>] <text> -> [<layer>:<source>] <text> (the last wins)` (a core
    /// without `config_json`; the setting has no ':' itself, so the first one is the separator).
    private void ConflictsFromLines(List<string> ls)
    {
        foreach (var raw in ls)
        {
            var m = Regex.Match(raw, @"^([^:]+):\s*(.*?)\s*\(the last wins\)\s*$");
            var body = m.Success ? m.Groups[2].Value : raw;
            var setting = m.Success ? m.Groups[1].Value.Trim() : raw[..Math.Max(0, raw.IndexOf(':'))].Trim();
            var cf = new ConfigConflict { Setting = setting };
            foreach (var part in Regex.Split(body, @"\s*->\s*"))
                if (ParseAnnotated(part.Trim()) is { } cl)
                    cf.Lines.Add(Lines.LastOrDefault(l => l.Layer == cl.Layer && l.Text == cl.Text && l.Source == cl.Source) ?? cl);
            if (cf.Lines.Count > 1) Conflicts.Add(cf);
        }
        MarkConflicts(computed: false);
    }

    /// Per-layer indices where the core gave none (the 1-based order within a layer, as `ref` counts them).
    private void NumberLayers()
    {
        var n = new Dictionary<string, int>();
        foreach (var l in Lines)
        {
            if (l.Layer == "") continue;
            n.TryGetValue(l.Layer, out var i);
            var next = i + 1;
            if (l.LayerIndex < 0) l.LayerIndex = next;
            n[l.Layer] = next;
        }
    }

    private void MarkConflicts(bool computed)
    {
        ConflictsComputed = computed;
        foreach (var l in Lines) { l.Conflict = null; l.Overridden = false; }
        foreach (var c in Conflicts)
            for (int i = 0; i < c.Lines.Count; i++) { c.Lines[i].Conflict = c.Setting; c.Lines[i].Overridden = i < c.Lines.Count - 1; }
    }

    /// The setting a CLI line writes, for conflicts ("set nav_rth_altitude", "feature GPS", "serial 2", "aux 0"…), with the
    /// profile it is in when the script switched profiles (`control_profile 2`); null = not a setting (profile switches,
    /// `mmix reset`, comments, blob headers). `ctx` carries the profile selections through a script.
    public static string? SettingKey(string line, Dictionary<string, string>? ctx = null)
    {
        var t = (line ?? "").Trim();
        if (t == "" || t.StartsWith('#')) return null;
        var w = t.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        var cmd = w[0].ToLowerInvariant();
        if (cmd is "control_profile" or "battery_profile" or "mixer_profile" or "profile" or "rateprofile")
        {
            if (ctx != null) ctx[cmd] = w.Length > 1 ? w[1] : "";
            return null;
        }
        string? key = cmd switch
        {
            "set" when w.Length >= 2 => "set " + w[1].Split('=')[0].ToLowerInvariant(),
            "feature" when w.Length >= 2 => "feature " + w[1].TrimStart('-').ToUpperInvariant(),
            "aux" or "serial" or "servo" or "smix" or "mmix" or "adjrange" or "vtxtable" or "led" or "color" or "beacon" or "rxrange" or "logic" or "gvar" or "safehome" or "osd_layout"
                when w.Length >= 2 && w[1] != "reset" => $"{cmd} {w[1]}",
            "resource" when w.Length >= 3 => $"resource {w[1]} {w[2]}",
            "map" or "mixer" or "board_name" or "manufacturer_id" or "name" => cmd,
            _ => null,
        };
        if (key == null || ctx == null || ctx.Count == 0) return key;
        var sel = string.Join(",", ctx.Where(kv => kv.Value != "" && kv.Value != "1" && kv.Value != "0").OrderBy(kv => kv.Key, StringComparer.Ordinal).Select(kv => $"{kv.Key} {kv.Value}"));
        return sel == "" ? key : $"{key} @{sel}";
    }

    /// Conflicts computed here (the core reported none): the enabled lines grouped by setting; a group spanning two or more
    /// layers is a conflict, the last line wins.
    public void ComputeConflicts()
    {
        var ctx = new Dictionary<string, string>();
        var by = new Dictionary<string, List<ConfigLine>>();
        foreach (var l in Enabled)
        {
            if (SettingKey(l.Text, ctx) is not { } k) continue;
            if (!by.TryGetValue(k, out var list)) by[k] = list = new List<ConfigLine>();
            list.Add(l);
        }
        Conflicts = by.Where(kv => kv.Value.Select(l => l.Layer).Distinct().Count() > 1)
                      .Select(kv => new ConfigConflict { Setting = kv.Key, Lines = kv.Value, Computed = true }).ToList();
        MarkConflicts(computed: true);
    }

    // ---- inference for a core that does not report layers ----
    /// Attributes the lines of a report without layers to the bench's files (board, aircraft, profiles, pasted text, the
    /// wired devices' `config:` lines, the bench lines), in the order the core assembles them; every attributed line is
    /// marked Inferred. Lines the app cannot trace stay "" (shown as "not traced"). Then the conflicts, when the core gave none.
    public void Infer(BenchData b, string root)
    {
        if (!Layered && Lines.Count > 0)
        {
            var expected = new List<(string Text, string Layer, string Source)>();
            void File_(string layer, string? abs, string shown)
            {
                if (abs == null || !System.IO.File.Exists(abs)) return;
                var ls = System.IO.File.ReadAllLines(abs);
                for (int i = 0; i < ls.Length; i++)
                    if (Clean(ls[i]) is { } t) expected.Add((t, layer, $"{shown}:{i + 1}"));
            }
            var board = BenchFiles.BoardFile(b, root);
            File_("board", board?.Abs, board?.Shown ?? "");
            var air = BenchFiles.AircraftCli(b, root);
            File_("aircraft", air?.Abs, air?.Shown ?? "");
            if (b.ProfileFile != "") File_(b.ProfileFile.Replace('\\', '/').Contains("models/") ? "aircraft" : "bench", BenchData.ResolvePath(b.ProfileFile, root), b.ProfileFile);
            if (b.BoardConfig.Trim() != "")
            {
                var ls = b.BoardConfig.Replace("\r\n", "\n").Split('\n');
                for (int i = 0; i < ls.Length; i++) if (Clean(ls[i]) is { } t) expected.Add((t, "bench", $"bench board config (step 4):{i + 1}"));
            }
            string dev = "device";
            foreach (var d in DeviceLines)
            {
                var m = Regex.Match(d.Trim(), @"^#\s*(.+?)(?:\s*\(device config\))?$");
                if (m.Success) { dev = "device " + m.Groups[1].Value.Trim(); continue; }
                if (Clean(d) is { } t) expected.Add((t, "device", dev));
            }
            if (b.BenchCli != null) File_("bench", BenchData.ResolvePath(b.BenchCli, root), b.BenchCli);
            var used = new bool[expected.Count];
            int at = 0;
            foreach (var l in Lines)
            {
                var t = l.Text.Trim();
                int hit = -1;
                for (int i = at; i < expected.Count && hit < 0; i++) if (!used[i] && expected[i].Text == t) hit = i;
                for (int i = 0; i < at && hit < 0; i++) if (!used[i] && expected[i].Text == t) hit = i;
                if (hit < 0) continue;
                used[hit] = true; at = hit + 1;
                (l.Layer, l.Source, l.Inferred) = (expected[hit].Layer, expected[hit].Source, true);
            }
            Inferred = Lines.Any(l => l.Inferred);
            NumberLayers();
        }
        // lines the bench disables: a core with layers lists them (disabled) or leaves them out — then they are shown
        // (disabled) so they can be turned on again; a core without layers ignores disabled_lines: said, not faked
        int ignored = 0;
        foreach (var d in b.DisabledLines ?? new List<string>())
        {
            var hit = Lines.FirstOrDefault(l => l.Text.Trim() == d || l.Ref == d);
            if (hit != null && !Layered && !hit.Disabled) { ignored++; continue; }
            if (hit != null) hit.Disabled = true;
            else Lines.Add(new ConfigLine { Text = d, Layer = LayerOfRef(d), Source = "disabled on this bench (not in the script)", Disabled = true });
        }
        if (ignored > 0) Notes.Add($"this core does not know disabled_lines (a core before config-layers): {ignored} line(s) this bench disables are still typed");
        if (!Layered && b.BenchCli != null && System.IO.File.Exists(BenchData.ResolvePath(b.BenchCli, root)) && !Lines.Any(l => l.Layer == "bench" && l.Source.StartsWith(b.BenchCli, StringComparison.Ordinal))
            && System.IO.File.ReadAllLines(BenchData.ResolvePath(b.BenchCli, root)).Any(x => Clean(x) != null))
            Notes.Add("this core does not know bench_cli (a core before config-layers): the bench lines are not in its script");
        if (Conflicts.Count == 0) ComputeConflicts();
    }

    private static string LayerOfRef(string r) => Regex.Match(r, @"^(board|aircraft|profile|device|bench):\d+$") is { Success: true } m ? m.Groups[1].Value : "device";

    /// A script line as the core takes it: stripped; blank, `#` comments, `save` / `exit` dropped (null).
    public static string? Clean(string raw)
    {
        var t = raw.Trim();
        if (t == "" || t.StartsWith('#')) return null;
        var w = t.Split(' ')[0].ToLowerInvariant();
        return w is "save" or "exit" ? null : t;
    }

    // ---- the core's session-check, run on a bench ----
    /// Runs a session-check on `b` and returns the report's stdout (a temp session file, removed after). Tests replace it
    /// (`Checker`: e.g. `tools/fake_core.py --session-check`).
    public static Func<string, string, Task<CoreCheck>>? Checker;

    public static async Task<(LayeredConfig? Config, string Error)> CheckAsync(BenchData b, string? exe, string root)
    {
        var tmp = Path.Combine(Path.GetTempPath(), $"flightbench-layers-{System.Environment.ProcessId}-{Guid.NewGuid():N}.session.json");
        try
        {
            File.WriteAllText(tmp, JsonSerializer.Serialize(b.ToSession(root), Json.Files), new UTF8Encoding(false));
            CoreCheck c;
            if (Checker != null) c = await Checker(tmp, root);
            else if (exe != null) c = await CoreLauncher.CheckAsync(exe, tmp, root, 30000);
            else return (null, "no core to ask");
            if (c.RunError != null) return (null, c.RunError);
            if (c.Rejected) return (null, "the core rejects this bench: " + string.Join(" | ", c.SessionLines));
            if (c.ExitCode != 0) return (null, $"session-check exit {c.ExitCode}: {c.Stderr.Trim()}");
            var lc = Parse(c.Stdout);
            lc.Infer(b, root);
            return (lc, "");
        }
        catch (Exception e) { return (null, e.Message); }
        finally { try { File.Delete(tmp); } catch (Exception) { } }
    }
}

/// `{"type":"diff","against":"script"}`'s answer: what the configurator (or anything else) changed against the script sent.
public sealed class ConfigChanges
{
    /// `script` / `firmware` are the FULL LINE TEXTS (docs/protocol.md `diff against:"script"`), not bare values;
    /// `setting` may carry a non-default profile context (`set roll_rate @control_profile 2`).
    public sealed record Changed(string Setting, string Script, string Firmware, string Layer = "", string Source = "");
    public List<Changed> ChangedSettings = new();
    /// In the firmware's diff, not in the script: the configurator added / set them.
    public List<string> OnlyFirmware = new();
    /// In the script, not in the firmware's diff: removed, or back at the firmware's default.
    public List<string> OnlyScript = new();
    public string Sent = "", Current = "";
    public string? Error;
    public double? T;

    public int Total => ChangedSettings.Count + OnlyFirmware.Count + OnlyScript.Count;

    /// Does this message carry a diff against the script (`changed` / `only_firmware` / `only_script`)?
    public static bool IsChanges(JsonElement m) =>
        m.ValueKind == JsonValueKind.Object && (Json.Str(m, "against") == "script" || m.TryGetProperty("changed", out _) || m.TryGetProperty("only_firmware", out _));

    public static ConfigChanges FromMessage(JsonElement m)
    {
        var c = new ConfigChanges { Error = Json.Str(m, "error"), T = Json.TryNum(m, "t", out var t) ? t : null };
        c.Sent = TextOf(m, "sent"); c.Current = TextOf(m, "current");
        if (m.TryGetProperty("changed", out var ch) && ch.ValueKind == JsonValueKind.Array)
            foreach (var e in ch.EnumerateArray())
                if (e.ValueKind == JsonValueKind.Object)
                    c.ChangedSettings.Add(new Changed(Json.Str(e, "setting") ?? "", ValueOf(e, "script"), ValueOf(e, "firmware"), Json.Str(e, "layer") ?? "", Json.Str(e, "source") ?? ""));
        c.OnlyFirmware = Items(m, "only_firmware");
        c.OnlyScript = Items(m, "only_script");
        return c;
    }

    private static string ValueOf(JsonElement e, string key) =>
        e.TryGetProperty(key, out var v) ? v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : v.ValueKind == JsonValueKind.Null ? "" : v.GetRawText() : "";

    private static string TextOf(JsonElement m, string key)
    {
        if (!m.TryGetProperty(key, out var v)) return "";
        if (v.ValueKind == JsonValueKind.String) return (v.GetString() ?? "").Replace("\r\n", "\n");
        if (v.ValueKind == JsonValueKind.Array) return string.Join("\n", v.EnumerateArray().Select(x => x.ValueKind == JsonValueKind.String ? x.GetString() : Json.Str(x, "text")).Where(x => x != null));
        return "";
    }

    /// A list of lines: strings, or objects — `only_firmware` carries `{setting,firmware}`, `only_script`
    /// `{setting,script,layer,source}` (both full line texts); `text` / `setting`+`value` as a fallback.
    private static List<string> Items(JsonElement m, string key)
    {
        var list = new List<string>();
        if (!m.TryGetProperty(key, out var a) || a.ValueKind != JsonValueKind.Array) return list;
        foreach (var e in a.EnumerateArray())
        {
            if (e.ValueKind == JsonValueKind.String) list.Add(e.GetString() ?? "");
            else if (e.ValueKind == JsonValueKind.Object)
                list.Add(Json.Str(e, "text") ?? Json.Str(e, "firmware") ?? Json.Str(e, "script")
                    ?? (Json.Str(e, "setting") is { } s ? (e.TryGetProperty("value", out _) ? $"{s} = {ValueOf(e, "value")}" : s) : e.GetRawText()));
        }
        return list;
    }

    /// The firmware's line for a changed setting, as a bench line: `firmware` is already the firmware's whole line
    /// (docs/protocol.md `diff against:"script"`'s `changed`), typed as is.
    public static string BenchLineFor(Changed c) => c.Firmware.Trim();
}

/// The files a bench owns next to its JSON in the benches folder (`./…` paths in the bench): its bench lines
/// (`./<bench>.cli`, layer 5) and its copies of a preset's aircraft / board files (`./<bench>.files/…`, copy-on-write).
public static class BenchFiles
{
    /// The benches folder, absolute.
    public static string Dir => ProjectSettings.GlobalizePath(BenchData.Dir).Replace('\\', '/').TrimEnd('/');
    /// A path under the benches folder, absolute.
    public static string Abs(string rel) { try { return Path.GetFullPath(Path.Combine(Dir, rel)).Replace('\\', '/'); } catch (Exception) { return $"{Dir}/{rel}"; } }

    public static string CliRel(string benchName) => $"./{BenchData.SafeName(benchName)}.cli";
    public static string FolderRel(string benchName) => $"./{BenchData.SafeName(benchName)}.files";

    /// The bench's bench-lines file (its `benchCli`, else where it would be), absolute.
    public static string CliPath(BenchData b) => BenchData.ResolvePath(b.BenchCli ?? CliRel(b.Name), CoreLocator.DataRoot);

    public static string ReadBenchLines(BenchData b)
    {
        var p = CliPath(b);
        try { return File.Exists(p) ? File.ReadAllText(p).Replace("\r\n", "\n") : ""; }
        catch (Exception) { return ""; }
    }

    /// Writes the bench lines (layer 5) next to the bench and points the bench at the file. False + error on failure.
    public static bool WriteBenchLines(BenchData b, string text, out string error)
    {
        b.BenchCli ??= CliRel(b.Name);
        var t = text.Replace("\r\n", "\n");
        if (t != "" && !t.EndsWith('\n')) t += "\n";
        return Json.WriteAtomic(CliPath(b), t, out error);
    }

    /// Is the bench made from a bundled preset (its board / aircraft files are the bundle's: read-only)?
    public static bool FromPreset(BenchData b) => b.Preset != null || Presets.List().Any(p => p.Name == b.Name);

    /// The file of the board layer: the bench's board config file, else `boards/<board>/<defaults_cli>` (a CLI board) or the
    /// board's *.config (a blob board). (Abs, as shown).
    public static (string Abs, string Shown)? BoardFile(BenchData b, string root)
    {
        if (b.BoardConfigFile != "") return (BenchData.ResolvePath(b.BoardConfigFile, root), b.BoardConfigFile);
        var boardJson = Path.Combine(root, "boards", b.Board, "board.json");
        try
        {
            if (File.Exists(boardJson))
            {
                using var d = JsonDocument.Parse(File.ReadAllText(boardJson));
                if (Json.Str(d.RootElement, "defaults_cli") is { Length: > 0 } cli) return (Path.Combine(root, "boards", b.Board, cli).Replace('\\', '/'), $"boards/{b.Board}/{cli}");
            }
        }
        catch (Exception) { }
        if (BoardInfo.ConfigPath(b.Board) is { } cfg) return (cfg.Replace('\\', '/'), $"boards/{b.Board}/{Path.GetFileName(cfg)}");
        return null;
    }

    /// The aircraft.json the bench uses, absolute.
    public static string AircraftJson(BenchData b, string root) =>
        b.AircraftFile != null ? BenchData.ResolvePath(b.AircraftFile, root) : Path.Combine(root, "models", b.Aircraft, "aircraft.json").Replace('\\', '/');

    /// The aircraft's CLI script (`firmware_cli`, relative to the data root, or absolute in a copy). (Abs, as shown).
    public static (string Abs, string Shown)? AircraftCli(BenchData b, string root)
    {
        try
        {
            var aj = AircraftJson(b, root);
            if (!File.Exists(aj)) return null;
            using var d = JsonDocument.Parse(File.ReadAllText(aj));
            if (Json.Str(d.RootElement, "firmware_cli") is not { Length: > 0 } cli) return null;
            return (BenchData.ResolvePath(cli, root), cli);
        }
        catch (Exception) { return null; }
    }

    /// Where a line's source file is, absolute (relative sources are under the data root; `./x` next to the bench).
    public static string ResolveSource(string file, string root) => BenchData.ResolvePath(file, root);

    /// Is this file one the bench owns (next to it in the benches folder)?
    public static bool Owned(string abs) => abs.Replace('\\', '/').StartsWith(Dir + "/", StringComparison.OrdinalIgnoreCase);

    /// What saving an edit of `abs` does for this bench: write it in place, or copy it next to the bench first (a
    /// preset's aircraft / board file). (Copy, why).
    public static (bool Copy, string Why) EditPolicy(BenchData b, string abs, string root)
    {
        if (Owned(abs)) return (false, $"the bench's own file: {abs}");
        if (FromPreset(b))
            return (true, $"a bundled preset's file (read-only): saving copies it next to the bench ({Abs(FolderRel(b.Name)[2..])}/…) and points this bench at the copy");
        return (false, $"writes {abs} in the data root — every bench with this {(IsAircraftFile(b, abs, root) ? "aircraft" : "board")} uses it");
    }

    public static bool IsAircraftFile(BenchData b, string abs, string root)
    {
        var dir = Path.GetDirectoryName(AircraftJson(b, root))?.Replace('\\', '/') ?? "";
        return AircraftCli(b, root) is { } a && Same(a.Abs, abs) || abs.Replace('\\', '/').StartsWith(dir + "/", StringComparison.OrdinalIgnoreCase);
    }

    public static bool Same(string a, string b)
    {
        try { return string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase); }
        catch (Exception) { return false; }
    }

    /// Saves an edited source file: in place, or (a preset's aircraft / board file) as the bench's copy first — the
    /// aircraft folder (models/<aircraft>/ without visual/) to `./<bench>.files/models/<aircraft>/` with its
    /// aircraft.json's `firmware_cli` pointing at the copy (absolute) and the bench's `aircraftFile` at it; a board file
    /// to `./<bench>.files/boards/<board>/<file>` and the bench's `boardConfigFile` at it. Returns the file written.
    public static string SaveSource(BenchData b, string abs, string text, string root, out string note)
    {
        var (copy, _) = EditPolicy(b, abs, root);
        string target = abs;
        note = "";
        if (copy)
        {
            if (IsAircraftFile(b, abs, root))
            {
                var aj = CopyAircraft(b, root);
                var rel = Path.GetRelativePath(Path.GetDirectoryName(AircraftJson(new BenchData { Aircraft = b.Aircraft }, root))!, abs);
                target = Path.Combine(Path.GetDirectoryName(aj)!, rel).Replace('\\', '/');
                // the aircraft's CLI may live outside its folder (firmware_cli is root-relative): then the copy takes it too
                if (rel.StartsWith("..", StringComparison.Ordinal)) { target = Path.Combine(Path.GetDirectoryName(aj)!, Path.GetFileName(abs)).Replace('\\', '/'); }
                SetFirmwareCli(aj, target);
                note = $"the aircraft '{b.Aircraft}' copied to {Path.GetDirectoryName(aj)!.Replace('\\', '/')} (this bench's aircraftFile)";
            }
            else
            {
                target = Abs($"{FolderRel(b.Name)[2..]}/boards/{b.Board}/{Path.GetFileName(abs)}");
                b.BoardConfigFile = $"{FolderRel(b.Name)}/boards/{b.Board}/{Path.GetFileName(abs)}";
                note = $"the board file copied to {target} (this bench's boardConfigFile)";
            }
        }
        var t = text.Replace("\r\n", "\n");
        if (t != "" && !t.EndsWith('\n')) t += "\n";
        if (!Json.WriteAtomic(target, t, out var err)) throw new IOException($"cannot write {target}: {err}");
        return target;
    }

    /// Copies the bench's aircraft (once) to `./<bench>.files/models/<aircraft>/`; returns its aircraft.json (absolute).
    public static string CopyAircraft(BenchData b, string root)
    {
        var rel = $"{FolderRel(b.Name)}/models/{b.Aircraft}/aircraft.json";
        var dst = Abs(rel[2..]);
        if (b.AircraftFile != null && Same(BenchData.ResolvePath(b.AircraftFile, root), dst) && File.Exists(dst)) return dst;
        var srcDir = Path.GetDirectoryName(AircraftJson(b, root))!;
        var dstDir = Path.GetDirectoryName(dst)!;
        CopyDir(srcDir, dstDir, skip: "visual");
        // the aircraft's CLI (firmware_cli, data-root relative) into the copy when it is not in the folder already
        if (AircraftCli(b, root) is { } cli && File.Exists(cli.Abs))
        {
            var inCopy = Path.Combine(dstDir, Path.GetRelativePath(srcDir, cli.Abs));
            if (Path.GetRelativePath(srcDir, cli.Abs).StartsWith("..", StringComparison.Ordinal)) inCopy = Path.Combine(dstDir, Path.GetFileName(cli.Abs));
            if (!File.Exists(inCopy)) File.Copy(cli.Abs, inCopy);
            SetFirmwareCli(dst, inCopy);
        }
        b.AircraftFile = rel;
        return dst;
    }

    /// Points an aircraft.json's `firmware_cli` at `cliAbs` (absolute: the core resolves a relative one under the data root).
    public static void SetFirmwareCli(string aircraftJson, string cliAbs)
    {
        var text = File.ReadAllText(aircraftJson);
        var v = JsonSerializer.Serialize(cliAbs.Replace('\\', '/'));
        var re = new Regex("(\"firmware_cli\"\\s*:\\s*)\"(?:[^\"\\\\]|\\\\.)*\"");
        text = re.IsMatch(text) ? re.Replace(text, m => m.Groups[1].Value + v, 1) : text;
        File.WriteAllText(aircraftJson, text, new UTF8Encoding(false));
    }

    private static void CopyDir(string src, string dst, string? skip = null)
    {
        Directory.CreateDirectory(dst);
        foreach (var f in Directory.GetFiles(src)) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), overwrite: true);
        foreach (var d in Directory.GetDirectories(src))
            if (!string.Equals(Path.GetFileName(d), skip, StringComparison.OrdinalIgnoreCase)) CopyDir(d, Path.Combine(dst, Path.GetFileName(d)), skip);
    }

    /// The files of bench `name` next to it: `<name>.cli` and everything under `<name>.files/` (absolute).
    public static List<string> OwnFiles(string name)
    {
        var list = new List<string>();
        try
        {
            var cli = Abs(CliRel(name)[2..]);
            if (File.Exists(cli)) list.Add(cli);
            var dir = Abs(FolderRel(name)[2..]);
            if (Directory.Exists(dir)) list.AddRange(Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Select(f => f.Replace('\\', '/')));
        }
        catch (Exception) { }
        return list;
    }

    /// After a delete: the bench's `<name>.files/` folder tree when nothing is left in it.
    public static void RemoveEmptyFolder(string name)
    {
        try
        {
            var dir = Abs(FolderRel(name)[2..]);
            if (Directory.Exists(dir) && Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Length == 0) Directory.Delete(dir, true);
        }
        catch (Exception) { }
    }

    /// A bench saved under a new name (Save as, the wizard's rename) is a new bench: its own files are copied to the new
    /// name's (`./<new>.cli`, `./<new>.files/`) and its paths follow; the old bench keeps its files.
    public static void Rehome(BenchData b, string oldName, bool move = false)
    {
        if (BenchData.SafeName(oldName) == BenchData.SafeName(b.Name)) return;
        string oldCli = CliRel(oldName), oldDir = FolderRel(oldName), newCli = CliRel(b.Name), newDir = FolderRel(b.Name);
        try
        {
            if (b.BenchCli == oldCli)
            {
                if (File.Exists(Abs(oldCli[2..])))
                {
                    Directory.CreateDirectory(Dir);
                    if (move) File.Move(Abs(oldCli[2..]), Abs(newCli[2..]), overwrite: true);
                    else File.Copy(Abs(oldCli[2..]), Abs(newCli[2..]), overwrite: true);
                }
                b.BenchCli = newCli;
            }
            bool uses = Uses(b.AircraftFile, oldDir) || Uses(b.BoardConfigFile, oldDir) || Uses(b.ProfileFile, oldDir);
            if (uses && Directory.Exists(Abs(oldDir[2..])))
            {
                CopyDir(Abs(oldDir[2..]), Abs(newDir[2..]));
                if (move) try { Directory.Delete(Abs(oldDir[2..]), true); } catch (Exception) { }
                // an aircraft copy's firmware_cli is absolute: point it into the new folder
                foreach (var aj in Directory.GetFiles(Abs(newDir[2..]), "aircraft.json", SearchOption.AllDirectories))
                {
                    try
                    {
                        using var d = JsonDocument.Parse(File.ReadAllText(aj));
                        if (Json.Str(d.RootElement, "firmware_cli") is { } cli && cli.Replace('\\', '/').StartsWith(Abs(oldDir[2..]) + "/", StringComparison.OrdinalIgnoreCase))
                            SetFirmwareCli(aj, Abs(newDir[2..]) + cli.Replace('\\', '/')[Abs(oldDir[2..]).Length..]);
                    }
                    catch (Exception) { }
                }
            }
            string Move(string p) => Uses(p, oldDir) ? newDir + p[oldDir.Length..] : p;
            if (b.AircraftFile != null) b.AircraftFile = Move(b.AircraftFile);
            b.BoardConfigFile = Move(b.BoardConfigFile);
            b.ProfileFile = Move(b.ProfileFile);
        }
        catch (Exception e) { GD.PushWarning($"bench '{b.Name}': its files from '{oldName}' not copied: {e.Message}"); }
    }

    private static bool Uses(string? p, string dirRel) => p != null && p.StartsWith(dirRel + "/", StringComparison.Ordinal);

    /// A preset copied into the benches folder (Presets.Open) brings the files it names with `./` from its own folder
    /// (an exported preset: `<id>.cli`, `<id>.files/`); an aircraft copy's `firmware_cli` is pointed at the new place.
    public static List<string> CopyFromPreset(BenchData b, string presetDir, string presetName)
    {
        var copied = new List<string>();
        void One(string? rel)
        {
            var own = BenchData.OwnRel(rel);
            if (own == null) return;                    // not `./…`, or `./../…` escaping the folder: nothing copied
            var src = Path.Combine(presetDir, own);
            var dst = Abs(own);
            if (!File.Exists(src)) return;
            Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
            File.Copy(src, dst, overwrite: true);
            copied.Add(dst);
        }
        One(b.BenchCli); One(b.BoardConfigFile); One(b.ProfileFile);
        if (BenchData.OwnRel(b.AircraftFile) is { } ownAircraft)
        {
            var srcDir = Path.GetDirectoryName(Path.Combine(presetDir, ownAircraft))!;
            var dstJson = Abs(ownAircraft);
            if (Directory.Exists(srcDir))
            {
                CopyDir(srcDir, Path.GetDirectoryName(dstJson)!);
                copied.Add(dstJson);
                try
                {
                    using var d = JsonDocument.Parse(File.ReadAllText(dstJson));
                    if (Json.Str(d.RootElement, "firmware_cli") is { } cli) SetFirmwareCli(dstJson, Path.Combine(Path.GetDirectoryName(dstJson)!, Path.GetFileName(cli)));
                }
                catch (Exception) { }
            }
        }
        // the preset's files carry the preset's safe name; a bench of another name gets them under its own
        if (BenchData.SafeName(presetName) != BenchData.SafeName(b.Name)) Rehome(b, presetName);
        return copied;
    }

    /// Export as preset: `<folder>/<id>.json` (the bench as a preset: its own files named `./<id>.cli` /
    /// `./<id>.files/…`, lastUsed / wiringLayout / preset dropped), `<id>.cli` (layer 5), `<id>.files/` (the bench's copies
    /// of aircraft / board files, only when it has them) and `<id>.README.md` (the layers: what each contributes, the
    /// conflicts, the firmware's diff when known). Returns the files written.
    public static List<string> ExportPreset(BenchData b, LayeredConfig? layers, string? firmwareDiff, string folder, string root)
    {
        var id = BenchData.SafeName(b.Name);
        var files = new List<string>();
        Directory.CreateDirectory(folder);
        var p = b.Clone();
        p.Name = b.Name; p.LastUsed = DateTime.MinValue; p.WiringLayout.Clear(); p.Preset = null; p.FirmwareDiff = null; p.Modes = null;
        p.Description ??= $"exported from the bench '{b.Name}' ({DateTime.Now:yyyy-MM-dd})";
        // layer 5
        var lines = ReadBenchLines(b);
        if (lines.Trim() != "")
        {
            var cli = Path.Combine(folder, id + ".cli");
            File.WriteAllText(cli, lines, new UTF8Encoding(false));
            files.Add(cli.Replace('\\', '/'));
            p.BenchCli = $"./{id}.cli";
        }
        else p.BenchCli = null;
        // the bench's own copies (a modified aircraft / board file)
        var own = Abs(FolderRel(b.Name)[2..]);
        bool hasOwn = Directory.Exists(own) && (Uses(b.AircraftFile, FolderRel(b.Name)) || Uses(b.BoardConfigFile, FolderRel(b.Name)) || Uses(b.ProfileFile, FolderRel(b.Name)));
        if (hasOwn)
        {
            var dst = Path.Combine(folder, id + ".files");
            CopyDir(own, dst);
            files.AddRange(Directory.GetFiles(dst, "*", SearchOption.AllDirectories).Select(f => f.Replace('\\', '/')));
            string Re(string x) => Uses(x, FolderRel(b.Name)) ? $"./{id}.files" + x[FolderRel(b.Name).Length..] : x;
            if (p.AircraftFile != null) p.AircraftFile = Re(p.AircraftFile);
            p.BoardConfigFile = Re(p.BoardConfigFile);
            p.ProfileFile = Re(p.ProfileFile);
            // the copy's firmware_cli (absolute on this machine) → the copy's own file; Presets.Open points it at the new place
            foreach (var aj in Directory.GetFiles(dst, "aircraft.json", SearchOption.AllDirectories))
            {
                try
                {
                    using var d = JsonDocument.Parse(File.ReadAllText(aj));
                    if (Json.Str(d.RootElement, "firmware_cli") is { } cli) SetFirmwareCli(aj, Path.Combine(Path.GetDirectoryName(aj)!, Path.GetFileName(cli)));
                }
                catch (Exception) { }
            }
        }
        var json = Path.Combine(folder, id + ".json");
        File.WriteAllText(json, JsonSerializer.Serialize(p, Json.Files), new UTF8Encoding(false));
        files.Insert(0, json.Replace('\\', '/'));
        var readme = Path.Combine(folder, id + ".README.md");
        File.WriteAllText(readme, Readme(b, p, id, layers, firmwareDiff, hasOwn), new UTF8Encoding(false));
        files.Add(readme.Replace('\\', '/'));
        return files;
    }

    private static string Readme(BenchData b, BenchData p, string id, LayeredConfig? layers, string? diff, bool hasOwn)
    {
        var sb = new StringBuilder();
        sb.AppendLine($"# Preset: {b.Name}");
        sb.AppendLine();
        sb.AppendLine($"Exported by FlightBench {DateTime.Now:yyyy-MM-dd HH:mm} from the bench '{b.Name}' ({b.Board} / {b.Aircraft}).");
        sb.AppendLine("Install: copy this folder's files into `<data root>/presets/` (the repo's `presets/`, an exported build's `data/presets/`).");
        sb.AppendLine("Opening it copies the bench and its own files (`./…` paths) into the benches folder; the preset files are never written.");
        sb.AppendLine();
        sb.AppendLine("## Files");
        sb.AppendLine($"- `{id}.json` — the bench (board, firmware, aircraft, wiring, core time, windows)");
        if (p.BenchCli != null) sb.AppendLine($"- `{id}.cli` — layer 5, the bench lines (`benchCli`)");
        if (hasOwn) sb.AppendLine($"- `{id}.files/` — the bench's own copies of modified files ({string.Join(", ", new[] { p.AircraftFile, p.BoardConfigFile != "" ? p.BoardConfigFile : null, p.ProfileFile != "" ? p.ProfileFile : null }.Where(x => x != null && x.StartsWith($"./{id}.files", StringComparison.Ordinal)))})");
        sb.AppendLine($"- `{id}.README.md` — this file");
        sb.AppendLine();
        sb.AppendLine("## Config layers (the script the core types into the firmware at its first start; a later layer wins)");
        if (layers == null) sb.AppendLine("(not known at export: no session-check result)");
        else
        {
            sb.AppendLine(layers.Summary);
            sb.AppendLine();
            foreach (var layer in LayeredConfig.Layers.Append(""))
            {
                var ls = layers.Lines.Where(l => l.Layer == layer).ToList();
                if (ls.Count == 0) continue;
                var sources = ls.Select(l => l.File ?? l.Source).Distinct().Take(6);
                sb.AppendLine($"### {LayeredConfig.LayerTitle(layer)} — {ls.Count(l => !l.Disabled)} lines{(ls.Any(l => l.Disabled) ? $", {ls.Count(l => l.Disabled)} disabled" : "")} ({string.Join(", ", sources)})");
                sb.AppendLine("```");
                foreach (var l in ls) sb.AppendLine((l.Disabled ? "# disabled: " : "") + l.Text + (l.Overridden ? "    # overridden by a later layer" : ""));
                sb.AppendLine("```");
            }
            if (layers.Conflicts.Count > 0)
            {
                sb.AppendLine("### Conflicts (later layer wins)");
                foreach (var c in layers.Conflicts) sb.AppendLine("- " + c.Describe());
            }
            if (b.DisabledLines is { Count: > 0 } dis) sb.AppendLine($"\nDisabled lines (`disabledLines`): {string.Join(", ", dis.Select(x => $"`{x}`"))}");
        }
        if (!string.IsNullOrWhiteSpace(diff))
        {
            sb.AppendLine();
            sb.AppendLine("## The firmware's `diff all` at export (informational)");
            sb.AppendLine("```");
            sb.AppendLine(diff.TrimEnd());
            sb.AppendLine("```");
        }
        return sb.ToString();
    }
}
