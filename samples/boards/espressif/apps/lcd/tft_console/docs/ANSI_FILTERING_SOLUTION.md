# ANSI Escape Sequence Fragment Filtering Solution

## 问题概述

在ESP32-S3 LCD Shell Console项目中，LCD显示器会出现"mmm"等垃圾字符，特别是在按下Enter键后。经过深入调试，发现这些字符实际上是**ANSI转义序列的碎片**。

## 根本原因

### 1. ANSI转义序列碎片化

Shell子系统会发送ANSI转义序列用于终端控制（如颜色、光标移动等），但这些序列经常被**分片发送**到UART传输层：

**正常的完整序列示例：**
```
ESC[m        → 重置所有属性 (3字节: 1B 5B 6D)
ESC[1;32m    → 设置绿色粗体 (7字节: 1B 5B 31 3B 33 32 6D)
ESC[6D       → 光标左移6位 (4字节: 1B 5B 36 44)
```

**实际收到的碎片示例：**
```
[HEX] len=3 hex=[1B 5B 6D ]      ← ESC[m (完整序列，正常)
[HEX] len=2 hex=[5B 6D ]         ← [m (缺少ESC字节的碎片)
[HEX] len=1 hex=[6D ]            ← m (单独的'm'字符碎片)
[HEX] len=4 hex=[3B 33 32 6D ]   ← ;32m (颜色代码碎片)
[HEX] len=2 hex=[5B 4A ]         ← [J (清屏序列碎片)
[HEX] len=3 hex=[33 32 6D ]      ← 32m (碎片)
[HEX] len=2 hex=[32 6D ]         ← 2m (碎片)
```

### 2. 碎片化的原因

1. **Shell内部缓冲机制**：Shell可能在不同时刻分别发送转义序列的各个部分
2. **UART传输层分片**：数据可能在传输层被分割成多个小包
3. **调试日志递归**：使用`LOG_INF`或`printk`调试时，这些函数本身也输出到Shell，造成递归调用和额外的碎片

### 3. 为什么传统ANSI状态机失效

传统的ANSI解析器使用状态机，期望按顺序接收：
```
ESC → [ → 参数 → 命令字符
```

但当碎片到达时，状态机可能收到：
1. 第一个包：`[m` (没有ESC开头)
2. 第二个包：`m` (单独的命令字符)
3. 第三个包：`;32m` (中间部分)

状态机无法正确处理这些不完整的序列，导致部分字符被当作普通文本显示。

## 解决方案

### 核心策略：Ultra-Aggressive Pre-Filtering

在数据进入主处理循环**之前**，使用激进的预过滤器拦截所有ANSI碎片：

#### 1. ANSI碎片检测函数

```c
static bool is_ansi_escape_fragment(const char *data, size_t len)
{
	if (len < 1 || len > 6) {
		return false;
	}
	
	/* 模式1: 拒绝任何以'm'结尾的序列 */
	if (data[len-1] == 'm') {
		bool is_ansi_garbage = false;
		
		/* 检查是否包含ANSI特征: '[', ';', 数字, 或ESC */
		for (size_t i = 0; i < len; i++) {
			char c = data[i];
			if (c == '[' || c == ';' || (c >= '0' && c <= '9') || c == 0x1B) {
				is_ansi_garbage = true;
				break;
			}
		}
		
		/* 拒绝单个'm'字符或包含ANSI特征的序列 */
		if (len == 1 || is_ansi_garbage) {
			return true;
		}
	}
	
	/* 模式2: 拒绝以'['开头的ANSI碎片 */
	if (data[0] == '[' && len >= 2) {
		char second = data[1];
		/* 如果第二个字符是数字、'm'、'J'、'D'等ANSI命令 */
		if (second == 'm' || second == 'J' || second == 'D' || second == 'H' ||
		    second == ';' || (second >= '0' && second <= '9')) {
			return true;
		}
	}
	
	return false;
}
```

#### 2. 字符级过滤

即使通过了预过滤，在字符处理阶段仍需进一步检查：

```c
static bool should_skip_character(char c, const char *data, size_t len, size_t i)
{
	/* '[' 后跟数字或'm' → ANSI序列的一部分 */
	if (c == '[' && i + 1 < len) {
		char next = data[i + 1];
		if ((next >= '0' && next <= '9') || next == 'm' || next == ';') {
			return true;
		}
	}
	
	/* 'm' 前面是'['或数字 → ANSI序列的结束 */
	if (c == 'm' && shell_display_len > 0) {
		char prev = shell_display_buffer[shell_display_len - 1];
		if (prev == '[' || (prev >= '0' && prev <= '9') || prev == ';') {
			/* 回退删除缓冲区中的ANSI垃圾 */
			while (shell_display_len > 0) {
				char ch = shell_display_buffer[shell_display_len - 1];
				if (ch == '[' || (ch >= '0' && ch <= '9') || ch == ';') {
					shell_display_len--;
				} else {
					break;
				}
			}
			return true;
		}
	}
	
	/* 数字或';'紧跟在'['后面 → ANSI参数 */
	if ((c >= '0' && c <= '9') || c == ';') {
		if (shell_display_len > 0 && shell_display_buffer[shell_display_len - 1] == '[') {
			return true;
		}
	}
	
	return false;
}
```

#### 3. 白名单策略

只允许以下字符通过：
- **可打印ASCII** (32-126): 空格到波浪线
- **换行符** ('\n'): 用于文本换行
- **制表符** ('\t'): 用于缩进
- **退格符** ('\b'): 用于删除字符

**所有其他字符一律丢弃**，包括：
- ESC (0x1B)
- 控制字符 (0x00-0x1F, 0x7F-0xFF)
- ANSI序列的碎片

### 实现流程

```
输入数据
    ↓
预过滤器: is_ansi_escape_fragment()
    ↓ (通过)
控制字符检查: is_control_only()
    ↓ (通过)
特殊消息处理 (0x1F前缀)
    ↓ (如果不是)
字符级处理循环:
    - 白名单检查 (32-126, \n, \t, \b)
    - 上下文过滤: should_skip_character()
    ↓
添加到显示缓冲区
    ↓
标记需要刷新显示
```

## 调试过程中的关键发现

### 1. LOG递归问题

使用`LOG_INF()`调试时会造成无限递归：
```
Shell输出 → LOG_INF() → 输出到Shell → 被拦截 → 调用LOG_INF() → ...
```

**解决**: 完全禁用调试日志，或使用与Shell隔离的日志后端。

### 2. printk递归问题

即使改用`printk()`，仍然会造成递归和大量"messages dropped"错误，因为`printk()`也输出到Shell。

### 3. Hex Dump分析的价值

通过添加临时hex dump功能，精确识别了碎片模式：
```c
char hex[64] = {0};
for (size_t i = 0; i < len; i++) {
    int off = strlen(hex);
    snprintf(&hex[off], sizeof(hex) - off, "%02X ", (uint8_t)data[i]);
}
printk("[HEX] len=%zu hex=[%s]\n", len, hex);
```

这帮助我们发现了：
- 碎片的确切字节模式
- 碎片的长度范围（1-6字节）
- 最常见的碎片类型（`[m`, `m`, `;32m`等）

## 最终效果

✅ **LCD显示完全干净** - 没有"mmm"或其他ANSI垃圾字符  
✅ **Enter键正常** - 不再出现额外字符  
✅ **Backspace正常** - 退格删除工作正确  
✅ **Tab补全正常** - 命令补全可见且正确  
✅ **Shell功能完整** - 所有命令正常工作  

## 代码架构

### 核心函数

1. **`is_ansi_escape_fragment()`** - 检测ANSI碎片
2. **`is_control_only()`** - 检测纯控制字符数据
3. **`should_skip_character()`** - 上下文相关的字符过滤
4. **`process_shell_output()`** - 主处理函数
5. **`shell_output_callback()`** - Shell输出回调入口

### 数据流

```
Shell Transport Write
        ↓
intercepted_uart_write()
        ↓
shell_output_callback()
        ↓
is_ansi_escape_fragment() [预过滤]
        ↓
is_control_only() [控制字符检查]
        ↓
process_shell_output() [字符级处理]
        ↓
shell_display_buffer [显示缓冲区]
        ↓
LVGL Label Update [LCD显示]
```

## 经验教训

### ✅ 有效的策略

1. **激进的预过滤** - 在数据进入主流程前拦截碎片
2. **白名单方法** - 只允许已知安全的字符通过
3. **多层防御** - 预过滤 + 字符级过滤 + 上下文检查
4. **Hex dump分析** - 精确定位问题模式

### ❌ 无效的策略

1. **传统ANSI状态机** - 无法处理碎片化输入
2. **持久化状态机** - 碎片顺序不可预测导致状态混乱
3. **模式匹配** - 碎片模式过多无法穷举
4. **日志调试** - 造成递归和更多问题

## 适用场景

此解决方案适用于：
- 嵌入式系统中的LCD/TFT Shell显示
- UART传输可能分片的场景
- 需要过滤ANSI转义序列的应用
- 资源受限无法使用完整终端模拟器的场景

## 未来优化方向

1. **性能优化** - 当前逐字符处理，可优化为批量处理
2. **缓冲区管理** - 可使用环形缓冲区减少memmove开销
3. **可配置性** - 允许用户自定义白名单字符集
4. **统计信息** - 记录过滤掉的碎片数量用于调试

## 参考资料

- [ANSI Escape Codes](https://en.wikipedia.org/wiki/ANSI_escape_code)
- [Zephyr Shell Documentation](https://docs.zephyrproject.org/latest/services/shell/index.html)
- [LVGL Label Documentation](https://docs.lvgl.io/master/widgets/label.html)

## 作者与版权

- **作者**: Heiweilu
- **日期**: 2025年10月15日
- **许可**: Apache-2.0
- **项目**: ESP32-S3 TFT Console Sample for Zephyr RTOS
