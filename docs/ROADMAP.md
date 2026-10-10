# 开放任务与边界

逐项状态以 `TODO.MD` 为准，架构和阶段证据以 `PLAN.MD` 为准。发布快照准备完成不代表 allocator 功能或性能已全部验收。

- 优先改善 small/MT/large 热路径，控制锁持有、VM 系统调用、decommit/recommit、全局一致性写和 metadata 维护成本；目前仍慢于系统实现。
- 完成 32 位 native/index queue、FULL/EMPTY/cached reservation 状态转换及公开 capability 契约；现有 PARTIAL raw-ready 批量复用不等于完整加速方案。
- 继续 treap deterministic worst-case fallback、metadata drift/fail-fast 与 physical-chain fallback 淘汰，不能用诊断预算门替代实现。
- 完成 cache-line 布局、side metadata/compact slab header 的多线程 A/B 和占用评估；已经分片的统计不重复当成未实现病灶。
- 完成诊断裁剪审计、异常慢档归因、各 focused profile 的正式性能证据；供电变化下保留同轮系统对照。
- macOS/Android 真机验证、嵌入式/纯用户态后端能力分层受当前环境限制无限期延期，兼容代码不宣传成已验证。

**持续门禁保持打开**：每个阶段及时 Git 提交，检查 PLAN/TODO 严格 UTF-8、无 BOM/替换字符/已知乱码，并人工抽查中文与执行 `git diff --check`。项目正式完工前，此 TODO 不允许勾选。
