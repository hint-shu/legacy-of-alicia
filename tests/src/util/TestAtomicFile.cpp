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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef WIN32

  #include <sys/stat.h>
  #include <sys/types.h>
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

  // Каталога нет — не ошибка и не бросок.
  const auto absent =
    server::util::HardenSecretFilesInDirectory(sandbox / "no-such-dir");
  assert(absent.examined == 0);
  assert(absent.narrowed == 0);
  assert(absent.failed == 0);
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
