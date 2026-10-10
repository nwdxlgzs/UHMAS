# 验证范围与命令

主验证平台为 Windows x64/Win32 和 WSL x64/i386。当前仓库保留阶段性回归摘要；每个摘要明确当轮运行范围，不能把某阶段结果当作所有后续提交的验证。macOS/Android 兼容代码尚无真实平台验证，嵌入式/纯用户态能力分层因环境限制无限期延期。

## 功能构建

README 中的 `dev` 构建包含 `uhmas_basic_tests` 和 `uhmas_platform_vm_tests`，分别覆盖分配/复用/并发/元数据故障及真实 VM 行为。测试获取运行时页尺寸，使用 VM reserve 的 arena；逻辑后端仅用于声明范围内的模拟。

Win32 使用独立构建目录：

```powershell
cmake -S . -B build-dev-win32 -G "Visual Studio 17 2022" -A Win32 -DUHMAS_EXPECT_32BIT=ON -DUHMAS_BUILD_PROFILE=dev
cmake --build build-dev-win32 --config Release
ctest --test-dir build-dev-win32 -C Release --output-on-failure
```

WSL i386 需要已安装对应的 32 位编译器与 libc 开发文件：

```sh
cmake -S . -B build-dev-i386 -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-m32 -DUHMAS_EXPECT_32BIT=ON -DUHMAS_BUILD_PROFILE=dev
cmake --build build-dev-i386 -j2
ctest --test-dir build-dev-i386 --output-on-failure
```

完整双位宽门由 `scripts/verify_32bit_gate.ps1` 管理；可以用 `-SkipWindows` 或 `-SkipWsl` 限定范围。

## 性能与诊断

Windows 根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_benchmarks.ps1 -Platform windows -Samples 3 -NoRegressionBaseline
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify_benchmark_gate.ps1 -Platform both -ProfileSet treap-adversarial -Samples 3
```

runner 默认目录为 `build-release` / `build-wsl-release`，需要时自动构建；现成构建可用 `-SkipBuild`。自动 WSL 路径由当前 checkout 的 `wslpath` 推导，也支持 `-WslProject` 与 `-WslDistro` 显式覆盖，不携带原作者本机路径。Linux shell 可直接运行构建中的 `uhmas_benchmarks`。

`-NoRegressionBaseline` 明确只生成当前遥测，不能宣称历史 regression 通过。focused profile 中诊断、metadata、延迟和 throughput comparison 是独立门；输出 `comparison_skipped=true` 或 missing baseline 时须保留此限制。

## Sanitizer

WSL ASAN/UBSAN 与 TSAN 使用 `verify_wsl_asan.ps1/.sh` 和 `verify_wsl_tsan.ps1/.sh`。需要对应工具链/runtime，不能用 timeout 124、重复 DEADLYSIGNAL 或 missing runtime 代替测试成功。仓库对 WSL ASAN 使用 non-PIE 避免 shadow reservation 冲突；TSAN 仍受运行环境地址空间策略影响，具体启动检查以脚本为准。

发布材料准备阶段还要从干净快照单独构建与运行测试/示例，防止依赖本地 ignored 文件。`PUBLICATION.json` 保存导出文件的 SHA-256；源码审计、Git 历史审计和运行测试分别提供证据。
