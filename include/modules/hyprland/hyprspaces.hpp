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

inline bool isSameHyprspacesRawWorkspaceIdentity(
    int leftId, std::string_view leftName, std::string_view leftOutput, int rightId,
    std::string_view rightName, std::string_view rightOutput) {
  return leftId == rightId && leftName == rightName && leftOutput == rightOutput;
}

inline std::string_view normalizeHyprspacesSnapshotWorkspaceName(std::string_view name) {
  if (name.starts_with("special:")) {
    return name.substr(8);
  }
  return name;
}

inline bool isSameHyprspacesSnapshotWorkspace(int leftId, std::string_view leftName,
                                              int rightId, std::string_view rightName) {
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

inline std::optional<std::string> makeHyprspacesWorkspaceKeyForOffset(
    int workspaceId, std::string_view output, int pairedOffset) {
  const auto displaySlot = getHyprspacesDisplaySlotForOffset(workspaceId, pairedOffset);
  if (!displaySlot.has_value()) {
    return std::nullopt;
  }
  return std::string(output) + ":" + std::to_string(*displaySlot);
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
    const auto workspaceKey = makeHyprspacesWorkspaceKeyForOffset(
        workspace.id, workspace.output, pairedOffset);
    if (workspaceKey.has_value()) {
      return "alias:" + *workspaceKey;
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
    if (isSameHyprspacesSnapshotWorkspace(workspace.id, workspace.name, current.id,
                                          current.name)) {
      return current;
    }
  }
  return std::nullopt;
}

inline HyprspacesCoalescedEvents coalesceHyprspacesWorkspaceEvents(
    std::vector<std::string> const& removes,
    std::vector<HyprspacesQueuedWorkspace> const& creates,
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
    const bool persistentPlaceholder = workspace.persistentConfig || workspace.persistentRule ||
                                       workspace.aliasPlaceholder;
    const auto currentWorkspace = workspace.aliasPlaceholder
                                      ? std::optional<HyprspacesQueuedWorkspace>{}
                                      : findHyprspacesSnapshotWorkspace(workspace, snapshot);
    const bool currentWorkspaceDisplayable = currentWorkspace.has_value() &&
                                             currentWorkspace->displayable;
    if (!shouldKeepHyprspacesPendingCreate(persistentPlaceholder,
                                           currentWorkspace.has_value(),
                                           currentWorkspaceDisplayable)) {
      continue;
    }

    HyprspacesQueuedWorkspace coalescedWorkspace = currentWorkspaceDisplayable ? *currentWorkspace
                                                                               : workspace;
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

inline int getHyprspacesPersistentWorkspaceId(int monitorId, int amount, int pairedOffset,
                                              int index) {
  int base = monitorId * amount;
  if (pairedOffset > 0) {
    base = monitorId * pairedOffset;
  }
  return base + index + 1;
}

inline std::string selectHyprspacesPersistentPlaceholderOutput(
    std::string_view barOutput, std::string_view configuredOutput, bool allOutputs) {
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
