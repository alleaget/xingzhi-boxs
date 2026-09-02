# 无名科技星智 S3 Wi-Fi（无屏）

无名科技星智 S3 Wi-Fi 板卡，基于 ESP32-S3 + ES8311，无显示屏。

## 硬件要点

- 芯片：ESP32-S3
- 音频：ES8311（I2S + I2C）
- 按键：BOOT `GPIO8`
- 电源：USB/电池 ADC 检测，电源键关机控制

## 编译

推荐：

```bash
python3 scripts/build.py nologo/xingzhi-s3-wifi --name xingzhi-s3-wifi
```

或手动：

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

选择：

`Xiaozhi Assistant` → `Target Board` → `Nologo Xingzhi S3 Wi-Fi (无名科技星智)`

然后：

```bash
idf.py build flash monitor
```
