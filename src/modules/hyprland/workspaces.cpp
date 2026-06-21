#include "modules/hyprland/workspaces.hpp"

#include <json/value.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "modules/hyprland/hyprspaces.hpp"
#include "util/regex_collection.hpp"
#include "util/string.hpp"

namespace waybar::modules::hyprland {

bool isDoubleSpecial(std::string const& workspace_name);

Workspaces::Workspaces(const std::string& id, const Bar& bar, const Json::Value& config)
    : AModule(config, "workspaces", id, false, false),
      m_bar(bar),
      m_box(bar.orientation, 0),
      m_ipc(IPC::inst()) {
  parseConfig(config);

  m_box.set_name("workspaces");
  if (!id.empty()) {
    m_box.get_style_context()->add_class(id);
  }
  m_box.get_style_context()->add_class(MODULE_CLASS);
  event_box_.add(m_box);

  init();
  registerIpc();
}

Workspaces::~Workspaces() {
  m_ipc.unregisterForIPC(this);
  // wait for possible event handler to finish
  std::lock_guard<std::mutex> lg(m_mutex);
}

void Workspaces::init() {
  setCurrentMonitorId();
  m_activeWorkspaceId = m_ipc.getSocket1JsonReply("activeworkspace")["id"].asInt();

  initializeWorkspaces();

  if (barScroll() && !m_scrollConnected) {
    auto& window = const_cast<Bar&>(m_bar).window;
    window.add_events(Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK);
    window.signal_scroll_event().connect(sigc::mem_fun(*this, &Workspaces::handleScroll));
    m_scrollConnected = true;
  }

  dp.emit();
}

Json::Value Workspaces::createMonitorWorkspaceData(std::string const& name,
                                                   std::string const& monitor) {
  spdlog::trace("Creating persistent workspace: {} on monitor {}", name, monitor);
  Json::Value workspaceData;

  auto workspaceId = parseWorkspaceId(name);
  if (!workspaceId.has_value()) {
    workspaceId = 0;
  }
  workspaceData["id"] = *workspaceId;
  workspaceData["name"] = name;
  workspaceData["monitor"] = monitor;
  workspaceData["windows"] = 0;
  return workspaceData;
}

std::optional<std::string> Workspaces::getHyprspacesCanonicalSlotKey(
    int workspaceId, std::string const& output) const {
  return makeHyprspacesCanonicalSlotKeyForOffset(workspaceId, output, m_hyprspacesPairedOffset);
}

std::optional<std::string> Workspaces::getMonitorName(int monitorId) const {
  const auto monitors = m_ipc.getSocket1JsonReply("monitors");
  auto monitor = std::ranges::find_if(monitors, [&](Json::Value const& value) {
    return value["id"].isInt() && value["id"].asInt() == monitorId;
  });
  if (monitor == monitors.end() || !(*monitor)["name"].isString()) {
    return std::nullopt;
  }
  return (*monitor)["name"].asString();
}

std::optional<std::string> Workspaces::getClientOutput(Json::Value const& clientData) const {
  if (clientData["workspace"].isObject() && clientData["workspace"]["monitor"].isString()) {
    return clientData["workspace"]["monitor"].asString();
  }
  if (clientData["monitor"].isString()) {
    return clientData["monitor"].asString();
  }
  if (clientData["monitor"].isInt()) {
    return getMonitorName(clientData["monitor"].asInt());
  }
  return std::nullopt;
}

std::optional<std::string> Workspaces::getClientHyprspacesCanonicalSlotKey(
    Json::Value const& clientData) const {
  if (!clientData["workspace"].isObject() || !clientData["workspace"]["id"].isInt()) {
    return std::nullopt;
  }
  const auto output = getClientOutput(clientData);
  if (!output.has_value()) {
    return std::nullopt;
  }
  return getHyprspacesCanonicalSlotKey(clientData["workspace"]["id"].asInt(), *output);
}

std::optional<std::string> Workspaces::getWindowPayloadOutput(
    WindowCreationPayload const& windowPayload) const {
  if (windowPayload.getWorkspaceOutput().has_value()) {
    return windowPayload.getWorkspaceOutput();
  }
  if (windowPayload.getMonitorId().has_value()) {
    return getMonitorName(*windowPayload.getMonitorId());
  }
  return std::nullopt;
}

std::optional<std::string> Workspaces::getWindowPayloadHyprspacesCanonicalSlotKey(
    WindowCreationPayload const& windowPayload) const {
  auto workspaceId = windowPayload.getWorkspaceId();
  if (!workspaceId.has_value()) {
    workspaceId = parseWorkspaceId(windowPayload.getWorkspaceName());
  }
  const auto output = getWindowPayloadOutput(windowPayload);
  if (!workspaceId.has_value() || !output.has_value()) {
    return std::nullopt;
  }
  return getHyprspacesCanonicalSlotKey(*workspaceId, *output);
}

bool Workspaces::hasDuplicateHyprspacesRealOwners(std::string const& canonicalSlotKey) const {
  int realOwners = 0;
  for (auto const& workspace : m_workspaces) {
    if (isHyprspacesPersistentAliasPlaceholder(*workspace)) {
      continue;
    }

    const auto workspaceCanonicalSlotKey =
        getHyprspacesCanonicalSlotKey(workspace->id(), workspace->output());
    if (workspaceCanonicalSlotKey.has_value() && *workspaceCanonicalSlotKey == canonicalSlotKey &&
        ++realOwners > 1) {
      return true;
    }
  }

  return false;
}

bool Workspaces::shouldUseHyprspacesCanonicalSlotForState(Workspace const& workspace) const {
  if (isHyprspacesPersistentAliasPlaceholder(workspace)) {
    return false;
  }

  const auto workspaceCanonicalSlotKey =
      getHyprspacesCanonicalSlotKey(workspace.id(), workspace.output());
  return workspaceCanonicalSlotKey.has_value() &&
         !hasDuplicateHyprspacesRealOwners(*workspaceCanonicalSlotKey);
}

bool Workspaces::shouldDisplayWorkspaceData(Json::Value const& workspaceData) {
  const std::string workspaceName = workspaceData["name"].asString();
  return (allOutputs() || m_bar.output->name == workspaceData["monitor"].asString()) &&
         (!workspaceName.starts_with("special") || showSpecialWorkspaceEntries()) &&
         !isDoubleSpecial(workspaceName) && !isWorkspaceIgnored(workspaceName);
}

bool Workspaces::isWorkspaceJsonRawMatch(Workspace const& workspace,
                                         Json::Value const& workspaceData) const {
  if (workspace.id() > 0 && workspaceData["id"].isInt() &&
      workspaceData["id"].asInt() == workspace.id()) {
    return true;
  }

  const auto workspaceName = workspaceData["name"].asString();
  return workspaceName == workspace.name() ||
         (workspace.isSpecial() && workspaceName == "special:" + workspace.name());
}

bool Workspaces::isWorkspaceJsonSameRealWorkspace(Workspace const& workspace,
                                                  Json::Value const& workspaceData) const {
  return !isHyprspacesPersistentAliasPlaceholder(workspace) && workspaceData["id"].isInt() &&
         workspaceData["name"].isString() &&
         isSameHyprspacesSnapshotWorkspace(workspace.id(), workspace.name(),
                                           workspaceData["id"].asInt(),
                                           workspaceData["name"].asString());
}

bool Workspaces::isWorkspaceJsonMatch(Workspace const& workspace,
                                      Json::Value const& workspaceData) const {
  const auto workspaceCanonicalSlotKey =
      getHyprspacesCanonicalSlotKey(workspace.id(), workspace.output());
  if (workspaceCanonicalSlotKey.has_value()) {
    if (isHyprspacesPersistentAliasPlaceholder(workspace)) {
      return false;
    }
    if (!shouldUseHyprspacesCanonicalSlotForState(workspace)) {
      return isWorkspaceJsonRawMatch(workspace, workspaceData);
    }
    if (!workspaceData["id"].isInt() || !workspaceData["monitor"].isString()) {
      return false;
    }

    const auto candidateCanonicalSlotKey = getHyprspacesCanonicalSlotKey(
        workspaceData["id"].asInt(), workspaceData["monitor"].asString());
    return candidateCanonicalSlotKey.has_value() &&
           candidateCanonicalSlotKey == workspaceCanonicalSlotKey;
  }

  return isWorkspaceJsonRawMatch(workspace, workspaceData);
}

bool Workspaces::isWindowInWorkspace(Workspace const& workspace,
                                     Json::Value const& clientData) const {
  const auto workspaceCanonicalSlotKey =
      getHyprspacesCanonicalSlotKey(workspace.id(), workspace.output());
  if (workspaceCanonicalSlotKey.has_value()) {
    if (isHyprspacesPersistentAliasPlaceholder(workspace)) {
      return false;
    }

    const auto clientKey = getClientHyprspacesCanonicalSlotKey(clientData);
    if (shouldUseHyprspacesCanonicalSlotForState(workspace) && clientKey.has_value()) {
      return clientKey == workspaceCanonicalSlotKey;
    }
  }

  return clientData["workspace"].isObject() && clientData["workspace"]["id"].isInt() &&
         clientData["workspace"]["id"].asInt() == workspace.id();
}

bool Workspaces::isWindowInWorkspace(Workspace const& workspace,
                                     WindowCreationPayload const& windowPayload) const {
  const auto workspaceCanonicalSlotKey =
      getHyprspacesCanonicalSlotKey(workspace.id(), workspace.output());
  if (workspaceCanonicalSlotKey.has_value()) {
    if (isHyprspacesPersistentAliasPlaceholder(workspace)) {
      return false;
    }

    const auto payloadKey = getWindowPayloadHyprspacesCanonicalSlotKey(windowPayload);
    if (shouldUseHyprspacesCanonicalSlotForState(workspace) && payloadKey.has_value()) {
      return payloadKey == workspaceCanonicalSlotKey;
    }

    if (windowPayload.getWorkspaceId().has_value()) {
      return *windowPayload.getWorkspaceId() == workspace.id();
    }
  }

  return windowPayload.getWorkspaceName() == workspace.name();
}

std::optional<HyprspacesPersistentAliasMetadata> Workspaces::readHyprspacesPersistentAliasMetadata(
    Json::Value const& workspaceData) const {
  if (!workspaceData["id"].isInt() || !workspaceData["name"].isString() ||
      !workspaceData["monitor"].isString()) {
    return std::nullopt;
  }

  return makeHyprspacesPersistentAliasMetadata(
      workspaceData["id"].asInt(), workspaceData["name"].asString(),
      workspaceData["monitor"].asString(), workspaceData["persistent-config"].asBool(),
      workspaceData["persistent-rule"].asBool());
}

Json::Value Workspaces::createHyprspacesExplicitAliasPlaceholderData(
    HyprspacesPersistentAliasMetadata const& alias) const {
  auto workspaceData = createMonitorWorkspaceData(alias.name, alias.output);
  workspaceData["id"] = alias.id;
  workspaceData["persistent-config"] = alias.persistentConfig;
  workspaceData["persistent-rule"] = alias.persistentRule;
  workspaceData[HYPRSPACES_EXPLICIT_ALIAS_PLACEHOLDER_KEY] = true;
  return workspaceData;
}

void Workspaces::rememberHyprspacesPersistentAlias(HyprspacesPersistentAliasMetadata alias) {
  auto key = getHyprspacesCanonicalSlotKey(alias.id, alias.output);
  if (key.has_value()) {
    m_hyprspacesPersistentAliases[*key] = std::move(alias);
  }
}

bool Workspaces::isHyprspacesExplicitAliasPlaceholderInput(Json::Value const& workspaceData) const {
  return workspaceData[HYPRSPACES_EXPLICIT_ALIAS_PLACEHOLDER_KEY].asBool();
}

bool Workspaces::isHyprspacesPersistentAliasPlaceholder(Workspace const& workspace) const {
  return workspace.isHyprspacesPersistentAliasPlaceholder();
}

void Workspaces::reconcileHyprspacesCanonicalSlotKey(std::string const& key) {
  if (key.empty() || m_hyprspacesPairedOffset <= 0 || m_reconcilingHyprspacesCanonicalSlotKey) {
    return;
  }

  struct ReconcileGuard {
    bool& reconciling;
    ~ReconcileGuard() { reconciling = false; }
  } guard{m_reconcilingHyprspacesCanonicalSlotKey};
  m_reconcilingHyprspacesCanonicalSlotKey = true;

  std::vector<size_t> matchingWorkspaces;
  for (size_t i = 0; i < m_workspaces.size(); ++i) {
    const auto workspaceCanonicalSlotKey =
        getHyprspacesCanonicalSlotKey(m_workspaces[i]->id(), m_workspaces[i]->output());
    if (workspaceCanonicalSlotKey.has_value() && *workspaceCanonicalSlotKey == key) {
      matchingWorkspaces.push_back(i);
    }
  }

  const auto alias = m_hyprspacesPersistentAliases.find(key);
  if (matchingWorkspaces.empty()) {
    if (alias != m_hyprspacesPersistentAliases.end()) {
      createWorkspace(createHyprspacesExplicitAliasPlaceholderData(alias->second));
    }
    return;
  }

  std::vector<size_t> keptRealWorkspaces;
  std::unordered_set<size_t> duplicateRawWorkspaces;
  for (const size_t index : matchingWorkspaces) {
    if (isHyprspacesPersistentAliasPlaceholder(*m_workspaces[index])) {
      continue;
    }

    const bool sameRawIdentityAlreadyKept =
        std::ranges::any_of(keptRealWorkspaces, [&](size_t keptIndex) {
          return isSameHyprspacesRawWorkspaceIdentity(
              m_workspaces[index]->id(), m_workspaces[index]->name(), m_workspaces[index]->output(),
              m_workspaces[keptIndex]->id(), m_workspaces[keptIndex]->name(),
              m_workspaces[keptIndex]->output());
        });
    if (sameRawIdentityAlreadyKept) {
      duplicateRawWorkspaces.insert(index);
    } else {
      keptRealWorkspaces.push_back(index);
    }
  }

  size_t owner = matchingWorkspaces.front();
  if (alias != m_hyprspacesPersistentAliases.end()) {
    auto realWorkspace = std::ranges::find_if(matchingWorkspaces, [&](size_t index) {
      return !isHyprspacesPersistentAliasPlaceholder(*m_workspaces[index]);
    });
    if (realWorkspace != matchingWorkspaces.end()) {
      owner = *realWorkspace;
    }
  }

  for (auto it = matchingWorkspaces.rbegin(); it != matchingWorkspaces.rend(); ++it) {
    const size_t index = *it;
    if (index == owner) {
      continue;
    }
    if (!isHyprspacesPersistentAliasPlaceholder(*m_workspaces[index]) &&
        !duplicateRawWorkspaces.contains(index)) {
      spdlog::debug("Leaving duplicate real workspace id={} for hyprspaces slot {}",
                    m_workspaces[index]->id(), key);
      continue;
    }
    spdlog::debug("Removing duplicate workspace id={} for hyprspaces slot {}",
                  m_workspaces[index]->id(), key);
    m_box.remove(m_workspaces[index]->button());
    m_workspaces.erase(m_workspaces.begin() + index);
  }
}

bool Workspaces::isWorkspacePersistent(Workspace const& workspace) const {
  const auto workspaceCanonicalSlotKey =
      getHyprspacesCanonicalSlotKey(workspace.id(), workspace.output());
  if (workspaceCanonicalSlotKey.has_value()) {
    return m_hyprspacesPersistentAliases.contains(*workspaceCanonicalSlotKey);
  }

  return workspace.isPersistent();
}

Workspace* Workspaces::createWorkspace(Json::Value const& workspace_data,
                                       Json::Value const& clients_data, bool initializeWindowMap) {
  auto workspaceName = workspace_data["name"].asString();
  auto workspaceId = workspace_data["id"].asInt();
  auto workspaceOutput = workspace_data["monitor"].asString();
  const auto hyprspacesCanonicalSlotKey =
      getHyprspacesCanonicalSlotKey(workspaceId, workspaceOutput);
  const bool isHyprspacesExplicitAliasPlaceholder =
      hyprspacesCanonicalSlotKey.has_value() &&
      isHyprspacesExplicitAliasPlaceholderInput(workspace_data);
  Json::Value workspaceData = workspace_data;
  if (hyprspacesCanonicalSlotKey.has_value()) {
    if (isHyprspacesExplicitAliasPlaceholder) {
      if (auto alias = readHyprspacesPersistentAliasMetadata(workspace_data); alias.has_value()) {
        rememberHyprspacesPersistentAlias(std::move(*alias));
      }
    }
    workspaceData["persistent-config"] = false;
    workspaceData["persistent-rule"] = false;
  }
  spdlog::debug("Creating workspace {}", workspaceName);

  // avoid recreating existing workspaces
  auto workspace = std::ranges::find_if(m_workspaces, [&](std::unique_ptr<Workspace> const& w) {
    if (workspaceId > 0) {
      if (!hyprspacesCanonicalSlotKey.has_value()) {
        return w->id() == workspaceId;
      }

      const auto existingCanonicalSlotKey = getHyprspacesCanonicalSlotKey(w->id(), w->output());
      const bool sameRawIdentity = isSameHyprspacesRawWorkspaceIdentity(
          w->id(), w->name(), w->output(), workspaceId, workspaceName, workspaceOutput);
      if (sameRawIdentity && !isHyprspacesPersistentAliasPlaceholder(*w) &&
          !isHyprspacesExplicitAliasPlaceholder) {
        return true;
      }

      if (existingCanonicalSlotKey.has_value() &&
          existingCanonicalSlotKey == hyprspacesCanonicalSlotKey) {
        return isHyprspacesPersistentAliasPlaceholder(*w) || isHyprspacesExplicitAliasPlaceholder;
      }

      return w->id() == workspaceId && !isHyprspacesExplicitAliasPlaceholder;
    }
    return (workspaceName.starts_with("special:") && workspaceName.substr(8) == w->name()) ||
           workspaceName == w->name();
  });

  if (workspace != m_workspaces.end() && hyprspacesCanonicalSlotKey.has_value() &&
      !isHyprspacesExplicitAliasPlaceholder &&
      isHyprspacesPersistentAliasPlaceholder(**workspace)) {
    spdlog::debug(
        "Replacing alias placeholder id={} with real workspace id={} for hyprspaces slot {}",
        (*workspace)->id(), workspaceId, *hyprspacesCanonicalSlotKey);
    m_box.remove((*workspace)->button());
    m_workspaces.erase(workspace);
    workspace = m_workspaces.end();
  }

  if (workspace != m_workspaces.end()) {
    // don't recreate workspace, but update persistency if necessary
    const auto keys = workspaceData.getMemberNames();

    const auto* k = "persistent-rule";
    if (std::ranges::find(keys, k) != keys.end()) {
      spdlog::debug("Set dynamic persistency of workspace {} to: {}", workspaceName,
                    workspaceData[k].asBool() ? "true" : "false");
      (*workspace)->setPersistentRule(workspaceData[k].asBool());
    }

    k = "persistent-config";
    if (std::ranges::find(keys, k) != keys.end()) {
      spdlog::debug("Set config persistency of workspace {} to: {}", workspaceName,
                    workspaceData[k].asBool() ? "true" : "false");
      (*workspace)->setPersistentConfig(workspaceData[k].asBool());
    }

    if (hyprspacesCanonicalSlotKey.has_value()) {
      reconcileHyprspacesCanonicalSlotKey(*hyprspacesCanonicalSlotKey);
    }

    return nullptr;
  }

  // create new workspace
  m_workspaces.emplace_back(std::make_unique<Workspace>(workspaceData, *this));
  auto* createdWorkspace = m_workspaces.back().get();
  if (initializeWindowMap) {
    createdWorkspace->initializeWindowMap(clients_data);
  }
  Gtk::Button& newWorkspaceButton = m_workspaces.back()->button();
  m_box.pack_start(newWorkspaceButton, false, false);
  sortWorkspaces();
  newWorkspaceButton.show_all();
  if (hyprspacesCanonicalSlotKey.has_value()) {
    reconcileHyprspacesCanonicalSlotKey(*hyprspacesCanonicalSlotKey);
  }
  return createdWorkspace;
}

void Workspaces::createWorkspacesToCreate() {
  struct PendingWindowMapInit {
    Workspace* workspace;
    int id;
    std::string name;
    std::string output;
    Json::Value clientsData;
  };

  std::vector<PendingWindowMapInit> pendingWindowMapInits;
  for (const auto& [workspaceData, clientsData] : m_workspacesToCreate) {
    auto* createdWorkspace = createWorkspace(workspaceData, clientsData, false);
    if (createdWorkspace != nullptr) {
      pendingWindowMapInits.push_back({createdWorkspace, createdWorkspace->id(),
                                       createdWorkspace->name(), createdWorkspace->output(),
                                       clientsData});
    }
  }

  for (const auto& pendingInit : pendingWindowMapInits) {
    auto workspace = std::ranges::find_if(m_workspaces, [&](const auto& current) {
      return current.get() == pendingInit.workspace && current->id() == pendingInit.id &&
             current->name() == pendingInit.name && current->output() == pendingInit.output;
    });
    if (workspace != m_workspaces.end()) {
      (*workspace)->initializeWindowMap(pendingInit.clientsData);
    }
  }

  if (!m_workspacesToCreate.empty()) {
    updateWindowCount();
    sortWorkspaces();
  }
  m_workspacesToCreate.clear();
}

void Workspaces::coalescePendingWorkspaceEvents() {
  auto makeQueuedWorkspace = [](Json::Value const& workspaceData) {
    HyprspacesQueuedWorkspace workspace;
    workspace.id = workspaceData["id"].asInt();
    workspace.name = workspaceData["name"].asString();
    workspace.output = workspaceData["monitor"].asString();
    workspace.windows = workspaceData["windows"].asInt();
    workspace.persistentConfig = workspaceData["persistent-config"].asBool();
    workspace.persistentRule = workspaceData["persistent-rule"].asBool();
    workspace.aliasPlaceholder = workspaceData[HYPRSPACES_EXPLICIT_ALIAS_PLACEHOLDER_KEY].asBool();
    return workspace;
  };

  if (m_workspacesToCreate.empty()) {
    m_workspacesToRemove =
        coalesceHyprspacesWorkspaceEvents(m_workspacesToRemove, {}, {}, m_hyprspacesPairedOffset)
            .removes;
    return;
  }

  const auto workspacesJson = m_ipc.getSocket1JsonReply("workspaces");
  std::vector<HyprspacesQueuedWorkspace> queuedCreates;
  queuedCreates.reserve(m_workspacesToCreate.size());
  for (const auto& [workspaceData, _] : m_workspacesToCreate) {
    queuedCreates.push_back(makeQueuedWorkspace(workspaceData));
  }

  std::vector<HyprspacesQueuedWorkspace> snapshot;
  for (const auto& workspaceData : workspacesJson) {
    auto workspace = makeQueuedWorkspace(workspaceData);
    workspace.displayable = shouldDisplayWorkspaceData(workspaceData);
    snapshot.push_back(std::move(workspace));
  }

  const auto coalesced = coalesceHyprspacesWorkspaceEvents(m_workspacesToRemove, queuedCreates,
                                                           snapshot, m_hyprspacesPairedOffset);
  m_workspacesToRemove = coalesced.removes;

  std::vector<std::pair<Json::Value, Json::Value>> workspaceCreates;
  workspaceCreates.reserve(coalesced.creates.size());
  for (const auto& create : coalesced.creates) {
    Json::Value workspaceData = m_workspacesToCreate[create.queuedIndex].first;
    if (create.refreshedFromSnapshot) {
      workspaceData["id"] = create.workspace.id;
      workspaceData["name"] = create.workspace.name;
      workspaceData["monitor"] = create.workspace.output;
      workspaceData["windows"] = create.workspace.windows;
    }
    workspaceCreates.emplace_back(std::move(workspaceData),
                                  m_workspacesToCreate[create.queuedIndex].second);
  }

  m_workspacesToCreate = std::move(workspaceCreates);
}

/**
 *  Workspaces::doUpdate - update workspaces in UI thread.
 *
 * Note: some memberfields are modified by both UI thread and event listener thread, use m_mutex to
 *       protect these member fields, and lock should released before calling AModule::update().
 */
void Workspaces::doUpdate() {
  std::unique_lock lock(m_mutex);

  coalescePendingWorkspaceEvents();
  removeWorkspacesToRemove();
  createWorkspacesToCreate();
  updateWorkspaceStates();
  updateWindowCount();
  sortWorkspaces();

  bool anyWindowCreated = updateWindowsToCreate();

  if (anyWindowCreated) {
    dp.emit();
  }
}

void Workspaces::extendOrphans(int workspaceId, Json::Value const& clientsJson) {
  spdlog::trace("Extending orphans with workspace {}", workspaceId);
  for (const auto& client : clientsJson) {
    if (client["workspace"]["id"].asInt() == workspaceId) {
      registerOrphanWindow({client});
    }
  }
}

std::string Workspaces::getRewrite(std::string window_class, std::string window_title) {
  std::string windowReprKey;
  if (windowRewriteConfigUsesTitle()) {
    windowReprKey = fmt::format("class<{}> title<{}>", window_class, window_title);
  } else {
    windowReprKey = fmt::format("class<{}>", window_class);
  }
  auto const rewriteRule = m_windowRewriteRules.get(windowReprKey);
  return fmt::format(fmt::runtime(rewriteRule), fmt::arg("class", window_class),
                     fmt::arg("title", window_title));
}

std::vector<int> Workspaces::getVisibleWorkspaces() {
  std::vector<int> visibleWorkspaces;
  auto monitors = IPC::inst().getSocket1JsonReply("monitors");
  for (const auto& monitor : monitors) {
    auto ws = monitor["activeWorkspace"];
    if (ws.isObject() && ws["id"].isInt()) {
      visibleWorkspaces.push_back(ws["id"].asInt());
    }
    auto sws = monitor["specialWorkspace"];
    auto name = sws["name"].asString();
    if (sws.isObject() && sws["id"].isInt() && !name.empty()) {
      visibleWorkspaces.push_back(sws["id"].asInt());
    }
  }
  return visibleWorkspaces;
}

void Workspaces::initializeWorkspaces() {
  spdlog::debug("Initializing workspaces");
  m_hyprspacesPersistentAliases.clear();

  // if the workspace rules changed since last initialization, make sure we reset everything:
  for (auto& workspace : m_workspaces) {
    m_workspacesToRemove.push_back(std::to_string(workspace->id()));
  }

  // get all current workspaces
  auto const workspacesJson = m_ipc.getSocket1JsonReply("workspaces");
  auto const clientsJson = m_ipc.getSocket1JsonReply("clients");

  for (Json::Value workspaceJson : workspacesJson) {
    if (shouldDisplayWorkspaceData(workspaceJson)) {
      m_workspacesToCreate.emplace_back(workspaceJson, clientsJson);
    } else {
      extendOrphans(workspaceJson["id"].asInt(), clientsJson);
    }
  }

  spdlog::debug("Initializing persistent workspaces");
  if (m_hyprspacesDynamicWorkspaces) {
    loadHyprspacesDynamicWorkspaces(clientsJson);
  } else if (m_persistentWorkspaceConfig.isObject()) {
    // a persistent workspace config is defined, so use that instead of workspace rules
    loadPersistentWorkspacesFromConfig(clientsJson);
  }
  // load Hyprland's workspace rules
  loadPersistentWorkspacesFromWorkspaceRules(clientsJson);
}

void Workspaces::loadHyprspacesDynamicWorkspaces(Json::Value const& clientsJson) {
  spdlog::info("Loading hyprspaces dynamic workspaces from plugin status");
  if (m_hyprspacesWorkspaceCount <= 0) {
    spdlog::warn(
        "hyprspaces-dynamic-workspaces is enabled but hyprspaces-workspace-count is not a "
        "positive integer; no dynamic workspaces will be created");
    return;
  }
  if (m_hyprspacesPairedOffset <= 0) {
    spdlog::warn(
        "hyprspaces-dynamic-workspaces is enabled but hyprspaces-paired-offset is not a "
        "positive integer; no dynamic workspaces will be created");
    return;
  }
  if (m_hyprspacesWorkspaceCount > m_hyprspacesPairedOffset) {
    spdlog::warn(
        "hyprspaces-workspace-count ({}) must not exceed hyprspaces-paired-offset ({}); no "
        "dynamic workspaces will be created",
        m_hyprspacesWorkspaceCount, m_hyprspacesPairedOffset);
    return;
  }

  Json::Value status;
  try {
    status = m_ipc.getSocket1JsonReply("hyprspaces");
  } catch (const std::exception& e) {
    spdlog::warn("Failed to query hyprspaces plugin status: {}", e.what());
    return;
  }

  if (!status.isObject()) {
    spdlog::warn(
        "hyprspaces plugin status is not a JSON object; no dynamic workspaces will be created");
    return;
  }
  if (!status["topologyValid"].isBool() || !status["topologyValid"].asBool()) {
    const auto topologyError = status["topologyError"].isString()
                                   ? status["topologyError"].asString()
                                   : "unknown topology error";
    spdlog::warn("hyprspaces topology is invalid: {}; no dynamic workspaces will be created",
                 topologyError);
    return;
  }
  if (!status["pairedOffset"].isInt() || status["pairedOffset"].asInt() <= 0) {
    spdlog::warn(
        "hyprspaces plugin status has an invalid pairedOffset; no dynamic workspaces will be "
        "created");
    return;
  }
  if (status["pairedOffset"].asInt() != m_hyprspacesPairedOffset) {
    spdlog::warn(
        "hyprspaces plugin pairedOffset ({}) differs from hyprspaces-paired-offset config ({}); "
        "no dynamic workspaces will be created",
        status["pairedOffset"].asInt(), m_hyprspacesPairedOffset);
    return;
  }

  const auto monitors = status["monitors"];
  if (!monitors.isArray()) {
    spdlog::warn(
        "hyprspaces plugin status has no monitor array; no dynamic workspaces will be created");
    return;
  }

  std::vector<std::optional<std::string>> monitorNames;
  monitorNames.reserve(monitors.size());
  for (const auto& monitor : monitors) {
    if (!monitor.isObject() || !monitor["name"].isString()) {
      monitorNames.push_back(std::nullopt);
      continue;
    }

    monitorNames.push_back(monitor["name"].asString());
  }
  const auto orderedMonitorNames = validateHyprspacesDynamicMonitorNames(monitorNames);
  if (!orderedMonitorNames.has_value()) {
    spdlog::warn(
        "hyprspaces plugin status has malformed, empty, or duplicate monitor names; no dynamic "
        "workspaces will be created");
    return;
  }
  if (orderedMonitorNames->empty()) {
    spdlog::warn(
        "hyprspaces plugin status has no valid monitors; no dynamic workspaces will be created");
    return;
  }

  const auto aliases =
      makeHyprspacesDynamicPersistentAliases(*orderedMonitorNames, m_bar.output->name, allOutputs(),
                                             m_hyprspacesPairedOffset, m_hyprspacesWorkspaceCount);
  if (aliases.empty()) {
    spdlog::warn("hyprspaces dynamic workspace generation produced no aliases for bar output '{}'",
                 m_bar.output->name);
    return;
  }

  for (const auto& alias : aliases) {
    rememberHyprspacesPersistentAlias(alias);
    m_workspacesToCreate.emplace_back(createHyprspacesExplicitAliasPlaceholderData(alias),
                                      clientsJson);
  }
}

bool isDoubleSpecial(std::string const& workspace_name) {
  // Hyprland's IPC sometimes reports the creation of workspaces strangely named
  // `special:special:<some_name>`. This function checks for that and is used
  // to avoid creating (and then removing) such workspaces.
  // See hyprwm/Hyprland#3424 for more info.
  return workspace_name.find("special:special:") != std::string::npos;
}

bool Workspaces::isWorkspaceIgnored(std::string const& name) {
  for (auto& rule : m_ignoreWorkspaces) {
    if (std::regex_match(name, rule)) {
      return true;
      break;
    }
  }

  return false;
}

void Workspaces::loadPersistentWorkspacesFromConfig(Json::Value const& clientsJson) {
  spdlog::info("Loading persistent workspaces from Waybar config");
  const std::vector<std::string> keys = m_persistentWorkspaceConfig.getMemberNames();
  struct PersistentWorkspaceToCreate {
    std::string name;
    std::string output;
  };
  std::vector<PersistentWorkspaceToCreate> persistentWorkspacesToCreate;

  const std::string currentMonitor = m_bar.output->name;
  std::unordered_map<std::string, int> monitorIdsByName;
  std::unordered_set<std::string> allMonitorNames;
  if (allOutputs()) {
    const auto monitors = m_ipc.getSocket1JsonReply("monitors");
    for (const auto& monitor : monitors) {
      if (monitor["name"].isString()) {
        const auto monitorName = monitor["name"].asString();
        allMonitorNames.insert(monitorName);
        if (monitor["id"].isInt()) {
          monitorIdsByName[monitorName] = monitor["id"].asInt();
        }
      }
    }
  }
  const bool monitorInConfig = std::ranges::find(keys, currentMonitor) != keys.end();
  for (const std::string& key : keys) {
    const bool keyIsMonitor =
        key == currentMonitor || (allOutputs() && allMonitorNames.contains(key));
    const bool keyIsWildcard = key == "*";
    // only add if either:
    // 1. key is the current monitor name
    // 2. key is "*" and this monitor is not already defined in the config
    // 3. all-outputs is enabled and key is an explicit monitor name
    bool canCreate = keyIsMonitor || (keyIsWildcard && !monitorInConfig);
    std::string targetMonitor = currentMonitor;
    if (keyIsMonitor) {
      targetMonitor =
          selectHyprspacesPersistentPlaceholderOutput(currentMonitor, key, allOutputs());
    }
    const Json::Value& value = m_persistentWorkspaceConfig[key];
    spdlog::trace("Parsing persistent workspace config: {} => {}", key, value.toStyledString());

    if (value.isInt()) {
      // value is a number => create that many workspaces for this monitor
      if (canCreate) {
        int amount = value.asInt();
        spdlog::debug("Creating {} persistent workspaces for monitor {}", amount, targetMonitor);
        int monitorId = static_cast<int>(m_monitorId);
        if (const auto targetMonitorId = monitorIdsByName.find(targetMonitor);
            targetMonitorId != monitorIdsByName.end()) {
          monitorId = targetMonitorId->second;
        }
        for (int i = 0; i < amount; i++) {
          const int workspaceId =
              getHyprspacesPersistentWorkspaceId(monitorId, amount, m_hyprspacesPairedOffset, i);
          persistentWorkspacesToCreate.push_back({std::to_string(workspaceId), targetMonitor});
        }
      }
    } else if (value.isArray() && !value.empty()) {
      // value is an array => create defined workspaces for this monitor
      if (canCreate) {
        for (const Json::Value& workspace : value) {
          spdlog::debug("Creating workspace {} on monitor {}", workspace, targetMonitor);
          persistentWorkspacesToCreate.push_back({workspace.asString(), targetMonitor});
        }
      } else {
        // key is the workspace and value is array of monitors to create on
        for (const Json::Value& monitor : value) {
          if (!monitor.isString()) {
            continue;
          }

          const auto monitorName = monitor.asString();
          const auto targetOutput = selectHyprspacesWorkspaceNamePersistentPlaceholderOutput(
              monitorName, currentMonitor, allOutputs());
          if (targetOutput.has_value()) {
            persistentWorkspacesToCreate.push_back({key, *targetOutput});
          }
        }
      }
    } else if (shouldCreateHyprspacesPersistentWorkspaceFallback(keyIsMonitor, keyIsWildcard)) {
      // this workspace should be displayed on all monitors
      persistentWorkspacesToCreate.push_back({key, currentMonitor});
    }
  }

  for (auto const& workspace : persistentWorkspacesToCreate) {
    auto workspaceData = createMonitorWorkspaceData(workspace.name, workspace.output);
    workspaceData["persistent-config"] = true;
    auto alias = readHyprspacesPersistentAliasMetadata(workspaceData);
    if (alias.has_value() && getHyprspacesCanonicalSlotKey(alias->id, alias->output).has_value()) {
      workspaceData[HYPRSPACES_EXPLICIT_ALIAS_PLACEHOLDER_KEY] = true;
      rememberHyprspacesPersistentAlias(std::move(*alias));
    }
    m_workspacesToCreate.emplace_back(workspaceData, clientsJson);
  }
}

void Workspaces::loadPersistentWorkspacesFromWorkspaceRules(const Json::Value& clientsJson) {
  spdlog::info("Loading persistent workspaces from Hyprland workspace rules");

  auto const workspaceRules = m_ipc.getSocket1JsonReply("workspacerules");
  for (Json::Value const& rule : workspaceRules) {
    if (!rule["workspaceString"].isString()) {
      spdlog::warn("Workspace rules: invalid workspaceString, skipping: {}", rule);
      continue;
    }
    if (!rule["persistent"].asBool()) {
      continue;
    }
    auto workspace = rule.isMember("defaultName") ? rule["defaultName"].asString()
                                                  : rule["workspaceString"].asString();

    // There could be persistent special workspaces, only show those when show-special is enabled.
    if (workspace.starts_with("special:") && !showSpecialWorkspaceEntries()) {
      continue;
    }

    // The prefix "name:" cause mismatches with workspace names taken anywhere else.
    if (workspace.starts_with("name:")) {
      workspace = workspace.substr(5);
    }
    auto const& monitor = rule["monitor"].asString();
    // create this workspace persistently if:
    // 1. the allOutputs config option is enabled
    // 2. the rule's monitor is the current monitor
    // 3. no monitor is specified in the rule => assume it needs to be persistent on every monitor
    if (allOutputs() || m_bar.output->name == monitor || monitor.empty()) {
      // => persistent workspace should be shown on this monitor
      const auto targetMonitor =
          selectHyprspacesPersistentPlaceholderOutput(m_bar.output->name, monitor, allOutputs());
      auto workspaceData = createMonitorWorkspaceData(workspace, targetMonitor);
      workspaceData["persistent-rule"] = true;
      auto alias = readHyprspacesPersistentAliasMetadata(workspaceData);
      if (alias.has_value() &&
          getHyprspacesCanonicalSlotKey(alias->id, alias->output).has_value()) {
        workspaceData[HYPRSPACES_EXPLICIT_ALIAS_PLACEHOLDER_KEY] = true;
        rememberHyprspacesPersistentAlias(std::move(*alias));
      }
      m_workspacesToCreate.emplace_back(workspaceData, clientsJson);
    } else {
      // This can be any workspace selector.
      m_workspacesToRemove.emplace_back(workspace);
    }
  }
}

void Workspaces::onEvent(const std::string& ev) {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::string eventName(begin(ev), begin(ev) + ev.find_first_of('>'));
  std::string payload = ev.substr(eventName.size() + 2);

  if (eventName == "workspacev2") {
    onWorkspaceActivated(payload);
  } else if (eventName == "activespecial") {
    onSpecialWorkspaceActivated(payload);
  } else if (eventName == "destroyworkspacev2") {
    onWorkspaceDestroyed(payload);
  } else if (eventName == "createworkspacev2") {
    onWorkspaceCreated(payload);
  } else if (eventName == "focusedmonv2" || eventName == "focusedmon") {
    onMonitorFocused(payload);
  } else if (eventName == "moveworkspacev2") {
    onWorkspaceMoved(payload);
  } else if (eventName == "openwindow") {
    onWindowOpened(payload);
  } else if (eventName == "closewindow") {
    onWindowClosed(payload);
  } else if (eventName == "movewindowv2") {
    onWindowMoved(payload);
  } else if (eventName == "urgent") {
    setUrgentWorkspace(payload);
  } else if (eventName == "renameworkspace") {
    onWorkspaceRenamed(payload);
  } else if (eventName == "windowtitlev2") {
    onWindowTitleEvent(payload);
  } else if (eventName == "activewindowv2") {
    onActiveWindowChanged(payload);
  } else if (eventName == "configreloaded") {
    onConfigReloaded();
  } else if (m_hyprspacesDynamicWorkspaces && eventName == "hyprspaces" && payload == "rebalance") {
    spdlog::info("hyprspaces topology rebalanced, reinitializing hyprland/workspaces module...");
    init();
  } else if (m_hyprspacesDynamicWorkspaces &&
             (eventName == "monitoradded" || eventName == "monitorremoved")) {
    spdlog::info("Monitor topology changed, reinitializing hyprland/workspaces module...");
    init();
  }

  dp.emit();
}

void Workspaces::onWorkspaceActivated(std::string const& payload) {
  const auto [workspaceIdStr, workspaceName] = splitDoublePayload(payload);
  const auto workspaceId = parseWorkspaceId(workspaceIdStr);
  if (workspaceId.has_value()) {
    m_activeWorkspaceId = *workspaceId;
  }
}

void Workspaces::onSpecialWorkspaceActivated(std::string const& payload) {
  std::string name(begin(payload), begin(payload) + payload.find_first_of(','));
  m_activeSpecialWorkspaceName = (!name.starts_with("special:") ? name : name.substr(8));
}

void Workspaces::onWorkspaceDestroyed(std::string const& payload) {
  const auto [workspaceId, workspaceName] = splitDoublePayload(payload);
  if (!isDoubleSpecial(workspaceName)) {
    m_workspacesToRemove.push_back(workspaceId);
  }
}

void Workspaces::onWorkspaceCreated(std::string const& payload, Json::Value const& clientsData) {
  spdlog::debug("Workspace created: {}", payload);

  const auto [workspaceIdStr, _] = splitDoublePayload(payload);

  const auto workspaceId = parseWorkspaceId(workspaceIdStr);
  if (!workspaceId.has_value()) {
    return;
  }

  auto const workspaceRules = m_ipc.getSocket1JsonReply("workspacerules");
  auto const workspacesJson = m_ipc.getSocket1JsonReply("workspaces");

  for (Json::Value workspaceJson : workspacesJson) {
    const auto currentId = workspaceJson["id"].asInt();
    if (currentId == *workspaceId) {
      std::string workspaceName = workspaceJson["name"].asString();
      // This workspace name is more up-to-date than the one in the event payload.
      if (isWorkspaceIgnored(workspaceName)) {
        spdlog::trace("Not creating workspace because it is ignored: id={} name={}", *workspaceId,
                      workspaceName);
        break;
      }

      if (shouldDisplayWorkspaceData(workspaceJson)) {
        for (Json::Value const& rule : workspaceRules) {
          auto ruleWorkspaceName = rule.isMember("defaultName")
                                       ? rule["defaultName"].asString()
                                       : rule["workspaceString"].asString();
          if (ruleWorkspaceName == workspaceName) {
            workspaceJson["persistent-rule"] = rule["persistent"].asBool();
            break;
          }
        }

        m_workspacesToCreate.emplace_back(workspaceJson, clientsData);
        break;
      }
    } else {
      extendOrphans(*workspaceId, clientsData);
    }
  }
}

void Workspaces::onWorkspaceMoved(std::string const& payload) {
  spdlog::debug("Workspace moved: {}", payload);

  // Update active workspace
  m_activeWorkspaceId = (m_ipc.getSocket1JsonReply("activeworkspace"))["id"].asInt();

  const auto [workspaceIdStr, workspaceName, monitorName] = splitTriplePayload(payload);
  const auto subPayload = makePayload(workspaceIdStr, workspaceName);

  if (allOutputs()) {
    const auto workspaceId = parseWorkspaceId(workspaceIdStr);
    auto workspace = m_workspaces.end();
    if (workspaceId.has_value()) {
      workspace = std::ranges::find_if(m_workspaces, [&](std::unique_ptr<Workspace>& x) {
        return *workspaceId == x->id() && !isHyprspacesPersistentAliasPlaceholder(*x);
      });
    } else {
      workspace = std::ranges::find_if(m_workspaces, [&](std::unique_ptr<Workspace>& x) {
        return workspaceName == x->name() && !isHyprspacesPersistentAliasPlaceholder(*x);
      });
    }

    if (workspace == m_workspaces.end()) {
      workspace = std::ranges::find_if(m_workspaces, [&](std::unique_ptr<Workspace>& x) {
        if (workspaceId.has_value()) {
          return *workspaceId == x->id();
        }
        return workspaceName == x->name();
      });
    }

    if (workspace == m_workspaces.end()) {
      return;
    }

    const auto oldCanonicalSlotKey =
        getHyprspacesCanonicalSlotKey((*workspace)->id(), (*workspace)->output());
    (*workspace)->setOutput(monitorName);
    const auto newCanonicalSlotKey =
        getHyprspacesCanonicalSlotKey((*workspace)->id(), (*workspace)->output());

    if (oldCanonicalSlotKey.has_value() && oldCanonicalSlotKey != newCanonicalSlotKey) {
      reconcileHyprspacesCanonicalSlotKey(*oldCanonicalSlotKey);
    }
    if (newCanonicalSlotKey.has_value() && oldCanonicalSlotKey != newCanonicalSlotKey) {
      reconcileHyprspacesCanonicalSlotKey(*newCanonicalSlotKey);
    }

    return;
  }

  if (m_bar.output->name == monitorName) {
    Json::Value clientsData = m_ipc.getSocket1JsonReply("clients");
    onWorkspaceCreated(subPayload, clientsData);
  } else {
    spdlog::debug("Removing workspace because it was moved to another monitor: {}", subPayload);
    onWorkspaceDestroyed(subPayload);
  }
}

void Workspaces::onWorkspaceRenamed(std::string const& payload) {
  spdlog::debug("Workspace renamed: {}", payload);
  const auto [workspaceIdStr, newName] = splitDoublePayload(payload);

  const auto workspaceId = parseWorkspaceId(workspaceIdStr);
  if (!workspaceId.has_value()) {
    return;
  }

  for (auto& workspace : m_workspaces) {
    if (workspace->id() == *workspaceId) {
      workspace->setName(newName);
      break;
    }
  }
  sortWorkspaces();
}

void Workspaces::onMonitorFocused(std::string const& payload) {
  spdlog::trace("Monitor focused: {}", payload);

  const auto [monitorName, workspaceIdStr] = splitDoublePayload(payload);

  const auto workspaceId = parseWorkspaceId(workspaceIdStr);
  if (!workspaceId.has_value()) {
    return;
  }

  m_activeWorkspaceId = *workspaceId;

  for (Json::Value& monitor : m_ipc.getSocket1JsonReply("monitors")) {
    if (monitor["name"].asString() == monitorName) {
      const auto name = monitor["specialWorkspace"]["name"].asString();
      m_activeSpecialWorkspaceName = !name.starts_with("special:") ? name : name.substr(8);
    }
  }
}

void Workspaces::onWindowOpened(std::string const& payload) {
  spdlog::trace("Window opened: {}", payload);
  updateWindowCount();
  size_t lastCommaIdx = 0;
  size_t nextCommaIdx = payload.find(',');
  std::string windowAddress = payload.substr(lastCommaIdx, nextCommaIdx - lastCommaIdx);

  lastCommaIdx = nextCommaIdx;
  nextCommaIdx = payload.find(',', nextCommaIdx + 1);
  std::string workspaceName = payload.substr(lastCommaIdx + 1, nextCommaIdx - lastCommaIdx - 1);

  lastCommaIdx = nextCommaIdx;
  nextCommaIdx = payload.find(',', nextCommaIdx + 1);
  std::string windowClass = payload.substr(lastCommaIdx + 1, nextCommaIdx - lastCommaIdx - 1);

  std::string windowTitle = payload.substr(nextCommaIdx + 1, payload.length() - nextCommaIdx);

  bool isActive = m_currentActiveWindowAddress == windowAddress;
  m_windowsToCreate.emplace_back(workspaceName, windowAddress, windowClass, windowTitle, isActive);
}

void Workspaces::onWindowClosed(std::string const& addr) {
  spdlog::trace("Window closed: {}", addr);
  updateWindowCount();
  m_orphanWindowMap.erase(addr);
  for (auto& workspace : m_workspaces) {
    if (workspace->closeWindow(addr)) {
      break;
    }
  }
}

void Workspaces::onWindowMoved(std::string const& payload) {
  spdlog::trace("Window moved: {}", payload);
  updateWindowCount();
  auto [windowAddress, _, workspaceName] = splitTriplePayload(payload);

  WindowRepr windowRepr;

  // If the window was still queued to be created, just change its destination
  // and exit
  for (auto& window : m_windowsToCreate) {
    if (window.getAddress() == windowAddress) {
      window.moveToWorkspace(workspaceName);
      return;
    }
  }

  // Take the window's representation from the old workspace...
  for (auto& workspace : m_workspaces) {
    if (auto windowAddr = workspace->closeWindow(windowAddress); windowAddr != std::nullopt) {
      windowRepr = windowAddr.value();
      break;
    }
  }

  // ...if it was empty, check if the window is an orphan...
  if (windowRepr.empty() && m_orphanWindowMap.contains(windowAddress)) {
    windowRepr = m_orphanWindowMap[windowAddress];
  }

  // ...and then add it to the new workspace
  if (!windowRepr.empty()) {
    m_orphanWindowMap.erase(windowAddress);
    m_windowsToCreate.emplace_back(workspaceName, windowAddress, windowRepr);
  }
}

void Workspaces::onWindowTitleEvent(std::string const& payload) {
  spdlog::trace("Window title changed: {}", payload);
  std::optional<std::function<void(WindowCreationPayload)>> inserter;

  const auto [windowAddress, _] = splitDoublePayload(payload);

  // If the window was an orphan, rename it at the orphan's vector
  if (m_orphanWindowMap.contains(windowAddress)) {
    inserter = [this](WindowCreationPayload wcp) { this->registerOrphanWindow(std::move(wcp)); };
  } else {
    auto windowWorkspace = std::ranges::find_if(m_workspaces, [windowAddress](auto& workspace) {
      return workspace->containsWindow(windowAddress);
    });

    // If the window exists on a workspace, rename it at the workspace's window
    // map
    if (windowWorkspace != m_workspaces.end()) {
      inserter = [windowWorkspace](WindowCreationPayload wcp) {
        (*windowWorkspace)->insertWindow(std::move(wcp));
      };
    } else {
      auto queuedWindow =
          std::ranges::find_if(m_windowsToCreate, [&windowAddress](auto& windowPayload) {
            return windowPayload.getAddress() == windowAddress;
          });

      // If the window was queued, rename it in the queue
      if (queuedWindow != m_windowsToCreate.end()) {
        inserter = [queuedWindow](WindowCreationPayload wcp) { *queuedWindow = std::move(wcp); };
      }
    }
  }

  if (inserter.has_value()) {
    Json::Value clientsData = m_ipc.getSocket1JsonReply("clients");
    std::string jsonWindowAddress = fmt::format("0x{}", windowAddress);

    auto client = std::ranges::find_if(clientsData, [jsonWindowAddress](auto& client) {
      return client["address"].asString() == jsonWindowAddress;
    });

    if (client != clientsData.end() && !client->empty()) {
      (*inserter)({*client});
    }
  }
}

void Workspaces::onActiveWindowChanged(WindowAddress const& activeWindowAddress) {
  spdlog::trace("Active window changed: {}", activeWindowAddress);
  m_currentActiveWindowAddress = activeWindowAddress;

  for (auto& [address, window] : m_orphanWindowMap) {
    window.setActive(address == activeWindowAddress);
  }
  for (auto const& workspace : m_workspaces) {
    workspace->setActiveWindow(activeWindowAddress);
  }
  for (auto& window : m_windowsToCreate) {
    window.setActive(window.getAddress() == activeWindowAddress);
  }
}

void Workspaces::onConfigReloaded() {
  spdlog::info("Hyprland config reloaded, reinitializing hyprland/workspaces module...");
  init();
}

auto Workspaces::parseConfig(const Json::Value& config) -> void {
  const auto& configFormat = config["format"];
  m_formatBefore = configFormat.isString() ? configFormat.asString() : "{name}";
  m_withIcon = m_formatBefore.find("{icon}") != std::string::npos;
  auto withWindows = m_formatBefore.find("{windows}") != std::string::npos;

  if (m_withIcon && m_iconsMap.empty()) {
    populateIconsMap(config["format-icons"]);
  }

  populateBoolConfig(config, "all-outputs", m_allOutputs);
  populateBoolConfig(config, "show-special", m_showSpecial);
  populateBoolConfig(config, "active-per-monitor", m_activePerMonitor);
  populateBoolConfig(config, "hyprspaces-special-overlay", m_hyprspacesSpecialOverlay);
  populateBoolConfig(config, "hyprspaces-dynamic-workspaces", m_hyprspacesDynamicWorkspaces);

  const auto& hyprspacesPairedOffset = config["hyprspaces-paired-offset"];
  if (hyprspacesPairedOffset.isInt()) {
    m_hyprspacesPairedOffset = std::max(0, hyprspacesPairedOffset.asInt());
  }
  const auto& hyprspacesWorkspaceCount = config["hyprspaces-workspace-count"];
  if (hyprspacesWorkspaceCount.isInt()) {
    m_hyprspacesWorkspaceCount = std::max(0, hyprspacesWorkspaceCount.asInt());
  }

  populateBoolConfig(config, "special-visible-only", m_specialVisibleOnly);
  populateBoolConfig(config, "persistent-only", m_persistentOnly);
  populateBoolConfig(config, "active-only", m_activeOnly);
  populateBoolConfig(config, "move-to-monitor", m_moveToMonitor);
  populateBoolConfig(config, "enable-bar-scroll", m_barScroll);

  m_persistentWorkspaceConfig = config.get("persistent-workspaces", Json::Value());
  populateSortByConfig(config);
  populateIgnoreWorkspacesConfig(config);
  populateFormatWindowSeparatorConfig(config);
  populateWindowRewriteConfig(config);

  if (withWindows) {
    populateWorkspaceTaskbarConfig(config);
  }
  if (m_enableTaskbar) {
    auto parts = split(m_formatBefore, "{windows}", 1);
    m_formatBefore = parts[0];
    m_formatAfter = parts.size() > 1 ? parts[1] : "";
  }
}

auto Workspaces::populateIconsMap(const Json::Value& formatIcons) -> void {
  for (const auto& name : formatIcons.getMemberNames()) {
    m_iconsMap.emplace(name, formatIcons[name].asString());
  }
  m_iconsMap.emplace("", "");
}

auto Workspaces::populateBoolConfig(const Json::Value& config, const std::string& key, bool& member)
    -> void {
  const auto& configValue = config[key];
  if (configValue.isBool()) {
    member = configValue.asBool();
  }
}

auto Workspaces::populateSortByConfig(const Json::Value& config) -> void {
  const auto& configSortBy = config["sort-by"];
  if (configSortBy.isString()) {
    auto sortByStr = configSortBy.asString();
    try {
      m_sortBy = m_enumParser.parseStringToEnum(sortByStr, m_sortMap);
    } catch (const std::invalid_argument& e) {
      m_sortBy = SortMethod::DEFAULT;
      spdlog::warn(
          "Invalid string representation for sort-by. Falling back to default sort method.");
    }
  }
}

auto Workspaces::populateIgnoreWorkspacesConfig(const Json::Value& config) -> void {
  auto ignoreWorkspaces = config["ignore-workspaces"];
  if (ignoreWorkspaces.isArray()) {
    for (const auto& workspaceRegex : ignoreWorkspaces) {
      if (workspaceRegex.isString()) {
        std::string ruleString = workspaceRegex.asString();
        try {
          const std::regex rule{ruleString, std::regex_constants::icase};
          m_ignoreWorkspaces.emplace_back(rule);
        } catch (const std::regex_error& e) {
          spdlog::error("Invalid rule {}: {}", ruleString, e.what());
        }
      } else {
        spdlog::error("Not a string: '{}'", workspaceRegex);
      }
    }
  }
}

auto Workspaces::populateFormatWindowSeparatorConfig(const Json::Value& config) -> void {
  const auto& formatWindowSeparator = config["format-window-separator"];
  m_formatWindowSeparator =
      formatWindowSeparator.isString() ? formatWindowSeparator.asString() : " ";
}

auto Workspaces::populateWindowRewriteConfig(const Json::Value& config) -> void {
  const auto& windowRewrite = config["window-rewrite"];
  if (!windowRewrite.isObject()) {
    spdlog::debug("window-rewrite is not defined or is not an object, using default rules.");
    return;
  }

  const auto& windowRewriteDefaultConfig = config["window-rewrite-default"];
  std::string windowRewriteDefault =
      windowRewriteDefaultConfig.isString() ? windowRewriteDefaultConfig.asString() : "?";

  m_windowRewriteRules = util::RegexCollection(
      windowRewrite, windowRewriteDefault,
      [this](std::string& window_rule) { return windowRewritePriorityFunction(window_rule); });
}

auto Workspaces::populateWorkspaceTaskbarConfig(const Json::Value& config) -> void {
  const auto& workspaceTaskbar = config["workspace-taskbar"];
  if (!workspaceTaskbar.isObject()) {
    spdlog::debug("workspace-taskbar is not defined or is not an object, using default rules.");
    return;
  }

  populateBoolConfig(workspaceTaskbar, "enable", m_enableTaskbar);
  populateBoolConfig(workspaceTaskbar, "update-active-window", m_updateActiveWindow);
  populateBoolConfig(workspaceTaskbar, "reverse-direction", m_taskbarReverseDirection);

  if (workspaceTaskbar["format"].isString()) {
    /* The user defined a format string, use it */
    std::string format = workspaceTaskbar["format"].asString();
    m_taskbarWithTitle = format.find("{title") != std::string::npos; /* {title} or {title.length} */
    auto parts = split(format, "{icon}", 1);
    m_taskbarFormatBefore = parts[0];
    if (parts.size() > 1) {
      m_taskbarWithIcon = true;
      m_taskbarFormatAfter = parts[1];
    }
  } else {
    /* The default is to only show the icon */
    m_taskbarWithIcon = true;
  }

  auto iconTheme = workspaceTaskbar["icon-theme"];
  if (iconTheme.isArray()) {
    for (auto& c : iconTheme) {
      m_iconLoader.add_custom_icon_theme(c.asString());
    }
  } else if (iconTheme.isString()) {
    m_iconLoader.add_custom_icon_theme(iconTheme.asString());
  }

  if (workspaceTaskbar["icon-size"].isInt()) {
    m_taskbarIconSize = workspaceTaskbar["icon-size"].asInt();
  }
  if (workspaceTaskbar["orientation"].isString() &&
      toLower(workspaceTaskbar["orientation"].asString()) == "vertical") {
    m_taskbarOrientation = Gtk::ORIENTATION_VERTICAL;
  }

  if (workspaceTaskbar["on-click-window"].isString()) {
    m_onClickWindow = workspaceTaskbar["on-click-window"].asString();
  }

  if (workspaceTaskbar["ignore-list"].isArray()) {
    for (auto& windowRegex : workspaceTaskbar["ignore-list"]) {
      std::string ruleString = windowRegex.asString();
      try {
        m_ignoreWindows.emplace_back(ruleString, std::regex_constants::icase);
      } catch (const std::regex_error& e) {
        spdlog::error("Invalid rule {}: {}", ruleString, e.what());
      }
    }
  }

  if (workspaceTaskbar["active-window-position"].isString()) {
    auto posStr = workspaceTaskbar["active-window-position"].asString();
    try {
      m_activeWindowPosition =
          m_activeWindowEnumParser.parseStringToEnum(posStr, m_activeWindowPositionMap);
    } catch (const std::invalid_argument& e) {
      spdlog::warn(
          "Invalid string representation for active-window-position. Falling back to 'none'.");
      m_activeWindowPosition = ActiveWindowPosition::NONE;
    }
  }
}

void Workspaces::registerOrphanWindow(WindowCreationPayload create_window_payload) {
  if (!create_window_payload.isEmpty(*this)) {
    m_orphanWindowMap[create_window_payload.getAddress()] = create_window_payload.repr(*this);
  }
}

auto Workspaces::registerIpc() -> void {
  m_ipc.registerForIPC("workspacev2", this);
  m_ipc.registerForIPC("activespecial", this);
  m_ipc.registerForIPC("createworkspacev2", this);
  m_ipc.registerForIPC("destroyworkspacev2", this);
  m_ipc.registerForIPC("focusedmonv2", this);
  m_ipc.registerForIPC("focusedmon", this);
  m_ipc.registerForIPC("moveworkspacev2", this);
  m_ipc.registerForIPC("renameworkspace", this);
  m_ipc.registerForIPC("openwindow", this);
  m_ipc.registerForIPC("closewindow", this);
  m_ipc.registerForIPC("movewindowv2", this);
  m_ipc.registerForIPC("urgent", this);
  m_ipc.registerForIPC("configreloaded", this);
  if (m_hyprspacesDynamicWorkspaces) {
    m_ipc.registerForIPC("hyprspaces", this);
    m_ipc.registerForIPC("monitoradded", this);
    m_ipc.registerForIPC("monitorremoved", this);
  }

  if (windowRewriteConfigUsesTitle() || m_taskbarWithTitle) {
    spdlog::info(
        "Registering for Hyprland's 'windowtitlev2' events because a user-defined window "
        "rewrite rule uses the 'title' field.");
    m_ipc.registerForIPC("windowtitlev2", this);
  }
  if (m_updateActiveWindow) {
    spdlog::info(
        "Registering for Hyprland's 'activewindowv2' events because 'update-active-window' is set "
        "to true.");
    m_ipc.registerForIPC("activewindowv2", this);
  }
}

void Workspaces::removeWorkspacesToRemove() {
  for (const auto& workspaceString : m_workspacesToRemove) {
    removeWorkspace(workspaceString);
  }
  m_workspacesToRemove.clear();
}

void Workspaces::removeWorkspace(std::string const& workspaceString) {
  spdlog::debug("Removing workspace {}", workspaceString);

  // If this succeeds, we have a workspace ID.
  const auto workspaceId = parseWorkspaceId(workspaceString);

  std::string name;
  // TODO: At some point we want to support all workspace selectors
  // This is just a subset.
  // https://wiki.hyprland.org/Configuring/Workspace-Rules/#workspace-selectors
  if (workspaceString.starts_with("special:")) {
    name = workspaceString.substr(8);
  } else if (workspaceString.starts_with("name:")) {
    name = workspaceString.substr(5);
  } else {
    name = workspaceString;
  }

  auto workspace = m_workspaces.end();
  if (workspaceId.has_value()) {
    if (m_hyprspacesPairedOffset > 0) {
      workspace = std::ranges::find_if(m_workspaces, [&](std::unique_ptr<Workspace>& x) {
        return *workspaceId == x->id() && !isHyprspacesPersistentAliasPlaceholder(*x);
      });
    }
    if (workspace == m_workspaces.end()) {
      workspace = std::ranges::find_if(
          m_workspaces, [&](std::unique_ptr<Workspace>& x) { return *workspaceId == x->id(); });
    }
  } else {
    workspace = std::ranges::find_if(
        m_workspaces, [&](std::unique_ptr<Workspace>& x) { return name == x->name(); });
  }

  if (workspace == m_workspaces.end()) {
    // happens when a workspace on another monitor is destroyed
    return;
  }

  const auto hyprspacesCanonicalSlotKey =
      getHyprspacesCanonicalSlotKey((*workspace)->id(), (*workspace)->output());

  if ((*workspace)->isPersistentConfig() && !hyprspacesCanonicalSlotKey.has_value()) {
    spdlog::trace("Not removing config persistent workspace id={} name={}", (*workspace)->id(),
                  (*workspace)->name());
    return;
  }

  m_box.remove(workspace->get()->button());
  m_workspaces.erase(workspace);

  if (hyprspacesCanonicalSlotKey.has_value()) {
    reconcileHyprspacesCanonicalSlotKey(*hyprspacesCanonicalSlotKey);
  }
}

void Workspaces::setCurrentMonitorId() {
  // get monitor ID from name (used by persistent workspaces)
  m_monitorId = 0;
  auto monitors = m_ipc.getSocket1JsonReply("monitors");
  auto currentMonitor = std::ranges::find_if(monitors, [this](const Json::Value& m) {
    return m["name"].asString() == m_bar.output->name;
  });
  if (currentMonitor == monitors.end()) {
    spdlog::error("Monitor '{}' does not have an ID? Using 0", m_bar.output->name);
  } else {
    m_monitorId = (*currentMonitor)["id"].asInt();
    spdlog::trace("Current monitor ID: {}", m_monitorId);
  }
}

void Workspaces::sortSpecialCentered() {
  std::vector<std::unique_ptr<Workspace>> specialWorkspaces;
  std::vector<std::unique_ptr<Workspace>> hiddenWorkspaces;
  std::vector<std::unique_ptr<Workspace>> normalWorkspaces;

  for (auto& workspace : m_workspaces) {
    if (workspace->isSpecial()) {
      specialWorkspaces.push_back(std::move(workspace));
    } else {
      if (workspace->button().is_visible()) {
        normalWorkspaces.push_back(std::move(workspace));
      } else {
        hiddenWorkspaces.push_back(std::move(workspace));
      }
    }
  }
  m_workspaces.clear();

  size_t center = normalWorkspaces.size() / 2;

  m_workspaces.insert(m_workspaces.end(), std::make_move_iterator(normalWorkspaces.begin()),
                      std::make_move_iterator(normalWorkspaces.begin() + center));

  m_workspaces.insert(m_workspaces.end(), std::make_move_iterator(specialWorkspaces.begin()),
                      std::make_move_iterator(specialWorkspaces.end()));

  m_workspaces.insert(m_workspaces.end(),
                      std::make_move_iterator(normalWorkspaces.begin() + center),
                      std::make_move_iterator(normalWorkspaces.end()));

  m_workspaces.insert(m_workspaces.end(), std::make_move_iterator(hiddenWorkspaces.begin()),
                      std::make_move_iterator(hiddenWorkspaces.end()));
}

void Workspaces::sortWorkspaces() {
  std::ranges::sort(  //
      m_workspaces, [&](std::unique_ptr<Workspace>& a, std::unique_ptr<Workspace>& b) {
        // Helper comparisons
        auto isIdLess = a->id() < b->id();
        auto isNameLess = a->name() < b->name();

        switch (m_sortBy) {
          case SortMethod::ID:
            return isIdLess;
          case SortMethod::NAME:
            return isNameLess;
          case SortMethod::NUMBER:
            try {
              return std::stoi(a->name()) < std::stoi(b->name());
            } catch (const std::exception& e) {
              // Handle the exception if necessary.
              break;
            }
          case SortMethod::DEFAULT:
          default:
            // Handle the default case here.
            // normal -> named persistent -> named -> special -> named special

            // both normal (includes numbered persistent) => sort by ID
            if (a->id() > 0 && b->id() > 0) {
              if (const auto displaySlotOrder = compareHyprspacesSameOutputDisplaySlot(
                      a->id(), a->output(), b->id(), b->output(), m_hyprspacesPairedOffset);
                  displaySlotOrder.has_value()) {
                return *displaySlotOrder;
              }
              return isIdLess;
            }

            // one normal, one special => normal first
            if ((a->isSpecial()) ^ (b->isSpecial())) {
              return b->isSpecial();
            }

            // only one normal, one named
            if ((a->id() > 0) ^ (b->id() > 0)) {
              return a->id() > 0;
            }

            // both special
            if (a->isSpecial() && b->isSpecial()) {
              // if one is -99 => put it last
              if (a->id() == -99 || b->id() == -99) {
                return b->id() == -99;
              }
              // both are 0 (not yet named persistents) / named specials
              // (-98 <= ID <= -1)
              return isNameLess;
            }

            // sort non-special named workspaces by name (ID <= -1377)
            return isNameLess;
            break;
        }

        // Return a default value if none of the cases match.
        return isNameLess;  // You can adjust this to your specific needs.
      });
  if (m_sortBy == SortMethod::SPECIAL_CENTERED) {
    this->sortSpecialCentered();
  }

  for (size_t i = 0; i < m_workspaces.size(); ++i) {
    m_box.reorder_child(m_workspaces[i]->button(), i);
  }
}

void Workspaces::setUrgentWorkspace(std::string const& windowaddress) {
  const Json::Value clientsJson = m_ipc.getSocket1JsonReply("clients");
  auto client = std::ranges::find_if(clientsJson, [&](Json::Value const& clientJson) {
    return clientJson["address"].asString().ends_with(windowaddress);
  });

  if (client == clientsJson.end()) {
    return;
  }

  auto workspace = std::ranges::find_if(m_workspaces, [&](std::unique_ptr<Workspace>& x) {
    return isWindowInWorkspace(*x, *client);
  });
  if (workspace != m_workspaces.end()) {
    workspace->get()->setUrgent();
  }
}

auto Workspaces::update() -> void {
  doUpdate();
  AModule::update();
}

void Workspaces::updateWindowCount() {
  const Json::Value workspacesJson = m_ipc.getSocket1JsonReply("workspaces");
  for (auto const& workspace : m_workspaces) {
    auto workspaceJson = std::ranges::find_if(
        workspacesJson, [&](Json::Value const& x) { return isWorkspaceJsonMatch(*workspace, x); });
    uint32_t count = 0;
    if (workspaceJson != workspacesJson.end()) {
      try {
        count = (*workspaceJson)["windows"].asUInt();
      } catch (const std::exception& e) {
        spdlog::error("Failed to update window count: {}", e.what());
      }
    }
    workspace->setWindows(count);
  }
}

bool Workspaces::updateWindowsToCreate() {
  bool anyWindowCreated = false;
  std::vector<WindowCreationPayload> notCreated;
  for (auto& windowPayload : m_windowsToCreate) {
    bool created = false;
    for (auto& workspace : m_workspaces) {
      if (workspace->onWindowOpened(windowPayload)) {
        created = true;
        anyWindowCreated = true;
        break;
      }
    }
    if (!created) {
      static auto const WINDOW_CREATION_TIMEOUT = 2;
      if (windowPayload.incrementTimeSpentUncreated() < WINDOW_CREATION_TIMEOUT) {
        notCreated.push_back(windowPayload);
      } else {
        registerOrphanWindow(windowPayload);
      }
    }
  }
  m_windowsToCreate.clear();
  m_windowsToCreate = notCreated;
  return anyWindowCreated;
}

Workspaces::WorkspaceStateSources Workspaces::fetchWorkspaceStateSources() {
  return {getVisibleWorkspaces(), m_ipc.getSocket1JsonReply("workspaces"),
          m_ipc.getSocket1JsonReply("activeworkspace"), m_ipc.getSocket1JsonReply("monitors")};
}

Workspaces::WorkspaceStateContext Workspaces::indexWorkspaceState(
    WorkspaceStateSources const& sources) const {
  WorkspaceStateContext context;
  context.visibleWorkspaceIds = sources.visibleWorkspaceIds;
  context.updatedWorkspaces = sources.workspaces;
  context.activeWorkspaceId = m_activeWorkspaceId;
  context.activeWorkspaceName =
      sources.activeWorkspace.isMember("name") ? sources.activeWorkspace["name"].asString() : "";
  context.activeSpecialWorkspaceName = m_activeSpecialWorkspaceName;
  context.hyprspacesPairingEnabled = m_hyprspacesPairedOffset > 0;
  context.hyprspacesIndex.visibleRawIds.insert(context.visibleWorkspaceIds.begin(),
                                               context.visibleWorkspaceIds.end());

  if (m_activePerMonitor) {
    auto monitor = std::ranges::find_if(sources.monitors, [this](const Json::Value& m) {
      return m["name"].asString() == m_bar.output->name;
    });

    if (monitor != sources.monitors.end()) {
      const auto monitorActiveWorkspace = (*monitor)["activeWorkspace"];
      if (monitorActiveWorkspace.isObject() && monitorActiveWorkspace["id"].isInt()) {
        context.activeWorkspaceId = monitorActiveWorkspace["id"].asInt();
      }
      if (monitorActiveWorkspace.isObject() && monitorActiveWorkspace["name"].isString()) {
        context.activeWorkspaceName = monitorActiveWorkspace["name"].asString();
      }
      context.activeWorkspaceOutput = m_bar.output->name;

      const auto monitorSpecialWorkspace = (*monitor)["specialWorkspace"];
      if (monitorSpecialWorkspace.isObject() && monitorSpecialWorkspace["name"].isString()) {
        auto specialName = monitorSpecialWorkspace["name"].asString();
        context.activeSpecialWorkspaceName =
            specialName.starts_with("special:") ? specialName.substr(8) : specialName;
      }
    }
  }

  context.monitorHasSpecialWorkspace = !context.activeSpecialWorkspaceName.empty();

  if (!context.hyprspacesPairingEnabled) {
    return context;
  }

  if (context.activeWorkspaceOutput.empty()) {
    auto activeWorkspace = std::ranges::find_if(sources.workspaces, [&](const auto& workspace) {
      if (!workspace["id"].isInt()) {
        return false;
      }
      const bool idMatches = workspace["id"].asInt() == context.activeWorkspaceId;
      const bool nameMatches = !context.activeWorkspaceName.empty() &&
                               workspace["name"].asString() == context.activeWorkspaceName;
      return idMatches || nameMatches;
    });
    if (activeWorkspace != sources.workspaces.end()) {
      context.activeWorkspaceOutput = (*activeWorkspace)["monitor"].asString();
    }
  }

  std::vector<HyprspacesQueuedWorkspace> snapshotWorkspaces;
  for (const auto& workspace : sources.workspaces) {
    if (!workspace["id"].isInt() || !workspace["windows"].isInt()) {
      continue;
    }

    snapshotWorkspaces.push_back({workspace["id"].asInt(), workspace["name"].asString(),
                                  workspace["monitor"].asString(), workspace["windows"].asInt()});
  }

  std::vector<HyprspacesVisibleWorkspace> visibleCanonicalWorkspaces;
  for (const auto& monitor : sources.monitors) {
    const auto monitorName = monitor["name"].asString();
    const auto activeWorkspace = monitor["activeWorkspace"];
    if (activeWorkspace.isObject() && activeWorkspace["id"].isInt()) {
      visibleCanonicalWorkspaces.push_back({activeWorkspace["id"].asInt(), monitorName});
    }
  }

  context.hyprspacesIndex =
      buildHyprspacesStateIndex(snapshotWorkspaces, visibleCanonicalWorkspaces,
                                context.visibleWorkspaceIds, m_hyprspacesPairedOffset);
  return context;
}

void Workspaces::reconcileWorkspaceStateOutputs(Json::Value const& updatedWorkspaces) {
  if (m_hyprspacesPairedOffset <= 0) {
    return;
  }

  std::unordered_set<std::string> hyprspacesCanonicalSlotKeysToReconcile;
  for (auto& workspace : m_workspaces) {
    if (isHyprspacesPersistentAliasPlaceholder(*workspace)) {
      continue;
    }

    auto updatedWorkspace = std::ranges::find_if(updatedWorkspaces, [&](const auto& w) {
      return isWorkspaceJsonSameRealWorkspace(*workspace, w);
    });
    if (updatedWorkspace == updatedWorkspaces.end()) {
      continue;
    }

    const auto oldCanonicalSlotKey =
        getHyprspacesCanonicalSlotKey(workspace->id(), workspace->output());
    workspace->setOutput((*updatedWorkspace)["monitor"].asString());
    const auto newCanonicalSlotKey =
        getHyprspacesCanonicalSlotKey(workspace->id(), workspace->output());
    if (oldCanonicalSlotKey == newCanonicalSlotKey) {
      continue;
    }
    if (oldCanonicalSlotKey.has_value()) {
      hyprspacesCanonicalSlotKeysToReconcile.insert(*oldCanonicalSlotKey);
    }
    if (newCanonicalSlotKey.has_value()) {
      hyprspacesCanonicalSlotKeysToReconcile.insert(*newCanonicalSlotKey);
    }
  }

  for (const auto& key : hyprspacesCanonicalSlotKeysToReconcile) {
    reconcileHyprspacesCanonicalSlotKey(key);
  }
}

HyprspacesWorkspaceRenderState Workspaces::classifyWorkspaceState(
    Workspace const& workspace, WorkspaceStateContext const& context) const {
  return classifyHyprspacesWorkspaceState(
      {workspace.id(), workspace.name(), workspace.output(), workspace.isSpecial(),
       isHyprspacesPersistentAliasPlaceholder(workspace),
       shouldUseHyprspacesCanonicalSlotForState(workspace)},
      {context.activeWorkspaceId, context.activeWorkspaceName, context.activeWorkspaceOutput,
       context.activeSpecialWorkspaceName, context.monitorHasSpecialWorkspace,
       m_hyprspacesSpecialOverlay},
      context.hyprspacesIndex, m_hyprspacesPairedOffset);
}

void Workspaces::renderWorkspaceState(Workspace& workspace,
                                      HyprspacesWorkspaceRenderState const& state) {
  workspace.setActive(state.active);
  workspace.setSpecialActive(state.specialActive);

  if (workspace.isActive() && workspace.isUrgent()) {
    workspace.setUrgent(false);
  }
  workspace.setVisible(state.visible);

  std::string& workspaceIcon = m_iconsMap[""];
  if (m_withIcon) {
    workspaceIcon = workspace.selectIcon(m_iconsMap);
  }

  workspace.setDisplayIdOverride(state.displaySlot);
  workspace.setDisplayNameOverride(state.displayLabel);
  workspace.setEmptyOverride(state.empty);
  workspace.update(workspaceIcon);
}

void Workspaces::updateWorkspaceStates() {
  const auto sources = fetchWorkspaceStateSources();
  const auto context = indexWorkspaceState(sources);
  reconcileWorkspaceStateOutputs(context.updatedWorkspaces);

  for (auto& workspace : m_workspaces) {
    renderWorkspaceState(*workspace, classifyWorkspaceState(*workspace, context));
  }
}

int Workspaces::windowRewritePriorityFunction(std::string const& window_rule) {
  // Rules that match against title are prioritized
  // Rules that don't specify if they're matching against either title or class are deprioritized
  bool const hasTitle = window_rule.find("title") != std::string::npos;
  bool const hasClass = window_rule.find("class") != std::string::npos;

  if (hasTitle && hasClass) {
    m_anyWindowRewriteRuleUsesTitle = true;
    return 3;
  }
  if (hasTitle) {
    m_anyWindowRewriteRuleUsesTitle = true;
    return 2;
  }
  if (hasClass) {
    return 1;
  }
  return 0;
}

template <typename... Args>
std::string Workspaces::makePayload(Args const&... args) {
  std::ostringstream result;
  bool first = true;
  ((result << (first ? "" : ",") << args, first = false), ...);
  return result.str();
}

std::pair<std::string, std::string> Workspaces::splitDoublePayload(std::string const& payload) {
  const std::string part1 = payload.substr(0, payload.find(','));
  const std::string part2 = payload.substr(part1.size() + 1);
  return {part1, part2};
}

std::tuple<std::string, std::string, std::string> Workspaces::splitTriplePayload(
    std::string const& payload) {
  const size_t firstComma = payload.find(',');
  const size_t secondComma = payload.find(',', firstComma + 1);

  const std::string part1 = payload.substr(0, firstComma);
  const std::string part2 = payload.substr(firstComma + 1, secondComma - (firstComma + 1));
  const std::string part3 = payload.substr(secondComma + 1);

  return {part1, part2, part3};
}

std::optional<int> Workspaces::parseWorkspaceId(std::string const& workspaceIdStr) {
  try {
    return workspaceIdStr == "special" ? -99 : std::stoi(workspaceIdStr);
  } catch (std::exception const& e) {
    spdlog::debug("Workspace \"{}\" is not bound to an id: {}", workspaceIdStr, e.what());
    return std::nullopt;
  }
}

bool Workspaces::handleScroll(GdkEventScroll* e) {
  // Ignore emulated scroll events on window
  if (gdk_event_get_pointer_emulated((GdkEvent*)e)) {
    return false;
  }
  auto dir = AModule::getScrollDir(e);
  if (dir == SCROLL_DIR::NONE) {
    return true;
  }

  if (dir == SCROLL_DIR::DOWN || dir == SCROLL_DIR::RIGHT) {
    if (allOutputs()) {
      m_ipc.getSocket1Reply("dispatch workspace e+1");
    } else {
      m_ipc.getSocket1Reply("dispatch workspace m+1");
    }
  } else if (dir == SCROLL_DIR::UP || dir == SCROLL_DIR::LEFT) {
    if (allOutputs()) {
      m_ipc.getSocket1Reply("dispatch workspace e-1");
    } else {
      m_ipc.getSocket1Reply("dispatch workspace m-1");
    }
  }

  return true;
}

}  // namespace waybar::modules::hyprland
