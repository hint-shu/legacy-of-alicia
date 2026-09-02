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
  //! ★С R71-14 такой кадр до классификатора уже не доходит — `HandleRelay`
  //! отбрасывает его в самом switch'е. Вид остаётся, потому что классификатор
  //! обязан быть ТОТАЛЬНОЙ функцией сам по себе, а не при условии вызывающего.
  Unparsed,
};

//! На какую СЕРВЕРНУЮ величину нагрузка ссылается вдобавок к действующему лицу
//! (LOA-fix, R71-15, находка ревью 2 #3).
//!
//! ★ЗАЧЕМ ОТДЕЛЬНОЕ ИЗМЕРЕНИЕ. Правило раунда состоит из двух половин: КТО действует
//! и С ЧЕМ он действует. Первая редакция классификатора отвечала только на первую, и
//! `SetTargetState` проезжал с `invokerRacerOid = свой` (законно) и произвольным
//! `magicEffectId` рядом — то есть гард обходился соседним полем ТОГО ЖЕ пакета, ровно
//! тем классом дефекта, который раунд объявил своей целью.
enum class RelayReferenceKind : uint8_t
{
  //! Нагрузка не называет ни одной серверной величины — сверять нечего.
  None,
  //! Нагрузка называет идентификатор ЭКЗЕМПЛЯРА эффекта. Их выдаёт сервер
  //! (`RaceTracker::GetNextEffectInstanceIdAndIncrementBy`), значит честный клиент
  //! может назвать только уже выданный.
  EffectInstanceId,
};

//! Что нагрузка утверждает о себе: кто действует и на какую серверную величину
//! ссылается.
struct RelayClaim
{
  RelayActorKind actorKind{RelayActorKind::Unparsed};
  //! Идентификатор действующего лица, который назвала САМА нагрузка.
  uint32_t actorId{0};
  RelayReferenceKind referenceKind{RelayReferenceKind::None};
  //! Названная серверная величина (смысл задаётся `referenceKind`).
  uint32_t referencedId{0};
};

//! Кто, по утверждению нагрузки, совершает действие и чем.
//!
//! ★SWITCH НАМЕРЕННО БЕЗ `default:`. Появится новый тип нагрузки — `-Wswitch`
//! (входит в `-Wall`, CMakeLists.txt:48-49) назовёт этот файл в сборке. С `default:`
//! новый тип тихо стал бы «неизвестным», то есть неавторизованным каналом. Одного
//! предупреждения мало — оно не останавливает сборку, — поэтому полноту дополнительно
//! стережёт `tools/check_relay_authz.sh`, и с R71-16 он вызывается ЦЕЛЬЮ СБОРКИ
//! (CMakeLists.txt), тестом CTest и раундовым `build_from_branch.sh`.
//! Значение ВНЕ перечисления (payloadType читается с провода как есть) падает на
//! `return` после switch — это законный путь, а не забытая ветка.
//!
//! ★`ResetPosOther` ГАРДИТСЯ, И ЭТО РЕШЕНИЕ, А НЕ ОЧЕВИДНОСТЬ. Имя допускает оба
//! чтения («я сбросился» / «сбросьте вон того»). Доводы за гард, в порядке силы:
//!  (1) ФОРМА НАГРУЗКИ. Кроме `affectedOid` в ней лежит ПОЛНЫЙ трансформ —
//!      right/up/forward/position (RelayMessageDefinitions.hpp:141-150). Позу, в
//!      которую надо поставить лошадь после сброса, вычисляет клиент ЕЁ владельца;
//!      посторонний клиент этих чисел не знает и осмысленно заполнить их не может.
//!      Значит отправитель называет здесь себя.
//!  (2) ЖИВОЙ ЗАХВАТ. В дампе настоящего заезда (28 000 датаграмм, 188 с) опкоды
//!      только 0x03, 0x07, 0x0d, 0x14, 0x16 — `ResetPosOther` (0x15) в обычном
//!      заезде не летит вовсе, то есть гард стоит не на горячем честном пути.
//!  (3) Остальные девять типов самоотчётные: архитектура relay такова, что клиент
//!      авторитетен только над своей лошадью.
//!  (4) Цена пропуска велика — это готовый телепорт чужой лошади.
//! ★Формулировка исправлена по ревью 2 (находка #8): прежний текст обещал
//! «самозалечивание за <=250 мс» — из снапшотов это НЕ следует, и такого обещания
//! здесь больше нет. Проверяемо на проде: жалоба печатает `payloadType`, и если в логе
//! пойдут `0x15` от РАЗНЫХ честных клиентов — откатить ровно этот `case` одним
//! коммитом.
[[nodiscard]] inline RelayClaim GetRelayClaim(
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
      // ★А рядом лежит `magicEffectId` — идентификатор экземпляра эффекта, который
      // выдал сервер. Его сверяет вызывающий по `referenceKind`.
      return {
        RelayActorKind::RacerOid,
        command.setTargetState.invokerRacerOid,
        RelayReferenceKind::EffectInstanceId,
        command.setTargetState.magicEffectId};
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
