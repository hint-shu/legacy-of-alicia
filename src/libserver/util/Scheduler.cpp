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

#include "libserver/util/Scheduler.hpp"

namespace server
{

Scheduler::Scheduler()
{
  _jobIterator = _jobs.cend();
}

void Scheduler::Tick()
{
  // LOA-fix (R36-1, round36, backlog #121): ГОНКА ДАННЫХ НА СПИСКЕ ЗАДАЧ.
  // Tick обходил, читал и СТИРАЛ узлы _jobs вообще без _jobsMutex, тогда как
  // Queue под этим замком делает emplace_back с СЕТЕВЫХ потоков. Порванная
  // линковка узла std::list — это не «потерянная задача», а прыжок по мусорному
  // указателю. TSan на стенде: 31 отчёт за прогон A и 38 за прогон B — самая
  // громкая гонка кластера.
  //
  // ★СЛИВ ЧЕРЕЗ SPLICE. Под замком ТОЛЬКО находим созревшую задачу и
  // ПЕРЕНОСИМ ЕЁ УЗЕЛ (std::list::splice — O(1) перелинковка указателей) в
  // локальный список; замок отпускаем; задачу исполняем УЖЕ СНАРУЖИ; Task
  // разрушается вместе с локальным списком на выходе из функции — тоже вне
  // замка. Под _jobsMutex не исполняется НИ СТРОЧКИ пользовательского кода:
  // ни task(), ни копирующий конструктор std::function, ни её деструктор.
  // Иначе нельзя:
  //   1) задачи штатно перезаводят себя изнутри (Telemetry::ScheduleCollectData,
  //      MatchmakingSystem::Search, RanchDirector::ScheduleFoalMaturityCheck) —
  //      это вызов Scheduler::Queue, то есть тот же нерекурсивный мьютекс:
  //      мгновенный самодедлок;
  //   2) task() — чужой код, он берёт замки директоров/комнат/данных; держать
  //      их под замком планировщика значит завести порядок локов, из которого
  //      растут дедлоки;
  //   3) копия и разрушение Task — ТОЖЕ чужой код (конструктор/деструктор
  //      захваченного состояния): захват, который в copy-ctor или в dtor
  //      дёрнет Queue, повесил бы тик ровно так же. Поэтому splice, а не
  //      `dueTask = job.task` + erase.
  // _jobsMutex остаётся ЛИСТОВЫМ: под ним живут только обход/сравнение
  // std::list, splice и Clock::now().
  //
  // Локальный держатель созревшего узла. Пуст — значит ничего не созрело:
  // ★проверяем ПУСТОТУ СПИСКА, а не bool(task), чтобы пустая задача (Task{})
  // по-прежнему давала std::bad_function_call, а не проглатывалась молча.
  decltype(_jobs) dueJob;
  // Позиция узла запоминается ПРЕДЫДУЩИМ соседом (или флагом «был первым»):
  // после task() из него вычисляется ровно тот итератор, который вернул бы
  // erase оригинала — см. пункт (в) в комментарии патча.
  decltype(_jobs)::const_iterator previousJob{};
  bool hasPreviousJob = false;

  {
    std::scoped_lock lock(_jobsMutex);

    // Make sure there is jobs to execute.
    if (_jobs.empty())
      return;

    // If the job iterator is at the end cycle it back to the front,
    // since the job list is not empty.
    if (_jobIterator == _jobs.cend())
    {
      _jobIterator = _jobs.begin();
    }

    // Iterate over the jobs until you find one that can be executed,
    // detach its node from the job list and execute it outside of the lock.
    while (true)
    {
      const auto& job = *_jobIterator;
      if (Clock::now() >= job.when)
      {
        // LOA-fix (R36-1): запоминаем соседа СЛЕВА, чтобы после исполнения
        // задачи восстановить позицию обхода один-в-один с оригиналом
        // (в оригинале erase шёл ПОСЛЕ task(), поэтому перезаведённая из
        // хвоста задача становилась следующей).
        hasPreviousJob = _jobIterator != _jobs.cbegin();
        if (hasPreviousJob)
          previousJob = std::prev(_jobIterator);

        // ★SPLICE, А НЕ КОПИЯ: узел целиком уезжает в локальный список.
        // Ни Job, ни Task при этом не копируются и не разрушаются.
        dueJob.splice(dueJob.cbegin(), _jobs, _jobIterator);

        // Пока замок снят, поле обязано указывать на действительный узел
        // ЭТОГО списка; cend у std::list стабилен и от emplace_back в Queue
        // не портится. Настоящая позиция ставится после исполнения задачи.
        _jobIterator = _jobs.cend();

        break;
      }

      if (++_jobIterator == _jobs.cend())
        break;
    }
  }

  // LOA-fix (R36-1): ★ИСПОЛНЕНИЕ ВНЕ ЗАМКА. За один тик по-прежнему выполняется
  // не более одной задачи.
  if (dueJob.empty())
    return;

  try
  {
    dueJob.front().task();
  }
  catch (const std::exception& x)
  {
    // Оригинал на std::exception СТИРАЛ задачу и бросал обёрнутую ошибку.
    // Узел уже вне списка — осталось довести _jobIterator до позиции,
    // которую вернул бы erase. Сам Task разрушится при раскрутке стека,
    // когда dueJob выйдет из области видимости, — замок к тому моменту снят.
    {
      std::scoped_lock lock(_jobsMutex);
      _jobIterator = hasPreviousJob ? std::next(previousJob) : _jobs.cbegin();
    }

    throw std::runtime_error(std::format("{}", x.what()));
  }
  catch (...)
  {
    // ★PRESERVE-LEGACY. Оригинальный catch ловил ТОЛЬКО std::exception,
    // поэтому не-std бросок уходил мимо обоих erase: задача ОСТАВАЛАСЬ в
    // очереди, а _jobIterator продолжал на неё указывать. Возвращаем узел
    // splice'ом ровно на прежнее место и восстанавливаем итератор.
    // (Вечного re-throw это не создаёт: выше по стеку тоже ловят только
    // std::exception, не-std бросок доходит до std::terminate.)
    std::scoped_lock lock(_jobsMutex);

    const auto restoredJob = dueJob.cbegin();
    _jobs.splice(
      hasPreviousJob ? std::next(previousJob) : _jobs.cbegin(),
      dueJob,
      restoredJob);
    _jobIterator = restoredJob;

    throw;
  }

  // Успех: оригинал стирал узел ПОСЛЕ task() и продолжал обход с того, что
  // вернул erase. Узел уже вырезан, ставим ту же позицию.
  {
    std::scoped_lock lock(_jobsMutex);
    _jobIterator = hasPreviousJob ? std::next(previousJob) : _jobs.cbegin();
  }

  // dueJob разрушает Task здесь, на выходе из функции — ВНЕ замка.
}

void Scheduler::Queue(
  const Task& task,
  const Clock::time_point when)
{
  std::scoped_lock lock(_jobsMutex);
  _jobs.emplace_back(Job{
    .when = when,
    .task = task});
}


} // namespace server
