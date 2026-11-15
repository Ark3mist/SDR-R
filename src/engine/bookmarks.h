// Persistent frequency bookmarks: ~/.config/cardsdr/bookmarks.json
// ($XDG_CONFIG_HOME/cardsdr/... when set). Seeded with defaults on first run.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace osx {

struct Bookmark {
    std::string name;
    int64_t frequencyHz = 0;
    std::string demodMode = "NFM";   // demodName() spelling
    int bandwidthHz = 12500;
    std::string category = "User";

    bool operator==(const Bookmark&) const = default;
};

class BookmarkStore {
public:
    static std::string defaultPath();
    static const std::vector<std::string>& categories();  // display / filter order
    static std::vector<Bookmark> defaults();

    explicit BookmarkStore(std::string path = defaultPath());

    // Loads the file, or seeds and writes defaults if it does not exist.
    // A corrupt file is kept aside as bookmarks.json.bad and defaults load.
    bool load();
    bool save() const;                       // atomic: write temp file, rename
    const std::string& path() const { return path_; }
    const std::string& lastError() const { return error_; }

    std::vector<Bookmark>& items() { return items_; }
    const std::vector<Bookmark>& items() const { return items_; }
    size_t add(const Bookmark& b);           // returns index; saves
    void rename(size_t i, const std::string& name);
    void setCategory(size_t i, const std::string& cat);
    void remove(size_t i);

private:
    std::string path_;
    mutable std::string error_;
    std::vector<Bookmark> items_;
};

}  // namespace osx
