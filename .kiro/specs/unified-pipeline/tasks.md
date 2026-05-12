# 任务列表：统一语义 Pipeline

## Phase 1：定义核心类型

- [x] 1. 新建 `core/content_part.h`，定义 `TextPart`（含 `std::string text`）、`ImagePart`（含 `std::string url`）、`AudioPart`（含 `std::vector<uint8_t> data`、`std::string format`）
  - 验证：需求 3.1
  - 文件：`core/content_part.h`（新建）

- [x] 2. 在 `core/content_part.h` 中定义 `ToolCallPart`（含 `tool_call_id`、`name`、`arguments_json`）和 `ToolResultPart`（含 `tool_call_id`、`tool_name`、`success`、`result_json`）
  - 验证：需求 3.2
  - 文件：`core/content_part.h`

- [x] 3. 在 `core/content_part.h` 中定义 `using ContentPart = std::variant<TextPart, ImagePart, AudioPart, ToolCallPart, ToolResultPart>` 和 `using ContentParts = std::vector<ContentPart>`
  - 验证：需求 3.3, 3.4
  - 文件：`core/content_part.h`

- [x] 4. 新建 `core/conversation_item.h`，定义 `ActorKind` enum（kHuman, kAssistant, kSystem, kTool）和 `ActorRef` struct（actor_id, display_name, kind）
  - 验证：需求 2.2
  - 文件：`core/conversation_item.h`（新建）

- [x] 5. 在 `core/conversation_item.h` 中定义 `ConversationItemKind` enum（kUserMessage, kAssistantMessage, kSystemEvent, kToolCall, kToolResult）
  - 验证：需求 2.2
  - 文件：`core/conversation_item.h`

- [x] 6. 在 `core/conversation_item.h` 中定义 `ConversationItem` struct，包含 `item_id`、`conversation_id`、`kind`、`actor`、`parts`（ContentParts）、`wall_time`（system_clock）、`reply_to_item_id`（optional）、`mentions`（vector<string>）
  - 验证：需求 2.1, 2.2, 5.1
  - 文件：`core/conversation_item.h`
  - 约束：不得包含 `annotations`、`extensions`、`payload` 等通用字段

- [x] 7. 新建 `core/invoke_batch.h`，定义 `TriggerReason` enum（kUserFlush, kDebounceTimeout, kToolContinuation, kSystemEvent, kIdlePrompt）和 `InvokeBatch` struct（conversation_id, items, reason, created_at）
  - 验证：需求 7.4
  - 文件：`core/invoke_batch.h`（新建）

- [x] 8. 新建 `core/control_signal.h`，定义 ControlSignal variant 类型，包含：FlushSignal、CancelSignal、InterruptSignal、ThinkStartSignal、ToolCallStartSignal（含 tool_call_id, name, arguments）、FinalAnswerStartSignal、TurnCompleteSignal、ToolResultSignal（含 tool_call_id, content, success）
  - 验证：需求 9.2
  - 文件：`core/control_signal.h`（新建）

- [x] 9. 新建 `io/audio_frame.h`，定义 `AudioFrame` struct（samples, sample_rate, timestamp），用于 perception 层高频音频路径
  - 验证：需求 1.2
  - 文件：`io/audio_frame.h`（新建）

- [x] 10. 更新 `core/CMakeLists.txt`，将新头文件纳入编译目标，确保项目能干净编译
  - 验证：编译通过

## Phase 2：删除全部旧类型和冗余代码

本 phase 的目标是一次性删除所有旧的中间类型、转换逻辑和冗余代码。删除后项目暂时无法编译——这是预期的，后续 phase 会重建所有依赖路径。

- [x] 11. 删除 `core/conversation/item.h` 和 `core/conversation/item.cpp`（旧的 ConversationItem，含 json payload、工厂函数、序列化函数）
  - 文件：`core/conversation/item.h`、`core/conversation/item.cpp`

- [x] 12. 删除 `core/conversation/render.h` 和 `core/conversation/render.cpp`（旧的 RenderForLlm 函数）
  - 文件：`core/conversation/render.h`、`core/conversation/render.cpp`

- [x] 13. 删除 `core/conversation/tags.h`（如果存在）
  - 文件：`core/conversation/tags.h`

- [x] 14. 删除 `core/context/types.h` 中的 `ContextMessage`、`ContextWindow`、`MemoryEntry`、`MemoryEntryType` 类型定义（整个文件内容清空或删除文件）
  - 文件：`core/context/types.h`

- [x] 15. 删除 `core/controller/types.h` 中的 `Observation` struct、`ObservationType` enum；保留 `State`、`Event`、`ActionType`、`ToolCall`、`ActionCandidate`、`ActivityKind`、`ActivityEvent`（这些在后续 phase 中会逐步迁移或保留）
  - 文件：`core/controller/types.h`

- [x] 16. 删除 `core/dialogue/types.h` 中的 `WorkspaceEntry` 和 `TurnWorkspace` struct；删除 `DialogueState` 中的 `workspace` 字段
  - 文件：`core/dialogue/types.h`

- [x] 17. 删除 `core/dialogue/types.h` 中所有引用 `Observation` 的 DialogueEvent 和 DialogueEffect（`UserMessageReceived`、`ToolResultReceived`、`SystemEventReceived`、`UserFragmentReceived`、`AggregationComplete`、`AggregationTimeout`、`RecordMemory`、`StartLlm`、`RecordToolResult`、`StartTurnTriggerClassification`、`BufferToWorkspace`、`CommitWorkspace`、`DiscardWorkspace` 等）
  - 文件：`core/dialogue/types.h`
  - 注意：这些会在 Phase 4 中用新类型重建

- [x] 18. 删除 `io/data_frame.h`（旧的 DataFrame 类型）
  - 文件：`io/data_frame.h`

- [x] 19. 删除 `io/control_frame.h`（旧的 string-based ControlFrame）
  - 文件：`io/control_frame.h`

- [x] 20. 删除 `io/interrupt_frame.h`（旧的 InterruptFrame）
  - 文件：`io/interrupt_frame.h`

- [x] 21. 删除 `core/interfaces/memory_store.h` 中基于 `MemoryEntry` 的接口（后续用新的 History 接口替代）
  - 文件：`core/interfaces/memory_store.h`

- [x] 22. 删除 `services/memory/in_memory_store.h`（基于 MemoryEntry 的实现）
  - 文件：`services/memory/in_memory_store.h`

- [x] 23. 删除 `services/llm/openai/json_parser.cpp` 中的 `MessageToJson` 函数（基于 ContextMessage 的旧序列化）
  - 文件：`services/llm/openai/json_parser.cpp`

- [x] 24. 删除 `core/strategies/` 目录下所有基于 Observation 的 strategy 接口和实现：`observation_aggregator.h`、`observation_filter.h`（及对应 .cpp）、`llm_observation_filter.h/.cpp`、`llm_observation_aggregator.h/.cpp`
  - 文件：`core/strategies/observation_aggregator.h`、`core/strategies/llm_observation_filter.h`、`core/strategies/llm_observation_filter.cpp`、以及相关 aggregator 文件

- [x] 25. 删除 `runtime/core_device.cpp` 中所有 DataFrame → Observation 的转换代码（`OnInput` 中的 metadata 解析、ConversationItem 构造逻辑）
  - 文件：`runtime/core_device.cpp`
  - 注意：CoreDevice 的新输入接口在 Phase 4 重建

- [x] 26. 删除所有 `#include` 引用已删除头文件的语句；删除所有因类型删除而变成死代码的函数体
  - 文件：全项目范围
  - 目标：消除所有 unused include 和引用已删除类型的代码

- [x] 27. 更新所有 CMakeLists.txt，移除已删除 .cpp 文件的编译单元引用
  - 文件：`core/CMakeLists.txt`、`services/CMakeLists.txt`、`io/*/CMakeLists.txt`

## Phase 3：让解释层输出 `ConversationItem`

- [x] 28. 重写 OneBot parser（`io/onebot/onebot_device.cpp`）：将 OneBot event JSON 解析为新的 `ConversationItem`，包含 actor（QQ号 + 昵称）、parts（TextPart + ImagePart）、mentions、reply_to_item_id
  - 验证：需求 6.1, 6.6
  - 文件：`io/onebot/onebot_device.h`、`io/onebot/onebot_device.cpp`

- [x] 29. 重写 Flutter input adapter（`ui/bridge/shizuru_bridge.cpp`）：将用户输入 + 附件转为 `ConversationItem`
  - 验证：需求 6.2, 6.6
  - 文件：`ui/bridge/shizuru_bridge.h`、`ui/bridge/shizuru_bridge.cpp`

- [x] 30. 重写 Scheduler adapter（`app/scheduler/scheduler_device.cpp`）：将定时事件转为 `ConversationItem`（kind=kSystemEvent）
  - 验证：需求 6.3
  - 文件：`app/scheduler/scheduler_device.h`、`app/scheduler/scheduler_device.cpp`

- [x] 31. 重写 ASR output adapter：仅在最终结果确定时输出 `ConversationItem`；内部维护 partial 状态不暴露
  - 验证：需求 6.4, 6.5
  - 文件：`io/asr/baidu/baidu_asr_device.cpp`

## Phase 4：重建 Core 输入与推理路径

- [x] 32. 定义 Core 的新输入接口：`void OnConversationItem(ConversationItem item)` 和 `void OnControl(ControlSignal signal)`
  - 验证：需求 7.1
  - 文件：`runtime/core_device.h`

- [x] 33. 重建 dialogue reducer 的事件类型：用 `ConversationItem` 替代旧的 Observation 载荷
  - 验证：需求 7.1
  - 文件：`core/dialogue/types.h`、`core/dialogue/default_reducer.cpp`

- [x] 34. 实现 Core batching 逻辑：维护 `vector<ConversationItem>` pending 队列，根据 turn policy 构造 `InvokeBatch`
  - 验证：需求 7.1, 7.2, 7.3
  - 文件：新建 `core/batching.h` / `core/batching.cpp`

- [x] 35. 实现 Core output interpretation：解析 LLM streaming response → 产出 ConversationItem + ControlSignal
  - 验证：需求 8.1, 8.2, 8.3
  - 文件：新建 `core/output_interpreter.h` / `core/output_interpreter.cpp`

- [x] 36. 重写 ToolDispatchDevice 回传路径：执行完成后只发出 `ToolResultSignal`
  - 验证：需求 9.1, 9.2
  - 文件：`runtime/tool_dispatch_device.cpp`

- [x] 37. 实现 Core 对 ToolResultSignal 的处理：构造 `ConversationItem(kToolResult)` 写入 history，触发 tool continuation
  - 验证：需求 9.3
  - 文件：`core/controller/controller.cpp`

## Phase 5：历史与 Provider Render

- [x] 38. 实现新的 History 存储接口：`void Append(ConversationItem)`、`vector<ConversationItem> GetWindow(int max_tokens)`
  - 验证：需求 10.1, 10.3
  - 文件：新建 `core/history.h` / 重写 `core/interfaces/memory_store.h`

- [x] 39. 实现 in-memory history store
  - 验证：需求 10.1
  - 文件：新建 `services/memory/in_memory_history.h`

- [x] 40. 更新 SQLite memory store：持久化格式改为 ConversationItem JSON
  - 验证：需求 10.1
  - 文件：`app/memory/sqlite_memory_store.cpp`

- [x] 41. 实现 Provider Render 模块：`InvokeBatch` + history → OpenAI messages JSON
  - 验证：需求 11.2, 11.3
  - 文件：新建 `services/llm/openai/provider_render.h` / `provider_render.cpp`
  - 实现细节：
    - kUserMessage → content array（TextPart/ImagePart/AudioPart）
    - kAssistantMessage → content string
    - kToolCall → tool_calls JSON array
    - kToolResult → role=tool message
    - 多 actor 群聊：在 TextPart 前注入 `<message>` 标签（render 层职责）

- [x] 42. 更新 LlmClient 接口：输入从 `ContextWindow` 改为 provider render 产出的 JSON
  - 验证：需求 11.2
  - 文件：`core/interfaces/llm_client.h`、`services/llm/openai/openai_client.h/.cpp`

## Phase 6：约束验证与测试

- [x] 43. 编写 `kind` 与 `parts` 约束的单元测试
  - 验证：需求 4.1-4.6
  - 文件：`tests/core/conversation_item_test.cpp`（新建）

- [x] 44. 编写多个并行 tool call 的 provider render 测试
  - 验证：需求 4.4
  - 文件：`tests/core/provider_render_test.cpp`（新建）

- [x] 45. 编写多个 tool result 的 provider render 测试
  - 验证：需求 4.5
  - 文件：`tests/core/provider_render_test.cpp`

- [x] 46. 编写群聊 A/B/A 顺序保持测试
  - 验证：需求 7.3
  - 文件：`tests/core/batching_test.cpp`（新建）

- [x] 47. 编写 ASR final-only 进入 Core 的测试
  - 验证：需求 6.4, 6.5
  - 文件：`tests/io/asr_interpreter_test.cpp`（新建）

- [x] 48. 编写 tool result 回传路径测试
  - 验证：需求 9.1, 9.2, 9.3
  - 文件：`tests/core/tool_result_flow_test.cpp`（新建）

## Phase 7：更新 Examples 与文档

- [x] 49. 更新 `examples/voice_agent.cpp`：使用新的 interpreter → ConversationItem → Core 路径
  - 文件：`examples/voice_agent.cpp`

- [x] 50. 更新 `examples/tool_call.cpp`：使用新的 ConversationItem 输入 + ControlSignal tool 回传
  - 文件：`examples/tool_call.cpp`

- [x] 51. 更新 `examples/onebot_agent.cpp`：使用新的 OneBot parser → ConversationItem 路径
  - 文件：`examples/onebot_agent.cpp`

- [x] 52. 更新其他 examples（asr_tts_echo_pipeline、elevenlabs_tts_playout 等）适配新类型
  - 文件：`examples/` 目录下相关文件

- [x] 53. 确认全项目编译通过、所有测试通过；确认无 `#if 0`、注释掉的旧代码、unused include 残留
  - 文件：全项目

- [x] 54. 更新 `AGENTS.md` 反映新的语义 pipeline 架构
  - 文件：`AGENTS.md`

- [x] 55. 更新 `.kiro/steering/architecture.md` 反映新数据模型和分层
  - 文件：`.kiro/steering/architecture.md`
