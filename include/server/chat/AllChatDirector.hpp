//
// Created by SergeantSerk on 30/12/2025.
//

#ifndef ALLCHATDIRECTOR_HPP
#define ALLCHATDIRECTOR_HPP

#include <libserver/network/chatter/ChatterServer.hpp>
#include <libserver/data/DataDefinitions.hpp>
#include <libserver/util/LogThrottle.hpp>

#include "server/Config.hpp"
#include "server/chat/ChatSocketReapRule.hpp"

#include <mutex>
#include <shared_mutex>
#include <vector>

namespace server
{

//! Chat client OTP constant
constexpr uint32_t AllChatOtpConstant = 0x14E05CE5;

class ServerInstance;

class AllChatDirector
  : private IChatterServerEventsHandler
{
private:
  struct ClientContext
  {
    //! Whether the client is authenticated.
    bool isAuthenticated{false};
    //! Unique ID of the client's character.
    data::Uid characterUid{data::InvalidUid};
    //! Online presence of the client.
    protocol::Presence presence{};

    //! LOA (R80-4, round80, backlog #235): МОМЕНТ ПОДКЛЮЧЕНИЯ. Основание P1, и
    //! оно НЕ ОСВЕЖАЕТСЯ активностью — иначе шумящий сканер бессмертен.
    chat::ReapClock::time_point connectedAt{};
    //! LOA (R80-4, round80, backlog #235): последняя входящая порция данных.
    //! Основание грейса P2 и (если включён) P3.
    chat::ReapClock::time_point lastActivity{};
  };

public:
  explicit AllChatDirector(ServerInstance& serverInstance);

  //! Get chat config.
  //! @return Chat config.
  [[nodiscard]] Config::AllChat& GetConfig();

  void Initialize();
  void Terminate();
  ClientContext& GetClientContext(
    network::ClientId clientId,
    bool requireAuthentication = true);
  void Tick();

  //! LOA (R80-4, round80, backlog #235): ЗАКРЫТЬ ВСЕ all-chat-сессии персонажа.
  //!
  //! ★ЗВАТЬ МОЖНО С ЛЮБОГО ПОТОКА, И ЭТО ЕДИНСТВЕННАЯ ПРИЧИНА, ПО КОТОРОЙ У
  //! ЭТОЙ КАРТЫ ПОЯВИЛСЯ ЗАМОК. До раунда `_clients` трогал только собственный
  //! сетевой поток; зов из уборки лобби (выход игрока) делает раунд ПЕРВЫМ
  //! чужепоточным писателем. Фаза 1 (отвязка) идёт здесь под исключительным
  //! замком, фаза 2 (разрыв) — в `DrainPendingDisconnects` на СВОЁМ потоке.
  void CloseSessionsOfCharacter(data::Uid characterUid);

private:
  void HandleClientConnected(network::ClientId clientId) override;
  void HandleClientDisconnected(network::ClientId clientId) override;

  //! LOA (R80-4, round80, backlog #235): штамп «пир говорит». Приходит с потока
  //! all-chat на КАЖДУЮ входящую порцию данных, ДО разбора кадра.
  void HandleClientActivity(network::ClientId clientId) override;

  //! LOA (R80-4, round80, backlog #235): сетевой тик СВОЕГО потока. Здесь и
  //! только здесь законно обходить карту и рвать соединения.
  void HandleNetworkTick() override;

  //! Фаза 1+2 развёртки. Только с потока all-chat.
  void SweepChatSockets();

  //! Слить очередь отложенных разрывов. Только с потока all-chat.
  void DrainPendingDisconnects();

  //! LOA (R80-4, round80, backlog #235): успешный вход вытесняет ПРЕЖНИЕ
  //! сессии того же персонажа. Зеркало R78 для мессенджера; зовётся с потока
  //! all-chat, поэтому фаза 2 здесь СИНХРОННА и очередь не нужна.
  void EvictOtherSessionsOfCharacter(
    network::ClientId keepClientId,
    data::Uid characterUid);

  //! Пороги жатвы, снятые из настроек ОДНИМ чтением на развёртку.
  [[nodiscard]] chat::ReapThresholds GetReapThresholds() const;

  // Handler methods for chatter commands
  void HandleChatterEnterRoom(
    network::ClientId clientId,
    const protocol::ChatCmdEnterRoom& command);
  
  void HandleChatterChat(
    network::ClientId clientId,
    const protocol::ChatCmdChat& command);

  void HandleChatterInputState(
    network::ClientId clientId,
    const protocol::ChatCmdInputState& command);

  ChatterServer _chatterServer;
  ServerInstance& _serverInstance;

  std::unordered_map<network::ClientId, ClientContext> _clients;

  //! LOA (R80-4, round80, backlog #235): ЗАМОК НАД КАРТОЙ КЛИЕНТОВ.
  //!
  //! ★ЗАЧЕМ ОН ПОЯВИЛСЯ ИМЕННО СЕЙЧАС. До раунда карту трогал ровно один поток
  //! — собственный сетевой, — поэтому замка не было и он не был нужен. Раунд
  //! вводит ПЕРВОГО чужепоточного писателя: `CloseSessionsOfCharacter` зовёт
  //! поток лобби на выходе игрока. Развёртка и вытеснение остаются на своём
  //! потоке, но и они обязаны стоять под тем же замком — иначе синхронизации
  //! нет, а есть только её половина.
  //!
  //! ★ВСТАВКА И УДАЛЕНИЕ ТОЖЕ ПОД ЗАМКОМ, И ЭТО НЕ ПЕДАНТИЗМ. `try_emplace` —
  //! СТРУКТУРНАЯ мутация: она вправе вызвать РЕХЭШ, а рехэш под чужим обходом
  //! рвёт всё, что обход держит. Это класс R61-2/#202, и R78 закрыл ровно его
  //! у мессенджера.
  //!
  //! ★ЗАМОК НЕ РЕКУРСИВНЫЙ. `Client::End()` СИНХРОННО возвращается в
  //! `HandleClientDisconnected`, а тот берёт этот же замок: любое закрытие
  //! соединения под ним было бы самозахватом (класс R59), поэтому вторая фаза
  //! ВСЕГДА вне замка.
  //!
  //! ★ЧЕГО ОН НЕ ЗАКРЫВАЕТ, СКАЗАНО ПРЯМО: `GetClientContext` отдаёт ССЫЛКУ
  //! ВНУТРЬ карты, и обращения через неё формально гоночны против фазы 1.
  //! Порчи памяти отсюда не следует (фаза 1 не вставляет и не удаляет, поля
  //! тривиальны), худший исход — один пойманный диспетчером бросок у уже
  //! уходящего игрока. Полное закрытие = возврат КОПИИ и `*Locked`-путь, то
  //! есть отдельный раунд (для лобби им был R64-3, #215).
  mutable std::shared_mutex _clientsMutex;

  //! LOA (R80-4, round80, backlog #235): ОТЛОЖЕННЫЕ РАЗРЫВЫ — та же форма, что
  //! у мессенджера (R78). Личность кладётся вместе с соединением не ради
  //! рассылки (all-chat присутствие никому не вещает), а чтобы слив мог
  //! перепроверить «запись всё ещё отвязана» тем же способом.
  struct PendingDisconnect
  {
    network::ClientId clientId{};
    data::Uid characterUid{data::InvalidUid};
  };

  std::mutex _pendingDisconnectsMutex;
  std::vector<PendingDisconnect> _pendingDisconnects;

  //! Когда развёртка шла последний раз. ★Замка не требует: читается и пишется
  //! ТОЛЬКО в `SweepChatSockets`, а та зовётся ТОЛЬКО из `HandleNetworkTick`.
  chat::ReapClock::time_point _lastChatSweep{};

  //! Дроссели редких жалоб: путь штатный, «одна строка на событие» была бы
  //! заготовкой флуда (урок R57).
  util::LogThrottle _lobbySnapshotThrottle{std::chrono::seconds(60)};
  util::LogThrottle _drainStepThrottle{std::chrono::seconds(60)};
};

} // namespace server

#endif // ALLCHATDIRECTOR_HPP
