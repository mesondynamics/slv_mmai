# ECU KiCad / STM32CubeMX 配置审查

审查日期：2026-08-26

MCU：STM32H563ZIT6，LQFP144

工具：STM32CubeMX 6.18.0、STM32CubeH5 1.7.0

硬件依据：`pcb/roller/ecu` KiCad 工程

兼容依据：上一版 `legacy ECU firmware` 与
`docs/SCH_2025-10-13.pdf`

## 1. 审查结论

`ECU.ioc` 已从新建工程补齐为可开发基线。引脚、外设归属、关键初始电平、
CAN 位时序、ETH MAC、DMA、定时器和 TrustZone 属性均已写回 `.ioc`，并非只
修改生成 C 文件。应用逻辑放在 `Secure/App`、`NonSecure/App`、
`Secure_nsclib` 和用户维护的 CMake 文件内；CubeMX 再生成不会删除这些文件。

当前 PCB 的 PB8/PB9 物理网络接反，已按用户决定改为 Secure GPIO 开漏模拟
I²C：`PB8=SW_I2C_SDA`、`PB9=SW_I2C_SCL`，无需飞线。ATECC608C 协议功能
明确延期；目前只做总线释放、时钟拉伸超时和 9 个 SCL 脉冲恢复，故障会上报
但不会阻止车辆基本功能。

LwIP 在 STM32H5 当前 CubeMX 工程中作为项目自有中间件集成。CubeMX 继续生成
ETH HAL、描述符和引脚初始化；LwIP、LAN8742 接口和 ECU UDP 应用放在生成目录
之外。这样既保留 `.ioc` 的硬件配置，又避免 UI 再生成覆盖协议代码。

## 2. 已持久化的 CubeMX 配置

| 子系统 | 当前设置 | 设计约束 |
|---|---|---|
| 时钟 | HSE 25 MHz，SYSCLK/HCLK 250 MHz，CSS 开启 | HSE 故障进入 NMI 并关断输出 |
| TrustZone | ADC12、SPI4、TIM4、TIM6、IWDG 为 Secure；对应 GPIO、EXTI、DMA 受保护 | NonSecure 不能直接写继电器、阀 PWM 或安全采样 |
| 急停 | PB2，Secure 双边沿 EXTI，标签 `ESTOP_DETECT` | 高电平立即锁存故障；恢复后仍需中性命令清故障 |
| 软件 I²C | PB8=软件 SDA、PB9=软件 SCL，GPIO Output、Open Drain、上电 High | 仅适用于当前接反的 PCB；内部不上拉，依赖板上外部上拉 |
| TPIC6A595 | SPI4 Secure；PE2 SCK、PE6 MOSI；8 bit、TX only、3.90625 Mbit/s | PE3 OE_N 上电 High；PE5 CLR_N、PE7 BUF_EN、PE4 RCK 上电 Low |
| 阀 PWM | TIM4 CH1=PB6、CH2=PB7；中心对齐 20 kHz；初值 0 | 正反向互锁；CH4=1 在中心对齐计数谷底、即 PWM 导通脉冲中心触发 ADC2 |
| 慢速 ADC | ADC1 九路，TIM6 TRGO 1 kHz，GPDMA1 CH0 circular | 外部量、VREFINT、MCU 温度；采样顺序见下表 |
| 电流 ADC | ADC2 两路，12.5 cycles，TIM4 TRGO，GPDMA1 CH1 circular；ADC2/GPDMA IRQ priority 1 | 前进 PA4、后退 PA6；20 kHz 电流 PI、上电零点校准、VREFINT 补偿和硬故障阈值 |
| FDCAN1 | Classic CAN 250 kbit/s，80% 采样点，自动重发；0 standard/6 extended filters | 车辆 J1939 接口；只接受六个 PGN |
| FDCAN2 | Classic CAN 250 kbit/s，0 filters | 预留，应用不启动控制器 |
| 速度输入 | TIM2 CH1=PA0；1 MHz、32 bit、输入滤波 8、IRQ | 20000 pulse/km，1500 ms 丢信号，IIR α=0.2 |
| Ethernet | RMII、MAC `8A:EA:B5:00:00:02`；PB14 PHY reset 上电 Low | NonSecure 应用延迟 10 ms 释放 LAN8742 |
| IWDG | Secure，LSI、Prescaler 32、Reload 1999，约 2 s | NonSecure 只能通过带递增 heartbeat 的 NSC 服务刷新 |

ADC1 DMA 固定顺序：

| 索引 | 信号 | MCU 通道 |
|---:|---|---|
| 0 | OIL_LEVEL_ADC | PC0 / ADC1_IN10 |
| 1 | OIL_TEMP_ADC | PC2 / ADC1_IN12 |
| 2 | WATER_LEVEL_ADC | PC3 / ADC1_IN13 |
| 3 | TEMP_SENSOR_1_ADC | PA3 / ADC1_IN15 |
| 4 | TEMP_SENSOR_2_ADC | PA5 / ADC1_IN19 |
| 5 | ENGINE_SENSOR_ADC | PB0 / ADC1_IN9 |
| 6 | VBAT_12V_ADC | PB1 / ADC1_IN5 |
| 7 | VREFINT | internal |
| 8 | MCU temperature | internal |

ADC2 索引 0 为前进阀电流 PA4，索引 1 为后退阀电流 PA6。车辆状态沿用
`mV = raw × 3360 / 4096`，发动机信号达到 3200 mV 判定运行。尚未取得传感器
标定曲线的通道只上报原始值/毫伏值，不编造工程量或报警阈值。

## 3. 必须在 CubeMX UI 中这样核对

正常情况下不需要再手工修改。使用 CubeMX 6.18 打开项目根目录 `ECU.ioc`；
不要从 NUCLEO 模板重建工程。若 UI 显示黄色冲突或要求迁移固件包，先取消生成
并确认仍使用 STM32CubeH5 1.7.0。

### 3.1 Secure context 和 PB8/PB9

1. 在 Pinout 页面切换到 **Cortex-M33 Secure** context。
2. PB8 选择 `GPIO_Output`，User Label 为 `SW_I2C_SDA`；PB9 选择
   `GPIO_Output`，User Label 为 `SW_I2C_SCL`。
3. 在两脚 GPIO 参数中确认 Output Level=`High`、Mode=`Output Open Drain`、
   Pull=`No pull-up and no pull-down`、Speed=`Low`、Pin Attribute=`Secure`。
4. **不要启用 I2C1，也不要把 PB8/PB9 改为硬件 AF。** 当前 PCB 只有上述
   反常映射才能不飞线工作。
5. `System Core > GTZC_S` 中不需要配置 I2C1；应确认 SPI4、ADC12、TIM4、
   TIM6、IWDG 为 Secure，并启用相应 Illegal Access interrupt。
6. CubeMX 6.18 当前会把 SRAM3 的 privilege vectors 全部生成为 privileged-only，
   且 `.ioc` 不保存手工加入的 MPCBB3 privilege 数组。ETH DMA 以非特权总线主设备
   访问 NonSecure 描述符，因此 `Secure/Core/Src/gtzc_s.c` 的
   `GTZC_S_Init 2` USER CODE 会在生成配置之后，仅将 SRAM3 前 64 KiB
   (`0x20050000..0x2005FFFF`) 改为允许非特权访问。该段由
   `ProjectManager.KeepUserCode=true` 保留；不要删除或移出 USER CODE 标记。
   其余 SRAM3 继续保持 privileged-only。

### 3.2 TPIC 和安全初始电平

1. `Connectivity > SPI4`：Master、Simplex Transmit Only、8 Bits、Prescaler
   32，计算速率应为 3.90625 Mbit/s。
2. PE2=`SPI4_SCK/TPIC_SRCK`，PE6=`SPI4_MOSI/TPIC_SER`，两脚均为 Secure。
3. PE3 `TPIC_OE_N` 初值 High；PE5 `TPIC_CLR_N`、PE7
   `TPIC_CTRL_BUF_EN`、PE4 `TPIC_RCK` 初值 Low。任何一个初值变化都可能造成
   上电瞬态吸合，不能接受。

### 3.3 ADC、DMA 和 TIM4/TIM6

1. ADC1 regular group 为 9 ranks，触发源 `TIM6 TRGO / Rising edge`，DMA
   circular；ADC2 为 2 ranks，触发源 `TIM4 TRGO / Rising edge`。
2. GPDMA1 CH0/CH1 分别为 ADC1/ADC2，source fixed、destination increment、
   half-word、circular，并标记 Secure channel/source/destination；Security 页的
   Channel Privilege 必须为 **Privileged**。ADC DMA 的 linked-list node 与目标缓冲区
   位于 privileged-only Secure SRAM，设成 Non-Privileged 会触发 GPDMA `USE` 错误并
   使 ADC 快照保持为零。
3. TIM4 Counter Mode=`Center Aligned mode 1`，Prescaler=0，Period=6249；
   CH1/CH2 PWM Pulse=0。ADC2 两个 regular rank 均为 12.5 cycles；ADC2 和
   GPDMA1 Channel1 IRQ 抢占优先级均为 1。
4. 保留虚拟 `PWM Generation4 No Output`，Pulse=1，Master TRGO=`OC4REF`。
   中心对齐 PWM1 在计数谷底跨越导通区，该触发点位于导通脉冲中心附近；
   删除 CH4 或恢复 3125 都会破坏 20 kHz 电流采样相位。
5. TIM6 Update Event 频率为 1 kHz，并启用 Secure global interrupt。

### 3.4 Ethernet 与 FDCAN

1. 切换到 **Cortex-M33 NonSecure** context。
2. ETH Mode=`RMII`，MAC=`8A:EA:B5:00:00:02`；核对 RMII 引脚与 KiCad
   网络一致，PB14 `RMII_NRST` 初值 Low。
3. FDCAN1/FDCAN2 均为 Classic CAN 250000 bit/s：kernel 25 MHz、Prescaler
   4、Seg1=19、Seg2=5、SJW=4、Auto Retransmission 与 Transmit Pause enabled。
4. FDCAN1 Standard Filters=0、Extended Filters=6；FDCAN2 两类均为 0。
   PGN/Mask 在应用启动时配置，CubeMX UI 只保存 RAM 元素数量。
5. FDCAN1 IT0 优先级 5；TIM2 捕获优先级 6。CAN2 不启用运行时通知。

### 3.5 LwIP 的特殊说明

不要在 CubeMX 中另加一份 LwIP，也不要复制 `MX_LWIP_Init()` 代码。本工程使用：

- `ThirdParty/LwIP`：固定版本的协议栈源码（避免 CubeMX 清理保留名 `Middlewares`）；
- `Drivers/BSP/Components/lan8742`：PHY 驱动；
- `NonSecure/App/Network`：本板 `ethernetif` 与 `lwipopts.h`；
- `NonSecure/App/Src/ecu_network.c`：静态 IP 和 UDP 协议。

`NonSecure/CMakeLists.txt` 顶部明确标注“generated only once”，CubeMX 后续不会
重写现有文件。生成的 `NonSecure/Core/Src/eth.c` 仍是 ETH HAL 唯一硬件实例，
应用层不会重复定义 `heth`、描述符或 MSP。

### 3.6 Project Manager

保持 Toolchain=`CMake`、Keep User Code=`enabled`、删除旧生成文件=`enabled`。
每次 UI 生成后必须执行：

```sh
./tools/audit_config.sh
./tools/test.sh
./tools/build.sh Debug
./tools/build.sh Release
```

`tools/build.sh` 对 Secure/NonSecure 和 Debug/Release 使用独立构建目录，避免
CubeMX 顶层 ExternalProject 在切换配置时复用错误对象。

## 4. 再生成安全边界

| 所有者 | 目录/文件 | CubeMX 行为 |
|---|---|---|
| CubeMX | `*/Core`、`*/mx-generated.cmake` | 可再生成；自定义入口只放 USER CODE 区 |
| 项目 | `Secure/App`、`NonSecure/App` | CubeMX 不管理 |
| 项目 | `Secure_nsclib/safety_api.h` | 共用 ABI，版本化维护 |
| 项目 | Secure/NonSecure `CMakeLists.txt` | CubeMX 只首次创建，不覆盖现有文件 |
| 项目 | `ThirdParty/LwIP`、`Drivers/BSP` | 固定依赖，CubeMX 不管理 |
| 项目 | `tools`、`tests`、`docs` | CubeMX 不管理 |

Secure NSC veneer 位于 `Secure/Core/Src/secure_nsc.c` 的 USER CODE 区，所有
NonSecure 指针先做 CMSE 地址范围和访问属性检查，再复制到 Secure 栈上验证。
NonSecure 不能传入裸 TPIC 位图，只能提交有范围约束的高层执行器结构。
重新 ARM 时始终保持 TPIC `OE_N=High`：先连通 AHCT541，再释放
`CLR_N`，经 SPI 移入并锁存 32 位全零，最后才打开 `OE_N`。禁止在
缓冲器断开时用无效 RCK 脉冲代替该流程。

## 5. 下一版 PCB 强制 ECO：恢复硬件 I²C

ECO 编号建议：`ECU-R2-I2C-001`。

1. 原理图将 MCU PB8 接物理 SCL、PB9 接物理 SDA，禁止继续沿用当前交换网络。
2. 核对两线各有适合总线电容和目标速率的外部上拉，电压域与 ATECC608C I/O
   一致；保留 SCL/SDA/GND 测试点。
3. 新板固件在 CubeMX Secure context 中移除 PB8/PB9 GPIO Output，启用 I2C1：
   PB8=`I2C1_SCL`、PB9=`I2C1_SDA`、100 kHz、Analog Filter enabled，并把
   I2C1 设为 Secure。
4. 删除 `software_i2c.c` 的构建引用，ATECC608C 驱动只通过受限 Secure 服务
   暴露签名/验证等高层操作，不向 NonSecure 开放任意寄存器访问。
5. 投板评审必须把 PCB 网络表与 `.ioc` 引脚表交叉检查，并做 idle、ACK、
   stuck-low recovery、时钟拉伸和掉电回灌测试。

当前板代码和 UI 中均以 `SW_I2C_PCB_R1` capability 标识此临时方案，避免它
被误认为下一版的长期设计。

## 6. Option Bytes：CubeMX 外的人工步骤

Option Bytes 不属于 `.ioc`。烧录前先只读导出并核对：

- `TZEN` 已启用；
- `SECBOOTADD=0x0C000000`，开发期 `SECBOOT_LOCK=0xC3`（未锁定）；
- Bank1 secure watermark 覆盖 Secure/NSC 镜像；
- Bank2 为 NonSecure，可从 `0x08100000` 启动；
- 开发期保持 RDP Level 0 和可用 SWD；
- 不在未备份、未核对 RM0481 的情况下修改 SECWM、RDP、BOOT_LOCK。

本工程的 `tools/program_ecu.sh provision-and-flash` 在首次写 Option Bytes 前强制
核对芯片 ID/Product State，并备份完整 2 MiB Flash、Option Bytes 与 SHA-256；
任何备份失败都中止迁移。该保护不替代对授权、能量隔离和目标序列号的人工复核。

当前 linker：Secure `0x0C000000` 长 1000 KiB，Bank1 sectors 125/126 的
`0x0C0FA000..0x0C0FDFFF` 保留给阀参数双扇区日志，NSC `0x0C0FE000` 长
8 KiB；NonSecure `0x08100000` 长 1024 KiB。附加 linker ASSERT 即使 CubeMX
重写主 linker，也会阻止镜像进入参数扇区。若实芯片 Option Bytes 与此布局不一致，
停止烧录并先评审迁移；不得自动写入未知安全选项。

NonSecure `.bss` 中包含 ETH 描述符及全部 LwIP pool；链接片段
`NonSecure/App/Linker/eth_dma_sram3_guard.ld` 强制它不超过
`0x20060000`。这样后续静态数据增长超出 GTZC 已审查的 64 KiB DMA 窗口时会在
链接阶段失败，而不是在实机上表现为隐蔽的 Ethernet DMA 超时。

## 7. 明确保留项与工业化缺口

- ATECC608C 按需求延期；当前只有软件 I²C 基础层和总线健康诊断。
- CAN2 预留，不启动；车辆通信全部迁移到 CAN1。
- 当前 PCB 无转向执行器、主电源继电器、电源锁存和 indicator1/2/3 对应输出；
  V2 继续接收这些兼容字段但无动作，状态固定为 0。
- 已按用户最终确认区分两条速度链路。发动机高/低转速触发使用 K3/K24/K25：
  K3 只在触发期间切断原车信号，触点稳定 50 ms 后，K24 接 BAT（高转速）或
  K25 接 GND（低转速）连续 1 s；源继电器先释放，50 ms 后 K3 再释放恢复
  原车。安全域强制 K24/K25 互斥。龟兔档使用 K13/K14：控制有效时 K13 接管，
  K14 吸合接 GND=兔子，释放悬空=乌龟。两条链路均需带真实车端负载测量触点
  电压、动作时序和失电恢复状态。
- 油温曲线、油压/水位阈值仍缺少经认可的标定数据，不虚构工程量报警。
  阀电流已按原理图 1 mV/mA 基线实现可调增益/偏置、上电零点校准、过流、
  开路、非活动通道电流和 ADC 轨故障保护；这些阈值在量产前仍必须用标准电流表
  校准并做开短路故障注入，不能只依赖软件仿真。
- Secure Boot、镜像签名、防回滚、持久化故障日志、量产密钥流程、诊断访问
  控制尚未在本阶段需求中定义。
- 单路 MCU 急停输入和软件控制不能独立宣称 SIL/PL。最终车辆必须保留独立的
  硬件安全链，并依据目标法规完成风险分析、EMC、环境、电气和耐久验证。

自动审计入口：

```sh
./tools/cubemx_cli.sh validate
./tools/cubemx_cli.sh generate
./tools/audit_config.sh
```
