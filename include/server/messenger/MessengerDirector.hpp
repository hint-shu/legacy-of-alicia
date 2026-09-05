//
// Created by rgnter on 30/08/2025.
//

#ifndef MESSENGERDIRECTOR_HPP
#define MESSENGERDIRECTOR_HPP

#include "server/ranch/BreedingMarket.hpp"

#include <libserver/network/chatter/ChatterServer.hpp>
#include <libserver/data/DataDefinitions.hpp>

#include "server/Config.hpp"

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

  ClientContext& GetClientContext(
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
  //! ★ЧЕГО ОН НЕ ЗАКРЫВАЕТ, И ЭТО СКАЗАНО ПРЯМО. Карту трогают ещё полтора
  //! десятка мест, и главное из них — `GetClientContext`, отдающий ССЫЛКУ
  //! ВНУТРЬ карты, которую вызывающие держат через выходы в чужой код. Чтобы
  //! закрыть их, нужен переход на возврат КОПИИ и внутренний `*Locked`-путь —
  //! ровно та работа, которой для лобби был посвящён отдельный раунд (R64-3,
  //! #215). R78 её не делает и на неё не претендует: раунд обязан снять
  //! гонку, которую ДОБАВЛЯЕТ, а не переписать модель потоков мессенджера.
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
};

} // namespace server

#endif //MESSENGERDIRECTOR_HPP
