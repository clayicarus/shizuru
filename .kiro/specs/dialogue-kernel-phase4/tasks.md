# 实现计划：对话内核 Phase 4 — Agenda 与混合主动

## 概述

Phase 4 引入 agenda 机制，让 agent 能够延迟处理系统提醒、在执行危险操作前
请求用户确认、在对话空闲时主动提起待办事项。实现按子能力分步推进，每步保持
编译和测试通过。

**硬范围：** agenda 类型 + 新事件/效果 + conversation idle timer + delayed
reminder + clarification + followup + 测试。

**不在 Phase 4：** UI 确认组件、agenda 持久化（SQLite）、proactive message
的触发策略（仅定义效果类型，不实现触发逻辑）。

## 任务

- [ ] 1. 添加 agenda 类型到 `core/dialogue/types.h`
  - [ ] 1.1 定义 `AgendaKind` 枚举
    - 值：`kClarification`、`kFollowup`、`kReminder`、`kProactive`
    - _需求：1.5_
  - [ ] 1.2 定义 `AgendaItem` 结构体
    - 字段：`id`（string）、`kind`（AgendaKind）、`topic`（string）、
      `priority`（int，默认 0）、`deadline`（optional time_point）、
      `context`（string）
    - _需求：1.4_
  - [ ] 1.3 定义 `AgendaState` 结构体
    - 字段：`pending_items`（vector of AgendaItem）、
      `active_item`（optional AgendaItem）
    - _需求：1.2, 1.3_
  - [ ] 1.4 在 `DialogueState` 中添加 `agenda` 字段
    - 类型：`AgendaState`，默认初始化（空）
    - _需求：1.1_
  - [ ] 1.5 添加 `kAwaitingClarification` 到 `DeliberationPhase`
    - _需求：2.1_
  - [ ] 1.6 添加 Phase 4 事件类型
    - `ClarificationReceived{item_id, approved, user_response, now}`
    - `AgendaItemDue{item_id, now}`
    - `ConversationIdleCheck{now}`
    - 更新 `DialogueEvent` 变体
    - _需求：7.1, 7.2, 7.3, 7.4_
  - [ ] 1.7 添加 Phase 4 效果类型
    - `RequestClarification{question, pending_action_id}`
    - `EnqueueAgendaItem{item}`
    - `DequeueAgendaItem{item_id}`
    - `EmitProactiveMessage{content, source}`
    - 更新 `DialogueEffect` 变体
    - _需求：8.1, 8.2, 8.3, 8.4, 8.5_
  - [ ] 1.8 在 `ControllerConfig` 中添加 `conversation_idle_check` 字段
    - 类型：`std::chrono::seconds`，默认 10s
    - _需求：6.5_

- [ ] 2. 检查点 — 验证扩展类型编译通过
  - 构建项目，确认无编译错误
  - 确认所有现有测试通过（类型因默认初始化向后兼容）

- [ ] 3. 启用 Conversation Idle Timer
  - [ ] 3.1 在 `HandleResponding` 完成后调度 idle timer
    - 在 `DeliverResponse` 效果执行后，Controller 调度
      `ScheduleTimer{kConversationIdle, "idle", now + config_.conversation_idle_check}`
    - _需求：6.2_
  - [ ] 3.2 在用户输入到达时取消 idle timer
    - 在 RunLoop 处理 `AggregationComplete` / `UserMessageReceived` 前，
      调用 `timer_book_.Cancel("idle")`
    - _需求：6.3_
  - [ ] 3.3 在 `HandleTimerExpired` 中处理 `kConversationIdle`
    - 构造 `ConversationIdleCheck{now}` 事件，调用 Reduce
    - _需求：6.4_
  - [ ] 3.4 实现 `HandleConversationIdleCheck` reducer handler
    - 如果 agenda 为空：返回 no-op（状态不变，效果为空）
    - 如果 agenda 有到期事项：取出最高优先级到期事项，产生相应效果
    - 重新调度 idle timer（通过 `ScheduleTimer` 效果）
    - _需求：6.4_
  - [ ] 3.5 更新 `Reduce` dispatch — 添加 `ConversationIdleCheck` 分发
    - _需求：7.4_

- [ ] 4. 检查点 — 验证 idle timer 工作
  - 构建项目，运行所有测试
  - Idle timer 在 agenda 为空时应为 no-op，不影响现有行为

- [ ] 5. 实现 Delayed Reminder
  - [ ] 5.1 修改 `HandleSystemEvent` — 忙时延迟到 agenda
    - 当 `deliberation != kIdle` 时：不再 `RecordMemory` + `StartLlm`，
      改为 `EnqueueAgendaItem{kReminder, reminder_content, priority=50}`
    - 当 `deliberation == kIdle` 时：行为不变（立即 RecordMemory + StartLlm）
    - _需求：3.1_
  - [ ] 5.2 更新 `HandleConversationIdleCheck` — 处理到期 reminder
    - 检查 agenda 中 `kReminder` 类型的事项
    - 如果有：取出，产生 `RecordMemory` + `StartLlm` 效果
    - _需求：3.2, 3.3_
  - [ ] 5.3 实现 `EnqueueAgendaItem` 效果处理器
    - 将 item 插入 `dialogue_state_.agenda.pending_items`，保持 priority 排序
    - _需求：8.2_
  - [ ] 5.4 实现 `DequeueAgendaItem` 效果处理器
    - 从 `pending_items` 或 `active_item` 中移除指定 id 的条目
    - _需求：8.3_

- [ ] 6. 检查点 — 验证 delayed reminder 工作
  - 构建项目，运行所有测试
  - 验证：忙时 reminder 入队 agenda，空闲时触发
  - 验证：空闲时 reminder 仍然立即触发（行为不变）

- [ ] 7. 实现 Clarification 机制
  - [ ] 7.1 修改 `HandleLlmCompleted` — tool call 需确认时进入 clarification
    - 在 `EmitToolCallFrames` 效果中添加 `needs_approval` 标志
    - 或者：在 `EmitToolCallFrames` 效果执行器中检查 policy，如果
      `kRequireApproval` 则不执行 tool call，而是回馈
      `ClarificationNeeded` 事件给 reducer
    - **设计决策：** 由于 reducer 不能调用 policy（纯度），approval 检查
      在效果执行器中进行。如果需要确认，效果执行器构造
      `ClarificationReceived`-like 事件回馈给 reducer。
    - 实际实现：在 `EmitToolCallFrames` 效果执行器中，如果 policy 返回
      `kRequireApproval`，不执行 tool call，而是：
      1. 构造 AgendaItem{kClarification, pending_action}
      2. 将 `EnqueueAgendaItem` + `RequestClarification` 作为内部事件
         回馈给 reducer
    - _需求：2.1, 2.2, 2.3_
  - [ ] 7.2 实现 `HandleClarificationReceived` reducer handler
    - 如果 `approved=true`：从 agenda 取出 pending action，产生
      `EmitToolCallFrames` 效果，设置 `deliberation = kAwaitingToolResults`
    - 如果 `approved=false`：从 agenda 移除，记录拒绝，设置
      `deliberation = kIdle`
    - _需求：2.5, 2.6, 2.7_
  - [ ] 7.3 处理 clarification 期间的新消息
    - 如果 `deliberation == kAwaitingClarification` 且收到
      `UserMessageReceived`：取消当前 clarification，清除 active_item，
      正常处理新消息
    - _需求：2.8_
  - [ ] 7.4 实现 `RequestClarification` 效果处理器
    - 通过 `EmitConversationItem` 向 UI 发送确认请求
    - 确认请求作为 assistant message，内容为 question
    - _需求：2.4_
  - [ ] 7.5 实现 Controller 中 clarification 回复的识别
    - 方案 A：UI 通过专用 API 发送确认/拒绝（需要 bridge 扩展）
    - 方案 B：用户的下一条消息自动作为 clarification 回复
    - **选择方案 B**（更简单）：当 `deliberation == kAwaitingClarification`
      时，下一条用户消息被解释为 clarification 回复。Controller 构造
      `ClarificationReceived` 事件而不是 `UserMessageReceived`。
      `approved` 的判断可以简单地基于关键词（"好"、"确定"、"是" → true，
      其他 → false），或者调用 LLM 分类器。初始实现用关键词匹配。
    - _需求：2.5_
  - [ ] 7.6 更新 `Reduce` dispatch — 添加 `ClarificationReceived` 分发

- [ ] 8. 检查点 — 验证 clarification 工作
  - 构建项目，运行所有测试
  - 验证：需确认的 tool call → 确认请求 → 用户确认 → 执行
  - 验证：需确认的 tool call → 确认请求 → 用户拒绝 → 放弃
  - 验证：需确认的 tool call → 确认请求 → 用户说别的 → 取消 + 处理新消息

- [ ] 9. 实现 Followup 跟进
  - [ ] 9.1 在 tool result 处理中检测 followup 创建
    - 当 tool call 是 `save_followup` 且结果成功时，从结果中提取
      followup 信息，构造 `EnqueueAgendaItem{kFollowup, topic, deadline}`
    - _需求：4.1, 4.2_
  - [ ] 9.2 更新 `HandleConversationIdleCheck` — 处理到期 followup
    - 检查 agenda 中 `kFollowup` 类型且 deadline <= now 的事项
    - 如果有：取出，产生 `RecordMemory` + `StartLlm` 效果
    - _需求：4.3, 4.4_
  - [ ] 9.3 Followup 完成后从 agenda 移除
    - 当 LLM 调用 `complete_followup` tool 时，产生
      `DequeueAgendaItem{followup_id}` 效果
    - _需求：4.5_

- [ ] 10. 检查点 — 验证 followup 工作

- [ ] 11. 编写 Phase 4 单元测试
  - [ ] 11.1 创建 `tests/agent/dialogue_reducer_phase4_test.cpp`
    - 测试 `HandleConversationIdleCheck`：agenda 为空 → no-op
    - 测试 `HandleConversationIdleCheck`：有到期 reminder → StartLlm
    - 测试 `HandleSystemEvent`：忙时 → EnqueueAgendaItem
    - 测试 `HandleSystemEvent`：空闲时 → 行为不变
    - 测试 `HandleClarificationReceived`：approved=true → EmitToolCallFrames
    - 测试 `HandleClarificationReceived`：approved=false → kIdle
    - 测试 `HandleUserMessage` 在 kAwaitingClarification → 取消 clarification
    - 测试 agenda 优先级排序
    - _需求：12.1, 12.2, 12.3, 12.4_
  - [ ] 11.2 编写 property test
    - 属性：Reducer 纯度（扩展覆盖 Phase 4 事件和状态）
    - 属性：Agenda 不变量（kAwaitingClarification → active_item 有值）
    - 属性：Delayed reminder 不丢失
    - 属性：Idle check 幂等
    - _需求：12.5_

- [ ] 12. 更新测试构建系统
  - [ ] 12.1 将 `dialogue_reducer_phase4_test.cpp` 添加到
    `dialogue_reducer_test` 目标

- [ ] 13. 检查点 — 验证所有测试通过

- [ ] 14. 更新架构文档
  - [ ] 14.1 更新 `.kiro/steering/architecture.md`
    - 在 dialogue/ 模块职责中添加 agenda 描述
    - 更新实现状态为 Phase 4 完成
  - [ ] 14.2 更新 `.kiro/steering/dialogue-kernel.md`
    - 标记 Phase 4 为已实现

- [ ] 15. 最终检查点 — 确保所有测试通过

## 注意事项

- Phase 4 的所有新能力在 agenda 为空时不影响现有行为（向后兼容）
- `EnqueueAgendaItem` 和 `DequeueAgendaItem` 效果处理器操作
  `dialogue_state_.agenda`，与 Phase 3 的 workspace 效果处理器模式一致
- Clarification 的 `approved` 判断初始实现用关键词匹配，后续可升级为
  LLM 分类器
- Followup 依赖 `save_followup` / `complete_followup` tool 的实现
  （当前在 ROADMAP 中标记为待做），Phase 4 只定义 agenda 侧的逻辑
- `EmitProactiveMessage` 效果类型在 Phase 4 中定义但不实现触发逻辑，
  为 Phase 5 或产品层预留
- Agenda 状态是 session 级内存数据，不持久化。持久化在 SQLite MemoryStore
  完成后再处理
