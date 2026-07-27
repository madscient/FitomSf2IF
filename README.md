# FitomSf2IF

FITOM_X の `IHWPlugin` C API を実装する、[FluidSynth](https://www.fluidsynth.org/)
(SF2サウンドフォント再生)統合 hwif プラグイン。

設計の経緯・決定事項は FITOM_X リポジトリ側のドキュメント
[`docs/sf2-fluidsynth-integration.md`](../FITOM_X/docs/sf2-fluidsynth-integration.md)
を正本とする。本READMEは実装リポジトリ側の構成・ビルド方法のみを記す
(現在の実装状況・未実装項目は [`docs/STATUS.md`](docs/STATUS.md) を参照)。

## アーキテクチャ概要

```
FITOM core
  └── HWPort (IPort アダプター)
        └── IHWPlugin C API  ← FitomSf2IF.dll（このライブラリ）
              ├── FluidSynthEngine   fluid_synth_t + fluid_audio_driver_t のラッパー
              │     └── SoundFontキャッシュ (file path -> sfont_id)
              └── MidiEventParser    生MIDIバイト列 → FluidSynthEngine呼び出しへ変換
```

- `fluid_synth_t`(合成エンジン層)をリンクし、FluidSynthのMIDI受信層
  (`fluid_midi_driver_t`/`fluid_player_t`)は使わない(生MIDIバイト列は
  `MidiEventParser`がパースする)。FluidSynth本体のソースは無改造。
- 音声出力はFluidSynth自身が持つ音声デバイス層(`fluid_audio_driver_t`)をそのまま
  使う。fluidsynthをプラグインへ完全に分離した結果、自前のオーディオライブラリを
  別途持つ理由が無くなったため、依存ライブラリを増やさずfluidsynth内蔵の機能に
  委譲する構成にしている。他のhwifプラグイン(FitomEmuIF等)と同じく、
  `HWPlugin_WriteBlock`で生MIDIバイト列を受け取るだけで音声出力まで自己完結する。
  `IHWPlugin.h`への新規追加は無い(既存契約のみで実装)。
- MPU×ch → fluidsynth chan(0-15)の「窓」変換、CC#32→SF2ファイル/バンクの解決は
  FITOM_Xコア本体側の責務であり、本プラグインはコアから既に変換済みの生MIDIバイト列
  (chanニブル済み変換)とプライベートSysEx(バンク選択、未確定・`docs/STATUS.md`参照)を
  受け取るだけの、状態を持たない「CH変換付きMIDIパッチベイ」の受け口として振る舞う。

## ディレクトリ構成

```
FitomSf2IF/
├── plugin_sdk/include/fitom/
│   └── IHWPlugin.h              # FITOM_X plugin_sdk/include/fitom/IHWPlugin.h の独立コピー(変更禁止)
├── src/
│   ├── Sf2IfImpl.cpp             # IHWPlugin C API 実装本体(エントリポイント)
│   ├── FluidSynthEngine.h/.cpp   # fluid_synth_t + fluid_audio_driver_t ラッパー
│   └── MidiEventParser.h/.cpp    # 生MIDIバイト列パーサ
├── examples/
│   └── smoke_test.cpp            # 動作確認用実行ファイル(BUILD_FITOMSF2IF_TEST=ON)
├── docs/
│   └── STATUS.md                 # 実装状況・未実装項目
├── sf2if_profile.example.json    # プロファイルサンプル
├── CMakeLists.txt
├── CMakePresets.json
└── vcpkg.json
```

## 依存ライブラリ(vcpkg)

| ライブラリ | 役割 |
|---|---|
| [fluidsynth](https://vcpkg.link/ports/fluidsynth) | 合成エンジン層(`fluid_synth_t`)+音声出力(`fluid_audio_driver_t`) |
| [nlohmann-json](https://vcpkg.link/ports/nlohmann-json) | プロファイルJSON読み込み |

`vcpkg.json` に manifest として定義済み。vcpkgのツールチェインファイルを指定して
configureすれば自動的にインストールされる。

## ビルド

```powershell
# Windows — VS2022
cmake --preset windows-vs2022
cmake --build --preset windows-vs2022

# Windows — VS2026
cmake --preset windows-vs2026
cmake --build --preset windows-vs2026

# Linux (vcpkg)
cmake --preset linux-vcpkg
cmake --build --preset linux-vcpkg
```

直接指定する場合:

```powershell
cmake -B build -G "Visual Studio 17 2022" `
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

動作確認用スモークテストをビルドする場合は `-DBUILD_FITOMSF2IF_TEST=ON` を付ける。

### ビルド成果物

| ターゲット | 成果物 |
|---|---|
| `FitomSf2IF` | `FitomSf2IF.dll` / `libFitomSf2IF.so` — FITOM_X ロード対象 |
| `FitomSf2IF_test` | 動作確認用実行ファイル(オプション) |

## プロファイル (`sf2if_profile.json`)

探索優先順(`HWPlugin_Init(profile_path)`):

1. `profile_path` 引数
2. 環境変数 `SF2IF_PROFILE`
3. カレントディレクトリの `sf2if_profile.json`
4. 見つからない場合はデフォルト設定(下表の既定値)で動作

```jsonc
{
  "sample_rate": 44100,     // synth.sample-rate

  "gain":      0.5,         // synth.gain
  "polyphony": 256,         // synth.polyphony
  "reverb":    true,        // synth.reverb.active
  "chorus":    true,        // synth.chorus.active

  "audio_driver":      "",  // audio.driver。空文字でfluidsynthが自動選択
  "audio_device":      "",  // audio.<audio_driver>.device。空文字で既定デバイス
  "audio_period_size": 0,   // audio.period-size。0でfluidsynthの既定値のまま
  "audio_periods":     0    // audio.periods。0でfluidsynthの既定値のまま
}
```

`audio_driver`/`audio_period_size`/`audio_periods`はfluidsynthの設定名
(`audio.driver`/`audio.period-size`/`audio.periods`)にそのまま対応する。
有効なドライバ名はプラットフォームによって異なる(Windows: `wasapi`/`dsound`、
Linux: `alsa`/`pulseaudio`/`jack`、macOS: `coreaudio`等)。

## 現在の実装状況

[`docs/STATUS.md`](docs/STATUS.md) を参照。

## ライセンス

本リポジトリは [`LICENSE`](LICENSE) の通り **MIT License**。

[FluidSynth](https://www.fluidsynth.org/)(LGPL-2.1-or-later)は`vcpkg`(既定triplet)
経由で**動的リンク**しており、ビルド成果物には`FitomSf2IF.dll`とは別の共有ライブラリ
(`libfluidsynth-3.dll`等)として同梱される。FluidSynthのコード自体は本プラグインの
バイナリに組み込まれない(実行時に別DLLとしてロードされるだけ)ため、本リポジトリ
(ラッパーコード側)はLGPL-2.1の影響を受けずMITのままでよい。同様の理由でFITOM_X本体も
このDLLを`IHWPlugin` C API経由で実行時にロードするだけであり、引き続きMITのままでよい。

再配布時は`libfluidsynth-3.dll`(および同梱される依存ライブラリ)自体がLGPL-2.1-or-later
であることに変わりはなく、当該ライブラリのライセンス表示・差し替え可能性の確保等、
LGPL側の条件は別途遵守すること。
