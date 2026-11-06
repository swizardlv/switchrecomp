/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Maxwell instruction decoder. The encodings are maxwell_opcodes.def; tools/shaderdec turns them into constant tables
   and a matcher (maxwell_decode.inc in the build folder), so nothing is built at run time. */
#include "nx/gpu/shader/translator.h"
#include "maxwell_decode.inc"

const Op* decode(u64 w) {
    const Op* op = shaderdec_match(w);
    /* EXIT.FCSM_TR (flow condition 0x1C), which the compiler puts after VOTE.VTG, is the hardware's cull exit: as an
       EXIT the vertex program would end before writing its position */
    if (op && op->id == O_EXIT && B(w, 0, 5) == 0x1C) return &shaderdec_ops[shaderdec_row[O_NOP]];
    return op;
}
