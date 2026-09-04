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

#ifndef BOUNDEDLIST_HPP
#define BOUNDEDLIST_HPP

#include "libserver/util/Stream.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <source_location>
#include <span>
#include <string_view>
#include <utility>

namespace server::util
{

//! LOA-fix (R74, round74, backlog #170): ЕДИНСТВЕННЫЙ СЕРИАЛИЗАТОР СПИСКА.
//!
//! ★ЧТО БЫЛО ОТКРЫТО. Сообщение протокола, чьё тело раздувается данными
//! игрока, умело убить персонажа насмерть двумя способами:
//!
//!  (а) СЧЁТЧИК ЛГАЛ. Сериализатор объявлял длину узким целым
//!      (`static_cast<uint8_t>(c.size())`) и следом писал ВЕСЬ контейнер.
//!      При `size() == 264` счётчик уезжал как 8 — клиент читал тело как
//!      мусор и кадр рассинхронивался навсегда.
//!
//!  (б) ТЕЛО ПЕРЕРАСТАЛО БУФЕР. `SinkStream::Write` бросает
//!      `std::overflow_error` (`src/libserver/util/Stream.cpp:58-68`) ИЗ
//!      ПОСТАВЩИКА ЗАПИСИ, то есть с потока `Client::WriteLoop`; там бросок
//!      ловится и зовёт `End()` (`src/libserver/network/Server.cpp:196-213`).
//!      Соединение закрывается — и так на КАЖДОМ входе, то есть персонаж не
//!      входит в игру больше НИКОГДА.
//!
//! ★ЧТО ЗАКРЫТО ЗДЕСЬ, А ЧТО НЕТ (граница названа вслух, см. RESULTS-R74 §7):
//!  * переполнение СЧЁТЧИКА закрыто ТОТАЛЬНО: после свипа ни одна площадка не
//!    пишет длину мимо этого хелпера, и это стережёт `tools/check_bounded_lists.py`;
//!  * переполнение ВМЕСТИМОСТИ закрыто для самого списка, и полностью — только
//!    для ТЕРМИНАЛЬНЫХ списков (список — последнее, что пишет сериализатор).
//!    Для нетерминальных всё, что пишется ПОСЛЕ списка, по-прежнему способно
//!    бросить, и бросок по-прежнему означает `End()`. Раунд превращает
//!    детерминированный разрыв в разрыв, зависящий от данных, и это надо
//!    называть своим именем.
//!
//! ★ПОЧЕМУ ОБРАТНАЯ ЗАПИСЬ СЧЁТА, А НЕ «ПОСЧИТАТЬ ЗАРАНЕЕ»: длина элемента
//! переменная — строки уезжают в EUC-KR (`src/libserver/util/Stream.cpp:72-82`),
//! поэтому заранее известно только ЧИСЛО элементов, но не их вес в байтах.
//! Счётчик пишется заглушкой, а после цикла переписывается фактическим числом.

//! Опции площадки списка.
//! @tparam CountType Целый тип счётчика НА ПРОВОДЕ (u8/u16/u32).
template <Numeric CountType>
struct BoundedListOptions
{
  //! Протокольный потолок В ЭЛЕМЕНТАХ. По умолчанию — сколько влезает в
  //! счётчик. ★Раунд не выдумывает новых потолков: сюда переезжают только те
  //! числа, которые код УЖЕ объявлял через `throw`, `assert` или `std::min`.
  std::size_t maxCount{std::numeric_limits<CountType>::max()};
  //! Во сколько единиц провода превращается ОДИН элемент контейнера.
  //! Единственный не-1 случай — `AcCmdCRStartRaceNotify::RaceRecord`: счётчик
  //! там объявляет СЕКТОРЫ, а контейнер хранит КРУГИ, по 3 сектора в круге.
  std::size_t countScale{1};
  //! Имя площадки для строки лога: "LobbyCommandLoginOK.equipmentItems".
  std::string_view name{};
  //! ★ПЕРЕПОЛНЕНИЕ ВМЕСТИМОСТИ НЕ ГАСИТСЯ, А ВЫПУСКАЕТСЯ НАРУЖУ.
  //!
  //! Ставится РОВНО там, где бросок из сериализатора уже кто-то ловит и
  //! превращает в штатный отказ. Сегодня такое место одно:
  //! `AcCmdCREnterRoomOK::racers` — его меряет гард
  //! `RaceNetworkHandler.cpp` (ответ прогоняется через скретч-`SinkStream`,
  //! бросок = «в комнату не пускаем»). Погаси мы там бросок «для
  //! единообразия» — вход в комнату разрешался бы с ТИХО ОБРЕЗАННЫМ ростером,
  //! то есть гард остался бы на месте и перестал бы работать.
  bool rethrowOnCapacity{false};
};

//! Жалуется на усечение списка, не чаще одного раза в окно на площадку
//! (площадка = файл+строка вызова).
//!
//! ★НЕШАБЛОННАЯ И В ОТДЕЛЬНОЙ TU НАМЕРЕННО. Живи форматная строка в шаблоне,
//! инстанцируемом в пяти единицах трансляции, число её копий в бинаре решал бы
//! линкер (`SHF_MERGE|SHF_STRINGS`), и ожидание лесенки «маркер = 1» было бы
//! везением, а не инвариантом. Здесь литерал структурно один, а у символа есть
//! тело в своей TU при выключенном LTO — значит `nm -C` его найдёт.
//!
//! ★`noexcept`: зовётся с потока `Client::WriteLoop`; бросок оттуда = выброшенный
//! клиент, ровно то, что этот раунд чинит.
//!
//! @param name Имя площадки.
//! @param requested Сколько элементов лежало в контейнере.
//! @param written Сколько ушло на провод.
//! @param bound Действовавший потолок в элементах.
//! @param capacityHit true — упёрлись в буфер команды, false — в потолок счёта.
//! @param where Место вызова (ключ подавления).
void BoundedListReport(
  std::string_view name,
  std::size_t requested,
  std::size_t written,
  std::size_t bound,
  bool capacityHit,
  const std::source_location& where) noexcept;

namespace detail
{

//! Считает потолок числа элементов, которое ВООБЩЕ можно объявить счётчиком.
//!
//! ★ДЕЛЕНИЕ, А НЕ УМНОЖЕНИЕ. Проверять надо `written * countScale <= max`, но
//! само умножение при большом `written` переполнилось бы раньше проверки.
//! `max(scale, 1)` — защита от `countScale == 0`, то есть от деления на ноль.
template <Numeric CountType>
[[nodiscard]] constexpr std::size_t BoundedListCountLimit(
  const BoundedListOptions<CountType>& options) noexcept
{
  static_assert(
    std::is_integral_v<CountType> && not std::is_same_v<CountType, bool>,
    "CountType должен быть целым: у enum std::numeric_limits<>::max() равен нулю, "
    "и потолок молча стал бы нулевым");

  const auto scale = std::max<std::size_t>(options.countScale, 1);
  const auto widest = static_cast<std::size_t>(std::numeric_limits<CountType>::max()) / scale;
  return std::min<std::size_t>(options.maxCount, widest);
}

//! Общее тело: пишет элементы, откатывая курсор на границу последнего целого
//! элемента при нехватке места, и возвращает, сколько записал.
template <typename Container, typename ElementWriter>
[[nodiscard]] std::size_t BoundedListWriteBody(
  SinkStream& stream,
  const Container& container,
  std::size_t planned,
  bool rethrowOnCapacity,
  ElementWriter&& writeElement,
  bool& capacityHit)
{
  std::size_t written = 0;
  capacityHit = false;

  for (const auto& element : container)
  {
    if (written == planned)
      break;

    const auto before = stream.GetCursor();
    try
    {
      writeElement(stream, element);
    }
    catch (const std::overflow_error&)
    {
      // ★ОТКАТ ОБЯЗАТЕЛЕН: `SinkStream::Write(const std::string&)` пишет
      // ПОБАЙТНО и способен бросить, оставив половину строки в буфере.
      // Незакоммиченные байты за финальным курсором никуда не уходят —
      // `CommandServer::SendCommand` коммитит ровно `GetCursor()`.
      stream.Seek(before);
      if (rethrowOnCapacity)
        throw;
      capacityHit = true;
      break;
    }
    ++written;
  }

  return written;
}

} // namespace detail

//! Пишет счётчик и тело списка так, что счётчик НЕ ЛЖЁТ.
//!
//! Порядок шагов обязателен:
//!  1. потолок считается В `size_t` ДО сужающего каста (иначе 264 при бонде 16
//!     дало бы 8 — это и есть дефект, который раунд закрывает);
//!  2. пишется заглушка счётчика, её курсор запоминается;
//!  3. пишутся элементы, пока хватает и счёта, и места;
//!  4. если записано не столько, сколько объявлено, счётчик переписывается.
//!
//! @returns Сколько элементов реально ушло на провод.
template <Numeric CountType, typename Container, typename ElementWriter>
std::size_t WriteBoundedList(
  SinkStream& stream,
  const Container& container,
  const BoundedListOptions<CountType>& options,
  ElementWriter&& writeElement,
  const std::source_location where = std::source_location::current())
{
  const auto countLimit = detail::BoundedListCountLimit(options);
  const std::size_t requested = container.size();
  const std::size_t planned = std::min<std::size_t>(requested, countLimit);

  const auto countCursor = stream.GetCursor();
  // ★БРОСОК ЗДЕСЬ НЕ ГАСИМ: если места не хватает даже под сам счётчик, значит
  // не-списочное содержимое кадра уже съело буфер команды. Это ЗА пределами
  // того, что раунд обещает закрыть, и молчаливое «списка не будет» скрыло бы
  // отказ вместо того, чтобы его назвать.
  stream.Write(static_cast<CountType>(planned * options.countScale));

  bool capacityHit = false;
  const auto written = detail::BoundedListWriteBody(
    stream,
    container,
    planned,
    options.rethrowOnCapacity,
    std::forward<ElementWriter>(writeElement),
    capacityHit);

  if (written != planned)
  {
    const auto end = stream.GetCursor();
    stream.Seek(countCursor);
    stream.Write(static_cast<CountType>(written * options.countScale));
    stream.Seek(end);
  }

  if (requested > written)
    BoundedListReport(options.name, requested, written, countLimit, capacityHit, where);

  return written;
}

//! Перегрузка без писателя элемента: элемент уходит как `stream.Write(element)`.
//!
//! ★Годится только когда элемент — `Numeric`, `std::string` или
//! `WritableStruct` (`include/libserver/util/Stream.hpp`). Для `std::pair`
//! (обход `std::unordered_map`) она НЕ КОМПИЛИРУЕТСЯ — такой площадке
//! обязателен явный писатель, и это хорошо: неявный обход неупорядоченной
//! карты уехал бы на провод в неопределённом порядке.
template <Numeric CountType, typename Container>
std::size_t WriteBoundedList(
  SinkStream& stream,
  const Container& container,
  const BoundedListOptions<CountType>& options,
  const std::source_location where = std::source_location::current())
{
  return WriteBoundedList<CountType>(
    stream,
    container,
    options,
    [](SinkStream& sink, const auto& element) { sink.Write(element); },
    where);
}

//! Булк-форма: счётчик + непрерывный блок байтов.
//!
//! ★ОСТАТОК СЧИТАЕТСЯ НАСЫЩАЮЩЕ. `stream.Size() - cursor` на беззнаковом типе
//! заворачивается в 2^64, если курсор уже за размером буфера, и «сколько
//! влезет» стало бы «влезет всё».
template <Numeric CountType>
std::size_t WriteBoundedBytes(
  SinkStream& stream,
  std::span<const std::byte> bytes,
  const BoundedListOptions<CountType>& options,
  const std::source_location where = std::source_location::current())
{
  const auto countLimit = detail::BoundedListCountLimit(options);

  const auto cursorAfterCount = stream.GetCursor() + sizeof(CountType);
  const std::size_t remaining = stream.Size() > cursorAfterCount
    ? stream.Size() - cursorAfterCount
    : 0;

  const auto byCount = std::min<std::size_t>(bytes.size(), countLimit);
  const auto n = std::min<std::size_t>(byCount, remaining);

  stream.Write(static_cast<CountType>(n));
  if (n > 0)
    stream.Write(bytes.data(), n);

  if (bytes.size() > n)
  {
    // Упёрлись в буфер, а не в счётчик, ровно когда именно вместимость срезала
    // то, что счётчик пропустил бы.
    const bool capacityHit = n < byCount;
    BoundedListReport(options.name, bytes.size(), n, countLimit, capacityHit, where);
  }

  return n;
}

//! Резервное место под счётчик, который пишется РАНЬШЕ тела.
//!
//! ★ЗАЧЕМ ОТДЕЛЬНАЯ ФОРМА. У почты чаттера счётчик и тело считают РАЗНЫЕ вещи:
//! на провод уезжает `mailboxInfo.mailCount` (его заполняет директор), а тело —
//! это цикл по другому вектору, и между ними в кадре стоит ещё одно поле
//! (`hasMoreMail`). Счётчик там способен оказаться больше тела уже сегодня.
template <Numeric CountType>
class BoundedListSlot
{
public:
  //! Пишет нулевую заглушку счётчика и запоминает её курсор.
  explicit BoundedListSlot(SinkStream& stream)
    : _cursor(stream.GetCursor())
  {
    stream.Write(static_cast<CountType>(0));
  }

  [[nodiscard]] std::size_t cursor() const noexcept { return _cursor; }

private:
  std::size_t _cursor;
};

//! То же, что `WriteBoundedList`, но счётчик уже зарезервирован слотом.
//! Обратная запись счёта делается ВСЕГДА — в слоте изначально ноль.
template <Numeric CountType, typename Container, typename ElementWriter>
std::size_t WriteBoundedListInto(
  SinkStream& stream,
  const BoundedListSlot<CountType>& slot,
  const Container& container,
  const BoundedListOptions<CountType>& options,
  ElementWriter&& writeElement,
  const std::source_location where = std::source_location::current())
{
  const auto countLimit = detail::BoundedListCountLimit(options);
  const std::size_t requested = container.size();
  const std::size_t planned = std::min<std::size_t>(requested, countLimit);

  bool capacityHit = false;
  const auto written = detail::BoundedListWriteBody(
    stream,
    container,
    planned,
    options.rethrowOnCapacity,
    std::forward<ElementWriter>(writeElement),
    capacityHit);

  const auto end = stream.GetCursor();
  stream.Seek(slot.cursor());
  stream.Write(static_cast<CountType>(written * options.countScale));
  stream.Seek(end);

  if (requested > written)
    BoundedListReport(options.name, requested, written, countLimit, capacityHit, where);

  return written;
}

} // namespace server::util

#endif // BOUNDEDLIST_HPP
