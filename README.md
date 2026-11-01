# OpenEverything v2.3.4

Everything 的开源复刻版本。

## 下载

最新版本：[OpenEverything v2.3.4](https://github.com/DisaWdcba/OpenEverything/releases/latest)

| 文件 | 说明 |
| --- | --- |
| `OpenEverything-v2.3.4-Setup-win64.exe` | 推荐。内嵌 GUI、CLI 和索引服务的单文件安装器，双击安装 |
| `OpenEverything-v2.3.4-GUI-win64.zip` | 免安装 GUI 包，解压后运行 `OpenEverything.exe` |
| `OpenEverything-v2.3.4-CLI-win64.zip` | 免安装 CLI/MCP 包，解压后运行 `OpenEverythingCLI.exe` |
| `SHA256SUMS.txt` | 发布文件的 SHA-256 校验值 |

系统要求：Windows 10/11 x64。首次启动需要管理员权限以建立 NTFS 索引；安装服务后，日常查询和 CLI/MCP 调用使用普通用户权限。

## V2.3.4 新功能

- **运行时语言切换**：视图 → 语言，默认内置简体中文和 English，可随时切换，无需重启。语言表保存在 `%LOCALAPPDATA%\OpenEverything\languages.ini`，该文件缺失或损坏时内置中英文仍然可用。
- **File 菜单**：新增“新建搜索窗口”（Ctrl+N，可在主界面多开）；选中文件后提供打开、打开路径、复制完整名称、删除、重命名、属性，并全部支持热键。
- **View 菜单**：详细信息 / 中等缩略图 / 大缩略图 / 超大缩略图（默认详细信息）；状态栏可勾选折叠（与筛选器同级）；排序方式支持字段、扩展名以及升序/降序。
- **Help → 命令行选项**：完整的 Everything 兼容参数表，并与 OpenEverything 已有的 `search`/`stats`/`update`/`mcp` 命令合并。
- **右键菜单**：不再重复系统项；“打开 / 重命名 / 删除 / 属性”由系统菜单提供（通过 `CMF_CANRENAME` 启用重命名），另外保留“打开路径”和“复制完整名称到剪贴板”。
- **匹配高亮**：结果中与输入匹配的部分以黑色加粗显示。
- **托盘**：通知区域图标，支持显示/隐藏窗口、新建搜索窗口、关闭到托盘、最小化到托盘和退出。

## 原理

OpenEverything 使用 NTFS MFT/USN Journal 建立本地文件索引，并通过 Win32 原生界面提供快速文件搜索体验。首次运行会建立索引并写入本地缓存，后续启动优先读取缓存。

## 热键

| 操作 | 热键 |
| --- | --- |
| 新建搜索窗口 | Ctrl+N |
| 打开 | Enter |
| 打开路径 | Ctrl+Enter |
| 复制完整名称 | Ctrl+Shift+C |
| 删除 | Del |
| 重命名 | F2 |
| 属性 | Alt+Enter |
| 刷新 | F5 |

## 语言文件

`%LOCALAPPDATA%\OpenEverything\languages.ini` 为可选文件，UTF-8（可带 BOM）或 UTF-16 编码：

```ini
[Languages]
en=English
zh-CN=简体中文
ja=日本語

[ja]
File=ファイル
Open=開く
New Search Window=新しい検索ウィンドウ
```

- `[Languages]` 中的 `语言ID=显示名称` 用于控制“视图 → 语言”子菜单。
- 每个非 `[Languages]` 段落定义一种语言；键是英文 UI 文本（必须与内置英文完全一致），值是该语言的翻译。
- 缺少的键会回退到英文；文件不存在时仍保留简体中文和 English。
- 修改后可用“视图 → 语言 → 重新加载语言文件”立即生效。

## 构建

需要 Windows、Visual Studio Build Tools/MSVC。

```bat
build.bat
```

构建输出：

- `build/OpenEverything.exe`：Win32 图形界面，同时支持 CLI 和 MCP 模式
- `build/OpenEverythingCLI.exe`：命令行客户端和 MCP 服务器
- `build/OpenEverythingService.exe`：后台 NTFS/USN 索引服务
- `build/OpenEverythingSetup.exe`：内嵌 GUI、CLI 和服务的单文件安装器

附加参数：`--debug`、`--release`、`--clean` 和 `--tests`。

## CLI

`OpenEverything.exe` 和 `OpenEverythingCLI.exe` 提供相同的 CLI/MCP 命令。前者无参数启动时进入 GUI，后者保留为独立的纯命令行版本。命令模式会优先连接 `OpenEverythingService` 并读取服务维护的 `%ProgramData%\OpenEverything\index.dat`；服务不可用或首次建库尚未完成时，自动回退到 `%LOCALAPPDATA%\OpenEverything\index.dat`。

基本语法：

```text
OpenEverythingCLI.exe search [QUERY] [options]
OpenEverythingCLI.exe stats [--json] [--index FILE]
OpenEverythingCLI.exe update [--timeout SECONDS] [--json]
OpenEverythingCLI.exe mcp [--index FILE]
```

常用查询语句：

```powershell
# 查找所有 PDF，最多返回 20 条
build\OpenEverythingCLI.exe search "*.pdf" --limit 20

# 精确查找中文文件名
build\OpenEverythingCLI.exe search "7.10通知.pdf" --limit 5

# 使用 ext: 查询一个或多个扩展名
build\OpenEverythingCLI.exe search "ext:pdf"
build\OpenEverythingCLI.exe search "ext:pdf;docx;xlsx" --limit 100

# 限定目录，不搜索子目录
build\OpenEverythingCLI.exe search "*.log" --folder "C:\Logs" --no-subfolders

# 匹配完整路径，仅返回可执行文件，并输出 JSON
build\OpenEverythingCLI.exe search "OpenEverything" --match-path --filter executable --json

# 按修改时间倒序
build\OpenEverythingCLI.exe search "*.pdf" --sort modified --descending --limit 50

# 查看当前索引状态
build\OpenEverythingCLI.exe stats --json

# 请求服务同步 USN，并等待索引更新完成
build\OpenEverythingCLI.exe update --json --timeout 60

# GUI 可执行文件也支持相同命令
build\OpenEverything.exe search "*.pdf" --limit 20
```

查询支持 `*`、`?` 通配符和 `ext:EXT`/`ext:EXT1;EXT2` 扩展名语法。`--filter` 可选 `everything`、`audio`、`compressed`、`document`、`executable`、`folder`、`image`、`video`；`--sort` 可选 `name`、`path`、`size`、`modified`、`created`、`attributes`。

运行 `build\OpenEverything.exe --help` 或 `build\OpenEverythingCLI.exe --help` 查看全部参数。使用 `--index FILE` 可以读取指定的 `index.dat`。GUI 模式仍按需请求管理员权限，CLI/MCP 模式不会因为使用 GUI 可执行文件而触发 UAC。交互式 CMD/PowerShell 推荐使用独立的 `OpenEverythingCLI.exe`。

## MCP

MCP 服务器使用标准输入输出传输，支持逐行 JSON-RPC 和 `Content-Length` 帧。GUI 与 CLI 可执行文件都可以作为 MCP 服务器。

使用 GUI 版本的配置：

```json
{
  "mcpServers": {
    "open-everything": {
      "command": "C:\\absolute\\path\\build\\OpenEverything.exe",
      "args": ["mcp"]
    }
  }
}
```

使用独立 CLI 版本的配置：

```json
{
  "mcpServers": {
    "open-everything": {
      "command": "C:\\absolute\\path\\build\\OpenEverythingCLI.exe",
      "args": ["mcp"]
    }
  }
}
```

安装服务后，可将 `command` 改为 `C:\\Program Files\\OpenEverything\\OpenEverythingCLI.exe`。两种入口暴露相同的 MCP 工具和协议行为。

服务器提供三个工具：

- `search_files`：按名称或完整路径搜索；支持 `query`、`limit`、`folder`、`filter`、`sort_by`、`descending`、`match_path`、`case_sensitive`、`whole_word` 和 `include_subfolders`
- `get_index_stats`：读取索引状态、条目数、卷数量和缓存路径
- `reload_index`：请求后台服务同步 USN，等待完成后重新加载 `index.dat`

`search_files` 参数示例：

```json
{
  "query": "*.pdf",
  "folder": "C:\\Users\\Disa\\Documents",
  "include_subfolders": true,
  "filter": "document",
  "sort_by": "modified",
  "descending": true,
  "limit": 20
}
```

可以直接向 Agent 提出类似请求：`查找 Documents 下最近修改的 20 个 PDF`、`精确查找 7.10通知.pdf`、`刷新索引后查找所有 OpenEverything*.exe`。

指定独立索引时将参数写为 `"args": ["mcp", "--index", "C:\\path\\index.dat"]`。

## 索引服务

安装服务需要一次管理员确认。`OpenEverythingSetup.exe` 内嵌 GUI、CLI 和服务三个程序，单独分发这一个文件即可完成安装。它会将内嵌程序写入 `%ProgramFiles%\OpenEverything`，创建 `%ProgramData%\OpenEverything` 的只读用户缓存目录，注册为延迟自动启动的 `LocalSystem` 服务并立即启动：

普通用户直接双击 `OpenEverythingSetup.exe` 即可使用中文图形安装界面。安装器会检测现有安装，提供安装、更新/修复和卸载操作；安装完成后创建开始菜单入口，并可选择创建桌面快捷方式。

```powershell
build\OpenEverythingSetup.exe install
```

安装完成后，普通用户 CMD 和 Agent 可以直接运行：

```powershell
& "C:\Program Files\OpenEverything\OpenEverythingCLI.exe" stats
& "C:\Program Files\OpenEverything\OpenEverythingCLI.exe" search "*.pdf" --limit 20
& "C:\Program Files\OpenEverything\OpenEverythingCLI.exe" update
```

管理命令：

```powershell
build\OpenEverythingSetup.exe start
build\OpenEverythingSetup.exe stop
build\OpenEverythingSetup.exe uninstall
build\OpenEverythingCLI.exe stats --json
```

`OpenEverythingSetup.exe` 通过嵌入式清单请求 UAC。双击运行时显示图形安装界面；显式传入 `install`、`update`、`start`、`stop` 或 `uninstall` 时保持静默，不显示管理员控制台。`OpenEverythingService.exe` 只由 Windows 服务控制管理器启动，不再承担提权、复制或注册服务的工作。查询、`stats`、`update` 和 MCP 调用保持普通用户权限。首次全量建库期间 `stats` 会显示 `building`，CLI 会在服务缓存可用前继续使用原有用户缓存。

## GitHub

- 仓库：https://github.com/DisaWdcba/OpenEverything
- 发布：https://github.com/DisaWdcba/OpenEverything/releases
- 当前版本：v2.3.4
