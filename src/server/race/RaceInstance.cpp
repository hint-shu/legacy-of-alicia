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

#include "server/ServerInstance.hpp"
#include "libserver/util/QuietLog.hpp"

#include "server/race/RaceInstance.hpp"
#include "server/race/RaceNetworkHandler.hpp"

#include <libserver/util/Util.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>
#include <format>

namespace server
{

namespace
{

constexpr registry::MapBlockId AllMapsCourseId = 10000;
constexpr registry::MapBlockId NewMapsCourseId = 10001;
constexpr registry::MapBlockId HotMapsCourseId = 10002;
//! Номер шины достижений «финиш заезда» (UserAchvEvent = 2).
constexpr uint16_t RaceAchievementEvent = 2;

//! LOA (R70 итерация 2, backlog #58): СКОЛЬКО ТРАССЫ НАДО ПРОЙТИ, ЧТОБЫ
//! СЧИТАТЬСЯ УЧАСТНИКОМ для ранговых условий достижений (доля 0..1).
//!
//! ★ЗАЧЕМ ЭТО ВООБЩЕ. Первая редакция считала составом «всех НЕотключившихся»,
//! и ревью (итерация 2) показало дыру: подключённый, но МОЛЧАЩИЙ альт ростер
//! всё равно покупает. Три (семь) таких аккаунтов, просто вошедших в заезд и не
//! приславших ни одного пакета, дают `numPlayer` 4 (8) — а это ПОВТОРЯЕМЫЕ тиры
//! `Win`/`TeamWin` (10225/10226/10054/10227/10228/10060, 24 очка) после
//! 15-секундного таймаута финиширования.
//! ★ЧЕМ МЕРЯЕМ: `Racer::trustedProgress` — СЕРВЕРНАЯ копия прогресса (R-revenge,
//! #13). Она растёт только после зелёного света, только в состоянии `Racing`, не
//! быстрее правдоподобного темпа (вся трасса за `MinPlausibleCourseTime`) и
//! никогда не убывает. Молчащий гонщик остаётся на нуле по построению: сервер
//! сам прогресс не выдаёт.
//! ★ПОЧЕМУ 0.1, А НЕ «БОЛЬШЕ НУЛЯ». Ноль отсекает только полное молчание, а
//! «больше нуля» покупается ОДНИМ пакетом позиции. Десятая часть трассы при
//! потолке темпа стоит не меньше трёх секунд отчётов о движении. Честному
//! гонщику этот порог не мешает никогда: круг в игре идёт полторы-три минуты, а
//! к моменту `Stop()` заезд уже отработал либо лимит карты, либо 15 секунд после
//! первого финиша.
//! ★ЧЕГО ЭТОТ ПОРОГ НЕ ДЕЛАЕТ (говорим прямо, чтобы не читалось шире): отличить
//! человека от скрипта сервер не может в принципе. Порог поднимает цену альта с
//! «войти и стоять» до «весь заезд слать правдоподобные пакеты движения» —
//! ровно ту, что платит игрок. Полностью закрыть чеканку `Win` сговором живых
//! аккаунтов нельзя ничем, кроме отказа от записи с `numPlayer`.
constexpr float MinMeaningfulRaceProgress = 0.1f;

//! Одна карта мастерства: имя условия каталога, ИМЯ КАРТЫ и потолок времени.
//!
//! ★ЕДИНИЦЫ. `racer.courseTime` — МИЛЛИСЕКУНДЫ (`tracker::MinPlausibleCourseTime
//! = 30000` = 30 с), а пороги оригинала — СЕКУНДЫ (`ach_lib.lua:15-27`,
//! `map_mastery_impl(me, id, сек)`). Пол правдоподобия (30 с) НИЖЕ любого потолка
//! мастерства — значит мастерство доказуемо честной ездой, а не только
//! «мгновенным финишем», который сервер всё равно превращает в DNF.
//! ★СВЕРКА ПО ИМЕНИ КАРТЫ, А НЕ ПО ID: см. комментарий у
//! `Course::MapBlockInfo::name`.
struct MasteryCourse
{
  std::string_view condition;
  std::string_view mapName;
  uint32_t courseTimeLimitMs;
};

//! Семь карт мастерства события 2. Значения — из `ach_conditions.lua:16-52`.
constexpr std::array<MasteryCourse, 7> MasteryCourses{{
  {"RiLand01Mastery", "ri_land01", 140000},
  {"RiLand02Mastery", "ri_land02", 140000},
  {"RiLand03Mastery", "ri_land03", 150000},
  {"RiLand04Mastery", "ri_land04", 130000},
  {"RiFore01Mastery", "ri_fore01", 140000},
  {"RiFore02Mastery", "ri_fore02", 160000},
  {"RiDorf04Mastery", "ri_dorf04", 150000},
}};

//! Часовое окно события 2 или пустая строка, если текущий час ни в одно не попал.
//!
//! ★Окна — дословно из оригинала (`ach_conditions.lua:457-497`): [8,13), [14,16),
//! [16,18), [19,22). Они НЕ покрывают сутки целиком и НЕ пересекаются, поэтому
//! активным может быть максимум одно.
//! ★Час берётся в игровом часовом поясе (`server::util::CurrentGameLocalHour`),
//! ОДИН раз на заезд: заезд, начавшийся в 15:59 и кончившийся в 16:01, попадает
//! в окно по МОМЕНТУ ПОДВЕДЕНИЯ ИТОГОВ — ровно как `os.date` в оригинале,
//! который вызывался при вычислении условия.
constexpr std::string_view GoalInHourWindowCondition(const uint32_t hour)
{
  if (hour >= 8 and hour < 13) return "GoalIn_8h_13h";
  if (hour >= 14 and hour < 16) return "GoalIn_14h_16h";
  if (hour >= 16 and hour < 18) return "GoalIn_16h_18h";
  if (hour >= 19 and hour < 22) return "GoalIn_19h_22h";
  return {};
}

//! LOA-fix (R11-1, round11, backlog #22): ПОТОЛОК СТАДИИ ЗАГРУЗКИ КАРТЫ.
//! Апстрим держал 60 c магическим числом в теле RaceInstance::Start() под
//! комментарием "todo: configurable loading timeout". Реальный бюджет клиента
//! был ещё меньше — дедлайн взводится в Start(), а команду «грузи карту»
//! (AcCmdCRStartRaceNotify) клиент получает через countdown комнаты (~5.3 c),
//! то есть на загрузку оставалось ~54.7 c. Медленные клиенты (HDD, слабое
//! железо) не успевали и вылетали из комнаты каждый заезд, ломая синхронность
//! остальным. Поднято до 150 c; точка взвода дедлайна перенесена туда, где
//! клиент реально начинает грузиться (ArmLoadingDeadline, R11-3).
constexpr std::chrono::seconds LoadingStageTimeout{150};

//! LOA-fix (R11-14, round11, backlog #20 п.5): ЗАПАС ВРЕМЕНИ ДЛЯ СОЛО-ЗАЕЗДА.
//! У соло-комнаты апстрим ставил дедлайн стадии Racing в time_point::max(),
//! то есть выйти из Racing можно было ТОЛЬКО по AcCmdUserRaceFinal от самого
//! гонщика. Гонщик вылетел/завис — комната остаётся в Racing навсегда и уже
//! никогда не возвращается в Waiting. Даём соло тот же лимит карты плюс этот
//! запас: честный круг занимает 1.5-3 минуты при лимите карты 300 c, поэтому
//! +300 c гарантированно не режет живую игру, но снимает вечное залипание.
constexpr std::chrono::seconds SoloRaceGracePeriod{300};

} // anon namespace

RaceInstance::RaceInstance(
  RaceNetworkHandler& raceDirector,
  const uint32_t roomUid)
  : _roomUid(roomUid)
  , _raceNetworkHandler(raceDirector)
{

}

void RaceInstance::GetRoom(const std::function<void(Room&)>& consumer)
{
  _raceNetworkHandler.GetServerInstance().GetRoomSystem().GetRoom(
    _roomUid,
    consumer);
}

void RaceInstance::GetRoom(const std::function<void(const Room&)>& consumer) const
{
  _raceNetworkHandler.GetServerInstance().GetRoomSystem().GetRoom(
    _roomUid,
    consumer);
}

std::unordered_map<data::Uid, network::ClientId>
RaceInstance::SnapshotRoomClientIds() const
{
  std::unordered_map<data::Uid, network::ClientId> clientIds;
  this->GetRoom(
    [&clientIds](const Room& room)
    {
      for (const auto& [characterUid, player] : room.GetPlayers())
        clientIds.emplace(characterUid, player.GetClientId());
    });
  return clientIds;
}

bool RaceInstance::Start(
  const Parameters& parameters)
{
  _parameters = parameters;

  try
  {
    PrepareGameMode();
    PrepareMap();
  }
  catch (const std::runtime_error& e)
  {
    server::util::QuietLogError("Failed to start race instance: {}", e.what());
    return false;
  }

  _stage = Stage::Loading;
  _loadingStartTimePoint = Clock::now();
  // LOA-fix (R8-1a, round8): ИНВАРИАНТ — _raceStartTimePoint валиден ТОЛЬКО
  // после реального перехода в Stage::Racing.
  // ЧТО БЫЛО НЕ ТАК: апстрим присваивает эту метку ровно в одном месте —
  // TickLoading (Loading → Racing), — а экземпляр RaceInstance живёт вместе с
  // комнатой и переиспользуется из заезда в заезд. Повторный Start() метку не
  // трогал, поэтому между заездами (и при re-entrant StartRace, см. R8-1b) она
  // указывала на старт ПРЕДЫДУЩЕГО заезда. Серверное измерение времени в
  // HandleUserRaceFinal (кламп A3/B5) считало elapsedMs от протухшей метки →
  // получались минуты, гейт правдоподобности B1
  // (finishCourseTime >= MinPlausibleCourseTime, 30 c) проходил мгновенно, и
  // сюжетные счётчики заездов накручивались пакетом финиша без езды.
  // ТЕПЕРЬ: сброс в max() возвращает обоим механизмам честное измерение — пока
  // TickLoading не перевзвёл метку, HandleUserRaceFinal уходит в уже
  // существующую ветку «финиш до старта заезда» и кладёт courseTime = 0.
  // Честную игру не задевает: легальный AcCmdUserRaceFinal физически не может
  // прийти раньше перевзвода — клиентский таймер стартует по
  // AcCmdUserRaceCountdown, который рассылает тот же TickLoading вместе с
  // присвоением метки.
  _raceStartTimePoint = Clock::time_point::max();
  // LOA-fix (R11-2, round11, backlog #22): 60 c → именованная LoadingStageTimeout
  // (150 c). Апстримный todo снят: константа теперь одна и подписана. Это
  // ПЕРВИЧНЫЙ взвод — он покрывает окно между StartRace и рассылкой
  // AcCmdCRStartRaceNotify; окончательный дедлайн переставляет
  // ArmLoadingDeadline() ровно в момент, когда клиенты получают команду грузить
  // карту (R11-3).
  _stageTimeoutTimePoint = _loadingStartTimePoint + LoadingStageTimeout;

  // R56 (#61): ростер ботов — пер-заездное состояние, и чистится он ЗДЕСЬ,
  // вплотную к `Tracker::Clear()` из вызывающего.
  //
  // ★Это же и есть фикс апстрим-бага B1. У них список ботов не чистился нигде
  // (21 обращение, ноль `.clear()`), а условие спавна требовало пустого списка
  // — значит боты появлялись в комнате РОВНО ОДИН РАЗ, а дальше все ветки
  // «если боты есть» продолжали срабатывать вхолостую. Достаточно было
  // привязать время жизни списка к времени жизни заезда, а не комнаты.
  _aiRacers.clear();

  // LOA-fix (R67-3, backlog #128b): НОВЫЙ ЗАЕЗД — НОВАЯ ЭПОХА.
  //
  // Довод тот же, что абзацем выше про `_aiRacers`: это ПЕР-ЗАЕЗДНОЕ
  // состояние, и меняется оно там же, где кончается прошлый заезд и
  // начинается новый. Отложенные джобы командного калибра
  // (`RaceNetworkHandler::HandleTeamGauge`) сравнивают захваченное значение с
  // этим и становятся no-op, если между планированием и исполнением комната
  // успела уехать в следующий заезд.
  //
  // ★ИНКРЕМЕНТ СТОИТ ПОСЛЕ `PrepareGameMode`/`PrepareMap`, то есть на пути,
  // где заезд ДЕЙСТВИТЕЛЬНО начался. Неудачный `Start()` возвращает false из
  // catch выше, мастеру уходит StartRaceCancel — нового заезда нет, и менять
  // номер нечему.
  // ★СТАДИЮ неудачный `Start()` не трогает вовсе (`_stage = Stage::Loading`
  // стоит НИЖЕ catch), и «останется Waiting» было бы неправдой: аварийная
  // ветка R11-15 пускает старт из ПРОСРОЧЕННОЙ стадии, и при неудаче
  // подготовки комната останется в ней же — Loading/Racing/Finishing. Для
  // эпохи это безразлично: она отмечает НАЧАВШИЙСЯ заезд, а не стадию. Цена
  // такого исхода — джоб прошлого заезда всё ещё считается своим; это ровно
  // пред-существующее поведение «заезд кончился, а джоб ещё летит», и первый
  // же удавшийся `Start()` его отсекает.
  ++_raceEpoch;

  return true;
}

// LOA-fix (R11-3b, round11, backlog #20 п.4): ПЕРЕВЗВОД ДЕДЛАЙНА ЗАГРУЗКИ.
// Клиент начинает грузить карту не тогда, когда мастер нажал «старт», а когда
// получил AcCmdCRStartRaceNotify — то есть на countdown комнаты позже
// (SystemContentRegistry ключ 17, дефолт 5310 мс). Дедлайн, взведённый в
// Start(), эти секунды съедал молча, и «60 секунд на загрузку» на деле были
// ~54.7. Зовём этот метод из планировщика ровно там, где рассылается
// StartRaceNotify — тогда написанное и реальное совпадают при ЛЮБОМ значении
// countdown на проде.
// Идемпотентно и безопасно: работает только в стадии Loading, поэтому
// опоздавший/лишний вызов на уже стартовавшем заезде — no-op.
void RaceInstance::ArmLoadingDeadline()
{
  if (_stage != Stage::Loading)
    return;

  _loadingStartTimePoint = Clock::now();
  _stageTimeoutTimePoint = _loadingStartTimePoint + LoadingStageTimeout;
}

void RaceInstance::Stop()
{
  protocol::AcCmdRCRaceResultNotify raceResult{};

  using Team = tracker::RaceTracker::Racer::Team;
  using State = tracker::RaceTracker::Racer::State;

  // Determine winning team (team of the first finisher).
  // Solo/FFA leaves `winningTeam` as Solo.
  Team winningTeam = Team::Solo;
  if (_parameters.teamMode == protocol::TeamMode::Team)
  {
    uint32_t best = tracker::InvalidCourseTime;
    for (const auto& racer : _tracker.GetRacers() | std::views::values)
    {
      if (racer.state != State::Disconnected
        && racer.courseTime != tracker::InvalidCourseTime
        && racer.courseTime < best)
      {
        best = racer.courseTime;
        winningTeam = racer.team;
      }
    }
  }

  // Build the score board.
  for (const auto& [characterUid, racer] : _tracker.GetRacers())
  {
    auto& score = raceResult.scores.emplace_back();

    // todo: figure out the other bit set values

    if (racer.state != State::Disconnected)
    {
      score.bitset = protocol::AcCmdRCRaceResultNotify::ScoreInfo::Bitset::Connected;
    }

    // If the player has disconnected
    score.courseTime = racer.state != State::Disconnected
      ? racer.courseTime
      : tracker::InvalidCourseTime;

    static constexpr uint32_t BaseExpReward = 420;
    static constexpr uint32_t BaseCarrotReward = 2500;

    score.experience = BaseExpReward;
    score.carrots = BaseCarrotReward;

    {
      // Multiplier as a percentage (example 100%)
      constexpr uint32_t CarrotExpMultiplierKey = 18;
      constexpr float DefaultCarrotExpMultiplier = 1.0f;
      const auto& carrotExpMultiplierOpt = _raceNetworkHandler.GetServerInstance()
        .GetSystemContentRegistry()
        .GetValue(CarrotExpMultiplierKey);

      const float systemMultiplier = carrotExpMultiplierOpt.has_value() ?
          carrotExpMultiplierOpt.value() / 100.0f :
          DefaultCarrotExpMultiplier;

      using BonusCourseType = protocol::BonusCourseType;
      using Bitset = protocol::AcCmdRCRaceResultNotify::ScoreInfo::Bitset;

      // Apply rewards only if player has finished the race
      if (racer.courseTime != tracker::InvalidCourseTime)
      {
        // TODO: put these in the config
        constexpr float EventCarrotMultiplier = 1.5f;
        constexpr float EventExpMultiplier = 2.0f;

        // Apply carrots
        if (_bonusCourseType == BonusCourseType::Carrots || _bonusCourseType == BonusCourseType::CarrotsAndExperience)
        {
          score.carrots = static_cast<uint32_t>(
            static_cast<float>(score.carrots) * systemMultiplier * EventCarrotMultiplier);
          score.bitset = static_cast<Bitset>(
            score.bitset | Bitset::EventBonusCarrots);
        }
        else
        {
          score.carrots = static_cast<uint32_t>(
            static_cast<float>(score.carrots) * systemMultiplier);
        }

        // Apply experience/bonus experience
        if (_bonusCourseType == BonusCourseType::Experience || _bonusCourseType == BonusCourseType::CarrotsAndExperience)
        {
          score.experience = static_cast<uint32_t>(
            static_cast<float>(score.experience) * EventExpMultiplier);
          score.bitset = static_cast<Bitset>(
            score.bitset | Bitset::EventBonusExperience);
        }
        else
        {
          // TODO: Apply bonus carrots only for now, do not touch exp
        }
      }
    }

    score.teamColor = racer.team;
    const auto characterRecord = _raceNetworkHandler.GetServerInstance().GetDataDirector().GetCharacter(
      characterUid);

    characterRecord.Mutable([this, &score](data::Character& character)
    {
      character.carrots() += score.carrots;
      character.experience() += score.experience;

      const uint32_t newLevel = _raceNetworkHandler.GetServerInstance().GetCharacterRegistry().GetLevelForExp(character.experience());
      if (newLevel > character.level())
      {
        character.level() = newLevel;
        score.bitset = static_cast<protocol::AcCmdRCRaceResultNotify::ScoreInfo::Bitset>(
          score.bitset | protocol::AcCmdRCRaceResultNotify::ScoreInfo::Bitset::LevelUp);
      }

      //populate the score info with the character data
      score.uid = character.uid();
      score.name = character.name();
      score.level = character.level();
      score.levelProgress = character.experience();

      _raceNetworkHandler.GetServerInstance().GetDataDirector().GetHorse(character.mountUid()).Immutable(
        [&score](const data::Horse& horse)
        {
          score.mountName = horse.name();
          score.horseClass = static_cast<uint8_t>(horse.clazz());
          score.horseClassProgress = horse.clazzProgress();
          score.growthPoints = static_cast<uint16_t>(horse.growthPoints());
        });

      // === LOA per-finish hooks — только для финишировавших ==============
      // courseTime валиден лишь у доехавших (у DNF/дисконнекта он
      // InvalidCourseTime, выставляется выше). Тот же гейт, что у начисления
      // наград — «доехал → награда И небольшой шанс износа».
      if (score.courseTime != tracker::InvalidCourseTime)
      {
        // --- S2: износ-травма на финише ---------------------------------
        // Оригинальная игра травмировала лошадей; эмулятор — нет. С небольшим
        // шансом наносим ЛЁГКУЮ травму и ТОЛЬКО если травмы сейчас нет (не
        // штабелируем). Cure-item из Batch A снимает её в 0 → петля ухода за
        // лошадью. Тяжёлые коды (18/34/66) здесь не наносим.
        // ТЮНИНГ — две константы ниже:
        //   InjuryChancePercent — шанс травмы за финиш, в процентах (0 = выкл);
        //   LightInjuryCodes    — набор лёгких травм, регион равновероятен.
        static constexpr uint32_t InjuryChancePercent = 10;
        // Коды == data::Horse::mountCondition.injury (protocol::Horse::Injury):
        // 17=MinorMuscleStrain, 33=MinorWounds, 65=MinorFracture.
        static constexpr uint32_t LightInjuryCodes[] = {17u, 33u, 65u};

        auto& injuryRng = server::util::GetRandomEngine();
        const bool inflictInjury =
          std::uniform_int_distribution<uint32_t>(1u, 100u)(injuryRng)
            <= InjuryChancePercent;
        if (inflictInjury)
        {
          constexpr size_t injuryCodeCount =
            sizeof(LightInjuryCodes) / sizeof(LightInjuryCodes[0]);
          const uint32_t injuryCode = LightInjuryCodes[
            std::uniform_int_distribution<size_t>(
              0u, injuryCodeCount - 1u)(injuryRng)];

          _raceNetworkHandler.GetServerInstance().GetDataDirector().GetHorse(
            character.mountUid()).Mutable(
            [injuryCode](data::Horse& horse)
            {
              // Не штабелируем: наносим только на здоровую лошадь.
              if (horse.mountCondition.injury() == 0)
                horse.mountCondition.injury() = injuryCode;
            });
        }

        // --- S8: прогресс дейлик-квестов «сделай N заездов» --------------
        // Сам вызов QuestSystem::OnQuestEvent делается НЕ здесь, а после
        // сортировки результатов (ниже), ВНЕ characterRecord.Mutable:
        // OnQuestEvent берёт shared-lock того же character-рекорда, а Record
        // использует НЕ рекурсивный shared_mutex → вызов внутри Mutable = дедлок.
      }
    });
  }

  // Sort: winning team first, then by result state, then by courseTime ascending.
  std::ranges::sort(raceResult.scores, [winningTeam](const auto& a, const auto& b)
  {
    auto priority = [winningTeam](const auto& score)
    {
      using ScoreInfo = protocol::AcCmdRCRaceResultNotify::ScoreInfo;

      const uint32_t bitset = static_cast<uint32_t>(score.bitset);
      const bool isConnected = (bitset & static_cast<uint32_t>(
        ScoreInfo::Bitset::Connected)) != 0;
      const bool hasValidTime = score.courseTime < tracker::InvalidCourseTime;

      // Connected racers with no valid finish time are time-over/DNF and should rank
      // below timed finishers but above disconnected racers.
      const auto resultRank = not isConnected ? 2 : hasValidTime ? 0 : 1;

      return std::make_tuple(
        score.teamColor != winningTeam ? 1 : 0,
        resultRank,
        score.courseTime);
    };
    return priority(a) < priority(b);
  });

  // === LOA-fix (race-stats): персистим счётчики побед лошади на финише ====
  // Профиль «Статистика заездов» был в нулях: сервер шлёт mountInfo клиенту, но
  // никогда его не инкрементил. Начисляем победу горсу победителя(ей) здесь —
  // после сортировки и после закрытия всех per-racer Mutable выше. Дискриминаторы
  // известны на финише: gameMode (Speed/Magic) + teamMode (Team → *Team, иначе →
  // *Single). Deadlock-safe: Character(shared) закрывается ДО Horse(unique).
  {
    const bool isSpeed = _parameters.gameMode == protocol::GameMode::Speed;
    const bool isMagic = _parameters.gameMode == protocol::GameMode::Magic;
    if (isSpeed || isMagic)
    {
      const bool isTeam = _parameters.teamMode == protocol::TeamMode::Team;

      // Начисляем одну победу горсу победителя: mountUid берём из Character
      // (Immutable закрываем ДО горс-Mutable, локи не вложены).
      const auto creditWin = [this, isSpeed, isTeam](const data::Uid winnerUid)
      {
        data::Uid mountUid = data::InvalidUid;
        _raceNetworkHandler.GetServerInstance().GetDataDirector().GetCharacter(
          winnerUid).Immutable(
          [&mountUid](const data::Character& character)
          {
            mountUid = character.mountUid();
          });
        if (mountUid == data::InvalidUid)
          return;

        _raceNetworkHandler.GetServerInstance().GetDataDirector().GetHorse(
          mountUid).Mutable(
          [isSpeed, isTeam](data::Horse& horse)
          {
            if (isSpeed)
            {
              if (isTeam)
                horse.mountInfo.winsSpeedTeam() += 1;
              else
                horse.mountInfo.winsSpeedSingle() += 1;
            }
            else
            {
              if (isTeam)
                horse.mountInfo.winsMagicTeam() += 1;
              else
                horse.mountInfo.winsMagicSingle() += 1;
            }
          });
      };

      if (isTeam)
      {
        // Командный заезд: победу получает КАЖДЫЙ доехавший из победившей команды
        // (winningTeam вычислен выше = команда первого финишировавшего).
        for (const auto& scoreInfo : raceResult.scores)
        {
          if (scoreInfo.courseTime == tracker::InvalidCourseTime)
            continue;
          if (scoreInfo.teamColor != winningTeam)
            continue;
          creditWin(scoreInfo.uid);
        }
      }
      else
      {
        // Solo/FFA: победа только у 1-го места (после сортировки — scores[0]) и
        // только если он реально доехал (валидный courseTime).
        if (not raceResult.scores.empty()
            && raceResult.scores[0].courseTime != tracker::InvalidCourseTime)
          creditWin(raceResult.scores[0].uid);
      }
    }
  }

  // === LOA-fix (R24, #14 фаза 1): персистим пер-заездную статистику лошади =====
  // topSpeed/totalDistance накоплены в HandleRaceUserPos. Единицы: метры и км/ч×10
  // (см. комменты DataDefinitions.hpp + R24 в apply_patches.py). Гейт тот же, что у
  // побед: доехал (валидный courseTime) и время правдоподобное (анти-фарм «заехал-
  // покрутился-вышел»). Deadlock-safe: Character(shared) закрывается ДО Horse(unique),
  // локи НЕ вложены — прямая копия шаблона creditWin выше. Итерируем GetRacers() (не
  // scores): ключ = characterUid для GetCharacter, и только Racer держит аккумуляторы.
  // Идемпотентно: Stop() зовётся ровно один раз за заезд.
  {
    for (const auto& [characterUid, racer] : _tracker.GetRacers())
    {
      // Отключившихся пропускаем: их Character/Horse-запись может рушиться в teardown,
      // а доехавший-затем-вышедший всё равно отфильтруется по courseTime ниже.
      if (racer.state == State::Disconnected)
        continue;
      if (racer.courseTime == tracker::InvalidCourseTime
        || racer.courseTime < tracker::MinPlausibleCourseTime)
        continue;

      // Поле «divided by 10 for the floating point» → км/ч × 10.
      const uint32_t topSpeedTenths = static_cast<uint32_t>(
        racer.topSpeedKph * 10.0f + 0.5f);
      // Поле «store in metres, displayed in kilometres» → метры.
      const uint32_t distanceMetres = static_cast<uint32_t>(
        racer.distanceMetres + 0.5);
      if (topSpeedTenths == 0 && distanceMetres == 0)
        continue;

      data::Uid mountUid = data::InvalidUid;
      _raceNetworkHandler.GetServerInstance().GetDataDirector().GetCharacter(
        characterUid).Immutable(
        [&mountUid](const data::Character& character)
        {
          mountUid = character.mountUid();
        });
      if (mountUid == data::InvalidUid)
        continue;

      _raceNetworkHandler.GetServerInstance().GetDataDirector().GetHorse(
        mountUid).Mutable(
        [topSpeedTenths, distanceMetres](data::Horse& horse)
        {
          // Рекорд = максимум, пробег = сумма. ★Насыщающее сложение: totalDistance
          // это uint32, обычный += за 2^32 обернулся бы и превратил near-limit пробег
          // в малый (порча данных лошади) — клампим в UINT32_MAX через uint64.
          if (topSpeedTenths > horse.mountInfo.topSpeed())
            horse.mountInfo.topSpeed() = topSpeedTenths;
          const uint64_t newTotalDistance =
            static_cast<uint64_t>(horse.mountInfo.totalDistance()) + distanceMetres;
          horse.mountInfo.totalDistance() = newTotalDistance > 0xFFFFFFFFull
            ? 0xFFFFFFFFu
            : static_cast<uint32_t>(newTotalDistance);
        });
    }
  }

  // === LOA (S8): прогресс ежедневных квестов финишировавших ============
  // Для каждого ДОЕХАВШЕГО гонщика дёргаем QuestSystem::OnQuestEvent — здесь,
  // ВНЕ characterRecord.Mutable (он уже закрыт выше), т.к. OnQuestEvent берёт
  // shared-lock того же character-рекорда, а Record'ный shared_mutex НЕ
  // рекурсивный → вызов внутри Mutable был бы дедлоком. Результаты уже
  // отсортированы; нам нужны лишь uid/команда/courseTime из scores.
  {
    auto& questSystem = _raceNetworkHandler.GetServerInstance().GetQuestSystem();
    const auto questGameMode = QuestSystem::ToGameModeFlag(
      _parameters.gameMode, _parameters.teamMode);

    // ★ОТПРАВКА ПО `ClientId` ИЗ КОМНАТЫ, А НЕ ПО ПОИСКУ В КАРТЕ КЛИЕНТОВ
    // (R70 итерация 2). Прежняя форма звала
    // `SendDailyQuestNotificationToCharacter`, а тот перебирает
    // `RaceNetworkHandler::_clients` — карту, которую мутирует СЕТЕВОЙ поток,
    // тогда как `Stop()` идёт на потоке гоночного директора. Это была гонка на
    // `unordered_map` (UB), и достижения R70 её сперва СКОПИРОВАЛИ; ревью
    // поймало копию, а чинить надо оба места, иначе правка — про место, а не
    // про причину. Снимок берётся под замком комнаты, устаревший `ClientId`
    // безопасен (`SendToClient` глушит бросок «клиента нет»).
    // ПАКЕТ НЕ ИЗМЕНИЛСЯ: у `AcCmdRCUpdateDailyQuestNotify` ровно те семь
    // полей, которые прежний хелпер перекладывал по одному, — теперь
    // отправляется тот же самый объект целиком.
    const auto dailyQuestClientIds = SnapshotRoomClientIds();

    const auto sendNotifies = [this, &dailyQuestClientIds](
      const std::vector<protocol::AcCmdRCUpdateDailyQuestNotify>& notifies)
    {
      for (const auto& notify : notifies)
      {
        const auto clientIdIter = dailyQuestClientIds.find(notify.characterUid);
        if (clientIdIter == dailyQuestClientIds.cend())
          continue;
        _raceNetworkHandler.SendToClient(clientIdIter->second, notify);
      }
    };

    // LOA-fix (F7, quest-batch-1): места 1-3 среди ФАКТИЧЕСКИ доехавших. Индекс
    // в raceResult.scores для этого не годится: сортировка выше ставит первой
    // победившую команду, а не лучшее время. Считаем отдельно по courseTime.
    struct QuestFinisher { uint32_t courseTime; data::Uid uid; };
    std::vector<QuestFinisher> questFinishers;
    for (const auto& scoreInfo : raceResult.scores)
    {
      // LOA-fix (A3, round3): «мгновенный финиш» не может ни занять призовое
      // место, ни вытеснить из тройки честного гонщика.
      if (scoreInfo.courseTime == tracker::InvalidCourseTime
          || scoreInfo.courseTime < tracker::MinPlausibleCourseTime)
        continue;
      questFinishers.push_back(QuestFinisher{scoreInfo.courseTime, scoreInfo.uid});
    }
    std::ranges::sort(questFinishers, [](const QuestFinisher& a, const QuestFinisher& b)
    {
      return a.courseTime < b.courseTime;
    });
    std::vector<data::Uid> prizeWinners;
    for (size_t i = 0; i < questFinishers.size() && i < 3; ++i)
      prizeWinners.push_back(questFinishers[i].uid);

    // Карта, на которой реально проехали (после PickRandomMapFromCourse) —
    // сверяется с Quest::functionValue у RunMap / PrizeWinnerInMap.
    const uint32_t finishedMapBlockId = static_cast<uint32_t>(_mapBlockId);

    for (const auto& scoreInfo : raceResult.scores)
    {
      // Только доехавшие. ⚠️ (N3, round7) Это гейт КВЕСТОВ, а НЕ денег: у
      // начисления наград отдельного гейта больше НЕТ — выплатной античит
      // (A1 / B2) откачен раундом 6, выплата идёт по апстримному правилу.
      // LOA-fix (A3, round3): и только правдоподобные времена — заезд, который
      // не ехали, не должен двигать ни дейлики, ни сюжетные квесты.
      if (scoreInfo.courseTime == tracker::InvalidCourseTime
          || scoreInfo.courseTime < tracker::MinPlausibleCourseTime)
        continue;

      // LOA-fix (F8, quest-batch-1): «доехал заезд» уходит ТОЛЬКО дейликам-
      // счётчикам заездов 1002 (12 заездов) и 1013 (25). Раньше фильтра не было,
      // а QuestEvent::Any матчится с ЛЮБЫМ function TRUE, причём gameModeFlag 0
      // трактуется IsModeMatch как «без ограничения» → один заезд двигал ещё и
      // «покорми лошадь» (1003/1014/1022), «помой лошадь» (1004/1015/1023) и
      // «покажи рывок» (1006/1017). Эти классы теперь двигаются своими каналами:
      // кормление/мытьё — в RanchDirector, рывок — в RaceNetworkHandler.
      sendNotifies(questSystem.OnQuestEvent(
        scoreInfo.uid, QuestSystem::QuestEvent::Any, questGameMode, 0,
        {1002u, 1013u}));

      // Победа в командном заезде — двигает TeamWin-дейлики.
      if (_parameters.teamMode == protocol::TeamMode::Team
        && scoreInfo.teamColor == winningTeam)
      {
        sendNotifies(questSystem.OnQuestEvent(
          scoreInfo.uid, QuestSystem::QuestEvent::TeamWin, questGameMode));
      }

      // --- LOA-fix (F7): призовое место (топ-3) ---------------------------
      bool isPrizeWinner = false;
      for (const data::Uid winnerUid : prizeWinners)
        if (winnerUid == scoreInfo.uid) { isPrizeWinner = true; break; }

      if (isPrizeWinner)
      {
        // Дейлики 1000/1001/1011/1012 (PrizeWinnerForLowLevel).
        // LOA-fix (R5, round2): им нужен ОТДЕЛЬНЫЙ режим-флаг. В quests.yaml они
        // объявлены как 33 (WinSpeedSolo) / 68 (WinMagicSolo), а questGameMode
        // (ToGameModeFlag) даёт для соло 35/76 → IsModeMatch(33, 35) == false, и
        // раунд 1 диспатчил в пустоту. ToPrizeGameModeFlag возвращает 33/68.
        sendNotifies(questSystem.OnQuestEvent(
          scoreInfo.uid, QuestSystem::QuestEvent::PrizeWinner,
          QuestSystem::ToPrizeGameModeFlag(_parameters.gameMode, _parameters.teamMode)));

        // MAIN-квесты лежат в character.quests(), а НЕ в трёх дейлик-слотах,
        // поэтому OnQuestEvent их физически не видит — двигаем отдельно.
        // 12012/12018/14015 — призовое место на любой карте;
        // 14016 (карта 40) / 14027 (карта 25) — только на своей.
        questSystem.AdvanceMainQuests(
          scoreInfo.uid, {12012u, 12018u, 14015u});
        questSystem.AdvanceMainQuests(
          scoreInfo.uid, {14016u, 14027u}, true, finishedMapBlockId);
      }

      // --- LOA-fix (F7): «пройди карту N раз» (RunMap) ---------------------
      // Матчер QuestSystem::IsEventMatch для RunMap был написан и ждал события,
      // которое не слал никто. Дейликов этого класса в quests.yaml нет ни одного,
      // поэтому OnQuestEvent здесь — задел на будущее, а реальную работу делает
      // проход по MAIN-квестам: 11037 (карта 19), 11041 (16), 13013 (19),
      // 13015 (25), 14013 (1), 14017 (41).
      sendNotifies(questSystem.OnQuestEvent(
        scoreInfo.uid, QuestSystem::QuestEvent::RunMap, questGameMode,
        finishedMapBlockId));
      questSystem.AdvanceMainQuests(
        scoreInfo.uid,
        {11037u, 11041u, 13013u, 13015u, 14013u, 14017u},
        true, finishedMapBlockId);
    }
  }

  // === LOA-fix (R-revenge, #13): БОНУС «НЕПЛОХАЯ МЕСТЬ» =======================
  // Заполняем ScoreInfo::bonusCarrots и ScoreInfo::member22 (апстримный
  // комментарий у поля — «! Revenge something»), которые сервер не заполнял
  // никогда, поэтому бонус всегда показывался нулём.
  // МЕСТО: последняя точка перед отправкой пакета, ПОСЛЕ сортировки scores и
  // ПОСЛЕ закрытия всех per-racer characterRecord.Mutable выше → локи не вложены.
  // Идемпотентно: Stop() зовётся ровно один раз за заезд.
  // БАЗУ НЕ ТРОГАЕМ: score.carrots / score.experience остаются 2500 / 420
  // (байт-паритет базовой награды — гейт приёмки).
  {
    if (_parameters.teamMode == protocol::TeamMode::Team)
    {
      auto& questSystem = _raceNetworkHandler.GetServerInstance().GetQuestSystem();
      auto& revengeRacers = _tracker.GetRacers();

      // Тиры под 3 клиентских порога шкалы (Revenge_SuccessMsg_Lv1..3).
      static constexpr uint32_t RevengeCarrotTiers[] = {500u, 750u, 1000u};
      static constexpr uint32_t RevengeClassExpTiers[] = {100u, 150u, 200u};
      // Максимальный класс лошади — за ним ApplyClassProgress уже no-op.
      static constexpr uint32_t MaxHorseClass = 30;

      for (auto& score : raceResult.scores)
      {
        // G6: финишный гейт по СЕРВЕРНОМУ замеру — тот же, что у квестов и
        // статистики лошади. «Мгновенный финиш» бонуса не получает.
        if (score.courseTime == tracker::InvalidCourseTime
          || score.courseTime < tracker::MinPlausibleCourseTime)
          continue;

        const auto revengeRacerIt = revengeRacers.find(
          static_cast<data::Uid>(score.uid));
        if (revengeRacerIt == revengeRacers.end())
          continue;

        const uint32_t credits =
          revengeRacerIt->second.revengeCredits > tracker::RevengeMaxCredits
            ? tracker::RevengeMaxCredits
            : revengeRacerIt->second.revengeCredits;
        if (credits == 0)
          continue;

        const uint32_t bonusCarrots = RevengeCarrotTiers[credits - 1];
        const uint32_t requestedExp = RevengeClassExpTiers[credits - 1];

        // --- КЛАСС-ОПЫТ: инвариант «показано == начислено» -----------------
        // Порядок обязателен: (1) маунт есть и класс не максимальный,
        // (2) только тогда тратим ОБЩИЙ суточный бюджет (тот же счётчик, что у
        // дейликов — иначе два канала независимо печатают по классу в день),
        // (3) применяем РОВНО выданное, (4) показываем РОВНО выданное.
        // Записи берём ПОСЛЕДОВАТЕЛЬНО (Character закрыт до Horse) — Record'ный
        // shared_mutex не рекурсивный, вложение = дедлок.
        data::Uid revengeMountUid = data::InvalidUid;
        _raceNetworkHandler.GetServerInstance().GetDataDirector().GetCharacter(
          score.uid).Immutable(
          [&revengeMountUid](const data::Character& character)
          {
            revengeMountUid = character.mountUid();
          });

        bool mountCanGrow = false;
        if (revengeMountUid != data::InvalidUid)
        {
          const auto revengeHorseRecord = _raceNetworkHandler.GetServerInstance()
            .GetDataDirector().GetHorse(revengeMountUid);
          if (revengeHorseRecord)
          {
            revengeHorseRecord.Immutable(
              [&mountCanGrow](const data::Horse& horse)
              {
                mountCanGrow = horse.clazz() < MaxHorseClass;
              });
          }
        }

        uint32_t grantedExp = 0;
        uint32_t appliedExp = 0;
        if (mountCanGrow)
        {
          grantedExp = questSystem.ClaimDailyClassExp(score.uid, requestedExp);
          if (grantedExp > 0)
          {
            // F3 (Codex-BLOCK TOCTOU): между проверкой mountCanGrow (отдельный
            // Immutable выше) и этим Mutable лошадь могла дойти до MaxHorseClass —
            // тогда ApplyClassProgress применяет НОЛЬ (ранний return на классе 30).
            // Меряем РЕАЛЬНО применённое по clazzProgress (пожизненный тотал, растёт
            // на gainedExp только когда применяется) → member22 покажет ровно
            // начисленное. clazzProgress НЕ убывает, поэтому after-before корректен.
            _raceNetworkHandler.GetServerInstance().GetDataDirector().GetHorse(
              revengeMountUid).Mutable(
              [this, grantedExp, &appliedExp](data::Horse& horse)
              {
                const uint32_t beforeExp = horse.clazzProgress();
                _raceNetworkHandler.GetServerInstance().GetHorseRegistry()
                  .ApplyClassProgress(horse, grantedExp);
                appliedExp = horse.clazzProgress() - beforeExp;
              });
            // F3 iter3 (Codex-BLOCK iter2 ABA): НАМЕРЕННО без рефанда. Рефанд
            // «grantedExp - appliedExp» создавал бы ABA через границу дня: если
            // между этим claim'ом и рефандом проходил суточный сброс
            // (SyncDailyClassExpBudget в QuestSystem обнуляет dailyClassExpGranted), рефанд занижал бы
            // счётчик УЖЕ НОВОГО дня → второй полный кап 6650 в те же сутки.
            // shown==credited держится и без рефанда (member22 = appliedExp ниже).
            // Единственный эффект отказа от рефанда: если лошадь достигла класса 30
            // РОВНО в микросекундном TOCTOU-окне между проверкой mountCanGrow и этим
            // Mutable, бюджет спишется без применения — кап станет КОНСЕРВАТИВНЫМ
            // (tighter, не looser: leak-free). Персонаж участвует в ОДНОМ заезде
            // за раз, конкурентных наград на одну лошадь нет → этот угол практически
            // не срабатывает; в любом реальном сценарии applied==granted и кап точен.
          }
        }

        // --- МОРКОВКИ -------------------------------------------------------
        // Базовые 2500 начислены выше в основном цикле; здесь ТОЛЬКО бонус.
        // F4 (Codex-BLOCK, класс R40-1): carrots — int32_t, `+= bonus` даёт знаковое
        // переполнение (UB) у INT32_MAX. Считаем в int64 и клампим свободным местом
        // до потолка 2e9; legacy-баланс вне [0,2e9] не трогаем. Показываем РОВНО
        // столько, сколько реально начислено (инвариант «показано==начислено»).
        int32_t creditedCarrots = 0;
        _raceNetworkHandler.GetServerInstance().GetDataDirector().GetCharacter(
          score.uid).Mutable(
          [bonusCarrots, &creditedCarrots](data::Character& character)
          {
            const int64_t current = static_cast<int64_t>(character.carrots());
            if (current >= 0 && current <= 2000000000)
            {
              const int64_t grant = std::min<int64_t>(
                static_cast<int64_t>(bonusCarrots), 2000000000 - current);
              character.carrots() = static_cast<int32_t>(current + grant);
              creditedCarrots = static_cast<int32_t>(grant);
            }
          });

        score.bonusCarrots = static_cast<uint32_t>(creditedCarrots);
        score.member22 = appliedExp;

        // Кап на морковки не вводим (серверная месть — подмножество командных
        // заездов, максимум +40 % к базе). Взамен — журнал на КАЖДУЮ выплату и
        // неделя наблюдения за экономикой.
        server::util::QuietLogInfo(
          "Revenge bonus: character {} revenged {} rival(s) -> +{} carrots, "
          "+{} class exp (requested {})",
          score.uid,
          credits,
          creditedCarrots,
          appliedExp,
          requestedExp);
      }
    }
  }

  // === R56 (#61): боты попадают ТОЛЬКО в уходящую копию ===================
  // Боты обязаны быть видны на табло, но не имеют права попасть в
  // `raceResult`: по этому же вектору НИЖЕ назначается новый мастер комнаты
  // (`raceResult.scores[0].uid`), а ВЫШЕ по функции — начисление побед лошади
  // `creditWin(scores[0].uid)` и цикл дейликов S8b. Строка бота, у которого
  // персонажа не существует, увела бы мастера в никуда, а
  // `GetCharacter(...).Immutable` бросил бы прямо посреди подведения итогов —
  // и живой игрок остался бы без результата заезда.
  //
  // ★Поэтому «показать» и «посчитать» разведены физически: считаем по
  // `raceResult`, показываем копию. Ни одна существующая и ни одна будущая
  // выборка по `scores` бота не увидит — не потому, что помнит про него, а
  // потому, что его там нет.
  if (_aiRacers.empty())
  {
    // Broadcast the race result
    _raceNetworkHandler.Broadcast(*this, raceResult);
  }
  else
  {
    auto broadcastResult = raceResult;

    for (const auto& aiRacer : _aiRacers)
    {
      auto& score = broadcastResult.scores.emplace_back();
      // Персонажа нет — и это записано явно, а не спрятано за магическим
      // диапазоном uid, как у апстрима.
      score.uid = data::InvalidUid;
      score.name = aiRacer.name;
      score.courseTime = aiRacer.courseTime;
      score.level = 1;
      score.teamColor = protocol::TeamColor::None;
      score.bitset = protocol::AcCmdRCRaceResultNotify::ScoreInfo::Bitset::Connected;
    }

    // Боты бывают только в соло-заезде, где команд нет, поэтому порядок задают
    // ровно два признака — «доехал» и время. Это та же ключевая функция, что и
    // в сортировке выше, за вычетом командной части.
    std::ranges::sort(broadcastResult.scores, [](const auto& a, const auto& b)
    {
      const auto key = [](const auto& score)
      {
        return std::make_pair(
          score.courseTime < tracker::InvalidCourseTime ? 0 : 1,
          score.courseTime);
      };
      return key(a) < key(b);
    });

    _raceNetworkHandler.Broadcast(*this, broadcastResult);
  }

  // Assign room master to the first-place finisher.
  if (not raceResult.scores.empty())
  {
    data::Uid newMasterUid = raceResult.scores[0].uid;
    std::string newMasterName = raceResult.scores[0].name;
    this->GetRoom(
      [&newMasterUid, &newMasterName, scores = raceResult.scores](Room& room)
      {
        // Check if room even has players
        if (room.GetPlayerCount() < 1)
        {
          // TODO: mark room for delete
          newMasterUid = data::InvalidUid;
          return;
        }

        // Get room details to update
        auto& details = room.GetRoomDetails();

        // Check if room has this player
        if (room.HasPlayer(newMasterUid))
        {
          // New master exists in room
          details.masterUid = newMasterUid;
          return;
        }

        // New master left, proceed with the scores list
        // and assign until none found
        for (const auto& score : scores)
        {
          // Check that this next best player is in room
          if (not room.HasPlayer(score.uid))
            continue;

          // Character is in room, set room master to new uid
          newMasterUid = details.masterUid = score.uid;
          newMasterName = score.name;
          return;
        }

        // No characters available for room master (how?)
        newMasterUid = data::InvalidUid;
      });

    if (newMasterUid != data::InvalidUid)
    {
      const auto userName = _raceNetworkHandler.GetServerInstance().GetLobbyDirector().GetUserByCharacterUid(
        newMasterUid).userName;
      server::util::QuietLogInfo("Player {} ({}) has won the match and is now master of [Room {}]",
        userName,
        newMasterName,
        this->GetRoomUid());

      const protocol::AcCmdCRChangeMasterNotify masterNotify{
        .masterUid = newMasterUid};
      _raceNetworkHandler.Broadcast(*this, masterNotify);
    }
  }

  // Clear the ready state of all of the players and update their balances.
  this->GetRoom(
    [this](Room& room)
    {
      room.SetRoomPlaying(false);
      for (auto& [uid, player] : room.GetPlayers())
      {
        // Set racer's ready state to false
        room.GetPlayer(uid).SetReady(false);

        // Update this racer's carrot balance
        protocol::AcCmdRCUpdateGameMoney updateGameMoney{};
        _raceNetworkHandler.GetServerInstance().GetDataDirector().GetCharacter(uid).Immutable(
          [&updateGameMoney](const data::Character& character)
          {
            updateGameMoney.carrotBalance = character.carrots();
          });

        _raceNetworkHandler.GetCommandServer().QueueCommand<protocol::AcCmdRCUpdateGameMoney>(
          player.GetClientId(),
          [updateGameMoney]()
          {
            return updateGameMoney;
          });
      }
    });

  // === LOA (R70, backlog #58): ДОСТИЖЕНИЯ СОБЫТИЯ 2 (финиш заезда) =========
  //
  // МЕСТО ВЫБРАНО ДВУМЯ ПРИЧИНАМИ, И ОБЕ ОБЯЗАТЕЛЬНЫ.
  // (1) ЛОКИ: `AchievementSystem::OnServerEvent` берёт `Mutable` того же
  //     character-рекорда, а `Record` использует НЕрекурсивный `shared_mutex`.
  //     Значит врезка обязана стоять ВНЕ любого `characterRecord.Mutable`;
  //     здесь закрыты все (последний — лямбда `GetRoom` прямо выше).
  // (2) ПОВТОРНЫЙ `Stop()`: `RaceInstance::Tick` глотает исключения,
  //     а `_stage = Stage::Waiting` стоит ПОСЛЕ `Stop()` в `TickFinishing` —
  //     бросок в середине `Stop()` заставляет `TickFinishing` позвать её ещё раз.
  //     Блок, стоящий ПОСЛЕДНИМ, при таком повторе исполнится РОВНО ОДИН раз:
  //     при первом заходе до него не дошли, при втором он отработает впервые.
  //     ★ЭТО УТВЕРЖДЕНИЕ ДЕРЖИТСЯ НА ДВУХ ФАКТАХ, И ОБА ПРОВЕРЯЕМЫ:
  //       (а) блок сам не может бросить — весь он обёрнут в `catch (...)` ниже;
  //       (б) после блока в `Stop()` НЕТ НИ ОДНОГО оператора — это машинно
  //           проверяет гейт раунда `tools/round/check_stop_tail.py`.
  //     ЛЮБОЙ будущий раунд, дописывающий что-то в конец `Stop()` (в очереди
  //     такие есть: R75 и R76 планируют врезки в `Stop()`, R76 — явно «самым
  //     последним шагом»), обязан либо встать ПЕРЕД этим блоком, либо доказать,
  //     что его код не бросает, либо завести здесь эпоху-гард по `_raceEpoch`.
  //     Гейт упадёт и заставит принять решение явно.
  try
  {
    auto& racers = _tracker.GetRacers();

    using FinishOutcome = tracker::RaceTracker::Racer::FinishOutcome;

    // --- режим -------------------------------------------------------------
    // Та же величина, что у квестов: маски достижений и квестов приходят из
    // одной клиентской таблицы. Второе отображение разошлось бы молча.
    // Обучение и всё вне четвёрки дают 0 → ни одна запись с ненулевой маской
    // не засчитается (инвариант I10).
    const uint32_t modeBit = static_cast<uint32_t>(
      QuestSystem::ToGameModeFlag(_parameters.gameMode, _parameters.teamMode));

    const uint32_t localHour = server::util::CurrentGameLocalHour();
    const std::string_view hourWindow = GoalInHourWindowCondition(localHour);
    const std::string_view mapName = _mapBlockInfo.name;

    // --- состав, финишеры, места и сходы ------------------------------------
    // ★СЧИТАЕМ ЗАНОВО, А НЕ ПО `raceResult.scores`, и это ПРЕЦЕДЕНТ, а не
    // самодеятельность: тот же приём и по той же причине стоит у дейликов
    // выше. Сортировка табло ставит первой ПОБЕДИВШУЮ КОМАНДУ, а не
    // лучшее время, и «мгновенный финиш» с табло не отсеян.
    // ★ПОРЯДОК СТРОГИЙ: ключ (courseTime, characterUid). При равных временах
    // «1 + число строго меньших» дало бы ДВА первых места; лексикографический
    // ключ даёт ровно одно.
    struct Finisher { uint32_t courseTime; data::Uid uid; Team team; };
    std::vector<Finisher> finishers;
    uint32_t retireCount = 0;
    uint32_t humanCount = 0;

    const auto isPlausibleFinish = [](const tracker::RaceTracker::Racer& racer)
    {
      return racer.courseTime != tracker::InvalidCourseTime
        and racer.courseTime >= tracker::MinPlausibleCourseTime;
    };

    // ★ИСХОД, ДОКАЗАННЫЙ СЕРВЕРОМ: гонщик либо доехал за правдоподобное время,
    // либо СОШЁЛ с заезда, который к тому моменту реально шёл (проверка стоит в
    // `HandleUserRaceFinal`). Всё остальное — молчание или отвергнутая попытка.
    const auto hasProvenOutcome = [&isPlausibleFinish](
      const tracker::RaceTracker::Racer& racer)
    {
      return isPlausibleFinish(racer)
        or racer.finishOutcome == FinishOutcome::Retired;
    };

    // ★СОСТАВ СЧИТАЕТСЯ ПО ТЕМ, КТО РЕАЛЬНО ЕХАЛ, и обе половины условия
    // обязательны.
    // (1) ПОДКЛЮЧЁН. Ростер (`GetRacers().size()`) стабилен от старта до
    //     `Stop()` — `RaceTracker::RemoveRacer` не вызывается нигде, а обрыв
    //     лишь ставит `state = Disconnected`. По ростеру «человек» равен
    //     «аккаунт когда-то вошёл», и состав покупается вошедшими и вышедшими.
    // (2) ЕХАЛ. Одной подключённости мало — это находка ревью (итерация 2):
    //     подключённый МОЛЧУН ростер покупает ровно так же, ему не нужно даже
    //     отключаться. Три (семь) молчащих альтов дают `numPlayer` 4 (8) у
    //     записей `Win`/`TeamWin` (10225, 10226, 10054, 10227, 10228, 10060) —
    //     а это ПОВТОРЯЕМЫЕ тиры. Поэтому спрашиваем СЕРВЕРНУЮ улику движения:
    //     `trustedProgress >= MinMeaningfulRaceProgress` (см. константу) либо
    //     правдоподобный финиш, который сам по себе означает 30+ секунд заезда.
    //     Второе слагаемое — страховка: если бы клиент по какой-то причине не
    //     слал `progress`, доехавшие всё равно остались бы составом, и ранговые
    //     условия не выключились бы у честной игры целиком.
    // ЦЕНА УЖЕСТОЧЕНИЯ, ЗАПИСАННАЯ ЧЕСТНО: честный игрок, чей единственный
    // соперник вылетел или простоял заезд, за ЭТОТ заезд ранговых условий не
    // получит (10003 `MyFirstWin`, разовая, 1 очко) — получит за следующий. Это
    // дешевле повторяемой чеканки `Win`.
    const auto countsInRoster = [&isPlausibleFinish](
      const tracker::RaceTracker::Racer& racer)
    {
      return racer.state != State::Disconnected
        and (racer.trustedProgress >= MinMeaningfulRaceProgress
          or isPlausibleFinish(racer));
    };

    for (const auto& [characterUid, racer] : racers)
    {
      if (countsInRoster(racer))
        ++humanCount;

      // ★ФИНИШЁРЫ И СХОДЫ СЧИТАЮТСЯ У ВСЕХ, ВКЛЮЧАЯ ОТКЛЮЧИВШИХСЯ, и это
      // ВТОРАЯ находка ревью (итерация 2). Отключение — это не отмена уже
      // доказанного исхода: гонщик доехал (сервер сам замерил время) либо сошёл
      // (сервер сам подтвердил), а `HandleLeaveRoom` лишь переписал `state`.
      // Пропуск таких записей стоил бы дважды:
      //   * честный ПОБЕДИТЕЛЬ, закрывший игру сразу после финиша, исчезал бы
      //     из `finishers` — и `Win`/`MyFirstWin` уходили бы ВТОРОМУ месту,
      //     то есть тому, кто не выигрывал. Это чеканка, а не потеря;
      //   * честный сход не считался бы сходом.
      // Состав (выше) — другое дело: там вопрос «сколько людей ехало», и
      // отключившийся на него отвечает «уже нисколько».
      if (isPlausibleFinish(racer))
      {
        finishers.push_back({racer.courseTime, characterUid, racer.team});
      }
      // ★СХОД ДОКАЗЫВАЕТСЯ ПОЛОЖИТЕЛЬНО. «Нет правдоподобного времени» — это
      // ТРИ разных события (см. `Racer::FinishOutcome`), и два из них сходом не
      // являются: отвергнутый сервером исход и «не прислал ничего». Считать
      // сходом только `Retired` обязательно и здесь, а не только у условия
      // `Retire`: `retireCount` кормит `PerfectWin` (10008), и без этого альт,
      // простоявший заезд или пытавшийся мгновенно финишировать, ПЕЧАТАЛ БЫ
      // «чистую победу» напарнику.
      else if (racer.finishOutcome == FinishOutcome::Retired)
      {
        ++retireCount;
      }
    }

    std::ranges::sort(finishers, [](const Finisher& a, const Finisher& b)
    {
      return std::tie(a.courseTime, a.uid) < std::tie(b.courseTime, b.uid);
    });

    // ★СВОЙ победитель команды, а не `winningTeam` табло. Табло считает первым
    // финишировавшего с ЛЮБЫМ временем — то есть «мгновенный финиш»
    // второго аккаунта короновал бы команду. Табло мы не трогаем (байт-паритет
    // рассылки — гейт приёмки), а награду считаем по СЕРВЕРНОМУ замеру.
    const Team achievementWinningTeam =
      (_parameters.teamMode == protocol::TeamMode::Team and not finishers.empty())
        ? finishers.front().team
        : Team::Solo;

    // --- пер-гонщик ---------------------------------------------------------
    // ★СНИМОК «ПЕРСОНАЖ → КЛИЕНТ» СНИМАЕТСЯ ОДИН РАЗ И БЕРЁТСЯ ИЗ КОМНАТЫ.
    // `Stop()` идёт на потоке гоночного директора, а карту клиентов
    // (`RaceNetworkHandler::_clients`) мутирует СЕТЕВОЙ поток — искать в ней
    // отсюда значит гонять `unordered_map` под параллельными
    // `try_emplace`/`erase`. `RoomSystem::GetRoom` держит замок комнаты на всё
    // время колбэка, и ровно этим путём ходит `Broadcast` (см.
    // `RaceNetworkHandler::SendToClient`).
    const auto clientIds = SnapshotRoomClientIds();

    for (const auto& [characterUid, racer] : racers)
    {
      // ★ОБРАБАТЫВАЕМ ПОДКЛЮЧЁННЫХ И ТЕХ, ЧЕЙ ИСХОД УЖЕ ДОКАЗАН. Честный
      // финишёр (или сошедший), закрывший игру до `Stop()`, теряет только
      // УВЕДОМЛЕНИЕ — прогресс обязан быть записан, он его заработал. Из
      // СОСТАВА (`humanCount`) отключившиеся при этом по-прежнему исключены.
      if (racer.state == State::Disconnected and not hasProvenOutcome(racer))
        continue;

      std::vector<std::string_view> conditions;
      conditions.reserve(8);

      if (not isPlausibleFinish(racer))
      {
        // Retire: `TimeRecord == 0` (ach_conditions.lua:1346-1350) — но только
        // для ДОКАЗАННОГО схода. Отвергнутый античитом финиш и молчание сходом
        // не считаются: см. `Racer::FinishOutcome` и подсчёт `retireCount` выше.
        if (racer.finishOutcome == FinishOutcome::Retired)
          conditions.emplace_back("Retire");
      }
      else
      {
        // GoalIn: `TimeRecord > 0` (:451-455). Сегодня потребителей БЕЗ
        // reset-гарда у него нет — все десять записей `GoalIn` события 2
        // сбрасываемые, и система их не двигает. Условие всё равно называется:
        // оно верно по смыслу, оно же — предмет негатива negD, и когда сброс
        // будет реализован, проводка уже стоит.
        conditions.emplace_back("GoalIn");

        if (not hourWindow.empty())
          conditions.emplace_back(hourWindow);

        for (const auto& mastery : MasteryCourses)
        {
          if (mapName == mastery.mapName
            and racer.courseTime <= mastery.courseTimeLimitMs)
            conditions.emplace_back(mastery.condition);
        }

        // ★ГАРД ЭКСПЛОЙТА. В соло-заезде табло состоит из ОДНОЙ строки, поэтому
        // место равно единице ВСЕГДА — и с семью ботами, и без них; ехать для
        // этого не нужно вовсе. Поэтому гард звучит не «не верить ботам», а
        // «условия, опирающиеся на МЕСТО, не выдаются, пока людей меньше двух».
        if (humanCount >= 2)
        {
          const bool isFirst =
            not finishers.empty() and finishers.front().uid == characterUid;

          if (isFirst)
          {
            // Win и MyFirstWin — оба `Rank == 1` (:1047-1051, :56-58).
            //
            // ★ЯВНАЯ ОБЛАСТЬ ДЕЙСТВИЯ `Win`/`TeamWin` — ЧИТАТЬ ДО ПРАВОК.
            // Эти два условия — ЕДИНСТВЕННЫЕ в блоке, которые зажигают записи с
            // ненулевым `numPlayer`, и потому единственные, чей набор записей
            // зависит от СОСТАВА заезда:
            //   `Win`     -> 10225 (numPlayer 4), 10226 (4), 10054 (8);
            //   `TeamWin` -> 10227 (4), 10228 (4), 10060 (8).
            // Все шесть — повторяемые, тиры [1,10,50,100] / [1,10,50,300] /
            // [1,2,3,4], суммарно 24 очка. Пока в заезде меньше ЧЕТЫРЁХ
            // ПОДКЛЮЧЁННЫХ людей, `AchievementInfo::CountsInMode` не пропускает
            // ни одну из них, и раунд зажигает ровно 17 записей / 53 очка —
            // это и есть заявленная приёмка. Граница «17 против 23» не на
            // словах: её проверяет `tests/src/registry/TestAchievementTiers.cpp`
            // (`TestCatalogShapesOfRankConditions`) прямо на формах каталога.
            // Отсюда два следствия для будущих правок: (а) `humanCount` обязан
            // считать ПОДКЛЮЧЁННЫХ, иначе состав покупается альтами; (б) любое
            // новое условие с ненулевым `numPlayer` обязано попасть в тот тест.
            conditions.emplace_back("Win");
            conditions.emplace_back("MyFirstWin");
            // PerfectWin — `Rank == 1 && RetireCount > 0` (:72-80).
            // ★РАДИУС ГАРДА: при ОДНОМ человеке PerfectWin недостижим и БЕЗ
            // этого гарда — единственный гонщик либо финишировал (тогда
            // retireCount == 0), либо не финишировал (тогда finishers пуст и
            // isFirst ложно). Гард здесь ради Win/MyFirstWin; предикат стенда
            // это отражает (§4.3, P_rank_guard судит только 10003).
            if (retireCount > 0)
              conditions.emplace_back("PerfectWin");
          }

          // TeamWin — `TeamRank == 1` (:1056-1060).
          if (_parameters.teamMode == protocol::TeamMode::Team
            and racer.team == achievementWinningTeam)
            conditions.emplace_back("TeamWin");
        }
      }

      // Revenge — `RevengeSuccess > 0` (:932-937). Не зависит от финиша:
      // отомстить можно и сойдя после этого с дистанции. `revengeCredits`
      // набирается ТОЛЬКО в командном заезде (гард G7 в `RaceNetworkHandler`),
      // поэтому наблюдается ровно в командной арке A10.
      if (racer.revengeCredits > 0)
        conditions.emplace_back("Revenge");

      // ★ПЕРЕХВАТ ВОКРУГ КАЖДОГО ГОНЩИКА, а не только вокруг всего блока: сбой
      // у одного не имеет права стоить достижений остальным. И значок не имеет
      // права стоить игроку результата заезда (R48-11).
      try
      {
        const auto notifies = _raceNetworkHandler.GetServerInstance()
          .GetAchievementSystem().OnServerEvent(
            characterUid,
            RaceAchievementEvent,
            1,
            conditions,
            AchievementSystem::EventContext{
              .modeBit = modeBit, .playerCount = humanCount});

        const auto clientIdIter = clientIds.find(characterUid);
        if (clientIdIter != clientIds.cend())
        {
          for (const auto& notify : notifies)
            _raceNetworkHandler.SendToClient(clientIdIter->second, notify);
        }
      }
      catch (const std::exception& x)
      {
        server::util::QuietLogError(
          "Race achievements for character {} were not processed: {}",
          characterUid, x.what());
      }
      catch (...)
      {
        server::util::QuietLogError(
          "Race achievements for character {} were not processed: {}",
          characterUid, "unknown exception");
      }
    }
  }
  // ★ВНЕШНИЙ ПЕРЕХВАТ — НЕ ДУБЛЬ ВНУТРЕННЕГО. Пролог блока стоит ВНЕ
  // пер-гонщикового `try`: чтение трекера, `std::vector<Finisher>` (push_back →
  // bad_alloc), `std::ranges::sort`, `std::vector<std::string_view>` с reserve(8).
  // Бросок оттуда вылетел бы из `Stop()`, стадия осталась бы `Finishing`,
  // `TickFinishing` позвала бы `Stop()` заново — и ПОВТОРИЛОСЬ БЫ ВСЁ, включая
  // выплату 2500 морковок. Цена этого перехвата — ноль, а без него
  // утверждение «исполнится ровно один раз» почти истинно, то есть неверно.
  catch (const std::exception& x)
  {
    server::util::QuietLogError(
      "Race achievements block failed for room {}: {}", GetRoomUid(), x.what());
  }
  catch (...)
  {
    server::util::QuietLogError(
      "Race achievements block failed for room {}: {}", GetRoomUid(),
      "unknown exception");
  }
}

void RaceInstance::Tick()
{
  try
  {
    // Stage tick
    switch (_stage)
    {
      case Stage::Waiting:
        // Do nothing on waiting stage
        break;
      case Stage::Loading:
        // Process rooms which are loading
        this->TickLoading();
        break;
      case Stage::Racing:
        // Process rooms which are racing
        this->TickRacing();
        break;
      case Stage::Finishing:
        // Process rooms which are finishing
        this->TickFinishing();
        break;
    }
  }
  catch (const std::exception& x)
  {
    server::util::QuietLogError("Exception ticking race instance {} stage: {}", GetRoomUid(), x.what());
  }

  try
  {
    // Post-stage tick
    switch (_stage)
    {
      case Stage::Waiting:
        break;
      case Stage::Loading:
        break;
      case Stage::Racing:
        this->TickActiveRaceContent();
        break;
      case Stage::Finishing:
        this->TickActiveRaceContent();
        break;
    }
  }
  catch (const std::exception& x)
  {
    server::util::QuietLogError("Exception ticking race instance {} post-stage: {}", GetRoomUid(), x.what());
  }
}

uint32_t RaceInstance::GetRoomUid()
{
  return _roomUid;
}

const RaceInstance::Parameters& RaceInstance::GetParameters() const
{
  return _parameters;
}

registry::GameModeId RaceInstance::GetGameModeId() const
{
  return _gameModeId;
}

registry::MapBlockId RaceInstance::GetMapBlockId() const
{
  return _mapBlockId;
}

std::chrono::steady_clock::time_point RaceInstance::GetLoadingStartTimePoint() const noexcept
{
  return _loadingStartTimePoint;
}

std::chrono::steady_clock::time_point RaceInstance::GetRaceStartTimePoint() const noexcept
{
  return _raceStartTimePoint;
}

RaceInstance::Stage RaceInstance::GetStage() const noexcept
{
  return _stage;
}

// LOA-fix (R67-4, backlog #128b): читатель номера заезда. Подпись
// `const noexcept` скопирована с соседнего `GetStage` не для красоты: зовут
// его отложенные джобы через `const RaceInstance&`, взятую из
// `_raceInstances` под `_raceInstancesMutex`, и гард, который умеет бросить,
// был бы гардом, умеющим не сработать.
uint32_t RaceInstance::GetRaceEpoch() const noexcept
{
  return _raceEpoch;
}

std::chrono::steady_clock::time_point RaceInstance::GetStageTimeoutTimePoint() const noexcept
{
  return _stageTimeoutTimePoint;
}

tracker::RaceTracker& RaceInstance::GetTracker()
{
  return _tracker;
}

const tracker::RaceTracker& RaceInstance::GetTracker() const
{
  return _tracker;
}

std::vector<RaceInstance::AiRacer>& RaceInstance::GetAiRacers() noexcept
{
  return _aiRacers;
}

const std::vector<RaceInstance::AiRacer>& RaceInstance::GetAiRacers() const noexcept
{
  return _aiRacers;
}

std::vector<tracker::Oid>& RaceInstance::GetAiOids() noexcept
{
  return _aiOids;
}

bool RaceInstance::IsAiRacerOid(const tracker::Oid oid) const noexcept
{
  // Пустой идентификатор — не бот. Иначе запись ростера, не получившая oid,
  // однажды превратилась бы в разрешение для всякого нулевого поля в пакете.
  if (oid == tracker::InvalidEntityOid)
    return false;

  // Ростер — не больше семи элементов, поиск линейный и без выделений памяти,
  // поэтому пометка `noexcept` здесь честная.
  for (const auto& aiRacer : _aiRacers)
  {
    if (aiRacer.oid == oid)
      return true;
  }

  return false;
}

void RaceInstance::NoteAiRacerProgress(
  const tracker::Oid oid,
  const float progress) noexcept
{
  // Тот же первый вопрос, что и у `IsAiRacerOid`: пустой идентификатор — не
  // бот, иначе всякое нулевое поле в пакете начало бы двигать ростер.
  if (oid == tracker::InvalidEntityOid)
    return;

  // Санитизация ДО поиска и в положительной форме: `not (x > 0)` истинно и для
  // NaN, и для отрицательного, и для нуля — то есть ни одно из этих значений в
  // храповик не попадёт. Верхняя граница — по контракту поля
  // (`Racer::raceProgress`: «нормирован клиентом в 0..1»).
  if (not (progress > 0.0f))
    return;

  const float sane = progress > 1.0f ? 1.0f : progress;

  // Ростер — не больше семи элементов, поиск линейный и без выделений памяти,
  // поэтому `noexcept` здесь честный (тот же довод, что у `IsAiRacerOid`).
  for (auto& aiRacer : _aiRacers)
  {
    if (aiRacer.oid != oid)
      continue;

    if (sane > aiRacer.raceProgress)
      aiRacer.raceProgress = sane;

    return;
  }
}

protocol::BonusCourseType RaceInstance::GetBonusCourseType() const noexcept
{
  return _bonusCourseType;
}

void RaceInstance::SetBonusCourseType(const protocol::BonusCourseType type) noexcept
{
  _bonusCourseType = type;
}

void RaceInstance::TickLoading()
{
  // Determine whether all racers have started racing.
  const bool allRacersLoaded = std::ranges::all_of(
    std::views::values(_tracker.GetRacers()),
    [](const tracker::RaceTracker::Racer& racer)
    {
      return racer.state == tracker::RaceTracker::Racer::State::Racing
        || racer.state == tracker::RaceTracker::Racer::State::Disconnected;
    });

  const bool loadTimeoutReached = std::chrono::steady_clock::now() >= _stageTimeoutTimePoint;

  // If not all the racers have loaded yet and the timeout has not been reached yet
  // do not start the race.
  if (not allRacersLoaded && not loadTimeoutReached)
    return;

  if (loadTimeoutReached)
  {
    server::util::QuietLogWarn("Room {} has reached the loading timeout threshold",
      this->GetRoomUid());
  }

  // LOA-fix (R11-11, round11, backlog #20 п.1): ВЫЧЕРКНУТОГО НАДО ОБЪЯВИТЬ
  // КЛИЕНТАМ. Апстримный цикл менял состояние ТОЛЬКО на сервере — ни одной
  // отправки в нём не было, — поэтому у оставшихся в комнате навсегда висел
  // призрак, а ростеры сервера и клиентов расходились на весь заезд.
  // Делаем ровно то же, что уже делает HandleLeaveRoom при честном выходе:
  // state = Disconnected + AcCmdUserRaceDeleteNotify всем, кроме самого
  // выбывшего. Итератор переписан со std::views::values на структурное
  // связывание — рассылке нужен characterUid, которого при обходе values нет.
  // Шлём ТОЛЬКО тем, кто перешёл в Disconnected именно сейчас: у вышедших
  // раньше (HandleLeaveRoom) notify уже ушёл, и дубль на удалённый oid клиенту
  // не нужен. Гонщика без выданного oid пропускаем — мусорный oid хуже, чем
  // отсутствие пакета.
  for (auto& [racerCharacterUid, racer] : _tracker.GetRacers())
  {
    // todo: handle the players that did not load in to the race.
    // for now just consider them disconnected
    if (racer.state == tracker::RaceTracker::Racer::State::Racing)
      continue;

    const bool wasAlreadyDisconnected =
      racer.state == tracker::RaceTracker::Racer::State::Disconnected;
    racer.state = tracker::RaceTracker::Racer::State::Disconnected;

    if (wasAlreadyDisconnected
      || racer.oid == tracker::InvalidEntityOid)
      continue;

    server::util::QuietLogWarn(
      "Room {}: racer {} (oid {}) did not load in time, removing them from the "
      "race for the remaining players",
      this->GetRoomUid(),
      racerCharacterUid,
      racer.oid);

    const protocol::AcCmdUserRaceDeleteNotify deleteNotify{
      .racerOid = racer.oid};
    _raceNetworkHandler.BroadcastExceptCharacterUid(
      *this,
      deleteNotify,
      racerCharacterUid);

    // LOA-fix (R11-11b, round11, backlog #20 п.1): И САМОМУ ВЫБЫВШЕМУ ТОЖЕ
    // НАДО ЧТО-ТО СКАЗАТЬ (найдено панелью 2026-08-17).
    // DeleteNotify выше уходит ВСЕМ, КРОМЕ него; отсчёт он не получит (R11-13
    // шлёт только тем, кто в состоянии Racing); его опоздавший LoadingComplete
    // отобьёт гард R11-12. То есть без этой отправки симптом просто менялся с
    // «выкинуло из заезда» на «висит на экране загрузки до конца чужого
    // заезда» — клиента разблокировала бы только рассылка результатов в Stop().
    // Шлём штатный AcCmdCRStartRaceCancel(Generic) — единственное в протоколе
    // сообщение «заезд для тебя не состоится»; сервер уже отвечает им на
    // неудачный старт (RaceNetworkHandler::SendStartRaceCancel).
    // ClientId берём через комнату, как это делает рассылка отсчёта ниже:
    // приватные хелперы RaceNetworkHandler (GetClientIdByCharacterUid,
    // SendStartRaceCancel) отсюда недоступны, а расширять его публичный
    // интерфейс ради одной строки не станем.
    // ДУБЛЯ НЕ БУДЕТ: если опоздавший LoadingComplete от этого же клиента придёт
    // следом, гард R11-12b распознаёт ровно эту комбинацию (стадия Racing +
    // состояние Disconnected + валидный oid) как «Cancel уже отправлен» и второй
    // раз его не шлёт.
    // Отдельная копия uid — чтобы не тащить в лямбду structured binding.
    const auto evictedCharacterUid = racerCharacterUid;
    this->GetRoom(
      [this, evictedCharacterUid](const Room& room)
      {
        for (const auto& [roomCharacterUid, player] : room.GetPlayers())
        {
          if (roomCharacterUid != evictedCharacterUid)
            continue;

          _raceNetworkHandler.GetCommandServer()
            .QueueCommand<protocol::AcCmdCRStartRaceCancel>(
              player.GetClientId(),
              []()
              {
                return protocol::AcCmdCRStartRaceCancel{
                  .reason = protocol::AcCmdCRStartRaceCancel::Reason::Generic};
              });
        }
      });
  }

  const auto& mapBlockTemplate = _raceNetworkHandler
    .GetServerInstance()
    .GetCourseRegistry()
    .GetMapBlockInfo(GetMapBlockId());

  // LOA-fix (R66-4, backlog #78): на какой карте заезд РЕАЛЬНО поехал.
  //
  // ★ОДНА СТРОКА НА ЗАЕЗД, И ИМЕННО НА СТАРТЕ. План просил печатать выбор карты
  // ещё и при каждой смене опций комнаты — я это СОЗНАТЕЛЬНО не делаю. Опции
  // щёлкают многократно, пока комната собирается, и такая строка дала бы поток
  // «выбрана-выбрана-выбрана», из которого нельзя посчитать ни одного заезда.
  // Задача #78 — прод-правда о том, НА ЧЁМ ездят (концентрация трасс, вход в
  // разговор о предзагрузке), а это величина ЗА ЗАЕЗД.
  // ★Если понадобится видеть ОТВЕРГНУТЫЕ выборы (гард R11-16 не пускает карту
  // не из пула режима), это должна быть отдельная строка ОБ ОТКАЗЕ, а не строка
  // о выборе: смешивать «поехали на карте N» и «просили карту M, не дали» в
  // одном тексте — верный способ получить сводку, которой нельзя верить.
  //
  // ★СЧИТАЕМ ТЕХ, КТО ПОЕХАЛ, А НЕ РАЗМЕР ТРЕКЕРА (WARN ревью, итерация 1).
  // Прямо ВЫШЕ в этой же функции недогрузившиеся помечаются `Disconnected` и
  // получают StartRaceCancel — они в заезд не поехали, но из `_racers` не
  // удаляются. `GetRacers().size()` посчитал бы их наравне с едущими, и строка
  // «поехали ввосьмером» соседствовала бы в журнале с тремя отменами старта.
  // Это ровно тот грех смешения веток, против которого написан абзац выше.
  const auto racingRacerCount = std::ranges::count_if(
    _tracker.GetRacers() | std::views::values,
    [](const tracker::RaceTracker::Racer& racer)
    {
      return racer.state != tracker::RaceTracker::Racer::State::Disconnected;
    });
  server::util::QuietLogInfo(
    "Room {}: race starting on map block {} (game mode {}, {} racers)",
    this->GetRoomUid(),
    static_cast<uint32_t>(GetMapBlockId()),
    static_cast<uint32_t>(GetGameModeId()),
    racingRacerCount);

  // Switch to the racing stage and set the timeout time point.
  _stage = Stage::Racing;
  // LOA-fix (R11-14, round11, backlog #20 п.5): У СОЛО ТОЖЕ ЕСТЬ ДЕДЛАЙН.
  // Раньше соло-комната получала time_point::max(), то есть стадия Racing была
  // ТЕРМИНАЛЬНОЙ: выйти из неё можно было только по AcCmdUserRaceFinal от
  // самого гонщика (Waiting присваивается ровно в одном месте — TickFinishing).
  // Гонщик вылетел или завис — комната стоит в Racing вечно, и её уже ничем не
  // расшить (наш R8-1b, справедливо, не даёт стартовать не из Waiting).
  // Даём соло тот же лимит карты плюс запас: честный круг 1.5-3 минуты при
  // лимите 300 c, поэтому живую игру это не режет, а зависание снимает.
  auto raceStageTimeLimit = std::chrono::seconds(mapBlockTemplate.timeLimit);
  if (_parameters.teamMode == protocol::TeamMode::Single)
    raceStageTimeLimit += SoloRaceGracePeriod;

  _stageTimeoutTimePoint = std::chrono::steady_clock::now() + raceStageTimeLimit;

  // Set up the race start time point.
  const auto now = std::chrono::steady_clock::now();
  _raceStartTimePoint = now + std::chrono::seconds(
    mapBlockTemplate.waitTime);

  const protocol::AcCmdUserRaceCountdown raceCountdown{
    .raceStartTimestamp = util::TimePointToRaceTimePoint(
      _raceStartTimePoint)};

  // LOA-fix (R11-13, round11, backlog #20 п.3): ОТСЧЁТ — ТОЛЬКО УЧАСТНИКАМ.
  // Broadcast перебирает ВСЕХ игроков комнаты без взгляда на трекер, поэтому
  // AcCmdUserRaceCountdown улетал и тому, кого строкой выше вычеркнули по
  // таймауту загрузки: клиент запускал заезд, которого для него уже нет.
  // Фильтр скопирован с готового образца в TickRacing (рассылка
  // AcCmdUserRaceFinalNotify только участникам) и ужесточён до состояния Racing.
  this->GetRoom(
    [this, raceCountdown](const Room& room)
    {
      for (const auto& [characterUid, player] : room.GetPlayers())
      {
        if (not _tracker.IsRacer(characterUid))
          continue;
        if (_tracker.GetRacer(characterUid).state
          != tracker::RaceTracker::Racer::State::Racing)
          continue;

        _raceNetworkHandler.GetCommandServer()
          .QueueCommand<protocol::AcCmdUserRaceCountdown>(
            player.GetClientId(),
            [raceCountdown]()
            {
              return raceCountdown;
            });
      }
    });
}

void RaceInstance::TickRacing()
{
  const bool raceTimeoutReached = std::chrono::steady_clock::now() >= _stageTimeoutTimePoint;

  const bool isFinishing = std::ranges::any_of(
    std::views::values(_tracker.GetRacers()),
    [](const tracker::RaceTracker::Racer& racer)
    {
      return racer.state == tracker::RaceTracker::Racer::State::Finishing;
    });

  // If the race is not finishing and the timeout was not reached
  // do not finish the race.
  if (not isFinishing && not raceTimeoutReached)
    return;

  // LOA-fix (R11-14b, round11, backlog #20 п.5): БРОШЕННОЕ СОЛО НЕ ПЛАТИТ.
  // ЗАЧЕМ ЭТО ЗДЕСЬ. R11-14 дал соло-комнате конечный дедлайн стадии Racing —
  // это лечит вечный залип, но одновременно ВПЕРВЫЕ открывает соло дорогу
  // TickRacing → Stage::Finishing → TickFinishing → Stop(). А в Stop() базовая
  // награда безусловна: score.carrots = 2500 и score.experience = 420
  // присваиваются ДО всяких проверок, courseTime гейтит только множители, и
  // ниже они начисляются каждому гонщику из трекера. Итог был бы такой: зашёл в
  // соло, нажал старт, ушёл пить чай, через (лимит карты + запас + 15 c) получил
  // 2500 морковок и 420 опыта, не проехав ни метра. Раньше этот путь был
  // недостижим (дедлайн = time_point::max()), то есть это была бы НОВАЯ выплата,
  // созданная нашим же патчем.
  // ЧТО ДЕЛАЕМ. Если в РЕАЛЬНО ОДИНОЧНОМ заезде сработал дедлайн и при этом нет
  // НИ ОДНОГО валидного courseTime (никто не доехал) — возвращаем комнату в
  // Waiting, минуя Stop(), то есть минуя весь блок наград.
  // ПОЧЕМУ УСЛОВИЙ ДВА, А НЕ ОДНО. teamMode == Single — это ОПЦИЯ комнаты, а не
  // «едет один»: матчмейкинг подселяет в Single-комнату второго и далее, пока
  // есть слоты. Поэтому к опции добавлено фактическое число ГОНЩИКОВ в заезде
  // (_tracker.GetRacers().size() == 1). Иначе ветка отбирала бы выплату у
  // мультизаезда, которому пин её платил, — то есть чинила бы залип ценой
  // регрессии экономики.
  // ПОЧЕМУ КРИТЕРИЙ ИМЕННО «НЕТ ВАЛИДНОГО courseTime», А НЕ «ВСЕ ОТВАЛИЛИСЬ».
  // Сузить его до Disconnected нельзя: тогда ПОДКЛЮЧЁННЫЙ игрок, который просто
  // простоял всю стадию Racing, снова получал бы 2500/420 «за постоять» — то
  // есть ровно тот фарм, ради закрытия которого эта ветка и написана.
  // ЧТО ЭТОТ КРИТЕРИЙ ЗАКРЫВАЕТ, А ЧТО НЕТ (чтобы не читалось шире, чем есть):
  // он закрывает случай, когда клиент НЕ ШЛЁТ AcCmdUserRaceFinal (краш,
  // зависание, AFK — то есть когда финал должен был прийти от сервера). Если же
  // клиент по СВОЕМУ таймеру карты сам присылает DNF-финал, срабатывает
  // пред-существующая дорога пина (state = Finishing → TickFinishing → Stop()) и
  // база выплачивается — см. «ГРАНИЦА ЭТОГО ФИКСА» ниже. Какая из двух дорог
  // случается на живом клиенте, замеряется первым же смоук-заездом раунда 11.
  // ЧЕГО НЕ ТРОГАЕМ (важно, это узкий фикс, а не смена правил экономики):
  //   * обычный мультизаезд идёт прежним путём — нефинишировавшие в нём как
  //     получали базовые 2500/420, так и получают. Это верно и для комнаты с
  //     опцией Single, если игроков в ней больше одного;
  //   * соло, где гонщик ДОЕХАЛ (courseTime валиден) или уже в состоянии
  //     Finishing, тоже идёт прежним путём и получает законную награду;
  //   * сама формула награды в Stop() не изменена ни на символ.
  // ГРАНИЦА ЭТОГО ФИКСА (уточнено второй панелью 2026-08-17, чтобы не читалось
  // шире, чем есть): закрывается ИМЕННО ТАЙМАУТНАЯ ветка, которую открыл R11-14.
  // Вторая дорога к базовой награде в соло существует С ПИНА и здесь НЕ
  // трогается: клиент шлёт AcCmdUserRaceFinal, HandleUserRaceFinal безусловно
  // ставит state = Finishing, isFinishing становится true, TickFinishing видит
  // allRacersFinished и зовёт Stop(). Это пред-существующее поведение, а не
  // регрессия батча; закрывать его — менять правила экономики, решение владельца
  // (отдельный пункт бэклога, вне scope #20).
  // ПОЧЕМУ РУЧНАЯ УБОРКА КОМНАТЫ. Stop() кроме наград делает и хозяйственную
  // часть — снимает флаг «комната играет» и сбрасывает готовность игроков.
  // Пропустить её нельзя: комната навсегда осталась бы «играющей» в списке
  // лобби с залипшими галками готовности. Повторяем ровно эти два действия,
  // ничего денежного.
  // ПОЧЕМУ ЕЩЁ И ТЕРМИНАЛЬНЫЙ ПАКЕТ (WARN второй панели 2026-08-17). Ранний
  // выход минует не только награды, но и ЕДИНСТВЕННУЮ рассылку, которая снимает
  // клиента с гоночной сцены — Broadcast(AcCmdRCRaceResultNotify) внутри Stop(),
  // — а заодно и рассылку AcCmdUserRaceFinalNotify ниже. Без пакета живой, но
  // не доехавший соло-гонщик остался бы на карте навсегда, хотя сервер уже
  // вернул комнату в Waiting: это тот же размен «краш → вечное висение», который
  // весь батч и лечит. Денег не платим, но СЦЕНУ КЛИЕНТУ ЗАКРЫВАЕМ.
  // ЧЕМ ИМЕННО ЗАКРЫВАЕМ И ПОЧЕМУ. Кандидатов в дереве два.
  //   * AcCmdUserRaceFinalNotify — единственный прецедент терминального пакета
  //     ЕДУЩЕМУ (TickRacing шлёт его участникам при обычном таймауте, чуть ниже).
  //     Но он открывает финальный экран и ждёт следом AcCmdRCRaceResultNotify из
  //     Stop() — то есть сам по себе в комнату НЕ возвращает, а Stop() мы здесь
  //     обязаны обойти. Отправить его одного = снова оставить клиента ждать.
  //   * AcCmdCRStartRaceCancel(Generic) — единственное в протоколе «заезда для
  //     тебя не будет», не требующее продолжения. Его же шлют R11-11b
  //     (вычеркнутому по таймауту загрузки) и R11-12b (опоздавшему), поэтому
  //     проверять на стенде надо ОДНУ реакцию клиента, а не две.
  // Берём второй. Прецедента отправки Cancel уже ЕДУЩЕМУ в апстриме нет — это
  // ГИПОТЕЗА, и она вынесена в смоук-чек-лист раунда 11 (CHANGES.md, раздел
  // «Смоук раунда 11», пункт 2: соло, не ехать до дедлайна — вернулся ли клиент
  // в комнату). Шлём только тем, кто ещё числится участником и не отвалился:
  // отвалившемуся пакет слать некому.
  // TeamMode::Single — ЭТО ОПЦИЯ КОМНАТЫ, А НЕ ГАРАНТИЯ ОДНОГО ГОНЩИКА (WARN
  // третьей панели 2026-08-17). Room::AddPlayer ограничивает только
  // maxPlayerCount, а MatchmakingSystem::Matchmake подселяет в комнату по
  // совпадению teamMode и свободному слоту — то есть Single-комната на 2-8 живых
  // игроков достижима штатным матчмейкингом. Без второго условия ветка отбирала
  // бы у ТАКОЙ комнаты выплату, которую пин платил всем её участникам, и
  // заявление «обычный мультизаезд идёт прежним путём» стало бы неправдой.
  // СЧИТАЕМ ПО РОСТЕРУ ТРЕКЕРА, А НЕ ПО Room::GetPlayerCount (BLOCK-2 панели
  // Codex T2 2026-08-17). Комната ≠ заезд, и её состав меняется на входах и
  // выходах ПРЯМО ВО ВРЕМЯ заезда, поэтому счёт по комнате врал в обе стороны:
  //   * двухигровой Single, из которого один вышел, к дедлайну выглядел как
  //     «соло» — и оставшийся второй участник терял законную выплату;
  //   * не-гонщик, зашедший в комнату во время одиночного заезда, поднимал
  //     GetPlayerCount() до 2, ветка не срабатывала — и брошенное соло снова
  //     получало AFK-награду, ради отсечения которой она и написана.
  // _tracker.GetRacers() набирается один раз на старте заезда (Tracker::Clear +
  // AddRacer в HandleStartRace) и до его конца не меняется — это стабильный
  // список тех, кто РЕАЛЬНО ЕДЕТ, и именно он здесь авторитетен. Он же
  // используется строкой ниже для проверки courseTime, так что критерий целиком
  // считается по одному источнику. Отдельная GetRoom-лямбда для подсчёта больше
  // не нужна: _tracker — прямой член RaceInstance.
  const bool isSoloRaceRoster = _tracker.GetRacers().size() == 1;

  const bool isAbandonedSoloRace =
    raceTimeoutReached
    && not isFinishing
    && _parameters.teamMode == protocol::TeamMode::Single
    && isSoloRaceRoster
    && std::ranges::none_of(
      std::views::values(_tracker.GetRacers()),
      [](const tracker::RaceTracker::Racer& racer)
      {
        return racer.courseTime != tracker::InvalidCourseTime;
      });

  if (isAbandonedSoloRace)
  {
    server::util::QuietLogWarn(
      "Room {}: solo race timed out without a single valid finish; returning "
      "the room to the waiting stage without running the results",
      this->GetRoomUid());

    this->GetRoom(
      [this](Room& room)
      {
        // Терминальный пакет участникам: заезд для них не состоялся. Делается ДО
        // возврата комнаты в Waiting и тем же способом, что в R11-11b — через
        // перебор игроков комнаты, потому что приватные хелперы
        // RaceNetworkHandler отсюда недоступны.
        for (const auto& [characterUid, player] : room.GetPlayers())
        {
          if (not _tracker.IsRacer(characterUid))
            continue;
          if (_tracker.GetRacer(characterUid).state
            == tracker::RaceTracker::Racer::State::Disconnected)
            continue;

          _raceNetworkHandler.GetCommandServer()
            .QueueCommand<protocol::AcCmdCRStartRaceCancel>(
              player.GetClientId(),
              []()
              {
                return protocol::AcCmdCRStartRaceCancel{
                  .reason = protocol::AcCmdCRStartRaceCancel::Reason::Generic};
              });
        }

        room.SetRoomPlaying(false);
        for (const auto& characterUid : room.GetPlayers() | std::views::keys)
        {
          room.GetPlayer(characterUid).SetReady(false);
        }
      });

    _stage = Stage::Waiting;
    return;
  }

  _stage = Stage::Finishing;
  _stageTimeoutTimePoint = std::chrono::steady_clock::now() + std::chrono::seconds(15);

  // If the race timeout was reached notify the clients about the finale.
  if (not raceTimeoutReached)
    return;

  // Broadcast the race final (only to participants).
  this->GetRoom([this](const Room& room)
  {
    for (const auto& [characterUid, player] : room.GetPlayers())
    {
      const bool isParticipant = _tracker.IsRacer(characterUid);
      if (not isParticipant)
        continue;

      const protocol::AcCmdUserRaceFinalNotify notify{};
      _raceNetworkHandler.GetCommandServer().QueueCommand<decltype(notify)>(
        player.GetClientId(),
        [notify]()
        {
          return notify;
        });
    }
  });
}

void RaceInstance::TickFinishing()
{
  // Determine whether all racers have finished.
  const bool allRacersFinished = std::ranges::all_of(
    std::views::values(_tracker.GetRacers()),
    [](const tracker::RaceTracker::Racer& racer)
    {
      return racer.state == tracker::RaceTracker::Racer::State::Finishing
        || racer.state == tracker::RaceTracker::Racer::State::Disconnected;
    });

  const bool finishTimeoutReached = std::chrono::steady_clock::now() >= _stageTimeoutTimePoint;

  // If not all the racers have finished yet and the timeout has not been reached yet
  // do not finish the race.
  if (not allRacersFinished && not finishTimeoutReached)
    return;

  if (finishTimeoutReached)
  {
    server::util::QuietLogWarn("Room {} has reached the race timeout threshold",
      this->GetRoomUid());
  }

  Stop();
 _stage = Stage::Waiting;
}

void RaceInstance::TickActiveRaceContent()
{
  // Tick active race content
  this->TickItemSpawners();
  if (this->GetParameters().gameMode == protocol::GameMode::Magic)
    // Tick magic gauge
    this->TickMagicGauge();
}

void RaceInstance::TickItemSpawners()
{
  constexpr double ItemSpawnDistanceThreshold = 90.0;

  const auto processItemSpawn = [&](
    ClientId clientId,
    tracker::RaceTracker::Racer& racer,
    const tracker::Oid oid,
    const uint32_t itemType,
    const protocol::Vector3& position,
    const uint32_t spawnStyle = 0)
  {
    const auto distance = (racer.worldPosition - position).Length();

    const bool isInProximity = distance < ItemSpawnDistanceThreshold;
    const bool isAlreadyTracked = racer.trackedDecks.contains(oid);

    if (isAlreadyTracked)
    {
      if (not isInProximity)
        racer.trackedDecks.erase(oid);

      return;
    }

    // If item is not in proximity or this is not the first pass
    // then do not trigger item spawn
    if (not isInProximity and not this->GetTracker().firstPassItemSpawn)
      return;

    const protocol::AcCmdRCCreateItem spawn{
      .itemId = oid,
      .itemType = itemType,
      .position = position,
      .spawnStyle = spawnStyle,
      .spawnerId = 0,
      .sizeLevel = 0};

    racer.trackedDecks.insert(oid);
    _raceNetworkHandler.GetCommandServer().QueueCommand<decltype(spawn)>(
      clientId,
      [spawn]()
      {
        return spawn;
      });
  };

  // Loop through each player in the room
  this->GetRoom([this, &processItemSpawn](const Room& room)
  {
    for (const auto& [characterUid, player] : room.GetPlayers())
    {
      // Check if this player is an active racer
      if (not this->GetTracker().IsRacer(characterUid))
        continue;

      auto& racer = this->GetTracker().GetRacer(characterUid);
      for (const auto& item : this->GetTracker().GetItemDecks() | std::views::values)
      {
        const auto now = std::chrono::steady_clock::now();

        // Spawner availability is per-racer,
        // skip spawn if this racer is currently on pickup cooldown
        const auto cooldownIter = racer.deckCooldown.find(item.oid);
        if (cooldownIter != racer.deckCooldown.end() && now < cooldownIter->second)
          continue;

        processItemSpawn(
          player.GetClientId(),
          racer,
          item.oid,
          item.currentItem,
          item.position);
      }

      for (const auto& eventItem : racer.eventItems)
        processItemSpawn(
          player.GetClientId(),
          racer,
          eventItem.oid,
          eventItem.itemType,
          eventItem.position,
          3);

      // LOA-fix (R68, backlog #5/#99): КВЕСТОВЫЕ предметы — тем же каналом.
      // ★spawnStyle остаётся ДЕФОЛТНЫМ 0, как у обычных деков, а не 3, как у
      // яиц. Значения не выдуманы: они прямо перечислены в клиентских скриптах
      // (`script_src/common/aistate.lua`) — NONE 0, POPUP 1, DANGLE 2, ROAD 3.
      // Яйцам нужен ROAD, потому что их позицию присылает клиент на ходу;
      // квестовый предмет, как и дека, приходит из АВТОРСКИХ координат карты,
      // значит и стиль у него дековый.
      for (const auto& questItem : racer.questItems)
        processItemSpawn(
          player.GetClientId(),
          racer,
          questItem.oid,
          questItem.itemType,
          questItem.position);
    }
  });

  // Flip first pass item spawn logic
  if (this->GetTracker().firstPassItemSpawn)
    this->GetTracker().firstPassItemSpawn = false;
}

void RaceInstance::TickMagicGauge()
{
  // Only regenerate magic during an active race (after the countdown finishes)
  const auto now = std::chrono::steady_clock::now();
  if (now <= this->GetRaceStartTimePoint())
    return;

  this->GetRoom([this, &now](const Room& room)
  {
    const auto& regenerationInfo = _raceNetworkHandler.GetServerInstance().GetMagicRegistry().GetRegenInfo();
    const auto tickInterval = std::chrono::milliseconds(regenerationInfo.intervalMs);

    for (const auto& [characterUid, player] : room.GetPlayers())
    {
      // Check if this player is an active racer
      if (not this->GetTracker().IsRacer(characterUid))
        continue;

      auto& racer = this->GetTracker().GetRacer(characterUid);
      const bool isRacerHoldingItem = racer.magicItem.has_value();

      // Anchor at race start so fill time is consistent regardless of when the first pos-update arrives.
      if (racer.lastGaugeUpdateTimePoint == std::chrono::steady_clock::time_point::max())
        racer.lastGaugeUpdateTimePoint = this->GetRaceStartTimePoint();

      // Elapsed time since the last gauge update.
      const auto elapsed = now - racer.lastGaugeUpdateTimePoint;
      const auto elapsedTickCount = elapsed / tickInterval;

      if (elapsedTickCount > 0)
      {
        racer.lastGaugeUpdateTimePoint = now;

        // Set bonus (effect 3) lets the gauge keep filling while holding a spell.
        const bool canRegen = not isRacerHoldingItem
          or racer.activeSetEffect == registry::SetEquipEffect::GaugeWhileHolding;

        if (canRegen and racer.starPointValue < _gameModeInfo.starPointsMax)
        {
          const auto& setBonusInfo =
            _raceNetworkHandler.GetServerInstance().GetMagicRegistry().GetSetBonusInfo();

          uint32_t gainedPerTick = regenerationInfo.pointPerTick
            * (1000u + regenerationInfo.courageScaleBp * racer.mountStats.courage) / 1000u;

          // Set bonus (effect 2): passive gauge fills faster.
          if (racer.activeSetEffect == registry::SetEquipEffect::PassiveGaugeFaster)
            gainedPerTick = gainedPerTick * (10000u + setBonusInfo.passiveGaugeScaleBp) / 10000u;

          // BufGauge buff doubles regen while active.
          if (racer.effects[20] or racer.effects[21])
            gainedPerTick *= 2;

          // Set bonus (effect 3): while holding a spell, the gauge fills at a reduced rate.
          if (isRacerHoldingItem)
            gainedPerTick = gainedPerTick * setBonusInfo.holdingGaugeScaleBp / 10000u;

          const uint32_t totalGain = gainedPerTick * static_cast<uint32_t>(elapsedTickCount);
          racer.starPointValue = std::min(
            _gameModeInfo.starPointsMax,
            racer.starPointValue + totalGain);
        }
      }

      const bool shouldGiveItem =
        not isRacerHoldingItem and
        racer.starPointValue >= _gameModeInfo.starPointsMax;

      const protocol::AcCmdCRStarPointGetOK starPointResponse{
        .characterOid = racer.oid,
        .starPointValue = racer.starPointValue,
        .giveMagicItem = shouldGiveItem};

      _raceNetworkHandler.GetCommandServer().QueueCommand<decltype(starPointResponse)>(
        player.GetClientId(),
        [starPointResponse]
        {
          return starPointResponse;
        });
    }
  });
}

void RaceInstance::PrepareGameMode()
{
  _gameModeId = static_cast<registry::GameModeId>(_parameters.gameMode);
  _gameModeInfo = _raceNetworkHandler
    .GetServerInstance()
    .GetCourseRegistry()
    .GetCourseGameModeInfo(_gameModeId);
}

void RaceInstance::PickRandomMapFromCourse()
{
  uint32_t masterLevel{};
  // Use the room master's level to filter the maps
  _raceNetworkHandler.GetServerInstance()
    .GetDataDirector()
    .GetCharacter(_parameters.masterUid)
    .Immutable(
      [&masterLevel](const data::Character& character)
      {
        masterLevel = character.level();
      });

  // Filter out the maps that are above the master's level.
  std::vector<registry::MapBlockId> filtered;
  std::ranges::copy_if(
    std::as_const(_gameModeInfo.mapBlockPool),
    std::back_inserter(filtered),
    [this, masterLevel](registry::MapBlockId mapBlockId)
    {
      try
      {
        const auto& mapBlockInfo = _raceNetworkHandler.GetServerInstance()
          .GetCourseRegistry()
          .GetMapBlockInfo(
            mapBlockId);
        return mapBlockInfo.requiredLevel <= masterLevel;
      }
      catch (const std::exception& e)
      {
        server::util::QuietLogWarn("Failed to get map block info for mapBlockId {}: {}", mapBlockId, e.what());
        return false;
      }
    });

  // LOA-fix (R11-17, round11, backlog #23): ГАРД ПУСТОЙ ВЫБОРКИ.
  // Фильтр выше молча выбрасывает карты, которых нет в реестре (catch внутри
  // лямбды возвращает false), поэтому пустой filtered достижим не только
  // ужесточением требований по уровню, но и просто битым mapBlockPool. На
  // пустом векторе `filtered.size() - 1` = SIZE_MAX, приведение к int даёт -1,
  // распределение с (0, -1) — UB, а следом чтение за границей.
  // Бросок здесь безопасен: PrepareMap вызывается из RaceInstance::Start()
  // внутри try/catch, Start() вернёт false, и мастер получит штатный
  // AcCmdCRStartRaceCancel вместо разрыва соединения.
  if (filtered.empty())
  {
    throw std::runtime_error(
      std::format(
        "Game mode {} has no maps available for master level {}",
        _gameModeId,
        masterLevel));
  }

  // Select a random map from the pool.
  std::uniform_int_distribution<registry::MapBlockId> distribution(
    0,
    static_cast<int>(filtered.size() - 1));

  _mapBlockId = filtered[distribution(server::util::GetRandomEngine())];
}

void RaceInstance::PrepareMap()
{
  if (_gameModeInfo.mapBlockPool.empty())
  {
    throw std::runtime_error(
      std::format(
        "Game mode {} does not have any maps",
        _gameModeId));
  }

  // If the map is set to a course pick a random map.
  if (_parameters.mapBlockId == AllMapsCourseId
    || _parameters.mapBlockId == NewMapsCourseId
    || _parameters.mapBlockId == HotMapsCourseId)
  {
    PickRandomMapFromCourse();
  }
  else
  {
    _mapBlockId = _parameters.mapBlockId;
  }

  _mapBlockInfo = _raceNetworkHandler
    .GetServerInstance()
    .GetCourseRegistry()
    .GetMapBlockInfo(_mapBlockId);

  try
  {
    // Prepare the item decks on the map.
    PrepareItemDecks();
  }
  catch (const std::exception& e)
  {
    throw std::runtime_error(
      std::format(
        "Exception while preparing items for game mode {} and map id {}: {}",
        _gameModeId,
        _mapBlockId,
        e.what()));
  }
}

void RaceInstance::PickRandomItemFromDeck(tracker::RaceTracker::ItemDeck& deck)
{
  if (deck.items.empty())
    return;

  std::uniform_int_distribution<size_t> distribution(0, deck.items.size() - 1);
  deck.currentItem = deck.items[distribution(server::util::GetRandomEngine())];
}

// LOA-fix (R68, backlog #5/#99): РАСКЛАДКА КВЕСТОВЫХ ПРЕДМЕТОВ.
//
// Класс целей `CollectDropItem` был мёртв в ОБЕ стороны: событие подбора не
// эмитил никто, и сами предметы не спавнил никто. Это вторая половина —
// гонщику, несущему квест «собери N штук предмета X», предметы X
// раскладываются на АВТОРСКИХ координатах карты (те же точки, что в клиентской
// таблице QuestItemDeckInfo, перенесённой в courses.yaml).
//
// ★ПРЕДМЕТЫ ПЕР-ГОНЩИКОВЫЕ И ПЕР-КВЕСТОВЫЕ. Пер-гонщиковые — потому что на
// одной точке карты у двух игроков лежит разное. Пер-КВЕСТОВЫЕ — потому что
// одного вида предмета мало для решения, кому засчитать: пять сюжетных квестов
// делят QTemID 3, три делят QTemID 6, два делят QTemID 5. Раскладка «по виду
// предмета» дала бы один осколок = +1 сразу пяти квестам.
void RaceInstance::PrepareQuestItems()
{
  //! Потолок предметов у ОДНОГО гонщика за заезд. Выбран ВЫШЕ худшего случая
  //! по данным: если бы игрок нёс все 12 сюжетных квестов класса сразу, самая
  //! щедрая карта (13) дала бы ему 27 предметов. То есть кап не режет игру, он
  //! ограничивает БЮДЖЕТ OID'ов (`_nextItemDeckOid` — uint16, общий с деками и
  //! яйцами, сбрасывается в 1 на каждом `Tracker::Clear()`): 8 гонщиков * 32 +
  //! деки карты (максимум 42) + яйца (<= 8) = 306 из 65535.
  constexpr std::size_t MaxQuestItemsPerRacer = 32;

  auto& serverInstance = _raceNetworkHandler.GetServerInstance();
  const auto& questItemDecks = serverInstance.GetCourseRegistry().GetQuestItemDecks();
  if (questItemDecks.empty())
    return;

  auto& dataDirector = serverInstance.GetDataDirector();
  const auto& questRegistry = serverInstance.GetQuestRegistry();

  std::size_t spawnedTotal = 0;

  for (auto& [characterUid, racer] : _tracker.GetRacers())
  {
    // --- 1. Какие КВЕСТЫ этого гонщика ждут предметов ----------------------
    // Собираем ПАРЫ (квест, предмет). Только чтение: запись персонажа берётся
    // shared, записи квестов — тоже shared, мутаций здесь нет вовсе.
    std::vector<std::pair<uint32_t, uint32_t>> pendingQuests;

    const auto characterRecord = dataDirector.GetCharacter(characterUid);
    if (not characterRecord)
      continue;

    characterRecord.Immutable(
      [&dataDirector, &questRegistry, &pendingQuests](const data::Character& character)
      {
        const auto questRecords = dataDirector.GetQuestCache().Get(character.quests());
        if (not questRecords)
          return;

        for (const auto& questRecord : *questRecords)
        {
          questRecord.Immutable(
            [&questRegistry, &pendingQuests](const data::Quest& quest)
            {
              if (quest.isCompleted() != data::Quest::Status::InProgress)
                return;

              const auto questTid = static_cast<uint32_t>(quest.questId());
              bool isCollectQuest = false;
              for (const uint32_t tid : QuestSystem::CollectDropItemMainQuestTids)
                if (tid == questTid) { isCollectQuest = true; break; }
              if (not isCollectQuest)
                return;

              const auto questTemplate = questRegistry.GetQuest(questTid);
              if (not questTemplate.has_value())
                return;

              pendingQuests.emplace_back(questTid, questTemplate->functionValue);
            });
        }
      });

    if (pendingQuests.empty())
      continue;

    // --- 2. Раскладка ------------------------------------------------------
    // ★ПОЗИЦИИ ОДНОЙ ДЕКИ БЕРУТСЯ ОДИН РАЗ НА ГОНЩИКА и раздаются по общему
    // курсору. Иначе два квеста, которым нужен один и тот же предмет (напр.
    // 12015 и 14024 — оба «шерсть»), положили бы свои предметы в ОДНИ И ТЕ ЖЕ
    // точки карты: два объекта в одной координате.
    // ★КУРСОР ЗАМЫКАЕТСЯ ПО КРУГУ, а не упирается в конец списка. Прямолинейный
    // вариант («точки кончились — следующий квест не получает ничего») выглядел
    // безобиднее, но на бедной точками карте (у деки 1002 на карте 13 их пять,
    // а трём «шерстяным» квестам нужно 4+4+4) он всегда обделял бы ОДИН И ТОТ
    // ЖЕ квест: порядок обхода детерминирован, значит обделённый не получил бы
    // предметов на этой карте НИКОГДА. Кольцо даёт каждому квесту его норму;
    // цена — совпадающие координаты у предметов РАЗНЫХ квестов, что честно
    // отражает данные (точек на карте физически меньше, чем спроса).
    // Внутри ОДНОГО квеста повторов нет: его норма клампится числом точек.
    std::map<registry::DeckId, std::pair<std::vector<protocol::Vector3>, std::size_t>>
      positionPool;

    for (const auto& questItemDeck : questItemDecks)
    {
      for (const auto& spawnPoint : questItemDeck.spawnPoints)
      {
        if (spawnPoint.mapBlockId != _mapBlockId)
          continue;

        for (const auto& pendingQuest : pendingQuests)
        {
          if (pendingQuest.second != questItemDeck.questItemId)
            continue;

          auto poolIter = positionPool.find(spawnPoint.deckId);
          if (poolIter == positionPool.end())
          {
            // Координаты этой деки на ЭТОЙ карте — ровно те, из которых берёт
            // позиции `PrepareItemDecks`, и с тем же смещением карты.
            std::vector<protocol::Vector3> positions;
            for (const auto& deckInstance : _mapBlockInfo.itemDecks)
            {
              if (deckInstance.deckId == spawnPoint.deckId)
                positions.push_back(deckInstance.position + _mapBlockInfo.offset);
            }

            // Точек на карте обычно БОЛЬШЕ, чем предметов за заезд (20 против 4
            // у шерсти на карте 1), — какие занять, решает жребий. Брать первые
            // N значило бы гонять квест на 20 предметов по одним и тем же
            // четырём кустам пять заездов подряд.
            std::shuffle(
              positions.begin(),
              positions.end(),
              server::util::GetRandomEngine());

            poolIter = positionPool.emplace(
              spawnPoint.deckId,
              std::make_pair(std::move(positions), std::size_t{0})).first;
          }

          auto& positions = poolIter->second.first;
          auto& cursor = poolIter->second.second;
          if (positions.empty())
            continue;

          const std::size_t spawnCount = std::min<std::size_t>(
            questItemDeck.spawnCount, positions.size());

          for (std::size_t index = 0; index < spawnCount; ++index)
          {
            if (racer.questItems.size() >= MaxQuestItemsPerRacer)
              break;

            auto& questItem = _tracker.AddQuestItem(characterUid);
            questItem.itemType = spawnPoint.deckId;
            questItem.questItemId = questItemDeck.questItemId;
            questItem.questTid = pendingQuest.first;
            questItem.position = positions[cursor % positions.size()];
            ++cursor;
            ++spawnedTotal;
          }
        }
      }
    }
  }

  if (spawnedTotal > 0)
    server::util::QuietLogInfo(
      "Room {} spawned {} quest drop item(s) on map {}",
      GetRoomUid(),
      spawnedTotal,
      static_cast<uint32_t>(_mapBlockId));
}

void RaceInstance::PrepareItemDecks()
{
  // Get the map position offset
  const auto& offset = _mapBlockInfo.offset;

  // Create item decks based on the game mode.
  for (const registry::DeckId usedDeckId : _gameModeInfo.usedDeckIds)
  {
    const auto& deckInfo = _raceNetworkHandler.GetServerInstance().GetCourseRegistry().GetDeckInfo(
      usedDeckId);

    for (const auto& deckInstance : _mapBlockInfo.itemDecks)
    {
      if (deckInstance.deckId != usedDeckId)
        continue;

      auto& deck = _tracker.AddItemDeck();
      deck.items = deckInfo.items;
      deck.respawnTime = deckInfo.respawnTime;

      // 50% chance for this item spawner to only spawn positional magic item 412
      if (_parameters.gameMode == protocol::GameMode::Magic)
      {
        std::bernoulli_distribution positionalSpawnerChance(0.5);
        if (positionalSpawnerChance(server::util::GetRandomEngine()))
        {
          static constexpr uint32_t positionalMagicItemDeckId = 412;
          deck.items = {positionalMagicItemDeckId};
        }
        else if (_parameters.teamMode != protocol::TeamMode::Team)
        {
          // Filter out team-only magic items
          const auto& courseRegistry = _raceNetworkHandler.GetServerInstance().GetCourseRegistry();
          const auto& magicRegistry = _raceNetworkHandler.GetServerInstance().GetMagicRegistry();

          std::erase_if(
            deck.items,
            [&](uint32_t deckItemId)
            {
              const auto magicSlot = courseRegistry.GetDeckItemInfo(deckItemId).magicSlot;
              const auto& slotInfo = magicRegistry.GetSlotInfo(magicSlot);
              return slotInfo.teamMode != 0;
            });
        }
      }
      
      deck.position = deckInstance.position + offset;

      PickRandomItemFromDeck(deck);
    }
  }
}

} // namespace server
