# GitHub Actions 编译 Windows exe 方法

这套方法适合你在 macOS 上开发，但需要产出 Windows 10 可运行的 `.exe`。

## 结论

- 可以用 PyInstaller 生成 `trans_type_py.exe`。
- PyInstaller 必须运行在 Windows 环境里才能生成 Windows `.exe`。
- macOS 本机运行 PyInstaller 只能生成 macOS 程序，不能可靠生成 Windows exe。
- 本项目已经提供 GitHub Actions，会在 GitHub 的 Windows 机器上自动编译。

## 产物

GitHub Actions 会生成一个名为 `trans_type-windows` 的 artifact，里面包含：

- `trans_type.exe`：原生 WinAPI C 版本，体积最小。
- `trans_type_cpp.exe`：C++ 包装版本，行为和原生版一致。
- `trans_type_py.exe`：Python + PyInstaller 版本，体积较大，但源码更容易改。
- `README.md`、`trans.txt`、源码和构建脚本。

## 方法一：网页上传，最简单

1. 登录 GitHub。
2. 新建一个 repository，例如 `trans-type-tool`。
3. 把本目录所有文件上传到仓库根目录。
4. 确认仓库里存在这个文件：

```text
.github/workflows/build-windows.yml
```

5. 打开仓库页面的 **Actions** 标签。
6. 选择 **Build Windows exes**。
7. 点击 **Run workflow**。
8. 等待构建完成。
9. 打开完成的 workflow run，在页面底部下载 artifact：`trans_type-windows`。
10. 解压后即可得到三个 Windows exe。

## 方法二：命令行推送

在 macOS 终端里进入本项目目录：

```sh
cd /Users/emvdy/Documents/verisilicon/tools
git init
git add .
git commit -m "Add trans type Windows tool"
git branch -M main
git remote add origin https://github.com/YOUR_NAME/trans-type-tool.git
git push -u origin main
```

然后在 GitHub 网页里进入仓库：

```text
Actions -> Build Windows exes -> Run workflow
```

构建完成后下载 `trans_type-windows` artifact。

## GitHub Actions 实际做了什么

当前 workflow 文件是：

```text
.github/workflows/build-windows.yml
```

它会在 `windows-latest` 上执行：

```bat
python -m py_compile trans_type.py
python trans_type.py --dry-run --ascii-only
build.bat
build_cpp.bat
build_python.bat
trans_type.exe --dry-run --ascii-only
trans_type_cpp.exe --dry-run --ascii-only
trans_type_py.exe --dry-run --ascii-only
```

其中 `build_python.bat` 会执行 PyInstaller：

```bat
python -m PyInstaller --clean --onefile --console --name trans_type_py trans_type.py
```

最终把生成的 exe 上传为 artifact。

## 只在 Windows 本机打包 Python exe

如果你手头有 Windows 10 机器，也可以不走 GitHub。

安装 Python 3.10+ 后，在项目目录运行：

```bat
build_python.bat
```

或者手动执行：

```bat
python -m pip install --upgrade -r requirements-build.txt
python -m PyInstaller --clean --onefile --console --name trans_type_py trans_type.py
copy /Y dist\trans_type_py.exe trans_type_py.exe
```

生成结果：

```text
trans_type_py.exe
```

使用时把 `trans_type_py.exe` 和 `trans.txt` 放在同一个目录。

## 常见问题

如果 Actions 没有出现：

- 确认 `.github/workflows/build-windows.yml` 路径正确。
- 确认文件已经 push 到 GitHub。
- 打开仓库的 **Actions**，如果 GitHub 提示启用 Actions，点击启用。

如果下载不到 artifact：

- 点进具体的 workflow run。
- 拉到页面底部找 **Artifacts** 区域。
- 下载 `trans_type-windows`。

如果 Windows 运行 exe 被拦截：

- 这是自编译 exe 常见情况。
- 右键 exe -> Properties -> 勾选 Unblock，如果有该选项。
- 或在可信目录中运行。

如果目标窗口没有输入：

- 倒计时结束前必须让 RDP 目标窗口获得焦点。
- 如果目标程序是管理员权限运行，工具也可能需要用管理员权限运行。
- 如果你不确定是不是权限、RDP 会话、输入法或 Unicode 问题，先用 Python 版跑诊断：

```bat
trans_type_py.exe --self-test
trans_type_py.exe --diagnose
trans_type_py.exe --debug-input
```

`--debug-input` 会让你把光标放到一个安全输入框，然后测试 ASCII virtual-key、Unicode ASCII 和 Unicode 中文输入。建议先对本地 Notepad 测，再对 RDP 窗口测；如果 Notepad 成功但 RDP 失败，问题就在 RDP 会话或目标窗口。

- `--ascii-only` 只是检查 `trans.txt` 是否全是 ASCII，不会改变输入方式。
- `--ascii-keys` 才会改成虚拟按键输入，只适合 ASCII 文本。
- 高延迟环境使用更慢参数：

```bat
trans_type_py.exe --delay-ms 80 --line-delay-ms 500
```
