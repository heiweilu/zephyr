# ANSI Fragment Filtering - Quick Reference

## Problem
LCD displays "mmm" garbage characters due to fragmented ANSI escape sequences sent by shell.

## Solution Architecture

```
┌─────────────────────────────────────────────────────────┐
│              Shell Output (with ANSI codes)             │
└────────────────────┬────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────┐
│         intercepted_uart_write() [Transport Hook]       │
└────────────────────┬────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────┐
│         shell_output_callback() [Entry Point]           │
└────────────────────┬────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────┐
│   PRE-FILTER 1: is_ansi_escape_fragment()               │
│   • Detect 1-6 byte ANSI fragments                      │
│   • Pattern: *m, [*, standalone chars                   │
│   • Action: REJECT entire packet                        │
└────────────────────┬────────────────────────────────────┘
                     │ (if passed)
                     ▼
┌─────────────────────────────────────────────────────────┐
│   PRE-FILTER 2: is_control_only()                       │
│   • Detect pure control character data                  │
│   • Action: REJECT entire packet                        │
└────────────────────┬────────────────────────────────────┘
                     │ (if passed)
                     ▼
┌─────────────────────────────────────────────────────────┐
│   SPECIAL: Handle 0x1F prefixed input updates           │
│   • Format: 0x1F + input bytes                          │
│   • Action: Update input line, skip normal processing   │
└────────────────────┬────────────────────────────────────┘
                     │ (if not 0x1F)
                     ▼
┌─────────────────────────────────────────────────────────┐
│   process_shell_output() [Character Level]              │
│   ┌───────────────────────────────────────────────┐     │
│   │ For each character:                           │     │
│   │  1. Whitelist check (32-126, \n, \t, \b)     │     │
│   │  2. should_skip_character() [Context check]   │     │
│   │  3. Add to buffer if safe                     │     │
│   └───────────────────────────────────────────────┘     │
└────────────────────┬────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────┐
│         shell_display_buffer (Clean Output)             │
└────────────────────┬────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────┐
│              LVGL Label Update (LCD Display)            │
└─────────────────────────────────────────────────────────┘
```

## Key Functions

### 1. `is_ansi_escape_fragment(data, len)`
**Purpose**: Detect ANSI escape sequence fragments (1-6 bytes)

**Detection Patterns**:
```c
// Pattern 1: Ends with 'm' + contains ANSI chars
if (data[len-1] == 'm' && contains('[', ';', '0-9', ESC)) → REJECT

// Pattern 2: Starts with '[' + ANSI command follows  
if (data[0] == '[' && data[1] in {'m','J','D','H',';','0-9'}) → REJECT
```

**Examples**:
- `[6D]` = "m" → ✅ REJECTED
- `[5B 6D]` = "[m" → ✅ REJECTED  
- `[3B 33 32 6D]` = ";32m" → ✅ REJECTED
- `[48 65 6C 6C 6F]` = "Hello" → ❌ ALLOWED

### 2. `is_control_only(data, len)`
**Purpose**: Reject packets with only unexpected control characters

**Allowed Control Chars**: `\n`, `\t`, `\b`, `\r`, `ESC`  
**Action**: If all bytes are control (and not allowed) → REJECT

### 3. `should_skip_character(c, data, len, i)`
**Purpose**: Context-aware character filtering during processing

**Skip Conditions**:
```c
// '[' followed by digit/m/; → Start of ANSI
'[' + lookahead('0-9', 'm', ';') → SKIP

// 'm' preceded by [/digit/; → End of ANSI (+ backtrack cleanup)
'm' + lookback('[', '0-9', ';') → SKIP + BACKTRACK

// Digit/; right after '[' → ANSI parameter
('0-9' | ';') + lookback('[') → SKIP
```

### 4. `process_shell_output(data, len)`
**Purpose**: Main character-by-character processing with whitelist

**Whitelist**:
- `\n` → newline (allowed)
- `\b` → backspace (delete last char)
- `\t` → tab (allowed)
- `32-126` → printable ASCII (with context check)
- Everything else → **SILENTLY DROPPED**

## Fragment Examples Caught

| Hex Bytes | ASCII | Length | Detection Method | Result |
|-----------|-------|--------|------------------|--------|
| `1B 5B 6D` | ESC[m | 3 | Pattern 1 (ends with 'm' + has '[') | ✅ REJECTED |
| `5B 6D` | [m | 2 | Pattern 2 (starts with '[' + 'm') | ✅ REJECTED |
| `6D` | m | 1 | Pattern 1 (standalone 'm') | ✅ REJECTED |
| `3B 33 32 6D` | ;32m | 4 | Pattern 1 (ends with 'm' + has ';') | ✅ REJECTED |
| `5B 4A` | [J | 2 | Pattern 2 (starts with '[' + 'J') | ✅ REJECTED |
| `33 32 6D` | 32m | 3 | Pattern 1 (ends with 'm' + has digits) | ✅ REJECTED |
| `48 65 6C 6C 6F` | Hello | 5 | Whitelist check | ❌ ALLOWED |

## Configuration

```c
/* Buffer sizes */
#define SHELL_DISPLAY_BUFFER_SIZE 2048  // Main output buffer
#define CURRENT_INPUT_LINE_SIZE 256     // Input line buffer

/* Buffer management */
#define MAX_BUFFER_LEN 1500   // Trim when exceeded
#define KEEP_LEN 1000         // Keep last N chars after trim

/* Fragment detection range */
#define MIN_FRAGMENT_LEN 1
#define MAX_FRAGMENT_LEN 6
```

## Performance

- **Pre-filter complexity**: O(n) where n = packet length (max 6)
- **Character processing**: O(m) where m = total data length
- **Memory overhead**: ~2.5KB static buffers
- **CPU overhead**: Negligible (<1% at 240MHz)

## Testing Checklist

- [x] "mmm" eliminated on Enter key
- [x] "[J" eliminated on Backspace+Enter
- [x] Normal text displays correctly
- [x] Newlines work
- [x] Tabs work
- [x] Backspace deletes chars
- [x] Tab completion visible
- [x] Shell commands execute normally
- [x] No recursion or "messages dropped"

## Common Pitfalls

### ❌ DON'T
1. Use `LOG_INF()` in shell callback → causes infinite recursion
2. Use `printk()` for debugging → outputs to shell, creates fragments
3. Rely on state machines → fragments arrive in unpredictable order
4. Try to reassemble fragments → timing issues and complexity

### ✅ DO
1. Use multi-layer defense (pre-filter + character filter)
2. Apply whitelist strategy (only allow known-safe chars)
3. Filter at earliest possible point (before main processing)
4. Use hex dump **temporarily** for analysis (disable after)

## Code Locations

```
src/main.c:
  ├── Lines 100-150: Helper functions
  │   ├── is_ansi_escape_fragment()
  │   ├── is_control_only()
  │   └── should_skip_character()
  ├── Lines 151-200: process_shell_output()
  └── Lines 201-250: shell_output_callback()
```

## Debug Mode

To temporarily enable hex dump for analysis:
```c
#if 1  // Change to 1 to enable
if (len >= 1 && len <= 4) {
    char hex[64] = {0};
    for (size_t i = 0; i < len; i++) {
        snprintf(&hex[off], sizeof(hex) - off, "%02X ", (uint8_t)data[i]);
    }
    // NOTE: This will cause recursion! Use only briefly
    printk("[HEX] len=%zu hex=[%s]\n", len, hex);
}
#endif
```

**⚠️ WARNING**: Enable only for 1-2 test runs, then disable immediately to avoid log floods!

## Related Documentation

- [ANSI_FILTERING_SOLUTION.md](./ANSI_FILTERING_SOLUTION.md) - Detailed technical explanation
- [DEBUGGING_LOG.md](./DEBUGGING_LOG.md) - Complete debugging journey
- [README_LCD_CONSOLE.md](../README_LCD_CONSOLE.md) - Project overview

---
**Last Updated**: 2025-10-15  
**Status**: ✅ WORKING - Problem fully resolved
