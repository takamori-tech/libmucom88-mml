// =============================================================================
// mml_engine.hpp  (revised)
// MML シーケンサー / YM2608 ドライバー（MUCOM88形式対応）
//
// 対応チャンネル:
//   A-C (0-2):  FM ch1-3（port 0）
//   D-F (3-5):  SSG ch1-3（PSG互換矩形波）
//   G   (6):    リズム音源（ADPCM-A: BD/SD/CY/HH/TM/RS）
//   H-J (7-9):  FM ch4-6（port 1）
//   K   (10):   ADPCM-B（未実装）
// =============================================================================

#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include "mml_parser.hpp"
#include "fm_engine_interface.hpp"

// FmPatch / Mucom88Patch は fm_common.hpp で定義済み（mml_parser.hpp 経由）

// ── デフォルト音色ファクトリ ─────────────────────────────
static Mucom88Patch makeDefaultPatch(int pno = 0)
{
    Mucom88Patch p;
    p.patchNo = pno; p.fb = 0; p.al = 4; p.valid = true;
    // AR DR SR RR SL TL KS ML DT
    p.op[0] = { 31, 5, 0, 5, 0, 28, 0, 1, 0 };
    p.op[1] = { 31, 5, 0, 5, 0, 28, 0, 1, 0 };
    p.op[2] = { 31, 5, 0, 5, 0, 28, 0, 1, 0 };
    p.op[3] = { 31, 5, 0, 5, 0,  0, 0, 1, 0 };
    return p;
}

// noteToFnum / noteToSSGPeriod は fm_common.hpp で定義済み（mml_parser.hpp 経由）

// =============================================================================
// MmlEngine
// =============================================================================
class MmlEngine
{
public:
    // MUCOM88 全11チャンネル (A-K)
    static constexpr int MAX_MML_CHANNELS = 11;
    // FM 6チャンネル (A-C=0-2, H-J=7-9 → FM index 0-5)
    static constexpr int MAX_FM_CHANNELS  = 6;
    // SSG 3チャンネル (D-F=3-5)
    static constexpr int MAX_SSG_CHANNELS = 3;

    MmlEngine() : m_engine(nullptr), m_sampleRate(44100), m_chipClock(7987200), m_playing(false) {}

    // ── チャンネル種別判定 ────────────────────────────────
    static bool isFM(int ch)     { return ch <= 2 || (ch >= 7 && ch <= 9); }
    static bool isSSG(int ch)    { return ch >= 3 && ch <= 5; }
    static bool isRhythm(int ch) { return ch == 6; }
    static bool isADPCMB(int ch) { return ch == 10; }
    // FM index (0-5) from MML channel
    static int toFMIndex(int ch) { return (ch <= 2) ? ch : (ch - 7 + 3); }
    // SSG index (0-2) from MML channel
    static int toSSGIndex(int ch) { return ch - 3; }

    // ── 初期化 ─────────────────────────────────────────
    void init(IFmEngine* engine, uint32_t sampleRate, uint32_t chipClock = 7987200)
    {
        m_engine     = engine;
        m_sampleRate = sampleRate;
        m_chipClock  = chipClock;
        for (auto& ch : m_channels) ch = ChannelState{};
        m_patchMap.clear();
        m_fmPatchNo.fill(0);
        m_ssgMixer   = 0x3F;  // 全SSGトーン・ノイズ無効
        m_rhythmMask = 0;
        m_patchMap[0] = makeDefaultPatch(0);
    }

    // ── MML イベント列を直接セット（MucFile用）─────────
    void setEvents(int ch, const std::vector<MmlEvent>& evts)
    {
        if (ch < 0 || ch >= MAX_MML_CHANNELS) return;
        m_channels[ch].events      = evts;
        m_channels[ch].eventIdx    = 0;
        m_channels[ch].noteOn      = false;
    }

    // ── MML 読み込み（シングルチャンネル用）────────────
    void loadMml(const std::string& mml, int ch = 0)
    {
        if (ch < 0 || ch >= MAX_MML_CHANNELS) return;
        setEvents(ch, parseSingleChannelMml(mml, ch));
    }

    // ── 音色設定（音色番号ベース・0〜127）────────────────
    void setPatch(int patchNo, const FmPatch& patch)
    {
        m_patchMap[patchNo] = patch;
    }

    // ── Cコマンド（全音符クロック数）設定 ──────────────
    // パーサーのwholeTick値を渡す。テンポ計算のPPQに影響。
    void setWholeTick(int wt) { m_wholeTick = (wt > 0) ? wt : 128; }

    // 曲全体のループ終端tickを外部から設定（パート分離比較用）
    // play()内のcommonEndTick計算を上書きする
    void setCommonEndTick(uint32_t tick) { m_overrideEndTick = tick; }

    // ── ループ設定 ──────────────────────────────────────
    void setLoop(bool loop) { m_loop = loop; }

    // ── 再生開始 ────────────────────────────────────────
    void play()
    {
        m_globalTick        = 0;
        m_globalSampleAccum = 0;
        m_audioLeftMs       = 0.0;
        m_globalTempo       = 120;
        m_loopTickOffset    = 0;

        // 全チャンネルのランタイム状態をフルリセット（libmucom88-mml#2）
        // イベント列(events)は保持し、再生位置とランタイム状態のみ初期化
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            auto& st = m_channels[ch];
            auto savedEvents = std::move(st.events);
            st = ChannelState{};
            st.events = std::move(savedEvents);
        }
        // 最初のTEMPOイベントを探して初期テンポを設定
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            for (const auto& ev : m_channels[ch].events) {
                if (ev.type == MmlEventType::TEMPO) {
                    m_globalTempo = ev.value;
                    break;
                }
            }
        }
        // 曲全体ループ周期の計算（OpenMUCOM88 maxcount 互換）
        // Wiki: Lコマンド = "曲全体のループ位置指定"
        // イベント列を直接走査してLOOP_POINTを検出（processEvents実行前に必要）
        // Z80コンパイラは全チャンネルを同一曲長（maxcount）にパディングする。
        // 我々のパーサーではチャンネル間でendTickが異なるため:
        //   commonEndTick  = max(endTick) — 最長チャンネルが曲全体長を決定（≈ maxcount）
        //   commonLoopTick = min(loopTick) — 最も早いLポイントからループ区間開始
        // min(endTick)を使うと短いチャンネルで曲が打ち切られ、loopTick > endTick で
        // uint32_t underflow → 毎tick globalLoopRestart() が発火する（Issue #32）
        {
            m_commonEndTick = 0;
            m_implicitLoop = false;
            m_commonLoopTick = UINT32_MAX;
            bool anyLoop = false;
            uint32_t maxEnd = 0;
            for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
                auto& st = m_channels[ch];
                if (st.events.empty()) continue;
                // イベント列からLOOP_POINTを探してhasLoopPointを事前設定
                for (size_t i = 0; i < st.events.size(); i++) {
                    if (st.events[i].type == MmlEventType::LOOP_POINT) {
                        st.hasLoopPoint = true;
                        st.loopEventIdx = i + 1;
                        st.loopTick     = st.events[i].tick;
                        break;
                    }
                }
                // 全非空チャンネルのendTickをmax計算に含める（Z80パディング互換）
                uint32_t endTick = st.events.back().tick;
                if (endTick > maxEnd) maxEnd = endTick;
                if (!st.hasLoopPoint) continue;
                anyLoop = true;
                if (st.loopTick < m_commonLoopTick)
                    m_commonLoopTick = st.loopTick;
            }
            if (anyLoop && maxEnd > 0) {
                m_commonEndTick = maxEnd;
            } else if (!anyLoop && maxEnd > 0) {
                // L無し曲: Z80は全チャンネルがDATA TOP(先頭)からループ
                // 暗黙のloop point = tick 0 として扱う
                m_commonEndTick = maxEnd;
                m_implicitLoop = true;
            }
            if (m_commonLoopTick == UINT32_MAX)
                m_commonLoopTick = 0;
            // 外部からループ終端tickが指定されている場合はそちらを優先
            // （パート分離比較時にOpenMUCOM88のMaxCountと合わせるため）
            if (m_overrideEndTick > 0)
                m_commonEndTick = m_overrideEndTick;

            // Z80コンパイラ互換: commonEndTickを超えるイベントは再生時にスキップ
            // イベント列自体は破壊しない（play()の再呼び出しに対応、libmucom88-mml#2）
        }
        // Timer-B 初期化
        recalcTimerB();
        m_timerBCount = m_timerBPeriod;

        if (m_engine) {
            allSoundOff();

            // OpenMUCOM88互換の初期化シーケンス
            // FM PAN: 全チャンネル L+R
            for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) {
                int port = fmPort(fi);
                int off  = fmOffset(fi);
                m_engine->writeReg(port, 0xB4 + off, 0xC0);
            }
            // リズム楽器 IL 初期化: L+R + Level 31
            m_rhythmIL.fill(0xDF);
            for (int i = 0; i < 6; i++)
                m_engine->writeReg(0, 0x18 + i, m_rhythmIL[i]);
            // SSG トーンレジスタ初期化
            for (int i = 0; i < 6; i++)
                m_engine->writeReg(0, i, 0x00);
            // SSG ミキサー: トーン有効, ノイズ無効（MUCOM88互換）
            m_ssgMixer = 0x38;
            m_engine->writeReg(0, 0x07, m_ssgMixer);
            // SSG ノイズ周期
            m_engine->writeReg(0, 0x06, 0x00);
            // Timer制御: 通常モード（CSMモード解除）
            // Z80 PLSET2: reg 0x27 = 0x3A（Timer-B有効、CSMなし）
            m_engine->writeReg(0, 0x27, 0x3A);

            // FM音色を再適用
            for (int fi = 0; fi < MAX_FM_CHANNELS; fi++)
                fmApplyPatch(fi, m_fmPatchNo[fi]);
        }

        // Z80互換: tick 0 のイベントを即時処理
        // Z80ドライバーはplay()時にMMLデータを即座に読み込み、最初のノートを設定する。
        // Timer-Bの最初の発火を待たない。これにより1ティック分の遅延を回避。
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            auto& st = m_channels[ch];
            if (st.events.empty()) continue;
            processEvents(ch, m_globalTick);  // m_globalTick = 0
        }

        m_playing = true;
    }

    // ── 停止 ────────────────────────────────────────────
    void stop()
    {
        m_playing = false;
        if (m_engine) allSoundOff();
    }

    // ── 一時停止 ────────────────────────────────────────
    void pause()
    {
        m_playing = false;
        if (m_engine) allSoundOff();
    }

    // ── 再開 ────────────────────────────────────────────
    void resume()
    {
        if (m_engine) {
            allSoundOff();
            for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) {
                fmApplyPatch(fi, m_fmPatchNo[fi]);
                fmSetVolume(fi, m_channels[fmMmlCh(fi)].volume);
            }
        }
        m_playing = true;
    }

    bool isPlaying() const { return m_playing; }
    uint32_t globalTick() const { return m_globalTick; }
    int globalTempo() const { return m_globalTempo; }

    // チャンネル状態取得（UI表示用）
    bool chNoteOn(int ch) const { return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].noteOn : false; }
    int  chNote(int ch) const { return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].currentNote : 0; }
    int  chVolume(int ch) const { return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].volume : 0; }
    int  chPan(int ch) const { return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].pan : 3; }
    // noteOnトリガーカウンター（UI activity検出用、advance()毎にインクリメント）
    // chNoteOn()はワンショット楽器で一瞬falseになるため、カウンターで検出する
    uint32_t chNoteOnCount(int ch) const { return (ch >= 0 && ch < MAX_MML_CHANNELS) ? m_channels[ch].noteOnCount : 0; }
    // FM パッチ番号取得（fi=FMインデックス 0-5）
    int  fmPatchNo(int fi) const { return (fi >= 0 && fi < MAX_FM_CHANNELS) ? m_fmPatchNo[fi] : -1; }

    // ── グローバル減衰（ダッキング用）────────────────────
    // att: FM TL加算値（0=通常、20≈-15dB）。SSGはatt/4で換算。
    // ADPCM-A/Bには影響しない（レジスタが別系統のため）。
    void setGlobalAttenuation(int att)
    {
        m_globalAtt = att;
        if (!m_engine) return;
        // 即時反映: 全アクティブFMチャンネルの音量を再設定
        for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) {
            int ch = fmMmlCh(fi);
            fmSetVolume(fi, m_channels[ch].volume);
        }
        // 即時反映: 全アクティブSSGチャンネルの振幅を再設定
        for (int si = 0; si < MAX_SSG_CHANNELS; si++) {
            int ch = si + 3;
            if (m_channels[ch].noteOn) {
                int vol = std::clamp(m_channels[ch].volume - m_globalAtt / 4, 0, 15);
                m_engine->writeReg(0, 0x08 + si, (uint8_t)(vol & 0x0F));
            }
        }
    }
    int globalAttenuation() const { return m_globalAtt; }

    // ── 時間を進める（MUCOM88互換: 全チャンネル同期クロック）─
    //
    // MUCOM88ではYM2608 Timer-BのINT3割り込みで全チャンネルが
    // 同期的に1クロック進む。これを再現するため、グローバルtickで
    // 全チャンネルのイベントを同時に処理する。
    //
    // Timer-B周期 = (256 - T) × 1152 / baseclock 秒
    // baseclock = 7987200 Hz (PC-8801)
    // samplesPerTick = (256 - T) × 1152 / 7987200 × sampleRate
    //               = (256 - T) × 1152 × sampleRate / 7987200
    void advance(uint32_t frameCount)
    {
        if (!m_playing || !m_engine) return;

        // OpenMUCOM88 完全互換 Timer-B タイミング
        //
        // fmgenの内部クロック計算:
        //   baseclock = 7987200 Hz (PC-8801 YM2608)
        //   fmgen SetRate() で clock /= 2 → 3993600
        //   fmclock = 3993600 / 6 / 12 = 55466.67
        //   timer_stepd = 1000.0 / fmclock * 16.0 = 0.28837... ms
        //
        // Timer-Bカウンタ:
        //   timerb = (int)((256 - T) * timer_stepd * 1024.0)
        //
        // OpenMUCOM88のCMucom::RenderAudio:
        //   16サンプルごとに AudioLeftMs += 16 * (1000.0 / sampleRate)
        //   整数ミリ秒分を UpdateTime(ms << 10) に渡す
        //   Timer-Bカウンタから (ms << 10) を減算、0以下でtick発生

        // 16サンプル単位で処理（OpenMUCOM88と同じ粒度）
        m_globalSampleAccum += frameCount;

        while (m_globalSampleAccum >= 16) {
            m_globalSampleAccum -= 16;
            m_audioLeftMs += 16.0 * 1000.0 / m_sampleRate;

            int ms = (int)m_audioLeftMs;
            if (ms <= 0) continue;
            m_audioLeftMs -= ms;

            // Timer-B カウンタ更新（OpenMUCOM88 fmtimer.cpp Count() 互換）
            int tickUnits = ms << 10;  // ms * 1024 (TICK_SHIFT)
            m_timerBCount -= tickUnits;

            while (m_timerBCount <= 0) {
                m_timerBCount += m_timerBPeriod;
                m_globalTick++;

                // 曲全体ループ判定: chTick が commonEndTick に達したら全チャンネル同時ループ
                // Wiki: L = "曲全体のループ位置指定"
                if (m_loop && m_commonEndTick > 0) {
                    uint32_t chTick = m_globalTick - m_loopTickOffset;
                    if (chTick >= m_commonEndTick) {
                        globalLoopRestart();
                        // ループ後のイベントを即座に処理
                        for (int ch2 = 0; ch2 < MAX_MML_CHANNELS; ch2++) {
                            if (m_channels[ch2].events.empty()) continue;
                            processEvents(ch2, m_globalTick);
                        }
                    }
                }

                // Z80 PLSET2: 毎tick Timer制御レジスタ書き込み（INT3ハンドラ先頭）
                // 通常モード: 0x3A、CSMエフェクトモード: 0x7A
                // Timer-Bオーバーフローフラグリセット + Timer制御の安定化
                if (m_engine) {
                    bool anyCsm = false;
                    for (int ch2 = 0; ch2 < MAX_MML_CHANNELS; ch2++)
                        if (m_channels[ch2].csmEnabled) { anyCsm = true; break; }
                    m_engine->writeReg(0, 0x27, anyCsm ? 0x7A : 0x3A);
                }

                // 全チャンネルを同じtickで同期処理（INT3割り込み相当）
                for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
                    auto& st = m_channels[ch];
                    if (st.events.empty()) continue;
                    if (st.eventIdx >= st.events.size()) continue;
                    processEvents(ch, m_globalTick);
                    if (st.noteOn && st.lfoEnabled && st.lfoDepth != 0)
                        tickLfo(ch);
                    if (st.portaActive)
                        tickPortamento(ch);
                }

                // MUCOM88互換: SSGソフトウェアエンベロープ
                // 発音中: 音量を毎tick再書き込み（SOFENV互換）
                // リリース中: 音量を減衰させる（MUCOM88 SSSUBA互換）
                for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
                    if (!isSSG(ch)) continue;
                    auto& cst = m_channels[ch];
                    int si = toSSGIndex(ch);

                    if (cst.ssgSoftEnv && (cst.noteOn || cst.ssgEnvPhase > 0)) {
                        // ── MUCOM88 SOFENV互換 ADSRステートマシン ──
                        // Z80 SSSUB0: BIT 7,(IX+6) → エンベロープ有効時のみSOFENV呼び出し
                        // 発音中(noteOn)またはRELEASE中(phase>0)のみ処理。
                        // 未発音時（休符等）はSOFENVを回さない（Z80と同じ）。
                        // Z80互換: KEY_ON tickではSOFENV進行をスキップ（SOFEV7のみ）
                        // Z80 SSSUBG: envValue=AL → CALL SOFEV7（音量計算のみ）
                        // Z80 SSSUB0: CALL SOFENV（エンベロープ進行+音量計算）← 次tick以降
                        if (cst.ssgEnvKeyOnTick) {
                            cst.ssgEnvKeyOnTick = false;
                        } else if (cst.ssgEnvPhase > 0) {
                            ssgTickEnvelope(ch);
                        }
                        int vol = std::clamp(cst.volume - m_globalAtt / 4, 0, 15);
                        int amp = ((vol + 1) * cst.ssgEnvValue) >> 8;
                        // SOFEV7リバーブ（Z80 music.asm:2336-2342）:
                        // BIT 6,(IX+31): SSGではTIEフラグ（SET=発音中, RES=KEY_OFF後）
                        // RET NZ: TIEフラグSET(発音中)→リバーブ適用しない
                        // BIT 5,(IX+33): リバーブON→適用
                        // → KEY_OFF後(noteOn=false)かつリバーブONならamp = (amp+rv)>>1
                        if (cst.reverbEnabled && !cst.noteOn) {
                            amp = (amp + cst.reverbValue) >> 1;
                        }
                        if (amp > 15) amp = 15;
                        if (amp < 0) amp = 0;
                        m_engine->writeReg(0, 0x08 + si, (uint8_t)amp);
                    } else if (cst.ssgReleasing) {
                        // Eコマンド未使用の簡易リリース
                        cst.ssgRelVol -= 2;
                        if (cst.ssgRelVol <= 0) {
                            cst.ssgRelVol = 0;
                            cst.ssgReleasing = false;
                        }
                        m_engine->writeReg(0, 0x08 + si, (uint8_t)(cst.ssgRelVol & 0x0F));
                    }
                }

                // MUCOM88互換: FMリバーブ毎tick TL書き込み（Z80 FS2互換）
                // Z80 FMSUB0: wait<q かつ リバーブON → FS2
                // FS2: C = ((IX+6) + (IX+17)) >> 1 → STV2（TOTALV加算なし）
                // IX+6は変更されない→毎tick同じ値を書く（定数、IIR減衰ではない）
                // Z80互換: チャンネルデータ終了後はFMSUB0が呼ばれないため停止
                for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
                    if (!isFM(ch)) continue;
                    auto& cst = m_channels[ch];
                    if (!cst.reverbActive) continue;
                    // チャンネル終了後は停止（Z80: データ終了後FMSUB0不呼び出し）
                    if (cst.eventIdx >= cst.events.size()) {
                        cst.reverbActive = false;
                        continue;
                    }
                    // Z80 FS2: C = (IX+6 + IX+17) >> 1 → STV2(FMVDAT[C])
                    // IX+6(volume)は不変→毎tick同じTL値を書く（定数、IIR減衰ではない）
                    fmSetReverbVolume(toFMIndex(ch), cst.volume, cst.reverbValue);
                }

                // テンポ変更 → Timer-B再計算
                recalcTimerB();
            }
        }

        // 全チャンネル終端 → 停止（ループなし時）
        if (!m_loop) {
            bool allDone = true;
            for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
                if (m_channels[ch].events.empty()) continue;
                if (m_channels[ch].eventIdx < m_channels[ch].events.size()) {
                    allDone = false;
                    break;
                }
            }
            if (allDone) stop();
        }
    }

    // ── 曲全体ループ: 全チャンネルをLポイントに同時巻き戻す ──
    // Wiki: L = "曲全体のループ位置指定 (各パート1箇所のみ指定可能)"
    // OpenMUCOM88互換: 全チャンネルが commonEndTick に達したら同時にループ。
    void globalLoopRestart()
    {
        // グローバルtickオフセット更新
        uint32_t loopLen = m_commonEndTick - m_commonLoopTick;
        if (loopLen == 0) loopLen = 1;
        m_loopTickOffset += loopLen;

        // 全チャンネル KEY_OFF + 状態リセット + Lポイントへ巻き戻し
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            auto& st = m_channels[ch];
            if (st.events.empty()) continue;

            // KEY_OFF
            if (st.noteOn) {
                if      (isFM(ch))  fmKeyOff(toFMIndex(ch));
                else if (isSSG(ch)) ssgKeyOff(toSSGIndex(ch));
                st.noteOn = false;
            }

            // eventIdxをループポイントに設定
            if (st.hasLoopPoint) {
                st.eventIdx = st.loopEventIdx;
            } else if (m_implicitLoop) {
                // L無し曲: 全チャンネルが先頭からループ（Z80 DATA TOP互換）
                st.eventIdx = 0;
            } else {
                // L有り曲のL無しチャンネル: ループせず沈黙を維持
                st.eventIdx = st.events.size();
            }

            // ランタイム状態リセット（全可変状態をデフォルト値に復元）
            // Issue #19: ループ2周目以降のSSGピッチずれ修正
            st.currentNote  = 0;
            st.noteOnCount  = 0;  // UI activityカウンタもリセット
            st.reverbActive = false;
            st.portaActive  = false;
            st.csmEnabled   = false;
            st.csmDetune[0] = st.csmDetune[1] = st.csmDetune[2] = st.csmDetune[3] = 0;
            // ピッチ関連
            st.detune       = 0;
            st.lfoPitchOffset = 0;
            st.lfoDelayCounter = 0;
            st.lfoStepCounter  = 0;
            st.lfoRateCounter  = 0;
            st.lfoDirection    = 1;
            st.lfoEnabled   = false;
            st.lfoDelay     = 0;
            st.lfoRate      = 1;
            st.lfoDepth     = 0;
            st.lfoCount     = 0;
            // 音量・パン・スタッカート
            st.volume       = 12;
            st.pan          = 3;
            st.staccato     = 0;
            // SSGエンベロープ
            st.ssgSoftEnv   = false;
            st.ssgEnvMode   = false;
            st.ssgEnvAL = st.ssgEnvAR = st.ssgEnvDR = 0;
            st.ssgEnvSL = st.ssgEnvSR = st.ssgEnvRR = 0;
            st.ssgEnvPhase  = 0;
            st.ssgEnvValue  = 0;
            st.ssgEnvKeyOnTick = false;
            st.ssgReleasing = false;
            st.ssgRelVol    = 0;
            // リバーブ
            st.reverbValue    = 0;
            st.reverbEnabled  = false;
            st.reverbQCutOnly = false;

            // SSGミキサーリセット（トーン有効、ノイズ無効）
            m_ssgMixer = 0x38;

            // ループ再開位置までのイベントを走査して状態復元
            for (size_t i = 0; i < st.eventIdx; i++) {
                const auto& ev = st.events[i];
                switch (ev.type) {
                case MmlEventType::TEMPO:    m_globalTempo = ev.value; break;
                case MmlEventType::VOLUME:   st.volume = ev.value; break;
                case MmlEventType::PATCH:
                    if (isFM(ch))  m_fmPatchNo[toFMIndex(ch)] = ev.value;
                    if (isSSG(ch)) ssgApplyPreset(toSSGIndex(ch), ev.value);
                    break;
                case MmlEventType::PAN:       st.pan = ev.value; break;
                case MmlEventType::STACCATO:  st.staccato = ev.value; break;
                case MmlEventType::DETUNE:    st.detune = ev.value; break;
                case MmlEventType::VIBRATO:
                    st.lfoEnabled = true;
                    st.lfoDelay = ev.vibDelay; st.lfoRate = ev.vibRate;
                    st.lfoDepth = ev.vibDepth; st.lfoCount = ev.vibCount;
                    break;
                case MmlEventType::VIBRATO_SWITCH:
                    st.lfoEnabled = (ev.value != 0);
                    break;
                case MmlEventType::LFO_PARAM:
                    switch (ev.vibDelay) {
                        case 0: st.lfoDelay = ev.value; break;
                        case 1: st.lfoRate  = std::max(ev.value, 1); break;
                        case 2: st.lfoDepth = ev.value; break;
                        case 3: st.lfoCount = std::max(ev.value, 1); break;
                    }
                    break;
                case MmlEventType::REG_WRITE: {
                    int addr = ev.note;
                    int data = ev.value;
                    // SSGミキサー仮想アドレス（0xF0-0xF2）の復元
                    if (addr >= 0xF0 && addr <= 0xF2) {
                        int si = addr - 0xF0;
                        bool toneOn  = (data & 1) != 0;
                        bool noiseOn = (data & 2) != 0;
                        if (toneOn)  m_ssgMixer &= ~(1 << si);
                        else         m_ssgMixer |=  (1 << si);
                        if (noiseOn) m_ssgMixer &= ~(1 << (si+3));
                        else         m_ssgMixer |=  (1 << (si+3));
                    }
                    break;
                }
                case MmlEventType::PORTAMENTO: {
                    // ポルタメント状態の復元（processEvents側と同一ロジック）
                    int startNote = ev.note;
                    int endNote   = ev.value;
                    int dur       = (int)ev.duration;
                    if (dur <= 0) dur = 1;
                    int sb = 0, eb = 0;
                    uint16_t sf = noteToFnum(startNote, sb);
                    uint16_t ef = noteToFnum(endNote, eb);
                    int startBF = (sb << 11) | sf;
                    int endBF   = (eb << 11) | ef;
                    st.portaActive      = true;
                    st.portaStartFnum   = startBF;
                    st.portaEndFnum     = endBF;
                    st.portaCurrentFnum = startBF;
                    st.portaTicksLeft   = dur;
                    st.portaStep        = (endBF - startBF) / dur;
                    break;
                }
                case MmlEventType::HARDWARE_LFO:
                    // HW LFO設定はレジスタ書き込みのみ（復元はplay後の再生で行われる）
                    // ここではst側の状態変更がないのでbreak
                    break;
                case MmlEventType::CSM_MODE:
                    st.csmDetune[0] = ev.vibDelay; st.csmDetune[1] = ev.vibRate;
                    st.csmDetune[2] = ev.vibDepth; st.csmDetune[3] = ev.vibCount;
                    st.csmEnabled = !(st.csmDetune[0] == 0 && st.csmDetune[1] == 0 &&
                                      st.csmDetune[2] == 0 && st.csmDetune[3] == 0);
                    break;
                case MmlEventType::REVERB_ENVELOPE:
                    st.reverbValue = ev.value; st.reverbEnabled = true; break;
                case MmlEventType::REVERB_SWITCH:
                    st.reverbEnabled = (ev.value != 0); break;
                case MmlEventType::REVERB_MODE:
                    st.reverbQCutOnly = (ev.value != 0); break;
                case MmlEventType::SSG_ENVELOPE:
                    st.ssgSoftEnv = true;
                    st.ssgEnvAL = ev.envAL; st.ssgEnvAR = ev.envAR;
                    st.ssgEnvDR = ev.envDR; st.ssgEnvSL = ev.envSL;
                    st.ssgEnvSR = ev.envSR; st.ssgEnvRR = ev.envRR;
                    break;
                default: break;
                }
            }
        }

        // Timer-B再計算 + 音色/PAN/音量復元
        recalcTimerB();
        for (int fi = 0; fi < MAX_FM_CHANNELS; fi++)
            fmApplyPatch(fi, m_fmPatchNo[fi]);
        // フェードアウト中のループ再開でキャリアTLが未減衰にならないよう
        // m_globalAtt を反映した音量を即時適用（Issue #19）
        for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) {
            int ch = fmMmlCh(fi);
            fmSetVolume(fi, m_channels[ch].volume);
        }
        for (int ch = 0; ch < MAX_MML_CHANNELS; ch++) {
            if (isFM(ch)) {
                int fi = toFMIndex(ch);
                int port = fmPort(fi);
                int off  = fmOffset(fi);
                m_engine->writeReg(port, 0xB4 + off,
                    (uint8_t)(panToReg(m_channels[ch].pan)));
            }
        }
        m_engine->writeReg(0, 0x07, m_ssgMixer);
    }

private:
    struct ChannelState {
        std::vector<MmlEvent> events;
        size_t   eventIdx    = 0;
        bool     noteOn      = false;
        uint32_t noteOnCount = 0;     // noteOnトリガーカウンター（UI activity検出用）
        int      currentNote = 0;
        int      staccato    = 0;
        int      volume      = 12;
        int      pan         = 3;   // パン（0=off, 1=right, 2=left, 3=center）
        bool     ssgEnvMode  = false; // SSGハードウェアエンベロープ（@N）
        // SSGソフトウェアエンベロープ（Eコマンド、MUCOM88 SOFENV互換）
        bool     ssgSoftEnv   = false; // Eコマンドで有効化
        int      ssgEnvAL     = 0;     // n1: Attack Level (初期値)
        int      ssgEnvAR     = 0;     // n2: Attack Rate
        int      ssgEnvDR     = 0;     // n3: Decay Rate
        int      ssgEnvSL     = 0;     // n4: Sustain Level (Decay目標値)
        int      ssgEnvSR     = 0;     // n5: Sustain Rate (毎tick減分, 0=保持)
        int      ssgEnvRR     = 0;     // n6: Release Rate
        // ADSR状態: 0=OFF, 1=ATTACK, 2=DECAY, 3=SUSTAIN, 4=RELEASE
        int      ssgEnvPhase  = 0;
        int      ssgEnvValue  = 0;     // 現在のエンベロープ値（0-255）
        bool     ssgEnvKeyOnTick = false; // Z80互換: KEY_ON tickではSOFENV進行をスキップ
        bool     ssgReleasing = false; // リリース中フラグ（Eコマンド未使用時の簡易版）
        int      ssgRelVol    = 0;     // リリース中の現在音量（簡易版）
        // デチューン: F-Numberオフセット（D コマンド）
        int      detune      = 0;
        // ソフトウェアLFO（M コマンド）
        bool     lfoEnabled  = false;   // MF1=true, MF0=false
        int      lfoDelay    = 0;       // 遅延（クロック数）
        int      lfoRate     = 1;       // クロック単位（何クロックごとに1ステップ）
        int      lfoDepth    = 0;       // 1ステップあたりのF-Number変化量
        int      lfoCount    = 0;       // 反転までのステップ数
        // LFO ランタイム状態
        int      lfoDelayCounter = 0;   // 遅延カウントダウン
        int      lfoStepCounter  = 0;   // 現在のステップ位置（0〜lfoCount）
        int      lfoRateCounter  = 0;   // レートカウンタ（0〜lfoRate）
        int      lfoDirection    = 1;   // 現在の進行方向（+1 or -1）
        int      lfoPitchOffset  = 0;   // 現在のピッチオフセット（F-Number単位）
        // ポルタメント（{}コマンド、MUCOM88 CULPTM互換）
        bool     portaActive    = false;   // ポルタメント中
        int      portaStartFnum = 0;       // 開始F-Number（block込み: block<<11 | fnum）
        int      portaEndFnum   = 0;       // 終了F-Number
        int      portaTicksLeft = 0;       // 残りtick数
        int      portaStep      = 0;       // 毎tickのF-Number増分（符号あり）
        int      portaCurrentFnum = 0;     // 現在のF-Number
        // リバーブ（Rコマンド、MUCOM88 REVERVE/REVSW/REVMOD互換）
        int      reverbValue    = 0;     // リバーブ音量加減値（IX+17）
        bool     reverbEnabled  = false; // リバーブON/OFF（IX+33 bit5）
        bool     reverbQCutOnly = false; // リバーブモード: true=qカット部分のみ（IX+33 bit4）
        bool     reverbActive   = false; // KEY_OFF済み=リバーブ減衰中
        int      reverbCurrentVol = 0;   // リバーブ減衰中の現在ボリューム（Z80 TOTALV互換、毎tick更新）
        // ループポイント（Lコマンド）
        size_t   loopEventIdx   = 0;   // ループ再開時のイベントインデックス
        uint32_t loopTick       = 0;   // ループ再開時のtick
        bool     hasLoopPoint   = false;
        // CSMモード（Sコマンド、FM ch3 エフェクトモード）
        // Z80 MDSET→TO_EFC/EXMODE: 毎tickで4オペレータ独立F-Number + 4回KEY ON
        bool     csmEnabled     = false;
        int      csmDetune[4]   = {0, 0, 0, 0};  // OP1-OP4 デチューンオフセット
    };

    IFmEngine*  m_engine;
    uint32_t    m_sampleRate;
    uint32_t    m_chipClock;  // YM2608マスタークロック（Issue #22）
    bool        m_playing = false;
    bool        m_loop    = true;
    // グローバル同期クロック（MUCOM88 INT3割り込み相当）
    uint32_t    m_globalTick        = 0;
    uint32_t    m_globalSampleAccum = 0;
    int         m_globalTempo       = 120;  // Timer-B値（BPMではない）
    // OpenMUCOM88完全互換 Timer-Bカウンタ（int: fmgenのtruncation挙動を再現）
    double      m_audioLeftMs       = 0.0;
    int         m_timerBCount       = 0;
    int         m_timerBPeriod      = 0;  // (256-T) * timer_stepd * 1024 (int truncation)
    int         m_wholeTick         = 128;  // Cコマンド（デフォルトC128）
    // 曲全体ループ（OpenMUCOM88 maxcount 互換）
    uint32_t    m_commonEndTick     = 0;    // 全チャンネル共通のループ終端tick
    bool        m_implicitLoop      = false; // L無し曲の暗黙ループ（全チャンネル先頭から）
    uint32_t    m_overrideEndTick  = 0;    // 外部から指定されたループ終端tick（0=未指定）
    uint32_t    m_commonLoopTick    = 0;    // 全チャンネル共通のLコマンドtick
    uint32_t    m_loopTickOffset    = 0;    // ループ巻き戻しの累積tickオフセット
    std::array<ChannelState, MAX_MML_CHANNELS> m_channels;
    std::unordered_map<int, FmPatch>           m_patchMap;
    std::array<int, MAX_FM_CHANNELS>           m_fmPatchNo;   // FM音色番号
    uint8_t     m_ssgMixer   = 0x3F;  // SSG mixer shadow (active-low)
    uint8_t     m_rhythmMask = 0;     // リズム楽器マスク（@N で設定）
    uint8_t     m_rhythmTL   = 0x3F;  // リズム全体音量TL（vコマンドで設定、0x3F=最大）
    // 楽器別 Individual Level レジスタ（0x18-0x1D）: bit7-6=PAN, bit4-0=Level
    // yコマンドやpコマンドで更新される。rhythmKeyOnで毎回書き込む。
    std::array<uint8_t, 6> m_rhythmIL = {0xDF,0xDF,0xDF,0xDF,0xDF,0xDF}; // L+R + level 31
    int         m_globalAtt  = 0;     // グローバル減衰（FM TL加算値、0=通常）
    int         m_pcmVolMode = 0;     // PVMODE: 0=IX+6のみ使用, 1=IX+6+IX+7
    int         m_pcmAddVol  = 0;     // ADPCM-B追加音量（PVMODE=1時のIX+7、V1→v設定）

    // ADPCM-B音楽チャンネル（Kトラック、ch10）
    // mucompcm.bin PCMADRテーブル: 8バイト/エントリ (startAddr, endAddr, reserved, param)
    static constexpr int MAX_PCM_VOICES = 32;
    struct PcmVoiceEntry {
        uint16_t startAddr = 0;
        uint16_t endAddr   = 0;
        uint16_t param     = 0;  // mucompcm.bin offset 26 のパラメータ
    };
    std::array<PcmVoiceEntry, MAX_PCM_VOICES> m_pcmTable;
    int      m_pcmVoiceCount = 0;
    bool     m_pcmLoaded     = false;
    int      m_pcmCurrentNum = 0;  // 現在のPCMサンプル番号（@Nで設定）
    uint8_t  m_pcmPan        = 0xC0;  // L+R

    // FM index → MML channel 逆引き
    static int fmMmlCh(int fi) { return (fi < 3) ? fi : (fi - 3 + 7); }

    // =====================================================================
    // イベント処理（チャンネル種別で分岐）
    // =====================================================================
    void processEvents(int ch, uint32_t tick)
    {
        auto& st = m_channels[ch];
        // 曲全体ループ: globalTickからグローバルloopTickOffsetを引いて
        // イベントの絶対tickと比較する（ループ巻き戻し後も正しく動作）
        uint32_t chTick = tick - m_loopTickOffset;
        while (st.eventIdx < st.events.size()) {
            const MmlEvent& ev = st.events[st.eventIdx];
            // commonEndTickを超えるイベントはスキップ（非破壊打ち切り、libmucom88-mml#2）
            // 同一tickのイベントは処理する（libmucom88-mml#3: >= → >）
            if (m_commonEndTick > 0 && ev.tick > m_commonEndTick) {
                // libmucom88-mml#5: SSG残留音防止
                // ブラケットループ展開でcommonEndTickを超えるNOTE_OFFがスキップされ
                // SSGが発音したまま残る問題を修正。Z80ではglobalLoopRestartで
                // 全チャンネルがKEY_OFFされるため、ここで明示的に消音する。
                if (st.noteOn) {
                    if (isSSG(ch)) {
                        if (st.ssgSoftEnv) {
                            st.ssgEnvPhase = 4;  // RELEASE
                        } else {
                            ssgKeyOff(toSSGIndex(ch));
                        }
                        st.noteOn = false;
                    } else if (isFM(ch)) {
                        fmKeyOff(toFMIndex(ch));
                        st.noteOn = false;
                    }
                }
                st.eventIdx = st.events.size();  // 残りイベントを全スキップ
                break;
            }
            if (ev.tick > chTick) break;

            switch (ev.type) {
            case MmlEventType::NOTE_ON:
                // Z80 FMSUB5→FMSUB4 タイ判定（music.asm line 529-557）:
                // FMSUB1は毎回SET 6,(IX+31)でKEYOFF_FLAG=1を設定する。
                // KEYOFF_FLAG=1の場合:
                //   FMSUB5: CALL NZ,KEYOFF → KEY_OFF実行（常に）
                //   FMSUB4: JR NZ,FMSUB9 → KEY_ON実行（常に）
                // KEYOFF_FLAG=0は0xFDカウントオーバー後のみ（通常到達しない）。
                //
                // したがってFMチャンネルでは、NOTE_ON時に必ずKEY_OFF→KEY_ONを行う。
                // SSGはTIEフラグON時にキーオフせず周波数だけ変更（SSSUB6互換）。
                if (st.noteOn && isSSG(ch)) {
                    // SSG: key-offせず周波数と音量を更新するだけ
                    // Z80 SSSUB6: TIEフラグON時はキーオフせずに周波数だけ変更
                    doKeyOn(ch, ev.note, ev.velocity);
                } else {
                    // Z80 FMSUB5: BIT 6,(IX+31) / CALL NZ,KEYOFF
                    if (st.reverbActive && isFM(ch)) {
                        doKeyOff(ch);
                    } else if (st.noteOn) {
                        doKeyOff(ch);
                    }
                    // Z80: FMSUB5→FMSUB4→FMSUB7→KEYON
                    // KEYON後: リバーブON時のみ STVOL（music.asm line 745-746）
                    if (ch == 2 && st.csmEnabled) {
                        // CSMモード: EXMODE互換 — 4オペレータ独立F-Number + 4回KEY ON
                        csmKeyOn(ch, ev.note, ev.velocity);
                    } else {
                        doKeyOn(ch, ev.note, ev.velocity);
                    }
                    if (isFM(ch) && st.reverbEnabled) {
                        doSetVolume(ch, st.volume);
                    }
                }
                st.noteOn       = true;
                st.noteOnCount++;
                st.currentNote  = ev.note;
                st.ssgReleasing = false;
                st.reverbActive = false;
                // SSGソフトウェアエンベロープ: ATTACK開始
                // Z80互換: SSSUBG→SOFEV7（音量計算のみ、エンベロープ進行なし）
                // KEY_ON tickではSOFENV進行をスキップし、SOFEV7相当の計算のみ行う
                if (isSSG(ch) && st.ssgSoftEnv) {
                    st.ssgEnvValue = st.ssgEnvAL;
                    st.ssgEnvPhase = 1;  // ATTACK
                    st.ssgEnvKeyOnTick = true;  // このtickではSOFENV進行スキップ
                }
                break;
            case MmlEventType::NOTE_OFF:
                if (st.noteOn && st.currentNote == ev.note) {
                    // Z80 FMSUB: wait==0でFMSUB1に直行（q=0でもFMSUB0はスキップ）
                    // FMSUB1→FMSUB5で KEY_OFF が実行されるため、
                    // 同tickの NOTE_OFF→NOTE_ON では NOTE_OFF 側の KEY_OFF は冗長。
                    // SSG: 同tick KEY_OFF→KEY_ON でクリックノイズ防止のためスキップ。
                    bool skipKeyOff = false;
                    if (isSSG(ch) && st.eventIdx + 1 < st.events.size()) {
                        const auto& next = st.events[st.eventIdx + 1];
                        if (next.tick == ev.tick && next.type == MmlEventType::NOTE_ON)
                            skipKeyOff = true;
                    }
                    if (!skipKeyOff) {
                        if (isFM(ch) && st.reverbEnabled) {
                            // FM リバーブ: KEY_OFFの代わりにFS2で音量設定
                            // Z80 FS2: C = (IX+6 + IX+17) >> 1 → STV2（TOTALV加算なし）
                            // IX+6は変更されない→毎tick同じ値を書く（定数）
                            fmSetReverbVolume(toFMIndex(ch), st.volume, st.reverbValue);
                            st.reverbActive = true;
                            // Z80互換: KEYOFFフラグ(IX+31 bit6)セットのみ
                            // noteOnは変更しない（LFO/ピッチ更新を継続するため）
                            // 実際のKEY_OFFはFMSUB5で次のノート開始時に行われる
                        } else if (isSSG(ch) && st.ssgSoftEnv) {
                            // SSGソフトウェアエンベロープ: RELEASE開始
                            st.ssgEnvPhase = 4;  // RELEASE
                            st.noteOn = false;
                        } else if (isSSG(ch)) {
                            // Eコマンド未使用時の簡易版リリース
                            st.ssgReleasing = true;
                            st.ssgRelVol = std::clamp(st.volume - m_globalAtt / 4, 0, 15);
                            st.noteOn = false;
                        } else {
                            doKeyOff(ch);
                            st.noteOn = false;
                        }
                    }
                }
                break;
            case MmlEventType::TIE_KEYOFF:
                // Z80 FMSUB3互換: ^タイ境界でのKEY_OFF/FS2処理
                // Rm1(reverbQCutOnly): FS3→実KEY_OFF（エンベロープRELEASE開始）
                // Rm0(reverb有効, !Rm1): FS2→リバーブ音量設定（KEY_OFFなし）
                if (st.noteOn && isFM(ch)) {
                    if (st.reverbQCutOnly) {
                        // Z80 FMSUB3: BIT 4,(IX+33)=1 → FS3 → CALL KEYOFF
                        doKeyOff(ch);
                        st.reverbActive = true;
                        // noteOnは維持（Z80もKEY_OFF後にLFO/ピッチ更新は継続）
                    } else {
                        // Z80 FMSUB3: BIT 5,(IX+33)=1 → FS2 → リバーブ音量
                        fmSetReverbVolume(toFMIndex(ch), st.volume, st.reverbValue);
                        st.reverbActive = true;
                    }
                }
                break;
            case MmlEventType::TEMPO:
                m_globalTempo = ev.value;  // テンポは全チャンネル共有
                break;
            case MmlEventType::VOLUME:
                if (isADPCMB(ch) && ev.note == 1) {
                    // PVMODE=1: v値をIX+7(追加音量)に格納。IX+6(baseVol)は変更しない
                    m_pcmVolMode = 1;
                    m_pcmAddVol = ev.value;
                    adpcmbSetVolume(st.volume);  // baseVol+addVolで再計算
                } else {
                    if (isADPCMB(ch) && ev.note == 0) {
                        // PVMODE=0: v値をIX+6(baseVol)に格納。IX+7は変更しない
                        m_pcmVolMode = 0;
                    }
                    st.volume = ev.value;
                    doSetVolume(ch, ev.value);
                }
                break;
            case MmlEventType::PATCH:
                doSetPatch(ch, ev.value);
                break;
            case MmlEventType::STACCATO:
                st.staccato = ev.value;
                break;
            case MmlEventType::DETUNE:
                st.detune = ev.value;
                // 発音中ならピッチを即時更新
                if (st.noteOn) updatePitch(ch);
                break;
            case MmlEventType::VIBRATO:
                st.lfoDelay = ev.vibDelay;
                st.lfoRate  = std::max(ev.vibRate, 1);
                st.lfoDepth = ev.vibDepth;
                st.lfoCount = std::max(ev.vibCount, 1);
                st.lfoEnabled = true;
                // LFO設定変更時にランタイム状態はリセットしない
                // （ノートオン時にリセットされる）
                break;
            case MmlEventType::VIBRATO_SWITCH:
                st.lfoEnabled = (ev.value != 0);
                if (!st.lfoEnabled) {
                    // LFO OFF: ピッチオフセットをクリアし即時反映
                    st.lfoPitchOffset = 0;
                    if (st.noteOn) updatePitch(ch);
                }
                break;
            case MmlEventType::LFO_PARAM:
                // MW/MC/ML/MD: LFO個別パラメータ変更
                switch (ev.vibDelay) {
                    case 0: st.lfoDelay = ev.value; break;              // MW
                    case 1: st.lfoRate  = std::max(ev.value, 1); break; // MC
                    case 2: st.lfoDepth = ev.value; break;              // ML
                    case 3: st.lfoCount = std::max(ev.value, 1); break; // MD
                }
                break;
            case MmlEventType::PAN:
                st.pan = ev.value;
                doSetPan(ch, ev.value);
                break;
            case MmlEventType::REST:
                if (st.noteOn || st.reverbActive) {
                    // Rm0(reverbQCutOnly=false): 休符でもリバーブ適用
                    // Rm1(reverbQCutOnly=true): 休符ではリバーブ適用しない→通常KEY_OFF
                    if (isFM(ch) && st.reverbEnabled && !st.reverbQCutOnly && st.noteOn) {
                        // Z80 FMSUB3: 休符時にリバーブON → FS2
                        fmSetReverbVolume(toFMIndex(ch), st.volume, st.reverbValue);
                        st.reverbActive = true;
                        // noteOnは維持（LFO継続）
                    } else {
                        doKeyOff(ch);
                        st.noteOn = false;
                        st.reverbActive = false;
                    }
                }
                break;
            case MmlEventType::REG_WRITE: {
                int addr = ev.note;
                int data = ev.value;
                // SSGミキサー仮想アドレス（0xF0-0xF2）: PコマンドのSSGミキサー制御
                if (addr >= 0xF0 && addr <= 0xF2) {
                    int si = addr - 0xF0;
                    // P0=無音(tone off,noise off), P1=トーン, P2=ノイズ, P3=トーン+ノイズ
                    // レジスタ0x07: bit0-2=tone(active low), bit3-5=noise(active low)
                    bool toneOn  = (data & 1) != 0;  // P1 or P3
                    bool noiseOn = (data & 2) != 0;  // P2 or P3
                    if (toneOn)  m_ssgMixer &= ~(1 << si);      // tone enable (active low)
                    else         m_ssgMixer |=  (1 << si);       // tone disable
                    if (noiseOn) m_ssgMixer &= ~(1 << (si+3));  // noise enable
                    else         m_ssgMixer |=  (1 << (si+3));   // noise disable
                    m_engine->writeReg(0, 0x07, m_ssgMixer);
                    break;
                }
                // 通常のレジスタ書き込み（yコマンド）
                int port = (addr >= 0x100) ? 1 : 0;
                m_engine->writeReg(port, (uint8_t)(addr & 0xFF), (uint8_t)(data & 0xFF));
                // リズム楽器 IL レジスタ（0x18-0x1D）への書き込みを追跡
                if (addr >= 0x18 && addr <= 0x1D) {
                    m_rhythmIL[addr - 0x18] = (uint8_t)(data & 0xFF);
                }
                break;
            }
            case MmlEventType::KEY_TRANSPOSE:
                // パーサー側でノート番号に適用済み。エンジンでは無処理。
                break;
            case MmlEventType::SSG_ENVELOPE:
                // SSGソフトウェアエンベロープ設定（Eコマンド）
                st.ssgSoftEnv = true;
                st.ssgEnvAL = ev.envAL;
                st.ssgEnvAR = ev.envAR;
                st.ssgEnvDR = ev.envDR;
                st.ssgEnvSR = ev.envSR;
                st.ssgEnvSL = ev.envSL;
                st.ssgEnvRR = ev.envRR;
                break;
            case MmlEventType::PORTAMENTO: {
                // ポルタメント: 次のNOTE_ONで発音される音のピッチスライドを設定
                // Z80 CULPTM→PLLFO→PLSKI2: F-Number(block込み14bit)への毎tick加算
                int startNote = ev.note;
                int endNote   = ev.value;
                int dur       = (int)ev.duration;
                if (dur <= 0) dur = 1;
                // F-Number 14bit = (block << 11) | fnum
                int sb = 0, eb = 0;
                uint16_t sf = noteToFnum(startNote, sb);
                uint16_t ef = noteToFnum(endNote, eb);
                int startBF = (sb << 11) | sf;
                int endBF   = (eb << 11) | ef;
                st.portaActive      = true;
                st.portaStartFnum   = startBF;
                st.portaEndFnum     = endBF;
                st.portaCurrentFnum = startBF;
                st.portaTicksLeft   = dur;
                st.portaStep        = (endBF - startBF) / dur;
                break;
            }
            case MmlEventType::REVERB_ENVELOPE:
                st.reverbValue = ev.value;
                st.reverbEnabled = true;  // R値設定時に自動ON（Z80: REVERVE→SET 5,(IX+33)）
                break;
            case MmlEventType::REVERB_SWITCH:
                st.reverbEnabled = (ev.value != 0);
                if (!st.reverbEnabled) {
                    // RF0: リバーブOFF→音量即時反映（Z80: REVSW→CALL STVOL）
                    st.reverbActive = false;
                    doSetVolume(ch, st.volume);
                }
                break;
            case MmlEventType::REVERB_MODE:
                st.reverbQCutOnly = (ev.value != 0);
                break;
            case MmlEventType::HARDWARE_LFO: {
                // ハードウェアLFO（Z80 HLFOON, music.asm:1033）
                // 0x22: LFO周波数(bit0-2) + ON(bit3)
                // 0xB4+ch: PAN(bit6-7) | AMS(bit4-5) | PMS(bit0-2)
                if (isFM(ch)) {
                    int freq = ev.vibDelay & 0x07;
                    int pms  = ev.vibRate & 0x07;
                    int ams  = ev.vibDepth & 0x03;
                    // レジスタ0x22: LFO ON + 周波数
                    m_engine->writeReg(0, 0x22, (uint8_t)(freq | 0x08));
                    // レジスタ0xB4+ch: PANビット保持 + AMS/PMS
                    int fi   = toFMIndex(ch);
                    int port = fmPort(fi);
                    int off  = fmOffset(fi);
                    int panBits = panToReg(st.pan) & 0xC0;
                    m_engine->writeReg(port, 0xB4 + off,
                        (uint8_t)(panBits | ((ams & 0x03) << 4) | (pms & 0x07)));
                }
                break;
            }
            case MmlEventType::CSM_MODE: {
                // FM ch3 CSMモード（Z80 MDSET→TO_EFC/EXMODE）
                // S n1,n2,n3,n4: OP1-OP4のデチューンオフセット設定
                // S0,0,0,0: 通常モード復帰（TO_NML）
                st.csmDetune[0] = ev.vibDelay;  // OP1
                st.csmDetune[1] = ev.vibRate;   // OP2
                st.csmDetune[2] = ev.vibDepth;  // OP3
                st.csmDetune[3] = ev.vibCount;  // OP4
                bool allZero = (st.csmDetune[0] == 0 && st.csmDetune[1] == 0 &&
                                st.csmDetune[2] == 0 && st.csmDetune[3] == 0);
                if (allZero) {
                    // TO_NML: 通常モード復帰（reg 0x27 = 0x3A）
                    st.csmEnabled = false;
                    if (m_engine)
                        m_engine->writeReg(0, 0x27, 0x3A);
                } else {
                    // TO_EFC: エフェクトモード有効化（reg 0x27 = 0x7A）
                    st.csmEnabled = true;
                    if (m_engine)
                        m_engine->writeReg(0, 0x27, 0x7A);
                }
                break;
            }
            case MmlEventType::LOOP_POINT:
                // ループ再開位置を記録（次のイベントから再開）
                st.hasLoopPoint = true;
                st.loopEventIdx = st.eventIdx + 1;
                st.loopTick     = ev.tick;
                break;
            case MmlEventType::END:
                st.eventIdx = st.events.size();
                return;
            default:
                break;
            }
            st.eventIdx++;
        }
    }

    // =====================================================================
    // 統合ディスパッチ（FM / SSG / Rhythm）
    // =====================================================================
    void doKeyOn(int ch, int noteNum, int velocity)
    {
        // LFOランタイム状態をリセット（ノートオンごとに遅延から再開）
        // Z80 LFORST+LFORST2: delay counter = delay, peak counter = peak/2(SRL A),
        // waveform position = initial depth, rate counter = rate
        auto& st = m_channels[ch];
        st.lfoDelayCounter = st.lfoDelay;
        st.lfoStepCounter  = st.lfoCount / 2;  // Z80 SETPEK: SRL A → peak/2
        st.lfoRateCounter  = 0;
        st.lfoDirection    = 1;
        st.lfoPitchOffset  = 0;

        if      (isFM(ch))     fmKeyOn(toFMIndex(ch), noteNum, velocity);
        else if (isSSG(ch))    ssgKeyOn(toSSGIndex(ch), noteNum);
        else if (isRhythm(ch)) rhythmKeyOn();
        else if (isADPCMB(ch)) adpcmbKeyOn(noteNum);
    }

    void doKeyOff(int ch)
    {
        if      (isFM(ch))     fmKeyOff(toFMIndex(ch));
        else if (isSSG(ch))    ssgKeyOff(toSSGIndex(ch));
        else if (isRhythm(ch)) rhythmKeyOff();
        else if (isADPCMB(ch)) adpcmbKeyOff();
    }

    void doSetVolume(int ch, int vol)
    {
        if      (isFM(ch))     fmSetVolume(toFMIndex(ch), vol);
        else if (isSSG(ch))    ssgSetVolume(toSSGIndex(ch), vol);
        else if (isRhythm(ch)) rhythmSetVolume(vol);
        else if (isADPCMB(ch)) adpcmbSetVolume(vol);
    }

    // MUCOM88 pan値 → YM2608レジスタ値変換
    // p0=off(0x00), p1=right(0x40), p2=left(0x80), p3=center(0xC0)
    static int panToReg(int pan) {
        static const int tbl[] = { 0x00, 0x40, 0x80, 0xC0 };
        return tbl[pan & 3];
    }

    void doSetPan(int ch, int pan)
    {
        if (isFM(ch)) {
            int fi = toFMIndex(ch);
            int port = fmPort(fi);
            int off  = fmOffset(fi);
            m_engine->writeReg(port, 0xB4 + off, (uint8_t)panToReg(pan));
        }
        // SSG: パンなし（モノラル）
        else if (isRhythm(ch)) {
            // リズムPAN: MUCOM88形式 p $NN
            // bit4-5: L/R (0=off, 1=R, 2=L, 3=LR)
            // bit0-3: 楽器インデックス (0-5)
            int inst = pan & 0x0F;
            int lr   = (pan >> 4) & 0x03;
            if (inst < 6) {
                // ILレジスタのPANビットのみ更新（レベルは保持）
                m_rhythmIL[inst] = (uint8_t)((lr << 6) | (m_rhythmIL[inst] & 0x1F));
            }
        }
    }

    void doSetPatch(int ch, int patchNo)
    {
        if (isFM(ch)) {
            int fi = toFMIndex(ch);
            m_fmPatchNo[fi] = patchNo;
            fmApplyPatch(fi, patchNo);
            // Z80 OTOPST (line 1172): CALL STENV → CALL STVOL
            // パッチロード後にSTVOLで現在のvolumeをキャリアTLに反映
            fmSetVolume(fi, m_channels[ch].volume);
        } else if (isSSG(ch)) {
            // SSG: @N でソフトウェアエンベロープのプリセットを選択
            // Z80 OTOSSG: SSGDAT テーブルから6バイト(AL,AR,DR,SL,SR,RR)をロードし
            // ENVPST で IX+12..17 にコピー、IX+6 に bit7(softEnv)|bit4(envMode) セット
            ssgApplyPreset(toSSGIndex(ch), patchNo);
        } else if (isRhythm(ch)) {
            // リズム: @N で楽器ビットマスクを設定
            // bit0=BD, bit1=SD, bit2=CY, bit3=HH, bit4=TM, bit5=RS
            m_rhythmMask = (uint8_t)(patchNo & 0x3F);
        } else if (isADPCMB(ch)) {
            // ADPCM-B: @N でPCMサンプル番号を選択
            m_pcmCurrentNum = patchNo;
        }
    }

    // Timer-B周期再計算（OpenMUCOM88 fmtimer.cpp完全互換）
    // fmgenはclock_を整数除算で計算するため、同じ整数除算を使用
    // fmgen: clock_ = 7987200/2 = 3993600, prescale=6, ratio=12
    //        fmclock = 3993600 / 6 / 12 = 55466 (int, 余り8切り捨て)
    void recalcTimerB()
    {
        static constexpr int    FMCLOCK_INT = 7987200 / 2 / 6 / 12;  // 55466 (fmgen互換)
        static constexpr double TIMER_STEPD = 1000.0 / FMCLOCK_INT * 16.0;

        int tb = 256 - m_globalTempo;
        if (tb <= 0) tb = 1;
        double calc = (double)tb * TIMER_STEPD;
        m_timerBPeriod = (int)(calc * 1024.0);  // fmgen互換: int truncation
        if (m_timerBPeriod <= 0) m_timerBPeriod = 1;
    }

    // 全消音
    void allSoundOff()
    {
        if (!m_engine) return;
        // FM 全キーオフ
        for (int fi = 0; fi < MAX_FM_CHANNELS; fi++) fmKeyOff(fi);
        // SSG 全振幅0 + ミキサー無効化
        m_ssgMixer = 0x3F;
        m_engine->writeReg(0, 0x07, m_ssgMixer);
        for (int i = 0; i < MAX_SSG_CHANNELS; i++)
            m_engine->writeReg(0, 0x08 + i, 0x00);
        // リズム全停止（Dump bit=1）
        m_engine->writeReg(0, 0x10, 0x80 | 0x3F);
    }

    // =====================================================================
    // FM ドライバー（既存ロジックをFMインデックス(0-5)ベースに変更）
    // =====================================================================
    static int fmPort(int fi)   { return (fi < 3) ? 0 : 1; }
    static int fmOffset(int fi) { return fi % 3; }

    void fmApplyPatch(int fi, int patchNo)
    {
        auto it = m_patchMap.find(patchNo);
        if (it != m_patchMap.end() && it->second.valid) {
            fmWritePatch(fi, it->second);
        } else {
            auto def = m_patchMap.find(0);
            if (def != m_patchMap.end() && def->second.valid)
                fmWritePatch(fi, def->second);
        }
    }

    void fmWritePatch(int fi, const FmPatch& patch)
    {
        if (!m_engine) return;
        int port = fmPort(fi);
        int off  = fmOffset(fi);

        // MUCOM88 STENV互換: 音色ロード前にKEY OFFし、SL/RR=0x0Fで最速リリース
        static const int slotOff[] = { 0, 8, 4, 12 };
        fmKeyOff(fi);
        for (int oi = 0; oi < 4; oi++)
            m_engine->writeReg(port, 0x80 + slotOff[oi] + off, 0x0F);

        // 音色パラメータ書き込み（MUCOM88 STENV: 6パラメータ×4オペレータ）
        for (int oi = 0; oi < 4; oi++) {
            int base = slotOff[oi] + off;
            const auto& op = patch.op[oi];
            m_engine->writeReg(port, 0x30 + base,
                (uint8_t)(((op.dt & 0x07) << 4) | (op.ml & 0x0F)));
            m_engine->writeReg(port, 0x40 + base, (uint8_t)(op.tl & 0x7F));
            m_engine->writeReg(port, 0x50 + base,
                (uint8_t)(((op.ks & 0x03) << 6) | (op.ar & 0x1F)));
            m_engine->writeReg(port, 0x60 + base, (uint8_t)(op.dr & 0x9F)); // bit7=AM, bit4-0=DR
            m_engine->writeReg(port, 0x70 + base, (uint8_t)(op.sr & 0x1F));
            m_engine->writeReg(port, 0x80 + base,
                (uint8_t)(((op.sl & 0x0F) << 4) | (op.rr & 0x0F)));
        }

        // FB/ALG
        m_engine->writeReg(port, 0xB0 + off,
            (uint8_t)(((patch.fb & 0x07) << 3) | (patch.al & 0x07)));
        // パン
        int mmlCh = (fi < 3) ? fi : (fi - 3 + 7);
        int panBits = panToReg(m_channels[mmlCh].pan);
        m_engine->writeReg(port, 0xB4 + off, (uint8_t)panBits);
    }

    // ── ソフトウェアLFO tick処理 ──────────────────────────
    void tickLfo(int ch)
    {
        auto& st = m_channels[ch];
        // 遅延カウントダウン
        if (st.lfoDelayCounter > 0) {
            st.lfoDelayCounter--;
            return;
        }
        // レートカウンタ: lfoRate tick ごとに1ステップ進む
        st.lfoRateCounter++;
        if (st.lfoRateCounter < st.lfoRate) return;
        st.lfoRateCounter = 0;

        // ピッチオフセットを変化させる
        st.lfoPitchOffset += st.lfoDepth * st.lfoDirection;
        st.lfoStepCounter++;

        // 反転: lfoCount ステップ到達で方向反転
        if (st.lfoStepCounter >= st.lfoCount) {
            st.lfoDirection = -st.lfoDirection;
            st.lfoStepCounter = 0;
        }

        // ピッチ即時反映
        updatePitch(ch);
    }

    // ── ポルタメント tick更新（Z80 PLLFO→PLSKI2互換）──────
    void tickPortamento(int ch)
    {
        auto& st = m_channels[ch];
        if (!st.portaActive) return;

        // F-Number(14bit = block<<11 | fnum)をステップ加算
        st.portaCurrentFnum += st.portaStep;
        st.portaTicksLeft--;

        if (st.portaTicksLeft <= 0) {
            st.portaCurrentFnum = st.portaEndFnum;
            st.portaActive = false;
        }

        // 14bit値からblock/fnumを分離してレジスタに書き込み
        int bf = st.portaCurrentFnum;
        if (bf < 0) bf = 0;
        int block = (bf >> 11) & 0x07;
        int fnum  = bf & 0x7FF;

        if (isFM(ch)) {
            int fi   = toFMIndex(ch);
            int port = fmPort(fi);
            int off  = fmOffset(fi);
            m_engine->writeReg(port, 0xA4 + off,
                (uint8_t)(((block & 0x07) << 3) | ((fnum >> 8) & 0x07)));
            m_engine->writeReg(port, 0xA0 + off, (uint8_t)(fnum & 0xFF));
        } else if (isSSG(ch)) {
            // SSG: 14bit block|fnum → SSGトーンピリオドに変換
            // PLLFO PLSKI2のSSG部: fnum >> octave でスケーリング
            int si = toSSGIndex(ch);
            int tp = fnum;
            // blockからオクターブシフト（block大→ピリオド小）
            if (block > 0) tp >>= block;
            tp = std::clamp(tp, 1, 0xFFF);
            m_engine->writeReg(0, si * 2,     (uint8_t)(tp & 0xFF));
            m_engine->writeReg(0, si * 2 + 1, (uint8_t)((tp >> 8) & 0x0F));
        }
    }

    // ── FM ch3 CSMモード KEY ON（Z80 EXMODE互換）──────────
    // Z80 EXMODE: ノートイベント時にOP1-OP4の独立F-Numberを設定し、各々KEY ONする。
    // OP1: 0xA6/0xA2（通常ch3レジスタ）
    // OP2: 0xAC/0xA8, OP3: 0xAD/0xA9, OP4: 0xAE/0xAA（ch3特殊モードレジスタ）
    void csmKeyOn(int ch, int noteNum, int /*velocity*/)
    {
        if (!m_engine) return;
        auto& st = m_channels[ch];

        // LFOランタイム状態をリセット（ノートオンごと）
        st.lfoDelayCounter = st.lfoDelay;
        st.lfoStepCounter  = 0;
        st.lfoRateCounter  = 0;
        st.lfoDirection    = 1;
        st.lfoPitchOffset  = 0;
        int pitchOffset = st.detune + st.lfoPitchOffset;
        int block = 4;
        uint16_t fnum = noteToFnum(noteNum, block);
        int adjusted = (int)fnum + pitchOffset;
        while (adjusted > 0x7FF && block < 7) { adjusted >>= 1; block++; }
        while (adjusted < 0 && block > 0)     { adjusted <<= 1; block--; }
        if (adjusted < 0) adjusted = 0;
        if (adjusted > 0x7FF) adjusted = 0x7FF;
        fnum = (uint16_t)adjusted;

        // 基準F-Number（16bit: block<<11 | fnum）
        int baseFnum = ((block & 0x07) << 11) | (fnum & 0x7FF);

        // OP1-OP4のF-Numberレジスタアドレス（ch3特殊モード）
        // Z80 EXMODE: FPORT=0xA4→(+IX+8=2)→0xA6 (OP1)
        //             FPORT=0xAA→0xAC (OP2), 0xAB→0xAD (OP3), 0xAC→0xAE (OP4)
        static const uint8_t msbRegs[4] = { 0xA6, 0xAC, 0xAD, 0xAE };
        static const uint8_t lsbRegs[4] = { 0xA2, 0xA8, 0xA9, 0xAA };

        // ch3 KEY ON: slot mask 0xF0 | ch3=2
        static const uint8_t keyOnData = 0xF0 | 0x02;

        for (int op = 0; op < 4; op++) {
            int opFnum = baseFnum + st.csmDetune[op];
            if (opFnum < 0) opFnum = 0;
            int opBlock = (opFnum >> 11) & 0x07;
            int opFn    = opFnum & 0x7FF;

            // F-Number書き込み（MSB→LSBの順、Z80 FMSUB6互換）
            m_engine->writeReg(0, msbRegs[op],
                (uint8_t)(((opBlock & 0x07) << 3) | ((opFn >> 8) & 0x07)));
            m_engine->writeReg(0, lsbRegs[op], (uint8_t)(opFn & 0xFF));

            // KEY ON（Z80 FMSUB6→FMSUB7→KEYON: 毎オペレータF-Number書き込み後にKEY ON）
            m_engine->writeReg(0, 0x28, keyOnData);
        }
    }

    // ── ピッチ更新（デチューン + LFOオフセット適用）─────
    void updatePitch(int ch)
    {
        auto& st = m_channels[ch];
        if (!st.noteOn) return;
        int offset = st.detune + st.lfoPitchOffset;
        if      (isFM(ch))  fmUpdateFreq(toFMIndex(ch), st.currentNote, offset);
        else if (isSSG(ch)) ssgUpdateFreq(toSSGIndex(ch), st.currentNote, offset);
    }

    // ── FM 周波数書き込み（オフセット付き）─────────────
    void fmWriteFreq(int fi, int noteNum, int pitchOffset)
    {
        if (!m_engine) return;
        int port = fmPort(fi);
        int off  = fmOffset(fi);

        int block = 4;
        uint16_t fnum = noteToFnum(noteNum, block);
        // ピッチオフセット適用（F-Number直接加算）
        int adjusted = (int)fnum + pitchOffset;
        // ブロック境界のキャリー処理
        while (adjusted > 0x7FF && block < 7) {
            adjusted >>= 1;
            block++;
        }
        while (adjusted < 0 && block > 0) {
            adjusted <<= 1;
            block--;
        }
        fnum = (uint16_t)std::clamp(adjusted, 0, 0x7FF);

        m_engine->writeReg(port, 0xA4 + off,
            (uint8_t)(((block & 0x07) << 3) | ((fnum >> 8) & 0x07)));
        m_engine->writeReg(port, 0xA0 + off, (uint8_t)(fnum & 0xFF));
    }

    void fmUpdateFreq(int fi, int noteNum, int pitchOffset)
    {
        fmWriteFreq(fi, noteNum, pitchOffset);
    }

    void fmKeyOn(int fi, int noteNum, int /*velocity*/)
    {
        if (!m_engine) return;
        // デチューン + LFOオフセット適用
        int mmlCh = fmMmlCh(fi);
        int offset = m_channels[mmlCh].detune + m_channels[mmlCh].lfoPitchOffset;
        fmWriteFreq(fi, noteNum, offset);

        uint8_t chKey = (fi < 3) ? (uint8_t)fi : (uint8_t)(fi - 3 + 4);
        m_engine->writeReg(0, 0x28, (uint8_t)(0xF0 | chKey));
    }

    void fmKeyOff(int fi)
    {
        if (!m_engine) return;
        uint8_t chKey = (fi < 3) ? (uint8_t)fi : (uint8_t)(fi - 3 + 4);
        m_engine->writeReg(0, 0x28, (uint8_t)(0x00 | chKey));
    }

    // MUCOM88 FMVDAT テーブル全20エントリ（Z80 music.asm:2700）
    // STV1: index = TOTALV(初期値4) + vol → FMVDAT[index]
    // FS2(リバーブ): index = (vol + R) >> 1 → STV2 → FMVDAT[index]（TOTALV加算なし）
    static constexpr int FMVDAT[20] = {
        0x36, 0x33, 0x30, 0x2D,  // FMVDAT[0-3]（STV1では通常使わない、FS2で使用）
        0x2A, 0x28, 0x25, 0x22,  // FMVDAT[4-7]  = vol 0-3
        0x20, 0x1D, 0x1A, 0x18,  // FMVDAT[8-11] = vol 4-7
        0x15, 0x12, 0x10, 0x0D,  // FMVDAT[12-15] = vol 8-11
        0x0A, 0x08, 0x05, 0x02   // FMVDAT[16-19] = vol 12-15
    };

    static constexpr int carrierOffsets[8][4] = {
        {12, -1, -1, -1},  // AL0: op4
        {12, -1, -1, -1},  // AL1: op4
        {12, -1, -1, -1},  // AL2: op4
        {12, -1, -1, -1},  // AL3: op4
        { 8, 12, -1, -1},  // AL4: op2,op4
        { 8,  4, 12, -1},  // AL5: op2,op3,op4
        { 8,  4, 12, -1},  // AL6: op2,op3,op4
        { 0,  8,  4, 12},  // AL7: 全op
    };

    // キャリアTLにFMVDATテーブル値を書き込む共通関数
    void fmWriteCarrierTL(int fi, int tlBase)
    {
        if (!m_engine) return;
        int port = fmPort(fi);
        int off  = fmOffset(fi);
        auto it = m_patchMap.find(m_fmPatchNo[fi]);
        int al = (it != m_patchMap.end()) ? it->second.al : 4;

        for (int oi = 0; oi < 4; oi++) {
            int so = carrierOffsets[al & 7][oi];
            if (so < 0) break;
            int tl = std::clamp(tlBase + m_globalAtt, 0, 127);
            m_engine->writeReg(port, 0x40 + so + off, (uint8_t)tl);
        }
    }

    // MUCOM88 STV1経由: FMVDAT[TOTALV + vol]（通常の音量設定）
    // TOTALV初期値=4 なので vol 0-15 → FMVDAT[4]-FMVDAT[19]
    void fmSetVolume(int fi, int vol)
    {
        int idx = std::clamp(vol + 4, 0, 19);
        fmWriteCarrierTL(fi, FMVDAT[idx]);
    }

    // MUCOM88 FS2→STV2経由: FMVDAT[(IX+6 + IX+17) >> 1]（リバーブ用）
    // Z80 IX+6にはコンパイラSETVOLが+4を加算済み（FM用: user_vol + TV_OFS + 4）
    // MmlEngineのvolは+4を含まないため、ここで加算する
    void fmSetReverbVolume(int fi, int vol, int reverbValue)
    {
        int idx = std::clamp((vol + 4 + reverbValue) >> 1, 0, 19);
        fmWriteCarrierTL(fi, FMVDAT[idx]);
    }

    // =====================================================================
    // SSG ドライバー
    //
    // レジスタマップ（port 0）:
    //   0x00-0x01: Ch A トーンピリオド（12bit: 下位8bit / 上位4bit）
    //   0x02-0x03: Ch B
    //   0x04-0x05: Ch C
    //   0x06:      ノイズピリオド（5bit）
    //   0x07:      ミキサー（active-low: bit0-2=Tone A/B/C, bit3-5=Noise A/B/C）
    //   0x08-0x0A: Ch A/B/C 振幅（bit4=envelope mode, bit0-3=固定振幅 0-15）
    //   0x0B-0x0C: エンベロープ周期（16bit）
    //   0x0D:      エンベロープ形状（4bit）
    // =====================================================================
    // ── SSG 周波数書き込み（オフセット付き）────────────
    void ssgWriteFreq(int si, int noteNum, int pitchOffset)
    {
        if (!m_engine) return;
        uint16_t tp = noteToSSGPeriod(noteNum, m_chipClock);
        // SSG: ピリオド値にオフセット（符号反転: F-Number増=周波数上昇=ピリオド減少）
        int adjusted = (int)tp - pitchOffset;
        tp = (uint16_t)std::clamp(adjusted, 1, 0xFFF);
        m_engine->writeReg(0, si * 2,     (uint8_t)(tp & 0xFF));
        m_engine->writeReg(0, si * 2 + 1, (uint8_t)((tp >> 8) & 0x0F));
    }

    void ssgUpdateFreq(int si, int noteNum, int pitchOffset)
    {
        ssgWriteFreq(si, noteNum, pitchOffset);
    }

    void ssgKeyOn(int si, int noteNum)
    {
        if (!m_engine) return;

        // トーンピリオド設定（デチューン + LFOオフセット適用）
        int mmlCh = si + 3;
        int offset = m_channels[mmlCh].detune + m_channels[mmlCh].lfoPitchOffset;
        ssgWriteFreq(si, noteNum, offset);

        // MUCOM88互換: ミキサーは初期化時に設定済み（0x38=トーン有効）
        // keyOn毎のミキサー操作はしない（MUCOM88のSSGはソフトウェアエンベロープで制御）

        // 振幅設定
        int vol = std::clamp(m_channels[mmlCh].volume - m_globalAtt / 4, 0, 15);
        uint8_t ampReg = (uint8_t)(vol & 0x0F);
        if (m_channels[mmlCh].ssgEnvMode) ampReg |= 0x10;
        m_engine->writeReg(0, 0x08 + si, ampReg);
    }

    // ── SSGソフトウェアエンベロープ tick更新（MUCOM88 SOFENV互換）──
    // Phase: 1=ATTACK, 2=DECAY, 3=SUSTAIN, 4=RELEASE
    void ssgTickEnvelope(int ch)
    {
        auto& st = m_channels[ch];
        int v = st.ssgEnvValue;
        switch (st.ssgEnvPhase) {
        case 1: // ATTACK: envelope += AR, 255でDECAYへ
            v += st.ssgEnvAR;
            if (v >= 255) { v = 255; st.ssgEnvPhase = 2; }
            break;
        case 2: // DECAY: envelope -= DR, SL以下でSUSTAINへ
            v -= st.ssgEnvDR;
            if (v <= st.ssgEnvSL) { v = st.ssgEnvSL; st.ssgEnvPhase = 3; }
            break;
        case 3: // SUSTAIN: envelope -= SR (SR=0なら保持)
            v -= st.ssgEnvSR;
            if (v < 0) v = 0;
            break;
        case 4: // RELEASE: envelope -= RR, 0で終了
            v -= st.ssgEnvRR;
            if (v <= 0) { v = 0; st.ssgEnvPhase = 0; }
            break;
        }
        st.ssgEnvValue = v;
    }

    void ssgKeyOff(int si)
    {
        if (!m_engine) return;
        // MUCOM88互換: ミキサーは触らない（トーンは有効のまま）
        // 音量を0にするだけで消音（MUCOM88ではソフトウェアエンベロープのRELEASE状態）
        m_engine->writeReg(0, 0x08 + si, 0x00);
    }

    void ssgSetVolume(int si, int vol)
    {
        if (!m_engine) return;
        int mmlCh = si + 3;
        if (m_channels[mmlCh].noteOn) {
            int v = std::clamp(vol - m_globalAtt / 4, 0, 15);
            uint8_t ampReg = (uint8_t)(v & 0x0F);
            if (m_channels[mmlCh].ssgEnvMode) ampReg |= 0x10;
            m_engine->writeReg(0, 0x08 + si, ampReg);
        }
    }

    // Z80 SSGDAT テーブル（music.asm:2162 + ssgdat.asm）
    // SSG @N プリセット: E(AL,AR,DR,SL,SR,RR), P(ミキサー), M(LFO: delay,rate,depth,count)
    // LFOパラメータはZ80 OTOSSG→LFOON経由で設定される
    static constexpr int SSGDAT_COUNT = 16;
    struct SsgPreset {
        int env[6];     // AL, AR, DR, SL, SR, RR
        int mixerP;     // P値: 1=トーン, 2=ノイズ, 3=トーン+ノイズ
        bool hasLfo;    // LFOパラメータあり
        int lfoDelay;   // M第1パラメータ: ノートオン後の遅延tick数
        int lfoRate;    // M第2パラメータ: 何tickで1ステップ進むか
        int lfoDepth;   // M第3パラメータ: ピッチ変化量（符号あり）
        int lfoCount;   // M第4パラメータ: 反転までのステップ数（負=無限）
    };
    static constexpr SsgPreset SSGDAT[SSGDAT_COUNT] = {
        {{255,255,255,255,  0,255}, 1, false, 0,0,0,0},       // @0: 持続
        {{255,255,255,200,  0, 10}, 1, false, 0,0,0,0},       // @1: 標準サステイン
        {{255,255,255,200,  1, 10}, 1, false, 0,0,0,0},       // @2: サステインレート1
        {{255,255,255,190,  0, 10}, 1, true,  16,1,25,4},     // @3: LFO付き
        {{255,255,255,190,  1, 10}, 1, true,  16,1,25,4},     // @4: LFO付き(SR=1)
        {{255,255,255,170,  0, 10}, 1, false, 0,0,0,0},       // @5: 速いリリース
        {{ 40, 70, 14,190,  0, 15}, 1, true,  16,1,24,5},     // @6: スローアタック+LFO
        {{120, 30,255,255,  0, 10}, 1, true,  16,1,25,4},     // @7: ベル風+LFO
        {{255,255,255,225,  8, 15}, 1, false, 0,0,0,0},       // @8: SR=8
        {{255,255,255,  1,255,255}, 2, false, 0,0,0,0},       // @9: ノイズ
        {{255,255,255,200,  8,255}, 2, false, 0,0,0,0},       // @10: ノイズ(SR=8)
        {{255,255,255,220, 20,  8}, 1, true,  1,1,300,-1},    // @11: 減衰ビブラート
        {{255,255,255,255,  0, 10}, 1, true,  1,1,-400,4},    // @12: ピッチダウン
        {{255,255,255,255,  0, 10}, 1, true,  1,1,80,-1},     // @13: ゆるいビブラート
        {{120, 80,255,255,  0,255}, 1, true,  1,1,-250,1},    // @14: 揺れ
        {{255,255,255,220,  0,255}, 1, true,  1,1,3000,-1},   // @15: 激しいビブラート
    };

    // Z80 OTOSSG → OTOCAL → ENVPST: SSGDATプリセットをソフトウェアエンベロープに適用
    // + LFO(M)パラメータ、ミキサーモード(P)も適用
    void ssgApplyPreset(int si, int presetNo)
    {
        int mmlCh = si + 3;
        int idx = presetNo & 0x0F;
        if (idx >= SSGDAT_COUNT) idx = 0;
        auto& st = m_channels[mmlCh];
        const auto& preset = SSGDAT[idx];

        // エンベロープ
        st.ssgSoftEnv = true;
        st.ssgEnvAL = preset.env[0];
        st.ssgEnvAR = preset.env[1];
        st.ssgEnvDR = preset.env[2];
        st.ssgEnvSL = preset.env[3];
        st.ssgEnvSR = preset.env[4];
        st.ssgEnvRR = preset.env[5];
        // Z80 ENVPST: OR 10010000B → bit7=softEnv ON, bit4=envMode ON
        st.ssgEnvMode = true;

        // ミキサーモード(P): Z80 OTOSSG→NOISE
        bool toneOn  = (preset.mixerP & 1) != 0;
        bool noiseOn = (preset.mixerP & 2) != 0;
        if (toneOn)  m_ssgMixer &= ~(1 << si);
        else         m_ssgMixer |=  (1 << si);
        if (noiseOn) m_ssgMixer &= ~(1 << (si+3));
        else         m_ssgMixer |=  (1 << (si+3));
        if (m_engine) m_engine->writeReg(0, 0x07, m_ssgMixer);

        // LFO(M): Z80 OTOSSG→LFOON
        if (preset.hasLfo) {
            st.lfoEnabled  = true;
            st.lfoDelay    = preset.lfoDelay;
            st.lfoRate     = preset.lfoRate;
            st.lfoDepth    = preset.lfoDepth;
            st.lfoCount    = preset.lfoCount;
            st.lfoDelayCounter = preset.lfoDelay;
            st.lfoRateCounter  = 0;
            st.lfoStepCounter  = 0;
            st.lfoPitchOffset = 0;
            st.lfoDirection   = 1;
        } else {
            st.lfoEnabled = false;
        }
    }

    // =====================================================================
    // ADPCM-B 音楽チャンネル（Kトラック、ch10）
    // Z80 music.asm: PCMGFQ→PLAY, mucompcm.binからPCMデータ+テーブルロード
    // =====================================================================
public:
    // mucompcm.bin からPCMADRテーブルとPCMデータをロード
    // format: [0x000-0x3FF] info table (32 bytes × 最大16エントリ)
    //         [0x400+]      raw ADPCM-B data
    // info entry: [0-15]=name, [26-27]=param, [28-29]=startAddr, [30-31]=length
    bool loadPcmData(const uint8_t* data, size_t size)
    {
        if (!data || size < 0x400 || !m_engine) return false;
        size_t infoSize = 0x400;
        (void)(size - infoSize);  // pcmSize: 今後のバリデーション用に予約

        // PCMADRテーブル構築
        m_pcmVoiceCount = 0;
        for (int i = 0; i < 16; i++) {
            const uint8_t* ent = data + i * 32;
            // 名前が空（最初のバイトが0）ならスキップ
            if (ent[0] == 0) continue;
            uint16_t startAddr = ent[28] | (ent[29] << 8);
            uint16_t length    = ent[30] | (ent[31] << 8);
            uint16_t param     = ent[26] | (ent[27] << 8);
            uint16_t endAddr   = startAddr + (length >> 2);  // mucomvm.cpp互換
            if (m_pcmVoiceCount < MAX_PCM_VOICES) {
                m_pcmTable[m_pcmVoiceCount].startAddr = startAddr;
                m_pcmTable[m_pcmVoiceCount].endAddr   = endAddr;
                m_pcmTable[m_pcmVoiceCount].param     = param;
                m_pcmVoiceCount++;
            }
        }

        // PCMデータのfmgenバッファへのロードは呼び出し側で行う
        // mmlEngine.loadPcmData() → テーブル解析のみ
        // fmgenEngine.loadPcmDataToAdpcmB(data + 0x400, size - 0x400) → PCMデータロード
        m_pcmLoaded = true;
        return true;
    }

    bool loadPcmFile(const std::string& path)
    {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) return false;
        size_t sz = (size_t)ifs.tellg();
        ifs.seekg(0);
        std::vector<uint8_t> buf(sz);
        ifs.read((char*)buf.data(), sz);
        return loadPcmData(buf.data(), sz);
    }

private:
    // ADPCM-B PCMデータのfmgenへのロードは呼び出し側で直接行う:
    //   fmgenEngine.loadPcmDataToAdpcmB(data + 0x400, size - 0x400);
    // loadPcmData() はPCMADRテーブルのみを解析する

    // Z80 PCMNMBテーブル（music.asm:3012-3015）
    // DW 49BAH+200 は Z80アセンブラで 0x49BA + 200(10進) = 0x49BA + 0xC8
    // C, C#, D, D#, E, F, F#, G, G#, A, A#, B
    static constexpr uint16_t PCMNMB[12] = {
        0x49BA + 200, 0x4E1C + 200, 0x52C1 + 200, 0x57AD + 200,  // C, C#, D, D#
        0x5CE4 + 200, 0x626A + 200, 0x6844 + 200, 0x6E77 + 200,  // E, F, F#, G
        0x7509 + 200, 0x7BFE + 120, 0x835E + 200, 0x8B2D + 200   // G#, A(+120), A#, B
    };

    // ノート番号からADPCM-BデルタN値を計算（Z80 PCMGFQ互換）
    // Z80: note byte 上位ニブル=シフト回数, 下位ニブル=音名(0-11)
    // MMLのo1がADPCM-Bの基準オクターブ（shift=0、原速再生）。
    // noteNum = (octave+1)*12 + semi なので o1c=24。shift = noteNum/12 - 2。
    uint16_t adpcmbNoteToDeltaN(int noteNum)
    {
        int semi    = noteNum % 12;
        int shift   = noteNum / 12 - 2;  // o1 = shift 0
        uint32_t dn = PCMNMB[semi];
        if (shift > 0) dn >>= shift;
        else if (shift < 0) dn <<= (-shift);  // o1より高いオクターブ
        return (uint16_t)(dn & 0xFFFF);
    }

    void adpcmbKeyOn(int noteNum)
    {
        if (!m_engine || !m_pcmLoaded) return;
        int idx = m_pcmCurrentNum - 1;  // Z80: DEC A (1-based → 0-based)
        if (idx < 0 || idx >= m_pcmVoiceCount) idx = 0;
        auto& pcm = m_pcmTable[idx];

        uint16_t deltaN = adpcmbNoteToDeltaN(noteNum);
        int vol = m_channels[10].volume;
        // Z80 PLAY volume: TOTALV*4 + IX+6 [+ IX+7], clamp 250
        // Z80コンパイラSTV4→STV1: ADPCM-BはIX+6 = user_vol（+4なし、TV_OFSなし）
        // PLAYルーチン: TOTALV*4 + IX+6 [+ IX+7(PVMODE=1時)]
        int finalVol = vol + m_globalAtt * 4;
        if (m_pcmVolMode != 0) finalVol += m_pcmAddVol;
        if (finalVol > 250) finalVol = 250;
        if (finalVol < 0) finalVol = 0;

        // Z80 PLAY register sequence (port 1)
        m_engine->writeReg(1, 0x0B, 0x00);              // mute
        m_engine->writeReg(1, 0x01, 0x00);              // pan off
        m_engine->writeReg(1, 0x00, 0x21);              // reset
        m_engine->writeReg(1, 0x10, 0x08);              // flag
        m_engine->writeReg(1, 0x10, 0x80);              // flag
        m_engine->writeReg(1, 0x02, (uint8_t)(pcm.startAddr & 0xFF));
        m_engine->writeReg(1, 0x03, (uint8_t)(pcm.startAddr >> 8));
        m_engine->writeReg(1, 0x04, (uint8_t)(pcm.endAddr & 0xFF));
        m_engine->writeReg(1, 0x05, (uint8_t)(pcm.endAddr >> 8));
        m_engine->writeReg(1, 0x09, (uint8_t)(deltaN & 0xFF));
        m_engine->writeReg(1, 0x0A, (uint8_t)(deltaN >> 8));
        m_engine->writeReg(1, 0x00, 0xA0);              // start playback
        m_engine->writeReg(1, 0x0B, (uint8_t)finalVol);  // volume
        m_engine->writeReg(1, 0x01, (uint8_t)(m_pcmPan)); // L/R
    }

    void adpcmbKeyOff()
    {
        if (!m_engine) return;
        m_engine->writeReg(1, 0x00, 0x01);  // stop playback
    }

    void adpcmbSetVolume(int vol)
    {
        if (!m_engine) return;
        // Z80 PLAY: TOTALV*4 + IX+6 [+ IX+7], clamp 250
        // Z80コンパイラSTV4→STV1: ADPCM-BはIX+6 = user_vol（+4なし、TV_OFSなし）
        int finalVol = vol + m_globalAtt * 4;
        if (m_pcmVolMode != 0) finalVol += m_pcmAddVol;
        if (finalVol > 250) finalVol = 250;
        if (finalVol < 0) finalVol = 0;
        m_engine->writeReg(1, 0x0B, (uint8_t)finalVol);
    }

    // =====================================================================
    // リズムドライバー（ADPCM-A: 6種の内蔵ドラム音源）
    //
    // レジスタマップ（port 0）:
    //   0x10: キーオン/Dump制御
    //         bit7=0: キーオン（bit0-5の楽器を発音開始）
    //         bit7=1: Dump（bit0-5の楽器を強制停止）
    //   0x11: 全体音量（TL: 6bit、0x3F=最大、0x00=無音 ※ymfm内部で^0x3Fされる）
    //   0x18-0x1D: 各楽器パン&音量（bit7=L, bit6=R, bit4-0=個別音量）
    //
    // 楽器ビット: bit0=BD, bit1=SD, bit2=CY, bit3=HH, bit4=TM, bit5=RS
    //
    // NOTE: ADPCM-A は YM2608 内蔵ROMのドラムサンプルを使用。
    //       ROMデータが未ロード（ymfm_external_read が0を返す）の場合は無音。
    // =====================================================================
    void rhythmKeyOn()
    {
        if (!m_engine || m_rhythmMask == 0) return;
        // 各楽器の IL（Individual Level: PAN + Level）を書き込み
        // yコマンドやpコマンドで設定された値をそのまま使用
        for (int i = 0; i < 6; i++) {
            if (m_rhythmMask & (1 << i)) {
                m_engine->writeReg(0, 0x18 + i, m_rhythmIL[i]);
            }
        }
        // 全体音量TL（MUCOM88互換: vコマンドの全体音量値を毎回書き込み）
        m_engine->writeReg(0, 0x11, (uint8_t)(m_rhythmTL & 0x3F));
        // キーオン（bit7=0）
        m_engine->writeReg(0, 0x10, m_rhythmMask & 0x3F);
    }

    void rhythmKeyOff()
    {
        if (!m_engine) return;
        // Dump（bit7=1）で楽器を停止
        m_engine->writeReg(0, 0x10, 0x80 | (m_rhythmMask & 0x3F));
    }

    void rhythmSetVolume(int vol)
    {
        if (!m_engine) return;
        // MUCOM88 リズム音量: v 0-63（全体音量）
        m_rhythmTL = (uint8_t)(std::clamp(vol, 0, 63) & 0x3F);
        m_engine->writeReg(0, 0x11, m_rhythmTL);
    }
};
