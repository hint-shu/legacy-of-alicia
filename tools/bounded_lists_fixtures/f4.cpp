// f4 — P2: тело динамического контейнера гоняется циклом мимо хелпера.
#include "fixtures.hpp"

void FixtureFour::Write(const FixtureCommand& command, SinkStream& stream)
{
  for (const auto& item : command.items)
  {
    stream.Write(item);
  }
}
