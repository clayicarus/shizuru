#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "app/persona/persona.h"

namespace shizuru::app {
namespace {

TEST(PersonaTest, DefaultPromptContainsIdentity) {
  const std::string prompt = DefaultPersonaPrompt();
  EXPECT_NE(prompt.find("Shizuru"), std::string::npos);
}

TEST(PersonaTest, DefaultPromptSetsTone) {
  const std::string prompt = DefaultPersonaPrompt();
  // Should mention natural conversational style, not formal.
  EXPECT_NE(prompt.find("friend"), std::string::npos);
}

TEST(PersonaTest, DefaultPromptForbidsMarkdown) {
  const std::string prompt = DefaultPersonaPrompt();
  EXPECT_NE(prompt.find("markdown"), std::string::npos);
}

TEST(PersonaTest, DefaultPromptExplainsNameField) {
  const std::string prompt = DefaultPersonaPrompt();
  EXPECT_NE(prompt.find("name"), std::string::npos);
  EXPECT_NE(prompt.find("scheduler"), std::string::npos);
}

TEST(PersonaTest, BuildSystemPromptWithEmptyContext) {
  const std::string prompt = BuildSystemPrompt({}, {});
  EXPECT_NE(prompt.find("Shizuru"), std::string::npos);
  // Should have current time.
  EXPECT_NE(prompt.find("Current time:"), std::string::npos);
  // Should NOT have preference or followup sections with actual items.
  EXPECT_EQ(prompt.find("What you know about this user"), std::string::npos);
  // The followup section header should not appear when list is empty.
  EXPECT_EQ(prompt.find("naturally check in on"), std::string::npos);
}

TEST(PersonaTest, BuildSystemPromptInjectsPreferences) {
  std::vector<UserPreference> prefs;
  prefs.push_back({"sleep schedule", "wants to sleep before 11pm", "turn-1", {}});

  const std::string prompt = BuildSystemPrompt(prefs, {});
  EXPECT_NE(prompt.find("What you know about this user"), std::string::npos);
  EXPECT_NE(prompt.find("sleep before 11pm"), std::string::npos);
}

TEST(PersonaTest, BuildSystemPromptInjectsFollowups) {
  std::vector<FollowUp> followups;
  FollowUp f;
  f.id = "f1";
  f.content = "Google interview";
  f.status = FollowUp::Status::kActive;
  followups.push_back(f);

  const std::string prompt = BuildSystemPrompt({}, followups);
  EXPECT_NE(prompt.find("past conversations"), std::string::npos);
  EXPECT_NE(prompt.find("Google interview"), std::string::npos);
}

TEST(PersonaTest, BuildSystemPromptIncludesCheckInTime) {
  std::vector<FollowUp> followups;
  FollowUp f;
  f.id = "f1";
  f.content = "dentist appointment";
  f.status = FollowUp::Status::kActive;
  f.check_in_time = std::chrono::system_clock::now() + std::chrono::hours(48);
  followups.push_back(f);

  const std::string prompt = BuildSystemPrompt({}, followups);
  EXPECT_NE(prompt.find("check in around"), std::string::npos);
}

TEST(PersonaTest, BuildSystemPromptCombinesAllSegments) {
  std::vector<UserPreference> prefs;
  prefs.push_back({"name", "Alex", "turn-0", {}});

  std::vector<FollowUp> followups;
  FollowUp f;
  f.id = "f1";
  f.content = "talk to landlord";
  f.status = FollowUp::Status::kActive;
  followups.push_back(f);

  const std::string prompt = BuildSystemPrompt(prefs, followups);
  EXPECT_NE(prompt.find("Shizuru"), std::string::npos);
  EXPECT_NE(prompt.find("Current time:"), std::string::npos);
  EXPECT_NE(prompt.find("Alex"), std::string::npos);
  EXPECT_NE(prompt.find("talk to landlord"), std::string::npos);
}

}  // namespace
}  // namespace shizuru::app
