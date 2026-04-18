/*
 * Linux-style dmesg boot splash for CHD-ESP32-S3-BOX launcher.
 *
 * Renders a fullscreen black textarea and drips out fake dmesg-style log
 * lines for ~2.5s before returning. Display must be powered on (blanking
 * off) BEFORE calling this so the user sees the boot text scroll.
 */
#ifndef BOOT_SPLASH_H
#define BOOT_SPLASH_H

#include <zephyr/device.h>

/* Blocking. Returns once the splash is done and removed. */
void boot_splash_run(const struct device *display);

#endif /* BOOT_SPLASH_H */
