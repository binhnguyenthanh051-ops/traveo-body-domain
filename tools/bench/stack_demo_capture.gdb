# stack_demo_capture.gdb — dump the STACK_DEMO=1 task's stack at every step (Node B, CM7_0).
#
# The image must be built with STACK_DEMO=1 (docs/hardware/kits.md "Bring-up switches").
# OpenOCD must already be running as the GDB server for Node B (see docs/bench/<date>/ notes:
# the CM7_0 core is OpenOCD's second target, normally on port 3334 — check its startup lines).
#
#   arm-none-eabi-gdb -q -batch \
#       -ex "set logging file docs/bench/2026-11-15/stack_demo_capture.txt" \
#       -x tools/bench/stack_demo_capture.gdb \
#       node_b_actuator/proj_cm7/build/APP_KIT_T2G-B-H_LITE/Debug/actuator_cm7.elf
#
# No reset: the demo task waits (g_stack_demo_go == 0) until this script lets it run, so
# the debugger can attach to a running board. Each run of the chain stops 5 times.

set pagination off
set confirm off
set logging overwrite on
set logging enabled on

target extended-remote localhost:3334

printf "=== the task's stack array and its neighbours ===\n"
info symbol &'stack_demo.c'::s_demo_stack
print &'stack_demo.c'::s_demo_stack
print sizeof('stack_demo.c'::s_demo_stack)
print &'stack_demo.c'::s_demo_spill
print &'stack_demo.c'::s_demo_tcb

printf "=== before the chain runs (the task is waiting) ===\n"
x/128wx &'stack_demo.c'::s_demo_stack

# "*": stop on the first instruction, before stack_demo_mark pushes its own r4, r5,
# so the dump shows exactly the callers' stack. The step is still in r0 there.
break *stack_demo_mark
set var g_stack_demo_go = 1

define dump_step
  printf "=== stop at stack_demo_mark(step=%u) ===\n", $r0
  info registers sp lr pc
  bt
  x/128wx &'stack_demo.c'::s_demo_stack
end

continue
dump_step
continue
dump_step
continue
dump_step
continue
dump_step
continue
dump_step

delete
printf "=== done: the chain ran once; the task is waiting again ===\n"
detach
