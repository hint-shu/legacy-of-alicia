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

void TestCeilingComesFromWhatIsStored()
{
  // ★ГЕЙТ, ОТКАЗЫВАЮЩИЙ ЖИВОМУ ИМЕНИ, ХУЖЕ ОТСУТСТВУЮЩЕГО (правка ревью,
  // итерация 1). Индекс принимает любое имя, лежащее на диске; константа 64
  // отбивала запрос ДО индекса, и персонаж, сохранённый до появления
  // `IsNameValid` с 65-байтовым ASCII-именем, оказывался проиндексирован и при
  // этом вечно неадресуем. Потолок обязан приходить параметром.
  const std::string legacy(65, 'a');
  assert(not IsStorableNameShaped(legacy));                 // потолок по умолчанию
  assert(not IsStorableNameShaped(legacy, kMaxStoredNameBytes));
  assert(IsStorableNameShaped(legacy, 65));                 // потолок поднят индексом
  // Поднятый потолок остаётся КОНЕЧНЫМ: 8190 байт с провода отбиваются и при нём.
  assert(not IsStorableNameShaped(std::string(8000, 'a'), 65));
  // Структурные отказы поднятие потолка не отменяет.
  assert(not IsStorableNameShaped(std::string(60, 'a') + "/x", 65));

  const std::string legacyLogin(49, 'b');
  assert(not IsLoginNameSafe(legacyLogin));
  assert(not IsLoginNameSafe(legacyLogin, kMaxLoginNameBytes));
  assert(IsLoginNameSafe(legacyLogin, 49));
  assert(not IsLoginNameSafe(std::string(8000, 'b'), 49));
  // Класс символов поднятие потолка не расширяет.
  assert(not IsLoginNameSafe(std::string(40, 'b') + "/x", 49));

  // ★КЛАСС ПОИСКА ШИРЕ КЛАССА РЕГИСТРАЦИИ, И ЭТО НАМЕРЕННО (правка ревью,
  // итерация 3). Индекс аккаунтов строится из имён ФАЙЛОВ и никакого allowlist
  // не требует: `data/users/john.doe.json` попадает в него как `john.doe`.
  // Если поиск охраняет `IsLoginNameSafe`, такой аккаунт становится
  // неадресуемым для staff-команды — гейт строже индекса, который он охраняет.
  // Строгий allowlist остаётся на входе и регистрации, где решается, какие
  // имена МОЖНО ЗАВЕСТИ.
  const std::string_view legacyStem = "john.doe";
  assert(not IsLoginNameSafe(legacyStem));                    // завести нельзя
  assert(IsStorableNameShaped(legacyStem, kMaxLoginNameBytes));// а найти нужно
  // Разделитель пути не пролезает и через широкий класс.
  assert(not IsStorableNameShaped("../etc/passwd", kMaxLoginNameBytes));
  assert(not IsStorableNameShaped(
    std::string_view("john\0doe", 8), kMaxLoginNameBytes));
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
  // 8000-байтному имени — работа на десятки микросекунд (длина отсекается
  // первой, содержимое не читается вовсе).
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

  // ★ПОРОГ РЕГРЕССИОННЫЙ, А НЕ МИКРОБЕНЧМАРК (правка ревью, итерация 7).
  // Прежняя миллисекунда — это НАСТЕННОЕ время: вытеснение планировщиком на
  // загруженной машине сборки роняет её и на совершенно правильном коде, то
  // есть тест умел быть ложно-КРАСНЫМ, а такой отключают. Порог поднят до
  // величины, которую невозможно превысить случайно: 250 мс на тысячу
  // отказов — это в тысячу раз больше измеренной стоимости, но всё ещё в
  // тысячи раз меньше, чем обход каталога, ради снятия которого гейт заведён.
  // Регрессия класса «гейт снова читает всё имя» видна и на этом пороге.
  assert(elapsed < std::chrono::milliseconds(250));
}

} // namespace

//! ★ГЕЙТ ПОИСКА ОГРАНИЧЕН, НО НЕ СТРОГ — И ЭТО РАЗНЫЕ НАПРАВЛЕНИЯ (ревью, 9).
//!
//! `IsStorableNameShaped` отвергает `/` и `\` правильно: он решает, какие имена
//! ВПРЕДЬ ЗАВОДИТЬ. Но имя персонажа — ПОЛЕ ВНУТРИ файла, названного uid'ом, и
//! тот же гейт, поставленный в ПОИСК, делал уже лежащего на диске `A/B` вечно
//! неадресуемым. Здесь проверено, что гейты разошлись и каждый делает своё.
void TestLookupKeyGuardIsBoundedButNotStrict()
{
  using server::util::IsLookupKeyShaped;
  using server::util::IsStorableNameShaped;

  // То, что лежит на диске, ищется — даже когда так больше не назвать.
  assert(IsLookupKeyShaped("A/B"));
  assert(IsLookupKeyShaped("C\\D"));
  assert(IsLookupKeyShaped("имя с пробелом"));
  assert(IsLookupKeyShaped(std::string("tab\there")));
  // ...и создание при этом осталось строгим: направления не сошлись в одно.
  assert(not IsStorableNameShaped("A/B"));
  assert(not IsStorableNameShaped("C\\D"));

  // Но поиск ОГРАНИЧЕН: пусто, длиннее потолка, и то, что рвёт строку журнала
  // или обрывает C-строку.
  assert(not IsLookupKeyShaped(""));
  assert(not IsLookupKeyShaped(std::string(65, 'x')));
  assert(IsLookupKeyShaped(std::string(64, 'x')));
  assert(not IsLookupKeyShaped(std::string("a\nb")));
  assert(not IsLookupKeyShaped(std::string("a\rb")));
  assert(not IsLookupKeyShaped(std::string("a\0b", 3)));

  // Потолок — параметр, ровно как у соседа: имя длиннее сегодняшней константы
  // обязано быть спрашиваемым, если оно лежит в индексе.
  assert(IsLookupKeyShaped(std::string(100, 'x'), 128));
  assert(not IsLookupKeyShaped(std::string(100, 'x'), 64));
}

int main()
{
  TestLookupKeyGuardIsBoundedButNotStrict();
  TestLoginNameSafe();
  TestStorableNameShaped();
  TestCeilingComesFromWhatIsStored();
  TestCaseFolding();
  TestGateCannotRefuseALegalName();
  TestBudget();
  std::puts("TestNameGuard: ok");
}
