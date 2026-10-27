# CalliforniaDesktopUpdater

Апдейтер десктоп-клиента Callifornia для Windows: маленький exe без
зависимостей, который применяет скачанное клиентом обновление. Ждёт выхода
приложения, проверяет подпись манифеста, атомарно подменяет файлы с журналом
и откатом и перезапускает клиент.

Отдельно не выпускается: `updater.exe` входит в каждый релиз
[CalliforniaDesktop](https://github.com/frontmany/CalliforniaDesktop) и
собирается его `release.yml`. На Linux и Mac апдейтера нет: там обновляет
пакетный менеджер или человек.

## Сборка и тесты

Нужны Visual Studio 2022+ (MSVC, C++23), CMake 3.24+ и две зависимости в
`deps/` по [`deps/README.md`](deps/README.md).

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
build\Release\updater_tests.exe
powershell -File scripts\integration-test.ps1
```

Интеграционному тесту нужен приватный ключ манифестов, парный ключу в
`src/public_key.hpp`. Он один и лежит в сейфе
(`C:\projects\.callifornia-secrets\updates-manifest-private.pem`, путь меняет
`-PrivateKeyFile`).

## Запуск

Руками апдейтер не запускают: его запускает клиент. Для отладки:

```
updater.exe apply  --install-dir <dir> --staging-dir <dir> --parent-pid <pid>
                   --relaunch-exe Callifornia.exe [--no-relaunch] [--log <file>]
updater.exe repair --install-dir <dir> [--relaunch-exe Callifornia.exe] [--log <file>]
```

Лог: `%LOCALAPPDATA%\Callifornia\logs\updater.log`, итог:
`%LOCALAPPDATA%\Callifornia\updates\last-apply.json`.

## Документация

- [docs/how-it-works.md](docs/how-it-works.md): как применяется обновление,
  коды выхода, журнал, откат, сторожевой таймер, связь с клиентом.
- [docs/styleguide.md](docs/styleguide.md): стиль кода.
- Выпуск и система обновлений целиком:
  [CalliforniaDesktop, docs/releasing.md](https://github.com/frontmany/CalliforniaDesktop/blob/main/docs/releasing.md).
