/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 * Private MT7961/MT7972 interface. Do not share these registers with MT7925.
 */

/*
 * Internal contract between the MT7961 boot, RF, gain and capture modules.
 *
 * MT_ENTRY, MT_MAILBOX and the challenge constants must agree with the original
 * program in firmware/iq_stream.S. They describe RAM and a boot handshake,
 * not persistent device configuration. Physical RF SPI ports are named here to
 * avoid confusing receive chain 0 (port 1) with its companion synth (port 0).
 *
 * The caller owns the USB interface. Prepare configures hardware while the MCU
 * waits for GO; start follows queued USB reads; stop requests DMA drain. Reset
 * restores saved analog gain before resetting WFSYS. RF/gain access must not
 * race an active RF controller. These helpers are private so the public API
 * can enforce that lifecycle rather than expose uncoordinated register writes.
 * File-level comments in each implementation explain the hardware mechanisms
 * and the limits of the measurements supporting them.
 */
#ifndef BIRRIASDR_MT7921_H
#define BIRRIASDR_MT7921_H
#include "internal.h"

#define MT_MAILBOX 0x02038360u
#define MT_BOOT_MAGIC 0x41495242u
#define MT_RESPONSE_XOR 0xb1771a5du
#define MT_ENTRY 0x00915000u
#define MT_TOP_PORT 5u
#define MT_RX_PORT 1u
#define MT_SYNTH_PORT 0u

int mt_rf_read(birriasdr_dev_t *dev, unsigned port, uint32_t address, uint32_t *value);
int mt_rf_write(birriasdr_dev_t *dev, unsigned port, uint32_t address, uint32_t value);
int mt_rf_update(birriasdr_dev_t *dev, unsigned port, uint32_t address,
                 uint32_t clear, uint32_t set);
int mt_configure_rf(birriasdr_dev_t *dev);
int mt_apply_gain(birriasdr_dev_t *dev);
int mt_restore_gain(birriasdr_dev_t *dev);
int mt_configure_capture(birriasdr_dev_t *dev);
int mt_reset(birriasdr_dev_t *dev);
int mt_prepare(birriasdr_dev_t *dev, uint32_t packets);
int mt_start(birriasdr_dev_t *dev);
int mt_stop(birriasdr_dev_t *dev);
int mt_status(birriasdr_dev_t *dev, uint32_t words[16]);
#endif
