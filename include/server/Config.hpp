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

#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <nlohmann/json.hpp>
#include <boost/asio/ip/address.hpp>

namespace server
{

namespace asio = boost::asio;

class Config
{
public:
  //! Generic listen section consisting of address and port fields.
  struct Listen
  {
    //! An IPv4 or a hostname.
    asio::ip::address_v4 address{
      asio::ip::address_v4::any()};
    //! A port.
    uint16_t port{0};
  };

  //!
  struct General
  {
    std::string brand;
    std::string notice;
    //! Passphrase required to use the //promote command.
    std::string promotePassphrase;
  } general{};

  //!
  struct Authentication
  {
    std::string backend{};

    struct Postgres
    {
      std::string connectionUri;
    } postgres;

  } authentication{};

  //!
  struct Telemetry
  {
    bool enabled;
    std::string backend;

    struct Postgres
    {
      std::string connectionUri;
    } postgres{};
  } telemetry{};

  //!
  struct Lobby
  {
    bool enabled{true};
    Listen listen{
      .port = 10030};

    struct Advertisement
    {
      Listen ranch{
        .address = asio::ip::address_v4::loopback(),
        .port = 10031};
      Listen race{
        .address = asio::ip::address_v4::loopback(),
        .port = 10032};
      Listen messenger{
        .address = asio::ip::address_v4::loopback(),
        .port = 10033};
      Listen allChat{
        .address = asio::ip::address_v4::loopback(),
        .port = 10034};
      Listen privateChat{
        .address = asio::ip::address_v4::loopback(),
        .port = 10035};
      Listen udpRaceRelay{
        .address = asio::ip::address_v4::loopback(),
        .port = 10500};
    } advertisement{};
  } lobby{};

  //!
  struct Ranch
  {
    bool enabled{true};
    Listen listen{
      .port = 10031};

    //! LOA (R70-fix-7, backlog #58; ЧИСЛО ПОДНЯТО R81, #261): СКОЛЬКО ДЕРЖАТЬ
    //! ПОПАП ДОСТИЖЕНИЯ ЗАЕЗДА, пока игрок не вернулся на ранчо.
    //! **86400 с = сутки.**
    //!
    //! ★ЗАЧЕМ ЭТО ВООБЩЕ НАСТРАИВАЕТСЯ. Не ради «гибкости»: срок обязан быть
    //! ПРОВЕРЯЕМ. Стенд раунда судит предикат «протухшее выброшено, удержано
    //! ноль», и ждать пятнадцать минут в каждой клетке матрицы нельзя —
    //! проверка, которую слишком дорого запускать, не запускается вовсе.
    //!
    //! ★ЧТО ЗДЕСЬ БЫЛО НАПИСАНО НЕВЕРНО ДО R81 (и потому переписано): «прод
    //! остаётся на умолчании». ПРОД ЧИТАЕТ YAML — конфиг бинд-маунтится с
    //! хоста (`tools/config_drift.sh`), и живое значение лежит в
    //! `resources/config/server/config.yaml`, ключ
    //! `ranch.achievement_notify_hold_seconds`. Правка ТОЛЬКО этого умолчания
    //! не изменила бы на проде НИЧЕГО. Поэтому 86400 стоит В ДВУХ копиях —
    //! здесь и в YAML, — и их расхождение стережёт проверка с числом в §6.2
    //! спеки раунда.
    //!
    //! ★86400, А НЕ «ПОБОЛЬШЕ»: (1) верхнюю границу памяти теперь задаёт
    //! ПОТОЛОК ЧИСЛА ПЕРСОНАЖЕЙ (`AchievementNotifyHold::CharacterCountCap`),
    //! а не срок; (2) сутки — это «зашёл вечером следующего дня», сценарий
    //! бэклога #261; (3) бесконечный срок был бы неотличим от персистенции,
    //! которой раунд НЕ делает (очередь не переживает перезапуск сервера).
    //!
    //! ★ЭТО НЕ ТО ЖЕ ЧИСЛО, ЧТО В КОНСТРУКТОРЕ ПОЛЯ ДИРЕКТОРА
    //! (`RanchDirector.hpp`, `seconds(900)`) — там КАНАРЕЙКА, читать её
    //! комментарий целиком, прежде чем «привести к одному числу».
    //! Ноль и отрицательные значения не принимаются (см. Config.cpp).
    uint32_t achievementNotifyHoldSeconds{86400};
  } ranch{};

  //!
  struct Race
  {
    bool enabled{true};
    Listen listen{
      .port = 10032};
  } race{};

  //!
  struct Messenger
  {
    bool enabled{true};
    Listen listen{
      .port = 10033};
  } messenger{};

  //!
  struct AllChat
  {
    bool enabled{true};
    Listen listen{
      .port = 10034};
  } allChat{};

  struct PrivateChat
  {
    bool enabled{true};
    Listen listen{
      .port = 10035};
  } privateChat{};

  //! LOA (R80-2, round80, backlog #235): ПОРОГИ ЖАТВЫ ЧАТ-СОКЕТОВ.
  //!
  //! ★ОБЩИЕ для мессенджера и all-chat: болезнь у них одна, а два набора ручек
  //! означали бы два способа настроить прод неверно.
  //!
  //! ★ЧИТАЮТСЯ ТОЛЬКО ИЗ СРЕДЫ, НИ ОДНОГО КЛЮЧА В `resources/config/**`, И ЭТО
  //! РЕШЕНИЕ, А НЕ ЛЕНЬ. `tools/config_drift.sh` требует ПОБАЙТОВОГО совпадения
  //! репозиторного конфига с конфигом прод-хоста; добавленный ключ — это дрейф,
  //! то есть красный гейт до выкладки конфига и обязательный шаг протокола 8.4.
  //! Стенду достаточно среды, прод остаётся на умолчаниях.
  //! ★ЦЕНА НАЗВАНА ВСЛУХ: на проде ручек нет, пороги меняются переменной в
  //! compose прод-хоста или пересборкой.
  struct ChatReap
  {
    //! Как часто идёт развёртка. Тик чат-сервера — 1 Гц, обход O(n) каждую
    //! секунду был бы пустой нагрузкой.
    //! ★ПОЧЕМУ 30. Развёртка обязана укладываться в ПОЛОВИНУ `orphanGrace`,
    //! иначе задержка обнаружения сироты доходит до двух грейсов и дедлайн
    //! перестаёт что-либо отличать. При грейсе 60 с это 30 с.
    uint32_t sweepIntervalSeconds{30};
    //! P1: сколько ждать рукопожатия. Настоящее занимает миллисекунды.
    uint32_t handshakeTimeoutSeconds{60};
    //! P2: сколько молчания требуется сироте ДОПОЛНИТЕЛЬНО к отсутствию
    //! лобби-сессии. Логаут доезжает за ≈1 с — запас шестидесятикратный.
    uint32_t orphanGraceSeconds{60};
    //! P3: абсолютный простой. ★0 = ВЫКЛЮЧЕНО, и это умолчание прода; включает
    //! только стенд. Разбор — `ParseStrictNonNegativeSeconds`.
    uint32_t absoluteIdleSeconds{0};
  } chatReap{};

  //!
  struct UdpRaceRelay
  {
    bool enabled{true};
    Listen listen{
      .address = asio::ip::address_v4::loopback(),
      .port = 10500};
  } udpRaceRelay{};

  //!
  struct Data
  {
    enum class Source
    {
      File, Postgres
    } source{Source::File};

    struct File
    {
      std::string basePath = "./data";
    } file{};

    struct Postgres
    {

    } postgres{};
  } data{};

  //! Loads the config from the environment.
  void LoadFromEnvironment();
  //! Loads the config from the specified file.
  //! @param  filePath Path to the config file.
  void LoadFromFile(const std::filesystem::path& filePath);
};

} // namespace server

#endif // CONFIG_HPP