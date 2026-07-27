// examples/smoke_test.cpp
//
// FitomSf2IF が公開する IHWPlugin C API を一通り呼び、ビルド・リンクと
// 基本的な初期化フロー(プロファイル読み込み → RtAudioストリームオープン →
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

    HWHandle handle = nullptr;
    const HWResult open_result = HWPlugin_Open(nullptr, &handle);
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

    HWPlugin_Close(handle);
    HWPlugin_Shutdown();
    std::printf("smoke test done.\n");
    return 0;
}
