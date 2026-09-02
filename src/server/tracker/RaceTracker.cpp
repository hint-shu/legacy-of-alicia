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

#include "server/tracker/RaceTracker.hpp"
#include <limits>

namespace server::tracker
{

// LOA-fix (R70 итерация 3, backlog #58): ЕДИНСТВЕННОЕ ОПРЕДЕЛЕНИЕ «ЕХАЛ ЛИ».
// Разбор улик, порогов и цены — в объявлении (`RaceTracker.hpp`). Здесь важно
// лишь одно: определение ОДНО, вне заголовка, и его спрашивают оба места, где
// сервер решает, был ли гонщик участником заезда (`HandleUserRaceFinal` и
// `RaceInstance::Stop`). Вторая копия правила разъехалась бы с первой молча.
// ★★ЧЕГО ЭТА УЛИКА НЕ ДОКАЗЫВАЕТ — ЗАПИСАННЫЙ ОСТАТОЧНЫЙ РИСК (решение
// владельца/лида, R70 итерация 4), а НЕ недосмотр. Ревью (Codex, ит.4) право
// буквально: `distanceMetres` сервер СУММИРУЕТ, но не НАБЛЮДАЕТ. Обе точки
// шага приходят из `command.position` (`RaceNetworkHandler.cpp:4186/4190/4329`),
// сверки с геометрией трассы, чекпойнтами, направлением и столкновениями нет
// нигде в дереве, а бюджет шага считается по КОНСТАНТЕ `MaxPlausibleSpeedKph`,
// а не по заявленной в том же пакете скорости. Поэтому МОДИФИЦИРОВАННЫЙ клиент,
// стоя на месте, может «проехать» 250 м тремя пакетами своего же oid
// (t=0/P=0/progress=1, t=1.6/P=125, t=3.2/P=250 — каждый шаг влезает в бюджет
// 300/3.6*1.6 = 133 м), выждать тридцать секунд и купить себе участие, исход и
// строку с `numPlayer`. Раунд это ПРИНИМАЕТ ОСОЗНАННО, по трём причинам:
//  (1) все награды R70 КОСМЕТИЧЕСКИЕ: ноль морковок, ноль предметов, очки
//      кормят только титулы. Ферма даёт значок, а не экономику.
//  (2) сервер приватный (владелец, семья, друзья), а оставшаяся ферма требует
//      ОДНОВРЕМЕННО модифицированного клиента, подделывающего пакеты позиции,
//      И альт-аккаунтов. ДЕШЁВЫЕ фермы — соло-«первое место», ИИ-соперники,
//      стояние на месте, заявка одними часами — закрыты гардами этого раунда,
//      и каждая из них имеет свой негатив в лесенке.
//  (3) авторитетная улика маршрута (валидированный проход чекпойнтов/кругов) —
//      это программа бэклога #30 «Этап 1: серверный трекинг прогресса трассы» и
//      #32 «Этап 3: античит скорости/финиша», у неё СВОИ раунды; воспроизводить
//      её здесь значило бы завести подсистему под видом правки достижений.
//
// TODO(backlog #30 этап 1, #32): когда серверный трекинг трассы приземлится,
// `HasProvenTraversal` ОБЯЗАНА начать читать ЕГО — сумму клиентских дельт
// заменить на валидированный проход чекпойнтов/кругов, а этот комментарий
// удалить вместе с остаточным риском. До тех пор находки, требующие для фарма
// МОДИФИЦИРОВАННОГО клиента, в этом раунде не блокирующие; находки про честных
// игроков, крэши, персистентность, владение потоком, потолок тиров, фильтры
// режима/состава, часовые окна и гард «людей >= 2» — блокирующие как и прежде.
bool RaceTracker::Racer::HasProvenTraversal() const
{
  return distanceMetres >= MinMeaningfulTraversalMetres
    and trustedProgress >= MinMeaningfulRaceProgress;
}

RaceTracker::Racer& RaceTracker::AddRacer(data::Uid characterUid)
{
  const auto [racerIter, created] = _racers.try_emplace(characterUid);
  if (not created)
    throw std::runtime_error("Character is already a racer");

  // Reuse the OID from a previous race if the player was here before, otherwise assign a new one.
  //
  // LOA-fix (R66-1, backlog #193): счётчик берётся ЧЕРЕЗ `ReserveOid`, а не сырым.
  // ★Ноль — это `InvalidEntityOid`, «нет объекта». При обороте счётчика сырое
  // значение выдало бы гонщику идентификатор, который весь остальной код читает
  // как «объекта нет»: гарды вида `racer.oid == InvalidEntityOid` начали бы
  // срабатывать на живом участнике.
  // ★Перешагивание нуля R56 стояло ТОЛЬКО в `ReserveOid` (строкой ниже), а этот
  // путь ходил мимо него — то есть защита была, но не на всех входах. Лечим не
  // добавлением второй копии проверки, а тем, что оба пути идут через ОДНУ
  // ([[total-invariant-beats-list-of-sites]]).
  const auto [oidIter, isNew] = _characterOids.try_emplace(characterUid, InvalidEntityOid);
  if (isNew)
    oidIter->second = ReserveOid();

  racerIter->second.oid = oidIter->second;

  return racerIter->second;
}

Oid RaceTracker::ReserveOid()
{
  // 0 — это InvalidEntityOid, «нет объекта»: при обороте счётчика его надо
  // перешагнуть, иначе выданный id означал бы отсутствие сущности.
  if (_nextCharacterOid == InvalidEntityOid)
    _nextCharacterOid = 1;

  return _nextCharacterOid++;
}

void RaceTracker::RemoveRacer(data::Uid characterUid)
{
  _racers.erase(characterUid);
}

bool RaceTracker::IsRacer(data::Uid characterUid) const
{
  return _racers.contains(characterUid);
}

RaceTracker::Racer& RaceTracker::GetRacer(data::Uid characterUid)
{
  auto racerIter = _racers.find(characterUid);
  if (racerIter == _racers.cend())
    throw std::runtime_error("Character is not a racer");

  return racerIter->second;
}

RaceTracker::RacerObjectMap& RaceTracker::GetRacers()
{
  return _racers;
}

RaceTracker::ItemDeck& RaceTracker::AddItemDeck()
{
  const auto [itemIter, created] = _itemDecks.try_emplace(_nextItemDeckOid);
  if (not created)
    throw std::runtime_error("Item is already added to the race map");

  itemIter->second.oid = _nextItemDeckOid++;
  return itemIter->second;
}

void RaceTracker::RemoveItemDeck(
  const uint16_t itemId)
{
  _itemDecks.erase(itemId);
}

RaceTracker::ItemDeck& RaceTracker::GetItemDeck(
  const Oid itemId)
{
  const auto itemIter = _itemDecks.find(itemId);
  if (itemIter == _itemDecks.cend())
    throw std::runtime_error("Item deck is not in the race map");

  return itemIter->second;
}

RaceTracker::ItemDeckMap& RaceTracker::GetItemDecks()
{
  return _itemDecks;
}

bool RaceTracker::IsEventThrottled(uint32_t eventId)
{
  const auto& now = std::chrono::steady_clock::now();

  const auto& [eventIter, inserted] = _events.try_emplace(eventId);
  if (not inserted and eventIter->second.throttledUntil > now)
  {
    // Existing event was throttled
    return true;
  }
  else if (inserted)
  {
    eventIter->second.id = eventId;
  }

  // New event or event expired, update throttle time
  eventIter->second.throttledUntil = now + EventThrottleDuration;
  return false;
}

RaceTracker::EventMap& RaceTracker::GetEvents()
{
  return _events;
}

RaceTracker::EventItem& RaceTracker::AddEventItem(data::Uid characterUid)
{
  auto& racer = GetRacer(characterUid);
  auto& eventItem = racer.eventItems.emplace_back();
  eventItem.oid = _nextItemDeckOid++;
  return eventItem;
}

Oid RaceTracker::FindEventItem(data::Uid characterUid, Oid oid)
{
  auto& racer = GetRacer(characterUid);
  for (auto& eventItem : racer.eventItems)
  {
    if (eventItem.oid == oid)
      return eventItem.oid;
  }
  return InvalidEntityOid;
}

RaceTracker::EventItem& RaceTracker::GetEventItem(data::Uid characterUid, Oid oid)
{
  auto& racer = GetRacer(characterUid);
  for (auto& eventItem : racer.eventItems)
  {
    if (eventItem.oid == oid)
      return eventItem;
  }

  throw std::runtime_error("Event item is not tracked for racer");
}

void RaceTracker::RemoveEventItem(data::Uid characterUid, Oid oid)
{
  auto& racer = GetRacer(characterUid);
  std::erase_if(racer.eventItems, [oid](const EventItem& e) { return e.oid == oid; });
}

// LOA-fix (R68, backlog #5/#99): КВЕСТОВЫЕ ПРЕДМЕТЫ ГОНЩИКА.
//
// ★Oid берётся из ТОГО ЖЕ `_nextItemDeckOid`, что у деков и яиц: у клиента
// предметы заезда живут в одном пространстве идентификаторов, и «заведомо
// свободного диапазона» здесь не бывает — тот же довод, что у `ReserveOid`
// (R66-1). Бюджет счётчика на заезд: до 8 гонщиков * 32 квестовых предмета +
// до 42 деков карты + до 8 яиц = 306 из 65535, а `Clear()` возвращает счётчик
// в 1 перед каждым заездом.
RaceTracker::QuestItem& RaceTracker::AddQuestItem(data::Uid characterUid)
{
  auto& racer = GetRacer(characterUid);
  auto& questItem = racer.questItems.emplace_back();
  questItem.oid = _nextItemDeckOid++;
  return questItem;
}

// ★ЛУКАП ГОНЩИКА ЗДЕСЬ СВОЙ, А НЕ ЧЕРЕЗ `GetRacer`, И ЭТО НЕ ДУБЛИРОВАНИЕ.
// `GetRacer` бросает на неизвестном ключе; обе функции ниже зовутся с
// КЛИЕНТСКОГО пути (`HandleUserRaceItemGet`), где неизвестный персонаж —
// штатный исход, а не сбой. Обещание «не бросает» в заголовке обязано быть
// правдой в самом коде, а не держаться на дисциплине вызывающих
// ([[obligation-that-can-fail-to-install]]).
RaceTracker::QuestItem* RaceTracker::FindQuestItem(data::Uid characterUid, Oid oid)
{
  const auto racerIter = _racers.find(characterUid);
  if (racerIter == _racers.cend())
    return nullptr;

  for (auto& questItem : racerIter->second.questItems)
  {
    if (questItem.oid == oid)
      return &questItem;
  }

  return nullptr;
}

void RaceTracker::RemoveQuestItem(data::Uid characterUid, Oid oid)
{
  const auto racerIter = _racers.find(characterUid);
  if (racerIter == _racers.cend())
    return;

  std::erase_if(
    racerIter->second.questItems,
    [oid](const QuestItem& e) { return e.oid == oid; });
}

uint16_t RaceTracker::GetNextEffectInstanceIdAndIncrementBy(uint16_t increment)
{
  const uint16_t nextId = _nextEffectInstanceId;
  _nextEffectInstanceId += increment;
  // LOA-fix (R71-15): оборот шестнадцатибитного счётчика запоминается. Сравнение
  // именно с ПРЕЖНИМ значением: после оборота новое всегда меньше.
  if (_nextEffectInstanceId < nextId)
    _effectInstanceIdWrapped = true;
  if (_nextEffectInstanceId == 0)
    _nextEffectInstanceId = 1;
  return nextId;
}

bool RaceTracker::HasIssuedEffectInstanceId(const uint32_t effectInstanceId) const
{
  // Домен идентификатора — шестнадцать бит по построению (`uint16_t` и у счётчика,
  // и у поля в протоколе). Всё, что шире, сервер выдать не мог.
  if (effectInstanceId > std::numeric_limits<uint16_t>::max())
    return false;
  if (_effectInstanceIdWrapped)
    return true;
  return effectInstanceId < _nextEffectInstanceId;
}

void RaceTracker::Clear()
{
  _racers.clear();
  _itemDecks.clear();
  _events.clear();
  _nextItemDeckOid = 1;
  firstPassItemSpawn = true;

  // LOA-fix (R67-5, backlog #128b): КОМАНДНОЕ СОСТОЯНИЕ ТОЖЕ ПЕР-ЗАЕЗДНОЕ.
  //
  // Функция называется `Clear()` и чистила всё, кроме этих двух полей. Очки
  // командного калибра, счётчик рывков и флаг блокировки переезжали в
  // СЛЕДУЮЩИЙ заезд той же комнаты (`RaceInstance` живёт вместе с комнатой),
  // и рематч мог начаться с почти полным калибром — мгновенный спур; с
  // `boostCount`, уже упёртым в верх таблицы `baseFillRates` — максимальная
  // скорость наполнения с первого рывка; или, наоборот, с НАМЕРТВО
  // заблокированным калибром, если прошлый заезд кончился внутри
  // 7-10-секундного окна спура.
  //
  // ★ЭТО ВОЗВРАТ ДРОПНУТОГО R30-4, И ВОЗВРАЩАТЬ ЕГО МОЖНО ТОЛЬКО ВМЕСТЕ С
  // ЭПОХОЙ. В R30 ревью сняло этот сброс отдельным вердиктом — и было право:
  // САМ ПО СЕБЕ он делал хуже. Сброс отдаёт новому заезду свежий незалоченный
  // калибр, а джоб разблокировки из ПРОШЛОГО заезда в этот момент ещё висел в
  // очереди и, выстрелив, снимал блокировку, которую новый заезд поставил
  // ЧЕСТНО, — то есть дарил команде второй спур подряд. Теперь такой джоб
  // отсеивается по несовпадению `RaceInstance::_raceEpoch` (R67-6/R67-7), и
  // сброс стал безопасен. Порядок обязателен: без гардов эпохи этот сброс
  // возвращать НЕЛЬЗЯ.
  //
  // ★ПОЧЕМУ СБРОС ЖИВЁТ ЗДЕСЬ, А ЭПОХА — НА ИНСТАНСЕ. Дефект именно здесь:
  // неполный список полей у функции, которая обязана обнулить трекер целиком
  // ([[total-invariant-beats-list-of-sites]]) — чинить его в вызывающем
  // значило бы оставить `Clear()` по-прежнему лживым. Эпоха же обязана
  // ПЕРЕЖИТЬ трекер, иначе её не с чем сравнивать. Разъехаться они не могут:
  // единственный вызов `Clear()` стоит вплотную перед `RaceInstance::Start()`
  // в `RaceNetworkHandler::HandleStartRace`, и оба идут под одним
  // `_raceInstancesMutex` — тем же, который берут оба джоба.
  blueTeam = TeamInfo{};
  redTeam = TeamInfo{};
}

} // namespace server::tracker
