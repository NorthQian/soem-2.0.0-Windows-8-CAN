# Simple Open EtherCAT Master Library

* Copyright (C) 2005-2025 Speciaal Machinefabriek Ketels v.o.f.
* Copyright (C) 2005-2025 Arthur Ketels
* Copyright (C) 2008-2009 TU/e Technische Universiteit Eindhoven
* Copyright (C) 2009-2025 RT-Labs AB, Sweden

SOEM (Simple Open EtherCAT Master) is a software library for
developing EtherCAT MainDevices.

This library is specifically designed for real-time communication in
embedded systems. Its lightweight architecture minimizes resource
consumption, making it suitable for environments with limited
resources. SOEM can also be utilized on both Linux and Windows
systems.

As a library rather than a standalone application, SOEM provides
flexibility and customization for developers looking to implement
EtherCAT technology. 

# Documentation

See https://docs.rt-labs.com/soem

# Contributions

Contributions are welcome. If you want to contribute you will need to
sign a Contributor License Agreement and send it to us either by
e-mail or by physical mail. More information is available on
[https://rt-labs.com/contribution](https://rt-labs.com/contribution).

---

# 本发行版构建说明（Windows x86-64）

> 本节针对本仓库这份 **Windows 预编译发行版**，与上游 SOEM 的构建方式不同。

## 与上游的区别：本发行版不含 SOEM 源码

本发行版只提供**预编译好的静态库 + 头文件**，没有 `SOEM-2.0.0/` 源码目录：

| 路径 | 说明 |
| --- | --- |
| `lib/libsoem.a` | 预编译静态库（UCRT 构建） |
| `include/soem/` | SOEM 头文件 |
| `main.cpp` | 示例程序，编译产物为 `soem_demo.exe` |
| `import-libs/` | 从本机 Npcap DLL 生成的导入库 |
| `third_party/npcap-sdk/Include/` | Npcap SDK 头文件（仅头文件） |
| `build/` | CMake 构建目录，产物为 `build/soem_demo.exe` |

所以 `CMakeLists.txt` 是把 `lib/libsoem.a` 当作 imported target 链接的，用的是 `add_subdirectory()` 以外的写法。

## 环境要求

- **MinGW-w64**（gcc 16.2.0，x86-64，UCRT），本机位于 `D:\mingw64`
  - 本机**没有安装 Visual Studio**，因此不能用默认的 NMake / Visual Studio 生成器，必须显式指定 `-G "MinGW Makefiles"`
- **CMake ≥ 3.20**（见 `CMakeLists.txt` 的 `cmake_minimum_required`），本机为 `C:\Program Files\CMake\bin`
- **Npcap 运行时**已安装（`C:\Windows\System32\wpcap.dll`、`Packet.dll`）
- 运行 `soem_demo.exe` 需要**管理员权限**——Npcap 不允许非管理员打开网卡

## 首次配置

```powershell
cd D:\soem-2.0.0-Windows-x86_64\soem-2.0.0-Windows-x86_64
mkdir build; cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build .
```

`build\` 是空的（或刚删过）时才需要这一串。生成器一旦写进 `CMakeCache.txt`，后面就不用再带 `-G` 了。

## 日常增量编译

改完 `main.cpp` 或 `CMakeLists.txt` 后：

```powershell
cd build
cmake --build .
```

一条就够。`CMakeLists.txt` 变动时 `cmake --build .` 会自动重新配置，不需要手动再跑 `cmake ..`。

也可以从仓库根目录直接：

```powershell
cmake --build build
```

> **别删 `build\` 目录。** 只要它在，`cmake --build .` 永远够用；一旦删掉，`cmake ..` 会因为没有缓存而退回默认的 NMake 生成器并报 `nmake ... no such file or directory`。

> 注意：`build/CMakeCache.txt` 里的 `CMAKE_BUILD_TYPE` 是**空的**，说明那次配置其实并没有带上 `-DCMAKE_BUILD_TYPE=RelWithDebInfo`。空构建类型在 MinGW 下等于不加优化、也不生成调试信息。想开优化要重新配置一次：

> ```powershell
> cd build
> cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
> ```

## 运行

```powershell
# 需以管理员身份打开 PowerShell
cd D:\soem-2.0.0-Windows-x86_64\soem-2.0.0-Windows-x86_64\build
.\soem_demo.exe
```

程序会先列出网卡让你输入编号，然后开始跑 EtherCAT。**注意这一步会往选中的网卡发广播包，别选错网口。**

## 示例程序行为

`main.cpp` 干三件事：跑 EtherCAT 主站、驱动板上的 LED 并读回开关、通过 CAN 网关控制一台达妙（DM）电机。

启动顺序：枚举从站 → 配置 PDO 映射 → 测量 DC → **把每个支持 DC 的从站设为 DC-Synchron（SYNC0）模式** → 切到 OP → 跑 10000 个周期（默认 1 ms，约 10 秒）。

从站若都配置成 free-run（不支持 DC），程序会自动退回 5 ms 自由运行的循环，总时长会变成约 50 秒。

### 1. DC 参数

在 `main.cpp` 顶部，按实际从站情况调整：

| 宏 | 默认值 | 说明 |
| --- | --- | --- |
| `DC_CYCLE_NS` | `1000000` | SYNC0 周期，同时也是循环周期（1 ms） |
| `DC_SHIFT_NS` | `0` | SYNC0 偏移，0 表示脉冲正好落在周期边界 |
| `DC_STARTUP_MS` | `200` | SYNC0 启动后的稳定等待时间；若从站进不了 OP，先调大这个 |

### 2. LED 与开关

LED 是**往返流水灯**：`led1 → led8` 再折回来 `led8 → led1`，不会从末尾直接跳回开头。每步 `LED_STEP_CYCLES`（默认 100）个周期，1 ms 周期下即 100 ms 一步，走完单程 700 ms。

| 宏 | 默认值 | 说明 |
| --- | --- | --- |
| `LED_ACTIVE_LOW` | `0` | 本板是**高电平点亮**。换成灌电流的板子时改成 `1` |
| `LED_FIRST_BIT` | `0` | `led1` 在该从站输出位段里的偏移。改 PDO 映射后要跟着改 |
| `LED_BITS` | `8` | LED 位数 |
| `LED_STEP_CYCLES` | `100` | 每步多少周期，调大即放慢 |
| `SW_FIRST_BIT` / `SW_BITS` | `0` / `8` | 开关位段，含义同上 |
| `SW_BIT_ORDER_LSB` | `1` | 打印开关时最左边是不是 switch1。镜像了就把这个换一下 |

过程数据按位读写（不是整字节 `memset`），因为 LED 位段和 CAN 字段挤在同一片缓冲区里，而且位段不一定从字节边界开始。

### 3. DM 电机（CAN 网关）

这块 EtherCAT 从站其实是个 **CAN 桥**：主站把 8 字节载荷写进 `can_txdata0..7`，从站固件把它发到 CAN 总线；电机回帧被抄进 `can_rxdata0..7`，主站读回来。**从站每个 EtherCAT 周期都自己发一次，没有握手**——所以主站要做的就是让缓冲区一直保持最新。

主站**不能直接指定 CAN ID**，只能写 `send_id`（电机 ID），由固件推导出 CAN ID。

过程数据布局（输出 10 字节 / 80 位，输入同样，无填充）：

```
outputs  led1..8        bits  0.. 7     (LED_FIRST_BIT .. +LED_BITS)
         send_id        bits  8..15     (CAN_TX_ID_FIRST_BIT)
         can_txdata0..7 bits 16..79     (CAN_TX_DATA_FIRST_BIT .. +64)
inputs   switch1..8     bits  0.. 7
         rec_id         bits  8..15
         can_rxdata0..7 bits 16..79
```

从站是**按字节数**（10 字节输出 + 10 字节输入）而不是按下标找的，找不到就打印 `no gateway` 并跳过全部 CAN 代码。所有 CAN 位偏移都从 `LED_FIRST_BIT`/`LED_BITS` 推导，改 LED 映射时两边不会错位。

运行流程：

1. **前 1 秒**往 `can_txdata` 写使能载荷 `FF FF FF FF FF FF FF FC`，持续 `DM_ENABLE_MS` 毫秒
2. 之后连发 **MIT 模式**控制帧，目标位置是正弦轨迹 `p = ±π·sin(2/π·t)`，周期 π² ≈ 9.87 s，峰值速度 2 rad/s（约 115°/s）
3. 循环结束后连发 `DM_DISABLE_CYCLES`（默认 50）帧失能载荷 `FF FF FF FF FF FF FF FD`，再关总线

使能和失能都是**「七个 `0xFF` + 一个命令字节」**的格式：`0xFC` = 使能，`0xFD` = 失能。两者和 MIT 控制帧走**同一个 CAN ID**（`motor_id`），只靠载荷区分。

状态行**每个周期都重写**（`\r` 回到行首原地覆盖，不是滚动刷屏）：

```
LED 00010000 SW 00000101 | cmd p= +2.221 v= +1.41 | fb p= +2.198 v= +1.40 t= +0.31 RUN 31/29C
```

- `LED` / `SW`：逐位打印，最左边是第 1 位
- `cmd p= v=`：主站当前发出的期望位置和期望速度。**即使收不到反馈它也在推进**，所以这是区分「主站在发但 CAN 回程断了」和「主站根本没发」最快的手段
- `fb ...`：电机回帧解出的位置/速度/力矩、状态（`STOP`/`RUN`/`ERR`/`CALIB`）、MOS 与线圈温度。还没收到回帧显示 `fb none`；电机 ID 对不上会多打一个 `ID!`

电机相关宏：

| 宏 | 默认值 | 说明 |
| --- | --- | --- |
| `DM_MOTOR_ID` | `1` | 写进 `send_id` 的电机 ID |
| `DM_ENABLE_MS` | `1000` | 使能载荷保持时长 |
| `DM_DISABLE_CYCLES` | `50` | 收尾失能帧数 |
| `DM_HEARTBEAT_CYCLES` | `0` | 每 N 个控制周期插一帧使能，**0 = 关闭**。MIT 模式不需要心跳，插进去反而会打断 MIT 流 |
| `DM_KP` / `DM_KD` / `DM_TFF` | `30` / `1` / `0` | 电机自己闭环用的增益 |
| `SIN_AMP` / `SIN_VEL` | `±3.14159` / `2.0` | 轨迹幅值（rad）与峰值速度（rad/s），周期由两者推导 |

### 4. 上电前必须确认的事

> **电机会真的转 ±π（半圈），峰值约 115°/s，周期约 9.87 s。** 第一次跑请确认机构能安全走完整个半圈，急停/断电开关放手边。

几个**错了会静默失效、不会报错**的假设：

- **`Kp_MAX` / `Kd_MAX` 取的是 500 / 5（DM 默认值）。** 如果实际固件用的是别的量程，`kp=30` 编码出来的数就不是 30，实际增益会差一个比例。这是最可能出错的一处。
- **从站固件推导出的 CAN ID 必须是 `motor_id + 0x000`（即 0x001）。** 如果是 `+0x100`（0x101），固件会把打包好的 MIT 帧当成两个 float32 读，第二个约 -1.6e38，电机会全速飞车。**MIT 控制一开始电机就狂奔的话，先怀疑这个。**
- **`rec_id` 的含义未知**，程序原样打印、不参与解码，所以猜错也不会污染其他数据。

另外注意，MIT 模式下从站会**一直转发最后一帧**，所以 EtherCAT 掉线时电机是"锁在最后位置"而不是松开的——这是设计上的失效模式，不是 bug。程序正常跑完时会发失能帧兜底；但如果是 WKC 不匹配提前 `break`，总线已经断了，那些帧到不了电机，**这种情况直接断电**。

## Npcap 升级后要重新生成导入库

`third_party/npcap-sdk` 里只有头文件，`.lib` 是 MSVC 格式、MinGW 链接不了，所以 `import-libs/` 下的导入库是从**本机已安装的 DLL** 现场生成的。**Npcap 升级后需要重新生成**，否则会出现链接错误：

```bash
cd import-libs
gendef /c/Windows/System32/wpcap.dll
gendef /c/Windows/System32/Packet.dll
dlltool -d wpcap.def  -l libwpcap.a  -D wpcap.dll
dlltool -d Packet.def -l libPacket.a -D Packet.dll
```

> 另注：`lib/libsoem.a` 是 UCRT 构建的，所以可执行文件也必须用 UCRT——`CMakeLists.txt` 里已定义 `_UCRT` 并链接 `ucrt`，不要删。

