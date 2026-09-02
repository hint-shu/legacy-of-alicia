// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// R72-fix-5 (находка Codex 5). ★ТРЕТИЙ ИСХОД ДЛЯ СКОБОК: форма, которую разбор
// не умеет отнести ни к методу, ни к полю, обязана ОСТАНАВЛИВАТЬ гард (код 2),
// а не проезжать молча. Объявление с завершающим типом возврата — именно такая
// форма: заканчивается оно не списком параметров, а типом.
// Ожидание: код 2.
#ifndef FIXTURE_TRAILING_RETURN_STOP_HPP
#define FIXTURE_TRAILING_RETURN_STOP_HPP

#include <cstdint>

namespace fixture
{

struct TrailingReturnStop
{
  auto GetValue() -> uint32_t;
};

} // namespace fixture

#endif
