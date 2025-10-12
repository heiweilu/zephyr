/*
 * LVGL Widget Wrapper Library Implementation
 * Copyright (c) 2025 Heiweilu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lvgl_wrapper.h"
#include <stdio.h>

LV_FONT_DECLARE(lv_font_unscii_8)

/* ========================================
 * LVGL控件封装函数实现v0.2
 * ======================================== */

lv_obj_t *create_container(lv_obj_t *parent, int32_t  width, int32_t  height, int32_t  pos_x, int32_t  pos_y, int32_t  radius_value, 
				int32_t  border_width, lv_color_t border_color, int32_t  pad, lv_color_t bg_color, int32_t  main_flag)
{
	lv_obj_t *area = lv_obj_create(parent);
	lv_obj_set_size(area, width, height);
	lv_obj_set_pos(area, pos_x, pos_y);
	lv_obj_set_style_radius(area, radius_value, 0);
	lv_obj_set_style_border_width(area, border_width, 0);
	lv_obj_set_style_border_color(area, border_color, 0);
	lv_obj_set_style_bg_color(area, bg_color, 0);

	if (main_flag) {
		lv_obj_set_style_pad_all(area, pad, 0); /* Padding */
	}

	return area;
}

lv_obj_t *create_button(lv_obj_t *parent, int32_t  width, int32_t  height, int32_t  pos_x, int32_t  pos_y, int32_t  radius,
			lv_color_t bg_color, lv_event_cb_t event_cb, void *user_data)
{
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, width, height);
	lv_obj_set_pos(btn, pos_x, pos_y);
	lv_obj_set_style_bg_color(btn, bg_color, 0);
	lv_obj_set_style_radius(btn, radius, 0);

	if (event_cb) {
		lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, user_data);
	}

	return btn;
}

lv_obj_t *create_label(lv_obj_t *parent, const char *text, int32_t  pos_x, int32_t  pos_y,
		       int32_t text_width, lv_color_t text_color, const lv_font_t *font, bool center)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text);
	if (pos_x >= 0 && pos_y >= 0) {
		lv_obj_set_pos(label, pos_x, pos_y);
	}
	lv_obj_set_style_text_color(label, text_color, 0);

	if (text_width > 0) {
		lv_obj_set_width(label, text_width);
	}
	if (font) {
		lv_obj_set_style_text_font(label, font, 0);
	}

	if (center) {
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_center(label);
	}

	return label;
}

lv_obj_t *create_button_with_label(lv_obj_t *parent, const char *text, int32_t  width, int32_t  height,
				   int32_t  pos_x, int32_t  pos_y, int32_t  radius, lv_color_t bg_color,
				   lv_color_t text_color, const lv_font_t *font,
				   lv_event_cb_t event_cb, void *user_data)
{
	lv_obj_t *btn = create_button(parent, width, height, pos_x, pos_y, radius, bg_color,
				      event_cb, user_data);
	create_label(btn, 
		text, 
		0, 
		0, 
		-1,
		text_color, 
		font, 
		true);
	return btn;
}

lv_obj_t *create_icon(lv_obj_t *parent, const uint16_t *icon_data, int32_t  pos_x, int32_t  pos_y, int32_t  size,
		      lv_color_t color, const char *label_text)
{
	lv_obj_t *icon_rect = lv_obj_create(parent);
	lv_obj_set_size(icon_rect, size, size);
	lv_obj_set_pos(icon_rect, pos_x, pos_y);
	lv_obj_set_style_bg_color(icon_rect, color, 0);
	lv_obj_set_style_radius(icon_rect, 2, 0);
	lv_obj_set_style_border_width(icon_rect, 1, 0);
	lv_obj_set_style_border_color(icon_rect, lv_color_white(), 0);

	if (label_text) {
		create_label(icon_rect, 
			label_text, 
			0, 
			0, 
			-1,
			lv_color_white(), 
			&lv_font_unscii_8,
			true);
	}

	return icon_rect;
}

lv_obj_t *create_card_with_label(lv_obj_t *parent, const char *title, const char *value, int32_t  width,
				 int32_t  height, int32_t  pos_x, int32_t  pos_y, int32_t  radius, lv_color_t bg_color,
				 lv_color_t text_color)
{
	lv_obj_t *card = create_container(parent, 
									width, 
									height, 
									pos_x, 
									pos_y, 
									radius, 
									0,
									lv_color_white(),
									0,
									bg_color, 
									0);

	// 创建显示文本（标题+数值）
	char full_text[64];
	snprintf(full_text, sizeof(full_text), "%s\n%s", title, value);

	create_label(card, 
		full_text, 
		0, 
		0, 
		-1,
		text_color, 
		&lv_font_unscii_8, 
		true);

	return card;
}

lv_obj_t *create_progress_bar(lv_obj_t *parent, int32_t  width, int32_t  height, int32_t  pos_x, int32_t  pos_y,
			      int32_t min, int32_t max, int32_t value, lv_color_t bg_color,
			      lv_color_t ind_color)
{
	lv_obj_t *bar = lv_bar_create(parent);
	lv_obj_set_size(bar, width, height);
	lv_obj_set_pos(bar, pos_x, pos_y);
	lv_bar_set_range(bar, min, max);
	lv_bar_set_value(bar, value, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(bar, bg_color, 0);
	lv_obj_set_style_bg_color(bar, ind_color, LV_PART_INDICATOR);

	return bar;
}

lv_obj_t *create_slider(lv_obj_t *parent, int32_t  width, int32_t  height, int32_t  pos_x, int32_t  pos_y, int32_t min,
			int32_t max, int32_t value, lv_color_t bg_color, lv_color_t knob_color,
			lv_event_cb_t event_cb)
{
	lv_obj_t *slider = lv_slider_create(parent);
	lv_obj_set_size(slider, width, height);
	lv_obj_set_pos(slider, pos_x, pos_y);
	lv_slider_set_range(slider, min, max);
	lv_slider_set_value(slider, value, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(slider, bg_color, 0);
	lv_obj_set_style_bg_color(slider, knob_color, LV_PART_KNOB);

	if (event_cb) {
		lv_obj_add_event_cb(slider, event_cb, LV_EVENT_VALUE_CHANGED, NULL);
	}

	return slider;
}

lv_obj_t *create_switch(lv_obj_t *parent, int32_t  pos_x, int32_t  pos_y, bool initial_state,
			lv_color_t bg_color, lv_color_t ind_color, lv_event_cb_t event_cb)
{
	lv_obj_t *sw = lv_switch_create(parent);
	lv_obj_set_pos(sw, pos_x, pos_y);
	lv_obj_set_style_bg_color(sw, bg_color, 0);
	lv_obj_set_style_bg_color(sw, ind_color, LV_PART_INDICATOR);

	if (initial_state) {
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	}

	if (event_cb) {
		lv_obj_add_event_cb(sw, event_cb, LV_EVENT_VALUE_CHANGED, NULL);
	}

	return sw;
}

lv_obj_t *create_checkbox(lv_obj_t *parent, const char *text, int32_t  pos_x, int32_t  pos_y,
			  bool initial_state, lv_color_t text_color, const lv_font_t *font,
			  lv_event_cb_t event_cb)
{
	lv_obj_t *cb = lv_checkbox_create(parent);
	lv_checkbox_set_text(cb, text);
	lv_obj_set_pos(cb, pos_x, pos_y);
	lv_obj_set_style_text_color(cb, text_color, 0);

	if (font) {
		lv_obj_set_style_text_font(cb, font, 0);
	}

	if (initial_state) {
		lv_obj_add_state(cb, LV_STATE_CHECKED);
	}

	if (event_cb) {
		lv_obj_add_event_cb(cb, event_cb, LV_EVENT_VALUE_CHANGED, NULL);
	}

	return cb;
}

lv_obj_t *create_icon_image(lv_obj_t *parent, const uint16_t *icon_data, int32_t  pos_x, int32_t  pos_y)
{
	/* Create a simple colored rectangle for now */
	lv_obj_t *icon_rect = lv_obj_create(parent);
	lv_obj_set_size(icon_rect, 16, 16);
	lv_obj_set_pos(icon_rect, pos_x, pos_y);

	/* Set color based on icon type */
	lv_color_t icon_color = lv_color_hex(0x07E0); /* Default green */

	/* Check if it's a sun icon by examining the icon data pattern */
	if (icon_data != NULL) {
		/* Simple heuristic: if the icon has yellow-like patterns, treat as sun */
		/* For better icon detection, you could compare against known patterns */
		if (icon_data[0] == 0x0000 && icon_data[4] == 0xFFE0) {
			icon_color = lv_color_hex(0xFFE0); /* Yellow for sun */
		}
	}

	lv_obj_set_style_bg_color(icon_rect, icon_color, 0);
	lv_obj_set_style_radius(icon_rect, 2, 0);
	lv_obj_set_style_border_width(icon_rect, 1, 0);
	lv_obj_set_style_border_color(icon_rect, lv_color_white(), 0);

	/* Add a simple text label to identify the icon */
	lv_obj_t *icon_label = lv_label_create(icon_rect);
	if (icon_data != NULL && icon_data[0] == 0x0000 && icon_data[4] == 0xFFE0) {
		lv_label_set_text(icon_label, "S"); /* S for Sun */
	} else {
		lv_label_set_text(icon_label, "?"); /* Unknown icon */
	}
	lv_obj_set_style_text_color(icon_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(icon_label, &lv_font_unscii_8, 0);
	lv_obj_center(icon_label);

	return icon_rect;
}

lv_obj_t *create_textarea(lv_obj_t *parent, int32_t width, int32_t height, int32_t pos_x, int32_t pos_y,
			  lv_color_t bg_color, lv_color_t text_color, const lv_font_t *font,
			  const char *placeholder, bool one_line)
{
	lv_obj_t *textarea = lv_textarea_create(parent);
	lv_obj_set_size(textarea, width, height);
	lv_obj_set_pos(textarea, pos_x, pos_y);
	lv_obj_set_style_bg_color(textarea, bg_color, 0);
	lv_obj_set_style_text_color(textarea, text_color, 0);
	
	if (font) {
		lv_obj_set_style_text_font(textarea, font, 0);
	}
	
	if (placeholder) {
		lv_textarea_set_placeholder_text(textarea, placeholder);
	}
	
	lv_textarea_set_one_line(textarea, one_line);
	
	return textarea;
}
