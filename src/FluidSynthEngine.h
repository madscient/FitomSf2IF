#pragma once
// src/FluidSynthEngine.h
//
// fluid_synth_t を薄くラップする。
// 合成エンジン層(fluid_synth_t)に加え、fluidsynth自身が提供する音声デバイス層
// (fluid_audio_driver_t)を使う。MIDI受信層(fluid_midi_driver_t/fluid_player_t)
// のみ使わない(生MIDIバイト列はHWPlugin_WriteBlock経由でMidiEventParserが
// パースしてこのクラスへ渡すため、fluidsynth側のMIDIパーサは不要)。
//
// 音声出力を自前のRtAudio等で持たずfluidsynth内蔵のaudio driverへ委譲するのは、
// fluidsynthをプラグインへ完全に分離した結果、依存ライブラリを増やす理由が
// 無くなったため(2026年7月、設計ドキュメント側の記述更新に伴う変更)。
//
// fluid_synth_t インスタンスは全MPUで1つを共有する
// (docs/sf2-fluidsynth-integration.md 5節)。
// MPU×ch → fluidsynth chan の変換はFITOM_Xコア側の責務であり、このクラスに
// 渡される chan は既に 0-15 の fluidsynth chan として解決済みの値とする。

#include <fluidsynth.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sf2if {

struct EngineConfig {
    unsigned int sample_rate   = 44100;
    unsigned int polyphony     = 256;   // fluidsynth既定値と同じ。synth.polyphony
    double       gain          = 0.5;   // synth.gain (0.0-10.0, fluidsynth既定0.2相当を少し上げた無難値)
    bool         reverb_active = true;
    bool         chorus_active = true;

    // ─── audio.* (fluid_audio_driver_t) ────────────────────────────────────
    // 空文字/0は「fluidsynthの既定値をそのまま使う」ことを意味する
    // (audio.driverは空文字だとコンパイル時に有効な最初のドライバへ自動選択される、
    //  audio.period-size/audio.periodsも未設定ならプラットフォーム別既定値になる)。
    std::string  audio_driver;        // 例: "wasapi"/"dsound"/"alsa"/"pulseaudio"/"coreaudio"
    std::string  audio_device;        // audio.<driver>.device へ設定(空文字で既定デバイス)
    unsigned int audio_period_size = 0; // audio.period-size (0 = fluidsynth既定値)
    unsigned int audio_periods     = 0; // audio.periods (0 = fluidsynth既定値)
};

// fluid_synth_t + fluid_audio_driver_t の生成・破棄・SoundFontロードキャッシュ・
// MIDIイベント中継を担う。
// スレッドセーフ: noteOn等の書き込み系はMIDI処理スレッドから呼ばれる。
// fluid_synth_t自体はスレッドセーフ(fluidsynth公式ドキュメント準拠)なため、
// sfont_idキャッシュ(map)のみを内部mutexで保護する。
// オーディオコールバックはfluid_audio_driver_t内部のスレッドで完結しており、
// このクラスから直接触れることはない。
class FluidSynthEngine {
public:
    FluidSynthEngine() = default;
    ~FluidSynthEngine();

    FluidSynthEngine(const FluidSynthEngine&)            = delete;
    FluidSynthEngine& operator=(const FluidSynthEngine&) = delete;

    // fluid_synth_t生成 + fluid_audio_driver_t起動までを行う。
    bool init(const EngineConfig& config);

    // HWPlugin_Shutdownから同期的に呼ばれる想定。delete_fluid_audio_driver()は
    // 内部のオーディオコールバックスレッドを同期的に停止・joinしてから返る
    // (docs/plugin-hwif.md「プラグイン全体のシャットダウン」節の要件を満たす)。
    void shutdown();
    bool isInitialized() const { return synth_ != nullptr; }

    // ─── 生MIDIバイト列相当のイベント(MidiEventParserから呼ばれる) ──────────
    void noteOn(int chan, int key, int vel);
    void noteOff(int chan, int key);
    void controlChange(int chan, int ctrl, int value);
    void programChange(int chan, int prog);
    void pitchBend(int chan, int value14bit);
    void channelPressure(int chan, int value);
    void keyPressure(int chan, int key, int value);
    void systemReset();

    // ─── SF2ファイル一覧(HWPlugin_Openのparams_json、⑥⑦節参照) ─────────────
    // FITOM_Xコアが`sf2_banks[].file`から重複除去して構築した絶対パス一覧を
    // params_jsonの"soundfonts"配列としてHWPlugin_Open()時に一度だけ渡してくる。
    // 各ファイルをこの場で(fluid_synth_sfload()により)ロードし、配列の添字
    // (soundfont_index)→sfont_idのマップを構築する。個々のファイルのロードに
    // 失敗しても他のエントリの処理は継続する(該当インデックスへのsfont_idは-1の
    // ままとし、selectProgramByIndex()は以降そのインデックスに対して黙って
    // 失敗する。④節末尾のステートレス方針を踏襲)。
    void setSoundFontList(const std::vector<std::string>& files);

    // ─── SF2直行パスのバンク選択(私的SysEx sub-cmd 0x05相当、④⑤節参照) ─────
    // soundfont_index: setSoundFontList()で渡した配列の添字(0起算)。
    // sf2_bank: 0-128(128はSF2慣習のパーカッションバンク)。
    bool selectProgramByIndex(int chan, int soundfont_index, int sf2_bank, int prog);

    // ─── マスターボリューム/ファインチューニング(GM2 Universal Realtime SysEx、
    //      4節②参照。FITOM_Xコアが8節のバイト列をそのままHWPlugin_WriteBlock経由で
    //      転送してくる) ──────────────────────────────────────────────────────
    // valueMsb: マスターボリュームSysEx(`04 01`)のmmバイト(0-127)。FITOM_X本体
    //           自身の解釈(8節「7bit精度で反映」)に合わせ、llバイトは使わない。
    void setMasterVolume(int valueMsb);
    // value14: マスターファインチューニングSysEx(`04 03`)のll/mmを14bit化した値
    //          (0-16383、8192=center=ピッチ変化なし)。
    void setMasterFineTuning(int value14);

    unsigned int sampleRate() const { return config_.sample_rate; }

    // audio.period-size * audio.periods (実際にfluidsynthへ設定された値を
    // fluid_settings_getint()で読み戻して計算する。設定を明示しなかった項目は
    // プラットフォーム別のfluidsynth既定値になる)。
    unsigned int latencySamples() const;

private:
    int loadSoundFontCached(const std::string& file);

    fluid_settings_t*     settings_     = nullptr;
    fluid_synth_t*        synth_        = nullptr;
    fluid_audio_driver_t* audio_driver_ = nullptr;
    EngineConfig           config_{};

    std::mutex sfont_cache_mtx_;
    std::unordered_map<std::string, int> sfont_cache_;   // file path -> sfont_id
    std::vector<int> soundfont_ids_by_index_;             // soundfont_index -> sfont_id (-1 = load失敗)
};

} // namespace sf2if
