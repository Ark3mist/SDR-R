// RDS (IEC 62106) encoder and decoder.
//
// Decoder chain, fed with the FM multiplex at the IF rate (DSP thread):
//   x e^{-j 2pi 57k t} -> 2-stage decimating low-pass (+-2.4 kHz) to ~25 kHz
//   -> Costas loop (BPSK carrier phase) -> half-sine chip matched filter
//   -> Gardner timing recovery at the 2375 Hz chip rate -> chip pairing
//   (biphase) -> differential decode -> block sync on CRC offset words
//   -> group decode (0A/0B PS + PTY, 2A/2B RadioText, PI).
// All buffers are sized in configure(); process() never allocates.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "core/types.h"
#include "dsp/fir.h"

namespace osx {

namespace rds {
constexpr double kCarrierHz = 57000.0, kBitRate = 1187.5, kChipRate = 2375.0;
enum Offset { A = 0, B, C, Cp, D, None };
uint16_t checkword(uint16_t info);         // 10-bit CRC, g(x) = x^10+x^8+x^7+x^5+x^4+x^3+1
uint16_t offsetWord(Offset o);
uint32_t makeBlock(uint16_t info, Offset o);
const char* ptyName(int pty);              // RDS (European) programme type table
}  // namespace rds

struct RdsInfo {
    bool synced = false;
    uint16_t pi = 0;
    bool piValid = false;
    int pty = -1;
    std::string ps, rt;                    // empty until a complete copy has been received
    uint32_t groups = 0;                   // good groups decoded
    float blockErrorRate = 1.0f;
};

// Generates the differentially encoded RDS bit stream for a station.
class RdsEncoder {
public:
    RdsEncoder(uint16_t pi, int pty, const std::string& ps, const std::string& rt);
    int nextBit();                         // 0/1 after differential encoding

private:
    void loadGroup();
    uint16_t pi_;
    int pty_;
    std::array<char, 8> ps_{};
    std::array<char, 64> rt_{};
    int rtSegments_ = 16;
    std::array<uint32_t, 4> blocks_{};
    int bitInGroup_ = 104, groupIdx_ = 0, prev_ = 0;
};

class RdsDecoder {
public:
    void configure(double inputRate);      // allocates; call on rebuild only
    void reset();
    void process(const float* mpx, size_t n);
    RdsInfo info() const;                  // any thread

private:
    void onBit(int bit);
    void onBlock(uint32_t block, int idx);
    void decodeGroup();
    void publish();

    // front end
    double fs_ = 0.0, rate_ = 0.0;
    uint32_t ncoPhase_ = 0, ncoInc_ = 0;
    size_t d1_ = 1, d2_ = 1, ph1_ = 0, ph2_ = 0;
    FirCR lp1_, lp2_;
    FirRR chipMf_;
    // Costas
    float theta_ = 0.0f, freq_ = 0.0f, alpha_ = 0.0f, beta_ = 0.0f;
    // Gardner (in low-rate samples)
    double halfChip_ = 5.0, strobe_ = 0.0, t_ = 0.0;
    bool midNext_ = false;
    float prevSample_ = 0.0f, lastChip_ = 0.0f, mid_ = 0.0f, amp_ = 1e-3f;
    // chip pairing
    std::array<float, 2> chipHist_{};
    uint32_t chipCount_ = 0;
    std::array<float, 2> pairScore_{};
    float prevSym_ = 0.0f;
    // block sync
    uint32_t reg_ = 0;
    uint64_t bitCount_ = 0;
    bool synced_ = false;
    int expect_ = 0, bitsToBlock_ = 0;
    int lastIdx_ = -1;
    uint64_t lastBit_ = 0;
    std::array<uint16_t, 4> grp_{};
    std::array<bool, 4> grpOk_{};
    std::array<bool, 64> errHist_{};
    int errPos_ = 0, errCount_ = 0;
    // decoded state
    uint16_t piCand_ = 0;
    int piRepeat_ = 0;
    std::array<char, 8> ps_{};
    uint8_t psMask_ = 0;
    std::array<char, 64> rt_{};
    uint32_t rtMask_ = 0;
    int rtAb_ = -1;
    uint32_t goodGroups_ = 0;

    mutable std::mutex outMutex_;
    RdsInfo out_;
    std::array<char, 9> outPs_{};
    std::array<char, 65> outRt_{};
};

}  // namespace osx
