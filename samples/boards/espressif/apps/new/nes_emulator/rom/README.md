# NES ROM 管理

## 当前内置 ROM

| 文件 | 说明 | Mapper | 大小 |
|------|------|--------|------|
| `sbp.nes` | **Super Bat Puncher Demo** — 平台动作游戏（当前默认） | 1 (MMC1) | 128KB |
| `nestest.nes` | CPU 精度测试 ROM（黑屏，仅验证核心） | 0 (NROM) | 24KB |
| `nrom-template.nes` | NROM 模板（基础行走测试） | 0 (NROM) | 24KB |
| `roborun.nes` | RoboRun（⚠️ PAL 专用，NTSC 不兼容） | 0 (NROM) | 40KB |

## 如何更换游戏 ROM

### 第1步：下载 ROM

推荐从以下 **合法免费** 的 Homebrew 游戏站点下载：

| 站点 | 地址 | 说明 |
|------|------|------|
| **itch.io** | https://itch.io/games/tag-nes | 大量免费/付费 NES homebrew |
| **pineight.com** | https://pineight.com/nes/ | Damian Yerrick 的免费 NES homebrew |
| **Morphcat Games** | http://morphcat.de/ | Super Bat Puncher 等精品 |
| **NESdev Wiki** | https://www.nesdev.org/wiki/Homebrew_games | 社区作品合集 |
| **Homebrew Hub** | https://hh.gbdev.io/games/nes | NES homebrew 数据库 |

### 第2步：确认兼容性

ROM 必须满足以下条件：
- **iNES 格式**（`.nes` 文件，以 `NES\x1A` 开头）
- **NTSC 制式**（60Hz，262 扫描线 — nofrendo 不支持 PAL 312 扫描线）
- **Mapper 支持**：nofrendo 已编译 Mapper 0-5, 7-9, 11, 15-16, 18-19, 24, 32-34, 40-42, 46, 50, 64-66, 70, 73, 75, 78-79, 85, 87, 93-94, 99, 160, 229, 231

可用 Python 快速检查 ROM 头：
```python
import struct
with open("game.nes", "rb") as f:
    hdr = f.read(16)
    magic = hdr[:4]
    prg = hdr[4] * 16  # KB
    chr_ = hdr[5] * 8   # KB
    mapper = ((hdr[6] & 0xF0) >> 4) | (hdr[7] & 0xF0)
    print(f"Magic: {magic}, PRG: {prg}KB, CHR: {chr_}KB, Mapper: {mapper}")
```

### 第3步：替换 ROM 文件

1. 将 `.nes` 文件复制到本目录（`nes_emulator/rom/`）
2. 编辑 `CMakeLists.txt`，修改 `NES_ROM_FILE` 路径：
   ```cmake
   set(NES_ROM_FILE "${CMAKE_CURRENT_LIST_DIR}/rom/your_game.nes")
   ```
3. 重新编译烧录：
   ```powershell
   west build -d build/chd_nes_emulator
   west flash -d build/chd_nes_emulator --esp-device COM5
   ```

### 操控映射

| 键盘按键 | NES 按钮 | 说明 |
|----------|----------|------|
| ↑ / W | D-pad Up | 上 |
| ↓ / S | D-pad Down | 下 |
| ← / A | D-pad Left | 左 |
| → / D | D-pad Right | 右 |
| K / Z | A 键 | 动作/跳跃 |
| J / X | B 键 | 攻击/取消 |
| Enter | Start | 开始/暂停 |
| Space | Select | 选择 |
| ESC | Hard Reset | 重启 |

> **法律提醒**：请仅使用你拥有合法副本的 ROM，或公开可用的 Homebrew / 测试 ROM。
