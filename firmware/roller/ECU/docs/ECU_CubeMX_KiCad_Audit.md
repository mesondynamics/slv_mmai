# ECU KiCad / STM32CubeMX 工程审查与交付说明

审查日期：2026-08-26  
目标器件：STM32H563ZIT6，LQFP144  
CubeMX：6.18.0；STM32CubeH5：1.7.0  
PCB 工程：`pcb/roller/ecu`  
固件工程：`firmware/roller/ECU`

## 1. 结论

工程已从 NUCLEO 模板残留状态改为自定义 ECU 板配置，并已由 CubeMX 自己回读、重新生成及双域编译。关键安全状态和外设归属已经写入 `ECU.ioc`，不是只修改生成后的 C 文件，因此以后从 CubeMX UI 再生成仍会生效。

当前是“工业控制开发基线”，不是经认证的功能安全产品。单路光耦急停检测、软件 PWM 关断和普通 MCU 看门狗不能单独形成 IEC 61508 SIL 或 ISO 13849 PL 安全功能；最终安全等级必须由系统风险分析、独立硬件切断链路、诊断覆盖率和验证证据共同确定。

## 2. 已固化到 CubeMX 的配置

| 功能 | 当前配置 | 设计理由 |
|---|---|---|
| 系统时钟 | HSE 25 MHz，SYSCLK/HCLK 250 MHz，HSE CSS 开启 | 外部晶振失效进入 NMI，Secure 异常路径立即关断输出 |
| TrustZone | ADC12、TIM4、TIM6、I2C1、IWDG 为 Secure；对应 GPIO/EXTI 和 GPDMA 通道为 Secure | NonSecure 网络/协议代码不能直接改阀 PWM、读安全总线或喂狗 |
| 急停 | PB2 `ESTOP_DETECT`，双边沿 Secure EXTI，电平高表示急停/线路故障 | 与 KiCad 光耦输出一致；上升沿立即锁存故障，下降沿不会自动重新使能 |
| 阀 PWM | TIM4 CH1=PB6、CH2=PB7，中心对齐，20 kHz，初始占空比 0 | 250 MHz / (2 × 6250) = 20 kHz；正反向软件互锁 |
| 电流采样 | ADC2：PA4/IN18、PA6/IN3；TIM4 TRGO 上升沿；GPDMA1 CH1 循环 | TIM4 内部 CH4 比较值 3125，在 PWM 中点触发；采样时间 47.5 cycles |
| 慢速模拟量 | ADC1 共 9 路，TIM6 TRGO 1 kHz，GPDMA1 CH0 循环 | PC0、PC2、PC3、PA3、PA5、PB0、PB1、VREFINT、芯片温度；外部通道 92.5 cycles，VREFINT/芯片温度 640.5 cycles |
| CAN1/CAN2 | Classic CAN，250 kbit/s，25 MHz kernel，Prescaler=4，Seg1=19，Seg2=5，SJW=4 | 25 tq/bit，80% 采样点；自动重发和 Transmit Pause 开启；各预留 8 个标准、2 个扩展过滤器 |
| SPI4/TPIC | PE2 SCK、PE6 MOSI，主机单向发送，8 bit，15.625 Mbit/s | 清除原 4-bit/62.5 Mbit/s 默认值，给板级串联驱动留足时序裕量 |
| TPIC 安全脚 | PE3 OE_N 上电高；PE5 CLR_N 低；PE7 CTRL_BUF_EN 低；PE4 RCK 低 | 与 R72/R73 硬件上下拉共同形成上电关闭状态 |
| I2C1 | PB8=SCL、PB9=SDA，100 kHz，模拟滤波开启，数字滤波 2 cycles | I2C 作为 Secure 低速安全传感器总线；物理换线要求见第 5 节 |
| 速度输入 | TIM2 CH1=PA0，1 MHz 计数，32-bit free-running，输入滤波 8，IRQ 优先级 6 | 便于后续做周期/频率捕获并降低毛刺影响 |
| PHY | RMII 引脚按原理图配置；PB14 `RMII_NRST` 上电保持低 | NonSecure 应用等待 10 ms 后释放 PHY 复位 |
| 看门狗 | Secure IWDG，LSI 32 kHz，Prescaler=32，Reload=1999，约 2 s | NonSecure 只能通过递增 heartbeat 的 NSC API 请求刷新 |
| 未使用脚 | 不主动初始化，保持 H5 复位后的模拟/高阻状态 | 避免无连接引脚数字输入浮动；SWD、晶振和已用引脚除外 |

ADC1 DMA 数组顺序固定如下，后续标定代码必须按此顺序解释：

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

ADC2 索引 0 为前进阀电流 PA4，索引 1 为后退阀电流 PA6。

ADC 内核输入为 250 MHz、异步预分频为 4，因此转换时钟为 62.5 MHz，低于
H563 数据手册给出的 75 MHz ADC 上限。640.5 cycles 对应约 10.25 µs，满足
VREFINT 至少 4.3 µs、内部温度传感器至少 9 µs 的采样时间要求。

## 3. 再生成安全的软件结构

CubeMX 生成目录仍由 `ECU.ioc` 管理。项目自有代码放在不会被生成器删除的位置：

- `Secure/App/Src/safety_service.c`：急停锁存、输出关断、PWM 方向互锁、ADC DMA 启动、GTZC/ADC 异常处理和 IWDG 服务。
- `Secure_nsclib/safety_api.h`：Secure/NonSecure 共用 ABI、状态位、返回码和常量。
- `Secure/Core/Src/secure_nsc.c` 的 USER CODE 区：NSC veneer，并对 NonSecure 输出指针做 CMSE 范围及读写属性检查。
- `NonSecure/App/Src/ecu_app.c`：不自动使能执行器，只释放 PHY、读取安全快照并提供基础 heartbeat。
- Secure/NonSecure `CMakeLists.txt`：自有源目录、额外警告和 `-fno-common`；这些文件由 CubeMX 只创建一次，不会重复覆盖。

安全输出状态机遵循以下原则：

1. 上电、异常、GTZC 非法访问、ADC 错误或急停时，PWM CCR1/CCR2 立即清零，TPIC OE_N 拉高，CLR_N 和缓冲使能拉低。
2. 急停恢复只消除实时电平，故障锁存仍保留；NonSecure 必须显式调用清故障，再单独调用 ARM。
3. 正向和反向 PWM 不允许同时非零；命令序号必须单调递增，重复或旧命令被拒绝。
4. ARM 前先锁存全零 TPIC 移位寄存器，再开放缓冲和输出。
5. 当前 NonSecure heartbeat 是开发基线。正式应用必须把控制任务、CAN、传感器更新和故障管理的健康结果汇总后，才允许 heartbeat 递增。

## 4. 在 CubeMX UI 中的核对方法

正常情况下不需要再手工改变这些页面；以下步骤用于你打开 UI 后验收。使用 CubeMX 6.18.0 打开根目录 `ECU.ioc`，不要以 NUCLEO 板模板重新建工程。

### 4.1 Pinout & Configuration

1. 切换到 Secure context。
2. 在 `System Core > GTZC_S` 核对 ADC12、I2C1、IWDG、TIM4、TIM6 显示为 Secure，并启用对应 Illegal Access interrupt。
3. 核对 PB2 是 Secure `GPIO_EXTI2`，模式为 Rising/Falling，标签为 `ESTOP_DETECT`；不要再使用旧的 PG2。
4. 核对 PB6/PB7、所有 ADC 引脚、PB8/PB9 为 Secure pin attribute。
5. 核对 PE3 初始电平是 High，PE5/PE7 是 Low；PB14 在 NonSecure context 初始为 Low。

### 4.2 ADC 和 DMA

1. ADC1 regular group 应有 9 ranks，External Trigger 为 `TIM6 TRGO / Rising edge`，Data Management 为 `DMA Circular`。
2. ADC2 regular group 应有 2 ranks，External Trigger 为 `TIM4 TRGO / Rising edge`，Data Management 为 `DMA Circular`。
3. GPDMA1 Channel 0/1 分别请求 ADC1/ADC2，source fixed、destination increment、两侧 half-word、circular，并显示 Secure source/destination/channel。

### 4.3 TIM4

1. Counter Mode 为 `Center Aligned mode 1`，Prescaler=0，Period=6249。
2. CH1/CH2 为 PWM Generation，Pulse=0。
3. 必须能看到虚拟通道 `PWM Generation4 No Output`，Pulse=3125。
4. Master Output Trigger 为 `OC4REF`。不要把虚拟 CH4 删除，否则 ADC2 不再随 PWM 中点采样。

### 4.4 FDCAN

两个控制器都应显示 250000 bit/s、80% sample point、Classic frame、Auto Retransmission enabled。中断使用 IT0，优先级 5。

过滤器的数量已预留，但过滤器 ID/Mask 尚未配置，因为工程中没有整车 CAN communication matrix。正式启用 CAN 前，必须按网络矩阵逐条配置白名单过滤器，再调用 `HAL_FDCAN_Start()`；不能依赖默认接受全部报文。

### 4.5 Project Manager

保持：Toolchain=CMake、Keep User Code enabled、删除旧生成文件 enabled。生成后运行：

```sh
./tools/audit_config.sh
./tools/build.sh Debug
```

CubeMX 6.18 生成的顶层 `ExternalProject` 会让 Debug/Release 共用
`Secure/build` 和 `NonSecure/build`，反复切换时可能沿用另一配置的对象。
`tools/build.sh` 因此按配置直接生成到各域的 `build/Debug` 或
`build/Release`；工程验收和 CI 请使用该入口，不要把顶层增量构建的成功
当作双配置已分别重编译的证据。

## 5. 必须完成的硬件动作：I2C 飞线

KiCad 网络把 MCU PB8/PB9 与板上 I2C SCL/SDA 物理接反。固件必须保持 STM32 标准复用：PB8=SCL、PB9=SDA；不能靠交换 GPIO 标签规避，因为外设开漏时序由硬件复用决定。

当前板处理要求：

1. 断电并确认 3V3、传感器电源无残压。
2. 切断 PB8 到错误 SDA 网络、PB9 到错误 SCL 网络的两条连接。
3. 交叉飞线，使 PB8 最终到物理 SCL，PB9 最终到物理 SDA。
4. 用断电通断档确认无短路，并确认每根线各自有上拉到正确 I/O 电压域。
5. 上电后先用示波器验证 idle high、100 kHz SCL 和 ACK，再连接正式传感器。

此项不能由 CubeMX 或软件消除。未完成飞线前，不要执行 I2C 总线功能测试。

## 6. 必须在 STM32CubeProgrammer 中配置/核对的 Option Bytes

Option Bytes 不保存在 `.ioc`，而且错误设置可能触发 mass erase 或导致设备无法按当前镜像启动。操作前先在 STM32CubeProgrammer 导出完整 Option Bytes/readout 报告，并保留可连接-under-reset 的调试口。

当前生成工程采用 STM32H563 官方 TrustZone 双 bank 基线：

- Secure image：Bank1 secure alias `0x0C000000`，1 MiB，其中末尾 8 KiB 为 NSC linker region。
- NonSecure image：Bank2 `0x08100000`，1 MiB。
- Secure RAM：SRAM1+SRAM2 共 320 KiB。
- NonSecure RAM：SRAM3 共 320 KiB。

在 `Option Bytes` 页面按“名称和读回结果”核对，不要只照抄十六进制值：

1. `TZEN` 必须为 Enabled。
2. Bank1 secure watermark 必须覆盖当前 Secure/NSC 镜像；Bank2 secure watermark 必须为空，即 `SECWM2_PSTRT > SECWM2_PEND`，使整个 Bank2 为 NonSecure。对 2 MiB H563，Bank1 通常是 128 个 8 KiB sector；写入前仍须用器件读回和 RM0481 核对边界。
3. 开发期保留 RDP Level 0，完成安全启动、恢复流程和量产烧录验证后再评审 RDP/BOOT_LOCK；不要在开发阶段提前锁死。
4. 量产配置建议把 IWDG 设为 Hardware 模式，使其早于应用代码启动；调试配置可以先保留 Software 模式。两种配置都要分别做复位原因和超时试验。
5. BOR level 必须根据 ECU 电源掉电曲线、外部 supervisor 和 Flash 工作电压实测后确定，不能只按模板默认值。

原方案中“Secure Flash 256 KiB / NonSecure 1792 KiB”的优化本次没有强行写入 linker：CubeMX 6.18 的该 TrustZone CMake 模板按 bank 生成 1 MiB/1 MiB，直接手改 linker 会在后续 UI 生成时被覆盖，还必须同步 SECWM、NSC、VTOR 和烧录流程。若后续确实需要 1792 KiB NonSecure 空间，应单独建立受版本控制的 memory-layout/post-generation 流程并在样片上验证后实施，不能只改一个地址。

## 7. 尚未配置，需由系统需求决定

以下不是可以从原理图可靠推断的参数，因此保留为明确的后续工作，而不是填入任意默认值：

- CAN1/CAN2 标准/扩展 ID 白名单、报文周期、timeout、bus-off 恢复策略和节点地址。
- 电流采样的零点、增益、允许峰值、短路阈值、去抖时间和关断反应时间；阈值确定前安全服务只完成采样与急停关断。
- 7 路外部模拟量的分压比、NTC/传感器曲线、开短路诊断上下限和生产标定数据。
- Secure I2C 设备地址、寄存器白名单、总线恢复策略以及数据 CRC/PEC。当前 I2C 不向 NonSecure 暴露任意读写接口。
- Ethernet 协议栈、PHY 地址/型号驱动、MAC 地址来源、网络安全策略和链路诊断。当前仅完成 RMII MAC 和 PHY reset 基线。
- TIM2 速度输入的齿数/脉冲数、最小最大周期、timeout 和溢出处理。
- MPU 的最终分区、不可执行 RAM、外设访问策略、Secure Boot/镜像签名、回滚保护和密钥配置。
- 故障日志的掉电保存、写入寿命和诊断服务。

## 8. 上板验收清单

1. 只烧 Secure 镜像时，确认阀 PWM、TPIC 输出和 PHY reset 均保持安全状态。
2. 烧双镜像后测 PB6/PB7：20 kHz、中心对齐、未 ARM 时占空比为 0。
3. 示波器/调试寄存器确认 TIM4 CCR4=3125，ADC2 DMA sequence 以约 20 kHz 增长；ADC1 sequence 以约 1 kHz 增长。
4. 急停 PB2 拉高时，测量输出关断延时并确认故障锁存；PB2 恢复低后输出不得自动恢复。
5. 停止 NonSecure heartbeat，确认约 2 s 看门狗复位并检查 reset cause。
6. 断开/扰动 HSE，确认 CSS/NMI 路径使输出安全并由看门狗复位。
7. 两路 CAN 分别做 250 kbit/s 示波器位宽、ACK、错误帧、bus-off 和恢复测试；按 ISO 11898 总线拓扑核对终端电阻。
8. 完成 I2C 飞线后做 idle、时钟、ACK、上拉电压和 stuck-low recovery 测试。
9. 做电源缓升/缓降、棕断、反复上电、EFT/ESD/浪涌后的安全状态验证；EMC 等级由整机标准决定。

自动静态配置审计：

```sh
./tools/audit_config.sh
```

CubeMX 无界面回读与再生成：

```sh
./tools/cubemx_cli.sh validate
./tools/cubemx_cli.sh generate
```

`validate` 会把 CubeMX 展开的配置写到 `/tmp/ECU-cubemx-expanded.ioc`，便于检查默认参数；该文件不是项目配置源。

## 9. 设计依据

- ST DS14258 Rev. 6，STM32H562xx/H563xx 数据手册：ADC 最大时钟、VREFINT
  和温度传感器采样时间、电气特性。
  <https://www.st.com/resource/en/datasheet/stm32h563ai.pdf>
- ST RM0481，STM32H5 参考手册：RCC、ADC、TrustZone、Flash secure
  watermark、GTZC 和 Option Bytes。
  <https://www.st.com/resource/en/reference_manual/rm0481-stm32h533-stm32h563-stm32h573-and-stm32h562-armbased-32bit-mcus-stmicroelectronics.pdf>
- 本仓库 KiCad 原理图和 PCB：所有引脚、网络名、硬件上下拉及 I2C 换线结论的
  板级依据。软件配置不能替代数据手册的电气限制或实板测量。
