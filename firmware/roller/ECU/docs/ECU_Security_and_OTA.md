# ECU 启动安全、ATECC608C 配对与 Ethernet OTA

## 1. 当前结论和安全边界

本工程已经形成 STM32H563 OEMiROT → Secure application → NonSecure application
的验证启动链，并将当前板载 ATECC608C 与本 MCU 做一对一配对。签名、加密、
防回滚和 Ethernet OTA 已在实板验证。

当前样件**仍处于 OPEN 产品状态**，用于完成开发台架验收。WRP/HDP 已保护并隐藏
OEMiROT boot 区，但 OPEN 状态下 ST-Link 仍能读取应用区，因此“用户无法导出
固件”尚未最终满足。只有在预装 `ReleaseClosed` 引导镜像、完成全部验收、确认
离线 PKI 备份可恢复并执行不可逆 CLOSED 产品状态转换后，才满足该项要求。

本工程的 Debug Authentication 策略只允许证书认证的 **Full Regression**：发生
售后恢复时整片擦除，不允许重开调试并保留客户固件。CLOSED 之后若 OTA 和应用均
损坏，正常恢复路径会丢失设备数据；这正是防提取策略的一部分。

## 2. 启动信任链

1. STM32 RSS 读取 HUK 封装的 OEMiROT OBKeys，唯一启动入口固定为
   `0x0C000000`；boot 受 WRP/HDP 保护。
2. OEMiROT 校验 Secure 和 NonSecure 两个 MCUboot primary 镜像的独立 ECDSA
   P-256 签名、版本依赖和 security counter。secondary 镜像在 Flash 中加密。
3. OEMiROT 在进入 HDPL3 前读取 MCU UID，通过位于 `0x3003FBC0` 的专用 SRAM
   handoff 传给 Secure 应用；记录带字段反码和 CRC-32C，linker 防止栈覆盖。
4. Secure 应用探测 ATECC608C，核对配置 CRC、I²C 地址、serial、revision、
   Config/Data/私钥 slot 锁状态和 MCU UID。随后由安全芯片私钥对 TRNG 随机
   challenge 签名，STM32 使用配对时固化的公钥验证。
5. 任何身份检查失败时，诊断和受签名 OTA 恢复通道仍可运行，但执行器 ARM 被
   Secure 域拒绝，全部 TPIC/PWM 输出保持安全态；OTA test image 也不会被确认。
6. 新 OTA 镜像只有在 ATECC 验证、Secure ADC/安全服务、CAN/速度和 LwIP 初始化
   都成功后才写 `image_ok`。之前发生 watchdog reset 时 OEMiROT 回滚旧镜像。

当前样件实测身份：

| 项目 | 值 |
|---|---|
| MCU UID | `003800613434511232383537` |
| ATECC serial | `0123d47eb2ee0e9bee` |
| ATECC revision | `00006005` |
| ATECC config CRC-32C | `0xEBB326F3` |
| ATECC 锁状态 | Config locked、Data locked、private key slot 2 locked |
| Pairing generation | 1 |

R1 PCB 因 PB8/PB9 网络交换，使用 Secure 开漏软件 I²C：PB8=SDA、PB9=SCL，
包含 clock-stretch timeout 和 9-clock bus recovery。下一版 PCB 必须恢复
PB8=SCL、PB9=SDA 的硬件 I²C1，并按 `ECU_CubeMX_KiCad_Audit.md` 执行 ECO；
当前代码和 UI capability 均标记 `SW_I2C_PCB_R1`，不能把临时方案带入 R2。

## 3. Flash 与保护布局

| 区域 | Secure alias / physical address | 大小 | 用途 |
|---|---:|---:|---|
| OEMiROT boot | `0x0C000000` | 128 KiB | WRP group 0..3，HDP |
| scratch | `0x0C020000` | 64 KiB | OEMiROT swap，HDP |
| Secure primary | `0x0C030000` | 192 KiB | vector=`0x0C030400` |
| Secure secondary | `0x0C060000` | 192 KiB | 加密 OTA image |
| NonSecure secondary | `0x0C090000` | 320 KiB | 加密 OTA image |
| Secure persistent | `0x0C0E0000` | 128 KiB | 身份、OTA journal、阀参数 |
| NonSecure primary | `0x08100000` | 320 KiB | vector=`0x08100400` |

MCUboot header=0x400、trailer=0x2000、最大对齐=16 B。布局只在
`Shared/ecu_flash_layout.h` 定义；OEMiROT、两个 linker、OTA writer 和 host
打包工具都引用或核对该定义。主应用的 CubeMX USER CODE 和项目自有目录不会被
再次生成覆盖，`audit_config.sh` 会检查 product-state profile、trailer offset、
持久区边界和临时启动 trace 已移除。

## 4. OTA 包与传输流程

发布包 `*.recu` 包含 metadata、固定 128 B manifest、64 B raw P-256 签名及成对
的 Secure/NonSecure 加密 MCUboot 镜像。host 工具先验证成员集合、尺寸、SHA-256
和 OTA transport 签名；ECU Secure 域再次执行同样验证。OEMiROT 在 reset 后用
另一组 root key 再验证两个内部镜像，传输签名私钥泄露不会直接绕过 secure boot。

UDP 50006 只接受源地址 `172.16.0.10`。BEGIN 会立即释放全部输出并进入 quarantine；
CHUNK 使用 512 B payload、CRC-32C、连续 offset 和 16 B Flash 编程粒度，重复的
已写 chunk 只有内容完全一致才允许恢复；FINISH 核对整镜像哈希和 trailer 后保存
接受序号并 reset。传输中断或未 FINISH 不会设置 swap magic，冷启动仍运行旧
primary。

`update_sequence` 是 ECU transport journal 的单调防重放序号；`security_counter`
是 OEMiROT 的签名镜像防降级计数，两者必须独立递增。签名有效但 counter 过低的
包仍可能消耗 transport sequence，发布系统必须再用更大的 sequence 生成修正版。

控制/诊断/PI 调参端口没有端到端鉴权，必须只位于隔离车辆网络。OTA 的源 IP
限制只是缩小攻击面，真正授权来自 transport ECDSA 和内部 OEMiROT image roots。

## 5. 构建、发布和升级命令

常规质量门：

```sh
./tools/cubemx_cli.sh validate
./tools/audit_config.sh
./tools/test.sh
./tools/build_oemirot.sh ReleaseOpen
./tools/build_oemirot.sh ReleaseClosed
./tools/build_signed_apps.sh Release
```

`ReleaseOpen` 和 `ReleaseClosed` 都是 CMake Release 优化构建；前者只把 boot 所需
最低产品状态设为 OPEN，后者要求 CLOSED。只有 Debug profile 定义开发行为，不能
用 `OEMIROT_DEV_MODE` 冒充量产构建。

生成新版本时 version、security counter 和 update sequence 必须来自受控发布记录：

```sh
python3 tools/package_firmware.py \
  --version 1.0.5 --security-counter 5 --update-sequence 5 \
  --build-type Release
sha256sum artifacts/firmware/1.0.5/roller-ecu-1.0.5.recu
```

主机网络和升级：

```sh
sudo ./tools/configure_ecu_network.sh
python3 tools/ethernet_ota.py --status
python3 tools/ethernet_ota.py \
  artifacts/firmware/1.0.5/roller-ecu-1.0.5.recu
```

也可在 `./tools/ecu_debug_ui.py --ecu-ip 172.16.0.11` 的固件升级页选择 `.recu`。
升级完成后必须等待 reset，再核对 status 的 `state=0`、`ota_result=0`、
`accepted_sequence`，ATECC auth、零输出状态和网络时延。

`--stop-after-bytes` 仅用于受控台架的掉电/中断恢复试验，不能出现在生产 SOP。

## 6. Provision、备份与 CLOSED 门槛

当前板 OEMiROT/OBKeys 已 provision，JP1 必须保持断开。OPEN 状态只读检查使用：

```sh
STM32_PROGRAMMER_CLI=/home/plac/.local/share/stm32cube/bundles/programmer/2.22.0+st.1/bin/STM32_Programmer_CLI \
  ./tools/provision_oemirot_open.sh inspect
```

首次 provision 的 JP1/BOOT0 分阶段命令只用于未配置的新板，并会整片擦除；不得
对当前板重新执行。当前板迁移前完整备份位于：

```text
artifacts/device-backups/
stm32h563-066BFF565456857187210935-20260828T065639Z-before-oemirot-*
```

PKI 工作副本位于 `/home/plac/.local/share/roller-ecu-pki`，目录权限 0700、文件
0600，且被排除在 Git 之外。CLOSED 前必须由密钥负责人把以下私钥材料备份到至少
两份独立、加密、离线介质，并实际演练读取公钥/签名验证：OEMiROT Secure/NS
认证私钥、image encryption 私钥、OTA transport 私钥、DA root/intermediate/leaf
私钥及其 passphrase。仓库中的 OBK、公钥证书和 hash manifest 不能替代私钥备份。

CLOSED 转换必须同时满足：

1. 本文全部 build/audit/test 和 Ethernet OTA 正向、篡改、中断、防重放、防回滚
   用例在目标板通过；阀体缺货不阻塞启动链测试，但必须记录带载功能仍待验收。
2. 已把 `ReleaseClosed/ECU_OEMiROT.bin` 写入 boot，在仍为 OPEN 时完成 verify，
   且所有 primary/secondary/persistent 数据已备份。
3. 离线 PKI 至少双份、校验值和恢复演练有签字记录。
4. 目标探针序列、MCU UID、ATECC serial、整机序列和固件 hash 经双人复核。
5. 用户明确授权本块板执行不可逆 CLOSED 转换。

本工程当前没有自动执行 CLOSED 的命令，避免把“构建完成”误变成硬件不可逆授权。
在上述条件全部确认前不得手工写 PRODUCT_STATE。转换后应验证普通 ST-Link 无法
读取 Flash、Ethernet OTA 仍可升级，以及 DA 只能执行 Full Regression、不能重开
保留数据的调试。

## 7. 本轮实板验证证据

2026-08-29 对 probe `066BFF565456857187210935` 完成：

- 最终 `1.0.4` 包 SHA-256：
  `405bc9eca5c87439ad2a7ac4ddf24cc7b6d34e94824797eb58fb136fbe8c4c4d`；
- Secure/NonSecure transport SHA-256 分别为
  `8ab152dba70bfc7fa8f64ef44178a9c2894fb64e0cd4e82c137706a7cc6dd86a`、
  `f0deea27ef73ff8dcfe327912d056357a48b417110fd5e19da77f34b953963d8`；
- 完整 OTA、65536 B 中断恢复、篡改拒绝、防重放和较低 security counter 防回滚
  均通过；最终 accepted sequence=4；
- 软件复位后 OTA state=IDLE/result=0，ATECC auth=0，全部 relay/PWM=0，PI 调参
  未启动时 telemetry frames=0；
- 100 包 0% 丢包，RTT min/avg/max=`0.071/0.113/0.247 ms`；
- host 协议测试 9 项、配置审计和 diff whitespace 检查通过；ReleaseOpen 与
  ReleaseClosed 均构建通过，ReleaseClosed 因当前硬件仍 OPEN 未在板运行。

剩余硬件项是阀体到货后的真实电流闭环、故障注入和标定，以及经明确授权后的
CLOSED gate；两者不得在报告中误写为已完成。
