# AI control and MPE

## MPE

* Channels are never remapped anywhere: live MIDI, the MIDI-file bounce, the take recorder and MIDI thru all keep the channel of every message, so per-note pitch bend, channel pressure and slide (CC74) reach the instrument as played.
* **MPE button** (next to MIDI REC; remembered): sends the lower-zone setup (15 members, +/-48 st member bend, +/-2 st master bend) to the loaded VSTi (on toggle and on instrument load), and prepends it to a MIDI file that has no MPE zone message before the bounce. Files that already carry RPN 6 are left alone. Toggling re-bounces the loaded file.
* **Top-note bend (part of the MPE button)**: with MPE on, a ONE-channel file that has pitch bend and no MPE setup is split: each note moves to its own member channel and the bend goes to the highest sounding note only (a note starts un-bent; when a higher note-on becomes the top, the old top's bend returns to centre, the bend does not move). CC / program / channel pressure go to the master channel; the source bend range (RPN 0, default 2 st) is rescaled to the member range. Multi-channel files and files that already carry an MPE setup are left alone. File bounce / file play only, not live keys. Test file: `tools/make_topbend_test.py`.
* Within one timestamp, note-ons are delivered after every other event, so a note never starts with the previous note's bend / slide still applied.
* The status line reports what the file looked like: `MPE zone setup in file, 7 note channels`, `no MPE zone setup, 2 note channels (zone setup added)`.
* The dashcam take recorder keeps the zone setup (RPN) and the first note's bend / pressure / slide, which arrive before the first note-on.

## Tracks (docs/TRACKS.md)

Four tracks and a master: **AUDIO** (the file player, or the live input), **INST 1-3** (a VSTi each), summed into **MASTER**
(`[FX insert] -> master strip -> tap -> device out`). Every track is `source -> strip` (gain, balance, mute, solo, meter).
The legacy FX (`role=fx`) is the master insert.

* `load_plugin` / `remove_plugin` / `show_editor` / `list_params` / `set_param` / `save_state` take `role=inst` (= `inst1`), `inst2`, `inst3`.
  `load_plugin role=inst2 midi_channels=[2]` sets the channels in the same call.
* **MIDI channels**: INST 1 listens to every channel by default, INST 2 and 3 to none until you give channels (`midi_channels`,
  or the field in the strip). A note on a channel nobody listens to is silent. With MPE, give an MPE instrument its master channel
  too (e.g. `1-8`): the zone setup travels on channel 1.
* `set_source` is a preset for who sounds (live / file: AUDIO; inst: the instruments), applied when the mode or what is loaded
  changes; the M and S buttons are yours in between.
* INST 2 and 3 are live-MIDI instruments: the MIDI-file bounce, MPE setup and PRE-RENDER stay with INST 1. `render_mix` renders
  all sounding tracks offline.
* Checks: `tools/tracks_smoke.py` (routing, mute, solo, gains by recording), `tools/live_vs_rig.py [--strips]` (live recording
  against the rig: bit-identical), `tools/render_mix_midi.py` (render_mix of an instrument track against the MIDI bounce),
  `tools/graph_scenarios.py` (routing of the live graph in ten states).

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
| set_source | `mode=live\|inst\|file` | which tracks sound (a preset for the strips: live/file -> AUDIO, inst -> the instruments); see Tracks below |
| load_audio, play, stop, seek, loop, prerender | | file player |
| track_set | `track=audio\|inst1\|inst2\|inst3\|master`, `gain_db`, `balance` (-1..1), `mute`, `solo`, `midi_channels` (INST: `[1,3]`, `"all"`, `"none"`) | the channel strip of a track; gain / balance / MIDI channels are kept across restarts (`tracks.json`) |
| track_status | | every track and the master: plug-in, gain, balance, mute, solo, MIDI channels |
| render_mix | `out`, `tail`, `compensate`, `rate`, `block` | the mixer as it is now, rendered offline by the rig: the sounding AUDIO file, each sounding INST track with the loaded MIDI file on its channels, the MASTER FX, the strips. Asynchronous (like rig_render); `left_out` names what could not be rendered (the live input, an instrument with no MIDI file loaded) |
| graph_dump | | the live graph's connections as sorted `node:ch -> node:ch` lines (checks that how the graph is built did not change the routing; `tools/graph_scenarios.py`) |
| rig_render | **`tracks=[{...}, ...}]`** (several tracks, each with the same keys as below; summed, then `master`; the graph lines them up by declared latency) or one track at the top level: `inst` (or `source=impulse`, `impulse_at`, `impulse_amp`; or `source=ara`, `ara=<ARA plug-in name or .vst3 path>`, `source_path` + the clip keys: the plug-in is bound to an ARA document made from the clip and renders it as a playback renderer (needs a build with the ARA SDK; `ara_probe` says whether a plug-in offers an ARA factory); `source=file`, `source_path`: a wav of the same rate as the input, placed as a clip with `clip_start` / `clip_offset` / `clip_length` in seconds and `clip_gain_db`), `insert` (or the test double `delay_actual` + `delay_declared`), `master` (name or .vst3 path), `events`, `out`, `rate`, `block`, `tail`, `compensate`, `dry_parallel`, `settle` (ms, default 500), `<role>_state`, `block_pattern` (block lengths used in turn, each 1..`block`), `bpm` / `time_sig` (the transport the plug-ins are told about), `automation` (`[{track, role, param, points:[[seconds, 0..1],...]}]`, applied sample-accurately; `automation_quantised` moves a change to the start of its block: the control), `probe=playhead|gain` (test doubles that report the transport / a host parameter), `master_tail_t60` / `master_tail_declared` (test double: a decaying tail of known length with a declared one), `<role>_params` (object: parameter name -> normalized 0..1, applied after the state) | fixed headless rig MIDI -> VSTi -> insert -> master, rendered offline from fresh instances; async, result in `status.rig` (declared latencies per stage, peak). `tools/rig_test.py` checks latency and repeatability. **settle**: Legacy Distortion loads its capture asynchronously and flips its declared latency 4 -> 0 when it lands; with settle=0 that raced the render (4 of 60 renders differed, latency read 0 or 4), with 500 ms 0 of 60. **dry_parallel** also sends the VSTi straight to the output so the graph's PDC must delay that path; the rig re-prepares until the graph's total equals the declared sum (without that, 2 of 40 parallel renders were uncompensated: graph latency 0, null residual +0.5 dB) and fails rather than render on a mismatch. **source=impulse**: one unit impulse instead of a VSTi, for measuring the real delay with `compensate=false` (peak position vs declared latency; `rig_test.py` section 4, with a known-delay control). **It runs in a separate worker process** (this exe started with `--rig-worker`; job in `rig_job.json`, log in `rig_result.json.log`): plugin loading, prepare, render and teardown all happen there, so a crashing or hanging plugin cannot take the bench down. A death is reported in `status.rig` (`worker_crashed`, exit code, the phase it was in, `job_file` kept for replay); `timeout` (s, default 600) kills a hang |
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
