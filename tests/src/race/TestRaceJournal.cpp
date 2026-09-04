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

//! LOA (R76, backlog #30 этап 1): ЖУРНАЛ ТРАССЫ — свойства, которые стенд НЕ
//! умеет проверить.
//!
//! ★ЧЕГО ЗДЕСЬ НАМЕРЕННО НЕТ. Само накопление журнала живёт ВНУТРИ
//! `RaceNetworkHandler::HandleRaceUserPos`, а печать — внутри
//! `RaceInstance::LogRaceAudit()`; обе — методы целей, которые в тестовый
//! бинарь не линкуются, и обе требуют живого заезда. Их доказывает матрица
//! стенда (6 арок x 6 улик) и пять негативных образов, а НЕ этот файл.
//! Переписать здесь цикл штамповки сплитов значило бы проверять копию кода
//! копией кода — «проверяющий, написанный по форме», который зелен ровно
//! тогда, когда обе копии ошибаются одинаково.
//!
//! ★ЧТО ЗДЕСЬ ЕСТЬ — четыре свойства, каждое из которых стенд пропустил бы:
//!  (1) поля журнала обязаны инициализироваться САМИ, без чужой помощи:
//!      снятый `{}` виден только на сыром, заведомо замусоренном хранилище;
//!  (2) «порог не взят» обязан быть НЕДОСТИЖИМ для честной отметки времени —
//!      иначе `InvalidSplitMs` однажды совпадёт с настоящим значением;
//!  (3) знаменатель проверки плотности обязан считаться в 64 битах — снятый
//!      каст молча превращает флуд пакетов в «жидкий заезд»;
//!  (4) пороги раунда 0 обязаны оставаться достижимыми: порог, который не
//!      может сработать (или срабатывает всегда), — это не порог.

#include "server/tracker/RaceTracker.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <tuple>
#include <type_traits>

namespace
{

//! ★НЕ `assert` (R76-fix-1, находка Codex 1 WARN-2). И образ, и юнит-прогон
//! раунда собираются `RelWithDebInfo`, а он несёт `-DNDEBUG` — то есть `assert`
//! выкидывается препроцессором и тест, написанный на нём, ПРОХОДИТ ВСЕГДА.
//! Первая редакция этого файла была написана именно так: три её рантайм-теста
//! не проверяли РОВНО НИЧЕГО, и ctest давал зелёный на пустом месте. Ровно та
//! же ловушка описана в шапке `tests/src/data/TestCourseRecords.cpp` (R75) —
//! я прошёл мимо предупреждения, оставленного соседним раундом.
//! Своя проверка живёт вне зависимости от NDEBUG и возвращает ненулевой код.
int g_failures = 0;

void Check(const bool condition, const char* const what, const int line)
{
  if (condition)
    return;
  std::fprintf(stderr, "TestRaceJournal.cpp:%d: ПРОВАЛ — %s\n", line, what);
  ++g_failures;
}

#define CHECK(cond, what) Check((cond), (what), __LINE__)

using Racer = server::tracker::RaceTracker::Racer;

//! (1) Поля журнала инициализируются САМИ.
//!
//! ★ЗАЧЕМ СЫРОЕ ХРАНИЛИЩЕ, А НЕ `Racer racer{};`. Значение-инициализация
//! обнулила бы поле и БЕЗ его собственного `{}` — то есть тест был бы зелен и
//! на коде, из которого инициализаторы вынули. Здесь память сперва заливается
//! мусором `0xA5`, и объект создаётся ПО УМОЛЧАНИЮ: всё, что не несёт своего
//! инициализатора, останется мусором и тест покраснеет. Это ровно тот дефект,
//! который на стенде невидим: `AddRacer` отдаёт `Racer` через `operator[]`
//! контейнера (value-init), и мусор проявился бы только на другом пути.
void TestJournalFieldsAreSelfInitialised()
{
  alignas(Racer) unsigned char storage[sizeof(Racer)];
  std::memset(storage, 0xA5, sizeof(storage));

  Racer* racer = new (storage) Racer;

  CHECK(racer->splitsReached == 0, "splitsReached инициализируется сам");
  CHECK(racer->posSampleCount == 0, "posSampleCount инициализируется сам");
  CHECK(racer->positionJumps == 0, "positionJumps инициализируется сам");
  CHECK(racer->discardedMetres == 0.0, "discardedMetres инициализируется сам");
  CHECK(racer->maxDiscardedStepMetres == 0.0f,
    "maxDiscardedStepMetres инициализируется сам");
  CHECK(racer->progressClipped == 0, "progressClipped инициализируется сам");
  CHECK(racer->maxDeclaredProgress == 0.0f,
    "maxDeclaredProgress инициализируется сам");
  for (const uint32_t split : racer->progressSplits)
    CHECK(split == 0, "элемент progressSplits инициализируется сам");

  racer->~Racer();
}

//! (2) «Порог не взят» недостижим для честной отметки времени.
//!
//! Цикл штамповки кламмпит отметку СТРОГО НИЖЕ `InvalidSplitMs`, и это имеет
//! смысл ровно потому, что сентинел стоит на самом верху диапазона: любое
//! реальное значение тогда отличимо от «не взят». Опустите сентинел куда-нибудь
//! в середину — и заезд длиной ровно в это число миллисекунд начнёт печататься
//! как незавершённый. Заодно проверяется, что размер массива и счётчик порогов
//! не разъехались: `splits 10/10` при массиве на 12 был бы враньём.
void TestInvalidSplitSentinelIsUnreachable()
{
  static_assert(Racer::InvalidSplitMs == std::numeric_limits<uint32_t>::max(),
    "сентинел «порог не взят» обязан стоять на верхней границе uint32");

  static_assert(
    std::tuple_size_v<std::remove_cvref_t<decltype(Racer{}.progressSplits)>>
      == Racer::ProgressSplitCount,
    "число порогов и длина массива отметок разъехались");

  static_assert(Racer::ProgressSplitCount == 10,
    "пороги трассы десятипроцентные — и печать, и WARN считают именно так");
}

//! (3) Плотность пакетов считается в 64 битах — проверяется ПРОДАКШН-КОД.
//!
//! ★ПЕРЕПИСАНО ПО НАХОДКЕ Codex 1 WARN-3. Первая редакция считала у себя
//! «широкое» и «узкое» выражения и сравнивала их между собой. Это была КОПИЯ
//! кода, проверяющая КОПИЮ кода: снятие `static_cast<uint64_t>` в
//! `RaceInstance.cpp` оставляло такой тест зелёным. Теперь условие живёт в
//! `Racer::HasPlausiblePacketDensity()` — той самой функции, которую зовёт WARN
//! «жидкий заезд», — и тест зовёт ЕЁ.
//!
//! Что доказывается: при `posSampleCount`, близком к пределу `uint32_t`,
//! произведение обязано перекрыть ЛЮБОЙ `courseTime`. В 32-битной арифметике
//! оно переполнилось бы и функция сказала бы «пакетов не хватило» — то есть
//! флуд читался бы как «никто не ехал».
void TestDensityIsComputedInSixtyFourBits()
{
  alignas(Racer) unsigned char storage[sizeof(Racer)];
  std::memset(storage, 0, sizeof(storage));
  Racer* racer = new (storage) Racer;

  // Честный заезд: 585 пакетов за 154 с — плотность достаточна.
  racer->posSampleCount = 585;
  CHECK(racer->HasPlausiblePacketDensity(154164),
    "585 пакетов за 154 с — плотность достаточна");

  // «Заезд в два пакета» за 42 с — плотности НЕТ (это и есть арка one-packet).
  racer->posSampleCount = 2;
  CHECK(not racer->HasPlausiblePacketDensity(42002),
    "2 пакета за 42 с — плотности нет");

  // ★ЯДРО ПРОВЕРКИ. При максимуме счётчика произведение обязано перекрыть
  // максимальный возможный courseTime. Со снятым 64-битным кастом произведение
  // переполнится, и эта строка покраснеет.
  racer->posSampleCount = std::numeric_limits<uint32_t>::max();
  CHECK(racer->HasPlausiblePacketDensity(std::numeric_limits<uint32_t>::max()),
    "при пределе счётчика пакетов плотность обязана быть достаточной "
    "(иначе знаменатель переполнился в 32 битах)");

  racer->~Racer();
}

//! (4) Пороги раунда 0 остаются достижимыми.
//!
//! Порог, который не может сработать, и порог, который срабатывает всегда, —
//! одинаково бесполезны, и оба выглядят как «настроили». Числа справа — это
//! ИЗМЕРЕНИЯ раунда 0 по 86 честным заездам живой игры, а не вкус: честный
//! минимум сплитов 8/10, честный максимум одного отброшенного шага 63.25 м,
//! честный финишный «часовой» карты до 2.0.
void TestRoundZeroThresholdsStayReachable()
{
  // ★ПОРОГ ОБЯЗАН УМЕТЬ СРАБОТАТЬ (R76-fix-1, находка Codex 1 WARN-4).
  // Первая редакция проверяла только ВЕРХНИЕ границы, и `MinPlausibleSplits = 0`
  // проходил их обе — то есть порог, который не срабатывает НИКОГДА, считался
  // настроенным. Нижняя граница столь же обязательна, сколь верхняя.
  static_assert(server::tracker::MinPlausibleSplits >= 1,
    "порог 0 не срабатывает никогда — это не порог");
  // Порог «жидкого заезда» обязан лежать НИЖЕ полного числа порогов, иначе
  // WARN получил бы даже тот, кто взял все десять.
  static_assert(server::tracker::MinPlausibleSplits < Racer::ProgressSplitCount,
    "порог «жидкого заезда» не ниже полного числа сплитов — WARN у всех");
  // И НИЖЕ честного минимума 8/10, иначе WARN пойдёт у честных финишёров
  // на карте 7 (ri_fore01 финиширует при progress 0.811).
  static_assert(server::tracker::MinPlausibleSplits <= 8,
    "порог выше честного минимума 8/10 — ложная тревога на карте 7");
  // ★И ЗАКРЕПЛЁН ПОИМЁННО. Ни одна арка стенда не даёт финиша с 4-5 сплитами
  // (честный доходит до 10, читерский — до 1), поэтому ИМЕННО ЭТО ЧИСЛО не
  // проверяется поведением нигде. Пусть его меняют осознанно: снижать по
  // живому логу МОЖНО, повышать — только с новым замером (раунд 0 дал
  // честный минимум 8/10 по 86 заездам, 6 оставляет два сплита запаса).
  static_assert(server::tracker::MinPlausibleSplits == 6,
    "измеренное значение порога изменено — нужен новый замер, а не правка теста");

  // Телепорт меряется ВЕЛИЧИНОЙ шага. Порог обязан стоять выше честного
  // максимума 63.25 м, иначе склейка пакетов честного игрока читается как
  // телепорт; и это ЕДИНСТВЕННЫЙ порог раунда, который вообще может сработать
  // на шаге, отброшенном бюджетом.
  static_assert(server::tracker::TeleportStepMetres > 63.25f,
    "порог телепорта ниже честного максимума одного отброшенного шага");

  // Потолок объявленного прогресса обязан стоять выше честного финишного
  // «часового» (до 2.0 на измеренных картах), иначе WARN получит каждый
  // честный финиш.
  static_assert(server::tracker::MaxDeclaredProgressCeiling > 2.0f,
    "потолок объявленного прогресса ниже честного финишного часового");

  // Средний интервал: честный каденс 3.77-4.00 Гц, то есть 250-265 мс.
  // Порог обязан стоять ВЫШЕ, иначе честный заезд объявляется «жидким».
  static_assert(server::tracker::MaxPlausibleMeanPosIntervalMs > 265,
    "верхняя граница среднего интервала ниже честного каденса");
}

} // namespace

int main()
{
  TestJournalFieldsAreSelfInitialised();
  TestInvalidSplitSentinelIsUnreachable();
  TestDensityIsComputedInSixtyFourBits();
  TestRoundZeroThresholdsStayReachable();
  // ★НЕНУЛЕВОЙ КОД ВОЗВРАТА — единственное, что видит ctest. Тест, который
  // печатает провал и выходит нулём, зелен для всех, кроме читателя лога.
  return g_failures == 0 ? 0 : 1;
}
