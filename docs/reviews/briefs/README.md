# AI 任务书（briefs）—— 不进仓库，完成即清除并归档

本目录最终只保留这份说明。任务书本体一律放在仓库外：`~/EMlink-briefs/`。

> 用户 2026-10-17 明确：任务书不提交、只放本地作参考，不要污染仓库。
> 关键理由：git 里凡是出现过的文件都会留下修改记录，留着任务书会污染后续读历史/读 diff 的上下文（对人和 AI 都一样）。


## 目录约定（仓库外）

| 位置 | 含义 |
|---|---|
| `~/EMlink-briefs/<ID>_brief.md` | 进行中的任务书 |
| `~/EMlink-briefs/done/<ID>_brief.md` | 已完成（代码验收通过）→ 立即移到这里归档 |
| `~/EMlink-briefs/recovered/` | 从 git 历史里恢复出来的旧任务书（A1–A7、B1–B3d、P0/P1 系列等，仅作参考） |

三个位置对应任务书的三个阶段：在做、做完、考古。


## 规矩

1. 写在哪：`~/EMlink-briefs/<ID>_brief.md`（仓库外）。命名用短 ID（例：`S1_sdr_frame_parser_brief.md`）。
2. 完成即清除并归档：该项代码验收通过后，立刻把它移到 `~/EMlink-briefs/done/`；仓库里不留任何副本。
3. 绝不 `git add` 任务书。本目录（仓库内 `docs/reviews/briefs/`）只放这份说明。
4. 调用 OpenCode 时怎么办：OpenCode 沙箱只允许读仓库内路径，所以调用前临时复制进去、跑完立刻删：
   ```bash
   cp ~/EMlink-briefs/<ID>_brief.md docs/reviews/briefs/_active.md
   bash ~/.local/bin/oc-deepseek '<调度提示>' -f docs/reviews/briefs/_active.md
   rm -f docs/reviews/briefs/_active.md
   ```
   `.gitignore` 已忽略本目录下除 `README.md` 以外的所有文件，`git status` 不会被它污染。
5. 要留的是结果，不是施工单：
   - 改了什么 / 为什么 / 怎么验证的 → `../FIX_LOG.md`（权威记录）
   - 现在的代码长什么样 → 代码本身 + 各模块 `.md` 的「实现架构」章
   - 平台的坑与硬约束 → `../../references/`（例：`LLCC68_E220-900M22S.md`、`SDR_LORA_MONITOR.md`）

第 4 条是这套流程唯一的操作要点，其余都是纪律问题。


## 任务书模板

1. 背景：贴原代码片段 + 说清后果（为什么非改不可）
2. 允许改动的文件：白名单逐条列，其它一律不动
3. 逐条改法：到函数 / 行 / 新代码原文（跨仓库的参考代码要贴进任务书，因为 OpenCode 读不到仓库外）
4. 自检问题：3~5 个「为什么」，逼 AI 讲清依据
5. 完成后报告：改动摘要 + 自检结论 + 诚实列出「本次改动不能保证什么」

调度时固定附加这句：

> *只允许改动任务书列出的文件；不要改任何 `.md`；不要跑 `pio`/编译；不要 `git add/commit`；只在仓库目录内活动（不要读 `/tmp`、`~/.platformio` 等仓库外路径）。*

五条模板里第 3 条最影响成功率：改法写到行，AI 才不会自由发挥。


## 环境提醒

- 命令：`bash ~/.local/bin/oc-deepseek '<调度提示词>' -f docs/reviews/briefs/_active.md`
- 任务书里如果需要引用仓库外的代码（官方示例、系统头文件），必须把相关片段抄进任务书 —— OpenCode 读不到仓库外，只能靠任务书自包含。
