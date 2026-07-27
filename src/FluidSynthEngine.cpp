#include "FluidSynthEngine.h"

#include <fluidsynth.h>

namespace sf2if {

FluidSynthEngine::~FluidSynthEngine() {
    shutdown();
}

bool FluidSynthEngine::init(const EngineConfig& config) {
    if (synth_ != nullptr) {
        return true; // 既に初期化済み
    }
    config_ = config;

    settings_ = new_fluid_settings();
    if (!settings_) {
        return false;
    }
    fluid_settings_setnum(settings_, "synth.sample-rate", config_.sample_rate);
    fluid_settings_setint(settings_, "synth.polyphony", static_cast<int>(config_.polyphony));
    fluid_settings_setnum(settings_, "synth.gain", config_.gain);
    fluid_settings_setint(settings_, "synth.reverb.active", config_.reverb_active ? 1 : 0);
    fluid_settings_setint(settings_, "synth.chorus.active", config_.chorus_active ? 1 : 0);

    // MIDI受信層(fluid_midi_driver_t/fluid_player_t)は使わない。生MIDIバイト列は
    // MidiEventParserがパースしこのクラスのnoteOn等を直接呼ぶ(2節参照)。
    // 音声デバイス層(fluid_audio_driver_t)はこのプラグインが自己完結するために使う
    // (他のhwifプラグインと同じく1台の楽器として振る舞う、4節②参照)。
    if (!config_.audio_driver.empty()) {
        fluid_settings_setstr(settings_, "audio.driver", config_.audio_driver.c_str());
    }
    if (!config_.audio_device.empty() && !config_.audio_driver.empty()) {
        const std::string key = "audio." + config_.audio_driver + ".device";
        fluid_settings_setstr(settings_, key.c_str(), config_.audio_device.c_str());
    }
    if (config_.audio_period_size > 0) {
        fluid_settings_setint(settings_, "audio.period-size", static_cast<int>(config_.audio_period_size));
    }
    if (config_.audio_periods > 0) {
        fluid_settings_setint(settings_, "audio.periods", static_cast<int>(config_.audio_periods));
    }

    synth_ = new_fluid_synth(settings_);
    if (!synth_) {
        delete_fluid_settings(settings_);
        settings_ = nullptr;
        return false;
    }

    audio_driver_ = new_fluid_audio_driver(settings_, synth_);
    if (!audio_driver_) {
        delete_fluid_synth(synth_);
        synth_ = nullptr;
        delete_fluid_settings(settings_);
        settings_ = nullptr;
        return false;
    }
    return true;
}

void FluidSynthEngine::shutdown() {
    if (audio_driver_) {
        // 内部のオーディオコールバックスレッドを同期的に停止・joinしてから返る。
        delete_fluid_audio_driver(audio_driver_);
        audio_driver_ = nullptr;
    }
    if (synth_) {
        delete_fluid_synth(synth_);
        synth_ = nullptr;
    }
    if (settings_) {
        delete_fluid_settings(settings_);
        settings_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(sfont_cache_mtx_);
    sfont_cache_.clear();
}

void FluidSynthEngine::noteOn(int chan, int key, int vel) {
    if (!synth_) return;
    fluid_synth_noteon(synth_, chan, key, vel);
}

void FluidSynthEngine::noteOff(int chan, int key) {
    if (!synth_) return;
    fluid_synth_noteoff(synth_, chan, key);
}

void FluidSynthEngine::controlChange(int chan, int ctrl, int value) {
    if (!synth_) return;
    fluid_synth_cc(synth_, chan, ctrl, value);
}

void FluidSynthEngine::programChange(int chan, int prog) {
    if (!synth_) return;
    fluid_synth_program_change(synth_, chan, prog);
}

void FluidSynthEngine::pitchBend(int chan, int value14bit) {
    if (!synth_) return;
    fluid_synth_pitch_bend(synth_, chan, value14bit);
}

void FluidSynthEngine::channelPressure(int chan, int value) {
    if (!synth_) return;
    fluid_synth_channel_pressure(synth_, chan, value);
}

void FluidSynthEngine::keyPressure(int chan, int key, int value) {
    if (!synth_) return;
    fluid_synth_key_pressure(synth_, chan, key, value);
}

void FluidSynthEngine::systemReset() {
    if (!synth_) return;
    fluid_synth_system_reset(synth_);
}

int FluidSynthEngine::loadSoundFontCached(const std::string& file) {
    std::lock_guard<std::mutex> lock(sfont_cache_mtx_);
    auto it = sfont_cache_.find(file);
    if (it != sfont_cache_.end()) {
        return it->second;
    }
    int sfont_id = fluid_synth_sfload(synth_, file.c_str(), /*reset_presets=*/0);
    if (sfont_id == FLUID_FAILED) {
        return -1;
    }
    sfont_cache_.emplace(file, sfont_id);
    return sfont_id;
}

bool FluidSynthEngine::selectProgram(int chan, const std::string& file, int sf2_bank, int prog) {
    if (!synth_) return false;
    const int sfont_id = loadSoundFontCached(file);
    if (sfont_id < 0) {
        // ④節:「該当presetが実在しない場合」はfluidsynth自身の挙動に委ねる方針だが、
        // ファイル自体のロード失敗はそれ以前の話なので単純に失敗を返す。
        return false;
    }
    return fluid_synth_program_select(synth_, chan, sfont_id, sf2_bank, prog) == FLUID_OK;
}

unsigned int FluidSynthEngine::latencySamples() const {
    if (!settings_) return 0;
    int period_size = 0;
    int periods = 0;
    fluid_settings_getint(settings_, "audio.period-size", &period_size);
    fluid_settings_getint(settings_, "audio.periods", &periods);
    if (period_size <= 0 || periods <= 0) return 0;
    return static_cast<unsigned int>(period_size) * static_cast<unsigned int>(periods);
}

} // namespace sf2if
