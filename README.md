# CallsUpdaterApp

Апдейтер десктоп-клиента Calls: маленький нативный exe, который применяет
скачанное клиентом обновление — дожидается выхода приложения, атомарно
подменяет файлы и перезапускает клиент.

Ключевые свойства:

- **Ноль зависимостей на рантайме**: без Qt и OpenSSL, статический CRT
  (`/MT`) — апдейтер подменяет DLL рядом с собой и не должен зависеть от них.
  SHA-256 — Windows CNG (bcrypt), Ed25519 — вендоренный orlp/ed25519
  (только verify), JSON — вендоренный nlohmann single-header.
- **Подписанные манифесты**: перед любыми действиями проверяется Ed25519-подпись
  `manifest.json` вшитым публичным ключом (`src/public_key.hpp`, генерируется
  скриптом `CallsApp/scripts/generate-update-keys.ps1`).
- **Атомарный swap с журналом**: staged-файлы копируются в
  `<install>\.update\incoming\` (после этого swap — только same-volume rename,
  атомарно на NTFS); журнал `.update\journal.json` + RunOnce-страховка
  позволяют докатить обновление после сбоя/перезагрузки (`repair`).
- **Откат**: если swap упал (залоченный файл и т.п.) — восстановление из
  `.update\backup\` и перезапуск старой версии.
- WIN32-subsystem (никаких мелькающих консолей); лог в
  `%LOCALAPPDATA%\Calls\logs\updater.log`; результат — в
  `%LOCALAPPDATA%\Calls\updates\last-apply.json`.

## CLI

```
updater.exe apply  --install-dir <dir> --staging-dir <dir> --parent-pid <pid>
                   --relaunch-exe Client.exe [--no-relaunch] [--log <file>]
updater.exe repair --install-dir <dir> [--relaunch-exe Client.exe] [--log <file>]
```

Клиент перед запуском копирует `updater.exe` из install-папки в
`%LOCALAPPDATA%\Calls\updates\bin\` (нельзя запускать из папки, которую
предстоит обновлять) и штатно завершается; апдейтер ждёт выхода по PID и
эксклюзивной пробе `Client.exe`.

Exit-коды: `0` применено; `2` аргументы/уже работает другой апдейтер; `3`
приложение не вышло; `4` подпись/хэш не сошлись; `5` нет прав записи; `6` swap
упал, откат успешен; `7` swap и откат упали (журнал оставлен — поможет
`repair`); `8` repair: делать нечего.

Пока апдейтер работает, он держит named mutex `Local\CallsUpdaterApplying` —
клиент проверяет его при старте и молча выходит, если идёт обновление.

## Сборка и тесты

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
build\Release\updater_tests.exe
powershell -File scripts\integration-test.ps1
```

Интеграционному тесту нужен dev-ключ подписи
(`CallsBackend/Updates/keys/manifest-private.pem`), соответствующий вшитому
публичному ключу.

## Связанные части системы

- **CallsApp** — качает обновления (per-file diff по манифесту) в
  `%LOCALAPPDATA%\Calls\updates\staging\<version>\` и запускает апдейтер.
- **CallsBackend/Updates** — сервис, отдающий манифесты; блобы файлов клиент
  качает с `/v1/updates/files/<sha256>` (MinIO через Nginx).
- **CallsApp/scripts/publish-release.ps1** — публикация релиза (подпись
  манифеста, залив блобов); `updater.exe` входит в состав каждого релиза.
