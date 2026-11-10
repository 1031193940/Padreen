# Phase 0 探测报告（MatePad Paper / HMW-W09）

> 2026-11-01 · 来源：真机实测（探测 App `com.paperdisplay.probe`）+ 对 `eink.framework.jar` 与原生库的反编译。
> 原始日志在 `docs/probe/run-*.txt`，反编译与厂商文件在 `docs/probe/dex|files`（含厂商二进制，不要提交到公开仓库）。
> **命名说明（2026-11-09）**：产品后来更名为 **Padreen**。本报告是按日期写的研究记录，里面的 PaperDisplay / PaperHost / `paperhost.exe` / `host/paperhost` / `host/poc` 里的旧路径 / logcat 标签 `PaperDisplay` 都是当时的旧名，现在分别对应 Padreen / `padreen.exe` / `host/padreen` / 标签 `Padreen`；探测 App `com.paperdisplay.probe`（目录 `android/probe`）是研究用的，不属于产品，保留旧名。

## 1. 设备事实

| 项目 | 值 |
|---|---|
| 型号 / 系统 | HMW-W09，HarmonyOS 2.1.0（`2.1.0.222(C00E170R1P8)`），EMUI 12.0，**Android 10 / API 29**，安全补丁 2020-11-01 |
| SoC | Kirin 820E，arm64-v8a；USER 版本，Bootloader 已锁，`ro.secure=1`，shell 为 uid 2000，**无 root** |
| USB | `VID_12D1&PID_107E`，复合设备：MTP + 大容量存储 + **ADB（MI_02，Windows 自动装好驱动）**；USB 2.0 高速 |
| 屏幕 | 面板**扫描方向是横屏 1872×1404**，227.5 dpi，**VSYNC 40 Hz（25 ms）**；系统逻辑方向是竖屏 1404×1872，所有图层带 ROT_90；全部 CLIENT（GPU）合成；整屏 `FLAG_SECURE`（screencap 只能得到黑图） |
| 横屏 | `SCREEN_ORIENTATION_LANDSCAPE` 时**数据线在右侧**画面正立（用户肉眼确认）；反向横屏用 `REVERSE_LANDSCAPE` |
| 墨水屏栈 | 掌阅（Zhangyue）开发的框架：`/system/framework/eink.framework.jar`（`android.eink.*`）；SurfaceFlinger 补丁 `libeinksfpatch.so`（`EinkSFPatchData`）；抖动 `libeinkdither.so`；直写 `libfbhandsurface.so`；TCON 驱动 `libT1000.so` |
| TCON | **ITE T1000**（`iTE-RISCore-APUx`，固件 V.T1000.H040，`/odm/etc/firmware/display/t1000.bin`）。设备节点 `/dev/t1000cdev`、`/dev/spidev3.0` 仅 system:graphics 可访问，**shell 读不到**，不能直接轮询忙闲 |

## 2. 隐藏 API 访问矩阵（普通 App，无需绕过）

`getDeclaredMethods()` 会过滤被禁方法，所以"没出现"即被禁。

| 类 | 放行 | 被禁 |
|---|---|---|
| `android.eink.EPDCDevice` | 18 个：`forceRefresh`、`getEpdcDefaultMode/LastMode`、`setForceNextPostMode`、`setEpdcDefaultMode`、`setDelayRefreshInterval`、`setNeedCompareFrame`、`setRefreshStrategy`、`setIngnoreAllReresh`、`nativePostCommand`、`updatePenStatus`、`isMarkPenRefreshComplete` … | `getT1000Info`、`getT1000LogStatus`、`setNextLayerMode`、`setForceSkipPostMode` |
| `SurfaceAPI`、`SurfaceControlAPI` | 无 | **整类被禁**（`addEpdc`、`forceUpdateMode`、`bindFBSurface` …） |
| `ViewAPI` | `invalidate(view, rect, mode)`、`setLayerForceUpdateMode` 等 11 个 | `addEpdc` |
| `HWSurfaceView` / `FBHandSurface` / `GraphicBuffer` / `FBPaint` | 全部公开方法（11 / 61 / 13 / 19） | `FBPaint.setDitherMode` 等少数 |

**本方案从不调用会改全局状态的方法**：`setEpdcDefaultMode`、`setDelayRefreshInterval`、`setNeedCompareFrame`、`setRefreshStrategy`、`setIngnoreAllReresh`、`setT1000LogStatus`、`nativePostCommand`、`updatePenStatus`。唯一用到的"全局"调用是 `setForceNextPostMode(mode)`，它只影响紧随其后的那一次 post。

## 3. 刷新模式常量（`android.eink.api.EinkMode`）

模式是位域：`波形号(0..15) | FULL(32) | WAIT(64) | COMBINE(128) | DITHER(256) | INVERT(512) | CONVERT(1024) | REAGLD(4096) | 1BPP(8192) | QUICK(65536) | FAST(131072) | Y1_ENABLE(262144) | Y1_DISABLE(524288) | HW_WHITE(1048576) | HW_BLACK(2097152) | IGNORE_REFRESH(4194304) | RECT_REDUCE_ENABLE(8388608)`。
波形号：INIT 0、**DU 1**、**GC16 2**、**GL16 3**、GLR16/ANIM_4BIT 4、GLD16/DU4_4BIT 5、**A2(ANIM) 6**、**DU4 7**、AUTO 15。

常用预设：`1` DU、`2` GC16 局部、`3` GL16、`5` DU4(4bit)、`7` DU4(5bit)、`6` A2、`8198` A2 1bpp、`8193` DU 1bpp、`260/262` A2+抖动、`97/98/99` DU/GC16/GL16 全屏。

**模式 → T1000 mainmode**（从 SF 日志 `t1000_i2c_set_mipi_mode ... to mainmode N` 读出）：

| mainmode | 波形 | 备注 |
|---|---|---|
| **4** | DU(1) | 手写笔默认；实测稳定 |
| **3** | GL16(3)、DU4(5/7)、GC16(2/98/99) | 同一 mainmode 内换波形**不需要 I2C 模式切换** |
| 1 | A2(6)、8198、262（日志里叫 `T1000_MIX_MODE`） | **不稳定，不要用**（见 §5） |
| 0 | 260 | — |

## 4. 两条输出路径的实测对比

### 4.1 普通 Surface（经 SurfaceFlinger 合成）
从 App 提交到 SF 调用 `postRefreshMode`（此时波形还没开始）：

| 模式 | 值 | 耗时 | 抖动阶段 |
|---|---|---|---|
| DU4 4bit | 5 | **35 ms** | 无 |
| DU4 | 7 | 56 ms | 无 |
| GC16 全屏 | 98 | 56 ms | 19 ms |
| A2+抖动4bit | 260 | 68 ms | 15 ms |
| A2 1bpp | 8198 | 95 ms | 57 ms |
| A2 | 6 | 88–156 ms | 56–112 ms |
| DU | 1 | 90–137 ms | 55–108 ms |
| GL16 / GC16局部 | 3 / 2 | 100 ms | 64–67 ms |
| DU 1bpp | 8193 | 162 ms | 112 ms |

SF 在 App `queueBuffer` 后 0–7 ms 内就开始处理（不等 VSYNC）；耗时主要是整帧抖动 `huawei_eink_rgba8888_dither`。

### 4.2 FB 直写通道（`HWSurfaceView` + `FBHandSurface`）—— **已弃用（见 §10：T1000 不稳）**
- **SF 侧只有约 0–5 ms**，无抖动、无合成。`postFrameBuffer()` 同步返回，**3–8 ms**；只有 T1000 需要唤醒时才会阻塞（见 §5）。
- `postFrameBuffer(l,t,r,b,mode)` 的 `mode` 参数**被忽略**，固定走 DU(1)；`setAhwPenPostMode(n)` 也无效。要换波形，在 post 前调 `EPDCDevice.setForceNextPostMode(m)`（已验证 5/7/3/1/98 都被采用）。
- 连发不产生背压：16 次 A2 提交（30 ms 间隔）每次 5–8 ms 返回，T1000 自己排队/合并。
- `isMarkPenRefreshComplete()` 恒为 true，`waitSync()` 立即返回，**不能用来测面板完成时间**。

### 4.3 FB 是什么、怎么画（**关键，踩坑多**）
- FB 是叠在画面上的 **1872×1404 ARGB 覆盖层，初始全 0（完全透明）**。
- 在 `HWSurfaceView` 的 active buffer 里用 Canvas 画的内容，**只在整屏合成时才上屏**，对局部 `postFrameBuffer` 无效。局部更新必须用 `FBHandSurface` 自己的绘制接口把**不透明**像素画进 FB，再 `postFrameBuffer`。
- 可用的绘制接口（FB 读回验证，`capture(GraphicBuffer)` 是放行的）：
  - `drawColor`：填纯色（需 `clipRect` 限制范围）。
  - **`drawGraphic(GraphicBuffer, Rect src, RectF dst, FBPaint)`**：逐像素 1:1 拷贝，**256 个灰阶完全保留**（无抖动/量化）。GraphicBuffer 必须是 `GraphicBuffer.create(w, h, 1 /*RGBA_8888*/, 0x33)`；多加 `HW_TEXTURE(0x100)` 变成**静默空操作**。`(x,y)` 位置版本也可用。
  - `drawPath`、`drawPoints` 也能写入。
- `drawGraphic` 孤立测速：100²→0.94 ms，400²→1.4 ms，800²→3.3 ms，1404²→6.4 ms。
- `autoHandWrite` 默认 true（系统自动画笔迹）；用户没碰屏幕时无影响。

## 5. T1000 电源/刷新陷阱（**设计必须绕开**）——早期观察，§10 有更准确的因果与最终结论

| 现象 | 触发 | 代价 |
|---|---|---|
| **休眠后唤醒**（`isn't in active mode, wakeup first`） | DU(mainmode 4) 空闲约 **17–20 s**；MIX 约 10 s；全屏 GL16/GC16 之后也会 | 成功时 **~105 ms** |
| **唤醒超时 + 硬复位**（`wait readypin timeout, reset t1000`） | 观察到：全屏刷新后切到 A2；mode 4→1 切换；一次 DU→DU | **1.3–3.2 s** 阻塞，之后恢复 |
| MIX 补偿卡顿 | A2 最后一次变化后约 2 s，T1000 自己做补偿刷新 | 此时 `setdisplaymode` 阻塞 **1.3–1.6 s** |
| 系统自动补刷 | ≥7–8 次 A2 之后，最后一次提交约 3.7 s，SF 自行发起一次 DU 刷新（`handleRefresh`，mainmode 1→4） | 之后 T1000 睡眠，下次提交要唤醒 |
| A2 行测试 | 在 DU4(mainmode 3) 行之后切到 A2 | **2 次复位，整行阻塞 7.7 s** |

稳定组合（实测）：
- **只用 DU（mainmode 4）**：空闲 0.3–16 s 每次 3–5.5 ms，无自动补刷；≥20 s 空闲后首次 +107 ms。
- **只用 DU4(5) / 同 mainmode 的 GL16**：空闲 0.3–20 s 每次 5–7.5 ms，**20 s 内没观察到休眠**。
- 相同像素的"心跳"提交**不能**阻止休眠（T1000 按"有无真实像素变化"计时）。

**结论**：不使用 A2/MIX；整个会话固定在一个 mainmode（优先 mainmode 3：DU4 快刷 + GL16 补灰阶 + 98 全刷）；需要补充长时间空闲（30 s–5 min）下 mainmode 3 是否休眠及唤醒是否稳定的测试，并设计"保持唤醒"策略（例如周期性真实像素变化）。

## 6. 肉眼确认（用户反馈）

1. FB 直写 + 整屏 98：竖屏、横屏（数据线在右）显示**正立的 R，画面干净**。
2. 用错误画法（Canvas 画在 active buffer）做的小方块：**完全看不到**（只有屏幕刷新）。
3. 用 `drawGraphic` + `postFrameBuffer` 画 4 行 × 10 个 150×150 方块，模式依次 DU4(5)、DU4-5bit(7)、GL16(3)、DU(1)：**每行 10 个方块都"几乎同时出现，约半秒内一次扫完"，黑得彻底，无整屏闪烁** → 面板能并行处理多个区域更新。
4. 面板的**物理刷新时长仍未测量**（需要相机，见 STATE.md）。

## 7. 端到端原型实测（Python 发送端 + 平板 App，USB `adb forward`）

PC 抓取镜像 1872×1404 区域；G 通道当灰度；36 px 瓦片差分；zlib level 1。

| 环节 | 中位数 |
|---|---|
| 时钟同步 RTT（adb forward） | 0.9–3 ms |
| PC 抓取+灰度（mss，Python） | 42 ms（16–19 帧/s）；这是原型限制，C++/DXGI 预计个位数毫秒 |
| 电脑发送 → 平板收到包头 | 2.3–4.6 ms |
| 解压 | 0.4–1.6 ms |
| 写入 FB（暂存位图 + GB + `drawGraphic`） | 12–22 ms（p95 ~28 ms，大矩形）→ 待优化 |
| `postFrameBuffer` | 3.3–3.8 ms |
| 抓取完成 → 平板 post 完成 | **26–48 ms** |

## 8. 已解决：正式 App 的 FB 读回不一致（2026-11-02）

**根因**：`SurfaceView` 的默认像素格式是 **RGB_565**，`HWSurfaceView` 按它把 FB 覆盖层建成 16 位；正式 App 没设格式，探测 App 设了 `RGBA_8888`。按 32 位读写 16 位 FB 就会得到"缩小一半、横向重复两份（偏移 960 = 1920×2 字节/4）、下半屏空白、颜色怪异"。
**验证**：同一 APK 用 `am start --es fmt 565` 做 A/B：565 时 `selftest` 三种尺寸全 WRONG；默认 RGBA_8888 时全 EXACT，`mirror.py --verify` 整屏与子矩形的 CRC32 完全一致。
**修复**：`holder.setFormat(PixelFormat.RGBA_8888)`（`MainActivity`）。——注意：FB 直写通道后来被放弃了（见 §10），但常规通道同样设 RGBA_8888。
## 9. 死胡同清单（不要再试）

`SurfaceAPI.*` / `SurfaceControlAPI.*` 被禁；`postFrameBuffer` 的 mode 参数无效；Canvas 画在 active buffer 不能用于局部更新；`isMarkPenRefreshComplete/waitSync` 无信息量；相同像素心跳不能防休眠；screencap 被 `FLAG_SECURE` 挡；T1000 sysfs/设备节点读不到；`drawGraphic` 用 `0x133` 用途标志是空操作；A2/MIX 模式会卡顿和复位（FB 通道；常规通道下 A2 的稳定性未系统测试）；**FB 直写通道整体（§10）**；IFBHandSurface.invalidate 当唤醒手段；用 getPowerStatus() 判断是否休眠（恒为 3）。

## 10. T1000 稳定性调查：FB 直写通道不可用，改用常规通道（2026-11-02，**决定性**）

### 10.1 现象
用户用 FB 直写通道的镜像拖动窗口："屏幕连续很多次黑白闪烁，闪烁过程中不显示任何图像"。logcat 里对应 `wait readypin timeout,reset t1000`（T1000 被系统硬复位，面板在复位/重启期间闪烁、不更新）。

### 10.2 方法
`host/poc/stress.py` 往平板发合成流量（棋盘格翻转 / 渐变 / 模拟拖窗 / 整屏），平板 App 在每个场景前后用 `MARK` 打时间戳；**必须在测试前开始流式抓 logcat 并按标签过滤**（`adb logcat -v threadtime PaperDisplay:V EinkSfpatchData_T1000:V T1000EPDCDevice:V hwtconepdcdisplay:V t1000_lib:V FBHandSurfaceClient:V DISPLAY_DITHER_V201_ALGO:V MD_REFRESH:V FB_GRALLOC:V '*:S' > .tmp\lc-eink.txt`）——事后 `logcat -d` 会因缓冲区被刷屏日志"expire"而丢掉大部分提交记录，并且**未过滤的 logcat 含其他 App 的隐私内容，不要落盘/打印**。脚本按"字节偏移"切片，统计 `reset t1000` / `ioctl err 11`（I²C 卡死）/ `sleep or poweroff`（被丢弃）/ `set_mipi_mode success`（T1000 实际刷新次数）。

### 10.3 FB 直写通道的三种故障模式
1. **T1000 休眠 + 不会被唤醒**：DU4(mainmode 3) 最后一次更新后约 **2 s** 进入休眠（`getPowerStatus()` 一直返回 3，**看不出来**）。FB 线程 `FBPostHwtcon::threadLoop` 每次最多轮询 `t1000_sysfs_get_displayready` 301 次×10 ms≈3.1 s，超时后若 `epd_device::get_powerstatus()` 是 1 或 8 就 `return directly`（**丢弃这次更新**），否则 `reset_t1000`。0.25–16 s 的任何稀疏提交都会命中。
2. **I²C 卡死**：提交落在休眠过渡期时 `t1000_i2c_set_mipi_mode` 阻塞 **2.25 s**（`(i2c_read)ioctl err 11`），之后 3.1 s 的就绪等待超时 → 硬复位 → 复位后 `Remote I/O error`（err 121）持续约 3.2 s，再复位一次才恢复。
3. **高负载**：整屏 DU4 以 10 次/秒提交 → 同样的 I²C 卡死 + 复位（900×720@30 Hz、模拟拖窗@30 Hz 都没触发；整屏 ≥ 10 Hz 触发）。
- 保活心跳（0.25/0.5/1/1.5 s，相同像素/1 级灰度差/整块翻转）**都不能**让 FB 通道稳定。
- 能唤醒 T1000 的只有走 `T1000EPDCDevice::update`（"wakeup first"）的**常规刷新**：`EPDCDevice.forceRefresh(模式)` 或改变普通 View 内容；`IFBHandSurface.invalidate(...)` 会让 SF 主线程自己卡 3.1 s 然后复位（**不要用**）。

### 10.4 常规通道（采用）
普通 `SurfaceView`（RGBA_8888）+ `lockCanvas(脏矩形)` + `setForceNextPostMode(波形)` + `unlockCanvasAndPost`：
- **间隔 0.5 / 1 / 2 / 4 / 8 / 16 / 30 / 90 s 的稀疏提交全部正常**（每次先唤醒 ~25 ms）。
- 负载：360²/900×720 在 5–30 Hz、模拟拖窗 30 Hz、整屏 3/10 Hz、DU4/DU4-5bit/DU/GL16/GC16 逐一测试，**零复位、零 I²C 卡死、零丢弃**；DU↔GL16↔DU4 来回切换（间隔 0.5–3 s）、DU 持续 30 s×10 Hz、用户 6 分钟真实拖窗镜像（4570 次刷新请求，656 次 T1000 更新）日志全干净。
- 没有渲染线程时 App 以 30 Hz 灌帧会在 SF 排队（确认延迟 1–7 s）；加了渲染线程（合并 + ≥60 ms 间隔）后 30 Hz 灌帧延迟稳定在 60–100 ms。

### 10.5 两类刷新路径与节奏（日志实测）
| 波形 | 路径 | T1000 更新间隔 | SF 每帧成本 |
|---|---|---|---|
| DU(1)、DU 1bpp(8193)、A2(6)、A2 1bpp(8198) | **直通**：`postRefreshMode` 里直接 `setdisplaymode` | ≈ 100 ms | ~60 ms 的整帧 Sierra 抖动（TVM 模型）+ 直接更新 |
| DU4(5)、DU4-5bit(7)、GL16(3)、GC16(2/98) | **节流**：`add_updatelist` → `FBPostHwtcon` 线程 | **550 ms**（`mDelayInterval=600 ms`，构造函数里写死 0x258） | DU4 无抖动 ~15 ms；GL16 "Mode is not picture type, No Dither" |
| 带 `FAST(0x20000)` 或 `QUICK(0x10000)` 标志 | 强制刷新 | ~700 ms | — |
- `EPDCDevice.setDelayRefreshInterval(ms)` = `nativePostCommand("delay-refresh-interval <ms>")`，最终写 `FBPostHwtcon.mDelayInterval`（`EinkSFPatchData::setHandwriteTimeInterval`）。**全局、进程内**参数，我们不碰（DU 直通路径用不着）。
- 逆向参考：`docs/probe/files/libeinksfpatch.so`（`llvm-objdump` 在 NDK 里；脚本思路见 STATE §6）。

### 10.6 结论
- 平板输出：**常规通道 + 渲染线程**；极速模式 DU(1) 1-bit，补灰阶 GL16，全刷 GC16。用户肉眼确认拖窗无闪屏、极速模式快很多。
- 以后任何新刷新方式（A2、DU4 直通、多矩形…）都先过 `stress.py` + 过滤 logcat 的零复位检验。

## 11. 端到端延迟实测（手机录像，2026-11-02）

**方法**：电脑显示器上一个参考窗口 + 平板（镜像虚拟屏）同时显示同一个每 1.2 s 黑白翻转的大方块（`host/poc/latency_pattern.py`，电脑端 `mirror.py --vdd --fast bw`），手机 30 fps 竖屏手持拍摄；`host/poc/analyze_auto.py`（每帧自动定位两个窗口 + 方块中心 ROI）/`analyze_video.py` 取参考方块翻转 → 平板方块开始变化的时间。分辨率 = 1 帧 = 33 ms。

**结果**（极速 1-bit DU 模式，1.2 s 翻转一次，T1000 保持唤醒；有效样本 n=9）：
| 指标 | 最小 | 中位数 | 最大 |
|---|---|---|---|
| 开始变化 | 421 ms | **450 ms** | 459 ms |
| 变化一半 | 451 ms | 472 ms | 495 ms |
| 完成 90% | 500 ms | **527 ms** | 547 ms |
- 面板自己的 DU 变化过程很快（开始 → 90% 只有 ≈ 75 ms）。
- **时间线分解**（`host/poc/trace_latency.py`：翻转时刻 / `mirror.py --trace-csv` / App `--ez trace true` 的 `POSTED` 日志 + 过滤 logcat 对齐到电脑时钟）：抓取完成(0) → 发出 +13 ms → 平板解码完 +23 → 渲染线程画完 +36 → 提交给系统 +39 → SF `handleRefresh` +61 → 整帧 Sierra 抖动完成 +126 → `setdisplaymode`(DU 直通) +135 ms；再加上电脑端从内容变化到抓取完成约 50 ms（Python/mss 抓取 ~27 ms + 轮询）。**软件链路合计 ≈ 185 ms；其余约 400 ms 是 T1000 + 面板在 `setdisplaymode` 之后到"开始变化"之间的固有延迟**，目前没有可见的软件杠杆。
- SF 对 DU 每次都对**整帧**（1872×1404）做 ~60 ms 的抖动，哪怕只改 36×36；DU4 不抖动，但走 550 ms 节流路径，单次孤立更新的 SF 耗时与 DU 相当（~90 ms）。
- 孤立更新遇到 T1000 休眠（≥2 s 无更新）时，SF 侧偶发额外 +150–500 ms（唤醒），表现为 `handleRefresh` 来得更晚。

**这次录像的另一个发现（重要，已处理）**：录像中间约 2.5 分钟，电脑端的参考窗口翻转间隔变成约 9.6 s，平板比参考再滞后约 3.8 s；只有用户操作电脑的开头和结尾几秒恢复正常（1.2 s / 0.5 s）。同样的进程在我自己的测试中（含 200 s 无人值守）完全正常 → 判断为电脑端进程被 Windows 在后台/无输入时限频（EcoQoS/定时器合并）。**对策**：所有 PaperDisplay 电脑端进程启动即关闭自身的电源节流、请求 1 ms 定时器、提高进程优先级（`host/poc/winperf.py`；C++ 主机从一开始就要做，另加 MMCSS 与 `PowerSetRequest`）。需要用第二次拍摄验证。

**待做的对比实验**（需要手机，稳定机位最好）：DU(1) / DU4(5) / A2(6) / DU 1bpp(8193) / GL16(3) 的孤立更新延迟；大方块 vs 小方块（区域面积对 T1000 延迟的影响）。

## 12. 画面内容与刷新策略（C++ PaperHost 实测，2026-11-02）

**用户反馈链**：Python 版"极速模式比 4 级灰度快很多、文字清楚" → 换 C++ 主机（更快的指针更新、更激进的补灰阶）后出现"重影严重、指针时有时无、经常整屏黑白闪、卡" → 逐项查日志修正，最后换纯白壁纸 → "不卡了、闪屏/重影改善、跟手感好多了"。

**根因与对策（按贡献大小）**
1. **虚拟屏的壁纸是一张很暗、细节很多的照片**（Photos 应用背景「死神.png」）。1-bit 抖动后是满屏点阵：每次拖窗口都要把露出的壁纸区域重新刷一遍，停手后又要大片补灰阶。→ **PaperHost 连接时只给这块虚拟屏设纯白壁纸（`IDesktopWallpaper::SetWallpaper(虚拟屏ID, 白图)`），退出还原**；守护每 2 s 校验（曾被"某个操作"改回去，实测 1 分钟内）；原值存 `wallpaper-original.txt`，崩溃后下次启动能恢复。
2. **补灰阶一口气发了 10 个 GL16 大块**（每块最多 1/4 屏）：GL16 被系统节流 ~0.55 s 一次，每次占用 T1000 ~0.8 s，期间快速更新排队 → "卡"，同时是"整屏闪"。→ 分阶段（停 1.2 s 补 1 块、4 s 补到 3 块、10 s 补完）、每块 ≤12% 屏、文字块优先、只补"1-bit 明显比灰度差"的瓦片（平坦深/浅色吸附后算精确，不补）。
3. **把"面积 >30% 的变化"当动画等平静**：拖大窗口就会超过，结果变成 0.8 s 刷一次 → "卡"。→ 阈值提高到 ≥80%（整屏重绘），去抖 120 ms 平静/500 ms 上限；平静后发**一次**整屏 GL16（3 s 内最多一次，期间的整屏变化走 DU）。指针更新永不被拖延。
4. **插上虚拟屏约 3 s 后 Windows 做壁纸淡入/缩放动画**（连续 4–5 帧 81%→100% 的脏矩形，每 100 ms 一帧）。→ 首帧等 ≥4.5 s 且平静 1 s。
5. **指针**：`MONOCHROME`/`MASKED_COLOR` 光标里的"异或/反色"像素（文本 I 形光标）要按"反转下面的灰度"处理，否则在深色上看不见；DXGI 还没给形状时用内置箭头；旧/新指针矩形分别更新，避免一次指针跳变生成大外接矩形。
6. 整屏黑白闪的 GC16 全刷只在：首帧、托盘"立即全屏刷新"、（可选）"自动清残影"（默认关）。

**数据点**
- C++ 主机：帧到发送 0.1–7 ms；活跃时 ~20 更新/s、~570 KB/s、平板确认往返 ~84 ms（尖峰 600–800 ms 出现在整屏 GL16 刚发完时）。
- 15 分钟使用日志：34 次"整屏变化后一次灰度刷新"（有些可能是 DXGI 误报整屏，待用真实像素差校验）、17 次补灰阶分块。
- T1000 零复位/零卡死（`readypin|reset t1000|ioctl err 11` 全无）。
- `raw wave` 8–11（A2 6/7/8/9 帧）走 `postRefreshMode browsera2 mode frame N` 直通路径，稳定，间隔 ~100 ms；**它们的"收到更新→开始变化"延迟是否比 DU 短尚未测**（见 STATE §5-1）。

## 13. 各波形模式的延迟对比（手机慢动作录像，2026-11-03，初步）

**做法**：`tools\latency-modes.ps1`（Python 发送端 + 不规则翻转的方块 + 每 25 s 切一种原始波形：DU(1)、A2 6/7/8/9 帧(8–11)、A2/MIX(6)、DU4(5)）；参考窗口在电脑显示器上、方块与平板同步翻转；`host/poc/analyze_modes.py` 按翻转时间序列对齐电脑日志与视频，算每次翻转的"平板方块开始变化 / 变一半 / 变 90%"。
**录像**：华为"慢动作"（`SL_MO_VID_*.mp4`）= 实际 119.6 fps、按 29.91 fps 存 → **实际时间 = 文件时间 / 4**；文件创建时间是**录制结束**的时刻；视频只覆盖测试的约 45–210 s（DU → A2 9 帧 + A2/MIX 开头），DU4 没拍到。手托着平板、手机有移动 → 每种模式只有 4–7 次有效翻转，**噪声大，只能得粗结论**。

| 模式 | 开始变化 | 变一半 | 变 90% | 有效次数 |
|---|---|---|---|---|
| DU(1) | 362 ms（p25 299 / p75 485） | 380 | 522 | 7 |
| A2 6 帧(8) | 316（298 / 403） | 327 | 339 | 7 |
| A2 7 帧(9) | 398 | 417 | 465 | 6 |
| A2 8 帧(10) | 409 | 422 | 459 | 7 |
| A2 9 帧(11) | 299（n=4） | 291 | 357 | 4 |
| A2/MIX(6) | 样本不足 | | | 2 |

（时间从"电脑里方块翻转"算起，含 Python 发送端的 ~185 ms 软件链路与参考窗口 ~60 ms 显示延迟；C++ PaperHost 的软件链路更短。）

**结论（粗）**
1. **所有模式的"开始变化"都在 ~300–410 ms，没有一种明显突破这个下限**；A2 6 帧可能比 DU 略早（~45 ms，在噪声之内）。
2. **A2 的"可见变化过程"明显更短**：开始→90% 只要 ~25–65 ms，DU 约 160 ms（所以 A2 看起来更"脆"）；代价通常是更浅的黑白对比和更多残影——需要用肉眼评价。
3. 之前"约 400 ms 全是 T1000 固有延迟"的说法偏高：扣掉软件链路（Python 发送端 ~185–230 ms）后，T1000 + 面板的"收到→开始变"大约 **100–200 ms**；C++ 主机把软件链路缩短后，端到端应已比第一次测的 450 ms 好。
4. 要更可靠的对比需要：平板和手机都固定不动、每种模式 ≥20 次有效翻转、含 DU4 与 A2/MIX。**更实用的做法是让用户在托盘里切换 A2 变体用肉眼比**（已实现，见 STATE）。

**分析工具要点（避免再踩）**：慢动作视频要加 `--time-scale 4`；ROI 按 5 s 分块用时间方差定位（手机/平板会动）；对齐用整段不规则翻转序列的方向一致性（只有正确偏移才能让全部翻转对上）；每次翻转的 e-ink 摆幅 <50 灰度级的丢弃（鬼影/手遮挡）。

## 14. Wi-Fi 无线实测（2026-11-03）

- 网络：电脑和平板在同一 Wi-Fi（192.168.18.x，路由器 5 GHz 802.11ac，电脑 AX201）；电脑到路由器 ping 1.5 ms，**电脑到平板 ping 平均 114 ms（68–310 ms）**——平板端 Wi-Fi 省电（PSM，约 1 个 beacon 间隔）造成。
- 应用层 `WifiLock(WIFI_MODE_FULL_LOW_LATENCY)` 已持有（`dumpsys wifi`：`Locks acquired: 1 full low latency`），但 `low_latency_active_time_ms: 0`——这台设备上没有真正启用，**不能依赖**。
- 对策：电脑端每 50 ms 一次 PING/PONG 保活（会话期间）→ **ping 4 ms（3–6），更新往返 ~30–40 ms**（USB 约 15–20 ms），ping 抖动 ≤ 16 ms。
- 带宽：mono1 加 LZ4 后拖动 900×700 文字窗口只有 ~80 KB/s（未压缩时 USB 上曾达 5–6 MB/s）。
- 重连：平板 App 被关闭 → 会话结束、壁纸还原；重新打开 App → 约 7 s 内自动重连。记住的 IP 失效 → 扫描 /24，约 4 s 找到并更新。
- 安全：未配对/密钥不符的连接只能拿到 HELLO + 挑战，随后被断开；记录带计数器 nonce，重放/篡改验不过。

## 15. 触控反馈实测（2026-11-03）

- 链路：平板 `dispatchTouchEvent` → `T_TOUCH` → 电脑 `InjectTouchInput`（TOUCH_FEEDBACK_NONE）。用户实机试用：**点按正常、拖动窗口正常、反馈有延迟但可接受**（延迟 = 墨水屏刷新 ~0.3–0.5 s）。平板日志无 T1000 故障。
- 自动化验证（`adb shell input` 模拟手指，测试窗口 DPI 感知）：3 次点按 → 按钮点击数 0→3；标题栏拖动 (+150,−100) → 窗口精确移动；拖动滚动条滑块 150 px → 列表滚动到第 144 行；注入错误 0。
- **踩坑**：① `POINTER_FLAG_NEW` → 87；② "抬起"帧位置与上一次更新不同 → 偶发 87，改为在最后位置抬起后 8 轮滑动 + 8 次点按零失败；③ **Python/Tk 测试窗口若不是 DPI 感知，`winfo_rootx` 返回被虚拟化的坐标（这台机器上虚拟屏缩放 150%：700×520 的窗口实际 1074×839）**，用它算触点会落在桌面上——测试程序要先 `SetProcessDpiAwarenessContext(-4)`；④ adb `input` 事件的 toolType 是 UNKNOWN、压力 1.0/0、接触面积 0，真实手指有面积和压力。
- 虚拟屏被 Windows 自动设成 150% 缩放（推荐值），对手指点按的目标大小有利；PaperHost 用物理像素映射，不受影响。

## 16. 整屏刷新的来源与"一键刷新"（2026-11-03；其中的音量键方案后来按用户要求取消，只留三指轻点）

用户反馈"还是会经常全屏刷新"。逐一核对后，自动触发的整屏/大面积刷新来源有三个：
1. **电脑端"整页重绘 → 整屏 GL16"**：DXGI 报 ≥80% 脏面积（翻页、最大化、切换窗口）且真实像素变化 ≥50% 瓦片，平静 120 ms 后发一次整屏灰阶 GL16（≥3 s 才允许一次）——它在使用记录里还带来过一次 2 s 的 rtt 尖峰（T1000 被 GL16 占住）。**这是最常见的"经常整屏刷新"。**
2. **DXGI 丢失后的重新同步**也是整屏 GL16（锁屏/UAC/分辨率变化后）。
3. **平板渲染器取外接矩形**：旧 `Renderer` 把所有待刷块（含 GL16 settle 盒和屏幕另一侧的指针更新）合成一个外接矩形，用最高优先级的模式发出 → 偶发巨大面积的 GL16。
另有逐盒的"自动补灰阶"（局部 GL16，每盒 ≤12% 屏，整页变化后会连续补十几盒，每盒占 T1000 ~0.8 s）——不是整屏，但观感上仍是"屏幕在刷新"。

**处理**：① ② 默认关闭（`auto_full=0`；整页重绘走普通 A2 快速更新，重新同步只补有差别的区域）；③ 渲染器按"快速/灰阶/整屏"三类分开合并；自动补灰阶默认**关闭**（托盘「自动补灰阶」：关/少量/完整；`settle=off|light|full`）；手动一键整屏刷新 = 音量键 / 三指轻点（平板），托盘「立即全屏刷新」（电脑）→ GC16 整屏灰度。
**验证**：整屏页面连翻 6 次（默认设置）→ 平板 SF 日志只有 A2 模式 11 次，**没有 GL16、没有整屏 GC16**；少量档 → 约 3 次小块 GL16；音量键（`adb shell input keyevent`）→ 平板记录"full refresh requested (volume key)"、电脑记录整屏刷新、SF 出现一次 `0x43`（GC16 全刷）；新渲染器压力测试（快速/灰阶/整屏 GC16 1 Hz/大面积 DU）**零复位、零 I²C 卡死**。注意 `stress.py` 的整屏场景要显式给 `x=0,y=0`，默认的 (972,252) 对 1872×1404 的矩形是越界（平板拒绝，不回 ACK）。

## 17. 产品化：局域网发现与配对、图标、安装包（2026-11-03）

- **平板没有摄像头**（`pm list features` 无 `android.hardware.camera*`，无 camera 服务）→ "扫码配对"在平板上做不了；改为 平板自动发现（UDP 广播）+ 9 位配对码 + USB 免码配对；反向（电脑摄像头扫平板屏幕上的二维码）技术上可行但需要电脑有摄像头 + 二维码解码库，未做。
- **架构变化**：要"平板端自动扫描电脑"，电脑就必须被连（入站）→ 电脑监听 TCP 38702 + UDP 38703，需要防火墙放行。做法：安装包以管理员加两条规则（`program=` 限本程序、`remoteip=localsubnet` 限本地子网、`profile=any`——本机 Wi-Fi 被 Windows 归为"公用网络"）；绿色版首次运行弹窗说明 → `ShellExecute runas cmd /c netsh …` 一次 UAC（实测通过，规则生效）。v1 的"电脑扫描平板 /24 + 平板监听 38701"已删除。
- **配对协议 PD2**（protocol/PROTOCOL.md）：ECDH P-256（CNG ↔ javax.crypto 互通已验证，注意 CNG 的 `KDF_RAW_SECRET` 小端要反转）+ PBKDF2-HMAC-SHA256(10 万次，与 Python hashlib 逐字节一致) 把配对码绑定到 ECDH 成绩单；每个配对窗口 ≤5 次错误、5 分钟；配对得到 32 字节 K，之后的连接是双向 HMAC 认证 + 会话密钥 + AES-128-GCM（与 v1.4 记录格式相同）。**服务器健壮性实测**（host/poc/pd2_probe.py）：未配对平板 → FAIL(1)；窗口未开 → FAIL(4)；5 次错码各 FAIL(5)，第 6 次起 FAIL(4)；静默连接 6 s 后被断开。
- **实机流程**：USB 连一次 → 自动配对（电脑 devices.ini 与平板 PcStore 各存一份 K）；清空平板数据后用配对码配对（用户亲手输入）→ 配对 + 认证 + 会话 ≈ 1 s；LAN 会话 ping 4–6 ms。
- **图标**：用户给的图（深色圆角方块 + 显示器 + 小平板，RGBA、角透明）→ `tools/make-icons.py` 生成 app.ico（16–256）、4 个状态点托盘图标、Android 传统 + 自适应图标（前景 = 提取的图案，背景 = 图的深色）。`rc.exe` 在 NuGet SDK 里就有（`third_party\winsdk\...\bin\10.0.26100.0\x64`），资源 = 图标 + 清单（Per-Monitor DPI v2、通用控件 6、asInvoker）+ 版本信息；Android 用 `aapt2 compile --dir res` + `link <res.zip>`（不是 `-R`，那是 overlay）。
- **安装包**：Inno Setup 7.1.1（官方 GitHub 发布，SHA-256 与 Authenticode 签名已核对；`/CURRENTUSER /DIR=D:\Tools\InnoSetup` 装在 D 盘，不需要管理员）；脚本 installer/PaperDisplay.iss；`ChineseSimplified.isl` 随 Inno 自带。踩坑：`RegQueryMultiStringValue` 返回的是 String 不是数组。**未签名**（SmartScreen 会提示）。
- 测试时踩坑：**用户在平板上同时手动操作时，我的 adb 模拟点击会和用户输入混在一起**（一度误判为"错码 4 次"）；测试窗口截图用 `PrintWindow`（不受 z-order 影响）；从隐藏窗口启动的进程第一次 `ShowWindow` 会被吞掉。

## 18. 合盖不休眠、切出去再切回来（2026-11-03 夜）

- **合盖动作不能由应用改**：`LIDACTION` 是电源计划设置（本机 Surface Pro 8 把它隐藏了，控制面板里没有；插电 0、电池 1），**写入需要管理员**（`HKLM\...\Power\User\PowerSchemes` 对 Users 只读）。`SetThreadExecutionState` / `PowerCreateRequest` 只能阻止**空闲**睡眠，挡不住合盖睡眠。→ 唯一不弹 UAC 又能"仅连接期间"生效的做法 = 常驻的、权限够的助手：Windows 服务（`paperhost.exe --power-service`，LocalSystem），用命名事件做"死人开关"心跳（见 STATE.md）。安装目录因此必须锁成只有管理员可写。
- **合盖/开盖时的真实表现（用户实测，日志证据）**：Windows 重排显示器，`desktop duplication lost` → 重新初始化 → **虚拟屏的 GDI 名字会变**（`\\.\DISPLAY1` ⇄ `\\.\DISPLAY5`，随内置屏/外接屏是否在桌面上而变）→ 抓取、触控映射、壁纸都必须每次取"当前名字"（`findVirtualDisplay`），否则重初始化会一直失败。恢复用时 1–9 s，会话不断，窗口不被搬走。本机的 Type Cover 合盖不会触发 `GUID_LIDSWITCH_STATE_CHANGE`（没收到 `lid:` 事件），所以不依赖它。
- **平板成为电脑唯一屏幕**（合盖 + 没接别的显示器）：`attachedRealDisplays()==0` 时申请 `PowerRequestDisplayRequired`，否则"关闭显示器"超时后它会黑屏/锁屏。实测这种状态下一切正常，但**此时重启 PaperHost 电脑会短暂没有任何显示器**。
- **平板端切出去**：旧结构里 `surfaceDestroyed` 会停掉 Receiver 并关 socket，所以 Home/Back/换 App 都会断开。新结构：`DisplayHub`（进程级单例）= 渲染线程 + `Receiver`（USB 监听 / LAN 会话）+ 影子缓冲区；`Renderer.attach/detach(Fb)`，没有表面时只更新影子不发帧，attach 时整屏 GC16 重绘（`hasContent` 才重绘，避免盖掉"等待电脑连接"提示）。前台服务 `SessionService` 保活，通知渠道 `session`（IMPORTANCE_LOW）。**坑**：`HomeActivity` 不能是 `singleTask`（从桌面点图标会把上面的显示 Activity 清掉）；USB 流程里任务根是 `MainActivity`，Back 直接回桌面，点图标后主页压在上面，主页横幅给"返回显示/断开"。实测：Home → 会话不断（电脑端无变化）→ 回来 `surfaceChanged` → 重绘；Back → 桌面，通知常驻；点图标 → 主页横幅 → 返回显示 ✓。
- **连接中断后的重连**：平板端 `DisplayHub.startReconnect()`（`Discovery.findOnce` 找电脑当前地址 → `PcClient.connect` → 原影子缓冲区继续）；电脑端 `runSession` 在 Wi-Fi 会话 `tablet link closed / send failed` 后**保留虚拟屏、`lan.setExpected(padId)` 只放行同一块平板（发现应答对它也不报 busy）**，最多 45 s，`link.adopt` 新连接后用 `quickFirst` 的新 `Session` 继续；`T_LEAVE` 表示主动断开 → 不等。实测：杀掉平板 App → 电脑保留虚拟屏 → 重新打开 App 自动连回 → "the tablet is back"；手动在宽限期内点「连接」同样可以。**坑**：平板点「断开」时电脑先发现的是写失败（`send failed`）而不是 `T_LEAVE`，所以宽限判断要同时看 `link.tabletLeft()`。
- **退出幽灵进程**：根因不在我们的线程——`wWinMain` 返回后 CRT/DLL 关闭阶段卡住（残留线程起点分别在 ntdll 线程池、`mswsock.dll`、`combase.dll`），进程只剩 2 个等待线程，持有 mutex 和 exe 文件几分钟到永远，用户态杀不掉。修法：清理完后 `TerminateProcess(GetCurrentProcess(), 0)`；退出从 50+ s 变成 0.6 s。
- 其它踩坑：PowerShell 命令模式下 `rep 'a' 'b' + "`n" + 'c'` 会把 `+` 当成独立参数（只用了前两个）——拼接要加括号；`icacls /T` 与 `/inheritance:r` 的组合见上；从隐藏窗口启动的进程第一次 `ShowWindow` 被吞；窗口要按光标所在显示器的 DPI 创建（系统 DPI 与实际显示器不一致时控件比例错乱）。

## 19. 三指轻点的右键菜单、三指上/下滑手势、"经常自动整屏刷新"的审计（2026-11-05）

**右键菜单的根因**：旧逻辑在"第三根指落下"时才发现是三指手势，而前两根指的 `T_TOUCH` 已经转发给电脑（电脑端 `InjectTouchInput` 看到两根指同时按下又很快抬起/取消 = Windows 的**双指轻点 = 右键**）。电脑端取消时用的是 `POINTER_FLAG_UP|POINTER_FLAG_CANCELED`，但这挡不住手势识别。**所以只能在平板上不把前几根指发出去**：`TouchGate`（平板，`android/app/src/.../TouchGate.java`）——一次触摸的第一根手指（指尖，手写笔不算）按下后先压住 ≤60 ms，第二根指落下后延长到第一根指后 140 ms；期间到了三根指（一只手三根指尖：两两间距 90–1100 px、一次触摸里恰好三根）就把压住的事件**整个扔掉**，电脑端什么都收不到；没等到第三根指则按原顺序放出，之后的事件直接转发。代价：开启手势时单指按下晚 ≤60 ms（单指轻点在抬起时立刻一起放出，不再多等）；平板主页可以关掉手势（零延迟）。16+2 个 JVM 单元测试（`android/app/test/run-tests.ps1`，用 Handler/MotionEvent 桩带虚拟时钟）覆盖：单击/拖动/双指轻点/手写笔/三指轻点/迟到的第三指/上滑/下滑/横滑/长按/四指/拇指先落下/手掌碎片/双手/取消/开关。

**三指上/下滑**：压住期间三指到齐后进入手势态：三指平均垂直位移 ≥200 px 且 ≥1.6×水平位移就触发一次（边滑边触发，不等抬起）→ `T_GESTURE 0x53`（kind 1 上 / 2 下）→ 电脑 `SendInput`：上 = Win+Tab，下 = Ctrl+Alt+Tab（切换器停在屏幕上可点选；`gesture_down=alttab` = 普通 Alt+Tab）。键盘注入对以管理员运行的窗口无效（UIPI）。

**"浏览网页时经常自动整屏刷新"——证据**：① 电脑端 `paperhost.log` 里所有 `session: full refresh (manual / scheduled)` 与平板 logcat 的 `full refresh requested (three-finger tap)` 一一对应（用户使用中 5 分钟 6 次），电脑端自己从未发起过整屏刷新（没有 `auto clean` / `full-panel change settled`、没有重复的 `first frame`）；② 同时段 SurfaceFlinger/T1000 日志 8 分钟 2160 次刷新**全部**是 A2（`waveform 8, update_mode 0`），没有 GC16/GL16、没有 `reset t1000`、没有读就绪超时。→ **"自动整屏刷新"几乎全是多指操作（缩放/双指滚动/手托平板时拇指碰屏）被旧的三指轻点识别误触发**：旧规则只要一次触摸里出现过 3 个触点、<1.2 s、移动 <120 px 就算，不管三根指是不是一起落下。新规则见上（一次触摸的前三根指 140 ms 内落下 + 一只手的指尖间距 + 轻点 ≤600 ms/≤70 px），并在平板日志里记录每次识别的细节（`three-finger tap: landed within N ms, lasted M ms, moved at most K px`）。

**所有会整屏（或近整屏）刷新的来源（现状）**：
| 来源 | 触发 | 是否自动 |
|---|---|---|
| 三指轻点 / 托盘「立即全屏刷新」/ 主界面按钮 / 切换极速↔画质 | 用户 | 主动 |
| 会话第一帧（GC16） | 每次新连接 | 必要 |
| 平板从后台回到前台（`Renderer.attach`，GC16） | 别的 App 画过屏幕，必须重绘；**同一表面重复的 `surfaceChanged` 不再重绘** | 必要 |
| Wi-Fi 重连 | 平板还显示着原画面（HELLO caps bit5 RESUME）→ 只补这期间变化的部分，不闪；平板被重启过 → 重新发一整屏 GC16 | 仅必要时 |
| `auto_full` / `auto_clean` / `full_refresh_minutes` / 自动补灰阶（GL16） | 默认全关 | 可选 |
| 大幅页面变化（翻页/切窗口/整页滚动） | 快速黑白（A2）一次画出，不是整屏刷新；画面停稳 120 ms（连续变化最多 500 ms）后发 | 自动（A2） |
| T1000 硬复位（`reset t1000`） | 过载时的硬件行为；本次日志没有出现 | — |
新增统计便于以后直接定位：平板每 5 s 一行 `render: N posts …, avg X% of the panel, B >=50%, F >=90%, S gray/full`，每个灰阶/整屏波形的 post 单独记一行 `slow post: …`；电脑每 5 s 的统计行带 `big(>=50%) / full(>=90%)`。**坑**：`adb logcat -c` 会清掉缓冲区里的历史证据（我在取证前清过一次）；SurfaceFlinger 的 T1000 日志每次刷新都打印整帧 1872×1404 的抖动（不是脏矩形），每次约 70 ms，看不出刷新面积。

**部署时踩的坑（11-05）**：① 用户的平板是电脑**唯一的屏幕**时，**UAC 弹窗在安全桌面，Desktop Duplication 抓不到（`DuplicateOutput failed 0x80070005`），平板上看不到**，用户第一次根本没法点「是」（返回"操作被用户取消"）——需要打开笔记本盖子或接显示器才能批准 UAC。② PaperHost 自带的 `adb` 服务进程（从 `{app}\adb\adb.exe` 启动）会一直占着这个文件，**安装包升级会在替换 `adb.exe` 时失败并回滚**（"DeleteFile … in use (5)" → "User canceled the installation"，exit code 5）。安装包现在先结束"路径在安装目录里的 adb 进程"（不动用户自己的 SDK adb），再替换文件。③ 杀掉占着 5037 端口的 adb 服务会让所有 adb 客户端（包括我的 logcat 抓取）断开。
**设备事实**：平板触摸屏是 `input_mt_wrapper`（`/dev/input/event3`，type-A 多点触摸：`ABS_MT_TRACKING_ID/BLOB_ID/POSITION_X/Y/PRESSURE`，坐标 0..1871 × 0..1403，**`ABS_MT_TOUCH_MAJOR` 只有 0..1**——没有触点大小，不能靠面积识别手掌）；手写笔是另一个设备 `huawei,ts_pen`（event4）。`/dev/input/event*` 是 `root:input rw-rw----`，adb shell 能读（`getevent` 可用），`sendevent` 写入多点触摸序列应该可以用来端到端模拟三指手势（未试，会影响用户正在用的屏幕）。
**新日志的第一批数据（用户测试三指轻点时）**：`three-finger tap: landed within 0–31 ms, lasted 51–82 ms, moved at most 0 px`——真实的快速三指轻点，三根指几乎同一个触摸报告帧落下；每次触发都对应电脑端 `full refresh` 和平板端 `slow post: protocol mode 3 … 100%`。`render:` 统计显示用户日常使用时平均每个 post 覆盖 2–24% 的面板，整页滚动时 5 s 内可达 18–29 个 ≥50% 面积的 post（快速 A2，没有 GC16）。