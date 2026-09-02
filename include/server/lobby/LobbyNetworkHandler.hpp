/**
 * Alicia Server - dedicated server software
 * Copyright (C) 2025-2026 Story Of Alicia
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

#ifndef ALICIA_SERVER_LOBBYNETWORKHANDLER_HPP
#define ALICIA_SERVER_LOBBYNETWORKHANDLER_HPP

#include "server/system/MatchmakingSystem.hpp"

#include <libserver/data/DataDefinitions.hpp>
#include <libserver/network/command/CommandServer.hpp>
#include <libserver/network/command/proto/LobbyMessageDefinitions.hpp>
#include <libserver/util/LogThrottle.hpp>

#include <string>

namespace server
{

class ServerInstance;

class LobbyNetworkHandler final
  : public CommandServer::EventHandlerInterface
{
public:
  explicit LobbyNetworkHandler(ServerInstance& serverInstance);

  void Initialize();
  void Terminate();

  void AcceptLogin(
    ClientId clientId,
    bool sendToCharacterCreator = false);
  void RejectLogin(
    ClientId clientId,
    protocol::AcCmdCLLoginCancel::Reason reason);

  void SendCharacterGuildInvitation(
    data::Uid inviteeUid,
    data::Uid guildUid,
    data::Uid inviterUid);

  void SetCharacterVisitPreference(
    data::Uid characterUid,
    data::Uid rancherUid);

  void DisconnectCharacter(
    data::Uid characterUid);
  void MuteCharacter(
    data::Uid characterUid,
    data::Clock::time_point expiration);
  void NotifyCharacter(
    data::Uid characterUid,
    const std::string& message);

  void NotifyAchievementReward(
    data::Uid characterUid);
  void NotifyMatchmakeResult(
    const data::Uid characterUid,
    const MatchmakingSystem::Result& result);

  //! LOA-fix (R72-fix-2, round72, backlog #129-S1, находка Codex 3):
  //! ★СЫРОЙ `CommandServer` БОЛЬШЕ НЕ ВЫХОДИТ ИЗ КЛАССА.
  //!
  //! Здесь стоял `GetCommandServer()`, отдававший наружу диспетчер целиком —
  //! то есть и `RegisterCommandHandler`. Пока он существовал, инвариант
  //! «у лобби нет регистрации без решения об авторизации» держался только на
  //! том, что никто не воспользовался публичным входом: регистрацию можно было
  //! завести из ЛЮБОЙ единицы трансляции, а гард смотрит один `.cpp`. Это
  //! ровно [[a-gate-must-prove-itself-first]]: числа зелены, а свойство не
  //! тотально. Границу закрывает не гард, а тип: диспетчер приватен, наружу
  //! отдаётся ровно то, что единственному вызывающему было нужно, — адрес.
  //!
  //! Единственный бывший потребитель — `LobbyDirector::ProcesLoginResponse`,
  //! которому нужен адрес клиента для строки лога.
  //! @param clientId ID клиента.
  //! @returns Адрес клиента строкой; пустая строка, если клиента уже нет.
  [[nodiscard]] std::string GetClientAddress(ClientId clientId) noexcept;

private:
  struct ClientContext
  {
    //! A flag indicating whether the client is authenticated.
    bool isAuthenticated{false};
    //! A flag indicating whether the client is in the character creator.
    bool isInCharacterCreator{false};
    //! A flag indicating whether the client just created a character.
    bool justCreatedCharacter{false};

    //! A time point of the last heartbeat.
    std::chrono::steady_clock::time_point lastHeartbeat{};

    //! LOA-fix (R12-5, round12, backlog #85): момент, когда лобби-таймауту
    //! впервые отсрочили кик из-за загрузки карты заезда. Значение по умолчанию
    //! (эпоха steady_clock) = «грейс не активен». Отдельная метка нужна потому,
    //! что сам грейс освежает `lastHeartbeat` (R12-4b) — по нему потолок жизни
    //! зомби посчитать уже нельзя. Метка живёт от входа в грейс до одного из
    //! трёх событий: реальный пульс клиента (R12-6), одноразовый хендофф сразу
    //! после окончания загрузки (R12-4b) и переиспользование контекста
    //! (подключение — R12-7, выход из создателя персонажа — R12-8). То есть
    //! каждая загрузка карты получает свежий потолок, а не остаток от прошлой.
    std::chrono::steady_clock::time_point raceLoadingGraceSince{};

    //! LOA-fix (R21-4a, round21, backlog #95): момент, когда лобби-таймауту
    //! впервые отсрочили кик из-за АКТИВНОГО РАНЧ-СОКЕТА. ★ЭТО ПОЛЕ — ТОЛЬКО
    //! ДЛЯ ЛОГА: в отличие от raceLoadingGraceSince (R12-5) оно НИЧЕГО НЕ
    //! ограничивает. У ранч-отсрочки потолка нет и быть не должно — игрок
    //! законно стоит на ранчо часами, а роль ограничителя играет свежесть
    //! записи активности (30 c) на стороне директора ранчо. Метка нужна ровно
    //! затем, чтобы вход в отсрочку печатался info один раз, а продолжение —
    //! debug'ом, и чтобы в логе было видно длительность эпизода. Гаснет на
    //! реальном пульсе (R21-4c) и при переиспользовании контекста (R21-4d/4e).
    std::chrono::steady_clock::time_point ranchGraceSince{};

    std::string userName{};

    //! LOA-fix (R72-fix2-1, round72, backlog #129-S1, находка Codex 1):
    //! ★«ПРОСЬБА О ВХОДЕ УЖЕ СТОИТ В ПЛАНИРОВЩИКЕ И ЕЩЁ НЕ ИСПОЛНЕНА».
    //!
    //! `AcCmdCLLogin` — команда ПРЕД-ЛОГИННАЯ по построению, и каждый её пакет
    //! заводил СВОЮ задачу планировщика. Планировщик исполняет не больше одной
    //! задачи за тик, то есть флуд входами копил очередь задач без всякой
    //! верхней границы — а дедупликация, стоявшая ВНУТРИ задачи
    //! (`LobbyDirector::QueueClientLogin`), к этому моменту уже опоздала:
    //! память съедена, задачи заведены. Схлопывать надо на ВХОДЕ, до постановки
    //! в планировщик, — здесь.
    //!
    //! ★ФЛАГ ЖИВЁТ В КОНТЕКСТЕ КЛИЕНТА, А НЕ В ОТДЕЛЬНОМ МНОЖЕСТВЕ: контекст
    //! снимается при разрыве (`LockedContextEraser`), то есть слот
    //! освобождается тем же путём, каким освобождается всё остальное о
    //! клиенте, и отдельного «места, где не забыть убрать» не появляется.
    bool loginRequestScheduled{false};

    data::Uid characterUid = data::InvalidUid;
    data::Uid rancherVisitPreference = data::InvalidUid;
  };


  ClientId GetClientIdByUserName(
    const std::string& userName,
    bool requiresAuthorization = true);
  ClientId GetClientIdByCharacterUid(
    data::Uid characterUid,
    bool requiresAuthorization = true);

  // LOA-fix (R64-3, round64, backlog #215): КОНТРАКТ СМЕНЁН — НАРУЖУ УХОДИТ
  // КОПИЯ, А НЕ ССЫЛКА В КАРТУ.
  //
  // ★ПОЧЕМУ ЗАМКА ВНУТРИ ПРЕЖНЕГО МЕТОДА БЫЛО БЫ НЕДОСТАТОЧНО. Он возвращал
  // `ClientContext&` — замок отпустился бы на выходе, а ссылка ушла бы наружу и
  // пережила бы и rehash при вставке, и удаление записи. Защищать надо не
  // момент поиска, а всё время использования; единственный способ это
  // гарантировать, не растягивая замок на чужой код, — не выпускать ссылку.
  //
  // ★Копия дёшева и это проверено, а не предположено: три `bool`, три
  // `time_point`, короткий `userName` (обычно в SSO, без аллокации) и два uid.
  [[nodiscard]] ClientContext GetClientContext(
    ClientId clientId,
    bool requireAuthentication = true);

  // ★МУТАЦИЯ — ТОЛЬКО ЧЕРЕЗ ЭТО. Лямбда исполняется ПОД исключительным замком,
  // поэтому в ней допустимы ровно присваивания полей: любой вызов наружу из-под
  // замка — это заявка на дедлок лобби↔ранчо (и на самозахват нерекурсивного
  // `shared_mutex`, которым уже был убит вход в игру в R59: 0 успешных из 24).
  // Возвращает false, если клиента уже нет — вызывающий обязан это учитывать.
  bool MutateClientContext(
    ClientId clientId,
    const std::function<void(ClientContext&)>& mutator);

  // ★ТОТ ЖЕ МУТАТОР, НО С ПРОВЕРКОЙ ТОЖДЕСТВА. Нужен там, где решение принято
  // по СНИМКУ, вне замка: за это время клиент мог отключиться, а его id —
  // достаться новому подключению. Мутация применяется, только если запись всё
  // ещё та же (сверяется `characterUid`), иначе no-op.
  bool MutateClientContextIfSame(
    ClientId clientId,
    data::Uid expectedCharacterUid,
    const std::function<void(ClientContext&)>& mutator);

  void HandleNetworkTick() override;
  void HandleClientConnected(ClientId clientId) override;
  void HandleClientDisconnected(ClientId clientId) override;

  void HandleLogin(
    ClientId clientId,
    const protocol::AcCmdCLLogin& command);

  void SendLoginOK(
    ClientId clientId);

  void SendLoginCancel(
    ClientId clientId,
    protocol::AcCmdCLLoginCancel::Reason command);

  void HandleRoomList(
    ClientId clientId,
    const protocol::AcCmdCLRoomList& command);

  void HandleHeartbeat(
    ClientId clientId);

  void HandleMakeRoom(
    ClientId clientId,
    const protocol::AcCmdCLMakeRoom& command);

  void HandleEnterRoom(
    ClientId clientId,
    const protocol::AcCmdCLEnterRoom& command);

  void HandleLeaveRoom(
    ClientId clientId);

  void HandleEnterChannel(
    ClientId clientId,
    const protocol::AcCmdCLEnterChannel& command);

  void HandleLeaveChannel(
    ClientId clientId,
    const protocol::AcCmdCLLeaveChannel& command);

  void SendCreateNicknameNotify(
    ClientId clientId);

  void HandleCreateNickname(
    ClientId clientId,
    const protocol::AcCmdCLCreateNickname& command);

  void SendCreateNicknameCancel(
    ClientId clientId,
    protocol::AcCmdCLCreateNicknameCancel::Reason reason);

  void HandleShowInventory(
    ClientId clientId,
    const protocol::AcCmdCLShowInventory& command);

  void HandleUpdateUserSettings(
    ClientId clientId,
    const protocol::AcCmdCLUpdateUserSettings& command);

  void HandleEnterRoomQuick(
    ClientId clientId,
    const protocol::AcCmdCLEnterRoomQuick& command);

  void HandleGoodsShopList(
    ClientId clientId,
    const protocol::AcCmdCLGoodsShopList& command);

  void HandleAchievementCompleteList(
    ClientId clientId,
    const protocol::AcCmdCLAchievementCompleteList& command);

  void HandleRequestPersonalInfo(
    ClientId clientId,
    const protocol::AcCmdCLRequestPersonalInfo& command);

  void HandleEnterRanch(
    ClientId clientId,
    const protocol::AcCmdCLEnterRanch& command);

  void HandleEnterRanchRandomly(
    ClientId clientId,
    const protocol::AcCmdCLEnterRanchRandomly& command);

  void SendEnterRanchOK(
    ClientId clientId,
    data::Uid rancherUid);

  void HandleFeatureCommand(
    ClientId clientId,
    const protocol::AcCmdCLFeatureCommand& command);

  void HandleRequestFestivalResult(
    ClientId clientId,
    const protocol::AcCmdCLRequestFestivalResult& command);

  void HandleSetIntroduction(
    ClientId clientId,
    const protocol::AcCmdCLSetIntroduction& command);

  void HandleGetMessengerInfo(
    ClientId clientId,
    const protocol::AcCmdCLGetMessengerInfo& command);

  void HandleCheckWaitingSeqno(
    ClientId clientId,
    const protocol::AcCmdCLCheckWaitingSeqno& command);

  void SendWaitingSeqno(
    ClientId clientId,
    size_t queuePosition);

  void HandleUpdateSystemContent(
    ClientId clientId,
    const protocol::AcCmdCLUpdateSystemContent& command);

  void HandleEnterRoomQuickStop(
    ClientId clientId,
    const protocol::AcCmdCLEnterRoomQuickStop& command);

  void HandleRequestFestivalPrize(
    ClientId clientId,
    const protocol::AcCmdCLRequestFestivalPrize& command);

  void HandleQueryServerTime(
    ClientId clientId);

  void HandleRequestMountInfo(
    ClientId clientId,
    const protocol::AcCmdCLRequestMountInfo& command);

  void HandleInquiryTreecash(
    ClientId clientId,
    const protocol::AcCmdCLInquiryTreecash& command);

  void HandleAcceptInviteToGuild(
    ClientId clientId,
    const protocol::AcCmdLCInviteGuildJoinOK& command);

  void HandleDeclineInviteToGuild(
    ClientId clientId,
    const protocol::AcCmdLCInviteGuildJoinCancel& command);

  void HandleClientNotify(
    ClientId clientId,
    const protocol::AcCmdClientNotify& command);

  void HandleChangeRanchOption(
    ClientId clientId,
    const protocol::AcCmdCLChangeRanchOption& command);

  void HandleRequestDailyQuestList(
    ClientId clientId,
    const protocol::AcCmdCLRequestDailyQuestList& command);

  void HandleRequestLeagueInfo(
    ClientId clientId,
    const protocol::AcCmdCLRequestLeagueInfo& command);

  // todo: AcCmdCLMakeGuildParty, AcCmdCLGuildPartyList, AcCmdCLEnterGuildParty,
  //       AcCmdCLLeaveGuildParty, AcCmdCLStartGuildPartyMatch, AcCmdCLStopGuildPartyMatch

  void HandleRequestQuestList(
    ClientId clientId,
    const protocol::AcCmdCLRequestQuestList& command);

  // todo: AcCmdCLChangeGuildPartyOptions,

  void HandleRequestSpecialEventList(
    ClientId clientId,
    const protocol::AcCmdCLRequestSpecialEventList& command);

  //! A server instance.
  // LOA-fix (R64-3, round64, backlog #215): ВНУТРЕННИЙ путь к контексту —
  // только для тех, кто УЖЕ держит `_clientsMutex`. Имя обязано кричать об
  // этом: контракт «замок берёт вызывающий» нельзя оставлять в памяти автора,
  // иначе первый же невнимательный вызов даст самозахват нерекурсивного
  // мьютекса, а это тихий дедлок в проде, а не падение на ревью.
  ClientContext& GetClientContextLocked(
    ClientId clientId,
    bool requireAuthentication = true);

  //! LOA-fix (R72-1, round72, backlog #129-S1): НЕБРОСАЮЩИЙ вопрос «этот
  //! клиент залогинен?».
  //!
  //! Существует отдельно от `GetClientContext` потому, что ворота регистрации
  //! спрашивают его на КАЖДОМ входящем пакете, в том числе от сокета, который
  //! не логинился: бросок там означал бы строку [error] на пакет
  //! (`CommandServer.cpp`, блок catch вокруг вызова хендлера) — то есть новый
  //! флуд вместо закрытой дыры.
  //! @param clientId ID клиента.
  //! @returns true, если у клиента есть контекст и он аутентифицирован.
  [[nodiscard]] bool IsClientAuthenticated(ClientId clientId) const;

  //! LOA-fix (R72-1, round72, backlog #129-S1): ЕДИНСТВЕННАЯ ЗАПИСЬ ОБ ОТКАЗЕ.
  //! Дросселирована и несёт НАКОПИТЕЛЬНЫЙ счётчик, чтобы проглоченный отказ
  //! нельзя было потерять.
  //! @param clientId ID клиента, приславшего команду.
  //! @param command Команда, которой отказано.
  void NoteRefusedPreAuthCommand(
    ClientId clientId,
    protocol::Command command) noexcept;

  //! LOA-fix (R72-1, round72, backlog #129-S1): ★РЕГИСТРАЦИЯ ХЕНДЛЕРА,
  //! ТРЕБУЮЩЕГО ЛОГИНА.
  //!
  //! Решение об аутентификации принимается ЗДЕСЬ, ОДИН РАЗ НА КОМАНДУ, и
  //! хендлер физически не может его «забыть»: до тела он не доходит. Корень
  //! дефекта #129-S1 был не в одном забытом вызове, а в том, что решение
  //! принимал каждый хендлер сам — 13 из 36 не спрашивали контекст вовсе.
  //! Список мест обязательно отстаёт от кода; инвариант диспетчеризации — нет.
  //! @param handler Обработчик команды.
  //! ★★ВОРОТА СТОЯТ ДО РАЗБОРА ПАКЕТА (R72-fix-1, находка Codex 1).
  //! Первая редакция раунда проверяла аутентификацию в обёртке НАД хендлером,
  //! то есть уже после `C::Read`. Незалогиненный сокет мог прислать
  //! зарегистрированную команду с УКОРОЧЕННЫМ телом: разбор бросал раньше
  //! ворот, счётчик отказов не рос, дроссель не звался, а диспетчер писал
  //! [error] на каждый пакет — новый флуд вместо закрытой дыры, ровно того
  //! класса, который раунд убирает. Поэтому регистрация идёт через
  //! `RegisterGatedCommandHandler`: решение принимается по clientId, до
  //! десериализации, а отказанный кадр поглощается целиком.
  template <ReadableCommandStruct C>
  void RegisterAuthenticatedHandler(
    std::function<void(ClientId, const C&)> handler)
  {
    _commandServer.RegisterGatedCommandHandler<C>(
      [this](const ClientId clientId, const protocol::Command command)
      {
        if (IsClientAuthenticated(clientId))
          return true;

        NoteRefusedPreAuthCommand(clientId, command);
        return false;
      },
      std::move(handler));
  }

  //! LOA-fix (R72-1, round72, backlog #129-S1): ★РЕГИСТРАЦИЯ ХЕНДЛЕРА,
  //! ЗАКОННОГО ДО ЛОГИНА.
  //!
  //! Каждое использование обязано нести письменное обоснование рядом с
  //! вызовом. Их ТРИ, и весь список виден в одном месте — в конструкторе.
  //! @param handler Обработчик команды.
  template <ReadableCommandStruct C>
  void RegisterPreAuthHandler(
    std::function<void(ClientId, const C&)> handler)
  {
    _commandServer.RegisterCommandHandler<C>(std::move(handler));
  }

  ServerInstance& _serverInstance;
  //! A command server.
  CommandServer _commandServer;

  // LOA-fix (R64-3, round64, backlog #215): КАРТА ПОД ЗАМКОМ.
  //
  // ★ГОНКА ЗДЕСЬ НЕ ГИПОТЕЗА — её поймал детектор на стенде R61 (чтение под
  // чужой записью, стеки обоих потоков). Потоков два: сетевой поток лобби
  // (подключение, разрыв, тик, обработчики команд) и поток лобби-директора,
  // который лезет сюда через десять методов (`AcceptLogin`, `RejectLogin`,
  // `DisconnectCharacter`, `NotifyCharacter`, …) и как минимум два из них
  // ПИШУТ в поля.
  //
  // ★И удаление записи ДВУХПОТОЧНОЕ, хотя выглядит сетевым: директор зовёт
  // `DisconnectCharacter` → `CommandServer::DisconnectClient` → `Client::End()`,
  // а тот СИНХРОННО доходит до `HandleClientDisconnected` — то есть стирает
  // запись НА ПОТОКЕ ДИРЕКТОРА. Именно поэтому перебор карты в тике опасен:
  // `erase` инвалидирует итератор обхода.
  mutable std::shared_mutex _clientsMutex;
  //! A map of clients. ★Трогать ТОЛЬКО под `_clientsMutex` и только через
  //! методы доступа — прямых обращений в файле не должно оставаться.
  std::unordered_map<ClientId, ClientContext> _clients;

  // ★ПЕРЕИСПОЛЬЗУЕМЫЙ БУФЕР СНИМКА, а не локальный вектор на каждый тик.
  // Тик идёт 50 раз в секунду (`TicksPerSecond = 50`), и аллокация на каждом
  // была бы платой в горячем пути. `clear()` сохраняет ёмкость, поэтому после
  // первого тика аллокаций нет. Живёт только внутри тика сетевого потока.
  std::vector<std::pair<ClientId, ClientContext>> _tickSnapshot;

  //! LOA-fix (R21-4f, round21, backlog #95): момент последней периодической
  //! уборки реестра активности ранчо (RanchDirector::SweepRanchActivity).
  //! Уборка глобальная, поэтому метка живёт у хендлера, а не у клиента. Гейт по
  //! ВРЕМЕНИ, а не по числу тиков: сетевой тик перезаряжается таймером и на
  //! загруженном сервере дрейфует. Значение по умолчанию (эпоха steady_clock)
  //! означает «ещё ни разу» — первая уборка случится на первом же тике и
  //! пройдёт по пустому реестру.
  std::chrono::steady_clock::time_point _lastRanchActivitySweep{};

  //! LOA-fix (R72-1, round72, backlog #129-S1): дроссель строк об отказе.
  //! Окно 10 с выбрано так, чтобы сканер портов оставлял в логе след (одна
  //! строка + счётчики), а не мегабайты. Общий на весь лобби-сервер: бороться
  //! надо с ОБЪЁМОМ, а не обеспечивать каждой команде свою строку.
  //! ★Полный счёт отказов дроссель ведёт САМ и не обнуляет — именно по нему
  //! судится «честной сессии не отказано ни разу».
  util::LogThrottle _preAuthRefusalThrottle{std::chrono::seconds(10)};

  //! LOA-fix (R72-fix2-1, round72, backlog #129-S1, находка Codex 1): дроссель
  //! строк о СХЛОПНУТЫХ просьбах о входе.
  //!
  //! Отдельный от `_preAuthRefusalThrottle` намеренно: это разные события и
  //! разные счётчики. Отказ пред-логинной команде означает «сокет полез туда,
  //! куда ему нельзя»; схлопнутый вход — «сокет торопит уже принятую просьбу».
  //! Слить их в один дроссель значило бы, что одно событие глушит строку о
  //! другом, а накопительные счета, по которым судит оракул раунда, перестают
  //! отвечать каждый за своё.
  util::LogThrottle _loginRequestCoalesceThrottle{std::chrono::seconds(10)};

  //! LOA-fix (R72-fix2-4, round72, backlog #170-item-28, находка Codex 4):
  //! дроссель строк о представлениях, не влезающих в исходящий кадр. Тоже
  //! отдельный: событие порождает клиент, счёт по нему судится сам по себе, и
  //! глушить им строки об отказах авторизации было бы смешиванием улик.
  util::LogThrottle _oversizedIntroductionThrottle{std::chrono::seconds(10)};
};

} // namespace server

#endif // ALICIA_SERVER_LOBBYNETWORKHANDLER_HPP
