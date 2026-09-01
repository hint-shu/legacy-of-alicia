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

namespace
{

using server::util::FileSensitivity;

//! Каталог-песочница одного прогона.
std::filesystem::path MakeSandbox()
{
  const auto sandbox = std::filesystem::temp_directory_path()
    / std::filesystem::path("alicia-atomicfile-test");
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
