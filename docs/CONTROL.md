# AI control and MPE

## MPE

* Channels are never remapped anywhere: live MIDI, the MIDI-file bounce, the take recorder and MIDI thru all keep the channel of every message, so per-note pitch bend, channel pressure and slide (CC74) reach the instrument as played.
* **MPE button** (next to MIDI REC; remembered): sends the lower-zone setup (15 members, +/-48 st member bend, +/-2 st master bend) to the loaded VSTi (on toggle and on instrument load), and prepends it to a MIDI file that has no MPE zone message before the bounce. Files that already carry RPN 6 are left alone. Toggling re-bounces the loaded file.
* **Top-note bend (part of the MPE button)**: with MPE on, a ONE-channel file that has pitch bend and no MPE setup is split: each note moves to its own member channel and the bend goes to the highest sounding note only (a note starts un-bent; when a higher note-on becomes the top, the old top's bend returns to centre, the bend does not move). CC / program / channel pressure go to the master channel; the source bend range (RPN 0, default 2 st) is rescaled to the member range. Multi-channel files and files that already carry an MPE setup are left alone. File bounce / file play only, not live keys. Test file: `tools/make_topbend_test.py`.
* Within one timestamp, note-ons are delivered after every other event, so a note never starts with the previous note's bend / slide still applied.
* The status line reports what the file looked like: `MPE zone setup in file, 7 note channels`, `no MPE zone setup, 2 note channels (zone setup added)`.
* The dashcam take recorder keeps the zone setup (RPN) and the first note's bend / pressure / slide, which arrive before the first note-on.

## AI control

The bench listens on `127.0.0.1:47213` (change it in `%APPDATA%\VstTestBench\control_port.txt`; shown in the window title). One JSON object per line in, one per line out: `{"id":1,"cmd":"status"}` -> `{"id":1,"ok":true,...}` or `{"ok":false,"error":"..."}`. Local only.

Client: `python tools/tb.py <cmd> key=value ...` (or a JSON body), `--wait` blocks until `busy` is false. From Python: `from tb import TestBench`.

| cmd | arguments | notes |
|---|---|---|
| status | | source, plugins, transport, MIDI/bounce, MPE, audio device, output level (peak/rms since last call), `busy` |
| list_plugins / load_plugin / remove_plugin | `role=fx\|inst`, `path=...vst3` or `name=` (cached) | loading is async: wait for `busy=false` |
| list_params | `role`, `filter`, `limit` | normalized value + text |
| set_param / set_params | `role`, `name` or `index`, `value` (0..1) or `text`; batch: `values=[...]` | goes through the host, so knob-driven re-render / re-bounce still triggers |
| save_state / load_state | `role`, `path` | raw plugin state |
| set_bypass, show_editor, screenshot | `screenshot target=main\|fx\|inst path=x.png` | PNG for vision; relative paths land in `%APPDATA%\VstTestBench` |
| set_source | `mode=live\|inst\|file` | |
| load_audio, play, stop, seek, loop, prerender | | file player |
| rig_render | `inst`, `insert`, `master` (name or .vst3 path), `events`, `out`, `rate`, `block`, `tail`, `compensate`, `dry_parallel`, `settle` (ms, default 500), `<role>_state` | fixed headless rig MIDI -> VSTi -> insert -> master, rendered offline from fresh instances; async, result in `status.rig` (declared latencies per stage, peak). `tools/rig_test.py` checks latency and repeatability. **settle**: Legacy Distortion loads its capture asynchronously and flips its declared latency 4 -> 0 when it lands; with settle=0 that raced the render (4 of 60 renders differed, latency read 0 or 4), with 500 ms 0 of 60. **dry_parallel** also sends the VSTi straight to the output so the graph's PDC must delay that path; the rig re-prepares until the graph's total equals the declared sum (without that, 2 of 40 parallel renders were uncompensated: graph latency 0, null residual +0.5 dB) and fails rather than render on a mismatch |
| load_midi | `path`, `mpe=true\|false` | offline bounce through the VSTi, then `play` |
| export_midi | `path` | write what the instrument receives (after TOP-BEND / zone setup) as an SMF, for inspection |
| mpe | `on`, `members`, `member_pb`, `master_pb`, `send` | |
| midi_send | `events=[...]` | immediate, live |
| midi_play / midi_play_file | `events=[...]` / `path` | timed, real time, own thread |
| midi_stop | | all notes / sound off, 16 channels |
| record_start / record_stop | `path`, `seconds` | record what the bench plays (32-bit float wav) |
| analyze | `path` | peak, rms, dc, first sound, silent ratio, 0.25 s rms envelope |
| audio_devices / set_audio | `type`, `output`, `rate`, `buffer` | |

Events: `{"t":0.5,"type":"note_on","ch":2,"note":60,"vel":100,"dur":1.0}`; types `note_on note_off pitch_bend (bend -1..1 or value 0..16383) pressure poly_pressure cc (cc,value) slide (CC74) program all_off`.

Typical loop: `load_plugin` -> `set_param` -> `load_midi` (or `midi_play` + `record_start`) -> `analyze` -> adjust.
Auto backend is on by default: `set_source file` switches to WASAPI, `inst`/`live` to ASIO.
