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

#include "server/ranch/AchievementNotifyHold.hpp"

#include <algorithm>

namespace server
{

std::size_t AchievementNotifyHold::Push(
  const data::Uid characterUid,
  const Notify& notify,
  const Clock::time_point now)
{
  auto& queue = _held[characterUid];
  queue.push_back(Entry{.notify = notify, .queuedAt = now});

  // ★ПОТОЛОК ПРОВЕРЯЕТСЯ ЦИКЛОМ, А НЕ ОДНИМ pop: он обязан держать инвариант
  // и тогда, когда потолок в будущем уменьшат, а очередь уже длиннее.
  std::size_t dropped = 0;
  while (queue.size() > CharacterCap)
  {
    // LOA-fix (R70-fix-8, backlog #58, находка Codex 6 WARN-3):
    // ★ВЫТЕСНЯЕМ САМЫЙ СТАРЫЙ ПРОГРЕССНЫЙ КАДР, А НЕ ПРОСТО САМЫЙ СТАРЫЙ.
    // Кадры в очереди НЕРАВНОЦЕННЫ: `isCompleted = true` — это взятый тир,
    // единственный экран, который игрок и считает наградой; `isCompleted =
    // false` — это «счётчик подрос», и его же значение придёт со следующим
    // кадром того же достижения. Слепое `pop_front` при переполнении меняло
    // взятую бронзу на «+1 к счётчику», то есть теряло РОВНО ТО, ради чего
    // очередь существует.
    // ★ЕСЛИ ПРОГРЕССНЫХ НЕТ ВОВСЕ — вытесняем самое старое завершение: потолок
    // обязан держаться при любом составе, иначе это не потолок.
    const auto victim = std::ranges::find_if(
      queue,
      [](const Entry& entry) { return not entry.notify.objectiveProgress.isCompleted; });
    queue.erase(victim != queue.end() ? victim : queue.begin());
    ++dropped;
  }
  return dropped;
}

std::size_t AchievementNotifyHold::Expire(const Clock::time_point now)
{
  std::size_t dropped = 0;
  for (auto iter = _held.begin(); iter != _held.end();)
  {
    auto& queue = iter->second;
    // ★ОЧЕРЕДЬ УПОРЯДОЧЕНА ПО ВРЕМЕНИ ПО ПОСТРОЕНИЮ (кладём только в хвост, и
    // только текущим временем), поэтому протухшее всегда лежит спереди —
    // отсюда `while` по голове, а не проход по всей очереди.
    while (not queue.empty() and (now - queue.front().queuedAt) >= _ttl)
    {
      queue.pop_front();
      ++dropped;
    }
    // Пустая очередь СТИРАЕТСЯ, а не остаётся пустым ведром: иначе карта росла
    // бы по числу когда-либо игравших персонажей — та же утечка, только
    // медленнее и незаметнее.
    if (queue.empty())
      iter = _held.erase(iter);
    else
      ++iter;
  }
  return dropped;
}

std::vector<data::Uid> AchievementNotifyHold::Characters() const
{
  std::vector<data::Uid> characters;
  characters.reserve(_held.size());
  for (const auto& [characterUid, queue] : _held)
    characters.push_back(characterUid);
  return characters;
}

std::vector<AchievementNotifyHold::Notify> AchievementNotifyHold::Take(
  const data::Uid characterUid)
{
  const auto iter = _held.find(characterUid);
  if (iter == _held.end())
    return {};

  std::vector<Notify> taken;
  taken.reserve(iter->second.size());
  for (auto& entry : iter->second)
    taken.push_back(std::move(entry.notify));
  _held.erase(iter);
  return taken;
}

std::size_t AchievementNotifyHold::HeldCount() const noexcept
{
  std::size_t count = 0;
  for (const auto& [characterUid, queue] : _held)
    count += queue.size();
  return count;
}

} // namespace server
