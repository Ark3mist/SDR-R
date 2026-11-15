#include "source/rtltcp_source.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace osx {

RtlTcpSource::RtlTcpSource()
    : gains_{0.0f, 0.9f, 1.4f, 2.7f, 3.7f, 7.7f, 8.7f, 12.5f, 14.4f, 15.7f, 16.6f, 19.7f, 20.7f, 22.9f, 25.4f,
             28.0f, 29.7f, 32.8f, 33.8f, 36.4f, 37.2f, 38.6f, 40.2f, 42.1f, 43.4f, 43.9f, 44.5f, 48.0f, 49.6f} {}

RtlTcpSource::~RtlTcpSource() { stop(); }

std::vector<double> RtlTcpSource::sampleRates() const {
    return {250e3, 1.024e6, 1.536e6, 1.792e6, 1.92e6, 2.048e6, 2.16e6, 2.4e6, 2.56e6, 2.88e6, 3.2e6};
}

// rtl_tcp command: 1 byte opcode + 32-bit big-endian parameter.
void RtlTcpSource::command(uint8_t cmd, uint32_t param) {
    const int fd = fd_.load();
    if (fd < 0) return;
    uint8_t pkt[5] = {cmd, uint8_t(param >> 24), uint8_t(param >> 16), uint8_t(param >> 8), uint8_t(param)};
    (void)::send(fd, pkt, sizeof(pkt), MSG_NOSIGNAL);
}

bool RtlTcpSource::start(RingBuffer<uint8_t>& out) {
    if (running_.load()) return true;
    error_.clear();

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        error_ = "rtl_tcp: cannot resolve " + host;
        return false;
    }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{3, 0};  // bounded connect / handshake
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        error_ = "rtl_tcp: cannot connect to " + host + ":" + std::to_string(port);
        return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // 12-byte greeting: "RTL0", tuner type, gain count (big-endian).
    uint8_t hdr[12];
    size_t got = 0;
    while (got < sizeof(hdr)) {
        const ssize_t r = ::recv(fd, hdr + got, sizeof(hdr) - got, 0);
        if (r <= 0) break;
        got += size_t(r);
    }
    if (got != sizeof(hdr) || std::memcmp(hdr, "RTL0", 4) != 0) {
        ::close(fd);
        error_ = "rtl_tcp: bad greeting from " + host;
        return false;
    }
    fd_.store(fd);
    command(0x02, uint32_t(sampleRate_));
    command(0x09, uint32_t(directSampling_));
    command(0x0a, offsetTuning_ ? 1 : 0);
    command(0x05, uint32_t(ppm_));
    command(0x01, uint32_t(centerHz_));
    command(0x08, rtlAgc_ ? 1 : 0);
    command(0x03, tunerAgc_ ? 0 : 1);
    if (!tunerAgc_) command(0x04, uint32_t(std::lround(gainDb_ * 10.0f)));
    command(0x0e, biasTee_ ? 1 : 0);

    running_.store(true);
    thread_ = std::thread(&RtlTcpSource::run, this, &out);
    return true;
}

void RtlTcpSource::run(RingBuffer<uint8_t>* out) {
    std::vector<uint8_t> buf(size_t(std::clamp(bufferKb, 4, 256)) * 1024);
    const int fd = fd_.load();
    bool odd = false;  // keep I/Q pairing if a read ends on an odd byte
    uint8_t carry = 0;
    while (running_.load(std::memory_order_relaxed)) {
        pollfd p{fd, POLLIN, 0};
        const int pr = ::poll(&p, 1, 200);
        if (pr == 0) continue;
        if (pr < 0 && errno == EINTR) continue;
        ssize_t r = pr < 0 ? -1 : ::recv(fd, buf.data() + 1, buf.size() - 1, 0);
        if (r <= 0) {
            if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (running_.exchange(false)) error_ = "rtl_tcp: connection closed by server";
            break;
        }
        uint8_t* data = buf.data() + 1;
        size_t len = size_t(r);
        if (odd) {  // prepend the byte held back last time
            --data;
            *data = carry;
            ++len;
        }
        odd = (len & 1u) != 0;
        if (odd) carry = data[--len];
        const size_t w = out->write(data, len);
        if (w < len) overruns_.fetch_add(len - w, std::memory_order_relaxed);
    }
}

void RtlTcpSource::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    const int fd = fd_.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

void RtlTcpSource::setCenterFrequency(double hz) {
    centerHz_ = hz;
    command(0x01, uint32_t(std::clamp(hz, 0.0, 4294967295.0)));
}

void RtlTcpSource::setGain(float db) {
    float best = gains_.front();
    for (float g : gains_)
        if (std::fabs(g - db) < std::fabs(best - db)) best = g;
    gainDb_ = best;
    if (!tunerAgc_) command(0x04, uint32_t(std::lround(gainDb_ * 10.0f)));
}

void RtlTcpSource::setDirectSampling(int mode) { directSampling_ = std::clamp(mode, 0, 2); command(0x09, uint32_t(directSampling_)); }
void RtlTcpSource::setOffsetTuning(bool on) { offsetTuning_ = on; command(0x0a, on ? 1 : 0); }
void RtlTcpSource::setRtlAgc(bool on) { rtlAgc_ = on; command(0x08, on ? 1 : 0); }
void RtlTcpSource::setTunerAgc(bool on) {
    tunerAgc_ = on;
    command(0x03, on ? 0 : 1);
    if (!on) command(0x04, uint32_t(std::lround(gainDb_ * 10.0f)));
}
void RtlTcpSource::setPpm(int ppm) { ppm_ = std::clamp(ppm, -200, 200); command(0x05, uint32_t(ppm_)); }
void RtlTcpSource::setBiasTee(bool on) { biasTee_ = on; command(0x0e, on ? 1 : 0); }

}  // namespace osx
