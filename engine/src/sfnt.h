#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Minimal TrueType (glyf outline) reader and subsetter used to embed fallback fonts.
namespace pdfengine::font {
using Bytes = std::vector<std::uint8_t>;

class Cmap {
public:
    Cmap() = default;
    explicit Cmap(Bytes table);
    std::uint32_t glyph(char32_t) const; // 0 when the code point is not covered
    bool valid() const { return format_ != 0; }
private:
    Bytes data_;
    std::size_t offset_{};
    int format_{};
};

class Sfnt {
public:
    // Null unless `data` is a single TrueType-outline font whose license permits subsetting/embedding.
    static std::shared_ptr<Sfnt> parse(Bytes data);
    static std::shared_ptr<Sfnt> load(const std::filesystem::path&);
    std::uint32_t glyph(char32_t cp) const { return cmap_.glyph(cp); }
    double advance(std::uint32_t gid) const; // in font design units
    // Keeps glyph ids stable (unused glyph outlines are emptied) so a CID can equal a glyph id.
    Bytes subset(const std::set<std::uint32_t>& gids) const;
    std::uint16_t unitsPerEm{1000};
    std::uint32_t numGlyphs{};
    std::int16_t xMin{}, yMin{}, xMax{}, yMax{}, ascent{}, descent{};
    std::string name;
private:
    struct Table { std::size_t offset{}, length{}; };
    Bytes data_;
    std::vector<std::pair<std::string, Table>> tables_;
    Cmap cmap_;
    std::uint16_t hMetrics_{};
    bool longLoca_{};
    const Table* find(const std::string&) const;
    std::uint32_t u32(std::size_t) const;
    std::uint16_t u16(std::size_t) const;
};

struct SystemFont { std::filesystem::path path; Cmap cmap; };

// Resolves a code point to a font that has a glyph for it: caller-provided fonts first, then installed fonts.
class FontSource {
public:
    void setFonts(std::vector<std::filesystem::path> paths);
    std::shared_ptr<Sfnt> find(char32_t cp);
private:
    std::vector<std::shared_ptr<Sfnt>> explicit_;
    std::vector<std::pair<std::filesystem::path, std::shared_ptr<Sfnt>>> loaded_;
};
}
