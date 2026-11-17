# stack_demo_overflow.gdb — catch the STACK_DEMO=2 overflow (Node B, CM7_0).
#
# The image must be built with STACK_DEMO=2. Start the GDB server first
# (bash ./tools/bench/gdbserver_node.sh B, which attaches without a reset), then:
#
#   arm-none-eabi-gdb -q -batch \
#       -ex "set logging file docs/bench/<date>/stack_demo_overflow.txt" \
#       -x tools/bench/stack_demo_overflow.gdb <actuator_cm7.elf>
#
# Two stops: step 4 (func_c has filled its too-big array; nothing has noticed yet), then
# FreeRTOS's overflow hook, called at the task switch that func_c asks for.
# A HardFault breakpoint catches the other possible ending.

set pagination off
set confirm off
set logging overwrite on
set logging enabled on

target extended-remote localhost:3334

print &'stack_demo.c'::s_demo_spill
print &'stack_demo.c'::s_demo_stack

break *stack_demo_mark if $r0 == 4
break vApplicationStackOverflowHook
break HardFault_Handler
set var g_stack_demo_go = 1

define dump_all
  info registers sp lr pc psp msp
  bt
  printf "--- s_demo_spill (the variable below the stack) ---\n"
  x/128wx &'stack_demo.c'::s_demo_spill
  printf "--- s_demo_stack ---\n"
  x/128wx &'stack_demo.c'::s_demo_stack
end

continue
printf "=== stop 1: func_c has filled arr_c (step 4), before the task switch ===\n"
dump_all

continue
printf "=== stop 2: where it ended ===\n"
print g_overflow_task_name
dump_all

delete
detach
