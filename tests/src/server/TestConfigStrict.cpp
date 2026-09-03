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

//! LOA (R70-fix-8, backlog #58, находка Codex 6 BLOCK-2): СТРОГИЙ РАЗБОР
//! ЗНАЧЕНИЯ КОНФИГА.
//!
//! ★ЗАЧЕМ ЮНИТ-ТЕСТ. Обещание «плохое значение — ОТКАЗ СТАРТА» держалось на
//! комментарии: `from_chars` не требовала съесть всю строку (`20junk` → 20),
//! явно пустое значение читалось как «ключ не задан», а YAML-ветку глотал
//! секционный `catch`. Каждая из этих трёх дыр выглядела одинаково снаружи:
//! сервер стартовал с 900 секундами вместо поданных двадцати, стенд не видел
//! протухания и объявлял фичу проверенной. Ложно-зелёный ровно там, где ключ
//! и заведён.
//!
//! ★ПОЧЕМУ НЕ `assert`: образ собирается Release, `NDEBUG` гасит `assert`
//! целиком — тест, который не умеет провалиться, читается как зелёный.

#include "server/ConfigStrict.hpp"

#include <cstdio>
#include <string_view>

namespace
{

int g_failures = 0;

void Check(const bool condition, const char* const what)
{
  if (condition)
    return;
  std::fprintf(stderr, "FAIL: %s\n", what);
  ++g_failures;
}

//! ★ГЕЙТ ОБЯЗАН СПЕРВА ДОКАЗАТЬ СЕБЯ.
void TestCheckerCanFail()
{
  const int before = g_failures;
  Check(false, "(намеренный провал самопроверки — так и должно быть)");
  const bool counted = g_failures == before + 1;
  g_failures = before;
  Check(counted, "Check() не считает провалы — тесту нельзя верить");
}

//! Отвергнуто ли значение? Ловим ИМЕННО `ConfigError`: тип важен, потому что
//! только он переживает секционные перехваты в `Config::LoadFromFile`.
bool Rejected(const std::string_view value)
{
  try
  {
    (void)server::ParseStrictPositiveSeconds("test.key", value);
  }
  catch (const server::ConfigError&)
  {
    return true;
  }
  catch (...)
  {
    return false;
  }
  return false;
}

void TestValidValuesAccepted()
{
  Check(
    server::ParseStrictPositiveSeconds("test.key", "20") == 20,
    "«20» обязано разобраться в двадцать");
  Check(
    server::ParseStrictPositiveSeconds("test.key", "900") == 900,
    "умолчание «900» обязано разбираться");
  Check(
    server::ParseStrictPositiveSeconds("test.key", "1") == 1,
    "минимальный допустимый срок — одна секунда");
}

//! ★ХВОСТ ПОСЛЕ ЧИСЛА — ОТКАЗ. Именно эту форму пропускала прежняя проверка:
//! `from_chars` возвращала успех, съев только «20».
void TestTrailingGarbageRejected()
{
  Check(Rejected("20junk"), "«20junk» обязано быть ОТВЕРГНУТО, а не прочитано как 20");
  Check(Rejected("20 "), "хвостовой пробел обязан отвергаться тем же правилом");
  Check(Rejected("20s"), "суффикс единиц не поддерживается и обязан отвергаться");
}

//! ★ЯВНО ПУСТОЕ ЗНАЧЕНИЕ — ОШИБКА, А НЕ «КЛЮЧА НЕТ». `FOO=` в среде и `key:`
//! в YAML — это опечатка; трактовка «как будто не задано» превращала бы её в
//! умолчание молча.
void TestEmptyRejected()
{
  Check(Rejected(""), "пустое значение обязано быть ОТВЕРГНУТО, а не принято за умолчание");
}

void TestZeroAndNegativeRejected()
{
  Check(Rejected("0"), "ноль обязан быть отвергнут: удержание с нулевым сроком ничего не держит");
  Check(Rejected("-5"), "отрицательное значение обязано быть отвергнуто");
  Check(Rejected("-0"), "«-0» обязано быть отвергнуто, а не прочитано как ноль-и-ладно");
}

//! ★ВЕДУЩИЙ ПРОБЕЛ. `from_chars` его не пропускает (в отличие от `strtoul`),
//! но правило держалось бы на вере в стандартную библиотеку — здесь оно
//! измерено.
void TestLeadingSpaceRejected()
{
  Check(Rejected(" 20"), "« 20» обязано быть отвергнуто: строка съедена не вся");
  Check(Rejected("+20"), "«+20» обязано быть отвергнуто тем же правилом");
}

} // namespace

int main()
{
  TestCheckerCanFail();
  TestValidValuesAccepted();
  TestTrailingGarbageRejected();
  TestEmptyRejected();
  TestZeroAndNegativeRejected();
  TestLeadingSpaceRejected();

  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
