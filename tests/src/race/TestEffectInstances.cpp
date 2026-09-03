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

// LOA-fix (R71-22, находки ревью 3 #1 и #5; ПЕРЕПИСАНО R71-25 по находке ревью 4 #6):
// ПРАВИЛА РЕЕСТРА ЭКЗЕМПЛЯРОВ ЭФФЕКТОВ.
//
// Реестр — единственное, чем сервер отличает честный отчёт «на мне сработало» от
// выдуманного, и ревью четыре итерации подряд находило в нём дыры, которые НЕ ловились
// ни сборкой, ни стендом: вытеснение живой записи, оборот счётчика, повторное
// применение одного экземпляра, общий на комнату бюджет номеров. Здесь эти правила
// проверяются напрямую, без сети и без докера, — то есть красное видно раньше, чем
// собран образ.
//
// ★`assert` НЕ ИСПОЛЬЗУЕТСЯ (находка ревью 4 #6): боевой образ собирается с -DNDEBUG,
// и в прошлой редакции ВСЕ проверки этого файла под ним исчезали — тест был зелёным,
// потому что не проверял ничего ([[checker-written-by-form]]). Тот же дефект ревью
// уже находило в этом раунде однажды; повторять форму, которую сам же чинил, нельзя.
// Проверки считают провалы явно и роняют код возврата.
//
// ★КАЖДАЯ ПРОВЕРКА УМЕЕТ ПРОВАЛИТЬСЯ: как именно — написано над ней.

#include "server/tracker/RaceTracker.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{

using server::tracker::Oid;
using server::tracker::RaceTracker;

int failures = 0;

void Check(const bool condition, const char* what)
{
  if (not condition)
  {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

//! Убрать бюджет выдачи из `CanIssueEffectInstances` — и цикл не кончится за
//! отведённые итерации: счётчик обернётся, `CanIssue` останется истинным, а
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
    const uint16_t first = tracker.GetNextEffectInstanceIdAndIncrementBy(Caster, Block);
    tracker.AddEffectInstances(first, Block, 10, Caster, false, {});
    // Освобождаем место — ровно то, чем кастер обходил потолок живых записей.
    tracker.RemoveEffectInstances(first, Block);
    issued += Block;
    ++iterations;
  }

  // Выдача упёрлась в бюджет, а не в предел итераций.
  Check(iterations < 10000, "выдача обязана упереться в бюджет, а не крутиться вечно");
  Check(
    not tracker.CanIssueEffectInstances(Caster, Block),
    "исчерпавший бюджет кастер обязан получать отказ");
  Check(
    issued <= RaceTracker::MaxEffectInstanceIssuancePerRacer,
    "суммарная выдача кастера не может превысить его бюджет");

  // Предикат «сервер выдавал такой номер» остался честным в обе стороны.
  Check(tracker.HasIssuedEffectInstanceId(0), "номер 0 сервер выдавал");
  Check(not tracker.HasIssuedEffectInstanceId(65535), "номер 0xFFFF сервер не выдавал");
  Check(not tracker.HasIssuedEffectInstanceId(70000), "номер шире домена — не выдавался");
}

//! LOA-fix (R71-25, находка ревью 4 #2): БЮДЖЕТ ПЕР-КАСТЕРНЫЙ, А НЕ ОБЩИЙ.
//!
//! Вернуть общий на комнату запрет (или снять пер-кастерный бюджет) — и эта проверка
//! покраснеет: честный второй кастер получит отказ из-за первого.
void TestIssuanceBudgetIsCasterLocal()
{
  RaceTracker tracker;
  constexpr Oid Flooder = 1;
  constexpr Oid HonestRacer = 2;

  // ★ПО ОДНОМУ НОМЕРУ ЗА РАЗ, И ЭТО ВАЖНО ДЛЯ САМОЙ ПРОВЕРКИ: блоками по 64 флудер
  // без бюджета останавливался бы за 64 номера до конца домена, и «честному не
  // хватило» не наступало бы никогда — проверка была бы зелёной по случайности.
  // По одному он выбирает домен ДО КОНЦА, и красное видно.
  int issuedByFlooder = 0;
  while (tracker.CanIssueEffectInstances(Flooder, 1))
  {
    const uint16_t first = tracker.GetNextEffectInstanceIdAndIncrementBy(Flooder, 1);
    tracker.AddEffectInstances(first, 1, 10, Flooder, false, {});
    tracker.RemoveEffectInstances(first, 1);
    ++issuedByFlooder;
  }
  Check(
    issuedByFlooder
      == static_cast<int>(RaceTracker::MaxEffectInstanceIssuancePerRacer),
    "флудера обязан остановить его собственный бюджет, а не домен номеров комнаты");

  Check(
    not tracker.CanIssueEffectInstances(Flooder, 1),
    "флудер обязан отказывать в кастах САМ СЕБЕ");
  Check(
    tracker.CanIssueEffectInstances(HonestRacer, 8),
    "честный гонщик обязан кастовать после того, как сосед выжег свой бюджет");
  Check(
    tracker.CanIssueEffectInstances(HonestRacer, 1),
    "и даже один номер честному гонщику обязан достаться");

  const uint16_t honestId = tracker.GetNextEffectInstanceIdAndIncrementBy(HonestRacer, 8);
  tracker.AddEffectInstances(honestId, 8, 10, HonestRacer, false, {});
  Check(
    tracker.FindEffectInstance(honestId) != nullptr,
    "запись честного гонщика обязана появиться");

  // Домен номеров при этом не исчерпан: 4096 на кастера x 8 мест = 32 768 из 65 535
  // (R71-30: числа пересчитаны по ОБОИМ путям выдачи, см. `RaceTracker.hpp`).
  Check(
    RaceTracker::MaxEffectInstanceIssuancePerRacer * 8
      <= std::numeric_limits<uint16_t>::max(),
    "бюджет обязан быть таким, чтобы полная комната не выбрала домен номеров");
}

//! Вернуть вытеснение (или снятие записи по совпадению номера) — и эта проверка
//! покраснеет: живая запись первого кастера исчезнет.
void TestLiveRecordsAreNeverEvicted()
{
  RaceTracker tracker;
  constexpr Oid FirstCaster = 1;
  constexpr Oid SecondCaster = 2;

  // Первый кастер выбирает свой потолок ЖИВЫХ записей целиком.
  const auto maxPerRacer = static_cast<uint16_t>(RaceTracker::MaxEffectInstancesPerRacer);
  Check(
    tracker.CanIssueEffectInstances(FirstCaster, maxPerRacer),
    "первому кастеру обязано хватить места на полный потолок");
  const uint16_t firstId = tracker.GetNextEffectInstanceIdAndIncrementBy(FirstCaster, maxPerRacer);
  tracker.AddEffectInstances(firstId, maxPerRacer, 10, FirstCaster, false, {});

  // Потолок считается НА КАСТЕРА: первому места больше нет, второму — есть.
  Check(
    not tracker.CanIssueEffectInstances(FirstCaster, 1),
    "выбравшему потолок живых записей обязан быть отказ");
  Check(
    tracker.CanIssueEffectInstances(SecondCaster, 8),
    "второму кастеру потолок соседа не мешает");

  const uint16_t secondId = tracker.GetNextEffectInstanceIdAndIncrementBy(SecondCaster, 8);
  tracker.AddEffectInstances(secondId, 8, 10, SecondCaster, false, {});

  // Ни одна запись первого кастера не пропала.
  bool allAlive = true;
  for (uint16_t i = 0; i < maxPerRacer; ++i)
  {
    const auto* instance = tracker.FindEffectInstance(static_cast<uint16_t>(firstId + i));
    if (instance == nullptr || instance->casterOid != FirstCaster)
      allAlive = false;
  }
  Check(allAlive, "ни одна живая запись первого кастера не имеет права исчезнуть");
}

//! LOA-fix (R71-25, находка ревью 4 #6): НАСТОЯЩЕЕ СТОЛКНОВЕНИЕ НОМЕРОВ.
//!
//! Прошлая редакция «проверки коллизии» брала СЛЕДУЮЩИЙ последовательный номер, то
//! есть коллизию не создавала вовсе — тест был зелёным, потому что ничего не
//! проверял. Здесь номер навязывается ЯВНО: второй `AddEffectInstances` вызывается с
//! тем же `instanceId`, что и первый.
//!
//! Вернуть в `AddEffectInstances` снятие записи по совпадению номера — и проверка
//! покраснеет: победит новая запись, а живая улика первого кастера исчезнет.
void TestDuplicateInstanceIdKeepsTheOlderRecord()
{
  RaceTracker tracker;
  constexpr Oid FirstCaster = 1;
  constexpr Oid SecondCaster = 2;
  constexpr uint16_t SharedId = 7;

  tracker.AddEffectInstances(SharedId, 1, 10, FirstCaster, false, {});
  const auto* firstRecord = tracker.FindEffectInstance(SharedId);
  Check(firstRecord != nullptr, "первая запись обязана появиться");
  Check(
    firstRecord != nullptr && firstRecord->casterOid == FirstCaster,
    "первая запись принадлежит первому кастеру");

  // ТОТ ЖЕ номер от ДРУГОГО кастера с другим типом — столкновение по-настоящему.
  tracker.AddEffectInstances(SharedId, 1, 2, SecondCaster, true, {});

  const auto* afterCollision = tracker.FindEffectInstance(SharedId);
  Check(afterCollision != nullptr, "запись обязана остаться на месте");
  Check(
    afterCollision != nullptr && afterCollision->casterOid == FirstCaster,
    "при столкновении номеров побеждает СТАРАЯ запись");
  Check(
    afterCollision != nullptr && afterCollision->magicType == 10,
    "тип старой записи не имеет права смениться");
  Check(
    afterCollision != nullptr && not afterCollision->serverApplied,
    "флаг старой записи не имеет права смениться");
}

//! LOA-fix (R71-25, находка ревью 4 #1): ТИП ХРАНИТСЯ ПОЛНОЙ ШИРИНОЙ.
//!
//! Вернуть полю `EffectInstance::magicType` ширину `uint16_t` — и эта проверка
//! покраснеет: 0x10002 усечётся до 2 и станет неотличим от FireBall'а.
void TestMagicTypeIsStoredFullWidth()
{
  RaceTracker tracker;
  constexpr Oid Caster = 1;
  constexpr uint32_t WideType = 0x10002;

  const uint16_t instanceId = tracker.GetNextEffectInstanceIdAndIncrementBy(Caster, 1);
  tracker.AddEffectInstances(instanceId, 1, WideType, Caster, false, {});

  const auto* instance = tracker.FindEffectInstance(instanceId);
  Check(instance != nullptr, "запись обязана появиться");
  Check(
    instance != nullptr && instance->magicType == WideType,
    "тип магии обязан храниться полной шириной, без усечения до 16 бит");
  Check(
    instance != nullptr && instance->magicType != 2,
    "широкий тип не имеет права стать неотличимым от типа 2");
}

//! Убрать потребление пары «экземпляр + цель» — и повтор станет разрешённым.
void TestInstanceTargetPairIsConsumedOnce()
{
  RaceTracker tracker;
  constexpr Oid Caster = 1;
  constexpr Oid FirstTarget = 20;
  constexpr Oid SecondTarget = 21;

  Check(tracker.CanIssueEffectInstances(Caster, 1), "место под один экземпляр есть");
  const uint16_t instanceId = tracker.GetNextEffectInstanceIdAndIncrementBy(Caster, 1);
  tracker.AddEffectInstances(
    instanceId, 1, 2, Caster, false, std::vector<Oid>{FirstTarget, SecondTarget});

  const auto* instance = tracker.FindEffectInstance(instanceId);
  Check(instance != nullptr, "запись обязана появиться");
  Check(
    instance != nullptr && instance->authorizedTargets.size() == 2,
    "запись обязана помнить обе названные цели");
  Check(instance != nullptr && not instance->serverApplied, "это не серверный эффект");

  // Первый отчёт цели проходит, второй — нет.
  Check(
    tracker.ConsumeEffectInstanceTarget(instanceId, FirstTarget),
    "первый отчёт цели обязан пройти");
  Check(
    not tracker.ConsumeEffectInstanceTarget(instanceId, FirstTarget),
    "второй отчёт той же цели обязан быть отвергнут");

  // Другая цель того же экземпляра — своя, независимая пара.
  Check(
    tracker.ConsumeEffectInstanceTarget(instanceId, SecondTarget),
    "вторая названная цель отчитывается независимо");
  Check(
    not tracker.ConsumeEffectInstanceTarget(instanceId, SecondTarget),
    "и тоже ровно один раз");

  // Несуществующий экземпляр не потребляется вовсе.
  Check(
    not tracker.ConsumeEffectInstanceTarget(
      static_cast<uint16_t>(instanceId + 1), FirstTarget),
    "выдуманный номер экземпляра не потребляется");
}

//! Убрать фейл-клоуз `MarkEffectInstanceServerApplied` — и пометка перестанет
//! ставиться, то есть расхождение классификации снова станет молчаливым.
void TestServerAppliedInstancesAreMarked()
{
  RaceTracker tracker;
  constexpr Oid Caster = 1;

  const uint16_t instanceId = tracker.GetNextEffectInstanceIdAndIncrementBy(Caster, 1);
  tracker.AddEffectInstances(instanceId, 1, 4, Caster, true, {});
  const auto* serverApplied = tracker.FindEffectInstance(instanceId);
  Check(
    serverApplied != nullptr && serverApplied->serverApplied,
    "экземпляр серверного эффекта обязан быть помечен при записи");

  const uint16_t secondId = tracker.GetNextEffectInstanceIdAndIncrementBy(Caster, 1);
  tracker.AddEffectInstances(secondId, 1, 2, Caster, false, {});
  const auto* clientReported = tracker.FindEffectInstance(secondId);
  Check(
    clientReported != nullptr && not clientReported->serverApplied,
    "экземпляр атаки не помечен серверным");
  tracker.MarkEffectInstanceServerApplied(secondId);
  const auto* marked = tracker.FindEffectInstance(secondId);
  Check(
    marked != nullptr && marked->serverApplied,
    "фейл-клоуз обязан пометить экземпляр серверным");
}

//! Убрать сброс `_nextEffectInstanceId`/`_effectInstances`/бюджета из `Clear()` — и
//! записи прошлого заезда останутся живыми в следующем.
void TestClearMakesInstancesPerRace()
{
  RaceTracker tracker;
  constexpr Oid Caster = 1;

  const uint16_t instanceId = tracker.GetNextEffectInstanceIdAndIncrementBy(Caster, 4);
  tracker.AddEffectInstances(instanceId, 4, 10, Caster, false, {});
  Check(tracker.FindEffectInstance(instanceId) != nullptr, "запись заезда есть");
  Check(tracker.HasIssuedEffectInstanceId(instanceId), "номер заезда выдан");

  tracker.Clear();

  Check(
    tracker.FindEffectInstance(instanceId) == nullptr,
    "запись прошлого заезда не имеет права пережить `Clear()`");
  Check(
    not tracker.HasIssuedEffectInstanceId(instanceId),
    "номер прошлого заезда не имеет права остаться выданным");
  Check(
    tracker.CanIssueEffectInstances(Caster, RaceTracker::MaxEffectInstancesPerRacer),
    "после `Clear()` кастеру доступен полный потолок живых записей");
}

//! LOA-fix (R71-25, находка ревью 4 #2): бюджет ВЫДАЧИ тоже пер-заездный.
//! Убрать `_issuedEffectInstanceCounts.clear()` из `Clear()` — и гонщик, выбравший
//! бюджет в первом заезде, останется без кастов во всех следующих заездах комнаты.
void TestIssuanceBudgetResetsPerRace()
{
  RaceTracker tracker;
  constexpr Oid Caster = 1;
  constexpr uint16_t Block = 64;

  while (tracker.CanIssueEffectInstances(Caster, Block))
  {
    const uint16_t first = tracker.GetNextEffectInstanceIdAndIncrementBy(Caster, Block);
    tracker.AddEffectInstances(first, Block, 10, Caster, false, {});
    tracker.RemoveEffectInstances(first, Block);
  }
  Check(
    not tracker.CanIssueEffectInstances(Caster, Block),
    "бюджет обязан кончиться внутри заезда");

  tracker.Clear();

  Check(
    tracker.CanIssueEffectInstances(Caster, Block),
    "следующий заезд обязан начинаться с полным бюджетом");
}

//! LOA-fix (R71-26): выбирает ровно `count` номеров бюджета кастера маленькими
//! пакетами и сразу снимает записи. ★Снимает — не для красоты: потолок ЖИВЫХ записей
//! (1024) меньше бюджета выдачи (4096), поэтому без снятия записей отказ пришёл бы по
//! месту под улику и был бы неотличим от отказа по бюджету, а речь здесь именно о
//! бюджете ([[a-blind-checker-says-clean]]).
void DrainIssuanceBudget(RaceTracker& tracker, const Oid caster, const uint32_t count)
{
  uint32_t drained = 0;
  while (drained < count)
  {
    const auto block = static_cast<uint16_t>(std::min<uint32_t>(8, count - drained));
    Check(
      tracker.CanIssueEffectInstances(caster, block),
      "выборка бюджета внутри потолка обязана разрешаться");
    const uint16_t first = tracker.GetNextEffectInstanceIdAndIncrementBy(caster, block);
    tracker.AddEffectInstances(first, block, 10, caster, false, {});
    tracker.RemoveEffectInstances(first, block);
    drained += block;
  }
}

//! LOA-fix (R71-26, находка ревью 6): ПОГРАНИЧНЫЙ КАСТ ДОЛЖЕН ПРОЙТИ ЦЕЛИКОМ.
//!
//! Бюджет выдачи спрашивается и списывается РОВНО ОДИН РАЗ на пакет, до выдачи
//! номеров. Раньше `AddEffectInstances` переспрашивал его уже ПОСЛЕ списания — и на
//! касте, доводящем кастера ровно до потолка, записи не создавались вовсе: номер
//! выдан, каст разослан, а честный отчёт по нему потом получал отказ.
//!
//! ★КАК ЭТА ПРОВЕРКА ПРОВАЛИТСЯ: верни в `AddEffectInstances` полный
//! `CanIssueEffectInstances(casterOid, 1)` (или спиши бюджет до проверки в
//! обработчике) — и `FindEffectInstance` на пограничном касте вернёт `nullptr`.
void TestBoundaryCastIsWrittenInFull()
{
  RaceTracker tracker;
  constexpr Oid Caster = 1;
  constexpr uint32_t MaxIssuance = RaceTracker::MaxEffectInstanceIssuancePerRacer;

  // Выбираем бюджет до «остался ровно один номер». Записи снимаем, чтобы потолок
  // ЖИВЫХ записей не мешал: проверяем именно бюджет выдачи, а не место под улику.
  DrainIssuanceBudget(tracker, Caster, MaxIssuance - 1);

  // Честный ПОГРАНИЧНЫЙ каст: он доводит суммарную выдачу ровно до потолка и обязан
  // пройти целиком — и по разрешению, и по записи.
  Check(
    tracker.CanIssueEffectInstances(Caster, 1),
    "последний номер бюджета обязан быть разрешён");
  const uint16_t boundaryId = tracker.GetNextEffectInstanceIdAndIncrementBy(Caster, 1);
  tracker.AddEffectInstances(boundaryId, 1, 10, Caster, false, {});
  Check(
    tracker.FindEffectInstance(boundaryId) != nullptr,
    "★пограничный каст обязан ОСТАВИТЬ ЗАПИСЬ, а не только выдать номер");
  Check(
    tracker.HasIssuedEffectInstanceId(boundaryId),
    "пограничный номер обязан считаться выданным");

  // Следующий каст — уже сверх бюджета: отказ приходит ДО выдачи номера.
  Check(
    not tracker.CanIssueEffectInstances(Caster, 1),
    "каст сверх бюджета обязан получить отказ");
}

//! LOA-fix (R71-26, находка ревью 6): ПАКЕТ БОЛЬШЕ ОСТАТКА ОТВЕРГАЕТСЯ ЦЕЛИКОМ И
//! НИЧЕГО НЕ СПИСЫВАЕТ.
//!
//! ★КАК ПРОВАЛИТСЯ: спиши бюджет до/без проверки — и остаток после отказанного
//! пакета перестанет быть прежним, то есть честный каст на оставшееся место получит
//! отказ, которого он не заслужил.
void TestOversizedPacketChargesNothing()
{
  RaceTracker tracker;
  constexpr Oid Caster = 1;
  constexpr uint32_t MaxIssuance = RaceTracker::MaxEffectInstanceIssuancePerRacer;

  DrainIssuanceBudget(tracker, Caster, MaxIssuance - 4);

  // Осталось ровно 4 номера. Пакет на 8 (ледяная стена во всю ширину) обязан быть
  // отвергнут ЦЕЛИКОМ — и не занять при этом ни одного номера.
  Check(
    not tracker.CanIssueEffectInstances(Caster, 8),
    "пакет больше остатка обязан быть отвергнут целиком");

  // Остаток не тронут: честные 4 номера обязаны пройти и оставить записи.
  Check(
    tracker.CanIssueEffectInstances(Caster, 4),
    "отказанный пакет не имеет права списать ни одного номера");
  const uint16_t tailId = tracker.GetNextEffectInstanceIdAndIncrementBy(Caster, 4);
  tracker.AddEffectInstances(tailId, 4, 10, Caster, false, {});
  for (uint16_t i = 0; i < 4; ++i)
  {
    Check(
      tracker.FindEffectInstance(static_cast<uint16_t>(tailId + i)) != nullptr,
      "★каждый номер пакета, выбирающего остаток, обязан оставить запись");
  }
  Check(
    not tracker.CanIssueEffectInstances(Caster, 1),
    "после выбранного остатка бюджет обязан кончиться");
}

} // namespace

int main()
{
  TestIssuanceNeverWraps();
  TestIssuanceBudgetIsCasterLocal();
  TestLiveRecordsAreNeverEvicted();
  TestDuplicateInstanceIdKeepsTheOlderRecord();
  TestMagicTypeIsStoredFullWidth();
  TestInstanceTargetPairIsConsumedOnce();
  TestServerAppliedInstancesAreMarked();
  TestClearMakesInstancesPerRace();
  TestIssuanceBudgetResetsPerRace();
  TestBoundaryCastIsWrittenInFull();
  TestOversizedPacketChargesNothing();

  if (failures != 0)
  {
    std::printf("TestEffectInstances: %d FAILURE(S)\n", failures);
    return 1;
  }

  std::puts("TestEffectInstances: OK");
  return 0;
}
