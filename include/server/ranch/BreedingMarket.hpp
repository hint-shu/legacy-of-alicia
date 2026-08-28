//
// Created for Alicia Server
//

#ifndef BREEDING_MARKET_HPP
#define BREEDING_MARKET_HPP

#include <libserver/data/DataDefinitions.hpp>
#include <libserver/util/Scheduler.hpp>
#include <libserver/util/Util.hpp>

#include <vector>
#include <unordered_map>
#include <optional>
#include <shared_mutex>
#include <set>

namespace server
{

// Forward declarations
class ServerInstance;

//! Manages the breeding market system where players can register stallions for breeding
class BreedingMarket
{
public:
  //! Breeding earnings information
  struct Earnings
  {
    //! A count of times the horse was mated.
    uint32_t timesMated{};
    //! The total revenue accrued by the stallion.
    uint32_t revenue{};
    //! A cost of a breed.
    uint32_t breedingFee{};
    //! Final earnings for the character, after fees.
    uint32_t earnings{};
    //! Current tax rate.
    float taxRate{};
    //! The generated UID of this earning's claim.
    data::Uid claimUid{};
  };

  //! Breeding fee range of a grade.
  struct GradeFeeRange
  {
    int32_t min{};
    int32_t max{};
  };

  //! Data of a registered stallion needed to perform a breeding.
  struct StallionData
  {
    //! UID of the stallion data record (not the horse).
    data::Uid stallionUid{data::InvalidUid};
    //! Price in carrots to breed with this stallion.
    uint32_t breedingCharge{0};
  };

  enum class SnapshotOrder
  {
    LineageDescending,
    TimeLeftDescending,
    FeeDescending,
    PregnancyChanceAscending,
    PregnancyChanceDescending,
    FeeAscending,
    TimeLeftAscending,
    LineageAscending
  };

  struct SnapshotFilter
  {
    enum class Stat
    {
      None,
      Agility,
      //! Also known as spirit.
      Ambition,
      //! Also known as speed.
      Rush,
      //! Also known as strength.
      Endurance,
      //! Also known as control.
      Courage,
    };

    uint8_t grade;
    std::set<data::Tid> coats;
    std::set<data::Tid> manes;
    std::set<data::Tid> tails;
    Stat firstPreferredStat{Stat::None};
    Stat secondPreferred{Stat::None};
  };

  struct Snapshot
  {
    struct Registration
    {
      data::Uid horseUid{data::InvalidUid};
      data::Uid stallionUid{data::InvalidUid};
    };

    std::vector<Registration> registrations{};
  };

  //! Constructor
  //! @param serverInstance Reference to the server instance
  explicit BreedingMarket(ServerInstance& serverInstance);
  //! Destructor
  ~BreedingMarket() = default;

  //! Initializes the breeding market.
  void Initialize();
  //! Terminates the breeding market.
  void Terminate();
  //! Ticks the breeding market.
  void Tick();

  [[nodiscard]] bool HandleRegisterStallion(
    data::Uid characterUid,
    data::Uid horseUid,
    int32_t breedingFee) noexcept;

  [[nodiscard]] bool HandleUnregisterStallion(
    data::Uid characterUid,
    data::Uid horseUid) noexcept;

  //! Calculates earnings for unregistering a stallion
  //! @param horseUid UID of the horse
  //! @returns Breeding earnings if registered, `std::nullopt` if not registered.
  [[nodiscard]] std::optional<Earnings> CalculateUnregisterEarnings(
    data::Uid horseUid) const noexcept;

  //! Gets the stallion data for a registered horse.
  //! @param horseUid UID of the horse registered as a stallion.
  //! @returns Stallion data if registered, `std::nullopt` otherwise.
  [[nodiscard]] std::optional<StallionData> GetStallionData(
    data::Uid horseUid) const noexcept;

  //! Checks if a horse is registered as a stallion
  //! @param horseUid UID of the horse to check
  //! @returns true if registered, false otherwise
  [[nodiscard]] bool IsRegistered(data::Uid horseUid) const noexcept;

  //! Calculates registration fee for registering a stallion.
  //! @param breedingFee Breeding fee of a stallion.
  //! @return Registration fee value.
  [[nodiscard]] int32_t CalculateRegistrationFee(int32_t breedingFee) const noexcept;

  //! Gets the breeding fee range for a grade.
  //! @param grade Grade.
  //! @return Breeding fee range of a grade.
  //!         If grade is not allowed to breed `std::nullopt` is returned.
  [[nodiscard]] std::optional<GradeFeeRange> GetGradeFeeRange(
    uint32_t grade) const noexcept;

  //! Gets all registered stallion horse UIDs
  //! @returns Vector of horse UIDs
  [[nodiscard]] Snapshot CollectMarketSnapshot(
    SnapshotOrder order,
    SnapshotFilter) const noexcept;

private:
  struct Registration
  {
    data::Uid stallionUid{data::InvalidUid};
  };

  //! Что стало с выплатой владельцу.
  //!
  //! ★Три состояния, а не два, ровно по той же причине, что и у изменения
  //! записи: `RewardSystem::CreateReward` умеет вернуть «не создал» ЧЕСТНО
  //! (пустой uid), а умеет и бросить УЖЕ ПОСЛЕ того, как заявка заполнена.
  //! Во втором случае мы не знаем, есть заявка или нет, и обязаны выбрать
  //! сторону: НЕ возвращать долг, иначе повтор снятия выдаст вторую заявку.
  enum class PayoutResult
  {
    //! Платить было нечего либо заявка создана.
    Paid,
    //! Заявки ТОЧНО нет — долг можно вернуть в запись и повторить позже.
    NotCreated,
    //! Заявка может существовать. Долг НЕ возвращаем: дубль хуже потери.
    Unknown,
  };

  //! Занимает слот рынка под лошадь ДО всякой опасной работы.
  //!
  //! ★Слот занимается ЗАГЛУШКОЙ (`stallionUid = InvalidUid`) под
  //! exclusive-замком. Это делает финальную публикацию небросающей (узел карты
  //! уже существует, вписать в него значение памяти не требует) и заодно
  //! закрывает гонку двух одновременных регистраций одной лошади.
  [[nodiscard]] bool ClaimRegistrationSlot(
    data::Uid characterUid,
    data::Uid horseUid) noexcept;
  //! Освобождает занятый слот.
  void ReleaseRegistrationSlot(data::Uid horseUid) noexcept;
  //! Делает всю бросающую работу регистрации, КРОМЕ денег.
  //! @returns UID созданной записи жеребца; `InvalidUid` — не удалось.
  [[nodiscard]] data::Uid PrepareRegistration(
    data::Uid characterUid,
    data::Uid horseUid,
    int32_t breedingFee) const noexcept;
  //! Снимает сбор и публикует регистрацию ПОД ОДНИМ ЗАМКОМ.
  //!
  //! ★Оба действия вместе именно потому, что порознь между ними возникает
  //! окно «деньги сняты, регистрации нет», и закрывать его пришлось бы
  //! возвратом — то есть компенсацией, которая сама умеет не сработать.
  //! Под общим замком публикация не может сорваться отдельно от оплаты:
  //! узел карты уже создан заглушкой, вписать в него значение нельзя не смочь.
  [[nodiscard]] bool CommitRegistration(
    data::Uid characterUid,
    data::Uid horseUid,
    data::Uid stallionUid,
    int32_t breedingFee) noexcept;
  //! Возвращает лошадь в обычное состояние и удаляет запись жеребца.
  void TakeStallionOffTheMarket(
    data::Uid horseUid,
    data::Uid stallionUid) const noexcept;
  //! Создаёт заявку на выплату владельцу.
  //! ★Вызывается ВНУТРИ того же изменения записи, которое гасит долг, — см.
  //! `UnregisterStallion`. Письмо отправляется отдельно и уже вне изменения:
  //! почта ходит к сессиям игроков, и держать под ней замок записи незачем.
  [[nodiscard]] PayoutResult CreateBreedingPayout(
    data::Uid ownerUid,
    data::Uid horseUid,
    data::Uid stallionUid,
    Earnings& earnings) const noexcept;
  //! Отправляет владельцу письмо о выплате.
  void SendBreedingPayoutMail(
    data::Uid ownerUid,
    data::Uid horseUid,
    data::Uid stallionUid,
    const Earnings& earnings) const noexcept;
  //! Снимает жеребца с рынка: гасит долг, возвращает тип, удаляет запись и
  //! платит владельцу.
  //! @returns `true`, если вызывающий обязан стереть регистрацию из карты.
  [[nodiscard]] bool UnregisterStallion(
    data::Uid horseUid,
    data::Uid stallionUid) const noexcept;
  void ScheduleExpirationCheck() noexcept;
  void RunExpirationCheck() noexcept;
  //! Сбор витрины без пояса; вызывается только из `CollectMarketSnapshot`.
  //! ★Фильтр по ССЫЛКЕ: копия у публичной обёртки уже есть, а вторая копия
  //! трёх множеств на КАЖДОМ успешном показе витрины была бы платой успешного
  //! пути за разделение функции надвое.
  [[nodiscard]] Snapshot CollectMarketSnapshotUnsafe(
    SnapshotOrder order,
    const SnapshotFilter& filter) const;
  //! ★Вызывается ТОЛЬКО под exclusive-замком `_mutex`: читает карту рынка.
  [[nodiscard]] bool CanRegisterStallion(data::Uid characterUid) const noexcept;

  //! Reference to the server instance
  ServerInstance& _serverInstance;

  //! Job scheduler.
  Scheduler _scheduler;

  //! Mutex for thread-safe access to breeding market data
  mutable std::shared_mutex _mutex;

  //! Map of horses which are registered as stallions.
  std::unordered_map<data::Uid, Registration> _horseRegistrations;

  //! Стоит ли проверка истечения в планировщике.
  //! ★Без этого признака одна сорвавшаяся постановка означала бы, что рынок
  //! перестал истекать НАВСЕГДА: жеребцы висели бы в списке вечно, бесплатно.
  //! `Tick` пробует поставить снова.
  bool _expirationCheckScheduled{false};
};

} // namespace server

#endif // BREEDING_MARKET_HPP

