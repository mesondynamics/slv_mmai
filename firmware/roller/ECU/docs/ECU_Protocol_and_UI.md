# ECU V2/A5 兼容通信协议、继电器映射与调试 UI

## 1. 网络参数

| 项目 | 配置 |
|---|---|
| 设备身份 | `SN-EJAHGJI`，旧版算法解算为 roller / device ID 1 |
| ECU/controller | `172.16.0.11/16`，MAC `8A:EA:B5:00:00:02` |
| 遥控器 | 固定 `172.16.0.9/16`，允许普通控制 |
| 调试/售后主机 | 固定 `172.16.0.10/16`，允许普通控制、调参和 OTA |
| 域控/IPC | 本设备解算地址 `172.16.0.12/16`，允许普通控制 |
| 状态 | UDP 50001，ECU 广播，50 ms |
| 控制 | UDP 50002，普通命令仅接受 `.9/.10/.12`，推荐 20 Hz |
| 诊断 | UDP 50003，ECU 广播，100 ms |
| 阀遥测 | UDP 50004，订阅后 ECU 单播，1 kHz 样本、批量发送 |
| 参数服务 | UDP 50005，仅接受 `172.16.0.10` 的 GET/APPLY/SAVE/RELOAD 与遥测订阅 |
| 固件升级 | UDP 50006，仅接受来自 `172.16.0.10` 的签名 OTA |

UDP 50002 同时识别 V2 与上一版 A5 V1 控制帧；其他命令服务只接受 V2。
V1 只开放可无歧义映射到当前安全域的继电器子集。行驶量已由百分比改成有物理
单位的 `valve_current_target_ma`，当前 PCB 又没有 ECU 可验证的转角反馈，因此
非零旧油门或旧转向请求都会拒绝整帧，绝不做隐式换算。

非紧急 V1/V2 控制只接受遥控器 `172.16.0.9`、固定调试机 `172.16.0.10` 和本设备
域控 `172.16.0.12`；调参、遥测订阅、工厂服务及 OTA 仍只接受 `.10`。同一
`sender_id` 的活动会话还绑定首次接受的源地址，所以三个控制端必须使用不同
`sender_id`，其他源不能续租、RELEASE 或清故障。结构和校验完整的网络急停仍在
源地址、sequence 和其他
执行字段语义之前主导，以免访问控制阻止安全停车。这个例外也意味着不可信网络
上的攻击者可造成拒绝服务，因此物理网络隔离仍是强制要求。

控制、状态、诊断和调参协议用于隔离的点对点车辆网络，没有密码学鉴权或跨重启
防重放能力；CRC/XOR 只能发现传输损坏。源 IP 白名单可被同一二层网络上的主机
伪造，属于纵深防护而不是身份认证。OTA 是独立例外：包有 ECDSA P-256 传输签名，
内部镜像另由 OEMiROT 根密钥认证并加密存放。不得把任何 UDP 端口直接暴露到
不可信网络；跨网段使用时必须由受控网关提供访问控制、身份认证和加密。
ECU 授权不绑定 UDP 源端口，因此每个白名单 IP 上的全部进程共享该主机的控制
信任边界；同一主机上的其他进程可用更新的 sequence 接管相同 sender。调试 UI
对转向状态额外
校验源端口，其余接收路径只把源 IP 作为纵深检查，这些都不能替代隔离网络。

### 1.1 上一版 A5 V1 安全兼容层

旧控制帧线格式保持不变：`A5 | length_hi | length_lo | payload | XOR | 5A`。
长度为大端，39 B packed payload 内的多字节字段为小端，XOR 只覆盖 payload，
完整控制帧固定 44 B。payload 前 8 B 依次为 `sender_id:u8`、`priority:u8`、
`flags:u16`、`sequence:u32`，其后是原 31 B `control_payload_t`。旧命令没有
timestamp；不能从不存在的字段推导时间。接收时用本机单调时钟建立 250 ms
watchdog，会话存活期间要求 sequence 严格递增并支持 uint32 自然回绕；超时后
才允许客户端从新序号建立会话。V1 与 V2 使用同一套发送方槽、优先级、实体急停、
网络紧急、输出失联释放和 Secure 300 ms 二级超时，不提供 V1 版
`CLEAR_FAULT` 或 `RELEASE`，普通 V1 帧的 flags 必须为 0。

结构、固定长度、结束字节和 XOR 均正确的 V1 帧中，只要
`emergency_stop_on != 0` 或 `priority=255`，就会在其他字段语义校验前进入紧急
权威并把命令内容清零；陈旧 sequence、未知 flags 或同帧运动字段不能阻止急停。
非紧急帧采用以下策略：

- 泵、前后主灯/LED、倒车蜂鸣器/喇叭、震动接管及级别/前后选择、发动机启动、
  龟兔档、发动机高低转速触发、驻车、左右转向灯按当前 V2 安全语义映射；所有
  bool 必须严格为 0 或 1，震动强/弱不能同时请求，发动机转速级别限于 -3..3。
- `steering_target_tdeg`、`steering_speed_tdeg_per_s`、`steering_enable` 任一非零
  都拒绝整帧。旧版闭环依赖外部转角电位器，当前 PCB 没有该反馈，禁止把目标角
  或限速猜测为 CAN2 开环角速度。
- `throttle_percent` 非零拒绝整帧；它既不会换算成 mA，也不会刷新控制 watchdog。
- `indicator1..3`、`power_latch_on`、`main_power_relay_on` 在当前 PCB 没有对应
  执行器，非零时拒绝整帧，避免客户端误判为已动作。

ECU 也可发出上一版固定 77 B 状态 payload（完整 A5 帧 82 B），其中
`sequence_id` 和 `timestamp_ms` 精确沿用当前状态快照，旧 `throttle_percent`
固定为 0；V1 没有阀目标/反馈/占空比、转向 CAN 状态或安全诊断的表达空间，不在
冻结结构后追加私有尾部。为保持无需先发控制命令的旧客户端发现和被动监视能力，
V1 状态在 UDP 50001 无条件保持上一版 50 ms 周期。周期发送使用绝对截止时间，
初始化和每次进入 operational 状态时按 V2/转向/V1/诊断/安全状态的
0/10/25/35/60 ms 相位启动；V2 首帧立即发送。主循环偶发停顿不会把后续周期
重新对齐到同一时刻，过旧帧会丢弃，并且同一 HAL 毫秒最多提交一类周期应用帧。
该约束不包括接收回调产生的 ICMP、参数或 OTA 应答。额外应用负载为
1640 B/s（13.12 kbit/s 应用数据，计入 Ethernet/IP/UDP 开销仍低于约
21 kbit/s），不会开启 PI 高速遥测。V1 与 V2 状态共享 50001，接收端必须先按
首字节 `A5` 或 `ECU2` 分流。

上一版没有诊断帧 ABI，因此不伪造 A5 诊断。V2 diagnostic capability bit10 表示
此安全兼容层存在；`valid_control_frames` 是 V1+V2 接受总数，冻结的
`legacy_v1_frames_rejected` 单独累计 A5 结构、语义、资源或 replay 拒绝。
固件内部另分开统计 `legacy_v1_frames_accepted` 与 A5 状态发送数，避免把兼容接收
误记成拒绝。

## 2. V2 帧

所有多字节字段均为小端，结构按 1 字节打包。CRC 为 CRC-32C/Castagnoli，参数
为 reflected polynomial `0x82F63B78`、初值和末值异或均为 `0xFFFFFFFF`；计算时
把头部 `crc32c` 四字节置零，并覆盖完整头和 payload。

| 偏移 | 类型 | 字段 | 要求 |
|---:|---|---|---|
| 0 | uint32 | magic | `0x32554345`，线上字节为 `ECU2` |
| 4 | uint8 | version | 2 |
| 5 | uint8 | message_type | 见下表 |
| 6 | uint16 | header_size | 24 |
| 8 | uint16 | payload_size | 实际业务长度 |
| 10 | uint16 | flags | 消息定义的标志 |
| 12 | uint32 | sequence | 发送方单调递增，允许自然回绕 |
| 16 | uint32 | timestamp_ms | 发送方单调时钟低 32 位 |
| 20 | uint32 | crc32c | 完整帧 CRC-32C |

| type | 消息 | payload |
|---:|---|---:|
| 0x01 | CONTROL_COMMAND | 36 B |
| 0x02 | STATUS | 100 B |
| 0x03 | DIAGNOSTIC | 86 B |
| 0x04 | VALVE_TELEMETRY | 12 + N×20 B，N≤16 |
| 0x05 | SECURITY_STATUS | 56 B |
| 0x06 | STEERING_STATUS | 52 B |
| 0x10 | VALVE_CONFIG_GET | 0 B |
| 0x11 | VALVE_CONFIG_APPLY | 48 B |
| 0x12 | VALVE_CONFIG_SAVE | 0 B |
| 0x13 | VALVE_CONFIG_RELOAD | 0 B |
| 0x14 | VALVE_CONFIG_REPLY | 72 B |
| 0x15 | TELEMETRY_SUBSCRIBE | 8 B |
| 0x16 | OPERATION_ACK | 8 B |
| 0x17 | TELEMETRY_UNSUBSCRIBE | 0 B |
| 0x40 | OTA_STATUS | 请求 0 B，回复 44 B |
| 0x41 | OTA_BEGIN | 192 B manifest+signature |
| 0x42 | OTA_CHUNK | 532 B，最多 512 B 数据 |
| 0x43 | OTA_FINISH | 4 B update_sequence |

ABI 定义在 `NonSecure/App/Inc/ecu_protocol.h`。固件用 `_Static_assert` 锁定全部
线上结构尺寸，主机端测试也独立校验同一组尺寸和 CRC 已知向量。

## 3. 控制命令与控制权

36 B 控制 payload 的前 4 B 是仲裁头：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | uint8 | sender_id；1=远程、2=操作员、3=自主 |
| 1 | uint8 | priority；1..254，数值越大越优先；255=网络紧急 |
| 2 | uint16 | reserved，必须为 0 |

其后是 32 B 高层控制结构：

| 偏移 | 类型 | 字段/动作 |
|---:|---|---|
| 4/5 | bool | pump_enable / pump_select；K26 或 K27 |
| 6/7 | bool | front/rear headlamp；K22/K23 |
| 8/9 | bool | front/rear LED；K15/K16 |
| 10/11 | bool | reverse/main buzzer；K17/K21 |
| 12..14 | bool | indicator1..3；保留且无硬件动作 |
| 15 | bool | vib_original_on；兼容字段，接管自动完成 |
| 16/17 | bool | vib_strong/weak，互斥；K4/K6 |
| 18/19 | bool | vib_front/rear_selected；K8/K10 |
| 20 | bool | engine_start_request；K20，最长 3 s |
| 21 | bool | power_latch_on；保留且无硬件动作 |
| 22 | bool | speed_mode_high；K13 接管，K14 接 GND=兔子 |
| 23 | int8 | engine_speed_level，-3..3；K25 低/K24 高触发次数 |
| 24 | int16 | `flags.bit2=1` 时为 **steering_velocity_tdeg_per_s**，-6000..6000（0.1°/s） |
| 26 | uint16 | 旧 ABI 保留字段；转向速率模式必须为 0 |
| 28 | bool | steering_enable；仅速率模式有效 |
| 29 | int16 | **valve_current_target_ma，-2000..2000 mA** |
| 31 | bool | parking_brake_on；K11 |
| 32 | bool | emergency_stop_request；立即提升为紧急控制 |
| 33 | bool | main_power_relay_on；当前板无动作 |
| 34/35 | bool | right/left turn；K19/K18 |

`flags.bit0=CLEAR_FAULT` 只允许配合全中性 payload；`flags.bit1=RELEASE` 只允许
全中性 payload，并立即结束该 sender 的控制会话。`flags.bit2=STEERING_RATE`
把偏移 24 的原有两字节重解释为带符号转向角速度；未设置时保持
1.0.16 及更旧固件的无转向动作语义。除 bit0..2 外的标志拒绝。

控制权优先级为：实体急停 > 网络紧急 255 > 普通发送方显式 priority 1..254 >
IDLE；数值越大越优先，相同 priority 时较小 `sender_id` 胜出。priority=0 仅为
旧客户端兼容：sender 1/2/3 分别回退为 1/2/3，其他 sender 回退为 1；新程序不得
发送 0。最多保存 6 个发送方；租约年龄 250 ms 仍有效，251 ms 起失效并
允许新序号建立新 session。过期是不可跳过的安全事件：即使同一主循环立即复用
相同 slot，仍先 IDLE/Disarm 完整 20 ms；该 sender 只有提交全中性、转向禁用帧且
Secure 已实际接受后才解除 rearm，251..299 ms 周期包不能夹在 300 ms Secure
watchdog 之前恢复非零输出。普通控制还必须来自 `.9/.10/.12`，活动发送方按源
地址绑定。无有效控制源
时全部继电器和两个 PWM 释放；Secure 域另有 300 ms 命令
超时，NonSecure 卡死也不能保持输出。

正电流目标只驱动前进阀，负目标只驱动后退阀，`|target|<50 mA` 归零。方向切换
必须先按下降斜率降到零，检测活动电流低于 50 mA 持续 20 ms，再等待 5 ms
死区后才允许另一方向。两路 PWM 在 Secure 域硬互斥。

### 3.1 CAN2 转向速率控制

本版转向是开环速率执行接口，不是 ECU 内的转角闭环。正数表示车辆
定义的正向，单位为 0.1°/s，协议兼容范围为 `-6000..6000`。电机对象
`0x2000` 的值不是 RPM，而是参数 0002“额定转速”的千分比。本型号按厂商
参数表明确配置为 80 rpm，因此 Secure 域按
`command_permille = -velocity_tdeg_per_s × 1000 / (60 × 80)` 换算并限制为
`-1000..1000`；对应命令范围最大为 ±4800（±480.0°/s），超过部分饱和。
`applied_velocity_tdeg_per_s` 仅表示最近一次速度 SDO 成功 ACK 对应的**已确认命令值**，
不是物理角速度或转角反馈；不得用于上位转角闭环。若现场修改电机参数 0002，必须同步
修改、评审并重新发布此编译期标定，不能只改驱动器。转角闭环和角度限位必须由
上位控制器完成；当前 PCB 没有接入可供本 ECU 验证的转角反馈，不得将该接口
当作转角指令。

CAN2 使用 Classic CAN 250 kbit/s、扩展帧、电机节点 1。Secure 域发送
`0x06000001`：速度对象 index `0x2000`、禁用 `0x200C`、使能 `0x200D`，
command subindex 均为 1。根据电机厂商的[方向盘舵机手册](https://ae-pic-a1.aliexpress-media.com/kf/Sf252692b27884931b38077b0e5716cf1s.pdf)，
写成功应答的第 4 字节固定为 0：只有在等待当前使能事务时收到
`60 0D 20 00 00 00 00 00` 才确认电机使能；速度/禁用应答分别为
`60 00 20 00 ...` / `60 0C 20 00 ...`。延迟或与当前状态无关的应答
不能激活电机或刷新命令事务时间；`0x80` abort 会记录故障并安全停机。
`0x07000001` 心跳必须精确为 8 B，高字节在前；其 data2/3 是实际
电机速度，data4/5 是速度给定，data6/7 是控制器 DTC。经验证心跳可
刷新电机在线诊断和真实故障码，但不能代替使能/速度 SDO 事务应答。
任何非零速度在使能确认前都不会发出。

sender 1/2 是人工来源，sender 3 是独立的自主来源；三者在 Secure 边界一一映射，
自定义 sender 不能携带 rate flag、enable、目标或旧速度字段来别名任一 Secure
转向身份（结构正确的 emergency 仍优先安全停机）。sender 1/2 单次非零持续最长 2 s，超时后必须先发送
`速度=0 + disable` 才能重新使能。sender 3 为自主控制器，可连续运行，
但同时受 NonSecure 250 ms 控制会话和 Secure 300 ms 命令超时监控。
控制源切换也强制先执行零速度/禁用序列。CAN 错误、bus-off、应答超时或
SDO abort 只隔离转向通道；除非同时出现原有全局安全故障，不得阻断其他已审执行器。

`STEERING_STATUS` 以 10 Hz 发送，52 B payload 依次包含请求速度/已确认命令速度、
速度命令（额定转速千分比）、真实 CAN 接收年龄、SDO 故障码、心跳中的有符号
电机速度原始值、状态/故障位、TX/RX/错误/bus-off 计数、状态机、总线状态和
电机使能确认。厂商手册没有给心跳速度字段声明物理单位，接口故意命名为
`motor_speed_feedback_raw`，不得直接标成 RPM。上位机必须同时检查
报文年龄、CAN RX 年龄和 `motor_enable_confirmed`，不能用“已发 TX”代替真实确认。

### 3.2 CAN1/J1939 只读边界

受 STM32H563 FDCAN message-RAM 勘误约束，FDCAN1 与 FDCAN2 均在 Secure 域且
同为 `SEC|NPRIV`。CAN1 只接收上一版六个 PGN：EEC1 `0xF004`、EEC2
`0xF003`、ET1 `0xFEEE`、EFL_P1 `0xFEEF`、AMB `0xFEF5`、DM1 `0xFECA`；
priority 与 source address 可变化，reserved/data-page、standard、remote、FD、
非 8 B 和未知 PGN 均拒绝。超时语义与旧版一致：age=1000 ms 仍有效，1001 ms
失效，并支持 uint32 时钟回绕。

NonSecure 没有 FDCAN handle、IRQ、HAL 或 raw-frame/TX 接口，只通过固定 84 B、
版本/容量/对齐受检的 NSC snapshot 读取工程量。NSC 失败立即清除全部 validity，
不能继续发布旧值。CAN1 初始化失败或运行期 warning/passive/bus-off/error 会置位
`SAFETY_STATUS_VEHICLE_CAN_FAULT`；Error Active 健康轮询后自动清除。该故障是
只读遥测域诊断，不锁存全局 fault，不影响 Ethernet/OTA 与无关执行器。

## 4. 电流 PI、保护和参数

Secure 域以 ADC2/TIM4 同步中断执行 20 kHz 内环 PI。反馈按 VREFINT 补偿 VDDA，
上电先在 PWM=0 时采集 2048 组零点；校准完成前不能 ARM。每方向独立参数：

| 字段 | 默认 | 允许范围 | 单位 |
|---|---:|---:|---|
| kp_permille_per_amp | 400 | 0..2000 | ‰/A |
| ki_permille_per_amp_second | 4000 | 0..20000 | ‰/(A·s) |
| filter_cutoff_hz | 500 | 50..2000 | Hz |
| rise_slew_ma_per_s | 1000 | 50..10000 | mA/s |
| fall_slew_ma_per_s | 2000 | 50..10000 | mA/s |
| max_duty_permille | 600 | 50..950 | ‰ |
| current_gain_ppm | 1000000 | 900000..1100000 | ppm |
| current_offset_ma | 0 | -100..100 | mA |

PI 使用定点运算、条件积分抗饱和和应用新参数时的无扰切换。以下保护阈值是固件
硬限制，不可由网络修改：单点 2500 mA、2200 mA 持续 2 ms、非活动方向
150 mA 持续 10 ms、PWM 全关仍有 100 mA 持续 100 ms、目标≥300 mA 且达到
占空比上限但反馈<50 mA 持续 300 ms、ADC 贴上电源轨、换向超时 500 ms。
任一阀故障都会锁存全局故障，两个 PWM=0，所有继电器（含 K12）释放。仅在目标
为零、输出已释放且两路反馈<50 mA 时允许人工清故障。

参数结构为 `version:uint32 + size:uint32 + forward:20 B + reverse:20 B`，共
48 B。APPLY 实时修改 RAM 并设置 dirty，不写 Flash；SAVE 要求控制权已释放和
阀完全静止，且相邻写入至少 10 s。Flash 使用 Bank1 sector 125/126
(`0x080FA000..0x080FDFFF`) 双扇区追加日志、CRC-32C、generation 和最后写入的
commit quadword；启动扫描有效的最新记录，损坏/掉电未提交记录不会被采用。

## 5. 实时遥测与 UI

订阅 payload 为 `destination_port:uint16=50004`、`sample_rate_hz:uint16=1000`、
`ttl_ms:uint32=500..2000`。订阅必须来自 `172.16.0.10`，并与当前活动控制源 IP
相同；ECU 处于 IDLE 时仍不接受其他地址。UI 每秒续订一次。

遥测批头为 `first_sequence:uint32`、`dropped_samples:uint32`、
`sample_count:uint16`、`sample_period_us:uint16`。每个 20 B 样本依次是：
`dropped_samples` 是当前订阅会话内的 Secure 环形缓冲覆盖数；订阅建立前无人读取
造成的历史覆盖不计入该值，因而可直接作为当前调参会话的数据完整性判据。

```text
timestamp_us:u32, requested_target_ma:i16, applied_target_ma:i16,
forward_current_ma:u16, reverse_current_ma:u16,
forward_duty_permille:u16, reverse_duty_permille:u16,
state:u16, fault_flags:u16
```

启动 UI：

```sh
sudo ./tools/configure_ecu_network.sh
./tools/ecu_debug_ui.py --ecu-ip 172.16.0.11
```

浏览器打开 <http://127.0.0.1:8088>。“功能控制”页覆盖继电器、阀电流目标和
CAN2 转向速率，并可独立设置 `sender_id` 与 1..254 仲裁优先级；修改任一连接/
身份参数前，后端必须先释放当前控制。转向必须先收到新鲜的
`STEERING_STATUS`，再人工确认使能；
按住正/反向按钮才发送非零速度，松手、离开页面、窗口失焦或 2 s 到期都立即
发送零速度/禁用。UI 不允许使用保留给上位自主控制器的 sender 3 执行手动转向。
ARM 后 750 ms 内未收到属于本轮的新鲜严格使能 ACK，后端会自动失能；一旦确认，
即使当前速度为零，后续新鲜状态丢失 ACTIVE、command-enable 或 motor-enable
确认也会失能，不能长期驻留在未确认的电机使能请求中。
页面风险 `confirm`、loopback HTTP、CSRF token 和后端至少 16 字符的 arm token 只防
浏览器误操作/跨站请求，不构成用户身份或安全授权。同机进程及具备原始套接字权限的
程序仍处于 `172.16.0.10` 信任域，能绕过对话直接发送 sender 控制；量产主机必须
另行实施账户、进程和网络隔离，不得把 UI 对话框记录为功能安全放行证据。

“电流 PI 调参”页直接提供与“功能控制”页双向同步的 `-2000..2000 mA` 目标、
立即归零、零目标启用和归零释放入口，并显示 10 s 的请求量、斜坡目标、带符号
反馈和前/反占空比。每次进入可见调参页时，非零目标输入保持锁定，直到 UI 已经
收到本轮首批新鲜 1 kHz 样本；暂停波形或遥测年龄达到 500 ms 也会重新锁定目标
输入，但“立即归零”始终可用。调参页启用控制会先清除全部潜在目标，再从零目标
开始发送，避免重新启用时恢复旧动作。参数操作必须按“读取 → 实时应用 → 低电流
验证 → 释放控制 → 保存 Flash”的顺序。浏览器超过 1 s 无心跳时 Python 后端发送
RELEASE；页面关闭也会尝试发送 RELEASE，但 ECU 自身超时仍是最终安全边界。
每次进入可见调参页使用一次性 lease token；退出/隐藏/pagehide 后 token 作废，
迟到的续租不能复活会话。后端在没有活跃 lease 和 ECU subscription 时会在解析前
丢弃迟到的 1 kHz 数据，因此正常运行不承担高速波形处理开销。

## 6. Ethernet OTA

OTA 只在 UDP 50006 上处理，来源必须为 `172.16.0.10`。ECU 在 BEGIN 时先释放
全部输出，再验证固定 128 B manifest 的 P-256 签名与单调 `update_sequence`；
CHUNK 按 16 B Flash 编程粒度、CRC-32C 和连续 offset 写入两个 secondary slot；
FINISH 再核对成对镜像 SHA-256、加密标志和 MCUboot trailer。随后 OEMiROT 在
reset 后独立验证镜像签名、依赖版本和 `security_counter`，以 test swap 启动。
只有 ATECC 身份验证、Secure ADC 初始化和 NonSecure 通信初始化全部成功，应用
才写入 `image_ok` 确认；否则 watchdog reset 后回滚。

`update_sequence` 防止传输包重放，`security_counter` 防止已签名旧固件降级，两者
用途不同且发布时都必须单调递增。一次签名正确、序号更新但 security counter 过低
的包会被传输层接受并消耗该 sequence，之后应使用更大的 sequence 重新发布。完整
操作与生命周期约束见 `ECU_Security_and_OTA.md`。

主机命令：

```sh
python3 tools/ethernet_ota.py --status
python3 tools/ethernet_ota.py artifacts/firmware/<version>/roller-ecu-<version>.recu
```

UI“固件升级”页调用相同的本机校验和传输实现；升级过程中控制权会释放且输出保持
隔离。

## 7. 继电器位映射

| bit | 继电器 | 功能 |
|---:|---|---|
| 0/4 | K8/K7 | 前振档位/原车接管 |
| 1/9 | K2/K4 | 大振接管/档位 |
| 2 | K21 | 喇叭 |
| 3 | K1 | 振动使能接管 |
| 6/30 | K26/K27 | 水泵 1/2 |
| 7/15 | K15/K16 | 前/后 LED |
| 8/19 | K9/K10 | 后振接管/档位 |
| 14/29 | K23/K22 | 后/前主灯 |
| 16 | K3 | 发动机高低速触发时临时切断原车信号 |
| 17/18 | K6/K5 | 小振档位/接管 |
| 20 | K17 | 倒车蜂鸣 |
| 21 | K11 | 驻车 |
| 22 | K12 | 行车许可，失电安全 |
| 23/28 | K18/K19 | 左/右转 |
| 24/27 | K25/K24 | GND 低速/BAT 高速，闭合 1 s 后释放 |
| 25/26 | K14/K13 | 龟兔选择/原车接管 |
| 31 | K20 | 发动机启动 |

bit 5、10..13 未布线并强制为零。普通控制 ARM 后 K1/K2/K5/K7/K9/K13
自动接管，K12 行车许可吸合；K3 只在高低转速切换时接管。K24/K25 先释放，
50 ms 后 K3 恢复原车；K24/K25 始终硬互斥。
