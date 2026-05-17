solenoid_test
##############

5V 电磁推杆（KK-1040B）+ 4 路继电器模块的最小驱动测试用例。
跑通后可作为 ``auto-turret-1130`` 整机的 *开炮* 子模块基线。

硬件清单
========

==================== ===========================================================
项                   规格 / 备注
==================== ===========================================================
ESP32-S3-BOX(-3)     主控板（自带 USB Type-C 串口 + 板载 BOOT 键）
4 路继电器模块       5V 供电、光耦隔离、低电平触发；触点 10A/30VDC
KK-1040B 电磁推杆    5V，标称 1.8A，实测启动峰值约 3A
DP100 可调电源       直接设为 5.0V / 限流 4A 输出，**不需要 XL4016**
导线 + 端子          AWG18 以上，承载推杆 3A
==================== ===========================================================

注意:
  本 sample **没有** 加 D4184 MOS / 大电容缓冲 / 续流二极管。
  目的是先验证「能不能在你现有模块上把推杆推动起来」。
  实测后再决定是否加保护模块。

接线
====

ESP32-S3-BOX-3 → 4 路继电器模块（信号侧）::

    BOX  GND     ──── 继电器 GND（只走信号地，电流极小）
    BOX  GPIO38  ──── 继电器 IN1
    DP100 +      ──── 继电器 VCC（**继电器信号侧 5V 也用 DP100，BOX-3 用户排针不输出 5V**）

如继电器板上有 ``JD-VCC / VCC`` 跳线帽，请保留默认（共电）；
若你的板子区分了 JD-VCC（线圈电）/ VCC（光耦逻辑电），两者都接 DP100 +5V 即可。

DP100 → 4 路继电器模块（功率侧）+ 推杆::

    DP100：先把电压调到 5.00V，限流先调到 4.0A
    （如果你的 DP100 是 0–32V 6A 那个常见型号，5V/4A 在它能力内）

    DP100 +  ─────────── 继电器 CH1 COM
    继电器 CH1 NO ────── 推杆 红线 (+)
    DP100 −  ─────────── 推杆 黑线 (−)

公共 GND（必须）::

    DP100 GND（功率地）= ESP32 GND = 继电器 GND

最小验证只接：DP100 → 继电器 CH1 → 推杆；
ESP32 USB 给自己供电（不要从 ESP32 的 5V 输出去拉推杆电流，会复位）。

引脚换其它 GPIO？
   修改 ``boards/esp32s3_devkitc_procpu.overlay`` 里 ``relay0`` 节点的
   ``gpios = <&gpio1 6 GPIO_ACTIVE_LOW>``。
   ESP32-S3 的 GPIO 大于等于 32 用 ``gpio1``，序号 = (gpio - 32)；
   GPIO 0–31 用 ``gpio0``。

行为
====

1. 上电 / 复位后每 3 秒自动打一发：继电器 ON 80 ms，OFF 2920 ms。
2. 按一下板上 BOOT 键立即手动单发一次。
3. 每次开炮在 USB 串口打印一行 ``FIRE #N (auto|button, pulse=80ms)``。

调整开炮节奏 / 脉宽
   修改 ``src/main.c`` 顶部 ``PULSE_MS`` / ``AUTO_PERIOD_MS``。
   推杆若打不到底，可逐步把 ``PULSE_MS`` 加到 100–150 ms；
   推杆若发烫，往下减到 60 ms。

编译 & 烧录
===========

.. code-block:: console

    cd <ZEPHYR_BASE>

    west build -b esp32s3_devkitc/esp32s3/procpu \
        samples/boards/espressif/esp32-s3-box/solenoid_test \
        -d build/solenoid_test

    west flash --build-dir build/solenoid_test

打开串口
========

烧录完成后 BOX 自带 USB Type-C 直接当串口（CDC-ACM）。
查看实际 COM 号（PowerShell）::

    Get-WmiObject Win32_SerialPort | Select-Object Name, DeviceID

用任意串口工具 115200 8N1 打开，应看到::

    [00:00:00.123,000] <inf> solenoid_test: solenoid_test ready: relay=GPIO38 (ACTIVE_LOW), button=BOOT (GPIO0)
    [00:00:00.124,000] <inf> solenoid_test: auto-fire every 3000 ms, pulse=80 ms; press BOOT for manual shot
    [00:00:02.920,000] <inf> solenoid_test: FIRE #1 (auto, pulse=80ms)
    [00:00:05.840,000] <inf> solenoid_test: FIRE #2 (auto, pulse=80ms)

测试步骤
========

#. **空载测继电器**：先**不接推杆**，听到继电器 *咔嗒* 声 = 信号链 OK。
#. **接推杆但低电流**：把 DP100 输出设到 5.0V，限流先调到 1A，开炮时若推杆**不动**且 DP100 进入 CC 限流状态（电流值定在 1.0A 不动） → 把限流加到 4A 再试。
#. **正常开炮**：限流 ≥ 4A、5.0V，应能听见推杆每 3 秒一次清脆 *咚* 声。
#. **按 BOOT 手动开炮**：再加一发，串口能看到 ``FIRE #N (button, ...)``。
#. **观察 5V 总线**：如果 ESP32 在开炮时频繁复位（串口看到 ``boot:`` 重启日志） → 说明浪涌已经拖塌 5V，需要后续加大电容缓冲板 / 改 D4184。

故障排查
========

继电器响但极性相反（推杆"伸出 2920ms 缩回 80ms"，刚好反过来）
   * 你这块 4 路继电器是高电平触发的。把 overlay 里 ``GPIO_ACTIVE_HIGH`` 改回 ``GPIO_ACTIVE_LOW``，或反之。
   * 本仓库默认已经是 ``GPIO_ACTIVE_HIGH``（按本人手上这批模块实测）。
继电器一直不响
   * 共地了吗？ESP32 GND 必须和继电器 GND、DP100 − 短接。
   * 试着把 ``GPIO_ACTIVE_HIGH`` 与 ``GPIO_ACTIVE_LOW`` 互换（不同批次模块触发极性不同）。
继电器响但推杆不动
   * 电源限流不够（KK-1040B 实测 3A 起步），把 DP100 限流加到 ≥ 4A。
   * DP100 输出实测掉到 4V 以下 → DP100 进入 CC 模式，电流被钳住，加大限流即可。
ESP32 一开炮就重启
   * 你的 ESP32 5V 和推杆 5V 是不是共了一路？应**分开**：ESP32 走 USB 供电；推杆走 DP100。
   * 共一个电源时必须加大电容缓冲板。

完成本测试后，请把以下信息记录下来，作为后续整机选型依据：
``实际驱动电压 / 实际限流值 / 是否复位 / 推杆是否每次都能推到底``。
