# Roller ECU 固件

本工程面向自定义 STM32H563ZIT6 ECU，使用 STM32CubeMX 6.18、
STM32CubeH5 1.7、CMake 和 TrustZone。Secure 域独占急停、TPIC6A595
继电器链、20 kHz 行驶阀电流 PI、ADC、参数 Flash 日志、软件 I²C 与 IWDG；
NonSecure 域负责 LwIP UDP V2、CAN1/J1939 和速度采集。CAN2 只完成硬件
初始化，暂不启动。V2 保留上一版车辆功能语义，但以 ±2000 mA 电流目标替代
无物理单位的百分比，旧 V1 帧会明确拒绝。

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

当前样件仍处于 STM32 **OPEN** 产品状态，以便完成台架调试。此时 ST-Link 仍可
读取应用 Flash，因而“防导出”要求只有在安装 ReleaseClosed、复核全部 OTA 测试、
确认私钥已离线备份并执行不可逆 CLOSED 转换后才算最终满足。工程不会自动执行
该转换。

## 文档

- [KiCad/CubeMX 配置审查与 UI 核对](docs/ECU_CubeMX_KiCad_Audit.md)
- [继电器映射、网络协议和调试 UI](docs/ECU_Protocol_and_UI.md)
- [烧录与台架验收步骤](docs/ECU_Bench_Acceptance.md)
- [启动安全、ATECC 配对与 Ethernet OTA](docs/ECU_Security_and_OTA.md)
- [Secure/NonSecure 共用安全 API](Secure_nsclib/safety_api.h)

本固件包含故障安全基线，但不等同于经过 IEC 61508、ISO 13849 或整车
功能安全认证的安全控制器。带载测试前必须完成系统风险评估、独立硬件切断链
验证，以及电源、EMC、热、短路和机械安全试验。
