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
  #include <dirent.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

//! ПРИЁМНИК РЕДКИХ ПРЕДУПРЕЖДЕНИЙ ЭТОГО ЗАГОЛОВКА (LOA-fix, R73-5, ревью 4).
//!
//! ★ЗАЧЕМ УКАЗАТЕЛЬ, А НЕ `QuietLog.hpp` ПРЯМО ЗДЕСЬ. Три правки этой итерации
//! (усыновление владельца, отказ символическим ссылкам, сужение поздно
//! пришедшего файла) обязаны быть СЛЫШНЫ — «сузили молча» неотличимо от «не
//! сузили». Но затащить сюда spdlog значит включить его в КАЖДУЮ единицу
//! трансляции, которая берёт этот заголовок, в том числе в юнит-тест, который
//! сегодня линкуется без него. Указатель на функцию оставляет заголовок
//! свободным от зависимости и — что важнее — делает сам факт предупреждения
//! ПРОВЕРЯЕМЫМ: тест ставит свой приёмник и считает сообщения, вместо того
//! чтобы разглядывать лог глазами.
using FileWarningSink = void (*)(std::string_view);

namespace detail
{

inline std::atomic<FileWarningSink> gFileWarningSink{nullptr};

} // namespace detail

//! Ставит приёмник. Зовётся один раз на старте (`FileDataSource::Initialize`,
//! конструктор `LocalAuthenticationBackend`); повторная установка того же
//! указателя безвредна.
inline void SetFileWarningSink(const FileWarningSink sink) noexcept
{
  detail::gFileWarningSink.store(sink, std::memory_order::relaxed);
}

//! ПЛОЩАДКА ДРОССЕЛЯ: одна на каждое место, которое умеет жаловаться.
//!
//! ★ДРОССЕЛЬ ОБЯЗАТЕЛЕН, А НЕ ЖЕЛАТЕЛЕН. Отказ ссылке и сужение режима стоят на
//! пути ЧТЕНИЯ аккаунта, то есть на каждом входе; строка на каждый вход — это
//! ровно дефект R57 (`HandleRaceUserPos`, 15 350 строк/час), только с другой
//! стороны. Подавленные сообщения не теряются: их число едет в следующем.
struct WarningThrottle
{
  static constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::min();
  std::atomic<std::int64_t> lastEmitNanos{kNever};
  std::atomic<std::size_t> suppressed{0};
};

//! Не чаще одного сообщения в минуту с одной площадки.
inline constexpr auto kFileWarningGap = std::chrono::seconds(60);

//! Печатает сообщение через установленный приёмник, но не чаще, чем раз в
//! `kFileWarningGap` с этой площадки. ★НИ ОДНОГО БРОСКА НАРУЖУ: зовётся из
//! `noexcept`-тел, где бросок означал бы `std::terminate`.
template <typename... Args>
void ReportFileWarning(
  WarningThrottle& throttle,
  const std::format_string<Args...> fmt,
  Args&&... args) noexcept
{
  const auto sink = detail::gFileWarningSink.load(std::memory_order::relaxed);
  if (sink == nullptr)
  {
    // Приёмник не установлен — считаем подавленным, чтобы первое сообщение
    // ПОСЛЕ установки честно назвало число пропущенных.
    throttle.suppressed.fetch_add(1, std::memory_order::relaxed);
    return;
  }

  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto gap = std::chrono::duration_cast<std::chrono::nanoseconds>(
    kFileWarningGap).count();

  std::int64_t last = throttle.lastEmitNanos.load(std::memory_order::relaxed);
  for (;;)
  {
    // ★СРАВНЕНИЕ ТОЛЬКО ПОСЛЕ ПРОВЕРКИ НА `kNever`: вычитание минимального
    // int64 из текущего времени переполняется, то есть первое же сообщение
    // ушло бы в неопределённое поведение.
    if (last != WarningThrottle::kNever && now - last < gap)
    {
      throttle.suppressed.fetch_add(1, std::memory_order::relaxed);
      return;
    }
    if (throttle.lastEmitNanos.compare_exchange_weak(
          last, now, std::memory_order::relaxed, std::memory_order::relaxed))
    {
      break;
    }
  }

  const auto suppressed = throttle.suppressed.exchange(
    0, std::memory_order::relaxed);
  try
  {
    const auto message = std::format(fmt, std::forward<Args>(args)...);
    if (suppressed == 0)
      sink(message);
    else
      sink(std::format("{} (ещё {} подобных сообщений подавлено)",
        message, suppressed));
  }
  catch (...)
  {
    // Потерянная строка лога дешевле потерянного процесса.
  }
}

namespace detail
{

//! ЭКРАНИРОВАНИЕ ИМЕНИ, ПРИШЕДШЕГО ИЗ ФАЙЛОВОЙ СИСТЕМЫ (LOA-fix R73-8, ревью 5).
//!
//! ★ИМЯ ФАЙЛА — ЭТО ВВОД, А НЕ ТЕКСТ ПРОГРАММЫ. Всё, что не `/` и не NUL,
//! годится в имя записи каталога, в том числе перевод строки. Помощник,
//! положивший рядом ссылку с именем «`a\nWARN подделанная строка`», одним
//! ЗАДРОССЕЛИРОВАННЫМ предупреждением напечатал бы НЕСКОЛЬКО видимых записей
//! лога: дроссель считает ВЫЗОВЫ, а не строки, которые из них вылезли. То есть
//! подделка журнала обходила бы ровно тот пояс, который заведён против потока
//! строк. Найдено ревью (итерация 5).
//!
//! ★И ДЛИНА ТОЖЕ ВВОД: 255-байтное имя × отвергнутая ссылка = строка, в которой
//! сообщение не видно. Хвост обрезается.
inline std::string EscapeForLog(const std::string_view raw) noexcept
{
  constexpr std::size_t kMaxLoggedBytes = 200;
  static constexpr char kHex[] = "0123456789abcdef";
  try
  {
    std::string escaped;
    escaped.reserve(raw.size() + 8);
    std::size_t taken = 0;
    for (const char symbol : raw)
    {
      if (taken >= kMaxLoggedBytes)
      {
        escaped += "...";
        break;
      }
      const auto byte = static_cast<unsigned char>(symbol);
      if (byte == '\\')
      {
        escaped += "\\\\";
      }
      else if (byte < 0x20 || byte == 0x7f)
      {
        // Перевод строки, возврат каретки, забой, управляющие последовательности
        // терминала — всё это становится видимым текстом, а не действием.
        escaped += "\\x";
        escaped += kHex[byte >> 4];
        escaped += kHex[byte & 0x0f];
      }
      else
      {
        escaped += symbol;
      }
      ++taken;
    }
    return escaped;
  }
  catch (...)
  {
    // Потерянное имя дешевле потерянного процесса: зовётся из `noexcept`-тел.
    return std::string{};
  }
}

} // namespace detail

//! Путь в виде, пригодном для ОДНОЙ строки лога. ★ЕДИНСТВЕННАЯ ФОРМА, В КОТОРОЙ
//! путь попадает в сообщение этого заголовка: `path.string()` прямо в
//! `std::format` — это и есть дефект, от которого спасает эта обёртка.
inline std::string LogPath(const std::filesystem::path& path) noexcept
{
  try
  {
    return detail::EscapeForLog(path.string());
  }
  catch (...)
  {
    return std::string{};
  }
}

#ifndef WIN32
namespace detail
{

//! ОТКРЫВАЕТ КАТАЛОГ, НЕ ПРОЙДЯ НИ ОДНОЙ СИМВОЛИЧЕСКОЙ ССЫЛКИ
//! (LOA-fix R73-9, правка ревью, итерация 5).
//!
//! ★ЗАЧЕМ, ЕСЛИ ЕСТЬ `O_NOFOLLOW`. `O_NOFOLLOW` защищает ТОЛЬКО ПОСЛЕДНИЙ
//! компонент пути. Ссылка на месте самого КАТАЛОГА (`data/users -> /tmp/theirs`)
//! проходилась молча: по ней ходили инициализация, чтения, записи, проход
//! сужения и `stat` владельца — то есть вход мог принять хеш пароля из чужого
//! дерева, а сохранение — записать файл за пределами `data/`. Инвариант «под
//! `data/` не ходят по ссылкам» был утверждением про имя файла, а не про путь.
//! Найдено ревью (итерация 5).
//!
//! ★ПОЭТОМУ ПУТЬ РАЗБИРАЕТСЯ ПОКОМПОНЕНТНО: каждый шаг — `openat` с
//! `O_DIRECTORY | O_NOFOLLOW` от дескриптора предыдущего. Ссылка ЛЮБОГО уровня
//! даёт `ELOOP`, и это свойство системного вызова, а не проверка с окном между
//! «посмотрели» и «открыли»: подменить компонент после того, как он открыт,
//! невозможно — дескриптор держит ИНОД, а не имя.
//!
//! ★ЦЕНА НАЗВАНА ЧЕСТНО: одно открытие на компонент, то есть 3-8 системных
//! вызовов на обращение к файлу. Записи и чтения записей идут через кэш
//! `DataDirector`, в горячем цикле тика их нет; альтернатива — держать
//! дескрипторы каталогов открытыми — заводит своё состояние и своё расхождение
//! с диском, а цена сегодня не измеряется ничем.
//!
//! `..` ОТВЕРГАЕТСЯ, а не разрешается: в путях данных его не бывает, а
//! разрешить его значило бы вернуть выход из дерева другим способом.
//!
//! @return Дескриптор каталога (закрывать вызывающему) или `-1` с `errno`.
[[nodiscard]] inline int OpenDirectoryNoSymlinks(
  const std::filesystem::path& directory) noexcept
{
  int current = -1;
  try
  {
    current = ::open(
      directory.is_absolute() ? "/" : ".",
      O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current < 0)
      return -1;

    for (const auto& part : directory)
    {
      const auto component = part.string();
      if (component.empty() || component == "/" || component == ".")
        continue;
      if (component == "..")
      {
        ::close(current);
        errno = EINVAL;
        return -1;
      }

      const int next = ::openat(
        current, component.c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      // ★`errno` СНИМАЕТСЯ ДО `close`: закрытие вправе его затереть, и тогда
      // вызывающий читал бы про отказ не тот код, что случился.
      const int failure = errno;
      ::close(current);
      if (next < 0)
      {
        errno = failure;
        return -1;
      }
      current = next;
    }

    return current;
  }
  catch (...)
  {
    if (current >= 0)
      ::close(current);
    errno = ENOMEM;
    return -1;
  }
}

//! Закрытие дескриптора, не трогающее `errno` вызывающего.
inline void CloseKeepingErrno(const int descriptor) noexcept
{
  const int preserved = errno;
  if (descriptor >= 0)
    ::close(descriptor);
  errno = preserved;
}

//! ДЕСКРИПТОР, КОТОРЫЙ ЗАКРЫВАЕТСЯ И НА БРОСКЕ.
//!
//! ★НУЖЕН ПОТОМУ, ЧТО `WriteFileAtomically` БРОСАЕТ ИЗ ДЕСЯТКА МЕСТ. Ручное
//! `close` перед каждым `throw` — это список мест, и одиннадцатый бросок,
//! написанный через полгода, утёк бы дескриптором каталога на каждой неудачной
//! записи; при частой ошибке ФС сервер упёрся бы в `EMFILE` и перестал бы
//! открывать сокеты. Обязательство, которое умеет не выполниться, — не
//! обязательство.
struct DescriptorGuard
{
  int descriptor = -1;

  DescriptorGuard() noexcept = default;
  explicit DescriptorGuard(const int opened) noexcept : descriptor(opened) {}
  DescriptorGuard(const DescriptorGuard&) = delete;
  DescriptorGuard& operator=(const DescriptorGuard&) = delete;

  //! ★ПЕРЕМЕЩЕНИЕ, НО НЕ КОПИРОВАНИЕ (правка ревью, итерация 6). Обход каталога
  //! обязан ОТДАТЬ свой дескриптор проходу сужения — иначе тот разбирает путь
  //! заново и работает уже не обязательно с тем инодом, который перечислял
  //! обход. Копирование при этом остаётся запрещённым: две копии закрыли бы
  //! один дескриптор дважды, а второе закрытие пришлось бы на чужой файл,
  //! успевший занять тот же номер.
  DescriptorGuard(DescriptorGuard&& other) noexcept
    : descriptor(other.descriptor)
  {
    other.descriptor = -1;
  }

  DescriptorGuard& operator=(DescriptorGuard&& other) noexcept
  {
    if (this != &other)
    {
      CloseKeepingErrno(descriptor);
      descriptor = other.descriptor;
      other.descriptor = -1;
    }
    return *this;
  }

  ~DescriptorGuard()
  {
    CloseKeepingErrno(descriptor);
  }
};

} // namespace detail
#endif

//! СОЗДАЁТ КАТАЛОГ ДАННЫХ, НЕ ПРОЙДЯ НИ ОДНОЙ СИМВОЛИЧЕСКОЙ ССЫЛКИ
//! (LOA-fix R73-12, правка ревью, итерация 6).
//!
//! ★ЗАЧЕМ, ЕСЛИ ЕСТЬ `std::filesystem::create_directories`. Инвариант раунда —
//! «под `data/` не ходят по ссылкам» — держался у ЧТЕНИЯ, ЗАПИСИ и ОБХОДА, но
//! не у СОЗДАНИЯ: `create_directories` разрешает путь именами и молча проходит
//! ссылку на месте любого промежуточного компонента. Достаточно подменить
//! `data/characters/equipment` ссылкой на чужое дерево — и старт сервера сам
//! СОЗДАЁТ там `items`, после чего каждая запись и каждое удаление предмета
//! работают за пределами `data/`. Найдено ревью (итерация 6): «P2 не тотален».
//!
//! ★ПОЭТОМУ КАЖДЫЙ КОМПОНЕНТ СОЗДАЁТСЯ И ОТКРЫВАЕТСЯ ОТ ДЕСКРИПТОРА
//! ПРЕДЫДУЩЕГО: `mkdirat` + `openat(O_DIRECTORY | O_NOFOLLOW)`. Ссылка любого
//! уровня даёт `ELOOP` — свойство системного вызова, а не проверка с окном.
//!
//! ★РЕЖИМ 0777, А НЕ 0755, И ЭТО НЕ РАСШИРЕНИЕ ПРАВ: ровно его передаёт
//! `create_directories`, и ровно его гасит `umask` процесса. Поставить 0755
//! значило бы НАЗНАЧИТЬ права там, где раунд обещал их не трогать (см.
//! `Initialize`: пятнадцать каталогов создаются как создавались).
//!
//! @return `true`, если каталог существует по окончании вызова.
[[nodiscard]] inline bool CreateManagedDirectories(
  const std::filesystem::path& directory) noexcept
{
#ifndef WIN32
  int current = -1;
  try
  {
    current = ::open(
      directory.is_absolute() ? "/" : ".",
      O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current < 0)
      return false;

    for (const auto& part : directory)
    {
      const auto component = part.string();
      if (component.empty() || component == "/" || component == ".")
        continue;
      if (component == "..")
      {
        ::close(current);
        errno = EINVAL;
        return false;
      }

      int next = ::openat(
        current, component.c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0 && errno == ENOENT)
      {
        // ★`EEXIST` — НЕ ОШИБКА: соседний поток (или соседний процесс) вправе
        // создать тот же каталог между нашими `openat` и `mkdirat`. Гонка
        // разрешается повторным открытием, а не отказом.
        if (::mkdirat(current, component.c_str(), 0777) != 0 && errno != EEXIST)
        {
          const int failure = errno;
          ::close(current);
          errno = failure;
          return false;
        }
        next = ::openat(
          current, component.c_str(),
          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      }

      // `errno` снимается ДО `close`: закрытие вправе его затереть.
      const int failure = errno;
      ::close(current);
      if (next < 0)
      {
        errno = failure;
        return false;
      }
      current = next;
    }

    ::close(current);
    return true;
  }
  catch (...)
  {
    if (current >= 0)
      ::close(current);
    errno = ENOMEM;
    return false;
  }
#else
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error)
    return std::filesystem::is_directory(directory, error) && not error;
  return true;
#endif
}

//! УДАЛЯЕТ УПРАВЛЯЕМУЮ ЗАПИСЬ, НЕ ПРОЙДЯ НИ ОДНОЙ ССЫЛКИ В ПУТИ
//! (LOA-fix R73-12, правка ревью, итерация 6).
//!
//! ★ЗАЧЕМ. Пятнадцать методов `Delete*` звали `std::filesystem::remove(path)`.
//! `remove` снимает КОНЕЧНУЮ ссылку саму (это правильно), но промежуточные
//! компоненты проходит НАСКВОЗЬ: подмена `data/characters/equipment` ссылкой на
//! чужое дерево превращала штатное `DeleteItem(17)` в удаление ЧУЖОГО файла.
//! Инвариант «под `data/` не ходят по ссылкам» обязан держаться у всех четырёх
//! действий — чтение, запись, обход, удаление, — иначе он не инвариант, а
//! список мест.
//!
//! ★ОТСУТСТВИЕ ФАЙЛА — УСПЕХ. Прежний `std::filesystem::remove` возвращал
//! `false` без исключения на отсутствующем файле, и все пятнадцать вызывающих
//! этот исход игнорировали; менять здесь политику значило бы завести отказ там,
//! где его не было.
//!
//! ★ВЕРДИКТ ЧИТАЕТ ОПЕРАТОР. Настоящая неудача (права, ввод-вывод, подменённый
//! каталог) уходит в дроссель предупреждений: удаление, которое молча не
//! состоялось, оставляет запись жить после того, как код считает её снятой.
inline bool RemoveManagedFile(const std::filesystem::path& path) noexcept
{
#ifndef WIN32
  std::string fileName;
  try
  {
    fileName = path.filename().string();
  }
  catch (...)
  {
    return false;
  }
  if (fileName.empty() || fileName == "." || fileName == "..")
    return false;

  detail::DescriptorGuard directory{
    detail::OpenDirectoryNoSymlinks(path.parent_path())};
  if (directory.descriptor < 0)
  {
    if (errno == ENOENT)
      return true;   // каталога нет — значит нет и записи
    static WarningThrottle directoryThrottle;
    ReportFileWarning(directoryThrottle,
      "Data file '{}' could not be deleted: its directory is not reachable "
      "without following a symbolic link (errno {})", LogPath(path), errno);
    return false;
  }

  if (::unlinkat(directory.descriptor, fileName.c_str(), 0) != 0)
  {
    if (errno == ENOENT)
      return true;
    static WarningThrottle unlinkThrottle;
    ReportFileWarning(unlinkThrottle,
      "Data file '{}' could not be deleted (errno {}); the record stays on disk "
      "after the server considered it removed", LogPath(path), errno);
    return false;
  }
  return true;
#else
  std::error_code error;
  std::filesystem::remove(path, error);
  return not static_cast<bool>(error);
#endif
}

//! ПОТОЛОК РАЗМЕРА ОДНОЙ УПРАВЛЯЕМОЙ ЗАПИСИ (LOA-fix R73-10, ревью 5).
//!
//! ★ЗАЧЕМ ПОТОЛОК ВООБЩЕ. Общий читатель складывает файл в память ДО разбора, а
//! чтение аккаунта стоит на пути входа — то есть ДО аутентификации. Помощник,
//! оставивший разрежённый `data/users/Alice.json` на несколько гигабайт, одной
//! попыткой входа именем `Alice` заставил бы контейнер (2 ГиБ) вычитать его
//! целиком. Прежний `nlohmann::json::parse(ifstream)` разбирал потоком и
//! отвергал на первом же негодном байте — то есть общий вход, заведённый ради
//! ссылок, ЗАВЁЛ бы отказ в обслуживании, которого до него не было.
//!
//! ★ЧИСЛО ВЗЯТО ИЗ ЗАМЕРА, А НЕ ИЗ ГОЛОВЫ: самая большая настоящая запись в
//! снимках прода — файл персонажа 2 878 байт. Потолок 4 МиБ даёт запас ×1400 и
//! при этом ограничивает вход одной попытки величиной, которую сервер переживёт.
inline constexpr std::uintmax_t kMaxManagedRecordBytes = 4u * 1024u * 1024u;

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
//!
//! ★ЧЕГО ОНА НЕ СОХРАНИТ: содержимое длиннее `kMaxManagedRecordBytes`. Потолок
//! у читателя без потолка у писателя — это запись, которая успешно сохраняется
//! и перестаёт читаться после перезапуска (правка ревью, итерация 6).
inline void WriteFileAtomically(
  const std::filesystem::path& path,
  const std::string_view payload,
  const std::string_view what,
  const FileSensitivity sensitivity)
{
  // ★ПОТОЛОК ЗАПИСИ — ТОТ ЖЕ, ЧТО У ЧТЕНИЯ, И ЭТО НЕ СИММЕТРИЯ РАДИ КРАСОТЫ
  // (правка ревью, итерация 6). Итерация 5 завела потолок ТОЛЬКО у читателя, и
  // тем самым создала запись, которая успешно СОХРАНЯЕТСЯ и не читается после
  // перезапуска: агрегат персонажа (почтовый ящик плюс несколько растущих
  // наборов uid) — единственный класс, который до 4 МиБ дорастить реально.
  // Молчаливая потеря записи хуже отказа сохранения: отказ виден сразу, у него
  // есть строка и есть виновник, а «сохранилось и пропало» обнаруживается
  // игроком через недели.
  //
  // ★ОТКАЗ ДО ЛЮБОГО КАСАНИЯ ДИСКА: старый файл остаётся целым, временный не
  // создаётся, и это ровно тот инвариант («либо целиком, либо не тронули»),
  // ради которого функция и написана. Бросок — тем же способом, что все прочие
  // отказы этой функции, поэтому вызывающие не получают нового класса исходов.
  if (payload.size() > static_cast<std::size_t>(kMaxManagedRecordBytes))
  {
    throw std::runtime_error(
      std::format(
        "{} '{}' is {} bytes, beyond the {} byte ceiling for a single record, "
        "and was not written", what, LogPath(path), payload.size(),
        kMaxManagedRecordBytes));
  }

#ifndef WIN32
  // ★ВСЯ ЗАПИСЬ ПРИКРЕПЛЕНА К ДЕСКРИПТОРУ КАТАЛОГА, А НЕ К ПУТИ (правка ревью,
  // итерация 5). Прежняя редакция работала именами: `lstat(path)`,
  // `open(temporaryPath)`, `stat(parent)`, `rename(tmp, path)`. Каждый из этих
  // вызовов ПРОХОДИЛ путь заново и шёл по символической ссылке на месте самого
  // КАТАЛОГА, поэтому `data/users -> /tmp/theirs` уводил запись файла игрока за
  // пределы дерева данных, а `O_NOFOLLOW` этого не видел — он про последний
  // компонент. Теперь путь разбирается ОДИН раз и покомпонентно
  // (`detail::OpenDirectoryNoSymlinks`), а всё дальнейшее — `fstatat`,
  // `openat`, `fstat`, `renameat` — адресует ИНОД каталога. Подменить его
  // после открытия невозможно.
  const auto fileName = path.filename().string();
  if (fileName.empty() || fileName == "." || fileName == "..")
  {
    throw std::runtime_error(
      std::format("{} '{}' not accessible", what, LogPath(path)));
  }
  // ★ВРЕМЕННЫЙ ФАЙЛ — В ТОМ ЖЕ КАТАЛОГЕ, и это условие корректности, а не стиль.
  // Два имени в одной директории физически не могут оказаться на разных файловых
  // системах, поэтому переименование атомарно. Общий staging-каталог или /tmp
  // (у нас это tmpfs) дали бы `EXDEV` и бросок на КАЖДОМ сохранении.
  const auto temporaryName = fileName + ".tmp";

  detail::DescriptorGuard directory{
    detail::OpenDirectoryNoSymlinks(path.parent_path())};
  if (directory.descriptor < 0)
  {
    if (errno == ELOOP)
    {
      throw std::runtime_error(
        std::format("{} '{}' is a symbolic link and is refused: symbolic links "
          "under the data directory are not managed records",
          what, LogPath(path)));
    }
    throw std::runtime_error(
      std::format("{} '{}': could not read the metadata of the file it replaces",
        what, LogPath(path)));
  }

  // Права целевого файла надо перенести на новый inode: в проде уже есть файлы
  // с 0600, а переименование без этого дало бы им 0644 & ~umask.
  //
  // ★ОДНО ЧТЕНИЕ МЕТАДАННЫХ НА ВЕСЬ ПУТЬ (R60, #205). Прежде существование и
  // режим брались через `std::filesystem::status`, а владелец — отдельным
  // `stat` уже ПОСЛЕ создания временного файла. Два чтения по пути в разные
  // моменты могут описывать РАЗНЫЕ inode: новому файлу достался бы режим
  // одного и владелец другого. Злоумышленник для этого не нужен — хватит
  // одновременной замены. Найдено ревью (итерация 2).
  // ★БЕЗ ПЕРЕХОДА ПО ССЫЛКЕ, И ЭТО НЕ СТИЛЬ (правка ревью, итерация 4).
  // Обычный `stat` ХОДИТ ПО ССЫЛКЕ: у `data/users/Alice.json`, ставшей ссылкой
  // на чужой файл 0644, он возвращал метаданные ЦЕЛИ, `hadPreviousFile`
  // становился истинным, новый инод наследовал режим цели — а `rename` заменял
  // САМУ ССЫЛКУ, оставляя цель с хешем пароля лежать под прежними правами.
  // Ссылка под `data/` не управляемая запись (см. `ReadManagedFile`), и писать
  // сквозь неё нельзя ни при каких правах.
  struct ::stat previousStat{};
  const int previousStatResult = ::fstatat(
    directory.descriptor, fileName.c_str(), &previousStat, AT_SYMLINK_NOFOLLOW);
  if (previousStatResult != 0 && errno != ENOENT)
  {
    // ★Отсутствие файла — это «новая запись», штатный случай. ЛЮБАЯ другая
    // ошибка означает, что мы не знаем, что заменяем, и продолжать нельзя.
    throw std::runtime_error(
      std::format("{} '{}': could not read the metadata of the file it replaces",
        what, LogPath(path)));
  }
  if (previousStatResult == 0 && S_ISLNK(previousStat.st_mode))
  {
    throw std::runtime_error(
      std::format("{} '{}' is a symbolic link and is refused: symbolic links "
        "under the data directory are not managed records",
        what, LogPath(path)));
  }
  const bool hadPreviousFile = previousStatResult == 0
    && S_ISREG(previousStat.st_mode);

  const auto abandon = [&directory, &temporaryName](const std::string& message)
  {
    ::unlinkat(directory.descriptor, temporaryName.c_str(), 0);
    throw std::runtime_error(message);
  };

  {
    // ★ФАЙЛ СОЗДАЁТСЯ СРАЗУ С НУЖНЫМИ ПРАВАМИ, ОДНОЙ ОПЕРАЦИЕЙ. Окно между
    // созданием файла и сужением прав ЗАКРЫТО: режим задаётся самим созданием.
    // Сузить их следующим шагом недостаточно: `ofstream` создал бы предсказуемо
    // названный файл с правами по umask, и в окне между созданием и сужением
    // посторонний читатель успел бы его открыть и удержать дескриптор, пока в
    // файл пишутся данные игрока. Найдено ревью (итерация 2) после того, как
    // итерация 1 передвинула сужение прав вперёд — окно уменьшилось, но не
    // исчезло; исчезает оно только вместе с отдельным шагом.
    ::unlinkat(directory.descriptor, temporaryName.c_str(), 0);

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

    // `O_EXCL` здесь тоже не украшение: он не даёт подсунуть на место
    // временного файла символьную ссылку или чужой файл.
    const int descriptor = ::openat(
      directory.descriptor, temporaryName.c_str(),
      O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, mode);
    if (descriptor < 0)
    {
      throw std::runtime_error(
        std::format("{} '{}' not accessible", what, LogPath(path)));
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
          what, LogPath(path),
          static_cast<unsigned>(previousStat.st_uid),
          static_cast<unsigned>(previousStat.st_gid)));
      }

    }
    else if (sensitivity == FileSensitivity::Secret)
    {
      // ★НОВЫЙ СЕКРЕТ БЕРЁТ ВЛАДЕЛЬЦА У СВОЕГО КАТАЛОГА (правка ревью,
      // итерация 4).
      //
      // Прежде `fchown` стоял ТОЛЬКО в ветке наследования, и это оставляло
      // дыру ровно там, где раунд обещал порядок: разовый `chown dev:dev` на
      // деплое приводит в порядок каталог и УЖЕ СУЩЕСТВУЮЩИЕ аккаунты, но
      // первый же вход нового игрока создаёт файл от владельца ПРОЦЕССА
      // (в контейнере это root) — `root:root 0600`. Непривилегированный
      // помощник (`set-password.py`, `add-friend.sh`) такой файл не откроет:
      // путь успеха у честного администратора отнят, и разовым `chown` это не
      // чинится, потому что следующий новый аккаунт снова придёт от root.
      //
      // Каталог — единственный источник правды о том, кому эти файлы
      // принадлежат: он и создан деплоем, и переживает перезапуски, и виден
      // отсюда одним `fstat` УЖЕ ОТКРЫТОГО дескриптора — то есть без второго
      // прохода по пути и без шанса спросить не тот каталог.
      //
      // ★НЕ FAIL-CLOSED, В ОТЛИЧИЕ ОТ ВЕТКИ НАСЛЕДОВАНИЯ, и разница
      // содержательная. Там отказ означал бы «мы НЕ СМОГЛИ сохранить владельца
      // существующей записи» — то есть потерю уже имеющегося свойства. Здесь
      // отказ (`EPERM` у непривилегированного процесса, чей каталог принадлежит
      // другому) означает лишь, что удобство помощника недостижимо; НЕСУЩАЯ
      // гарантия раунда — режим 0600 — при этом целà и ставится ниже
      // безусловно. Обменять вход игрока на удобство скрипта было бы неверной
      // ценой.
      struct ::stat directoryStat{};
      if (::fstat(directory.descriptor, &directoryStat) == 0
        && ::fchown(descriptor, directoryStat.st_uid, directoryStat.st_gid) != 0)
      {
        // ★ОДНА ПЛОЩАДКА ДРОССЕЛЯ НА ВЕСЬ ПРОЦЕСС: регистрация идёт по одному
        // разу на игрока, но при систематически неверном владельце каталога
        // строка иначе шла бы на каждый первый вход.
        static WarningThrottle ownershipThrottle;
        ReportFileWarning(ownershipThrottle,
          "{} '{}': could not adopt the owner {}:{} of its directory (errno {}); "
          "the file is owner-only regardless, but an unprivileged helper may not "
          "be able to read it",
          what, LogPath(path),
          static_cast<unsigned>(directoryStat.st_uid),
          static_cast<unsigned>(directoryStat.st_gid),
          errno);
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
          abandon(std::format("{} '{}' failed to write", what, LogPath(path)));
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
            what, LogPath(path)));
        }
        abandon(std::format(
          "{} '{}' could not be secured with the intended permissions",
          what, LogPath(path)));
      }

      // ★Закрытие проверяется: именно на нём всплывает отложенная ошибка записи
      // (переполнение диска, ошибка ввода-вывода). Проглотить её значит
      // переименовать недописанный файл поверх целого — ровно та беда, от
      // которой помощник и защищает.
      if (::close(descriptor) != 0)
        abandon(std::format("{} '{}' failed to flush", what, LogPath(path)));
    }
  }

  // ★ПОДМЕНА ТОЖЕ ПРИКРЕПЛЕНА К КАТАЛОГУ. `std::filesystem::rename` разрешает
  // путь заново, то есть между созданием временного файла и подменой каталог
  // можно было подменить ссылкой и увести готовую запись наружу. `renameat` от
  // того же дескриптора этого окна не имеет.
  //
  // ★ГРАНИЦА ГАРАНТИИ, СКАЗАННАЯ ТОЧНО. Под POSIX (наш прод — Linux, ext4)
  // подмена атомарна: `rename` поверх существующего имени либо произошла, либо
  // нет, и читатель всегда видит либо старый файл целиком, либо новый целиком.
  if (::renameat(
        directory.descriptor, temporaryName.c_str(),
        directory.descriptor, fileName.c_str()) != 0)
  {
    const std::error_code renameError(errno, std::system_category());
    abandon(std::format(
      "{} '{}' failed to replace: {}", what, LogPath(path), renameError.message()));
  }
#else
  std::filesystem::path temporaryPath = path;
  temporaryPath += ".tmp";

  std::error_code error;

  // Под Windows режима в POSIX-смысле нет — класс конфиденциальности здесь
  // ничего изменить не может (см. `FileSensitivity`), но параметр обязан
  // остаться «использованным», чтобы сборка MSVC не сыпала предупреждениями.
  //
  // ★И АТОМАРНОСТЬ ПОДМЕНЫ ТОЖЕ ОТ ПЛАТФОРМЫ ЗАВИСИТ — я написал в заголовке
  // обратное и был неправ (найдено ревью, итерация 5 прошлого раунда). Под
  // Windows `std::filesystem::rename` сводится к `MoveFileExW`, у которого
  // одновременный читатель может увидеть ОТСУТСТВИЕ файла в момент подмены.
  // Но и там эта ветка НЕ ХУЖЕ той, что заменяет, а лучше: апстрим открывал
  // `ofstream` прямо по живому пути, то есть обрезал файл на всё время записи
  // и оставлял его пустым НАВСЕГДА при любом сбое.
  (void)sensitivity;

  const auto previousStatus = std::filesystem::status(path, error);
  const bool hadPreviousFile = not error
    && std::filesystem::is_regular_file(previousStatus);
  error.clear();

  const auto abandon = [&temporaryPath](const std::string& message)
  {
    std::error_code ignored;
    std::filesystem::remove(temporaryPath, ignored);
    throw std::runtime_error(message);
  };

  std::filesystem::remove(temporaryPath, error);
  error.clear();

  {
    std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
    if (not file.is_open())
    {
      abandon(std::format("{} '{}' not accessible", what, LogPath(path)));
    }

    if (hadPreviousFile)
    {
      std::filesystem::permissions(temporaryPath, previousStatus.permissions(), error);
      if (error)
      {
        abandon(std::format(
          "{} '{}' could not inherit the permissions of the file it replaces: {}",
          what, LogPath(path), error.message()));
      }
    }

    file.write(payload.data(), static_cast<std::streamsize>(payload.size()));

    // ★ПРОВЕРКА ПОСЛЕ ЗАПИСИ ОБЯЗАТЕЛЬНА. Без неё эта функция была бы ХУЖЕ
    // болезни: при переполнении диска недописанный временный файл переехал бы
    // поверх целого, и мы бы САМИ уничтожили запись, которую взялись защищать.
    if (not file.good())
      abandon(std::format("{} '{}' failed to write", what, LogPath(path)));

    // Явное закрытие с проверкой: деструктор потока ошибку сброса ГЛОТАЕТ.
    file.close();
    if (not file.good())
      abandon(std::format("{} '{}' failed to flush", what, LogPath(path)));
  }

  std::filesystem::rename(temporaryPath, path, error);
  if (error)
  {
    abandon(std::format(
      "{} '{}' failed to replace: {}", what, LogPath(path), error.message()));
  }
#endif
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
  // ★ПРАВИМ ДЕСКРИПТОР, А НЕ ПУТЬ (правка ревью, итерация 5). `chmod` по пути
  // ХОДИТ ПО ССЫЛКЕ: если `data/users` подменён ссылкой на `/etc`, прежняя
  // редакция ставила 0700 на `/etc` — то есть правка, взявшаяся сужать права
  // каталога аккаунтов, правила права ЧУЖОГО каталога. Открытие
  // покомпонентно и без перехода по ссылкам даёт `ELOOP` (а не `ENOENT`),
  // поэтому такой каталог читается как ОТКАЗ, о котором вызывающий печатает
  // строку, а не как «его нет».
  detail::DescriptorGuard directory{detail::OpenDirectoryNoSymlinks(path)};
  if (directory.descriptor < 0)
    return errno == ENOENT;                   // нет каталога — не наша забота
  struct ::stat directoryStat{};
  if (::fstat(directory.descriptor, &directoryStat) != 0)
    return false;
  if ((directoryStat.st_mode & 07777) == static_cast<mode_t>(mode))
    return true;                              // уже как надо, syscall не нужен
  return ::fchmod(directory.descriptor, static_cast<mode_t>(mode)) == 0;
#else
  // Под Windows понятия режима в этом смысле нет — ветка пустая осознанно.
  (void)path; (void)mode;
  return true;
#endif
}

//! СПИСОК ОБЫЧНЫХ ФАЙЛОВ КАТАЛОГА, снятый без бросающего продвижения итератора
//! (LOA-fix R73-3b, правка ревью, итерация 2).
//!
//! ★ЗАЧЕМ ОТДЕЛЬНЫЙ ИНСТРУМЕНТ, А НЕ ПРАВКА НА МЕСТЕ. `for (auto& e :
//! directory_iterator(dir, error))` выглядит защищённым, но `error_code` здесь
//! покрывает ТОЛЬКО открытие каталога: range-for продвигает итератор бросающим
//! `operator++`, а `directory_entry::is_regular_file()` без `error_code` бросает
//! тоже. Раунд завёл ТРИ таких обхода; чинить их поштучно значит оставить класс
//! открытым для четвёртого, который напишут через полгода. Поэтому обход ровно
//! один — здесь, — а места вызова получают готовый список и сохраняют свою
//! логику пропусков (`continue`) нетронутой.
//!
//! ★НЕПОЛНОТА — ЭТО ЗНАЧЕНИЕ, А НЕ МОЛЧАНИЕ. Оборванный обход возвращает то же,
//! что и пустой каталог, поэтому «ничего не нашли» обязано отличаться от «не
//! смогли посмотреть» полем, а не интонацией лога.
struct DirectoryListing
{
  //! Каталог, который обходили. ★ЗНАЧЕНИЕ, А НЕ УДОБСТВО: проход сужения
  //! получает готовый список и обязан открывать те же файлы В ТОМ ЖЕ каталоге,
  //! не разбирая путь заново (правка ревью, итерация 5).
  std::filesystem::path directory;
  //! Пути обычных файлов каталога (без каталогов, ссылок и прочего).
  std::vector<std::filesystem::path> files;
  //! ★ОТВЕРГНУТЫЕ СИМВОЛИЧЕСКИЕ ССЫЛКИ — ЗНАЧЕНИЕ, А НЕ МОЛЧАНИЕ (правка ревью,
  //! итерация 4). Прежняя редакция считала ссылку «безобидным ничем» и
  //! пропускала её БЕЗ СЛЕДА, поэтому проход сужения рапортовал полный успех,
  //! индекс не знал об имени, а вход всё это время открывал ЦЕЛЬ ссылки и
  //! принимал её хеш пароля: три потребителя расходились в том, существует
  //! запись или нет. Теперь политика одна для всех — ссылка ОТВЕРГНУТА, — и
  //! ровно потому её обязано быть ВИДНО: «отвергли» и «не нашли» это разные
  //! ответы.
  //!
  //! ★ВИСЯЧАЯ И ЖИВАЯ ССЫЛКИ ЗДЕСЬ НЕ РАЗЛИЧАЮТСЯ, И ЭТО РЕШЕНИЕ. Различить их
  //! можно только сходив ПО ссылке, то есть сделав ровно то, чего политика не
  //! разрешает; а «живая ведёт к настоящему аккаунту» — это утверждение о
  //! ЦЕЛИ, которую в тот же миг можно подменить. Единая политика не имеет окна
  //! проверки-и-действия, разная имела бы.
  std::vector<std::filesystem::path> refusedSymlinks;
  //! Обход не дошёл до конца: список НЕПОЛОН.
  bool incomplete = false;

#ifndef WIN32
  //! ★ДЕСКРИПТОР ОБОЙДЁННОГО КАТАЛОГА, ОСТАЮЩИЙСЯ ОТКРЫТЫМ (правка ревью,
  //! итерация 6).
  //!
  //! Прежде обход закрывал свой дескриптор, а проход сужения РАЗРЕШАЛ ПУТЬ
  //! `listing.directory` заново — то есть «обошли и сузили в одном каталоге»
  //! было утверждением о ИМЕНИ, а не о иноде: между двумя фазами каталог можно
  //! подменить другим настоящим каталогом, и `ELOOP` этого не покажет. Открытый
  //! дескриптор держит ИНОД, поэтому обе фазы адресуют один и тот же каталог по
  //! построению.
  //!
  //! ★ПОЭТОМУ СПИСОК ПЕРЕМЕЩАЕМЫЙ, НО НЕ КОПИРУЕМЫЙ (см. `DescriptorGuard`).
  //! Все потребители получают его как `const auto listing = ListRegularFiles(…)`
  //! — это гарантированное исключение копии, а не копирование.
  detail::DescriptorGuard descriptor;
#endif
};

#ifdef WIN32
namespace detail
{

//! Классификация ОДНОЙ записи каталога для `ListRegularFiles` (ветка Windows).
inline void ClassifyOneDirectoryEntry(
  const std::filesystem::directory_entry& entry,
  DirectoryListing& listing)
{
  std::error_code linkError;
  const bool link = entry.is_symlink(linkError);
  if (linkError)
  {
    listing.incomplete = true;
    return;
  }
  if (link)
  {
    listing.refusedSymlinks.push_back(entry.path());
    return;
  }

  std::error_code kindError;
  const bool regular = entry.is_regular_file(kindError);
  if (kindError)
  {
    listing.incomplete = true;
    return;
  }
  if (not regular)
    return;

  listing.files.push_back(entry.path());
}

} // namespace detail
#endif

[[nodiscard]] inline DirectoryListing ListRegularFiles(
  const std::filesystem::path& directory) noexcept
{
  DirectoryListing listing;
  try
  {
    listing.directory = directory;
  }
  catch (...)
  {
    listing.incomplete = true;
    return listing;
  }

#ifndef WIN32
  // ★ОБХОД ПРИКРЕПЛЁН К ДЕСКРИПТОРУ КАТАЛОГА (правка ревью, итерация 5).
  // `std::filesystem::directory_iterator` разрешает ПУТЬ, то есть ссылка на
  // месте самого каталога (`data/users -> /tmp/theirs`) обходилась молча, и
  // проход сужения, пол uid и оба индекса работали по ЧУЖОМУ дереву.
  // `O_NOFOLLOW` тут не помогал: он про последний компонент. Открываем
  // покомпонентно, дальше `fdopendir`/`fstatat` от того же дескриптора —
  // подменить каталог после открытия нельзя.
  //
  // ★И ЗАОДНО ИСЧЕЗ БРОСАЮЩИЙ `operator++`: `readdir` возвращает код, а не
  // исключение, поэтому «обход оборвался» стало значением по построению.
  const int directoryDescriptor = detail::OpenDirectoryNoSymlinks(directory);
  if (directoryDescriptor < 0)
  {
    listing.incomplete = true;
    if (errno == ELOOP)
    {
      static WarningThrottle directoryLinkThrottle;
      ReportFileWarning(directoryLinkThrottle,
        "Data directory '{}' is reached through a symbolic link and was "
        "refused: symbolic links under the data directory are not managed "
        "records", LogPath(directory));
    }
    return listing;
  }

  // ★ДЕСКРИПТОР ПЕРЕЕЗЖАЕТ В РЕЗУЛЬТАТ, А ОБХОДУ ОТДАЁТСЯ ЕГО ДУБЛИКАТ (правка
  // ревью, итерация 6). `fdopendir` ЗАБИРАЕТ дескриптор себе и закрывает его
  // `closedir`, поэтому отдать ему единственный дескриптор значило бы снова
  // заставить проход сужения разрешать путь заново. `F_DUPFD_CLOEXEC`, а не
  // `dup`: обычный дубликат теряет `O_CLOEXEC` и утёк бы в дочерний процесс.
  listing.descriptor = detail::DescriptorGuard{directoryDescriptor};

  const int streamDescriptor = ::fcntl(directoryDescriptor, F_DUPFD_CLOEXEC, 0);
  if (streamDescriptor < 0)
  {
    listing.incomplete = true;
    return listing;
  }

  DIR* const stream = ::fdopendir(streamDescriptor);
  if (stream == nullptr)
  {
    detail::CloseKeepingErrno(streamDescriptor);
    listing.incomplete = true;
    return listing;
  }

  try
  {
    for (;;)
    {
      // `readdir` отличает конец каталога от ошибки только через `errno`,
      // поэтому его обязано обнулить ПЕРЕД вызовом: иначе «дочитали» и «не
      // смогли» неразличимы — ровно то ложно-зелёное, ради которого заведён
      // `incomplete`.
      errno = 0;
      const struct ::dirent* const entry = ::readdir(stream);
      if (entry == nullptr)
      {
        if (errno != 0)
          listing.incomplete = true;
        break;
      }

      const std::string name(entry->d_name);
      if (name == "." || name == "..")
        continue;

      // ★ТИП БЕРЁТСЯ `fstatat` С `AT_SYMLINK_NOFOLLOW`, А НЕ `d_type`: `d_type`
      // на части файловых систем всегда `DT_UNKNOWN`, и код, который ему верит,
      // молча пропускал бы всё. Флаг `AT_SYMLINK_NOFOLLOW` — это `lstat`, то
      // есть ссылка видна как ссылка, а не как её цель.
      struct ::stat entryStat{};
      if (::fstatat(
            directoryDescriptor, entry->d_name, &entryStat,
            AT_SYMLINK_NOFOLLOW) != 0)
      {
        // ★ОШИБКА КЛАССИФИКАЦИИ — ЭТО НЕПОЛНОТА, А НЕ ПРОПУСК (правка ревью,
        // итерация 3). Молчаливый пропуск возвращал усечённый снимок, помеченный
        // полным: `characters/100.json` не попадал в список, пол uid
        // восстанавливался как 99, и следующий персонаж получал uid 100 — поверх
        // живого файла. «Не смогли посмотреть» обязано отличаться от «не нашли».
        listing.incomplete = true;
        continue;
      }

      if (S_ISLNK(entryStat.st_mode))
      {
        listing.refusedSymlinks.push_back(directory / name);
        continue;
      }
      // Каталог, FIFO, сокет, устройство — не наши файлы, и это ЗНАНИЕ, а не
      // незнание: неполнотой такой пропуск не является.
      if (not S_ISREG(entryStat.st_mode))
        continue;

      listing.files.push_back(directory / name);
    }
  }
  catch (...)
  {
    // `push_back` умеет бросить `bad_alloc`, а функция помечена `noexcept`:
    // без этого перехвата нехватка памяти звала бы `std::terminate`. Честный
    // ответ — «список неполон», а не падение процесса.
    listing.incomplete = true;
  }

  ::closedir(stream);
#else
  std::error_code error;
  if (not std::filesystem::is_directory(directory, error) || error)
  {
    listing.incomplete = true;
    return listing;
  }

  std::filesystem::directory_iterator entry(directory, error);
  if (error)
  {
    listing.incomplete = true;
    return listing;
  }

  const std::filesystem::directory_iterator end;
  try
  {
    while (entry != end)
    {
      detail::ClassifyOneDirectoryEntry(*entry, listing);

      entry.increment(error);
      if (error)
      {
        listing.incomplete = true;
        break;
      }
    }
  }
  catch (...)
  {
    listing.incomplete = true;
  }
#endif

  // ★ЕДИНСТВЕННОЕ МЕСТО, ГДЕ ОБ ОТВЕРГНУТОЙ ССЫЛКЕ ГОВОРЯТ ВСЛУХ. Обход один на
  // весь раунд (пол uid, оба индекса, проход сужения), поэтому и жалоба одна:
  // будь она у потребителей, четвёртый потребитель, написанный через полгода,
  // унаследовал бы молчание. Дроссель — потому что каталог обходится и на
  // сверке индекса, то есть по запросу.
  //
  // ★ИМЯ ЭКРАНИРУЕТСЯ (правка ревью, итерация 5): запись каталога вправе нести
  // перевод строки, и без экранирования ОДНО задросселированное предупреждение
  // печатало бы НЕСКОЛЬКО поддельных записей лога — дроссель считает вызовы, а
  // не строки, которые из них вылезли.
  if (not listing.refusedSymlinks.empty())
  {
    static WarningThrottle symlinkThrottle;
    ReportFileWarning(symlinkThrottle,
      "Data directory '{}': {} entry(ies) are symbolic links and were refused "
      "(first: '{}'); symbolic links are not managed records and are neither "
      "read, indexed nor written through",
      LogPath(directory), listing.refusedSymlinks.size(),
      LogPath(listing.refusedSymlinks.front()));
  }

  return listing;
}

//! ИСХОД ПРИНУЖДЕНИЯ ФАЙЛА К ПОЛИТИКЕ `Secret` (LOA-fix R73-11, ревью 5).
//!
//! ★ТРИ ИСХОДА, А НЕ `bool`, И ЭТО НЕ КОСМЕТИКА. Прежняя редакция возвращала
//! `false` И на «уже owner-only», И на «`fchmod` провалился» — то есть отказ
//! ПРИНУЖДЕНИЯ был неотличим от отсутствия работы. Чтение печатало строку «хеш
//! остаётся открытым» и ТУТ ЖЕ отдавало этот хеш вызывающему, а перестройка
//! индекса заводила такое имя как обычное. Инвариант «ни один байт секрета не
//! используется раньше, чем его инод приведён к политике» держался ровно до
//! первой неудачи. Найдено ревью (итерация 5).
enum class SecretEnforcement
{
  //! Файл уже был доступен только владельцу — работы не потребовалось.
  AlreadySecure,
  //! Режим был шире и СУЖЕН ПРЯМО СЕЙЧАС.
  Narrowed,
  //! Режим шире политики, и сузить его НЕ УДАЛОСЬ. Содержимое такого файла
  //! использовать нельзя.
  Failed,
};

//! Итог сужения режимов у СУЩЕСТВУЮЩИХ файлов с секретом (LOA-fix, R73-2b, #206).
struct SecretFileHardening
{
  //! Сколько обычных файлов удалось осмотреть.
  std::size_t examined = 0;
  //! Сколько из них несли биты group/other и были сужены.
  std::size_t narrowed = 0;
  //! Сколько не удалось ни осмотреть, ни сузить.
  std::size_t failed = 0;
  //! ★ИМЕНА ФАЙЛОВ, КОТОРЫЕ НЕ ДОКАЗАНЫ ПОД ПОЛИТИКОЙ (правка ревью, итерация
  //! 5, расширено итерацией 6). Числа хватало отчёту, но не ВЫЗЫВАЮЩЕМУ:
  //! перестройка индекса обязана не заводить такое имя, а чтение — не отдавать
  //! содержимое. «Сколько» отвечает человеку, «какие» отвечает коду.
  //!
  //! ★СЮДА ПОПАДАЕТ И ТО, ЧЕГО МЫ НЕ СМОТРЕЛИ, А НЕ ТОЛЬКО ТО, ЧТО НЕ СУЗИЛИ
  //! (правка ревью, итерация 6). Запись, перечисленная обходом и ставшая к
  //! моменту открытия ссылкой (`ELOOP`) или исчезнувшая (`ENOENT`), прежде
  //! пропускалась МОЛЧА — и имя из СТАРОГО списка всё равно уходило в индекс,
  //! то есть «непроиндексировано никогда» было неправдой. Список отвечает на
  //! вопрос «какие имена публиковать НЕЛЬЗЯ», а не «какие мы не сумели
  //! починить»: неосмотренное и неисправленное для публикации равны.
  std::vector<std::filesystem::path> unsecured;
  //! Сколько записей каталога отвергнуто как символические ссылки (правка
  //! ревью, итерация 4). ★НЕ `failed`: это не «не смогли», а «не стали» —
  //! осознанный отказ по политике, о котором обязан узнать вызывающий, чтобы
  //! не рапортовать полный проход над файлом, которого он не трогал.
  std::size_t refusedLinks = 0;
  //! ★ОБХОД МОГ ОБОРВАТЬСЯ НА СЕРЕДИНЕ (правка ревью, итерация 2). Без этого
  //! флага прерванный проход возвращает «0 отказов», и вызывающий читает его
  //! как «все файлы осмотрены» — ложно-зелёное ровно того рода, ради которого
  //! этот проход и заведён. Незавершённость — НЕ то же самое, что отказ на
  //! конкретном файле, поэтому это отдельное поле, а не ++failed.
  bool incomplete = false;
};

#ifndef WIN32
namespace detail
{

//! ПРИВОДИТ УЖЕ ОТКРЫТЫЙ ФАЙЛ К ПОЛИТИКЕ `Secret` (LOA-fix R73-6, ревью 4).
//!
//! ★ЗАЧЕМ ОН НУЖЕН ПОМИМО СТАРТОВОГО ПРОХОДА. Проход сужения — это СНИМОК: он
//! приводит в порядок то, что лежало в каталоге на момент старта. Штатный путь
//! завести аккаунт рядом с работающим сервером (помощник переименовывает
//! готовый `Alice.json` в `data/users`) кладёт файл с обычным umask, то есть
//! 0644 — и до следующего перезапуска ИЛИ до следующего сохранения этого
//! аккаунта хеш пароля лежит читаемым для group/other. Вход при этом работает,
//! то есть дефект не проявляет себя ничем.
//!
//! ★ПОЭТОМУ СУЖЕНИЕ ПРИВЯЗАНО К ОТКРЫТИЮ, А НЕ К МОМЕНТУ ВРЕМЕНИ. «Файл,
//! впервые увиденный после старта» невозможно перечислить списком мест; зато
//! можно назвать инвариант: НИ ОДИН байт секрета не используется раньше, чем
//! его инод приведён к политике. Обе двери — чтение (`ReadManagedFile`) и
//! проход сужения (`HardenOneSecretFile`) — зовут ЭТУ функцию, поэтому
//! инвариант тотален по построению, а не по числу исправленных потребителей.
//!
//! ★ПРАВИМ ДЕСКРИПТОР, А НЕ ПУТЬ: между `open` и `chmod` по имени запись можно
//! подменить, и тогда сужен окажется чужой файл. Дескриптор уже открыт с
//! `O_NOFOLLOW` и проверен на `S_ISREG` вызывающим.
//!
//! ★ВЛАДЕЛЕЦ ПРОВЕРЯЕТСЯ НЕЗАВИСИМО ОТ РЕЖИМА (правка ревью, итерация 5).
//! Прежняя редакция уходила ранним `return` на первом же owner-only файле — и
//! тогда восстановленный из копии `root:root 0600`, положенный в каталог
//! `dev:dev`, не усыновлялся НИКОГДА: вход работал, а `set-password.py` и
//! `add-friend.sh` не могли его открыть бессрочно. Хеш при этом не раскрыт, но
//! требование P1 «владелец — владелец каталога» нарушено, а нарушение,
//! спрятанное за ранним выходом, само себя не покажет.
//!
//! @param directoryDescriptor Дескриптор каталога, в котором лежит файл. ★Он же
//!        источник правды о владельце: `fstat` открытого каталога нельзя
//!        подсунуть не тому пути.
inline SecretEnforcement EnsureSecretOnOpen(
  const int descriptor,
  const int directoryDescriptor,
  const std::filesystem::path& path,
  const struct ::stat& fileStat) noexcept
{
  // ★ВЛАДЕЛЕЦ ПЕРВЫМ, РЕЖИМ ВТОРЫМ — тот же порядок и по той же причине, что в
  // `WriteFileAtomically`: смена владельца гасит setuid/setgid, обратный
  // порядок терял бы часть только что поставленного режима. Усыновление
  // владельца best-effort ровно как там: несущая гарантия — режим.
  struct ::stat directoryStat{};
  if (::fstat(directoryDescriptor, &directoryStat) == 0
    && (fileStat.st_uid != directoryStat.st_uid
      || fileStat.st_gid != directoryStat.st_gid))
  {
    if (::fchown(descriptor, directoryStat.st_uid, directoryStat.st_gid) != 0)
    {
      static WarningThrottle ownershipThrottle;
      ReportFileWarning(ownershipThrottle,
        "Account file '{}': could not adopt the owner {}:{} of its directory "
        "(errno {}); narrowing its mode regardless",
        LogPath(path),
        static_cast<unsigned>(directoryStat.st_uid),
        static_cast<unsigned>(directoryStat.st_gid),
        errno);
    }
  }

  if ((fileStat.st_mode & (S_IRWXG | S_IRWXO)) == 0)
    return SecretEnforcement::AlreadySecure;

  // Та же арифметика сужения, что у `Secret` в `WriteFileAtomically`: маска
  // снимает group/other, пол оставляет владельцу чтение и запись.
  mode_t narrowed = static_cast<mode_t>(fileStat.st_mode & 07777);
  narrowed &= ~static_cast<mode_t>(S_IRWXG | S_IRWXO);
  narrowed |= static_cast<mode_t>(S_IRUSR | S_IWUSR);

  if (::fchmod(descriptor, narrowed) != 0)
  {
    static WarningThrottle failureThrottle;
    ReportFileWarning(failureThrottle,
      "Account file '{}' arrived with mode {:o} readable beyond its owner and "
      "could not be narrowed (errno {}); its contents are refused",
      LogPath(path), static_cast<unsigned>(fileStat.st_mode & 07777), errno);
    return SecretEnforcement::Failed;
  }

  static WarningThrottle narrowedThrottle;
  ReportFileWarning(narrowedThrottle,
    "Account file '{}' arrived with mode {:o} readable beyond its owner and was "
    "narrowed to {:o} before its contents were used",
    LogPath(path), static_cast<unsigned>(fileStat.st_mode & 07777),
    static_cast<unsigned>(narrowed));
  return SecretEnforcement::Narrowed;
}

//! Сужает права ОДНОГО файла каталога аккаунтов. Вынесено из
//! `HardenSecretFilesInDirectory` затем, чтобы пропуск записи был `return`, а не
//! `continue`: в цикле с ЯВНЫМ продвижением `continue` перепрыгнул бы
//! продвижение итератора и подвесил старт навсегда.
//!
//! ★ОТКРЫВАЕТ ОТ ДЕСКРИПТОРА КАТАЛОГА (правка ревью, итерация 5) и зовёт ТОТ ЖЕ
//! `EnsureSecretOnOpen`, что и чтение: собственная копия арифметики сужения
//! здесь и была тем, из-за чего «отказ» считался в одном месте и терялся в
//! другом.
inline void HardenOneSecretFile(
  const int directoryDescriptor,
  const std::filesystem::path& file,
  SecretFileHardening& result) noexcept
{
  // ★ЛЮБОЙ ВЫХОД, КРОМЕ «ФАЙЛ ПОД ПОЛИТИКОЙ», ОБЪЯВЛЯЕТ ИМЯ НЕПУБЛИКУЕМЫМ
  // (правка ревью, итерация 6). Иначе запись, перечисленная обходом и ставшая к
  // моменту открытия ссылкой, тихо пропускалась — а её имя из старого списка
  // всё равно попадало в индекс.
  const auto refuseToPublish = [&result](const std::filesystem::path& path)
  {
    try
    {
      result.unsecured.push_back(path);
    }
    catch (...)
    {
      // Имя не удалось запомнить — вызывающий не сможет его обойти стороной.
      result.incomplete = true;
    }
  };

  std::string name;
  try
  {
    name = file.filename().string();
  }
  catch (...)
  {
    result.incomplete = true;
    refuseToPublish(file);
    return;
  }

  // ★`O_NONBLOCK`, И ЭТО НЕ МИКРООПТИМИЗАЦИЯ (правка ревью, итерация 3). Без
  // него `open(O_RDONLY)` на ИМЕНОВАННОМ КАНАЛЕ ждёт писателя — то есть один
  // `mkfifo data/users/x.json` вешает старт сервера НАВСЕГДА, без строки в
  // логе. Список выше уже отсеял всё, что не обычный файл, но между обходом и
  // открытием запись могли подменить, поэтому защита стоит и здесь: обход
  // отвечает за норму, флаг — за гонку.
  const int descriptor = ::openat(
    directoryDescriptor, name.c_str(),
    O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (descriptor < 0)
  {
    // ★НЕ ВСЯКИЙ ОТКАЗ ОТКРЫТИЯ — ОТКАЗ СУЖЕНИЯ (правка ревью, итерация 3).
    // Эти errno означают «это не обычный файл» или «его уже нет», то есть
    // запись, которую политика и так велит ПРОПУСТИТЬ. Считать их отказами
    // значило бы отказать в старте из-за сокета, положенного в каталог, —
    // отказ обслуживать по причине, к секрету отношения не имеющей.
    //   ELOOP        — стала символической ссылкой;
    //   ENOENT       — исчезла между обходом и открытием;
    //   ENXIO/ENODEV — сокет либо устройство без драйвера.
    // Всё остальное (EACCES, EPERM, EMFILE, EIO) — настоящий отказ.
    if (errno != ELOOP && errno != ENOENT && errno != ENXIO && errno != ENODEV)
      ++result.failed;
    // ★НО ПУБЛИКОВАТЬ ЭТО ИМЯ НЕЛЬЗЯ В ЛЮБОМ СЛУЧАЕ (правка ревью, итерация 6):
    // «не стали смотреть» и «не смогли смотреть» одинаково означают, что о
    // режиме этого инода мы не знаем ничего.
    refuseToPublish(file);
    return;
  }

  struct ::stat fileStat{};
  if (::fstat(descriptor, &fileStat) != 0)
  {
    ++result.failed;
    refuseToPublish(file);
    ::close(descriptor);
    return;
  }
  if (not S_ISREG(fileStat.st_mode))
  {
    // Обход отдавал обычный файл, а открылось не оно: запись подменили между
    // перечислением и открытием. Публиковать нечего.
    refuseToPublish(file);
    ::close(descriptor);
    return;
  }

  ++result.examined;
  switch (EnsureSecretOnOpen(descriptor, directoryDescriptor, file, fileStat))
  {
    case SecretEnforcement::Narrowed:
      ++result.narrowed;
      break;
    case SecretEnforcement::Failed:
      ++result.failed;
      refuseToPublish(file);
      break;
    case SecretEnforcement::AlreadySecure:
      break;
  }
  ::close(descriptor);
}

//! УБОРКА ОДНОГО КАТАЛОГА ПО ДЕСКРИПТОРУ, РЕКУРСИВНО (правка ревью, итерация 6).
//!
//! ★ЗАБИРАЕТ ДЕСКРИПТОР СЕБЕ и закрывает его сам (через `closedir` либо
//! напрямую): владение одно, поэтому «кто закрывает» не зависит от того, каким
//! путём функция вышла.
inline void SweepDirectoryDescriptor(
  const int directoryDescriptor, const int depthLeft) noexcept
{
  DIR* const stream = ::fdopendir(directoryDescriptor);
  if (stream == nullptr)
  {
    CloseKeepingErrno(directoryDescriptor);
    return;
  }

  for (;;)
  {
    errno = 0;
    const struct ::dirent* const entry = ::readdir(stream);
    if (entry == nullptr)
      break;   // конец каталога либо ошибка: уборка молчит и в том, и в другом

    const std::string_view name(entry->d_name);
    if (name == "." || name == "..")
      continue;

    // Тип берётся `fstatat` без перехода по ссылке: `d_type` на части файловых
    // систем всегда `DT_UNKNOWN`.
    struct ::stat entryStat{};
    if (::fstatat(
          directoryDescriptor, entry->d_name, &entryStat,
          AT_SYMLINK_NOFOLLOW) != 0)
    {
      continue;
    }

    if (S_ISDIR(entryStat.st_mode))
    {
      if (depthLeft <= 0)
        continue;
      const int child = ::openat(
        directoryDescriptor, entry->d_name,
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (child < 0)
        continue;
      SweepDirectoryDescriptor(child, depthLeft - 1);
      continue;
    }

    if (not S_ISREG(entryStat.st_mode))
      continue;
    // Тот же фильтр, что у прежнего сравнения расширения с «.tmp»: имя РОВНО
    // `.tmp` расширения не имеет и уборке не подлежит.
    if (name.size() <= 4 || name.substr(name.size() - 4) != ".tmp")
      continue;

    ::unlinkat(directoryDescriptor, entry->d_name, 0);
  }

  ::closedir(stream);
}

} // namespace detail
#endif

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
//! ★БЕЗ ПЕРЕХОДА ПО ССЫЛКАМ НА ВСЮ ГЛУБИНУ ПУТИ, А НЕ `chmod` ПО ИМЕНИ.
//! Символическая ссылка в каталоге аккаунтов увела бы `chmod` на чужой файл, а
//! ссылка на месте САМОГО каталога увела бы весь проход в чужое дерево.
//! Каталог открывается покомпонентно один раз, файлы — `openat` от него.
//!
//! ★НЕ ЛОГИРУЕТ САМ (та же причина, что у `EnsureDirectoryMode`) — возвращает
//! числа, а строку печатает вызывающий.
[[nodiscard]] inline SecretFileHardening HardenSecretFiles(
  const DirectoryListing& listing) noexcept
{
  SecretFileHardening result;
#ifndef WIN32
  result.incomplete = listing.incomplete;
  result.refusedLinks = listing.refusedSymlinks.size();

  if (listing.files.empty())
    return result;

  // ★ТОТ ЖЕ ДЕСКРИПТОР, ЧТО У ОБХОДА, А НЕ ПОВТОРНОЕ РАЗРЕШЕНИЕ ПУТИ (правка
  // ревью, итерация 6). Прежде здесь стоял свой `OpenDirectoryNoSymlinks`, то
  // есть между перечислением и сужением каталог можно было подменить ДРУГИМ
  // НАСТОЯЩИМ каталогом: ссылок нет, `ELOOP` не сработает, а сужали бы мы уже не
  // те иноды, которые перечислили. Дескриптор держит инод — окна не остаётся.
  if (listing.descriptor.descriptor < 0)
  {
    // Обход отдал файлы, но не отдал дескриптор: смотреть нечем. «Мы не
    // смотрели» — это неполнота, а не чистый проход, и ни одно из этих имён
    // публиковать нельзя.
    result.incomplete = true;
    for (const auto& file : listing.files)
    {
      try
      {
        result.unsecured.push_back(file);
      }
      catch (...)
      {
        result.incomplete = true;
        break;
      }
    }
    return result;
  }

  for (const auto& file : listing.files)
    detail::HardenOneSecretFile(listing.descriptor.descriptor, file, result);
#else
  (void)listing;
#endif
  return result;
}

//! ★ПРИНИМАЕТ ГОТОВЫЙ СПИСОК ОТДЕЛЬНОЙ ПЕРЕГРУЗКОЙ (правка ревью, итерация 4).
//! Перестройка индекса аккаунтов уже обходит `data/users` — ей нужно сузить
//! режимы ТЕХ ЖЕ файлов, которые она собирается проиндексировать, и второй
//! независимый обход дал бы окно между «сузили одно» и «проиндексировали
//! другое». Один список, два потребителя, ни одного окна.
[[nodiscard]] inline SecretFileHardening HardenSecretFilesInDirectory(
  const std::filesystem::path& directory) noexcept
{
#ifndef WIN32
  // ★ОБХОД ОДИН НА ВЕСЬ РАУНД (правка ревью, итерация 3). Собственный цикл
  // здесь ОТКРЫВАЛ каждую запись каталога, чтобы через `fstat` узнать, обычный
  // ли это файл, — то есть FIFO вешал старт, а сокет давал `++failed` и, после
  // правки «отказ сужения останавливает старт», ЛОЖНЫЙ отказ в обслуживании из-
  // за записи, которую политика велит пропустить. Классификация принадлежит
  // обходу, а не потребителю: `ListRegularFiles` отдаёт только обычные файлы и
  // честно сообщает о неполноте. Тот же обход теперь у пола uid и у обоих
  // индексов — класс закрыт целиком, а не в трёх местах по отдельности.
  return HardenSecretFiles(ListRegularFiles(directory));
#else
  (void)directory;
  return SecretFileHardening{};
#endif
}

//! ИСХОД ЧТЕНИЯ УПРАВЛЯЕМОГО ФАЙЛА ДАННЫХ (LOA-fix R73-6, ревью 4).
enum class ManagedReadStatus
{
  //! Прочитан обычный файл; `content` — его содержимое.
  Ok,
  //! Файла нет (`ENOENT`). Для аутентификации это «новое имя», а не отказ.
  Missing,
  //! Запись существует, но управляемой записью НЕ ЯВЛЯЕТСЯ: символическая
  //! ссылка, каталог, канал, сокет, устройство. ★Отдельный исход от `Failed`
  //! намеренно: «мы не стали» и «мы не смогли» требуют разных решений у
  //! вызывающего и разных строк у оператора.
  Refused,
  //! Прочитать не удалось (права, ввод-вывод, нехватка памяти).
  Failed,
};

//! Результат чтения вместе с тем, что по дороге пришлось починить.
struct ManagedFileRead
{
  ManagedReadStatus status = ManagedReadStatus::Failed;
  std::string content;
  //! Режим файла был расширен и сужен ПРЯМО СЕЙЧАС — до того, как содержимое
  //! стало доступно вызывающему.
  bool narrowed = false;
};

//! ЕДИНСТВЕННЫЙ ВХОД В ЧТЕНИЕ ФАЙЛА ПОД `data/` (LOA-fix R73-6, ревью 4).
//!
//! ★ЗАЧЕМ ОН ЗАВЁЛСЯ. `std::ifstream file(path)` ХОДИТ ПО ССЫЛКЕ и открывает
//! каналы. До этой правки девятнадцать чтений в `FileDataSource` и вход в
//! `LocalAuthenticationBackend` были написаны именно так, а обход каталога
//! ссылку молча пропускал — то есть три потребителя расходились в ответе на
//! вопрос «существует ли запись `Alice`»: проход сужения её не видел, индекс не
//! знал имени, а вход открывал ЦЕЛЬ ссылки и принимал её хеш пароля. Чинить это
//! в двадцати местах значило бы оставить класс открытым для двадцать первого.
//!
//! ★ЧТО ОН ГАРАНТИРУЕТ, ОДНОЙ ФРАЗОЙ: содержимое отдаётся ТОЛЬКО из обычного
//! файла, открытого без перехода по ссылке, и для класса `Secret` — только
//! после того, как режим этого инода приведён к политике (см.
//! `detail::EnsureSecretOnOpen`).
//!
//! ★`O_NONBLOCK` — по той же причине, что в проходе сужения: `open(O_RDONLY)` у
//! именованного канала ждёт писателя, то есть один `mkfifo data/users/x.json`
//! подвесил бы вход НАВСЕГДА и без строки в логе.
//!
//! ★И ЧЕГО ОН НЕ ОТДАЁТ. Файл класса `Secret`, чей режим сузить НЕ УДАЛОСЬ,
//! возвращается как `Failed` БЕЗ содержимого: «мы сказали в лог» — это не
//! политика. Файл длиннее `kMaxManagedRecordBytes` возвращается как `Refused`,
//! не будучи прочитанным: общий читатель складывает файл в память ДО разбора, и
//! без потолка одна попытка входа именем, под которым лежит гигабайтный файл,
//! стоила бы контейнеру всей памяти (обе правки — ревью, итерация 5).
//!
//! ★ПОРЯДОК ЭТИХ ДВУХ ОТКАЗОВ — СВОЙСТВО, А НЕ СЛУЧАЙНОСТЬ (правка ревью,
//! итерация 6): принуждение секрета идёт ПЕРВЫМ, потолок размера вторым. Отказ
//! в обслуживании не освобождает от обязанности привести инод к политике, иначе
//! «слишком большой» становится способом оставить хеш пароля открытым.
//!
//! ★НЕ БРОСАЕТ. Политику отказа выбирает вызывающий: `FileDataSource` бросает
//! своим прежним текстом (это маркеры лесенки прошлых раундов), вход отвечает
//! `false` (fail-closed), обходы пропускают запись.
[[nodiscard]] inline ManagedFileRead ReadManagedFile(
  const std::filesystem::path& path,
  const FileSensitivity sensitivity) noexcept
{
  ManagedFileRead result;

#ifndef WIN32
  std::string fileName;
  try
  {
    fileName = path.filename().string();
  }
  catch (...)
  {
    result.status = ManagedReadStatus::Failed;
    return result;
  }
  if (fileName.empty() || fileName == "." || fileName == "..")
  {
    result.status = ManagedReadStatus::Refused;
    return result;
  }

  // ★КАТАЛОГ РАЗБИРАЕТСЯ ПОКОМПОНЕНТНО И БЕЗ ПЕРЕХОДА ПО ССЫЛКАМ (правка ревью,
  // итерация 5). `O_NOFOLLOW` защищает только ПОСЛЕДНИЙ компонент, поэтому
  // `data/users -> /tmp/theirs` прежде проходился молча и вход принимал хеш
  // пароля из чужого дерева. Теперь ссылка ЛЮБОГО уровня — `ELOOP`.
  detail::DescriptorGuard directory{
    detail::OpenDirectoryNoSymlinks(path.parent_path())};
  if (directory.descriptor < 0)
  {
    if (errno == ENOENT)
    {
      result.status = ManagedReadStatus::Missing;
    }
    else if (errno == ELOOP || errno == ENOTDIR || errno == EINVAL)
    {
      result.status = ManagedReadStatus::Refused;
      static WarningThrottle directoryLinkThrottle;
      ReportFileWarning(directoryLinkThrottle,
        "Data file '{}' is reached through a symbolic link or a non-directory "
        "and was refused: paths under the data directory are resolved without "
        "following links", LogPath(path));
    }
    else
    {
      result.status = ManagedReadStatus::Failed;
    }
    return result;
  }

  const int descriptor = ::openat(
    directory.descriptor, fileName.c_str(),
    O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (descriptor < 0)
  {
    if (errno == ENOENT)
    {
      result.status = ManagedReadStatus::Missing;
    }
    else if (errno == ELOOP)
    {
      // ★ИМЕННО ЗДЕСЬ ССЫЛКА И ОТВЕРГАЕТСЯ. `O_NOFOLLOW` превращает «пошли бы
      // по ссылке» в `ELOOP`, поэтому отказ — свойство системного вызова, а не
      // проверка, которую можно обойти подменой между `lstat` и `open`.
      result.status = ManagedReadStatus::Refused;
      static WarningThrottle symlinkThrottle;
      ReportFileWarning(symlinkThrottle,
        "Data file '{}' is a symbolic link and was refused: symbolic links "
        "under the data directory are not managed records and are never read "
        "through", LogPath(path));
    }
    else if (errno == ENXIO || errno == ENODEV)
    {
      // Сокет либо устройство без драйвера — не наша запись.
      result.status = ManagedReadStatus::Refused;
    }
    else
    {
      result.status = ManagedReadStatus::Failed;
    }
    return result;
  }

  struct ::stat fileStat{};
  if (::fstat(descriptor, &fileStat) != 0)
  {
    ::close(descriptor);
    result.status = ManagedReadStatus::Failed;
    return result;
  }
  if (not S_ISREG(fileStat.st_mode))
  {
    // Каталог, канал, устройство: `O_NONBLOCK` дал открыться, но читать это
    // содержимым записи нельзя.
    ::close(descriptor);
    result.status = ManagedReadStatus::Refused;
    return result;
  }

  // ★СУЖЕНИЕ ПЕРЕД ЛЮБЫМ ДРУГИМ РЕШЕНИЕМ, А НЕ ТОЛЬКО ПЕРЕД ЧТЕНИЕМ (правка
  // ревью, итерация 6). Итерация 5 поставила потолок размера ВЫШЕ этого блока, и
  // тем самым завела путь, на котором файл аккаунта отвергается, НИ РАЗУ не
  // пройдя принуждение: положить рядом с работающим сервером `Alice.json` с
  // режимом 0644 и добить его нулями за 4 МиБ — и вход честно отказывает, а хеш
  // пароля остаётся читаемым для group/other до перезапуска или до перестройки
  // индекса, то есть неограниченно долго. Отказ обслуживать не отменяет
  // обязанности привести инод к политике: принуждение не читает НИ ОДНОГО байта
  // содержимого, поэтому ставить его первым ничего не стоит.
  //
  // ★СУЖЕНИЕ ДО ЧТЕНИЯ, А НЕ ПОСЛЕ. Если бы оно стояло после, между выдачей
  // содержимого и правкой режима существовало бы окно, в котором сервер уже
  // ПОЛЬЗУЕТСЯ секретом из файла, доступного посторонним.
  //
  // ★И ОТКАЗ СУЖЕНИЯ ЗАКРЫВАЕТ ЧТЕНИЕ (правка ревью, итерация 5). Прежде
  // `fchmod`, вернувший `EPERM`, давал ту же `false`, что и «уже узкий»: строка
  // «хеш остаётся открытым» уходила в лог, а хеш — вызывающему, который его
  // принимал. Отказ принуждения обязан быть отказом ЧТЕНИЯ, иначе политика
  // существует только в журнале.
  if (sensitivity == FileSensitivity::Secret)
  {
    const auto enforcement = detail::EnsureSecretOnOpen(
      descriptor, directory.descriptor, path, fileStat);
    if (enforcement == SecretEnforcement::Failed)
    {
      ::close(descriptor);
      result.status = ManagedReadStatus::Failed;
      return result;   // ни одного байта содержимого
    }
    result.narrowed = enforcement == SecretEnforcement::Narrowed;
  }

  // ★ПОТОЛОК РАЗМЕРА ПРОВЕРЯЕТСЯ ДО ЧТЕНИЯ И ЕЩЁ РАЗ ВО ВРЕМЯ (правка ревью,
  // итерация 5). Один `st_size` не гарантия: разрежённый файл вправе вырасти
  // между `fstat` и последним `read`, а «прочитали 4 ГиБ, потому что в момент
  // проверки было 4 КиБ» — тот же исчерпанный контейнер. Проверка до чтения
  // делает отказ дешёвым, проверка во время делает его ВЕРНЫМ.
  if (fileStat.st_size < 0
    || static_cast<std::uintmax_t>(fileStat.st_size) > kMaxManagedRecordBytes)
  {
    ::close(descriptor);
    result.status = ManagedReadStatus::Refused;
    static WarningThrottle oversizeThrottle;
    ReportFileWarning(oversizeThrottle,
      "Data file '{}' is {} bytes, beyond the {} byte ceiling for a single "
      "record, and was refused without being read",
      LogPath(path), static_cast<std::uintmax_t>(fileStat.st_size < 0
        ? 0 : fileStat.st_size),
      kMaxManagedRecordBytes);
    return result;
  }

  try
  {
    std::string content;
    // Резерв ограничен потолком, а не тем, что сказал `st_size`.
    content.reserve(static_cast<std::size_t>(fileStat.st_size));
    std::vector<char> buffer(64 * 1024);
    for (;;)
    {
      const ssize_t chunk = ::read(descriptor, buffer.data(), buffer.size());
      if (chunk < 0)
      {
        if (errno == EINTR)
          continue;   // прерывание сигналом — не ошибка
        ::close(descriptor);
        result.status = ManagedReadStatus::Failed;
        return result;
      }
      if (chunk == 0)
        break;
      if (content.size() + static_cast<std::size_t>(chunk)
        > static_cast<std::size_t>(kMaxManagedRecordBytes))
      {
        ::close(descriptor);
        result.status = ManagedReadStatus::Refused;
        result.content.clear();
        static WarningThrottle grewThrottle;
        ReportFileWarning(grewThrottle,
          "Data file '{}' grew past the {} byte ceiling for a single record "
          "while being read and was refused",
          LogPath(path), kMaxManagedRecordBytes);
        return result;
      }
      content.append(buffer.data(), static_cast<std::size_t>(chunk));
    }
    result.content = std::move(content);
  }
  catch (...)
  {
    // Функция `noexcept`: нехватка памяти обязана стать ответом, а не падением.
    ::close(descriptor);
    result.status = ManagedReadStatus::Failed;
    result.content.clear();
    return result;
  }

  ::close(descriptor);
  result.status = ManagedReadStatus::Ok;
#else
  // ★Под Windows ни `O_NOFOLLOW`, ни режима в POSIX-смысле нет; ссылка
  // отвергается проверкой по пути, а класс конфиденциальности здесь не может
  // изменить ничего (см. `FileSensitivity`).
  (void)sensitivity;
  try
  {
    std::error_code error;
    if (std::filesystem::is_symlink(path, error) && not error)
    {
      result.status = ManagedReadStatus::Refused;
      return result;
    }
    if (not std::filesystem::exists(path, error) || error)
    {
      result.status = ManagedReadStatus::Missing;
      return result;
    }
    // Тот же потолок записи, что и под POSIX: одна запись не имеет права
    // занять память, которой у процесса нет.
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaxManagedRecordBytes)
    {
      result.status = ManagedReadStatus::Refused;
      return result;
    }
    std::ifstream file(path, std::ios::binary);
    if (not file.is_open())
    {
      result.status = ManagedReadStatus::Failed;
      return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (not file.good() && not file.eof())
    {
      result.status = ManagedReadStatus::Failed;
      return result;
    }
    result.content = buffer.str();
    result.status = ManagedReadStatus::Ok;
  }
  catch (...)
  {
    result.status = ManagedReadStatus::Failed;
    result.content.clear();
  }
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
#ifndef WIN32
  // ★ОБХОД ПРИКРЕПЛЁН К ДЕСКРИПТОРАМ, А НЕ К ПУТЯМ (правка ревью, итерация 6).
  // `recursive_directory_iterator` по умолчанию в ссылки не заходит, но
  // `std::filesystem::remove(entry.path())` разрешает ПУТЬ целиком заново: ссылка
  // на месте промежуточного каталога (`data/characters/equipment ->
  // /tmp/theirs`) превращала уборку своего мусора в удаление ЧУЖИХ файлов,
  // подходящих под `*.tmp`. Здесь каждый шаг — `openat(O_DIRECTORY|O_NOFOLLOW)`
  // от дескриптора предыдущего, а удаление — `unlinkat` от каталога, в котором
  // запись и была перечислена.
  const int descriptor = detail::OpenDirectoryNoSymlinks(root);
  if (descriptor < 0)
    return;   // уборка — не повод не стартовать
  // Глубина ограничена: дерево данных двухуровневое (`characters/equipment/items`),
  // потолок 32 недостижим честными данными и делает рекурсию заведомо конечной.
  detail::SweepDirectoryDescriptor(descriptor, 32);
#else
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
#endif
}

} // namespace server::util

#endif // ATOMIC_FILE_HPP
