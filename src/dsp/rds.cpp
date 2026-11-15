#include "dsp/rds.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/exp_table.h"

namespace osx {

namespace rds {

uint16_t checkword(uint16_t info) {
    constexpr uint32_t g = 0x5B9;  // x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1
    uint32_t reg = uint32_t(info) << 10;
    for (int bit = 25; bit >= 10; --bit)
        if (reg & (1u << bit)) reg ^= g << (bit - 10);
    return uint16_t(reg & 0x3FF);
}

uint16_t offsetWord(Offset o) {
    static const uint16_t w[] = {0x0FC, 0x198, 0x168, 0x350, 0x1B4};
    return o < None ? w[o] : 0;
}

uint32_t makeBlock(uint16_t info, Offset o) {
    return (uint32_t(info) << 10) | uint32_t(checkword(info) ^ offsetWord(o));
}

const char* ptyName(int pty) {
    static const char* names[32] = {"None", "News", "Affairs", "Info", "Sport", "Educate", "Drama", "Culture",
                                    "Science", "Varied", "Pop M", "Rock M", "Easy M", "Light M", "Classics", "Other M",
                                    "Weather", "Finance", "Children", "Social", "Religion", "Phone In", "Travel", "Leisure",
                                    "Jazz", "Country", "Nation M", "Oldies", "Folk M", "Document", "TEST", "Alarm"};
    return (pty >= 0 && pty < 32) ? names[pty] : "-";
}

}  // namespace rds

// ---------------------------------------------------------------------------
// Encoder (used by the simulator and the tests)
// ---------------------------------------------------------------------------

RdsEncoder::RdsEncoder(uint16_t pi, int pty, const std::string& ps, const std::string& rt) : pi_(pi), pty_(pty) {
    ps_.fill(' ');
    rt_.fill(' ');
    std::copy_n(ps.begin(), std::min<size_t>(8, ps.size()), ps_.begin());
    const size_t n = std::min<size_t>(63, rt.size());
    std::copy_n(rt.begin(), n, rt_.begin());
    rt_[n] = 0x0D;  // end-of-message marker
    rtSegments_ = int(n / 4) + 1;
}

void RdsEncoder::loadGroup() {
    // Schedule: PS segments 0..3 (group 0A), then four RadioText segments (2A).
    const int slot = groupIdx_ % 8;
    const int round = groupIdx_ / 8;
    ++groupIdx_;
    const uint16_t common = uint16_t((pty_ & 31) << 5);
    if (slot < 4) {
        const int seg = slot;
        const uint16_t b = uint16_t((0u << 12) | common | 0x0008 /* music */ | seg);
        blocks_ = {rds::makeBlock(pi_, rds::A), rds::makeBlock(b, rds::B), rds::makeBlock(0xE0CD, rds::C),
                   rds::makeBlock(uint16_t((uint8_t(ps_[size_t(2 * seg)]) << 8) | uint8_t(ps_[size_t(2 * seg + 1)])), rds::D)};
    } else {
        const int seg = (round * 4 + (slot - 4)) % rtSegments_;
        const uint16_t b = uint16_t((2u << 12) | common | seg);
        auto pair = [&](int i) { return uint16_t((uint8_t(rt_[size_t(i)]) << 8) | uint8_t(rt_[size_t(i + 1)])); };
        blocks_ = {rds::makeBlock(pi_, rds::A), rds::makeBlock(b, rds::B), rds::makeBlock(pair(4 * seg), rds::C),
                   rds::makeBlock(pair(4 * seg + 2), rds::D)};
    }
    bitInGroup_ = 0;
}

int RdsEncoder::nextBit() {
    if (bitInGroup_ >= 104) loadGroup();
    const uint32_t block = blocks_[size_t(bitInGroup_ / 26)];
    const int bit = int((block >> (25 - bitInGroup_ % 26)) & 1u);
    ++bitInGroup_;
    prev_ ^= bit;  // differential encoding
    return prev_;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

void RdsDecoder::configure(double inputRate) {
    fs_ = inputRate;
    d1_ = std::max<size_t>(1, size_t(fs_ / 50e3));
    const double r1 = fs_ / double(d1_);
    d2_ = std::max<size_t>(1, size_t(r1 / 19e3));
    rate_ = r1 / double(d2_);
    // Stage 1 only has to keep its aliases out of +-2.4 kHz.
    lp1_.setTaps(designLowpass(fs_, 0.5 * (2.4e3 + (r1 - 3.8e3)), firTapsFor(fs_, r1 - 3.8e3 - 2.4e3, FirWindow::Hamming, 255),
                               FirWindow::Hamming));
    // Stage 2 rejects the L-R sub-band, whose upper edge lands 4 kHz below 57 kHz.
    lp2_.setTaps(designLowpass(r1, 3.1e3, firTapsFor(r1, 1.4e3, FirWindow::Hamming, 511), FirWindow::Hamming));
    // Half-sine chip matched filter.
    const int nc = std::max(3, int(std::lround(rate_ / rds::kChipRate)));
    std::vector<float> mf(static_cast<size_t>(nc));
    for (int k = 0; k < nc; ++k) mf[size_t(k)] = float(std::sin(std::numbers::pi * (double(k) + 0.5) / double(nc)));
    chipMf_.setTaps(mf);
    ncoInc_ = phaseInc(rds::kCarrierHz, fs_);
    const double wn = 2.0 * std::numbers::pi * 12.0 / rate_;  // ~12 Hz Costas loop
    alpha_ = float(2.0 * 0.7071 * wn);
    beta_ = float(wn * wn);
    halfChip_ = rate_ / rds::kChipRate / 2.0;
    reset();
}

void RdsDecoder::reset() {
    ncoPhase_ = 0;
    ph1_ = ph2_ = 0;
    theta_ = freq_ = 0.0f;
    strobe_ = halfChip_ * 2.0;
    t_ = 0.0;
    midNext_ = false;
    prevSample_ = lastChip_ = mid_ = 0.0f;
    amp_ = 1e-3f;
    chipHist_.fill(0.0f);
    chipCount_ = 0;
    pairScore_.fill(0.0f);
    prevSym_ = 0.0f;
    reg_ = 0;
    bitCount_ = 0;
    synced_ = false;
    lastIdx_ = -1;
    errHist_.fill(false);
    errPos_ = errCount_ = 0;
    piRepeat_ = 0;
    ps_.fill(' ');
    rt_.fill(' ');
    psMask_ = 0;
    rtMask_ = 0;
    rtAb_ = -1;
    goodGroups_ = 0;
    std::lock_guard<std::mutex> lk(outMutex_);
    out_ = RdsInfo{};
    outPs_.fill(0);
    outRt_.fill(0);
}

RdsInfo RdsDecoder::info() const {
    std::lock_guard<std::mutex> lk(outMutex_);
    RdsInfo r = out_;
    r.ps = outPs_.data();
    r.rt = outRt_.data();
    return r;
}

void RdsDecoder::process(const float* mpx, size_t n) {
    if (fs_ <= 0.0) return;
    const auto& tab = expTable();
    for (size_t i = 0; i < n; ++i) {
        const cf32 lo = tab[ncoPhase_ >> 16];
        ncoPhase_ += ncoInc_;
        lp1_.push(cf32(mpx[i] * lo.real(), -mpx[i] * lo.imag()));
        if (++ph1_ < d1_) continue;
        ph1_ = 0;
        lp2_.push(lp1_.compute());
        if (++ph2_ < d2_) continue;
        ph2_ = 0;

        // ---- low rate (~25 kHz): Costas loop ------------------------------
        const cf32 z = lp2_.compute();
        const cf32 rot = tab[uint32_t(int64_t(theta_ * 4294967296.0f)) >> 16];
        const cf32 y = z * std::conj(rot);
        const float pw = std::norm(y) + 1e-20f;
        const float err = y.real() * y.imag() / pw;  // ~ sin(2 phi) / 2
        freq_ = std::clamp(freq_ + beta_ * err, -0.002f, 0.002f);
        theta_ += freq_ + alpha_ * err;
        theta_ -= std::floor(theta_);

        // ---- chip matched filter + Gardner timing --------------------------
        const float s = chipMf_.filter(y.real());
        t_ += 1.0;
        while (strobe_ <= t_) {
            const float frac = float(strobe_ - (t_ - 1.0));
            const float v = prevSample_ + (s - prevSample_) * frac;
            if (midNext_) {
                mid_ = v;
                strobe_ += halfChip_;
            } else {
                const float e = (v - lastChip_) * mid_ / (amp_ * amp_ + 1e-20f);
                lastChip_ = v;
                amp_ += 0.02f * (std::fabs(v) - amp_);
                strobe_ += halfChip_ - std::clamp(double(e), -1.0, 1.0) * 0.08 * halfChip_;

                // ---- chip pairing: correct pairs always differ in sign ----
                chipHist_[0] = chipHist_[1];
                chipHist_[1] = v;
                const int parity = int(chipCount_++ & 1u);
                pairScore_[size_t(parity)] = 0.995f * pairScore_[size_t(parity)] + std::fabs(chipHist_[1] - chipHist_[0]);
                const int best = pairScore_[0] >= pairScore_[1] ? 0 : 1;
                if (parity == best) {
                    const float sym = chipHist_[0] - chipHist_[1];
                    onBit((sym > 0.0f) != (prevSym_ > 0.0f) ? 1 : 0);
                    prevSym_ = sym;
                }
            }
            midNext_ = !midNext_;
        }
        prevSample_ = s;
        if (t_ > 1e6) {  // keep the clock values small
            t_ -= 1e6;
            strobe_ -= 1e6;
        }
    }
}

void RdsDecoder::onBit(int bit) {
    reg_ = ((reg_ << 1) | uint32_t(bit)) & 0x3FFFFFFu;
    ++bitCount_;
    auto offsetOf = [](uint32_t blk) {
        const uint16_t syn = uint16_t((blk & 0x3FF) ^ rds::checkword(uint16_t(blk >> 10)));
        for (int o = rds::A; o < rds::None; ++o)
            if (syn == rds::offsetWord(rds::Offset(o))) return o;
        return int(rds::None);
    };

    if (!synced_) {
        const int o = offsetOf(reg_);
        if (o == rds::None || bitCount_ < 26) return;
        const int idx = (o == rds::Cp) ? 2 : (o > rds::Cp ? o - 1 : o);  // A0 B1 C2 D3
        if (lastIdx_ >= 0) {
            const uint64_t dt = bitCount_ - lastBit_;
            if (dt % 26 == 0 && dt / 26 <= 6 && (lastIdx_ + int(dt / 26)) % 4 == idx) {
                synced_ = true;
                errHist_.fill(false);
                errPos_ = errCount_ = 0;
                grpOk_.fill(false);
                expect_ = idx;
                bitsToBlock_ = 0;
                onBlock(reg_, idx);
                return;
            }
        }
        lastIdx_ = idx;
        lastBit_ = bitCount_;
        return;
    }

    if (++bitsToBlock_ < 26) return;
    bitsToBlock_ = 0;
    const int o = offsetOf(reg_);
    const int want = (expect_ + 1) % 4;
    const bool ok = (o != rds::None) && ((want == 2) ? (o == rds::C || o == rds::Cp) : (o == (want < 2 ? want : want + 1)));
    errCount_ += int(!ok) - int(errHist_[size_t(errPos_)]);
    errHist_[size_t(errPos_)] = !ok;
    errPos_ = (errPos_ + 1) % int(errHist_.size());
    if (errCount_ > 22) {  // > ~1/3 of the last 64 blocks bad: re-acquire
        synced_ = false;
        lastIdx_ = -1;
        std::lock_guard<std::mutex> lk(outMutex_);
        out_.synced = false;
        return;
    }
    expect_ = want;
    if (ok) onBlock(reg_, want);
    else grpOk_[size_t(want)] = false;
    if (want == 3) decodeGroup();
}

void RdsDecoder::onBlock(uint32_t block, int idx) {
    if (idx == 0) grpOk_.fill(false);
    grp_[size_t(idx)] = uint16_t(block >> 10);
    grpOk_[size_t(idx)] = true;
}

void RdsDecoder::decodeGroup() {
    if (grpOk_[0]) {
        if (grp_[0] == piCand_) piRepeat_ = std::min(piRepeat_ + 1, 10);
        else { piCand_ = grp_[0]; piRepeat_ = 0; }
    }
    if (!grpOk_[1]) { publish(); return; }
    const uint16_t b = grp_[1];
    const int type = b >> 12, versionB = (b >> 11) & 1;
    const int pty = (b >> 5) & 31;

    auto putChars = [](std::array<char, 64>& dst, size_t pos, uint16_t w) {
        auto clean = [](uint8_t c) { return (c >= 0x20 && c < 0x7F) ? char(c) : (c == 0x0D ? char(0x0D) : '?'); };
        if (pos < dst.size()) dst[pos] = clean(uint8_t(w >> 8));
        if (pos + 1 < dst.size()) dst[pos + 1] = clean(uint8_t(w & 0xFF));
    };

    if (type == 0 && grpOk_[3]) {
        const int seg = b & 3;
        std::array<char, 64> tmp{};
        putChars(tmp, 0, grp_[3]);
        ps_[size_t(2 * seg)] = tmp[0] == 0x0D ? ' ' : tmp[0];
        ps_[size_t(2 * seg + 1)] = tmp[1] == 0x0D ? ' ' : tmp[1];
        psMask_ |= uint8_t(1u << seg);
        if (psMask_ == 0x0F) {
            std::lock_guard<std::mutex> lk(outMutex_);
            for (size_t i = 0; i < 8; ++i) outPs_[i] = ps_[i];
            outPs_[8] = 0;
            psMask_ = 0;
        }
    } else if (type == 2) {
        const int seg = b & 15, ab = (b >> 4) & 1;
        if (rtAb_ != ab) {  // text A/B flag toggled: a new message starts
            rt_.fill(' ');
            rtMask_ = 0;
            rtAb_ = ab;
        }
        bool got = false;
        if (!versionB && grpOk_[2] && grpOk_[3]) {
            putChars(rt_, size_t(4 * seg), grp_[2]);
            putChars(rt_, size_t(4 * seg + 2), grp_[3]);
            got = true;
        } else if (versionB && grpOk_[3]) {
            putChars(rt_, size_t(2 * seg), grp_[3]);
            got = true;
        }
        if (got) {
            rtMask_ |= 1u << seg;
            // Complete when every segment up to the one holding 0x0D (or all 16) has arrived.
            const size_t perSeg = versionB ? 2 : 4;
            size_t end = rt_.size();
            for (size_t i = 0; i < rt_.size(); ++i)
                if (rt_[i] == 0x0D) { end = i; break; }
            const int lastSeg = int((end == 0 ? 0 : end - 1) / perSeg);
            const uint32_t need = (lastSeg >= 15) ? 0xFFFFu : ((1u << (lastSeg + 1)) - 1u);
            if ((rtMask_ & need) == need && (end < rt_.size() || lastSeg >= 15)) {
                std::lock_guard<std::mutex> lk(outMutex_);
                size_t len = std::min(end, size_t(64));
                while (len > 0 && rt_[len - 1] == ' ') --len;
                for (size_t i = 0; i < len; ++i) outRt_[i] = rt_[i];
                outRt_[len] = 0;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(outMutex_);
        out_.pty = pty;
    }
    ++goodGroups_;
    publish();
}

void RdsDecoder::publish() {
    std::lock_guard<std::mutex> lk(outMutex_);
    out_.synced = synced_;
    if (piRepeat_ >= 1) {
        out_.pi = piCand_;
        out_.piValid = true;
    }
    out_.groups = goodGroups_;
    out_.blockErrorRate = float(errCount_) / float(errHist_.size());
}

}  // namespace osx
