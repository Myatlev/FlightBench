// Bundled presets: ready benches shipped with the app in `<data root>/presets/*.json` (repo `presets/`, the
// exported build's `data/presets/`), in the same form the wizard saves. Their paths (firmware, board config,
// profile) are relative to the data root, so a fresh install flies at once. Opening one copies it into the
// benches folder (user://benches, or FB_BENCH_DIR) under its name; the bundle itself is never written. A
// bench of that name that already exists is opened as it is (the user may have changed it), not replaced.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Godot;

namespace FlightBench;

public sealed record Preset(string File, BenchData Bench)
{
    /// The file name without `.json` (`rs7-first-flight`): FB_PRESET matches it or the bench name.
    public string Id => Path.GetFileNameWithoutExtension(File);
    public string Name => Bench.Name;
    public string Description => Bench.Description ?? "";
}

public static class Presets
{
    /// `<data root>/presets` (CoreLocator.DataRoot: the exported build's data/, else the repo root).
    public static string Dir => Path.Combine(CoreLocator.DataRoot, "presets");

    /// Every preset that parses as a bench, sorted by file name; a file that does not is reported in `errors`.
    public static List<Preset> List(List<string>? errors = null)
    {
        var list = new List<Preset>();
        try
        {
            if (!Directory.Exists(Dir)) return list;
            foreach (var f in Directory.GetFiles(Dir, "*.json").OrderBy(f => f, StringComparer.Ordinal))
            {
                try { list.Add(new Preset(f, BenchData.Parse(File.ReadAllText(f)))); }
                catch (Exception e) { errors?.Add($"{Path.GetFileName(f)}: {e.Message}"); GD.PushWarning($"preset {f}: {e.Message}"); }
            }
        }
        catch (Exception e) { errors?.Add(e.Message); }
        return list;
    }

    /// The paths a preset names (firmware, board config file, profile file), each with where it resolves under
    /// `root` and whether it is there. Relative ones only are "bundled"; absolute ones are reported as they are.
    public static List<(string What, string Path, string Resolved, bool Exists)> Paths(BenchData b, string root)
    {
        var list = new List<(string, string, string, bool)>();
        void Add(string what, string p)
        {
            if (string.IsNullOrWhiteSpace(p)) return;
            var r = BenchData.ResolvePath(p, root);
            list.Add((what, p, r, File.Exists(r)));
        }
        Add("firmware", b.FirmwarePath);
        Add("board config", b.BoardConfigFile);
        Add("profile", b.ProfileFile);
        Add("visual", b.Visual ?? "");
        // an exported preset's own files (`./…`: next to the preset file)
        foreach (var (what, p) in new[] { ("bench lines", b.BenchCli), ("aircraft", b.AircraftFile) })
            if (p != null && BenchData.IsBenchRelative(p))
                list.Add((what, p, BenchData.OwnRel(p) != null ? "(next to the preset)" : "(leaves the preset folder: ignored)", BenchData.OwnRel(p) != null));
        return list;
    }

    /// Opens a preset as a bench: copies it into the benches folder under its name (`BenchData.PathFor`) and
    /// returns that bench; when a bench of that name exists it is loaded instead (`existed`), unchanged. The
    /// preset file is only read. `note` says what happened (for the flight screen's log).
    public static BenchData? Open(Preset p, out bool existed, out string note)
    {
        var target = BenchData.PathFor(p.Name);
        existed = Godot.FileAccess.FileExists(target);
        if (existed)
        {
            var have = BenchData.Load(target);
            note = have != null && have.Name != p.Name
                ? $"preset '{p.Name}': its bench file {ProjectSettings.GlobalizePath(target)} is taken by the bench '{have.Name}' (same file name): opened that one as it is, the preset was not copied; rename that bench to get the preset"
                : have != null
                ? $"preset '{p.Name}': a bench of that name exists ({ProjectSettings.GlobalizePath(target)}): opened it as it is, the preset was not copied again"
                : $"preset '{p.Name}': a bench file of that name exists but does not load ({ProjectSettings.GlobalizePath(target)}): left as it is, nothing opened";
            return have;
        }
        BenchData b;
        try { b = BenchData.Parse(File.ReadAllText(p.File)); }
        catch (Exception e) { note = $"preset {p.File}: {e.Message}"; return null; }
        b.Name = p.Name;
        b.LastUsed = DateTime.Now;
        b.Preset = p.Id;                            // its aircraft / board files are the bundle's: an edit copies them first
        var own = BenchFiles.CopyFromPreset(b, Path.GetDirectoryName(p.File)!, p.Name);   // an exported preset's `./` files
        b.Save();                                   // Json.WriteAtomic creates the folder
        note = $"preset '{p.Name}' copied to {ProjectSettings.GlobalizePath(target)} (from {p.File}){(own.Count > 0 ? $" with its files {string.Join(", ", own)}" : "")}";
        return b;
    }
}
