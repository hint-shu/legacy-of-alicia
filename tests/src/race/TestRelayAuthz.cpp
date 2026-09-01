//! Юнит-тест классификатора действующего лица relay-нагрузки (LOA-fix, R71-6b, #31).
//!
//! ★ЗАЧЕМ. `GetRelayActor` — единственное место, где решается «кто действует» внутри
//! ретранслируемой нагрузки. Гейт `tools/check_relay_authz.sh` доказывает, что назван
//! КАЖДЫЙ тип; этот тест доказывает, что у каждого типа взято ТО поле — форма и
//! полнота ловятся разными проверками намеренно.
//!
//! ★`assert` НЕ ИСПОЛЬЗУЕТСЯ: боевой образ собирается с -DNDEBUG.
#include "server/race/RelayAuthz.hpp"

#include <cstdint>
#include <cstdio>

namespace
{

using server::protocol::relay::RelayCommandId;
using server::race::RelayActorKind;

int failures = 0;

void Check(const bool condition, const char* what)
{
  if (!condition)
  {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

server::protocol::AcCmdCRRelay MakeRelay(const RelayCommandId type)
{
  server::protocol::AcCmdCRRelay c{};
  c.fromOid = 1;
  c.toOid = 0;
  c.payloadType = type;
  // Каждое поле «действующего лица» получает СВОЁ значение, чтобы тест ловил не
  // только «вид верен», но и «взято ТО поле».
  c.snapshot.racerOid = 11;
  c.syncProgress.racerOid = 12;
  c.setTargetState.invokerRacerOid = 13;
  c.setTargetState.targetRacerOid = 99;
  c.netSetState.racerOid = 14;
  c.netSetLayerAnimation.racerOid = 15;
  c.syncGoalIn.racerOid = 16;
  c.spurLevel.racerOid = 17;
  c.slidingMotion.racerOid = 18;
  c.resetPosOther.affectedOid = 19;
  c.broadcastCharacterUid.selfCharacterUid = 4242;
  return c;
}

void Expect(
  const RelayCommandId type,
  const RelayActorKind kind,
  const uint32_t id,
  const char* what)
{
  const auto actor = server::race::GetRelayActor(MakeRelay(type));
  Check(actor.kind == kind && actor.claimedId == id, what);
}

} // namespace

int main()
{
  Expect(RelayCommandId::Snapshot,               RelayActorKind::RacerOid,       11, "Snapshot");
  Expect(RelayCommandId::SyncProgress,           RelayActorKind::RacerOid,       12, "SyncProgress");
  Expect(RelayCommandId::SetTargetStateEnabled,  RelayActorKind::RacerOid,       13, "SetTargetStateEnabled");
  Expect(RelayCommandId::SetTargetStateDisabled, RelayActorKind::RacerOid,       13, "SetTargetStateDisabled");
  Expect(RelayCommandId::NetSetState,            RelayActorKind::RacerOid,       14, "NetSetState");
  Expect(RelayCommandId::NetSetLayerAnimation,   RelayActorKind::RacerOid,       15, "NetSetLayerAnimation");
  Expect(RelayCommandId::SyncGoalIn,             RelayActorKind::RacerOid,       16, "SyncGoalIn");
  Expect(RelayCommandId::SpurLevel,              RelayActorKind::RacerOid,       17, "SpurLevel");
  Expect(RelayCommandId::SlidingMotion,          RelayActorKind::RacerOid,       18, "SlidingMotion");
  Expect(RelayCommandId::ResetPosOther,          RelayActorKind::RacerOid,       19, "ResetPosOther");
  Expect(RelayCommandId::BroadcastCharacterUid,  RelayActorKind::CharacterUid, 4242, "BroadcastCharacterUid");

  // Значение вне перечисления приходит с провода как есть — законный путь.
  Expect(static_cast<RelayCommandId>(0x7fff), RelayActorKind::Unparsed, 0, "неизвестный тип");

  if (failures)
  {
    std::printf("TestRelayAuthz: %d нарушений\n", failures);
    return 1;
  }
  std::printf("TestRelayAuthz: OK\n");
  return 0;
}
