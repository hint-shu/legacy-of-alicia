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

#ifndef RECORD_ACCESS_HPP
#define RECORD_ACCESS_HPP

#include "libserver/data/Record.hpp"
#include "libserver/util/QuietLog.hpp"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace server::util
{

//! Что СТАЛО с изменением записи.
//!
//! Три состояния, а не два, — потому что `Record::Mutable` НЕ атомарен:
//! просьба сохранить запись (`_patchListener`) стоит в самом его конце и умеет
//! бросить УЖЕ ПОСЛЕ того, как изменение применено. Пояс, который трактует
//! такой бросок как «не сделано», создаёт тихую потерю предмета; пояс, который
//! трактует его как «сделано» вслепую, — дубль. Поэтому ответов три.
enum class MutateOutcome
{
  //! Изменение не начиналось: запись осталась ровно такой, какой была.
  NotApplied,
  //! Изменение применено ПОЛНОСТЬЮ, но просьба сохранить запись не поставлена.
  AppliedNotPersisted,
  //! Изменение применено и поставлено в очередь сохранения.
  Applied,
};

//! Применяет изменение к записи и точно сообщает, что произошло.
//!
//! ★Точность держится не на внимательности автора, а на ТИПЕ: тело изменения
//! обязано быть небросающим, иначе код не соберётся. Только при этом условии
//! состояние «применено наполовину» недостижимо ПО ПОСТРОЕНИЮ, а значит
//! `NotApplied` действительно означает «запись не тронута».
//!
//! @param record Запись данных.
//! @param what Что за изменение (уходит в строку лога при отказе).
//! @param change Небросающее тело изменения.
//! @returns Что стало с изменением.
template <typename Data, typename Change>
[[nodiscard]] MutateOutcome TryMutate(
  const Record<Data>& record,
  const char* const what,
  Change&& change) noexcept
{
  static_assert(
    std::is_nothrow_invocable_v<Change&, Data&>,
    "тело изменения записи обязано быть небросающим (пометить лямбду noexcept): "
    "иначе бросок изнутри оставит запись изменённой наполовину, и различить "
    "'не применено' от 'применено' станет невозможно");

  bool applied = false;
  try
  {
    record.Mutable(
      [&applied, &change](Data& data)
      {
        change(data);
        // ★Отметка ставится ПОСЛЕДНИМ действием: изменение состоялось целиком.
        applied = true;
      });

    return MutateOutcome::Applied;
  }
  catch (const std::exception& x)
  {
    QuietLogError("Record change '{}' failed: {}", what, x.what());
  }
  catch (...)
  {
    QuietLogError("Record change '{}' failed: unknown exception", what);
  }

  return applied ? MutateOutcome::AppliedNotPersisted : MutateOutcome::NotApplied;
}

//! Читает запись под поясом.
//! @returns `true`, если чтение состоялось.
template <typename Data, typename View>
[[nodiscard]] bool TryImmutable(
  const Record<Data>& record,
  const char* const what,
  View&& view) noexcept
{
  static_assert(
    std::is_nothrow_invocable_v<View&, const Data&>,
    "тело чтения записи обязано быть небросающим (пометить лямбду noexcept)");

  try
  {
    record.Immutable(
      [&view](const Data& data)
      {
        view(data);
      });
    return true;
  }
  catch (const std::exception& x)
  {
    QuietLogError("Record read '{}' failed: {}", what, x.what());
  }
  catch (...)
  {
    QuietLogError("Record read '{}' failed: unknown exception", what);
  }

  return false;
}

//! Запрашивает запись (или пачку записей) у хранилища под поясом.
//! Отказ = пустой результат, ровно как штатная недоступность данных, которую
//! вызывающий код уже умеет проверять.
template <typename Storage, typename Key>
[[nodiscard]] auto TryGet(
  Storage& storage,
  const Key& key,
  const char* const what) noexcept -> decltype(storage.Get(key))
{
  try
  {
    return storage.Get(key);
  }
  catch (const std::exception& x)
  {
    QuietLogError("Lookup of '{}' failed: {}", what, x.what());
  }
  catch (...)
  {
    QuietLogError("Lookup of '{}' failed: unknown exception", what);
  }

  return {};
}

//! Просит хранилище удалить запись. @returns `true`, если просьба принята.
template <typename Storage, typename Key>
[[nodiscard]] bool TryDelete(
  Storage& storage,
  const Key& key,
  const char* const what) noexcept
{
  try
  {
    storage.Delete(key);
    return true;
  }
  catch (const std::exception& x)
  {
    QuietLogError("Delete request of '{}' failed: {}", what, x.what());
  }
  catch (...)
  {
    QuietLogError("Delete request of '{}' failed: unknown exception", what);
  }

  return false;
}

//! Просит хранилище сохранить запись. Нужен там, где изменение состоялось, а
//! просьба сохранить его — нет (`MutateOutcome::AppliedNotPersisted`).
template <typename Storage, typename Key>
bool TrySave(
  Storage& storage,
  const Key& key,
  const char* const what) noexcept
{
  try
  {
    storage.Save(key);
    return true;
  }
  catch (const std::exception& x)
  {
    QuietLogError("Save request of '{}' failed: {}", what, x.what());
  }
  catch (...)
  {
    QuietLogError("Save request of '{}' failed: unknown exception", what);
  }

  return false;
}

//! Резервирует место под ОДИН будущий элемент.
//!
//! Смысл — порядок: после успешного резерва `emplace_back` не выделяет память,
//! то есть добавление уже не может сорваться. Значит всю бросающую работу можно
//! сделать ДО того, как появится то, что придётся добавлять.
//! @returns `true`, если место есть.
template <typename Element>
[[nodiscard]] bool TryReserveOneMore(std::vector<Element>& container) noexcept
{
  static_assert(
    std::is_trivially_copyable_v<Element>,
    "резерв даёт гарантию небросающего добавления только для тривиально "
    "копируемых элементов; для прочих добавление умеет бросить и после резерва");

  // ★Место уже есть — не трогаем ничего. Самый частый случай.
  if (container.size() < container.capacity())
    return true;

  try
  {
    // ★ГЕОМЕТРИЯ СОХРАНЯЕТСЯ. Наивное `reserve(size + 1)` дало бы ТОЧНО столько
    // места, сколько попросили (libstdc++ не округляет вверх), и каждый
    // следующий предмет заново перевыделял бы весь инвентарь: рост стал бы
    // квадратичным. Это была бы плата УСПЕШНОГО пути за поведение на отказе —
    // ровно то, что запрещает урок R51 (dont-trade-success-path-for-failure-path).
    // Поэтому растим как вектор растёт сам: вдвое, но не меньше восьми.
    constexpr std::size_t MinimalCapacity = 8;
    container.reserve(std::max(container.capacity() * 2, MinimalCapacity));
    return true;
  }
  catch (...)
  {
    return false;
  }
}

//! Выполняет небросающее действие под ЭКСКЛЮЗИВНЫМ замком.
//!
//! Взятие замка — операция, которая по стандарту умеет бросить
//! (`std::system_error`), поэтому в `noexcept`-функции оно обязано быть накрыто
//! так же, как выделение памяти. @returns `true`, если действие выполнено.
template <typename Mutex, typename Action>
[[nodiscard]] bool TryLocked(Mutex& mutex, Action&& action) noexcept
{
  static_assert(
    std::is_nothrow_invocable_v<Action&>,
    "тело под замком обязано быть небросающим (пометить лямбду noexcept): "
    "иначе непонятно, в каком состоянии осталось защищённое замком данное");

  try
  {
    const std::scoped_lock lock(mutex);
    action();
    return true;
  }
  catch (const std::exception& x)
  {
    QuietLogError("Taking an exclusive lock failed: {}", x.what());
  }
  catch (...)
  {
    QuietLogError("Taking an exclusive lock failed: unknown exception");
  }

  return false;
}

//! То же самое, но под РАЗДЕЛЯЕМЫМ замком — для чтения.
//! ★Отдельная функция, а не флаг: читающие пути обязаны остаться читающими,
//! иначе раунд молча превратил бы параллельные чтения рынка в очередь.
template <typename Mutex, typename Action>
[[nodiscard]] bool TryShared(Mutex& mutex, Action&& action) noexcept
{
  static_assert(
    std::is_nothrow_invocable_v<Action&>,
    "тело под замком обязано быть небросающим (пометить лямбду noexcept)");

  try
  {
    const std::shared_lock lock(mutex);
    action();
    return true;
  }
  catch (const std::exception& x)
  {
    QuietLogError("Taking a shared lock failed: {}", x.what());
  }
  catch (...)
  {
    QuietLogError("Taking a shared lock failed: unknown exception");
  }

  return false;
}

//! Занимает место под будущее значение в УЗЛОВОМ контейнере (`unordered_map`).
//!
//! ★Приём вектора здесь НЕ работает: `reserve` у `unordered_map` растит только
//! корзины, а вставка всё равно выделяет УЗЕЛ и умеет бросить. Поэтому узел
//! создаётся заранее, со значением по умолчанию, — а запись значения в уже
//! существующий узел памяти не требует и сорваться не может.
//!
//! Смысл тот же, что у резерва: сделать всю опасную работу ДО того, как
//! появится то, что придётся публиковать.
//!
//! @returns `true`, если узел под ключом создан ЭТИМ вызовом. `false` — либо
//!          ключ уже занят, либо памяти не хватило; в обоих случаях вызывающий
//!          не имеет права продолжать.
template <typename Map, typename Key>
[[nodiscard]] bool TryClaimSlot(Map& map, const Key& key) noexcept
{
  try
  {
    return map.try_emplace(key).second;
  }
  catch (...)
  {
    return false;
  }
}

//! Ставит задачу в планировщик, НЕ бросая (R55, backlog #179 часть 5).
//!
//! ★Зачем отдельный помощник, а не перехват по месту. `Scheduler::Queue`
//! растит список задач под мьютексом, то есть ВЫДЕЛЯЕТ ПАМЯТЬ, — и зовут его
//! из функций, помеченных `noexcept`. Там `bad_alloc` это не «задача не
//! поставилась», а `std::terminate` всего сервера.
//!
//! ★Возвращаемое значение ОБЯЗАН читать вызывающий: неудача постановки
//! означает, что периодическая задача больше НИКОГДА не проснётся, — молча
//! потерянный таймер хуже громкого отказа
//! ([[a-check-nobody-reads-is-not-a-check]]). Поэтому `[[nodiscard]]`.
//!
//! Шаблон по типу планировщика, чтобы заголовок доступа к записям не тянул
//! за собой заголовок планировщика.
template <typename Scheduler, typename Task, typename When>
[[nodiscard]] bool TryQueue(Scheduler& scheduler, Task&& task, const When when) noexcept
{
  try
  {
    scheduler.Queue(std::forward<Task>(task), when);
    return true;
  }
  catch (...)
  {
    return false;
  }
}

} // namespace server::util

#endif // RECORD_ACCESS_HPP
