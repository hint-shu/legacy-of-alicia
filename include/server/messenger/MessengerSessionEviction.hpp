//
// LOA (R78, round78, backlog #255): вытеснение устаревшей мессенджер-сессии.
//

#ifndef MESSENGERSESSIONEVICTION_HPP
#define MESSENGERSESSIONEVICTION_HPP

#include <libserver/data/DataDefinitions.hpp>
#include <libserver/network/NetworkDefinitions.hpp>

#include <optional>
#include <vector>

namespace server::messenger
{

//! LOA (R78, round78, backlog #255): ОТВЯЗКА ЧУЖИХ СЕССИЙ ОДНОГО ПЕРСОНАЖА.
//!
//! ★ЗАЧЕМ ЭТО ВООБЩЕ НУЖНО. Раунд чинит повторный вход в мессенджер после
//! заезда. Но старое мессенджер-соединение сервер держит вечно (#235), поэтому
//! клиент возвращается ВТОРЫМ соединением (в проде: `[0] ChatCmdLogin` в 09:03,
//! `Client 1` в 09:22). Если после починки входа обе записи останутся
//! привязанными к одному `characterUid`, адресная доставка
//! (`MessengerDirector::GetClientByCharacterUid`) будет выбирать между ними
//! порядком `unordered_map` — то есть личка с шансом примерно один к двум
//! уходила бы в МЁРТВЫЙ сокет, и игрок по-прежнему не видел бы сообщений.
//! Вход обязан оставлять ровно одну живую привязку.
//!
//! ★ПОЧЕМУ ОТВЯЗКА, А НЕ УДАЛЕНИЕ ЗАПИСИ. Значения правятся НА МЕСТЕ: ни
//! вставки, ни удаления, ни рехэша. Карту клиентов мессенджера читают чужие
//! потоки (ранч и заезд ходят в `GetClientByCharacterUid`), и удаление посреди
//! обхода было бы новым классом гонки поверх уже существующей.
//!
//! ★ПОЧЕМУ ФЛАГ СНИМАЕТСЯ ЗДЕСЬ, А НЕ ПРИ ОТКЛЮЧЕНИИ. Уборка разорванного
//! соединения зовёт `HandleChatterUpdateState` со статусом Offline, а тот
//! выходит на `not isAuthenticated`. Снятый заранее флаг — единственное, что
//! не даёт закрытию мёртвого сокета разослать друзьям «игрок вышел» про
//! ЖИВОГО игрока, который только что вошёл.
//!
//! ★ГАРД `InvalidUid` — НЕСУЩИЙ, А НЕ ГИГИЕНА. Свежее соединение до входа
//! лежит в карте с `characterUid == InvalidUid`. Без гарда вызов с невыясненной
//! личностью совпал бы КО ВСЕМ таким записям и закрыл бы соединения ни в чём не
//! повинных клиентов, которые как раз подключаются.
//!
//! @param clients Карта `clientId -> контекст` (контекст обязан иметь поля
//!                `isAuthenticated`, `characterUid`, `otpCode`).
//! @param keepClientId Соединение, которое только что вошло — его не трогаем.
//! @param characterUid Личность, доказанная этим входом.
//! @return Идентификаторы отвязанных соединений. Закрывать их обязан
//!         вызывающий — ВТОРОЙ фазой, вне обхода карты: `Client::End()`
//!         синхронно возвращается в `HandleClientDisconnected`, который
//!         стирает запись, а стирание посреди цикла — инвалидация итератора.
template <typename ContextMap>
std::vector<network::ClientId> UnbindSessionsOfCharacter(
  ContextMap& clients,
  const data::Uid characterUid,
  const std::optional<network::ClientId> keepClientId)
{
  std::vector<network::ClientId> unbound;

  if (characterUid == data::InvalidUid)
    return unbound;

  for (auto& [clientId, clientContext] : clients)
  {
    if (keepClientId.has_value() && clientId == keepClientId.value())
      continue;
    if (clientContext.characterUid != characterUid)
      continue;

    clientContext.isAuthenticated = false;
    clientContext.characterUid = data::InvalidUid;
    clientContext.otpCode.reset();

    unbound.emplace_back(clientId);
  }

  return unbound;
}

//! Вход персонажа: отвязать ВСЕ его сессии, кроме вошедшей.
template <typename ContextMap>
std::vector<network::ClientId> UnbindOtherSessionsOfCharacter(
  ContextMap& clients,
  const network::ClientId keepClientId,
  const data::Uid characterUid)
{
  return UnbindSessionsOfCharacter(clients, characterUid, keepClientId);
}

//! LOA (R78-fix4, round78, backlog #255, находка ревю W1): выход персонажа из
//! игры — отвязать ВСЕ его сессии, не щадя ни одной.
//!
//! ★ЗАЧЕМ ОТДЕЛЬНЫЙ ВХОД, А НЕ «keepClientId = что-нибудь несуществующее».
//! `network::ClientId` — это `size_t`, и НИ ОДНО его значение не запрещено:
//! часового, который заведомо ни с чем не совпадёт, не существует. Поэтому
//! «щадить некого» выражено ТИПОМ (`std::nullopt`), а не магическим числом,
//! которое однажды совпадёт с настоящим соединением.
template <typename ContextMap>
std::vector<network::ClientId> UnbindAllSessionsOfCharacter(
  ContextMap& clients,
  const data::Uid characterUid)
{
  return UnbindSessionsOfCharacter(clients, characterUid, std::nullopt);
}

//! LOA (R78-fix11, round78, backlog #255, находка ревю #4 BLOCK — вторая
//! половина): ЗАПИСЬ, ПО КОТОРОЙ НЕЛЬЗЯ РАССЫЛАТЬ ПРИСУТСТВИЕ.
//!
//! ★ПОЧЕМУ ОДНОГО ФЛАГА МАЛО. Рассылка присутствия обходит снимок карты
//! клиентов и по каждой записи идёт в `GetCharacter(characterUid).Immutable`.
//! `GetCharacter(InvalidUid)` возвращает НЕДОСТУПНУЮ запись, а `Immutable` на
//! ней БРОСАЕТ — и бросок уносит с собой ВЕСЬ обход, то есть уведомления
//! друзьям и гильдии не уходят никому. Одна порченая запись ломает рассылку
//! всему серверу, поэтому пропуск обязан ключиться на том, чего не хватает
//! (личности), а не только на флаге.
//!
//! ★ЛЕГАЛЬНОЙ ЗАПИСИ ЭТО НЕ КАСАЕТСЯ. Вход поднимает флаг раньше, чем
//! публикует личность, но и то и другое делает ОДИН поток мессенджера — тот
//! же, что рассылает присутствие, — поэтому увидеть полусвязанную запись
//! рассылка не может: к моменту любой рассылки личность уже записана.
//! Пропускается ровно недостижимое состояние: пояс, а не логика.
//!
//! ★ЭТО ПОЯС, И ЕГО ФАЛЬСИФИКАТОР — ЮНИТ-ТЕСТ, А НЕ СТЕНД. Пока первичный
//! фикс (R78-fix9) на месте, зомби не появляется, поэтому стенд пояс не
//! краснит. Правило проверяется здесь, детерминированно.
template <typename Context>
bool IsPresenceBroadcastable(const Context& clientContext)
{
  return clientContext.isAuthenticated
    && clientContext.characterUid != data::InvalidUid;
}

} // namespace server::messenger

#endif // MESSENGERSESSIONEVICTION_HPP
