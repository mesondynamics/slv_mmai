# ECU KiCad / STM32CubeMX 配置审查

审查日期：2026-08-26；安全状态更新：2026-08-30

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
I²C：`PB8=SW_I2C_SDA`、`PB9=SW_I2C_SCL`，无需飞线。软件总线包含释放、
时钟拉伸超时和 stuck-low 恢复；ATECC 启动另行无条件执行
`START + 9 clocks + START + STOP`、reset/sleep 归一化，并在最长 725 ms 的有界
恢复期间服务 IWDG。ATECC608C 已在 Secure 域完成探测、锁区、
P-256 随机挑战和 MCU/安全芯片配对。总线、芯片、锁状态、配置 CRC、序列号、
MCU UID 或签名任一不匹配都会禁止执行器 ARM 和 OTA 试运行确认。

LwIP 在 STM32H5 当前 CubeMX 工程中作为项目自有中间件集成。CubeMX 继续生成
ETH HAL、描述符和引脚初始化；LwIP、LAN8742 接口和 ECU UDP 应用放在生成目录
之外。这样既保留 `.ioc` 的硬件配置，又避免 UI 再生成覆盖协议代码。

## 2. 已持久化的 CubeMX 配置

| 子系统 | 当前设置 | 设计约束 |
|---|---|---|
| 时钟 | HSE 25 MHz，SYSCLK/HCLK 250 MHz，CSS 开启 | HSE 故障进入 NMI 并关断输出 |
| TrustZone | ADC12、SPI4、TIM4、TIM6、TIM7、IWDG 为 Secure；对应 GPIO、EXTI、DMA 受保护 | NonSecure 不能直接写继电器、阀 PWM 或安全采样 |
| 急停 | PB2，Secure 双边沿 EXTI，标签 `ESTOP_DETECT` | 高电平立即锁存故障；恢复后仍需中性命令清故障 |
| 软件 I²C | PB8=软件 SDA、PB9=软件 SCL，GPIO Output、Open Drain、上电 High | 仅适用于当前接反的 PCB；无条件 9-clock host-reset 同步与 725 ms 上限属于 Secure 自维护代码，内部不上拉 |
| TPIC6A595 | SPI4 Secure；PE2 SCK、PE6 MOSI；8 bit、TX only、3.90625 Mbit/s | PE3 OE_N 上电 High；PE5 CLR_N、PE7 BUF_EN、PE4 RCK 上电 Low |
| 阀 PWM | TIM4 CH1=PB6、CH2=PB7；中心对齐 20 kHz；初值 0 | 正反向互锁；CH4=1 在中心对齐计数谷底、即 PWM 导通脉冲中心触发 ADC2 |
| 慢速 ADC | ADC1 九路，TIM6 TRGO 1 kHz，GPDMA1 CH0 circular | 外部量、VREFINT、MCU 温度；采样顺序见下表 |
| 电流 ADC | ADC2 两路，12.5 cycles，TIM4 TRGO，GPDMA1 CH1 circular；ADC2/GPDMA IRQ priority 1 | 前进 PA4、后退 PA6；20 kHz 电流 PI、上电零点校准、VREFINT 补偿和硬故障阈值 |
| FDCAN1 | Classic CAN 250 kbit/s，80% 采样点，自动重发；0 standard/6 extended filters | 车辆 J1939 接口；只接受六个 PGN |
| FDCAN2 | Classic CAN 250 kbit/s，0 filters | 预留，应用不启动控制器 |
| 速度输入 | TIM2 CH1=PA0；1 MHz、32 bit、输入滤波 8、IRQ | 20000 pulse/km，1500 ms 丢信号，IIR α=0.2 |
| Ethernet | RMII、MAC `8A:EA:B5:00:00:02`；PB14 PHY reset 上电 Low | ETH 初始化前保持复位 30 ms，再释放 LAN8742 |
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
   PB14 必须保持初值 Low：`eth.c` 的 CubeMX `USER CODE BEGIN ETH_Init 0` 会继续保持
   nRST 30 ms，然后释放并等待 50 MHz REFCLKO 稳定，随后同一函数才进入
   `HAL_ETH_Init()`。不得把复位释放移回 `ECU_AppInit()`；LAN8742A 在本板上是
   MCU RMII 时钟源，先初始化 ETH、后释放 PHY 会形成冷启动竞态。
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

本版硬件的 R70=10 kOhm、C65=100 nF 只能提供约 1 ms 的 nRST 延迟，不能满足
LAN8742A 规定的“外部电源稳定后至少 25 ms 再释放 nRST”。固件因此在 CubeMX
保留区内强制执行 30 ms 复位。R41 把 PHYAD0 固定为地址 0；驱动在地址 0 读取
PHYID1/PHYID2 并校验 `0x0007/0xC13x`，明确拒绝断开的 MDIO 总线返回的
`0xFFFF/0xFFFF`。PHY 暂时不可用时 LwIP 与安全主循环继续运行，250 ms 轮询会
自动重试，连续管理接口读错误也会退回重新探测，而不会进入 `Error_Handler()`。
HAL MDIO 在总线断开时可能返回 `HAL_OK + 0xFFFF`，底层读函数同样把该组合视为
读错误，避免运行期被误判成某个合法速率。

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

### 3.7 OEMiROT 应用 SAU 边界

保持 `partition_stm32h563xx.h` 中 SAU 为 disabled、`ALLNS=1`，region 0..7
全部 disabled。这与 ST 的 STM32H563 OEMiROT TrustZone 应用模板一致：Cortex-M33
地址别名的初始属性由 IDAU 决定，Flash 安全水印与 GTZC/MPCBB 再约束 Flash、
外设和 DMA 主设备。Secure `main.c` 只清理由 OEMiROT 继承的 MPU region，不再
部分重写并启用 SAU；否则 RSS/DA 遗留且未清除的 comparator 会重新生效。

Debug Authentication 或 Hotplug 会进入 RSS，属于侵入式现场。认证成功后必须
执行受控复位，再判断应用状态；不得把 resetless Hotplug 后读取到的 SAU 或故障
寄存器当成无探针冷启动现场。

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

## 6. OEMiROT 与 Option Bytes：CubeMX 外的受控步骤

Option Bytes、OBKeys 和产品生命周期不属于 `.ioc`。当前 OEMiROT 样件状态为：

- STM32H563 Device ID `0x484`，`TZEN=0xB4`，当前产品状态为 CLOSED
  (`0x72`)；
- `BOOT_UBE=0xB4`，`SECBOOTADD=0x0C000000` 且 `SECBOOT_LOCK=0xB4`；
- Bank1 全部 Secure、Bank2 NonSecure，禁止 Bank Swap；
- WRP group 0..3 保护 OEMiROT，HDP Bank1 `0x00..0x17` 隐藏 boot+scratch；
- DA OBK 权限为 `0x4040`：仅允许证书认证的 HDPL3 S/NS 临时调试和破坏性
  Full Regression；不开放 HDPL1/2，也不允许 Partial Regression；
- Secure SRAM2 在 reset 清除并启用 ECC。

`tools/provision_oemirot_open.sh` 在 OPEN 制造或 Full Regression 恢复的破坏性迁移前
核对芯片、探针和产品状态，备份完整 2 MiB Flash、Option Bytes、持久区及
SHA-256，并按阶段写日志。本样件曾完成 CLOSED/DA 验证，随后实际执行
Full Regression 并重建为 OPEN/ReleaseOpen；该过程保留为历史恢复证据。之后
Secure/NonSecure primary 完成 `1.0.15+0`、security counter=15、
`image_ok=0x01/0x01`、OTA `accepted_sequence=15` 的密码学审计，并通过不可变
CLOSED 事务安装 ReleaseClosed。事务目录为
`artifacts/device-backups/stm32h563-066BFF565456857187210935-20260830T063129Z-closed-transition/`，
UUID=
`0f3537fe-c04d-4298-ba50-995773e07a6a`，最终 phase=
`product_state_closed_verified`。随后在保持 `0x72 CLOSED` 期间通过 Ethernet 安装
1.0.16/counter16/accepted-sequence16；JP1 保持断开。不可逆转换、Full Regression
恢复和 DA 售后边界见 `ECU_Security_and_OTA.md`。

1.0.15 CLOSED 转换/恢复基线及当前 1.0.16 运行证据如下：

- ATECC 在持续上电条件下，以 option-byte reset、under-reset UID read、final reset
  的 hostile 顺序连续回归 20 次，20/20 均恢复配对认证、accepted15、零输出和
  Ethernet。实现使用 25 ms 步长、最长 725 ms 的启动预算，每次尝试服务 IWDG，
  超时保持 quarantine；
- pairing A sector=`0x080E0000`，B sector=`0x080E2000`。B 从全擦除修复后与 A
  committed record 字节一致，16 KiB dual-store SHA-256=
  `45f1f08b626f3934f2359e4d44b1993c9333c5275f62870d74a77caeb6c9f310`。CLOSED
  finalizer 已在任何目标写操作前完成独立 readback、dual verify 并持久化
  `pairing_store_dual_verified`，其后才进入 `pre_mutation_evidence_verified`；
- 当前无阀线圈功能回归 29/29，真实阀线圈为 SKIP。PI 调参高速遥测严格随 session
  门控，frames=`0 → 175 → 175`、drop=0；调参 RTT min/avg/max=
  `0.059/0.114/0.290 ms`，退出空闲为 `0.059/0.127/0.185 ms`；
- Full Regression recovery 已生成并离线验证通过，实物包为
  `artifacts/device-backups/stm32h563-066BFF565456857187210935-20260830T064244Z-final-1.0.15/`，
  schema v4 绑定上述 CLOSED UUID、精确 release/package 与 dual pairing evidence。manifest
  中 `capture.product_state="0xED OPEN"` 是 CLOSED 前不可变恢复输入，不是当前状态；
  `target.product_state="0x72 CLOSED"` 才是当前已验证生命周期；
- `artifacts/hardware-regression/20260830T064437Z-closed-da-service/` 证明认证前普通
  ST-Link 读取被拒绝，permission `c` 认证后 Secure/NonSecure primary 回读与
  CLOSED 事务一致，close-debug 后未认证读取再次被拒绝。本轮未再次执行
  破坏性 Full Regression；
- 唯一签名的 1.0.16/counter16/sequence16 包 SHA-256=
  `3e5ab07c7b6127dcedb46a1b4b1a695740904dd940d7128631f714d959a0b623`，已通过
  CLOSED Ethernet OTA 完成传输、reset/TEST swap 和确认；最终
  `state=IDLE/result=0/ota_result=0/accepted_sequence=16`，session 字段全 0；
- post-OTA MCU/ATECC/pairing、no-quarantine、零输出和调参关闭 PASS。OTA 前/
  后无阀线圈回归均为 29/29，真实电流跟踪仍 SKIP。telemetry 严格门控为
  `0 → 681 → 681`、drop=0；active/idle RTT 分别为
  `0.064/0.138/0.340 ms` 和 `0.055/0.104/0.148 ms`；
- 完整移除 ST-Link、断电至少 10 s 后仅 ECU 冷启动 PASS，100 包 0% 丢包，
  RTT min/avg/max=`0.067/0.116/0.328 ms`；
- 1.0.16 发布后 DA 终验证据为
  `artifacts/hardware-regression/20260830T073110Z-closed-ota-1.0.16-da-service/`：
  认证前普通读取 rc=1/拒绝；严格 discovery 确认 target `0x484`、SDA `2.4.0`、
  CLOSED、integrity VALID；permission `c` 认证 PASS。Secure
  `0x0C030000/0x30000` 与分段 NonSecure `0x08100000/0x50000` 回读经
  `tools/verify_closed_ota_readback.py` 验证为 `1.0.16+0`/counter16、
  `image_ok=0x01/0x01`。canonical swap size 为 Secure `0x2DDE8`、NonSecure
  `max(1.0.15 0x8D39, 1.0.16 0x8D38)=0x8D39`；审计 JSON SHA-256=
  `034c7d99996eed055346e315c3109ba355044051f53a679771b15db79ffbc4b8`，
  `evidence.json` SHA-256=
  `1478a4ac2c8748e585a86756f16a7fe6384a80f3b633196b023e23667347ca6e`；
- close-debug 后严格 CLOSED/VALID 复核 PASS，普通未认证读取再次 rc=1/拒绝；
  本轮未执行 Full Regression。完整移除 ST-Link 并断电至少 10 s 后的最终仅 ECU
  冷启动 PASS：MCU/ATECC/pairing、no-quarantine、relay/PWM 全零、调参关闭、
  telemetry=0，OTA IDLE/result0/ota_result0/accepted16；100 包 0% 丢包，
  RTT min/avg/max=`0.060/0.116/0.331 ms`。

OEMiROT 固定布局由 `Shared/ecu_flash_layout.h` 单点定义：boot 128 KiB、scratch
64 KiB、Secure primary/secondary 各 192 KiB、NonSecure secondary/primary 各
320 KiB，中间 `0x0C0E0000..0x0C0FFFFF` 的 128 KiB 专用于安全身份、OTA 日志
和阀参数。应用 vector 分别为 `0x0C030400` 和 `0x08100400`。构建期 ASSERT 阻止
镜像、trailer 或持久区互相越界；不得再用旧的直接 Secure/NonSecure linker 布局
烧录 OEMiROT 样件。

NonSecure `.bss` 中包含 ETH 描述符及全部 LwIP pool；链接片段
`NonSecure/App/Linker/eth_dma_sram3_guard.ld` 强制它不超过
`0x20060000`。这样后续静态数据增长超出 GTZC 已审查的 64 KiB DMA 窗口时会在
链接阶段失败，而不是在实机上表现为隐蔽的 Ethernet DMA 超时。

## 7. 明确保留项与工业化缺口

- ATECC608C 启动验证、MCU 配对、锁区和 OEMiROT/OTA 已实现；当前样件为
  `ReleaseClosed` + 已确认 1.0.16 配对镜像，生命周期为 `0x72 CLOSED`。
  1.0.15 的不可变 CLOSED 转换/schema-v4 恢复和 DA 记录仍是转换基线；1.0.16
  已独立完成精确 primary DA 回读、close-debug 和最终无探针冷启动。此前实际
  Full Regression→OPEN 和 1.0.14 的三次无探针冷启动继续作为历史证据；本轮
  没有执行 Full Regression。
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
- 控制/诊断 V2 UDP 仍面向隔离车辆网络，不具备端到端鉴权；只有 OTA 包有独立
  ECDSA 传输签名和 OEMiROT 镜像认证。不可把 50001..50006 直接暴露到不可信网络。
- 持久化故障事件日志尚未实现；量产还需为每块板建立唯一序列、ATECC 配对记录、
  证书/密钥托管、工装权限和报废/返修流程。
- 单路 MCU 急停输入和软件控制不能独立宣称 SIL/PL。最终车辆必须保留独立的
  硬件安全链，并依据目标法规完成风险分析、EMC、环境、电气和耐久验证。

自动审计入口：

```sh
./tools/cubemx_cli.sh validate
./tools/cubemx_cli.sh generate
./tools/audit_config.sh
```
