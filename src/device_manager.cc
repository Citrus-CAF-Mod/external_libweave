// Copyright 2015 The Weave Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/device_manager.h"

#include <string>

#include <base/bind.h>

#include "src/access_api_handler.h"
#include "src/access_revocation_manager_impl.h"
#include "src/base_api_handler.h"
#include "src/commands/schema_constants.h"
#include "src/component_manager_impl.h"
#include "src/config.h"
#include "src/device_registration_info.h"
#include "src/privet/auth_manager.h"
#include "src/privet/privet_manager.h"
#include "src/string_utils.h"
#include "src/utils.h"

namespace weave {

DeviceManager::DeviceManager(provider::ConfigStore* config_store,
                             provider::TaskRunner* task_runner,
                             provider::HttpClient* http_client,
                             provider::Network* network,
                             provider::DnsServiceDiscovery* dns_sd,
                             provider::HttpServer* http_server,
                             provider::Wifi* wifi,
                             provider::Bluetooth* bluetooth)
    : task_runner_{task_runner},
      network_{network},
      dns_sd_{dns_sd},
      http_server_{http_server},
      wifi_{wifi},
      config_{new Config{config_store}},
      component_manager_{new ComponentManagerImpl{task_runner}} {
  if (http_server) {
    access_revocation_manager_.reset(
        new AccessRevocationManagerImpl{config_store});
    auth_manager_.reset(
        new privet::AuthManager(config_.get(), access_revocation_manager_.get(),
                                http_server->GetHttpsCertificateFingerprint()));
    access_api_handler_.reset(
        new AccessApiHandler{this, access_revocation_manager_.get()});
  }

  device_info_.reset(new DeviceRegistrationInfo(
      config_.get(), component_manager_.get(), task_runner, http_client,
      network, auth_manager_.get()));
  base_api_handler_.reset(new BaseApiHandler{device_info_.get(), this});

  device_info_->Start();

  if (http_server) {
    StartPrivet();
    AddSettingsChangedCallback(base::Bind(&DeviceManager::OnSettingsChanged,
                                          weak_ptr_factory_.GetWeakPtr()));
  } else {
    CHECK(!dns_sd);
  }
}

DeviceManager::~DeviceManager() {}

const Settings& DeviceManager::GetSettings() const {
  return device_info_->GetSettings();
}

void DeviceManager::AddSettingsChangedCallback(
    const SettingsChangedCallback& callback) {
  device_info_->GetMutableConfig()->AddOnChangedCallback(callback);
}

Config* DeviceManager::GetConfig() {
  return device_info_->GetMutableConfig();
}

void DeviceManager::StartPrivet() {
  if (privet_)
    return;
  privet_.reset(new privet::Manager{task_runner_});
  privet_->Start(network_, dns_sd_, http_server_, wifi_, auth_manager_.get(),
                 device_info_.get(), component_manager_.get());
}

void DeviceManager::StopPrivet() {
  privet_.reset();
}

GcdState DeviceManager::GetGcdState() const {
  return device_info_->GetGcdState();
}

void DeviceManager::AddGcdStateChangedCallback(
    const GcdStateChangedCallback& callback) {
  device_info_->AddGcdStateChangedCallback(callback);
}

void DeviceManager::AddTraitDefinitionsFromJson(const std::string& json) {
  CHECK(component_manager_->LoadTraits(json, nullptr));
}

void DeviceManager::AddTraitDefinitions(const base::DictionaryValue& dict) {
  CHECK(component_manager_->LoadTraits(dict, nullptr));
}

const base::DictionaryValue& DeviceManager::GetTraits() const {
  return component_manager_->GetTraits();
}

void DeviceManager::AddTraitDefsChangedCallback(const base::Closure& callback) {
  component_manager_->AddTraitDefChangedCallback(callback);
}

bool DeviceManager::AddComponent(const std::string& name,
                                 const std::vector<std::string>& traits,
                                 ErrorPtr* error) {
  return component_manager_->AddComponent("", name, traits, error);
}

bool DeviceManager::RemoveComponent(const std::string& name, ErrorPtr* error) {
  return component_manager_->RemoveComponent("", name, error);
}

void DeviceManager::AddComponentTreeChangedCallback(
    const base::Closure& callback) {
  component_manager_->AddComponentTreeChangedCallback(callback);
}

const base::DictionaryValue& DeviceManager::GetComponents() const {
  return component_manager_->GetComponents();
}

bool DeviceManager::SetStatePropertiesFromJson(const std::string& component,
                                               const std::string& json,
                                               ErrorPtr* error) {
  return component_manager_->SetStatePropertiesFromJson(component, json, error);
}

bool DeviceManager::SetStateProperties(const std::string& component,
                                       const base::DictionaryValue& dict,
                                       ErrorPtr* error) {
  return component_manager_->SetStateProperties(component, dict, error);
}

const base::Value* DeviceManager::GetStateProperty(const std::string& component,
                                                   const std::string& name,
                                                   ErrorPtr* error) const {
  return component_manager_->GetStateProperty(component, name, error);
}

bool DeviceManager::SetStateProperty(const std::string& component,
                                     const std::string& name,
                                     const base::Value& value,
                                     ErrorPtr* error) {
  return component_manager_->SetStateProperty(component, name, value, error);
}

void DeviceManager::AddCommandHandler(const std::string& component,
                                      const std::string& command_name,
                                      const CommandHandlerCallback& callback) {
  component_manager_->AddCommandHandler(component, command_name, callback);
}

bool DeviceManager::AddCommand(const base::DictionaryValue& command,
                               std::string* id,
                               ErrorPtr* error) {
  auto command_instance = component_manager_->ParseCommandInstance(
      command, Command::Origin::kLocal, UserRole::kOwner, id, error);
  if (!command_instance)
    return false;
  component_manager_->AddCommand(std::move(command_instance));
  return true;
}

Command* DeviceManager::FindCommand(const std::string& id) {
  return component_manager_->FindCommand(id);
}

void DeviceManager::AddStateChangedCallback(const base::Closure& callback) {
  component_manager_->AddStateChangedCallback(callback);
}

void DeviceManager::Register(const RegistrationData& registration_data,
                             const DoneCallback& callback) {
  device_info_->RegisterDevice(registration_data, callback);
}

void DeviceManager::AddPairingChangedCallbacks(
    const PairingBeginCallback& begin_callback,
    const PairingEndCallback& end_callback) {
  if (privet_)
    privet_->AddOnPairingChangedCallbacks(begin_callback, end_callback);
}

void DeviceManager::OnSettingsChanged(const Settings& settings) {
  if (settings.local_access_enabled && http_server_) {
    StartPrivet();
  } else {
    StopPrivet();
  }
}

std::unique_ptr<Device> Device::Create(provider::ConfigStore* config_store,
                                       provider::TaskRunner* task_runner,
                                       provider::HttpClient* http_client,
                                       provider::Network* network,
                                       provider::DnsServiceDiscovery* dns_sd,
                                       provider::HttpServer* http_server,
                                       provider::Wifi* wifi,
                                       provider::Bluetooth* bluetooth) {
  return std::unique_ptr<Device>{
      new DeviceManager{config_store, task_runner, http_client, network, dns_sd,
                        http_server, wifi, bluetooth}};
}

}  // namespace weave
