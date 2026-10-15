// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 xesf
// Clear wrong "no return" flags, rebuild the functions that were cut
// short, then reanalyze. Run in IDA: File > Script file...
//
// 0x521D20 was matched as _exit, but it is the operator new wrapper
// (size 0 -> 1, heap alloc, new-handler retry). Because it was flagged as
// no-return, IDA also flagged every function whose paths all end in an
// allocation, and so on up the call graph.
//
// The script clears the flag on every function below the C runtime and
// rebuilds callers that end at a call to one of them. IDA sets the flag
// again on functions that really never return. It repeats until the
// number of flags stops going down.
//
// Tip: the undo buffer fills up; it is safe to ignore the UNDO messages.
#include <idc.idc>

#define CRT_START 0x640C80 // C runtime functions from here on are correct

// Rebuilds callers of `ea` whose function ends right after the call.
static rebuild_callers(ea)
{
    auto x, start, fixed;

    fixed = 0;
    for (x = get_first_cref_to(ea); x != BADADDR; x = get_next_cref_to(ea, x)) {
        start = get_func_attr(x, FUNCATTR_START);
        if (start == BADADDR) {
            // Code after the call may have been left unexplored.
            auto_mark_range(x, next_head(x, BADADDR) + 1, AU_CODE);
            continue;
        }
        if (get_func_attr(x, FUNCATTR_END) == next_head(x, BADADDR)) {
            del_func(start);
            add_func(start, BADADDR);
            fixed++;
        }
    }
    return fixed;
}

static clear_pass()
{
    auto ea, flags, cleared, fixed;

    cleared = 0;
    fixed = 0;
    for (ea = get_next_func(get_inf_attr(INF_MIN_EA) - 1); ea != BADADDR && ea < CRT_START;
         ea = get_next_func(ea)) {
        flags = get_func_flags(ea);
        if (flags & FUNC_NORET) {
            set_func_flags(ea, flags & ~FUNC_NORET);
            fixed = fixed + rebuild_callers(ea);
            cleared++;
        }
    }
    plan_and_wait(get_inf_attr(INF_MIN_EA), get_inf_attr(INF_MAX_EA));
    msg("pass: cleared %d flags, rebuilt %d callers\n", cleared, fixed);
    return cleared;
}

static main()
{
    auto ea, pass, count, noret, cleared, prev;

    // Functions that really never return get flagged again each pass, so
    // stop once the number of cleared flags stops going down.
    prev = 0x7fffffff;
    for (pass = 0; pass < 8; pass++) {
        cleared = clear_pass();
        if (cleared == 0 || cleared >= prev)
            break;
        prev = cleared;
    }

    // List what IDA still considers no-return, for review.
    count = 0;
    noret = 0;
    for (ea = get_next_func(get_inf_attr(INF_MIN_EA) - 1); ea != BADADDR; ea = get_next_func(ea)) {
        count++;
        if (get_func_flags(ea) & FUNC_NORET) {
            noret++;
            msg("noreturn: %08X %s\n", ea, get_func_name(ea));
        }
    }
    msg("done: %d functions, %d no-return\n", count, noret);
}
