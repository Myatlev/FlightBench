#!/usr/bin/env python3
"""Measure host seconds per N virtual seconds of the firmware's idle main loop.

Starts QEMU (nexus-xr) with an HMP monitor on TCP, polls the firmware's
millisecond uptime counter (sysTickUptime, address from `nm`) with `xp`, and
times how long the host needs for the counter to advance by N*1000 ms once
the firmware is past boot (uptime >= --warmup ms).

usage: speed.py --qemu Q --elf E [--icount "shift=2,sleep=off"] [--seconds 10]
                [--extra "-device loader,file=defaults.hex"]
       speed.py --qemu Q --elf E --session S [--machine-opts "idle-skip=on"]
                (the whole bench through run.sh --session S: board config, profile, FDM,
                control server; S must name its own ports; HMP on --port, motion/pilot on
                --port+1/+2; icount from the session)
"""
import argparse, os, re, shlex, socket, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--qemu", required=True)
ap.add_argument("--elf", required=True)
ap.add_argument("--nm", default="arm-none-eabi-nm")
ap.add_argument("--icount", default=None)
ap.add_argument("--seconds", type=float, default=10)
ap.add_argument("--warmup", type=int, default=2000, help="virtual ms before timing starts")
ap.add_argument("--port", type=int, default=31240)
ap.add_argument("--extra", default="")
ap.add_argument("--machine", default="nexus-xr", help="-M value, e.g. nexus-xr,imu=off")
ap.add_argument("--session", help="run.sh --session file (ports inside); --elf only for nm")
ap.add_argument("--machine-opts", default="", help="MACHINE_OPTS for run.sh --session")
a = ap.parse_args()

sym = subprocess.run([a.nm, a.elf], capture_output=True, text=True).stdout
addr = next(int(l.split()[0], 16) for l in sym.splitlines() if l.split()[-1].startswith("sysTickUptime"))

env = None
if a.session:
    cmd = [os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "run.sh"),
           "--session", a.session]
    env = dict(os.environ, QEMU=a.qemu, MON_PORT=str(a.port), MOTION_PORT=str(a.port + 1),
               PILOT_PORT=str(a.port + 2), MACHINE_OPTS=a.machine_opts, BENCH_STATE="")
else:
    cmd = [a.qemu, "-M", a.machine, "-nographic", "-kernel", a.elf, "-serial", "null",
           "-monitor", f"tcp:127.0.0.1:{a.port},server=on,wait=off"]
    if a.icount:
        cmd += ["-icount", a.icount]
cmd += shlex.split(a.extra)
q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
try:
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", a.port)); break
        except OSError:
            time.sleep(0.1)
    s.settimeout(2)
    buf = b""
    def hmp(c):
        global buf
        s.sendall(c.encode() + b"\n")
        while True:
            m = re.search(rb"%08x: (0x[0-9a-f]+)" % addr, buf)
            if m:
                buf = buf[m.end():]
                return int(m.group(1), 16)
            buf += s.recv(4096)
    def uptime():
        return hmp(f"xp /1wx 0x{addr:08x}")
    def bench():
        global buf
        s.sendall(b"qom-get /machine bench\n")
        while True:
            m = re.search(rb'"(t=[^"]*)"', buf)
            if m:
                buf = buf[m.end():]
                return dict(kv.split("=", 1) for kv in m.group(1).decode().split() if "=" in kv)
            buf += s.recv(4096)
    def imu_samples():
        global buf
        s.sendall(b"qom-get /machine imu-samples\n")
        while True:
            m = re.search(rb"\r?\n(\d+)\r?\n", buf)
            if m:
                buf = buf[m.end():]
                return int(m.group(1))
            if b"Error" in buf:
                buf = b""
                return None
            buf += s.recv(4096)
    while uptime() < a.warmup:
        time.sleep(0.02)
    u0, t0 = uptime(), time.monotonic()
    n0 = imu_samples()
    b0 = bench()
    target = u0 + a.seconds * 1000
    while True:
        u, t = uptime(), time.monotonic()
        if u >= target:
            break
        time.sleep(0.01)
    host = t - t0
    virt = (u - u0) / 1000
    n1 = imu_samples()
    b1 = bench()
    skip = ""
    if "idle_skips" in b1:
        dt = float(b1["idle_skipped_s"]) - float(b0["idle_skipped_s"])
        skip = (f"; idle skips {(int(b1['idle_skips']) - int(b0['idle_skips'])) / virt:.0f}/virtual s, "
                f"{100 * dt / virt:.1f} % of virtual time skipped")
    print(f"{'session ' + os.path.basename(a.session) + (' ' + a.machine_opts if a.machine_opts else '') if a.session else 'icount=' + (a.icount or 'off')} virtual {virt:.3f} s -> host {host:.2f} s "
          f"(realtime factor {virt/host:.3f}x; host s per 10 virtual s: {10*host/virt:.2f})"
          + (f"; IMU samples {(n1 - n0) / virt:.0f}/virtual s" if n0 is not None else "") + skip,
          flush=True)
finally:
    q.kill()
    if a.session:   # run.sh execs QEMU, so this is QEMU itself; wait so its ports are free
        q.wait()
