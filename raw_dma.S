/* SPDX-License-Identifier: MIT
 *
 * MIT License
 *
 * Copyright (c) 2026 Luigi C.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* BirriaSDR MT7961 volatile raw-I/Q DMA patch, matching mediatek-usb-sdr.yml.
 * Active profile: live capture, 8192-byte packets, 750 us polling guard,
 * 0x7fffffff packet limit, no sample conversion or CPU sample copying.
 * The flowgraph already embeds the compiled patch; assembly is optional.
 *
 * Rebuild with GNU Binutils 2.35.2 configured for nds32le-elf:
 *
 *   nds32le-elf-as -EL -march=v3 -mno-all-relax -o /tmp/mt7961.o raw_dma.S
 *   nds32le-elf-ld --no-relax -e producer -T /dev/stdin \
 *     -o /tmp/mt7961.elf /tmp/mt7961.o <<'EOF'
 *   SECTIONS {
 *     .entry 0x95c7a0 : { *(.entry) }
 *     .text  0x96c276 : { *(.text) }
 *   }
 *   EOF
 *   nds32le-elf-objcopy -O binary -j .entry /tmp/mt7961.elf /tmp/mt7961-entry.bin
 *   nds32le-elf-objcopy -O binary -j .text  /tmp/mt7961.elf /tmp/mt7961-text.bin
 *
 * The two sections total 790 bytes. PATCH_RECIPE in the flowgraph contains
 * their bytes at the above MCU addresses, with unchanged stock bytes retained
 * after each section. The entry must fit in 256 bytes, text in 604 bytes.
 * After editing, replace the corresponding prefixes of
 * PATCH_RECIPE's replacement hex strings; keep its expected stock bytes.
 * The Python patch_ram() function repairs the firmware region/container
 * checksums. Also update PATCH_RECIPE's firmware_sha256 to the SHA-256 of
 * that complete patched image. Only the pinned stock firmware is supported.
 * Original-profile patched firmware SHA-256:
 * c1e189a2a10a5e94c5d380cd6a34b7510d0869603596a60080a5d821befc5a7c
 */
    .set LIVE, 1
    .set PACKET_SHIFT, 13
    .set PACKETS, 0x7fffffff
    .set POLL_US, 750
    .set SOURCE_ADDRESS, 0xe0200000

/* Bounded raw SRAM export, entered instead of the stock ICAP dump.
 * No host DMA addresses, no radio TX. Returns after restoring TX0 accounting.
 * Mailbox reuses the replaced ICAP reader's state at GP + 0x3535c:
 * status (1 running, 2 done, 3 busy, 4 DMA timeout, 5 overrun, 6 source timeout,
 * 7 allocation failure, 8 ambiguous producer wrap, 9 host stop), packets,
 * elapsed us, DMA errors.
 * Stock ring accounting and interrupt state are restored before returning.
 */
    .ifdef DECIMATE
    .set COPY_OUTPUT, 1
    .set OUTPUT_BYTES, 2048
    .endif
    .ifdef CI8
    .set COPY_OUTPUT, 1
    .set OUTPUT_BYTES, 4096
    .endif
    .ifdef SYNTHETIC_TRANSPORT
    .set COPY_OUTPUT, 1
    .set OUTPUT_BYTES, 8192
    .endif
    .section .entry,"ax"
    .globl producer
    .macro load32 reg,value
    sethi \reg, (\value >> 12)
    ori \reg, \reg, (\value & 0xfff)
    .endm
producer:
    push25 $r10, 80
    .ifdef COPY_OUTPUT
    movi $r0, (OUTPUT_BYTES + 4)
    load32 $r2, 0x9178ea
    jral5 $r2
    swi $r0, [$sp + 64]
    beqz $r0, allocated
    lwi $r0, [$r0 + 12]
    swi $r0, [$sp + 68]
allocated:
    .endif
    mfsr $r0, $psw
    swi $r0, [$sp + 60]
    setgie.d
    dsb
    load32 $fp, 0x0203835c
    movi $r0, 1
    swi $r0, [$fp + 0]
    movi $r5, 0
    swi $r5, [$fp + 4]
    swi $r5, [$fp + 8]
    swi $r5, [$fp + 12]
    .ifdef COPY_OUTPUT
    lwi $r0, [$sp + 64]
    beqz $r0, allocation_failed
    .endif
    load32 $r6, 0x54000300
    lwi $r7, [$r6 + 0]
    lwi $r8, [$r6 + 8]
    lwi $r0, [$r6 + 12]
    bne $r0, $r8, busy
    lwi $r0, [$r6 + 4]
    movi $r1, 4
    bne $r0, $r1, busy
    /* Reclaim stock packets before temporarily taking the idle TX ring. */
    movi $r0, 0
    movi $r1, 0
    load32 $r2, 0x82e2b4
    jral5 $r2
    load32 $r0, 0x02015510
    lwi $r1, [$r0 + 0x30]
    swi $r1, [$sp + 56]
    /* One header, reused only after DMA consumed the previous packet. */
    movi $r0, 0
    addi $r1, $sp, 0
    movi $r2, 11
clear_header:
    swi.bi $r0, [$r1], 4
    addi $r2, $r2, -1
    bnez $r2, clear_header
    .ifdef COPY_OUTPUT
    load32 $r0, (0x3800002c + OUTPUT_BYTES)
    .else
    load32 $r0, (0x3800002c + (1 << PACKET_SHIFT))
    .endif
    swi $r0, [$sp + 0]
    .ifdef COPY_OUTPUT
    load32 $r0, (0x38000014 + OUTPUT_BYTES)
    .else
    load32 $r0, (0x38000014 + (1 << PACKET_SHIFT))
    .endif
    swi $r0, [$sp + 24]
    .if PACKET_SHIFT == 13
    j run
    .text
run:
    .endif
    .ifdef SYNTHETIC_TRANSPORT
    /* Fill an owned packet buffer once. Transport diagnostic only, no RF data
     * and no per-packet CPU sample copying or conversion. */
    lwi $r1, [$sp + 68]
    load32 $r0, 0xa5a50000
    movi $r2, 2049
fill_test_buffer:
    swi.bi $r0, [$r1], 4
    addi $r0, $r0, 1
    addi $r2, $r2, -1
    bnez $r2, fill_test_buffer
    .endif
    movi $r0, 0xed
    swi $r0, [$sp + 28]
    .ifdef CI8
    movi $r0, 0xf3
    .else
    movi $r0, 0xf1
    .endif
    swi $r0, [$sp + 32]
    load32 $r9, SOURCE_ADDRESS
    load32 $r0, 0x89020068
    lwi $r0, [$r0 + 0]
    swi $r0, [$sp + 52]
    .if PACKET_SHIFT == 14
    .ifdef LIVE
    swi $r0, [$sp + 72]
    .endif
    j run
    .text
run:
    .endif
    .ifdef SNAPSHOT
    /* Time a finite capture entirely on the MCU, then export stopped SRAM. */
    load32 $r4, 0x80021090
    movi $r0, SNAPSHOT_CONTROL
    swi $r0, [$r4 + 0]
    load32 $r1, 1000000
snapshot_wait:
    lwi $r0, [$r4 + 0]
    andi $r0, $r0, 2
    beqz $r0, snapshot_done
    addi $r1, $r1, -1
    bnez $r1, snapshot_wait
    movi $r0, 6
    j finish
snapshot_done:
    load32 $r0, 0x89020068
    lwi $r0, [$r0 + 0]
    lwi $r1, [$sp + 52]
    sub $r0, $r0, $r1
    swi $r0, [$sp + 44]
    .endif
    .ifdef LIVE
    .if PACKET_SHIFT == 13
    swi $r0, [$sp + 72]
    .endif
    .ifndef DECIMATE
    /* RX1 is the host-command ring. A new command requests a clean stop;
     * leave its descriptor untouched for the stock handler after returning. */
    lwi $r0, [$r6 + 0x21c]
    swi $r0, [$sp + 76]
    .endif
    .endif
    load32 $r10, PACKETS
    .ifdef LIVE
    movi $r0, 0
    swi $r0, [$sp + 44]
    swi $r0, [$sp + 48]
    load32 $r4, 0x80021090
    movi $r0, 0x24f3
    swi $r0, [$r4 + 0]
    addi $r4, $r4, 36
    .endif
publish:
    .ifdef LIVE
    load32 $r1, 1000000
wait_source:
    jal progress
    slti $r2, $r0, (1 << (PACKET_SHIFT - 2))
    beqz $r2, source_ready
    addi $r1, $r1, -1
    bnez $r1, wait_source
    movi $r10, -6
    j drained
source_ready:
    .endif
    .ifdef DECIMATE
    /* One word per 16-byte capture beat: chain 0, every second time sample.
     * Output keeps raw little-endian Q,I slots, with no amplitude conversion. */
    addi $r2, $r9, 0
    lwi $r3, [$sp + 68]
    movi $r1, 512
copy_samples:
    lwi.bi $r0, [$r2], 16
    swi.bi $r0, [$r3], 4
    addi $r1, $r1, -1
    bnez $r1, copy_samples
    .endif
    .ifdef CI8
    /* Every dense raw14 pair -> signed I,Q bytes. No time decimation.
     * Source remains untouched; an owned packet allocation holds the output.
     * This CPU loop is intentionally limited to stopped captures. */
    addi $r2, $r9, 0
    lwi $r3, [$sp + 68]
    movi $r1, 2048
quantize_samples:
    lwi.bi $r0, [$r2], 4
    srli $r4, $r0, 22
    srli $r0, $r0, 6
    andi $r0, $r0, 255
    or_slli $r0, $r4, $r0, 8
    shi.bi $r0, [$r3], 2
    addi $r1, $r1, -1
    bnez $r1, quantize_samples
    .endif
    swi $r5, [$sp + 36]
    slli $r0, $r5, PACKET_SHIFT
    swi $r0, [$sp + 40]
    slli $r3, $r8, 4
    add $r3, $r7, $r3
    swi $sp, [$r3 + 0]
    .ifdef COPY_OUTPUT
    lwi $r0, [$sp + 68]
    swi $r0, [$r3 + 8]
    .else
    swi $r9, [$r3 + 8]
    .endif
    movi $r0, 0
    swi $r0, [$r3 + 12]
    .ifdef COPY_OUTPUT
    load32 $r0, (0x002c4000 | (OUTPUT_BYTES + 4))
    .else
    .if PACKET_SHIFT == 13
    load32 $r0, 0x002c6004
    .else
    /* Continue the same packet in a second descriptor; no CPU sample copy. */
    load32 $r0, 0x002c2000
    swi $r0, [$r3 + 4]
    addi $r8, $r8, 1
    andi $r8, $r8, 3
    slli $r3, $r8, 4
    add $r3, $r7, $r3
    addi $r0, $r9, 8192
    swi $r0, [$r3 + 0]
    movi $r0, 0
    swi $r0, [$r3 + 8]
    swi $r0, [$r3 + 12]
    load32 $r0, 0x60040000
    .endif
    .endif
    swi $r0, [$r3 + 4]
    dsb
    addi $r8, $r8, 1
    andi $r8, $r8, 3
    swi $r8, [$r6 + 8]
    addi $r5, $r5, 1
    swi $r5, [$fp + 4]
    load32 $r1, 1000000
wait_dma:
    .ifdef LIVE
    jal progress
    .endif
    lwi $r0, [$r6 + 12]
    beq $r0, $r8, consumed
    addi $r1, $r1, -1
    bnez $r1, wait_dma
    movi $r0, 4
    j finish
consumed:
    addi $r9, $r9, 8192
    .if PACKET_SHIFT == 14
    addi $r9, $r9, 8192
    .endif
    load32 $r0, (SOURCE_ADDRESS + 0x40000)
    bne $r9, $r0, nowrap
    load32 $r9, SOURCE_ADDRESS
nowrap:
    addi $r10, $r10, -1
    bnez $r10, publish
drained:
    /* Every descriptor has completed. Drop our extra segment fields and
     * put the stock producer/reclaimer at the resulting ring position. */
    addi $r1, $r7, 0
    movi $r2, 4
    load32 $r0, 0xc0000000
    movi $r3, 0
clear_descriptors:
    swi $r0, [$r1 + 4]
    swi $r3, [$r1 + 8]
    addi $r1, $r1, 16
    addi $r2, $r2, -1
    bnez $r2, clear_descriptors
    load32 $r0, 0x02015510
    swi $r8, [$r0 + 0x2c]
    lwi $r1, [$sp + 56]
    add $r1, $r1, $r5
    swi $r1, [$r0 + 0x30]
    swi $r1, [$r0 + 0x34]
    movi $r0, 2
    bgez $r10, finish
    subri $r0, $r10, 0
    j finish
busy:
    movi $r0, 3
    j finish
    .ifdef COPY_OUTPUT
allocation_failed:
    movi $r0, 7
    .endif
finish:
    swi $r0, [$fp + 0]
    .ifdef LIVE
    /* Stop only after DMA completion on the successful path. */
    load32 $r1, 0x80021090
    movi $r2, 0x4f0
    swi $r2, [$r1 + 0]
    .endif
    .ifdef SNAPSHOT
    lwi $r0, [$sp + 44]
    .else
    load32 $r1, 0x89020068
    lwi $r0, [$r1 + 0]
    lwi $r2, [$sp + 52]
    sub $r0, $r0, $r2
    .endif
    swi $r0, [$fp + 8]
    load32 $r1, 0x54000110
    lwi $r0, [$r1 + 0]
    swi $r0, [$fp + 12]
    dsb
    lwi $r0, [$sp + 60]
    mtsr $r0, $psw
    isb
    .ifdef COPY_OUTPUT
    lwi $r0, [$sp + 64]
    beqz $r0, freed
    movi $r1, 1
    load32 $r2, 0x91793e
    jral5 $r2
freed:
    .endif
    movi $r0, 0
    pop25 $r10, 80
    .ifdef LIVE
/* Must run during DMA waits too: observe wraps before the producer can lap.
 * r0 returns unread capture units; r2/r3 are scratch, r1 timeout is preserved.
 * A safety margin prevents DMA from reading a chunk being overwritten. */
progress:
    .ifndef DECIMATE
    lwi $r2, [$r6 + 0x21c]
    lwi $r3, [$sp + 76]
    bne $r2, $r3, host_stop
    /* A missed complete turn of the ring is otherwise invisible. POLL_US must
     * remain below the ring period for the selected capture rate. The default
     * 20 MSps packed mode turns the ring in 1638.4 us; faster probes tighten
     * this guard in the host-side builder. */
    load32 $r2, 0x89020068
    lwi $r2, [$r2 + 0]
    lwi $r3, [$sp + 72]
    swi $r2, [$sp + 72]
    sub $r2, $r2, $r3
    slti $r3, $r2, POLL_US
    beqz $r3, timing_fault
    .endif
    lwi $r0, [$r4 + 0]
    lwi $r2, [$sp + 44]
    swi $r0, [$sp + 44]
    sub $r0, $r0, $r2
    zeh33 $r0, $r0
    lwi $r2, [$sp + 48]
    add $r0, $r0, $r2
    swi $r0, [$sp + 48]
    slli $r2, $r5, (PACKET_SHIFT - 2)
    sub $r0, $r0, $r2
    movi $r3, 61440
    slt $r2, $r0, $r3
    bnez $r2, progress_ok
    movi $r10, -5
fault_drain:
    /* Finish an already published descriptor while capture is still active. */
    load32 $r1, 1000000
overrun_drain:
    lwi $r0, [$r6 + 12]
    beq $r0, $r8, overrun_drained
    addi $r1, $r1, -1
    bnez $r1, overrun_drain
    movi $r0, 4
    j finish
overrun_drained:
    j drained
    .ifndef DECIMATE
timing_fault:
    movi $r10, -8
    j fault_drain
host_stop:
    movi $r10, -9
    j fault_drain
    .endif
progress_ok:
    ret5 $lp
    .endif
