// f5 — P2 (булк): непрерывный блок байтов уезжает мимо WriteBoundedBytes.
#include "fixtures.hpp"

void FixtureFive::Write(const FixtureCommand& command, SinkStream& stream)
{
  stream.Write(command.blob.data(), command.blob.size());
}
