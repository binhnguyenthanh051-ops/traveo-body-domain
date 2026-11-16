# Thumb-2 examples for the stack_bound post — 2026-11-14 (host only)

Evidence for the post's "concept" section. Host run, no board. Run by Claude.

**Image:** Node B `proj_cm7`, built 2026-11-12 (`actuator_cm7.elf`, sha256 `7de1298d7ad0…`, MTB GCC 14.2.1,
`-Og`). **Disassembler:** `arm-none-eabi-objdump -d --no-show-raw-insn` from MTB GCC 14.2.1.
These addresses differ from `tools/stack_bound/docs/how-it-reads-thumb2.md`, which used an earlier build.

## Symbols (`arm-none-eabi-nm`)

```
10086128 T crypto_mac
10087eb0 t g_oracle
10086328 t log_ring_copy_in
1008687a t verify_core
```

## `verify_core` (excerpt)

```
1008687a:	stmdb	sp!, {r4, r5, r6, r7, r8, r9, sl, fp, lr}
1008687e:	sub	sp, #100	@ 0x64
10086880:	mov	r7, r0
10086888:	ldr	r6, [sp, #144]	@ 0x90
10086898:	str.w	sl, [sp]
1008689e:	bl	100867c4 <build_mac_input>
100868a4:	ldr	r3, [r7, #0]
100868a6:	ldr.w	fp, [r3]
100868ae:	ldr	r0, [r7, #4]
100868b0:	blx	fp
100868bc:	bl	1008683e <secoc_ct_equal>
100868c4:	b.n	100868d0 <verify_core+0x56>
100868da:	bl	100867ba <get_u16le>
100868f0:	add	sp, #100	@ 0x64
100868f2:	ldmia.w	sp!, {r4, r5, r6, r7, r8, r9, sl, fp, pc}
```

Frame: 9 registers × 4 = 36 B + 100 B = 136 B, equal to GCC's `.su` (`secoc.c:126:30:verify_core 136 static`).
Arguments 5–8 of `verify_core` are read at `[sp, #136]`…`[sp, #148]`, i.e. just above its own frame.

## `get_u16le`: a leaf (Node B)

```
100867ba <get_u16le>:
100867ba:	ldrb	r3, [r0, #0]
100867bc:	ldrb	r0, [r0, #1]
100867be:	orr.w	r0, r3, r0, lsl #8
100867c2:	bx	lr
```

No push, no `sub sp`: frame 0 B, equal to GCC's `.su` (`secoc.c:38:17:get_u16le 0 static`).

## `PendSV_Handler`: the FreeRTOS context switch (Node A)

Node A `gateway_app.elf`, built 2026-11-12 (sha256 `60d6eb89202a…`), same objdump. The only `vstmdb`/`vpush`
in the image; Node B's app code has no `vpush` either.

```
10041690:	mrs	r0, PSP
1004169c:	tst.w	lr, #16
100416a0:	it	eq
100416a2:	vstmdbeq	r0!, {s16-s31}
100416a6:	stmdb	r0!, {r4, r5, r6, r7, r8, r9, sl, fp, lr}
100416aa:	str	r0, [r2, #0]
100416c0:	bl	1004279c <vTaskSwitchContext>
100416d2:	ldmia.w	r0!, {r4, r5, r6, r7, r8, r9, sl, fp, lr}
100416dc:	vldmiaeq	r0!, {s16-s31}
100416e0:	msr	PSP, r0
100416e8:	bx	lr
```

`lr` here holds EXC_RETURN; bit 4 clear means the hardware stacked an FP frame, so `s16–s31` are saved too
(64 B), then `r4–r11, lr` (36 B). The new stack pointer is stored in the task's TCB (`str r0, [r2]`).

## `crypto_mac` prologue

```
10086128:	push	{r4, r5, lr}
1008612a:	sub.w	sp, sp, #556	@ 0x22c
```

Frame: 12 B + 556 B = 568 B.

## `g_oracle` in flash (`objdump -s`)

```
 10087eb0 29610810
```

Little-endian word `0x10086129` = `crypto_mac` (`0x10086128`) with the Thumb bit set.
Source: `static const secoc_mac_if_t g_oracle = { crypto_mac };` (`node_b_actuator/proj_cm7/src/secoc_app.c:27`).

## Worst path of `can_task` in this build (stack_bound 0.2.0, exit 0)

```
worst path, task can_task (1256 B + 208 B switch frame, stack from ELF symbol s_stack):
     can_task 168 B
   -> secoc_app_verify_and_decode 104 B
   -> secoc_rx_process 64 B
   -> secoc_verify_len 56 B
   -> verify_core 136 B
   ~> crypto_mac 568 B   [listed: cy->oracle->mac: g_oracle = { crypto_mac } (secoc_app.c:27), the only secoc_mac_if_t in the image]
   -> crypto_report 8 B
   -> log_evt 80 B
   -> log_ring_put_locked 40 B
   -> log_ring_copy_in 24 B
   -> memcpy 8 B
```

From this: depth(log_evt) = 80 + 40 + 24 + 8 = 152 B; depth(crypto_mac) = 568 + 8 + 152 = 728 B;
depth(verify_core) = 136 + 728 = 864 B. (The 2026-11-11 image had `log_try_enqueue` 48 B here instead of
`log_ring_put_locked` 40 B: the F-022 fix of 2026-11-12.)

## Float ABI (both nodes)

- Node B `proj_cm7`: `VFP_SELECT=hardfp` (Makefile line 172); `actuator_cm7.ninja` passes `-mfloat-abi=hard`;
  `arm-none-eabi-readelf -A actuator_cm7.elf` → `Tag_FP_arch: FPv5/FP-D16 for ARMv8`, `Tag_ABI_VFP_args: VFP registers`.
- Node A `app`: `VFP_SELECT=` (Makefile line 128, ModusToolbox default); `gateway_app.ninja` passes
  `-mfloat-abi=softfp`; `readelf -A` → `Tag_FP_arch: VFPv4-D16`, no `Tag_ABI_VFP_args` (float arguments in core registers).

## FreeRTOS overflow check (both nodes)

`configCHECK_FOR_STACK_OVERFLOW 2` in `node_a_gateway/app/config/FreeRTOSConfig.h:41` and
`node_b_actuator/proj_cm7/FreeRTOSConfig.h:88`. Method 2 (`mtb_shared/freertos/release-v10.6.202/Source/include/stack_macros.h:92-97`)
checks, at switch-out, that the lowest 4 words of the stack still hold `0xa5a5a5a5`; new stacks are filled
with `tskSTACK_FILL_BYTE 0xa5` (`tasks.c:79`).
