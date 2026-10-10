# UHMAS

UHMAS 是一个仍在开发的 C11 堆内存管理系统。它在同一预留虚拟地址空间中提供 `malloc` 与 `mmap` 两类接口，通过用户提供的 VM 函数表适配平台，并支持默认平台包装器。

管理状态属于各个 `uhm_heap`，没有可变的进程级 allocator 全局状态。内部 `heap->F` 保存操作函数表，对外用 `uhm_get_api(heap)` 取得表，随后通过函数指针调用；C 接口仍显式传入 heap。可选 TLS 缓存与显式 `uhm_thread_ctx` 均有堆内生命周期管理。

当前重点是稳定性、并发正确性和性能改进。Windows x64/Win32 与 WSL x64/i386 已有验证记录；macOS/Android 真实平台验证及嵌入式能力分层受环境限制无限期延期。当前小对象、多线程和大块路径仍慢于系统实现，详见 [测速说明](docs/BENCHMARKS.md)。

## 构建

需要 CMake 3.20+、C11 编译器。Windows 开发构建使用 Visual Studio 2022 的 C 工具链：

```powershell
cmake -S . -B build-dev -G "Visual Studio 17 2022" -A x64 -DUHMAS_BUILD_PROFILE=dev -DUHMAS_BUILD_EXAMPLES=ON
cmake --build build-dev --config Release
ctest --test-dir build-dev -C Release --output-on-failure
./build-dev/Release/uhmas_example.exe
```

Linux/WSL 在 Linux shell 中执行：

```sh
cmake -S . -B build-dev-linux -DCMAKE_BUILD_TYPE=Release -DUHMAS_BUILD_PROFILE=dev -DUHMAS_BUILD_EXAMPLES=ON
cmake --build build-dev-linux -j2
ctest --test-dir build-dev-linux --output-on-failure
./build-dev-linux/uhmas_example
```

`dev` profile 包含白盒测试与诊断，不代表生产开销。单独建立无诊断库：

```sh
cmake -S . -B build-production -DCMAKE_BUILD_TYPE=Release -DUHMAS_BUILD_PROFILE=release
cmake --build build-production --config Release
```

请使用不同构建目录。CMake 已缓存的选项不会仅因切换 profile 自动重置。

## 使用

[examples/basic.c](examples/basic.c) 使用真实平台 VM 初始化默认堆，演示函数表 `malloc/free`、页粒度 `mmap/mprotect/munmap` 和正确的 owner 关闭顺序。

```c
#include "uhm/uhm_platform.h"

uhm_platform_heap owner;
uhm_heap *heap = NULL;
if (uhm_platform_heap_open_default(&owner, &heap) == UHM_OK) {
    const uhm_api *api = uhm_get_api(heap);
    void *p = api->malloc(heap, 64);
    api->free(heap, p);
    uhm_platform_heap_close(&owner);
}
```

默认包装器负责预留真实 VM arena 和初始句柄存储。自定义接入可用 `uhm_heap_storage_query`、`uhm_vm_ops`、`uhm_config` 与 `uhm_open` 控制所有权；不能对普通系统 malloc 获得的共享堆页执行本系统的物理回收或保护操作。

## 文档

- [API 与所有权](docs/API.md)
- [内存布局与并发架构](docs/ARCHITECTURE.md)
- [验证范围与命令](docs/VALIDATION.md)
- [测速、供电差异与证据说明](docs/BENCHMARKS.md)
- [开放任务与边界](docs/ROADMAP.md)
- [完整计划](PLAN.MD) 和 [逐项待办](TODO.MD)
- [首发与隐私检查](docs/PUBLISH.md)

## 许可证

当前未添加许可证，公开代码不等于已授予开源许可证下的使用、修改或再分发权限。许可证由项目所有者后续决定。
