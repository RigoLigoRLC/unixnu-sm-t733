# T733 MSC service

本工具可编译为 `dist/T733 MSC.app`。状态栏程序以登录用户运行；root helper 由用户在 Terminal 中启动，后续客户端经本地 Unix socket 发送请求。

新克隆可直接使用命令行服务，无需先编译状态栏程序。从设备仓库根目录执行：

```sh
sudo /usr/bin/python3 -I tools/msc-service/service.py serve --detach
tools/msc-service/mscctl status
tools/msc-service/mscctl list
```

系统重建和恢复输入步骤见 [重建教程](../../docs/REBUILDING.md)。

## 启动和停止

双击此目录的 `Start Service.command`，在 Terminal 输入一次 sudo 密码。出现后台启动成功提示后即可关闭该窗口，服务继续常驻。它同时打开状态栏应用。也可先打开 `dist/T733 MSC.app`，选择 `Start service in Terminal`。

状态栏显示服务是否运行和传输百分比，菜单可列出设备、读写 FAT、显示任务日志、安全弹出和停止服务。`Quit menu app` 只关闭菜单程序；`Stop service` 停止 helper。传输中停止请求会返回忙碌。服务无需登录项或 launchd 安装，Mac 重启后重新启动即可。故障诊断时可在 Terminal 手动执行 `sudo /usr/bin/python3 -I service.py serve` 保持前台运行，Ctrl-C 会等当前操作结束再退出。

工具依赖本机 `/usr/bin/python3` 和 macOS `diskutil`。构建使用 `/usr/bin/swiftc`，命令为 `bash tools/msc-service/build.sh`。编译缓存和应用都保存在本目录。应用使用本机 ad-hoc 签名。

## 客户端

从项目根目录运行，均无需 sudo：

```sh
tools/msc-service/mscctl status
tools/msc-service/mscctl list
tools/msc-service/mscctl read-fat out/t733-fat-backup.img
tools/msc-service/mscctl write-fat /absolute/path/to/512MiB-payload.img
tools/msc-service/mscctl logs
tools/msc-service/mscctl shutdown
```

所有输出均为 JSONL。传输中输出 `{"progress":1048576,"total":536870912}`；最后一条 `{"ok":true,"message":"write-fat completed"}` 表示完成，失败为 `{"ok":false,"error":"..."}`。退出码为 0 或 1。写入完成包含磁盘刷新。客户端中断连接后，helper 会继续已接受的传输，通过 `status` 和 `logs` 获取结果。

FAT 读写由 helper 调用系统 `/bin/dd`，采用 `bs=1048576 count=512` 和固定 `skip=8948` 或 `seek=8948`；写入增加 `conv=notrunc`，结束后执行 `/bin/sync`。客户端文件描述符作为 dd 的标准输入或输出，helper 每秒通过 SIGINFO 取得 dd 的实际字节进度。未按 MiB 对齐的只读日志请求使用 4096 字节块。

`list` 返回包含 `partition`、`whole` 和 `mounted` 的设备列表。服务先从 external physical 的分区列表按 userdata 的容量筛选，只对候选分区执行 `diskutil info` 核对名称、USB 来源与几何。没有匹配设备时列表为空；有多个匹配设备时拒绝传输。每个磁盘操作重新枚举，因此调用方不传磁盘编号。目标分区已挂载时只卸载该分区，未挂载时直接打开 raw 节点检查 SHNGROOT 头，再调用 dd。

`read-fat` 输出必须是尚不存在的新文件，父目录须已存在。写入失败后应保留备份及任务日志，再根据实际错误恢复。收到 `write-fat completed` 后完整重启平板固件读取新 FAT。当前固件在写入后的 eject 请求重新枚举期间出现过异常，部署流程已停止调用该操作；菜单中的 eject 功能仍保留。

还提供只读的有界日志区采集：

```sh
tools/msc-service/mscctl read-log out/t733-log-region.bin --offset 0 --length 0x100000
```

该命令读取 userdata 最后 1 GiB 内的指定字节，单次最多 64 MiB，offset/length 按 4096 字节对齐。它不假定设备已经写入日志格式；日志区尚未部署时读取的是该位置原有数据。

## 设备范围和 IPC

helper 要求 USB、external physical、可写、userdata 名称、4096 字节块、分区偏移 9506390016、分区长度 117878796288 全部匹配。读写 FAT 前检查 SHNGROOT v1 的 OS/SystemOS 两个窗口及 FAT 位置。唯一允许写入的范围是 userdata 相对 `0x22F400000` 起的 512 MiB；输入还须为标签 `T733ESP` 的完整 FAT32 镜像。客户端不能传任意磁盘、写偏移或 shell 命令。

socket 位于 root 所有的 `/var/run/t733-msc-UID/service.sock`，模式 0600，仅启动者 UID 可连接；服务通过 macOS `getpeereid` 校验真实 peer UID。客户端自己打开文件，通过 SCM_RIGHTS 传递 descriptor；helper 只接受该 UID 拥有的常规文件。root 不根据客户端路径创建或打开输出文件。任务日志为该目录下的 `tasks.jsonl`，客户端 `logs` 最多读取末尾 64 KiB。服务通过互斥锁一次执行一个磁盘操作，status 在传输时仍可查询。

该服务的系统 dd 和 sync 流程已用于实机 512 MiB FAT 更新。76b 的构建与部署结果见 [76 记录](../../docs/GPU_UPSTREAM_REBASE_76_2026-11-04.md)。
