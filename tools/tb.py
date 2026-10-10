#!/usr/bin/env python3
"""Client for the VST TestBench control server (line-JSON over 127.0.0.1).

CLI:   python tools/tb.py status
       python tools/tb.py load_plugin role=inst name="PAGANIHANDS"
       python tools/tb.py set_param role=inst name=Cutoff value=0.4
       python tools/tb.py midi_play '{"events":[{"type":"note_on","ch":2,"note":60,"dur":1}]}'
       python tools/tb.py --wait load_midi path=C:/x.mid mpe=true     (waits until busy=false)
Values are parsed as JSON when possible (numbers, true/false), else kept as text.
A single argument starting with { is taken as the whole request body.

Library:  from tb import TestBench; tb = TestBench(); tb.call("status")
Port: %APPDATA%/VstTestBench/control_port.txt, default 47213.
"""
import json, os, socket, sys, time


def default_port():
    p = os.path.join(os.environ.get("APPDATA", ""), "VstTestBench", "control_port.txt")
    try:
        return int(open(p).read().strip())
    except Exception:
        return 47213


class TestBench:
    def __init__(self, port=None, host="127.0.0.1", timeout=150):
        self.sock = socket.create_connection((host, port or default_port()), timeout=timeout)
        self.f = self.sock.makefile("rwb")
        self._id = 0

    def call(self, cmd, **kw):
        self._id += 1
        req = dict(kw, cmd=cmd, id=self._id)
        self.f.write((json.dumps(req) + "\n").encode("utf-8"))
        self.f.flush()
        line = self.f.readline()
        if not line:
            raise ConnectionError("server closed the connection")
        return json.loads(line.decode("utf-8"))

    def wait_idle(self, timeout=300, poll=0.25):
        """Block until status.busy is false (plugin loads, MIDI bounce, renders)."""
        t0 = time.time()
        time.sleep(poll)
        while time.time() - t0 < timeout:
            st = self.call("status")
            if not st.get("busy"):
                return st
            time.sleep(poll)
        raise TimeoutError("bench still busy after %ss" % timeout)


def _val(s):
    try:
        return json.loads(s)
    except Exception:
        return s


def main(argv):
    wait = False
    if argv and argv[0] == "--wait":
        wait, argv = True, argv[1:]
    if not argv:
        print(__doc__)
        return 2
    cmd, rest = argv[0], argv[1:]
    if rest and rest[0].lstrip().startswith("{"):
        kw = json.loads(rest[0])
    else:
        kw = {}
        for a in rest:
            if "=" not in a:
                print("bad argument (want key=value): " + a)
                return 2
            k, v = a.split("=", 1)
            kw[k] = _val(v)
    try:
        tb = TestBench()
    except OSError as e:
        print("cannot reach the bench (is VST TestBench running?): %s" % e)
        return 1
    r = tb.call(cmd, **kw)
    if wait and r.get("ok"):
        r["after_wait"] = tb.wait_idle()
    print(json.dumps(r, ensure_ascii=False, indent=2))
    return 0 if r.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
