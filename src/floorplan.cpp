// ##################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2026 ARH Laboratory
//  SPDX-License-Identifier: MIT
// ##################################################################################################
#include "floorplan.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace pdi_where {
namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw PdiError(path + ": cannot open");
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// "SLICE_X12Y34" -> {"SLICE", 12, 34}
bool site_name(const std::string& s, std::string& type, int& x, int& y) {
    static const std::regex re(R"(^(.+)_X(\d+)Y(\d+)$)");
    std::smatch m;
    if (!std::regex_match(s, m, re)) return false;
    type = m[1];
    x = std::stoi(m[2]);
    y = std::stoi(m[3]);
    return true;
}

// Split a Tcl-ish line into words, keeping [...] and {...} groups as one word each.
std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0, n = line.size();
    while (i < n) {
        while (i < n && isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= n) break;
        char open = line[i];
        if (open == '[' || open == '{' || open == '"') {
            char close = open == '[' ? ']' : open == '{' ? '}' : '"';
            int depth = 0;
            size_t j = i;
            for (; j < n; ++j) {
                if (line[j] == open && open != '"') ++depth;
                else if (line[j] == close) {
                    if (open == '"' && j > i) break;
                    if (open != '"' && --depth == 0) break;
                }
            }
            out.push_back(line.substr(i, std::min(j, n - 1) - i + 1));
            i = j + 1;
        } else {
            size_t j = i;
            while (j < n && !isspace(static_cast<unsigned char>(line[j]))) ++j;
            out.push_back(line.substr(i, j - i));
            i = j;
        }
    }
    return out;
}

std::string strip(std::string w) {
    while (!w.empty() && (w.front() == '{' || w.front() == '"' || w.front() == '[')) w.erase(0, 1);
    while (!w.empty() && (w.back() == '}' || w.back() == '"' || w.back() == ']')) w.pop_back();
    return w;
}

// The pblock a [get_pblocks ...] word, or a bare pblock word, names.
std::string pblock_of(const std::string& w) {
    if (w.empty()) return "";
    if (w.front() != '[') return strip(w);
    auto in = words(w.substr(1, w.size() - 2));
    if (in.empty() || in[0] != "get_pblocks") return "";
    for (size_t k = 1; k < in.size(); ++k)
        if (in[k][0] != '-') return strip(in[k]);
    return "";
}

void add_ranges(Pblock& pb, const std::string& list, bool remove) {
    std::istringstream s(strip(list));
    std::string r;
    while (s >> r) {
        auto colon = r.find(':');
        std::string a = r.substr(0, colon), b = colon == std::string::npos ? a : r.substr(colon + 1);
        Range rg;
        int ax, ay, bx, by;
        std::string tb;
        if (!site_name(a, rg.type, ax, ay) || !site_name(b, tb, bx, by) || tb != rg.type)
            throw PdiError(pb.source + ": cannot read range \"" + r + "\" of " + pb.name);
        rg.x0 = std::min(ax, bx), rg.x1 = std::max(ax, bx);
        rg.y0 = std::min(ay, by), rg.y1 = std::max(ay, by);
        rg.remove = remove;
        pb.ranges.push_back(rg);
    }
}

}  // namespace

int Device::cfu_row(int slr, int cr_row) const {
    const auto& v = fabric_rows[slr];
    auto it = std::find(v.begin(), v.end(), cr_row);
    return it == v.end() ? -1 : int(it - v.begin());
}

bool load_frames(Device& d, const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    parse_frames(d, slurp(path));
    return true;
}

const std::vector<FrameCol>* Device::columns(int slr, int row) const {
    auto it = frame_cols[slr].find(row);
    return it == frame_cols[slr].end() || it->second.empty() ? nullptr : &it->second;
}

Device parse_device(const std::string& text) {
    Device d;
    std::istringstream in(text);
    std::string line;
    std::array<std::set<int>, kSlrs> rows;
    while (std::getline(in, line)) {
        if (line.rfind("# pdi_where device table: part ", 0) == 0) {
            d.part = line.substr(31, line.find(',') - 31);
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        std::istringstream f(line);
        std::string type, slr, cr, row;
        DeviceCell c;
        std::getline(f, type, '\t');
        std::getline(f, slr, '\t');
        std::getline(f, cr, '\t');
        std::getline(f, row, '\t');
        if (!(f >> c.xmin >> c.xmax >> c.ymin >> c.ymax >> c.sites))
            throw PdiError("device table: bad line: " + line);
        c.type = type;
        c.slr = std::stoi(slr);
        if (c.slr < 0 || c.slr >= kSlrs) throw PdiError("device table: SLR out of range: " + line);
        c.cr_row = row == "-" ? -1 : std::stoi(row);
        std::string t;
        int cx = -1, cy = -1;
        if (cr != "-" && !site_name("CR_" + cr, t, cx, cy))
            throw PdiError("device table: bad clock region: " + line);
        c.cr_x = cx;
        if (c.type == "SLICE" && c.cr_row >= 0) rows[c.slr].insert(c.cr_row);
        d.cells.push_back(c);
    }
    if (d.cells.empty()) throw PdiError("device table is empty");
    for (int k = 0; k < kSlrs; ++k) d.fabric_rows[k].assign(rows[k].begin(), rows[k].end());
    return d;
}

Device load_device(const std::string& path) { return parse_device(slurp(path)); }

std::string frames_path(const std::string& device_path) {
    const std::string suf = ".tsv";
    if (device_path.size() >= suf.size() && device_path.compare(device_path.size() - suf.size(), suf.size(), suf) == 0)
        return device_path.substr(0, device_path.size() - suf.size()) + ".frames.tsv";
    return device_path + ".frames.tsv";
}

void parse_frames(Device& d, const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("# pdi_where frame table: part ", 0) == 0) {
            d.frames_part = line.substr(30, line.find(',') - 30);
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        std::istringstream f(line);
        int slr, row;
        FrameCol c;
        if (!(f >> slr >> row >> c.x0 >> c.x1 >> c.frame) || slr < 0 || slr >= kSlrs || c.x0 > c.x1)
            throw PdiError("frame table: bad line: " + line);
        d.frame_cols[slr][row].push_back(c);
    }
    if (d.frames_part != d.part)
        throw PdiError("frame table is for part " + d.frames_part + ", device table for " + d.part);
    for (auto& rows : d.frame_cols)
        for (auto& [row, v] : rows) {
            std::sort(v.begin(), v.end(), [](const FrameCol& a, const FrameCol& b) { return a.x0 < b.x0; });
            for (size_t i = 1; i < v.size(); ++i)
                if (v[i].x0 <= v[i - 1].x1 || v[i].frame <= v[i - 1].frame)
                    throw PdiError("frame table: row " + std::to_string(row) + ": columns overlap or frames fall as X rises");
        }
}

std::vector<Pblock> parse_xdc(const std::string& text, const std::string& source) {
    // join backslash continuations, drop comments
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line, acc;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line.back() == '\\') {
            acc += line.substr(0, line.size() - 1) + " ";
            continue;
        }
        acc += line;
        size_t first = acc.find_first_not_of(" \t");
        if (first != std::string::npos && acc[first] != '#') lines.push_back(acc.substr(first));
        acc.clear();
    }

    std::vector<Pblock> pbs;
    auto get = [&](const std::string& name) -> Pblock& {
        for (auto& p : pbs)
            if (p.name == name) return p;
        throw PdiError(source + ": pblock \"" + name + "\" used before create_pblock");
    };
    for (const auto& l : lines) {
        auto w = words(l);
        if (w.empty()) continue;
        if (w[0] == "create_pblock" && w.size() >= 2) {
            Pblock p;
            p.name = strip(w.back());
            p.source = source;
            pbs.push_back(p);
        } else if (w[0] == "resize_pblock" && w.size() >= 2) {
            Pblock& p = get(pblock_of(w[1]));
            for (size_t k = 2; k + 1 < w.size(); ++k)
                if (w[k] == "-add" || w[k] == "-remove") add_ranges(p, w[k + 1], w[k] == "-remove");
        } else if (w[0] == "add_cells_to_pblock" && w.size() >= 3) {
            Pblock& p = get(pblock_of(w[1]));
            static const std::regex cell(R"(\blist\s+([^\]]+)\])");
            std::smatch m;
            if (std::regex_search(w[2], m, cell)) {
                std::istringstream cs(m[1]);
                std::string c;
                while (cs >> c) p.cells.push_back(strip(c));
            }
        } else if (w[0] == "set_property" && w.size() >= 4) {
            std::string pbn = pblock_of(w[3]);
            if (pbn.empty() || w[3].find("get_pblocks") == std::string::npos) continue;
            Pblock& p = get(pbn);
            if (w[1] == "SNAPPING_MODE") p.snapping = strip(w[2]) != "OFF";
            if (w[1] == "CONTAIN_ROUTING") p.contain_routing = strip(w[2]) == "1" || strip(w[2]) == "true";
        }
    }
    return pbs;
}

std::vector<Pblock> load_xdc(const std::string& path) { return parse_xdc(slurp(path), path); }

Footprint footprint(const Pblock& pb, const Device& dev) {
    Footprint fp;
    std::set<std::string> known, unknown;
    for (const auto& c : dev.cells) known.insert(c.type);
    for (const auto& r : pb.ranges) {
        if (r.remove) {
            fp.has_remove = true;
            continue;
        }
        bool cr = r.type == "CLOCKREGION";
        if (!cr && !known.count(r.type)) {
            unknown.insert(r.type);
            continue;
        }
        for (const auto& c : dev.cells) {
            double frac;
            if (cr) {
                if (c.cr_row < 0 || c.cr_x < 0 || c.cr_x < r.x0 || c.cr_x > r.x1 || c.cr_row < r.y0 || c.cr_row > r.y1)
                    continue;
                frac = 1.0;
            } else {
                if (c.type != r.type) continue;
                int x0 = std::max(r.x0, c.xmin), x1 = std::min(r.x1, c.xmax);
                int y0 = std::max(r.y0, c.ymin), y1 = std::min(r.y1, c.ymax);
                if (x0 > x1 || y0 > y1) continue;
                frac = double(x1 - x0 + 1) * (y1 - y0 + 1) /
                       (double(c.xmax - c.xmin + 1) * (c.ymax - c.ymin + 1));
            }
            fp.slr[c.slr] = true;
            fp.types[c.slr].insert(cr ? "CLOCKREGION" : c.type);
            if (c.cr_row >= 0) {
                int row = dev.cfu_row(c.slr, c.cr_row);
                if (row >= 0) fp.cfu_rows[c.slr].insert(row);
                if (row >= 0 && c.type == "SLICE") {
                    fp.fabric_rows[c.slr].insert(row);
                    int x0 = cr ? c.xmin : std::max(r.x0, c.xmin), x1 = cr ? c.xmax : std::min(r.x1, c.xmax);
                    for (int x = x0; x <= x1; ++x) fp.slice_cols[c.slr][row].insert(x);
                }
            }
            if (c.type == "SLICE") fp.slice_sites[c.slr] += frac * c.sites;
        }
    }
    fp.unknown_types.assign(unknown.begin(), unknown.end());
    return fp;
}

}  // namespace pdi_where
