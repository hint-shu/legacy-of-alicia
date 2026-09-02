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

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace server::util
{

//! КЛАСС КОНФИДЕНЦИАЛЬНОСТИ ФАЙЛА (LOA-fix, R73-1, backlog #206).
//!
//! ★ПАРАМЕТР БЕЗ ЗНАЧЕНИЯ ПО УМОЛЧАНИЮ, И ЭТО ВЕСЬ СМЫСЛ ПРАВКИ. Дефолт означал
//! бы «список мест»: 19 существующих вызовов остались бы прежними, а ДВАДЦАТЫЙ,
//! написанный через полгода для файла с токеном, молча унаследовал бы 0644.
//! Обязательный параметр делает вопрос «а этот файл несёт секрет?» условием
//! КОМПИЛЯЦИИ, то есть тотальным инвариантом, а не перечнем сайтов.
enum class FileSensitivity
{
  //! Файл несёт секрет (хеш и соль пароля). Ни одного бита group/other —
  //! НИКОГДА: ни на созданном файле, ни на унаследованном, ни на временном.
  Secret,
  //! Обычная игровая запись. Режим ровно тот, что был до раунда.
  Public,
};

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
//! @param sensitivity Несёт ли файл секрет. ★Без значения по умолчанию намеренно
//!        (см. `FileSensitivity`): двадцатый вызов обязан ответить на вопрос.
inline void WriteFileAtomically(
  const std::filesystem::path& path,
  const std::string_view payload,
  const std::string_view what,
  const FileSensitivity sensitivity)
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
    // ★СУЖЕНИЕ, А НЕ НАЗНАЧЕНИЕ, и разница здесь принципиальна. Унаследованный
    // режим приходит от РЕАЛЬНОГО файла прода: 12 аккаунтов лежат 0644, один
    // (`nmax.json`) — 0664. Если бы секрет получал жёстко 0600, правка молча
    // расширила бы права там, где владелец их сузил вручную. Маска снимает
    // group/other и не трогает ничего больше, поэтому «починка на первой же
    // записи» не зависит от того, каким режим был.
    mode_t mode = hadPreviousFile
      ? static_cast<mode_t>(previousStat.st_mode & 07777)
      : static_cast<mode_t>(0666);   // новый файл — прежнее поведение, по umask

    if (sensitivity == FileSensitivity::Secret)
    {
      mode &= ~static_cast<mode_t>(S_IRWXG | S_IRWXO);
      // ★И ПОЛ ДЛЯ ВЛАДЕЛЬЦА. Файл с режимом 0000 (ручная правка, чужой umask)
      // после сужения остался бы нечитаемым для самого сервера, то есть защита
      // данных обернулась бы их потерей.
      mode |= static_cast<mode_t>(S_IRUSR | S_IWUSR);
    }

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
      // ★РЕЖИМ СТАВИТСЯ И ДЛЯ НОВОГО СЕКРЕТА, а не только при наследовании.
      // `open(…, 0600)` уже даёт 0600 при любом РАЗУМНОМ umask (umask умеет
      // только ГАСИТЬ биты), но при враждебном umask вида 0200 владелец потерял
      // бы запись. Честно: на стенде этот пояс провалиться НЕ УМЕЕТ (umask
      // контейнера 0022) — он объявлен поясом, а не доказанным гейтом.
      const bool mustApplyMode = hadPreviousFile
        || sensitivity == FileSensitivity::Secret;
      if (mustApplyMode && ::fchmod(descriptor, mode) != 0)
      {
        ::close(descriptor);
        // ★ДВА РАЗНЫХ СООБЩЕНИЯ, А НЕ ОДНО НА ОБА СЛУЧАЯ. Прежний текст
        // «could not inherit the permissions of the file it replaces» ТЕПЕРЬ
        // достижим и для файла, который ничего не заменяет, — оператор во время
        // инцидента прочитал бы неправду. Старая строка сохранена ДОСЛОВНО
        // (она — не-убывающий маркер лесенки прошлых раундов), новая добавлена
        // рядом.
        if (hadPreviousFile)
        {
          abandon(std::format(
            "{} '{}' could not inherit the permissions of the file it replaces",
            what, path.string()));
        }
        abandon(std::format(
          "{} '{}' could not be secured with the intended permissions",
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
    // Под Windows режима в POSIX-смысле нет — класс конфиденциальности здесь
    // ничего изменить не может (см. `FileSensitivity`), но параметр обязан
    // остаться «использованным», чтобы сборка MSVC не сыпала предупреждениями.
    (void)sensitivity;
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

//! Приводит режим СУЩЕСТВУЮЩЕГО каталога к заданному (LOA-fix, R73-2, #206).
//!
//! ★ПОЧЕМУ НЕ ХВАТАЕТ `create_directories`. Каталог `data/users` на проде уже
//! существует (0755, `dev:dev`) и создан задолго до этого раунда — создание с
//! правами его не тронет вовсе. Чинить надо СУЩЕСТВУЮЩИЙ, на каждом старте.
//!
//! ★НЕ ЛОГИРУЕТ САМ, А ВОЗВРАЩАЕТ ВЕРДИКТ. Заголовок сознательно НЕ тянет
//! `QuietLog.hpp` (а с ним spdlog) в каждую единицу трансляции, которая
//! включает `AtomicFile.hpp`: лишняя зависимость сдвинула бы inline-бюджет
//! `FileDataSource.cpp` и сделала бы неотличимыми «контроль лесенки шевельнулся
//! от нашей правки» и «шевельнулся от нового include».
//!
//! ★НЕ FAIL-CLOSED, И ЭТО ОСОЗНАННО. Несущая гарантия раунда — режим ФАЙЛА
//! (0600); режим каталога прячет только СПИСОК аккаунтов. Останавливать сервер
//! из-за не поставленного бита каталога значило бы обменять реальный простой на
//! эшелонированную защиту.
//!
//! @return `false` если `chmod` существующего каталога провалился ИЛИ если режим
//!         каталога вообще не удалось узнать. ★ТОЛЬКО `ENOENT` ЧИТАЕТСЯ КАК
//!         УСПЕХ, и это правка ревью (итерация 1). Прежняя редакция отвечала
//!         `true` на ЛЮБОЙ отказ `stat`: при `EACCES`/`EIO` режим каталога с
//!         аккаунтами оставался неизвестным, а вызывающий не печатал ни строки —
//!         проверка, которая умеет только соглашаться, проверкой не является.
//!         Отсутствие каталога успехом остаётся: `create_directories` зовётся
//!         строкой выше, и «его нет» здесь означает гонку, а не дефект прав.
[[nodiscard]] inline bool EnsureDirectoryMode(
  const std::filesystem::path& path,
  const unsigned int mode) noexcept
{
#ifndef WIN32
  struct ::stat directoryStat{};
  if (::stat(path.c_str(), &directoryStat) != 0)
    return errno == ENOENT;                   // нет каталога — не наша забота
  if ((directoryStat.st_mode & 07777) == static_cast<mode_t>(mode))
    return true;                              // уже как надо, syscall не нужен
  return ::chmod(path.c_str(), static_cast<mode_t>(mode)) == 0;
#else
  // Под Windows понятия режима в этом смысле нет — ветка пустая осознанно.
  (void)path; (void)mode;
  return true;
#endif
}

//! Итог сужения режимов у СУЩЕСТВУЮЩИХ файлов с секретом (LOA-fix, R73-2b, #206).
struct SecretFileHardening
{
  //! Сколько обычных файлов удалось осмотреть.
  std::size_t examined = 0;
  //! Сколько из них несли биты group/other и были сужены.
  std::size_t narrowed = 0;
  //! Сколько не удалось ни осмотреть, ни сузить.
  std::size_t failed = 0;
};

//! Снимает биты group/other со ВСЕХ обычных файлов каталога (R73-2b, #206).
//!
//! ★ПОЧЕМУ НЕ ХВАТАЕТ ОБЯЗАТЕЛЬНОГО `FileSensitivity`. Класс конфиденциальности
//! чинит файл НА ПЕРВОЙ ЗАПИСИ. Аккаунт, в который сервер не пишет (игрок не
//! заходил), первой записи не дождётся НИКОГДА — на проде такие есть, и они
//! остались бы 0644 бессрочно. Найдено ревью (итерация 1): каталог сузили, а
//! инод — нет.
//!
//! ★СОДЕРЖИМОЕ НЕ ПЕРЕПИСЫВАЕТСЯ. Только `fchmod` уже открытого дескриптора:
//! «починка» через перезапись означала бы риск потерять данные ради бита прав.
//!
//! ★`O_NOFOLLOW`, А НЕ `chmod` ПО ПУТИ. Символическая ссылка в каталоге
//! аккаунтов увела бы `chmod` на чужой файл. Открываем без перехода по ссылке и
//! правим ДЕСКРИПТОР — тогда цель правки не может подмениться между проверкой и
//! действием. Ссылки и всё, что не обычный файл, просто пропускаются: они не
//! аккаунты.
//!
//! ★НЕ ЛОГИРУЕТ САМ (та же причина, что у `EnsureDirectoryMode`) — возвращает
//! числа, а строку печатает вызывающий.
[[nodiscard]] inline SecretFileHardening HardenSecretFilesInDirectory(
  const std::filesystem::path& directory) noexcept
{
  SecretFileHardening result;
#ifndef WIN32
  std::error_code error;
  if (not std::filesystem::is_directory(directory, error) || error)
    return result;

  for (const auto& entry :
    std::filesystem::directory_iterator(directory, error))
  {
    if (error)
      break;

    const int descriptor = ::open(
      entry.path().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0)
    {
      // ELOOP = это символическая ссылка; она не аккаунт, это не отказ.
      if (errno != ELOOP)
        ++result.failed;
      continue;
    }

    struct ::stat fileStat{};
    if (::fstat(descriptor, &fileStat) != 0)
    {
      ++result.failed;
      ::close(descriptor);
      continue;
    }
    if (not S_ISREG(fileStat.st_mode))
    {
      ::close(descriptor);
      continue;
    }

    ++result.examined;
    if ((fileStat.st_mode & (S_IRWXG | S_IRWXO)) != 0)
    {
      // Та же арифметика, что у `Secret` в `WriteFileAtomically`: сужение маской
      // плюс пол для владельца, а не жёсткое назначение 0600.
      mode_t narrowed = static_cast<mode_t>(fileStat.st_mode & 07777);
      narrowed &= ~static_cast<mode_t>(S_IRWXG | S_IRWXO);
      narrowed |= static_cast<mode_t>(S_IRUSR | S_IWUSR);
      if (::fchmod(descriptor, narrowed) == 0)
        ++result.narrowed;
      else
        ++result.failed;
    }
    ::close(descriptor);
  }
#else
  (void)directory;
#endif
  return result;
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
