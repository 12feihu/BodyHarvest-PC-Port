#!/usr/bin/env python3
"""Post-process RSPRecomp's aspMain.cpp to fix three classes of issue
introduced by linearly disassembling unreachable code past an aspMain
`break` instruction.

Run with no args; reads/writes aspMain.cpp in this directory.

Bugs:

1. `addiu $zero, $zero, X` — RSPRecomp emits `0 = RSP_ADD32(0, X);`
   which is not assignable. Real hardware: writes to $zero are nops,
   so we drop the line.

2. `goto L_<addr>;` where L_<addr> was never emitted (because <addr>
   is outside [text_address, text_address+text_size)). The branch comes
   from a misanalysed instruction in unreachable code; rewrite as
   `return RspExitReason::Unsupported;`.

3. `case 0xXXXX: goto L_XXXX;` in the indirect-jump dispatch where
   L_XXXX was never emitted. Drop the case.

We *don't* try to fix the underlying RSPRecomp pass because the
errors only happen on this one ucode and patching the recompiler
risks regressing other consumers.
"""

import re
import sys
from pathlib import Path

src = Path(__file__).with_name("aspMain.cpp")
text = src.read_text()

# ---- 1. Drop addiu/addi to $zero (rt==0) -----------------------------------
# Pattern emitted by RSPRecomp: `    0 = RSP_ADD32(0, ...);`
text, n_zero = re.subn(
    r"^[ \t]*0 = RSP_ADD32\(0,[^)]*\);[ \t]*\n",
    "",
    text,
    flags=re.MULTILINE,
)

# ---- 2/3. Find which L_xxxx labels are actually defined --------------------
defined = set(re.findall(r"^L_([0-9A-F]{4}):", text, flags=re.MULTILINE))

# ---- 2. Rewrite goto to undefined labels -----------------------------------
n_goto = 0
def _maybe_drop_goto(m):
    global n_goto
    label = m.group(1)
    if label in defined:
        return m.group(0)
    n_goto += 1
    indent = re.match(r"^[ \t]*", m.group(0)).group(0)
    return f"{indent}return RspExitReason::Unsupported;\n"

text = re.sub(
    r"^[ \t]*goto L_([0-9A-F]{4});[ \t]*\n",
    _maybe_drop_goto,
    text,
    flags=re.MULTILINE,
)

# ---- 3. Drop dispatch cases referencing undefined labels -------------------
def _maybe_drop_case(m):
    label = m.group(1)
    if label in defined:
        return m.group(0)
    return ""

text, n_case = re.subn(
    r"^[ \t]*case 0x[0-9A-F]+: goto L_([0-9A-F]{4});[ \t]*\n",
    _maybe_drop_case,
    text,
    flags=re.MULTILINE,
)

# ---- 4. UnhandledJumpTarget -> Broke (and silence the printf spam) ---------
# BH's aspMain frequently dispatches via `jr` to halfword values stored in
# its DMEM data table (e.g. 0xC000 at DMEM[0xBA]). On real hardware the RSP
# `jr` masks to the low 12 bits, so 0xC000 -> PC 0x000, which lands in
# rspboot's body. rspboot's halt path is the natural "ucode done" exit on a
# real RSP. We don't ship rspboot in librecomp, so reaching the
# `UnhandledJumpTarget` fallthrough in the indirect dispatch is functionally
# equivalent to "ucode exited cleanly".
#
# RSPRecomp wraps that fallthrough with two giant printf() calls (jump
# target + 32-register dump). With BH hitting this path on every audio task
# (~3/sec), those printfs flood stdout and block the SP Task Thread on
# NtWriteFile, snowballing into a downstream stall. Strip the printfs
# AND convert the return code in one pass.
# Hoist the debug counter declarations to file scope (extern "C" linkage
# specifiers can't appear inside a function body in C++). Inject right
# after the existing #include line at the top of aspMain.cpp.
text = re.sub(
    r'(#include "librecomp/rsp_vu_impl.hpp"\n)',
    r'\1'
    '// Phase 11 audio-silence diagnostic — see ../bh-app/src/main.cpp\n'
    'extern "C" void aspmain_dbg_count_unhandled_jr(uint32_t);\n'
    'extern "C" void aspmain_dbg_count_natural_break(void);\n',
    text,
    count=1,
)

text, n_exit = re.subn(
    r'    printf\("Unhandled jump target.*?return RspExitReason::UnhandledJumpTarget;',
    '    // patched: silently treat as rspboot exit (Phase 11 diag: count\n'
    '    // this path vs natural BREAK exits to see whether aspMain is\n'
    '    // exiting before producing audio samples)\n'
    '    aspmain_dbg_count_unhandled_jr(jump_target);\n'
    "    return RspExitReason::Broke;",
    text,
    flags=re.DOTALL,
)

# ---- 5. Instrument natural break exits with a debug counter ---------------
# Each `return RspExitReason::Broke;` at the bottom of a real `break 0`
# instruction increments aspmain_dbg_count_natural_break(). Comparing this
# to the patched-unhandled-jr count tells us whether aspMain reaches its
# normal end of work most of the time.
text, n_brk = re.subn(
    r'// break       0\n[ \t]*return RspExitReason::Broke;',
    "// break       0\n"
    "    aspmain_dbg_count_natural_break();\n"
    "    return RspExitReason::Broke;",
    text,
)

src.write_text(text)
print(f"fixup_aspmain: stripped {n_zero} zero-write line(s), "
      f"rewrote {n_goto} goto(s) to undefined labels, "
      f"dropped {n_case} dispatch case(s), "
      f"converted {n_exit} UnhandledJumpTarget -> Broke, "
      f"instrumented {n_brk} natural break exit(s).")
