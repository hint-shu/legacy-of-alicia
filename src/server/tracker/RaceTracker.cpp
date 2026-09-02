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

uint16_t RaceTracker::GetNextEffectInstanceIdAndIncrementBy(
  const Oid casterOid,
  const uint16_t increment)
{
  // LOA-fix (R71-22, находка ревью 3 #5): ОБОРОТА ЗДЕСЬ БОЛЬШЕ НЕ БЫВАЕТ.
  //
  // Флаг «счётчик обернулся» убран вместе с породившей его уступкой: единственный
  // вызывающий обязан спросить `CanIssueEffectInstances`, а тот отказывает, как
  // только выдача переступила бы шестнадцатибитную границу. Молчаливой подгонки
  // (`== 0 -> 1`) тоже не осталось: она существовала только ради жизни ПОСЛЕ
  // оборота и маскировала бы его, случись он.
  const uint16_t nextId = _nextEffectInstanceId;
  _nextEffectInstanceId = static_cast<uint16_t>(_nextEffectInstanceId + increment);

  // LOA-fix (R71-25, находка ревью 4 #2): СПИСАНИЕ С БЮДЖЕТА КАСТЕРА СТОИТ ЗДЕСЬ.
  // Это единственная точка, где номера рождаются, поэтому здесь же они и считаются:
  // разнеси проверку и списание по разным функциям — и они разъедутся при первой же
  // правке ([[total-invariant-beats-list-of-sites]]).
  _issuedEffectInstanceCounts[casterOid] += increment;

  return nextId;
}

bool RaceTracker::CanIssueEffectInstances(
  const Oid casterOid,
  const uint16_t count) const
{
  // LOA-fix (R71-22, находка ревью 3 #5): СУММАРНАЯ ВЫДАЧА, А НЕ ТОЛЬКО ЖИВЫЕ ЗАПИСИ.
  //
  // Первый вопрос — про место под улику (живые записи этого кастера), второй — про
  // сам номер. Второго не было, и это и был дефект: живая запись освобождается по
  // слому стены и по её истечению, поэтому «256 живых на кастера» НЕ ограничивало
  // число ВЫДАННЫХ номеров ничем. Счётчик доходил до оборота, а после оборота
  // предикат «сервер такой номер выдавал» принимал весь домен.
  //
  // ★ГРАНИЦА СТРОГАЯ И БЕЗ ЗАПАСА: последний допустимый номер — 0xFFFE, чтобы
  // `_nextEffectInstanceId` после инкремента остался представимым и НИКОГДА не стал
  // нулём. Один потерянный номер из 65 536 — честная цена за то, что оборот
  // недостижим, а не «маловероятен».
  // LOA-fix (R71-25, находка ревью 4 #2): ПЕРВЫЙ ВОПРОС — ПРО БЮДЖЕТ КАСТЕРА.
  //
  // Он же и единственный ДОСТИЖИМЫЙ: 8 мест в комнате x 512 = 4096 номеров, то есть
  // домен исчерпать нельзя, пока каждый кастер сидит в своём бюджете. Отказ поэтому
  // всегда локален — «у ТЕБЯ кончился», а не «у комнаты кончилось», и честный сосед
  // продолжает кастовать (находка ревью 4 #2: прежний общий запрет был DoS'ом).
  const auto issuedIter = _issuedEffectInstanceCounts.find(casterOid);
  const uint32_t alreadyIssued =
    issuedIter == _issuedEffectInstanceCounts.cend() ? 0u : issuedIter->second;
  if (static_cast<uint64_t>(alreadyIssued) + count > MaxEffectInstanceIssuancePerRacer)
    return false;

  // ★ФЕЙЛ-КЛОУЗ, КОТОРЫЙ НЕДОСТИЖИМ ПО АРИФМЕТИКЕ ВЫШЕ, И ЭТО НАМЕРЕННО: если состав
  // заезда когда-нибудь перестанет быть ограниченным восемью местами, оборот счётчика
  // не должен вернуться тихо. Граница строгая — последний допустимый номер 0xFFFE,
  // чтобы `_nextEffectInstanceId` после инкремента остался представимым и НИКОГДА не
  // стал нулём.
  if (static_cast<uint32_t>(_nextEffectInstanceId) + count
      > std::numeric_limits<uint16_t>::max())
    return false;

  size_t ownedCount = 0;
  for (const auto& instance : _effectInstances)
  {
    if (instance.casterOid == casterOid)
      ++ownedCount;
  }

  return ownedCount + count <= MaxEffectInstancesPerRacer;
}

void RaceTracker::AddEffectInstances(
  const uint16_t firstInstanceId,
  const uint16_t count,
  const uint32_t magicType,
  const Oid casterOid,
  const bool serverApplied,
  const std::vector<Oid>& authorizedTargets)
{
  for (uint16_t i = 0; i < count; ++i)
  {
    const auto instanceId = static_cast<uint16_t>(firstInstanceId + i);

    // LOA-fix (R71-22, находка ревью 3 #5): ЧУЖУЮ ЖИВУЮ ЗАПИСЬ НЕ СТИРАЕМ НИКОГДА.
    //
    // Здесь стоял `RemoveEffectInstance(instanceId)` с доводом «совпадение бывает
    // только после оборота, а тогда старая запись мертва». Довод неверен дважды:
    // после оборота она мертвой быть НЕ ОБЯЗАНА, и сама эта строка была вторым
    // способом уничтожить улику о живой стене — тем самым, который ревью 2 уже
    // запретило в другом месте функции. Оборот теперь запрещён в источнике, поэтому
    // номера внутри заезда уникальны по построению; если совпадение всё-таки
    // случится, побеждает СТАРАЯ запись, а новая не делается.
    if (FindEffectInstance(instanceId) != nullptr)
      continue;

    // LOA-fix (R71-21, находка ревью 2 #3): ВЫТЕСНЕНИЯ НЕТ. Прежняя редакция
    // стирала самую старую запись, чтобы освободить место, — то есть уничтожала
    // улику о ЖИВОЙ стене, и честный слом этой стены после переполнения получал
    // отказ. Место спрашивают ЗАРАНЕЕ (`CanIssueEffectInstances`); если правило
    // всё-таки нарушено, новая запись не делается, но ничего живого не гибнет.
    if (not CanIssueEffectInstances(casterOid, 1))
      return;

    _effectInstances.push_back(
      EffectInstance{
        .instanceId = instanceId,
        .magicType = magicType,
        .casterOid = casterOid,
        .serverApplied = serverApplied,
        .authorizedTargets = authorizedTargets});
  }
}

bool RaceTracker::ConsumeEffectInstanceTarget(
  const uint16_t instanceId,
  const Oid targetOid)
{
  for (auto& instance : _effectInstances)
  {
    if (instance.instanceId != instanceId)
      continue;

    // LOA-fix (R71-22, находка ревью 3 #1): ОДИН ЭКЗЕМПЛЯР — ОДИН ОТЧЁТ НА ЦЕЛЬ.
    // Второй отчёт той же цели по тому же номеру — это либо дубль пакета, либо
    // «переигровка» уже истёкшего эффекта; и то и другое обязано умереть ДО
    // рассылки, иначе один каст размножается в сколько угодно кадров каждому в
    // комнате (`ScheduleSkillEffect` рассылает РАНЬШЕ, чем отвергает дубль).
    for (const Oid consumed : instance.consumedTargets)
    {
      if (consumed == targetOid)
        return false;
    }

    instance.consumedTargets.push_back(targetOid);
    return true;
  }

  return false;
}

const RaceTracker::EffectInstance* RaceTracker::FindEffectInstance(
  const uint16_t instanceId) const
{
  for (const auto& instance : _effectInstances)
  {
    if (instance.instanceId == instanceId)
      return &instance;
  }

  return nullptr;
}

void RaceTracker::MarkEffectInstanceServerApplied(const uint16_t instanceId)
{
  for (auto& instance : _effectInstances)
  {
    if (instance.instanceId == instanceId)
    {
      instance.serverApplied = true;
      return;
    }
  }
}

void RaceTracker::RemoveEffectInstance(const uint16_t instanceId)
{
  std::erase_if(
    _effectInstances,
    [instanceId](const EffectInstance& instance)
    { return instance.instanceId == instanceId; });
}

void RaceTracker::RemoveEffectInstances(
  const uint16_t firstInstanceId,
  const uint16_t count)
{
  for (uint16_t i = 0; i < count; ++i)
    RemoveEffectInstance(static_cast<uint16_t>(firstInstanceId + i));
}

bool RaceTracker::HasIssuedEffectInstanceId(const uint32_t effectInstanceId) const
{
  // Домен идентификатора — шестнадцать бит по построению (`uint16_t` и у счётчика,
  // и у поля в протоколе). Всё, что шире, сервер выдать не мог.
  if (effectInstanceId > std::numeric_limits<uint16_t>::max())
    return false;
  // LOA-fix (R71-22, находка ревью 3 #5): ни одной ветки «после оборота принимаем
  // всё» здесь больше нет — оборот запрещён в источнике выдачи.
  return effectInstanceId < _nextEffectInstanceId;
}

void RaceTracker::Clear()
{
  _racers.clear();
  _itemDecks.clear();
  _events.clear();
  // LOA-fix (R71-17/R71-21): экземпляры эффектов живут РОВНО один заезд — как и
  // гонщики, которые их кастовали. Иначе номер из прошлого заезда остался бы «живым».
  //
  // ★СЧЁТЧИК НОМЕРОВ ЧИСТИТСЯ ЗДЕСЬ ЖЕ, И ЭТО ПОЧИНКА, А НЕ КОСМЕТИКА: он не
  // сбрасывался, а трекер живёт вместе с КОМНАТОЙ — за её жизнь счётчик мог
  // обернуться, после чего `HasIssuedEffectInstanceId` навсегда отвечал «да» на любой
  // номер. Сброс ставит номера в один ряд с `_nextItemDeckOid`, который пер-заездным
  // был всегда; oid'ы персонажей остаются пер-комнатными (клиент их не переназначает).
  _effectInstances.clear();
  _nextEffectInstanceId = 0;
  // LOA-fix (R71-25, находка ревью 4 #2): бюджет выдачи — пер-заездный, как и сам
  // счётчик. Иначе гонщик, выбравший его в первом заезде, остался бы без кастов на
  // все следующие заезды той же комнаты.
  _issuedEffectInstanceCounts.clear();
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
