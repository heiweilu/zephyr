CyberPiercer 炮台控制
######################

ESP32-S3-BOX(-3) + 2×DS3225 舵机 + KK-1040B 电磁推杆的炮台控制用例。
分 5 个阶段测试，**每个阶段只新增一个组件的接线**，接一个测一个。

硬件清单
========

==================== ===========================================================
项                   规格 / 备注
==================== ===========================================================
ESP32-S3-BOX(-3)     主控板（USB Type-C 串口 + 板载 BOOT 键）
DS3225 舵机 ×2       6V / 25kg·cm 数字舵机，PWM 500–2500μs，50Hz
KK-1040B 电磁推杆    5V，标称 1.8A，启动峰值 ~3A
4 路继电器模块       5V 供电、光耦隔离、本批次**高电平触发**
DP100 可调电源       6.0V / 限流 5A（舵机阶段），或 5.0V（推杆阶段）
DC-DC 可调降压模块   Phase 4+ 用于将 6V 降至 5V 给继电器 + 推杆
导线 + 端子          AWG18 以上
==================== ===========================================================

GPIO 分配
=========

======= ============ =========== ================================
GPIO    功能         LEDC 通道   备注
======= ============ =========== ================================
GPIO9   水平舵机 PWM CH0         LEDC_CH0_GPIO9
GPIO10  垂直舵机 PWM CH1         LEDC_CH1_GPIO10
GPIO38  继电器 IN1   N/A         &gpio1 6, ACTIVE_HIGH
GPIO0   BOOT 按钮    N/A         板载，手动开炮
======= ============ =========== ================================

DS3225 舵机线序
===============

三线制，JR-3pin-2.54mm 插头：

======= ======= =========
颜色    功能    接法
======= ======= =========
棕/黑   GND(−)  DP100 −
红      VCC(+)  DP100 +
橙/白   Signal  ESP32 GPIO
======= ======= =========

⚠ 安全注意
==========

1. **ESP32 必须用 USB 自供电**，不要从 DP100 给 ESP32 供电。
2. **先不接舵机/推杆，空载测信号**：万用表量 GPIO 有没有 PWM 方波。
3. **接舵机时先接地线 (GND)**，再接 VCC，最后接信号线 — 防止浮空损坏舵机。
4. **每个阶段测试完再接下一个组件**，不要一次全部接完。
5. DP100 开机前先确认电压/限流设置正确。
6. 舵机和推杆**不要从 ESP32 排针取电**，电流远超 ESP32 输出能力。

分阶段接线与测试
================

Phase 1: 水平舵机独立测试
--------------------------

**目的**: 验证 PWM 输出 + 水平舵机运动正常

**代码开关**: ``src/main.c`` 中::

    #define ENABLE_SERVO_H    1
    #define ENABLE_SERVO_V    0
    #define ENABLE_SOLENOID   0

**接线**::

    DP100 设置: 6.0V，限流 3A

    DP100 +  ──── 水平舵机 VCC (红线)
    DP100 −  ──── 水平舵机 GND (棕/黑线)
    ESP32 GPIO9 ── 水平舵机 Signal (橙/白线)
    ESP32 GND ──── DP100 − (公共地！必须！)

    ESP32 通过 USB 连接 PC（自供电 + 串口调试）

**测试步骤**:

#. 编译烧录（见下方编译章节）。
#. 先**不接舵机**，用万用表测 GPIO9 有无 ~50Hz 方波。
#. 确认 DP100 输出 6.0V，限流 3A。
#. 依次接线: GND → VCC → Signal。
#. 接好后上电，舵机应先回到中点 (正前方)，然后左右扫描 ±45°。
#. 串口输出::

    [inf] cyber_piercer: === CyberPiercer turret control ===
    [inf] cyber_piercer: Phase config: SERVO_H=1  SERVO_V=0  SOLENOID=0
    [inf] cyber_piercer: servo_h ready: GPIO9, center=135°
    [inf] cyber_piercer: init complete. BOOT=manual fire. Entering scan loop.
    [inf] cyber_piercer: H=142° (pulse=1550us)
    [inf] cyber_piercer: H=149° (pulse=1600us)
    ...

#. **观察**: 舵机是否平稳运动，有无抖动/异响。
#. **调零**: 如果默认位置不是正前方，记录实际偏差角度，后续调整 ``SERVO_CENTER``。

Phase 2: 垂直舵机独立测试
--------------------------

**断开水平舵机**（拔掉 Signal 线即可，或同时断电）。

**代码开关**::

    #define ENABLE_SERVO_H    0
    #define ENABLE_SERVO_V    1
    #define ENABLE_SOLENOID   0

**接线**: 同 Phase 1，但换成 GPIO10 和垂直舵机::

    ESP32 GPIO10 ── 垂直舵机 Signal (橙/白线)
    DP100 + / − ── 垂直舵机 VCC / GND

**测试步骤**: 同 Phase 1，但观察的是垂直方向上下扫描运动。
垂直舵机会自动在 ±45° 范围内上下扫描。

   注意: 垂直舵机承受重力载荷。如果 6V 下出现抖动/嗡鸣，
   属于供电不足或机械卡涩，检查 DP100 限流是否触发。

Phase 3: 双舵机联合测试
------------------------

**代码开关**::

    #define ENABLE_SERVO_H    1
    #define ENABLE_SERVO_V    1
    #define ENABLE_SOLENOID   0

**接线**::

    DP100 设置: 6.0V，限流 5A（两个舵机同时堵转峰值 ~4A+）

    DP100 +  ──┬── 水平舵机 VCC (红)
               └── 垂直舵机 VCC (红)
    DP100 −  ──┬── 水平舵机 GND (棕)
               └── 垂直舵机 GND (棕)
    ESP32 GPIO9  ── 水平舵机 Signal
    ESP32 GPIO10 ── 垂直舵机 Signal
    ESP32 GND ──── DP100 −

**测试步骤**:

#. 重新编译烧录。
#. 上电后两个舵机应同时回到中点。
#. 水平舵机左右扫描，垂直舵机保持中点。
#. 观察 DP100 电流读数：空转时 <100mA，运动时 0.5-1.5A 正常。

Phase 4: 电磁推杆测试
----------------------

在 Phase 3 基础上**新增**继电器和推杆接线。

**代码开关**::

    #define ENABLE_SERVO_H    1
    #define ENABLE_SERVO_V    1
    #define ENABLE_SOLENOID   1

**接线**::

    继续保持 Phase 3 的所有接线（舵机 6V 由 DP100 直供）。
    新增: DC-DC 降压模块，输入接 DP100 6V，输出调至 5.0V。

    DC-DC 输出 5V + ──── 继电器 VCC
    DC-DC 输出 5V + ──── 继电器 CH1 COM
    DC-DC 输出 GND ──── 继电器 GND
    ESP32 GPIO38 ──────── 继电器 IN1
    继电器 CH1 NO ─────── 推杆红线 (+)
    DC-DC 输出 GND ──── 推杆黑线 (−)

    ESP32 GND ──── DC-DC GND ──── DP100 − (全部共地)

**测试步骤**:

#. **先不接推杆**，重新编译烧录，确认继电器每次按 BOOT 能听到 *咔嗒* 声。
#. 接上推杆，按 BOOT 手动开炮。
#. 串口应显示::

    [inf] cyber_piercer: FIRE #1 (button, pulse=80ms)

Phase 5: 全系统整合
--------------------

Phase 4 的接线即为最终接线。验证：

#. 上电后舵机回中，炮台对准正前方。
#. 舵机自动扫描中，按 BOOT 开炮。
#. 观察开炮时舵机是否抖动（电源浪涌问题）。
#. 如果 ESP32 在开炮时复位 → 推杆电流浪涌拉塌 DC-DC 输出 → 需在 DC-DC 输出加大电容。

编译 & 烧录
===========

.. code-block:: console

    cd <ZEPHYR_BASE>

    west build -b esp32s3_devkitc/esp32s3/procpu \
        samples/boards/espressif/esp32-s3-box/cyber_piercer \
        -d build/cyber_piercer

    west flash --build-dir build/cyber_piercer

串口调试
========

.. code-block:: console

    west espressif monitor --port <YOUR_COM_PORT>

按 ``Ctrl+]`` 退出监视。

参数调整
========

机械零点校准
   水平中点: 修改 ``SERVO_CENTER``（默认 1500μs）。
   垂直中点: 修改 ``SERVO_V_CENTER``（默认 1463μs，补偿上偏 5°）。
   DS3225 每 ±7.4μs ≈ ±1°，根据实际偏差微调。

扫描范围
   修改 ``SERVO_H_MIN`` / ``SERVO_H_MAX`` 和 ``SERVO_STEP``。
   默认 1000–2000μs ≈ ±45°。

开炮脉宽
   修改 ``PULSE_MS``。推杆打不到底就加到 100-150ms，发烫就减到 60ms。

故障排查
========

舵机不动但 GPIO 有 PWM 信号
   * 检查 VCC/GND 接线是否牢固。
   * DP100 限流是否太低（< 1A）。
   * 舵机线序是否接反（棕=GND，红=VCC，橙=Signal）。

舵机抖动 / 嗡鸣
   * 电源电压不够 → DP100 调到 6V，限流 ≥ 3A。
   * 机械卡涩 → 手动转动舵机确认顺滑。
   * PWM 频率错误 → 串口确认 period=20ms。

开炮时舵机突然跳位
   * 推杆浪涌 (3A) 拉塌电源。
   * 解决: DC-DC 输出加 470μF/16V 电解电容，或舵机与推杆分路供电。

ESP32 在开炮时复位
   * 共地不良 → 检查所有 GND 星形连接。
   * USB 供电不足 → 换更好的 USB 线或 USB 电源适配器。

完成本测试后的记录
==================

请记录以下信息作为后续整机选型依据::

    水平舵机: 6V 扫描是否正常 / 有无抖动 / 中点偏差角度
    垂直舵机: 6V 承载是否正常 / 有无嗡鸣
    电磁推杆: 80ms 脉宽是否打到底 / DP100 瞬时电流峰值
    系统: 开炮时舵机是否受影响 / ESP32 是否复位
