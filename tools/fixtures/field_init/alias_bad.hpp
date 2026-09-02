// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// ★КЛАСС ДЕФЕКТА #174: тип поля спрятан за ПСЕВДОНИМОМ. Пересчёт, ключащийся
// на имена встроенных типов, такое поле не видит; гард обязан пройти цепочку
// псевдонимов и назвать поле нарушителем.
#ifndef FIXTURE_ALIAS_BAD_HPP
#define FIXTURE_ALIAS_BAD_HPP

#include <cstdint>

namespace fixture
{

using MyRawId = uint32_t;
using MyId = MyRawId;

struct AliasBad
{
  MyId id;
};

} // namespace fixture

#endif
