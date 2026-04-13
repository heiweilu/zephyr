/*
 * NES Emulator — main entry point
 *
 * Board:  CHD-ESP32-S3-BOX (ESP32-S3, 16 MB Flash, 16 MB OctalPSRAM)
 * RTOS:   Zephyr v4.2
 *
 * Flow:
 *   main()
 *     └─ nofrendo_main(0, NULL)           [never returns]
 *           └─ osd_main()                 [our impl in osd.c]
 *                 └─ main_loop("osd:", system_nes)
 *                       ├─ osd_init()     [display ready]
 *                       ├─ osd_getvideoinfo() → vid_init() → zephyr_vid_init()
 *                       ├─ osd_installtimer(60, ...) → k_timer at 60 Hz
 *                       └─ nes_emulate()  [emulation loop, calls osd_getinput
 *                                          and zephyr_custom_blit each frame]
 *
 * Include nofrendo types BEFORE Zephyr headers (see osd.c for explanation).
 */

#include <noftypes.h>
#include <nofrendo.h>
/* Undo memguard.h macro redefinitions (same as memguard.c itself does) */
#undef malloc
#undef free
#undef strdup

#include <zephyr/kernel.h>

int main(void)
{
	printk("=== NES Emulator starting (nofrendo / esp32-nesemu) ===\n");

	/*
	 * nofrendo_main() → osd_main() → main_loop() — does not return
	 * under normal emulation.
	 */
	int rc = nofrendo_main(0, NULL);

	printk("NES Emulator exited with code %d\n", rc);
	return rc;
}
