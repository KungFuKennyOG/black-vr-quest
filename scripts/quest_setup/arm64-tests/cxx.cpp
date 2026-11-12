#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
int main() {
  std::map<std::string, std::vector<int>> m;
  for (int i = 0; i < 1000; i++) m[std::to_string(i % 37)].push_back(i);
  auto p = std::make_unique<int[]>(1 << 16);
  int caught = 0;
  try { throw std::runtime_error("x"); } catch (const std::exception &e) { caught = e.what()[0] == 'x'; }
  std::printf("cxx buckets=%zu caught=%d\n", m.size(), caught);
  return (m.size() == 37 && caught) ? 0 : 1;
}
