#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "modules/hyprland/hyprspaces.hpp"

namespace hyprland = waybar::modules::hyprland;

namespace {

hyprland::HyprspacesQueuedWorkspace queuedWorkspace(int id, std::string name, std::string output,
                                                    int windows = 0, bool persistentConfig = false,
                                                    bool persistentRule = false,
                                                    bool aliasPlaceholder = false,
                                                    bool displayable = true) {
  return {id,
          std::move(name),
          std::move(output),
          windows,
          persistentConfig,
          persistentRule,
          aliasPlaceholder,
          displayable};
}

}  // namespace

TEST_CASE("Hyprspaces raw workspace identity requires same id name and output",
          "[hyprland][hyprspaces]") {
  CHECK(hyprland::isSameHyprspacesRawWorkspaceIdentity(1, "1", "DP-1", 1, "1", "DP-1"));
  CHECK_FALSE(hyprland::isSameHyprspacesRawWorkspaceIdentity(1, "1", "DP-1", 11, "11", "DP-1"));
  CHECK_FALSE(hyprland::isSameHyprspacesRawWorkspaceIdentity(1, "1", "DP-1", 1, "1", "DP-2"));
  CHECK_FALSE(hyprland::isSameHyprspacesRawWorkspaceIdentity(1, "1", "DP-1", 1, "dev", "DP-1"));
}

TEST_CASE("Hyprspaces snapshot identity follows moved real workspaces", "[hyprland][hyprspaces]") {
  CHECK(hyprland::isSameHyprspacesSnapshotWorkspace(1, "1", 1, "1"));
  CHECK(hyprland::isSameHyprspacesSnapshotWorkspace(-99, "special:magic", -99, "magic"));
  CHECK_FALSE(hyprland::isSameHyprspacesRawWorkspaceIdentity(1, "1", "DP-1", 1, "1", "DP-2"));
  CHECK_FALSE(hyprland::isSameHyprspacesSnapshotWorkspace(1, "1", 11, "11"));
}

TEST_CASE("Hyprspaces pending creates are kept only when current or persistent",
          "[hyprland][hyprspaces]") {
  CHECK(hyprland::shouldKeepHyprspacesPendingCreate(false, true, true));
  CHECK(hyprland::shouldKeepHyprspacesPendingCreate(true, false, false));
  CHECK_FALSE(hyprland::shouldKeepHyprspacesPendingCreate(false, false, false));
  CHECK_FALSE(hyprland::shouldKeepHyprspacesPendingCreate(false, true, false));
  CHECK_FALSE(hyprland::shouldKeepHyprspacesPendingCreate(true, true, false));
}

TEST_CASE("Hyprspaces coalescing applies queued workspace lifecycle", "[hyprland][hyprspaces]") {
  const std::vector<std::string> removes{"1", "1", "2"};
  const std::vector<hyprland::HyprspacesQueuedWorkspace> creates{
      queuedWorkspace(1, "1", "DP-1"),
      queuedWorkspace(2, "2", "DP-1"),
      queuedWorkspace(11, "11", "DP-1", 0, true, false, true),
  };
  const std::vector<hyprland::HyprspacesQueuedWorkspace> snapshot{
      queuedWorkspace(1, "1", "DP-2", 0, false, false, false, false),
      queuedWorkspace(2, "dev", "HDMI-A-1", 3),
  };

  const auto result = hyprland::coalesceHyprspacesWorkspaceEvents(removes, creates, snapshot, 10);

  REQUIRE(result.removes.size() == 2);
  CHECK(result.removes[0] == "1");
  CHECK(result.removes[1] == "2");

  REQUIRE(result.creates.size() == 2);
  CHECK(result.creates[0].queuedIndex == 1);
  CHECK(result.creates[0].refreshedFromSnapshot);
  CHECK(result.creates[0].workspace.id == 2);
  CHECK(result.creates[0].workspace.name == "dev");
  CHECK(result.creates[0].workspace.output == "HDMI-A-1");
  CHECK(result.creates[0].workspace.windows == 3);

  CHECK(result.creates[1].queuedIndex == 2);
  CHECK_FALSE(result.creates[1].refreshedFromSnapshot);
  CHECK(result.creates[1].workspace.id == 11);
  CHECK(result.creates[1].workspace.output == "DP-1");
  CHECK(result.creates[1].workspace.aliasPlaceholder);
}

TEST_CASE("Hyprspaces coalescing keeps duplicate real slot owners separate",
          "[hyprland][hyprspaces]") {
  const std::vector<hyprland::HyprspacesQueuedWorkspace> creates{
      queuedWorkspace(1, "1", "DP-1"),
      queuedWorkspace(11, "11", "DP-1"),
  };
  const std::vector<hyprland::HyprspacesQueuedWorkspace> snapshot{
      queuedWorkspace(1, "1", "DP-1"),
      queuedWorkspace(11, "11", "DP-1"),
  };

  const auto result = hyprland::coalesceHyprspacesWorkspaceEvents({}, creates, snapshot, 10);

  REQUIRE(result.creates.size() == 2);
  CHECK(result.creates[0].workspace.id == 1);
  CHECK(result.creates[1].workspace.id == 11);
  CHECK(hyprland::makeHyprspacesCanonicalSlotKeyForOffset(1, "DP-1", 10) ==
        hyprland::makeHyprspacesCanonicalSlotKeyForOffset(11, "DP-1", 10));
}

TEST_CASE("Hyprspaces persistent aliases are typed metadata", "[hyprland][hyprspaces]") {
  const auto alias =
      hyprland::makeHyprspacesPersistentAliasMetadata(11, "11", "HDMI-A-1", true, false);

  CHECK(alias.id == 11);
  CHECK(alias.name == "11");
  CHECK(alias.output == "HDMI-A-1");
  CHECK(alias.persistentConfig);
  CHECK_FALSE(alias.persistentRule);
  CHECK(hyprland::makeHyprspacesCanonicalSlotKeyForOffset(alias.id, alias.output, 10) ==
        "HDMI-A-1:1");
}

TEST_CASE("Hyprspaces dynamic aliases follow plugin monitor order", "[hyprland][hyprspaces]") {
  const auto aliases =
      hyprland::makeHyprspacesDynamicPersistentAliases({"DP-1", "DP-2"}, "DP-1", true, 10, 5);

  REQUIRE(aliases.size() == 10);
  for (int i = 0; i < 5; ++i) {
    CHECK(aliases[static_cast<size_t>(i)].id == i + 1);
    CHECK(aliases[static_cast<size_t>(i)].name == std::to_string(i + 1));
    CHECK(aliases[static_cast<size_t>(i)].output == "DP-1");
  }
  for (int i = 0; i < 5; ++i) {
    const auto aliasIndex = static_cast<size_t>(i + 5);
    CHECK(aliases[aliasIndex].id == i + 11);
    CHECK(aliases[aliasIndex].name == std::to_string(i + 11));
    CHECK(aliases[aliasIndex].output == "DP-2");
  }
}

TEST_CASE("Hyprspaces dynamic aliases can target only the bar output", "[hyprland][hyprspaces]") {
  const auto aliases =
      hyprland::makeHyprspacesDynamicPersistentAliases({"DP-1", "DP-2"}, "DP-2", false, 10, 5);

  REQUIRE(aliases.size() == 5);
  for (int i = 0; i < 5; ++i) {
    CHECK(aliases[static_cast<size_t>(i)].id == i + 11);
    CHECK(aliases[static_cast<size_t>(i)].output == "DP-2");
  }
}

TEST_CASE("Hyprspaces dynamic aliases use slot zero for single STREAM topology",
          "[hyprland][hyprspaces]") {
  const auto aliases =
      hyprland::makeHyprspacesDynamicPersistentAliases({"STREAM"}, "STREAM", false, 10, 5);

  REQUIRE(aliases.size() == 5);
  for (int i = 0; i < 5; ++i) {
    CHECK(aliases[static_cast<size_t>(i)].id == i + 1);
    CHECK(aliases[static_cast<size_t>(i)].output == "STREAM");
  }
}

TEST_CASE("Hyprspaces dynamic aliases use slot zero for single remaining monitor",
          "[hyprland][hyprspaces]") {
  const auto aliases =
      hyprland::makeHyprspacesDynamicPersistentAliases({"DP-2"}, "DP-2", false, 10, 5);

  REQUIRE(aliases.size() == 5);
  CHECK(aliases.front().id == 1);
  CHECK(aliases.back().id == 5);
  CHECK(aliases.front().output == "DP-2");
}

TEST_CASE("Hyprspaces same-output workspace ordering follows display slots",
          "[hyprland][hyprspaces]") {
  CHECK(hyprland::compareHyprspacesSameOutputDisplaySlot(11, "DP-1", 4, "DP-1", 10) == true);
  CHECK(hyprland::compareHyprspacesSameOutputDisplaySlot(4, "DP-1", 11, "DP-1", 10) == false);
  CHECK_FALSE(
      hyprland::compareHyprspacesSameOutputDisplaySlot(11, "DP-1", 1, "DP-1", 10).has_value());
  CHECK_FALSE(
      hyprland::compareHyprspacesSameOutputDisplaySlot(11, "DP-1", 4, "DP-2", 10).has_value());
}

TEST_CASE("Hyprspaces dynamic aliases require valid count and offset", "[hyprland][hyprspaces]") {
  CHECK(hyprland::makeHyprspacesDynamicPersistentAliases({"DP-1"}, "DP-1", false, 10, 0).empty());
  CHECK(hyprland::makeHyprspacesDynamicPersistentAliases({"DP-1"}, "DP-1", false, 0, 5).empty());
  CHECK(hyprland::makeHyprspacesDynamicPersistentAliases({"DP-1"}, "DP-1", false, 10, 11).empty());
}

TEST_CASE("Hyprspaces dynamic aliases skip missing bar output", "[hyprland][hyprspaces]") {
  CHECK(hyprland::makeHyprspacesDynamicPersistentAliases({"DP-1", "DP-2"}, "STREAM", false, 10, 5)
            .empty());
}

TEST_CASE("Hyprspaces dynamic monitor validation rejects missing names", "[hyprland][hyprspaces]") {
  CHECK_FALSE(hyprland::validateHyprspacesDynamicMonitorNames(
                  {std::nullopt, std::optional<std::string>{"DP-2"}})
                  .has_value());
}

TEST_CASE("Hyprspaces dynamic monitor validation rejects empty names", "[hyprland][hyprspaces]") {
  CHECK_FALSE(hyprland::validateHyprspacesDynamicMonitorNames({std::string{"DP-1"}, std::string{}})
                  .has_value());
}

TEST_CASE("Hyprspaces dynamic monitor validation rejects duplicate names",
          "[hyprland][hyprspaces]") {
  CHECK_FALSE(hyprland::validateHyprspacesDynamicMonitorNames(
                  {std::string{"DP-1"}, std::string{"DP-1"}, std::string{"DP-2"}})
                  .has_value());
}

TEST_CASE("Hyprspaces persistent ids follow refreshed monitor ids", "[hyprland][hyprspaces]") {
  const auto oldMonitorWorkspaceId = hyprland::getHyprspacesPersistentWorkspaceId(0, 3, 10, 0);
  const auto newMonitorWorkspaceId = hyprland::getHyprspacesPersistentWorkspaceId(2, 3, 10, 0);

  CHECK(oldMonitorWorkspaceId != newMonitorWorkspaceId);
  CHECK(hyprland::getHyprspacesDisplaySlotForOffset(oldMonitorWorkspaceId, 10) == 1);
  CHECK(hyprland::getHyprspacesDisplaySlotForOffset(newMonitorWorkspaceId, 10) == 1);
}

TEST_CASE("Hyprspaces workspace clicks dispatch paired slot switches", "[hyprland][hyprspaces]") {
  CHECK(hyprland::makeHyprspacesWorkspaceClickDispatch(13, "13", false, false, 10) ==
        "dispatch hyprspaces:switch 3");
  CHECK(hyprland::makeHyprspacesWorkspaceClickDispatch(13, "13", false, true, 10) ==
        "dispatch hyprspaces:switch 3");
  CHECK(hyprland::makeHyprspacesWorkspaceClickDispatch(3, "3", false, false, 0) ==
        "dispatch workspace 3");
  CHECK(hyprland::makeHyprspacesWorkspaceClickDispatch(3, "3", false, true, 0) ==
        "dispatch focusworkspaceoncurrentmonitor 3");
  CHECK(hyprland::makeHyprspacesWorkspaceClickDispatch(0, "dev", false, false, 10) ==
        "dispatch workspace name:dev");
  CHECK(hyprland::makeHyprspacesWorkspaceClickDispatch(-98, "magic", true, false, 10) ==
        "dispatch togglespecialworkspace magic");
  CHECK(hyprland::makeHyprspacesWorkspaceClickDispatch(-99, "special", true, false, 10) ==
        "dispatch togglespecialworkspace");
}

TEST_CASE("Hyprspaces persistent fallback skips monitor and wildcard keys",
          "[hyprland][hyprspaces]") {
  CHECK(hyprland::shouldCreateHyprspacesPersistentWorkspaceFallback(false, false));
  CHECK_FALSE(hyprland::shouldCreateHyprspacesPersistentWorkspaceFallback(true, false));
  CHECK_FALSE(hyprland::shouldCreateHyprspacesPersistentWorkspaceFallback(false, true));
}

TEST_CASE("Hyprspaces persistent placeholders use configured all outputs target",
          "[hyprland][hyprspaces]") {
  const auto output =
      hyprland::selectHyprspacesPersistentPlaceholderOutput("DP-1", "HDMI-A-1", true);
  const auto id = hyprland::getHyprspacesPersistentWorkspaceId(1, 3, 10, 0);

  CHECK(output == "HDMI-A-1");
  CHECK(id == 11);
  CHECK(hyprland::makeHyprspacesCanonicalSlotKeyForOffset(id, output, 10) == "HDMI-A-1:1");
}

TEST_CASE("Hyprspaces persistent placeholders keep bar output without explicit all outputs target",
          "[hyprland][hyprspaces]") {
  const auto output = hyprland::selectHyprspacesPersistentPlaceholderOutput("DP-1", "", true);
  const auto id = hyprland::getHyprspacesPersistentWorkspaceId(0, 5, 10, 0);

  CHECK(hyprland::selectHyprspacesPersistentPlaceholderOutput("DP-1", "HDMI-A-1", false) == "DP-1");
  CHECK(output == "DP-1");
  CHECK(id == 1);
  CHECK(hyprland::makeHyprspacesCanonicalSlotKeyForOffset(id, output, 10) == "DP-1:1");
}

TEST_CASE("Hyprspaces workspace-name persistent placeholders target configured monitors",
          "[hyprland][hyprspaces]") {
  const auto allOutputsTarget =
      hyprland::selectHyprspacesWorkspaceNamePersistentPlaceholderOutput("HDMI-A-1", "DP-1", true);
  const auto currentOutputTarget =
      hyprland::selectHyprspacesWorkspaceNamePersistentPlaceholderOutput("HDMI-A-1", "HDMI-A-1",
                                                                         false);
  const auto otherOutputTarget =
      hyprland::selectHyprspacesWorkspaceNamePersistentPlaceholderOutput("HDMI-A-1", "DP-1", false);

  REQUIRE(allOutputsTarget.has_value());
  REQUIRE(currentOutputTarget.has_value());
  CHECK(*allOutputsTarget == "HDMI-A-1");
  CHECK(*currentOutputTarget == "HDMI-A-1");
  CHECK_FALSE(otherOutputTarget.has_value());
  CHECK(hyprland::makeHyprspacesCanonicalSlotKeyForOffset(11, *allOutputsTarget, 10) ==
        "HDMI-A-1:1");
}

TEST_CASE("Hyprspaces workspace-rule placeholders select configured or bar output",
          "[hyprland][hyprspaces]") {
  const auto configuredRuleOutput =
      hyprland::selectHyprspacesPersistentPlaceholderOutput("DP-1", "HDMI-A-1", true);
  const auto emptyRuleOutput =
      hyprland::selectHyprspacesPersistentPlaceholderOutput("DP-1", "", true);

  CHECK(configuredRuleOutput == "HDMI-A-1");
  CHECK(emptyRuleOutput == "DP-1");
  CHECK(hyprland::makeHyprspacesCanonicalSlotKeyForOffset(11, configuredRuleOutput, 10) ==
        "HDMI-A-1:1");
  CHECK(hyprland::makeHyprspacesCanonicalSlotKeyForOffset(11, emptyRuleOutput, 10) == "DP-1:1");
}

TEST_CASE("Hyprspaces state index tracks raw ids and canonical slots", "[hyprland][hyprspaces]") {
  const std::vector<hyprland::HyprspacesQueuedWorkspace> snapshot{
      queuedWorkspace(1, "1", "DP-1"),
      queuedWorkspace(11, "11", "DP-1", 2),
  };
  const std::vector<hyprland::HyprspacesVisibleWorkspace> visibleSlots{{11, "DP-1"}};
  const std::vector<int> visibleRawIds{1, 11};

  const auto index = hyprland::buildHyprspacesStateIndex(snapshot, visibleSlots, visibleRawIds, 10);

  CHECK(index.visibleRawIds.contains(1));
  CHECK(index.visibleRawIds.contains(11));
  CHECK_FALSE(index.occupiedRawIds.contains(1));
  CHECK(index.occupiedRawIds.contains(11));
  CHECK(index.visibleCanonicalSlotKeys.contains("DP-1:1"));
  CHECK(index.occupiedCanonicalSlotKeys.contains("DP-1:1"));
}

TEST_CASE("Hyprspaces aliases classify as explicit empty placeholders", "[hyprland][hyprspaces]") {
  hyprland::HyprspacesStateIndex index;
  index.visibleRawIds.insert(1);
  index.visibleCanonicalSlotKeys.insert("DP-1:1");
  index.occupiedCanonicalSlotKeys.insert("DP-1:1");

  const hyprland::HyprspacesWorkspaceView placeholder{1, "1", "DP-1", false, true, false};
  const hyprland::HyprspacesWorkspaceView canonicalOwner{1, "1", "DP-1", false, false, true};
  const hyprland::HyprspacesActiveContext active{11, "11", "DP-1", "", false, true};

  const auto canonicalState =
      hyprland::classifyHyprspacesWorkspaceState(canonicalOwner, active, index, 10);
  const auto state = hyprland::classifyHyprspacesWorkspaceState(placeholder, active, index, 10);

  CHECK(canonicalState.active);
  CHECK(canonicalState.visible);
  REQUIRE(canonicalState.empty.has_value());
  CHECK_FALSE(*canonicalState.empty);

  CHECK_FALSE(state.active);
  CHECK_FALSE(state.visible);
  REQUIRE(state.displaySlot.has_value());
  CHECK(*state.displaySlot == 1);
  REQUIRE(state.displayLabel.has_value());
  CHECK(*state.displayLabel == "1");
  REQUIRE(state.empty.has_value());
  CHECK(*state.empty);
}

TEST_CASE("Hyprspaces duplicate real owners classify by raw identity", "[hyprland][hyprspaces]") {
  hyprland::HyprspacesStateIndex index;
  index.visibleRawIds.insert(11);
  index.occupiedRawIds.insert(11);
  index.visibleCanonicalSlotKeys.insert("DP-1:1");
  index.occupiedCanonicalSlotKeys.insert("DP-1:1");

  const hyprland::HyprspacesActiveContext active{11, "11", "DP-1", "", false, false};
  const hyprland::HyprspacesWorkspaceView rawOne{1, "1", "DP-1", false, false, false};
  const hyprland::HyprspacesWorkspaceView rawEleven{11, "11", "DP-1", false, false, false};

  const auto firstState = hyprland::classifyHyprspacesWorkspaceState(rawOne, active, index, 10);
  const auto secondState = hyprland::classifyHyprspacesWorkspaceState(rawEleven, active, index, 10);

  CHECK_FALSE(firstState.active);
  CHECK_FALSE(firstState.visible);
  REQUIRE(firstState.empty.has_value());
  CHECK(*firstState.empty);

  CHECK(secondState.active);
  CHECK(secondState.visible);
  REQUIRE(secondState.empty.has_value());
  CHECK_FALSE(*secondState.empty);
}
