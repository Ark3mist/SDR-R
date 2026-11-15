#include "engine/bookmarks.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace osx {

namespace fs = std::filesystem;
using nlohmann::json;

std::string BookmarkStore::defaultPath() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/cardsdr/bookmarks.json";
    const char* home = std::getenv("HOME");
    return std::string(home && *home ? home : ".") + "/.config/cardsdr/bookmarks.json";
}

const std::vector<std::string>& BookmarkStore::categories() {
    static const std::vector<std::string> c = {"Aviation", "Marine", "Weather", "FM", "Amateur", "User"};
    return c;
}

// Generic, region-neutral seeds. Edit or delete freely.
std::vector<Bookmark> BookmarkStore::defaults() {
    return {
        {"Air Guard 121.5", 121500000, "AM", 8330, "Aviation"},
        {"Air-to-Air 123.45", 123450000, "AM", 8330, "Aviation"},
        {"KLIA Approach", 119450000, "AM", 8330, "Aviation"},
        {"Marine Ch16 Distress", 156800000, "NFM", 12500, "Marine"},
        {"Marine Ch09 Calling", 156450000, "NFM", 12500, "Marine"},
        {"Marine Ch70 DSC", 156525000, "NFM", 12500, "Marine"},
        {"NOAA WX 162.550", 162550000, "NFM", 12500, "Weather"},
        {"NOAA WX 162.400", 162400000, "NFM", 12500, "Weather"},
        {"Simulator FM + RDS", 100000000, "WFM", 200000, "FM"},
        {"FM 98.0", 98000000, "WFM", 200000, "FM"},
        {"2 m Calling 145.500", 145500000, "NFM", 12500, "Amateur"},
        {"2 m SSB 144.300", 144300000, "USB", 2800, "Amateur"},
    };
}

BookmarkStore::BookmarkStore(std::string path) : path_(std::move(path)) {}

bool BookmarkStore::load() {
    error_.clear();
    std::error_code ec;
    if (!fs::exists(path_, ec)) {
        items_ = defaults();
        return save();
    }
    try {
        std::ifstream in(path_);
        const json j = json::parse(in);
        const json& arr = j.is_object() ? j.at("bookmarks") : j;
        std::vector<Bookmark> v;
        for (const json& e : arr) {
            Bookmark b;
            b.name = e.at("name").get<std::string>();
            b.frequencyHz = e.at("frequency_hz").get<int64_t>();
            b.demodMode = e.value("demod_mode", std::string("NFM"));
            b.bandwidthHz = e.value("bandwidth_hz", 12500);
            b.category = e.value("category", std::string("User"));
            v.push_back(std::move(b));
        }
        items_ = std::move(v);
        return true;
    } catch (const std::exception& ex) {
        error_ = std::string("bookmarks.json unreadable (") + ex.what() + "), saved aside as .bad";
        fs::rename(path_, path_ + ".bad", ec);
        items_ = defaults();
        save();
        return false;
    }
}

bool BookmarkStore::save() const {
    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    json arr = json::array();
    for (const Bookmark& b : items_)
        arr.push_back({{"name", b.name}, {"frequency_hz", b.frequencyHz}, {"demod_mode", b.demodMode},
                       {"bandwidth_hz", b.bandwidthHz}, {"category", b.category}});
    const json doc = {{"version", 1}, {"bookmarks", arr}};
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            error_ = "cannot write " + tmp;
            return false;
        }
        out << doc.dump(2) << "\n";
        if (!out) {
            error_ = "write failed: " + tmp;
            return false;
        }
    }
    fs::rename(tmp, path_, ec);  // atomic replace: a crash never leaves a half-written file
    if (ec) {
        error_ = "cannot replace " + path_ + ": " + ec.message();
        return false;
    }
    return true;
}

size_t BookmarkStore::add(const Bookmark& b) {
    items_.push_back(b);
    save();
    return items_.size() - 1;
}

void BookmarkStore::rename(size_t i, const std::string& name) {
    if (i >= items_.size() || name.empty()) return;
    items_[i].name = name;
    save();
}

void BookmarkStore::setCategory(size_t i, const std::string& cat) {
    if (i >= items_.size()) return;
    items_[i].category = cat;
    save();
}

void BookmarkStore::remove(size_t i) {
    if (i >= items_.size()) return;
    items_.erase(items_.begin() + std::ptrdiff_t(i));
    save();
}

}  // namespace osx
