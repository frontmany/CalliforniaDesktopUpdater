# deps

Клонируйте зависимости в эту папку **локально** после clone (как
`CalliforniaDesktop/deps`). В git лежат только `.gitkeep` и этот README.

Обе — header/source-only, достаточно клонировать, сборка не нужна. Версии
**запинены** (совпадают с CI-workflow `CalliforniaDesktop/.github/workflows/release.yml`):

```bash
git clone https://github.com/orlp/ed25519.git deps/ed25519
git -C deps/ed25519 checkout b1f19fab4aeb   # тегов нет, пин на коммит master
git clone --depth 1 --branch v3.11.3 https://github.com/nlohmann/json.git deps/json
```

## Структура

```
deps/
├── ed25519/    # deps/ed25519/src/*.{c,h} — CMake компилирует только
│               # fe.c/ge.c/sc.c/sha512.c/verify.c (только проверка подписи,
│               # апдейтер никогда не подписывает — см. CMakeLists.txt)
└── json/       # header-only — deps/json/include/nlohmann/json.hpp
```
