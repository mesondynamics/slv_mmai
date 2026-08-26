# ECU V2 通信协议、继电器映射与调试 UI

## 1. 网络参数

| 项目 | 配置 |
|---|---|
| ECU | `172.16.0.11/16`，MAC `8A:EA:B5:00:00:02` |
| 台架主机 | `enp2s0`，`172.16.0.10/16` |
| 状态 | UDP 50001，ECU 广播，50 ms |
| 控制 | UDP 50002，主机单播到 ECU，推荐 20 Hz |
| 诊断 | UDP 50003，ECU 广播，100 ms |
| 阀遥测 | UDP 50004，订阅后 ECU 单播，1 kHz 样本、批量发送 |
| 参数服务 | UDP 50005，GET/APPLY/SAVE/RELOAD 与遥测订阅 |

所有端口只接受 V2 帧。V1 的 `0xA5...0x5A` 帧会被拒绝并计入
`legacy_v1_frames_rejected`。车辆功能字段和控制权规则沿用上一版语义，但行驶量
已由百分比改成有物理单位的 `valve_current_target_ma`，因此不能安全地继续复用
旧帧 ABI。

本协议用于隔离的车辆网络，没有加密、鉴权或跨重启防重放能力。不得把 UDP
端口直接暴露到不可信网络；跨网段使用时应由受控网关提供身份认证和加密。

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
| 0x10 | VALVE_CONFIG_GET | 0 B |
| 0x11 | VALVE_CONFIG_APPLY | 48 B |
| 0x12 | VALVE_CONFIG_SAVE | 0 B |
| 0x13 | VALVE_CONFIG_RELOAD | 0 B |
| 0x14 | VALVE_CONFIG_REPLY | 72 B |
| 0x15 | TELEMETRY_SUBSCRIBE | 8 B |
| 0x16 | OPERATION_ACK | 8 B |

ABI 定义在 `NonSecure/App/Inc/ecu_protocol.h`。固件用 `_Static_assert` 锁定全部
线上结构尺寸，主机端测试也独立校验同一组尺寸和 CRC 已知向量。

## 3. 控制命令与控制权

36 B 控制 payload 的前 4 B 是仲裁头：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | uint8 | sender_id；1=远程、2=操作员、3=自主 |
| 1 | uint8 | priority；1..254，255=网络紧急 |
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
| 24 | int16 | steering_target_tdeg，-300..300；当前板无动作 |
| 26 | uint16 | steering_speed_tdeg_per_s，0..6000；当前板无动作 |
| 28 | bool | steering_enable；当前板无动作 |
| 29 | int16 | **valve_current_target_ma，-2000..2000 mA** |
| 31 | bool | parking_brake_on；K11 |
| 32 | bool | emergency_stop_request；立即提升为紧急控制 |
| 33 | bool | main_power_relay_on；当前板无动作 |
| 34/35 | bool | right/left turn；K19/K18 |

`flags.bit0=CLEAR_FAULT` 只允许配合全中性 payload；`flags.bit1=RELEASE` 只允许
全中性 payload，并立即结束该 sender 的控制会话。其他标志拒绝。

控制权优先级为：实体急停 > 网络紧急 255 > 自定义优先级/自主 3/操作员 2/
远程 1 > IDLE。最多保存 6 个发送方；250 ms 未更新则会话失效并允许新序号重新
建立。无有效控制源时全部继电器和两个 PWM 释放；Secure 域另有 300 ms 命令
超时，NonSecure 卡死也不能保持输出。

正电流目标只驱动前进阀，负目标只驱动后退阀，`|target|<50 mA` 归零。方向切换
必须先按下降斜率降到零，检测活动电流低于 50 mA 持续 20 ms，再等待 5 ms
死区后才允许另一方向。两路 PWM 在 Secure 域硬互斥。

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
`ttl_ms:uint32=500..2000`。订阅源必须与当前控制源 IP 相同，或 ECU 处于 IDLE；
UI 每秒续订一次。

遥测批头为 `first_sequence:uint32`、`dropped_samples:uint32`、
`sample_count:uint16`、`sample_period_us:uint16`。每个 20 B 样本依次是：

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

浏览器打开 <http://127.0.0.1:8088>。“功能控制”页覆盖继电器和电流目标；
“电流 PI 调参”页显示 10 s 的请求量、斜坡目标、带符号反馈和前/反占空比，支持
暂停、清空和 CSV 导出。参数操作必须按“读取 → 实时应用 → 低电流验证 → 释放
控制 → 保存 Flash”的顺序。浏览器超过 1 s 无心跳时 Python 后端发送 RELEASE；
页面关闭也会尝试发送 RELEASE，但 ECU 自身超时仍是最终安全边界。

## 6. 继电器位映射

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
