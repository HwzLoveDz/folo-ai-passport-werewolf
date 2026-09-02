# AI-PASSPORT Werewolf 硬件边界与验证指南

本文只记录狼人杀产品所依赖的 AI-PASSPORT 硬件事实和验证方法。官方固件是
硬件层参考，不是产品软件基线；官方菜单、Radar、演示页面、视觉资产、导航和业务
状态均不得回到本项目。设备开机只进入 `werewolf_app`。

## 1. 证据边界

允许从官方或板级参考实现中核对：

- 原理图、芯片数据手册和 `bsp_pins.h` 对应的引脚；
- ST7789P3 初始化序列、SPI mode、反色和旋转；
- ADC 三键分压范围及按下、长按、松手的物理边沿；
- ES8311 与 CW2017 的共享 I2C 总线；
- I2S、背光 PWM、电源和电池计的硬件行为。

禁止继承：

- 官方开机菜单、Demo/Radar 页面和页面切换方式；
- 官方字体、配色、吉祥物、云/草地、卡片或其他界面资产；
- 官方软件任务、业务状态、蓝牙 Radar 协议和演示日志；
- 为方便调硬件而把演示入口重新接回产品固件。

临时硬件诊断必须在隔离构建或独立分支中完成。诊断代码不能进入生产入口，验证后
将结论沉淀到 BSP 注释、测试或本指南。

当前仓库尚未登记板卡硬件版本和所参考上游实现的精确 commit。下表因此只能作为
当前代码快照，不能替代发布前的原理图、实板和来源授权记录。

## 2. 当前板级事实

引脚和板级外设参数以 `components/bsp/include/bsp_pins.h` 为代码事实源；MCU、
控制台、LVGL 与内存配置以 `sdkconfig.defaults` 为准；分区布局以 `partitions.csv`
为准。修改前必须同时核对原理图、实板和对应芯片资料。

| 子系统 | 当前配置 | 关键约束 |
|---|---|---|
| MCU | ESP32-C3 | 无 PSRAM，显示与网络均占内部 RAM |
| 屏幕 | ST7789P3，240 × 320 | SPI2，40 MHz，mode 0，当前屏需 INVON |
| LCD SPI | MOSI 9，SCLK 8，CS 1，DC 20 | RST 未接 MCU，使用软件复位 |
| 背光 | GPIO21 / LEDC | USB 控制台必须走 USB-Serial-JTAG，不能占 UART0 TX21 |
| 三键 | GPIO0 / ADC1_CH0 | 外部 10 kΩ 上拉，三键共用一条 ADC 分压链 |
| I2C0 | SDA10，SCL7 | ES8311 与 CW2017 共用；不得创建第二个 I2C0 master |
| 音频 | I2S0，MCLK6/BCLK5/WS3/DOUT2/DIN4 | 功放使能当前未接 MCU |
| 电量计 | CW2017，地址 0x63 | 读取失败必须显式报告，不能伪造电量 UI |

表格是当前代码快照，不代替实板证据。

## 3. BSP 与产品层分工

`components/bsp` 只提供硬件能力：

- `bsp_display.*`：面板、背光和 LVGL port；
- `bsp_button.*`：ADC 三键及 PRESS/RELEASE/CLICK/LONG 事件；
- `bsp_i2c.*`：共享 I2C0 总线所有权；
- `bsp_audio.*`：ES8311/I2S；
- `bsp_battery.*`：CW2017 数据。

`main/` 只实现狼人杀产品。`main/main.c` 保持薄入口，游戏控制器拥有业务状态；网络
回调只投递有界值事件；LVGL 只在锁内更新。若某项硬件尚未用于产品，不要仅为保留
演示能力而在启动阶段初始化它。

## 4. UI 与启动约束

- 当前产品视觉基线是 **Eclipse Ledger**：近黑空间、月蚀圆环与单元格、暖纸色文字、
  克制的琥珀/青色/红色状态色、Kode Mono 和整数像素布局。Gesture Wand 只保留为
  早期排版与模拟器工程参考，不再作为现行界面基线，也不得复制其业务或页面。
- 模拟器必须直接编译生产 `werewolf_ui.c` 和固件字体对象。
- 上板前先运行 `simulator/preview.sh`，审查从 240 × 320 RGB565 framebuffer 转出的
  PNG 候选图和隐私态逐字节比较。
- 面板在产品首帧同步刷新完成前保持黑屏，再开启背光，避免白屏或旧 GRAM 残影。
- 私密内容必须绑定本次 `OK PRESS` 的页面和 private gate epoch。松手立即封闭并
  清空私密 label，但不提交确认；同一 gate 内可重复查看，另一次独立短按 `OK` 才
  完成。换页、epoch 变化、断线或输入关闭必须 fail closed；普通同 gate heartbeat
  或 UI revision 更新不得误取消有效按住或复看资格。
- 不显示没有真实数据源的电量、RSSI、链路图或“AI 状态”。

## 5. 构建与证据分级

使用 ESP-IDF 5.5.3，保留现有 `build/`、managed components 和本地配置；除非已有
证据表明缓存损坏，不执行 `fullclean`。

```bash
./tests/run_host_tests.sh
WEREWOLF_UI_OUTPUT_DIR=/tmp/werewolf-ui-candidate \
    bash simulator/preview.sh
source /path/to/esp-idf-v5.5.3/export.sh  # 替换为本机安装路径
idf.py build
```

交付时分开报告：

1. 主机规则/协议测试；
2. UI 模拟器和像素隐私断言；
3. ESP-IDF 源码构建、镜像大小和 RAM 余量；
4. 实际烧录；
5. 单机或多机物理行为。

前 3 项不等于已烧录或已上板。每次写设备前必须重新枚举串口、核对完整分区表和
安全状态，并取得明确写入许可。普通烧录许可不包含擦除 NVS、eFuse、密钥或其他
安全状态变更。

## 6. 硬件变更验证清单

### 屏幕或 UI

- 冷启动无白闪、无官方页面、无旧 GRAM 残影；
- 方向、裁切、RGB/BGR、反色和背光正确；
- 三键全状态可达，长按和松手边沿无遗漏；
- LVGL 内存、内部堆和任务栈有记录；
- 模拟器截图与实物构图一致。

### ADC 三键

- 在隔离诊断构建中调用 `bsp_button_read_mv()`；
- 记录松开及每个键的多次实测电压；
- 以相邻实测分布的安全中点更新窗口，检查温漂和 USB 供电差异；
- 验证 `OK RELEASE` 能立即封闭并清空私密内容，但不离开页面、不提交确认；随后仍可
  重复查看并以独立短按 `OK` 完成。

### I2C、音频或电池

- 复用 `bsp_i2c` 的同一个 I2C0 master；
- 核对地址、上拉、电压域、速率和错误恢复；
- 音频 DMA 与 LVGL/ESP-NOW 并发时检查内部 RAM；
- 电池数据无效时显示未知/不显示，不生成假值。

### 无线联机

- 检查 Wi-Fi channel、加密 peer 数和内部 RAM；
- 广播只用于发现及 LMK 前承诺/揭示，秘密只走加密单播；
- 串口和抓包中不得出现角色、目标、投票、查验结果或密钥；
- 单机只能证明启动/交互，多机结论必须等真实设备数量满足后再记录。

## 7. 完成标准

硬件层参考被吸收到可解释的 BSP 后，产品编译单元、ELF 字符串、开机画面和交互流中
都不应再出现官方 Demo/Radar 软件。任何未来协作者若需要参考官方固件，应先写明要
验证的硬件事实及证据来源，不得从旧界面反推本产品设计。
