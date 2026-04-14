# Voice Assistant — ESP32-S3 AI 语音助手

![Zephyr](https://img.shields.io/badge/RTOS-Zephyr-blue)
![ESP32-S3](https://img.shields.io/badge/MCU-ESP32--S3-red)
![Qwen Omni](https://img.shields.io/badge/AI-Qwen3.5--Omni--Flash-green)
![License](https://img.shields.io/badge/License-Apache--2.0-orange)

Push-to-Talk 语音助手，运行在 Zephyr RTOS + ESP32-S3 上，通过阿里云百炼 DashScope API 调用 `qwen3.5-omni-flash` 全模态大模型实现语音交互。

## 工作流程

```
按住 BOOT → 录音(ES7210 16kHz) → 松开 → base64 WAV 编码
  → TLS HTTPS POST(SSE 流式) → qwen3.5-omni-flash
  → 解析文本 + 音频响应 → 24kHz→16kHz 重采样 → ES8311 播放
```

## 硬件需求

| 组件 | 型号 | 接口 |
|------|------|------|
| MCU | ESP32-S3 (8MB PSRAM) | — |
| 麦克风 ADC | ES7210 | I2S + I2C (0x40) |
| 扬声器 DAC | ES8311 | I2S + I2C (0x18) |
| PA 功放 | — | GPIO IO46 |
| 按键 | BOOT (IO0) | GPIO |

## API 参数

| 参数 | 值 | 说明 |
|------|------|------|
| `model` | `qwen3.5-omni-flash` | 阿里云百炼全模态模型 |
| `enable_thinking` | `false` | 显式禁用深度思考，确保最优响应速度 |
| `modalities` | `["text", "audio"]` | 同时输出文本和音频 |
| `audio.voice` | `Tina` | 音色选择 |
| `stream` | `true` | 必须流式（Qwen-Omni 强制要求） |

## PSRAM 内存布局

| 缓冲区 | 大小 | 用途 |
|--------|------|------|
| `record_pcm` | 320 KB | 录音 PCM (5s × 16kHz × 16bit) |
| `b64_wav_buf` | 220 KB | base64 编码后的 WAV |
| `rsp_audio_b64` | 512 KB | 响应音频 base64 累积 |
| `rsp_pcm_24k` | 720 KB | 响应 PCM (24kHz) |
| `playback_pcm` | 960 KB | 播放 PCM (16kHz, 30s) |
| `sse_line_buf` | 64 KB | SSE 行解析缓冲 |

## 编译 & 烧录

```powershell
# 编译
west build -b esp32s3_devkitc/esp32s3/procpu \
    samples/boards/espressif/apps/new/voice_assistant \
    -d build/voice_assistant -p

# 烧录
west flash -d build/voice_assistant --esp-device COM5
```

## 配置

在 `src/secrets.h` 中配置：

```c
#define WIFI_SSID     "your_wifi_ssid"
#define WIFI_PSK      "your_wifi_password"
#define AI_API_KEY    "sk-your-dashscope-api-key"
```

## 文件结构

```
voice_assistant/
├── CMakeLists.txt
├── prj.conf
├── README.md
├── boards/
│   └── esp32s3_devkitc_procpu.overlay
└── src/
    ├── main.c          # 主逻辑：WiFi/TLS/HTTP/SSE/按键/录音/播放
    ├── audio.c/h       # ES8311 DAC + ES7210 ADC + I2S 初始化
    ├── base64.c/h      # base64 编解码
    ├── ca_certificate.h # TLS CA 证书
    └── secrets.h       # WiFi 和 API 密钥（不提交）
```
