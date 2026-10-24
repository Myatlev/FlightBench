// The firmware config belongs to the bench (core branch `bench-state`, docs/protocol.md "Launching the core"): the
// launcher passes `bench-state=<file>` in `-M`, a per-bench file next to the core's cfg-cache
// (`<cache>/<bench-id>.state.bin`; the core keeps `<bench-id>.state.json` metadata beside it). The core writes the
// firmware's config sector there after every save it sees (CLI `save`, MSP_EEPROM_WRITE, calibrations) and restores it
// at start, applying only the delta of the config script when the bench changed. Restarting a bench (Restart core,
// Edit bench → Save & start) passes the same file, so calibrations and configurator-set modes survive.
// bench-id = the bench's file name (BenchData.SafeName): a bench saved under another name is another bench (the wizard's
// rename = a new bench, the original stays) and starts from its own first start.
// Where: the core's cfg-cache directory (`XDG_CACHE_HOME`, else `%LOCALAPPDATA%` on Windows / `~/.cache`, +
// `flightbench/config`); `<bench dir>/state/` when FB_BENCH_DIR moves the benches (dev / test runs leave the real cache alone).
// An older core without the option (`-M <machine>,help` does not list `bench-state`) is started without it (logged).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Godot;

namespace FlightBench;

public static class BenchState
{
    /// The core's default cfg-cache directory: `<user cache dir>/flightbench/config` (core/qemu/README.md).
    public static string CacheDir
    {
        get
        {
            var xdg = System.Environment.GetEnvironmentVariable("XDG_CACHE_HOME");
            string baseDir = !string.IsNullOrWhiteSpace(xdg) ? xdg
                : OperatingSystem.IsWindows() ? System.Environment.GetEnvironmentVariable("LOCALAPPDATA") ?? System.Environment.GetFolderPath(System.Environment.SpecialFolder.LocalApplicationData)
                : Path.Combine(System.Environment.GetEnvironmentVariable("HOME") ?? "", ".cache");
            return Path.Combine(baseDir, "flightbench", "config").Replace('\\', '/');
        }
    }

    /// Where this run keeps the bench-state files: the cfg-cache, or `<bench dir>/state` when the benches' folder is moved.
    public static string Dir => BenchData.Dir == "user://benches" ? CacheDir : ProjectSettings.GlobalizePath(BenchData.Dir + "/state").Replace('\\', '/');

    /// The bench's id in the state file name (its file name: one bench file ↔ one state).
    public static string Id(string benchName) => BenchData.SafeName(benchName);

    /// `<dir>/<bench-id>.state.bin`: what the launcher passes as `bench-state=`.
    public static string PathFor(string benchName) => $"{Dir}/{Id(benchName)}.state.bin";

    /// Every file of the bench's state that exists: `<id>.state.bin`, its `.state.json` metadata, a leftover temp file.
    public static List<string> Files(string benchName)
    {
        var list = new List<string>();
        try
        {
            if (!Directory.Exists(Dir)) return list;
            var prefix = Id(benchName) + ".state.";
            list.AddRange(Directory.GetFiles(Dir).Where(f => Path.GetFileName(f).StartsWith(prefix, StringComparison.Ordinal)).Select(f => f.Replace('\\', '/')));
        }
        catch (Exception e) { GD.PushWarning($"bench state of '{benchName}': {e.Message}"); }
        return list;
    }
}
