// ##################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2026 ARH Laboratory
//  SPDX-License-Identifier: MIT
// ##################################################################################################
//
// pdi_where: say which reconfigurable pblock a partial PDI belongs to.
//
//   pdi_where [-v] [--json] --xdc impl.xdc [--xdc other.xdc ...] [--device TSV] [--rp NAME ...]
//             FILE.pdi|FILE.vbin ...
//   pdi_where --selftest | --version
//
// Evidence, strongest first:
//   1. SLRs. A partial writes configuration frames for every SLR its pblock has fabric in, and for
//      no other: an SLR it does not touch gets only its fixed empty CFU chunks (896 B compressed,
//      1,456 B uncompressed, no NPI writes; a configured SLR carries at least 1.45 MB). The
//      pblock's SLRs come from the XDC ranges through the device table.
//   2. Clock-region rows: each frame write names its CFU row, which is the index of the clock-region
//      row among the SLR's fabric rows (rows with SLICE sites). Uncompressed streams are decoded
//      frame by frame. Compressed streams are not decoded, but their FAR packet headers survive
//      byte-shifted; a row counts as written when its header pattern occurs at least
//      max(500, 2 % of the SLR's busiest row) times and the row exists in the SLR (74 real compressed
//      partials: 522 written rows with 7,954-70,579 hits, at least 12 % of the busiest row; rows
//      not written at most 70 hits; the out-of-range values 82, 161 and 253 recur with up to 60k
//      hits and are dropped only by the row-count check). Every row in which the
//      pblock has SLICE sites must be written (hard). A row written outside the pblock counts
//      against it but does not reject it: a full-device SLASH compute partial writes SLR0 CFU row 0,
//      which no site of its pblock reaches, and the service layer's partial writes a GT-side part
//      of SLR0 row 0 through hard blocks that lie in no clock region.
//   3. Tie-break when several pblocks pass: fewest rows written outside the pblock, then the share
//      of CFU bytes per SLR against the share of the pblock's SLICE sites per SLR (frames scale
//      with area; compressed bytes only roughly).
// Status: "match" when exactly one pblock passes the hard tests (SLRs, rows the pblock must have
// written), or when several do and exactly one of them has no row written outside it; "ambiguous"
// when several pass with no row outside (nearest by area share is named); "probable" when several
// pass but every one has rows outside it (the closest is named); "none" when nothing passes.
//
// Exit status: 0 every PDI is a "match"; 1 some PDI is probable, ambiguous or none; 2 error.
#include <unistd.h>

#include "config.hpp"
#include "git.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "floorplan.hpp"
#include "pdi.hpp"

using namespace pdi_where;

namespace {

const char* kSlrName[kSlrs] = {"SLR0", "SLR1", "SLR2"};

struct Candidate {
    const Pblock* pb;
    Footprint fp;
};

struct Verdict {
    const Candidate* c = nullptr;
    bool slr_ok = true, rows_ok = true, rows_checked = false;
    int extra_rows = 0;            // rows written outside the pblock (uncompressed only)
    std::vector<std::string> why;  // reasons it fails, or notes
    double share_dist = 0;         // L1 distance of per-SLR shares, 0..2
};

std::string join(const std::set<int>& s) {
    std::ostringstream o;
    bool first = true;
    for (int v : s) o << (first ? "" : ",") << v, first = false;
    return o.str();
}

// "row 3" or "rows 0,2,3"
std::string rows_of(const std::set<int>& s) { return (s.size() == 1 ? "row " : "rows ") + join(s); }

std::string slr_list(const std::array<bool, kSlrs>& a) {
    std::string o;
    for (int k = 0; k < kSlrs; ++k)
        if (a[k]) o += (o.empty() ? "" : "+") + std::string(kSlrName[k]);
    return o.empty() ? "none" : o;
}

constexpr uint64_t kMinFarHits = 500;
constexpr double kMinFarShare = 0.02;

// CFU rows an SLR's stream writes. Decoded chunks contribute their frame rows directly; chunks that
// did not decode contribute the thresholded FAR-header scan. The two are unioned, so one
// undecodable chunk cannot hide the rows of chunks that did decode in the same SLR.
std::set<int> rows_written(const SlrUse& u, size_t fabric_rows) {
    std::set<int> out;
    for (int r : u.cfu_rows)
        if (r < int(fabric_rows)) out.insert(r);
    uint64_t top = 0;
    for (auto [r, n] : u.far_hits)
        if (r < int(fabric_rows)) top = std::max(top, n);
    for (auto [r, n] : u.far_hits)
        if (r < int(fabric_rows) && n >= kMinFarHits && n >= kMinFarShare * top) out.insert(r);
    return out;
}

const Device* g_dev = nullptr;  // set in main / selftest; rows_written needs the SLR's row count

Verdict judge(const PdiInfo& p, const Candidate& c) {
    Verdict v;
    v.c = &c;
    double pdi_tot = 0, pb_tot = 0;
    for (int k = 0; k < kSlrs; ++k) {
        const SlrUse& u = p.slr[k];
        bool fabric = c.fp.slice_sites[k] > 0;
        if (u.configured() && !c.fp.slr[k]) {
            v.slr_ok = false;
            v.why.push_back(std::string(kSlrName[k]) + " written, not in pblock");
        }
        if (!u.configured() && fabric) {
            v.slr_ok = false;
            v.why.push_back(std::string(kSlrName[k]) + " in pblock, not written");
        }
        if (!u.configured() && c.fp.slr[k] && !fabric) {
            std::string t;
            for (const auto& s : c.fp.types[k]) t += (t.empty() ? "" : ",") + s;
            v.why.push_back(std::string("note: ") + kSlrName[k] + " reached only via " + t + ", not written");
        }
        std::set<int> written = rows_written(u, g_dev->fabric_rows[k].size());
        if (u.configured() && !written.empty()) {
            v.rows_checked = true;
            std::set<int> extra, missing;
            for (int r : written)
                if (!c.fp.cfu_rows[k].count(r)) extra.insert(r);
            for (int r : c.fp.fabric_rows[k])
                if (!written.count(r)) missing.insert(r);
            if (!missing.empty()) {
                v.rows_ok = false;
                v.why.push_back(std::string(kSlrName[k]) + " " + rows_of(missing) + " in pblock, not written");
            }
            if (!extra.empty()) {
                v.extra_rows += int(extra.size());
                v.why.push_back(std::string(kSlrName[k]) + " " + rows_of(extra) + " written outside pblock" +
                                (c.fp.cfu_rows[k].empty() ? "" : " (pblock has " + join(c.fp.cfu_rows[k]) + ")"));
            }
        }
        if (u.configured()) pdi_tot += double(u.cfu_bytes);
        pb_tot += c.fp.slice_sites[k];
    }
    for (int k = 0; k < kSlrs; ++k) {
        double a = pdi_tot > 0 && p.slr[k].configured() ? p.slr[k].cfu_bytes / pdi_tot : 0;
        double b = pb_tot > 0 ? c.fp.slice_sites[k] / pb_tot : 0;
        v.share_dist += std::fabs(a - b);
    }
    return v;
}

// "top_i/service_layer" -> does the member name start with "top_i_service_layer_"?
std::string name_hint(const PdiInfo& p, const std::vector<Candidate>& cs) {
    std::string base = p.name.substr(p.name.find_last_of("/:") + 1);
    for (const auto& c : cs)
        for (std::string cell : c.pb->cells) {
            std::replace(cell.begin(), cell.end(), '/', '_');
            if (base.rfind(cell + "_", 0) == 0) return c.pb->name;
        }
    return "";
}

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) {
        if (ch == '"' || ch == '\\') o += '\\';
        o += ch;
    }
    return o + "\"";
}

struct Result {
    const Candidate* chosen = nullptr;
    std::string status;  // "match", "probable", "ambiguous", "none"
    std::vector<Verdict> verdicts;
    std::string hint;
};

Result classify(const PdiInfo& p, const std::vector<Candidate>& cs) {
    Result r;
    for (const auto& c : cs) r.verdicts.push_back(judge(p, c));
    std::vector<const Verdict*> hard, exact;
    for (const auto& v : r.verdicts)
        if (v.slr_ok && v.rows_ok) {
            hard.push_back(&v);
            if (v.extra_rows == 0) exact.push_back(&v);
        }
    auto order = [](const Verdict* a, const Verdict* b) {
        return a->extra_rows != b->extra_rows ? a->extra_rows < b->extra_rows : a->share_dist < b->share_dist;
    };
    std::sort(hard.begin(), hard.end(), order);
    std::sort(exact.begin(), exact.end(), order);
    r.hint = name_hint(p, cs);
    if (hard.empty()) {
        r.status = "none";
    } else if (hard.size() == 1 || exact.size() == 1) {
        r.status = "match";
        r.chosen = hard[0]->c;
    } else if (exact.size() > 1) {
        r.status = "ambiguous";
        r.chosen = exact[0]->c;  // nearest by area share; reported as a guess
    } else {
        r.status = "probable";
        r.chosen = hard[0]->c;
    }
    return r;
}

std::string basename(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }

std::string verdict(const Result& r) {
    if (r.status == "none") return "no match";
    return r.status == "match" ? r.chosen->pb->name : r.chosen->pb->name + " (" + r.status + ")";
}

void print_short(const PdiInfo& p, const Result& r) {
    std::printf("%s: %s\n", p.name.c_str(), verdict(r).c_str());
    if (!r.hint.empty() && (!r.chosen || r.chosen->pb->name != r.hint))
        std::fprintf(stderr, "pdi_where: warning: %s: member name points to %s\n", p.name.c_str(), r.hint.c_str());
}

void print_long(const PdiInfo& p, const Result& r) {
    std::printf("%s: %s\n", p.name.c_str(), verdict(r).c_str());
    std::printf("  %llu bytes, idcode 0x%08x, image 0x%08x (%s), %s\n\n",
                static_cast<unsigned long long>(p.bytes), p.id_code, p.image_id, p.image_name.c_str(),
                p.compressed() ? "compressed" : "uncompressed");
    std::printf("  SLR  %10s  %4s  rows\n", "CFU bytes", "NPI");
    for (int k = 0; k < kSlrs; ++k) {
        const SlrUse& u = p.slr[k];
        std::set<int> rows = rows_written(u, g_dev->fabric_rows[k].size());
        std::printf("  %-3d  %10llu  %4d  %-7s", k, static_cast<unsigned long long>(u.cfu_bytes), u.npi_writes,
                    u.configured() && !rows.empty() ? join(rows).c_str() : "-");
        if (!u.configured())
            std::printf("  not configured");
        else if (u.decoded)
            std::printf("  %llu frames", static_cast<unsigned long long>(u.frames));
        else {
            std::printf("  far hits");
            for (auto [row, n] : u.far_hits)
                if (row < int(g_dev->fabric_rows[k].size()))
                    std::printf(" %d:%llu", row, static_cast<unsigned long long>(n));
        }
        std::printf("\n");
    }
    int wn = 6, wx = 3;
    for (const auto& v : r.verdicts) {
        wn = std::max(wn, int(v.c->pb->name.size()));
        wx = std::max(wx, int(basename(v.c->pb->source).size()));
    }
    std::printf("\n  %-*s  %-*s  %-5s  %4s\n", wn, "pblock", wx, "xdc", "SLRs", "dist");
    for (const auto& v : r.verdicts) {
        std::string slrs;
        for (int k = 0; k < kSlrs; ++k)
            if (v.c->fp.slr[k]) slrs += (slrs.empty() ? "" : ",") + std::to_string(k);
        std::string state = !(v.slr_ok && v.rows_ok) ? "rejected"
                            : v.extra_rows == 0      ? "fits"
                                                     : "fits, " + std::to_string(v.extra_rows) +
                                                      (v.extra_rows == 1 ? " row" : " rows") + " outside";
        std::printf("  %-*s  %-*s  %-5s  %4.2f  %s\n", wn, v.c->pb->name.c_str(), wx,
                    basename(v.c->pb->source).c_str(), slrs.empty() ? "-" : slrs.c_str(), v.share_dist, state.c_str());
        for (const auto& w : v.why) std::printf("    %s\n", w.c_str());
    }
    if (!r.hint.empty())
        std::printf("\n  member name points to %s%s\n", r.hint.c_str(),
                    r.chosen && r.chosen->pb->name == r.hint ? "" : " (differs)");
}

void print_json(const PdiInfo& p, const Result& r, bool last) {
    std::printf("  {\"pdi\": %s, \"bytes\": %llu, \"id_code\": \"0x%08x\", \"image_id\": \"0x%08x\", "
                "\"compressed\": %s,\n   \"slrs\": [",
                json_str(p.name).c_str(), static_cast<unsigned long long>(p.bytes), p.id_code,
                p.image_id, p.compressed() ? "true" : "false");
    for (int k = 0; k < kSlrs; ++k) {
        const SlrUse& u = p.slr[k];
        std::printf("%s{\"slr\": %d, \"configured\": %s, \"cfu_bytes\": %llu, \"npi_writes\": %d",
                    k ? ", " : "", k, u.configured() ? "true" : "false",
                    static_cast<unsigned long long>(u.cfu_bytes), u.npi_writes);
        if (u.decoded) std::printf(", \"frames\": %llu", static_cast<unsigned long long>(u.frames));
        if (u.configured())
            std::printf(", \"cfu_rows\": [%s], \"rows_from\": \"%s\"",
                        join(rows_written(u, g_dev->fabric_rows[k].size())).c_str(), u.decoded ? "frames" : "far_scan");
        if (!u.far_hits.empty()) {
            std::printf(", \"far_hits\": {");
            bool first = true;
            for (auto [row, n] : u.far_hits)
                std::printf("%s\"%d\": %llu", first ? "" : ", ", row, static_cast<unsigned long long>(n)), first = false;
            std::printf("}");
        }
        std::printf("}");
    }
    std::printf("],\n   \"candidates\": [");
    for (size_t i = 0; i < r.verdicts.size(); ++i) {
        const auto& v = r.verdicts[i];
        std::printf("%s{\"pblock\": %s, \"xdc\": %s, \"pass\": %s, \"share_distance\": %.4f, \"rows_outside\": %d, \"reasons\": [",
                    i ? ", " : "", json_str(v.c->pb->name).c_str(), json_str(v.c->pb->source).c_str(),
                    v.slr_ok && v.rows_ok ? "true" : "false", v.share_dist, v.extra_rows);
        for (size_t j = 0; j < v.why.size(); ++j) std::printf("%s%s", j ? ", " : "", json_str(v.why[j]).c_str());
        std::printf("]}");
    }
    std::printf("],\n   \"status\": \"%s\", \"pblock\": %s, \"name_hint\": %s}%s\n", r.status.c_str(),
                r.chosen ? json_str(r.chosen->pb->name).c_str() : "null",
                r.hint.empty() ? "null" : json_str(r.hint).c_str(), last ? "" : ",");
}

std::vector<Candidate> candidates(const std::vector<std::string>& xdcs, const std::vector<std::string>& rps,
                                  const Device& dev, std::vector<Pblock>& store) {
    for (const auto& x : xdcs)
        for (auto& p : load_xdc(x)) store.push_back(p);
    std::vector<Candidate> cs;
    for (const auto& p : store) {
        bool want = rps.empty() ? p.snapping : std::find(rps.begin(), rps.end(), p.name) != rps.end();
        if (!want) continue;
        Candidate c{&p, footprint(p, dev)};
        for (const auto& t : c.fp.unknown_types)
            std::fprintf(stderr, "pdi_where: warning: %s: %s: site type %s not in device table, ignored\n",
                         p.source.c_str(), p.name.c_str(), t.c_str());
        if (c.fp.has_remove)
            std::fprintf(stderr, "pdi_where: warning: %s: %s: -remove ranges ignored\n",
                         p.source.c_str(), p.name.c_str());
        cs.push_back(std::move(c));
    }
    if (cs.empty())
        throw PdiError(rps.empty() ? "no pblock with SNAPPING_MODE in the XDC (use --rp)"
                                   : "--rp: no such pblock in the XDC");
    return cs;
}

// ---------------------------------------------------------------------------------------------
// --selftest: synthetic device, XDC and PDIs in the format of the real ones
// ---------------------------------------------------------------------------------------------
void put(std::vector<uint8_t>& b, uint32_t w) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(w >> (8 * i)));
}
void put_at(std::vector<uint8_t>& b, size_t off, uint32_t w) {
    for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(w >> (8 * i));
}
void quad(std::vector<uint8_t>& b, uint32_t a, uint32_t x = 0, uint32_t y = 0, uint32_t z = 0) {
    put(b, a), put(b, x), put(b, y), put(b, z);
}
void cmd(std::vector<uint8_t>& out, int mod, int id, const std::vector<uint8_t>& payload) {
    uint32_t n = uint32_t(payload.size() / 4);
    if (n >= 0xFF) put(out, 0xFFu << 16 | mod << 8 | id), put(out, n);
    else put(out, n << 16 | mod << 8 | id);
    out.insert(out.end(), payload.begin(), payload.end());
}
void keyhole(std::vector<uint8_t>& out, uint64_t dst, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> p;
    put(p, uint32_t(dst >> 32)), put(p, uint32_t(dst)), put(p, 0x10000);
    p.insert(p.end(), data.begin(), data.end());
    cmd(out, 1, 0x0D, p);
}
// uncompressed CFU chunk: open, FAR/FDRI/frame/CRC per frame, close
std::vector<uint8_t> cfu_frames(const std::vector<std::pair<int, int>>& frames) {
    std::vector<uint8_t> b;
    quad(b, 0x520000D6, 0xC0FEBABE, 0xC0FEBABE, 0xC0FEBABE);
    quad(b, 0xA11F0689, 2);
    for (auto [row, far] : frames) {
        quad(b, 0xA1000100u | row << 16, far);
        quad(b, 0x52000400u | row << 16, 25);
        for (int i = 0; i < 100; ++i) put(b, far);
        quad(b, 0xA1000000u | row << 16, 0x1234);
    }
    quad(b, 0x52000047, 0xDEAFBEAD, 0xDEAFBEAD, 0xDEAFBEAD);
    return b;
}
// opaque chunk standing in for a compressed stream: valid open, then words that are no packet
std::vector<uint8_t> cfu_opaque(size_t bytes) {
    std::vector<uint8_t> b;
    quad(b, 0x520000D6, 0xC0FEBABE, 0xC0FEBABE, 0xC0FEBABE);
    uint32_t x = 0x9E3779B9;
    while (b.size() < bytes) x = x * 1664525u + 1013904223u, put(b, 0x07000000u | (x & 0xFFFFFF));
    return b;
}
// compressed-like chunk with byte-shifted FAR headers: hits[row] records of 7 bytes each
std::vector<uint8_t> cfu_scrambled(const std::map<int, int>& hits) {
    std::vector<uint8_t> b;
    quad(b, 0x520000D6, 0xC0FEBABE, 0xC0FEBABE, 0xC0FEBABE);
    for (auto [row, n] : hits)
        for (int i = 0; i < n; ++i) {
            const uint8_t rec[7] = {0x07, 0x05, uint8_t(i), 0x01, uint8_t(row), 0xA1, 0x0a};
            b.insert(b.end(), rec, rec + 7);
        }
    while (b.size() < 70000) b.push_back(0x07);
    return b;
}
std::vector<uint8_t> section(const std::vector<uint8_t>& cmds) {
    std::vector<uint8_t> s;
    put(s, 4), put(s, 0x004F4443), put(s, 0x200), put(s, uint32_t(cmds.size() / 4)), put(s, 0);
    s.insert(s.end(), cmds.begin(), cmds.end());
    return s;
}
// per SLR: CFU chunk payload (empty vector -> just the empty open/close chunk) and NPI writes.
// extra[k], when non-empty, is forwarded as a second CFU keyhole in that SLR, so an SLR can carry
// one decodable and one undecodable chunk at once.
std::vector<uint8_t> synth_pdi(std::vector<uint8_t> c0, std::vector<uint8_t> c1, std::vector<uint8_t> c2,
                               std::array<int, kSlrs> npi, std::array<std::vector<uint8_t>, kSlrs> extra = {}) {
    const std::vector<uint8_t>* cfu[kSlrs] = {&c0, &c1, &c2};
    auto body = [&](int k) {
        std::vector<uint8_t> c;
        keyhole(c, 0xF1F80000, cfu[k]->empty() ? cfu_frames({}) : *cfu[k]);
        if (!extra[k].empty()) keyhole(c, 0xF1F80000, extra[k]);
        for (int i = 0; i < npi[k]; ++i) {
            std::vector<uint8_t> p;
            put(p, 0), put(p, 0xF6230100 + 0x10000 * i);
            for (int j = 0; j < 16; ++j) put(p, 0);
            cmd(c, 1, 0x05, p);
        }
        cmd(c, 1, 0x11, {});
        return c;
    };
    std::vector<uint8_t> master = body(0);
    const uint64_t slave[kSlrs] = {0, 0x10A100000ULL, 0x112100000ULL};
    for (int k = 1; k < kSlrs; ++k) {
        std::vector<uint8_t> stream = section(body(k));
        auto more = section({0x11, 0x01, 0x00, 0x00});  // a second section, one NOP (0x00000111)
        stream.insert(stream.end(), more.begin(), more.end());
        size_t half = stream.size() / 2 & ~size_t(3);
        keyhole(master, slave[k], {stream.begin(), stream.begin() + half});
        keyhole(master, slave[k], {stream.begin() + half, stream.end()});
    }
    std::vector<uint8_t> b(0x150, 0);
    const uint8_t smap[16] = {0xdd, 0, 0, 0, 0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55, 0xcc, 0xbb, 0xaa, 0x99};
    std::copy(smap, smap + 16, b.begin());
    put_at(b, 0x10 + 0x04, 1), put_at(b, 0x10 + 0x08, 0x90 / 4), put_at(b, 0x10 + 0x0C, 1);
    put_at(b, 0x10 + 0x10, 0xD0 / 4), put_at(b, 0x10 + 0x18, 0x14d2f093);
    std::copy_n("IDPP", 4, b.begin() + 0x10 + 0x28);
    std::copy_n("selftest", 8, b.begin() + 0x90 + 0x10);
    put_at(b, 0x90 + 0x20, 0x18700002);
    std::vector<uint8_t> part;
    put(part, 4), put(part, 0x004F4443), put(part, 0x200), put(part, uint32_t(master.size() / 4)), put(part, 0);
    part.insert(part.end(), master.begin(), master.end());
    put_at(b, 0xD0 + 0x08, uint32_t(part.size() / 4)), put_at(b, 0xD0 + 0x20, 0x150 / 4);
    b.insert(b.end(), part.begin(), part.end());
    return b;
}

int selftest() {
    int fails = 0, checks = 0;
    auto check = [&](const char* what, bool ok) {
        std::printf("%s %d - %s\n", ok ? "ok" : "not ok", ++checks, what);
        fails += !ok;
    };
    // device: 3 SLRs, two fabric rows each (CR rows 1-2, 3-4, 5-6), 100 SLICE rows per CR row,
    // one NOC column, and an MMCM in SLR0 row 0 (no fabric there)
    std::string dt = "# pdi_where device table: part xtest, Vivado 0\n";
    for (int cr = 1; cr <= 6; ++cr) {
        int slr = (cr - 1) / 2, y0 = (cr - 1) * 100;
        char line[200];
        std::snprintf(line, sizeof line, "SLICE\t%d\tX0Y%d\t%d\t0\t99\t%d\t%d\t%d\n", slr, cr, cr, y0, y0 + 99, 100 * 100);
        dt += line;
        std::snprintf(line, sizeof line, "NOC_NMU512\t%d\tX0Y%d\t%d\t0\t0\t%d\t%d\t1\n", slr, cr, cr, cr, cr);
        dt += line;
    }
    dt += "MMCM\t0\tX0Y0\t0\t0\t0\t0\t0\t1\n";
    Device dev = parse_device(dt);
    g_dev = &dev;
    check("device: part name read", dev.part == "xtest");
    check("device: fabric rows per SLR", dev.fabric_rows[0] == std::vector<int>{1, 2} &&
                                         dev.fabric_rows[2] == std::vector<int>{5, 6});
    check("device: CFU row = index among the SLR's fabric rows", dev.cfu_row(1, 4) == 1 && dev.cfu_row(1, 1) == -1);

    // service: SLR0 + SLR1 (rows 1-4, i.e. y 0..399) plus an MMCM; user: SLR1 top row + SLR2;
    // decoupler: static, no SNAPPING_MODE; region "half": SLR1 + SLR2 again but larger in SLR1
    const char* xdc =
        "# comment\n"
        "create_pblock pblock_service\n"
        "add_cells_to_pblock [get_pblocks pblock_service] [get_cells -quiet [list top_i/service_layer]]\n"
        "resize_pblock [get_pblocks pblock_service] -add {SLICE_X0Y0:SLICE_X99Y399 \\\n"
        "    MMCM_X0Y0:MMCM_X0Y0}\n"
        "set_property SNAPPING_MODE ON [get_pblocks pblock_service]\n"
        "create_pblock pblock_user\n"
        "add_cells_to_pblock [get_pblocks pblock_user] [get_cells -quiet [list top_i/slash]]\n"
        "resize_pblock [get_pblocks -quiet pblock_user] -add {SLICE_X0Y300:SLICE_X99Y599}\n"
        "resize_pblock pblock_user -add NOC_NMU512_X0Y5:NOC_NMU512_X0Y6\n"
        "set_property SNAPPING_MODE ON [get_pblocks pblock_user]\n"
        "create_pblock pblock_wide\n"
        "resize_pblock [get_pblocks pblock_wide] -add {SLICE_X0Y200:SLICE_X99Y599}\n"
        "set_property SNAPPING_MODE ON [get_pblocks pblock_wide]\n"
        "create_pblock pblock_dfx_decoupler\n"
        "resize_pblock [get_pblocks pblock_dfx_decoupler] -add {SLICE_X0Y590:SLICE_X99Y599}\n";
    std::vector<Pblock> store = parse_xdc(xdc, "test.xdc");
    check("xdc: four pblocks", store.size() == 4);
    check("xdc: continuation line joined", store[0].ranges.size() == 2);
    check("xdc: get_pblocks -quiet and bare names", store[1].ranges.size() == 2);
    check("xdc: cells", store[0].cells == std::vector<std::string>{"top_i/service_layer"});
    check("xdc: snapping marks reconfigurable", store[0].snapping && !store[3].snapping);
    std::vector<Candidate> cs;
    for (const auto& p : store)
        if (p.snapping) cs.push_back({&p, footprint(p, dev)});
    check("footprint: service covers SLR0+SLR1", slr_list(cs[0].fp.slr) == "SLR0+SLR1");
    check("footprint: user covers SLR1+SLR2", slr_list(cs[1].fp.slr) == "SLR1+SLR2");
    check("footprint: user SLR1 CFU rows {1}", cs[1].fp.cfu_rows[1] == std::set<int>{1});
    check("footprint: SLICE estimate", std::fabs(cs[0].fp.slice_sites[0] - 20000) < 1e-6 &&
                                       std::fabs(cs[1].fp.slice_sites[1] - 10000) < 1e-6);
    try {
        parse_xdc("resize_pblock [get_pblocks nope] -add {SLICE_X0Y0:SLICE_X1Y1}\n", "bad.xdc");
        check("xdc: pblock used before create_pblock is an error", false);
    } catch (const PdiError&) {
        check("xdc: pblock used before create_pblock is an error", true);
    }

    auto frames = [](int row, int n) {
        std::vector<std::pair<int, int>> f;
        for (int i = 0; i < n; ++i) f.push_back({row, i});
        return f;
    };
    auto big = [&](int row) {  // > 64 KiB of uncompressed frames in one CFU row
        auto f = frames(row, 200);
        return cfu_frames(f);
    };
    // 1. uncompressed service partial: SLR0 rows 0-1, SLR1 rows 0-1
    {
        auto a = big(0), b = big(1);
        a.insert(a.end() - 16, b.begin() + 32, b.end() - 16);  // rows 0 and 1 in one chunk
        PdiInfo p = analyse("svc.pdi", synth_pdi(a, a, {}, {2, 3, 0}));
        check("pdi: SLR footprint read", p.slr[0].configured() && p.slr[1].configured() && !p.slr[2].configured());
        check("pdi: uncompressed decoded with rows", !p.compressed() && p.slr[0].cfu_rows == std::set<int>{0, 1});
        check("pdi: NPI writes counted", p.slr[0].npi_writes == 2 && p.slr[1].npi_writes == 3);
        Result r = classify(p, cs);
        check("classify: uncompressed service -> pblock_service", r.status == "match" && r.chosen->pb->name == "pblock_service");
    }
    // 2. compressed user partial (SLR1 small, SLR2 large): passes user and wide; area share picks user
    {
        PdiInfo p = analyse("top_i_slash_slash_x_inst_0_partial.pdi", synth_pdi({}, cfu_opaque(80000), cfu_opaque(240000), {0, 1, 4}));
        check("pdi: opaque stream marked compressed", p.compressed());
        Result r = classify(p, cs);
        check("classify: compressed user -> ambiguous, nearest pblock_user",
              r.status == "ambiguous" && r.chosen->pb->name == "pblock_user");
        check("classify: file-name hint from add_cells_to_pblock", r.hint == "pblock_user");
    }
    auto rows01 = [&]() {
        auto a = big(0), b = big(1);
        a.insert(a.end() - 16, b.begin() + 32, b.end() - 16);
        return a;
    };
    // 3. uncompressed, SLR1 rows 0-1 and SLR2 rows 0-1: pblock_wide fits exactly; pblock_user
    //    (SLR1 row 1 only) fits loosely, with SLR1 row 0 outside it -> match pblock_wide
    {
        PdiInfo p = analyse("wide.pdi", synth_pdi({}, rows01(), rows01(), {0, 1, 1}));
        Result r = classify(p, cs);
        check("classify: rows separate same-SLR pblocks", r.status == "match" && r.chosen->pb->name == "pblock_wide");
        check("classify: row outside the pblock counted", r.verdicts[1].rows_ok && r.verdicts[1].extra_rows == 1);
    }
    // 3b. uncompressed, SLR1 row 0 only: pblock_user misses its fabric row 1 (hard fail); pblock_wide
    //     misses row 1 too -> none
    {
        PdiInfo p = analyse("short.pdi", synth_pdi({}, big(0), rows01(), {0, 1, 1}));
        Result r = classify(p, cs);
        check("classify: unwritten pblock row rejects", r.status == "none");
    }
    // 3c. only pblock_user remains, PDI also writes SLR1 row 0 -> still the one hard fit: match;
    //     two such candidates, both with a row outside -> probable
    {
        std::vector<Candidate> only{cs[1]}, two{cs[1], cs[1]};
        PdiInfo p = analyse("loose.pdi", synth_pdi({}, rows01(), rows01(), {0, 1, 1}));
        Result r = classify(p, only);
        check("classify: sole hard fit with a row outside -> match", r.status == "match" && r.chosen->pb->name == "pblock_user");
        Result r2 = classify(p, two);
        check("classify: several loose fits -> probable", r2.status == "probable");
    }
    // 3d. compressed, rows from FAR-header scan: SLR1 rows 0-1, SLR2 rows 0-1, plus stray hits
    //     (row 1 below threshold in SLR2's twin test, row 200 out of range) -> pblock_wide
    {
        auto s1 = cfu_scrambled({{0, 3000}, {1, 3000}, {200, 900}});
        auto s2 = cfu_scrambled({{0, 3000}, {1, 3000}});
        PdiInfo p = analyse("scan.pdi", synth_pdi({}, s1, s2, {0, 1, 1}));
        check("scan: compressed stream not decoded", p.compressed() && !p.slr[1].decoded);
        check("scan: rows from FAR hits, out-of-range row dropped",
              rows_written(p.slr[1], dev.fabric_rows[1].size()) == std::set<int>({0, 1}));
        Result r = classify(p, cs);
        check("scan: rows separate same-SLR pblocks when compressed",
              r.status == "match" && r.chosen->pb->name == "pblock_wide");
        auto t1 = cfu_scrambled({{0, 40}, {1, 3000}});  // 40 hits: below max(500, 2 %)
        PdiInfo q = analyse("scan2.pdi", synth_pdi({}, t1, s2, {0, 1, 1}));
        check("scan: stray hits below the threshold ignored",
              rows_written(q.slr[1], dev.fabric_rows[1].size()) == std::set<int>({1}));
        Result rq = classify(q, cs);
        check("scan: compressed user-shaped PDI -> pblock_user", rq.status == "match" && rq.chosen->pb->name == "pblock_user");
    }
    // 3e. mixed SLR: one decodable frame chunk (rows 0-1) and one undecodable chunk in the same SLR.
    //     The decodable chunk's rows must survive; one opaque chunk must not blank the SLR.
    {
        std::array<std::vector<uint8_t>, kSlrs> extra;
        extra[1] = cfu_opaque(4000);  // second CFU keyhole in SLR1, not the frame format
        PdiInfo p = analyse("mixed.pdi", synth_pdi({}, rows01(), rows01(), {0, 1, 1}, extra));
        check("mixed: SLR with an undecodable chunk is not marked fully decoded", !p.slr[1].decoded);
        check("mixed: decoded rows survive the undecodable chunk",
              rows_written(p.slr[1], dev.fabric_rows[1].size()) == std::set<int>({0, 1}));
        Result r = classify(p, cs);
        check("mixed: placement uses the decoded rows -> pblock_wide",
              r.status == "match" && r.chosen->pb->name == "pblock_wide");
    }
    // 4. a partial that configures only SLR2: matches nothing (every candidate has fabric elsewhere)
    {
        PdiInfo p = analyse("odd.pdi", synth_pdi({}, {}, cfu_opaque(100000), {0, 0, 1}));
        Result r = classify(p, cs);
        check("classify: inconsistent PDI -> none", r.status == "none" && r.chosen == nullptr);
    }
    // 5. not a PDI
    try {
        analyse("junk", std::vector<uint8_t>(4096, 0x5A));
        check("pdi: junk rejected", false);
    } catch (const PdiError&) {
        check("pdi: junk rejected", true);
    }
    // 6. tar reader: one member, ustar, read back
    {
        std::vector<uint8_t> t(512, 0);
        std::string n = "images/top_i_slash_slash_x_inst_0_partial.pdi";
        std::copy(n.begin(), n.end(), t.begin());
        std::snprintf(reinterpret_cast<char*>(t.data() + 124), 12, "%011o", 5);
        t[156] = '0';
        std::copy_n("ustar", 5, t.begin() + 257);
        for (char ch : std::string("hello")) t.push_back(uint8_t(ch));
        t.resize(512 * 2 + 1024, 0);
        auto m = untar(t);
        check("tar: member name and data", m.size() == 1 && m[0].first == n && m[0].second.size() == 5);
    }
    std::printf("1..%d\n", checks);
    if (fails) std::printf("# %d of %d failed\n", fails, checks);
    return fails ? 1 : 0;
}

// --device, else $PDI_WHERE_DEVICE, else the first xcv80.tsv found: next to the executable (build
// tree), in the source tree above it, in the install tree relative to it, at the configured prefix.
std::string default_device() {
    if (const char* e = std::getenv("PDI_WHERE_DEVICE")) return e;
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string dir = ".";
    if (n > 0) {
        std::string exe(buf, size_t(n));
        dir = exe.substr(0, exe.find_last_of('/'));
    }
    const std::string tries[] = {dir + "/device/xcv80.tsv", dir + "/../device/xcv80.tsv",
                                 dir + "/" PDI_WHERE_DATADIR_REL "/xcv80.tsv", PDI_WHERE_DATADIR "/xcv80.tsv"};
    for (const auto& t : tries)
        if (access(t.c_str(), R_OK) == 0) return t;
    return tries[0];
}

const char kUsage[] =
    "usage: pdi_where [-v] [--json] --xdc FILE [--xdc FILE ...] [--device TSV] [--rp PBLOCK ...] PDI|VBIN ...\n"
    "       pdi_where --selftest\n"
    "       pdi_where --version\n"
    "\n"
    "  --xdc FILE    pblock constraints; candidates are pblocks with SNAPPING_MODE set\n"
    "  --rp PBLOCK   use these pblocks as candidates instead\n"
    "  --device TSV  device table (default: device/xcv80.tsv next to the binary)\n"
    "  -v            show SLRs, rows and every candidate\n"
    "  --json        machine-readable output\n";

int usage(bool ok = false) {
    std::fputs(kUsage, ok ? stdout : stderr);
    return ok ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> xdcs, rps, inputs;
    std::string device = default_device();
    bool json = false, verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--selftest") {
            try {
                return selftest();
            } catch (const std::exception& e) {
                std::printf("Bail out! %s\n", e.what());
                return 1;
            }
        }
        if (a == "--version") {
            std::printf("pdi_where %s%s%s\n", PDI_WHERE_VERSION, *PDI_WHERE_GIT ? " " : "", PDI_WHERE_GIT);
            return 0;
        }
        if (a == "--json") json = true;
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if ((a == "--xdc" || a == "--device" || a == "--rp") && i + 1 < argc)
            (a == "--xdc" ? xdcs.push_back(argv[++i]) : a == "--rp" ? rps.push_back(argv[++i]) : void(device = argv[++i]));
        else if (a == "-h" || a == "--help") return usage(true);
        else if (!a.empty() && a[0] == '-') return usage();
        else inputs.push_back(a);
    }
    if (xdcs.empty() || inputs.empty()) return usage();
    try {
        Device dev = load_device(device);
        g_dev = &dev;
        std::vector<Pblock> store;
        store.reserve(64);
        std::vector<Candidate> cs = candidates(xdcs, rps, dev, store);
        std::vector<std::pair<PdiInfo, Result>> res;
        for (const auto& in : inputs)
            for (auto& [name, data] : load(in)) {
                PdiInfo p = analyse(name, data);
                Result r = classify(p, cs);
                res.emplace_back(std::move(p), std::move(r));
            }
        bool all = true;
        if (json) std::printf("[\n");
        for (size_t i = 0; i < res.size(); ++i) {
            if (json) print_json(res[i].first, res[i].second, i + 1 == res.size());
            else if (verbose) print_long(res[i].first, res[i].second);
            else print_short(res[i].first, res[i].second);
            if (verbose && !json && i + 1 < res.size()) std::putchar('\n');
            all = all && res[i].second.status == "match";
        }
        if (json) std::printf("]\n");
        return all ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pdi_where: %s\n", e.what());
        return 2;
    }
}
