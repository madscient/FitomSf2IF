// src/Sf2IfImpl.cpp
//
// IHWPlugin C API 実装本体。FITOM_X本体からは他のhwifプラグインと区別のつかない
// 1つのhwifインスタンスとして振る舞う(docs/sf2-fluidsynth-integration.md 4節①②参照)。
//
// fluid_synth_t インスタンスは全MPUで1つを共有する(同5節)。複数の`devices[]`
// エントリが`chip=="SF2"`を使った場合はFITOM_Xコア側が起動時エラーで停止する
// (4節⑥、2026年7月確定)ため、HWPlugin_Openはこのプラグインに対して実質1回しか
// 呼ばれない。呼ばれるたびに共有エンジンを指す軽量ハンドルを新規発行するだけの
// 実装で十分であり、複数ハンドル間の優先順位・排他は設けていない。

#include "fitom/IHWPlugin.h"

#include "FluidSynthEngine.h"
#include "MidiEventParser.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_set>
#include <vector>

// IHWPlugin.h はHWDeviceOpaqueを前方宣言のみ(不透明ポインタ)。実体はこのDLL内で
// 定義する。このプラグインは全ハンドルで共有エンジンを指すだけなので、状態は持たない。
struct HWDeviceOpaque {};

namespace {

constexpr const char* kProfileFileName   = "sf2if_profile.json";
constexpr const char* kProfileEnvVarName = "SF2IF_PROFILE";

// ─── プロファイル探索(FitomHwIF::PluginRegistry::load()と同じ優先順) ───────
//   1. HWPlugin_Init(profile_path) の引数
//   2. 環境変数 SF2IF_PROFILE
//   3. このDLLと同じディレクトリ
//   4. カレントディレクトリ
// いずれも見つからない場合はデフォルト設定(EngineConfig/AudioConfig既定値)で動作する。
std::filesystem::path resolveProfilePath(const char* profile_path_arg) {
    if (profile_path_arg && profile_path_arg[0] != '\0') {
        return profile_path_arg;
    }
    if (const char* env = std::getenv(kProfileEnvVarName); env && env[0] != '\0') {
        return env;
    }
    // TODO: DLL自身のディレクトリの取得(GetModuleHandle/dladdr)は未実装。
    // 現状はカレントディレクトリのみを探索する。
    if (std::filesystem::exists(kProfileFileName)) {
        return kProfileFileName;
    }
    return {};
}

struct PluginState {
    std::mutex mtx;
    bool initialized = false;
    bool offline = false;   // HWPlugin_InitOfflineで初期化した

    sf2if::FluidSynthEngine engine;
    std::unique_ptr<sf2if::MidiEventParser> parser;

    std::unordered_set<HWHandle> open_handles;
};

PluginState& state() {
    static PluginState s;
    return s;
}

bool loadProfile(const char* profile_path_arg, sf2if::EngineConfig& engine_cfg) {
    const std::filesystem::path profile_path = resolveProfilePath(profile_path_arg);
    if (!profile_path.empty()) {
        std::ifstream ifs(profile_path);
        if (!ifs) {
            return false; // 明示指定されたパスが開けない = 設定ミス、失敗を返す
        }
        nlohmann::json j;
        try {
            ifs >> j;
        } catch (const nlohmann::json::exception&) {
            return false;
        }
        engine_cfg.sample_rate       = j.value("sample_rate", engine_cfg.sample_rate);
        engine_cfg.polyphony         = j.value("polyphony", engine_cfg.polyphony);
        engine_cfg.gain              = j.value("gain", engine_cfg.gain);
        engine_cfg.reverb_active     = j.value("reverb", engine_cfg.reverb_active);
        engine_cfg.chorus_active     = j.value("chorus", engine_cfg.chorus_active);
        engine_cfg.audio_driver      = j.value("audio_driver", engine_cfg.audio_driver);
        engine_cfg.audio_device      = j.value("audio_device", engine_cfg.audio_device);
        engine_cfg.audio_period_size = j.value("audio_period_size", engine_cfg.audio_period_size);
        engine_cfg.audio_periods     = j.value("audio_periods", engine_cfg.audio_periods);
    }
    return true;
}

bool initEngine(const sf2if::EngineConfig& engine_cfg) {
    auto& s = state();
    if (!s.engine.init(engine_cfg)) {
        return false;
    }
    s.parser = std::make_unique<sf2if::MidiEventParser>(s.engine);
    return true;
}

char* dupString(const std::string& s) {
    char* out = new (std::nothrow) char[s.size() + 1];
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

} // namespace

extern "C" {

FITOM_HWP_API const char* FITOM_HWP_CALL HWPlugin_GetName() {
    return "FitomSf2IF";
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Init(const char* profile_path) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.initialized) {
        return s.offline ? HW_ERR_INVALID_ARG : HW_OK; // 同じモードなら冪等
    }
    sf2if::EngineConfig engine_cfg;
    if (!loadProfile(profile_path, engine_cfg) || !initEngine(engine_cfg)) {
        return HW_ERR_OPEN_FAILED;
    }
    s.initialized = true;
    return HW_OK;
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_InitOffline(const char* profile_path, uint32_t sample_rate) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.initialized) {
        return s.offline ? HW_OK : HW_ERR_INVALID_ARG; // 同じモードなら冪等
    }
    if (!sf2if::FluidSynthEngine::supportsSampleRate(sample_rate)) {
        return HW_ERR_INVALID_ARG;
    }
    sf2if::EngineConfig engine_cfg;
    if (!loadProfile(profile_path, engine_cfg)) {
        return HW_ERR_OPEN_FAILED;
    }
    engine_cfg.sample_rate = sample_rate;
    engine_cfg.offline     = true;
    if (!initEngine(engine_cfg)) {
        return HW_ERR_OPEN_FAILED;
    }
    s.initialized = true;
    s.offline     = true;
    return HW_OK;
}

FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_Shutdown() {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.initialized) {
        return;
    }
    // FluidSynthEngine::shutdown()内のdelete_fluid_audio_driver()がオーディオ
    // コールバックスレッドを同期的に停止・joinしてから戻る
    // (plugin-hwif.md「プラグイン全体のシャットダウン」節の要件を満たす)。
    // オフラインではオーディオドライバもスレッドも無い。
    s.parser.reset();
    s.engine.shutdown();
    s.open_handles.clear();
    s.initialized = false;
    s.offline     = false;
}

FITOM_HWP_API const char* FITOM_HWP_CALL HWPlugin_Enumerate() {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    // GUIの「デバイス一覧」にSF2は出さない方針(5節)だが、Enumerate自体はhwif契約上
    // 汎用のためプラグイン種別だけ返す。物理デバイスのような列挙対象は存在しない。
    const std::string json = s.initialized
        ? R"([{"type":"SF2","engine":"fluidsynth","index":0}])"
        : "[]";
    return dupString(json);
}

FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_FreeString(const char* str) {
    delete[] str;
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Open(const char* params_json, HWHandle* out_handle) {
    if (!out_handle) {
        return HW_ERR_INVALID_ARG;
    }
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.initialized) {
        // IHWPlugin.h: 「初期化前に他の関数を呼んだ場合の動作は未定義
        // （実装は失敗を返すこと）」
        return HW_ERR_INVALID_ARG;
    }

    // params_jsonの"soundfonts"配列(FITOM_Xコアがsf2_banks[].fileから重複除去して
    // 構築した絶対パス一覧)を読み取り、soundfont_index→sfont_idのキャッシュを
    // 構築する(4節⑥⑦、2026年7月確定)。他のフィールド(chip/type等)はこのプラグイン
    // には無関係なので無視する。"soundfonts"が無い/空でもエラーにしない
    // (`sf2_banks`未設定でSF2デバイスだけ用意する構成を許容する、4節⑦末尾)。
    if (params_json && params_json[0] != '\0') {
        try {
            const nlohmann::json j = nlohmann::json::parse(params_json);
            if (j.contains("soundfonts")) {
                s.engine.setSoundFontList(j.at("soundfonts").get<std::vector<std::string>>());
            }
        } catch (const nlohmann::json::exception&) {
            return HW_ERR_INVALID_ARG;
        }
    }

    // このプラグインは全ハンドルで共有エンジンを指すだけなので、ハンドル自体には
    // 状態を持たせない(不透明ポインタとして識別できれば十分)。
    HWHandle handle = new (std::nothrow) HWDeviceOpaque();
    if (!handle) {
        return HW_ERR_OPEN_FAILED;
    }
    *out_handle = handle;
    s.open_handles.insert(handle);
    return HW_OK;
}

FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_Close(HWHandle handle) {
    if (!handle) return;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.open_handles.erase(handle) == 0) {
        return; // 未知のハンドル
    }
    delete handle;
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Write(HWHandle handle, uint16_t /*addr*/, uint8_t data) {
    // このプラグインにとって意味のある単位はMIDIメッセージ(複数バイト)であり、
    // 単一バイトのWriteは本来WriteBlockの1バイト版として扱う以上の意味を持たない。
    return HWPlugin_WriteBlock(handle, 0, &data, 1);
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_WriteBlock(
    HWHandle handle, uint8_t /*startAddr*/, const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return HW_ERR_INVALID_ARG;
    }
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.initialized || s.open_handles.find(handle) == s.open_handles.end()) {
        return HW_ERR_INVALID_ARG;
    }
    // startAddrは無視し、dataを生MIDIバイト列としてそのまま渡す
    // (docs/sf2-fluidsynth-integration.md 4節②参照)。
    s.parser->parseBlock(data, len);
    return HW_OK;
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Reset(HWHandle handle, unsigned int /*pulse_us*/) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.initialized || s.open_handles.find(handle) == s.open_handles.end()) {
        return HW_ERR_INVALID_ARG;
    }
    s.engine.systemReset(); // fluid_synth_system_reset相当(全ノートオフ+GMリセット)
    return HW_OK;
}

FITOM_HWP_API int FITOM_HWP_CALL HWPlugin_GetClock(HWHandle /*handle*/) {
    return 0; // SF2/fluidsynthにFMチップのようなマスタークロックの概念は無い
}

FITOM_HWP_API int FITOM_HWP_CALL HWPlugin_GetPanpot(HWHandle /*handle*/) {
    return 0; // ステレオ固定(fluid_synth_write_floatの2ch出力をそのまま使う)
}

FITOM_HWP_API bool FITOM_HWP_CALL HWPlugin_IsOpen(HWHandle handle) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    return s.initialized && s.open_handles.find(handle) != s.open_handles.end();
}

FITOM_HWP_API uint32_t FITOM_HWP_CALL HWPlugin_GetLatencySamples(HWHandle /*handle*/) {
    // FMエンジン内蔵hwif(FitomEmuIF等)と同じ理由: 自前のオーディオバッファサイズが
    // そのままレイテンシになる(docs/plugin-hwif.md「レイテンシ同期」節参照)。
    // オフラインではオーディオドライバが無いので0になる(契約どおり)。
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    return s.initialized ? s.engine.latencySamples() : 0;
}

FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_SetDelaySamples(HWHandle /*handle*/, uint32_t /*delay_samples*/) {
    // 自身のレイテンシ(バッファサイズ)と一致するよう構成済みのためno-op
    // (FMエンジン内蔵hwifと同じ理由、plugin-hwif.md参照)。
}

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Render(float* out, uint32_t frames) {
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!out || !s.initialized || !s.offline) {
        return HW_ERR_INVALID_ARG;
    }
    // 全ハンドルが1つのfluid_synth_tを共有しているので、その出力がそのまま全デバイスの
    // ミックスになる。
    return s.engine.render(out, frames) ? HW_OK : HW_ERR_IO;
}

} // extern "C"
