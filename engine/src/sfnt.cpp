#include "sfnt.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace pdfengine::font {
namespace {
std::uint16_t rd16(const Bytes& b, std::size_t o) { return o + 2 <= b.size() ? static_cast<std::uint16_t>(b[o] << 8 | b[o + 1]) : 0; }
std::uint32_t rd32(const Bytes& b, std::size_t o) { return o + 4 <= b.size() ? (std::uint32_t(b[o]) << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]) : 0; }
void put16(Bytes& b, std::uint16_t v) { b.push_back(v >> 8); b.push_back(v & 255); }
void put32(Bytes& b, std::uint32_t v) { put16(b, v >> 16); put16(b, v & 0xFFFF); }
constexpr std::size_t maxFontBytes = 96 * 1024 * 1024;
// fsType: bit 1 forbids embedding, bit 8 forbids subsetting.
bool embeddable(std::uint16_t fsType) { return !(fsType & 0x0002) && !(fsType & 0x0100); }
std::uint32_t checksum(const std::uint8_t* p, std::size_t n) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < n; i += 4) {
        std::uint32_t word = 0;
        for (std::size_t j = 0; j < 4; ++j) word = word << 8 | (i + j < n ? p[i + j] : 0);
        sum += word;
    }
    return sum;
}
}

Cmap::Cmap(Bytes table) : data_(std::move(table)) {
    const auto count = rd16(data_, 2);
    int best = 0;
    for (std::size_t i = 0; i < count && 4 + i * 8 + 8 <= data_.size(); ++i) {
        auto platform = rd16(data_, 4 + i * 8), encoding = rd16(data_, 6 + i * 8);
        std::size_t offset = rd32(data_, 8 + i * 8);
        auto format = rd16(data_, offset);
        if (format != 4 && format != 12) continue;
        int rank = 0;
        if (platform == 3 && encoding == 10) rank = 4;
        else if (platform == 0 && (encoding == 4 || encoding == 6)) rank = 3;
        else if (platform == 3 && encoding == 1) rank = 2;
        else if (platform == 0) rank = 1;
        if (rank > best) { best = rank; offset_ = offset; format_ = format; }
    }
}
std::uint32_t Cmap::glyph(char32_t cp) const {
    if (format_ == 12) {
        std::size_t groups = rd32(data_, offset_ + 12), lo = 0, hi = groups;
        while (lo < hi) {
            auto mid = (lo + hi) / 2; auto base = offset_ + 16 + mid * 12;
            if (base + 12 > data_.size()) return 0;
            auto start = rd32(data_, base), end = rd32(data_, base + 4);
            if (cp < start) hi = mid; else if (cp > end) lo = mid + 1; else return rd32(data_, base + 8) + (cp - start);
        }
    } else if (format_ == 4 && cp <= 0xFFFF) {
        std::size_t segments = rd16(data_, offset_ + 6) / 2, lo = 0, hi = segments;
        const auto ends = offset_ + 14, starts = ends + segments * 2 + 2, deltas = starts + segments * 2, ranges = deltas + segments * 2;
        while (lo < hi) {
            auto mid = (lo + hi) / 2;
            if (ranges + mid * 2 + 2 > data_.size()) return 0;
            auto end = rd16(data_, ends + mid * 2), start = rd16(data_, starts + mid * 2);
            if (cp > end) lo = mid + 1;
            else if (cp < start) hi = mid;
            else {
                auto delta = rd16(data_, deltas + mid * 2), range = rd16(data_, ranges + mid * 2);
                if (range == 0) return (cp + delta) & 0xFFFF;
                auto gid = rd16(data_, ranges + mid * 2 + range + (cp - start) * 2);
                return gid ? (gid + delta) & 0xFFFF : 0;
            }
        }
    }
    return 0;
}

const Sfnt::Table* Sfnt::find(const std::string& tag) const {
    for (const auto& item : tables_) if (item.first == tag) return &item.second;
    return nullptr;
}
std::uint32_t Sfnt::u32(std::size_t o) const { return rd32(data_, o); }
std::uint16_t Sfnt::u16(std::size_t o) const { return rd16(data_, o); }

std::shared_ptr<Sfnt> Sfnt::parse(Bytes data) {
    if (data.size() < 12 || data.size() > maxFontBytes) return {};
    auto version = rd32(data, 0);
    if (version != 0x00010000 && version != 0x74727565) return {};
    auto font = std::make_shared<Sfnt>();
    font->data_ = std::move(data);
    const auto& d = font->data_;
    auto count = rd16(d, 4);
    if (count == 0 || count > 64 || 12 + std::size_t(count) * 16 > d.size()) return {};
    for (std::size_t i = 0; i < count; ++i) {
        auto record = 12 + i * 16;
        std::string tag(reinterpret_cast<const char*>(&d[record]), 4);
        std::size_t offset = rd32(d, record + 8), length = rd32(d, record + 12);
        if (offset > d.size() || length > d.size() - offset) return {};
        font->tables_.push_back({tag, {offset, length}});
    }
    auto head = font->find("head"), hhea = font->find("hhea"), maxp = font->find("maxp"), hmtx = font->find("hmtx"),
         loca = font->find("loca"), glyf = font->find("glyf"), cmap = font->find("cmap"), os2 = font->find("OS/2");
    if (!head || !hhea || !maxp || !hmtx || !loca || !glyf || !cmap || head->length < 54 || hhea->length < 36 || maxp->length < 6) return {};
    if (os2 && os2->length >= 10 && !embeddable(rd16(d, os2->offset + 8))) return {};
    font->unitsPerEm = rd16(d, head->offset + 18);
    if (font->unitsPerEm < 16 || font->unitsPerEm > 16384) return {};
    font->xMin = static_cast<std::int16_t>(rd16(d, head->offset + 36)); font->yMin = static_cast<std::int16_t>(rd16(d, head->offset + 38));
    font->xMax = static_cast<std::int16_t>(rd16(d, head->offset + 40)); font->yMax = static_cast<std::int16_t>(rd16(d, head->offset + 42));
    font->longLoca_ = rd16(d, head->offset + 50) != 0;
    font->ascent = static_cast<std::int16_t>(rd16(d, hhea->offset + 4)); font->descent = static_cast<std::int16_t>(rd16(d, hhea->offset + 6));
    font->hMetrics_ = rd16(d, hhea->offset + 34);
    font->numGlyphs = rd16(d, maxp->offset + 4);
    if (font->numGlyphs == 0 || font->hMetrics_ == 0 || font->hMetrics_ > font->numGlyphs || hmtx->length < std::size_t(font->hMetrics_) * 4 ||
        loca->length < (std::size_t(font->numGlyphs) + 1) * (font->longLoca_ ? 4 : 2)) return {};
    font->cmap_ = Cmap(Bytes(d.begin() + cmap->offset, d.begin() + cmap->offset + cmap->length));
    if (!font->cmap_.valid()) return {};
    return font;
}
std::shared_ptr<Sfnt> Sfnt::load(const std::filesystem::path& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size > maxFontBytes) return {};
    std::ifstream in(path, std::ios::binary);
    Bytes data(size);
    if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) return {};
    auto font = parse(std::move(data));
    if (font) font->name = path.stem().string();
    return font;
}
double Sfnt::advance(std::uint32_t gid) const {
    auto hmtx = find("hmtx");
    if (!hmtx) return 0;
    auto index = std::min<std::uint32_t>(gid, hMetrics_ - 1u);
    return u16(hmtx->offset + std::size_t(index) * 4);
}

Bytes Sfnt::subset(const std::set<std::uint32_t>& requested) const {
    const auto& loca = *find("loca"); const auto& glyf = *find("glyf");
    auto glyphRange = [&](std::uint32_t gid) -> std::pair<std::size_t, std::size_t> {
        std::size_t a, b;
        if (longLoca_) { a = u32(loca.offset + std::size_t(gid) * 4); b = u32(loca.offset + std::size_t(gid) * 4 + 4); }
        else { a = std::size_t(u16(loca.offset + std::size_t(gid) * 2)) * 2; b = std::size_t(u16(loca.offset + std::size_t(gid) * 2 + 2)) * 2; }
        if (b < a || b > glyf.length) return {0, 0};
        return {a, b - a};
    };
    std::set<std::uint32_t> keep{0};
    std::vector<std::uint32_t> pending(requested.begin(), requested.end());
    pending.push_back(0);
    while (!pending.empty()) {
        auto gid = pending.back(); pending.pop_back();
        if (gid >= numGlyphs) continue;
        keep.insert(gid);
        auto [start, length] = glyphRange(gid);
        auto base = glyf.offset + start;
        if (length < 10 || static_cast<std::int16_t>(u16(base)) >= 0) continue;
        std::size_t pos = base + 10;
        for (int components = 0; components < 256; ++components) {
            if (pos + 4 > base + length) break;
            auto flags = u16(pos), component = u16(pos + 2);
            if (!keep.count(component)) pending.push_back(component);
            pos += 4 + ((flags & 1) ? 4 : 2);
            if (flags & 8) pos += 2; else if (flags & 0x40) pos += 4; else if (flags & 0x80) pos += 8;
            if (!(flags & 0x20)) break;
        }
    }
    Bytes newGlyf, newLoca;
    for (std::uint32_t gid = 0; gid < numGlyphs; ++gid) {
        put32(newLoca, static_cast<std::uint32_t>(newGlyf.size()));
        if (!keep.count(gid)) continue;
        auto [start, length] = glyphRange(gid);
        newGlyf.insert(newGlyf.end(), data_.begin() + glyf.offset + start, data_.begin() + glyf.offset + start + length);
        while (newGlyf.size() % 4) newGlyf.push_back(0);
    }
    put32(newLoca, static_cast<std::uint32_t>(newGlyf.size()));
    std::map<std::string, Bytes> out;
    auto copy = [&](const std::string& tag) { if (auto t = find(tag)) out[tag] = Bytes(data_.begin() + t->offset, data_.begin() + t->offset + t->length); };
    for (const char* tag : {"head", "hhea", "maxp", "hmtx", "cvt ", "fpgm", "prep"}) copy(tag);
    out["glyf"] = std::move(newGlyf); out["loca"] = std::move(newLoca);
    out["head"][8] = out["head"][9] = out["head"][10] = out["head"][11] = 0;
    out["head"][50] = 0; out["head"][51] = 1;
    Bytes file;
    put32(file, 0x00010000);
    const std::uint16_t n = static_cast<std::uint16_t>(out.size());
    std::uint16_t pow2 = 1, log = 0; while (pow2 * 2 <= n) { pow2 *= 2; ++log; }
    put16(file, n); put16(file, pow2 * 16); put16(file, log); put16(file, n * 16 - pow2 * 16);
    std::size_t offset = 12 + std::size_t(n) * 16, headRecord = 0, index = 0;
    for (auto& [tag, bytes] : out) {
        if (tag == "head") headRecord = 12 + index * 16;
        file.insert(file.end(), tag.begin(), tag.end());
        put32(file, checksum(bytes.data(), bytes.size())); put32(file, static_cast<std::uint32_t>(offset)); put32(file, static_cast<std::uint32_t>(bytes.size()));
        offset += (bytes.size() + 3) & ~std::size_t(3); ++index;
    }
    std::size_t headOffset = 0;
    for (auto& [tag, bytes] : out) {
        if (tag == "head") headOffset = file.size();
        file.insert(file.end(), bytes.begin(), bytes.end());
        while (file.size() % 4) file.push_back(0);
    }
    (void)headRecord;
    auto adjust = 0xB1B0AFBAu - checksum(file.data(), file.size());
    for (int i = 0; i < 4; ++i) file[headOffset + 8 + i] = static_cast<std::uint8_t>(adjust >> (24 - 8 * i));
    return file;
}

namespace {
bool wanted(const std::string& name) {
    std::string lower(name); std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const char* skip : {"braille", "keyboard", "emoji", "webdings", "wingdings", "symbol", "lastresort", "bold", "italic", "oblique", "black", "light", "narrow", "condensed", "mono"})
        if (lower.find(skip) != std::string::npos) return false;
    return true;
}
struct Catalog {
    std::vector<SystemFont> fonts;
    std::map<char32_t, int> cache;
    Catalog() {
        std::vector<std::filesystem::path> roots;
        if (const char* extra = std::getenv("FOLIOFORGE_FONT_PATH")) {
            std::stringstream list(extra); std::string item;
#ifdef _WIN32
            const char separator = ';';
#else
            const char separator = ':';
#endif
            while (std::getline(list, item, separator)) if (!item.empty()) roots.push_back(item);
        }
#ifdef _WIN32
        if (const char* windir = std::getenv("WINDIR")) roots.push_back(std::filesystem::path(windir) / "Fonts");
#elif defined(__APPLE__)
        roots = {"/System/Library/Fonts", "/System/Library/Fonts/Supplemental", "/Library/Fonts"};
        if (const char* home = std::getenv("HOME")) roots.push_back(std::filesystem::path(home) / "Library/Fonts");
#else
        roots = {"/usr/share/fonts", "/usr/local/share/fonts"};
        if (const char* home = std::getenv("HOME")) { roots.push_back(std::filesystem::path(home) / ".fonts"); roots.push_back(std::filesystem::path(home) / ".local/share/fonts"); }
#endif
        std::vector<std::pair<int, std::filesystem::path>> files;
        auto rank = [](const std::string& name) {
            static const std::array<const char*, 8> preferred = {"arial unicode", "arial", "notosans-regular", "notosans", "dejavusans", "segoeui", "liberationsans-regular", "helvetica"};
            std::string lower(name); std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            for (std::size_t i = 0; i < preferred.size(); ++i) if (lower.rfind(preferred[i], 0) == 0) return static_cast<int>(i);
            return 100;
        };
        for (const auto& root : roots) {
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end && files.size() < 4000; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                auto ext = it->path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                auto stem = it->path().stem().string();
                if (ext == ".ttf" && wanted(stem)) files.push_back({rank(stem), it->path()});
            }
        }
        std::stable_sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [ignored, path] : files) {
            std::ifstream in(path, std::ios::binary);
            Bytes header(12);
            if (!in.read(reinterpret_cast<char*>(header.data()), 12)) continue;
            auto version = rd32(header, 0);
            if (version != 0x00010000 && version != 0x74727565) continue;
            auto count = rd16(header, 4);
            if (count == 0 || count > 64) continue;
            Bytes directory(std::size_t(count) * 16);
            if (!in.read(reinterpret_cast<char*>(directory.data()), static_cast<std::streamsize>(directory.size()))) continue;
            std::size_t cmapOffset = 0, cmapLength = 0; bool ok = true, hasGlyf = false;
            for (std::size_t i = 0; i < count; ++i) {
                std::string tag(reinterpret_cast<const char*>(&directory[i * 16]), 4);
                std::size_t offset = rd32(directory, i * 16 + 8), length = rd32(directory, i * 16 + 12);
                if (tag == "glyf") hasGlyf = true;
                if (tag == "cmap" && length < 8 * 1024 * 1024) { cmapOffset = offset; cmapLength = length; }
                if (tag == "OS/2" && length >= 10) {
                    Bytes os2(10); in.clear(); in.seekg(static_cast<std::streamoff>(offset));
                    if (in.read(reinterpret_cast<char*>(os2.data()), 10) && !embeddable(rd16(os2, 8))) ok = false;
                }
            }
            if (!ok || !hasGlyf || !cmapLength) continue;
            Bytes table(cmapLength); in.clear(); in.seekg(static_cast<std::streamoff>(cmapOffset));
            if (!in.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(cmapLength))) continue;
            Cmap cmap(std::move(table));
            if (cmap.valid()) fonts.push_back({path, std::move(cmap)});
        }
    }
};
std::mutex catalogMutex;
Catalog& catalog() { static Catalog instance; return instance; }
}

void FontSource::setFonts(std::vector<std::filesystem::path> paths) {
    explicit_.clear();
    for (const auto& path : paths) {
        auto font = Sfnt::load(path);
        if (!font) throw std::runtime_error("Unsupported font file: " + path.filename().string() + ". A TrueType (glyf) font that permits embedding is required.");
        explicit_.push_back(std::move(font));
    }
}
std::shared_ptr<Sfnt> FontSource::find(char32_t cp) {
    for (const auto& font : explicit_) if (font->glyph(cp)) return font;
    std::lock_guard lock(catalogMutex);
    auto& cat = catalog();
    auto known = cat.cache.find(cp);
    int index = -1;
    if (known != cat.cache.end()) index = known->second;
    else {
        for (std::size_t i = 0; i < cat.fonts.size(); ++i) if (cat.fonts[i].cmap.glyph(cp)) { index = static_cast<int>(i); break; }
        cat.cache[cp] = index;
    }
    if (index < 0) return {};
    const auto& path = cat.fonts[static_cast<std::size_t>(index)].path;
    for (const auto& [loadedPath, font] : loaded_) if (loadedPath == path) return font;
    auto font = Sfnt::load(path);
    if (!font) return {};
    loaded_.push_back({path, font});
    return font;
}
}
