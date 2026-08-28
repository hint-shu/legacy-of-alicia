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

#include "server/system/ItemSystem.hpp"

#include <spdlog/spdlog.h>

#include "server/ServerInstance.hpp"
#include "libserver/data/DataDirector.hpp"
#include "libserver/registry/ItemRegistry.hpp"
#include "libserver/util/QuietLog.hpp"
#include "libserver/util/RecordAccess.hpp"


namespace server
{

ItemSystem::ItemSystem(ServerInstance& serverInstance)
  : _serverInstance(serverInstance)
{
}

data::Uid ItemSystem::GetItem(
  data::Character& character,
  data::Tid itemTid) const noexcept
{
  const auto searchItems = [this, &itemTid](const std::vector<data::Uid>& itemUids) -> data::Uid
  {
    // LOA-fix (R52-2, round52, backlog #179 часть 2): запрос записей и их
    // чтение — бросающая работа (реестр записей и очередь чтения выделяют
    // память) внутри `noexcept`-функции. Отказ означает «не нашли»; последствие
    // названо честно: `AddItem` создаст ВТОРУЮ стопку того же tid вместо
    // слияния. Количество при этом сохранено, ценность не дублируется.
    const auto itemRecords = util::TryGet(
      _serverInstance.GetDataDirector().GetItemCache(), itemUids, "items of a character");
    if (not itemRecords)
      return data::InvalidUid;

    for (const auto& itemRecord : *itemRecords)
    {
      auto foundItemUid = data::InvalidUid;
      if (not util::TryImmutable(itemRecord, "look up an item by its tid",
        [&foundItemUid, &itemTid](const data::Item& item) noexcept
        {
          if (item.tid() != itemTid)
            return;

          foundItemUid = item.uid();
        }))
      {
        continue;
      }

      if (foundItemUid != data::InvalidUid)
        return foundItemUid;
    }

    return data::InvalidUid;
  };

  auto foundUid = searchItems(character.inventory());
  if (foundUid != data::InvalidUid)
    return foundUid;

  return searchItems(character.characterEquipment());
}

data::Uid ItemSystem::AddItem(
  data::Character& character,
  const data::Tid itemTid,
  const uint32_t count) const noexcept
{
  const auto itemUid = GetItem(character, itemTid);
  if (itemUid == data::InvalidUid)
  {
    // LOA-fix (A6, round3): здесь не было `return` — при неудаче CreateItem()
    // выполнение проваливалось на .Mutable НЕДОСТУПНОЙ записи, Record::Mutable
    // бросал std::runtime_error, а AddItem объявлен noexcept → std::terminate.
    // (1) Несколько попыток: каждая берёт СЛЕДУЮЩИЙ uid и обходит уже занятый
    // ключ в DataStorage. (2) Полная неудача — громкий лог и InvalidUid вместо
    // падения. (3) Невалидный uid в инвентарь не кладём.
    // LOA-fix (R52-3, round52, backlog #179 часть 2): ★ПОРЯДОК «СНАЧАЛА МЕСТО,
    // ПОТОМ ПРЕДМЕТ». Место под указатель в инвентаре резервируется ДО создания
    // записи: отказ здесь означает, что не создано и не изменено НИЧЕГО —
    // определённый отказ без мусора. Прежний порядок создавал предмет (и ставил
    // его в очередь сохранения), а потом мог не найти памяти под рост инвентаря
    // — предмет оставался в базе, не принадлежа никому, а игрок получал отказ:
    // тихая потеря плюс вечный мусор. После успешного резерва добавление ниже
    // не выделяет память и не бросает.
    if (not util::TryReserveOneMore(character.inventory()))
    {
      util::QuietLogError(
        "Failed to reserve an inventory slot for new item '{}' of character '{}'; "
        "nothing was created",
        itemTid,
        character.name());
      return data::InvalidUid;
    }

    Record<data::Item> createdItemRecord{};
    for (int attempt = 0; attempt < 8 && not createdItemRecord; ++attempt)
      createdItemRecord = _serverInstance.GetDataDirector().CreateItem();

    if (not createdItemRecord)
    {
      util::QuietLogError(
        "Failed to create new item '{}' (x{}) for character '{}'",
        itemTid,
        count,
        character.name());
      return data::InvalidUid;
    }

    auto createdItemUid = data::InvalidUid;

    const auto fillOutcome = util::TryMutate(
      createdItemRecord,
      "fill a newly created item",
      [&itemTid, &count, &createdItemUid](data::Item& item) noexcept
      {
        item.tid() = itemTid;
        item.count() = count;
        item.duration() = std::chrono::seconds::zero();
        item.createdAt() = data::Clock::now();

        createdItemUid = item.uid();
      });

    if (fillOutcome == util::MutateOutcome::NotApplied
      || createdItemUid == data::InvalidUid)
    {
      // Запись создана, но не наполнена — она не принадлежит никому. Утечка
      // ВИДИМАЯ и залогированная честнее тихой порчи (правило R50).
      util::QuietLogError(
        "Created item record for '{}' was not filled, leaving the record orphaned; "
        "not adding it to '{}'",
        itemTid,
        character.name());
      return data::InvalidUid;
    }

    if (fillOutcome == util::MutateOutcome::AppliedNotPersisted)
    {
      // Предмет наполнен верно, потеряна только просьба сохранить — просим ещё
      // раз. Выдача при этом СОСТОЯЛАСЬ, и вызывающий узнает об этом честно.
      static_cast<void>(util::TrySave(
        _serverInstance.GetDataDirector().GetItemCache(), createdItemUid, "a new item"));
    }

    character.inventory().emplace_back(createdItemUid);
    return createdItemUid;
  }

  const auto itemRecord = util::TryGet(
    _serverInstance.GetDataDirector().GetItemCache(), itemUid, "an item stack to add to");
  if (not itemRecord)
    return data::InvalidUid;

  // LOA-fix (R52-3, round52, backlog #179 часть 2): ★возвращаемое значение
  // обязано РАВНЯТЬСЯ тому, что произошло с данными. Сказать «не выдано» после
  // состоявшегося изменения — тихая потеря (вызывающий компенсирует игроку
  // второй раз или откажет); сказать «выдано» вслепую — дубль.
  const auto mergeOutcome = util::TryMutate(
    *itemRecord,
    "add to an item stack",
    [&count](data::Item& item) noexcept
    {
      item.count() += count;
    });

  if (mergeOutcome == util::MutateOutcome::NotApplied)
  {
    util::QuietLogError(
      "Failed to add item '{}' to the stack {} of character '{}'; nothing was granted",
      itemTid,
      itemUid,
      character.name());
    return data::InvalidUid;
  }

  if (mergeOutcome == util::MutateOutcome::AppliedNotPersisted)
  {
    static_cast<void>(util::TrySave(
      _serverInstance.GetDataDirector().GetItemCache(), itemUid, "an item stack"));
  }

  return itemUid;
}

data::Uid ItemSystem::AddItem(
  data::Character& character,
  data::Tid itemTid,
  std::chrono::seconds duration) const noexcept
{
  const auto itemUid = GetItem(character, itemTid);
  if (itemUid == data::InvalidUid)
  {
    // LOA-fix (A6, round3): ровно тот же дефект, что в count-перегрузке, и
    // именно ЭТА перегрузка роняла сервер на сдаче квеста 11030 (награда 110 =
    // tid 20027, items.yaml type: 1 = Temporary → сюда). Заодно чиним формат
    // лога: плейсхолдеров было два, а аргументов три.
    // LOA-fix (R52-3, round52, backlog #179 часть 2): ★ПОРЯДОК «СНАЧАЛА МЕСТО,
    // ПОТОМ ПРЕДМЕТ» — тот же, что в count-перегрузке. Отказ резерва означает,
    // что не создано и не изменено ничего.
    if (not util::TryReserveOneMore(character.inventory()))
    {
      util::QuietLogError(
        "Failed to reserve an inventory slot for new item '{}' of character '{}'; "
        "nothing was created",
        itemTid,
        character.name());
      return data::InvalidUid;
    }

    Record<data::Item> createdItemRecord{};
    for (int attempt = 0; attempt < 8 && not createdItemRecord; ++attempt)
      createdItemRecord = _serverInstance.GetDataDirector().CreateItem();

    if (not createdItemRecord)
    {
      util::QuietLogError(
        "Failed to create new item '{}' ({}s) for character '{}'",
        itemTid,
        duration.count(),
        character.name());
      return data::InvalidUid;
    }

    auto createdItemUid = data::InvalidUid;

    const auto fillOutcome = util::TryMutate(
      createdItemRecord,
      "fill a newly created temporary item",
      [&itemTid, &duration, &createdItemUid](data::Item& item) noexcept
      {
        item.tid() = itemTid;
        item.count() = 1;
        item.duration() = duration;
        item.createdAt() = data::Clock::now();

        createdItemUid = item.uid();
      });

    if (fillOutcome == util::MutateOutcome::NotApplied
      || createdItemUid == data::InvalidUid)
    {
      // Тот же случай, что в count-перегрузке: запись есть, владельца нет.
      util::QuietLogError(
        "Created item record for '{}' was not filled, leaving the record orphaned; "
        "not adding it to '{}'",
        itemTid,
        character.name());
      return data::InvalidUid;
    }

    if (fillOutcome == util::MutateOutcome::AppliedNotPersisted)
    {
      static_cast<void>(util::TrySave(
        _serverInstance.GetDataDirector().GetItemCache(), createdItemUid, "a new item"));
    }

    character.inventory().emplace_back(createdItemUid);
    return createdItemUid;
  }

  const auto itemRecord = util::TryGet(
    _serverInstance.GetDataDirector().GetItemCache(), itemUid, "an item stack to extend");
  if (not itemRecord)
    return data::InvalidUid;

  const auto mergeOutcome = util::TryMutate(
    *itemRecord,
    "extend the lifetime of an item stack",
    [&duration](data::Item& item) noexcept
    {
      // LOA-fix (NEW-3, round3): КАП суммарной длительности. Повторная выдача
      // одного и того же Temporary-предмета просто СКЛАДЫВАЛА сроки, верхней
      // границы не было вообще — предмет «на 36 часов» становился де-факто
      // вечным: каждая награда дня/квеста добавляла ещё 36 ч поверх остатка.
      // Теперь остаток пересобирается от «сейчас» и обрезается по потолку.
      // ПОЧЕМУ ПОТОЛОК ИМЕННО ГОДОВОЙ, А НЕ «пара выдач»: эта же перегрузка
      // обслуживает МАГАЗИН и ПОДАРКИ (RanchDirector: AddItem(..., hours(
      // priceRange))), а в items.yaml есть легальные 30-суточные позиции — жёсткий
      // кап в 30 суток отобрал бы у игрока вторую купленную месячную подписку.
      // Год — гарантированная верхняя граница («вечности» больше нет), при этом
      // ни одна честная покупка в неё не упирается: чтобы дойти до потолка
      // 36-часовыми наградами дня, нужны годы ежедневного клейма.
      // ТЮНИНГ — одна константа ниже (сколько предмет может «висеть» суммарно).
      static constexpr std::chrono::seconds MaxAccumulatedDuration =
        std::chrono::hours(24 * 365); // 365 суток

      const auto now = data::Clock::now();
      const auto expiresAt = item.createdAt() + item.duration();

      if (expiresAt <= now)
      {
        // The item is already expired, restart its lifetime from now
        // instead of extending an expired duration.
        item.createdAt() = now;
        item.duration() = std::min(duration, MaxAccumulatedDuration);
      }
      else
      {
        // Остаток + новая длительность, но не больше потолка. Точку отсчёта
        // переносим на «сейчас», иначе кап пришлось бы считать от старой
        // createdAt и он тёк бы вместе с ней. Момент истечения при этом никогда
        // не сдвигается НАЗАД — игрок ничего не теряет.
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
          expiresAt - now);
        item.createdAt() = now;
        item.duration() = std::min(remaining + duration, MaxAccumulatedDuration);
      }
    });

  if (mergeOutcome == util::MutateOutcome::NotApplied)
  {
    util::QuietLogError(
      "Failed to extend item '{}' in the stack {} of character '{}'; nothing was granted",
      itemTid,
      itemUid,
      character.name());
    return data::InvalidUid;
  }

  if (mergeOutcome == util::MutateOutcome::AppliedNotPersisted)
  {
    static_cast<void>(util::TrySave(
      _serverInstance.GetDataDirector().GetItemCache(), itemUid, "an item stack"));
  }

  return itemUid;
}

void ItemSystem::RemoveItem(
  data::Character& character,
  const data::Tid itemTid) const noexcept
{
  // LOA-fix (R52-5, round52, backlog #179 часть 2). ★ЧЕСТНО: у этой функции
  // СЕГОДНЯ НЕТ ВЫЗЫВАЮЩИХ во всём дереве — делаем её корректной, а не живой.
  auto& itemCache = _serverInstance.GetDataDirector().GetItemCache();

  const auto itemRecords = util::TryGet(
    itemCache, character.inventory(), "items of a character");
  if (not itemRecords)
    return;

  auto itemUid{data::InvalidUid};
  for (const auto& itemRecord : *itemRecords)
  {
    // ★Поиск по tid — это ЧТЕНИЕ. Прежде здесь стоял `Mutable`, то есть
    // эксклюзивный замок И просьба сохранить КАЖДЫЙ просмотренный предмет:
    // сканирование инвентаря переписывало весь инвентарь на диск и на ровном
    // месте добавляло бросающую работу в `noexcept`-функцию.
    if (not util::TryImmutable(itemRecord, "look up an item to remove",
      [&itemUid, &itemTid](const data::Item& item) noexcept
      {
        if (item.tid() != itemTid)
          return;

        itemUid = item.uid();
      }))
    {
      continue;
    }

    if (itemUid != data::InvalidUid)
      break;
  }

  if (itemUid != data::InvalidUid)
  {
    // ПОРЯДОК: снятие из инвентаря не бросает, удаление записи — под поясом.
    // Обратный порядок оставил бы игроку стопку-призрака в инвентаре.
    const auto itemsToRemove = std::ranges::remove(character.inventory(), itemUid);
    character.inventory().erase(itemsToRemove.begin(), itemsToRemove.end());

    if (not util::TryDelete(itemCache, itemUid, "a removed item"))
    {
      util::QuietLogError(
        "Item {} was taken from the inventory of '{}' but its record could not be deleted; "
        "the record is left orphaned",
        itemUid,
        character.name());
    }
  }
}

ItemSystem::ConsumeVerdict ItemSystem::ConsumeItem(
  data::Character& character,
  const data::Tid itemTid,
  const uint32_t count) const noexcept
{
  auto& itemCache = _serverInstance.GetDataDirector().GetItemCache();

  const auto itemRecords = util::TryGet(
    itemCache, character.inventory(), "items of a character");
  if (not itemRecords)
    return {};

  for (const auto& itemRecord : *itemRecords)
  {
    ConsumeVerdict verdict{};

    const auto consumeOutcome = util::TryMutate(
      itemRecord,
      "consume from an item stack",
      [&verdict, &itemTid, &count](data::Item& item) noexcept
    {
      if (item.tid() != itemTid)
        return;

      verdict.itemUid = item.uid();

      if (static_cast<int64_t>(item.count()) - count >= 0)
      {
        item.count() = item.count() - count;
        verdict.itemConsumed = true;
        verdict.remainingItemCount = item.count();
      }
    });

    if (verdict.itemUid != data::InvalidUid)
    {
      if (consumeOutcome == util::MutateOutcome::AppliedNotPersisted
        && verdict.itemConsumed)
      {
        // LOA-fix (R52-6, round52, backlog #179 часть 2): ★списание
        // СОСТОЯЛОСЬ, потеряна только просьба сохранить. Сообщить «не списано»
        // значило бы отказать игроку, у которого предмет УЖЕ забрали — тихая
        // потеря в чистом виде. Поэтому вердикт остаётся успешным, а сохранение
        // просим ещё раз.
        static_cast<void>(util::TrySave(itemCache, verdict.itemUid, "a consumed item stack"));
      }

      // LOA-fix (R29-7, #59 S19, HARDENING): удалять стопку можно ТОЛЬКО после
      // УСПЕШНОГО списания. Когда в стопке не хватает count, лямбда выше ставит
      // verdict.itemUid, но НЕ трогает itemConsumed/remainingItemCount — а дефолт
      // ConsumeVerdict::remainingItemCount равен 0 (ItemSystem.hpp), то есть
      // «не хватило» и «списали в ноль» давали ОДНО И ТО ЖЕ значение, и
      // ПРОВАЛИВШЕЕСЯ списание СТИРАЛО стопку и из кэша, и из инвентаря.
      // ★ЧЕСТНО: сегодня ветка ЛАТЕНТНА — все существующие вызовы ConsumeItem
      // передают count = 1, поэтому «не хватило» достижимо лишь на стопке с
      // count == 0. Это hardening на будущее (первый же вызов с count > 1 сделал
      // бы её живой), а НЕ живой баг.
      if (verdict.itemConsumed && verdict.remainingItemCount == 0)
      {
        // ПОРЯДОК: сначала снимаем uid из инвентаря (не бросает), потом просим
        // удалить запись. Обратный порядок оставлял бы игроку стопку-призрака.
        const auto itemRange = std::ranges::remove(character.inventory(), verdict.itemUid);
        character.inventory().erase(itemRange.begin(), itemRange.end());

        if (not util::TryDelete(itemCache, verdict.itemUid, "an emptied item stack"))
        {
          util::QuietLogError(
            "Emptied stack {} was taken from the inventory of '{}' but its record could not be "
            "deleted; the record is left orphaned",
            verdict.itemUid,
            character.name());
        }

        verdict.itemUid = data::InvalidUid;
      }

      return verdict;
    }

    if (consumeOutcome == util::MutateOutcome::NotApplied)
    {
      // Стопку не удалось даже осмотреть. Останавливаемся: «не списано» —
      // правда, вызывающий откажет игроку, а предмет у игрока цел.
      util::QuietLogError(
        "Failed to consume item '{}' of character '{}'; nothing was consumed",
        itemTid,
        character.name());
      return {};
    }
  }

  return {};
}

bool ItemSystem::HasItem(
  const data::Character& character,
  const data::Tid itemTid) const noexcept
{
  const auto HasItemWithTid = [this, &itemTid](const std::vector<data::Uid>& itemUids)
  {
    // LOA-fix (R52-7, round52, backlog #179 часть 2): отказ хранилища читается
    // как «нет предмета». Направление безопасное: действие будет ОТКЛОНЕНО, а
    // не выдано даром; обратное решение открывало бы бесплатное использование.
    const auto itemRecords = util::TryGet(
      _serverInstance.GetDataDirector().GetItemCache(), itemUids, "items of a character");
    if (not itemRecords)
      return false;

    for (const auto& itemRecord : *itemRecords)
    {
      bool isMatch = false;
      if (not util::TryImmutable(itemRecord, "check an item tid",
        [&isMatch, &itemTid](const data::Item& item) noexcept
        {
          isMatch = item.tid() == itemTid;
        }))
      {
        continue;
      }

      if (isMatch)
        return true;
    }

    return false;
  };

  if (HasItemWithTid(character.inventory()))
    return true;

  if (HasItemWithTid(character.characterEquipment()))
    return true;

  return false;
}

bool ItemSystem::HasItemInstance(
  const data::Character& character,
  data::Uid itemUid) const noexcept
{
  if (std::ranges::contains(character.inventory(), itemUid))
    return true;

  if (std::ranges::contains(character.characterEquipment(), itemUid))
    return true;

  return false;
}

} // namespace server