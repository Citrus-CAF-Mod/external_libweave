// Copyright 2015 The Weave Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/component_manager_impl.h"

#include <base/strings/string_number_conversions.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>

#include "src/commands/schema_constants.h"
#include "src/json_error_codes.h"
#include "src/string_utils.h"
#include "src/utils.h"

namespace weave {

namespace {
// Max of 100 state update events should be enough in the queue.
const size_t kMaxStateChangeQueueSize = 100;

const char kMinimalRole[] = "minimalRole";

const EnumToStringMap<UserRole>::Map kMap[] = {
    {UserRole::kViewer, "viewer"},
    {UserRole::kUser, "user"},
    {UserRole::kOwner, "owner"},
    {UserRole::kManager, "manager"},
};

void RemoveInaccessibleState(const ComponentManagerImpl* manager,
                             base::DictionaryValue* component,
                             UserRole role) {
  std::vector<std::string> state_props_to_remove;
  base::DictionaryValue* state = nullptr;
  if (component->GetDictionary("state", &state)) {
    for (base::DictionaryValue::Iterator it_trait(*state); !it_trait.IsAtEnd();
         it_trait.Advance()) {
      const base::DictionaryValue* trait = nullptr;
      CHECK(it_trait.value().GetAsDictionary(&trait));
      for (base::DictionaryValue::Iterator it_prop(*trait); !it_prop.IsAtEnd();
           it_prop.Advance()) {
        std::string prop_name = base::StringPrintf(
            "%s.%s", it_trait.key().c_str(), it_prop.key().c_str());
        UserRole minimal_role;
        if (manager->GetStateMinimalRole(prop_name, &minimal_role, nullptr) &&
            minimal_role > role) {
          state_props_to_remove.push_back(prop_name);
        }
      }
    }
  }
  // Now remove any inaccessible properties from the state collection.
  for (const std::string& path : state_props_to_remove) {
    // Remove starting from component level in order for "state" to be removed
    // if no sub-properties remain.
    CHECK(component->RemovePath(base::StringPrintf("state.%s", path.c_str()),
                                nullptr));
  }

  // If this component has any sub-components, filter them too.
  base::DictionaryValue* sub_components = nullptr;
  if (component->GetDictionary("components", &sub_components)) {
    for (base::DictionaryValue::Iterator it_component(*sub_components);
         !it_component.IsAtEnd(); it_component.Advance()) {
      base::Value* sub_component = nullptr;
      CHECK(sub_components->Get(it_component.key(), &sub_component));
      if (sub_component->GetType() == base::Value::TYPE_LIST) {
        base::ListValue* component_array = nullptr;
        CHECK(sub_component->GetAsList(&component_array));
        for (const auto& item : *component_array) {
          CHECK(item->GetAsDictionary(&component));
          RemoveInaccessibleState(manager, component, role);
        }
      } else if (sub_component->GetType() == base::Value::TYPE_DICTIONARY) {
        CHECK(sub_component->GetAsDictionary(&component));
        RemoveInaccessibleState(manager, component, role);
      }
    }
  }
}

}  // anonymous namespace

template <>
LIBWEAVE_EXPORT EnumToStringMap<UserRole>::EnumToStringMap()
    : EnumToStringMap(kMap) {}

ComponentManagerImpl::ComponentManagerImpl(provider::TaskRunner* task_runner,
                                           base::Clock* clock)
    : clock_{clock ? clock : &default_clock_},
      command_queue_{task_runner, clock_} {}

ComponentManagerImpl::~ComponentManagerImpl() {}

bool ComponentManagerImpl::AddComponent(const std::string& path,
                                        const std::string& name,
                                        const std::vector<std::string>& traits,
                                        ErrorPtr* error) {
  base::DictionaryValue* root = &components_;
  if (!path.empty()) {
    root = FindComponentGraftNode(path, error);
    if (!root)
      return false;
  }
  if (root->GetWithoutPathExpansion(name, nullptr)) {
    return Error::AddToPrintf(error, FROM_HERE, errors::commands::kInvalidState,
                              "Component '%s' already exists at path '%s'",
                              name.c_str(), path.c_str());
  }

  // Check to make sure the declared traits are already defined.
  for (const std::string& trait : traits) {
    if (!FindTraitDefinition(trait)) {
      return Error::AddToPrintf(error, FROM_HERE,
                                errors::commands::kInvalidPropValue,
                                "Trait '%s' is undefined", trait.c_str());
    }
  }
  std::unique_ptr<base::DictionaryValue> dict{new base::DictionaryValue};
  std::unique_ptr<base::ListValue> traits_list{new base::ListValue};
  traits_list->AppendStrings(traits);
  dict->Set("traits", std::move(traits_list));
  root->SetWithoutPathExpansion(name, std::move(dict));
  for (const auto& cb : on_componet_tree_changed_)
    cb.Run();
  return true;
}

bool ComponentManagerImpl::AddComponentArrayItem(
    const std::string& path,
    const std::string& name,
    const std::vector<std::string>& traits,
    ErrorPtr* error) {
  base::DictionaryValue* root = &components_;
  if (!path.empty()) {
    root = FindComponentGraftNode(path, error);
    if (!root)
      return false;
  }
  base::ListValue* array_value = nullptr;
  if (!root->GetListWithoutPathExpansion(name, &array_value)) {
    array_value = new base::ListValue;
    root->SetWithoutPathExpansion(name, array_value);
  }
  std::unique_ptr<base::DictionaryValue> dict{new base::DictionaryValue};
  std::unique_ptr<base::ListValue> traits_list{new base::ListValue};
  traits_list->AppendStrings(traits);
  dict->Set("traits", std::move(traits_list));
  array_value->Append(std::move(dict));
  for (const auto& cb : on_componet_tree_changed_)
    cb.Run();
  return true;
}

bool ComponentManagerImpl::RemoveComponent(const std::string& path,
                                           const std::string& name,
                                           ErrorPtr* error) {
  base::DictionaryValue* root = &components_;
  if (!path.empty()) {
    root = FindComponentGraftNode(path, error);
    if (!root)
      return false;
  }

  if (!root->RemoveWithoutPathExpansion(name, nullptr)) {
    return Error::AddToPrintf(error, FROM_HERE, errors::commands::kInvalidState,
                              "Component '%s' does not exist at path '%s'",
                              name.c_str(), path.c_str());
  }

  for (const auto& cb : on_componet_tree_changed_)
    cb.Run();
  return true;
}

bool ComponentManagerImpl::RemoveComponentArrayItem(const std::string& path,
                                                    const std::string& name,
                                                    size_t index,
                                                    ErrorPtr* error) {
  base::DictionaryValue* root = &components_;
  if (!path.empty()) {
    root = FindComponentGraftNode(path, error);
    if (!root)
      return false;
  }

  base::ListValue* array_value = nullptr;
  if (!root->GetListWithoutPathExpansion(name, &array_value)) {
    return Error::AddToPrintf(
        error, FROM_HERE, errors::commands::kInvalidState,
        "There is no component array named '%s' at path '%s'", name.c_str(),
        path.c_str());
  }

  if (!array_value->Remove(index, nullptr)) {
    return Error::AddToPrintf(
        error, FROM_HERE, errors::commands::kInvalidState,
        "Component array '%s' at path '%s' does not have an element %zu",
        name.c_str(), path.c_str(), index);
  }

  for (const auto& cb : on_componet_tree_changed_)
    cb.Run();
  return true;
}

void ComponentManagerImpl::AddComponentTreeChangedCallback(
    const base::Closure& callback) {
  on_componet_tree_changed_.push_back(callback);
  callback.Run();
}

bool ComponentManagerImpl::LoadTraits(const base::DictionaryValue& dict,
                                      ErrorPtr* error) {
  bool modified = false;
  bool result = true;
  // Check if any of the new traits are already defined. If so, make sure the
  // definition is exactly the same, or else this is an error.
  for (base::DictionaryValue::Iterator it(dict); !it.IsAtEnd(); it.Advance()) {
    if (it.value().GetType() != base::Value::TYPE_DICTIONARY) {
      Error::AddToPrintf(error, FROM_HERE, errors::commands::kTypeMismatch,
                         "Trait '%s' must be an object", it.key().c_str());
      result = false;
      break;
    }
    const base::DictionaryValue* existing_def = nullptr;
    if (traits_.GetDictionary(it.key(), &existing_def)) {
      if (!existing_def->Equals(&it.value())) {
        Error::AddToPrintf(error, FROM_HERE, errors::commands::kTypeMismatch,
                           "Trait '%s' cannot be redefined", it.key().c_str());
        result = false;
        break;
      }
    } else {
      traits_.Set(it.key(), it.value().CreateDeepCopy());
      modified = true;
    }
  }

  if (modified) {
    for (const auto& cb : on_trait_changed_)
      cb.Run();
  }
  return result;
}

bool ComponentManagerImpl::LoadTraits(const std::string& json,
                                      ErrorPtr* error) {
  std::unique_ptr<const base::DictionaryValue> dict = LoadJsonDict(json, error);
  if (!dict)
    return false;
  return LoadTraits(*dict, error);
}

void ComponentManagerImpl::AddTraitDefChangedCallback(
    const base::Closure& callback) {
  on_trait_changed_.push_back(callback);
  callback.Run();
}

void ComponentManagerImpl::AddCommand(
    std::unique_ptr<CommandInstance> command_instance) {
  command_queue_.Add(std::move(command_instance));
}

std::unique_ptr<CommandInstance> ComponentManagerImpl::ParseCommandInstance(
    const base::DictionaryValue& command,
    Command::Origin command_origin,
    UserRole role,
    std::string* id,
    ErrorPtr* error) {
  std::string command_id;
  auto command_instance =
      CommandInstance::FromJson(&command, command_origin, &command_id, error);
  // If we fail to validate the command definition, but there was a command ID
  // specified there, return it to the caller when requested. This will be
  // used to abort cloud commands.
  if (id)
    *id = command_id;

  if (!command_instance)
    return nullptr;

  UserRole minimal_role;
  if (!GetCommandMinimalRole(command_instance->GetName(), &minimal_role, error))
    return nullptr;

  if (role < minimal_role) {
    return Error::AddToPrintf(error, FROM_HERE, "access_denied",
                              "User role '%s' less than minimal: '%s'",
                              EnumToString(role).c_str(),
                              EnumToString(minimal_role).c_str());
  }

  std::string component_path = command_instance->GetComponent();
  if (component_path.empty()) {
    // Find the component to which to route this command. Get the trait name
    // from the command name and find the first component that has this trait.
    auto trait_name =
        SplitAtFirst(command_instance->GetName(), ".", true).first;
    component_path = FindComponentWithTrait(trait_name);
    if (component_path.empty()) {
      return Error::AddToPrintf(
          error, FROM_HERE, "unrouted_command",
          "Unable route command '%s' because there is no component supporting"
          "trait '%s'",
          command_instance->GetName().c_str(), trait_name.c_str());
    }
    command_instance->SetComponent(component_path);
  }

  const base::DictionaryValue* component = FindComponent(component_path, error);
  if (!component)
    return nullptr;

  // Check that the command's trait is supported by the given component.
  auto pair = SplitAtFirst(command_instance->GetName(), ".", true);

  bool trait_supported = false;
  const base::ListValue* supported_traits = nullptr;
  if (component->GetList("traits", &supported_traits)) {
    for (const auto& value : *supported_traits) {
      std::string trait;
      CHECK(value->GetAsString(&trait));
      if (trait == pair.first) {
        trait_supported = true;
        break;
      }
    }
  }

  if (!trait_supported) {
    return Error::AddToPrintf(error, FROM_HERE, "trait_not_supported",
                              "Component '%s' doesn't support trait '%s'",
                              component_path.c_str(), pair.first.c_str());
  }

  if (command_id.empty()) {
    command_id = std::to_string(++next_command_id_);
    command_instance->SetID(command_id);
    if (id)
      *id = command_id;
  }

  return command_instance;
}

CommandInstance* ComponentManagerImpl::FindCommand(const std::string& id) {
  return command_queue_.Find(id);
}

void ComponentManagerImpl::AddCommandAddedCallback(
    const CommandQueue::CommandCallback& callback) {
  command_queue_.AddCommandAddedCallback(callback);
}

void ComponentManagerImpl::AddCommandRemovedCallback(
    const CommandQueue::CommandCallback& callback) {
  command_queue_.AddCommandRemovedCallback(callback);
}

void ComponentManagerImpl::AddCommandHandler(
    const std::string& component_path,
    const std::string& command_name,
    const Device::CommandHandlerCallback& callback) {
  // If both component_path and command_name are empty, we are adding the
  // default handler for all commands.
  if (!component_path.empty() || !command_name.empty()) {
    CHECK(FindCommandDefinition(command_name)) << "Command undefined: "
                                               << command_name;
  }
  command_queue_.AddCommandHandler(component_path, command_name, callback);
}

const base::DictionaryValue* ComponentManagerImpl::FindComponent(
    const std::string& path,
    ErrorPtr* error) const {
  return FindComponentAt(&components_, path, error);
}

const base::DictionaryValue* ComponentManagerImpl::FindTraitDefinition(
    const std::string& name) const {
  const base::DictionaryValue* trait = nullptr;
  traits_.GetDictionaryWithoutPathExpansion(name, &trait);
  return trait;
}

const base::DictionaryValue* ComponentManagerImpl::FindCommandDefinition(
    const std::string& command_name) const {
  const base::DictionaryValue* definition = nullptr;
  std::vector<std::string> components = Split(command_name, ".", true, false);
  // Make sure the |command_name| came in form of trait_name.command_name.
  if (components.size() != 2)
    return definition;
  std::string key = base::StringPrintf("%s.commands.%s", components[0].c_str(),
                                       components[1].c_str());
  traits_.GetDictionary(key, &definition);
  return definition;
}

const base::DictionaryValue* ComponentManagerImpl::FindStateDefinition(
    const std::string& state_property_name) const {
  const base::DictionaryValue* definition = nullptr;
  std::vector<std::string> components =
      Split(state_property_name, ".", true, false);
  // Make sure the |state_property_name| came in form of trait_name.state_name.
  if (components.size() != 2)
    return definition;
  std::string key = base::StringPrintf("%s.state.%s", components[0].c_str(),
                                       components[1].c_str());
  traits_.GetDictionary(key, &definition);
  return definition;
}

bool ComponentManagerImpl::GetCommandMinimalRole(
    const std::string& command_name,
    UserRole* minimal_role,
    ErrorPtr* error) const {
  const base::DictionaryValue* command = FindCommandDefinition(command_name);
  if (!command) {
    return Error::AddToPrintf(
        error, FROM_HERE, errors::commands::kInvalidCommandName,
        "Command definition for '%s' not found", command_name.c_str());
  }
  std::string value;
  // The JSON definition has been pre-validated already in LoadCommands, so
  // just using CHECKs here.
  CHECK(command->GetString(kMinimalRole, &value));
  CHECK(StringToEnum(value, minimal_role));
  return true;
}

bool ComponentManagerImpl::GetStateMinimalRole(
    const std::string& state_property_name,
    UserRole* minimal_role,
    ErrorPtr* error) const {
  const base::DictionaryValue* state = FindStateDefinition(state_property_name);
  if (!state) {
    return Error::AddToPrintf(error, FROM_HERE, errors::commands::kInvalidState,
                              "State definition for '%s' not found",
                              state_property_name.c_str());
  }
  std::string value;
  if (state->GetString(kMinimalRole, &value)) {
    CHECK(StringToEnum(value, minimal_role));
  } else {
    *minimal_role = UserRole::kUser;
  }
  return true;
}

void ComponentManagerImpl::AddStateChangedCallback(
    const base::Closure& callback) {
  on_state_changed_.push_back(callback);
  callback.Run();  // Force to read current state.
}

std::unique_ptr<base::DictionaryValue>
ComponentManagerImpl::GetComponentsForUserRole(UserRole role) const {
  auto components = components_.CreateDeepCopy();
  // Build a list of all state properties that are inaccessible to the given
  // user. These properties will be removed from the components collection
  // returned from this method.
  for (base::DictionaryValue::Iterator it_component(components_);
       !it_component.IsAtEnd(); it_component.Advance()) {
    base::DictionaryValue* component = nullptr;
    CHECK(components->GetDictionary(it_component.key(), &component));
    RemoveInaccessibleState(this, component, role);
  }

  return components;
}

bool ComponentManagerImpl::SetStateProperties(const std::string& component_path,
                                              const base::DictionaryValue& dict,
                                              ErrorPtr* error) {
  base::DictionaryValue* component =
      FindMutableComponent(component_path, error);
  if (!component)
    return false;

  base::DictionaryValue* state = nullptr;
  if (!component->GetDictionary("state", &state)) {
    state = new base::DictionaryValue;
    component->Set("state", state);
  }
  state->MergeDictionary(&dict);
  last_state_change_id_++;
  auto& queue = state_change_queues_[component_path];
  if (!queue)
    queue.reset(new StateChangeQueue{kMaxStateChangeQueueSize});
  base::Time timestamp = clock_->Now();
  queue->NotifyPropertiesUpdated(timestamp, dict);
  for (const auto& cb : on_state_changed_)
    cb.Run();
  return true;
}

bool ComponentManagerImpl::SetStatePropertiesFromJson(
    const std::string& component_path,
    const std::string& json,
    ErrorPtr* error) {
  std::unique_ptr<const base::DictionaryValue> dict = LoadJsonDict(json, error);
  return dict && SetStateProperties(component_path, *dict, error);
}

const base::Value* ComponentManagerImpl::GetStateProperty(
    const std::string& component_path,
    const std::string& name,
    ErrorPtr* error) const {
  const base::DictionaryValue* component = FindComponent(component_path, error);
  if (!component)
    return nullptr;
  auto pair = SplitAtFirst(name, ".", true);
  if (pair.first.empty()) {
    return Error::AddToPrintf(error, FROM_HERE,
                              errors::commands::kPropertyMissing,
                              "Empty state package in '%s'", name.c_str());
  }
  if (pair.second.empty()) {
    return Error::AddToPrintf(
        error, FROM_HERE, errors::commands::kPropertyMissing,
        "State property name not specified in '%s'", name.c_str());
  }
  std::string key = base::StringPrintf("state.%s", name.c_str());
  const base::Value* value = nullptr;
  if (!component->Get(key, &value)) {
    return Error::AddToPrintf(error, FROM_HERE,
                              errors::commands::kPropertyMissing,
                              "State property '%s' not found in component '%s'",
                              name.c_str(), component_path.c_str());
  }
  return value;
}

bool ComponentManagerImpl::SetStateProperty(const std::string& component_path,
                                            const std::string& name,
                                            const base::Value& value,
                                            ErrorPtr* error) {
  base::DictionaryValue dict;
  auto pair = SplitAtFirst(name, ".", true);
  if (pair.first.empty()) {
    return Error::AddToPrintf(error, FROM_HERE,
                              errors::commands::kPropertyMissing,
                              "Empty state package in '%s'", name.c_str());
  }
  if (pair.second.empty()) {
    return Error::AddToPrintf(
        error, FROM_HERE, errors::commands::kPropertyMissing,
        "State property name not specified in '%s'", name.c_str());
  }
  dict.Set(name, value.CreateDeepCopy());
  return SetStateProperties(component_path, dict, error);
}

ComponentManager::StateSnapshot
ComponentManagerImpl::GetAndClearRecordedStateChanges() {
  StateSnapshot snapshot;
  snapshot.update_id = GetLastStateChangeId();
  for (auto& pair : state_change_queues_) {
    auto changes = pair.second->GetAndClearRecordedStateChanges();
    auto component = pair.first;
    auto conv = [component](weave::StateChange& change) {
      return ComponentStateChange{change.timestamp, component,
                                  std::move(change.changed_properties)};
    };
    std::transform(changes.begin(), changes.end(),
                   std::back_inserter(snapshot.state_changes), conv);
  }

  // Sort events by the timestamp.
  auto pred = [](const ComponentStateChange& lhs,
                 const ComponentStateChange& rhs) {
    return lhs.timestamp < rhs.timestamp;
  };
  std::sort(snapshot.state_changes.begin(), snapshot.state_changes.end(), pred);
  state_change_queues_.clear();
  return snapshot;
}

void ComponentManagerImpl::NotifyStateUpdatedOnServer(UpdateID id) {
  on_server_state_updated_.Notify(id);
}

ComponentManager::Token ComponentManagerImpl::AddServerStateUpdatedCallback(
    const base::Callback<void(UpdateID)>& callback) {
  if (state_change_queues_.empty())
    callback.Run(GetLastStateChangeId());
  return Token{on_server_state_updated_.Add(callback)};
}

std::string ComponentManagerImpl::FindComponentWithTrait(
    const std::string& trait) const {
  for (base::DictionaryValue::Iterator it(components_); !it.IsAtEnd();
       it.Advance()) {
    const base::ListValue* supported_traits = nullptr;
    const base::DictionaryValue* component = nullptr;
    CHECK(it.value().GetAsDictionary(&component));
    if (component->GetList("traits", &supported_traits)) {
      for (const auto& value : *supported_traits) {
        std::string supported_trait;
        CHECK(value->GetAsString(&supported_trait));
        if (trait == supported_trait)
          return it.key();
      }
    }
  }
  return std::string{};
}

base::DictionaryValue* ComponentManagerImpl::FindComponentGraftNode(
    const std::string& path,
    ErrorPtr* error) {
  base::DictionaryValue* root = nullptr;
  base::DictionaryValue* component = FindMutableComponent(path, error);
  if (component && !component->GetDictionary("components", &root)) {
    root = new base::DictionaryValue;
    component->Set("components", root);
  }
  return root;
}

base::DictionaryValue* ComponentManagerImpl::FindMutableComponent(
    const std::string& path,
    ErrorPtr* error) {
  return const_cast<base::DictionaryValue*>(
      FindComponentAt(&components_, path, error));
}

const base::DictionaryValue* ComponentManagerImpl::FindComponentAt(
    const base::DictionaryValue* root,
    const std::string& path,
    ErrorPtr* error) {
  auto parts = Split(path, ".", true, false);
  std::string root_path;
  for (size_t i = 0; i < parts.size(); i++) {
    auto element = SplitAtFirst(parts[i], "[", true);
    int array_index = -1;
    if (element.first.empty()) {
      return Error::AddToPrintf(
          error, FROM_HERE, errors::commands::kPropertyMissing,
          "Empty path element at '%s'", root_path.c_str());
    }
    if (!element.second.empty()) {
      if (element.second.back() != ']') {
        return Error::AddToPrintf(
            error, FROM_HERE, errors::commands::kPropertyMissing,
            "Invalid array element syntax '%s'", parts[i].c_str());
      }
      element.second.pop_back();
      std::string index_str;
      base::TrimWhitespaceASCII(element.second, base::TrimPositions::TRIM_ALL,
                                &index_str);
      if (!base::StringToInt(index_str, &array_index) || array_index < 0) {
        return Error::AddToPrintf(
            error, FROM_HERE, errors::commands::kInvalidPropValue,
            "Invalid array index '%s'", element.second.c_str());
      }
    }

    if (!root_path.empty()) {
      // We have processed at least one item in the path before, so now |root|
      // points to the actual parent component. We need the root to point to
      // the 'components' element containing child sub-components instead.
      if (!root->GetDictionary("components", &root)) {
        return Error::AddToPrintf(error, FROM_HERE,
                                  errors::commands::kPropertyMissing,
                                  "Component '%s' does not exist at '%s'",
                                  element.first.c_str(), root_path.c_str());
      }
    }

    const base::Value* value = nullptr;
    if (!root->GetWithoutPathExpansion(element.first, &value)) {
      Error::AddToPrintf(error, FROM_HERE, errors::commands::kPropertyMissing,
                         "Component '%s' does not exist at '%s'",
                         element.first.c_str(), root_path.c_str());
      return nullptr;
    }

    if (value->GetType() == base::Value::TYPE_LIST && array_index < 0) {
      return Error::AddToPrintf(error, FROM_HERE,
                                errors::commands::kTypeMismatch,
                                "Element '%s.%s' is an array",
                                root_path.c_str(), element.first.c_str());
    }
    if (value->GetType() == base::Value::TYPE_DICTIONARY && array_index >= 0) {
      return Error::AddToPrintf(error, FROM_HERE,
                                errors::commands::kTypeMismatch,
                                "Element '%s.%s' is not an array",
                                root_path.c_str(), element.first.c_str());
    }

    if (value->GetType() == base::Value::TYPE_DICTIONARY) {
      CHECK(value->GetAsDictionary(&root));
    } else {
      const base::ListValue* component_array = nullptr;
      CHECK(value->GetAsList(&component_array));
      const base::Value* component_value = nullptr;
      if (!component_array->Get(array_index, &component_value) ||
          !component_value->GetAsDictionary(&root)) {
        return Error::AddToPrintf(
            error, FROM_HERE, errors::commands::kPropertyMissing,
            "Element '%s.%s' does not contain item #%d", root_path.c_str(),
            element.first.c_str(), array_index);
      }
    }
    if (!root_path.empty())
      root_path += '.';
    root_path += parts[i];
  }
  return root;
}

}  // namespace weave
