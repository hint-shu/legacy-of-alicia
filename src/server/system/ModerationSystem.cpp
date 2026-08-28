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

#include "server/system/ModerationSystem.hpp"

#include <libserver/util/QuietLog.hpp>

#include <yaml-cpp/yaml.h>

namespace server
{

void ModerationSystem::ReadConfig(
  const std::filesystem::path& configPath)
{
  const auto root = YAML::LoadFile(configPath.string());

  const auto wordsSection = root["words"];
  if (not wordsSection)
    throw std::runtime_error("Missing words section");

  const auto wordsCollectionSection = wordsSection["collection"];
  if (not wordsCollectionSection)
    throw std::runtime_error("Missing words collection section");

  for (const auto& wordSection : wordsCollectionSection)
  {
    const auto word = wordSection["word"].as<std::string>();
    const bool isPrevented = wordSection["prevent"].as<bool>(false);

    _words.emplace_back(
      Word{
        .isPrevented = isPrevented,
        .regex = std::regex(word, std::regex_constants::ECMAScript | std::regex_constants::icase)});
  }
}

ModerationSystem::Verdict ModerationSystem::Moderate(
  const std::string& input) const noexcept
{
  Verdict verdict;

  for (const auto& word : _words)
  {
    // LOA-fix (R55-12, round55, backlog #179 часть 5): пояс ВНУТРИ цикла, а не
    // вокруг него — отказ на одном правиле не имеет права отменять проверку по
    // остальным.
    bool matches = false;
    try
    {
      matches = std::regex_search(input, word.regex);
    }
    catch (...)
    {
      // ★ЗАКРЫВАЕМСЯ, А НЕ ОТКРЫВАЕМСЯ. Исход проверки неизвестен, и выбор
      // между «пропустить» и «заблокировать» тут НЕ симметричен:
      //   * пропустить = обход модерации, то есть чужая проблема (мат в общем
      //     канале видят все);
      //   * заблокировать = пострадало РОВНО ОДНО сообщение РОВНО ТОГО, кто его
      //     прислал. Атакующий, подобравший ввод против движка регулярок,
      //     блокирует сам себя — коллатерали нет.
      // Поэтому отказ трактуем как «запрещено».
      //
      // Уровень DEBUG, а не ERROR, сознательно: ввод задаёт игрок, и запись
      // уровня ошибки на каждое такое сообщение была бы управляемым извне
      // заливом лога.
      util::QuietLogDebug(
        "Moderation rule check failed on player input; treating the message as prevented");
      verdict.isPrevented = true;
      break;
    }

    // Check if any part of the input matches the word.
    if (!matches)
      continue;

    // Check if the word is prevented or just censored.
    if (!word.isPrevented)
      continue;

    // todo: censor

    verdict.isPrevented = true;
    break;
  }

  return verdict;
}

} // namespace server

