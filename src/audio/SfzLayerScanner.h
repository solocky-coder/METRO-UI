#pragma once
// =============================================================================
//  SfzLayerScanner.h  —  finds the "layers" of an SFZ instrument
// =============================================================================
//  A layer is a note range of the instrument. The scanner reads the SFZ text
//  (no sfizz, no JUCE: std only) and returns one SfzLayer per distinct note
//  range, so the arranger can create one child track per layer whose piano
//  roll is limited to that range.
//
//  Rules:
//    * Regions are grouped by their <group> header (regions before the first
//      <group> form an implicit group).
//    * A group's range is the union of its regions' lokey..hikey. lokey/hikey/
//      key may be set on <global>, <master>, <group> or <region> and are
//      inherited downwards; a region value overrides its parent's.
//    * Groups with the identical range (velocity layers, round robins) are
//      merged into one layer.
//    * The layer name is the first non-empty group_label of its groups,
//      otherwise "Layer N".
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
    std::string name;
    int  loKey       = 0;
    int  hiKey       = 127;
    int  numRegions  = 0;
    int  numGroups   = 0;     // groups merged into this layer
    bool overlaps    = false; // shares at least one key with another layer
};

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

/** Scans SFZ text and returns the instrument's layers (empty if it has no regions). */
inline std::vector<SfzLayer> scan (const std::string& sfzText)
{
    using namespace detail;

    struct Group { std::string label; int lo = 128, hi = -1, regions = 0; };
    std::vector<Group> groups;

    Range global, master, group;
    int current = -1;                       // index into groups, -1 = none yet

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
            global = {}; master = {}; group = {}; current = -1;
            applyKeyOpcodes (ops, global);
        }
        else if (header == "master")
        {
            master = {}; group = {}; current = -1;
            applyKeyOpcodes (ops, master);
        }
        else if (header == "group")
        {
            group = {};
            applyKeyOpcodes (ops, group);
            groups.emplace_back();
            current = (int) groups.size() - 1;
            for (auto& op : ops)
                if (op.name == "group_label") groups.back().label = op.value;
        }
        else if (header == "region")
        {
            if (current < 0) { groups.emplace_back(); current = (int) groups.size() - 1; }

            Range region;
            applyKeyOpcodes (ops, region);
            const Range eff = inherit (inherit (inherit (global, master), group), region);
            const int lo = eff.lo >= 0 ? eff.lo : 0;
            const int hi = eff.hi >= 0 ? eff.hi : 127;

            auto& g = groups[(size_t) current];
            g.lo = std::min (g.lo, lo);
            g.hi = std::max (g.hi, hi);
            ++g.regions;
        }
    }

    // Merge groups with the identical range into one layer.
    std::vector<SfzLayer> layers;
    for (auto& g : groups)
    {
        if (g.regions == 0) continue;
        auto it = std::find_if (layers.begin(), layers.end(),
                                [&] (const SfzLayer& l) { return l.loKey == g.lo && l.hiKey == g.hi; });
        if (it == layers.end())
        {
            SfzLayer l;
            l.name = g.label; l.loKey = g.lo; l.hiKey = g.hi;
            l.numRegions = g.regions; l.numGroups = 1;
            layers.push_back (std::move (l));
        }
        else
        {
            if (it->name.empty()) it->name = g.label;
            it->numRegions += g.regions;
            ++it->numGroups;
        }
    }

    std::sort (layers.begin(), layers.end(), [] (const SfzLayer& a, const SfzLayer& b)
               { return a.loKey != b.loKey ? a.loKey < b.loKey : a.hiKey < b.hiKey; });

    for (size_t i = 0; i < layers.size(); ++i)
    {
        if (layers[i].name.empty()) layers[i].name = "Layer " + std::to_string (i + 1);
        for (size_t j = 0; j < layers.size(); ++j)
            if (i != j && layers[i].loKey <= layers[j].hiKey && layers[j].loKey <= layers[i].hiKey)
                layers[i].overlaps = true;
    }
    return layers;
}
} // namespace SfzLayerScanner
