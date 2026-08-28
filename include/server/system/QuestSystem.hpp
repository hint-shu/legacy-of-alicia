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

#ifndef QUESTSYSTEM_HPP
#define QUESTSYSTEM_HPP

#include <libserver/data/DataDefinitions.hpp>
#include <libserver/network/command/proto/CommonMessageDefinitions.hpp>
#include <libserver/network/command/proto/CommonStructureDefinitions.hpp>
#include <libserver/registry/QuestRegistry.hpp>

#include <array>
#include <vector>

namespace server
{

class ServerInstance;

class QuestSystem
{
public:
  explicit QuestSystem(ServerInstance& serverInstance);

  //! Events that can advance daily quest objectives.
  //! Each value corresponds to a quest `function` string in quests.yaml.
  enum class QuestEvent
  {
    //! Any action (used by quests with function 'TRUE').
    Any,
    //! Finished in a placing position (1st–3rd).
    PrizeWinner,
    //! Achieved a perfect jump over a hurdle.
    PerfectJump,
    //! Used a fireball / magic attack.
    FireballAttack,
    //! Completed a specific map (value = map block ID).
    RunMap,
    //! Won a team race.
    TeamWin,
    //! Accumulated gliding distance (value = distance units).
    GlidingDistance,
    //! Collected a drop item during a race.
    CollectDropItem,
  };

  //! Evaluates all active daily quests for a character against the given event
  //! and advances progress on any matching quests.
  //! @param characterUid UID of the character.
  //! @param event The event that occurred.
  //! @param gameMode The game mode in which the event occurred.
  //! @param value Optional scalar value for the event (e.g. map ID, distance).
  //! @returns A list of notify packets to be sent to the character by the caller.
  //! LOA-fix (F8, quest-batch-1): результат продвижения одного MAIN-квеста.
  struct MainQuestProgress
  {
    //! TID квеста.
    uint32_t questTid{};
    //! Прогресс ПОСЛЕ инкремента.
    uint32_t progress{};
    //! Цель квеста (successValue).
    uint32_t successValue{};
    //! Цель достигнута (запись переведена в ReadyToClaim).
    bool completed{};
  };

  [[nodiscard]] std::vector<protocol::AcCmdRCUpdateDailyQuestNotify> OnQuestEvent(
    data::Uid characterUid,
    QuestEvent event,
    registry::Quest::GameModeFlag gameMode,
    uint32_t value = 0,
    const std::vector<uint32_t>& tidFilter = {});

  //! LOA-fix (F7, quest-batch-1): продвигает MAIN-квесты (groupType 0) персонажа.
  //! OnQuestEvent выше умеет двигать ТОЛЬКО 3 слота дневной группы; сюжетные
  //! квесты живут в character.quests() как отдельные data::Quest-записи, поэтому
  //! для них нужен собственный проход. Список tid'ов задаётся вызывающей стороной
  //! ЯВНО: в данных квест «проведи заезд» и квест «покорми лошадь» неразличимы
  //! (у обоих function TRUE), так что вывести список из quests.yaml нельзя.
  //! @param characterUid UID персонажа.
  //! @param tids Разрешённые TID'ы (пустой список = не делать ничего).
  //! @param matchFunctionValue Требовать совпадения Quest::functionValue со
  //!        значением события (карта для RunMap/PrizeWinnerInMap).
  //! @param functionValue Значение события для сверки.
  //! @returns Список продвинутых квестов (для логов/нотификаций вызывающего).
  std::vector<MainQuestProgress> AdvanceMainQuests(
    data::Uid characterUid,
    const std::vector<uint32_t>& tids,
    bool matchFunctionValue = false,
    uint32_t functionValue = 0);

  //! Converts a protocol GameMode + TeamMode pair to the matching GameModeFlag
  //! used by the quest registry for mode-based filtering.
  //! @param gameMode Speed or Magic.
  //! @param teamMode Team or Solo.
  //! @returns The corresponding GameModeFlag value.
  static registry::Quest::GameModeFlag ToGameModeFlag(
    protocol::GameMode gameMode,
    protocol::TeamMode teamMode);

  //! LOA-fix (R5, round2): режим-флаг для события ПРИЗОВОГО МЕСТА.
  //! Дейлики «войти в тройку» объявлены в quests.yaml как gameModeFlag 33
  //! (WinSpeedSolo — 1000/1011) и 68 (WinMagicSolo — 1001/1012), а ToGameModeFlag
  //! возвращает для соло 35/76 (…SoloAction) → IsModeMatch(33, 35) ложно и весь
  //! класс призовых дейликов не двигался никогда. Значения 33/68 до этого патча
  //! не использовались нигде, кроме объявления enum'а.
  //! @param gameMode Speed или Magic.
  //! @param teamMode Team или Solo.
  //! @returns 33/68 для соло-заездов, 2/8 для командных — командный заезд личное
  //!          призовое место не засчитывает, так и задумано в данных («개인전»).
  static registry::Quest::GameModeFlag ToPrizeGameModeFlag(
    protocol::GameMode gameMode,
    protocol::TeamMode teamMode);

  //! LOA-fix (NEW-1, round3): есть ли у сервера СОБСТВЕННЫЙ счётчик этого класса
  //! целей. Единая точка правды для двух мест: HandleUpdateDailyQuest (что не
  //! пишем со слов клиента) и HandleRequestDailyQuestReward (какие слоты идут в
  //! гейт и в сумму очков). Классы, которых сервер измерить не может
  //! (GlidingDistanceValue, CollectDropItem, ClearMission), навсегда остаются на
  //! нуле — награду за них выдавать нельзя, но и блокировать ими день тоже.
  //! @param function Класс цели из quests.yaml.
  //! @returns `true`, если прогресс двигают серверные хуки.
  static bool IsServerTrackedFunction(registry::Quest::Function function);

  //! LOA-fix (R68, backlog #5/#99): СЮЖЕТНЫЕ квесты класса `CollectDropItem`.
  //!
  //! ★ЕДИНЫЙ СПИСОК НА ТРИ МЕСТА, и это не украшательство. Один и тот же набор
  //! читают: раскладка предметов на старте заезда
  //! (`RaceInstance::PrepareQuestItems`), продвижение при подборе
  //! (`RaceNetworkHandler::HandleUserRaceItemGet`) и гейт выплаты у NPC
  //! (`RanchDirector::HandleRequestQuestReward`). Три копии чисел разъехались
  //! бы молча, а цена расхождения РАЗНАЯ по направлениям: лишний tid в гейте
  //! выплаты = вечно несдаваемый квест, недостающий = награда без прогресса.
  //!
  //! ★СПИСОК ЯВНЫЙ, А НЕ ВЫВЕДЕННЫЙ ИЗ quests.yaml — та же причина, что у
  //! `AdvanceMainQuests`: в данных сюжетный квест и ивентовый неразличимы по
  //! классу цели. Здесь ровно 12 MAIN-квестов (groupType 0). Пять ИВЕНТОВЫХ
  //! квестов того же класса (1035-1039, groupType 7) НЕ входят: их предметы
  //! кладёт дека 701, у которой в courses.yaml нет ни одной координаты, —
  //! прогресс им всё равно недостижим, а попадание в гейт выплаты сделало бы
  //! их несдаваемыми навсегда.
  static constexpr std::array<uint32_t, 12> CollectDropItemMainQuestTids{
    12013u, 12014u, 12015u, 12016u, 12017u,
    14010u, 14014u, 14019u, 14020u, 14021u, 14024u, 14028u};

  //! LOA-fix (R-revenge, #13): СУТОЧНЫЙ ПОТОЛОК класс-опыта лошади для «мягких»
  //! каналов (дейлики R17-cap + бонус мести). 6650 = ровно один класс 1->2.
  //! ПОЧЕМУ ОБЩИЙ: с раздельными счётчиками каждый канал независимо даёт по
  //! классу в день и кап R17 перестаёт быть капом. Опыт с самого заезда
  //! (клиентский gainedClassProgress, R20-2) сюда НЕ входит — у него свой
  //! пер-заездный кламп MaxPlausibleClassProgress.
  //! Поднят из тела OnQuestEvent в класс, чтобы источник правды был один.
  static constexpr uint32_t DailyClassExpCap = 6650;

  //! LOA-fix (R-revenge, #13): списывает `requested` класс-опыта из СУТОЧНОГО
  //! бюджета персонажа и возвращает, сколько РЕАЛЬНО разрешено выдать (0..requested).
  //! Вызывающий ОБЯЗАН применить к лошади именно возвращённое значение и показать
  //! клиенту именно его — инвариант «показано == начислено».
  //! Счётчик — DailyQuestGroup::dailyClassExpGranted, тот же, что у дейликов
  //! (R17-cap); суточный сброс уже делает RanchDirector.
  //! ЛОКИ: берёт Character(shared) и DailyQuestGroup(unique) ПОСЛЕДОВАТЕЛЬНО.
  //! Звать ТОЛЬКО вне любого Character/Horse-Mutable — Record'ный shared_mutex
  //! не рекурсивный, вложение = дедлок.
  //! FAIL-CLOSED: если у персонажа нет группы дейликов (dailyQuestGroupUid ==
  //! InvalidUid — группа заводится клиентской регистрацией), вернёт 0.
  //! @param characterUid UID персонажа.
  //! @param requested Сколько класс-опыта хочет выдать вызывающий.
  //! @returns Фактически списанный (= разрешённый к выдаче) класс-опыт.
  [[nodiscard]] uint32_t ClaimDailyClassExp(
    data::Uid characterUid,
    uint32_t requested);

private:
  //! Returns true if the quest's gameModeFlag is compatible with the given mode.
  static bool IsModeMatch(
    registry::Quest::GameModeFlag questFlag,
    registry::Quest::GameModeFlag eventMode);

  //! Returns true if the quest's function matches the given event.
  static bool IsEventMatch(
    registry::Quest::Function function,
    QuestEvent event,
    uint32_t questFunctionValue,
    uint32_t eventValue);

  ServerInstance& _serverInstance;
};

} // namespace server

#endif // QUESTSYSTEM_HPP
