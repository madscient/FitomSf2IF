# CLAUDE.md

このファイルは Claude Code(または他のセッション)がこのリポジトリで作業する際に
参照する引継ぎ情報です。

作業を始める前に、必ず [`docs/STATUS.md`](docs/STATUS.md)(実装状況・未実装項目)を
先に読んでください。`README.md`は外部向けの静的なプロジェクト説明(現状を含まない)。

## コミュニケーション

- ユーザーへの報告・応答は日本語で行うこと。

## プロジェクト概要

**FitomSf2IF** は FITOM_X の `IHWPlugin` C API を実装する FluidSynth(SF2サウンド
フォント再生)統合 hwif プラグイン(Windows/Linux、C++17)。設計の経緯・決定事項は
FITOM_X側のドキュメントが正本であり、本リポジトリのREADME.mdはそれを前提にした
実装リポジトリ側の構成・ビルド方法のみを記す。

## 他リポジトリとの関係(重要)

関連リポジトリ(同じ `source/repos` 配下、参照用):

- **`../FITOM_X`** — FITOM_X本体。以下が正本:
  - `docs/sf2-fluidsynth-integration.md` — このプラグインの設計検討ドキュメント(必読)。
    ステータス・未決事項は5節「残る検討事項」を参照。
  - `plugin_sdk/include/fitom/IHWPlugin.h` — hwifプラグインC API契約の正本。
    `plugin_sdk/include/fitom/IHWPlugin.h`(このリポジトリ内)はその独立コピー。
  - `config_schema/profile.schema.json` — `sf2_banks`/`sf2_channel_windows` のスキーマ
    (実装済み、FITOM_Xコア側のローダーは未実装)。
  - `docs/plugin-hwif.md` — hwifプラグイン要件定義(共通契約)。
  - `docs/manuals/midi-message-reference.md` 8.1節 — プライベートSysEx
    (マニュファクチャラID `00 48 01`)の既存フォーマット。sub-cmd 0x05(バンク選択)は
    この枠組みに新設される想定だがバイト列は未確定。

**`source/repos`配下の他リポジトリ(`../FITOM_X`を含む)のファイルは、たとえ原因や
修正箇所がそちら側にあると判明しても、ユーザーの明示的な許可なく勝手に編集しない
こと。** 調査・参照は自由に行ってよいが、実際にコードを書き換える前に必ずユーザーに
確認を取る。

## 設計上の要点(要約、詳細は `../FITOM_X/docs/sf2-fluidsynth-integration.md` 参照)

- `fluid_synth_t`(合成エンジン層)をリンクする。FluidSynthのMIDI受信層
  (`fluid_midi_driver_t`/`fluid_player_t`)は使わない。FluidSynth本体は無改造。
- **音声出力はFluidSynth自身のaudio driver層(`fluid_audio_driver_t`)をそのまま使う**
  (2026年7月、依存削減のためRtAudioを外して変更。旧設計ドキュメントの「音声デバイス
  層も使わない」という記述は、FITOM_X本体が直接fluidsynthを使役する前提での懸念
  ―複数の独立オーディオストリーム間のクロックずれ・OSレベルミキシングの必要性・
  マスターボリューム/ピッチの一括制御不能―を根拠にしていたが、fluidsynthをプラグインへ
  完全に分離した現在はこの懸念が当てはまらない。ユーザーの指示により本体ドキュメント側も
  改訂予定)。
- `fluid_synth_t` インスタンスは全MPUで1つを共有する。
- MPU×ch → fluidsynth chan(0-15)の「窓」変換はFITOM_Xコア側の責務。本プラグインが
  `HWPlugin_WriteBlock`で受け取る生MIDIバイト列は、チャンネルニブルが既に
  fluidsynth chan として解決済みの前提で実装している。
- CC#0(バンクセレクトMSB)・CC#32(バンクセレクトLSB)は標準CCとして転送されない
  想定(コア側がプライベートSysExへ変換して送る)。生バイト列として届いた場合は
  読み捨てる(`MidiEventParser::dispatchChannelVoiceMessage`)。
- `Sf2BankRegistry`(CC#32インデックス→{file, sf2_bank})はFITOM_Xコア側に新設される
  設計であり、本リポジトリのスコープ外。

## 未解決の TODO・現在の実装状況

[`docs/STATUS.md`](docs/STATUS.md) を参照(実装済み/未実装の一覧を一元管理。
このファイルとの重複記載は避けること)。

## 重要な命名規則・変更してはいけないもの

| 項目 | 扱い |
|---|---|
| `plugin_sdk/include/fitom/IHWPlugin.h` | FITOM_Xの正本の独立コピー。内容は同期させる必要があるが、ファイル名・パス自体は変更しない |
| ターゲット名 | `FitomSf2IF`(CMakeターゲット名・DLL名) |
| プロファイルファイル名 `sf2if_profile.json` / 環境変数 `SF2IF_PROFILE` | プロジェクト名`FitomSf2IF`に合わせた名称 |

## コーディング規約

- **名前空間**: プラグイン固有ロジックは `sf2if::` に統一
- **C++標準**: C++17
- **DLLエクスポート**: `extern "C"` + `FITOM_HWP_API` + `FITOM_HWP_CALL` を必ず付ける
  (`HWPlugin_*`関数のみ。内部クラスは通常のC++リンケージ)
- **スレッドセーフ**: `HWPlugin_Write`/`HWPlugin_WriteBlock`は必ずスレッドセーフにする
  (現状は`PluginState::mtx`で単純に直列化している)
- **`HWPlugin_Shutdown`**: `delete_fluid_audio_driver()`がオーディオコールバック
  スレッドを同期的に停止・joinしてから戻ることを前提にしている(Windowsのローダー
  ロック問題を避けるため、`docs/plugin-hwif.md`「プラグイン全体のシャットダウン」
  節参照)。この前提が崩れていないか(fluidsynthのバージョンアップ等で挙動が変わって
  いないか)は将来的に要確認。
