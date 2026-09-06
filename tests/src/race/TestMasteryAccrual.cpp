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

//! LOA-fix (R81, backlog #273): арифметика мастерства лошади.
//!
//! ★ЗАЧЕМ ЮНИТ, ЕСЛИ ЕСТЬ ТРИ АРКИ СТЕНДА. Арки доказывают, что величина
//! ДОЕХАЛА до файла коня и что каждый потолок УМЕЕТ сработать. Они не
//! доказывают ГРАНИЦ (`== потолок` против `потолок + 1`), не доказывают
//! насыщения у 0xFFFFFFFF (столько единиц не набить ни одним заездом) и не
//! доказывают, что долевой бюджет не переполняется на честной длине круга.
//! Всё это здесь, и всё это про ВЕЧНЫЕ монотонные поля: ошибку в них снимают
//! правкой файлов при остановленном сервере.
//!
//! ★ПОЧЕМУ НЕ `assert`: образ Release, `NDEBUG` гасит `assert` целиком.

#include "server/race/MasteryAccrual.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>

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

namespace tracker = server::tracker;
using server::race::AccumulateSaturating;
using server::race::IsPlausibleJumpDelta;
using server::race::IsPlausibleSlideDelta;
using server::race::IsPlausibleSpurMagicDelta;
using server::race::SlideBudgetMillis;
using server::race::SlidingMillisToSeconds;

//! ★ГЕЙТ ОБЯЗАН СПЕРВА ДОКАЗАТЬ СЕБЯ.
void TestCheckerCanFail()
{
  Check(false, "(ожидаемо) самопроверка счётчика провалов");
  if (g_failures == 1)
  {
    g_failures = 0;
    return;
  }
  std::fprintf(stderr, "FAIL: Check не считает провалы — тесту верить нельзя\n");
  g_failures = 1000;
}

//! ★НАКОПЛЕНИЕ, А НЕ ПЕРЕЗАПИСЬ: четыре заезда обязаны сложиться.
void TestAccumulationIsAdditive()
{
  uint32_t stored = 0;
  const uint32_t perRace[] = {11, 9, 13, 10};
  uint32_t expected = 0;
  for (const auto delta : perRace)
  {
    stored = AccumulateSaturating(stored, delta);
    expected += delta;
  }
  Check(stored == expected,
    "★мастерство обязано НАКАПЛИВАТЬСЯ, а не заменяться последним заездом");
  Check(stored == 43, "сумма четырёх заездов обязана быть 43 (замер T76)");
}

//! ★НАСЫЩЕНИЕ, А НЕ ОБОРОТ: полная полоса не имеет права стать пустой.
void TestAccumulationSaturates()
{
  constexpr uint32_t max = std::numeric_limits<uint32_t>::max();
  Check(AccumulateSaturating(max, 1) == max, "насыщение у потолка uint32");
  Check(AccumulateSaturating(max - 1, 5) == max, "насыщение при переходе через потолок");
  Check(AccumulateSaturating(max, 0) == max, "нулевое приращение ничего не портит");
  Check(AccumulateSaturating(0, 0) == 0, "ноль плюс ноль — ноль");
}

//! ★ГРАНИЦА ПОТОЛКА «РЫВКИ + ПРЕДМЕТЫ»: `<=`, то есть ровно 64 ещё честно.
void TestSpurMagicCeiling()
{
  Check(IsPlausibleSpurMagicDelta(0), "ноль правдоподобен");
  Check(IsPlausibleSpurMagicDelta(tracker::MaxPlausibleSpurMagicPerRace),
    "★ровно потолок обязан остаться правдоподобным (потолок не ловит честного)");
  Check(not IsPlausibleSpurMagicDelta(tracker::MaxPlausibleSpurMagicPerRace + 1),
    "★потолок + 1 обязан быть отброшен");
  Check(IsPlausibleSpurMagicDelta(41),
    "честный максимум магического заезда (41) обязан проходить");
  Check(IsPlausibleSpurMagicDelta(63),
    "честный максимум скоростного заезда (63) обязан проходить");
}

//! ★ГРАНИЦА ПОТОЛКА ПРЫЖКОВ.
void TestJumpCeiling()
{
  Check(IsPlausibleJumpDelta(24), "наблюдённые ~24 прыжка за заезд правдоподобны");
  Check(IsPlausibleJumpDelta(tracker::MaxPlausibleJumpsPerRace),
    "★ровно потолок обязан остаться правдоподобным");
  Check(not IsPlausibleJumpDelta(tracker::MaxPlausibleJumpsPerRace + 1),
    "★потолок + 1 обязан быть отброшен");
  Check(not IsPlausibleJumpDelta(200),
    "200 прыжков за заезд (арка mastery-flood) обязаны быть отброшены");
}

//! ★ДОЛЕВОЙ ПОТОЛОК СКОЛЬЖЕНИЯ — ОТ ВРЕМЕНИ ЗАЕЗДА, А НЕ КОНСТАНТА.
void TestSlideShareCeiling()
{
  // 150-секундный заезд: бюджет 37.5 с; наблюдение T76 — ~4 с.
  Check(SlideBudgetMillis(150000) == 37500, "бюджет 25 % от 150 с = 37500 мс");
  Check(IsPlausibleSlideDelta(4000, 150000),
    "наблюдённые ~4 с заноса на 150-секундном заезде обязаны проходить");
  Check(IsPlausibleSlideDelta(37500, 150000), "★ровно бюджет ещё правдоподобен");
  Check(not IsPlausibleSlideDelta(37501, 150000), "★бюджет + 1 мс уже нет");
  // Арка mastery-flood: 15 с закрытого скольжения в заезде ~40 с (бюджет 10 с).
  Check(not IsPlausibleSlideDelta(15000, 40000),
    "★15 с скольжения в 40-секундном заезде обязаны быть отброшены");
  Check(IsPlausibleSlideDelta(0, 0),
    "ноль скольжения правдоподобен даже при нулевом времени");
  Check(not IsPlausibleSlideDelta(1, 0),
    "любое скольжение при нулевом времени заезда неправдоподобно");
}

//! ★64 БИТА В БЮДЖЕТЕ — НЕ УКРАШЕНИЕ. `courseTime` в миллисекундах × 25 в
//! 32 битах переполняется на 172-секундном заезде, то есть НА ЧЕСТНОЙ ДЛИНЕ
//! КРУГА: бюджет схлопнулся бы, и потолок отбрасывал бы честное скольжение.
void TestSlideBudgetDoesNotOverflow()
{
  Check(SlideBudgetMillis(300000) == 75000, "300-секундный заезд: бюджет 75 с");
  Check(SlideBudgetMillis(200000) == 50000,
    "★200-секундный заезд: бюджет 50 с (в 32 битах 200000*25 переполнилось бы)");
  const uint32_t maxTime = std::numeric_limits<uint32_t>::max();
  Check(SlideBudgetMillis(maxTime) == static_cast<uint64_t>(maxTime) * 25 / 100,
    "бюджет обязан считаться в 64 битах и на максимальном времени");
  Check(IsPlausibleSlideDelta(45000, 200000),
    "45 с заноса в 200-секундном заезде обязаны проходить бюджет 50 с");
}

//! ★ЕДИНИЦЫ: миллисекунды -> СЕКУНДЫ, усечением ВНИЗ.
void TestMillisToSecondsTruncates()
{
  Check(SlidingMillisToSeconds(0) == 0, "ноль остаётся нулём");
  Check(SlidingMillisToSeconds(999) == 0,
    "★неполная секунда даёт НОЛЬ, а не единицу: округление вверх завышало бы вечное поле");
  Check(SlidingMillisToSeconds(1000) == 1, "ровно секунда — одна единица");
  Check(SlidingMillisToSeconds(1999) == 1, "усечение вниз");
  Check(SlidingMillisToSeconds(4321) == 4,
    "наблюдение T76 (~4 единицы за заезд) воспроизводится в СЕКУНДАХ");
}

//! ★ОТБРАСЫВАЕМ, А НЕ КЛАМПИМ. Свойство формулируется целиком: неправдоподобное
//! приращение обязано дать РОВНО НОЛЬ, а не потолок. Кламп позволил бы
//! модклиенту приколотить вечное поле ровно к потолку каждым заездом.
void TestImplausibleYieldsZeroNotCeiling()
{
  const auto deltaOf = [](const bool plausible, const uint32_t value) -> uint32_t
  {
    return plausible ? value : 0u;
  };
  const uint32_t flooded = 200;
  Check(deltaOf(IsPlausibleJumpDelta(flooded), flooded) == 0,
    "★неправдоподобные 200 прыжков обязаны дать 0, а не 150");
  const uint32_t floodedSpurs = tracker::MaxPlausibleSpurMagicPerRace + 100;
  Check(deltaOf(IsPlausibleSpurMagicDelta(floodedSpurs), floodedSpurs) == 0,
    "★неправдоподобные рывки/предметы обязаны дать 0, а не 64");
}

} // namespace

int main()
{
  TestCheckerCanFail();
  TestAccumulationIsAdditive();
  TestAccumulationSaturates();
  TestSpurMagicCeiling();
  TestJumpCeiling();
  TestSlideShareCeiling();
  TestSlideBudgetDoesNotOverflow();
  TestMillisToSecondsTruncates();
  TestImplausibleYieldsZeroNotCeiling();

  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all mastery accrual checks passed\n");
  return 0;
}
