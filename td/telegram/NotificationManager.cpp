//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2018
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/NotificationManager.h"

#include "td/telegram/AuthManager.h"
#include "td/telegram/ConfigShared.h"
#include "td/telegram/ContactsManager.h"
#include "td/telegram/Global.h"
#include "td/telegram/Td.h"
#include "td/telegram/TdDb.h"

#include "td/utils/misc.h"

#include <tuple>

namespace td {

int VERBOSITY_NAME(notifications) = VERBOSITY_NAME(WARNING);

NotificationManager::NotificationManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
  flush_pending_notifications_timeout_.set_callback(on_flush_pending_notifications_timeout_callback);
  flush_pending_notifications_timeout_.set_callback_data(static_cast<void *>(this));
}

void NotificationManager::on_flush_pending_notifications_timeout_callback(void *notification_manager_ptr,
                                                                          int64 group_id_int) {
  if (G()->close_flag()) {
    return;
  }

  auto notification_manager = static_cast<NotificationManager *>(notification_manager_ptr);
  notification_manager->flush_pending_notifications(NotificationGroupId(narrow_cast<int32>(group_id_int)));
}

bool NotificationManager::is_disabled() const {
  return td_->auth_manager_->is_bot();
}

void NotificationManager::start_up() {
  if (is_disabled()) {
    return;
  }

  current_notification_id_ =
      NotificationId(to_integer<int32>(G()->td_db()->get_binlog_pmc()->get("notification_id_current")));
  current_notification_group_id_ =
      NotificationGroupId(to_integer<int32>(G()->td_db()->get_binlog_pmc()->get("notification_group_id_current")));

  on_notification_group_count_max_changed();
  on_notification_group_size_max_changed();

  on_online_cloud_timeout_changed();
  on_notification_cloud_delay_changed();
  on_notification_default_delay_changed();

  // TODO load groups
}

void NotificationManager::tear_down() {
  parent_.reset();
}

NotificationManager::NotificationGroups::iterator NotificationManager::get_group(NotificationGroupId group_id) {
  // TODO optimize
  for (auto it = groups_.begin(); it != groups_.end(); ++it) {
    if (it->first.group_id == group_id) {
      return it;
    }
  }
  return groups_.end();
}

NotificationId NotificationManager::get_next_notification_id() {
  if (is_disabled()) {
    return NotificationId();
  }

  current_notification_id_ = NotificationId(current_notification_id_.get() % 0x7FFFFFFF + 1);
  G()->td_db()->get_binlog_pmc()->set("notification_id_current", to_string(current_notification_id_.get()));
  return current_notification_id_;
}

NotificationGroupId NotificationManager::get_next_notification_group_id() {
  if (is_disabled()) {
    return NotificationGroupId();
  }

  current_notification_group_id_ = NotificationGroupId(current_notification_group_id_.get() % 0x7FFFFFFF + 1);
  G()->td_db()->get_binlog_pmc()->set("notification_group_id_current", to_string(current_notification_group_id_.get()));
  return current_notification_group_id_;
}

NotificationManager::NotificationGroupKey NotificationManager::get_last_updated_group_key() const {
  int left = max_notification_group_count_;
  auto it = groups_.begin();
  while (it != groups_.end() && left > 1) {
    ++it;
    left--;
  }
  if (it == groups_.end()) {
    return NotificationGroupKey();
  }
  return it->first;
}

int32 NotificationManager::get_notification_delay_ms(DialogId dialog_id,
                                                     const PendingNotification &notification) const {
  auto delay_ms = [&]() {
    if (dialog_id.get_type() == DialogType::SecretChat) {
      return 0;  // there is no reason to delay notifications in secret chats
    }
    if (!notification.type->can_be_delayed()) {
      return 0;
    }

    auto online_info = td_->contacts_manager_->get_my_online_status();
    if (!online_info.is_online_local && online_info.is_online_remote) {
      // If we are offline, but online from some other client then delay notification
      // for 'notification_cloud_delay' seconds.
      return notification_cloud_delay_ms_;
    }

    if (!online_info.is_online_local &&
        online_info.was_online_remote > max(static_cast<double>(online_info.was_online_local),
                                            G()->server_time_cached() - online_cloud_timeout_ms_ * 1e-3)) {
      // If we are offline, but was online from some other client in last 'online_cloud_timeout' seconds
      // after we had gone offline, then delay notification for 'notification_cloud_delay' seconds.
      return notification_cloud_delay_ms_;
    }

    if (online_info.is_online_remote) {
      // If some other client is online, then delay notification for 'notification_default_delay' seconds.
      return notification_default_delay_ms_;
    }

    // otherwise send update without additional delay
    return 0;
  }();

  auto passed_time_ms = max(0, static_cast<int32>((G()->server_time_cached() - notification.date - 1) * 1000));
  return max(delay_ms - passed_time_ms, MIN_NOTIFICATION_DELAY_MS);
}

void NotificationManager::add_notification(NotificationGroupId group_id, DialogId dialog_id, int32 date,
                                           DialogId notification_settings_dialog_id, bool is_silent,
                                           NotificationId notification_id, unique_ptr<NotificationType> type) {
  if (is_disabled()) {
    return;
  }

  CHECK(group_id.is_valid());
  CHECK(dialog_id.is_valid());
  CHECK(notification_settings_dialog_id.is_valid());
  CHECK(notification_id.is_valid());
  CHECK(type != nullptr);
  VLOG(notifications) << "Add " << notification_id << " to " << group_id << " in " << dialog_id
                      << " with settings from " << notification_settings_dialog_id
                      << (is_silent ? " silent" : " with sound") << ": " << *type;

  auto group_it = get_group(group_id);
  if (group_it == groups_.end()) {
    NotificationGroupKey group_key;
    group_key.group_id = group_id;
    group_key.dialog_id = dialog_id;
    group_key.last_notification_date = 0;
    group_it = std::move(groups_.emplace(group_key, NotificationGroup()).first);
  }

  PendingNotification notification;
  notification.date = date;
  notification.settings_dialog_id = notification_settings_dialog_id;
  notification.is_silent = is_silent;
  notification.notification_id = notification_id;
  notification.type = std::move(type);

  auto delay_ms = get_notification_delay_ms(dialog_id, notification);
  VLOG(notifications) << "Delay " << notification_id << " for " << delay_ms << " milliseconds";
  auto flush_time = delay_ms * 0.001 + Time::now_cached();

  NotificationGroup &group = group_it->second;
  if (group.pending_notifications_flush_time == 0 || flush_time < group.pending_notifications_flush_time) {
    group.pending_notifications_flush_time = flush_time;
    flush_pending_notifications_timeout_.set_timeout_at(group_id.get(), group.pending_notifications_flush_time);
  }
  group.pending_notifications.push_back(std::move(notification));
}

td_api::object_ptr<td_api::notification> NotificationManager::get_notification_object(
    DialogId dialog_id, const Notification &notification) {
  return td_api::make_object<td_api::notification>(notification.notification_id.get(),
                                                   notification.type->get_notification_type_object(dialog_id));
}

void NotificationManager::send_update_notification_group(td_api::object_ptr<td_api::updateNotificationGroup> update) {
  // TODO delay and combine updates while getDifference is running
  VLOG(notifications) << "Send " << to_string(update);
  send_closure(G()->td(), &Td::send_update, std::move(update));
}

void NotificationManager::send_update_notification(NotificationGroupId notification_group_id, DialogId dialog_id,
                                                   const Notification &notification) {
  auto notification_object = get_notification_object(dialog_id, notification);
  if (notification_object->type_ == nullptr) {
    return;
  }

  // TODO delay and combine updates while getDifference is running
  auto update =
      td_api::make_object<td_api::updateNotification>(notification_group_id.get(), std::move(notification_object));
  VLOG(notifications) << "Send " << to_string(update);
  send_closure(G()->td(), &Td::send_update, std::move(update));
}

void NotificationManager::flush_pending_notifications(NotificationGroupKey &group_key, NotificationGroup &group,
                                                      vector<PendingNotification> &pending_notifications) {
  if (pending_notifications.empty()) {
    return;
  }

  VLOG(notifications) << "Flush " << pending_notifications.size() << " pending notifications in " << group_key
                      << " with available " << group.notifications.size() << " from " << group.total_count
                      << " notifications";

  size_t old_notification_count = group.notifications.size();
  size_t shown_notification_count = min(old_notification_count, max_notification_group_size_);

  vector<td_api::object_ptr<td_api::notification>> added_notifications;
  added_notifications.reserve(pending_notifications.size());
  for (auto &pending_notification : pending_notifications) {
    Notification notification{pending_notification.notification_id, std::move(pending_notification.type)};
    added_notifications.push_back(get_notification_object(group_key.dialog_id, notification));
    if (added_notifications.back()->type_ == nullptr) {
      added_notifications.pop_back();
    } else {
      group.notifications.push_back(std::move(notification));
    }
  }
  if (added_notifications.size() > max_notification_group_size_) {
    added_notifications.erase(
        added_notifications.begin(),
        added_notifications.begin() + (added_notifications.size() - max_notification_group_size_));
  }

  vector<int32> removed_notification_ids;
  if (shown_notification_count + added_notifications.size() > max_notification_group_size_) {
    auto removed_notification_count =
        shown_notification_count + added_notifications.size() - max_notification_group_size_;
    removed_notification_ids.reserve(removed_notification_count);
    for (size_t i = 0; i < removed_notification_count; i++) {
      removed_notification_ids.push_back(
          group.notifications[old_notification_count - shown_notification_count + i].notification_id.get());
    }
  }

  group.total_count += narrow_cast<int32>(added_notifications.size());
  if (!added_notifications.empty()) {
    send_update_notification_group(td_api::make_object<td_api::updateNotificationGroup>(
        group_key.group_id.get(), group_key.dialog_id.get(), pending_notifications[0].settings_dialog_id.get(),
        pending_notifications[0].is_silent, group.total_count, std::move(added_notifications),
        std::move(removed_notification_ids)));
  } else {
    CHECK(removed_notification_ids.empty());
  }
  pending_notifications.clear();
}

void NotificationManager::send_remove_group_update(NotificationGroupId group_id) {
  CHECK(group_id.is_valid());
  auto group_it = get_group(group_id);
  CHECK(group_it != groups_.end());

  auto total_size = group_it->second.notifications.size();
  auto removed_size = min(total_size, max_notification_group_size_);
  vector<int32> removed_notification_ids;
  removed_notification_ids.reserve(removed_size);
  for (size_t i = total_size - removed_size; i < total_size; i++) {
    removed_notification_ids.push_back(group_it->second.notifications[i].notification_id.get());
  }

  if (!removed_notification_ids.empty()) {
    send_update_notification_group(td_api::make_object<td_api::updateNotificationGroup>(
        group_id.get(), group_it->first.dialog_id.get(), group_it->first.dialog_id.get(), true, 0,
        vector<td_api::object_ptr<td_api::notification>>(), std::move(removed_notification_ids)));
  }
}

void NotificationManager::send_add_group_update(const NotificationGroupKey &group_key, const NotificationGroup &group) {
  auto total_size = group.notifications.size();
  auto added_size = min(total_size, max_notification_group_size_);
  vector<td_api::object_ptr<td_api::notification>> added_notifications;
  added_notifications.reserve(added_size);
  for (size_t i = total_size - added_size; i < total_size; i++) {
    added_notifications.push_back(get_notification_object(group_key.dialog_id, group.notifications[i]));
    if (added_notifications.back()->type_ == nullptr) {
      added_notifications.pop_back();
    }
  }

  if (!added_notifications.empty()) {
    send_update_notification_group(
        td_api::make_object<td_api::updateNotificationGroup>(group_key.group_id.get(), group_key.dialog_id.get(), 0,
                                                             true, 0, std::move(added_notifications), vector<int32>()));
  }
}

void NotificationManager::flush_pending_notifications(NotificationGroupId group_id) {
  auto group_it = get_group(group_id);
  CHECK(group_it != groups_.end());
  auto group_key = group_it->first;
  auto group = std::move(group_it->second);

  groups_.erase(group_it);

  CHECK(!group.pending_notifications.empty());
  auto final_group_key = group_key;
  for (auto &pending_notification : group.pending_notifications) {
    if (pending_notification.date >= final_group_key.last_notification_date) {
      final_group_key.last_notification_date = pending_notification.date;
    }
  }
  CHECK(final_group_key.last_notification_date != 0);

  VLOG(notifications) << "Flush pending notifications in " << group_key << " up to "
                      << final_group_key.last_notification_date;

  auto last_group_key = get_last_updated_group_key();
  bool was_updated = group_key.last_notification_date != 0 && group_key < last_group_key;
  bool is_updated = final_group_key < last_group_key;

  if (!is_updated) {
    CHECK(!was_updated);
    VLOG(notifications) << "There is no need to send updateNotificationGroup in " << group_key
                        << ", because of newer notification groups";
    for (auto &pending_notification : group.pending_notifications) {
      group.notifications.emplace_back(pending_notification.notification_id, std::move(pending_notification.type));
    }
  } else {
    if (!was_updated) {
      if (last_group_key.last_notification_date != 0) {
        // need to remove last notification group to not exceed max_notification_group_size_
        send_remove_group_update(last_group_key.group_id);
      }
      send_add_group_update(group_key, group);
    }

    DialogId notification_settings_dialog_id;
    bool is_silent = false;

    // split notifications by groups with common settings
    vector<PendingNotification> grouped_notifications;
    for (auto &pending_notification : group.pending_notifications) {
      if (notification_settings_dialog_id != pending_notification.settings_dialog_id ||
          is_silent != pending_notification.is_silent) {
        flush_pending_notifications(group_key, group, grouped_notifications);
        notification_settings_dialog_id = pending_notification.settings_dialog_id;
        is_silent = pending_notification.is_silent;
      }
      grouped_notifications.push_back(std::move(pending_notification));
    }
    flush_pending_notifications(group_key, group, grouped_notifications);
  }

  group.pending_notifications_flush_time = 0;
  group.pending_notifications.clear();
  if (group.notifications.size() >
      keep_notification_group_size_ + EXTRA_GROUP_SIZE) {  // ensure that we delete a lot of messages simultaneously
    // keep only keep_notification_group_size_ last notifications in memory
    group.notifications.erase(
        group.notifications.begin(),
        group.notifications.begin() + (group.notifications.size() - keep_notification_group_size_));
  }

  groups_.emplace(std::move(final_group_key), std::move(group));
}

void NotificationManager::edit_notification(NotificationGroupId group_id, NotificationId notification_id,
                                            unique_ptr<NotificationType> type) {
  if (is_disabled()) {
    return;
  }

  CHECK(notification_id.is_valid());
  CHECK(type != nullptr);
  VLOG(notifications) << "Edit " << notification_id << ": " << *type;

  auto group_it = get_group(group_id);
  auto &group = group_it->second;
  for (size_t i = 0; i < group.notifications.size(); i++) {
    auto &notification = group.notifications[i];
    if (notification.notification_id == notification_id) {
      notification.type = std::move(type);
      if (i + max_notification_group_size_ >= group.notifications.size()) {
        send_update_notification(group_it->first.group_id, group_it->first.dialog_id, notification);
        return;
      }
    }
  }
  for (auto &notification : group.pending_notifications) {
    if (notification.notification_id == notification_id) {
      notification.type = std::move(type);
    }
  }
}

void NotificationManager::remove_notification(NotificationGroupId group_id, NotificationId notification_id,
                                              Promise<Unit> &&promise) {
  if (!notification_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Notification identifier is invalid"));
  }

  if (is_disabled()) {
    return promise.set_value(Unit());
  }

  VLOG(notifications) << "Remove " << notification_id;

  // TODO remove notification from database by notification_id

  // TODO update total_count
  promise.set_value(Unit());
}

void NotificationManager::remove_notification_group(NotificationGroupId group_id, NotificationId max_notification_id,
                                                    Promise<Unit> &&promise) {
  if (!group_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Group identifier is invalid"));
  }
  if (!max_notification_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Notification identifier is invalid"));
  }

  if (is_disabled()) {
    return promise.set_value(Unit());
  }

  VLOG(notifications) << "Remove " << group_id << " up to " << max_notification_id;

  // TODO update total_count
  promise.set_value(Unit());
}

void NotificationManager::on_notification_group_count_max_changed() {
  if (is_disabled()) {
    return;
  }

  auto new_max_notification_group_count =
      G()->shared_config().get_option_integer("notification_group_count_max", DEFAULT_GROUP_COUNT_MAX);
  CHECK(MIN_NOTIFICATION_GROUP_COUNT_MAX <= new_max_notification_group_count &&
        new_max_notification_group_count <= MAX_NOTIFICATION_GROUP_COUNT_MAX);

  if (static_cast<size_t>(new_max_notification_group_count) == max_notification_group_count_) {
    return;
  }

  VLOG(notifications) << "Change max notification group count from " << max_notification_group_count_ << " to "
                      << new_max_notification_group_count;

  if (max_notification_group_count_ != 0) {
    // TODO
  }

  max_notification_group_count_ = static_cast<size_t>(new_max_notification_group_count);
}

void NotificationManager::on_notification_group_size_max_changed() {
  if (is_disabled()) {
    return;
  }

  auto new_max_notification_group_size =
      G()->shared_config().get_option_integer("notification_group_size_max", DEFAULT_GROUP_SIZE_MAX);
  CHECK(MIN_NOTIFICATION_GROUP_SIZE_MAX <= new_max_notification_group_size &&
        new_max_notification_group_size <= MAX_NOTIFICATION_GROUP_SIZE_MAX);

  if (static_cast<size_t>(new_max_notification_group_size) == max_notification_group_size_) {
    return;
  }

  VLOG(notifications) << "Change max notification group size from " << max_notification_group_size_ << " to "
                      << new_max_notification_group_size;

  if (max_notification_group_size_ != 0) {
    // TODO
  }

  max_notification_group_size_ = static_cast<size_t>(new_max_notification_group_size);
  keep_notification_group_size_ =
      max_notification_group_size_ + max(EXTRA_GROUP_SIZE / 2, min(max_notification_group_size_, EXTRA_GROUP_SIZE));
}

void NotificationManager::on_online_cloud_timeout_changed() {
  online_cloud_timeout_ms_ =
      G()->shared_config().get_option_integer("online_cloud_timeout_ms", DEFAULT_ONLINE_CLOUD_TIMEOUT_MS);
  VLOG(notifications) << "Set online_cloud_timeout_ms to " << online_cloud_timeout_ms_;
}

void NotificationManager::on_notification_cloud_delay_changed() {
  notification_cloud_delay_ms_ =
      G()->shared_config().get_option_integer("notification_cloud_delay_ms", DEFAULT_ONLINE_CLOUD_DELAY_MS);
  VLOG(notifications) << "Set notification_cloud_delay_ms to " << notification_cloud_delay_ms_;
}

void NotificationManager::on_notification_default_delay_changed() {
  notification_default_delay_ms_ =
      G()->shared_config().get_option_integer("notification_default_delay_ms", DEFAULT_DEFAULT_DELAY_MS);
  VLOG(notifications) << "Set notification_default_delay_ms to " << notification_default_delay_ms_;
}

}  // namespace td