# ECU 烧录与台架验收

## 1. 安全前置条件

每次主动测试前逐项确认：

1. ECU 和车辆可靠固定，供电熔断/限流有效，实体急停可立即触及。
2. 车辆已支撑或行驶、启动、泵、振动、驻车等执行器已从危险能量链卸载；
   K12 失电确认为安全态。
3. 前进/后退阀允许在 200 mA 低目标下短时动作，两个方向切换不会造成机械运动。
4. 主机 UI、自动测试脚本和其他控制器不能同时占用控制权。
5. 只具备万用表/电流表时，不把 PWM 频率、开关瞬态、过冲和 EMC 判为已验证；
   这些项目必须在后续示波器和故障注入试验中补齐。

控制值归零仍保持原车线路接管；只有发送 RELEASE、关闭 UI 或等待控制超时，
K1/K2/K5/K7/K9/K13/K12 等接管继电器才会释放。

## 2. 构建、审计与烧录

```sh
./tools/cubemx_cli.sh validate
./tools/audit_config.sh
./tools/test.sh
./tools/build.sh Debug
./tools/build.sh Release
./tools/program_ecu.sh inspect
./tools/program_ecu.sh flash
```

首次启用 TrustZone 才使用：

```sh
./tools/program_ecu.sh provision-and-flash --accept-existing-flash-erase
```

该操作会先核对 STM32H563 ID `0x484`、Open 产品状态并备份完整 2 MiB Flash。
正常 `flash` 要求既有 Option Bytes 已满足：

```text
TZEN=0xB4
SECBOOTADD=0xC0000, SECBOOT_LOCK=0xC3
SECWM1_STRT=0x00, SECWM1_END=0x7F
SECWM2_STRT=0x01, SECWM2_END=0x00
```

Secure 镜像位于 Bank1，NonSecure 位于 Bank2。Bank1 sectors 125/126
`0x080FA000..0x080FDFFF` 是阀参数日志，镜像 linker 不使用这 16 KiB；烧录脚本
在普通升级前后读回比较该区域，若 Programmer 意外改动会报错并保留升级前备份。
NSC 使用最后一个 sector `0x080FE000..0x080FFFFF`。

## 3. 本机 Ethernet

```sh
sudo ./tools/configure_ecu_network.sh
ip -brief address show enp2s0
ip route get 172.16.0.11
ping -c 3 172.16.0.11
./tools/check_ecu_latency.sh
```

预期主机 `172.16.0.10/16`、ECU `172.16.0.11/16`，路由直接从 enp2s0 发出。
脚本创建 NetworkManager 连接 `ecu-bench`，无 gateway/DNS/default route，并用
priority 100 路由规则避免 VPN 抢占 ECU 流量。恢复原连接：

延迟门槛默认是 100 包无丢包、平均 RTT≤1.0 ms、最大 RTT≤5.0 ms。固件使用
异步 Ethernet TX，状态/诊断不能阻塞 RX；1 kHz 阀样本按 8 ms 批量发送。空闲、
持续控制和遥测开启三种状态都必须分别通过，不能只测无业务空闲状态。

```sh
sudo nmcli connection down ecu-bench
sudo nmcli connection up 'Wired connection 1'
```

## 4. 自动 V2 功能验收

确认第 1 节全部成立、退出 UI 后执行：

```sh
./tools/ecu_bench_test.py --ecu-ip 172.16.0.11 --accept-unloaded-actuation
```

如果已经确认两个阀线圈均未接入，可使用下列显式降级模式继续验证继电器、目标
斜坡、PWM 互斥和换向状态机；该模式不会把电流闭环标记为通过：

```sh
./tools/ecu_bench_test.py --ecu-ip 172.16.0.11 \
  --accept-unloaded-actuation --accept-disconnected-valves
```

脚本通过 V2 UDP 50002 控制并从 50001/50003 回读，覆盖：

- 100 B 状态、86 B 诊断及 CRC-32C；
- 中性清故障、Secure ARM、K12 行车许可和固定接管 mask；
- K21/K17、K22/K23、K15/K16、K26/K27、K4/K6、K8/K10；
- K11 驻车、K13/K14 龟兔档、K18/K19；
- 无硬件兼容字段保持 no-op；
- 前进 200 mA 电流闭环、斜坡和 PWM 互斥；
- 从前进安全换向到后退 200 mA，另一方向 PWM 必须为零；
- K20 主动释放和 Secure 3 s 强制释放；
- K3 临时接管，K24(BAT/高) 或 K25(GND/低) 闭合 1 s，再恢复原车；
- `emergency_stop_request` 和 priority 255 紧急关断；
- sender 3 对 sender 1 的仲裁、超时回退、250 ms 后 IDLE/全继电器释放。

脚本失败或退出时主动发送 RELEASE；Secure 300 ms 超时是独立最终关断。诊断
mask 只证明软件要求的 TPIC 位，不证明触点、电磁阀或线束实际动作。

## 5. UI 电流调试与 DMM 验收

阀线圈位于 J6，当前原理图/PCB 的接线如下。必须断电接线，负端是 ECU 低边输出，
不能短接至 BAT 或外部 GND：

| 阀 | 线圈正端 | 线圈负端/ECU 低边 |
|---|---|---|
| 前进 `YV_FWD` | J6-26（+12 V） | J6-28 `YV_FWD_COIL_N` |
| 后退 `YV_BACK` | J6-27（+12 V） | J6-29 `YV_BACK_COIL_N` |

```sh
./tools/ecu_debug_ui.py --ecu-ip 172.16.0.11
```

打开 <http://127.0.0.1:8088>。推荐步骤：

1. 确认状态年龄 <300 ms，READY/ADC 有效，CALIBRATING 已消失，阀目标、反馈、
   占空比均为零；如有可清故障，保持中性后点击清除。
2. 在“电流 PI 调参”读取 ECU 参数。首次没有有效日志时应显示默认参数来源。
3. 启用控制，依次给前进 `+100、+200、+300 mA`，每档至少保持 2 s；用串联
   标准电流表测量线圈电流，并与 UI 前进反馈比较。回零，确认反馈 <50 mA。
4. 依次给后退 `-100、-200、-300 mA` 并重复测量。不要直接从较大正值跳到较大
   负值；换向试验先用 ±200 mA，观察施加目标先降零且双 PWM 不重叠。
5. 每点记录：目标、稳态 DMM、ECU 反馈、占空比、供电电压、线圈温度。初始工程
   验收建议 `|ECU-DMM| ≤ max(50 mA, DMM读数×10%)`；生产指标必须由标定规范收紧。
6. PI 调节先限制最大占空比并从低电流开始。无示波器时只能依据 1 kHz 曲线评价
   毫秒级响应，不能排除 ADC/PWM 周期内振荡。优先小步调整 Kp，再调整 Ki 消除
   稳态误差；出现持续摆动立即目标归零并恢复已知参数。
7. “实时应用”只写 RAM。确认参数后先目标归零、等待反馈 <50 mA、点击释放控制，
   再“保存到 Flash”。保存成功后记录 active revision、generation 和 CRC。
8. 断电至少 10 s 后重上电，重新读取，参数和 generation 必须保持；然后再进行
   100 mA 低能量复验。这一步才构成掉电保存验收，ST-Link reset 不能替代断电。
9. 结束时目标归零、释放控制，确认 IDLE、requested/applied mask=0、两个 duty=0。

UI 波形包含请求目标、斜坡目标、带符号反馈、两路占空比，支持 CSV 导出。原始
文件应随测试编号、固件 commit、板号和仪表信息归档。

## 6. 参数存储专项测试

| 测试 | 操作 | 通过标准 |
|---|---|---|
| RAM-01 | 修改一个 Kp 后 APPLY，不 SAVE，复位 | 参数恢复为 Flash 值 |
| NVM-01 | APPLY 后 RELEASE/SAVE，断电重启 | 新参数和 generation 保持，CRC 有效 |
| NVM-02 | 控制有效或电流未归零时 SAVE | 返回 BUSY，不执行擦写 |
| NVM-03 | 10 s 内重复写不同参数 | 第二次返回 RATE_LIMITED |
| NVM-04 | 连续写满单扇区并跨扇区 | 最新 generation 有效，上一扇区掉电备份仍可扫描 |
| NVM-05 | 在记录提交前掉电（开发样件） | 未 commit 记录被忽略，恢复上一个有效记录 |
| FW-01 | 保存参数后执行普通 `flash` | 烧录前后参数区字节一致，重启仍加载原参数 |

NVM-04/05 会反复擦写或故意断电，只能在开发样件和受控供电夹具上执行，不能在
车辆运行状态进行。

## 7. 仍需仪器或外部信号的验证

| 编号 | 项目 | 通过标准 |
|---|---|---|
| HW-01 | 实体急停 | 输出立即全关、故障锁存、恢复后不自动 ARM |
| HW-02 | TPIC 位序/触点 | 每 bit 与 K1..K27 一致，保留位不动作，失电回到安全触点 |
| HW-03 | K20 启动 | 示波器确认连续高电平不超过 3 s |
| HW-04 | K3/K24/K25 | K3 先 50 ms；K24 或 K25 1 s 且互斥；源先释放，50 ms 后 K3 恢复 |
| HW-05 | PWM/ADC 同步 | 20 kHz 中心对齐；ADC 在导通脉冲中心采样；换向无重叠脉冲 |
| HW-06 | 阀故障注入 | 过流、开路、反馈轨故障、非活动电流均锁存并全局关断 |
| HW-07 | 速度 PA0 | 20000 pulse/km 换算正确，1500 ms 后信号丢失 |
| HW-08 | 慢 ADC | 各通道顺序、电压换算、开短路行为符合传感器规范 |
| HW-09 | CAN1/J1939 | 250 kbit/s 六 PGN 解析与旧车一致，1000 ms 过期，其他 ID 拒绝 |
| HW-10 | CAN2 | 保持预留，不启动、不发送、不接受车辆控制 |
| HW-11 | PB8/PB9 软件 I²C | PB9=SCL、PB8=SDA；9 clock 恢复；当前 R1 无飞线 |
| HW-12 | IWDG/CSS | NonSecure 停止或 HSE 故障时先关输出，约 2 s 复位 |
| HW-13 | 电源/EMC/热/耐久 | 无非预期吸合，等级符合整机风险分析和目标标准 |

该固件的故障安全设计不能替代独立硬件安全链，也不构成 IEC 61508、ISO 13849
或整车功能安全认证。

## 8. 2026-08-27 当前样件实测记录

测试对象为 STM32H563（Device ID `0x484`），ST-Link 序列号
`066BFF565456857187210935`，调试固件从本工作区构建并烧录。结果如下：

| 项目 | 实测结果 | 结论 |
|---|---|---|
| CubeMX/代码 | CubeMX 6.18 隔离目录实际生成后，配置审计、7 项协议测试及 Debug 构建通过；最终 Debug/Release 构建通过 | PASS |
| 普通固件升级 | Secure/NonSecure 下载及 verify 成功；`0x080FA000..0x080FDFFF` 升级前后逐字节一致 | PASS |
| Ethernet 空闲 | 100 包，0% 丢包，RTT min/avg/max=`0.071/0.122/0.329 ms` | PASS |
| Ethernet + 1 kHz 遥测 | 100 包，0% 丢包，RTT min/avg/max=`0.086/0.126/0.265 ms`，会话丢样=0 | PASS |
| 完整动作后 Ethernet | 100 包，0% 丢包，RTT min/avg/max=`0.082/0.128/0.312 ms` | PASS |
| 参数 RAM/Flash | 默认参数 APPLY 成功；SAVE generation=1、CRC32C=`0xACD6FCBE`；ST-Link 硬复位及断电至少 10 s 后均完整读回 | PASS |
| V2/继电器/安全逻辑 | 自动脚本 29 项通过；结束后 IDLE、relay mask=0、两路 duty=0、故障=0 | PASS |
| 前/后阀无负载输出 | ±200 mA 目标斜坡、占空比约 596‰、PWM 互斥及安全换向通过 | PASS |
| 前/后阀真实电流闭环 | 当前反馈仅 0–1 mA，确认台架未形成线圈电流回路 | BLOCKED：接入线圈后执行第 5 节 |

本记录没有把诊断 relay mask 当作触点电气验证。10 s 真实掉电后的参数保持已经
通过；连接阀线圈并串联电流表完成电流闭环标定后，才能关闭剩余的阀带载验收。
