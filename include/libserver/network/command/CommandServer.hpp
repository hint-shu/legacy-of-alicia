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

#ifndef COMMAND_SERVER_HPP
#define COMMAND_SERVER_HPP

#include "CommandProtocol.hpp"
#include "libserver/Constants.hpp"
#include "libserver/network/Server.hpp"
#include "libserver/util/LogThrottle.hpp"
#include "libserver/util/QuietLog.hpp"
#include "libserver/util/Stream.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace server
{

namespace asio = network::asio;
using ClientId = network::ClientId;

//! A command handler.
using RawCommandHandler = std::function<void(ClientId, SourceStream&)>;

//! A command supplier.
using CommandSupplier = std::function<void(SinkStream&)>;

//! A command client.
class CommandClient
{
public:
  void SetCode(protocol::XorCode code);
  void RollCode();

  [[nodiscard]] const protocol::XorCode& GetRollingCode() const;
  [[nodiscard]] int32_t GetRollingCodeInt() const;

private:
  // LOA-fix (R61-20, round61, находка ревью): МЁРТВЫЙ ЧЛЕН УБРАН.
  // `_commandQueue` не читался и не писался НИГДЕ — единственное упоминание во
  // всём дереве было это объявление. При этом он делал каждую запись о клиенте
  // тяжёлой (пустая `std::queue` — сотни байт против четырёх у кода), то есть
  // именно он составлял бОльшую часть цены течи #208: карта росла по числу
  // соединений за всю жизнь процесса, и каждая запись тащила эту очередь.
  protocol::XorCode _rollingCode{};
};

template <typename T>
concept ReadableCommandStruct = ReadableStruct<T> and requires
{
  {T::GetCommand()};
};

template <typename T>
concept WritableCommandStruct = WritableStruct<T> and requires
{
  {T::GetCommand()};
};

//! A command server.
class CommandServer final
{
public:
  //! Command server event handler interface.
  class EventHandlerInterface
  {
  public:
    virtual ~EventHandlerInterface() = default;
    virtual void HandleNetworkTick() {}
    virtual void HandleClientConnected(ClientId clientId) = 0;
    virtual void HandleClientDisconnected(ClientId clientId) = 0;

    //! LOA-fix (R21-3a, round21, backlog #95): вызывается на КАЖДУЮ входящую
    //! дейтаграмму клиента, ДО разбора команд. Нужен именно на этом уровне, а
    //! не на уровне хендлеров: канал держит живым в том числе AcCmdCRHeartbeat
    //! (0x12), у которой зарегистрированного хендлера нет вообще.
    //! Дефолт — no-op: лобби/гонка/мессенджер ничего не платят и не меняются.
    //! ★Реализация обязана быть дешёвой и НЕ бросать: исключение отсюда уходит
    //! в сетевой read-loop и рвёт клиенту соединение.
    //! @param clientId ID клиента, приславшего данные (в дефолтной пустышке
    //!        не именован — иначе -Wunused-parameter на каждом наследнике).
    virtual void HandleClientActivity(ClientId) {}
  };

  //! Constructor.
  //! @param eventHandlerInterface Instance of event handler.
  CommandServer(
    EventHandlerInterface& eventHandlerInterface);

  //! Destructor. Останавливает и дожидается поток сервера, если он ещё жив
  //! (R49-14c, backlog #178).
  ~CommandServer();

  CommandServer(const CommandServer&) = delete;
  CommandServer& operator=(const CommandServer&) = delete;

  CommandServer(CommandServer&&) = delete;
  CommandServer& operator=(CommandServer&&) = delete;

  //! Begins the command server.
  //! @param address Address.
  //! @param port Port.
  void BeginHost(const asio::ip::address& address, uint16_t port);

  //! Ends the command server.
  void EndHost();

  asio::ip::address_v4 GetClientAddress(ClientId);
  void DisconnectClient(ClientId clientId);

  void SetCode(ClientId client, protocol::XorCode code);

  //! Registers a command handler.
  //! @param handler Handler of the command.
  //!
  //! LOA-fix (R71-18, находка ревью 2 #6): РАЗБОР, УПАВШИЙ НА ДАННЫХ КЛИЕНТА, — ЭТО
  //! ТИХИЙ ОТКАЗ, А НЕ СТРОКА В ЛОГЕ НА КАЖДЫЙ ПАКЕТ.
  //!
  //! ★ЗАЧЕМ ЗДЕСЬ, А НЕ В ХЕНДЛЕРАХ. Гарды раунда стоят ВНУТРИ обработчиков, а
  //! `C::Read` выполняется ДО них — то есть авторизация обходится, не доходя до
  //! авторизации: заявить ледяную стену (`magicItemId = 10`) и не дослать шесть
  //! floats, или прислать известный тип ретрансляции с нулевой нагрузкой. `Read`
  //! бросает `std::underflow_error` из потока, бросок ловится в
  //! `CommandServer.cpp:515-527` и печатает `[error]` — по строке на пакет, без
  //! дросселя. Чинить это в каждом `Read` (их сотни) значило бы вести список мест;
  //! правило ставится там, где оно тотально по построению: разбор ЛЮБОЙ команды либо
  //! удался, либо команда не обрабатывается.
  //!
  //! ★ДРОССЕЛЬ — СВОЙ НА КАЖДУЮ КОМАНДУ, БЕЗ ЕДИНОЙ КАРТЫ. `static` внутри шаблона
  //! даёт ровно один экземпляр на КАЖДЫЙ тип `C`: ёмкость фиксирована числом
  //! зарегистрированных команд, ключа от клиента нет, расти нечему. Флуд разбором
  //! одной команды не заглушает жалобу на другую.
  //!
  //! ★БРОСОК ИЗ САМОГО ХЕНДЛЕРА НЕ ГЛОТАЕТСЯ: он остаётся внешнему `catch`, как и
  //! был. Это разные события — «клиент прислал мусор» и «сервер не справился», и
  //! сваливать их в одну ветку значило бы прятать вторую за первой.
  template <ReadableCommandStruct C>
  void RegisterCommandHandler(
    std::function<void(ClientId clientId, const C& command)> handler)
  {
    _handlers[C::GetCommand()] = [handler](ClientId clientId, SourceStream& source)
    {
      C command;

      try
      {
        C::Read(command, source);
      }
      catch (const std::exception& x)
      {
        //! ★ЯВНОЕ ОКНО (итерация 12): конструктор `LogThrottle` редакции R72,
        //! которая лежит в `main`, значения по умолчанию не имеет. Пять секунд —
        //! то же окно, что было у умолчания прежней редакции R71.
        static server::util::LogThrottle malformedPayloadThrottle{std::chrono::seconds(5)};

        uint64_t suppressed = 0, total = 0;
        if (malformedPayloadThrottle.Allow(suppressed, total))
          server::util::QuietLogWarn(
            "Malformed payload for command '{}' from client '{}': {} (suppressed {})",
            protocol::GetCommandName(C::GetCommand()),
            clientId,
            x.what(),
            suppressed);
        return;
      }

      handler(clientId, command);
    };
  }

  //! LOA-fix (R72-fix-1, round72, backlog #129-S1, находка Codex 1):
  //! ★РЕГИСТРАЦИЯ С ВОРОТАМИ, КОТОРЫЕ СТОЯТ ДО РАЗБОРА ПАКЕТА.
  //!
  //! ПОЧЕМУ ОТДЕЛЬНЫЙ ВХОД, А НЕ ПРОВЕРКА В ТЕЛЕ ХЕНДЛЕРА. Обычный
  //! `RegisterCommandHandler` разбирает пакет (`C::Read`) ПЕРЕД тем, как
  //! отдать управление хендлеру. Значит любая проверка, живущая в хендлере или
  //! в обёртке над ним, стоит ПОСЛЕ десериализации — и сокет, которому вообще
  //! нечего тут делать, доходит до разбора. Кадр НЕВЕРНОЙ ДЛИНЫ заставляет
  //! `C::Read` бросить, бросок ловит диспетчер и пишет строку [error] НА
  //! КАЖДЫЙ ПАКЕТ, а счётчик отказов не растёт вовсе: то есть отказ и не
  //! дросселирован, и не сосчитан. Ворота обязаны стоять раньше разбора.
  //!
  //! ★ДАННЫЕ ПОГЛОЩАЮТСЯ, А НЕ БРОСАЮТСЯ. Отказ двигает курсор потока команды
  //! в конец. Диспетчер после хендлера утверждает `assert(cursor == size)`;
  //! оставленный непрочитанным хвост — это падение отладочной сборки на каждом
  //! отказе, то есть проверка, «работающая» только потому, что в релизе
  //! `assert` выкинут препроцессором.
  //!
  //! @param gate Ворота: получают клиента и команду, возвращают «пускать ли».
  //!             Обязаны быть дешёвыми и НЕ бросать — они на пути пакета.
  //! @param handler Обработчик команды.
  template <ReadableCommandStruct C>
  void RegisterGatedCommandHandler(
    std::function<bool(ClientId clientId, protocol::Command command)> gate,
    std::function<void(ClientId clientId, const C& command)> handler)
  {
    _handlers[C::GetCommand()] = [gate, handler](ClientId clientId, SourceStream& source)
    {
      if (not gate(clientId, C::GetCommand()))
      {
        // Поглощаем команду целиком, не разбирая её.
        source.Seek(source.Size());
        return;
      }

      C command;
      C::Read(command, source);
      handler(clientId, command);
    };
  }

  //! Queues a command for sending.
  //! @param clientId ID of the client to send the command to.
  //! @param supplier Supplier of the command.
  template <WritableStruct C>
  void QueueCommand(
    ClientId clientId,
    std::function<C()> supplier)
  {
    SendCommand(clientId, C::GetCommand(), [supplier](SinkStream& sink){
      C::Write(supplier(), sink);
    });
  }

private:
  class NetworkEventHandler
    : public network::EventHandlerInterface
  {
  public:
    NetworkEventHandler(CommandServer& commandServer);

    void HandleNetworkTick() override;
    void OnClientConnected(network::ClientId clientId) override;
    void OnClientDisconnected(network::ClientId clientId) override;
    size_t OnClientData(network::ClientId clientId, const std::span<const std::byte>& data) override;

  private:
    CommandServer& _commandServer;
  };

  //! LOA-fix (R61-12, round61, backlog #202/#208): ЕДИНСТВЕННЫЕ входы в карту.
  //! ★Смысл не в краткости, а в том, чтобы «место, где надо не забыть взять
  //! замок» было ровно ОДНО. Список мест обязательно отстанет от кода; одна
  //! точка — нет. Наружу отдаётся КОПИЯ указателя, поэтому вызывающий работает
  //! с объектом уже без замка и переживает любой рехэш.
  //!
  //! ★ПОИСК НЕ ВСТАВЛЯЕТ — И ЭТО ГЛАВНОЕ СВОЙСТВО (находка ревью, итерация 2).
  //! Пока поиск был вставляющим, удаление не было ТЕРМИНАЛЬНЫМ: запоздавший
  //! пакет или `SetCode` с потока директора воскрешали запись уже после снятия,
  //! и течь #208 возвращалась. Запись заводится ровно один раз — на подключении.
  void CreateClient(ClientId clientId);
  [[nodiscard]] std::shared_ptr<CommandClient> FindClient(ClientId clientId) const;

  //! LOA-fix (R61-12b, round61, backlog #208): удаление записи о клиенте.
  void RemoveClient(ClientId clientId);

  //!
  void SendCommand(
    ClientId clientId,
    protocol::Command commandId,
    CommandSupplier supplier);

  bool debugIncomingCommandData = constants::DebugCommands;
  bool debugOutgoingCommandData = constants::DebugCommands;
  bool debugCommands = constants::DebugCommands;

  std::unordered_map<protocol::Command, RawCommandHandler> _handlers{};
  //! LOA-fix (R61-11, round61, backlog #202): значение стало `shared_ptr`.
  //!
  //! ★ЧТО ИМЕННО ЗАЩИЩЕНО — ЧИТАТЬ ВНИМАТЕЛЬНО. Замок и указатель закрывают
  //! ВРЕМЯ ЖИЗНИ ЗАПИСИ и рехэш КАРТЫ. Содержимое самого `CommandClient`
  //! (`_rollingCode`) НЕ синхронизировано и этим раундом НЕ чинится: `SetCode`
  //! приходит с потоков лобби и заезда, а `RollCode`/`GetRollingCode` — с
  //! сетевого. Это дореформенная гонка, заведена как #216.
  //! ★Формулировка нарочно узкая. Первая редакция этого блока утверждала
  //! инвариант, которого нет («приём однопоточный»), и ревью поймало это как
  //! BLOCK: ложное утверждение о потокобезопасности живёт тихо и подводит
  //! следующий раунд, который на него обопрётся. Здесь написано ровно то, что
  //! верно, и явно сказано, что НЕ верно.
  //!
  //! ★ПОЧЕМУ УКАЗАТЕЛЬ, А НЕ ЗНАЧЕНИЕ. В карту ВСТАВЛЯЛИ обе стороны через
  //! вставляющий `operator[]`, а `OnClientData` брал ССЫЛКУ внутрь карты и жил
  //! с ней всю обработку команды — вставка с чужого потока делала её висячей.
  //! Указатель это лечит: объект перестаёт ездить при рехэше, и взятая копия
  //! остаётся действительной, даже если карта переехала целиком.
  std::unordered_map<ClientId, std::shared_ptr<CommandClient>> _clients{};
  //! Замок карты. Разделяемый на чтение, исключительный на вставку/удаление.
  mutable std::shared_mutex _clientsMutex{};

  EventHandlerInterface& _eventHandler;
  NetworkEventHandler _serverNetworkEventHandler;

  network::Server _server;
  std::thread _serverThread;
};

} // namespace server

#endif // COMMAND_SERVER_HPP
