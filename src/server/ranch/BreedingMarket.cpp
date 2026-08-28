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

#include "server/ranch/BreedingMarket.hpp"

#include "server/ServerInstance.hpp"

#include <libserver/util/QuietLog.hpp>
#include <libserver/util/RecordAccess.hpp>

#include <spdlog/spdlog.h>

// LOA-fix (R63-1, round63, backlog #185): дедлайн предзагрузки рынка ждёт
// временем и уступает процессор на бесплодном круге.
#include <chrono>
#include <thread>

namespace server
{

namespace
{

// todo: configure me
constexpr float RegistrationFee = 0.50f;
constexpr float EarningTaxes = 0.20f;
constexpr size_t MaxStallionsPerCharacter = 3;

constexpr auto MarketDuration = std::chrono::hours(24);

} // anon namespace

BreedingMarket::BreedingMarket(ServerInstance& serverInstance)
  : _serverInstance(serverInstance)
{
}

void BreedingMarket::Initialize()
{
  auto stallionUids = _serverInstance.GetDataDirector().ListRegisteredStallions();

  // LOA-fix (R63-1, round63, backlog #185): у цикла ПОЯВИЛСЯ ВЫХОД ПО НЕУДАЧЕ.
  // Прежде запись жеребца, чью лошадь или владельца загрузить нельзя (удалены),
  // крутилась здесь ВЕЧНО: удаление из списка стоит только на успешной ветке.
  // А `RanchDirector::Initialize` зовёт нас ДО строки «Ranch server listening»,
  // поэтому зависание здесь означает не «рынок без записи», а РАНЧ-СЕРВЕР,
  // КОТОРЫЙ НЕ ПОДНЯЛСЯ. Проверено стендом: контроль встал с 4 слушателями из 5.
  //
  // ★КЛЮЧ К ЧЕСТНОМУ ВЫХОДУ — СПРАШИВАТЬ ОБ ОТКАЗЕ, А НЕ О ВРЕМЕНИ.
  // `Get()` возвращает пусто в ДВУХ разных состояниях: «чтение провалилось» и
  // «чтение ещё в очереди». Выход по одному лишь таймеру их не различает, и на
  // медленном диске или длинной очереди выбросил бы ЗДОРОВЫЕ регистрации —
  // причём стенд с нормальным таймингом этого бы не показал (замечание ревью,
  // итерация 1; принято).
  // Различить их умеет само хранилище: `GetRetrieveFailureCount` > 0 означает
  // ПОДТВЕРЖДЁННЫЙ отказ источника. И, по контракту `DataStorage`, «запись
  // никогда не перезапрашивается сама после неудачи» — то есть ждать её
  // дальше бессмысленно: цикл ждал бы того, что уже окончательно провалилось.
  // ★ВРЕМЯ БОЛЬШЕ НЕ ВЫБРАСЫВАЕТ НИЧЕГО — ЭТО ТРЕБОВАНИЕ РЕВЬЮ (итерации 1 и 2).
  // Первая редакция бросала по таймеру; ревью показало, что улучшенный текст лога
  // инвариант не чинит: «пусто и отказов нет» — это ЗДОРОВАЯ запись в очереди, и
  // на медленном диске её всё равно теряли. Таймер оказался не страховкой, а тем
  // же дефектом с более вежливой формулировкой.
  // ★И он не нужен: исходное зависание (#185) лечится ТОЧНЫМ критерием — записи с
  // подтверждённым отказом снимаются сразу. Состояние ожидания ровно одно и оно
  // всегда разрешается: поток данных либо принесёт запись, либо отчитается об
  // отказе, и тогда она уйдёт по первой ветке.
  // Остаётся только ДИАГНОСТИКА: если ожидание затянулось, говорим об этом вслух —
  // но продолжаем ждать, а не жертвуем чужой регистрацией ради быстрого старта.
  constexpr auto SlowPreloadWarnAfter = std::chrono::seconds(30);
  constexpr auto BarrenPassPause = std::chrono::milliseconds(5);
  const auto preloadStarted = std::chrono::steady_clock::now();
  bool slowPreloadReported = false;

  //! Возвращает true и называет причину, если зависимость ПОДТВЕРЖДЁННО не
  //! читается. Пустой результат без отказов — это «ещё едет», не повод бросать.
  // ★InvalidUid СЧИТАЕТСЯ ОТКАЗОМ СРАЗУ (замечание ревью, итерация 2): такая
  // ссылка не разрешится никогда, и ждать её — то же самое зависание.
  const auto retrievalFailed = [](auto& cache, const data::Uid uid)
  {
    return uid == data::InvalidUid || cache.GetRetrieveFailureCount(uid) > 0;
  };

  size_t removedThisPass = 0;
  auto iterator = stallionUids.begin();
  while (not stallionUids.empty())
  {
    if (iterator == stallionUids.end())
    {
      // Круг пройден. Пока он что-то снимает, список убывает и цикл конечен сам
      // по себе; спать имеет смысл ровно в состоянии «за целый круг ничего», то
      // есть в ожидании потока данных.
      if (removedThisPass == 0)
      {
        if (not slowPreloadReported
            && std::chrono::steady_clock::now() - preloadStarted >= SlowPreloadWarnAfter)
        {
          // ★ГОВОРИМ, НО НЕ БРОСАЕМ. Раз выбрасывать здоровое нельзя, единственное
          // честное действие — сделать затянувшееся ожидание видимым. Молчаливое
          // ожидание неотличимо от зависания, а это ровно то, с чего начался #185.
          slowPreloadReported = true;
          server::util::QuietLogWarn(
            "Breeding market: {} stallion registration(s) are still loading after {} s "
            "and none has reported a failure. The ranch server is still starting up; "
            "if this line repeats on every start, look at the data director",
            stallionUids.size(),
            std::chrono::duration_cast<std::chrono::seconds>(SlowPreloadWarnAfter).count());
        }
        std::this_thread::sleep_for(BarrenPassPause);
      }

      removedThisPass = 0;
      iterator = stallionUids.begin();
    }

    const auto stallionUid = *iterator;
    auto& dataDirector = _serverInstance.GetDataDirector();
    const auto stallionRecord = dataDirector.GetStallionCache().Get(stallionUid);

    if (not stallionRecord)
    {
      // ★Запись самого жеребца подтверждённо не читается — ждать нечего.
      if (retrievalFailed(dataDirector.GetStallionCache(), stallionUid))
      {
        server::util::QuietLogError(
          "Breeding market: stallion registration {} is SKIPPED - its own record "
          "could not be read. The ranch server continues starting up; check for a "
          "damaged stallion file",
          stallionUid);
        iterator = stallionUids.erase(iterator);
        ++removedThisPass;
        continue;
      }
      ++iterator;
      continue;
    }

    auto horseUid = data::InvalidUid;
    auto ownerUid = data::InvalidUid;
    stallionRecord->Immutable([&horseUid, &ownerUid](const data::Stallion& stallion)
    {
      horseUid = stallion.horseUid();
      ownerUid = stallion.ownerUid();
    });

    // Preload horse and character records
    const auto horseRecord = dataDirector.GetHorseCache().Get(horseUid);
    const auto characterRecord = dataDirector.GetCharacterCache().Get(ownerUid);
    if (not horseRecord || not characterRecord)
    {
      // ★ОСИРОТЕВШАЯ ЗАПИСЬ — ровно тот случай, что вешал старт: лошадь или
      // владелец удалены, источник об этом уже отчитался, и по контракту
      // хранилища сам он повторять не станет.
      const bool horseGone = retrievalFailed(dataDirector.GetHorseCache(), horseUid);
      const bool ownerGone = retrievalFailed(dataDirector.GetCharacterCache(), ownerUid);
      if (horseGone || ownerGone)
      {
        // ★Формулировку собираем ЦЕЛОЙ ФРАЗОЙ, а не вставкой в общий хвост:
        // вариант с «neither/nor» вместе с хвостом «could not be read» давал
        // двойное отрицание, то есть строку, читающуюся ровно наоборот.
        server::util::QuietLogError(
          "Breeding market: stallion registration {} is SKIPPED - {} (horse {}, "
          "owner {}). The ranch server continues starting up; check for an "
          "orphaned stallion record",
          stallionUid,
          horseGone && ownerGone ? "neither its horse nor its owner could be read"
            : (horseGone ? "its horse could not be read"
                         : "its owner could not be read"),
          horseUid,
          ownerUid);
        iterator = stallionUids.erase(iterator);
        ++removedThisPass;
        continue;
      }
      ++iterator;
      continue;
    }

    iterator = stallionUids.erase(iterator);
    ++removedThisPass;

    _horseRegistrations.try_emplace(horseUid, Registration{
      .stallionUid = stallionUid});
  }

  ScheduleExpirationCheck();
}

void BreedingMarket::Terminate()
{
  _horseRegistrations.clear();
}

void BreedingMarket::Tick()
{
  // LOA-fix (R53-3, round53, backlog #179 часть 3): постановка задачи в
  // планировщик умеет сорваться, а другого места, где рынок узнал бы об этом,
  // нет. Без повторной попытки жеребцы перестали бы истекать до перезапуска.
  if (not _expirationCheckScheduled)
    ScheduleExpirationCheck();

  _scheduler.Tick();
}

bool BreedingMarket::CanRegisterStallion(const data::Uid characterUid) const noexcept
{
  // ★Вызывается ТОЛЬКО под exclusive-замком `_mutex`: читает карту рынка.
  const auto characterRecord = util::TryGet(
    _serverInstance.GetDataDirector().GetCharacterCache(),
    characterUid,
    "a character registering a stallion");

  if (not characterRecord)
  {
    util::QuietLogWarn(
      "Character '{}' can not register a stallion, their character record is not available",
      characterUid);
    return false;
  }

  // ★Считаем НА МЕСТЕ, а не по копии списка лошадей: копия — это выделение
  // памяти на пути, который обязан не бросать, и ради неё же прежний код
  // держал вектор, который тут вообще не нужен.
  size_t characterStallionCount = 0;
  if (not util::TryImmutable(
    *characterRecord,
    "count the stallions of a character",
    [this, &characterStallionCount](const data::Character& character) noexcept
    {
      for (const data::Uid horseUid : character.horses())
      {
        if (_horseRegistrations.contains(horseUid))
          ++characterStallionCount;
      }
    }))
  {
    return false;
  }

  if (characterStallionCount >= MaxStallionsPerCharacter)
  {
    // LOA-fix (R53-4, round53, backlog #177): отказ регистрации был совершенно
    // молчаливым — игрок видел Cancel без причины, а лог не знал о нём ничего.
    util::QuietLogInfo(
      "Character '{}' can not register another stallion: {} of {} slots are in use",
      characterUid,
      characterStallionCount,
      MaxStallionsPerCharacter);
    return false;
  }

  return true;
}

bool BreedingMarket::HandleRegisterStallion(
  const data::Uid characterUid,
  const data::Uid horseUid,
  const int32_t breedingFee) noexcept
{
  // LOA-fix (R53-5, round53, backlog #179 часть 3): порядок регистрации
  // переставлен так, чтобы отказ ЛЮБОГО шага означал «не произошло ничего».
  //
  //   занять слот -> подготовить (запись, тип, СБОР ПОСЛЕДНИМ) -> опубликовать
  //
  // Прежний порядок снимал сбор ДО того, как регистрация существовала, и при
  // неудаче не возвращал его: игрок платил до 20 000 морковок за Cancel.
  // Прежняя проверка «уже зарегистрирован?» шла под shared-замком, замок
  // отпускался, и вся работа с деньгами шла без него — два одновременных
  // запроса на одну лошадь снимали сбор ДВАЖДЫ. Слот-заглушка занимается под
  // exclusive-замком раньше денег и закрывает обе дыры сразу.
  if (not ClaimRegistrationSlot(characterUid, horseUid))
    return false;

  const auto stallionUid = PrepareRegistration(characterUid, horseUid, breedingFee);
  if (stallionUid == data::InvalidUid)
  {
    ReleaseRegistrationSlot(horseUid);
    return false;
  }

  // ★Оплата и публикация — ОДНИМ шагом под одним замком. Порознь между ними
  // оставалось бы окно «деньги сняты, регистрации нет», и закрывать его
  // пришлось бы возвратом — компенсацией, которая сама умеет не сработать
  // (ревью Codex, итерация 1, находка 3).
  if (not CommitRegistration(characterUid, horseUid, stallionUid, breedingFee))
  {
    TakeStallionOffTheMarket(horseUid, stallionUid);
    ReleaseRegistrationSlot(horseUid);
    return false;
  }

  return true;
}

bool BreedingMarket::HandleUnregisterStallion(
  const data::Uid characterUid,
  const data::Uid horseUid) noexcept
{
  try
  {
    const std::scoped_lock lock(_mutex);

    const auto stallionIterator = _horseRegistrations.find(horseUid);

    // If the horse is not a registered stallion do an early return.
    if (stallionIterator == _horseRegistrations.end())
      return false;

    const auto stallionUid = stallionIterator->second.stallionUid;

    const auto characterRecord = util::TryGet(
      _serverInstance.GetDataDirector().GetCharacterCache(),
      characterUid,
      "a character unregistering a stallion");

    if (not characterRecord)
      return false;

    // Check if the character can unregister the stallion.
    bool canUnregisterStallion = false;
    if (not util::TryImmutable(
      *characterRecord,
      "check the ownership of a stallion",
      [&canUnregisterStallion, horseUid](const data::Character& character) noexcept
      {
        canUnregisterStallion = std::ranges::contains(character.horses(), horseUid);
      }))
    {
      return false;
    }

    if (not canUnregisterStallion)
      return false;

    // ★Регистрация стирается ТОЛЬКО если снятие состоялось. Иначе жеребец
    // остаётся на рынке со своим долгом и попытку можно повторить: потерять
    // запись о долге страшнее, чем отказать игроку в снятии.
    if (not UnregisterStallion(horseUid, stallionUid))
      return false;

    _horseRegistrations.erase(stallionIterator);

    return true;
  }
  catch (const std::exception& x)
  {
    util::QuietLogError(
      "Character '{}' could not unregister the stallion of horse '{}': {}",
      characterUid,
      horseUid,
      x.what());
  }
  catch (...)
  {
    util::QuietLogError(
      "Character '{}' could not unregister the stallion of horse '{}': unknown exception",
      characterUid,
      horseUid);
  }

  return false;
}

std::optional<BreedingMarket::Earnings> BreedingMarket::CalculateUnregisterEarnings(
  const data::Uid horseUid) const noexcept
{
  // LOA-fix (R53-7, round53, backlog #179 часть 3): отказ читается как «оценка
  // недоступна» — клиент получит Cancel на окно оценки. Направление безопасное:
  // ни одна морковка по этому пути не двигается.
  data::Uid stallionUid = data::InvalidUid;

  if (not util::TryShared(_mutex, [&]() noexcept
    {
      // If the horse is not a registered stallion do an early return.
      const auto horseIterator = _horseRegistrations.find(horseUid);
      if (horseIterator != _horseRegistrations.end())
        stallionUid = horseIterator->second.stallionUid;
    }))
  {
    return std::nullopt;
  }

  if (stallionUid == data::InvalidUid)
    return std::nullopt;

  auto& dataDirector = _serverInstance.GetDataDirector();
  const auto stallionRecord = util::TryGet(
    dataDirector.GetStallionCache(), stallionUid, "a stallion being estimated");
  const auto horseRecord = util::TryGet(
    dataDirector.GetHorseCache(), horseUid, "a horse being estimated");

  // If the stallion record or the horse record are not available do an early return.
  if (not stallionRecord || not horseRecord)
    return std::nullopt;

  // Populate the earnings.
  Earnings earnings{
    .taxRate = EarningTaxes};
  if (not util::TryImmutable(
    *stallionRecord,
    "read the breeding earnings estimate",
    [&earnings](const data::Stallion& stallion) noexcept
    {
      earnings.timesMated = stallion.timesMated();
      earnings.breedingFee = stallion.breedingCharge();
    }))
  {
    return std::nullopt;
  }

  earnings.revenue = earnings.timesMated * earnings.breedingFee;

  return earnings;
}

std::optional<BreedingMarket::StallionData> BreedingMarket::GetStallionData(
  const data::Uid horseUid) const noexcept
{
  // LOA-fix (R53-8, round53, backlog #179 часть 3): это путь ПОКУПАТЕЛЯ —
  // случка спрашивает отсюда цену. Отказ читается как «жеребец не выставлен»:
  // покупатель получит отказ ДО списания платы, то есть ничьи деньги не
  // двигаются. Обратное решение (считать цену нулём или прежней) означало бы
  // случку за чужой счёт.
  data::Uid stallionUid = data::InvalidUid;

  if (not util::TryShared(_mutex, [&]() noexcept
    {
      // If the horse is not a registered stallion do an early return.
      const auto horseIterator = _horseRegistrations.find(horseUid);
      if (horseIterator != _horseRegistrations.end())
        stallionUid = horseIterator->second.stallionUid;
    }))
  {
    return std::nullopt;
  }

  if (stallionUid == data::InvalidUid)
    return std::nullopt;

  const auto stallionRecord = util::TryGet(
    _serverInstance.GetDataDirector().GetStallionCache(),
    stallionUid,
    "a stallion being bred with");

  if (not stallionRecord)
    return std::nullopt;

  StallionData data;
  data.stallionUid = stallionUid;
  if (not util::TryImmutable(
    *stallionRecord,
    "read the breeding charge of a stallion",
    [&data](const data::Stallion& stallion) noexcept
    {
      data.breedingCharge = stallion.breedingCharge();
    }))
  {
    return std::nullopt;
  }

  return data;
}

bool BreedingMarket::IsRegistered(
  const data::Uid horseUid) const noexcept
{
  // LOA-fix (R53-9, round53, backlog #179 часть 3): взятие замка объявлено
  // бросающим и здесь не было накрыто ничем. Отказ читается как «не на рынке» —
  // лошадь просто не покажется выставленной.
  bool isRegistered = false;
  if (not util::TryShared(_mutex, [&]() noexcept
    {
      // ★Занятый, но ещё не заполненный слот (заглушка) выставленным жеребцом
      // НЕ считается. Иначе чужой игрок в те же миллисекунды увидел бы лошадь
      // «на рынке», спросил бы её данные, не получил их — и обработчик витрины
      // бросил бы `runtime_error` (RanchDirector: «registered stallion but no
      // stallion data»). Ревью Codex, итерация 1, находка 4.
      const auto horseIterator = _horseRegistrations.find(horseUid);
      isRegistered = horseIterator != _horseRegistrations.end()
        && horseIterator->second.stallionUid != data::InvalidUid;
    }))
  {
    return false;
  }

  return isRegistered;
}

int32_t BreedingMarket::CalculateRegistrationFee(const int32_t breedingFee) const noexcept
{
  return static_cast<int32_t>(std::ceilf(static_cast<float>(breedingFee) * RegistrationFee));
}

std::optional<BreedingMarket::GradeFeeRange> BreedingMarket::GetGradeFeeRange(
  const uint32_t grade) const noexcept
{
  switch (grade)
  {
    case 4: return GradeFeeRange{4000u, 12000u};
    case 5: return GradeFeeRange{5000u, 15000u};
    case 6: return GradeFeeRange{6000u, 18000u};
    case 7: return GradeFeeRange{8000u, 24000u};
    case 8: return GradeFeeRange{10000u, 40000u};
    default: return std::nullopt;
  }
}

BreedingMarket::Snapshot BreedingMarket::CollectMarketSnapshot(
  const SnapshotOrder order,
  const SnapshotFilter filter) const noexcept
{
  // LOA-fix (R53-10, round53, backlog #179 часть 3): сбор витрины выделяет
  // память на каждом шаге — вектор регистраций, узлы кэша записей, замки, — то
  // есть бросает. Функция обещала обратное, и первый же отказ убивал процесс.
  // Теперь отказ означает ПУСТУЮ витрину: игрок увидит пустой список
  // жеребцов, а не разрыв связи.
  try
  {
    return CollectMarketSnapshotUnsafe(order, filter);
  }
  catch (const std::exception& x)
  {
    util::QuietLogError("Collecting the breeding market snapshot failed: {}", x.what());
  }
  catch (...)
  {
    util::QuietLogError("Collecting the breeding market snapshot failed: unknown exception");
  }

  return {};
}

BreedingMarket::Snapshot BreedingMarket::CollectMarketSnapshotUnsafe(
  const SnapshotOrder order,
  const SnapshotFilter& filter) const
{
  std::shared_lock lock(_mutex);

  Snapshot snapshot{};

  // Filter the horse registrations.
  for (const auto [horseUid, registration] : _horseRegistrations)
  {
    // ★Записи берутся ЧЕРЕЗ ХРАНИЛИЩЕ, а не через `GetStallion`/`GetHorse`: те
    // объявлены `noexcept`, но внутри выделяют память, поэтому бросок в них
    // убивает процесс ДО возврата сюда — обрамляющий пояс был бы бесполезен.
    const auto stallionRecord = _serverInstance.GetDataDirector().GetStallionCache().Get(
      registration.stallionUid);
    if (not stallionRecord)
      continue;

    const auto horseRecord = _serverInstance.GetDataDirector().GetHorseCache().Get(
      horseUid);
    if (not horseRecord)
      continue;

    const auto& registry = _serverInstance.GetHorseRegistry();

    bool isMatch = true;
    horseRecord->Immutable([&filter, &isMatch, &registry](const data::Horse& horse)
    {
      if (filter.grade != 0 && static_cast<uint8_t>(horse.grade()) != filter.grade)
        isMatch = false;
      if (!filter.coats.empty() && !filter.coats.contains(horse.parts.skinTid()))
        isMatch = false;
      // The mane and tail filters are keyed by shape, not by the mane/tail TID.
      if (!filter.manes.empty()
        && !filter.manes.contains(registry.GetMane(horse.parts.maneTid()).shape))
        isMatch = false;
      if (!filter.tails.empty()
        && !filter.tails.contains(registry.GetTail(horse.parts.tailTid()).shape))
        isMatch = false;

      if (filter.firstPreferredStat != SnapshotFilter::Stat::None
        || filter.secondPreferred != SnapshotFilter::Stat::None)
      {
        constexpr uint32_t RequiredSharePercentPerStat = 38u;

        const uint32_t totalStats = horse.stats.agility()
          + horse.stats.courage()
          + horse.stats.rush()
          + horse.stats.endurance()
          + horse.stats.ambition();

        const auto statValue = [&horse](const SnapshotFilter::Stat stat) -> uint32_t
        {
          switch (stat)
          {
            case SnapshotFilter::Stat::Agility: return horse.stats.agility();
            case SnapshotFilter::Stat::Ambition: return horse.stats.ambition();
            case SnapshotFilter::Stat::Rush: return horse.stats.rush();
            case SnapshotFilter::Stat::Endurance: return horse.stats.endurance();
            case SnapshotFilter::Stat::Courage: return horse.stats.courage();
            default: return 0u;
          }
        };

        // The required share for a stat accounts for both slots, so selecting
        // the same stat twice doubles its threshold.
        const auto requiredSharePercent = [&filter](const SnapshotFilter::Stat stat) -> uint32_t
        {
          uint32_t percent = 0u;
          if (filter.firstPreferredStat == stat)
            percent += RequiredSharePercentPerStat;
          if (filter.secondPreferred == stat)
            percent += RequiredSharePercentPerStat;
          return percent;
        };

        const auto meetsShare = [&](const SnapshotFilter::Stat stat) -> bool
        {
          if (stat == SnapshotFilter::Stat::None)
            return true;
          return statValue(stat) * 100u >= requiredSharePercent(stat) * totalStats;
        };

        if (!meetsShare(filter.firstPreferredStat)
          || !meetsShare(filter.secondPreferred))
          isMatch = false;
      }
    });

    if (isMatch)
    {
      snapshot.registrations.emplace_back(Snapshot::Registration{
        .horseUid = horseUid,
        .stallionUid = registration.stallionUid});
    }
  }

  std::ranges::sort(
    snapshot.registrations,
    [order, this](
    const Snapshot::Registration firstRegistration,
    const Snapshot::Registration secondRegistration) -> bool
    {
      // ★То же правило, что в цикле выше: только хранилище, не обёртки.
      const auto firstHorseRecord = _serverInstance.GetDataDirector().GetHorseCache().Get(
        firstRegistration.horseUid);
      const auto firstStallionRecord = _serverInstance.GetDataDirector().GetStallionCache().Get(
        firstRegistration.stallionUid);

      if (!firstHorseRecord || !firstStallionRecord)
        return false;

      const auto secondHorseRecord = _serverInstance.GetDataDirector().GetHorseCache().Get(
        secondRegistration.horseUid);
      const auto secondStallionRecord = _serverInstance.GetDataDirector().GetStallionCache().Get(
        secondRegistration.stallionUid);

      if (!secondHorseRecord || !secondStallionRecord)
        return true;

      // Sort to order by lineage.
      if (order == SnapshotOrder::LineageAscending || order == SnapshotOrder::LineageDescending)
      {
        size_t firstLineage{};
        size_t secondLineage{};

        firstHorseRecord->Immutable([&firstLineage](
          const data::Horse& horse)
        {
          firstLineage = horse.lineage();
        });

        secondHorseRecord->Immutable([&secondLineage](
          const data::Horse& horse)
        {
          secondLineage = horse.lineage();
        });

        // If the sort order is descending, the greater lineage should appear first.
        // Otherwise, the lesser lineage should appear first.
        return order == SnapshotOrder::LineageDescending
          ? firstLineage > secondLineage
          : firstLineage < secondLineage;
      }

      if (order == SnapshotOrder::TimeLeftAscending || order == SnapshotOrder::TimeLeftDescending)
      {
        data::Clock::time_point firstExpiresAt{};
        data::Clock::time_point secondExpiresAt{};

        firstStallionRecord->Immutable([&firstExpiresAt](
          const data::Stallion& horse)
        {
          firstExpiresAt = horse.expiresAt();
        });

        secondStallionRecord->Immutable([&secondExpiresAt](
          const data::Stallion& horse)
        {
          secondExpiresAt = horse.expiresAt();
        });

        // If the sort order is descending, the expiration time further in the future should appear first.
        // Otherwise, the expiration time sooner in future should appear first.
        return order == SnapshotOrder::TimeLeftDescending
          ? firstExpiresAt > secondExpiresAt
          : firstExpiresAt < secondExpiresAt;
      }

      if (order == SnapshotOrder::FeeAscending || order == SnapshotOrder::FeeDescending)
      {
        size_t firstFee{};
        size_t secondFee{};

        firstStallionRecord->Immutable([&firstFee](
          const data::Stallion& horse)
        {
          firstFee = horse.breedingCharge();
        });

        secondStallionRecord->Immutable([&secondFee](
          const data::Stallion& horse)
        {
          secondFee = horse.breedingCharge();
        });

        // If the sort order is descending, the greater breeding fee should appear first.
        // Otherwise, the lesser breeding fee should appear first.
        return order == SnapshotOrder::FeeDescending
          ? firstFee > secondFee
          : firstFee < secondFee;
      }

      if (order == SnapshotOrder::PregnancyChanceAscending
        || order == SnapshotOrder::PregnancyChanceDescending)
      {
        size_t firstBreedingCount{};
        size_t secondBreedingCount{};

        firstHorseRecord->Immutable([&firstBreedingCount](
          const data::Horse& horse)
        {
          firstBreedingCount = horse.breedingCount();
        });

        secondHorseRecord->Immutable([&secondBreedingCount](
          const data::Horse& horse)
        {
          secondBreedingCount = horse.breedingCount();
        });

        return order == SnapshotOrder::PregnancyChanceDescending
          ? firstBreedingCount < secondBreedingCount
          : firstBreedingCount > secondBreedingCount;
      }

      return true;
    });

  return snapshot;
}

bool BreedingMarket::ClaimRegistrationSlot(
  const data::Uid characterUid,
  const data::Uid horseUid) noexcept
{
  bool claimed = false;

  if (not util::TryLocked(_mutex, [&]() noexcept
    {
      // If the horse is a registered stallion do an early return.
      if (_horseRegistrations.contains(horseUid))
        return;

      if (not CanRegisterStallion(characterUid))
        return;

      claimed = util::TryClaimSlot(_horseRegistrations, horseUid);
    }))
  {
    return false;
  }

  if (not claimed)
    return false;

  return true;
}

void BreedingMarket::ReleaseRegistrationSlot(const data::Uid horseUid) noexcept
{
  if (not util::TryLocked(_mutex, [&]() noexcept
    {
      _horseRegistrations.erase(horseUid);
    }))
  {
    // Заглушка доживёт до перезапуска: карта рынка строится из записей
    // жеребцов, а записи у неё нет, поэтому лошадь до перезапуска не выставить.
    // Деньги при этом не потеряны — а это главное.
    util::QuietLogError(
      "The market slot of horse '{}' could not be released", horseUid);
  }
}

data::Uid BreedingMarket::PrepareRegistration(
  const data::Uid characterUid,
  const data::Uid horseUid,
  const int32_t breedingFee) const noexcept
{
  auto& dataDirector = _serverInstance.GetDataDirector();

  // ★Записи берутся ЧЕРЕЗ ХРАНИЛИЩЕ, а не через `GetCharacter`/`GetHorse`: те
  // объявлены `noexcept`, но внутри выделяют память, и бросок в них убивает
  // процесс ДО возврата сюда — пояс на нашей стороне был бы бесполезен.
  const auto characterRecord = util::TryGet(
    dataDirector.GetCharacterCache(), characterUid, "a character registering a stallion");
  const auto horseRecord = util::TryGet(
    dataDirector.GetHorseCache(), horseUid, "a horse being registered as a stallion");

  if (not characterRecord || not horseRecord)
    return data::InvalidUid;

  // Check if the character can register the stallion.
  bool ownsTheHorse = false;
  if (not util::TryImmutable(
    *characterRecord,
    "check the ownership of a horse",
    [&ownsTheHorse, horseUid](const data::Character& character) noexcept
    {
      ownsTheHorse = std::ranges::contains(character.horses(), horseUid);
    }))
  {
    return data::InvalidUid;
  }

  if (not ownsTheHorse)
  {
    util::QuietLogWarn(
      "Character '{}' tried to register another character's horse '{}' as a stallion",
      characterUid,
      horseUid);
    return data::InvalidUid;
  }

  // Get the horse grade.
  uint32_t horseGrade = 0;
  if (not util::TryImmutable(
    *horseRecord,
    "read the grade of a horse",
    [&horseGrade](const data::Horse& horse) noexcept
    {
      horseGrade = horse.grade();
    }))
  {
    return data::InvalidUid;
  }

  const auto gradeFeeRange = GetGradeFeeRange(horseGrade);
  if (not gradeFeeRange)
  {
    // LOA-fix (R53-11, round53, backlog #177): отказ по классу лошади был
    // молчаливым — игрок видел Cancel без причины, и в логе не было ничего.
    util::QuietLogInfo(
      "Character '{}' can not register horse '{}' as a stallion: "
      "grade {} is not allowed to breed",
      characterUid,
      horseUid,
      horseGrade);
    return data::InvalidUid;
  }

  // Validate the breeding fee according to grade fee range.
  if (breedingFee < gradeFeeRange->min || breedingFee > gradeFeeRange->max)
  {
    util::QuietLogInfo(
      "Character '{}' can not register horse '{}' as a stallion: breeding fee {} "
      "is outside the range [{}, {}] of grade {}",
      characterUid,
      horseUid,
      breedingFee,
      gradeFeeRange->min,
      gradeFeeRange->max,
      horseGrade);
    return data::InvalidUid;
  }

  // Create the stallion record.
  //
  // ★Проверка записи ОБЯЗАТЕЛЬНА и была главной дырой этой подсистемы:
  // `CreateStallion` штатно отдаёт ПУСТУЮ запись при сбое источника данных (её
  // собственный пояс так и задуман, и соседний `RewardSystem` эту запись
  // проверяет), а `Record::Mutable` на пустой записи БРОСАЕТ. Из noexcept-
  // функции это было `std::terminate` — сервер целиком, по нажатию кнопки.
  const auto stallionRecord = dataDirector.CreateStallion();
  if (not stallionRecord)
  {
    util::QuietLogError(
      "Character '{}' can not register horse '{}' as a stallion: "
      "the stallion record could not be created",
      characterUid,
      horseUid);
    return data::InvalidUid;
  }

  // UID нужен ДО наполнения: без него запись, которую не удалось наполнить,
  // нельзя было бы даже удалить — она осталась бы на диске и воскресала бы на
  // рынке при каждом старте.
  data::Uid stallionUid = data::InvalidUid;
  if (not util::TryImmutable(
    stallionRecord,
    "read the uid of a new stallion record",
    [&stallionUid](const data::Stallion& stallion) noexcept
    {
      stallionUid = stallion.uid();
    }))
  {
    util::QuietLogError(
      "The stallion record created for horse '{}' could not be read; "
      "the record is left orphaned",
      horseUid);
    return data::InvalidUid;
  }

  const auto registeredAt = util::Clock::now();
  const auto filled = util::TryMutate(
    stallionRecord,
    "fill a new stallion record",
    [&](data::Stallion& stallion) noexcept
    {
      stallion.horseUid() = horseUid;
      stallion.ownerUid() = characterUid;
      stallion.breedingCharge() = breedingFee;
      stallion.registeredAt() = registeredAt;

      // todo: this should be configurable if the client supports it.
      stallion.expiresAt() = registeredAt + MarketDuration;
    });

  if (filled == util::MutateOutcome::NotApplied)
  {
    TakeStallionOffTheMarket(horseUid, stallionUid);
    return data::InvalidUid;
  }
  if (filled == util::MutateOutcome::AppliedNotPersisted)
    (void)util::TrySave(dataDirector.GetStallionCache(), stallionUid, "a new stallion record");

  // Set horse type to Stallion.
  const auto typed = util::TryMutate(
    *horseRecord,
    "mark a horse as a stallion",
    [](data::Horse& horse) noexcept
    {
      horse.type() = data::Horse::Type::Stallion;
    });

  if (typed == util::MutateOutcome::NotApplied)
  {
    TakeStallionOffTheMarket(horseUid, stallionUid);
    return data::InvalidUid;
  }
  if (typed == util::MutateOutcome::AppliedNotPersisted)
    (void)util::TrySave(dataDirector.GetHorseCache(), horseUid, "a horse marked as a stallion");

  // Деньги здесь НЕ трогаются: сбор снимает `CommitRegistration` — вместе с
  // публикацией и под одним замком.
  return stallionUid;
}

bool BreedingMarket::CommitRegistration(
  const data::Uid characterUid,
  const data::Uid horseUid,
  const data::Uid stallionUid,
  const int32_t breedingFee) noexcept
{
  const auto registrationFee = CalculateRegistrationFee(breedingFee);

  auto& dataDirector = _serverInstance.GetDataDirector();
  const auto characterRecord = util::TryGet(
    dataDirector.GetCharacterCache(), characterUid, "a character paying for a stallion");

  if (not characterRecord)
    return false;

  bool charged = false;
  bool published = false;
  auto chargeOutcome = util::MutateOutcome::NotApplied;

  // ★ОДИН ЗАМОК на оплату и публикацию. Публикация — запись в УЖЕ созданный
  // узел карты, памяти не требует и сорваться не может, поэтому состояние
  // «деньги сняты, регистрации нет» недостижимо ПО ПОСТРОЕНИЮ, а не благодаря
  // возврату, который сам умеет не сработать.
  if (not util::TryLocked(_mutex, [&]() noexcept
    {
      const auto slotIterator = _horseRegistrations.find(horseUid);
      if (slotIterator == _horseRegistrations.end())
        return;

      chargeOutcome = util::TryMutate(
        *characterRecord,
        "charge the stallion registration fee",
        [&charged, registrationFee](data::Character& character) noexcept
        {
          if (character.carrots() < registrationFee)
            return;

          character.carrots() -= registrationFee;
          charged = true;
        });

      if (not charged)
        return;

      slotIterator->second.stallionUid = stallionUid;
      published = true;
    }))
  {
    return false;
  }

  if (not charged)
  {
    if (chargeOutcome != util::MutateOutcome::NotApplied)
    {
      // LOA-fix (R53, backlog #177): нехватка морковок тоже была молчаливой.
      util::QuietLogInfo(
        "Character '{}' can not register horse '{}' as a stallion: "
        "the registration fee of {} carrots is not affordable",
        characterUid,
        horseUid,
        registrationFee);
    }
    return false;
  }

  if (chargeOutcome == util::MutateOutcome::AppliedNotPersisted)
  {
    (void)util::TrySave(
      dataDirector.GetCharacterCache(), characterUid, "a charged character");
  }

  return published;
}

void BreedingMarket::TakeStallionOffTheMarket(
  const data::Uid horseUid,
  const data::Uid stallionUid) const noexcept
{
  auto& dataDirector = _serverInstance.GetDataDirector();

  const auto horseRecord = util::TryGet(
    dataDirector.GetHorseCache(), horseUid, "a horse leaving the breeding market");

  if (horseRecord)
  {
    const auto outcome = util::TryMutate(
      *horseRecord,
      "return a horse to adult",
      [](data::Horse& horse) noexcept
      {
        horse.type() = data::Horse::Type::Adult;
      });

    if (outcome == util::MutateOutcome::AppliedNotPersisted)
    {
      (void)util::TrySave(
        dataDirector.GetHorseCache(), horseUid, "a horse returned to adult");
    }
  }

  if (stallionUid == data::InvalidUid)
    return;

  // Delete the stallion record.
  if (not util::TryDelete(
    dataDirector.GetStallionCache(), stallionUid, "a stallion record"))
  {
    util::QuietLogError(
      "The stallion record '{}' of horse '{}' could not be deleted; the record is "
      "left orphaned and will reappear on the market after a restart",
      stallionUid,
      horseUid);
  }
}

BreedingMarket::PayoutResult BreedingMarket::CreateBreedingPayout(
  const data::Uid ownerUid,
  const data::Uid horseUid,
  const data::Uid stallionUid,
  Earnings& earnings) const noexcept
{
  auto result = PayoutResult::Paid;

  if (earnings.timesMated > 0)
  {
    // ★Пока не доказано обратное, считаем исход НЕИЗВЕСТНЫМ: `CreateReward`
    // умеет бросить УЖЕ ПОСЛЕ того, как заявка заполнена (просьба сохранить
    // стоит последней строкой `Record::Mutable`). Возврат долга в таком
    // случае означал бы вторую заявку при повторном снятии — чеканку.
    // (Ревью Codex, итерация 1, находка 1.)
    result = PayoutResult::Unknown;

    try
    {
      // Register payout in the RewardSystem
      earnings.claimUid = _serverInstance.GetRewardSystem().CreateReward(
        ownerUid,
        data::Reward::Type::Breeding,
        earnings.earnings);

      // ЧЕСТНЫЙ возврат: заявки точно нет, долг можно вернуть и повторить.
      result = earnings.claimUid != data::InvalidUid
        ? PayoutResult::Paid
        : PayoutResult::NotCreated;
    }
    catch (const std::exception& x)
    {
      util::QuietLogError(
        "Exception while paying out the breeding earnings of stallion '{}' (horse '{}') "
        "to character '{}': {}",
        stallionUid,
        horseUid,
        ownerUid,
        x.what());
    }
    catch (...)
    {
      util::QuietLogError(
        "Unknown exception while paying out the breeding earnings of stallion '{}' "
        "(horse '{}')",
        stallionUid,
        horseUid);
    }

    if (result != PayoutResult::Paid)
    {
      // Отказ выплаты назван ТОЧНО — с суммой и числом случек.
      util::QuietLogError(
        "Breeding earnings of stallion '{}' (horse '{}') were NOT paid out to "
        "character '{}': {} carrots for {} matings ({})",
        stallionUid,
        horseUid,
        ownerUid,
        earnings.earnings,
        earnings.timesMated,
        result == PayoutResult::NotCreated
          ? "no claim was created, the debt is restored"
          : "the claim may or may not exist, the debt is NOT restored");
    }
  }

  return result;
}

void BreedingMarket::SendBreedingPayoutMail(
  const data::Uid ownerUid,
  const data::Uid horseUid,
  const data::Uid stallionUid,
  const Earnings& earnings) const noexcept
{
  try
  {
    // Send mail with payout information
    _serverInstance.GetMessengerDirector().SendStallionReward(
      ownerUid,
      horseUid,
      earnings);
  }
  catch (const std::exception& x)
  {
    util::QuietLogWarn(
      "The breeding payout mail for stallion '{}' (horse '{}') was not sent to "
      "character '{}': {}",
      stallionUid,
      horseUid,
      ownerUid,
      x.what());
  }
  catch (...)
  {
    util::QuietLogWarn(
      "The breeding payout mail for stallion '{}' (horse '{}') was not sent to "
      "character '{}': unknown exception",
      stallionUid,
      horseUid,
      ownerUid);
  }
}

bool BreedingMarket::UnregisterStallion(
  const data::Uid horseUid,
  const data::Uid stallionUid) const noexcept
{
  // LOA-fix (R53-12, round53, backlog #179 часть 3): порядок снятия с рынка
  // переставлен, и это главная денежная правка раунда.
  //
  // Как было: заработок выплачивался ПЕРВЫМ, а при сбое выплаты (её ловил мой
  // же пояс R49-7d) выполнение шло дальше — лошадь возвращалась из рынка, а
  // запись жеребца УДАЛЯЛАСЬ вместе со счётчиком `timesMated`. То есть деньги,
  // накопленные с ЧУЖИХ игроков, исчезали молча и безвозвратно.
  //
  // Как стало:
  //   1) создать заявку на выплату и погасить долг ОДНИМ изменением записи:
  //      долг гасится, только если заявка не «точно не создана». Не вышло —
  //      не делаем НИЧЕГО: жеребец остаётся на рынке со своим долгом, и
  //      попытку можно повторить;
  //   2) письмо владельцу — уже вне изменения записи;
  //   3) и только потом снять с рынка.
  //
  // ★Почему заявка и гашение — ОДНО действие, а не два. Двойную выплату
  // убивает гашение долга: после него любое повторное снятие начислит ноль,
  // включая воскресшую после перезапуска запись. Но если гасить ОТДЕЛЬНО от
  // выплаты, между ними возникает окно «долг погашен, заявки нет», и закрывать
  // его пришлось бы восстановлением долга — то есть компенсацией, которая сама
  // умеет не сработать. Внутри одного изменения такого окна не существует.
  auto& dataDirector = _serverInstance.GetDataDirector();

  const auto stallionRecord = util::TryGet(
    dataDirector.GetStallionCache(), stallionUid, "a stallion leaving the market");

  if (not stallionRecord)
  {
    util::QuietLogWarn(
      "Not unregistering stallion '{}' (horse '{}'), "
      "the stallion record is not available",
      stallionUid,
      horseUid);
    return false;
  }

  // Populate the earnings.
  Earnings earnings{
    .taxRate = EarningTaxes};

  data::Uid ownerUid{data::InvalidUid};
  if (not util::TryImmutable(
    *stallionRecord,
    "read the breeding earnings",
    [&earnings, &ownerUid](const data::Stallion& stallion) noexcept
    {
      ownerUid = stallion.ownerUid();

      earnings.timesMated = stallion.timesMated();
      earnings.breedingFee = stallion.breedingCharge();
    }))
  {
    return false;
  }

  earnings.revenue = earnings.timesMated * earnings.breedingFee;
  earnings.earnings = earnings.revenue - static_cast<uint32_t>(
    static_cast<float>(earnings.revenue) * earnings.taxRate);

  // ★ШАГ 1. Заявка на выплату создаётся ВНУТРИ ТОГО ЖЕ изменения записи,
  // которое гасит долг, и долг гасится ТОЛЬКО если заявка не «точно не
  // создана». Так окно между гашением и выплатой исчезает целиком — раньше на
  // его месте стояло восстановление долга, то есть компенсация, которая сама
  // умеет не сработать (конструкцию подсказало ревью, итерация 2).
  //
  // Разбор всех исходов:
  //   NotApplied — тело не выполнялось: ни заявки, ни гашения, долг цел;
  //   NotCreated — заявки ТОЧНО нет, счётчик не тронут, долг цел;
  //   Paid       — заявка есть, долг погашен;
  //   Unknown    — заявка МОЖЕТ существовать (бросок из `CreateReward` уже
  //                после её заполнения). Долг гасим: дубль хуже потери. Это
  //                единственный остаток, и он неустраним без идемпотентной
  //                выплаты или журнала выдач (#167).
  auto payout = PayoutResult::NotCreated;
  const auto settled = util::TryMutate(
    *stallionRecord,
    "settle the breeding debt",
    [&](data::Stallion& stallion) noexcept
    {
      payout = CreateBreedingPayout(ownerUid, horseUid, stallionUid, earnings);
      if (payout == PayoutResult::NotCreated)
        return;

      stallion.timesMated() = 0;
    });

  if (settled == util::MutateOutcome::NotApplied || payout == PayoutResult::NotCreated)
  {
    util::QuietLogError(
      "Not unregistering stallion '{}' (horse '{}'): {}. The stallion stays on the "
      "market with its debt of {} matings intact and the attempt can be repeated",
      stallionUid,
      horseUid,
      settled == util::MutateOutcome::NotApplied
        ? "the stallion record could not be entered"
        : "the payout claim was not created",
      earnings.timesMated);
    return false;
  }

  if (settled == util::MutateOutcome::AppliedNotPersisted)
    (void)util::TrySave(dataDirector.GetStallionCache(), stallionUid, "a settled stallion");

  // ШАГ 2. Письмо — уже ВНЕ изменения записи: почта ходит к сессиям игроков, и
  // держать под ней замок записи жеребца незачем.
  SendBreedingPayoutMail(ownerUid, horseUid, stallionUid, earnings);

  // ШАГ 3. Снять с рынка.
  TakeStallionOffTheMarket(horseUid, stallionUid);

  return true;
}

void BreedingMarket::ScheduleExpirationCheck() noexcept
{
  try
  {
    _scheduler.Queue(
      [this]()
      {
        RunExpirationCheck();
        ScheduleExpirationCheck();
      },
      Scheduler::Clock::now() + std::chrono::seconds(60));

    _expirationCheckScheduled = true;
    return;
  }
  catch (const std::exception& x)
  {
    util::QuietLogError(
      "The breeding market expiration check could not be scheduled: {}", x.what());
  }
  catch (...)
  {
    util::QuietLogError(
      "The breeding market expiration check could not be scheduled: unknown exception");
  }

  // ★Признак снят: `Tick` попробует поставить задачу снова. Без этого одна
  // сорвавшаяся постановка означала бы, что рынок не истекает НИКОГДА и все
  // выставленные жеребцы висят в списке бесплатно до перезапуска.
  _expirationCheckScheduled = false;
}

void BreedingMarket::RunExpirationCheck() noexcept
{
  struct Entry
  {
    data::Uid horseUid{data::InvalidUid};
    data::Uid stallionUid{data::InvalidUid};
  };

  std::vector<Entry> expiredHorseUids;

  // Collect expired horse stallion registrations.
  (void)util::TryShared(_mutex, [&]() noexcept
    {
      for (const auto& [horseUid, registration] : _horseRegistrations)
      {
        // ★Только хранилище: `GetStallion` объявлен noexcept и внутри бросает.
        const auto stallionRecord = util::TryGet(
          _serverInstance.GetDataDirector().GetStallionCache(),
          registration.stallionUid,
          "a stallion being checked for expiration");

        if (not stallionRecord)
          continue;

        data::Clock::time_point expiresAt;
        if (not util::TryImmutable(
          *stallionRecord,
          "read the expiration of a stallion",
          [&expiresAt](const data::Stallion& stallion) noexcept
          {
            expiresAt = stallion.expiresAt();
          }))
        {
          continue;
        }

        const auto now = data::Clock::now();
        if (now <= expiresAt)
          continue;

        // Место под запись резервируется ДО добавления: иначе рост списка —
        // это выделение памяти прямо посреди обхода карты под замком.
        if (not util::TryReserveOneMore(expiredHorseUids))
          break;

        expiredHorseUids.emplace_back(Entry{
          .horseUid = horseUid,
          .stallionUid = registration.stallionUid});
      }
    });

  // Erase collected horse stallion registrations.
  (void)util::TryLocked(_mutex, [&]() noexcept
    {
      for (const auto [horseUid, stallionUid] : expiredHorseUids)
      {
        // ★Между сбором списка и этим местом замок отпускался, и регистрация
        // могла смениться: старую сняли вручную, лошадь выставили заново. Тогда
        // снятие по СТАРОМУ uid стёрло бы НОВУЮ регистрацию, оставив себе её
        // сбор. Сверяем актуальное значение. (Ревью Codex, находка 5.)
        const auto registrationIterator = _horseRegistrations.find(horseUid);
        if (registrationIterator == _horseRegistrations.end()
          || registrationIterator->second.stallionUid != stallionUid)
        {
          continue;
        }

        // ★Регистрация стирается ТОЛЬКО если снятие состоялось. Иначе запись о
        // долге пропала бы, а жеребец так и остался бы «жеребцом»: следующая
        // проверка через минуту попробует снова.
        if (not UnregisterStallion(horseUid, stallionUid))
          continue;

        // Remove from horse stallion registration.
        _horseRegistrations.erase(registrationIterator);
      }
    });
}

} // namespace server

