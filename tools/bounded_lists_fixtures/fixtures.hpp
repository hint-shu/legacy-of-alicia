// Фикстуры гейта ограниченных списков (R74). НЕ КОМПИЛИРУЮТСЯ и не входят ни в
// одну цель сборки: это вход для tools/check_bounded_lists.py, на котором гейт
// обязан доказать, что умеет краснеть на КАЖДОМ своём предикате, ДО того как
// вынесет вердикт дереву.
#ifndef BOUNDED_LIST_FIXTURES_HPP
#define BOUNDED_LIST_FIXTURES_HPP

struct FixtureItem
{
  uint32_t uid{};
};

struct FixtureCommand
{
  std::vector<FixtureItem> items{};
  std::vector<std::byte> blob{};
  std::array<FixtureItem, 4> fixed{};
  uint32_t count{};
};

struct FixtureAvatar
{
  std::vector<FixtureItem> equipment{};
};

#endif
