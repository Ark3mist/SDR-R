#include "cardputer/input.h"

#include <cctype>
#include <cstring>
#include <sstream>

#include <SDL.h>

#ifdef __linux__
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace osx {

char shiftedChar(char c, bool shift) {
    if (!shift) return c;
    if (c >= 'a' && c <= 'z') return char(c - 'a' + 'A');
    static const char* from = "1234567890-=[]\\;',./`";
    static const char* to = "!@#$%^&*()_+{}|:\"<>?~";
    for (size_t i = 0; from[i]; ++i)
        if (from[i] == c) return to[i];
    return c;
}

KeyEvent keyFromSdl(const SDL_Event& e) {
    KeyEvent k;
    if (e.type != SDL_KEYDOWN) return k;
    const SDL_Keycode sym = e.key.keysym.sym;
    k.shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
    k.ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;
    switch (sym) {
        case SDLK_LEFT: k.key = Key::Left; break;
        case SDLK_RIGHT: k.key = Key::Right; break;
        case SDLK_UP: k.key = Key::Up; break;
        case SDLK_DOWN: k.key = Key::Down; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: k.key = Key::Enter; break;
        case SDLK_ESCAPE: k.key = Key::Esc; break;
        case SDLK_TAB: k.key = Key::Tab; break;
        case SDLK_BACKSPACE: case SDLK_DELETE: k.key = Key::Backspace; break;
        case SDLK_SPACE: k.key = Key::Space; break;
        case SDLK_KP_PERIOD: k.key = Key::Char; k.ch = '.'; break;
        case SDLK_KP_PLUS: k.key = Key::Char; k.ch = '+'; break;
        case SDLK_KP_MINUS: k.key = Key::Char; k.ch = '-'; break;
        default:
            if (sym >= SDLK_KP_1 && sym <= SDLK_KP_9) { k.key = Key::Char; k.ch = char('1' + (sym - SDLK_KP_1)); }
            else if (sym == SDLK_KP_0) { k.key = Key::Char; k.ch = '0'; }
            else if (sym > 32 && sym < 127) { k.key = Key::Char; k.ch = char(sym); }
            break;
    }
    if (k.key == Key::Char) k.text = shiftedChar(k.ch, k.shift);
    else if (k.key == Key::Space) k.text = ' ';
    return k;
}

std::vector<KeyEvent> parseKeyScript(const std::string& script) {
    std::vector<KeyEvent> out;
    std::stringstream ss(script);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        KeyEvent k;
        for (;;) {
            if (tok.rfind("S-", 0) == 0 && tok.size() > 2) { k.shift = true; tok = tok.substr(2); }
            else if (tok.rfind("C-", 0) == 0 && tok.size() > 2) { k.ctrl = true; tok = tok.substr(2); }
            else break;
        }
        if (tok == "LEFT") k.key = Key::Left;
        else if (tok == "RIGHT") k.key = Key::Right;
        else if (tok == "UP") k.key = Key::Up;
        else if (tok == "DOWN") k.key = Key::Down;
        else if (tok == "ENTER") k.key = Key::Enter;
        else if (tok == "ESC") k.key = Key::Esc;
        else if (tok == "TAB") k.key = Key::Tab;
        else if (tok == "BKSP") k.key = Key::Backspace;
        else if (tok == "SPACE") k.key = Key::Space;
        else if (tok == "NOP") k.key = Key::None;
        else if (tok == "COMMA") { k.key = Key::Char; k.ch = ','; }
        else if (tok.size() == 1) { k.key = Key::Char; k.ch = char(std::tolower(static_cast<unsigned char>(tok[0]))); }
        else continue;
        if (k.key == Key::Char) k.text = shiftedChar(k.ch, k.shift);
        else if (k.key == Key::Space) k.text = ' ';
        out.push_back(k);
    }
    return out;
}

#ifdef __linux__
namespace {
bool testBit(const unsigned long* bits, int bit) {
    return (bits[size_t(bit) / (8 * sizeof(unsigned long))] >> (size_t(bit) % (8 * sizeof(unsigned long)))) & 1ul;
}

char charForCode(int code) {
    static const struct { int code; char ch; } map[] = {
        {KEY_A, 'a'}, {KEY_B, 'b'}, {KEY_C, 'c'}, {KEY_D, 'd'}, {KEY_E, 'e'}, {KEY_F, 'f'}, {KEY_G, 'g'},
        {KEY_H, 'h'}, {KEY_I, 'i'}, {KEY_J, 'j'}, {KEY_K, 'k'}, {KEY_L, 'l'}, {KEY_M, 'm'}, {KEY_N, 'n'},
        {KEY_O, 'o'}, {KEY_P, 'p'}, {KEY_Q, 'q'}, {KEY_R, 'r'}, {KEY_S, 's'}, {KEY_T, 't'}, {KEY_U, 'u'},
        {KEY_V, 'v'}, {KEY_W, 'w'}, {KEY_X, 'x'}, {KEY_Y, 'y'}, {KEY_Z, 'z'},
        {KEY_1, '1'}, {KEY_2, '2'}, {KEY_3, '3'}, {KEY_4, '4'}, {KEY_5, '5'}, {KEY_6, '6'}, {KEY_7, '7'},
        {KEY_8, '8'}, {KEY_9, '9'}, {KEY_0, '0'},
        {KEY_KP1, '1'}, {KEY_KP2, '2'}, {KEY_KP3, '3'}, {KEY_KP4, '4'}, {KEY_KP5, '5'}, {KEY_KP6, '6'},
        {KEY_KP7, '7'}, {KEY_KP8, '8'}, {KEY_KP9, '9'}, {KEY_KP0, '0'}, {KEY_KPDOT, '.'},
        {KEY_DOT, '.'}, {KEY_COMMA, ','}, {KEY_SLASH, '/'}, {KEY_SEMICOLON, ';'}, {KEY_MINUS, '-'},
        {KEY_EQUAL, '='}, {KEY_GRAVE, '`'}, {KEY_APOSTROPHE, '\''}, {KEY_LEFTBRACE, '['}, {KEY_RIGHTBRACE, ']'},
        {KEY_BACKSLASH, '\\'}, {KEY_KPPLUS, '+'}, {KEY_KPMINUS, '-'},
    };
    for (const auto& m : map)
        if (m.code == code) return m.ch;
    return 0;
}
}  // namespace

EvdevKeyboard::~EvdevKeyboard() { close(); }

int EvdevKeyboard::open() {
    close();
    for (int i = 0; i < 32; ++i) {
        const std::string path = "/dev/input/event" + std::to_string(i);
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long keys[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = {};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 && testBit(keys, KEY_A) && testBit(keys, KEY_ENTER)) {
            char name[128] = "?";
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            ioctl(fd, EVIOCGRAB, 1);
            fds_.push_back(fd);
            desc_ += (desc_.empty() ? "" : ", ") + std::string(name);
        } else {
            ::close(fd);
        }
    }
    return int(fds_.size());
}

void EvdevKeyboard::close() {
    for (int fd : fds_) {
        ioctl(fd, EVIOCGRAB, 0);
        ::close(fd);
    }
    fds_.clear();
    desc_.clear();
}

void EvdevKeyboard::poll(std::vector<KeyEvent>& out) {
    input_event ev[32];
    for (int fd : fds_) {
        for (;;) {
            const ssize_t n = ::read(fd, ev, sizeof(ev));
            if (n < ssize_t(sizeof(input_event))) break;
            for (size_t i = 0; i < size_t(n) / sizeof(input_event); ++i) {
                if (ev[i].type != EV_KEY) continue;
                const int code = ev[i].code, value = ev[i].value;  // 0 up, 1 down, 2 auto-repeat
                if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) { shift_ = value != 0; continue; }
                if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) { ctrl_ = value != 0; continue; }
                if (value == 0) continue;
                KeyEvent k;
                k.shift = shift_;
                k.ctrl = ctrl_;
                switch (code) {
                    case KEY_LEFT: k.key = Key::Left; break;
                    case KEY_RIGHT: k.key = Key::Right; break;
                    case KEY_UP: k.key = Key::Up; break;
                    case KEY_DOWN: k.key = Key::Down; break;
                    case KEY_ENTER: case KEY_KPENTER: k.key = Key::Enter; break;
                    case KEY_ESC: k.key = Key::Esc; break;
                    case KEY_TAB: k.key = Key::Tab; break;
                    case KEY_BACKSPACE: case KEY_DELETE: k.key = Key::Backspace; break;
                    case KEY_SPACE: k.key = Key::Space; break;
                    default:
                        if (const char c = charForCode(code)) { k.key = Key::Char; k.ch = c; }
                        break;
                }
                if (k.key == Key::Char) k.text = shiftedChar(k.ch, k.shift);
                else if (k.key == Key::Space) k.text = ' ';
                if (k.key != Key::None) out.push_back(k);
            }
        }
    }
}
#else
EvdevKeyboard::~EvdevKeyboard() {}
int EvdevKeyboard::open() { return 0; }
void EvdevKeyboard::close() {}
void EvdevKeyboard::poll(std::vector<KeyEvent>&) {}
#endif

}  // namespace osx
