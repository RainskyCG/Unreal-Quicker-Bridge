# Unreal Quicker Bridge

QuickerBridge 是一个仅用于 Unreal Editor 的桥接插件。它为 Quicker、Python 和其他本地自动化工具提供：

- 鼠标所在 UE 编辑器区域的上下文识别。
- 通过本地 HTTP 执行 Unreal Python。
- 面向 Python/蓝图的通用编辑器控制接口。
- 蓝图和材质节点生成。
- 内容浏览器过滤器控制。
- 多 UE 实例固定端口协调。
- 基于鼠标所在区域的 Scroll Lock 控制。

本文档是插件的统一入口和长期记忆文件。**以后每次修改插件接口、HTTP API、行为、设置或关键架构时，必须同步更新本文档和末尾更新记录。**

## 下载与安装

配套项目：[Blender Quicker Bridge](https://github.com/RainskyCG/Blender-Quicker-Bridge)。两者分别适配各自软件，端口、上下文标签与 Python API 不通用。

从 Releases 下载对应引擎版本的插件包，将其中的 `QuickerBridge` 放入 `<Project>/Plugins/`，启用插件及其 Python、Modeling Tools 依赖并重启编辑器。源码包需要自行编译；不同 UE 版本的二进制不能混用。

本次发布提供 UE 5.8.3 Win64 构建，已通过完整编译、模块加载、HTTP 健康检查及 Python 执行验证。界面交互与多实例切换仍需实际场景回归；其他 UE 版本需重新编译验证。

服务检查：`http://127.0.0.1:18080/health`。设置入口：`Editor Preferences > Plugins > QuickerBridge`。

- `Ctrl+Alt+P`：复制鼠标所在区域的详细上下文 JSON。
- `Ctrl+Alt+Shift+V`：执行剪贴板中的 Unreal Python；只运行来源可信的代码。

## 快速信息

| 项目 | 当前值 |
|---|---|
| 插件目录 | `Plugins/QuickerBridge` |
| 模块 | `QuickerBridge` |
| 模块类型 | `Editor` |
| 默认 HTTP 地址 | `http://127.0.0.1:18080` |
| 主要 Python 类 | `unreal.QuickerBridgeLibrary` |
| UE 版本 | UE 5.8.3 Win64 已通过编译和基础运行验证；开发工程使用 UE 5.7 |
| 依赖插件 | `PythonScriptPlugin`、`ModelingToolsEditorMode` |
| 编译目标 | `ModularHallwayEditor Win64 Development` |

## 设计原则

- C++ 只提供通用、宽泛、基础框架接口。
- 具体工作流、条件判断和组合行为优先放在 Python 或 Quicker 中。
- 鼠标上下文按调用即时解析，不长期缓存 `FWidgetPath`，避免 Slate 控件销毁后产生悬空引用和崩溃。
- 上下文字符串保持稳定的旧标签顺序；需要更详细信息时使用 `/context/detail`。
- HTTP 与编辑器对象操作最终切回游戏线程执行。

## HTTP API

所有响应均附带以下响应头，便于确认当前由哪个 UE 实例提供服务：

- `X-QuickerBridge-ProcessId`
- `X-QuickerBridge-Project`
- `Cache-Control: no-store`

| 方法 | 完整地址 | 功能 |
|---|---|---|
| `GET` | `http://127.0.0.1:18080/context/current` | 返回鼠标所在区域的简洁上下文字符串。 |
| `GET` | `http://127.0.0.1:18080/health` | 返回引擎版本、进程、端口、Python 初始化和路由绑定状态。 |
| `GET` | `http://127.0.0.1:18080/context/detail` | 返回结构化上下文 JSON，包含区域、图表、选择、节点和函数信息。 |
| `GET` | `http://127.0.0.1:18080/context/debug` | 返回 Slate 路径、窗口和鼠标位置等调试信息。 |
| `POST` / `PUT` | `http://127.0.0.1:18080/python/execute` | 在 UE 主线程执行 Unreal Python。 |

### 执行 Python

默认推荐使用 `ExecuteFile`，适合多行 Python：

```json
{
  "command": "import unreal\nunreal.log('Hello from QuickerBridge')",
  "mode": "ExecuteFile"
}
```

`mode` 支持：

- `ExecuteFile`：执行多行脚本。
- `ExecuteStatement`：执行单条语句。
- `EvaluateStatement`：计算表达式并返回结果。

成功或失败响应结构：

```json
{
  "success": true,
  "result": "",
  "output": [
    {
      "type": "Info",
      "output": "Hello from QuickerBridge"
    }
  ]
}
```

注意：

- 请求体必须是 UTF-8 JSON。
- `command` 必须经过 JSON 序列化，不能手工拼接未转义的多行代码。
- 此接口不依赖 Remote Control API。
- HTTP API 没有鉴权，按本机自动化接口使用；不要将端口暴露到不可信网络。

## 上下文识别

### `/context/current`

返回适合 Quicker 标签匹配的纯文本。标签通常按以下顺序组成：

`区域_编辑器或图表类型_模式或选择状态_对象或节点技术类型_具体函数`

常见示例：

```text
Viewport_Level_SelectMode_StaticMesh_Lit
Viewport_Level_PlayInEditor_Lit
Viewport_Level_SelectMode_StaticMesh_Wireframe
Viewport_Asset
Graph_Material
Graph_Event_MultipleNodes
Graph_Event_CallFunction_K2_SetRelativeRotation
Graph_Event_Literal_StaticMesh
ContentBrowser
Outliner
PlaceActors
Details
SettingsEditor
Plugin_Setting
BottomCommandLine
TopBar
TopTab
```

已识别的图表类别包括：

```text
Material
Event
ConstructionScript
Function
Macro
Delegate
Blueprint
Anim
Anim_StateMachine
Anim_State
Anim_Transition
Niagara
MetaSound
Widget
BehaviorTree
Rig
```

关卡视口可附加：

- 编辑模式：`SelectMode`、`LandscapeMode`、`FoliageMode`、`MeshPaintMode`、`ModelingMode`、`FractureMode`、`AnimationMode`
- 运行状态：`PlayInEditor`、`SimulateInEditor`
- 选择状态：单个 Actor 类型或 `Multiple`
- 视口状态：`Ortho`
- 显示模式：只对 `Viewport_Level` 添加，并始终作为最后一个标签，例如 `Lit`、`Wireframe`、`Unlit`。包含下划线的 UE 模式会压缩为单个标签，例如 `Lit_Wireframe` 返回 `LitWireframe`。

### `/context/detail`

返回结构化 JSON，适合不希望解析标签字符串的调用方。主要字段：

| 字段 | 含义 |
|---|---|
| `current` | 与 `/context/current` 相同的完整上下文。 |
| `area` | `Graph`、`Viewport`、`ContentBrowser` 等区域。 |
| `editorType` | `Blueprint`、`Material`、`Level`、`Asset` 等编辑器类型。 |
| `graphType` | `Event`、`Material`、`Function` 等图表类型。 |
| `selection` | `None`、`SingleNode`、`MultipleNodes`、`SingleActor`、`MultipleActors`。 |
| `selectionObjectType` | 当前单选 Actor 类型。 |
| `nodeType` | `CallFunction`、`Literal`、`MacroInstance` 或其他节点技术类型。 |
| `nodeClass` | 节点实际 UObject 类名。 |
| `functionName` | CallFunction 节点的实际函数名。 |
| `functionOwner` | 函数所属类。 |
| `editorMode` | 当前关卡编辑模式。 |
| `playState` | `PlayInEditor` 或 `SimulateInEditor`。 |
| `viewMode` | 鼠标所在关卡视口的显示模式，例如 `Lit` 或 `Wireframe`；非关卡视口为空。 |
| `tags` | 已整理的标签数组。 |
| `mouseX` / `mouseY` | 当前屏幕鼠标坐标。 |

## Python 暴露接口

以下函数均通过 `unreal.QuickerBridgeLibrary` 调用。C++ 的 PascalCase 名称在 Python 中转换为 snake_case。

### 节点生成

| Python 函数 | 用途与关键行为 |
|---|---|
| `spawn_material_node_at_mouse(node_class, offset_x, offset_y)` | 在鼠标所在材质或材质函数图表生成 `MaterialExpression`。 |
| `spawn_blueprint_node_at_mouse(node_name, offset_x, offset_y)` | 通用蓝图动作名称生成；当前等同于 action 生成。 |
| `spawn_blueprint_action_node_at_mouse(node_name, offset_x, offset_y)` | 使用当前蓝图右键菜单动作集按名称生成节点。动作集按图表上下文缓存。 |
| `spawn_blueprint_class_node_at_mouse(node_class, offset_x, offset_y)` | 按 `UEdGraphNode` 子类生成节点。 |
| `spawn_blueprint_macro_node_at_mouse(macro_graph_path, offset_x, offset_y)` | 按宏图表路径或标准宏短名生成宏节点，例如 `ForLoop`。 |
| `spawn_blueprint_function_node_at_mouse(owner_class, function_name, offset_x, offset_y)` | 按所属类和实际函数名生成 `K2Node_CallFunction`。 |
| `redraw_graph_wires(material_expression)` | 重新同步材质图表连线并刷新图表。 |

蓝图节点生成规则：

- 只在鼠标所在 K2 图表中生成。
- 生成操作包含撤销事务。
- 当前只选中一个节点时，会尝试将该节点的第一个兼容输出连接到新节点。
- 多选时不自动连接。
- 自动连接优先执行引脚，其次普通数据引脚。
- `Timeline` / `时间轴` 由专用底层逻辑创建对应 TimelineTemplate。
- action 名称查找会构建当前蓝图图表的右键菜单动作缓存；首次调用可能比后续调用慢。

示例：

```python
import unreal

node = unreal.QuickerBridgeLibrary.spawn_blueprint_macro_node_at_mouse(
    "ForLoop", 0.0, 0.0
)
```

### 编辑器 UI 与面板

| Python 函数 | 用途 |
|---|---|
| `control_editor_panel(tab_id, command)` | 通用面板控制。`command` 支持 `Check`、`Open`、`Focus`、`Close`、`Toggle`。返回操作后的打开状态。 |
| `get_editor_ui_scale()` | 获取全局 Slate UI 缩放。 |
| `set_editor_ui_scale(scale)` | 设置全局 Slate UI 缩放，C++ 限制范围为 `0.5` 到 `3.0`。 |
| `get_graph_mouse_position(override_x, override_y)` | 将屏幕鼠标位置转换为图表坐标。 |
| `refresh_current_graph_ui(override_x, override_y)` | 刷新鼠标所在图表。 |
| `get_currently_edited_material(override_x, override_y)` | 获取鼠标所在材质图表对应的材质或材质函数对象。 |

常用面板 Tab ID 示例：

```text
OutputLog
PlacementBrowser
ContentBrowserTab1
ContentBrowserTab2
```

示例：

```python
import unreal

is_open = unreal.QuickerBridgeLibrary.control_editor_panel(
    "ContentBrowserTab2", "Toggle"
)
```

### 通用视口接口

| Python 函数 | 用途 |
|---|---|
| `get_viewport_view_mode(target)` | 获取目标编辑器视口的 `EViewModeIndex` 名称，返回值去除 `VMI_` 前缀。未找到视口时返回空字符串。 |
| `set_viewport_view_mode(view_mode, target)` | 按 `EViewModeIndex` 名称设置目标编辑器视口。接受 `Lit` 或 `VMI_Lit` 形式。 |
| `get_viewport_mouse_raycast(...)` | 从编辑器视口鼠标位置发射射线，返回命中状态、位置、法线和 Actor。 |
| `activate_editor_mode_and_tool(mode_id, sub_tool_id)` | 激活编辑器模式；子工具当前只稳定支持 `EM_ModelingToolsEditorMode`。 |

视口目标 `target`：

- `UnderMouse`：鼠标所在编辑器视口；默认值，兼容旧脚本。
- `ActiveLevel`：最近激活的关卡视口，不依赖鼠标位置，不会操作资产预览视口。

线框与光照切换应由 Python 组合，不在 C++ 中加入专用切换函数：

```python
import unreal

bridge = unreal.QuickerBridgeLibrary
current = bridge.get_viewport_view_mode("ActiveLevel")
target = "Lit" if current.lower() == "wireframe" else "Wireframe"

if current and not bridge.set_viewport_view_mode(target, "ActiveLevel"):
    unreal.log_warning("视口模式切换失败")
```

### 内容浏览器过滤器

这些接口作用于鼠标所在内容浏览器；若 Slate 路径刚被过滤器重建，会回退搜索活动窗口中的可见内容浏览器。

| Python 函数 | 用途 |
|---|---|
| `control_content_browser_asset_filter(filter_name, command)` | 精确控制资产类型过滤器。 |
| `clear_content_browser_filters()` | 清除当前内容浏览器全部过滤器。 |
| `toggle_content_browser_common_filters(content_browser_id)` | 有过滤器时全部清除；无过滤器时固定全部常用过滤器但保持未激活。参数为兼容保留，当前不用于定位。 |

`control_content_browser_asset_filter` 命令：

- `Enable`：固定并激活。
- `Disable` / `PinInactive`：固定但不激活。
- `Remove`：移除。
- `Toggle`：不存在则固定并激活，存在则切换激活状态。
- `Check`：仅查询。

返回状态：

```text
Active
Inactive
Removed
WrongThread
NoContentBrowserUnderMouse
ClassNotFound
FilterNotFound
InvalidCommand
```

示例：

```python
import unreal

result = unreal.QuickerBridgeLibrary.control_content_browser_asset_filter(
    "StaticMesh", "Toggle"
)
unreal.log(result)
```

### 通用属性接口

| Python 函数 | 用途 |
|---|---|
| `force_get_editor_property(target_object, property_name)` | 通过反射将属性导出为字符串。 |
| `force_set_editor_property(target_object, property_name, property_value)` | 通过反射导入属性字符串，调用 `Modify`、`PostEditChange`、`SaveConfig` 并刷新关卡视口。 |

此接口能力宽泛，但传入错误属性值可能造成编辑器状态异常。优先使用专用或原生公开接口。

## 插件设置

位置：`Editor Preferences > Plugins > QuickerBridge`

| 设置 | 当前代码默认值 | 行为 |
|---|---:|---|
| 启用多 UE 实例端口切换 | `false` | 多个启用插件的 UE 实例共享固定端口 `18080`，前台 UE 实例抢占端口。 |
| 启用 Scroll Lock 视口控制 | `true` | 鼠标位于当前 UE 的任意视口时关闭 Scroll Lock；位于当前 UE 其他区域时开启。 |
| Scroll Lock 保底检测间隔 | `0.5s` | 鼠标移动时立即检测；此值只用于鼠标不动或外部更改状态时的保底检测。范围 `0.01s` 到 `2.0s`。 |

注意：当前源码中的 Scroll Lock 默认保底间隔是 `0.5s`。如果预期默认值应为 `0.05s`，需要单独修改 `QuickerBridgeSettings.h` 并重新编译。

## 多 UE 实例端口协调

启用“多 UE 实例端口切换”后：

1. 所有参与切换的 UE 项目都必须安装并启用 QuickerBridge。
2. 每个进程监听同一个固定端口 `18080`，但同时只允许一个进程持有。
3. Windows 前台窗口切换到另一个 `UnrealEditor.exe` 时：
   - 原 UE 实例停止 HTTP listener 并释放互斥锁。
   - 新前台 UE 实例尝试获取互斥锁并启动 HTTP listener。
4. 切换到非 UE 程序时，不主动更换当前端口持有者。
5. 抢占失败时以 `0.05s` 间隔最多重试 10 次。

响应头中的进程 ID 和项目名可用于检查请求是否由预期项目处理。

## Scroll Lock 控制

实现由两部分组成：

- Slate 鼠标移动事件：鼠标移动时立即重新判断。
- `FTSTicker` 保底检测：处理鼠标静止或状态被外部程序修改的情况。

规则：

```text
鼠标位于当前 UE 的任意包含 Viewport 的 Slate 路径 -> Scroll Lock 关闭
鼠标位于当前 UE 的其他区域                    -> Scroll Lock 开启
鼠标位于其他进程窗口                           -> 当前 UE 不修改全局状态
```

插件关闭或 UE 退出时不会主动恢复某个固定 Scroll Lock 状态。

## 源码结构

| 文件 | 主要职责 |
|---|---|
| `Source/QuickerBridge/Private/QuickerBridge.cpp` | 模块启动/关闭、HTTP 路由、上下文识别、详细上下文。 |
| `Source/QuickerBridge/Private/QuickerBridgeContextService.*` | 即时解析鼠标 Slate 路径和当前进程窗口判断；禁止持久缓存 `FWidgetPath`。 |
| `Source/QuickerBridge/Private/QuickerBridgeLibrary.cpp` | 材质/蓝图节点生成、动作缓存、自动连接。 |
| `Source/QuickerBridge/Private/QuickerBridgeEditorUi.cpp` | 面板控制、图表坐标、材质获取、图表刷新、UI 缩放。 |
| `Source/QuickerBridge/Private/QuickerBridgeModelingViewport.cpp` | 编辑器模式/建模工具、通用视口模式、视口射线。 |
| `Source/QuickerBridge/Private/QuickerBridgeContentBrowserFilters.cpp` | 内容浏览器过滤器定位与控制。 |
| `Source/QuickerBridge/Private/QuickerBridgePropertyTools.cpp` | 通用 UObject 属性读写。 |
| `Source/QuickerBridge/Private/QuickerBridgeInstanceCoordinator.*` | Windows 多 UE 前台实例固定端口协调。 |
| `Source/QuickerBridge/Private/QuickerBridgeScrollLockController.*` | Scroll Lock 视口控制。 |
| `Source/QuickerBridge/Public/QuickerBridgeLibrary.h` | 所有 Python/蓝图公开函数声明。 |
| `Source/QuickerBridge/Public/QuickerBridgeSettings.h` | 编辑器插件设置。 |

## 编译与安装

项目插件编译命令：

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" `
  ModularHallwayEditor Win64 Development `
  -Project="D:\WorkSpace\Projects\ModularHallway\ModularHallway.uproject" `
  -WaitMutex -FromMsBuild -architecture=x64
```

成功标志：

```text
Result: Succeeded
```

安装位置二选一：

- 当前项目：`<Project>/Plugins/QuickerBridge`
- 当前引擎全部项目：`<UE_5.7>/Engine/Plugins/Marketplace/QuickerBridge`

不要让项目目录和引擎目录同时存在同名 `QuickerBridge`，否则可能出现重复插件、加载来源不明确或模块冲突。

## 修改与维护规则

修改插件时必须遵守：

1. 优先新增通用基础接口，不在 C++ 中固化单一工作流。
2. 不随意重命名或删除现有 UFUNCTION，避免已有 Python/Quicker 动作失效。
3. 不持久缓存 `FWidgetPath` 或可能随 Slate 重建失效的控件引用。
4. 修改 HTTP 路由、上下文标签、公开函数、设置、默认值或关键行为时，同步更新本文档。
5. 每次修改后至少编译 `ModularHallwayEditor Win64 Development`。
6. 如果同步到引擎插件目录，先确认不存在项目级同名插件。
7. 保持 `/context/current` 标签顺序兼容；新增详细信息优先放入 `/context/detail`。

## 已知限制与风险

- 插件依赖 UE Editor 私有或半私有 Slate/ContentBrowser 实现，升级 UE 版本后需要重新编译和验证。
- 部分接口通过鼠标所在 Slate 控件定位目标，调用时鼠标必须位于正确区域。
- 蓝图 action 名称可能受图表上下文、本地化文本和当前蓝图类型影响。
- 蓝图 action 缓存只存在内存中，并按图表、蓝图、父类和 Schema 上下文重建。
- `activate_editor_mode_and_tool` 的子工具激活当前只稳定支持建模模式。
- 内容浏览器过滤器接口会搜索可见内容浏览器；多个内容浏览器同时可见时应将鼠标放在目标浏览器中。
- 多 UE 实例端口切换仅在 Windows 上实现完整协调。
- `force_set_editor_property` 能绕过常规 UI 限制，应谨慎使用。

## 更新记录

### v1.0.0 / 2026-10-05

- 整理首次公开发布的安装说明和 UE 5.8.3 Win64 验证范围。
- 提供健康检查、设置面板端口说明、上下文探针和剪贴板 Python 快捷键。
- 详细上下文包含世界、Actor、资产、文件夹和所选图表节点；集合最多返回 16 项并提供截断标记。
- 完整编译、插件加载、HTTP 请求与 Python 执行验证通过。

### 2026-06-09

- 创建本统一入口文档。
- 新增通用视口显示模式接口：
  - `get_viewport_view_mode`
  - `set_viewport_view_mode`
- 视口模式接口通过 UE `EViewModeIndex` 枚举解析，支持框架级扩展，不包含专用“线框/光照切换”接口。
- 通用视口显示模式接口增加可选目标：`UnderMouse` 和 `ActiveLevel`；默认 `UnderMouse` 保持现有脚本兼容。
- `/context/current` 为关卡视口增加最后缀显示模式标签，例如 `Viewport_Level_SelectMode_StaticMesh_Wireframe`；资产预览视口不添加。
- `/context/detail` 增加独立 `viewMode` 字段。
- 关卡视口的 `Ortho` 和显示模式改为读取鼠标实际命中的关卡视口，并清除原来依赖活动视口的重复判断。
- 编译验证通过：`ModularHallwayEditor Win64 Development`。

### 既有关键能力

- HTTP `/context/current`、`/context/detail`、`/context/debug` 和 `/python/execute`。
- 多 UE 实例固定端口前台协调。
- Slate 鼠标事件与保底检测结合的 Scroll Lock 视口控制。
- 无持久 `FWidgetPath` 缓存的上下文解析。
- 材质节点、蓝图 action/class/macro/function 节点生成。
- 单选蓝图节点到新节点的兼容引脚自动连接。
- 内容浏览器通用过滤器、精确资产类型过滤器和清除过滤器接口。
