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


#ifndef LOGTHROTTLE_HPP
#define LOGTHROTTLE_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

namespace server::util
{

//! LOA-fix (R72-1, round72, backlog #129-S1): ДРОССЕЛЬ ДЛЯ СТРОК ЛОГА,
//! КОТОРЫЕ ПОРОЖДАЕТ КЛИЕНТ.
//!
//! Пропускает не чаще одной строки в окно и сообщает вызывающему, сколько
//! событий было проглочено с прошлой выпущенной строки, чтобы «одна строка»
//! не читалась как «случилось один раз».
//!
//! ★ЗАЧЕМ КЛАСС ВООБЩЕ. В дереве не было ни одного дросселя логов. Был только
//! приём «одна строка на непрерывную полосу отказов» через
//! `std::atomic<bool>::exchange` (`RanchDirector.cpp`, R51) — он годится для
//! ПЕРЕХОДА СОСТОЯНИЯ, но не для «клиент шлёт 10 000 пакетов». Нужен дроссель
//! по ОКНУ ВРЕМЕНИ, иначе новый гейт авторизации сам стал бы тем флудом,
//! который этот же раунд убирает у `HandleFeatureCommand`/`HandleClientNotify`.
//!
//! ★«ПОНИЗИТЬ ДО debug» — НЕ РЕШЕНИЕ: уровень debug в проде ВКЛЮЧЁН
//! (`src/server/main.cpp`, `set_level(spdlog::level::debug)`), то есть это был
//! бы тот же флуд другой буквой.
class LogThrottle final
{
public:
  using Clock = std::chrono::steady_clock;

  //! ★КОНСТРУКТОР — INLINE В ЗАГОЛОВКЕ. Поле-член класса-владельца
  //! инициализируется NSDMI, то есть конструктор зовётся из ДРУГОГО TU;
  //! определение в .cpp тоже сработало бы, но лишний межмодульный вызов
  //! ничего не даёт, а риск «объявлен и нигде не определён» снимается прямо
  //! здесь.
  //! @param window Минимальный промежуток между выпущенными строками.
  explicit LogThrottle(Clock::duration window) noexcept
    : _window(window)
  {
  }

  //! Спрашивает разрешение написать строку прямо сейчас.
  //!
  //! @param suppressed Получает число проглоченных событий с прошлой
  //!        ВЫПУЩЕННОЙ строки (0, если эта строка первая в окне).
  //! @param total Получает ПОЛНОЕ число событий с момента создания дросселя,
  //!        включая текущее. ★Существует затем, чтобы проглоченное событие
  //!        нельзя было потерять: следующая же выпущенная строка называет
  //!        полный счёт, и оракул регрессии считает по нему, а не по числу
  //!        строк в логе.
  //! ★ГАРАНТИЯ СОГЛАСОВАННОСТИ ПАРЫ (R72-fix2-5, находка Codex 5): когда
  //! функция вернула `true`, выполняется `total >= suppressed + 1` — числа
  //! в одной выпущенной строке описывают одно и то же состояние. Раньше
  //! `total` брался раньше `suppressed`, и под нагрузкой в строку попадало
  //! «проглочено 1, всего 1».
  //! @returns true, если вызывающему МОЖНО писать строку.
  //!
  //! ★`noexcept` и без аллокаций: зовётся с сетевого потока на пути пакета.
  //! ★ОПРЕДЕЛЕНИЕ — В `.cpp`, и не `inline`: это ступень лесенки раунда, а
  //! встроенная функция с единственным вызывающим не оставила бы внешнего
  //! символа, и «маркер не доехал» читалось бы на ВЕРНОЙ сборке.
  [[nodiscard]] bool Allow(uint64_t& suppressed, uint64_t& total) noexcept;

  //! ТО ЖЕ РЕШЕНИЕ, НО С ЯВНО ПЕРЕДАННЫМ «СЕЙЧАС» (R72-fix-1, находка Codex 6).
  //!
  //! ★ШОВ СУЩЕСТВУЕТ РАДИ ПРОВЕРЯЕМОСТИ ОТРИЦАТЕЛЬНОЙ ЭПОХИ. Стандарт не
  //! обещает, что `steady_clock::now().time_since_epoch()` неотрицательна —
  //! эпоха у `steady_clock` не специфицирована. На реализации с эпохой в
  //! будущем счёт тиков отрицателен, и прежняя метка «ещё ни разу» (ноль)
  //! означала бы «писать нельзя до наступления нуля»: первые отказы гасились
  //! бы вовсе без строки, то есть ровно тот тихий пропуск, ради которого
  //! дроссель и заведён. Проверить это через `Allow()` нельзя — часы вернут
  //! то, что вернут; поэтому решение вынесено в функцию от `now`, и тест
  //! подаёт ей отрицательное время.
  //! @param now Текущий момент в тиках `Clock`.
  //! @param suppressed См. `Allow`.
  //! @param total См. `Allow`.
  //! @returns true, если вызывающему МОЖНО писать строку.
  [[nodiscard]] bool AllowAt(
    Clock::rep now,
    uint64_t& suppressed,
    uint64_t& total) noexcept;

private:
  //! Окно дросселирования.
  Clock::duration _window;
  //! Момент, раньше которого писать нельзя, в тиках steady_clock.
  //!
  //! ★СТАРТОВОЕ ЗНАЧЕНИЕ — МИНИМУМ ТИПА, А НЕ НОЛЬ (R72-fix-1, находка Codex 6).
  //! Ноль как метка «ещё ни разу» — это молчаливое допущение, что эпоха
  //! `steady_clock` неотрицательна; стандарт этого не обещает. Минимум типа
  //! гарантирует `now >= _nextAllowed` на ЛЮБОЙ реализации, то есть первая
  //! строка выходит всегда — и при этом остаётся ОДНА атомарная переменная,
  //! на которой CAS-цикл ниже честно решает гонку. Отдельный флаг «первый
  //! раз» пришлось бы согласовывать со второй атомарной величиной — две
  //! независимые атомарные операции не образуют атомарного решения.
  std::atomic<Clock::rep> _nextAllowed{std::numeric_limits<Clock::rep>::min()};
  //! Проглочено с прошлой ВЫПУЩЕННОЙ строки (обнуляется при выпуске).
  std::atomic<uint64_t> _suppressed{0};
  //! Всего событий за жизнь дросселя. НИКОГДА не обнуляется.
  std::atomic<uint64_t> _total{0};
};

} // namespace server::util

#endif // LOGTHROTTLE_HPP
