/* 
 * Assembly code for interrupt handling.  
 *  Good exercise: find 10 places where if you delete,
 *  you get non-deterministic failures.
 */

#include "rpi-asm.h"

/*
 * Enable/disable interrupts.
 *
 * CPSR [A2-11] = current program status register
 *        upper bits are different carry flags.
 *        lower 8:
 *           7 6 5 4 3 2 1 0
 *          +-+-+-+---------+
 *          |I|F|T|   Mode  |
 *          +-+-+-+---------+
 *  I : disables IRQ when = 1.
 *  F : disables FIQ when = 1.
 *  T : = 0 indicates ARM execution, 
 *      = 1 is thumb execution.  
 *  Mode = current mode.
 */

@ enable system interrupts by modifying cpsr.
@    note: should make a version that returns 
@    previous state.
@ <.globl> makes name visible to other files
.globl enable_interrupts  
enable_interrupts:
    mrs r0,cpsr         @ move cpsr to r0
    bic r0,r0,#(1<<7)	@ clear 7th bit. @bic is beat clear operator.
    msr cpsr_c,r0		@ move r0 back to PSR
    bx lr		        @ return.

@ disable them
.globl disable_interrupts
disable_interrupts:
    mrs r0,cpsr		       
    orr r0,r0,#(1<<7)	@ set 7th bit @orr is the or operator in asembly
    msr cpsr_c,r0
    bx lr

@ the interrupt table that we copy to 0x0.
@   - start = <_interrupt_table>
@   - end = <_interrupt_table_end>
@   - look at the disassembly
@
@ note: *it must be position independent since
@ we copy it!*
.globl _interrupt_table
.globl _interrupt_table_end
_interrupt_table:
  @ Q: why can we copy these ldr jumps and have
  @ them work the same?
  ldr pc, _reset_asm                    @ 0x0: Q: why this order?[A2-16]
  ldr pc, _undefined_instruction_asm    @ 0x4
  ldr pc, _software_interrupt_asm       @ 0x8
  ldr pc, _prefetch_abort_asm
  ldr pc, _data_abort_asm
  ldr pc, _reset_asm
  ldr pc, _interrupt_asm
fast_interrupt_asm:
  sub   lr, lr, #4 @First instr of FIQ handler
  push  {lr}
  push  {r0-r12}
  mov   r0, lr              @ Pass old pc
  bl    fast_interrupt_vector    @ C function
  pop   {r0-r12}
  ldm   sp!, {pc}^

_reset_asm:                   .word reset_asm
_undefined_instruction_asm:   .word undefined_instruction_asm
_software_interrupt_asm:      .word software_interrupt_asm
_prefetch_abort_asm:          .word prefetch_abort_asm
_data_abort_asm:              .word data_abort_asm
_interrupt_asm:               .word interrupt_asm
_interrupt_table_end:   @ end of the table.

@ only handler that should run since we 
@ only enable general interrupts
interrupt_asm:
  @ NOTE:
  @  - each mode has its own <sp> that persists when
  @    we switch out of the mode (i.e., will be the same
  @    when switch back).
  @  - <INT_STACK_ADDR> is a physical address we reserve 
  @   for exception stacks today.  we don't do recursive
  @   exception/interupts so one stack is enough.
  mov sp, #INT_STACK_ADDR   @ Q: what if you delete?
  sub   lr, lr, #4

  @ push regs: beter match a pop
  push  {r0-r12,lr}         @ XXX: pushing too many 
                            @ registers: only need caller
                            @ saved.

  mov   r0, lr              @ Pass old pc as arg 0
  bl    interrupt_vector    @ C function: expects C 
                            @ calling conventions.

  @ pop regs: better match push (what happens if not?)
  pop   {r0-r12,lr} 	    @ pop integer registers
                            @ this MUST MATCH the push.
                            @ very common mistake.

  @ return from interrupt handler: will re-enable general ints.
  @ Q: what happens if you do "mov" instead?
  @ Q: what other instructions could we use?
  movs    pc, lr        @ 1: moves <spsr> into <cpsr> 
                        @ 2. moves <lr> into the <pc> of that
                        @    mode.

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ FAST interrupt handler: everything inlined in assembly.
@
@ Optimizations vs the original C handler:
@   1. Only save r0-r5,lr (7 regs) instead of r0-r12,lr (14)
@   2. No bl to C function (saves pipeline flush + link overhead)
@   3. No dev_barrier() calls (3 removed, 1 dsb kept for timer clear)
@   4. Inline GPIO write (no function call to gpio_write/gpio_set_on/off)
@   5. Inline IRQ check + clear (no GET32/PUT32 function calls)
@
interrupt_asm_fast:
  mov sp, #INT_STACK_ADDR
  sub lr, lr, #4
  push {r0-r5, lr}           @ only save regs we use + lr

  @ debug: set GPIO pin 11 high (interrupt start)
  ldr r0, =0x2020001C        @ GPSET0
  mov r1, #(1 << 11)
  str r1, [r0]

  @ 1. check if timer IRQ is pending
  ldr r0, =0x2000B200        @ IRQ_basic_pending
  ldr r1, [r0]
  tst r1, #1                 @ ARM_Timer_IRQ = bit 0
  beq .Ldone

  @ 2. clear timer IRQ
  ldr r0, =0x2000B40C        @ ARM_Timer_IRQ_Clear
  mov r1, #1
  str r1, [r0]
  mcr p15, 0, r0, c7, c10, 4 @ DSB: ensure clear propagates before return

  @ 3. bounds check: if pdm_bit_index >= total_pdm_bits, skip
  ldr r0, =pdm_bit_index
  ldr r2, [r0]               @ r2 = current bit index
  ldr r1, =total_pdm_bits
  ldr r1, [r1]
  cmp r2, r1
  bhs .Ldone

  @ 4. read PDM bit: byte = pdm_out[bit_index >> 3]
  @    bit  = (byte >> (7 - (bit_index & 7))) & 1
  ldr r3, =pdm_out
  ldr r3, [r3]               @ r3 = buffer base pointer
  ldrb r3, [r3, r2, lsr #3]  @ r3 = pdm_out[bit_index / 8]
  and r4, r2, #7
  rsb r4, r4, #7             @ r4 = 7 - (bit_index & 7)
  mov r3, r3, lsr r4         @ shift bit into position 0
  tst r3, #1

  @ 5. GPIO pin 19: write GPSET0 or GPCLR0
  mov r4, #(1 << 19)         @ pin 19 mask
  ldr r5, =0x20200028        @ r5 = GPCLR0 (bit=0 case)
  subne r5, r5, #12          @ r5 = GPSET0 (bit=1: 0x28-0xC=0x1C)
  str r4, [r5]

  @ 6. increment pdm_bit_index
  add r2, r2, #1
  str r2, [r0]               @ r0 still holds &pdm_bit_index

.Ldone:
  @ debug: set GPIO pin 11 low (interrupt done)
  ldr r0, =0x20200028        @ GPCLR0
  mov r1, #(1 << 11)
  str r1, [r0]

  pop {r0-r5, lr}
  movs pc, lr

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ currently we don't use any of these, so just panic and
@ and halt.  should not occur!

reset_asm:
    @ Q: if we delete this instruction what happens?
    mov sp, #INT_STACK_ADDR   
    @ Q: is <4> correct?  how to tell?
    sub   r0, lr, #4
    bl    reset_vector

@ note: we compile this <.S> file with gcc 
@ so that we can use C macro's to clean up.
@ note the use of semi-colons!   Q: delete and what happens?
#define unexpected(fn, offset)      \
    mov sp, #INT_STACK_ADDR;        \
    sub   r0, lr, #(offset);        \
    bl    fn

@ Q: what are the right offsets for the following?
undefined_instruction_asm:                      @ A2-19
    unexpected(undefined_instruction_vector,4)  
software_interrupt_asm:                         @ A2-20
    unexpected(syscall_vector,4) 
prefetch_abort_asm:
    unexpected(prefetch_abort_vector,4)
data_abort_asm:
    unexpected(data_abort_vector,4)

@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@ simple demonstration of how to switch modes to get the
@ banked lr.
@
@ note from the documents that <SYSTEM> has the same 
@ registers as <USER>.  but, unlike user you can switch
@ back from it.
#define SUPER_MODE      0b10011     // A2-3
get_super_lr:
.globl get_super_lr
    mrs r1, cpsr
    msr cpsr_c, #SUPER_MODE
    mov r0, lr
    msr cpsr_c, r1
    bx lr
