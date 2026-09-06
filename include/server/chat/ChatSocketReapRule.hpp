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

#ifndef CHATSOCKETREAPRULE_HPP
#define CHATSOCKETREAPRULE_HPP

//! LOA (R80-1, round80, backlog #235): ПРАВИЛО «КОГО ЖАТЬ» — ЦЕЛИКОМ И ОТДЕЛЬНО.
//!
//! ★ЗАЧЕМ ОТДЕЛЬНЫЙ ЗАГОЛОВОК. Это единственное место раунда, где можно
//! ошибиться тихо и дорого: закрыть сокет живому игроку. Чистая функция от
//! явных аргументов проверяется таблицей в юнит-тесте БЕЗ подъёма сервера —
//! ровно так же, как R78 вынес `MessengerSessionEviction.hpp`.
//!
//! ★ПОЧЕМУ `constexpr` В ЗАГОЛОВКЕ, А НЕ ОПРЕДЕЛЕНИЕ В `.cpp`. Правило берут
//! ДВА директора и юнит-тест; ступенью лесенки оно не является (символы лесенки
//! — существующие функции обоих директоров), поэтому довод R71/R72 «определение
//! в .cpp, чтобы символ дожил до `nm`» здесь не действует.

#include <libserver/data/DataDefinitions.hpp>

#include <chrono>

namespace server::chat
{

//! ★МОНОТОННЫЕ ЧАСЫ, И ЭТО НЕСУЩЕЕ. `system_clock` можно перевести (NTP, смена
//! TZ), и перевод назад сделал бы все сроки недостижимыми, а перевод вперёд —
//! мгновенно достижимыми, то есть выкосил бы живых игроков.
using ReapClock = std::chrono::steady_clock;

//! Почему сокет подлежит закрытию. `Keep` — не подлежит.
enum class ReapVerdict
{
  Keep,
  //! P1: рукопожатие не состоялось за отведённый срок.
  Handshake,
  //! P2: аутентифицирован, но персонажа нет в игре.
  Orphan,
  //! P3: абсолютный простой (по умолчанию ВЫКЛЮЧЕН).
  Idle
};

//! Пороги. Ноль у `absoluteIdle` означает ВЫКЛЮЧЕНО (см. `Config::ChatReap`);
//! ноль у остальных двух конфигом НЕ ДОПУСКАЕТСЯ.
struct ReapThresholds
{
  std::chrono::seconds handshakeTimeout{60};
  std::chrono::seconds orphanGrace{60};
  std::chrono::seconds absoluteIdle{0};
};

//! Состояние одного чат-сокета, снятое директором.
struct ChatSocketState
{
  bool isAuthenticated{false};
  data::Uid characterUid{data::InvalidUid};
  ReapClock::time_point connectedAt{};
  ReapClock::time_point lastActivity{};
};

//! ★ПРАВИЛО ЦЕЛИКОМ. Ни времени, ни карт, ни логов внутри — всё приходит
//! аргументами, поэтому его можно доказать таблицей.
//!
//! @param state       Состояние сокета.
//! @param characterIsInGame Есть ли `state.characterUid` в снимке лобби.
//!        ★Смысл параметра важнее его типа: это ЛИЧНОСТЬ, а не адрес. Правило
//!        гейта окна выката (#234) вынужденно судило по адресу пира, потому что
//!        снаружи личности не видно; внутри сервера она есть, и она строго
//!        лучше — NAT даёт ложные ответы в обе стороны.
//! @param now         Текущий момент.
//! @param thresholds  Пороги.
//! @returns Вердикт; `Keep` — сокет не трогаем.
[[nodiscard]] constexpr ReapVerdict DecideChatSocketReap(
  const ChatSocketState& state,
  const bool characterIsInGame,
  const ReapClock::time_point now,
  const ReapThresholds& thresholds) noexcept
{
  // ★ЧТО СЧИТАЕТСЯ ПРИВЯЗАННОЙ СЕССИЕЙ. Флага мало: он и личность ставятся
  // РАЗНЫМИ операторами, и между ними лежит тяжёлое тело входа. У мессенджера
  // `isAuthenticated = authorized` стоит ДО чтения записи персонажа через
  // `GetCharacter(...).Mutable(...)`, а тот БРОСАЕТ на недоступной записи
  // (`Record.hpp`). Бросок глушит пер-хендлерный `catch` в
  // `ChatterServer::OnClientData`, и соединение живёт дальше — с поднятым
  // флагом и `InvalidUid` НАВСЕГДА.
  // ★Такой сокет судим по P1 от подключения: это строго безопаснее, потому что
  // новых ложных срабатываний не создаёт (клиент, чей вход не состоялся за
  // `handshakeTimeout`, был бы зажат P1 и без флага) и снимает бессмертие.
  // Вернуть здесь `Keep` значило бы завести свой собственный класс утечки
  // взамен закрытого.
  const bool bound = state.isAuthenticated
    && state.characterUid != data::InvalidUid;

  // P1 — РУКОПОЖАТИЕ. Считается от МОМЕНТА ПОДКЛЮЧЕНИЯ, а НЕ от последней
  // активности, и это не стилистика: пир, который шлёт мусор раз в секунду и
  // никогда не входит, при отсчёте от активности жил бы вечно — то есть сканер
  // обходил бы гард одним циклом write(). Настоящее рукопожатие занимает
  // миллисекунды (замер прода: connect и ChatCmdLogin в одну миллисекунду),
  // запас шестидесятикратный.
  if (not bound)
  {
    return (now - state.connectedAt) > thresholds.handshakeTimeout
      ? ReapVerdict::Handshake
      : ReapVerdict::Keep;
  }

  // ★НИЖЕ — ТОЛЬКО ПРИВЯЗАННЫЕ СЕССИИ: флаг поднят И личность опубликована.
  // P2 — СИРОТА. Два условия И, и оба обязательны:
  //   • персонажа НЕТ в игре — это несущее условие. Логаут доезжает до
  //     реестра лобби за ≈1 с, грейс 60 с — шестидесятикратный запас;
  //   • сокет молчит дольше грейса — это ремень: он гасит гонку «лобби-сессия
  //     ещё не создана / уже снята, а чат-сокет живой и активный».
  if (not characterIsInGame
    && (now - state.lastActivity) > thresholds.orphanGrace)
  {
    return ReapVerdict::Orphan;
  }

  // P3 — АБСОЛЮТНЫЙ ПРОСТОЙ. ★НОЛЬ = ВЫКЛЮЧЕНО, и это умолчание. В
  // чат-протоколе НЕТ heartbeat: молчание живого клиента законно и может
  // длиться часами (человек стоит на ранчо и не пишет в чат). Измерено: живой
  // мессенджер-сокет играющего человека имел простой 7 минут. Любой включённый
  // порог здесь — потенциальный кик живого игрока. Ключ существует, чтобы
  // стенд мог ДОКАЗАТЬ, что механизм работает; прод остаётся на нуле.
  if (thresholds.absoluteIdle > std::chrono::seconds::zero()
    && (now - state.lastActivity) > thresholds.absoluteIdle)
  {
    return ReapVerdict::Idle;
  }

  return ReapVerdict::Keep;
}

} // namespace server::chat

#endif // CHATSOCKETREAPRULE_HPP
