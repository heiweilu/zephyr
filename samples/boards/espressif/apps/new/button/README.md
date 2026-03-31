# Button (Module 2)

GPIO0 按键检测，中断回调 + 30ms 消抖（k_work_delayable），双边沿触发。

## 硬件

- GPIO0（BOOT 按钮），板卡 DTS 已定义，低电平有效，内部上拉

## 编译

```powershell
west build -b esp32s3_devkitc/esp32s3/procpu samples/boards/espressif/apps/new/button -p
```

## 烧录

```powershell
west flash --esp-device COM15
```

## 串口监控

```powershell
west espressif monitor --port COM15
```

## 预期输出

```
*** Booting Zephyr OS build v4.2.0-3653-g97780a51fce6 ***
Button configured at gpio@60004000 pin 0 (debounce 30ms)
Press the button
Button PRESSED (pin 0)
Button RELEASED (pin 0)
```
