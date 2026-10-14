// Clear wrong "no return" flags set by FLIRT, rebuild the functions that
// were cut short, then reanalyze. Run in IDA: File > Script file...
//
// 0x521D20 was matched as _exit, but it is the operator new wrapper
// (size 0 -> 1, heap alloc, new-handler retry). Because it was flagged as
// no-return, every function that allocates was cut short at the call.
#include <idc.idc>

static fix_noreturn(ea)
{
    auto flags, x, start, fixed;

    flags = get_func_flags(ea);
    if (flags == -1)
        return;
    if (flags & FUNC_NORET) {
        set_func_flags(ea, flags & ~FUNC_NORET);
        msg("cleared noreturn at %08X\n", ea);
    }

    // Rebuild callers whose function ends right after the call.
    fixed = 0;
    for (x = get_first_cref_to(ea); x != BADADDR; x = get_next_cref_to(ea, x)) {
        start = get_func_attr(x, FUNCATTR_START);
        if (start == BADADDR)
            continue;
        if (get_func_attr(x, FUNCATTR_END) == next_head(x, BADADDR)) {
            del_func(start);
            add_func(start, BADADDR);
            fixed++;
        }
    }
    msg("rebuilt %d callers of %08X\n", fixed, ea);
}

static main()
{
    auto ea, count;

    fix_noreturn(0x521D20); // operator_new

    plan_and_wait(get_inf_attr(INF_MIN_EA), get_inf_attr(INF_MAX_EA));

    // Second pass: callers that only became code during reanalysis.
    fix_noreturn(0x521D20);
    plan_and_wait(get_inf_attr(INF_MIN_EA), get_inf_attr(INF_MAX_EA));

    // List the remaining no-return functions so they can be checked by hand.
    count = 0;
    for (ea = get_next_func(get_inf_attr(INF_MIN_EA) - 1); ea != BADADDR; ea = get_next_func(ea)) {
        count++;
        if (get_func_flags(ea) & FUNC_NORET)
            msg("noreturn: %08X %s\n", ea, get_func_name(ea));
    }
    msg("done: %d functions\n", count);
}
