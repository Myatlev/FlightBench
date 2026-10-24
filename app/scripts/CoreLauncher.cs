// The local core (docs/protocol.md "Launching the core"): find qemu-system-arm (CoreLocator), validate a
// bench with session-check (CoreCheck: the report, its `launch:` line, the `session:` errors), start it
// with the contract's command line (CoreLauncher.StartArgs), wait for the control port, stop it
// (CoreProcess: TerminateProcess on Windows, SIGTERM elsewhere; only processes this app started).
// Everything that waits runs on background tasks (CoreLaunch.Task): the main thread polls.
// Output of a started core is read line by line from both pipes (no deadlock) into a bounded buffer the
// flight screen drains into the Log window with a `core:` prefix.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Godot;

namespace FlightBench;

/// The five ports of one core: control (bench `ports.control`), msp (`ports.msp`), and the HMP monitor,
/// motion and pilot sockets, which the bench schema does not carry (31236/31237/31238 by default).
public readonly record struct CorePorts(int Control, int Msp, int Monitor, int Motion, int Pilot)
{
    public const int DefaultControl = 31250, DefaultMsp = 31235, DefaultMonitor = 31236, DefaultMotion = 31237, DefaultPilot = 31238;
    public static CorePorts From(int control, int msp) => new(control, msp, DefaultMonitor, DefaultMotion, DefaultPilot);
    /// All five moved together by n (the "next free ports" offer).
    public CorePorts Offset(int n) => new(Control + n, Msp + n, Monitor + n, Motion + n, Pilot + n);
    public int[] All => new[] { Control, Msp, Monitor, Motion, Pilot };
    public bool Valid => All.All(p => p is > 0 and <= 65535) && All.Distinct().Count() == 5;
    public override string ToString() => $"control {Control}, msp {Msp}, monitor {Monitor}, motion {Motion}, pilot {Pilot}";
}

/// Where the core executable and the data root are.
public static class CoreLocator
{
    public static string ExeName => OperatingSystem.IsWindows() ? "qemu-system-arm.exe" : "qemu-system-arm";
    private static string ExeDir => Path.GetDirectoryName(OS.GetExecutablePath()) ?? "";
    private static bool HasData(string d) => Directory.Exists(Path.Combine(d, "boards")) && Directory.Exists(Path.Combine(d, "models"));

    /// `<exe dir>/data/core/qemu-system-arm(.exe)` (what app/tools/export.ps1 bundles).
    public static string BundledPath => Path.Combine(ExeDir, "data", "core", ExeName);

    /// The core to start: the bundled one (exported build), else Settings → Paths → core (CorePath), else
    /// FB_CORE_EXE. Null when none exists; `source` says which one was taken or why none was.
    public static string? Find(out string source)
    {
        var tried = new List<string>();
        if (File.Exists(BundledPath)) { source = "bundled data/core"; return Path.GetFullPath(BundledPath); }
        var s = (AppSettings.Current.CorePath ?? "").Trim();
        if (s != "")
        {
            if (File.Exists(s)) { source = "Settings → Paths"; return Path.GetFullPath(s); }
            tried.Add($"CorePath {s} does not exist");
        }
        if (DevEnv.CoreExe is { } e)
        {
            if (File.Exists(e)) { source = "FB_CORE_EXE"; return Path.GetFullPath(e); }
            tried.Add($"FB_CORE_EXE {e} does not exist");
        }
        source = tried.Count > 0 ? string.Join("; ", tried) : "no bundled data/core, no CorePath, no FB_CORE_EXE";
        return null;
    }

    /// The data root the core resolves relative bench paths against (`root=`): `<exe dir>/data` in an
    /// exported build that has it, else the repo root the wizard uses for boards/ and models/.
    public static string DataRoot
    {
        get
        {
            if (!OS.HasFeature("editor"))
            {
                var d = Path.Combine(ExeDir, "data");
                if (HasData(d)) return Path.GetFullPath(d);
            }
            return AppState.RepoRoot;
        }
    }

    /// Is this core address this machine (the app may start the core), or a remote one (connect only)?
    public static bool IsLocal(string? host) =>
        string.IsNullOrWhiteSpace(host) || host.Trim() is "127.0.0.1" or "localhost" or "::1" or "[::1]";
}

/// A session-check report (docs/protocol.md "Validate first").
public sealed class CoreCheck
{
    public int ExitCode = -1;
    public string Stdout = "", Stderr = "";
    /// The check could not run at all (no exe, timeout): why.
    public string? RunError;
    /// `session: ...` lines of stderr (errors when rejected, warnings otherwise).
    public List<string> SessionLines = new();
    /// The `launch:` line's key=value pairs (keys may be added: unknown ones are kept, not needed).
    public Dictionary<string, string> Launch = new();
    public bool HasLaunchLine;
    public int? IcountShift;          // null = no -icount (icount_shift=none)
    public bool Realtime;
    public int Control, Msp;
    public string? ManifestJson;
    /// The QEMU machine to start (`machine=` of the launch line: nexus-xr, speedybee-f405-wing); nexus-xr when absent (older cores).
    public string Machine => Launch.TryGetValue("machine", out var m) && m != "" ? m : "nexus-xr";

    public bool Rejected => RunError == null && ExitCode == 2;
    public bool Ok => RunError == null && ExitCode == 0 && Control > 0 && Msp > 0;

    public string Summary => Ok
        ? $"machine {Machine}, icount {(IcountShift is { } s ? "shift=" + s : "none")}, realtime {(Realtime ? "on" : "off")}, control {Control}, msp {Msp}{(SessionLines.Count > 0 ? $", {SessionLines.Count} session note(s)" : "")}"
        : RunError ?? (Rejected ? $"rejected (exit 2): {string.Join(" | ", SessionLines)}" : $"exit {ExitCode}: {Stderr.Trim()}");

    /// The report for the Bench check panel: stdout (the manifest line shortened) + stderr.
    public string Report
    {
        get
        {
            var sb = new StringBuilder();
            if (RunError != null) sb.AppendLine("could not run the check: " + RunError);
            sb.AppendLine($"exit code {ExitCode}");
            foreach (var l in Lines(Stdout))
                sb.AppendLine(l.StartsWith("manifest: ", StringComparison.Ordinal) && l.Length > 160 ? l[..160] + $"… ({l.Length} chars)" : l);
            if (Stderr.Trim() != "") { sb.AppendLine("--- stderr"); sb.Append(Stderr.TrimEnd()).AppendLine(); }
            return sb.ToString();
        }
    }

    private static IEnumerable<string> Lines(string s) => s.Replace("\r\n", "\n").Split('\n');

    /// Parses the report: the first `launch: ` line before `manifest: ` (older reports: `icount:` / `ports:` /
    /// `realtime:`), the manifest line, the `session:` lines of stderr.
    public static CoreCheck Parse(int exitCode, string stdout, string stderr)
    {
        var c = new CoreCheck { ExitCode = exitCode, Stdout = stdout, Stderr = stderr };
        c.SessionLines = Lines(stderr).Where(l => l.StartsWith("session: ", StringComparison.Ordinal)).Select(l => l.TrimEnd()).ToList();
        string? icount = null, ports = null, realtime = null;
        foreach (var raw in Lines(stdout))
        {
            var l = raw.TrimEnd('\r');
            if (l.StartsWith("manifest: ", StringComparison.Ordinal)) { c.ManifestJson = l["manifest: ".Length..]; break; }   // the blob after it is free text
            if (!c.HasLaunchLine && l.StartsWith("launch: ", StringComparison.Ordinal))
            {
                c.HasLaunchLine = true;
                foreach (var kv in l["launch: ".Length..].Split(' ', StringSplitOptions.RemoveEmptyEntries))
                {
                    int eq = kv.IndexOf('=');
                    if (eq > 0) c.Launch[kv[..eq]] = kv[(eq + 1)..];
                }
            }
            else if (l.StartsWith("icount: ", StringComparison.Ordinal)) icount ??= l["icount: ".Length..].Trim();
            else if (l.StartsWith("ports: ", StringComparison.Ordinal)) ports ??= l["ports: ".Length..].Trim();
            else if (l.StartsWith("realtime: ", StringComparison.Ordinal)) realtime ??= l["realtime: ".Length..].Trim();
        }
        if (c.HasLaunchLine)
        {
            if (c.Launch.TryGetValue("icount_shift", out var s)) c.IcountShift = int.TryParse(s, out var n) ? n : null;
            c.Realtime = c.Launch.TryGetValue("realtime", out var r) && r == "on";
            c.Control = c.Launch.TryGetValue("control", out var ct) && int.TryParse(ct, out var cn) ? cn : 0;
            c.Msp = c.Launch.TryGetValue("msp", out var ms) && int.TryParse(ms, out var mn) ? mn : 0;
        }
        else
        {
            // older report: "icount: shift=2,sleep=off" | "icount: none"; "ports: control=31250 msp=31235"
            if (icount != null && icount.StartsWith("shift=", StringComparison.Ordinal) && int.TryParse(icount[6..].Split(',')[0], out var sh)) c.IcountShift = sh;
            c.Realtime = realtime == "on";
            foreach (var kv in (ports ?? "").Split(' ', StringSplitOptions.RemoveEmptyEntries))
            {
                if (kv.StartsWith("control=") && int.TryParse(kv[8..], out var cp)) c.Control = cp;
                if (kv.StartsWith("msp=") && int.TryParse(kv[4..], out var mp)) c.Msp = mp;
            }
        }
        return c;
    }
}

/// A core this app started. Output lines (stdout and stderr, both read asynchronously) go into a bounded
/// buffer; readers keep a cursor. Every started process is registered so app exit can stop them all.
public sealed class CoreProcess
{
    private static readonly List<CoreProcess> Started = new();
    /// Processes started since the app began (self-tests: "nothing spawned").
    public static int SpawnCount { get; private set; }

    private readonly Process _p;
    private readonly object _lock = new();
    private readonly List<string> _lines = new();
    private long _total;                          // lines ever added (the cursor space)
    private const int MaxLines = 4000;

    public int Pid { get; }
    public string Exe { get; }
    public string CommandLine { get; }
    public CorePorts Ports { get; }
    /// What it runs (bench file + ports): the flight screen reuses a kept core only for the same key.
    public string Key { get; }
    public bool StopRequested { get; private set; }

    private CoreProcess(Process p, string exe, string cmd, CorePorts ports, string key)
    {
        _p = p; Exe = exe; CommandLine = cmd; Ports = ports; Key = key; Pid = p.Id;
    }

    public bool HasExited { get { try { return _p.HasExited; } catch (Exception) { return true; } } }
    public int? ExitCode { get { try { return _p.HasExited ? _p.ExitCode : null; } catch (Exception) { return null; } } }

    public static CoreProcess Start(string exe, IReadOnlyList<string> args, string workDir, CorePorts ports, string key)
    {
        var psi = new ProcessStartInfo(exe)
        {
            WorkingDirectory = workDir,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,          // never the app's console
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = new UTF8Encoding(false),
            StandardErrorEncoding = new UTF8Encoding(false),
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
        var cmd = exe + " " + string.Join(" ", args.Select(a => a.Contains(' ') ? "\"" + a + "\"" : a));
        p.Start();
        WinJob.Assign(p);
        var cp = new CoreProcess(p, exe, cmd, ports, key);
        // both pipes read asynchronously, line by line (no deadlock); no callback runs before BeginXReadLine
        p.OutputDataReceived += (_, e) => { if (e.Data != null) cp.Add(e.Data); };
        p.ErrorDataReceived += (_, e) => { if (e.Data != null) cp.Add(e.Data); };
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        lock (Started) { Started.Add(cp); SpawnCount++; }
        return cp;
    }

    private void Add(string line)
    {
        lock (_lock)
        {
            _lines.Add(line);
            _total++;
            if (_lines.Count > MaxLines) _lines.RemoveRange(0, _lines.Count - MaxLines);
        }
    }

    /// Lines after `cursor` (moved to the end); lines that fell out of the buffer are skipped. A cursor past the
    /// end (one kept from another process) reads from the start of this one's buffer.
    public List<string> ReadNew(ref long cursor)
    {
        lock (_lock)
        {
            long first = _total - _lines.Count;
            if (cursor > _total) cursor = 0;
            if (cursor < first) cursor = first;
            var r = _lines.GetRange((int)(cursor - first), (int)(_total - cursor));
            cursor = _total;
            return r;
        }
    }

    public List<string> Tail(int n)
    {
        lock (_lock) return _lines.Skip(Math.Max(0, _lines.Count - n)).ToList();
    }

    /// Ends the process (TerminateProcess on Windows, SIGTERM elsewhere, SIGKILL after 3 s) and waits up to
    /// waitMs for it to go. True when it is gone. Blocking: call from a background task, or at app exit.
    public bool StopNow(int waitMs)
    {
        StopRequested = true;
        try
        {
            if (_p.HasExited) return true;
            if (OperatingSystem.IsWindows()) _p.Kill();
            else
            {
                Posix.Term(Pid);
                if (!_p.WaitForExit(Math.Min(waitMs, 3000))) _p.Kill();
            }
            return _p.WaitForExit(waitMs);
        }
        catch (InvalidOperationException) { return true; }       // already gone
        catch (Exception e) { GD.PushError($"core pid {Pid}: stop failed: {e.Message}"); return HasExited; }
    }

    public Task<bool> StopAsync(int waitMs = 5000) => Task.Run(() => StopNow(waitMs));

    /// After the process exited: waits (bounded) until both redirected pipes reached EOF, so every line is in.
    public async Task OutputDoneAsync(int timeoutMs)
    {
        using var cts = new CancellationTokenSource(timeoutMs);
        try { await _p.WaitForExitAsync(cts.Token); } catch (Exception) { }
    }

    /// Is a process with this pid still there (after a stop: the self-test's "process gone")?
    public bool GoneFromOs()
    {
        try { using var p = Process.GetProcessById(Pid); return p.HasExited; }
        catch (ArgumentException) { return true; }
        catch (InvalidOperationException) { return true; }
    }

    /// App exit: every core this app started and did not hand over (keep-running applies to closing a
    /// bench, not to leaving the app) is stopped; bounded wait.
    public static void StopAll(int waitMs = 3000)
    {
        List<CoreProcess> list;
        lock (Started) list = Started.ToList();
        foreach (var c in list)
            if (!c.HasExited) { GD.Print($"[core] app exit: stopping core pid {c.Pid}"); c.StopNow(waitMs); }
    }

    private static class Posix
    {
        [DllImport("libc", SetLastError = true)] private static extern int kill(int pid, int sig);
        public static void Term(int pid) { try { kill(pid, 15); } catch (Exception) { } }
    }
}

/// Windows: every started core goes into one job object with KILL_ON_JOB_CLOSE, so the cores die with the
/// app even when it is killed (no qemu-system-arm.exe left behind). Other platforms: nothing.
internal static class WinJob
{
    private static IntPtr _job;
    private static bool _tried;

    public static void Assign(Process p)
    {
        if (!OperatingSystem.IsWindows()) return;
        try
        {
            if (!_tried)
            {
                _tried = true;
                var job = CreateJobObject(IntPtr.Zero, null);
                var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
                info.BasicLimitInformation.LimitFlags = 0x2000;          // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                if (job != IntPtr.Zero && SetInformationJobObject(job, 9, ref info, Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>())) _job = job;
                else GD.Print("[core] no job object: a core outlives an app that is killed");
            }
            if (_job != IntPtr.Zero && !AssignProcessToJobObject(_job, p.Handle)) GD.Print($"[core] pid {p.Id} not in the job object (error {Marshal.GetLastWin32Error()})");
        }
        catch (Exception e) { GD.Print("[core] job object: " + e.Message); }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IO_COUNTERS { public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes; }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr CreateJobObject(IntPtr attrs, string? name);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, int length);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
}

public enum LaunchOutcome { Running, Started, Rejected, PortBusy, Failed }

/// What to launch: the bench file (a core session JSON), the data root, the listen host, the ports
/// (null = the bench's, from the check's launch line).
public sealed class LaunchRequest
{
    public string Exe = "";
    public string Bench = "";
    public string Root = "";
    public string Host = "127.0.0.1";
    public CorePorts? Ports;
    public bool Prof;
    public int StartTimeoutMs = 20000;
    /// The bench's config state file (`bench-state=`, BenchState.PathFor); null = none. Passed only when the core has the option.
    public string? BenchState;
}

/// One launch on a background task: check → port probe → spawn → wait for the control port. The main
/// thread polls Outcome (Running until done) and drains Process's lines meanwhile.
public sealed class CoreLaunch
{
    public readonly LaunchRequest Req;
    public volatile CoreProcess? Process;
    public CoreCheck? Check { get; private set; }
    public CorePorts Ports { get; private set; }
    public string Message { get; private set; } = "";
    /// PortBusy: the port that is taken, and whether a core already answers on the control port.
    public int BusyPort { get; private set; }
    public bool BusyAnswers { get; private set; }
    public volatile string Stage = "checking the bench";
    /// The `bench-state=` file the core was started with (null: none asked, or the core has no such option).
    public string? BenchStateUsed { get; private set; }
    /// Why a requested bench state was not passed (an older core), else "".
    public string BenchStateNote { get; private set; } = "";
    public readonly Task<LaunchOutcome> Task;
    public LaunchOutcome Outcome => Task.IsCompleted ? Task.Result : LaunchOutcome.Running;

    private CoreLaunch(LaunchRequest r, CoreProcess? stopFirst)
    {
        Req = r;
        Task = System.Threading.Tasks.Task.Run(() => RunAsync(stopFirst));
    }

    /// Starts the launch; `stopFirst` (the previous core of this app) is stopped before anything else.
    public static CoreLaunch Run(LaunchRequest r, CoreProcess? stopFirst = null) => new(r, stopFirst);

    private async Task<LaunchOutcome> RunAsync(CoreProcess? stopFirst)
    {
        try
        {
            if (stopFirst != null && !stopFirst.HasExited) { Stage = "stopping the previous core"; stopFirst.StopNow(5000); }
            Stage = "checking the bench";
            Check = await CoreLauncher.CheckAsync(Req.Exe, Req.Bench, Req.Root);
            if (Check.Rejected) { Message = string.Join("\n", Check.SessionLines.Count > 0 ? Check.SessionLines : new List<string> { Check.Stderr.Trim() }); return LaunchOutcome.Rejected; }
            if (!Check.Ok) { Message = "bench check failed: " + Check.Summary; return LaunchOutcome.Failed; }
            Ports = Req.Ports ?? CorePorts.From(Check.Control, Check.Msp);
            if (!Ports.Valid) { Message = "invalid ports: " + Ports; return LaunchOutcome.Failed; }
            Stage = "probing the ports";
            if (await CoreLauncher.AnswersAsync(Ports.Control, 500))
            {
                // something accepts on the control port: a FlightBench core only if it greets with `hello`
                BusyPort = Ports.Control; BusyAnswers = await CoreLauncher.HelloAsync(Ports.Control, 1500);
                Message = CoreLauncher.BusyText(BusyPort, BusyAnswers, other: !BusyAnswers);
                return LaunchOutcome.PortBusy;
            }
            foreach (var p in Ports.All)
                if (!CoreLauncher.CanBind(p)) { BusyPort = p; Message = CoreLauncher.BusyText(p, false); return LaunchOutcome.PortBusy; }
            Stage = "starting the core";
            if (Req.BenchState is { } bs)
            {
                if (CoreLauncher.HasOption(Req.Exe, Check.Machine, "bench-state"))
                {
                    try { Directory.CreateDirectory(Path.GetDirectoryName(bs)!); } catch (Exception) { }
                    BenchStateUsed = bs;
                }
                else BenchStateNote = $"this core has no bench-state option (-M {Check.Machine},help): the firmware config is not kept per bench (the first-start snapshot is restored instead)";
            }
            var args = CoreLauncher.StartArgs(Req.Bench, Req.Root, Req.Host, Ports, Check.IcountShift, Req.Prof, Check.Machine, BenchStateUsed);
            var proc = Process = CoreProcess.Start(Req.Exe, args, Req.Root, Ports, CoreLauncher.KeyOf(Req.Bench, Ports));
            Stage = $"waiting for the control port {Ports.Control}";
            var sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < Req.StartTimeoutMs)
            {
                if (proc.HasExited)
                {
                    await proc.OutputDoneAsync(2000);                    // the last lines of both pipes (EOF), not a fixed sleep
                    var tail = proc.Tail(20);
                    var busy = CoreLauncher.BusyPortIn(tail, Ports);
                    if (busy > 0) { BusyPort = busy; Message = CoreLauncher.BusyText(busy, false) + "\n" + string.Join("\n", tail.Where(CoreLauncher.IsBindError)); return LaunchOutcome.PortBusy; }
                    Message = $"core exited at start (code {proc.ExitCode})\n" + string.Join("\n", tail);
                    return LaunchOutcome.Failed;
                }
                if (await CoreLauncher.AnswersAsync(Ports.Control, 300)) return LaunchOutcome.Started;
                await System.Threading.Tasks.Task.Delay(200);
            }
            var last = proc.Tail(20);
            proc.StopNow(3000);
            Message = $"the core did not accept on control port {Ports.Control} within {Req.StartTimeoutMs / 1000} s (stopped it); last lines:\n" + string.Join("\n", last);
            return LaunchOutcome.Failed;
        }
        catch (Exception e)
        {
            Process?.StopNow(3000);
            Message = $"launch failed: {e.GetType().Name}: {e.Message}";
            return LaunchOutcome.Failed;
        }
    }
}

public static class CoreLauncher
{
    /// A path in a `-M` value: forward slashes, a comma doubled (QEMU splits -M at commas).
    public static string MVal(string path) => path.Replace('\\', '/').Replace(",", ",,");

    public static List<string> CheckArgs(string bench, string root) =>
        new() { "-M", $"nexus-xr,session={MVal(bench)},root={MVal(root)},session-check=on", "-display", "none" };

    /// The contract's command line (docs/protocol.md "Launching the core").
    /// `machine` = the check's launch line `machine=` (the bench's board); the check itself runs on nexus-xr (any machine resolves any bench).
    /// `benchState`: the bench's config state file (`bench-state=`, core branch bench-state), null = left out.
    public static List<string> StartArgs(string bench, string root, string host, CorePorts p, int? icountShift, bool prof, string machine = "nexus-xr", string? benchState = null)
    {
        var a = new List<string> { "-M", $"{machine},session={MVal(bench)},root={MVal(root)},ctl={host}:{p.Control}{(benchState != null ? $",bench-state={MVal(benchState)}" : "")}{(prof ? ",prof=on" : "")}", "-nographic" };
        if (icountShift is { } s) { a.Add("-icount"); a.Add($"shift={s},sleep=off"); }
        a.AddRange(new[]
        {
            "-chardev", $"socket,id=u1,host={host},port={p.Msp},server=on,wait=off", "-serial", "chardev:u1",
            "-monitor", $"tcp:127.0.0.1:{p.Monitor},server=on,wait=off",
            "-chardev", $"socket,id=fbmotion,host=127.0.0.1,port={p.Motion},server=on,wait=off",
            "-chardev", $"socket,id=fbpilot,host=127.0.0.1,port={p.Pilot},server=on,wait=off",
        });
        return a;
    }

    private static readonly Dictionary<string, string> MachineLists = new();
    private static readonly Dictionary<string, string> MachineOptions = new();

    /// Does this core's machine take the `-M` option `name` (`-M <machine>,help`, cached per exe + machine)?
    public static bool HasOption(string exe, string machine, string name)
    {
        string list;
        lock (MachineOptions)
        {
            var key = exe + "|" + machine;
            if (!MachineOptions.TryGetValue(key, out list!))
            {
                list = "";
                try
                {
                    var psi = new ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
                    psi.ArgumentList.Add("-M"); psi.ArgumentList.Add(machine + ",help");
                    using var pr = Process.Start(psi)!;
                    var err = pr.StandardError.ReadToEndAsync();
                    list = pr.StandardOutput.ReadToEnd();
                    if (!pr.WaitForExit(15000)) { pr.Kill(); list = ""; }
                }
                catch (Exception) { list = ""; }
                MachineOptions[key] = list;
            }
        }
        return list.Replace("\r", "").Split('\n').Any(l => l.TrimStart().StartsWith(name + "=", StringComparison.Ordinal));
    }

    /// Does this core executable have the QEMU machine `machine` (`-M help`, cached per exe)?
    public static bool HasMachine(string exe, string machine)
    {
        lock (MachineLists)
        {
            if (!MachineLists.TryGetValue(exe, out var list))
            {
                list = "";
                try
                {
                    var psi = new ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
                    psi.ArgumentList.Add("-M"); psi.ArgumentList.Add("help");
                    using var pr = Process.Start(psi)!;
                    var err = pr.StandardError.ReadToEndAsync();
                    list = pr.StandardOutput.ReadToEnd();
                    if (!pr.WaitForExit(15000)) { pr.Kill(); list = ""; }
                }
                catch (Exception) { list = ""; }
                MachineLists[exe] = list;
            }
            return list.Replace("\r", "").Split('\n').Any(l => l.Split(' ', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() == machine);
        }
    }

    public static string KeyOf(string bench, CorePorts p)
    {
        string text = "";
        try { text = File.ReadAllText(bench); } catch (Exception) { }
        return $"{Path.GetFullPath(bench)}|{p}|{text.GetHashCode()}";
    }

    /// Runs `-M nexus-xr,session=<bench>,root=<root>,session-check=on -display none` (no sockets) and parses it.
    public static Task<CoreCheck> CheckAsync(string exe, string bench, string root, int timeoutMs = 15000) =>
        RunCaptureAsync(exe, CheckArgs(bench, root), root, timeoutMs);

    /// The core's manifest without a bench (`session-check=on` alone prints only `manifest: ...`).
    public static Task<CoreCheck> ManifestAsync(string exe, string workDir, int timeoutMs = 15000) =>
        RunCaptureAsync(exe, new List<string> { "-M", "nexus-xr,session-check=on", "-display", "none" }, workDir, timeoutMs);

    private static async Task<CoreCheck> RunCaptureAsync(string exe, List<string> args, string workDir, int timeoutMs)
    {
        if (!File.Exists(exe)) return new CoreCheck { RunError = "core not found: " + exe };
        var psi = new ProcessStartInfo(exe)
        {
            WorkingDirectory = Directory.Exists(workDir) ? workDir : Path.GetDirectoryName(exe) ?? "",
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = new UTF8Encoding(false), StandardErrorEncoding = new UTF8Encoding(false),
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        using var p = new Process { StartInfo = psi };
        try { p.Start(); }
        catch (Exception e) { return new CoreCheck { RunError = $"cannot run {exe}: {e.Message}" }; }
        WinJob.Assign(p);
        var so = p.StandardOutput.ReadToEndAsync();        // both pipes at once: no deadlock on a full pipe
        var se = p.StandardError.ReadToEndAsync();
        using var cts = new CancellationTokenSource(timeoutMs);
        try { await p.WaitForExitAsync(cts.Token); }
        catch (OperationCanceledException)
        {
            try { p.Kill(); } catch (Exception) { }
            return new CoreCheck { RunError = $"session-check did not finish within {timeoutMs / 1000} s (killed)" };
        }
        var c = CoreCheck.Parse(p.ExitCode, await so, await se);
        return c;
    }

    /// Does something accept a TCP connection on 127.0.0.1:port within timeoutMs?
    public static async Task<bool> AnswersAsync(int port, int timeoutMs)
    {
        using var c = new TcpClient();
        try
        {
            using var cts = new CancellationTokenSource(timeoutMs);
            await c.ConnectAsync(IPAddress.Loopback, port, cts.Token);
            return c.Connected;
        }
        catch (Exception) { return false; }
    }

    /// Can this port be bound on 127.0.0.1 now (exclusively)?
    public static bool CanBind(int port)
    {
        try
        {
            var l = new TcpListener(IPAddress.Loopback, port) { ExclusiveAddressUse = true };
            l.Start();
            l.Stop();
            return true;
        }
        catch (SocketException) { return false; }
    }

    /// The first offset 1..200 at which all five ports can be bound and nothing answers on control.
    public static async Task<CorePorts?> NextFreeAsync(CorePorts from)
    {
        for (int n = 1; n <= 200; n++)
        {
            var p = from.Offset(n);
            if (!p.Valid || p.All.Any(x => x > 65535)) break;
            if (p.All.All(CanBind) && !await AnswersAsync(p.Control, 200)) return p;
        }
        return null;
    }

    /// Does a FlightBench core answer on 127.0.0.1:port: it sends a `hello` message (docs/protocol.md) among its
    /// first JSON lines (the core broadcasts a `log` "client ... connected" before it)? A listener that sends
    /// nothing, or a line that is not a JSON message with a string `type`, within timeoutMs is not one.
    public static async Task<bool> HelloAsync(int port, int timeoutMs)
    {
        using var c = new TcpClient();
        using var cts = new CancellationTokenSource(timeoutMs);
        try
        {
            await c.ConnectAsync(IPAddress.Loopback, port, cts.Token);
            using var r = new StreamReader(c.GetStream(), new UTF8Encoding(false));
            for (int i = 0; i < 16; i++)
            {
                var line = await r.ReadLineAsync(cts.Token);
                if (line == null) return false;
                using var doc = JsonDocument.Parse(line);
                if (doc.RootElement.ValueKind != JsonValueKind.Object || !doc.RootElement.TryGetProperty("type", out var t) || t.ValueKind != JsonValueKind.String) return false;
                if (t.GetString() == "hello") return true;
            }
            return false;
        }
        catch (Exception) { return false; }
    }

    public static string BusyText(int port, bool answers, bool other = false) =>
        answers ? $"port {port} is in use — another FlightBench core answers there"
        : other ? $"port {port} is in use — by another program (it does not answer as a FlightBench core)"
                : $"port {port} is in use — another FlightBench core?";

    /// A QEMU bind failure line ("Failed to find an available port", "Address already in use").
    public static bool IsBindError(string l) =>
        l.Contains("available port", StringComparison.OrdinalIgnoreCase) || l.Contains("already in use", StringComparison.OrdinalIgnoreCase)
        || l.Contains("address in use", StringComparison.OrdinalIgnoreCase);

    /// Which of our ports a bind-failure line names (0 = none).
    public static int BusyPortIn(IEnumerable<string> lines, CorePorts p)
    {
        foreach (var l in lines.Where(IsBindError))
            foreach (var port in p.All)
                if (l.Contains($"port={port},") || l.Contains($"port={port}:") || l.Contains($":{port}:") || l.Contains($":{port},") || l.EndsWith($":{port}")) return port;
        return lines.Any(IsBindError) ? p.Control : 0;
    }

    /// The bench as the core's session file, `user://sessions/<name>.session.json` (`<bench dir>/sessions/` when
    /// the benches' folder is not the default) (UTF-8 without BOM,
    /// paths resolved under the data root). The file's absolute path.
    /// `user://sessions/<name>.session.json`, or `<bench dir>/sessions/…` when the benches' folder is moved.
    public static string SessionFileFor(string benchName)
    {
        var dir = BenchData.Dir == "user://benches" ? "user://sessions" : BenchData.Dir + "/sessions";
        return $"{dir}/{BenchData.SafeName(benchName)}.session.json";
    }

    public static string WriteSession(BenchData b, string root, out string error)
    {
        // next to the benches when their folder is moved (FB_BENCH_DIR, self-tests' temp folders): such runs
        // leave nothing in the real user://sessions
        var path = SessionFileFor(b.Name);
        var json = JsonSerializer.Serialize(b.ToSession(root), Json.Files);
        Json.WriteAtomic(path, json, out error);
        return ProjectSettings.GlobalizePath(path);
    }
}
