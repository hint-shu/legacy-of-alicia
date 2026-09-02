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
  return AllowAt(Clock::now().time_since_epoch().count(), suppressed, total);
}

bool LogThrottle::AllowAt(
  const Clock::rep now,
  uint64_t& suppressed,
  uint64_t& total) noexcept
{
  // ★СЧЁТ СОБЫТИЙ — ПЕРВЫМ И БЕЗУСЛОВНО. Иначе «проглоченное» событие не
  // попадает никуда, и оракул «за легитимную сессию — ноль отказов» слепнет
  // ровно на том, ради чего он заведён.
  total = _total.fetch_add(1, std::memory_order_relaxed) + 1;

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
      // LOA-fix (R72-fix2-5, round72, находка Codex 5): ★ПАРА ЧИСЕЛ В ОДНОЙ
      // ВЫПУЩЕННОЙ СТРОКЕ ОБЯЗАНА БЫТЬ СОГЛАСОВАННОЙ.
      //
      // Раньше `total` брался из СВОЕГО `fetch_add` в начале функции, а
      // `suppressed` — из обмена здесь. Между этими двумя точками другие
      // потоки успевали и посчитаться в `_total`, и записаться в
      // `_suppressed`, поэтому победитель мог напечатать «проглочено 1, всего
      // 1» — то есть отчёт, из которого следует, что проглоченного события не
      // было вовсе. Оракул раунда судит именно по этим числам, и
      // несогласованная пара — это ложная улика.
      //
      // ★ПОРЯДОК ОПЕРАЦИЙ ДАЁТ ГАРАНТИЮ, А НЕ УДАЧУ. Проглотивший поток
      // увеличивает `_total` (шаг выше по функции), а ЗАТЕМ `_suppressed`
      // с `release`. Победитель читает `_suppressed` обменом с `acquire` —
      // значит всё, что проглотивший сделал ДО своей release-операции, ему
      // видно, — и только после этого перечитывает `_total`. Отсюда
      // `total >= suppressed + 1` всегда (единица — собственное событие
      // победителя).
      suppressed = _suppressed.exchange(0, std::memory_order_acq_rel);
      total = _total.load(std::memory_order_relaxed);
      return true;
    }
  }

  // ★`release`, А НЕ `relaxed`: это вторая половина пары синхронизации,
  // описанной выше. С `relaxed` победитель мог бы увидеть увеличенный
  // `_suppressed`, но ещё не увидеть увеличенный `_total`.
  _suppressed.fetch_add(1, std::memory_order_release);
  suppressed = 0;
  return false;
}

} // namespace server::util
