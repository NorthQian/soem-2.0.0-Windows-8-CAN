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
| `build/` | CMake 构建目录 |

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

> **别删 `build\` 目录。** 只要它在，`cmake --build .` 永远够用；一旦删掉，`cmake ..` 会因为没有缓存而退回默认的 NMake 生成器并报 `nmake ... no such file or directory`。

产物：`build\soem_demo.exe`

## 运行

```powershell
# 需以管理员身份打开 PowerShell
cd D:\soem-2.0.0-Windows-x86_64\soem-2.0.0-Windows-x86_64\build
.\soem_demo.exe
```

程序会先列出网卡让你输入编号，然后开始跑 EtherCAT。**注意这一步会往选中的网卡发广播包，别选错网口。**

## 示例程序行为

`main.cpp` 会：枚举从站 → 配置 PDO 映射 → 测量 DC → **把每个支持 DC 的从站设为 DC-Synchron（SYNC0）模式** → 切到 OP → 以 SYNC0 为基准跑 10000 个周期（默认 1 ms）。

DC 参数在 `main.cpp` 顶部，按实际从站情况调整：

| 宏 | 默认值 | 说明 |
| --- | --- | --- |
| `DC_CYCLE_NS` | `1000000` | SYNC0 周期，同时也是循环周期（1 ms） |
| `DC_SHIFT_NS` | `0` | SYNC0 偏移，0 表示脉冲正好落在周期边界 |
| `DC_STARTUP_MS` | `200` | SYNC0 启动后的稳定等待时间；若从站进不了 OP，先调大这个 |

从站若都配置成 free-run（不支持 DC），程序会自动退回 5 ms 自由运行的循环。

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

