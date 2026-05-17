/*
 * tune_shell.c — Zephyr Shell 命令：运行时参数调整 + 遥测输出 + NVS 持久化
 *
 * 命令:
 *   tune get [param]      — 查询参数（省略 param 则显示全部）
 *   tune set <param> <val> — 设置参数
 *   tune apply            — 立即将 center 值应用到舵机
 *   tune telem <on|off>   — 开启/关闭遥测输出
 *   tune save             — 保存全部参数到 NVS flash
 */

#include <zephyr/shell/shell.h>
#include <zephyr/settings/settings.h>
#include <stdlib.h>
#include <string.h>
#include "tune_params.h"

/* 遥测开关 */
volatile bool g_telem_enabled;

/* 前向声明 */
static volatile int32_t *param_ptr(enum tune_param_id id);

/* ── NVS Settings handler ── */

static int tune_settings_set(const char *name, size_t len,
			     settings_read_cb read_cb, void *cb_arg)
{
	for (int i = 0; i < TUNE_PARAM_COUNT; i++) {
		if (strcmp(name, tune_param_table[i].name) == 0) {
			int32_t val;

			if (len != sizeof(val)) {
				return -EINVAL;
			}
			int rc = read_cb(cb_arg, &val, sizeof(val));

			if (rc < 0) {
				return rc;
			}
			/* 范围校验: 防止 NVS 损坏数据加载越界值 */
			if (val < tune_param_table[i].min_val ||
			    val > tune_param_table[i].max_val) {
				return -EINVAL;
			}
			volatile int32_t *p = param_ptr(tune_param_table[i].id);

			if (p) {
				*p = val;
			}
			return 0;
		}
	}
	return -ENOENT;
}

static int tune_settings_export(int (*cb)(const char *name,
					  const void *value, size_t val_len))
{
	for (int i = 0; i < TUNE_PARAM_COUNT; i++) {
		volatile int32_t *p = param_ptr(tune_param_table[i].id);

		if (p) {
			char key[32];
			int32_t val = *p;

			snprintf(key, sizeof(key), "tune/%s",
				 tune_param_table[i].name);
			(void)cb(key, &val, sizeof(val));
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tune, "tune", NULL,
			       tune_settings_set, NULL,
			       tune_settings_export);

int tune_params_save(void)
{
	return settings_save();
}

int tune_params_load(void)
{
	return settings_load_subtree("tune");
}

/* ── 参数读写辅助 ── */

static volatile int32_t *param_ptr(enum tune_param_id id)
{
	switch (id) {
	case TUNE_H_CENTER:      return &g_tune.h_center;
	case TUNE_V_CENTER:      return &g_tune.v_center;
	case TUNE_H_LEFT:        return &g_tune.h_left;
	case TUNE_H_RIGHT:       return &g_tune.h_right;
	case TUNE_V_TOP:         return &g_tune.v_top;
	case TUNE_V_BOTTOM:      return &g_tune.v_bottom;
	case TUNE_H_OFFSET:      return &g_tune.h_offset;
	case TUNE_V_OFFSET:      return &g_tune.v_offset;
	case TUNE_SLEW_RATE:     return &g_tune.slew_rate;
	case TUNE_INTERVAL:      return &g_tune.interval_ms;
	case TUNE_EMA_NUM:       return &g_tune.ema_num;
	case TUNE_EMA_DEN:       return &g_tune.ema_den;
	case TUNE_FIRE_COOLDOWN: return &g_tune.fire_cooldown_ms;
	case TUNE_FIRE_PULSE:    return &g_tune.fire_pulse_ms;
	default:                 return NULL;
	}
}

static const struct tune_param_entry *find_param(const char *name)
{
	for (int i = 0; i < TUNE_PARAM_COUNT; i++) {
		if (strcmp(tune_param_table[i].name, name) == 0) {
			return &tune_param_table[i];
		}
	}
	return NULL;
}

/* ── shell 命令: tune get ── */
static int cmd_tune_get(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2) {
		/* tune get <param> */
		const struct tune_param_entry *e = find_param(argv[1]);

		if (!e) {
			shell_error(sh, "Unknown param: %s", argv[1]);
			return -EINVAL;
		}
		volatile int32_t *p = param_ptr(e->id);

		shell_print(sh, "$P %s=%d %s", e->name, (int)*p, e->unit);
	} else {
		/* tune get — 显示全部, 每行一个, 用 $P 前缀方便机器解析 */
		for (int i = 0; i < TUNE_PARAM_COUNT; i++) {
			volatile int32_t *p = param_ptr(tune_param_table[i].id);

			shell_print(sh, "$P %s=%d %s",
				    tune_param_table[i].name, (int)*p,
				    tune_param_table[i].unit);
		}
	}
	return 0;
}

/* ── shell 命令: tune set ── */
static int cmd_tune_set(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_error(sh, "Usage: tune set <param> <value>");
		return -EINVAL;
	}

	const struct tune_param_entry *e = find_param(argv[1]);

	if (!e) {
		shell_error(sh, "Unknown param: %s", argv[1]);
		return -EINVAL;
	}

	char *endptr;
	long val = strtol(argv[2], &endptr, 10);

	if (*endptr != '\0') {
		shell_error(sh, "Invalid number: %s", argv[2]);
		return -EINVAL;
	}

	if (val < e->min_val || val > e->max_val) {
		shell_error(sh, "%s: value %ld out of range [%d, %d]",
			    e->name, val, e->min_val, e->max_val);
		return -ERANGE;
	}

	/* EMA 交叉校验: ema_num 必须 < ema_den */
	if (e->id == TUNE_EMA_NUM && val >= g_tune.ema_den) {
		shell_error(sh, "ema_num(%ld) must be < ema_den(%d)",
			    val, (int)g_tune.ema_den);
		return -ERANGE;
	}
	if (e->id == TUNE_EMA_DEN && val <= g_tune.ema_num) {
		shell_error(sh, "ema_den(%ld) must be > ema_num(%d)",
			    val, (int)g_tune.ema_num);
		return -ERANGE;
	}

	volatile int32_t *p = param_ptr(e->id);
	*p = (int32_t)val;

	shell_print(sh, "$OK %s=%d %s", e->name, (int)*p, e->unit);

	/* 如果改的是 center 参数，立即应用到舵机 */
	if (e->id == TUNE_H_CENTER || e->id == TUNE_V_CENTER) {
		tune_servo_apply_center();
		shell_print(sh, "$APPLIED center → servo");
	}

	return 0;
}

/* ── shell 命令: tune apply ── */
static int cmd_tune_apply(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	tune_servo_apply_center();
	shell_print(sh, "$APPLIED h=%d v=%d", g_tune.h_center, g_tune.v_center);
	return 0;
}

/* ── shell 命令: tune save ── */
static int cmd_tune_save(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int rc = tune_params_save();

	if (rc) {
		shell_error(sh, "$ERR save failed: %d", rc);
	} else {
		shell_print(sh, "$SAVED %d params to NVS", TUNE_PARAM_COUNT);
	}
	return rc;
}

/* ── shell 命令: tune telem ── */
static int cmd_tune_telem(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "telem=%s", g_telem_enabled ? "on" : "off");
		return 0;
	}
	if (strcmp(argv[1], "on") == 0) {
		g_telem_enabled = true;
		shell_print(sh, "$TELEM on");
	} else if (strcmp(argv[1], "off") == 0) {
		g_telem_enabled = false;
		shell_print(sh, "$TELEM off");
	} else {
		shell_error(sh, "Usage: tune telem <on|off>");
		return -EINVAL;
	}
	return 0;
}

/* ── 子命令注册 ── */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_tune,
	SHELL_CMD_ARG(get, NULL, "Get param(s): tune get [name]",
		      cmd_tune_get, 1, 1),
	SHELL_CMD_ARG(set, NULL, "Set param: tune set <name> <value>",
		      cmd_tune_set, 3, 0),
	SHELL_CMD_ARG(apply, NULL, "Apply center to servos now",
		      cmd_tune_apply, 1, 0),
	SHELL_CMD_ARG(save, NULL, "Save all params to NVS flash",
		      cmd_tune_save, 1, 0),
	SHELL_CMD_ARG(telem, NULL, "Telemetry: tune telem <on|off>",
		      cmd_tune_telem, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(tune, &sub_tune, "Runtime parameter tuning", NULL);
