# stack_bound failure demo — 2026-11-14 (host only)

Evidence for the stack_bound blog post's user manual. Host run, no board. Run by Claude.

**Image:** Node B `proj_cm7` as built on 2026-11-12 (`actuator_cm7.elf`, sha256 `7de1298d7ad0…`, MTB GCC
14.2.1), with its `.su`/`.ci` in `build/`. **Tool:** `tools/stack_bound` at `894e799` (v0.2.0).
**Project files:** copies of `node_b_actuator/proj_cm7/stack_bound.toml` with one change each, kept in the
session scratchpad (not committed).

Command, from `node_b_actuator/proj_cm7` with `PYTHONPATH=../../tools/stack_bound/src`:

```
python -m stack_bound build/APP_KIT_T2G-B-H_LITE/Debug/actuator_cm7.elf --su build --project <file>
```

## 0. Unchanged project file: exit 0

```
STACK                   REQUIRED  CONFIGURED   USED       RECOMMENDED  STATUS
task can_task             1464 B      2048 B    71%    1832 B (458 w)  ok
task heartbeat_task        392 B       512 B    77%     496 B (124 w)  ok
task log_cyclic_task       308 B       512 B    60%      392 B (98 w)  ok
not analysed: main stack (MSP): the interrupt chain and the code before the scheduler starts (REQ-SB-204..206, not implemented yet)
```

(`can_task` is 1464 B here, 1472 B in the 2026-11-11 image: a later build, after the F-020/F-022 fixes.)

## 1. The `verify_core` binding deleted: exit 2

```
stack_bound: analysis error, no result:
  - task can_task: verify_core: indirect call at C:/00_Projects/00_Portfolio/00_TraveoBody/traveo-body-domain/shared/secoc/src/secoc.c:139:10 has no binding (add an [[indirect]] entry)
```

## 2. The `verify_core` binding pointed at the wrong function (`secoc_rx_process`): exit 2

```
stack_bound: analysis error, no result:
  - indirect 'verify_core' -> 'secoc_rx_process': secoc_rx_process is never address-taken, so no pointer can hold it
```

Both refusals give no result, so the build-script gate (`tools/stack_gate.sh`) would stop before flashing.

## 3. Run time

Wall clock of the whole command (Python 3.14.3, this PC), 3 runs each with the unchanged project files:

```
Node A  gateway_app.elf   1.6  1.7  1.8 s   exit 0
Node B  actuator_cm7.elf  2.2  2.5  2.0 s   exit 0
```
