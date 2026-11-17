/*
 * stack_demo.h — Node B (CM7) stack demo for the blog (bench only, see stack_demo.c).
 */
#ifndef STACK_DEMO_H
#define STACK_DEMO_H

/* Create the demo task. Only built with STACK_DEMO=1 or STACK_DEMO=2 on the make
 * command line (docs/hardware/kits.md "Bring-up switches"). Call before
 * vTaskStartScheduler(). */
void stack_demo_create(void);

#endif /* STACK_DEMO_H */
