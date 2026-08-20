# xv6 Labs 2021 实验报告（八）
## Lab lock: Parallelism and Locking

作者：Shalom0919  
实验分支：`lock`  
本地提交：`58ff314`  
代码仓库：[github.com/Shalom0919/xv6_labs](https://github.com/Shalom0919/xv6_labs)  
完成日期：2026-08-17  
官方评分：**70/70**

---

# 1. 实验概述

本实验基于 MIT 6.S081 Fall 2021 的 `xv6-labs-2021` 仓库 `lock` 分支，通过重新设计物理页分配器和磁盘块缓存的数据结构，降低多核环境下的自旋锁争用。原始实现分别使用一把 `kmem.lock` 保护全局空闲页链表、一把 `bcache.lock` 保护全部缓存 buffer；即使多个 CPU 操作互不相关的页面或磁盘块，也必须串行执行。

实验目标不是删除同步，而是缩小共享状态和锁的覆盖范围：

- 为每个 CPU 建立独立物理页 freelist，使常规 `kalloc()` 和 `kfree()` 只访问本地状态。
- 当本地 freelist 为空时，从其他 CPU 批量窃取页面，保持所有物理内存仍可使用。
- 使用 13 个哈希桶组织 buffer cache，每桶一把锁，使不同磁盘块的查找、引用和释放可以并行。
- 仅在 cache miss 和淘汰时使用全局锁，保持“同一磁盘块最多一个缓存副本”的不变量。
- 用释放时间戳替代原有全局 LRU 双向链表，避免 `brelse()` 获取全局锁。

官方实验说明：[MIT 6.S081 - Lab: locks](https://pdos.csail.mit.edu/6.828/2021/labs/lock.html)

## 1.1 实验环境

| 项目 | 配置 |
|---|---|
| 宿主环境 | Windows + WSL2 Ubuntu，x86_64 |
| 客体系统 | xv6-riscv，RISC-V 64 |
| QEMU CPU 数 | 3 |
| 编译工具链 | `riscv64-linux-gnu-gcc` |
| 官方基线 | `origin/lock`，提交 `281b66c` |
| 实验成果 | `lock` 分支，提交 `58ff314` |
| 官方评分 | `make grade`，70/70 |

## 1.2 修改文件

| 文件 | 主要作用 |
|---|---|
| `kernel/kalloc.c` | per-CPU freelist 与批量 stealing |
| `kernel/bio.c` | 哈希桶 buffer cache 与串行淘汰 |
| `kernel/buf.h` | 为 buffer 增加最后释放时间戳 |
| `Makefile`、`gradelib.py` | 当前工具链与 Python 兼容处理 |
| `time.txt` | 记录实验用时 |

# 2. Per-CPU 物理页分配器

## 2.1 数据结构拆分

原始 `kmem` 只有一把锁和一条链表。修改后按 `NCPU` 建立数组：

```c
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void kinit(void)
{
  for(int i = 0; i < NCPU; i++)
    initlock(&kmem[i].lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}
```

所有锁名以 `kmem` 开头，使实验提供的锁统计系统调用能够识别。启动阶段由执行 `freerange()` 的 CPU 接收全部空闲页；其他 CPU 第一次分配时通过 stealing 获得本地页面。

## 2.2 本地释放与分配

`cpuid()` 只有在中断关闭时才能安全使用，否则获取 CPU 编号后进程可能被中断并迁移。实现用一层 `push_off()`/`pop_off()` 包围 CPU 编号和本地 freelist 操作：

```c
push_off();
id = cpuid();
acquire(&kmem[id].lock);
r->next = kmem[id].freelist;
kmem[id].freelist = r;
release(&kmem[id].lock);
pop_off();
```

`acquire()` 自身也会关闭中断，但外层 `push_off()` 保证从 `cpuid()` 到锁释放期间 CPU 身份不变。xv6 的中断关闭计数支持嵌套，因此内外两层会按相反顺序安全恢复。

`kalloc()` 首先只锁当前 CPU 的 freelist，取出头结点后立即释放。绝大多数分配和释放由运行该进程的 CPU 本地完成，三个 CPU 不再持续竞争同一 cache line。

## 2.3 正确性不变量

每个空闲物理页恰好属于一条 freelist；已分配页面不在任何 freelist 中。链表修改只在所属 `kmem[i].lock` 下发生。页面仍保留原有地址范围、页对齐检查和 junk fill，因此非法释放、悬空引用检测及调用接口均与原实现一致。

# 3. 空闲页窃取策略

## 3.1 为什么采用批量窃取

如果本地为空时每次只从其他 CPU 取一页，进程连续扩展地址空间会为每一页访问 donor 锁，重新制造争用。本实现统计 donor freelist 长度，并一次摘取约一半：

```c
acquire(&kmem[donor].lock);
count = 0;
for(last = kmem[donor].freelist; last; last = last->next)
  count++;

take = (count + 1) / 2;
stolen = kmem[donor].freelist;
last = stolen;
for(int n = 1; n < take; n++)
  last = last->next;
kmem[donor].freelist = last->next;
last->next = 0;
release(&kmem[donor].lock);
```

随后在本地锁下接入 stolen 链，并立即取出一页返回。批量转移使后续分配走本地快速路径，同时给 donor 保留约一半页面，避免一次窃取造成极端失衡。

## 3.2 避免多锁死锁

实现不同时持有 donor 与 local 两把锁：先在 donor 锁下摘链，释放后再获取 local 锁接入。这避免两个 CPU 同时互相窃取时形成 `CPU0 lock -> CPU1 lock` 与 `CPU1 lock -> CPU0 lock` 的环形等待。

摘下的链在两个锁之间暂时由当前执行流独占，不属于任何共享 freelist，因此其他 CPU 无法访问。外层中断关闭又保证当前 CPU 不会在中间阶段切换到其他内核执行路径。

## 3.3 内存完整性验证

`kalloctest test2` 会分配几乎全部物理页，并统计可用页数。本次运行报告 32495 个空闲页（总物理页 32768，差值为内核占用），测试通过。`usertests sbrkmuch` 也通过，说明页面在各 CPU freelist 间转移后没有丢失、重复或形成链表环。

# 4. 哈希化 Buffer Cache

## 4.1 从全局 LRU 链表到哈希桶

buffer cache 使用 13 个桶，质数桶数可降低连续 block number 的冲突：

```c
#define NBUCKET 13

struct bucket {
  struct spinlock lock;
  struct buf *head;
};

struct {
  struct spinlock lock;       // only serializes cache misses
  struct buf buf[NBUF];
  struct bucket bucket[NBUCKET];
  uint clock;
} bcache;

static uint bhash(uint dev, uint blockno)
{
  return (dev + blockno) % NBUCKET;
}
```

每个 `bcache.bucket` 锁保护该桶链表、buffer 的 `dev/blockno` 身份、`refcnt` 和 `timestamp`。缓存命中只遍历并锁定目标桶，不访问全局 `bcache.lock`；映射到不同桶的磁盘块可以在不同 CPU 上并行。

## 4.2 命中路径

```c
index = bhash(dev, blockno);
acquire(&bcache.bucket[index].lock);
for(b = bcache.bucket[index].head; b; b = b->next){
  if(b->dev == dev && b->blockno == blockno){
    b->refcnt++;
    release(&bcache.bucket[index].lock);
    acquiresleep(&b->lock);
    return b;
  }
}
release(&bcache.bucket[index].lock);
```

桶自旋锁只保护元数据操作，buffer 内容仍由原有 sleeplock 保护。必须在释放桶锁之前增加 `refcnt`；否则 cache miss 路径可能把刚查到但尚未锁定的 buffer 当成空闲项淘汰。

## 4.3 brelse、bpin 与 bunpin

`brelse()` 先释放 buffer sleeplock，再在对应桶锁下递减引用数。引用数归零时，用原子递增的逻辑时钟记录最后释放时间：

```c
b->refcnt--;
if(b->refcnt == 0)
  b->timestamp = __sync_add_and_fetch(&bcache.clock, 1);
```

原实现每次释放都要修改全局 LRU 双向链表；时间戳方案把操作限制在一个桶内。`bpin()` 和 `bunpin()` 同样改用 buffer 所在桶的锁，日志系统对 buffer 的固定引用仍保持正确。

# 5. Cache Miss、淘汰与并发正确性

## 5.1 二次查找保证唯一副本

首次桶查找 miss 后获取全局 `bcache.lock`，然后再次查找目标桶。原因是等待全局锁期间，另一个 CPU 可能已经为相同 `(dev, blockno)` 创建缓存项。如果不二次检查，两个 CPU 会各自选择 victim，破坏“同一块最多一个缓存副本”的核心不变量。

全局锁仅串行化 miss/eviction，不参与 hit 和 release，因此 `bcachetest test0` 的常见路径仍保持并行。

## 5.2 选择 LRU victim

在全局 miss 锁下扫描固定 `bcache.buf` 数组。对每个 buffer 获取它当前所属桶锁，寻找 `refcnt==0` 且 `timestamp` 最小的项。选中后重新获取旧桶锁并再次检查 `refcnt`；如果期间被命中，就放弃并重新选择。

淘汰过程从旧桶链表删除 victim，设置新 `dev`、`blockno`、`valid=0`、`refcnt=1`，再插入目标桶。旧桶和新桶相同时只获取一次锁；不同时在全局 miss 锁下获取两把桶锁。因为其他路径最多持有一把桶锁，其他 eviction 又被全局锁串行化，所以不会形成桶锁之间的循环等待。

## 5.3 refcnt 与 sleeplock 的配合

`refcnt` 表示使用或等待该 buffer 的调用者数；buffer sleeplock 确保同一时刻只有一个调用者读写内容。cache hit 在等待 sleeplock 前增加引用，释放内容锁后才减少引用，因此淘汰器永远不会复用正在使用或等待的 buffer。

该设计允许同一磁盘块上的调用者发生合理串行化，也允许 cache miss 淘汰阶段短暂串行化；官方性能要求关注的是不同缓存块的高频命中与释放不应争用一把全局锁。

# 6. 实验结果与分析

## 6.1 锁争用统计

![Lab lock 锁争用统计](../screenshots/lab-lock-manual-run.png)

图 1　`kalloctest` 与 `bcachetest` 实际争用计数

`#test-and-set` 统计 `acquire()` 自旋循环中设置锁失败的次数。三个活跃 kmem 锁分别进行了大量获取，但失败次数均为 0，合计 `tot=0`。buffer cache 的 13 个桶中仅一个桶出现 6 次失败，合计 `tot=6`，远低于官方允许的 500。

专项测试均通过：`kalloctest test1/test2` 验证低争用与物理页完整性，`usertests sbrkmuch` 验证大地址空间分配，`bcachetest test0/test1` 验证低争用和超过 `NBUF` 后的替换路径。

## 6.2 官方评分

![Lab lock 官方评分结果](../screenshots/lab-lock-make-grade.png)

图 2　官方 `grade-lab-lock` 评分结果

官方脚本还执行完整 `usertests`，176.8 秒后输出 OK。最终各项得分为：kalloc 相关测试 30/30，bcache 相关测试 20/20，完整 usertests 19/19，time 1/1，合计 **70/70**。

# 7. 复现方法与实验总结

## 7.1 官方评分复现

```sh
wsl -d Ubuntu
cd /mnt/c/Users/Aemeath/Helper/2026/OS_Design/xv6-labs-2021
git switch lock
make clean
make grade
echo $?
```

通过标准是最后输出 `Score: 70/70`，且退出码为 0。由于锁争用计数受宿主负载影响，应尽量关闭其他高负载程序，并保留默认 `CPUS=3`。

手工查看详细统计：

```sh
make qemu
# 在 xv6 shell 中依次运行：
kalloctest
bcachetest
usertests sbrkmuch
```

编译兼容方面，当前 RISC-V 工具链需要明确使用 `rv64gc`，并对实验基线触发的新版本 GCC 警告进行兼容；评分库使用 `shlex.quote` 代替 Python 3 已移除的 `pipes.quote`。这些修改不改变锁算法和评分逻辑。

## 7.2 实验总结

本实验说明减少锁争用通常需要同时改变锁粒度和数据布局。物理页分配器通过 per-CPU ownership 把共享 freelist 转化为本地快速路径，只在资源不平衡时批量窃取；buffer cache 无法按 CPU 私有化，因此使用哈希桶分散不同磁盘块的元数据访问，并把全局锁限制在少见的 miss/eviction 路径。实现同时保持页面唯一归属、缓存块唯一副本、引用计数与 sleeplock 生命周期等不变量。最终 kmem 争用为 0、bcache 争用为 6，所有专项和回归测试通过，官方得分 70/70。
