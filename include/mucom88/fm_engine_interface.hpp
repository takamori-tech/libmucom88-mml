// =============================================================================
// fm_engine_interface.hpp
// FM音源エンジン共通インターフェース
//
// fmgen版 (FmEngineFmgen) と ymfm版 (FmEngine) の両方がこの
// インターフェースを実装する。mml_engine やゲーム本体は
// IFmEngine* を通じて操作し、実装を切り替え可能にする。
// =============================================================================

#pragma once

#include <cstdint>
#include <string>

class IFmEngine
{
public:
    virtual ~IFmEngine() = default;

    // 初期化（出力サンプルレート指定）
    virtual void init(uint32_t sampleRate) = 0;

    // YM2608レジスタ書き込み
    // port=0: ポート0（FM ch1-3 / SSG）, port=1: ポート1（FM ch4-6）
    virtual void writeReg(int port, uint8_t addr, uint8_t data) = 0;

    // ステレオPCM生成（インターリーブ L,R,L,R...）
    virtual void generateInterleaved(int16_t* buf, uint32_t frameCount) = 0;

    // リセット
    virtual void reset() = 0;

    // ADPCM-A ROM ロード（リズム音源用）
    virtual bool loadAdpcmRom(const std::string& path) = 0;
    virtual bool loadAdpcmRomFromMemory(const uint8_t* data, size_t size) = 0;
    virtual bool hasAdpcmRom() const = 0;

    // ADPCM-B ボイステーブル
    virtual bool loadVoiceTable(const std::string& path) = 0;
    virtual bool loadVoiceTableFromMemory(const uint8_t* data, size_t dataSize) = 0;
    virtual void playVoice(int voiceId) = 0;
    virtual void stopVoice() = 0;
};
