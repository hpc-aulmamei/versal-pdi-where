// ##################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2026 ARH Laboratory
//  SPDX-License-Identifier: MIT
// ##################################################################################################
//
// Read a Versal SSIT partial PDI (raw .pdi, or the _partial.pdi members of a gzip'd .vbin tar) and
// say, per SLR, what it configures. The format knowledge is inferred from partial PDIs written by
// Vivado 2025.1 for the xcv80 (V80), not taken from AMD documentation.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace pdi_where {

constexpr int kSlrs = 3;

struct PdiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct SlrUse {
    uint64_t cfu_bytes = 0;      // keyhole payload written to this SLR's CFU stream (0xF1F80000)
    int cfu_chunks = 0;          // number of such keyhole writes
    int npi_writes = 0;          // DMA writes (module 1, cmd 0x05): NoC / hard-block NPI config
    std::set<uint32_t> npi_bases;  // NPI target addresses & ~0xFFFF
    bool decoded = false;        // every CFU chunk parsed as the uncompressed frame format
    uint64_t frames = 0;         // frames written (decoded streams only)
    std::set<int> cfu_rows;      // CFU rows written (decoded streams only)
    // Compressed streams: the FAR packet header (0xA1 | row | 0x01 | check, little-endian) survives
    // compression intact but not quad-aligned; hits of that 4-byte pattern at any byte offset, per
    // row byte. Real rows get thousands of hits; see rows_written() in main.cpp for the filter.
    std::map<int, uint64_t> far_hits;
    int stray_keyholes = 0;        // keyhole writes to neither CFU nor an SLR-forward (never in real
                                   // partials); a frame-capable write the accounting does not follow
    std::set<uint64_t> stray_keyhole_dsts;
    bool configured() const;       // see kEmptyCfuMax in pdi.cpp
    // CFU content that is neither clearly empty nor clearly a configured SLR: 0 < bytes <= the empty
    // ceiling, or an undecodable chunk small enough that the SLR still reads as empty. Placement
    // cannot tell whether such an SLR is configured, so a verifier abstains on it.
    bool ambiguous_cfu() const;
};

struct PdiInfo {
    std::string name;            // file, or file:member for a vbin
    uint64_t bytes = 0;
    uint32_t id_code = 0;        // device IDCODE from the image header table
    uint32_t image_id = 0;
    std::string image_name;
    std::array<SlrUse, kSlrs> slr;
    bool compressed() const;     // true when some configured SLR's CFU stream did not decode
};

// One PDI image in memory.
PdiInfo analyse(const std::string& name, const std::vector<uint8_t>& pdi);

// Every partial PDI in a path: one for a .pdi, each *_partial.pdi member for a .vbin.
std::vector<std::pair<std::string, std::vector<uint8_t>>> load(const std::string& path);

// Minimal ustar reader over an in-memory (already gunzipped) tar.
std::vector<std::pair<std::string, std::vector<uint8_t>>> untar(const std::vector<uint8_t>& tar);

}  // namespace pdi_where
