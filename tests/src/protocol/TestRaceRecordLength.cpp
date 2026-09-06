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

//! LOA-fix (R81, backlog #274): АРИФМЕТИКА ДЛИНЫ `RaceRecord`.
//!
//! ★ЗАЧЕМ. Раунд заполняет ТОЛЬКО `finalRecordMs` и НЕ трогает `teamMode` —
//! именно потому, что `RaceRecord::Write` дописывает шесть полей
//! `trainingRecord` (5×`uint16_t` + `uint8_t` = 11 БАЙТ) при
//! `teamMode == TeamMode::Single`. Проставить `teamMode` «для полноты» значит
//! удлинить `AcCmdRCRaceResultNotify` на 11 байт на КАЖДОГО гонщика и сдвинуть
//! разбор `ScoreInfo` у каждого клиента комнаты.
//!
//! ★ЭТОТ ТЕСТ НЕ ЗАМЕНЯЕТ ОБРАЗ `neg-k`. Образ доказывает ЖИВОЙ кадр в
//! комнате; тест пинует САМО ЧИСЛО 11, чтобы будущая правка сериализатора не
//! сделала обоснование раунда молча неверным.

#include "libserver/network/command/proto/RaceMessageDefinitions.hpp"
#include "libserver/util/Stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

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

using server::SinkStream;
using RaceRecord = server::protocol::AcCmdCRStartRaceNotify::RaceRecord;

//! Сколько байт занимает запись при данном режиме команд.
std::size_t WrittenSize(const RaceRecord& record)
{
  std::array<std::byte, 4096> storage{};
  SinkStream sink{std::span{storage}};
  RaceRecord::Write(record, sink);
  return sink.GetCursor();
}

//! ★ГЕЙТ ОБЯЗАН СПЕРВА ДОКАЗАТЬ СЕБЯ.
void TestCheckerCanFail()
{
  Check(false, "(ожидаемо) самопроверка счётчика провалов");
  if (g_failures == 1)
  {
    g_failures = 0;
    return;
  }
  std::fprintf(stderr, "FAIL: Check не считает провалы — тесту верить нельзя\n");
  g_failures = 1000;
}

//! ★ГЛАВНОЕ ЧИСЛО РАУНДА: разница ровно 11 байт.
void TestSingleTeamModeAddsElevenBytes()
{
  RaceRecord defaulted{};
  const auto baseline = WrittenSize(defaulted);

  RaceRecord single{};
  single.teamMode = server::protocol::TeamMode::Single;
  const auto withTraining = WrittenSize(single);

  Check(withTraining == baseline + 11,
    "★teamMode == Single обязан удлинять RaceRecord РОВНО на 11 байт "
    "(5 x uint16 + uint8)");
  std::printf("RaceRecord: default %zu bytes, Single %zu bytes (delta %zu)\n",
    baseline, withTraining, withTraining - baseline);
}

//! ★ДЕФОЛТНЫЙ `teamMode` — НИ ОДИН ИЗ ТРЁХ РЕЖИМОВ, поэтому ветка
//! `trainingRecord` не пишется никогда. Именно на этом стоит утверждение
//! раунда «длина пакета не меняется ни на байт».
void TestDefaultTeamModeIsNotSingle()
{
  RaceRecord defaulted{};
  Check(static_cast<uint8_t>(defaulted.teamMode) == 0,
    "дефолт члена teamMode обязан быть 0, то есть ни FFA, ни Team, ни Single");
  Check(static_cast<uint8_t>(server::protocol::TeamMode::Single) != 0,
    "Single обязан быть отличен от дефолта — иначе ветка писалась бы всегда");
  Check(WrittenSize(defaulted) == WrittenSize(RaceRecord{}),
    "две дефолтные записи обязаны занимать одинаково");
}

//! ★ЗАПОЛНЕНИЕ `finalRecordMs` НЕ МЕНЯЕТ ДЛИНУ — это и есть то, что делает
//! раунд. Ноль и большое число обязаны занимать одинаково.
void TestFinalRecordDoesNotChangeLength()
{
  RaceRecord zero{};
  RaceRecord filled{};
  filled.finalRecordMs = 149030;
  Check(WrittenSize(zero) == WrittenSize(filled),
    "★заполнение finalRecordMs обязано быть длинно-нейтральным");
}

//! ★СЧЁТЧИК СЕКТОРОВ СЧИТАЕТ СЕКТОРА, А НЕ КРУГИ (гоча R74): один круг даёт
//! три записи по 12 байт. Здесь это проверяется числом, потому что разбор
//! стенда ключится ровно на нём.
void TestLapRecordsAreCountedInSectors()
{
  RaceRecord none{};
  RaceRecord oneLap{};
  oneLap.lapRecords.push_back(RaceRecord::LapRecord{});
  Check(WrittenSize(oneLap) == WrittenSize(none) + 12,
    "один круг обязан добавлять 3 x uint32 = 12 байт");
}

} // namespace

int main()
{
  TestCheckerCanFail();
  TestSingleTeamModeAddsElevenBytes();
  TestDefaultTeamModeIsNotSingle();
  TestFinalRecordDoesNotChangeLength();
  TestLapRecordsAreCountedInSectors();

  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all race record length checks passed\n");
  return 0;
}
