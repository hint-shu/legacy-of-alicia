// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// ★ГЛАВНОЕ СВОЙСТВО ГАРДА: неизвестное имя типа не проезжает как «наверное,
// ок». Ожидание — код 2 (проверка недействительна), а не 0.
#ifndef FIXTURE_UNKNOWN_TYPE_STOP_HPP
#define FIXTURE_UNKNOWN_TYPE_STOP_HPP

namespace fixture
{

struct UnknownTypeStop
{
  some::Unknown u;
};

} // namespace fixture

#endif
