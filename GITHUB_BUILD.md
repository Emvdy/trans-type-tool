# 使用 GitHub Actions 编译 Windows x86/x64 程序

这套流程适合在 macOS 上维护源码，通过 GitHub 的 Windows 运行器分别生成 x86
和 x64 原生 Windows `.exe`。

从注册 Windows 10 runner 到下载并实际使用 exe 的完整中文步骤见
`WINDOWS10_ACTIONS.md`。

## 结论

- macOS 上的 PyInstaller 不能生成受支持的 Windows `.exe`。
- Windows exe 必须在 Windows 环境中打包。
- GitHub 没有 `windows-10` 官方托管标签，不能靠修改 `runs-on` 获得真实
  Windows 10。
- 本仓库使用 `windows-2022`、MSVC x86/x64 交叉编译环境和 Python 3.12 测试。
- 手动运行 workflow 时，默认继续在带 `windows-10` 标签的真实 self-hosted
  Windows 10 x64 runner 上重复完整测试。
- Actions 不会自动操作交互式 RDP 窗口，最终按键冒烟测试仍需人在场完成。

## Actions 产物

`Build Windows executables and validate Windows 10` 会上传两个 artifact：

- `qr-transfer-windows-x86`：只包含 x86 `trans_type.exe`。
- `qr-transfer-windows-x64`：只包含 x64 `trans_type.exe`。

每个 artifact 的压缩包内只有一个可执行文件，默认保留 14 天，不是永久 Release。

macOS workflow 还会上传 `qr-transfer-macos`，只包含 `trans_type_mac`。该二进制
同时包含 `arm64` 和 `x86_64`，最低目标是 macOS 11。

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
2. 选择 **Build Windows executables and validate Windows 10**。
3. 点击 **Run workflow**，选择 `main`。
4. 已注册 Windows 10 runner 时，保持 Windows 10 验证选项为选中。
5. 只想构建时取消该选项，否则没有在线 runner 会一直 queued。
6. 等待所有实际运行的步骤变绿。
7. 打开本次 run，在 **Artifacts** 下载 `qr-transfer-windows-x86` 或
   `qr-transfer-windows-x64`。

每次 push 到 `main` 都会自动运行托管构建；Windows 10 self-hosted job 只在手动
workflow run 且勾选验证时运行。

## 从命令行运行

`gh` 登录有效后：

```sh
gh workflow run build-windows.yml --ref main -f validate_windows_10=true
gh run list --workflow build-windows.yml --limit 5
gh run watch --exit-status
```

仅构建、不等待 Windows 10 runner：

```sh
gh workflow run build-windows.yml --ref main -f validate_windows_10=false
```

下载指定 run 的产物：

```sh
gh run download RUN_ID -n qr-transfer-windows-x86 -D artifacts/windows-x86
gh run download RUN_ID -n qr-transfer-windows-x64 -D artifacts/windows-x64
```

## Windows workflow 实际检查

workflow 文件：

```text
.github/workflows/build-windows.yml
```

托管的 `Build Windows x86 executable` 和 `Build Windows x64 executable` 矩阵任务会执行：

1. Python 语法检查和协议单元测试。
2. Python 源码三种模式的 dry-run。
3. MSVC 分别以 x86 和 x64 环境编译原生 C 版本，静态链接 C 运行库。
4. 读取 PE 头，确认 x86 machine 为 `0x014c`、x64 machine 为 `0x8664`。
5. 确认对应 exe 的 simple 模式拒绝大写、`@` 和 Unicode。
6. 导出 `cmd-hex`、`zip-hex` 命令流并检查无需 Shift 的字符白名单。
7. 在 Windows PowerShell 5.1 中真实执行命令流，并使用 `certutil` 解码。
8. 对 UTF-8、UTF-8 BOM、preserve 三种输出逐字节比对。
9. 确认目标路径 helper 和 `tt.*` 临时文件清理行为。

因此，Actions 不只是检查“程序能启动”，还会验证复杂协议确实恢复出相同字节。

手动勾选 Windows 10 验证后，`Validate on real Windows 10 x64` job 会：

1. 等待 `[self-hosted, windows, x64, windows-10]` runner。
2. 从操作系统读取 ProductType、版本和 build，拒绝 Windows Server/Windows 11。
3. 检查 64 位系统、Windows PowerShell 5.1、`certutil` 和 `Expand-Archive`。
4. 下载同一次 workflow 生成的 `qr-transfer-windows-x64` artifact。
5. 在真实 Windows 10 上再次执行 x64 exe 的 simple、PE、压缩、编码和协议回读。

注册 self-hosted runner 的完整步骤见 `WINDOWS10_ACTIONS.md`。不要把公开仓库的
self-hosted runner 安装在生产服务器上，也不要让它保存生产凭据。

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
6. 用含 NUL/非文本字节的文件测试 `--output-encoding preserve`。
7. 用可压缩的长文本测试 `zip-hex`，再次比较 SHA-256。
8. 用临时目录测试 `zip-hex --source <目录>`，核对默认 basename 目标目录、
   `tt.zip` 的 SHA-256、嵌套文件、Unicode 文件名和空目录。
9. 测试 `--remote-output ../`，确认自动追加来源 basename；再测试
   `--remote-output "C:/Drop Folder/output.bin"`，确认复杂路径通过 helper 正确恢复。
10. 确认完成后远端没有残留 `tt.hex`、`tt.zip`、`tt.out`、`tt.cmd.hex` 或
    `tt.cmd`；中止时则在重试前清理残留。
11. Windows 测试 Esc 中止且普通 Space 不触发暂停；macOS 测试 Esc 行尾暂停、
    暂停后 Space 继续和第二次 Esc 结束的状态机。

正常 medium integrity 的 RDP 不要求管理员权限。如果目标窗口本身以管理员身份
运行，Windows 可能阻止低完整性进程发送输入。应把目标窗口重新以普通权限打开，
而不是绕过完整性边界。

## 企业策略和报错

`cmd-hex`/`zip-hex` 会显式调用 PowerShell、`certutil` 和 `Expand-Archive`。
AppLocker、WDAC、EDR 或组织策略可能阻止或告警。这种情况下应联系系统所有者做
批准或白名单，不应关闭或规避安全控制。

如果 Actions 失败，先打开失败步骤读取真实编译器/测试输出，再针对该错误修改：

- MSVC 失败：查看对应 x86/x64 job 的 `Build native WinAPI exe`。
- 架构失败：查看 `Verify PE architecture`，不要把 x86 和 x64 artifact 混用。
- 协议失败：查看 `Execute emitted protocols with Windows PowerShell 5.1`。
- Windows 10 job 一直 queued：确认 runner 正在运行并带有 `windows-10` 标签。
- Windows 10 系统断言失败：不要修改断言伪装版本，检查 runner 是否实际为
  Windows 10 client x64。
- 没有 Actions：确认 workflow 已 push，并在仓库设置中启用 Actions。
- 下载不到 artifact：确认 run 成功且登录账号有仓库访问权限。

不要根据猜测修改键盘逻辑；保留失败日志、程序版本、Windows 版本、输入法、键盘
布局和 RDP 客户端信息，才能重现问题。
