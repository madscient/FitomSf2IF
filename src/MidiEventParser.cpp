#include "MidiEventParser.h"
#include "FluidSynthEngine.h"

namespace sf2if {

namespace {
constexpr uint8_t kStatusNoteOff        = 0x80;
constexpr uint8_t kStatusNoteOn         = 0x90;
constexpr uint8_t kStatusPolyPressure   = 0xA0;
constexpr uint8_t kStatusControlChange  = 0xB0;
constexpr uint8_t kStatusProgramChange  = 0xC0;
constexpr uint8_t kStatusChannelPressure= 0xD0;
constexpr uint8_t kStatusPitchBend      = 0xE0;
constexpr uint8_t kStatusSysExStart     = 0xF0;
constexpr uint8_t kStatusSysExEnd       = 0xF7;

constexpr uint8_t kCcBankSelectMsb = 0;   // ④節: コア側でsub-cmd 0x05へ変換される想定、素通しでは読み捨てる
constexpr uint8_t kCcBankSelectLsb = 32;

// 標準チャンネルボイスメッセージが持つデータバイト数(プログラムチェンジ/
// チャンネルプレッシャーのみ1バイト、他は2バイト)。
size_t dataBytesFor(uint8_t statusHighNibble) {
    switch (statusHighNibble) {
        case kStatusProgramChange:
        case kStatusChannelPressure:
            return 1;
        default:
            return 2;
    }
}
} // namespace

void MidiEventParser::parseBlock(const uint8_t* data, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        const size_t consumed = parseOneMessage(data + pos, len - pos);
        if (consumed == 0) {
            // パース不能(データバイトのみでステータスバイトが無い等)。
            // ランニングステータス非対応のため読み飛ばす(ヘッダコメント参照)。
            ++pos;
            continue;
        }
        pos += consumed;
    }
}

size_t MidiEventParser::parseOneMessage(const uint8_t* data, size_t len) {
    const uint8_t status = data[0];
    if (status < 0x80) {
        return 0; // ランニングステータス非対応(未実装)。呼び出し元が1バイト読み飛ばす。
    }

    if (status == kStatusSysExStart) {
        size_t end = 1;
        while (end < len && data[end] != kStatusSysExEnd) {
            ++end;
        }
        if (end >= len) {
            return len; // 終端(0xF7)未着 = ブロック境界をまたいだ不完全なSysEx。破棄する。
        }
        dispatchSystemExclusive(data, end + 1);
        return end + 1;
    }

    if (status >= 0xF0) {
        // その他のシステムメッセージ(未対応)。1バイトとして読み飛ばす。
        return 1;
    }

    const uint8_t highNibble = status & 0xF0;
    const size_t need = 1 + dataBytesFor(highNibble);
    if (len < need) {
        return len; // 不完全なメッセージ。ブロック境界をまたいでいる。
    }
    dispatchChannelVoiceMessage(status, data + 1, need - 1);
    return need;
}

void MidiEventParser::dispatchChannelVoiceMessage(uint8_t status, const uint8_t* payload, size_t payload_len) {
    const int chan = status & 0x0F;
    const uint8_t highNibble = status & 0xF0;

    switch (highNibble) {
        case kStatusNoteOff:
            engine_.noteOff(chan, payload[0]);
            break;
        case kStatusNoteOn:
            if (payload[1] == 0) {
                engine_.noteOff(chan, payload[0]); // velocity=0はNote Off扱い(MIDI仕様標準)
            } else {
                engine_.noteOn(chan, payload[0], payload[1]);
            }
            break;
        case kStatusPolyPressure:
            engine_.keyPressure(chan, payload[0], payload[1]);
            break;
        case kStatusControlChange:
            if (payload[0] == kCcBankSelectMsb || payload[0] == kCcBankSelectLsb) {
                break; // ④節: バンク選択はプライベートSysEx経由。標準CCは読み捨てる。
            }
            engine_.controlChange(chan, payload[0], payload[1]);
            break;
        case kStatusProgramChange:
            engine_.programChange(chan, payload[0]);
            break;
        case kStatusChannelPressure:
            engine_.channelPressure(chan, payload[0]);
            break;
        case kStatusPitchBend: {
            const int value14 = (static_cast<int>(payload[1]) << 7) | payload[0];
            engine_.pitchBend(chan, value14);
            break;
        }
        default:
            break; // 未対応ステータス。
    }
}

void MidiEventParser::dispatchSystemExclusive(const uint8_t* data, size_t len) {
    // プライベートSysEx: F0 00 48 01 <sub-cmd> ... F7
    // (docs/manuals/midi-message-reference.md 8.1節と同じマニュファクチャラID)
    //
    // TODO(sub-cmd 0x05): SF2直行パスのバンク選択メッセージ
    // {chan, file, sf2_bank, prog} を1メッセージにまとめる方針は決定済みだが、
    // 具体的なバイト列レイアウトは未確定
    // (docs/sf2-fluidsynth-integration.md 5節「バンク選択を伝えるプライベート
    // SysEx(sub-cmd 0x05案)の正式なバイト列仕様」参照。file が可変長文字列に
    // なるため、既存の8.1節JSON埋め込み方式に寄せるか固定長+文字列末尾にするか
    // 未確定)。FITOM_X本体側の実装が固まり次第、ここでパースして
    // engine_.selectProgram(chan, file, sf2_bank, prog) を呼ぶこと。
    constexpr uint8_t kManufacturerId[3] = {0x00, 0x48, 0x01};
    constexpr size_t  kHeaderLen = 1 /*F0*/ + 3 /*manufacturer id*/ + 1 /*sub-cmd*/;

    if (len < kHeaderLen) {
        return;
    }
    if (data[1] != kManufacturerId[0] || data[2] != kManufacturerId[1] || data[3] != kManufacturerId[2]) {
        return; // FITOM_Xのプライベートフォーマットではない。無視する。
    }

    const uint8_t subCmd = data[4];
    switch (subCmd) {
        case 0x05:
            // TODO: 未実装(上記コメント参照)。
            break;
        default:
            // sub-cmd 0x01-0x04等、このプラグインに関係しないメッセージは無視する
            // (sub-cmd 0x04 = チャンネル窓割り当てはFITOM_Xコア内部の変換テーブル
            // であり、本プラグインには送られてこない想定。5節参照)。
            break;
    }
}

} // namespace sf2if
