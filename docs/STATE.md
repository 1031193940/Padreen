# 工作状态与交接

> 快照：2026-11-09 20:30（+08:00）。**本轮（第 16 轮，已部署并实机验证）**：①**产品更名为 Padreen**（旧名 PaperDisplay / 电脑端 PaperHost；`padreen.exe`、`Padreen-Setup-1.1.1.exe`、平板 App `com.padreen.app`；源码目录 `host/padreen`、`android/app/src/com/padreen`；**线协议里的魔数/端口不变**，所以旧版平板 App 仍能连新电脑端）——**本文件里 11-09 之前发生的事，当时的名字是旧名，已被批量替换成新名，读历史时请自行对应**；旧安装的迁移与现场状态见 §3.1；②白壁纸只作用于平板虚拟屏的**加固**（`wallpaper.cpp`：快照所有真实屏幕、2 s 巡检发现真实屏幕显示了我们的白图就改回、"原壁纸"永不记成白图、退出时把白图从 Windows「最近使用的图片」里去掉；实机验证：会话中两块真实屏壁纸不变）；③**自动旋转开关**加到电脑主界面和托盘菜单（默认开，`rotate`，连接着也能切；`T_GEOM` flags bit1 `NO_ROTATE`；实机验证通过）；④**Padreen 在平板最前面时音量键调电脑音量**（显示页和主页都处理，连着或重连宽限期内吞键；实机验证：电脑音量随按键变化）；⑤用户反馈"自动旋转不灵敏"→ 查明：平板**只有加速度计**（无陀螺仪/磁力计），系统旋转监听**没接传感器**（`mSensor=null`），而且平板常**平放在桌上**（倾角 <1°，重力方向不随平面内转动变化，传感器物理上感觉不到）→ 平板 App 改为**自己读加速度计**判断方向（`OrientationDetector`，转约 52° 就触发，倾斜 ≥10° 即可判断），**平放时保持当前方向**（用户明确要求），显示页重建后也保持离开时的方向（`DisplayHub.lastRotation` + `MainActivity.onCreate` 先请求它；新 Activity 的第一个 Surface 有时还是上一个界面的方向，`surfaceChanged` 会等转到位再报给电脑，避免电脑虚拟屏横→竖来回折腾，已用 adb 经 Back/launcher 路径复现并验证）；曾加过"三指左右滑手动旋转"，用户说不需要，已撤掉。平板 App **build 17 / v1.1.3**（已装在平板上）；电脑端已装 **1.1.1**，`dist\Padreen-Setup-1.1.2.exe` 是最新构建（比已装的只多一项：`requiredAppBuild` 15→16，下次有电脑端改动时一起装，不急）。上一轮（11-07）：**自动方向旋转** + **音量键调电脑音量**（build 13，协议见 PROTOCOL.md「平板转方向与音量键」）。11-05：三指轻点不再弹右键菜单（`TouchGate` 手势门）、三指上滑 = Win+Tab / 下滑 = Ctrl+Alt+Tab、误触整屏刷新的审计与收紧；11-05 夜另做了一份**墨水屏"慢和重影"的只读探查**（在工程之外：`D:\Workspace\_eink-research\FINDINGS.md`，结论：每次更新 SF 要做 ~75 ms 整帧 CPU 抖动并被 VSYNC 凑成 100 ms 一帧；我们的"整屏刷新"被系统改写成局部 GL16，等等；**方案待用户拍板，工程里还没动**）。
> 配套文档：[PLAN.md](../PLAN.md)（方案 v0.9）· [probe-report.md](probe-report.md)（实测与结论，**§8、§10–§17 最重要**）· [PROTOCOL.md](../protocol/PROTOCOL.md)（线协议 v2.0 = 数据面 v1.6 + 局域网/配对 PD2）

## 1. 用户的要求与决策（必须遵守）

| 日期 | 内容 |
|---|---|
| 10-31 | 目标：平板当电脑外接显示器；**极致响应速度**；即插即用；不影响平板原功能；尽量支持无线 |
| 10-31 | **只做显示**（当前阶段），不回传触控/笔 |
| 10-31 | **默认横屏**（1872×1404，数据线在右侧）；无线**只做 Wi-Fi**，舍弃蓝牙 |
| 10-31 | **不要因为外接设备修改电脑的全局设置**，类似操作都要规避；环境软件**一律装 D 盘** |
| 11-02 | **同意安装虚拟显示驱动 Parsec VDD 0.41**（用户已以管理员身份运行 `tools\vdd\pkg\install-vdd.ps1`） |
| 11-02 | **先做 C++ Padreen + 托盘自动连接**；要求记录：**后续要做 Wi-Fi 无线连接、触控操作反馈**（平板触控 → 电脑，对"只做显示"的有意放宽，作为后续阶段） |
| 11-02 | **同意**连接时自动给"平板这块虚拟屏"设**纯白壁纸**（只此一块屏，退出还原；原壁纸是 Photos 应用背景里的「死神.png」）。理由：深色大图壁纸在 1-bit 墨水屏上是满屏抖动点，是"重影/闪屏/卡"的主要根源；设白后用户确认"不卡了、闪屏和重影改善、跟手感好多了" |
| 11-03 | 用户肉眼对比后选定**「A2 6 帧」为极速模式的刷新方式**（已设为默认；托盘子菜单可换 DU/A2 7–9 帧/A2·MIX） |
| 11-03 | **要求一键全屏刷新**：现在还是会经常全屏刷新，我希望在平板端能有一个什么机制来一键触发全屏刷新，如果不是手动触发的情况下，尽量不要触发全屏刷新（→ 平板音量键/三指轻点；默认关闭所有自动整屏刷新与自动补灰阶，见 probe-report §16） |
| 11-03 | **要求**：①只留三指轻点刷新，取消音量键（音量键恢复调系统音量）；②"进一步包装成成熟的产品"：能匹配不同的 PC，**只需 PC 端启动程序，Pad 端自动局域网扫描、或输入配对码、或扫码来建立配对**；③用用户给的图标（assets/icon/source.png）做应用图标。→ 做了：平板主页自动扫描 + 9 位配对码（**MatePad Paper 没有摄像头，不能扫码**，已向用户说明）+ USB 免码配对、多电脑/多平板、PD2 协议、控制面板、安装包、图标 |
| 11-03 | **要求触控反馈**："平板端支持触控反馈，可以直接通过手指的方式点击按钮、拖动窗口、拖动滚动条等"（试用反馈：点按正常、拖动窗口正常、反馈有延迟但可接受） |
| 11-03 | **同意做 Wi-Fi 无线连接**，方案：首次 USB 配对（电脑生成密钥给平板）→ 之后拔线也能用；无线时在平板上手动打开 App，电脑托盘自动连接（只出站，不开防火墙端口、不改系统设置）；全程密钥认证 + AES-GCM 加密；平板 App 升级需用户在平板上点系统「继续安装」确认（不能代点） |
| 11-03 | **要求**：①"使用这块外接屏的时候应该允许笔记本电脑合上盖子但是不影响正常使用"；②"外接屏程序临时切出去还能正常切回来连接正常使用"。**同意**：用「电源助手」Windows 服务**仅在连接期间临时**把「合上盖子时」改成不采取任何操作，断开/退出/崩溃后自动还原（这是对"不要改全局设置"规则的有意、受控的例外）。→ 做了：见 probe-report §18 |
| 11-05 | **三条反馈**：①三指轻点触发刷新时还会触发右键菜单，要求三指轻触不触发任何事件；②加手势：三指上滑 = 多桌面（Win+Tab）、三指下滑 = 多任务（Alt+Tab），同样不触发其它事件；③在 Pad 上浏览网页、使用软件时仍经常自动整屏刷新，严重影响体验：检查哪些情况会触发整屏刷新，除非主动触发和极特殊的必要情况，其余都不要触发 |
| 11-05 | **要求**探索墨水屏显示"速度慢和重影"的原因，提出进一步优化方案（可从驱动角度或其它方向），**暂时只讨论方案、不要改工程**（→ 做了只读探查，见 `D:\Workspace\_eink-research\FINDINGS.md`，方案等用户拍板） |
| 11-07 | **两条需求**：①"支持自动方向旋转：设备切换到竖屏的时候，自动将桌面旋转方向切换到竖屏"；②"设备在开启桌面的时候，音量键用于调节电脑系统音量"（→ build 13：`OrientSync`/`T_ORIENT`/`T_GEOM`、`T_MEDIAKEY`；平板主页可分别关闭；电脑 `rotate=1`） |
| 11-09 | **四条要求**：①"要确保只有 Pad 的桌面是白色的，不能同时修改电脑桌面！"（→ 壁纸守护加固，见顶部快照）；②"自动方向旋转要有面板上的一个开关（默认开启）"（→ 电脑主界面 + 托盘菜单「自动旋转」，平板主页原有的保留）；③"Pad 端处于前端激活状态时，Pad 的音量键用于调节电脑系统的音量，而不是 Pad 的音量"（→ 显示页和主页都处理，连着电脑或重连宽限期内吞键，没连电脑时系统照常）；④**"产品更名为 Padreen，PC 端的安装包和可执行程序、Pad 端的 UI 等全部替换成这个名字"**（→ 全部更名 + 从旧版迁移，包括 Android 包名 `com.padreen.app`）。并要求"继续之前未完成的任务"（= 部署 build 13 的旋转/音量功能并实机验证；合并进本次一次部署） |
| 11-10 | **要求简化 UI**："请简化UI，删减UI上冗杂的说明，每个选项只保留最简单的几个字即可，删除掉所有UI上一堆乱七八糟的说明，包括Github Readme，只保留最核心的介绍、展示、使用方法即可"（→ 做了，见 §3.2「UI 简化」；**构建与发布说明不再放 README，只在本文件**） |
| — | 会话没有管理员权限 → 凡需管理员的操作（驱动安装/卸载）都得由用户手动运行脚本 |

## 2. 进度

| 阶段 | 状态 |
|---|---|
| Phase 0 探测与基线 | ✅ |
| 平板 App（`android/app`） | ✅ **build 17 / v1.1.3**（原 build 9 / v1.0 的基础上加了旋转/音量键等）：主页 `HomeActivity`（UDP 发现、已配对电脑列表、配对码数字键盘、手动输入 IP、自动连接上次的电脑、忘记）+ 显示 `MainActivity`；客户端握手 `PcClient`（连接/配对 PD2）、`Discovery`、`PcStore`（多电脑）；USB 回环服务保留（免码配对）；渲染器三类分开合并；三指轻点整屏刷新；触控回传；应用图标（自适应图标）。**平板没有摄像头 → 无扫码**。 |
| 虚拟显示器 | ✅ Parsec VDD 0.41 已装；Padreen 自己增删（`vdd.cpp`） |
| **C++ Padreen**（`host/padreen`） | ✅ **托盘常驻、自动检测平板（adb track-devices）、自动装/升级 App、转发端口、拉起 App、插虚拟屏、DXGI 桌面复制、1-bit 化、补灰阶、指针、白壁纸守护、拔线即拔屏、断线自动恢复、设置记忆、开机自启开关、A2 刷新方式子菜单、Wi-Fi（配对/记住 IP/扫描/50 ms 保活/加密）、LZ4 压缩 mono 更新**。抓取处理 0.1–7 ms（Python 原型 ~50 ms） |
| Python 原型（`host/poc`） | 保留作实验/测量工具：`mirror.py`、`stress.py`、`vdd.py`、`phctl.py`（给运行中的 Padreen 发托盘命令）、延迟测量脚本 |
| 延迟测量 | ✅ 第一次：450 ms（probe-report §11）；第二次（各波形，慢动作）：粗结论 §13；Wi-Fi 实测 §14 |
| **局域网（PD2）** | ✅ 电脑常驻监听 TCP 38702 + UDP 38703（只入站、限本地子网的防火墙规则；安装包加规则，绿色版首次运行引导 UAC）；平板主页自动发现 → 配对码配对（ECDH P-256 + PBKDF2 绑定配对码，错 5 次作废）→ 之后自动连接（双向 HMAC 认证 + AES-128-GCM）。实测：配对码配对、USB 配对、自动连接、会话、触控回传全通；LAN 服务器健壮性测试（未配对/窗口关闭/5 次错码/静默连接）通过。**v1 的"电脑主动连平板 38701"已移除**。 |
| **产品化** | ✅ 电脑端：托盘左键=控制面板（状态/配对/已配对设备/整屏刷新/设置）、配对窗口（大号配对码+倒计时）、已配对设备窗口、首次运行自动弹配对窗口、应用/托盘图标（4 色状态点）、版本信息 + 清单（Per-Monitor DPI v2、通用控件 6）、adb 缺失时只走局域网、设置/日志目录自动回退 %LOCALAPPDATA%；**安装包 `dist\Padreen-Setup-1.1.1.exe`（Inno Setup 7，默认装 D 盘，加防火墙规则、装 VDD 驱动（已有则跳过）、卸载还原）**。安装包本机已实际运行（11-05 起多次；11-09 完成更名迁移并升级到 1.1.1，见 §3.1）。 |
| **触控反馈** | ✅ 手指点按/拖窗口/拖滚动条已通（Windows 触控注入，多指，不动鼠标指针，不改系统设置）；手写笔目前按手指处理（无笔压）；托盘「触控输入」开关，`touch=1`，`touch_mode=touch|mouse` |

## 3. 现场状态

**平板（HMW-W09）**：`com.padreen.app`（11-09 起；当前 **build 17 / v1.1.3**，已装）。USB 调试保持开启。**没有写入任何平板系统设置**。卸载：`adb uninstall com.padreen.app`。**没有摄像头**（`pm list features` 无 camera）。

**电脑**：
- **虚拟显示驱动**（用户装）：设备 `ROOT\DISPLAY\0000`「Parsec Virtual Display Adapter」；驱动包在 `C:\Windows\System32\DriverStore`，`mm.dll` 在 `...\drivers\UMDF`；注册表 `HKLM\SOFTWARE\Parsec\vdd\0/1`。卸载：管理员运行 `tools\vdd\pkg\uninstall-vdd.ps1`；状态：`tools\vdd\pkg\status-vdd.ps1`。
- **显示器**：`\\.\DISPLAY2` Dell S2725DSM 2560×1440 在 (0,0)（主屏）；`\\.\DISPLAY1` Surface 自带 2880×1920 在 (2560,-7)；虚拟屏出现时 1872×1404（位置由 Windows 记住：曾在 (5440,0)，用户后来把它摆到了 Dell 下方，(445,1440)）。
- **壁纸守护的副作用（需知道）**：Padreen 用 `IDesktopWallpaper::SetWallpaper(虚拟屏ID, 白图)` 只改这块屏。原值写在 `host\padreen\build\wallpaper-original.txt`（正常退出会还原并删除）；**异常退出（被杀进程）会遗留这个文件**，下次启动时自动识别并最终还原。其他两块屏的壁纸没动过（已用只读 COM 查询核对）。
- 临时文件、缓存都在 D 盘；`.tmp\lc-eink.txt`、`.tmp\slices\` 是已按标签过滤的 logcat。

**产品化新增的环境 / 现场状态（11-03 晚）**
- **Inno Setup 7.1.1** 装在 `D:\Tools\InnoSetup`（官方 GitHub 发布，SHA-256 + Authenticode 已核对，`/CURRENTUSER`，无需管理员；卸载入口在 HKCU）。用 `tools\make-installer.ps1 [-Build]` 生成 `dist\Padreen-Setup-1.1.1.exe`。
- **安装包已端到端实测**（用户同意后在本机运行，UAC 一次）：装到 `D:\PadreenTest` → 文件/开始菜单/卸载项/路径专属防火墙规则 `Padreen LAN TCP|UDP (<exe 路径>)`（TCP 38702、UDP 38703，`remoteip=localsubnet`、`profile=any`）都对；VDD 驱动已存在 → 跳过且不标记"由安装包装的"；装好的副本启动后 USB 自动配对成功（新电脑身份）；**卸载（程序在运行时）**：优雅退出程序、删除目录/开始菜单/卸载项/防火墙规则，原有 VDD 驱动保留。
- **本机残留（无害）**：旧版（无路径）防火墙规则 `Padreen LAN TCP` / `Padreen LAN UDP` 仍在（需管理员才能删：`netsh advfirewall firewall delete rule name="Padreen LAN TCP"`）；开发副本现有路径专属规则 `…\host\padreen\build\padreen.exe`。平板配对列表里有一条"不在线（已配对）"的旧条目（安装包测试副本的身份）——在平板主页点「忘记」即可。
- 平板主页的"自动连接"= 连接**当前在线的已配对电脑**（最近用过的优先）；失败后每 15 s 重试；PC 主动退出（BYE）后 60 s 内不重连。**会话不再属于界面**：`DisplayHub`（进程级）持有网络会话 + 渲染器 + 影子缓冲区，`SessionService`（前台服务，常驻通知）保活；`MainActivity` 只负责把 e-ink 表面 attach/detach（回到前台时整屏 GC16 重绘）；Back 只是离开画面，主页顶部横幅「返回显示 / 断开」；LAN 会话意外中断时平板在后台自动重连（最多 2 分钟）。会话里 10 s 没有任何数据判定断线。平板 App **build 11**（`APP_BUILD`/`versionCode`/PC `requiredAppBuild` 三处一起升）。
**合盖 / 切出去 相关的现场状态（11-03 夜）**
- **本机是 Surface Pro 8**（Modern Standby；「合盖」= 合上 Type Cover）。电源计划「平衡」的「合上盖子时」：插电 0（不采取操作）、电池 1（睡眠）；该选项在控制面板里被 Windows 隐藏（Attributes=1），只能按 GUID `5ca83367-6e45-459f-a27b-476b1d01c936`（子组 `4f971e89-…`）读写，**写入需要管理员**，所以必须由服务来做。
- **电源助手**：服务 `PadreenPower`（LocalSystem，自动启动，二进制 = 安装目录里的 `padreen.exe --power-service`）。电脑端程序连接期间每 3 s 给命名事件 `Global\PadreenLidHold` 发心跳（任何登录用户可发信号，DACL：SYSTEM/管理员全部、认证用户 MODIFY+SYNCHRONIZE），服务第一次收到时把原值存到 `%ProgramData%\Padreen\lid-state.ini`（仅 SYSTEM/管理员可写）再改成 0，**15 s 没心跳 / `Global\PadreenLidRelease` / 服务停止 / 服务启动（崩溃后恢复）都会还原**。已实测：心跳 → DC 1→0；释放立刻还原；停止心跳 14 s 后还原。
- **安装目录必须只允许管理员写**（服务以 SYSTEM 运行该目录里的 exe，目录用户可写 = 提权漏洞）：安装包 `icacls "{app}" /inheritance:r /grant:r SYSTEM:F Administrators:F Users:RX`，再 `icacls "{app}\*" /reset /T`（**坑：直接 `/T` 加 `/inheritance:r` 会让文件没有任何权限，exe 打不开**）。因此装好后设置/配对/日志放 `%LOCALAPPDATA%\Padreen`（`dataDir()` 自动回退；`wallpaper-original.txt` 也在那里）。
- 本机已用安装包装到 `D:\Padreen`（含服务，非驱动：VDD 早就有）。开发副本（`host\padreen\build`）与安装副本可以各自运行（不要同时），两者共用同一个服务。
- **幽灵进程**：旧版 Padreen 退出后进程会残留（2 个线程在等，持有 mutex/文件几十分钟，用户态杀不掉）；根因在 CRT/DLL 关闭阶段（残留线程分别在 ntdll 线程池、`mswsock.dll`、`combase.dll`），现在 `wWinMain` 末尾 `TerminateProcess(自身)`，退出 0.6 s。本机仍有一个 22:48 的旧幽灵（重启电脑才消失，无害）。
**11-05 现场状态（历史）**：当时用户运行的是**安装版**（旧名 PaperDisplay，`D:\PaperDisplay`）；平板 App = build 12；……（现在的状态见下面 §3.1）。后台曾有一个 `adb logcat` 抓取在写 `.tmp\lc-gest.txt`。安装包升级前会先结束安装目录里的 adb 服务；**部署需要用户能看到并批准 UAC（平板是唯一屏幕时看不到，要打开盖子/接显示器）**。

### 3.1 11-09 更名后的现场状态与迁移（以此为准）
- **电脑**：已用 `Padreen-Setup-1.1.1.exe` 装到 **`D:\Padreen`**（Inno Setup，新 AppId `{162D520D-0087-4422-96F0-E36B5AF01211}`，卸载入口「Padreen」）；服务 **`PadreenPower`**（自动启动，二进制 `D:\Padreen\padreen.exe --power-service`）；防火墙规则 `Padreen LAN TCP|UDP (D:\Padreen\padreen.exe)`；开始菜单「Padreen」。**数据在 `%LOCALAPPDATA%\Padreen`**（`padreen.ini` 的节名 `[padreen]`、`devices.ini`、`padreen.log`）。**没有勾「开机自动启动」**（用户原来就没开）：关机重启后要手动运行 Padreen。
- **从旧版迁移（安装包 + 程序自己做）**：安装包 `PrepareToInstall` → ①退出旧托盘程序（窗口类 `PaperHostTray`）②停并删旧服务 `PaperDisplayPower`（停服务会还原合盖动作）③`Get-NetFirewallRule 'PaperDisplay LAN*'` 全删（含开发副本路径的规则）④**静默运行旧卸载程序**（不动 VDD 驱动：旧安装从没标记过"由我们装的"；若标记过，先清掉旧标记再卸载并把标记转给新键）⑤删旧目录。程序首次运行 `migrateLegacy()`：把 `%LOCALAPPDATA%\PaperDisplay` 的 `paperhost.ini`（`[paperhost]`→`[padreen]`）、`devices.ini`（电脑身份 + 配对密钥原样保留）、`wallpaper-original.txt`、日志搬到新位置；旧「开机自启」值（`PaperDisplay`）在旧程序文件已不存在时转成 `Padreen`。平板：USB 连一次，`prepareTablet()` 装新 App `com.padreen.app` 后 `adb uninstall com.paperdisplay.app`；新 App 主页还有「卸载旧版」按钮；USB 配对新身份时按"同型号 + 同 IP"去掉旧条目。
- **踩坑（已修）**：旧卸载程序在 `/VERYSILENT /SUPPRESSMSGBOXES` 下会把「要同时删除 AppData 里的设置和配对记录吗？」**答成"是"**——第一次部署时它把 `%LOCALAPPDATA%\PaperDisplay` 删光了（设置 + `devices.ini` + 日志）；当时从部署前做的备份 `D:\Workspace\_backup\PaperDisplay-appdata-20261109\` 恢复后程序才接手成功。现在安装包在运行旧卸载程序前先把该目录改名为 `PaperDisplay.keep`、之后改回（`Padreen.iss` 的 `RemoveLegacyProduct`）。
- **备份**（`D:\Workspace\_backup\`，可删）：`MatePadPaper-src-before-padreen.zip`（改名前的源码）、`PaperDisplay-appdata-20261109\`（旧数据 + `_state-before.txt` 现场快照）、`PaperDisplay-Setup-1.0.0.exe`（旧安装包，回滚用；`dist\` 里也有一份）。
- **平板（HMW-W09）**：只装 `com.padreen.app`（旧 `com.paperdisplay.app` 已被 PC 卸载；没有探测 App）。要更新平板 App：直接 `adb install -r android\app\build\padreen.apk`（平板上点「继续安装」），或让 Padreen 自动做（PC 的 `requiredAppBuild` 高于平板的 appBuild 时）。
- **已知遗留（无害）**：`devices.ini` 里还有一条旧身份的 HMW-W09（IP 192.168.43.8，旧 App 的密钥，永远用不上了），可在「已配对的平板…」里删。电脑音量被测试按键从 34% 调到了 18%（用户自己调回即可）。
- **会话中 Windows 的副作用（正常）**：只要平板虚拟屏的壁纸和真实屏幕不同，Windows 会把 `HKCU\Control Panel\Desktop\Wallpaper` 暂时改成 `...\Themes\TranscodedWallpaper`（同一张图的缓存副本）并多出 `TranscodedImageCache_001`；会话结束、虚拟屏被拔掉后恢复。真实屏幕的壁纸路径全程没变（`wp-query.ps1` 只读核对过）。
- **只读检查脚本**（`.tmp\`）：`wp-query.ps1`（IDesktopWallpaper 全部显示器 + 注册表）、`vol-watch.ps1`（监视电脑主音量）。

### 3.2 2026-11-10 换到新电脑（Dell Pro Max Tower）后的构建环境（以此为准）
- **仓库在 `D:\GithubProject\Padreen`**（不再是 `D:\Workspace\MatePadPaper`；本文件里旧路径是历史，脚本已全部改成按脚本位置推导）。原开发机（Surface Pro 8）上的旧开发密钥 `dev.jks`（已作废，见下）、Windows SDK NuGet 包等**不在**这台电脑上。
- **一条命令配环境**：`tools\setup-build-env.ps1`（`-CheckOnly` 只检查）。工具位置由 `tools\toolchain.ps1` 统一查找；本机实际是：MSVC 14.42 + Windows SDK 10.0.22621（VS 2022 Professional 在 C:，经 vswhere + vcvars64.bat）、JDK 11.0.33（微软构建，`D:\Tools\jdk-11.0.33`）、Android SDK `D:\Android\SDK`（`ANDROID_HOME`；本次用它自带的 sdkmanager 加了 build-tools 32.0.0，许可证早已接受过）、Inno Setup 7.1.1（`D:\Tools\InnoSetup`）、GitHub CLI 2.103.3（`D:\Tools\gh`）、7-Zip（choco 的 shim）。没有改 PATH、没有设任何环境变量。公司机器：执行策略 RemoteSigned，一律用 `-ExecutionPolicy Bypass -File` 跑脚本。
- **复现验证**：本机构建的 APK（build 17 / v1.1.3，185,663 字节）与平板上装着的那个，14 个条目（`classes.dex`、`AndroidManifest.xml`、`resources.arsc`、全部资源）**逐字节相同**，只有签名证书不同——说明本机工具链能复现原来的构建，仓库当前源码就是平板上的 build 17。
- **签名密钥（2026-11-10 起换成新的发布密钥，旧的作废）**：`tools\keys\release.jks`（别名 `release`，`CN=Padreen`，RSA 2048，2054-03 到期）+ 同目录 `release.jks.pass`（随机口令，不写进任何文档）。由 `android\app\build.ps1 -NewKey` 在本机生成，目录 ACL 只留当前用户和 SYSTEM，`.gitignore` 排除（`tools/keys/`）。证书 SHA-256 `e8fc0fe2666acc90f36a2cb4ed28ca20a12fbdabee7d24bc8f11aaba9d4c60c4`。**务必备份 `tools\keys`**（丢了就不能再升级已装的平板）。没有密钥时 `build.ps1` 拒绝构建（不再悄悄新建，也没有默认口令）。
  - **私钥没有上传到 GitHub**：仓库是公开的，私钥一旦公开，任何人都能签出 Android 认可的"升级包"，而且无法吊销（用户曾要求一同上传，已说明并改为本机备份；要在 GitHub Actions 里出包，请自己把密钥 base64 后放进仓库的 Actions Secrets）。
  - **旧开发密钥**（`CN=PaperDisplay Dev`，证书 SHA-256 `a1271d70e0d38c2628fa36b3761c9bcf32d3eefff9b279108536042e457193b9`，口令 `paperdisplay` 已在首个提交里公开）作废，文件也不在这台电脑上。2026-11-10 用户把平板上旧密钥签名的 App 卸载了，我用 `adb -s <平板序列号> install -r` 装上了新密钥签名的 build 17（v1.1.3，与 `dist\Padreen\padreen.apk` 逐字节相同，SHA-256 `938B36FC…1B85`；pm path 与签名证书都核对过）；配对记录随卸载清掉，插 USB 后 Padreen 自动重新配对。以后若有平板还装着旧密钥签名的版本：电脑端只有平板 appBuild < `requiredAppBuild`（16）时才去覆盖安装，所以它继续能用；将来 `requiredAppBuild` 升高，或想换成新密钥的 App 时，要先在平板上卸载 `com.padreen.app`（清掉配对），再插 USB——Padreen 自动装新版并自动重新配对（装不上时托盘提示里有这一步）。
- **发布**：`tools\make-installer.ps1 -Build` 出安装包和绿色版目录（出包时会用刚做好的 exe 实测"能从 Parsec 取到并校验驱动"，并断言包里没有任何 Parsec 文件）；用 GitHub CLI 发布（仓库 `1031193940/Padreen`，登录账号需要写权限）。流程：①`D:\Tools\gh\bin\gh.exe auth login` 登录一次；②提交并推送：作者身份沿用首个提交，用 `git -c user.name=… -c user.email=… commit` 传入（不改 git 配置），提交信息末尾加 `Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>`；推送用 gh 当 credential helper，只在这一次进程里生效：环境变量 `GIT_CONFIG_COUNT=2`、`GIT_CONFIG_KEY_0=credential.helper`（值留空）、`GIT_CONFIG_KEY_1=credential.helper`、`GIT_CONFIG_VALUE_1=!"D:/Tools/gh/bin/gh.exe" auth git-credential`、`GIT_TERMINAL_PROMPT=0`；**暂存时点名文件（`git add -u` + 新文件），不要 `git add -A`**；提交前扫一遍暂存的内容（私钥口令、`BEGIN ... PRIVATE KEY`、token、个人/公司信息）；③打 tag `v<版本>` 并推送；④`D:\Tools\gh\bin\gh.exe release create v<版本> dist\Padreen-Setup-<版本>.exe --repo 1031193940/Padreen --title … --notes …`。已发布的版本看 https://github.com/1031193940/Padreen/releases ；产物在 `dist\`（不进 git）。安装包本身还没有在干净电脑上真正安装试过（Inno 编译通过；驱动下载安装的端到端已在真机测过，见 §4 1b）。
- **USB 自动安装（2026-11-10 补强）**：本来就有（`prepareTablet`：设备到达 → `pm path` 没有 App，或 appBuild 低于 `requiredAppBuild` → `adb install -r`，平板上点一次「继续安装」）。这次补的：①`DeviceWatcher` 也报告 adb 看得到、但还不能用的设备（`unauthorized` / `offline`），空闲状态显示橙色提示（"请在平板上点允许 USB 调试"等）；②安装失败不再显示 adb 的原始输出，`explainInstallFailure()`（`adb.cpp`）翻成原因和办法（签名密钥不同 → 先卸载、用户取消/系统禁止、空间不足、降级、系统太旧…），原文进日志；缺 `padreen.apk` 单独提示；③连续 3 次失败后的"已暂停重试"保留真正的原因（以前被改写成"平板上的 App 已关闭"）；④`Adb::abortWhen(&quit)`：退出时直接结束还在等平板确认的 `adb install`（以前最多卡 10 分钟）；⑤识别 `ANDROID_ADB_SERVER_PORT`（与 adb 自己一致，默认 5037）。测试：假 adb 服务器 + 假 `adb.exe` + 本程序副本（不碰真设备，未入库），5 个场景共 16 项检查全部通过：未授权提示、授权后自动安装（`install -r` 一次）、签名不同、用户取消、3 次后暂停并保留原因、安装等待中退出（264 ms）。
- **UI 简化（2026-11-10，用户要求）**：每个选项只留几个字，去掉所有说明句（原则：不解释；只在确实要用户做决定的对话框里留一句话）。**功能一个没删，只是字少了。**
  - 电脑端（`ui.cpp` / `main.cpp` / `driver.cpp` / `adb.cpp` / `power.cpp`）：主界面 8 个开关 = Wi-Fi 连接 / 优先 USB / 触控 / 三指手势 / 自动旋转 / 纯白壁纸 / 开机启动 / 合盖不休眠，按钮 = 配对… / 已配对… / 全屏刷新 / 日志 / 关闭；「安装驱动…」按钮只在没装驱动时出现，「启用…」只在防火墙规则缺失时出现，合盖开关在没装电源助手时变灰并注明；配对窗口只留一行指引 + 配对码 + 剩余时间；托盘菜单、状态行、各类提示都改成短句（失败原因的细节进日志）。
  - 平板主页（`HomeActivity.java` 等）：副标题只剩版本，「搜索」「输入 IP」「自动连接」「三指手势」「自动旋转」「音量键控制电脑」；**去掉了「使用说明」按钮和 `showHelp()`**；列表为空只写「没有找到电脑」。安装包向导（`Padreen.iss`）的组件/任务名也缩成几个字。
  - README 与 `host\padreen\README-dist.txt`（装成 `使用说明.txt`）重写成 介绍 / 展示（`docs/images/panel.png`：电脑主界面截图，IP 是占位值）/ 使用，其余内容（目录、构建、发布、驱动、设计要点）只在本文件。
  - 验证：电脑端窗口用临时测试程序（`ui.cpp` + 假数据 + `PrintWindow`，不进库）逐个截图看过：主界面三种状态（正常 / 缺驱动 + 缺电源助手 + 防火墙未放行 / 长状态换行）、配对窗口三种状态（等待 / 过期 / 已配对）、已配对窗口；**别在用户的 Padreen 运行时再起一个真实实例**（单实例互斥体 `Local\PadreenSingleton` 会让它直接退出并只弹出已运行实例的主界面）。平板主页的新版 APK 已编译（`android\app\build\padreen.apk`），装到平板上目视检查需要先征得用户同意：会打断正在进行的显示会话。
  - **版本号没动**（电脑端 1.1.2；平板 build 17 / v1.1.3）。发布带新 UI 的版本时：电脑端升 `res/resource.h` 的 `PD_VER_STRING` 和 `Padreen.iss` 的 `AppVersion`；平板端 `AndroidManifest.xml` 的 versionCode / versionName 与 `Receiver.APP_BUILD` 升一版，并把电脑端的 `requiredAppBuild` 提到同一值，已装的平板才会被自动升级（平板 App 的文字变了，但 build 17 的平板不升级就还是旧界面）。
- **仓库速查（原 README 的目录表、构建段和设计要点，2026-11-10 起只放这里）**：
  - 目录：`host/padreen`（电脑端 C++：托盘、控制面板、配对窗口、DXGI 抓屏、编解码、VDD、触控注入、电源助手）· `host/poc`（早期 Python 原型与延迟测量，不参与产品构建）· `android/app`（平板 App `com.padreen.app`）· `android/app/test`（手势 / 方向 / 音量键的 JVM 单元测试）· `android/probe`（墨水屏驱动探测 APK）· `protocol/PROTOCOL.md`（线协议 v2.0）· `docs/probe-report.md`（实测与结论）· `PLAN.md`（方案与变更历史）· `tools`（环境准备、打包、探测脚本）· `installer/Padreen.iss`（Inno Setup 脚本）· `third_party/lz4`（BSD-2-Clause）。
  - 构建（都用 `powershell -ExecutionPolicy Bypass -File`）：电脑端 `host\padreen\build.ps1`（→ `host\padreen\build\padreen.exe`）；平板 APK `android\app\build.ps1 [-Install] [-Start]`（不用 Gradle：aapt2 + javac + d8 + apksigner；需要签名密钥）；平板端单元测试 `android\app\test\run-tests.ps1`；绿色版目录 `tools\make-dist.ps1 [-Build] [-SkipDriverCheck]`，安装包 `tools\make-installer.ps1 [-Build] [-SkipDriverCheck]`（`-SkipDriverCheck` 只给离线构建用，发布不要用）。各脚本经 `tools\toolchain.ps1` 找工具：环境变量（`PADREEN_TOOLS` / `PADREEN_JDK` / `PADREEN_ISCC` / `PADREEN_GH` / `ANDROID_HOME`）→ `D:\Tools` → 常见安装位置 → 原开发机的目录布局。`tools\fetch-vdd.ps1` 是开发辅助（把驱动包取到 `tools\vdd\pkg`，需要 7-Zip），不属于发布。
  - 边界：不需要 root，不改平板的系统设置；电脑端唯一的系统级改动是虚拟显示器驱动（用户同意后由程序从 Parsec 官方地址取得并安装，设备管理器里可卸载）；构建产物、第三方 SDK、Parsec 的文件、私钥、测试录像都不进 git（见 `.gitignore`）。
  - 设计要点：①**不走 FB 直写**——`FBHandSurface.postFrameBuffer` 绕过系统唤醒与节流，会让 T1000 控制器休眠后收不到更新、I²C 卡死 2.25 s 被硬复位（表现为拖窗口时连续黑白闪屏）；最终走 `SurfaceView.lockCanvas(脏矩形)` + `EPDCDevice.setForceNextPostMode` + `unlockCanvasAndPost`，由系统负责唤醒与节流，压力测试零复位。②极速模式 = DU/A2 波形纯黑白，停手 1.2 s 后用 GL16 对改动区域补灰阶；自动整屏刷新与自动补灰阶默认关闭（用户手动触发：三指轻点）。③刷新要懒：网络线程只更新影子帧缓冲与待刷矩形，渲染线程按节奏取最新内容提交，系统忙时自然合并，不排旧帧。④虚拟屏自动设纯白壁纸（仅平板那一块，退出还原）：深色大图在 1-bit 墨水屏上是满屏抖动点，是重影 / 闪屏的主因。

**已装环境（D 盘；驱动除外）**

| 内容 | 位置 |
|---|---|
| Android NDK r27d（`llvm-objdump` 等） | `D:\Android\android-sdk\ndk\27.3.13750724` |
| MSVC 14.34（VS 2022 Pro，**装在 D:**）、CMake、Ninja | `D:\Visual Studio Professional 2022\IDE\VC\Tools\MSVC\14.34.31933` |
| Windows SDK 头文件/库（NuGet 10.0.26100，免安装） | `third_party\winsdk`（`third_party\nupkg` 可删） |
| LZ4 v1.10.0 源码（`lz4.c/.h` 平铺在目录里） | `third_party\lz4` |
| Python 3.8 + numpy/Pillow；`mss`、`imageio-ffmpeg`（自带 ffmpeg 4.2.2） | `D:\Python3.8`、`.pydeps` |
| Parsec VDD 包（**开发用，不分发**） | `tools\vdd\pkg`（脚本 + `NOTICE.txt` 在 git 里；0.41 驱动 + 已签名 nefconw 由 `tools\fetch-vdd.ps1` 从官方地址取得并校验，二进制不进 git，任何发布包里都没有）；`padreen.exe` 内置安装包与驱动文件的 SHA-256（`host\padreen\src\driver.cpp`），首次运行/托盘菜单时**在用户电脑上**从 Parsec 官方地址下载、解包（`vddpack.cpp`，不需要 7-Zip）、安装 |
| 7-Zip（Unity 自带） | `D:\Unity\2022.3.62f3c1\Editor\Data\Tools\7z.exe`（只有开发脚本 `fetch-vdd.ps1` 要用，可选） |
| 发布签名密钥（口令在同目录 `release.jks.pass`；**不进 git，务必备份**） | `tools\keys\release.jks` |

## 4. 恢复清单（下次从这里开始）

```powershell
# -1) 新电脑 / 环境坏了：先配构建环境（见 §3.2；缺什么装什么，-CheckOnly 只检查）。下面命令里的 D:\Workspace\MatePadPaper 是原开发机的历史路径，换成仓库所在目录（现在是 D:\GithubProject\Padreen）即可，脚本本身已不依赖它。
& "$PSHOME\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File tools\setup-build-env.ps1
#     发布：tools\make-installer.ps1 -Build（需要 tools\keys\release.jks：没有就先 android\app\build.ps1 -NewKey，见 §3.2「签名密钥」「发布」）；构建环境变量 PADREEN_TOOLS / PADREEN_JDK / PADREEN_ISCC / PADREEN_GH / PADREEN_KEYSTORE / PADREEN_KEYSTORE_PASS 可覆盖默认位置

# 0) 日常使用：运行 padreen.exe（安装包装到 D:\Padreen；开发时是 host\padreen\build\padreen.exe）。托盘左键 = 控制面板，右键 = 快捷菜单；平板打开 Padreen 自动连接。
#    命令行：--console（附带控制台输出）  --debug（额外记录大面积变化帧，并把配对码写进日志——只用于测试）  --gray（画质模式启动）  --debug-touch（逐帧打印触控）  --autostart-on / --autostart-off（安装包用，设置本用户开机启动后退出）  --verify-driver [--package 文件夹]（不需要管理员：核对驱动包是不是这个 exe 要的那份，默认看程序目录的 driver 文件夹，退出码 0 = 是；3 缺文件、4 与内置哈希不符）  --fetch-driver --cache 文件夹（不需要管理员、不弹窗：像首次运行那样从 Parsec 取驱动并核对，只写 --cache 下面；make-dist.ps1 用它当发布检查，退出码 0 = 成功，8 下载失败，9 下载到的不是预期的文件，10 解包/写入失败）  --install-driver [--package 文件夹] [--log 文件]（需要管理员：驱动安装的提权部分，由程序自己经 UAC 拉起，不用手动运行）
#    配置：padreen.ini + 配对列表 devices.ini（密钥 DPAPI 加密）在程序目录（不可写则 %LOCALAPPDATA%\Padreen）；主界面里的设置会写回。关键项：lan=1 lan_port=38702 lan_discovery_port=38703 pair_minutes=5 usb=1 fast_wave=a2f6 settle=off touch=1 …（见 padreen.ini.sample）
#    局域网：平板打开 Padreen（主页）→ 自动发现 → 点「配对…」输入电脑「配对新平板」窗口里的 9 位码（或 USB 连一次自动配对）→ 之后自动连接。usb=0 = 忽略 USB 线只走局域网（测试用）。防火墙规则：安装包添加；绿色版首次运行弹窗引导（UAC 一次）；托盘「启用局域网连接（需要允许防火墙）」可重试。
#    日志：padreen.log（UTF-8，>2 MB 自动轮转）。退出请用托盘「退出」（会还原壁纸、拔虚拟屏、让平板退出显示）；脚本里用 `python host\poc\phctl.py quit|full|reconnect|fast|gray|lan|pair|devices|open|touch|du|a2f6|settle-off|…`（给托盘窗口发 WM_COMMAND）。

# 1) 构建 Padreen（输出 host\padreen\build\padreen.exe，并复制 padreen-white.png）
& "$PSHOME\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File D:\Workspace\MatePadPaper\host\padreen\build.ps1
#    Padreen 自带的 APK 是 host\padreen\build\padreen.apk —— 重新构建平板 App 后要复制过去：
#    Copy-Item android\app\build\padreen.apk host\padreen\build\padreen.apk -Force   （appBuild 比 Padreen 要求的低时，Padreen 会自动 adb install -r）

# 1b) 虚拟显示器驱动（Parsec VDD 0.41 + nefconw）：**不进任何发布包**（Parsec 的软件，没有找到允许第三方再分发的授权；2026-11-10 查证：Parsec 官方把它当作 Teams/Warp 功能，通过 Parsec 应用下载，条款里没有授予再分发的内容）。
#    程序在用户电脑上自己取：首次运行/托盘「安装虚拟显示器驱动」→ 说明并询问 → 后台线程（界面不卡）用 WinHTTP 从 https://builds.parsec.app/vdd/ 下载 parsec-vdd-0.45.0.0.exe（取 nefconw.exe）和 parsec-vdd-0.41.0.0.exe（取 mm.inf/dll/cat），
#    先核对安装包 SHA-256，再由 vddpack.cpp 解包（NSIS：每个文件是独立的 raw deflate blob；按内容 SHA-256 挑文件，不信压缩包里的名字；自带 inflate，不需要 7-Zip），再核对 4 个文件，放进缓存 <设置文件夹>\driver-cache（安装包+pkg），然后经 UAC 拉起 padreen.exe --install-driver --package <pkg>。
#    查找顺序：<程序目录>\driver（离线/开发时手放的完整包）→ driver-cache\pkg → driver-cache 里的两个安装包（手动下载放进去也行）→ 联网下载。任何哈希不符 → 拒绝使用并提示更新 Padreen。
#    发布检查：make-dist.ps1 用刚做好的 exe 跑 --fetch-driver --cache .tmp\vdd-check（真实下载），并断言 dist 里没有 nefconw.exe / mm.* / parsec-vdd-*.exe；Parsec 换文件时出包会失败 → 更新 driver.cpp 里的哈希（和 fetch-vdd.ps1）。
#    开发用：tools\fetch-vdd.ps1（需要 7-Zip）把同样的 4 个文件取到 tools\vdd\pkg，给旁边的 install-vdd.ps1 / uninstall-vdd.ps1 / status-vdd.ps1 用（这些脚本不分发）
& "$PSHOME\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File D:\Workspace\MatePadPaper\tools\fetch-vdd.ps1
#    换驱动版本：同时改 tools\fetch-vdd.ps1 和 host\padreen\src\driver.cpp 里的 SHA-256（kSources 两个安装包 + kPins 四个文件）；padreen.exe --verify-driver --package tools\vdd\pkg 可核对两者是否一致
#    安装卸载：安装包（Inno）不再带驱动、不装也不卸驱动（卸载 Padreen 不动驱动；卸驱动 = 设备管理器里卸载设备并勾选删除驱动程序，或开发用 uninstall-vdd.ps1）。ini 里 driver_declined=1 表示用户在首次提示时选了"否"（托盘菜单仍可装）
#    提权部分：先核对哈希、复制到只有管理员能写的 C:\Windows\Temp\Padreen-vdd-*，再用 nefconw.exe 装；--package 指向用户可写的缓存目录是安全的，因为每个文件都按内置哈希核对，装的是核对过的那份字节
#    测试（2026-11-10，真机 + 真实 UAC）：卸载驱动后 → 测试副本首次启动 → 提问框 → 下载（0.6 s）→ UAC → 安装成功，设备 OK、1872×1404@60 / 1404×1872@60 已登记，程序全程可响应、可正常退出；另有单元测试（.NET DeflateStream 生成的 stored/fixed/dynamic 向量、真实安装包、截断/比特翻转/噪声）和 13 项 CLI 检查（缓存复用、损坏的缓存安装包被拒并重下、被篡改的包被发现并重建）
#    ShellExecuteEx 的坑：调用线程没初始化 COM 时它转到后台线程立刻返回、拿不到进程句柄（等不到 UAC 和安装结果）→ 要 CoInitializeEx + SEE_MASK_NOASYNC（driver.cpp 已处理）

# 2) 构建并安装平板 App
$env:TEMP='D:\Workspace\MatePadPaper\.tmp'; $env:TMP=$env:TEMP
& "$PSHOME\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File D:\Workspace\MatePadPaper\android\app\build.ps1 -Install
#    注意：adb install 会在平板上弹出华为"安装成功"页面（来源：PC 工具），可能要等/点一下

# 2b) 重新生成图标（源图 assets\icon\source.png）→ assets\icon\*.ico、android\app\res\mipmap-*\n& D:\Python3.8\python.exe tools\make-icons.py\n\n# 2c) 打包：staging 文件夹 dist\Padreen + 安装包 dist\Padreen-Setup-1.1.1.exe（Inno Setup 7 在 D:\Tools\InnoSetup；-Build 先重建 APK 和 exe）\n& "$PSHOME\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File D:\Workspace\MatePadPaper\tools\make-installer.ps1 -Build\n\n# 3) adb 服务（WMI 拉起；Padreen 自己也会拉起，且不继承句柄）
Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = '"D:\Android\android-sdk\platform-tools\adb.exe" -P 5037 start-server' }

# 4) 过滤 logcat（后台）+ 压力测试 / 延迟测量
adb logcat -c; adb logcat -v threadtime Padreen:V EinkSfpatchData_T1000:V T1000EPDCDevice:V hwtconepdcdisplay:V t1000_lib:V DISPLAY_DITHER_V201_ALGO:V '*:S' > .tmp\lc-eink.txt
D:\Python3.8\python.exe host\poc\stress.py --log .tmp\lc-eink.txt --gap 3 --plan "flip:w=900,h=720,hz=10,sec=5,mode=du;idle:sec=3"     # 先退出 Padreen（平板 App 一次只接一个连接）
#    App 用 `adb shell am start -n com.padreen.app/.MainActivity --ez trace true` 启动才有 POSTED 日志

# 5) 手机录像测延迟（需要先退出 Padreen）
#    a) 单模式：tools\latency-test.ps1 → analyze_auto.py <视频>
#    b) 各波形模式对比：tools\latency-modes.ps1（7 种模式各 25 s，参考窗口上有倒计时）→ docs\video\ → analyze_modes.py <视频> --flips .tmp\modes-flips.csv --waves .tmp\modes-waves.csv
#    c) 细分时间线：mirror.py --trace-csv + latency_pattern.py --flip-log + 过滤 logcat → trace_latency.py
```

## 5. 后续任务（按优先级）

1. **请用户验证新的局域网流程**：平板主页（自动发现/配对码/手动 IP/忘记/自动连接）、返回手势退出显示后不自动重连、电脑端控制面板与配对窗口、首次运行引导；以及**安装包**（需管理员：防火墙规则 + 驱动（已有则跳过））。若 Wi-Fi 下体验好 → 再考虑"USB→Wi-Fi 无缝切换"。
2. **各波形模式延迟对比**：已完成粗对比（probe-report §13），用户选定 A2 6 帧（默认）。要更可靠的数据需要平板和手机都固定、每模式 ≥20 次、含 DU4/A2·MIX——优先级低。
3. **Padreen 打磨**：
   - **用户肉眼确认整屏刷新是否少了**（默认已无自动整屏/自动补灰阶；若觉得 A2 文字太硬，可在托盘「自动补灰阶」选 少量/完整）；三指轻点实机验证（三指手势没法用 adb 模拟）。
   - A2 残影靠手动整屏刷新清；若需要可加"累计 N 次快速更新后在托盘提示该刷新了"（只提示，不自动刷）。
   - 画质：1-bit 的文字阈值/抖动；深色主题窗口的观感。
4. 平板端打磨：`mono1→gray` 展开 NEON/JNI；渲染间隔 60 ms 调参；Wi-Fi 的 `WifiLock` 在这台设备上不起作用（保活包代替）。
5. **触控后续（可选）**：手写笔支持（`TOOL_TYPE_STYLUS` → Windows 笔注入 `InjectSyntheticPointerInput(PT_PEN)`，带压力/倾斜；橡皮 = `PEN_FLAG_ERASER`）；手掌误触；两指滚动/捏合在各 App 里的表现；触控按下时的视觉反馈（墨水屏上 Windows 的触控圈被关了，应用自己的按下态会显示）。
6. 发布：安装包已生成；**未签名**（SmartScreen 会提示"未知发布者"，需代码签名证书才能消除）；可选：自动更新、平板 App 的网页下载（电脑端起一个小 HTTP 服务供平板浏览器下载 padreen.apk）、手写笔压力、PC 摄像头扫平板上的二维码（平板无摄像头，扫码只能反过来做）。

## 6. 关键经验（别再踩）

1. **壁纸是头号敌人**：深色大图壁纸 → 满屏 Bayer 抖动 → 拖窗口露出大片点阵 + 补灰阶大块 → 重影/闪屏/卡。纯白壁纸后一切明显好转。
2. **补灰阶（GL16）要克制**：GL16 被系统节流为 ~0.55 s 一次且占用 T1000 ~0.8 s，期间的快速更新会排队 → 卡。现在：停手 1.2 s 先补 1 块（≤12% 屏、文字块优先，只补"1-bit 比灰度明显差"的瓦片，平坦深/浅色吸附不补），停 4 s 补到 3 块，停 10 s 补完；**整屏灰度帧 3 s 内最多一次**；整屏黑白闪的"自动清残影"默认关。
3. **"大面积变化"别当动画拖延**：拖大窗口的脏面积就能超过 30%，延迟发送会变成 0.8 s 一刷 → 卡。现在只对 ≥80% 的整屏重绘去抖（120 ms 平静或 500 ms 上限），指针更新永不被拖。
4. **插虚拟屏后 Windows 会做壁纸淡入动画（约 +3 s 开始）**：首帧要等 ≥4.5 s 且平静 1 s，否则连闪三次整屏。
5. DXGI 桌面复制：指针单独给（含"反色"光标要自己处理）；虚拟屏在 Intel 核显上 `DuplicateOutput` 正常；`ACCESS_LOST` 要重建。
6. **电脑端进程会被 Windows 限频**：所有进程启动调用 `winperf`（Python）/`boostProcess()`（C++）：关 EcoQoS、1 ms 定时器、高优先级；抓取线程用 MMCSS "Games"。
7. 平板 App 一次只接受一个连接；开着 Padreen 时跑 `stress.py`/`mirror.py` 会连不上 → 先退出 Padreen（托盘菜单或给它的消息窗口发 `WM_COMMAND 1007`）。
8. 华为 `adb install` 会在平板上弹**「风险提示 → 继续安装」系统确认**（必须由用户点，不能代点）；Padreen 安装/升级 App 时状态栏会提示，超时 10 分钟。App 只在协议/版本变化时才需要升级。
9. 详细的 T1000/SF 机理见 probe-report §5、§10、§11。
10. **Wi-Fi 要保活**：平板 Wi-Fi 省电让 ping 平均 114 ms；电脑每 50 ms 发 PING 保活后降到 ~4 ms（§14，LAN 会话同样有效）。**局域网架构（PD2）：电脑监听、平板主动连**（因为要"平板端自动发现电脑"）→ 电脑必须有一次防火墙放行（入站 TCP 38702 / UDP 38703，限本地子网）；绿色版首次运行引导，安装包直接加。
13. **PowerShell/工具坑**：`$s.Replace(a,b,1)` 在 PS 5.1 无此重载；`$a -lt $b` 才是比较（`<` 是重定向）；单引号字符串里的 `` `n `` 不是换行；文件混用 LF/CRLF 时锚点会匹配不上 → 先统一换行。**平板上有人（用户）同时操作时，adb 模拟点击会和用户的输入混在一起**——测试前先问/等。**从隐藏窗口启动的进程第一次 `ShowWindow` 会被 STARTUPINFO 吞掉**（已在 ui.cpp 处理）。
12. **测试触控/窗口位置的脚本必须 DPI 感知**（`SetProcessDpiAwarenessContext(-4)`），否则 Tk/Python 报的窗口坐标是虚拟化的（虚拟屏缩放 150%）；用 `adb shell input tap/swipe` 可以模拟手指（toolType UNKNOWN、无面积）。Padreen 加 `--debug-touch` 会逐帧打印触控帧。
11. 升级平板 App 要改 `requiredAppBuild`（`main.cpp`）并同步 `Receiver.APP_BUILD` / manifest `versionCode`，重新构建 APK 后复制到 `host\padreen\build\padreen.apk`；每次升级用户都要在平板上点一次「继续安装」；新旧 APK 必须是同一把密钥签的，否则 `adb install -r` 失败（`INSTALL_FAILED_UPDATE_INCOMPATIBLE`），平板上要先卸载再装（托盘提示会这么说）。不要在用户使用时 `Stop-Process` 杀 Padreen（窗口会被 Windows 搬回主屏；用 `phctl.py quit`）。

## 7. 工具坑（省得再踩）

1. 工具 shell 是 **Windows PowerShell 5.1**，执行策略禁止 `.ps1` → 用 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File ...`；**不能 dot-source**，要 `Invoke-Expression (Get-Content x.ps1 -Raw)`（构建脚本这样加载 `vcenv.ps1`）。
2. `build-tools` 的 `d8.bat`/`apksigner.bat` 在 JDK 11 下挂 → 构建脚本直接 `java -cp d8.jar` / `java -jar apksigner.jar`。
3. 在工具 shell 里直接跑 `adb ...` 且服务没起，拉起的服务会继承管道导致挂起 → 先用 WMI 拉起；Padreen 里用 `DETACHED_PROCESS` + 不继承句柄。
4. **PowerShell 字符串的坑**（本轮多次中招）：双引号字符串里 `\n` 不是换行（会把字面 `\n` 写进代码）；反引号是转义符（Markdown 的 `` `a `` `` `t `` 会变成响铃/制表符，把文档写坏）；弯引号 “ ” 会被当成字符串边界。→ 改代码/文档用 `edit`/`write` 工具或单引号 here-string，不要用 `$s.Replace("..多行..")`。
5. `edit`/`write` 工具要求文件在上次读取后没被别的方式改过（PowerShell 改过要重新 `read`）。
6. `ask_user_question` 会阻塞到用户回答；用户可能看到问题就以为"结束"→ 需要长时间配合的测试要在被测窗口上直接显示倒计时/结束提示。
7. screencap 对本 App 只能得到黑图（`FLAG_SECURE`）；看平板内容只能靠肉眼/相机；看**虚拟屏内容**可以用 `mss` 截电脑上的那块屏。
8. logcat：事后 `logcat -d` 会因缓冲区刷满丢记录；未过滤的 logcat 含他人隐私 → 只抓过滤后的标签，不落盘/不打印未过滤日志。`.tmp` 里别放和标准库同名的脚本（`dis.py` 会被 `import dis` 误导入）。
9. `getPowerStatus()` 恒为 3，看不出 T1000 是否休眠。
10. 逆向 SF 补丁：`llvm-objdump -d --no-show-raw-insn`（NDK）；库文件在 `docs/probe/files`（`docs/probe/getprop.txt` 含设备序列号，不要公开）。
11. COM `IDesktopWallpaper` 要在 STA 线程调用；PowerShell 里可用 `Add-Type` 的 C# 互操作（IID `B92B56A9-8B55-4E14-9A89-0199BBB6F93B`，CLSID `C2CF3110-460E-4FC1-B9D0-8A1C0C9CC4BD`）做只读查询。
12. 旧探测 App 命令仍可用：`tools/probe.ps1 -Cmd "..."`。

## 8. 下次需要用户

- **验证局域网/产品化**：① 平板主页界面是否顺手（列表、配对键盘、「手动输入电脑地址」「使用说明」、勾选"自动连接上次使用的电脑"）；② 配对码流程；③ 返回手势退出后停留在主页、重新打开才连；④ 电脑端控制面板/配对窗口/已配对的平板；⑤ **是否同意我在你的电脑上运行一次安装包做端到端测试（会弹 UAC；会更新防火墙规则；装完我会再卸载）**。
- 验证：三指同时轻点 = 整屏刷新；整屏刷新是否确实只在手动时发生；A2 下残影/文字观感是否可接受（要不要开自动补灰阶）。音量键已恢复为系统音量。
- 反馈：使用中哪些场景仍觉得慢/闪/重影（A2 6 帧的残影是否可接受）；是否要开机自启（托盘里自己勾，需用户同意）；触控：滚动条/小目标好不好点、要不要手写笔（带压力）支持、有没有误触。
