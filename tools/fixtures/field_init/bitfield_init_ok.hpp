// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// R72-fix-5 (находка Codex 5). Ожидание: НОЛЬ нарушителей. Контроль ложной
// тревоги к соседней фикстуре bitfield_bad.hpp: битовое поле со значением
// (в обеих формах — `{}` и `=`) нарушителем быть не должно, иначе «гард видит
// битовые поля» означало бы «гард ругается на любое битовое поле».
#ifndef FIXTURE_BITFIELD_INIT_OK_HPP
#define FIXTURE_BITFIELD_INIT_OK_HPP

#include <cstdint>

namespace fixture
{

struct BitfieldInitOk
{
  uint32_t braced : 3 {0};
  uint32_t assigned : 5 = 0;
  //! Псевдоним ширины — тоже допустимая форма.
  uint32_t sized : sizeof(uint32_t) {1};
};

} // namespace fixture

#endif
