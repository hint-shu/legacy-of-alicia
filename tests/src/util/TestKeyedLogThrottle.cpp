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

//! Юнит-тест дросселя с ключом (LOA-fix, R71-31, находка ревью 8 #1, BLOCK).
//!
//! ★ГЛАВНОЕ УТВЕРЖДЕНИЕ ЗДЕСЬ — НЕ «ДРОССЕЛЬ ДРОССЕЛИРУЕТ», А «КЛЮЧ ИЗ ПАКЕТА НЕ
//! ОТКРЫВАЕТ ОКНО». Дроссель, ключённый данными, которые выбирает клиент, — это
//! обычно НОВЫЙ флуд, а не защита от него: достаточно менять ключ на каждом пакете.
//! Проверка написана ровно на этот дефект: сделай окно принадлежащим КЛЮЧУ (сбрасывай
//! состояние ведра при смене ключа) — и утверждение о потолке строк краснеет.
#include "libserver/util/KeyedLogThrottle.hpp"

#include <cstdio>
#include <chrono>
#include <thread>

namespace
{

int failures = 0;

void Check(const bool condition, const char* what)
{
  if (condition)
    return;
  std::printf("FAIL: %s\n", what);
  ++failures;
}

//! Первая жалоба каждого ведра проходит, повторная в том же окне — нет.
void TestFirstAllowedThenSuppressed()
{
  server::util::KeyedLogThrottle throttle{std::chrono::seconds(5)};

  uint64_t suppressed = 42;
  Check(throttle.Allow(1, suppressed), "первая жалоба обязана пройти");
  Check(suppressed == 0, "подавленных до первой жалобы нет");

  Check(not throttle.Allow(1, suppressed), "вторая жалоба в окне обязана быть подавлена");
  Check(suppressed == 0, "подавленная жалоба не сообщает счётчик");
}

//! ★ЧЕРЕДОВАНИЕ КЛЮЧЕЙ НЕ ДАЁТ СТРОКУ НА ПАКЕТ.
void TestKeyFlippingCannotFlood()
{
  server::util::KeyedLogThrottle throttle{std::chrono::seconds(5)};

  int allowed = 0;
  uint64_t suppressed = 0;
  // 10 000 пакетов, ключ меняется на каждом — ровно та атака, ради которой ключ и
  // опасен. Строк не может быть больше, чем вёдер.
  for (int i = 0; i < 10000; ++i)
  {
    if (throttle.Allow(static_cast<uint64_t>(i), suppressed))
      ++allowed;
  }

  Check(
    allowed <= static_cast<int>(server::util::KeyedLogThrottle::SlotCount),
    "★строк за окно не больше, чем вёдер: ключ из пакета не имеет права открывать окно");
  Check(allowed > 0, "полностью немой дроссель — это гард, судимый по молчанию");
}

//! Разные классы события различимы: два ключа, попавшие в РАЗНЫЕ вёдра, жалуются
//! независимо — иначе один брошенный хендлер глушил бы диагностику другого.
void TestDistinctKeysAreIndependent()
{
  server::util::KeyedLogThrottle throttle{std::chrono::seconds(5)};

  uint64_t suppressed = 0;
  Check(throttle.Allow(0, suppressed), "первый класс обязан пожаловаться");
  Check(throttle.Allow(1, suppressed), "второй класс — своё ведро, своё окно");
  Check(not throttle.Allow(0, suppressed), "повтор первого класса подавлен");
}

//! Счётчик подавленных возвращается по истечении окна — без него «одна строка»
//! неотличима от «одна попытка».
void TestSuppressedCountIsReported()
{
  server::util::KeyedLogThrottle throttle{std::chrono::milliseconds(50)};

  uint64_t suppressed = 0;
  Check(throttle.Allow(7, suppressed), "первая жалоба проходит");

  for (int i = 0; i < 5; ++i)
    Check(not throttle.Allow(7, suppressed), "жалобы внутри окна подавлены");

  std::this_thread::sleep_for(std::chrono::milliseconds(80));

  Check(throttle.Allow(7, suppressed), "после окна жалоба снова проходит");
  Check(suppressed == 5, "★число подавленных обязано вернуться — это и есть мера атаки");
}

} // namespace

int main()
{
  TestFirstAllowedThenSuppressed();
  TestKeyFlippingCannotFlood();
  TestDistinctKeysAreIndependent();
  TestSuppressedCountIsReported();

  if (failures != 0)
  {
    std::printf("TestKeyedLogThrottle: %d FAILURE(S)\n", failures);
    return 1;
  }

  std::puts("TestKeyedLogThrottle: OK");
  return 0;
}
