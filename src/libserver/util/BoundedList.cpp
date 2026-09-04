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

#include "libserver/util/BoundedList.hpp"

#include "libserver/util/LogThrottle.hpp"
#include "libserver/util/QuietLog.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <shared_mutex>
#include <utility>

namespace server::util
{

namespace
{

//! Окно подавления повтора НА ПЛОЩАДКУ.
//!
//! ★НЕ «ОДНАЖДЫ ЗА ПРОЦЕСС». Главный аргумент раунда — что путь живой и
//! самоинфлицируемый (список растёт, когда tid выпадает из реестра предметов, а
//! реестр мы правим каждой волной локализации). «Одна строка за процесс»
//! означала бы, что об усечении у ВТОРОГО игрока не узнать никогда.
constexpr auto BoundedListReportWindow = std::chrono::minutes{5};

//! Идёт ли на этом потоке СУХОЙ ПРОГОН (измерение кадра, чьи байты никто не
//! получит). См. `ScopedBoundedListSilence`.
thread_local bool g_silenced = false;

} // namespace

ScopedBoundedListSilence::ScopedBoundedListSilence() noexcept
  : _previous(g_silenced)
{
  g_silenced = true;
}

ScopedBoundedListSilence::~ScopedBoundedListSilence() noexcept
{
  g_silenced = _previous;
}

void BoundedListReport(
  const std::string_view name,
  const std::size_t requested,
  const std::size_t written,
  const std::size_t bound,
  const bool capacityHit,
  const std::source_location& where) noexcept
{
  // ★ВЫХОД ДО ВСЕГО: сухой прогон не оставляет ни строки, ни счётчика окна.
  // Проверка стоит ПЕРВОЙ строкой, а не внутри `try`, чтобы измерение не
  // трогало даже карту площадок.
  if (g_silenced)
    return;

  try
  {
    // ★ПОДАВЛЕНИЕ — НА ЧУЖОМ, УЖЕ ВЫКАЧЕННОМ ПРИМИТИВЕ (`util::LogThrottle`,
    // R72). Свой пересчёт времени здесь был бы вторым дросселем под другим
    // именем; карта нужна только затем, чтобы у КАЖДОЙ площадки был свой,
    // потому что `LogThrottle` — по экземпляру, а не по ключу.
    //
    // ★КАРТА НЕ РАСТЁТ ОТ КЛИЕНТА: ключ — файл+строка вызова, то есть множество
    // ограничено по построению числом площадок свипа. Это не повторение
    // безразмерного `_events` из #130-C6.
    //
    // ★R74-fix-2 (subreview #1, NIT 4): КЛЮЧ БЕЗ АЛЛОКАЦИИ И БЕЗ ЭКСКЛЮЗИВНОГО
    // ЗАМКА НА ПОДАВЛЁННОМ ПУТИ. Раньше ключ строился как `std::string` от
    // имени файла (кратчайший basename здесь 26 символов против SSO 15 — то
    // есть КУЧА) внутри критической секции, и замок брался ДО решения о
    // подавлении. При окне 5 минут подавлено практически 100% вызовов, то есть
    // платили полной ценой ровно там, где ничего не делаем.
    //
    // Ключ теперь — УКАЗАТЕЛЬ на литерал имени файла плюс строка. Для одной
    // точки вызова `source_location::current()` даёт один и тот же статический
    // объект, поэтому указатель стабилен; если линкер склеит одинаковые
    // литералы разных TU, совпадение указателя И строки означает буквально ту
    // же площадку — ключ верен в обе стороны. Узлы `std::map` стабильны, так
    // что найденный `LogThrottle` переживает освобождение замка.
    using SiteKey = std::pair<const char*, std::uint_least32_t>;
    static std::shared_mutex mutex;
    static std::map<SiteKey, LogThrottle> sites;

    const SiteKey key{where.file_name(), where.line()};
    LogThrottle* throttle = nullptr;
    {
      const std::shared_lock lock(mutex);
      const auto it = sites.find(key);
      if (it != sites.end())
        throttle = &it->second;
    }
    if (throttle == nullptr)
    {
      const std::unique_lock lock(mutex);
      throttle = &sites.try_emplace(key, BoundedListReportWindow).first->second;
    }

    std::uint64_t suppressed = 0;
    std::uint64_t total = 0;
    if (not throttle->Allow(suppressed, total))
      return;

    // ★СТРОКА НАЗЫВАЕТ ПОЛНЫЙ СЧЁТ, а не только себя: «одна строка» иначе
    // читалась бы как «случилось один раз».
    QuietLogWarn(
      "bounded list truncated: {} — {} entries requested, {} written (bound {}, {});"
      " suppressed {} more at this site, {} in total, next line in {} minutes at the earliest",
      name.empty() ? std::string_view{"<unnamed>"} : name,
      requested,
      written,
      bound,
      capacityHit ? "packet capacity" : "count bound",
      suppressed,
      total,
      BoundedListReportWindow.count());
  }
  catch (...)
  {
    // Не пожаловаться безопаснее, чем бросить с потока записи: бросок отсюда
    // означал бы `End()` клиента — ровно то, что раунд чинит.
  }
}

} // namespace server::util
