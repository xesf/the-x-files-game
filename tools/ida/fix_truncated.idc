// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 xesf and contributors
// Rebuild functions that end right after a call. A real function never
// ends that way: these were cut short while a callee was wrongly flagged
// as no-return (see fix_noreturn.idc), and IDA can set the flag again on
// later analysis. Run in IDA: File > Script file...
//
// For each function below the C runtime that ends with a call followed
// by more code (not int3 padding), the script clears the no-return flag
// of the callee (unless the callee is in the C runtime), deletes the
// function and any function IDA started right after the call, and lets
// IDA define it again. It repeats until nothing changes.
#include <idc.idc>

#define CRT_START 0x640C80

static is_truncated(f)
{
    auto end, last;

    if (f >= CRT_START)
        return 0;
    end = get_func_attr(f, FUNCATTR_END);
    last = prev_head(end, f);
    if (last == BADADDR || print_insn_mnem(last) != "call")
        return 0;
    // Padding after the call means the function really ends there.
    if (get_wide_byte(end) == 0xCC || get_wide_byte(end) == 0x90)
        return 0;
    return 1;
}

static main()
{
    auto f, end, last, target, flags, pass, n, total;

    total = 0;
    for (pass = 0; pass < 8; pass++) {
        n = 0;
        for (f = get_next_func(get_inf_attr(INF_MIN_EA) - 1); f != BADADDR; f = get_next_func(f)) {
            if (!is_truncated(f))
                continue;
            end = get_func_attr(f, FUNCATTR_END);
            last = prev_head(end, f);
            target = get_operand_value(last, 0);
            flags = get_func_flags(target);
            if (target < CRT_START && flags != -1 && (flags & FUNC_NORET))
                set_func_flags(target, flags & ~FUNC_NORET);
            if (target >= CRT_START && flags != -1 && (flags & FUNC_NORET))
                continue; // a real no-return call such as _exit
            if (get_func_attr(end, FUNCATTR_START) == end)
                del_func(end);
            del_func(f);
            add_func(f, BADADDR);
            n++;
        }
        plan_and_wait(get_inf_attr(INF_MIN_EA), get_inf_attr(INF_MAX_EA));
        msg("pass %d: rebuilt %d functions\n", pass, n);
        total = total + n;
        if (n == 0)
            break;
    }
    n = 0;
    for (f = get_next_func(get_inf_attr(INF_MIN_EA) - 1); f != BADADDR; f = get_next_func(f))
        if (is_truncated(f)) {
            if (n < 40)
                msg("still ends with a call: %08X %s\n", f, get_func_name(f));
            n++;
        }
    msg("done: rebuilt %d, %d still end with a call\n", total, n);
}
