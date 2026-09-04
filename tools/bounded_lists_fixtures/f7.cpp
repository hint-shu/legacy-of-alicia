// f7 — P2 через ЛОКАЛЬНУЮ `const auto&`. Корень выражения объявлен не
// параметром, а ссылкой выше по телу (`const auto& skillRanks = command.skillRanks;`
// в LoginOK); резолвер обязан пройти по объявлению, а не сдаться.
#include "fixtures.hpp"

void FixtureSeven::Write(const FixtureCommand& command, SinkStream& stream)
{
  const auto& list = command.items;
  for (const auto& item : list)
  {
    stream.Write(item);
  }
}
