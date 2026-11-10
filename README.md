# Padreen — 把华为 MatePad Paper 当成电脑的第二块显示器

墨水屏平板没有视频输入，只能软件推流。Padreen 用一块虚拟显示器 + DXGI 桌面复制，把 Windows 桌面推到平板的墨水屏上，并把平板的触摸回传给电脑，让这块 10.3" 墨水屏成为能点、能拖、能用的扩展屏。

- **电脑端**：`host/padreen`，单个 C++ 托盘程序（无 Python 依赖），自动检测平板、插虚拟屏、抓屏、1-bit 化/灰度化、瓦片差分、LZ4 压缩、发送。
- **平板端**：`android/app`，普通 APK，不 root，走 SurfaceFlinger 常规刷新通道，用华为 e-ink 的波形接口控制刷新模式（A2/DU/GL16/GC16）。
- **连接**：局域网自动发现 + 9 位配对码（ECDH P-256 + AES-128-GCM）或 USB 线（adb forward），两种通道共用同一套协议。

极速模式下实测约 **0.1 s** 一次刷新；停手后自动对改动区域补灰阶。触摸回传用 Windows 触控注入（`InjectTouchInput`），真多指，不动鼠标指针、不改系统设置。

## 目录结构

| 路径 | 内容 |
|---|---|
| `host/padreen/` | 电脑端 C++（托盘、控制面板、配对窗口、DXGI 抓屏、编解码、VDD、触控注入、电源助手） |
| `host/poc/` | 早期 Python 原型与延迟测量工具（仅实验用，不参与产品构建） |
| `android/app/` | 平板 App（`com.padreen.app`）：主页发现/配对、显示页渲染器、触控手势闸、方向同步、音量键 |
| `android/app/test/` | 手势/方向/音量键的 JVM 单元测试（`run-tests.ps1`，带 android.os 桩） |
| `android/probe/` | 墨水屏驱动探测 APK（找 e-ink 系统接口、测波形） |
| `protocol/PROTOCOL.md` | 线协议 v2.0（数据面 + 局域网配对 PD2） |
| `docs/probe-report.md` | 实测与结论（延迟、波形、FB 直写为什么不能用等） |
| `docs/STATE.md` | 工作状态、用户需求与决策、恢复步骤 |
| `PLAN.md` | 方案与变更历史 |
| `tools/` | 环境准备、打包（`make-dist.ps1`）、安装包（`make-installer.ps1`）、探测脚本 |
| `installer/Padreen.iss` | Inno Setup 安装脚本 |
| `third_party/lz4/` | LZ4（BSD-2-Clause），用于 mono 更新的压缩 |

## 构建

前置：Windows x64、Visual Studio 2022 的 MSVC 工具链、JDK 11、Android SDK build-tools（aapt2 / d8 / zipalign / apksigner）、Inno Setup 7（仅安装包需要）。

```powershell
# 电脑端（输出 host\padreen\build\padreen.exe）
powershell -ExecutionPolicy Bypass -File host\padreen\build.ps1

# 平板 APK（不用 Gradle：aapt2 + javac + d8 + apksigner）
powershell -ExecutionPolicy Bypass -File android\app\build.ps1 [-Install] [-Start]

# 平板端单元测试
powershell -ExecutionPolicy Bypass -File android\app\test\run-tests.ps1

# 组装可分发的 green 版目录 dist\Padreen\ 与安装包
powershell -ExecutionPolicy Bypass -File tools\make-dist.ps1 [-Build]
powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1 [-Build]
```

脚本里存在写死的 `D:\Workspace\MatePadPaper` 与 `D:\` 下的工具链路径（本项目按"环境软件一律装 D 盘、临时文件不落 C 盘"的约定编写），换机器需要按 `tools/setup-env.ps1` 和 `host/padreen/vcenv.ps1` 调整。

## 这不是什么

- 不需要 root，也不修改平板的任何系统设置；电脑端唯一的系统级改动是安装虚拟显示器驱动（Parsec VDD，可卸载），插屏只在程序运行且平板连接时存在。
- 不包含构建产物、第三方 SDK、驱动二进制、私钥与测试录像（见 `.gitignore`）。

## 设计要点

1. **不走 FB 直写**：`FBHandSurface.postFrameBuffer` 绕过了系统唤醒与节流，会让 T1000 控制器休眠后收不到更新、I²C 卡死 2.25 s 被硬复位（表现为拖窗口时连续黑白闪屏）。最终走 `SurfaceView.lockCanvas(脏矩形)` + `EPDCDevice.setForceNextPostMode` + `unlockCanvasAndPost`，由系统负责唤醒与节流，压力测试零复位。
2. **极速模式 = DU/A2 波形纯黑白**，停手 1.2 s 后用 GL16 对改动区域补 16 级灰度；自动整屏刷新与自动补灰阶默认关闭（用户手动触发：三指轻点）。
3. **刷新要懒**：网络线程只更新影子帧缓冲与待刷矩形，渲染线程按节奏取**最新**内容提交，系统忙时自然合并，不排旧帧。
4. **虚拟屏自动设纯白壁纸**（仅平板那一块，退出还原）：深色大图在 1-bit 墨水屏上是满屏抖动点，是重影/闪屏的主因。

详见 [docs/probe-report.md](docs/probe-report.md) 与 [PLAN.md](PLAN.md)。
