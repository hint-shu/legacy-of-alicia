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
#include <mutex>
#include <string>
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

} // namespace

void BoundedListReport(
  const std::string_view name,
  const std::size_t requested,
  const std::size_t written,
  const std::size_t bound,
  const bool capacityHit,
  const std::source_location& where) noexcept
{
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
    static std::mutex mutex;
    static std::map<std::pair<std::string, std::uint_least32_t>, LogThrottle> sites;

    std::uint64_t suppressed = 0;
    std::uint64_t total = 0;

    {
      const std::scoped_lock lock(mutex);
      const auto [it, inserted] = sites.try_emplace(
        std::pair{std::string{where.file_name()}, where.line()},
        BoundedListReportWindow);

      if (not it->second.Allow(suppressed, total))
        return;
    }

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
