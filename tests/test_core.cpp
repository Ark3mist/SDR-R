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

#include "audio/limiter.h"
#include "dsp/dsp_engine.h"
#include "engine/radio.h"
#include "source/rtltcp_source.h"
#include "dsp/receiver.h"
#include "dsp/spectrum_analyzer.h"
#include "source/sim_source.h"

using namespace osx;

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
    dc.setTimeConstant(0.01, 100e3);
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

struct RxRun { std::vector<float> l, r; float ctcss; bool stereo; float level; double seconds; };

static RxRun runRx(double centerHz, double offsetHz, RxConfig cfg, double seconds = 2.5, double fs = 1.024e6) {
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
    CHECK(radio.sampleRate() == 1.024e6 && radio.analyzer.fftSize() == 1024 && radio.analyzer.rate() == 30, "CM0 boot defaults");
    radio.tune(100.4e6);
    radio.setMode(Demod::NFM);
    CHECK(radio.play(), "play: %s", radio.lastError().c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const float level = radio.receiver.levelDb.load(), ctcss = radio.receiver.ctcssHz.load();
    radio.tune(1.0905e9);  // outside the current span: hardware centre must follow
    CHECK(std::fabs(radio.vfoHz() - 1.0905e9) < 0.5 && std::fabs(radio.vfo.offsetHz) <= 0.512e6, "retune");
    radio.selectSource(Radio::SourceKind::RtlSdr);  // no dongle here: must fail cleanly
    const bool rtlStarted = radio.playing();
    radio.stop();
    std::printf("  engine: NFM level %.1f dBFS, CTCSS %.1f Hz, audio sink '%s', RTL-SDR %s\n", double(level), double(ctcss),
                radio.audio.status().c_str(), rtlStarted ? "started" : radio.lastError().c_str());
    CHECK(std::fabs(level + 30.0f) < 2.0f && std::fabs(ctcss - 100.0f) < 0.01f, "live NFM chain");
    CHECK(radio.sim.overruns() == 0 && radio.receiver.audioDrops.load() == 0, "overruns / audio drops");
    radio.shutdown();
}

static void benchReceiver() {
    for (Demod m : {Demod::WFM, Demod::NFM, Demod::USB}) {
        RxConfig c; c.mode = m; c.bandwidthHz = demodLimits(m).defaultBw;
        const auto t0 = std::chrono::steady_clock::now();
        runRx(100.2e6, -200e3, c, 4.0);
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  bench %s @1.024 MSPS: %.1fx real time on this host (includes signal generation)\n", demodName(m), 4.0 / dt);
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
    benchReceiver();
    std::printf(failures ? "%d FAILURE(S)\n" : "all core tests passed\n", failures);
    return failures ? 1 : 0;
}
