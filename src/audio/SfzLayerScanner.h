#pragma once
// =============================================================================
//  SfzLayerScanner.h  —  finds the "layers" of an SFZ instrument
// =============================================================================
//  A layer is one zone of the instrument: a <region> with its own note range
//  (lokey..hikey). The scanner reads the SFZ text (no sfizz, no JUCE: std only)
//  and returns one SfzLayer per distinct note range, so the arranger can create
//  one child track per layer whose piano roll is limited to that range.
//
//  Rules:
//    * lokey/hikey/key may be set on <global>, <master>, <group> or <region> and
//      are inherited downwards; a region value overrides its parent's.
//    * Regions with the identical range (velocity layers, round robins) are
//      merged into one layer.
//    * The layer name is the sample file name of its first region (no folder, no
//      extension), otherwise "Layer N".
//    * At most kMaxLayers layers are returned (lowest notes first); SfzLayer
//      lists are sorted by note range.
//    * Layers whose ranges share keys are flagged (SfzLayer::overlaps), since
//      playing a note on one of them would also sound the other.
//
//  Not handled: #include and #define (ignored). Regions that pull their range
//  from them fall back to the full 0..127 range.
// =============================================================================

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

struct SfzLayer
{
    static constexpr int kMaxLayers = 128;
    std::string name;
    int  loKey       = 0;
    int  hiKey       = 127;
    int  numRegions  = 0;
    bool overlaps    = false; // shares at least one key with another layer
};

/** One zone (region) of an instrument: a name and a note range. */
struct SfzZone { std::string name; int lo = 0, hi = 127; };

namespace SfzLayerScanner
{
namespace detail
{
    struct Range { int lo = -1; int hi = -1; };   // -1 = not set at this level

    inline std::string stripComments (const std::string& s)
    {
        std::string out;
        out.reserve (s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
            {
                while (i < s.size() && s[i] != '\n') ++i;
                out += '\n';
            }
            else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < s.size() && ! (s[i] == '*' && s[i + 1] == '/')) ++i;
                ++i;
                out += ' ';
            }
            else
                out += s[i];
        }
        return out;
    }

    // "60", "c4", "C#4", "db3", "a-1". sfizz convention: c4 = 60. Returns -1 if invalid.
    inline int parseKey (std::string v)
    {
        while (! v.empty() && std::isspace ((unsigned char) v.back()))  v.pop_back();
        while (! v.empty() && std::isspace ((unsigned char) v.front())) v.erase (v.begin());
        if (v.empty()) return -1;

        if (std::isdigit ((unsigned char) v[0]) || v[0] == '-')
        {
            char* end = nullptr;
            const long n = std::strtol (v.c_str(), &end, 10);
            return (end != v.c_str() && n >= 0 && n <= 127) ? (int) n : -1;
        }

        static const int semis[7] = { 9, 11, 0, 2, 4, 5, 7 };   // a b c d e f g
        const int letter = std::tolower ((unsigned char) v[0]) - 'a';
        if (letter < 0 || letter > 6) return -1;
        size_t i = 1;
        int semi = semis[letter];
        if (i < v.size() && v[i] == '#')                     { ++semi; ++i; }
        else if (i < v.size() && (v[i] == 'b' || v[i] == 'B')) { --semi; ++i; }
        if (i >= v.size()) return -1;
        char* end = nullptr;
        const long oct = std::strtol (v.c_str() + i, &end, 10);
        if (end == v.c_str() + i) return -1;
        const long n = (oct + 1) * 12 + semi;
        return (n >= 0 && n <= 127) ? (int) n : -1;
    }

    struct Opcode { std::string name, value; };

    // Splits "a=1 b=some path with spaces c=2" into opcodes. A value runs until
    // the next " name=" token, so sample paths containing spaces survive.
    inline std::vector<Opcode> parseOpcodes (const std::string& body)
    {
        std::vector<Opcode> out;
        const size_t n = body.size();
        auto isNameChar = [] (char c) { return std::isalnum ((unsigned char) c) || c == '_' || c == '$'; };

        std::vector<std::pair<size_t, size_t>> starts;   // name start, '=' position
        for (size_t i = 0; i < n; ++i)
        {
            if (body[i] != '=') continue;
            size_t b = i;
            while (b > 0 && isNameChar (body[b - 1])) --b;
            if (b == i) continue;
            if (b == 0 || std::isspace ((unsigned char) body[b - 1]))
                starts.emplace_back (b, i);
        }
        for (size_t k = 0; k < starts.size(); ++k)
        {
            const size_t valBegin = starts[k].second + 1;
            const size_t valEnd   = (k + 1 < starts.size()) ? starts[k + 1].first : n;
            std::string v = body.substr (valBegin, valEnd - valBegin);
            while (! v.empty() && std::isspace ((unsigned char) v.back())) v.pop_back();
            std::string nm = body.substr (starts[k].first, starts[k].second - starts[k].first);
            std::transform (nm.begin(), nm.end(), nm.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
            out.push_back ({ std::move (nm), std::move (v) });
        }
        return out;
    }

    inline void applyKeyOpcodes (const std::vector<Opcode>& ops, Range& r)
    {
        for (auto& op : ops)
        {
            if (op.name == "key")        { const int k = parseKey (op.value); if (k >= 0) r.lo = r.hi = k; }
            else if (op.name == "lokey") { const int k = parseKey (op.value); if (k >= 0) r.lo = k; }
            else if (op.name == "hikey") { const int k = parseKey (op.value); if (k >= 0) r.hi = k; }
        }
    }

    inline Range inherit (const Range& parent, const Range& child)
    {
        return { child.lo >= 0 ? child.lo : parent.lo, child.hi >= 0 ? child.hi : parent.hi };
    }
}

/** Turns a zone list into layers: identical ranges merge, sorted by range, capped at
    SfzLayer::kMaxLayers. */
inline std::vector<SfzLayer> buildLayers (const std::vector<SfzZone>& zones, int* droppedCount = nullptr)
{
    if (droppedCount != nullptr) *droppedCount = 0;
    // Merge regions with the identical range into one layer.
    std::vector<SfzLayer> layers;
    for (const auto& z : zones)
    {
        auto it = std::find_if (layers.begin(), layers.end(),
                                [&] (const SfzLayer& l) { return l.loKey == z.lo && l.hiKey == z.hi; });
        if (it == layers.end())
        {
            SfzLayer l;
            l.name = z.name; l.loKey = z.lo; l.hiKey = z.hi; l.numRegions = 1;
            layers.push_back (std::move (l));
        }
        else
        {
            if (it->name.empty()) it->name = z.name;
            ++it->numRegions;
        }
    }

    std::sort (layers.begin(), layers.end(), [] (const SfzLayer& a, const SfzLayer& b)
               { return a.loKey != b.loKey ? a.loKey < b.loKey : a.hiKey < b.hiKey; });
    if ((int) layers.size() > SfzLayer::kMaxLayers)
    {
        if (droppedCount != nullptr) *droppedCount = (int) layers.size() - SfzLayer::kMaxLayers;
        layers.resize ((size_t) SfzLayer::kMaxLayers);
    }

    for (size_t i = 0; i < layers.size(); ++i)
    {
        if (layers[i].name.empty()) layers[i].name = "Layer " + std::to_string (i + 1);
        for (size_t j = 0; j < layers.size(); ++j)
            if (i != j && layers[i].loKey <= layers[j].hiKey && layers[j].loKey <= layers[i].hiKey)
                layers[i].overlaps = true;
    }
    return layers;
}

/** Scans SFZ text and returns the instrument's layers (empty if it has no regions). */
inline std::vector<SfzLayer> scan (const std::string& sfzText, int* droppedCount = nullptr)
{
    using namespace detail;

    std::vector<SfzZone> zones;

    Range global, master, group;

    const std::string text = stripComments (sfzText);
    size_t pos = 0;
    while (pos < text.size())
    {
        const size_t open = text.find ('<', pos);
        if (open == std::string::npos) break;
        const size_t close = text.find ('>', open);
        if (close == std::string::npos) break;

        const std::string header = text.substr (open + 1, close - open - 1);
        size_t next = text.find ('<', close);
        if (next == std::string::npos) next = text.size();
        const auto ops = parseOpcodes (text.substr (close + 1, next - close - 1));
        pos = next;

        if (header == "global")
        {
            global = {}; master = {}; group = {};
            applyKeyOpcodes (ops, global);
        }
        else if (header == "master")
        {
            master = {}; group = {};
            applyKeyOpcodes (ops, master);
        }
        else if (header == "group")
        {
            group = {};
            applyKeyOpcodes (ops, group);
        }
        else if (header == "region")
        {
            Range region;
            applyKeyOpcodes (ops, region);
            const Range eff = inherit (inherit (inherit (global, master), group), region);

            SfzZone z;
            z.lo = eff.lo >= 0 ? eff.lo : 0;
            z.hi = eff.hi >= 0 ? eff.hi : 127;
            for (auto& op : ops)
                if (op.name == "sample")
                {
                    std::string n = op.value;
                    const size_t slash = n.find_last_of ("/\\");
                    if (slash != std::string::npos) n = n.substr (slash + 1);
                    const size_t dot = n.find_last_of ('.');
                    if (dot != std::string::npos && dot > 0) n = n.substr (0, dot);
                    z.name = n;
                }
            zones.push_back (std::move (z));
        }
    }

    return buildLayers (zones, droppedCount);
}
} // namespace SfzLayerScanner
