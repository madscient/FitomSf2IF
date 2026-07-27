# STATUS.md — 実装状況

作業を始める前にこのファイルを読むこと。プロジェクトの静的な説明は`README.md`、
他リポジトリとの関係・コーディング規約は`CLAUDE.md`を参照。

設計の正本は FITOM_X リポジトリ側のドキュメント
[`docs/sf2-fluidsynth-integration.md`](../../FITOM_X/docs/sf2-fluidsynth-integration.md)。
以下は同ドキュメントのうち**決定済みの部分**を実装したスケルトン段階の状況。

## 実装済み

- IHWPlugin C API 一式(`src/Sf2IfImpl.cpp`): `HWPlugin_GetName/Init/Shutdown/
  Enumerate/FreeString/Open/Close/Write/WriteBlock/Reset/GetClock/GetPanpot/
  IsOpen/GetLatencySamples/SetDelaySamples`
- 標準MIDIチャンネルボイスメッセージ(Note On/Off, CC, Program Change,
  Pitch Bend, Channel/Poly Pressure)の`fluid_synth_t`への中継
  (`src/MidiEventParser.cpp`)
- SoundFontロードキャッシュ(`FluidSynthEngine::selectProgram`、file path→sfont_id)
- `fluid_audio_driver_t`による自己完結オーディオ出力
  (2026年7月、RtAudio依存を外してfluidsynth内蔵の音声デバイス層へ切替。
  詳細はCLAUDE.md「設計上の要点」参照)
- `HWPlugin_Shutdown`での同期停止(`delete_fluid_audio_driver`)
- プロファイルJSON読み込み(`sf2if_profile.json`)、CMake/vcpkgによるビルド一式
  (`cmake --preset windows-vs2022`でconfigure→ビルド→スモークテスト実行まで
  動作確認済み)

## 未実装・意図的に保留中(FITOM_X本体側の対応する実装が固まるまで)

同ドキュメント5節「残る検討事項」に基づく:

- **バンク選択を伝えるプライベートSysEx(sub-cmd 0x05案)**: `{chan, file, sf2_bank,
  prog}`を1メッセージにまとめる方針は決定済みだが、バイト列レイアウトは未確定。
  `src/MidiEventParser.cpp`の`dispatchSystemExclusive()`に受信側の骨格のみ用意し、
  `FluidSynthEngine::selectProgram()`は実装済み(呼び出し側の配線が未完了)。
- **`Sf2BankRegistry`(CC#32→file/sf2_bank解決)**: FITOM_Xコア側に新設する設計のため、
  本リポジトリのスコープ外。
- **複数の`devices[]`エントリが同じプラグインを使う場合の扱い**: 未確定。現状は
  `HWPlugin_Open`を呼ぶたびに共有エンジンを指す軽量ハンドルを新規発行するのみで、
  優先順位・排他制御は設けていない。
- **マスターボリューム/マスターピッチのSF2出力への適用**: 未検討のため未実装。
- **GUI(`sf2_channel_windows`編集ダイアログ等)**: FITOM_X本体側のスコープ。

## その他の既知の課題

- **`plugin_sdk/include/fitom/IHWPlugin.h`の同期問題**: FITOM_Xの
  `plugin_sdk/include/fitom/IHWPlugin.h`と内容を同一に保つ必要があるが、現状は
  独立コピーとして手動管理している(FitomEmuIFと同じ既知の課題)。
- **実機検証は未実施**: 実際のSoundFont・実オーディオデバイスでの音声確認は未実施
  (スモークテストによるAPI呼び出しフローの疎通確認のみ)。
