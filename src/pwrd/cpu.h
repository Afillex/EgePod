#pragma once

/* Apply all CPU power settings for DAP operation:
 *   - Hotplug A76 cores (cpu6, cpu7) offline
 *   - Set powersave governor on all remaining cores
 *   - Cap A55 max frequency to 1.2 GHz (ample for audio decode)
 *   - Disable GPU clock
 * Returns 0 on success, negative errno on failure. */
int cpu_apply_dap_policy(void);

/* Restore default online state (all CPUs online, performance governor).
 * Called on graceful shutdown. */
void cpu_restore_defaults(void);
