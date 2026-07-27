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

namespace {
// F0 00 48 01 05 <chan> <soundfont_index> <sf2_bank_msb> <sf2_bank_lsb> <prog> F7
// (docs/sf2-fluidsynth-integration.md 4節④⑤、2026年7月確定)
constexpr size_t kBankSelectSysExLen = 11;

// F0 7F <dev> 04 01/03 ll mm F7 (GM2 Universal Realtime SysEx、8節)
constexpr size_t kUniversalRealtimeSysExLen = 8;
constexpr uint8_t kUniversalRealtimeId  = 0x7F;
constexpr uint8_t kDeviceControlSubId1  = 0x04;
constexpr uint8_t kMasterVolumeSubId2   = 0x01;
constexpr uint8_t kMasterFineTuneSubId2 = 0x03;
} // namespace

void MidiEventParser::dispatchSystemExclusive(const uint8_t* data, size_t len) {
    // GM2 Universal Realtime SysEx: F0 7F <dev> 04 01/03 ll mm F7
    // マスターボリューム/マスターファインチューニング(8節・4節②参照)。
    // デバイスIDは判定しない(FITOM_X本体自身の受信側実装と同じ規約、8節冒頭)。
    if (len == kUniversalRealtimeSysExLen && data[1] == kUniversalRealtimeId
        && data[3] == kDeviceControlSubId1) {
        const uint8_t subId2 = data[4];
        const uint8_t ll = data[5];
        const uint8_t mm = data[6];
        if (subId2 == kMasterVolumeSubId2) {
            engine_.setMasterVolume(mm); // 8節: 「mmの値、7bit精度で反映」。llは使わない。
        } else if (subId2 == kMasterFineTuneSubId2) {
            const int value14 = (static_cast<int>(mm) << 7) | ll;
            engine_.setMasterFineTuning(value14);
        }
        return;
    }

    // プライベートSysEx: F0 00 48 01 <sub-cmd> ... F7
    // (docs/manuals/midi-message-reference.md 8.1節と同じマニュファクチャラID)
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
        case 0x05: {
            // F0 00 48 01 05 <chan> <soundfont_index> <sf2_bank_msb> <sf2_bank_lsb> <prog> F7
            if (len != kBankSelectSysExLen) {
                break; // 不正な長さ。破棄する。
            }
            const int chan            = data[5];
            const int soundfont_index = data[6];
            const int sf2_bank        = (static_cast<int>(data[7]) << 7) | data[8]; // MSB×128+LSB(14bit)
            const int prog            = data[9];
            engine_.selectProgramByIndex(chan, soundfont_index, sf2_bank, prog);
            break;
        }
        default:
            // sub-cmd 0x01-0x04等、このプラグインに関係しないメッセージは無視する
            // (sub-cmd 0x04 = チャンネル窓割り当てはFITOM_Xコア内部の変換テーブル
            // であり、本プラグインには送られてこない想定。5節参照)。
            break;
    }
}

} // namespace sf2if
