/*
 * LVGL Widget Wrapper Library
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 这是一个LVGL控件封装库头文件，包含了常用控件的简化创建函数
 * 可以轻松地复制到其他项目中使用，提高开发效率
 */

#ifndef LVGL_WRAPPER_H
#define LVGL_WRAPPER_H

#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================
 * LVGL控件封装函数声明
 * ======================================== */

/**
 * 创建容器
 * @param parent 父对象
 * @param width 宽度
 * @param height 高度
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @param radius_value 圆角半径
 * @param border_width 边框宽度
 * @param border_color 边框颜色
 * @param pad 内边距
 * @param bg_color 背景颜色
 * @param main_flag 是否是主容器(影响padding设置)
 * @return 创建的容器对象
 * 示例：   g_lcd_console.shell_container = create_container(parent, 240, 135, 0, 0, 0, 1, 2,
						      lv_color_black(), 1);
 */
lv_obj_t *create_container(lv_obj_t *parent, 
				int32_t  width, int32_t  height, 
				int32_t  pos_x, int32_t  pos_y,
				int32_t  radius_value, 
				int32_t  border_width, lv_color_t border_color, 
				int32_t  pad, 
				lv_color_t bg_color,
				int32_t  main_flag);

/**
 * 创建按钮
 * @param parent 父对象
 * @param width 宽度
 * @param height 高度
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @param radius 圆角半径
 * @param bg_color 背景颜色
 * @param event_cb 事件回调函数
 * @param user_data 用户数据
 * @return 创建的按钮对象
 */
lv_obj_t *create_button(lv_obj_t *parent, int32_t  width, int32_t  height, int32_t  pos_x, int32_t  pos_y, int32_t  radius,
			lv_color_t bg_color, lv_event_cb_t event_cb, void *user_data);

/**
 * 创建标签
 * @param parent 父对象
 * @param text 文本内容
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @param text_width 文本宽度
 * @param text_color 文字颜色
 * @param font 字体
 * @param center 是否居中显示
 * @return 创建的标签对象
 */
lv_obj_t *create_label(lv_obj_t *parent, const char *text, int32_t  pos_x, int32_t  pos_y,
		       int32_t text_width, lv_color_t text_color, const lv_font_t *font, bool center);

/**
 * 创建带标签的按钮
 * @param parent 父对象
 * @param text 按钮文字
 * @param width 宽度
 * @param height 高度
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @param radius 圆角半径
 * @param bg_color 背景颜色
 * @param text_color 文字颜色
 * @param font 字体
 * @param event_cb 事件回调函数
 * @param user_data 用户数据
 * @return 创建的按钮对象
 */
lv_obj_t *create_button_with_label(lv_obj_t *parent, const char *text, int32_t  width, int32_t  height,
				   int32_t  pos_x, int32_t  pos_y, int32_t  radius, lv_color_t bg_color,
				   lv_color_t text_color, const lv_font_t *font,
				   lv_event_cb_t event_cb, void *user_data);

/**
 * 创建图标
 * @param parent 父对象
 * @param icon_data 图标数据(可以为NULL)
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @param size 图标尺寸
 * @param color 图标颜色
 * @param label_text 图标标识文字
 * @return 创建的图标对象
 */
lv_obj_t *create_icon(lv_obj_t *parent, const uint16_t *icon_data, int32_t  pos_x, int32_t  pos_y, int32_t  size,
		      lv_color_t color, const char *label_text);

/**
 * 创建图标图像（简化版本）
 * @param parent 父对象
 * @param icon_data 图标数据
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @return 创建的图标对象
 */
lv_obj_t *create_icon_image(lv_obj_t *parent, const uint16_t *icon_data, int32_t  pos_x, int32_t  pos_y);

/**
 * 创建带标签的卡片
 * @param parent 父对象
 * @param title 卡片标题
 * @param value 卡片数值
 * @param width 宽度
 * @param height 高度
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @param radius 圆角半径
 * @param bg_color 背景颜色
 * @param text_color 文字颜色
 * @return 创建的卡片对象
 */
lv_obj_t *create_card_with_label(lv_obj_t *parent, const char *title, const char *value, int32_t  width,
				 int32_t  height, int32_t  pos_x, int32_t  pos_y, int32_t  radius, lv_color_t bg_color,
				 lv_color_t text_color);

/**
 * 创建进度条
 * @param parent 父对象
 * @param width 宽度
 * @param height 高度
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @param min 最小值
 * @param max 最大值
 * @param value 当前值
 * @param bg_color 背景颜色
 * @param ind_color 指示器颜色
 * @return 创建的进度条对象
 */
lv_obj_t *create_progress_bar(lv_obj_t *parent, int32_t  width, int32_t  height, int32_t  pos_x, int32_t  pos_y,
			      int32_t min, int32_t max, int32_t value, lv_color_t bg_color,
			      lv_color_t ind_color);

/**
 * 创建滑动条
 * @param parent 父对象
 * @param width 宽度
 * @param height 高度
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @param min 最小值
 * @param max 最大值
 * @param value 当前值
 * @param bg_color 背景颜色
 * @param knob_color 滑块颜色
 * @param event_cb 事件回调函数
 * @return 创建的滑动条对象
 */
lv_obj_t *create_slider(lv_obj_t *parent, int32_t  width, int32_t  height, int32_t  pos_x, int32_t  pos_y, int32_t min,
			int32_t max, int32_t value, lv_color_t bg_color, lv_color_t knob_color,
			lv_event_cb_t event_cb);

/**
 * 创建开关
 * @param parent 父对象
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @param initial_state 初始状态
 * @param bg_color 背景颜色
 * @param ind_color 指示器颜色
 * @param event_cb 事件回调函数
 * @return 创建的开关对象
 */
lv_obj_t *create_switch(lv_obj_t *parent, int32_t  pos_x, int32_t  pos_y, bool initial_state,
			lv_color_t bg_color, lv_color_t ind_color, lv_event_cb_t event_cb);

/**
 * 创建复选框
 * @param parent 父对象
 * @param text 复选框文字
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @param initial_state 初始状态
 * @param text_color 文字颜色
 * @param font 字体
 * @param event_cb 事件回调函数
 * @return 创建的复选框对象
 */
lv_obj_t *create_checkbox(lv_obj_t *parent, const char *text, int32_t  pos_x, int32_t  pos_y,
			  bool initial_state, lv_color_t text_color, const lv_font_t *font,
			  lv_event_cb_t event_cb);

/**
 * 创建文本输入区域
 * @param parent 父对象
 * @param width 宽度
 * @param height 高度
 * @param pos_x X坐标
 * @param pos_y Y坐标
 * @param bg_color 背景颜色
 * @param text_color 文字颜色
 * @param font 字体
 * @param placeholder 占位符文本
 * @param one_line 是否单行输入
 * @return 创建的文本输入区域对象
 */
lv_obj_t *create_textarea(lv_obj_t *parent, int32_t width, int32_t height, int32_t pos_x, int32_t pos_y,
			  lv_color_t bg_color, lv_color_t text_color, const lv_font_t *font,
			  const char *placeholder, bool one_line);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_WRAPPER_H */
