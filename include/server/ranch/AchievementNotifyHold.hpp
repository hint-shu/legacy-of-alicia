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

#ifndef ACHIEVEMENTNOTIFYHOLD_HPP
#define ACHIEVEMENTNOTIFYHOLD_HPP

#include "libserver/data/DataDefinitions.hpp"
#include "libserver/network/command/proto/RaceMessageDefinitions.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <unordered_map>
#include <vector>

namespace server
{

//! LOA (R70-fix-7, backlog #58): ★ПРИДЕРЖАННЫЕ ПОПАПЫ ДОСТИЖЕНИЙ ЗАЕЗДА.
//!
//! ЗАЧЕМ ЭТОТ КЛАСС СУЩЕСТВУЕТ (измерение, а не вкус).
//! `AcCmdRCAchievementUpdateNotify` (0xe4) доставляется ТОЛЬКО по ранчевому
//! соединению — это единственная пара «опкод/сокет», доказанная живым
//! клиентом. Но настоящий клиент ЗАКРЫВАЕТ ранчевую ногу ровно при входе в
//! заезд (снято с захвата; то же делает и сессия тестера,
//! `tester/bot/session.py::_on_room_handoff_wait`). Значит в момент
//! `RaceInstance::Stop()` ранчевого соединения у игрока НЕТ, и правило «нет
//! клиента — выбросить» теряло бы попап ПОЧТИ ВСЕГДА: фича работала бы только
//! списком 0xe6. Поэтому нотификация ПРИДЕРЖИВАЕТСЯ до ближайшего входа
//! персонажа на ранчо.
//!
//! ★ПОЧЕМУ ОТДЕЛЬНЫЙ КЛАСС, А НЕ ПОЛЕ-МАПА В ДИРЕКТОРЕ. Политика удержания —
//! это ТРИ решения (срок, потолок, порядок вытеснения), каждое из которых
//! умеет тихо испортиться: протухание без срока превращает очередь в утечку,
//! потолок без вытеснения — в неограниченный рост от одного игрока, вытеснение
//! не с того конца отдаёт игроку самые старые значки вместо свежих. Внутри
//! директора это проверялось бы только живым стендом (ждать 15 минут);
//! отдельным классом — юнит-тестом с ПОДАННЫМ временем, за миллисекунды.
//! Шов по времени (`now` параметром) существует ровно для этого.
//!
//! ★ПОТОК. Класс НЕ потокобезопасен и не обязан быть: его защищает ЛИСТОВОЙ
//! мьютекс владельца (`RanchDirector::_pendingAchievementNotifiesMutex`).
//! Гоночный поток только кладёт (`Push`), ранч-сетевой тик только разбирает
//! (`Expire`/`Characters`/`Take`). Ни один метод не зовёт наружу, поэтому
//! держать их под листовым локом законно.
class AchievementNotifyHold final
{
public:
  using Clock = std::chrono::steady_clock;
  using Notify = protocol::AcCmdRCAchievementUpdateNotify;

  //! ПОТОЛОК НА ПЕРСОНАЖА. Решение лида: 32 записи.
  //! ★ЧТО ЭТО ЗА ЧИСЛО. Один заезд даёт игроку не больше 17 попаданий
  //! (приёмка раунда: 17 записей / 53 очка), то есть потолок — это примерно
  //! два заезда подряд без возврата на ранчо. Больше держать незачем:
  //! прогресс всё равно записан и виден в списке 0xe6, а очередь обязана иметь
  //! верхнюю границу памяти, не зависящую от поведения игрока.
  static constexpr std::size_t CharacterCap = 32;

  //! @param ttl Срок жизни записи. По истечении она выбрасывается МОЛЧА:
  //!        протухание — это «игрок не вернулся», штатное состояние, а не сбой.
  explicit AchievementNotifyHold(Clock::duration ttl) noexcept
    : _ttl(ttl)
  {
  }

  //! Положить нотификацию персонажу.
  //! ★ВЫТЕСНЯЕТСЯ САМАЯ СТАРАЯ, а не самая новая: свежий значок игроку
  //! интереснее позавчерашнего, и именно свежий он ожидает увидеть, вернувшись
  //! на ранчо.
  //! @returns сколько записей вытеснено потолком (обычно 0).
  std::size_t Push(data::Uid characterUid, const Notify& notify, Clock::time_point now);

  //! Выбросить всё, что старше `ttl`.
  //! @returns сколько записей выброшено.
  std::size_t Expire(Clock::time_point now);

  //! Снимок UID'ов, у которых что-то придержано. Копия, а не ссылка: вызывающий
  //! ходит по нему, ОТПУСТИВ лок.
  [[nodiscard]] std::vector<data::Uid> Characters() const;

  //! Забрать всё придержанное для персонажа (в порядке появления) и стереть
  //! его очередь. Пустой вектор, если ничего нет.
  [[nodiscard]] std::vector<Notify> Take(data::Uid characterUid);

  //! Сколько записей придержано ВСЕГО. ★Существует ради оракула стенда: «после
  //! протухания очередь пуста» проверяется числом, а не отсутствием строк.
  [[nodiscard]] std::size_t HeldCount() const noexcept;

  //! Сколько персонажей имеют придержанные записи.
  [[nodiscard]] std::size_t CharacterCount() const noexcept
  {
    return _held.size();
  }

  //! Поставить срок жизни. ★ЗВАТЬ ТОЛЬКО ДО ПЕРВОГО `Push` — из
  //! `RanchDirector::Initialize`, когда конфиг УЖЕ прочитан (в конструкторе
  //! директора он ещё содержит умолчания: `ServerInstance::Initialize`
  //! загружает YAML ПОСЛЕ построения директоров — проверено по порядку
  //! вызовов, а не по ожиданию).
  void SetTtl(const Clock::duration ttl) noexcept
  {
    _ttl = ttl;
  }

  [[nodiscard]] Clock::duration Ttl() const noexcept
  {
    return _ttl;
  }

private:
  struct Entry
  {
    Notify notify;
    Clock::time_point queuedAt;
  };

  Clock::duration _ttl;
  std::unordered_map<data::Uid, std::deque<Entry>> _held;
};

} // namespace server

#endif // ACHIEVEMENTNOTIFYHOLD_HPP
