/*
 * log_wrap.c — ld --wrap targets for nofrendo log_init/log_shutdown.
 *
 * The CMakeLists.txt passes `-Wl,--wrap=log_init -Wl,--wrap=log_shutdown`
 * so the linker rewrites nofrendo's undefined references to log_init /
 * log_shutdown into __wrap_log_init / __wrap_log_shutdown. Without this
 * wrapper the linker would resolve nofrendo's call sites to Zephyr
 * subsys/logging's `void log_init(void)` (CONFIG_LOG=y), which has the
 * wrong return type and causes nofrendo_main() to bail out with -1.
 */

int __wrap_log_init(void)
{
	return 0;
}

void __wrap_log_shutdown(void)
{
}
