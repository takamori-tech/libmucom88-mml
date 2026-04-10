// =============================================================================
// mml_parser.hpp
// MUCOM88形式 MML パーサー
//
// 古代祐三氏が1987年に開発し、イース・ソーサリアン・ベア・ナックル等の
// 楽曲制作に使用したMUCOM88（MUSIC LALF）の文法に準拠。
// 対象音源: YM2608 (OPNA) = PC-8801 サウンドボードII / CLAUDIUSと同じチップ。
//
// 参考: https://github.com/onitama/mucom88/wiki/MMLReference
//       (CC BY-NC-SA 4.0, Onion Software / Yuzo Koshiro)
//
// ■ トラック構成（YM2608 / MUCOM88準拠）
//   A〜C : FM ch1〜3
//   D〜F : SSG ch1〜3（PSG互換矩形波）
//   G    : リズム音源（BD/SD/CY/HH/TM/RY）
//   H〜J : FM ch4〜6
//   K    : ADPCMチャンネル
//
// ■ ファイル形式（.muc）
//   #title    タイトル名
//   #composer 作曲者名
//   #voice    音色ファイル名
//   #pcm      PCMファイル名
//   ; コメント（行末まで）
//
// ■ MML文法（FMチャンネル）
//   c d e f g a b  : ドレミファソラシ（小文字）
//   c+ / c#        : シャープ
//   c-             : フラット
//   c4             : 4分音符（c8=8分、c16=16分）
//   c.             : 付点（1.5倍）
//   c^             : タイ延長（1クロック＝1/48全音符）
//   c4&c8          : タイ（従来形式も対応）
//   r4             : 休符
//   l8             : デフォルト音長（以降の音符のデフォルト）
//   o4             : オクターブ指定（o1〜o8）
//   <              : オクターブ1つ下
//   >              : オクターブ1つ上
//   t120           : テンポ（BPM）
//   v12            : ボリューム（0〜15）
//   @5             : 音色番号
//   q6             : スタッカート（1〜8、音符長全体に対するキーオン時間の比）
//   p3             : パン（0=なし、1=右、2=左、3=センター）
//
// ■ 音色定義（MMLファイル内に記述）
//   @(番号)  FB,AL
//     AR,DR,SR,RR,SL,TL,KS,ML,DT  ; op1
//     AR,DR,SR,RR,SL,TL,KS,ML,DT  ; op2
//     AR,DR,SR,RR,SL,TL,KS,ML,DT  ; op3
//     AR,DR,SR,RR,SL,TL,KS,ML,DT  ; op4
//   ※パラメーター順はMUCOM88準拠（AR,DR,SR,RR,SL,TL,KS,ML,DT）
//   ※ymfmへの書き込み順（DT,ML,TL,KS,AR,DR,SR,SL,RR）とは異なるので変換が必要
//
// ■ マクロ
//   #*(番号){内容}  : マクロ定義
//   *番号           : マクロ呼び出し
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <cctype>
#include <algorithm>
#include <cstring>
#include <sstream>
#include "fm_common.hpp"

// =============================================================================
// MmlEventType
// =============================================================================
enum class MmlEventType {
    NOTE_ON,    // 発音
    NOTE_OFF,   // 消音（NOTE_ONのdurationTicks後）
    REST,       // 休符
    TEMPO,      // テンポ変更
    VOLUME,     // ボリューム変更（v コマンド）
    PATCH,      // 音色変更（@ コマンド）
    PAN,        // パン変更（p コマンド）
    STACCATO,   // スタッカート変更（q コマンド）
    DETUNE,     // デチューン（D コマンド: F-Numberオフセット）
    VIBRATO,    // ソフトウェアLFO設定（M コマンド）
    VIBRATO_SWITCH, // ソフトウェアLFO ON/OFF（MF0/MF1）
    LOOP_POINT, // ループポイント（L コマンド: ループ再開位置）
    REG_WRITE,  // 直接レジスタ書き込み（y コマンド: addr=value, data=value2）
    KEY_TRANSPOSE, // キートランスポーズ（k コマンド: 全ノートを N 半音シフト）
    SSG_ENVELOPE,  // SSGソフトウェアエンベロープ（E コマンド: AL,AR,DR,SR,SL,RR）
    REVERB_ENVELOPE, // リバーブ音量加減値（R コマンド: value=加減値）
    REVERB_SWITCH,   // リバーブON/OFF（RF コマンド: value=0/1）
    REVERB_MODE,     // リバーブモード（Rm コマンド: value=0:休符含む/1:qカットのみ）
    PORTAMENTO,      // ポルタメント（{} コマンド: note=開始音, value=終了音, duration=tick数）
    HARDWARE_LFO,    // ハードウェアLFO（H コマンド: vibDelay=freq, vibRate=PMS, vibDepth=AMS）
    LFO_PARAM,       // LFO個別パラメータ変更（MW/MC/ML/MD: value=パラメータ値, vibDelay=種別 0=W,1=C,2=L,3=D）
    CSM_MODE,        // FM ch3 CSMモード（S コマンド: vibDelay-vibCount=OP1-OP4デチューン）
    TIE_KEYOFF,      // タイ境界KEY_OFF（Z80 FMSUB3互換: ^タイ接続点でKEY_OFF/FS2）
    END,        // チャンネル終端
};

// =============================================================================
// MmlEvent: 1イベント
// =============================================================================
struct MmlEvent {
    MmlEventType type        = MmlEventType::END;
    uint32_t     tick        = 0;       // 発生タイミング（tick）
    int          note        = 0;       // ノート番号（MIDI準拠、NOTE_ON/OFF）
    int          velocity    = 100;     // ベロシティ（0〜127）
    uint32_t     duration    = 0;       // 音符の長さ（tick、NOTE_ON）
    int          value       = 0;       // テンポ/ボリューム/パッチ/パン/スタッカート/デチューン値
    int          channel     = 0;       // トラック番号（A=0, B=1, ...）
    // VIBRATO イベント用パラメーター（M コマンド）
    int          vibDelay    = 0;       // 遅延（ノートオン後のクロック数）
    int          vibRate     = 0;       // クロック単位（何クロックごとに1ステップ進むか）
    int          vibDepth    = 0;       // 振幅（F-Number変化量、符号あり）
    int          vibCount    = 0;       // 反転までのステップ数
    // SSG_ENVELOPE イベント用パラメーター（E コマンド）
    int          envAL       = 0;       // n1: Attack Level (初期値)
    int          envAR       = 0;       // n2: Attack Rate (tick毎の増分)
    int          envDR       = 0;       // n3: Decay Rate (tick毎の減分)
    int          envSL       = 0;       // n4: Sustain Level (Decay目標値)
    int          envSR       = 0;       // n5: Sustain Rate (Sustain中のtick毎減分)
    int          envRR       = 0;       // n6: Release Rate (key-off後のtick毎減分)
};

// Mucom88Patch は fm_common.hpp で FmPatch として定義
// using Mucom88Patch = FmPatch; （fm_common.hpp で定義済み）

// =============================================================================
// MmlParser: MUCOM88形式MMLパーサー
//
// 1 tick = 1 MUCOM88クロック
// MUCOM88のデフォルト: 全音符 = C128 = 128クロック
// 本実装では 1tick = 1クロック として完全互換を実現
// =============================================================================
class MmlParser
{
public:
    static constexpr int PPQ        = 32;          // 4分音符 = 32 tick (= 32クロック)
    static constexpr int WHOLE_TICK = PPQ * 4;     // 全音符  = 128 tick (= C128)
    // MUCOM88の1クロック = 1tick（完全一致）
    static constexpr int CLOCK_TICK = 1;           // ^1 = 1tick = 1クロック

    // トラック文字 → チャンネル番号
    // A=0(FM1), B=1(FM2), C=2(FM3), D=3(FM4), E=4(FM5), F=5(FM6)
    // H=6(SSG1), I=7(SSG2), J=8(SSG3), G=9(Rhythm), K=10(ADPCM)
    // MUCOM88パート文字 → チャンネル番号
    // A=FM1(0), B=FM2(1), C=FM3(2)
    // D=SSG1(3), E=SSG2(4), F=SSG3(5)  ← SSGはD〜F
    // G=Rhythm(6)
    // H=FM4(7), I=FM5(8), J=FM6(9)     ← FM4〜6はH〜J
    // K=ADPCM(10)
    static int trackCharToChannel(char c) {
        c = std::toupper((unsigned char)c);
        if (c >= 'A' && c <= 'C') return c - 'A';        // FM1〜3: 0〜2
        if (c >= 'D' && c <= 'F') return 3 + (c - 'D');  // SSG1〜3: 3〜5
        if (c == 'G') return 6;                           // Rhythm: 6
        if (c >= 'H' && c <= 'J') return 7 + (c - 'H'); // FM4〜6: 7〜9
        if (c == 'K') return 10;                          // ADPCM: 10
        return -1;
    }

    // ==========================================================================
    // MucFile: .muc ファイル全体をパースした結果
    // ==========================================================================
    // 音源チップモード（MUCOM88EX拡張: #mode ディレクティブ）
    enum class ChipMode { OPNA, OPM, OPNB };

    struct MucFile {
        std::string title;
        std::string composer;
        std::string voiceFile;
        std::string pcmFile;
        ChipMode    chipMode  = ChipMode::OPNA; // デフォルトOPNA（#mode未指定時）
        int         wholeTick = WHOLE_TICK;  // Cコマンドの値（デフォルト128）

        // チャンネルごとのイベント列（インデックス = チャンネル番号）
        std::array<std::vector<MmlEvent>, 11> channelEvents;

        // 音色テーブル（@番号 → Mucom88Patch）
        std::unordered_map<int, Mucom88Patch> patches;
    };

    // ==========================================================================
    // voice.dat 読み込み（外部音色ファイル）
    // MUCOM88 形式: 256音色 × 32バイト = 8192バイト
    // MUCファイル内に定義がない音色番号を補完する。
    // ==========================================================================
    bool loadVoiceDat(const std::string& path)
    {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) return false;
        auto size = ifs.tellg();
        if (size < 32) return false;
        ifs.seekg(0);
        m_voiceDat.resize((size_t)size);
        ifs.read(reinterpret_cast<char*>(m_voiceDat.data()), size);
        return true;
    }

    bool loadVoiceDatFromMemory(const uint8_t* data, size_t size)
    {
        if (!data || size < 32) return false;
        m_voiceDat.assign(data, data + size);
        return true;
    }
    bool hasVoiceDat() const { return m_voiceDat.size() >= 32; }

    // ==========================================================================
    // parse: .muc形式のMML文字列全体をパース
    // ==========================================================================
    MucFile parse(const std::string& muc)
    {
        MucFile result;

        // マクロテーブル（#*N{...}）
        m_macros.clear();

        // 音色テーブル（@N ...）
        m_patches.clear();

        // チャンネル状態をリセット（複数行またぎで引き継ぐため）
        for (auto& st : m_chState) st = State{};

        // グローバルエコーパラメータをリセット（Z80 COMPIL: BFDAT=1相当）
        m_echoBufIdx = 0;  // Z80初期値: BFDAT=1 → index 0（最新のトーン）
        m_echoVolRed = 0;  // Z80初期値: VDDAT=0

        // 前処理：マクロと音色定義を先に収集
        collectMacros(muc);
        collectAllPatches(muc);

        // voice.dat から未定義音色を補完
        if (hasVoiceDat()) {
            int maxPatch = (int)(m_voiceDat.size() / 32);
            for (int i = 0; i < maxPatch; i++) {
                if (m_patches.count(i) && m_patches[i].valid) continue;
                Mucom88Patch p = parseVoiceDatEntry(i);
                if (p.valid) m_patches[i] = p;
            }
        }

        result.patches = m_patches;

        // 行ごとに処理
        std::istringstream stream(muc);
        std::string line;
        while (std::getline(stream, line)) {
            // ヘッダタグ・マクロ定義行はスキップ（収集済み）
            if (line.size() > 0 && line[0] == '#') {
                parseHeader(line, result);
                continue;
            }

            // 行頭がトラック文字か確認
            // MUCOM88: 各行の先頭にトラック文字（A〜F, G, H〜J, K）
            // 先頭の空白をスキップ
            size_t pos = 0;
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) pos++;
            if (pos >= line.size()) continue;

            char track = line[pos];
            int ch = trackCharToChannel(track);
            if (ch < 0) {
                // 音色定義行（@番号で始まる）はスキップ（収集済み）
                continue;
            }
            pos++;  // トラック文字をスキップ

            // コメント除去
            size_t commentPos = line.find(';', pos);
            std::string mmlStr = (commentPos != std::string::npos)
                ? line.substr(pos, commentPos - pos)
                : line.substr(pos);

            // マクロ展開
            mmlStr = expandMacros(mmlStr);

            // このチャンネルのMMLをパース
            parseChannelMml(mmlStr, ch, result.channelEvents[ch]);
        }

        // ── 後処理: 中間ENDを除去し、末尾に1つだけENDを残す ──
        // 複数行の同一トラックをパースすると行ごとにENDが挿入される
        // → 中間ENDはNOTE_ONより手前にあり再生を途中で止めてしまう
        for (int ch = 0; ch < 11; ch++) {
            auto& evs = result.channelEvents[ch];
            if (evs.empty()) continue;

            // ENDイベントを全て除去してから末尾に1つ追加
            uint32_t lastTick = 0;
            std::vector<MmlEvent> cleaned;
            cleaned.reserve(evs.size());
            for (auto& ev : evs) {
                if (ev.type == MmlEventType::END) {
                    if (ev.tick > lastTick) lastTick = ev.tick;
                    continue;  // ENDは除去
                }
                if (ev.tick > lastTick) lastTick = ev.tick;
                cleaned.push_back(ev);
            }
            // 末尾にENDを1つ追加
            MmlEvent endEv{};
            endEv.type    = MmlEventType::END;
            endEv.tick    = lastTick;
            endEv.channel = ch;
            cleaned.push_back(endEv);
            evs = std::move(cleaned);
        }

        // Cコマンドの値を結果に記録（最初のチャンネルのwholeTickを採用）
        for (int ch = 0; ch < 11; ch++) {
            if (m_chState[ch].wholeTick != WHOLE_TICK) {
                result.wholeTick = m_chState[ch].wholeTick;
                break;
            }
        }

        return result;
    }

private:
    // チャンネルごとのパース状態（複数行またぎで引き継ぐ）
    // ループスタック用フレーム
    struct LoopFrame {
        size_t   startPos;      // MML文字列内のループ開始位置
        uint32_t startTick;     // ループ開始tick
        size_t   eventStart;    // イベント配列のループ開始インデックス
        int      count;         // 繰り返し回数
        size_t   breakPos;      // /（ブレーク）の位置（npos=なし）
        uint32_t breakTick;     // /のtick
        size_t   breakEvIdx;    // /のイベントインデックス
        int      volumeAtStart = 0; // ループ開始時のボリューム（(/)累積補正用）
    };

    struct State {
        int      octave    = 6;  // MUCOM88デフォルト: o6（公式REF準拠）
        int      defLen    = 4;
        int      tempo     = 120;
        int      volume    = 12;
        int      patch     = 0;
        int      pan       = 3;
        int      staccato  = 0;   // q: 0=レガート（MUCOM88デフォルト）
        int      detune    = 0;   // D: デチューン（F-Numberオフセット）
        int      transpose = 0;   // k: キートランスポーズ（半音単位）
        bool     defLenIsClock = false; // l%N指定時: defLenがクロック値
        uint32_t tick      = 0;
        int      wholeTick = WHOLE_TICK; // Cコマンドで変更可能（デフォルトC128=128）
        bool     tieActive = false; // 行末タイ(&)継続フラグ
        int      tieNote   = -1;    // タイ継続中のノート番号
        // エコーマクロ（\コマンド、MUCOM88 SETBEF互換）
        // echoCount/echoVolRedはグローバル変数（m_echoBufIdx/m_echoVolRed）に移動
        // Z80コンパイラのBFDAT/VDDATは全チャンネル共有のグローバル変数
        uint32_t lastFullTicks = 0; // Z80 BEFCO互換: 直前ノートのフル音長（staccato前）
        int      pcmVolMode = 0;   // V0=baseVol(IX+6), V1=addVol(IX+7)（ADPCM-B用、現在未使用）
        int      tvOffset   = 0;   // V N: Total Volume Offset（Z80 muc88.asm TV_OFS互換）
        bool     reverbEnabled  = false; // リバーブON/OFF追跡（パーサー内、^タイ境界判定用）
        bool     reverbQCutOnly = false; // Rm1追跡（パーサー内、^タイ境界判定用）
        std::vector<LoopFrame> loopStack;  // ネストループ用スタック
    };

    std::unordered_map<int, std::string>    m_macros;
    std::unordered_map<int, Mucom88Patch>   m_patches;
    std::array<State, 11>                   m_chState;

    // Z80 BFDAT/VDDAT互換: グローバルエコーパラメータ（全チャンネル共有）
    // Z80コンパイラでは\=N,MのN(BFDAT)とM(VDDAT)は固定アドレスのグローバル変数
    int m_echoBufIdx = 0;   // BFDAT: BEFTONEバッファインデックス（N-1、初期値=1→0相当）
    int m_echoVolRed = 0;   // VDDAT: 音量減衰値（初期値=0）

    std::vector<uint8_t> m_voiceDat;  // voice.dat バイナリデータ

    // voice.dat の 1エントリ(32バイト)をパース（fm_common.hpp の共通関数を使用）
    Mucom88Patch parseVoiceDatEntry(int patchNo) const
    {
        return ::parseVoiceDatEntry(m_voiceDat.data(), m_voiceDat.size(), patchNo);
    }

    // voice.dat から音色名で検索（@"name" コマンド用）
    // MUCOM88 Z80コンパイラのSRCHPCM相当: name[6]フィールドをバイト列比較
    // 戻り値: 見つかった場合は音色番号(0-255)、見つからない場合は-1
    int findPatchByName(const std::string& name) const
    {
        if (m_voiceDat.size() < 32) return -1;
        int maxPatch = (int)(m_voiceDat.size() / 32);

        for (int i = 0; i < maxPatch; i++) {
            const uint8_t* rec = &m_voiceDat[(size_t)i * 32];
            // voice.dat byte 26-31: 音色名（6バイト、末尾0x00/0x20パディング）
            std::string entryName;
            for (int j = 0; j < 6; j++) {
                entryName += (char)rec[26 + j];
            }
            while (!entryName.empty() && (entryName.back() == ' ' || entryName.back() == '\0'))
                entryName.pop_back();
            if (entryName == name) return i;
        }
        return -1;
    }

    // ==========================================================================
    // ヘッダタグ処理
    // ==========================================================================
    void parseHeader(const std::string& line, MucFile& result)
    {
        auto startsWith = [&](const char* prefix) {
            return line.compare(0, strlen(prefix), prefix) == 0;
        };
        auto getValue = [&](size_t offset) -> std::string {
            size_t s = line.find_first_not_of(" \t", offset);
            return (s == std::string::npos) ? "" : line.substr(s);
        };

        if (startsWith("#title"))    result.title    = getValue(6);
        else if (startsWith("#composer")) result.composer = getValue(9);
        else if (startsWith("#voice"))   result.voiceFile = getValue(6);
        else if (startsWith("#pcm"))     result.pcmFile   = getValue(4);
        else if (startsWith("#mode")) {
            // MUCOM88EX拡張: #mode OPNA / #mode OPM / #mode OPNB
            auto mode = getValue(5);
            // 大文字に正規化
            for (auto& c : mode) c = (char)std::toupper((unsigned char)c);
            // 末尾空白・改行除去
            while (!mode.empty() && (mode.back() == ' ' || mode.back() == '\r' || mode.back() == '\n'))
                mode.pop_back();
            if (mode == "OPM" || mode == "G2")        result.chipMode = ChipMode::OPM;
            else if (mode == "OPNB" || mode == "NG")  result.chipMode = ChipMode::OPNB;
            else                                       result.chipMode = ChipMode::OPNA;
        }
    }

    // ==========================================================================
    // マクロ定義（#*N{...}）を収集
    // ==========================================================================
    void collectMacros(const std::string& muc)
    {
        std::istringstream stream(muc);
        std::string line;
        while (std::getline(stream, line)) {
            size_t pos = 0;
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) pos++;
            if (pos >= line.size()) continue;

            // `# *N{...}` or `#*N{...}` — # の後のスペースをスキップ
            if (line[pos] == '#') {
                size_t p = pos + 1;
                while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
                if (p < line.size() && line[p] == '*') {
                    pos = p + 1;
                    int macroNo = readInt(line, pos, -1);
                    if (macroNo >= 0 && pos < line.size() && line[pos] == '{') {
                        pos++;
                        size_t end = line.find('}', pos);
                        if (end != std::string::npos) {
                            m_macros[macroNo] = line.substr(pos, end - pos);
                        }
                    }
                }
            }
        }
    }

    // ==========================================================================
    // 全文から @N 音色定義を収集
    // ==========================================================================
    void collectAllPatches(const std::string& muc)
    {
        size_t pos = 0;
        while (pos < muc.size()) {
            // '@' を探す
            size_t atPos = muc.find('@', pos);
            if (atPos == std::string::npos) break;

            // '@%' はレジスタ直接形式（今回は非対応）
            if (atPos + 1 < muc.size() && muc[atPos+1] == '%') {
                pos = atPos + 1;
                continue;
            }

            // 音色定義の @ は行頭（改行直後またはファイル先頭、先頭空白OK）のみ対象
            // MMLトラック内の @10（例: "A t108 @10 v12"）を誤検出しないように
            // "英字の後の@" は除外する（トラック文字 + コマンド列内の @）
            bool isLineStart = (atPos == 0);
            if (!isLineStart) {
                // @ の前を行頭方向にスキャンして、改行かファイル先頭まで
                // 空白/タブのみなら行頭の音色定義と判定
                size_t scan = atPos;
                while (scan > 0 && (muc[scan-1] == ' ' || muc[scan-1] == '\t'))
                    scan--;
                if (scan == 0 || muc[scan-1] == '\n' || muc[scan-1] == '\r')
                    isLineStart = true;
            }
            if (!isLineStart) {
                pos = atPos + 1;
                continue;
            }

            size_t p = atPos + 1;
            if (p >= muc.size() || !std::isdigit((unsigned char)muc[p])) {
                pos = atPos + 1;
                continue;
            }

            // 音色番号
            int patchNo = readInt(muc, p, -1);
            if (patchNo < 0) { pos = atPos + 1; continue; }

            Mucom88Patch patch;
            patch.patchNo = patchNo;

            // FB,AL は @N と同じ行にある（例: "@10  2,5"）
            skipSpacesAndComments(muc, p);
            patch.fb = readInt(muc, p, 0);
            skipSep(muc, p);
            patch.al = readInt(muc, p, 0);

            // 4行分のオペレーターパラメーター（次行から）
            bool ok = true;
            for (int oi = 0; oi < 4 && ok; oi++) {
                // 次の行へ
                size_t lend = muc.find('\n', p);
                if (lend == std::string::npos) { ok = false; break; }
                p = lend + 1;
                skipSpacesAndComments(muc, p);

                // AR,DR,SR,RR,SL,TL,KS,ML,DT
                patch.op[oi].ar  = readInt(muc, p, 31); skipSep(muc, p);
                patch.op[oi].dr  = readInt(muc, p, 0);  skipSep(muc, p);
                patch.op[oi].sr  = readInt(muc, p, 0);  skipSep(muc, p);
                patch.op[oi].rr  = readInt(muc, p, 7);  skipSep(muc, p);
                patch.op[oi].sl  = readInt(muc, p, 0);  skipSep(muc, p);
                patch.op[oi].tl  = readInt(muc, p, 0);  skipSep(muc, p);
                patch.op[oi].ks  = readInt(muc, p, 0);  skipSep(muc, p);
                patch.op[oi].ml  = readInt(muc, p, 1);  skipSep(muc, p);
                patch.op[oi].dt  = readInt(muc, p, 0);
            }

            // 音色名パース: op4行末の ,"name" または ,\"name\"
            if (ok) {
                // op4のDTの後にカンマ+"文字列"があれば音色名
                while (p < muc.size() && (muc[p] == ' ' || muc[p] == '\t')) p++;
                if (p < muc.size() && muc[p] == ',') {
                    p++;
                    while (p < muc.size() && (muc[p] == ' ' || muc[p] == '\t')) p++;
                    if (p < muc.size() && muc[p] == '"') {
                        p++; // skip opening "
                        std::string name;
                        while (p < muc.size() && muc[p] != '"' && muc[p] != '\n' && muc[p] != '\r') {
                            name += muc[p++];
                        }
                        if (p < muc.size() && muc[p] == '"') p++;
                        patch.name = name;
                    }
                }
                patch.source = PatchSource::Inline;
                patch.valid = true;
                m_patches[patchNo] = patch;
            }
            pos = p;
        }
    }

    // ==========================================================================
    // マクロ展開
    // ==========================================================================
    std::string expandMacros(const std::string& mml)
    {
        std::string result;
        result.reserve(mml.size());
        size_t pos = 0;
        while (pos < mml.size()) {
            if (mml[pos] == '*' && pos + 1 < mml.size()
                && std::isdigit((unsigned char)mml[pos+1])) {
                pos++;
                int no = readInt(mml, pos, -1);
                auto it = m_macros.find(no);
                if (it != m_macros.end()) {
                    result += expandMacros(it->second);  // ネスト対応
                }
            } else {
                result += mml[pos++];
            }
        }
        return result;
    }

    // ==========================================================================
    // チャンネルMMLをパース → events に追加
    // ==========================================================================
    void parseChannelMml(const std::string& mml, int ch,
                         std::vector<MmlEvent>& events)
    {
        // クラスメンバーの State を使う（複数行またぎで octave 等が引き継がれる）
        State& st = m_chState[ch];

        // tick は既存イベントの末尾から継続
        if (!events.empty() && st.tick == 0) {
            for (const auto& ev : events) {
                if (ev.tick > st.tick) st.tick = ev.tick;
            }
        }

        size_t pos = 0;
        while (pos < mml.size()) {
            char c = std::tolower((unsigned char)mml[pos]);

            // 空白・区切り文字スキップ（|=視認性用区切り、Wiki準拠）
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '_' || c == '|') {
                pos++;
                continue;
            }
            // : — これ以降コンパイルしない（Wiki準拠）
            if (c == ':') {
                pos = mml.size();
                continue;
            }
            if (c == ';') {
                while (pos < mml.size() && mml[pos] != '\n') pos++;
                continue;
            }

            // エコーマクロ（\コマンド、MUCOM88 SETBEF互換）
            // Z80コンパイラのBFDAT/VDDATはグローバル変数 → m_echoBufIdx/m_echoVolRed
            // \=N,M: エコーパラメータ設定（N=トーンバッファインデックス1-9, M=音量減衰0-15）
            // \: 直前のノートを音量-Mで再発音（Z80では常に実行、"echo off"状態は存在しない）
            if (mml[pos] == '\\') {
                pos++;
                if (pos < mml.size() && mml[pos] == '=') {
                    // \=N,M: パラメータ設定（グローバル、全チャンネル共有）
                    pos++;
                    int n = 1;
                    if (pos < mml.size() && std::isdigit((unsigned char)mml[pos]))
                        n = readInt(mml, pos, 1);
                    if (n < 1) n = 1;
                    if (n > 9) n = 9;
                    m_echoBufIdx = n - 1;  // Z80 BFDAT = N-1（BEFTONEバッファインデックス）
                    if (pos < mml.size() && mml[pos] == ',') {
                        pos++;
                        if (pos < mml.size() && std::isdigit((unsigned char)mml[pos]))
                            m_echoVolRed = readInt(mml, pos, 0);
                    }
                } else if (st.lastFullTicks > 0) {
                    // \: 直前のノートをエコー再生（Z80 STBF3互換）
                    // Z80はBEFCO（直前ノートのフル音長、staccato適用前）を使用する。
                    // events[i].durationはstaccato適用後のkeyOnTicksなので、
                    // st.lastFullTicks（= Z80のBEFCO相当）でtickを進める。
                    // Z80 STBF3にはechoCount>0チェックがない — \は常にエコーを生成する
                    uint32_t echoDur = st.lastFullTicks;  // Z80 BEFCO互換
                    for (int i = (int)events.size() - 1; i >= 0; i--) {
                        if (events[i].type == MmlEventType::NOTE_ON && events[i].channel == ch) {
                            // 音量下げイベント
                            MmlEvent volDown{};
                            volDown.type = MmlEventType::VOLUME;
                            volDown.tick = st.tick;
                            // Z80 STBF3: 0xFB(-M) → VOLUPF → ADD A,(IX+6) — クランプなし
                            // FM: IX+6は+4オフセット含みのため負値許容（Issue #57と同じ）
                            // SSG: VOLUPS → RET NC で範囲外なら変更しない → 0クランプ
                            if (isFMChannel(ch)) {
                                volDown.value = st.volume - m_echoVolRed;  // FM: クランプなし
                            } else {
                                volDown.value = std::max(st.volume - m_echoVolRed, 0);
                            }
                            volDown.channel = ch;
                            volDown.note = 3;  // エコー音量マーカー（ブラケットループvolDelta補正対象、vコマンド検出除外）
                            events.push_back(volDown);

                            // ノート複製（同じ音高、フル音長で発音）
                            MmlEvent echoOn = events[i];
                            echoOn.tick = st.tick;
                            echoOn.duration = echoDur;
                            events.push_back(echoOn);

                            // NOTE_OFF
                            MmlEvent echoOff{};
                            echoOff.type = MmlEventType::NOTE_OFF;
                            echoOff.tick = st.tick + echoDur;
                            echoOff.note = events[i].note;
                            echoOff.channel = ch;
                            events.push_back(echoOff);

                            st.tick += echoDur;

                            // 音量復帰
                            MmlEvent volUp{};
                            volUp.type = MmlEventType::VOLUME;
                            volUp.tick = st.tick;
                            volUp.value = st.volume;
                            volUp.channel = ch;
                            volUp.note = 3;  // エコー音量マーカー
                            events.push_back(volUp);
                            break;
                        }
                    }
                }
                continue;
            }

            // 大文字 T = テンポ（BPM指定）→ Timer-B値に変換
            // 小文字 t = Timer-B直接値（switch文内で処理）
            if (mml[pos] == 'T' && pos + 1 < mml.size()
                && std::isdigit((unsigned char)mml[pos + 1])) {
                pos++;
                int bpm = readInt(mml, pos, 120);
                // BPM → Timer-B値変換
                // Timer-B period = (256-T) * 16 / fmclock
                // fmclock = 7987200/2/6/12 = 55466
                // tick per minute = fmclock / 16 / (256-T)
                // BPM = tick_per_min / PPQ_quarter
                // ここで PPQ_quarter = wholeTick/4 (C128の場合32)
                // T = 256 - fmclock / (16 * BPM * PPQ_quarter / 60)
                // T = 256 - 60 * fmclock / (16 * BPM * PPQ_quarter)
                static constexpr int FMCLOCK_INT = 7987200 / 2 / 6 / 12;  // 55466
                int ppqQ = st.wholeTick / 4;  // 4分音符あたりのクロック数
                if (ppqQ <= 0) ppqQ = 32;
                if (bpm <= 0) bpm = 120;
                int tb = 256 - (int)(60.0 * FMCLOCK_INT / (16.0 * bpm * ppqQ));
                tb = std::clamp(tb, 0, 255);
                st.tempo = tb;
                MmlEvent ev{};
                ev.type = MmlEventType::TEMPO;
                ev.tick = st.tick; ev.value = tb; ev.channel = ch;
                events.push_back(ev);
                continue;
            }

            // 大文字 R = リバーブ（MUCOM88 REVERVE/REVSW/REVMOD）
            // 小文字 r（休符）と区別するため tolower の前で処理
            if (mml[pos] == 'R') {
                if (pos + 1 < mml.size() && std::isalpha((unsigned char)mml[pos+1])) {
                    pos++; // skip 'R'
                    // サブコマンド文字列を取得
                    std::string sub;
                    while (pos < mml.size() && std::isalpha((unsigned char)mml[pos])) {
                        sub += (char)std::toupper((unsigned char)mml[pos]);
                        pos++;
                    }
                    int param = 0;
                    if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                        param = readInt(mml, pos, 0);

                    if (sub == "F") {
                        // RF0/RF1: リバーブスイッチ
                        MmlEvent ev{};
                        ev.type = MmlEventType::REVERB_SWITCH;
                        ev.tick = st.tick; ev.value = param; ev.channel = ch;
                        events.push_back(ev);
                        st.reverbEnabled = (param != 0);
                    } else if (sub == "M") {
                        // Rm0/Rm1: リバーブモード
                        MmlEvent ev{};
                        ev.type = MmlEventType::REVERB_MODE;
                        ev.tick = st.tick; ev.value = param; ev.channel = ch;
                        events.push_back(ev);
                        st.reverbQCutOnly = (param != 0);
                    }
                    // その他 (RG等) はスキップ
                } else {
                    // R<N>: リバーブ音量加減値（Z80: REVERVE, IX+17）
                    pos++; // skip 'R'
                    int rv = 0;
                    if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                        rv = readInt(mml, pos, 0);
                    MmlEvent ev{};
                    ev.type = MmlEventType::REVERB_ENVELOPE;
                    ev.tick = st.tick; ev.value = rv; ev.channel = ch;
                    events.push_back(ev);
                    st.reverbEnabled = true;  // Z80: REVERVE→SET 5,(IX+33)
                }
                continue;
            }

            // 大文字 V = Total Volume Offset（Z80コンパイラ TV_OFS互換）
            // Z80 muc88.asm TOTALV: V Nでコンパイル時のボリュームオフセットを設定
            // vコマンド値にTV_OFSを加算してバイトコードに格納する（ランタイムコマンドではない）
            // SSG: compiled_vol = v + TV_OFS, FM: compiled_vol = v + TV_OFS + 4
            if (mml[pos] == 'V') {
                pos++;
                int ofs = 0;
                if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                    ofs = readInt(mml, pos, 0);
                st.tvOffset = ofs;
                continue;
            }

            // 大文字 S = CSMモード / PCM制御コマンド（S n1,n2,n3,n4）
            // ch==2（Track C = FM ch3）: CSM_MODEイベント生成（Z80 MDSET→TO_EFC/EXMODE）
            // その他のチャンネル: スキップ（PCM制御等）
            if (mml[pos] == 'S' && pos + 1 < mml.size()
                && (std::isdigit((unsigned char)mml[pos+1]) || mml[pos+1] == '-')) {
                pos++;
                int params[4] = {0, 0, 0, 0};
                params[0] = readInt(mml, pos, 0);
                for (int pi = 1; pi < 4 && pos < mml.size() && mml[pos] == ','; pi++) {
                    pos++;
                    if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                        params[pi] = readInt(mml, pos, 0);
                }
                if (ch == 2) {
                    // FM ch3 CSMモード: S n1,n2,n3,n4 = OP1-OP4デチューンオフセット
                    // S0,0,0,0 = 通常モード復帰（TO_NML: reg 0x27 = 0x3A）
                    MmlEvent ev{};
                    ev.type = MmlEventType::CSM_MODE;
                    ev.tick = st.tick; ev.channel = ch;
                    ev.vibDelay = params[0];  // OP1 detune
                    ev.vibRate  = params[1];  // OP2 detune
                    ev.vibDepth = params[2];  // OP3 detune
                    ev.vibCount = params[3];  // OP4 detune
                    events.push_back(ev);
                }
                continue;
            }

            // 大文字 H = ハードウェアLFO（MUCOM88 HLFOON）
            // H freq,PMS,AMS  (Z80: 0x22=freq|0x08, 0xB4+ch=(PAN&0xC0)|(AMS<<4)|PMS)
            if (mml[pos] == 'H') {
                pos++;
                int freq = 0, pms = 0, ams = 0;
                if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                    freq = readInt(mml, pos, 0);
                if (pos < mml.size() && mml[pos] == ',') {
                    pos++;
                    if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                        pms = readInt(mml, pos, 0);
                }
                if (pos < mml.size() && mml[pos] == ',') {
                    pos++;
                    if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                        ams = readInt(mml, pos, 0);
                }
                MmlEvent ev{};
                ev.type = MmlEventType::HARDWARE_LFO;
                ev.tick = st.tick; ev.channel = ch;
                ev.vibDelay = freq;  // LFO周波数(0-7)
                ev.vibRate  = pms;   // PMS(0-7)
                ev.vibDepth = ams;   // AMS(0-3)
                events.push_back(ev);
                continue;
            }

            // 大文字 K = 移調絶対指定（-128〜+128）
            // 小文字 k と同じ機能（MUCOM88 wiki準拠）
            if (mml[pos] == 'K') {
                pos++;
                int trans = 0;
                if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                    trans = readInt(mml, pos, 0);
                st.transpose = trans;
                MmlEvent ev{};
                ev.type = MmlEventType::KEY_TRANSPOSE;
                ev.tick = st.tick; ev.value = trans; ev.channel = ch;
                events.push_back(ev);
                continue;
            }

            // 大文字 P = SSGミキサーモード / PCMパン
            // SSG: P0=無音, P1=トーン, P2=ノイズ, P3=トーン+ノイズ
            // FM/リズム/ADPCM: パン（$NN形式を含む）
            if (mml[pos] == 'P') {
                pos++;
                int pval = 0;
                if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-' || mml[pos] == '$'))
                    pval = readInt(mml, pos, 0);
                if (ch >= 3 && ch <= 5) {
                    // SSGチャンネル: ミキサーモードをREG_WRITEで設定
                    // P値をchに紐づけたSSG_MIXERイベントとして発行
                    // エンジン側でレジスタ0x07のビットを更新する
                    MmlEvent ev{};
                    ev.type = MmlEventType::REG_WRITE;
                    ev.tick = st.tick; ev.channel = ch;
                    // addr=0x07(mixer), data=P値+SSGインデックスを上位ビットにエンコード
                    // エンジン側で解釈: note=0xF0+si, value=P値
                    ev.note = 0xF0 + (ch - 3);  // 仮想アドレス: SSGミキサー制御
                    ev.value = pval & 0x03;
                    events.push_back(ev);
                }
                // FM/リズム/ADPCMの場合はスキップ（小文字pで処理済み）
                continue;
            }

            // 大文字 E = SSGソフトウェアエンベロープ（音符 e と区別）
            // E AL,AR,DR,SR,SL,RR — 6パラメータ（0-255）
            if (mml[pos] == 'E' && pos + 1 < mml.size()
                && (std::isdigit((unsigned char)mml[pos+1]) || mml[pos+1] == '-')) {
                pos++;
                MmlEvent ev{};
                ev.type = MmlEventType::SSG_ENVELOPE;
                ev.tick = st.tick; ev.channel = ch;
                ev.envAL = readInt(mml, pos, 0);
                if (pos < mml.size() && mml[pos] == ',') { pos++; ev.envAR = readInt(mml, pos, 0); }
                if (pos < mml.size() && mml[pos] == ',') { pos++; ev.envDR = readInt(mml, pos, 0); }
                if (pos < mml.size() && mml[pos] == ',') { pos++; ev.envSL = readInt(mml, pos, 0); }
                if (pos < mml.size() && mml[pos] == ',') { pos++; ev.envSR = readInt(mml, pos, 0); }
                if (pos < mml.size() && mml[pos] == ',') { pos++; ev.envRR = readInt(mml, pos, 0); }
                events.push_back(ev);
                continue;
            }

            // 大文字 D = デチューンコマンド（音符 d と区別）
            // Wiki: D+N = 相対（現在値に加算）、DN / D-N = 絶対指定
            if (mml[pos] == 'D') {
                pos++;
                int detune = 0;
                bool relative = false;
                if (pos < mml.size() && mml[pos] == '+') {
                    relative = true;
                    pos++;
                }
                if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                    detune = readInt(mml, pos, 0);
                if (relative)
                    detune = st.detune + detune;  // 相対: 現在値に加算
                st.detune = detune;
                MmlEvent ev{};
                ev.type = MmlEventType::DETUNE;
                ev.tick = st.tick; ev.value = detune; ev.channel = ch;
                events.push_back(ev);
                continue;
            }

            // 大文字 C = クロック設定（全音符のクロック数、デフォルトC128）
            if (mml[pos] == 'C' && pos + 1 < mml.size()
                && std::isdigit((unsigned char)mml[pos + 1])) {
                pos++;
                int clk = readInt(mml, pos, WHOLE_TICK);
                if (clk > 0) st.wholeTick = clk;
                continue;
            }

            // 大文字 L = ループポイント（MUCOM88: 曲ループ開始位置）
            if (mml[pos] == 'L' && (pos + 1 >= mml.size()
                || !std::isdigit((unsigned char)mml[pos + 1]))) {
                pos++;
                MmlEvent lp{};
                lp.type    = MmlEventType::LOOP_POINT;
                lp.tick    = st.tick;
                lp.channel = ch;
                events.push_back(lp);
                continue;
            }

            // 音符
            if (c >= 'a' && c <= 'g') {
                parseNote(mml, pos, ch, st, events);
                continue;
            }

            // 休符
            if (c == 'r') {
                pos++;
                uint32_t ticks;
                if (pos < mml.size() && mml[pos] == '%') {
                    // r%N: クロック直接指定
                    pos++;
                    ticks = readInt(mml, pos, 0);
                } else {
                    int len = 0;
                    bool explicitLen = false;
                    if (pos < mml.size() && std::isdigit((unsigned char)mml[pos])) {
                        len = readInt(mml, pos, st.defLen);
                        explicitLen = true;
                    } else {
                        len = st.defLen;
                    }
                    int dotCount = 0;
                    while (pos < mml.size() && mml[pos] == '.') { dotCount++; pos++; }
                    // Z80 SETRS2: 数値なしの場合COUNT(=defLen)をそのまま使用
                    // defLenIsClock時はクロック直接値として扱う（wholeTick/lenしない）
                    if (!explicitLen && st.defLenIsClock) {
                        ticks = len;
                        int dot = (int)ticks;
                        for (int d = 0; d < dotCount; d++) {
                            dot >>= 1;
                            ticks += dot;
                        }
                    } else {
                        bool tieDummy = false;
                        ticks = calcTicks(len, dotCount, st, mml, pos, tieDummy);
                    }
                }

                MmlEvent ev{};
                ev.type     = MmlEventType::REST;
                ev.tick     = st.tick;
                ev.duration = ticks;
                ev.channel  = ch;
                events.push_back(ev);
                st.tick += ticks;
                continue;
            }

            switch (c) {
            case 't': {  // テンポ（Timer-B直接値）
                // MUCOM88 Wiki: t = FM音源チップのタイマーBの数値を直接指定
                // MmlEngineのrecalcTimerB()で (256-t) を使ってTimer-B周期を計算する。
                // 大文字 T（BPM指定）は tolower 前のブロックで Timer-B値に変換済み。
                pos++;
                st.tempo = readInt(mml, pos, 200);
                MmlEvent ev{};
                ev.type = MmlEventType::TEMPO;
                ev.tick = st.tick; ev.value = st.tempo; ev.channel = ch;
                events.push_back(ev);
                break;
            }
            case 'v': {  // ボリューム
                pos++;
                if (ch == 6) {
                    // リズムチャンネル: v n1,n2,n3,n4,n5,n6,n7
                    // n1=全体(0-63), n2-n7=BD,SD,CY,HH,TM,RS個別(0-31)
                    st.volume = std::clamp(readInt(mml, pos, 60), 0, 63);
                    // 個別音量をREG_WRITEイベントとして出力（0x18-0x1D）
                    // MUCOM88 Z80ドライバーはv個別音量をPAN込みでレジスタに書く
                    for (int ri = 0; ri < 6 && pos < mml.size() && mml[pos] == ','; ri++) {
                        pos++;
                        int ilevel = 0;
                        if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                            ilevel = std::clamp(readInt(mml, pos, 0), 0, 31);
                        MmlEvent rw{};
                        rw.type = MmlEventType::REG_WRITE;
                        rw.tick = st.tick; rw.channel = ch;
                        rw.note = 0x18 + ri;
                        rw.value = 0xC0 | (ilevel & 0x1F);  // L+R + level
                        events.push_back(rw);
                    }
                } else if (ch == 10) {
                    // ADPCM-B: v 0-255（Wiki準拠）
                    // Z80 PCMVOL: PVMODE=0→IX+6(baseVol), PVMODE=1→IX+7(addVol)
                    st.volume = std::clamp(readInt(mml, pos, 128), 0, 255);
                    // pcmVolModeをVOLUMEイベントのnoteフィールドで通知
                    // note=0: baseVol(IX+6), note=1: addVol(IX+7)
                } else {
                    // FM/SSG: v 0-15
                    // Z80コンパイラ互換: TV_OFS(Vコマンド)を加算してからクランプ
                    // Z80 muc88.asm STV12(SSG): compiled_vol = TV_OFS + v
                    // Z80 muc88.asm FM path:    compiled_vol = TV_OFS + v + 4（+4はfmVolTable側で吸収済み）
                    st.volume = std::clamp(readInt(mml, pos, 12) + st.tvOffset, 0, 15);
                }
                MmlEvent ev{};
                ev.type = MmlEventType::VOLUME;
                ev.tick = st.tick; ev.value = st.volume; ev.channel = ch;
                // ADPCM-B: pcmVolMode をnoteフィールドで通知（0=baseVol, 1=addVol）
                if (ch == 10) ev.note = st.pcmVolMode;
                events.push_back(ev);
                break;
            }
            case 'l': {  // デフォルト音長
                pos++;
                if (pos < mml.size() && mml[pos] == '%') {
                    // l%N: デフォルト音長をクロック直接値で指定
                    pos++;
                    st.defLen = readInt(mml, pos, 32);
                    st.defLenIsClock = true;
                } else {
                    st.defLen = readInt(mml, pos, 4);
                    st.defLenIsClock = false;
                }
                break;
            }
            case 'o': {  // オクターブ
                pos++;
                st.octave = std::clamp(readInt(mml, pos, 4), 1, 8);
                break;
            }
            case '<': {  // オクターブ下
                pos++;
                st.octave = std::max(1, st.octave - 1);
                break;
            }
            case '>': {  // オクターブ上
                pos++;
                st.octave = std::min(8, st.octave + 1);
                break;
            }
            case '@': {  // 音色
                pos++;
                // '@%' はレジスタ直接形式（スキップ）
                if (pos < mml.size() && mml[pos] == '%') {
                    while (pos < mml.size() && mml[pos] != ' ' && mml[pos] != '\n') pos++;
                    break;
                }
                // '@"name"' は文字列音色名（MUCOM88拡張）
                // voice.datのname[6]フィールドとバイト列比較して音色番号を解決
                if (pos < mml.size() && mml[pos] == '"') {
                    pos++;
                    std::string voiceName;
                    while (pos < mml.size() && mml[pos] != '"')
                        voiceName += mml[pos++];
                    if (pos < mml.size()) pos++;  // 閉じ '"' をスキップ
                    int pno = findPatchByName(voiceName);
                    if (pno >= 0) {
                        st.patch = pno;
                        MmlEvent ev{};
                        ev.type = MmlEventType::PATCH;
                        ev.tick = st.tick; ev.value = pno; ev.channel = ch;
                        events.push_back(ev);
                    }
                    break;
                }
                st.patch = readInt(mml, pos, 0);
                MmlEvent ev{};
                ev.type = MmlEventType::PATCH;
                ev.tick = st.tick; ev.value = st.patch; ev.channel = ch;
                events.push_back(ev);
                break;
            }
            case 'q': {  // スタッカート
                // MUCOM88仕様: 音符長から q クロック分早めてキーオフ
                // q0 = レガート（キーオフしない）、デフォルト = 0（レガート）
                pos++;
                st.staccato = std::clamp(readInt(mml, pos, 0), 0, 127);
                MmlEvent ev{};
                ev.type = MmlEventType::STACCATO;
                ev.tick = st.tick; ev.value = st.staccato; ev.channel = ch;
                events.push_back(ev);
                break;
            }
            case 'p': {  // パン
                pos++;
                if (ch == 6) {
                    // リズムチャンネル: p $NN (bit0-3=楽器種類, bit4-5=パン)
                    // 複数回呼ばれる（楽器ごとにパン設定）
                    int pval = readInt(mml, pos, 0x33);
                    MmlEvent ev{};
                    ev.type = MmlEventType::PAN;
                    ev.tick = st.tick; ev.value = pval; ev.channel = ch;
                    events.push_back(ev);
                } else {
                    // FM/SSG: 0=なし, 1=右, 2=左, 3=センター
                    st.pan = std::clamp(readInt(mml, pos, 3), 0, 3);
                    MmlEvent ev{};
                    ev.type = MmlEventType::PAN;
                    ev.tick = st.tick; ev.value = st.pan; ev.channel = ch;
                    events.push_back(ev);
                }
                break;
            }
            // ── ループ構文 [...]N ──────────────────────
            case '[': {
                pos++;
                // ループ開始位置を記録（ネストスタック）
                LoopFrame lf;
                lf.startPos     = pos;
                lf.startTick    = st.tick;
                lf.eventStart   = events.size();
                lf.count        = 0;  // ]で決定
                lf.breakPos     = std::string::npos;
                lf.breakTick    = 0;
                lf.breakEvIdx   = 0;
                lf.volumeAtStart = st.volume; // (/)累積補正用
                st.loopStack.push_back(lf);
                break;
            }
            case '/': {
                // ループブレーク: 最終回はここから ] の後へジャンプ
                pos++;
                if (!st.loopStack.empty()) {
                    auto& lf = st.loopStack.back();
                    lf.breakPos   = pos;
                    lf.breakTick  = st.tick;
                    lf.breakEvIdx = events.size();
                }
                break;
            }
            case ']': {
                pos++;
                int count = 2;  // デフォルト2回
                if (pos < mml.size() && std::isdigit((unsigned char)mml[pos]))
                    count = readInt(mml, pos, 2);
                if (count < 1) count = 1;

                if (!st.loopStack.empty()) {
                    auto lf = st.loopStack.back();
                    st.loopStack.pop_back();

                    // 1回目はすでにパース済み。2回目以降を展開。
                    // 1回目のイベント列を保存
                    std::vector<MmlEvent> loopBody(
                        events.begin() + lf.eventStart, events.end());
                    uint32_t bodyTicks = st.tick - lf.startTick;
                    // Z80互換: ループ内 (/) ボリューム累積補正
                    // Z80はランタイムで毎回(/)を実行→累積的に音量が変化
                    // MmlParserは1回目のイベントをコピーするため、VOLUMEイベントの
                    // 絶対値を反復ごとにデルタ分ずらす必要がある
                    int volDelta = st.volume - lf.volumeAtStart; // 1反復あたりの音量変化
                    int maxVol = (ch == 10) ? 255 : (ch == 6) ? 63 : 15;

                    // ループ本体に絶対音量設定(vコマンド, note==0)がある場合はvolDelta=0
                    // vコマンドは毎回ボリュームをリセットするため、(/)の累積補正は不要
                    // 例: [v11(2bb)4]3 → v11が毎回リセット、各イテレーション同一
                    // note==2: (/)コマンド、note==3: エコー音量 → どちらも補正対象（volDelta有効）
                    // note==0: vコマンド（絶対設定）→ volDelta=0
                    if (volDelta != 0) {
                        for (size_t ei = lf.eventStart; ei < events.size(); ei++) {
                            if (events[ei].type == MmlEventType::VOLUME && events[ei].note == 0) {
                                volDelta = 0;
                                break;
                            }
                        }
                    }

                    // ブレーク情報
                    bool hasBreak = (lf.breakPos != std::string::npos);
                    size_t breakRelIdx = hasBreak ? (lf.breakEvIdx - lf.eventStart) : loopBody.size();
                    uint32_t breakRelTick = hasBreak ? (lf.breakTick - lf.startTick) : bodyTicks;

                    for (int rep = 1; rep < count; rep++) {
                        bool isLast = (rep == count - 1);
                        size_t limit = (isLast && hasBreak) ? breakRelIdx : loopBody.size();
                        uint32_t tickLimit = (isLast && hasBreak) ? breakRelTick : bodyTicks;
                        for (size_t ei = 0; ei < limit; ei++) {
                            MmlEvent ev = loopBody[ei];
                            // tick をベースからのオフセットに再計算
                            ev.tick = (loopBody[ei].tick - lf.startTick) + (lf.startTick + rep * bodyTicks);
                            // VOLUMEイベント: (/)による累積変化を反映
                            // note==2: 相対音量変更（(/)コマンド）— 補正対象
                            // note==3: エコー音量（\コマンド）— 補正対象
                            // note==0: 絶対音量設定（vコマンド）— 累積補正しない
                            if (ev.type == MmlEventType::VOLUME && volDelta != 0 && (ev.note == 2 || ev.note == 3)) {
                                // Z80 VOLUPF: FM はクランプなし（IX+6に+4含むため負値OK）
                                if (isFMChannel(ch)) {
                                    ev.value = ev.value + rep * volDelta;  // クランプなし
                                } else {
                                    ev.value = std::clamp(ev.value + rep * volDelta, 0, maxVol);
                                }
                            }
                            events.push_back(ev);
                        }
                        if (isLast && hasBreak)
                            st.tick = lf.startTick + rep * bodyTicks + tickLimit;
                        else
                            st.tick = lf.startTick + (rep + 1) * bodyTicks;
                    }
                    // パーサーのボリューム状態を最終反復の値に更新
                    if (volDelta != 0) {
                        // Z80 VOLUPF: FMはクランプなし（負値許容）
                        if (isFMChannel(ch)) {
                            st.volume = st.volume + (count - 1) * volDelta;
                        } else {
                            st.volume = std::clamp(st.volume + (count - 1) * volDelta, 0, maxVol);
                        }
                    }
                }
                break;
            }
            // ── 相対音量 ( ) ──────────────────────────
            // MUCOM88: ( = 音量下げ、) = 音量上げ
            // (N で N段階下げ、)N で N段階上げ（省略時1段階）
            case '(': {
                pos++;
                int delta = 1;
                if (pos < mml.size() && std::isdigit((unsigned char)mml[pos]))
                    delta = readInt(mml, pos, 1);
                // Z80 VOLUPF: ADD A,(IX+6) — クランプなし
                // FM: IX+6は+4オフセット含みのため、ユーザーvol=-2でもIX+6=2で有効
                // SSG VOLUPS: 結果が0未満または>=16なら変更を適用しない（RET NC）
                if (isFMChannel(ch)) {
                    st.volume -= delta;  // FM: クランプなし（Z80互換）
                } else {
                    st.volume = std::max(st.volume - delta, 0);
                }
                MmlEvent ev{};
                ev.type = MmlEventType::VOLUME;
                ev.tick = st.tick; ev.value = st.volume; ev.channel = ch;
                ev.note = 2;  // 相対音量変更マーカー（ブラケットループ展開時のvolDelta補正対象）
                events.push_back(ev);
                break;
            }
            case ')': {
                pos++;
                int delta = 1;
                if (pos < mml.size() && std::isdigit((unsigned char)mml[pos]))
                    delta = readInt(mml, pos, 1);
                int maxVol = (ch == 10) ? 255 : (ch == 6) ? 63 : 15;  // ADPCM=256段階, リズム=64, FM/SSG=16
                st.volume = std::min(st.volume + delta, maxVol);
                MmlEvent ev{};
                ev.type = MmlEventType::VOLUME;
                ev.tick = st.tick; ev.value = st.volume; ev.channel = ch;
                ev.note = 2;  // 相対音量変更マーカー（ブラケットループ展開時のvolDelta補正対象）
                events.push_back(ev);
                break;
            }
            // (Detune 'D' は大文字判定で先に処理済み)
            // ── 以下はスキップする未実装コマンド ──────────
            case 'm': {
                // M ビブラート: Mn1,n2,n3,n4 / MF / MW / MC / ML / MD
                pos++;
                if (pos < mml.size()) {
                    char sub = std::tolower((unsigned char)mml[pos]);
                    if (sub == 'f') {
                        // MF0 = LFO OFF, MF1 = LFO ON
                        pos++;
                        int sw = 0;
                        if (pos < mml.size() && std::isdigit((unsigned char)mml[pos]))
                            sw = readInt(mml, pos, 0);
                        MmlEvent ev{};
                        ev.type = MmlEventType::VIBRATO_SWITCH;
                        ev.tick = st.tick; ev.value = sw; ev.channel = ch;
                        events.push_back(ev);
                        break;
                    }
                    if (sub == 'w' || sub == 'c' || sub == 'l' || sub == 'd') {
                        // MW=delay, MC=clock_unit, ML=amplitude, MD=count
                        // LFO個別パラメータ変更（Z80 LFOON後に個別上書き）
                        int paramType = (sub == 'w') ? 0 : (sub == 'c') ? 1 : (sub == 'l') ? 2 : 3;
                        pos++;
                        int val = 0;
                        if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                            val = readInt(mml, pos, 0);
                        MmlEvent ev{};
                        ev.type = MmlEventType::LFO_PARAM;
                        ev.tick = st.tick; ev.channel = ch;
                        ev.vibDelay = paramType;
                        ev.value = val;
                        events.push_back(ev);
                        break;
                    }
                }
                {
                    // Mn1,n2,n3,n4 — カンマ区切り4パラメーター
                    int params[4] = {0, 0, 0, 0};
                    for (int pi = 0; pi < 4 && pos < mml.size(); pi++) {
                        if (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-')
                            params[pi] = readInt(mml, pos, 0);
                        if (pos < mml.size() && mml[pos] == ',') pos++;
                        else break;
                    }
                    MmlEvent ev{};
                    ev.type = MmlEventType::VIBRATO;
                    ev.tick = st.tick; ev.channel = ch;
                    ev.vibDelay = params[0];
                    ev.vibRate  = params[1];
                    ev.vibDepth = params[2];
                    ev.vibCount = params[3];
                    events.push_back(ev);
                }
                break;
            }
            case '{': {
                // ポルタメント {note1[duration]note2}
                // Z80: CULPTM(expand.asm:756)で開始音/終了音のF-Number差分を計算、
                //       毎tick F-Numberを更新してピッチスライド
                pos++; // skip '{'
                // 空白スキップ
                while (pos < mml.size() && (mml[pos] == ' ' || mml[pos] == '\t')) pos++;

                // 開始音をパースしてNOTE_ON/NOTE_OFFを生成
                size_t evCountBefore = events.size();
                if (pos < mml.size()) {
                    char nc = std::tolower((unsigned char)mml[pos]);
                    if (nc >= 'a' && nc <= 'g') {
                        parseNote(mml, pos, ch, st, events);
                    }
                }

                // 終了音をパース（ノート番号のみ取得、tick消費なし）
                // 空白スキップ
                while (pos < mml.size() && (mml[pos] == ' ' || mml[pos] == '\t')) pos++;
                int endNote = -1;
                if (pos < mml.size() && mml[pos] != '}') {
                    static const int noteOffsets[] = { 9, 11, 0, 2, 4, 5, 7 }; // a-g
                    char nc = std::tolower((unsigned char)mml[pos]);
                    if (nc >= 'a' && nc <= 'g') {
                        int semi = noteOffsets[nc - 'a'];
                        pos++;
                        if (pos < mml.size()) {
                            char sc = mml[pos];
                            if (sc == '+' || sc == '#') { semi++; pos++; }
                            else if (sc == '-')          { semi--; pos++; }
                        }
                        endNote = (st.octave + 1) * 12 + semi + st.transpose;
                        endNote = std::clamp(endNote, 0, 127);
                    }
                }

                // PORTAMENTOイベントをNOTE_ONの直前に挿入
                if (endNote >= 0 && events.size() > evCountBefore) {
                    // parseNoteが生成したNOTE_ONイベントを探す
                    for (size_t i = evCountBefore; i < events.size(); i++) {
                        if (events[i].type == MmlEventType::NOTE_ON) {
                            MmlEvent pe{};
                            pe.type     = MmlEventType::PORTAMENTO;
                            pe.tick     = events[i].tick;
                            pe.note     = events[i].note;  // 開始音
                            pe.value    = endNote;          // 終了音
                            pe.duration = events[i].duration; // tick数
                            pe.channel  = ch;
                            events.insert(events.begin() + (int)i, pe);
                            break;
                        }
                    }
                }

                // 閉じ '}' までスキップ
                while (pos < mml.size() && mml[pos] != '}') pos++;
                if (pos < mml.size()) pos++; // skip '}'
                break;
            }
            case '%': {
                // Z80 SETDCO: %N はCOUNT変数を直接設定（l%Nと同等）
                // 次のノート/レストのデフォルト長をNクロックに設定する
                // ※ st.tickに加算してはいけない（二重カウントになる）
                pos++;
                int val = readInt(mml, pos, 0);
                if (val > 0) {
                    st.defLen = val;
                    st.defLenIsClock = true;
                }
                break;
            }
            case 'y': {
                // 直接レジスタ書き込み y<addr>,<data>
                // yXX,slot,data 形式: FM拡張レジスタ名
                pos++;
                if (pos < mml.size() && std::isalpha((unsigned char)mml[pos])) {
                    // 拡張レジスタ名をパース
                    std::string regName;
                    while (pos < mml.size() && std::isalpha((unsigned char)mml[pos])) {
                        regName += (char)std::toupper((unsigned char)mml[pos]);
                        pos++;
                    }
                    // yDM/yTL/yKA/yDR/ySR/ySL/ySE → ベースレジスタ
                    int baseReg = -1;
                    if      (regName == "DM") baseReg = 0x30;
                    else if (regName == "TL") baseReg = 0x40;
                    else if (regName == "KA") baseReg = 0x50;
                    else if (regName == "DR") baseReg = 0x60;
                    else if (regName == "SR") baseReg = 0x70;
                    else if (regName == "SL") baseReg = 0x80;
                    else if (regName == "SE") baseReg = 0x90;

                    if (baseReg >= 0) {
                        // ,slot,data
                        int slot = 0, data = 0;
                        if (pos < mml.size() && mml[pos] == ',') { pos++; slot = readInt(mml, pos, 0); }
                        if (pos < mml.size() && mml[pos] == ',') { pos++; data = readInt(mml, pos, 0); }
                        // Z80互換: スロット番号は1-based（1-4）
                        // Z80 SETR4: slot 2↔3 スワップ後、DEC A, *4
                        //   slot 1→op1(off=0), 2→op3(off=8), 3→op2(off=4), 4→op4(off=12)
                        static const int slotOff[] = { 0, 8, 4, 12 };
                        int si = slot - 1;  // 1-based → 0-based
                        int off = (si >= 0 && si <= 3) ? slotOff[si] : 0;
                        // Z80 SETR8: COMNOW(チャンネル番号)をオフセットに加算
                        // FM ch A-C(0-2): port0, offset=ch
                        // FM ch H-J(7-9): port1(+0x100), offset=ch-7
                        int chOff = 0;
                        int portBase = 0;
                        if (ch <= 2) {
                            chOff = ch;   // A=0, B=1, C=2
                        } else if (ch >= 7 && ch <= 9) {
                            chOff = ch - 7;   // H=0, I=1, J=2
                            portBase = 0x100;  // port 1
                        }
                        int addr = portBase + baseReg + off + chOff;
                        MmlEvent ev{};
                        ev.type = MmlEventType::REG_WRITE;
                        ev.tick = st.tick; ev.channel = ch;
                        ev.note = addr;
                        ev.value = data;
                        events.push_back(ev);
                    }
                    // 未知のレジスタ名はスキップ
                    break;
                }
                int addr = readInt(mml, pos, 0);
                int data = 0;
                if (pos < mml.size() && mml[pos] == ',') {
                    pos++;
                    data = readInt(mml, pos, 0);
                }
                MmlEvent ev{};
                ev.type = MmlEventType::REG_WRITE;
                ev.tick = st.tick; ev.channel = ch;
                ev.note = addr;    // addr を note フィールドに格納
                ev.value = data;   // data を value フィールドに格納
                events.push_back(ev);
                break;
            }
            case 'k': {
                // キートランスポーズ相対指定（v1.7）: 現在値に加算（Wiki準拠）
                // 大文字 K は tolower 前のブロックで絶対指定として処理済み
                pos++;
                int trans = 0;
                if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                    trans = readInt(mml, pos, 0);
                st.transpose += trans;  // 相対: 現在値に加算
                MmlEvent ev{};
                ev.type = MmlEventType::KEY_TRANSPOSE;
                ev.tick = st.tick; ev.value = trans; ev.channel = ch;
                events.push_back(ev);
                break;
            }
            case 'w': {
                // SSGノイズ周波数 w (0-31) → レジスタ 0x06
                pos++;
                int noisePeriod = 0;
                if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                    noisePeriod = readInt(mml, pos, 0);
                // 残りのカンマ区切りパラメータをスキップ
                // （一部楽曲で w2,0,0,-100 等の拡張形式が使われる）
                while (pos < mml.size() && mml[pos] == ',') {
                    pos++;
                    if (pos < mml.size() && (std::isdigit((unsigned char)mml[pos]) || mml[pos] == '-'))
                        readInt(mml, pos, 0);
                }
                MmlEvent ev{};
                ev.type = MmlEventType::REG_WRITE;
                ev.tick = st.tick; ev.channel = ch;
                ev.note = 0x06;  // SSG noise period register
                ev.value = noisePeriod & 0x1F;
                events.push_back(ev);
                break;
            }
            case '!': {
                // Wiki: このパートのこれ以降を全て休符とする（発音しない）
                // パースループを抜けて以降のMMLを無視
                pos = mml.size();
                break;
            }
            // case 'R' は不要（tolower前に大文字Rとして処理済み）
            default:
                pos++;  // 未知文字はスキップ
                break;
            }
        }

        // 終端イベント
        MmlEvent end{};
        end.type = MmlEventType::END;
        end.tick = st.tick;
        end.channel = ch;
        events.push_back(end);
    }

    void parseNote(const std::string& mml, size_t& pos, int ch,
                   State& st, std::vector<MmlEvent>& events)
    {
        // ノート名 → 半音オフセット（Cからの距離）
        static const int noteOffsets[] = {
            9,  // a
            11, // b
            0,  // c
            2,  // d
            4,  // e
            5,  // f
            7,  // g
        };
        char c = std::tolower((unsigned char)mml[pos]);
        int semi = noteOffsets[c - 'a'];
        pos++;

        // シャープ / フラット
        if (pos < mml.size()) {
            char nc = mml[pos];
            if (nc == '+' || nc == '#') { semi++;  pos++; }
            else if (nc == '-')          { semi--;  pos++; }
        }

        // 音長（%N=クロック直接、数字=音符分割、なければデフォルト）
        int len = 0;
        uint32_t directTicks = 0;  // %N指定時のクロック直接値
        if (pos < mml.size() && mml[pos] == '%') {
            pos++;
            directTicks = readInt(mml, pos, 0);
        } else if (pos < mml.size() && std::isdigit((unsigned char)mml[pos])) {
            len = readInt(mml, pos, st.defLen);
        } else if (st.defLenIsClock) {
            // l%N でデフォルト音長がクロック値の場合
            directTicks = st.defLen;
        } else {
            len = st.defLen;
        }

        // 付点（複数ドット対応: b4.. = 24+12+6 = 42）
        int dotCount = 0;
        while (pos < mml.size() && mml[pos] == '.') { dotCount++; pos++; }

        // `^` クロック延長（MUCOM88独自）と `&` タイを処理してtickを累積
        bool tieOut = false;
        uint32_t ticks;
        // ^タイ境界収集（FMチャンネル+リバーブ有効時のみ）
        // Z80 FMSUB3: ^タイ接続点でRm1→FS3(KEYOFF), Rm0→FS2(リバーブ音量)
        bool collectTieBoundaries = isFMChannel(ch) && st.reverbEnabled;
        std::vector<uint32_t> tieBoundaries;
        std::vector<uint32_t> ampSegStarts;  // &セグメント開始位置（自動分割境界計算用）
        if (directTicks > 0) {
            // %N: クロック直接指定（付点・分割なし）
            ticks = directTicks;
            // ^延長・&タイは引き続き処理
            // MUCOM88はスペースを無視: `a ^ ^` = `a^^`（Issue #11: sq1_112）
            while (true) {
                size_t peek = pos;
                while (peek < mml.size() && (mml[peek] == ' ' || mml[peek] == '\t')) peek++;
                if (peek >= mml.size() || mml[peek] != '^') break;
                pos = peek;
                pos++;
                if (collectTieBoundaries)
                    tieBoundaries.push_back(ticks);  // ^境界位置（加算前）
                if (pos < mml.size() && mml[pos] == '%') {
                    pos++;
                    ticks += readInt(mml, pos, 0);
                } else {
                    int elen = 0;
                    if (pos < mml.size() && std::isdigit((unsigned char)mml[pos]))
                        elen = readInt(mml, pos, 0);
                    else
                        elen = st.defLen;
                    if (elen <= 0) elen = 4;
                    uint32_t t0 = (st.defLenIsClock && elen == st.defLen)
                                  ? (uint32_t)elen : st.wholeTick / elen;
                    int d = (int)t0;
                    while (pos < mml.size() && mml[pos] == '.') { pos++; d >>= 1; t0 += d; }
                    ticks += t0;
                }
            }
            // &タイ — Z80 TIEコマンド(KEY_OFFなし)
            // ただし各&セグメント内で独立にZ80自動分割が行われるため、セグメント開始を記録
            while (pos < mml.size() && mml[pos] == '&') {
                pos++;
                while (pos < mml.size() && (mml[pos] == ' ' || mml[pos] == '\t')) pos++;
                if (pos >= mml.size()) { tieOut = true; break; }
                char nc = std::tolower((unsigned char)mml[pos]);
                if (!(nc >= 'a' && nc <= 'g')) { tieOut = true; break; }
                pos++;
                if (pos < mml.size() && (mml[pos]=='+' || mml[pos]=='#' || mml[pos]=='-')) pos++;
                if (collectTieBoundaries)
                    ampSegStarts.push_back(ticks);  // &セグメント開始位置
                int tlen = 0;
                if (pos < mml.size() && mml[pos] == '%') {
                    pos++;
                    ticks += readInt(mml, pos, 0);
                } else if (pos < mml.size() && std::isdigit((unsigned char)mml[pos])) {
                    tlen = readInt(mml, pos, st.defLen);
                    uint32_t t0 = st.wholeTick / tlen;
                    int d = (int)t0;
                    while (pos < mml.size() && mml[pos] == '.') { pos++; d >>= 1; t0 += d; }
                    ticks += t0;
                } else {
                    // defLenIsClock時はクロック直接値を使用
                    uint32_t t0 = st.defLenIsClock
                                  ? (uint32_t)st.defLen : st.wholeTick / st.defLen;
                    int d = (int)t0;
                    while (pos < mml.size() && mml[pos] == '.') { pos++; d >>= 1; t0 += d; }
                    ticks += t0;
                }
            }
        } else {
            ticks = calcTicks(len, dotCount, st, mml, pos, tieOut,
                              collectTieBoundaries ? &tieBoundaries : nullptr,
                              collectTieBoundaries ? &ampSegStarts : nullptr);
        }

        // ノート番号（MIDI準拠: C4=60）+ キートランスポーズ
        int noteNum = (st.octave + 1) * 12 + semi + st.transpose;
        noteNum = std::clamp(noteNum, 0, 127);

        // タイ継続中（前の行末の&から続くノート）の場合:
        // NOTE_ONを出さず、前のNOTE_OFFを後ろにずらす
        if (st.tieActive && st.tieNote == noteNum) {
            // 前のNOTE_OFFを探して除去し、新しい位置に再配置
            // （最後のNOTE_OFFイベントを末尾から検索）
            for (int i = (int)events.size() - 1; i >= 0; i--) {
                if (events[i].type == MmlEventType::NOTE_OFF &&
                    events[i].note == noteNum && events[i].channel == ch) {
                    events.erase(events.begin() + i);
                    break;
                }
            }
            // NOTE_ONは出さず、NOTE_OFFだけ新しい位置に
            uint32_t keyOnTicks = ticks;
            if (st.staccato > 0) {
                uint32_t earlyOff = (uint32_t)st.staccato;
                keyOnTicks = (ticks > earlyOff) ? ticks - earlyOff : 1;
            }

            MmlEvent off{};
            off.type    = MmlEventType::NOTE_OFF;
            off.tick    = st.tick + (tieOut ? ticks : keyOnTicks);
            off.note    = noteNum;
            off.channel = ch;
            events.push_back(off);

            st.tieActive = tieOut;
            st.tieNote   = tieOut ? noteNum : -1;
            st.tick += ticks;
            st.lastFullTicks = ticks;  // Z80 BEFCO互換
            return;
        }

        // 通常のノート処理
        st.tieActive = false;
        st.tieNote   = -1;

        // スタッカート
        uint32_t keyOnTicks;
        if (st.staccato <= 0) {
            keyOnTicks = ticks;
        } else {
            uint32_t earlyOff = (uint32_t)st.staccato;
            keyOnTicks = (ticks > earlyOff) ? ticks - earlyOff : 1;
        }
        if (keyOnTicks == 0) keyOnTicks = 1;

        // NOTE_ON
        MmlEvent on{};
        on.type     = MmlEventType::NOTE_ON;
        on.tick     = st.tick;
        on.note     = noteNum;
        on.velocity = st.volume * 8;  // 0〜15 → 0〜120
        on.duration = tieOut ? ticks : keyOnTicks;
        on.channel  = ch;
        events.push_back(on);

        // ^タイ境界 + Z80自動分割境界のTIE_KEYOFFイベント生成（Z80 FMSUB3互換）
        // Z80コンパイラは7bit長制限（max 127 ticks/bytecode entry）で自動分割し、
        // 各分割点にtie bitを設定。FMSUB3でRm1→FS3(KEYOFF), Rm0→FS2(リバーブ音量)
        //
        // 自動分割は^セグメント内でのみ適用。&セグメント内の自動分割は対象外:
        // Z80では&境界でKEY_ON(FMSUB9)が即座に実行されるため、
        // 直前の自動分割KEY_OFFは1tick以内にKEY_ONで打ち消される。
        // 我々のエンジンには&境界のKEY_ONがないため、自動分割KEY_OFFを
        // 出すとリリース状態が継続し退行する。
        if (collectTieBoundaries) {
            // ^境界のみでセグメント分割（&境界は自動分割の区切りに使わない）
            std::vector<uint32_t> segPoints;
            segPoints.push_back(0);
            for (uint32_t b : tieBoundaries) segPoints.push_back(b);
            segPoints.push_back(ticks);
            // 各セグメント内の127tick超を自動分割
            std::vector<uint32_t> allBoundaries;
            for (size_t s = 0; s + 1 < segPoints.size(); s++) {
                uint32_t segStart = segPoints[s];
                uint32_t segEnd   = segPoints[s + 1];
                uint32_t segLen   = segEnd - segStart;
                // &境界がこのセグメント内にあるか確認
                // ある場合、自動分割を完全スキップ
                // Z80では&境界でKEY_ON(FMSUB9)が即座に実行されるため、
                // 自動分割KEY_OFFは1tick以内にKEY_ONで打ち消される。
                // 我々のエンジンには&境界のKEY_ONがないため自動分割は害になる。
                bool hasAmpInSegment = false;
                for (uint32_t as : ampSegStarts) {
                    if (as > segStart && as <= segEnd) {
                        hasAmpInSegment = true;
                        break;
                    }
                }
                // Z80: 127tick超のセグメントを127tickずつ分割（&なしの場合のみ）
                if (!hasAmpInSegment) {
                    uint32_t remain = segLen;
                    uint32_t p = segStart;
                    while (remain > 127) {
                        p += 127;
                        allBoundaries.push_back(p);
                        remain -= 127;
                    }
                }
                // ^境界は明示的KEY_OFF
                if (s + 1 < segPoints.size() - 1)
                    allBoundaries.push_back(segEnd);
            }
            // 重複除去・ソート
            std::sort(allBoundaries.begin(), allBoundaries.end());
            allBoundaries.erase(std::unique(allBoundaries.begin(), allBoundaries.end()),
                                allBoundaries.end());
            for (uint32_t boundary : allBoundaries) {
                MmlEvent tkev{};
                tkev.type    = MmlEventType::TIE_KEYOFF;
                tkev.tick    = st.tick + boundary;
                tkev.note    = noteNum;
                tkev.channel = ch;
                events.push_back(tkev);
            }
        }

        // NOTE_OFF（タイ継続中は出さない — 次行で延長される）
        if (!tieOut) {
            MmlEvent off{};
            off.type    = MmlEventType::NOTE_OFF;
            off.tick    = st.tick + keyOnTicks;
            off.note    = noteNum;
            off.channel = ch;
            events.push_back(off);
        } else {
            // タイ行末: NOTE_OFFを仮の位置に配置（次行で延長される）
            MmlEvent off{};
            off.type    = MmlEventType::NOTE_OFF;
            off.tick    = st.tick + ticks;
            off.note    = noteNum;
            off.channel = ch;
            events.push_back(off);
            st.tieActive = true;
            st.tieNote   = noteNum;
        }

        st.tick += ticks;
        st.lastFullTicks = ticks;  // Z80 BEFCO互換: エコー用にフル音長を記録
    }

    // 複数ドット読み取り + tick加算ヘルパー
    static uint32_t applyDots(uint32_t base, const std::string& mml, size_t& pos) {
        uint32_t add = base / 2;
        while (pos < mml.size() && mml[pos] == '.' && add > 0) {
            base += add; add /= 2; pos++;
        }
        return base;
    }

    // FMチャンネル判定（MmlEngine::isFMと同一）
    static bool isFMChannel(int ch) { return ch <= 2 || (ch >= 7 && ch <= 9); }

    // ==========================================================================
    // tick計算（付点・^ クロック延長・& タイを処理）
    // tieOut: trueが返るとタイが行末で切れた（次行の最初のノートを結合すべき）
    // tieBoundaries: ^タイ境界のオフセット（ノート先頭からの相対tick）を収集
    // ampSegStarts: &タイのセグメント開始オフセット（自動分割の境界計算用、KEY_OFFは不要）
    // ==========================================================================
    uint32_t calcTicks(int len, int dotCount, const State& st,
                       const std::string& mml, size_t& pos,
                       bool& tieOut,
                       std::vector<uint32_t>* tieBoundaries = nullptr,
                       std::vector<uint32_t>* ampSegStarts = nullptr)
    {
        tieOut = false;
        if (len <= 0) len = 4;
        int wt = st.wholeTick;
        uint32_t ticks = wt / len;
        // 複数ドット対応: b4. = 24+12, b4.. = 24+12+6, b4... = 24+12+6+3
        { uint32_t add = ticks / 2;
          for (int d = 0; d < dotCount && add > 0; d++) {
              ticks += add; add /= 2;
          }
        }

        // `^` 音長延長（MUCOM88: ^N で N分音符分延長、^単独はデフォルト音長(l値)分）
        // Z80互換: ^の後に数値がない場合はdefLen（lコマンドで設定）を使用
        // ノート自体の音長ではない（例: l4 e1^. = 128 + dotted_l4(48) = 176）
        // MUCOM88はスペースを無視: `a ^ ^` = `a^^`（Issue #11: sq1_112）
        while (true) {
            // ^の前のスペースをスキップ（先読みで^がある場合のみ消費）
            size_t peek = pos;
            while (peek < mml.size() && (mml[peek] == ' ' || mml[peek] == '\t')) peek++;
            if (peek >= mml.size() || mml[peek] != '^') break;
            pos = peek;  // ^が見つかったのでスペースを消費
            pos++;
            if (tieBoundaries)
                tieBoundaries->push_back(ticks);  // ^境界位置（加算前）
            if (pos < mml.size() && mml[pos] == '%') {
                // ^%N: クロック直接指定（付点なし）
                pos++;
                ticks += readInt(mml, pos, 0);
            } else {
                int elen = 0;
                if (pos < mml.size() && std::isdigit((unsigned char)mml[pos]))
                    elen = readInt(mml, pos, 0);
                else
                    elen = st.defLen;  // Z80互換: デフォルト音長(lコマンド値)
                if (elen <= 0) elen = 4;
                uint32_t et = applyDots(wt / elen, mml, pos);
                ticks += et;
            }
        }

        // `&` タイ（従来形式）— &はZ80 TIEコマンド（KEY_OFFなし）
        // ただし各&セグメント内で独立にZ80自動分割が行われるため、セグメント開始位置を記録
        while (pos < mml.size() && mml[pos] == '&') {
            pos++;
            // 空白スキップ（行末の & の後にスペースがある場合）
            while (pos < mml.size() && (mml[pos] == ' ' || mml[pos] == '\t'))
                pos++;
            // 行末またはデータ末尾でタイ先がない → 次行に継続
            if (pos >= mml.size()) {
                tieOut = true;
                break;
            }
            char nc = std::tolower((unsigned char)mml[pos]);
            if (!(nc >= 'a' && nc <= 'g')) {
                // タイ先が音名でない場合も行またぎとみなす
                tieOut = true;
                break;
            }
            // タイ先の音名をスキップ（同じ音として扱う）
            pos++;
            // シャープ・フラット
            if (pos < mml.size() && (mml[pos]=='+' || mml[pos]=='#' || mml[pos]=='-'))
                pos++;

            // &セグメント開始位置を記録（Z80自動分割はセグメント内で独立に行われる）
            if (ampSegStarts)
                ampSegStarts->push_back(ticks);
            int tlen = 0;
            if (pos < mml.size() && mml[pos] == '%') {
                // &note%N: クロック直接指定（付点なし）
                pos++;
                ticks += readInt(mml, pos, 0);
                tlen = 0;  // ^のデフォルト長には使わない
            } else {
                if (pos < mml.size() && std::isdigit((unsigned char)mml[pos]))
                    tlen = readInt(mml, pos, st.defLen);
                else
                    tlen = st.defLen;
                uint32_t tt = applyDots(wt / tlen, mml, pos);
                ticks += tt;
            }
            // ^ もここで処理（^はZ80 tie bitでKEY_OFF境界になる）
            while (pos < mml.size() && mml[pos] == '^') {
                pos++;
                if (tieBoundaries)
                    tieBoundaries->push_back(ticks);  // ^境界位置（加算前）
                if (pos < mml.size() && mml[pos] == '%') {
                    // ^%N: クロック直接指定
                    pos++;
                    ticks += readInt(mml, pos, 0);
                } else {
                    int elen2 = 0;
                    if (pos < mml.size() && std::isdigit((unsigned char)mml[pos]))
                        elen2 = readInt(mml, pos, 0);
                    else
                        elen2 = (tlen > 0) ? tlen : st.defLen;
                    if (elen2 <= 0) elen2 = 4;
                    uint32_t et2 = applyDots(wt / elen2, mml, pos);
                    ticks += et2;
                }
            }
        }

        return ticks;
    }

    // ==========================================================================
    // ユーティリティ
    // ==========================================================================
    // 整数読み込み（負号・16進数 $ プレフィックス対応）
    static int readInt(const std::string& s, size_t& pos, int defVal)
    {
        if (pos >= s.size()) return defVal;

        // 負号処理
        bool negative = false;
        if (s[pos] == '-') {
            negative = true;
            pos++;
            if (pos >= s.size() || (!std::isdigit((unsigned char)s[pos]) && s[pos] != '$')) {
                pos--;  // 負号だけで数字がない場合は戻す
                return defVal;
            }
        }

        // 16進数: $XXXX
        if (s[pos] == '$') {
            pos++;
            if (pos >= s.size() || !std::isxdigit((unsigned char)s[pos])) {
                if (negative) pos--;  // $の前の-も戻す
                return defVal;
            }
            int val = 0;
            while (pos < s.size() && std::isxdigit((unsigned char)s[pos])) {
                char c = std::tolower((unsigned char)s[pos]);
                val = val * 16 + (c >= 'a' ? c - 'a' + 10 : c - '0');
                pos++;
            }
            return negative ? -val : val;
        }
        if (!std::isdigit((unsigned char)s[pos])) {
            if (negative) pos--;  // 負号だけで数字がない場合は戻す
            return defVal;
        }
        int val = 0;
        while (pos < s.size() && std::isdigit((unsigned char)s[pos])) {
            val = val * 10 + (s[pos] - '0');
            pos++;
        }
        return negative ? -val : val;
    }

    // セパレーター（数字以外の区切り）をスキップ
    static void skipSep(const std::string& s, size_t& pos)
    {
        while (pos < s.size() && !std::isdigit((unsigned char)s[pos])
               && s[pos] != '$' && s[pos] != '\n' && s[pos] != ';')
            pos++;
    }

    // 空白・コメントをスキップ
    static void skipSpacesAndComments(const std::string& s, size_t& pos)
    {
        while (pos < s.size()) {
            if (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n') {
                pos++; continue;
            }
            if (s[pos] == ';') {  // コメント
                while (pos < s.size() && s[pos] != '\n') pos++;
                continue;
            }
            break;
        }
    }

};

// ==========================================================================
// 後方互換: チャンネル単体でのパース（loadMml互換インターフェース）
// ==========================================================================
inline std::vector<MmlEvent> parseSingleChannelMml(
    const std::string& mml, int channel = 0)
{
    // トラック文字なし・1チャンネル分のMMLをパース
    // 既存の loadMml(str, ch) 互換
    MmlParser parser;
    std::string wrapped = "A " + mml;  // Aトラックとして包んでパース
    auto result = parser.parse(wrapped);
    // チャンネル0のイベントを返す（トラックAはch=0）
    auto events = result.channelEvents[0];
    // チャンネル番号を上書き
    for (auto& ev : events) ev.channel = channel;
    return events;
}
