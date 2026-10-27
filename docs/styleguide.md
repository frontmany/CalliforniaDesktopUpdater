# Стиль кода

Правила клиента CalliforniaDesktop (`docs/styleguide.md` там), без частей про
QML и Qt: апдейтер это отдельный нативный инструмент без Qt. Новый и тронутый
код следует им, старый ещё бывает смешанным.

## Имена

| Что | Стиль | Пример |
|---|---|---|
| Типы (`class`, `struct`, `enum`, псевдонимы) | `PascalCase` | `Manifest`, `FileOp::Kind::Replace` |
| Функции | `camelCase` | `diffManifests()`, `runApply()` |
| Нестатические поля класса | `m` + `PascalCase` | `mHandle`, `mCancelled` |
| Поля открытых структур (DTO) | `camelCase` | `installDir`, `parentPid` |
| Параметры и локальные | `camelCase` | `installDir`, `expectedSha256` |
| Константы, `constexpr` | `k` + `PascalCase` | `kAttempts`, `kApplyMutexName` |
| Пространство имён | строчные | `updater` |
| Файлы | `snake_case` | `sha256.cpp` |
| Флаги командной строки | `--kebab-case` | `--install-dir`, `--parent-pid` |

- Геттеры `foo()`, `isFoo()`, `hasFoo()`, `canFoo()`, без `getFoo`.
- Без `m_foo` и `foo_`.
- Ключи JSON манифеста остаются `snake_case` (`min_supported_version`), поля
  разобранной структуры `camelCase` (`minSupportedVersion`).
- Вендоренный код в `deps/` сохраняет свой стиль.

## Владение

- Владение через `std::unique_ptr`; не владеющее это сырой указатель или
  ссылка.
- Хэндлы Windows (`HANDLE`, `BCRYPT_HASH_HANDLE`) оборачиваются в маленький
  RAII-тип со своим удалителем (`Hasher` в `sha256.cpp`, `ApplyMutex` в
  `apply.cpp`), а не закрываются руками в каждом месте.

## Ошибки

- Исправимые сбои (файловая система, проверка, сеть) возвращаются как
  `std::expected<T, std::string>`, а не исключениями: это маленький
  инструмент, сбой которого пишется в лог и становится кодом выхода.
- Исключения допустимы на границах разбора (`nlohmann::json` на кривом
  входе) и ловятся рядом с этой границей.

## Прочее

- `[[nodiscard]]` там, где результат нельзя терять.
- Без `using namespace std` в заголовках.
- Порядок include: свой заголовок, проект, сторонние, std.
- Комментарии по-английски и о том, почему, а не что.
- Никаких зависимостей от Qt, DLL OpenSSL и любых других DLL, которые
  апдейтеру могут поручить заменить: `/MT`, CNG для SHA-256, вендоренный
  Ed25519.
