# 离线智能音频终端

一款基于 STM32F407 + ESP32-C3 的桌面智能终端原型，集成 **音乐播放、天气预报、AI 文字问答、NTP 授时、系统监控** 功能，完全脱离手机/PC 独立运行。

## 功能

- **音乐播放**：SD 卡存储 WAV 曲库，支持播放/暂停、上下曲切换、进度拖动、音量调节、循环/随机播放模式
- **天气查询**：拼音输入城市名，获取实时天气与三日预报
- **AI 问答**：拼音键盘输入文字，通过 DeepSeek 大模型回复
- **NTP 授时**：开机自动同步网络时间
- **WiFi 管理**：扫描热点、输入密码连接
- **系统监控**：实时显示 CPU 占用、RAM/Flash 余量
- **亮度调节**

## 硬件

| 器件 | 型号 | 说明 |
|---|---|---|
| 主控 | STM32F407VET6 | 168MHz, 512KB Flash, 128KB SRAM |
| WiFi | ESP32-C3 SuperMini | 通过软件串口与主控通信 |
| 音频 | WM8978 | I2S 输入 + 内置 DAC |
| 字库 | W25Q16 | 2MB SPI Flash，存储 GBK 字库 |
| 屏幕 | ILI9341 | 320×240 LCD，FSMC 16-bit 并口 |
| 触摸 | XPT2046 | 四线电阻触摸 |

## 软件架构

```
STM32F407（主控）
├── FreeRTOS 多任务调度
│   ├── LVGL 任务………GUI 渲染、触摸交互（优先级 2）
│   ├── 音频任务………WAV 解码、双缓冲 DMA 播放（优先级 4）
│   ├── 串口任务………接收 ESP32 数据、协议解析（优先级 1）
│   └── 字库任务………字库缓存预热（优先级 1）
├── FatFs → SDIO(DMA) → SD 卡
├── I2S(DMA) → WM8978
├── FSMC → ILI9341
├── SPI → XPT2046 / W25Q16
├── Soft UART → ESP32-C3
└── 自定义文本协议（基于 `\n` 分帧）

ESP32-C3（通信协处理）
├── WiFi / HTTP / NTP
├── 自定义帧协议解析
└── UART → STM32F407
```

## 编译与烧录

### STM32F407

1. 打开 `STM32F407源码/USER/Template.uvprojx`（Keil MDK 5.24+）
2. 编译器选择 ARMCC V5.06
3. 编译后通过 ST-Link 烧录

### ESP32-C3

1. 打开 `ESP32-AI源码/`（VS Code + PlatformIO 或 Arduino IDE）
2. 修改 `main/config.h` 中的 API Key：
   - `AI_KEY` → [DeepSeek API Key](https://platform.deepseek.com/)
   - `SEN_KEY` → [心知天气 API Key](https://www.seniverse.com/)
3. 编译烧录

## 引脚连接

### F407 ↔ ESP32-C3

| F407 | ESP32-C3 | 说明 |
|---|---|---|
| PA11 (TX) | GPIO1 (RX) | 软件串口 |
| PA12 (RX) | GPIO0 (TX) | 软件串口 |
| 3.3V / GND | 3.3V / GND | 供电 |

完整引脚定义见各源码目录下的硬件头文件。

## SD 卡字库使用

项目支持从 SD 卡加载自定义字库（.bin 格式），替代内置 C 数组字库以节省 Flash 空间。


### 字库文件位置

将 `.bin` 字库文件放入 SD 卡 `FONT/` 目录，例如：

```
SD:/
├── FONT/
│   └── Alimama_16.bin    ← 16px 宋体/阿里妈妈字库
│   └── MyFont_24.bin     ← 24px 自定义字库
└── MUSIC/
    └── *.wav
```

### 字库格式要求

字库为 LVGL 兼容的二进制字体，头部 4 字节魔数固定为 `FTB1`（`0x46 0x54 0x42 0x31`）。可用 LVGL 官方字体转换工具 [Online Font Converter](https://lvgl.io/tools/fontconverter) 生成，参数需满足：

| 参数 | 要求 |
|---|---|
| 字体名称（Name） | 自由命名 |
| 大小（Size） | 与 .bin 内 glyph 高度一致 |
| 位深（Bpp） | 1 或 2（推荐） |
| 字符范围（Symbols） | 包含全部界面用到的汉字与符号；推荐以 `font_charset_union.txt` 为输入 |
| 格式（Format） | **Bin**（二进制） |

### 加载与回退

- 上电初始化时，固件先尝试从 SD 卡 `FONT/` 目录加载首位 .bin 字库
- 若 SD 卡未插入或无 `FONT/` 目录，自动回退到 W25Q16 内置字库
- 若 W25Q16 内无字库，再回退到 LVGL 默认西文字体

### 字体工具参考

- 字符集输入文件：[项目所需字体表.txt](./项目所需字体表.txt)
- 在线转换：[https://lvgl.io/tools/fontconverter](https://lvgl.io/tools/fontconverter)

## 目录结构

```
├── STM32F407源码/       主控端 Keil 工程
│   ├── APP/             业务模块（音频、AI、天气、WiFi、键盘等）
│   ├── HARDWARE/        外设驱动（SDIO、I2S、WM8978、LCD、触摸、W25Q16）
│   ├── FATFS/           FatFs 文件系统
│   ├── LVGL/            LVGL 8.3 图形库
│   ├── FREEPTOS/        FreeRTOS 内核
│   └── UI/              SquareLine Studio 设计稿
├── ESP32-AI源码/        通信端代码
│   └── main/
│       ├── config.h     API Key / 引脚配置
│       ├── link.cpp     协议解析与路由
│       ├── net.cpp      WiFi / HTTP / NTP / 天气
│       └── ime.cpp      拼音候选词（后缀搜索）
├── UI素材/              LVGL 官方源码（参考）
└── README.md
```

## 开源协议

本项目代码以 MIT 协议开源。依赖的第三方库（FreeRTOS、LVGL、FatFs 等）遵循各自原有协议。

## 致谢

- [LVGL](https://lvgl.io/)
- [FreeRTOS](https://www.freertos.org/)
- [FatFs](http://elm-chan.org/fsw/ff/)
- [DeepSeek](https://www.deepseek.com/)
- [心知天气](https://www.seniverse.com/)