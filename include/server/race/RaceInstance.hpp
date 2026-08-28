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

#ifndef RACEINSTANCE_HPP
#define RACEINSTANCE_HPP

#include "server/race/P2dIdPool.hpp"
#include "server/tracker/RaceTracker.hpp"

#include <libserver/data/DataDefinitions.hpp>
#include <libserver/network/NetworkDefinitions.hpp>
#include <libserver/network/command/proto/CommonStructureDefinitions.hpp>
#include <libserver/registry/CourseRegistry.hpp>

#include <chrono>
#include <functional>
#include <unordered_set>

namespace server
{

class RaceNetworkHandler;
class Room;

class RaceInstance
{
public:
  using Clock = std::chrono::steady_clock;
  
  enum class Stage
  {
    Waiting,
    Loading,
    Racing,
    Finishing,
  };

  struct Parameters
  {
    //! A game mode of the race.
    protocol::GameMode gameMode{};
    //! A team mode of the race.
    protocol::TeamMode teamMode{};
    //! A map block ID of the race.
    registry::MapBlockId mapBlockId{};
    //! A mission ID of the race.
    uint16_t missionId{};
    //! A UID of the master.
    data::Uid masterUid{};
  };

  explicit RaceInstance(
    RaceNetworkHandler& raceDirector,
    uint32_t roomUid);
  ~RaceInstance() = default;

  void GetRoom(const std::function<void(Room&)>& consumer);
  void GetRoom(const std::function<void(const Room&)>& consumer) const;

  bool Start(const Parameters& parameters);
  void Stop();

  //! LOA-fix (R11-3a, round11, backlog #20 п.4): перевзводит дедлайн стадии
  //! загрузки от ТЕКУЩЕГО момента. Зовётся из планировщика гонки ровно там, где
  //! клиентам уходит AcCmdCRStartRaceNotify — то есть в момент, когда они
  //! реально начинают грузить карту. Без этого бюджет загрузки урезался на
  //! величину countdown комнаты (~5.3 c из 60). No-op вне стадии Loading.
  void ArmLoadingDeadline();

  void Tick();

  uint32_t GetRoomUid();

  const Parameters& GetParameters() const;

  [[nodiscard]] registry::GameModeId GetGameModeId() const;
  [[nodiscard]] registry::MapBlockId GetMapBlockId() const;

  [[nodiscard]] Clock::time_point GetLoadingStartTimePoint() const noexcept;
  [[nodiscard]] Clock::time_point GetRaceStartTimePoint() const noexcept;

  [[nodiscard]] Stage GetStage() const noexcept;
  [[nodiscard]] Clock::time_point GetStageTimeoutTimePoint() const noexcept;

  //! Номер ТЕКУЩЕГО заезда в этой комнате (LOA-fix R67-1, backlog #128b).
  //! Отложенные джобы командного калибра захватывают его ПО ЗНАЧЕНИЮ и молча
  //! ничего не делают, если к моменту исполнения номер уже другой. Подробности
  //! — у поля `_raceEpoch` и в `RaceNetworkHandler::HandleTeamGauge`.
  [[nodiscard]] uint32_t GetRaceEpoch() const noexcept;

  [[nodiscard]] tracker::RaceTracker& GetTracker();
  [[nodiscard]] const tracker::RaceTracker& GetTracker() const;

  [[nodiscard]] protocol::BonusCourseType GetBonusCourseType() const noexcept;
  void SetBonusCourseType(protocol::BonusCourseType type) noexcept;

  //! AI-соперник соло-заезда (R56, #61).
  //!
  //! ★ЖИВЁТ ТОЛЬКО ЗДЕСЬ, и это главное свойство раунда. В `RaceTracker` его
  //! нет и быть не должно: трекер читают пути, которые ПИШУТ в данные —
  //! морковки/опыт/уровень в `RaceInstance::Stop`, травма лошади (S2), победы
  //! лошади (race-stats), пер-заездная телеметрия (R24), месть, дейлик-квесты
  //! (S8b), назначение мастера комнаты. Синтетическая сущность в трекере
  //! означала бы список мест, где её надо вычесть обратно; отсутствие в
  //! трекере означает, что вычитать негде.
  //!
  //! Движение ботов сервер не считает вообще: их водит САМ КЛИЕНТ по своим
  //! таблицам `SAI_<тир><N>Param`. Серверу остаётся объявить ростер и назвать
  //! времена на финише.
  struct AiRacer
  {
    //! Object id, выданный ТЕМ ЖЕ аллокатором, что и у живых гонщиков
    //! (`RaceTracker::ReserveOid`), — поэтому столкнуться с ними не может.
    tracker::Oid oid{tracker::InvalidEntityOid};
    //! Идентификатор P2P-ретрансляции. Боты по сети не ездят, но поле в пакете
    //! обязано быть заполнено значением, не совпадающим с чужим.
    race::P2dId p2dId{};
    //! Отображаемое имя из ростера клиента.
    std::string name;
    //! Индекс личности 1..7 внутри тира: им клиент выбирает таблицу вождения.
    uint8_t personality{};
    //! Выдуманное время финиша. До финиша живого игрока — «не доехал».
    uint32_t courseTime{tracker::InvalidCourseTime};
    //! Наибольший прогресс по трассе, о котором клиент отчитался ЗА ЭТОГО бота
    //! (R62, #196). Та же шкала, что у `Racer::raceProgress`: клиент нормирует
    //! её в 0..1 на всю дистанцию.
    //!
    //! ★ЗАЧЕМ ПОЛЕ ВООБЩЕ ПОЯВИЛОСЬ. До этого раунда сервер о ботах не знал
    //! НИЧЕГО и время финиша выдумывал симметрично вокруг времени игрока
    //! (±5 %). Симметрия и есть баг #196: пять исходов из одиннадцати делают
    //! бота быстрее, то есть в среднем ТРИ бота из семи «обгоняли» человека,
    //! который на экране пересёк черту первым. Прогресс — единственная
    //! наблюдаемая величина, связывающая табло с тем, что игрок реально видел.
    //!
    //! ★ХРАПОВИК, ТОЛЬКО ВВЕРХ. Понижение — это либо потерянный пакет, либо
    //! попытка занизить соперника; ни то ни другое двигать итог не должно.
    //!
    //! ★ЗНАЧЕНИЕ НЕДОВЕРЕННОЕ, И ЭТО ДОПУСТИМО РОВНО ЗДЕСЬ: бот не существует
    //! ни в трекере, ни в `raceResult` (R56 §1), поэтому единственное, на что
    //! влияет это число, — строка бота на табло соло-заезда. Ни морковок, ни
    //! опыта, ни мастера комнаты, ни квестов оно не касается.
    //!
    //! Живёт РОВНО один заезд: ростер чистится в `Start()`.
    float raceProgress{};
  };

  [[nodiscard]] std::vector<AiRacer>& GetAiRacers() noexcept;
  [[nodiscard]] const std::vector<AiRacer>& GetAiRacers() const noexcept;

  //! Постоянные object id ботов этой КОМНАТЫ (R56, #61).
  //!
  //! ★Почему на комнату, а не на заезд (находка ревью R56-i1). `Oid` — это
  //! `uint16_t`, и счётчик трекера только растёт. Выдавай мы боту свежий id
  //! каждый заезд — семь штук за заезд, — счётчик обернулся бы примерно на
  //! девятитысячном заезде в одной комнате, и бот получил бы id живого игрока:
  //! клиент перепутал бы их молча. Держим по набору на комнату — ровно так же,
  //! как трекер держит id живого игрока: один раз выдал и переиспользует.
  [[nodiscard]] std::vector<tracker::Oid>& GetAiOids() noexcept;

  //! «Этот object id принадлежит боту ТЕКУЩЕГО заезда?» (R57, #195).
  //!
  //! Ботов ведёт клиент, и он же присылает пакеты от их имени. Для сервера это
  //! законно и означает ровно одно: состояния для такого участника у него нет,
  //! поэтому делать нечего. Предикат существует, чтобы отличить это от подлога
  //! чужого гонщика, который по-прежнему обязан быть шумным.
  //!
  //! ★Смотрит на РОСТЕР ЗАЕЗДА (`_aiRacers`), а НЕ на `_aiOids`: последний
  //! живёт на комнату и в заезде без ботов дал бы тихий пропуск там, где нужна
  //! жалоба.
  [[nodiscard]] bool IsAiRacerOid(tracker::Oid oid) const noexcept;

  //! Запоминает прогресс бота, о котором отчитался клиент (R62, #196).
  //!
  //! Зовётся на КАЖДОМ пакете позиции и САМ решает, относится ли `oid` к боту:
  //! для живого гонщика это пустая операция. Значение санитизируется и
  //! храповиком идёт только вверх.
  //!
  //! ★ПОЧЕМУ ВЫЗОВ СТОИТ СНАРУЖИ ВЕТКИ ЖАЛОБЫ, А НЕ ВНУТРИ НЕЁ. Форма гарда
  //! R57 (`if (command.X != racer.oid) { if (IsAiRacerOid(command.X)) return; …`)
  //! проверяется оракулом #195 ДОСЛОВНО и по числу совпадений: любой оператор,
  //! вставленный между условием и `return`, ломает доказательство тотальности
  //! прошлого раунда. Самогардящийся вызов до сравнения оставляет ту форму
  //! нетронутой ([[dont-trade-success-path-for-failure-path]]).
  void NoteAiRacerProgress(tracker::Oid oid, float progress) noexcept;

private:
  void TickLoading();
  void TickRacing();
  void TickFinishing();

  void TickActiveRaceContent();
  void TickItemSpawners();
  void TickMagicGauge();

  void PrepareGameMode();
  void PickRandomMapFromCourse();
  void PrepareMap();

public:
  // todo: this needs to be fixed
  void PickRandomItemFromDeck(tracker::RaceTracker::ItemDeck& deck);

public:
  //! LOA-fix (R68, backlog #5/#99): раскладывает КВЕСТОВЫЕ предметы гонщикам.
  //!
  //! ★ПОЧЕМУ НЕ ВНУТРИ `Start()`, где живёт `PrepareItemDecks`. Обычные деки
  //! общие для всех и от состава заезда не зависят; квестовые — пер-гонщиковые
  //! и зависят от того, какие квесты несёт КАЖДЫЙ. А гонщиков в трекер
  //! добавляет вызывающий (`RaceNetworkHandler::HandleStartRace`) уже ПОСЛЕ
  //! `Start()` — внутри `Start()` раскладывать было бы просто некому.
  //!
  //! Идемпотентности не требует и не даёт: зовётся ровно один раз за заезд,
  //! сразу после `AddRacer`. Гонщики создаются связкой `Clear()`+`AddRacer`
  //! заново, поэтому на входе вектор предметов всегда пуст.
  void PrepareQuestItems();

private:
  void PrepareItemDecks();

  const uint32_t _roomUid{};

  //! The race parameters.
  Parameters _parameters;

  registry::GameModeId _gameModeId{};
  registry::Course::GameModeInfo _gameModeInfo;
  registry::MapBlockId _mapBlockId{};
  registry::Course::MapBlockInfo _mapBlockInfo;
  
  //! A time point of when the race started loading.
  Clock::time_point _loadingStartTimePoint{
    Clock::time_point::max()};
  //! A time point of when the race started.
  Clock::time_point _raceStartTimePoint{
    Clock::time_point::max()};

  //! The current stage of the race.
  Stage _stage{Stage::Waiting};
  //! A time point of when the stage timeout occurs.
  Clock::time_point _stageTimeoutTimePoint{
    Clock::time_point::max()};

  //! Номер заезда в этой комнате (LOA-fix R67-2, backlog #128b). Растёт на
  //! КАЖДОМ успешном `Start()`, то есть меняется ровно тогда, когда в комнате
  //! начинается НОВЫЙ заезд.
  //!
  //! ★ЗАЧЕМ. `RaceInstance` живёт вместе с КОМНАТОЙ и переиспользуется из
  //! заезда в заезд, а отложенные джобы командного калибра
  //! (`RaceNetworkHandler::HandleTeamGauge`) переискивают инстанс ТОЛЬКО по
  //! `roomUid`. То есть личность КОМНАТЫ они проверяют, а личности ЗАЕЗДА у
  //! них не было вовсе — и джоб, запланированный в заезде N (внешний +1.5 с,
  //! внутренний +7-10 с), исполнялся в заезде N+1 той же комнаты: рассылал
  //! чужой спур и снимал ЧУЖУЮ блокировку калибра.
  //!
  //! ★ПОЧЕМУ НА ИНСТАНСЕ, А НЕ В ТРЕКЕРЕ И НЕ У ГОНЩИКА. Соседний счётчик
  //! поколений `RaceTracker::Racer::effectGenerations` живёт У ГОНЩИКА и
  //! умирает вместе с ним в `RaceTracker::Clear()` — эффектам этого хватает,
  //! потому что и сам эффект пер-гонщиковый. Командное состояние
  //! (`RaceTracker::blueTeam`/`redTeam`) переживает гонщиков, значит и метка,
  //! которой отличают заезды, обязана жить ДОЛЬШЕ трекера — на самом инстансе.
  //!
  //! ★НОЛЬ БЕЗОПАСЕН, ОБОРОТ НЕДОСТИЖИМ. Захватить эпоху можно только внутри
  //! заезда, то есть после хотя бы одного `Start()`, где значение уже >= 1.
  //! Оборот 32-битного счётчика потребовал бы 2^32 заездов в ОДНОЙ комнате
  //! внутри 10-секундного окна джоба.
  uint32_t _raceEpoch{};

  //! A race object tracker.
  tracker::RaceTracker _tracker;

  //! Ростер AI-соперников текущего соло-заезда (R56, #61). Пуст всегда, кроме
  //! соло-заездов. Чистится в `Start()` — там же, где чистится трекер, поэтому
  //! рассинхрону между ними взяться неоткуда (апстрим-баг B1).
  std::vector<AiRacer> _aiRacers;

  //! Object id ботов, выданные ОДИН РАЗ на время жизни комнаты (R56, #61).
  //!
  //! ★Живёт ДОЛЬШЕ ростера и намеренно НЕ чистится в `Start()`: ростер — это
  //! состояние заезда, а личности ботов постоянны, как личности игроков. См.
  //! `GetAiOids`.
  std::vector<tracker::Oid> _aiOids;

  protocol::BonusCourseType _bonusCourseType{
    protocol::BonusCourseType::None};

  RaceNetworkHandler& _raceNetworkHandler;
};

} // namespace server

#endif // RACEINSTANCE_HPP
