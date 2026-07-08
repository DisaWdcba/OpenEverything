# OpenEverything

Everything 的开源复刻版本。

## 原理

OpenEverything 使用 NTFS MFT/USN Journal 建立本地文件索引，并通过 Win32 原生界面提供快速文件搜索体验。首次运行会建立索引并写入本地缓存，后续启动优先读取缓存。

## 构建

需要 Windows、Visual Studio Build Tools/MSVC。

```bat
build.bat
```

输出文件位于 `build/OpenEverything.exe`。

## GitHub

https://github.com/DisaWdcba
