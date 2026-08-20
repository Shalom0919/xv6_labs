# xv6 Labs 2021 实验报告（九）
## Lab fs: File System

作者：Shalom0919  
实验分支：`fs`  
本地提交：`ebd5b99`  
代码仓库：[github.com/Shalom0919/xv6_labs](https://github.com/Shalom0919/xv6_labs)  
完成日期：2026-08-17  
官方评分：**100/100**

---

# 1. 实验概述

本实验基于 MIT 6.S081 Fall 2021 的 `xv6-labs-2021` 仓库 `fs` 分支，围绕 xv6 文件系统完成两个功能：扩大单个文件可寻址的数据块数量，以及实现符号链接。实验涉及磁盘 inode 格式、逻辑块到磁盘块的映射、文件截断回收、路径解析和系统调用接口。

主要目标如下：

- 将 inode 的 12 个直接块指针调整为 11 个，以直接、一级间接和二级间接寻址把最大文件从 268 块提升到 65,803 块。
- 在 `itrunc()` 中完整回收二级间接树，避免磁盘块泄漏。
- 新增 `symlink(target, path)`、`T_SYMLINK` 和 `O_NOFOLLOW`，在 `open()` 中递归跟随并检测链接环。
- 保持 `link()` 与 `unlink()` 操作链接 inode 本身，不改变原有硬链接语义。

官方实验说明：[MIT 6.S081 - Lab: file system](https://pdos.csail.mit.edu/6.828/2021/labs/fs.html)

## 1.1 实验环境

| 项目 | 配置 |
|---|---|
| 宿主环境 | Windows + WSL2 Ubuntu，x86_64 |
| 客体系统 | xv6-riscv，RISC-V 64 |
| 文件系统镜像 | 200,000 blocks，`BSIZE=1024` |
| 编译工具链 | `riscv64-linux-gnu-gcc` |
| 官方基线 | `origin/fs`，提交 `46bcbaf` |
| 实验成果 | `fs` 分支，提交 `ebd5b99` |
| 官方评分 | `make grade`，100/100 |

## 1.2 修改文件

| 文件 | 主要作用 |
|---|---|
| `kernel/fs.h`、`kernel/file.h` | 调整 inode 地址槽并定义最大文件大小 |
| `kernel/fs.c` | 实现二级间接映射和完整块回收 |
| `kernel/sysfile.c` | 实现 `sys_symlink()` 和 `open()` 链接跟随 |
| 系统调用与用户接口头文件 | 注册调用，新增链接类型、标志和用户桩函数 |
| `Makefile`、`gradelib.py` | 构建 `symlinktest` 并兼容当前工具链 |
| `time.txt` | 记录实验用时 |

# 2. 大文件寻址结构

## 2.1 inode 地址槽重新分配

磁盘 inode 的大小不能改变。原布局为 12 个直接块地址和 1 个一级间接块地址，共 13 个 `uint` 槽位。本实现将其重新解释为 11 个直接块、1 个一级间接块和 1 个二级间接块：

```c
#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
#define NDOUBLY (NINDIRECT * NINDIRECT)
#define MAXFILE (NDIRECT + NINDIRECT + NDOUBLY)

struct dinode {
  ...
  uint addrs[NDIRECT+2];
};
```

内存 inode 的 `addrs[]` 同样改为 `NDIRECT+2`，确保内存表示和磁盘表示具有相同大小。每个间接块可以保存 `1024 / 4 = 256` 个块号，因此最大文件块数为：

| 层级 | 数据块数量 | 逻辑块范围 |
|---|---:|---|
| 直接块 | 11 | 0 至 10 |
| 一级间接 | 256 | 11 至 266 |
| 二级间接 | 256 x 256 | 267 至 65,802 |
| 合计 | 65,803 | 最大约 64.26 MiB |

## 2.2 二级索引计算

进入二级间接区后，先减去直接区和一级间接区的逻辑块数，再将剩余编号拆成两级索引：

```c
bn -= NINDIRECT;
uint outer = bn / NINDIRECT;
uint inner = bn % NINDIRECT;
```

`outer` 选择二级间接块中的一级间接块地址，`inner` 再选择该一级间接块中的数据块地址。除法与取模将连续逻辑块稳定映射到 256 x 256 的地址矩阵。

# 3. bmap() 二级间接映射

## 3.1 按需分配元数据块

`bmap()` 只有在实际写入对应范围时才分配二级间接块、下层一级间接块和数据块：

```c
if((addr = ip->addrs[NDIRECT+1]) == 0)
  ip->addrs[NDIRECT+1] = addr = balloc(ip->dev);
bp = bread(ip->dev, addr);
a = (uint*)bp->data;

if((addr = a[outer]) == 0){
  a[outer] = addr = balloc(ip->dev);
  log_write(bp);
}
brelse(bp);

ibp = bread(ip->dev, addr);
ia = (uint*)ibp->data;
if((addr = ia[inner]) == 0){
  ia[inner] = addr = balloc(ip->dev);
  log_write(ibp);
}
brelse(ibp);
return addr;
```

该流程保持稀疏使用时的空间效率：小文件仍然只使用直接块；文件越过第 267 个数据块后才创建二级结构。`balloc()` 返回清零后的块，因此新建索引块中未使用的槽位均为 0。

## 3.2 日志与 buffer 生命周期

每次修改索引块内的块号后调用 `log_write()`，使元数据更新进入 xv6 文件系统日志。所有 `bread()` 都存在配对的 `brelse()`；在读取下一级前先记录块号并释放上一级 buffer，避免不必要地长期占用 buffer cache。

直接写 `ip->addrs[NDIRECT+1]` 不需要单独调用 `log_write()`，因为 inode 的内存副本会在 `writei()` 完成后由原有 `iupdate()` 路径写回并记录到日志中。这与原一级间接块入口的处理方式一致。

# 4. itrunc() 完整回收

删除或截断大文件时，除了数据块，还必须释放两层索引块。实现先读取顶层二级间接块，再逐项读取其中存在的一级间接块：

```c
if(ip->addrs[NDIRECT+1]){
  bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
  a = (uint*)bp->data;
  for(i = 0; i < NINDIRECT; i++){
    if(a[i]){
      ibp = bread(ip->dev, a[i]);
      ia = (uint*)ibp->data;
      for(j = 0; j < NINDIRECT; j++)
        if(ia[j])
          bfree(ip->dev, ia[j]);
      brelse(ibp);
      bfree(ip->dev, a[i]);
    }
  }
  brelse(bp);
  bfree(ip->dev, ip->addrs[NDIRECT+1]);
  ip->addrs[NDIRECT+1] = 0;
}
```

释放顺序由叶到根：先释放数据块，再释放一级索引块，最后释放顶层二级索引块。若先释放父索引块，就会丢失对子块的地址引用。完成后将 inode 地址槽清零，并由原有代码把 `ip->size` 设为 0、调用 `iupdate()` 持久化。

`bigfile` 在写完 65,803 块后会删除测试文件，随后完整 `usertests` 继续创建和写入文件。两者连续通过，说明大文件使用的块已被正确归还，没有产生可见的空间泄漏或重复分配。

# 5. 符号链接系统调用

## 5.1 用户态到内核态接口

实现新增系统调用号 `SYS_symlink=22`，并在 `user/usys.pl`、`user/user.h`、`kernel/syscall.c` 中接通用户态桩函数和内核分发表。`T_SYMLINK=4` 标识链接 inode，`O_NOFOLLOW=0x800` 与现有 open 标志不重叠。

## 5.2 创建链接

`sys_symlink()` 不要求 target 已存在。它创建类型为 `T_SYMLINK` 的 inode，并将目标路径连同字符串终止符保存在 inode 数据中：

```c
if(argstr(0, target, MAXPATH) < 0 ||
   argstr(1, path, MAXPATH) < 0)
  return -1;

begin_op();
if((ip = create(path, T_SYMLINK, 0, 0)) == 0){
  end_op();
  return -1;
}
n = strlen(target) + 1;
if(writei(ip, 0, (uint64)target, 0, n) != n){
  iunlockput(ip);
  end_op();
  return -1;
}
iunlockput(ip);
end_op();
```

创建 inode、写入目标和目录项更新都位于一个文件系统事务中。保存终止符使后续 `namei()` 可以直接使用读取出的缓冲区。悬空链接能够成功创建，只有在普通 `open()` 尝试解析不存在的 target 时才返回失败。

# 6. open() 路径跟随与并发语义

## 6.1 递归跟随和环检测

当未指定 `O_NOFOLLOW` 且当前 inode 类型为 `T_SYMLINK` 时，`open()` 读取目标路径，释放当前 inode，再对目标调用 `namei()`：

```c
if((omode & O_NOFOLLOW) == 0){
  for(int depth = 0; ip->type == T_SYMLINK; depth++){
    if(depth >= 10 ||
       (n = readi(ip, 0, (uint64)target, 0, MAXPATH)) <= 0){
      iunlockput(ip);
      end_op();
      return -1;
    }
    target[MAXPATH-1] = 0;
    iunlockput(ip);
    if((ip = namei(target)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
  }
}
```

最多跟随 10 层链接。若第 10 层后仍得到符号链接，则按循环或异常深链处理并返回 `-1`。每次解析下一目标前使用 `iunlockput()`，避免在 `namei()` 遍历文件系统时持有旧 inode 锁，从而防止自环和多节点环造成锁递归或死锁。

## 6.2 O_NOFOLLOW 与其他系统调用

指定 `O_NOFOLLOW` 时跳过解析，文件描述符直接引用符号链接 inode，因此 `fstat()` 能观察到 `T_SYMLINK`。`link()` 和 `unlink()` 没有调用这段跟随逻辑，仍操作路径最后一个分量对应的 inode 本身，满足实验要求。

并发测试由两个子进程反复创建、查询和删除同一路径。`sys_symlink()` 和 `sys_unlink()` 均使用 `begin_op()`/`end_op()`，目录项修改仍在原有 inode 锁和日志规则下执行；测试通过说明没有暴露半写入的链接 inode，也没有破坏目录一致性。

# 7. 实验结果、复现与总结

## 7.1 官方评分结果

![Lab fs 官方评分结果](../screenshots/lab-fs-make-grade.png)

图 1　官方 `grade-lab-fs` 评分结果

官方脚本在 WSL 的 Linux 原生文件系统中运行，结果如下：

| 测试项 | 得分 | 结果 | 分析 |
|---|---:|---|---|
| `bigfile` | 40/40 | 127.0 秒，OK | 成功写入 65,803 块并完成清理 |
| 基本符号链接 | 20/20 | OK | 跟随、悬空链接、深链、环和 `O_NOFOLLOW` 正确 |
| 并发符号链接 | 20/20 | OK | 并发创建、查询和删除保持一致性 |
| `usertests` | 19/19 | 202.6 秒，OK | 原有文件系统及系统调用无回归 |
| `time.txt` | 1/1 | OK | 格式符合要求 |
| 合计 | 100/100 | 通过 | 官方评分脚本退出码为 0 |

首次直接在 `/mnt/c` 的 Windows 挂载卷运行时，`bigfile` 和 `usertests` 分别触发 180 秒、360 秒超时；输出始终停留在正常写入进度，没有 panic 或断言错误。将同一工作树复制到 WSL 的 ext4 文件系统后，不修改代码和评分脚本即得到 100/100。原因是本实验会密集随机更新约 200 MB 的 `fs.img`，跨 Windows 挂载层的同步 I/O 显著慢于 Linux 原生文件系统。

## 7.2 官方评分复现

建议在 WSL 原生临时目录运行官方评分，避免 `/mnt/c` 性能造成假失败：

```sh
wsl -d Ubuntu
workdir=$(mktemp -d)
cp -a /mnt/c/Users/Aemeath/Helper/2026/OS_Design/xv6-labs-2021/. "$workdir/"
cd "$workdir"
git switch fs
make clean
make grade
echo $?
```

通过标准是最后输出 `Score: 100/100`，并且 `echo $?` 输出 `0`。此命令仍执行仓库原始的 `grade-lab-fs`，只改变工作目录所在的存储介质，不修改超时、测试内容或计分规则。

## 7.3 实验总结

本实验将文件系统的两条关键路径连接起来：`bmap()` 负责从逻辑块号向下建立地址树，`itrunc()` 必须按相反方向完整拆除同一棵树；两者只有对称实现，才能同时保证大文件可用和磁盘空间可回收。符号链接则展示了 inode 数据与路径解析的组合：链接本身是独立 inode，目标路径是其内容，只有 `open()` 在未设置 `O_NOFOLLOW` 时解释该内容。最终大文件、符号链接、并发测试和完整回归测试全部通过，官方得分 100/100。
