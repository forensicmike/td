//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2024
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/ReferralProgramManager.h"

#include "td/telegram/ChatManager.h"
#include "td/telegram/DialogManager.h"
#include "td/telegram/Global.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/ReferralProgramInfo.h"
#include "td/telegram/StarManager.h"
#include "td/telegram/Td.h"
#include "td/telegram/UserManager.h"

#include "td/utils/buffer.h"
#include "td/utils/misc.h"
#include "td/utils/Status.h"

namespace td {

class UpdateStarRefProgramQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  UserId user_id_;

 public:
  explicit UpdateStarRefProgramQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(UserId user_id, telegram_api::object_ptr<telegram_api::InputUser> &&input_user,
            const ReferralProgramParameters &parameters) {
    user_id_ = user_id;
    int32 flags = 0;
    if (parameters.get_month_count() != 0) {
      flags |= telegram_api::bots_updateStarRefProgram::DURATION_MONTHS_MASK;
    }
    send_query(G()->net_query_creator().create(telegram_api::bots_updateStarRefProgram(
        flags, std::move(input_user), parameters.get_commission(), parameters.get_month_count())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::bots_updateStarRefProgram>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for UpdateStarRefProgramQuery: " << to_string(ptr);
    td_->user_manager_->on_update_user_referral_program_info(user_id_, ReferralProgramInfo(std::move(ptr)));
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class ResolveReferralProgramQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chat>> promise_;

 public:
  explicit ResolveReferralProgramQuery(Promise<td_api::object_ptr<td_api::chat>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(const string &username, const string &referrer) {
    int32 flags = telegram_api::contacts_resolveUsername::REFERER_MASK;
    send_query(G()->net_query_creator().create(telegram_api::contacts_resolveUsername(flags, username, referrer)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_resolveUsername>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for ResolveReferralProgramQuery: " << to_string(ptr);
    td_->user_manager_->on_get_users(std::move(ptr->users_), "ResolveReferralProgramQuery");
    td_->chat_manager_->on_get_chats(std::move(ptr->chats_), "ResolveReferralProgramQuery");

    DialogId dialog_id(ptr->peer_);
    if (dialog_id.get_type() != DialogType::User || !td_->user_manager_->have_user(dialog_id.get_user_id())) {
      return on_error(Status::Error(400, "Chat not found"));
    }

    td_->messages_manager_->force_create_dialog(dialog_id, "ResolveReferralProgramQuery");
    promise_.set_value(td_->messages_manager_->get_chat_object(dialog_id, "ResolveReferralProgramQuery"));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class ReferralProgramManager::GetSuggestedStarRefBotsQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::foundAffiliatePrograms>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetSuggestedStarRefBotsQuery(Promise<td_api::object_ptr<td_api::foundAffiliatePrograms>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, ReferralProgramSortOrder sort_order, const string &offset, int32 limit) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->dialog_manager_->get_input_peer(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);
    int32 flags = 0;
    switch (sort_order) {
      case ReferralProgramSortOrder::Profitability:
        break;
      case ReferralProgramSortOrder::Date:
        flags |= telegram_api::payments_getSuggestedStarRefBots::ORDER_BY_DATE_MASK;
        break;
      case ReferralProgramSortOrder::Revenue:
        flags |= telegram_api::payments_getSuggestedStarRefBots::ORDER_BY_REVENUE_MASK;
        break;
      default:
        UNREACHABLE();
    }
    send_query(G()->net_query_creator().create(telegram_api::payments_getSuggestedStarRefBots(
        flags, false /*ignored*/, false /*ignored*/, std::move(input_peer), offset, limit)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getSuggestedStarRefBots>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetSuggestedStarRefBotsQuery: " << to_string(ptr);

    td_->user_manager_->on_get_users(std::move(ptr->users_), "GetSuggestedStarRefBotsQuery");

    vector<td_api::object_ptr<td_api::foundAffiliateProgram>> programs;
    for (auto &ref : ptr->suggested_bots_) {
      SuggestedBotStarRef star_ref(std::move(ref));
      if (!star_ref.is_valid()) {
        LOG(ERROR) << "Receive invalid referral program for " << dialog_id_;
        continue;
      }
      programs.push_back(star_ref.get_found_affiliate_program_object(td_));
    }

    auto total_count = ptr->count_;
    if (total_count < static_cast<int32>(programs.size())) {
      LOG(ERROR) << "Receive total count = " << total_count << ", but " << programs.size() << " referral programs";
      total_count = static_cast<int32>(programs.size());
    }
    promise_.set_value(
        td_api::make_object<td_api::foundAffiliatePrograms>(total_count, std::move(programs), ptr->next_offset_));
  }

  void on_error(Status status) final {
    td_->dialog_manager_->on_get_dialog_error(dialog_id_, status, "GetSuggestedStarRefBotsQuery");
    promise_.set_error(std::move(status));
  }
};

class ReferralProgramManager::ConnectStarRefBotQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatAffiliateProgram>> promise_;
  DialogId dialog_id_;

 public:
  explicit ConnectStarRefBotQuery(Promise<td_api::object_ptr<td_api::chatAffiliateProgram>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, telegram_api::object_ptr<telegram_api::InputUser> &&input_user) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->dialog_manager_->get_input_peer(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);
    send_query(G()->net_query_creator().create(
        telegram_api::payments_connectStarRefBot(std::move(input_peer), std::move(input_user))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_connectStarRefBot>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for ConnectStarRefBotQuery: " << to_string(ptr);
    if (ptr->connected_bots_.size() != 1u) {
      return on_error(Status::Error(500, "Receive invalid response"));
    }

    td_->user_manager_->on_get_users(std::move(ptr->users_), "ConnectStarRefBotQuery");

    ConnectedBotStarRef ref(std::move(ptr->connected_bots_[0]));
    if (!ref.is_valid()) {
      LOG(ERROR) << "Receive invalid connected referral program for " << dialog_id_;
      return on_error(Status::Error(500, "Receive invalid response"));
    }
    promise_.set_value(ref.get_chat_affiliate_program_object(td_));
  }

  void on_error(Status status) final {
    td_->dialog_manager_->on_get_dialog_error(dialog_id_, status, "ConnectStarRefBotQuery");
    promise_.set_error(std::move(status));
  }
};

class ReferralProgramManager::EditConnectedStarRefBotQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatAffiliateProgram>> promise_;
  DialogId dialog_id_;

 public:
  explicit EditConnectedStarRefBotQuery(Promise<td_api::object_ptr<td_api::chatAffiliateProgram>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &url) {
    dialog_id_ = dialog_id;
    int32 flags = telegram_api::payments_editConnectedStarRefBot::REVOKED_MASK;
    auto input_peer = td_->dialog_manager_->get_input_peer(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);
    send_query(G()->net_query_creator().create(
        telegram_api::payments_editConnectedStarRefBot(flags, false /*ignored*/, std::move(input_peer), url)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_editConnectedStarRefBot>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for EditConnectedStarRefBotQuery: " << to_string(ptr);
    if (ptr->connected_bots_.size() != 1u) {
      return on_error(Status::Error(500, "Receive invalid response"));
    }

    td_->user_manager_->on_get_users(std::move(ptr->users_), "EditConnectedStarRefBotQuery");

    ConnectedBotStarRef ref(std::move(ptr->connected_bots_[0]));
    if (!ref.is_valid()) {
      LOG(ERROR) << "Receive invalid connected referral program for " << dialog_id_;
      return on_error(Status::Error(500, "Receive invalid response"));
    }
    promise_.set_value(ref.get_chat_affiliate_program_object(td_));
  }

  void on_error(Status status) final {
    td_->dialog_manager_->on_get_dialog_error(dialog_id_, status, "EditConnectedStarRefBotQuery");
    promise_.set_error(std::move(status));
  }
};

class ReferralProgramManager::GetConnectedStarRefBotQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatAffiliateProgram>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetConnectedStarRefBotQuery(Promise<td_api::object_ptr<td_api::chatAffiliateProgram>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, telegram_api::object_ptr<telegram_api::InputUser> &&input_user) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->dialog_manager_->get_input_peer(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);
    send_query(G()->net_query_creator().create(
        telegram_api::payments_getConnectedStarRefBot(std::move(input_peer), std::move(input_user))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getConnectedStarRefBot>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for ConnectStarRefBotQuery: " << to_string(ptr);
    if (ptr->connected_bots_.size() != 1u) {
      if (ptr->connected_bots_.empty()) {
        return promise_.set_value(nullptr);
      }
      return on_error(Status::Error(500, "Receive invalid response"));
    }

    td_->user_manager_->on_get_users(std::move(ptr->users_), "GetConnectedStarRefBotQuery");

    ConnectedBotStarRef ref(std::move(ptr->connected_bots_[0]));
    if (!ref.is_valid()) {
      LOG(ERROR) << "Receive invalid connected referral program for " << dialog_id_;
      return on_error(Status::Error(500, "Receive invalid response"));
    }
    promise_.set_value(ref.get_chat_affiliate_program_object(td_));
  }

  void on_error(Status status) final {
    td_->dialog_manager_->on_get_dialog_error(dialog_id_, status, "GetConnectedStarRefBotQuery");
    promise_.set_error(std::move(status));
  }
};

class ReferralProgramManager::GetConnectedStarRefBotsQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatAffiliatePrograms>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetConnectedStarRefBotsQuery(Promise<td_api::object_ptr<td_api::chatAffiliatePrograms>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &offset, int32 limit) {
    dialog_id_ = dialog_id;
    int32 date = 0;
    string link;
    int32 flags = 0;
    if (!offset.empty()) {
      auto splitted_offset = split(offset);
      date = to_integer<int32>(splitted_offset.first);
      link = std::move(splitted_offset.second);
      flags |= telegram_api::payments_getConnectedStarRefBots::OFFSET_DATE_MASK;
    }
    auto input_peer = td_->dialog_manager_->get_input_peer(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);

    send_query(G()->net_query_creator().create(
        telegram_api::payments_getConnectedStarRefBots(flags, std::move(input_peer), date, link, limit)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getConnectedStarRefBots>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetConnectedStarRefBotsQuery: " << to_string(ptr);

    td_->user_manager_->on_get_users(std::move(ptr->users_), "GetConnectedStarRefBotsQuery");

    vector<td_api::object_ptr<td_api::chatAffiliateProgram>> programs;
    string next_offset;
    for (auto &ref : ptr->connected_bots_) {
      next_offset = PSTRING() << ref->date_ << ' ' << ref->url_;
      ConnectedBotStarRef star_ref(std::move(ref));
      if (!star_ref.is_valid()) {
        LOG(ERROR) << "Receive invalid connected referral program for " << dialog_id_;
        continue;
      }
      programs.push_back(star_ref.get_chat_affiliate_program_object(td_));
    }

    auto total_count = ptr->count_;
    if (total_count < static_cast<int32>(programs.size())) {
      LOG(ERROR) << "Receive total count = " << total_count << ", but " << programs.size() << " referral programs";
      total_count = static_cast<int32>(programs.size());
    }
    promise_.set_value(
        td_api::make_object<td_api::chatAffiliatePrograms>(total_count, std::move(programs), next_offset));
  }

  void on_error(Status status) final {
    td_->dialog_manager_->on_get_dialog_error(dialog_id_, status, "GetConnectedStarRefBotsQuery");
    promise_.set_error(std::move(status));
  }
};

ReferralProgramManager::SuggestedBotStarRef::SuggestedBotStarRef(
    telegram_api::object_ptr<telegram_api::starRefProgram> &&ref)
    : user_id_(ref->bot_id_), info_(std::move(ref)) {
}

td_api::object_ptr<td_api::foundAffiliateProgram>
ReferralProgramManager::SuggestedBotStarRef::get_found_affiliate_program_object(Td *td) const {
  CHECK(is_valid());
  return td_api::make_object<td_api::foundAffiliateProgram>(
      td->user_manager_->get_user_id_object(user_id_, "foundAffiliateProgram"),
      info_.get_affiliate_program_info_object());
}

ReferralProgramManager::ConnectedBotStarRef::ConnectedBotStarRef(
    telegram_api::object_ptr<telegram_api::connectedBotStarRef> &&ref)
    : url_(std::move(ref->url_))
    , date_(ref->date_)
    , user_id_(ref->bot_id_)
    , parameters_(ref->commission_permille_, ref->duration_months_)
    , participant_count_(ref->participants_)
    , revenue_star_count_(StarManager::get_star_count(ref->revenue_))
    , is_revoked_(ref->revoked_) {
}

td_api::object_ptr<td_api::chatAffiliateProgram>
ReferralProgramManager::ConnectedBotStarRef::get_chat_affiliate_program_object(Td *td) const {
  CHECK(is_valid());
  return td_api::make_object<td_api::chatAffiliateProgram>(
      url_, td->user_manager_->get_user_id_object(user_id_, "chatAffiliateProgram"),
      parameters_.get_affiliate_program_parameters_object(), date_, is_revoked_, participant_count_,
      revenue_star_count_);
}

ReferralProgramManager::ReferralProgramManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
}

void ReferralProgramManager::tear_down() {
  parent_.reset();
}

void ReferralProgramManager::set_dialog_referral_program(DialogId dialog_id, ReferralProgramParameters parameters,
                                                         Promise<Unit> &&promise) {
  if (!parameters.is_valid() && parameters != ReferralProgramParameters()) {
    return promise.set_error(Status::Error(400, "Invalid affiliate parameters specified"));
  }
  switch (dialog_id.get_type()) {
    case DialogType::User: {
      TRY_RESULT_PROMISE(promise, bot_data, td_->user_manager_->get_bot_data(dialog_id.get_user_id()));
      if (bot_data.can_be_edited) {
        break;
      }
      return promise.set_error(Status::Error(400, "The bot isn't owned"));
    }
    case DialogType::Chat:
    case DialogType::Channel:
    case DialogType::SecretChat:
    case DialogType::None:
      return promise.set_error(Status::Error(400, "The chat can't have affiliate program"));
    default:
      UNREACHABLE();
  }
  auto bot_user_id = dialog_id.get_user_id();
  TRY_RESULT_PROMISE(promise, input_user, td_->user_manager_->get_input_user(bot_user_id));

  td_->create_handler<UpdateStarRefProgramQuery>(std::move(promise))
      ->send(bot_user_id, std::move(input_user), parameters);
}

void ReferralProgramManager::search_dialog_referral_program(const string &username, const string &referral,
                                                            Promise<td_api::object_ptr<td_api::chat>> &&promise) {
  td_->create_handler<ResolveReferralProgramQuery>(std::move(promise))->send(username, referral);
}

Status ReferralProgramManager::check_referable_dialog_id(DialogId dialog_id) const {
  TRY_STATUS(
      td_->dialog_manager_->check_dialog_access(dialog_id, false, AccessRights::Read, "check_referable_dialog_id"));
  switch (dialog_id.get_type()) {
    case DialogType::User: {
      if (dialog_id == td_->dialog_manager_->get_my_dialog_id()) {
        break;
      }
      TRY_RESULT(bot_data, td_->user_manager_->get_bot_data(dialog_id.get_user_id()));
      if (bot_data.can_be_edited) {
        break;
      }
      return Status::Error(400, "The bot isn't owned");
    }
    case DialogType::Chat:
      return Status::Error(400, "The chat must be a channel chat");
    case DialogType::Channel: {
      auto channel_id = dialog_id.get_channel_id();
      if (!td_->chat_manager_->is_broadcast_channel(channel_id)) {
        return Status::Error(400, "The chat must be a channel chat");
      }
      auto status = td_->chat_manager_->get_channel_permissions(channel_id);
      if (!status.can_post_messages()) {
        return Status::Error(400, "Not enough rights in the chat");
      }
      break;
    }
    case DialogType::SecretChat:
    case DialogType::None:
    default:
      UNREACHABLE();
  }
  return Status::OK();
}

void ReferralProgramManager::search_referral_programs(
    DialogId dialog_id, ReferralProgramSortOrder sort_order, const string &offset, int32 limit,
    Promise<td_api::object_ptr<td_api::foundAffiliatePrograms>> &&promise) {
  TRY_STATUS_PROMISE(promise, check_referable_dialog_id(dialog_id));
  if (limit <= 0) {
    return promise.set_error(Status::Error(400, "Limit must be positive"));
  }

  td_->create_handler<GetSuggestedStarRefBotsQuery>(std::move(promise))->send(dialog_id, sort_order, offset, limit);
}

void ReferralProgramManager::connect_referral_program(
    DialogId dialog_id, UserId bot_user_id, Promise<td_api::object_ptr<td_api::chatAffiliateProgram>> &&promise) {
  TRY_STATUS_PROMISE(promise, check_referable_dialog_id(dialog_id));
  TRY_RESULT_PROMISE(promise, input_user, td_->user_manager_->get_input_user(bot_user_id));

  td_->create_handler<ConnectStarRefBotQuery>(std::move(promise))->send(dialog_id, std::move(input_user));
}

void ReferralProgramManager::revoke_referral_program(
    DialogId dialog_id, const string &url, Promise<td_api::object_ptr<td_api::chatAffiliateProgram>> &&promise) {
  TRY_STATUS_PROMISE(promise, check_referable_dialog_id(dialog_id));

  td_->create_handler<EditConnectedStarRefBotQuery>(std::move(promise))->send(dialog_id, url);
}

void ReferralProgramManager::get_connected_referral_program(
    DialogId dialog_id, UserId bot_user_id, Promise<td_api::object_ptr<td_api::chatAffiliateProgram>> &&promise) {
  TRY_STATUS_PROMISE(promise, check_referable_dialog_id(dialog_id));
  TRY_RESULT_PROMISE(promise, input_user, td_->user_manager_->get_input_user(bot_user_id));

  td_->create_handler<GetConnectedStarRefBotQuery>(std::move(promise))->send(dialog_id, std::move(input_user));
}

void ReferralProgramManager::get_connected_referral_programs(
    DialogId dialog_id, const string &offset, int32 limit,
    Promise<td_api::object_ptr<td_api::chatAffiliatePrograms>> &&promise) {
  TRY_STATUS_PROMISE(promise, check_referable_dialog_id(dialog_id));
  if (limit <= 0) {
    return promise.set_error(Status::Error(400, "Limit must be positive"));
  }

  td_->create_handler<GetConnectedStarRefBotsQuery>(std::move(promise))->send(dialog_id, offset, limit);
}

}  // namespace td