#!/usr/bin/env python3
"""Tracks on the live bench (docs/TRACKS.md): MIDI channel routing, mute, per-track gain and master gain, measured by
recording what the bench plays.  Needs a running TestBench with PAGANIHANDS (INST 1) and Flesh808 (INST 2) cached.

  python tools/tracks_smoke.py

Checks: a note on a channel nobody listens to is silent (-120 dB); a muted track is silent; a track strip at -6 dB lowers the
track by 6.02 dB (+-0.2); the master strip does the same for everything.
"""
import sys, time, os, tempfile
sys.path.insert(0, 'C:/dev/vst-testbench/tools')
from tb import TestBench
tb = TestBench()
for r in ("fx", "inst", "inst2", "inst3"):
    tb.call("remove_plugin", role=r)
tb.wait_idle()
tb.call("load_plugin", role="inst", name="PAGANIHANDS"); tb.wait_idle()
tb.call("load_plugin", role="inst2", name="Flesh808"); tb.wait_idle()
tb.call("set_source", mode="inst"); tb.wait_idle()
tmp = tempfile.gettempdir()

def play(ch, name):
    rec = os.path.join(tmp, f"multi_{name}.wav")
    tb.call("record_start", path=rec); time.sleep(0.3)
    ev = [{"t": 0.0, "type": "note_on", "ch": ch, "note": 57, "vel": 100, "dur": 1.0}]
    tb.call("midi_play", events=ev); time.sleep(1.8)
    tb.call("record_stop"); tb.call("midi_stop"); time.sleep(0.4)
    a = tb.call("analyze", path=rec)
    return round(a.get("rms_db", -120), 2)

tb.call("track_set", track="inst1", midi_channels=[1])
tb.call("track_set", track="inst2", midi_channels=[2], mute=False, gain_db=0)
tb.call("track_set", track="master", gain_db=0)
base1 = play(1, "a1"); base2 = play(2, "a2"); none3 = play(3, "a3")
print("INST 1 on ch1:", base1, "| INST 2 on ch2:", base2, "| ch3 (nobody):", none3)
tb.call("track_set", track="inst2", mute=True)
print("ch2 with INST 2 muted:", play(2, "m"))
tb.call("track_set", track="inst2", mute=False, gain_db=-6)
g = play(2, "g")
print("ch2 INST 2 at -6 dB:", g, " (expected about", round(base2 - 6, 2), ")")
tb.call("track_set", track="inst2", gain_db=0)
tb.call("track_set", track="master", gain_db=-6)
mg = play(2, "mm"); print("ch2 MASTER at -6 dB:", mg, " (expected about", round(base2 - 6, 2), ")")
tb.call("track_set", track="master", gain_db=0)
tb.call("track_set", track="inst1", midi_channels=[1, 2])
print("ch2 reaching both INST 1 and INST 2:", play(2, "both"))

# solo: soloing INST 2 silences INST 1 (a note on channel 1) and leaves INST 2 alone; un-soloing brings INST 1 back
tb.call("track_set", track="inst1", midi_channels=[1]); tb.call("track_set", track="inst2", midi_channels=[2], gain_db=0, mute=False)
alone = play(1, "solo0")
tb.call("track_set", track="inst2", solo=True)
silenced = play(1, "solo1")
still = play(2, "solo2")
tb.call("track_set", track="inst2", solo=False)
back = play(1, "solo3")
print("solo: INST 1 alone", alone, "| INST 2 soloed -> INST 1", silenced, "| INST 2 itself", still, "| solo off -> INST 1", back)
