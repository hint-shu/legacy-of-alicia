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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

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

//! ★ОТРИЦАТЕЛЬНАЯ ЭПОХА `steady_clock` (R72-fix-1, находка Codex 6).
//!
//! Стандарт НЕ обещает, что `steady_clock::now().time_since_epoch()`
//! неотрицательна: эпоха не специфицирована и вправе лежать в будущем. Пока
//! меткой «ещё ни разу не писали» служил НОЛЬ, на такой реализации условие
//! «пора писать» (`now >= _nextAllowed`) было ложным для КАЖДОГО отрицательного
//! `now` — то есть первые отказы гасились бы вовсе, без единой строки и без
//! итогового счёта. Проверить это через `Allow()` невозможно: часы вернут то,
//! что вернут. Поэтому решение вынесено в `AllowAt(now, …)`, и тест подаёт ему
//! отрицательное время напрямую.
//!
//! ★ЭТА ПРОВЕРКА УМЕЕТ ПРОВАЛИТЬСЯ: со стартовым значением 0 первый же CHECK
//! ниже красный (прогон на заведомо сломанной копии — в отчёте раунда).
void TestNegativeEpochStillEmitsTheFirstLine()
{
  server::util::LogThrottle throttle(TestWindow);

  // Тики отрицательные и заведомо далеки от нуля.
  const auto farInThePast =
    -1'000'000 * static_cast<server::util::LogThrottle::Clock::rep>(1);

  uint64_t suppressed = 12345;
  uint64_t total = 12345;

  // Первая строка обязана выйти ДАЖЕ при отрицательной эпохе.
  CHECK(throttle.AllowAt(farInThePast, suppressed, total));
  CHECK(suppressed == 0);
  CHECK(total == 1);

  // Внутри окна — глотаем, но считаем.
  suppressed = 12345;
  total = 12345;
  CHECK(not throttle.AllowAt(farInThePast + 1, suppressed, total));
  CHECK(suppressed == 0);
  CHECK(total == 2);

  // Окно (1 с) прошло — снова можно, и проглоченное названо числом.
  const auto pastWindow = farInThePast
    + std::chrono::duration_cast<server::util::LogThrottle::Clock::duration>(
        TestWindow).count()
    + 1;

  suppressed = 12345;
  total = 12345;
  CHECK(throttle.AllowAt(pastWindow, suppressed, total));
  CHECK(suppressed == 1);
  CHECK(total == 3);
}

//! ★ПАРА ЧИСЕЛ В ВЫПУЩЕННОЙ СТРОКЕ СОГЛАСОВАНА ПОД НАГРУЗКОЙ
//! (R72-fix2-5, находка Codex 5).
//!
//! Проверяемое свойство одностороннее и потому не флаки: КАЖДАЯ выпущенная
//! строка обязана удовлетворять `total >= suppressed + 1` — проглоченных не
//! может быть больше, чем всего событий минус собственное событие
//! выпускающего, иначе строка утверждает, что проглоченного не было вовсе.
//!
//! ★ФОРМА ТЕСТА ВЫБРАНА ПО СЧЁТУ, А НЕ ПО ВИДУ. Простой «четыре потока молотят
//! один дроссель» НЕ ЛОВИТ дефект и был отброшен прогоном: чтобы победитель
//! соврал, число проглоченных, набежавших МЕЖДУ его собственным счётом и его
//! обменом, должно превысить число уже выпущенных строк — а оно растёт, и
//! окно закрывается почти сразу. Поэтому гонка ставится там, где выпущенных
//! строк ещё НОЛЬ: каждый круг — СВЕЖИЙ дроссель, и все потоки заходят в него
//! одновременно. На прежней реализации это даёт ровно ту строку, которую
//! назвало ревью: «проглочено 1, всего 1» (прогон на сломанной копии — в
//! отчёте раунда, 10 запусков из 10 красные).
void TestEmittedPairIsCoherentUnderContention()
{
  constexpr int ThreadCount = 4;
  constexpr int Rounds = 200000;

  // Окно заведомо больше всего прогона: в каждом круге выпускается ровно одна
  // строка, остальные заходы обязаны быть проглочены.
  constexpr auto HugeWindow = std::chrono::hours(1);

  std::atomic<int> generation{0};
  std::atomic<int> arrived{0};
  std::atomic<server::util::LogThrottle*> current{nullptr};
  std::atomic<bool> incoherent{false};
  std::atomic<uint64_t> worstSuppressed{0};
  std::atomic<uint64_t> worstTotal{0};

  std::vector<std::thread> threads;
  threads.reserve(ThreadCount);

  for (int threadIdx = 0; threadIdx < ThreadCount; ++threadIdx)
  {
    threads.emplace_back(
      [&, threadIdx]()
      {
        for (int round = 0; round < Rounds; ++round)
        {
          // Поток 0 — координатор круга: заводит свежий дроссель и открывает
          // круг. Остальные ждут открытия.
          if (threadIdx == 0)
          {
            current.store(
              new server::util::LogThrottle(HugeWindow),
              std::memory_order_release);
            generation.store(round + 1, std::memory_order_release);
          }
          else
          {
            while (generation.load(std::memory_order_acquire) != round + 1)
              std::this_thread::yield();
          }

          auto* const throttle = current.load(std::memory_order_acquire);

          uint64_t suppressed = 0;
          uint64_t total = 0;
          if (throttle->Allow(suppressed, total) && total < suppressed + 1)
          {
            worstSuppressed.store(suppressed, std::memory_order_relaxed);
            worstTotal.store(total, std::memory_order_relaxed);
            incoherent.store(true, std::memory_order_relaxed);
          }

          arrived.fetch_add(1, std::memory_order_acq_rel);

          // Координатор закрывает круг: дроссель уничтожается только после
          // того, как ВСЕ потоки в нём отметились.
          if (threadIdx == 0)
          {
            while (arrived.load(std::memory_order_acquire)
              < (round + 1) * ThreadCount)
            {
              std::this_thread::yield();
            }
            delete throttle;
          }
        }
      });
  }

  for (auto& thread : threads)
    thread.join();

  if (incoherent.load(std::memory_order_relaxed))
  {
    std::fprintf(
      stderr,
      "TestLogThrottle.cpp: выпущенная строка несогласована — "
      "проглочено %llu при полном счёте %llu\n",
      static_cast<unsigned long long>(worstSuppressed.load()),
      static_cast<unsigned long long>(worstTotal.load()));
  }

  CHECK(not incoherent.load(std::memory_order_relaxed));
}

} // namespace

int main()
{
  TestFirstLinePassesAndFloodIsSwallowed();
  TestNegativeEpochStillEmitsTheFirstLine();
  TestEmittedPairIsCoherentUnderContention();
  return 0;
}
