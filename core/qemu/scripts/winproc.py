"""Starting and stopping `run.sh --session` from the evidence scripts, also on Windows (MSYS2).

On Linux run.sh execs QEMU, so the Popen's PID is QEMU's and killing it stops the core. On Windows a
native Python cannot exec a shell script (it runs PATH's bash, the MSYS2 one), and MSYS2's `exec` of a
native program keeps bash as a proxy under another PID: killing that proxy would leave QEMU running.
There the QEMU process is found by a token of its command line (a path unique to the run) and
stopped by its own PID (never by image name: the user's own FlightBench core may be running).
"""
import os, shutil, subprocess, sys, time

WIN = sys.platform == "win32"


def popen_run_sh(args, **kw):
    """subprocess.Popen of run.sh <args> (a bash script) on any platform; no bench state unless the
    caller's env has BENCH_STATE (the evidence starts from the first start or its snapshot)"""
    env = kw.get("env")
    kw["env"] = dict(os.environ if env is None else env)
    kw["env"].setdefault("BENCH_STATE", "")
    if WIN:
        bash = shutil.which("bash") or "bash"   # PATH's bash (MSYS2), not System32's (WSL)
        return subprocess.Popen([bash] + list(args), **kw)
    return subprocess.Popen(list(args), **kw)


def qemu_pids(token):
    """PIDs of qemu-system-arm.exe processes whose command line contains token (Windows only)"""
    if not WIN:
        return []
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='qemu-system-arm.exe'\" | "
          "Where-Object { $_.CommandLine -and $_.CommandLine.Contains($env:FB_TOKEN) } | "
          "ForEach-Object { $_.ProcessId }")
    r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
                       capture_output=True, text=True, env=dict(os.environ, FB_TOKEN=token))
    return [int(x) for x in r.stdout.split() if x.isdigit()]


def stop(p, token):
    """stop the core started by popen_run_sh: QEMU by its PID (Windows: found by token), then the proxy"""
    if WIN:
        for pid in qemu_pids(token):
            subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
        end = time.time() + 10
        while qemu_pids(token) and time.time() < end:
            time.sleep(0.2)
    if p.poll() is None:
        p.kill()
    p.wait()
