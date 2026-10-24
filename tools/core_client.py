#!/usr/bin/env python3
"""Client for the FlightBench core control server (docs/protocol.md), for scripts and evidence.

  core_client.py dump [--port 31250] [--seconds 1.0] [--out file.jsonl]
      every message of `seconds` of virtual time after hello (first/last state printed)
  core_client.py send '<json>' ... [--wait 2.0]
      send messages, print what comes back (no state) for `wait` host seconds

As a module: Core(port) with send(), wait_t(), wait_for(), last state, and collected messages.
"""
import argparse, collections, json, queue, socket, sys, threading, time


class LatestQueue:
    """A bounded message queue: when full, the oldest message is dropped (and counted), so a
    consumer that stops reading costs bounded memory and still sees the newest states."""

    def __init__(self, maxlen=4096):
        self.d = collections.deque()
        self.maxlen = maxlen
        self.dropped = 0
        self.cv = threading.Condition()

    def put(self, m):
        with self.cv:
            if len(self.d) >= self.maxlen:
                self.d.popleft()
                self.dropped += 1
            self.d.append(m)
            self.cv.notify()

    def get(self, timeout):
        with self.cv:
            if not self.d and not self.cv.wait_for(lambda: self.d, timeout):
                raise queue.Empty
            return self.d.popleft()

    def empty(self):
        with self.cv:
            return not self.d

    def clear(self):
        with self.cv:
            self.d.clear()


class Core:
    def __init__(self, port=31250, host="127.0.0.1", timeout=30.0):
        t0 = time.time()
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=5)
                break
            except OSError:
                if time.time() - t0 > timeout:
                    raise
                time.sleep(0.5)
        self.sock.settimeout(None)
        self.f = self.sock.makefile("rb")
        self.q = LatestQueue()   # bounded; self.state always holds the latest state
        self.state = None
        self.hello = None
        self.manifest = None
        self.signals = None
        self.msgs = []           # everything except state/samples
        self.nstate = 0
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()
        self.wait_for(lambda m: m["type"] == "hello", 10)

    def _reader(self):
        try:
            self._read_lines()
        except ConnectionError:
            pass                 # the core went away (Windows resets the socket; Linux reads EOF)

    def _read_lines(self):
        for line in self.f:
            try:
                m = json.loads(line)
            except ValueError:
                print("bad json:", line[:200], file=sys.stderr)
                continue
            with self.lock:
                if m["type"] == "state":
                    self.state = m
                    self.nstate += 1
                elif m["type"] == "hello":
                    self.hello = m
                elif m["type"] == "manifest":
                    self.manifest = m
                elif m["type"] == "signals":
                    self.signals = m
                if m["type"] not in ("state", "samples"):
                    self.msgs.append(m)
            self.q.put(m)
        self.q.put(None)

    def send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())

    def t(self):
        return self.state["t"] if self.state else 0.0

    def wait_for(self, pred, timeout=60.0):
        """Next message (from now on) that satisfies pred; None on timeout."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                m = self.q.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if m is None:
                raise EOFError("core closed the connection")
            if pred(m):
                return m
        return None

    def wait_t(self, t, timeout=600.0):
        """Wait until a state with virtual time >= t arrived."""
        return self.wait_for(lambda m: m["type"] == "state" and m["t"] >= t, timeout)

    def drain(self):
        self.q.clear()

    def cli(self, text, quiet_s=1.0, timeout=60.0, first_s=5.0, host_quiet_s=3.0):
        """Send a CLI line and collect the `cli` log lines. Returns as soon as the reply is
        complete: quiet_s of virtual time without more output once output came, first_s of
        virtual time with no output at all (e.g. the firmware ignores the CLI while armed), an
        `error` for the request, or the state stream stopping (paused core): no message for
        host_quiet_s, stretched to 3x the largest host gap seen between states (a slow `speed`
        or a low `state_hz` spaces them out). Without states (not yet, or state_hz 0) only
        host_quiet_s of silence after output ends it; timeout (host s) is the outer bound."""
        self.drain()
        self.send({"type": "cli", "text": text})
        lines, last, t0 = [], None, None
        h_state, h_gap = None, 0.0                      # host time of the last state, max gap
        end = time.time() + timeout
        while time.time() < end:
            if h_state is not None:
                quiet = max(host_quiet_s, 3 * h_gap)
            else:                                       # no states (yet): only after output
                quiet = host_quiet_s if lines else end - time.time()
            m = self.wait_for(lambda m: m["type"] in ("log", "state", "error"),
                              min(quiet, max(0.01, end - time.time())))
            if m is None:
                break                                   # states stopped: paused or gone
            if m["type"] == "error" and m.get("what", "").startswith("cli"):
                break
            if m["type"] == "log" and m.get("src") == "cli":
                lines.append(m["text"])
                last = m["t"]
            elif m["type"] == "state":
                now = time.time()
                if h_state is not None:
                    h_gap = max(h_gap, now - h_state)
                h_state = now
                t0 = m["t"] if t0 is None else t0
                if last is not None and m["t"] - last > quiet_s:
                    break
                if last is None and m["t"] - t0 > first_s:
                    break
        return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["dump", "send"])
    ap.add_argument("msgs", nargs="*")
    ap.add_argument("--port", type=int, default=31250)
    ap.add_argument("--seconds", type=float, default=1.0)
    ap.add_argument("--out")
    ap.add_argument("--wait", type=float, default=2.0)
    a = ap.parse_args()
    if a.cmd == "dump":
        s = socket.create_connection(("127.0.0.1", a.port))
        f = s.makefile("rb")
        out = open(a.out, "w") if a.out else None
        t0 = None
        counts = {}
        first = last = None
        for line in f:
            m = json.loads(line)
            counts[m["type"]] = counts.get(m["type"], 0) + 1
            if out:
                out.write(line.decode())
            if m["type"] == "state":
                if t0 is None:
                    t0, first = m["t"], m
                last = m
                if m["t"] - t0 >= a.seconds - 1e-9:
                    break
            elif m["type"] != "signals":
                print(json.dumps(m))
        print("counts:", counts)
        if first:
            print(f"states: {counts['state']} from t={first['t']} to t={last['t']}")
            print("last state:", json.dumps(last))
        return
    c = Core(a.port)
    print("hello:", json.dumps(c.hello))
    for m in a.msgs:
        c.send(json.loads(m))
    end = time.time() + a.wait
    while time.time() < end:
        m = c.wait_for(lambda m: m["type"] not in ("state", "samples"), end - time.time())
        if m:
            print(json.dumps(m))
    print(f"t={c.t()} states={c.nstate}")


if __name__ == "__main__":
    main()
