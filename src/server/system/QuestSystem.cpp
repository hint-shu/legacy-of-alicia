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

#include "server/system/QuestSystem.hpp"
#include "libserver/util/QuietLog.hpp"

#include "server/ServerInstance.hpp"

#include <libserver/data/DataDirector.hpp>
#include <libserver/registry/QuestRegistry.hpp>
#include <libserver/util/Util.hpp>

#include <spdlog/spdlog.h>

namespace server
{

// LOA-fix (R42, #8 F2): день-граница self-heal СЧЁТЧИКА суточного класс-опыта.
// Счётчик dailyClassExpGranted читается ТОЛЬКО в двух spend-путях (этот файл:
// дейлик-спендер + ClaimDailyClassExp). Sync зовётся в НАЧАЛЕ каждого spend'а под
// груп-локом и обнуляет счётчик, если его собственная дата протухла (новый игровой
// день). ★НЕ трогает lastResetDate → квест-сброс не подавляется. Счётчик теперь
// сбрасывается ЭКСКЛЮЗИВНО здесь (RanchDirector-строки убраны R42-7/R42-8): те
// сбрасывали по lastResetDate и могли стереть spend, случившийся между границей дня
// и ранч-входом (это и был F2-баг: второй полный 6650 в те же сутки).
static void SyncDailyClassExpBudget(data::DailyQuestGroup& group)
{
  // Простой rollover-чек: миграция legacy-записей (сид resetDate из lastResetDate)
  // сделана в FileDataSource при десериализации (R42-3), ДО любого runtime-reset —
  // поэтому здесь lastResetDate НЕ читается вообще (decoupling от квест-сброса полный,
  // quest-reset-safe, ordering-инвариантно). Счётчик обнуляется ровно когда ЕГО
  // собственная дата протухла (новый игровой день).
  const uint32_t today = util::CurrentGameDayIndex();
  if (group.dailyClassExpResetDate() < today)
  {
    group.dailyClassExpGranted = 0;
    group.dailyClassExpResetDate = today;
  }
}

QuestSystem::QuestSystem(ServerInstance& serverInstance)
  : _serverInstance(serverInstance)
{
}

bool QuestSystem::IsModeMatch(
  const registry::Quest::GameModeFlag questFlag,
  const registry::Quest::GameModeFlag eventMode)
{
  // Flag None (0): no mode restriction
  if (questFlag == registry::Quest::GameModeFlag::None)
    return true;
  // Flag Any (111): explicitly matches all race modes
  if (questFlag == registry::Quest::GameModeFlag::Any)
    return true;
  // LOA-fix (R26, #10): gameModeFlag — БИТОВАЯ маска (spd-solo=1/team=2, mag-solo=4/
  // team=8, +категорийные 32/64). Прежнее СРАВНЕНИЕ РАВЕНСТВОМ теряло прогресс в
  // командных заездах у квестов с флагом solo+team (35/76): eventMode давал чистый
  // team-бит (2/8), а 35 != 2. Теперь ПЕРЕСЕЧЕНИЕ масок. eventMode (из ToGameModeFlag/
  // ToPrizeGameModeFlag) — ТОЛЬКО чистый бит 1/2/4/8 (без категорийных) → личный-приз
  // 33(=32|1) в команде: 33 & 2 = 0, не засчитывается (overpay-гард держится битово;
  // negative-test это арбитрит).
  return (static_cast<uint32_t>(questFlag) & static_cast<uint32_t>(eventMode)) != 0;
}

registry::Quest::GameModeFlag QuestSystem::ToGameModeFlag(
  const protocol::GameMode gameMode,
  const protocol::TeamMode teamMode)
{
  using GameModeFlag = registry::Quest::GameModeFlag;
  const bool isTeam = teamMode == protocol::TeamMode::Team;

  switch (gameMode)
  {
    case protocol::GameMode::Speed:
      // LOA-fix (R26, #10): SOLO -> ЧИСТЫЙ event-бит (speed-solo=1, magic-solo=4),
      // НЕ композитные SpeedSoloAction(35)/MagicSoloAction(76): под mask-intersection
      // их категорийный бит 32/64 дал бы латентный false-match. Team уже чистый (2/8).
      return isTeam ? GameModeFlag::SpeedTeam : static_cast<GameModeFlag>(1);
    case protocol::GameMode::Magic:
      return isTeam ? GameModeFlag::MagicTeam : static_cast<GameModeFlag>(4);
    default:
      return GameModeFlag::None;
  }
}

registry::Quest::GameModeFlag QuestSystem::ToPrizeGameModeFlag(
  const protocol::GameMode gameMode,
  const protocol::TeamMode teamMode)
{
  // LOA-fix (R5, round2): см. заголовок. Соло-заезд для события «призовое место»
  // отображается в WinSpeedSolo(33)/WinMagicSolo(68) — именно эти значения стоят
  // у дейликов 1000/1011 и 1001/1012. Командный заезд оставляем как есть
  // (SpeedTeam/MagicTeam): личное призовое место в команде не засчитывается.
  using GameModeFlag = registry::Quest::GameModeFlag;
  const bool isTeam = teamMode == protocol::TeamMode::Team;

  switch (gameMode)
  {
    case protocol::GameMode::Speed:
      // LOA-fix (R26, #10): SOLO -> ЧИСТЫЙ event-бит (1/4), НЕ WinSpeedSolo(33)/
      // WinMagicSolo(68): их категорийный бит под mask-intersection дал бы латентный
      // false-match личный-приз->команда. Team чистый (2/8): личный приз в команде =
      // 33 & 2 = 0, не засчитывается (это и гейтит negative-test).
      return isTeam ? GameModeFlag::SpeedTeam : static_cast<GameModeFlag>(1);
    case protocol::GameMode::Magic:
      return isTeam ? GameModeFlag::MagicTeam : static_cast<GameModeFlag>(4);
    default:
      return GameModeFlag::None;
  }
}

bool QuestSystem::IsServerTrackedFunction(const registry::Quest::Function function)
{
  // LOA-fix (NEW-1, round3): список ровно тех классов, которые двигает САМ
  // сервер своими хуками:
  //   True                        — заезды/кормление/мытьё/рывок (RaceInstance,
  //                                 RanchDirector, RaceNetworkHandler);
  //   TeamWin, PrizeWinner*       — RaceInstance::Stop;
  //   RunMap                      — RaceInstance::Stop (по фактической карте);
  //   PerfectJump                 — RaceNetworkHandler::HandleHurdleClearResult;
  //   FireballAttack              — RaceNetworkHandler::HandleUseMagicItem.
  // Всё остальное (GlidingDistanceValue, CollectDropItem, ClearMission) сервер
  // не наблюдает НИКАК: события нет ни в одном пакете, который до него доходит.
  switch (function)
  {
    case registry::Quest::Function::True:
    case registry::Quest::Function::TeamWin:
    case registry::Quest::Function::RunMap:
    case registry::Quest::Function::PrizeWinnerForLowLevel:
    case registry::Quest::Function::PrizeWinnerInMapForLowLevel:
    case registry::Quest::Function::PerfectJump:
    case registry::Quest::Function::FireballAttack:
      return true;
    default:
      return false;
  }
}

bool QuestSystem::IsEventMatch(
  const registry::Quest::Function function,
  const QuestEvent event,
  const uint32_t questFunctionValue,
  const uint32_t eventValue)
{
  switch (event)
  {
    case QuestEvent::Any:
      return function == registry::Quest::Function::True;
    case QuestEvent::PrizeWinner:
      return function == registry::Quest::Function::PrizeWinnerForLowLevel ||
             function == registry::Quest::Function::PrizeWinnerInMapForLowLevel;
    case QuestEvent::PerfectJump:
      return function == registry::Quest::Function::PerfectJump;
    case QuestEvent::FireballAttack:
      return function == registry::Quest::Function::FireballAttack;
    case QuestEvent::RunMap:
      return function == registry::Quest::Function::RunMap && questFunctionValue == eventValue;
    case QuestEvent::TeamWin:
      return function == registry::Quest::Function::TeamWin;
    case QuestEvent::GlidingDistance:
      return function == registry::Quest::Function::GlidingDistanceValue;
    case QuestEvent::CollectDropItem:
      // LOA-fix (R68, backlog #5/#99): СВЕРКА ЗНАЧЕНИЯ, как у RunMap строкой
      // выше. У квестов этого класса `functionValue` — это `QTemID` предмета
      // (сверено по всем 17 записям quests.yaml), а событие несёт QTemID
      // ПОДОБРАННОГО предмета. Без сравнения один подобранный клок шерсти
      // двигал бы и «собери 20 клоков», и «собери 20 листовок».
      // ★ЧЕСТНО: эта ветка сегодня НЕДОСТИЖИМА. `OnQuestEvent` работает по
      // трём слотам ДНЕВНОЙ группы, а живой канал класса — сюжетные квесты
      // через `AdvanceMainQuests`, где сверку значения делает параметр
      // `matchFunctionValue`. Чиним всё равно: матчер, который лжёт про свой
      // класс, — заряженное ружьё для первого, кто сюда что-нибудь
      // задиспатчит.
      return function == registry::Quest::Function::CollectDropItem
        && questFunctionValue == eventValue;
    default:
      return false;
  }
}

std::vector<protocol::AcCmdRCUpdateDailyQuestNotify> QuestSystem::OnQuestEvent(
  const data::Uid characterUid,
  const QuestEvent event,
  const registry::Quest::GameModeFlag gameMode,
  const uint32_t value,
  const std::vector<uint32_t>& tidFilter)
{
  auto& dataDirector = _serverInstance.GetDataDirector();
  const auto& questRegistry = _serverInstance.GetQuestRegistry();

  const auto characterRecord = dataDirector.GetCharacter(characterUid);
  if (!characterRecord)
    return {};

  // Read the character's daily quest group UID
  data::Uid dailyQuestGroupUid = data::InvalidUid;
  characterRecord.Immutable([&dailyQuestGroupUid](const data::Character& character)
  {
    dailyQuestGroupUid = character.dailyQuestGroupUid();
  });

  if (dailyQuestGroupUid == data::InvalidUid)
    return {};

  auto questGroupRecord = dataDirector.GetDailyQuestGroup(dailyQuestGroupUid);
  if (!questGroupRecord)
    return {};

  std::vector<protocol::AcCmdRCUpdateDailyQuestNotify> notifies;

  // LOA-fix (R17-1, quest-batch-2, #8): НАКОПИТЕЛИ НАГРАДЫ ЗА ДЕЙЛИКИ. Награда
  // (rewardGameMoney / rewardExp из quests.yaml) ниже вычисляется в
  // carrotsReward/mountExp, кладётся в notify — и на этом всё: игроку она НЕ
  // НАЧИСЛЯЛАСЬ. Клиент рисовал «+N морковок», баланс не менялся. Копим суммы
  // здесь, а начисляем ПОСЛЕ выхода из questGroupRecord.Mutable (см. R17-2):
  // локи записей не рекурсивны, брать Character изнутри лока группы нельзя.
  uint32_t pendingCarrots = 0;
  uint32_t pendingMountExp = 0;
  // Суточный кап класс-опыта лошади: ровно один класс/день (класс 1->2 = 6650).
  // Без капа дейлики дают ~4.8 класса/день (RMT-time-кран). Опыт с заездов
  // отдельный (R20). Морковки НЕ капим.
  // LOA-fix (R-revenge, #13): константа ПОДНЯТА в класс
  // (QuestSystem::DailyClassExpCap, объявлена в QuestSystem.hpp) — тем же
  // бюджетом пользуется бонус мести через ClaimDailyClassExp. Кап теперь
  // общий для всех «мягких» каналов, источник правды один.

  questGroupRecord.Mutable([&](data::DailyQuestGroup& group)
  {
    SyncDailyClassExpBudget(group);   // R42 #8 F2: self-heal cap on day boundary before reading it
    auto questSlots = group.quests();
    // LOA-fix (R17-cap, quest-batch-2, #8): seed THIS payout's remaining daily class-exp
    // budget from the persisted counter, INSIDE the group lock (the counter is a field of
    // this record). Each completing quest's exp is clamped against it in the loop below,
    // so both the client notify and the accrued total stay within DailyClassExpCap.
    uint32_t remainingClassExpCap =
      group.dailyClassExpGranted() >= DailyClassExpCap
        ? 0u
        : (DailyClassExpCap - group.dailyClassExpGranted());
    for (auto& entry : questSlots)
    {
      if (entry.questId == 0)
        continue;

      // LOA-fix (F8, quest-batch-1): необязательный белый список tid'ов.
      // Причина: function TRUE матчится с QuestEvent::Any, а gameModeFlag 0
      // трактуется IsModeMatch как «без ограничения», поэтому событие «доехал
      // заезд» двигало ещё и «покорми лошадь» (1003/1014/1022) и «помой лошадь»
      // (1004/1015/1023). Различить их по данным невозможно — только явным
      // списком у места диспатча. Пустой фильтр = прежнее поведение.
      if (not tidFilter.empty())
      {
        bool allowed = false;
        for (const uint32_t allowedTid : tidFilter)
          if (allowedTid == static_cast<uint32_t>(entry.questId)) { allowed = true; break; }
        if (not allowed)
          continue;
      }

      const auto questDef = questRegistry.GetQuest(entry.questId);
      if (!questDef)
        continue;

      // Already satisfied
      if (entry.progress >= questDef->successValue)
        continue;

      // Check game mode and function match
      if (!IsModeMatch(questDef->gameModeFlag, gameMode))
        continue;
      if (!IsEventMatch(questDef->function, event, questDef->functionValue, value))
        continue;

      // Advance progress by 1 (all quest functions are count-based)
      entry.progress = std::min(entry.progress + 1, questDef->successValue);

      const bool completed = entry.progress >= questDef->successValue;

      // Determine reward params (only populated on completion)
      const auto rewardType = completed
        ? static_cast<protocol::QuestRewardType>(group.rewardType())
        : protocol::QuestRewardType::None;
      const uint32_t carrotsReward =
        rewardType == protocol::QuestRewardType::Carrots
          ? questDef->rewardGameMoney
          : 0;
      const uint32_t mountExp =
        rewardType == protocol::QuestRewardType::Exp
          ? questDef->rewardExp
          : 0;

      // LOA-fix (R17-1/R17-cap, quest-batch-2, #8): cap THIS quest's class-exp against
      // the account's remaining daily budget (remainingClassExpCap, seeded from
      // dailyClassExpGranted just before the loop) BEFORE it reaches the notify OR the
      // payout, so the client is never shown more class-exp than is credited and the
      // persisted counter equals exactly what the player is told they claimed today.
      // Reward numbers are quests.yaml only. No double-pay: the `progress >= successValue`
      // guard above skips a finished daily. Carrots are NOT capped.
      const uint32_t grantedExp = std::min(mountExp, remainingClassExpCap);
      remainingClassExpCap -= grantedExp;
      if (completed)
      {
        pendingCarrots += carrotsReward;
        pendingMountExp += grantedExp;
      }

      notifies.push_back({
        .characterUid = static_cast<uint32_t>(characterUid),
        .questId = static_cast<uint16_t>(entry.questId),
        .objectiveProgress = {
          .isCompleted = completed,
          .progress = entry.progress,
        },
        .carrotsReward = carrotsReward,
        .rewardType = rewardType,
        .unk2 = 0,
        .mountExp = grantedExp,
      });
    }

    // LOA-fix (R17-cap, quest-batch-2, #8): commit the daily class-exp CLAIM.
    // pendingMountExp is ALREADY capped per-quest in the loop above (each quest's exp was
    // clamped against remainingClassExpCap, which was seeded from dailyClassExpGranted),
    // so the notified exp == the accrued exp == what we record here. rewardType is
    // client-influenced, so this cap MUST live in the same authoritative path as the
    // payout (else a modded client sets rewardType=Exp everywhere and farms ~32K exp/day).
    // SEMANTICS: dailyClassExpGranted tracks class-exp CLAIMED from dailies today (capped
    // at DailyClassExpCap = one horse class/day), which for a normal mounted, non-maxed
    // horse equals what is applied. A missing or max-class mount still consumes the daily
    // claim — fail-closed: the anti-farm bound (never MORE than the cap per account per
    // day, mount-independent) holds regardless. Carrots are NEVER capped.
    group.dailyClassExpGranted = group.dailyClassExpGranted() + pendingMountExp;

    // Mark quests field as modified so it gets persisted
    group.quests = questSlots;
  });

  // LOA-fix (R17-2, quest-batch-2, #8): ФАКТИЧЕСКАЯ ВЫПЛАТА за закрытые сегодня
  // дейлики. ПОЧЕМУ ЗДЕСЬ, А НЕ ВНУТРИ ЦИКЛА: data::Record держит НЕ рекурсивный
  // мьютекс на запись, и брать Character/Horse изнутри лока DailyQuestGroup значило
  // бы держать два лока разом в порядке, которого больше нигде нет. Все точки вызова
  // OnQuestEvent (RanchDirector: кормление/мытьё; RaceNetworkHandler: рывок/
  // перфект-прыжок/фаербол) находятся ВНЕ лока Character — проверено поимённо
  // (тред-аудит), поэтому взятие Character-unique тут безопасно. Character и Horse
  // берём ПОСЛЕДОВАТЕЛЬНО, а не вложенно: в момент правки лошади лок персонажа уже
  // отпущен. Опыт лошади: поля `experience` у data::Horse нет, рост класса идёт
  // только через HorseRegistry::ApplyClassProgress (двигает clazz/growthPoints) —
  // тот же вызов, что на финише заезда.
  if (pendingCarrots > 0 || pendingMountExp > 0)
  {
    data::Uid mountUid = data::InvalidUid;

    characterRecord.Mutable([pendingCarrots, &mountUid](data::Character& character)
    {
      character.carrots() += static_cast<int32_t>(pendingCarrots);
      mountUid = character.mountUid();
    });

    if (pendingMountExp > 0 && mountUid != data::InvalidUid)
    {
      const auto horseRecord = dataDirector.GetHorse(mountUid);
      if (horseRecord)
      {
        horseRecord.Mutable([this, pendingMountExp](data::Horse& horse)
        {
          _serverInstance.GetHorseRegistry().ApplyClassProgress(
            horse, pendingMountExp);
        });
      }
      else
      {
        server::util::QuietLogWarn(
          "Daily quest reward: mount {} of character {} is unavailable; "
          "{} class exp not applied",
          mountUid,
          characterUid,
          pendingMountExp);
      }
    }
  }

  return notifies;
}

// LOA-fix (R-revenge, #13): ЕДИНАЯ ТОЧКА СПИСАНИЯ суточного класс-опыта.
// Нужна бонусу мести (RaceInstance::Stop), чтобы он и дейлики делили ОДИН
// бюджет 6650/сутки. Возвращает выданное, а не булев «влезло/нет»: вызывающий
// обязан показать клиенту ровно то, что начислил.
// АТОМАРНОСТЬ: расчёт и коммит счётчика — внутри одного Mutable группы, поэтому
// два параллельных заезда одного персонажа не могут списать бюджет дважды.
uint32_t QuestSystem::ClaimDailyClassExp(
  const data::Uid characterUid,
  const uint32_t requested)
{
  if (requested == 0)
    return 0;

  auto& dataDirector = _serverInstance.GetDataDirector();

  const auto characterRecord = dataDirector.GetCharacter(characterUid);
  if (!characterRecord)
    return 0;

  data::Uid dailyQuestGroupUid = data::InvalidUid;
  characterRecord.Immutable([&dailyQuestGroupUid](const data::Character& character)
  {
    dailyQuestGroupUid = character.dailyQuestGroupUid();
  });

  // FAIL-CLOSED: без группы дейликов бюджет вести негде. Морковки бонуса это
  // не трогает — вызывающий платит их независимо.
  if (dailyQuestGroupUid == data::InvalidUid)
  {
    server::util::QuietLogDebug(
      "ClaimDailyClassExp: character {} has no daily quest group; "
      "{} class exp not granted",
      characterUid,
      requested);
    return 0;
  }

  auto questGroupRecord = dataDirector.GetDailyQuestGroup(dailyQuestGroupUid);
  if (!questGroupRecord)
    return 0;

  uint32_t granted = 0;
  questGroupRecord.Mutable([requested, &granted](data::DailyQuestGroup& group)
  {
    SyncDailyClassExpBudget(group);   // R42 #8 F2: self-heal cap on day boundary before reading it
    const uint32_t used = group.dailyClassExpGranted();
    if (used >= DailyClassExpCap)
      return;
    const uint32_t remaining = DailyClassExpCap - used;
    granted = requested < remaining ? requested : remaining;
    group.dailyClassExpGranted = used + granted;
  });

  return granted;
}

std::vector<QuestSystem::MainQuestProgress> QuestSystem::AdvanceMainQuests(
  const data::Uid characterUid,
  const std::vector<uint32_t>& tids,
  const bool matchFunctionValue,
  const uint32_t functionValue)
{
  // LOA-fix (F7, quest-batch-1): продвижение СЮЖЕТНЫХ квестов. OnQuestEvent выше
  // работает исключительно по 3 слотам дневной группы — MAIN-квесты туда не
  // попадают никогда, поэтому диспатч RunMap/PrizeWinner через него был бы
  // NO-OP'ом. Зеркалим форму хука кормления (P4a): гард InProgress, инкремент с
  // капом по successValue, перевод в ReadyToClaim по достижению цели.
  // Deadlock-safety: Character берётся shared, Quest — unique; это РАЗНЫЕ записи,
  // вложенности одного и того же мьютекса нет (Record использует НЕ рекурсивный
  // shared_mutex). Вызывать только ВНЕ characterRecord.Mutable вызывающего.
  std::vector<MainQuestProgress> advanced;
  if (tids.empty())
    return advanced;

  auto& dataDirector = _serverInstance.GetDataDirector();
  const auto& questRegistry = _serverInstance.GetQuestRegistry();

  const auto characterRecord = dataDirector.GetCharacter(characterUid);
  if (!characterRecord)
    return advanced;

  characterRecord.Immutable(
    [&dataDirector, &questRegistry, &tids, matchFunctionValue, functionValue, &advanced](
      const data::Character& character)
    {
      const auto questRecords = dataDirector.GetQuestCache().Get(character.quests());
      if (not questRecords)
        return;

      for (const auto& questRecord : *questRecords)
      {
        questRecord.Mutable(
          [&questRegistry, &tids, matchFunctionValue, functionValue, &advanced](
            data::Quest& quest)
          {
            if (quest.isCompleted() != data::Quest::Status::InProgress)
              return;

            const auto qid = static_cast<uint32_t>(quest.questId());
            bool wanted = false;
            for (const uint32_t tid : tids)
              if (tid == qid) { wanted = true; break; }
            if (not wanted)
              return;

            const auto tmpl = questRegistry.GetQuest(qid);
            if (not tmpl.has_value())
              return;

            // RunMap / PrizeWinnerInMap: квест засчитывается только на «своей»
            // карте (Quest::functionValue == mapBlockId заезда).
            if (matchFunctionValue && tmpl->functionValue != functionValue)
              return;

            // Кап и перевод статуса РАЗДЕЛЕНЫ (тот же порядок, что в F5):
            // запись, уже стоящая на N/N со статусом InProgress, должна стать
            // ReadyToClaim, а не проваливаться в early-return навсегда.
            if (quest.progress() < tmpl->successValue)
              quest.progress() = quest.progress() + 1;
            if (quest.progress() >= tmpl->successValue)
              quest.isCompleted() = data::Quest::Status::ReadyToClaim;

            advanced.push_back(MainQuestProgress{
              .questTid = qid,
              .progress = quest.progress(),
              .successValue = tmpl->successValue,
              .completed = quest.progress() >= tmpl->successValue});
          });
      }
    });

  return advanced;
}

} // namespace server
