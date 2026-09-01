// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// Ожидание: НОЛЬ нарушителей. Типы класса сами себя конструируют, поэтому
// отсутствие инициализатора у них — не дефект.
#ifndef FIXTURE_CLASS_MEMBERS_OK_HPP
#define FIXTURE_CLASS_MEMBERS_OK_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fixture
{

struct Element
{
  uint32_t value{0};
};

struct ClassMembersOk
{
  std::string name;
  std::vector<Element> elements;
  std::unordered_map<uint32_t, Element> byId;
  Element element;
};

} // namespace fixture

#endif
