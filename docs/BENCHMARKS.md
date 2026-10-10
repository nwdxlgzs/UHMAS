# 测速与证据说明

当前性能仍有明显改进空间，尤其是 small/MT 路径。以下为 2026-10-10 Windows x64 Release、ordinary quick 三样本的同轮记录；诊断功能编译模式与运行是否启用要区别看待。这不是无诊断生产库的普遍性能承诺。

| 场景 | UHM | 系统对照 | UHM / 系统耗时 |
| --- | ---: | ---: | ---: |
| small malloc/free | 390.4 ns | 98.4 ns | 3.97x |
| MT small | 258.0 ns | 63.7 ns | 4.05x |
| large realloc/free | 745.2 ns | 521.7 ns | 1.43x |
| mmap/munmap | 12.569 us | 11.069 us | 1.14x |
| fixed/auto | 6.462 us | — | — |
| mprotect | 5.373 us | — | — |
| collision | 10.846 us | — | — |
| release mixed | 6.849 us | — | — |

来源为 `artifacts/verification/publication-20261010.json` 与 PLAN 0.60。发布整理复测使用 `-NoRegressionBaseline`，不宣称历史性能回归通过；x64 benchmark `.text` SHA 与 PLAN 0.59 完全相同。用户已确认不同插电环境功率上限不同，跨轮绝对耗时仅作参考，主要使用同轮系统对照和结构/正确性证据判断实现影响。每次测量未记录瓦数，不能倒推出全部历史差值都由功率变化造成。

同时保留 treap 路径验证的 1/420 diagnostic failure：heap-lock max hold 1.857ms，门槛 1ms。当时并行快照构建，尚未证明具体原因，不放宽阈值或反复重跑；自动路径与参数转发随后由双平台 metadata 场景独立验证，throughput baseline 缺失仍明确跳过。

测试条件应同时记录架构、编译选项、诊断模式、样本数量、工作负载和基线比较状态。不要删除慢档负样本、不断复跑直到偶然通过，或将只有结构门通过的结果宣传为 throughput 回归闭环。

## 历史证据脱敏

公开保留的历史 JSON 将本机绝对路径替换为 `<PROJECT_ROOT>` / `<USER_HOME>`，并添加 `publication_redaction`。原数值未改；既有 binary/source/workload 等 provenance 哈希仍指向原测量输入，不代表脱敏文件本身的字节校验。脱敏前后文件 SHA 在 `publication-redaction-20261010.json` 中记录。

这些数据作为历史参考，不能凭脱敏重新晋升为当前提交的正式基线。正式比较仍应检查 workload/provenance 合约。部分历史原始日志和文档恢复备份仅保留在开发仓库，公开快照不包含所有 PLAN 中提及的原始 artifact。
