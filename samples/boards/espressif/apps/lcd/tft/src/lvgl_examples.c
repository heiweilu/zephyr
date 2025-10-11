/*
 * LVGL Widget Wrapper Library Usage Example
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 这个文件展示了如何使用LVGL封装库快速创建UI界面
 */
/* ========================================
 * 使用方法总结
 * ======================================== */

/*
LVGL变量显示控制完整指南：

=== 1. 基本控件创建 ===
#include "lvgl_wrapper.h"

- 使用 create_container() 创建容器布局
- 使用 create_button_with_label() 创建按钮
- 使用 create_label() 创建文本标签
- 使用 create_slider() 创建滑动条
- 使用 create_progress_bar() 创建进度条
- 使用 create_switch() 创建开关
- 使用 create_checkbox() 创建复选框
- 使用 create_card_with_label() 创建数据卡片
- 使用 create_icon() 创建图标

=== 2. 变量显示控制方法 ===

A) 静态显示：
   lv_obj_t *label = create_label(parent, "Initial Text", x, y, color, font, center);

B) 动态更新 - 整数：
   lv_label_set_text_fmt(label, "%d", integer_variable);

C) 动态更新 - 浮点数：
   // 方法1：直接使用浮点格式（需要确保系统支持）
   lv_label_set_text_fmt(label, "%.2f", (double)float_variable);

   // 方法2：推荐 - 使用整数避免浮点格式化问题
   int temp_int = (int)(temperature * 10);
   int temp_whole = temp_int / 10;
   int temp_frac = temp_int % 10;
   lv_label_set_text_fmt(label, "%d.%dC", temp_whole, temp_frac);

D) 动态更新 - 字符串：
   lv_label_set_text(label, string_variable);

E) 动态更新 - 布尔值：
   lv_label_set_text(label, bool_variable ? "ON" : "OFF");

F) 进度条数值绑定：
   lv_obj_t *progress = create_progress_bar(parent, w, h, x, y, min, max, value, bg_color,
fg_color); lv_bar_set_value(progress, new_value, LV_ANIM_OFF);  // 更新进度条

G) 滑动条数值绑定：
   lv_obj_t *slider = create_slider(parent, w, h, x, y, min, max, value, bg_color, fg_color,
callback); int32_t value = lv_slider_get_value(slider);  // 在回调中获取值

=== 3. 实时更新策略 ===

A) 事件驱动更新：
   static void button_handler(lv_event_t *e) {
       if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
	   // 更新变量
	   variable++;
	   // 更新显示
	   lv_label_set_text_fmt(label, "%d", variable);
       }
   }

B) 定时器更新：
   static void timer_cb(lv_timer_t *timer) {
       // 读取传感器或更新变量
       temperature = read_sensor();
       // 更新显示
       lv_label_set_text_fmt(temp_label, "%.1f°C", (double)temperature);
   }

   // 创建定时器（每1000ms执行一次）
   lv_timer_create(timer_cb, 1000, NULL);

C) 主循环更新：
   while(1) {
       // 更新变量
       update_variables();
       // 更新GUI显示
       update_gui_display();
       // LVGL任务处理
       lv_timer_handler();
       k_msleep(100);
   }

=== 4. 变量类型处理示例 ===

// 整数变量
int temperature = 25;
lv_label_set_text_fmt(temp_label, "%d°C", temperature);

// 浮点变量 - 推荐使用整数方法避免格式化问题
float voltage = 3.14f;
// 方法1：直接格式化（某些系统可能显示"float"）
lv_label_set_text_fmt(volt_label, "%.2fV", (double)voltage);
// 方法2：推荐 - 使用整数避免问题
int volt_int = (int)(voltage * 100);
int volt_whole = volt_int / 100;
int volt_frac = volt_int % 100;
lv_label_set_text_fmt(volt_label, "%d.%02dV", volt_whole, volt_frac);

// 字符串变量
char device_name[32] = "ESP32-S3";
lv_label_set_text(name_label, device_name);

// 布尔变量
bool led_on = true;
lv_label_set_text(status_label, led_on ? "ON" : "OFF");
lv_obj_set_style_text_color(status_label, led_on ? lv_color_green() : lv_color_red(), 0);

// 数组变量
int sensor_data[4] = {25, 60, 1013, 15};
for(int i = 0; i < 4; i++) {
    lv_label_set_text_fmt(sensor_labels[i], "%d", sensor_data[i]);
}

=== 5. 高级技巧 ===

A) 格式化显示：
   lv_label_set_text_fmt(label, "Temp: %d°C\nHumidity: %d%%", temp, humidity);

   // 对于浮点数，推荐使用整数方法
   int temp_int = (int)(temp_float * 10);
   lv_label_set_text_fmt(label, "Temp: %d.%dC\nHumidity: %d%%",
			temp_int/10, temp_int%10, humidity);

B) 条件显示：
   lv_obj_set_style_text_color(label, (value > threshold) ? lv_color_red() : lv_color_green(), 0);

C) 动画更新：
   lv_bar_set_value(progress, new_value, LV_ANIM_ON);  // 带动画的进度条更新

D) 范围限制：
   value = (value < min) ? min : ((value > max) ? max : value);

E) 对象指针管理：
   // 保存标签指针供后续更新
   static lv_obj_t *temp_label = NULL;
   temp_label = create_label(...);

   // 或使用用户数据
   lv_obj_set_user_data(label, "temperature_sensor");

=== 6. 布局和设计建议 ===

- 使用一致的颜色编码表示不同类型的数据
- 为不同范围的数值使用不同颜色（正常/警告/错误）
- 保持标签和数值的对齐
- 使用合适的字体大小确保可读性
- 预留足够空间显示最大可能的数值

=== 7. 性能优化和兼容性建议 ===

- 只在数值真正改变时才更新显示
- 使用定时器控制更新频率，避免过于频繁的刷新
- 批量更新多个控件后再调用 lv_timer_handler()
- 对于高频更新的数据，考虑使用平均值或滤波
- 浮点数显示：某些嵌入式系统的printf可能不完全支持浮点格式化，
  如果显示"float"而非数值，建议使用整数方法进行格式化
- 避免使用特殊Unicode字符（如°符号），可能导致显示问题

这些方法让你能够轻松地在LVGL界面中显示和控制各种类型的变量！
*/

#include <stdio.h>
#include <stdlib.h>
#include "lvgl_wrapper.h"

LV_FONT_DECLARE(lv_font_unscii_8)

/* ========================================
 * 事件处理函数声明
 * ======================================== */

static void button_event_handler(lv_event_t *e);
static void slider_event_handler(lv_event_t *e);
static void switch_event_handler(lv_event_t *e);
static void checkbox_event_handler(lv_event_t *e);

// 变量显示控制相关函数声明
static void update_variables_handler(lv_event_t *e);
static void toggle_led_handler(lv_event_t *e);
static void reset_variables_handler(lv_event_t *e);
static void voltage_slider_handler(lv_event_t *e);
static void start_timer_handler(lv_event_t *e);
static void stop_timer_handler(lv_event_t *e);
static void timer_update_cb(lv_timer_t *timer);

/* ========================================
 * 事件处理函数示例
 * ======================================== */

static void button_event_handler(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_CLICKED) {
		// 按钮被点击
		printf("Button clicked!\n");
	}
}

static void slider_event_handler(lv_event_t *e)
{
	lv_obj_t *slider = lv_event_get_target_obj(e);
	int32_t value = lv_slider_get_value(slider);
	printf("Slider value: %d\n", (int)value);
}

static void switch_event_handler(lv_event_t *e)
{
	lv_obj_t *sw = lv_event_get_target_obj(e);
	bool state = lv_obj_has_state(sw, LV_STATE_CHECKED);
	printf("Switch state: %s\n", state ? "ON" : "OFF");
}

static void checkbox_event_handler(lv_event_t *e)
{
	lv_obj_t *cb = lv_event_get_target_obj(e);
	bool checked = lv_obj_has_state(cb, LV_STATE_CHECKED);
	printf("Checkbox: %s\n", checked ? "Checked" : "Unchecked");
}

/* ========================================
 * UI创建示例
 * ======================================== */

/**
 * 示例1：创建一个简单的控制面板
 */
void create_control_panel_example(void)
{
	lv_obj_t *screen = lv_screen_active();

	// 创建主容器
	lv_obj_t *main_container =
		create_container(screen, 240, 135, 0, 0, 10, 2, 8, lv_color_hex(0x1E1E1E), 1);

	// 创建标题
	create_label(main_container, "CONTROL PANEL", 120, 15, lv_color_white(), &lv_font_unscii_8,
		     true);

	// 创建控制按钮
	create_button_with_label(main_container, "START", 60, 25, 10, 35, 5, lv_color_hex(0x4CAF50),
				 lv_color_white(), &lv_font_unscii_8, button_event_handler, NULL);

	create_button_with_label(main_container, "STOP", 60, 25, 80, 35, 5, lv_color_hex(0xF44336),
				 lv_color_white(), &lv_font_unscii_8, button_event_handler, NULL);

	create_button_with_label(main_container, "RESET", 60, 25, 150, 35, 5,
				 lv_color_hex(0xFF9800), lv_color_white(), &lv_font_unscii_8,
				 button_event_handler, NULL);

	// 创建速度滑动条
	create_label(main_container, "Speed:", 10, 70, lv_color_white(), &lv_font_unscii_8, false);
	create_slider(main_container, 120, 15, 50, 72, 0, 100, 50, lv_color_hex(0x2196F3),
		      lv_color_hex(0xFFFFFF), slider_event_handler);

	// 创建开关控制
	create_label(main_container, "LED:", 10, 95, lv_color_white(), &lv_font_unscii_8, false);
	create_switch(main_container, 50, 95, false, lv_color_hex(0x757575), lv_color_hex(0x4CAF50),
		      switch_event_handler);

	// 创建复选框
	create_checkbox(main_container, "Auto Mode", 110, 95, false, lv_color_white(),
			&lv_font_unscii_8, checkbox_event_handler);
}

/**
 * 示例2：创建一个数据监控面板
 */
void create_monitoring_panel_example(void)
{
	lv_obj_t *screen = lv_screen_active();

	// 创建主容器
	lv_obj_t *main_container =
		create_container(screen, 240, 135, 0, 0, 8, 1, 5, lv_color_hex(0x0F1419), 1);

	// 创建标题
	create_label(main_container, "SYSTEM MONITOR", 120, 10, lv_color_hex(0x00D9FF),
		     &lv_font_unscii_8, true);

	// 创建温度卡片
	create_card_with_label(main_container, "TEMP", "24.5°C", 70, 40, 10, 25, 8,
			       lv_color_hex(0xFF6B35), lv_color_white());

	// 创建湿度卡片
	create_card_with_label(main_container, "HUMIDITY", "65%", 70, 40, 85, 25, 8,
			       lv_color_hex(0x00A8CC), lv_color_white());

	// 创建压力卡片
	create_card_with_label(main_container, "PRESSURE", "1013hPa", 70, 40, 160, 25, 8,
			       lv_color_hex(0x7209B7), lv_color_white());

	// 创建CPU使用率进度条
	create_label(main_container, "CPU:", 10, 75, lv_color_white(), &lv_font_unscii_8, false);
	create_progress_bar(main_container, 180, 12, 35, 78, 0, 100, 45, lv_color_hex(0x404040),
			    lv_color_hex(0x00FF88));

	// 创建内存使用率进度条
	create_label(main_container, "MEM:", 10, 95, lv_color_white(), &lv_font_unscii_8, false);
	create_progress_bar(main_container, 180, 12, 35, 98, 0, 100, 72, lv_color_hex(0x404040),
			    lv_color_hex(0xFF6B35));

	// 创建状态指示图标
	create_icon(NULL, NULL, 220, 5, 12, lv_color_hex(0x00FF00), "OK");
}

/**
 * 示例3：创建一个设置界面
 */
void create_settings_panel_example(void)
{
	lv_obj_t *screen = lv_screen_active();

	// 创建主容器
	lv_obj_t *main_container =
		create_container(screen, 240, 135, 0, 0, 12, 1, 10, lv_color_hex(0x2C3E50), 1);

	// 创建标题
	create_label(main_container, "SETTINGS", 120, 15, lv_color_hex(0xECF0F1), &lv_font_unscii_8,
		     true);

	// 亮度调节
	create_label(main_container, "Brightness:", 15, 35, lv_color_white(), &lv_font_unscii_8,
		     false);
	create_slider(main_container, 120, 15, 90, 37, 10, 100, 80, lv_color_hex(0x34495E),
		      lv_color_hex(0xF39C12), slider_event_handler);

	// 音量调节
	create_label(main_container, "Volume:", 15, 55, lv_color_white(), &lv_font_unscii_8, false);
	create_slider(main_container, 120, 15, 90, 57, 0, 100, 60, lv_color_hex(0x34495E),
		      lv_color_hex(0x9B59B6), slider_event_handler);

	// 功能开关
	create_checkbox(main_container, "WiFi", 15, 80, true, lv_color_white(), &lv_font_unscii_8,
			checkbox_event_handler);

	create_checkbox(main_container, "Bluetooth", 80, 80, false, lv_color_white(),
			&lv_font_unscii_8, checkbox_event_handler);

	create_checkbox(main_container, "Auto Save", 170, 80, true, lv_color_white(),
			&lv_font_unscii_8, checkbox_event_handler);

	// 保存按钮
	create_button_with_label(main_container, "SAVE", 80, 25, 80, 105, 6, lv_color_hex(0x27AE60),
				 lv_color_white(), &lv_font_unscii_8, button_event_handler, NULL);
}

/* ========================================
 * 变量显示控制示例
 * ======================================== */

// 全局变量示例
static int system_temperature = 25;
static int cpu_usage = 45;
static float voltage = 3.3f;
static bool led_status = false;
static char device_name[32] = "ESP32-S3";

// 用于存储标签对象的指针，以便后续更新
static lv_obj_t *temp_value_label = NULL;
static lv_obj_t *cpu_value_label = NULL;
static lv_obj_t *voltage_value_label = NULL;
static lv_obj_t *status_label = NULL;

/**
 * 示例4：变量显示和实时更新
 */
void create_variable_display_example(void)
{
	lv_obj_t *screen = lv_screen_active();

	// 创建主容器
	lv_obj_t *main_container =
		create_container(screen, 240, 135, 0, 0, 8, 1, 8, lv_color_hex(0x1A1A1A), 1);

	// 标题
	create_label(main_container, "VARIABLE DISPLAY", 120, 5, lv_color_hex(0x00FF88),
		     &lv_font_unscii_8, true);

	// 显示温度变量
	create_label(main_container, "Temperature:", 5, 25, lv_color_white(), &lv_font_unscii_8,
		     false);
	temp_value_label = create_label(main_container, "25°C", 90, 25, lv_color_hex(0xFF6B35),
					&lv_font_unscii_8, false);

	// 显示CPU使用率变量
	create_label(main_container, "CPU Usage:", 5, 40, lv_color_white(), &lv_font_unscii_8,
		     false);
	cpu_value_label = create_label(main_container, "45%", 90, 40, lv_color_hex(0x00A8CC),
				       &lv_font_unscii_8, false);

	// 显示电压变量（浮点数）- 使用整数方法避免格式化问题
	create_label(main_container, "Voltage:", 5, 55, lv_color_white(), &lv_font_unscii_8, false);
	voltage_value_label = create_label(main_container, "3.30V", 90, 55, lv_color_hex(0xFFE066),
					   &lv_font_unscii_8, false);

	// 显示设备名称（字符串）
	create_label(main_container, "Device:", 5, 70, lv_color_white(), &lv_font_unscii_8, false);
	create_label(main_container, device_name, 90, 70, lv_color_hex(0xFF66FF), &lv_font_unscii_8,
		     false);

	// 显示状态（布尔值）
	create_label(main_container, "LED Status:", 5, 85, lv_color_white(), &lv_font_unscii_8,
		     false);
	status_label = create_label(main_container, led_status ? "ON" : "OFF", 90, 85,
				    led_status ? lv_color_hex(0x00FF00) : lv_color_hex(0xFF0000),
				    &lv_font_unscii_8, false);

	// 控制按钮
	create_button_with_label(main_container, "UPDATE", 50, 20, 140, 25, 4,
				 lv_color_hex(0x4CAF50), lv_color_white(), &lv_font_unscii_8,
				 update_variables_handler, NULL);

	create_button_with_label(main_container, "TOGGLE LED", 70, 20, 140, 50, 4,
				 lv_color_hex(0xFF9800), lv_color_white(), &lv_font_unscii_8,
				 toggle_led_handler, NULL);

	create_button_with_label(main_container, "RESET", 50, 20, 140, 75, 4,
				 lv_color_hex(0xF44336), lv_color_white(), &lv_font_unscii_8,
				 reset_variables_handler, NULL);
}

// 更新变量的事件处理函数
static void update_variables_handler(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
		// 模拟变量变化
		system_temperature += (rand() % 10) - 5;            // ±5度变化
		cpu_usage = (cpu_usage + (rand() % 20) - 10) % 100; // ±10%变化
		voltage += ((rand() % 20) - 10) * 0.01f;            // ±0.1V变化

		// 限制范围
		if (system_temperature < 0) {
			system_temperature = 0;
		}
		if (system_temperature > 100) {
			system_temperature = 100;
		}
		if (cpu_usage < 0) {
			cpu_usage = 0;
		}
		if (voltage < 0) {
			voltage = 0;
		}
		if (voltage > 5.0f) {
			voltage = 5.0f;
		}

		// 更新显示 - 方法1：使用整数避免浮点格式化问题
		lv_label_set_text_fmt(temp_value_label, "%d°C", system_temperature);
		lv_label_set_text_fmt(cpu_value_label, "%d%%", cpu_usage);

		// 电压使用整数方法格式化
		int volt_int = (int)(voltage * 100);
		int volt_whole = volt_int / 100;
		int volt_frac = volt_int % 100;
		lv_label_set_text_fmt(voltage_value_label, "%d.%02dV", volt_whole, volt_frac);

		printf("Variables updated: Temp=%d°C, CPU=%d%%, Voltage=%.2fV\n",
		       system_temperature, cpu_usage, (double)voltage);
	}
}

// 切换LED状态的事件处理函数
static void toggle_led_handler(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
		led_status = !led_status;

		// 更新显示 -
		// 方法2：使用lv_label_set_text()设置文本，lv_obj_set_style_text_color()设置颜色
		lv_label_set_text(status_label, led_status ? "ON" : "OFF");
		lv_obj_set_style_text_color(
			status_label, led_status ? lv_color_hex(0x00FF00) : lv_color_hex(0xFF0000),
			0);

		printf("LED toggled: %s\n", led_status ? "ON" : "OFF");
	}
}

// 重置变量的事件处理函数
static void reset_variables_handler(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
		// 重置所有变量到初始值
		system_temperature = 25;
		cpu_usage = 45;
		voltage = 3.3f;
		led_status = false;

		// 更新显示
		lv_label_set_text_fmt(temp_value_label, "%d°C", system_temperature);
		lv_label_set_text_fmt(cpu_value_label, "%d%%", cpu_usage);

		// 电压重置为3.30V
		int volt_int = (int)(voltage * 100);
		int volt_whole = volt_int / 100;
		int volt_frac = volt_int % 100;
		lv_label_set_text_fmt(voltage_value_label, "%d.%02dV", volt_whole, volt_frac);
		lv_label_set_text(status_label, "OFF");
		lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);

		printf("All variables reset to default values\n");
	}
}

/**
 * 示例5：进度条和滑动条的变量绑定
 */
void create_progress_variable_example(void)
{
	lv_obj_t *screen = lv_screen_active();

	// 创建主容器
	lv_obj_t *main_container =
		create_container(screen, 240, 135, 0, 0, 8, 1, 8, lv_color_hex(0x2C2C2C), 1);

	// 标题
	create_label(main_container, "PROGRESS CONTROL", 120, 5, lv_color_hex(0x66CCFF),
		     &lv_font_unscii_8, true);

	// CPU使用率进度条（绑定到cpu_usage变量）
	create_label(main_container, "CPU Usage:", 5, 25, lv_color_white(), &lv_font_unscii_8,
		     false);
	lv_obj_t *cpu_progress =
		create_progress_bar(main_container, 150, 12, 5, 40, 0, 100, cpu_usage,
				    lv_color_hex(0x404040), lv_color_hex(0x00FF88));

	// 温度进度条（绑定到temperature变量）
	create_label(main_container, "Temperature:", 5, 60, lv_color_white(), &lv_font_unscii_8,
		     false);
	lv_obj_t *temp_progress =
		create_progress_bar(main_container, 150, 12, 5, 75, 0, 100, system_temperature,
				    lv_color_hex(0x404040), lv_color_hex(0xFF6B35));

	// 电压滑动条（可以修改voltage变量）
	create_label(main_container, "Set Voltage:", 5, 95, lv_color_white(), &lv_font_unscii_8,
		     false);
	lv_obj_t *voltage_slider = create_slider(main_container, 150, 15, 5, 110, 0, 500,
						 (int)(voltage * 100), lv_color_hex(0x2196F3),
						 lv_color_hex(0xFFFFFF), voltage_slider_handler);

	// 保存进度条对象指针供后续更新使用
	lv_obj_set_user_data(cpu_progress, "cpu_progress");
	lv_obj_set_user_data(temp_progress, "temp_progress");
	lv_obj_set_user_data(voltage_slider, "voltage_slider");
}

// 电压滑动条事件处理函数
static void voltage_slider_handler(lv_event_t *e)
{
	lv_obj_t *slider = lv_event_get_target_obj(e);
	int32_t value = lv_slider_get_value(slider);

	// 将滑动条值（0-500）转换为电压（0.0-5.0V）
	voltage = value / 100.0f;

	printf("Voltage set to: %.2fV\n", (double)voltage);
}

/**
 * 示例6：定时器更新变量显示
 */
static lv_timer_t *update_timer = NULL;

void create_timer_update_example(void)
{
	lv_obj_t *screen = lv_screen_active();

	// 创建主容器
	lv_obj_t *main_container =
		create_container(screen, 240, 135, 0, 0, 8, 1, 8, lv_color_hex(0x0F1419), 1);

	// 标题
	create_label(main_container, "AUTO UPDATE", 120, 5, lv_color_hex(0x00D9FF),
		     &lv_font_unscii_8, true);

	// 创建实时更新的数据显示
	create_label(main_container, "Real-time Data:", 5, 25, lv_color_white(), &lv_font_unscii_8,
		     false);

	// 系统运行时间
	create_label(main_container, "Uptime:", 5, 40, lv_color_white(), &lv_font_unscii_8, false);
	lv_obj_t *uptime_label = create_label(main_container, "0s", 80, 40, lv_color_hex(0x00FF88),
					      &lv_font_unscii_8, false);

	// 随机数据
	create_label(main_container, "Random:", 5, 55, lv_color_white(), &lv_font_unscii_8, false);
	lv_obj_t *random_label = create_label(main_container, "0", 80, 55, lv_color_hex(0xFF6B35),
					      &lv_font_unscii_8, false);

	// 计数器
	create_label(main_container, "Counter:", 5, 70, lv_color_white(), &lv_font_unscii_8, false);
	lv_obj_t *counter_label = create_label(main_container, "0", 80, 70, lv_color_hex(0xFFE066),
					       &lv_font_unscii_8, false);

	// 启动/停止按钮
	create_button_with_label(main_container, "START TIMER", 80, 20, 5, 90, 4,
				 lv_color_hex(0x4CAF50), lv_color_white(), &lv_font_unscii_8,
				 start_timer_handler, NULL);

	create_button_with_label(main_container, "STOP TIMER", 80, 20, 90, 90, 4,
				 lv_color_hex(0xF44336), lv_color_white(), &lv_font_unscii_8,
				 stop_timer_handler, NULL);

	// 保存标签对象指针
	lv_obj_set_user_data(uptime_label, "uptime");
	lv_obj_set_user_data(random_label, "random");
	lv_obj_set_user_data(counter_label, "counter");
}

// 定时器回调函数 - 每秒更新一次数据
static void timer_update_cb(lv_timer_t *timer)
{
	static int counter = 0;
	static uint32_t start_time = 0;

	if (start_time == 0) {
		start_time = lv_tick_get();
	}

	counter++;
	uint32_t uptime = (lv_tick_get() - start_time) / 1000; // 秒
	int random_value = rand() % 1000;

	// 查找标签对象并更新（这里需要你根据实际情况获取对象指针）
	// 实际应用中，你应该将这些对象指针保存为全局变量
	printf("Timer update: Uptime=%ds, Counter=%d, Random=%d\n", (int)uptime, counter,
	       random_value);
}

// 启动定时器事件处理函数
static void start_timer_handler(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
		if (update_timer == NULL) {
			update_timer =
				lv_timer_create(timer_update_cb, 1000, NULL); // 每1000ms执行一次
			printf("Timer started\n");
		}
	}
}

// 停止定时器事件处理函数
static void stop_timer_handler(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
		if (update_timer != NULL) {
			lv_timer_delete(update_timer);
			update_timer = NULL;
			printf("Timer stopped\n");
		}
	}
}
