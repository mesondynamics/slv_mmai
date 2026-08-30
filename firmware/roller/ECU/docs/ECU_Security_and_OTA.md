# ECU 启动安全、ATECC608C 配对与 Ethernet OTA

## 1. 当前结论和安全边界

本工程已经形成 STM32H563 OEMiROT → Secure application → NonSecure application
的验证启动链，并将当前板载 ATECC608C 与本 MCU 做一对一配对。签名、加密、
防回滚和 Ethernet OTA 已在实板验证。

当前样件曾在历史 CLOSED 验收后执行 Full Regression 返回 OPEN，现已通过
不可变事务
`artifacts/device-backups/stm32h563-066BFF565456857187210935-20260830T063129Z-closed-transition/`
再次完成 ReleaseClosed 写入和 `PRODUCT_STATE=0x72` CLOSED 验证。事务 UUID 为
`0f3537fe-c04d-4298-ba50-995773e07a6a`，最终 phase 为
`product_state_closed_verified`。在保持 CLOSED 的情况下，样件已通过 Ethernet
安装唯一签名的 `1.0.16+0` Secure/NonSecure 成对发布，security counter=16，
OTA `accepted_sequence=16`。1.0.16 已完成普通未认证读取拒绝、permission `c`
DA 临时调试、Secure/NonSecure primary 精确回读、close-debug、关闭后的再次
未认证读取拒绝，以及完整移除 ST-Link 后的最终仅 ECU 冷启动；生命周期和
provisioning integrity 在 DA 前后均严格复核为 CLOSED/VALID。
离线 PKI 已完成双备份；私钥、口令和可解密备份均不得进入仓库或随
ECU 交付。

Debug Authentication 使用产品自有的三层 P-256 证书链和最小权限 `0x4040`：
`bit6` 允许工程师认证后临时打开 HDPL3 Secure+NonSecure 应用调试，`bit14`
允许 Full Regression 回到 OPEN。没有授权 HDPL1/HDPL2 调试或 Partial Regression，
因此售后调试不能越过 OEMiROT 的 HDP 边界。临时调试跨 reset 保持、断电或显式
close-debug 后失效；Full Regression 则必然擦除全部用户 Flash、OBKeys 和安全
存储，不是保留固件的生命周期切换。

## 2. 启动信任链

1. STM32 RSS 读取 HUK 封装的 OEMiROT OBKeys，唯一启动入口固定为
   `0x0C000000`；boot 受 WRP/HDP 保护。
2. OEMiROT 校验 Secure 和 NonSecure 两个 MCUboot primary 镜像的独立 ECDSA
   P-256 签名、版本依赖和 security counter。secondary 镜像在 Flash 中加密。
3. STM32H563 Rev X 在 OEMiROT HDPL1 下直接读取 engineering-information UID、
   RSS descriptor 或 DBGMCU 并不可靠，可能返回全零或 fault。因此 OEMiROT **不做**
   所谓“实时 DBGMCU 双采样”：它使用构建时固定并经 OPEN 制造流程复核的 RSS
   manifest（IDCODE=`0x10076484`、SFSP=`2.5.0`）和单板制造身份，并通过位于
   `0x3003FBC0` 的专用 Secure SRAM handoff 传给 Secure 应用；记录带字段反码和
   CRC-32C，linker 防止栈覆盖。执行不可逆 CLOSED 前，Programmer 从
   `0x08FFF800` 物理读取 12-byte UID，必须精确等于
   `003800613434511232383537`，并写入不可变事务 manifest。真正的防移植根是
   芯片 HUK 封装的 OBKeys、CLOSED/HDP 内的 ATECC 配对记录及 ATECC 不可导出私钥，
   不是可公开的 UID 或构建时 RSS 常量。
4. Secure 应用探测 ATECC608C，核对配置 CRC、I²C 地址、serial、revision、
   Config/Data/私钥 slot 锁状态、制造身份及 CLOSED/HDP 保护区中的 MCU 配对记录。
   随后由安全芯片不可导出的私钥对 TRNG 随机 challenge 签名，STM32 使用配对时
   固化的公钥验证。配对记录使用 A/B 两个独立 Flash sector；运行时扫描合法
   commit/CRC 并选择有效记录继续真实密码学认证，而进入 CLOSED 前的制造门禁
   进一步要求两份都精确匹配本板身份且字节一致。UID 不是秘密，也不是防移植的
   唯一信任因子。
5. 任何身份检查失败时，诊断和受签名 OTA 恢复通道仍可运行，但执行器 ARM 被
   Secure 域拒绝，全部 TPIC/PWM 输出保持安全态；OTA test image 也不会被确认。
6. Secure 内部读取两个 primary trailer 的 `image_ok`：两者均为 `0x01`
   才是已确认，任一为 `0xFF` 则通过既有 `SECURE_SafetyGetStatus()` 的
   `SAFETY_STATUS_OTA_UNCONFIRMED` (bit 25) 向 NonSecure 给出正向指示；其他
   flag 值按完整性故障处理。运行状态查询没有增加新的 NSC query API。
7. OTA test-swap 镜像只有在 ATECC 认证的 Secure 安全服务、ADC、CAN、
   速度处理和 LwIP 均已初始化，且固定地址 0 的 LAN8742A 返回预期
   PHY ID 后才进入确认。NonSecure 启动门禁和 Secure watchdog 均对未确认
   镜像实施不可延长的 5 s 上限；等待期间 UDP 控制、调参和 OTA 均不进入
   operational 状态。超时或确认失败时撤销输出、停止喂 IWDG，由
   watchdog reset 进入 OEMiROT 回滚。已确认镜像不进入这个回滚路径。

PHY 启动门禁要求 MDIO 上的地址/ID 校验及 BSR/BCR 读取有效；合法的
link-down 或自协商未完成状态已能证明管理通道可用，因此不要求插入网线。
250 ms 轮询可在 5 s 窗口内恢复初次探测失败或短暂 MDIO 读错，但不会再次
翻转 PB14 PHY reset，也不能恢复 PHY 硬锁死、50 MHz REFCLKO 丢失或需要完整
重建 ETH HAL/DMA 的故障。未确认镜像遇到这类故障必须回滚；已确认镜像则
保持网口 down 和输出安全，安全主循环不因此复位，硬恢复需要受控 MCU
复位或整机掉电。

当前样件实测身份：

| 项目 | 值 |
|---|---|
| MCU UID | `003800613434511232383537` |
| ATECC serial | `0123d47eb2ee0e9bee` |
| ATECC revision | `00006005` |
| ATECC config CRC-32C | `0xEBB326F3` |
| ATECC 锁状态 | Config locked、Data locked、private key slot 2 locked |
| Pairing generation | 1 |

R1 PCB 因 PB8/PB9 网络交换，使用 Secure 开漏软件 I²C：PB8=SDA、PB9=SCL。
普通 stuck-low recovery 之外，Secure 启动会无条件执行 Microchip host-reset
同步序列 `START → SDA 释放为高的 9 个 SCL → START → STOP`，再用 word address
`0x00` reset 内部地址状态、`0x01` sleep 清理易失状态。若 MCU reset 时 ATECC
仍 busy/awake，则以 25 ms 步长、最长 725 ms 的硬预算重试，并在每次尝试服务
已经运行的 IWDG；超出预算仍保持 quarantine，绝不跳过真实 P-256 challenge。
下一版 PCB 必须恢复
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

OEMiROT 签名应用的 NSC 窗口固定为 `0x0C05DC00..0x0C05DFFF`。
`Secure_nsclib/secure_nsc_abi_v1.s` 保存固件 1.0.12 已交付导入库的 veneer
地址（函数指针值含 Thumb bit），签名 Secure 构建通过 `--in-implib` 锁定既有
地址。例如 `SECURE_SafetyGetStatus()` 保持在 `0x0C05DC59`，既有的
`SECURE_SafetyOtaConfirmRunningImages()` 保持在 `0x0C05DC89`。本轮只在
`GetStatus` 返回的 32 bit 保留位中定义 `SAFETY_STATUS_OTA_UNCONFIRMED`，
没有添加 `SECURE_SafetyOtaRunningImagesConfirmed` 之类的 NSC veneer。开发直连镜像使用
另一 NSC 布局，不是 OTA 兼容性基准。

确认服务在写任一 flag 前先获取 Secure 和 NonSecure 两个 Flash controller，
对每个 16 B Flash program unit 都执行 cache 失效和回读，严格按
**NonSecure `image_ok` 先、Secure `image_ok` 后**的顺序写入。但两个 Flash program
unit 无法实现硬件原子写，因此 OEMiROT 的派生 loader 把 Secure/NonSecure 作为
一个成对发布单元处理。两个 controller 均解锁后，写入成功或失败的所有可返回路径
都会重新锁定它们；`HAL_FLASH_Lock_S()` 或 `HAL_FLASH_Lock_NS()` 任一失败时，公共 Flash
guard 执行 `DSB` 后立即 `NVIC_SystemReset()`，不会让应用在 controller 可能仍解锁的
状态下继续运行。硬件复位重新建立 Flash 锁定状态，随后仍由 OEMiROT 重新判定这次
完整确认或半确认状态。

成对 OTA **只允许 TEST swap**。派生 loader 在执行 swap 前拒绝任一
`BOOT_SWAP_TYPE_PERM`，也拒绝同时出现 TEST/REVERT 的矛盾状态，两者都以
`BOOT_EBADIMAGE` fail closed。`secure-initial.bin`/`nonsecure-initial.bin` 是制造阶段直接
安装的已确认 primary，不是 PERM OTA；不得用手工 `imgtool` 生成 PERM
secondary 规避该策略。

PERM 拒绝覆盖 clean 和 interrupted 两种入口。尚未开始的 clean PERM 在常规
swap-type 策略中以 `BOOT_EBADIMAGE` 拒绝；已经留下 partial-swap 状态的 interrupted
PERM 则在 `boot_prepare_image_for_update()` 的恢复分支内直接检查 `bs->swap_type`，
并在 `boot_review_image_swap_types()`、`boot_complete_partial_swap()` 及任何新的恢复
Flash 写入之前进入 `FIH_PANIC` fail closed。任一侧或两侧同时存在 partial PERM 都
不能先完成恢复、擦除 PERM 证据并变成 `NONE`，因此不能绕过 TEST 的应用确认门或
触发单侧 NV counter 提交。

派生 loader 保留上游依赖检查的优先级：先完成 `boot_verify_dependencies()` 及其
取消/解析，再进入目标配对门。Ethernet staging 中途掉电可能只让一侧完整 secondary
暴露 TEST trailer；该状态必须先由依赖规则取消，使已确认旧配对仍可启动，不能把
目标配对门提前到依赖检查之前。依赖解析后，如果断电续做使两侧成为
`NONE/TEST` 或 `NONE/REVERT`，目标门对 `NONE` 侧选择 primary、对 TEST/REVERT 侧
选择 secondary，分别完成签名/哈希验证并读取完整发布身份
`(major, minor, revision, build, protected-TLV security_counter)`；两侧身份不一致即
`BOOT_EBADIMAGE`，不执行后续 swap。

每个发布的成对身份为两个已验签 primary 共享的
`(image version, protected-TLV security_counter)`。半确认和回滚中断的派生规则为：

| 启动状态 | primary 成对身份 | OEMiROT 动作 |
|---|---|---|
| 两侧 `image_ok=0xFF` | 新 S/NS 身份相同 | 普通未确认 TEST，看门狗复位后回滚 |
| 仅一侧 `image_ok=0x01` | 两主槽身份元组相同：version 相同且签名 counter 相同 | 识别为同一发布的 `NONE/REVERT` 半确认；先验证两个旧 secondary，再强制两侧 `REVERT` |
| 成对回滚已完成一侧后掉电 | 两主槽身份元组不同：version 或签名 counter 至少一项不同；合法的连续发布两项都不同 | 识别为回滚续做，不再将两侧重新改为 `REVERT`；保留 MCUboot 状态，只完成剩余一侧 |
| 两侧 `image_ok=0x01` 且 header 有效 | 新 S/NS 身份相同 | 成对确认完成，才允许后续推进两侧硬件防回滚计数 |
| 未确认 TEST 且两个 rollback secondary 均损坏 | 新 S/NS `image_ok` 未双确认，swap 可退化为 `FAIL/FAIL` | 在任何 `image_ok` 合成前 FIH fail closed；不推进 NV counter，不进入应用 |
| 已确认 trusted pair 遇到损坏 staging | 双 `image_ok=0x01`，本次双侧完整验签且身份相同 | 允许幂等 FAIL 清理；只能重写已为 SET 的 flag，不能创造新的应用健康结论 |

`image_ok` 判定必须是两侧都**精确等于** `BOOT_FLAG_SET (0x01)`，不再使用
上游的“不等于 UNSET”宽松条件；`BOOT_FLAG_BAD` 或无效 header 都 fail closed。
该条件同时门禁正常硬件 counter 更新和 fault-injection countermeasure 路径，
所以半确认时两个硬件 counter 都不会提前增长，旧的成对镜像仍然符合
防回滚条件。两侧确认后，每个硬件 counter 可分别更新并在需要时复位续做；
每次续做仍要求双 `image_ok` 和有效 header，不会退回单镜像宽松提交。

首次和冗余 countermeasure 两条 NV counter 路径在任一
`boot_update_security_counter()` 之前都必须同时满足：两个 primary trailer 的
`image_ok` 精确为 `0x01`；两个 primary 在**本次启动**均已完成 MCUboot 全镜像
密码学验证（包括启用时的 double-sign 状态检查）；两个镜像均能读取完整
`(major, minor, revision, build, protected-TLV security_counter)`，且身份完全一致。
首次路径在提交前主动重新验签两侧 primary；冗余路径使用本次最终 primary 验签
记录并在各次 counter 写入前再次读取 trailer。任一 trailer/TLV 读取、验签、
double-sign 计数或身份一致性检查失败都 fail closed，Release 构建也不依赖
会被 `NDEBUG` 移除的 `assert()`。因此仅伪造确认 flag、partial PERM 或单侧有效镜像
都不能推进任一硬件防回滚计数。

`BOOT_SWAP_TYPE_FAIL` 本身也不是确认来源。上游 FAIL 清理会调用
`swap_set_image_ok()` 以避免再次处理坏 secondary；若不加门禁，未确认 TEST 的两个
rollback secondary 同时损坏并形成 `FAIL/FAIL` 时，loader 可能替应用合成两个
`image_ok`，随后误推进 NV counter。派生 loader 将 swap 前的 trusted-pair 授权以
FIH 编码状态保存，默认是 `FIH_FAILURE`；只有在当次 swap loop **之前**已经满足
双 `image_ok=0x01`、双侧当次完整密码学验证和完整发布身份一致时才置为
`FIH_SUCCESS`。每个 FAIL 分支都在 `swap_set_image_ok()` 之前检查该授权；未确认、
半确认、验签/读取失败或身份不一致均以 `BOOT_EBADIMAGE`/`FIH_PANIC` 停止，原 flag
和两个 NV counter 不得改变。只有上述已确认 trusted pair 可以清除损坏 staging；
此时写 `image_ok` 是幂等操作，不能把一个未测试发布转换为已确认发布。

回滚续做的身份判据必须同时包含 primary header version 和从已签名
protected TLV 取出的 `security_counter`。因此每个新发布的 version 和 counter
都必须在全部已签发布历史中唯一且单调递增，S/NS 必须使用同一组值。
任一值重用都可能使“已回滚一侧”与“同发布半确认”发生别名，破坏续做判断。
1.0.12 NSC ABI 固定仍是掉电过渡态的纵深防御，但成对 loader 不会把混合
镜像当作正常终态启动。

所有更新/回滚动作结束且 MCUboot 已完成两个 primary 的密码学验证后，派生 loader
在选择 image 0 交给应用之前还执行一次**最终 primary 完整身份门**。该门重新从两个
primary 读取上述完整 version 元组和受签名保护的 `security_counter`，并使用
FIH 编码的相等性结果作控制流判定；读取失败或任一字段不一致均以
`BOOT_EBADIMAGE` 停止启动。这是掉电续做、secondary 损坏导致状态降级或其他异常
路径的最终纵深防线，任何 old/new 混合 primary 都不得交给 Secure/NonSecure 应用。

## 4. OTA 包与传输流程

发布包 `*.recu` 包含 metadata、固定 128 B manifest、64 B raw P-256 签名及成对
的 Secure/NonSecure 加密 MCUboot 镜像。host 工具先验证成员集合、尺寸、SHA-256
和 OTA transport 签名；ECU Secure 域再次执行同样验证。OEMiROT 在 reset 后用
另一组 root key 再验证两个内部镜像，传输签名私钥泄露不会直接绕过 secure boot。
S/NS 镜像互相声明对同一 version 的 dependency，使用同一个签名
`security_counter`，secondary trailer 只能请求 TEST swap。

UDP 50006 只接受源地址 `172.16.0.10`。BEGIN 会立即释放全部输出并进入 quarantine；
CHUNK 使用 512 B payload、CRC-32C、连续 offset 和 16 B Flash 编程粒度，重复的
已写 chunk 只有内容完全一致才允许恢复；FINISH 核对整镜像哈希和 trailer 后以
schema-2 journal 原子保存接受序号并 reset。journal 最终 quadword 同时保存 commit、
反码、generation 和 sequence 副本；写后会失效 ICACHE 再回读，避免 STM32H5 对
刚擦写 Flash 的缓存旧值造成假失败。

MCUboot 的完整 secondary 镜像本身带 trailer magic，因此“完整传完但未 FINISH”
后再复位，OEMiROT 仍可能评估并试启动这个已签名镜像；FINISH 是 transport 防重放
journal 的提交点，不是唯一的 swap 触发点。不完整或被篡改的镜像不能通过 OEMiROT
签名/哈希验证。BEGIN 至结束期间执行器保持 quarantine，生产升级工具必须完成
FINISH 并核对 `accepted_sequence`，不得把“只传输不 FINISH”作为正常流程。

`update_sequence` 是 ECU transport journal 的单调防重放序号；`security_counter`
是签名镜像中的发布身份及 OEMiROT 硬件防降级计数的输入。两者必须独立
递增，version 与 `security_counter` 一经签发就永不复用，即使包没有安装或后来被撤销。
签名有效但 counter 过低的包仍可能消耗 transport sequence，发布系统必须再用更大的
version、counter 和 sequence 生成新发布，不得覆盖原发布。

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

| 发布 | security counter | update sequence | 状态 |
|---|---:|---:|---|
| `1.0.12` | 12 | 12 | 已签发 NSC ABI 基线 |
| `1.0.13` | 13 | 13 | **REVOKED / 禁止刷写**；在成对回滚与硬件 counter 门禁定稿前生成，只保留作审计证据 |
| `1.0.14` | 14 | 14 | 已安装并完成 OPEN loader 替换后的历史无探针冷启动基线；身份已消耗，不得复用 |
| `1.0.15` | 15 | 15 | 已安装、双确认并完成 fresh audit；当前 CLOSED 不可变转换与 schema-v4 Full Regression recovery 基线，身份已消耗不得复用 |
| `1.0.16` | 16 | 16 | 已唯一签名并经 CLOSED Ethernet OTA 安装；成对 TEST swap/确认、runtime 和一次无探针冷启动 PASS，身份已消耗不得复用 |

`1.0.13` 不得通过 Ethernet OTA、ST-Link、factory initial image 或任何恢复流程
写入 ECU，也不得修改内容后重新使用 version 1.0.13 或 counter 13。打包工具在
编译和签名前扫描 `artifacts/firmware/*/metadata.json`，任一已用 version、已用
counter、不完整/不可信发布历史或指向旧发布的 output directory 都 fail closed；
`--force` 不能绕过身份门禁。`1.0.15` 仍是不可变 CLOSED 转换和恢复
基线，禁止重新打包、改写或重放。当前目标已接受 sequence16，`1.0.16`
身份同样已消耗，不得通过改写内容或任何绕过手段复用。

```sh
sha256sum artifacts/firmware/1.0.15/roller-ecu-1.0.15.recu
sha256sum artifacts/firmware/1.0.16/roller-ecu-1.0.16.recu
```

固定的 1.0.15 转换/恢复基线包 SHA-256 为
`9980acbf248f242c77c7044f48d627ca862d6714221f5835c0a21b7a24bf9e54`。已唯一签名并安装的
1.0.16/counter16/update-sequence16 包 SHA-256 为
`3e5ab07c7b6127dcedb46a1b4b1a695740904dd940d7128631f714d959a0b623`。

主机网络和升级：

```sh
sudo ./tools/configure_ecu_network.sh
python3 tools/ethernet_ota.py --status
# 1.0.16 的已完成历史命令；当前 accepted_sequence=16，禁止重放
python3 tools/ethernet_ota.py \
  artifacts/firmware/1.0.16/roller-ecu-1.0.16.recu
```

也可在 `./tools/ecu_debug_ui.py --ecu-ip 172.16.0.11` 的固件升级页选择 `.recu`。
1.0.16 传输、reset/test swap 和应用确认已完成；最终 OTA 为
`state=IDLE/result=0/ota_result=0/accepted_sequence=16`，全部 session 字段为 0。
post-OTA ATECC/MCU/配对身份、无 quarantine、零输出和调参关闭均 PASS；详细的
无探针冷启动、网络、29 项回归和 telemetry 结果见第 8.3 节。

`--stop-after-bytes` 仅用于受控台架的掉电/中断恢复试验，不能出现在生产 SOP。

售后 DA 命令：

```sh
# CLOSED 设备只读发现；列出实际允许的服务
python3 tools/ecu_debug_auth.py discover --accept-closed-target

# 临时开放 HDPL3 Secure+NonSecure 应用调试
python3 tools/ecu_debug_auth.py open-app-debug --accept-closed-target

# 维修结束立即关闭；完整断电也会恢复 CLOSED 默认调试状态
python3 tools/ecu_debug_auth.py close-debug --accept-closed-target

# 灾难恢复：整片擦除、OBKeys/安全存储失效并回到 OPEN
python3 tools/ecu_debug_auth.py \
  --accept-closed-target --accept-full-device-erase \
  full-regression-to-open
```

工具只把解密后的 leaf 私钥短暂放入 `/dev/shm`、权限 0600，完成后覆盖并删除；
工程师工作站仍必须使用全盘加密、最小账户权限和维修操作审计。Full Regression
后必须按新板流程重新 provision，旧 DHUK 包装的资产和旧安全存储不能复用。
`close-debug` 后芯片可能停留在本供电周期的 RSS-DA 服务态，维修 SOP 必须再将 ECU
完全断电至少 10 s 后上电，并验证 Ethernet、ATECC auth 和零输出；普通 NRST 不等同
于完整掉电。不要为恢复业务而再次执行 `open-app-debug`。

## 6. Provision、备份与 CLOSED 门槛

当前板为 `PRODUCT_STATE=0x72` CLOSED 状态，JP1 保持断开。CLOSED 生命
周期、provisioning integrity 和 DA 服务的只读发现使用：

```sh
STM32_PROGRAMMER_CLI=/home/plac/.local/share/stm32cube/bundles/programmer/2.23.0/bin/STM32_Programmer_CLI \
  python3 tools/ecu_debug_auth.py --accept-closed-target discover
```

### 6.1 已完成的 1.0.14 OPEN loader 替换（历史流程）

当时把新的成对策略 loader 写入 boot 时，只允许使用独立的 OPEN-only 事务；
旧的 `repair-boot-layout` journal 是历史恢复记录，不能作为本轮授权或备份。先执行
完全离线、不会连接板卡的输入预检：

```sh
./tools/provision_oemirot_open.sh preflight-open-oemirot-replacement
```

真正执行前必须保持 JP1 断开，并把 ST-Link 的 SWD、GND、VTref、**NRST** 全部接好。
显式确认操作会先用 HWRSTPULSE 和 Secure uptime 证明 NRST 确实到达 MCU，且硬复位
采样的是 BOOT0 低/正常应用启动；然后创建新的事务目录，备份 2 MiB Flash、完整
128 KiB persistent、Option Bytes、runtime safe 和 OTA 状态。工具从该新备份提取
两个 primary，以 OEMiROT 公钥分别验签，要求完整
`major/minor/revision/build/security_counter` 身份相同、boot magic 合法且两个
`image_ok` 都是精确的 `0x01`；同时由 Programmer 直接读取 MCU UID 并核对为
`003800613434511232383537`，不能只相信探针序列号。任一条件不满足时不会解除
boot 保护。

```sh
./tools/provision_oemirot_open.sh replace-open-oemirot \
  --accept-open-boot-replacement
```

写入输入会复制到事务目录并固定 size/SHA-256，避免后续构建覆盖待写文件。脚本只
临时解除 boot 的 WRP/HDP，写入并独立回读 `0x0C000000`，随即恢复并复核
`WRPSGn1=0xFFFFFFF0`、`HDP1=0x00..0x17`；primary、secondary、scratch 和
persistent 均不擦除。1.0.14 `.recu` 也复制到事务目录并纳入 manifest；后续 OTA
必须使用该固定副本，不能换回可能被并行构建覆盖的 canonical 路径。中途失败时
保持 ST-Link 和供电，按错误消息用**同一个**事务
目录恢复，不得选择“最新 journal”猜测恢复点：

```sh
ECU_OPEN_LOADER_TRANSACTION_DIR=/absolute/...-open-loader-replacement \
  ./tools/provision_oemirot_open.sh resume-open-oemirot-replacement \
    --accept-resume-open-boot-replacement
```

loader 替换完成后仍运行板上原 1.0.12 应用；该应用不含本轮 LAN8742 冷启动 30 ms
复位修复。因此此阶段有强制 embargo：**不得移除 ST-Link、不得断电冷启动**。
保持探针和 NRST 接入，直接通过当前 Ethernet 安装 1.0.14；等待复位至少 5 s，确认
`accepted_sequence=14`、双镜像已确认、ATECC/零输出/runtime safe 和延迟全部通过
后，才允许移除 ST-Link 进行无探针冷启动回归。否则极易把旧应用的 PHY 时序问题
误判为新 OEMiROT 再次失败。

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

### 6.2 1.0.15 CLOSED 门禁与受控事务流程

本板再次执行 CLOSED 转换前必须同时满足以下门禁；本次转换已按
同一不可变事务完成并保留证据：

1. 全部 build/audit/test 主机门禁通过；当前板完成正常 OTA-P01、两个 primary 的
   独立验签/完整身份/双确认审计，并在 JP1 断开、完全移除 ST-Link 后完成仅 ECU
   冷启动、ATECC/零输出和 Ethernet 延迟复核。OTA-P02..P16 的断电和故障注入属于
   绑定硬件/loader/发布 hash 的专用可恢复样件发布/型式鉴定，不在每块量产板重复
   执行；未完成项必须保持 PENDING，不能误写为当前板 PASS。
2. 本次 CLOSED 转换时的运行镜像必须精确为 `1.0.15+0`、security counter=15，OTA
   `state=IDLE/result=0/ota_result=0/accepted_sequence=15`，两个 primary 的
   `image_ok` 均精确为 `0x01`；`ReleaseClosed/ECU_OEMiROT.bin` 必须是已审发布
   产物：52292 bytes，SHA-256
   `4073a9faa15eed7ca6014f168ae91efc35ba1e26d8e74651552c9a0a3d8ef884`。
3. 离线 PKI 至少双份、校验值和恢复演练有签字记录。
4. 目标探针序列、MCU UID、ATECC serial、整机序列和固件 hash 经双人复核。
5. 用户明确授权本块板执行不可逆 CLOSED 转换。

转换使用 `tools/finalize_oemirot_closed.sh`，必须显式传入
`--accept-irreversible-closed`。脚本先从 fresh UI 状态精确核对 MCU/ATECC 身份、
config CRC、pairing generation、零输出、双确认 bit 和 OTA sequence；Programmer
再从 `0x08FFF800` 读取物理 UID。随后建立唯一、权限 0700 的事务目录，固定并哈希
ReleaseClosed、三份 OBK、完整 DA chain、公钥、Programmer 2.23.0 二进制以及 fresh
2 MiB Flash/128 KiB persistent 备份。它还从 full Flash 的物理
`0x080E0000..0x080E3FFF` 提取 16 KiB pairing store，与独立 persistent readback
逐字节比较，并用 dual verifier 分别验证 A=`0x080E0000`、B=`0x080E2000` 的
commit/CRC/固定身份及 A/B 字节一致。只有先持久化
`pairing_store_dual_verified`，才能进入 `pre_mutation_evidence_verified` 和任何
Option Byte/Flash 变更。两个 primary 必须分别通过 staged OEMiROT 公钥验签，
发布身份必须精确为 1.0.15/counter15，且 raw `image_ok=0x01/0x01`。
全部输入和 manifest `fsync` 后才允许第一次 Option Byte/Flash 写；phase journal
每次都以“完整下一前缀写临时文件 → `fsync` → 原子 rename → 目录 `fsync`”提交，
掉电后只能观察到旧或新两个合法前缀，不会留下不可恢复的半行。

脚本按 OPEN → PROVISIONING(`0x17`) → CLOSED 顺序推进；每个 phase 都先持久化
write-ahead 记录。进入 CLOSED 前必须完成三份 staged OBK 的顺序提交、同步保存
Programmer exit/log/OBK hash 证据，并在 PROVISIONING 下由 DA discovery 明确证明
`ST_LIFECYCLE_PROVISIONING` 和有效 provisioning-integrity。若断电恰好发生在
`-sdp` 已开始但同步成功证据尚未形成的窗口，脚本会 fail closed，禁止猜测重放、
禁止继续下一 OBK 或 CLOSED。

`halt` 是易失状态，phase 不能证明当前核心仍停住；因此每次 OPEN fresh/resume
都会在最终全量 Option Bytes 回读之后重新执行 halt，并紧邻写入 `0x17`。同理，
每次准备写 `0x72`（包括中断恢复重试）都必须重新执行只读 DA discovery，持久化
本次 attempt，证明当前目标仍为 PROVISIONING 且 integrity 有效，再 hard reset 并
重新证明 Device ID `0x484`、SFSP `2.5.0`、`PRODUCT_STATE=0x17`。

普通中断只能绑定错误消息给出的**精确**事务目录恢复，禁止自动选择“最新”目录：

```sh
ECU_CLOSED_TRANSACTION_DIR=/absolute/...-closed-transition \
  ./tools/finalize_oemirot_closed.sh resume-closed-transition \
    --accept-same-physical-target
```

每个 CLOSED 事务达到 `product_state_closed_verified` 后，必须立即从上述
**同一个 CLOSED 事务目录**
生成 Full Regression 恢复包。生成器只接受新版不可变事务目录，不接受历史
`SOURCE_BEFORE_CLOSED_PREFIX`；旧 v1/v2/v3 包仅保留为证据，不能用于破坏性恢复：

```sh
./tools/create_full_regression_recovery.sh \
  /absolute/...-closed-transition final-1.0.15

ECU_INITIAL_VERSION=1.0.15 \
ECU_RECOVERY_BACKUP_DIR=/absolute/...-final-1.0.15 \
  ./tools/provision_oemirot_open.sh validate-full-regression-package
```

v4 包绑定 CLOSED transaction UUID、完整 phase、Programmer 物理 UID、两个已确认
primary、2 MiB Flash、128 KiB application-owned Secure persistent 数据，以及
ReleaseOpen/ReleaseClosed、1.0.15 initial pair、`.recu` 和 manifest-bound 16 KiB
dual pairing store 的精确 size/SHA-256。
它不包含、也不能还原 HUK 封装的硬件安全存储；认证 Full Regression 后仍须从
双备份 PKI 重新生成并 provision OBKeys。执行灾难恢复前先做不访问目标 Flash 的
预检，确认目标确为 Full Regression 产生的 blank OPEN 后才允许重建：

```sh
ECU_INITIAL_VERSION=1.0.15 \
ECU_RECOVERY_BACKUP_DIR=/absolute/...-final-1.0.15 \
  ./tools/provision_oemirot_open.sh preflight-full-regression-recovery

ECU_INITIAL_VERSION=1.0.15 \
ECU_RECOVERY_BACKUP_DIR=/absolute/...-final-1.0.15 \
  ./tools/provision_oemirot_open.sh recover-open-after-full-regression \
    --accept-blank-open-rebuild
```

后续 JP1/OBKey 阶段只能继续使用恢复命令打印的精确 journal，不得重新启动普通
`provision-open`，也不得从“最新目录”猜测恢复点。

STM32H563 的 ES0565 限制使 PROVISIONING/CLOSED DA discovery 不能可靠返回 SoC ID；
该阶段只能依靠 OPEN 时 Programmer UID、不可变 manifest、同一 probe/Target ID/
SFSP/单调 phase 及操作者确认物理连续性，不能声称重新读取了 UID。CLOSED 后首次
仅 ECU 冷启动必须再通过 fresh Ethernet runtime 精确核对 MCU UID、ATECC serial/
config CRC/generation 和零输出。在上述条件全部确认前不得手工写 PRODUCT_STATE。
转换后验收必须覆盖：普通 ST-Link 不能读取 Flash、Ethernet OTA 正常、
DA discovery 列出 Full Regression 和 HDPL3 S/NS debug、临时调试能认证打开并能
显式关闭。本板已完成 CLOSED、1.0.15 基线默认拒绝/DA 服务验证、
1.0.16 实板 Ethernet OTA、精确 primary DA 回读、close-debug 及之后的最终
无探针冷启动。本轮没有执行破坏性 Full Regression。

### 6.3 本板 1.0.15 CLOSED 转换、DA 与恢复基线

- CLOSED 事务目录为
  `artifacts/device-backups/stm32h563-066BFF565456857187210935-20260830T063129Z-closed-transition/`，
  UUID=`0f3537fe-c04d-4298-ba50-995773e07a6a`。phase journal 按单调前缀完成
  `pairing_store_dual_verified`、`pre_mutation_evidence_verified`、三份 OBK
  提交、provisioning integrity 检查和 CLOSED 请求，最终为
  `product_state_closed_verified`；当前产品状态为 `0x72 CLOSED`。
- 绑定同一事务的 schema-v4 Full Regression 恢复包位于
  `artifacts/device-backups/stm32h563-066BFF565456857187210935-20260830T064244Z-final-1.0.15/`，
  schema=`roller-ecu-full-regression-backup-v4`，离线验证为 PASS。其 manifest 中
  `capture.product_state="0xED OPEN"` 仅表示**该次 CLOSED 变更前捕获的不可变
  恢复输入**，并非当前生命周期；`target.product_state="0x72 CLOSED"`
  才是该恢复包绑定的已验证目标。`1.0.15` 因此是不可变转换与恢复基线。
- 1.0.15 CLOSED 基线的 DA 实测证据位于
  `artifacts/hardware-regression/20260830T064437Z-closed-da-service/`。discovery 确认
  `ST_LIFECYCLE_CLOSED`、provisioning integrity=`0xEAEAEAEA`/VALID，并显示
  Full Regression 能力；本次使用 permission `c` 完成 HDPL3 Secure+NonSecure
  临时调试认证。认证前对 `0x0C030400` 的 256 B 普通读取被拒绝；
  认证后 Secure primary 196608 B 及 NonSecure primary 327680 B 分段回读均与
  CLOSED 事务备份一致；`close-debug` 成功后相同未认证读取再次被拒绝。
  本轮只验证 Full Regression 权限可被授权发现及恢复包可离线消费，
  **未再次执行破坏性 Full Regression**。该 DA 证据生成于 1.0.16 OTA 之前，
  继续作为不可变 1.0.15 CLOSED 转换基线；1.0.16 的发布后 DA 终验证据另见
  第 8.3 节，两者不得混用。

## 7. 2026-08-29 CLOSED 历史验证证据

2026-08-29 对 probe `066BFF565456857187210935` 完成：

- 最终 `1.0.9` 包 SHA-256：
  `c2c18673e6d370462bb64abc5dcdd3f8443813205ca5ef050a9af7bf4cbd335e`；
- CLOSED 生命周期为 `PRODUCT_STATE=0x72`，DA discovery 完整性为 VALID，证书
  ECDSA P-256；权限 `0x4040` 只开放 Full Regression 和 HDPL3 S/NS debug；
- 工程师使用 leaf 私钥和三证书链认证后，Secure/NonSecure primary 可读；显式
  close-debug 后普通 ST-Link 连接 AP1/读取 `0x0C030400` 被拒绝；
- 在 CLOSED 板上由 `1.0.8` 仅通过 Ethernet 正常升级到 `1.0.9`，传输、FINISH、
  OEMiROT test swap、应用确认均成功，最终 `state=IDLE`、`result=0`、
  `accepted_sequence=9`；journal A 为 schema 2、generation 7、sequence 9，
  commit/反码及 generation/sequence 副本全部匹配；
- 此前 journal 写后校验失败定位为 ICACHE 返回擦写前旧内容，现已在 OTA、身份和
  阀参数持久化写后失效 cache 再校验；最终 1.0.9 的升级不依赖 ST-Link；
- 完整掉电冷启动后 ATECC auth=0，Config/Data/slot 2 locked，全部 relay/PWM=0，
  PI 调参关闭且 telemetry frames=0；
- 冷启动 10 包 0% 丢包，RTT min/avg/max=`0.078/0.140/0.186 ms`；此前最终门禁
  100 包也是 0% 丢包，RTT=`0.058/0.129/0.185 ms`；
- host 协议测试 9 项、完整配置审计、CubeMX 6.18 `.ioc` 加载校验、diff whitespace、
  签名应用和 ReleaseClosed OEMiROT 构建全部通过。

以上 CLOSED 记录是 1.0.9 阶段、Full Regression 前的历史验收证据，不代表
当前 1.0.16 运行基线；当前 `0x72 CLOSED` 状态以第 6.3 节的转换证据、
第 8.2 节的 1.0.15 不可变恢复基线和第 8.3 节的 1.0.16 OTA 证据为准。

剩余硬件项仅包括阀体到货后的真实电流闭环、故障注入和标定，以及整车端触点、
传感器、CAN、EMC/环境/耐久验收；这些项目不得在报告中误写为已完成。

## 8. 2026-08-30 OPEN→CLOSED 回归证据

### 8.1 OPEN loader 替换与 1.0.14 历史基线

- OPEN-only 不可变 loader 事务
  `stm32h563-066BFF565456857187210935-20260830T050808Z-open-loader-replacement`
  已完成固定输入、fresh 备份、物理 UID、双 primary 验签、ReleaseOpen 写后回读和
  WRP/HDP 恢复；ReleaseOpen SHA-256 为
  `5fdaaa3f5e2d2a6c01ed527282ba08b3d8d5e579c61ec3c4e097cdf2c0fd7761`；
- 使用事务内固定 1.0.14 包（SHA-256
  `a052f51a7ef14bbbec57f412950edad7f44f92a2323227d74c606d8dd866324d`）完成 Ethernet
  OTA，最终 `state=IDLE`、`result=0`、`ota_result=0`、`accepted_sequence=14`；
- Flash fresh audit 证明 Secure/NonSecure 分别通过对应 OEMiROT 公钥验签，完整
  identity 均为 `1.0.14+0`/counter14，且 `image_ok=0x01/0x01`；
- 明确阀线圈未接入后，V2 继电器/目标/PWM/换向/仲裁/超时回归 29/29 通过；这不构成
  真实阀电流闭环或车端触点验收；
- 100 包延迟在 OTA 后空闲为 `0.057/0.128/0.183 ms`，29 项动作后空闲为
  `0.057/0.133/0.186 ms`，PI 1 kHz 调参遥测开启时为
  `0.063/0.129/0.185 ms`（均为 min/avg/max、0% 丢包）；调参退出后累计发送
  406 帧、丢样 0，遥测已停止；
- 连续三次保持 JP1 断开、完整移除 ST-Link，ECU 断电至少 10 s 后仅 ECU 冷启动，
  每次均通过 fresh MCU/ATECC 身份、OTA IDLE/result=0/`accepted_sequence=14`、
  双确认和零输出门禁；三次 100 包均为 0% 丢包，RTT min/avg/max/mdev 分别为
  `0.051/0.118/0.192/0.036 ms`、`0.063/0.123/0.247/0.040 ms` 和
  `0.064/0.143/0.194/0.028 ms`。

以上 1.0.14 loader 替换、三次无探针冷启动和当时的网络数据均保留为历史证据，
不得改写成 1.0.15 的实测结果。

### 8.2 1.0.15 CLOSED 转换与恢复基线

- 使用 `roller-ecu-1.0.15.recu`（SHA-256
  `9980acbf248f242c77c7044f48d627ca862d6714221f5835c0a21b7a24bf9e54`）完成成对
  TEST swap 和确认；当时为 `1.0.15+0`、counter15、OTA
  `state=IDLE/result=0/ota_result=0/accepted_sequence=15`。post-swap fresh audit
  精确核对两侧 installed-format header/TLV/plaintext payload/trailer，并确认
  `image_ok=0x01/0x01`；
- 配对冗余修复前，A sector（物理 `0x080E0000`）有效而 B sector（物理
  `0x080E2000`）全擦除。修复只补写并回读 B，A 前后 SHA-256 不变；修复后 A/B
  均为同一 generation-1 committed identity 且字节一致。16 KiB dual store
  SHA-256=`45f1f08b626f3934f2359e4d44b1993c9333c5275f62870d74a77caeb6c9f310`；
- ATECC startup 现在先执行无条件 `START + 9 clocks + START + STOP`，再做
  reset/sleep 归一化；busy/POST 恢复以 25 ms 步长限制在最长 725 ms，并在每次
  尝试服务 IWDG。实板以 option-byte display/reset → under-reset UID read → final
  reset 的 hostile 次序连续执行 20 次，ATECC 持续上电，20/20 均恢复真实配对认证、
  fresh runtime、accepted15、零输出和 Ethernet；证据目录为
  `artifacts/hardware-regression/20260830T061938Z-atecc-reset-recovery/`；
- 1.0.15 阶段再执行无阀线圈模式功能回归为 29/29；继电器、目标、PWM 互斥、
  安全换向、仲裁与超时释放通过，**真实阀线圈电流闭环仍为 SKIP**；
- PI 高速遥测只随调参 session 开启：telemetry frames 从 `0 → 175 → 175`，退出后
  不再增长，dropped samples=0。调参时 100 包 0% 丢包，RTT min/avg/max=
  `0.059/0.114/0.290 ms`；退出后的空闲值为 `0.059/0.127/0.185 ms`；
- CLOSED 不可变事务 UUID=`0f3537fe-c04d-4298-ba50-995773e07a6a`已在
  `product_state_closed_verified` 结束；dual pairing 门禁已在首次目标写入前实际
  执行，当前生命周期为 `0x72 CLOSED`。
- Full Regression recovery 已生成 schema v4/1.0.15 实物包，目录为
  `artifacts/device-backups/stm32h563-066BFF565456857187210935-20260830T064244Z-final-1.0.15/`，
  与上述 UUID、exact ReleaseOpen/ReleaseClosed、1.0.15 pair/package 和 dual pairing
  evidence 绑定，离线验证 PASS。manifest 的 OPEN capture 是转换前恢复输入，
  不是当前生命周期。
- `artifacts/hardware-regression/20260830T064437Z-closed-da-service/` 证明普通读取
  默认被拒绝、permission `c` 认证后 Secure/NonSecure primary 回读与事务一致，
  且 close-debug 后未认证读取再次被拒绝。

### 8.3 1.0.16 CLOSED Ethernet OTA、DA 与冷启动实测

- 唯一签名的 1.0.16/counter16/update-sequence16 包 SHA-256=
  `3e5ab07c7b6127dcedb46a1b4b1a695740904dd940d7128631f714d959a0b623`。在保持
  `0x72 CLOSED` 期间仅通过 Ethernet 完成传输、reset、成对 TEST swap 和确认；
  最终 `state=IDLE/result=0/ota_result=0/accepted_sequence=16`，全部 session 字段为 0。
- post-OTA runtime 通过 MCU、ATECC、pairing 和 no-quarantine 检查，继电器/PWM
  零输出、调参关闭、OTA IDLE 均 PASS。
- OTA 前和 OTA 后各执行一次无阀线圈模式台架回归，均为 29/29 PASS；
  两路真实阀线圈未接入，因此真实电流跟踪仍为 SKIP。
- 调参高速 telemetry 门控 PASS：inactive frames=0，active 增长到 681、
  dropped=0，关闭后仍为 681。active 时 100 包 0% 丢包，RTT min/avg/max=
  `0.064/0.138/0.340 ms`；返回 idle 后为 `0.055/0.104/0.148 ms`。
- 完整移除 ST-Link 并使 ECU 断电至少 10 s 后的一次仅 ECU 冷启动 PASS；
  100 包 0% 丢包，RTT min/avg/max=`0.067/0.116/0.328 ms`，同时通过上述
  runtime 身份、no-quarantine、零输出和调参关闭检查。
- 1.0.16 发布后 DA 终验证据位于
  `artifacts/hardware-regression/20260830T073110Z-closed-ota-1.0.16-da-service/`。
  认证前普通读取返回 rc=1 并被拒绝；严格 discovery 精确确认 target ID=`0x484`、
  SDA=`2.4.0`、`ST_LIFECYCLE_CLOSED` 和 provisioning integrity=VALID；随后
  permission `c` 认证 PASS。认证后回读 Secure `0x0C030000/0x30000` 和分段回读
  NonSecure `0x08100000/0x50000`，`tools/verify_closed_ota_readback.py` 验证两者
  均为 `1.0.16+0`/counter16、`image_ok=0x01/0x01`。swap size 按“上一版 primary
  与本版 secondary 取大值”的 canonical 规则核对：Secure=`0x2DDE8`；NonSecure
  为 `max(1.0.15 0x8D39, 1.0.16 0x8D38)=0x8D39`。审计文件
  `primary-readback-audit.json` SHA-256=
  `034c7d99996eed055346e315c3109ba355044051f53a679771b15db79ffbc4b8`；整项终验
  `evidence.json` SHA-256=
  `1478a4ac2c8748e585a86756f16a7fe6384a80f3b633196b023e23667347ca6e`。
- `close-debug` PASS，随后严格 discovery 再次确认 CLOSED/VALID，普通未认证读取
  再次返回 rc=1 并被拒绝。本轮**未执行 Full Regression**。
- DA 关闭后完整移除 ST-Link，ECU 断电至少 10 s 再仅 ECU 上电，最终无探针冷启动
  PASS：MCU/ATECC/pairing 身份正确、无 quarantine、全部 relay/PWM 为零、
  `tuning_active=false`、telemetry frames=0；OTA 为
  `state=IDLE/result=0/ota_result=0/accepted_sequence=16`。100 包 0% 丢包，
  RTT min/avg/max=`0.060/0.116/0.331 ms`。

阀体带载闭环和故障注入、实际车辆继电器负载、CAN/传感器以及
电源/EMC/环境/热/耐久仍为 PENDING，不得标记为 PASS。
