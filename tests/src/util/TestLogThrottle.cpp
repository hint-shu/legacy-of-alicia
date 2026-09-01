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


#include <libserver/util/LogThrottle.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace
{

//! ★НЕ `assert`. Образ и юнит-прогон раунда собираются `RelWithDebInfo`, а он
//! несёт `-DNDEBUG` — то есть `assert` выкидывается препроцессором и тест,
//! написанный на нём, ПРОХОДИТ ВСЕГДА. Соседние тесты этим больны; новый
//! наследовать эту болезнь не должен: проверка, которая не умеет провалиться,
//! — не проверка. Своя проверка живёт вне зависимости от NDEBUG.
void Check(
  const bool condition,
  const char* const what,
  const int line)
{
  if (condition)
    return;

  std::fprintf(
    stderr,
    "TestLogThrottle.cpp:%d: ПРОВАЛ — %s\n",
    line,
    what);
  std::exit(1);
}

#define CHECK(cond) Check((cond), #cond, __LINE__)

//! Окно дросселя в тесте. Взято заведомо большим, чем время исполнения тысячи
//! атомарных операций (микросекунды), чтобы «строку выпустило посреди флуда»
//! не могло случиться от планировщика: запас — четыре порядка.
constexpr auto TestWindow = std::chrono::seconds(1);
//! Пауза, гарантированно уводящая часы за окно.
constexpr auto PastWindow = std::chrono::milliseconds(1200);

//! Первая строка проходит, следующая тысяча — нет, и все они посчитаны.
void TestFirstLinePassesAndFloodIsSwallowed()
{
  server::util::LogThrottle throttle(TestWindow);

  uint64_t suppressed = 12345;
  uint64_t total = 12345;

  CHECK(throttle.Allow(suppressed, total));
  CHECK(suppressed == 0);
  CHECK(total == 1);

  for (uint64_t event = 2; event <= 1000; ++event)
  {
    suppressed = 12345;
    total = 12345;
    CHECK(not throttle.Allow(suppressed, total));
    // Проглоченному вызывающему сообщать нечего — он ничего не пишет.
    CHECK(suppressed == 0);
    // ★А ВОТ ПОЛНЫЙ СЧЁТ РАСТЁТ НА КАЖДОМ СОБЫТИИ, включая проглоченные.
    // Именно на этом стоит оракул «честной сессии не отказано ни разу».
    CHECK(total == event);
  }

  // Часы ушли за окно — строка снова разрешена, и она НЕСЁТ число
  // проглоченных с прошлой выпущенной.
  std::this_thread::sleep_for(PastWindow);

  suppressed = 12345;
  total = 12345;
  CHECK(throttle.Allow(suppressed, total));
  CHECK(suppressed == 999);
  CHECK(total == 1001);

  // Счётчик проглоченных обнулился выпуском, накопительный — нет.
  suppressed = 12345;
  total = 12345;
  CHECK(not throttle.Allow(suppressed, total));
  CHECK(suppressed == 0);
  CHECK(total == 1002);

  std::this_thread::sleep_for(PastWindow);

  suppressed = 12345;
  total = 12345;
  CHECK(throttle.Allow(suppressed, total));
  CHECK(suppressed == 1);
  CHECK(total == 1003);
}

} // namespace

int main()
{
  TestFirstLinePassesAndFloodIsSwallowed();
  return 0;
}
