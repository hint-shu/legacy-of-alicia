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

#ifndef ATOMIC_FILE_HPP
#define ATOMIC_FILE_HPP

#ifndef WIN32
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace server::util
{

//! ЗАПИСЬ, КОТОРАЯ ЛИБО СОСТОЯЛАСЬ ЦЕЛИКОМ, ЛИБО НЕ ТРОНУЛА ФАЙЛ
//! (LOA-fix, round58, backlog #175).
//!
//! ★ГРАНИЦА ГАРАНТИИ, СКАЗАННАЯ ТОЧНО. Под POSIX (наш прод — Linux, ext4)
//! подмена атомарна: `rename` поверх существующего имени либо произошла, либо
//! нет, и читатель всегда видит либо старый файл целиком, либо новый целиком.
//! Под Windows этого обещать НЕЛЬЗЯ: реализация `std::filesystem::rename` в MSVC
//! сводится к `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`, у которого есть открытая
//! ошибка — одновременный читатель может получить `ERROR_FILE_NOT_FOUND` в
//! момент подмены, то есть увидеть ОТСУТСТВИЕ файла. Для нашего кода это ровно
//! то поведение, которое раунд и устраняет.
//!
//! Правильное лечение под Windows — `ReplaceFileW` с проверкой непрерывной
//! видимости под одновременными читателями. Я его НЕ пишу здесь и говорю почему:
//! у меня нет Windows, чтобы эту проверку провести, а писать путь, который я не
//! могу ни собрать, ни выполнить, значит снова заявить непроверенное свойство —
//! именно то, от чего этот комментарий и предостерегает. Заведено отдельной
//! задачей (#204); прод от неё не зависит.
//!
//! Апстримная форма записи выглядела так:
//!
//!     std::ofstream file(path);      // ← ЖИВОЙ файл обрезан до нуля ПРЯМО ЗДЕСЬ
//!     if (not file.is_open()) throw;
//!     nlohmann::json json;           // ← десятки бросающих операций
//!     json["..."] = record.field();  //   (у StoreCharacter — 165 строк)
//!     file << json.dump(2);          // ← состояние потока НЕ проверяется
//!
//! То есть файл игрока обрезался ДО того, как данные вообще сформированы. Любой
//! бросок в середине сборки оставлял на диске ПУСТОЙ файл вместо целой записи, а
//! наши же пояса при этом сервер сохраняли — то есть игрок продолжал играть, не
//! зная, что его лошадь уже стёрта.
//!
//! ★И дальше становилось хуже: `repair::CleanseCharacterReferences` через две
//! секунды НЕОБРАТИМО вычёркивает ссылки на нечитаемую запись из персонажа и
//! сохраняет персонажа обратно. После этого восстановление файла из бэкапа уже
//! не помогает — ссылок на него не осталось.
//!
//! ★ЧЕГО ЭТА ФУНКЦИЯ НЕ ДЕЛАЕТ, И ЭТО ОСОЗНАННО: она не зовёт `fsync`. Замер на
//! нашей машине (ext4/NVMe, 2000 итераций): обрезание на месте 8 мкс, временный
//! файл с переименованием 16,5 мкс, с `fsync` файла — 9 848 мкс (1230×), с
//! `fsync` каталога — 20 233 мкс (2529×). От гибели ПРОЦЕССА (kill, бросок,
//! `std::terminate`, OOM, SIGKILL при деплое) `fsync` не нужен вовсе: отданные в
//! `write` данные лежат в страничном кэше ЯДРА и смерть процесса их не трогает —
//! а это ровно наш сценарий. От гибели ХОСТА он нужен, но ext4 с `data=ordered`
//! и `auto_da_alloc` при переименовании ПОВЕРХ существующего файла выталкивает
//! данные перед фиксацией, поэтому худший исход — СТАРАЯ ЦЕЛАЯ версия, то есть
//! ровно тот инвариант, ради которого всё и делается. При бюджете тика 20 мс и
//! залповом сохранении сотен файлов на остановке `fsync` стоил бы дороже, чем
//! защищает.
//!
//! @param path Целевой файл.
//! @param payload ГОТОВОЕ содержимое. ★Именно готовое: вся сборка и все её
//!        броски обязаны случиться ДО вызова, иначе окно порчи просто переедет
//!        на временный файл.
//! @param what Название сущности для сообщения об ошибке («Character file»).
inline void WriteFileAtomically(
  const std::filesystem::path& path,
  const std::string_view payload,
  const std::string_view what)
{
  // ★ВРЕМЕННЫЙ ФАЙЛ — В ТОМ ЖЕ КАТАЛОГЕ, и это условие корректности, а не стиль.
  // Два имени в одной директории физически не могут оказаться на разных файловых
  // системах, поэтому переименование атомарно. Общий staging-каталог или /tmp
  // (у нас это tmpfs) дали бы `EXDEV` и бросок на КАЖДОМ сохранении.
  std::filesystem::path temporaryPath = path;
  temporaryPath += ".tmp";

  std::error_code error;

  // Права целевого файла надо перенести на новый inode: в проде уже есть файлы
  // с 0600, а переименование без этого дало бы им 0644 & ~umask.
#ifndef WIN32
  // ★ОДНО ЧТЕНИЕ МЕТАДАННЫХ НА ВЕСЬ ПУТЬ (R60, #205). Прежде существование и
  // режим брались через `std::filesystem::status`, а владелец — отдельным
  // `stat` уже ПОСЛЕ создания временного файла. Два чтения по пути в разные
  // моменты могут описывать РАЗНЫЕ inode: новому файлу достался бы режим
  // одного и владелец другого. Злоумышленник для этого не нужен — хватит
  // одновременной замены. Найдено ревью (итерация 2).
  struct ::stat previousStat{};
  const int previousStatResult = ::stat(path.c_str(), &previousStat);
  if (previousStatResult != 0 && errno != ENOENT)
  {
    // ★Отсутствие файла — это «новая запись», штатный случай. ЛЮБАЯ другая
    // ошибка означает, что мы не знаем, что заменяем, и продолжать нельзя.
    throw std::runtime_error(
      std::format("{} '{}': could not read the metadata of the file it replaces",
        what, path.string()));
  }
  const bool hadPreviousFile = previousStatResult == 0
    && S_ISREG(previousStat.st_mode);
#else
  const auto previousStatus = std::filesystem::status(path, error);
  const bool hadPreviousFile = not error
    && std::filesystem::is_regular_file(previousStatus);
  error.clear();
#endif

  const auto abandon = [&temporaryPath](const std::string& message)
  {
    std::error_code ignored;
    std::filesystem::remove(temporaryPath, ignored);
    throw std::runtime_error(message);
  };

  {
    // ★ФАЙЛ СОЗДАЁТСЯ СРАЗУ С НУЖНЫМИ ПРАВАМИ, ОДНОЙ ОПЕРАЦИЕЙ.
    //
    // ★Ветка по платформе — не украшение: проект собирается и MSVC (см.
    // CMakeLists и COMPILING.md), а безусловные POSIX-заголовки сломали бы эту
    // сборку. Форма взята у соседа: `main.cpp` уже разводит заголовки через
    // `#ifdef WIN32`. Найдено ревью (итерация 3).
    //
    // ★И ЧЕСТНО О РАЗНИЦЕ ГАРАНТИЙ. Под POSIX окно между созданием файла и
    // сужением прав ЗАКРЫТО: режим задаётся самим созданием. Под Windows
    // понятий umask и режима в этом смысле нет, там `permissions` переключает
    // только признак «только для чтения», поэтому ветка проще, а гарантия у неё
    // ýже.
    //
    // ★И АТОМАРНОСТЬ ПОДМЕНЫ ТОЖЕ ОТ ПЛАТФОРМЫ ЗАВИСИТ — я написал здесь
    // обратное и был неправ (найдено ревью, итерация 5; см. заголовок файла).
    // Под POSIX её даёт `rename`, под Windows `std::filesystem::rename` сводится
    // к `MoveFileExW`, у которого одновременный читатель может увидеть
    // ОТСУТСТВИЕ файла в момент подмены.
    //
    // Но и там эта ветка НЕ ХУЖЕ той, что заменяет, а лучше: апстрим открывал
    // `ofstream` прямо по живому пути, то есть обрезал файл на всё время записи
    // и оставлял его пустым НАВСЕГДА при любом сбое. Здесь плохое окно
    // сжимается до мгновения подмены и никогда не остаётся насовсем.
    // Сузить их следующим шагом недостаточно: `ofstream` создал бы предсказуемо
    // названный файл с правами по umask, и в окне между созданием и сужением
    // посторонний читатель успел бы его открыть и удержать дескриптор, пока в
    // файл пишутся данные игрока. Найдено ревью (итерация 2) после того, как
    // итерация 1 передвинула сужение прав вперёд — окно уменьшилось, но не
    // исчезло; исчезает оно только вместе с отдельным шагом.
    //
    // `O_EXCL` здесь тоже не украшение: он не даёт подсунуть на место
    // временного файла символьную ссылку или чужой файл.
    std::filesystem::remove(temporaryPath, error);
    error.clear();

#ifndef WIN32
    // `O_EXCL` здесь тоже не украшение: он не даёт подсунуть на место
    // временного файла символьную ссылку или чужой файл.
    const mode_t mode = hadPreviousFile
      ? static_cast<mode_t>(previousStat.st_mode & 07777)
      : static_cast<mode_t>(0666);   // новый файл — прежнее поведение, по umask

    const int descriptor = ::open(
      temporaryPath.c_str(), O_CREAT | O_EXCL | O_WRONLY, mode);
    if (descriptor < 0)
    {
      throw std::runtime_error(
        std::format("{} '{}' not accessible", what, path.string()));
    }

    if (hadPreviousFile)
    {
      // ★СНАЧАЛА ВЛАДЕЛЕЦ, ПОТОМ РЕЖИМ, и порядок существен: смена владельца
      // гасит биты setuid/setgid, то есть обратный порядок молча терял бы часть
      // только что восстановленного режима. Найдено ревью (итерация 1).
      // ★И БЕЗУСЛОВНО, а не «если владелец отличается от нашего»: в каталоге с
      // битом setgid новый inode получает группу КАТАЛОГА, так что сравнение с
      // `geteuid`/`getegid` было догадкой о владельце, а не фактом.
      // ★FAIL-CLOSED, и это пересмотр моего же решения. Я боялся, что
      // непривилегированный сервер потеряет запись вообще — но когда владелец
      // СОВПАДАЕТ, `fchown` на тот же uid/gid разрешён владельцу файла, то есть
      // обычная запись не страдает. Отказ случается ровно там, где владельца
      // сохранить НЕВОЗМОЖНО, а это и есть случай, ради которого раунд сделан:
      // тихо пропустить его значит вернуть риск неполного отката.
      if (::fchown(descriptor, previousStat.st_uid, previousStat.st_gid) != 0)
      {
        ::close(descriptor);
        abandon(std::format(
          "{} '{}': could not restore the previous owner {}:{}",
          what, path.string(),
          static_cast<unsigned>(previousStat.st_uid),
          static_cast<unsigned>(previousStat.st_gid)));
      }

    }

    // ★ЗАПИСЬ ЧЕРЕЗ ТОТ ЖЕ ДЕСКРИПТОР, А НЕ ПОВТОРНОЕ ОТКРЫТИЕ ПО ПУТИ.
    // Прежде права применялись к открытому inode, дескриптор закрывался, и
    // временный файл открывался заново потоком — а между этими шагами его
    // можно подменить, и тогда переименован окажется inode БЕЗ восстановленных
    // прав. Гарантия раунда терялась бы молча. Найдено ревью (итерация 2).
    {
      std::size_t written = 0;
      while (written < payload.size())
      {
        const ssize_t chunk = ::write(
          descriptor, payload.data() + written, payload.size() - written);
        if (chunk < 0 && errno == EINTR)
          continue;   // прерывание сигналом — не ошибка, повторяем
        if (chunk <= 0)
        {
          ::close(descriptor);
          abandon(std::format("{} '{}' failed to write", what, path.string()));
        }
        written += static_cast<std::size_t>(chunk);
      }

      // ★РЕЖИМ СТАВИТСЯ ПОСЛЕ ЗАПИСИ, И ЭТО НЕ ПЕРЕСТАНОВКА РАДИ КРАСОТЫ.
      // В Linux запись в файл ГАСИТ биты setuid/setgid, если пишет
      // непривилегированный процесс. Поставленный до записи режим терял бы их
      // ровно на непустых файлах — то есть на всех настоящих. Найдено ревью
      // (итерация 3).
      // Порядок целиком: владелец → запись → режим → закрытие. Владелец первым
      // потому, что смена владельца тоже гасит эти биты (итерация 1); режим
      // последним потому, что их гасит и запись.
      if (hadPreviousFile && ::fchmod(descriptor, mode) != 0)
      {
        ::close(descriptor);
        abandon(std::format(
          "{} '{}' could not inherit the permissions of the file it replaces",
          what, path.string()));
      }

      // ★Закрытие проверяется: именно на нём всплывает отложенная ошибка записи
      // (переполнение диска, ошибка ввода-вывода). Проглотить её значит
      // переименовать недописанный файл поверх целого — ровно та беда, от
      // которой помощник и защищает.
      if (::close(descriptor) != 0)
        abandon(std::format("{} '{}' failed to flush", what, path.string()));
    }
#else
    std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
    if (not file.is_open())
    {
      abandon(std::format("{} '{}' not accessible", what, path.string()));
    }


#ifdef WIN32
    if (hadPreviousFile)
    {
      std::filesystem::permissions(temporaryPath, previousStatus.permissions(), error);
      if (error)
      {
        abandon(std::format(
          "{} '{}' could not inherit the permissions of the file it replaces: {}",
          what, path.string(), error.message()));
      }
    }
#endif

    file.write(payload.data(), static_cast<std::streamsize>(payload.size()));

    // ★ПРОВЕРКА ПОСЛЕ ЗАПИСИ ОБЯЗАТЕЛЬНА. Без неё эта функция была бы ХУЖЕ
    // болезни: при переполнении диска недописанный временный файл переехал бы
    // поверх целого, и мы бы САМИ уничтожили запись, которую взялись защищать.
    if (not file.good())
      abandon(std::format("{} '{}' failed to write", what, path.string()));

    // Явное закрытие с проверкой: деструктор потока ошибку сброса ГЛОТАЕТ.
    file.close();
    if (not file.good())
      abandon(std::format("{} '{}' failed to flush", what, path.string()));
#endif
  }

  std::filesystem::rename(temporaryPath, path, error);
  if (error)
  {
    abandon(std::format(
      "{} '{}' failed to replace: {}", what, path.string(), error.message()));
  }
}

//! Убирает недописанные временные файлы, оставшиеся от прерванных сохранений
//! (LOA-fix, round58, backlog #175).
//!
//! ★Не косметика. Осиротевший `X.json.tmp` попадает под обходы каталогов,
//! которые парсят КАЖДЫЙ файл, — и тогда фикс породил бы ровно тот дефект,
//! который чинит. Фильтр расширения в этих обходах ставится той же правкой, а
//! уборка нужна, чтобы мусор не копился годами.
inline void SweepStaleTemporaries(const std::filesystem::path& root) noexcept
{
  try
  {
    if (not std::filesystem::is_directory(root))
      return;

    for (const auto& entry :
      std::filesystem::recursive_directory_iterator(root))
    {
      std::error_code error;
      if (not entry.is_regular_file(error) || error)
        continue;
      if (entry.path().extension() != ".tmp")
        continue;

      std::filesystem::remove(entry.path(), error);
    }
  }
  catch (...)
  {
    // Уборка — не повод не стартовать.
  }
}

} // namespace server::util

#endif // ATOMIC_FILE_HPP
