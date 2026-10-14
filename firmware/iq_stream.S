/*
MIT License

Copyright (c) 2026 Luigi C.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
.equ MAILBOX, 0x2038360
.equ BOOT_MAGIC, 0x41495242
.equ RESPONSE_XOR, 0xb1771a5d
.equ RING, 0x2040000
.equ HEADERS, 0x2040100
.equ PAYLOAD, 0x2041000
.equ SLOT_STRIDE, 0x3000
.equ RING_COUNT, 0x4
.equ PAYLOAD_BYTES, 0x2000
/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C. See LICENSE.
 * Original continuous ICAP SRAM -> serial USB DMA diagnostic.
 * No downloaded MediaTek firmware; ROM PHY power-on only.
 * Extra mailbox words: 16 produced units, 17 final ICAP control,
 * 18 final producer, 19 power-on return, 20 maximum backlog (units),
 * 21 maximum producer poll interval (microseconds).
 * Additional terminal states: 8 ambiguous wrap, 9 source overrun,
 * 10 source timeout. All stop capture and drain the DMA descriptor.
 * RF tuning/calibration are NOT implemented by this diagnostic.
 */
    .set IQ_STREAM, 1
    /* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C. See LICENSE.
 *
 * Original standalone synthetic USB DMA producer. No ROM helper calls,
 * allocator, instruction tables, vendor RAM image or radio initialization.
 * Uses dedicated DLM descriptors, headers and payloads, outside ROM workspace.
 * The immutable ROM handles enumeration/download before this entry.
 *
 * Mailbox words: magic, nonce, answer, PSW, SP, GP, LP, state,
 * packet limit (0=until stop), stop, submitted, completed, ticks, DMA errors,
 * initial timer, go. State: 1 ready/running, 2 finite done, 3 host stop,
 * 4 DMA timeout, 5 start timeout, 6 initial DMA busy.
 * At most three packets are outstanding; every buffer is reused only after DMA
 * completes it. No interrupts or vendor software ring accounting are used.
 * After completion this program parks until a host WFSYS reset.
 */
    .section .text.entry,"ax"
    .global _start
    .type _start, @function
    .macro load32 reg,value
    sethi \reg, (\value >> 12)
    ori \reg, \reg, (\value & 0xfff)
    .endm
    .ifdef IQ_SNAPSHOT
    .set IQ_CAPTURE, 1
    .endif
    .ifdef IQ_STREAM
    .set IQ_CAPTURE, 1
    /* Observe the live capture even while USB is stalled. One address unit is
     * one complex sample; the SRAM ring contains 65536 units. Scratch r0/r1/r3,
     * preserve the DMA wait counter (r2) and all ring/buffer registers. */
    .macro observe_source
    load32 $r0, 0x89020068
    lwi $r0, [$r0]
    lwi $r1, [$fp + 88]
    swi $r0, [$fp + 88]
    sub $r1, $r0, $r1
    lwi $r3, [$fp + 84]
    slt $r3, $r3, $r1
    beqz $r3, 90f
    swi $r1, [$fp + 84]
90:
    slti $r3, $r1, 750
    beqz $r3, timing_fault
    load32 $r0, 0x800210b4
    lwi $r0, [$r0]
    zeh33 $r0, $r0
    lwi $r1, [$fp + 72]
    swi $r0, [$fp + 72]
    sub $r1, $r0, $r1
    zeh33 $r1, $r1
    lwi $r0, [$fp + 64]
    add $r0, $r0, $r1
    swi $r0, [$fp + 64]
    lwi $r1, [$fp + 92]
    sub $r0, $r0, $r1
    lwi $r3, [$fp + 80]
    slt $r3, $r3, $r0
    beqz $r3, 91f
    swi $r0, [$fp + 80]
91:
    movi $r3, 61440
    slt $r3, $r0, $r3
    beqz $r3, source_overrun
    .endm
    .endif
_start:
    load32 $fp, MAILBOX
    mfsr $r0, $psw
    swi $r0, [$fp + 12]
    swi $sp, [$fp + 16]
    swi $gp, [$fp + 20]
    swi $lp, [$fp + 24]
    lwi $r0, [$fp + 4]
    load32 $r1, RESPONSE_XOR
    xor $r0, $r0, $r1
    swi $r0, [$fp + 8]
    .ifdef IQ_CAPTURE
    /* Capture variants use the immutable ROM only for PHY power-on. */
    load32 $r2, 0x00837142
    jral5 $r2
    swi $r0, [$fp + 76]
    .endif
    load32 $r6, 0x54000300
    load32 $r7, RING
    /* ROM handoff disables both directions. Only take an idle engine. */
    lwi $r0, [$r6 - 248]
    andi $r1, $r0, 10
    bnez $r1, initial_busy
    bitci $r0, $r0, 5
    bclr $r0, $r0, 30
    swi $r0, [$r6 - 248]
    movi $r0, 0
    swi $r0, [$r6 - 252]  /* 0x54000204: mask DMA interrupts */
    /* Clear all our ring slots and 64-byte event headers. */
    load32 $r1, RING
    movi $r2, (RING_COUNT * 4)
clear_ring:
    swi.bi $r0, [$r1], 4
    addi $r2, $r2, -1
    bnez $r2, clear_ring
    load32 $r1, HEADERS
    movi $r2, (RING_COUNT * 16)
clear_headers:
    swi.bi $r0, [$r1], 4
    addi $r2, $r2, -1
    bnez $r2, clear_headers
    /* Pattern is challenge-specific. First/last payload words change per packet. */
    load32 $r8, HEADERS
    load32 $r9, PAYLOAD
    movi $r10, RING_COUNT
initialize_slot:
    load32 $r0, (0x3800002c + PAYLOAD_BYTES)
    swi $r0, [$r8 + 0]
    load32 $r0, (0x38000014 + PAYLOAD_BYTES)
    swi $r0, [$r8 + 24]
    movi $r0, 0xed
    swi $r0, [$r8 + 28]
    .ifdef IQ_CAPTURE
    movi $r0, 0xf5
    .else
    movi $r0, 0xf4
    .endif
    swi $r0, [$r8 + 32]
    lwi $r0, [$fp + 4]
    swi $r0, [$r8 + 40]
    .ifndef IQ_CAPTURE
    load32 $r1, 0xa5a50000
    xor $r0, $r0, $r1
    addi $r1, $r9, 0
    movi $r2, (PAYLOAD_BYTES / 4)
fill_payload:
    swi.bi $r0, [$r1], 4
    addi $r0, $r0, 1
    addi $r2, $r2, -1
    bnez $r2, fill_payload
    movi $r0, 0
    swi $r0, [$r1]       /* USB trailer, inside our allocation */
    .endif
    addi $r8, $r8, 64
    addi $r9, $r9, SLOT_STRIDE
    addi $r10, $r10, -1
    bnez $r10, initialize_slot
    dsb
    swi $r7, [$r6 + 0]
    movi $r0, RING_COUNT
    swi $r0, [$r6 + 4]
    movi $r0, 1
    swi $r0, [$r6 - 244] /* 0x5400020c: reset TX0 DMA index */
    movi $r0, 0
    swi $r0, [$r6 + 8]
    lwi $r0, [$r6 - 248]
    ori $r0, $r0, 1      /* TX enabled; RX remains disabled */
    swi $r0, [$r6 - 248]
    movi $r0, 1
    swi $r0, [$fp + 28]
    dsb
    load32 $r0, BOOT_MAGIC
    swi $r0, [$fp]
    dsb
    movi $r4, 0          /* sequence */
    movi $r5, 0          /* producer index */
    load32 $r2, 100000000
wait_go:
    lwi $r0, [$fp + 60]
    bnez $r0, begin_stream
    lwi $r0, [$fp + 36]
    bnez $r0, stopped
    addi $r2, $r2, -1
    bnez $r2, wait_go
    movi $r0, 5
    j finish
begin_stream:
    .ifdef IQ_SNAPSHOT
    /* A finite snapshot cannot read beyond its 256 KiB allocation. */
    movi $r0, 32
    swi $r0, [$fp + 32]
    .endif
    load32 $r0, 0x89020068
    lwi $r0, [$r0]
    swi $r0, [$fp + 56]
    .ifdef IQ_STREAM
    swi $r0, [$fp + 88]
    movi $r0, 0
    swi $r0, [$fp + 64]  /* produced address units, modulo 2^32 */
    swi $r0, [$fp + 72]  /* last 16-bit producer index */
    swi $r0, [$fp + 92]  /* completed address units, modulo 2^32 */
    swi $r0, [$fp + 80]
    swi $r0, [$fp + 84]
    load32 $r0, 0x80021090
    movi $r1, 0x24f3
    swi $r1, [$r0]
    .endif
    .ifdef IQ_SNAPSHOT
    load32 $r0, 0x80021090
    movi $r1, 0x4f3
    swi $r1, [$r0]
    load32 $r2, 1000000
snapshot_wait:
    lwi $r1, [$r0]
    andi $r1, $r1, 2
    beqz $r1, snapshot_ready
    addi $r2, $r2, -1
    bnez $r2, snapshot_wait
    movi $r1, 0x4f1
    swi $r1, [$r0]
    movi $r0, 7
    j finish
snapshot_ready:
    load32 $r0, 0x89020068
    lwi $r0, [$r0]
    lwi $r1, [$fp + 56]
    sub $r0, $r0, $r1
    swi $r0, [$fp + 64]
    load32 $r0, 0x80021090
    lwi $r1, [$r0]
    swi $r1, [$fp + 68]
    lwi $r1, [$r0 + 36]
    swi $r1, [$fp + 72]
    .endif
publish:
    lwi $r0, [$fp + 36]
    bnez $r0, stopped
    lwi $r0, [$fp + 32]
    beqz $r0, next_packet
    beq $r4, $r0, done
next_packet:
    /* Keep one ring slot empty to distinguish full from empty. Hardware can
     * consume up to three descriptors between observations without ambiguity. */
    addi $r10, $r5, 1
    andi $r10, $r10, (RING_COUNT - 1)
    load32 $r2, 100000000
wait_space:
    lwi $r0, [$r6 + 12]
    sub $r1, $r5, $r0
    andi $r1, $r1, (RING_COUNT - 1)
    sub $r1, $r4, $r1
    swi $r1, [$fp + 44]
    .ifdef IQ_STREAM
    slli $r1, $r1, 11
    swi $r1, [$fp + 92]
    observe_source
    lwi $r0, [$r6 + 12]
    .endif
    .ifdef IQ_CAPTURE
    /* SRAM reads require serial descriptors on this part. Multiple queued
     * SRAM packets have reproduced silent payload corruption. */
    beq $r0, $r5, space_ready
    .else
    bne $r0, $r10, space_ready
    .endif
    lwi $r0, [$fp + 36]
    bnez $r0, stopped
    addi $r2, $r2, -1
    bnez $r2, wait_space
    movi $r0, 4
    j finish
space_ready:
    .ifdef IQ_STREAM
    slli $r1, $r4, 11
    swi $r1, [$fp + 92]
    load32 $r2, 1000000
wait_source:
    lwi $r0, [$fp + 36]
    bnez $r0, stopped
    observe_source
    slti $r3, $r0, (PAYLOAD_BYTES / 4)
    beqz $r3, source_ready
    addi $r2, $r2, -1
    bnez $r2, wait_source
    movi $r10, 10
    j drain
source_ready:
    .endif
    slli $r0, $r5, 6
    load32 $r8, HEADERS
    add $r8, $r8, $r0
    .ifdef IQ_CAPTURE
    /* Direct DMA from SRAM: no CPU copying or sample conversion.
     * USB consumes four extra trailer bytes, discarded by the host. The
     * last packet reads these from the adjacent, mapped SRAM word. */
    andi $r0, $r4, 31
    slli $r0, $r0, 13
    load32 $r9, 0xe0200000
    add $r9, $r9, $r0
    .else
    /* SLOT_STRIDE=0x3000; calculate without a runtime multiply helper. */
    slli $r0, $r5, 12
    slli $r1, $r5, 13
    add $r0, $r0, $r1
    load32 $r9, PAYLOAD
    add $r9, $r9, $r0
    .endif
    swi $r4, [$r8 + 36]
    .ifndef IQ_CAPTURE
    swi $r4, [$r9 + 0]
    nor $r0, $r4, $r4
    swi $r0, [$r9 + (PAYLOAD_BYTES - 4)]
    .endif
    slli $r0, $r5, 4
    add $r0, $r7, $r0
    swi $r8, [$r0]
    swi $r9, [$r0 + 8]
    movi $r1, 0
    swi $r1, [$r0 + 12]
    load32 $r1, ((44 << 16) | (1 << 14) | (PAYLOAD_BYTES + 4))
    swi $r1, [$r0 + 4]
    dsb
    addi $r5, $r10, 0
    swi $r5, [$r6 + 8]
    addi $r4, $r4, 1
    swi $r4, [$fp + 40]
    j publish
done:
    movi $r10, 2
    j drain
stopped:
    movi $r10, 3
drain:
    /* Drain before disabling the capture SRAM path. A detected overrun is
     * always a failed stream, including any final descriptor being drained. */
    load32 $r2, 100000000
wait_dma:
    lwi $r0, [$r6 + 12]
    beq $r0, $r5, completed
    addi $r2, $r2, -1
    bnez $r2, wait_dma
    movi $r0, 4
    j finish
completed:
    swi $r4, [$fp + 44]
    addi $r0, $r10, 0
    j finish
initial_busy:
    movi $r0, 6
finish:
    addi $r10, $r0, 0
    .ifdef IQ_STREAM
    load32 $r0, 0x80021090
    movi $r1, 0x4f0
    swi $r1, [$r0]
    lwi $r1, [$r0]
    swi $r1, [$fp + 68]
    lwi $r1, [$r0 + 36]
    lwi $r2, [$fp + 72]
    swi $r1, [$fp + 72]
    sub $r1, $r1, $r2
    zeh33 $r1, $r1
    lwi $r2, [$fp + 64]
    add $r1, $r1, $r2
    swi $r1, [$fp + 64]
    .endif
    load32 $r0, 0x89020068
    lwi $r0, [$r0]
    lwi $r1, [$fp + 56]
    sub $r0, $r0, $r1
    swi $r0, [$fp + 48]
    lwi $r0, [$r6 - 496] /* 0x54000110: DMA errors */
    swi $r0, [$fp + 52]
    dsb
    swi $r10, [$fp + 28] /* Publish terminal state after all result fields. */
    dsb
park:
    j park
    .ifdef IQ_STREAM
timing_fault:
    movi $r10, 8
    j drain
source_overrun:
    movi $r10, 9
    j drain
    .endif
    .size _start, .-_start

