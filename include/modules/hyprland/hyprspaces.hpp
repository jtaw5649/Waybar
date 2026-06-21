#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace waybar::modules::hyprland {

inline bool isSameHyprspacesRawWorkspaceIdentity(int leftId, std::string_view leftName,
                                                 std::string_view leftOutput, int rightId,
                                                 std::string_view rightName,
                                                 std::string_view rightOutput) {
  return leftId == rightId && leftName == rightName && leftOutput == rightOutput;
}

inline std::string_view normalizeHyprspacesSnapshotWorkspaceName(std::string_view name) {
  if (name.starts_with("special:")) {
    return name.substr(8);
  }
  return name;
}

inline bool isSameHyprspacesSnapshotWorkspace(int leftId, std::string_view leftName, int rightId,
                                              std::string_view rightName) {
  if (leftId > 0 && rightId > 0) {
    return leftId == rightId;
  }
  return normalizeHyprspacesSnapshotWorkspaceName(leftName) ==
         normalizeHyprspacesSnapshotWorkspaceName(rightName);
}

inline bool shouldKeepHyprspacesPendingCreate(bool persistentPlaceholder, bool presentInSnapshot,
                                              bool displayable) {
  return (presentInSnapshot && displayable) || (persistentPlaceholder && !presentInSnapshot);
}

inline bool shouldCreateHyprspacesPersistentWorkspaceFallback(bool keyIsMonitor,
                                                              bool keyIsWildcard) {
  return !keyIsMonitor && !keyIsWildcard;
}

inline std::optional<int> getHyprspacesDisplaySlotForOffset(int workspaceId, int pairedOffset) {
  if (workspaceId <= 0 || pairedOffset <= 0) {
    return std::nullopt;
  }
  return ((workspaceId - 1) % pairedOffset) + 1;
}

inline std::optional<std::string> makeHyprspacesCanonicalSlotKeyForOffset(int workspaceId,
                                                                          std::string_view output,
                                                                          int pairedOffset) {
  const auto displaySlot = getHyprspacesDisplaySlotForOffset(workspaceId, pairedOffset);
  if (!displaySlot.has_value()) {
    return std::nullopt;
  }
  return std::string(output) + ":" + std::to_string(*displaySlot);
}

inline std::string makeHyprspacesWorkspaceClickDispatch(int workspaceId,
                                                        std::string_view workspaceName,
                                                        bool special, bool moveToMonitor,
                                                        int pairedOffset) {
  if (workspaceId > 0) {
    const auto displaySlot = getHyprspacesDisplaySlotForOffset(workspaceId, pairedOffset);
    if (displaySlot.has_value()) {
      return "dispatch hyprspaces:switch " + std::to_string(*displaySlot);
    }
    return std::string("dispatch ") +
           (moveToMonitor ? "focusworkspaceoncurrentmonitor " : "workspace ") +
           std::to_string(workspaceId);
  }

  if (!special) {
    return std::string("dispatch ") +
           (moveToMonitor ? "focusworkspaceoncurrentmonitor name:" : "workspace name:") +
           std::string(workspaceName);
  }

  if (workspaceId != -99) {
    return "dispatch togglespecialworkspace " + std::string(workspaceName);
  }
  return "dispatch togglespecialworkspace";
}

struct HyprspacesQueuedWorkspace {
  int id = 0;
  std::string name;
  std::string output;
  int windows = 0;
  bool persistentConfig = false;
  bool persistentRule = false;
  bool aliasPlaceholder = false;
  bool displayable = true;
};

struct HyprspacesVisibleWorkspace {
  int id = 0;
  std::string output;
};

struct HyprspacesStateIndex {
  std::unordered_set<int> visibleRawIds;
  std::unordered_set<int> occupiedRawIds;
  std::unordered_set<std::string> visibleCanonicalSlotKeys;
  std::unordered_set<std::string> occupiedCanonicalSlotKeys;
};

struct HyprspacesWorkspaceView {
  int id = 0;
  std::string name;
  std::string output;
  bool special = false;
  bool explicitAliasPlaceholder = false;
  bool useCanonicalSlotForState = false;
};

struct HyprspacesActiveContext {
  int workspaceId = 0;
  std::string workspaceName;
  std::string workspaceOutput;
  std::string specialWorkspaceName;
  bool monitorHasSpecialWorkspace = false;
  bool specialOverlay = false;
};

struct HyprspacesWorkspaceRenderState {
  bool active = false;
  bool specialActive = false;
  bool visible = false;
  std::optional<int> displaySlot;
  std::optional<std::string> displayLabel;
  std::optional<bool> empty;
};

struct HyprspacesPersistentAliasMetadata {
  int id = 0;
  std::string name;
  std::string output;
  bool persistentConfig = false;
  bool persistentRule = false;
};

inline std::optional<std::vector<std::string>> validateHyprspacesDynamicMonitorNames(
    std::vector<std::optional<std::string>> const& monitorNames) {
  std::vector<std::string> orderedMonitorNames;
  std::unordered_set<std::string> seenMonitorNames;
  orderedMonitorNames.reserve(monitorNames.size());

  for (const auto& monitorName : monitorNames) {
    if (!monitorName.has_value() || monitorName->empty() ||
        !seenMonitorNames.insert(*monitorName).second) {
      return std::nullopt;
    }
    orderedMonitorNames.push_back(*monitorName);
  }

  return orderedMonitorNames;
}

inline HyprspacesPersistentAliasMetadata makeHyprspacesPersistentAliasMetadata(
    int id, std::string name, std::string output, bool persistentConfig, bool persistentRule) {
  return {id, std::move(name), std::move(output), persistentConfig, persistentRule};
}

inline std::vector<HyprspacesPersistentAliasMetadata> makeHyprspacesDynamicPersistentAliases(
    std::vector<std::string> const& orderedMonitorNames, std::string_view barOutput,
    bool allOutputs, int pairedOffset, int workspaceCount) {
  std::vector<HyprspacesPersistentAliasMetadata> aliases;
  if (pairedOffset <= 0 || workspaceCount <= 0 || workspaceCount > pairedOffset) {
    return aliases;
  }

  for (size_t monitorSlot = 0; monitorSlot < orderedMonitorNames.size(); ++monitorSlot) {
    const auto& monitorName = orderedMonitorNames[monitorSlot];
    if (monitorName.empty() || (!allOutputs && monitorName != barOutput)) {
      continue;
    }

    const int baseWorkspaceId = static_cast<int>(monitorSlot) * pairedOffset;
    for (int index = 0; index < workspaceCount; ++index) {
      const int workspaceId = baseWorkspaceId + index + 1;
      aliases.push_back(makeHyprspacesPersistentAliasMetadata(
          workspaceId, std::to_string(workspaceId), monitorName, true, false));
    }
  }

  return aliases;
}

struct HyprspacesCoalescedCreate {
  size_t queuedIndex = 0;
  HyprspacesQueuedWorkspace workspace;
  bool refreshedFromSnapshot = false;
};

struct HyprspacesCoalescedEvents {
  std::vector<std::string> removes;
  std::vector<HyprspacesCoalescedCreate> creates;
};

inline std::optional<std::string> getHyprspacesCreateQueueKey(
    HyprspacesQueuedWorkspace const& workspace, int pairedOffset) {
  if (workspace.aliasPlaceholder) {
    const auto canonicalSlotKey =
        makeHyprspacesCanonicalSlotKeyForOffset(workspace.id, workspace.output, pairedOffset);
    if (canonicalSlotKey.has_value()) {
      return "alias:" + *canonicalSlotKey;
    }
  }

  if (workspace.id > 0) {
    return "id:" + std::to_string(workspace.id);
  }
  if (workspace.name.empty()) {
    return std::nullopt;
  }
  return "name:" + std::string(normalizeHyprspacesSnapshotWorkspaceName(workspace.name));
}

inline std::optional<HyprspacesQueuedWorkspace> findHyprspacesSnapshotWorkspace(
    HyprspacesQueuedWorkspace const& workspace,
    std::vector<HyprspacesQueuedWorkspace> const& snapshot) {
  for (const auto& current : snapshot) {
    if (isSameHyprspacesSnapshotWorkspace(workspace.id, workspace.name, current.id, current.name)) {
      return current;
    }
  }
  return std::nullopt;
}

inline HyprspacesCoalescedEvents coalesceHyprspacesWorkspaceEvents(
    std::vector<std::string> const& removes, std::vector<HyprspacesQueuedWorkspace> const& creates,
    std::vector<HyprspacesQueuedWorkspace> const& snapshot, int pairedOffset) {
  HyprspacesCoalescedEvents result;
  std::unordered_set<std::string> seenRemovals;
  for (const auto& workspaceString : removes) {
    if (seenRemovals.insert(workspaceString).second) {
      result.removes.push_back(workspaceString);
    }
  }

  std::unordered_map<std::string, size_t> queuedCreateByKey;
  for (size_t index = 0; index < creates.size(); ++index) {
    const auto& workspace = creates[index];
    const bool persistentPlaceholder =
        workspace.persistentConfig || workspace.persistentRule || workspace.aliasPlaceholder;
    const auto currentWorkspace = workspace.aliasPlaceholder
                                      ? std::optional<HyprspacesQueuedWorkspace>{}
                                      : findHyprspacesSnapshotWorkspace(workspace, snapshot);
    const bool currentWorkspaceDisplayable =
        currentWorkspace.has_value() && currentWorkspace->displayable;
    if (!shouldKeepHyprspacesPendingCreate(persistentPlaceholder, currentWorkspace.has_value(),
                                           currentWorkspaceDisplayable)) {
      continue;
    }

    HyprspacesQueuedWorkspace coalescedWorkspace =
        currentWorkspaceDisplayable ? *currentWorkspace : workspace;
    if (currentWorkspaceDisplayable) {
      coalescedWorkspace.persistentConfig = workspace.persistentConfig;
      coalescedWorkspace.persistentRule = workspace.persistentRule;
      coalescedWorkspace.aliasPlaceholder = workspace.aliasPlaceholder;
    }

    HyprspacesCoalescedCreate coalescedCreate{index, coalescedWorkspace,
                                              currentWorkspaceDisplayable};
    const auto key = getHyprspacesCreateQueueKey(coalescedCreate.workspace, pairedOffset);
    if (!key.has_value()) {
      result.creates.push_back(std::move(coalescedCreate));
      continue;
    }

    const auto queuedCreate = queuedCreateByKey.find(*key);
    if (queuedCreate == queuedCreateByKey.end()) {
      queuedCreateByKey[*key] = result.creates.size();
      result.creates.push_back(std::move(coalescedCreate));
    } else {
      result.creates[queuedCreate->second] = std::move(coalescedCreate);
    }
  }

  return result;
}

inline HyprspacesStateIndex buildHyprspacesStateIndex(
    std::vector<HyprspacesQueuedWorkspace> const& workspaceSnapshot,
    std::vector<HyprspacesVisibleWorkspace> const& visibleWorkspaces,
    std::vector<int> const& visibleRawIds, int pairedOffset) {
  HyprspacesStateIndex index;
  index.visibleRawIds.insert(visibleRawIds.begin(), visibleRawIds.end());

  for (const auto& workspace : workspaceSnapshot) {
    if (workspace.id <= 0 || workspace.windows <= 0) {
      continue;
    }

    index.occupiedRawIds.insert(workspace.id);
    auto key =
        makeHyprspacesCanonicalSlotKeyForOffset(workspace.id, workspace.output, pairedOffset);
    if (key.has_value()) {
      index.occupiedCanonicalSlotKeys.insert(*key);
    }
  }

  for (const auto& workspace : visibleWorkspaces) {
    auto key =
        makeHyprspacesCanonicalSlotKeyForOffset(workspace.id, workspace.output, pairedOffset);
    if (key.has_value()) {
      index.visibleCanonicalSlotKeys.insert(*key);
    }
  }

  return index;
}

inline HyprspacesWorkspaceRenderState classifyHyprspacesWorkspaceState(
    HyprspacesWorkspaceView const& workspace, HyprspacesActiveContext const& active,
    HyprspacesStateIndex const& index, int pairedOffset) {
  HyprspacesWorkspaceRenderState state;
  const auto canonicalSlotKey =
      makeHyprspacesCanonicalSlotKeyForOffset(workspace.id, workspace.output, pairedOffset);
  const bool isPlaceholder = canonicalSlotKey.has_value() && workspace.explicitAliasPlaceholder;
  const auto activeCanonicalSlotKey = makeHyprspacesCanonicalSlotKeyForOffset(
      active.workspaceId, active.workspaceOutput, pairedOffset);

  const bool activeByCanonicalSlot = workspace.useCanonicalSlotForState &&
                                     activeCanonicalSlotKey.has_value() &&
                                     canonicalSlotKey == activeCanonicalSlotKey;
  const bool activeByName = !workspace.useCanonicalSlotForState && !isPlaceholder &&
                            !active.workspaceName.empty() && workspace.name == active.workspaceName;
  const bool specialActive = !workspace.useCanonicalSlotForState && !isPlaceholder &&
                             workspace.special && workspace.name == active.specialWorkspaceName;
  const bool activeByRawId = !isPlaceholder && workspace.id == active.workspaceId;

  state.active = workspace.useCanonicalSlotForState
                     ? activeByCanonicalSlot
                     : activeByRawId || activeByName || specialActive;
  const bool specialOverlayActive =
      workspace.useCanonicalSlotForState ? activeByCanonicalSlot : activeByRawId || activeByName;
  state.specialActive = active.specialOverlay && active.monitorHasSpecialWorkspace &&
                        !workspace.special && specialOverlayActive;
  state.visible =
      !isPlaceholder && (workspace.useCanonicalSlotForState
                             ? canonicalSlotKey.has_value() &&
                                   index.visibleCanonicalSlotKeys.contains(*canonicalSlotKey)
                             : index.visibleRawIds.contains(workspace.id));

  if (pairedOffset > 0 && workspace.id > 0) {
    state.displaySlot = getHyprspacesDisplaySlotForOffset(workspace.id, pairedOffset);
    if (state.displaySlot.has_value()) {
      state.displayLabel = std::to_string(*state.displaySlot);
    }
    state.empty =
        isPlaceholder || (workspace.useCanonicalSlotForState
                              ? !canonicalSlotKey.has_value() ||
                                    !index.occupiedCanonicalSlotKeys.contains(*canonicalSlotKey)
                              : !index.occupiedRawIds.contains(workspace.id));
  }

  return state;
}

inline int getHyprspacesPersistentWorkspaceId(int monitorId, int amount, int pairedOffset,
                                              int index) {
  int base = monitorId * amount;
  if (pairedOffset > 0) {
    base = monitorId * pairedOffset;
  }
  return base + index + 1;
}

inline std::string selectHyprspacesPersistentPlaceholderOutput(std::string_view barOutput,
                                                               std::string_view configuredOutput,
                                                               bool allOutputs) {
  if (allOutputs && !configuredOutput.empty()) {
    return std::string(configuredOutput);
  }
  return std::string(barOutput);
}

inline std::optional<std::string> selectHyprspacesWorkspaceNamePersistentPlaceholderOutput(
    std::string_view configuredOutput, std::string_view currentOutput, bool allOutputs) {
  if (allOutputs || configuredOutput == currentOutput) {
    return std::string(configuredOutput);
  }
  return std::nullopt;
}

}  // namespace waybar::modules::hyprland
