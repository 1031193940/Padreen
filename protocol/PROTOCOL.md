# 线协议 v2.0（当前已实现版本；v2 = 局域网由平板主动连接 + 配对码配对，数据面与 v1.6 相同）

实现：平板 `android/app/src/com/padreen/app/Receiver.java` + `Renderer.java`，电脑端 `host/padreen/src/net.cpp`（C++ 正式实现）、原型 `host/poc/mirror.py` 与 `host/poc/stress.py`（压力测试）。
传输：裸 TCP，小端。USB 模式下 **电脑主动连接** `127.0.0.1:38700`（经 `adb forward tcp:38700 tcp:38700`），平板只监听回环地址；握手之后平板**不发送任何电脑需要等待的数据**（ACK 仅用于测量）。

## 帧

```
u8 type | u8 flags | u16 reserved | u32 length | payload[length]
```

| type | 方向 | 内容 |
|---|---|---|
| `0x01` HELLO | 平板→电脑 | 36 字节（旧版 16 字节）：`u16 proto(=1), u16 w, u16 h, u16 dpi, u8 orientation, u8 appBuild, u16 rsv, u32 caps`（caps: bit0 zlib，bit1 mono1，bit2 LZ4，bit3 LZ4(mono1)，bit4 触控反馈，bit6 ORIENT = 平板随设备方向通知电脑（`T_ORIENT` / `T_GEOM`，见下面「平板转方向与音量键」），bit7 MEDIAKEYS = 显示桌面时平板的音量键以 `T_MEDIAKEY` 发给电脑，bit5 RESUME = 平板屏幕上还留着上一次会话的画面（只在断线重连时置位：显示在前台且有内容）；电脑据此决定重连后的第一帧只补发变化的部分（不整屏刷新），否则重新发一整屏 GC16）；`u8[16] deviceId`（本次安装的随机标识）+ `u8[4] keyId`（平板所存配对密钥的 SHA-256 前 4 字节，全 0 = 未配对）追加在 16 字节之后。`appBuild`（现为 18）：低于 Padreen 要求值时，Padreen 会 `adb install -r` 自带的 APK |
| `0x10` UPDATE | 电脑→平板 | `u32 seq, u8 mode, u8 codec, u16 x, u16 y, u16 w, u16 h, u32 dataLen, data` |
| `0x11` GEOM | 电脑→平板 | `u16 w, u16 h, u8 seq, u8 flags(bit0 = 拒绝，bit1 = NO_ROTATE 电脑不跟随方向), u16 rsv`：电脑现在交付的桌面尺寸（回答 `T_ORIENT`；seq 0 = 通告：会话开始、重连后、电脑上的「自动旋转」开关被切换时），见「平板转方向与音量键」 |
| `0x12` ACK | 平板→电脑 | `u32 seq, u64 tHeader, u64 tBody, u64 tDecoded, u64 tDrawn, u64 tPosted`（平板 `nanoTime`，仅测量用；**由渲染线程在对应像素被提交给系统之后发送，合并的每个 UPDATE 各发一条**） |
| `0x20` PING | 电脑→平板 | `u64 pcTime` |
| `0x21` PONG | 平板→电脑 | `u64 pcTime(回显), u64 tabletNanoTime`（用来估算时钟偏移） |
| `0x7F` BYE | 电脑→平板 | 空；平板 `finish()`，回到之前的界面 |
| `0x30` VERIFY / `0x31` VERIFYR | 调试 | **仅 FB 通道（`--es path fb`）可用**；常规通道返回失败 |
| `0x40` SELFTEST / `0x41` SELFTESTR | 调试 | 同上，仅 FB 通道 |
| `0x60` MARK | 电脑→平板 | UTF-8 文本，平板写入 logcat（`Padreen: MARK ...`），压力测试用来对齐时间线 |
| `0x61` WAKE / `0x62` WAKER | 调试 | `u8 方法, u8 波形`：1=`forceRefresh`、2=FB `invalidate`、3=FB `post`、4=普通 View 刷新；返回 UTF-8 报告。仅做唤醒研究，**生产不使用** |
| `0x63` RAWWAVE | 电脑→平板 | `i32` 原始 EinkMode 值，之后所有提交用它代替协议 mode 对应的波形；`-1` 恢复（调试；`mirror.py --wave-schedule` 用它对比各波形模式：1 DU、5 DU4、6 A2/MIX、8–11 A2 6/7/8/9 帧、8193 DU 1bpp） |

## UPDATE

- `mode`（决定这一批像素用哪个波形）：`0` DU4(5) · `1` **DU(1)，极速模式** · `2` **GL16(3)，补灰阶** · `3` **GC16 全屏(98)，全刷** · `4` DU4-5bit(7) · **`5` A2/MIX(6) · `6`/`7`/`8`/`9` A2 6/7/8/9 帧(8/9/10/11)**（v1.3 新增，平板 App build ≥5；与 DU 同属"直通"路径、同属快速类、合并时后到的覆盖先到的；Padreen 托盘「极速模式的刷新方式」选用）。
  - 平板在合并多个更新时按优先级取最强的一个：全刷(3) > 补灰阶(2) > 其余。
  - DU(1)/A2 走系统"直通"路径（~0.1 s 一次）；DU4/GL16/GC16 被系统节流到 ~0.55–0.6 s 一次。
- `codec`：`0` 原始 gray8（行优先，`w*h` 字节）· `1` zlib(gray8) · `2` 原始 mono1（每行按字节对齐，高位在前，1=白，`h*((w+7)/8)` 字节；用于极速模式，体积是 gray8 的 1/8）· `3` **LZ4 块(gray8)**（`LZ4_compress_default`，解压后恰好 `w*h` 字节；Padreen 用它发首帧/补灰阶，Python 原型仍用 zlib）。
- 平板处理：网络线程解码 → 写入影子帧缓冲（灰度 0/255 或 gray8）→ 扩大待刷矩形；渲染线程（相邻提交 ≥60 ms）取矩形内**最新**内容 → ARGB → `lockCanvas(矩形)` → `setForceNextPostMode(波形)` → `unlockCanvasAndPost`。
- 电脑端约定（hybrid 极速模式，C++ Padreen 的实际行为）：只发 1-bit 画面里**真的变了**的瓦片的外接矩形（mode=1, codec=2）；**停手 1.2 s 后补灰阶**：只补"1-bit 明显比灰度差"的瓦片、每块 ≤12% 屏、文字块优先、分阶段（1.2 s 补 1 块，4 s 补到 3 块，10 s 补完），数据是吸附过的 gray8（`<48→0`、`>208→255`），mode=2、codec=3；首帧整屏 gray8（mode=3，GC16）；整屏重绘（≥80% 面积）去抖后发一次整屏 GL16（3 s 内最多一次）。
- 计划（未实现）：多矩形 UPDATE、光标独立消息、REFRESH（手动/周期全刷）、配对/加密握手（Wi-Fi 模式）、编码时直接发 1-bit + 压缩。

## 预留（后续）

- `0x50–0x5F`：平板→电脑的**输入事件**（触控/笔：`u8 kind, u8 pointerId, u16 x, u16 y, u16 pressure, u32 timestamp, ...`），用于「触控操作反馈」（用户要求的后续任务）；当前版本不发送。
- 加密握手/配对（Wi-Fi 模式）、多矩形 UPDATE 见 PLAN.md。

## Wi-Fi（v1.4，平板 App build ≥ 6）

**传输**：平板 App 同时监听 `127.0.0.1:38700`（USB，经 `adb forward`，明文，只有 adb 够得着）和 `0.0.0.0:38701`（Wi-Fi，**必须认证 + 加密**）。电脑主动外连，不需要在电脑上开任何入站端口/防火墙规则；发现平板用"已记住的 IP → 失败则扫描 Wi-Fi 网卡所在 /24 的 38701 端口"（只走出站 TCP；只扫 Wi-Fi 网卡，不扫 VPN/隧道）。同一时刻只服务一个会话（先握手后抢会话锁，端口扫描器不会堵住真正的电脑）。

**配对（只在 USB 连接上允许）**：电脑首次连上平板时生成 32 字节随机密钥（电脑端用 Windows DPAPI 加密后存在 `padreen.ini` 的 `wifi_key`），发 `T_PAIR 0x70`（`u8[32] key`）；平板存入本 App 的私有配置并回 `T_PAIR_OK 0x75`（`u8[4] keyId`）。以后 HELLO 里的 `keyId` 与电脑密钥不符（换电脑/重装）就自动重新配对。同时电脑用 `adb shell ip addr show wlan0` 记下平板 Wi-Fi 地址。

**握手（Wi-Fi 连接，明文帧）**
1. 平板 → `T_HELLO`（36 字节）→ `T_CHALLENGE 0x71`（`u8[16] nonceT`）。
2. 电脑 → `T_AUTH 0x72`：`u8[16] nonceP | u8[32] HMAC-SHA256(key, "PD1-auth" | nonceT | nonceP)`。
3. 平板验证（常数时间）后 → `T_AUTH_OK 0x73`：`HMAC-SHA256(key, "PD1-ok" | nonceP | nonceT)`（电脑据此确认平板确有密钥）；失败/未配对 → `T_AUTH_FAIL 0x74` 并断开。
4. 会话密钥：`PRK = HMAC(salt = nonceT|nonceP, key)`；`K_p2t = HMAC(PRK, "pd1 p2t" | 0x01)[0..16)`（电脑→平板），`K_t2p = HMAC(PRK, "pd1 t2p" | 0x01)[0..16)`。

**之后每个帧是一条 AES-128-GCM 记录**：`u32 recordLength | ciphertext(整个原帧：8 字节帧头 + payload) | tag(16)`；nonce = 12 字节（前 4 字节 0 + 每方向各自的 u64 小端计数器，从 0 起每条 +1），`recordLength`（= 帧长 + 16）作为 AAD；计数器保证重放/乱序/篡改的记录验不过。

**保活（Wi-Fi 必须）**：平板的 Wi-Fi 省电会让往返延迟 70–300 ms（实测 ICMP 平均 114 ms；应用持有的 `WIFI_MODE_FULL_LOW_LATENCY` 在这台设备上 `low_latency_active_time_ms = 0`，不起作用）。电脑每 50 ms 发一个 `T_PING`、平板回 `T_PONG`，让两端无线电保持活跃：ping 降到 ~4 ms，更新往返 ~30–40 ms（USB 约 15–20 ms）。

**codec 4 = LZ4 块(mono1)**（caps bit3）：mono1 打包后的数据再 LZ4（只在压缩到 <90% 时使用）；拖大窗口时带宽从 ~5 MB/s 降到 ~80 KB/s（文字窗口）。USB 同样受益。

## 触控反馈（v1.5，平板 App build ≥ 7，HELLO caps bit4）

平板把手指/手写笔事件经**同一条连接**（USB 明文 / Wi-Fi 加密记录）回传给电脑；电脑端在对应的虚拟屏位置重放。对"只做显示"的有意放宽（用户 11-03 明确要求："可以直接通过手指的方式点击按钮、拖动窗口、拖动滚动条"）。

**`T_TOUCH 0x50`（平板→电脑）**：`u32 eventTimeMs | u8 actionMasked | u8 count | count × 16 字节`；每个指针 16 字节：`u8 id | u8 state | u8 tool | u8 buttons | u16 x | u16 y | u16 pressure | u16 touchMajor | u16 touchMinor | u16 rsv`。
- 每个 MotionEvent 一帧，**包含所有当前指针**；`state`：0 移动/保持、1 新按下、2 抬起、3 取消；`tool`：0 手指、1 手写笔、2 橡皮、3 其它；坐标为**面板像素**（0..1871 × 0..1403，已按视图尺寸缩放），`pressure` 0..1000，`touchMajor/Minor` 为面板像素。
- 平板端用独立线程写 socket（队列 ≤256，网络拥堵时只丢"纯移动"帧），UI 线程永不阻塞；会话结束清空队列。
- 电脑端（`input.cpp`）：坐标 → 虚拟屏物理像素 = 屏幕矩形.left + x × 宽 / 平板当前宽（横屏 1872、竖屏 1404；平板转方向后由 `T_GEOM` 流程更新）；**Windows 触控注入**（`InitializeTouchInjection(10, TOUCH_FEEDBACK_NONE)` + `InjectTouchInput`，按进程设置、不改系统设置；没有可见的触控圈、不移动鼠标指针）：按下 = `DOWN|INRANGE|INCONTACT`，移动 = `UPDATE|INRANGE|INCONTACT`，抬起 = `UP`（**在最后一次上报的位置抬起**，抬起帧里位置跳变会被系统以 87 拒绝），取消 = `UP|CANCELED`；**不要用 `POINTER_FLAG_NEW`（会 87）**；接触面积 `rcContact` = 手指面积（胖手指命中），压力 1..1024；手指按住不动时每 40 ms 重复上报 `UPDATE`（否则系统丢触点）；连续的"纯移动"帧合并只发最新；已按下的 id 又收到按下 → 先对旧触点发 `UP|CANCELED`；注入返回错误 → 全部触点复位。会话结束/断线时抬起所有手指。
- 备用 `touch_mode=mouse`：SendInput 绝对坐标鼠标，第一根手指 = 左键。
- 电脑进程必须是 **Per-Monitor DPI 感知（V2）**，否则在带缩放的显示器上坐标会被虚拟化而落点错位；被注入窗口若是**管理员权限**运行的，普通进程注入会被 UIPI 拦截。

## 一键全屏刷新（v1.6，平板 App build ≥ 8）

用户要求（11-03）："平板端有一键触发全屏刷新的机制；不是手动触发时尽量不要全屏刷新"。

**平板 → 电脑 `T_GESTURE 0x53`（payload：u8 kind，1 = 三指上滑，2 = 三指下滑）**：电脑把它变成键盘快捷键：上滑 = Win+Tab（多桌面 / 任务视图），下滑 = 窗口切换（默认 Ctrl+Alt+Tab，切换器停在屏幕上；`gesture_down=alttab` 时是普通 Alt+Tab）。手势由平板识别：一次触摸的**前三根手指必须在 140 ms 内落下**（单指按下后平板先压住 ≤60 ms，第二根指落下后延长到第一根指后 140 ms），三指都到齐就**整个吞掉**——这次触摸的任何 `T_TOUCH` 都不发给电脑（所以不会再有"前两根指被当成双指轻点 = 弹出右键菜单"）。轻点（≤600 ms、移动 ≤70 px）= `T_REFRESH`，上/下滑（平均纵向位移 ≥200 px 且 ≥1.6 倍横向）= `T_GESTURE`；手写笔、以及在平板主页关掉手势时，不压住也不识别。

**平板 → 电脑 `T_LEAVE 0x52`（空 payload）**：用户在平板上点了「断开」——平板马上关闭连接。电脑据此立刻结束会话；没有这条消息而连接断了（Wi-Fi 抖动、平板 App 被杀）时，电脑把它当作**意外中断**：Wi-Fi 会话保留虚拟屏最多 `lan_grace_seconds`（默认 45 s），只接纳同一块平板（PD2 的 `padId`）重新连接并继续（第一帧提前到 ~0.5 s），USB 会话不等。

**平板 → 电脑 `T_REFRESH 0x51`（空 payload）**：用户在平板上做了**三指轻点**（音量键不再用于刷新，保持调节系统音量）（三指同时落下、抬起，期间移动 <120 px、<1.2 s；手势期间不向电脑转发触控，并先对已转发的前两指发 `state=3` 取消）。电脑收到后等同托盘「立即全屏刷新」：下一轮发一整屏灰度 **GC16**（`UPDATE mode 3`，会整屏黑白闪一次，清残影并出 16 级灰阶）。平板同时计时 1.5 s：这段时间内没有收到 ≥90% 面积的 mode 3 更新（老版电脑端、链路卡住、没人连接），就用自己影子缓冲区本地 GC16 整屏刷新一次。1.5 s 内重复触发会被忽略。

**默认不再有任何自动的整屏刷新**（电脑端）：① 整页重绘（≥80% 脏面积且真实变化 ≥50% 瓦片）平静后，以前发一整屏 GL16 灰阶，现在作为普通快速更新（A2/DU）一次发出；② DXGI 丢失后的重新同步以前发整屏 GL16，现在只补"真正有差别"的区域；③ 自动"补灰阶"（局部 GL16）默认**关闭**，可在托盘选 少量/完整。仍然会发整屏 GC16 的只有：会话刚建立时的第一帧（一次）、手动（托盘/音量键/三指）、`auto_clean=1`/`full_refresh_minutes>0`/`auto_full=1` 这几个默认关闭的选项。

**平板端渲染器按类别分开合并**（不再把所有待刷矩形取外接矩形）：快速类（DU/A2）= 一个并集矩形、优先发；灰阶类（GL16 settle 盒）= 每盒一组，只在相接时合并；整屏类（GC16）= 发出时盖住并吞掉其内部的待刷块。旧版把"一块 settle 盒 + 屏幕另一侧的指针更新"取外接矩形，会变成一次巨大的 GL16 刷新。

## 平板转方向与音量键（v2.1，平板 App build ≥ 13；`NO_ROTATE` 与前台音量键规则 build ≥ 14；平板自己用加速度计判断方向、记住当前方向 build ≥ 16）

用户要求（11-07）：①"支持自动方向旋转：设备切换到竖屏时，自动把桌面旋转方向切换到竖屏"；②"设备在显示桌面的时候，音量键用于调节电脑系统音量"。（11-09 补充：旋转要在电脑控制面板上有开关，默认开；Padreen 在平板最前面时音量键调电脑音量而不是平板音量。）

**方向由平板决定，虚拟屏由电脑改。** 显示 Activity 跟随设备怎么拿（平板主页可关）：这台平板（HMW-W09）只有加速度计、没有陀螺仪/磁力计，系统自带的旋转监听又没有接传感器（`dumpsys window`：`mSensor=null`，`SCREEN_ORIENTATION_FULL_SENSOR` 只在 Activity 重新创建时生效），所以 App 自己判断：`OrientationDetector.java`（纯逻辑，有 JVM 单元测试）读加速度计，φ = atan2(x, y)（0 = 自然竖屏，+90 = 横屏，−90 = 反向横屏，±180 = 反向竖屏），低通滤波 τ≈120 ms，转出当前区间 45°+7° 滞回后、同一目标保持 250 ms、距上次切换 ≥1 s 才切（摇晃/搬动、倾斜不到约 10° 的平放状态都不做决定 = **平放时保持当前方向**），再用显式的 `setRequestedOrientation(PORTRAIT/LANDSCAPE/REVERSE_*)` 转 Activity；当前方向记在 `DisplayHub` 里，显示页重建后仍从它开始。它的 Surface 尺寸变了（横屏 1872×1404 ↔ 竖屏 1404×1872；两种形状像素数相同）就是"设备被转了"。横屏 ↔ 反向横屏（转 180°）尺寸不变：电脑不需要变，平板整屏重绘一次（GC16）。平板侧的状态机是 `OrientSync.java`（纯逻辑，有 JVM 单元测试），电脑侧在 `main.cpp` 的 `runSession` 循环里：

1. 平板：作废影子缓冲区（形状不对了），显示一张空白页（A2），发 **`T_ORIENT 0x54`（平板→电脑）**：`u16 w, u16 h, u8 seq(1..255), u8 rot, u16 rsv`（"请交付 w×h 的桌面"；seq 递增）；然后**忽略所有 UPDATE 和触摸**，直到收到同 seq 的 `T_GEOM`（线上还在飞的旧形状的帧、和此刻发出会被映射到旧虚拟屏的触摸，都不能要）。
2. 电脑：`Session::run` 在 ~0.1 s 内返回 "geometry change"；用 `ChangeDisplaySettingsEx` 把虚拟屏切到该分辨率（驱动里已登记 1872×1404 与 1404×1872 两种模式：`HKLM\SOFTWARE\Parsec\vdd\0/1`，只影响这块屏），等 Windows 报出新尺寸，更新触控映射（`TouchInjector::setPanel/setTarget`），再发 **`T_GEOM 0x11`（电脑→平板）**：`u16 w, u16 h, u8 seq(回显), u8 flags(bit0 = 没能切换，bit1 = NO_ROTATE：这台电脑当前不跟随方向，见第 4/5 点), u16 rsv`，最后**新建一个 Session**（所有画面缓冲都是旧形状）：第一帧是整屏 GC16（`quickFirst`，~0.3 s 平静后）。
3. 平板收到同 seq 的 `T_GEOM` 且尺寸与自己的 Surface 一致 → 恢复接收；之后的 UPDATE 都是新尺寸。
4. 电脑**不能或不愿**切换（电脑上的「自动旋转」关了 / `rotate=0`、驱动没有这个模式、Windows 拒绝）：回 `T_GEOM`（flags bit0 = 1，w×h = 仍在交付的尺寸；「自动旋转」关了时同时带 bit1 NO_ROTATE）。平板据此**把 Activity 锁回电脑的方向**（`setRequestedOrientation`）直到会话结束或电脑再次通告「可以旋转」，并在尺寸回到电脑的尺寸后请求一次整屏刷新（`T_REFRESH`）。老电脑从不回应：平板 3 s 超时按"被拒绝"处理，同样锁回横屏。「自动旋转」关着时，电脑始终是横屏：对横屏的请求照办（竖屏的桌面会变回横屏），对竖屏的请求一律拒绝。
5. **`T_GEOM` seq 0 = 通告**：会话开始时、Wi-Fi 重连后、以及电脑上的「自动旋转」开关被切换时发送，带电脑现在交付的尺寸和 flags bit1（NO_ROTATE：关了自动旋转）。平板的反应（`OrientSync.onGeom`）：bit1 置位 → 把 Activity 锁到通告的形状（不再追传感器、不必先发 `T_ORIENT` 被拒绝一次；形状不同就整屏重画）；bit1 清零 → 若之前被锁住（被拒绝/超时/NO_ROTATE）就解锁、重新跟随传感器，且平板的形状与通告不同时自己再发一次 `T_ORIENT`（会话建立期间被转了）。电脑上关闭「自动旋转」时若虚拟屏是竖屏，电脑先把它切回横屏再通告。HELLO 里的 `w、h` 是平板此刻 Surface 的尺寸、`orientation` = 1 横屏 / 2 竖屏，电脑据此**直接按这个形状建虚拟屏**（关了自动旋转时一律建横屏）。
6. 断线重连（Wi-Fi 宽限期内）：新的 HELLO 带着平板此刻的形状；与虚拟屏不同就先切换分辨率，并且不用 RESUME（平板屏幕上是别的形状的画面），重新发第一帧。Windows 自己改了虚拟屏尺寸（`Session` 重新初始化后发现尺寸变了）：电脑把它改回平板的形状（20 s 内最多 3 次），改不回就通告实际尺寸，由平板决定。
7. 平板触摸坐标始终是平板自己 Surface 的像素（竖屏时 0..1403 × 0..1871），电脑用当前虚拟屏矩形映射。

**`T_MEDIAKEY 0x55`（平板→电脑）**：`u8 key`（1 = 音量加，2 = 音量减，3 = 静音）。**Padreen 在平板最前面（显示 Activity 或主页 Activity 有焦点）且连着电脑**（会话在运行，或处在 Wi-Fi 重连宽限期内）时，两个 Activity 的 `dispatchKeyEvent` 都吞掉硬件音量键（按下、重复、抬起全部吞，平板自己的音量不变、系统音量面板不弹）；按下时发这个帧（按住约每秒 10 次，宽限期内只吞不发）；电脑用 `SendInput` 注入 Windows 媒体键 `VK_VOLUME_UP/DOWN/MUTE`（与键盘上的音量键完全一样）。没连电脑、Padreen 不在最前面（切到别的 App、息屏）、或在平板主页关了「音量键调节电脑音量」时，系统照常调平板自己的音量。

## 局域网连接与配对（v2.0 "PD2"，平板 App build ≥ 9）

**角色**：电脑 Padreen 常驻监听（TCP `38702` + UDP `38703`），平板 App 的主页自动在局域网里找电脑、输入配对码配对、连接；**电脑端只要启动程序**。USB 仍是"电脑经 `adb forward` 连平板回环端口 38700"（明文，物理信任），v1 的"电脑主动连平板 38701"（PD1）已**移除**。电脑端需要一次防火墙放行（入站 TCP 38702 / UDP 38703，限本地子网）——安装程序会加规则；绿色版首次运行由 Padreen 引导（需要管理员确认一次）。

### 身份
- 电脑：`pcId` = 16 随机字节（首次运行生成，存于 `padreen.ini`），`pcName` = 计算机名。平板：`padId` = 16 随机字节（App 首次启动生成，即 HELLO 的 `deviceId`），`padName` = 设备型号/名称。
- 配对结果 = 双方各存一份 **32 字节配对密钥 K** + 对方的 id/名称：电脑存"已配对平板"列表（密钥 DPAPI 加密），平板存"已配对电脑"列表。一台平板可配多台电脑，一台电脑可配多个平板（同一时刻只服务一个会话）。

### 发现（UDP 38703）
- 查询（平板→广播 `255.255.255.255` 与各网卡子网广播）：`"PDQ2"` + `u8[16] padId`（20 字节）。
- 应答（电脑→查询源地址，单播）：`"PDR2"` + `u8 ver(2)` + `u8 flags`（bit0 = 配对窗口已打开，bit1 = 忙/已有会话，bit2 = 局域网被关闭则不应答）+ `u16 tcpPort(LE)` + `u8[16] pcId` + `u8 nameLen` + `name(UTF-8，≤40 字节)`。电脑对同一来源限速（≤5 次/秒）。
- 平板另支持"手动输入电脑 IP"（跨网段/广播被拦时）。

### TCP 握手（明文帧 `u8 type | u8 flags | u16 rsv | u32 len | payload`；握手超时 6 s）
- `0x80 PD_HELLO`（平板→电脑，首帧）：`u8 ver(2) | u8 mode(0 连接 / 1 配对) | u8[16] padId | u8 nameLen | padName`。
- **连接（mode 0）**：
  1. 电脑 → `0x81 PD_CHALLENGE`：`u8[16] pcId | u8[16] nonceS | u8 nameLen | pcName`（`padId` 不在已配对列表 → 改回 `0x84 PD_FAIL(1)`）。
  2. 平板 → `0x82 PD_AUTH`：`u8[16] nonceC | u8[32] macC`，`macC = HMAC-SHA256(K, "PD2-auth-c" | pcId | padId | nonceS | nonceC)`。
  3. 电脑验证（常数时间）→ `0x83 PD_AUTH_OK`：`u8[32] macS`，`macS = HMAC-SHA256(K, "PD2-auth-s" | padId | pcId | nonceC | nonceS)`（平板据此确认电脑确有密钥）；失败 → `0x84 PD_FAIL(2)`。
  4. 会话密钥：`PRK = HMAC(salt = nonceS|nonceC, K)`；`kC2S = HMAC(PRK, "pd2 c2s" | 0x01)[0..16)`（平板→电脑），`kS2C = HMAC(PRK, "pd2 s2c" | 0x01)[0..16)`。此后每帧是一条 AES-128-GCM 记录：`u32 recordLen | 密文(整帧) | tag(16)`，nonce = 4 字节 0 + 各方向 u64 小端计数器（从 0 起），`recordLen` 作 AAD（与 v1.4 的记录格式一致）。
  5. **平板发出的第一条记录 = 数据面 `T_HELLO`**（面板尺寸、appBuild、caps、deviceId、keyId、名称），之后与 USB 会话相同（`T_UPDATE`/`T_ACK`/`T_PING`/`T_TOUCH`/`T_REFRESH`/`T_BYE`…）。
- **配对（mode 1）**（电脑的配对窗口必须开着，否则 `PD_FAIL(4)`）：
  1. 电脑 → `0x85 PD_PAIR_CH`：`u8[16] pcId | u8 nameLen | pcName | u8[65] ecdhPubS | u8[16] salt | u8 digits(9)`。
  2. 平板用户输入配对码（9 位十进制，电脑窗口显示）；两边算 `Kpw = PBKDF2-HMAC-SHA256(密码 = 配对码数字的 ASCII, salt, 100000 次, 32 字节)`；平板 → `0x86 PD_PAIR_PROOF`：`u8[65] ecdhPubC | u8[32] proofC`，其中 `transcript = SHA256("PD2-pair" | helloPayload | pairChPayload | ecdhPubC)`（两个 payload 为发送时的原始字节），`proofC = HMAC(Kpw, "PD2-pair-c" | transcript)`。
  3. 电脑验证；码错 → 记一次失败（**每个配对窗口最多 5 次失败，之后窗口自动关闭**，每次失败应答延迟 1 s）→ `PD_FAIL(5)`；对 → `0x87 PD_PAIR_OK`：`u8[32] proofS`，`proofS = HMAC(Kpw, "PD2-pair-s" | transcript)`。
  4. 双方算 `Z` = P-256 ECDH 共享密钥的 X 坐标（32 字节大端），`K = HMAC-SHA256(key = Z, "PD2-key" | transcript)`，各自保存；电脑关闭该连接并关闭配对窗口；平板随后用 mode 0 重新连接。
  - 安全性：配对码只用来证明"两端的用户是同一个人"（绑定到 ECDH 成绩单）；被动窃听拿不到 K；主动中间人需要在握手的几秒内破出 9 位码（PBKDF2 10 万次 ≈ 每次猜测 ~0.1 s）；配对窗口默认 5 分钟、码一次一换、错 5 次即关。
- `0x84 PD_FAIL`：`u8 reason | 文字(可选)`：1 未配对（平板应提示重新配对）、2 认证失败、3 忙（已有会话）、4 配对窗口未打开、5 配对码错误/次数用尽、6 局域网连接已关闭、7 协议错误。

### USB 配对（物理信任，免输码）
`T_PAIR 0x70`（电脑→平板，仅 USB 回环连接）载荷：`u8[32] K | u8[16] pcId | u8 nameLen | pcName`（旧版只有 32 字节 K）；平板回 `T_PAIR_OK 0x75`（`u8[4] keyId`，= SHA-256(K) 前 4 字节）。电脑把平板（HELLO 里的 `padId`/名称）记入已配对列表。HELLO 在 36 字节之后追加 `u8 nameLen | padName | u8 nKeys | nKeys × u8[4]`（平板持有的所有电脑密钥的 keyId = SHA-256(K) 前 4 字节；电脑据此判断自己的密钥平板还在不在，不在才重新配对；旧的 `keyId` 字段已不用，填 0）。

### 保活与其它
数据面的 50 ms `T_PING`/`T_PONG` 保活（防平板 Wi-Fi 省电）、触控回传、三指轻点刷新（`T_REFRESH`）、LZ4 mono 等与 v1.6 相同。