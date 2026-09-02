# 无名科技星智 S3 4G（无屏）

无名科技星智 S3 4G 板卡，基于 ESP32-S3 + ES8311 + ML307，无显示屏。

## 硬件要点

- 芯片：ESP32-S3
- 网络：ML307 Cat.1 4G（TX `GPIO12` / RX `GPIO11`）
- 音频：ES8311（I2S + I2C）
- 按键：BOOT `GPIO8`
- 电源：USB/电池 ADC 检测，电源键关机控制

## 编译

```bash
python3 scripts/build.py nologo/xingzhi-s3-4g --name xingzhi-s3-4g
```
