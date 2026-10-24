// examples/smoke_test.cpp
//
// FitomSf2IF が公開する IHWPlugin C API を一通り呼び、ビルド・リンクと
// 基本的な初期化フロー(プロファイル読み込み → オーディオドライバ起動 →
// MIDIメッセージ送出 → 停止)が動作することを確認する。
//
// オーディオデバイスが存在しない環境(CI等)ではHWPlugin_Initが
// HW_ERR_OPEN_FAILED を返すことを許容する(その場合はAPI呼び出し自体が
// 正常に行えたことのみ確認して終了する)。
// sf2if_profile.json を用意していない場合はデフォルト設定で開こうとする。

#include "fitom/IHWPlugin.h"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    std::printf("HWPlugin_GetName: %s\n", HWPlugin_GetName());

    const HWResult init_result = HWPlugin_Init(nullptr);
    if (init_result != HW_OK) {
        std::printf("HWPlugin_Init failed (HWResult=%d) — "
                     "no audio device available? skipping I/O checks.\n",
                     static_cast<int>(init_result));
        return 0;
    }

    // params_json: FITOM_Xコアがsf2_banks[].fileから構築する"soundfonts"配列を模す
    // (docs/sf2-fluidsynth-integration.md 4節⑥⑦)。実ファイルは無いため空配列で
    // API疎通だけ確認する。
    HWHandle handle = nullptr;
    const HWResult open_result = HWPlugin_Open(R"({"soundfonts":[]})", &handle);
    if (open_result != HW_OK || handle == nullptr) {
        std::printf("HWPlugin_Open failed (HWResult=%d)\n", static_cast<int>(open_result));
        HWPlugin_Shutdown();
        return 1;
    }
    std::printf("HWPlugin_IsOpen: %s\n", HWPlugin_IsOpen(handle) ? "true" : "false");
    std::printf("HWPlugin_GetLatencySamples: %u\n", HWPlugin_GetLatencySamples(handle));

    // Note On (ch=0, key=60, vel=100) -> Note Off
    const uint8_t note_on[]  = {0x90, 60, 100};
    const uint8_t note_off[] = {0x80, 60, 0};

    HWPlugin_WriteBlock(handle, 0, note_on, sizeof(note_on));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    HWPlugin_WriteBlock(handle, 0, note_off, sizeof(note_off));

    // マスターボリューム(F0 7F <dev> 04 01 ll mm F7、8節): 半分の音量(0x40=64)
    const uint8_t master_volume[] = {0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, 0x40, 0xF7};
    HWPlugin_WriteBlock(handle, 0, master_volume, sizeof(master_volume));

    // マスターファインチューニング(F0 7F <dev> 04 03 ll mm F7、8節): center(0x2000)
    const uint8_t master_tune[] = {0xF0, 0x7F, 0x7F, 0x04, 0x03, 0x00, 0x40, 0xF7};
    HWPlugin_WriteBlock(handle, 0, master_tune, sizeof(master_tune));

    // SF2バンク選択(F0 00 48 01 05 <chan> <sfidx> <bank_msb> <bank_lsb> <prog> F7、4節④⑤)。
    // soundfonts配列が空のため soundfont_index=0 は解決できず無視される想定
    // (クラッシュしないことのみ確認)。
    const uint8_t bank_select[] = {0xF0, 0x00, 0x48, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7};
    HWPlugin_WriteBlock(handle, 0, bank_select, sizeof(bank_select));

    HWPlugin_Close(handle);
    HWPlugin_Shutdown();
    std::printf("smoke test done.\n");
    return 0;
}
