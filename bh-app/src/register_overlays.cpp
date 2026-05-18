// Wraps N64Recomp's auto-generated section/overlay tables and registers them
// with librecomp at startup. The tables themselves live in
// RecompiledFuncs/recomp_overlays.inl (regenerated every N64Recomp run).

#include "librecomp/overlays.hpp"

// This header includes everything we need: section_table, num_sections,
// overlay_sections_by_index. It is meant to be included into exactly one
// translation unit (it defines static arrays).
#include "recomp_overlays.inl"

#include "bh_app.hpp"

void bh::register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections {
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays {
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);
}
