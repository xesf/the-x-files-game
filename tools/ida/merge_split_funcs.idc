// Rejoin functions that IDA split at a call it wrongly took for no-return.
// Run after fix_noreturn.idc. Run in IDA: File > Script file...
//
// Pattern: function A ends with a call to a function that returns, and
// function B starts right after that call with no jump or call into it.
// B is then the rest of A.
#include <idc.idc>

static is_split(a)
{
    auto end, last, target, b;

    end = get_func_attr(a, FUNCATTR_END);
    last = prev_head(end, a);
    if (last == BADADDR || print_insn_mnem(last) != "call")
        return 0;
    target = get_operand_value(last, 0);
    if (get_func_flags(target) != -1 && (get_func_flags(target) & FUNC_NORET))
        return 0;
    b = get_func_attr(end, FUNCATTR_START);
    if (b != end)
        return 0;
    // Only the fall-through from the call may reach B.
    if (get_first_fcref_to(end) != BADADDR)
        return 0;
    if (get_first_dref_to(end) != BADADDR)
        return 0;
    return 1;
}

static main()
{
    auto a, merged, pass, n;

    merged = 0;
    for (pass = 0; pass < 4; pass++) {
        n = 0;
        for (a = get_next_func(get_inf_attr(INF_MIN_EA) - 1); a != BADADDR; a = get_next_func(a)) {
            if (is_split(a)) {
                del_func(get_func_attr(a, FUNCATTR_END));
                del_func(a);
                add_func(a, BADADDR);
                msg("merged %08X\n", a);
                n++;
            }
        }
        plan_and_wait(get_inf_attr(INF_MIN_EA), get_inf_attr(INF_MAX_EA));
        merged = merged + n;
        if (n == 0)
            break;
    }
    msg("done: merged %d split functions\n", merged);
}
