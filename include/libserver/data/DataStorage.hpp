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

#ifndef DATASTORAGE_HPP
#define DATASTORAGE_HPP

#include "libserver/data/Record.hpp"
#include "libserver/util/QuietLog.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <ranges>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace server
{

template <typename Key, typename Data>
class DataStorage
{
public:
  using KeySpan = std::span<const Key>;

  using DataSourceRetrieveListener = std::function<bool(const Key& key, Data& data)>;
  using DataSourceStoreListener = std::function<bool(const Key& key, Data& data)>;
  using DataSourceDeleteListener = std::function<bool(const Key& key)>;

  using DataSupplier = std::function<std::pair<Key, Data>()>;

  DataStorage(
    const DataSourceRetrieveListener& retrieveListener,
    const DataSourceStoreListener& storeListener,
    const DataSourceDeleteListener& deleteListener)
    : _dataSourceRetrieveListener(retrieveListener)
    , _dataSourceStoreListener(storeListener)
    , _dataSourceDeleteListener(deleteListener)
  {
  }

  void Initialize()
  {
  }

  void Terminate()
  {
    {
      std::scoped_lock lock(_entriesMutex);
      for (auto& entry : _entries)
      {
        if (not entry.second.available)
          continue;
        RequestStore(entry.first);
      }
    }

    // Process the queued operations.
    ProcessRetrieveQueue();

    // ★ОСТАНОВКА ОБЯЗАНА ВЫБРАТЬ ВЕСЬ БЮДЖЕТ ПОВТОРОВ, А НЕ ОДНУ ПОПЫТКУ
    // (LOA-fix, R58-15, round58, backlog #175). Ограниченный повтор
    // рассчитывает на следующий тик, но при завершении тиков больше НЕ БУДЕТ:
    // попытки со второй по последнюю не случились бы никогда, и строка «сдаюсь»
    // тоже не появилась бы. А ведь именно остановка — момент максимального
    // риска: она ставит в очередь ВСЕ записи разом. Найдено ревью (итерация 1).
    for (uint32_t attempt = 0; attempt < MaxStoreAttempts; ++attempt)
    {
      ProcessStoreQueue();

      bool pending = false;
      {
        std::scoped_lock queueLock(_storeQueue.mutex);
        pending = not _storeQueue.data.empty();
      }
      if (not pending)
        break;
    }

    ProcessDeleteQueue();

    {
      std::scoped_lock lock(_entriesMutex);
      _entries.clear();
    }
  }

  //! Whether data record is available.
  //! @param key Key of the datum.
  //! @returns `true` if datum is available, `false` otherwise.
  bool IsAvailable(const Key& key)
  {
    const auto iterator = _entries.find(key);
    if (iterator == _entries.cend())
      return false;
    return iterator->second.available;
  }

  //! Returns how many times in a row the retrieval of a datum from the data source failed.
  //! A datum is never retrieved again on its own once a retrieval failed.
  //! @param key Key of the datum.
  //! @returns The count of consecutive failed retrievals.
  uint32_t GetRetrieveFailureCount(const Key& key)
  {
    std::scoped_lock lock(_entriesMutex);
    const auto iterator = _entries.find(key);
    if (iterator == _entries.cend())
      return 0;
    return iterator->second.retrieveFailureCount.load(std::memory_order::relaxed);
  }
  //! @param key Key of the datum.
  //! @param duration Duration the retrievals have to have been failing for.
  //! @returns `true` if the retrievals have been failing for at least the duration.
  bool HasRetrieveFailedFor(
    const Key& key,
    const std::chrono::steady_clock::duration duration)
  {
    std::scoped_lock lock(_entriesMutex);
    const auto iterator = _entries.find(key);
    if (iterator == _entries.cend())
      return false;

    const auto& entry = iterator->second;
    if (entry.retrieveFailureCount.load(std::memory_order::relaxed) == 0)
      return false;

    const auto firstFailure = entry.firstRetrieveFailure.load(std::memory_order::relaxed);
    return std::chrono::steady_clock::now() - firstFailure >= duration;
  }

  //! Queues another retrieval attempt of a datum which previously failed to retrieve.
  //! @param key Key of the datum.
  void RetryRetrieve(const Key& key)
  {
    RequestRetrieve(key);
  }

  //! Whether data records are available.
  //! @param keys Keys of the data.
  //! @returns `true` if data are available, `false` otherwise.
  bool IsAvailable(KeySpan keys)
  {
    for (const auto& key : keys)
    {
      if (not IsAvailable(key))
        return false;
    }

    return true;
  }

  Record<Data> Create(DataSupplier supplier)
  {
    auto [key, data] = supplier();

    std::unique_lock lock(_entriesMutex);
    auto [it, created] = _entries.try_emplace(key);
    lock.unlock();

    if (not created)
      throw std::runtime_error(std::format("Entry with key {} already exists", key));

    auto& entry = it->second;
    entry.value = std::move(data);
    entry.available = true;

    RequestStore(key);

    return Record(&entry.value, &entry.mutex, [this, key]()
      {
        RequestStore(key);
      });
  }

  Record<Data> GetOrCreate(DataSupplier supplier)
  {
    auto [key, data] = supplier();

    std::unique_lock lock(_entriesMutex);
    auto [it, created] = _entries.try_emplace(key);
    lock.unlock();

    if (not created)
      return Record(&it->second.value, &it->second.mutex, [this, key]()
      {
        RequestStore(key);
      });

    auto& entry = it->second;
    entry.value = std::move(data);
    entry.available = true;

    RequestStore(key);

    return Record(&entry.value, &entry.mutex, [this, key]()
      {
        RequestStore(key);
      });
  }

  std::optional<Record<Data>> Get(const Key& key, bool retrieve = true)
  {
    std::unique_lock lock(_entriesMutex);
    auto [recordIter, created] = _entries.try_emplace(key);
    lock.unlock();

    auto& record = recordIter->second;

    if (created && retrieve)
    {
      RequestRetrieve(key);
      return std::nullopt;
    }

    if (record.available)
      return Record(&record.value, &record.mutex, [this, key]()
      {
        RequestStore(key);
      });
    return std::nullopt;
  }

  std::optional<std::vector<Record<Data>>> Get(const KeySpan keys)
  {
    bool isComplete = true;

    std::vector<Record<Data>> records;
    for (const auto& key : keys)
    {
      auto record = Get(key);
      if (not record)
      {
        isComplete = false;
        continue;
      }

      records.emplace_back() = std::move(*record);
    }

    if (isComplete)
      return records;
    return std::nullopt;
  }

  void Delete(const Key& key)
  {
    RequestDelete(key);
  }

  std::vector<Key> GetKeys()
  {
    std::vector<Key> keys;
    for (const auto& key : std::ranges::views::keys(_entries))
    {
      keys.emplace_back(key);
    }
    return keys;
  }

  void Save(const Key& key)
  {
    RequestStore(key);
  }

  void Tick()
  {
    ProcessRetrieveQueue();
    ProcessStoreQueue();
    ProcessDeleteQueue();
  }

private:
  void RequestRetrieve(const Key& key)
  {
    std::scoped_lock lock(_retrieveQueue.mutex);
    _retrieveQueue.data.insert(key);
    _retrieveQueue.dataFlag.store(true, std::memory_order::relaxed);
  }

  void RequestStore(const Key& key)
  {
    std::scoped_lock lock(_storeQueue.mutex);
    _storeQueue.data.insert(key);
    _storeQueue.dataFlag.store(true, std::memory_order::relaxed);
  }

  void RequestDelete(const Key& key)
  {
    std::scoped_lock lock(_deleteQueue.mutex);
    _deleteQueue.data.insert(key);
    _deleteQueue.dataFlag.store(true, std::memory_order::relaxed);
  }

  void ProcessRetrieveQueue()
  {
    if (not _retrieveQueue.dataFlag.exchange(false, std::memory_order::relaxed))
      return;

    // LOA-fix (R65-3, backlog #186): флаг опущен, набор ключей ещё цел —
    // страж вернёт флаг, если обход не дойдёт до конца. Повторное чтение уже
    // прочитанного ключа безвредно (источник перечитает файл), а вот молча
    // уснувшая очередь — нет.
    FlagGuard flagGuard(_retrieveQueue);

    std::scoped_lock queueLock(_retrieveQueue.mutex);
    for (const auto& key : _retrieveQueue.data)
    {
      std::unique_lock lock(_entriesMutex);
      auto& entry = _entries[key];
      lock.unlock();

      if (_dataSourceRetrieveListener(key, entry.value))
      {
        // LOA-fix (R47-S2, находка ревью R47): публикуем флаг ГОТОВНОСТИ с
        // release-семантикой. Раньше стояло `relaxed`, и читатель, увидевший
        // `available == true`, не получал никакой гарантии, что видит и САМИ
        // данные, которые строчкой выше положил поток чтения: связи
        // «произошло-до» между записью значения и публикацией флага не было.
        // Release здесь + обычные (последовательно-согласованные) чтения флага
        // у читателей дают эту связь: увидел флаг — видишь и данные целиком.
        entry.available.store(true, std::memory_order::release);
        entry.retrieveFailureCount.store(0, std::memory_order::relaxed);
      }
      else
      {
        if (entry.retrieveFailureCount.fetch_add(1, std::memory_order::relaxed) == 0)
        {
          entry.firstRetrieveFailure.store(
            std::chrono::steady_clock::now(), std::memory_order::relaxed);
        }
      }
    }
    _retrieveQueue.data.clear();
    flagGuard.Dismiss();
  }

  void ProcessStoreQueue()
  {
    if (not _storeQueue.dataFlag.exchange(false, std::memory_order::relaxed))
      return;

    // LOA-fix (R65-3, backlog #186; ИСПРАВЛЕНО ПО РЕВЬЮ, итерация 2): между
    // опусканием флага и установкой стража хвоста было ОКНО. `keysToStore.assign`
    // ниже выделяет память и может бросить — тогда набор ключей остаётся целым,
    // но флаг уже опущен, и очередь сохранения засыпает НАВСЕГДА. То есть ровно
    // тот дефект, ради которого раунд и писался, только в новом месте.
    // ★Этот страж прикрывает именно окно; как только хвост забран и за него
    // отвечает StoreTailGuard, страж снимается.
    FlagGuard earlyGuard(_storeQueue);

    // LOA-fix (R47-S1, находка ревью R47): забираем очередь целиком и
    // ОТПУСКАЕМ её замок до того, как возьмём замок записи. Порядок важен:
    // `Record::Mutable` берёт их в обратном порядке (сначала запись, потом
    // очередь — когда просит сохранение), и удержание обоих разом даёт
    // перекрёстный дедлок.
    std::vector<Key> keysToStore;
    {
      std::scoped_lock queueLock(_storeQueue.mutex);
      keysToStore.assign(_storeQueue.data.begin(), _storeQueue.data.end());
      _storeQueue.data.clear();
    }

    // LOA-fix (R65-3, backlog #186): обход по ИНДЕКСУ, а не по диапазону, и
    // страж хвоста. Индекс — общее состояние цикла и стража: бросок на шаге N
    // оставляет его равным N, и в очередь возвращаются ключи [N, конец) —
    // включая тот, на котором бросило: он честно заслуживает повтора.
    size_t index = 0;
    StoreTailGuard tailGuard(_storeQueue, keysToStore, index);
    // Хвост теперь под ответственностью стража хвоста — ранний снимаем.
    earlyGuard.Dismiss();

    for (; index < keysToStore.size(); ++index)
    {
      const auto& key = keysToStore[index];

      // ★ПОПЫТКА И ЕЁ УЧЁТ РАЗВЕДЕНЫ (исправлено по ревью, итерация 3).
      // Раньше счётчик отказов трогали в двух местах — в теле и в поясе, — и
      // бросок ПОСЛЕ уже засчитанного отказа списывал попытку ДВАЖДЫ, а ветка
      // «сдаёмся после броска» вдобавок не обнуляла счётчик, из-за чего
      // следующая просьба сохранить ту же запись сдавалась бы с первого раза.
      // Теперь тело только УЗНАЁТ исход, а вся бухгалтерия — ниже, ровно в
      // одном месте и ровно один раз за попытку.
      enum class Outcome
      {
        Stored,
        Skipped,
        Failed
      };
      auto outcome = Outcome::Failed;

      try
      {
        std::unique_lock lock(_entriesMutex);
        auto& entry = _entries[key];
        lock.unlock();

        if (not entry.available)
        {
          outcome = Outcome::Skipped;
        }
        else
        {
          // ★СЕРИАЛИЗАЦИЯ ЧИТАЕТ ЗАПИСЬ ПОД ЕЁ ЗАМКОМ. Раньше она читала поля,
          // пока другой поток менял их под `Record::Mutable` (тот держит этот же
          // мьютекс исключительно) — то есть параллельно с изменением. Для
          // векторов (инвентарь, подарки, достижения) это неопределённое
          // поведение, а для пары «прогресс + отметка тира» — ещё и риск записать
          // несогласованную пару. Замок РАЗДЕЛЯЕМЫЙ: сохранение только читает и
          // задерживает лишь пишущих в эту одну запись.
          std::shared_lock valueLock(entry.mutex);

          // ★РЕЗУЛЬТАТ СОХРАНЕНИЯ ЧИТАЕТСЯ (LOA-fix, R58-12, round58, backlog #175).
          // Раньше он ОТБРАСЫВАЛСЯ — при том что соседи по этому же файлу свой
          // результат читают. Ключ уже снят с очереди выше, повтора нет,
          // объявленное поле `dirty` не используется нигде — то есть
          // провалившееся сохранение исчезало бесследно, и запись игрока молча
          // оставалась старой навсегда
          // ([[a-check-nobody-reads-is-not-a-check]]).
          //
          // ★ПОЛИТИКА НЕ ИЗОБРЕТАЕТСЯ, А ЗЕРКАЛИТСЯ С ПУТИ ЧТЕНИЯ: там уже есть
          // счётчик подряд идущих отказов. Безграничный повтор был бы голодовкой
          // (урок R54/#187), поэтому бюджет конечен, а по его исчерпании ключ
          // снимается с ОДНОЙ однозначной строкой.
          const bool stored = _dataSourceStoreListener(key, entry.value);
          valueLock.unlock();

          outcome = stored ? Outcome::Stored : Outcome::Failed;
        }
      }
      catch (...)
      {
        // ★БРОСОК — ЭТО ТОЖЕ НЕУДАВШАЯСЯ ПОПЫТКА, а не особый случай. Без этого
        // он уходил наружу, не тратил бюджет, и ключ возвращался в очередь вечно;
        // хуже того, каждый раз ронял `DataDirector::Tick` ДО того, как дотикают
        // остальные пятнадцать хранилищ — они висят в одном `try` последовательно.
        outcome = Outcome::Failed;
      }

      if (outcome == Outcome::Skipped)
        continue;

      bool requeue = false;
      bool giveUp = false;
      try
      {
        std::scoped_lock entriesLock(_entriesMutex);
        const auto iterator = _entries.find(key);
        if (iterator == _entries.end())
          continue;

        auto& failureCount = iterator->second.storeFailureCount;
        if (outcome == Outcome::Stored)
        {
          failureCount.store(0, std::memory_order::relaxed);
          continue;
        }

        const auto failures = failureCount.fetch_add(
          1, std::memory_order::relaxed) + 1;
        if (failures < MaxStoreAttempts)
        {
          requeue = true;
        }
        else
        {
          // ★СБРОС НА КАЖДОМ ПУТИ СДАЧИ, а не только на одном: иначе счётчик
          // остаётся исчерпанным, и следующая просьба сохранить эту запись
          // сдастся с первой же неудачи.
          failureCount.store(0, std::memory_order::relaxed);
          giveUp = true;
        }
      }
      catch (...)
      {
        // Учесть не вышло — не повод потерять ключ; вернём его в очередь.
        requeue = true;
      }

      // ★ОЧЕРЕДЬ БЕРЁТСЯ ПОСЛЕ ТОГО, КАК ОТПУЩЕН ЗАМОК ЗАПИСЕЙ. Порядок «записи,
      // затем очередь» здесь не удерживается разом — см. R47-S1 выше.
      if (requeue)
      {
        try
        {
          {
            std::scoped_lock queueLock(_storeQueue.mutex);
            _storeQueue.data.emplace(key);
          }
          _storeQueue.dataFlag.store(true, std::memory_order::relaxed);
        }
        catch (...)
        {
          server::util::QuietLogError(
            "A datum could not be put back into the store queue; its pending "
            "changes stay in memory only");
        }
      }
      else if (giveUp)
      {
        server::util::QuietLogError(
          "Giving up on saving a datum after {} attempts. The record stays in memory "
          "and its file on disk is the previous version", MaxStoreAttempts);
      }
    }
  }

  void ProcessDeleteQueue()
  {
    if (not _deleteQueue.dataFlag.exchange(false, std::memory_order::relaxed))
      return;

    // LOA-fix (R65-3, backlog #186): тот же страж, что у чтения — радиус здесь
    // такой же мягкий (набор цел, спит только флаг).
    FlagGuard flagGuard(_deleteQueue);

    std::scoped_lock queueLock(_deleteQueue.mutex);
    for (const auto& key : _deleteQueue.data)
    {
      std::unique_lock lock(_entriesMutex);
      auto& entry = _entries[key];
      lock.unlock();

      if (entry.available)
        if (_dataSourceDeleteListener(key))
          entry.available.store(false, std::memory_order::relaxed);
    }
    _deleteQueue.data.clear();
    flagGuard.Dismiss();
  }

  //! Сколько раз подряд повторять сохранение, прежде чем сдаться (R58, #175).
  //! ★Число конечно НАМЕРЕННО: безграничный повтор падающей записи — это
  //! голодовка очереди, урок R54/#187. Пять попыток при тике в 20 мс дают около
  //! десятой доли секунды на пережидание короткого сбоя, а постоянный отказ
  //! (кончилось место, права) громко называется и не блокирует остальных.
  static constexpr uint32_t MaxStoreAttempts = 5;

  struct Entry
  {
    std::atomic_bool available{false};
    std::atomic_bool dirty{false};
    //! Сколько раз подряд не удалось СОХРАНИТЬ запись (R58, #175). Зеркало
    //! `retrieveFailureCount` пути чтения — политика та же, чтобы не заводить
    //! в одном классе две разные.
    std::atomic_uint32_t storeFailureCount{0};
    //! A count of consecutive failed retrievals of the datum from the data source.
    std::atomic_uint32_t retrieveFailureCount{0};
    //! A time point of the first of the consecutive failed retrievals.
    std::atomic<std::chrono::steady_clock::time_point> firstRetrieveFailure{};
    Record<Data>::PatchListener listener;
    std::shared_mutex mutex{};
    Data value;
  };

  struct Queue
  {
    std::mutex mutex;
    std::atomic_bool dataFlag;
    std::unordered_set<Key> data;
  };

  //! Возвращает флаг наличия данных, если обход очереди НЕ дошёл до конца
  //! (LOA-fix, R65-3, backlog #186).
  //!
  //! ★ЗАЧЕМ. Все три обработчика опускают флаг ДО обхода. Бросок в середине
  //! оставляет набор ключей целым, но флаг уже опущен — и хвост лежит МЁРТВЫМ
  //! до следующего внешнего запроса к этой же очереди. Снаружи это выглядит
  //! как «сервер перестал читать/удалять часть записей» без единой жалобы.
  //!
  //! ★УСТАНОВКА ОБЯЗАТЕЛЬСТВА НЕ ДОЛЖНА БРОСАТЬ, иначе страж не встанет ровно
  //! тогда, когда он нужен. Здесь конструктор только запоминает ссылку, а
  //! деструктор делает единственную атомарную запись — оба честно `noexcept`.
  class FlagGuard
  {
  public:
    explicit FlagGuard(Queue& queue) noexcept
      : _queue(queue) {}

    FlagGuard(const FlagGuard&) = delete;
    FlagGuard& operator=(const FlagGuard&) = delete;

    ~FlagGuard()
    {
      if (not _dismissed)
        _queue.dataFlag.store(true, std::memory_order::relaxed);
    }

    //! Обход дошёл до конца — возвращать флаг не нужно.
    void Dismiss() noexcept { _dismissed = true; }

  private:
    Queue& _queue;
    bool _dismissed{false};
  };

  //! Возвращает в очередь сохранения необработанный ХВОСТ ключей
  //! (LOA-fix, R65-3, backlog #186).
  //!
  //! ★ПОЧЕМУ У СОХРАНЕНИЯ ОТДЕЛЬНЫЙ СТРАЖ, А НЕ ТОТ ЖЕ. Радиус здесь ХУЖЕ:
  //! очередь сохранения забирает ключи себе и очищает набор ДО обхода, поэтому
  //! бросок в середине теряет хвост НАСОВСЕМ — ключей нет ни в очереди, ни у
  //! кого. Одного флага мало, надо вернуть сами ключи.
  //!
  //! ★Конструктор `noexcept` и без работы; вся возможная аллокация — в
  //! деструкторе и обёрнута, потому что деструктор не имеет права бросить.
  class StoreTailGuard
  {
  public:
    StoreTailGuard(Queue& queue, const std::vector<Key>& keys, const size_t& index) noexcept
      : _queue(queue), _keys(keys), _index(index) {}

    StoreTailGuard(const StoreTailGuard&) = delete;
    StoreTailGuard& operator=(const StoreTailGuard&) = delete;

    ~StoreTailGuard()
    {
      if (_index >= _keys.size())
        return;

      const size_t expected = _keys.size() - _index;
      size_t returned = 0;
      try
      {
        std::scoped_lock queueLock(_queue.mutex);
        // ★МЕСТО ПОД КОРЗИНЫ ПРОСИТСЯ ЗАРАНЕЕ — но это НЕ гарантия «всё или
        // ничего», и прежняя редакция этого комментария врала (поймано ревью).
        // `reserve` у `unordered_set` резервирует КОРЗИНЫ, а не узлы: каждая
        // `emplace` всё равно выделяет свой узел и всё равно может бросить.
        // Польза от резерва реальная, но узкая — перехеширование выносится ВПЕРЁД
        // всех вставок, и отказ на нём не оставляет половину возвращённой.
        _queue.data.reserve(_queue.data.size() + expected);
        for (size_t idx = _index; idx < _keys.size(); ++idx)
        {
          _queue.data.emplace(_keys[idx]);
          ++returned;
        }
      }
      catch (...)
      {
      }

      _queue.dataFlag.store(true, std::memory_order::relaxed);

      // ★ЧЕСТНАЯ ГРАНИЦА, А НЕ ОБЕЩАНИЕ. Гарантии «хвост будет обработан
      // целиком» здесь НЕТ и быть не может: возврат каждого ключа выделяет узел.
      // Что есть: возврат по мере возможности, и ГРОМКИЙ отчёт о том, сколько
      // вернуть не удалось. ★Отчёт тоже не всесилен — при полном исчерпании
      // памяти не выйдет и его; это предел, а не недосмотр. Молчаливая же потеря
      // выглядела бы как штатная работа ([[a-check-nobody-reads-is-not-a-check]]).
      if (returned != expected)
      {
        server::util::QuietLogError(
          "{} of {} pending record(s) could not be put back into the store queue; "
          "their changes stay in memory only",
          expected - returned,
          expected);
      }
    }

  private:
    Queue& _queue;
    const std::vector<Key>& _keys;
    const size_t& _index;
  };

  Queue _retrieveQueue;
  Queue _storeQueue;
  Queue _deleteQueue;

  std::mutex _entriesMutex;
  std::unordered_map<Key, Entry> _entries{};

  DataSourceRetrieveListener _dataSourceRetrieveListener;
  DataSourceStoreListener _dataSourceStoreListener;
  DataSourceDeleteListener _dataSourceDeleteListener;
};

} // namespace server

#endif // DATASTORAGE_HPP
