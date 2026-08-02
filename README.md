# OpenEverything v0.2.3 SP1

Everything 的开源复刻版本。

## 原理

OpenEverything 使用 NTFS MFT/USN Journal 建立本地文件索引，并通过 Win32 原生界面提供快速文件搜索体验。首次运行会建立索引并写入本地缓存，后续启动优先读取缓存。

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

https://github.com/DisaWdcba
