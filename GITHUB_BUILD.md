# 使用 GitHub Actions 编译 Windows 10 x64 程序

这套流程适合在 macOS 上维护源码，通过 GitHub 的 Windows 运行器生成三个
Windows `.exe`。

## 结论

- macOS 上的 PyInstaller 不能生成受支持的 Windows `.exe`。
- Windows exe 必须在 Windows 环境中打包。
- 本仓库使用 `windows-2022` x64 运行器、MSVC、Python 3.12 和 PyInstaller。
- 运行器不是 Windows 10，但编译目标设置为 Windows 10，产物仍需在实际
  Windows 10 + RDP 环境做最后一次人工冒烟测试。

## Actions 产物

`Build Windows exes` 会上传 `trans_type-windows` artifact，包含：

- `trans_type.exe`：原生 Win32 C 版本，体积最小。
- `trans_type_cpp.exe`：复用同一实现的 C++ 版本。
- `trans_type_py.exe`：Python + PyInstaller 单文件版本，体积较大。
- README、构建脚本、源码和测试。

artifact 默认保留 14 天，不是永久 Release。

macOS workflow 还会上传 `trans-type-macos-universal`，其中的二进制同时包含
`arm64` 和 `x86_64`，最低目标是 macOS 11。

## 推送仓库

当前仓库建议使用 SSH，不要再使用 GitHub 账号密码。GitHub 已停止支持
Git HTTPS 密码认证。

检查远程地址：

```sh
git remote -v
```

SSH 远程示例：

```sh
git remote set-url origin ssh://git@ssh.github.com:443/OWNER/trans-type-tool.git
git ls-remote origin HEAD
git push origin main
```

浏览器登录 GitHub 与 `gh` 命令行登录是两套状态。如果出现 token 失效：

```sh
gh auth status
gh auth login -h github.com
```

按提示选择 GitHub.com、HTTPS 或 SSH 均可；Git 推送已经使用 SSH 时，`gh`
只用于启动、查看和下载 Actions。不要把 token 写入仓库或聊天记录。

## 从网页运行

1. 打开仓库的 **Actions** 页面。
2. 选择 **Build Windows exes**。
3. 点击 **Run workflow**，选择 `main`。
4. 等待所有步骤变绿。
5. 打开本次 run，在 **Artifacts** 下载 `trans_type-windows`。

workflow 在每次 push 时也会自动运行。

## 从命令行运行

`gh` 登录有效后：

```sh
gh workflow run "Build Windows exes" --ref main
gh run list --workflow build-windows.yml --limit 5
gh run watch --exit-status
```

下载指定 run 的产物：

```sh
gh run download RUN_ID -n trans_type-windows -D artifacts/windows
```

## Windows workflow 实际检查

workflow 文件：

```text
.github/workflows/build-windows.yml
```

它会执行：

1. Python 语法检查和协议单元测试。
2. Python 源码三种模式的 dry-run。
3. MSVC x64 编译原生 C 和 C++ 版本，静态链接 C 运行库。
4. 使用锁定版本的 PyInstaller 构建 Python 单文件 exe。
5. 检查三个文件的 PE machine 都是 x64 (`0x8664`)。
6. 确认三个 exe 的 simple 模式拒绝大写、`@` 和 Unicode。
7. 让三个 exe 分别导出 `cmd-hex`、`zip-hex` 命令流。
8. 检查命令流只有小写、数字和无需 Shift 的白名单字符。
9. 在 Windows PowerShell 5.1 中真实执行命令流，并使用 `certutil` 解码。
10. 对 UTF-8、UTF-8 BOM、preserve 三种输出逐字节比对。
11. 确认恢复用 `.hex`/`.zip` 临时文件仍然存在。

因此，Actions 不只是检查“程序能启动”，还会验证复杂协议确实恢复出相同字节。

## 在 Windows 本机打包 Python exe

安装 64 位 Python 3.10 或更高版本，在仓库目录运行：

```bat
build_python.bat
```

脚本检查 `requirements-build.txt` 中锁定的版本，必要时安装，然后执行：

```bat
python -m PyInstaller --clean --onefile --console --name trans_type_py trans_type.py
```

生成文件：

```text
trans_type_py.exe
```

PyInstaller 会内嵌 Python，因此文件明显大于 C 版本。某些杀毒软件会对未知的
PyInstaller 单文件程序产生误报。项目没有使用 UPX、代码混淆、隐藏执行或绕过
安全软件。内部发布时应保留源码、记录 SHA-256，并优先使用组织代码签名证书。

## 下载后的校验

Windows PowerShell：

```powershell
get-filehash .\trans_type.exe -algorithm sha256
get-filehash .\trans_type_cpp.exe -algorithm sha256
get-filehash .\trans_type_py.exe -algorithm sha256
```

把哈希记录到内部发布单或变更记录。GitHub artifact 本身不会自动完成
Authenticode 签名。

## Windows 10 最终测试清单

在正式 RDP 目标前，先对本地记事本和临时目录测试：

```bat
trans_type.exe --mode simple --dry-run
trans_type.exe --mode cmd-hex --dry-run --commands-out commands.txt
trans_type.exe --mode zip-hex --dry-run --commands-out commands.txt
trans_type.exe --self-test
trans_type_py.exe --diagnose
trans_type_py.exe --debug-input
```

然后按顺序验证：

1. 关闭 Caps Lock，释放 Shift/Ctrl/Alt。
2. 本地和 RDP 都切换到英文输入法，并使用相同的 US 类键盘布局。
3. simple 只测试允许的小写、数字和无修饰符字符。
4. 在远端临时目录打开普通权限的 `cmd.exe` 或 PowerShell。
5. 用短文本测试 `cmd-hex`，比较程序显示的 SHA-256 与远端 `certutil` 输出。
6. 用可压缩的长文本测试 `zip-hex`，再次比较 SHA-256。
7. 确认输出无误后，再人工删除远端 `tt.hex` 和 `tt.zip`。

正常 medium integrity 的 RDP 不要求管理员权限。如果目标窗口本身以管理员身份
运行，Windows 可能阻止低完整性进程发送输入。应把目标窗口重新以普通权限打开，
而不是绕过完整性边界。

## 企业策略和报错

`cmd-hex`/`zip-hex` 会显式调用 PowerShell、`certutil` 和 `Expand-Archive`。
AppLocker、WDAC、EDR 或组织策略可能阻止或告警。这种情况下应联系系统所有者做
批准或白名单，不应关闭或规避安全控制。

如果 Actions 失败，先打开失败步骤读取真实编译器/测试输出，再针对该错误修改：

- MSVC 失败：查看 `Build native WinAPI exe` 或 `Build C++ wrapper exe`。
- PyInstaller 失败：查看 `Build Python exe` 和依赖安装输出。
- 协议失败：查看 `Execute emitted protocols with Windows PowerShell 5.1`。
- 没有 Actions：确认 workflow 已 push，并在仓库设置中启用 Actions。
- 下载不到 artifact：确认 run 成功且登录账号有仓库访问权限。

不要根据猜测修改键盘逻辑；保留失败日志、程序版本、Windows 版本、输入法、键盘
布局和 RDP 客户端信息，才能重现问题。
