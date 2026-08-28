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

#include "libserver/network/Server.hpp"
#include "libserver/util/Cleanup.hpp"
#include "libserver/util/Deferred.hpp"
#include "libserver/util/QuietLog.hpp"

#include <cassert>
#include <ranges>
#include <spdlog/spdlog.h>
#include <stacktrace>

namespace server::network
{

namespace
{

constexpr std::size_t MaxConnectionsPerAddress = 10;
constexpr std::size_t MaxTotalConnections = 1024;
constexpr std::size_t MaxConnectRatePerAddress = 10;
constexpr auto RateWindow = std::chrono::seconds(30);

} // namespace

Client::Client(
  ClientId clientId,
  asio::ip::tcp::socket&& socket,
  asio::ip::address_v4 remoteAddress,
  uint16_t remotePort,
  EventHandlerInterface& networkEventHandler)
  : _clientId(clientId)
  , _remoteAddress(remoteAddress)
  , _remotePort(remotePort)
  , _socket(std::move(socket))
  , _networkEventHandler(networkEventHandler)
{
}

void Client::Begin()
{
  if (_shouldRun.exchange(true, std::memory_order::acq_rel))
    return;

  _networkEventHandler.OnClientConnected(_clientId);

  ReadLoop();
}

void Client::End()
{
  if (not _shouldRun.exchange(false, std::memory_order::seq_cst))
    return;

  try
  {
    if (_socket.is_open())
    {
      _socket.shutdown(asio::socket_base::shutdown_both);
      _socket.close();
    }
  }
  catch (const std::exception&)
  {
    // Ignore
  }

  _networkEventHandler.OnClientDisconnected(_clientId);
}

void Client::QueueWrite(WriteSupplier writeSupplier)
{
  if (not _shouldRun.load(std::memory_order::acquire))
    return;

  {
    std::scoped_lock lock(_writeMutex);
    _writeQueue.emplace(writeSupplier);
  }

  WriteLoop();
}

asio::ip::address_v4 Client::GetAddress() const noexcept
{
  return _remoteAddress;
}

uint16_t Client::GetPort() const noexcept
{
  return _remotePort;
}

namespace
{

//! LOA-fix (R49-15a, round49, backlog #178): уборка соединения — сама бросающая
//! операция (`Client::End` доходит до `OnClientDisconnected` обработчика
//! событий), а зовут её ИЗ ВЕТКИ ПЕРЕХВАТА asio-хендлера. Пояс функции сюда не
//! достаёт: хендлер вызывается позже и уже не из тела `ReadLoop`/`WriteLoop`, а
//! из очереди asio. Бросок оттуда доезжал до функции потока и клал ВЕСЬ сервер
//! ради одного клиента — стенд поймал это ячейкой `end`.
void EndQuietly(const std::shared_ptr<Client>& client) noexcept
{
  try
  {
    client->End();
  }
  catch (...)
  {
    // Молча глотать нельзя: без строки в логе такой сбой невидим, а он значит,
    // что соединение осталось убрано не до конца.
    util::QuietLogWarn("Unhandled exception while ending a client connection");
  }
}

} // anon namespace

// LOA-fix (R49-12a, round49, backlog #178): ФУНКЦИОНАЛЬНЫЙ try — пояс на всю
// функцию, а не на один перехват внутри. Бросить умеет не только поставщик
// данных: инициация асинхронной записи, блокировка, профилировщик и даже
// уборка `End()` в ветке перехвата — а функция noexcept, и любой такой бросок
// был бы смертью процесса. Выход за конец обработчика у void-функции
// равнозначен `return;`.
void Client::WriteLoop() noexcept
try
{
  // todo: forgive me for this, its not clean, its not pretty and i'm pretty sure there some side effects
  //       i'll nuke it in the future

  if (not _shouldRun.load(std::memory_order::acquire))
    return;

  // Незамкнутая проверка оставлена как оптимизация горячего пути: она дёшево
  // отсекает подавляющее большинство лишних заходов.
  if (_isSending.load(std::memory_order::acquire))
    return;

  std::scoped_lock lock(_writeMutex);

  // LOA-fix (R61-9, round61, backlog #182): ПОВТОРНАЯ ПРОВЕРКА ПОД ЗАМКОМ.
  // ★Проверка выше читает флаг ДО взятия замка, то есть по устаревшему
  // значению. Гонка (требует трёх участников, потому и редка, но заезды на
  // 50 Гц узкие окна достают): поток A прошёл проверку, взял замок, опустошил
  // очередь, поднял флаг, запустил `async_write_some`, отпустил замок. Поток C
  // прошёл ту же проверку РАНЬШЕ, пока флаг был снят, и всё это время ждал на
  // замке. Между освобождением замка потоком A и захватом его потоком C поток D
  // кладёт в очередь новую команду. C получает замок, очередь НЕ пуста — и C
  // запускает ВТОРОЙ `async_write_some` по тому же сокету, пока первый в полёте.
  // ★ВРЕД НЕ В «ПЕРЕМЕШАННЫХ БАЙТАХ». Второй дренаж зовёт `_writeBuffer.prepare`,
  // а тот умеет ПЕРЕАЛЛОЦИРОВАТЬ вектор под буфером, который уже держит летящий
  // `async_write_some(_writeBuffer.data(), ...)`. Это heap-use-after-free с
  // отправкой куска чужой кучи в сокет.
  // ★ПОТЕРЯННОГО ПРОБУЖДЕНИЯ НЕ ВОЗНИКАЕТ: снимающий флаг СРАЗУ следом зовёт
  // `WriteLoop`, а кладущий в очередь кладёт ДО чтения флага.
  if (_isSending.load(std::memory_order::acquire))
    return;

  // LOA-fix (R61-9b, round61, backlog #207): ВЫХОД ТОЛЬКО КОГДА ПУСТО И В
  // ОЧЕРЕДИ, И В БУФЕРЕ. `async_write_some` пишет СКОЛЬКО ПОЛУЧИТСЯ, а не всё;
  // обработчик завершения потребляет ровно записанное и снова зовёт эту
  // функцию. Прежний выход по одной лишь пустой очереди оставлял непосланный
  // хвост лежать в буфере до тех пор, пока кто-нибудь не положит СЛЕДУЮЩУЮ
  // команду. Клиент в это время ждёт продолжение обрезанной команды: в заезде
  // следующая команда приходит через миллисекунды и симптом лечится сам, а в
  // лобби на простое — нет, и клиент висит до таймаута.
  if (_writeQueue.empty() && _writeBuffer.size() == 0)
  {
    return;
  }

  // Write the suppliers to the write buffer.
  while (not _writeQueue.empty())
  {
    try
    {
      const auto& supplier = _writeQueue.front();
      supplier(_writeBuffer);
      _writeQueue.pop();
    }
    catch (const std::exception& e)
    {
      // LOA-fix (R49-2c, round49, backlog #178): перехват стоит ПРЯМО в теле
      // noexcept-функции — ни один внешний пояс сюда не дотянется.
      util::QuietLogError("Unhandled exception in supplier of write data for client {}: {}",
        _clientId,
        e.what());
      End();
      return;
    }
    catch (...)
    {
      util::QuietLogError("Unhandled unknown exception in supplier of write data for client {}",
        _clientId);
      End();
      return;
    }
  }

  _isSending.store(true, std::memory_order::release);

  // Asynchronously write the data to the socket.
  _writeProfiler.Start();
  _socket.async_write_some(
    _writeBuffer.data(),
    [clientPtr = this->shared_from_this()](const boost::system::error_code& error, const std::size_t size)
    {
      try
      {
        if (error)
        {
          switch (error.value())
          {
            case asio::error::operation_aborted:
              throw std::runtime_error("Connection aborted by the server");
            case asio::error::connection_reset:
              throw std::runtime_error("Connection reset by the client");
            default:
              throw std::runtime_error(
                std::format("Generic network error {}", error.message()));
          }
        }

        // Consume the sent bytes.
        {
          std::scoped_lock lock(clientPtr->_writeMutex);
          clientPtr->_writeBuffer.consume(size);
        }
      }
      catch (const std::exception& x)
      {
        util::QuietLogDebug(
          "Client {} is disconnecting because of read loop exception: {}",
          clientPtr->_clientId,
          x.what());

        EndQuietly(clientPtr);
      }
      catch (...)
      {
        util::QuietLogDebug(
          "Client {} is disconnecting because of an unknown write loop exception",
          clientPtr->_clientId);

        EndQuietly(clientPtr);
      }

      clientPtr->_isSending.store(false, std::memory_order::release);
      clientPtr->_writeProfiler.Stop();
      clientPtr->WriteLoop();
    });
}
catch (...)
{
  util::QuietLogError("Unhandled exception in the write loop of client {}", _clientId);
  // Соединение бросаем намеренно: после броска на полпути флаг отправки мог
  // остаться поднятым, и клиент замолчал бы навсегда. Уборка тоже под своим
  // перехватом — из обработчика функционального try бросать нельзя.
  try
  {
    End();
  }
  catch (...)
  {
  }
}

// LOA-fix (R49-12c, round49, backlog #178): см. R49-12a — пояс на всю функцию.
void Client::ReadLoop() noexcept
try
{
  if (not _shouldRun.load(std::memory_order::acquire))
    return;

  _readProfiler.Start();
  _socket.async_read_some(
    _readBuffer.prepare(1024),
    [clientPtr = this->shared_from_this()](boost::system::error_code error, std::size_t size)
    {
      try
      {
        if (error)
        {
          switch (error.value())
          {
            case asio::error::operation_aborted:
              throw std::runtime_error("Connection aborted by the server");
            case asio::error::misc_errors::eof:
            case asio::error::connection_reset:
              throw std::runtime_error("Connection reset by the client");
            default:
              throw std::runtime_error(
                std::format("Generic network error {}", error.message()));
          }
        }

        clientPtr->_readBuffer.commit(size);

        const std::span receivedData{
          static_cast<const std::byte*>(clientPtr->_readBuffer.data().data()),
          clientPtr->_readBuffer.data().size()};

        const auto consumedBytes = clientPtr->_networkEventHandler.OnClientData(
          clientPtr->_clientId,
          receivedData);

        clientPtr->_readBuffer.consume(consumedBytes);

        // Continue the read loop.
        clientPtr->_readProfiler.Stop();
        clientPtr->ReadLoop();
      }
      catch (const std::exception& x)
      {
        util::QuietLogDebug(
          "Client {} is disconnecting because of read loop exception: {}",
          clientPtr->_clientId,
          x.what());

        EndQuietly(clientPtr);
      }
      catch (...)
      {
        util::QuietLogDebug(
          "Client {} is disconnecting because of an unknown read loop exception",
          clientPtr->_clientId);

        EndQuietly(clientPtr);
      }
    });
}
catch (...)
{
  util::QuietLogError("Unhandled exception in the read loop of client {}", _clientId);
  try
  {
    End();
  }
  catch (...)
  {
  }
}

// LOA-fix (R51-7, round51, backlog #179): `noexcept` снят — контекст
// ввода-вывода и акцептор выделяют память при конструировании, то есть
// обещание не бросать было ложным и здесь. Сервер конструируется на
// старте: честный бросок даёт внятный отказ запуска вместо terminate.
Server::Server(EventHandlerInterface& networkEventHandler)
  : _acceptor(_io_ctx)
  , _timer(_io_ctx)
  , _networkEventHandler(networkEventHandler)
{
}

void Server::Begin(const asio::ip::address& address, uint16_t port)
{
  const asio::ip::tcp::endpoint server_endpoint(address, port);

  try
  {
    _acceptor.open(server_endpoint.protocol());
    _acceptor.bind(server_endpoint);
    _acceptor.listen();
  }
  catch (const std::exception& x)
  {
    throw std::runtime_error(
      std::format(
        "Exception while trying to host server on {}:{}: {}",
        address.to_string(),
        port,
        x.what()));
  }

  // Run the accept loop.
  AcceptLoop();

  try
  {
    // Run the tick loop.
    TickLoop();
  }
  catch (const std::exception& x)
  {
    throw std::runtime_error(
      std::format(
        "Exception in network tick loop: {}",
        x.what()));
  }

  try
  {
    _io_ctx.run();
  }
  catch (const std::exception& x)
  {
    throw std::runtime_error(
      std::format(
        "Exception in asio IO context: {}",
        x.what()));
  }
}

void Server::End() noexcept
{
  // LOA-fix (R49-17, round49, backlog #178): остановка сервера обязана быть
  // НЕБРОСАЮЩЕЙ в самом корне, а не «обёрнутой у вызывающего». Причины две.
  // (1) Её зовут из веток перехвата под функциями потоков. (2) Бросающая
  // перегрузка `close()` пропускала бы `stop()` — контекст остался бы жить, и
  // поток сервера никогда бы не завершился; дальше деструктор либо ждёт вечно,
  // либо (если бы мы малодушно отцепили поток) оставил бы живой поток поверх
  // разрушаемого объекта. Поэтому закрываем акцептор через код ошибки и
  // ОСТАНАВЛИВАЕМ контекст ВСЕГДА.
  boost::system::error_code error;
  _acceptor.close(error);

  // ★Остановка контекста — СРАЗУ, до всякой диагностики: она и есть то, ради
  // чего функция существует, и ни одна жалоба не вправе её отменить.
  _io_ctx.stop();

  if (error)
  {
    // ★`error.message()` возвращает std::string, то есть ВЫДЕЛЯЕТ ПАМЯТЬ и умеет
    // бросить — а вычисляется он ДО входа в тихую жалобу, снаружи её перехвата.
    // Поэтому вся сборка сообщения стоит под собственным перехватом.
    try
    {
      util::QuietLogWarn("Failed to close the server acceptor: {}", error.message());
    }
    catch (...)
    {
    }
  }
}

std::shared_ptr<Client> Server::GetClient(ClientId clientId)
{
  // LOA-fix (R61-3, round61, backlog #202): чтение карты под РАЗДЕЛЯЕМЫМ замком.
  // Это тот самый путь, которым потоки директоров ходят в карту на каждой
  // отправке команды, пока свой сетевой поток её мутирует.
  // ★ЗАМОК СНИМАЕТСЯ ДО ВОЗВРАТА, и это существенно: наружу уезжает КОПИЯ
  // `shared_ptr`, а не ссылка внутрь карты, поэтому объект переживёт любой
  // последующий рехэш и удаление записи. Держать замок дольше нельзя — вызывающий
  // тут же уходит в работу с клиентом, и удержание превратило бы разделяемый
  // замок в очередь на горячем пути заезда.
  std::shared_ptr<Client> client;
  {
    std::shared_lock lock(_clientsMutex);
    const auto clientItr = _clients.find(clientId);
    if (clientItr != _clients.end())
      client = clientItr->second;
  }

  if (not client)
  {
    throw std::runtime_error("Invalid client");
  }

  return client;
}

void Server::HandleNetworkTick()
{
}

void Server::OnClientConnected(
  ClientId clientId)
{
  _networkEventHandler.OnClientConnected(clientId);
}

void Server::OnClientDisconnected(
  ClientId clientId)
{
  // LOA-fix (R37-3, round37, backlog #122): здесь стоял `assert`, а прод
  // собирается с -DNDEBUG — то есть в боевом билде проверки НЕ БЫЛО ВООБЩЕ и
  // промах find дал бы разыменование end(). В PRE-бинаре эта ветка ведёт в
  // .cold-клон `mov 0x10,%rax; ud2` — то есть краш В ЛЮБОМ СЛУЧАЕ: сначала
  // SIGSEGV на чтении по адресу 0x10, а ud2 (illegal instruction) стоит следом.
  // ★ТОЧНОСТЬ (NIT ревью round37): раньше здесь говорилось просто «SIGILL» —
  // формально первым срабатывает SIGSEGV, ud2 лишь замыкает ветку.
  // Сегодня промах невозможен
  // (Client::End() гарантирует ровно одно уведомление на клиента, гард
  // `_shouldRun.exchange(false)`), поэтому это гигиена, а не живой краш.
  // Меняем «ассерт, которого нет в проде» на честную проверку: лог + выход.
  // LOA-fix (R61-4, round61, backlog #202): поиск под РАЗДЕЛЯЕМЫМ замком, и
  // наружу берётся КОПИЯ указателя, а не итератор. Итератор пережить чужую
  // вставку не может, а копия может — и это важно именно здесь, потому что
  // ниже управление уходит в обработчик разрыва директора.
  std::shared_ptr<Client> disconnectingClient;
  {
    std::shared_lock lock(_clientsMutex);
    const auto clientIt = _clients.find(clientId);
    if (clientIt != _clients.end())
      disconnectingClient = clientIt->second;
  }

  if (not disconnectingClient)
  {
    server::util::QuietLogWarn(
      "Disconnect notification for an unknown client {} (already removed?)",
      clientId);
    return;
  }

  const auto address = disconnectingClient->GetAddress();
  OnThrottleDisconnect(address);

  // LOA-fix (R49-21, round49, backlog #178): обработчик разрыва делает
  // бросающую работу (лобби, например, ещё и ставит задачу в очередь). Раньше
  // его бросок уносил процесс; теперь он не должен уносить и реестр. Удаление
  // выполняется в ЛЮБОМ случае и ПО КЛЮЧУ — итератор мог быть испорчен
  // обработчиком, если тот сам дошёл до реестра.
  try
  {
    _networkEventHandler.OnClientDisconnected(clientId);
  }
  catch (const std::exception& x)
  {
    util::QuietLogWarn(
      "Disconnect handler of client {} failed: {}", clientId, x.what());
  }
  catch (...)
  {
    util::QuietLogWarn("Disconnect handler of client {} failed", clientId);
  }

  // LOA-fix (R61-6, round61, backlog #202): удаление под ИСКЛЮЧИТЕЛЬНЫМ замком.
  // ★ЗАМОК БЕРЁТСЯ ЗДЕСЬ, А НЕ ВЫШЕ, И ЭТО НЕ МЕЛОЧЬ. Между поиском и этой
  // строкой управление уходит в `_networkEventHandler.OnClientDisconnected` —
  // то есть в код директоров, который сам ходит в сеть и может вернуться сюда
  // же. Замок, удержанный через этот вызов, дал бы самодедлок: ровно так в R59
  // (#209) `unique_lock` поверх реэнтрантного `AcceptLogin` уронил вход
  // полностью — 0 успешных логинов из 24 на стенде. `shared_mutex` НЕ
  // рекурсивен, повторный захват на том же потоке — неопределённое поведение.
  {
    std::unique_lock lock(_clientsMutex);
    _clients.erase(clientId);
  }
}

size_t Server::OnClientData(
  ClientId clientId,
  const std::span<const std::byte>& data)
{
  return _networkEventHandler.OnClientData(clientId, data);
}

// LOA-fix (R51-2, round51, backlog #179): СНЯТ `noexcept`. Функция растит
// реестр адресов и очередь меток времени, то есть выделяет память, — а стояла
// под `noexcept`, где любой сбой это `std::terminate`. Новое поведение отказа
// придумывать не пришлось: оно уже определено вызывающим — пояс приёма
// соединения (R50-2) напишет строку, ЭТО соединение не состоится, приём
// переармируется. Контракт просто перестаёт лгать.
bool Server::IsConnectionThrottled(const asio::ip::address_v4& address)
{
  auto& state = _addressStates[address];
  // If there are more active connections than allowed by `MaxConnectionsPerAddress`
  // throttle the connection from the address.
  if (state.activeConnections >= MaxConnectionsPerAddress)
    return true;

  const auto now = std::chrono::steady_clock::now();

  // Pop the connection timestamps which are over the rate window and have expired.
  while (not state.connectionTimestamps.empty())
  {
    const auto timeSinceConnection = now - state.connectionTimestamps.front();
    if (timeSinceConnection < RateWindow)
      break;

    state.connectionTimestamps.pop_front();
  }

  // If there are more connection attempts than allowed by `MaxConnectRatePerAddress`
  // throttle the connection from the address.
  if (state.connectionTimestamps.size() >= MaxConnectRatePerAddress)
    return true;

  // ★ПОРЯДОК ОПЕРАЦИЙ — ВТОРОЙ ДЕФЕКТ ЭТОЙ ФУНКЦИИ, класс #180 в новом месте.
  // Раньше счётчик активных соединений увеличивался ДО `push_back`. Сбой
  // выделения в `push_back` оставлял счётчик завышенным НАВСЕГДА: уменьшает
  // его только разрыв соединения, а этого соединения не будет никогда — то
  // есть адрес оказывался заблокирован до перезапуска сервера. Теперь сначала
  // растёт очередь меток (сбой здесь не меняет ничего), и только потом
  // считается соединение.
  state.connectionTimestamps.push_back(now);
  state.activeConnections++;

  return false;
}

void Server::OnThrottleDisconnect(const asio::ip::address_v4& address) noexcept
{
  const auto it = _addressStates.find(address);
  if (it == _addressStates.end())
  {
    return;
  }

  if (it->second.activeConnections > 0)
  {
    --it->second.activeConnections;
  }

  if (it->second.activeConnections == 0 &&
      it->second.connectionTimestamps.empty())
  {
    _addressStates.erase(it);
  }
}

// LOA-fix (R49-12e, round49, backlog #178): пояс на всю функцию. Сама инициация
// `async_accept` и работа с реестром соединений тоже умеют бросить, а функция
// noexcept. Цена перехвата — сервер перестаёт принимать НОВЫЕ соединения (и
// говорит об этом в лог); цена его отсутствия — смерть всего процесса вместе с
// уже играющими.
void Server::AcceptLoop() noexcept
try
{
  _acceptor.async_accept(
    [&](const boost::system::error_code& error, asio::ip::tcp::socket client_socket)
    {
      // LOA-fix (R51-1c/R51-1e, round51, backlog #179): адрес, порт и признак
      // «единица троттлинга числится за нами» объявлены ДО `try` — ветки
      // перехвата обязаны их видеть, иначе откатывать будет нечего.
      asio::ip::address_v4 remoteAddress;
      uint16_t remotePort{0};
      bool connectionCounted = false;

      try
      {
        // LOA-fix (R50-2b, round50, backlog #180): отказ САМОГО акцептора —
        // единственный путь, который переармировать НЕЛЬЗЯ. При закрытии
        // слушателя (Server::End) сюда приходит operation_aborted, при
        // исчерпании дескрипторов — постоянная ошибка; повторный арм в обоих
        // случаях означал бы петлю «арм → мгновенный отказ → арм» на полном
        // ходу процессора. Поведение прежнее (приём прекращается), строка лога
        // прежняя — раньше её печатал общий перехват ниже.
        if (error)
        {
          util::QuietLogError(
            "Error in the server accept loop: Network exception 0x{}",
            error.value());
          return;
        }

        try
        {
          // LOA-fix (R51-1c, round51, backlog #179): адрес И ПОРТ читаются
          // ЗДЕСЬ, в единственном месте, где осечка уже осмыслена (соединение
          // оборвалось слишком рано — продолжаем принимать). Конструктору они
          // передаются готовыми, поэтому спрашивать их у сокета второй раз, да
          // ещё из noexcept-контекста, больше не нужно.
          const auto endpoint = client_socket.remote_endpoint();
          remoteAddress = endpoint.address().to_v4();
          remotePort = endpoint.port();
        }
        catch (const std::exception&)
        {
          // The connection was broken too early.
          // Continue the accept loop.
          AcceptLoop();
          return;
        }

        // LOA-fix (R51-1e, round51, backlog #179): ОТКАТ СЧЁТА СОЕДИНЕНИЯ.
        // Гейт троттлинга не просто проверяет — он СЧИТАЕТ соединение за
        // адресом, а уменьшает счётчик только разрыв. Если между гейтом и живым
        // клиентом что-то бросит (выделение буферов, реестр), разрыва не будет
        // никогда и счётчик останется завышенным — адрес окажется заблокирован
        // до перезапуска. Флаг говорит, за кем сейчас числится единица.
        if (IsConnectionThrottled(remoteAddress))
        {
          util::QuietLogWarn(
            "Connection rejected from {} (throttled)",
            remoteAddress.to_string());
          client_socket.close();
          AcceptLoop();
          return;
        }

        // С этой строки единица троттлинга числится за НАМИ: гейт выше её
        // засчитал, а живого клиента, который её вернёт разрывом, ещё нет.
        connectionCounted = true;

        // Sequential Id.
        const ClientId clientId = _client_id++;

        // Create the client.
        // LOA-fix (R61-7, round61, backlog #202): вставка под ИСКЛЮЧИТЕЛЬНЫМ
        // замком. Это второй из двух писателей в карту; именно рост карты
        // вызывает рехэш, под которым чужой `find` из `GetClient` разъезжается.
        // ★ОБЪЕКТ СОЗДАЁТСЯ ДО ЗАМКА (находка ревью): под замком остаётся только
        // перемещение готового указателя в карту. Конструирование `Client`
        // трогает сокет и буферы, и держать на нём исключительный замок карты
        // значило бы задерживать ВСЕ отправки команд всех потоков.
        // ★ЗАМОК ОТПУСКАЕТСЯ ДО `Begin()`: тот уводит управление в обработчики
        // подключения ВСЕХ директоров, и удержание замка через них было бы той
        // же ошибкой, что стоила R59 полного отказа входа.
        auto createdClient = std::make_shared<Client>(clientId,
          std::move(client_socket),
          remoteAddress,
          remotePort,
          *this);

        std::shared_ptr<Client> newClient;
        {
          std::unique_lock lock(_clientsMutex);
          const auto [itr, emplaced] = _clients.try_emplace(
            clientId, std::move(createdClient));

          // Id is sequential so emplacement should never fail.
          assert(emplaced);
          newClient = itr->second;
        }

        // LOA-fix (R50-3, round50, backlog #180): ПОЛУПОДНЯТЫЙ КЛИЕНТ.
        // `Client::Begin()` сначала ставит `_shouldRun = true`, потом уводит
        // управление в обработчики подключения ВСЕХ директоров и только затем
        // запускает читающий цикл. Бросок из середины оставлял клиента в
        // реестре с ОТКРЫТЫМ сокетом и БЕЗ чтения: EOF не придёт никогда,
        // значит `Client::End()` не позовёт никто и уведомления о разрыве не
        // будет вовсе — сокет и запись жили бы до конца процесса. Убираем сами,
        // тем же путём, что и обычный разрыв.
        // ★КОПИЯ shared_ptr ОБЯЗАТЕЛЬНА, а не `itr->second`: уборка доходит до
        // `Server::_clients.erase`, то есть разрушает ту самую ячейку map. Без
        // своей ссылки объект умер бы прямо под `Client::End()` (тот же приём,
        // что у asio-обработчиков с `shared_from_this`).
        const auto client = newClient;
        try
        {
          client->Begin();
        }
        catch (...)
        {
          // Уборка доходит до `OnThrottleDisconnect`, то есть единицу вернёт
          // она — нам откатывать уже нечего.
          EndQuietly(client);
          connectionCounted = false;
          throw;
        }

        // Клиент жив: за возврат единицы отвечает его разрыв.
        connectionCounted = false;

        // Continue the accept loop.
        AcceptLoop();
      }
      catch (const std::exception& x)
      {
        // Клиента не случилось, а единица за адресом засчитана — возвращаем,
        // иначе адрес копил бы её каждую неудачную попытку.
        if (connectionCounted)
          OnThrottleDisconnect(remoteAddress);

        util::QuietLogError(
          "Error in the server accept loop: {}",
          x.what());

        // LOA-fix (R50-2, round50, backlog #180): ПЕРЕАРМИРОВАНИЕ ПРИЁМА.
        // Раньше сбой на приёме соединения (создание клиента, обработчики
        // подключения) уходил сюда, писал строку — и всё: `async_accept` больше
        // не заводился НИКОГДА. Слушатель остаётся открытым, порт отвечает, уже
        // играющие играют, а новые войти не могут до перезапуска. Петли тут
        // быть не может: в эту ветку попадают только сбои ПОСЛЕ принятого
        // соединения, а отказ самого акцептора отсечён в R50-2b.
        AcceptLoop();
      }
      catch (...)
      {
        if (connectionCounted)
          OnThrottleDisconnect(remoteAddress);

        util::QuietLogError("Unknown error in the server accept loop");
        AcceptLoop();
      }
    });
}
catch (...)
{
  util::QuietLogError("Unhandled exception while arming the server accept loop");
}

void Server::TickLoop() noexcept
{
  // LOA-fix (R49-2b, round49, backlog #178): ПОЯС НА ГРАНИЦЕ noexcept.
  // Пропущенный тик — потеря одной секунды работы отложенных задач; бросок
  // мимо noexcept — потеря всего сервера. Жалоба тихая: обычный
  // spdlog::error, стоящий в ветке перехвата под noexcept, сам способен
  // перебросить (SPDLOG_LOGGER_CATCH перебрасывает не-std исключения) и
  // убить процесс ровно там, где мы страховались от падения.
  try
  {
    _networkEventHandler.HandleNetworkTick();
  }
  catch (const std::exception& x)
  {
    util::QuietLogError("Unhandled exception in a network tick: {}", x.what());
  }
  catch (...)
  {
    util::QuietLogError("Unhandled unknown exception in a network tick");
  }

  // LOA-fix (R49-12g, round49, backlog #178): переармирование таймера — тоже
  // бросающая операция внутри noexcept-функции, и оно НАМЕРЕННО в своём поясе:
  // общий с тиком означал бы, что первый же сбой обработчика останавливает
  // тиканье навсегда.
  try
  {
    _timer.expires_after(std::chrono::seconds(1));
    _timer.async_wait([this](const boost::system::error_code& error)
      {
        if (error)
          return;

        TickLoop();
      });
  }
  catch (const std::exception& x)
  {
    util::QuietLogError("Failed to rearm the network tick timer: {}", x.what());
  }
  catch (...)
  {
    util::QuietLogError("Failed to rearm the network tick timer: unknown exception");
  }
}

} // namespace server::network
