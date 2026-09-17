# ECU 原理图公开依据与可追溯性索引

本文档为原理图 `PROJECT1.kicad_sch` 及 `PAGE1.kicad_sch`～`PAGE9.kicad_sch` 建立公开技术依据索引。

## 证据等级

| 等级 | 含义 | 使用边界 |
| --- | --- | --- |
| A | 同型号器件的官方典型应用、参考原理图或评估板与本页拓扑近似 | 可以作为直接复核依据；元件值仍应按实际工况重新核算 |
| B | 同型号官方数据手册/应用笔记给出的连接规则、接口电路或设计方法 | 支持局部结构或设计规则，不表示整块电路完全相同 |
| C | 公开的通用原理、相邻型号资料或器件能力说明 | 只证明方案合理性；不能据此认定历史来源或完成参数验证 |

## 电路划分与来源映射

### PAGE1 — 电源变换与状态指示

| ID | 电路与覆盖范围 | 公开依据（文档定位） | 等级 | 对应关系与差异 |
| --- | --- | --- | --- | --- |
| P1-01 | 高电流同步降压：`U8`、`L2` 及其输入、输出、反馈、频率设定和去耦网络 | [S01](#s01)，Figure 10，p.69 | A | 同为 MPQ4371-1000 的 5 V、2.2 MHz 典型应用；反馈、频率设定、输入/输出电容结构高度一致。工程中的器件额定值、并联数量及使能/模式配置需以原理图和负载验证为准。 |
| P1-02 | 小功率同步降压：`U9`、`L1` 及 `C7,C10-C12,C14-C18,C23,C25,C26,R23-R30,R32,R33,R50,R60,R61,R64` | [S02](#s02)，Application Information 与 Typical Application Circuits，pp.21–25 | A | 拓扑、软启动、反馈、输入/输出去耦与 MPQ2178 典型应用一致；输出电压和可选/未装参数属于工程定制。 |
| P1-03 | 两组电源良好/状态指示与开漏缓冲：`Q1,Q9,D7,D14,R34,R35,R63,R65,R66` | [S01](#s01)，Power Good；[S02](#s02)，Power Good；[S15](#s15)，MOSFET switching application | B | 数据手册支持开漏状态输出和外部上拉；LED 与小信号 MOSFET组合属于实现层设计，并非数据手册中的完整复制电路。 |

### PAGE2 — MCU、时钟、复位、调试与安全器件

| ID | 电路与覆盖范围 | 公开依据（文档定位） | 等级 | 对应关系与差异 |
| --- | --- | --- | --- | --- |
| P2-01 | MCU 供电、模拟参考、内部稳压电容与就地去耦：`U1` 及 `C6,C28-C46,C51,R36,R37` | [S03](#s03)，Figures 2、15、19，pp.5、36、40 | A | AN5711 明确覆盖 STM32H563 系列的电源域、参考设计和去耦布局；具体电容分配需结合所用封装引脚逐项核对。 |
| P2-02 | 外部复位按键、上拉/滤波与 ESD：`SW1,D1,R49,C46` | [S03](#s03)，Figure 9，p.15；[S28](#s28) | B | 复位网络符合 STM32H5 硬件指南，ESD 器件能力由其官方数据手册支持；保护等级仍取决于布局与脉冲源阻抗。 |
| P2-03 | 启动配置跳线：`JP1` 及相邻配置电阻 | [S03](#s03)，Boot configuration，p.26 | B | 文档给出启动脚配置原则；跳线形式及默认状态是工程选择。 |
| P2-04 | 25 MHz 高速外部晶振：`Y2,C47,C48` | [S04](#s04)，§§3–4（Pierce oscillator、load capacitance、crystal selection） | B | 结构符合 Pierce 振荡器；负载电容必须按晶体 CL、引脚和 PCB 寄生重新核算。 |
| P2-05 | 32.768 kHz 低速外部晶振：`Y1,C49,C50` | [S04](#s04)，§§3–4；[S03](#s03)，clock recommendations | B | 结构与低速晶体网络一致；低功耗晶体的驱动裕量和负阻测试不由原理图本身证明。 |
| P2-06 | SWD 调试接口：`J1` | [S03](#s03)，Figure 12，p.30 | A | 信号集合与 STM32H5 官方 SWD 连接方式一致；连接器针序是项目实现。 |
| P2-07 | I²C 安全器件及去耦/上拉：`U221,R105,R106,C45` | [S05](#s05)，Figure 1 与 Table 1；[S06](#s06)，DT100104 schematic，sheet 1 | A | 官方板原理图展示 ATECC608C 的 I²C、上拉和 100 nF 去耦。地址/配置区内容不在原理图可追溯范围内。 |

### PAGE3 — 10/100 Ethernet PHY 与物理接口

| ID | 电路与覆盖范围 | 公开依据（文档定位） | 等级 | 对应关系与差异 |
| --- | --- | --- | --- | --- |
| P3-01 | RMII PHY、偏置、内部稳压电容与电源滤波：`U4,FB1,R53,C54-C63` 及相邻去耦 | [S07](#s07)，p.13 与 Figures 3-20、3-21，pp.53–54 | A | 12.1 kΩ RBIAS、内部 1.2 V 稳压电容和单电源连接与 LAN8742A 官方应用图一致。 |
| P3-02 | PHY 25 MHz 晶振：`Y3,C52,C53` | [S07](#s07)，clock/oscillator sections；[S04](#s04)，§§3–4 | B | PHY 数据手册规定时钟接口，AN2867提供晶体负载核算方法；实际起振裕量需测试。 |
| P3-03 | MDI 端接、共模磁性器件与外部连接器：`R58,R59,R67,R68,R54-R56,C64,T201,J3` | [S07](#s07)，Figure 3-23，p.56；[S08](#s08)；[S09](#s09) | A | 49.9 Ω 端接、磁性器件、Bob Smith 类端接与官方单电源双绞线接口图一致；连接器由项目改为 M12 X-code，针序须按其图纸核对。 |
| P3-04 | 高速线 ESD 保护：`U5` | [S10](#s10)，Application Diagrams 与 PCB layout recommendations | B | USBLC6-4 的低电容多线保护结构可用于高速差分接口；具体 IEC 等级依赖布局，且该器件名称并不把用途限定为 USB。 |
| P3-05 | PHY 状态 LED 与串联电阻：`D19,D20,R51,R57` | [S07](#s07)，LED pins/configuration | B | PHY 提供 LED 输出及配置说明；颜色、亮度和电阻值为工程选择。 |
| P3-06 | RMII 串联阻尼/配置电阻：`R38-R48,R69,R70,R113-R115` | [S07](#s07)，RMII interface、strap inputs 与 layout guidance | B | 数据手册支持启动配置和数字接口布线原则；33 Ω 阻尼值须结合走线阻抗和边沿测量确认。 |

### PAGE4 — 两路 CAN 物理层

| ID | 电路与覆盖范围 | 公开依据（文档定位） | 等级 | 对应关系与差异 |
| --- | --- | --- | --- | --- |
| P4-01 | 两个同构 CAN 收发与可配置 120 Ω 端接通道：`U13,U14,C40,C85,R42,R43,R107,R108` | [S11](#s11)，Figures 38–39，p.27 | A | 官方图给出 CAN 总线与 120 Ω 端接原则；0 Ω 配置电阻使端接是否接入由装配方案决定。 |
| P4-02 | 两路 CAN 线对浪涌/ESD 保护：`D21,D22` | [S12](#s12)，High-Speed/Fault-Tolerant CAN Surge Protection application figure | A | NUP2105L 官方应用图即为 CANH/CANL 双线保护；最终抗扰度仍需结合接地回路和 PCB 布局验证。 |

### PAGE5 — 逻辑缓冲与串行低边驱动

| ID | 电路与覆盖范围 | 公开依据（文档定位） | 等级 | 对应关系与差异 |
| --- | --- | --- | --- | --- |
| P5-01 | 八路逻辑缓冲/三态控制：`U300,C68,R75,R76,R78` | [S14](#s14)，Function Table、Typical Application 与 power-supply recommendations | B | 器件功能与使能脚连接有直接依据；各通道所驱动的工程信号不在公开来源中。 |
| P5-02 | 四片级联串入并出低边驱动：`U301-U304,C5,C21,C66,C67,R71-R74` | [S13](#s13)，§7.3.3 与 §7.3.4 Cascaded Application，p.13 | A | SER OUT 到下一片 SER IN 的级联方式、低边开漏 DMOS 和感性负载钳位均由官方资料明确给出；负载数量与映射为工程定制。 |
| P5-03 | 小信号 MOSFET 控制/电平处理：`Q6,R77` | [S15](#s15)，Description and Applications | C | 器件适合开关和电源管理；仅凭公开数据手册无法确定该局部逻辑的唯一参考电路。 |

### PAGE6 — 继电器触点阵列与电源储能

| ID | 电路与覆盖范围 | 公开依据（文档定位） | 等级 | 对应关系与差异 |
| --- | --- | --- | --- | --- |
| P6-01 | 18 个常开继电器单元：`K4,K6,K8,K10,K12,K14-K23,K25-K27` | [S16](#s16)，HFV6 Contact Form 1 Form A 与 ratings | B | 库封装和触点形式对应 HFV6 系列 1 Form A；整个触点矩阵的组合关系是项目专用，未找到公开的同拓扑参考设计。 |
| P6-02 | 9 个转换继电器单元：`K1,K2,K3,K5,K7,K9,K11,K13,K24` | [S16](#s16)，HFV6 Contact Form 1 Form C 与 ratings | B | 器件级触点形式可追溯至厂商资料；联锁/切换组合仅能按本页连线复核，不能声称来自某一公开示例。 |
| P6-03 | 继电器电源母线储能：`C22,C69-C71` | [S17](#s17)，Input and Output Capacitor Selection，bulk-capacitance discussion | C | 并联大容量电容符合负载阶跃储能的一般原则；容量、ESR、纹波电流、浪涌及故障能量必须按实际继电器同时动作数验证。 |

### PAGE7 — 两路功率低边驱动与电流检测

| ID | 电路与覆盖范围 | 公开依据（文档定位） | 等级 | 对应关系与差异 |
| --- | --- | --- | --- | --- |
| P7-01 | 两个同构的栅极驱动、N-MOSFET 低边开关和续流二极管通道：`U10,U11,Q2,Q3,D2,D3,R79-R82,C72-C75` | [S18](#s18)，Typical Application；[S19](#s19)；[S20](#s20) | B | UCC27517A-Q1 支持低边 MOSFET 栅极驱动，MOSFET 与 60 V/5 A Schottky 的额定能力由各自数据手册支持；完整功率级及门极电阻值是工程实现。 |
| P7-02 | 两路 50 mΩ 分流电阻与 INA240 电流检测：`R1,R2,U6,U12,R83,R84,C76,C77` | [S21](#s21)，§9.2.2 Solenoid Drive Current-Sense Application，Figure 9-5；[S22](#s22) | A | INA240 官方应用与 PWM/感性负载电流检测直接对应；分流器为 50 mΩ、3 W。量程、功耗、Kelvin 引线和滤波仍须按峰值电流验证。 |

### PAGE8 — 模拟量、温度、隔离量与脉冲输入

| ID | 电路与覆盖范围 | 公开依据（文档定位） | 等级 | 对应关系与差异 |
| --- | --- | --- | --- | --- |
| P8-01 | 四个同构的受保护模拟输入：`D4,D9,R85,R86,R94,C78`；`D12,D13,R91,R97,R98,C82`；`D15,D16,R99-R101,C83`；`D17,D18,R102-R104,C84` | [S23](#s23)，ADC input impedance/RC guidance；[S24](#s24)；[S25](#s25) | B/C | 分压、串联限流、RC 低通和轨钳位符合 MCU ADC 通用前端；ESD5V0D5 的制造商无法由当前匿名符号唯一确定，因此该器件只给出同型号公开资料，列为 C 级器件证据。 |
| P8-02 | 两个同构 NTC 分压/滤波输入：`J2,R109,R110,C86`；`J4,R111,R112,C87` | [S26](#s26)，Monitoring NTC Thermistor Circuit With Single-Ended ADC | B | 电阻分压与 ADC 输入电容的结构一致；NTC 的 B 值、标称阻值、线束电阻和温度换算表须由实际探头补充。 |
| P8-03 | 电源电压分压、RC 滤波与双肖特基钳位：`R92,R93,R95,R96,C79,D10` | [S23](#s23)，ADC input protection/RC guidance；[S24](#s24) | B | 结构符合高压量测的电阻分压和限流钳位原则；量程与故障注入电流需按 ADC 绝对最大额定值重新计算。 |
| P8-04 | 光耦隔离数字输入：`R117,R118,U15,R121,R122,C80` | [S27](#s27)，Figure 1 switching test circuit 与 electrical characteristics；[S31](#s31) | B | 输入限流、光晶体管输出上拉和滤波有器件/应用资料支持；阈值、CTR 老化裕量与现场电压范围是项目参数。 |
| P8-05 | 受保护的脉冲整形输入：`D8,R87,R88,D11,Q4,R89,C81,U7,R90` | [S29](#s29)，Schmitt-trigger input；[S30](#s30)；[S24](#s24) | B/C | TVS/肖特基限制瞬态，BJT 提供电平/电流处理，施密特反相器恢复边沿；该组合未找到同型号完整公开参考图，故整体按 C 级、各器件规则按 B 级。 |

### PAGE9 — 电源入口、反接保护与连接/机械件

| ID | 电路与覆盖范围 | 公开依据（文档定位） | 等级 | 对应关系与差异 |
| --- | --- | --- | --- | --- |
| P9-01 | 入口双向 TVS 与高低频旁路：`D5,C88,C89` | [S32](#s32)，SMCJ24CA-Q electrical characteristics；[S17](#s17) | B | 器件型号对应 24 V、1500 W、双向汽车级 TVS；电容并联用于宽频旁路。整机脉冲等级仍取决于源阻抗、线束和布局。 |
| P9-02 | 理想二极管控制器与外部 N-MOSFET 反接/反向电流保护：`U2,Q5` | [S33](#s33)，Typical Application Schematic 与 Figure 3；[S34](#s34) | A | LM74700-Q1 官方典型图即采用外部 N 沟道 MOSFET，并包含入口 TVS/电容；所选 60 V MOSFET 的耐压、导阻和热设计需针对实际浪涌复核。 |
| P9-03 | 两个密封板端连接器：`J6,J7` | [S35](#s35)；[S36](#s36) | A | 厂商页面/图纸分别确认 35 位和 23 位 AMPSEAL 直角板端连接器。针位的电气分配属于工程定义。 |
| P9-04 | 带焊盘安装孔：`H1-H4` | 本工程机械/接地实现，无唯一外部参考电路 | C | 位号覆盖完整；是否连接机壳、保护地或信号参考必须结合 PCB、结构和 EMC 方案判断。 |

## 尚不能由公开资料证明的内容

- PAGE6 的继电器矩阵、PAGE8 的完整脉冲整形链路以及各页的网络分配属于项目级组合。本文只证明其器件用法或通用原理有公开依据，不把它们描述成某份公开参考设计的复制品。
- `ESD5V0D5` 在当前匿名库中的数据手册字段指向另一相邻系列，且型号本身有多个供应商版本；在 BOM/采购料号未明确前，只能使用同型号公开资料作 C 级参考。
- 公开典型应用不能替代设计验证。汽车电源瞬态、EMC、热、降额、晶体负阻、ADC 故障注入、继电器触点寿命和功能安全均需单独计算与试验。
- 文档链接可能随厂商网站更新；下列条目记录了文档号、章节或图号，以便链接迁移后继续检索。

## 公开来源

访问日期均为 **2026-09-15**。

<a id="s01"></a>**S01 — Monolithic Power Systems, MPQ4371-AEC1 Datasheet**, Rev. 1.0，2024-03-21，Figure 10 “Typical Application Circuit for MPQ4371-1000”，p.69。  
https://www.monolithicpower.com/en/documentview/productdocument/index/version/2/document_type/Datasheet/lang/en/sku/MPQ4371GVE-AEC1/

<a id="s02"></a>**S02 — Monolithic Power Systems, MPQ2178-AEC1 Datasheet**, Rev. 1.1，2023-05-04，Application Information 与 Typical Application Circuits，pp.21–25。  
https://www.monolithicpower.com/en/documentview/productdocument/index/version/2/document_type/Datasheet/lang/en/sku/MPQ2178GQHE-AEC1/

<a id="s03"></a>**S03 — STMicroelectronics, AN5711: Getting started with STM32H5 MCU hardware development**, Rev. 3，Figures 2、9、12、15、19。  
https://www.st.com/resource/en/application_note/an5711-getting-started-with-stm32h5-mcu-hardware-development-stmicroelectronics.pdf

<a id="s04"></a>**S04 — STMicroelectronics, AN2867: Oscillator design guide for STM8AF/AL/S, STM32 MCUs and MPUs**, Rev. 21，§§3–4。  
https://www.st.com/resource/en/application_note/an2867-oscillator-design-guide-for-stm8afals-stm32-mcus-and-mpus-stmicroelectronics.pdf

<a id="s05"></a>**S05 — Microchip, CryptoAuthentication ATECC608C Summary Data Sheet**, DS40002513，Figure 1 与 Table 1。  
https://ww1.microchip.com/downloads/aemDocuments/documents/SCBU/ProductDocuments/DataSheets/ATECC608C-CryptoAuthentication-Summary-Data-Sheet-DS40002513.pdf

<a id="s06"></a>**S06 — Microchip, DT100104 Schematic**, Rev. 5，2024-05-06，sheet 1。  
https://ww1.microchip.com/downloads/aemDocuments/documents/SCBU/ProductDocuments/BoardDesignFiles/DT100104-Schematic.PDF

<a id="s07"></a>**S07 — Microchip, LAN8742A/LAN8742Ai Datasheet**, DS00001989A，§3.9，Figures 3-20、3-21、3-23。  
https://www.microchip.com/content/dam/mchp/documents/OTH/ProductDocuments/DataSheets/DS_LAN8742_00001989A.pdf

<a id="s08"></a>**S08 — Bourns, SM91071AL 10/100 Base-T Transformer Datasheet**。  
https://www.bourns.com/docs/product-datasheets/sm91071al.pdf

<a id="s09"></a>**S09 — Cinch Connectivity Solutions, SS-12400-004 product page and drawings**。  
https://www.cinch.com/products/ethernet-usb/connectors/m-series-connectors/ss-12400-004

<a id="s10"></a>**S10 — STMicroelectronics, USBLC6-4 Datasheet**, application diagrams 与 layout recommendations。  
https://www.st.com/resource/en/datasheet/usblc6-4.pdf

<a id="s11"></a>**S11 — Texas Instruments, SN65HVD230/SN65HVD231/SN65HVD232 Datasheet**, Rev. O，Figures 38–39，p.27。  
https://www.ti.com/lit/ds/symlink/sn65hvd230.pdf

<a id="s12"></a>**S12 — onsemi, NUP2105L Dual Line CAN Bus Protector Datasheet**, High-Speed/Fault-Tolerant CAN Surge Protection application figure。  
https://www.onsemi.com/download/data-sheet/pdf/nup2105l-d.pdf

<a id="s13"></a>**S13 — Texas Instruments, TPIC6A595 Power Logic 8-Bit Shift Register Datasheet**, Rev. C，§7.3.3、§7.3.4，p.13。  
https://www.ti.com/lit/ds/symlink/tpic6a595.pdf

<a id="s14"></a>**S14 — Texas Instruments, SN74AHCT541 Octal Buffer/Driver Datasheet**, function table 与 application information。  
https://www.ti.com/lit/ds/symlink/sn74ahct541.pdf

<a id="s15"></a>**S15 — Diodes Incorporated, 2N7002KQ Datasheet**, DS43528，Description and Applications。  
https://www.diodes.com/datasheet/download/2N7002KQ.pdf

<a id="s16"></a>**S16 — Hongfa, HFV6 Automotive Relay product page/data sheet**, 1 Form A、1 Form C 与额定参数。  
https://www.hongfa.com/Product/automotive-relay/HFV6

<a id="s17"></a>**S17 — Texas Instruments, SLTA055: Input and Output Capacitor Selection**, bulk-capacitance 与 ESR 讨论。  
https://www.ti.com/lit/pdf/slta055

<a id="s18"></a>**S18 — Texas Instruments, UCC27517A-Q1 Single-Channel Low-Side Gate Driver Datasheet**, Typical Application。  
https://www.ti.com/lit/ds/symlink/ucc27517a-q1.pdf

<a id="s19"></a>**S19 — onsemi, NVD5C684NL 60 V N-Channel MOSFET Datasheet**, Rev. 2，2024-07。  
https://www.onsemi.com/download/data-sheet/pdf/nvd5c684nl-d.pdf

<a id="s20"></a>**S20 — STMicroelectronics, STPS5L60-Y Automotive Power Schottky Rectifier Datasheet**, Doc ID 022951 Rev. 1。  
https://www.st.com/resource/en/datasheet/stps5l60-y.pdf

<a id="s21"></a>**S21 — Texas Instruments, INA240 Datasheet**, Rev. C，§9.2.2、Figure 9-5。  
https://www.ti.com/lit/ds/symlink/ina240.pdf

<a id="s22"></a>**S22 — TE Connectivity, TLRP3A30DR050FTE product page**, 0.05 Ω、3 W、1% current-sense resistor；关联文档 ENG_DS_1773449-4_D3。  
https://www.te.com/en/product-6-2176422-7.html

<a id="s23"></a>**S23 — STMicroelectronics, AN2834: How to optimize the ADC accuracy in the STM32 MCUs**, Rev. 10，2024-10，ADC source impedance、RC 与硬件建议。  
https://www.st.com/resource/en/application_note/an2834-how-to-optimize-the-adc-accuracy-in-the-stm32-mcus-stmicroelectronics.pdf

<a id="s24"></a>**S24 — Diodes Incorporated, BAT54S Datasheet**, DS11005，dual Schottky series configuration。  
https://www.diodes.com/assets/Datasheets/ds11005.pdf

<a id="s25"></a>**S25 — Micro Commercial Components, ESD3V3D5/ESD5V0D5/ESD7V0D5/ESD12VD5 Datasheet**, Rev. 3，2019-01。该来源仅用于同型号公开参数参考。  
https://www.mouser.com/datasheet/2/258/ESD3V3D5_7eESD12VD5%28SOD-523%29-V1-1626101.pdf

<a id="s26"></a>**S26 — Texas Instruments, SBAA338A: Monitoring NTC Thermistor Circuit With Single-Ended ADC**, Rev. A，2022-06。  
https://www.ti.com/lit/an/sbaa338a/sbaa338a.pdf

<a id="s27"></a>**S27 — Broadcom, ACPL-214/ACPL-217 Datasheet**, AV02-0469EN，Figure 1 与 electrical characteristics。  
https://docs.broadcom.com/doc/AV02-0469EN

<a id="s28"></a>**S28 — Nexperia, PESD5V0S1BA-Q Datasheet**。  
https://assets.nexperia.com/documents/data-sheet/PESD5V0S1BA-Q.pdf

<a id="s29"></a>**S29 — Texas Instruments, SN74LVC1G14 Datasheet**, Rev. AA，§6.3.2 Schmitt-Trigger Inputs。  
https://www.ti.com/lit/ds/symlink/sn74lvc1g14.pdf

<a id="s30"></a>**S30 — STMicroelectronics, SM6T Transil Series Datasheet**。该文档用于 SMB 封装输入 TVS 的通用选型/保护依据，不代表工程中 `D8` 的精确订货型号。  
https://www.st.com/resource/en/datasheet/sm6t.pdf

<a id="s31"></a>**S31 — Texas Instruments, SLLA420C: How To Simplify Isolated 24-V PLC Digital Input Module Designs**，traditional optocoupler input comparison。  
https://www.ti.com/document-viewer/lit/html/SLLA420C

<a id="s32"></a>**S32 — Bourns, SMCJ-Q Transient Voltage Suppressor Diode Series Datasheet**，SMCJ24CA-Q electrical characteristics。  
https://www.bourns.com/docs/product-datasheets/smcj-q.pdf

<a id="s33"></a>**S33 — Texas Instruments, LM74700-Q1 Datasheet**, Rev. G，Typical Application Schematic、Figure 3。  
https://www.ti.com/lit/ds/symlink/lm74700-q1.pdf

<a id="s34"></a>**S34 — Infineon, BSC065N06LS5 Datasheet**, Rev. 2.1，2019-10-31。  
https://www.infineon.com/assets/row/public/documents/24/49/infineon-bsc065n06ls5-datasheet-en.pdf

<a id="s35"></a>**S35 — TE Connectivity, 1-776163-1 documents and drawings**, 35-position AMPSEAL header；drawing 776163、specification 114-16016。  
https://www.te.com/commerce/DocumentDelivery/DDEController?Action=selcritrslt&iso=usa&searchby=part&searchfor=1-776163-1

<a id="s36"></a>**S36 — TE Connectivity, 776087-1 product page**, 23-position AMPSEAL header；drawing 776087、specification 114-16016。  
https://www.te.com/en/product-776087-1.html

## 维护规则

1. 新增或拆分电路时，先在对应 PAGE 表中增加 ID、覆盖位号、来源定位和证据等级，再加入来源条目。
2. 若 BOM 明确了当前不唯一的制造商/订货号，应将 C 级器件来源替换为该制造商的官方数据手册。
3. 若实际设计文件与公开典型应用存在差异，应在“对应关系与差异”中记录，不应为提高等级而省略差异。
4. README 不得写入原始网络名、原始页名、接口业务名称、线束定义或产品身份；这些信息应留在受控设计文档中。
