#include "source/file_source.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <regex>

namespace osx {

namespace {
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
}  // namespace

double centerFromFilename(const std::string& path) {
    static const std::regex re("(\\d{4,11})Hz");
    std::smatch m;
    const std::string base = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
    if (std::regex_search(base, m, re)) return std::stod(m[1].str());
    return 0.0;
}

bool parseIqWav(const std::string& path, WavInfo& info, std::string& error) {
    info = WavInfo{};
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long long fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        std::fclose(f);
        error = "not a RIFF/WAVE file";
        return false;
    }
    int fmtTag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    bool haveFmt = false;
    for (;;) {
        uint8_t ch[8];
        if (std::fread(ch, 1, 8, f) != 8) break;
        const uint32_t size = le32(ch + 4);
        const long long body = std::ftell(f);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[40] = {};
            const size_t want = std::min<size_t>(size, sizeof(fmt));
            if (std::fread(fmt, 1, want, f) != want || want < 16) break;
            fmtTag = le16(fmt);
            channels = le16(fmt + 2);
            rate = le32(fmt + 4);
            bits = le16(fmt + 14);
            if (fmtTag == 0xFFFE && want >= 26) fmtTag = le16(fmt + 24);  // EXTENSIBLE: sub-format GUID
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            if (!haveFmt) break;
            info.dataOffset = uint64_t(body);
            // Streaming writers leave 0 / 0xFFFFFFFF here: fall back to the file size.
            uint64_t bytes = size;
            if (size == 0 || size == 0xFFFFFFFFu || body + (long long)size > fileSize) bytes = uint64_t(fileSize - body);
            const int bytesPerFrame = channels * bits / 8;
            if (channels != 2) { error = "need 2 channels (I/Q), file has " + std::to_string(channels); break; }
            if (fmtTag == 1 && bits == 8) info.encoding = 8;
            else if (fmtTag == 1 && bits == 16) info.encoding = 16;
            else if (fmtTag == 3 && bits == 32) info.encoding = 32;
            else { error = "unsupported sample format (" + std::to_string(bits) + "-bit, tag " + std::to_string(fmtTag) + ")"; break; }
            info.sampleRate = double(rate);
            info.frames = bytes / uint64_t(bytesPerFrame);
            info.centerHz = centerFromFilename(path);
            std::fclose(f);
            if (info.frames == 0) { error = "no samples"; return false; }
            return true;
        }
        std::fseek(f, body + (long long)size + (size & 1), SEEK_SET);  // chunks are word aligned
    }
    std::fclose(f);
    if (error.empty()) error = haveFmt ? "no data chunk" : "no fmt chunk";
    return false;
}

FileSource::~FileSource() {
    stop();
    if (file_) std::fclose(file_);
}

bool FileSource::open(const std::string& path) {
    if (running()) return false;
    WavInfo wi;
    std::string err;
    if (!parseIqWav(path, wi, err)) {
        error_ = err;
        return false;
    }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error_ = "cannot open " + path;
        return false;
    }
    if (file_) std::fclose(file_);
    file_ = f;
    info_ = wi;
    path_ = path;
    error_.clear();
    pos_ = 0;
    seekTo_ = -1;
    if (wi.centerHz > 0) centerHz_ = wi.centerHz;
    raw_.assign(size_t(65536) * 8, 0);  // largest chunk the player asks for, f32 frames
    return true;
}

double FileSource::position() const {
    return info_.frames ? double(pos_.load()) / double(info_.frames) : 0.0;
}

void FileSource::seek(double fraction) {
    seekTo_.store(int64_t(std::clamp(fraction, 0.0, 1.0) * double(info_.frames)));
}

size_t FileSource::read(cf32* dst, size_t n) {
    if (!file_) return 0;
    const int64_t req = seekTo_.exchange(-1);
    if (req >= 0) pos_ = std::min<uint64_t>(uint64_t(req), info_.frames);
    const size_t bpf = size_t(info_.encoding / 8) * 2;
    n = std::min(n, raw_.size() / bpf);
    size_t done = 0;
    while (done < n) {
        uint64_t pos = pos_.load();
        if (pos >= info_.frames) {
            if (!loop.load()) break;
            pos = 0;
        }
        const size_t want = size_t(std::min<uint64_t>(n - done, info_.frames - pos));
        std::fseek(file_, long(info_.dataOffset + pos * bpf), SEEK_SET);
        const size_t got = std::fread(raw_.data(), bpf, want, file_);
        if (got == 0) break;
        const uint8_t* r = raw_.data();
        cf32* o = dst + done;
        switch (info_.encoding) {
            case 8:
                for (size_t i = 0; i < got; ++i) o[i] = {(float(r[2 * i]) - 127.5f) / 127.5f, (float(r[2 * i + 1]) - 127.5f) / 127.5f};
                break;
            case 16:
                for (size_t i = 0; i < got; ++i) {
                    int16_t a, b;
                    std::memcpy(&a, r + 4 * i, 2);
                    std::memcpy(&b, r + 4 * i + 2, 2);
                    o[i] = {float(a) / 32768.0f, float(b) / 32768.0f};
                }
                break;
            default:
                std::memcpy(o, r, got * sizeof(cf32));  // little-endian float32 pairs == std::complex<float>
                break;
        }
        done += got;
        pos_ = pos + got;
    }
    return done;
}

bool FileSource::start(const SourceRings& out) {
    if (!out.f32) return false;
    if (!file_) {
        error_ = "no IQ file loaded";
        return false;
    }
    if (running_.load()) return true;
    if (thread_.joinable()) thread_.join();  // reap a player that hit end of file
    running_.store(true);
    if (pos_.load() >= info_.frames) pos_ = 0;
    thread_ = std::thread(&FileSource::run, this, out.f32);
    return true;
}

void FileSource::stop() {
    running_.store(false);  // the thread may already have ended itself at end of file
    if (thread_.joinable()) thread_.join();
}

void FileSource::run(RingBuffer<cf32>* out) {
    using clock = std::chrono::steady_clock;
    const double sr = sampleRate();
    const size_t block = std::clamp<size_t>(size_t(sr / 200.0), 1024, 65536);  // ~5 ms
    std::vector<cf32> buf(block);
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(double(block) / sr));
    auto next = clock::now();
    while (running_.load(std::memory_order_relaxed)) {
        const size_t got = read(buf.data(), block);
        if (got == 0) {  // end of file without loop
            error_ = "end of IQ file";
            running_.store(false);
            break;
        }
        const size_t w = out->write(buf.data(), got);
        if (w < got) overruns_.fetch_add((got - w) * sizeof(cf32), std::memory_order_relaxed);
        next += period;
        const auto now = clock::now();
        if (now - next > std::chrono::milliseconds(250)) next = now;
        std::this_thread::sleep_until(next);
    }
}

}  // namespace osx
