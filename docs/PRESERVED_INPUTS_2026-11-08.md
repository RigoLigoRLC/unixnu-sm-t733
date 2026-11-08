# 清理工作区前保留的原始输入

2026-11-08，私有文件另存于 `/Users/rigoligo/expm/unixnu-sm-t733-preserved-20261108`。
目录包含 28 个输入文件，总计 144370636 字节，约 137.68 MiB；下表及说明文字另计。
只保留无法从公开源码和原厂固件重新生成的内容。未保存 Git 副本、系统下载包、
构建产物、派生缓存或重复密钥副本。重建步骤见 [REBUILDING.md](REBUILDING.md)。

| 路径 | 字节 | 作用 |
| --- | ---: | --- |
| `local.toml` | 376 | 本机身份和软件钥匙串根密钥 |
| `data.dbr` | 1290400 | 原固定 UUID 的 Data 种子 |
| `owner.pem` | 2484 | 根卷签名私钥；公钥由它生成 |
| `device-original/partitions/efs.img` | 20971520 | 原厂设备状态 |
| `device-original/partitions/sec_efs.img` | 20971520 | 原厂设备状态 |
| `device-original/partitions/persist.img` | 33554432 | 原厂设备状态 |
| `device-original/partitions/modemst1.img` | 3145728 | 原厂设备状态 |
| `device-original/partitions/modemst2.img` | 3145728 | 原厂设备状态 |
| `device-original/partitions/param.img` | 10485760 | 原厂设备状态 |
| `device-original/partitions/persistent.img` | 524288 | 原厂设备状态 |
| `device-original/partitions/bota.img` | 42352640 | 原厂设备状态 |
| `device-original/partitions/steady.img` | 4194304 | 原厂设备状态 |
| `device-original/partitions/ddr.img` | 1048576 | 原厂设备状态 |
| `device-original/partitions/devinfo.img` | 4096 | 原厂设备状态 |
| `device-original/partitions/uefivarstore.img` | 524288 | 原厂设备状态 |
| `device-original/partitions/btd.img` | 2097152 | 原厂设备状态 |
| `device-original/gpt/sda` 至 `sdf` 的 `primary-header.bin`、`primary-entries.bin` | 合计 57344 | 原分区布局和 GUID，共 12 文件 |

分区镜像来自 2026-10-22 的本机原厂备份。按 PIT 的 Flash Filename 与对应
AP/BL/CSC/HOME_CSC 成员逐项比较，36 个分区可从原厂包重取。其余 29 个中，16 个镜像
完整读取均为空白；保留上表 13 个非空且原厂包没有重建来源的镜像。
EFS、persist、modemst 保存本机身份、校准或原厂状态，PARAM 有原厂驱动读写记录。
其余已确认含原始状态，内部字段尚未全部解码。

以下原备份为空白，按原分区长度填零即可重建，所以未另存：
`fsc`、`fsg`、`ssd`、`keystore`、`keyrefuge`、`frp`、`cdt`、`em`、`testparti`、
`cmnlib`、`cmnlib64`、`uhuh`、`limits`、`limits-cdsp`、`splash`、`spu`。
原长度、分区名与布局保存在 GPT 主表。备份 GPT 和解码文本可由主表生成。

这些分区备份和 Data 种子不包含当前平板的 userdata 用户文件。原厂状态镜像供恢复原厂
身份或布局使用；普通 UniXNU FAT 更新使用平板现存 Data。
复制过程未读取密钥值到日志，未计算文件确认哈希；私有目录权限 0700，文件权限 0600。
