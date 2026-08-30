# Roller ECU 固件

本工程面向自定义 STM32H563ZIT6 ECU，使用 STM32CubeMX 6.18、
STM32CubeH5 1.7、CMake 和 TrustZone。Secure 域独占急停、TPIC6A595
继电器链、20 kHz 行驶阀电流 PI、ADC、参数 Flash 日志、软件 I²C 与 IWDG；
NonSecure 域负责 LwIP UDP V2/安全受限的旧 A5 兼容层和速度采集；受 STM32H563
ES0565 §2.2.34 约束，FDCAN1/FDCAN2 均归 Secure 且属性同为 `SEC|NPRIV`。
Secure 域在 CAN1 只读解析六类 J1939 PGN，经版本化 NSC 快照提供工程量，并通过
CAN2 独占控制节点 1 转向电机，把两条总线的故障分别隔离。V2 保留上一版车辆功能语义，
但以 ±2000 mA 电流目标替代无物理单位的百分比；显式 bit2 扩展提供带符号转向
角速度。旧 A5 帧可继续控制有等价硬件的继电器功能；旧百分比行驶请求、旧转角
闭环请求和本板不存在的输出会拒绝整帧，不能被静默换算为阀电流或电机转速。
非紧急控制、调参和 OTA 固定只接受点对点台架主机 `172.16.0.10`；该 IP 约束不是
密码学身份认证，车辆网络必须保持隔离。结构正确的急停不受该来源过滤阻挡。

当前 PCB 的 PB8/PB9 接反，固件明确使用 `PB8=软件 SDA`、`PB9=软件 SCL`。
这是 R1 板临时兼容方案，不需要飞线；下一次投板必须按 STM32 硬件 I²C
复用修正为 `PB8=SCL`、`PB9=SDA`。当前样件的 ATECC608C 已完成 P-256
设备密钥生成、锁区和 MCU 配对；每次启动均执行随机挑战验证，失败时执行器保持
隔离。OEMiROT 对 Secure/NonSecure 成对镜像做签名、防回滚、加密 OTA 和试运行
确认。

## 常用命令

```sh
./tools/cubemx_cli.sh validate
./tools/cubemx_cli.sh generate
./tools/audit_config.sh
./tools/test.sh
./tools/build.sh Debug
./tools/build.sh Release
./tools/build_oemirot.sh ReleaseOpen
./tools/build_oemirot.sh ReleaseClosed
python3 ./tools/ethernet_ota.py --status
```

本机台架网口及 UI：

```sh
sudo ./tools/configure_ecu_network.sh
./tools/check_ecu_latency.sh
./tools/ecu_debug_ui.py --ecu-ip 172.16.0.11
```

浏览器打开 <http://127.0.0.1:8088>。脚本只给 `enp2s0` 创建独立的
`ecu-bench` 配置（`172.16.0.10/16`、无默认路由），不会删除原连接。
UI 的“电流 PI 调参”页提供 1 kHz 实时波形、RAM 实时应用与显式 Flash 保存；
固件升级镜像不占用参数扇区。`check_ecu_latency.sh` 对 100 包零丢包、平均
RTT≤1 ms、最大 RTT≤5 ms 设置硬门槛，应在空闲和遥测开启时都执行。

构建产物：

- `Secure/build/Debug/ECU_S.elf`
- `NonSecure/build/Debug/ECU_NS.elf`
- Release 镜像位于对应的 `build/Release` 目录
- OEMiROT 台架/量产引导镜像分别位于
  `Bootloader/OEMiROT/build/ReleaseOpen` 和 `ReleaseClosed`
- 已签名、加密的以太网升级包位于 `artifacts/firmware/<version>/*.recu`

当前样件已处于 STM32 **CLOSED (`0x72`)** 产品状态，并已通过 Ethernet 接受唯一
签名的 1.0.17/counter17/update-sequence17 发布；OTA、DA 双主槽精确回读、调试
重锁、最终无探针冷启动、运行时安全和网络延迟均已通过。真实转向电机尚未接入，
主动 enable/速度/停止/超时/故障测试仍待硬件到位后完成。
普通 ST-Link 读取默认被拒绝。售后只允许持有 DA leaf 私钥和完整证书链的工程师
临时打开 HDPL3 Secure/NonSecure 调试，并在操作后执行 `close-debug` 与冷启动。
破坏性的 Full Regression 使用独立 permission `a` 和显式整片擦除确认，不能作为
日常调试步骤。后续应用更新优先使用签名、加密、成对验证的 Ethernet OTA。

## 文档

- [KiCad/CubeMX 配置审查与 UI 核对](docs/ECU_CubeMX_KiCad_Audit.md)
- [继电器映射、网络协议和调试 UI](docs/ECU_Protocol_and_UI.md)
- [烧录与台架验收步骤](docs/ECU_Bench_Acceptance.md)
- [启动安全、ATECC 配对与 Ethernet OTA](docs/ECU_Security_and_OTA.md)
- [Secure/NonSecure 共用安全 API](Secure_nsclib/safety_api.h)

本固件包含故障安全基线，但不等同于经过 IEC 61508、ISO 13849 或整车
功能安全认证的安全控制器。带载测试前必须完成系统风险评估、独立硬件切断链
验证，以及电源、EMC、热、短路和机械安全试验。
