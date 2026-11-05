# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 ARH Laboratory
#  SPDX-License-Identifier: MIT
# ##################################################################################################
#
# Write the pdi_where frame table for one part: for every SLICE column of every CFU row, the block-0
# frame index that holds its flip-flop bits.
#
#   vivado -mode batch -source scripts/gen_frame_table.tcl -tclargs xcv80-lsva4737-2MHP-e-S device/xcv80.frames.tsv
#
# It builds a bare netlist with one flip-flop on AFF and one on HFF2 of a SLICE at every X in every
# clock region, places and routes it, and reads the frame addresses back from the logic location
# file (write_device_image -logic_location_file). The design has no CIPS, so the CIPS-2 DRC is
# lowered to a warning; the device image is a by-product and must never be loaded. Columns whose X
# share one frame (the two sites of a CLE tile) become one line. About 8 minutes on the xcv80. Run it
# from a scratch directory: Vivado leaves a few files (wdi_info.xml, clockInfo.txt) in the current one.
lassign $argv part out
if {$part eq "" || $out eq ""} {
    puts "usage: vivado -mode batch -source gen_frame_table.tcl -tclargs PART OUT.tsv"
    exit 1
}
set work [file join [file dirname [file normalize $out]] .frames_probe]
file mkdir $work

link_design -part $part
create_net gnd
create_cell -reference GND gnd_i
connect_net -net gnd -objects [get_pins gnd_i/G]

set sites [get_sites -filter {SITE_TYPE =~ SLICE*}]
set pick [dict create]
foreach s $sites cr [get_property CLOCK_REGION $sites] {
    if {$cr eq "" || ![regexp {X(\d+)Y\d+$} $s - x]} continue
    regexp {Y(\d+)$} $cr - row
    if {![dict exists $pick $row,$x]} { dict set pick $row,$x $s }
}
set i 0
dict for {k s} $pick {
    foreach bel {AFF HFF2} {
        set c [create_cell -reference FDRE p$i]
        set_property DONT_TOUCH true $c
        connect_net -net gnd -objects [get_pins "p$i/C p$i/CE p$i/R p$i/D"]
        create_net q$i
        connect_net -net q$i -objects [get_pins p$i/Q]
        set_property LOC $s $c
        set_property BEL $bel $c
        incr i
    }
}
puts "gen_frame_table: [dict size $pick] SLICE columns, $i flip-flops"
place_design
route_design
set_property SEVERITY {Warning} [get_drc_checks CIPS-2]
write_device_image -force -logic_location_file -no_pdi [file join $work probe]

# Bit <offset> <frame address> <frame offset> SLR<n> <n> Block=SLICE_X..Y.. Latch=..
# frame address: bits 23+ CFU row, 20-22 block, 0-19 frame index
set col [dict create]
set fh [open [file join $work probe.ll]]
while {[gets $fh line] >= 0} {
    if {![regexp {^Bit\s+\d+\s+0x([0-9a-fA-F]+)\s+\d+\s+SLR\d+\s+(\d+)\s+Block=SLICE_X(\d+)Y\d+} $line - fa slr x]} continue
    set fa [expr {"0x$fa"}]
    if {(($fa >> 20) & 7) != 0} { error "gen_frame_table: flip-flop bit outside block 0: $line" }
    set key "$slr [expr {$fa >> 23}] $x"
    set f [expr {$fa & 0xFFFFF}]
    if {[dict exists $col $key] && [dict get $col $key] != $f} { error "gen_frame_table: SLICE X$x has bits in two frames" }
    dict set col $key $f
}
close $fh

set rows [dict create]
dict for {key f} $col {
    lassign $key slr row x
    dict lappend rows "$slr $row" [list $x $f]
}
set o [open $out w]
puts $o "# pdi_where frame table: part $part, Vivado [version -short]"
puts $o "# slr\tcfu_row\tx_first\tx_last\tframe"
set lines 0
foreach key [lsort -dictionary [dict keys $rows]] {
    lassign $key slr row
    set prev ""
    foreach e [lsort -integer -index 0 [dict get $rows $key]] {
        lassign $e x f
        if {$prev ne "" && [lindex $prev 2] == $f} {
            lset prev 1 $x
            continue
        }
        if {$prev ne ""} {
            if {$f < [lindex $prev 2]} { error "gen_frame_table: SLR$slr row $row: frames fall as X rises at X$x" }
            puts $o "$slr\t$row\t[join $prev \t]"
            incr lines
        }
        set prev [list $x $x $f]
    }
    puts $o "$slr\t$row\t[join $prev \t]"
    incr lines
}
close $o
file delete -force $work
puts "gen_frame_table: $lines columns in [dict size $rows] rows written to $out"
