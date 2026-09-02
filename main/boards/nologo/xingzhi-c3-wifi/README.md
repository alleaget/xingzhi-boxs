# 无名科技星智 C3 Wi-Fi（无屏）

无名科技星智 C3 Wi-Fi 板卡，基于 ESP32-C3 + ES8311，无显示屏。

## 硬件要点

- 芯片：ESP32-C3
- 音频：ES8311（I2S + I2C）
- 按键：BOOT `GPIO9`
- 电源：USB/电池 ADC 检测，电源键关机控制

## 编译

```bash
python3 scripts/build.py nologo/xingzhi-c3-wifi --name xingzhi-c3-wifi
```
