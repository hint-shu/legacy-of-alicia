// f3 — P3: кламп ПОСЛЕ сужающего каста. При size()==256 такой `min` даёт 0.
#include "fixtures.hpp"

void FixtureThree::Write(const FixtureCommand& command, SinkStream& stream)
{
  const uint8_t count = std::min(static_cast<uint8_t>(command.count), uint8_t{10});
  stream.Write(count);
}
