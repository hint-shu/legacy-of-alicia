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

#ifndef RANCHDIRECTOR_HPP
#define RANCHDIRECTOR_HPP

#include "libserver/network/command/CommandDeferrer.hpp"
#include "libserver/util/LogThrottle.hpp"
#include "libserver/util/Scheduler.hpp"
#include "server/Config.hpp"
#include "server/ranch/AchievementNotifyHold.hpp"
#include "server/ranch/BreedingMarket.hpp"
#include "server/tracker/RanchTracker.hpp"

#include "libserver/network/command/CommandServer.hpp"
#include "libserver/network/command/proto/CommonMessageDefinitions.hpp"
#include "libserver/network/command/proto/RanchMessageDefinitions.hpp"
#include "libserver/network/command/proto/RaceMessageDefinitions.hpp"
#include "libserver/network/command/proto/CommonMessageDefinitions.hpp"

// LOA-fix (R34-9, round34, backlog #96): <atomic> — под
// _connectSeqCounter (std::atomic<std::uint64_t>), <cstdint> — под сам
// std::uint64_t (connectSeq в ClientContext и значение очереди отложенных
// разрывов). В пине оба заголовка отсутствовали; полагаться на транзитивный
// подтяг через <mutex>/spdlog нельзя — стандарт этого не обещает.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
// LOA-fix (R48-1, #58/R2-D): <span> и <string_view> — под список доказанных
// условий достижения, который хук передаёт системе (SendAchievementEvent).
#include <span>
// LOA-fix (R72-fix-3, находка Codex 2): <string> — под значение очереди
// отложенных уведомлений о представлении (_pendingIntroductionNotifies).
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace server
{

class ServerInstance;

class RanchDirector final
  : public CommandServer::EventHandlerInterface
{
public:
  //!
  explicit RanchDirector(ServerInstance& serverInstance);

  void Initialize();
  void Terminate();
  void Tick();

  std::vector<data::Uid> GetOnlineCharacters();

  void HandleNetworkTick() override;
  void HandleClientConnected(ClientId clientId) override;
  void HandleClientDisconnected(ClientId client) override;

  //! LOA-fix (R21, round21, backlog #95): ОСВЕЖАЕТ (и только освежает) отметку
  //! «этот клиент говорил по ранч-сокету». Зовётся из CommandServer на КАЖДУЮ
  //! входящую дейтаграмму ранч-канала (включая AcCmdCRHeartbeat 0x12, у которой
  //! нет зарегистрированного хендлера) — до разбора команды.
  //! ★UPDATE-ONLY: записи НЕ СОЗДАЁТ. Создание живёт ровно в одном месте —
  //! HandleEnterRanch. Иначе частый штамп воскрешал бы запись, только что
  //! стёртую teardown'ом с ЧУЖОГО потока, и плодил сирот (разбор гонки — в
  //! шапке раунда 21).
  //! ★ПОТОК: ранч-поток. Читать _clients здесь законно (тот же поток, что и все
  //! остальные хендлеры ранчо); мьютекс реестра берётся ОТДЕЛЬНО и только на
  //! операцию с map — вложенных локов нет.
  //! Не бросает: контекст ищем через find, отсутствие клиента = тихий выход
  //! (исключение отсюда порвало бы соединение в read-loop сети).
  //! @param clientId ID клиента ранч-канала.
  void HandleClientActivity(ClientId clientId) override;

  //! LOA-fix (R34-3, round34, backlog #96): ПРОСИТ порвать ранч-соединение
  //! персонажа. ★НЕ РВЁТ ЕГО ЗДЕСЬ И НЕ ТРОГАЕТ _clients.
  //! Зовётся с ЧУЖИХ потоков: лобби-СЕТЕВОГО (LobbyNetworkHandler::
  //! HandleNetworkTick, сетевой таймаут) и лобби-директорского (ChatSystem,
  //! GM-бан и GM-сброс персонажа). До раунда 34 метод прямо оттуда обходил и
  //! правил _clients директора ранчо, которая не защищена ничем и принадлежит
  //! ранч-сетевому потоку, — гонка #96. Заодно DisconnectClient чужим потоком
  //! доходил до Server::_clients, ВТОРОЙ незащищённой map.
  //! ★ПОЧЕМУ МАРШРУТИЗАЦИЯ, А НЕ МЬЮТЕКС: GetClientContext отдаёт
  //! ClientContext& НАРУЖУ, ссылка переживает любой лок внутри аксессора и
  //! разъезжается по хендлерам — «залочить map» здесь = ложная безопасность.
  //! ★Реальный разрыв делает DrainPendingDisconnects на ранч-сетевом тике,
  //! задержка ≤1 c. Возврата «получилось/нет» у метода не было и раньше.
  //! ★РЕКОННЕКТ ВНУТРИ ОКНА ЗАКРЫТ ШТАМПОМ ПОКОЛЕНИЯ. Очередь ключуется
  //! characterUid'ом, поэтому сам по себе он не отличает старую сессию от
  //! той, которой персонаж успел переподключиться за ≤1 c (на стенде такой
  //! реконнект укладывается в 0.25 c, а _clients спокойно держит ДВЕ
  //! аутентифицированные записи одного персонажа — дедупа по characterUid при
  //! входе нет). Поэтому метод снимает СНИМОК _connectSeqCounter (requestSeq)
  //! и кладёт его в очередь вместе с UID, а дренаж рвёт только соединение с
  //! connectSeq <= requestSeq — родившееся ДО просьбы. Свежее соединение имеет
  //! connectSeq > requestSeq и не может быть выбрано; старое, наоборот, будет
  //! найдено. Для GM-бана это принципиально: иначе забаненный, успевший
  //! переподключиться, терял свежее соединение, а СТАРОЕ переживало дренаж —
  //! обход санкции.
  void Disconnect(data::Uid characterUid);

  //! LOA-fix (R21, round21, backlog #95): «персонаж прямо сейчас активен на
  //! ранчо?» — свежесть последнего входящего ранч-пакета. ЕДИНСТВЕННЫЙ метод
  //! директора ранчо, который законно звать с ЧУЖОГО потока (лобби-тик): он
  //! трогает только _ranchActivityMutex и _ranchActivity и НИКОГДА _clients.
  //! ★Лок ЛИСТОВОЙ: под ним не берётся другой лок и не делается вызовов наружу.
  //! @param characterUid UID персонажа.
  //! @param freshness Окно свежести (старше — считаем неактивным).
  //! @returns true, если ранч-сокет персонажа говорил не позже freshness назад.
  [[nodiscard]] bool IsCharacterActiveOnRanch(
    data::Uid characterUid,
    std::chrono::seconds freshness) const;

  //! LOA-fix (R21, round21, backlog #95): ПЕРИОДИЧЕСКАЯ УБОРКА реестра
  //! активности — сносит все записи старше maxAge. Это ГАРАНТИЯ ВЕРХНЕЙ ГРАНИЦЫ
  //! памяти: выходные пути (disconnect/leave/kick) убирают записи быстро, но
  //! teardown умеет бежать на ЧУЖОМ потоке, и редкая гонка enter-vs-disconnect
  //! способна оставить сироту, которую не подберёт ни один выходной путь.
  //! Живому игроку уборка не грозит: он переставляет метку каждые ≤8.5 c, а
  //! maxAge берётся заведомо больше окна свежести (90 c против 30 c).
  //! ★Как и IsCharacterActiveOnRanch, законно зовётся с ЧУЖОГО потока
  //! (лобби-тик): трогает только _ranchActivityMutex и _ranchActivity.
  //! ★Лок ЛИСТОВОЙ: обход + erase, никаких вызовов наружу. O(n), n ≤ числа
  //! игроков, стоящих на ранчо.
  //! @param maxAge Возраст, начиная с которого запись считается мусором.
  void SweepRanchActivity(std::chrono::seconds maxAge);

  //! LOA-fix (R72-fix-3, round72, backlog #170-item-28, находка Codex 2):
  //! ★ПРОСИТ разослать новое представление. НЕ РАССЫЛАЕТ ЗДЕСЬ и НЕ ТРОГАЕТ
  //! `_clients`/`_ranches`.
  //!
  //! Единственный вызывающий — `LobbyNetworkHandler::HandleSetIntroduction`,
  //! то есть ЛОББИ-СЕТЕВОЙ поток. А `_clients` и `_ranches` принадлежат
  //! РАНЧ-СЕТЕВОМУ (см. их объявления ниже): ранч-поток в тот же момент
  //! законно вставляет и стирает записи в HandleClientConnected /
  //! HandleClientDisconnected / HandleRanchLeave. Читать их с лобби-потока —
  //! гонка по неатомарной unordered_map: rehash под чужим обходом это порча
  //! памяти, а не «редкий сбой». До этой правки метод делал ровно это, а
  //! документация обещала обратное — ложное обещание о потокобезопасности
  //! живёт тихо и подводит следующий раунд, который на него обопрётся.
  //!
  //! ★ПРИЁМ ТОТ ЖЕ, ЧТО У `Disconnect` (R34-4): кладём ЗНАЧЕНИЯ в очередь под
  //! ЛИСТОВЫМ локом и уходим; рассылку делает DrainPendingIntroductionNotifies
  //! на ранч-сетевом тике (задержка ≤1 c). Мьютексом `_clients` эту гонку не
  //! лечат: `GetClientContext` отдаёт ссылку НАРУЖУ, она переживает любой лок
  //! внутри аксессора — это была бы ложная безопасность.
  //!
  //! ★ОЧЕРЕДЬ КЛЮЧУЕТСЯ ПЕРСОНАЖЕМ, поэтому повторные правки представления
  //! схлопываются в последнюю: верхняя граница памяти = число персонажей
  //! онлайн, а не число присланных пакетов. Рассылка и так «лучшая попытка»,
  //! промежуточные редакции текста никому не нужны.
  //! @param characterUid UID персонажа, сменившего представление.
  //! @param introduction Новый текст представления.
  void BroadcastSetIntroductionNotify(
    uint32_t characterUid,
    const std::string& introduction);

  //! LOA (R70, backlog #58): ★ПРОСИТ ДОСТАВИТЬ ЗНАЧКИ ЗАЕЗДА ПО РАНЧЕВОМУ
  //! СОЕДИНЕНИЮ. НЕ отправляет здесь и НЕ трогает `_clients`.
  //!
  //! Зовут отсюда с потока ГОНОЧНОГО директора (`RaceInstance::Stop`), а
  //! `_clients` принадлежит РАНЧ-СЕТЕВОМУ — приём тот же, что у
  //! `BroadcastSetIntroductionNotify` (R72-fix-3) и `Disconnect` (R34-4):
  //! кладём ЗНАЧЕНИЯ под ЛИСТОВЫМ локом и уходим, доставку делает
  //! `DrainPendingAchievementNotifies` на ранч-сетевом тике (задержка ≤1 c).
  //!
  //! ★ПОЧЕМУ ВООБЩЕ РАНЧЕВОЕ СОЕДИНЕНИЕ, А НЕ ГОНОЧНОЕ (пара опкод/сокет).
  //! `AcCmdRCAchievementUpdateNotify` (0xe4) до этого раунда отправлял ровно
  //! один путь — `SendAchievementEvent` ранч-директора, в ранчевую очередь; это
  //! ЕДИНСТВЕННАЯ пара «опкод — сокет», доказанная живым клиентом. Структура
  //! лежит в `RaceMessageDefinitions.hpp`, но этот файл — свалка, а не
  //! утверждение о канале. Первая редакция R70 слала 0xe4 по ГОНОЧНОМУ сокету;
  //! что делает клиент с командой, которой его гоночный диспетчер не знает, не
  //! измерено, а прецедент в проекте плохой — на неверно смаршрутизированном
  //! `UpdateGameMoney` клиент рвал соединение. Значок не имеет права стоить
  //! игроку результата заезда (R48-11), поэтому канал берётся доказанный.
  //!
  //! ★ОЧЕРЕДЬ КОПИТ, А НЕ СХЛОПЫВАЕТ (в отличие от очереди представлений):
  //! каждый 0xe4 — отдельный значок/тир, потерять промежуточный значит потерять
  //! попап.
  //!
  //! ★НОТИФИКАЦИЯ ПРИДЕРЖИВАЕТСЯ ДО ВОЗВРАЩЕНИЯ НА РАНЧО (R70-fix-7, решение
  //! лида по находке fix-6). Настоящий клиент ЗАКРЫВАЕТ ранчевую ногу ровно
  //! при входе в заезд (захват; то же делает сессия тестера), поэтому в момент
  //! `RaceInstance::Stop()` ранчевого соединения у игрока НЕТ, и прежнее
  //! правило «нет клиента — выбросить» теряло бы попап почти всегда. Теперь
  //! записи лежат в `AchievementNotifyHold` (срок и потолок — там же) и
  //! уходят на ближайшем ранч-тике ПОСЛЕ того, как персонаж снова окажется на
  //! ранчо. Если контекст есть уже сейчас — уйдут немедленно, как и раньше.
  //! @param characterUid UID персонажа-получателя.
  //! @param notifies Готовые нотификации (значения, без ссылок и ClientId).
  void QueueAchievementNotifies(
    data::Uid characterUid,
    std::vector<protocol::AcCmdRCAchievementUpdateNotify> notifies) noexcept;

  //!
  void BroadcastUpdateMountInfoNotify(
    data::Uid characterUid,
    data::Uid rancherUid,
    data::Uid horseUid);

  //! LOA-fix (SYNC-1): рассылает смену лошади всем клиентам ранча и обновляет
  //! трекер лошадей инстанса (новая лошадь уходит из загона, прошлая — в загон).
  //! @param clientId Клиент, сменивший лошадь.
  //! @param previousMountUid UID прошлой лошади.
  //! @param newMountUid UID новой лошади.
  void BroadcastMountChange(
    ClientId clientId,
    data::Uid previousMountUid,
    data::Uid newMountUid);

  //! LOA-fix (SYNC-3): собирает ростерную запись персонажа для рассылок
  //! (тот же состав полей, что и в EnterRanchOK), без исключений.
  //! @param characterUid UID персонажа.
  //! @param rancherUid UID владельца ранча, в инстансе которого он находится.
  //! @param protocolCharacter Заполняемая ростерная запись.
  //! @returns true, если запись пригодна к отправке.
  bool BuildRanchCharacterInfo(
    data::Uid characterUid,
    data::Uid rancherUid,
    protocol::RanchCharacter& protocolCharacter);

  //! LOA-fix (SYNC-3): переспавнивает ростерную запись игрока у остальных
  //! клиентов ранча (Leave+Enter), чтобы применились изменившиеся данные
  //! персонажа — прежде всего ник.
  //! @param clientId Клиент, чью запись обновляем.
  void BroadcastRanchCharacterRefresh(ClientId clientId);

  //! Send a RequestUser notification to a character connected to this director.
  void SummonCharacter(
    data::Uid characterUid,
    bool force,
    std::string characterName,
    uint32_t roomUid,
    uint32_t ranchUid) noexcept;

  //! Show popup notification for client indicating a new item in storage, by character UID
  void SendStorageNotification(
    data::Uid characterUid,
    protocol::AcCmdCRRequestStorage::Category category);

  void BroadcastChangeAgeNotify(
    data::Uid characterUid,
    data::Uid rancherUid,
    protocol::AcCmdCRChangeAge::Age age);

  void BroadcastHideAgeNotify(
    data::Uid characterUid,
    data::Uid rancherUid,
    protocol::AcCmdCRHideAge::Option option);

  void BroadcastUpdateGuildMemberGradeNotify(
    data::Uid guildUid,
    data::Uid characterUid,
    protocol::GuildRole guildRole);

  void SendDailyQuestNotificationToCharacter(
    data::Uid characterUid,
    const protocol::AcCmdRCUpdateDailyQuestNotify& updateNotify);

  //! Сообщает системе достижений о серверном событии и отправляет персонажу
  //! то, что она вернула.
  //! @param characterUid Персонаж, совершивший действие.
  //! @param achievementEvent Номер шины событий (UserAchvEvent).
  //! @param provenConditions Условия, проверенные на месте события; пусто, если
  //!        достижение просто считает события.
  //! ★Не бросает НИКОГДА (R48-11, находка ревью): значок не имеет права стоить
  //! игроку действия, которое он уже совершил.
  void SendAchievementEvent(
    data::Uid characterUid,
    uint16_t achievementEvent,
    std::span<const std::string_view> provenConditions = {}) noexcept;

  void SendGuildInviteDeclined(
    data::Uid characterUid,
    data::Uid inviterCharacterUid,
    std::string inviterCharacterName,
    data::Uid guildUid);

  void SendGuildInviteAccepted(
    data::Uid guildUid,
    data::Uid characterUid,
    const std::string& newMemberCharacterName);

  void AddRanchHorse(
    data::Uid rancherUid,
    data::Uid horseUid);

  ServerInstance& GetServerInstance();
  Config::Ranch& GetConfig();

  //! LOA-fix (batch1 task3 → R10, round10): server-authoritative суточный сброс
  //! дейлик-квестов. Если группа дейликов персонажа сбрасывалась раньше
  //! текущего игрового дня (граница 06:00 UTC) — ОДНОЙ мутацией очищает три
  //! слота целей, обнуляет rewardPoints, снимает carrotsClaimed и
  //! dailyRewardClaimed и ставит lastResetDate = сегодня. Атомарность
  //! обязательна: раздельное снятие dailyRewardClaimed при живом вчерашнем
  //! прогрессе — это эксплойт бесплатной награды дня (C2/E1). Идемпотентен:
  //! повторный вызов в тот же игровой день — no-op. No-op и если у персонажа
  //! ещё нет группы дейликов.
  //!
  //! PUBLIC, А НЕ PRIVATE (R10): зовётся из лобби —
  //! LobbyNetworkHandler::SendLoginOK и ::HandleRequestDailyQuestList, то есть
  //! ДО того, как клиент снимет снапшот дневных целей. Из HandleEnterRanch
  //! вызов убран: там уже поздно, а push-канала «набор сброшен» в протоколе
  //! нет. Тред-безопасно: метод трогает только записи DataDirector (мутации под
  //! их собственным shared_mutex) и не касается ни _clients, ни планировщика
  //! ранча, так что вызов с лобби-треда законен — ровно как у
  //! HorseSystem::PromoteMaturedFoals рядом.
  //! @param characterUid UID персонажа.
  void ResetDailyQuestsIfNeeded(data::Uid characterUid);

private:
  struct ClientContext
  {
    //! User name.
    std::string userName;
    //! Whether the client is authenticated.
    bool isAuthenticated{false};
    //! Unique ID of the client's character.
    data::Uid characterUid{data::InvalidUid};
    //! LOA-fix (R34-8, round34, backlog #96): ★НОМЕР ПОКОЛЕНИЯ СОЕДИНЕНИЯ.
    //! Уникальный возрастающий номер, выданный этому соединению в момент
    //! accept'а (HandleClientConnected, R34-7) из _connectSeqCounter. Ноль =
    //! запись ещё не проштампована (в норме недостижимо: штамп ставится сразу
    //! после try_emplace, до того как клиент может что-либо прислать).
    //! ★ПОТОКИ: пишется РОВНО ОДИН РАЗ и только ранч-сетевым потоком при
    //! подключении; читается тем же ранч-сетевым потоком в
    //! DrainPendingDisconnects. Чужие потоки его НЕ ЧИТАЮТ (им хватает
    //! снимка счётчика), поэтому новой межпотоковой шаренной памяти поле не
    //! добавляет — она вся сидит в atomic-счётчике.
    //! ★ЗАЧЕМ: отличить соединение, родившееся ДО просьбы о разрыве
    //! (connectSeq <= requestSeq — его и рвём), от реконнекта, родившегося
    //! ПОСЛЕ (connectSeq > requestSeq — не трогаем). См. R34-2/R34-4.
    //! ★И БОЛЬШЕ ТОГО (skip-if-newer-survives, R34-4): само наличие такого
    //! реконнекта ОТМЕНЯЕТ разрыв старых соединений этого персонажа целиком —
    //! их teardown чистит ранч-состояние по characterUid и затёр бы живую
    //! сессию реконнекта. Подробности в DrainPendingDisconnects.
    std::uint64_t connectSeq{};
    //! Unique ID of the owner of the ranch the client is visiting.
    data::Uid visitingRancherUid{data::InvalidUid};

    uint8_t busyState{0};
    //! Whether there's a pending breeding failure card waiting to be claimed
    bool hasPendingFailureCard{false};
    //! Current breeding failure card type.
    protocol::BreedingFailureCardType pendingCardType{};
    //! Fee paid for the failed breeding; scales the failure-card reward grade.
    uint32_t pendingFailureCardSpend{0};

    //! The client's foals still maturing into adults, each mapped to the time
    //! it becomes an adult. Rebuilt on ranch entry and appended to when a foal
    //! is bred; the maturity sweep only looks at these, and only does a record
    //! lookup once an entry's deadline has passed.
    std::unordered_map<data::Uid, data::Clock::time_point> maturingFoals;

    //! Number of times a deferred ranch entry has been retried while waiting
    //! for horse records to load. Capped so the client isn't stuck forever.
    uint32_t enterRanchDeferAttempts{0};

    //! Number of times a deferred breeding attempt has been retried 
    uint32_t tryBreedingDeferAttempts{0};
  };

  struct RanchInstance
  {
    //! A world tracker of the ranch.
    tracker::RanchTracker tracker;
    //! A set of clients connected to the ranch.
    std::unordered_set<ClientId> clients;
    //! LOA-fix (SYNC-9): последний пространственный снапшот каждого персонажа
    //! инстанса, ключ — UID персонажа. Позиции/поворота в protocol::RanchCharacter
    //! нет, они ходят только снапшотами, поэтому вошедшему гостю их неоткуда
    //! взять — кэшируем и проигрываем при входе. Чистится на выходе с ранча.
    std::unordered_map<data::Uid, protocol::RanchCommandRanchSnapshotNotify> snapshots;
  };

  //! Get client context.
  //! @param clientId Id of the client.
  //! @param requireAuthentication Require the client to be authorized.
  //! @returns Client context.
  [[nodiscard]] ClientContext& GetClientContext(ClientId clientId, bool requireAuthentication = true);

  //! Get the client ID by the character's unique ID.
  //! @param characterUid UID of the character.
  //! @returns Client ID.
  [[nodiscard]] ClientId GetClientIdByCharacterUid(data::Uid characterUid);

  //! LOA-fix (R72-2, round72, backlog #170-item-28): ПОИСК, КОТОРЫЙ УМЕЕТ НЕ
  //! НАЙТИ.
  //!
  //! Прежний `GetClientContextByCharacterUid` бросал «Character not associated
  //! with any client» на ШТАТНОМ состоянии «персонаж не на ранчо», а его
  //! единственный вызывающий (`BroadcastSetIntroductionOnRanchThread`) — путь,
  //! управляемый клиентом: одна строка [error] на каждый пакет 0x171.
  //!
  //! ★ОТДАЁТ И ClientId. Не удобство: skip-self обязан сравнивать ClientId с
  //! ClientId, а взять его иначе значило бы ЕЩЁ РАЗ перебирать `_clients` —
  //! ту самую чужепоточную карту, число обращений к которой раунд обязан не
  //! увеличивать.
  //!
  //! @param characterUid UID искомого персонажа.
  //! @param clientId Получает ClientId найденного клиента; не трогается, если
  //!        клиент не найден.
  //! @returns nullptr, если аутентифицированного ранч-клиента у персонажа нет.
  //! ★Указатель действителен ровно до следующей мутации `_clients`; звать и
  //! использовать только на РАНЧ-СЕТЕВОМ потоке. ★С R72-fix-3 это обещание
  //! ВЫПОЛНЕНО и вызывающим: единственный вызывающий —
  //! DrainPendingIntroductionNotifies, а он живёт на ранч-сетевом тике. До
  //! правки сюда приходил лобби-поток, и предупреждение было ложным.
  [[nodiscard]] ClientContext* TryGetClientContextByCharacterUid(
    data::Uid characterUid,
    ClientId& clientId);

  //! Handles the ranch enter command.
  //! @param clientId ID of the client
  //! @param command Command
  //! @returns True if the command should be deferred and retried (a required
  //!          horse record was not yet available), false otherwise.
  bool HandleEnterRanch(
    ClientId clientId,
    const protocol::AcCmdCREnterRanch& command);

  void HandleRanchLeave(
    ClientId clientId);

  //! Rebuilds the client's set of maturing foals, promoting any that already
  //! reached the grow-up duration to adults in the data store. Called on ranch
  //! entry so foals matured while away are adults before the snapshot is sent.
  //! @param characterUid UID of the owning character.
  //! @param clientContext Context of the owning client to refresh.
  void RefreshMaturingFoals(data::Uid characterUid, ClientContext& clientContext);

  //! Promotes matured foals for every character currently standing on their
  //! own ranch, announcing the grow-up to that ranch. Only the tracked
  //! maturing foals are inspected.
  //! LOA-fix (R35-1, round35, backlog #124): ★КОНТРАКТ ПОТОКА. Метод обходит и
  //! МУТИРУЕТ `_clients` (в том числе ClientContext::maturingFoals), трогает
  //! `_ranches` и `_commandServer` ⇒ звать его можно ТОЛЬКО с РАНЧ-СЕТЕВОГО
  //! потока, то есть из HandleNetworkTick (R35-4). До раунда 35 он бежал прямо
  //! из задачи планировщика на потоке ранч-ДИРЕКТОРА и рвал гонку с
  //! HandleClientConnected/HandleClientDisconnected.
  void RunFoalMaturityCheck();

  //! Queues the next foal maturity check on the scheduler, re-scheduling
  //! itself so the sweep runs on a fixed interval.
  //! LOA-fix (R35-1, round35, backlog #124): задача планировщика больше НЕ
  //! исполняет проход — она только звонит будильником (`_foalMaturityCheckDue`)
  //! и перезаводит себя. Сам проход снимает ранч-сетевой поток.
  void ScheduleFoalMaturityCheck() noexcept;

  //! Announces that a foal grew up to an adult to the owning client and the
  //! visitors of its ranch.
  //! @param clientId ID of the owning client.
  //! @param rancherUid UID of the ranch the horse resides on.
  //! @param characterUid UID of the owning character.
  //! @param horseUid UID of the horse that grew up.
  void AnnounceFoalGrewUp(
    ClientId clientId,
    data::Uid characterUid,
    data::Uid horseUid);

  void ReturnHorseToNature(
    data::Uid characterUid,
    data::Uid horseUid,
    std::string userName,
    bool breedingAbandon);

  void HandleChat(
    ClientId clientId,
    const protocol::AcCmdCRRanchChat& command);

  void HandleSnapshot(
    ClientId clientId,
    const protocol::AcCmdCRRanchSnapshot& command);

  void HandleEnterBreedingMarket(
    ClientId clientId,
    const protocol::AcCmdCREnterBreedingMarket& command);

  void HandleSearchStallion(
    ClientId clientId,
    const protocol::AcCmdCRSearchStallion& command);

  void HandleRegisterStallion(
    ClientId clientId,
    const protocol::AcCmdCRRegisterStallion& command);

  void SendRegisterStallionCancel(
    ClientId clientId);

  void HandleUnregisterStallion(
    ClientId clientId,
    const protocol::AcCmdCRUnregisterStallion& command);

  void SendUnregisterStallionCancel(
    ClientId clientId);

  void HandleUnregisterStallionEstimateInfo(
    ClientId clientId,
    const protocol::AcCmdCRUnregisterStallionEstimateInfo& command);

  void HandleCheckStallionCharge(
    ClientId clientId,
    const protocol::AcCmdCRCheckStallionCharge& command);

  //! Handles the breeding attempt command.
  //! @param clientId ID of the client.
  //! @param command Command.
  //! @returns True if the command should be deferred and retried
  bool HandleTryBreeding(
    ClientId clientId,
    const protocol::AcCmdCRTryBreeding& command);

  //! Rolls a breeding bonus based on the stallion's grade.
  //! @param stallionGrade Grade of the stallion.
  //! @returns The rolled bonus, or a default (id 0) bonus if none activated.
  [[nodiscard]] protocol::BreedingBonus RollBreedingBonus(uint32_t stallionGrade);

  //! Calculates the breeding success rate (0-100).
  //! @param stallionGrade Grade of the stallion.
  //! @param stallionBreedingCount Lifetime breeding count of the stallion.
  //! @param bonus Rolled breeding bonus.
  //! @returns Success rate as a percentage capped at 100.
  [[nodiscard]] uint32_t CalculateBreedingSuccessRate(
    uint32_t stallionGrade,
    uint32_t stallionBreedingCount,
    const protocol::BreedingBonus& bonus);

  //! Creates a foal from a successful breeding, spawns it on the ranch and fills
  //! the breeding response.
  //! @param clientId Client that triggered the breeding.
  //! @param clientContext Context of the breeding client (owner and visited ranch).
  //! @param command Breeding command (holds mare/stallion horse UIDs).
  //! @param bonus Rolled breeding bonus.
  //! @param response Response to populate with the foal's details.
  //! @returns UID of the created foal.
  data::Uid CreateBredFoal(
    ClientId clientId,
    const ClientContext& clientContext,
    const protocol::AcCmdCRTryBreeding& command,
    const protocol::BreedingBonus& bonus,
    protocol::RanchCommandTryBreedingOK& response);

  void HandleBreedingAbandon(
    ClientId clientId,
    const protocol::AcCmdCRBreedingAbandon& command);

  //!
  void HandleBreedingWishlist(
    ClientId clientId,
    const protocol::AcCmdCRBreedingWishlist& command);

  //!
  void HandleBreedingFailureCard(
    ClientId clientId,
    const protocol::AcCmdCRBreedingFailureCard& command);

  //!
  void HandleBreedingFailureCardChoose(
    ClientId clientId,
    const protocol::AcCmdCRBreedingFailureCardChoose& command);

  //!
  void HandleCmdAction(
    ClientId clientId,
    const protocol::AcCmdCRRanchCmdAction& command);

  //!
  void HandleRanchStuff(
    ClientId clientId,
    const protocol::RanchCommandRanchStuff& command);

  //!
  void HandleUpdateBusyState(
    ClientId clientId,
    const protocol::RanchCommandUpdateBusyState& command);

  //!
  void HandleUpdateMountNickname(
    ClientId clientId,
    const protocol::AcCmdCRUpdateMountNickname& command);

  void SendUpdateMountNicknameCancel(
    ClientId clientId,
    protocol::HorseNicknameUpdateError reason);

  //!
  void HandleRequestStorage(
    ClientId clientId,
    const protocol::AcCmdCRRequestStorage& command);

  //!
  void HandleGetItemFromStorage(
    ClientId clientId,
    const protocol::AcCmdCRGetItemFromStorage& command);

  //!
  void HandleRequestNpcDressList(
    ClientId clientId,
    const protocol::RanchCommandRequestNpcDressList& requestNpcDressList);

  void HandleWearEquipment(
    ClientId clientId,
    const protocol::AcCmdCRWearEquipment& command);

  void HandleRemoveEquipment(
    ClientId clientId,
    const protocol::AcCmdCRRemoveEquipment& command);

  void HandleCreateGuild(
    ClientId clientId,
    const protocol::RanchCommandCreateGuild& command);

  void HandleRequestGuildInfo(
    ClientId clientId,
    const protocol::RanchCommandRequestGuildInfo& command);

  void HandleWithdrawGuild(
    ClientId clientId,
    const protocol::AcCmdCRWithdrawGuildMember& command);

  void HandleUpdatePet(
    ClientId clientId,
    const protocol::AcCmdCRUpdatePet& command);

  void SendUpdatePetCancel(
    ClientId clientId,
    const protocol::AcCmdRCUpdatePetCancel& command);

  void HandleIncubateEgg(
    ClientId clientId,
    const protocol::AcCmdCRIncubateEgg& command);

  void HandleBoostIncubateInfoList(
    ClientId clientId,
    const protocol::AcCmdCRBoostIncubateInfoList& command);
  
  void HandleBoostIncubateEgg(
    ClientId clientId,
    const protocol::AcCmdCRBoostIncubateEgg& command);

  void HandleRequestPetBirth(
    ClientId clientId,
    const protocol::AcCmdCRRequestPetBirth& command);

  void HandlePetBornResult(
    ClientId clientId,
    const protocol::AcCmdCRPetBornResult& command);

  void HandleUserPetInfos(
    ClientId clientId,
    const protocol::RanchCommandUserPetInfos& command);

  //! Confirm whether item in the shop can be purchased or gifted.
  void HandleConfirmItem(
    ClientId clientId,
    const protocol::AcCmdCRConfirmItem& command);

  //! Confirm whether item set in the shop can be purchased or gifted.
  void HandleConfirmSetItem(
    ClientId clientId,
    const protocol::AcCmdCRConfirmSetItem& command);

  //! Broadcasts an equipment update of the character owned by the client
  //! to the currently connected ranch.
  //! @param clientId ID of the client.
  void BroadcastEquipmentUpdate(
    ClientId clientId);

  bool HandleUseFoodItem(
    data::Uid mountUid,
    data::Uid characterUid,
    data::Tid usedItemTid,
    protocol::AcCmdCRUseItemOK& response);

  bool HandleUseCleanItem(
    data::Uid mountUid,
    data::Uid characterUid,
    data::Tid usedItemTid,
    protocol::AcCmdCRUseItemOK& response);
  
  bool HandleUsePlayItem(
    data::Uid characterUid,
    data::Uid mountUid,
    data::Tid usedItemTid,
    protocol::AcCmdCRUseItem::PlaySuccessLevel successLevel,
    protocol::AcCmdCRUseItemOK& response);

  bool HandleUseCureItem(
    data::Uid characterUid,
    data::Uid mountUid,
    data::Tid usedItemTid,
    protocol::AcCmdCRUseItemOK& response);

  void HandleUseItem(
    ClientId clientId,
    const protocol::AcCmdCRUseItem& command);

  void HandleHousingBuild(
    ClientId clientId,
    const protocol::AcCmdCRHousingBuild& command);

  void HandleHousingRepair(
    ClientId clientId,
    const protocol::AcCmdCRHousingRepair& command);
  
  void HandleOpCmd(ClientId clientId,
    const protocol::AcCmdCROpCmd& command);

  void HandleRequestLeagueTeamList(ClientId clientId,
    const protocol::RanchCommandRequestLeagueTeamList& command);

  bool HandleMountFamilyTree(ClientId clientId,
    const protocol::AcCmdCRMountFamilyTree& command);

  void HandleRecoverMount(
    ClientId clientId,
    const protocol::AcCmdCRRecoverMount command);

  void HandleCheckStorageItem(
    ClientId clientId,
    const protocol::AcCmdCRCheckStorageItem command);

  void HandleChangeAge(
    ClientId clientId,
    const protocol::AcCmdCRChangeAge command);

  void HandleHideAge(
    ClientId clientId,
    const protocol::AcCmdCRHideAge command);

  void HandleStatusPointApply(
    ClientId clientId,
    const protocol::AcCmdCRStatusPointApply command);

  //! LOA (batch2): learn/advance a care skill (0x277). Phase 1 = learn+persist,
  //! no strict validation, no effects. newRank = current learned rank + 1.
  void HandleStudyCareSkill(
    ClientId clientId,
    const protocol::AcCmdCRStudyCareSkill& command);

  //! LOA (batch2): reset learned care skills (0x27d). Phase 1 = clear learned
  //! ranks, no 30000-carrot charge.
  void HandleResetCareSkill(
    ClientId clientId,
    const protocol::AcCmdCRResetCareSkill& command);

  void HandleChangeSkillCardPreset(
    ClientId clientId,
    const protocol::AcCmdCRChangeSkillCardPreset command);

  void HandleGetGuildMemberList(
    ClientId clientId,
    const protocol::AcCmdCRGuildMemberList& command);

  void HandleRequestGuildMatchInfo(
    ClientId clientId,
    const protocol::AcCmdCRRequestGuildMatchInfo& command);
  
  void HandleUpdateGuildMemberGrade(
    ClientId clientId,
    const protocol::AcCmdCRUpdateGuildMemberGrade& command);

  void HandleInviteToGuild(
    ClientId clientId,
    const protocol::AcCmdCRInviteGuildJoin& command);
    
  void HandleGetEmblemList(
    ClientId clientId,
    const protocol::AcCmdCREmblemList& command);

  void HandleChangeNickname(
    ClientId clientId, 
    const protocol::AcCmdCRChangeNickname& command);

  void HandleUpdateDailyQuest(
    ClientId clientId,
    const protocol::AcCmdCRUpdateDailyQuest& command);

  void HandleRegisterDailyQuestGroup(
    ClientId clientId,
    const protocol::AcCmdCRRegisterDailyQuestGroup& command);

  void HandleRequestDailyQuestReward(
      ClientId clientId,
      const protocol::AcCmdCRRequestDailyQuestReward& command);

  void HandleRegisterQuest(
      ClientId clientId,
      const protocol::AcCmdCRRegisterQuest& command);

  void HandleRequestQuestReward(
    ClientId clientId,
    const protocol::AcCmdCRRequestQuestReward& command);

  void HandleGiveupQuest(
    ClientId clientId,
    const protocol::AcCmdCRGiveupQuest& command);

  void SendChangeNicknameCancel(
    ClientId clientId,
    protocol::ChangeNicknameError reason);

  void HandleBuyOwnItem(
    ClientId clientId,
    const protocol::AcCmdCRBuyOwnItem& command);

  void HandleSendGift(
    ClientId clientId,
    const protocol::AcCmdCRSendGift& command);

  void HandleOpenRandomBox(
    ClientId clientId,
    const protocol::AcCmdCROpenRandomBox& command);

  void HandleUpdateMountInfo(
    ClientId clientId,
    const protocol::AcCmdCRUpdateMountInfo command);

  void HandlePasswordAuth(
    ClientId clientId,
    const protocol::AcCmdCRPasswordAuth command);

  //! Ranch clients can only invite characters in other ranches.
  void HandleInviteUser(
    ClientId clientId,
    const protocol::AcCmdCRInviteUser& command);

  void HandleRequestUser(
    ClientId clientId,
    const protocol::AcCmdCRRequestUser& command);

  void HandleBreedingTakeMoney(
    ClientId clientId,
    const protocol::AcCmdCRBreedingTakeMoney& command);

  void HandleExpandMountSlot(
    ClientId clientId,
    const protocol::AcCmdCRExpandMountSlot& command);

  void HandleBreedingWishlistAdd(
    ClientId clientId,
    const protocol::AcCmdCRBreedingWishlistAdd& command);

  void HandleBreedingWishlistDelete(
    ClientId clientId,
    const protocol::AcCmdCRBreedingWishlistDel& command);

  //! LOA-fix (R34-1, round34, backlog #96): СЛИВ ОЧЕРЕДИ ОТЛОЖЕННЫХ РАЗРЫВОВ.
  //! Зовётся ТОЛЬКО из HandleNetworkTick, то есть строго на РАНЧ-СЕТЕВОМ потоке
  //! (CommandServer::_serverThread) — единственном, которому законно трогать
  //! _clients и _commandServer.
  //! ★Очередь снимается целиком под _pendingDisconnectsMutex, лок ОТПУСКАЕТСЯ,
  //! и только потом рвутся соединения: под листовым локом не должно быть ни
  //! одного вызова наружу (дисциплина раунда 21).
  //! ★DisconnectClient СИНХРОННО стирает запись из _clients (Client::End()
  //! зовёт OnClientDisconnected в том же стеке), поэтому ClientId выбирается
  //! ОТДЕЛЬНЫМ проходом и разрыв делается уже ПОСЛЕ выхода из обхода map.
  void DrainPendingDisconnects();

  //! LOA-fix (R72-fix-3, round72, находка Codex 2): СЛИВ ОЧЕРЕДИ ОТЛОЖЕННЫХ
  //! УВЕДОМЛЕНИЙ О ПРЕДСТАВЛЕНИИ.
  //! Зовётся ТОЛЬКО из HandleNetworkTick, то есть строго на РАНЧ-СЕТЕВОМ
  //! потоке — единственном, которому законно трогать `_clients`, `_ranches` и
  //! `_commandServer`.
  //! ★Очередь снимается целиком, лок ОТПУСКАЕТСЯ, и только потом идут поиски и
  //! отправки: под листовым локом не должно быть ни одного вызова наружу
  //! (дисциплина раундов 21/34).
  void DrainPendingIntroductionNotifies();

  //! LOA (R70, backlog #58): СЛИВ ОЧЕРЕДИ ОТЛОЖЕННЫХ НОТИФИКАЦИЙ ДОСТИЖЕНИЙ.
  //! Зовётся ТОЛЬКО из HandleNetworkTick, то есть строго на РАНЧ-СЕТЕВОМ
  //! потоке — единственном, которому законно трогать `_clients` и
  //! `_commandServer`.
  //! ★Очередь снимается целиком, лок ОТПУСКАЕТСЯ, и только потом идут поиски и
  //! отправки (дисциплина раундов 21/34/72).
  //! ★Персонаж без ранчевого соединения НЕ ТЕРЯЕТ попап: его записи остаются в
  //! удержании до следующего входа на ранчо (R70-fix-7). Выбрасываются они
  //! только по сроку `AchievementNotifyHold::Ttl()` — и МОЛЧА в смысле
  //! «без жалобы»: «игрок не вернулся» — штатное состояние, а не сбой; строка
  //! в лог при этом пишется, потому что она несёт СЧЁТ (оракул стенда судит
  //! «после протухания удержано ноль» числом, а не отсутствием строк).
  //! ★ПОРЯДОК В ТИКЕ (R70-fix-8, находка Codex 6 BLOCK-1): вызов стоит ПОСЛЕ
  //! `_enterRanchDeferrer.Tick()`, а отдаёт попап ТОЛЬКО соединению с
  //! `visitingRancherUid != data::InvalidUid` — то есть тому, чей вход РЕАЛЬНО
  //! завершён. `isAuthenticated` этого не доказывает: он ставится ещё до
  //! отсрочки и переживает `LeaveRanch`.
  void DrainPendingAchievementNotifies();

  //! LOA-fix (R72-fix-3, round72, находка Codex 2): САМА РАССЫЛКА нового
  //! представления — тело, которое до правки жило прямо в
  //! `BroadcastSetIntroductionNotify` и исполнялось на ЛОББИ-потоке.
  //! ★ПОТОК: только РАНЧ-СЕТЕВОЙ. Единственный вызывающий —
  //! DrainPendingIntroductionNotifies.
  //! @param characterUid UID персонажа, сменившего представление.
  //! @param introduction Новый текст представления.
  void BroadcastSetIntroductionOnRanchThread(
    uint32_t characterUid,
    const std::string& introduction);

  //! LOA-fix (R38-1, round38, backlog #131): ДЕДУП СЕССИЙ ОДНОГО ПЕРСОНАЖА.
  //! Рвёт ВСЕ уже аутентифицированные ранч-соединения персонажа, КРОМЕ
  //! указанного нового. Зовётся ровно из одной точки — HandleEnterRanch, сразу
  //! после успешной авторизации по OTP и ДО того, как новая сессия начнёт
  //! раскладывать своё ранч-состояние.
  //! ★ПОТОК: только РАНЧ-СЕТЕВОЙ (владелец _clients и _commandServer).
  //! HandleEnterRanch приходит либо прямо из обработчика команды ранч-канала,
  //! либо из _enterRanchDeferrer.Tick() внутри HandleNetworkTick — оба на этом
  //! потоке. Гонки #96 здесь нет по построению.
  //! ★★РВЁТ ПРЯМЫМ DisconnectClient, НЕ ЧЕРЕЗ Disconnect(characterUid).
  //! Disconnect() кладёт просьбу в _pendingDisconnects со снимком
  //! requestSeq = _connectSeqCounter.load(). connectSeq НОВОЙ сессии B выдан ещё
  //! на accept'е и УЖЕ учтён этим счётчиком, то есть B.connectSeq <= requestSeq.
  //! Значит дренаж снёс бы КАЖДУЮ сессию этого персонажа с
  //! connectSeq <= requestSeq — ВКЛЮЧАЯ саму легитимную B, — и сделал бы это
  //! ПОЗЖЕ, отдельным тиком (асинхронно), когда B уже финализировала своё
  //! ранч-состояние: ровно инверсия #96. Поэтому нужен прямой СИНХРОННЫЙ
  //! DisconnectClient: рвётся ТОЛЬКО заранее существовавший стейл-набор и
  //! целиком ДО того, как B положит своё состояние.
  //! ★2 ШАГА: DisconnectClient синхронно доходит до _clients.erase, поэтому
  //! ClientId сначала собираются ПО ЗНАЧЕНИЮ отдельным проходом, и только
  //! потом идут разрывы — вне обхода map.
  //! ★СЕМАНТИКА: побеждает ПОСЛЕДНИЙ вход (старая сессия вытесняется, новая
  //! пускается) — переподключение обязано вытеснить собственного призрака.
  //! @param newClientId Клиент, который прямо сейчас входит (его не трогаем).
  //! @param characterUid UID персонажа, предъявленный и подтверждённый OTP.
  void DedupeStaleCharacterSessions(
    ClientId newClientId,
    data::Uid characterUid);

  //!
  ServerInstance& _serverInstance;
  //!
  CommandServer _commandServer;

  //! The breeding market system.
  BreedingMarket _breedingMarket;

  //!
  std::unordered_map<ClientId, ClientContext> _clients;
  //!
  std::unordered_map<data::Uid, RanchInstance> _ranches;

  //! LOA-fix (R21-1c, round21, backlog #95): ★ЛИСТОВОЙ мьютекс реестра
  //! активности. Единственная разделяемая между потоками структура директора
  //! ранчо. ПРАВИЛО, нарушение которого = deadlock ранчо↔лобби: под этим локом
  //! НЕ берётся ни один другой лок и НЕ делается ни одного вызова наружу
  //! (_clients, _ranches, _commandServer, _scheduler). Только сама map.
  //! mutable — чтобы IsCharacterActiveOnRanch остался const-методом.
  mutable std::mutex _ranchActivityMutex;

  //! LOA-fix (R21-1c, round21, backlog #95): момент последнего ВХОДЯЩЕГО пакета
  //! ранч-канала по каждому персонажу, стоящему на ранчо. Пишется ранч-потоком
  //! (HandleClientActivity), читается лобби-потоком (IsCharacterActiveOnRanch) —
  //! обе операции строго под _ranchActivityMutex. Запись заводится лениво, на
  //! первом же пакете аутентифицированного клиента, уже вошедшего на ранчо, и
  //! стирается на КАЖДОМ выходе (disconnect / leave / kick) — стирание, а не
  //! свежесть, даёт верхнюю границу памяти.
  std::unordered_map<data::Uid, std::chrono::steady_clock::time_point> _ranchActivity;

  //! LOA-fix (R34-2, round34, backlog #96): ★ЛИСТОВОЙ мьютекс очереди
  //! отложенных разрывов. Второй (после _ranchActivityMutex) и последний
  //! межпотоковый замок директора ранчо. Правило то же, нарушение = deadlock:
  //! под этим локом НЕ берётся ни один другой лок и НЕ делается ни одного
  //! вызова наружу (_clients, _ranches, _commandServer, _scheduler).
  std::mutex _pendingDisconnectsMutex;

  //! LOA-fix (R34-2, round34, backlog #96): ★ШТАМП ПОКОЛЕНИЯ СОЕДИНЕНИЙ.
  //! Монотонный счётчик «рождений» ранч-соединений. Каждое принятое соединение
  //! получает в HandleClientConnected уникальный возрастающий номер
  //! (ClientContext::connectSeq, см. R34-7), каждая просьба о разрыве
  //! запоминает СНИМОК счётчика на момент просьбы (requestSeq, см. R34-4).
  //! ★ЗАЧЕМ: очередь ключуется characterUid'ом, а он у персонажа один на все
  //! его соединения — по нему нельзя отличить СТАРУЮ сессию от той, которой
  //! персонаж успел переподключиться внутри окна слива. Номер поколения
  //! отличает их по построению: connectSeq <= requestSeq ⇒ соединение
  //! РОДИЛОСЬ ДО просьбы (кандидат на разрыв); connectSeq > requestSeq ⇒
  //! родилось ПОСЛЕ, просьба была не про него (не трогаем).
  //! ★ПОТОКИ: инкремент делает ранч-сетевой поток, load — чужие потоки в
  //! Disconnect ⇒ std::atomic. Операции ДЕФОЛТНЫЕ (seq_cst) осознанно: цена
  //! одного fetch_add на соединение ничтожна, а правильность важнее.
  //! Переполнение uint64 недостижимо (это счётчик TCP-accept'ов).
  std::atomic<std::uint64_t> _connectSeqCounter{0};

  //! LOA-fix (R35-2, round35, backlog #124): ★БУДИЛЬНИК ПРОВЕРКИ ЖЕРЕБЯТ.
  //! Ставится в true задачей планировщика на потоке ранч-ДИРЕКТОРА
  //! (ScheduleFoalMaturityCheck, раз в FoalMaturityCheckInterval = 60 c),
  //! снимается exchange(false) РАНЧ-СЕТЕВЫМ потоком в HandleNetworkTick,
  //! который тут же и делает проход RunFoalMaturityCheck.
  //! ★ПОЧЕМУ ФЛАГ, А НЕ ОЧЕРЕДЬ (в отличие от _pendingDisconnects, R34-2): у
  //! этой работы нет полезной нагрузки. Директорский поток не может вычислить,
  //! КОМУ слать уведомление, не читая `_clients` — а это ровно та гонка,
  //! которую раунд 35 и убирает. Единица работы одна: «пройди всех».
  //! ★ЛИСТОВОЙ ПРИМИТИВ: под atomic по построению не берётся ни один лок и не
  //! делается ни одного вызова наружу — дисциплина раундов 21/34 соблюдена в
  //! пределе.
  //! ★СХЛОПЫВАНИЕ БЕЗОБИДНО: два звонка подряд дают один проход, а проход и так
  //! обрабатывает ВСЕ созревшие к `now` записи.
  //! Операции ДЕФОЛТНЫЕ (seq_cst) осознанно: цена одного exchange в секунду
  //! ничтожна, а правильность важнее.
  std::atomic<bool> _foalMaturityCheckDue{false};

  //! Признак того, что перезавод таймера созревания НЕ УДАЛСЯ (R55, #179
  //! часть 5). Читается сетевым тиком ранча, который повторяет попытку.
  //!
  //! ★Флаг существует только чтобы отказ не был ТИХИМ: без него неудачная
  //! постановка означала бы навсегда потерянный таймер без единого следа.
  std::atomic<bool> _foalMaturityRearmFailed{false};

  //! LOA-fix (R34-2, round34, backlog #96): очередь отложенных разрывов —
  //! UID персонажа → requestSeq (снимок _connectSeqCounter на момент просьбы).
  //! Просьбы приходят с ЧУЖИХ потоков: лобби-тик (сетевой таймаут) через
  //! RanchDirector::Disconnect и GM-команды бана/сброса из ChatSystem.
  //! Пишется чужими потоками, читается и опустошается РАНЧ-СЕТЕВЫМ потоком в
  //! HandleNetworkTick (задержка ≤1 c — период Server::TickLoop).
  //! map, а не vector: повторные просьбы про один UID схлопываются, что даёт
  //! верхнюю границу памяти = числу онлайн-персонажей; при схлопывании
  //! хранится МАКСИМАЛЬНЫЙ requestSeq — порог «до какого поколения рвать».
  //! ★ПОЧЕМУ НЕ ПАРА {characterUid, clientId}: у чужого потока clientId нет,
  //! он знает только персонажа, а достать id = снова читать чужую _clients —
  //! ровно та гонка, которую раунд 34 и убирает.
  std::unordered_map<data::Uid, std::uint64_t> _pendingDisconnects;

  //! LOA-fix (R72-fix-3, round72, находка Codex 2): ★ЛИСТОВОЙ мьютекс очереди
  //! отложенных уведомлений о представлении. Третий и последний межпотоковый
  //! замок директора ранчо. Правило то же, нарушение = deadlock: под ним НЕ
  //! берётся ни один другой лок и НЕ делается ни одного вызова наружу
  //! (`_clients`, `_ranches`, `_commandServer`, `_scheduler`).
  std::mutex _pendingIntroductionNotifiesMutex;

  //! LOA-fix (R72-fix-3, round72, находка Codex 2): очередь отложенных
  //! уведомлений — UID персонажа → его новое представление.
  //! Пишется ЛОББИ-сетевым потоком (BroadcastSetIntroductionNotify), читается и
  //! опустошается РАНЧ-СЕТЕВЫМ в HandleNetworkTick (задержка ≤1 c).
  //! ★ТОЛЬКО ЗНАЧЕНИЯ: ни указателей, ни ссылок, ни ClientId. ClientId у
  //! чужого потока взять неоткуда, не читая `_clients`, — а это ровно та
  //! гонка, которую правка убирает.
  //! map, а не vector: повторные правки одного персонажа схлопываются, что и
  //! даёт верхнюю границу памяти = числу персонажей онлайн.
  std::unordered_map<data::Uid, std::string> _pendingIntroductionNotifies;

  //! LOA (R70, backlog #58): ★ЛИСТОВОЙ мьютекс очереди отложенных нотификаций
  //! достижений. Правило то же, нарушение = deadlock: под ним НЕ берётся ни
  //! один другой лок и НЕ делается ни одного вызова наружу.
  std::mutex _pendingAchievementNotifiesMutex;

  //! LOA (R70, backlog #58; удержание — R70-fix-7): придержанные нотификации
  //! достижений — UID персонажа → его нотификации в порядке появления.
  //! Пишется потоком ГОНОЧНОГО директора (`QueueAchievementNotifies`),
  //! разбирается РАНЧ-СЕТЕВЫМ в HandleNetworkTick.
  //! ★ТОЛЬКО ЗНАЧЕНИЯ: ни указателей, ни ссылок, ни ClientId.
  //! ★Срок жизни берётся из конфига (`ranch.achievementNotifyHoldSeconds`,
  //! по умолчанию 900 с = 15 минут). Причина знобки — не «настраиваемость ради
  //! настраиваемости»: стенд обязан УВИДЕТЬ протухание, а ждать пятнадцать
  //! минут в каждой клетке матрицы нельзя. Прод остаётся на 15 минутах.
  AchievementNotifyHold _pendingAchievementNotifies{std::chrono::seconds(900)};

  //! LOA (R70-fix-7): дроссель жалобы о вытеснении по потолку. Вытеснение —
  //! это НЕ штатное состояние (значит, игрок набрал больше двух заездов
  //! попаданий, ни разу не вернувшись на ранчо), но и не повод залить лог:
  //! источник события — поведение игрока.
  util::LogThrottle _achievementHoldOverflowThrottle{std::chrono::seconds(60)};

  //! A command deferrer for the `AcCmdCRMountFamilyTree` command.
  CommandDeferrer<protocol::AcCmdCRMountFamilyTree> _mountFamilyTreeDeferrer;

  //! A command deferrer for the `AcCmdCREnterRanch` command.
  CommandDeferrer<protocol::AcCmdCREnterRanch> _enterRanchDeferrer;

  //! A command deferrer for the `AcCmdCRTryBreeding` command.
  CommandDeferrer<protocol::AcCmdCRTryBreeding> _tryBreedingDeferrer;

  //! Drives periodic ranch chores, such as the foal maturity sweep.
  Scheduler _scheduler;
};

} // namespace server

#endif // RANCHDIRECTOR_HPP
