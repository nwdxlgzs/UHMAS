# 首发步骤

本次采用干净快照重新建立 Git 历史。开发仓库的旧作者信息、历史路径、构建结果和恢复档案继续留在本地，不能把开发仓库的全部 branch/tag 推到首发仓库。

## 材料与导出

在开发仓库根目录先将本阶段审查过的文件纳入 Git，再运行：

```powershell
python scripts/export_publication.py
```

默认导出到 `draft/publish/`。工具只复制已跟踪的源码/测试/脚本、当前 PLAN/TODO、脱敏 JSON 和本目录的首页/文档模板；它不复制旧 `.git`。目标非空时拒绝覆盖，可使用 `--output` 指定新的空目录。输出目录已由外层 `.gitignore` 忽略。

生成目录已经是独立 Git 仓库：`main` 分支、单个中性身份首提交、无 remote。`PUBLICATION.json` 记录文件清单与 SHA-256，报告不包含开发目录绝对路径或旧作者原值。后续修改若准备公开，仍须及时提交并重新审计。

## 最后检查

进入导出的目录，检查工作树与公开历史：

```powershell
git status --short
git log --format="%h %an <%ae> %s"
python scripts/publication_privacy.py --tracked --history
```

作者/提交者应为 `UHMAS contributors <contributors@example.invalid>`。此身份仅用于去个人化的初始快照，不用于宣称贡献者身份。模式审计只能检查定义的规则，还要人工检查新材料中的联系人、截图、日志和运行环境；本次导出不含这些额外附件。

按 README 从快照独立构建，确认没有依赖开发仓库的 ignored 文件。新生成的日志/benchmark JSON 可能重新包含本机路径，公开前先脱敏，不直接强制 add。Git 历史中的个人信息不能靠工作树删行消除，因此首发采用新历史。

## 创建远程仓库后

在平台上创建空仓库，不自动创建 LICENSE/README。进入发布目录后，用你实际选择的远程地址执行：

```powershell
git remote add origin <REMOTE_URL>
git push -u origin main
```

`<REMOTE_URL>` 是需要自行替换的占位符。平台账号、仓库命名、远程访问权限及后续提交署名由发布者决定；本阶段未创建远程仓库或 push。不要在开发仓库执行 `push --all` 或 `push --mirror`。

## 许可证

按当前选择暂不添加 LICENSE，也不声明 MIT/Apache 等授权。后续决定许可证时，再明确授权范围并单独提交。
