// Headless verification of the non-GUI pipeline:
//   ring buffer integrity, u8 conversion, DC blocker, simulated source ->
//   FFT level/frequency accuracy, and the threaded source->DSP->FFT chain.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "dsp/dsp_engine.h"
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

int main() {
    testRingSpsc();
    testConvertAndDc();
    testSimSpectrum();
    testThreadedChain();
    std::printf(failures ? "%d FAILURE(S)\n" : "all core tests passed\n", failures);
    return failures ? 1 : 0;
}
