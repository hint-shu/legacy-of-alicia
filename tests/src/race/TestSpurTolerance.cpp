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

//! LOA-fix (R81, backlog #270): решение о нехватке звёздной шкалы.
//!
//! ★ЧТО ЗДЕСЬ ПРОВЕРЯЕТСЯ, А ЧТО НЕТ. Стенд доказывает ЖИВОЙ кадр: рывок
//! засчитан, ответы ушли, строки `[error]` нет. Он НЕ доказывает границы —
//! до 50-го рывка и до 8-го прощения арка не доходит, а «≈200 из 200» на
//! `spur-flood` не различает `<` и `<=`. Границы стоят здесь.
//!
//! ★ПОЧЕМУ НЕ `assert`: образ собирается Release, `NDEBUG` гасит `assert`
//! целиком — тест, который не умеет провалиться, читается как зелёный.

#include "server/race/SpurTolerance.hpp"

#include <cstdio>

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

using server::race::SpurShortfallDecision;
namespace tracker = server::tracker;

//! ★ГЕЙТ ОБЯЗАН СПЕРВА ДОКАЗАТЬ СЕБЯ: если `Check` не умеет считать провал,
//! все остальные проверки зелены по построению.
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

//! Уровень 3: заезд НЕ идёт — отказ при любых счётчиках.
void TestRefusedOutsideRace()
{
  Check(not SpurShortfallDecision(false, 0, 0),
    "вне заезда нехватка не прощается даже на нулевых счётчиках");
  Check(not SpurShortfallDecision(false, 1, 1),
    "вне заезда нехватка не прощается и на прогретых счётчиках");
}

//! Уровень 2: заезд идёт, счётчики в пределах — рывок засчитывается.
void TestForgivenInsideBudget()
{
  Check(SpurShortfallDecision(true, 0, 0),
    "первая нехватка за заезд обязана прощаться");
  Check(SpurShortfallDecision(true, 10, 3),
    "нехватка в середине бюджета обязана прощаться");
}

//! ★ГРАНИЦА ПОТОЛКА РЫВКОВ. `<`, а не `<=`: 50-й рывок (индекс 49) ещё
//! прощается, 51-й (индекс 50) — уже нет. Стенд этой точки не достигает.
void TestSpurCeilingBoundary()
{
  Check(SpurShortfallDecision(true, tracker::MaxPlausibleSpursPerRace - 1, 0),
    "на предпоследнем рывке бюджета нехватка ещё прощается");
  Check(not SpurShortfallDecision(true, tracker::MaxPlausibleSpursPerRace, 0),
    "★на потолке рывков за заезд нехватка уже НЕ прощается");
  Check(not SpurShortfallDecision(true, tracker::MaxPlausibleSpursPerRace + 1, 0),
    "за потолком — тем более");
}

//! ★ГРАНИЦА БЮДЖЕТА ПРОЩЕНИЙ, тот же довод.
void TestGraceBoundary()
{
  Check(SpurShortfallDecision(true, 0, tracker::SpurShortfallGrace - 1),
    "последнее прощение бюджета обязано состояться");
  Check(not SpurShortfallDecision(true, 0, tracker::SpurShortfallGrace),
    "★исчерпанный бюджет прощений обязан давать отказ");
}

//! ★БЮДЖЕТ НЕ ВОССТАНАВЛИВАЕТСЯ ВНУТРИ ЗАЕЗДА. Проверяется как свойство
//! монотонности: решение, ставшее отказом при N прощениях, обязано оставаться
//! отказом при любом N' >= N, сколько бы рывков ни прошло между ними.
void TestGraceDoesNotRecover()
{
  for (uint32_t forgiven = tracker::SpurShortfallGrace;
       forgiven < tracker::SpurShortfallGrace + 5; ++forgiven)
  {
    for (uint32_t spurs = 0; spurs < 5; ++spurs)
    {
      Check(not SpurShortfallDecision(true, spurs, forgiven),
        "★исчерпанный бюджет прощений не восстанавливается новыми рывками");
    }
  }
}

//! ★ТРИ УСЛОВИЯ НЕЗАВИСИМЫ: снятие любого одного из трёх обязано менять
//! вердикт. Иначе конъюнкция была бы декорацией.
void TestAllThreeConditionsMatter()
{
  Check(SpurShortfallDecision(true, 0, 0), "опорная точка обязана быть true");
  Check(not SpurShortfallDecision(false, 0, 0), "снятие «заезд идёт» меняет вердикт");
  Check(not SpurShortfallDecision(true, tracker::MaxPlausibleSpursPerRace, 0),
    "снятие «рывков в пределах» меняет вердикт");
  Check(not SpurShortfallDecision(true, 0, tracker::SpurShortfallGrace),
    "снятие «прощений в пределах» меняет вердикт");
}

} // namespace

int main()
{
  TestCheckerCanFail();
  TestRefusedOutsideRace();
  TestForgivenInsideBudget();
  TestSpurCeilingBoundary();
  TestGraceBoundary();
  TestGraceDoesNotRecover();
  TestAllThreeConditionsMatter();

  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all spur tolerance checks passed\n");
  return 0;
}
