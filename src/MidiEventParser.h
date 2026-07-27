#pragma once
// src/MidiEventParser.h
//
// HWPlugin_WriteBlock() で渡される生MIDIバイト列をパースし、FluidSynthEngine の
// 対応メソッドへディスパッチする(docs/sf2-fluidsynth-integration.md 2節②参照)。
//
// 前提:
//   - チャンネルニブルは、FITOM_Xコア側で既にMPU×ch→fluidsynth chan(0-15)へ
//     変換済みの値が渡ってくる(5節「窓」変換はコア側の責務、本プラグインは
//     関知しない)。
//   - CC#0(バンクセレクトMSB)・CC#32(バンクセレクトLSB)は、コア側が代わりに
//     プライベートSysEx(sub-cmd 0x05, 未確定)で送ってくる想定のため、生バイト列
//     として届いた場合は読み捨てる(④節末尾)。
//   - ランニングステータス(直前のステータスバイト省略)は現時点では非対応。
//     FITOM_Xコア側が組み立てる生MIDIバイト列がランニングステータスを使うかは
//     未確認のため、実装時に要確認(使う場合はここに状態保持を追加する)。

#include <cstddef>
#include <cstdint>

namespace sf2if {

class FluidSynthEngine;

class MidiEventParser {
public:
    explicit MidiEventParser(FluidSynthEngine& engine) : engine_(engine) {}

    // data/len を1メッセージ以上のMIDIバイト列として解釈し、engine_へディスパッチする。
    // 未知/未対応のステータスバイトは黙って読み飛ばす(ステートレスな素通し方針、
    // docs/sf2-fluidsynth-integration.md 5節「CH変換付きMIDIパッチベイに徹する」)。
    void parseBlock(const uint8_t* data, size_t len);

private:
    size_t parseOneMessage(const uint8_t* data, size_t len);
    void dispatchChannelVoiceMessage(uint8_t status, const uint8_t* payload, size_t payload_len);
    void dispatchSystemExclusive(const uint8_t* data, size_t len);

    FluidSynthEngine& engine_;
};

} // namespace sf2if
