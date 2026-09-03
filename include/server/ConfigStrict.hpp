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

#ifndef CONFIGSTRICT_HPP
#define CONFIGSTRICT_HPP

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace server
{

//! LOA (R70-fix-8, backlog #58, находка Codex 6 BLOCK-2): ★ОШИБКА СТРОГОГО
//! КЛЮЧА КОНФИГА — ОТДЕЛЬНЫЙ ТИП, А НЕ `std::runtime_error`.
//!
//! ЗАЧЕМ ОТДЕЛЬНЫЙ ТИП. `Config::LoadFromFile` разбирает конфиг посекционно и
//! КАЖДУЮ секцию заворачивает в `catch (const std::exception&)` с записью в
//! лог — намеренно: сломанная секция мессенджера не должна ронять сервер
//! целиком. Но ровно этот перехват и обессмысливал обещание «плохое значение —
//! отказ старта»: исключение съедалось, старт продолжался со СТАРЫМ значением,
//! и стенд, поставивший 20 секунд, молча получал 900 и объявлял «протухания
//! нет» — ложно-зелёный ровно там, где ключ и заведён.
//! Отдельный тип позволяет секционному (и внешнему) перехвату ПЕРЕБРОСИТЬ
//! именно строгие ключи, не трогая снисходительность ко всему остальному.
class ConfigError final : public std::runtime_error
{
public:
  ConfigError(const std::string_view key, const std::string_view reason)
    : std::runtime_error(
        std::string("configuration key '").append(key).append("' is invalid: ").append(reason))
  {
  }
};

//! Строгий разбор положительного числа секунд.
//!
//! ★ПОЧЕМУ «СТРОГИЙ» — ЭТО ТРИ ОТДЕЛЬНЫХ ТРЕБОВАНИЯ, И КАЖДОЕ УМЕЕТ ТИХО
//! ИСПОРТИТЬСЯ ПО-СВОЕМУ:
//!   • `from_chars` обязана СЪЕСТЬ ВСЮ строку. Без проверки `result.ptr`
//!     значение `20junk` разбиралось бы в 20 — «сработало», но не то, что
//!     написал человек, и молча;
//!   • ПУСТАЯ строка — ошибка, а не «ключа нет». Явно поданное пустое значение
//!     (`FOO=` в среде, `key:` в YAML) — это опечатка, а трактовка «как будто
//!     не задано» превращает опечатку в умолчание;
//!   • НОЛЬ и отрицательное отвергаются здесь же. Ноль означал бы «срок
//!     истёк мгновенно», то есть удержание, которое ничего не удерживает;
//!     минус `from_chars` для `uint32_t` не примет, но сообщение об ошибке
//!     обязано быть про значение, а не про тип.
//! ★Ведущие пробелы `from_chars` НЕ пропускает (в отличие от `strtoul`),
//!   поэтому " 20" отвергается тем же правилом «съесть всю строку» — не нужно
//!   отдельной ветки, но нужен тест, иначе правило держится на вере в libc++.
//!
//! @param key Имя ключа — попадает в текст исключения, чтобы отказ старта
//!        называл виновника.
//! @param value Поданное значение КАК ЕСТЬ.
//! @returns Разобранное число секунд (всегда > 0).
//! @throws ConfigError на любом невалидном значении.
[[nodiscard]] inline uint32_t ParseStrictPositiveSeconds(
  const std::string_view key,
  const std::string_view value)
{
  if (value.empty())
    throw ConfigError(key, "value is empty (remove the key to use the default)");

  uint32_t seconds = 0;
  const auto* const begin = value.data();
  const auto* const end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, seconds);

  if (result.ec != std::errc{} or result.ptr != end)
    throw ConfigError(key, "value is not a whole positive number of seconds");
  if (seconds == 0)
    throw ConfigError(key, "value must be at least 1 second");

  return seconds;
}

} // namespace server

#endif // CONFIGSTRICT_HPP
