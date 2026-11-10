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
| `tools/` | 环境准备、打包（`make-dist.ps1`）、安装包（`make-installer.ps1`）、开发用的驱动包脚本（`fetch-vdd.ps1`）、探测脚本 |
| `installer/Padreen.iss` | Inno Setup 安装脚本 |
| `third_party/lz4/` | LZ4（BSD-2-Clause），用于 mono 更新的压缩 |

## 构建

前置：Windows x64、Visual Studio 2022（"使用 C++ 的桌面开发"工作负载，带 Windows SDK）、JDK 11、Android SDK（build-tools 32.0.0、platform android-33）、Inno Setup 7（仅安装包需要）、GitHub CLI（仅发布到 GitHub 时用）；7-Zip 只有开发辅助脚本 `fetch-vdd.ps1` 用到（可选）。

**一条命令配好环境**：缺什么装什么，装到 `D:\Tools`（没有 D 盘则 `%LOCALAPPDATA%\Padreen\tools`），不需要管理员权限，不改 PATH、不设环境变量，每个下载都校验 SHA-256（Inno Setup 另验 Authenticode 签名）。可重复运行；`-CheckOnly` 只检查不安装。它不会替你接受 Android SDK 的许可证，也不会装 Visual Studio（只检查）。

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup-build-env.ps1 [-CheckOnly]
```

各构建脚本通过 `tools/toolchain.ps1` 自己找工具，依次试：环境变量（`PADREEN_TOOLS` / `PADREEN_JDK` / `PADREEN_ISCC` / `PADREEN_GH` / `ANDROID_HOME`）→ `D:\Tools` → 常见安装位置 → 原开发电脑的目录布局，所以换电脑不用改脚本。

```powershell
# 电脑端（输出 host\padreen\build\padreen.exe）
powershell -ExecutionPolicy Bypass -File host\padreen\build.ps1

# 平板 APK（不用 Gradle：aapt2 + javac + d8 + apksigner；需要签名密钥，见下面"发布"）
powershell -ExecutionPolicy Bypass -File android\app\build.ps1 [-Install] [-Start]

# 平板端单元测试
powershell -ExecutionPolicy Bypass -File android\app\test\run-tests.ps1

# 组装可分发的 green 版目录 dist\Padreen\ 与安装包（出包时会实测"程序能从 Parsec 官方地址取到并校验驱动"，需要联网；离线构建加 -SkipDriverCheck，发布不要用）
powershell -ExecutionPolicy Bypass -File tools\make-dist.ps1 [-Build] [-SkipDriverCheck]
powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1 [-Build] [-SkipDriverCheck]

# 开发辅助（不属于发布）：把驱动包取到 tools\vdd\pkg，给旁边的 install-vdd.ps1 等脚本用（需要 7-Zip）
powershell -ExecutionPolicy Bypass -File tools\fetch-vdd.ps1 [-PkgDir <文件夹>]
```

**虚拟显示器驱动不随 Padreen 分发。** 它是 Parsec 的软件（Parsec VDD 0.41 + nefconw.exe），没有找到允许 Padreen 再分发它的授权，所以安装包、绿色版目录和源码里都没有它；来源与校验值见 `tools/vdd/pkg/NOTICE.txt`。用户一侧仍然不需要手工步骤：`padreen.exe` 首次运行发现没有驱动时说明情况并询问，点「是」后**在用户自己的电脑上**从 Parsec 官方地址（`builds.parsec.app`）下载两个安装包（共约 0.7 MB），先核对安装包的 SHA-256，再用程序自带的小解包器（Parsec 的安装包是 NSIS + deflate，不需要 7-Zip）按内容哈希取出 4 个文件，再核对一遍，然后经一次 Windows 管理员授权安装（不依赖 PowerShell 脚本；托盘右键菜单随时可重装/修复）。任何一个哈希对不上（比如 Parsec 换了文件）就拒绝使用，并提示去更新 Padreen。下载过的安装包和解出的文件保存在设置文件旁的 `driver-cache` 文件夹里：修复/重装不用再下载；也可以手动把两个安装包放进去（没有网络或代理拦截时）。`<程序目录>\driver` 里放一份完整的驱动包同样会被直接使用（离线或开发）。程序里内置了安装包和驱动文件的 SHA-256（`host/padreen/src/driver.cpp`）；换驱动版本时要同时改它和 `tools/fetch-vdd.ps1`。`make-dist.ps1` 在出包时用刚做好的 exe 对 Parsec 的真实下载跑一遍 `padreen.exe --fetch-driver --cache <文件夹>`，并检查包里没有任何 Parsec 文件——否则中止。

## 发布

1. **签名密钥**（只需一次，**私钥只留在本机，绝不提交或上传**）：平板 App 只能被**同一把密钥**签名的包覆盖升级。仓库是公开的，谁拿到私钥谁就能签出 Android 认可的"升级包"，而且无法吊销，所以密钥不在仓库里：`tools\keys\` 被 `.gitignore` 排除。第一次发布前在本机生成：`powershell -ExecutionPolicy Bypass -File android\app\build.ps1 -NewKey`，得到 `tools\keys\release.jks`（别名 `release`，`CN=Padreen`，RSA 2048，有效期约 27 年）和同目录的 `release.jks.pass`（随机口令；目录权限只给当前用户与 SYSTEM）。**生成后马上把整个 `tools\keys` 备份到私密的地方**（密码管理器、加密盘）——丢了就再也不能升级已装的平板。换电脑时把备份放回 `tools\keys\`，或用环境变量 `PADREEN_KEYSTORE`（密钥文件）和 `PADREEN_KEYSTORE_PASS`（口令；也可写在 `<密钥文件>.pass`）指定。没有密钥时 `android\app\build.ps1` 会拒绝构建，**不会悄悄新建**。每次构建结束都会打印证书指纹，发布前核对：当前发布密钥的证书 SHA-256 是 `e8fc0fe2666acc90f36a2cb4ed28ca20a12fbdabee7d24bc8f11aaba9d4c60c4`。想在 GitHub Actions 里自动出包：把密钥文件做 base64 存进仓库的 Actions Secrets，不要放进仓库文件。
   - 旧的开发密钥（`CN=PaperDisplay Dev`，口令 `paperdisplay` 已随仓库公开）**已作废**，不要再用它出包。平板上若还装着用它签名的旧版 App，电脑端不会去覆盖（只有平板 App 版本低于电脑端要求时才会升级）；要换成新密钥的 App：先在平板上卸载 Padreen（会清掉配对），再插 USB 线，Padreen 自动装上新版并自动重新配对。
   - 平板 App 的自动安装：Padreen 运行时一插 USB 线（或先插线再启动），就检查平板上有没有 Padreen App，没有或版本低就 `adb install -r`（平板上点一次「继续安装」）；平板还没允许 USB 调试、安装被取消、签名密钥不同等情况，托盘提示里会写明原因和办法。
2. **出包**：`powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1 -Build`，得到 `dist\Padreen-Setup-<版本>.exe` 和绿色版目录 `dist\Padreen\`。
3. **发到 GitHub**：先登录一次 `D:\Tools\gh\bin\gh.exe auth login`，把改动提交、推送并打好 tag，再 `D:\Tools\gh\bin\gh.exe release create <tag> dist\Padreen-Setup-<版本>.exe --repo 1031193940/Padreen --title "…" --notes "…"`。

## 这不是什么

- 不需要 root，也不修改平板的任何系统设置；电脑端唯一的系统级改动是安装虚拟显示器驱动（Parsec VDD，用户同意后由程序从 Parsec 官方地址取得并安装，可在设备管理器里卸载），插屏只在程序运行且平板连接时存在。
- 不包含构建产物、第三方 SDK、Parsec 的驱动文件、私钥与测试录像（见 `.gitignore`）；任何发布包里都没有 Parsec 的文件，驱动只在用户电脑上从 Parsec 官方地址下载。

## 设计要点

1. **不走 FB 直写**：`FBHandSurface.postFrameBuffer` 绕过了系统唤醒与节流，会让 T1000 控制器休眠后收不到更新、I²C 卡死 2.25 s 被硬复位（表现为拖窗口时连续黑白闪屏）。最终走 `SurfaceView.lockCanvas(脏矩形)` + `EPDCDevice.setForceNextPostMode` + `unlockCanvasAndPost`，由系统负责唤醒与节流，压力测试零复位。
2. **极速模式 = DU/A2 波形纯黑白**，停手 1.2 s 后用 GL16 对改动区域补 16 级灰度；自动整屏刷新与自动补灰阶默认关闭（用户手动触发：三指轻点）。
3. **刷新要懒**：网络线程只更新影子帧缓冲与待刷矩形，渲染线程按节奏取**最新**内容提交，系统忙时自然合并，不排旧帧。
4. **虚拟屏自动设纯白壁纸**（仅平板那一块，退出还原）：深色大图在 1-bit 墨水屏上是满屏抖动点，是重影/闪屏的主因。

详见 [docs/probe-report.md](docs/probe-report.md) 与 [PLAN.md](PLAN.md)。
