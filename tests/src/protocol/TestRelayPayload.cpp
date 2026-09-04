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

//! LOA-fix (R71-19, находка ревью 2 #2): ИСЧЕРПАНИЕ НАГРУЗКИ РЕТРАНСЛЯЦИИ.
//!
//! Проверка обязана уметь ПРОВАЛИТЬСЯ, поэтому тест ходит в обе стороны: точный
//! размер обязан разобраться, лишний хвост и недобор обязаны бросить, а неизвестный
//! тип обязан остаться разрешённым для разбора (его отбрасывает хендлер, а не поток).

#include "libserver/network/command/proto/RaceMessageDefinitions.hpp"
#include "libserver/util/Stream.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{

using server::protocol::AcCmdCRRelay;
using server::protocol::relay::RelayCommandId;

int failures = 0;

void Check(const bool condition, const char* what)
{
  if (not condition)
  {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

//! Собирает тело команды `AcCmdCRRelay` из конверта и произвольной нагрузки.
std::vector<std::byte> MakeBody(
  const RelayCommandId payloadType,
  const std::vector<uint8_t>& payload)
{
  std::vector<std::byte> body;
  const auto push16 = [&body](const uint16_t value)
  {
    body.push_back(static_cast<std::byte>(value & 0xff));
    body.push_back(static_cast<std::byte>((value >> 8) & 0xff));
  };

  push16(7);                                          // fromOid
  push16(0);                                          // toOid
  push16(static_cast<uint16_t>(payloadType));         // payloadType
  push16(static_cast<uint16_t>(payload.size()));      // bufferSize
  for (const uint8_t datum : payload)
    body.push_back(static_cast<std::byte>(datum));

  return body;
}

//! @returns `true`, если разбор БРОСИЛ.
bool ReadThrows(
  const RelayCommandId payloadType,
  const std::vector<uint8_t>& payload,
  AcCmdCRRelay& command)
{
  const auto body = MakeBody(payloadType, payload);
  server::SourceStream stream{std::span<const std::byte>(body)};

  try
  {
    AcCmdCRRelay::Read(command, stream);
  }
  catch (const std::exception&)
  {
    return true;
  }

  return false;
}

} // namespace

int main()
{
  // 1. Точный размер (SpurLevel: oid 2 байта + счётчик 1 байт = 3) обязан разобраться.
  {
    AcCmdCRRelay command;
    Check(
      not ReadThrows(RelayCommandId::SpurLevel, {0x2a, 0x00, 0x03}, command),
      "нагрузка точного размера обязана разобраться");
    Check(command.spurLevel.racerOid == 0x2a, "разбор обязан взять названный oid");
    Check(command.spurLevel.successiveSpurCount == 3, "разбор обязан взять счётчик");
  }

  // 2. Лишний хвост обязан быть отвергнут — это и есть канал усиления.
  {
    AcCmdCRRelay command;
    Check(
      ReadThrows(RelayCommandId::SpurLevel, {0x2a, 0x00, 0x03, 0xff}, command),
      "нагрузка с лишним байтом обязана быть отвергнута");
  }

  // 3. Хвост в килобайт — тот самый случай из ревью.
  {
    AcCmdCRRelay command;
    std::vector<uint8_t> payload{0x2a, 0x00, 0x03};
    payload.resize(1024, 0xaa);
    Check(
      ReadThrows(RelayCommandId::SpurLevel, payload, command),
      "нагрузка с килобайтным хвостом обязана быть отвергнута");
  }

  // 4. Недобор обязан быть отвергнут (бросает сам поток).
  {
    AcCmdCRRelay command;
    Check(
      ReadThrows(RelayCommandId::SpurLevel, {0x2a, 0x00}, command),
      "укороченная нагрузка обязана быть отвергнута");
  }

  // 5. Снапшот ровно 56 байт (размер из живого захвата) обязан разобраться.
  {
    AcCmdCRRelay command;
    std::vector<uint8_t> payload(56, 0x00);
    payload[0] = 0x01; // racerOid
    Check(
      not ReadThrows(RelayCommandId::Snapshot, payload, command),
      "снапшот из 56 байт обязан разобраться");
    Check(command.snapshot.racerOid == 1, "снапшот обязан отдать oid гонщика");
  }

  // 6. Неизвестный тип разбором не занимается — и требовать с него исчерпания
  //    нельзя: правило применяется только там, где разбор был.
  {
    AcCmdCRRelay command;
    Check(
      not ReadThrows(static_cast<RelayCommandId>(0x7ffe), {1, 2, 3, 4, 5}, command),
      "неизвестный тип не обязан исчерпывать нагрузку");
  }

  if (failures)
  {
    std::printf("TestRelayPayload: %d нарушений\n", failures);
    return 1;
  }

  std::printf("TestRelayPayload: OK\n");
  return 0;
}
