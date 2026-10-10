# 内存布局与并发架构

整体架构和逐阶段证据以 `PLAN.MD` 为准。下图表达地址方向；实际 ownership 与复用由 page map、extent/VMA 索引共同维护。

```text
低地址                                                       高地址
| 控制带 | malloc slab / large extent -> | 碰撞空闲区 | <- mmap VMA |
```

控制元数据从低地址侧自举，按需扩展并回收描述符。malloc 优先复用 size class、slab 空槽与大块 free extent；mmap 优先复用 mmap free extent 和有界近期/尺寸提示缓存。双向水位负责尚未划出的空间，不能用水位代替对 hole、FIXED 请求或跨派借用的 ownership 检查。

中间“碰撞空闲区”是统一的空闲资源来源。分配前先验证溢出、对齐、owner 和可提交范围，预留元数据；发布必须与 VM 成败一致。回滚失败或 metadata drift 需要明确暴露健康状态，不能伪装成成功。控制页、malloc 页和 mmap 页的物理页所有权彼此隔离；地址邻接不允许共享同一物理页。

释放包含两个层面：逻辑空闲资源归还，以及按预算回收物理页。Windows 使用 decommit 保留 reserve，POSIX 使用真实 VM 回调的 `madvise/mprotect` 或原址重映射策略。回收能力、不可访问与零填充分别表达；后端不能保证的属性不能仅靠 no-op 回调假装完成。malloc 允许短期保留可复用提交页，减少 decommit/recommit 风暴；mmap 回收保持 arena 地址资源供下次使用。

并发采用按能力选择的快路径和锁内慢路径组合。可用平台支持 atomics、remote queue、page map 分片和统计分片；复杂 extent/VMA 操作、物理页提交及回滚有明确锁和事务边界。部分 VM 操作已实现锁外事务，整体锁持有窗口和尾延迟仍是开放优化项，不能描述成全部无锁。

32 位不以“缺少 lock-free 64-bit atomic”为由放弃所有加速。目前有 native-width treap priority、per-class raw-ready 队列及 PARTIAL slab 的 raw-lock 内批量复用；完整状态转换、native/index tagged queue 和稳定性能验收仍未完成。运行时 capability 必须反映真正可用的路径。

元数据索引使用 treap、page map 与有界缓存，包含退化诊断、健康快检及部分 repair 策略。已经完成的 focused 深度/步数/延迟门不等于数学意义的全场景 worst-case 保证。完整 deterministic fallback、metadata 回收预算、伪共享布局与小对象 header 紧缩继续按 TODO 推进。
