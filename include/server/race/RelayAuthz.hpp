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

#ifndef SERVER_RACE_RELAY_AUTHZ_HPP
#define SERVER_RACE_RELAY_AUTHZ_HPP

#include "libserver/network/command/proto/RaceMessageDefinitions.hpp"
#include "libserver/network/command/proto/relay/RelayMessageDefinitions.hpp"

#include <cstdint>

namespace server::race
{

//! Чем нагрузка ретрансляции НАЗЫВАЕТ действующее лицо (LOA-fix, R71-6b, backlog #31).
enum class RelayActorKind : uint8_t
{
  //! Нагрузка называет oid гонщика — сверяется с oid отправителя.
  RacerOid,
  //! Нагрузка называет uid ПЕРСОНАЖА (не oid!) — сверяется с uid отправителя.
  CharacterUid,
  //! Тип нагрузки серверу неизвестен: `AcCmdCRRelay::Read` его не разбирал
  //! (`default:` на :1316-1320), поэтому называть нечего и сверять нечего.
  Unparsed,
};

struct RelayActor
{
  RelayActorKind kind{RelayActorKind::Unparsed};
  //! Идентификатор, который назвала САМА нагрузка.
  uint32_t claimedId{0};
};

//! Кто, по утверждению нагрузки, совершает действие.
//!
//! ★SWITCH НАМЕРЕННО БЕЗ `default:`. Появится новый тип нагрузки — `-Wswitch`
//! (входит в `-Wall`, CMakeLists.txt:48-49) назовёт этот файл в сборке. С `default:`
//! новый тип тихо стал бы «неизвестным», то есть неавторизованным каналом.
//! Значение ВНЕ перечисления (payloadType читается с провода как есть) падает на
//! `return` после switch — это законный путь, а не забытая ветка.
//!
//! ★`ResetPosOther` ГАРДИТСЯ, И ЭТО РЕШЕНИЕ, А НЕ ОЧЕВИДНОСТЬ. Имя допускает оба
//! чтения («я сбросился» / «сбросьте вон того»). Три довода за гард: (1) все
//! остальные девять типов — самоотчётные, архитектура relay такова, что клиент
//! авторитетен только над своей лошадью; (2) цена ложного срабатывания мала и
//! самозалечивается — позиция приезжает Snapshot'ами 4 раза в секунду, поэтому
//! ошибочно выброшенный ResetPosOther даёт рассинхрон на <=250 мс, а не поломку;
//! (3) цена пропуска велика — это готовый телепорт чужой лошади. Проверяемо на
//! проде: жалоба печатает `payloadType`, и если в логе пойдут `0x15` от РАЗНЫХ
//! честных клиентов — откатить ровно этот `case` одним коммитом.
[[nodiscard]] inline RelayActor GetRelayActor(
  const protocol::AcCmdCRRelay& command) noexcept
{
  using protocol::relay::RelayCommandId;

  switch (command.payloadType)
  {
    case RelayCommandId::Snapshot:
      return {RelayActorKind::RacerOid, command.snapshot.racerOid};
    case RelayCommandId::SyncProgress:
      return {RelayActorKind::RacerOid, command.syncProgress.racerOid};
    case RelayCommandId::SetTargetStateEnabled:
    case RelayCommandId::SetTargetStateDisabled:
      // ★Сверяется ИНВОКЕР, а не цель: навести магию на другого — законно.
      return {RelayActorKind::RacerOid, command.setTargetState.invokerRacerOid};
    case RelayCommandId::NetSetState:
      return {RelayActorKind::RacerOid, command.netSetState.racerOid};
    case RelayCommandId::NetSetLayerAnimation:
      return {RelayActorKind::RacerOid, command.netSetLayerAnimation.racerOid};
    case RelayCommandId::SyncGoalIn:
      return {RelayActorKind::RacerOid, command.syncGoalIn.racerOid};
    case RelayCommandId::BroadcastCharacterUid:
      return {RelayActorKind::CharacterUid, command.broadcastCharacterUid.selfCharacterUid};
    case RelayCommandId::SpurLevel:
      return {RelayActorKind::RacerOid, command.spurLevel.racerOid};
    case RelayCommandId::ResetPosOther:
      return {RelayActorKind::RacerOid, command.resetPosOther.affectedOid};
    case RelayCommandId::SlidingMotion:
      return {RelayActorKind::RacerOid, command.slidingMotion.racerOid};
  }

  return {RelayActorKind::Unparsed, 0};
}

} // namespace server::race

#endif // SERVER_RACE_RELAY_AUTHZ_HPP
