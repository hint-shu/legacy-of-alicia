// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// Ожидание: НОЛЬ нарушителей. Все законные формы инициализатора, какие
// встречаются в реестрах.
#ifndef FIXTURE_INITIALISED_OK_HPP
#define FIXTURE_INITIALISED_OK_HPP

#include <cstdint>

namespace fixture
{

enum class Region : uint8_t
{
  Unknown = 0,
};

struct InitialisedOk
{
  uint32_t braces{};
  uint32_t bracesValue{0};
  uint32_t assigned = 0;
  Region region{Region::Unknown};
  uint32_t* pointer{nullptr};
  bool flag{false};
};

} // namespace fixture

#endif
