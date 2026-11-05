# versal-pdi-where

`pdi_where` reads a Versal SSIT partial PDI and the pblock XDC of the shell, and says which
reconfigurable pblock the PDI configures. In the [SLASH](https://github.com/Xilinx/SLASH) service
shell, that tells a service-layer partial (`pblock_service_layer`) from a user-region partial
(`pblock_slash`). It needs no Vivado at run time and reads both compressed and uncompressed PDIs.
When a pblock changes, pass the new XDC: the footprint is recomputed from it on every run.

## Build

Needs CMake 3.21 or later, a C++17 compiler and zlib.

```
cmake --preset default && cmake --build --preset default    # build/pdi_where
ctest --preset default                                      # selftest and CLI checks
cmake --install build --prefix ~/.local                     # bin/, share/versal-pdi-where/
```

The presets are `default` (RelWithDebInfo, in `build/`), `release`, and `debug` (ASan, UBSan and
`-Werror`, in `build/debug/`). The plain `cmake -S . -B build` route works too. Options:

| Option | Default | |
|---|---|---|
| `PDI_WHERE_LTO` | ON | link-time optimisation in Release and RelWithDebInfo |
| `PDI_WHERE_SANITIZE` | OFF | AddressSanitizer and UndefinedBehaviorSanitizer |
| `PDI_WHERE_WERROR` | OFF | warnings are errors |
| `PDI_WHERE_BUILDS` | `$BUILDS` | directory of SLASH builds; adds the `real` test, which runs `tests/test_real.sh` |

`--version` prints the project version and the `git describe` of the tree it was built from.
`cpack` in the build directory makes a `.tar.gz` of the install tree.

## Usage

```
pdi_where --xdc impl.xdc  top_i_service_layer_..._partial.pdi  app.vbin
pdi_where --xdc a.xdc --xdc b.xdc ...    # candidates from several shells or floorplans at once
pdi_where -v ...                         # SLRs, rows and every candidate
pdi_where --json ...                     # machine-readable
pdi_where --rp pblock_slash ...          # name the candidate pblocks instead of guessing them
pdi_where --selftest                     # synthetic device, XDC and PDIs, TAP output
```

One line per partial, `file: pblock`, with `(probable)` or `(ambiguous)` after the name when the
result is weaker than a match, or `no match`. With `-v`, on a SLASH service-shell build:

```
$ pdi_where -v --xdc tests/fixtures/slash-service.xdc top_i_service_layer_service_layer_inst_0_partial.pdi
top_i_service_layer_service_layer_inst_0_partial.pdi: pblock_service_layer
  13738624 bytes, idcode 0x14d2f093, image 0x18700002 (CONFIG_MASTER), compressed

  SLR   CFU bytes   NPI  rows
  0       4694576    70  0,2,3    far hits 0:7954 1:14 2:59078 3:64231
  1       8745552    92  0,1,2    far hits 0:57416 1:65688 2:49731
  2           896     0  -        not configured

  pblock                xdc                SLRs   dist
  pblock_slash          slash-service.xdc  1,2    1.79  rejected
    SLR0 written, not in pblock
    SLR0 rows 0,2,3 written outside pblock
    SLR1 rows 0,1 written outside pblock (pblock has 2)
    SLR2 in pblock, not written
  pblock_service_layer  slash-service.xdc  0,1    0.01  fits, 1 row outside
    SLR0 row 0 written outside pblock (pblock has 2,3)

  member name points to pblock_service_layer
```

`dist` is the area-share distance used to break ties (see below).

## Inputs

- **Partials:** a `.pdi`, or a `.vbin` (gzip'd tar), from which every `*_partial.pdi` member is
  read. Only the one-image, one-partition partials Vivado writes are handled, not full PDIs.
- **One or more XDCs.** Candidates are the pblocks with `SNAPPING_MODE` set, which Vivado applies
  only to reconfigurable pblocks; static pblocks such as a DFX decoupler's are left out. `--rp`
  names the candidates explicitly instead.
  - Commands read: `create_pblock`, `resize_pblock … -add/-remove {ranges}`,
    `add_cells_to_pblock` and `set_property … [get_pblocks …]`.
  - Syntax handled: `[get_pblocks -quiet X]`, bare names, braces and backslash continuations.
  - `-remove` ranges are ignored, with a warning.
- **The device table** `device/xcv80.tsv`, written once per part by
  `scripts/gen_device_table.tcl`. That script opens the part only, with no design and no build:

  ```
  vivado -mode batch -source scripts/gen_device_table.tcl -tclargs xcv80-lsva4737-2MHP-e-S device/xcv80.tsv
  ```

  For each site type, the table gives the X/Y range and the SLR within every clock region. Sites
  that lie in no clock region would get one line per type and SLR; on the xcv80 there are none,
  all 645,774 sites lie in a clock region. Generating the table takes about 2.5 minutes. The tool
  looks for it at `--device`, then `$PDI_WHERE_DEVICE`, then next to the binary
  (`device/xcv80.tsv`, as in the build tree), then in `share/versal-pdi-where/` of the install
  tree.
- **The frame table** `device/xcv80.frames.tsv`, optional, read from beside the device table.
  For every SLICE column (the two sites of one CLE tile) of every CFU row, it gives the frame that
  holds that column's flip-flop bits: 1,786 columns on the xcv80. `scripts/gen_frame_table.tcl`
  writes it in about 8 minutes, again with no build. Run it from a scratch directory, since
  Vivado leaves a few files in the current one:

  ```
  vivado -mode batch -source scripts/gen_frame_table.tcl -tclargs xcv80-lsva4737-2MHP-e-S device/xcv80.frames.tsv
  ```

  The script places one flip-flop on AFF and one on HFF2 of a SLICE at every X in every clock
  region, in a bare netlist, and reads the frame addresses back from Vivado's logic location file.
  The netlist has no CIPS, so the CIPS-2 DRC is lowered to a warning, and its device image must
  never be loaded. Without the table, the tool works as before, per SLR and row only.

Exit status: 0 if every PDI is a `match`, 1 if any is `ambiguous`, `probable`, `unverified` or
`none`, 2 on error.

## How it decides

The master SLR's CDO stream configures SLR0. Keyhole writes (module 1, command 0x0D) to
0x10A100000 and 0x112100000 forward the sub-streams of SLR1 and SLR2, each a sequence of CDO
sections. A keyhole write to 0xF1F80000 is CFU (configuration-frame) data for the SLR whose stream
carries it.

1. **SLRs (hard).** A partial writes frames in every SLR where its pblock has fabric, and in no
   other. An SLR it does not touch still receives its fixed CFU chunks: exactly 896 B compressed
   (7 × 128 B) or 1,456 B uncompressed, and no NPI writes. This held for all 40 such SLRs in 77
   real partials; the smallest configured SLR in those partials carries 1.45 MB. An SLR therefore
   counts as configured above 64 KiB of CFU data, or with any NPI write. On the pblock side, an
   SLR where the pblock has only clock or hard-block sites, and no SLICEs, need not be
   configured; the tool prints a note when it isn't.
2. **Clock-region rows (hard one way, soft the other).** Each frame names its CFU row: the index
   of the clock-region row among the SLR's fabric rows (rows that contain SLICEs). On the xcv80,
   SLR0 has rows 1–4 (row 0 has no fabric), SLR1 rows 5–7 and SLR2 rows 8–10. The tool reads the
   rows written as follows:
   - **Uncompressed stream:** decoded frame by frame.
   - **Compressed stream:** not decoded. The FAR packet header (`0xA1 | row | 0x01 | check`)
     survives compression, but not aligned to 16-byte quads. The tool counts that 4-byte pattern
     at every byte offset, per row byte. A row counts as written at ≥ max(500, 2 % of the SLR's
     busiest row) hits, and only if the SLR has that row. In 74 real compressed partials, the 522
     written rows got 7,954–70,579 hits (at least 12 % of the SLR's busiest row), and the 50
     in-range rows that weren't written got at most 70. The row values 82, 161 and 253 (0x52,
     0xA1, 0xFD) recur as structural artefacts with up to 60,307 hits. They are dropped only
     because no SLR has that many rows, so the device table's row count is load-bearing here.

   Every row where the pblock has SLICEs must be written: this rejects a candidate. A row written
   outside the pblock only counts against the candidate, for two reasons. A full-device SLASH
   compute partial writes SLR0 CFU row 0, which no site of its pblock reaches. The service layer's
   partial writes part of SLR0 row 0 through GT-side hard blocks that belong to no clock region.
3. **Columns (uncompressed streams with a frame table; hard one way, soft the other).** Every
   frame follows its own FAR write. FAR bits 0–19 are a frame index within the row that rises with
   X, bits 20–22 the block type, and bits 23 and up are 0, or 1 or 2 on frames two pblocks share.
   A partial writes every frame of each column it configures. So a SLICE column counts as written
   when its flip-flop frame is, and that needs no rule for where a column's frames end. In every
   row where both the stream writes and the pblock has SLICEs:
   - Every column of the pblock's SLICE ranges must be written: this rejects a candidate.
   - A written column outside those ranges only counts against the candidate. Snapping widens a
     pblock by a few columns, and a pblock reaches more columns through its non-SLICE sites
     (`IRI_QUAD`, hard blocks) than its SLICE ranges show.

   Frames between two columns' flip-flop frames (interconnect, and hard-block columns such as
   BRAM, DSP or NoC) are not attributed. Compressed streams don't keep FAR values: in the
   service-shell partials, taking the word after each surviving FAR header as a column gives more
   false columns than true ones.
4. **Tie-break.** The fewest rows written outside the pblock wins, then the fewest columns, then
   the per-SLR share of CFU bytes compared with the pblock's per-SLR share of SLICEs.

Status:
- **match:** exactly one pblock passes the hard tests, or several do and exactly one has no row
  outside it. Or several pass with no row outside them, columns were checked for all of them, and
  exactly one has the fewest columns outside. That holds for the true pblock: a pblock that is a
  column subset of it passes too, but with every column it lacks counted outside, and a superset
  fails on the columns that weren't written.
- **ambiguous:** several pass with no row outside them, and columns don't separate them; the
  nearest by area share is named.
- **probable:** several pass, but all with rows outside them; the closest is named.
- **unverified:** a pblock was chosen, but the PDI carries frame-capable content the SLR and row
  accounting does not follow, so the placement cannot be trusted. The best-fit pblock is still
  named, as a guess; the reasons are printed under `unverified:` (to stderr in the default output,
  in the `caveats` array in JSON). Three things trigger it, none seen in any real Vivado partial:
  - a keyhole write to a destination other than the CFU stream or an SLR forward;
  - CFU content in an SLR that is neither a clean empty chunk nor a configured SLR (0 < it < the
    configured threshold);
  - with a frame table, an uncompressed frame that doesn't follow its own FAR write in its row,
    so its column is unknown (all 594,744 frames of the two service-shell exports follow one).
- **none:** nothing passes.

What the verdict rests on, and what it does not: the tool follows the keyhole-CFU configuration
path (frames to `0xF1F80000` and the SLR-forward sub-streams) and counts NPI writes. It does not
interpret the other CDO commands a partial contains (direct register and mask writes), so it cannot
prove a partial configures *only* what its pblock covers through some other path; `unverified`
flags the cases it can detect. A placement is therefore a strong statement about the frame path,
not a security boundary on its own.

The `.vbin` member name (`top_i_<cell>_…_partial.pdi`, compared with `add_cells_to_pblock`) is
printed as a cross-check and is never used to decide.

## What it was checked on

72 real partials from Vivado 2025.1 builds for the V80 (xcv80), all placed in the right pblock:

- **SLASH service shell:** 5 service-layer and 5 user-region partials, each a `match`. Both
  shells' XDCs were given together.
- **SLASH compute shell:** 15 partials, each a `match`.
- **Five other floorplans of the compute shell,** all named `pblock_slash` and told apart only by
  their XDCs: 42 partials, 3 of them uncompressed.
  - 34 are a `match`. These include the three floorplans that share SLR1 + SLR2 and differ only
    in which SLR1 rows they cover.
  - 8 are `probable`, with the right pblock named. All come from the full-device floorplan,
    where a second floorplan, the first one's SLR0 top row plus SLR1 and SLR2, also passes.

With the frame table, the result is the same for all 72, and the true pblock passes every column
check in the three uncompressed partials. Two more uncompressed partials were exported from a
service-shell build's routed checkpoint, one per pblock; their compressed twins are byte-identical
to the partials the build shipped. Both are a `match`. In SLR1 row 2, which both pblocks reach,
the columns alone reject the other pblock:
- The service layer's partial leaves the user region's X164–243 unwritten.
- The user region's partial leaves the service layer's X0–83 and X352–363 unwritten.

In the build's logic location files, every flip-flop bit of the 68,515 SLICE sites that have one
lies in the table's frame for its column, for all 16 flip-flops of a site, not only the two the
probe uses. The generator's probe confirms the CFU-row mapping on all ten fabric rows.

`tests/slash.manifest` re-runs the SLASH part against the bundled XDC fixtures.

## Limits

- **Format:** the PDI format knowledge is inferred from Vivado 2025.1 xcv80 partials, not taken
  from AMD documentation. Another Vivado version or part may differ; the selftest pins the
  format the tool assumes. Other parts need their own device table, and the tool expects three
  SLRs.
- **Footprint estimate:** from XDC bounding boxes intersected with the device table. A site type
  missing from the table is warned about and ignored.
- **NPI writes:** counted, but not mapped to NoC sites; Vivado exposes no NPI address on a site.
- **Columns in uncompressed streams only.** Compressed partials, which are the ones loaded on the
  card, are still told apart per SLR and row only. So two pblocks that share their rows and differ
  only by column are `ambiguous` when compressed.
- **Columns from SLICE ranges only.** A pblock's columns come from its SLICE and `CLOCKREGION`
  ranges. Frames of hard-block columns, and columns reached only through non-SLICE sites, are not
  checked, so that part of a partial's footprint goes unverified.

## License

MIT, © 2026 ARH Laboratory; see `LICENSE`. The XDCs in `tests/fixtures/` are copied unchanged
from SLASH (© Advanced Micro Devices, Inc., MIT) and keep their own notice.
