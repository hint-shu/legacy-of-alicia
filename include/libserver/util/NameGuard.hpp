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

#ifndef NAME_GUARD_HPP
#define NAME_GUARD_HPP

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace server::util
{

//! Потолок длины имени, которое ВООБЩЕ может быть сохранено (LOA-fix R73-3, #130-C8).
//!
//! ★ВЕЛИЧИНА ВЫВЕДЕНА, А НЕ ВЫБРАНА, и вывод обязан быть здесь: гейт, который
//! может отказать ЗАКОННОМУ имени, хуже отсутствующего. `locale::IsNameValid`
//! (Locale.cpp:150, cap по умолчанию 18 — Locale.hpp:52) пропускает не более 18
//! БАЙТ EUC-KR. Хангыль стоит 2 байта EUC-KR (`EucKrWideByteCount`,
//! Locale.cpp:31) и 3 байта UTF-8 → максимум 27 байт UTF-8; кириллица входит в
//! `LatinLettersPattern` (Locale.cpp:38, `[А-Яа-яЁёA-Za-z0-9()\[\]{}]`) и
//! считается УЗКОЙ (1 байт EUC-KR, Locale.cpp:32), а в UTF-8 стоит 2 байта →
//! максимум 36 байт UTF-8. Худший случай — 36. 64 — потолок с запасом, который
//! отказать законному имени НЕ МОЖЕТ. Провод при этом даёт до ~8190 байт
//! (Stream.cpp:99-118 читает до NUL, CommandServer.cpp:38 `MaxCommandDataSize`
//! = 8192), то есть отсекаемое здесь на два порядка длиннее всего, что сервер
//! способен хранить.
inline constexpr std::size_t kMaxStoredNameBytes = 64;

//! Потолок длины имени логина — ровно тот, что уже стоит в проверке #18b
//! (LocalAuthenticationBackend.cpp:128).
inline constexpr std::size_t kMaxLoginNameBytes = 48;

//! ASCII-свёртка одного байта в нижний регистр.
//!
//! ★СТРУКТУРНО, А НЕ ЧЕРЕЗ `std::tolower`. Поведение `std::tolower` для байтов
//! ≥ 0x80 зависит от ГЛОБАЛЬНОЙ C-локали. Сегодня процесс её не ставит (единственный
//! `std::locale("")` в дереве — локальный, MessengerDirector.cpp:264), поэтому
//! `tolower` здесь и сейчас ASCII. Но ключ индекса имён — ДОЛГОЖИВУЩЕЕ состояние в
//! памяти: если завтрашний раунд или транзитивно слинкованная библиотека поставит
//! локаль, ключи, построенные до и после, разошлись бы для кириллицы и хангыля, и
//! имена молча перестали бы находиться. Утверждение «локали нет» — не гарантия;
//! это — гарантия.
[[nodiscard]] constexpr char AsciiToLower(const char symbol) noexcept
{
  return (symbol >= 'A' && symbol <= 'Z')
    ? static_cast<char>(symbol - 'A' + 'a')
    : symbol;
}

//! Имя логина: строгий allowlist `[A-Za-z0-9_-]` + потолок длины.
//!
//! ★ЭТО НЕ НОВЫЙ КЛАСС, А ПЕРЕЕЗД СУЩЕСТВУЮЩЕГО. Точно такая же проверка
//! написана внутри `LocalAuthenticationBackend::Authenticate` (:128-138) — и
//! ровно поэтому КАЖДОЕ имя файла в `data/users` принадлежит этому классу по
//! построению. Пока определений было два, они умели разъехаться молча.
[[nodiscard]] inline bool IsLoginNameSafe(const std::string_view name) noexcept
{
  if (name.empty() || name.size() > kMaxLoginNameBytes)
    return false;
  return std::ranges::all_of(name, [](const char symbol)
    {
      return (symbol >= 'A' && symbol <= 'Z')
        || (symbol >= 'a' && symbol <= 'z')
        || (symbol >= '0' && symbol <= '9')
        || symbol == '_' || symbol == '-';
    });
}

//! Структурный гейт имени персонажа/гильдии/питомца: «это ВООБЩЕ может быть
//! сохранённым именем?».
//!
//! ★НАМЕРЕННО НЕ `locale::IsNameValid`. Класс символов l10n (Locale.cpp:38)
//! менялся вместе с раундами (#29 добавил кириллицу), и имя, СОХРАНЁННОЕ до
//! этих правок, законно живёт на диске, не проходя сегодняшнюю проверку.
//! Прогнать поиск через `IsNameValid` значило бы сделать такого персонажа
//! неадресуемым — обменять путь успеха на путь отказа. Здесь проверяется только
//! то, что имя ФИЗИЧЕСКИ не может быть именем на диске: пустое, длиннее
//! потолка, с управляющим байтом или с разделителем пути.
[[nodiscard]] inline bool IsStorableNameShaped(const std::string_view name) noexcept
{
  if (name.empty() || name.size() > kMaxStoredNameBytes)
    return false;
  return std::ranges::none_of(name, [](const char symbol)
    {
      const auto byte = static_cast<unsigned char>(symbol);
      return byte < 0x20 || byte == 0x7f || byte == '/' || byte == '\\';
    });
}

//! Сравнение имён без учёта регистра ПО ASCII.
//!
//! ★РЕГИСТР ОСТАЁТСЯ ASCII-ONLY НАМЕРЕННО: `std::regex_constants::icase` на
//! байтовом `std::regex` (то, что стояло здесь до раунда) складывает регистр
//! ТОЛЬКО у ASCII, значит `Аня` и `аня` были разными именами и до раунда. Этот
//! раунд не меняет, какие имена считаются одинаковыми, — он меняет только то,
//! ЧЕМ они сравниваются. Улика того же свойства живёт на проде: аккаунты
//! `NighteWolf` и `NighteWOlf` — два РАЗНЫХ файла (ext4 регистрозависим).
[[nodiscard]] inline bool EqualsAsciiIgnoreCase(
  const std::string_view lhs, const std::string_view rhs) noexcept
{
  // `std::ranges::equal` на sized-диапазонах сперва сверяет ДЛИНУ, поэтому
  // «префикс равен» никогда не читается как «равно» (это и был дефект `name.*`).
  return std::ranges::equal(lhs, rhs,
    [](const char a, const char b)
    { return AsciiToLower(a) == AsciiToLower(b); });
}

//! Ключ индекса: имя, свёрнутое в нижний ASCII-регистр.
[[nodiscard]] inline std::string AsciiLowerKey(const std::string_view name)
{
  std::string key(name);
  std::ranges::transform(key, key.begin(), AsciiToLower);
  return key;
}

} // namespace server::util

#endif // NAME_GUARD_HPP
