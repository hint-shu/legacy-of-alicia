/**
 * Alicia Server - dedicated server software
 * Copyright (C) 2024 Story Of Alicia
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 **/

#include "server/system/AchievementSystem.hpp"
#include "libserver/util/QuietLog.hpp"

#include "libserver/registry/AchievementRegistry.hpp"
#include "server/ServerInstance.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace server
{

namespace
{

//! Условия, семантику которых система умеет исполнять СЕГОДНЯ.
//!
//! ★ПРАВИЛО: неизвестное условие НЕ СРАБАТЫВАЕТ НИКОГДА. Достижение, выданное
//! по ошибке, назад не забирается — поэтому список расширяется по одному
//! условию за раунд, с проверкой, а не оптимистично. "TRUE" в каталоге
//! означает «просто считай событие»: само событие и есть условие.
bool IsPlainCounter(const std::string& function)
{
  return function == "TRUE" or function == "True" or function == "true";
}

} // namespace

AchievementSystem::AchievementSystem(ServerInstance& serverInstance)
  : _serverInstance(serverInstance)
{
}

std::vector<protocol::AcCmdRCAchievementUpdateNotify> AchievementSystem::OnServerEvent(
  const data::Uid characterUid,
  const uint16_t event,
  const uint32_t increment,
  const std::span<const std::string_view> provenConditions,
  const std::optional<EventContext> context)
{
  std::vector<protocol::AcCmdRCAchievementUpdateNotify> notifies;
  if (increment == 0)
    return notifies;

  const auto& achievementRegistry = _serverInstance.GetAchievementRegistry();
  const auto& matching = achievementRegistry.GetAchievementsByEvent(event);
  if (matching.empty())
    return notifies;

  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    characterUid);
  if (not characterRecord.IsAvailable())
    return notifies;

  characterRecord.Mutable(
    [&notifies, &matching, &provenConditions, &context, characterUid, increment](
      data::Character& character)
    {
      for (const auto* const info : matching)
      {
        // Заведомо невыполнимая заглушка: выдача закрыла бы системную книгу
        // навсегда.
        if (info->neverAward)
          continue;

        // Событие, о котором сервер узнаёт только со слов клиента, не может
        // прийти сюда легально: сюда зовёт серверный код. Если такое
        // достижение оказалось на этой шине — это ошибка проводки, и молча
        // двигать его нельзя.
        if (info->source != registry::AchievementSource::Server)
          continue;

        // ★СБРОС ПРОГРЕССА НЕ РЕАЛИЗОВАН — значит запись не двигаем ВООБЩЕ.
        // 29 достижений каталога меряют не «сколько всего», а «сколько подряд»
        // или «сколько за один визит»: «Одержимость любовью» (10184) считает
        // разведения за ОДИН заход в центр, серии побед — победы без поражения
        // между ними. Считать их пожизненным счётчиком значит выдать платину
        // тому, кто условия не выполнял: пять разведений за месяц не равны пяти
        // подряд, а тир назад не забирается.
        // ★Сброс записан ДВУМЯ способами, и проверять надо ОБА: у 12 записей
        // задано событие сброса (`resetEvent`), а у 17 — только его условие
        // (`resetFunction`, например «Win ... сбрасывается на NotWin») при
        // нулевом событии. Проверка одного лишь события пропустила бы вторую
        // половину класса.
        if (info->resetEvent != 0 or not info->resetFunction.empty())
          continue;
        // ★ФИЛЬТР РЕЖИМА И СОСТАВА (R70). Поля `gameModeFlag`/`numPlayer` в
        // каталоге были всегда, но не проверялись НИГДЕ — и до сих пор это было
        // безвредно, потому что у всех 20 записей живых шин оба нуля.
        // На событии 2 ненулевой `gameModeFlag` у подавляющего большинства
        // записей: без фильтра «победа в магическом соло» (10226) засчиталась бы
        // после скоростного заезда, а мастерство — после обучающего.
        // Контекста нет (все ранчевые вызовы) — фильтр не применяется.
        if (context.has_value()
          and not info->CountsInMode(context->modeBit, context->playerCount))
          continue;

        // ★УСЛОВИЕ ИСПОЛНЯЕТ ТОТ, У КОГО ЕСТЬ ДАННЫЕ. Система умеет сама только
        // «просто считай событие» (Function = TRUE). Всё прочее требует данных
        // с места события — сытость лошади, класс жеребёнка, вес, — и потому
        // проверяется там, а сюда приходит уже доказанным по имени.
        if (not IsPlainCounter(info->function)
          and std::ranges::find(provenConditions, info->function)
            == provenConditions.end())
          continue;

        auto& achievements = character.achievements();
        auto entry = std::ranges::find(
          achievements, info->tid, &data::Character::AchievementEntry::tid);
        if (entry == achievements.end())
        {
          achievements.push_back({.tid = info->tid});
          entry = achievements.end() - 1;
        }

        const auto tiersBefore = info->GetReachedTierCount(entry->progress);
        // Все ДОСТУПНЫЕ тиры уже взяты — дальше считать нечего, и лишний пакет
        // клиенту не нужен. Мерить ёмкостью массива отметок (всегда 4) нельзя:
        // у записи без порогов доступный тир ровно один, и сравнение с
        // четвёркой не срабатывало бы никогда → бесконечный рост прогресса и
        // 0xe4 на КАЖДОМ событии. См. AchievementInfo::GetAvailableTierCount.
        if (tiersBefore >= info->GetAvailableTierCount())
          continue;

        entry->progress += increment;
        const auto tiersAfter = info->GetReachedTierCount(entry->progress);

        const auto now = data::Clock::now();
        for (auto tier = tiersBefore; tier < tiersAfter; ++tier)
        {
          entry->tierEarnedAt[tier] = now;
          server::util::QuietLogInfo(
            "Character '{}' reached tier {} of achievement {} ('{}')",
            characterUid,
            tier + 1,
            info->tid,
            info->name);
        }

        const bool reachedTier = tiersAfter > tiersBefore;

        protocol::AcCmdRCAchievementUpdateNotify notify{};
        notify.achievementTid = info->tid;
        notify.objectiveProgress.isCompleted = reachedTier;
        notify.objectiveProgress.progress = entry->progress;
        // Тир нумеруется с нуля: бронза = 0. Пока ни один не взят, клиенту
        // уходит None.
        notify.objectiveProgress.achievementTier = reachedTier
          ? static_cast<protocol::ObjectiveProgress::AchievementTier>(tiersAfter - 1)
          : protocol::ObjectiveProgress::AchievementTier::None;
        // Баланс АБСОЛЮТНЫЙ: клиент ставит его как есть. Достижения в этом
        // раунде не платят, поэтому отдаём текущий — иначе клиент нарисовал бы
        // ноль морковок.
        notify.carrotBalance = character.carrots();

        notifies.push_back(notify);
      }
    });

  return notifies;
}

} // namespace server
