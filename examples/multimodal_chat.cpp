// Multimodal chat example: test interleaved text + image_url content parts.
//
// Tests the OpenAI-compatible API with a single user message containing
// multiple interleaved content parts:
//
//   {"role": "user", "content": [
//     {"type": "text", "text": "<message actor_id=\"A\">A的话</message>"},
//     {"type": "image_url", "image_url": {"url": "https://A的图片"}},
//     {"type": "text", "text": "<message actor_id=\"B\">B的话</message>"},
//     {"type": "image_url", "image_url": {"url": "https://B的图片"}},
//     ...
//   ]}
//
// Uses libcurl directly for exact control over the JSON payload structure.
//
// Usage:
//   export OPENAI_API_KEY=...
//   ./multimodal_chat [--base-url <url>] [--model <model>] [--api-path <path>]
//
// Examples:
//   ./multimodal_chat --base-url https://api.openai.com --model gpt-4o
//   ./multimodal_chat --base-url https://dashscope.aliyuncs.com \
//       --api-path /compatible-mode/v1/chat/completions --model qwen-vl-max

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

// A message from a group chat participant.
struct ActorMessage {
  std::string actor_id;
  std::string text;
  std::string image_url;  // optional, empty if no image
};

// Build interleaved content parts from a list of actor messages.
json BuildInterleavedContent(const std::vector<ActorMessage>& messages) {
  json parts = json::array();
  for (const auto& msg : messages) {
    // Text part with <message> tag wrapping
    std::string tagged_text =
        "<message actor_id=\"" + msg.actor_id + "\">" + msg.text + "</message>";
    parts.push_back({{"type", "text"}, {"text", tagged_text}});

    // Image part (if present)
    if (!msg.image_url.empty()) {
      parts.push_back({
          {"type", "image_url"},
          {"image_url", {{"url", msg.image_url}}},
      });
    }
  }
  return parts;
}

// Build the full request body JSON.
json BuildRequestBody(const std::string& model,
                      const std::string& system_prompt,
                      const std::vector<ActorMessage>& actor_messages,
                      double temperature = 0.7,
                      int max_tokens = 2048) {
  json body;
  body["model"] = model;
  body["temperature"] = temperature;
  body["max_tokens"] = max_tokens;
  body["stream"] = false;

  json messages = json::array();

  // System message
  if (!system_prompt.empty()) {
    messages.push_back({{"role", "system"}, {"content", system_prompt}});
  }

  // User message with interleaved content parts
  json user_msg;
  user_msg["role"] = "user";
  user_msg["content"] = BuildInterleavedContent(actor_messages);
  messages.push_back(std::move(user_msg));

  body["messages"] = std::move(messages);
  return body;
}

// libcurl write callback.
size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* response = static_cast<std::string*>(userdata);
  response->append(ptr, size * nmemb);
  return size * nmemb;
}

// Send HTTP POST request and return response body.
std::string HttpPost(const std::string& url,
                     const std::string& api_key,
                     const std::string& body) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    throw std::runtime_error("Failed to initialize libcurl");
  }

  std::string response;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  std::string auth_header = "Authorization: Bearer " + api_key;
  headers = curl_slist_append(headers, auth_header.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

  CURLcode res = curl_easy_perform(curl);

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    throw std::runtime_error(std::string("curl error: ") +
                             curl_easy_strerror(res));
  }

  if (http_code != 200) {
    std::fprintf(stderr, "[HTTP %ld] %s\n", http_code, response.c_str());
    throw std::runtime_error("HTTP " + std::to_string(http_code));
  }

  return response;
}

}  // namespace

int main(int argc, char* argv[]) {
  // ── CLI args ──────────────────────────────────────────────────────────────
  std::string base_url = "https://api.openai.com";
  std::string model = "gpt-4o";
  std::string api_path = "/v1/chat/completions";
  std::string system_prompt =
      "You are observing a group chat. Each message is tagged with an "
      "actor_id and may include an image. Describe what you see in each "
      "image and summarize the conversation.";
  bool interactive = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--base-url" && i + 1 < argc) {
      base_url = argv[++i];
    } else if (arg == "--model" && i + 1 < argc) {
      model = argv[++i];
    } else if (arg == "--api-path" && i + 1 < argc) {
      api_path = argv[++i];
    } else if (arg == "--system" && i + 1 < argc) {
      system_prompt = argv[++i];
    } else if (arg == "--interactive") {
      interactive = true;
    }
  }

  // ── Environment ───────────────────────────────────────────────────────────
  const char* api_key = std::getenv("OPENAI_API_KEY");
  if (api_key == nullptr) {
    std::fprintf(stderr, "Error: set OPENAI_API_KEY env var.\n");
    return 1;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  std::printf("=== Multimodal Interleaved Content Test ===\n\n");
  std::printf("Endpoint: %s%s\n", base_url.c_str(), api_path.c_str());
  std::printf("Model:    %s\n\n", model.c_str());

  // ── Build test payload ────────────────────────────────────────────────────
  // Example: 3 actors, each with a text message and an image.
  // Replace these URLs with real accessible image URLs for actual testing.
  std::vector<ActorMessage> actor_messages = {
      {"Alice",
       "看看我今天做的蛋糕！好不好看？",
       "https://upload.wikimedia.org/wikipedia/commons/thumb/4/40/"
       "Chocolate_cake.jpg/800px-Chocolate_cake.jpg"},
      {"Bob",
       "哇看起来好好吃！我也分享一张我家猫的照片",
       "https://upload.wikimedia.org/wikipedia/commons/thumb/3/3a/"
       "Cat03.jpg/1200px-Cat03.jpg"},
      {"Charlie",
       "你们都好有生活情趣，我今天只有加班",
       ""},  // Charlie has no image
  };

  json request_body =
      BuildRequestBody(model, system_prompt, actor_messages);

  // ── Print the request for inspection ──────────────────────────────────────
  std::printf("--- Request Body ---\n%s\n\n", request_body.dump(2).c_str());

  // ── Send request ──────────────────────────────────────────────────────────
  std::string url = base_url + api_path;
  std::string body_str = request_body.dump();

  std::printf("--- Sending... ---\n\n");

  try {
    std::string response = HttpPost(url, api_key, body_str);

    // Parse response
    json resp = json::parse(response);

    if (resp.contains("error")) {
      std::fprintf(stderr, "[API error] %s\n",
                   resp["error"].value("message", "unknown").c_str());
      curl_global_cleanup();
      return 1;
    }

    // Extract response text
    if (resp.contains("choices") && !resp["choices"].empty()) {
      auto& choice = resp["choices"][0];
      std::string content;
      if (choice.contains("message") &&
          choice["message"].contains("content") &&
          !choice["message"]["content"].is_null()) {
        content = choice["message"]["content"].get<std::string>();
      }

      std::printf("--- Response ---\n%s\n\n", content.c_str());

      // Token usage
      if (resp.contains("usage")) {
        auto& usage = resp["usage"];
        std::printf("Tokens: prompt=%d, completion=%d, total=%d\n",
                    usage.value("prompt_tokens", 0),
                    usage.value("completion_tokens", 0),
                    usage.value("total_tokens", 0));
      }
    } else {
      std::printf("--- Raw Response ---\n%s\n", response.c_str());
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "\n[error] %s\n", e.what());
    curl_global_cleanup();
    return 1;
  }

  // ── Interactive mode ──────────────────────────────────────────────────────
  if (interactive) {
    std::printf("\n--- Interactive Mode ---\n");
    std::printf("Enter messages as: actor_id|text|image_url (image_url optional)\n");
    std::printf("Empty line sends the batch. Type 'quit' to exit.\n\n");

    while (true) {
      std::vector<ActorMessage> batch;
      std::printf("Enter messages (empty line to send):\n");

      std::string line;
      while (std::getline(std::cin, line)) {
        if (line.empty()) break;
        if (line == "quit") {
          curl_global_cleanup();
          return 0;
        }

        // Parse: actor_id|text|image_url
        ActorMessage msg;
        size_t p1 = line.find('|');
        if (p1 == std::string::npos) {
          msg.actor_id = "User";
          msg.text = line;
        } else {
          msg.actor_id = line.substr(0, p1);
          size_t p2 = line.find('|', p1 + 1);
          if (p2 == std::string::npos) {
            msg.text = line.substr(p1 + 1);
          } else {
            msg.text = line.substr(p1 + 1, p2 - p1 - 1);
            msg.image_url = line.substr(p2 + 1);
          }
        }
        batch.push_back(std::move(msg));
      }

      if (batch.empty()) continue;

      json req = BuildRequestBody(model, system_prompt, batch);
      std::printf("\n--- Sending %zu messages... ---\n", batch.size());

      try {
        std::string response = HttpPost(url, api_key, req.dump());
        json resp = json::parse(response);

        if (resp.contains("choices") && !resp["choices"].empty()) {
          auto& choice = resp["choices"][0];
          if (choice.contains("message") &&
              choice["message"].contains("content") &&
              !choice["message"]["content"].is_null()) {
            std::printf("\n[assistant] %s\n\n",
                        choice["message"]["content"].get<std::string>().c_str());
          }
        }
      } catch (const std::exception& e) {
        std::fprintf(stderr, "[error] %s\n\n", e.what());
      }
    }
  }

  curl_global_cleanup();
  std::printf("\n=== Done ===\n");
  return 0;
}
