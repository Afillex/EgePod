#pragma once

/* Block all rfkill-managed radios (BT, Wi-Fi, NFC, modem).
 * Uses /dev/rfkill ioctl; does not require each driver to be loaded.
 * Returns number of radios blocked, or -1 on error. */
int rfkill_block_all(void);

/* Unblock all radios (for graceful shutdown / recovery). */
void rfkill_unblock_all(void);
