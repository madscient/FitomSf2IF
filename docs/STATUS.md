# STATUS.md — 実装状況

作業を始める前にこのファイルを読むこと。プロジェクトの静的な説明は`README.md`、
他リポジトリとの関係・コーディング規約は`CLAUDE.md`を参照。

設計の正本は FITOM_X リポジトリ側のドキュメント
[`docs/sf2-fluidsynth-integration.md`](../../FITOM_X/docs/sf2-fluidsynth-integration.md)。
**2026年7月、同ドキュメントの設計が確定**し、以下は確定内容を実装した状況。

## 実装済み

- IHWPlugin C API 一式(`src/Sf2IfImpl.cpp`): `HWPlugin_GetName/Init/Shutdown/
  Enumerate/FreeString/Open/Close/Write/WriteBlock/Reset/GetClock/GetPanpot/
  IsOpen/GetLatencySamples/SetDelaySamples/InitOffline/Render`
  - 任意の組のうち、部位ごとのゲイン(`HWPlugin_GetPartCount`ほか3関数)と
    `HWPlugin_SetMasterVolume`はエクスポートしない。FITOM_Xは`chip:"SF2"`の
    デバイスに`SetMasterVolume`を呼ばず、マスターボリュームはSysExで送ってくる
    (FITOM_X `docs/interfaces/plugin-hwif.md`「SoundFont プラグインへの追加の約束」節)。
- **オフライン描画(`HWPlugin_InitOffline`/`HWPlugin_Render`)**: FITOM_Xの`fitom_render`
  (SMF→WAV変換)用。`fluid_audio_driver_t`を作らず、`HWPlugin_Render`で
  `fluid_synth_write_float()`を呼ぶ。サンプルレートは引数がプロファイルより優先、
  `GetLatencySamples`は0、出力はクリップしない。範囲外のサンプルレートは
  fluidsynthの`synth.sample-rate`の範囲を問い合わせて`HW_ERR_INVALID_ARG`にする。
  `tests/offline_render_test.cpp`(CTest登録、音声デバイス不要)で確認済み。
  契約との差は下の「その他の既知の課題」を参照。
- 標準MIDIチャンネルボイスメッセージ(Note On/Off, CC, Program Change,
  Pitch Bend, Channel/Poly Pressure)の`fluid_synth_t`への中継
  (`src/MidiEventParser.cpp`)。CC#0/CC#32(バンクセレクト)は読み捨てる(下記
  sub-cmd 0x05で代替)。
- **バンク選択プライベートSysEx(sub-cmd 0x05、設計ドキュメント4節④⑤で確定)**:
  `F0 00 48 01 05 <chan> <soundfont_index> <sf2_bank_msb> <sf2_bank_lsb> <prog> F7`
  を受信し`FluidSynthEngine::selectProgramByIndex()`→`fluid_synth_program_select()`
  へ配線済み(`src/MidiEventParser.cpp`)。
- **SoundFont一覧の受け渡し(4節⑥⑦で確定)**: `HWPlugin_Open()`の`params_json`の
  `"soundfonts"`配列(絶対パス一覧、FITOM_Xコアが`sf2_banks[].file`から重複除去して
  構築)を受け取り、その場で全ファイルを`fluid_synth_sfload()`してキャッシュする
  (`FluidSynthEngine::setSoundFontList()`)。個別ファイルのロード失敗はそのインデックス
  だけ無効化し、他のインデックスの処理は継続する。
- **マスターボリューム/マスターファインチューニング(4節②で確定、GM2 Universal
  Realtime SysEx `04 01`/`04 03`をFITOM_Xコアがそのまま転送)**:
  `MidiEventParser::dispatchSystemExclusive()`で受信し、`fluid_synth_set_gain()`
  (ボリューム、プロファイルの`gain`を100%として比例)/`fluid_synth_set_gen()`with
  `GEN_FINETUNE`(全16chへ、ファインチューニング)へ変換して適用する。マスター
  コースチューニング(`04 04`)はFITOM_X側で使わない設計のため未対応(4節②末尾)。
- SoundFontロードキャッシュ(`FluidSynthEngine`内部、file path→sfont_id)
- `fluid_audio_driver_t`による自己完結オーディオ出力(2026年7月、RtAudio依存を
  外してfluidsynth内蔵の音声デバイス層へ切替。詳細はCLAUDE.md「設計上の要点」参照)
- `HWPlugin_Shutdown`での同期停止(`delete_fluid_audio_driver`)
- プロファイルJSON読み込み(`sf2if_profile.json`)、CMake/vcpkgによるビルド一式
  (`cmake --preset windows-vs2022`でconfigure→ビルド→スモークテスト実行まで
  動作確認済み。スモークテストはsub-cmd 0x05・マスターボリューム/ファインチューニング
  の送出も含む)

## 本リポジトリのスコープ外(FITOM_Xコア側の実装)

- **`Sf2BankRegistry`(CC#32→soundfont_index/sf2_bank解決、`soundfonts`一覧構築)**:
  FITOM_Xコア側に新設される(4節④⑦)。
- **`devices[]`/`hw_plugins[]`での`chip:"SF2"`登録、複数`devices[]`エントリの
  起動時エラー化**: FITOM_Xコア側(4節⑥)。本プラグインは「実質1回しかOpenされない」
  という前提に乗るだけで、複数ハンドルの排他制御自体は実装していない。
- **`sf2_channel_windows`編集用GUIダイアログ**: FITOM_X本体側のスコープ(5節)。

## その他の既知の課題

- **`plugin_sdk/include/fitom/IHWPlugin.h`の同期問題**: FITOM_Xの
  `plugin_sdk/include/fitom/IHWPlugin.h`と内容を同一に保つ必要があるが、現状は
  独立コピーとして手動管理している(FitomEmuIFと同じ既知の課題)。
  最後に同期したのはFITOM_Xのコミット`faaf183`の版(部位ゲイン・
  マスターボリューム・オフライン描画の任意関数を含む)。
- **オフライン描画の書き込みの反映は64フレーム単位**: 契約は「`HWPlugin_Render`の
  呼び出しより前の`Write`/`WriteBlock`は、描画する最初のフレームから反映する」だが、
  fluidsynthは64フレーム(`FLUID_BUFSIZE`)単位で合成し、前回の描画で余ったフレームを
  次の描画の先頭で返すため、書き込みは次の64フレーム境界から反映される。遅れは
  `(64 - それまでの描画総数 % 64) % 64`フレーム(0〜63)。FITOMは1ms分ずつ描画するので、
  48kHzでは0/16/32/48フレーム(最大1ms)、44.1kHzでは0〜63フレーム(最大約1.4ms)の
  揺れになる。
  確認済み: `tests/offline_render_test.cpp`の`testWriteTiming`が、それまでの描画の
  分け方(44/45/48/64/128/100フレームの1回、48フレームの2〜4回、44+44+45フレーム)を
  変えて発音の遅れを計測し、上の式と一致した(fluidsynth 2.5.5)。fluidsynthの公開APIにはブロック内の時刻を指定してイベントを
  与える手段が無いため、本プラグイン側では解消できない。最初の描画より前の書き込みは
  先頭から反映される。契約を緩めるか、この差を受け入れるかはFITOM_X側の判断待ち。
- **実機検証は未実施**: 実際のSoundFontファイル・実オーディオデバイスを使った
  音声そのものの確認は未実施(スモークテストによるAPI呼び出しフロー・パース処理の
  疎通確認のみ。soundfontsは空配列でテストしている)。
- **マスターファインチューニングの実装方式は実装側の判断**: 設計ドキュメントが
  「fluidsynthがこれらの標準SysExをどう処理するかはラッパー側の実装詳細」と
  明記している(4節②)ため、`GEN_FINETUNE`を全16chへ設定する方式を採用したが、
  他の実装(例: 個別チャンネルのpitch bendレンジ調整等)の方が適切な可能性もあり、
  実機での聴感確認は未実施。
