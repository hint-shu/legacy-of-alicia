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


#include "libserver/util/LogThrottle.hpp"

namespace server::util
{

bool LogThrottle::Allow(uint64_t& suppressed, uint64_t& total) noexcept
{
  // ★СЧЁТ СОБЫТИЙ — ПЕРВЫМ И БЕЗУСЛОВНО. Иначе «проглоченное» событие не
  // попадает никуда, и оракул «за легитимную сессию — ноль отказов» слепнет
  // ровно на том, ради чего он заведён.
  total = _total.fetch_add(1, std::memory_order_relaxed) + 1;

  const auto now = Clock::now().time_since_epoch().count();
  auto nextAllowed = _nextAllowed.load(std::memory_order_relaxed);

  // ★ЦИКЛ CAS, А НЕ «load; store». Лобби-поток один, но сам класс общий, и
  // «проверил-записал» двумя операциями пропустило бы N строк на N потоках —
  // ровно тот дефект, ради которого дроссель и заводится.
  while (now >= nextAllowed)
  {
    const auto candidate = now + _window.count();
    if (_nextAllowed.compare_exchange_weak(
          nextAllowed,
          candidate,
          std::memory_order_acq_rel,
          std::memory_order_relaxed))
    {
      suppressed = _suppressed.exchange(0, std::memory_order_acq_rel);
      return true;
    }
  }

  _suppressed.fetch_add(1, std::memory_order_relaxed);
  suppressed = 0;
  return false;
}

} // namespace server::util
