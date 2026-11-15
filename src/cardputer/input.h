// Keyboard abstraction: the UI only ever sees KeyEvents. They come from SDL
// (window / KMSDRM mode) or straight from Linux evdev (framebuffer mode,
// where no SDL video driver owns the keyboard).
#pragma once

#include <string>
#include <vector>

union SDL_Event;

namespace osx {

enum class Key { None, Left, Right, Up, Down, Enter, Esc, Tab, Backspace, Space, Char };

struct KeyEvent {
    Key key = Key::None;
    char ch = 0;        // Key::Char: lower-case letter, digit or punctuation
    bool shift = false;
};

// SDL_KEYDOWN -> KeyEvent (key == None if the event is not a usable key).
KeyEvent keyFromSdl(const SDL_Event& e);

// Parses a test script: "TAB,2,DOWN,RIGHT,S-b,1,2,1,.,5,ENTER".
std::vector<KeyEvent> parseKeyScript(const std::string& script);

class EvdevKeyboard {
public:
    ~EvdevKeyboard();
    // Opens every /dev/input/event* that looks like a keyboard and grabs it
    // so keystrokes do not also reach the Linux console. Returns device count.
    int open();
    void close();
    void poll(std::vector<KeyEvent>& out);
    const std::string& description() const { return desc_; }

private:
    std::vector<int> fds_;
    bool shift_ = false;
    std::string desc_;
};

}  // namespace osx
