# Unreal Quicker Bridge v1.0.0

首次公开发布，为 Quicker 和本机自动化工具提供 Unreal Editor 上下文识别与 Python 执行接口。

## 安装包

- `Unreal-Quicker-Bridge-v1.0.0-UE5.8.3-Win64.zip`：UE 5.8.3 Windows x64 插件，包含源码和已编译模块。
- `Unreal-Quicker-Bridge-v1.0.0-Source.zip`：源码，需要对应引擎与 C++ 编译环境。

解压后将 `QuickerBridge` 放到项目的 `Plugins` 目录，启用插件并重启。不要在引擎与项目目录重复安装同名插件。

## 功能

- 本地地址 `http://127.0.0.1:18080`：健康检查、上下文识别和 Unreal Python 执行。
- 识别视口、图表、内容浏览器等区域；底部命令行返回 `BottomCommandLine`。
- 设置面板显示主要端口、接口和默认快捷键。
- `Ctrl+Alt+P` 复制上下文；`Ctrl+Alt+Shift+V` 执行剪贴板 Python。
- 节点生成、面板控制、内容浏览器过滤器、视口控制及多 UE 实例端口协调。

## 验证范围

UE 5.8.3 Win64 完整编译、DLL 加载、HTTP 健康检查、详细上下文请求和 Python 执行测试通过。界面交互及多实例切换未完成本轮逐项回归；其他平台与引擎版本需重新验证。

Python 接口没有鉴权，只用于本机可信自动化。引擎升级后需要验证编辑器私有 API 的兼容性。
