# 2026-10-13 线性整合

本地原 HEAD 为 `fccb059`，跨平台实现仍是未提交改动。整合前已执行
`git stash push --include-untracked`，完整备份保留为
`67de5440390697ed800e17eca0071be3963f26a5`，没有删除该 stash。

先快进到远端 `origin/main` 的 `c1b4fba`，保留原三个提交：

1. `748e043`：WAL 追赶修复、恢复完整 ngram postings。
2. `f10fdcb`：MFT 快照列压缩。
3. `c1b4fba`：Windows 服务、资源、日志和相关改进。

再重放本地跨平台实现。没有创建 merge commit，没有改写远端提交，
也没有强推。新增整合提交直接以 `c1b4fba` 为父提交。

## 冲突处理

- `mft_service.cpp`：同时保留远端卷根完整性检查和本地事件路径构造。
  远端新增的监控线程异常处理也会使新增事件流失效，避免异常后继续
  对外提供看似完整的同步快照。
- `FileSearchService.cpp`：同时保留崩溃诊断状态、事件连接上限、停止日志
  和事件服务线程的 join。
- 原两个 Release 二进制均从整合源码重新构建，没有简单选用任一旧版本。
- 远端 `ngram_index.cpp`、`mft_snapshot_io.cpp`、`file_name_store.cpp`、
  `logger.cpp` 和 `win_file_id.cpp` 的改动原样保留。
- 原 `AppendWalChange` 的 void 签名继续保留；新增失败检测接口不影响
  远端 WAL 游标修复。

Windows Release 服务沿用远端的启动权限行为。控制台 `run` 测试需显式
以管理员启动（PowerShell `Start-Process ... -Verb RunAs`），而不是把
远端 manifest 改回自动提权。服务管理器启动方式未变。

历史验收文档 `implementation-validation.md` 记录的是 2026-10-02 的基线；
本次整合保留远端新增的 v7 压缩格式，不把历史“格式不变”的描述解释为
撤销用户随后提交的格式升级。

## 整合后验证

- Windows Release 全部目标构建成功，两个已跟踪二进制已重新生成。
- Windows CMake 单元测试 5/5 通过。
- 实际 Windows 搜索/推送及通用 Node 客户端端到端通过。
- 新压缩格式下原生快照测试通过：创建、释放 facts 后重命名、删除和
  快照游标围栏均验证成功。
- Windows 快照配额/超时测试、32 连接上限/断开回收/停止测试通过。
- WSL 常规 30 项测试无失败，其中 Btrfs 因未加载模块跳过；随后在同一
  执行中显式 `modprobe btrfs`，Btrfs 实际子卷/快照测试 1/1 通过。
- `git diff --check` 无错误，冲突索引为空。

测试输出和独立缓存不纳入提交；原始 stash 保留用于恢复。
