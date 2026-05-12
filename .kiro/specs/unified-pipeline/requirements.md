# 需求文档：统一语义 Pipeline

## 简介

本次重构的目标是建立统一的语义 pipeline，使系统明确区分：

- 原始信号
- 解释后的语义事件
- 一次 LLM 调用的批次
- 控制流信号

核心结论：

- `ConversationItem` 是解释层的一次最终解释结果
- `InvokeBatch` 是一次 LLM 调用的输入单位
- Tool Executor 只回传控制信号，Core 负责构造 `kToolResult` 的 history item

## 需求

### 需求 1：原始信号与语义输入分层

1. 系统必须将 perception 路径上的原始信号类型与语义输入类型分离。
2. 音频、视频等高频信号不得直接建模成 `ConversationItem`。
3. `ConversationItem` 只表示解释层已经完成的一次 source-final 结果。

### 需求 2：统一的 `ConversationItem`

1. 系统必须定义统一的 `ConversationItem` 类型，作为解释层对外输出的规范语义单元。
2. `ConversationItem` 必须包含：
   - `item_id`
   - `conversation_id`
   - `kind`
   - `actor`
   - `parts`
   - `wall_time`
3. `ConversationItem` 必须是 source-final 的。
4. `ConversationItem` 不得承担 provisional/partial 状态。
5. 如果源头产生多个最终事件，它们必须表示为多个有序的 `ConversationItem`。

### 需求 3：统一的 `ContentPart`

1. 系统必须定义 `TextPart`、`ImagePart`、`AudioPart`。
2. 系统必须定义 `ToolCallPart` 与 `ToolResultPart`。
3. `ContentPart` 必须通过 `std::variant` 统一表示。
4. `ContentParts` 必须是 `std::vector<ContentPart>`。

### 需求 4：`kind` 与 `parts` 的约束

1. `kUserMessage` 只允许 `TextPart`、`ImagePart`、`AudioPart`。
2. `kAssistantMessage` 只允许 `TextPart`、`ImagePart`、`AudioPart`。
3. `kSystemEvent` 默认只允许 `TextPart`，其他模态必须显式论证需求。
4. `kToolCall` 只允许一个或多个 `ToolCallPart`。
5. `kToolResult` 只允许一个或多个 `ToolResultPart`。
6. `ToolCallPart` 与 `ToolResultPart` 不得在同一个 `ConversationItem` 中交错混装。

### 需求 5：不引入通用 annotations 垃圾桶

1. `ConversationItem` 不得包含通用 `annotations` 或 `extensions` 字段。
2. 新的结构化语义必须通过：
   - 新增显式字段
   - 新增 `ContentPart`
   - 新增 `ConversationItemKind`
   三者之一表达。

### 需求 6：解释层职责

1. OneBot parser 必须直接输出最终的 `ConversationItem`。
2. Flutter input adapter 必须直接输出最终的 `ConversationItem`。
3. Scheduler adapter 必须直接输出最终的 `ConversationItem`。
4. ASR adapter 只在最终结果产出时输出 `ConversationItem`。
5. ASR partial / provisional 状态不得暴露给 Core。
6. 对于天然 final 的输入源，解释层不得额外引入时间窗口等待。

### 需求 7：Core batching 职责

1. Core 必须是唯一的跨消息 batching 决策者。
2. 是否将多个 `ConversationItem` 合并为一次 LLM 调用，必须由 Core 决定。
3. source adapter 不得擅自做跨 actor 聚合。
4. `InvokeBatch` 必须表示一次 LLM 调用的输入单位。

### 需求 8：输出方向的 interpretation

1. LLM 原始输出不得直接视为 `ConversationItem`。
2. Core 必须对 LLM 原始输出做 output interpretation。
3. output interpretation 必须能够产出：
   - assistant message 类型的 `ConversationItem`
   - `kToolCall` 类型的 history item
   - `ControlSignal`

### 需求 9：Tool Executor 回传约束

1. Tool Executor 不得直接构造 `ConversationItem`。
2. Tool Executor 只回传 `ToolResult` 类 `ControlSignal`。
3. Core 在收到 tool result signal 后，必须构造 `ConversationItem(kind = kToolResult)` 写入 history。

### 需求 10：统一 history 与 replay 语义

1. History 中存储的语义对象必须是 `ConversationItem`。
2. UI replay 读取的语义对象必须是 `ConversationItem`。
3. `kToolCall` / `kToolResult` 必须作为会话事件进入 history。

### 需求 11：Provider payload 是终端投影

1. Provider request payload 不是系统内部真相。
2. 系统必须通过 render 过程将 `InvokeBatch` / history 投影为 provider payload。
3. 内部模型不得直接坍缩为某个单一 provider 的 payload 格式。

### 需求 12：逐步迁移与清理

1. 系统必须允许分阶段迁移到新模型。
2. 迁移完成后，应删除旧的多层重复类型链路。
3. 重构完成后，不应再存在从扁平字符串重建结构化消息的核心路径。
