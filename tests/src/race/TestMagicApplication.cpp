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

//! Юнит-тест классификации типа магии (LOA-fix, R71-25, находка ревью 4 #1).
//!
//! ★ЗАЧЕМ. `ClassifyMagicApplication` решает, что вообще разрешено делать по
//! экземпляру эффекта: серверный эффект не разрешает НИ ОДНОГО клиентского отчёта,
//! стена одноразова, атака применяется только по отчёту цели. Пока классификатор жил
//! в анонимном пространстве имён внутри хендлера, проверить его можно было только
//! глазами в ревью — и ревью нашло в нём две дыры сразу: усечение `uint32_t` из
//! конфига до `uint16_t` и фейл-оупен на неизвестном типе.
//!
//! ★ГЛАВНОЕ ЗДЕСЬ — ХВОСТ. Проверяется не только «известные типы разложены верно», но
//! и «неизвестное НЕ становится атакой»: значение шире 16 бит (0x10002), значение из
//! дыры между семействами (26), нулевой тип и максимум домена.
//!
//! ★`assert` НЕ ИСПОЛЬЗУЕТСЯ: боевой образ собирается с -DNDEBUG.
#include "server/race/MagicApplication.hpp"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>

namespace
{

using server::race::ClassifyMagicApplication;
using server::race::MagicApplication;

int failures = 0;

void Check(const bool condition, const char* what)
{
  if (not condition)
  {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

void CheckClass(
  const uint32_t magicType,
  const MagicApplication expected,
  const char* what)
{
  if (ClassifyMagicApplication(magicType) != expected)
  {
    std::printf("FAIL: тип %u — %s\n", magicType, what);
    ++failures;
  }
}

//! Сервер вешает эти эффекты сам при касте: щиты, бустеры, разгон, командные бафы.
void TestServerAppliedFamilies()
{
  for (uint32_t type = 4; type <= 9; ++type)
    CheckClass(type, MagicApplication::ServerAppliedAtCast, "щит/бустер/разгон вешает сервер");
  for (uint32_t type = 20; type <= 25; ++type)
    CheckClass(type, MagicApplication::ServerAppliedAtCast, "командный баф вешает сервер");
}

//! Ледяная стена — препятствие со своей веткой отчёта.
void TestIceWallFamily()
{
  CheckClass(10, MagicApplication::IceWallObstacle, "обычная ледяная стена");
  CheckClass(11, MagicApplication::IceWallObstacle, "критическая ледяная стена");
}

//! Атаки: применяются ТОЛЬКО по отчёту цели.
void TestAttackFamilies()
{
  for (const uint32_t type : {2u, 3u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u})
    CheckClass(type, MagicApplication::TargetReportedAttack, "атака применяется по отчёту цели");
}

//! ★ЭТО И ЕСТЬ НАХОДКА РЕВЬЮ 4 #1. Вернуть терминальный
//! `return TargetReportedAttack` вместо `default: return Unknown` — и КАЖДАЯ проверка
//! ниже покраснеет: неизвестный тип снова получит права атаки.
void TestUnknownTypesAreNotAttacks()
{
  // Значение шире 16 бит: пока тип усекался до `uint16_t`, 0x10002 был неотличим от
  // FireBall'а (тип 2), и отчёт по «типу 2» совпадал с усечённой записью реестра.
  CheckClass(0x10002, MagicApplication::Unknown, "широкий тип не имеет права стать FireBall'ом");
  Check(
    ClassifyMagicApplication(0x10002) != ClassifyMagicApplication(2),
    "0x10002 и 2 обязаны классифицироваться по-разному");
  CheckClass(0x1000A, MagicApplication::Unknown, "широкий тип не имеет права стать стеной");
  CheckClass(0x10004, MagicApplication::Unknown, "широкий тип не имеет права стать серверным");

  // Дыры между семействами и края домена.
  CheckClass(0, MagicApplication::Unknown, "нулевой тип неизвестен");
  CheckClass(1, MagicApplication::Unknown, "тип 1 в конфиге отсутствует");
  CheckClass(26, MagicApplication::Unknown, "тип 26 в конфиге отсутствует");
  // 27 в `magic.yaml` есть, но выдать его нельзя (нулевые веса, `skillEffectId 99999`).
  CheckClass(27, MagicApplication::Unknown, "невыдаваемый тип 27 не имеет права быть атакой");
  CheckClass(28, MagicApplication::Unknown, "тип за концом конфига неизвестен");
  CheckClass(
    std::numeric_limits<uint32_t>::max(),
    MagicApplication::Unknown,
    "максимум домена неизвестен");
}

} // namespace

int main()
{
  TestServerAppliedFamilies();
  TestIceWallFamily();
  TestAttackFamilies();
  TestUnknownTypesAreNotAttacks();

  if (failures != 0)
  {
    std::printf("TestMagicApplication: %d FAILURE(S)\n", failures);
    return 1;
  }

  std::puts("TestMagicApplication: OK");
  return 0;
}
