// Copyright 2016 The Weave Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/access_api_handler.h"

#include <base/bind.h>
#include <weave/device.h>

#include "src/access_black_list_manager.h"
#include "src/commands/schema_constants.h"
#include "src/data_encoding.h"
#include "src/json_error_codes.h"
#include "src/utils.h"

namespace weave {

namespace {

const char kComponent[] = "accessControl";
const char kTrait[] = "_accessRevocationList";
const char kStateCapacity[] = "_accessRevocationList.capacity";
const char kUserId[] = "userId";
const char kApplicationId[] = "applicationId";
const char kExpirationTime[] = "expirationTime";
const char kBlackList[] = "revocationListEntries";

bool GetIds(const base::DictionaryValue& parameters,
            std::vector<uint8_t>* user_id_decoded,
            std::vector<uint8_t>* app_id_decoded,
            ErrorPtr* error) {
  std::string user_id;
  parameters.GetString(kUserId, &user_id);
  if (!Base64Decode(user_id, user_id_decoded)) {
    Error::AddToPrintf(error, FROM_HERE, errors::commands::kInvalidPropValue,
                       "Invalid user id '%s'", user_id.c_str());
    return false;
  }

  std::string app_id;
  parameters.GetString(kApplicationId, &app_id);
  if (!Base64Decode(app_id, app_id_decoded)) {
    Error::AddToPrintf(error, FROM_HERE, errors::commands::kInvalidPropValue,
                       "Invalid app id '%s'", user_id.c_str());
    return false;
  }

  return true;
}

}  // namespace

AccessApiHandler::AccessApiHandler(Device* device,
                                   AccessBlackListManager* manager)
    : device_{device}, manager_{manager} {
  device_->AddTraitDefinitionsFromJson(R"({
    "_accessRevocationList": {
      "commands": {
        "revoke": {
          "minimalRole": "owner",
          "parameters": {
            "userId": {
              "type": "string"
            },
            "applicationId": {
              "type": "string"
            },
            "expirationTime": {
              "type": "integer"
            }
          }
        },
        "list": {
          "minimalRole": "owner",
          "parameters": {},
          "results": {
            "revocationEntriesList": {
              "type": "array",
              "items": {
                "type": "object",
                "properties": {
                  "userId": {
                    "type": "string"
                  },
                  "applicationId": {
                    "type": "string"
                  },
                  "expirationTime": {
                    "type": "integer"
                  }
                },
                "additionalProperties": false
              }
            }
          }
        }
      },
      "state": {
        "capacity": {
          "type": "integer",
          "isRequired": true
        }
      }
    }
  })");
  CHECK(device_->AddComponent(kComponent, {kTrait}, nullptr));
  UpdateState();

  device_->AddCommandHandler(
      kComponent, "_accessRevocationList.revoke",
      base::Bind(&AccessApiHandler::Block, weak_ptr_factory_.GetWeakPtr()));
  device_->AddCommandHandler(
      kComponent, "_accessRevocationList.list",
      base::Bind(&AccessApiHandler::List, weak_ptr_factory_.GetWeakPtr()));
}

void AccessApiHandler::Block(const std::weak_ptr<Command>& cmd) {
  auto command = cmd.lock();
  if (!command)
    return;

  CHECK(command->GetState() == Command::State::kQueued)
      << EnumToString(command->GetState());
  command->SetProgress(base::DictionaryValue{}, nullptr);

  const auto& parameters = command->GetParameters();
  std::vector<uint8_t> user_id;
  std::vector<uint8_t> app_id;
  ErrorPtr error;
  if (!GetIds(parameters, &user_id, &app_id, &error)) {
    command->Abort(error.get(), nullptr);
    return;
  }

  int time_j2k = 0;
  if (!parameters.GetInteger(kExpirationTime, &time_j2k)) {
    Error::AddToPrintf(&error, FROM_HERE, errors::commands::kInvalidPropValue,
                       "Expiration time is missing");
    command->Abort(error.get(), nullptr);
    return;
  }

  manager_->Block(user_id, app_id, FromJ2000Time(time_j2k),
                  base::Bind(&AccessApiHandler::OnCommandDone,
                             weak_ptr_factory_.GetWeakPtr(), cmd));
}

void AccessApiHandler::List(const std::weak_ptr<Command>& cmd) {
  auto command = cmd.lock();
  if (!command)
    return;

  CHECK(command->GetState() == Command::State::kQueued)
      << EnumToString(command->GetState());
  command->SetProgress(base::DictionaryValue{}, nullptr);

  std::unique_ptr<base::ListValue> entries{new base::ListValue};
  for (const auto& e : manager_->GetEntries()) {
    std::unique_ptr<base::DictionaryValue> entry{new base::DictionaryValue};
    entry->SetString(kUserId, Base64Encode(e.user_id));
    entry->SetString(kApplicationId, Base64Encode(e.app_id));
    entries->Append(entry.release());
  }

  base::DictionaryValue result;
  result.Set(kBlackList, entries.release());

  command->Complete(result, nullptr);
}

void AccessApiHandler::OnCommandDone(const std::weak_ptr<Command>& cmd,
                                     ErrorPtr error) {
  auto command = cmd.lock();
  if (!command)
    return;
  UpdateState();
  if (error) {
    command->Abort(error.get(), nullptr);
    return;
  }
  command->Complete({}, nullptr);
}

void AccessApiHandler::UpdateState() {
  base::DictionaryValue state;
  state.SetInteger(kStateCapacity, manager_->GetCapacity());
  device_->SetStateProperties(kComponent, state, nullptr);
}

}  // namespace weave
