#!/usr/bin/env python3
"""REFERENCE IMPLEMENTATION of devices/src/cli_norm.c (include/fb/cli_norm.h has the rules): the
firmware CLI lines as settings, so two layers of a config script, or the script and the firmware's
`diff all`, compare setting by setting. test_session_equiv.sh checks both on every bench's
conflicts and on tests/fixtures/cli_diff/ (a change of rules goes into both).

  cli_norm.py --diff <script.json> <diff.txt>    the script ([{text, layer, source}] or a text file,
                                                 one line each) against a `diff all` text: the JSON
                                                 object fb_cli_diff_json makes, one line
  cli_norm.py --keys <file>                      each line's kind, key and value (debugging)
"""
import json, re, sys

WS = " \t\r\n\v\f"
INDEXED = {"serial": 1, "aux": 1, "smix": 1, "mmix": 1, "servo": 1, "adjrange": 1, "rxrange": 1, "logic": 1,
           "gvar": 1, "pid": 1, "led": 1, "safehome": 1, "temp_sensor": 1, "color": 1, "timer": 1, "geozone": 1,
           "fwapproach": 1, "mode_color": 2, "osd_layout": 2, "resource": 2}
FLAGS = ("feature", "beeper", "blackbox")
IGNORED = ("batch", "defaults", "save", "exit")
CONTEXTS = ("control_profile", "profile", "mixer_profile", "battery_profile", "rateprofile")
OTHER, SET, CONTEXT, RESET = "other", "set", "context", "reset"
NUM = re.compile(r"[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?")


def words(z):
    return [w for w in re.split(r"[ \t\r\n\v\f]+", z) if w]


def lower(z):
    return "".join(c.lower() if c < "\x80" else c for c in z)


def upper(z):
    return "".join(c.upper() if c < "\x80" else c for c in z)


def _trunc(z, n):
    """C's snprintf into a char[n + 1]: the first n bytes"""
    return z.encode("utf-8", "surrogateescape")[:n].decode("utf-8", "surrogateescape")


class Ctx:
    def __init__(self):
        self.name, self.idx = "", ""

    def suffix(self):
        return "" if not self.name or self.idx == "1" else f" @{self.name} {self.idx}"


def norm(ctx, line):
    """(kind, key, value); key/value None unless SET (RESET: key only)"""
    w = words(line)
    if not w or w[0].startswith("#"):
        return OTHER, None, None
    cmd = lower(w[0])
    if cmd in IGNORED:
        return OTHER, None, None
    if cmd in CONTEXTS and len(w) == 2:
        ctx.name = "control_profile" if cmd == "profile" else cmd
        ctx.idx = _trunc(w[1], 15)
        return CONTEXT, None, None
    if cmd == "set":
        rest = " ".join(w[1:])
        if "=" not in rest:
            return OTHER, None, None
        a, v = rest.split("=", 1)
        a = a.rstrip(WS)
        if not a:
            return OTHER, None, None
        return SET, "set " + lower(a) + ctx.suffix(), " ".join(words(v))
    if cmd in FLAGS:
        if len(w) >= 2 and w[1] != "-":
            off = w[1].startswith("-")
            return SET, f"{cmd} {upper(w[1][1:] if off else w[1])}", "off" if off else "on"
        return OTHER, None, None
    if cmd == "map":
        if len(w) >= 2:
            return SET, "map", " ".join(w[1:])
        return OTHER, None, None
    if cmd in INDEXED:
        sf = ctx.suffix() if cmd in ("smix", "mmix") else ""
        n = INDEXED[cmd]
        if len(w) >= 2 and w[1] == "reset":
            return RESET, cmd + sf, None
        if len(w) > n:
            return SET, f"{cmd} {' '.join(w[1:1 + n])}{sf}", " ".join(w[1 + n:])
        return OTHER, None, None
    return SET, " ".join(w), ""


def _num(z):
    return float(z) if NUM.fullmatch(z) else None


def _caseeq(a, b):
    return lower(a) == lower(b)


def _tok_eq(a, b):
    if _caseeq(a, b):
        return True
    x, y = _num(a), _num(b)
    return x is not None and y is not None and x == y


def _strtoul(z):
    m = re.match(r"[ \t\r\n\v\f]*\+?([0-9]+)", z)
    return min(int(m.group(1)), 2 ** 64 - 1) if m else 0


def _rel_bit(z):
    """the bit of a relative serial word after its sign: 0..31, else -1 (cli_norm.c rel_bit)"""
    m = re.match(r"[ \t\r\n\v\f]*([+-]?[0-9]+)", z)
    b = int(m.group(1)) if m else 0
    return b if 0 <= b <= 31 else -1


def _serial(key):
    return key.startswith("serial ")


def _rel(v):
    return v[:1] in ("+", "-")


def value_eq(key, a, b):
    wa, wb = words(a), words(b)
    if _serial(key) and wa and _rel(wa[0]) and not (wb and _rel(wb[0])):
        m = _strtoul(wb[0]) if wb else 0
        r = True
        for t in wa:
            if not r or not _rel(t):
                break
            bit = _rel_bit(t[1:])
            s = bit >= 0 and (m >> bit & 1) == 1
            r = s if t[0] == "+" else not s
        return r
    if _serial(key) and wb and _rel(wb[0]) and not (wa and _rel(wa[0])):
        return value_eq(key, b, a)
    return len(wa) == len(wb) and all(_tok_eq(x, y) for x, y in zip(wa, wb))


def _reset_hits(reset, key):
    rs = reset.find(" @")
    cl = rs if rs >= 0 else len(reset)
    if key[:cl] != reset[:cl] or key[cl:cl + 1] != " ":
        return False
    ks = key.find(" @")
    return (key[ks:] if ks >= 0 else "") == (reset[rs:] if rs >= 0 else "")


def _serial_combine(old, rel):
    wo, wr = words(old), words(rel)
    if wo and not _rel(wo[0]):
        m = _strtoul(wo[0])
        for t in wr:
            if not _rel(t):
                break
            bit = _rel_bit(t[1:])
            if bit < 0:
                continue
            if t[0] == "+":
                m |= 1 << bit
            else:
                m &= ~(1 << bit) & (2 ** 64 - 1)
        rest = " ".join(wo[1:])
        return f"{m}{' ' if rest else ''}{rest}"
    return f"{old} {rel}"


def effective(texts):
    """[(key, value, line)] as the script ends, ordered by the line that set each last"""
    ctx, eff = Ctx(), []
    for i, t in enumerate(texts):
        kind, k, v = norm(ctx, t)
        if kind == RESET:
            eff = [e for e in eff if not _reset_hits(k, e[0])]
        elif kind == SET:
            for j, e in enumerate(eff):
                if e[0] == k:
                    nv = _serial_combine(e[1], v) if _serial(k) and _rel(v) else v
                    eff[j] = (k, nv, i)
                    break
            else:
                eff.append((k, v, i))
    return sorted(eff, key=lambda e: e[2])


def conflicts(lines):
    """[(key, [line indexes])]: lines = [{text, layer, source}]"""
    ctx = Ctx()
    kv = []
    for x in lines:
        kind, k, v = norm(ctx, x["text"])
        kv.append((k, v) if kind == SET else (None, None))
    out, done = [], set()
    for i, (k, _) in enumerate(kv):
        if k is None or i in done:
            continue
        idx = [j for j in range(i, len(kv)) if kv[j][0] == k]
        done.update(idx)
        last = idx[-1]
        layers = {lines[j]["layer"] or "" for j in idx}
        differ = any(not value_eq(k, kv[j][1], kv[last][1]) for j in idx)
        if len(layers) >= 2 and differ:
            out.append((k, idx))
    return out


def dumps(o):
    return json.dumps(o, ensure_ascii=False, separators=(",", ":"))


def diff_json(script, diff_text):
    """fb_cli_diff_json: the members' text (no braces)"""
    fl = [x.strip(WS) for x in (diff_text or "").split("\n")] if diff_text else []
    es = effective([x["text"] for x in script])
    ef = effective(fl)
    fk = {e[0]: e for e in ef}
    sk = {e[0] for e in es}
    sent, current, changed, only_fw, only_sc = [], [], [], [], []
    for k, v, i in es:
        L = script[i]
        sent.append({"setting": k, "value": v, "text": L["text"], "layer": L.get("layer"), "source": L.get("source")})
        f = fk.get(k)
        if f is None:
            only_sc.append({"setting": k, "script": L["text"], "layer": L.get("layer"), "source": L.get("source")})
        elif not value_eq(k, v, f[1]):
            changed.append({"setting": k, "script": L["text"], "firmware": fl[f[2]], "layer": L.get("layer"),
                            "source": L.get("source")})
    for k, v, i in ef:
        current.append({"setting": k, "value": v, "text": fl[i]})
        if k not in sk:
            only_fw.append({"setting": k, "firmware": fl[i]})
    return ",".join(f"{dumps(n)}:{dumps(x)}" for n, x in (("sent", sent), ("current", current), ("changed", changed),
                                                          ("only_firmware", only_fw), ("only_script", only_sc)))


def _read(p):
    with open(p, "rb") as f:
        return f.read().decode("utf-8", "surrogateescape")


def main():
    out = sys.stdout.buffer
    if len(sys.argv) == 4 and sys.argv[1] == "--diff":
        t = _read(sys.argv[2])
        try:
            script = json.loads(t)
        except ValueError:
            script = [{"text": x.strip(WS), "layer": None, "source": None} for x in t.split("\n") if x.strip(WS)]
        out.write(("{" + diff_json(script, _read(sys.argv[3])) + "}\n").encode("utf-8", "surrogateescape"))
        return 0
    if len(sys.argv) == 3 and sys.argv[1] == "--keys":
        ctx = Ctx()
        for x in _read(sys.argv[2]).split("\n"):
            kind, k, v = norm(ctx, x)
            out.write(f"{kind}\t{k}\t{v}\n".encode("utf-8", "surrogateescape"))
        return 0
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
