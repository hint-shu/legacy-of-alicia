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

#include "server/authentication/LocalAuthenticationBackend.hpp"
#include "libserver/util/AtomicFile.hpp"
#include "libserver/util/NameGuard.hpp"
#include "libserver/util/QuietLog.hpp"

#include <array>
#include <cstddef>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include <libserver/util/picosha2.hpp>

namespace server
{

namespace
{

//! LOA-fix (#18c): раундов растяжки. SHA-256 быстрый — прогоняем его много раз,
//! чтобы перебор пароля был дорогим (KDF из быстрого хеша). Проверка — не
//! горячий путь (раз на логин), 100k раундов ~единицы мс.
constexpr int kPasswordStretchRounds = 100000;

//! 16 случайных байт соли в hex. std::random_device — крипто-энтропия ОС
//! (уже используется в кодовой базе: OtpSystem.hpp, LobbyNetworkHandler.cpp).
std::string GenerateSaltHex()
{
  std::random_device rd;
  std::array<unsigned char, 16> salt{};
  for (auto& byte : salt)
    byte = static_cast<unsigned char>(rd() & 0xFF);
  std::ostringstream oss;
  for (unsigned char byte : salt)
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
  return oss.str();
}

//! Растянутый хеш пароля: h0 = SHA256(saltHex || password), затем ещё
//! (rounds-1) раз h = SHA256(hex(h)). Детерминирован — register и verify на
//! ОДНОМ сервере всегда совпадают (совместимость с внешним инструментом не
//! требуется: миграция идёт через grandfather-вход, сервер сам хеширует). Соль
//! на юзера → одинаковые пароли дают разные хеши.
std::string StretchPassword(const std::string& saltHex, const std::string& password)
{
  std::string hash = picosha2::hash256_hex_string(saltHex + password);
  for (int round = 1; round < kPasswordStretchRounds; ++round)
    hash = picosha2::hash256_hex_string(hash);
  return hash;
}

//! Сравнение хешей за постоянное время (не сливаем через тайминг позицию
//! первого несовпадения байта).
bool ConstantTimeEquals(const std::string& lhs, const std::string& rhs)
{
  if (lhs.size() != rhs.size())
    return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < lhs.size(); ++i)
    diff |= static_cast<unsigned char>(lhs[i] ^ rhs[i]);
  return diff == 0;
}

//! Атомарная запись файла аккаунта (tmp + rename). false при любом сбое —
//! вызывающий трактует как отказ (fail-closed), чтобы НИКОГДА не пустить с
//! незаписанным паролем.
bool WriteUserJsonAtomic(const std::filesystem::path& path, const nlohmann::json& json)
{
  // LOA-fix (R58-10, round58, backlog #175): та же атомарность, но через общего
  // помощника. Своя копия имела три дефекта: закрытие потока не проверялось
  // (деструктор ошибку сброса глотает), ранний `return false` ОСТАВЛЯЛ временный
  // файл на диске, и права исходного файла терялись при переименовании.
  // ★Контракт функции сохранён дословно: `false` на любом сбое, ни одного броска
  // наружу — вызывающий трактует отказ как fail-closed и не пускает с
  // незаписанным паролем.
  try
  {
    // ★ЭТОТ ФАЙЛ НЕСЁТ ХЕШ И СОЛЬ ПАРОЛЯ — единственный класс `Secret` здесь
    // (LOA-fix R73-1, #206). Один вызов обслуживает ОБЕ ветки записи аккаунта:
    // регистрацию при первом входе и «дедовскую» ветку старых файлов.
    server::util::WriteFileAtomically(
      path, json.dump(2), "User file", server::util::FileSensitivity::Secret);
    return true;
  }
  catch (const std::exception& x)
  {
    server::util::QuietLogError(
      "Failed to write the user file '{}': {}", server::util::LogPath(path), x.what());
    return false;
  }
}

} // namespace

LocalAuthenticationBackend::LocalAuthenticationBackend(
  std::filesystem::path usersDirectory)
  : _usersDirectory(std::move(usersDirectory))
{
  // ★ПРИЁМНИК ПРЕДУПРЕЖДЕНИЙ ФАЙЛОВОГО ПОМОЩНИКА (LOA-fix R73-5, ревью 4).
  // `AtomicFile.hpp` сознательно не тянет spdlog, поэтому редкие жалобы —
  // отвергнутая ссылка, сужение поздно пришедшего файла — выходят наружу через
  // указатель на функцию. Ставится и здесь, и в `FileDataSource::Initialize`:
  // указатель один и тот же, повторная установка безвредна, а порядок создания
  // этих двух объектов не гарантирован ничем — тот, кто окажется первым, и
  // включит звук.
  server::util::SetFileWarningSink(
    [](const std::string_view message)
    {
      server::util::QuietLogWarn("{}", message);
    });
}

std::optional<bool> LocalAuthenticationBackend::Authenticate(
  const std::string& userName,
  const std::string& userToken)
{
  // #18b/#18c: имя логина — строгий allowlist [A-Za-z0-9_-] (как в
  // инсталляторе/лаунчере). Разом закрывает path traversal (нет '/', '\\',
  // '.', '..'), NUL/control-байты и unicode-трюки → имя ВСЕГДА безопасное имя
  // файла в каталоге users. Сервер не доверяет клиенту — проверяет сам.
  // ★ОПРЕДЕЛЕНИЕ КЛАССА ПЕРЕЕХАЛО В `util::IsLoginNameSafe` (R73-3): та же
  // проверка была нужна `FileDataSource::IsUserNameUnique`, а два независимых
  // определения одного класса умеют разъехаться молча.
  if (not server::util::IsLoginNameSafe(userName))
    return false;

  // #18c: userToken == authKey из settings.json == ПАРОЛЬ. Пустой пароль не
  // регистрируем и не пускаем (HandleLogin уже отсекает пустой authKey — дубль).
  if (userToken.empty())
    return false;

  const std::filesystem::path userPath = _usersDirectory / (userName + ".json");

  // ★ОДНО ОТКРЫТИЕ ВМЕСТО «СУЩЕСТВУЕТ?» И ПОТОМ `ifstream` (LOA-fix R73-6,
  // правка ревью, итерация 4).
  //
  // Прежняя пара `exists(path)` + `std::ifstream in(path)` ХОДИЛА ПО ССЫЛКЕ
  // дважды: `data/users/Alice.json`, ставший ссылкой на чужой файл 0644, здесь
  // открывался, его хеш принимался как пароль Алисы — при том что проход
  // сужения эту запись не трогал, а индекс имён её не знал. Три потребителя
  // отвечали по-разному на вопрос, существует ли аккаунт. Теперь ответ один:
  // ссылка под `data/` не управляемая запись и НЕ АУТЕНТИФИЦИРУЕТ.
  //
  // ★И КЛАСС `Secret` ЗДЕСЬ НЕ УКРАШЕНИЕ: `ReadManagedFile` сузит режим файла,
  // положенного рядом помощником с обычным umask (0644), ПРЕЖДЕ чем отдаст его
  // содержимое, — то есть первый же вход чинит поздно пришедший аккаунт, не
  // дожидаясь ни перезапуска, ни следующего сохранения.
  //
  // ★ЗАОДНО ИСЧЕЗЛА ГОНКА «ПРОВЕРИЛИ-И-ОТКРЫЛИ»: решение «новое имя или нет»
  // принимается по ИСХОДУ ОТКРЫТИЯ, а не по отдельному предварительному
  // вопросу к файловой системе, ответ на который к моменту открытия мог
  // устареть.
  const auto userRead = server::util::ReadManagedFile(
    userPath, server::util::FileSensitivity::Secret);
  if (userRead.status == server::util::ManagedReadStatus::Refused
    || userRead.status == server::util::ManagedReadStatus::Failed)
  {
    return false; // fail-closed: ссылка, канал, ошибка ФС
  }

  // --- Ветка 1: новое имя → register-on-first-use ---
  // Создаём аккаунт с солью+хешем присланного пароля. characterUid=0
  // (InvalidUid) → игрок пойдёт в создание персонажа. CreateUser в
  // FileDataSource — заглушка, поэтому пишем файл здесь (иначе пароль негде
  // сохранить: login-флоу пароля не видит).
  if (userRead.status == server::util::ManagedReadStatus::Missing)
  {
    const std::string salt = GenerateSaltHex();
    const std::string hash = StretchPassword(salt, userToken);
    nlohmann::json json;
    json["name"] = userName;
    json["token"] = "";
    json["characterUid"] = 0;
    json["infractions"] = nlohmann::json::array();
    json["lastSeenOnline"] = 0;
    json["passwordHash"] = hash;
    json["passwordSalt"] = salt;
    return WriteUserJsonAtomic(userPath, json); // не записалось → false (fail-closed)
  }

  // --- Существующий аккаунт: читаем сохранённый хеш ---
  nlohmann::json json;
  try
  {
    json = nlohmann::json::parse(userRead.content);
  }
  catch (const std::exception&)
  {
    return false; // битый JSON → fail-closed
  }

  const std::string storedHash = json.value("passwordHash", std::string{});
  const std::string storedSalt = json.value("passwordSalt", std::string{});

  // Битая пара (ровно ОДНО поле пусто) — порча файла/ручная правка/будущий баг.
  // Fail-closed: НЕ переустанавливаем пароль, иначе аккаунт с УЖЕ заданным
  // паролем можно было бы захватить повторным grandfather.
  const bool bothEmpty = storedHash.empty() && storedSalt.empty();
  const bool bothSet = not storedHash.empty() && not storedSalt.empty();
  if (not bothEmpty && not bothSet)
    return false;

  // --- Ветка 2: легаси-аккаунт БЕЗ пароля (ОБА поля пусты) → grandfather ---
  // Принимаем и запоминаем присланный пароль как пароль аккаунта. Все прочие
  // поля (name/token/characterUid/infractions/lastSeenOnline) сохраняются как
  // есть — nlohmann хранит непрочитанные ключи. Персонаж владельца не теряется.
  if (bothEmpty)
  {
    const std::string salt = GenerateSaltHex();
    const std::string hash = StretchPassword(salt, userToken);
    json["passwordHash"] = hash;
    json["passwordSalt"] = salt;
    return WriteUserJsonAtomic(userPath, json);
  }

  // --- Ветка 3: пароль задан → сверяем ---
  const std::string computed = StretchPassword(storedSalt, userToken);
  return ConstantTimeEquals(computed, storedHash);
}

} // namespace server