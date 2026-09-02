/**
 * Alicia Server - dedicated server software
 * Copyright (C) 2026 Story Of Alicia
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

// LOA-fix (R71-22, находки ревью 3 #1 и #5): ПРАВИЛА РЕЕСТРА ЭКЗЕМПЛЯРОВ ЭФФЕКТОВ.
//
// Реестр — единственное, чем сервер отличает честный отчёт «на мне сработало» от
// выдуманного, и ревью три итерации подряд находило в нём дыры, которые НЕ ловились
// ни сборкой, ни стендом: вытеснение живой записи, оборот счётчика, повторное
// применение одного экземпляра. Здесь эти правила проверяются напрямую, без сети и
// без докера, — то есть красное видно раньше, чем собран образ.
//
// ★КАЖДАЯ ПРОВЕРКА УМЕЕТ ПРОВАЛИТЬСЯ: как именно — написано над ней.

#include "server/tracker/RaceTracker.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{

using server::tracker::Oid;
using server::tracker::RaceTracker;

//! Убрать правило запрета оборота из `CanIssueEffectInstances` — и цикл не кончится
//! за отведённые итерации: счётчик обернётся, `CanIssue` останется истинным, а
//! `HasIssuedEffectInstanceId(0xFFFF)` начнёт врать «да».
void TestIssuanceNeverWraps()
{
  RaceTracker tracker;
  constexpr Oid Caster = 1;
  constexpr uint16_t Block = 64;

  uint32_t issued = 0;
  int iterations = 0;
  // 10 000 x 64 = 640 000 номеров — вдесятеро больше домена. Если бюджет
  // суммарной выдачи не работает, цикл дойдёт до предела итераций.
  while (iterations < 10000 && tracker.CanIssueEffectInstances(Caster, Block))
  {
    const uint16_t first = tracker.GetNextEffectInstanceIdAndIncrementBy(Block);
    tracker.AddEffectInstances(first, Block, 10, Caster, false, {});
    // Освобождаем место — ровно то, чем кастер обходил потолок живых записей.
    tracker.RemoveEffectInstances(first, Block);
    issued += Block;
    ++iterations;
  }

  // Выдача упёрлась в границу домена, а не в предел итераций.
  assert(iterations < 10000);
  assert(not tracker.CanIssueEffectInstances(Caster, Block));
  assert(issued <= 65535);

  // Предикат «сервер выдавал такой номер» остался честным в обе стороны.
  assert(tracker.HasIssuedEffectInstanceId(0));
  assert(not tracker.HasIssuedEffectInstanceId(65535));
  assert(not tracker.HasIssuedEffectInstanceId(70000));
}

//! Вернуть вытеснение (или снятие записи по совпадению номера) — и эта проверка
//! покраснеет: живая запись первого кастера исчезнет.
void TestLiveRecordsAreNeverEvicted()
{
  RaceTracker tracker;
  constexpr Oid FirstCaster = 1;
  constexpr Oid SecondCaster = 2;

  // Первый кастер выбирает свой потолок целиком.
  const auto maxPerRacer = static_cast<uint16_t>(RaceTracker::MaxEffectInstancesPerRacer);
  assert(tracker.CanIssueEffectInstances(FirstCaster, maxPerRacer));
  const uint16_t firstId = tracker.GetNextEffectInstanceIdAndIncrementBy(maxPerRacer);
  tracker.AddEffectInstances(firstId, maxPerRacer, 10, FirstCaster, false, {});

  // Потолок считается НА КАСТЕРА: первому места больше нет, второму — есть.
  assert(not tracker.CanIssueEffectInstances(FirstCaster, 1));
  assert(tracker.CanIssueEffectInstances(SecondCaster, 8));

  const uint16_t secondId = tracker.GetNextEffectInstanceIdAndIncrementBy(8);
  tracker.AddEffectInstances(secondId, 8, 10, SecondCaster, false, {});

  // Ни одна запись первого кастера не пропала.
  for (uint16_t i = 0; i < maxPerRacer; ++i)
  {
    const auto* instance = tracker.FindEffectInstance(static_cast<uint16_t>(firstId + i));
    assert(instance != nullptr);
    assert(instance->casterOid == FirstCaster);
  }
}

//! Убрать потребление пары «экземпляр + цель» — и повтор станет разрешённым.
void TestInstanceTargetPairIsConsumedOnce()
{
  RaceTracker tracker;
  constexpr Oid Caster = 1;
  constexpr Oid FirstTarget = 20;
  constexpr Oid SecondTarget = 21;

  assert(tracker.CanIssueEffectInstances(Caster, 1));
  const uint16_t instanceId = tracker.GetNextEffectInstanceIdAndIncrementBy(1);
  tracker.AddEffectInstances(
    instanceId, 1, 2, Caster, false, std::vector<Oid>{FirstTarget, SecondTarget});

  const auto* instance = tracker.FindEffectInstance(instanceId);
  assert(instance != nullptr);
  assert(instance->authorizedTargets.size() == 2);
  assert(not instance->serverApplied);

  // Первый отчёт цели проходит, второй — нет.
  assert(tracker.ConsumeEffectInstanceTarget(instanceId, FirstTarget));
  assert(not tracker.ConsumeEffectInstanceTarget(instanceId, FirstTarget));

  // Другая цель того же экземпляра — своя, независимая пара.
  assert(tracker.ConsumeEffectInstanceTarget(instanceId, SecondTarget));
  assert(not tracker.ConsumeEffectInstanceTarget(instanceId, SecondTarget));

  // Несуществующий экземпляр не потребляется вовсе.
  assert(not tracker.ConsumeEffectInstanceTarget(static_cast<uint16_t>(instanceId + 1), FirstTarget));
}

//! Убрать фейл-клоуз `MarkEffectInstanceServerApplied` — и пометка перестанет
//! ставиться, то есть расхождение классификации снова станет молчаливым.
void TestServerAppliedInstancesAreMarked()
{
  RaceTracker tracker;
  constexpr Oid Caster = 1;

  const uint16_t instanceId = tracker.GetNextEffectInstanceIdAndIncrementBy(1);
  tracker.AddEffectInstances(instanceId, 1, 4, Caster, true, {});
  assert(tracker.FindEffectInstance(instanceId)->serverApplied);

  const uint16_t secondId = tracker.GetNextEffectInstanceIdAndIncrementBy(1);
  tracker.AddEffectInstances(secondId, 1, 2, Caster, false, {});
  assert(not tracker.FindEffectInstance(secondId)->serverApplied);
  tracker.MarkEffectInstanceServerApplied(secondId);
  assert(tracker.FindEffectInstance(secondId)->serverApplied);
}

//! Убрать сброс `_nextEffectInstanceId`/`_effectInstances` из `Clear()` — и записи
//! прошлого заезда останутся живыми в следующем.
void TestClearMakesInstancesPerRace()
{
  RaceTracker tracker;
  constexpr Oid Caster = 1;

  const uint16_t instanceId = tracker.GetNextEffectInstanceIdAndIncrementBy(4);
  tracker.AddEffectInstances(instanceId, 4, 10, Caster, false, {});
  assert(tracker.FindEffectInstance(instanceId) != nullptr);
  assert(tracker.HasIssuedEffectInstanceId(instanceId));

  tracker.Clear();

  assert(tracker.FindEffectInstance(instanceId) == nullptr);
  assert(not tracker.HasIssuedEffectInstanceId(instanceId));
  assert(tracker.CanIssueEffectInstances(Caster, RaceTracker::MaxEffectInstancesPerRacer));
}

} // namespace

int main()
{
  TestIssuanceNeverWraps();
  TestLiveRecordsAreNeverEvicted();
  TestInstanceTargetPairIsConsumedOnce();
  TestServerAppliedInstancesAreMarked();
  TestClearMakesInstancesPerRace();

  std::puts("TestEffectInstances: OK");
  return 0;
}
