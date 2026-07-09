# VST TestBench — ビルド & 使い方

Cubaseの重さに耐えられない、VSTを1個だけ通して素早く音を確認するための**軽量VST3ホスト（テストベンチ）**。
JUCE 8.0.13 スタンダードアロンアプリ。

## 目的（何をするソフトか）
DAWの重い機能を捨て、「MIDIスイッチング」＋「VST再生」だけをやる。

## v0.2.0〜: ソース段3系統 + PRE-RENDERモード
- **ソース段をコンボで3切替**: ①Live入力(下図の実機経路) ②VSTiプレイバック(MIDI in→VSTi→FX)
  ③音楽ファイルプレイヤー(Play/Pause/Stop/Loop/シーク。WAV/FLAC/MP3/OGGネイティブ、
  m4a/AAC/OPUS等はffmpegフォールバックで`decode_cache.wav`に変換)
- **PRE-RENDERモード**(ファイル→FX経路限定): FXのオフライン複製でファイル全体をRAMに
  先行レンダリングし、再生はキャッシュを流すだけにする。リアルタイム制約を捨てて
  重い処理を大メモリブロック単位で回す設計。**思想と進化の記録は [docs/PRERENDER.md](docs/PRERENDER.md)**
- **Auto backend (v0.7.0)**: ソースモード連動でオーディオバックエンドを自動切替 —
  ファイル再生=WASAPI(レイテンシー無関係・ASIOドライバをホットパスから外す)/
  Live・VSTi=ASIO。各バックエンドのデバイス設定は個別スナップショット保存
  (`audio_settings_ASIO.xml` / `audio_settings_Windows_Audio.xml`)で往復しても崩れない
- **GPU FX (v0.8.x)**: gpufx常駐ワーカー(TCP)でファイルをGPU異世界化→世代ファイルを
  位置保持ホットスワップ。describeスキーマからパラメータUIを動的生成
- **MIDIバウンス (v0.9.0 / Phase C)**: **Open MIDI file...** でMIDIファイルをロード済みVSTiの
  オフライン複製(state同期・バイパスパラメータ0化)で実時間超バウンス→
  `midi_bounce_NNNN.wav`(世代番号付き)をファイルプレイヤーに載せる=
  **GPU FX / FXチェーン / PRE-RENDERが無改修で全部効く**。VSTiのノブを回すと
  600msデバウンスで再バウンス→再生位置保持でホットスワップ。
  ライブMIDI入力は対象外のまま(未来が未知=先行レンダリング不能)。
  テスト用MIDI: `assets/test_bounce.mid`(Cメジャーアルペジオ9秒)

```
鍵盤 ─MIDI─▶ TestBench ─(thru)─▶ Reface CP ─音─▶ UR-RT2 in3/4 ─▶ VST ─▶ 出力
                                      └ VSTi検査時は MIDI が VST にも入る
```

- プラグイン未ロード時は **in3/4 をそのまま出力にモニター**（配線チェック用）
- **フォルダスキャンしない**。VSTはファイル指定で1個ずつ読み、`KnownPluginList` をXMLキャッシュ
  （プラグイン800個環境でも起動時スキャンゼロ）

## 依存
- **JUCE**: `C:/JUCE`（8.0.13）
- **ASIO SDK**: `C:/SDKs/ASIOSDK`（`common/iasiodrv.h` の存在で自動有効化）
  - なければ WASAPI/DirectSound にフォールバック（CMakeが警告を出す）

## ビルド
```powershell
cmake -S C:\dev\vst-testbench -B C:\dev\vst-testbench\build -G "Visual Studio 17 2022" -A x64
cmake --build C:\dev\vst-testbench\build --config Release
```
出力: `C:\dev\vst-testbench\build\VstTestBench_artefacts\Release\VST TestBench.exe`

> 実行中は exe がロックされるため、再ビルド前にアプリを閉じること
> （`Get-Process -Name "VST TestBench" | Stop-Process -Force`）。

## 使い方
1. **Audio / MIDI Settings** → Type=**ASIO** / Device=**Yamaha Steinberg USB ASIO (UR-RT2)**
   → Active input channels で **3 / 4** をチェック / MIDI Inputs で鍵盤を有効化
2. **VST input pair** = **3 / 4**（デフォルト）
3. **Reface MIDI out** で Reface CP を選択（名前に "reface" があれば自動選択）→ **MIDI thru → Reface** ON
4. **Load VST3 file...** で `.vst3` を1個選択（初期フォルダ `Common Files\VST3`）
   → 以後は **Recent plugins** からワンクリック（再スキャンなし）
5. **Open Plugin UI** / **Bypass** で素通し比較 / **Remove Plugin** で外す

## 設定の保存先
`%APPDATA%\VstTestBench\`
- `known_plugins.xml` — 読み込み済みプラグインのキャッシュ
- `audio_settings.xml` — オーディオ/MIDIデバイス設定

## 構成
```
CMakeLists.txt          juce_add_gui_app + ASIO条件付き有効化 + VST3ホスト
assets/icon.png         タスクバー用「VT」アイコン（白タイル/黒枠/黒字）
Source/Main.cpp         JUCEApplication + DocumentWindow
Source/MainComponent.*  AudioProcessorGraph ホスト本体
```

## 実装メモ
- 中核は `juce::AudioProcessorGraph`（audioIn / audioOut / midiIn / plugin ノードを配線）
- MIDI thru（→Reface）は `AudioDeviceManager` の MidiInputCallback で直接 `MidiOutput` に転送
- 同じMIDIを `AudioProcessorPlayer` 経由でグラフにも供給（VSTi検査対応）
- **JUCE 8.0.13 API変更**: `AudioPluginFormatManager::addDefaultFormats()` は削除
  → 自由関数 `juce::addDefaultFormatsToManager(mgr)` を使う
- プラグインエディタは `createEditorAndMakeActive()`、無ければ `GenericAudioProcessorEditor`
