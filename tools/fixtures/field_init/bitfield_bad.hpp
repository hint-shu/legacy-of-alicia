// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// R72-fix-5 (находка Codex 5). Ожидание: ровно один нарушитель — битовое поле
// `flags` на строке 18. Первая редакция гарда пропускала битовые поля явной
// строкой `continue`, то есть считала их нулём полей: скаляр без значения
// уходил из-под проверки ровно тем способом, ради которого она заведена.
#ifndef FIXTURE_BITFIELD_BAD_HPP
#define FIXTURE_BITFIELD_BAD_HPP

#include <cstdint>

namespace fixture
{

struct BitfieldBad
{
  //! Инициализированное битовое поле — не нарушитель.
  uint32_t ok : 1 {0};
  uint32_t flags : 3;
};

} // namespace fixture

#endif
