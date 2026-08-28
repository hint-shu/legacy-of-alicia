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

#include "libserver/registry/AchievementRegistry.hpp"
#include "libserver/util/QuietLog.hpp"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string_view>

namespace server::registry
{

namespace
{

//! Читает четыре числа списком, недостающие оставляет нулями. Реестр
//! генерируется скриптом, но падать на кривом файле всё равно нельзя: сервер
//! без реестра не стартует вообще.
template <typename T, size_t N>
std::array<T, N> ReadArray(const YAML::Node& node)
{
  std::array<T, N> values{};
  if (not node or not node.IsSequence())
    return values;

  const auto count = std::min(node.size(), N);
  for (size_t index = 0; index < count; ++index)
    values[index] = node[index].as<T>(T{});

  return values;
}

//! Список ровно из N ЧИСЕЛ. ★Проверять `IsScalar` мало (находка ревью R46,
//! iteration 2): строка «oops» тоже скаляр, а `as<T>(fallback)` молча вернул
//! бы за неё ноль — и сервер поднялся бы с тихо неверным каталогом.
template <typename T>
bool IsNumberArrayOf(const YAML::Node& node, const size_t size)
{
  if (not node or not node.IsSequence() or node.size() != size)
    return false;

  for (const auto& element : node)
  {
    if (not element.IsScalar())
      return false;
    T parsed{};
    if (not YAML::convert<T>::decode(element, parsed))
      return false;
  }
  return true;
}

//! Пороги идут подряд с начала: ноль означает «дальше тиров нет». Ненулевое
//! ПОСЛЕ нуля — дыра, из-за которой часть тиров стала бы недостижимой молча.
bool AreThresholdsContiguous(const std::array<uint32_t, 4>& thresholds)
{
  bool ended = false;
  for (const auto threshold : thresholds)
  {
    if (threshold == 0)
      ended = true;
    else if (ended)
      return false;
  }
  return true;
}

} // namespace

uint8_t AchievementInfo::GetReachedTierCount(const uint32_t progress) const
{
  // ★НАПРАВЛЕНИЕ СРАВНЕНИЯ ЗАДАЁТ compareType, а не порядок чисел (находка
  // ревью R46). У `AtMost` меряется время круга: меньше — лучше, и пороги в
  // данных убывают. Считать их «как все» значило бы выдавать этой записи то
  // ноль тиров, то сразу четыре.
  const bool lowerIsBetter = compareType == AchievementCompareType::AtMost;

  // ★Ноль у «меньше — лучше» означает «замера ЕЩЁ НЕ БЫЛО», а не идеальное
  // время (находка ревью R46, iteration 2): без этого игрок, ни разу не
  // катавший круг с шариками, получал бы сразу все четыре тира, потому что
  // 0 <= 28. Ноль во всём остальном каталоге тоже значит «прогресса нет».
  if (lowerIsBetter and progress == 0)
    return 0;

  uint8_t reached = 0;
  for (const auto threshold : thresholds)
  {
    // Ноль = тир не используется. Дальше идти незачем: пороги идут подряд.
    if (threshold == 0)
      break;
    if (lowerIsBetter ? progress > threshold : progress < threshold)
      break;
    ++reached;
  }

  // Запись без единого порога — разовая: закрывается первым же событием,
  // считать ей нечего. Для `AtMost` такой формы в каталоге нет, и трактовать
  // «ноль порогов» как «любое время годится» было бы выдумкой.
  if (reached == 0 and thresholds[0] == 0 and progress > 0
    and not lowerIsBetter)
  {
    reached = 1;
  }

  return reached;
}

void AchievementRegistry::ReadConfig(const std::filesystem::path& configPath)
{
  const auto root = YAML::LoadFile(configPath.string());
  const auto collection = root["achievements"]["collection"];

  // ★СТРОГО, В ОТЛИЧИЕ ОТ ДАННЫХ ИГРОКА (находка ревью R46). Файл персонажа
  // мы читаем снисходительно: кривое значение стоит поля, но не персонажа.
  // Здесь наоборот — это НАШ сгенерированный файл, и кривизна в нём means
  // каталог неверен. Молча проглотить её значило бы получить достижения,
  // которые тихо не работают, — ровно тот класс отказов, который мы уже
  // ловили с подарками. Поэтому: отказ старта с внятным указанием записи.
  if (not collection or not collection.IsSequence() or collection.size() == 0)
  {
    throw std::runtime_error(std::format(
      "Achievement registry '{}' has no 'achievements.collection' sequence",
      configPath.string()));
  }

  const auto require = [&configPath](
    const bool condition, const uint16_t tid, const std::string_view what)
  {
    if (not condition)
    {
      throw std::runtime_error(std::format(
        "Achievement registry '{}': achievement {} {}",
        configPath.string(), tid, what));
    }
  };

  size_t clientReported = 0;
  size_t withOriginalCondition = 0;
  size_t paying = 0;

  for (const auto& node : collection)
  {
    AchievementInfo info;
    require(node.IsMap(), 0, "entry is not a mapping");
    info.tid = node["tid"].as<uint16_t>(0);
    require(info.tid != 0, 0, "has no usable tid");

    info.name = node["name"].as<std::string>("");
    info.book = static_cast<int8_t>(node["book"].as<int32_t>(-2));
    // Источник — не догадка, а факт из таблицы связей, поэтому неизвестное
    // значение здесь означает испорченный файл, а не «наверное серверное».
    const auto source = node["source"].as<std::string>("");
    require(source == "server" or source == "client", info.tid,
      "has an unknown 'source' (expected 'server' or 'client')");
    info.source = source == "client"
      ? AchievementSource::Client
      : AchievementSource::Server;
    info.property = node["property"].as<std::string>("");
    info.function = node["function"].as<std::string>("");
    info.hasOriginalCondition = node["hasOriginalCondition"].as<bool>(false);
    require(IsNumberArrayOf<int32_t>(node["functionValues"], 4), info.tid,
      "has a malformed 'functionValues' (expected four numbers)");
    require(IsNumberArrayOf<uint32_t>(node["thresholds"], 4), info.tid,
      "has a malformed 'thresholds' (expected four numbers)");
    require(IsNumberArrayOf<uint32_t>(node["rewards"], 4), info.tid,
      "has a malformed 'rewards' (expected four numbers)");
    require(IsNumberArrayOf<uint32_t>(node["originalRewards"], 4), info.tid,
      "has a malformed 'originalRewards' (expected four numbers)");
    info.functionValues = ReadArray<int32_t, 4>(node["functionValues"]);
    // Числовые поля читаем ПРОВЕРЯЕМО: `as<T>(fallback)` проглотил бы «nope»
    // и подставил ноль (находка ревью R46, iteration 2).
    const auto readNumber = [&require](
      const YAML::Node& parent, const char* key, const uint16_t tid) -> uint32_t
    {
      uint32_t value{};
      require(parent[key] and parent[key].IsScalar()
          and YAML::convert<uint32_t>::decode(parent[key], value),
        tid, std::format("has a malformed '{}'", key));
      return value;
    };

    info.event = static_cast<uint16_t>(readNumber(node, "event", info.tid));
    info.gameModeFlag = readNumber(node, "gameModeFlag", info.tid);
    info.numPlayer = readNumber(node, "numPlayer", info.tid);
    const auto compareType = readNumber(node, "compareType", info.tid);
    require(compareType <= static_cast<uint32_t>(AchievementCompareType::Total),
      info.tid, "has an unknown 'compareType'");
    info.compareType = static_cast<AchievementCompareType>(compareType);
    info.successType = readNumber(node, "successType", info.tid);
    info.thresholds = ReadArray<uint32_t, 4>(node["thresholds"]);
    require(AreThresholdsContiguous(info.thresholds), info.tid,
      "has a gap in 'thresholds' (a non-zero threshold after a zero)");
    info.rewards = ReadArray<uint32_t, 4>(node["rewards"]);
    info.originalRewards = ReadArray<uint32_t, 4>(node["originalRewards"]);
    info.points = readNumber(node, "points", info.tid);
    info.resetEvent = node["resetEvent"].as<uint16_t>(0);
    info.resetFunction = node["resetFunction"].as<std::string>("");
    info.resetValue = node["resetValue"].as<int32_t>(0);
    info.neverAward = node["neverAward"].as<bool>(false);

    if (info.source == AchievementSource::Client)
      ++clientReported;
    if (info.hasOriginalCondition)
      ++withOriginalCondition;
    if (std::ranges::any_of(info.originalRewards, [](const auto value) { return value > 0; }))
      ++paying;

    const auto tid = info.tid;
    const auto [entry, inserted] = _achievements.emplace(tid, std::move(info));
    // Дубликат tid в СГЕНЕРИРОВАННОМ файле означает, что каталог собран
    // неверно: какая из двух записей правильная, знать неоткуда.
    require(inserted, tid, "is listed twice in the registry");
  }

  for (const auto& [tid, info] : _achievements)
    _byEvent[info.event].push_back(&info);

  server::util::QuietLogInfo(
    "Achievement registry loaded {} achievements on {} events "
    "({} server-measured, {} client-reported, {} with the original condition, "
    "{} paid carrots originally)",
    _achievements.size(),
    _byEvent.size(),
    _achievements.size() - clientReported,
    clientReported,
    withOriginalCondition,
    paying);
}

const AchievementInfo* AchievementRegistry::GetAchievement(const uint16_t tid) const
{
  const auto entry = _achievements.find(tid);
  return entry == _achievements.cend() ? nullptr : &entry->second;
}

const std::vector<const AchievementInfo*>& AchievementRegistry::GetAchievementsByEvent(
  const uint16_t event) const
{
  const auto entry = _byEvent.find(event);
  return entry == _byEvent.cend() ? _empty : entry->second;
}

size_t AchievementRegistry::GetAchievementCount() const
{
  return _achievements.size();
}

} // namespace server::registry
