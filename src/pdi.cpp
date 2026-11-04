// ##################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2026 ARH Laboratory
//  SPDX-License-Identifier: MIT
// ##################################################################################################
//
// Layout of a partial PDI (header fields checked against bootgen -arch versal -read):
//   0x000  four SMAP words
//   0x010  Image Header Table: n_images @+0x04, ih_offset @+0x08 (words), n_partitions @+0x0c,
//          pht_offset @+0x10 (words), id_code @+0x18, "PPDI" @+0x28
//   IH     name @+0x10 (16 B), id @+0x20
//   PH     total_length @+0x08 (words), partition_offset @+0x20 (words)
// The single partition is a CDO stream (header: 4, "CDO\0", version, length in words, checksum).
// The master SLR's commands configure SLR0 directly; keyhole writes (module 1, cmd 0x0D) to
// 0x10A100000 / 0x112100000 forward a sub-stream to SLR1 / SLR2, which is itself a sequence of CDO
// sections. A keyhole write to 0xF1F80000 is CFU (configuration frame) data for the SLR whose
// stream carries it.
#include "pdi.hpp"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <sstream>

namespace pdi_where {
namespace {

constexpr uint64_t kCfu = 0xF1F80000ULL;
constexpr uint64_t kSlaveAddr[kSlrs] = {0, 0x10A100000ULL, 0x112100000ULL};
constexpr uint32_t kCdoMagic = 0x004F4443;  // "CDO\0"
constexpr uint32_t kIht = 0x10;
constexpr int kFrameWords = 100;

// An SLR the partial does not touch still gets its fixed CFU open/close chunks: exactly 896 B in
// every compressed partial seen here (7 x 128 B) and 1,456 B in the uncompressed ones, with no NPI
// writes (40 such SLRs in 77 real partials). The smallest SLR that is really configured carries
// 1.45 MB. 64 KiB separates the two with a wide margin on both sides.
constexpr uint64_t kEmptyCfuMax = 64 * 1024;

uint32_t u32(const std::vector<uint8_t>& b, uint64_t off) {
    if (off + 4 > b.size()) {
        std::ostringstream m;
        m << "read at 0x" << std::hex << off << " past the end of " << std::dec << b.size()
          << " bytes";
        throw PdiError(m.str());
    }
    uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

struct Cmd {
    uint64_t payload;  // byte offset of the payload
    uint32_t words;    // payload length in words
    uint8_t module, id;
};

// Walk CDO commands in [start, end).
template <class F>
void walk_cmds(const std::vector<uint8_t>& b, uint64_t start, uint64_t end, F&& f) {
    uint64_t i = start;
    while (i < end) {
        uint32_t h = u32(b, i);
        uint32_t len = (h >> 16) & 0xFF;
        uint64_t hdr = 4;
        if (len == 0xFF) {
            len = u32(b, i + 4);
            hdr = 8;
        }
        Cmd c{i + hdr, len, uint8_t((h >> 8) & 0xFF), uint8_t(h & 0xFF)};
        if (c.payload + 4ULL * len > end) throw PdiError("CDO command runs past its section");
        f(c);
        i = c.payload + 4ULL * len;
    }
}

// Every CDO section in a slave stream: [cmd_start, cmd_end).
std::vector<std::pair<uint64_t, uint64_t>> sections(const std::vector<uint8_t>& b) {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    uint64_t j = 4;
    while (j + 16 <= b.size()) {
        if (u32(b, j) == kCdoMagic && u32(b, j - 4) == 4) {
            uint64_t n = u32(b, j + 8);
            uint64_t s = j + 16, e = s + 4 * n;
            if (e > b.size()) throw PdiError("slave CDO section runs past its stream");
            out.emplace_back(s, e);
            j = e + 4;
        } else {
            j += 4;
        }
    }
    return out;
}

uint64_t keyhole_dst(const std::vector<uint8_t>& b, const Cmd& c) {
    return (uint64_t(u32(b, c.payload)) << 32) | u32(b, c.payload + 4);
}

// Decode one CFU keyhole payload in the uncompressed format: a sequence of 128-bit quads whose first
// word is a packet header (bits 31:24 opcode, 23:16 CFU row, 15:8 register). 0xA1 is a one-quad
// register write (register 0x01 = FAR); 0x52 with register 0x04 (FDRI) is followed by a count of
// data quads, always 25 here (one 100-word frame); other 0x52 quads stand alone.
// Returns false, without touching `u`, if anything in it is not that format (compressed streams).
bool decode_cfu(const uint8_t* p, uint64_t n, SlrUse& u) {
    if (n % 16) return false;
    uint64_t words = n / 4, q = 0, frames = 0;
    std::set<int> rows;
    bool far_seen = false;
    auto w = [&](uint64_t k) {
        uint32_t v;
        std::memcpy(&v, p + 4 * k, 4);
        return v;
    };
    while (q < words) {
        uint32_t h = w(q);
        uint32_t op = h >> 24, row = (h >> 16) & 0xFF, reg = (h >> 8) & 0xFF;
        if (op == 0x52 && reg == 0x04) {  // FDRI: one 100-word frame
            if (q + 1 >= words) return false;
            uint32_t cnt = w(q + 1);
            if (cnt * 4 != kFrameWords || !far_seen || q + 4 + 4ULL * cnt > words) return false;
            rows.insert(int(row));
            ++frames;
            q += 4 + 4ULL * cnt;
            continue;
        }
        if (op == 0xA1) {
            if (reg == 0x01 && row != 0x1F) far_seen = true;
        } else if (op != 0x52) {
            return false;
        }
        q += 4;
    }
    u.frames += frames;
    u.cfu_rows.insert(rows.begin(), rows.end());
    return true;
}

void visit(const std::vector<uint8_t>& b, uint64_t s, uint64_t e, int slr, PdiInfo& info,
           std::vector<uint8_t>* slave_out[kSlrs], bool& all_decoded) {
    SlrUse& u = info.slr[slr];
    walk_cmds(b, s, e, [&](const Cmd& c) {
        if (c.module == 1 && c.id == 0x0D && c.words >= 3) {
            uint64_t dst = keyhole_dst(b, c);
            uint64_t data = c.payload + 12, n = 4ULL * c.words - 12;
            if (dst == kCfu) {
                u.cfu_bytes += n;
                ++u.cfu_chunks;
                if (!decode_cfu(b.data() + data, n, u)) {
                    all_decoded = false;
                    const uint8_t* q = b.data() + data;
                    for (uint64_t i = 0; i + 4 <= n; ++i)
                        if (q[i + 3] == 0xA1 && q[i + 1] == 0x01) ++u.far_hits[q[i + 2]];
                }
                return;
            }
            if (slr == 0) {
                for (int k = 1; k < kSlrs; ++k)
                    if (dst == kSlaveAddr[k]) {
                        slave_out[k]->insert(slave_out[k]->end(), b.begin() + data,
                                             b.begin() + data + n);
                        return;
                    }
            }
            // A keyhole write that is neither CFU nor an SLR-forward routed from the master. Real
            // partials never do this; it is recorded so a placement resting on this SLR can abstain,
            // since such a write could carry frames the row and SLR accounting does not see.
            ++u.stray_keyholes;
            u.stray_keyhole_dsts.insert(dst);
            return;
        }
        if (c.module == 1 && c.id == 0x05 && c.words >= 2) {
            ++u.npi_writes;
            u.npi_bases.insert(u32(b, c.payload + 4) & ~0xFFFFu);
        }
    });
}

}  // namespace

bool SlrUse::configured() const { return cfu_bytes > kEmptyCfuMax || npi_writes > 0; }

// An SLR that is not configured is still unambiguously empty when its CFU content is a clean
// open/close with no frames. That shows two ways: the chunk decodes with zero frames (the
// uncompressed empty), or it is one of the two fixed empty footprints seen in every real untouched
// SLR, 896 B compressed (7 x 128 B) and 1,456 B uncompressed, which do not decode as frames.
// Anything else below the configured ceiling, undecodable bytes or a handful of frames, sits in a
// grey zone the SLR/row accounting cannot resolve.
bool SlrUse::ambiguous_cfu() const {
    if (configured() || cfu_bytes == 0) return false;
    if (decoded && frames == 0) return false;
    return cfu_bytes != 896 && cfu_bytes != 1456;
}

bool PdiInfo::compressed() const {
    for (const auto& u : slr)
        if (u.configured() && !u.decoded) return true;
    return false;
}

PdiInfo analyse(const std::string& name, const std::vector<uint8_t>& b) {
    static const uint8_t smap[16] = {0xdd, 0, 0, 0, 0x44, 0x33, 0x22, 0x11,
                                     0x88, 0x77, 0x66, 0x55, 0xcc, 0xbb, 0xaa, 0x99};
    if (b.size() < 0x200 || std::memcmp(b.data(), smap, 16) != 0)
        throw PdiError(name + ": no SMAP words at offset 0; not a partial PDI");
    if (std::memcmp(b.data() + kIht + 0x28, "IDPP", 4) != 0)
        throw PdiError(name + ": image header table is not \"PPDI\"; full PDIs are not handled");

    PdiInfo info;
    info.name = name;
    info.bytes = b.size();
    info.id_code = u32(b, kIht + 0x18);
    uint32_t n_images = u32(b, kIht + 0x04), n_parts = u32(b, kIht + 0x0C);
    uint64_t ih = 4ULL * u32(b, kIht + 0x08), ph = 4ULL * u32(b, kIht + 0x10);
    if (n_images != 1 || n_parts != 1) {
        std::ostringstream m;
        m << name << ": " << n_images << " images / " << n_parts
          << " partitions; only the one-image, one-partition partials Vivado writes are handled";
        throw PdiError(m.str());
    }
    char nm[17] = {};
    if (ih + 0x20 > b.size()) throw PdiError(name + ": image header past the end");
    std::memcpy(nm, b.data() + ih + 0x10, 16);
    info.image_name = nm;
    info.image_id = u32(b, ih + 0x20);
    uint64_t part = 4ULL * u32(b, ph + 0x20), total = 4ULL * u32(b, ph + 0x08);
    if (part + total > b.size()) throw PdiError(name + ": partition runs past the end of the file");
    if (u32(b, part) != 4 || u32(b, part + 4) != kCdoMagic)
        throw PdiError(name + ": partition is not a CDO stream");
    uint64_t s = part + 20, e = s + 4ULL * u32(b, part + 12);
    if (e > part + total) throw PdiError(name + ": CDO length exceeds the partition");

    std::vector<uint8_t> slave[kSlrs];
    std::vector<uint8_t>* slave_out[kSlrs] = {nullptr, &slave[1], &slave[2]};
    bool dec[kSlrs] = {true, true, true};
    visit(b, s, e, 0, info, slave_out, dec[0]);
    for (int k = 1; k < kSlrs; ++k)
        for (auto [cs, ce] : sections(slave[k])) visit(slave[k], cs, ce, k, info, nullptr, dec[k]);
    for (int k = 0; k < kSlrs; ++k) info.slr[k].decoded = dec[k] && info.slr[k].cfu_chunks > 0;
    return info;
}

std::vector<std::pair<std::string, std::vector<uint8_t>>> untar(const std::vector<uint8_t>& t) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    std::string longname;
    uint64_t off = 0;
    while (off + 512 <= t.size()) {
        const uint8_t* h = t.data() + off;
        bool zero = true;
        for (int i = 0; i < 512 && zero; ++i) zero = h[i] == 0;
        if (zero) break;
        std::string name(reinterpret_cast<const char*>(h), strnlen(reinterpret_cast<const char*>(h), 100));
        if (std::memcmp(h + 257, "ustar", 5) == 0 && h[345]) {
            std::string pre(reinterpret_cast<const char*>(h + 345), strnlen(reinterpret_cast<const char*>(h + 345), 155));
            name = pre + "/" + name;
        }
        uint64_t size = 0;
        for (int i = 124; i < 136 && h[i]; ++i)
            if (h[i] >= '0' && h[i] <= '7') size = size * 8 + (h[i] - '0');
        char type = char(h[156]);
        uint64_t data = off + 512;
        if (data + size > t.size()) throw PdiError("tar member runs past the archive");
        if (type == 'L') {
            longname.assign(reinterpret_cast<const char*>(t.data() + data), strnlen(reinterpret_cast<const char*>(t.data() + data), size));
        } else {
            if (!longname.empty()) name = longname, longname.clear();
            if (type == '0' || type == '\0')
                out.emplace_back(name, std::vector<uint8_t>(t.begin() + data, t.begin() + data + size));
        }
        off = data + (size + 511) / 512 * 512;
    }
    return out;
}

std::vector<std::pair<std::string, std::vector<uint8_t>>> load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw PdiError(path + ": cannot open");
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() >= 2 && raw[0] == 0x1f && raw[1] == 0x8b) {
        gzFile g = gzopen(path.c_str(), "rb");
        if (!g) throw PdiError(path + ": gzopen failed");
        std::vector<uint8_t> tar;
        uint8_t buf[1 << 16];
        int n;
        while ((n = gzread(g, buf, sizeof buf)) > 0) tar.insert(tar.end(), buf, buf + n);
        int err;
        const char* msg = gzerror(g, &err);
        gzclose(g);
        if (n < 0) throw PdiError(path + ": gzip: " + msg);
        raw.swap(tar);
    }
    if (raw.size() >= 262 && std::memcmp(raw.data() + 257, "ustar", 5) == 0) {
        std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
        for (auto& [n, d] : untar(raw)) {
            const std::string suf = "_partial.pdi";
            if (n.size() >= suf.size() && n.compare(n.size() - suf.size(), suf.size(), suf) == 0)
                out.emplace_back(path + ":" + n.substr(n.rfind('/') + 1), std::move(d));
        }
        if (out.empty()) throw PdiError(path + ": archive holds no *_partial.pdi");
        return out;
    }
    return {{path, std::move(raw)}};
}

}  // namespace pdi_where
