// FB_SELFTEST corelaunch / corelaunch_reject / corelaunch_portbusy: the app's core launcher (CoreLauncher.cs)
// against the real core FB_CORE_EXE (skipped — PASS "skipped: no core exe" — when it is not set), on the
// alternative ports 31290..31294 so a core running on the defaults is not disturbed.
//   corelaunch: FB_BENCH_FILE checked (report parsed: icount shift 2, ports 31250/31235), started, control
//     port waited for, `hello` + `manifest` received, stopped, the process gone.
//   corelaunch_restart: the flight screen's Restart bench path: FB_BENCH_FILE started on the alternative ports,
//     its output drained with a cursor, then CoreLaunch.Run(stopFirst: that core) - stop, check, start on the
//     same ports (a CLI-configured bench restores its config snapshot from cfg-cache) -> Started (not "port in
//     use"), the first core gone, the old cursor read on the new core without an exception (it reads from the
//     new core's first line), `hello` on the new core; stopped.
//   corelaunch_reject: a temp bench with a WiringError (receiver on the RX-only TELEM pad) → exit 2, the
//     error text captured, nothing spawned.
//   corelaunch_portbusy: a TCP listener on the control port first → "port 31290 is in use", not taken for a
//     core (no hello), nothing spawned; QEMU's own bind-failure lines map to the port they name; the hello
//     probe: a silent listener → false, one greeting with `log` + `hello` → true.
// Polled from SelfTestRunner._Process (nothing blocks the main thread); Done when all three reported.
using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Threading.Tasks;
using Godot;

namespace FlightBench;

public sealed class CoreLaunchTests
{
    public static readonly CorePorts Alt = DevEnv.CorePorts ?? new(31290, 31291, 31292, 31293, 31294);   // FB_CORE_PORTS moves them (e.g. 31295)
    private static readonly string[] Names = { "corelaunch", "corelaunch_restart", "corelaunch_reject", "corelaunch_portbusy" };
    private enum St { Start, Check, Launch, Hello, Stop, Restart, RLaunch1, RHello1, RLaunch2, RHello2, RStop, Reject, RejectWait, Busy, BusyWait, BusyHello, Done }
    private St _st = St.Start;
    private ulong _deadline;
    private string _exe = "";
    private string? _bench;
    private Task<CoreCheck>? _check;
    private bool _machineBad;                 // the started core's -M was not the check's machine (nexus-xr for the RS7 bench)
    private CoreLaunch? _launch;
    private CoreClient? _client;
    private bool _hello, _manifest;
    private Task<bool>? _stop;
    private int _spawnBefore;
    private string _tmpDir = "";
    private TcpListener? _listener;
    private string _detail = "";
    private Task<bool>? _helloProbe, _silentProbe, _coreHello;
    private bool _busyOk;
    private CoreProcess? _first;              // corelaunch_restart: the core that the restart replaces
    private long _cursor;                     // ... the app's cursor into its output (kept across the restart)

    public bool Done => _st == St.Done;

    private static bool Want(string n) => DevEnv.SelfTest(n);

    public void Poll()
    {
        switch (_st)
        {
            case St.Start:
            {
                if (!Names.Any(Want)) { _st = St.Done; return; }
                if (DevEnv.CoreExe is not { } exe)
                {
                    foreach (var n in Names.Where(Want)) SelfTests.Report(n, true, "skipped: no core exe (FB_CORE_EXE not set)");
                    _st = St.Done; return;
                }
                if (!File.Exists(exe))
                {
                    foreach (var n in Names.Where(Want)) SelfTests.Report(n, false, $"FB_CORE_EXE={exe} does not exist");
                    _st = St.Done; return;
                }
                _exe = Path.GetFullPath(exe);
                _bench = DevEnv.BenchFile is { } bf && File.Exists(bf) ? Path.GetFullPath(bf) : null;
                GD.Print($"[selftest] corelaunch: core {_exe}, bench {_bench ?? "(FB_BENCH_FILE not set)"}, data root {CoreLocator.DataRoot}, ports {Alt}");
                if (!Want("corelaunch")) { _st = St.Restart; return; }
                if (_bench == null) { SelfTests.Report("corelaunch", false, $"FB_BENCH_FILE not set or missing ({DevEnv.BenchFile})"); _st = St.Restart; return; }
                _check = CoreLauncher.CheckAsync(_exe, _bench, CoreLocator.DataRoot);
                _deadline = Time.GetTicksMsec() + 90000;
                _st = St.Check;
                break;
            }
            case St.Check:
            {
                if (Late("check")) return;
                if (_check is not { IsCompleted: true }) return;
                var c = _check.Result;
                if (!(c.Ok && c.HasLaunchLine && c.IcountShift == 2 && c.Control == 31250 && c.Msp == 31235))
                {
                    Fail($"check: {c.Summary}; launch line {c.HasLaunchLine}, shift {c.IcountShift}, ports {c.Control}/{c.Msp} (want 2, 31250/31235)");
                    return;
                }
                // the machine to start: the launch line's machine= (the RS7 bench: nexus-xr), nexus-xr when absent, unknown keys ignored
                var pm = CoreCheck.Parse(0, "launch: icount_shift=2 control=1 msp=2 future=x machine=speedybee-f405-wing", "");
                var pn = CoreCheck.Parse(0, "launch: icount_shift=2 control=1 msp=2 future=x", "");
                if (c.Machine != "nexus-xr" || pm.Machine != "speedybee-f405-wing" || !pm.Ok || pn.Machine != "nexus-xr" || !pn.Ok)
                {
                    Fail($"machine: bench {c.Machine} (want nexus-xr), parsed {pm.Machine} / absent {pn.Machine}");
                    return;
                }
                _detail = $"check: launch line shift {c.IcountShift}, realtime {(c.Realtime ? "on" : "off")}, ports {c.Control}/{c.Msp}, machine {c.Machine}, {c.SessionLines.Count} session notes";
                if (!Alt.All.All(CoreLauncher.CanBind)) { Fail($"alternative ports {Alt} not free"); return; }
                _launch = CoreLaunch.Run(new LaunchRequest { Exe = _exe, Bench = _bench!, Root = CoreLocator.DataRoot, Ports = Alt });
                _st = St.Launch;
                break;
            }
            case St.Launch:
                if (Late("launch (" + _launch?.Stage + ")")) return;
                if (_launch!.Outcome == LaunchOutcome.Running) return;
                if (_launch.Outcome != LaunchOutcome.Started) { Fail($"launch: {_launch.Outcome}: {_launch.Message}"); return; }
                _detail += $"; started pid {_launch.Process!.Pid}";
                if (!_launch.Process.CommandLine.Contains("nexus-xr,session=")) { _detail += $"; FAIL: not started as nexus-xr: {_launch.Process.CommandLine}"; _machineBad = true; }
                GD.Print($"[selftest] corelaunch: {_launch.Process.CommandLine}");
                _client = new CoreClient();
                _client.HelloReceived += _ => _hello = true;
                _client.ManifestReceived += _ => _manifest = true;
                _client.Connect("127.0.0.1", Alt.Control);
                _coreHello = CoreLauncher.HelloAsync(Alt.Control, 5000);    // the port-in-use probe sees a real core as one
                _st = St.Hello;
                break;
            case St.Hello:
                _client!.Poll();
                if (Late($"hello {_hello} / manifest {_manifest}")) return;
                if (!(_hello && _manifest) || _coreHello is not { IsCompleted: true }) return;
                if (!_coreHello.Result) { Fail("the hello probe (port-in-use check) did not recognise the running core"); return; }
                _detail += $"; hello + manifest on {Alt.Control}, hello probe true";
                _client.Disconnect(); _client.Dispose(); _client = null;
                _stop = _launch!.Process!.StopAsync();
                _st = St.Stop;
                break;
            case St.Stop:
            {
                if (Late("stop")) return;
                if (_stop is not { IsCompleted: true }) return;
                var p = _launch!.Process!;
                bool gone = _stop.Result && p.HasExited && p.GoneFromOs();
                SelfTests.Report("corelaunch", gone && !_machineBad, _detail + (gone ? $"; stopped, pid {p.Pid} gone" : $"; pid {p.Pid} still there after the stop"));
                _launch = null;
                _st = St.Restart;
                break;
            }
            case St.Restart:
            {
                if (!Want("corelaunch_restart")) { _st = St.Reject; return; }
                if (_bench == null) { SelfTests.Report("corelaunch_restart", false, $"FB_BENCH_FILE not set or missing ({DevEnv.BenchFile})"); _st = St.Reject; return; }
                if (!Alt.All.All(CoreLauncher.CanBind)) { Fail($"alternative ports {Alt} not free"); return; }
                _detail = "";
                _hello = false;
                _cursor = 0;
                _launch = CoreLaunch.Run(new LaunchRequest { Exe = _exe, Bench = _bench, Root = CoreLocator.DataRoot, Ports = Alt });
                _deadline = Time.GetTicksMsec() + 90000;
                _st = St.RLaunch1;
                break;
            }
            case St.RLaunch1:
                if (Late("first launch (" + _launch?.Stage + ")")) return;
                if (_launch!.Outcome == LaunchOutcome.Running) return;
                if (_launch.Outcome != LaunchOutcome.Started) { Fail($"first launch: {_launch.Outcome}: {_launch.Message}"); return; }
                _first = _launch.Process!;
                _client = new CoreClient();
                _client.HelloReceived += _ => _hello = true;
                _client.Connect("127.0.0.1", Alt.Control);
                _st = St.RHello1;
                break;
            case St.RHello1:
            {
                _client!.Poll();
                if (Late("hello from the first core")) return;
                if (!_hello) return;
                _first!.ReadNew(ref _cursor);                     // what the Log window drained of the first core
                _client.Disconnect(); _client.Dispose(); _client = null;
                _hello = false;
                // Restart bench: the same request with the running core stopped first (Flight.StartCore -> CoreLaunch.Run(.., prev))
                _launch = CoreLaunch.Run(new LaunchRequest { Exe = _exe, Bench = _bench!, Root = CoreLocator.DataRoot, Ports = Alt }, _first);
                _detail = $"first core pid {_first.Pid}: hello, {_cursor} output lines drained";
                _st = St.RLaunch2;
                break;
            }
            case St.RLaunch2:
            {
                if (Late("restart (" + _launch?.Stage + ")")) return;
                if (_launch!.Outcome == LaunchOutcome.Running) return;
                if (_launch.Outcome != LaunchOutcome.Started) { Fail($"{_detail}; restart on the same ports: {_launch.Outcome}: {_launch.Message}"); return; }
                var p = _launch.Process!;
                bool firstGone = _first!.HasExited && _first.GoneFromOs();
                // the first core's cursor on the new one (past its end): no exception, reads from the new core's first line
                string cur;
                try
                {
                    long stale = Math.Max(_cursor, 1_000_000), fresh = 0;
                    var a = p.ReadNew(ref stale);
                    var b = p.ReadNew(ref fresh);
                    cur = a.Count > 0 && b.Count >= a.Count && a[0] == b[0] ? "ok" : $"stale cursor read {a.Count} lines (fresh {b.Count})";
                }
                catch (Exception e) { cur = "stale cursor threw " + e.GetType().Name; }
                if (!firstGone || cur != "ok") { Fail($"{_detail}; restarted pid {p.Pid}; first core gone: {firstGone}; {cur}"); return; }
                _detail += $"; restarted on the same ports ({Alt.Control}): pid {p.Pid}, first core gone, stale cursor read ok";
                _client = new CoreClient();
                _client.HelloReceived += _ => _hello = true;
                _client.Connect("127.0.0.1", Alt.Control);
                _st = St.RHello2;
                break;
            }
            case St.RHello2:
            {
                _client!.Poll();
                if (Late("hello from the restarted core")) return;
                if (!_hello) return;
                var cfg = _launch!.Process!.Tail(4000).FirstOrDefault(l => l.Contains("event config"));
                _detail += "; hello on the restarted core" + (cfg != null ? "; " + cfg[cfg.IndexOf("event config", StringComparison.Ordinal)..] : "; no config event (not a CLI-configured bench)");
                _client.Disconnect(); _client.Dispose(); _client = null;
                _stop = _launch.Process.StopAsync();
                _st = St.RStop;
                break;
            }
            case St.RStop:
            {
                if (Late("stop")) return;
                if (_stop is not { IsCompleted: true }) return;
                var p = _launch!.Process!;
                bool gone = _stop.Result && p.HasExited && p.GoneFromOs();
                SelfTests.Report("corelaunch_restart", gone, _detail + (gone ? $"; stopped, pid {p.Pid} gone" : $"; pid {p.Pid} still there after the stop"));
                _launch = null; _first = null;
                _st = St.Reject;
                break;
            }
            case St.Reject:
            {
                if (!Want("corelaunch_reject")) { _st = St.Busy; return; }
                // a bench the core rejects: the receiver on uart2 (TELEM, RX only)
                _tmpDir = $"user://selftest-core-{Time.GetTicksUsec()}";
                var path = _tmpDir + "/reject.session.json";
                var text = "{\"board\":\"nexus-xr\",\"firmware\":\"none.elf\",\"board_config\":\"boards/nexus-xr/NEXUS_XR.config\",\"aircraft\":\"models/rs7/aircraft.json\",\n"
                         + " \"wiring\":[{\"device\":\"crsf\",\"port\":\"uart\",\"pad\":\"uart2\"}],\"ports\":{\"control\":" + Alt.Control + ",\"msp\":" + Alt.Msp + "}}\n";
                if (!Json.WriteAtomic(path, text, out var err)) { SelfTests.Report("corelaunch_reject", false, "cannot write the temp bench: " + err); _st = St.Busy; return; }
                _spawnBefore = CoreProcess.SpawnCount;
                _launch = CoreLaunch.Run(new LaunchRequest { Exe = _exe, Bench = ProjectSettings.GlobalizePath(path), Root = CoreLocator.DataRoot, Ports = Alt });
                _deadline = Time.GetTicksMsec() + 30000;
                _st = St.RejectWait;
                break;
            }
            case St.RejectWait:
            {
                if (Late("reject")) return;
                if (_launch!.Outcome == LaunchOutcome.Running) return;
                bool ok = _launch.Outcome == LaunchOutcome.Rejected && _launch.Check?.ExitCode == 2 && _launch.Process == null && CoreProcess.SpawnCount == _spawnBefore
                          && _launch.Message.StartsWith("session: ") && (_launch.Message.Contains("uart2") || _launch.Message.Contains("TELEM"));
                SelfTests.Report("corelaunch_reject", ok, $"{_launch.Outcome}, exit {_launch.Check?.ExitCode}, spawned {CoreProcess.SpawnCount - _spawnBefore}, error: {_launch.Message.Replace("\n", " | ")}");
                _launch = null;
                _st = St.Busy;
                break;
            }
            case St.Busy:
            {
                if (!Want("corelaunch_portbusy")) { Finish(); return; }
                if (_bench == null) { SelfTests.Report("corelaunch_portbusy", false, "FB_BENCH_FILE not set or missing"); Finish(); return; }
                _listener = new TcpListener(IPAddress.Loopback, Alt.Control);
                try { _listener.Start(); }
                catch (SocketException e) { SelfTests.Report("corelaunch_portbusy", false, $"cannot bind {Alt.Control} for the test: {e.Message}"); _listener = null; Finish(); return; }
                _spawnBefore = CoreProcess.SpawnCount;
                _launch = CoreLaunch.Run(new LaunchRequest { Exe = _exe, Bench = _bench, Root = CoreLocator.DataRoot, Ports = Alt });
                _deadline = Time.GetTicksMsec() + 30000;
                _st = St.BusyWait;
                break;
            }
            case St.BusyWait:
            {
                if (Late("portbusy")) return;
                if (_launch!.Outcome == LaunchOutcome.Running) return;
                // QEMU's own bind failures (the core exiting at once) name the port
                string ctlLine = $"qemu-system-arm.exe: nexus-xr: control server on 127.0.0.1:{Alt.Control}: Failed to find an available port: Unknown error";
                string mspLine = $"qemu-system-arm.exe: -chardev socket,id=u1,host=127.0.0.1,port={Alt.Msp},server=on,wait=off: Failed to find an available port: Unknown error";
                int p1 = CoreLauncher.BusyPortIn(new[] { ctlLine }, Alt), p2 = CoreLauncher.BusyPortIn(new[] { mspLine }, Alt), p3 = CoreLauncher.BusyPortIn(new[] { "session: fine" }, Alt);
                // a bare listener is not a FlightBench core (no `hello`): "in use", but no "Connect to it"
                _busyOk = _launch.Outcome == LaunchOutcome.PortBusy && _launch.BusyPort == Alt.Control && _launch.Message.Contains($"port {Alt.Control} is in use")
                          && !_launch.BusyAnswers && _launch.Process == null && CoreProcess.SpawnCount == _spawnBefore && p1 == Alt.Control && p2 == Alt.Msp && p3 == 0;
                _detail = $"{_launch.Outcome}: \"{_launch.Message}\" (a core answers: {_launch.BusyAnswers}), spawned {CoreProcess.SpawnCount - _spawnBefore}; QEMU bind lines -> {p1}/{p2}, other line -> {p3}";
                _launch = null;
                // the hello probe itself: a listener that greets with a `hello` line is a core, a silent one is not
                var l = _listener!;
                l.Stop(); l.Start();                                      // a fresh backlog (the launch's probes sit in the old one)
                _silentProbe = CoreLauncher.HelloAsync(Alt.Control, 800);
                _helloProbe = Task.Run(async () =>
                {
                    await _silentProbe;                                   // nobody accepted / wrote: must be false
                    var probe = CoreLauncher.HelloAsync(Alt.Control, 3000);
                    var line = System.Text.Encoding.UTF8.GetBytes("{\"type\":\"log\",\"src\":\"core\",\"text\":\"client connected\"}\n{\"type\":\"hello\",\"core\":\"flightbench-qemu\"}\n");
                    var open = new System.Collections.Generic.List<TcpClient>();
                    try
                    {
                        // greet every connection (the silent probe's closed one comes first) until the probe is done
                        while (!probe.IsCompleted)
                        {
                            var acc = l.AcceptTcpClientAsync();
                            if (await Task.WhenAny(acc, probe) == probe) break;
                            var c = await acc;
                            open.Add(c);
                            try { await c.GetStream().WriteAsync(line); } catch (Exception) { }
                        }
                        return await probe;
                    }
                    finally { foreach (var c in open) c.Dispose(); }
                });
                _deadline = Time.GetTicksMsec() + 15000;
                _st = St.BusyHello;
                break;
            }
            case St.BusyHello:
            {
                if (Late("portbusy hello probe")) return;
                if (_helloProbe is not { IsCompleted: true } hp) return;
                bool silent = _silentProbe!.Result, hello = !hp.IsFaulted && hp.Result;
                SelfTests.Report("corelaunch_portbusy", _busyOk && !silent && hello, _detail + $"; hello probe: silent listener -> {silent}, greeting listener -> {(hp.IsFaulted ? hp.Exception!.InnerException!.Message : hello)}");
                Finish();
                break;
            }
        }
    }

    private bool Late(string what)
    {
        if (Time.GetTicksMsec() < _deadline) return false;
        Fail("timed out at " + what);
        return true;
    }

    /// The running test failed: clean up, go on with the next.
    private void Fail(string why)
    {
        var name = _st switch
        {
            St.Restart or St.RLaunch1 or St.RHello1 or St.RLaunch2 or St.RHello2 or St.RStop => "corelaunch_restart",
            St.Reject or St.RejectWait => "corelaunch_reject",
            St.Busy or St.BusyWait or St.BusyHello => "corelaunch_portbusy",
            _ => "corelaunch",
        };
        SelfTests.Report(name, false, why + (_launch?.Process is { } p ? $" (last lines: {string.Join(" | ", p.Tail(8))})" : ""));
        _client?.Dispose(); _client = null;
        _launch?.Process?.StopNow(3000);
        _launch = null;
        if (_first is { HasExited: false } f) f.StopNow(3000);
        _first = null;
        if (name == "corelaunch") _st = St.Restart;
        else if (name == "corelaunch_restart") _st = St.Reject;
        else if (name == "corelaunch_reject") _st = St.Busy;
        else Finish();
    }

    private void Finish()
    {
        try { _listener?.Stop(); } catch (Exception) { }
        _listener = null;
        _launch?.Process?.StopNow(3000);
        _launch = null;
        if (_tmpDir != "") try { Directory.Delete(ProjectSettings.GlobalizePath(_tmpDir), true); } catch (Exception) { }
        _st = St.Done;
    }
}
