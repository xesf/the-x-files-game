# Clear wrong "no return" flags set by FLIRT, then reanalyze the database.
# Run in IDA: File > Script file...
#
# 0x521d20 was matched as _exit, but it is the operator new wrapper
# (size 0 -> 1, heap alloc, new-handler retry). Because it was flagged as
# no-return, every function that allocates was cut short at the call.
import ida_auto
import ida_funcs
import ida_ida
import idautils
import idc

WRONG_NORETURN = [
    0x521D20,  # operator_new
]

for ea in WRONG_NORETURN:
    f = ida_funcs.get_func(ea)
    if f and (f.flags & ida_funcs.FUNC_NORET):
        f.flags &= ~ida_funcs.FUNC_NORET
        ida_funcs.update_func(f)
        print("cleared noreturn at %08X" % ea)

# Calls to the fixed functions must be re-flowed, so rebuild code analysis.
ida_auto.plan_range(ida_ida.inf_get_min_ea(), ida_ida.inf_get_max_ea())
ida_auto.auto_wait()

# List the remaining no-return functions so they can be checked by hand.
for ea in idautils.Functions():
    f = ida_funcs.get_func(ea)
    if f.flags & ida_funcs.FUNC_NORET:
        print("noreturn: %08X %s" % (ea, idc.get_func_name(ea)))
print("done: %d functions" % len(list(idautils.Functions())))
