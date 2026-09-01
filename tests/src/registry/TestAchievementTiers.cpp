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

//! LOA (R70, backlog #58): потолок доступных тиров и фильтр режима/состава.
//!
//! ★ПОЧЕМУ НЕ `assert`. Остальные тесты дерева проверяют `assert`, а образ
//! собирается `-DCMAKE_BUILD_TYPE=Release` — там `NDEBUG` гасит `assert`
//! целиком, и тест, который НЕ УМЕЕТ ПРОВАЛИТЬСЯ, читается как зелёный.
//! Собственный `Check` работает независимо от NDEBUG и печатает, что именно
//! не сошлось.

#include <libserver/registry/AchievementRegistry.hpp>
#include <libserver/util/Util.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace
{

int g_failures = 0;

void Check(const bool condition, const char* const what)
{
  if (condition)
    return;
  std::fprintf(stderr, "FAIL: %s\n", what);
  ++g_failures;
}

using server::registry::AchievementCompareType;
using server::registry::AchievementInfo;

//! Ёмкость массива отметок тиров в записи персонажа — та самая четвёрка,
//! которой R70 перестал мерить потолок.
constexpr uint8_t TierEarnedAtCapacity = 4;

AchievementInfo MakeInfo(
  const std::array<uint32_t, 4>& thresholds,
  const AchievementCompareType compareType = AchievementCompareType::Counter,
  const uint32_t gameModeFlag = 0,
  const uint32_t numPlayer = 0)
{
  AchievementInfo info{};
  info.thresholds = thresholds;
  info.compareType = compareType;
  info.gameModeFlag = gameModeFlag;
  info.numPlayer = numPlayer;
  return info;
}

void TestAvailableTierCount()
{
  Check(MakeInfo({1, 2, 3, 4}).GetAvailableTierCount() == 4, "четыре порога -> 4");
  Check(MakeInfo({1, 2, 0, 0}).GetAvailableTierCount() == 2, "два порога -> 2");
  Check(MakeInfo({5, 0, 0, 0}).GetAvailableTierCount() == 1, "один порог -> 1");

  // Единственная запись AtMost каталога (10233, пороги по убыванию).
  Check(
    MakeInfo({31, 30, 29, 28}, AchievementCompareType::AtMost)
      .GetAvailableTierCount() == 4,
    "AtMost с четырьмя порогами -> 4");

  // ★СЕРДЦЕ ФИКСА R70: у 37 из 62 безсбросных записей события 2 пороги нулевые.
  // Запись разовая — доступный тир ровно один, и он МЕНЬШЕ ёмкости массива
  // отметок. Прежний потолок `tiersBefore >= tierEarnedAt.size()` не срабатывал
  // на них никогда: прогресс рос бесконечно, и клиенту уходил 0xe4 на КАЖДОМ
  // событии.
  const auto zeroThresholds = MakeInfo({0, 0, 0, 0});
  Check(zeroThresholds.GetAvailableTierCount() == 1, "без порогов Counter -> 1");
  Check(
    zeroThresholds.GetAvailableTierCount() < TierEarnedAtCapacity,
    "без порогов доступных тиров МЕНЬШЕ ёмкости массива отметок");
  Check(
    MakeInfo({0, 0, 0, 0}, AchievementCompareType::MaxRecord)
      .GetAvailableTierCount() == 1,
    "без порогов MaxRecord -> 1");
  Check(
    MakeInfo({0, 0, 0, 0}, AchievementCompareType::Total)
      .GetAvailableTierCount() == 1,
    "без порогов Total -> 1");

  // У AtMost без порогов тира не будет никогда: GetReachedTierCount отдаёт ноль
  // и на нулевом прогрессе, и на любом другом (ветка «reached == 0 -> 1» явно
  // исключает lowerIsBetter). «Доступный» тир был бы выдумкой.
  Check(
    MakeInfo({0, 0, 0, 0}, AchievementCompareType::AtMost)
      .GetAvailableTierCount() == 0,
    "без порогов AtMost -> 0");
}

//! Инвариант, ради которого величина и заведена: взятых тиров НИКОГДА не больше
//! доступных. Проверяется перебором по всем формам каталога и по сетке прогресса.
void TestReachedNeverExceedsAvailable()
{
  const std::array<AchievementInfo, 7> infos{
    MakeInfo({1, 2, 3, 4}),
    MakeInfo({1, 2, 0, 0}),
    MakeInfo({5, 0, 0, 0}),
    MakeInfo({0, 0, 0, 0}),
    MakeInfo({0, 0, 0, 0}, AchievementCompareType::AtMost),
    MakeInfo({31, 30, 29, 28}, AchievementCompareType::AtMost),
    MakeInfo({0, 0, 0, 0}, AchievementCompareType::MaxRecord)};

  for (const auto& info : infos)
  {
    const auto available = info.GetAvailableTierCount();
    for (uint32_t progress = 0; progress <= 64; ++progress)
    {
      Check(
        info.GetReachedTierCount(progress) <= available,
        "GetReachedTierCount(p) <= GetAvailableTierCount()");
    }
  }
}

void TestCountsInMode()
{
  // Маска ноль = без ограничения по режиму; состав ноль = без ограничения.
  const auto unrestricted = MakeInfo({1, 0, 0, 0});
  Check(unrestricted.CountsInMode(1, 1), "маска 0 засчитывает speed-solo");
  Check(unrestricted.CountsInMode(0, 1), "маска 0 засчитывает и режим вне четвёрки");

  // Пересечение масок — как у квестов.
  const auto masteryLike = MakeInfo({0, 0, 0, 0}, AchievementCompareType::Counter, 15, 0);
  Check(masteryLike.CountsInMode(1, 1), "15 & 1 -> засчитано");
  Check(masteryLike.CountsInMode(8, 2), "15 & 8 -> засчитано");

  // ★I10: режим вне четвёрки (обучение) не засчитывает НИЧЕГО с ненулевой
  // маской. Единственное, что отделяет обучающий заезд по ri_land01 от
  // бесплатного мастерства, — это 15 & 0 == 0. Негатив negA снимает
  // именно этот оператор.
  Check(not masteryLike.CountsInMode(0, 8), "15 & 0 -> НЕ засчитано (Tutorial)");

  const auto speedOnly = MakeInfo({0, 0, 0, 0}, AchievementCompareType::Counter, 3, 0);
  Check(speedOnly.CountsInMode(1, 2), "3 & 1 -> засчитано");
  Check(speedOnly.CountsInMode(2, 2), "3 & 2 -> засчитано");
  Check(not speedOnly.CountsInMode(4, 2), "3 & 4 -> НЕ засчитано (magic-solo)");

  const auto wideMask = MakeInfo({0, 0, 0, 0}, AchievementCompareType::Counter, 127, 0);
  Check(wideMask.CountsInMode(1, 1), "127 & 1 -> засчитано, спецслучай Any не нужен");

  // Категорийные биты 16/32/64 не кладутся отображением режима вовсе — такие
  // записи не сматчатся никогда, и это правильный результат.
  const auto categoryOnly = MakeInfo({0, 0, 0, 0}, AchievementCompareType::Counter, 32, 0);
  for (uint32_t bit : {0u, 1u, 2u, 4u, 8u})
    Check(not categoryOnly.CountsInMode(bit, 8), "категорийный бит не сматчится");

  // `numPlayer` — МИНИМУМ (выбор со строгой стороны, §2.2в спеки).
  const auto winLike = MakeInfo({0, 0, 0, 0}, AchievementCompareType::Counter, 1, 4);
  Check(not winLike.CountsInMode(1, 1), "numPlayer 4: один человек -> нет");
  Check(not winLike.CountsInMode(1, 3), "numPlayer 4: трое -> нет");
  Check(winLike.CountsInMode(1, 4), "numPlayer 4: четверо -> да");
  Check(winLike.CountsInMode(1, 8), "numPlayer 4: восемь -> да (минимум, не равенство)");

  // Оба оператора независимы: негатив negA снимает первый, negB — второй.
  const auto both = MakeInfo({0, 0, 0, 0}, AchievementCompareType::Counter, 4, 4);
  Check(not both.CountsInMode(1, 8), "режим не подошёл -> нет, хотя состав подошёл");
  Check(not both.CountsInMode(4, 2), "состав не подошёл -> нет, хотя режим подошёл");
  Check(both.CountsInMode(4, 4), "оба подошли -> да");
}

//! Игровой час = UTC + 3 (Europe/Moscow, постоянный сдвиг с 2014 года).
//! Негатив negF обнуляет смещение — тогда равенство ниже станет ложным.
void TestGameLocalHour()
{
  // Час может смениться между двумя замерами — берём три попытки и требуем,
  // чтобы сошлась хотя бы одна: иначе тест был бы флаки раз в час.
  bool matched = false;
  for (int attempt = 0; attempt < 3 and not matched; ++attempt)
  {
    const auto now = std::chrono::system_clock::now();
    const auto utcHour = static_cast<uint32_t>(
      std::chrono::hh_mm_ss{now - std::chrono::floor<std::chrono::days>(now)}
        .hours()
        .count());
    matched = server::util::CurrentGameLocalHour() == (utcHour + 3) % 24;
  }
  Check(matched, "CurrentGameLocalHour() == (UTC-час + 3) mod 24");
}

//! ★ГЕЙТ СНАЧАЛА ДОКАЗЫВАЕТ СЕБЯ: проверка, которая не умеет провалиться,
//! проверкой не является. Здесь заведомо ложное утверждение обязано быть
//! посчитано провалом — и после этого счётчик возвращается назад.
void TestCheckerCanFail()
{
  const int before = g_failures;
  Check(false, "(self-test) заведомо ложное утверждение");
  if (g_failures != before + 1)
  {
    std::fprintf(stderr, "FATAL: Check() не умеет провалиться\n");
    std::exit(2);
  }
  g_failures = before;
}

} // namespace

int main()
{
  TestCheckerCanFail();
  TestAvailableTierCount();
  TestReachedNeverExceedsAvailable();
  TestCountsInMode();
  TestGameLocalHour();

  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
