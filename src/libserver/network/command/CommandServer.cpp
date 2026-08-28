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

#include "libserver/network/command/CommandServer.hpp"
#include "libserver/util/QuietLog.hpp"

#include "libserver/util/Deferred.hpp"
#include "libserver/util/Util.hpp"

#include <ranges>
#include <stacktrace>

#include <spdlog/spdlog.h>

namespace server
{

namespace
{

//! Max size of the command data.
constexpr std::size_t MaxCommandDataSize = 8192;

//! Max size of the whole command payload.
//! That is command data size + size of the message magic.
constexpr std::size_t MaxCommandSize = MaxCommandDataSize + sizeof(protocol::MessageMagic);

//! Reads every specified byte of the source stream,
//! performs XOR operation on that byte with the specified sliding key
//! and writes the result to the sink stream.
//!
//! @param key Xor Key
//! @param source Source stream.
//! @param sink Sink stream.
void XorAlgorithm(
  const protocol::XorCode& key,
  SourceStream& source,
  SinkStream& sink)
{
  for (std::size_t idx = 0; idx < source.Size(); idx++)
  {
    const auto shift = idx % 4;

    // Read a byte.
    std::byte v;
    source.Read(v);

    // Xor it with the key.
    sink.Write(v ^ key[shift]);
  }
}

bool IsMuted(protocol::Command id)
{
  return id == protocol::Command::AcCmdCLHeartbeat
    || id == protocol::Command::AcCmdCLRoomList
    || id == protocol::Command::AcCmdCLRoomListOK
    || id == protocol::Command::AcCmdCRHeartbeat
    || id == protocol::Command::AcCmdCRRanchSnapshot
    || id == protocol::Command::AcCmdCRRanchSnapshotNotify
    || id == protocol::Command::AcCmdUserRaceUpdatePos
    || id == protocol::Command::AcCmdCRRelay
    || id == protocol::Command::AcCmdCRRelayNotify
    || id == protocol::Command::AcCmdCRRelayCommand
    || id == protocol::Command::AcCmdCRRelayCommandNotify
    || id == protocol::Command::AcCmdUserRaceActivateEvent
    || id == protocol::Command::AcCmdCRStarPointGet
    || id == protocol::Command::AcCmdCRStarPointGetOK;
}

} // namespace

void CommandClient::SetCode(protocol::XorCode code)
{
  this->_rollingCode = code;
}

void CommandClient::RollCode()
{
  auto& rollingCode = *reinterpret_cast<int32_t*>(
    _rollingCode.data());

  rollingCode = rollingCode * protocol::XorMultiplier;
  rollingCode = protocol::XorControl - rollingCode;
}

const protocol::XorCode& CommandClient::GetRollingCode() const
{
  return _rollingCode;
}

int32_t CommandClient::GetRollingCodeInt() const
{
  return *reinterpret_cast<const int32_t*>(_rollingCode.data());
}

CommandServer::CommandServer(
  EventHandlerInterface& networkEventHandler)
  : _eventHandler(networkEventHandler)
  , _serverNetworkEventHandler(*this)
  , _server(_serverNetworkEventHandler)
{
}

CommandServer::~CommandServer()
{
  // LOA-fix (R49-14c, round49, backlog #178): деструктор по умолчанию убивал
  // процесс, если поток сервера всё ещё жив — а он остаётся живым каждый раз,
  // когда директор вышел через исключение и не дошёл до своего Terminate().
  try
  {
    EndHost();
  }
  catch (...)
  {
  }

}

namespace
{

//! LOA-fix (R49-4a, round49, backlog #178): тот же доклад, что и у директорских
//! потоков — из ветки перехвата под функцией потока бросать нельзя вообще.
//! LOA-fix (R49-14b, round49, backlog #178): остановка сервера — сама бросающая
//! операция (`Server::End` зовёт `_acceptor.close()` в бросающей перегрузке), а
//! стоит она в ветке перехвата ПРЯМО под функцией потока. Бросок оттуда —
//! std::terminate.
template <typename ServerType>
void StopQuietly(ServerType& server) noexcept
{
  try
  {
    server.End();
  }
  catch (...)
  {
  }
}

void ReportHostFailure(const char* reason) noexcept
{
  try
  {
    server::util::QuietLogError("Unhandled command server network exception: {}", reason);

    for (const auto& entry : std::stacktrace::current())
    {
      server::util::QuietLogError("[Stack] {}({}): {}", entry.source_file(), entry.source_line(), entry.description());
    }
  }
  catch (...)
  {
  }
}

} // anon namespace

void CommandServer::BeginHost(const asio::ip::address& address, uint16_t port)
{
  _serverThread = std::thread(
    [this, address, port]()
    {
      try
      {
        _server.Begin(address, port);
      }
      catch (const std::exception& x)
      {
        ReportHostFailure(x.what());
        StopQuietly(_server);
      }
      catch (...)
      {
        ReportHostFailure("unknown exception");
        StopQuietly(_server);
      }
    });
}

void CommandServer::EndHost()
{
  if (not _serverThread.joinable())
    return;

  _server.End();
  _serverThread.join();
}

asio::ip::address_v4 CommandServer::GetClientAddress(
  const ClientId clientId)
{
  return _server.GetClient(clientId)->GetAddress();
}

void CommandServer::DisconnectClient(
  const ClientId clientId)
{
  _server.GetClient(clientId)->End();
}

void CommandServer::CreateClient(const ClientId clientId)
{
  // LOA-fix (R61-14, round61, backlog #208): запись заводится РОВНО ОДИН РАЗ —
  // на подключении, и больше нигде.
  // ★ЗАЧЕМ ИМЕННО ТАК (находка ревью, итерация 2). Пока запись мог создать
  // любой обратившийся, удаление не было ТЕРМИНАЛЬНЫМ: снятая на разрыве
  // запись тут же воскресала — либо запоздавшим пакетом, либо `SetCode` с
  // потока директора, — и снимать её было уже некому. Течь #208 возвращалась
  // молча, а комментарий в коде уверял, что этого не бывает.
  std::unique_lock lock(_clientsMutex);
  _clients.try_emplace(clientId, std::make_shared<CommandClient>());
}

std::shared_ptr<CommandClient> CommandServer::FindClient(
  const ClientId clientId) const
{
  // ★РАЗДЕЛЯЕМЫЙ ЗАМОК И НИКАКОЙ ВСТАВКИ. Возвращается КОПИЯ указателя:
  // вызывающий работает с объектом уже без замка и переживает и рехэш, и
  // удаление записи другим потоком.
  std::shared_lock lock(_clientsMutex);
  const auto it = _clients.find(clientId);
  return it == _clients.end() ? nullptr : it->second;
}

void CommandServer::RemoveClient(const ClientId clientId)
{
  // LOA-fix (R61-14b, round61, backlog #208): запись о клиенте УДАЛЯЕТСЯ.
  // ★До этого раунда во всём файле не было ни одного `erase`, ни одного
  // `clear` — карта росла по числу СОЕДИНЕНИЙ за всю жизнь процесса, а не по
  // числу одновременных игроков: идентификаторы монотонны, переиспользования
  // нет, поэтому каждый вход, каждое переподключение и каждый оборванный
  // коннект добавляли запись НАВСЕГДА. И это не пара байт: внутри код
  // шифрования, а до чистки #208 лежала ещё и мёртвая очередь.
  // ★УДАЛЕНИЕ ТЕРМИНАЛЬНО: заводит запись только `CreateClient`, поэтому после
  // снятия воскресить её некому — ни запоздавшим пакетом, ни чужим потоком.
  // ★ЗНАЧЕНИЕ ВЫНИМАЕТСЯ ИЗ-ПОД ЗАМКА И РАЗРУШАЕТСЯ СНАРУЖИ: деструктор
  // `CommandClient` не должен отрабатывать под замком карты.
  std::shared_ptr<CommandClient> removed;
  {
    std::unique_lock lock(_clientsMutex);
    const auto it = _clients.find(clientId);
    if (it == _clients.end())
      return;
    removed = std::move(it->second);
    _clients.erase(it);
  }
}

void CommandServer::SetCode(
  const ClientId client,
  const protocol::XorCode code)
{
  // LOA-fix (R61-13, round61, backlog #202): через единственный вход в карту.
  // Зовётся с потоков лобби и заезда — то есть это ровно тот чужепоточный
  // писатель, из-за которого возможен одновременный рехэш с двух сторон.
  // ★ПОИСК НЕ ВСТАВЛЯЮЩИЙ: если записи уже нет, клиент разорвался, и создавать
  // её заново значило бы вернуть течь #208 (запись, которую больше некому
  // снять). Молча пропускаем — это штатная гонка разрыва, а не ошибка.
  const auto clientRecord = FindClient(client);
  if (not clientRecord)
  {
    server::util::QuietLogWarn(
      "Ignoring code assignment for client {}: the client is already gone",
      client);
    return;
  }

  clientRecord->SetCode(code);
}

CommandServer::NetworkEventHandler::NetworkEventHandler(
  CommandServer& commandServer)
  : _commandServer(commandServer)
{
}

void CommandServer::NetworkEventHandler::HandleNetworkTick()
{
  _commandServer._eventHandler.HandleNetworkTick();
}

void CommandServer::NetworkEventHandler::OnClientConnected(
  network::ClientId clientId)
{
  // LOA-fix (R61-18a, round61, backlog #208): запись заводится ЗДЕСЬ и только
  // здесь — до того, как обработчик сможет что-либо отправить этому клиенту.
  _commandServer.CreateClient(clientId);

  _commandServer._eventHandler.HandleClientConnected(clientId);
}

void CommandServer::NetworkEventHandler::OnClientDisconnected(
  network::ClientId clientId)
{
  // LOA-fix (R61-18b, round61, backlog #208): удаление ОБЯЗАНО случиться ДАЖЕ
  // ЕСЛИ ОБРАБОТЧИК БРОСИЛ.
  // ★ЭТО НАХОДКА РЕВЬЮ, И ОНА БЫЛА РЕАЛЬНОЙ ТЕЧЬЮ. `HandleClientDisconnected`
  // умеет бросать: все три реализации обращаются к `GetClientContext`, который
  // на отсутствующей записи кидает «client is not available». Бросок ловится
  // этажом выше, процесс живёт, `Server::_clients` чистится — а снятие ЭТОЙ
  // записи, стой оно простой строкой ниже, просто не выполнялось бы. То есть
  // ровно на пути отказа течь #208 оставалась.
  // ★Апстрим выше по стеку уже пришёл к тому же выводу (R49-21): удаление
  // делается В ЛЮБОМ СЛУЧАЕ и ПО КЛЮЧУ. Здесь тот же приём.
  const Deferred removeRecord([&] { _commandServer.RemoveClient(clientId); });

  _commandServer._eventHandler.HandleClientDisconnected(clientId);
}

size_t CommandServer::NetworkEventHandler::OnClientData(
  network::ClientId clientId,
  const std::span<const std::byte>& data)
{
  // LOA-fix (R21-3b, round21, backlog #95): отметка «клиент говорит» ДО разбора
  // команд. Здесь, а не в хендлере: AcCmdCRHeartbeat (0x12) хендлера не имеет и
  // уходит в ветку «Unhandled command» — хук уровнем выше её бы не увидел.
  // Дефолтная реализация — no-op, платит только директор ранчо (R21-2a).
  _commandServer._eventHandler.HandleClientActivity(clientId);

  SourceStream commandStream(data);

  while (commandStream.GetCursor() != commandStream.Size())
  {
    // The size of the buffer that was not read yet.
    const size_t bufferedDataSize = commandStream.Size() - commandStream.GetCursor();

    // Do not continue if the available data does not contain the data
    // for the message magic.
    if (bufferedDataSize < sizeof(protocol::MessageMagic))
      break;

    // Store the origin cursor position before reading the command.
    // This is so we can return to it if the command is not completely buffered.
    const auto streamOrigin = commandStream.GetCursor();
    bool isCommandBufferedWhole = true;

    const Deferred deferredResetCommandStreamCursor(
      [streamOrigin, &commandStream, &isCommandBufferedWhole]()
      {
        // If the command was not buffered whole,
        // reset the stream to the cursor before the command was read,
        // so that it may be read when more data arrive.
        if (not isCommandBufferedWhole)
          commandStream.Seek(streamOrigin);
      });

    // Read the message magic.
    uint32_t magicValue{};
    commandStream.Read(magicValue);

    const auto magic = protocol::decode_message_magic(magicValue);

    // Command ID must be within the valid range.
    if (magic.id > static_cast<uint16_t>(protocol::Command::Count))
    {
      throw std::runtime_error(
        std::format(
          "Invalid command magic: Bad command ID '{}'",
          magic.id)
          .c_str());
    }

    // The provided payload length must be at least the size
    // of the magic itself and smaller than the max command size.
    if (magic.length < sizeof(protocol::MessageMagic) || magic.length > MaxCommandSize)
    {
      throw std::runtime_error(
        std::format(
          "Invalid command magic: Bad command data size '{}'",
          magic.length)
          .c_str());
    }

    // Size of the data portion of the command.
    const size_t commandDataSize = static_cast<size_t>(magic.length) - sizeof(protocol::MessageMagic);
    // Size of the available data.
    const size_t availableDataSize = bufferedDataSize - sizeof(protocol::MessageMagic);

    // If all the required command data are not buffered,
    // wait for them to arrive.
    if (commandDataSize > availableDataSize)
    {
      isCommandBufferedWhole = false;
      break;
    }

    // Buffer for the command data.
    std::array<std::byte, MaxCommandDataSize> commandDataBuffer{};

    // Read the command data.
    commandStream.Read(
      commandDataBuffer.data(),
      commandDataSize);

    SourceStream commandDataStream(nullptr);

    // LOA-fix (R61-15, round61, backlog #202): КОПИЯ УКАЗАТЕЛЯ, А НЕ ССЫЛКА
    // ВНУТРЬ КАРТЫ. Прежняя `auto&` жила всю обработку команды, и вставка с
    // потока лобби или заезда в это время делала её висячей. Теперь объект
    // держится своим указателем и переживает любой рехэш.
    // ★ПОИСК НЕ ВСТАВЛЯЮЩИЙ: запись заводится только на подключении. Её
    // отсутствие здесь означает, что клиент уже разорвался, а данные с его
    // сокета ещё в пути. Создавать запись заново нельзя — снимать её было бы
    // некому, и течь #208 вернулась бы.
    // ★ДАННЫЕ ПОГЛОЩАЮТСЯ ЦЕЛИКОМ, а не отбрасываются возвратом нуля: вызывающий
    // делает `consume(результат)`, и ноль оставил бы буфер неразобранным —
    // то есть бесконечный цикл вместо аккуратного завершения.
    const auto client = _commandServer.FindClient(clientId);
    if (not client)
    {
      server::util::QuietLogWarn(
        "Discarding {} bytes for client {}: the client is already gone",
        data.size(), clientId);
      return data.size();
    }

    const auto commandId = static_cast<protocol::Command>(magic.id);

    // Validate and process the command data.
    if (commandDataSize > 0)
    {
      client->RollCode();

      const uint32_t code = client->GetRollingCodeInt();

      // Extract the padding from the code.
      const auto padding = code & 7;

      // The command is malformed if the provided command data
      // size is smaller or equal to the generated padding.
      if (padding >= commandDataSize)
      {
        throw std::runtime_error(
          std::format(
            "Malformed command {}: Bad command data size '{}', padding is {}.",
            GetCommandName(commandId),
            commandDataSize,
            padding)
            .c_str());
      }

      const auto actualCommandDataSize = commandDataSize - padding;

      // Source stream of the command data.
      SourceStream dataSourceStream(
        {commandDataBuffer.begin(), commandDataSize});
      // Sink stream of the command data.
      // Correctly points to the same buffer.
      SinkStream dataSinkStream(
        {commandDataBuffer.begin(), commandDataBuffer.end()});

      // Apply XOR algorithm to the data.
      XorAlgorithm(
        client->GetRollingCode(),
        dataSourceStream,
        dataSinkStream);

      commandDataStream = std::move(SourceStream(
        {commandDataBuffer.begin(), actualCommandDataSize}));

      if (_commandServer.debugIncomingCommandData
        && not IsMuted(commandId))
      {
        server::util::QuietLogDebug("Read data for command '{}' (0x{:X}),\n\n"
          "XOR code: {:#X},\n"
          "Command data size: {} (padding: {}),\n"
          "Actual command data size: {}\n"
          "Processed data dump: \n\n{}\n",
          GetCommandName(commandId),
          magic.id,
          code,
          commandDataSize,
          padding,
          actualCommandDataSize,
          util::GenerateByteDump({commandDataBuffer.data(), commandDataSize}));
      }
    }

    // Find the handler of the command.
    const auto handlerIter = _commandServer._handlers.find(commandId);
    if (handlerIter == _commandServer._handlers.cend())
    {
      if (_commandServer.debugCommands
        && not IsMuted(commandId))
      {
        server::util::QuietLogWarn(
          "Unhandled command '{}' (0x{:x})",
          GetCommandName(commandId),
          magic.id);
      }
    }
    else
    {
      const auto& handler = handlerIter->second;
      // Handler validity is checked when registering.
      assert(handler);

      try
      {
        // Call the handler.
        handler(clientId, commandDataStream);
      }
      catch (const std::exception& x)
      {
        server::util::QuietLogError(
          "Unhandled exception handling command '{}' (0x{:x}): {}",
          protocol::GetCommandName(commandId),
          magic.id,
          x.what());
      }

      // There shouldn't be any left-over data in the stream.
      assert(commandDataStream.GetCursor() == commandDataStream.Size());

      if (_commandServer.debugCommands
        && not IsMuted(commandId))
      {
        server::util::QuietLogDebug(
          "Handled command '{}' (0x{:x})",
          GetCommandName(commandId),
          magic.id);
      }
    }
  }

  return commandStream.GetCursor();
}

void CommandServer::SendCommand(
  ClientId clientId,
  protocol::Command commandId,
  CommandSupplier supplier)
{
  try
  {
    _server.GetClient(clientId)->QueueWrite(
      [this, commandId, supplier = std::move(supplier)](asio::streambuf& writeBuffer)
      {
        const auto mutableBuffer = writeBuffer.prepare(MaxCommandSize);
        const auto writeBufferView = std::span(
          static_cast<std::byte*>(mutableBuffer.data()),
          mutableBuffer.size());

        SinkStream commandSink(writeBufferView);

        const auto streamOrigin = commandSink.GetCursor();
        commandSink.Seek(streamOrigin + sizeof(protocol::MessageMagic));

        // Write the message data.
        supplier(commandSink);

        // Command size is the size of the whole command.
        const size_t commandSize = commandSink.GetCursor();

        if (debugOutgoingCommandData
          && not IsMuted(commandId))
        {
          server::util::QuietLogDebug("Write data for command '{}' (0x{:X}),\n\n"
            "Command data size: {} \n"
            "Data dump: \n\n{}\n",
            GetCommandName(commandId),
            static_cast<uint32_t>(commandId),
            commandSize,
            util::GenerateByteDump(
              std::span(
                static_cast<std::byte*>(mutableBuffer.data()) + sizeof(protocol::MessageMagic),
                commandSize - sizeof(protocol::MessageMagic))));
        }

        // Traverse back the stream before the message data,
        // and write the message magic.
        commandSink.Seek(streamOrigin);

        // Write the message magic.
        const protocol::MessageMagic magic{
          .id = static_cast<uint16_t>(commandId),
          .length = static_cast<uint16_t>(commandSize)};

        commandSink.Write(encode_message_magic(magic));
        writeBuffer.commit(magic.length);

        if (debugCommands
          && not IsMuted(commandId))
        {
          server::util::QuietLogDebug("Sent command message '{}' (0x{:X})",
          GetCommandName(commandId),
          static_cast<uint32_t>(commandId));
        }

        return commandSize;
      });
  }
  catch (std::exception&)
  {
    // the client disconnected, todo dont use client ids, or dont
  }
}

} // namespace server
