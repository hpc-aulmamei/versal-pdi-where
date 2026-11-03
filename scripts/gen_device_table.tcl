# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 ARH Laboratory
#  SPDX-License-Identifier: MIT
# ##################################################################################################
#
# Write the device table pdi_where needs to turn pblock site ranges into SLRs: for every site type
# and every clock region, the X/Y range of that type's sites and the clock region's SLR. Sites that
# belong to no clock region are listed per type and SLR with clock region "-".
# Opens the part only (no design, nothing is built).
#
#   vivado -mode batch -nojournal -nolog -source gen_device_table.tcl \
#          -tclargs xcv80-lsva4737-2MHP-e-S device/xcv80.tsv

lassign $argv part out
create_project -in_memory -part $part
link_design -part $part

proc site_key {name} {
    # SLICE_X12Y34 -> {SLICE 12 34}; NOC_NMU512_X0Y5 -> {NOC_NMU512 0 5}
    if {[regexp {^(.+)_X(\d+)Y(\d+)$} $name -> t x y]} { return [list $t $x $y] }
    return {}
}

set fh [open $out w]
puts $fh "# pdi_where device table: part $part, Vivado [version -short]"
puts $fh "# type\tslr\tclock_region\tcr_row\txmin\txmax\tymin\tymax\tsites"
set seen [dict create]
foreach cr [lsort [get_clock_regions]] {
    set slr [get_property SLR_INDEX [get_slrs -of_objects $cr]]
    set row [get_property ROW_INDEX $cr]
    set agg [dict create]
    foreach s [get_sites -quiet -of_objects $cr] {
        set k [site_key $s]
        if {$k eq {}} continue
        lassign $k t x y
        dict set seen $s 1
        if {[dict exists $agg $t]} {
            lassign [dict get $agg $t] x0 x1 y0 y1 n
            dict set agg $t [list [expr {min($x0,$x)}] [expr {max($x1,$x)}] \
                                  [expr {min($y0,$y)}] [expr {max($y1,$y)}] [incr n]]
        } else {
            dict set agg $t [list $x $x $y $y 1]
        }
    }
    dict for {t v} $agg {
        lassign $v x0 x1 y0 y1 n
        puts $fh "$t\t$slr\t$cr\t$row\t$x0\t$x1\t$y0\t$y1\t$n"
    }
}
# Sites outside every clock region (GT, XPIO, PMC-side blocks, ...): one line per site type and
# SLR, with that type's bounding box in the SLR.
set orphans 0
foreach slr [lsort [get_slrs]] {
    set idx [get_property SLR_INDEX $slr]
    set agg [dict create]
    foreach s [get_sites -quiet -of_objects $slr] {
        if {[dict exists $seen $s]} continue
        set k [site_key $s]
        if {$k eq {}} continue
        lassign $k t x y
        incr orphans
        if {[dict exists $agg $t]} {
            lassign [dict get $agg $t] x0 x1 y0 y1 n
            dict set agg $t [list [expr {min($x0,$x)}] [expr {max($x1,$x)}] \
                                  [expr {min($y0,$y)}] [expr {max($y1,$y)}] [incr n]]
        } else {
            dict set agg $t [list $x $x $y $y 1]
        }
    }
    dict for {t v} $agg {
        lassign $v x0 x1 y0 y1 n
        puts $fh "$t\t$idx\t-\t-\t$x0\t$x1\t$y0\t$y1\t$n"
    }
}
close $fh
puts "gen_device_table: wrote $out ([dict size $seen] sites in clock regions, $orphans outside)"
