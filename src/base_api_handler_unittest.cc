// Copyright 2015 The Weave Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base_api_handler.h"

#include <base/strings/string_number_conversions.h>
#include <base/values.h>
#include <gtest/gtest.h>
#include <weave/provider/test/mock_config_store.h>
#include <weave/provider/test/mock_http_client.h>
#include <weave/test/mock_device.h>

#include "src/commands/command_manager.h"
#include "src/commands/unittest_utils.h"
#include "src/config.h"
#include "src/device_registration_info.h"
#include "src/states/mock_state_change_queue_interface.h"
#include "src/states/state_manager.h"

using testing::_;
using testing::AnyOf;
using testing::Eq;
using testing::Invoke;
using testing::Return;
using testing::ReturnRef;
using testing::StrictMock;

namespace weave {

class BaseApiHandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    EXPECT_CALL(mock_state_change_queue_, MockNotifyPropertiesUpdated(_, _))
        .WillRepeatedly(Return(true));

    command_manager_ = std::make_shared<CommandManager>();

    state_manager_ = std::make_shared<StateManager>(&mock_state_change_queue_);

    EXPECT_CALL(device_, AddStateDefinitionsFromJson(_))
        .WillRepeatedly(Invoke([this](const std::string& json) {
          EXPECT_TRUE(
              state_manager_->LoadStateDefinitionFromJson(json, nullptr));
        }));
    EXPECT_CALL(device_, SetStateProperties(_, _))
        .WillRepeatedly(
            Invoke(state_manager_.get(), &StateManager::SetProperties));
    EXPECT_CALL(device_, AddCommandDefinitionsFromJson(_))
        .WillRepeatedly(Invoke([this](const std::string& json) {
          EXPECT_TRUE(command_manager_->LoadCommands(json, nullptr));
        }));

    EXPECT_CALL(device_, AddCommandHandler(AnyOf("base.updateBaseConfiguration",
                                                 "base.updateDeviceInfo"),
                                           _))
        .WillRepeatedly(
            Invoke(command_manager_.get(), &CommandManager::AddCommandHandler));

    std::unique_ptr<Config> config{new Config{&config_store_}};
    config->Load();
    dev_reg_.reset(new DeviceRegistrationInfo(command_manager_, state_manager_,
                                              std::move(config), nullptr,
                                              &http_client_, nullptr));

    EXPECT_CALL(device_, GetSettings())
        .WillRepeatedly(ReturnRef(dev_reg_->GetSettings()));

    handler_.reset(new BaseApiHandler{dev_reg_.get(), &device_});
  }

  void AddCommand(const std::string& command) {
    auto command_instance = CommandInstance::FromJson(
        test::CreateDictionaryValue(command.c_str()).get(),
        Command::Origin::kLocal, command_manager_->GetCommandDictionary(),
        nullptr, nullptr);
    EXPECT_TRUE(!!command_instance);

    std::string id{base::IntToString(++command_id_)};
    command_instance->SetID(id);
    command_manager_->AddCommand(std::move(command_instance));
    EXPECT_EQ(Command::State::kDone,
              command_manager_->FindCommand(id)->GetState());
  }

  std::unique_ptr<base::DictionaryValue> GetBaseState() {
    auto state = state_manager_->GetState();
    std::set<std::string> result;
    for (base::DictionaryValue::Iterator it{*state}; !it.IsAtEnd();
         it.Advance()) {
      if (it.key() != "base")
        state->Remove(it.key(), nullptr);
    }
    return state;
  }

  provider::test::MockConfigStore config_store_;
  StrictMock<provider::test::MockHttpClient> http_client_;
  std::unique_ptr<DeviceRegistrationInfo> dev_reg_;
  std::shared_ptr<CommandManager> command_manager_;
  testing::StrictMock<MockStateChangeQueueInterface> mock_state_change_queue_;
  std::shared_ptr<StateManager> state_manager_;
  std::unique_ptr<BaseApiHandler> handler_;
  StrictMock<test::MockDevice> device_;
  int command_id_{0};
};

TEST_F(BaseApiHandlerTest, Initialization) {
  auto command_defs =
      command_manager_->GetCommandDictionary().GetCommandsAsJson(nullptr);

  auto expected = R"({
    "base": {
      "updateBaseConfiguration": {
        "minimalRole": "manager",
        "parameters": {
          "localAnonymousAccessMaxRole": {
            "enum": [ "none", "viewer", "user" ],
            "type": "string"
          },
          "localDiscoveryEnabled": {
            "type": "boolean"
          },
          "localPairingEnabled": {
            "type": "boolean"
          }
        }
      },
      "updateDeviceInfo": {
        "minimalRole": "manager",
        "parameters": {
          "description": {
            "type": "string"
          },
          "location": {
            "type": "string"
          },
          "name": {
            "type": "string"
          }
        }
      }
    }
  })";
  EXPECT_JSON_EQ(expected, *command_defs);
}

TEST_F(BaseApiHandlerTest, UpdateBaseConfiguration) {
  const Settings& settings = dev_reg_->GetSettings();

  AddCommand(R"({
    'name' : 'base.updateBaseConfiguration',
    'parameters': {
      'localDiscoveryEnabled': false,
      'localAnonymousAccessMaxRole': 'none',
      'localPairingEnabled': false
    }
  })");
  EXPECT_EQ(AuthScope::kNone, settings.local_anonymous_access_role);
  EXPECT_FALSE(settings.local_discovery_enabled);
  EXPECT_FALSE(settings.local_pairing_enabled);

  auto expected = R"({
    'base': {
      'firmwareVersion': 'TEST_FIRMWARE',
      'localAnonymousAccessMaxRole': 'none',
      'localDiscoveryEnabled': false,
      'localPairingEnabled': false
    }
  })";
  EXPECT_JSON_EQ(expected, *GetBaseState());

  AddCommand(R"({
    'name' : 'base.updateBaseConfiguration',
    'parameters': {
      'localDiscoveryEnabled': true,
      'localAnonymousAccessMaxRole': 'user',
      'localPairingEnabled': true
    }
  })");
  EXPECT_EQ(AuthScope::kUser, settings.local_anonymous_access_role);
  EXPECT_TRUE(settings.local_discovery_enabled);
  EXPECT_TRUE(settings.local_pairing_enabled);
  expected = R"({
    'base': {
      'firmwareVersion': 'TEST_FIRMWARE',
      'localAnonymousAccessMaxRole': 'user',
      'localDiscoveryEnabled': true,
      'localPairingEnabled': true
    }
  })";
  EXPECT_JSON_EQ(expected, *GetBaseState());

  {
    Config::Transaction change{dev_reg_->GetMutableConfig()};
    change.set_local_anonymous_access_role(AuthScope::kViewer);
  }
  expected = R"({
    'base': {
      'firmwareVersion': 'TEST_FIRMWARE',
      'localAnonymousAccessMaxRole': 'viewer',
      'localDiscoveryEnabled': true,
      'localPairingEnabled': true
    }
  })";
  EXPECT_JSON_EQ(expected, *GetBaseState());
}

TEST_F(BaseApiHandlerTest, UpdateDeviceInfo) {
  AddCommand(R"({
    'name' : 'base.updateDeviceInfo',
    'parameters': {
      'name': 'testName',
      'description': 'testDescription',
      'location': 'testLocation'
    }
  })");

  const Settings& config = dev_reg_->GetSettings();
  EXPECT_EQ("testName", config.name);
  EXPECT_EQ("testDescription", config.description);
  EXPECT_EQ("testLocation", config.location);

  AddCommand(R"({
    'name' : 'base.updateDeviceInfo',
    'parameters': {
      'location': 'newLocation'
    }
  })");

  EXPECT_EQ("testName", config.name);
  EXPECT_EQ("testDescription", config.description);
  EXPECT_EQ("newLocation", config.location);
}

}  // namespace weave