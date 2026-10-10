#!/usr/bin/env python3
"""Operate the project inside the Synthesizer V Studio VST3 plug-in from outside (an AI, a script).

The plug-in's saved state (`save_state`) is JUCE's VST3 wrapper around a JSON document whose `projectContent` is the whole
project (the .svp format: tracks, notes with lyrics, the voice database, tempo). So a project is edited by decoding the state,
changing the JSON, encoding it again and loading it (`load_state`), or by giving it to the offline rig (`inst_state`).

  python tools/synthv_project.py dump   STATE.bin            # the project as JSON (stdout)
  python tools/synthv_project.py write  STATE.bin PROJECT.json OUT.bin   # a state carrying that project (STATE.bin is the template)
  python tools/synthv_project.py sing   OUT.wav "a:60:1 i:62:1 u:64:1 e:65:1" [--db "Kasane Teto"] [--bpm 120]
        # notes "lyric:midi_pitch:beats" -> a project -> the offline rig renders it (needs a running TestBench)

What was found out (2026-10-11):
  - the database is named by `mainRef.database` = {"name": "Kasane Teto", "version": "104", ...}; the name is the one in the
    database's own header (".name"), WITHOUT "AI"; a wrong name silently gives a plain placeholder tone (spectral centroid = the
    pitch) instead of a voice. `backendType` must be left as the plug-in wrote it ("SVR1" made it fall back to the placeholder).
  - a note is {"onset": blicks, "duration": blicks, "lyrics": "a", "phonemes": "", "pitch": midi, "detune": 0, "attributes": {}};
    705600000 blicks per quarter note; the project's tempo is in `time.tempo`.
  - the live TestBench host gives the plug-in no transport, so the project plays only in the offline rig (which does).

Library:  from synthv_project import decode_state, encode_state
"""
import json
import re
import struct
import sys

_CHARSET = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"   # JUCE MemoryBlock::toBase64Encoding


def _b64_decode(text, n):
    bits = nb = 0
    out = bytearray()
    for ch in text:
        bits |= _CHARSET.index(ch) << nb
        nb += 6
        while nb >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            nb -= 8
    return bytes(out[:n])


def _b64_encode(data):
    bits = nb = 0
    out = []
    for byte in data:
        bits |= byte << nb
        nb += 8
        while nb >= 6:
            out.append(_CHARSET[bits & 63])
            bits >>= 6
            nb -= 6
    if nb:
        out.append(_CHARSET[bits & 63])
    return "".join(out)


def decode_state(blob):
    """-> (wrapper text before/after the component data, the plug-in's own state: dict, the bytes after its JSON)"""
    text = blob.decode("latin1")
    m = re.search(r"<IComponent>(\d+)\.([^<]+)</IComponent>", text)
    if not m:
        raise ValueError("not a Synthesizer V plug-in state")
    n = int(m.group(1))
    comp = _b64_decode(m.group(2), n)
    obj, end = json.JSONDecoder().raw_decode(comp.decode("utf-8"))
    tail = comp[len(comp.decode("utf-8")[:end].encode("utf-8")):]
    return blob, m, obj, tail


def encode_state(template, project):
    """A state like `template`, carrying `project` (a dict: the .svp content)."""
    blob, m, obj, tail = decode_state(template)
    obj["projectContent"] = json.dumps(project)
    comp = json.dumps(obj).encode("utf-8") + tail
    xml = blob[blob.index(b"<?xml"):]
    new_xml = xml.decode("latin1").replace(m.group(0), f"<IComponent>{len(comp)}.{_b64_encode(comp)}</IComponent>").encode("latin1")
    head = blob[:blob.index(b"<?xml")]                 # "VC2!" + the length of the XML
    head = head[:4] + struct.pack("<I", len(new_xml) - (len(xml) - struct.unpack("<I", head[4:8])[0])) if len(head) >= 8 else head
    return head + new_xml


def project_with_notes(template, notes, db=None, bpm=None):
    """notes: [(lyric, midi_pitch, beats), ...] one after the other from beat 0 -> the project (dict) carrying them"""
    proj = project_of(template)
    blick = 705600000
    t, out = 0, []
    for lyric, pitch, beats in notes:
        out.append({"onset": int(t * blick), "duration": int(beats * blick), "lyrics": lyric, "phonemes": "", "pitch": int(pitch), "detune": 0, "attributes": {}})
        t += beats
    trk = proj["tracks"][0]
    trk["mainGroup"]["notes"] = out
    if db:
        trk["mainRef"]["database"].update(db)
    if bpm:
        proj["time"]["tempo"] = [{"position": 0, "bpm": float(bpm)}]
    return proj, t


def project_of(blob):
    return json.loads(decode_state(blob)[2]["projectContent"])


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "dump":
        print(json.dumps(project_of(open(sys.argv[2], "rb").read()), ensure_ascii=False, indent=1))
    elif len(sys.argv) >= 5 and sys.argv[1] == "write":
        out = encode_state(open(sys.argv[2], "rb").read(), json.load(open(sys.argv[3], encoding="utf-8")))
        open(sys.argv[4], "wb").write(out)
        print("wrote", sys.argv[4], len(out), "bytes")
    elif len(sys.argv) >= 4 and sys.argv[1] == "sing":
        import os, argparse
        ap = argparse.ArgumentParser()
        ap.add_argument("cmd"); ap.add_argument("out"); ap.add_argument("notes")
        ap.add_argument("--db", default="Kasane Teto"); ap.add_argument("--version", default="104"); ap.add_argument("--bpm", type=float, default=120.0)
        a = ap.parse_args()
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from tb import TestBench
        tb = TestBench()
        plugin = r"C:\Program Files\Common Files\VST3\synthv-studio-plugin-x64.vst3"
        # the plug-in's empty state is the template: load it into INST 2, save it, then drop it
        tb.call("remove_plugin", role="inst2"); tb.wait_idle()
        assert tb.call("load_plugin", role="inst2", path=plugin).get("ok"); tb.wait_idle(timeout=120)
        tmpl_path = os.path.join(os.environ.get("TEMP", "."), "synthv_template.bin")
        tb.call("save_state", role="inst2", path=tmpl_path)
        tb.call("remove_plugin", role="inst2"); tb.wait_idle()
        notes = []
        for tok in a.notes.split():
            lyric, pitch, beats = tok.split(":")
            notes.append((lyric, int(pitch), float(beats)))
        proj, total_beats = project_with_notes(open(tmpl_path, "rb").read(), notes, {"name": a.db, "version": a.version}, a.bpm)
        state = os.path.join(os.environ.get("TEMP", "."), "synthv_sing_state.bin")
        open(state, "wb").write(encode_state(open(tmpl_path, "rb").read(), proj))
        seconds = total_beats * 60.0 / a.bpm
        r = tb.call("rig_render", inst=plugin, inst_state=state, events=[{"t": 0.0, "type": "cc", "ch": 1, "cc": 1, "value": 0}],
                    out=os.path.abspath(a.out), rate=44100, block=512, tail=seconds + 1.0, settle=3000)
        tb.wait_idle(timeout=300)
        print("rendered", a.out, tb.call("status").get("rig", {}).get("peak_db"), "dB peak")
    else:
        print(__doc__)
