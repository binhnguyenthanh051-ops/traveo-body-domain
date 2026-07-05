<!--
DRAFT — M1 bootloader bring-up chapter. Adapt before publishing:
- This is written in a first-person engineer voice as a starting point. Do a real editing
  pass so it sounds like YOU, not like a polished-by-AI draft — your own asides and phrasing
  are what make it authentic (and that authenticity is part of what's being evaluated).
- [MEDIA: ...] markers show where to drop photos/GIFs/screenshots (see the shot list).
- Copyright: paraphrase vendor docs/TRM; do NOT paste vendor driver source or TRM tables
  verbatim. Keep your own code snippets short and illustrative.
- Set the episode number/series label to match your publishing order.
- Link the code at the `m1-bootloader` tag where noted.
-->

# The bootloader that only crashed when I unplugged the debugger

*Part of a series building an automotive body-domain network from scratch on an Infineon
TRAVEO™ T2G, with the design done in the open and AI used as a visible collaborator.*

I spent a couple of weeks designing a flash bootloader before the board for it had even
shipped. Memory map, the boot-decision state machine, the no-init RAM handshake, the jump
into the application — all written down as design records, argued over, reviewed, and — for
the parts that didn't need hardware — proven with unit tests on my laptop.

Then the board arrived, and the silicon started telling me which parts of my design were
fiction.

This post is about that gap: the place where a clean design document meets a real chip. None
of the surprises were in the boot *logic* I'd tested on x86. Every single one lived at the
seam between my code and the hardware — and the best of them was a bug that only appeared
when no debugger was watching.

## The bet: test the logic where the logic lives

A quick word on how the project is structured, because it's the reason the rest of this story
went the way it did.

A bootloader is mostly decisions: given the reset cause, the persisted state, and whether the
application image is valid, do I run the app, stay in the bootloader to be reprogrammed, or
fall back to a safe state? Those decisions are pure logic. They don't need a chip to be
correct — they need to be *right*, and the way you know they're right is tests.

So I split the bootloader in two. The decision logic lives in a portable core that compiles
and unit-tests on my laptop with plain GCC. Everything that actually touches the chip — read
the reset-cause register, prime RAM, write a backup register, perform the jump — sits behind
a small interface, with a fake implementation for the tests and a real one for the board.

The promise of that split is simple: when the board misbehaves, I'll know the bug is in the
hardware-facing code, because the logic behind it has already been proven. Spoiler: that
promise held, completely.

[MEDIA: the board on the bench, debugger connected, LED lit — the "hardware is alive" shot]

## Surprise 1: "FBL + app" is a single-core fantasy

My design map was flat and obvious: bootloader at the bottom of flash, application right
above it. On a single-core part, that's exactly right. This part is not single-core.

TRAVEO™ T2G is asymmetric dual-core, and the boot ROM brings up the Cortex-M0+ *first*. The
M0+ runs a prebuilt image that owns the first chunk of flash, and only then does my CM4 code —
the bootloader — get to run, starting well above where I'd assumed. My "application base"
address, as designed, landed *inside* the M0+'s region. If I'd flashed against that map, I'd
have been writing the app on top of the code that boots the chip.

The fix was to move the addresses and, for the application, to strip its bundled M0+ image
entirely: in a bootloader-plus-app split on a dual-core part, exactly one image carries the
M0+ boot, and the other is a pure CM4 payload that I address and stamp by hand.

The lesson I keep coming back to: my design doc had the memory map marked *"verify in the
reference manual."* In practice, the board support package's linker script *was* the reference
manual — the ground truth was in the build system, not the PDF.

[MEDIA: a simple before/after diagram of the memory map — flat (wrong) vs dual-core (real)]

## Surprise 2: the reset register that defaults to non-zero

To decide what survived the last reset, the bootloader reads the reset-cause register. My
draft logic leaned on a reasonable-sounding assumption: a power-on reset clears the register,
so a value of zero can be read as "we came up cold."

On this chip, that's backwards. Power-on is a specific bit, and the register's default value
has that bit *set* — so the cold-boot state is a non-zero value, and a literal zero means "no
recorded cause," which is the opposite of what my draft assumed. Reading zero as power-on
would have mis-filed every real cold boot.

I rewrote the classification to detect the cold-boot group by its actual bits rather than by
zero, and confirmed it live by power-cycling and resetting the board. Textbook "the datasheet
said X, the chip said Y" — and a good reminder that "the register clears on reset" is an
intuition, not a fact. Read the bitfield.

[MEDIA: debugger watch window showing the reset-cause register's non-zero default value]

## The big one: benign with a debugger, fatal without

Here's the one that cost me an afternoon and taught me the most.

I had the boot-loop fallback working — the bootloader counts unexpected resets and, after too
many, refuses to run a misbehaving app and waits to be reprogrammed instead. I demoed it,
power-cycled the board to start fresh, and the bootloader came up dead. HardFault. No app, no
blink, nothing.

What made it baffling: it had worked *seconds earlier*, under the debugger.

The fault frame told the story once I read it carefully. No bus fault, no memory fault, no
usage fault — the flags that normally point at the problem were all clear. Instead, the
hard-fault status flagged a debug event. A debug event, in a HardFault, with everything else
clean, means one specific thing: the CPU executed a breakpoint instruction. Resolving the
faulting address back to a function name pointed straight at a vendor driver call I was making
to read a backup register — with an index of zero.

The vendor's backup-register functions contain an assertion that rejects index zero, even
though the API documentation says the index starts at zero. The assertion contradicts the
documented contract, and my code — which used backup register zero to hold a validity marker —
tripped it on every call.

And *that* is why it only died without the debugger. The assertion is implemented as a
breakpoint instruction. With a debugger attached, a breakpoint just halts the core — you
resume past it and everything appears to work, which is exactly what I'd been doing all
afternoon without realising it. Detached, after a real power cycle with the debugger's USB
gone, the same breakpoint had nowhere to halt to and escalated into the HardFault that bricked
the boot.

The fix was to stop going through the vendor wrapper and access the backup register directly —
which is precisely what the wrapper does internally, just without the assertion that disagrees
with its own documentation.

Three things I took from this one:

- **Read the driver source, not just the header.** The behaviour that bit me wasn't in the
  documentation — it contradicted it. The truth was in the implementation.
- **"Works attached, faults detached" is a whole category of bug**, and a nasty one, because
  the debugger you reach for to investigate is the very thing that hides it. A breakpoint is
  harmless with a debugger and fatal without.
- **Always do a final pass on real power, with nothing attached.** If I'd only ever tested
  under the debugger, this would have shipped and failed in the field, not on my bench.

[MEDIA: debugger view of the fault registers — the clean fault flags plus the debug-event bit]
[MEDIA: terminal showing the faulting address resolving to the backup-register function]

## A quieter lesson: the jump is a hand-over, not a branch

One more, caught by review rather than by a crash. Jumping from the bootloader to the
application *looks* like setting two registers and branching. But if you take that jump after
the app has already run once — say, after a software reset rather than a cold boot — you can
carry live interrupt state into an application whose interrupt table isn't set up yet, and
fault.

So the real handover stops the system timer, clears pending interrupts across the board, and
only then switches the vector table and branches. A bootloader-to-app jump is a *state*
hand-over. The classic "works from cold boot but not after a warm path" failure is exactly the
result of carrying dirty interrupt state across that boundary.

## What the whole thing proved

Step back and look at where the surprises landed. The dual-core memory map. A reset-cause bit.
A vendor-driver assertion. An interrupt-state hand-over. **Every one of them lived at the seam
between my code and the chip.** The boot decision logic, the integrity check, the boot-loop
counter rules — all the stuff I'd unit-tested on my laptop — were correct on the first run on
real silicon and never came up again.

That's the bet from the start of this post paying off exactly as designed. Because I'd proven
the logic where the logic lives, every bug I hit on the board, I could trust was at the
hardware seam — which is a very different debugging experience from staring at a fault and
wondering whether it's my algorithm or my register access. It was always the register access.
That's the single most useful habit I'd carry into any bring-up.

## Where this leaves M1

The milestone is real: the bootloader comes up, checks the application image for integrity,
and jumps into it cleanly from a cold boot, with the boot-loop fallback working on silicon.
The code at this point is tagged [`m1-bootloader`](LINK-TO-TAG).

Two things are honestly deferred, not done: exercising the no-init RAM handshake and the
recovery "knock" window both need a CAN interface on the bench, and their real consumers live
in later milestones — so they're carried forward rather than rushed. And a note I'll keep
repeating for as long as it's true: there's no secure boot turned on here yet. The integrity
check catches a corrupted or half-flashed image; it is *not* authenticity. Until the
root-of-trust is actually provisioned, this system is not secure, and I'd rather say so plainly
than imply otherwise.

Next up is the application itself, on a real-time OS — which, conveniently, brings the CAN
interface online and lets me close out the two M1 items I just deferred.

---

*A note on how this was built: the design records and a lot of the scaffolding were drafted
with an AI coding assistant, then reviewed — and the reviews caught real problems before the
board ever arrived, including a couple of "correct on paper, wrong in behaviour" bugs. I treat
every generated line as a draft I have to understand and defend, which is the only way the
"explain this in an interview" test is survivable. More on that workflow in another post.*
