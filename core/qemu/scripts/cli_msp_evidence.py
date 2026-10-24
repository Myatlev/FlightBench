#!/usr/bin/env python3
"""CLI through the control server while a host tool talks MSP on the same UART (review2,
2026-09-26). The core's own '#' CLI entry must go in between the host's MSP frames, its own
MSP polls (v2, `$X<`) must never reach the host, and every v1 reply the host gets must be a
valid answer to what it asked.

  cli_msp_evidence.py [control port 31274] [msp port 31270] [rounds 3]

A streamer thread sends MSP v1 requests stop-and-wait (MSP_STATUS / ATTITUDE / RC — the commands
the core polls too — and FC_VARIANT / FC_VERSION), 0.5 host s per reply. It holds off once the
core logs its CLI entry (like a configurator that sees the port go quiet) and resumes when the
firmware is back on MSP. Each round: stream, `cli status`, `cli exit`, stream again.
"""
import os, socket, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
from core_client import Core  # noqa: E402

port = int(sys.argv[1]) if len(sys.argv) > 1 else 31274
msp_port = int(sys.argv[2]) if len(sys.argv) > 2 else 31270
rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 3
T0 = time.time()


def log(msg):
    print(f"[{time.time() - T0:6.1f}s] {msg}", flush=True)


def frame(cmd):
    return bytes([ord('$'), ord('M'), ord('<'), 0, cmd, cmd])


c = Core(port)
c.wait_for(lambda m: m["type"] == "state" and m["fc"]["msp"], 120)
log(f"core up at t={c.t():.2f}, fc.msp={c.state['fc']['msp']}")

counts = {"sent": 0, "ok": 0, "bad": 0, "timeout": 0, "v2_leaked": 0, "held": 0}
bad_examples = []
stop = False
hold = threading.Event()        # set: do not send (CLI in progress)
entry_seen = [len(c.msgs)]


def entering():
    """the core logged its CLI entry since the last check"""
    msgs = c.msgs
    for m in msgs[entry_seen[0]:]:
        if m.get("type") == "log" and m.get("src") == "core" and "entering CLI mode" in m.get("text", ""):
            entry_seen[0] = len(msgs)
            return True
    entry_seen[0] = len(msgs)
    return False


def streamer():
    s = socket.create_connection(("127.0.0.1", msp_port))
    s.settimeout(0.05)
    cmds = [101, 108, 105, 2, 3]
    i = 0
    buf = b""
    while not stop:
        if entering():
            hold.set()
        if hold.is_set():
            counts["held"] += 1
            time.sleep(0.05)
            try:
                buf = b""
                s.recv(65536)           # CLI text for the host socket, not MSP
            except socket.timeout:
                pass
            continue
        cmd = cmds[i % len(cmds)]
        i += 1
        s.sendall(frame(cmd))
        counts["sent"] += 1
        end = time.time() + 0.5
        got = None
        while time.time() < end and got is None:
            try:
                buf += s.recv(4096)
            except socket.timeout:
                pass
            counts["v2_leaked"] += buf.count(b"$X>") + buf.count(b"$X!")
            buf = buf.replace(b"$X>", b"").replace(b"$X!", b"")
            j = buf.find(b"$M")
            if j < 0 or len(buf) < j + 5 or len(buf) < j + 6 + buf[j + 3]:
                continue
            n = buf[j + 3]
            f = buf[j:j + 6 + n]
            buf = buf[j + 6 + n:]
            x = 0
            for b in f[3:-1]:
                x ^= b
            got = f
            if f[2] != ord('>') or x != f[-1] or f[4] != cmd:
                counts["bad"] += 1
                if len(bad_examples) < 5:
                    bad_examples.append((cmd, f.hex()))
            else:
                counts["ok"] += 1
        if got is None:
            counts["timeout"] += 1
            if entering():
                hold.set()
    s.close()


th = threading.Thread(target=streamer)
th.start()
try:
    for r in range(rounds):
        time.sleep(3.0)
        before = dict(counts)
        k = len(c.msgs)
        lines = c.cli("status")
        core = [m["text"] for m in c.msgs[k:] if m["type"] == "log" and m["src"] == "core" and "cli" in m["text"]]
        errs = [m for m in c.msgs[k:] if m["type"] == "error"]
        head = [l for l in lines if l.strip()][:3]
        log(f"round {r + 1}: host MSP before the cli request: {before['ok']} ok, {before['bad']} bad; "
            f"cli 'status' -> {len(lines)} lines, fc.cli={c.state['fc']['cli']}: " + " | ".join(head))
        log(f"round {r + 1}: core cli logs: {' | '.join(core)}; errors: {errs}")
        c.cli("exit", quiet_s=0.5)
        c.wait_for(lambda m: m["type"] == "state" and m["fc"]["msp"] and not m["fc"]["cli"], 120)
        hold.clear()
        log(f"round {r + 1}: back on MSP at t={c.t():.2f}")
    time.sleep(3.0)
finally:
    stop = True
    th.join()
log(f"host MSP (v1, stop-and-wait) over all rounds: {counts['sent']} requests, {counts['ok']} valid replies, "
    f"{counts['bad']} bad (wrong cmd/checksum), {counts['timeout']} timeouts (CLI entry window), "
    f"{counts['v2_leaked']} core v2 replies leaked to the host; bad examples: {bad_examples}")
