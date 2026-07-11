# 在真实 Windows 10 上运行 GitHub Actions

## 最短使用流程

只需要编译并下载 Windows exe，不运行真实 Windows 10 测试：

```sh
gh workflow run build-windows.yml --ref main -f validate_windows_10=false
gh run watch --exit-status
```

已经准备好 Windows 10 self-hosted runner：

```sh
gh workflow run build-windows.yml --ref main -f validate_windows_10=true
gh run watch --exit-status
```

运行完成后下载 artifact：

```sh
gh run list --workflow build-windows.yml --limit 5
gh run download run-id -n trans_type-windows -D artifacts/windows
```

把 `run-id` 替换为第一条命令显示的数字。网页也可以在对应 run 页面底部下载
`trans_type-windows`。

## 为什么必须使用 self-hosted runner

GitHub 官方托管 runner 当前提供 `windows-2022`、`windows-2025`、
`windows-latest` 和 `windows-11-arm`，没有 `windows-10` 标签。

因此，把 workflow 写成 `runs-on: windows-10` 只会一直等待一个不存在的托管
runner。真正验证 Windows 10 的可靠方式是注册一台真实 Windows 10 x64 电脑作为
self-hosted runner。

本仓库采用两阶段流程：

1. `windows-2022` 托管 runner 负责安装编译器、构建和基础测试。
2. 手动触发时，把同一 run 的 artifact 下载到真实 Windows 10 x64 runner，再做
   操作系统断言和完整协议回归。

这避免让日常 push 因本地 Windows 10 电脑关机而一直排队。

## 准备 Windows 10 runner

使用一台专门的 Windows 10 x64 测试机，不要直接把离线生产服务器注册到公开
GitHub 仓库，也不要在 runner 上保存生产凭据。

官方参考：

- [GitHub-hosted runners reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
- [Adding self-hosted runners](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/add-runners)

1. 打开仓库页面。
2. 进入 **Settings > Actions > Runners**。
3. 点击 **New self-hosted runner**。
4. 选择 **Windows** 和 **x64**。
5. 在 Windows 10 PowerShell 中执行页面生成的下载和解压命令。
6. 配置 runner 时，在页面给出的 `config.cmd` 命令末尾增加标签：

```powershell
.\config.cmd --url https://github.com/Emvdy/trans-type-tool --token one-time-token --labels windows-10
```

页面生成的 token 是短期一次性 token，不要写入仓库或文档。配置完成后，GitHub
页面应显示这些标签：

```text
self-hosted
windows
x64
windows-10
```

前台启动 runner：

```powershell
.\run.cmd
```

runner 显示 `Listening for Jobs` 后保持窗口运行。

## 触发 Windows 10 验证

网页：

1. 打开 **Actions**。
2. 选择 **Build Windows exes and validate Windows 10**。
3. 点击 **Run workflow**。
4. 保持 `Run compatibility tests on a self-hosted Windows 10 x64 runner` 为选中。
5. 选择 `main` 并运行。

出于 self-hosted runner 安全考虑，Windows 10 job 只接受 `main` 分支；即使从其他
分支手动启动 workflow，该 job 也会跳过。

命令行：

```sh
gh workflow run build-windows.yml --ref main -f validate_windows_10=true
gh run watch --exit-status
```

workflow 应包含两个 job：

- `Build Windows x64 artifacts`
- `Validate on real Windows 10 x64`

如果第二个 job 一直显示 queued，说明 runner 没有在线，或者缺少
`windows-10` 自定义标签。

只需要重新编译、不运行 Windows 10 job 时：

```sh
gh workflow run build-windows.yml --ref main -f validate_windows_10=false
```

## Windows 10 job 会验证什么

job 不相信 runner 名称，而是读取操作系统本身并检查：

- `ProductType == 1`，排除 Windows Server。
- NT 版本为 `10.0`。
- build 在 `10240` 到 `21999`，排除 Windows 11。
- 操作系统为 64 位。
- shell 是 Windows PowerShell 5.1 Desktop。
- `certutil.exe` 和 `Expand-Archive` 存在。

随后它会下载本次构建的三个 exe，并再次执行：

- Python 协议单元测试。
- 三个 exe 的 strict simple dry-run。
- PE x64 检查。
- simple 对大写、`@` 和 Unicode 的拒绝检查。
- `cmd-hex` 和 `zip-hex` 的 PowerShell 5.1/certutil 真实回读。
- 小写相对路径直传，以及含空格/大写路径的 hex helper 实际重建。
- UTF-8、UTF-8 BOM、preserve 三种输出逐字节对比。
- ZIP 压缩率、全部固定中间文件和自动清理检查。

## 下载后如何使用 exe

artifact 中有三个 Windows x64 程序：

- `trans_type.exe`：原生 C 版本，约 233 KB，日常使用优先选择。
- `trans_type_cpp.exe`：与 C 版本行为相同。
- `trans_type_py.exe`：Python/PyInstaller 版本，体积较大，便于继续调试。

默认读取 exe 同目录下的 `trans.txt`。先在本地 PowerShell 中进入解压目录并做
dry-run：

```powershell
.\trans_type.exe --mode simple --dry-run
```

### simple 模式

只适合小写字母、数字和无需 Shift 的基础字符。启动后，在倒计时内切换焦点到 RDP
目标编辑框：

```powershell
.\trans_type.exe --mode simple
```

指定文本文件或读取本地 Windows 剪贴板。`--source` 可以直接接路径；`--file`
已经删除：

```powershell
.\trans_type.exe --mode simple --source c:\work\input.txt
.\trans_type.exe --mode simple --source clipboard
```

simple 会拒绝大写、Unicode、`@#$` 和其他需要 Shift 的字符。遇到这些内容不要
修改键盘模拟逻辑，直接使用复杂模式。

### cmd-hex 模式

适合 `@#$`、脚本、中文和其他 Unicode。远端先打开普通权限的 `cmd.exe` 或
PowerShell，然后在倒计时内把焦点切到该远端窗口：

```powershell
.\trans_type.exe --mode cmd-hex --source c:\work\input.txt
```

它会输入全小写、无需 Shift 的 PowerShell/certutil 命令。路径来源默认使用本地
basename，因此上例在远端当前目录创建 `input.txt`，但不会执行该文件。

### zip-hex 模式

适合较长且重复内容较多的文件，也是在目录来源下唯一允许的模式：

```powershell
.\trans_type.exe --mode zip-hex --source c:\work\input.txt
```

小文件或已经压缩过的内容不一定更快，先用 `--dry-run` 比较输出大小。

逐字节复制任意单个文件时使用 `preserve`；目录会自动逐字节打包，不需要编码选项：

```powershell
.\trans_type.exe --mode cmd-hex --source c:\work\tool.exe --output-encoding preserve
.\trans_type.exe --mode zip-hex --source c:\work\data
```

目录来源只允许 `zip-hex`。默认远端目标目录名是本地目录 basename；解压会覆盖/合并同名文件，
不会删除目标目录中无关的旧文件。程序拒绝目录中的符号链接、junction、reparse
point 和特殊文件，默认统计所有普通文件后仍受 `--max-bytes` 限制。目录成员名在
ZIP 元数据中传输，因此成员名可以有大写或 Unicode，不会直接出现在模拟键盘命令中。

### 远端输出名称和目录

`--remote-output TARGET` 现在同时表示远端名称和路径，旧的独立路径参数已删除。
剪贴板和 `--source file` 默认目标是 `.\trans.txt`；`--source <路径>` 默认目标是
`.\<本地 basename>`。`TARGET` 可以是新名称、相对路径、当前盘根路径、带盘符的
绝对路径或 UNC 路径：

```powershell
.\trans_type.exe --mode cmd-hex --source c:\work\input.txt --remote-output renamed.txt
.\trans_type.exe --mode cmd-hex --source c:\work\input.txt --remote-output ../
.\trans_type.exe --mode cmd-hex --source c:\work\input.txt --remote-output ../renamed.txt
.\trans_type.exe --mode zip-hex --source c:\work\tool.exe --output-encoding preserve --remote-output 'c:/Drop Folder/'
```

`/` 和 `\` 都会统一为 Windows 分隔符。参数以分隔符结尾，或者参数正好是 `.`/
`..` 时，程序把它当作目录并自动追加本地来源 basename。例如 `--remote-output ../`
最终得到 `..\input.txt`，`--remote-output c:/drop/` 最终得到
`c:\drop\input.txt`。缺少的父目录会自动创建。

全小写且不需要 Shift 的目标使用较短的直接协议。包含大写、空格、下划线、`:`、
Unicode 或其他需要修饰键字符的目标会自动改用 hex 编码的内部 `tt.cmd` helper；
真实路径不会直接出现在模拟按键流中，外层仍然只有小写和无需 Shift 的字符。helper
会增加少量打字量，因此目标位置可调整时，小写相对路径更快。非法 Windows 路径、
保留设备名、结尾点/空格和固定临时文件名会在倒计时前被拒绝。

`--remote-output` 只用于 `cmd-hex`/`zip-hex`。`simple` 只是直接输入文本，不创建
远端文件，因此显式使用该参数会报错。

### 调整速度和编码

高延迟 RDP 可以放慢：

```powershell
.\trans_type.exe --mode cmd-hex --delay-ms 50 --line-delay-ms 300 --start-delay-sec 10
```

复杂模式默认生成无 BOM UTF-8，也可以选择：

```powershell
.\trans_type.exe --mode cmd-hex --output-encoding utf8-bom
.\trans_type.exe --mode zip-hex --source c:\work\input.bin --output-encoding preserve --remote-output input.bin
```

`preserve` 只支持 `cmd-hex`/`zip-hex` 的普通文件输入，并且跳过文本解码，支持任意
二进制字节。剪贴板仍然是文本来源。

### 完成后核对

普通文件的复杂模式启动前会显示输出文件的本地预期 SHA-256，远端完成后
`certutil` 也会显示 SHA-256。目录模式显示并核对 `tt.zip` 的 SHA-256，解压时 ZIP
CRC 会继续校验每个成员。生成命令会在重建/解压后自动删除远端 `tt.hex` 和
`tt.zip`。复杂目标路径还会使用并删除 `tt.out`（仅 `cmd-hex`）、`tt.cmd.hex` 和
`tt.cmd`。如果在清理命令输入前按 Esc 中止，重试前人工删除残留的 `tt.*` 中间文件。

输入期间按 Esc 中止；短按 Space 暂停，再按一次继续。物理 Space 会先到达目标，
程序在松键后发送一次 Backspace 清除它，因此不要长按 Space，以免键盘自动重复。

运行前始终关闭 Caps Lock、释放 Shift/Ctrl/Alt，并把本地和 RDP 输入法都切换到
英文、相同的 US 类键盘布局。

## 仍然不能自动验证的部分

self-hosted job 只验证 Windows 10 运行兼容性和文件恢复协议，不会自动向正在使用的
RDP 窗口发送按键。Windows 服务通常运行在 Session 0，自动键盘测试既无意义也
可能输入到错误窗口。

最终的 RDP simple/cmd-hex/zip-hex 输入和 Space 暂停测试仍应由人在场完成，并
核对远端 SHA-256。GitHub runner 可以作为服务运行；人工 RDP 冒烟测试应另外在
交互桌面中直接启动 exe。

Windows 10 已结束常规支持。用于长期运维时，应使用组织批准的 ESU/LTSC 版本或
隔离测试机，并继续遵守现有补丁和安全策略。
