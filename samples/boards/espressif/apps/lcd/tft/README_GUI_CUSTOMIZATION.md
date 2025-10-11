# ESP32-S3 LVGL GUI 定制指南

本文档详细介绍如何在ESP32-S3 TFT显示项目中定制LVGL GUI界面，包括字体、图标、图片、页面切换和主题等功能。

## 目录
1. [字体大小和字体切换](#字体大小和字体切换)
2. [图标设计和使用](#图标设计和使用)
3. [图片显示](#图片显示)
4. [页面切换](#页面切换)
5. [主题切换](#主题切换)
6. [实用技巧](#实用技巧)

---

## 字体大小和字体切换

### 1.1 修改字体大小

LVGL中可以通过多种方式修改字体大小：

#### 方法一：使用内置字体
```c
/* 使用LVGL内置字体 */
lv_obj_t *label = lv_label_create(parent);
lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);  // 14像素
lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);  // 16像素
lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);  // 18像素
lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);  // 20像素
lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);  // 24像素
```

#### 方法二：在prj.conf中启用更多字体
```ini
# 在 prj.conf 中添加
CONFIG_LV_FONT_MONTSERRAT_12=y
CONFIG_LV_FONT_MONTSERRAT_14=y
CONFIG_LV_FONT_MONTSERRAT_16=y
CONFIG_LV_FONT_MONTSERRAT_18=y
CONFIG_LV_FONT_MONTSERRAT_20=y
CONFIG_LV_FONT_MONTSERRAT_24=y
CONFIG_LV_FONT_MONTSERRAT_28=y
```

### 1.2 自定义字体

#### 创建自定义字体
```c
/* 定义不同大小的字体样式 */
static lv_style_t style_title;
static lv_style_t style_normal;
static lv_style_t style_small;

void init_font_styles(void)
{
    /* 标题字体样式 */
    lv_style_init(&style_title);
    lv_style_set_text_font(&style_title, &lv_font_montserrat_20);
    lv_style_set_text_color(&style_title, lv_color_white());
    
    /* 正常字体样式 */
    lv_style_init(&style_normal);
    lv_style_set_text_font(&style_normal, &lv_font_montserrat_14);
    lv_style_set_text_color(&style_normal, lv_color_make(0xC0, 0xC0, 0xC0));
    
    /* 小字体样式 */
    lv_style_init(&style_small);
    lv_style_set_text_font(&style_small, &lv_font_montserrat_12);
    lv_style_set_text_color(&style_small, lv_color_make(0x80, 0x80, 0x80));
}

/* 使用字体样式 */
void create_label_with_font(lv_obj_t *parent, const char *text, int font_size)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    
    switch(font_size) {
    case 0: /* 标题 */
        lv_obj_add_style(label, &style_title, 0);
        break;
    case 1: /* 正常 */
        lv_obj_add_style(label, &style_normal, 0);
        break;
    case 2: /* 小字 */
        lv_obj_add_style(label, &style_small, 0);
        break;
    }
}
```

### 1.3 中文字体支持

要支持中文，需要自定义字体文件：

```c
/* 中文字体声明 */
LV_FONT_DECLARE(chinese_font_16);
LV_FONT_DECLARE(chinese_font_20);

/* 使用中文字体 */
lv_obj_t *chinese_label = lv_label_create(parent);
lv_label_set_text(chinese_label, "温度：23.5°C");
lv_obj_set_style_text_font(chinese_label, &chinese_font_16, 0);
```

---

## 图标设计和使用

### 2.1 创建16x16像素图标

#### 图标数据格式
```c
/* RGB565格式的16x16图标数据 */
static const uint16_t icon_temperature[16*16] = {
    /* 温度计图标 - 使用RGB565颜色值 */
    0x0000,0x0000,0x0000,0xF800,0xF800,0xF800,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0xF800,0xF800,0xFFFF,0xF800,0xF800,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    // ... 更多行数据
};

/* 湿度图标 */
static const uint16_t icon_humidity[16*16] = {
    /* 水滴图标 - 蓝色系 */
    0x0000,0x0000,0x0000,0x0000,0x001F,0x001F,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x001F,0x001F,0x001F,0x001F,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    // ... 更多行数据
};
```

#### RGB565颜色值参考
```c
/* 常用颜色RGB565值 */
#define COLOR_RED    0xF800  /* 红色 */
#define COLOR_GREEN  0x07E0  /* 绿色 */
#define COLOR_BLUE   0x001F  /* 蓝色 */
#define COLOR_WHITE  0xFFFF  /* 白色 */
#define COLOR_BLACK  0x0000  /* 黑色 */
#define COLOR_YELLOW 0xFFE0  /* 黄色 */
#define COLOR_CYAN   0x07FF  /* 青色 */
#define COLOR_ORANGE 0xFD20  /* 橙色 */
```

### 2.2 图标创建函数

```c
/* 创建图标的通用函数 */
static lv_obj_t *create_colored_icon(lv_obj_t *parent, const uint16_t *icon_data, 
                                     int x, int y, const char *name)
{
    static lv_image_dsc_t img_dsc;
    
    /* 配置图像描述符 */
    img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    img_dsc.header.flags = 0;
    img_dsc.header.w = 16;
    img_dsc.header.h = 16;
    img_dsc.header.stride = 16 * 2;
    img_dsc.data_size = 16 * 16 * 2;
    img_dsc.data = (const uint8_t *)icon_data;
    
    /* 创建图像对象 */
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &img_dsc);
    lv_obj_set_pos(img, x, y);
    
    return img;
}

/* 带文字的图标卡片 */
static lv_obj_t *create_icon_card(lv_obj_t *parent, const uint16_t *icon_data,
                                  const char *title, const char *value,
                                  int x, int y, lv_color_t bg_color)
{
    /* 创建卡片容器 */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 70, 60);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, bg_color, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    
    /* 添加图标 */
    create_colored_icon(card, icon_data, 5, 5, title);
    
    /* 添加标题 */
    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(title_label, 25, 8);
    
    /* 添加数值 */
    lv_obj_t *value_label = lv_label_create(card);
    lv_label_set_text(value_label, value);
    lv_obj_set_style_text_color(value_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(value_label, 10, 35);
    
    return card;
}
```

### 2.3 图标设计工具

可以使用以下工具设计图标：
1. **在线像素编辑器**：Piskel.app
2. **图片转换工具**：LVGL Image Converter
3. **手工编码**：直接编写RGB565数组

---

## 图片显示

### 3.1 显示外部图片文件

#### PNG/JPEG支持配置
```ini
# 在 prj.conf 中启用图片解码器
CONFIG_LV_USE_PNG=y
CONFIG_LV_USE_JPEG=y
CONFIG_LV_USE_BMP=y
CONFIG_LV_USE_GIF=y
```

#### 从文件系统加载图片
```c
/* 显示SD卡或Flash中的图片 */
void display_image_from_file(lv_obj_t *parent, const char *file_path, int x, int y)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, file_path);  /* 例如: "S:/images/logo.png" */
    lv_obj_set_pos(img, x, y);
    
    /* 可选：设置图片大小 */
    lv_obj_set_size(img, 100, 100);
    lv_image_set_scale(img, 256);  /* 原始大小 */
}
```

### 3.2 嵌入式图片数据

#### 大图片数据定义
```c
/* 32x32像素的LOGO图片 */
static const uint16_t logo_image[32*32] = {
    /* RGB565格式的图片数据 */
    COLOR_WHITE, COLOR_WHITE, COLOR_BLACK, COLOR_BLACK, /* ... 更多数据 */
    // 总共1024个RGB565值
};

/* 创建大图片的函数 */
static lv_obj_t *create_logo_image(lv_obj_t *parent, int x, int y)
{
    static lv_image_dsc_t logo_dsc = {
        .header.magic = LV_IMAGE_HEADER_MAGIC,
        .header.cf = LV_COLOR_FORMAT_RGB565,
        .header.flags = 0,
        .header.w = 32,
        .header.h = 32,
        .header.stride = 32 * 2,
        .data_size = 32 * 32 * 2,
        .data = (const uint8_t *)logo_image
    };
    
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &logo_dsc);
    lv_obj_set_pos(img, x, y);
    
    return img;
}
```

### 3.3 动态图片处理

```c
/* 图片缩放和旋转 */
void animate_image(lv_obj_t *img)
{
    /* 缩放动画 */
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, img);
    lv_anim_set_values(&scale_anim, 256, 512);  /* 从100%到200% */
    lv_anim_set_time(&scale_anim, 1000);
    lv_anim_set_exec_cb(&scale_anim, (lv_anim_exec_xcb_t)lv_image_set_scale);
    lv_anim_start(&scale_anim);
    
    /* 旋转动画 */
    lv_anim_t rotate_anim;
    lv_anim_init(&rotate_anim);
    lv_anim_set_var(&rotate_anim, img);
    lv_anim_set_values(&rotate_anim, 0, 3600);  /* 0到360度 */
    lv_anim_set_time(&rotate_anim, 2000);
    lv_anim_set_exec_cb(&rotate_anim, (lv_anim_exec_xcb_t)lv_image_set_rotation);
    lv_anim_start(&rotate_anim);
}
```

---

## 页面切换

### 4.1 页面管理系统

#### 页面枚举和结构
```c
/* 页面类型定义 */
typedef enum {
    PAGE_HOME = 0,
    PAGE_SETTINGS,
    PAGE_CHARTS,
    PAGE_INFO,
    PAGE_COUNT
} page_type_t;

/* 页面管理器结构 */
typedef struct {
    lv_obj_t *screens[PAGE_COUNT];
    page_type_t current_page;
    lv_obj_t *nav_buttons[PAGE_COUNT];
} page_manager_t;

static page_manager_t page_mgr = {0};
```

#### 页面创建函数
```c
/* 创建主页 */
static void create_home_page(void)
{
    page_mgr.screens[PAGE_HOME] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(page_mgr.screens[PAGE_HOME], lv_color_black(), 0);
    
    /* 添加主页内容 */
    lv_obj_t *title = lv_label_create(page_mgr.screens[PAGE_HOME]);
    lv_label_set_text(title, "主页");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_center(title);
}

/* 创建设置页 */
static void create_settings_page(void)
{
    page_mgr.screens[PAGE_SETTINGS] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(page_mgr.screens[PAGE_SETTINGS], lv_color_make(0x20, 0x20, 0x40), 0);
    
    /* 添加设置内容 */
    lv_obj_t *title = lv_label_create(page_mgr.screens[PAGE_SETTINGS]);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_pos(title, 10, 10);
    
    /* 添加设置选项 */
    create_setting_item(page_mgr.screens[PAGE_SETTINGS], "亮度", "80%", 10, 40);
    create_setting_item(page_mgr.screens[PAGE_SETTINGS], "音量", "50%", 10, 70);
}
```

### 4.2 页面切换动画

```c
/* 页面切换函数 */
static void switch_to_page(page_type_t new_page)
{
    if (new_page == page_mgr.current_page) return;
    
    lv_obj_t *old_screen = page_mgr.screens[page_mgr.current_page];
    lv_obj_t *new_screen = page_mgr.screens[new_page];
    
    /* 滑动切换动画 */
    lv_screen_load_anim(new_screen, LV_SCREEN_TRANS_SLIDE_LEFT, 300, 0, false);
    
    page_mgr.current_page = new_page;
    
    /* 更新导航按钮状态 */
    update_navigation_buttons();
    
    LOG_INF("切换到页面: %d", new_page);
}

/* 更新导航按钮状态 */
static void update_navigation_buttons(void)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == page_mgr.current_page) {
            lv_obj_set_style_bg_color(page_mgr.nav_buttons[i], lv_color_make(0x00, 0x80, 0xFF), 0);
        } else {
            lv_obj_set_style_bg_color(page_mgr.nav_buttons[i], lv_color_make(0x40, 0x40, 0x40), 0);
        }
    }
}
```

### 4.3 手势页面切换

```c
/* 手势事件处理 */
static void gesture_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target_obj(e);
    
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        
        switch (dir) {
        case LV_DIR_LEFT:
            /* 向左滑动 - 下一页 */
            if (page_mgr.current_page < PAGE_COUNT - 1) {
                switch_to_page(page_mgr.current_page + 1);
            }
            break;
        case LV_DIR_RIGHT:
            /* 向右滑动 - 上一页 */
            if (page_mgr.current_page > 0) {
                switch_to_page(page_mgr.current_page - 1);
            }
            break;
        }
    }
}

/* 启用手势支持 */
void enable_page_gestures(lv_obj_t *screen)
{
    lv_obj_add_event_cb(screen, gesture_event_handler, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_GESTURE_BUBBLE);
}
```

---

## 主题切换

### 5.1 主题定义

#### 深色主题
```c
/* 深色主题颜色定义 */
typedef struct {
    lv_color_t bg_primary;
    lv_color_t bg_secondary;
    lv_color_t text_primary;
    lv_color_t text_secondary;
    lv_color_t accent;
    lv_color_t success;
    lv_color_t warning;
    lv_color_t error;
} theme_colors_t;

static const theme_colors_t dark_theme = {
    .bg_primary = {.red = 0x10, .green = 0x10, .blue = 0x10},     /* 深灰背景 */
    .bg_secondary = {.red = 0x20, .green = 0x20, .blue = 0x20},   /* 次要背景 */
    .text_primary = {.red = 0xFF, .green = 0xFF, .blue = 0xFF},   /* 白色文字 */
    .text_secondary = {.red = 0xC0, .green = 0xC0, .blue = 0xC0}, /* 灰色文字 */
    .accent = {.red = 0x00, .green = 0x80, .blue = 0xFF},         /* 蓝色强调 */
    .success = {.red = 0x00, .green = 0xFF, .blue = 0x00},        /* 绿色成功 */
    .warning = {.red = 0xFF, .green = 0x80, .blue = 0x00},        /* 橙色警告 */
    .error = {.red = 0xFF, .green = 0x00, .blue = 0x00}           /* 红色错误 */
};

/* 浅色主题 */
static const theme_colors_t light_theme = {
    .bg_primary = {.red = 0xFF, .green = 0xFF, .blue = 0xFF},     /* 白色背景 */
    .bg_secondary = {.red = 0xF0, .green = 0xF0, .blue = 0xF0},   /* 浅灰背景 */
    .text_primary = {.red = 0x00, .green = 0x00, .blue = 0x00},   /* 黑色文字 */
    .text_secondary = {.red = 0x60, .green = 0x60, .blue = 0x60}, /* 深灰文字 */
    .accent = {.red = 0x00, .green = 0x60, .blue = 0xC0},         /* 深蓝强调 */
    .success = {.red = 0x00, .green = 0xC0, .blue = 0x00},        /* 深绿成功 */
    .warning = {.red = 0xC0, .green = 0x60, .blue = 0x00},        /* 深橙警告 */
    .error = {.red = 0xC0, .green = 0x00, .blue = 0x00}           /* 深红错误 */
};
```

### 5.2 主题应用函数

```c
/* 当前主题 */
static const theme_colors_t *current_theme = &dark_theme;
static bool is_dark_theme = true;

/* 应用主题到对象 */
static void apply_theme_to_object(lv_obj_t *obj, int style_type)
{
    switch (style_type) {
    case 0: /* 主要背景 */
        lv_obj_set_style_bg_color(obj, current_theme->bg_primary, 0);
        break;
    case 1: /* 次要背景 */
        lv_obj_set_style_bg_color(obj, current_theme->bg_secondary, 0);
        break;
    case 2: /* 主要文字 */
        lv_obj_set_style_text_color(obj, current_theme->text_primary, 0);
        break;
    case 3: /* 次要文字 */
        lv_obj_set_style_text_color(obj, current_theme->text_secondary, 0);
        break;
    case 4: /* 强调色 */
        lv_obj_set_style_bg_color(obj, current_theme->accent, 0);
        break;
    }
}

/* 切换主题 */
void toggle_theme(void)
{
    is_dark_theme = !is_dark_theme;
    current_theme = is_dark_theme ? &dark_theme : &light_theme;
    
    /* 重新应用主题到所有UI元素 */
    refresh_all_ui_elements();
    
    LOG_INF("主题切换到: %s", is_dark_theme ? "深色" : "浅色");
}

/* 刷新所有UI元素 */
static void refresh_all_ui_elements(void)
{
    /* 更新状态栏 */
    apply_theme_to_object(app.status_bar, 1);
    apply_theme_to_object(app.time_label, 2);
    
    /* 更新内容区域 */
    apply_theme_to_object(app.content_area, 0);
    
    /* 更新导航栏 */
    apply_theme_to_object(app.bottom_nav, 1);
    
    /* 触发重绘 */
    lv_obj_invalidate(lv_screen_active());
}
```

### 5.3 主题切换按钮

```c
/* 主题切换按钮事件 */
static void theme_toggle_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        toggle_theme();
        
        /* 更新按钮图标 */
        lv_obj_t *btn = lv_event_get_target_obj(e);
        lv_obj_t *icon = lv_obj_get_child(btn, 0);
        if (is_dark_theme) {
            /* 显示月亮图标 */
            create_colored_icon(btn, icon_moon, 0, 0, "dark");
        } else {
            /* 显示太阳图标 */
            create_colored_icon(btn, icon_sun, 0, 0, "light");
        }
    }
}

/* 创建主题切换按钮 */
static void create_theme_toggle_button(lv_obj_t *parent)
{
    lv_obj_t *theme_btn = lv_btn_create(parent);
    lv_obj_set_size(theme_btn, 30, 20);
    lv_obj_set_pos(theme_btn, 80, 2);
    lv_obj_add_event_cb(theme_btn, theme_toggle_event_handler, LV_EVENT_CLICKED, NULL);
    
    /* 初始图标 */
    create_colored_icon(theme_btn, icon_sun, 5, 2, "theme");
}
```

---

## 实用技巧

### 6.1 性能优化

```c
/* 减少重绘次数 */
void optimize_rendering(void)
{
    /* 批量更新UI */
    lv_obj_invalidate(lv_screen_active());
    
    /* 使用静态样式 */
    static lv_style_t cached_style;
    lv_style_init(&cached_style);
    /* 配置样式... */
    
    /* 重用对象而不是重新创建 */
    lv_label_set_text(existing_label, new_text);  /* 好 */
    // lv_obj_del(old_label); lv_label_create(parent);  /* 避免 */
}
```

### 6.2 内存管理

```c
/* 正确的对象生命周期管理 */
void cleanup_ui_objects(void)
{
    /* 删除不需要的对象 */
    if (temp_dialog != NULL) {
        lv_obj_del(temp_dialog);
        temp_dialog = NULL;
    }
    
    /* 清理样式 */
    lv_style_reset(&temp_style);
}
```

### 6.3 响应式设计

```c
/* 根据屏幕大小调整布局 */
void adaptive_layout(int screen_width, int screen_height)
{
    if (screen_width < 240) {
        /* 小屏幕布局 */
        lv_obj_set_size(app.temp_card, 60, 50);
    } else {
        /* 正常屏幕布局 */
        lv_obj_set_size(app.temp_card, 70, 60);
    }
}
```

### 6.4 调试技巧

```c
/* 添加调试信息 */
void debug_ui_info(lv_obj_t *obj)
{
    lv_coord_t x = lv_obj_get_x(obj);
    lv_coord_t y = lv_obj_get_y(obj);
    lv_coord_t w = lv_obj_get_width(obj);
    lv_coord_t h = lv_obj_get_height(obj);
    
    LOG_DBG("对象位置: (%d, %d), 大小: %dx%d", x, y, w, h);
}
```

---

## 总结

本指南涵盖了ESP32-S3 LVGL项目中的主要定制功能：

1. **字体系统**：支持多种大小和样式的字体
2. **图标设计**：16x16像素RGB565格式图标
3. **图片显示**：支持嵌入式和外部图片
4. **页面管理**：完整的页面切换系统
5. **主题系统**：深色/浅色主题切换

通过这些技术，您可以创建专业级的嵌入式GUI界面，满足各种应用需求。

### 相关文件
- `main.c` - 主要实现代码
- `prj.conf` - 项目配置
- `README.md` - 项目说明

### 技术支持
- LVGL官方文档：https://docs.lvgl.io/
- Zephyr RTOS文档：https://docs.zephyrproject.org/
- ESP32-S3参考手册：https://www.espressif.com/
