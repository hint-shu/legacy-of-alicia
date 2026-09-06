// f6 — ФИКСИРОВАННЫЙ std::array. Находок быть НЕ должно: у такого цикла нет
// счётчика на проводе и нет роста от клиента. Пропуск делается ПО ТИПУ,
// разрешённому из объявления, а не по имени члена.
#include "fixtures.hpp"

void FixtureSix::Write(const FixtureCommand& command, SinkStream& stream)
{
  for (const auto& item : command.fixed)
  {
    stream.Write(item);
  }
}
