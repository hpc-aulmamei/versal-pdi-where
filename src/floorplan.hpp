// ##################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2026 ARH Laboratory
//  SPDX-License-Identifier: MIT
// ##################################################################################################
//
// Turn pblocks (from an XDC) into the SLRs and clock-region rows they cover, using a device table
// written once per part by gen_device_table.tcl. No Vivado at run time.
#pragma once

#include "pdi.hpp"

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pdi_where {

// One line of the device table: the sites of one type inside one clock region (or one site that
// lies in no clock region, cr_row == -1).
struct DeviceCell {
    std::string type;
    int slr, cr_x, cr_row;
    int xmin, xmax, ymin, ymax, sites;
};

struct Device {
    std::string part;
    std::vector<DeviceCell> cells;
    // per SLR, its fabric clock-region rows (rows with SLICE sites), ascending; the CFU row number
    // in an uncompressed PDI is the index into this list
    std::array<std::vector<int>, kSlrs> fabric_rows;
    int cfu_row(int slr, int cr_row) const;  // -1 if cr_row is not a fabric row of slr
};

Device load_device(const std::string& path);
Device parse_device(const std::string& text);

struct Range {
    std::string type;  // site type, or "CLOCKREGION"
    int x0, y0, x1, y1;
    bool remove = false;
};

struct Pblock {
    std::string name, source;      // source: the XDC it came from
    std::vector<Range> ranges;
    std::vector<std::string> cells;
    bool snapping = false;         // SNAPPING_MODE set: Vivado only snaps reconfigurable pblocks
    bool contain_routing = false;
};

std::vector<Pblock> load_xdc(const std::string& path);
std::vector<Pblock> parse_xdc(const std::string& text, const std::string& source);

struct Footprint {
    std::array<bool, kSlrs> slr{};
    std::array<std::set<int>, kSlrs> cfu_rows;   // fabric rows reached by any site type, as CFU rows
    std::array<std::set<int>, kSlrs> fabric_rows;  // fabric rows with pblock SLICE sites, as CFU rows
    std::array<double, kSlrs> slice_sites{};    // SLICE sites covered (bounding-box estimate)
    std::array<std::set<std::string>, kSlrs> types;  // site types that put the pblock in this SLR
    std::vector<std::string> unknown_types;     // range types the device table does not know
    bool has_remove = false;                    // -remove ranges were ignored
};

Footprint footprint(const Pblock& pb, const Device& dev);

}  // namespace pdi_where
