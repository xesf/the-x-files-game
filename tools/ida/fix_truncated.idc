// Rebuild functions that end right after a call to a function that
// returns. A real function never ends that way; these were cut short
// while some callee was wrongly flagged as no-return (see
// fix_noreturn.idc). Run in IDA: File > Script file...
//
// For each such function the script deletes it, and the function IDA
// started right after the call (if any), and lets IDA define it again
// from its start. It repeats until nothing changes.
#include <idc.idc>

static ends_with_returning_call(f)
{
    auto end, last, target, flags;

    end = get_func_attr(f, FUNCATTR_END);
    last = prev_head(end, f);
    if (last == BADADDR || print_insn_mnem(last) != "call")
        return 0;
    target = get_operand_value(last, 0);
    flags = get_func_flags(target);
    if (flags == -1)
        return 0; // indirect or unknown target: leave it
    return (flags & FUNC_NORET) == 0;
}

static main()
{
    auto f, end, pass, n, total;

    total = 0;
    for (pass = 0; pass < 6; pass++) {
        n = 0;
        for (f = get_next_func(get_inf_attr(INF_MIN_EA) - 1); f != BADADDR; f = get_next_func(f)) {
            if (!ends_with_returning_call(f))
                continue;
            end = get_func_attr(f, FUNCATTR_END);
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
    // Count what is left, for review.
    n = 0;
    for (f = get_next_func(get_inf_attr(INF_MIN_EA) - 1); f != BADADDR; f = get_next_func(f))
        if (ends_with_returning_call(f)) {
            if (n < 40)
                msg("still ends with a call: %08X %s\n", f, get_func_name(f));
            n++;
        }
    msg("done: rebuilt %d, %d still end with a call\n", total, n);
}
