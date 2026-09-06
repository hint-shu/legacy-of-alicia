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
#include <cstddef>
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

//! Кадр ВЗЯТОГО ТИРА — то, ради чего очередь и существует.
AchievementNotifyHold::Notify MakeCompletedNotify(const uint16_t tid)
{
  auto notify = MakeNotify(tid);
  notify.objectiveProgress.isCompleted = true;
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

  Check(hold.Push(7, MakeNotify(10003), t0).droppedByCharacterCap == 0,
    "первый Push не должен вытеснять");
  Check(hold.Push(7, MakeNotify(10018), t0).droppedByCharacterCap == 0,
    "второй Push не должен вытеснять");
  Check(hold.HeldCount() == 2, "удержано должно быть две записи");
  Check(hold.CharacterCount() == 1, "персонаж в удержании должен быть один");

  const auto taken = hold.Take(7);
  Check(taken.size() == 2, "Take обязан отдать обе записи");
  Check(taken.at(0).notify.achievementTid == 10003
      and taken.at(1).notify.achievementTid == 10018,
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
  Check(hold.Take(7).at(0).notify.achievementTid == 10018,
    "остаться обязана именно младшая");

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
    dropped += hold.Push(7, MakeNotify(index), t0).droppedByCharacterCap;
  Check(dropped == 0, "до потолка не должно вытесняться ничего");
  Check(hold.HeldCount() == AchievementNotifyHold::CharacterCap,
    "удержано должно быть ровно по потолок");

  dropped = hold.Push(7, MakeNotify(999), t0).droppedByCharacterCap;
  Check(dropped == 1, "запись сверх потолка обязана вытеснить ровно одну");
  Check(hold.HeldCount() == AchievementNotifyHold::CharacterCap,
    "потолок обязан держаться");

  const auto taken = hold.Take(7);
  Check(taken.front().notify.achievementTid == 1,
    "★вытесняться обязана САМАЯ СТАРАЯ (tid 0), а не свежая");
  Check(taken.back().notify.achievementTid == 999, "свежая запись обязана остаться");
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
  Check(hold.Take(9).at(0).notify.achievementTid == 10018,
    "и остаться ИМЕННО его записями");
}

//! LOA (R70-fix-8, находка Codex 6 WARN-3): ★ПОТОЛОК ВЫТЕСНЯЕТ ПРОГРЕСС, А НЕ
//! НАГРАДУ. Кладём завершение ПЕРВЫМ (значит, оно самое старое), заполняем
//! остаток прогрессными кадрами и переполняем. Слепое «выбрось самое старое»
//! отдало бы именно завершение — и игрок не увидел бы взятый тир.
void TestCapEvictsProgressBeforeCompletion()
{
  AchievementNotifyHold hold(std::chrono::minutes(15));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);

  Check(
    hold.Push(7, MakeCompletedNotify(10003), t0).droppedByCharacterCap == 0,
    "первый Push не должен вытеснять");
  for (std::size_t index = 1; index < AchievementNotifyHold::CharacterCap; ++index)
    hold.Push(7, MakeNotify(static_cast<uint16_t>(20000 + index)), t0);
  Check(
    hold.HeldCount() == AchievementNotifyHold::CharacterCap,
    "очередь обязана быть заполнена ровно под потолок");

  Check(
    hold.Push(7, MakeNotify(30000), t0).droppedByCharacterCap == 1,
    "запись сверх потолка обязана вытеснить ровно одну");

  const auto taken = hold.Take(7);
  Check(
    taken.size() == AchievementNotifyHold::CharacterCap,
    "после вытеснения обязано остаться ровно столько, сколько потолок");
  bool completionSurvived = false;
  for (const auto& entry : taken)
  {
    if (entry.notify.objectiveProgress.isCompleted
      and entry.notify.achievementTid == 10003)
      completionSurvived = true;
  }
  Check(
    completionSurvived,
    "вытеснение обязано было выбросить ПРОГРЕССНЫЙ кадр, а не взятый тир");
  Check(
    taken.front().notify.achievementTid == 10003,
    "уцелевшее завершение обязано остаться первым в порядке выдачи");
  Check(
    taken.at(1).notify.achievementTid == 20002,
    "выброшен обязан быть САМЫЙ СТАРЫЙ прогрессный кадр (20001), а не любой");
}

//! ★ЕСЛИ ПРОГРЕССНЫХ КАДРОВ НЕТ ВОВСЕ — потолок обязан держаться всё равно,
//! выбрасывая самое старое завершение. Иначе «беречь награды» превращается в
//! неограниченный рост от одного игрока, то есть в отсутствие потолка.
void TestCapEvictsOldestCompletionWhenAllCompleted()
{
  AchievementNotifyHold hold(std::chrono::minutes(15));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);

  for (std::size_t index = 0; index < AchievementNotifyHold::CharacterCap; ++index)
    hold.Push(7, MakeCompletedNotify(static_cast<uint16_t>(20000 + index)), t0);
  Check(
    hold.Push(7, MakeCompletedNotify(30000), t0).droppedByCharacterCap == 1,
    "потолок обязан сработать и на одних завершениях");
  Check(
    hold.HeldCount() == AchievementNotifyHold::CharacterCap,
    "после вытеснения удержано обязано быть ровно по потолок");

  const auto taken = hold.Take(7);
  Check(
    taken.front().notify.achievementTid == 20001,
    "выброшено обязано быть САМОЕ СТАРОЕ завершение (20000)");
  Check(
    taken.back().notify.achievementTid == 30000,
    "новое завершение обязано лежать в хвосте");
}

//! === СЛУЧАЙ 9 (LOA-fix R81, #261) ========================================
//! ★ПОТОЛОК НА ЧИСЛО ПЕРСОНАЖЕЙ ДЕРЖИТСЯ. До R81 карту не ограничивало НИЧТО,
//! кроме срока; подняв срок с 15 минут до суток, раунд обязан был поставить
//! вторую границу — иначе одна неограниченная величина сменилась бы другой.
//! Стендом это недоказуемо: `Push` достижим только из `RaceInstance::Stop()`,
//! то есть положить 300 придержанных очередей нечем.
void TestCharacterCountCapHolds()
{
  AchievementNotifyHold hold(std::chrono::hours(24));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);

  for (std::size_t index = 0;
       index < AchievementNotifyHold::CharacterCountCap + 40; ++index)
  {
    hold.Push(
      static_cast<server::data::Uid>(1000 + index),
      MakeNotify(10003),
      t0 + std::chrono::seconds(static_cast<long>(index)));
  }
  Check(hold.CharacterCount() == AchievementNotifyHold::CharacterCountCap,
    "★число персонажей в удержании обязано упереться в CharacterCountCap");
  Check(hold.HeldCount() == AchievementNotifyHold::CharacterCountCap,
    "записей обязано остаться ровно по числу уцелевших персонажей");
}

//! === СЛУЧАЙ 10 (LOA-fix R81, #261) =======================================
//! ★ВЫТЕСНЯЕТСЯ ПЕРСОНАЖ С САМОЙ СТАРОЙ ГОЛОВНОЙ ЗАПИСЬЮ, А НЕ СЛУЧАЙНЫЙ КЛЮЧ.
//! `unordered_map::begin()` отдаёт «кого попало» по хэшу — правило на таком
//! выборе было бы монеткой, а не предикатом, и половина прогонов «проходила»
//! бы случайно.
void TestEvictionPicksOldestHead()
{
  AchievementNotifyHold hold(std::chrono::hours(24));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);

  // Персонаж 1 — САМЫЙ СТАРЫЙ; дальше строго моложе.
  for (std::size_t index = 0;
       index < AchievementNotifyHold::CharacterCountCap; ++index)
  {
    hold.Push(
      static_cast<server::data::Uid>(1 + index),
      MakeNotify(10003),
      t0 + std::chrono::seconds(static_cast<long>(index)));
  }
  Check(hold.CharacterCount() == AchievementNotifyHold::CharacterCountCap,
    "карта обязана стоять ровно на потолке до вытеснения");

  const auto pushed = hold.Push(
    99999, MakeNotify(10018),
    t0 + std::chrono::hours(2));
  Check(pushed.evictedCharacters == 1,
    "★новый персонаж сверх потолка обязан вытеснить РОВНО одного");
  Check(hold.Take(1).empty(),
    "★вытеснен обязан быть персонаж с САМОЙ СТАРОЙ головной записью (uid 1)");
  Check(hold.Take(2).size() == 1,
    "второй по старшинству обязан уцелеть — вытеснение не случайное");
  Check(hold.Take(99999).size() == 1, "новичок обязан лежать в удержании");
}

//! === СЛУЧАЙ 11 (LOA-fix R81, #261) — КРАСНАЯ ТОЧКА `neg-v` ===============
//! ★ДВА ЧИСЛА ПРОВЕРЯЮТСЯ ПОРОЗНЬ. Переполнение очереди ОДНОГО персонажа и
//! вытеснение ЧУЖОГО персонажа целиком — разные события с разными потолками и
//! разными строками лога. Сложи их в одно число — и строка R70
//! («… dropped (cap {} per character)») станет ложной по ОБОИМ числам.
void TestPushResultSeparatesTwoEvictions()
{
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);

  // (а) переполнение очереди ОДНОГО персонажа: первое число > 0, второе == 0.
  {
    AchievementNotifyHold hold(std::chrono::hours(24));
    for (std::size_t index = 0; index < AchievementNotifyHold::CharacterCap; ++index)
      hold.Push(7, MakeNotify(static_cast<uint16_t>(index)), t0);
    const auto pushed = hold.Push(7, MakeNotify(999), t0);
    Check(pushed.droppedByCharacterCap == 1,
      "★переполнение очереди персонажа обязано считаться СВОИМ числом");
    Check(pushed.evictedCharacters == 0,
      "★и НЕ обязано попадать в число вытесненных персонажей");
  }

  // (б) переполнение КАРТЫ: первое число == 0, второе > 0.
  {
    AchievementNotifyHold hold(std::chrono::hours(24));
    for (std::size_t index = 0;
         index < AchievementNotifyHold::CharacterCountCap; ++index)
    {
      hold.Push(
        static_cast<server::data::Uid>(1 + index),
        MakeNotify(10003),
        t0 + std::chrono::seconds(static_cast<long>(index)));
    }
    const auto pushed = hold.Push(99999, MakeNotify(10018), t0 + std::chrono::hours(2));
    Check(pushed.evictedCharacters == 1,
      "★вытеснение персонажа обязано считаться СВОИМ числом");
    Check(pushed.droppedByCharacterCap == 0,
      "★и НЕ обязано попадать в число выброшенных потолком персонажа");
  }
}

//! === СЛУЧАЙ 12 (LOA-fix R81, #261) =======================================
//! ★ВОЗВРАТ ПАЧКИ НЕ ПРОДЛЕВАЕТ СРОК. `Take` отдаёт `queuedAt`, и возврат идёт
//! ТЕМ ЖЕ временем. Верни мы «текущим» — попап жил бы вечно у любого
//! персонажа, чья запись читается с ошибкой, и суточный срок стал бы
//! бессрочным.
void TestReturnedBatchKeepsItsAge()
{
  AchievementNotifyHold hold(std::chrono::seconds(20));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);
  hold.Push(7, MakeNotify(10003), t0);

  const auto taken = hold.Take(7);
  Check(taken.size() == 1, "Take обязан отдать запись");
  Check(taken.at(0).queuedAt == t0, "★Take обязан отдать ИСХОДНОЕ время постановки");

  // Возврат тем же временем, что и было.
  for (const auto& entry : taken)
    hold.Push(7, entry.notify, entry.queuedAt);
  Check(hold.HeldCount() == 1, "возвращённая запись обязана снова лежать в удержании");
  Check(hold.Expire(t0 + std::chrono::seconds(20)) == 1,
    "★возвращённая запись обязана протухнуть В СВОЙ СРОК, а не отсчитывать заново");
  Check(hold.HeldCount() == 0, "после протухания удержано ноль");
}

//! === СЛУЧАЙ 13 (LOA-fix R81, #261) =======================================
//! ★СЛЕДСТВИЕ (б) НОВОГО КОНТРАКТА ПОТОКОВ: возврат полной пачки персонажу,
//! которому гоночный поток успел положить ещё один кадр, упирается в
//! `CharacterCap` — и вытесняет ПРОГРЕССНЫЙ кадр, а не завершение тира.
//! Цена отказа чтения записи — один счётчик, но НЕ награда.
void TestReturnedBatchEvictsProgressNotCompletion()
{
  AchievementNotifyHold hold(std::chrono::hours(24));
  const auto t0 = Clock::time_point{} + std::chrono::hours(1);

  // Полная пачка: одно ЗАВЕРШЕНИЕ и остальное — прогресс.
  hold.Push(7, MakeCompletedNotify(10003), t0);
  for (std::size_t index = 1; index < AchievementNotifyHold::CharacterCap; ++index)
    hold.Push(7, MakeNotify(static_cast<uint16_t>(20000 + index)), t0);
  const auto taken = hold.Take(7);
  Check(taken.size() == AchievementNotifyHold::CharacterCap,
    "снята обязана быть полная пачка");

  // Гоночный поток успел положить ещё один кадр между Take и возвратом.
  hold.Push(7, MakeNotify(30000), t0 + std::chrono::seconds(1));

  std::size_t droppedOnReturn = 0;
  std::size_t evictedOnReturn = 0;
  for (const auto& entry : taken)
  {
    const auto pushed = hold.Push(7, entry.notify, entry.queuedAt);
    droppedOnReturn += pushed.droppedByCharacterCap;
    evictedOnReturn += pushed.evictedCharacters;
  }
  Check(droppedOnReturn == 1,
    "★возврат сверх потолка обязан выбросить ровно один кадр");
  Check(evictedOnReturn == 0,
    "возврат СВОЕМУ ЖЕ персонажу не имеет права вытеснять чужие очереди");
  Check(hold.HeldCount() == AchievementNotifyHold::CharacterCap,
    "потолок персонажа обязан держаться и на возврате");

  const auto after = hold.Take(7);
  bool completionSurvived = false;
  for (const auto& entry : after)
  {
    if (entry.notify.objectiveProgress.isCompleted
      and entry.notify.achievementTid == 10003)
      completionSurvived = true;
  }
  Check(completionSurvived,
    "★выброшен обязан быть ПРОГРЕССНЫЙ кадр, а не взятый тир");
}

} // namespace

int main()
{
  TestCheckerCanFail();
  TestTakeReturnsInOrder();
  TestHeldUntilTakenWhileNotExpired();
  TestExpiryDropsOnlyOldEntries();
  TestCapDropsOldest();
  TestCapEvictsProgressBeforeCompletion();
  TestCapEvictsOldestCompletionWhenAllCompleted();
  TestCapIsPerCharacter();
  TestCharactersAreIndependent();
  TestCharacterCountCapHolds();
  TestEvictionPicksOldestHead();
  TestPushResultSeparatesTwoEvictions();
  TestReturnedBatchKeepsItsAge();
  TestReturnedBatchEvictsProgressNotCompletion();

  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
