# FileSearch 实现与验收记录

验收日期：2026-10-02。范围是原请求的 Windows / Ubuntu 文件搜索、文件变化推送、状态丢失恢复，以及本地 WSL 测试；不是新增独立采集进程或改写已上线的 Windows 搜索算法。

## 需求逐项对应

| 原需求 / 约束 | 实现 | 验证证据 |
| --- | --- | --- |
| 采集器不与搜索服务分离 | Linux 一个服务进程内运行扫描、fanotify、索引、SQLite 和 IPC；Windows 继续使用原服务 | Linux 完整服务端到端、全根测试、实际 WSL systemd 启停 |
| 保持已验证 Windows 搜索行为 | 原 `Search` 默认路径继续使用；显式模糊阶段使用新增 `SearchWithPhase`；原搜索管道保留 | 实际 Windows 搜索/推送回归；基线头文件编译的客户端调用新 DLL 成功 |
| 避免 Windows 搜索算法 / 格式破坏性改动 | `ngram_index.cpp`、`filter_index.cpp`、`filter.cpp`、`pinyin.cpp`、`mft_snapshot_io.cpp`、`mft_snapshot.h` 相对 Git HEAD 无改动；原 95 个 DLL 导出符号全部保留 | Git diff 检查；新 DLL 导出符号共 120 个，缺失基线符号 0 个；原 WAL 写入 `void` 导出恢复，失败检测用新增接口 |
| 两端都能搜索 | 文件名子串、拼音/首字母、范围/数量限制；显式 exact / fuzzy / combined；不改变 Windows 省略 phase 时的默认行为 | Linux 索引差分测试、共享 Node 服务端到端；新文件、重命名、删除后的模糊搜索回归 |
| 两端都有变化推送 | 新增多读者有界事件流；Windows 独立 Events 管道，Linux events.sock；创建/修改/重命名/删除、类型/动作/范围过滤 | Windows 实际 NTFS 操作回归；Linux 实际 fanotify 服务、别名、XFS/Btrfs 测试 |
| 状态丢失能明确恢复，不能静默漏变更 | epoch/sequence 游标、过期通知、不可变分页快照与后续事件续接；服务不健康时拒绝快照和恢复 | 事件环过期/未来游标测试；Windows 快照加事件重建实际目录；Linux overflow、并发恢复、快照失效、重启测试 |
| Ubuntu 24.04，root | 原生 C++20 / SQLite / fanotify，实现使用 root 所需内核能力；提供 systemd unit | Ubuntu 24.04 chroot 用户空间 30/30，0 跳过；真实 WSL systemd 临时单元启动、崩溃自动重启与停止 |
| WSL 排除宿主机磁盘 | `/mnt/<单个小写盘符>`、`/mnt/wslg` 永久排除；仅选择 ext4/XFS/Btrfs，排除伪/网络/不支持文件系统 | 挂载策略测试；以 `/` 为根启动服务后，检查完整 SQLite 目录库每条路径：81,423 条，无宿主盘符、伪文件系统或服务状态目录 |
| 可实施、可调用 | Linux CMake 构建、systemd unit；独立通用 Node >=18 客户端，64 位身份/游标使用 BigInt；原 Windows addon 不修改 | 共享客户端单元/真实服务测试；部署命令见 linux-service.md |

## 本轮最终测试

- Windows Release 构建成功。
- Windows CMake 测试 5/5：连续性检查、快照合并、快照路径视图、健康状态/关闭 WAL 注入、事件环。
- Windows 实际服务：`windows_event_e2e.ps1`、`windows_snapshot_e2e.js`、`windows_ipc_limits_e2e.js` 全部通过；结果文件 `tests/windows_event_e2e.result.json` 为 `ok:true`。
- 基线原生客户端：使用 Git HEAD 的头文件、由基线 DLL 导出重建的导入库编译，再加载新 DLL；状态、释放 facts、范围搜索、创建/重命名/删除及停止全部通过。记录见 `tests/baseline-search-abi-final.result.txt`。这证明的是匹配当前 Git 基线的 AutoFileSearch ABI，不是任意历史版本的内部类布局。
- Ubuntu 24.04 用户空间完整测试 30/30，0 跳过，50.40 秒；包含真实 ext4/XFS/Btrfs、子卷、overflow、重启、损坏目录库隔离、IPC、快照和搜索。
- WSL ASan + UBSan（含 detect_leaks）完整测试 30/30，0 跳过，64.56 秒。
- WSL `/` 根目录实际搜索、重命名/删除推送、范围快照及完整目录库排除检查通过。
- WSL 实际 systemd 临时单元：root 运行、目录/套接字权限、SIGKILL 后自动重启、既有 ID 保持、停机期间变更恢复、正常停止及持久状态保留通过。测试独立状态目录已清理，生产单元未修改。

百万实际文件的 Linux 完整服务测量，以及 Windows 452 万条完整分页导出，见独立 benchmark 文档；它们是已完成的专项测试，不是对任意机器的性能保证。

## 丢失状态是否需要全量扫描

| 情况 | 当前实现的恢复动作 |
| --- | --- |
| 客户端断线，游标仍在服务事件环内 | 直接续接事件，不扫描文件系统 |
| 客户端游标过期，但服务目录库健康 | 客户端重新获取范围快照，再从快照游标续接；服务不因此重新扫描磁盘 |
| Linux 服务重启或停机期间发生变化 | 扫描配置根目录，和持久目录库对账；fanotify 没有可回放任意停机时间的持久日志 |
| Linux 内核队列溢出、根身份/挂载拓扑改变 | 失效事件 epoch、暂时拒绝旧目录搜索/快照，重新扫描整个配置根目录；尚未实现只扫描受影响子树的优化 |
| Linux 目录库确认损坏/丢失 | 保留隔离损坏文件，重建配置根；客户端丢弃旧 epoch 和旧 ID |
| Windows 原生 USN 历史仍连续 | 保留原缓存/USN 同步方式，无需仅因订阅断线而重新枚举 MFT |
| Windows USN 更换/历史缺口、非正常读取失败或推送所需 WAL 写入失败 | 新事件流失效并拒绝同步快照，要求重启/必要时重建；不冒充完整状态，也不自动改动生产 USN 日志 |

配置根可以是 `/home/某目录`，也可以是 `/`；“全量”指配置根内符合策略的文件系统，不包括 WSL 宿主盘符和其他排除挂载。

## 明确边界

1. 事件流是变更通知，不是完整系统调用审计；内核可能合并事件。两端事件历史均为进程内缓存，没有持久跨重启事件日志。
2. Linux 默认 root-only：状态目录 0700、套接字 0600。普通用户调用需另行设计授权，不自动扩大权限。
3. Ubuntu 24.04 验证在 WSL 内核上运行；不是原生 Ubuntu 24.04 内核或开机引导测试。live systemd 测试在宿主 WSL Ubuntu 用户空间完成。
4. Windows 健康故障测试通过生产辅助函数注入失效和关闭 WAL，不对真实 USN 日志做破坏测试，也不模拟物理硬盘损坏。
5. 仓库旁现有 addon 包缺少入口，旧预编译 `.node` 在 `directStatus` 上与 Git 基线 DLL 同样异常退出。这是既有版本不匹配，未把它伪报为兼容性通过，也未改动该 addon；本次新增通用客户端可以直接调用两端服务。
6. Windows Filter 模式没有模糊搜索能力，显式请求会报错；默认 Ngram 模式支持新增显式阶段。原默认搜索路径不启用新增模糊回退。
