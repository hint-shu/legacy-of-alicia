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

#ifndef ACHIEVEMENTREGISTRY_HPP
#define ACHIEVEMENTREGISTRY_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace server::registry
{

//! Откуда сервер узнаёт о событии достижения.
enum class AchievementSource : uint8_t
{
  //! Событие приходит серверу его СОБСТВЕННЫМИ командами (финиш заезда,
  //! кормёжка, разведение, уровень). Репорт клиента для него не нужен вовсе.
  Server,
  //! Событие приходит ТОЛЬКО репортом клиента (0x16b): скорость, заносы,
  //! дистанция планирования, скриншоты. Значение — со слов клиента, поэтому
  //! прогресс по нему допустим, а выплата нет.
  Client,
};

//! Как сравнивать измеренное значение с порогами тиров (колонка CompareType).
enum class AchievementCompareType : uint8_t
{
  //! 0 — счётчик: прогресс накапливается, порог берётся «не меньше».
  Counter = 0,
  //! 1 — рекорд, где МЕНЬШЕ значит лучше (время круга): порог «не больше», и
  //! пороги в данных идут по УБЫВАНИЮ.
  AtMost = 1,
  //! 2 — рекорд, где больше значит лучше (максимальная скорость, ступень
  //! рывка): порог «не меньше», но значение не накапливается, а заменяется.
  MaxRecord = 2,
  //! 3 — суммарный итог (потрачено морковок за всё время): «не меньше».
  Total = 3,
};

//! Одна запись каталога достижений. Числа — из клиентского libconfig,
//! признак источника — из таблицы AchvEventPropertyLink, признак известной
//! семантики — из ach_conditions.lua оригинального сервера.
struct AchievementInfo
{
  uint16_t tid{};
  //! Имя из клиентских данных; сервер его не показывает, но с ним читаемы логи.
  std::string name;
  //! Номер книги; -2 = вне книг, 0 = служебная книга.
  int8_t book{};
  //! UserAchvEvent — шина, по которой достижение продвигается.
  uint16_t event{};
  AchievementSource source{AchievementSource::Server};
  //! Имя свойства из AchvEventPropertyLink (только у клиентских событий).
  std::string property;
  //! Имя предиката условия (колонка Function).
  std::string function;
  //! Реализован ли этот предикат в оригинальном ach_conditions.lua.
  bool hasOriginalCondition{};
  std::array<int32_t, 4> functionValues{};
  uint32_t gameModeFlag{};
  uint32_t numPlayer{};
  //! ★Вид измерения И НАПРАВЛЕНИЕ сравнения. Не теория: в каталоге ровно одна
  //! запись с `AtMost` (10233 «Помешанный на шариках», пороги [31,30,29,28]) —
  //! там меряется ВРЕМЯ круга, и меньше значит лучше. Остальные сравниваются
  //! «не меньше порога». Найдено ревью R46 — прежний код считал пороги всегда
  //! возрастающими и выдавал этой записи то ноль тиров, то сразу четыре.
  AchievementCompareType compareType{AchievementCompareType::Counter};
  uint32_t successType{};
  //! Пороги четырёх тиров; ноль = тир не используется.
  std::array<uint32_t, 4> thresholds{};
  //! Выплата за тир. ★У нас обнулена решением владельца — награда идёт
  //! бейджем, очками, званием и косметикой книг.
  std::array<uint32_t, 4> rewards{};
  //! Оригинальные выплаты Ntreev — хранятся, чтобы включение экономики было
  //! правкой данных, а не кода.
  std::array<uint32_t, 4> originalRewards{};
  uint32_t points{};
  uint16_t resetEvent{};
  std::string resetFunction;
  int32_t resetValue{};
  //! Заведомо невыполнимая заглушка: выдача закроет системную книгу навсегда.
  bool neverAward{};

  //! Сколько тиров закрывает накопленный прогресс.
  //! @param progress Накопленный прогресс.
  //! @returns Число взятых тиров, 0..4.
  [[nodiscard]] uint8_t GetReachedTierCount(uint32_t progress) const;
};

//! Серверное зеркало каталога достижений, читается из
//! resources/config/game/achievements.yaml при старте. Дальше только чтение.
class AchievementRegistry final
{
public:
  void ReadConfig(const std::filesystem::path& configPath);

  //! @param tid Достижение.
  //! @returns Запись или nullptr, если такого tid в каталоге нет.
  [[nodiscard]] const AchievementInfo* GetAchievement(uint16_t tid) const;

  //! @param event Шина событий (UserAchvEvent).
  //! @returns Достижения, которые двигаются этим событием.
  [[nodiscard]] const std::vector<const AchievementInfo*>& GetAchievementsByEvent(
    uint16_t event) const;

  //! @returns Сколько записей в каталоге.
  [[nodiscard]] size_t GetAchievementCount() const;

private:
  std::unordered_map<uint16_t, AchievementInfo> _achievements;
  std::unordered_map<uint16_t, std::vector<const AchievementInfo*>> _byEvent;
  //! Пустой список для события, которого никто не слушает: так вызывающая
  //! сторона обходится без проверки на nullptr.
  std::vector<const AchievementInfo*> _empty;
};

} // namespace server::registry

#endif // ACHIEVEMENTREGISTRY_HPP
