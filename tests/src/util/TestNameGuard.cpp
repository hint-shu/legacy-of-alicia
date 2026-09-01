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

// ★ПРОВЕРКА, КОТОРАЯ НЕ УМЕЕТ ОСЛЕПНУТЬ. `assert` под `-DNDEBUG` исчезает
// целиком, и тест «проходит», ничего не проверив. Снимаем NDEBUG до включения
// <cassert>, чтобы результат не зависел от типа сборки.
#undef NDEBUG

#include <libserver/util/NameGuard.hpp>
#include <libserver/util/Locale.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>

namespace
{

using namespace server::util;

void TestLoginNameSafe()
{
  assert(IsLoginNameSafe("loatest-1"));
  assert(IsLoginNameSafe("NighteWOlf"));
  assert(IsLoginNameSafe("a"));
  assert(IsLoginNameSafe(std::string(48, 'a')));

  assert(not IsLoginNameSafe(""));
  assert(not IsLoginNameSafe(std::string(49, 'a')));
  assert(not IsLoginNameSafe("na/me"));
  assert(not IsLoginNameSafe("na..me"));
  // Кириллица — законное имя ПЕРСОНАЖА, но не имя файла аккаунта (#18b).
  assert(not IsLoginNameSafe("\xD0\xB8\xD0\xBC\xD1\x8F"));
  // NUL внутри имени: длина берётся из string_view, а не из C-строки.
  assert(not IsLoginNameSafe(std::string_view("a\0b", 3)));
}

void TestStorableNameShaped()
{
  assert(IsStorableNameShaped("Ann(1)"));
  // «Конь» в UTF-8 — 8 байт, законное имя после #29.
  assert(IsStorableNameShaped("\xD0\x9A\xD0\xBE\xD0\xBD\xD1\x8C"));
  assert(IsStorableNameShaped("([]{})"));
  assert(IsStorableNameShaped(std::string(64, 'a')));   // ровно потолок

  assert(not IsStorableNameShaped(""));
  assert(not IsStorableNameShaped(std::string(65, 'a')));
  assert(not IsStorableNameShaped(std::string(8000, 'a')));
  assert(not IsStorableNameShaped("a/b"));
  assert(not IsStorableNameShaped("a\\b"));
  assert(not IsStorableNameShaped("a\rb"));
  assert(not IsStorableNameShaped("a\x7f"));
  assert(not IsStorableNameShaped(std::string_view("a\x01" "b", 3)));
}

void TestCaseFolding()
{
  assert(AsciiToLower('A') == 'a');
  assert(AsciiToLower('z') == 'z');
  assert(AsciiToLower('(') == '(');

  assert(EqualsAsciiIgnoreCase("Ann", "ANN"));
  assert(EqualsAsciiIgnoreCase("", ""));
  // ★ДЛИНА СВЕРЯЕТСЯ ПЕРВОЙ. Это и был дефект шаблона `name.*`: префикс
  // читался как совпадение.
  assert(not EqualsAsciiIgnoreCase("Ann", "Anna"));
  assert(not EqualsAsciiIgnoreCase("Anna", "Ann"));
  assert(not EqualsAsciiIgnoreCase("nik", "nikross"));

  assert(AsciiLowerKey("Ann(1)") == "ann(1)");
  // ★M16-ГАРАНТИЯ КАК ТЕСТ, А НЕ КАК УТВЕРЖДЕНИЕ: байты ≥ 0x80 не трогаются
  // вообще, поэтому ключ индекса не зависит от глобальной локали.
  assert(AsciiLowerKey("\xD0\x90") == "\xD0\x90");
  assert(AsciiLowerKey("\xEA\xB0\x80") == "\xEA\xB0\x80");
}

void TestGateCannotRefuseALegalName()
{
  // ★МАССИВ СКОПИРОВАН ДОСЛОВНО ИЗ `TestLocale.cpp`, а не выведен заново:
  // именно эти имена сервер считает законными, и структурный гейт обязан
  // пропустить КАЖДОЕ. Гейт, который может отказать законному имени, хуже
  // отсутствующего.
  constexpr std::array validNames = {
    // ASCII range boundaries
    "AZaz09",
    // Accepted punctuation
    "Name(1)",
    "Name[2]",
    "Name{3}",
    "([]{})",
    // Korean range boundaries: U+AC00 and U+D7A3
    "\xEA\xB0\x80" "\xED\x9E\xA3",
    // Mixed Korean, latin and digit characters
    "\xEA\xB0\x80" "A0",
    // LOA-fix (#29): 18 узких символов — новая граница cap (валидно при 18).
    "ABCDEFGHIJKLMNOPQR",
    // LOA-fix (#29): кириллические имена теперь принимаются (U+0400-04FF, «Конь»).
    "\xD0\x9A" "\xD0\xBE" "\xD0\xBD" "\xD1\x8C"};

  for (const auto& entry : validNames)
  {
    assert(server::locale::IsNameValid(entry) == true);
    assert(IsStorableNameShaped(entry) == true);
  }
}

void TestBudget()
{
  // Гейт обязан быть дешевле любого обращения к диску: 1000 проходов по
  // 8000-байтному имени укладываются в миллисекунду (длина отсекается первой).
  const std::string huge(8000, 'a');
  const auto started = std::chrono::steady_clock::now();
  std::size_t rejected = 0;
  for (int i = 0; i < 1000; ++i)
  {
    if (not IsStorableNameShaped(huge))
      ++rejected;
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  assert(rejected == 1000);
  assert(elapsed < std::chrono::milliseconds(1));
}

} // namespace

int main()
{
  TestLoginNameSafe();
  TestStorableNameShaped();
  TestCaseFolding();
  TestGateCannotRefuseALegalName();
  TestBudget();
  std::puts("TestNameGuard: ok");
}
