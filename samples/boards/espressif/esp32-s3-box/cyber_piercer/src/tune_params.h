/*
 * tune_params.h — 运行时可调参数（共享定义）
 *
 * main.c 定义这些变量, tune_shell.c 通过 extern 访问并修改。
 * 舵机参数单位为微秒 (μs)，由 PWM_USEC() 宏转换为纳秒；
 * 控制/发射参数单位为毫秒 (ms)。
 */
#ifndef TUNE_PARAMS_H_
#define TUNE_PARAMS_H_

#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>

/* ── 参数结构体 ── */
struct tune_params {
	/* 舵机中点 (ns) */
	int32_t h_center;
	int32_t v_center;

	/* 水平映射范围 (ns) */
	int32_t h_left;      /* cx=0 时脉宽 */
	int32_t h_right;     /* cx=FRAME_W 时脉宽 */

	/* 垂直映射范围 (ns) */
	int32_t v_top;       /* cy=0 时脉宽 */
	int32_t v_bottom;    /* cy=FRAME_H 时脉宽 */

	/* 视差补偿 (μs, 存储时乘1000转ns) */
	int32_t h_offset;    /* μs */
	int32_t v_offset;    /* μs */

	/* 控制参数 */
	int32_t slew_rate;   /* μs/cycle */
	int32_t interval_ms; /* ms */
	int32_t ema_num;     /* EMA 分子 */
	int32_t ema_den;     /* EMA 分母 */

	/* 发射参数 */
	int32_t fire_cooldown_ms;
	int32_t fire_pulse_ms;
};

/* 全局参数实例 (main.c 中定义) */
extern volatile struct tune_params g_tune;

/* 参数 ID 枚举 (用于 shell 命令) */
enum tune_param_id {
	TUNE_H_CENTER,
	TUNE_V_CENTER,
	TUNE_H_LEFT,
	TUNE_H_RIGHT,
	TUNE_V_TOP,
	TUNE_V_BOTTOM,
	TUNE_H_OFFSET,
	TUNE_V_OFFSET,
	TUNE_SLEW_RATE,
	TUNE_INTERVAL,
	TUNE_EMA_NUM,
	TUNE_EMA_DEN,
	TUNE_FIRE_COOLDOWN,
	TUNE_FIRE_PULSE,
	TUNE_PARAM_COUNT,
};

/* 参数名 ↔ ID 映射表 */
struct tune_param_entry {
	const char *name;
	enum tune_param_id id;
	int32_t min_val;
	int32_t max_val;
	const char *unit;
};

extern const struct tune_param_entry tune_param_table[TUNE_PARAM_COUNT];

/* 遥测开关 (tune_shell.c 中定义) */
extern volatile bool g_telem_enabled;

/* 立即应用中点参数到舵机 (main.c 中定义) */
void tune_servo_apply_center(void);

/* NVS 持久化 (tune_shell.c 中定义) */
int tune_params_save(void);
int tune_params_load(void);

#endif /* TUNE_PARAMS_H_ */
