# API 与所有权

公共声明见 `include/uhm/uhm.h`，默认平台 VM 与便捷 owner 包装器见 `include/uhm/uhm_platform.h`。当前 ABI 为 0.18，仍处开发阶段。结构体按 `size`、`abi_major`、`abi_minor` 协商；初始化时先清零，再设置这些字段，不复制内部结构或假设固定存储尺寸。

## 函数表

`uhm_heap`、`uhm_thread_ctx` 对外是不透明类型。库的 bootstrap 符号为 `uhm_abi_query`、`uhm_heap_storage_query`、`uhm_open`、`uhm_get_api`。业务操作从 `const uhm_api *api = uhm_get_api(heap)` 调用，表保存在内部 `heap->F` 中，各函数显式接收所属 heap。这种方式减少导出符号；它不是自动捕获 heap 的 C 闭包。

| 类别 | 函数表成员 | 约束 |
| --- | --- | --- |
| 对象分配 | `malloc/calloc/realloc/aligned_alloc/free/malloc_usable_size` | malloc 至少 16B 对齐；只处理同堆对象 |
| 映射 | `mmap/mmap_ex/munmap/mremap` | 独占页、匿名 arena 内映射，尺寸按系统页修正 |
| 保护 | `mprotect` | 根据页所有权和后端能力检查，不改变同页其他小对象 |
| 显式线程状态 | `thread_ctx_open/close/flush`、`malloc_ctx/free_ctx/free_deferred_ctx` | 存储由用户提供；ctx 必须与 heap 匹配 |
| 生命周期与观测 | `close/check/get_stats/enumerate_leaks/get_last_error` | close 后不得再使用其 heap、表或对象 |

头文件内的 `uhm_malloc` 等 static inline 包装也通过函数表调用。

## 两种初始化方式

默认方式调用 `uhm_platform_heap_open_default(&owner, &heap)`，由真实平台 reserve/commit 回调分配句柄存储与 arena。通过 `uhm_platform_heap_open(&config, &owner, &heap)` 可指定 arena 尺寸、缓存、锁策略等。结束时调用 `uhm_platform_heap_close(&owner)`；它先关闭 heap，再释放包装器持有的句柄存储。不要先独立关闭 heap 再重复关闭 owner。

完全自定义方式先查询 heap/ctx 存储大小与对齐，准备句柄存储、VM 回调表和 arena，然后调用 `uhm_open`。设置 `UHM_ARENA_PRE_RESERVED` 时 arena 由调用者预留并保留所有权，heap close 后由调用者决定释放；允许库 reserve 的 arena 则由 heap close 释放。句柄存储仍由调用者管理。平台包装器的 `page_size=0` 自动获取系统值；自定义回调必须准确表达真实后端的页尺寸、分配粒度与能力。

`uhm_platform_usermode_vm_ops_init` 提供逻辑 VM 模拟，不能保证物理页解绑、零填充或实际保护。若需要这些保证，应使用真实 VM 后端，不能拿系统 malloc arena 代替预留地址空间。

## 映射与保护

`mmap(heap, &addr, size, prot, flags)` 成功时回写地址。自动映射先复用空闲 extent，再从中间空闲区切出页；`UHM_MAP_FIXED` 与 `UHM_MAP_FIXED_NOREPLACE` 均受所有权检查，不覆盖 live malloc、live mmap 或控制带。当前接口不是 POSIX mmap 的全部兼容替代，不包含文件描述符、文件偏移或任意进程映射管理。

非零尺寸向上修正为系统页整数倍，修正溢出则失败。`munmap` 按同样规则修正尺寸；地址仍须合法且按页对齐。`mremap` 根据 old/new 修正后的页数决定是否保持、缩小、原址扩展或在 `UHM_REMAP_MAYMOVE` 下移动，失败时遵循事务回滚边界。旧地址在成功移动后失效。

`mprotect` 可以切分/合并 VMA。共享 slab 的单个 malloc 小对象不能单独设置页保护；page-private 分配按独占页规则处理。未拥有、未映射、跨 owner 或不支持的操作返回对应错误码；`UHM_ERR_UNSUPPORTED` 不能当作操作成功。Windows 严格 NUMA bind 也属于不支持能力。

## 线程、缓存与 NUMA

普通函数表支持多线程操作。ctx 适合线程独占使用，由用户提供存储并显式注册；跨线程释放走相应 remote 路径。默认包装器可启用 TLS 缓存，缓存以页数为预算在 min/max 边界内自适应；没有可变进程级 allocator 全局状态不意味着禁止 TLS。关闭时必须停止后续分配并确保调用者不再访问失效对象；close 的 BUSY/失败结果需要按生命周期契约处理，不能直接释放仍在使用的存储。

尽量保留用户首次接触的数据页分配机会；已有物理页复用、OS 策略与 CPU 调度可能影响最终亲和性。开启 NUMA 使用时，`mmap_ex` 的 `numa_policy/numa_node` 表达映射级请求，malloc 的 heap policy 表达推荐。实际 preferred/bind 能力从 VM capability 判断：Windows preferred、Linux/WSL preferred/bind 都受运行环境与系统权限影响。不要把逻辑后端的 hint 或单节点测试等同于真实多节点 placement 验证。
