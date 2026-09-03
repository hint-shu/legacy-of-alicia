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

//! LOA (R70-fix-7, backlog #58): удержание попапов достижений заезда.
//!
//! ★ЗАЧЕМ ЮНИТ-ТЕСТ, А НЕ ТОЛЬКО СТЕНД. Политика удержания — это срок,
//! потолок и порядок вытеснения. Срок по умолчанию 15 минут: проверить его
//! стендом значит ждать пятнадцать минут в клетке, то есть НЕ проверять.
//! Здесь время ПОДАЁТСЯ, и весь срок проходит за микросекунды.
//!
//! ★ПОЧЕМУ НЕ `assert`: образ собирается Release, `NDEBUG` гасит `assert`
//! целиком — тест, который не умеет провалиться, читается как зелёный.
//! Тот же собственный `Check`, что и в TestAchievementTiers.cpp.

#include "server/ranch/AchievementNotifyHold.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

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

using server::AchievementNotifyHold;
using Clock = AchievementNotifyHold::Clock;

AchievementNotifyHold::Notify MakeNotify(const uint16_t tid)
{
  AchievementNotifyHold::Notify notify{};
  notify.achievementTid = tid;
  return notify;
}

//! ★ГЕЙТ ОБЯЗАН СПЕРВА ДОКАЗАТЬ СЕБЯ: если `Check` не умеет считать провал,
//! все остальные проверки зелены по построению.
void TestCheckerCanFail()
{
  const int before = g_failures;
  Check(false, "(намеренный провал самопроверки — так и должно быть)");
  const bool counted = g_failures == before + 1;
  g_failures = before;
  Check(counted, "Check() не считает провалы — тесту нельзя верить");
}

//! Есть клиент прямо сейчас — забрали всё и в порядке появления.
void TestTakeReturnsInOrder()
{
  AchievementNotifyHold hold(std::chrono::minutes(15));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);

  Check(hold.Push(7, MakeNotify(10003), t0) == 0, "первый Push не должен вытеснять");
  Check(hold.Push(7, MakeNotify(10018), t0) == 0, "второй Push не должен вытеснять");
  Check(hold.HeldCount() == 2, "удержано должно быть две записи");
  Check(hold.CharacterCount() == 1, "персонаж в удержании должен быть один");

  const auto taken = hold.Take(7);
  Check(taken.size() == 2, "Take обязан отдать обе записи");
  Check(taken.at(0).achievementTid == 10003 and taken.at(1).achievementTid == 10018,
    "Take обязан сохранять ПОРЯДОК появления");
  Check(hold.HeldCount() == 0, "после Take удержание обязано опустеть");
  Check(hold.CharacterCount() == 0, "после Take пустое ведро обязано быть стёрто");
  Check(hold.Take(7).empty(), "повторный Take обязан отдать пусто, а не повтор");
}

//! ★ГЛАВНОЕ СВОЙСТВО FIX-7: пока клиента нет, записи ЛЕЖАТ, а не пропадают.
//! Проверяется тем, что до срока `Expire` не выбрасывает ничего сколько бы раз
//! его ни звали, — то есть ранч-тик может крутиться свободно.
void TestHeldUntilTakenWhileNotExpired()
{
  AchievementNotifyHold hold(std::chrono::seconds(20));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);
  hold.Push(7, MakeNotify(10003), t0);

  for (int second = 0; second < 20; ++second)
  {
    Check(hold.Expire(t0 + std::chrono::seconds(second)) == 0,
      "до истечения срока Expire не имеет права выбрасывать");
  }
  Check(hold.HeldCount() == 1, "запись обязана дожить до срока");

  const auto taken = hold.Take(7);
  Check(taken.size() == 1, "вернувшийся игрок обязан получить придержанное");
}

//! Срок: на границе выбрасывается, до границы — нет.
void TestExpiryDropsOnlyOldEntries()
{
  AchievementNotifyHold hold(std::chrono::seconds(20));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);
  hold.Push(7, MakeNotify(10003), t0);
  hold.Push(7, MakeNotify(10018), t0 + std::chrono::seconds(10));

  Check(hold.Expire(t0 + std::chrono::seconds(19)) == 0,
    "за секунду до срока не должно выбрасываться ничего");
  Check(hold.Expire(t0 + std::chrono::seconds(20)) == 1,
    "ровно на сроке обязана выброситься СТАРШАЯ запись");
  Check(hold.HeldCount() == 1, "младшая запись обязана остаться");
  Check(hold.Take(7).at(0).achievementTid == 10018, "остаться обязана именно младшая");

  hold.Push(9, MakeNotify(10036), t0);
  Check(hold.Expire(t0 + std::chrono::hours(1)) == 1, "просрочка обязана выброситься");
  Check(hold.HeldCount() == 0, "★ПОСЛЕ ПРОТУХАНИЯ УДЕРЖАНО НОЛЬ — это и есть «не течёт»");
  Check(hold.CharacterCount() == 0, "пустое ведро персонажа обязано быть стёрто");
}

//! Потолок: держим не больше `CharacterCap`, вытесняется САМАЯ СТАРАЯ.
void TestCapDropsOldest()
{
  AchievementNotifyHold hold(std::chrono::minutes(15));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);

  std::size_t dropped = 0;
  for (uint16_t index = 0; index < AchievementNotifyHold::CharacterCap; ++index)
    dropped += hold.Push(7, MakeNotify(index), t0);
  Check(dropped == 0, "до потолка не должно вытесняться ничего");
  Check(hold.HeldCount() == AchievementNotifyHold::CharacterCap,
    "удержано должно быть ровно по потолок");

  dropped = hold.Push(7, MakeNotify(999), t0);
  Check(dropped == 1, "запись сверх потолка обязана вытеснить ровно одну");
  Check(hold.HeldCount() == AchievementNotifyHold::CharacterCap,
    "потолок обязан держаться");

  const auto taken = hold.Take(7);
  Check(taken.front().achievementTid == 1, "★вытесняться обязана САМАЯ СТАРАЯ (tid 0), а не свежая");
  Check(taken.back().achievementTid == 999, "свежая запись обязана остаться");
}

//! Потолок — на ПЕРСОНАЖА, а не на всё удержание: сосед не выталкивает соседа.
void TestCapIsPerCharacter()
{
  AchievementNotifyHold hold(std::chrono::minutes(15));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);

  for (uint16_t index = 0; index < AchievementNotifyHold::CharacterCap + 5; ++index)
    hold.Push(7, MakeNotify(index), t0);
  hold.Push(9, MakeNotify(10003), t0);

  Check(hold.HeldCount() == AchievementNotifyHold::CharacterCap + 1,
    "переполнение у одного персонажа не имеет права трогать другого");
  Check(hold.Take(9).size() == 1, "у соседа обязана остаться его запись");
}

//! Персонажи независимы: Take одного не трогает другого.
void TestCharactersAreIndependent()
{
  AchievementNotifyHold hold(std::chrono::minutes(15));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);
  hold.Push(7, MakeNotify(10003), t0);
  hold.Push(9, MakeNotify(10018), t0);

  const auto characters = hold.Characters();
  Check(characters.size() == 2, "Characters обязан назвать обоих");

  Check(hold.Take(7).size() == 1, "Take обязан отдать записи ЗАПРОШЕННОГО");
  Check(hold.HeldCount() == 1, "записи соседа обязаны остаться на месте");
  Check(hold.Take(9).at(0).achievementTid == 10018, "и остаться ИМЕННО его записями");
}

} // namespace

int main()
{
  TestCheckerCanFail();
  TestTakeReturnsInOrder();
  TestHeldUntilTakenWhileNotExpired();
  TestExpiryDropsOnlyOldEntries();
  TestCapDropsOldest();
  TestCapIsPerCharacter();
  TestCharactersAreIndependent();

  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
