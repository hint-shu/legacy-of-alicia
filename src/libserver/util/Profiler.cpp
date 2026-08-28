/**
 * Alicia Server - dedicated server software
 * Copyright (C) 2026 Story Of Alicia
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

#include "libserver/util/Profiler.hpp"

namespace server
{

// LOA-fix (R51-6, round51, backlog #179): замеры берут мьютекс, а его захват
// умеет бросить на системной ошибке. Зовут их `Client::ReadLoop`/`WriteLoop` —
// путь КАЖДОГО пакета, поэтому под `noexcept` это была мина на каждом пакете.
//
// ★КОНТРАКТ ЗАЯВЛЯЕТСЯ ЧЕСТНО, а не красиво: профилировщик — диагностика с
// НАИЛУЧШИМИ УСИЛИЯМИ. Отказ захвата означает, что замер не состоялся; если не
// состоялось именно начало, следующая длительность может оказаться бессмысленной
// (она отсчитается от более раннего начала). Единственное, что гарантируется:
// профилировщик НИКОГДА не стоит соединения и тем более процесса.
//
// ★Флага «замер начат», который отбрасывал бы такие пары, здесь НАМЕРЕННО НЕТ, и
// это решение ревью (итерации 2-3): флаг делает результат зависимым от ПРАВИЛЬНОЙ
// парности начал и концов, а парность в записи не гарантирована — проверка
// `_isSending` сделана до взятия замка и допускает двух писателей сразу (дефект
// апстрима, заведён как #182). Флаг превращал бы чужой безобидный перекос в
// съеденный чужой замер, то есть менял бы поведение УСПЕШНОГО пути ради
// поведения на отказе. Чинить надо парность — отдельным раундом и своим стендом.
void Profiler::Start() noexcept
{
  try
  {
    std::scoped_lock lock(_mutex);

    _start = Clock::now();
  }
  catch (...)
  {
  }
}

void Profiler::Stop() noexcept
{
  try
  {
    std::scoped_lock lock(_mutex);

    _lastSample = Clock::now() - _start;
  }
  catch (...)
  {
  }
}

Profiler::ScopeGuard Profiler::Scope() noexcept
{
  return ScopeGuard(*this);
}

std::optional<Profiler::Duration> Profiler::Result() const noexcept
{
  try
  {
    std::scoped_lock lock(_mutex);
    return _lastSample;
  }
  catch (...)
  {
    // Замера нет — ровно то, что означает пустой optional.
    return std::nullopt;
  }
}



} // namespace server
