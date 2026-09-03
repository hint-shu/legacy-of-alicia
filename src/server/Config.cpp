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

#include "server/Config.hpp"
#include "server/ConfigStrict.hpp"
#include "libserver/util/QuietLog.hpp"

#include <libserver/util/Util.hpp>

#include <charconv>
#include <format>
#include <fstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

namespace server
{

void Config::LoadFromEnvironment()
{
  const auto getEnvValue = [](const std::string& key)
  {
    std::string value;

    // Unfortunately musl standard implementation does not support getenv_s,
    // so we have to stick with the unsafe version.
    const char* envValue = getenv(key.data());
    if (envValue == nullptr)
      return value;

    value = std::string_view(envValue);
    return value;
  };

  const auto getAddressAndPortVariables = [&getEnvValue](
    const std::string& addressVariableName,
    const std::string& portVariableName,
    asio::ip::address_v4& address,
    uint16_t& port)
  {
    try
    {
      // Get the address.
      const auto addressValue = getEnvValue(addressVariableName);
      if (not addressValue.empty())
      {
        address = util::ResolveHostName(addressValue);
      }
    }
    catch (const std::exception&)
    {
      server::util::QuietLogError(" Couldn't resolve the host for '{}'", addressVariableName);
    }

    // Get the port.
    const std::string portValue = getEnvValue(portVariableName);
    if (not portValue.empty())
    {
      const auto result = std::from_chars(
        portValue.c_str(),
        portValue.c_str() + portValue.length(),
        port);
      if (result.ec != std::errc{})
      {
        server::util::QuietLogError("Couldn't resolve the port for '{}'.", portVariableName);
      }
    }
  };

  // Lobby address and port.
  getAddressAndPortVariables(
    std::format("LOBBY_SERVER_ADDRESS"),
    std::format("LOBBY_SERVER_PORT"),
    lobby.listen.address,
    lobby.listen.port);

  // Lobby advertised address and port for ranch.
  getAddressAndPortVariables(
    std::format("LOBBY_ADVERTISED_RANCH_ADDRESS"),
    std::format("LOBBY_ADVERTISED_RANCH_PORT"),
    lobby.advertisement.ranch.address,
    lobby.advertisement.ranch.port);

  // Lobby advertised address and port for race.
  getAddressAndPortVariables(
    std::format("LOBBY_ADVERTISED_RACE_ADDRESS"),
    std::format("LOBBY_ADVERTISED_RACE_PORT"),
    lobby.advertisement.race.address,
    lobby.advertisement.race.port);

  // Lobby advertised address and port for udp race relay.
  getAddressAndPortVariables(
    std::format("LOBBY_ADVERTISED_UDP_RACE_RELAY_ADDRESS"),
    std::format("LOBBY_ADVERTISED_UDP_RACE_RELAY_PORT"),
    lobby.advertisement.udpRaceRelay.address,
    lobby.advertisement.udpRaceRelay.port);

  // Ranch address and port.
  getAddressAndPortVariables(
    std::format("RANCH_SERVER_ADDRESS"),
    std::format("RANCH_SERVER_PORT"),
    ranch.listen.address,
    ranch.listen.port);

  // LOA (R70-fix-7, backlog #58): срок удержания попапов достижений заезда.
  // ★ПЕРЕМЕННАЯ СРЕДЫ, А НЕ ТОЛЬКО КЛЮЧ YAML, И ЭТО НЕ УДОБСТВО. Конфиг живёт
  // ВНУТРИ образа и стендами не монтируется намеренно (каталог достижений
  // обязан ехать тем же каноном, что и бинарь). Значит единственный способ дать
  // стенду короткий срок, не подменяя образ, — среда. Прод переменную не
  // ставит и остаётся на 900 с.
  // ★ЗНАЧЕНИЕ, КОТОРОЕ НЕ РАЗОБРАЛОСЬ, — ОТКАЗ, А НЕ УМОЛЧАНИЕ: тихий откат к
  // 900 означал бы, что стенд поставил 20 с, получил 900 и объявил «протухания
  // нет» — ложно-зелёный ровно там, где эта настройка и заведена.
  // ★ЧИТАЕМ getenv НАПРЯМУЮ, А НЕ ЧЕРЕЗ `getEnvValue` (R70-fix-8, находка
  // Codex 6 BLOCK-2). `getEnvValue` возвращает пустую строку И на отсутствующей
  // переменной, И на явно пустой (`FOO=`) — то есть СКЛЕИВАЕТ «не задано» с
  // «задано мусором». Именно на этой склейке опечатка становилась умолчанием.
  // Разбор — общий с YAML-веткой (`ParseStrictPositiveSeconds`), чтобы правила
  // строгости не разъехались между двумя источниками.
  {
    const char* const holdValue = getenv("RANCH_ACHIEVEMENT_NOTIFY_HOLD_SECONDS");
    if (holdValue != nullptr)
    {
      ranch.achievementNotifyHoldSeconds = ParseStrictPositiveSeconds(
        "RANCH_ACHIEVEMENT_NOTIFY_HOLD_SECONDS",
        holdValue);
    }
  }

  // Race address and port.
  getAddressAndPortVariables(
    std::format("RACE_SERVER_ADDRESS"),
    std::format("RACE_SERVER_PORT"),
    race.listen.address,
    race.listen.port);

  // All chat address and port.
  getAddressAndPortVariables(
    std::format("ALL_CHAT_SERVER_ADDRESS"),
    std::format("ALL_CHAT_SERVER_PORT"),
    allChat.listen.address,
    allChat.listen.port);

  // Private chat address and port.
  getAddressAndPortVariables(
    std::format("PRIVATE_CHAT_SERVER_ADDRESS"),
    std::format("PRIVATE_CHAT_SERVER_PORT"),
    privateChat.listen.address,
    privateChat.listen.port);

  // UDP race relay address and port.
  getAddressAndPortVariables(
    std::format("UDP_RACE_RELAY_SERVER_ADDRESS"),
    std::format("UDP_RACE_RELAY_SERVER_PORT"),
    udpRaceRelay.listen.address,
    udpRaceRelay.listen.port);
}

void Config::LoadFromFile(const std::filesystem::path& filePath)
{
  std::ifstream file(filePath);

  if (not file.is_open())
  {
    server::util::QuietLogError(
      "Could not open configuration file at '{}'",
      filePath.string());
    return;
  }

  const auto parseListenSection = [](const YAML::Node& node)
  {
    try
    {
      return Listen{
        .address = util::ResolveHostName(node["address"].as<std::string>()),
        .port = node["port"].as<uint16_t>()
      };
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Failed parsing address or port: {}", e.what());
    }

    return Listen{};
  };

  try
  {
    const YAML::Node yamlConfig = YAML::Load(file);
    const auto serverYaml = yamlConfig["server"];

    // General config
    try
    {
      const auto generalYaml = serverYaml["general"];
      general.brand = generalYaml["brand"].as<std::string>("<not set>");
      general.notice = generalYaml["notice"].as<std::string>("");
      general.promotePassphrase = generalYaml["promotePassphrase"].as<std::string>("");
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Unhandled exception parsing the general config: {}", e.what());
    }

    // Authentication config
    try
    {
      const auto authenticationYaml = serverYaml["authentication"];
      authentication.backend = authenticationYaml["backend"].as<std::string>("local");
      authentication.postgres.connectionUri = authenticationYaml["postgres"]["connectionUri"].as<std::string>("");
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Unhandled exception parsing the authentication config: {}", e.what());
    }

    // Metrics config
    try
    {
      const auto telemetryYaml = serverYaml["metrics"];
      telemetry.enabled = telemetryYaml["enabled"].as<bool>(false);
      telemetry.backend = telemetryYaml["backend"].as<std::string>("none");
      telemetry.postgres.connectionUri = telemetryYaml["postgres"]["connectionUri"].as<std::string>("");
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Unhandled exception parsing the metrics config: {}", e.what());
    }

    // Lobby config
    try
    {
      const auto lobbyYaml = serverYaml["lobby"];
      lobby.enabled = lobbyYaml["enabled"].as<bool>();
      lobby.listen = parseListenSection(lobbyYaml["listen"]);

      const auto lobbyAdvertisementYaml = lobbyYaml["advertisement"];
      lobby.advertisement.ranch = parseListenSection(lobbyAdvertisementYaml["ranch"]);
      lobby.advertisement.race = parseListenSection(lobbyAdvertisementYaml["race"]);
      lobby.advertisement.messenger = parseListenSection(lobbyAdvertisementYaml["messenger"]);
      lobby.advertisement.allChat = parseListenSection(lobbyAdvertisementYaml["all_chat"]);
      lobby.advertisement.privateChat = parseListenSection(lobbyAdvertisementYaml["private_chat"]);
      lobby.advertisement.udpRaceRelay = parseListenSection(lobbyAdvertisementYaml["udp_race_relay"]);
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Unhandled exception parsing the lobby config: {}", e.what());
    }

    // Ranch config
    try
    {
      const auto ranchYaml = serverYaml["ranch"];
      ranch.enabled = ranchYaml["enabled"].as<bool>();
      ranch.listen = parseListenSection(ranchYaml["listen"]);

      // LOA (R70-fix-7, backlog #58): срок удержания попапа достижения заезда.
      // ★КЛЮЧ НЕОБЯЗАТЕЛЕН, НО ЕСЛИ ОН ЕСТЬ — ОН ОБЯЗАН БЫТЬ ЧИТАЕМ. Старые
      // конфиги без ключа обязаны работать (иначе правка ломает деплой), а
      // конфиг С ключом, который не разбирается, — это тихая подмена срока на
      // умолчание, то есть ровно тот ложно-зелёный, ради которого раунд и
      // завёл настраиваемость: стенд поставил бы 20 с, получил бы 900 и
      // объявил «протухания нет».
      // ★РАЗБИРАЕМ СКАЛЯР САМИ, А НЕ `as<uint32_t>()` (R70-fix-8, находка
      // Codex 6 BLOCK-2). Конверсия yaml-cpp снисходительна и, главное, её
      // исключение — обычный `std::exception`, который секционный перехват
      // ниже съедал бы вместе с остальными. Через `Scalar()` строка приходит
      // как есть и проходит ТЕ ЖЕ правила, что и переменная среды.
      if (const auto holdYaml = ranchYaml["achievement_notify_hold_seconds"];
        holdYaml.IsDefined())
      {
        if (not holdYaml.IsScalar())
          throw ConfigError(
            "ranch.achievement_notify_hold_seconds", "value is not a scalar");
        ranch.achievementNotifyHoldSeconds = ParseStrictPositiveSeconds(
          "ranch.achievement_notify_hold_seconds",
          holdYaml.Scalar());
      }
    }
    // ★СТРОГИЕ КЛЮЧИ ПЕРЕБРАСЫВАЮТСЯ, А НЕ ЛОГИРУЮТСЯ. Перехват ниже — это
    // осознанная снисходительность к секции целиком (сломанный `listen` не
    // должен ронять сервер), но ключ, чьё ОБЕЩАНИЕ — «плохое значение = отказ
    // старта», обязан пройти сквозь неё. Иначе обещание существует только в
    // комментарии.
    catch (const ConfigError&)
    {
      throw;
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Unhandled exception parsing the ranch config: {}", e.what());
    }

    // Race config
    try
    {
      const auto raceYaml = serverYaml["race"];
      race.enabled = raceYaml["enabled"].as<bool>();
      race.listen = parseListenSection(raceYaml["listen"]);
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Unhandled exception parsing the race config: {}", e.what());
    }

    // Messenger config
    try
    {
      const auto messengerYaml = serverYaml["messenger"];
      messenger.enabled = messengerYaml["enabled"].as<bool>();
      messenger.listen = parseListenSection(messengerYaml["listen"]);
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Unhandled exception parsing the messenger config: {}", e.what());
    }

    // All chat config
    try
    {
      const auto allChatYaml = serverYaml["all_chat"];
      allChat.enabled = allChatYaml["enabled"].as<bool>();
      allChat.listen = parseListenSection(allChatYaml["listen"]);
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Unhandled exception parsing the all chat config: {}", e.what());
    }

    // Private chat config
    try
    {
      const auto privateChatYaml = serverYaml["private_chat"];
      privateChat.enabled = privateChatYaml["enabled"].as<bool>();
      privateChat.listen = parseListenSection(privateChatYaml["listen"]);
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Unhandled exception parsing the private chat config: {}", e.what());
    }

    // UDP race relay config
    try
    {
      const auto udpYaml = serverYaml["udp_race_relay"];
      udpRaceRelay.enabled = udpYaml["enabled"].as<bool>();
      udpRaceRelay.listen = parseListenSection(udpYaml["listen"]);
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Unhandled exception parsing the udp race relay config: {}", e.what());
    }

    // Messenger config
    try
    {
      const auto dataYaml = serverYaml["data"];

      const auto dataSourceName = dataYaml["source"].as<std::string>();
      if (dataSourceName == "file")
      {
        const auto fileYaml = dataYaml["file"];
        data.file.basePath = fileYaml["basePath"].as<std::string>();
      }
      else
      {
        server::util::QuietLogError("Unsupported data source type: {}", dataSourceName);
      }
    }
    catch (const std::exception& e)
    {
      server::util::QuietLogError("Unhandled exception parsing the dat config: {}", e.what());
    }
  }
  // ★И ВНЕШНИЙ ПЕРЕХВАТ ТОЖЕ ПРОПУСКАЕТ СТРОГИЕ КЛЮЧИ. Их два, и починить
  // только внутренний значило бы оставить дыру ровно того же класса одним
  // уровнем выше — [[total-invariant-beats-list-of-sites]].
  catch (const ConfigError&)
  {
    throw;
  }
  catch (const std::exception& e)
  {
    server::util::QuietLogError("Unhandled exception parsing the config: {}", e.what());
  }
}

} // namespace server
