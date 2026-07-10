# 在真实 Windows 10 上运行 GitHub Actions

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

## 仍然不能自动验证的部分

self-hosted job 只验证 Windows 10 运行兼容性和文件恢复协议，不会自动向正在使用的
RDP 窗口发送按键。Windows 服务通常运行在 Session 0，自动键盘测试既无意义也
可能输入到错误窗口。

最终的 RDP simple/cmd-hex/zip-hex 输入测试仍应由人在场完成，并核对远端
SHA-256。GitHub runner 可以作为服务运行；人工 RDP 冒烟测试应另外在交互桌面中
直接启动 exe。

Windows 10 已结束常规支持。用于长期运维时，应使用组织批准的 ESU/LTSC 版本或
隔离测试机，并继续遵守现有补丁和安全策略。
