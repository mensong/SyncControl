# SyncControl 分阶段实现 Prompt

> 每个阶段的 prompt 可直接复制使用。按 P1 -> P8 顺序逐阶段实现，每次一个提问。

---

## 注意事项

1. **先读方案**：每次开始前先读 `方案.md`，理解完整设计上下文，不要凭记忆假设
2. **文件编码**：修改已有文件前必须检测编码，按检测到的编码写文件
3. **不破坏前序阶段**：新阶段代码不能破坏已完成的阶段功能，增量开发
4. **项目结构**：严格按照 `方案.md` 第 5 节的项目结构组织文件，不要随意更改目录结构
5. **构建验证**：每个阶段完成后必须能编译通过，C++ 用 CMake + MSVC，C# 用 dotnet build
6. **端口**：所有网络通信默认使用端口 `19881`
7. **KCP 库**：使用 ikcp.h/ikcp.c（KCP 官方 C 实现），放在 `core/third_party/kcp/` 下
8. **JSON 库**：IPC 通信使用 nlohmann/json，放在 `core/third_party/nlohmann/json/` 下
9. **不要过度设计**：只实现当前阶段需要的功能，不要提前实现后续阶段的内容
10. **代码注释**：复杂逻辑加简短注释，不要空泛注释

---

## P1：C++ 核心网络层

### Prompt

```
请实现 SyncControl 项目的 P1 阶段：C++ 核心网络层。

请先阅读 方案.md 了解完整设计，然后实现以下内容：

1. 项目骨架搭建：
   - 在 core/ 下创建 CMakeLists.txt，配置 C++17 + MSVC
   - 创建 src/main.cpp，支持命令行参数 --master 和 --slave 区分角色
   - 创建 third_party/kcp/ 放入 KCP 官方实现 (ikcp.h/ikcp.c)
   - 创建 third_party/nlohmann/json/ 放入 nlohmann/json

2. 协议定义 (net/Protocol.h + Protocol.cpp)：
   - 定义方案 4.4 节中所有消息类型枚举 (0x01~0x43)
   - 实现消息序列化/反序列化：[2字节类型][2字节长度][N字节payload]
   - 实现通道路由判断：MOUSE_MOVE 走纯UDP，其余走KCP可靠通道，CLIPBOARD_FILE_DATA 走TCP

3. UDP Socket 封装 (net/UdpSocket.h + UdpSocket.cpp)：
   - 封装 Windows UDP socket (WSAStartup + socket + bind + sendto + recvfrom)
   - 支持非阻塞模式
   - 端口 19881

4. KCP 可靠传输层 (net/KcpReliable.h + KcpReliable.cpp)：
   - 基于 UdpSocket，封装 KCP 实例
   - 实现 send/recv 接口（可靠有序）
   - 定时调用 ikcp_update 维持 KCP 内部逻辑
   - KCP 参数：nodelay=1, interval=10, resend=2, nc=1

5. TCP 文件通道 (net/TcpFileChannel.h + TcpFileChannel.cpp)：
   - 封装 Windows TCP socket，用于文件传输
   - 支持连接/监听/收发

6. 主控端网络管理 (net/MasterNet.h + MasterNet.cpp)：
   - 向被控端发起 UDP+KCP 连接
   - 发送 HANDSHAKE_REQUEST，等待 HANDSHAKE_RESPONSE
   - 维护已连接被控端列表
   - 定时发送 HEARTBEAT，检测超时断开

7. 被控端网络管理 (net/SlaveNet.h + SlaveNet.cpp)：
   - 监听 UDP 端口，接受主控端连接
   - 收到 HANDSHAKE_REQUEST 后回复 HANDSHAKE_RESPONSE（含本机屏幕分辨率）
   - 响应 HEARTBEAT

8. 服务主循环 (core/Service.h + Service.cpp)：
   - 根据 --master/--slave 参数启动对应模式
   - 事件循环：处理网络收发 + KCP update + 心跳

9. IPC 服务端 (ipc/IpcServer.h + IpcServer.cpp)：
   - Named Pipe 服务端 (\\.\pipe\SyncControl)
   - 接收 GUI 的 JSON 指令 (start_master/start_slave/stop/get_status)
   - 返回状态信息给 GUI

验证标准：两台电脑分别以 --master 和 --slave 启动，主控端能连接被控端，握手成功，心跳正常，通过 IPC 可查询状态。
```

### 注意事项

- KCP 的 output 回调需要绑定到 UdpSocket 的 sendto
- KCP 需要定时调用 ikcp_update，间隔不能超过 interval 参数
- UDP socket 要设置 SO_REUSEADDR，避免端口占用
- 握手要携带预共享密钥（本阶段可先用硬编码占位，P8 再完善安全）
- HANDSHAKE_RESPONSE 必须包含被控端的屏幕分辨率（GetSystemMetrics）
- main.cpp 要处理 Windows 消息循环（部分 Win32 API 需要消息循环）
- 编译为 x64

---

## P2：输入捕获 + 输入注入

### Prompt

```
请实现 SyncControl 项目的 P2 阶段：输入捕获与输入注入。

请先阅读 方案.md 了解完整设计，然后基于 P1 阶段已有的网络层，实现以下内容：

1. 输入捕获模块 (input/InputCapture.h + InputCapture.cpp) — 主控端：
   - 使用 SetWindowsHookEx 安装 WH_MOUSE_LL 和 WH_KEYBOARD_LL 全局低级钩子
   - 捕获方案 4.1 节中所有鼠标事件：
     * 鼠标移动 (WM_MOUSEMOVE)
     * 左键/右键/中键 按下和抬起 (WM_LBUTTONDOWN/UP, WM_RBUTTONDOWN/UP, WM_MBUTTONDOWN/UP)
     * 侧键 X1/X2 按下和抬起 (WM_XBUTTONDOWN/UP, XBUTTON1/XBUTTON2)
     * 垂直滚轮 (WM_MOUSEWHEEL)
     * 水平滚轮 (WM_MOUSEHWHEEL)
   - 捕获键盘事件 (WM_KEYDOWN/WM_KEYUP/WM_SYSKEYDOWN/WM_SYSKEYUP)，记录 vk_code 和 scan_code
   - 维护"当前焦点"状态：本地(Master) 或 某个被控端
   - 焦点在远程时：吞掉本地事件，通过网络转发给对应被控端
   - 焦点在本地时：不吞，正常传递
   - 鼠标移动合并节流：维护待发 MOUSE_MOVE 槽位，新事件覆盖旧值

2. 输入注入模块 (input/InputInject.h + InputInject.cpp) — 被控端：
   - 使用 SendInput 实现方案 4.2 节中所有注入：
     * 鼠标移动 (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE，坐标需转换为 0~65535 范围)
     * 左键/右键/中键 按下抬起 (MOUSEEVENTF_LEFTDOWN/UP, RIGHTDOWN/UP, MIDDLEDOWN/UP)
     * 侧键 X1/X2 (MOUSEEVENTF_XDOWN/UP, dwData=XBUTTON1/XBUTTON2)
     * 垂直滚轮 (MOUSEEVENTF_WHEEL)
     * 水平滚轮 (MOUSEEVENTF_HWHEEL)
     * 键盘按下抬起 (KEYBDINPUT, wVk + wScan)

3. 切换逻辑：
   - 主控端发送 SWITCH_IN (含初始坐标) 给被控端，隐藏本地光标
   - 被控端收到 SWITCH_IN 后移动光标到指定位置
   - 主控端发送 SWITCH_OUT，被控端收到后不再注入输入
   - 本阶段切换触发暂时用硬编码：鼠标碰到任意屏幕边缘即切换到第一个已连接的被控端（P3 阶段再实现精确路由）

4. 网络层集成：
   - MOUSE_MOVE 走纯 UDP 通道
   - MOUSE_BUTTON/MOUSE_WHEEL/KEY_DOWN/KEY_UP/SWITCH_IN/SWITCH_OUT 走 KCP 可靠通道
   - 被控端收到消息后调用 InputInject 对应方法

验证标准：主控端鼠标移到屏幕边缘后切换到被控端，被控端光标跟随移动，左/右/中/侧键点击、滚轮、键盘输入都能在被控端正确生效。
```

### 注意事项

- 低级钩子回调必须快速返回，否则系统会移除钩子（超时约 300ms）
- 鼠标绝对坐标要乘以 65535 再除以屏幕分辨率：`x * 65535 / screenW`
- WH_MOUSE_LL 的 lParam 是 MSLLHOOKSTRUCT，包含 pt.x/pt.y 和 mouseData
- 滚轮的 delta 在 mouseData 高字
- 键盘要处理 WM_SYSKEYDOWN/UP（Alt 组合键）
- 吞掉事件时回调返回非零值（鼠标）或返回 1（键盘）
- 隐藏/显示光标用 ShowCursor(FALSE)/ShowCursor(TRUE)，注意要多次调用确保生效
- 钩子线程需要有消息循环（DispatchMessage），否则钩子不工作
- 不要在钩子回调里做任何阻塞操作

---

## P3：屏幕布局图 + 边缘段路由器

### Prompt

```
请实现 SyncControl 项目的 P3 阶段：屏幕布局图与边缘段路由器。

请先阅读 方案.md 了解完整设计，然后基于 P1/P2 已有功能，实现以下内容：

1. 屏幕布局数据结构 (layout/ScreenLayout.h + ScreenLayout.cpp)：
   - 实现 ScreenNode 结构（id, hostname, ip, screenW, screenH, layoutRect）
   - 实现 EdgeSegment 结构（fromEdge, rangeStart, rangeEnd, toId, toEdge）
   - 实现 ScreenRouting 结构（screenId, segments[4]）
   - 实现布局管理类：加载/保存布局配置，管理所有 ScreenNode 和 ScreenRouting

2. 边缘路由器 (layout/EdgeRouter.h + EdgeRouter.cpp)：
   - 实现 lookupRoute(routing, edge, ratio) 查询函数（方案 4.3 节）
   - 实现坐标换算 calcTargetPos(seg, globalRatio, targetScreen)（方案 4.3 节）
   - 边缘检测：给定鼠标坐标和屏幕尺寸，判断是否触碰边缘，返回触碰的 Edge 和位置比例 ratio

3. 替换 P2 的硬编码切换逻辑：
   - 鼠标移动时，如果触碰屏幕边缘，计算 globalRatio
   - 调用 EdgeRouter.lookupRoute 查询目标
   - 如果找到目标段：计算目标坐标，发送 SWITCH_IN
   - 如果落在空隙段：将鼠标位置回退到边缘内侧 1px，不切换
   - 被控端之间的切换也要支持（被控端收到 SWITCH_OUT 后，主控端根据被控端的边缘路由表决定下一步）

4. 布局配置通过 IPC 下发：
   - IPC 收到 start_master 指令时，解析 layout JSON（方案 4.6 节格式）
   - 构建 ScreenLayout 和所有 ScreenRouting
   - 运行时收到 update_layout 指令时热更新路由表

验证标准：配置 3 台电脑（Master + B + C），B 在 Master 右边，C 在 Master 上边右半部分。鼠标从 Master 右边缘移出切换到 B，从 Master 上边缘右半部分移出切换到 C，从上边缘左半部分移出不切换（卡在边缘）。
```

### 注意事项

- 边缘检测要考虑屏幕多显示器情况（GetSystemMetrics SM_XVIRTUALSCREEN/SM_YVIRTUALSCREEN）
- ratio 计算：水平边缘用 mouseX/screenW，垂直边缘用 mouseY/screenH
- 空隙段处理：鼠标回退到边缘内侧 1px，用 SetCursorPos 强制定位
- 坐标换算的 localRatio 公式：(globalRatio - rangeStart) / (rangeEnd - rangeStart)
- 被控端切换回主控端时，主控端要恢复光标显示并定位到对应边缘
- 路由表热更新时不能影响正在进行的输入操作（如按键按住状态）
- Edge 枚举顺序要和 segments 数组索引一致：Top=0, Bottom=1, Left=2, Right=3
- 浮点比较用 epsilon，不要直接 == 比较

---

## P4：C# GUI 布局编辑器 + IPC

### Prompt

```
请实现 SyncControl 项目的 P4 阶段：C# WPF GUI 管理程序。

请先阅读 方案.md 了解完整设计，然后实现以下内容：

1. WPF 项目搭建 (gui/SyncControl.GUI/)：
   - .NET 8 WPF 项目，添加 NuGet：CommunityToolkit.Mvvm、H.NotifyIcon.Wpf
   - App.xaml 启动逻辑
   - MVVM 架构：Views + ViewModels + Models

2. 数据模型 (Models/)：
   - ScreenNode.cs：id, hostname, ip, screenW, screenH, 布局矩形位置 (X/Y/Width/Height)
   - EdgeSegment.cs：fromEdge, rangeStart, rangeEnd, toId, toEdge
   - LayoutConfig.cs：screens 列表 + routing 字典，序列化为方案 4.6 节 JSON 格式

3. 布局编辑器 (Views/MainWindow.xaml + LayoutCanvas)：
   - 画布区域：中心固定一个矩形标注"本机（主控）"，周围可添加/拖拽被控电脑矩形
   - 每个矩形显示电脑名称、IP、屏幕分辨率、连接状态（绿色/灰色/黄色）
   - 鼠标拖拽矩形改变位置，松手后自动吸附对齐（边对边贴合，阈值 5px）
   - 拖拽时实时显示边贴合高亮：贴合段用高亮色标出，不同目标用不同颜色
   - 一条边上可有多段高亮，空隙段不亮
   - 右键矩形弹出菜单：编辑属性（IP、端口、屏幕分辨率）、删除
   - 工具栏按钮：添加电脑、保存布局、开始控制、停止控制

4. RoutingBuilder (Services/RoutingBuilder.cs)：
   - 根据画布上所有矩形的位置，计算每台电脑 4 条边的 EdgeSegment 列表
   - 算法：对每条边，找所有贴合的其他矩形，计算重叠区间，归一化为 [0.0, 1.0] 比例段
   - 按 rangeStart 排序，段之间自动留空隙
   - 矩形位置变化时重新计算

5. IPC 客户端 (Services/IpcClient.cs)：
   - Named Pipe 客户端，连接 \\.\pipe\SyncControl
   - 发送 JSON 指令：start_master(含layout)/start_slave/stop/update_layout/get_status
   - 接收服务端状态推送：status/peer_connected/peer_disconnected

6. 服务管理 (Services/ServiceManager.cs)：
   - 启动/停止 C++ 服务进程 (core 可执行文件)
   - 传递命令行参数 --master 或 --slave

7. ViewModel (ViewModels/MainViewModel.cs)：
   - 绑定矩形列表、连接状态、当前控制状态
   - "开始控制"按钮：调用 RoutingBuilder 生成路由表，通过 IPC 发送 start_master
   - "停止控制"按钮：通过 IPC 发送 stop
   - 实时更新连接状态（监听 IPC 推送）

验证标准：GUI 能添加被控电脑矩形并拖拽摆放，边贴合高亮正确显示，点击"开始控制"后 C++ 服务启动并连接被控端，鼠标能按布局切换。
```

### 注意事项

- WPF 拖拽用 MouseDown/MouseMove/MouseUp 事件，不要用 DragDrop（那是文件拖放）
- 矩形吸附对齐：松手时检查四周 5px 内是否有其他矩形的边，有则吸附
- 边贴合高亮可以用 Canvas 上绘制小矩形或 Path 实现
- Named Pipe 连接要处理服务端未启动的情况（重试或提示用户）
- JSON 序列化用 System.Text.Json，注意中文编码
- 连接状态颜色用 Converter 或直接绑定 Background
- 布局保存到本地文件（如 layout.json），下次启动自动加载
- 不要在 UI 线程做 IPC 通信，用 async/await
- H.NotifyIcon.Wpf 的托盘图标本阶段先占位，P7 阶段完善

---

## P5：剪贴板文本共享

### Prompt

```
请实现 SyncControl 项目的 P5 阶段：剪贴板文本共享。

请先阅读 方案.md 了解完整设计，然后基于 P1-P4 已有功能，实现以下内容：

1. 剪贴板监听模块 (clipboard/ClipboardMonitor.h + ClipboardMonitor.cpp)：
   - 使用 AddClipboardFormatListener 注册剪贴板监听 (WM_CLIPBOARDUPDATE)
   - 需要创建一个隐藏窗口接收消息
   - 检测剪贴板内容类型：CF_UNICODETEXT（文本）或 CF_HDROP（文件）
   - 文本内容：读取并缓存为 UTF-8 string
   - 文件内容：本阶段只记录文件元信息（文件名+大小），P6 再实现传输

2. 文本剪贴板传输流程（方案 4.5 节）：
   - 焦点从电脑 X 切换到电脑 Y 时：
     * 如果 X 是 Master：读取本地缓存的剪贴板文本，发送 CLIPBOARD_TEXT 给 Y
     * 如果 X 是被控端：向 X 发送 CLIPBOARD_REQUEST 请求文本，X 回传 CLIPBOARD_TEXT，再转发给 Y
   - 被控端收到 CLIPBOARD_TEXT：OpenClipboard + EmptyClipboard + SetClipboardData(CF_UNICODETEXT)
   - CLIPBOARD_TEXT 走 KCP 可靠通道

3. 集成到切换逻辑：
   - 在 P2/P3 的 SWITCH_IN/SWITCH_OUT 流程中加入剪贴板同步
   - 切换前先获取源端剪贴板内容，切换后发送给目标端
   - 注意：只在内容变化时才传输，避免每次切换都发空剪贴板

4. GUI 状态显示：
   - 在 GUI 中显示当前剪贴板同步状态（文本/文件/空）

验证标准：在 Master 上复制文本，鼠标切换到被控端后直接 Ctrl+V 能粘贴相同文本；反向同理。
```

### 注意事项

- AddClipboardFormatListener 需要一个 HWND，创建隐藏消息窗口
- OpenClipboard 要处理失败情况（其他程序可能锁住剪贴板），重试 3 次
- SetClipboardData 前必须 EmptyClipboard
- 剪贴板数据用 GlobalAlloc(GMEM_MOVEABLE) 分配
- WM_CLIPBOARDUPDATE 可能高频触发（某些软件会频繁写入剪贴板），需要去重
- 文本编码统一用 UTF-8，Windows 剪贴板用 UTF-16，需要转换
- 剪贴板传输和切换之间有竞态：先完成剪贴板传输再显示目标端光标
- 不要在钩子回调里操作剪贴板，用 PostMessage 转到单独窗口处理

---

## P6：剪贴板文件按需传输

### Prompt

```
请实现 SyncControl 项目的 P6 阶段：剪贴板文件按需传输。

请先阅读 方案.md 了解完整设计，然后基于 P1-P5 已有功能，实现以下内容：

1. 文件元信息提取 (clipboard/ClipboardMonitor 扩展)：
   - 检测到 CF_HDROP 时，用 DragQueryFile 提取文件路径列表
   - 对每个文件：获取文件名、文件大小（GetFileSizeEx）、是否为目录
   - 生成 CLIPBOARD_FILES_META 消息（文件名列表 + 各文件大小）

2. 延迟渲染机制（方案 4.5 节文件剪贴板）：
   - 被控端收到 CLIPBOARD_FILES_META 后：
     * 记录文件元信息到内存
     * 设置剪贴板为自定义延迟格式（RegisterClipboardFormat + SetClipboardData 延迟渲染）
     * 延迟渲染：当有程序请求剪贴板数据时，触发 WM_RENDERFORMAT 消息
   - 用户在目标端 Ctrl+V 粘贴时：
     * 检测到粘贴操作，向源端发送 CLIPBOARD_REQUEST（含请求的文件列表）
     * 源端收到请求后，通过 TCP 文件通道分片发送 CLIPBOARD_FILE_DATA

3. 文件传输 (clipboard/FileTransfer.h + FileTransfer.cpp)：
   - 使用 P1 阶段的 TcpFileChannel 进行文件传输
   - 发送端：读取文件，按 64KB 分片发送 CLIPBOARD_FILE_DATA（含文件索引+偏移+数据）
   - 接收端：收齐分片后写入本地临时目录（GetTempPath）
   - 多文件逐个传输，每个文件传输完成后通知接收端
   - 传输进度回调（供 GUI 显示进度）

4. 粘贴完成：
   - 接收端文件写入完成后，用 SHCreateShellFolderViewEx 或 Explorer 的文件操作完成粘贴
   - 或者更简单：将文件路径列表以 CF_HDROP 格式放入剪贴板，让 Explorer 自己处理

5. GUI 进度显示：
   - 文件传输时在 GUI 中显示进度条（文件名、已传输/总大小、百分比）

验证标准：在 Master 上复制一个文件，鼠标切换到被控端，Ctrl+V 后文件被传输到被控端并完成粘贴。多文件和目录也要支持。
```

### 注意事项

- CF_HDROP 用 DragQueryFileCount/DragQueryFile 提取路径
- 延迟渲染：SetClipboardData(format, NULL) 表示延迟，系统会发 WM_RENDERFORMAT
- 文件写入到临时目录，路径要防穿越（检查 .. 和绝对路径）
- 64KB 分片要记录：文件索引、当前片偏移、是否最后一片
- TCP 文件通道要单独连接，不复用 UDP 通道
- 大文件传输不要阻塞主循环，用单独线程或异步 IO
- 目录传输要递归：先发目录结构，再逐个发文件
- 传输中断（网络断开）要能清理临时文件
- 文件名包含中文时要正确处理编码

---

## P7：托盘图标 + 状态显示 + 被控端 GUI + 错误处理

### Prompt

```
请实现 SyncControl 项目的 P7 阶段：托盘图标、状态显示、被控端 GUI 和错误处理。

请先阅读 方案.md 了解完整设计，然后基于 P1-P6 已有功能，实现以下内容：

1. 主控端托盘图标 (TrayIcon/TrayIconManager.cs)：
   - 使用 H.NotifyIcon.Wpf 创建系统托盘图标
   - 右键菜单：开始控制 / 停止控制 / 打开管理界面 / 退出
   - 左键单击：切换主窗口显示/隐藏
   - 气泡通知：被控端连接/断开、文件传输完成
   - 图标状态：运行中（绿色）/ 已停止（灰色）/ 有错误（红色）

2. 被控端 GUI (Views/SlaveWindow.xaml + ViewModels/SlaveViewModel.cs)：
   - 极简界面：显示"等待主控端连接..."状态
   - 连接成功后显示主控端 IP 和连接状态
   - 被控端托盘图标：右键菜单（退出）/ 左键显示状态窗口
   - 被控端不需要布局编辑器

3. 状态显示增强：
   - 主控端 GUI 状态栏：当前焦点电脑、已连接被控端数量、各被控端延迟
   - 延迟测量：心跳往返时间 RTT
   - 被控端 GUI：当前主控端 IP、连接持续时间

4. 错误处理与自动重连：
   - 网络断开检测：心跳超时（3 次未收到心跳判定断开）
   - 主控端：被控端断开时更新 GUI 状态为灰色，尝试自动重连（间隔 5s）
   - 被控端：主控端断开时回到"等待连接"状态，继续监听
   - IPC 断开：GUI 检测到 C++ 服务崩溃时提示用户并支持重启
   - 输入注入失败：记录日志，不影响其他功能

5. 日志系统：
   - C++ 端：简单文件日志（OutputDebugString + 文件），记录连接/断开/错误
   - C# 端：log 到文件，记录 IPC 通信/状态变化/异常

6. 开机自启动（可选）：
   - 注册表 HKCU\Software\Microsoft\Windows\CurrentVersion\Run
   - GUI 设置中提供开关

验证标准：主控端和被控端都有托盘图标，状态显示正确，网络断开后自动重连，服务崩溃后 GUI 能提示并重启。
```

### 注意事项

- H.NotifyIcon.Wpf 的 ContextMenu 要用 WPF 菜单，不是 WinForms
- 托盘图标在应用退出时要显式 Dispose，否则残留
- 自动重连不要在 UI 线程做，用后台 Task + 延迟
- 日志文件大小要限制（如 10MB 轮转），不要无限增长
- 被控端窗口默认隐藏到托盘，不显示在任务栏
- 错误处理不要用空 catch 吞异常，至少记日志
- 心跳超时阈值要合理：LAN 环境 3s（3 次 * 1s 间隔）
- RTT 计算：发送心跳时记录时间戳，收到回复时计算差值

---

## P8：KCP 参数调优 + 安全认证 + 跨网段处理

### Prompt

```
请实现 SyncControl 项目的 P8 阶段：生产级优化。

请先阅读 方案.md 了解完整设计，然后基于 P1-P7 已有功能，实现以下内容：

1. KCP 参数调优：
   - 根据网络环境动态调整 KCP 参数：
     * LAN 环境：nodelay=1, interval=10, resend=2, nc=1, wndsize=128
     * 跨网段：nodelay=1, interval=20, resend=3, nc=1, wndsize=64
   - 通过心跳 RTT 自动判断网络环境（RTT < 2ms 为 LAN）
   - 鼠标移动纯 UDP 通道也加序列号，接收端检测丢包率，供调优参考

2. 安全认证：
   - 首次运行时生成随机预共享密钥（256-bit），保存到配置文件
   - 主控端和被控端需要配置相同密钥才能连接
   - HANDSHAKE_REQUEST 携带密钥的 SHA-256 哈希，被控端校验
   - 校验失败直接断开连接
   - 密钥可通过 GUI 导入/导出（方便配置多台电脑）

3. 跨网段 UDP 连通性处理：
   - 检测是否同网段：比较主控端和被控端的 IP 子网
   - 跨网段时自动调整 KCP 参数（更长重传间隔）
   - UDP 不通时回退提示：检测 UDP 包是否到达，不通则提示用户检查防火墙/路由
   - 支持 NAT 穿透占位（本阶段不实现 STUN/TURN，但预留接口）

4. 性能调优：
   - 鼠标移动合并：确认节流逻辑在高频移动下不堆积
   - 钩子回调耗时 profiling：确保 < 0.1ms
   - KCP flush 策略优化：批量 flush 而非每包 flush
   - 内存池：频繁分配的 Message 结构用对象池

5. GUI 安全配置界面：
   - 设置页面：密钥管理（显示/导入/导出/重新生成）
   - 网络设置：手动指定 KCP 参数（高级选项）

6. 最终集成测试：
   - 多台电脑完整流程测试
   - 长时间运行稳定性测试（内存泄漏检测）
   - 网络中断恢复测试

验证标准：系统在 LAN 和跨网段环境下稳定运行，密钥认证有效，鼠标键盘操作延迟无明显感知，长时间运行无内存泄漏。
```

### 注意事项

- SHA-256 用 Windows CNG API (BCryptCreateHash) 或嵌入一个小型哈希库
- 密钥文件要设置 ACL 只允许当前用户访问
- KCP 参数动态切换时要平滑，不能断开现有连接
- 跨网段检测：比较 IP 的网络地址部分（需要子网掩码）
- UDP 连通性检测：发送探测包，等待回应，超时则判定不通
- 内存池用简单的 freelist 即可，不需要复杂实现
- profiling 用 QueryPerformanceCounter 测量钩子回调耗时
- 不要引入大型第三方库（如 OpenSSL），保持依赖最小
- 最终编译为 Release 模式，开启 /O2 优化
