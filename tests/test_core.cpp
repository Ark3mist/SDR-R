// Headless verification of the non-GUI pipeline:
//   ring buffer integrity, u8 conversion, DC blocker, simulated source ->
//   FFT level/frequency accuracy, and the threaded source->DSP->FFT chain.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <new>

#include "audio/limiter.h"
#include "dsp/auto_notch.h"
#include "dsp/conditioning.h"
#include "dsp/noise_blanker.h"
#include "dsp/rds.h"
#include "dsp/spectral_nr.h"
#include "dsp/dsp_engine.h"
#include "engine/bookmarks.h"
#include "engine/radio.h"
#include "source/file_source.h"
#include <filesystem>
#include <fstream>
#include "source/rtltcp_source.h"
#include "dsp/receiver.h"
#include "dsp/spectrum_analyzer.h"
#include "source/sim_source.h"

using namespace osx;

// Global allocation counter: proves the real-time paths never touch the heap.
// (new/delete are both malloc/free here, so the pairing is consistent.)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
static std::atomic<uint64_t> g_allocs{0};
void* operator new(size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static int failures = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            ++failures;                                   \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
        }                                                 \
    } while (0)

static void testRingSpsc() {
    RingBuffer<uint32_t> rb(1000);  // rounds to 1024
    CHECK(rb.capacity() == 1024, "capacity %zu", rb.capacity());
    constexpr uint32_t kTotal = 2'000'000;
    std::thread prod([&] {
        uint32_t next = 0, buf[97];
        while (next < kTotal) {
            const uint32_t n = std::min<uint32_t>(97, kTotal - next);
            for (uint32_t i = 0; i < n; ++i) buf[i] = next + i;
            size_t w = 0;
            while (w < n) {
                const size_t k = rb.write(buf + w, n - w);
                if (k == 0) std::this_thread::yield();
                w += k;
            }
            next += n;
        }
    });
    uint32_t expect = 0, buf[211];
    bool ok = true;
    while (expect < kTotal) {
        rb.waitReadable(1);
        const size_t n = rb.read(buf, 211);
        for (size_t i = 0; i < n; ++i) ok &= (buf[i] == expect++);
    }
    prod.join();
    CHECK(ok, "SPSC ring delivered corrupted / reordered data");
    CHECK(rb.readable() == 0, "ring not empty");
}

static void testConvertAndDc() {
    const uint8_t raw[4] = {0, 255, 128, 127};
    cf32 out[2];
    convertU8ToCf32(raw, out, 2, false);
    CHECK(std::fabs(out[0].real() + 1.0f) < 1e-6f && std::fabs(out[0].imag() - 1.0f) < 1e-6f, "u8 range");
    convertU8ToCf32(raw, out, 2, true);
    CHECK(out[0].real() > 0.99f && out[0].imag() < -0.99f, "IQ swap");

    DcBlocker dc;
    std::vector<cf32> x(100000, cf32{0.2f, -0.1f});
    dc.process(x.data(), x.size());
    CHECK(std::abs(x.back()) < 1e-3f, "DC residual %g", double(std::abs(x.back())));
}

// AM emitter sits at +600 kHz in the band plan: carrier = level + 20log10(0.55).
static void testSimSpectrum() {
    constexpr size_t N = 8192;
    SimSource src;
    src.setSampleRate(2.4e6);
    src.setCenterFrequency(100.0e6);
    src.noiseDb = -60.0f;
    src.dcOffset = 0.0f;
    std::vector<uint8_t> raw(N * 2);
    src.generate(raw.data(), N);  // settle
    src.generate(raw.data(), N);
    std::vector<cf32> iq(N);
    convertU8ToCf32(raw.data(), iq.data(), N, false);

    SpectrumFft fft(N, FftWindow::BlackmanHarris);
    std::vector<float> db(N);
    fft.compute(iq.data(), 0, db.data());

    const size_t expectBin = N / 2 + size_t(600e3 / 2.4e6 * N);
    size_t peak = expectBin - 20;
    for (size_t i = expectBin - 20; i <= expectBin + 20; ++i)
        if (db[i] > db[peak]) peak = i;
    const float expectDb = -28.0f + 20.0f * std::log10(0.55f);
    CHECK(peak == expectBin, "AM carrier bin %zu, expected %zu", peak, expectBin);
    CHECK(std::fabs(db[peak] - expectDb) < 1.0f, "AM carrier %.2f dBFS, expected %.2f", double(db[peak]), double(expectDb));

    // Inverting IQ must mirror the spectrum about DC.
    convertU8ToCf32(raw.data(), iq.data(), N, true);
    fft.compute(iq.data(), 0, db.data());
    const size_t mirror = N - expectBin;
    CHECK(db[mirror] > expectDb - 1.5f, "mirrored carrier %.2f dBFS at bin %zu", double(db[mirror]), mirror);
}

static void testThreadedChain() {
    RingBuffer<uint8_t> hw(1 << 22);
    RingBuffer<cf32> tap(1 << 20);
    SimSource src;
    src.setSampleRate(1.024e6);
    DspEngine dsp(hw, tap);
    SpectrumAnalyzer an(tap);
    an.setFftSize(4096);
    an.setRate(50);
    an.start();
    dsp.start(src.sampleRate());
    src.start(hw);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    src.stop();
    dsp.stop();
    an.stop();

    int lines = 0;
    std::vector<float> line;
    while (an.popLine(line)) ++lines;
    const double rate = double(dsp.samplesProcessed());
    CHECK(rate > 0.90 * 1.024e6 && rate < 1.10 * 1.024e6, "throughput %.0f S in 1 s", rate);
    CHECK(src.overruns() == 0, "overruns %llu", static_cast<unsigned long long>(src.overruns()));
    CHECK(lines > 0 && line.size() == 4096, "analyzer produced %d lines", lines);
    std::printf("  threaded chain: %.0f samples/s, %d pending FFT lines\n", rate, lines);
}

// ---- receiver: demodulate simulator emitters offline and check the audio ----
static double tonePower(const std::vector<float>& x, double hz) {
    const double c = 2.0 * std::cos(2.0 * 3.14159265358979 * hz / 48000.0);
    double s1 = 0, s2 = 0;
    for (float v : x) { const double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (double(x.size()) * double(x.size()) / 4.0);  // amplitude^2
}

struct RxRun { std::vector<float> l, r; float ctcss; bool stereo; float level; double seconds; RdsInfo rds; float nbPct; };

static RxRun runRx(double centerHz, double offsetHz, RxConfig cfg, double seconds = 2.5, double fs = 1.024e6,
                   float impulseRate = 0.0f) {
    RingBuffer<float> audio(size_t(1) << 20);
    Receiver rx(audio);
    rx.setClockTracking(false);
    rx.setInputRate(fs);
    rx.setConfig(cfg);
    rx.setOffset(offsetHz);
    SimSource src;
    src.setSampleRate(fs);
    src.setCenterFrequency(centerHz);
    src.noiseDb = -55.0f;
    src.impulseRate = impulseRate;
    const size_t block = 8192;
    std::vector<uint8_t> raw(block * 2);
    std::vector<cf32> iq(block);
    std::vector<float> tmp(65536);
    RxRun out{};
    size_t skip = size_t(0.8 * 48000) * 2;  // let filters, PLLs and AGC settle
    for (size_t done = 0; done < size_t(seconds * fs); done += block) {
        out.seconds += double(block) / fs;
        src.generate(raw.data(), block);
        convertU8ToCf32(raw.data(), iq.data(), block, false);
        rx.process(iq.data(), block);
        size_t n;
        while ((n = audio.read(tmp.data(), tmp.size())) > 0)
            for (size_t i = 0; i + 1 < n; i += 2) {
                if (skip) { skip -= 2; continue; }
                out.l.push_back(tmp[i]);
                out.r.push_back(tmp[i + 1]);
            }
    }
    out.ctcss = rx.ctcssHz.load();
    out.stereo = rx.stereoLocked.load();
    out.level = rx.levelDb.load();
    out.rds = rx.rdsInfo();
    out.nbPct = rx.nbBlankedPct.load();
    return out;
}

static double dB(double p) { return 10.0 * std::log10(p + 1e-30); }

static void testReceiver() {
    // Band plan (repeats every 2 MHz): WFM +0, NFM +400k, AM +600k, CW +750k.
    {   // WFM stereo: L carries 1 kHz, R carries 3 kHz.
        RxConfig c; c.mode = Demod::WFM; c.bandwidthHz = 200e3; c.stereo = true; c.deemphasis = 0;
        const RxRun a = runRx(100.2e6, -200e3, c);
        const double l1 = dB(tonePower(a.l, 1000)), l3 = dB(tonePower(a.l, 3000));
        const double r1 = dB(tonePower(a.r, 1000)), r3 = dB(tonePower(a.r, 3000));
        std::printf("  WFM stereo: lock=%d  L: 1k %.1f / 3k %.1f dB   R: 1k %.1f / 3k %.1f dB  19k leak %.1f dB\n",
                    int(a.stereo), l1, l3, r1, r3, dB(tonePower(a.l, 19000)));
        CHECK(a.stereo, "pilot PLL did not lock");
        CHECK(l1 > -12 && l1 - l3 > 20, "L channel separation %.1f dB", l1 - l3);
        CHECK(r3 > -12 && r3 - r1 > 20, "R channel separation %.1f dB", r3 - r1);
        CHECK(dB(tonePower(a.l, 19000)) < l1 - 40, "pilot leaks into audio");
        c.stereo = false;
        const RxRun mono = runRx(100.2e6, -200e3, c, 1.5);
        CHECK(!mono.stereo && std::fabs(dB(tonePower(mono.l, 1000)) - dB(tonePower(mono.l, 3000))) < 3, "mono sum");
    }
    {   // NFM: 1.2 kHz tone, 100.0 Hz CTCSS detected and filtered from the audio.
        RxConfig c; c.mode = Demod::NFM; c.bandwidthHz = 12.5e3; c.deemphasis = 0;
        const RxRun a = runRx(100.5e6, -100e3, c, 3.0);
        const double t = dB(tonePower(a.l, 1200)), sub = dB(tonePower(a.l, 100));
        std::printf("  NFM: 1.2k %.1f dB, 100 Hz residual %.1f dB, CTCSS %.1f Hz, level %.1f dBFS\n", t, sub, double(a.ctcss), double(a.level));
        CHECK(t > -15, "NFM tone %.1f dB", t);
        CHECK(std::fabs(a.ctcss - 100.0f) < 0.01f, "CTCSS decoded %.1f Hz", double(a.ctcss));
        CHECK(sub < t - 15, "sub-tone not attenuated");
        CHECK(std::fabs(a.level + 30.0f) < 1.5f, "channel level %.1f dBFS (emitter is -30)", double(a.level));
        // Squelch closed above the signal level -> silence.
        c.squelch = true; c.squelchDb = -10.0f;
        const RxRun q = runRx(100.5e6, -100e3, c, 1.5);
        CHECK(dB(tonePower(q.l, 1200)) < -80, "squelch leak %.1f dB", dB(tonePower(q.l, 1200)));
    }
    for (Demod m : {Demod::AM, Demod::SAM}) {  // 700 Hz, 80 % modulation
        RxConfig c; c.mode = m; c.bandwidthHz = 10e3;
        const RxRun a = runRx(100.5e6, 100e3, c);
        const double t = dB(tonePower(a.l, 700)), o = dB(tonePower(a.l, 1900));
        std::printf("  %s: 700 Hz %.1f dB, off-tone %.1f dB\n", demodName(m), t, o);
        CHECK(t > -25 && t - o > 30, "%s tone %.1f dB vs %.1f dB", demodName(m), t, o);
    }
    {   // SSB: tune 1 kHz below / above the AM carrier -> 1 kHz beat note.
        RxConfig c; c.mode = Demod::USB; c.bandwidthHz = 2.8e3;
        const RxRun u = runRx(100.5e6, 100e3 - 1000, c);
        c.mode = Demod::LSB;
        const RxRun l = runRx(100.5e6, 100e3 + 1000, c);
        const RxRun wrong = runRx(100.5e6, 100e3 - 1000, c);  // LSB on the wrong side: carrier rejected
        const double pu = dB(tonePower(u.l, 1000)), pl = dB(tonePower(l.l, 1000)), pw = dB(tonePower(wrong.l, 1000));
        std::printf("  USB 1k %.1f dB, LSB 1k %.1f dB, opposite sideband %.1f dB\n", pu, pl, pw);
        CHECK(pu > -25 && pl > -25, "SSB beat note");
        CHECK(pu - pw > 25, "opposite sideband rejection %.1f dB", pu - pw);
    }
    {   // CW beacon at +750k (centre 100.5 -> +250k), 700 Hz pitch.
        RxConfig c; c.mode = Demod::CW; c.bandwidthHz = 500; c.cwPitchHz = 700; c.agc = 0;
        const RxRun a = runRx(100.5e6, 250e3, c, 4.0);
        const double t = dB(tonePower(a.l, 700)), o = dB(tonePower(a.l, 1500));
        std::printf("  CW: 700 Hz %.1f dB, 1.5 kHz %.1f dB\n", t, o);
        CHECK(t - o > 25, "CW pitch tone");
    }
    {   // RAW: complex baseband on L/R; AM carrier 5 kHz above the VFO.
        RxConfig c; c.mode = Demod::RAW; c.bandwidthHz = 48e3;
        const RxRun a = runRx(100.5e6, 100e3 - 5000, c, 1.5);
        const double i = dB(tonePower(a.l, 5000)), q = dB(tonePower(a.r, 5000));
        std::printf("  RAW: I %.1f dB, Q %.1f dB at 5 kHz\n", i, q);
        CHECK(std::fabs(i - q) < 1.0 && i > -45, "RAW IQ");
    }
    {   // Awkward input rates still resample to 48 kHz.
        RxConfig c; c.mode = Demod::NFM; c.bandwidthHz = 12.5e3; c.deemphasis = 0;
        for (double fs : {250e3, 2.4e6, 3.2e6}) {
            const RxRun a = runRx(100.4e6 + 50e3, -50e3, c, 1.5, fs);
            const double t = dB(tonePower(a.l, 1200));
            const double rate = double(a.l.size()) / (a.seconds - 0.8);
            CHECK(t > -15 && std::fabs(rate - 48000.0) < 300.0, "fs=%.0f: tone %.1f dB, audio rate %.0f", fs, t, rate);
        }
    }
}

// ---- output limiter: nothing may leave above -6 dBFS -------------------------
static void testLimiter() {
    OutputLimiter lim;
    std::vector<float> x(48000 * 2);
    for (size_t i = 0; i < 48000; ++i) x[2 * i] = x[2 * i + 1] = 2.0f * std::sin(float(i) * 0.13f);  // +6 dBFS input
    float peak = 0.0f;
    for (size_t off = 0; off < 48000; off += 480) {
        lim.process(&x[off * 2], 480, 1.0f, false);
        peak = std::max(peak, lim.peak);
    }
    std::printf("  limiter: +6 dBFS in at full volume -> %.2f dBFS out\n", 20.0 * std::log10(double(peak)));
    CHECK(peak <= OutputLimiter::kCeiling + 1e-6f, "limiter peak %.4f", double(peak));
    CHECK(peak > 0.45f, "limiter over-attenuates (%.3f)", double(peak));
    std::vector<float> q(960, 0.1f);
    lim = OutputLimiter();
    lim.process(q.data(), 480, 1.0f, false);
    CHECK(std::fabs(q[100] - 0.1f) < 1e-6f, "limiter altered a quiet signal");
    lim.process(q.data(), 480, 1.0f, true);
    CHECK(q[100] == 0.0f, "mute");
}

// ---- rtl_tcp client against an in-process mock server -------------------------
static void testRtlTcp() {
    const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    CHECK(::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && ::listen(ls, 1) == 0, "mock listen");
    ::getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &len);

    std::vector<std::pair<uint8_t, uint32_t>> cmds;
    std::atomic<bool> done{false};
    std::thread server([&] {
        const int c = ::accept(ls, nullptr, nullptr);
        const uint8_t hello[12] = {'R', 'T', 'L', '0', 0, 0, 0, 5, 0, 0, 0, 29};
        (void)!::write(c, hello, sizeof(hello));
        timeval tv{0, 2000};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        std::vector<uint8_t> iq(4099);  // odd size on purpose: client must keep I/Q pairing
        for (size_t i = 0; i < iq.size(); ++i) iq[i] = uint8_t(i);
        uint8_t pkt[5];
        size_t have = 0;
        while (!done.load()) {
            if (::send(c, iq.data(), iq.size(), MSG_NOSIGNAL) < 0) break;
            const ssize_t r = ::recv(c, pkt + have, 5 - have, 0);
            if (r > 0 && (have += size_t(r)) == 5) {
                cmds.emplace_back(pkt[0], (uint32_t(pkt[1]) << 24) | (uint32_t(pkt[2]) << 16) | (uint32_t(pkt[3]) << 8) | pkt[4]);
                have = 0;
            }
        }
        ::close(c);
    });

    RingBuffer<uint8_t> ring(1 << 22);
    RtlTcpSource src;
    src.host = "127.0.0.1";
    src.port = ntohs(addr.sin_port);
    src.setSampleRate(1.024e6);
    src.setCenterFrequency(121.5e6);
    CHECK(src.start(ring), "rtl_tcp start: %s", src.lastError().c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    src.setGain(20.0f);
    src.setCenterFrequency(118.1e6);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const size_t got = ring.readable();
    src.stop();
    done = true;
    server.join();
    ::close(ls);

    auto has = [&](uint8_t op, uint32_t v) { return std::find(cmds.begin(), cmds.end(), std::make_pair(op, v)) != cmds.end(); };
    std::printf("  rtl_tcp: %zu bytes streamed, %zu commands seen by the server\n", got, cmds.size());
    CHECK(got > 100000 && got % 2 == 0, "stream bytes %zu", got);
    CHECK(has(0x02, 1024000) && has(0x01, 121500000) && has(0x01, 118100000), "freq / rate commands");
    CHECK(has(0x03, 1) && has(0x04, 197), "manual gain snapped to 19.7 dB (tenths)");
    // Byte order must be intact across odd-sized TCP reads.
    std::vector<uint8_t> head(4099 * 2);
    ring.read(head.data(), head.size());
    bool ok = true;
    for (size_t i = 0; i < head.size(); ++i) ok &= (head[i] == uint8_t((i % 4099)));
    CHECK(ok, "rtl_tcp stream corrupted");

    RtlTcpSource bad;
    bad.host = "127.0.0.1";
    bad.port = 1;
    CHECK(!bad.start(ring) && !bad.lastError().empty(), "connect failure must be reported");
}

// ---- full engine: source -> DSP -> receiver -> audio sink, real threads --------
static void testRadioEngine() {
    Radio radio;
    radio.init("null", false);  // ALSA "null" PCM (falls back to a timer-driven sink if absent)
    CHECK(radio.sampleRate() == 1.024e6 && radio.analyzer.fftSize() == 1024 && radio.analyzer.rate() == 30 &&
              std::fabs(radio.audio.volume.load() - 0.75f) < 1e-6f, "CM0 boot defaults");
    radio.tune(100.4e6);
    radio.setMode(Demod::NFM);
    CHECK(radio.play(), "play: %s", radio.lastError().c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const float level = radio.receiver.levelDb.load(), ctcss = radio.receiver.ctcssHz.load();
    radio.tune(1.0905e9);  // outside the current span: hardware centre must follow
    CHECK(std::fabs(radio.vfoHz() - 1.0905e9) < 0.5 && std::fabs(radio.vfo.offsetHz) <= 0.512e6, "retune");
    radio.stop();
    radio.selectSource(Radio::SourceKind::RtlSdr);  // no dongle here: must fall back to the simulator
    const bool rtlStarted = radio.playWithFallback();
    CHECK(rtlStarted && radio.sourceKind() == Radio::SourceKind::Simulated && radio.fallbackActive(),
          "RTL-SDR fallback to simulator");
    radio.stop();
    std::printf("  engine: NFM level %.1f dBFS, CTCSS %.1f Hz, audio sink '%s', RTL-SDR %s\n", double(level), double(ctcss),
                radio.audio.status().c_str(), radio.lastError().c_str());
    CHECK(std::fabs(level + 30.0f) < 2.0f && std::fabs(ctcss - 100.0f) < 0.01f, "live NFM chain");
    CHECK(radio.sim.overruns() == 0 && radio.receiver.audioDrops.load() == 0, "overruns / audio drops");
    radio.shutdown();
}

// ---- RDS: encoder -> FM multiplex (simulator) -> decoder -----------------------
static void testRds() {
    // CRC / offset words against values from IEC 62106 Annex B.
    CHECK(rds::checkword(0x0000) == 0x000 && rds::offsetWord(rds::A) == 0x0FC, "RDS CRC basics");
    for (double fs : {1.024e6, 2.048e6, 250e3, 1.536e6}) {
        RxConfig c; c.mode = Demod::WFM; c.bandwidthHz = 200e3; c.stereo = true;
        const bool narrow = fs < 400e3;  // 250 kSPS only fits the station centred
        const RxRun a = runRx(narrow ? 100.0e6 : 100.2e6, narrow ? 0.0 : -200e3, c, 6.0, fs);
        std::printf("  RDS @%.3f MSPS: sync=%d PI=%04X PTY=%d(%s) PS='%s' RT='%s' groups=%u BLER=%.2f\n", fs / 1e6,
                    int(a.rds.synced), a.rds.pi, a.rds.pty, rds::ptyName(a.rds.pty), a.rds.ps.c_str(), a.rds.rt.c_str(),
                    a.rds.groups, double(a.rds.blockErrorRate));
        CHECK(a.rds.synced && a.rds.piValid && a.rds.pi == SimSource::kSimRdsPi, "RDS PI @%.0f", fs);
        CHECK(a.rds.ps == SimSource::simRdsPs(), "RDS PS '%s' @%.0f", a.rds.ps.c_str(), fs);
        CHECK(a.rds.rt == SimSource::simRdsRt(), "RDS RT '%s' @%.0f", a.rds.rt.c_str(), fs);
        CHECK(a.rds.pty == SimSource::kSimRdsPty, "RDS PTY %d", a.rds.pty);
    }
    // Station without RDS (WFM B at +1.1 MHz): decoder must stay unsynced, no garbage.
    RxConfig c; c.mode = Demod::WFM;
    const RxRun b = runRx(101.0e6, 100e3, c, 4.0);
    CHECK(!b.rds.synced && b.rds.ps.empty() && b.rds.rt.empty(), "RDS false sync on a non-RDS station");
}

// ---- I/Q imbalance correction -----------------------------------------------
static void testIqBalance() {
    constexpr size_t N = 8192;
    auto imageRejection = [](bool correct) {
        SimSource src;
        src.setSampleRate(1.024e6);
        src.setCenterFrequency(100.55e6);  // AM carrier at +50 kHz, image at -50 kHz (empty)
        src.iqGainErrDb = 1.0f;
        src.iqPhaseErrDeg = 4.0f;
        src.dcOffset = 0.0f;
        std::vector<uint8_t> raw(N * 2);
        std::vector<cf32> iq(N);
        IqBalance bal;
        DcBlocker dc;
        std::vector<float> db(N);
        SpectrumFft fft(N, FftWindow::BlackmanHarris);
        for (int k = 0; k < 60; ++k) {  // ~0.5 s for the estimator to settle
            src.generate(raw.data(), N);
            convertU8ToCf32(raw.data(), iq.data(), N, false);
            dc.process(iq.data(), N);
            if (correct) bal.process(iq.data(), N, 1.024e6);
        }
        fft.compute(iq.data(), 0, db.data());
        const size_t sig = N / 2 + size_t(50e3 / 1.024e6 * N), img = N - sig;
        float s = -200, i = -200;
        for (size_t k = sig - 3; k <= sig + 3; ++k) s = std::max(s, db[k]);
        for (size_t k = img - 3; k <= img + 3; ++k) i = std::max(i, db[k]);
        return std::make_pair(double(s - i), bal);
    };
    const auto [off, b0] = imageRejection(false);
    const auto [on, b1] = imageRejection(true);
    std::printf("  IQ balance: image rejection %.1f dB -> %.1f dB (est. %.2f deg, %.2f dB)\n", off, on,
                double(b1.phaseErrorDeg()), double(b1.gainErrorDb()));
    CHECK(off < 35.0 && on > off + 15.0, "IQ correction %.1f -> %.1f dB", off, on);
}

// ---- noise blanker on AM with ignition-like impulses ---------------------------
static void testNoiseBlanker() {
    auto noiseFloor = [](const RxRun& a) {  // audio energy away from the 700 Hz tone
        double p = 0;
        for (double f : {1300.0, 1900.0, 2600.0, 3300.0, 4100.0}) p += tonePower(a.l, f);
        return dB(p / 5.0);
    };
    RxConfig c; c.mode = Demod::AM; c.bandwidthHz = 10e3; c.agc = 3; c.afGainDb = 0.0f;
    const RxRun clean = runRx(100.5e6, 100e3, c, 2.5, 1.024e6, 0.0f);
    const RxRun off = runRx(100.5e6, 100e3, c, 2.5, 1.024e6, 300.0f);
    c.nb = true; c.nbThreshold = 3.0f;
    const RxRun on = runRx(100.5e6, 100e3, c, 2.5, 1.024e6, 300.0f);
    const double tOff = dB(tonePower(off.l, 700)), tOn = dB(tonePower(on.l, 700));
    std::printf("  NB: clean floor %.1f dB | ", noiseFloor(clean));
    std::printf("impulse noise floor %.1f -> %.1f dB, tone %.1f -> %.1f dB, blanked %.2f %%\n", noiseFloor(off),
                noiseFloor(on), tOff, tOn, double(on.nbPct));
    CHECK(noiseFloor(on) < noiseFloor(off) - 10.0, "noise blanker improvement");
    CHECK(std::fabs(tOn - tOff) < 1.5 && on.nbPct < 0.5f, "noise blanker damages the signal");
}

// ---- LMS auto-notch: removes a steady tone, keeps noise-like signals ----------
static void testAutoNotch() {
    AutoNotch anf;
    std::vector<float> x(48000 * 3), noise(x.size());
    uint64_t rng = 12345;
    for (size_t i = 0; i < x.size(); ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        noise[i] = 0.05f * (float(rng >> 40) / 8388608.0f - 1.0f);
        x[i] = noise[i] + 0.3f * std::sin(2.0f * 3.14159265f * 1000.0f * float(i) / 48000.0f);
    }
    const std::vector<float> in(x.end() - 48000, x.end());
    anf.process(x.data(), x.size());
    const std::vector<float> out(x.end() - 48000, x.end());
    const std::vector<float> nIn(noise.end() - 48000, noise.end());
    double pn = 0, po = 0;
    for (size_t i = 0; i < nIn.size(); ++i) { pn += double(nIn[i]) * nIn[i]; po += double(out[i]) * out[i]; }
    const double tIn = dB(tonePower(in, 1000)), tOut = dB(tonePower(out, 1000));
    std::printf("  ANF: 1 kHz carrier %.1f -> %.1f dB, broadband power ratio %.1f dB\n", tIn, tOut, dB(po / pn));
    CHECK(tOut < tIn - 25.0, "auto-notch tone suppression");
    CHECK(std::fabs(dB(po / pn)) < 3.0, "auto-notch broadband damage");
}

// ---- spectral NR: lowers hiss, keeps the tone, transparent at strength 0 ----------
static void testSpectralNr() {
    std::vector<float> x(48000 * 3);
    uint64_t rng = 999;
    for (size_t i = 0; i < x.size(); ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        // Tone keyed 200 ms on / 200 ms off, like syllables or CW: NR must keep it.
        const bool on = (i / 9600) % 2 == 0;
        x[i] = 0.05f * (float(rng >> 40) / 8388608.0f - 1.0f) + (on ? 0.2f : 0.0f) * std::sin(2.0f * 3.14159265f * 800.0f * float(i) / 48000.0f);
    }
    std::vector<float> y = x;
    SpectralNR nr;
    nr.strength = 0.0f;
    nr.process(y.data(), y.size());
    double maxErr = 0;
    for (size_t i = 256; i < y.size(); ++i) maxErr = std::max(maxErr, double(std::fabs(y[i] - x[i - 256])));
    CHECK(maxErr < 1e-4, "NR at strength 0 is not transparent (err %g)", maxErr);

    y = x;
    nr.reset();
    nr.strength = 1.0f;
    nr.process(y.data(), y.size());
    const std::vector<float> a(x.end() - 48000, x.end()), b(y.end() - 48000, y.end());
    auto hiss = [](const std::vector<float>& v) {
        double p = 0;
        for (double f : {3000.0, 5000.0, 7000.0, 9000.0, 12000.0}) p += tonePower(v, f);
        return dB(p);
    };
    std::printf("  NR: hiss %.1f -> %.1f dB, 800 Hz tone %.1f -> %.1f dB\n", hiss(a), hiss(b), dB(tonePower(a, 800)), dB(tonePower(b, 800)));
    CHECK(hiss(b) < hiss(a) - 10.0, "NR hiss reduction");
    CHECK(std::fabs(dB(tonePower(b, 800)) - dB(tonePower(a, 800))) < 3.0, "NR damages the tone");
}

// ---- zero heap allocations in the steady-state receive chain -------------------
static void testNoHotLoopAllocations() {
    RingBuffer<float> audio(size_t(1) << 20);
    Receiver rx(audio);
    rx.setClockTracking(true);
    rx.setInputRate(1.024e6);
    SimSource src;
    src.setSampleRate(1.024e6);
    src.setCenterFrequency(100.2e6);
    std::vector<uint8_t> raw(32768 * 2);
    std::vector<cf32> iq(32768);
    std::vector<float> sink(1 << 16);
    DcBlocker dc;
    IqBalance bal;
    for (int m = 0; m < int(Demod::Count); ++m) {
        RxConfig c; c.mode = Demod(m); c.bandwidthHz = demodLimits(c.mode).defaultBw;
        c.nb = c.anf = c.nr = true; c.squelch = true;
        rx.setConfig(c);
        rx.setOffset(m == 0 ? -200e3 : 100e3);
        uint64_t during = 0;
        for (int k = 0; k < 12; ++k) {
            src.generate(raw.data(), iq.size());  // generator itself is outside the measured region
            const uint64_t before = g_allocs.load();
            convertU8ToCf32(raw.data(), iq.data(), iq.size(), false);
            dc.process(iq.data(), iq.size());
            bal.process(iq.data(), iq.size(), 1.024e6);
            rx.process(iq.data(), iq.size());
            if (k == 6) {  // a live change (squelch level) must not allocate either
                c.squelchDb = -40.0f;
                rx.setConfig(c);
            }
            if (k >= 2) during += g_allocs.load() - before;  // k = 0 rebuilds (allowed), k = 1 warms up
            while (audio.read(sink.data(), sink.size()) > 0) {}
        }
        CHECK(during == 0, "%s: %llu heap allocations in the steady-state DSP loop", demodName(Demod(m)),
              static_cast<unsigned long long>(during));
    }
    std::printf("  hot loop: 0 heap allocations across all %d modes with NB+ANF+NR (checked)\n", int(Demod::Count));
}

// ---- IQ file player ----------------------------------------------------------
static std::string writeIqWav(const std::string& dir, int bits, double centerHz, double seconds, double fs = 1.024e6) {
    const std::string path = dir + "/baseband_" + std::to_string(int64_t(centerHz)) + "Hz_" + std::to_string(bits) + ".wav";
    SimSource src;
    src.setSampleRate(fs);
    src.setCenterFrequency(centerHz);
    const size_t frames = size_t(seconds * fs);
    std::vector<uint8_t> raw(frames * 2);
    src.generate(raw.data(), frames);
    std::vector<uint8_t> data;
    for (size_t i = 0; i < frames * 2; ++i) {
        const float v = (float(raw[i]) - 127.5f) / 127.5f;
        if (bits == 8) data.push_back(raw[i]);
        else if (bits == 16) { const int16_t q = int16_t(std::lrintf(v * 32767.0f)); data.push_back(uint8_t(q)); data.push_back(uint8_t(q >> 8)); }
        else { uint8_t b[4]; std::memcpy(b, &v, 4); data.insert(data.end(), b, b + 4); }
    }
    auto u32 = [](std::vector<uint8_t>& o, uint32_t v) { for (int k = 0; k < 4; ++k) o.push_back(uint8_t(v >> (8 * k))); };
    auto u16 = [](std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); };
    std::vector<uint8_t> f = {'R', 'I', 'F', 'F'};
    u32(f, uint32_t(4 + 8 + 16 + 8 + 4 + 8 + data.size()));
    f.insert(f.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    u32(f, 16);
    u16(f, bits == 32 ? 3 : 1);
    u16(f, 2);
    u32(f, uint32_t(fs));
    u32(f, uint32_t(fs * 2 * bits / 8));
    u16(f, uint16_t(2 * bits / 8));
    u16(f, uint16_t(bits));
    f.insert(f.end(), {'L', 'I', 'S', 'T'});  // an extra chunk the parser must skip
    u32(f, 4);
    f.insert(f.end(), {'I', 'N', 'F', 'O'});
    f.insert(f.end(), {'d', 'a', 't', 'a'});
    u32(f, uint32_t(data.size()));
    f.insert(f.end(), data.begin(), data.end());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
    return path;
}

static void testFileSource() {
    const std::string dir = std::filesystem::temp_directory_path().string() + "/osx_test_iq";
    std::filesystem::create_directories(dir);
    for (int bits : {8, 16, 32}) {
        const std::string path = writeIqWav(dir, bits, 100.5e6, 0.25);
        FileSource fsrc;
        CHECK(fsrc.open(path), "open %d-bit WAV: %s", bits, fsrc.lastError().c_str());
        CHECK(fsrc.info().encoding == bits && fsrc.sampleRate() == 1.024e6 && fsrc.centerFrequency() == 100.5e6 &&
                  fsrc.info().frames == 256000, "%d-bit WAV header", bits);
        // AM carrier (+100 kHz) must be at the right bin and level.
        constexpr size_t N = 8192;
        std::vector<cf32> iq(N);
        CHECK(fsrc.read(iq.data(), N) == N, "read");
        SpectrumFft fft(N, FftWindow::BlackmanHarris);
        std::vector<float> db(N);
        fft.compute(iq.data(), 0, db.data());
        const size_t bin = N / 2 + size_t(100e3 / 1.024e6 * N);
        const float expect = -28.0f + 20.0f * std::log10(0.55f);
        CHECK(std::fabs(db[bin] - expect) < 1.5f, "%d-bit: carrier %.1f dBFS (expected %.1f)", bits, double(db[bin]), double(expect));
        // Loop and seek.
        fsrc.seek(0.999);
        fsrc.loop = false;
        std::vector<cf32> tail(4096);
        const size_t got = fsrc.read(tail.data(), tail.size());
        CHECK(got == 256, "%d-bit: read past end without loop gave %zu", bits, got);
        fsrc.loop = true;
        fsrc.seek(0.999);
        CHECK(fsrc.read(tail.data(), tail.size()) == tail.size() && fsrc.position() < 0.05, "%d-bit: loop wraps", bits);
    }
    {   // Bad inputs are rejected with a reason, never crash.
        std::ofstream(dir + "/junk.wav") << "not a wav file";
        FileSource b;
        CHECK(!b.open(dir + "/junk.wav") && !b.lastError().empty(), "junk WAV rejected");
        CHECK(!b.open(dir + "/missing.wav"), "missing WAV rejected");
        CHECK(centerFromFilename("/x/SDRSharp_20240101_120000Z_145500000Hz_IQ.wav") == 145.5e6, "SDR# file name");
    }
    {   // Full threaded chain from a recording: file -> cf32 ring -> DSP -> NFM + CTCSS.
        const std::string path = writeIqWav(dir, 16, 100.5e6, 4.0);
        Radio radio;
        radio.init("null", false);
        CHECK(radio.openFile(path), "Radio::openFile: %s", radio.lastError().c_str());
        CHECK(radio.centerHz() == 100.5e6 && radio.sampleRate() == 1.024e6, "file centre / rate adopted");
        radio.setMode(Demod::NFM);
        radio.tune(100.4e6);
        const double before = radio.centerHz();
        radio.tune(120.0e6);  // outside the recording: VFO must clamp, centre must not move
        CHECK(radio.centerHz() == before && std::fabs(radio.vfo.offsetHz) < 0.512e6, "tuning clamped inside the recording");
        radio.tune(100.4e6);
        CHECK(radio.play(), "file play: %s", radio.lastError().c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(2600));
        const float level = radio.receiver.levelDb.load(), tone = radio.receiver.ctcssHz.load();
        radio.shutdown();
        std::printf("  file player: 8/16/32-bit WAV ok; live chain from file: NFM %.1f dBFS, CTCSS %.1f Hz\n", double(level), double(tone));
        CHECK(std::fabs(level + 30.0f) < 2.0f && std::fabs(tone - 100.0f) < 0.01f, "file -> receiver chain");
    }
    std::filesystem::remove_all(dir);
}

// ---- bookmarks ------------------------------------------------------------------
static void testBookmarks() {
    const std::string dir = std::filesystem::temp_directory_path().string() + "/osx_test_cfg/cardsdr";
    std::filesystem::remove_all(dir);
    const std::string path = dir + "/bookmarks.json";
    {
        BookmarkStore st(path);
        CHECK(st.load() && std::filesystem::exists(path) && st.items().size() == BookmarkStore::defaults().size(), "seed defaults");
        st.add({"KLIA Approach test", 119450000, "AM", 8330, "Aviation"});
        st.rename(st.items().size() - 1, "Renamed");
        st.remove(0);
    }
    {
        BookmarkStore st(path);
        CHECK(st.load(), "reload");
        const Bookmark& b = st.items().back();
        CHECK(b.name == "Renamed" && b.frequencyHz == 119450000 && b.demodMode == "AM" && b.bandwidthHz == 8330 &&
                  b.category == "Aviation" && st.items().size() == BookmarkStore::defaults().size(), "bookmark round trip");
    }
    std::ofstream(path) << "{ this is not json";
    {
        BookmarkStore st(path);
        CHECK(!st.load() && std::filesystem::exists(path + ".bad") && st.items().size() == BookmarkStore::defaults().size(),
              "corrupt bookmarks file recovered");
    }
    std::filesystem::remove_all(std::filesystem::path(dir).parent_path());
    std::printf("  bookmarks: seed, add/rename/delete, persist, corrupt-file recovery ok\n");
}

static void testMisc() {
    CHECK(RtlSdrSource::usbBlockBytes(2.048e6) == 262144 && RtlSdrSource::usbBlockBytes(2.4e6) == 262144 &&
              RtlSdrSource::usbBlockBytes(1.024e6) == 131072 && RtlSdrSource::usbBlockBytes(250e3) == 16384,
          "USB block sizing");
    CHECK(AudioSink::volumeToGain(1.0f) == 1.0f && AudioSink::volumeToGain(0.0f) == 0.0f &&
              std::fabs(20.0f * std::log10(AudioSink::volumeToGain(0.75f)) + 10.0f) < 0.01f &&
              std::fabs(20.0f * std::log10(AudioSink::volumeToGain(0.5f)) + 20.0f) < 0.01f, "log volume law");
    // Sink render: +6 dBFS in at 100 % volume never exceeds -6 dBFS; 75 % is 10 dB down.
    RingBuffer<float> ring(1 << 16);
    AudioSink sink(ring);
    std::vector<float> loud(2 * 9600);
    for (size_t i = 0; i < 9600; ++i) loud[2 * i] = loud[2 * i + 1] = 2.0f * std::sin(float(i) * 0.1f);
    ring.write(loud.data(), loud.size());
    sink.volume = 1.0f;
    std::vector<int16_t> pcm(2 * 4096);
    sink.render(pcm.data(), 4096);
    int pk = 0;
    for (int16_t v : pcm) pk = std::max(pk, std::abs(int(v)));
    CHECK(pk <= 16423 && pk > 15000, "sink output peak %d (ceiling 16423 = -6 dBFS)", pk);
    std::printf("  audio: log volume (75%% = -10 dB), sink peak %.2f dBFS for +6 dBFS input\n", 20.0 * std::log10(pk / 32767.0));
}

// Receiver cost alone (I/Q pre-generated), the number that matters for the CM0.
static void benchReceiver() {
    constexpr double fs = 1.024e6;
    const size_t n = size_t(fs * 2.0);
    SimSource src;
    src.setSampleRate(fs);
    src.setCenterFrequency(100.2e6);
    std::vector<uint8_t> raw(n * 2);
    src.generate(raw.data(), n);
    std::vector<cf32> iq(n);
    convertU8ToCf32(raw.data(), iq.data(), n, false);
    struct Case { const char* name; Demod m; double off; bool nr; };
    const Case cases[] = {{"WFM stereo+RDS", Demod::WFM, -200e3, false}, {"NFM", Demod::NFM, 200e3, false},
                          {"NFM +NB+ANF+NR", Demod::NFM, 200e3, true}, {"USB +NB+ANF+NR", Demod::USB, 400e3, true},
                          {"AM", Demod::AM, 400e3, false}};
    for (const Case& c : cases) {
        RingBuffer<float> audio(size_t(1) << 22);
        Receiver rx(audio);
        rx.setClockTracking(false);
        rx.setInputRate(fs);
        RxConfig cfg; cfg.mode = c.m; cfg.bandwidthHz = demodLimits(c.m).defaultBw;
        cfg.nb = cfg.anf = cfg.nr = c.nr;
        rx.setConfig(cfg);
        rx.setOffset(c.off);
        rx.process(iq.data(), 32768);  // build + warm up outside the timing
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i + 32768 <= n; i += 32768) rx.process(iq.data() + i, 32768);
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  bench %-16s @1.024 MSPS: %5.1f%% of one core on this host\n", c.name, 100.0 * dt / (double(n - 32768) / fs));
    }
}

int main() {
    testRingSpsc();
    testConvertAndDc();
    testSimSpectrum();
    testThreadedChain();
    testReceiver();
    testLimiter();
    testRtlTcp();
    testRadioEngine();
    testRds();
    testIqBalance();
    testNoiseBlanker();
    testAutoNotch();
    testSpectralNr();
    testNoHotLoopAllocations();
    testFileSource();
    testBookmarks();
    testMisc();
    benchReceiver();
    std::printf(failures ? "%d FAILURE(S)\n" : "all core tests passed\n", failures);
    return failures ? 1 : 0;
}
