// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// Ожидание: НОЛЬ нарушителей. Объявление метода, using, friend и
// static_assert — не поля, и гард не имеет права считать их полями.
#ifndef FIXTURE_METHODS_OK_HPP
#define FIXTURE_METHODS_OK_HPP

#include <cstdint>

namespace fixture
{

struct MethodsOk
{
  using Id = uint32_t;

  uint32_t GetFoo();
  uint32_t GetBar() const;
  void SetFoo(uint32_t value);

  friend struct Other;
  static_assert(sizeof(uint32_t) == 4);

  Id id{0};
};

} // namespace fixture

#endif
