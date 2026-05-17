/* cyber_piercer — 炮台控制主程序
 *
 * 功能:
 *   1) 上电后舵机回到默认位置 (H: 1500μs, V: 1463μs)
 *   2) 水平舵机: 自动左右扫描 (±45°) 或追踪模式
 *   3) 垂直舵机: 保持校准零点 (SERVO_V_CENTER=1463μs, 补偿上偏5°)
 *   4) 按 BOOT 键: 发射一次电磁推杆
 *   5) 串口打印实时角度 / 状态
 *
 * ENABLE_TRACKING=1: WiFi 连接 → PC server → 追踪目标
 * ENABLE_TRACKING=0: 使用原有的自动扫描模式
 *
 * Phase 1 测试: 仅水平舵机 — 注释 ENABLE_SERVO_V 和 ENABLE_SOLENOID
 * Phase 2 测试: 仅垂直舵机 — 注释 ENABLE_SERVO_H 和 ENABLE_SOLENOID
 * Phase 3 测试: 双舵机     — 注释 ENABLE_SOLENOID
 * Phase 4 测试: 仅推杆     — 注释 ENABLE_SERVO_H 和 ENABLE_SERVO_V
 * Phase 5 测试: 全部启用
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

/* ── 阶段开关 (必须在条件 include 之前) ── */
#define ENABLE_SERVO_H    1   /* 水平舵机 */
#define ENABLE_SERVO_V    1   /* 垂直舵机 */
#define ENABLE_SOLENOID   0   /* 电磁推杆 (Phase 4+: 打开) */
#define ENABLE_TRACKING   0   /* 追踪模式 (需要 WiFi + PC server) */
#define CALIBRATION_MODE  1   /* 校准模式: 上电归零点后静止, 不扫描 */

#if ENABLE_TRACKING
#include "wifi_mgr.h"
#include "track_client.h"
#include "secrets.h"
#endif

#include "tune_params.h"

LOG_MODULE_REGISTER(cyber_piercer, LOG_LEVEL_INF);

/* ── 运行时可调参数 (初始值 = 原 #define 值) ── */
volatile struct tune_params g_tune = {
	.h_center       = 1722,   /* μs */
	.v_center       = 1389,   /* μs */
	.h_left         = 2200,   /* μs */
	.h_right        = 1200,   /* μs */
	.v_top          = 1700,   /* μs */
	.v_bottom       = 1100,   /* μs */
	.h_offset       = 22,     /* μs */
	.v_offset       = -74,    /* μs */
	.slew_rate      = 100,    /* μs/cycle */
	.interval_ms    = 20,     /* ms */
	.ema_num        = 3,
	.ema_den        = 20,
	.fire_cooldown_ms = 500,  /* ms */
	.fire_pulse_ms  = 80,     /* ms */
};

/* 参数名表 */
const struct tune_param_entry tune_param_table[TUNE_PARAM_COUNT] = {
	[TUNE_H_CENTER]      = {"h_center",      TUNE_H_CENTER,      500, 2500, "us"},
	[TUNE_V_CENTER]      = {"v_center",      TUNE_V_CENTER,      500, 2500, "us"},
	[TUNE_H_LEFT]        = {"h_left",        TUNE_H_LEFT,        500, 2500, "us"},
	[TUNE_H_RIGHT]       = {"h_right",       TUNE_H_RIGHT,       500, 2500, "us"},
	[TUNE_V_TOP]         = {"v_top",         TUNE_V_TOP,         500, 2500, "us"},
	[TUNE_V_BOTTOM]      = {"v_bottom",      TUNE_V_BOTTOM,      500, 2500, "us"},
	[TUNE_H_OFFSET]      = {"h_offset",      TUNE_H_OFFSET,     -500,  500, "us"},
	[TUNE_V_OFFSET]      = {"v_offset",      TUNE_V_OFFSET,     -500,  500, "us"},
	[TUNE_SLEW_RATE]     = {"slew_rate",     TUNE_SLEW_RATE,      10,  500, "us/cyc"},
	[TUNE_INTERVAL]      = {"interval",      TUNE_INTERVAL,        5,  200, "ms"},
	[TUNE_EMA_NUM]       = {"ema_num",       TUNE_EMA_NUM,         1,   19, ""},
	[TUNE_EMA_DEN]       = {"ema_den",       TUNE_EMA_DEN,         2,  100, ""},
	[TUNE_FIRE_COOLDOWN] = {"fire_cooldown", TUNE_FIRE_COOLDOWN, 100, 5000, "ms"},
	[TUNE_FIRE_PULSE]    = {"fire_pulse",    TUNE_FIRE_PULSE,     20,  500, "ms"},
};

/* ── DS3225 舵机参数 (固定常量) ── */
#define SERVO_PERIOD_NS   PWM_MSEC(20)            /* 50 Hz */
#define SERVO_MIN_PULSE   PWM_USEC(500)           /* 0° */
#define SERVO_MAX_PULSE   PWM_USEC(2500)          /* 270° */
#define SERVO_CENTER      PWM_USEC(g_tune.h_center) /* 水平中点 (运行时可调) */
#define SERVO_V_CENTER    PWM_USEC(g_tune.v_center) /* 垂直中点 (运行时可调) */
#define SERVO_STEP        PWM_USEC(50)            /* 每步增量 */
#define SERVO_STEP_MS     200                     /* 步进间隔 (ms) */

/* ── 扫描/追踪范围 (全范围 500-2500μs) ── */
#define SERVO_H_MIN       PWM_USEC(500)
#define SERVO_H_MAX       PWM_USEC(2500)

/* ── 电磁推杆参数 (运行时可调) ── */
#define PULSE_MS          (g_tune.fire_pulse_ms)
#define FIRE_COOLDOWN_MS  (g_tune.fire_cooldown_ms)

/* ── 设备树节点 ── */
#if ENABLE_SERVO_H
const struct pwm_dt_spec servo_h = PWM_DT_SPEC_GET(DT_ALIAS(servo_h));
#endif

#if ENABLE_SERVO_V
const struct pwm_dt_spec servo_v = PWM_DT_SPEC_GET(DT_ALIAS(servo_v));
#endif

#if ENABLE_SOLENOID
static const struct gpio_dt_spec relay =
	GPIO_DT_SPEC_GET(DT_ALIAS(relay_fire), gpios);
#endif

static const struct gpio_dt_spec button =
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* ── 状态 ── */
static struct gpio_callback button_cb;
static struct k_work fire_work;
static atomic_t fire_count = ATOMIC_INIT(0);
static int64_t last_fire_time = -500; /* will use g_tune.fire_cooldown_ms at runtime */

#if ENABLE_TRACKING
/* ── 追踪参数 (运行时从 g_tune 读取) ── */
#define TRACK_FRAME_W     320   /* PC server 输出帧宽 */
#define TRACK_FRAME_H     240   /* PC server 输出帧高 */

/* 以下宏从 g_tune 实时读取 */
#define TRACK_SLEW_RATE   (g_tune.slew_rate)
#define TRACK_INTERVAL_MS (g_tune.interval_ms)
#define H_LEFT_PULSE      PWM_USEC(g_tune.h_left)
#define H_RIGHT_PULSE     PWM_USEC(g_tune.h_right)
#define TRACK_H_OFFSET    (g_tune.h_offset)
#define V_TOP_PULSE       PWM_USEC(g_tune.v_top)
#define V_BOTTOM_PULSE    PWM_USEC(g_tune.v_bottom)
#define TRACK_V_OFFSET    (g_tune.v_offset)
#define EMA_ALPHA_NUM     (g_tune.ema_num)
#define EMA_ALPHA_DEN     (g_tune.ema_den)

#define SERVO_V_MIN       PWM_USEC(800)
#define SERVO_V_MAX       PWM_USEC(2000)

/* 追踪状态 (track_client 回调线程 → main 循环, atomic 保证 SMP 可见性) */
static atomic_t track_cx = ATOMIC_INIT(-1);  /* -1 = 无目标 */
static atomic_t track_cy = ATOMIC_INIT(-1);

static void on_track_target(int cx, int cy, void *user)
{
	ARG_UNUSED(user);
	atomic_set(&track_cx, cx);
	atomic_set(&track_cy, cy);
}
#endif

/* ── 辅助函数 ── */

/** 脉宽 → 近似角度 (0-270°, 中点 135°) */
static inline int pulse_to_deg(uint32_t pulse_ns)
{
	/* 500μs=0°, 2500μs=270° → deg = (pulse - 500000) * 270 / 2000000 */
	int32_t us = (int32_t)(pulse_ns / 1000);

	return (us - 500) * 270 / 2000;
}

static int servo_set(const struct pwm_dt_spec *spec, uint32_t pulse_ns,
		     const char *name)
{
	int ret = pwm_set_pulse_dt(spec, pulse_ns);

	if (ret < 0) {
		LOG_ERR("%s: pwm_set_pulse failed: %d", name, ret);
	}
	return ret;
}

/* ── tune_shell.c 调用: 立即应用中点到舵机 ── */
void tune_servo_apply_center(void)
{
#if ENABLE_SERVO_H
	servo_set(&servo_h, SERVO_CENTER, "tune_h");
#endif
#if ENABLE_SERVO_V
	servo_set(&servo_v, SERVO_V_CENTER, "tune_v");
#endif
}

#if ENABLE_SOLENOID
static void fire_once(const char *src)
{
	int n = atomic_inc(&fire_count) + 1;

	LOG_INF("FIRE #%d (%s, pulse=%dms)", n, src, PULSE_MS);
	gpio_pin_set_dt(&relay, 1);
	k_msleep(PULSE_MS);
	gpio_pin_set_dt(&relay, 0);
	last_fire_time = k_uptime_get();
}
#endif

static void fire_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
#if ENABLE_SOLENOID
	int64_t now = k_uptime_get();

	if ((now - last_fire_time) < FIRE_COOLDOWN_MS) {
		LOG_WRN("fire cooldown, ignored");
		return;
	}
	fire_once("button");
#else
	LOG_INF("BOOT pressed (solenoid disabled in this phase)");
#endif
}

static void button_isr(const struct device *port,
		       struct gpio_callback *cb,
		       gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_work_submit(&fire_work);
}

/* ── 初始化 ── */

static int init_button(void)
{
	int ret;

	if (!gpio_is_ready_dt(&button)) {
		LOG_ERR("button gpio not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret) {
		LOG_ERR("button configure failed: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret) {
		LOG_ERR("button interrupt failed: %d", ret);
		return ret;
	}

	k_work_init(&fire_work, fire_work_handler);
	gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb);
	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("=== CyberPiercer turret control ===");
	LOG_INF("Phase config: SERVO_H=%d  SERVO_V=%d  SOLENOID=%d  TRACKING=%d",
		ENABLE_SERVO_H, ENABLE_SERVO_V, ENABLE_SOLENOID, ENABLE_TRACKING);

	/* ── NVS Settings: 加载保存的参数 ── */
	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("settings init failed: %d", ret);
	} else {
		ret = tune_params_load();
		if (ret) {
			LOG_WRN("settings load failed: %d (using defaults)", ret);
		} else {
			LOG_INF("NVS params loaded OK");
		}
	}

	/* ── 按钮 ── */
	ret = init_button();
	if (ret) {
		return ret;
	}

	/* ── 继电器 ── */
#if ENABLE_SOLENOID
	if (!gpio_is_ready_dt(&relay)) {
		LOG_ERR("relay gpio not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&relay, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("relay configure failed: %d", ret);
		return ret;
	}
	LOG_INF("relay ready: GPIO38 (ACTIVE_HIGH)");
#endif

	/* ── 水平舵机 ── */
#if ENABLE_SERVO_H
	if (!pwm_is_ready_dt(&servo_h)) {
		LOG_ERR("servo_h PWM not ready");
		return -ENODEV;
	}
	/* 回到中点 (正前方) */
	ret = servo_set(&servo_h, SERVO_CENTER, "servo_h");
	if (ret) {
		return ret;
	}
	LOG_INF("servo_h ready: GPIO9, center=%d°", pulse_to_deg(SERVO_CENTER));
#endif

	/* ── 垂直舵机 ── */
#if ENABLE_SERVO_V
	if (!pwm_is_ready_dt(&servo_v)) {
		LOG_ERR("servo_v PWM not ready");
		return -ENODEV;
	}
	ret = servo_set(&servo_v, SERVO_V_CENTER, "servo_v");
	if (ret) {
		return ret;
	}
	LOG_INF("servo_v ready: GPIO10, center=%d°", pulse_to_deg(SERVO_V_CENTER));
#endif

	LOG_INF("init complete. BOOT=manual fire.");

#if ENABLE_TRACKING
	/* ── WiFi + 追踪客户端 ── */
	{
		char ip[16];

		LOG_INF("Connecting WiFi ...");
		ret = wifi_mgr_connect_blocking(ip, sizeof(ip));
		if (ret) {
			LOG_ERR("WiFi connect failed: %d", ret);
			return ret;
		}
		LOG_INF("WiFi OK, IP=%s", ip);

		ret = track_client_start(PC_SERVER_HOST, PC_SERVER_PORT,
					 on_track_target, NULL);
		if (ret) {
			LOG_ERR("track_client_start failed: %d", ret);
			return ret;
		}
		LOG_INF("track client started → " PC_SERVER_HOST ":%d",
			PC_SERVER_PORT);
	}

	/* ── 追踪主循环 (绝对映射) ── */
	int32_t h_pulse = (int32_t)SERVO_CENTER;
	int32_t v_pulse = (int32_t)SERVO_V_CENTER;
	int32_t ema_cx = TRACK_FRAME_W / 2;
	int32_t ema_cy = TRACK_FRAME_H / 2;
	bool ema_h_init = false;
	bool ema_v_init = false;

	/* 3-点中位数滤波: 消除 YOLO 检测尖刺 */
	int med_buf_x[3] = {-1, -1, -1};
	int med_buf_y[3] = {-1, -1, -1};
	int med_idx_x = 0, med_idx_y = 0;

	LOG_INF("Entering tracking loop (absolute map, slew=%d us/cycle)",
		TRACK_SLEW_RATE);
	LOG_INF("  H map: cx=0→%uus, cx=%d→%uus",
		(uint32_t)H_LEFT_PULSE / 1000,
		TRACK_FRAME_W,
		(uint32_t)H_RIGHT_PULSE / 1000);
	LOG_INF("  V map: cy=0→%uus, cy=%d→%uus",
		(uint32_t)V_TOP_PULSE / 1000,
		TRACK_FRAME_H,
		(uint32_t)V_BOTTOM_PULSE / 1000);

	while (1) {
		int cx = (int)atomic_get(&track_cx);
		int cy = (int)atomic_get(&track_cy);

		int32_t h_diff = 0, v_diff = 0;
		int32_t slew = (int32_t)PWM_USEC(TRACK_SLEW_RATE);

		/* ── 水平追踪 ── */
		if (cx >= 0) {
			med_buf_x[med_idx_x % 3] = cx;
			med_idx_x++;

			int med_cx;
			if (med_idx_x < 3) {
				med_cx = cx;
			} else {
				int a = med_buf_x[0], b = med_buf_x[1],
				    c = med_buf_x[2], t;
				if (a > b) { t = a; a = b; b = t; }
				if (b > c) { t = b; b = c; c = t; }
				if (a > b) { t = a; a = b; b = t; }
				med_cx = b;
			}

			if (!ema_h_init) {
				ema_cx = med_cx;
				ema_h_init = true;
			} else {
				ema_cx = (EMA_ALPHA_NUM * med_cx +
					  (EMA_ALPHA_DEN - EMA_ALPHA_NUM) * ema_cx) /
					 EMA_ALPHA_DEN;
			}

			int32_t h_target = (int32_t)H_LEFT_PULSE +
				((int32_t)H_RIGHT_PULSE - (int32_t)H_LEFT_PULSE) *
				ema_cx / TRACK_FRAME_W +
				(int32_t)PWM_USEC(TRACK_H_OFFSET);
			if (h_target > (int32_t)SERVO_H_MAX) {
				h_target = (int32_t)SERVO_H_MAX;
			} else if (h_target < (int32_t)SERVO_H_MIN) {
				h_target = (int32_t)SERVO_H_MIN;
			}

			h_diff = h_target - h_pulse;
			if (h_diff > slew) { h_diff = slew; }
			else if (h_diff < -slew) { h_diff = -slew; }
			if (h_diff != 0) {
				h_pulse += h_diff;
#if ENABLE_SERVO_H
				servo_set(&servo_h, (uint32_t)h_pulse, "servo_h");
#endif
			}
		}

		/* ── 垂直追踪 ── */
		if (cy >= 0) {
			med_buf_y[med_idx_y % 3] = cy;
			med_idx_y++;

			int med_cy;
			if (med_idx_y < 3) {
				med_cy = cy;
			} else {
				int a = med_buf_y[0], b = med_buf_y[1],
				    c = med_buf_y[2], t;
				if (a > b) { t = a; a = b; b = t; }
				if (b > c) { t = b; b = c; c = t; }
				if (a > b) { t = a; a = b; b = t; }
				med_cy = b;
			}

			if (!ema_v_init) {
				ema_cy = med_cy;
				ema_v_init = true;
			} else {
				ema_cy = (EMA_ALPHA_NUM * med_cy +
					  (EMA_ALPHA_DEN - EMA_ALPHA_NUM) * ema_cy) /
					 EMA_ALPHA_DEN;
			}

			int32_t v_target = (int32_t)V_TOP_PULSE +
				((int32_t)V_BOTTOM_PULSE - (int32_t)V_TOP_PULSE) *
				ema_cy / TRACK_FRAME_H +
				(int32_t)(TRACK_V_OFFSET * 1000);
			if (v_target > (int32_t)SERVO_V_MAX) {
				v_target = (int32_t)SERVO_V_MAX;
			} else if (v_target < (int32_t)SERVO_V_MIN) {
				v_target = (int32_t)SERVO_V_MIN;
			}

			v_diff = v_target - v_pulse;
			if (v_diff > slew) { v_diff = slew; }
			else if (v_diff < -slew) { v_diff = -slew; }
			if (v_diff != 0) {
				v_pulse += v_diff;
#if ENABLE_SERVO_V
				servo_set(&servo_v, (uint32_t)v_pulse, "servo_v");
#endif
			}
		}

		/* ── 自动发射: 双轴已收敛且目标持续存在时开炮 ── */
#if ENABLE_SOLENOID
		if (cx >= 0 && cy >= 0 && h_diff == 0 && v_diff == 0) {
			int64_t now = k_uptime_get();

			if ((now - last_fire_time) >= FIRE_COOLDOWN_MS) {
				fire_once("track");
			}
		}
#endif

		if (cx >= 0 || cy >= 0) {
			LOG_INF("TRK cx=%d cy=%d H=%uus%s V=%uus%s",
				cx, cy,
				(uint32_t)h_pulse / 1000,
				h_diff ? "M" : ".",
				(uint32_t)v_pulse / 1000,
				v_diff ? "M" : ".");
		}

		k_msleep(TRACK_INTERVAL_MS);
	}

#else /* !ENABLE_TRACKING — 扫描模式 */
	LOG_INF("Entering scan loop.");

#if CALIBRATION_MODE
	/* 校准模式: 舵机已归位到 CENTER, 静止不动等待观察 */
	LOG_INF("CALIBRATION MODE: servos at center, holding still.");
	LOG_INF("  H center = %u us", (uint32_t)SERVO_CENTER / 1000);
	LOG_INF("  V center = %u us", (uint32_t)SERVO_V_CENTER / 1000);
	while (1) {
		k_msleep(1000);
	}
#endif

	/* ── 主循环: 扫描 ── */
	int32_t h_pulse = (int32_t)SERVO_CENTER;
	int h_dir = 1; /* 1=向右, -1=向左 */

#if ENABLE_SERVO_V && !ENABLE_SERVO_H
	/* Phase 2: 垂直舵机独立扫描 */
	int32_t v_pulse = (int32_t)SERVO_V_CENTER;
	int v_dir = 1;
#endif

	while (1) {
#if ENABLE_SERVO_H
		h_pulse += h_dir * (int32_t)SERVO_STEP;

		if (h_pulse >= (int32_t)SERVO_H_MAX) {
			h_pulse = (int32_t)SERVO_H_MAX;
			h_dir = -1;
		} else if (h_pulse <= (int32_t)SERVO_H_MIN) {
			h_pulse = (int32_t)SERVO_H_MIN;
			h_dir = 1;
		}

		servo_set(&servo_h, (uint32_t)h_pulse, "servo_h");
		LOG_INF("H=%d° (pulse=%uus)", pulse_to_deg((uint32_t)h_pulse),
			(uint32_t)h_pulse / 1000);
#endif

#if ENABLE_SERVO_V && !ENABLE_SERVO_H
		v_pulse += v_dir * (int32_t)SERVO_STEP;

		if (v_pulse >= (int32_t)SERVO_H_MAX) {
			v_pulse = (int32_t)SERVO_H_MAX;
			v_dir = -1;
		} else if (v_pulse <= (int32_t)SERVO_H_MIN) {
			v_pulse = (int32_t)SERVO_H_MIN;
			v_dir = 1;
		}

		servo_set(&servo_v, (uint32_t)v_pulse, "servo_v");
		LOG_INF("V=%d° (pulse=%uus)", pulse_to_deg((uint32_t)v_pulse),
			(uint32_t)v_pulse / 1000);
#endif

		k_msleep(SERVO_STEP_MS);
	}
#endif /* ENABLE_TRACKING */

	return 0;
}
