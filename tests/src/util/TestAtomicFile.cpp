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

// См. пояснение в TestNameGuard.cpp: `assert` не имеет права исчезнуть.
#undef NDEBUG

#include <libserver/util/AtomicFile.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32

  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/un.h>
  #include <unistd.h>

namespace
{

using server::util::FileSensitivity;

//! Снимает замки с остатков прошлой песочницы.
//!
//! ★НУЖНО ПОТОМУ, ЧТО ТЕСТ САМ СТАВИТ КАТАЛОГ В 0000 (проверка `EACCES` у
//! `EnsureDirectoryMode`). Упавший на `assert` прогон обрывается ДО снятия
//! замка, и следующий запуск не смог бы даже убрать за собой: тест стал бы
//! красным навсегда и по причине, к проверяемому коду отношения не имеющей.
//! Ложно-красный не безопаснее ложно-зелёного — цена у него та же.
void UnlockTree(const std::filesystem::path& root)
{
  std::error_code error;
  if (not std::filesystem::exists(root, error) || error)
    return;
  ::chmod(root.c_str(), 0700);

  std::filesystem::recursive_directory_iterator entry(
    root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  for (; not error && entry != end; entry.increment(error))
  {
    std::error_code kind;
    // Каталог правится ДО спуска в него, иначе обход в него не войдёт.
    if (entry->is_directory(kind) && not kind)
      ::chmod(entry->path().c_str(), 0700);
  }
}

//! Каталог-песочница одного прогона.
std::filesystem::path MakeSandbox()
{
  const auto sandbox = std::filesystem::temp_directory_path()
    / std::filesystem::path("alicia-atomicfile-test");
  UnlockTree(sandbox);
  std::error_code ignored;
  std::filesystem::remove_all(sandbox, ignored);
  std::filesystem::create_directories(sandbox);
  return sandbox;
}

mode_t ModeOf(const std::filesystem::path& path)
{
  struct ::stat status{};
  const int result = ::stat(path.c_str(), &status);
  assert(result == 0);
  return static_cast<mode_t>(status.st_mode & 07777);
}

std::string ReadBack(const std::filesystem::path& path)
{
  std::ifstream file(path);
  assert(file.is_open());
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

void SeedFile(const std::filesystem::path& path, const mode_t mode)
{
  {
    std::ofstream file(path);
    assert(file.is_open());
    file << "старое содержимое";
  }
  const int result = ::chmod(path.c_str(), mode);
  assert(result == 0);
  assert(ModeOf(path) == mode);
}

constexpr std::string_view kPayload = "{\n  \"passwordHash\": \"секрет\"\n}";

void TestNewSecretFileIsPrivate(const std::filesystem::path& sandbox)
{
  const auto path = sandbox / "new-secret.json";
  server::util::WriteFileAtomically(path, kPayload, "User file",
    FileSensitivity::Secret);
  assert(ModeOf(path) == 0600);
  assert(ReadBack(path) == kPayload);
}

void TestNewPublicFileKeepsUmaskBehaviour(const std::filesystem::path& sandbox)
{
  // ★UMASK СЧИТАЕТСЯ, А НЕ ЗАШИВАЕТСЯ. Жёсткое `0644` сделало бы тест
  // зависимым от окружения прогонщика, то есть ложно-красным или
  // ложно-зелёным по причине, к раунду отношения не имеющей.
  const mode_t currentUmask = ::umask(0);
  ::umask(currentUmask);

  const auto path = sandbox / "new-public.json";
  server::util::WriteFileAtomically(path, kPayload, "Character file",
    FileSensitivity::Public);
  assert(ModeOf(path) == static_cast<mode_t>(0666 & ~currentUmask));
  assert(ReadBack(path) == kPayload);
}

void TestInheritedModesAreNarrowedForSecrets(const std::filesystem::path& sandbox)
{
  // 12 из 13 живых аккаунтов прода лежат 0644 …
  const auto inherited644 = sandbox / "inherited-644.json";
  SeedFile(inherited644, 0644);
  server::util::WriteFileAtomically(inherited644, kPayload, "User file",
    FileSensitivity::Secret);
  assert(ModeOf(inherited644) == 0600);
  assert(ReadBack(inherited644) == kPayload);

  // … а `nmax.json` — 0664. Фикстура сидит РЕАЛЬНЫМИ значениями прода.
  const auto inherited664 = sandbox / "inherited-664.json";
  SeedFile(inherited664, 0664);
  server::util::WriteFileAtomically(inherited664, kPayload, "User file",
    FileSensitivity::Secret);
  assert(ModeOf(inherited664) == 0600);
  assert(ReadBack(inherited664) == kPayload);
}

void TestPublicInheritanceIsUntouched(const std::filesystem::path& sandbox)
{
  // I8: у публичного файла поведение раунда обязано быть прежним до бита.
  const auto path = sandbox / "inherited-640.json";
  SeedFile(path, 0640);
  server::util::WriteFileAtomically(path, kPayload, "Character file",
    FileSensitivity::Public);
  assert(ModeOf(path) == 0640);
  assert(ReadBack(path) == kPayload);
}

void TestOwnerFloor(const std::filesystem::path& sandbox)
{
  // ★ЗАЩИТА ДАННЫХ НЕ ИМЕЕТ ПРАВА ОБЕРНУТЬСЯ ИХ ПОТЕРЕЙ: файл с режимом 0000
  // после сужения обязан остаться читаемым для самого сервера.
  const auto path = sandbox / "inherited-000.json";
  SeedFile(path, 0000);
  server::util::WriteFileAtomically(path, kPayload, "User file",
    FileSensitivity::Secret);
  assert(ModeOf(path) == 0600);
  assert(ReadBack(path) == kPayload);
}

void TestHostileUmask(const std::filesystem::path& sandbox)
{
  // ★ЕДИНСТВЕННОЕ МЕСТО, ГДЕ I2 ВООБЩЕ ПРОВЕРЯЕМ. На стенде umask контейнера
  // 0022, и безусловный `fchmod` для секрета там провалиться не умеет; здесь
  // umask враждебный (0277), и без `fchmod` владелец потерял бы запись.
  const mode_t previousUmask = ::umask(0277);
  const auto path = sandbox / "hostile-umask.json";
  server::util::WriteFileAtomically(path, kPayload, "User file",
    FileSensitivity::Secret);
  const mode_t observed = ModeOf(path);
  ::umask(previousUmask);
  assert(observed == 0600);
  assert(ReadBack(path) == kPayload);
}

void TestEnsureDirectoryMode(const std::filesystem::path& sandbox)
{
  const auto directory = sandbox / "users";
  std::filesystem::create_directories(directory);
  const int seeded = ::chmod(directory.c_str(), 0755);
  assert(seeded == 0);
  assert(ModeOf(directory) == 0755);

  // Существующий каталог сужается — именно этого `create_directories` не делает.
  assert(server::util::EnsureDirectoryMode(directory, 0700));
  assert(ModeOf(directory) == 0700);

  // Повторный вызов ничего не меняет и не считается ошибкой.
  assert(server::util::EnsureDirectoryMode(directory, 0700));
  assert(ModeOf(directory) == 0700);

  // Отсутствующий каталог — не ошибка прав и не бросок.
  assert(server::util::EnsureDirectoryMode(sandbox / "no-such-directory", 0700));

  // ★ОТКАЗ `stat`, НЕ РАВНЫЙ «ЕГО НЕТ», ОБЯЗАН ЧИТАТЬСЯ КАК ПРОВАЛ (правка
  // ревью, итерация 1). Прежняя редакция отвечала `true` на любой отказ, и
  // режим каталога с аккаунтами оставался неизвестным БЕЗ единой строки в
  // логе. Улику ставим руками: родительский каталог без бита `x` делает
  // `stat` по вложенному пути `EACCES`, а не `ENOENT`.
  const auto closed = sandbox / "closed";
  const auto hidden = closed / "users";
  std::filesystem::create_directories(hidden);
  const int shut = ::chmod(closed.c_str(), 0000);
  assert(shut == 0);
  if (::geteuid() != 0)   // под root биты прав не действуют — проверка была бы слепой
  {
    assert(not server::util::EnsureDirectoryMode(hidden, 0700));
  }
  const int reopened = ::chmod(closed.c_str(), 0700);
  assert(reopened == 0);
}

void TestHardenSecretFilesInDirectory(const std::filesystem::path& sandbox)
{
  // ★АККАУНТ, В КОТОРЫЙ СЕРВЕР НИКОГДА НЕ НАПИШЕТ, ТОЖЕ ОБЯЗАН СУЗИТЬСЯ
  // (правка ревью, итерация 1): `FileSensitivity` чинит режим на ПЕРВОЙ записи,
  // а спящий аккаунт первой записи не дождётся.
  const auto directory = sandbox / "dormant";
  std::filesystem::create_directories(directory);

  const auto worldReadable = directory / "dormant-0644.json";
  const auto groupWritable = directory / "dormant-0664.json";
  const auto alreadyPrivate = directory / "dormant-0600.json";
  const auto exotic = directory / "dormant-0640.json";
  SeedFile(worldReadable, 0644);
  SeedFile(groupWritable, 0664);
  SeedFile(alreadyPrivate, 0600);
  SeedFile(exotic, 0640);
  const auto contentsBefore = ReadBack(worldReadable);

  const auto result = server::util::HardenSecretFilesInDirectory(directory);
  assert(result.examined == 4);
  assert(result.narrowed == 3);       // 0600 уже был узким и не считается
  assert(result.failed == 0);

  assert(ModeOf(worldReadable) == 0600);
  assert(ModeOf(groupWritable) == 0600);
  assert(ModeOf(alreadyPrivate) == 0600);
  assert(ModeOf(exotic) == 0600);
  // ★СОДЕРЖИМОЕ НЕ ТРОНУТО: правка меняет инод, а не запись.
  assert(ReadBack(worldReadable) == contentsBefore);

  // Повторный проход не находит работы — то есть проверка умеет отличать
  // «сузили» от «и так было узко», а не отчитывается о работе всегда.
  const auto second = server::util::HardenSecretFilesInDirectory(directory);
  assert(second.examined == 4);
  assert(second.narrowed == 0);
  assert(second.failed == 0);

  // ★ССЫЛКА НЕ АККАУНТ, И ИДТИ ПО НЕЙ НЕЛЬЗЯ. Цель ссылки обязана остаться со
  // своим режимом: иначе «сужение прав аккаунтов» правило бы чужие файлы.
  const auto outsider = sandbox / "outsider-0644.json";
  SeedFile(outsider, 0644);
  std::error_code linkError;
  std::filesystem::create_symlink(outsider, directory / "link.json", linkError);
  assert(not linkError);
  const auto third = server::util::HardenSecretFilesInDirectory(directory);
  assert(third.examined == 4);        // ссылка не осмотрена и не провалена
  assert(third.failed == 0);
  assert(ModeOf(outsider) == 0644);

  // Каталога нет — не ошибка и не бросок, но и НЕ «всё проверено»: этот случай
  // обязан быть отличим от пустого читаемого каталога, иначе «0 отказов»
  // означало бы «чисто» там, где мы вообще не смотрели.
  const auto absent =
    server::util::HardenSecretFilesInDirectory(sandbox / "no-such-dir");
  assert(absent.examined == 0);
  assert(absent.narrowed == 0);
  assert(absent.failed == 0);
  assert(absent.incomplete);

  // ...а пустой СУЩЕСТВУЮЩИЙ каталог — это полный обход с нулём находок.
  const auto emptyDirectory = sandbox / "empty-accounts";
  std::filesystem::create_directories(emptyDirectory);
  const auto empty = server::util::HardenSecretFilesInDirectory(emptyDirectory);
  assert(empty.examined == 0);
  assert(not empty.incomplete);
}

//! ★ОБЩИЙ ОБХОД КАТАЛОГА (правка ревью, итерация 2). Проверяется РОВНО то
//! свойство, ради которого он заведён: полный обход отличим от оборванного, а
//! нечитаемый каталог не выглядит как пустой.
void TestListRegularFiles(const std::filesystem::path& sandbox)
{
  const auto directory = sandbox / "listing";
  std::filesystem::create_directories(directory);
  SeedFile(directory / "a.json", 0600);
  SeedFile(directory / "b.json", 0600);
  std::filesystem::create_directories(directory / "nested");

  const auto listing = server::util::ListRegularFiles(directory);
  assert(listing.files.size() == 2);   // подкаталог — не обычный файл
  assert(not listing.incomplete);

  // Каталога нет — пусто И неполно, а не просто пусто.
  const auto absent = server::util::ListRegularFiles(sandbox / "no-such-listing");
  assert(absent.files.empty());
  assert(absent.incomplete);

  // ★НЕЧИТАЕМЫЙ КАТАЛОГ НЕ ИМЕЕТ ПРАВА ВЫГЛЯДЕТЬ ПУСТЫМ. Под root права не
  // ограничивают, и утверждение молча стало бы ложно-зелёным, поэтому оно
  // выполняется только под обычным пользователем.
  if (::geteuid() != 0)
  {
    const auto locked = sandbox / "locked-listing";
    std::filesystem::create_directories(locked);
    SeedFile(locked / "hidden.json", 0600);
    assert(::chmod(locked.c_str(), 0000) == 0);

    const auto blind = server::util::ListRegularFiles(locked);
    assert(blind.files.empty());
    assert(blind.incomplete);

    assert(::chmod(locked.c_str(), 0700) == 0);
  }
}

//! ★НЕОБЫЧНАЯ ЗАПИСЬ В КАТАЛОГЕ АККАУНТОВ НЕ ИМЕЕТ ПРАВА НИ ПОВЕСИТЬ СТАРТ, НИ
//! ОТМЕНИТЬ ЕГО (правка ревью, итерация 3).
//!
//! Проход сужения сперва ОТКРЫВАЛ каждую запись, чтобы через `fstat` узнать её
//! тип. На именованном канале `open(O_RDONLY)` ждёт писателя — старт сервера
//! вис бы навсегда; на сокете `open` даёт ENXIO, а после правки «отказ сужения
//! останавливает старт» это означало ОТКАЗ В ОБСЛУЖИВАНИИ из-за записи, которую
//! политика велит пропустить. Оба случая проверяются здесь настоящими объектами
//! файловой системы, а не рассуждением.
void TestNonRegularEntriesAreSkippedNotFatal(const std::filesystem::path& sandbox)
{
  const auto directory = sandbox / "exotic-accounts";
  std::filesystem::create_directories(directory);

  const auto account = directory / "real-0644.json";
  SeedFile(account, 0644);

  // ★ИМЕНОВАННЫЙ КАНАЛ. Если `O_NONBLOCK` пропадёт, этот тест не покраснеет —
  // он ПОВИСНЕТ, и повисший прогон есть тот же отказ, что и на старте сервера.
  const auto fifo = directory / "fifo.json";
  assert(::mkfifo(fifo.c_str(), 0600) == 0);

  // ★СОКЕТ. `open` по нему даёт ENXIO: прежняя редакция посчитала бы это
  // отказом сужения (++failed) и уронила бы старт.
  const auto socketPath = directory / "socket.json";
  const int socketDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
  assert(socketDescriptor >= 0);
  ::sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto socketName = socketPath.string();
  assert(socketName.size() + 1 < sizeof(address.sun_path));
  std::memcpy(address.sun_path, socketName.c_str(), socketName.size() + 1);
  const int bound = ::bind(
    socketDescriptor,
    reinterpret_cast<const ::sockaddr*>(&address),
    static_cast<::socklen_t>(sizeof(address)));
  assert(bound == 0);
  assert(::close(socketDescriptor) == 0);

  // ★ВИСЯЧАЯ ССЫЛКА. `is_regular_file` ходит ПО ссылке и даёт ENOENT; считать
  // эту ошибку неполнотой значило бы останавливать старт из-за мусора.
  std::error_code linkError;
  std::filesystem::create_symlink(
    directory / "no-such-target.json", directory / "dangling.json", linkError);
  assert(not linkError);

  const auto listing = server::util::ListRegularFiles(directory);
  assert(listing.files.size() == 1);      // только настоящий аккаунт
  assert(listing.files.front() == account);
  assert(not listing.incomplete);         // и обход ПОЛНЫЙ, а не «мы не смотрели»

  const auto hardening = server::util::HardenSecretFilesInDirectory(directory);
  assert(hardening.examined == 1);
  assert(hardening.narrowed == 1);
  assert(hardening.failed == 0);          // сокет и канал — не отказы сужения
  assert(not hardening.incomplete);       // и не повод отказать в старте
  assert(ModeOf(account) == 0600);

  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
}

//! Пара «владелец:группа» файла или каталога.
std::pair<uid_t, gid_t> OwnerOf(const std::filesystem::path& path)
{
  struct ::stat status{};
  const int result = ::lstat(path.c_str(), &status);
  assert(result == 0);
  return {status.st_uid, status.st_gid};
}

//! Собранные приёмником сообщения — ими и проверяется, что жалоба ПРОЗВУЧАЛА.
//!
//! ★ЗАЧЕМ ВООБЩЕ ЛОВИТЬ СТРОКИ. Три правки этой итерации молчаливы по природе:
//! ссылку «отвергли», режим «сузили», владельца «усыновили». Отличить их от
//! «ничего не сделали» можно только по внешнему признаку, и лог — тот самый
//! признак, который в бою прочитает человек. Проверка, чей вердикт никто не
//! читает, проверкой не является — поэтому здесь его читает тест.
std::vector<std::string>& SinkMessages()
{
  static std::vector<std::string> messages;
  return messages;
}

void CapturingSink(const std::string_view message)
{
  SinkMessages().emplace_back(message);
}

//! ★ДРОССЕЛЬ ОБЯЗАН И ПРОПУСКАТЬ, И ГЛУШИТЬ. Проверка только «пропускает»
//! оставила бы поток строк на пути входа (дефект R57) необнаруженным, проверка
//! только «глушит» — не отличила бы работающий приёмник от отсутствующего.
void TestWarningSinkIsThrottled()
{
  server::util::SetFileWarningSink(&CapturingSink);
  SinkMessages().clear();

  server::util::WarningThrottle throttle;
  server::util::ReportFileWarning(throttle, "первое сообщение {}", 1);
  server::util::ReportFileWarning(throttle, "второе сообщение {}", 2);
  server::util::ReportFileWarning(throttle, "третье сообщение {}", 3);

  // Одна площадка — ровно одно сообщение в окне.
  assert(SinkMessages().size() == 1);
  assert(SinkMessages().front().find("первое сообщение 1") != std::string::npos);

  // Соседняя площадка дросселируется независимо: иначе редкая, но важная
  // жалоба тонула бы в частой соседней.
  server::util::WarningThrottle other;
  server::util::ReportFileWarning(other, "другая площадка");
  assert(SinkMessages().size() == 2);

  // Подавленные не теряются: их число едет в следующем сообщении этой площадки.
  throttle.lastEmitNanos.store(server::util::WarningThrottle::kNever);
  server::util::ReportFileWarning(throttle, "после окна");
  assert(SinkMessages().size() == 3);
  assert(SinkMessages().back().find("подавлено") != std::string::npos);

  SinkMessages().clear();
}

//! ★ССЫЛКА ОТВЕРГАЕТСЯ ВСЕМИ ЧЕТЫРЬМЯ ПОТРЕБИТЕЛЯМИ, А НЕ ТРЕМЯ ИЗ ЧЕТЫРЁХ.
//!
//! Ревью (итерация 4) показало ровно расхождение потребителей: обход ссылку
//! молча пропускал, проход сужения рапортовал полный успех, индекс имени не
//! знал — а вход открывал ЦЕЛЬ и принимал её хеш пароля. Поэтому здесь один
//! тест на все стороны сразу: если хоть одна снова начнёт ходить по ссылке,
//! красным станет именно она.
void TestSymbolicLinksAreRefusedEverywhere(const std::filesystem::path& sandbox)
{
  server::util::SetFileWarningSink(&CapturingSink);
  SinkMessages().clear();

  const auto directory = sandbox / "linked-accounts";
  std::filesystem::create_directories(directory);

  // ★ЦЕЛЬ ЛЕЖИТ В ДРУГОМ КАТАЛОГЕ, И ЭТО УСЛОВИЕ КОРРЕКТНОСТИ ТЕСТА, А НЕ
  // декорация. Лежи она рядом, проход сужения нашёл бы её СВОИМ обходом как
  // обычный файл и честно сузил — а тест засчитал бы это как «пошли по ссылке»
  // и был бы ложно-красным. Проверять надо ровно одно: сквозь ССЫЛКУ не ходят.
  const auto elsewhere = sandbox / "link-target-elsewhere";
  std::filesystem::create_directories(elsewhere);
  const auto target = elsewhere / "target-0644.data";
  SeedFile(target, 0644);
  const auto link = directory / "Alice.json";
  std::error_code linkError;
  std::filesystem::create_symlink(target, link, linkError);
  assert(not linkError);

  // 1) Обход: ссылки нет среди файлов, но она НАЗВАНА отдельно.
  const auto listing = server::util::ListRegularFiles(directory);
  assert(listing.refusedSymlinks.size() == 1);
  assert(listing.refusedSymlinks.front() == link);
  assert(not listing.incomplete);
  for (const auto& file : listing.files)
    assert(file != link);

  // 2) Проход сужения: ссылка сосчитана как отвергнутая и НЕ сужена по цели.
  const auto hardening = server::util::HardenSecretFiles(listing);
  assert(hardening.refusedLinks == 1);
  assert(hardening.failed == 0);
  assert(ModeOf(target) == 0644);   // цель не тронута — по ссылке не ходили

  // 3) Чтение: отказ, ни байта содержимого, и цель по-прежнему не тронута.
  const auto read = server::util::ReadManagedFile(link, FileSensitivity::Secret);
  assert(read.status == server::util::ManagedReadStatus::Refused);
  assert(read.content.empty());
  assert(not read.narrowed);
  assert(ModeOf(target) == 0644);

  // 4) Запись: бросок, и ни ссылка, ни цель не заменены.
  bool threw = false;
  try
  {
    server::util::WriteFileAtomically(
      link, kPayload, "User file", FileSensitivity::Secret);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  assert(threw);
  assert(std::filesystem::is_symlink(link));
  assert(ReadBack(target) == "старое содержимое");

  // И об отказе БЫЛО СКАЗАНО ВСЛУХ хотя бы один раз.
  assert(not SinkMessages().empty());

  SinkMessages().clear();
  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
  std::filesystem::remove_all(elsewhere, cleanup);
}

//! ★ФАЙЛ, ПРИШЕДШИЙ ПОСЛЕ СТАРТА, СУЖАЕТСЯ ДО ТОГО, КАК ЕГО ПРОЧТУТ.
//!
//! Стартовый проход — снимок; помощник, положивший `Alice.json` с обычным
//! umask рядом с работающим сервером, оставлял хеш пароля читаемым всем до
//! перезапуска. Здесь проверяется именно порядок: содержимое отдано, и к этому
//! моменту режим уже 0600.
void TestLateArrivingSecretIsNarrowedOnRead(const std::filesystem::path& sandbox)
{
  server::util::SetFileWarningSink(&CapturingSink);
  SinkMessages().clear();

  const auto directory = sandbox / "late-arrival";
  std::filesystem::create_directories(directory);
  const auto account = directory / "Bob.json";
  SeedFile(account, 0644);

  const auto read = server::util::ReadManagedFile(account, FileSensitivity::Secret);
  assert(read.status == server::util::ManagedReadStatus::Ok);
  assert(read.narrowed);                       // сузили ПРЯМО СЕЙЧАС
  assert(ModeOf(account) == 0600);             // и до выдачи содержимого
  assert(read.content == "старое содержимое"); // содержимое при этом целое
  assert(not SinkMessages().empty());

  // Повторное чтение уже ничего не чинит и ни одного лишнего вызова не делает.
  const auto again = server::util::ReadManagedFile(account, FileSensitivity::Secret);
  assert(again.status == server::util::ManagedReadStatus::Ok);
  assert(not again.narrowed);
  assert(ModeOf(account) == 0600);

  // ★А `Public` НЕ СУЖАЕТ, и это не забывчивость: режим файла персонажа —
  // ровно тот, что был до раунда (радиус правки не расширяется молча).
  const auto character = directory / "100.json";
  SeedFile(character, 0644);
  const auto publicRead = server::util::ReadManagedFile(
    character, FileSensitivity::Public);
  assert(publicRead.status == server::util::ManagedReadStatus::Ok);
  assert(not publicRead.narrowed);
  assert(ModeOf(character) == 0644);

  // Отсутствие файла — отдельный исход, а не отказ: на нём стоит регистрация.
  const auto missing = server::util::ReadManagedFile(
    directory / "no-such.json", FileSensitivity::Secret);
  assert(missing.status == server::util::ManagedReadStatus::Missing);

  SinkMessages().clear();
  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
}

//! ★НОВЫЙ СЕКРЕТ БЕРЁТ ВЛАДЕЛЬЦА У КАТАЛОГА, А НЕ У ПРОЦЕССА.
//!
//! ★УТВЕРЖДЕНИЕ, А НЕ ТЕСТ, ПРОПУСКАЕТСЯ ПОД НЕ-ROOT. Доказать усыновление
//! можно только там, где владелец каталога ОТЛИЧАЕТСЯ от владельца процесса, а
//! назначить чужого владельца умеет лишь root. Под обычным пользователем
//! проверяется всё остальное (режим 0600, владелец совпал с каталогом) —
//! ложно-зелёного утверждения «усыновили», которое на деле сравнивало бы
//! процесс сам с собой, здесь нет.
void TestNewSecretAdoptsDirectoryOwner(const std::filesystem::path& sandbox)
{
  const auto directory = sandbox / "owned-accounts";
  std::filesystem::create_directories(directory);

  const bool asRoot = ::geteuid() == 0;
  if (asRoot)
  {
    // Каталог принадлежит НЕ нам — ровно расклад прода: контейнер стартует от
    // root, а `data/users` на деплое отдан непривилегированному помощнику.
    assert(::chown(directory.c_str(), 1000, 1000) == 0);
  }

  const auto account = directory / "Carol.json";
  server::util::WriteFileAtomically(
    account, kPayload, "User file", FileSensitivity::Secret);

  assert(ModeOf(account) == 0600);
  assert(OwnerOf(account) == OwnerOf(directory));
  if (asRoot)
  {
    // ★ИМЕННО ЭТО И БЫЛО СЛОМАНО: файл получал root:root, и помощник
    // `set-password.py` терял путь успеха на КАЖДОМ новом аккаунте.
    assert(OwnerOf(account).first == 1000u);
    assert(OwnerOf(account).second == 1000u);
  }

  // ★А `Public` ВЛАДЕЛЬЦА НЕ УСЫНОВЛЯЕТ: политика объявлена для секрета, и
  // семнадцать прочих писателей ведут себя ровно как до раунда.
  if (asRoot)
  {
    const auto record = directory / "200.json";
    server::util::WriteFileAtomically(
      record, kPayload, "Character file", FileSensitivity::Public);
    assert(OwnerOf(record).first == 0u);
  }

  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
}

//! ★ОТКАЗ СУЖЕНИЯ — ЭТО ОТКАЗ ЧТЕНИЯ, А НЕ СТРОЧКА В ЛОГЕ.
//!
//! Ревью (итерация 5) показало ложно-зелёное по построению: `EnsureSecretOnOpen`
//! возвращал `false` И на «уже узкий», И на «`fchmod` провалился», поэтому
//! чтение печатало «хеш остаётся открытым» и ТУТ ЖЕ отдавало этот хеш, а
//! перестройка индекса заводила такое имя как рабочее. Здесь отказ вызывается
//! по-настоящему: файл принадлежит ТРЕТЬЕМУ пользователю, а мы смотрим на него
//! из-под непривилегированного euid — открыть его можно (0644), а сузить нельзя.
//!
//! ★УТВЕРЖДЕНИЕ ПРОПУСКАЕТСЯ ПОД НЕ-ROOT, А НЕ ТЕСТ. Разложить владельцев умеет
//! только root; под обычным пользователем проверять было бы нечего, и «зелёный»
//! означал бы «не проверяли».
void TestUnenforceableSecretIsRefusedNotServed(const std::filesystem::path& sandbox)
{
  if (::geteuid() != 0)
    return;

  server::util::SetFileWarningSink(&CapturingSink);
  SinkMessages().clear();

  const auto directory = sandbox / "unenforceable";
  std::filesystem::create_directories(directory);
  assert(::chmod(directory.c_str(), 0755) == 0);

  const auto account = directory / "Dave.json";
  SeedFile(account, 0644);
  // Владелец — ЧУЖОЙ и для каталога, и для того, кем мы станем ниже: только
  // тогда `fchmod` действительно отказывает, а не «мог бы отказать».
  assert(::chown(account.c_str(), 1001, 1001) == 0);

  // ★КОНТРОЛЬНЫЙ ФАЙЛ В СОСЕДНЕМ КАТАЛОГЕ — ЭТО ДОКАЗАТЕЛЬСТВО, ЧТО ХАРНЕСС
  // ВИДИТ. Без него тест был бы зелёным и там, где до каталога вообще не
  // добраться: `Failed` от `EACCES` на пути и `Failed` от несостоявшегося
  // `fchmod` — один и тот же исход, а проверяем мы ВТОРОЙ. (Ровно так этот
  // тест и провалился в контейнере: `TMPDIR` лежал под `/root` с режимом 0700,
  // и uid 1000 не мог войти в песочницу.) Каталог отдельный, чтобы контроль не
  // попал в тот же проход сужения и не сдвинул число отказов.
  const auto controlDirectory = sandbox / "unenforceable-control";
  std::filesystem::create_directories(controlDirectory);
  assert(::chmod(controlDirectory.c_str(), 0755) == 0);
  const auto control = controlDirectory / "Readable.json";
  SeedFile(control, 0644);
  assert(::chown(control.c_str(), 1001, 1001) == 0);

  // Сохранённый uid остаётся нулевым, поэтому вернуться сможем.
  assert(::seteuid(1000) == 0);

  const auto controlRead = server::util::ReadManagedFile(
    control, FileSensitivity::Public);
  const bool harnessCanSee =
    controlRead.status == server::util::ManagedReadStatus::Ok
    && not controlRead.content.empty();

  const auto read = server::util::ReadManagedFile(account, FileSensitivity::Secret);
  const bool refusedRead = read.status == server::util::ManagedReadStatus::Failed;
  const bool withoutContent = read.content.empty();
  const bool notNarrowed = not read.narrowed;

  const auto listing = server::util::ListRegularFiles(directory);
  const bool listingComplete = not listing.incomplete && listing.files.size() == 1;
  const auto hardening = server::util::HardenSecretFiles(listing);
  const bool countedFailed = hardening.failed == 1;
  const bool namedFailed = hardening.unsecured.size() == 1
    && hardening.unsecured.front() == account;

  assert(::seteuid(0) == 0);

  // ★СНАЧАЛА КОНТРОЛЬ: если он красный, всё остальное ничего не значит.
  assert(harnessCanSee);
  assert(listingComplete);

  // ★СОДЕРЖИМОЕ НЕ ОТДАНО. Именно это и было сломано: строка в лог уходила, а
  // хеш — вызывающему.
  assert(refusedRead);
  assert(withoutContent);
  assert(notNarrowed);
  // ★И ПРОХОД СУЖЕНИЯ НАЗЫВАЕТ ФАЙЛ ПОИМЁННО, а не только считает: перестройка
  // индекса обязана уметь ОБОЙТИ его стороной, а числу это не под силу.
  assert(countedFailed);
  assert(namedFailed);
  assert(ModeOf(account) == 0644);   // сузить и вправду не удалось

  SinkMessages().clear();
  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
  std::filesystem::remove_all(controlDirectory, cleanup);
}

//! ★ВЛАДЕЛЕЦ УСЫНОВЛЯЕТСЯ И У ФАЙЛА, КОТОРЫЙ УЖЕ УЗОК.
//!
//! Ревью (итерация 5): ранний выход «режим и так owner-only» пропускал ПРОВЕРКУ
//! ВЛАДЕЛЬЦА целиком, поэтому восстановленный из копии `root:root 0600`,
//! положенный в каталог `dev:dev`, не усыновлялся никогда — вход работал, а
//! помощник администратора не мог открыть файл бессрочно.
void TestOwnerIsAdoptedEvenWhenModeIsAlreadyNarrow(
  const std::filesystem::path& sandbox)
{
  if (::geteuid() != 0)
    return;

  const auto directory = sandbox / "adoption-on-read";
  std::filesystem::create_directories(directory);
  assert(::chown(directory.c_str(), 1000, 1000) == 0);

  const auto account = directory / "Eve.json";
  SeedFile(account, 0600);
  assert(::chown(account.c_str(), 0, 0) == 0);

  const auto read = server::util::ReadManagedFile(account, FileSensitivity::Secret);
  assert(read.status == server::util::ManagedReadStatus::Ok);
  assert(not read.narrowed);                 // сужать было нечего…
  assert(OwnerOf(account) == OwnerOf(directory));  // …а владельца всё равно взяли
  assert(OwnerOf(account).first == 1000u);

  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
}

//! ★ЗАПИСЬ ДЛИННЕЕ ПОТОЛКА НЕ ЧИТАЕТСЯ ВОВСЕ.
//!
//! Общий читатель складывает файл в память ДО разбора, а чтение аккаунта стоит
//! на пути входа, то есть ДО аутентификации: без потолка одна попытка входа
//! именем, под которым лежит разрежённый гигабайтный файл, стоила бы контейнеру
//! всей памяти (ревью, итерация 5).
void TestOversizedRecordIsRefusedWithoutReading(
  const std::filesystem::path& sandbox)
{
  const auto directory = sandbox / "oversized";
  std::filesystem::create_directories(directory);

  const auto huge = directory / "Huge.json";
  {
    std::ofstream file(huge);
    assert(file.is_open());
    file << "{";
  }
  // Разрежённый: на диске занимает почти ничего, но `st_size` — за потолком.
  assert(::truncate(
    huge.c_str(),
    static_cast<off_t>(server::util::kMaxManagedRecordBytes) + 1) == 0);

  const auto read = server::util::ReadManagedFile(huge, FileSensitivity::Secret);
  assert(read.status == server::util::ManagedReadStatus::Refused);
  assert(read.content.empty());

  // А запись ровно по потолку читается: граница отсекает лишнее, а не живое.
  const auto atCeiling = directory / "AtCeiling.json";
  SeedFile(atCeiling, 0600);
  const auto ok = server::util::ReadManagedFile(atCeiling, FileSensitivity::Public);
  assert(ok.status == server::util::ManagedReadStatus::Ok);

  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
}

//! ★ОТКАЗ ПО РАЗМЕРУ НЕ ОТМЕНЯЕТ ПРИНУЖДЕНИЯ РЕЖИМА (ревью, итерация 6).
//!
//! Потолок размера, поставленный итерацией 5, встал ВЫШЕ принуждения секрета —
//! и завёл путь, на котором файл аккаунта отвергается, ни разу не пройдя
//! `EnsureSecretOnOpen`: помощник кладёт рядом с работающим сервером
//! `Alice.json` 0644, добитый до 4 МиБ, вход честно отказывает, а хеш пароля
//! остаётся читаемым для group/other до перезапуска. Отказ обслуживать и отказ
//! защищать — разные вещи.
void TestOversizedSecretIsNarrowedBeforeBeingRefused(
  const std::filesystem::path& sandbox)
{
  const auto directory = sandbox / "oversized-secret";
  std::filesystem::create_directories(directory);

  const auto account = directory / "Fiona.json";
  SeedFile(account, 0644);
  // Разрежённый: `st_size` за потолком, места на диске почти не занимает.
  assert(::truncate(
    account.c_str(),
    static_cast<off_t>(server::util::kMaxManagedRecordBytes) + 1) == 0);
  assert(ModeOf(account) == 0644);

  const auto read = server::util::ReadManagedFile(account, FileSensitivity::Secret);

  // Содержимое не отдано — потолок на месте…
  assert(read.status == server::util::ManagedReadStatus::Refused);
  assert(read.content.empty());
  // …и при этом режим ПРИВЕДЁН к политике, а не оставлен «на потом».
  assert(ModeOf(account) == 0600);

  // Контроль направления: тот же файл класса `Public` режима не меняет —
  // значит 0600 выше пришло от принуждения секрета, а не от чего-то ещё.
  const auto other = directory / "public.json";
  SeedFile(other, 0644);
  assert(::truncate(
    other.c_str(),
    static_cast<off_t>(server::util::kMaxManagedRecordBytes) + 1) == 0);
  const auto publicRead = server::util::ReadManagedFile(
    other, FileSensitivity::Public);
  assert(publicRead.status == server::util::ManagedReadStatus::Refused);
  assert(ModeOf(other) == 0644);

  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
}

//! ★ПОТОЛОК ЕСТЬ И У ПИСАТЕЛЯ, А НЕ ТОЛЬКО У ЧИТАТЕЛЯ (ревью, итерация 6).
//!
//! Потолок в одну сторону создавал запись, которая успешно СОХРАНЯЕТСЯ и
//! перестаёт читаться после перезапуска, — то есть тихую потерю данных вместо
//! громкого отказа. Здесь проверяется и то, что отказ НЕ ТРОГАЕТ прежний файл:
//! инвариант «либо целиком, либо не тронули» обязан держаться и на этом отказе.
void TestWriterRefusesRecordsBeyondTheCeiling(
  const std::filesystem::path& sandbox)
{
  const auto directory = sandbox / "write-ceiling";
  std::filesystem::create_directories(directory);

  const auto record = directory / "Big.json";
  server::util::WriteFileAtomically(
    record, kPayload, "Character file", FileSensitivity::Public);
  const auto before = ReadBack(record);

  const std::string oversized(
    static_cast<std::size_t>(server::util::kMaxManagedRecordBytes) + 1, 'x');
  bool threw = false;
  try
  {
    server::util::WriteFileAtomically(
      record, oversized, "Character file", FileSensitivity::Public);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  assert(threw);
  // Прежняя запись цела, и временного файла не осталось.
  assert(ReadBack(record) == before);
  std::error_code exists;
  assert(not std::filesystem::exists(
    directory / "Big.json.tmp", exists));

  // Ровно по потолку — пишется: граница отсекает лишнее, а не живое.
  const std::string atCeiling(
    static_cast<std::size_t>(server::util::kMaxManagedRecordBytes), 'y');
  server::util::WriteFileAtomically(
    record, atCeiling, "Character file", FileSensitivity::Public);
  assert(ReadBack(record).size() == atCeiling.size());

  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
}

//! ★СОЗДАНИЕ И УДАЛЕНИЕ ТОЖЕ НЕ ХОДЯТ ПО ССЫЛКАМ (ревью, итерация 6).
//!
//! `O_NOFOLLOW` и общий читатель закрыли чтение и запись, но пятнадцать методов
//! `Delete*` звали `std::filesystem::remove(path)`, а пути данных рождались
//! через `create_directories(path)`. Обе функции разрешают ПРОМЕЖУТОЧНЫЕ
//! компоненты насквозь: подмена `characters/equipment` ссылкой на чужое дерево
//! превращала штатное удаление предмета в удаление ЧУЖОГО файла.
void TestManagedCreateAndRemoveRefuseSymlinkedDirectories(
  const std::filesystem::path& sandbox)
{
  const auto root = sandbox / "anchored";
  const auto foreign = sandbox / "foreign-tree";
  std::filesystem::create_directories(root);
  std::filesystem::create_directories(foreign);

  const auto victim = foreign / "17.json";
  SeedFile(victim, 0644);

  std::error_code linkError;
  std::filesystem::create_directory_symlink(foreign, root / "equipment", linkError);
  assert(not linkError);

  // Создание НЕ пролезает сквозь ссылку…
  assert(not server::util::CreateManagedDirectories(root / "equipment" / "items"));
  assert(not std::filesystem::exists(foreign / "items"));

  // …и удаление тоже: чужой файл цел.
  assert(not server::util::RemoveManagedFile(root / "equipment" / "17.json"));
  assert(std::filesystem::exists(victim));

  // Контроль направления: те же две операции в НАСТОЯЩЕМ каталоге работают.
  assert(server::util::CreateManagedDirectories(root / "real" / "items"));
  assert(std::filesystem::is_directory(root / "real" / "items"));
  const auto mine = root / "real" / "items" / "17.json";
  SeedFile(mine, 0644);
  assert(server::util::RemoveManagedFile(mine));
  assert(not std::filesystem::exists(mine));
  // Отсутствующая запись — успех, а не отказ (прежний `remove` вёл себя так же).
  assert(server::util::RemoveManagedFile(mine));

  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
  std::filesystem::remove_all(foreign, cleanup);
}

//! ★УБОРКА ВРЕМЕННЫХ ФАЙЛОВ НЕ ВЫХОДИТ ЗА ДЕРЕВО ДАННЫХ (ревью, итерация 6).
//!
//! Прежний проход шёл `recursive_directory_iterator` (в ссылки он не заходит),
//! но удалял через `std::filesystem::remove(entry.path())` — по ПУТИ, то есть
//! сквозь промежуточную ссылку. Достаточно было подменить один подкаталог, и
//! уборка своего мусора становилась удалением чужих файлов.
void TestSweepStaysInsideTheDataTree(const std::filesystem::path& sandbox)
{
  const auto root = sandbox / "sweep-root";
  const auto foreign = sandbox / "sweep-foreign";
  std::filesystem::create_directories(root / "characters");
  std::filesystem::create_directories(foreign);

  const auto theirs = foreign / "victim.tmp";
  SeedFile(theirs, 0644);
  const auto mine = root / "characters" / "7.json.tmp";
  SeedFile(mine, 0644);
  const auto keep = root / "characters" / "7.json";
  SeedFile(keep, 0644);

  std::error_code linkError;
  std::filesystem::create_directory_symlink(foreign, root / "equipment", linkError);
  assert(not linkError);

  server::util::SweepStaleTemporaries(root);

  // Свой мусор убран…
  assert(not std::filesystem::exists(mine));
  // …настоящая запись не тронута…
  assert(std::filesystem::exists(keep));
  // …а чужое дерево уборка не увидела вовсе.
  assert(std::filesystem::exists(theirs));

  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
  std::filesystem::remove_all(foreign, cleanup);
}

//! ★ЧТО ОБХОД ПЕРЕЧИСЛИЛ, НО СУЖЕНИЕ НЕ ОСМОТРЕЛО, ПУБЛИКОВАТЬ НЕЛЬЗЯ
//! (ревью, итерация 6).
//!
//! Обход и проход сужения работали по РАЗНЫМ дескрипторам одного имени, а
//! запись, исчезнувшая или ставшая ссылкой между фазами, пропускалась МОЛЧА —
//! после чего имя из СТАРОГО списка всё равно уходило в индекс, то есть
//! «никогда не индексируется» было неправдой. Здесь проверяется и то, и другое:
//! дескриптор у списка ОДИН и остаётся открытым, а неосмотренное имя названо.
void TestUnexaminedListedPathsAreNotPublishable(
  const std::filesystem::path& sandbox)
{
  const auto directory = sandbox / "unexamined";
  std::filesystem::create_directories(directory);

  const auto staying = directory / "Stays.json";
  const auto vanishing = directory / "Vanishes.json";
  SeedFile(staying, 0644);
  SeedFile(vanishing, 0644);

  const auto listing = server::util::ListRegularFiles(directory);
  assert(not listing.incomplete);
  assert(listing.files.size() == 2);
  // ★ДЕСКРИПТОР ОБХОДА ЖИВ: именно он, а не повторное разрешение пути, отдаётся
  // проходу сужения.
  assert(listing.descriptor.descriptor >= 0);

  // Запись исчезает МЕЖДУ фазами — ровно окно, о котором говорит ревью.
  std::error_code removal;
  std::filesystem::remove(vanishing, removal);
  assert(not removal);

  const auto hardening = server::util::HardenSecretFiles(listing);

  // Оставшийся файл сужен и публикуем…
  assert(ModeOf(staying) == 0600);
  assert(hardening.narrowed == 1);
  // …а исчезнувший НАЗВАН непубликуемым, а не пропущен молча.
  assert(hardening.unsecured.size() == 1);
  assert(hardening.unsecured.front() == vanishing);
  // И это не «отказ сужения»: старт из-за пропавшего файла не падает.
  assert(hardening.failed == 0);

  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
}

//! ★ССЫЛКА НА МЕСТЕ САМОГО КАТАЛОГА ТОЖЕ ОТВЕРГАЕТСЯ.
//!
//! `O_NOFOLLOW` защищает ТОЛЬКО последний компонент, поэтому `data/users`,
//! подменённый ссылкой, прежде проходился молча всеми четырьмя потребителями
//! (ревью, итерация 5). Здесь проверяется, что путь разбирается покомпонентно:
//! ни обход, ни чтение, ни запись, ни правка режима каталога сквозь ссылку не
//! ходят — и настоящий каталог остаётся нетронутым.
void TestSymlinkedDirectoryIsRefused(const std::filesystem::path& sandbox)
{
  server::util::SetFileWarningSink(&CapturingSink);
  SinkMessages().clear();

  const auto real = sandbox / "real-accounts";
  std::filesystem::create_directories(real);
  assert(::chmod(real.c_str(), 0755) == 0);
  const auto account = real / "Frank.json";
  SeedFile(account, 0644);

  const auto linked = sandbox / "linked-accounts-dir";
  std::error_code linkError;
  std::filesystem::create_directory_symlink(real, linked, linkError);
  assert(not linkError);

  // 1) Обход сквозь ссылку-каталог — не пустой список, а НЕПОЛНЫЙ.
  const auto listing = server::util::ListRegularFiles(linked);
  assert(listing.files.empty());
  assert(listing.incomplete);

  // 2) Чтение файла ЧЕРЕЗ ссылку-каталог отвергнуто, содержимого нет, и режим
  //    настоящего файла не тронут (то есть по ссылке не ходили вовсе).
  const auto read = server::util::ReadManagedFile(
    linked / "Frank.json", FileSensitivity::Secret);
  assert(read.status == server::util::ManagedReadStatus::Refused);
  assert(read.content.empty());
  assert(ModeOf(account) == 0644);

  // 3) Запись через ссылку-каталог бросает и НИЧЕГО не создаёт в настоящем.
  bool threw = false;
  try
  {
    server::util::WriteFileAtomically(
      linked / "Grace.json", kPayload, "User file", FileSensitivity::Secret);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  assert(threw);
  assert(not std::filesystem::exists(real / "Grace.json"));
  assert(not std::filesystem::exists(real / "Grace.json.tmp"));

  // 4) Правка режима каталога сквозь ссылку — отказ, и режим цели тот же.
  //    Прежняя редакция звала здесь `chmod` ПО ПУТИ, то есть правила бы права
  //    чужого каталога.
  assert(not server::util::EnsureDirectoryMode(linked, 0700));
  assert(ModeOf(real) == 0755);

  SinkMessages().clear();
  std::error_code cleanup;
  std::filesystem::remove(linked, cleanup);
  std::filesystem::remove_all(real, cleanup);
}

//! ★ИМЯ ИЗ КАТАЛОГА — ЭТО ВВОД, И В ЛОГ ОНО ПОПАДАЕТ ЭКРАНИРОВАННЫМ.
//!
//! Всё, кроме `/` и NUL, годится в имя записи каталога, в том числе перевод
//! строки. Без экранирования ОДНО задросселированное предупреждение печатало бы
//! НЕСКОЛЬКО видимых записей лога — дроссель считает вызовы, а не строки,
//! которые из них вылезли, то есть подделка журнала обходила бы ровно тот пояс,
//! который заведён против потока строк (ревью, итерация 5).
void TestFilesystemNamesAreEscapedInWarnings(const std::filesystem::path& sandbox)
{
  server::util::SetFileWarningSink(&CapturingSink);
  SinkMessages().clear();

  const auto directory = sandbox / "hostile-names";
  std::filesystem::create_directories(directory);

  const std::string hostile = "a\nWARN forged log line\tb.json";
  std::error_code linkError;
  std::filesystem::create_symlink(
    sandbox / "no-such-target", directory / hostile, linkError);
  assert(not linkError);

  const auto listing = server::util::ListRegularFiles(directory);
  assert(listing.refusedSymlinks.size() == 1);
  assert(not SinkMessages().empty());

  const auto& message = SinkMessages().back();
  // Ни одного СЫРОГО управляющего байта — то есть одна жалоба остаётся одной
  // строкой, что бы ни лежало в каталоге.
  assert(message.find('\n') == std::string::npos);
  assert(message.find('\t') == std::string::npos);
  // И при этом имя не потеряно: оно видно, просто в экранированном виде.
  assert(message.find("\\x0a") != std::string::npos);
  assert(message.find("WARN forged log line") != std::string::npos);

  SinkMessages().clear();
  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
}

} // namespace

int main()
{
  const auto sandbox = MakeSandbox();
  TestNewSecretFileIsPrivate(sandbox);
  TestNewPublicFileKeepsUmaskBehaviour(sandbox);
  TestInheritedModesAreNarrowedForSecrets(sandbox);
  TestPublicInheritanceIsUntouched(sandbox);
  TestOwnerFloor(sandbox);
  TestHostileUmask(sandbox);
  TestEnsureDirectoryMode(sandbox);
  TestHardenSecretFilesInDirectory(sandbox);
  TestListRegularFiles(sandbox);
  TestNonRegularEntriesAreSkippedNotFatal(sandbox);
  TestWarningSinkIsThrottled();
  // ★ЭТОТ ТЕСТ ИДЁТ ПЕРВЫМ СРЕДИ ЧИТАЮЩИХ ЖАЛОБЫ, И ЭТО УСЛОВИЕ ЕГО
  // ОСМЫСЛЕННОСТИ, А НЕ ПОРЯДОК РАДИ ПОРЯДКА: площадка дросселя у обхода одна
  // на процесс, поэтому вторым он читал бы ПОДАВЛЕННОЕ сообщение и был бы
  // ложно-красным по причине, к экранированию отношения не имеющей.
  TestFilesystemNamesAreEscapedInWarnings(sandbox);
  TestSymbolicLinksAreRefusedEverywhere(sandbox);
  TestLateArrivingSecretIsNarrowedOnRead(sandbox);
  TestNewSecretAdoptsDirectoryOwner(sandbox);
  TestUnenforceableSecretIsRefusedNotServed(sandbox);
  TestOwnerIsAdoptedEvenWhenModeIsAlreadyNarrow(sandbox);
  TestOversizedRecordIsRefusedWithoutReading(sandbox);
  TestOversizedSecretIsNarrowedBeforeBeingRefused(sandbox);
  TestWriterRefusesRecordsBeyondTheCeiling(sandbox);
  TestManagedCreateAndRemoveRefuseSymlinkedDirectories(sandbox);
  TestSweepStaysInsideTheDataTree(sandbox);
  TestUnexaminedListedPathsAreNotPublishable(sandbox);
  TestSymlinkedDirectoryIsRefused(sandbox);

  UnlockTree(sandbox);
  std::error_code ignored;
  std::filesystem::remove_all(sandbox, ignored);
  std::puts("TestAtomicFile: ok");
}

#else

int main()
{
  // Под Windows режима в POSIX-смысле нет — проверять нечего, но цель обязана
  // собираться, иначе сборка ломается там, где раунд ничего не менял.
  return 0;
}

#endif
