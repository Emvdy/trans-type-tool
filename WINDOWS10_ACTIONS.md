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
- UTF-8、UTF-8 BOM、preserve 三种输出逐字节对比。
- ZIP 压缩率和恢复临时文件检查。

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

指定文件或读取本地 Windows 剪贴板：

```powershell
.\trans_type.exe --mode simple --file c:\work\input.txt
.\trans_type.exe --mode simple --source clipboard
```

simple 会拒绝大写、Unicode、`@#$` 和其他需要 Shift 的字符。遇到这些内容不要
修改键盘模拟逻辑，直接使用复杂模式。

### cmd-hex 模式

适合 `@#$`、脚本、中文和其他 Unicode。远端先打开普通权限的 `cmd.exe` 或
PowerShell，然后在倒计时内把焦点切到该远端窗口：

```powershell
.\trans_type.exe --mode cmd-hex --file c:\work\input.txt --remote-output trans.txt
```

它会输入全小写、无需 Shift 的 PowerShell/certutil 命令，在远端当前目录创建
`trans.txt`，但不会执行该文件。

### zip-hex 模式

适合较长且重复内容较多的文件：

```powershell
.\trans_type.exe --mode zip-hex --file c:\work\input.txt --remote-output trans.txt
```

小文件或已经压缩过的内容不一定更快，先用 `--dry-run` 比较输出大小。

### 调整速度和编码

高延迟 RDP 可以放慢：

```powershell
.\trans_type.exe --mode cmd-hex --delay-ms 50 --line-delay-ms 300 --start-delay-sec 10
```

复杂模式默认生成无 BOM UTF-8，也可以选择：

```powershell
.\trans_type.exe --mode cmd-hex --output-encoding utf8-bom
.\trans_type.exe --mode zip-hex --file c:\work\input.txt --output-encoding preserve
```

`preserve` 只支持文件输入。

### 完成后核对

复杂模式启动前会显示本地预期 SHA-256，远端完成后 `certutil` 也会显示 SHA-256。
两者必须相同。远端 `tt.hex` 和 `tt.zip` 会保留用于恢复；确认输出正确后再人工
删除。

运行前始终关闭 Caps Lock、释放 Shift/Ctrl/Alt，并把本地和 RDP 输入法都切换到
英文、相同的 US 类键盘布局。

## 仍然不能自动验证的部分

self-hosted job 只验证 Windows 10 运行兼容性和文件恢复协议，不会自动向正在使用的
RDP 窗口发送按键。Windows 服务通常运行在 Session 0，自动键盘测试既无意义也
可能输入到错误窗口。

最终的 RDP simple/cmd-hex/zip-hex 输入测试仍应由人在场完成，并核对远端
SHA-256。GitHub runner 可以作为服务运行；人工 RDP 冒烟测试应另外在交互桌面中
直接启动 exe。

Windows 10 已结束常规支持。用于长期运维时，应使用组织批准的 ESU/LTSC 版本或
隔离测试机，并继续遵守现有补丁和安全策略。
