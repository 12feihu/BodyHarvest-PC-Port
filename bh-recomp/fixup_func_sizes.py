#!/usr/bin/env python3
"""Patch zero-sized FUNC symbols in a MIPS ELF by computing size = next_symbol_va - this_va.

BH's decomp asm files don't emit `.size` directives, so ld writes thousands of
FUNC symbols with st_size = 0. N64Recomp requires nonzero size to lift cross-
section jals to those functions, so we infer sizes here from neighboring
symbols within the same section.

Usage:
    python fixup_func_sizes.py INPUT.elf OUTPUT.elf
"""

import sys
import lief


CODE_SECTIONS = {
    ".core",
    ".overlay_gameplay_frontend",
    ".overlay_gameplay_outside",
    ".overlay_gameplay_inside",
    ".overlay_level_greece",
    ".overlay_level_java",
    ".overlay_level_america",
    ".overlay_level_siberia",
    ".overlay_level_comet",
}


def main(inp: str, outp: str) -> int:
    binary = lief.ELF.parse(inp)
    if binary is None:
        print(f"Failed to parse {inp}", file=sys.stderr)
        return 1

    # Map section index -> (name, va_start, va_end)
    sections = list(binary.sections)
    code_section_indices = {
        i for i, s in enumerate(sections) if s.name in CODE_SECTIONS
    }
    if not code_section_indices:
        print("No matching code sections found", file=sys.stderr)
        return 1

    # Bucket symbols by section index.
    by_section: dict[int, list] = {i: [] for i in code_section_indices}
    for sym in binary.symbols:
        if sym.shndx in by_section and sym.type == lief.ELF.Symbol.TYPE.FUNC:
            by_section[sym.shndx].append(sym)

    patched = 0
    skipped_nonzero = 0
    for sidx, syms in by_section.items():
        section = sections[sidx]
        section_end = section.virtual_address + section.size
        # Sort by value (VA), then by current size desc so larger-sized comes
        # first when duplicates exist at the same address.
        syms.sort(key=lambda s: (s.value, -s.size))

        # Compute "next distinct VA" for each unique VA in this section.
        unique_vas = sorted({s.value for s in syms})
        next_va_for = {}
        for i, va in enumerate(unique_vas):
            next_va_for[va] = unique_vas[i + 1] if i + 1 < len(unique_vas) else section_end

        for sym in syms:
            if sym.size != 0:
                skipped_nonzero += 1
                continue
            new_size = next_va_for[sym.value] - sym.value
            if new_size <= 0 or new_size > 0x100000:
                # Sanity-clamp; >1 MiB function is almost certainly wrong.
                continue
            sym.size = new_size
            patched += 1

    print(f"Patched {patched} FUNC symbols (left {skipped_nonzero} with non-zero size untouched)")
    binary.write(outp)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
