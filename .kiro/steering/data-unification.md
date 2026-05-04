---
inclusion: auto
---

# 数据层统一

## 状态

待评审提案。定义整条 pipeline 的目标数据架构，用清晰的三层结构替代当前
临时拼凑的多重表示模型。

## 问题

当前 pipeline 对语义上相同的数据（一条对话消息）有六种不同的表示：

```
DataFrame → Observation → DialogueEvent → MemoryEntry → ContextMessage
                ↕
         ConversationItem
```

每种表示各自携带一套身份和内容字段：

| 类型              | 身份字段                                 | 内容字段                            |
|-------------------|------------------------------------------|-------------------------------------|
| DataFrame         | metadata["actor_id"]（可选 KV）          | payload（字节）                     |
| Observation       | source（字符串）                         | content（字符串）                   |
| ConversationItem  | Actor{actor_id, kind, display_name}      | payload（JSON）                     |
| MemoryEntry       | role + source_tag                        | content + item_json + tool_calls_json |
| ContextMessage    | role + name                              | content + tool_call_id + tool_calls_json |

问题：

1. **冗余的平行字段**。`Observation` 同时携带 `content` 和
   `item.payload["text"]`。`MemoryEntry` 同时携带 `content`、`source_tag`、
   `tool_calls_json` 以及 `item_json`（完整序列化的 ConversationItem）。
   改了一个忘改另一个就会产生静默不一致。

2. **每次转换都丢失身份信息**。`Actor{id, kind, display_name}` 在
   Observation 中被压缩为 `source`（字符串），在 MemoryEntry 中变成
   `role + source_tag`，在 ContextMessage 中变成 `role + name`。
   `ActorKind` 在第一次转换后就丢了。`display_name` 除非 optional 的
   `item` 字段被填充，否则也会丢失。

3. **多条渲染路径**。`ContextStrategy::BuildContext` 有两条代码路径：
   一条反序列化 `item_json` 并调用 `RenderForLlm`，另一条从扁平的
   `role/content/source_tag` 字段构造 `ContextMessage` 作为兜底。
   兜底路径会丢失所有结构化身份信息。

4. **群聊不可能实现**。在多用户对话中，LLM 必须区分"Alice 说了 X"和
   "Bob 说了 Y"。当前 `source = "user"` 的字符串无法承载这一信息。
   `ConversationItem` 中结构化的 `Actor` 可以，但它是 optional 的且经常
   缺失。

## 设计：三层数据架构

```
┌─────────────────────────────────────────────────────────┐
│  传输层：io/data_frame.h                                │
│  DataFrame — 原始字节 + metadata KV                     │
│  作用域：io/ 和 runtime/ 设备总线                       │
│  不了解对话语义                                         │
└────────────────────────┬────────────────────────────────┘
                         │ CoreDevice::OnInput()
                         │（边界转换 — DataFrame 变成
                         │  ConversationItem 的唯一位置）
┌────────────────────────▼────────────────────────────────┐
│  语义层：core/conversation/item.h                       │
│  ConversationItem — 唯一的业务真相                      │
│  作用域：core/、app/、ui/bridge/、持久化                │
│  携带：Actor 身份、payload、线程元数据                  │
└────────────────────────┬────────────────────────────────┘
                         │ RenderForLlm()
                         │（投影 — 无状态，对 LLM 无损）
┌────────────────────────▼────────────────────────────────┐
│  LLM 投影层：core/context/types.h                       │
│  ContextMessage / ContextWindow — OpenAI API 格式       │
│  作用域：context strategy → LlmClient                   │
│  不参与业务逻辑，不参与持久化                           │
└─────────────────────────────────────────────────────────┘
```

### 原则

1. **ConversationItem 是唯一的业务真相**。pipeline 中所有语义数据都以它
   为准。其他表示要么构造它（DataFrame → ConversationItem），要么从它
   派生（ConversationItem → ContextMessage）。

2. **对 message-like 条目不允许平行内容字段**。如果一个 message-like
   条目（用户消息、助手消息、工具调用、工具结果）需要文本内容，它持有
   一个 `ConversationItem`，而不是在 `item` 旁边再放独立的 `content`
   字符串。非 message 条目（`kSummary`、`kExternalContext`）不受此约束，
   它们继续使用扁平字段作为载荷。

3. **身份是结构化的，不是字符串**。用户身份始终是
   `Actor{actor_id, kind, display_name}`，而不是裸的 `source` 字符串。
   这使得群聊成为数据模型的自然结果，而非特殊情况。

4. **投影类型是终端的**。`ContextMessage` 和 `ContextWindow` 在最后一刻
   由 `RenderForLlm()` 生成，仅由 `LlmClient` 消费。它们不会回流到
   pipeline 中，不会被持久化，不会用于业务决策。

5. **传输类型留在传输层**。`DataFrame` 不进入 `core/`。
   `ConversationItem` 不进入 `io/`。`CoreDevice` 是唯一的边界翻译器。

## 具体变更

### Observation（core/controller/types.h）

变更前：
```cpp
struct Observation {
  ObservationType type;
  std::string content;
  std::string source;
  std::chrono::steady_clock::time_point timestamp;
  std::optional<conversation::ConversationItem> item;
};
```

变更后：
```cpp
struct Observation {
  ObservationType type;
  conversation::ConversationItem item;  // 必填，不再 optional
  std::chrono::steady_clock::time_point timestamp;
};
```

- 删除 `content` — 使用 `item.payload["text"]` 或相应的 payload 字段。
- 删除 `source` — 使用 `item.actor.actor_id` 获取身份，
  `item.actor.kind` 获取分类。
- `item` 现在是必填的。对于 `kContinuation` 和 `kInterruption`（不携带
  对话内容），使用哨兵 item，或者更好的做法是将它们拆分为独立的内部信号
  类型，不再属于 `Observation`（见下文）。

便捷访问器（内联，用于迁移过渡）：
```cpp
// 已废弃 — 请直接使用 item.payload["text"]。
inline std::string ObservationText(const Observation& obs) {
  return obs.item.payload.value("text", "");
}
// 已废弃 — 请直接使用 obs.item.actor.actor_id。
inline std::string ObservationSource(const Observation& obs) {
  return obs.item.actor.actor_id;
}
```

### ObservationType 清理

`kInterruption` 和 `kContinuation` 不是观察——它们是不携带对话内容的
内部控制信号，不应该是 `Observation` 的变体。

建议拆分：
```cpp
// Observation 类型 — 携带 ConversationItem 的事物
enum class ObservationType {
  kUserMessage,
  kToolResult,
  kSystemEvent,
};

// 内部信号 — 没有 ConversationItem，单独处理
// （已部分建模为 DialogueEvent 变体：
//  InterruptRequested、ContinuationRequested）
```

这已经是对话内核的演进方向。`kInterruption` 通过
`Controller::Interrupt()` → `InterruptRequested` 事件处理。
`kContinuation` 通过 `ContinuationRequested` 事件处理。
一旦所有调用点迁移完毕，这些 `ObservationType` 变体即可移除。

### MemoryEntry（core/context/types.h）

变更前：
```cpp
struct MemoryEntry {
  MemoryEntryType type;
  std::string role;
  std::string content;
  std::string source_tag;
  std::string tool_call_id;
  std::string tool_calls_json;
  std::string item_json;
  std::chrono::steady_clock::time_point timestamp;
  int estimated_tokens = 0;
};
```

变更后：
```cpp
struct MemoryEntry {
  MemoryEntryType type;                 // 保留 — 拥有 context/persistence 语义
  conversation::ConversationItem item;  // message-like 条目的主载荷
  std::string role;                     // summary/external 条目仍需要
  std::string content;                  // summary/external 条目仍需要
  std::string source_tag;               // summary/external 条目仍需要
  std::chrono::steady_clock::time_point timestamp;
  int estimated_tokens = 0;
};
```

设计要点：

- **保留 `MemoryEntryType`**。`kSummary` 和 `kExternalContext` 是
  context/persistence 层的语义概念，不是对话消息。它们由
  `ContextStrategy::MaybeSummarize()` 等机制产生和消费，不应被强行
  塞进 `ConversationItem` 的 `ItemKind`。`MemoryEntryType` 继续作为
  memory store 的分类维度。

- **保留 `role`、`content`、`source_tag` 扁平字段**。这些字段是
  `kSummary` 和 `kExternalContext` 条目的实际载荷。这两类条目不经过
  `ConversationItem`，它们的渲染路径直接从扁平字段构造 `ContextMessage`。

- **删除 `tool_call_id`、`tool_calls_json`、`item_json`**。对于
  message-like 条目，这些信息全部由 `item` 承载，不再需要冗余副本。
  `item_json` 被 `item` 本身替代（持久化层直接序列化 `item`）。

- 对于 **message-like 条目**（`kUserMessage`、`kAssistantMessage`、
  `kToolCall`、`kToolResult`），`item` 是主载荷。`role`、`content`、
  `source_tag` 字段不使用（为空）。`type` 可从 `item.kind` 派生
  但仍显式存储以保持 persistence 层的自洽。

- 对于 **非 message 条目**（`kSummary`、`kExternalContext`），`item`
  为空的默认 `ConversationItem`。实际内容由 `type` + `role` +
  `content` + `source_tag` 承载。这些条目数量极少且只在 context 层
  内部流转。

总结：`ConversationItem` 统一的是 **message-like 条目**的载荷，
不是 memory store 的全部分类体系。`MemoryEntryType` 和扁平字段
作为 persistence 层的基础设施继续存在，服务于 summary/external
这类非对话条目。

### ContextMessage / ContextWindow（core/context/types.h）

不做结构性变更。继续作为 LLM 投影类型。

通过注释明确其角色：
```cpp
// LLM 投影类型 — 由 RenderForLlm() 生成，仅由 LlmClient 消费。
// 不参与持久化，不参与业务逻辑。
// 这是 OpenAI Chat Completions API 的消息格式。
struct ContextMessage { ... };
struct ContextWindow { ... };
```

### ContextStrategy::BuildContext

简化为两条明确的渲染路径（按 `MemoryEntryType` 分派）：
```cpp
ContextWindow BuildContext(const std::string& session_id,
                           const Observation& current_observation) {
  // 1. System instruction → ContextMessage
  // 2. store_.GetRecent() → vector<MemoryEntry>
  //    对每条：
  //    - message-like (kUserMessage/kAssistantMessage/kToolCall/kToolResult):
  //        RenderForLlm(entry.item) → ContextMessage
  //    - non-message (kSummary/kExternalContext):
  //        从 entry.role + entry.content 构造 ContextMessage
  // 3. RenderForLlm(current_observation.item) → ContextMessage
}
```

相比现状的改进：
- 删除 `LegacyObservationMessage`（从 Observation 的扁平字段兜底构造
  ContextMessage 的路径）— 因为 Observation 不再有扁平字段。
- message-like 条目不再有"先尝试反序列化 item_json，失败则退化到
  role + content"的双路径 — 统一走 `RenderForLlm(entry.item)`。
- summary/external 条目保留从扁平字段构造 ContextMessage 的路径 —
  这不是"兜底"，而是这类条目的正常且唯一的渲染方式。

### DialogueEvent / DialogueEffect（core/dialogue/types.h）

内嵌 `Observation` 的事件自动受益于更精简的 `Observation`。
事件/效果类型本身不需要结构性变更。

`WorkspaceEntry` 简化为：
```cpp
struct WorkspaceEntry {
  conversation::ConversationItem item;
  std::chrono::steady_clock::time_point timestamp;
};
```

删除 `content`、`source`、`entry_type` — 均可从 `item` 派生。

### ActionCandidate（core/controller/types.h）

`ActionCandidate` 表示 LLM 输出，不是对话历史。暂时保持不变。
当 LLM 响应提交到记忆时，在那个时刻转换为 `ConversationItem`
（通过 `MakeAssistantMessageItem` 或 `MakeToolCallItem`）。

### ConversationItem 对群聊的支持

现有的 `ConversationItem` 已经具备群聊所需的结构：

```cpp
struct Actor {
  std::string actor_id;      // "alice"、"bob"、"system:scheduler"
  ActorKind kind;             // kHuman、kAssistant、kSystem、kTool
  std::string display_name;   // "Alice"、"Bob"
};

struct ConversationItem {
  // ... 现有字段 ...
  std::optional<std::string> reply_to_item_id;  // 线程关联
  std::vector<std::string> mentions;             // @提及
  std::optional<std::string> turn_group_id;      // 轮次分组
};
```

`RenderForLlm` 已经能渲染多用户标记：
```xml
<message actor_id="alice" actor_name="Alice">Hello!</message>
```

`ConversationItem` 本身不需要修改。群聊能力通过让 `Actor` 成为整条
pipeline 中的权威身份来解锁（这正是本次统一所实现的）。

### 聚合器：不按用户分桶

聚合器的职责是语音端点检测（判断一段话是否说完），不是按说话人分组。
在群聊中，不同用户的消息按时间顺序到达，这种交织本身就是有意义的上下文。
按用户分桶聚合会破坏对话的自然时序。

每条消息已经通过 `Observation.item.actor` 携带了完整的 `Actor` 身份。
聚合器继续在全局流上按到达顺序操作。群聊支持完全由数据统一
（Phase A–C）实现，不需要聚合器变更。

## 统一后的数据流

```
DataFrame                              io 层：原始字节 + metadata
  │
  │ CoreDevice::OnInput()              唯一的边界转换
  ▼
Observation { type, item, ts }         core 入口：事件信封
  │                                    item = ConversationItem（必填）
  │ Aggregator（端点检测，全局流）
  ▼
Observation                            聚合后的完整消息
  │
  │ 包装为 DialogueEvent 变体
  ▼
DialogueEvent                          reducer 输入
  │
  │ reducer
  ▼
DialogueDecision { next_state, effects }
  │
  │ effect executor
  ├──→ MemoryEntry { type, item, ... } 持久化：message-like 条目存 ConversationItem
  │      │                              summary/external 条目存扁平字段
  │      │ BuildContext:
  │      │   message-like → RenderForLlm(entry.item)
  │      │   summary/ext  → role + content 构造
  │      ▼
  │    ContextMessage                   LLM 投影（终端）
  │
  ├──→ EmitConversationItem(item)      UI 推送：直接使用 ConversationItem
  │
  └──→ DataFrame                       TTS / io 输出
```

**对于 message-like 条目，ConversationItem 是从 CoreDevice 到持久化和
UI 的唯一权威载体。ContextMessage 是在最后一刻生成的无状态投影，仅供
LLM 消费。非 message 条目（summary/external）不经过 ConversationItem，
由 context 层内部通过扁平字段管理。**

## 不变的部分

- `DataFrame` — 传输层，留在 io/runtime。
- `ContextMessage` / `ContextWindow` — LLM 投影，留在 context/。
- `DialogueEvent` / `DialogueEffect` / `DialogueDecision` — reducer
  契约，留在 dialogue/。事件内嵌更精简的 `Observation`。
- `ActionCandidate` / `ToolCall` / `LlmResult` — LLM 输出类型，留在
  controller/types.h。在提交时转换为 `ConversationItem`。
- `RenderForLlm()` — 唯一的投影函数，留在 conversation/render.h。

## 迁移计划

### Phase A：让 Observation.item 成为必填

1. 将 `std::optional<ConversationItem> item` 改为 `ConversationItem item`。
2. 删除 `content` 和 `source` 字段。
3. 添加已废弃的内联访问器 `ObservationText()` 和 `ObservationSource()`
   用于渐进式迁移。
4. 更新所有 Observation 构造点：
   - `CoreDevice::OnInput` — 已经在构造 ConversationItem，只需停止设置
     被删除的字段。
   - `LlmObservationAggregator::FlushBuffer` — 从 buffer 构造 item。
   - `Controller` 内部构造（continuation、interrupt）— 将这些拆分为
     非 Observation 的信号类型，或使用哨兵 item。
5. 删除 `EnsureConversationItem()` 辅助函数 — 不再需要。

### Phase B：精简 MemoryEntry

1. 为 message-like 条目用 `ConversationItem item` 替换
   `tool_call_id`、`tool_calls_json`、`item_json`。保留
   `MemoryEntryType`、`role`、`content`、`source_tag`（供
   summary/external 条目使用）。
2. 更新 `MemoryEntryFromItem()` — message-like 条目只需设置
   `type` + `item` + `ts`，扁平字段留空。
3. 更新 `ContextStrategy::BuildContext` — message-like 条目走
   `RenderForLlm(entry.item)`；`kSummary`/`kExternalContext` 走
   `role + content` 构造。删除 `LegacyObservationMessage` 和
   "先尝试反序列化 item_json 再退化"的双路径逻辑。
4. 更新 `MemoryStore` 实现（SQLite、InMemory）— message-like 条目将
   `item` 序列化为 JSON blob；summary/external 条目保持现有存储方式。
5. 更新 dialogue/types.h 中的 `WorkspaceEntry` — 同样的模式。

### Phase C：清理 ObservationType

1. 从 `ObservationType` 中移除 `kInterruption` 和 `kContinuation`。
2. 确保所有中断/继续路径直接使用 `DialogueEvent` 变体
   （`InterruptRequested`、`ContinuationRequested`）。
3. `ObservationType` 变成干净的三变体枚举：
   `kUserMessage`、`kToolResult`、`kSystemEvent`。

每个阶段可独立编译和测试。Phase A 改动面最大。Phase B 和 C 是机械性
清理。

群聊不需要聚合器变更。聚合器在全局流上处理语音端点检测；actor 身份由
`ConversationItem` 携带，通过 Phase A–C 在整条 pipeline 中保持完整。

## 风险

- **Phase A 影响面广**。每个读取 `obs.content` 或 `obs.source` 的地方
  都需要更新。通过已废弃的访问器函数和渐进式迁移来缓解。

- **MemoryStore schema 迁移**。现有 SQLite 数据库以扁平字段存储
  `MemoryEntry`。需要一条迁移路径：读取旧格式（通过已有的 `item_json`
  字段）并写入新格式。`item_json` 字段正是为此前向兼容性而添加的。

- **ConversationItem 相比扁平字符串的性能**。`ConversationItem` 使用
  `nlohmann::json` 作为 payload，比纯 `std::string content` 更重。
  对于 pipeline 的吞吐量（每秒几十条消息，不是几百万条），这可以忽略。

## 与对话内核的关系

本提案与对话内核的各阶段正交。可以在任何内核阶段之前、期间或之后实施。
但它简化了未来的内核工作，因为：

- Reducer 事件携带更丰富的身份信息，无需额外管道。
- Workspace 条目是自描述的（不需要单独的 `entry_type` 枚举）。
- 上下文构建只有一条代码路径而非两条。
- 群聊支持从统一的身份模型中自然产生。

建议排序：在启动对话内核 Phase 4（议程/混合主动）之前实施 Phase A，
因为 Phase 4 将添加新的事件类型，这些类型受益于更干净的 Observation
结构。
