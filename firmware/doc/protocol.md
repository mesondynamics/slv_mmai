# CCU 通信协议

## 1. 协议概述

本协议定义了 PC 与 CCU 之间通过多种物理链路（TCP/IP、CANBUS、蓝牙）进行数据通信的帧格式、命令、参数同步、时间戳同步及各类数据的具体内容，确保多平台互通与数据一致性。

---

## 2. 通信层规范

### 2.1 通信物理层
- **TCP/IP**：使用以太网/无线网络，基于Socket通信。
- **CANBUS**：标准/扩展帧均可，ID 分配详见下节。
- **蓝牙**：基于串口透传（SPP）或BLE自定义服务。

### 2.2 数据封装格式

所有链路下，数据包格式统一：

| 字段     |  长度  | 说明                                           |
| :------- | :----: | :--------------------------------------------- |
| 帧头     | 2 字节 | 固定 0xAA 0x55                                 |
| 帧类型   | 1 字节 | 见 [4] 帧类型定义                              |
| 数据长度 | 2 字节 | 单位字节数，不含帧头、帧类型、数据长度和校验   |
| 数据内容 | N 字节 | 参考各数据类型定义                             |
| CRC16    | 2 字节 | CRC16-CCITT 校验，覆盖帧类型+数据长度+数据内容 |

- **CANBUS** 可用多帧分包，包头以主ID区分，帧序号区分分包。
- **TCP/IP、蓝牙** 长帧时直接按上述结构发送。

---

## 3. 时间戳同步流程

协议实现如下：

1. **PC→CCU**：发送时间同步请求帧，数据内容为 $T_{PC1}$（uint64_t，us）。
2. **CCU→PC**：CCU 记录原PC时间戳 $T_{PC1}$（uint64_t）、接收时CCU时间戳 `T_CCU1`（uint64_t）、发送时CCU时间戳 `T_CCU2`（uint64_t）；发送时间同步请求帧，数据内容为 `T_CCU2`（uint64_t，us）。
3. **PC→CCU**：PC 记录接收时时间戳 $T_{PC2}$（uint64_t）；发送时间同步请求帧，数据内容为 $T_{PC2}$（uint64_t，us）。
4. **CCU**：计算在传输回路中的总时间 $T_{trans}=T_{PC2}-T_{PC1}-(T_{CCU2}-T_{CCU1})$ -> PC发送传输时间 $T_{trans}$ -> CCU计算与PC时间戳的差 $\Delta t_{PC\_CCU} = T_{PC1} - T_{CCU1} - T_{trans}/2$，在时间上使用滑动中值滤波得到最终的时间戳差。

<img src="https://jpeg.icu/i/2025/04/24-CCU-time synchronization-d96e8f78ee8f24be71e8dd3c2c5a5b7d/6809ec64a7ff5.png" alt="CCU-time synchronization.png" style="zoom:60%;" />

---

## 4. 数据类型与帧类型定义

### 4.1 参数

所有参数支持掉电保存。

#### 4.1.1 读取参数请求

参数读取请求将设定所需读参数的帧类型，数据长度为0，数据内容为0。

例如发送获取编码器参数，将帧类型设置为 `0x01`，数据长度设置为 `0x0000`。数据包如下：

```
AA55 01 0000 CRC
```

#### 4.1.2 参数设置与读取

参数设置和读取按照 [2.2 数据封装格式] 和参数帧结构打包和解析数据。

#### 4.1.3 参数帧结构

| 帧类型 | 描述       | 数据内容                                                     | 数据长度 |
| :----: | :--------- | :----------------------------------------------------------- | -------- |
|  0x01  | 编码器参数 | uint32_t encoder_pulse_per_rev<br/>float    front_left_wheel_radius<br/>float    front_right_wheel_radius<br/>float    wheelbase<br/>float    time_factor<br/>float[6] pose_covariance<br/>float[6] twist_covariance<br/>uint8_t  raw_data_output_frequency<br/>uint8_t  odometry_data_output_frequency | 66       |
|  0x02  | GPS参数    | float   latitude_reference_point<br/>float   longitude_reference_point<br/>float   altitude_reference_point<br/>uint8_t gps_raw_data_output_frequency<br/>uint8_t gps_local_transformed_data_output_frequency | 14       |
|  0x03  | EKF1参数   | float[15] process_noise_covariance<br/>uint8_t   frequency   | 61       |
|  0x04  | EKF2参数   | float[15] process_noise_covariance<br/>uint8_t   frequency   | 61       |

### 4.2 命令

#### 4.2.1 命令数据打包

对于没有数据内容的命令与 [4.1.1 读取参数请求] 一致，按照命令帧结构操作即可。如果有数据内容的命令，按照 [2.2 数据封装格式] 和命令帧结构打包数据。

#### 4.2.2 命令帧结构

| 帧类型 | 描述         | 数据内容 | 数据长度 |
| :----: | :----------- | :------- | -------- |
|  0x30  | EKF1复位命令 | -        | 0        |
|  0x31  | EKF2复位命令 | -        | 0        |

### 4.3 数据流

#### 4.3.1 数据打包

按照 [2.2 数据封装格式] 和数据帧结构打包数据。

#### 4.3.2 数据帧结构

| 帧类型 | 描述           | 数据内容                                                     | 数据长度 | 方向   |
| :----: | :------------- | :----------------------------------------------------------- | -------- | ------ |
|  0x60  | 左轮编码器数据 | uint64_t timestamp<br/>int32_t  count                        | 12       | CCU→PC |
|  0x61  | 右轮编码器数据 | uint64_t timestamp<br/>int32_t  count                        | 12       | CCU→PC |
|  0x62  | GPS原始数据    | uint64_t timestamp<br/>uint8_t  pos_status<br/>uint8_t  ori_status<br/>double   latitude<br/>double   longitude<br/>double   altitude<br/>float    heading<br/>float    pitch<br/>float[3] position_covariance<br/>float[3] orientation_covariance | 77       | CCU→PC |
|  0x63  | GPS转化数据    | uint64_t timestamp<br/>float    x<br/>float    y<br/>float    z<br/>float    ox<br/>float    oy<br/>float    oz<br/>float    ow<br/>float[6] covariance | 56       | CCU→PC |
|  0x64  | EKF1融合数据   | uint64_t timestamp<br/>float    x<br/>float    y<br/>float    z<br/>float    ox<br/>float    oy<br/>float    oz<br/>float    ow<br/>float[36] pose_covariance<br/>float    vx<br/>float    vy<br/>float    vz<br/>float    vroll<br/>float    vpitch<br/>float    vyaw<br/>float[36] twist_covariance | 230      | CCU→PC |
|  0x65  | EKF2融合数据   | uint64_t timestamp<br/>float    x<br/>float    y<br/>float    z<br/>float    ox<br/>float    oy<br/>float    oz<br/>float    ow<br/>float[36] pose_covariance<br/>float    vx<br/>float    vy<br/>float    vz<br/>float    vroll<br/>float    vpitch<br/>float    vyaw<br/>float[36] twist_covariance | 230      | CCU→PC |
|  0x66  | 时间同步数据   | uint64_t timestamp                                           | 8        | 双向   |
|  0x67  | 激光定位数据   | uint64_t timestamp<br/>float    x<br/>float    y<br/>float    z<br/>float    ox<br/>float    oy<br/>float    oz<br/>float    ow<br/>float[6] covariance | 56       | PC→CCU |

## 5. 其它说明

- 所有数值均采用小端格式（LE）。

