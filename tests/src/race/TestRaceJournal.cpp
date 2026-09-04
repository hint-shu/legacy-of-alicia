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

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <tuple>
#include <type_traits>

namespace
{

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

  assert(racer->splitsReached == 0);
  assert(racer->posSampleCount == 0);
  assert(racer->positionJumps == 0);
  assert(racer->discardedMetres == 0.0);
  assert(racer->maxDiscardedStepMetres == 0.0f);
  assert(racer->progressClipped == 0);
  assert(racer->maxDeclaredProgress == 0.0f);
  for (const uint32_t split : racer->progressSplits)
    assert(split == 0);

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

//! (3) Знаменатель плотности считается в 64 битах.
//!
//! Условие WARN «жидкий заезд» умножает число принятых пакетов на верхнюю
//! границу среднего интервала. В 32 битах это произведение переполняется
//! задолго до предела счётчика, и заезд, в котором пакетов пришло СЛИШКОМ
//! МНОГО, получил бы маленькое произведение — то есть флуд читался бы как
//! «никто не ехал». Тест не повторяет условие, он показывает, что две
//! арифметики РАСХОДЯТСЯ и почему выбрана 64-битная.
void TestDensityArithmeticDoesNotOverflow()
{
  constexpr uint32_t manySamples = std::numeric_limits<uint32_t>::max();

  const uint64_t wide = static_cast<uint64_t>(manySamples)
    * server::tracker::MaxPlausibleMeanPosIntervalMs;
  const uint32_t narrow = manySamples
    * server::tracker::MaxPlausibleMeanPosIntervalMs;

  // Две арифметики обязаны РАЗОЙТИСЬ — иначе тест ничего не стерёг бы.
  assert(wide != static_cast<uint64_t>(narrow));

  // И только широкая честно говорит «пакетов хватило на любой заезд»:
  // courseTime — uint32, значит его максимум заведомо меньше произведения.
  assert(wide > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
  assert(static_cast<uint64_t>(narrow)
    < static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
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
  // Порог «жидкого заезда» обязан лежать НИЖЕ полного числа порогов, иначе
  // WARN получил бы даже тот, кто взял все десять.
  static_assert(server::tracker::MinPlausibleSplits < Racer::ProgressSplitCount,
    "порог «жидкого заезда» не ниже полного числа сплитов — WARN у всех");
  // И НИЖЕ честного минимума 8/10, иначе WARN пойдёт у честных финишёров
  // на карте 7 (ri_fore01 финиширует при progress 0.811).
  static_assert(server::tracker::MinPlausibleSplits <= 8,
    "порог выше честного минимума 8/10 — ложная тревога на карте 7");

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
  TestDensityArithmeticDoesNotOverflow();
  TestRoundZeroThresholdsStayReachable();
}
