# QuickerBridge UE 后续改进方向

日期：2026-07-21

本文参考 Blender 版 QuickerBridge：`D:\WorkSpace\Code\BlenderAddons\QuickerBridge`。

参考边界很重要：Blender 版只能提供“本地自动化桥”的工程经验，不能照搬实现。UE 版应该继续围绕 UE 自身场景设计，包括 Slate 控件路径、Editor Subsystem、`PythonScriptPlugin`、蓝图/材质图表 API、Content Browser、视口客户端、PIE 状态、多 UE 实例和编辑器私有 API 兼容风险。

## 当前 UE 版基线

UE 版已经不是简单脚本接收器，现有能力包括：

- HTTP 路由：`/context/current`、`/context/detail`、`/context/debug`、`/python/execute`。
- HTTP 回调进入编辑器对象前会切回游戏线程。
- `QuickerBridgeContextService` 每次即时解析 Slate 路径，不长期缓存 `FWidgetPath`。
- 上下文能识别关卡视口、PIE/Simulate、显示模式、图表类型、图表节点选择、Content Browser、顶部栏、标签栏、底部命令行等。
- 暴露给 Python/蓝图的编辑器工具包括蓝图/材质节点生成、图表刷新、面板控制、视口显示模式、射线检测、Content Browser 过滤器、UI 缩放、反射属性读写。
- Windows 下已有可选的多 UE 固定端口协调，基于互斥锁和前台窗口事件。
- 已有基于鼠标所在区域的 Scroll Lock 视口控制。

## Blender 版可借鉴的点

Blender 版值得借鉴的是服务化思路，不是代码结构本身。

- 固定本地端口要配套健康检查和调试入口。
- 上下文读取和 Python 执行要分层：读状态轻量，执行命令受控。
- 最近错误应该被记录，并能通过 debug 接口看到。
- 长任务不适合全部走同步 HTTP，需要任务生命周期。
- 鼠标坐标来源要显式，方便定位 DPI、多屏和前台窗口问题。
- Python 执行是高权限入口，应提供可选 token 或开关。
- 当主文件继续增长时，应把运行状态、HTTP 路由、Python 执行、上下文细节拆到独立文件。

## 不建议照搬的点

- 不照搬 Blender 的 modal listener/timer 执行模型。UE 有游戏线程、Slate input processor、`AsyncTask`、`FTSTicker` 和编辑器 delegate。
- 不照搬 Blender 的 `screen.areas` 上下文模型。UE 应继续使用 Slate 路径和编辑器 subsystem。
- 不让 UE 返回 Blender 风格状态字符串。UE 标签应继续围绕 `Viewport_Level`、`Graph_Event`、`Graph_Material`、`ContentBrowser`、PIE、编辑器模式、Actor 类型和 ViewMode。
- 不急着改成临时文件 owner 模型。当前 Windows 互斥锁更适合 UE 多进程端口协调。
- 不把 Quicker 的工作流策略固化进 C++。C++ 提供稳定基础能力，组合逻辑继续放在 Quicker 或 Python。

## 优先级建议

### 1. 增加 `/health`

建议新增 `GET /health`，返回结构化 JSON：

- `ok`
- `processId`
- `project`
- `engineVersion`
- `pythonInitialized`
- `multiInstanceSwitchingEnabled`
- `ownsPort`，仅在启用多实例端口协调时有意义
- `routesBound`
- `recentErrors` 数量
- 后续可加 `activeTaskName`

这是最低风险、最高收益的第一步。Quicker 在执行 Python 前可以先确认自己连到的是预期 UE 进程。

### 2. 增加最近错误记录

建议新增一个小的运行状态模块，功能类似 Blender 版 `BridgeState`，但用 UE C++ 实现：

- 记录路由绑定失败。
- 记录 JSON 解析失败和非法执行模式。
- 记录 Python 执行失败，包含执行模式和截断后的输出。
- 通过 `/context/debug` 或未来的 `/debug/state` 暴露最近错误。

不要记录每一次正常的上下文 miss。真正有价值的是服务、路由、JSON、Python、线程状态类错误。

### 3. 给 Python 执行增加可选保护

`/python/execute` 会执行本机 Python，属于高权限入口。建议新增编辑器设置：

- `bEnablePythonExecution`，默认 `true`，保持当前兼容性。
- `ExecutionToken`，可为空。
- `bRequireExecutionToken`，默认 `false`。

启用 token 后，变更性路由要求请求头 `X-QuickerBridge-Token`。上下文读取接口可以保持免 token，除非后续有明确需求。

### 4. 增加异步任务生命周期

UE Python 可能触发资产导入、蓝图编译、图表重建、批量修改等长任务。建议新增：

- `POST /python/execute_async`
- `GET /task?id=...`

建议任务状态：

- `QUEUED`
- `RUNNING`
- `FINISHED`
- `ERROR`
- `EXPIRED`

先只覆盖 Python 执行任务，不要一次性改造所有现有接口。任务结果保存在有上限的内存表里，按时间和数量裁剪。

### 5. 改进鼠标坐标来源

当前 UE 版主要读 Slate 当前鼠标位置。建议在上下文接口支持可选请求头：

```http
X-Mouse: x,y
```

并在 `/context/detail` 返回：

- `coordSource`: `Header`、`SlateCursor` 或 `Unavailable`
- 命中的窗口标题
- 鼠标在窗口内的局部坐标
- 顶层窗口几何信息

这样能更好排查 Quicker 坐标、DPI、多屏和前台窗口边界问题，同时不破坏已有 Slate 光标路径。

### 6. 扩展 UE 特有的结构化上下文

`/context/current` 应保持短标签和兼容性。新增细节优先放进 `/context/detail`：

- Content Browser 当前路径、选择资产数量、资产类。
- 关卡视口中选中 Actor 的名称、类、组件摘要。
- 当前 Blueprint 类、Graph 名称、Schema、是否 dirty。
- 材质图表对应的 Material 或 MaterialFunction 路径。
- PIE 状态、World 类型、当前命中的 LevelViewportClient 信息。

这样 Quicker 可以读取结构化字段分支，不必继续解析越来越长的字符串标签。

### 7. 强化蓝图和图表工具

下一阶段适合围绕 UE 图表工作流继续加强：

- 节点生成返回结构化状态，而不是只返回对象或 `null`。
- 增加只读的“当前蓝图图表可用 Action 查询”能力。
- 在 `/context/detail` 暴露选中节点数组和节点关键字段。
- 蓝图编译、蓝图切换、Graph/Schema 变化时失效 action 缓存。
- 自动连线失败时返回原因，例如无兼容引脚、执行引脚已占用、Schema 禁止连接。

这些都是 UE 独有价值，Blender 版只能提供“要有结构化结果”的思路。

### 8. 降低 Content Browser 私有 API 风险

当前 Content Browser 过滤器控制依赖 `ContentBrowser/Private`，这很实用，但 UE 小版本升级时风险较高。建议：

- 私有 Content Browser 逻辑继续集中在 `QuickerBridgeContentBrowserFilters.cpp`。
- 标明 UE 5.7 相关假设。
- Slate 结构变化时返回清晰失败字符串。
- 每次 UE 版本升级后手动验证过滤器定位、清除、切换、多个 Content Browser 同屏。

### 9. 增强多 UE 实例可观测性

当前互斥锁加前台窗口 hook 的方向适合 Windows UE 使用。下一步重点不是换方案，而是增加可见性：

- `/health` 暴露当前是否持有端口。
- debug 状态暴露最近前台进程 id、最近 claim/release 时间。
- 固定端口策略保持可预测，不静默换端口。
- 增加两个 UE 实例同时打开时的手动测试清单。

### 10. 行为稳定后再拆分 `QuickerBridge.cpp`

`QuickerBridge.cpp` 目前同时负责路由、响应、Python 执行、上下文识别、debug 输出和模块启动关闭。建议在 `/health` 和运行状态落地后再拆：

- `QuickerBridgeHttpRoutes.*`
- `QuickerBridgeRuntimeState.*`
- `QuickerBridgePythonExecution.*`
- `QuickerBridge.cpp` 保留为模块启动/关闭入口。

拆分应以降低维护风险为目标，不要只为了文件看起来更整齐。

## 建议实施顺序

1. 新增 `/health`，同时引入运行状态和最近错误记录。
2. 为 `/python/execute` 增加可选 token 和执行开关。
3. 上下文接口支持 `X-Mouse`，并在 detail/debug 里返回 `coordSource`。
4. 新增异步 Python 执行和 `/task` 查询。
5. 扩展 Content Browser、Graph、Viewport 的结构化上下文。
6. 行为稳定后拆分 HTTP 路由和运行状态文件。
7. 增加发布前手动检查清单：UE 启动、插件重载、两个 UE 实例、context current/detail/debug、Python 成功和失败、蓝图节点生成、材质节点生成、Content Browser 过滤器、视口显示模式切换。

## 下一步最推荐先做什么

先实现 `/health`。它改动小、适合 UE 当前架构，并且能让 Quicker 在执行任何动作前确认服务状态。第二步做 `/python/execute` 的可选 token 保护。二者能同时补齐可靠性和安全边界，不会破坏现有上下文标签和工作流接口。
