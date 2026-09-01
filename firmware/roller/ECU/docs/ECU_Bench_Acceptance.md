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
6. 转向主动测试前，车轮/铰接必须离地或与危险机械能量脱离，行程两端
   留有充足余量，人员离开夹挤区，并可立即切断电机供电。断电测量 J7-13
   `CAN2_H` 到 J7-16 `CAN2_L` 的总终端约为 60 Ω，然后才允许非零速度。

控制值归零仍保持原车线路接管；只有发送 RELEASE、关闭 UI 或等待控制超时，
K1/K2/K5/K7/K9/K13/K12 等接管继电器才会释放。

## 2. 构建、审计与烧录

```sh
./tools/cubemx_cli.sh validate
./tools/audit_config.sh
./tools/test.sh
./tools/build.sh Debug
./tools/build.sh Release
./tools/build_oemirot.sh ReleaseOpen
./tools/build_oemirot.sh ReleaseClosed
./tools/build_signed_apps.sh Release
STM32_PROGRAMMER_CLI=/home/plac/.local/share/stm32cube/bundles/programmer/2.23.0/bin/STM32_Programmer_CLI \
  python3 tools/ecu_debug_auth.py --accept-closed-target discover
```

当前样件已从旧的直接 TrustZone 布局迁移为 OEMiROT，不能再用
`tools/program_ecu.sh flash` 覆盖 primary slot。日常升级使用 Ethernet：

```sh
python3 tools/ethernet_ota.py --status
python3 tools/ethernet_ota.py \
  artifacts/firmware/<version>/roller-ecu-<version>.recu
```

### 2.1 1.0.14 OPEN loader 替换历史门禁

当时的 1.0.14 OPEN loader 替换必须先离线运行：

```sh
./tools/provision_oemirot_open.sh preflight-open-oemirot-replacement
```

该命令不访问板卡。当时真正替换前 JP1 保持断开，ST-Link 必须接
SWD/GND/VTref/NRST；
使用 `replace-open-oemirot --accept-open-boot-replacement` 后，事务目录必须同时包含
fresh full Flash、128 KiB persistent、Option Bytes、runtime/OTA、两个 primary、
Programmer 直读 MCU UID、验签/完整身份/trailer 报告、固定的 ReleaseOpen 和
1.0.14 `.recu` 字节/hash，以及逐字节 boot readback。失败时只能
以错误消息给出的精确事务目录执行 `resume-open-oemirot-replacement`，不能复用旧
`repair-boot-layout` journal。

当时替换 loader 后先保持 ST-Link/NRST 接入，通过 Ethernet 安装并确认
1.0.14。板上原
1.0.12 应用尚未包含 LAN8742 冷启动复位时序修复，所以在 `accepted_sequence=14`、
双确认、runtime safe 和延迟复核完成前，严禁移除 ST-Link或执行掉电冷启动。首次
无探针冷启动必须发生在确认后的 1.0.14 上。

当前板和历史 CLOSED 验收阶段的 Option Bytes 必须满足：

```text
TZEN=0xB4
PRODUCT_STATE=0x72 (Closed)
BOOT_UBE=0xB4
SECBOOTADD=0xC0000, SECBOOT_LOCK=0xB4
SECWM1_STRT=0x00, SECWM1_END=0x7F
SECWM2_STRT=0x01, SECWM2_END=0x00
WRPSGn1=0xFFFFFFF0
WRPSGn2=0xFFFFFFFF
HDP1_STRT=0x00, HDP1_END=0x17
HDP2_STRT=0x01, HDP2_END=0x00
```

完整布局、首次 provision、JP1/BOOT0 阶段和 CLOSED 门槛见
`ECU_Security_and_OTA.md`。本样件曾完成 OPEN→CLOSED，随后因 Full Regression
回到 OPEN；现已通过 UUID=`0f3537fe-c04d-4298-ba50-995773e07a6a` 的不可变事务
再次进入 `0x72 CLOSED`。该 Full Regression→OPEN 过程仍作为历史恢复证据保留，
当前生命周期以安全文档第 6.3 节为准。

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
2. 进入“电流 PI 调参”，等待“波形就绪”且首批 1 kHz 基线样本已经显示，再读取
   ECU 参数；首次没有有效日志时应显示默认参数来源。
3. 在调参页启用零目标控制，依次给前进 `+100、+200、+300 mA`，每档至少保持
   2 s；用串联标准电流表测量线圈电流，并与 UI 前进反馈比较。每次阶跃必须从
   调参页直接设置，确认波形包含动作前基线；随后立即归零并确认反馈 <50 mA。
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

### 5.1 UI CAN2 转向分阶段验收

转向无本地角度反馈，必须把“通信零动作”与“低能量实动”分开记录。
第一阶段不勾选转向使能：

1. 确认 UI 每 100 ms 收到 52 B `STEERING_STATUS`，状态年龄 <500 ms，
   请求速度、已确认命令速度（非反馈）、速度命令千分比和电机速度反馈原始值为 0，
   `motor_enable_confirmed=0`。
2. 上电时 CAN 分析仪应看到先发 `0x2000=0`、再发 `0x200C=0`；禁止在
   启动序列中出现非零 `0x2000`。使能 `0x200D` 未收到严格 SDO ACK 时，
   状态不得进入 ACTIVE；节点 1 的厂商规定 ACK 是
   `ID=0x05800001, data=60 0D 20 00 00 00 00 00`，第 4 字节不是命令中的 1。
3. 分别在闲置、持续普通控制和打开 PI 波形三种情况执行
   `./tools/check_ecu_latency.sh`；均须满足无丢包、平均≤1 ms、最大≤5 ms，不得重现
   20 ms 级阻塞峰值。
4. 抓包确认 V2/V1 状态各为 20 Hz，转向/诊断/安全状态各为 10 Hz，周期类报文按
   0/10/25/35/60 ms 相位分散，同一 HAL 毫秒最多提交一类周期应用帧；调参未打开时
   UDP 50004 必须没有高速遥测。
5. 分别从遥控器 `172.16.0.9`、调试机 `172.16.0.10`、SN-EJAHGJI 域控
   `172.16.0.12` 使用不同 sender 发送全中性控制，再以不同的 1..254 priority
   逐级竞争；确认数值较大的活动 sender 获得控制，相同数值由较小 sender ID
   确定性胜出，低优先级 sender 继续续租但不会影响当前输出。当前活动控制方超过
   250 ms 后按既有 IDLE/Disarm/rearm 边界处理。
6. 从 `.9` 和 `.12` 发送 GET/APPLY/SUBSCRIBE/OTA STATUS，确认 ECU 不回复；只有
   `.10` 可使用调参、遥测和 OTA。从未列入白名单的受控测试地址发送普通控制，
   确认 ECU 不动作；再发送结构及校验正确的零输出急停，确认其仍进入安全停止。
   该项只验证纵深 IP 策略，不得记录为密码学鉴权测试。
7. 在每个白名单 IP 上用第二个受控测试进程和不同 UDP 源端口发送更新 sequence，
   记录同一 IP 属于一个主机信任域而可接管相同 sender；同时确认不同 IP 不能续租
   已绑定 sender。生产主机进程隔离和点对点二层隔离必须纳入系统验收，不能把
   源端口、源 IP 或 CRC 当作密码学身份认证。

只有本文第 1 节与转向附加安全条件均已由现场人员确认，才进入第二阶段：

1. 将 UI sender 设为 1 或 2，输入小速度（建议首次 0.6°/s，换算后约为 1‰
   额定转速命令），勾选转向使能并确认风险对话；保持不按方向键，确认使能
   ACK 后速度命令仍为 0。电机参数 0002 必须先核对为 80 rpm；否则禁止实动。
   断开或屏蔽 ACK 时，ARM 必须在 750 ms 内自动失能；已确认后即使保持零速度，
   丢失 ACTIVE/command-enable/motor-enable 任一确认也必须自动失能。
2. 点按正向不超过 200 ms 后松开，确认方向与整车定义一致、松手立即回零；
   反向重复。若方向错误，立即停止并修正受版本控制的方向配置，不得靠现场记忆补偿。
3. 按住某方向不松，确认 2 s 到期后自动零速度/禁用、置位
   `MANUAL_LIMIT|REARM_REQUIRED`，且持续非零包不能重启；必须先 STOP 才能再次 ARM。
4. 在零速度下测试页面失焦、切换 sender、断开上位端网络和 CAN 节点掉线；
   分别确认零速度/禁用序列、250/300 ms 双看门狗、bus-off/timeout 故障可见，
   且转向局部故障不破坏 Ethernet、CAN1 和无关继电器的调度。
   另用 251 ms、299 ms 周期发送同一 sender 的非零包，确认控制权仍在过期瞬间
   IDLE/Disarm，非零帧被拒绝；只有全中性禁用帧、完整 20 ms 安全轮和 Secure
   接受后才能重新 ARM。反复每 1 ms 请求安全或每 20 ms发送 disable-zero 时，
   延迟 45 ms 的零速/禁用 SDO ACK 仍必须完成原事务，不能被刷新丢弃。
5. 结束后点击 STOP、取消使能、释放控制，确认零速度、电机未使能、
   控制模式 IDLE，并归档 CAN 抓包、UI 截图、延迟报告和固件 hash。

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
| HW-09 | Secure CAN1/J1939 | 250 kbit/s 六 PGN 解析与旧车一致，1000 ms 有效/1001 ms 过期，非法格式/其他 ID 拒绝；NonSecure 仅见 NSC 快照；warning/passive/bus-off 置 bit27，恢复后清除且不影响 Ethernet/CAN2 |
| HW-10 | CAN2 转向 | 250 kbit/s；只接收节点 1 SDO/heartbeat；零速度/禁用先行；使能 ACK 前无非零 TX；正反向短脉冲、手动 2 s 限时和通信故障安全停机全部通过 |
| HW-11 | PB8/PB9 软件 I²C/ATECC reset recovery | PB9=SCL、PB8=SDA；启动无条件 START+9 clocks+START+STOP，随后 reset/sleep；最长 725 ms 有界重试期间服务 IWDG；当前 R1 无飞线 |
| HW-12 | IWDG/CSS | NonSecure 停止或 HSE 故障时先关输出，约 2 s 复位 |
| HW-13 | 电源/EMC/热/耐久 | 无非预期吸合，等级符合整机风险分析和目标标准 |

该固件的故障安全设计不能替代独立硬件安全链，也不构成 IEC 61508、ISO 13849
或整车功能安全认证。

## 8. 成对 OTA 故障与断电验收矩阵

验证层级定义如下：`HOST` 是静态源代码/派生 loader 审计或本机单元测试；
`HW` 是目标 ECU、Ethernet 和受控电源上的实板项目；`HW-FI` 是必须使用专用
故障注入固件或夹具执行的项目。HOST 通过只能证明策略和构建产物结构，不能替代
真实 Flash、复位、IWDG 和掉电时序证据。

量产板逐板进入 CLOSED 前的本板门禁是 OTA-H01..H06、OTA-P01 正常安装、两个
primary 的独立验签/完整身份/双确认审计，以及 JP1 断开、完全移除 ST-Link 后的
仅 ECU 冷启动、ATECC/零输出和 Ethernet 延迟复核。OTA-P02..P16 是绑定硬件版本、
loader 源码与发布 hash 的**发布/型式鉴定矩阵**，只在具备完整恢复能力的专用 OPEN
样件或可替换 Flash 夹具上执行；它们不是每块量产 ECU 在 CLOSED 前重复承受的测试。

### 8.1 静态与主机门禁

| 编号 | 检查/命令 | 通过标准 | 层级 |
|---|---|---|---|
| OTA-H01 | `./tools/test.sh` 的 paired policy 测试 | PERM、TEST/REVERT 冲突拒绝；`NONE/TEST`、`NONE/REVERT` 与完整发布身份比较的状态枚举符合设计 | HOST |
| OTA-H02 | `./tools/test.sh` 的 confirmation policy 测试 | 穷举两侧 `image_ok` 字节，只有精确的 `0x01/0x01` 可提交硬件 counter | HOST |
| OTA-H03 | `./tools/build_oemirot.sh ReleaseOpen` 和 `ReleaseClosed` | 实际生成 loader 的顺序严格为 partial-PERM guard → recovery review/complete → initial counter gate → dependency → target-pair gate → swap → primary validation → final-pair gate → redundant countermeasure | HOST |
| OTA-H04 | `./tools/audit_config.sh` | clean/partial PERM、双 counter 提交谓词、最终身份门、FIH 判定、双 controller relock-or-reset 和 gate 顺序锚点均存在 | HOST |
| OTA-H05 | `python3 -m unittest discover -s tests -p 'test_package_firmware.py' -v` | version/counter 不可重用，发布历史不完整时 fail closed，已进入发布历史的 1.0.13 身份不能被重用 | HOST |
| OTA-H06 | `./tools/test.sh` 后构建两个 OEMiROT profile | trusted-pair 谓词拒绝未确认/半确认、单侧未验签或身份不一致；派生 loader 中 FIH 授权默认失败，只能在完整提交门后打开，且 FAIL guard 严格早于 `swap_set_image_ok()` | HOST |

### 8.2 发布/型式鉴定硬件矩阵

| 编号 | 故障或断电注入点 | 通过标准 | 层级 |
|---|---|---|---|
| OTA-P01 | 正常安装 1.0.15 或更高的同身份 TEST 配对 | 两侧验签并启动同一 version/counter；NS 后 S 精确写入 `image_ok=0x01`；只有双确认后硬件 counter 才推进，冷启动保持新配对 | HW |
| OTA-P02 | CHUNK 期间多点断电，包括仅一侧完整 secondary 已带 TEST trailer、另一侧尚不完整 | 依赖检查先取消不完整请求，旧的已确认 S/NS 配对启动；目标门不误拒绝可恢复旧配对，应用绝不接收 old/new 混合 primary | HW |
| OTA-P03 | FINISH 后及两侧 forward swap 的每个可控阶段断电 | OEMiROT 续做到完整新配对或恢复完整旧配对；每次应用入口处两个 primary 的完整 version/counter 必须相同 | HW |
| OTA-P04 | 未确认镜像启动时分别断开网线、制造短暂 MDIO 读错和持续 PHY/时钟故障 | 仅 link-down 不阻止地址 0/PHY ID 就绪与确认；短暂故障可在 5 s 内恢复；持续故障在 5 s 内停止喂狗并成对回滚 | HW |
| OTA-P05 | 在写 NonSecure `image_ok` 之前断电 | 两侧仍未确认，下一次启动成对回滚；两个硬件 counter 均不推进 | HW |
| OTA-P06 | NonSecure `image_ok=0x01` 后、Secure flag 写入前断电 | 相同发布身份的半确认被派生 loader 识别为 `NONE/REVERT`，验证旧 secondary 后强制双回滚；两个硬件 counter 均不推进 | HW |
| OTA-P07 | 双回滚已完成一侧、另一侧尚未完成时多点断电 | primary 身份不同被识别为回滚续做，只完成剩余一侧；最终为完整旧配对，混合配对从不进入应用 | HW |
| OTA-P08 | 两侧 `image_ok=0x01` 后，在两个硬件 counter 依次更新及其复位边界断电 | 不回滚已确认新配对；每次复位仍先验证双 flag/header，安全地续做尚未推进的一侧 counter | HW |
| OTA-P09 | 用受控测试密钥构造签名均有效但 version 或 protected counter 不同的 S/NS 目标，并覆盖剩余 secondary 不可读/损坏路径 | post-dependency 目标门或最终 primary 完整身份门以 `BOOT_EBADIMAGE` 停止；任何混合 primary 均不跳转应用 | HW-FI |
| OTA-P10 | 注入 `BOOT_FLAG_BAD`、无效 header 和非 `0xFF/0x01` flag | 确认、counter 与应用启动均 fail closed，不把异常值解释为已确认 | HW-FI |
| OTA-P11 | 分别强制 `HAL_FLASH_Lock_S()`、`HAL_FLASH_Lock_NS()` 失败 | 在返回 NonSecure 应用前立即执行系统复位；复位后 controller 为锁定态，OEMiROT 按实际双/半确认状态处理 | HW-FI |
| OTA-P12 | 对已确认镜像制造持续 PHY/REFCLKO 故障，再恢复或受控复位 | 安全主循环持续运行、输出保持安全且网口保持 down，不因 5 s test-swap 门禁自动回滚或复位；硬故障仅由受控 MCU 复位/整机掉电恢复 | HW |
| OTA-P13 | 分别在 Secure 或 NonSecure PERM swap 的各恢复阶段断电，形成单侧 partial PERM | `boot_prepare_image_for_update()` 在 review/complete 和任何新恢复写入前 FIH fail closed；partial 状态不被擦除为 `NONE`，应用和 NV counter 更新均不可达 | HW-FI |
| OTA-P14 | 构造两侧 interrupted PERM，并组合双 `image_ok=0x01`、单侧/双侧有效签名尝试绕过确认 | 无论先处理哪一侧，partial-PERM guard 都先于恢复与 counter 路径拒绝；两个 NV counter 均不改变，任何镜像组合均不跳转应用 | HW-FI |
| OTA-P15 | 安装尚未确认的同身份 TEST pair，再破坏两个 rollback secondary，使两侧均进入 `BOOT_SWAP_TYPE_FAIL` | FIH trusted-pair 授权保持失败；在任一 `swap_set_image_ok()` 前 fail closed，两个 primary flag 保持原未确认值，两个 NV counter 不变且应用不启动 | HW-FI |
| OTA-P16 | 以已双确认、当次双侧完整验签且身份一致的 primary pair，注入单侧及双侧坏 staging/FAIL | 只有该 trusted pair 允许幂等 FAIL 清理；flag 保持 `0x01`、发布身份和 NV counter 不被伪造或意外改变，最终仍只启动同一 trusted pair | HW-FI |

OTA-P01 必须在待转换板或 CLOSED 后唯一发布形成独立实测记录；第 10 节历史
OTA PASS 不能替代第 11.2 节的 1.0.15 转换基线、第 11.3 节的 1.0.16
完整终验或第 11.4 节的 1.0.17 CLOSED Ethernet OTA。第 11.1 节的 1.0.14 三次无探针
冷启动保留为历史证据，不能冒充当前发布。OTA-P02..P16 的发布/型式鉴定记录
必须精确绑定固件包 SHA-256、S/NS version/counter、update sequence、loader 源码和
硬件版本；相关断电记录还应包含注入时刻、掉电保持时间、重启后的
swap/flag/counter、零输出和网络状态。已批准且绑定相同发布基线的型式试验证据可供
该批量产板 CLOSED 评审引用，不在每块生产板上重做破坏性注入。

不得刷写已撤销的 1.0.13，也不得为了补测而复用已经接受的 1.0.14、1.0.15、
1.0.16 或 1.0.17
identity；尚未
完成的 OTA-P02..P16 必须在下一唯一发布身份或专用测试发布上执行，并明确记录为
PENDING，不能写成 PASS。OTA-P15/P16 会故意破坏 rollback/staging，只能使用具备
完整恢复包的 OPEN 专用样件或可替换 Flash 夹具执行，不得在生产 ECU、唯一验收板
或 CLOSED 样件上试验。

## 9. 2026-08-27 功能实测记录（历史）

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

## 10. 2026-08-29 安全启动与 OTA 实测记录（历史）

下表记录同一开发样件执行 Full Regression 之前的状态：当时已安装去除临时启动
trace 的 `ReleaseClosed` OEMiROT，JP1 断开，产品状态为 CLOSED (`0x72`)；最终
运行版本为 `1.0.9`，security counter=9，update sequence=9。它不是当前生命周期
状态，也不是第 8 节新增成对策略的 HW/HW-FI 验收证据。

| 项目 | 实测结果 | 结论 |
|---|---|---|
| ATECC608C | serial=`0123d47eb2ee0e9bee`，revision=`00006005`，config CRC32C=`0xEBB326F3`，Config/Data/slot 2 locked，P-256 随机挑战通过 | PASS |
| MCU 配对 | OPEN 阶段 Programmer 物理 UID=`003800613434511232383537`；OEMiROT 使用固定且已复核的 RSS IDCODE/SFSP manifest，handoff 带反码和 CRC32C，不依赖不可用的实时 DBGMCU 读取；HUK 封装 OBKeys、ATECC 不可导出私钥及 CLOSED/HDP 配对记录构成防移植根，pairing generation=1 | PASS |
| 主机测试/审计 | 9 项协议测试、完整配置审计、CubeMX 6.18 `.ioc` 加载校验、`git diff --check`、签名应用和 ReleaseClosed 构建通过 | PASS |
| CLOSED Ethernet OTA | `1.0.8`→`1.0.9` 成对 Secure 192 KiB + NonSecure 320 KiB 下载、FINISH、签名/哈希验证、test swap、应用确认成功；全程未使用 ST-Link | PASS |
| 中断/提交边界 | 65536 B 中断的无效 incomplete image 不启动；完整 signed secondary 自带 trailer magic，未 FINISH 后复位仍可能被 OEMiROT 评估，因此生产流程强制 FINISH 并核对 journal | PASS（边界已修正文档） |
| 传输签名 | 篡改包拒绝，result=-16 | PASS |
| 防重放 | 已接受 sequence 后再次发送同包，BEGIN 拒绝，result=-19；schema-2 journal 最终 quadword 带反码及 generation/sequence 副本 | PASS |
| OEMiROT 防回滚 | 较低 security counter 的签名包可完成传输但不替换已确认 primary | PASS |
| Flash 写后校验 | 定位并修复 STM32H5 ICACHE 对刚擦写 journal 返回旧值；OTA、身份、阀参数存储均在回读前失效 cache | PASS |
| 最终冷启动状态 | ota_result=0、state=IDLE、accepted_sequence=9；ATECC auth=0；relay mask=0、两路 duty=0、调参未启用、遥测发送计数=0 | PASS |
| 最终 Ethernet | 冷启动 10 包 0% 丢包，RTT min/avg/max=`0.078/0.140/0.186 ms`；100 包门禁=`0.058/0.129/0.185 ms` | PASS |
| DA 售后调试 | 三证书链+leaf 私钥成功临时打开 HDPL3 S/NS；Secure/NonSecure primary 可读；close-debug 成功 | PASS |
| Full Regression 能力 | DA permission `0x4040` 含 bit14；工具要求 `--accept-full-device-erase`，未在本样件执行以免擦除全部 Flash/OBKeys | PASS（非破坏性验证） |
| ST-Link 防导出 | close-debug 后普通 Hotplug AP1 连接和 `0x0C030400` 读取均被拒绝 | PASS |

固件包 `artifacts/firmware/1.0.9/roller-ecu-1.0.9.recu` 的 SHA-256 为
`c2c18673e6d370462bb64abc5dcdd3f8443813205ca5ef050a9af7bf4cbd335e`。离线 PKI
双备份已由密钥负责人确认；本记录不包含破坏性的 Full Regression 实跑。

## 11. 2026-08-30 OPEN→CLOSED 样件回归记录

本节只记录已取得的实测证据。当前产品状态为 CLOSED (`0x72`)，已接受唯一签名的
1.0.17/counter17/`accepted_sequence=17`，JP1 断开。1.0.15 的不可变 CLOSED
转换、普通未认证读取拒绝与 DA 售后边界继续作为转换/恢复基线；1.0.16 的发布后
DA 回读、关闭调试和最终无探针冷启动也已独立完成。1.0.17 的 DA 精确回读、
关闭调试、关闭后读取拒绝和最终无探针冷启动均已完成。

### 11.1 1.0.14 OPEN loader 替换与无探针冷启动历史证据

| 项目 | 2026-08-30 实测结果 | 结论 |
|---|---|---|
| OPEN loader 替换 | 不可变事务 `stm32h563-066BFF565456857187210935-20260830T050808Z-open-loader-replacement` 达到 `replacement_complete_stlink_cold_start_embargo_active`；ReleaseOpen 52276 bytes、SHA-256=`5fdaaa3f5e2d2a6c01ed527282ba08b3d8d5e579c61ec3c4e097cdf2c0fd7761`，写后逐字节回读一致并恢复 WRP/HDP | PASS |
| Ethernet OTA 1.0.14 | 使用事务内固定包 SHA-256=`a052f51a7ef14bbbec57f412950edad7f44f92a2323227d74c606d8dd866324d` 完成成对 TEST swap；最终 `state=IDLE`、`result=0`、`ota_result=0`、`accepted_sequence=14` | PASS |
| primary 密码学审计 | Secure/NonSecure 分别用各自 OEMiROT 公钥验签；完整 identity 均为 `1.0.14+0`/counter14，trailer magic 合法且 `image_ok=0x01/0x01` | PASS |
| V2 无负载回归 | 在明确两路阀线圈未接入的模式下 29/29 项通过；继电器逻辑、200 mA 目标斜坡、PWM 互斥、安全换向、仲裁和超时释放通过，结束后 IDLE/relay mask=0/duty=0 | PASS（真实阀电流跟踪 SKIP） |
| 替换前基线延迟 | 100 包 0% 丢包，RTT min/avg/max/mdev=`0.059/0.116/0.190/0.025 ms` | PASS |
| OTA 后空闲延迟 | 100 包 0% 丢包，RTT min/avg/max/mdev=`0.057/0.128/0.183/0.033 ms` | PASS |
| 29 项动作后空闲延迟 | 100 包 0% 丢包，RTT min/avg/max/mdev=`0.057/0.133/0.186/0.034 ms` | PASS |
| PI 调参遥测延迟 | 仅在调参会话中启动 1 kHz 遥测时，100 包 0% 丢包，RTT min/avg/max/mdev=`0.063/0.129/0.185/0.030 ms`；退出后 telemetry frames=406、dropped samples=0，遥测已停止 | PASS |
| 无探针冷启动 | 连续三次保持 JP1 断开、完整移除 ST-Link，并在 ECU 断电至少 10 s 后仅 ECU 上电；每次均通过 fresh MCU/ATECC 身份、`accepted_sequence=14`、OTA IDLE/result=0、双确认和零输出门禁 | PASS |
| 冷启动周期 1 延迟 | 100 包 0% 丢包，RTT min/avg/max/mdev=`0.051/0.118/0.192/0.036 ms` | PASS |
| 冷启动周期 2 延迟 | 100 包 0% 丢包，RTT min/avg/max/mdev=`0.063/0.123/0.247/0.040 ms` | PASS |
| 冷启动周期 3 延迟 | 100 包 0% 丢包，RTT min/avg/max/mdev=`0.064/0.143/0.194/0.028 ms` | PASS |

以上 1.0.14 结果保持原始版本、counter、sequence 和 RTT，不回填为当前发布。

### 11.2 1.0.15 CLOSED 转换与恢复基线

| 项目 | 2026-08-30 实测结果 | 结论 |
|---|---|---|
| Ethernet OTA 1.0.15 | package SHA-256=`9980acbf248f242c77c7044f48d627ca862d6714221f5835c0a21b7a24bf9e54`；成对 TEST swap 和确认完成，最终 `state=IDLE/result=0/ota_result=0/accepted_sequence=15` | PASS |
| installed primary fresh audit | Secure/NonSecure 均为 `1.0.15+0`/counter15，精确匹配 reviewed header、TLV、解密后 plaintext payload 与两次 swap trailer，`image_ok=0x01/0x01` | PASS |
| ATECC MCU-only hostile reset | 每轮依次执行 option-byte display/reset、under-reset UID read、final reset，ATECC 全程不断电；20/20 周期均通过 fresh 配对认证、accepted15、零输出和 Ethernet，证据为 `artifacts/hardware-regression/20260830T061938Z-atecc-reset-recovery/` | PASS |
| ATECC 有界恢复策略 | 启动无条件执行 `START + 9 clocks + START + STOP`，再执行 word-address reset/sleep；以 25 ms 步长、最长 725 ms 重试并服务约 2 s IWDG，超时保持 quarantine | PASS |
| Pairing A/B repair | A=`0x080E0000` 原 committed record 保持不变；B=`0x080E2000` 从全擦除修复为同一 generation-1 record，写后逐字节回读；A/B 字节一致，16 KiB SHA-256=`45f1f08b626f3934f2359e4d44b1993c9333c5275f62870d74a77caeb6c9f310` | PASS |
| CLOSED dual pairing gate | 事务 `artifacts/device-backups/stm32h563-066BFF565456857187210935-20260830T063129Z-closed-transition/`，UUID=`0f3537fe-c04d-4298-ba50-995773e07a6a`；finalizer 从 fresh full Flash 与独立 persistent readback 得到同一 16 KiB store，dual verifier 校验 A/B 后先持久化 `pairing_store_dual_verified`，再进入 `pre_mutation_evidence_verified` 和首次写操作；最终 phase=`product_state_closed_verified` | PASS |
| V2 基线无负载回归 | 1.0.15 上 29/29 通过，结束后 IDLE/relay mask=0/duty=0 | PASS（真实阀线圈 SKIP） |
| PI telemetry gating | frames=`0 → 175 → 175`，只在调参 session 内增长，退出后保持 175；dropped samples=0 | PASS |
| PI 调参网络延迟 | 调参开启时 100 包 0% 丢包，RTT min/avg/max=`0.059/0.114/0.290 ms` | PASS |
| 退出调参后空闲延迟 | 100 包 0% 丢包，RTT min/avg/max=`0.059/0.127/0.185 ms` | PASS |
| Full Regression recovery | `artifacts/device-backups/stm32h563-066BFF565456857187210935-20260830T064244Z-final-1.0.15/`，schema v4，exact 1.0.15，绑定同一 CLOSED UUID、dual pairing evidence 和发布输入，离线验证通过；manifest 的 `0xED OPEN` 为转换前 capture，当前 target 为 `0x72 CLOSED` | PASS |
| 1.0.15 基线 DA 售后调试 | DA OBK 权限为 `0x4040`；discovery 为 `ST_LIFECYCLE_CLOSED`、integrity VALID；permission `c` 认证后 Secure primary 196608 B 和 NonSecure primary 327680 B 分段回读均与 CLOSED 事务一致，证据为 `artifacts/hardware-regression/20260830T064437Z-closed-da-service/` | PASS |
| 1.0.15 基线 ST-Link 防未授权导出 | 认证前 `0x0C030400`/256 B 普通读取被拒绝；`close-debug` 成功后再次读取仍被拒绝 | PASS |
| CLOSED 转换基线 | ReleaseClosed + confirmed 1.0.15；`PRODUCT_STATE=0x72`；Full Regression 权限可发现但本轮未再次执行 | PASS |

### 11.3 1.0.16 CLOSED Ethernet OTA、DA 与冷启动

| 项目 | 2026-08-30 实测结果 | 结论 |
|---|---|---|
| 唯一发布 | version 1.0.16/counter16/update-sequence16，package SHA-256=`3e5ab07c7b6127dcedb46a1b4b1a695740904dd940d7128631f714d959a0b623` | PASS；身份已消耗 |
| CLOSED Ethernet OTA | 传输、reset、成对 TEST swap 和确认完成；`state=IDLE/result=0/ota_result=0/accepted_sequence=16`，全部 session 字段为 0 | PASS |
| post-OTA runtime | MCU/ATECC/pairing 验证通过，无 quarantine，relay/PWM 零输出，调参关闭 | PASS |
| V2 无负载回归 | OTA 前 29/29、OTA 后 29/29；两路阀线圈未接 | PASS（真实电流跟踪 SKIP） |
| PI telemetry gating | inactive=0，active 增长到 681、dropped=0，close 后保持 681 | PASS |
| PI 调参网络 | active 时 100 包 0% 丢包，RTT min/avg/max=`0.064/0.138/0.340 ms`；idle 后=`0.055/0.104/0.148 ms` | PASS |
| 升级后仅 ECU 冷启动 | 移除 ST-Link，ECU 断电至少 10 s 后仅 ECU 上电；runtime 安全检查 PASS；100 包 0% 丢包，RTT min/avg/max=`0.067/0.116/0.328 ms` | PASS |
| 1.0.16 DA 默认拒绝与认证 | 证据目录=`artifacts/hardware-regression/20260830T073110Z-closed-ota-1.0.16-da-service/`，`evidence.json` SHA-256=`1478a4ac2c8748e585a86756f16a7fe6384a80f3b633196b023e23667347ca6e`；认证前普通读取 rc=1/拒绝；严格 discovery 为 target `0x484`、SDA `2.4.0`、`ST_LIFECYCLE_CLOSED`、integrity VALID；permission `c` 认证 PASS | PASS |
| 1.0.16 primary 精确回读 | Secure `0x0C030000/0x30000`、NonSecure `0x08100000/0x50000`（分段）完成；`tools/verify_closed_ota_readback.py` 验证 `1.0.16+0`/counter16、`image_ok=0x01/0x01`；canonical swap size：S=`0x2DDE8`，NS=`max(1.0.15 0x8D39, 1.0.16 0x8D38)=0x8D39`；`primary-readback-audit.json` SHA-256=`034c7d99996eed055346e315c3109ba355044051f53a679771b15db79ffbc4b8` | PASS |
| 1.0.16 close-debug | `close-debug` PASS；严格 CLOSED/VALID 复核 PASS；关闭后普通未认证读取再次 rc=1/拒绝；本轮未执行 Full Regression | PASS |
| DA 后最终无探针冷启动 | 完整移除 ST-Link，ECU 断电至少 10 s 后仅 ECU 上电；MCU/ATECC/pairing、no-quarantine、relay/PWM 全零、`tuning_active=false`、telemetry=0，OTA `state=IDLE/result=0/ota_result=0/accepted_sequence=16`；100 包 0% 丢包，RTT min/avg/max=`0.060/0.116/0.331 ms` | PASS |

### 11.4 1.0.17 CLOSED Ethernet OTA、DA、冷启动与转向失联安全状态

| 项目 | 2026-08-30 实测结果 | 结论 |
|---|---|---|
| 唯一发布 | version 1.0.17/counter17/update-sequence17；package SHA-256=`75948795f39d898795b85841e4ad2e06c47738240184ff9ffbdc729fc11ab599`；双 initial 与 transport 签名离线验证通过 | PASS；身份已消耗 |
| CLOSED Ethernet OTA | 未接 ST-Link 完成传输、FINISH、reset、成对 TEST swap 和确认；最终 `state=IDLE/result=0/ota_result=0/accepted_sequence=17`，全部 session 字段为 0 | PASS |
| post-OTA runtime | MCU/ATECC/pairing/no-quarantine 通过；relay mask、两路阀目标/PWM、转向请求/应用速度及 motor-enable 均为 0；调参和高速 telemetry 关闭 | PASS |
| CAN2 电机失联 | 当前 `rx_frames=0`、protocol fault、bus passive、`SAFE_DISABLE_PENDING`，控制器持续请求安全禁用且所有转向控制量为零 | PASS（fail-closed）；主动电机控制 PENDING |
| Ethernet 性能 | OTA 前 100 包 0% 丢包，RTT min/avg/max=`0.068/0.099/0.157 ms`；OTA 后=`0.065/0.104/0.144 ms` | PASS |
| 非侵入证据 | `artifacts/hardware-regression/20260830T150744Z-closed-ota-1.0.17-runtime/` 含摘要及 SHA-256 manifest | PASS |
| 1.0.17 DA 默认拒绝与认证 | 证据目录=`artifacts/hardware-regression/20260830T154340Z-closed-da-readback-1.0.17/`；认证前普通读取 rc=1/无文件；严格 discovery 为 target `0x484`、SDA `2.4.0`、CLOSED、integrity VALID；仅 permission `c` 认证 PASS，本轮未执行 Full Regression | PASS |
| 1.0.17 primary 精确回读 | Secure `0x0C030000/0x30000`、NonSecure `0x08100000/0x50000` 六段回读完成；verifier 证明 `1.0.17+0`/counter17、`image_ok=copy_done=0x01/0x01`、S swap=`0x2DDE8`、NS swap=`0x8D38`；审计 JSON SHA-256=`8edbfecbccfbce61d724299d6dd23845eb576f457555a65cd7dc6a1b4cfb8401` | PASS |
| 1.0.17 close-debug | `Locking Debug` 和严格 CLOSED/VALID 复核 PASS；关闭后同一普通读取再次 rc=1/无文件 | PASS |
| DA 后最终仅 ECU 冷启动 | 完整移除 ST-Link、保持 JP1 断开并断电至少 10 s；MCU/ATECC/pairing/no-quarantine、relay/阀/PWM/转向零输出、调参关闭、telemetry=0；OTA IDLE/result=0/accepted17；100 包 0% 丢包，RTT min/avg/max=`0.066/0.103/0.146 ms` | PASS |
| DA 证据外部备份 | 已复制到 `/home/plac/Documents/ECU_PKI/artifacts/hardware-regression/20260830T154340Z-closed-da-readback-1.0.17/`；0700/0600、逐字节 diff 和 manifest 均通过 | PASS |

### 11.5 1.0.18 控制端白名单、优先级仲裁与 OTA 发布准备

| 项目 | 2026-08-31 结果 | 结论 |
|---|---|---|
| 网络身份 | 产品序列号 `SN-EJAHGJI`；ECU `.11`、域控 `.12`；固定遥控器 `.9`、调试/售后机 `.10` | PASS（源码与静态审计） |
| 控制边界 | 普通控制只接受 `.9/.10/.12`；调参、遥测、工厂服务和 OTA 仍只接受 `.10`；活动 sender 绑定源 IP | PASS（自动化测试） |
| 多源仲裁 | 显式 priority `1..254` 数值越大越优先；同优先级较小 sender ID 胜出；0 仅兼容旧客户端，255 保留网络紧急 | PASS（自动化测试） |
| UI | sender ID 与 priority 独立配置；活动控制期间修改任一项均要求先 RELEASE | PASS（自动化测试） |
| 主机回归 | `tools/test.sh` 的 Python/静态 199 项通过；本机无 `cc`，C 主机行为断言降级为 ARM `-Werror` 交叉编译，签名 Release 同源构建/链接通过；`tools/audit_config.sh` 与 `git diff --check` 通过；NonSecure Flash=`30752 B/311 KiB`、RAM=`91960 B/320 KiB` | C 主机运行时断言 SKIP；目标 ARM 构建 PASS |
| 唯一发布 | version 1.0.18/counter18/update-sequence18；package SHA-256=`4586f106b91fe91f293643a356105c26bf3af74161ac28034fc3494f999a517a` | PASS；身份已消耗 |
| 离线密码学/格式 | transport ECDSA、双 initial/加密 update OEMiROT 签名、镜像 identity/counter/dependency、1.0.17 previous-swap reference 与双 key-area hash 均固定并验证 | PASS |
| CLOSED Ethernet OTA | 等待从 macOS 固定服务地址 `172.16.0.10` 向 ECU `172.16.0.11` 执行；当前板仍为 accepted sequence 17 | **PENDING** |
| post-OTA runtime/冷启动 | 必须验证 accepted sequence 18、ATECC/MCU pairing、无 quarantine、零输出、PI telemetry 关闭、网络延迟和仅 ECU 冷启动 | **PENDING** |

以下项目仍为 PENDING，不得继承历史记录或把软件 mask 当作硬件 PASS：

- 阀体到货后的真实前进/后退电流闭环、PI 标定、开路/短路/过流和换向故障注入；
- K1..K27 实际触点、车辆负载、急停/驻车/启动/高低转速/龟兔档失电恢复；
- CAN1/J1939 车辆报文、速度输入和油温/油压/水位等传感器实信号；
- 接入真实转向电机后的 CAN2 enable/速度/停止/超时/DTC/bus-off/急停测试；
- 电源瞬态、IWDG/CSS、PHY 故障、EMC、环境、热和耐久型式试验；
- 第 8.2 节 OTA-P02..P16 专用可恢复样件发布/型式鉴定。
