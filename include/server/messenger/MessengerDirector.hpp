//
// Created by rgnter on 30/08/2025.
//

#ifndef MESSENGERDIRECTOR_HPP
#define MESSENGERDIRECTOR_HPP

#include "server/ranch/BreedingMarket.hpp"

#include <libserver/network/chatter/ChatterServer.hpp>
#include <libserver/data/DataDefinitions.hpp>

#include "server/Config.hpp"
#include "server/chat/ChatSocketReapRule.hpp"

#include <libserver/util/LogThrottle.hpp>

#include <functional>
#include <mutex>
#include <shared_mutex>

namespace server
{

//! Messenger client OTP constant
constexpr uint32_t MessengerOtpConstant = 0xBABF67A0;

class ServerInstance;

class MessengerDirector
  : private IChatterServerEventsHandler
{
private:
  struct ClientContext
  {
    //! Whether the client is authenticated.
    bool isAuthenticated{false};
    //! The otp code used to authenticate this client.
    std::optional<uint32_t> otpCode{};
    //! Unique ID of the client's character.
    data::Uid characterUid{data::InvalidUid};
    //! Online presence of the client.
    protocol::Presence presence{};

    //! LOA (R80-3, round80, backlog #235): МОМЕНТ ПОДКЛЮЧЕНИЯ. Основание P1, и
    //! оно НЕ ОСВЕЖАЕТСЯ активностью — иначе шумящий сканер бессмертен.
    chat::ReapClock::time_point connectedAt{};
    //! LOA (R80-3, round80, backlog #235): последняя входящая порция данных.
    //! Основание грейса P2 и (если включён) P3.
    chat::ReapClock::time_point lastActivity{};
  };

  struct Client
  {
    network::ClientId clientId{};
    ClientContext clientContext{};
  };

public:
  explicit MessengerDirector(ServerInstance& serverInstance);

  void Initialize();
  void Terminate();

  //! Get messenger config.
  //! @return Messenger config.
  [[nodiscard]] Config::Messenger& GetConfig();

  // === LOA-fix (R82, round82, director-clients-lock-hardening): КОНТРАКТ R64-3 ===
  //! ★НАРУЖУ УХОДИТ КОПИЯ, А НЕ ССЫЛКА В КАРТУ. Прежний `ClientContext&` жил у
  //! вызывающего через выходы в чужой код (`GetCharacter`, `GetDataDirector`,
  //! `QueueCommand`), а чужой поток фазы 1 гашения (`CloseSessionsOfCharacter`
  //! с потока лобби, GM-бан с потока чат-команд) в это же время правил те же
  //! поля. Защищать надо не момент поиска, а ВСЁ ВРЕМЯ ИСПОЛЬЗОВАНИЯ; растянуть
  //! замок на чужой код нельзя (нерекурсивный `shared_mutex`, класс R59/R78),
  //! значит единственный выход — не выпускать ссылку наружу вовсе.
  //!
  //! ★ЭТО РОВНО ТА РАБОТА, КОТОРУЮ R78 ОТЛОЖИЛ ЯВНЫМ ТЕКСТОМ (врезка ниже, у
  //! `_clientsMutex`), и ровно тот контракт, которому для лобби был посвящён
  //! отдельный раунд R64-3/#215 (`LobbyNetworkHandler.hpp:188-201`).
  //!
  //! ★ПОЧЕМУ КОНТРАКТ, А НЕ ПОЧИНКА ПО МЕСТАМ. Шестнадцать читателей «скопируй
  //! скаляр под коротким замком» — это СПИСОК МЕСТ, который отстаёт от кода:
  //! следующий обработчик снова возьмёт ссылку и никто не покраснеет. Возврат
  //! копией — тотальный инвариант, и его умеет утверждать один гейт
  //! (`tools/check_messenger_context_copy.py`).
  //!
  //! ★КОПИЯ ДЁШЕВА: `bool`, `optional<uint32_t>`, `Uid`, `Presence`, два
  //! `time_point` — ни одного владеющего аллокацией поля.
  [[nodiscard]] ClientContext GetClientContext(
    network::ClientId clientId,
    bool requireAuthentication = true);

  [[nodiscard]] std::optional<Client> GetClientByCharacterUid(const data::Uid characterUid) const;

  //! LOA (R78-fix4, round78, backlog #255, находка ревю W1): ЗАКРЫТЬ ВСЕ
  //! мессенджер-сессии персонажа. Зовёт лобби, когда снимает долгоживущий ключ
  //! на выходе игрока из игры.
  //!
  //! ★ЗАЧЕМ. Снятие ключа (`OtpSystem::RevokeLtk`) трогает только карту ключей,
  //! а право обслуживать мессенджер живёт в `clientContext.isAuthenticated`, и
  //! после входа `OtpSystem` не опрашивается больше НИКОГДА. Значит уже
  //! ОТКРЫТАЯ сессия снятие ключа переживала: держатель подсмотренного ключа
  //! продолжал читать и слать письма от имени игрока, который уже вышел.
  //! Ключ и сессия обязаны умирать вместе.
  //!
  //! ★ЗВАТЬ ТОЛЬКО ВНЕ ЗАМКОВ вызывающего: метод берёт СВОЙ замок карты
  //! клиентов, а закрытие соединений синхронно возвращается в
  //! `HandleClientDisconnected` (класс R59 — нерекурсивный `shared_mutex`).
  //! ★ЗВАТЬ МОЖНО С ЛЮБОГО ПОТОКА, И ЭТО ГЛАВНОЕ СВОЙСТВО МЕТОДА (R78-fix7).
  //! Сам он ничего не рвёт: правит значения под замком карты и КЛАДЁТ
  //! соединения в очередь. Настоящее закрытие делает `HandleNetworkTick()` на
  //! потоке ЧАТ-СЕРВЕРА — не `Tick()`, который приходит с потока задач
  //! директора (уточнено по NIT ревю #3 №4). Разбор — у `_pendingDisconnects`.
  void CloseSessionsOfCharacter(data::Uid characterUid);
  [[nodiscard]] bool IsCharacterOnline(const data::Uid characterUid) const;
  void SendStallionReward(
    data::Uid characterUid,
    data::Uid horseUid,
    const BreedingMarket::Earnings& earnings);

  void Tick();

private:
  void HandleClientConnected(network::ClientId clientId) override;
  void HandleClientDisconnected(network::ClientId clientId) override;

  //! LOA (R80-3, round80, backlog #235): штамп «пир говорит». Приходит с потока
  //! чат-сервера на КАЖДУЮ входящую порцию данных, ДО разбора кадра.
  void HandleClientActivity(network::ClientId clientId) override;

  // Handler methods for chatter commands
  void HandleChatterLogin(
    network::ClientId clientId,
    const protocol::ChatCmdLogin& command);

  void HandleChatterBuddyAdd(
    network::ClientId clientId,
    const protocol::ChatCmdBuddyAdd& command);

  void HandleChatterBuddyAddReply(
    network::ClientId clientId,
    const protocol::ChatCmdBuddyAddReply& command);

  void HandleChatterBuddyDelete(
    network::ClientId clientId,
    const protocol::ChatCmdBuddyDelete& command);

  void HandleChatterBuddyMove(
    network::ClientId clientId,
    const protocol::ChatCmdBuddyMove& command);

  void HandleChatterGroupAdd(
    network::ClientId clientId,
    const protocol::ChatCmdGroupAdd& command);

  void HandleChatterGroupRename(
    network::ClientId clientId,
    const protocol::ChatCmdGroupRename& command);

  void HandleChatterGroupDelete(
    network::ClientId clientId,
    const protocol::ChatCmdGroupDelete& command);

  void HandleChatterLetterList(
    network::ClientId clientId,
    const protocol::ChatCmdLetterList& command);

  void HandleChatterLetterSend(
    network::ClientId clientId,
    const protocol::ChatCmdLetterSend& command);

  void HandleChatterLetterRead(
    network::ClientId clientId,
    const protocol::ChatCmdLetterRead& command);

  void HandleChatterLetterDelete(
    network::ClientId clientId,
    const protocol::ChatCmdLetterDelete& command);

  void HandleChatterUpdateState(
    network::ClientId clientId,
    const protocol::ChatCmdUpdateState& command);

  void HandleChatterChatInvite(
    network::ClientId clientId,
    const protocol::ChatCmdChatInvite& command);

  void HandleChatterGameInvite(
    network::ClientId clientId,
    const protocol::ChatCmdGameInvite& command);

  void HandleChatterChannelInfo(
    network::ClientId clientId,
    const protocol::ChatCmdChannelInfo& command);

  void HandleChatterGuildLogin(
    network::ClientId clientId,
    const protocol::ChatCmdGuildLogin& command);

  //! LOA (R78, round78, backlog #255): вход персонажа оставляет ровно одну
  //! привязанную к нему мессенджер-сессию. Правило вытеснения вынесено в
  //! `MessengerSessionEviction.hpp` ИМЕННО затем, чтобы его можно было
  //! проверить юнит-тестом, не поднимая сервер; здесь остаётся вторая фаза —
  //! закрытие отвязанных соединений ВНЕ обхода карты.
  void EvictOtherSessionsOfCharacter(
    network::ClientId keepClientId,
    data::Uid characterUid);

  //! Фаза 2 ДЛЯ ПУТИ ВХОДА: закрыть отвязанные соединения прямо здесь.
  //! ★ЗВАТЬ ТОЛЬКО С ПОТОКА МЕССЕНДЖЕРА. Путь входа (`HandleChatterLogin`)
  //! исполняется на нём по построению, поэтому синхронное закрытие тут
  //! законно — это ровно тот приём, что стоял в базе в ветке отказа
  //! авторизации. Для ЧУЖИХ потоков есть очередь, см. `_pendingDisconnects`.
  //! ★ВСЕГДА вне замка карты — см. разбор у `_clientsMutex`.
  void DisconnectUnboundSessions(
    const std::vector<network::ClientId>& unbound,
    const char* reason,
    data::Uid characterUid);

  //! Сетевой тик чат-сервера. Приходит на потоке чат-сервера.
  void HandleNetworkTick() override;

  //! LOA (R78-fix8, round78, backlog #255, находка ревю #3 WARN-1): рассылка
  //! присутствия персонажа, НЕ ЗАВИСЯЩАЯ от флага контекста. Личность и
  //! присутствие приходят параметрами, потому что на пути гашения контекст
  //! уже обнулён фазой 1.
  //! @param reason Ненулевой — путь ГАШЕНИЯ: рассылка оставит след в логе,
  //!        по которому стенд её и меряет. `nullptr` — штатное обновление
  //!        присутствия от клиента, его логирует сам обработчик.
  void BroadcastPresenceOfCharacter(
    data::Uid characterUid,
    const protocol::Presence& presence,
    network::ClientId selfClientId,
    const char* reason);

  //! Слить очередь отложенных разрывов. Только с потока мессенджера.
  void DrainPendingDisconnects();

  //! LOA (R80-3, round80, backlog #235): РАЗВЁРТКА-BACKSTOP. Только с потока
  //! мессенджера (`HandleNetworkTick`): фаза 1 обходит карту клиентов, а фаза 2
  //! кладёт находки в ту же очередь отложенных разрывов, что и уборка лобби.
  void SweepChatSockets();

  //! Пороги жатвы, снятые из настроек ОДНИМ чтением на развёртку.
  [[nodiscard]] chat::ReapThresholds GetReapThresholds() const;

  //! LOA (R82, round82, director-clients-lock-hardening): ВНУТРЕННИЙ ПУТЬ
  //! «ВЫЗЫВАЮЩИЙ ДЕРЖИТ ЗАМОК» — ссылка В ЖИВУЮ КАРТУ, замка сам НЕ берёт.
  //! Форма из лобби (`LobbyNetworkHandler.hpp:399`).
  //!
  //! ★НАЗВАННЫЙ ВЫЗЫВАЮЩИЙ РОВНО ОДИН — `HandleChatterLogin`, и метод заведён
  //! ради него, а не «на будущее». Вход обязан (а) читать гард повтора по
  //! ЖИВОЙ карте, а не по копии момента входа в функцию, и (б) писать флаг и
  //! личность В КАРТУ под теми ДВУМЯ прямыми замками, которые уже стоят в его
  //! теле с R78 (их нельзя увести в `MutateClientContext`: без прямого
  //! `unique_lock` в теле покраснеет `MUST_LOCK_EXCLUSIVELY` гейта
  //! `check_messenger_client_map_lock.py`, а гард повтора потерял бы смысл).
  //!
  //! ★ГЕЙТ ЕГО ИСКЛЮЧАЕТ НАМЕРЕННО: незалоченное обращение к `_clients` в теле
  //! — это и есть контракт. В `MUST_LOCK` его вносить НЕЛЬЗЯ.
  //! @warning Вызывать только удерживая `_clientsMutex`.
  [[nodiscard]] ClientContext& GetClientContextLocked(
    network::ClientId clientId,
    bool requireAuthentication = true);

  //! LOA (R82, round82, director-clients-lock-hardening): ЕДИНСТВЕННЫЙ путь
  //! записи значения поля извне залоченного блока. Форма из лобби
  //! (`LobbyNetworkHandler.hpp:203-219`): исключительный замок берётся ВНУТРИ,
  //! лямбда — ТОЛЬКО присваивание полей.
  //!
  //! ★НИ ОДНОГО ВЫХОДА В ЧУЖОЙ КОД ВНУТРИ ЛЯМБДЫ. Замок нерекурсивный: вызов,
  //! который сам берёт `_clientsMutex` (например `BroadcastPresenceOfCharacter`
  //! через `GetClientByCharacterUid`), даёт самозахват — класс R59/R78. Это не
  //! пожелание: `tools/check_messenger_disconnect_outside_lock.py` краснеет на
  //! такой лямбде всегда.
  //! @return `false`, если записи с таким `clientId` в карте нет.
  bool MutateClientContext(
    network::ClientId clientId,
    const std::function<void(ClientContext&)>& mutation);

  ChatterServer _chatterServer;
  ServerInstance& _serverInstance;

  std::unordered_map<network::ClientId, ClientContext> _clients;

  //! LOA (R78-fix2, round78, backlog #255, находка Codex 2): замок РОВНО НАД
  //! ОДНОЙ ПАРОЙ «писатель/читатель», которую вводит этот раунд.
  //!
  //! ★ЧТО ИМЕННО ОН ЗАКРЫВАЕТ. `GetClientByCharacterUid` снимает КОПИЮ всей
  //! карты и зовётся с ЧУЖИХ потоков — ранча (`RanchDirector.cpp:11616`) и
  //! заезда (`RaceNetworkHandler.cpp:5992`). Вытеснение (R78) пишет поля чужих
  //! записей с потока чата. Одновременные чтение и запись — гонка данных и UB,
  //! и «правим только значения, не структуру» гарантией НЕ является: это
  //! замечание ревью, и оно верное. Обе стороны этой пары теперь под замком.
  //!
  //! ★ЧЕГО ОН НЕ ЗАКРЫВАЛ, И ЭТО БЫЛО СКАЗАНО ПРЯМО. Карту трогали ещё полтора
  //! десятка мест, и главное из них — `GetClientContext`, отдававший ССЫЛКУ
  //! ВНУТРЬ карты, которую вызывающие держали через выходы в чужой код. Чтобы
  //! закрыть их, нужен переход на возврат КОПИИ и внутренний `*Locked`-путь —
  //! ровно та работа, которой для лобби был посвящён отдельный раунд (R64-3,
  //! #215). R78 её не делал и на неё не претендовал: раунд обязан снять
  //! гонку, которую ДОБАВЛЯЕТ, а не переписать модель потоков мессенджера.
  //!
  //! ★СДЕЛАНО R82 (round82, director-clients-lock-hardening). Отложенное выше
  //! закрыто целиком: `GetClientContext` отдаёт КОПИЮ под `shared_lock`,
  //! появились `GetClientContextLocked` (вызывающий держит замок) и
  //! `MutateClientContext` (запись под исключительным замком), шесть
  //! незалоченных снимков всей карты переведены на лямбду-снимок-под-замком, а
  //! избыточная запись `isAuthenticated` гильд-входа удалена (её ставил true
  //! поверх гарантированного true). Тотальный инвариант стережёт
  //! `tools/check_messenger_context_copy.py`, класс «выход под замком» —
  //! `tools/check_messenger_disconnect_outside_lock.py`.
  //!
  //! ★ЗАМОК НЕ РЕКУРСИВНЫЙ. Ни один держатель не имеет права звать отсюда
  //! чужой код: `Client::End()` синхронно возвращается в
  //! `HandleClientDisconnected`, и вложенный захват был бы тихим дедлоком
  //! (класс R59). Поэтому отключение вытесненных вынесено во ВТОРУЮ фазу,
  //! за пределы замка.
  mutable std::shared_mutex _clientsMutex;

  //! LOA (R78-fix7, round78, backlog #255, находка ревю #2 BLOCK):
  //! ОТЛОЖЕННЫЕ РАЗРЫВЫ. Соединения, которые попросили закрыть с ЧУЖОГО потока.
  //!
  //! ★ЗАЧЕМ ОЧЕРЕДЬ, А НЕ ПРЯМОЙ РАЗРЫВ. `CloseSessionsOfCharacter` зовёт лобби
  //! (выход игрока) и поток чат-команд (GM-бан). Прямой `DisconnectClient`
  //! оттуда синхронно уходит в `Client::End()` → `OnClientDisconnected` →
  //! `HandleClientDisconnected`, то есть ЧУЖОЙ поток стирал бы записи из
  //! `_clients` мессенджера и из `Server::_clients`/`Server::_addressStates`
  //! (последняя вообще без замка), пока поток чата законно их читает. Это
  //! гонка по неатомарным картам и use-after-free по ссылке, которую
  //! `HandleChatterLogin` держит через всё тяжёлое тело, — а не «редкий сбой».
  //!
  //! ★ПОЧЕМУ НЕ «ПРОСТО МЬЮТЕКС». Тот же довод, по которому очередью написан
  //! `RanchDirector::Disconnect` (R34-4, #96): `GetClientContext` отдаёт
  //! `ClientContext&` НАРУЖУ, и ссылка живёт дольше любого замка внутри
  //! аксессора. Замок в аксессоре дал бы ЛОЖНУЮ безопасность.
  //!
  //! ★ШТАМП ПОКОЛЕНИЯ НЕ НУЖЕН ДЛЯ НОВЫХ СОЕДИНЕНИЙ, и ровно так это и надо
  //! читать (уточнено по NIT ревю #3 №3). В очередь кладутся УЖЕ ОТОБРАННЫЕ
  //! `clientId`, а `ClientId` монотонен и не переиспользуется
  //! (`Server::_client_id`, `Server.hpp:192`), поэтому РЕКОННЕКТ того же
  //! персонажа получает другой идентификатор и под слив не попадает.
  //!
  //! ★НО УЖЕ СТОЯЩИЙ В ОЧЕРЕДИ СОКЕТ МОГ ПЕРЕВЯЗАТЬСЯ. Скриптованный клиент
  //! успевает между постановкой и сливом пройти повторный вход (гард повтора
  //! не спасает: фаза 1 только что сняла оба поля) и снова стать законной
  //! сессией персонажа. Поэтому слив ПЕРЕПРОВЕРЯЕТ каждую запись и пропускает
  //! те, что успели перевязаться, — обещание «по построению» на этот случай
  //! не распространялось.
  //!
  //! Замок — ЛИСТОВОЙ: под ним ровно одна операция с контейнером и ни одного
  //! вызова наружу.
  //! LOA (R78-fix8, ревю #3 WARN-1): в очереди лежит ПАРА. Личность нужна,
  //! чтобы слив мог разослать «офлайн» уже ПОСЛЕ того, как фаза 1 стёрла её из
  //! контекста, — иначе рассылка молча пропадает.
  struct PendingDisconnect
  {
    network::ClientId clientId{};
    data::Uid characterUid{data::InvalidUid};
  };

  std::mutex _pendingDisconnectsMutex;
  std::vector<PendingDisconnect> _pendingDisconnects;

  //! LOA (R80-3, round80, backlog #235): когда развёртка шла последний раз.
  //! ★ЗАМКА НЕ ТРЕБУЕТ: читается и пишется ТОЛЬКО в `SweepChatSockets`, а та
  //! зовётся ТОЛЬКО из `HandleNetworkTick`, то есть только потоком чат-сервера.
  chat::ReapClock::time_point _lastChatSweep{};

  //! Дроссель жалобы «снимок лобби не снялся». Путь штатный (раз в развёртку),
  //! поэтому «одна строка на событие» была бы заготовкой флуда (урок R57).
  util::LogThrottle _lobbySnapshotThrottle{std::chrono::seconds(60)};

  //! Дроссель пояса на элемент слива.
  util::LogThrottle _drainStepThrottle{std::chrono::seconds(60)};
};

} // namespace server

#endif //MESSENGERDIRECTOR_HPP
