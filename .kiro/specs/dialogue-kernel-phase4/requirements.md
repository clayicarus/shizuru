# 需求文档：对话内核 Phase 4 — Agenda 与混合主动

## 简介

Phase 4 为对话内核引入 agenda（议程）机制和混合主动行为。在此之前，agent
完全是被动的：用户说话 → agent 回复。Phase 4 之后，agent 能够：

- 在执行危险操作前主动向用户确认（clarification）
- 记住待办事项并在合适时机主动提起（followup）
- 智能调度系统提醒，不在用户说话时粗暴打断（delayed reminder）
- 在一个 turn 内显式跟踪多步骤任务进度（agent-led turn）

**范围边界：** Phase 4 不涉及 UI 层的确认交互组件（按钮、卡片等），只定义
内核侧的事件、状态和效果。UI 层通过现有的 `ConversationItem` 和
`ActivityEvent` 回调接收 agent 的确认请求和主动消息，具体渲染由 UI 自行
实现。Phase 4 也不涉及持久化存储（SQLite）——agenda 状态暂时是 session 级
内存数据，持久化在后续阶段处理。

**行为连续性：** Phase 4 的所有新能力默认关闭或向后兼容。不启用 agenda
功能时，现有行为完全不变。

## 术语

- **Agenda**：一个优先级队列，存放 agent 需要在未来某个时刻处理的事项。
  每个事项有类型、优先级、截止时间和关联上下文。
- **AgendaItem**：agenda 中的单个条目。类型包括 `kClarification`（确认）、
  `kFollowup`（跟进）、`kReminder`（提醒）、`kProactive`（主动发起）。
- **Clarification**：agent 在执行操作前向用户提问以确认意图。用户回复后
  agent 决定执行或放弃。
- **Mixed-Initiative**：agent 和用户都可以发起对话 turn，而不是只有用户
  能发起。
- **Proactive Message**：agent 主动发出的消息，不是对用户输入的回复。
- **Conversation Idle**：对话空闲状态，一段时间内没有用户输入也没有 agent
  活动。Phase 2 已声明 `TimerKind::kConversationIdle` 但未使用，Phase 4
  启用它。

## 需求

### 需求 1：AgendaState 子状态

**用户故事：** 作为核心开发者，我希望 DialogueState 包含一个 agenda 子状态，
以便 reducer 能够跟踪待处理事项并在合适时机触发它们。

#### 验收标准

1. DialogueState 应包含 `agenda` 字段，类型为 `AgendaState`
2. AgendaState 应包含 `pending_items`（`std::vector<AgendaItem>`）
3. AgendaState 应包含 `active_item`（`std::optional<AgendaItem>`），
   表示当前正在处理的 agenda 事项
4. AgendaItem 应包含 `id`（string）、`kind`（AgendaKind 枚举）、
   `topic`（string）、`priority`（int）、`deadline`（optional time_point）、
   `context`（string，关联上下文如 pending action JSON）
5. AgendaKind 枚举应包含 `kClarification`、`kFollowup`、`kReminder`、
   `kProactive`
6. pending_items 应按 priority 降序排列（高优先级在前）

### 需求 2：Clarification 机制

**用户故事：** 作为用户，我希望 agent 在执行危险操作前先问我确认，而不是
直接执行。

#### 验收标准

1. 当 LLM 返回 tool call 且 policy 标记为需要确认时，reducer 应进入
   `DeliberationPhase::kAwaitingClarification` 状态
2. Reducer 应将 pending action 存入 agenda 作为 `kClarification` 条目
3. Reducer 应产生 `RequestClarification{question, pending_action_id}`
   效果
4. 效果执行器应通过 `EmitConversationItem` 向 UI 发送确认请求
5. 当用户回复时（`ClarificationReceived{item_id, approved}` 事件），
   reducer 应根据 `approved` 决定执行或放弃 pending action
6. 如果 approved=true，reducer 应产生 `EmitToolCallFrames` 效果并
   恢复正常 tool call 流程
7. 如果 approved=false，reducer 应产生 `RecordMemory`（记录拒绝）并
   回到 `kIdle`
8. 如果用户在等待确认期间发送了新的无关消息，reducer 应取消当前
   clarification 并处理新消息

### 需求 3：Delayed Reminder 调度

**用户故事：** 作为用户，我希望系统提醒不会在我说话时打断我，而是等我说完
再自然地提起。

#### 验收标准

1. 当 `SystemEventReceived` 到达且 `deliberation != kIdle` 时，reducer
   应将提醒存入 agenda 作为 `kReminder` 条目，而不是立即 `StartLlm`
2. 当对话进入空闲状态（`TimerExpired{kConversationIdle}`）时，reducer
   应检查 agenda 中是否有到期的 reminder
3. 如果有到期 reminder，reducer 应产生 `StartLlm` 效果，将 reminder
   内容注入 context
4. 如果没有到期 reminder，reducer 应保持 `kIdle`
5. `kConversationIdle` timer 应在每次 turn 完成后由 reducer 调度
   （`ScheduleTimer{kConversationIdle, "idle", deadline}`）
6. 用户输入应取消 idle timer（`CancelTimer{"idle"}`）

### 需求 4：Followup 跟进

**用户故事：** 作为用户，我希望 agent 能记住它承诺的跟进事项，并在合适时机
主动提起。

#### 验收标准

1. 当 LLM 调用 followup 相关 tool（如 `save_followup`）时，reducer 应
   将 followup 存入 agenda 作为 `kFollowup` 条目
2. Followup 条目应包含 deadline（到期时间）
3. 当 followup 到期且对话空闲时，reducer 应产生 `StartLlm` 效果，
   将 followup 内容注入 context
4. Followup 的触发逻辑与 delayed reminder 共享同一个 idle timer 检查路径
5. 已完成的 followup 应从 agenda 中移除

### 需求 5：Proactive Message

**用户故事：** 作为核心开发者，我希望 reducer 能产生主动消息效果，以便
agent 在没有用户输入的情况下发起对话。

#### 验收标准

1. DialogueEffect 变体应包含 `EmitProactiveMessage{content, source}`
2. 效果执行器应通过 `EmitConversationItem` 向 UI 发送主动消息
3. 主动消息应记录到 committed history
4. 主动消息不应触发新的 LLM 调用（它本身就是 agent 的输出）

### 需求 6：Conversation Idle Timer

**用户故事：** 作为核心开发者，我希望有一个显式的对话空闲 timer，以便
reducer 能在对话空闲时检查 agenda 并触发待办事项。

#### 验收标准

1. `TimerKind::kConversationIdle` 应在 Phase 4 中启用（Phase 2 已声明）
2. 当 turn 完成（`DeliverResponse` 效果执行后）时，Controller 应调度
   idle timer
3. 当用户输入到达时，Controller 应取消 idle timer
4. 当 idle timer 触发时，reducer 应检查 agenda 并处理到期事项
5. Idle timeout 时长应可配置（`ControllerConfig::conversation_idle_check`）

### 需求 7：新事件类型

**用户故事：** 作为核心开发者，我希望 DialogueEvent 变体包含 Phase 4 所需
的所有新事件类型。

#### 验收标准

1. 新增 `ClarificationReceived{item_id, approved, user_response, now}`
   事件
2. 新增 `AgendaItemDue{item_id, now}` 事件
3. 新增 `ConversationIdleCheck{now}` 事件（idle timer 触发时由 Controller
   构造）
4. 更新 `DialogueEvent` 变体包含所有新事件

### 需求 8：新效果类型

**用户故事：** 作为核心开发者，我希望 DialogueEffect 变体包含 Phase 4 所需
的所有新效果类型。

#### 验收标准

1. 新增 `RequestClarification{question, pending_action_id}` 效果
2. 新增 `EnqueueAgendaItem{item}` 效果
3. 新增 `DequeueAgendaItem{item_id}` 效果
4. 新增 `EmitProactiveMessage{content, source}` 效果
5. 更新 `DialogueEffect` 变体包含所有新效果

### 需求 9：Reducer 纯度保持

**用户故事：** 作为核心开发者，我希望 reducer 在 Phase 4 扩展后仍然是纯函数。

#### 验收标准

1. Reducer 不应执行 I/O、不应阻塞、不应修改共享状态
2. Reducer 不应读取系统时钟——所有时间戳来自事件字段
3. 相同输入调用两次 Reduce 应产生相同输出
4. Agenda 操作（入队、出队、检查到期）应是纯状态更新

### 需求 10：行为等价性

**用户故事：** 作为核心开发者，我希望 Phase 4 不破坏现有行为。

#### 验收标准

1. 所有现有测试应在 Phase 4 集成后无修改通过
2. 不启用 agenda 功能时（agenda 为空），所有路径行为与 Phase 3 完全一致
3. `HandleSystemEvent` 在 `deliberation == kIdle` 时的行为应保持不变
   （立即 StartLlm），只有在 `deliberation != kIdle` 时才延迟到 agenda

### 需求 11：模块隔离

**用户故事：** 作为核心开发者，我希望对话模块不依赖 `io/`、`runtime/`、
`services/` 或 `app/`。

#### 验收标准

1. 对话模块不应包含 `io/`、`runtime/`、`services/` 或 `app/` 的头文件
2. 新的 agenda 类型应只依赖 C++17 标准库和现有 `core/` 头文件

### 需求 12：测试

**用户故事：** 作为核心开发者，我希望有专门的测试覆盖 agenda 行为。

#### 验收标准

1. 测试套件应包含 clarification 流程的单元测试（请求→确认→执行、
   请求→拒绝→放弃、请求→新消息→取消）
2. 测试套件应包含 delayed reminder 的单元测试（忙时延迟、空闲时触发）
3. 测试套件应包含 conversation idle timer 的单元测试
4. 测试套件应包含 agenda 优先级排序的单元测试
5. 测试套件宜包含 property test 验证 agenda 不变量
