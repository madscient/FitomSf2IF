// tests/offline_render_test.cpp
//
// HWPlugin_InitOffline / HWPlugin_Render(オフライン描画)の試験。
// 音声デバイスを使わないので、どの環境でも同じ結果になる。
//
// リポジトリにSoundFontを置かない方針(.gitignore)のため、1kHzの正弦波を1つだけ
// 持つ最小のSF2を一時ディレクトリへ生成して使う。音色の遅延・アタックは0にして
// あるので、ノートオンは最初の描画フレームから音になる。プロファイルもリバーブ・
// コーラスを切ったものを一時ディレクトリへ書く(無音が厳密に0.0fになる)。
//
// プラグインの状態はプロセスに1つなので、各試験はHWPlugin_Shutdownで終えて次の
// 試験が初期化し直す(契約上は1プロセス1回の初期化だが、本実装は再初期化できる)。

#include "fitom/IHWPlugin.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

// ─── 最小SF2の生成 ──────────────────────────────────────────────────────────

constexpr uint32_t kSf2SampleRate   = 48000;
constexpr uint32_t kSinePeriod      = 48;      // 48000 / 48 = 1kHz (ルートキー60で発音したとき)
constexpr uint32_t kSampleLength    = kSinePeriod * 10;
constexpr uint32_t kLoopStart       = kSinePeriod;
constexpr uint32_t kLoopEnd         = kSinePeriod * 9;
constexpr uint8_t  kRootKey         = 60;

class Bytes {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { u8(static_cast<uint8_t>(v & 0xFF)); u8(static_cast<uint8_t>(v >> 8)); }
    void u32(uint32_t v) { u16(static_cast<uint16_t>(v & 0xFFFF)); u16(static_cast<uint16_t>(v >> 16)); }
    void fourcc(const char* id) { for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(id[i])); }
    // 固定長の名前欄(SF2のachName等)。余りは0で埋める。
    void name(const char* s, size_t field_len) {
        const size_t len = std::strlen(s);
        for (size_t i = 0; i < field_len; ++i) u8(i < len ? static_cast<uint8_t>(s[i]) : 0);
    }
    void append(const std::vector<uint8_t>& b) { buf_.insert(buf_.end(), b.begin(), b.end()); }
    const std::vector<uint8_t>& data() const { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

std::vector<uint8_t> riffChunk(const char* id, const std::vector<uint8_t>& body) {
    Bytes b;
    b.fourcc(id);
    b.u32(static_cast<uint32_t>(body.size()));
    b.append(body);
    if (body.size() & 1) b.u8(0);
    return b.data();
}

std::vector<uint8_t> riffList(const char* type, const std::vector<std::vector<uint8_t>>& chunks) {
    Bytes b;
    b.fourcc(type);
    for (const auto& c : chunks) b.append(c);
    return riffChunk("LIST", b.data());
}

// SF2のzstr: 0終端を含めて偶数長
std::vector<uint8_t> zstr(const char* s) {
    Bytes b;
    b.name(s, (std::strlen(s) + 2) & ~size_t{1});
    return b.data();
}

std::vector<uint8_t> generatorRecord(uint16_t oper, uint16_t amount) {
    Bytes b;
    b.u16(oper);
    b.u16(amount);
    return b.data();
}

std::vector<uint8_t> buildSineSf2() {
    // SF2 2.01 8.1.2 の generator 番号
    constexpr uint16_t kGenDelayVolEnv  = 33;
    constexpr uint16_t kGenAttackVolEnv = 34;
    constexpr uint16_t kGenInstrument   = 41;
    constexpr uint16_t kGenSampleId     = 53;
    constexpr uint16_t kGenSampleModes  = 54;
    // timecentsの最小値は「時間0」の意味。既定の-12000は約1msの遅延・アタックになる。
    constexpr uint16_t kZeroTime        = 0x8000;

    Bytes ifil;
    ifil.u16(2);
    ifil.u16(1);
    const auto info = riffList("INFO", {
        riffChunk("ifil", ifil.data()),
        riffChunk("isng", zstr("EMU8000")),
        riffChunk("INAM", zstr("OfflineRenderTest")),
    });

    Bytes smpl;
    const double pi = std::acos(-1.0);
    for (uint32_t i = 0; i < kSampleLength; ++i) {
        const double v = 32000.0 * std::sin(2.0 * pi * i / kSinePeriod);
        smpl.u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(v))));
    }
    for (int i = 0; i < 46; ++i) smpl.u16(0); // SF2はサンプルの後に46個の0を要求する
    const auto sdta = riffList("sdta", {riffChunk("smpl", smpl.data())});

    Bytes phdr;
    phdr.name("Sine", 20); phdr.u16(0); phdr.u16(0); phdr.u16(0); phdr.u32(0); phdr.u32(0); phdr.u32(0);
    phdr.name("EOP", 20);  phdr.u16(0); phdr.u16(0); phdr.u16(1); phdr.u32(0); phdr.u32(0); phdr.u32(0);

    Bytes pbag;
    pbag.u16(0); pbag.u16(0);
    pbag.u16(1); pbag.u16(0);

    Bytes pgen;
    pgen.append(generatorRecord(kGenInstrument, 0));
    pgen.append(generatorRecord(0, 0));

    Bytes inst;
    inst.name("Sine", 20); inst.u16(0);
    inst.name("EOI", 20);  inst.u16(1);

    Bytes ibag;
    ibag.u16(0); ibag.u16(0);
    ibag.u16(4); ibag.u16(0);

    Bytes igen;
    igen.append(generatorRecord(kGenDelayVolEnv, kZeroTime));
    igen.append(generatorRecord(kGenAttackVolEnv, kZeroTime));
    igen.append(generatorRecord(kGenSampleModes, 1)); // ループし続ける
    igen.append(generatorRecord(kGenSampleId, 0));    // sampleIDはゾーンの最後に置く
    igen.append(generatorRecord(0, 0));

    const std::vector<uint8_t> terminal_mod(10, 0);

    Bytes shdr;
    shdr.name("Sine", 20);
    shdr.u32(0); shdr.u32(kSampleLength); shdr.u32(kLoopStart); shdr.u32(kLoopEnd);
    shdr.u32(kSf2SampleRate); shdr.u8(kRootKey); shdr.u8(0); shdr.u16(0); shdr.u16(1);
    shdr.name("EOS", 20);
    shdr.u32(0); shdr.u32(0); shdr.u32(0); shdr.u32(0); shdr.u32(0); shdr.u8(0); shdr.u8(0); shdr.u16(0); shdr.u16(0);

    const auto pdta = riffList("pdta", {
        riffChunk("phdr", phdr.data()),
        riffChunk("pbag", pbag.data()),
        riffChunk("pmod", terminal_mod),
        riffChunk("pgen", pgen.data()),
        riffChunk("inst", inst.data()),
        riffChunk("ibag", ibag.data()),
        riffChunk("imod", terminal_mod),
        riffChunk("igen", igen.data()),
        riffChunk("shdr", shdr.data()),
    });

    Bytes body;
    body.fourcc("sfbk");
    body.append(info);
    body.append(sdta);
    body.append(pdta);
    return riffChunk("RIFF", body.data());
}

// ─── 試験の補助 ─────────────────────────────────────────────────────────────

struct Fixture {
    std::filesystem::path dir;
    std::filesystem::path sf2;
};

Fixture g_fixture;

bool writeFile(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(ofs);
}

// プロファイルのsample_rateは、InitOfflineの引数が優先されることを確かめるため
// どの試験の値とも違う22050にしておく。
std::filesystem::path writeProfile(const char* file_name, double gain) {
    const std::filesystem::path path = g_fixture.dir / file_name;
    const std::string json = "{\"sample_rate\": 22050, \"gain\": " + std::to_string(gain)
                           + ", \"reverb\": false, \"chorus\": false}";
    writeFile(path, json.data(), json.size());
    return path;
}

// InitOffline → Open(生成したSF2を渡す) → ch0にそのSF2のプリセットを選ぶ。
bool startOffline(const std::filesystem::path& profile, uint32_t sample_rate, HWHandle& handle) {
    if (HWPlugin_InitOffline(profile.u8string().c_str(), sample_rate) != HW_OK) {
        std::printf("  HWPlugin_InitOffline failed\n");
        return false;
    }
    const std::string params = "{\"soundfonts\":[\"" + g_fixture.sf2.generic_u8string() + "\"]}";
    if (HWPlugin_Open(params.c_str(), &handle) != HW_OK) {
        std::printf("  HWPlugin_Open failed\n");
        HWPlugin_Shutdown();
        return false;
    }
    const uint8_t bank_select[] = {0xF0, 0x00, 0x48, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7};
    HWPlugin_WriteBlock(handle, 0, bank_select, sizeof(bank_select));
    return true;
}

void noteOn(HWHandle handle) {
    const uint8_t msg[] = {0x90, kRootKey, 127};
    HWPlugin_WriteBlock(handle, 0, msg, sizeof(msg));
}

std::vector<float> render(uint32_t frames) {
    std::vector<float> buf(static_cast<size_t>(frames) * 2, -999.0f);
    CHECK(HWPlugin_Render(buf.data(), frames) == HW_OK);
    return buf;
}

// 最初に音が出たフレーム。無ければ-1。
long firstSoundingFrame(const std::vector<float>& buf) {
    for (size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] != 0.0f) return static_cast<long>(i / 2);
    }
    return -1;
}

bool allZero(const std::vector<float>& buf) {
    return firstSoundingFrame(buf) < 0;
}

// ─── 試験 ───────────────────────────────────────────────────────────────────

void testRenderRequiresOfflineInit() {
    std::printf("[RenderRequiresOfflineInit]\n");
    float buf[32] = {};
    CHECK(HWPlugin_Render(buf, 16) == HW_ERR_INVALID_ARG);

    const auto profile = writeProfile("profile_unity.json", 1.0);
    HWHandle h = nullptr;
    if (!startOffline(profile, 48000, h)) { ++g_failures; return; }
    CHECK(HWPlugin_Render(nullptr, 16) == HW_ERR_INVALID_ARG);
    CHECK(HWPlugin_Render(buf, 16) == HW_OK);
    HWPlugin_Close(h);
    HWPlugin_Shutdown();
    CHECK(HWPlugin_Render(buf, 16) == HW_ERR_INVALID_ARG);
}

void testRejectsUnsupportedSampleRate() {
    std::printf("[RejectsUnsupportedSampleRate]\n");
    const auto profile = writeProfile("profile_unity.json", 1.0);
    CHECK(HWPlugin_InitOffline(profile.u8string().c_str(), 0) == HW_ERR_INVALID_ARG);
    CHECK(HWPlugin_InitOffline(profile.u8string().c_str(), 10000000) == HW_ERR_INVALID_ARG);
    float buf[32] = {};
    CHECK(HWPlugin_Render(buf, 16) == HW_ERR_INVALID_ARG); // 初期化されていない
}

// 1kHzの正弦波の周期を数えて、描画のサンプルレートがプロファイルの22050ではなく
// 引数の48000であることを確かめる。
void testSampleRateArgumentOverridesProfile() {
    std::printf("[SampleRateArgumentOverridesProfile]\n");
    const auto profile = writeProfile("profile_unity.json", 1.0);
    HWHandle h = nullptr;
    if (!startOffline(profile, 48000, h)) { ++g_failures; return; }
    CHECK(HWPlugin_GetLatencySamples(h) == 0);

    noteOn(h);
    const auto buf = render(4800);
    // 先頭の480フレームは数えない(発音直後の補間・振幅の立ち上がりを避ける)
    int rising = 0;
    for (size_t f = 481; f < 4800; ++f) {
        if (buf[(f - 1) * 2] < 0.0f && buf[f * 2] >= 0.0f) ++rising;
    }
    // 4319フレーム = 48000Hzなら約90周期、22050Hzなら約196周期
    std::printf("  rising zero crossings: %d\n", rising);
    CHECK(rising >= 88 && rising <= 92);
    HWPlugin_Close(h);
    HWPlugin_Shutdown();
}

void testOutputIsNotClipped() {
    std::printf("[OutputIsNotClipped]\n");
    const auto profile = writeProfile("profile_loud.json", 10.0);
    HWHandle h = nullptr;
    if (!startOffline(profile, 48000, h)) { ++g_failures; return; }
    noteOn(h);
    const auto buf = render(4800);
    float peak = 0.0f;
    for (float v : buf) peak = std::fmax(peak, std::fabs(v));
    std::printf("  peak: %f\n", peak);
    CHECK(peak > 1.0f);
    HWPlugin_Close(h);
    HWPlugin_Shutdown();
}

// 描画を始める前に送ったノートオンが、最初の描画の何フレーム目から音になるか。
long measureOnsetOnFreshSynth(const std::filesystem::path& profile) {
    HWHandle h = nullptr;
    if (!startOffline(profile, 48000, h)) { ++g_failures; return -1; }
    noteOn(h);
    const long onset = firstSoundingFrame(render(512));
    HWPlugin_Close(h);
    HWPlugin_Shutdown();
    return onset;
}

// preceding の各フレーム数で順に描画した後に送ったノートオンが、次の描画の
// 何フレーム目から音になるか。
long measureOnsetAfter(const std::filesystem::path& profile, const std::vector<uint32_t>& preceding) {
    HWHandle h = nullptr;
    if (!startOffline(profile, 48000, h)) { ++g_failures; return -1; }
    for (uint32_t frames : preceding) {
        CHECK(allZero(render(frames)));
    }
    noteOn(h);
    const long onset = firstSoundingFrame(render(512));
    HWPlugin_Close(h);
    HWPlugin_Shutdown();
    return onset;
}

// 契約は「呼び出しより前のWriteは、描画する最初のフレームから反映する」。
// fluidsynthは64フレーム単位で合成するので、それまでの描画総数が64の倍数のとき
// だけ満たせる。それ以外は次の64フレーム境界まで遅れる
// (FluidSynthEngine::render()のコメント参照)。
void testWriteTiming() {
    std::printf("[WriteTiming]\n");
    const auto profile = writeProfile("profile_unity.json", 1.0);
    const long reference = measureOnsetOnFreshSynth(profile);
    std::printf("  onset on fresh synth: frame %ld\n", reference);
    CHECK(reference >= 0 && reference <= 1);

    // 48 = 48kHzでのFITOMの1ms分。44/45 = 44.1kHzでの1ms分。
    // 複数回に分けた場合は、遅れが直前の1回ではなく総数で決まることを見る。
    const std::vector<std::vector<uint32_t>> cases = {
        {44}, {45}, {48}, {64}, {128}, {100},
        {48, 48}, {48, 48, 48}, {48, 48, 48, 48}, {44, 44, 45},
    };
    for (const auto& preceding : cases) {
        uint32_t total = 0;
        std::string label;
        for (uint32_t frames : preceding) {
            total += frames;
            label += (label.empty() ? "" : "+") + std::to_string(frames);
        }
        const long onset = measureOnsetAfter(profile, preceding);
        const long delay = onset - reference;
        const long expected = static_cast<long>((64 - total % 64) % 64);
        std::printf("  after %s frames: onset delayed by %ld frames (64-frame grid predicts %ld)\n",
                    label.c_str(), delay, expected);
        CHECK(onset >= 0);
        CHECK(delay == expected);
    }
}

void testShutdownWithoutAudioDriver() {
    std::printf("[ShutdownWithoutAudioDriver]\n");
    const auto profile = writeProfile("profile_unity.json", 1.0);

    CHECK(HWPlugin_InitOffline(profile.u8string().c_str(), 44100) == HW_OK);
    HWPlugin_Shutdown(); // 何も開かずに終える

    HWHandle h = nullptr;
    if (!startOffline(profile, 44100, h)) { ++g_failures; return; }
    noteOn(h);
    render(1000);
    HWPlugin_Shutdown(); // ハンドルを開いたまま、発音中に終える
    CHECK(!HWPlugin_IsOpen(h));

    // 終えた後に初期化し直せること(状態が残っていない)
    if (!startOffline(profile, 44100, h)) { ++g_failures; return; }
    CHECK(allZero(render(256)));
    HWPlugin_Close(h);
    HWPlugin_Shutdown();
}

} // namespace

int main() {
    g_fixture.dir = std::filesystem::temp_directory_path() / "FitomSf2IF_offline_render_test";
    std::filesystem::create_directories(g_fixture.dir);
    g_fixture.sf2 = g_fixture.dir / "sine.sf2";
    const auto sf2 = buildSineSf2();
    if (!writeFile(g_fixture.sf2, sf2.data(), sf2.size())) {
        std::printf("cannot write %s\n", g_fixture.sf2.u8string().c_str());
        return 1;
    }

    testRenderRequiresOfflineInit();
    testRejectsUnsupportedSampleRate();
    testSampleRateArgumentOverridesProfile();
    testOutputIsNotClipped();
    testWriteTiming();
    testShutdownWithoutAudioDriver();

    std::error_code ec;
    std::filesystem::remove_all(g_fixture.dir, ec);

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
