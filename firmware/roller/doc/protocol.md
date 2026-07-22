# 通信协议

## 1. 协议概述

本协议定义了通过多种物理链路（TCP/IP、CANBUS、蓝牙）进行数据通信的帧格式、命令、参数同步、时间戳同步及各类数据的具体内容，确保多平台互通与数据一致性。

---

## 2. 通信层规范

### 2.1 通信物理层

- **TCP/IP**：使用以太网/无线网络，基于Socket通信。
- **CANBUS**：标准/扩展帧均可，ID 分配详见下节。
- **蓝牙**：基于串口透传（SPP）或BLE自定义服务。

### 2.2 数据封装格式

所有链路下，数据包格式统一：

| 字段     |  长度  | 说明                                              |
| :------- | :----: | :------------------------------------------------ |
| 帧类型   | 1 字节 | 见 [4] 帧类型定义                                 |
| 子序号   | 1 字节 | 根据数据内容按照8个字节长度拆分分配               |
| 数据内容 | 8 字节 | 参考各数据类型定义，不足 8 个字节补0              |
| CRC8     | 1 字节 | CRC-8 校验，覆盖帧类型+数据内容（仅串口通信需要） |

- **CANBUS** 直接使用帧类型+子序号作为ID，不使用CRC。
- **TCP/IP、蓝牙** 直接按上述结构发送，不使用CRC，每个数据包定长 10 个字节。
- **串口** 直接按上述结构发送，每个数据包定长 11 个字节。

---

## 3. 时间戳同步流程

以PC与CCU时间同步为例，协议实现如下：

1. **PC→CCU**：发送时间同步请求帧，数据内容为 $T_{PC1}$（uint64_t，us）。
2. **CCU→PC**：CCU 记录原PC时间戳 $T_{PC1}$（uint64_t）、接收时CCU时间戳 `T_CCU1`（uint64_t）、发送时CCU时间戳 `T_CCU2`（uint64_t）；发送时间同步请求帧，数据内容为 `T_CCU2`（uint64_t，us）。
3. **PC→CCU**：PC 记录接收时时间戳 $T_{PC2}$（uint64_t）；发送时间同步请求帧，数据内容为 $T_{PC2}$（uint64_t，us）。
4. **CCU**：计算在传输回路中的总时间 $T_{trans}=T_{PC2}-T_{PC1}-(T_{CCU2}-T_{CCU1})$ -> PC发送传输时间 $T_{trans}$ -> CCU计算与PC时间戳的差 $\Delta t_{PC\_CCU} = T_{PC1} - T_{CCU1} - T_{trans}/2$

<img src="https://jpeg.icu/i/2025/04/24-CCU-time synchronization-d96e8f78ee8f24be71e8dd3c2c5a5b7d/6809ec64a7ff5.png" alt="CCU-time synchronization.png" style="zoom:60%;" />

---

## 4. 数据类型与帧类型定义

### 4.1 参数

所有参数支持掉电保存。

#### 4.1.1 读取参数请求

参数读取请求将设定所需读参数的帧类型，子序号设置为 `0xFF`，数据内容为 `0xFFFFFFFFFFFFFFFF`。

例如使用TCP/IP发送获取编码器参数请求时，将帧类型设置为 `0x01`，子序号设置为 `0xFF`，数据内容为 `0xFFFFFFFFFFFFFFFF`。数据包如下：

```
01 FF FFFFFFFFFFFFFFFF
```

#### 4.1.2 参数设置与读取

参数设置和读取按照 [2.2 数据封装格式] 和参数帧结构打包和解析数据。

例如发送GPS数据，由于GPS数据总字节数为14，因此需要拆分为14//8=2个数据包发送，第一个数据包帧类型为0x02，子序号为0x01，数据内容为latitude_reference_point+longitude_reference_point；第二个数据包帧类型为0x02，子序号为0x02，数据内容为altitude_reference_point+gps_raw_data_output_frequency+gps_local_transformed_data_output_frequency。两个数据包如下：

```
02 01 AAAAAAAAAAAAAAAA
02 02 BBBBBBBBBBBBBBBB
```

其中AAAAAAAAAAAAAAAA为latitude_reference_point+longitude_reference_point数据，BBBBBBBBBBBBBBBB为altitude_reference_point+gps_raw_data_output_frequency+gps_local_transformed_data_output_frequency数据。

数据读取解析同理。

#### 4.1.3 参数帧结构

| 帧类型 | 描述       | 数据内容                                                     | 数据长度 |
| :----: | :--------- | :----------------------------------------------------------- | -------- |
|  0x01  | 编码器参数 | uint32_t encoder_pulse_per_rev<br/>float front_left_wheel_radius<br/>float front_right_wheel_radius<br/>float front_wheelbase<br/>float rear_left_wheel_radius<br/>float rear_right_wheel_radius<br/>float rear_wheelbase<br/>float time_factor<br/>float x_covariance<br/>float y_covariance<br/>float z_covariance<br/>float rx_covariance<br/>float ry_covariance<br/>float rz_covariance<br/>float vx_covariance<br/>float vy_covariance<br/>float vz_covariance<br/>float wx_covariance<br/>float wy_covariance<br/>float wz_covariance<br/>uint8_t raw_data_output_frequency<br/>uint8_t odometry_data_output_frequency | 82       |
|  0x02  | GPS参数    | float latitude_reference_point<br/>float longitude_reference_point<br/>float altitude_reference_point<br/>uint8_t gps_raw_data_output_frequency<br/>uint8_t gps_local_transformed_data_output_frequency | 14       |
|  0x03  | EKF1参数   | float x_covariance<br/>float y_covariance<br/>float z_covariance<br/>float rx_covariance<br/>float ry_covariance<br/>float rz_covariance<br/>float vx_covariance<br/>float vy_covariance<br/>float vz_covariance<br/>float wx_covariance<br/>float wy_covariance<br/>float wz_covariance<br/>float ax_covariance<br/>float ay_covariance<br/>float az_covariance<br/>uint8_t frequency | 61       |
|  0x04  | EKF2参数   | float x_covariance<br/>float y_covariance<br/>float z_covariance<br/>float rx_covariance<br/>float ry_covariance<br/>float rz_covariance<br/>float vx_covariance<br/>float vy_covariance<br/>float vz_covariance<br/>float wx_covariance<br/>float wy_covariance<br/>float wz_covariance<br/>float ax_covariance<br/>float ay_covariance<br/>float az_covariance<br/>uint8_t frequency | 61       |

### 4.2 命令

#### 4.2.1 命令数据打包与解析

与 [4.1.2 参数设置与读取] 方式一致。

#### 4.2.2 命令帧结构

| 帧类型 | 描述        | 数据内容                            | 数据长度 |
| :----: | :---------- | :---------------------------------- | -------- |
|  0x30  | EKF相关命令 | bool ekf1_reset<br/>bool ekf2_reset |          |
|        |             |                                     |          |

### 4.3 数据流

#### 4.3.1 数据打包与解析

与 [4.1.2 参数设置与读取] 方式一致。

#### 4.3.2 数据帧结构

| 帧类型 | 描述         | 数据内容                                                     | 数据长度 |
| :----: | :----------- | :----------------------------------------------------------- | -------- |
|  0x50  | 编码器数据   | uint64_t timestamp<br/>int32_t front_left_count<br />int32_t front_right_count<br />int32_t rear_left_count<br />int32_t rear_right_count | 24       |
|  0x51  | 里程计数据   | uint64_t timestamp<br/>float x<br/>float y<br/>float z<br/>float ox<br/>float oy<br/>float oz<br/>float ow<br/>float x_covariance<br/>float y_covariance<br/>float z_covariance<br/>float rx_covariance<br/>float ry_covariance<br/>float rz_covariance<br/>float vx<br/>float vy<br/>float vz<br/>float wx<br/>float wy<br/>float wz<br/>float vx_covariance<br/>float vy_covariance<br/>float vz_covariance<br/>float wx_covariance<br/>float wy_covariance<br/>float wz_covariance | 108      |
|  0x52  | GPS原始数据  | uint64_t timestamp<br/>float x<br/>float y<br/>float z<br/>float ox<br/>float oy<br/>float oz<br/>float ow<br/>float x_covariance<br/>float y_covariance<br/>float z_covariance<br/>float rx_covariance<br/>float ry_covariance<br/>float rz_covariance | 60       |
|  0x53  | GPS转化数据  | uint64_t timestamp<br/>float x<br/>float y<br/>float z<br/>float ox<br/>float oy<br/>float oz<br/>float ow<br/>float x_covariance<br/>float y_covariance<br/>float z_covariance<br/>float rx_covariance<br/>float ry_covariance<br/>float rz_covariance | 60       |
|  0x54  | EKF1融合数据 | uint64_t timestamp<br/>float x<br/>float y<br/>float z<br/>float ox<br/>float oy<br/>float oz<br/>float ow<br/>float x_covariance<br/>float y_covariance<br/>float z_covariance<br/>float rx_covariance<br/>float ry_covariance<br/>float rz_covariance<br/>float vx<br/>float vy<br/>float vz<br/>float wx<br/>float wy<br/>float wz<br/>float vx_covariance<br/>float vy_covariance<br/>float vz_covariance<br/>float wx_covariance<br/>float wy_covariance<br/>float wz_covariance<br/> | 108      |
|  0x55  | EKF2融合数据 | uint64_t timestamp<br/>float x<br/>float y<br/>float z<br/>float ox<br/>float oy<br/>float oz<br/>float ow<br/>float x_covariance<br/>float y_covariance<br/>float z_covariance<br/>float rx_covariance<br/>float ry_covariance<br/>float rz_covariance<br/>float vx<br/>float vy<br/>float vz<br/>float wx<br/>float wy<br/>float wz<br/>float vx_covariance<br/>float vy_covariance<br/>float vz_covariance<br/>float wx_covariance<br/>float wy_covariance<br/>float wz_covariance<br/> | 108      |
|  0x56  | 时间同步数据 | uint64_t timestamp                                           | 8        |
|  0x57  | 激光定位数据 | uint64_t timestamp<br/>float x<br/>float y<br/>float z<br/>float ox<br/>float oy<br/>float oz<br/>float ow<br/>float x_covariance<br/>float y_covariance<br/>float z_covariance<br/>float rx_covariance<br/>float ry_covariance<br/>float rz_covariance<br/> | 56       |
|  0x58  | 车身角数据 | double steering_angle | 4       |

## 5. 其它说明

- 所有数值均采用小端格式（LE）。

