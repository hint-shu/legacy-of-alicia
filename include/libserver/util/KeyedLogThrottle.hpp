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

#ifndef KEYED_LOG_THROTTLE_HPP
#define KEYED_LOG_THROTTLE_HPP

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace server::util
{

//! ДРОССЕЛЬ С КЛЮЧОМ И ФИКСИРОВАННОЙ ЁМКОСТЬЮ (LOA-fix, R71-31, находка ревью 8 #1).
//!
//! ★ЗАЧЕМ ОН ОТДЕЛЬНО ОТ `LogThrottle`. Обычный `LogThrottle` заводится ПО МЕСТУ
//! защиты, и этого достаточно, пока мест конечное число и они известны глазу. У
//! ТОЧКИ РАЗВЕТВЛЕНИЯ — внешнего `catch` диспетчера команд — место ОДНО, а классов
//! события столько, сколько пар «команда x причина броска». Один общий дроссель там
//! гасил бы жалобу на один брошенный хендлер из-за другого; дроссель на каждый класс
//! в виде КАРТЫ был бы утечкой памяти, ключённой клиентом (тот же класс, что #130-C6).
//!
//! ★ПОЭТОМУ ЁМКОСТЬ ФИКСИРОВАНА ПО ПОСТРОЕНИЮ: массив из `SlotCount` вёдер, ключ
//! отображается в ведро ОСТАТКОМ. Памяти ровно `SlotCount` записей, независимо от
//! того, сколько разных ключей придумает клиент. Столкновение двух классов в одном
//! ведре возможно и НАЗВАНО ВСЛУХ: оно стоит одной пропущенной диагностической
//! строки, но никогда не стоит новой памяти.
//!
//! ★ОКНО ПРИНАДЛЕЖИТ ВЕДРУ, А НЕ КЛЮЧУ, И ЭТО ГЛАВНОЕ СВОЙСТВО. Если бы смена ключа
//! в ведре сбрасывала окно, атакующий чередовал бы два ключа и печатал бы строку на
//! КАЖДЫЙ пакет — то есть дроссель, ключённый данными из пакета, сам стал бы
//! флудом. Здесь смена ключа окна НЕ сбрасывает: пока окно ведра открыто, ответ
//! «нельзя» независимо от ключа. Верхняя граница строк за окно — `SlotCount`, и она
//! не зависит ни от чего клиентского.
//!
//! ★СВОЙ ЗАМОК — ЛИСТ: `Allow` не логирует, не зовёт чужой код и не берёт других
//! замков. ★У `LogThrottle` замка НЕТ ВООБЩЕ — с раунда R72 он атомарный
//! (`compare_exchange` над одним словом, см. `LogThrottle.hpp`). Здесь мьютекс
//! всё же нужен: ведро из `SlotCount` ячеек одним словом не обновить.
class KeyedLogThrottle
{
public:
  using Clock = std::chrono::steady_clock;

  //! Сколько разных классов события дроссель различает ОДНОВРЕМЕННО. Это же и
  //! потолок строк за окно.
  static constexpr size_t SlotCount = 64;

  explicit KeyedLogThrottle(Clock::duration window = std::chrono::seconds(5)) noexcept
    : _window(window)
  {
  }

  //! @param key класс события (например, пара «id команды + причина броска»).
  //! @param suppressed [out] сколько жалоб этого ВЕДРА подавлено с прошлого
  //!        разрешения (0, если сейчас жаловаться нельзя).
  //! @returns `true` — жаловаться СЕЙЧАС.
  [[nodiscard]] bool Allow(uint64_t key, uint64_t& suppressed) noexcept;

private:
  struct Slot
  {
    Clock::time_point nextAllowed{};
    uint64_t suppressed{0};
    bool hasLogged{false};
  };

  std::mutex _mutex;
  Clock::duration _window;
  std::array<Slot, SlotCount> _slots{};
};

} // namespace server::util

#endif // KEYED_LOG_THROTTLE_HPP
