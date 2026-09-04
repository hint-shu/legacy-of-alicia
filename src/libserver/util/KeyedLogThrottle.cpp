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

#include "libserver/util/KeyedLogThrottle.hpp"

namespace server::util
{

bool KeyedLogThrottle::Allow(uint64_t key, uint64_t& suppressed) noexcept
{
  try
  {
    const auto now = Clock::now();
    const std::scoped_lock lock(_mutex);

    Slot& slot = _slots[static_cast<size_t>(key % SlotCount)];

    // ★Окно принадлежит ВЕДРУ. Ключ здесь уже не спрашивается: иначе чередование
    // ключей в одном ведре открывало бы окно на каждый пакет.
    if (slot.hasLogged && now < slot.nextAllowed)
    {
      ++slot.suppressed;
      suppressed = 0;
      return false;
    }

    suppressed = slot.suppressed;
    slot.suppressed = 0;
    slot.nextAllowed = now + _window;
    slot.hasLogged = true;
    return true;
  }
  catch (...)
  {
    // Захват замка умеет бросить (`std::system_error`), а `noexcept` превратил бы
    // это в `std::terminate`. Ответ «сейчас не жалуемся» безопасен: теряется только
    // строка лога. ★`LogThrottle::Allow` этой ветки не имеет и иметь не может:
    // с R72 он атомарный и замка не берёт — бросать там нечему.
    suppressed = 0;
    return false;
  }
}

} // namespace server::util
